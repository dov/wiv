#include "DisplayImage.h"

#include "ImagePluginHost.h"

#include <algorithm>
#include <cmath>

namespace giv
{

namespace
{

// Reads sample (x,y) as a double, regardless of sampleType - mirrors
// giv_image_get_value() (also duplicated, file-locally, in
// ImagePluginHost.cpp for the min/max scan at load time).
double sampleValue(const LoadedImage& img, size_t idx)
{
    switch (img.sampleType)
    {
        case WIV_SAMPLE_U8:
            return img.samples[idx];
        case WIV_SAMPLE_U16:
            return reinterpret_cast<const uint16_t*>(img.samples.data())[idx];
        case WIV_SAMPLE_FLOAT:
            return reinterpret_cast<const float*>(img.samples.data())[idx];
        default:
            return 0.0;
    }
}

} // namespace

void renderDisplayRgba(const LoadedImage& src, float contrastMin, float contrastMax,
                        colormaps::Id colormapId, bool colormapEnabled, std::vector<uint8_t>& outRgba)
{
    const size_t pixelCount = static_cast<size_t>(src.width) * src.height;
    outRgba.resize(pixelCount * 4);

    if (src.sampleType == WIV_SAMPLE_NONE)
    {
        outRgba = src.rgba;
        return;
    }

    const uint8_t* lut = colormapEnabled ? colormaps::lut(colormapId) : nullptr;
    const float range = contrastMax - contrastMin;

    for (size_t i = 0; i < pixelCount; ++i)
    {
        double v = sampleValue(src, i);
        double t = range != 0.0f ? (v - contrastMin) / range : 0.5;
        int u8 = static_cast<int>(std::lround(t * 255.0));
        u8 = std::clamp(u8, 0, 255);

        uint8_t* dst = &outRgba[i * 4];
        if (lut)
        {
            dst[0] = lut[u8 * 3 + 0];
            dst[1] = lut[u8 * 3 + 1];
            dst[2] = lut[u8 * 3 + 2];
        }
        else
        {
            dst[0] = dst[1] = dst[2] = static_cast<uint8_t>(u8);
        }
        dst[3] = 255;
    }
}

} // namespace giv
