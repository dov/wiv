//
// exr_plugin.cpp - wiv image plugin for OpenEXR (.exr), via the OpenEXR
// library's RGBA interface (single-part, scanline or tiled; Y/RGB/RGBA
// channels, any data type). Values are clamped to [0,1] and sRGB-encoded
// for the RGBA8 preview; alpha is kept linear.
//
#include "../wiv_plugin_common.h"

#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfThreading.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
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

unsigned char encodeSrgb(float v)
{
    if (!(v > 0.f))
        return 0;
    if (v >= 1.f)
        return 255;
    float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(s * 255.f + 0.5f);
}

} // namespace

extern "C" bool wiv_plugin_supports_file(const char* filename)
{
    return hasExtension(filename, ".exr");
}

extern "C" WivPluginImage* wiv_plugin_load_image(const char* filename, char** error_msg)
{
    try
    {
        // OpenEXR opens narrow paths via fopen/ifstream, which on Windows
        // needs the ANSI code page; the 8.3 short name sidesteps that.
        Imf::RgbaInputFile file(wiv_plugin::asciiSafePath(filename).c_str());
        const Imath::Box2i dw = file.dataWindow();
        const int w = dw.max.x - dw.min.x + 1;
        const int h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0)
            throw std::runtime_error("empty data window");

        Imf::Array2D<Imf::Rgba> pixels(h, w);
        file.setFrameBuffer(&pixels[0][0] - dw.min.x - static_cast<size_t>(dw.min.y) * w, 1, w);
        file.readPixels(dw.min.y, dw.max.y);

        const bool hasAlpha = (file.channels() & Imf::WRITE_A) != 0;
        WivPluginImage* img = wiv_plugin::allocImage(w, h);
        for (int y = 0; y < h; ++y)
        {
            unsigned char* dst = img->rgba + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                const Imf::Rgba& p = pixels[y][x];
                dst[x * 4 + 0] = encodeSrgb(p.r);
                dst[x * 4 + 1] = encodeSrgb(p.g);
                dst[x * 4 + 2] = encodeSrgb(p.b);
                dst[x * 4 + 3] = hasAlpha ? static_cast<unsigned char>(std::clamp(static_cast<float>(p.a), 0.f, 1.f) * 255.f + 0.5f) : 255;
            }
        }
        return img;
    }
    catch (const std::exception& e)
    {
        if (error_msg)
            *error_msg = wiv_plugin::makeError("exr: %s", e.what());
        return nullptr;
    }
}
