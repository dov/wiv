#include "HistogramWidget.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace givqt
{

namespace
{
constexpr int kHandleGrabPx = 6;
}

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
    if (width() <= 0) return axisMin_;
    double t = static_cast<double>(x) / width();
    return static_cast<float>(axisMin_ + t * (axisMax_ - axisMin_));
}

int HistogramWidget::valueToX(float v) const
{
    if (axisMax_ == axisMin_) return 0;
    double t = (v - axisMin_) / (axisMax_ - axisMin_);
    return static_cast<int>(std::lround(t * width()));
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
        const double barW = static_cast<double>(width()) / 256.0;
        for (int i = 0; i < 256; ++i)
        {
            if (counts_[static_cast<size_t>(i)] == 0) continue;
            double h = logMax > 0.0
                           ? std::log1p(static_cast<double>(counts_[static_cast<size_t>(i)]) * scale) / logMax
                           : 0.0;
            int barH = static_cast<int>(h * (height() - 2));
            p.drawRect(QRectF(i * barW, height() - barH, std::max(1.0, barW), barH));
        }
    }

    // Draggable contrast window overlay.
    int xMin = valueToX(contrastMin_);
    int xMax = valueToX(contrastMax_);
    p.setBrush(QColor(80, 140, 255, 50));
    p.setPen(Qt::NoPen);
    p.drawRect(QRect(xMin, 0, xMax - xMin, height()));

    QPen handlePen(QColor(30, 100, 220), 2);
    p.setPen(handlePen);
    p.drawLine(xMin, 0, xMin, height());
    p.drawLine(xMax, 0, xMax, height());
}

void HistogramWidget::mousePressEvent(QMouseEvent* event)
{
    int x = event->pos().x();
    int xMin = valueToX(contrastMin_);
    int xMax = valueToX(contrastMax_);

    if (std::abs(x - xMin) <= kHandleGrabPx)
        dragMode_ = DragMode::Min;
    else if (std::abs(x - xMax) <= kHandleGrabPx)
        dragMode_ = DragMode::Max;
    else if (x > xMin && x < xMax)
        dragMode_ = DragMode::Window;
    else
        dragMode_ = DragMode::None;

    dragStartX_ = x;
    dragStartMin_ = contrastMin_;
    dragStartMax_ = contrastMax_;
}

void HistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (dragMode_ == DragMode::None) return;

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

void HistogramWidget::mouseReleaseEvent(QMouseEvent*)
{
    dragMode_ = DragMode::None;
}

} // namespace givqt
