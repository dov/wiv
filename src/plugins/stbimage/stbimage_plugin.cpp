//
// stbimage_plugin.cpp - wiv image plugin for PNG/JPEG/BMP/TGA/GIF/PSD via
// the vendored single-header stb_image (third_party/stb_image.h).
//
#include "../wiv_plugin_common.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
    static const char* kExtensions[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".psd"};
    for (const char* ext : kExtensions)
        if (hasExtension(f, ext))
            return true;
    return false;
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(filename, &width, &height, &channels, 4);
    if (!pixels)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("stbimage: failed to load %s", filename);
        return nullptr;
    }

    WivPluginImage* img = wiv_plugin::allocImage(width, height);
    std::memcpy(img->rgba, pixels, static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);

    // 16-bit grayscale PNG carries real dynamic range beyond the 8-bit
    // preview above - decode it a second time into a raw sample buffer for
    // the Contrast/Color Table tools. Other formats/channel counts stay
    // RGBA8-only (stbi_load already collapsed them to an 8-bit preview).
    int rawWidth = 0, rawHeight = 0, rawChannels = 0;
    if (stbi_info(filename, &rawWidth, &rawHeight, &rawChannels) && rawChannels == 1 &&
        stbi_is_16_bit(filename))
    {
        unsigned short* samples16 = stbi_load_16(filename, &rawWidth, &rawHeight, &rawChannels, 1);
        if (samples16 && rawWidth == width && rawHeight == height)
        {
            wiv_plugin::allocSamples(img, WIV_SAMPLE_U16);
            std::memcpy(img->samples, samples16, static_cast<size_t>(width) * height * 2);
        }
        if (samples16)
            stbi_image_free(samples16);
    }

    return img;
}
