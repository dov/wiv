#pragma once
//
// CalibrateDialog.h - Tools > Calibrate Pixel Size, giv's GivCalibrateDialog
// (src/giv-calibrate-dialog.gob there): lets the user tell wiv how many
// real-world units a single image pixel spans, so the caliper tool's
// distance readout can show real units instead of raw pixels.
//
// The "Source" combo picks what the "Distance" field's number means:
//   Pixel        - the field *is* the pixel size directly (real units/px).
//   Last measure - the field is the real-world length of the caliper's last
//                  measured (on-screen) distance; pixel size is derived by
//                  dividing by that distance in pixels.
//   Image width/height - likewise, but divided by the loaded image's pixel
//                  width/height instead.
// Switching sources re-derives the field's displayed number so it always
// reflects the same underlying pixel size (mirrors giv's cb_combo_changed).
//
#include <QDialog>

#include <optional>
#include <utility>

class QComboBox;
class QLineEdit;

namespace givqt
{

class CalibrateDialog : public QDialog
{
    Q_OBJECT

public:
    // `lastMeasureDistancePixels` <= 0 disables the "Last measure" source;
    // no `imageSize` disables "Image width"/"Image height".
    CalibrateDialog(QWidget* parent, double pixelSize, const QString& unit,
                     double lastMeasureDistancePixels,
                     std::optional<std::pair<double, double>> imageSize);

signals:
    void calibrationChanged(double pixelSize, QString unit);

private:
    enum Source
    {
        kPixel,
        kLastMeasure,
        kImageWidth,
        kImageHeight
    };

    QComboBox* sourceCombo_ = nullptr;
    QLineEdit* distanceEdit_ = nullptr;
    QLineEdit* unitEdit_ = nullptr;

    double lastMeasureDistancePixels_ = 0.0;
    std::optional<std::pair<double, double>> imageSize_;
    int currentSource_ = kPixel;

    // Real-world units/pixel implied by the field's current text, read back
    // as if `source` were still the active one (giv's
    // giv_calibrate_dialog_calc_pixel_size).
    double calcPixelSize(int source) const;

    void onSourceChanged(int index);
    void applyCalibration();
};

} // namespace givqt
