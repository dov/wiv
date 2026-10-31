#pragma once
//
// ColorTablePanel.h - Tools > Color Table: picks a predefined colormap
// (see Colormaps.h, ported from giv's colormaps.cc) to recolor the current
// contrast-stretched grayscale image. Mirrors giv's View > Pseudo Color
// radio-group menu, as a dock widget (matches ContrastPanel/MarkTreeView's
// "live tool" pattern rather than CalibrateDialog's one-shot modal).
//
#include "Colormaps.h"

#include <QWidget>
#include <QPixmap>

class QListWidget;

namespace givqt
{

class VulkanViewport;

// Horizontal gradient preview of a colormap, shared with ContrastPanel's
// color table combo box.
QPixmap colormapSwatch(giv::colormaps::Id id, int width, int height);

class ColorTablePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit ColorTablePanel(VulkanViewport* viewport, QWidget* parent = nullptr);

    // Re-selects the current image's colormap in the list. Call whenever
    // the displayed image changes while this panel is open.
    void refreshForImage();

signals:
    // Emitted when the user picks a colormap here (so other views of the
    // colormap, e.g. ContrastPanel's combo box, can resync).
    void colormapChanged();

private:
    VulkanViewport* viewport_ = nullptr;
    QListWidget* list_ = nullptr;

    void onSelectionChanged();
};

} // namespace givqt
