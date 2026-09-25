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

namespace wiv_plugin
{

inline WivPluginImage* allocImage(int width, int height)
{
    auto* img = static_cast<WivPluginImage*>(std::malloc(sizeof(WivPluginImage)));
    img->width = width;
    img->height = height;
    img->rgba = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(width) * height * 4));
    return img;
}

inline char* makeError(const char* fmt, const char* arg)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    return strdup(buf);
}

} // namespace wiv_plugin
