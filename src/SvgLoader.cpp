#include "SvgLoader.h"
#include "ImagePluginHost.h"
#include "PathUtils.h"
#include "SvgAnimation.h"
#include "SvgDom.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

// Static: the stbimage/ico plugins carry their own copies of stb_image.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
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

// nanosvg has no support at all for SVG <filter>/feGaussianBlur - it's a
// pure geometry parser. This is NOT a general CSS parser either: it
// recognizes just enough of Inkscape's usual drop-shadow/soft-highlight
// export shape (a <filter id="X"> element containing a <feGaussianBlur
// stdDeviation="D"/>, referenced by some element's own style="...
// filter:url(#X)...") to resolve elementId -> blur stdDeviation, so
// loadSvgFile can rasterize+blur just those shapes (see
// rasterizeBlurredShape) instead of silently dropping the effect.
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
        ds.spriteRGBA = std::move(img.rgba);
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
// <text> support. nanosvg ignores text, so it is read from the DOM here and
// becomes text marks (the same kind a .giv "t" line makes), scaled with the
// drawing. Supported: x/y (lists use the first), dx/dy, text-anchor, fill,
// fill-opacity/opacity, font-family/size/weight/style, transform (translate,
// uniform scale, rotation), and <tspan> (a tspan with x/y/dy starts a new line).
// ---------------------------------------------------------------------------
struct TextStyle
{
    double r = 0, g = 0, b = 0, alpha = 1;
    bool fillNone = false;
    double fontPx = 16.0;
    std::string family = "Sans";
    bool bold = false, italic = false;
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
    if (!v.empty()) st.bold = v == "bold" || v == "bolder" || parseLength(v, 400) >= 600;
    v = cssProp(el, "font-style");
    if (!v.empty()) st.italic = v == "italic" || v == "oblique";
    v = cssProp(el, "text-anchor");
    if (v == "middle") st.anchorCol = 1;
    else if (v == "end") st.anchorCol = 2;
    else if (v == "start") st.anchorCol = 0;
    if (cssProp(el, "display") == "none" || cssProp(el, "visibility") == "hidden") st.hidden = true;
    return st;
}

struct TextWalker
{
    SceneData& scene;
    int index = 0;

    struct Run
    {
        double x, y;
        std::string text;
        TextStyle style;
    };

    // Appends the character data of `el` (and its tspans) to the runs.
    void collect(const SvgNode& el, const TextStyle& st, double& cx, double& cy, std::vector<Run>& runs)
    {
        for (const auto& c : el.children)
        {
            if (c->name == "#text")
            {
                if (runs.empty()) runs.push_back({cx, cy, {}, st});
                runs.back().text += c->text;
            }
            else if (c->name == "tspan" || c->name == "a")
            {
                const bool newLine = c->hasAttr("x") || c->hasAttr("y") || c->hasAttr("dy");
                if (c->hasAttr("x")) cx = parseLength(c->attr("x"), cx);
                if (c->hasAttr("y")) cy = parseLength(c->attr("y"), cy);
                cx += parseLength(c->attr("dx"));
                cy += parseLength(c->attr("dy"));
                const TextStyle inner = applyTextStyle(st, *c);
                // A tspan that moves the cursor starts a new run in its own style;
                // otherwise it extends the current run (whose style then wins).
                if (newLine && !runs.empty() && !runs.back().text.empty()) runs.push_back({cx, cy, {}, inner});
                else if (runs.empty()) runs.push_back({cx, cy, {}, inner});
                collect(*c, inner, cx, cy, runs);
            }
        }
    }

    static std::string collapse(const std::string& in)
    {
        std::string out;
        bool space = true; // drops leading whitespace
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
        if (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    void addText(const SvgNode& el, const Affine& parent, const TextStyle& inherited)
    {
        TextStyle st = applyTextStyle(inherited, el);
        if (st.hidden || st.fillNone) return;
        const Affine m = parent * parseTransform(el.attr("transform"));

        // x/y/dx/dy may be lists; the first value positions the text.
        auto first = [](const std::string& l, double d) { auto n = parseNumbers(l); return n.empty() ? d : n[0]; };
        double cx = first(el.attr("x"), 0.0) + first(el.attr("dx"), 0.0);
        double cy = first(el.attr("y"), 0.0) + first(el.attr("dy"), 0.0);
        std::vector<Run> runs;
        collect(el, st, cx, cy, runs);

        // Linear part of m: uniform scale and rotation (skew is ignored).
        const double sc = std::sqrt(std::abs(m.a * m.d - m.b * m.c));
        const double angle = std::atan2(m.b, m.a);
        if (sc <= 0) return;

        for (Run& run : runs)
        {
            run.text = collapse(run.text);
            if (run.text.empty() || run.style.fillNone || run.style.hidden) continue;
            const TextStyle& st = run.style;
            const double fontWorld = st.fontPx * sc;
            const double wx = m.a * run.x + m.c * run.y + m.e;
            const double wy = m.b * run.x + m.d * run.y + m.f;

            scene.datasets.emplace_back();
            Dataset& ds = scene.datasets.back();
            const std::string& id = el.attr("id");
            ds.pathName = id.empty() ? "svg text " + std::to_string(index) : id;
            ++index;
            ds.color = Color::opaque(static_cast<float>(st.r / 255.0), static_cast<float>(st.g / 255.0),
                                     static_cast<float>(st.b / 255.0), static_cast<float>(st.alpha));
            ds.doDrawLines = false;
            ds.doDrawPolygon = false;
            ds.doDrawMarks = false;
            ds.doScaleFonts = true;
            ds.textSize = fontWorld * 72.0 / 96.0; // SceneBuilder takes points
            ds.textAngle = angle;
            ds.fontName = st.family + (st.bold ? " Bold" : "") + (st.italic ? " Italic" : "");
            const int align = 1 + st.anchorCol; // bottom row: y is the baseline
            ds.texts.push_back(TextItem{ds.pointCount(), align, run.text});
            ds.addPoint(static_cast<float>(wx), static_cast<float>(wy), Op::Text);

            // Estimated extent (~0.55 em per character) so fit-to-window leaves room.
            const double w = 0.55 * fontWorld * static_cast<double>(run.text.size()), h = fontWorld;
            SceneData::TextExtent te{wx, wy, 0, w, 0, h, true};
            if (st.anchorCol == 1) { te.l = -w / 2; te.r = w / 2; }
            else if (st.anchorCol == 2) { te.l = -w; te.r = 0; }
            scene.textExtents.push_back(te);
            scene.updateBounds(wx, wy);
        }
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
    // with a 1-unit canvas); give such a root a concrete size.
    if (dom && resolveRootPercentSize(*dom))
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
