// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
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
#include <cstdio>
#include <cstdint>
#include <bit>
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


// ---- Metadata (File > Image Info) ----

const char* niftiDatatypeName(int dt)
{
    switch (dt)
    {
        case 1: return "binary (1 bit)";
        case 2: return "uint8";
        case 4: return "int16";
        case 8: return "int32";
        case 16: return "float32";
        case 32: return "complex64";
        case 64: return "float64";
        case 128: return "RGB24";
        case 256: return "int8";
        case 512: return "uint16";
        case 768: return "uint32";
        case 1024: return "int64";
        case 1280: return "uint64";
        case 1536: return "float128";
        case 1792: return "complex128";
        case 2048: return "complex256";
        case 2304: return "RGBA32";
        default: return "unknown";
    }
}

const char* niftiIntentName(int code)
{
    switch (code)
    {
        case 0: return "none";
        case 2: return "correlation statistic";
        case 3: return "t statistic";
        case 4: return "F statistic";
        case 5: return "z score";
        case 6: return "chi-squared";
        case 10: return "chi statistic";
        case 22: return "p-value";
        case 1001: return "estimate";
        case 1002: return "label index";
        case 1003: return "NeuroNames index";
        case 1004: return "general matrix";
        case 1005: return "symmetric matrix";
        case 1006: return "displacement vector";
        case 1007: return "vector";
        case 1008: return "point set";
        case 1009: return "triangle set";
        case 1010: return "quaternion";
        case 1011: return "dimensionless";
        case 2001: return "time series";
        case 2002: return "node index";
        case 2003: return "RGB vector";
        case 2004: return "RGBA vector";
        case 2005: return "shape";
        default: return nullptr;
    }
}

const char* niftiSpaceUnit(int u)
{
    switch (u)
    {
        case 1: return "m";
        case 2: return "mm";
        case 3: return "um";
        default: return "";
    }
}

const char* niftiTimeUnit(int u)
{
    switch (u)
    {
        case 8: return "s";
        case 16: return "ms";
        case 24: return "us";
        case 32: return "Hz";
        case 40: return "ppm";
        case 48: return "rad/s";
        default: return "";
    }
}

const char* niftiXformName(int code)
{
    switch (code)
    {
        case 0: return "unknown";
        case 1: return "scanner anatomical coordinates";
        case 2: return "aligned to another file";
        case 3: return "Talairach";
        case 4: return "MNI 152";
        case 5: return "template";
        default: return "unknown";
    }
}

const char* niftiSliceCodeName(int code)
{
    switch (code)
    {
        case 0: return "unknown";
        case 1: return "sequential, increasing";
        case 2: return "sequential, decreasing";
        case 3: return "alternating, increasing";
        case 4: return "alternating, decreasing";
        case 5: return "alternating, increasing (2)";
        case 6: return "alternating, decreasing (2)";
        default: return "unknown";
    }
}

// A fixed-size, possibly unterminated text field.
std::string textField(const unsigned char* p, size_t n)
{
    size_t len = 0;
    while (len < n && p[len] != 0) ++len;
    std::string s(reinterpret_cast<const char*>(p), len);
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127) c = ' ';
    return s;
}

// "RAS", "LPS", ... : for each voxel axis, the world direction (R/L, A/P, S/I) in which the
// index increases, from the 3x3 part of the voxel -> world affine (nibabel's aff2axcodes).
std::string orientationCode(const double m[3][4])
{
    static const char* pos = "RAS";
    static const char* neg = "LPI";
    std::string code;
    for (int col = 0; col < 3; ++col)
    {
        int best = 0;
        for (int row = 1; row < 3; ++row)
            if (std::fabs(m[row][col]) > std::fabs(m[best][col])) best = row;
        if (m[best][col] == 0.0) return "";
        code += m[best][col] > 0 ? pos[best] : neg[best];
    }
    return code;
}

std::string matrixRow(const double* r)
{
    return wiv_plugin::MetadataBuilder::number(r[0]) + "  " + wiv_plugin::MetadataBuilder::number(r[1]) + "  " +
           wiv_plugin::MetadataBuilder::number(r[2]) + "  " + wiv_plugin::MetadataBuilder::number(r[3]);
}

// qform: unit quaternion (b, c, d), voxel size and qfac -> affine (NIfTI-1 "method 2").
void qformMatrix(double b, double c, double d, double qx, double qy, double qz, const double dx, const double dy,
                 const double dz, double qfac, double m[3][4])
{
    double a = 1.0 - (b * b + c * c + d * d);
    a = a < 1e-7 ? 0.0 : std::sqrt(a);
    const double R[3][3] = {
        {a * a + b * b - c * c - d * d, 2 * b * c - 2 * a * d, 2 * b * d + 2 * a * c},
        {2 * b * c + 2 * a * d, a * a + c * c - b * b - d * d, 2 * c * d - 2 * a * b},
        {2 * b * d - 2 * a * c, 2 * c * d + 2 * a * b, a * a + d * d - c * c - b * b},
    };
    const double zoom[3] = {dx, dy, dz * (qfac < 0 ? -1.0 : 1.0)};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = R[i][j] * zoom[j];
    m[0][3] = qx;
    m[1][3] = qy;
    m[2][3] = qz;
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

extern "C" char* wiv_plugin_metadata(const char* filename, int /*page*/)
{
    gzFile gz = gzopen(filename, "rb");
    if (!gz)
        return nullptr;
    unsigned char hdr[348];
    if (gzread(gz, hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr)))
    {
        gzclose(gz);
        return nullptr;
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
        return nullptr;
    }
    auto i16 = [&](size_t off) { return static_cast<int>(readField<int16_t>(hdr, off, swap)); };
    auto f32 = [&](size_t off) { return static_cast<double>(readField<float>(hdr, off, swap)); };

    wiv_plugin::MetadataBuilder md;
    const char* S = "NIfTI header";
    md.add(S, "Version", "NIfTI-1");
    md.add(S, "Byte order", swap ? (std::endian::native == std::endian::little ? "big-endian" : "little-endian")
                                 : (std::endian::native == std::endian::little ? "little-endian" : "big-endian"));
    md.add(S, "Storage", std::memcmp(hdr + 344, "n+1", 3) == 0 ? "single file (.nii)" : "header/image pair (.hdr/.img)");

    const int ndim = i16(40);
    std::string dimStr;
    for (int i = 1; i <= std::min(std::max(ndim, 0), 7); ++i)
        dimStr += (i > 1 ? " \xC3\x97 " : "") + std::to_string(i16(40 + 2 * i));
    md.add(S, "Dimensions", std::to_string(ndim) + "D: " + dimStr);

    const int dt = i16(70);
    md.add(S, "Data type", std::string(niftiDatatypeName(dt)) + " (code " + std::to_string(dt) + ")");
    md.add(S, "Bits per voxel", i16(72));
    const std::string dataType = textField(hdr + 4, 10);
    if (!dataType.empty()) md.add(S, "data_type", dataType);
    const std::string dbName = textField(hdr + 14, 18);
    if (!dbName.empty()) md.add(S, "db_name", dbName);

    const int xyzt = hdr[123];
    const std::string su = niftiSpaceUnit(xyzt & 7), tu = niftiTimeUnit(xyzt & 0x38);
    std::string vox;
    for (int i = 1; i <= std::min(std::max(ndim, 0), 7); ++i)
    {
        vox += (i > 1 ? " \xC3\x97 " : "") + wiv_plugin::MetadataBuilder::number(f32(76 + 4 * i));
    }
    if (ndim > 0)
        md.add(S, "Voxel size", vox + (su.empty() && tu.empty() ? "" : " (" + su + (tu.empty() ? "" : ", " + tu) + ")"));
    md.add(S, "Spatial unit", su.empty() ? "unknown" : su);
    md.add(S, "Temporal unit", tu.empty() ? "unknown" : tu);
    const double qfac = f32(76);
    if (qfac != 0.0 && qfac != 1.0) md.add(S, "qfac (pixdim[0])", qfac);

    const double slope = f32(112), inter = f32(116);
    md.add(S, "Scaling (slope, intercept)",
           (slope == 0.0 ? std::string("0 (no scaling)") : wiv_plugin::MetadataBuilder::number(slope)) + ", " +
               wiv_plugin::MetadataBuilder::number(inter));
    md.add(S, "Display range (cal_min, cal_max)",
           wiv_plugin::MetadataBuilder::number(f32(128)) + ", " + wiv_plugin::MetadataBuilder::number(f32(124)));
    const int glmin = readField<int32_t>(hdr, 144, swap), glmax = readField<int32_t>(hdr, 140, swap);
    if (glmin != 0 || glmax != 0) md.add(S, "Global min / max (glmin, glmax)", std::to_string(glmin) + ", " + std::to_string(glmax));
    md.add(S, "Data offset (vox_offset)", f32(108));

    const int dimInfo = hdr[39];
    if (dimInfo != 0)
        md.add(S, "Frequency / phase / slice dims",
               std::to_string(dimInfo & 3) + " / " + std::to_string((dimInfo >> 2) & 3) + " / " + std::to_string((dimInfo >> 4) & 3));
    const int intent = i16(68);
    {
        const char* name = niftiIntentName(intent);
        md.add(S, "Intent", name ? std::string(name) + " (code " + std::to_string(intent) + ")" : "code " + std::to_string(intent));
        if (intent != 0)
            md.add(S, "Intent parameters", wiv_plugin::MetadataBuilder::number(f32(56)) + ", " +
                                              wiv_plugin::MetadataBuilder::number(f32(60)) + ", " +
                                              wiv_plugin::MetadataBuilder::number(f32(64)));
        const std::string intentName = textField(hdr + 328, 16);
        if (!intentName.empty()) md.add(S, "Intent name", intentName);
    }
    const int sliceCode = hdr[122];
    if (sliceCode != 0 || i16(74) != 0 || i16(120) != 0)
    {
        md.add(S, "Slice order", std::string(niftiSliceCodeName(sliceCode)));
        md.add(S, "Slice range (start, end)", std::to_string(i16(74)) + ", " + std::to_string(i16(120)));
        md.add(S, "Slice duration", f32(132));
    }
    if (f32(136) != 0.0) md.add(S, "Time offset (toffset)", f32(136));
    const std::string descrip = textField(hdr + 148, 80);
    if (!descrip.empty()) md.add(S, "Description", descrip);
    const std::string aux = textField(hdr + 228, 24);
    if (!aux.empty()) md.add(S, "Auxiliary file", aux);

    // ---- Spatial orientation ----
    const char* G = "NIfTI orientation";
    const int qcode = i16(252), scode = i16(254);
    md.add(G, "qform code", std::to_string(qcode) + " (" + niftiXformName(qcode) + ")");
    md.add(G, "sform code", std::to_string(scode) + " (" + niftiXformName(scode) + ")");
    double affine[3][4] = {};
    bool haveAffine = false;
    if (scode > 0)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) affine[r][c] = f32(280 + 16 * r + 4 * c);
        haveAffine = true;
        for (int r = 0; r < 3; ++r) md.add(G, std::string("sform row ") + char('x' + r), matrixRow(affine[r]));
    }
    if (qcode > 0)
    {
        md.add(G, "Quaternion (b, c, d)", wiv_plugin::MetadataBuilder::number(f32(256)) + ", " +
                                              wiv_plugin::MetadataBuilder::number(f32(260)) + ", " +
                                              wiv_plugin::MetadataBuilder::number(f32(264)));
        md.add(G, "Quaternion offset (x, y, z)", wiv_plugin::MetadataBuilder::number(f32(268)) + ", " +
                                                    wiv_plugin::MetadataBuilder::number(f32(272)) + ", " +
                                                    wiv_plugin::MetadataBuilder::number(f32(276)));
        if (!haveAffine)
        {
            qformMatrix(f32(256), f32(260), f32(264), f32(268), f32(272), f32(276), f32(80), f32(84), f32(88), qfac, affine);
            haveAffine = true;
        }
    }
    if (haveAffine)
    {
        const std::string code = orientationCode(affine);
        if (!code.empty()) md.add(G, "Axis orientation", code + " (" + (scode > 0 ? "from sform" : "from qform") + ")");
    }

    // ---- Header extensions (after the 4-byte extender, before the data) ----
    const auto voxOffset = static_cast<long long>(f32(108));
    unsigned char ext[4];
    if (voxOffset > 352 && gzread(gz, ext, 4) == 4 && ext[0] != 0)
    {
        long long pos = 352;
        int n = 0;
        while (pos + 8 <= voxOffset && n < 32)
        {
            unsigned char eh[8];
            if (gzread(gz, eh, 8) != 8) break;
            int32_t esize = readField<int32_t>(eh, 0, swap), ecode = readField<int32_t>(eh, 4, swap);
            if (esize < 8 || esize % 16 != 0 || pos + esize > voxOffset) break;
            std::string desc = "(" + std::to_string(esize) + " bytes)";
            const char* name = ecode == 2 ? "DICOM" : ecode == 4 ? "AFNI XML" : ecode == 6 ? "comment" : ecode == 32 ? "CIfTI" : nullptr;
            const size_t payload = static_cast<size_t>(esize) - 8;
            std::vector<unsigned char> buf(payload);
            if (payload && gzread(gz, buf.data(), static_cast<unsigned>(payload)) != static_cast<int>(payload)) break;
            if (ecode == 6 || ecode == 4)
            {
                std::string text = textField(buf.data(), std::min<size_t>(payload, 2000));
                if (!text.empty()) desc += ": " + text;
            }
            md.add("NIfTI extensions", "Extension " + std::to_string(++n) + (name ? std::string(" - ") + name : ""),
                   "code " + std::to_string(ecode) + " " + desc);
            pos += esize;
        }
    }
    gzclose(gz);
    return md.release();
}
