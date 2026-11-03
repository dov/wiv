#include "SvgLoader.h"
#include "PathUtils.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QFile>
#include <QImage>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cctype>
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

// Finds ` name="value"` (or single-quoted) in a tag, requiring whitespace
// before the name so that e.g. "width" doesn't match "stroke-width".
std::string tagAttr(const std::string& tag, const std::string& name)
{
    size_t pos = 0;
    while ((pos = tag.find(name, pos)) != std::string::npos)
    {
        size_t end = pos + name.size();
        bool okStart = pos > 0 && std::isspace(static_cast<unsigned char>(tag[pos - 1]));
        size_t q = end;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (okStart && q < tag.size() && tag[q] == '=')
        {
            ++q;
            while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\''))
            {
                size_t close = tag.find(tag[q], q + 1);
                if (close != std::string::npos) return tag.substr(q + 1, close - q - 1);
            }
        }
        pos = end;
    }
    return {};
}

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

// Drops XML character references for line breaks (Inkscape wraps base64 with
// "&#10;"), which would otherwise leave stray digits in the base64 text.
std::string stripLineEntities(std::string s)
{
    for (const char* ent : {"&#10;", "&#13;", "&#xA;", "&#xa;", "&#xD;", "&#xd;"})
    {
        size_t p;
        const std::string e(ent);
        while ((p = s.find(e)) != std::string::npos) s.erase(p, e.size());
    }
    return s;
}

bool decodeImageHref(std::string href, const std::string& absref, const std::string& svgFilename, QImage& out)
{
    href = stripLineEntities(href);
    if (href.compare(0, 5, "data:") == 0)
    {
        size_t comma = href.find(',');
        if (comma == std::string::npos) return false;
        std::string meta = href.substr(5, comma - 5);
        QByteArray payload(href.data() + comma + 1, static_cast<qsizetype>(href.size() - comma - 1));
        QByteArray bytes = meta.find(";base64") != std::string::npos
                               ? QByteArray::fromBase64(payload.simplified().replace(' ', ""))
                               : QByteArray::fromPercentEncoding(payload);
        return out.loadFromData(bytes);
    }
    // Candidate paths, in order: the href as written (resolved against the
    // svg's directory), then percent-decoded, then Inkscape's absref.
    const QString baseDir = QFileInfo(QString::fromStdString(svgFilename)).absolutePath();
    auto resolve = [&](QString path)
    {
        if (path.startsWith("file:")) path = QUrl(path).toLocalFile();
        if (QDir::isRelativePath(path)) path = baseDir + "/" + path;
        return path;
    };
    const QString raw = QString::fromUtf8(href.c_str());
    QStringList candidates{resolve(raw), resolve(QUrl::fromPercentEncoding(href.c_str()))};
    if (!absref.empty()) candidates << resolve(QString::fromUtf8(absref.c_str()));
    for (const QString& c : candidates)
        if (QFileInfo::exists(c) && out.load(c)) return true;
    return false;
}

void addSvgImages(const std::string& xml, const std::string& svgFilename, const NSVGimage* svg, SceneData& scene)
{
    // User units -> the pixel space nanosvg's shapes are in (see nsvgParse):
    // the viewBox is mapped onto width/height, xMidYMid meet.
    double scale = 1.0, offX = 0.0, offY = 0.0;
    {
        size_t t = xml.find("<svg");
        size_t te = t == std::string::npos ? t : xml.find('>', t);
        if (te != std::string::npos)
        {
            auto vb = parseNumbers(tagAttr(xml.substr(t, te - t + 1), "viewBox"));
            if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0 && svg->width > 0 && svg->height > 0)
            {
                scale = std::min(svg->width / vb[2], svg->height / vb[3]);
                offX = -vb[0] * scale + (svg->width - vb[2] * scale) * 0.5;
                offY = -vb[1] * scale + (svg->height - vb[3] * scale) * 0.5;
            }
        }
    }
    Affine root{scale, 0, 0, scale, offX, offY};

    std::vector<Affine> stack{root};
    size_t pos = 0;
    int index = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos)
    {
        if (xml.compare(pos, 4, "<!--") == 0)
        {
            size_t e = xml.find("-->", pos);
            if (e == std::string::npos) break;
            pos = e + 3;
            continue;
        }
        // Tag end: first '>' outside quotes (hrefs hold no '>').
        size_t end = pos;
        char quote = 0;
        for (; end < xml.size(); ++end)
        {
            char ch = xml[end];
            if (quote) { if (ch == quote) quote = 0; }
            else if (ch == '"' || ch == '\'') quote = ch;
            else if (ch == '>') break;
        }
        if (end >= xml.size()) break;
        std::string tag = xml.substr(pos, end - pos + 1);
        pos = end + 1;
        if (tag.size() < 2 || tag[1] == '?' || tag[1] == '!') continue;

        bool closing = tag[1] == '/';
        bool selfClosing = tag[tag.size() - 2] == '/';
        size_t ns = closing ? 2 : 1, ne = ns;
        while (ne < tag.size() && !std::isspace(static_cast<unsigned char>(tag[ne])) && tag[ne] != '>' && tag[ne] != '/') ++ne;
        std::string name = tag.substr(ns, ne - ns);

        if (closing)
        {
            if ((name == "g" || name == "a" || name == "svg" || name == "switch") && stack.size() > 1) stack.pop_back();
            continue;
        }
        if (name == "g" || name == "a" || name == "switch" || name == "svg")
        {
            if (name == "svg" && stack.size() == 1 && !selfClosing && index == 0 && xml.find("<svg") == pos - tag.size())
            {
                // The root element: its transform (if any) is not applied by nanosvg either.
                stack.push_back(stack.back());
                continue;
            }
            if (!selfClosing) stack.push_back(stack.back() * parseTransform(tagAttr(tag, "transform")));
            continue;
        }
        if (name != "image") continue;

        std::string href = tagAttr(tag, "xlink:href");
        if (href.empty()) href = tagAttr(tag, "href");
        std::string style = tagAttr(tag, "style");
        if (style.find("display:none") != std::string::npos || tagAttr(tag, "display") == "none") continue;

        QImage img;
        if (href.empty() || !decodeImageHref(href, tagAttr(tag, "sodipodi:absref"), svgFilename, img) || img.isNull()) continue;
        img = img.convertToFormat(QImage::Format_RGBA8888);

        double x = parseLength(tagAttr(tag, "x")), y = parseLength(tagAttr(tag, "y"));
        double w = parseLength(tagAttr(tag, "width"), img.width());
        double h = parseLength(tagAttr(tag, "height"), img.height());
        if (w <= 0 || h <= 0) continue;

        std::string par = tagAttr(tag, "preserveAspectRatio");
        if (par.find("none") == std::string::npos)
        {
            double s = std::min(w / img.width(), h / img.height());
            double iw = img.width() * s, ih = img.height() * s;
            bool slice = par.find("slice") != std::string::npos;
            if (slice) { s = std::max(w / img.width(), h / img.height()); iw = img.width() * s; ih = img.height() * s; }
            double fx = par.find("xMin") != std::string::npos ? 0.0 : par.find("xMax") != std::string::npos ? 1.0 : 0.5;
            double fy = par.find("YMin") != std::string::npos ? 0.0 : par.find("YMax") != std::string::npos ? 1.0 : 0.5;
            x += (w - iw) * fx;
            y += (h - ih) * fy;
            w = iw;
            h = ih;
            // slice would need clipping to the viewport; show the whole image instead.
        }

        Affine m = stack.back() * parseTransform(tagAttr(tag, "transform"));
        double px[4] = {x, x + w, x, x + w}, py[4] = {y, y, y + h, y + h};
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (int i = 0; i < 4; ++i)
        {
            double tx = m.a * px[i] + m.c * py[i] + m.e;
            double ty = m.b * px[i] + m.d * py[i] + m.f;
            minX = std::min(minX, tx); maxX = std::max(maxX, tx);
            minY = std::min(minY, ty); maxY = std::max(maxY, ty);
        }

        double opacity = parseLength(tagAttr(tag, "opacity"), 1.0);
        size_t op = style.find("opacity:");
        if (op != std::string::npos && (op == 0 || style[op - 1] == ';' || style[op - 1] == ' '))
            opacity = parseLength(style.substr(op + 8), opacity);
        if (opacity < 1.0)
        {
            for (int row = 0; row < img.height(); ++row)
            {
                uchar* line = img.scanLine(row);
                for (int col = 0; col < img.width(); ++col)
                    line[col * 4 + 3] = static_cast<uchar>(line[col * 4 + 3] * std::max(0.0, opacity));
            }
        }

        scene.datasets.emplace_back();
        Dataset& ds = scene.datasets.back();
        std::string id = tagAttr(tag, "id");
        ds.pathName = id.empty() ? "svg image " + std::to_string(index) : id;
        ++index;
        ds.doDrawLines = false;
        ds.doDrawPolygon = false;
        ds.doDrawMarks = false;
        ds.isSprite = true;
        ds.spriteWidth = img.width();
        ds.spriteHeight = img.height();
        ds.spriteRGBA.resize(static_cast<size_t>(img.width()) * img.height() * 4);
        for (int row = 0; row < img.height(); ++row)
            std::memcpy(ds.spriteRGBA.data() + static_cast<size_t>(row) * img.width() * 4, img.constScanLine(row),
                        static_cast<size_t>(img.width()) * 4);
        ds.spriteX = minX;
        ds.spriteY = minY;
        ds.spriteW = maxX - minX;
        ds.spriteH = maxY - minY;
        scene.updateBounds(minX, minY);
        scene.updateBounds(maxX, maxY);
    }
}

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
    std::string rawXml;
    std::unordered_map<std::string, float> elementBlur;
    {
        std::ifstream in(giv::utf8ToPath(filename), std::ios::binary);
        if (in)
        {
            std::ostringstream ss;
            ss << in.rdbuf();
            rawXml = ss.str();
            elementBlur = parseElementBlurRadii(rawXml);
        }
    }

    // Images first: nanosvg drops <image>, so their z-order relative to the
    // shapes is lost; the usual case is a picture annotated with shapes.
    addSvgImages(rawXml, filename, svg, scene);

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
