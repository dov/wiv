// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// netcdf_plugin.cpp - wiv image plugin for netCDF files (.nc .nc4 .cdf .netcdf),
// via libnetcdf. Every numeric variable with 2 to 4 dimensions is one "page" -
// PgUp/PgDn steps between them - and is shown as grayscale data:
//
//   2D (y,x)        a single image;
//   3D (t,y,x)      a stack of slices (time / level / band);
//   4D (t,z,y,x)    the leading axes are flattened into slices.
//
// The last two dimensions are always (rows, columns). Packed data is
// unpacked: _FillValue / missing_value samples become NaN and scale_factor /
// add_offset are applied (such variables are shown as float). Unsigned 8 / 16
// bit variables without those attributes keep their raw samples; everything
// else is converted to float. When the row axis has an ascending latitude (or
// projection-y) coordinate the rows are flipped so north is up.
//
// netCDF-4 files are HDF5 files: they are only claimed by extension here, and
// content sniffing recognizes just the classic "CDF" signature, so a
// misnamed HDF5 file still goes to the hdf5 plugin.
//
#include "../wiv_plugin_common.h"

#include <netcdf.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace
{

// libnetcdf (and the HDF5 under it) is not reliably thread-safe; the host
// decodes on several worker threads, so serialize every call into it.
std::mutex g_ncMutex;

// Per-thread breakdown of the last load, for wiv_plugin_last_timing().
thread_local std::string t_timing;

struct PhaseTimer
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point last = Clock::now();
    // Appends "name X ms" for the time since the previous mark.
    void mark(const char* name)
    {
        const auto now = Clock::now();
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%s %.1f", t_timing.empty() ? "" : ", ", name,
                      std::chrono::duration<double, std::milli>(now - last).count());
        t_timing += buf;
        last = now;
    }
};

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    return std::equal(filename.end() - n, filename.end(), ext, [](char a, char b)
                       { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

struct Candidate
{
    int varid = -1;
    std::string name;
    int ndims = 0;
    int dimids[4] = {0, 0, 0, 0};
    size_t dims[4] = {1, 1, 1, 1};
    nc_type type = NC_NAT;
};

struct NcFile
{
    int ncid = -1;
    std::vector<Candidate> candidates;
    ~NcFile()
    {
        if (ncid >= 0)
            nc_close(ncid);
    }
};

bool isNumeric(nc_type t)
{
    return t >= NC_BYTE && t <= NC_UINT64 && t != NC_CHAR;
}

bool readDoubleAttr(int ncid, int varid, const char* name, double& out)
{
    nc_type t;
    size_t len;
    if (nc_inq_att(ncid, varid, name, &t, &len) != NC_NOERR || len != 1 || !isNumeric(t))
        return false;
    return nc_get_att_double(ncid, varid, name, &out) == NC_NOERR;
}

std::string readTextAttr(int ncid, int varid, const char* name)
{
    nc_type t;
    size_t len;
    if (nc_inq_att(ncid, varid, name, &t, &len) != NC_NOERR || t != NC_CHAR || len == 0 || len > 256)
        return {};
    std::string s(len, '\0');
    if (nc_get_att_text(ncid, varid, name, &s[0]) != NC_NOERR)
        return {};
    while (!s.empty() && s.back() == '\0')
        s.pop_back();
    return s;
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

void collect(int ncid, std::vector<Candidate>& out)
{
    int nvars = 0;
    if (nc_inq_nvars(ncid, &nvars) != NC_NOERR)
        return;
    for (int v = 0; v < nvars && out.size() < 4096; ++v)
    {
        char name[NC_MAX_NAME + 1];
        nc_type type;
        int ndims, dimids[NC_MAX_VAR_DIMS], natts;
        if (nc_inq_var(ncid, v, name, &type, &ndims, dimids, &natts) != NC_NOERR)
            continue;
        if (ndims < 2 || ndims > 4 || !isNumeric(type))
            continue;
        Candidate c;
        c.varid = v;
        c.name = name;
        c.ndims = ndims;
        c.type = type;
        bool ok = true;
        for (int i = 0; i < ndims; ++i)
        {
            size_t len = 0;
            c.dimids[i] = dimids[i];
            if (nc_inq_dimlen(ncid, dimids[i], &len) != NC_NOERR || len == 0 || len > 0x7fffffff)
                ok = false;
            c.dims[i] = len;
        }
        if (ok)
            out.push_back(c);
    }
}

bool openFile(const char* filename, NcFile& nf)
{
    if (nc_open(wiv_plugin::asciiSafePath(filename).c_str(), NC_NOWRITE, &nf.ncid) != NC_NOERR)
    {
        nf.ncid = -1;
        return false;
    }
    collect(nf.ncid, nf.candidates);
    return true;
}

// Does the row axis run south-to-north (ascending latitude), which would show
// the image upside down?
bool rowsAscendNorthward(int ncid, const Candidate& c)
{
    const int rowDim = c.dimids[c.ndims - 2];
    char dimName[NC_MAX_NAME + 1];
    if (nc_inq_dimname(ncid, rowDim, dimName) != NC_NOERR)
        return false;
    int coord;
    if (nc_inq_varid(ncid, dimName, &coord) != NC_NOERR)
        return false;
    int nd;
    nc_inq_varndims(ncid, coord, &nd);
    if (nd != 1)
        return false;
    const std::string units = readTextAttr(ncid, coord, "units");
    const std::string stdName = readTextAttr(ncid, coord, "standard_name");
    const std::string axis = readTextAttr(ncid, coord, "axis");
    const bool northish = startsWith(units, "degrees_north") || startsWith(units, "degree_north") ||
                          startsWith(units, "degrees_N") || startsWith(units, "degree_N") ||
                          stdName == "latitude" || stdName == "projection_y_coordinate" || axis == "Y";
    if (!northish)
        return false;
    const size_t n = c.dims[c.ndims - 2];
    if (n < 2)
        return false;
    double first, last;
    size_t i0 = 0, i1 = n - 1;
    if (nc_get_var1_double(ncid, coord, &i0, &first) != NC_NOERR ||
        nc_get_var1_double(ncid, coord, &i1, &last) != NC_NOERR)
        return false;
    return last > first;
}

void minMax(const float* p, size_t n, float& lo, float& hi)
{
    lo = 1e30f;
    hi = -1e30f;
    for (size_t i = 0; i < n; ++i)
    {
        const float v = p[i];
        if (!std::isfinite(v))
            continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (lo > hi)
        lo = hi = 0.f;
    if (hi == lo)
        hi = lo + 1.f;
}

template <typename T>
void minMaxInt(const T* p, size_t n, float& lo, float& hi)
{
    T l = p[0], h = p[0];
    for (size_t i = 1; i < n; ++i)
    {
        l = std::min(l, p[i]);
        h = std::max(h, p[i]);
    }
    lo = float(l);
    hi = h == l ? lo + 1.f : float(h);
}

// Reverses the row order of every (h x w) slice of `data`.
template <typename T>
void flipRows(T* data, size_t w, size_t h, size_t depth)
{
    for (size_t d = 0; d < depth; ++d)
    {
        T* slice = data + d * w * h;
        for (size_t y = 0; y < h / 2; ++y)
            std::swap_ranges(slice + y * w, slice + (y + 1) * w, slice + (h - 1 - y) * w);
    }
}

template <typename T>
void fillPreview(WivPluginImage* img, const T* p, float lo, float hi)
{
    const size_t plane = size_t(img->width) * img->height;
    for (size_t i = 0; i < plane; ++i)
    {
        const float v = static_cast<float>(p[i]);
        unsigned char g;
        if (!(v > lo))
            g = 0;
        else if (!(v < hi))
            g = 255;
        else
            g = static_cast<unsigned char>((v - lo) / (hi - lo) * 255.f + 0.5f);
        img->rgba[i * 4 + 0] = img->rgba[i * 4 + 1] = img->rgba[i * 4 + 2] = g;
        img->rgba[i * 4 + 3] = 255;
    }
}

WivPluginImage* fail(WivPluginImage* img, char** error_msg, const char* fmt, const std::string& what)
{
    if (img)
        wiv_plugin_free_image(img);
    if (error_msg)
        *error_msg = wiv_plugin::makeError(fmt, what.c_str());
    return nullptr;
}

// Planes above this get no RGBA8 preview (see WivPluginImage::rgba).
constexpr size_t kPreviewMaxPixels = size_t(4) << 20;

WivPluginImage* loadVariable(int ncid, const Candidate& c, char** error_msg)
{
    PhaseTimer timer;
    t_timing.clear();
    const int w = int(c.dims[c.ndims - 1]);
    const int h = int(c.dims[c.ndims - 2]);
    size_t depthSz = 1;
    for (int i = 0; i < c.ndims - 2; ++i)
        depthSz *= c.dims[i];
    const size_t plane = size_t(w) * h;
    if (depthSz > 0x7fffffff || plane * depthSz > (size_t(1) << 31))
        return fail(nullptr, error_msg, "netcdf: variable %s is too large", c.name);
    const int depth = int(depthSz);
    const size_t total = plane * depthSz;

    double fillValue = 0, missingValue = 0, scale = 1, offset = 0;
    const bool hasFill = readDoubleAttr(ncid, c.varid, "_FillValue", fillValue);
    const bool hasMissing = readDoubleAttr(ncid, c.varid, "missing_value", missingValue);
    const bool hasScale = readDoubleAttr(ncid, c.varid, "scale_factor", scale);
    const bool hasOffset = readDoubleAttr(ncid, c.varid, "add_offset", offset);
    const bool packed = hasFill || hasMissing || hasScale || hasOffset;

    WivSampleType st = WIV_SAMPLE_FLOAT;
    if (!packed && c.type == NC_UBYTE)
        st = WIV_SAMPLE_U8;
    else if (!packed && c.type == NC_USHORT)
        st = WIV_SAMPLE_U16;

    timer.mark("attrs");
    // The host displays from the samples; an RGBA8 preview of a big variable is just wasted time and memory.
    WivPluginImage* img = wiv_plugin::allocImage(w, h, plane <= kPreviewMaxPixels);
    wiv_plugin::allocSamples(img, st, depth);
    timer.mark("alloc");
    if (!img->samples)
        return fail(img, error_msg, "netcdf: out of memory reading %s", c.name);

    int rc;
    if (st == WIV_SAMPLE_U8)
        rc = nc_get_var_uchar(ncid, c.varid, static_cast<unsigned char*>(img->samples));
    else if (st == WIV_SAMPLE_U16)
        rc = nc_get_var_ushort(ncid, c.varid, static_cast<unsigned short*>(img->samples));
    else
        rc = nc_get_var_float(ncid, c.varid, static_cast<float*>(img->samples));
    timer.mark("nc_get_var");
    // NC_ERANGE: some values didn't fit the conversion; the rest were still read.
    if (rc != NC_NOERR && rc != NC_ERANGE)
        return fail(img, error_msg, "netcdf: failed to read variable %s", c.name);

    if (st == WIV_SAMPLE_FLOAT)
    {
        float* f = static_cast<float*>(img->samples);
        if (packed)
        {
            const float fillF = float(fillValue), missF = float(missingValue);
            const float sc = float(scale), off = float(offset);
            for (size_t i = 0; i < total; ++i)
            {
                const float v = f[i];
                if ((hasFill && v == fillF) || (hasMissing && v == missF))
                    f[i] = std::nanf("");
                else
                    f[i] = v * sc + off;
            }
        }
    }

    timer.mark("scale/offset/nan");
    if (rowsAscendNorthward(ncid, c))
    {
        if (st == WIV_SAMPLE_U8)
            flipRows(static_cast<uint8_t*>(img->samples), w, h, depth);
        else if (st == WIV_SAMPLE_U16)
            flipRows(static_cast<uint16_t*>(img->samples), w, h, depth);
        else
            flipRows(static_cast<float*>(img->samples), w, h, depth);
    }

    timer.mark("flip");
    // Slice-0 preview, scaled to its own range (the host redraws from the samples).
    if (!img->rgba)
    {
        timer.mark("preview (skipped)");
        return img;
    }
    float lo, hi;
    if (st == WIV_SAMPLE_U8)
    {
        const auto* p = static_cast<uint8_t*>(img->samples);
        minMaxInt(p, plane, lo, hi);
        fillPreview(img, p, lo, hi);
    }
    else if (st == WIV_SAMPLE_U16)
    {
        const auto* p = static_cast<uint16_t*>(img->samples);
        minMaxInt(p, plane, lo, hi);
        fillPreview(img, p, lo, hi);
    }
    else
    {
        const auto* p = static_cast<float*>(img->samples);
        minMax(p, plane, lo, hi);
        fillPreview(img, p, lo, hi);
    }
    timer.mark("preview");
    return img;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    const std::string f(filename);
    return hasExtension(f, ".nc") || hasExtension(f, ".nc4") || hasExtension(f, ".cdf") ||
           hasExtension(f, ".netcdf");
}

// Only the classic / 64-bit-offset / CDF-5 formats ("CDF" + version byte);
// netCDF-4 files carry the HDF5 signature and are left to the hdf5 plugin.
extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return false;
    unsigned char buf[4];
    const bool got = std::fread(buf, 1, 4, fh) == 4;
    std::fclose(fh);
    return got && buf[0] == 'C' && buf[1] == 'D' && buf[2] == 'F' && (buf[3] == 1 || buf[3] == 2 || buf[3] == 5);
}

extern "C" int wiv_plugin_page_count(const char* filename)
{
    std::lock_guard<std::mutex> lock(g_ncMutex);
    NcFile nf;
    if (!openFile(filename, nf))
        return 0;
    return static_cast<int>(nf.candidates.size());
}

extern "C" WivPluginImage* wiv_plugin_load_page(const char* filename, int page, char** error_msg)
{
    std::lock_guard<std::mutex> lock(g_ncMutex);
    NcFile nf;
    if (!openFile(filename, nf))
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("netcdf: cannot open %s", filename);
        return nullptr;
    }
    if (page < 0 || page >= static_cast<int>(nf.candidates.size()))
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("netcdf: no 2D-4D numeric variable in %s", filename);
        return nullptr;
    }
    return loadVariable(nf.ncid, nf.candidates[page], error_msg);
}

extern "C" const char* wiv_plugin_last_timing(void)
{
    return t_timing.empty() ? nullptr : t_timing.c_str();
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    return wiv_plugin_load_page(filename, 0, error_msg);
}
