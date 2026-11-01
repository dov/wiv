#pragma once
//
// ContrastPanel.h - Tools > Adjust Contrast (a dock widget), giv's GivContrast dialog
// (src/giv-contrast.gob + src/giv-histo.gob there): shows a histogram of
// the current image's real pixel values with draggable min/max handles,
// min/max/center/window entry fields, and "Auto Contrast" (stretch to the
// image's own min/max). This is a live tool (like MarkTreeView and
// CalibratePanel) that needs to keep reading/writing VulkanViewport as the user drags
// handles or switches images while it stays open - see MainWindow::
// showContrastPanel().
//
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QSlider;

namespace givqt
{

class VulkanViewport;
class HistogramWidget;

class ContrastPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit ContrastPanel(VulkanViewport* viewport, QWidget* parent = nullptr);

    // Re-reads the current image's histogram and contrast state from
    // viewport_ and refreshes the display. Call whenever the displayed
    // image changes while this panel is open (see MainWindow::imageChanged).
    void refreshForImage();

    // The displayed slice of a multi-slice image changed: refreshes the
    // histogram if it is of the current slice (a whole-volume one is unchanged).
    void refreshForSlice();

signals:
    // Emitted when the user picks a colormap here (so ColorTablePanel can
    // resync its selection).
    void colormapChanged();

private:
    VulkanViewport* viewport_ = nullptr;
    HistogramWidget* histogram_ = nullptr;
    QRadioButton* minMaxRadio_ = nullptr;
    QRadioButton* centerWindowRadio_ = nullptr;
    QLineEdit* minEdit_ = nullptr;
    QLineEdit* maxEdit_ = nullptr;
    QLineEdit* centerEdit_ = nullptr;
    QLineEdit* windowEdit_ = nullptr;
    QSlider* strengthSlider_ = nullptr;
    QComboBox* colormapCombo_ = nullptr;
    QCheckBox* volumeHistogramCheck_ = nullptr; // shown only for multi-slice images

    void updateHistogram();

    void onRadioToggled();
    void onUpdateClicked();
    void onAutoContrast();
    void onColormapPicked(int row);
    void onHandlesDragged(float min, float max);

    // Sets both field-pairs (min/max and center/window) from `min`/`max`,
    // pushes the new contrast to viewport_, and updates the histogram
    // overlay - the single path every entry point above funnels through.
    void applyContrast(float min, float max);
};

} // namespace givqt
