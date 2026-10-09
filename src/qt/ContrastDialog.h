#pragma once
//
// ContrastDialog.h - Tools > Adjust Contrast, giv's GivContrast dialog
// (src/giv-contrast.gob + src/giv-histo.gob there): shows a histogram of
// the current image's real pixel values with draggable min/max handles,
// min/max/center/window entry fields, and "Auto Contrast" (stretch to the
// image's own min/max). Unlike CalibrateDialog's fire-and-forget "read
// initial values in, emit a signal with the final result out" pattern,
// this is a non-modal, live tool (mirrors MarkTreeView, not CalibrateDialog)
// that needs to keep reading/writing VulkanViewport as the user drags
// handles or switches images while it stays open - see MainWindow::
// showContrastDialog().
//
#include <QDialog>

class QComboBox;
class QLineEdit;
class QRadioButton;
class QSlider;

namespace givqt
{

class VulkanViewport;
class HistogramWidget;

class ContrastDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit ContrastDialog(VulkanViewport* viewport, QWidget* parent = nullptr);

    // Re-reads the current image's histogram and contrast state from
    // viewport_ and refreshes the display. Call whenever the displayed
    // image changes while this dialog is open (see MainWindow::imageChanged).
    void refreshForImage();

signals:
    // Emitted when the user picks a colormap here (so ColorTableDialog can
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
