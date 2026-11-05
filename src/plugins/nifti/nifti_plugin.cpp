//
// nifti_plugin.cpp - wiv image plugin for NIfTI-1 (.nii, .nii.gz) files.
// Counterpart of giv's ~/hd/github/giv/src/plugins/nifti.c, but a small
// self-contained header parser on top of zlib instead of a dependency on
// nifti_clib. 2D and 3D volumes are supported (a trailing singleton 4th
// dimension is accepted); the z axis becomes the slice axis (Up/Down).
// Either byte order is handled. scl_slope/scl_inter are applied when set.
//
// wiv's raw-sample buffer has no signed or 32-bit integer type (see
// wiv_plugin.h), so everything except uint8/uint16 without scaling is
// converted to FLOAT samples at load time.
//
#include "../wiv_plugin_common.h"

#include <zlib.h>

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

template <typename T>
T swapBytes(T v)
{
    unsigned char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    std::reverse(b, b + sizeof(T));
    std::memcpy(&v, b, sizeof(T));
    return v;
}

template <typename T>
T readField(const unsigned char* hdr, size_t off, bool swap)
{
    T v;
    std::memcpy(&v, hdr + off, sizeof(T));
    return swap ? swapBytes(v) : v;
}

// Converts `count` elements of file type T to float, applying slope/inter.
template <typename T>
void convert(const unsigned char* src, float* dst, size_t count, bool swap, double slope, double inter)
{
    for (size_t i = 0; i < count; ++i)
    {
        T v;
        std::memcpy(&v, src + i * sizeof(T), sizeof(T));
        if (swap)
            v = swapBytes(v);
        dst[i] = static_cast<float>(static_cast<double>(v) * slope + inter);
    }
}

// Min/max-normalized grayscale preview of slice 0 (see npy_plugin.cpp).
void fillPreview(WivPluginImage* img)
{
    const size_t n = static_cast<size_t>(img->width) * img->height;
    auto valueAt = [&](size_t i) -> double
    {
        switch (img->sampleType)
        {
            case WIV_SAMPLE_U8: return static_cast<const uint8_t*>(img->samples)[i];
            case WIV_SAMPLE_U16: return static_cast<const uint16_t*>(img->samples)[i];
            default: return static_cast<const float*>(img->samples)[i];
        }
    };
    double lo = valueAt(0), hi = lo;
    for (size_t i = 1; i < n; ++i)
    {
        double v = valueAt(i);
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const double range = hi - lo;
    for (size_t i = 0; i < n; ++i)
    {
        double t = range > 0.0 ? (valueAt(i) - lo) / range : 0.5;
        auto g = static_cast<uint8_t>(std::clamp(static_cast<int>(t * 255.0 + 0.5), 0, 255));
        img->rgba[i * 4 + 0] = img->rgba[i * 4 + 1] = img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".nii") || hasExtension(f, ".nii.gz");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    auto fail = [&](const char* msg) -> WivPluginImage*
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(msg, filename);
        return nullptr;
    };

    // gzopen reads plain, uncompressed files transparently too.
    gzFile gz = gzopen(filename, "rb");
    if (!gz)
        return fail("nifti: failed to open %s");

    unsigned char hdr[348];
    if (gzread(gz, hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr)))
    {
        gzclose(gz);
        return fail("nifti: truncated header in %s");
    }

    bool swap = false;
    int32_t hdrSize = readField<int32_t>(hdr, 0, false);
    if (hdrSize != 348)
    {
        swap = true;
        hdrSize = readField<int32_t>(hdr, 0, true);
    }
    if (hdrSize != 348 || (std::memcmp(hdr + 344, "n+1", 3) != 0 && std::memcmp(hdr + 344, "ni1", 3) != 0))
    {
        gzclose(gz);
        return fail("nifti: not a NIfTI-1 file: %s");
    }

    const int ndim = readField<int16_t>(hdr, 40, swap);
    int dims[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    for (int i = 1; i <= 7 && i <= std::max(ndim, 0); ++i)
        dims[i] = readField<int16_t>(hdr, 40 + 2 * i, swap);
    if (ndim < 2 || ndim > 4 || (ndim == 4 && dims[4] != 1) || dims[1] <= 0 || dims[2] <= 0 || dims[3] <= 0)
    {
        gzclose(gz);
        return fail("nifti: only 2D and 3D volumes are supported: %s");
    }
    const int width = dims[1], height = dims[2], depth = dims[3];

    const int datatype = readField<int16_t>(hdr, 70, swap);
    size_t elemSize;
    switch (datatype)
    {
        case 2: case 256: elemSize = 1; break;       // uint8, int8
        case 4: case 512: elemSize = 2; break;       // int16, uint16
        case 8: case 768: case 16: elemSize = 4; break; // int32, uint32, float32
        case 64: elemSize = 8; break;                // float64
        default:
            gzclose(gz);
            return fail("nifti: unsupported data type in %s");
    }

    double slope = readField<float>(hdr, 112, swap);
    const double inter = readField<float>(hdr, 116, swap);
    if (!std::isfinite(slope) || slope == 0.0)
        slope = 1.0;
    const bool scaled = slope != 1.0 || (std::isfinite(inter) && inter != 0.0);

    const auto voxOffset = static_cast<z_off_t>(readField<float>(hdr, 108, swap));
    if (voxOffset < 352 || gzseek(gz, voxOffset, SEEK_SET) < 0)
    {
        gzclose(gz);
        return fail("nifti: bad data offset in %s");
    }

    const size_t total = static_cast<size_t>(width) * height * depth;
    std::vector<unsigned char> raw(total * elemSize);
    const size_t chunk = 1u << 28; // gzread takes an unsigned int length
    for (size_t done = 0; done < raw.size();)
    {
        int want = static_cast<int>(std::min(chunk, raw.size() - done));
        int got = gzread(gz, raw.data() + done, want);
        if (got <= 0)
        {
            gzclose(gz);
            return fail("nifti: truncated pixel data in %s");
        }
        done += static_cast<size_t>(got);
    }
    gzclose(gz);

    const bool direct = !scaled && (datatype == 2 || (datatype == 512));
    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    wiv_plugin::allocSamples(img, direct ? (datatype == 2 ? WIV_SAMPLE_U8 : WIV_SAMPLE_U16) : WIV_SAMPLE_FLOAT, depth);

    if (direct)
    {
        std::memcpy(img->samples, raw.data(), raw.size());
        if (swap && datatype == 512)
        {
            auto* p = static_cast<uint16_t*>(img->samples);
            for (size_t i = 0; i < total; ++i) p[i] = swapBytes(p[i]);
        }
    }
    else
    {
        auto* dst = static_cast<float*>(img->samples);
        const double in = std::isfinite(inter) ? inter : 0.0;
        switch (datatype)
        {
            case 2: convert<uint8_t>(raw.data(), dst, total, swap, slope, in); break;
            case 256: convert<int8_t>(raw.data(), dst, total, swap, slope, in); break;
            case 4: convert<int16_t>(raw.data(), dst, total, swap, slope, in); break;
            case 512: convert<uint16_t>(raw.data(), dst, total, swap, slope, in); break;
            case 8: convert<int32_t>(raw.data(), dst, total, swap, slope, in); break;
            case 768: convert<uint32_t>(raw.data(), dst, total, swap, slope, in); break;
            case 16: convert<float>(raw.data(), dst, total, swap, slope, in); break;
            case 64: convert<double>(raw.data(), dst, total, swap, slope, in); break;
        }
    }

    fillPreview(img);
    return img;
}
