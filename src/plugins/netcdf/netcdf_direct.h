// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// netcdf_direct.h - fast path for reading one big 2D netCDF-4 variable.
//
// libnetcdf/HDF5 inflate a compressed variable chunk by chunk under one global
// lock, so a 470M-sample GOES image takes seconds on one core. Here HDF5 is
// asked only where each chunk lives (H5Dchunk_iter); the compressed chunks are
// then read with pread() and inflated, converted (packed ints -> float with
// scale/offset/fill -> NaN) and row-flipped straight into the destination by
// all cores at once.
//
// Anything unusual - not HDF5, not 2D, shuffle or other filters, an undefined fill value,
// a foreign byte order - returns false and the caller uses the
// ordinary nc_get_var path, so this only ever speeds up, never changes,
// what is read.
//
#if defined(WIV_NETCDF_DIRECT_CHUNKS)

#include <hdf5.h>
#include <zlib.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

namespace ncdirect
{

enum class SrcKind
{
    I8,
    U8,
    I16,
    U16,
    F32
};

struct Request
{
    const char* path = nullptr;
    const char* varName = nullptr; // dataset name in the file's root group
    SrcKind src = SrcKind::I16;
    size_t h = 0, w = 0;
    bool toFloat = true; // else the output has the source's own type (unsigned 8/16-bit)
    bool packed = false; // apply fill/missing -> NaN and scale/offset (float output only)
    bool hasFill = false, hasMissing = false;
    float fillF = 0, missF = 0, scale = 1, offset = 0;
    bool flipRows = false;
    void* out = nullptr; // h*w elements
};

namespace detail
{

struct Chunk
{
    hsize_t y = 0, x = 0;
    haddr_t addr = 0;
    size_t size = 0;
    bool missing = true; // never written: reads as the fill value
};

struct IterState
{
    std::vector<Chunk> chunks;
    bool ok = true;
};

inline int chunkCallback(const hsize_t* offset, unsigned filterMask, haddr_t addr, hsize_t size, void* data)
{
    auto* st = static_cast<IterState*>(data);
    if (filterMask != 0)
        st->ok = false;
    st->chunks.push_back({offset[0], offset[1], addr, static_cast<size_t>(size), false});
    return H5_ITER_CONT;
}

struct Handles
{
    hid_t file = -1, dset = -1, space = -1, type = -1, dcpl = -1, fcpl = -1;
    ~Handles()
    {
        if (fcpl >= 0) H5Pclose(fcpl);
        if (dcpl >= 0) H5Pclose(dcpl);
        if (type >= 0) H5Tclose(type);
        if (space >= 0) H5Sclose(space);
        if (dset >= 0) H5Dclose(dset);
        if (file >= 0) H5Fclose(file);
    }
};

// Silences HDF5's automatic error printing while probing a file we may well decline.
struct QuietErrors
{
    H5E_auto2_t func = nullptr;
    void* data = nullptr;
    QuietErrors()
    {
        H5Eget_auto2(H5E_DEFAULT, &func, &data);
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    }
    ~QuietErrors() { H5Eset_auto2(H5E_DEFAULT, func, data); }
};

template <class Src, class Dst>
void convertChunk(const Src* in, size_t chunkW, size_t rows, size_t cols, size_t y0, size_t x0, const Request& r, Dst* out)
{
    for (size_t row = 0; row < rows; ++row)
    {
        const size_t dy = r.flipRows ? r.h - 1 - (y0 + row) : y0 + row;
        const Src* s = in + row * chunkW;
        Dst* d = out + dy * r.w + x0;
        if constexpr (std::is_same_v<Dst, float>)
        {
            if (r.packed)
            {
                for (size_t i = 0; i < cols; ++i)
                {
                    const float v = static_cast<float>(s[i]);
                    d[i] = ((r.hasFill && v == r.fillF) || (r.hasMissing && v == r.missF)) ? std::nanf("") : v * r.scale + r.offset;
                }
            }
            else
            {
                for (size_t i = 0; i < cols; ++i)
                    d[i] = static_cast<float>(s[i]);
            }
        }
        else
        {
            std::memcpy(d, s, cols * sizeof(Dst));
        }
    }
}

template <class Src, class Dst>
bool run(const Request& r, const std::vector<Chunk>& chunks, size_t chunkH, size_t chunkW, bool deflated, int fd, const Src& fillElem)
{
    const size_t chunkElems = chunkH * chunkW;
    const size_t chunkBytes = chunkElems * sizeof(Src);
    Dst* out = static_cast<Dst*>(r.out);
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};

    auto worker = [&] {
        std::vector<unsigned char> comp, raw(chunkBytes);
        z_stream zs{};
        if (deflated && inflateInit(&zs) != Z_OK)
        {
            failed = true;
            return;
        }
        for (size_t i; !failed && (i = next.fetch_add(1)) < chunks.size();)
        {
            const Chunk& c = chunks[i];
            const size_t rows = std::min<size_t>(chunkH, r.h - c.y), cols = std::min<size_t>(chunkW, r.w - c.x);
            const unsigned char* data = nullptr;
            if (c.missing)
            {
                std::fill_n(reinterpret_cast<Src*>(raw.data()), chunkElems, fillElem);
                data = raw.data();
            }
            else if (deflated)
            {
                comp.resize(c.size);
                if (pread(fd, comp.data(), c.size, static_cast<off_t>(c.addr)) != static_cast<ssize_t>(c.size))
                {
                    failed = true;
                    break;
                }
                inflateReset(&zs);
                zs.next_in = comp.data();
                zs.avail_in = static_cast<uInt>(c.size);
                zs.next_out = raw.data();
                zs.avail_out = static_cast<uInt>(chunkBytes);
                const int rc = inflate(&zs, Z_FINISH);
                if (rc != Z_STREAM_END || zs.total_out != chunkBytes)
                {
                    failed = true;
                    break;
                }
                data = raw.data();
            }
            else
            {
                if (c.size != chunkBytes || pread(fd, raw.data(), c.size, static_cast<off_t>(c.addr)) != static_cast<ssize_t>(c.size))
                {
                    failed = true;
                    break;
                }
                data = raw.data();
            }
            convertChunk(reinterpret_cast<const Src*>(data), chunkW, rows, cols, c.y, c.x, r, out);
        }
        if (deflated) inflateEnd(&zs);
    };

    const size_t threads = std::max<size_t>(1, std::min<size_t>({16, std::thread::hardware_concurrency(), chunks.size()}));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t)
        pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    return !failed;
}

inline hid_t nativeType(SrcKind k)
{
    switch (k)
    {
        case SrcKind::I8: return H5T_NATIVE_INT8;
        case SrcKind::U8: return H5T_NATIVE_UINT8;
        case SrcKind::I16: return H5T_NATIVE_INT16;
        case SrcKind::U16: return H5T_NATIVE_UINT16;
        default: return H5T_NATIVE_FLOAT;
    }
}

inline size_t srcSize(SrcKind k)
{
    switch (k)
    {
        case SrcKind::I8:
        case SrcKind::U8: return 1;
        case SrcKind::I16:
        case SrcKind::U16: return 2;
        default: return 4;
    }
}

} // namespace detail

// Reads the whole variable into r.out. False (nothing usable written) if the
// fast path doesn't apply or anything went wrong.
inline bool read(const Request& r)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    return false;
#endif
    using namespace detail;
    if (!r.toFloat && !((r.src == SrcKind::U8) || (r.src == SrcKind::U16))) return false;

    QuietErrors quiet;
    Handles hd;
    hd.file = H5Fopen(r.path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (hd.file < 0) return false;
    hd.dset = H5Dopen2(hd.file, r.varName, H5P_DEFAULT);
    if (hd.dset < 0) return false;
    hd.space = H5Dget_space(hd.dset);
    hd.type = H5Dget_type(hd.dset);
    hd.dcpl = H5Dget_create_plist(hd.dset);
    hd.fcpl = H5Fget_create_plist(hd.file);
    if (hd.space < 0 || hd.type < 0 || hd.dcpl < 0 || hd.fcpl < 0) return false;

    hsize_t dims[2];
    if (H5Sget_simple_extent_ndims(hd.space) != 2 || H5Sget_simple_extent_dims(hd.space, dims, nullptr) != 2) return false;
    if (dims[0] != r.h || dims[1] != r.w) return false;
    if (H5Tget_size(hd.type) != srcSize(r.src) || H5Tget_order(hd.type) != H5T_ORDER_LE) return false;
    if (H5Pget_layout(hd.dcpl) != H5D_CHUNKED) return false;
    hsize_t cdims[2];
    if (H5Pget_chunk(hd.dcpl, 2, cdims) != 2 || cdims[0] == 0 || cdims[1] == 0) return false;

    // Uncompressed, or exactly one filter and it is deflate.
    const int nfilters = H5Pget_nfilters(hd.dcpl);
    bool deflated = false;
    if (nfilters == 1)
    {
        unsigned flags = 0, mask = 0;
        size_t nelmts = 0;
        if (H5Pget_filter2(hd.dcpl, 0, &flags, &nelmts, nullptr, 0, nullptr, &mask) != H5Z_FILTER_DEFLATE) return false;
        deflated = true;
    }
    else if (nfilters != 0)
        return false;

    hsize_t userblock = 0;
    H5Pget_userblock(hd.fcpl, &userblock);

    // What a chunk that was never written (all fill, so HDF5 doesn't store it) reads as.
    H5D_fill_value_t fillStatus = H5D_FILL_VALUE_UNDEFINED;
    if (H5Pfill_value_defined(hd.dcpl, &fillStatus) < 0 || fillStatus == H5D_FILL_VALUE_UNDEFINED) return false;
    alignas(8) unsigned char fillBytes[8] = {};
    if (H5Pget_fill_value(hd.dcpl, nativeType(r.src), fillBytes) < 0) return false;

    IterState st;
    if (H5Dchunk_iter(hd.dset, H5P_DEFAULT, chunkCallback, &st) < 0 || !st.ok) return false;
    const size_t gridH = (r.h + cdims[0] - 1) / cdims[0], gridW = (r.w + cdims[1] - 1) / cdims[1];
    std::vector<Chunk> grid(gridH * gridW);
    for (size_t gy = 0; gy < gridH; ++gy)
        for (size_t gx = 0; gx < gridW; ++gx)
        {
            grid[gy * gridW + gx].y = gy * cdims[0];
            grid[gy * gridW + gx].x = gx * cdims[1];
        }
    for (Chunk& c : st.chunks)
    {
        if (c.y >= r.h || c.x >= r.w || c.y % cdims[0] || c.x % cdims[1]) return false;
        c.addr += userblock;
        grid[(c.y / cdims[0]) * gridW + c.x / cdims[1]] = c;
    }
    const size_t chunkH = cdims[0], chunkW = cdims[1];
    // Done with HDF5: the rest never touches its global lock.
    const int fd = open(r.path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = false;
    switch (r.src)
    {
        case SrcKind::I8:  ok = r.toFloat && run<int8_t, float>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const int8_t*>(fillBytes)); break;
        case SrcKind::U8:  ok = r.toFloat ? run<uint8_t, float>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const uint8_t*>(fillBytes))
                                          : run<uint8_t, uint8_t>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const uint8_t*>(fillBytes)); break;
        case SrcKind::I16: ok = r.toFloat && run<int16_t, float>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const int16_t*>(fillBytes)); break;
        case SrcKind::U16: ok = r.toFloat ? run<uint16_t, float>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const uint16_t*>(fillBytes))
                                          : run<uint16_t, uint16_t>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const uint16_t*>(fillBytes)); break;
        case SrcKind::F32: ok = r.toFloat && run<float, float>(r, grid, chunkH, chunkW, deflated, fd, *reinterpret_cast<const float*>(fillBytes)); break;
    }
    close(fd);
    return ok;
}

} // namespace ncdirect

#endif // WIV_NETCDF_DIRECT_CHUNKS
