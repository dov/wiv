// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
//
// qoi_plugin.cpp - wiv image plugin for QOI ("Quite OK Image") files. The codec is the
// dependency-free src/Qoi.h.
//
#include "../wiv_plugin_common.h"

#include "Qoi.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

bool hasQoiExtension(const char* filename)
{
    const size_t n = std::strlen(filename);
    if (n < 4) return false;
    std::string tail(filename + n - 4);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".qoi";
}

bool readFile(const char* filename, std::vector<uint8_t>& data)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh) return false;
    std::fseek(fh, 0, SEEK_END);
    const long size = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    bool ok = size > 0;
    if (ok)
    {
        data.resize(static_cast<size_t>(size));
        ok = std::fread(data.data(), 1, data.size(), fh) == data.size();
    }
    std::fclose(fh);
    return ok;
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasQoiExtension(filename);
}

extern "C" bool wiv_plugin_sniff_file(const char* filename)
{
    FILE* fh = wiv_plugin::fopenUtf8(filename, "rb");
    if (!fh) return false;
    char magic[4] = {};
    const bool ok = std::fread(magic, 1, 4, fh) == 4 && std::memcmp(magic, "qoif", 4) == 0;
    std::fclose(fh);
    return ok;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    std::vector<uint8_t> data;
    if (!readFile(filename, data))
    {
        if (error_msg) *error_msg = wiv_plugin::makeError("qoi: failed to read %s", filename);
        return nullptr;
    }
    giv::qoi::Header h;
    std::vector<uint8_t> rgba;
    if (!giv::qoi::decode(data.data(), data.size(), h, rgba))
    {
        if (error_msg) *error_msg = wiv_plugin::makeError("qoi: not a valid QOI file: %s", filename);
        return nullptr;
    }
    WivPluginImage* img = wiv_plugin::allocImage(static_cast<int>(h.width), static_cast<int>(h.height));
    std::memcpy(img->rgba, rgba.data(), rgba.size());
    if (h.channels == 3)
        for (size_t i = 3; i < rgba.size(); i += 4) img->rgba[i] = 255;
    return img;
}
