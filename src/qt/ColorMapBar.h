#pragma once
//
// ColorMapBar.h - giv's View > Colormap Bar: a horizontal gradient strip under
// the canvas showing how the displayed image's contrast window (min..max) is
// mapped to colors, with the min/max values at its ends.
//
#include <QWidget>

namespace givqt
{

class VulkanViewport;

class ColorMapBar : public QWidget
{
    Q_OBJECT

public:
    explicit ColorMapBar(VulkanViewport* viewport, QWidget* parent = nullptr);

    QSize sizeHint() const override { return {200, 36}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VulkanViewport* viewport_ = nullptr;
};

} // namespace givqt
