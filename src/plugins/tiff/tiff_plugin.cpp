//
// tiff_plugin.cpp - wiv image plugin for TIFF, via libtiff. Modeled on
// giv's ~/hd/github/giv/src/plugins/tiff.c. Uses libtiff's any-format-to-
// RGBA8 convenience reader (TIFFReadRGBAImageOriented) for the always-
// present RGBA8 preview. Additionally, for single-channel (grayscale)
// TIFFs in one of the sample formats the Contrast/Color Table tools
// understand (8/16-bit unsigned, 32-bit float), decodes the raw scanlines
// a second time into a raw sample buffer so those tools can operate on the
// true dynamic range instead of the already-quantized preview. Other
// combos (color, bilevel, signed/32-bit-int samples) stay RGBA8-only.
//
#include "../wiv_plugin_common.h"

#include <tiffio.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".tif") || hasExtension(f, ".tiff");
}

extern "C" int wiv_plugin_page_count(const char* filename)
{
    TIFF* tif = TIFFOpen(filename, "r");
    if (!tif)
        return 0;
    const int count = static_cast<int>(TIFFNumberOfDirectories(tif));
    TIFFClose(tif);
    return count;
}

extern "C" WivPluginImage* wiv_plugin_load_page(const char* filename, int page, char** error_msg)
{
    TIFF* tif = TIFFOpen(filename, "r");
    if (!tif)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: failed to open %s", filename);
        return nullptr;
    }
    if (page > 0 && !TIFFSetDirectory(tif, static_cast<tdir_t>(page)))
    {
        TIFFClose(tif);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: no such page in %s", filename);
        return nullptr;
    }

    uint32_t width = 0, height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    if (width == 0 || height == 0)
    {
        TIFFClose(tif);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: invalid dimensions in %s", filename);
        return nullptr;
    }

    WivPluginImage* img = wiv_plugin::allocImage(static_cast<int>(width), static_cast<int>(height));

    // Decode the raw single-channel sample buffer (if applicable) before
    // TIFFReadRGBAImageOriented below, so TIFFReadScanline's own strip/row
    // cursor is read in plain increasing order from a freshly opened
    // handle rather than after RGBAImageOriented's internal decode state.
    uint16_t samplesPerPixel = 1, bitsPerSample = 8, sampleFormat = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);

    WivSampleType sampleType = WIV_SAMPLE_NONE;
    if (samplesPerPixel == 1)
    {
        if (sampleFormat == SAMPLEFORMAT_UINT && bitsPerSample == 8)
            sampleType = WIV_SAMPLE_U8;
        else if (sampleFormat == SAMPLEFORMAT_UINT && bitsPerSample == 16)
            sampleType = WIV_SAMPLE_U16;
        else if (sampleFormat == SAMPLEFORMAT_IEEEFP && bitsPerSample == 32)
            sampleType = WIV_SAMPLE_FLOAT;
    }

    if (sampleType != WIV_SAMPLE_NONE)
    {
        wiv_plugin::allocSamples(img, sampleType);
        const tmsize_t scanlineSize = TIFFScanlineSize(tif);
        std::vector<unsigned char> scanline(static_cast<size_t>(scanlineSize));
        bool ok = true;
        for (uint32_t row = 0; row < height; ++row)
        {
            if (TIFFReadScanline(tif, scanline.data(), row, 0) < 0)
            {
                ok = false;
                break;
            }
            std::memcpy(static_cast<unsigned char*>(img->samples) +
                            static_cast<size_t>(row) * width * wiv_plugin::sampleTypeSize(sampleType),
                        scanline.data(), static_cast<size_t>(width) * wiv_plugin::sampleTypeSize(sampleType));
        }
        if (!ok)
        {
            // Raw scanline decode failed - keep the RGBA8 preview, just
            // drop the raw path rather than failing the whole load.
            std::free(img->samples);
            img->samples = nullptr;
            img->sampleType = WIV_SAMPLE_NONE;
        }
    }

    // TIFFReadRGBAImageOriented fills top-to-bottom, ABGR-packed-as-uint32
    // (i.e. byte order R,G,B,A on little-endian hosts) - exactly the
    // layout WivPluginImage::rgba expects.
    if (!TIFFReadRGBAImageOriented(tif, width, height, reinterpret_cast<uint32_t*>(img->rgba),
                                    ORIENTATION_TOPLEFT, 0))
    {
        TIFFClose(tif);
        wiv_plugin_free_image(img);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: failed to decode %s", filename);
        return nullptr;
    }

    TIFFClose(tif);
    return img;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    return wiv_plugin_load_page(filename, 0, error_msg);
}
