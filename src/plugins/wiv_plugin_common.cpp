// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "wiv_plugin.h"

#include <cstdlib>

extern "C" void wiv_plugin_free_image(WivPluginImage* img)
{
    if (!img)
        return;
    std::free(img->rgba);
    std::free(img->samples);
    std::free(img);
}

extern "C" void wiv_plugin_free_error(char* error_msg)
{
    std::free(error_msg);
}
