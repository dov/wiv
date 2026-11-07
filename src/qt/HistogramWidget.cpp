// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "HistogramWidget.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace givqt
{

namespace
{
constexpr int kHandleGrabPx = 7;
// Blank space kept on each side of the plot so a handle sitting at the axis
// min/max (or clamped there, when the contrast range extends past the axis)
// is fully visible and always grabbable.
constexpr int kMarginPx = 10;
constexpr int kGripW = 10;
constexpr int kGripH = 16;
constexpr int kGammaTriW = 12;
constexpr int kGammaTriH = 10;
constexpr float kMinGamma = 0.05f, kMaxGamma = 20.0f;
constexpr int kTopMarginPx = 14; // room above the tallest bar for the cursor marker
} // namespace

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(120);
    setMouseTracking(true);
}

void HistogramWidget::setHistogram(const std::array<uint32_t, 256>& counts, float axisMin, float axisMax)
{
    counts_ = counts;
    axisMin_ = axisMin;
    axisMax_ = axisMax;
    update();
}

void HistogramWidget::setContrastRange(float min, float max)
{
    contrastMin_ = min;
    contrastMax_ = max;
    update();
}

void HistogramWidget::setStrength(double value)
{
    strength_ = value;
    update();
}

void HistogramWidget::setGamma(float gamma)
{
    if (gamma == gamma_) return;
    gamma_ = gamma;
    update();
}

int HistogramWidget::gammaX() const
{
    const int xMin = handleX(contrastMin_);
    const int xMax = handleX(contrastMax_);
    const double f = std::pow(0.5, static_cast<double>(gamma_));
    return xMin + static_cast<int>(std::lround(f * (xMax - xMin)));
}

void HistogramWidget::setCursorValue(bool valid, double value)
{
    if (valid == hasCursorValue_ && (!valid || value == cursorValue_)) return;
    hasCursorValue_ = valid;
    cursorValue_ = value;
    update();
}

float HistogramWidget::xToValue(int x) const
{
    int w = width() - 2 * kMarginPx;
    if (w <= 0) return axisMin_;
    double t = static_cast<double>(x - kMarginPx) / w;
    return static_cast<float>(axisMin_ + t * (axisMax_ - axisMin_));
}

int HistogramWidget::valueToX(float v) const
{
    if (axisMax_ == axisMin_) return kMarginPx;
    double t = (v - axisMin_) / (axisMax_ - axisMin_);
    return kMarginPx + static_cast<int>(std::lround(t * (width() - 2 * kMarginPx)));
}

// Handle x for drawing/hit-testing: pinned inside the widget so it stays
// reachable even if the contrast value lies outside the histogram axis.
int HistogramWidget::handleX(float v) const
{
    int lo = kMarginPx / 2;
    int hi = std::max(lo, width() - kMarginPx / 2);
    return std::clamp(valueToX(v), lo, hi);
}

HistogramWidget::DragMode HistogramWidget::hitTest(QPoint pos) const
{
    const int x = pos.x();
    // The gamma handle lives in the bottom strip, below the min/max grips.
    if (pos.y() >= height() - kGammaTriH - 4 && std::abs(x - gammaX()) <= kGammaTriW / 2 + 2) return DragMode::Gamma;
    int xMin = handleX(contrastMin_);
    int xMax = handleX(contrastMax_);
    int dMin = std::abs(x - xMin);
    int dMax = std::abs(x - xMax);
    if (dMin <= kHandleGrabPx || dMax <= kHandleGrabPx)
    {
        // Nearest wins; on a tie (handles coincide) pick by side so both
        // stay reachable.
        if (dMin == dMax) return x <= xMin ? DragMode::Min : DragMode::Max;
        return dMin < dMax ? DragMode::Min : DragMode::Max;
    }
    if (x > xMin && x < xMax) return DragMode::Window;
    return DragMode::None;
}

void HistogramWidget::updateCursor(DragMode mode)
{
    switch (mode)
    {
        case DragMode::Min:
        case DragMode::Max:
        case DragMode::Gamma: setCursor(Qt::SizeHorCursor); break;
        case DragMode::Window: setCursor(Qt::OpenHandCursor); break;
        default: unsetCursor(); break;
    }
}

void HistogramWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().base());

    uint32_t maxCount = *std::max_element(counts_.begin(), counts_.end());
    if (maxCount > 0)
    {
        // Log-scale y zoom: value 0 fits the whole histogram (tallest bar
        // just under the top margin), value 1 multiplies the counts by 10000.
        double scale = std::pow(10.0, 4.0 * strength_);
        double logMax = std::log1p(static_cast<double>(maxCount));

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(128, 128, 128));
        const double barW = static_cast<double>(width() - 2 * kMarginPx) / 256.0;
        for (int i = 0; i < 256; ++i)
        {
            if (counts_[static_cast<size_t>(i)] == 0) continue;
            double h = logMax > 0.0
                           ? std::log1p(static_cast<double>(counts_[static_cast<size_t>(i)]) * scale) / logMax
                           : 0.0;
            h = std::min(h, 1.0);
            int barH = static_cast<int>(h * (height() - kTopMarginPx));
            p.drawRect(QRectF(kMarginPx + i * barW, height() - barH, std::max(1.0, barW), barH));
        }
    }

    // Red downward triangle at the top showing the pixel value under the cursor.
    if (hasCursorValue_ && axisMax_ != axisMin_)
    {
        int cx = std::clamp(valueToX(static_cast<float>(cursorValue_)), kMarginPx, std::max(kMarginPx, width() - kMarginPx));
        QPolygon tri;
        tri << QPoint(cx - 5, 0) << QPoint(cx + 5, 0) << QPoint(cx, 9);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::red);
        p.drawPolygon(tri);
    }

    // Draggable contrast window overlay.
    int xMin = handleX(contrastMin_);
    int xMax = handleX(contrastMax_);
    p.setBrush(QColor(80, 140, 255, dragMode_ == DragMode::Window ? 80 : 50));
    p.setPen(Qt::NoPen);
    p.drawRect(QRect(xMin, 0, xMax - xMin, height()));

    const QColor lineCol(30, 100, 220);
    p.setPen(QPen(lineCol, 2));
    p.drawLine(xMin, 0, xMin, height());
    p.drawLine(xMax, 0, xMax, height());

    // The gamma curve (output level vs. input position) across the window.
    if (xMax > xMin)
    {
        const double inv = 1.0 / gamma_;
        QPolygonF curve;
        const int n = xMax - xMin;
        for (int i = 0; i <= n; i += 2)
            curve << QPointF(xMin + i, (height() - 1) * (1.0 - std::pow(static_cast<double>(i) / n, inv)));
        curve << QPointF(xMax, 0.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(230, 120, 20), 1.5));
        p.drawPolyline(curve);
    }

    // Grip tabs so the handles are visibly grabbable; the highlighted one is
    // under the cursor or being dragged.
    const bool dragging = dragMode_ != DragMode::None;
    auto grip = [&](int x, DragMode m)
    {
        bool hot = dragging ? dragMode_ == m : hoverMode_ == m;
        p.setPen(Qt::NoPen);
        p.setBrush(hot ? QColor(255, 170, 40) : lineCol);
        p.drawRoundedRect(QRect(x - kGripW / 2, height() / 2 - kGripH / 2, kGripW, kGripH), 2, 2);
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(x - 1, height() / 2 - 4, x - 1, height() / 2 + 4);
        p.drawLine(x + 1, height() / 2 - 4, x + 1, height() / 2 + 4);
    };
    grip(xMin, DragMode::Min);
    grip(xMax, DragMode::Max);

    // Gamma handle: an upward triangle on the bottom edge, between the grips.
    {
        const int gx = gammaX();
        const bool hot = dragging ? dragMode_ == DragMode::Gamma : hoverMode_ == DragMode::Gamma;
        QPolygon tri;
        tri << QPoint(gx - kGammaTriW / 2, height() - 1) << QPoint(gx + kGammaTriW / 2, height() - 1)
            << QPoint(gx, height() - 1 - kGammaTriH);
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(hot ? QColor(255, 170, 40) : QColor(110, 110, 110));
        p.drawPolygon(tri);
    }
}

void HistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    int x = event->pos().x();
    dragMode_ = hitTest(event->pos());
    if (dragMode_ == DragMode::Window) setCursor(Qt::ClosedHandCursor);

    dragStartX_ = x;
    dragStartMin_ = contrastMin_;
    dragStartMax_ = contrastMax_;
    update();
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (dragMode_ == DragMode::None)
    {
        DragMode m = hitTest(event->pos());
        if (m != hoverMode_)
        {
            hoverMode_ = m;
            update();
        }
        updateCursor(m);
        return;
    }

    if (dragMode_ == DragMode::Gamma)
    {
        const int xMin = handleX(contrastMin_);
        const int xMax = handleX(contrastMax_);
        if (xMax <= xMin) return;
        // Position f of the handle in the window -> gamma with 0.5^gamma == f.
        const double f = std::clamp(static_cast<double>(event->pos().x() - xMin) / (xMax - xMin), 1e-6, 1.0 - 1e-6);
        gamma_ = std::clamp(static_cast<float>(std::log(f) / std::log(0.5)), kMinGamma, kMaxGamma);
        update();
        emit gammaChanged(gamma_);
        return;
    }

    float newMin = contrastMin_, newMax = contrastMax_;
    switch (dragMode_)
    {
        case DragMode::Min:
            newMin = std::min(xToValue(event->pos().x()), contrastMax_);
            break;
        case DragMode::Max:
            newMax = std::max(xToValue(event->pos().x()), contrastMin_);
            break;
        case DragMode::Window:
        {
            float delta = xToValue(event->pos().x()) - xToValue(dragStartX_);
            newMin = dragStartMin_ + delta;
            newMax = dragStartMax_ + delta;
            break;
        }
        default:
            return;
    }

    contrastMin_ = newMin;
    contrastMax_ = newMax;
    update();
    emit rangeChanged(contrastMin_, contrastMax_);
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent* event)
{
    dragMode_ = DragMode::None;
    hoverMode_ = hitTest(event->pos());
    updateCursor(hoverMode_);
    update();
}

void HistogramWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-clicking the gamma handle resets it to linear.
    if (event->button() == Qt::LeftButton && hitTest(event->pos()) == DragMode::Gamma)
    {
        gamma_ = 1.0f;
        update();
        emit gammaChanged(gamma_);
    }
}

void HistogramWidget::leaveEvent(QEvent*)
{
    hoverMode_ = DragMode::None;
    update();
}

} // namespace givqt
