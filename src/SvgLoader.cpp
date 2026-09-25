#include "SvgLoader.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace giv
{

namespace
{

Color colorFromSvg(unsigned int packed, float shapeOpacity)
{
    Color c;
    c.r = static_cast<float>(packed & 0xff) / 255.0f;
    c.g = static_cast<float>((packed >> 8) & 0xff) / 255.0f;
    c.b = static_cast<float>((packed >> 16) & 0xff) / 255.0f;
    c.a = (static_cast<float>((packed >> 24) & 0xff) / 255.0f) * shapeOpacity;
    return c;
}

// Dataset only carries one flat Color per shape (no gradient shader), so a
// gradient fill/stroke is approximated by its stops' average, weighted by
// the offset interval each stop covers - close enough to read as "the
// shape's color" rather than leaving it unfilled, which is how this looked
// before (bottle body etc. all used gradients and rendered as empty outlines).
// (Gradient shapes that also carry a blur filter skip this entirely - see
// rasterizeBlurredShape, which rasterizes the true gradient via nanosvgrast.)
Color averageGradientColor(const NSVGgradient* grad, float shapeOpacity)
{
    Color c;
    if (!grad || grad->nstops == 0) return c;
    if (grad->nstops == 1)
    {
        c = colorFromSvg(grad->stops[0].color, shapeOpacity);
        return c;
    }

    double r = 0, g = 0, b = 0, a = 0, wsum = 0;
    for (int i = 0; i < grad->nstops; ++i)
    {
        float lo = (i == 0) ? grad->stops[0].offset : (grad->stops[i - 1].offset + grad->stops[i].offset) * 0.5f;
        float hi = (i == grad->nstops - 1) ? grad->stops[i].offset
                                            : (grad->stops[i].offset + grad->stops[i + 1].offset) * 0.5f;
        double w = std::max(0.0f, hi - lo);
        if (w <= 0.0) w = 1.0 / grad->nstops; // degenerate/equal offsets: fall back to a plain average
        Color sc = colorFromSvg(grad->stops[i].color, 1.0f);
        r += sc.r * w;
        g += sc.g * w;
        b += sc.b * w;
        a += sc.a * w;
        wsum += w;
    }
    if (wsum <= 0.0) wsum = 1.0;
    c.r = static_cast<float>(r / wsum);
    c.g = static_cast<float>(g / wsum);
    c.b = static_cast<float>(b / wsum);
    c.a = static_cast<float>(a / wsum) * shapeOpacity;
    return c;
}

bool isGradient(signed char paintType)
{
    return paintType == NSVG_PAINT_LINEAR_GRADIENT || paintType == NSVG_PAINT_RADIAL_GRADIENT;
}

Color paintColor(const NSVGpaint& paint, float shapeOpacity)
{
    if (paint.type == NSVG_PAINT_COLOR) return colorFromSvg(paint.color, shapeOpacity);
    return averageGradientColor(paint.gradient, shapeOpacity);
}

// Appends one NSVGpath's cubic-bezier point stream (x0,y0, then repeated
// c1,c2,end triples - see NSVGpath::pts) to `ds` using the same Op grammar
// GivParser uses for its own $curve directive (Move, then Curve/Cont/Cont
// per segment), so SceneBuilder tessellates SVG curves identically to
// giv-native ones - no rasterization, no precision loss.
void appendPath(Dataset& ds, SceneData& scene, const NSVGpath* path)
{
    if (path->npts < 1) return;
    const float* pts = path->pts;

    ds.addPoint(pts[0], pts[1], Op::Move);
    scene.updateBounds(pts[0], pts[1]);

    for (int seg = 0; (seg * 3 + 3) < path->npts; ++seg)
    {
        const float* c1 = pts + (seg * 3 + 1) * 2;
        const float* c2 = pts + (seg * 3 + 2) * 2;
        const float* end = pts + (seg * 3 + 3) * 2;
        ds.addPoint(c1[0], c1[1], Op::Curve);
        ds.addPoint(c2[0], c2[1], Op::Cont);
        ds.addPoint(end[0], end[1], Op::Cont);
        scene.updateBounds(end[0], end[1]);
    }

    if (path->closed) ds.addPoint(0, 0, Op::ClosePath);
}

// Returns the value of attribute `attr` within xml[tagStart..tagEnd] (a
// single "<...>" tag's text span), or "" if absent. Order-independent -
// searches for `attr="` anywhere in that span, so it doesn't matter
// whether e.g. `id` or `style` comes first in the tag.
std::string extractAttr(const std::string& xml, size_t tagStart, size_t tagEnd, const std::string& attr)
{
    std::string needle = attr + "=\"";
    size_t p = xml.find(needle, tagStart);
    if (p == std::string::npos || p >= tagEnd) return {};
    p += needle.size();
    size_t q = xml.find('"', p);
    if (q == std::string::npos || q > tagEnd) return {};
    return xml.substr(p, q - p);
}

// nanosvg has no support at all for SVG <filter>/feGaussianBlur - it's a
// pure geometry parser. This is NOT a general XML/CSS parser either: it
// recognizes just enough of Inkscape's usual drop-shadow/soft-highlight
// export shape (a <filter id="X"> block containing exactly one
// <feGaussianBlur stdDeviation="D"/>, referenced by some element's own
// style="...filter:url(#X)...") to resolve elementId -> blur stdDeviation,
// so loadSvgFile can rasterize+blur just those shapes (see
// rasterizeBlurredShape) instead of silently dropping the effect.
std::unordered_map<std::string, float> parseElementBlurRadii(const std::string& xml)
{
    std::unordered_map<std::string, float> filterStdDev;
    size_t pos = 0;
    while ((pos = xml.find("<filter", pos)) != std::string::npos)
    {
        size_t tagEnd = xml.find('>', pos);
        if (tagEnd == std::string::npos) break;
        std::string id = extractAttr(xml, pos, tagEnd, "id");
        size_t nextFilterPos = xml.find("<filter", pos + 1);
        size_t blurPos = xml.find("<feGaussianBlur", tagEnd);
        if (!id.empty() && blurPos != std::string::npos && (nextFilterPos == std::string::npos || blurPos < nextFilterPos))
        {
            size_t blurTagEnd = xml.find('>', blurPos);
            if (blurTagEnd != std::string::npos)
            {
                std::string sd = extractAttr(xml, blurPos, blurTagEnd, "stdDeviation");
                if (!sd.empty())
                {
                    try
                    {
                        filterStdDev[id] = std::stof(sd);
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        pos = tagEnd + 1;
    }

    std::unordered_map<std::string, float> elementBlur;
    pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos)
    {
        if (xml.compare(pos, 2, "</") == 0)
        {
            pos += 2;
            continue;
        }
        size_t tagEnd = xml.find('>', pos);
        if (tagEnd == std::string::npos) break;

        std::string id = extractAttr(xml, pos, tagEnd, "id");
        std::string style = extractAttr(xml, pos, tagEnd, "style");
        if (!id.empty() && !style.empty())
        {
            size_t fp = style.find("filter:url(#");
            if (fp != std::string::npos)
            {
                size_t start = fp + std::strlen("filter:url(#");
                size_t end = style.find(')', start);
                if (end != std::string::npos)
                {
                    auto it = filterStdDev.find(style.substr(start, end - start));
                    if (it != filterStdDev.end()) elementBlur[id] = it->second;
                }
            }
        }
        pos = tagEnd + 1;
    }
    return elementBlur;
}

// Separable Gaussian blur (2-pass, clamp-to-edge), operating on
// premultiplied alpha so transparent surroundings don't bleed dark/light
// fringes into the blurred edge once unpremultiplied back out. `sigma` is
// in source pixels, which for buf are the same units as the SVG's own
// stdDeviation since the shape was rasterized at plain "px"/96dpi, scale 1.
void gaussianBlurRGBA(std::vector<unsigned char>& buf, int w, int h, float sigma)
{
    if (sigma <= 0.05f || w <= 0 || h <= 0) return;

    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<float> pr(n), pg(n), pb(n), pa(n);
    for (size_t i = 0; i < n; ++i)
    {
        float a = buf[i * 4 + 3] / 255.0f;
        pr[i] = buf[i * 4 + 0] / 255.0f * a;
        pg[i] = buf[i * 4 + 1] / 255.0f * a;
        pb[i] = buf[i * 4 + 2] / 255.0f * a;
        pa[i] = a;
    }

    int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
    std::vector<float> kernel(2 * radius + 1);
    float ksum = 0.0f;
    for (int i = -radius; i <= radius; ++i)
    {
        float v = std::exp(-(static_cast<float>(i * i)) / (2.0f * sigma * sigma));
        kernel[i + radius] = v;
        ksum += v;
    }
    for (auto& v : kernel) v /= ksum;

    std::vector<float> tmp(n);
    auto blur1D = [&](std::vector<float>& channel, bool horizontal) {
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                float acc = 0.0f;
                for (int k = -radius; k <= radius; ++k)
                {
                    int sx = horizontal ? x + k : x;
                    int sy = horizontal ? y : y + k;
                    sx = std::clamp(sx, 0, w - 1);
                    sy = std::clamp(sy, 0, h - 1);
                    acc += channel[static_cast<size_t>(sy) * w + sx] * kernel[k + radius];
                }
                tmp[static_cast<size_t>(y) * w + x] = acc;
            }
        }
        channel = tmp;
    };

    for (auto* channel : {&pr, &pg, &pb, &pa})
    {
        blur1D(*channel, true);
        blur1D(*channel, false);
    }

    for (size_t i = 0; i < n; ++i)
    {
        float a = std::clamp(pa[i], 0.0f, 1.0f);
        buf[i * 4 + 3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        if (a > 1e-4f)
        {
            buf[i * 4 + 0] = static_cast<unsigned char>(std::clamp(pr[i] / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            buf[i * 4 + 1] = static_cast<unsigned char>(std::clamp(pg[i] / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            buf[i * 4 + 2] = static_cast<unsigned char>(std::clamp(pb[i] / a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        else
        {
            buf[i * 4 + 0] = buf[i * 4 + 1] = buf[i * 4 + 2] = 0;
        }
    }
}

// Rasterizes just `shape` (temporarily detached from the rest of the
// shape list) at 1:1 scale into an RGBA8 buffer sized to its bounds plus a
// 3-sigma margin, then Gaussian-blurs it - the path taken for any shape
// whose SVG filter wiv's flat-triangle vector renderer can't otherwise
// express. Uses nanosvgrast rather than the flat-average-color
// approximation elsewhere in this file, so a blurred *gradient* shape (the
// bottle glass highlight, here) still renders its true gradient, not just
// its blur.
bool rasterizeBlurredShape(NSVGshape* shape, float stdDev, Dataset& out)
{
    const float margin = std::max(4.0f, stdDev * 3.0f);
    float x0 = shape->bounds[0] - margin;
    float y0 = shape->bounds[1] - margin;
    float x1 = shape->bounds[2] + margin;
    float y1 = shape->bounds[3] + margin;
    int w = static_cast<int>(std::ceil(x1 - x0));
    int h = static_cast<int>(std::ceil(y1 - y0));
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

    NSVGshape* savedNext = shape->next;
    shape->next = nullptr; // isolate - rasterize only this one shape

    NSVGimage tmpImage;
    std::memset(&tmpImage, 0, sizeof(tmpImage));
    tmpImage.width = static_cast<float>(w);
    tmpImage.height = static_cast<float>(h);
    tmpImage.shapes = shape;

    std::vector<unsigned char> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    bool ok = rast != nullptr;
    if (ok) nsvgRasterize(rast, &tmpImage, -x0, -y0, 1.0f, buf.data(), w, h, w * 4);
    if (rast) nsvgDeleteRasterizer(rast);

    shape->next = savedNext;
    if (!ok) return false;

    gaussianBlurRGBA(buf, w, h, stdDev);

    out.isSprite = true;
    out.spriteRGBA = std::move(buf);
    out.spriteWidth = w;
    out.spriteHeight = h;
    out.spriteX = x0;
    out.spriteY = y0;
    out.spriteW = w;
    out.spriteH = h;
    return true;
}

} // namespace

bool loadSvgFile(const std::string& filename, SceneData& scene, std::string& error)
{
    NSVGimage* svg = nsvgParseFromFile(filename.c_str(), "px", 96.0f);
    if (!svg)
    {
        error = "could not parse svg file: " + filename;
        return false;
    }

    std::unordered_map<std::string, float> elementBlur;
    {
        std::ifstream in(filename, std::ios::binary);
        if (in)
        {
            std::ostringstream ss;
            ss << in.rdbuf();
            elementBlur = parseElementBlurRadii(ss.str());
        }
    }

    int shapeIndex = 0;
    for (NSVGshape* shape = svg->shapes; shape; shape = shape->next, ++shapeIndex)
    {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        if (!shape->paths) continue;

        auto blurIt = shape->id[0] ? elementBlur.find(shape->id) : elementBlur.end();
        if (blurIt != elementBlur.end() && blurIt->second > 0.0f)
        {
            scene.datasets.emplace_back();
            Dataset& ds = scene.datasets.back();
            ds.pathName = shape->id;
            ds.doDrawLines = false;
            ds.doDrawPolygon = false;
            ds.doDrawMarks = false;
            if (rasterizeBlurredShape(shape, blurIt->second, ds))
            {
                scene.updateBounds(ds.spriteX, ds.spriteY);
                scene.updateBounds(ds.spriteX + ds.spriteW, ds.spriteY + ds.spriteH);
            }
            else
            {
                scene.datasets.pop_back(); // rasterization failed - drop rather than leave an empty dataset
            }
            continue;
        }

        const bool hasFill = shape->fill.type == NSVG_PAINT_COLOR || isGradient(shape->fill.type);
        const bool hasStroke = shape->stroke.type == NSVG_PAINT_COLOR || isGradient(shape->stroke.type);

        if (hasFill)
        {
            scene.datasets.emplace_back();
            Dataset& ds = scene.datasets.back();
            ds.pathName = shape->id[0] ? shape->id : ("svg shape " + std::to_string(shapeIndex));
            ds.color = paintColor(shape->fill, shape->opacity);
            ds.doDrawLines = false;
            ds.doDrawPolygon = true;
            ds.doDrawMarks = false;
            for (NSVGpath* path = shape->paths; path; path = path->next) appendPath(ds, scene, path);
        }

        if (hasStroke)
        {
            scene.datasets.emplace_back();
            Dataset& ds = scene.datasets.back();
            ds.pathName = shape->id[0] ? shape->id : ("svg shape " + std::to_string(shapeIndex));
            ds.color = paintColor(shape->stroke, shape->opacity);
            ds.lineWidth = shape->strokeWidth > 0 ? shape->strokeWidth : 1.0;
            ds.lineCap = shape->strokeLineCap; // NSVG_CAP_* matches giv's line_cap encoding
            ds.doDrawLines = true;
            ds.doDrawPolygon = false;
            ds.doDrawMarks = false;
            for (NSVGpath* path = shape->paths; path; path = path->next) appendPath(ds, scene, path);
        }

        // Shapes with neither a fill nor a stroke paint produce no dataset.
    }

    nsvgDelete(svg);
    return true;
}

} // namespace giv
