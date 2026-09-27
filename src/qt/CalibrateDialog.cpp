#include "CalibrateDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace givqt
{

CalibrateDialog::CalibrateDialog(QWidget* parent, double pixelSize, const QString& unit,
                                  double lastMeasureDistancePixels,
                                  std::optional<std::pair<double, double>> imageSize)
    : QDialog(parent), lastMeasureDistancePixels_(lastMeasureDistancePixels), imageSize_(imageSize)
{
    setWindowTitle("Calibrate Pixel Size");

    sourceCombo_ = new QComboBox(this);
    sourceCombo_->addItem("Pixel", kPixel);
    sourceCombo_->addItem("Last measure", kLastMeasure);
    sourceCombo_->addItem("Image width", kImageWidth);
    sourceCombo_->addItem("Image height", kImageHeight);
    auto* model = qobject_cast<QStandardItemModel*>(sourceCombo_->model());
    if (lastMeasureDistancePixels_ <= 0.0) model->item(kLastMeasure)->setEnabled(false);
    if (!imageSize_)
    {
        model->item(kImageWidth)->setEnabled(false);
        model->item(kImageHeight)->setEnabled(false);
    }
    connect(sourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CalibrateDialog::onSourceChanged);

    distanceEdit_ = new QLineEdit(QString::number(pixelSize, 'g', 4), this);

    unitEdit_ = new QLineEdit(unit, this);

    auto form = new QFormLayout();
    form->addRow("Source:", sourceCombo_);
    form->addRow("Distance:", distanceEdit_);
    form->addRow("Unit:", unitEdit_);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        applyCalibration();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &CalibrateDialog::applyCalibration);

    auto layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    setLayout(layout);
}

double CalibrateDialog::calcPixelSize(int source) const
{
    double pixelSize = distanceEdit_->text().toDouble();
    switch (source)
    {
    case kLastMeasure:
        if (lastMeasureDistancePixels_ > 0.0) pixelSize /= lastMeasureDistancePixels_;
        break;
    case kImageWidth:
        if (imageSize_ && imageSize_->first > 0.0) pixelSize /= imageSize_->first;
        break;
    case kImageHeight:
        if (imageSize_ && imageSize_->second > 0.0) pixelSize /= imageSize_->second;
        break;
    default:
        break;
    }
    return pixelSize;
}

void CalibrateDialog::onSourceChanged(int index)
{
    int newSource = sourceCombo_->itemData(index).toInt();

    // Re-derive the underlying pixel size from the field's text as it reads
    // under the *previous* source, then re-express it under the new one -
    // the displayed number changes, but the pixel size it implies doesn't
    // (mirrors giv's cb_combo_changed).
    double pixelSize = calcPixelSize(currentSource_);
    double newDist = pixelSize;
    switch (newSource)
    {
    case kLastMeasure: newDist *= lastMeasureDistancePixels_; break;
    case kImageWidth: newDist *= imageSize_ ? imageSize_->first : 1.0; break;
    case kImageHeight: newDist *= imageSize_ ? imageSize_->second : 1.0; break;
    default: break;
    }

    distanceEdit_->setText(QString::number(newDist, 'g', 4));
    currentSource_ = newSource;
}

void CalibrateDialog::applyCalibration()
{
    emit calibrationChanged(calcPixelSize(currentSource_), unitEdit_->text());
}

} // namespace givqt
