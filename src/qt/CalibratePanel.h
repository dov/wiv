// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// CalibratePanel.h - Tools > Calibrate Pixel Size, giv's GivCalibrateDialog
// (src/giv-calibrate-dialog.gob there) as a dock widget: lets the user tell
// wiv how many real-world units a single image pixel spans, so the caliper
// tool's distance readout can show real units instead of raw pixels.
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
#include <QWidget>

#include <optional>
#include <utility>

class QComboBox;
class QLineEdit;

namespace givqt
{

class VulkanViewport;

class CalibratePanel final : public QWidget
{
    Q_OBJECT

public:
    explicit CalibratePanel(VulkanViewport* viewport, QWidget* parent = nullptr);

    // Re-reads the calibration, the last caliper distance and the image size
    // from the viewport and resets the fields. Call when the panel is shown
    // and whenever the displayed image changes.
    void refresh();

    // Re-reads only the last caliper distance and image size to enable or
    // disable the sources that depend on them, leaving the fields as typed.
    // Call after a measurement.
    void refreshSources();

private:
    enum Source
    {
        kPixel,
        kLastMeasure,
        kImageWidth,
        kImageHeight
    };

    VulkanViewport* viewport_ = nullptr;
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
