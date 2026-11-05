#include "DisplayImage.h"

#include "ImagePluginHost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace giv
{

namespace
{

// Splits [0, n) over the hardware threads. Rendering a 15-megapixel image is
// memory-bound but still scales well over a few cores; small images stay on
// the calling thread.
template <class Fn> void parallelRange(size_t n, Fn fn)
{
    const size_t kMinPerThread = 1u << 18;
    const size_t threads = std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), std::max<size_t>(1, n / kMinPerThread));
    if (threads <= 1)
    {
        fn(size_t{0}, n);
        return;
    }
    std::vector<std::thread> pool;
    const size_t chunk = (n + threads - 1) / threads;
    for (size_t t = 1; t < threads; ++t)
    {
        const size_t begin = t * chunk, end = std::min(n, begin + chunk);
        if (begin < end) pool.emplace_back([=] { fn(begin, end); });
    }
    fn(size_t{0}, std::min(n, chunk));
    for (auto& th : pool) th.join();
}

uint32_t packRgba(const uint8_t* lut, int u8)
{
    uint8_t px[4];
    if (lut)
    {
        px[0] = lut[u8 * 3 + 0];
        px[1] = lut[u8 * 3 + 1];
        px[2] = lut[u8 * 3 + 2];
    }
    else
        px[0] = px[1] = px[2] = static_cast<uint8_t>(u8);
    px[3] = 255;
    uint32_t v;
    std::memcpy(&v, px, 4);
    return v;
}

// Contrast-stretches a sample to a 0..255 index (same rounding as before: nearest, clamped).
int stretch(double v, double contrastMin, double range)
{
    const double x = range != 0.0 ? (v - contrastMin) / range * 255.0 : 127.5;
    if (!(x > 0.0)) return 0; // also NaN
    if (x >= 255.0) return 255;
    return static_cast<int>(x + 0.5);
}

// Integer samples have at most 65536 distinct values, so the whole
// stretch + colormap is one table lookup per pixel.
template <class T>
void renderIntegral(const T* in, size_t n, size_t tableSize, double contrastMin, double range, const uint8_t* lut, uint8_t* out)
{
    std::vector<uint32_t> table(tableSize);
    for (size_t v = 0; v < tableSize; ++v) table[v] = packRgba(lut, stretch(static_cast<double>(v), contrastMin, range));
    parallelRange(n, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) std::memcpy(out + i * 4, &table[in[i]], 4);
    });
}

// giv's PSEUDO_COLOR_EQUALIZE / PSEUDO_COLOR_NORMALIZE (giv-win.gob): the gray
// table is derived from the contrast-stretched 8-bit image itself.
std::vector<uint8_t> adaptiveLut(const LoadedImage& src, double contrastMin, double range, colormaps::Id id, int slice)
{
    const size_t pixelCount = static_cast<size_t>(src.width) * src.height;
    const size_t sliceOffset = pixelCount * static_cast<size_t>(std::max(0, slice));
    std::array<size_t, 256> hist{};
    switch (src.sampleType)
    {
        case WIV_SAMPLE_U8:
        {
            const uint8_t* in = src.samples.data() + sliceOffset;
            std::array<int, 256> level;
            for (int v = 0; v < 256; ++v) level[v] = stretch(v, contrastMin, range);
            for (size_t i = 0; i < pixelCount; ++i) ++hist[level[in[i]]];
            break;
        }
        case WIV_SAMPLE_U16:
        {
            const uint16_t* in = reinterpret_cast<const uint16_t*>(src.samples.data()) + sliceOffset;
            std::vector<uint8_t> level(65536);
            for (int v = 0; v < 65536; ++v) level[v] = static_cast<uint8_t>(stretch(v, contrastMin, range));
            for (size_t i = 0; i < pixelCount; ++i) ++hist[level[in[i]]];
            break;
        }
        case WIV_SAMPLE_FLOAT:
        {
            const float* in = reinterpret_cast<const float*>(src.samples.data()) + sliceOffset;
            for (size_t i = 0; i < pixelCount; ++i) ++hist[stretch(in[i], contrastMin, range)];
            break;
        }
        default: break;
    }

    std::array<uint8_t, 256> map{};
    if (id == colormaps::Id::Equalize)
    {
        size_t accumulated = 0; // pixels strictly below this level
        for (int level = 0; level < 256; ++level)
        {
            map[level] = static_cast<uint8_t>(accumulated * 255 / std::max<size_t>(pixelCount, 1));
            accumulated += hist[level];
        }
    }
    else
    {
        int lo = 255, hi = 0;
        for (int level = 0; level < 256; ++level)
            if (hist[level])
            {
                lo = std::min(lo, level);
                hi = std::max(hi, level);
            }
        const int span = std::max(hi - lo, 1);
        for (int level = 0; level < 256; ++level) map[level] = static_cast<uint8_t>(std::clamp(255 * (level - lo) / span, 0, 255));
    }
    std::vector<uint8_t> lut(256 * 3);
    for (int level = 0; level < 256; ++level) lut[level * 3] = lut[level * 3 + 1] = lut[level * 3 + 2] = map[level];
    return lut;
}

} // namespace

std::vector<uint8_t> displayLut(const LoadedImage& src, float contrastMin, float contrastMax, colormaps::Id colormapId,
                                bool colormapEnabled, int slice)
{
    if (!colormapEnabled) return {};
    if (colormaps::isAdaptive(colormapId))
        return adaptiveLut(src, contrastMin, static_cast<double>(contrastMax) - contrastMin, colormapId, slice);
    const uint8_t* lut = colormaps::lut(colormapId);
    return lut ? std::vector<uint8_t>(lut, lut + 256 * 3) : std::vector<uint8_t>();
}

void renderDisplayRgba(const LoadedImage& src, float contrastMin, float contrastMax,
                        colormaps::Id colormapId, bool colormapEnabled, PixelBuffer& outRgba,
                        int slice)
{
    if (src.sampleType == WIV_SAMPLE_NONE)
    {
        outRgba = src.rgba; // shares the buffer: a colour image is displayed as is
        return;
    }
    const size_t pixelCount = static_cast<size_t>(src.width) * src.height;
    std::vector<uint8_t> rendered(pixelCount * 4);

    const std::vector<uint8_t> lutTable = displayLut(src, contrastMin, contrastMax, colormapId, colormapEnabled, slice);
    const uint8_t* lut = lutTable.empty() ? nullptr : lutTable.data();
    const double range = static_cast<double>(contrastMax) - contrastMin;
    const size_t sliceOffset = pixelCount * static_cast<size_t>(std::max(0, slice));
    uint8_t* out = rendered.data();

    switch (src.sampleType)
    {
        case WIV_SAMPLE_U8:
            renderIntegral(src.samples.data() + sliceOffset, pixelCount, 256, contrastMin, range, lut, out);
            break;
        case WIV_SAMPLE_U16:
            renderIntegral(reinterpret_cast<const uint16_t*>(src.samples.data()) + sliceOffset, pixelCount, 65536, contrastMin,
                           range, lut, out);
            break;
        case WIV_SAMPLE_FLOAT:
        {
            uint32_t table[256];
            for (int u = 0; u < 256; ++u) table[u] = packRgba(lut, u);
            const float* in = reinterpret_cast<const float*>(src.samples.data()) + sliceOffset;
            const float fmin = contrastMin, scale = range != 0.0 ? static_cast<float>(255.0 / range) : 0.0f;
            const float fallback = 127.5f;
            parallelRange(pixelCount, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i)
                {
                    const float x = range != 0.0 ? (in[i] - fmin) * scale : fallback;
                    const int u8 = !(x > 0.0f) ? 0 : (x >= 255.0f ? 255 : static_cast<int>(x + 0.5f));
                    std::memcpy(out + i * 4, &table[u8], 4);
                }
            });
            break;
        }
        default:
        {
            const uint32_t black = packRgba(nullptr, 0);
            for (size_t i = 0; i < pixelCount; ++i) std::memcpy(out + i * 4, &black, 4);
        }
    }
    outRgba = std::move(rendered);
}

uint32_t displayRgbaAt(const LoadedImage& src, float contrastMin, float contrastMax, colormaps::Id colormapId,
                       bool colormapEnabled, size_t sampleIndex)
{
    double v = 0.0;
    switch (src.sampleType)
    {
        case WIV_SAMPLE_U8: v = src.samples[sampleIndex]; break;
        case WIV_SAMPLE_U16: v = reinterpret_cast<const uint16_t*>(src.samples.data())[sampleIndex]; break;
        case WIV_SAMPLE_FLOAT: v = reinterpret_cast<const float*>(src.samples.data())[sampleIndex]; break;
        default: break;
    }
    const double range = static_cast<double>(contrastMax) - contrastMin;
    if (!colormapEnabled || !colormaps::isAdaptive(colormapId))
        return packRgba(colormapEnabled ? colormaps::lut(colormapId) : nullptr, stretch(v, contrastMin, range));

    // Equalize/Normalize need the whole image's histogram: keep the table for
    // the pixel under the cursor instead of rebuilding it on every mouse move.
    static struct
    {
        const void* samples = nullptr;
        float min = 0, max = 0;
        colormaps::Id id = colormaps::Id::None;
        std::vector<uint8_t> lut;
    } cache;
    const size_t pixelCount = static_cast<size_t>(src.width) * src.height;
    const int slice = pixelCount ? static_cast<int>(sampleIndex / pixelCount) : 0;
    if (cache.samples != src.samples.data() || cache.min != contrastMin || cache.max != contrastMax || cache.id != colormapId ||
        cache.lut.empty())
    {
        cache.lut = displayLut(src, contrastMin, contrastMax, colormapId, true, slice);
        cache.samples = src.samples.data();
        cache.min = contrastMin;
        cache.max = contrastMax;
        cache.id = colormapId;
    }
    return packRgba(cache.lut.data(), stretch(v, contrastMin, range));
}

} // namespace giv
