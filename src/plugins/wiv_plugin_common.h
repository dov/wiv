// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// wiv_plugin_common.h - tiny shared helpers for wiv image plugins.
// Header-only for allocImage/makeError (both are actually called from
// every plugin's own TU, so they get emitted); wiv_plugin_free_image/
// wiv_plugin_free_error are declared here but defined in
// wiv_plugin_common.cpp (compiled into each plugin target) - an `inline`
// extern "C" definition that's never ODR-used within its own TU is not
// guaranteed to be emitted at all, which silently dropped both symbols
// from every plugin's dynamic symbol table the first time this was tried.
//
#include "wiv_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>


#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wiv_plugin
{

// fopen for UTF-8 filenames. On Windows narrow fopen goes through the ANSI
// code page (the UTF-8 manifest isn't reliable for every CRT), so convert to
// UTF-16 and use _wfopen.
inline FILE* fopenUtf8(const char* filename, const char* mode)
{
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, filename, -1, nullptr, 0);
    if (n <= 0) return std::fopen(filename, mode);
    wchar_t* w = static_cast<wchar_t*>(std::malloc(sizeof(wchar_t) * n));
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, w, n);
    wchar_t wmode[8] = {};
    for (int i = 0; mode[i] && i < 7; ++i) wmode[i] = static_cast<wchar_t>(mode[i]);
    FILE* f = _wfopen(w, wmode);
    std::free(w);
    return f;
#else
    return std::fopen(filename, mode);
#endif
}


// UTF-8 -> UTF-16 (Windows only).
#ifdef _WIN32
inline std::wstring utf8ToWide(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

// For libraries that only take narrow paths and open them with fopen (cfitsio):
// return the 8.3 short name, which is pure ASCII, when the path is non-ASCII.
inline std::string asciiSafePath(const char* utf8)
{
    bool ascii = true;
    for (const char* p = utf8; *p; ++p) if (static_cast<unsigned char>(*p) >= 0x80) ascii = false;
    if (ascii) return utf8;
    const std::wstring w = utf8ToWide(utf8);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetShortPathNameW(w.c_str(), buf, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) return utf8;
    std::string out;
    for (DWORD i = 0; i < n; ++i) out.push_back(static_cast<char>(buf[i]));
    return out;
}
#else
inline std::string asciiSafePath(const char* utf8) { return utf8; }
#endif
// `withPreview` false leaves rgba NULL: for a raw-sample image too big for an
// RGBA8 preview to be worth its memory and time (see WivPluginImage::rgba).
inline WivPluginImage* allocImage(int width, int height, bool withPreview = true)
{
    auto* img = static_cast<WivPluginImage*>(std::malloc(sizeof(WivPluginImage)));
    img->width = width;
    img->height = height;
    img->rgba = withPreview ? static_cast<unsigned char*>(std::malloc(static_cast<size_t>(width) * height * 4)) : nullptr;
    img->orientation = 1;
    img->sampleType = WIV_SAMPLE_NONE;
    img->samples = nullptr;
    img->depth = 1;
    return img;
}

inline size_t sampleTypeSize(WivSampleType type)
{
    switch (type)
    {
        case WIV_SAMPLE_U8:
            return 1;
        case WIV_SAMPLE_U16:
            return 2;
        case WIV_SAMPLE_FLOAT:
            return 4;
        default:
            return 0;
    }
}

// Allocates and attaches a raw single-channel sample buffer to `img`
// (already created via allocImage). Called by plugins that decode
// genuinely single-channel/grayscale source data. `depth` slices of
// width*height samples each are allocated (depth=1 for an ordinary 2D
// image) - see WivPluginImage::depth.
inline void allocSamples(WivPluginImage* img, WivSampleType type, int depth = 1)
{
    img->sampleType = type;
    img->depth = depth;
    img->samples = std::malloc(static_cast<size_t>(img->width) * img->height *
                                static_cast<size_t>(depth) * sampleTypeSize(type));
}

inline char* makeError(const char* fmt, const char* arg)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    return strdup(buf);
}

} // namespace wiv_plugin
