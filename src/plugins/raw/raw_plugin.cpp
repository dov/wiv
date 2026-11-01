//
// raw_plugin.cpp - wiv image plugin for camera RAW files (CR2/CR3, NEF,
// ARW, DNG, ORF, RW2, RAF, PEF, ...), via LibRaw. Files are demosaiced with
// the camera white balance into 8-bit sRGB; LibRaw applies the file's
// rotation itself, so `orientation` stays 1. The decode is cancellable (the
// progress callback aborts it).
//
// Like most viewers the camera's embedded JPEG is shown
// when it is close to full resolution: it is what the camera rendered, costs
// a 2 MB read plus a ~70 ms JPEG decode instead of a 12+ MB read and ~700 ms
// of demosaicing, and supports libjpeg's DCT scaling for quick previews.
// Set WIV_RAW_DECODE=1 to always demosaic the real RAW data instead. Files
// without a usable embedded JPEG fall back to LibRaw's half-size decode for
// previews and a full decode otherwise.
//
#include "../wiv_plugin_common.h"

#include <libraw/libraw.h>

#include <cstdio>
#include <jpeglib.h>

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    return std::equal(filename.end() - n, filename.end(), ext, [](char a, char b)
                       { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

int progressCallback(void* data, enum LibRaw_progress, int, int)
{
    const volatile int* cancel = static_cast<const volatile int*>(data);
    return cancel && *cancel ? 1 : 0;
}

struct JpegError
{
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void onJpegFatal(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1); }

// EXIF orientation value for LibRaw's `flip` (0, 3 = 180, 5 = 90 CCW, 6 = 90 CW).
int flipToExif(int flip)
{
    switch (flip)
    {
        case 3: return 3;
        case 5: return 8;
        case 6: return 6;
        default: return 1;
    }
}

// Decodes the embedded JPEG that unpack_thumb() just loaded. minLongSide > 0
// requests a preview at the largest 1/2, 1/4 or 1/8 scale that still has a
// long side >= minLongSide (nullptr if no reduction is possible).
WivPluginImage* decodeEmbeddedJpeg(const unsigned char* bytes, size_t len, int flip, const volatile int* cancel,
                                    int minLongSide, int* fullW, int* fullH)
{
    jpeg_decompress_struct cinfo;
    JpegError jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = onJpegFatal;
    WivPluginImage* img = nullptr;
    if (setjmp(jerr.jump))
    {
        jpeg_destroy_decompress(&cinfo);
        if (img) wiv_plugin_free_image(img);
        return nullptr;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes, len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_RGBA;
    if (minLongSide > 0)
    {
        const int longSide = int(std::max(cinfo.image_width, cinfo.image_height));
        int denom = 1;
        while (denom < 8 && longSide / (denom * 2) >= minLongSide) denom *= 2;
        if (denom == 1)
        {
            jpeg_destroy_decompress(&cinfo);
            return nullptr;
        }
        if (fullW) *fullW = int(cinfo.image_width), *fullH = int(cinfo.image_height);
        cinfo.scale_num = 1;
        cinfo.scale_denom = unsigned(denom);
        cinfo.dct_method = JDCT_IFAST;
    }
    jpeg_start_decompress(&cinfo);
    img = wiv_plugin::allocImage(int(cinfo.output_width), int(cinfo.output_height));
    img->orientation = flipToExif(flip);
    const size_t stride = size_t(cinfo.output_width) * 4;
    while (cinfo.output_scanline < cinfo.output_height)
    {
        if (cancel && (cinfo.output_scanline & 15) == 0 && *cancel)
        {
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            wiv_plugin_free_image(img);
            return nullptr;
        }
        unsigned char* row = img->rgba + size_t(cinfo.output_scanline) * stride;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return img;
}

// Decodes `filename`; halfSize != 0 asks for the fast quarter-pixel decode.
// *fullW/*fullH (if non-null) receive the full-resolution upright size.
WivPluginImage* decodeRaw(const char* filename, bool halfSize, int minLongSide, const volatile int* cancel,
                        int* fullW, int* fullH, char** error_msg)
{
    LibRaw raw;
    auto fail = [&](const char* msg) -> WivPluginImage*
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("raw: %s", msg);
        return nullptr;
    };
    auto failLR = [&](int code) -> WivPluginImage*
    {
        if (code == LIBRAW_CANCELLED_BY_CALLBACK)
            return nullptr;
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s: %s", filename, libraw_strerror(code));
        return fail(buf);
    };

    #ifdef _WIN32
    int rc = raw.open_file(wiv_plugin::utf8ToWide(filename).c_str());
#else
    int rc = raw.open_file(filename);
#endif
    if (rc != LIBRAW_SUCCESS)
        return failLR(rc);

    const auto& sz = raw.imgdata.sizes;
    const bool swap = sz.flip & 4; // 90/270 degree rotations
    const int upW = swap ? sz.iheight : sz.iwidth, upH = swap ? sz.iwidth : sz.iheight;
    // Embedded JPEG: for a preview any size >= minLongSide will do (and
    // scaled decoding makes it cheap); for the full image it must be nearly
    // sensor resolution, unless the user asked for real RAW decoding.
    const char* forceRaw = std::getenv("WIV_RAW_DECODE");
    const bool wantRaw = forceRaw && *forceRaw && *forceRaw != '0';
    if ((halfSize || !wantRaw) && raw.unpack_thumb() == LIBRAW_SUCCESS)
    {
        const auto& th = raw.imgdata.thumbnail;
        const int thLong = std::max<int>(th.twidth, th.theight);
        const bool usable = th.tformat == LIBRAW_THUMBNAIL_JPEG &&
                            (halfSize ? thLong >= minLongSide : thLong * 10 >= std::max(upW, upH) * 9);
        if (usable)
        {
            int jw = 0, jh = 0;
            WivPluginImage* img = decodeEmbeddedJpeg(reinterpret_cast<const unsigned char*>(th.thumb), th.tlength,
                                                      sz.flip, cancel, halfSize ? minLongSide : 0, &jw, &jh);
            if (img)
            {
                if (halfSize && fullW) *fullW = jw, *fullH = jh;
                return img;
            }
            if (cancel && *cancel)
                return nullptr;
            // No JPEG reduction possible, but a near-full-size embedded JPEG
            // makes the full load as cheap as any preview: let the host do that.
            if (halfSize && !wantRaw && thLong * 10 >= std::max(upW, upH) * 9)
                return nullptr;
        }
    }
    if (halfSize)
    {
        // Only worthwhile if the half-size result still covers the request.
        if (std::max(upW, upH) / 2 < minLongSide)
            return nullptr;
        if (fullW) *fullW = upW, *fullH = upH;
        raw.imgdata.params.half_size = 1;
    }
    if (fullW && !halfSize) *fullW = upW, *fullH = upH;

    auto& p = raw.imgdata.params;
    p.use_camera_wb = 1;
    p.output_bps = 8;
    p.output_color = 1; // sRGB
    raw.set_progress_handler(progressCallback, const_cast<int*>(cancel));

    if ((rc = raw.unpack()) != LIBRAW_SUCCESS)
        return failLR(rc);
    if ((rc = raw.dcraw_process()) != LIBRAW_SUCCESS)
        return failLR(rc);

    libraw_processed_image_t* im = raw.dcraw_make_mem_image(&rc);
    if (!im)
        return failLR(rc);
    WivPluginImage* img = nullptr;
    if (im->type == LIBRAW_IMAGE_BITMAP && im->bits == 8 && (im->colors == 3 || im->colors == 1))
    {
        img = wiv_plugin::allocImage(im->width, im->height);
        const unsigned char* s = im->data;
        for (size_t i = 0, n = size_t(im->width) * im->height; i < n; ++i, s += im->colors)
        {
            img->rgba[i * 4 + 0] = s[0];
            img->rgba[i * 4 + 1] = im->colors == 3 ? s[1] : s[0];
            img->rgba[i * 4 + 2] = im->colors == 3 ? s[2] : s[0];
            img->rgba[i * 4 + 3] = 255;
        }
    }
    LibRaw::dcraw_clear_mem(im);
    return img ? img : fail("unsupported decoded format");
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    static const char* kExtensions[] = {".cr2", ".cr3", ".crw", ".nef", ".nrw", ".arw", ".sr2", ".srf", ".dng",
                                        ".orf", ".rw2", ".raf", ".pef", ".srw", ".raw", ".rwl", ".3fr", ".fff",
                                        ".erf", ".kdc", ".dcr", ".mef", ".mos", ".mrw", ".iiq", ".x3f", ".gpr"};
    for (const char* ext : kExtensions)
        if (hasExtension(f, ext))
            return true;
    return false;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    return decodeRaw(filename, false, 0, nullptr, nullptr, nullptr, error_msg);
}

extern "C" WivPluginImage* wiv_plugin_load_page_cancellable(const char* filename, int /*page*/,
                                                              const volatile int* cancel, char** error_msg)
{
    return decodeRaw(filename, false, 0, cancel, nullptr, nullptr, error_msg);
}

extern "C" WivPluginImage* wiv_plugin_load_preview(const char* filename, int min_long_side, int* full_width,
                                                     int* full_height, char** error_msg)
{
    return decodeRaw(filename, true, min_long_side, nullptr, full_width, full_height, error_msg);
}
