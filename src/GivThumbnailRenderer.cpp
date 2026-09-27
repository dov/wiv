#include "GivThumbnailRenderer.h"

#include "GivParser.h"
#include "GivScene.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <vector>

namespace giv
{

namespace
{

// Matches SceneBuilder.cpp's own curve/ellipse flattening tolerance (see
// tessellateCubic/tessellateEllipse there) so thumbnails have the same
// curve smoothness as the real viewer, independent of thumbnail scale -
// tessellation happens in world coordinates, before the scene is fit to
// the output image.
constexpr double kCurveFlatness = 0.05;
constexpr int kCurveMaxDepth = 24;
constexpr double kPi = 3.14159265358979323846;

double length(QPointF v) { return std::hypot(v.x(), v.y()); }

// De Casteljau adaptive cubic-bezier flattening (world coordinates), ported
// from SceneBuilder.cpp's tessellateCubic - see that function's comment for
// the algorithm. Appends sampled points to `out` (p0 assumed already
// present as the current path point).
void tessellateCubic(QPointF p0, QPointF p1, QPointF p2, QPointF p3, std::vector<QPointF>& out, int depth = 0)
{
    QPointF chord = p3 - p0;
    double chordLenSq = chord.x() * chord.x() + chord.y() * chord.y();
    auto distFromChord = [&](QPointF p) {
        if (chordLenSq < 1e-12) return length(p - p0);
        double t = ((p.x() - p0.x()) * chord.x() + (p.y() - p0.y()) * chord.y()) / chordLenSq;
        return length(p - (p0 + chord * t));
    };

    if (depth >= kCurveMaxDepth || (distFromChord(p1) <= kCurveFlatness && distFromChord(p2) <= kCurveFlatness))
    {
        out.push_back(p3);
        return;
    }

    QPointF p01 = (p0 + p1) * 0.5, p12 = (p1 + p2) * 0.5, p23 = (p2 + p3) * 0.5;
    QPointF p012 = (p01 + p12) * 0.5, p123 = (p12 + p23) * 0.5;
    QPointF p0123 = (p012 + p123) * 0.5;

    tessellateCubic(p0, p01, p012, p0123, out, depth + 1);
    tessellateCubic(p0123, p123, p23, p3, out, depth + 1);
}

// Ported from SceneBuilder.cpp's tessellateEllipse - returns a closed
// polyline (first == last point) approximating a rotated ellipse.
std::vector<QPointF> tessellateEllipsePoints(QPointF center, double rx, double ry, double angleDeg)
{
    double maxR = std::max(rx, ry);
    int segments = (maxR > 0.0) ? static_cast<int>(std::ceil(kPi * std::sqrt(maxR / (2.0 * kCurveFlatness)))) : 24;
    segments = std::clamp(segments, 24, 256);

    std::vector<QPointF> out;
    out.reserve(segments + 1);
    const double angleRad = angleDeg * kPi / 180.0;
    const double ca = std::cos(angleRad), sa = std::sin(angleRad);
    for (int i = 0; i <= segments; ++i)
    {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(segments);
        const double ex = rx * std::cos(t), ey = ry * std::sin(t);
        out.emplace_back(center.x() + ex * ca - ey * sa, center.y() + ex * sa + ey * ca);
    }
    return out;
}

QColor toQColor(const Color& c)
{
    return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f),
                             c.isNone ? 0.0f : std::clamp(c.a, 0.0f, 1.0f));
}

// Small filled triangular arrowhead at `tip` (device-pixel coordinates),
// pointing along unit vector `dir` (the direction of travel), sized
// relative to `hw` (the stroke half-width the arrow's line was drawn
// with) - a simplified stand-in for SceneBuilder's addArrowHead(), which
// matches giv's AGG arrowhead shape exactly; not worth reproducing at
// thumbnail scale.
void drawArrowHead(QPainter& painter, QPointF tip, QPointF dir, double hw, const QColor& color)
{
    QPointF back(-dir.x(), -dir.y());
    QPointF normal(-dir.y(), dir.x());
    auto at = [&](double lx, double ly) { return tip + back * lx + normal * ly; };

    QPolygonF poly;
    poly << tip << at(6.0 * hw, -3.0 * hw) << at(6.0 * hw, 3.0 * hw);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(poly);
}

// World-to-device-pixel mapping for one thumbnail render: uniform scale (no
// stretch) fitting the scene's bounding box into the output image, plus the
// offset that centers it.
struct RenderContext
{
    double scale = 1.0;
    double offsetX = 0.0;
    double offsetY = 0.0;

    QPointF toPixel(double x, double y) const { return QPointF(x * scale + offsetX, y * scale + offsetY); }
    QPointF toPixel(QPointF p) const { return toPixel(p.x(), p.y()); }
};

// Draws one Dataset's marks/lines/curves/ellipses/polygons/quivers/text,
// mirroring the point-stream walk in SceneBuilder::build() (SceneBuilder.cpp,
// the loop building `marks`/`lines`/`fillTris` from ds.x/y/op) but issuing
// immediate QPainter calls instead of GPU vertex batches.
void renderDataset(QPainter& painter, const Dataset& ds, const RenderContext& ctx)
{
    if (!ds.isVisible) return;

    if (ds.isSprite)
    {
        if (ds.spriteWidth > 0 && ds.spriteHeight > 0 &&
            ds.spriteRGBA.size() >= static_cast<size_t>(ds.spriteWidth) * static_cast<size_t>(ds.spriteHeight) * 4)
        {
            QImage sprite(ds.spriteRGBA.data(), ds.spriteWidth, ds.spriteHeight, ds.spriteWidth * 4, QImage::Format_RGBA8888);
            const QPointF topLeft = ctx.toPixel(ds.spriteX, ds.spriteY);
            const QPointF bottomRight = ctx.toPixel(ds.spriteX + ds.spriteW, ds.spriteY + ds.spriteH);
            painter.drawImage(QRectF(topLeft, bottomRight), sprite);
        }
        return;
    }

    const QColor fillColor = toQColor(ds.color);
    const QColor lineColor = toQColor(ds.color);
    const QColor outlineColor = toQColor(ds.outlineColor);
    const QColor quiverColor = toQColor(ds.quiverColor);
    const QColor markColor = toQColor(ds.color);

    // Mark/line sizes are device-pixel-constant in giv (independent of view
    // zoom) unless doScaleMarks says otherwise - see Dataset's field
    // comments in GivScene.h and SceneBuilder.cpp's markHalfSize/
    // lineHalfWidth (~L936-940). Clamped to stay legible (not vanish, not
    // dominate) at thumbnail scale.
    double markHalfSize = std::max(0.5, ds.markSize * 0.5);
    if (ds.markType == MarkType::Pixel) markHalfSize = std::min(markHalfSize, 0.5);
    if (ds.doScaleMarks) markHalfSize *= ctx.scale;
    markHalfSize = std::clamp(markHalfSize, 0.5, 24.0);

    const double lineWidth = std::clamp(ds.lineWidth, 0.5, 6.0);
    const double lineHalfWidth = std::max(0.5, lineWidth * 0.5);

    auto drawMark = [&](QPointF pixelPos) {
        if (!ds.doDrawMarks || ds.color.isNone) return;
        const double r = markHalfSize;
        switch (ds.markType)
        {
            case MarkType::FCircle:
                painter.setPen(Qt::NoPen);
                painter.setBrush(markColor);
                painter.drawEllipse(pixelPos, r, r);
                break;
            case MarkType::FSquare:
                painter.setPen(Qt::NoPen);
                painter.setBrush(markColor);
                painter.drawRect(QRectF(pixelPos.x() - r, pixelPos.y() - r, 2 * r, 2 * r));
                break;
            case MarkType::Circle:
                painter.setPen(QPen(markColor, 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(pixelPos, r, r);
                break;
            case MarkType::Square:
                painter.setPen(QPen(markColor, 1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(QRectF(pixelPos.x() - r, pixelPos.y() - r, 2 * r, 2 * r));
                break;
            case MarkType::Pixel:
                painter.setPen(QPen(markColor, 1.0));
                painter.drawPoint(pixelPos);
                break;
        }
    };

    // Subpath collection in world coordinates, mirroring SceneBuilder's own
    // Move/Draw/Curve/Ellipse/ClosePath walk (SceneBuilder.cpp ~L968-1123).
    // Mapped to device pixels only once a subpath is actually painted below.
    std::vector<std::vector<QPointF>> subpaths;
    std::vector<bool> subpathClosed;
    std::vector<QPointF> cur;
    QPointF currentPoint(0.0, 0.0);

    const size_t n = ds.pointCount();
    size_t textIdx = 0;
    const bool needSubpaths = ds.doDrawLines || ds.doDrawPolygon || ds.arrowType != ArrowType::None;

    for (size_t i = 0; i < n; ++i)
    {
        const Op op = ds.op[i];
        // giv's point arrays are already Y-down, same convention QImage
        // uses - unlike SceneBuilder (which negates Y for its Y-up math
        // camera), no flip is needed here.
        const QPointF p(ds.x[i], ds.y[i]);

        switch (op)
        {
            case Op::Move:
                drawMark(ctx.toPixel(p));
                if (needSubpaths)
                {
                    if (!cur.empty())
                    {
                        subpaths.push_back(cur);
                        subpathClosed.push_back(false);
                    }
                    cur.clear();
                    cur.push_back(p);
                }
                currentPoint = p;
                break;

            case Op::Draw:
                drawMark(ctx.toPixel(p));
                if (needSubpaths) cur.push_back(p);
                currentPoint = p;
                break;

            case Op::ClosePath:
                if (needSubpaths && !cur.empty())
                {
                    cur.push_back(cur.front());
                    subpaths.push_back(cur);
                    subpathClosed.push_back(true);
                    cur.clear();
                }
                break;

            case Op::Curve:
            {
                QPointF c1 = p;
                QPointF c2 = (i + 1 < n) ? QPointF(ds.x[i + 1], ds.y[i + 1]) : c1;
                QPointF end = (i + 2 < n) ? QPointF(ds.x[i + 2], ds.y[i + 2]) : c2;
                if (needSubpaths)
                {
                    QPointF start = cur.empty() ? currentPoint : cur.back();
                    if (cur.empty()) cur.push_back(start);
                    tessellateCubic(start, c1, c2, end, cur);
                }
                currentPoint = end;
                i += 2;
                break;
            }

            case Op::Cont:
                break; // orphan continuation point; Curve/Ellipse consume their own

            case Op::Ellipse:
            {
                QPointF center = p;
                QPointF wh = (i + 1 < n) ? QPointF(ds.x[i + 1], ds.y[i + 1]) : QPointF(1, 1);
                double angle = (i + 2 < n) ? ds.x[i + 2] : 0.0;
                if (needSubpaths)
                {
                    if (!cur.empty())
                    {
                        subpaths.push_back(cur);
                        subpathClosed.push_back(false);
                        cur.clear();
                    }
                    subpaths.push_back(tessellateEllipsePoints(center, wh.x(), wh.y(), angle));
                    subpathClosed.push_back(true);
                }
                currentPoint = center;
                i += 2;
                break;
            }

            case Op::Quiver:
            {
                const QPointF tipWorld(currentPoint.x() + p.x() * ds.quiverScale, currentPoint.y() + p.y() * ds.quiverScale);
                if (!ds.quiverColor.isNone)
                {
                    const QPointF p0px = ctx.toPixel(currentPoint), p1px = ctx.toPixel(tipWorld);
                    painter.setPen(QPen(quiverColor, lineHalfWidth * 2.0, Qt::SolidLine, Qt::RoundCap));
                    painter.drawLine(p0px, p1px);
                    if (ds.quiverHead)
                    {
                        QPointF dir = p1px - p0px;
                        double len = length(dir);
                        if (len > 1e-6) drawArrowHead(painter, p1px, dir / len, lineHalfWidth, quiverColor);
                    }
                }
                break;
            }

            case Op::Text:
                if (textIdx < ds.texts.size() && ds.texts[textIdx].pointIndex == i)
                {
                    const TextItem& item = ds.texts[textIdx];
                    ++textIdx;
                    if (!ds.color.isNone && !item.text.empty())
                    {
                        double size = ds.textSize > 0 ? ds.textSize : 12.0;
                        if (ds.doScaleFonts) size *= ctx.scale;
                        size = std::clamp(size, 6.0, 28.0);

                        QFont font;
                        if (!ds.fontName.empty()) font.setFamily(QString::fromStdString(ds.fontName));
                        font.setPointSizeF(size);
                        painter.setFont(font);
                        const QFontMetricsF fm(font);

                        const QString text = QString::fromStdString(item.text);
                        const double textWidth = fm.horizontalAdvance(text);
                        const double ascent = fm.ascent(), descent = fm.descent();

                        // Numpad alignment (see TextItem's comment in
                        // GivScene.h and SceneBuilder.cpp ~L1090-1098):
                        // col 0=left,1=center,2=right; row 0=bottom,1=center,2=top.
                        const int col = (item.align - 1) % 3;
                        const int row = (item.align - 1) / 3;

                        const QPointF anchor = ctx.toPixel(p);
                        const double x0 = (col == 0) ? 0.0 : (col == 1) ? -textWidth / 2.0 : -textWidth;
                        const double baselineY = (row == 0)   ? anchor.y() - descent
                                                 : (row == 2) ? anchor.y() + ascent
                                                              : anchor.y() + (ascent - descent) / 2.0;

                        painter.setPen(markColor);
                        painter.drawText(QPointF(anchor.x() + x0, baselineY), text);
                    }
                }
                break;
        }
    }

    if (needSubpaths && !cur.empty())
    {
        subpaths.push_back(cur);
        subpathClosed.push_back(false);
    }

    for (size_t spIdx = 0; spIdx < subpaths.size(); ++spIdx)
    {
        const auto& sp = subpaths[spIdx];
        if (sp.size() < 2 && !ds.doDrawPolygon) continue;

        QPolygonF pixelPoly;
        pixelPoly.reserve(static_cast<int>(sp.size()));
        for (const QPointF& wp : sp) pixelPoly << ctx.toPixel(wp);

        if (ds.doDrawPolygon && !ds.color.isNone && sp.size() >= 3)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(fillColor);
            painter.drawPolygon(pixelPoly);
        }

        if (ds.doDrawPolygonOutline && ds.doDrawPolygon)
        {
            if (!ds.outlineColor.isNone)
            {
                painter.setPen(QPen(outlineColor, lineWidth));
                painter.setBrush(Qt::NoBrush);
                painter.drawPolyline(pixelPoly);
            }
        }
        else if (ds.doDrawLines && !ds.color.isNone)
        {
            painter.setPen(QPen(lineColor, lineWidth));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(pixelPoly);
        }

        if (ds.doDrawLines && ds.arrowType != ArrowType::None && sp.size() >= 2 && !ds.color.isNone)
        {
            if (ds.arrowType == ArrowType::End || ds.arrowType == ArrowType::Both)
            {
                const bool spClosed = subpathClosed[spIdx];
                const bool haveTriple = spClosed ? sp.size() >= 3 : sp.size() >= 2;
                if (haveTriple)
                {
                    const size_t tipIdx = spClosed ? sp.size() - 2 : sp.size() - 1;
                    const size_t prevIdx = tipIdx - 1;
                    const QPointF tip = ctx.toPixel(sp[tipIdx]), prev = ctx.toPixel(sp[prevIdx]);
                    QPointF dir = tip - prev;
                    double len = length(dir);
                    if (len > 1e-6) drawArrowHead(painter, tip, dir / len, lineHalfWidth, lineColor);
                }
            }
            if (ds.arrowType == ArrowType::Start || ds.arrowType == ArrowType::Both)
            {
                const QPointF tip = ctx.toPixel(sp.front()), next = ctx.toPixel(sp[1]);
                QPointF dir = tip - next;
                double len = length(dir);
                if (len > 1e-6) drawArrowHead(painter, tip, dir / len, lineHalfWidth, lineColor);
            }
        }
    }
}

} // namespace

QImage renderGivThumbnail(const SceneData& scene, int maxSize)
{
    if (!scene.hasBounds() || maxSize <= 0) return QImage();

    double bboxW = scene.maxX - scene.minX;
    double bboxH = scene.maxY - scene.minY;
    if (bboxW < 1e-6) bboxW = 1.0;
    if (bboxH < 1e-6) bboxH = 1.0;

    const double paddingPx = std::max(4.0, maxSize * 0.06);
    const double avail = std::max(1.0, maxSize - 2.0 * paddingPx);
    const double scale = avail / std::max(bboxW, bboxH);

    RenderContext ctx;
    ctx.scale = scale;
    ctx.offsetX = (maxSize - bboxW * scale) / 2.0 - scene.minX * scale;
    ctx.offsetY = (maxSize - bboxH * scale) / 2.0 - scene.minY * scale;

    QImage image(maxSize, maxSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const Dataset& ds : scene.datasets) renderDataset(painter, ds, ctx);
    painter.end();

    return image;
}

QImage renderGivFileThumbnail(const std::string& filename, int maxSize)
{
    SceneData scene;
    std::string error;
    GivParser parser;
    if (!parser.parseFile(filename, scene, error)) return QImage();
    return renderGivThumbnail(scene, maxSize);
}

} // namespace giv
