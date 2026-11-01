#pragma once
//
// wiv_plugin.h - the C ABI every wiv image-format plugin (.so) must
// export. Mirrors giv's own plugin ABI (~/hd/github/giv/src/givplugin.h,
// givplugin.cc - the real, load-bearing symbol names there are
// giv_plugin_supports_file / giv_plugin_load_file). Every plugin normalizes
// an 8-bit RGBA preview at load time (used directly for color images, and
// as the fallback for formats with no raw-sample path below). Formats that
// carry real single-channel dynamic range (grayscale PGM/TIFF/16-bit PNG)
// additionally populate an optional raw sample buffer, which is what the
// Contrast/Color Table tools operate on (see DisplayImage.h) - RGBA8 alone
// would quantize away the dynamic range those tools are stretching.
//
#include <stdbool.h>

#ifdef _WIN32
#define WIV_PLUGIN_API __declspec(dllexport)
#else
#define WIV_PLUGIN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum WivSampleType
    {
        WIV_SAMPLE_NONE = 0, // no raw sample data; rgba is the only representation
        WIV_SAMPLE_U8,
        WIV_SAMPLE_U16,
        WIV_SAMPLE_FLOAT
    } WivSampleType;

    typedef struct WivPluginImage
    {
        int width;
        int height;
        unsigned char* rgba; // width*height*4 bytes, top-to-bottom row order

        // EXIF/TIFF Orientation tag value (1-8, see the Exif spec's table),
        // or 1 ("normal", no transform) if the plugin doesn't parse one.
        // The host applies the corresponding rotation/flip centrally
        // (ImagePluginHost::load()) so every plugin shares one
        // implementation instead of duplicating it; a plugin that already
        // hands back upright pixels (e.g. tiff_plugin.cpp's
        // TIFFReadRGBAImageOriented) should just leave this at 1.
        int orientation;

        // Optional raw single-channel sample buffer: width*height*depth
        // samples, row-major, top-to-bottom, slice-major (slice 0's
        // width*height samples, then slice 1's, ...), each sampleType wide
        // (1/2/4 bytes). WIV_SAMPLE_NONE/NULL for plugins/formats with no
        // meaningful dynamic range beyond the RGBA8 preview (e.g. color
        // JPEG/WebP).
        WivSampleType sampleType;
        void* samples;

        // Number of 2D slices stacked in `samples` (1 for an ordinary 2D
        // image). >1 for formats that carry a depth axis (npy 3D arrays,
        // FITS NAXIS=3, multi-frame DICOM) - see giv's Up/Down slice-cycling
        // (giv-win.cc's current_slice). `rgba` is always just a single
        // width*height*4 preview (slice 0) - the host regenerates the
        // per-slice display buffer from `samples` (see DisplayImage.h).
        int depth;
    } WivPluginImage;

    // Does this plugin recognize `filename` (by extension)? Called by the
    // host for every loaded plugin, in sorted-filename order, until one
    // returns true (first-match-wins).
    WIV_PLUGIN_API bool wiv_plugin_supports_file(const char* filename);

    // OPTIONAL. Does the *content* of `filename` look like a format this
    // plugin handles, regardless of its name? Does file I/O, so the host only
    // calls it for a file the user named explicitly (command line, drop, File
    // > Open) whose suffix no plugin claims - never from a directory scan.
    // Plugins that don't export it are simply never sniffed.
    WIV_PLUGIN_API bool wiv_plugin_sniff_file(const char* filename);

    // Load `filename` into a freshly heap-allocated WivPluginImage. On
    // failure returns NULL and, if error_msg is non-NULL, sets *error_msg
    // to a freshly heap-allocated message (free with wiv_plugin_free_error).
    WIV_PLUGIN_API WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg);

    // OPTIONAL, for formats that hold several pages in one file (multi-page
    // TIFF). A plugin exports both or neither. wiv_plugin_page_count returns
    // the number of pages (>= 1, or 0 if the file can't be read);
    // wiv_plugin_load_page loads page `page` (0-based) like
    // wiv_plugin_load_image does, which is equivalent to page 0. Plugins
    // that don't export them are treated as having a single page.
    WIV_PLUGIN_API int wiv_plugin_page_count(const char* filename);
    WIV_PLUGIN_API WivPluginImage* wiv_plugin_load_page(const char* filename, int page, char** error_msg);

    // OPTIONAL, for plugins whose decode can take long enough that the host
    // wants to abandon it (background prefetch of a neighbour the user has
    // already navigated away from). Like wiv_plugin_load_page (`page` is 0
    // for single-page formats), but the plugin polls `*cancel` at convenient
    // points - e.g. every few scanlines - and, as soon as it reads non-zero,
    // frees whatever it allocated and returns NULL. The host sets the flag
    // from another thread and never clears it while the call is running, so
    // a plain (volatile) read suffices. On cancellation the error message
    // may be left NULL; the host ignores it. A plugin that doesn't export
    // this is simply run to completion (its result is discarded if the flag
    // was set meanwhile), so exporting it is purely an optimization.
    WIV_PLUGIN_API WivPluginImage* wiv_plugin_load_page_cancellable(const char* filename, int page,
                                                                       const volatile int* cancel, char** error_msg);

    // OPTIONAL. A fast, reduced-resolution decode used to put *something* on
    // screen while the full decode runs in the background (JPEG: libjpeg's
    // 1/2, 1/4 or 1/8 DCT scaling, several times faster than a full decode).
    // The plugin picks the largest reduction whose longer side is still
    // >= min_long_side, returns that image (same ownership/`orientation`
    // rules as wiv_plugin_load_image) and stores the full-resolution
    // dimensions - before orientation is applied - in *full_width/*full_height.
    // Returns NULL, leaving error_msg NULL, if no worthwhile reduction exists
    // (the image is small, or the format can't decode cheaply at reduced
    // size); the host then just does the full load.
    WIV_PLUGIN_API WivPluginImage* wiv_plugin_load_preview(const char* filename, int min_long_side, int* full_width,
                                                              int* full_height, char** error_msg);

    // Frees an image returned by wiv_plugin_load_image.
    WIV_PLUGIN_API void wiv_plugin_free_image(WivPluginImage* img);

    // Frees an error message set by wiv_plugin_load_image.
    WIV_PLUGIN_API void wiv_plugin_free_error(char* error_msg);

#ifdef __cplusplus
}
#endif
