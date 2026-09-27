//
// jpegturbo_plugin.cpp - wiv image plugin for JPEG, via libjpeg-turbo's
// classic libjpeg API (jpeglib.h) rather than the vendored stb_image (see
// ../stbimage/stbimage_plugin.cpp, which still covers PNG/BMP/TGA/GIF/PSD).
//
// Why a separate plugin just for JPEG: with $image cycling now decoding
// on demand (one full-resolution decode per next/previousImage() step -
// see ImagePluginHost::isSupported()/ImageCache in ../../ImagePluginHost.h),
// decode speed directly gates how fast paging through a folder of camera
// JPEGs feels. libjpeg-turbo's decoder is SIMD-accelerated (hand-written
// x86/ARM intrinsics for IDCT/upsampling/color conversion); stb_image's is
// plain scalar C. On typical multi-megapixel camera JPEGs that's several
// times faster - directly attacking the other half (decode time, next to
// the pipeline-recreation fix in SceneBuilder::build()) of "why isn't
// next/previousImage as fast as giv".
//
// ImagePluginHost loads plugins in sorted-filename order and dispatches to
// the first one whose wiv_plugin_supports_file() claims the file
// (first-match-wins) - "jpegturbo" sorts before "stbimage", so this plugin
// is always consulted first for a .jpg/.jpeg and stb_image's own (still
// listed) JPEG support only ever acts as a fallback if this plugin were
// ever removed from the plugin directory.
//
#include "../wiv_plugin_common.h"

#include <jerror.h>
#include <jpeglib.h>

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

// Reads the EXIF Orientation tag (0x0112) out of the JPEG's APP1 marker, if
// any. Requires jpeg_save_markers(cinfo, JPEG_APP0+1, 0xffff) to have been
// called before jpeg_read_header() so libjpeg retains the raw marker bytes
// in cinfo.marker_list instead of just skipping over them. Returns 1
// ("normal", no transform) if there's no EXIF data or no orientation tag.
int readExifOrientation(jpeg_decompress_struct& cinfo)
{
    for (jpeg_saved_marker_ptr marker = cinfo.marker_list; marker; marker = marker->next)
    {
        if (marker->marker != JPEG_APP0 + 1 || marker->data_length < 14)
            continue;
        if (std::memcmp(marker->data, "Exif\0\0", 6) != 0)
            continue;

        const unsigned char* tiff = marker->data + 6;
        const size_t tiffLen = marker->data_length - 6;

        bool bigEndian;
        if (tiff[0] == 'I' && tiff[1] == 'I')
            bigEndian = false;
        else if (tiff[0] == 'M' && tiff[1] == 'M')
            bigEndian = true;
        else
            continue;

        auto read16 = [&](const unsigned char* p) -> uint16_t {
            return bigEndian ? (static_cast<uint16_t>(p[0]) << 8) | p[1] : (static_cast<uint16_t>(p[1]) << 8) | p[0];
        };
        auto read32 = [&](const unsigned char* p) -> uint32_t {
            return bigEndian ? (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                                    (static_cast<uint32_t>(p[2]) << 8) | p[3]
                              : (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                                    (static_cast<uint32_t>(p[1]) << 8) | p[0];
        };

        const uint32_t ifdOffset = read32(tiff + 4);
        if (static_cast<size_t>(ifdOffset) + 2 > tiffLen)
            continue;

        const uint16_t numEntries = read16(tiff + ifdOffset);
        const size_t entriesStart = static_cast<size_t>(ifdOffset) + 2;
        for (uint16_t i = 0; i < numEntries; ++i)
        {
            const size_t entryOffset = entriesStart + static_cast<size_t>(i) * 12;
            if (entryOffset + 12 > tiffLen)
                break;
            const unsigned char* entry = tiff + entryOffset;
            if (read16(entry) != 0x0112 /* Orientation */ || read16(entry + 2) != 3 /* SHORT */)
                continue;
            const uint16_t value = read16(entry + 8);
            if (value >= 1 && value <= 8)
                return value;
        }
    }
    return 1;
}

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    return std::equal(filename.end() - n, filename.end(), ext,
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

// libjpeg's default error handler prints to stderr and calls exit() - fatal
// for a viewer that should just skip an unreadable/corrupt file (see
// VulkanViewport::decodeCurrentImage()'s scan-forward-on-failure). This
// longjmp's back out to wiv_plugin_load_image instead.
struct JpegErrorContext
{
    jpeg_error_mgr pub;
    jmp_buf jumpBuffer;
};

void onFatalError(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<JpegErrorContext*>(cinfo->err);
    longjmp(err->jumpBuffer, 1);
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".jpg") || hasExtension(f, ".jpeg");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    FILE* file = std::fopen(filename, "rb");
    if (!file)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("jpegturbo: failed to open %s", filename);
        return nullptr;
    }

    jpeg_decompress_struct cinfo;
    JpegErrorContext jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = onFatalError;

    if (setjmp(jerr.jumpBuffer))
    {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(file);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("jpegturbo: failed to decode %s", filename);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file);
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xffff); // retain APP1 (EXIF) for orientation
    jpeg_read_header(&cinfo, TRUE);
    const int orientation = readExifOrientation(cinfo);

    // libjpeg-turbo colorspace extension: decode straight to interleaved
    // RGBA (alpha filled with 0xff), same layout WivPluginImage expects -
    // skips the manual RGB->RGBA expansion stb_image's caller would need.
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);

    WivPluginImage* img = wiv_plugin::allocImage(static_cast<int>(cinfo.output_width), static_cast<int>(cinfo.output_height));
    img->orientation = orientation;
    const int rowStride = static_cast<int>(cinfo.output_width) * 4;
    while (cinfo.output_scanline < cinfo.output_height)
    {
        unsigned char* rowPtr = img->rgba + static_cast<size_t>(cinfo.output_scanline) * rowStride;
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(file);
    return img;
}
