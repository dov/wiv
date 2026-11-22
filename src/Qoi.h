// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// Qoi.h - a small, dependency-free encoder/decoder for the QOI ("Quite OK Image") format,
// written from the public specification (https://qoiformat.org/qoi-specification.pdf).
// Used by the qoi image plugin and by File > Export. Always works on 8-bit RGBA.
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace giv::qoi
{

struct Header
{
    uint32_t width = 0, height = 0;
    int channels = 4;   // 3 or 4, as stored in the file
    int colorspace = 0; // 0 = sRGB with linear alpha, 1 = all linear
};

namespace detail
{
constexpr uint8_t kOpIndex = 0x00, kOpDiff = 0x40, kOpLuma = 0x80, kOpRun = 0xc0, kOpRgb = 0xfe, kOpRgba = 0xff;
constexpr uint8_t kMask2 = 0xc0;
constexpr size_t kHeaderSize = 14;
constexpr uint8_t kEnd[8] = {0, 0, 0, 0, 0, 0, 0, 1};

inline int hash(const uint8_t* p) { return (p[0] * 3 + p[1] * 5 + p[2] * 7 + p[3] * 11) & 63; }
inline void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
} // namespace detail

// Encodes width*height RGBA pixels (4 bytes each). `channels` (3 or 4) only labels the file;
// pixels are always passed as RGBA. Empty result on bad arguments.
inline std::vector<uint8_t> encode(const uint8_t* rgba, uint32_t width, uint32_t height, int channels = 4)
{
    using namespace detail;
    std::vector<uint8_t> out;
    if (!rgba || width == 0 || height == 0 || (channels != 3 && channels != 4)) return out;
    const size_t pixelCount = size_t(width) * height;
    out.reserve(kHeaderSize + pixelCount * 2 + sizeof(kEnd));
    out.push_back('q'), out.push_back('o'), out.push_back('i'), out.push_back('f');
    put32(out, width);
    put32(out, height);
    out.push_back(uint8_t(channels));
    out.push_back(0);

    uint8_t index[64][4] = {};
    uint8_t prev[4] = {0, 0, 0, 255};
    int run = 0;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        uint8_t px[4] = {rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], channels == 4 ? rgba[i * 4 + 3] : uint8_t(255)};
        if (std::memcmp(px, prev, 4) == 0)
        {
            if (++run == 62)
            {
                out.push_back(uint8_t(kOpRun | (run - 1)));
                run = 0;
            }
        }
        else
        {
            if (run > 0)
            {
                out.push_back(uint8_t(kOpRun | (run - 1)));
                run = 0;
            }
            const int h = hash(px);
            if (std::memcmp(index[h], px, 4) == 0)
                out.push_back(uint8_t(kOpIndex | h));
            else
            {
                std::memcpy(index[h], px, 4);
                if (px[3] == prev[3])
                {
                    const int dr = int(px[0]) - prev[0], dg = int(px[1]) - prev[1], db = int(px[2]) - prev[2];
                    const int dgr = dr - dg, dgb = db - dg;
                    if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2)
                        out.push_back(uint8_t(kOpDiff | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2)));
                    else if (dgr > -9 && dgr < 8 && dg > -33 && dg < 32 && dgb > -9 && dgb < 8)
                    {
                        out.push_back(uint8_t(kOpLuma | (dg + 32)));
                        out.push_back(uint8_t((dgr + 8) << 4 | (dgb + 8)));
                    }
                    else
                    {
                        out.push_back(kOpRgb);
                        out.push_back(px[0]), out.push_back(px[1]), out.push_back(px[2]);
                    }
                }
                else
                {
                    out.push_back(kOpRgba);
                    out.push_back(px[0]), out.push_back(px[1]), out.push_back(px[2]), out.push_back(px[3]);
                }
            }
        }
        std::memcpy(prev, px, 4);
    }
    if (run > 0) out.push_back(uint8_t(kOpRun | (run - 1)));
    out.insert(out.end(), kEnd, kEnd + sizeof(kEnd));
    return out;
}

// Reads just the header; false if `data` isn't a plausible QOI file.
inline bool readHeader(const uint8_t* data, size_t size, Header& h)
{
    using namespace detail;
    if (size < kHeaderSize + sizeof(kEnd) || std::memcmp(data, "qoif", 4) != 0) return false;
    h.width = get32(data + 4);
    h.height = get32(data + 8);
    h.channels = data[12];
    h.colorspace = data[13];
    return h.width > 0 && h.height > 0 && (h.channels == 3 || h.channels == 4) && h.colorspace <= 1 &&
           uint64_t(h.width) * h.height <= (1ull << 31) / 4;
}

// Decodes to 8-bit RGBA. False (and `rgba` cleared) for a malformed or truncated stream.
inline bool decode(const uint8_t* data, size_t size, Header& h, std::vector<uint8_t>& rgba)
{
    using namespace detail;
    rgba.clear();
    if (!readHeader(data, size, h)) return false;
    const size_t pixelCount = size_t(h.width) * h.height;
    rgba.resize(pixelCount * 4);
    uint8_t index[64][4] = {};
    uint8_t px[4] = {0, 0, 0, 255};
    size_t p = kHeaderSize;
    const size_t chunksEnd = size - sizeof(kEnd);
    int run = 0;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        if (run > 0)
            --run;
        else
        {
            if (p >= chunksEnd)
            {
                rgba.clear();
                return false;
            }
            const uint8_t b1 = data[p++];
            if (b1 == kOpRgb)
            {
                if (p + 3 > chunksEnd) { rgba.clear(); return false; }
                px[0] = data[p++], px[1] = data[p++], px[2] = data[p++];
            }
            else if (b1 == kOpRgba)
            {
                if (p + 4 > chunksEnd) { rgba.clear(); return false; }
                px[0] = data[p++], px[1] = data[p++], px[2] = data[p++], px[3] = data[p++];
            }
            else if ((b1 & kMask2) == kOpIndex)
                std::memcpy(px, index[b1], 4);
            else if ((b1 & kMask2) == kOpDiff)
            {
                px[0] = uint8_t(px[0] + ((b1 >> 4) & 3) - 2);
                px[1] = uint8_t(px[1] + ((b1 >> 2) & 3) - 2);
                px[2] = uint8_t(px[2] + (b1 & 3) - 2);
            }
            else if ((b1 & kMask2) == kOpLuma)
            {
                if (p >= chunksEnd) { rgba.clear(); return false; }
                const uint8_t b2 = data[p++];
                const int vg = (b1 & 0x3f) - 32;
                px[0] = uint8_t(px[0] + vg - 8 + ((b2 >> 4) & 0x0f));
                px[1] = uint8_t(px[1] + vg);
                px[2] = uint8_t(px[2] + vg - 8 + (b2 & 0x0f));
            }
            else // kOpRun
                run = b1 & 0x3f;
            std::memcpy(index[hash(px)], px, 4);
        }
        std::memcpy(&rgba[i * 4], px, 4);
    }
    return true;
}

} // namespace giv::qoi
