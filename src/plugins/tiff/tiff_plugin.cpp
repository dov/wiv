//
// tiff_plugin.cpp - wiv image plugin for TIFF, via libtiff. Modeled on
// giv's ~/hd/github/giv/src/plugins/tiff.c, but simplified to use
// libtiff's own any-format-to-RGBA8 convenience reader
// (TIFFReadRGBAImageOriented) instead of giv's manual per-format-combo
// path, since wiv has no raw-sample/contrast pipeline to preserve bit
// depth for (Phase 1: normalize straight to RGBA8 at load time).
//
#include "../wiv_plugin_common.h"

#include <tiffio.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

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

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    std::string f(filename);
    return hasExtension(f, ".tif") || hasExtension(f, ".tiff");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    TIFF* tif = TIFFOpen(filename, "r");
    if (!tif)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: failed to open %s", filename);
        return nullptr;
    }

    uint32_t width = 0, height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    if (width == 0 || height == 0)
    {
        TIFFClose(tif);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: invalid dimensions in %s", filename);
        return nullptr;
    }

    WivPluginImage* img = wiv_plugin::allocImage(static_cast<int>(width), static_cast<int>(height));

    // TIFFReadRGBAImageOriented fills top-to-bottom, ABGR-packed-as-uint32
    // (i.e. byte order R,G,B,A on little-endian hosts) - exactly the
    // layout WivPluginImage::rgba expects.
    if (!TIFFReadRGBAImageOriented(tif, width, height, reinterpret_cast<uint32_t*>(img->rgba),
                                    ORIENTATION_TOPLEFT, 0))
    {
        TIFFClose(tif);
        wiv_plugin_free_image(img);
        if (error_msg)
            *error_msg = wiv_plugin::makeError("tiff: failed to decode %s", filename);
        return nullptr;
    }

    TIFFClose(tif);
    return img;
}
