// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// ora_plugin.cpp - wiv image plugin for OpenRaster (.ora) and Krita (.kra)
// files. Counterpart of giv's ~/hd/github/giv/src/plugins/ora.cc: both
// formats are zip archives holding a flattened "mergedimage.png", which is
// all that is shown (layers are not composited). Instead of libzip + libpng
// this uses a minimal zip reader on top of zlib and the vendored stb_image.
//
#include "../wiv_plugin_common.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <zlib.h>

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

uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const unsigned char* p) { return le16(p) | (static_cast<uint32_t>(le16(p + 2)) << 16); }

// Reads the whole archive into memory; ora/kra files with a merged preview
// are small compared to what the host already decodes for other formats.
bool readFile(const char* filename, std::vector<unsigned char>& out)
{
    FILE* fh = std::fopen(filename, "rb");
    if (!fh)
        return false;
    std::fseek(fh, 0, SEEK_END);
    long size = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
    bool ok = size > 0 && std::fread(out.data(), 1, out.size(), fh) == out.size();
    std::fclose(fh);
    return ok;
}

// Extracts entry `name` from the in-memory zip archive. Uses the central
// directory, so it also works for entries written with data descriptors.
bool extractZipEntry(const std::vector<unsigned char>& zip, const char* name, std::vector<unsigned char>& out)
{
    if (zip.size() < 22)
        return false;
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22, lim = zip.size() > 65557 ? zip.size() - 65557 : 0;; --i)
    {
        if (le32(&zip[i]) == 0x06054b50)
        {
            eocd = i;
            break;
        }
        if (i == lim)
            break;
    }
    if (eocd == std::string::npos)
        return false;

    const unsigned count = le16(&zip[eocd + 10]);
    size_t pos = le32(&zip[eocd + 16]);
    for (unsigned e = 0; e < count; ++e)
    {
        if (pos + 46 > zip.size() || le32(&zip[pos]) != 0x02014b50)
            return false;
        const unsigned method = le16(&zip[pos + 10]);
        const uint32_t csize = le32(&zip[pos + 20]);
        const uint32_t usize = le32(&zip[pos + 24]);
        const unsigned nameLen = le16(&zip[pos + 28]);
        const unsigned extraLen = le16(&zip[pos + 30]);
        const unsigned commentLen = le16(&zip[pos + 32]);
        const size_t localOff = le32(&zip[pos + 42]);
        if (pos + 46 + nameLen > zip.size())
            return false;
        const bool match = nameLen == std::strlen(name) && std::memcmp(&zip[pos + 46], name, nameLen) == 0;
        pos += 46 + nameLen + extraLen + commentLen;
        if (!match)
            continue;

        if (localOff + 30 > zip.size() || le32(&zip[localOff]) != 0x04034b50)
            return false;
        const size_t dataOff = localOff + 30 + le16(&zip[localOff + 26]) + le16(&zip[localOff + 28]);
        if (dataOff + csize > zip.size())
            return false;
        out.resize(usize);
        if (method == 0)
        {
            if (csize != usize)
                return false;
            std::memcpy(out.data(), &zip[dataOff], usize);
            return true;
        }
        if (method != 8)
            return false;
        z_stream zs{};
        if (inflateInit2(&zs, -15) != Z_OK)
            return false;
        zs.next_in = const_cast<Bytef*>(&zip[dataOff]);
        zs.avail_in = csize;
        zs.next_out = out.data();
        zs.avail_out = usize;
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        return rc == Z_STREAM_END;
    }
    return false;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".ora") || hasExtension(f, ".kra");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    auto fail = [&](const char* msg) -> WivPluginImage*
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError(msg, filename);
        return nullptr;
    };

    std::vector<unsigned char> zip, png;
    if (!readFile(filename, zip))
        return fail("ora: failed to read %s");
    if (!extractZipEntry(zip, "mergedimage.png", png) && !extractZipEntry(zip, "Thumbnails/thumbnail.png", png))
        return fail("ora: no mergedimage.png in %s");

    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
    if (!pixels)
        return fail("ora: failed to decode merged image in %s");

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    std::memcpy(img->rgba, pixels, static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return img;
}
