// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "SvgLoader.h"
#include "ImagePluginHost.h"
#include "PathUtils.h"
#include "SvgAnimation.h"
#include "SvgCss.h"
#include "SvgDom.h"
#include "SvgPath.h"
#include "TextLayout.h"

#define NANOSVG_ALL_COLOR_KEYWORDS // all CSS color names, not just the 22 basic ones
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

// Static: the stbimage/ico plugins carry their own copies of stb_image.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
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
// rasterizeShapeSprite, which rasterizes the true gradient via nanosvgrast.)
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

// stroke-dasharray: nanosvg parses the pattern but the line renderer has no
// notion of dashes, so a dashed stroke is cut into separate polyline pieces
// here (curves flattened first). Dash lengths are in the path's own units.
void appendDashedPath(Dataset& ds, SceneData& scene, const NSVGpath* path, const float* dashArray, int dashCount,
                      float dashOffset)
{
    if (path->npts < 1) return;
    std::vector<double> pat(dashArray, dashArray + dashCount);
    if (pat.size() % 2) pat.insert(pat.end(), pat.begin(), pat.end());
    double sum = 0;
    for (double& d : pat)
    {
        d = std::max(0.0, d);
        sum += d;
    }
    if (sum <= 1e-9) return appendPath(ds, scene, path);

    // Flatten to a polyline.
    std::vector<std::array<double, 2>> poly;
    const float* pts = path->pts;
    poly.push_back({pts[0], pts[1]});
    for (int seg = 0; (seg * 3 + 3) < path->npts; ++seg)
    {
        const float* p0 = pts + seg * 3 * 2;
        const float* p1 = p0 + 2;
        const float* p2 = p0 + 4;
        const float* p3 = p0 + 6;
        const double ctrl = std::hypot(p1[0] - p0[0], p1[1] - p0[1]) + std::hypot(p2[0] - p1[0], p2[1] - p1[1]) +
                            std::hypot(p3[0] - p2[0], p3[1] - p2[1]);
        const int steps = std::clamp(static_cast<int>(ctrl / 2.0), 4, 200);
        for (int i = 1; i <= steps; ++i)
        {
            const double t = double(i) / steps, u = 1 - t;
            poly.push_back({u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                            u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]});
        }
    }
    if (path->closed && (poly.back()[0] != poly[0][0] || poly.back()[1] != poly[0][1])) poly.push_back(poly[0]);

    // Where in the pattern the path starts.
    double off = std::fmod(static_cast<double>(dashOffset), sum);
    if (off < 0) off += sum;
    size_t idx = 0;
    while (off >= pat[idx])
    {
        off -= pat[idx];
        idx = (idx + 1) % pat.size();
    }
    double remain = pat[idx] - off;
    bool on = idx % 2 == 0;
    auto emit = [&](double x, double y, Op op) {
        ds.addPoint(static_cast<float>(x), static_cast<float>(y), op);
        scene.updateBounds(x, y);
    };
    double startX = poly[0][0], startY = poly[0][1]; // where the current dash began
    const double dot = std::max(1e-3, ds.lineWidth * 0.02);
    if (on) emit(poly[0][0], poly[0][1], Op::Move);
    for (size_t i = 1; i < poly.size(); ++i)
    {
        double x0 = poly[i - 1][0], y0 = poly[i - 1][1];
        const double x1 = poly[i][0], y1 = poly[i][1];
        double len = std::hypot(x1 - x0, y1 - y0);
        while (len > remain)
        {
            const double f = remain / len;
            x0 += (x1 - x0) * f;
            y0 += (y1 - y0) * f;
            len -= remain;
            if (on)
            {
                // A zero-length dash (round-capped dots) still needs some direction to draw along.
                if (x0 == startX && y0 == startY) emit(x0 + (x1 - x0) / (len + remain) * dot, y0 + (y1 - y0) / (len + remain) * dot, Op::Draw);
                else emit(x0, y0, Op::Draw);
            }
            on = !on;
            idx = (idx + 1) % pat.size();
            remain = pat[idx];
            if (on)
            {
                emit(x0, y0, Op::Move);
                startX = x0;
                startY = y0;
            }
        }
        remain -= len;
        if (on) emit(x1, y1, Op::Draw);
    }
}

// nanosvg has no support at all for SVG <filter>/feGaussianBlur - it's a
// pure geometry parser. This is NOT a general CSS parser either: it
// recognizes just enough of Inkscape's usual drop-shadow/soft-highlight
// export shape (a <filter id="X"> element containing a <feGaussianBlur
// stdDeviation="D"/>, referenced by some element's own style="...
// filter:url(#X)...") to resolve elementId -> blur stdDeviation, so
// loadSvgFile can rasterize+blur just those shapes (see
// rasterizeShapeSprite) instead of silently dropping the effect.
const SvgNode* findDescendant(const SvgNode& n, const char* name)
{
    for (const auto& c : n.children)
    {
        if (c->name == name) return c.get();
        if (const SvgNode* d = findDescendant(*c, name)) return d;
    }
    return nullptr;
}

void collectFilterStdDev(const SvgNode& n, std::unordered_map<std::string, float>& out)
{
    if (n.name == "filter" && !n.attr("id").empty())
    {
        if (const SvgNode* blur = findDescendant(n, "feGaussianBlur"))
        {
            const std::string& sd = blur->attr("stdDeviation");
            if (!sd.empty())
            {
                try
                {
                    out[n.attr("id")] = std::stof(sd);
                }
                catch (...)
                {
                }
            }
        }
    }
    for (const auto& c : n.children) collectFilterStdDev(*c, out);
}

void collectElementBlur(const SvgNode& n, const std::unordered_map<std::string, float>& filterStdDev,
                        std::unordered_map<std::string, float>& out)
{
    const std::string& id = n.attr("id");
    const std::string& style = n.attr("style");
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
                if (it != filterStdDev.end()) out[id] = it->second;
            }
        }
    }
    for (const auto& c : n.children) collectElementBlur(*c, filterStdDev, out);
}

std::unordered_map<std::string, float> parseElementBlurRadii(const SvgNode& root)
{
    std::unordered_map<std::string, float> filterStdDev, elementBlur;
    collectFilterStdDev(root, filterStdDev);
    collectElementBlur(root, filterStdDev, elementBlur);
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
// shape list), fill and stroke together, into an RGBA8 sprite covering its
// bounds plus a margin (room for the stroke and, if stdDev > 0, 3 sigma of
// Gaussian blur). Used for any shape the flat-triangle vector renderer
// can't express: SVG blur filters and gradient paints. nanosvgrast renders
// the true gradient. `pixelsPerUnit` oversamples the sprite so it stays
// reasonably sharp when zoomed in; the sprite's world size is unaffected.
bool rasterizeShapeSprite(NSVGshape* shape, float stdDev, float pixelsPerUnit, Dataset& out)
{
    const float margin = std::max(2.0f + 0.5f * shape->strokeWidth, stdDev * 3.0f);
    float x0 = shape->bounds[0] - margin;
    float y0 = shape->bounds[1] - margin;
    float x1 = shape->bounds[2] + margin;
    float y1 = shape->bounds[3] + margin;
    float worldW = x1 - x0, worldH = y1 - y0;
    int w = static_cast<int>(std::ceil(worldW * pixelsPerUnit));
    int h = static_cast<int>(std::ceil(worldH * pixelsPerUnit));
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

    NSVGshape* savedNext = shape->next;
    shape->next = nullptr; // isolate - rasterize only this one shape

    NSVGimage tmpImage;
    std::memset(&tmpImage, 0, sizeof(tmpImage));
    tmpImage.width = static_cast<float>(w);
    tmpImage.height = static_cast<float>(h);
    tmpImage.shapes = shape;

    auto buf = std::make_shared<std::vector<unsigned char>>(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    bool ok = rast != nullptr;
    if (ok) nsvgRasterize(rast, &tmpImage, -x0 * pixelsPerUnit, -y0 * pixelsPerUnit, pixelsPerUnit, buf->data(), w, h, w * 4);
    if (rast) nsvgDeleteRasterizer(rast);

    shape->next = savedNext;
    if (!ok) return false;

    gaussianBlurRGBA(*buf, w, h, stdDev * pixelsPerUnit);

    out.isSprite = true;
    out.spriteRGBA = std::move(buf);
    out.spriteWidth = w;
    out.spriteHeight = h;
    out.spriteX = x0;
    out.spriteY = y0;
    out.spriteW = worldW;
    out.spriteH = worldH;
    return true;
}

// Oversampling for gradient sprites: up to 4x, but keeping the larger side
// of the sprite within ~1024 pixels so memory stays modest.
float spriteOversampling(const NSVGshape* shape)
{
    float extent = std::max(shape->bounds[2] - shape->bounds[0], shape->bounds[3] - shape->bounds[1]);
    if (extent <= 0.0f) return 1.0f;
    return std::clamp(1024.0f / extent, 1.0f, 4.0f);
}

// ---------------------------------------------------------------------------
// <image> elements. nanosvg ignores them, so they are found with the same
// kind of minimal tag scan as the blur filters above and added as sprite
// Datasets. Handles data: URIs (base64 or percent-encoded) and file
// hrefs relative to the svg, x/y/width/height, preserveAspectRatio, opacity,
// and translate/scale/matrix transforms on the image and its <g> ancestors
// (a rotated/skewed image is drawn as its axis-aligned bounding box).
// ---------------------------------------------------------------------------
struct Affine
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    // this * m (m is applied first)
    Affine operator*(const Affine& m) const
    {
        return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d,
                b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
    }
};

std::vector<double> parseNumbers(const std::string& s)
{
    std::vector<double> v;
    const char* p = s.c_str();
    char* endp = nullptr;
    while (*p)
    {
        double x = std::strtod(p, &endp);
        if (endp == p) { ++p; continue; }
        v.push_back(x);
        p = endp;
    }
    return v;
}

Affine parseTransform(const std::string& str)
{
    Affine t;
    size_t pos = 0;
    while (true)
    {
        size_t open = str.find('(', pos);
        if (open == std::string::npos) break;
        size_t close = str.find(')', open);
        if (close == std::string::npos) break;
        size_t nb = open;
        while (nb > pos && std::isspace(static_cast<unsigned char>(str[nb - 1]))) --nb;
        size_t ns = nb;
        while (ns > pos && std::isalpha(static_cast<unsigned char>(str[ns - 1]))) --ns;
        std::string name = str.substr(ns, nb - ns);
        auto n = parseNumbers(str.substr(open + 1, close - open - 1));
        Affine m;
        if (name == "matrix" && n.size() >= 6) m = {n[0], n[1], n[2], n[3], n[4], n[5]};
        else if (name == "translate" && !n.empty()) { m.e = n[0]; m.f = n.size() > 1 ? n[1] : 0; }
        else if (name == "scale" && !n.empty()) { m.a = n[0]; m.d = n.size() > 1 ? n[1] : n[0]; }
        else if (name == "skewX" && !n.empty()) m.c = std::tan(n[0] * M_PI / 180.0);
        else if (name == "skewY" && !n.empty()) m.b = std::tan(n[0] * M_PI / 180.0);
        else if (name == "rotate" && !n.empty())
        {
            double r = n[0] * M_PI / 180.0, cs = std::cos(r), sn = std::sin(r);
            double cx = n.size() > 2 ? n[1] : 0, cy = n.size() > 2 ? n[2] : 0;
            m = {cs, sn, -sn, cs, cx - cs * cx + sn * cy, cy - sn * cx - cs * cy};
        }
        t = t * m;
        pos = close + 1;
    }
    return t;
}

double parseLength(const std::string& s, double def = 0.0)
{
    if (s.empty()) return def;
    auto n = parseNumbers(s);
    return n.empty() ? def : n[0];
}


// ---------------------------------------------------------------------------
// <image> support: href decoding (data: URIs and files) and sprite creation
// ---------------------------------------------------------------------------
int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2])));
            i += 2;
        }
        else out.push_back(s[i]);
    }
    return out;
}

// Lenient base64: skips whitespace (Inkscape wraps lines, sometimes as "&#10;"
// which expat has already turned into a newline) and stops at padding.
std::vector<unsigned char> base64Decode(const std::string& s)
{
    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    unsigned acc = 0;
    int bits = 0;
    for (char c : s)
    {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

struct RgbaImage
{
    int width = 0, height = 0;
    std::vector<unsigned char> rgba; // width*height*4, top-to-bottom, straight alpha
    bool empty() const { return rgba.empty(); }
};

bool decodeBytes(const unsigned char* data, size_t size, RgbaImage& out)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!px) return false;
    out.width = w;
    out.height = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

// "file:///a/b%20c.png" -> "/a/b c.png"
std::string fileUrlToPath(const std::string& url)
{
    std::string p = url.substr(5); // after "file:"
    if (p.compare(0, 2, "//") == 0)
    {
        size_t slash = p.find('/', 2);
        p = slash == std::string::npos ? std::string() : p.substr(slash);
    }
#ifdef _WIN32
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p.erase(0, 1); // "/C:/x" -> "C:/x"
#endif
    return percentDecode(p);
}

bool decodeImageHref(std::string href, const std::string& absref, const std::string& svgFilename, RgbaImage& out)
{
    if (href.compare(0, 5, "data:") == 0)
    {
        size_t comma = href.find(',');
        if (comma == std::string::npos) return false;
        const std::string meta = href.substr(5, comma - 5);
        const std::string payload = href.substr(comma + 1);
        std::vector<unsigned char> bytes;
        if (meta.find(";base64") != std::string::npos) bytes = base64Decode(payload);
        else
        {
            const std::string d = percentDecode(payload);
            bytes.assign(d.begin(), d.end());
        }
        return !bytes.empty() && decodeBytes(bytes.data(), bytes.size(), out);
    }
    // Candidate paths, in order: the href as written (resolved against the
    // svg's directory), then percent-decoded, then Inkscape's absref.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path baseDir = fs::absolute(giv::utf8ToPath(svgFilename), ec).parent_path();
    auto resolve = [&](std::string path)
    {
        if (path.compare(0, 5, "file:") == 0) path = fileUrlToPath(path);
        fs::path p = giv::utf8ToPath(path);
        return p.is_relative() ? baseDir / p : p;
    };
    std::vector<fs::path> candidates{resolve(href), resolve(percentDecode(href))};
    if (!absref.empty()) candidates.push_back(resolve(absref));
    for (const fs::path& c : candidates)
    {
        if (!fs::is_regular_file(c, ec)) continue;
        // The image plugins cover far more formats than stb (WebP, HEIF, ...).
        if (auto loaded = ImagePluginHost::load(giv::pathToUtf8(c)); loaded && !loaded->rgba.empty())
        {
            out.width = loaded->width;
            out.height = loaded->height;
            out.rgba.assign(loaded->rgba.data(), loaded->rgba.data() + loaded->rgba.size());
            return true;
        }
    }
    return false;
}

// Decoded <image> pictures by href, so an animation doesn't decode them again every frame.
using SvgImageCache = std::unordered_map<std::string, RgbaImage>;

struct ImageWalker
{
    const std::string& svgFilename;
    SvgImageCache* cache;
    SceneData& scene;
    int index = 0;

    void addImage(const SvgNode& el, const Affine& parent)
    {
        std::string href = el.attr("xlink:href");
        if (href.empty()) href = el.attr("href");
        const std::string& style = el.attr("style");
        if (style.find("display:none") != std::string::npos || el.attr("display") == "none") return;

        RgbaImage img;
        if (href.empty()) return;
        auto cached = cache ? cache->find(href) : SvgImageCache::iterator();
        if (cache && cached != cache->end()) img = cached->second;
        else
        {
            if (!decodeImageHref(href, el.attr("sodipodi:absref"), svgFilename, img) || img.empty()) return;
            if (cache) (*cache)[href] = img;
        }

        double x = parseLength(el.attr("x")), y = parseLength(el.attr("y"));
        double w = parseLength(el.attr("width"), img.width);
        double h = parseLength(el.attr("height"), img.height);
        if (w <= 0 || h <= 0) return;

        const std::string& par = el.attr("preserveAspectRatio");
        if (par.find("none") == std::string::npos)
        {
            double s = std::min(w / img.width, h / img.height);
            double iw = img.width * s, ih = img.height * s;
            bool slice = par.find("slice") != std::string::npos;
            if (slice) { s = std::max(w / img.width, h / img.height); iw = img.width * s; ih = img.height * s; }
            double fx = par.find("xMin") != std::string::npos ? 0.0 : par.find("xMax") != std::string::npos ? 1.0 : 0.5;
            double fy = par.find("YMin") != std::string::npos ? 0.0 : par.find("YMax") != std::string::npos ? 1.0 : 0.5;
            x += (w - iw) * fx;
            y += (h - ih) * fy;
            w = iw;
            h = ih;
            // slice would need clipping to the viewport; show the whole image instead.
        }

        Affine m = parent * parseTransform(el.attr("transform"));
        double px[4] = {x, x + w, x, x + w}, py[4] = {y, y, y + h, y + h};
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (int i = 0; i < 4; ++i)
        {
            double tx = m.a * px[i] + m.c * py[i] + m.e;
            double ty = m.b * px[i] + m.d * py[i] + m.f;
            minX = std::min(minX, tx); maxX = std::max(maxX, tx);
            minY = std::min(minY, ty); maxY = std::max(maxY, ty);
        }

        double opacity = parseLength(el.attr("opacity"), 1.0);
        size_t op = style.find("opacity:");
        if (op != std::string::npos && (op == 0 || style[op - 1] == ';' || style[op - 1] == ' '))
            opacity = parseLength(style.substr(op + 8), opacity);
        if (opacity < 1.0)
        {
            for (size_t i = 3; i < img.rgba.size(); i += 4)
                img.rgba[i] = static_cast<unsigned char>(img.rgba[i] * std::max(0.0, opacity));
        }

        scene.datasets.emplace_back();
        Dataset& ds = scene.datasets.back();
        const std::string& id = el.attr("id");
        ds.pathName = id.empty() ? "svg image " + std::to_string(index) : id;
        ++index;
        ds.doDrawLines = false;
        ds.doDrawPolygon = false;
        ds.doDrawMarks = false;
        ds.isSprite = true;
        ds.spriteWidth = img.width;
        ds.spriteHeight = img.height;
        ds.spriteRGBA = std::make_shared<const std::vector<unsigned char>>(std::move(img.rgba));
        ds.spriteX = minX;
        ds.spriteY = minY;
        ds.spriteW = maxX - minX;
        ds.spriteH = maxY - minY;
        scene.updateBounds(minX, minY);
        scene.updateBounds(maxX, maxY);
    }

    void walk(const SvgNode& node, const Affine& m)
    {
        for (const auto& c : node.children)
        {
            const std::string& n = c->name;
            if (n == "g" || n == "a" || n == "switch" || n == "svg") walk(*c, m * parseTransform(c->attr("transform")));
            else if (n == "image") addImage(*c, m);
        }
    }
};

// User units -> the pixel space nanosvg's shapes are in (see nsvgParse): the
// viewBox is mapped onto width/height, xMidYMid meet.
Affine rootAffine(const SvgNode& root, const NSVGimage* svg)
{
    double scale = 1.0, offX = 0.0, offY = 0.0;
    auto vb = parseNumbers(root.attr("viewBox"));
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0 && svg->width > 0 && svg->height > 0)
    {
        scale = std::min(svg->width / vb[2], svg->height / vb[3]);
        offX = -vb[0] * scale + (svg->width - vb[2] * scale) * 0.5;
        offY = -vb[1] * scale + (svg->height - vb[3] * scale) * 0.5;
    }
    return Affine{scale, 0, 0, scale, offX, offY};
}

void addSvgImages(const SvgNode& root, const std::string& svgFilename, const NSVGimage* svg, SvgImageCache* cache, SceneData& scene)
{
    // The root element's own transform (if any) is not applied by nanosvg either.
    ImageWalker walker{svgFilename, cache, scene};
    walker.walk(root, rootAffine(root, svg));
}

// ---------------------------------------------------------------------------
// <text> support. nanosvg ignores text, so it is read from the DOM here, laid
// out with TextLayout (fontconfig + FriBidi + HarfBuzz) and drawn as filled
// glyph outlines, which makes it scale, rotate and skew like any other shape.
// Supported: x/y (lists use the first), dx/dy, text-anchor, direction, fill,
// fill-opacity/opacity, font-family/size/weight/style, letter-spacing,
// xml:lang, transform, <tspan> (styled pieces flow along the line; x/y/dy start
// new chunks, each anchored as a whole), textLength/lengthAdjust and <textPath>.
// ---------------------------------------------------------------------------
struct TextStyle
{
    double r = 0, g = 0, b = 0, alpha = 1;
    bool fillNone = false;
    bool strokeNone = true;
    double sr = 0, sg = 0, sb = 0, strokeAlpha = 1, strokeWidth = 1;
    double fontPx = 16.0;
    std::string family = "Sans";
    int weight = 400;
    bool italic = false;
    double letterSpacing = 0.0;
    bool rtl = false;
    std::string lang;
    int anchorCol = 0; // 0 start, 1 middle, 2 end
    bool hidden = false;
};

// "style" declarations and presentation attributes of `el`; style wins.
std::string cssProp(const SvgNode& el, const char* name)
{
    const std::string& style = el.attr("style");
    size_t pos = 0;
    const std::string key(name);
    while (pos < style.size())
    {
        size_t semi = style.find(';', pos);
        std::string decl = style.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t colon = decl.find(':');
        if (colon != std::string::npos)
        {
            auto trimmed = [](std::string t) {
                size_t a = t.find_first_not_of(" \t\n\r"), b = t.find_last_not_of(" \t\n\r");
                return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
            };
            if (trimmed(decl.substr(0, colon)) == key) return trimmed(decl.substr(colon + 1));
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return el.attr(name);
}

TextStyle applyTextStyle(TextStyle st, const SvgNode& el)
{
    std::string v = cssProp(el, "fill");
    if (v == "none") st.fillNone = true;
    else if (!v.empty() && parseSvgColor(v, st.r, st.g, st.b)) st.fillNone = false;
    v = cssProp(el, "stroke");
    if (v == "none") st.strokeNone = true;
    else if (!v.empty() && parseSvgColor(v, st.sr, st.sg, st.sb)) st.strokeNone = false;
    v = cssProp(el, "stroke-width");
    if (!v.empty()) st.strokeWidth = std::max(0.0, parseLength(v, st.strokeWidth));
    v = cssProp(el, "stroke-opacity");
    if (!v.empty()) st.strokeAlpha *= std::clamp(parseLength(v, 1.0), 0.0, 1.0);
    // opacity multiplies down the tree; fill-opacity is not inherited as a product but close enough.
    for (const char* k : {"opacity", "fill-opacity"})
    {
        v = cssProp(el, k);
        if (!v.empty()) st.alpha *= std::clamp(parseLength(v, 1.0), 0.0, 1.0);
    }
    v = cssProp(el, "font-size");
    if (!v.empty())
    {
        double n = parseLength(v, st.fontPx);
        if (v.find("pt") != std::string::npos) n *= 96.0 / 72.0;
        else if (v.find("em") != std::string::npos) n *= st.fontPx;
        else if (v.find('%') != std::string::npos) n = st.fontPx * n / 100.0;
        if (n > 0) st.fontPx = n;
    }
    v = cssProp(el, "font-family");
    if (!v.empty())
    {
        std::string fam = v.substr(0, v.find(','));
        fam.erase(std::remove(fam.begin(), fam.end(), '\''), fam.end());
        fam.erase(std::remove(fam.begin(), fam.end(), '"'), fam.end());
        size_t a = fam.find_first_not_of(" \t"), b = fam.find_last_not_of(" \t");
        fam = a == std::string::npos ? std::string() : fam.substr(a, b - a + 1);
        if (fam == "sans-serif") fam = "Sans";
        else if (fam == "serif") fam = "Serif";
        else if (fam == "monospace") fam = "Monospace";
        if (!fam.empty()) st.family = fam;
    }
    v = cssProp(el, "font-weight");
    if (!v.empty())
    {
        if (v == "bold" || v == "bolder") st.weight = 700;
        else if (v == "lighter") st.weight = 300;
        else if (v == "normal") st.weight = 400;
        else st.weight = static_cast<int>(std::clamp(parseLength(v, 400), 1.0, 1000.0));
    }
    v = cssProp(el, "letter-spacing");
    if (!v.empty() && v != "normal")
    {
        double n = parseLength(v, 0.0);
        if (v.find("em") != std::string::npos) n *= st.fontPx;
        st.letterSpacing = n;
    }
    v = cssProp(el, "direction");
    if (!v.empty()) st.rtl = v == "rtl";
    if (el.hasAttr("xml:lang")) st.lang = el.attr("xml:lang");
    else if (el.hasAttr("lang")) st.lang = el.attr("lang");
    v = cssProp(el, "font-style");
    if (!v.empty()) st.italic = v == "italic" || v == "oblique";
    v = cssProp(el, "text-anchor");
    if (v == "middle") st.anchorCol = 1;
    else if (v == "end") st.anchorCol = 2;
    else if (v == "start") st.anchorCol = 0;
    if (cssProp(el, "display") == "none" || cssProp(el, "visibility") == "hidden") st.hidden = true;
    return st;
}

// A path's points through an affine map (the cumulative lengths follow).
MotionPath transformedPath(const MotionPath& in, const Affine& m)
{
    MotionPath out;
    for (size_t i = 0; i < in.pts.size(); ++i)
    {
        const double x = in.pts[i][0], y = in.pts[i][1];
        const bool jump = i > 0 && in.cum[i] == in.cum[i - 1] && (x != in.pts[i - 1][0] || y != in.pts[i - 1][1]);
        out.add(m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f, jump);
    }
    return out;
}

struct TextWalker
{
    SceneData& scene;
    const SvgNode* root = nullptr;
    int index = 0;

    // textLength / lengthAdjust of an element: the text it covers is stretched to `length`.
    struct LengthAdj
    {
        double length = 0;
        bool glyphs = false; // lengthAdjust="spacingAndGlyphs" (else only the spacing changes)
    };

    // A stretch of character data in one style. Pieces flow along the line; a
    // piece with `hasX` starts a new chunk, which is what text-anchor applies to.
    struct Piece
    {
        std::string text;
        TextStyle style;
        bool hasX = false;
        double x = 0, y = 0, dx = 0;
        int group = -1;                // index into the LengthAdj list, or -1
        const SvgNode* path = nullptr; // <textPath> target
        std::string startOffset;
    };

    struct Cursor
    {
        double x = 0, y = 0, dx = 0;
        bool hasX = true;
    };

    struct Context
    {
        std::vector<Piece> pieces;
        std::vector<LengthAdj> groups;
        bool space = true; // the last character emitted was a collapsed space
    };

    static double firstNumber(const std::string& l, double d)
    {
        auto n = parseNumbers(l);
        return n.empty() ? d : n[0];
    }

    const SvgNode* findById(const SvgNode& n, const std::string& id) const
    {
        if (n.attr("id") == id) return &n;
        for (const auto& c : n.children)
            if (const SvgNode* r = findById(*c, id)) return r;
        return nullptr;
    }

    // XML whitespace handling: runs of white space become one space, and one that
    // follows another (even across tspans) disappears.
    static std::string collapseInto(const std::string& in, bool& space)
    {
        std::string out;
        for (char ch : in)
        {
            if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r')
            {
                if (!space) out += ' ';
                space = true;
            }
            else
            {
                out += ch;
                space = false;
            }
        }
        return out;
    }

    int openGroup(const SvgNode& el, int group, Context& ctx)
    {
        if (!el.hasAttr("textLength")) return group;
        LengthAdj g;
        g.length = parseLength(el.attr("textLength"), 0.0);
        g.glyphs = el.attr("lengthAdjust") == "spacingAndGlyphs";
        if (g.length <= 0) return group;
        ctx.groups.push_back(g);
        return static_cast<int>(ctx.groups.size()) - 1;
    }

    // Appends the character data of `el` (and its tspans) as pieces.
    void collect(const SvgNode& el, const TextStyle& st, int group, Cursor& cur, Context& ctx, const SvgNode* path,
                 const std::string& startOffset)
    {
        for (const auto& c : el.children)
        {
            if (c->name == "#text")
            {
                std::string t = collapseInto(c->text, ctx.space);
                if (t.empty()) continue;
                Piece p;
                p.text = std::move(t);
                p.style = st;
                p.hasX = cur.hasX;
                p.x = cur.x;
                p.y = cur.y;
                p.dx = cur.dx;
                p.group = group;
                p.path = path;
                p.startOffset = startOffset;
                cur.hasX = false;
                cur.dx = 0;
                ctx.pieces.push_back(std::move(p));
            }
            else if (c->name == "tspan" || c->name == "a" || c->name == "textPath")
            {
                const bool isPath = c->name == "textPath";
                if (!isPath && c->hasAttr("x"))
                {
                    cur.x = firstNumber(c->attr("x"), cur.x);
                    cur.hasX = true;
                }
                if (!isPath && c->hasAttr("y")) cur.y = firstNumber(c->attr("y"), cur.y);
                cur.dx += firstNumber(c->attr("dx"), 0.0);
                cur.y += firstNumber(c->attr("dy"), 0.0);
                const TextStyle inner = applyTextStyle(st, *c);
                const SvgNode* innerPath = path;
                std::string innerOffset = startOffset;
                if (isPath)
                {
                    const std::string& h = c->hasAttr("xlink:href") ? c->attr("xlink:href") : c->attr("href");
                    innerPath = root && h.size() > 1 && h[0] == '#' ? findById(*root, h.substr(1)) : nullptr;
                    innerOffset = c->attr("startOffset");
                    cur.hasX = true;
                }
                collect(*c, inner, openGroup(*c, group, ctx), cur, ctx, innerPath, innerOffset);
                if (isPath) cur.hasX = true; // text after the path starts a chunk of its own
            }
        }
    }

    struct Placed
    {
        const Piece* piece = nullptr;
        TextOutline outline;
        std::vector<Affine> xf; // per glyph: outline coordinates -> user space of the text element
        std::vector<bool> skip; // per glyph: off the end of its path
        double x = 0;           // start from the chunk origin (path-less pieces)
        double xs = 1, extra = 0; // textLength: glyph stretch and added spacing
        double advance() const { return outline.advance * xs + extra * outline.glyphEnd.size(); }
    };

    // Lays out one run of pieces that share an origin (or one <textPath> piece).
    void placeChunk(std::vector<Placed>& chunk, const Context& ctx)
    {
        if (chunk.empty()) return;
        const Piece& first = *chunk[0].piece;
        double anchor = first.style.anchorCol == 1 ? 0.5 : (first.style.anchorCol == 2 ? 1.0 : 0.0);
        if (first.style.rtl) anchor = 1.0 - anchor;

        // textLength: stretch each group to its length, from the natural advances.
        for (size_t gi = 0; gi < ctx.groups.size(); ++gi)
        {
            double natural = 0;
            size_t glyphs = 0;
            for (const Placed& p : chunk)
                if (p.piece->group == static_cast<int>(gi))
                {
                    natural += p.outline.advance + p.piece->dx;
                    glyphs += p.outline.glyphEnd.size();
                }
            if (natural <= 0) continue;
            const LengthAdj& adj = ctx.groups[gi];
            for (Placed& p : chunk)
            {
                if (p.piece->group != static_cast<int>(gi)) continue;
                if (adj.glyphs) p.xs = adj.length / natural;
                else if (glyphs > 1) p.extra = (adj.length - natural) / static_cast<double>(glyphs - 1);
            }
        }

        // Positions along the line.
        double pen = 0;
        for (Placed& p : chunk)
        {
            pen += p.piece->dx;
            p.x = pen;
            pen += p.advance();
        }
        const double total = pen;

        for (Placed& p : chunk)
        {
            const size_t n = p.outline.glyphEnd.size();
            p.xf.assign(n, Affine{});
            p.skip.assign(n, false);
            if (!p.piece->path)
            {
                const double baseX = first.x - total * anchor + p.x;
                for (size_t g = 0; g < n; ++g)
                    p.xf[g] = Affine{p.xs, 0, 0, 1, baseX + p.extra * static_cast<double>(g), p.piece->y};
                continue;
            }
            // <textPath>: each glyph sits on the path at its midpoint, turned to the tangent.
            const SvgNode& pn = *p.piece->path;
            MotionPath path = transformedPath(flattenPath(pn.attr("d")), parseTransform(pn.attr("transform")));
            double s0 = 0;
            const std::string& so = p.piece->startOffset;
            if (!so.empty()) s0 = so.back() == '%' ? parseLength(so, 0.0) * path.total() / 100.0 : parseLength(so, 0.0);
            s0 -= p.advance() * anchor;
            for (size_t g = 0; g < n; ++g)
            {
                const double ox = p.outline.glyphOx[g];
                const double nextOx = g + 1 < n ? p.outline.glyphOx[g + 1] : p.outline.advance;
                const double adv = std::abs(nextOx - ox);
                const double mid = (std::min(ox, nextOx) + adv / 2) * p.xs + p.extra * static_cast<double>(g);
                const double dist = s0 + mid;
                if (path.pts.size() < 2 || dist < 0 || dist > path.total())
                {
                    p.skip[g] = true;
                    continue;
                }
                double px, py, ang;
                path.atDistance(dist, px, py, ang);
                const double ca = std::cos(ang * M_PI / 180.0), sa = std::sin(ang * M_PI / 180.0);
                const double local = std::min(ox, nextOx) + adv / 2; // glyph midpoint in outline coordinates
                p.xf[g] = Affine{ca, sa, -sa, ca, px - ca * local, py - sa * local};
            }
        }
    }

    void emitChunk(const SvgNode& el, const std::vector<Placed>& chunk, const Affine& m)
    {
        for (const Placed& pl : chunk)
        {
            const TextOutline& outline = pl.outline;
            const TextStyle& st = pl.piece->style;
            // One dataset per run of glyphs: the fill step nests contours within a dataset.
            constexpr size_t kGlyphsPerDataset = 150;
            for (size_t g0 = 0; g0 < outline.glyphEnd.size(); g0 += kGlyphsPerDataset)
            {
                const size_t g1 = std::min(outline.glyphEnd.size(), g0 + kGlyphsPerDataset);
                const std::string& id = el.attr("id");
                const std::string name = id.empty() ? "svg text " + std::to_string(index) : id;
                ++index;
                // The same outline twice at most: SVG paints the fill, then the stroke over it.
                for (int pass = 0; pass < 2; ++pass)
                {
                    if (pass == 0 && st.fillNone) continue;
                    if (pass == 1 && st.strokeNone) continue;
                    scene.datasets.emplace_back();
                    Dataset& ds = scene.datasets.back();
                    ds.pathName = name;
                    ds.doDrawMarks = false;
                    if (pass == 0)
                    {
                        ds.color = Color::opaque(static_cast<float>(st.r / 255.0), static_cast<float>(st.g / 255.0),
                                                 static_cast<float>(st.b / 255.0), static_cast<float>(st.alpha));
                        ds.doDrawLines = false;
                        ds.doDrawPolygon = true;
                    }
                    else
                    {
                        ds.color = Color::opaque(static_cast<float>(st.sr / 255.0), static_cast<float>(st.sg / 255.0),
                                                 static_cast<float>(st.sb / 255.0), static_cast<float>(st.strokeAlpha));
                        ds.lineWidth = std::max(1e-3, st.strokeWidth * std::sqrt(std::abs(m.a * m.d - m.b * m.c)));
                        ds.lineCap = 1;
                        ds.doDrawLines = true;
                        ds.doDrawPolygon = false;
                    }
                    for (size_t g = g0; g < g1; ++g)
                    {
                        if (pl.skip[g]) continue;
                        const Affine full = m * pl.xf[g];
                        for (size_t pt = g == 0 ? 0 : outline.glyphEnd[g - 1]; pt < outline.glyphEnd[g]; ++pt)
                        {
                            if (outline.op[pt] == Op::ClosePath)
                            {
                                ds.addPoint(0, 0, Op::ClosePath);
                                continue;
                            }
                            const double wx = full.a * outline.x[pt] + full.c * outline.y[pt] + full.e;
                            const double wy = full.b * outline.x[pt] + full.d * outline.y[pt] + full.f;
                            ds.addPoint(static_cast<float>(wx), static_cast<float>(wy), outline.op[pt]);
                            scene.updateBounds(wx, wy);
                        }
                    }
                }
            }
        }
    }

    void addText(const SvgNode& el, const Affine& parent, const TextStyle& inherited)
    {
        TextStyle st = applyTextStyle(inherited, el);
        if (st.hidden || (st.fillNone && st.strokeNone)) return;
        const Affine m = parent * parseTransform(el.attr("transform"));

        // x/y/dx/dy may be lists; the first value positions the text.
        Context ctx;
        Cursor cur;
        cur.x = firstNumber(el.attr("x"), 0.0);
        cur.y = firstNumber(el.attr("y"), 0.0) + firstNumber(el.attr("dy"), 0.0);
        cur.dx = firstNumber(el.attr("dx"), 0.0);
        collect(el, st, openGroup(el, -1, ctx), cur, ctx, nullptr, {});
        // Trailing white space of the whole text is dropped.
        while (!ctx.pieces.empty())
        {
            std::string& t = ctx.pieces.back().text;
            if (!t.empty() && t.back() == ' ') t.pop_back();
            if (t.empty()) ctx.pieces.pop_back();
            else break;
        }

        std::vector<Placed> chunk;
        auto flush = [&] {
            placeChunk(chunk, ctx);
            emitChunk(el, chunk, m);
            chunk.clear();
        };
        for (const Piece& piece : ctx.pieces)
        {
            if (piece.style.hidden || (piece.style.fillNone && piece.style.strokeNone)) continue;
            if (piece.hasX || piece.path || (!chunk.empty() && chunk.back().piece->path)) flush();
            TextRequest req;
            req.text = piece.text;
            req.family = piece.style.family;
            req.weight = piece.style.weight;
            req.italic = piece.style.italic;
            req.sizePx = piece.style.fontPx;
            req.letterSpacing = piece.style.letterSpacing;
            req.rtlBase = piece.style.rtl;
            req.lang = piece.style.lang;
            Placed pl;
            pl.piece = &piece;
            if (!layoutText(req, pl.outline) || pl.outline.empty()) continue;
            chunk.push_back(std::move(pl));
        }
        flush();
    }

    void walk(const SvgNode& node, const Affine& m, const TextStyle& st)
    {
        for (const auto& c : node.children)
        {
            const std::string& n = c->name;
            if (n == "text") addText(*c, m, st);
            else if (n == "g" || n == "a" || n == "switch" || n == "svg")
            {
                TextStyle inner = applyTextStyle(st, *c);
                if (!inner.hidden) walk(*c, m * parseTransform(c->attr("transform")), inner);
            }
        }
    }
};

void addSvgTexts(const SvgNode& root, const NSVGimage* svg, SceneData& scene)
{
    TextWalker walker{scene};
    walker.root = &root;
    walker.walk(root, rootAffine(root, svg), applyTextStyle(TextStyle{}, root));
}

// If the root <svg> has a percentage width/height, replaces it by pixels:
// the viewBox size if there is one, else a default (800x600) viewport.
// Returns true if anything changed.
bool resolveRootPercentSize(SvgNode& root)
{
    bool changed = false;
    auto vb = parseNumbers(root.attr("viewBox"));
    const bool hasVb = vb.size() == 4 && vb[2] > 0 && vb[3] > 0;
    for (const char* name : {"width", "height"})
    {
        const std::string& v = root.attr(name);
        if (v.empty() || v.back() != '%') continue;
        const bool isW = name[0] == 'w';
        const double ref = hasVb ? vb[isW ? 2 : 3] : (isW ? 800.0 : 600.0);
        root.setAttr(name, std::to_string(parseLength(v, 100.0) * ref / 100.0));
        changed = true;
    }
    return changed;
}

// Everything after nanosvg's parse: the <image> sprites and one dataset per
// fill/stroke. `dom` (may be null) is the XML the NSVGimage was parsed from.
void populateScene(const NSVGimage* svgConst, const SvgNode* dom, const std::string& filename,
                   const std::unordered_map<std::string, float>& elementBlur, SvgImageCache* imageCache, SceneData& scene)
{
    NSVGimage* svg = const_cast<NSVGimage*>(svgConst);
    // Images first: nanosvg drops <image>, so their z-order relative to the
    // shapes is lost; the usual case is a picture annotated with shapes.
    if (dom) addSvgImages(*dom, filename, svg, imageCache, scene);

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
            if (rasterizeShapeSprite(shape, blurIt->second, 1.0f, ds))
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

        // A gradient fill or stroke can't be drawn by the flat-color vector
        // path (it would be reduced to an average color); rasterize the whole
        // shape so the gradient is rendered for real.
        if (isGradient(shape->fill.type) || isGradient(shape->stroke.type))
        {
            scene.datasets.emplace_back();
            Dataset& ds = scene.datasets.back();
            ds.pathName = shape->id[0] ? shape->id : ("svg shape " + std::to_string(shapeIndex));
            ds.doDrawLines = false;
            ds.doDrawPolygon = false;
            ds.doDrawMarks = false;
            if (rasterizeShapeSprite(shape, 0.0f, spriteOversampling(shape), ds))
            {
                scene.updateBounds(ds.spriteX, ds.spriteY);
                scene.updateBounds(ds.spriteX + ds.spriteW, ds.spriteY + ds.spriteH);
                continue;
            }
            scene.datasets.pop_back(); // too large to rasterize: fall back to the flat average color below
        }

        const bool hasFill = shape->fill.type == NSVG_PAINT_COLOR || isGradient(shape->fill.type);
        const bool hasStroke = shape->stroke.type == NSVG_PAINT_COLOR || isGradient(shape->stroke.type);

        if (hasFill)
        {
            scene.datasets.emplace_back();
            Dataset& ds = scene.datasets.back();
            ds.pathName = shape->id[0] ? shape->id : ("svg shape " + std::to_string(shapeIndex));
            ds.color = paintColor(shape->fill, shape->opacity);
            ds.fromSvg = true; // also marks the file as an SVG for the transparency backdrop (see SceneBuilder)
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
            ds.fromSvg = true;
            ds.lineCap = shape->strokeLineCap; // NSVG_CAP_* matches giv's line_cap encoding
            ds.doDrawLines = true;
            ds.doDrawPolygon = false;
            ds.doDrawMarks = false;
            for (NSVGpath* path = shape->paths; path; path = path->next)
            {
                if (shape->strokeDashCount > 0)
                    appendDashedPath(ds, scene, path, shape->strokeDashArray, shape->strokeDashCount, shape->strokeDashOffset);
                else appendPath(ds, scene, path);
            }
        }

        // Shapes with neither a fill nor a stroke paint produce no dataset.
    }

    // nanosvg ignores <text>, so text goes on top of the shapes, from the DOM.
    if (dom) addSvgTexts(*dom, svg, scene);
}

} // namespace

// Renders the frames of an animated SVG on demand: evaluate the SMIL
// animations at the frame's time, serialize the resulting DOM, and run it
// through nanosvg again. A few recent frames are kept.
namespace
{
class SvgFrameProvider : public FrameProvider
{
public:
    SvgFrameProvider(std::unique_ptr<SvgNode> dom, std::string filename, double fps)
        : dom_(std::move(dom)), anim_(*dom_), filename_(std::move(filename)), fps_(fps)
    {
        elementBlur_ = parseElementBlurRadii(*dom_);
    }

    const SvgAnimation& animation() const { return anim_; }

    // Frame `frame` as a scene: datasets tagged with `frame`, and their bounds.
    SceneData render(int frame)
    {
        SceneData tmp;
        auto evaluated = anim_.evaluate(frame / fps_);
        std::string xml = serializeSvgXml(*evaluated);
        std::vector<char> text(xml.begin(), xml.end());
        text.push_back('\0');
        if (NSVGimage* svg = nsvgParse(text.data(), "px", 96.0f))
        {
            populateScene(svg, evaluated.get(), filename_, elementBlur_, &images_, tmp);
            nsvgDelete(svg);
        }
        for (Dataset& ds : tmp.datasets) ds.frame = frame;
        return tmp;
    }

    std::vector<Dataset> datasets(int frame) override
    {
        auto it = cache_.find(frame);
        if (it != cache_.end()) return it->second;
        SceneData tmp = render(frame);

        constexpr size_t kMaxCached = 48;
        if (cache_.size() >= kMaxCached)
        {
            cache_.erase(order_.front());
            order_.erase(order_.begin());
        }
        cache_[frame] = tmp.datasets;
        order_.push_back(frame);
        return tmp.datasets;
    }

private:
    std::unique_ptr<SvgNode> dom_;
    SvgAnimation anim_;
    std::string filename_;
    double fps_;
    std::unordered_map<std::string, float> elementBlur_;
    SvgImageCache images_;
    std::unordered_map<int, std::vector<Dataset>> cache_;
    std::vector<int> order_; // oldest first
};
} // namespace

bool loadSvgFile(const std::string& filename, SceneData& scene, std::string& error)
{
    std::vector<char> svgText;
    NSVGimage* svg = giv::readFileUtf8(filename, svgText) ? nsvgParse(svgText.data(), "px", 96.0f) : nullptr;
    if (!svg)
    {
        error = "could not parse svg file: " + filename;
        return false;
    }

    // nsvgParse() rewrites svgText in place, so re-read the pristine text.
    std::unique_ptr<SvgNode> dom;
    std::unordered_map<std::string, float> elementBlur;
    {
        std::vector<char> raw;
        std::string xmlError;
        if (giv::readFileUtf8(filename, raw) && raw.size() > 1)
            dom = parseSvgXml(raw.data(), raw.size() - 1, xmlError);
        if (dom) elementBlur = parseElementBlurRadii(*dom);
    }

    // nanosvg has no viewport to resolve width="100%" against (it ends up
    // with a 1-unit canvas); give such a root a concrete size. <style> rules
    // (beyond the simple class selectors nanosvg knows) and CSS animations are
    // folded into the DOM as well.
    bool domChanged = false;
    if (dom)
    {
        domChanged = resolveRootPercentSize(*dom);
        domChanged = applySvgCss(*dom) || domChanged;
    }
    if (domChanged)
    {
        std::string xml = serializeSvgXml(*dom);
        std::vector<char> text(xml.begin(), xml.end());
        text.push_back('\0');
        if (NSVGimage* resized = nsvgParse(text.data(), "px", 96.0f))
        {
            nsvgDelete(svg);
            svg = resized;
        }
    }

    if (dom)
    {
        SvgAnimation probe(*dom);
        if (!probe.empty())
        {
            // Frames are rendered on demand from the DOM (see SvgFrameProvider).
            constexpr double kDefaultFps = 30.0;
            constexpr int kMaxFrames = 20000;
            if (probe.duration() > 0)
            {
                double fps = kDefaultFps;
                if (probe.duration() * fps > kMaxFrames) fps = kMaxFrames / probe.duration();
                const bool loops = probe.loops();
                const double duration = probe.duration();
                auto provider = std::make_shared<SvgFrameProvider>(std::move(dom), filename, fps);
                // A looping timeline's end is its start again; a one-shot one needs a
                // frame at its very end to show where it freezes.
                scene.frameCount = loops ? std::max(1, static_cast<int>(std::ceil(duration * fps)))
                                         : static_cast<int>(std::floor(duration * fps)) + 1;
                scene.fps = fps;
                scene.loopCount = loops ? 0 : 1;
                {
                    SceneData first = provider->render(0);
                    for (Dataset& ds : first.datasets) scene.datasets.push_back(std::move(ds));
                    scene.updateBounds(first.minX, first.minY);
                    scene.updateBounds(first.maxX, first.maxY);
                    scene.textExtents = std::move(first.textExtents);
                }
                // Later frames may draw outside frame 0's extent; the canvas bounds them.
                scene.updateBounds(0, 0);
                scene.updateBounds(svg->width, svg->height);
                scene.frameProvider = std::move(provider);
                nsvgDelete(svg);
                return true;
            }
            // Only instantaneous <set>s: render the document as it is at t = 0.
            auto at0 = probe.evaluate(0);
            std::string xml = serializeSvgXml(*at0);
            std::vector<char> text(xml.begin(), xml.end());
            text.push_back('\0');
            if (NSVGimage* animated = nsvgParse(text.data(), "px", 96.0f))
            {
                nsvgDelete(svg);
                svg = animated;
                populateScene(svg, at0.get(), filename, elementBlur, nullptr, scene);
                nsvgDelete(svg);
                return true;
            }
        }
    }

    populateScene(svg, dom.get(), filename, elementBlur, nullptr, scene);

    nsvgDelete(svg);
    return true;
}

} // namespace giv
