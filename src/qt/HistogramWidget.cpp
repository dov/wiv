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
} // namespace

HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(120);
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

HistogramWidget::DragMode HistogramWidget::hitTest(int x) const
{
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
        case DragMode::Max: setCursor(Qt::SizeHorCursor); break;
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
        // giv's histogram-strength slider: pow(1000, 1-value) visually
        // boosts low-count buckets without changing the contrast math.
        double scale = std::pow(1000.0, 1.0 - strength_);
        double logMax = std::log1p(static_cast<double>(maxCount) * scale);

        p.setPen(Qt::NoPen);
        p.setBrush(palette().text());
        const double barW = static_cast<double>(width() - 2 * kMarginPx) / 256.0;
        for (int i = 0; i < 256; ++i)
        {
            if (counts_[static_cast<size_t>(i)] == 0) continue;
            double h = logMax > 0.0
                           ? std::log1p(static_cast<double>(counts_[static_cast<size_t>(i)]) * scale) / logMax
                           : 0.0;
            int barH = static_cast<int>(h * (height() - 2));
            p.drawRect(QRectF(kMarginPx + i * barW, height() - barH, std::max(1.0, barW), barH));
        }
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
}

void HistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    int x = event->pos().x();
    dragMode_ = hitTest(x);
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
        DragMode m = hitTest(event->pos().x());
        if (m != hoverMode_)
        {
            hoverMode_ = m;
            update();
        }
        updateCursor(m);
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
    hoverMode_ = hitTest(event->pos().x());
    updateCursor(hoverMode_);
    update();
}

void HistogramWidget::leaveEvent(QEvent*)
{
    hoverMode_ = DragMode::None;
    update();
}

} // namespace givqt
