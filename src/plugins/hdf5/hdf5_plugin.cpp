// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// hdf5_plugin.cpp - wiv image plugin for HDF5 files (.h5 .hdf5 .he5), via
// libhdf5. Every numeric 2D or 3D dataset in the file (found by walking the
// groups) is one "page" - PgUp/PgDn steps between them - and is shown as:
//
//   2D (H,W)      a grayscale image;
//   3D            either a colour image or a stack of grayscale slices,
//                 decided heuristically (see classify3D()):
//                   * an HDF5 Image-spec dataset (CLASS="IMAGE") says so via
//                     INTERLACE_MODE;
//                   * otherwise a last axis of 3 or 4 with both other axes
//                     larger = colour, channels last (H,W,C); a first axis
//                     of 3 or 4 with both others larger = colour, channels
//                     first (C,H,W);
//                   * anything else = slices (D,H,W), like npy/FITS/DICOM.
//
// Grayscale data keeps raw samples (u8 / u16 / float - other types are
// converted to float) for the contrast and colour-table tools. Colour data
// is 8-bit as is, or mapped to 8 bits from its value range (float data that
// already lies in [0,1] is taken at face value).
//
#include "../wiv_plugin_common.h"

#include <hdf5.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{

// libhdf5 is only thread-safe if built that way; the host decodes on several
// worker threads, so serialize every call into it.
std::mutex g_h5Mutex;

bool hasExtension(const std::string& filename, const char* ext)
{
    size_t n = std::strlen(ext);
    if (filename.size() < n)
        return false;
    return std::equal(filename.end() - n, filename.end(), ext, [](char a, char b)
                       { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

struct Handle
{
    hid_t id;
    herr_t (*close)(hid_t);
    explicit Handle(hid_t i = -1, herr_t (*c)(hid_t) = nullptr) : id(i), close(c) {}
    ~Handle()
    {
        if (id >= 0 && close)
            close(id);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool ok() const { return id >= 0; }
    void reset(hid_t i, herr_t (*c)(hid_t))
    {
        if (id >= 0 && close)
            close(id);
        id = i;
        close = c;
    }
};

struct Candidate
{
    std::string path;
    int rank = 0;
    hsize_t dims[3] = {1, 1, 1};
    bool isImageSpec = false;
    bool planar = false; // Image spec: INTERLACE_PLANE (C,H,W)
};

// A string attribute of `obj`, or "" (fixed-length strings only, which is
// what the Image spec uses).
std::string readStringAttr(hid_t obj, const char* name)
{
    if (H5Aexists(obj, name) <= 0)
        return {};
    Handle attr(H5Aopen(obj, name, H5P_DEFAULT), H5Aclose);
    if (!attr.ok())
        return {};
    Handle type(H5Aget_type(attr.id), H5Tclose);
    if (!type.ok() || H5Tget_class(type.id) != H5T_STRING || H5Tis_variable_str(type.id) > 0)
        return {};
    std::vector<char> buf(H5Tget_size(type.id) + 1, '\0');
    if (H5Aread(attr.id, type.id, buf.data()) < 0)
        return {};
    return std::string(buf.data());
}

bool inspectDataset(hid_t loc, const std::string& path, Candidate& out)
{
    Handle ds(H5Dopen2(loc, path.c_str(), H5P_DEFAULT), H5Dclose);
    if (!ds.ok())
        return false;
    Handle type(H5Dget_type(ds.id), H5Tclose);
    if (!type.ok())
        return false;
    const H5T_class_t cls = H5Tget_class(type.id);
    if (cls != H5T_INTEGER && cls != H5T_FLOAT)
        return false;
    Handle space(H5Dget_space(ds.id), H5Sclose);
    if (!space.ok() || H5Sget_simple_extent_type(space.id) != H5S_SIMPLE)
        return false;
    const int rank = H5Sget_simple_extent_ndims(space.id);
    if (rank != 2 && rank != 3)
        return false;
    hsize_t dims[3] = {1, 1, 1};
    hsize_t d[3];
    H5Sget_simple_extent_dims(space.id, d, nullptr);
    for (int i = 0; i < rank; ++i)
    {
        if (d[i] == 0 || d[i] > 0x7fffffff)
            return false;
        dims[i] = d[i];
    }
    out.path = path;
    out.rank = rank;
    std::copy(dims, dims + 3, out.dims);
    if (rank == 3 && readStringAttr(ds.id, "CLASS") == "IMAGE")
    {
        const std::string il = readStringAttr(ds.id, "INTERLACE_MODE");
        if (il == "INTERLACE_PIXEL" || il == "INTERLACE_PLANE")
        {
            out.isImageSpec = true;
            out.planar = il == "INTERLACE_PLANE";
        }
    }
    return true;
}

// Depth-first walk collecting every usable dataset, in file order.
void collect(hid_t file, const std::string& groupPath, int depth, std::vector<Candidate>& out)
{
    if (depth > 12 || out.size() > 4096)
        return;
    Handle group(H5Gopen2(file, groupPath.c_str(), H5P_DEFAULT), H5Gclose);
    if (!group.ok())
        return;
    H5G_info_t info;
    if (H5Gget_info(group.id, &info) < 0)
        return;
    for (hsize_t i = 0; i < info.nlinks; ++i)
    {
        const ssize_t n = H5Lget_name_by_idx(group.id, ".", H5_INDEX_NAME, H5_ITER_INC, i, nullptr, 0, H5P_DEFAULT);
        if (n <= 0)
            continue;
        std::string name(static_cast<size_t>(n), '\0');
        H5Lget_name_by_idx(group.id, ".", H5_INDEX_NAME, H5_ITER_INC, i, &name[0], name.size() + 1, H5P_DEFAULT);
        const std::string path = (groupPath == "/" ? "" : groupPath) + "/" + name;
        // Opening follows soft links; a dangling one just fails and is skipped.
        Handle obj(H5Oopen(file, path.c_str(), H5P_DEFAULT), H5Oclose);
        if (!obj.ok())
            continue;
        const H5I_type_t t = H5Iget_type(obj.id);
        if (t == H5I_GROUP)
            collect(file, path, depth + 1, out);
        else if (t == H5I_DATASET)
        {
            Candidate c;
            if (inspectDataset(file, path, c))
                out.push_back(c);
        }
    }
}

enum class Layout { Gray, Slices, ColorLast, ColorFirst };

Layout classify(const Candidate& c)
{
    if (c.rank == 2)
        return Layout::Gray;
    if (c.isImageSpec)
        return c.planar ? Layout::ColorFirst : Layout::ColorLast;
    const hsize_t a = c.dims[0], b = c.dims[1], cc = c.dims[2];
    if ((cc == 3 || cc == 4) && a > 4 && b > 4)
        return Layout::ColorLast;
    if ((a == 3 || a == 4) && b > 4 && cc > 4)
        return Layout::ColorFirst;
    return Layout::Slices;
}

struct H5OpenFile
{
    Handle file;
    std::vector<Candidate> candidates;
};

bool openFile(const char* filename, H5OpenFile& of)
{
    static std::once_flag once;
    std::call_once(once, [] { H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr); });
    of.file.reset(H5Fopen(wiv_plugin::asciiSafePath(filename).c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
    if (!of.file.ok())
        return false;
    collect(of.file.id, "/", 0, of.candidates);
    return true;
}

// Reads a whole dataset into a malloc'd-size vector of T, letting HDF5
// convert from the stored type.
template <typename T>
bool readAll(hid_t file, const std::string& path, hid_t memType, size_t count, std::vector<T>& out)
{
    Handle ds(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
    if (!ds.ok())
        return false;
    out.resize(count);
    return H5Dread(ds.id, memType, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) >= 0;
}

// How the stored type maps onto wiv's raw sample types.
WivSampleType sampleTypeFor(hid_t file, const std::string& path)
{
    Handle ds(H5Dopen2(file, path.c_str(), H5P_DEFAULT), H5Dclose);
    Handle type(H5Dget_type(ds.id), H5Tclose);
    if (H5Tget_class(type.id) == H5T_INTEGER && H5Tget_sign(type.id) == H5T_SGN_NONE)
    {
        const size_t sz = H5Tget_size(type.id);
        if (sz == 1)
            return WIV_SAMPLE_U8;
        if (sz == 2)
            return WIV_SAMPLE_U16;
    }
    return WIV_SAMPLE_FLOAT;
}

unsigned char to8(float v, float lo, float hi)
{
    if (!(v > lo))
        return 0;
    if (!(v < hi))
        return 255;
    return static_cast<unsigned char>((v - lo) / (hi - lo) * 255.f + 0.5f);
}

template <typename T>
void minMax(const T* p, size_t n, float& lo, float& hi)
{
    lo = 1e30f;
    hi = -1e30f;
    for (size_t i = 0; i < n; ++i)
    {
        const float v = static_cast<float>(p[i]);
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

WivPluginImage* loadDataset(hid_t file, const Candidate& c, char** error_msg)
{
    const Layout layout = classify(c);
    const bool color = layout == Layout::ColorLast || layout == Layout::ColorFirst;
    int w, h, depth = 1, channels = 1;
    switch (layout)
    {
    case Layout::Gray: h = int(c.dims[0]); w = int(c.dims[1]); break;
    case Layout::Slices: depth = int(c.dims[0]); h = int(c.dims[1]); w = int(c.dims[2]); break;
    case Layout::ColorLast: h = int(c.dims[0]); w = int(c.dims[1]); channels = int(c.dims[2]); break;
    default: channels = int(c.dims[0]); h = int(c.dims[1]); w = int(c.dims[2]); break;
    }
    if (color && channels != 3 && channels != 4 && channels != 2)
    {
        // Image-spec data with an odd channel count: show it as slices instead.
        if (error_msg)
            *error_msg = wiv_plugin::makeError("hdf5: unsupported channel count in %s", c.path.c_str());
        return nullptr;
    }
    const size_t plane = size_t(w) * h;
    const size_t total = plane * depth * channels;
    const WivSampleType st = sampleTypeFor(file, c.path);

    WivPluginImage* img = wiv_plugin::allocImage(w, h);

    if (!color)
    {
        wiv_plugin::allocSamples(img, st, depth);
        bool ok;
        if (st == WIV_SAMPLE_U8)
        {
            std::vector<uint8_t> tmp;
            ok = readAll(file, c.path, H5T_NATIVE_UINT8, total, tmp);
            if (ok) std::memcpy(img->samples, tmp.data(), total);
        }
        else if (st == WIV_SAMPLE_U16)
        {
            std::vector<uint16_t> tmp;
            ok = readAll(file, c.path, H5T_NATIVE_UINT16, total, tmp);
            if (ok) std::memcpy(img->samples, tmp.data(), total * 2);
        }
        else
        {
            std::vector<float> tmp;
            ok = readAll(file, c.path, H5T_NATIVE_FLOAT, total, tmp);
            if (ok) std::memcpy(img->samples, tmp.data(), total * 4);
        }
        if (!ok)
        {
            wiv_plugin_free_image(img);
            if (error_msg)
                *error_msg = wiv_plugin::makeError("hdf5: failed to read dataset %s", c.path.c_str());
            return nullptr;
        }
        // Slice-0 preview, scaled to its own range (the host redraws from the samples).
        auto preview = [&](auto* p)
        {
            float lo, hi;
            minMax(p, plane, lo, hi);
            for (size_t i = 0; i < plane; ++i)
            {
                const unsigned char g = to8(static_cast<float>(p[i]), lo, hi);
                img->rgba[i * 4 + 0] = img->rgba[i * 4 + 1] = img->rgba[i * 4 + 2] = g;
                img->rgba[i * 4 + 3] = 255;
            }
        };
        if (st == WIV_SAMPLE_U8) preview(static_cast<uint8_t*>(img->samples));
        else if (st == WIV_SAMPLE_U16) preview(static_cast<uint16_t*>(img->samples));
        else preview(static_cast<float*>(img->samples));
        return img;
    }

    // Colour: gather into interleaved RGBA8.
    std::vector<uint8_t> u8;
    std::vector<float> f32;
    float lo = 0.f, hi = 255.f;
    bool ok;
    if (st == WIV_SAMPLE_U8)
        ok = readAll(file, c.path, H5T_NATIVE_UINT8, total, u8);
    else
    {
        ok = readAll(file, c.path, H5T_NATIVE_FLOAT, total, f32);
        if (ok)
        {
            minMax(f32.data(), total, lo, hi);
            if (lo >= 0.f && hi <= 1.f) { lo = 0.f; hi = 1.f; } // already normalized
        }
    }
    if (!ok)
    {
        wiv_plugin_free_image(img);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("hdf5: failed to read dataset %s", c.path.c_str());
        return nullptr;
    }
    const bool last = layout == Layout::ColorLast;
    for (size_t i = 0; i < plane; ++i)
    {
        unsigned char px[4] = {0, 0, 0, 255};
        for (int ch = 0; ch < channels; ++ch)
        {
            const size_t idx = last ? i * channels + ch : ch * plane + i;
            const unsigned char v = st == WIV_SAMPLE_U8 ? u8[idx] : to8(f32[idx], lo, hi);
            if (channels == 2) // gray + alpha
                px[ch == 0 ? 0 : 3] = v;
            else
                px[ch] = v;
        }
        if (channels == 2)
            px[1] = px[2] = px[0];
        std::memcpy(img->rgba + i * 4, px, 4);
    }
    return img;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    const std::string f(filename);
    return hasExtension(f, ".h5") || hasExtension(f, ".hdf5") || hasExtension(f, ".he5");
}

extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return false;
    // The signature sits at 0, or at 512, 1024, 2048, ... when there is a user block.
    static const unsigned char sig[8] = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};
    bool found = false;
    for (long off = 0; off <= 8192 && !found; off = off ? off * 2 : 512)
    {
        unsigned char buf[8];
        if (std::fseek(fh, off, SEEK_SET) != 0 || std::fread(buf, 1, 8, fh) != 8)
            break;
        found = std::memcmp(buf, sig, 8) == 0;
    }
    std::fclose(fh);
    return found;
}

extern "C" int wiv_plugin_page_count(const char* filename)
{
    std::lock_guard<std::mutex> lock(g_h5Mutex);
    H5OpenFile of;
    if (!openFile(filename, of))
        return 0;
    return static_cast<int>(of.candidates.size());
}

extern "C" WivPluginImage* wiv_plugin_load_page(const char* filename, int page, char** error_msg)
{
    std::lock_guard<std::mutex> lock(g_h5Mutex);
    H5OpenFile of;
    if (!openFile(filename, of))
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("hdf5: cannot open %s", filename);
        return nullptr;
    }
    if (page < 0 || page >= static_cast<int>(of.candidates.size()))
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("hdf5: no 2D or 3D numeric dataset in %s", filename);
        return nullptr;
    }
    return loadDataset(of.file.id, of.candidates[page], error_msg);
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    return wiv_plugin_load_page(filename, 0, error_msg);
}
