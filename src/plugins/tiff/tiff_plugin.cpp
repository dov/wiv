// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
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
#include <cmath>
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

namespace wiv_plugin
{
inline TIFF* tiffOpen(const char* filename)
{
#ifdef _WIN32
    return TIFFOpenW(utf8ToWide(filename).c_str(), "r");
#else
    return TIFFOpen(filename, "r");
#endif
}
}

extern "C" int wiv_plugin_page_count(const char* filename)
{
    TIFF* tif = wiv_plugin::tiffOpen(filename);
    if (!tif)
        return 0;
    const int count = static_cast<int>(TIFFNumberOfDirectories(tif));
    TIFFClose(tif);
    return count;
}

namespace
{


template <typename T>
void convertRow(const unsigned char* src, float* dst, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        T v;
        std::memcpy(&v, src + i * sizeof(T), sizeof(T));
        dst[i] = static_cast<float>(v);
    }
}

void fillFloatPreview(WivPluginImage* img, size_t count)
{
    const float* f = static_cast<const float*>(img->samples);
    float lo = 0.f, hi = 0.f;
    bool any = false;
    for (size_t i = 0; i < count; ++i)
    {
        if (!std::isfinite(f[i]))
            continue;
        if (!any) { lo = hi = f[i]; any = true; }
        else { lo = std::min(lo, f[i]); hi = std::max(hi, f[i]); }
    }
    const double range = static_cast<double>(hi) - lo;
    for (size_t i = 0; i < count; ++i)
    {
        double t = (std::isfinite(f[i]) && range != 0.0) ? (f[i] - lo) / range : 0.5;
        uint8_t g = static_cast<uint8_t>(std::clamp(static_cast<int>(t * 255.0 + 0.5), 0, 255));
        img->rgba[i * 4 + 0] = g;
        img->rgba[i * 4 + 1] = g;
        img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }
}

WivPluginImage* loadTiff(const char* filename, int page, const volatile int* cancel, char** error_msg)
{
    TIFF* tif = wiv_plugin::tiffOpen(filename);
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

    // Native sample layout -> raw sample type. Types wiv has no raw type for
    // (signed/32-bit int, float16/64) are converted to FLOAT row by row.
    enum class Conv { None, I8, I16, U32, I32, F64 };
    Conv conv = Conv::None;
    WivSampleType sampleType = WIV_SAMPLE_NONE;
    if (samplesPerPixel == 1)
    {
        if (sampleFormat == SAMPLEFORMAT_UINT && bitsPerSample == 8)
            sampleType = WIV_SAMPLE_U8;
        else if (sampleFormat == SAMPLEFORMAT_UINT && bitsPerSample == 16)
            sampleType = WIV_SAMPLE_U16;
        else if (sampleFormat == SAMPLEFORMAT_IEEEFP && bitsPerSample == 32)
            sampleType = WIV_SAMPLE_FLOAT;
        else if (sampleFormat == SAMPLEFORMAT_IEEEFP && bitsPerSample == 64)
            { sampleType = WIV_SAMPLE_FLOAT; conv = Conv::F64; }
        else if (sampleFormat == SAMPLEFORMAT_INT && bitsPerSample == 8)
            { sampleType = WIV_SAMPLE_FLOAT; conv = Conv::I8; }
        else if (sampleFormat == SAMPLEFORMAT_INT && bitsPerSample == 16)
            { sampleType = WIV_SAMPLE_FLOAT; conv = Conv::I16; }
        else if (sampleFormat == SAMPLEFORMAT_UINT && bitsPerSample == 32)
            { sampleType = WIV_SAMPLE_FLOAT; conv = Conv::U32; }
        else if (sampleFormat == SAMPLEFORMAT_INT && bitsPerSample == 32)
            { sampleType = WIV_SAMPLE_FLOAT; conv = Conv::I32; }
    }

    if (sampleType != WIV_SAMPLE_NONE)
    {
        wiv_plugin::allocSamples(img, sampleType);
        const tmsize_t scanlineSize = TIFFScanlineSize(tif);
        std::vector<unsigned char> scanline(static_cast<size_t>(scanlineSize));
        const size_t elemSize = wiv_plugin::sampleTypeSize(sampleType);
        bool ok = true;
        for (uint32_t row = 0; row < height; ++row)
        {
            if (cancel && (row & 15) == 0 && *cancel)
            {
                TIFFClose(tif);
                wiv_plugin_free_image(img);
                return nullptr;
            }
            if (TIFFReadScanline(tif, scanline.data(), row, 0) < 0)
            {
                ok = false;
                break;
            }
            unsigned char* dstRow = static_cast<unsigned char*>(img->samples) + static_cast<size_t>(row) * width * elemSize;
            if (conv == Conv::None)
                std::memcpy(dstRow, scanline.data(), static_cast<size_t>(width) * elemSize);
            else
            {
                float* d = reinterpret_cast<float*>(dstRow);
                switch (conv)
                {
                    case Conv::I8: convertRow<int8_t>(scanline.data(), d, width); break;
                    case Conv::I16: convertRow<int16_t>(scanline.data(), d, width); break;
                    case Conv::U32: convertRow<uint32_t>(scanline.data(), d, width); break;
                    case Conv::I32: convertRow<int32_t>(scanline.data(), d, width); break;
                    case Conv::F64: convertRow<double>(scanline.data(), d, width); break;
                    default: break;
                }
            }
        }
        if (!ok)
        {
            // Raw scanline decode failed - keep the RGBA8 preview, just
            // drop the raw path rather than failing the whole load.
            std::free(img->samples);
            img->samples = nullptr;
            img->sampleType = WIV_SAMPLE_NONE;
            sampleType = WIV_SAMPLE_NONE;
        }
    }

    // libtiff's RGBA reader rejects float/signed/32-bit samples, and the
    // viewer regenerates the display from the raw buffer anyway (see
    // DisplayImage.h), so for those build a min/max-normalized preview here.
    if (img->samples && sampleType == WIV_SAMPLE_FLOAT)
    {
        fillFloatPreview(img, static_cast<size_t>(width) * height);
        TIFFClose(tif);
        return img;
    }

    // TIFFReadRGBAImageOriented below is one monolithic libtiff call, so
    // this is the last point at which a cancel request can take effect.
    if (cancel && *cancel)
    {
        TIFFClose(tif);
        wiv_plugin_free_image(img);
        return nullptr;
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

} // namespace

extern "C" WivPluginImage* wiv_plugin_load_page(const char* filename, int page, char** error_msg)
{
    return loadTiff(filename, page, nullptr, error_msg);
}

extern "C" WivPluginImage* wiv_plugin_load_page_cancellable(const char* filename, int page,
                                                              const volatile int* cancel, char** error_msg)
{
    return loadTiff(filename, page, cancel, error_msg);
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    return loadTiff(filename, 0, nullptr, error_msg);
}
