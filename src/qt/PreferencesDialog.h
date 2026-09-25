#pragma once
//
// PreferencesDialog.h - Edit > Preferences. Background color and auto-fit
// margin apply immediately (persisted via QSettings and re-applied to the
// live VulkanViewport); MSAA sample count is QSettings-persisted only and
// takes effect on next launch, since changing it means recreating the
// Vulkan swapchain/window, not attempted live.
//
#include <QColor>
#include <QDialog>

class QSpinBox;
class QComboBox;
class QPushButton;

namespace givqt
{

class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget* parent = nullptr);

    QColor backgroundColor() const { return backgroundColor_; }
    double autoFitMarginPx() const;
    int msaaSamples() const;

    // Reads current values from QSettings ("wiv", "wiv").
    static QColor loadBackgroundColor();
    static double loadAutoFitMarginPx();
    static int loadMsaaSamples();

signals:
    void backgroundColorChanged(const QColor& color);
    void autoFitMarginChanged(double px);

private:
    QColor backgroundColor_;
    QPushButton* colorButton_ = nullptr;
    QSpinBox* marginSpin_ = nullptr;
    QComboBox* msaaCombo_ = nullptr;

    void pickColor();
    void save();
};

} // namespace givqt
