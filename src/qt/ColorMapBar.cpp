#include "ColorMapBar.h"

#include "Colormaps.h"
#include "VulkanViewport.h"

#include <QPainter>

namespace givqt
{

ColorMapBar::ColorMapBar(VulkanViewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport)
{
    setFixedHeight(sizeHint().height());
    connect(viewport_, &VulkanViewport::contrastChanged, this, qOverload<>(&QWidget::update));
    connect(viewport_, &VulkanViewport::imageChanged, this, qOverload<>(&QWidget::update));
}

void ColorMapBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());

    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img || img->sampleType == WIV_SAMPLE_NONE) return;

    const auto state = viewport_->currentContrastState();
    const uint8_t* lut = state.colormapEnabled ? giv::colormaps::lut(state.colormapId) : nullptr;

    const QFontMetrics fm = p.fontMetrics();
    const QRect bar(0, 0, width(), height() - fm.height() - 2);
    for (int x = 0; x < bar.width(); ++x)
    {
        int i = bar.width() > 1 ? x * 255 / (bar.width() - 1) : 0;
        QColor c = lut ? QColor(lut[i * 3], lut[i * 3 + 1], lut[i * 3 + 2]) : QColor(i, i, i);
        p.setPen(c);
        p.drawLine(x, bar.top(), x, bar.bottom());
    }

    p.setPen(palette().windowText().color());
    const QRect labels(0, bar.bottom() + 1, width(), fm.height() + 1);
    p.drawText(labels, Qt::AlignLeft | Qt::AlignVCenter, QString::number(state.min, 'g', 6));
    p.drawText(labels, Qt::AlignRight | Qt::AlignVCenter, QString::number(state.max, 'g', 6));
}

} // namespace givqt
