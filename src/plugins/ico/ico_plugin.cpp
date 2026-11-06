// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// ico_plugin.cpp - wiv image plugin for Windows icons and cursors (.ico,
// .cur). Shows the largest image in the file (ties go to the higher bit
// depth). Entries are either an embedded PNG (decoded with the vendored
// stb_image) or a classic DIB with an AND mask, which is decoded here:
// 1/4/8-bit palettised, 16 (555), 24 and 32-bit BI_RGB.
//
#include "../wiv_plugin_common.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

uint32_t rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
uint32_t rd32(const unsigned char* p) { return rd16(p) | (rd16(p + 2) << 16); }

// Decodes one DIB-format entry (BITMAPINFOHEADER + palette + XOR bitmap + AND
// mask) into freshly allocated RGBA, or returns nullptr.
WivPluginImage* decodeDib(const unsigned char* d, size_t size)
{
    if (size < 40 || rd32(d) < 40)
        return nullptr;
    const uint32_t hdrSize = rd32(d);
    const int width = static_cast<int>(rd32(d + 4));
    const int height = static_cast<int>(rd32(d + 8)) / 2; // XOR + AND stacked
    const int bpp = static_cast<int>(rd16(d + 14));
    const uint32_t compression = rd32(d + 16);
    uint32_t colors = rd32(d + 32);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || compression != 0)
        return nullptr;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return nullptr;
    if (bpp <= 8 && (colors == 0 || colors > (1u << bpp)))
        colors = 1u << bpp;
    else if (bpp > 8)
        colors = 0;

    const unsigned char* palette = d + hdrSize;
    const size_t paletteBytes = size_t(colors) * 4;
    const size_t xorStride = ((size_t(width) * bpp + 31) / 32) * 4;
    const size_t andStride = ((size_t(width) + 31) / 32) * 4;
    const unsigned char* xorBits = palette + paletteBytes;
    const unsigned char* andBits = xorBits + xorStride * height;
    if (size_t(xorBits - d) + xorStride * height > size)
        return nullptr;
    const bool haveMask = size_t(andBits - d) + andStride * height <= size;

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    bool anyAlpha = false;
    for (int y = 0; y < height; ++y)
    {
        const unsigned char* row = xorBits + xorStride * (height - 1 - y); // bottom-up
        unsigned char* o = img->rgba + size_t(y) * width * 4;
        for (int x = 0; x < width; ++x, o += 4)
        {
            switch (bpp)
            {
                case 32: o[0] = row[x * 4 + 2]; o[1] = row[x * 4 + 1]; o[2] = row[x * 4]; o[3] = row[x * 4 + 3];
                         anyAlpha |= o[3] != 0; break;
                case 24: o[0] = row[x * 3 + 2]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3]; o[3] = 255; break;
                case 16:
                {
                    uint32_t v = rd16(row + x * 2);
                    o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31;
                    o[3] = 255;
                    break;
                }
                default:
                {
                    unsigned idx = bpp == 8   ? row[x]
                                   : bpp == 4 ? (row[x / 2] >> ((x & 1) ? 0 : 4)) & 15
                                              : (row[x / 8] >> (7 - (x & 7))) & 1;
                    if (idx >= colors) idx = 0;
                    o[0] = palette[idx * 4 + 2]; o[1] = palette[idx * 4 + 1]; o[2] = palette[idx * 4]; o[3] = 255;
                }
            }
        }
    }
    // 32-bit icons normally carry their own alpha; everything else (and
    // 32-bit with an all-zero alpha channel) is masked by the AND bitmap.
    if (haveMask && !(bpp == 32 && anyAlpha))
    {
        for (int y = 0; y < height; ++y)
        {
            const unsigned char* m = andBits + andStride * (height - 1 - y);
            unsigned char* o = img->rgba + size_t(y) * width * 4;
            for (int x = 0; x < width; ++x)
                o[x * 4 + 3] = ((m[x / 8] >> (7 - (x & 7))) & 1) ? 0 : 255;
        }
    }
    else if (bpp == 32 && !anyAlpha)
    {
        for (size_t i = 0, n = size_t(width) * height; i < n; ++i)
            img->rgba[i * 4 + 3] = 255;
    }
    return img;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".ico") || hasExtension(f, ".cur");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    auto fail = [&](const char* fmt) -> WivPluginImage*
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(fmt, filename);
        return nullptr;
    };

    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh)
        return fail("ico: failed to open %s");
    std::fseek(fh, 0, SEEK_END);
    long fsize = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    if (fsize < 22)
    {
        std::fclose(fh);
        return fail("ico: file too small: %s");
    }
    std::vector<unsigned char> data(static_cast<size_t>(fsize));
    size_t nread = std::fread(data.data(), 1, data.size(), fh);
    std::fclose(fh);
    if (nread != data.size())
        return fail("ico: failed to read %s");

    const unsigned char* h = data.data();
    const unsigned type = rd16(h + 2), count = rd16(h + 4);
    if (rd16(h) != 0 || (type != 1 && type != 2) || count == 0 || 6 + size_t(count) * 16 > data.size())
        return fail("ico: not a valid icon file: %s");

    // Rank the entries (largest first, then deepest); fall through to the
    // next one if the best fails to decode.
    struct Entry { int area, bpp; size_t off, len; };
    std::vector<Entry> entries;
    for (unsigned i = 0; i < count; ++i)
    {
        const unsigned char* e = h + 6 + i * 16;
        int w = e[0] ? e[0] : 256, hh = e[1] ? e[1] : 256;
        size_t len = rd32(e + 8), off = rd32(e + 12);
        if (off >= data.size() || len > data.size() - off)
            continue;
        entries.push_back({w * hh, int(rd16(e + 6)), off, len});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
                     { return a.area != b.area ? a.area > b.area : a.bpp > b.bpp; });

    for (const Entry& e : entries)
    {
        const unsigned char* p = h + e.off;
        if (e.len >= 8 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0)
        {
            int w = 0, hh = 0, ch = 0;
            unsigned char* px = stbi_load_from_memory(p, static_cast<int>(e.len), &w, &hh, &ch, 4);
            if (!px)
                continue;
            WivPluginImage* img = wiv_plugin::allocImage(w, hh);
            std::memcpy(img->rgba, px, size_t(w) * hh * 4);
            stbi_image_free(px);
            return img;
        }
        if (WivPluginImage* img = decodeDib(p, e.len))
            return img;
    }
    return fail("ico: no decodable image in %s");
}
