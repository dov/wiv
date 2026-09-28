//
// npy_plugin.cpp - wiv image plugin for NumPy .npy files (v1.0 header).
// Modeled on giv's ~/hd/github/giv/src/plugins/npy.c, ported off glib
// (g_regex/g_file_get_contents) onto std::regex and plain iostreams, and
// extended to expose 3D arrays (shape (depth,height,width)) as a
// multi-slice image (WivPluginImage::depth) instead of giv's own
// (separate, GtkImageViewer-side) slice handling.
//
// Only little-endian dtypes and C (row-major, fortran_order=False) layout
// are supported - the only combination giv's own npy.c ever handled.
// wiv's raw-sample buffer has no signed-16/32-bit or float64 type (see
// wiv_plugin.h's WivSampleType), so i2/i4/u4/f8 are converted to FLOAT
// samples at load time; u1/u2 are copied through as-is (U8/U16).
//
#include "../wiv_plugin_common.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace
{

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    std::string tail = filename.substr(filename.size() - n);
    for (char& c : tail)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ext;
}

// Converts `count` little-endian native-typed elements at `src` into
// `dst` as float, for the dtypes wiv has no matching raw sample type for.
template <typename T>
void convertToFloat(const unsigned char* src, float* dst, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        T v;
        std::memcpy(&v, src + i * sizeof(T), sizeof(T));
        dst[i] = static_cast<float>(v);
    }
}

// Builds a quick min/max-normalized grayscale RGBA8 preview of slice 0 -
// never actually displayed (DisplayImage.h regenerates the shown image from
// the raw sample buffer whenever sampleType != WIV_SAMPLE_NONE), just a
// reasonable placeholder for anything that looks at img->rgba directly.
void fillPreview(WivPluginImage* img, size_t sliceElemCount)
{
    double lo = 0.0, hi = 0.0;
    auto valueAt = [&](size_t i) -> double
    {
        switch (img->sampleType)
        {
            case WIV_SAMPLE_U8: return static_cast<const uint8_t*>(img->samples)[i];
            case WIV_SAMPLE_U16: return static_cast<const uint16_t*>(img->samples)[i];
            case WIV_SAMPLE_FLOAT: return static_cast<const float*>(img->samples)[i];
            default: return 0.0;
        }
    };
    if (sliceElemCount > 0)
    {
        lo = hi = valueAt(0);
        for (size_t i = 1; i < sliceElemCount; ++i)
        {
            double v = valueAt(i);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    const double range = hi - lo;
    for (size_t i = 0; i < sliceElemCount; ++i)
    {
        double t = range != 0.0 ? (valueAt(i) - lo) / range : 0.5;
        uint8_t g = static_cast<uint8_t>(std::clamp(static_cast<int>(t * 255.0 + 0.5), 0, 255));
        img->rgba[i * 4 + 0] = g;
        img->rgba[i * 4 + 1] = g;
        img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(filename, ".npy");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: failed to open %s", filename);
        return nullptr;
    }

    unsigned char preamble[10];
    if (std::fread(preamble, 1, sizeof(preamble), fh) != sizeof(preamble) ||
        std::memcmp(preamble, "\x93NUMPY", 6) != 0)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: not a valid .npy file: %s", filename);
        return nullptr;
    }
    const uint16_t headerLen = static_cast<uint16_t>(preamble[8] | (preamble[9] << 8));

    std::string header(headerLen, '\0');
    if (std::fread(header.data(), 1, headerLen, fh) != headerLen)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: truncated header in %s", filename);
        return nullptr;
    }

    static const std::regex kHeaderRe(
        R"(^\{\s*'descr':\s*'(.*?)'\s*,\s*'fortran_order':\s*(\w+)\s*,\s*)"
        R"('shape':\s*\(\s*(\d+)\s*,\s*(\d+)(?:,\s*(\d+))?\s*\),?\s*\})");
    std::smatch match;
    if (!std::regex_search(header, match, kHeaderRe) || match[2].str() != "False")
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: unsupported/malformed header in %s", filename);
        return nullptr;
    }

    const std::string dtype = match[1].str();
    const long dim0 = std::stol(match[3].str());
    const long dim1 = std::stol(match[4].str());
    const bool has3rdDim = match[5].matched;
    const long dim2 = has3rdDim ? std::stol(match[5].str()) : 0;

    int width, height, depth;
    if (has3rdDim)
    {
        depth = static_cast<int>(dim0);
        height = static_cast<int>(dim1);
        width = static_cast<int>(dim2);
    }
    else
    {
        depth = 1;
        height = static_cast<int>(dim0);
        width = static_cast<int>(dim1);
    }
    if (width <= 0 || height <= 0 || depth <= 0)
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: invalid shape in %s", filename);
        return nullptr;
    }

    size_t nativeElemSize;
    WivSampleType sampleType;
    bool needsFloatConvert = false;
    enum class Native { U8, U16, I16, U32, I32, F32, F64 } native;
    if (dtype == "|u1") { nativeElemSize = 1; sampleType = WIV_SAMPLE_U8; native = Native::U8; }
    else if (dtype == "<u2") { nativeElemSize = 2; sampleType = WIV_SAMPLE_U16; native = Native::U16; }
    else if (dtype == "<i2") { nativeElemSize = 2; sampleType = WIV_SAMPLE_FLOAT; native = Native::I16; needsFloatConvert = true; }
    else if (dtype == "<u4") { nativeElemSize = 4; sampleType = WIV_SAMPLE_FLOAT; native = Native::U32; needsFloatConvert = true; }
    else if (dtype == "<i4") { nativeElemSize = 4; sampleType = WIV_SAMPLE_FLOAT; native = Native::I32; needsFloatConvert = true; }
    else if (dtype == "<f4") { nativeElemSize = 4; sampleType = WIV_SAMPLE_FLOAT; native = Native::F32; }
    else if (dtype == "<f8") { nativeElemSize = 8; sampleType = WIV_SAMPLE_FLOAT; native = Native::F64; needsFloatConvert = true; }
    else
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: unsupported dtype in %s", filename);
        return nullptr;
    }

    const size_t sliceElemCount = static_cast<size_t>(width) * height;
    const size_t totalElemCount = sliceElemCount * static_cast<size_t>(depth);

    std::vector<unsigned char> raw(totalElemCount * nativeElemSize);
    if (std::fread(raw.data(), 1, raw.size(), fh) != raw.size())
    {
        std::fclose(fh);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("npy: truncated pixel data in %s", filename);
        return nullptr;
    }
    std::fclose(fh);

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    wiv_plugin::allocSamples(img, sampleType, depth);

    if (!needsFloatConvert)
    {
        std::memcpy(img->samples, raw.data(), raw.size());
    }
    else
    {
        float* dst = static_cast<float*>(img->samples);
        switch (native)
        {
            case Native::I16: convertToFloat<int16_t>(raw.data(), dst, totalElemCount); break;
            case Native::U32: convertToFloat<uint32_t>(raw.data(), dst, totalElemCount); break;
            case Native::I32: convertToFloat<int32_t>(raw.data(), dst, totalElemCount); break;
            case Native::F64: convertToFloat<double>(raw.data(), dst, totalElemCount); break;
            default: break; // unreachable - only the four cases above set needsFloatConvert
        }
    }

    fillPreview(img, sliceElemCount);
    return img;
}
