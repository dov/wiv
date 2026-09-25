#pragma once
//
// wiv_plugin.h - the C ABI every wiv image-format plugin (.so) must
// export. Mirrors giv's own plugin ABI (~/hd/github/giv/src/givplugin.h,
// givplugin.cc - the real, load-bearing symbol names there are
// giv_plugin_supports_file / giv_plugin_load_file), collapsed to
// RGBA8-only output: wiv has no raw-sample/contrast pipeline (Phase 1),
// so each plugin normalizes straight to 8-bit RGBA at load time.
//
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct WivPluginImage
    {
        int width;
        int height;
        unsigned char* rgba; // width*height*4 bytes, top-to-bottom row order
    } WivPluginImage;

    // Does this plugin recognize `filename` (by extension)? Called by the
    // host for every loaded plugin, in sorted-filename order, until one
    // returns true (first-match-wins).
    bool wiv_plugin_supports_file(const char* filename);

    // Load `filename` into a freshly heap-allocated WivPluginImage. On
    // failure returns NULL and, if error_msg is non-NULL, sets *error_msg
    // to a freshly heap-allocated message (free with wiv_plugin_free_error).
    WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg);

    // Frees an image returned by wiv_plugin_load_image.
    void wiv_plugin_free_image(WivPluginImage* img);

    // Frees an error message set by wiv_plugin_load_image.
    void wiv_plugin_free_error(char* error_msg);

#ifdef __cplusplus
}
#endif
