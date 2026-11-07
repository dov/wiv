// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// HistogramWidget.h - the histogram plot + draggable min/max contrast
// handles inside ContrastDialog, factored out into its own QWidget since it
// needs Q_OBJECT/moc and is otherwise just an implementation detail of
// ContrastDialog. Mirrors giv's GivHisto (src/giv-histo.gob): a fixed
// x-axis spanning the image's own sampleMin/sampleMax, with a draggable
// overlay window (the live contrast range) on top.
//
#include <QPoint>
#include <QWidget>

#include <array>
#include <cstdint>

namespace givqt
{

class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistogramWidget(QWidget* parent = nullptr);

    // Sets the fixed histogram axis (the image's own min/max, giv's
    // img_gl_min/img_gl_max) and bucket counts.
    void setHistogram(const std::array<uint32_t, 256>& counts, float axisMin, float axisMax);

    // Sets the draggable contrast window without emitting rangeChanged
    // (external state -> widget).
    void setContrastRange(float min, float max);

    // Log-scale y zoom, value in [0,1]: 0 fits the whole histogram, 1 multiplies
    // the counts by 10000. Does not affect the contrast math,
    // only how visible low-count buckets are.
    void setStrength(double value);

    // Pixel value under the image cursor, shown as a red triangle at the top.
    void setCursorValue(bool valid, double value = 0.0);

    // Display gamma shown as the middle handle between the contrast handles
    // (without emitting gammaChanged). The handle sits where the gamma curve
    // reaches 50% output, i.e. at fraction 0.5^gamma of the window.
    void setGamma(float gamma);

signals:
    // Emitted continuously while a handle is being dragged (widget -> caller).
    void rangeChanged(float min, float max);
    // Emitted continuously while the middle (gamma) handle is dragged.
    void gammaChanged(float gamma);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    enum class DragMode
    {
        None,
        Min,
        Max,
        Gamma, // the middle handle: the midtone point of the window
        Window // dragging between the two handles shifts both, preserving width
    };

    std::array<uint32_t, 256> counts_{};
    float axisMin_ = 0.0f;
    float axisMax_ = 1.0f;
    float contrastMin_ = 0.0f;
    float contrastMax_ = 1.0f;
    double strength_ = 0.0;
    float gamma_ = 1.0f;
    bool hasCursorValue_ = false;
    double cursorValue_ = 0.0;

    DragMode dragMode_ = DragMode::None;
    DragMode hoverMode_ = DragMode::None;
    int dragStartX_ = 0;
    float dragStartMin_ = 0.0f;
    float dragStartMax_ = 0.0f;

    float xToValue(int x) const;
    int valueToX(float v) const;
    int handleX(float v) const;
    DragMode hitTest(QPoint pos) const;
    int gammaX() const;
    void updateCursor(DragMode mode);
};

} // namespace givqt
