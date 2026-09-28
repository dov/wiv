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
