#include "CalibratePanel.h"

#include "VulkanViewport.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace givqt
{

CalibratePanel::CalibratePanel(VulkanViewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport)
{
    sourceCombo_ = new QComboBox(this);
    sourceCombo_->addItem("Pixel", kPixel);
    sourceCombo_->addItem("Last measure", kLastMeasure);
    sourceCombo_->addItem("Image width", kImageWidth);
    sourceCombo_->addItem("Image height", kImageHeight);
    connect(sourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CalibratePanel::onSourceChanged);

    distanceEdit_ = new QLineEdit(this);
    unitEdit_ = new QLineEdit(this);

    auto form = new QFormLayout();
    form->addRow("Source:", sourceCombo_);
    form->addRow("Distance:", distanceEdit_);
    form->addRow("Unit:", unitEdit_);

    auto apply = new QPushButton("Apply", this);
    connect(apply, &QPushButton::clicked, this, &CalibratePanel::applyCalibration);
    connect(distanceEdit_, &QLineEdit::returnPressed, this, &CalibratePanel::applyCalibration);
    connect(unitEdit_, &QLineEdit::returnPressed, this, &CalibratePanel::applyCalibration);

    auto layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(apply);
    layout->addStretch(1);

    refresh();
}

void CalibratePanel::refresh()
{
    QSignalBlocker blocker(sourceCombo_); // resetting must not re-derive the number
    sourceCombo_->setCurrentIndex(kPixel);
    currentSource_ = kPixel;
    distanceEdit_->setText(QString::number(viewport_->pixelSize(), 'g', 4));
    unitEdit_->setText(QString::fromStdString(viewport_->pixelSizeUnit()));
    refreshSources();
}

void CalibratePanel::refreshSources()
{
    lastMeasureDistancePixels_ = viewport_->lastMeasureDistancePixels();
    imageSize_ = viewport_->currentImageSize();
    auto* model = qobject_cast<QStandardItemModel*>(sourceCombo_->model());
    model->item(kLastMeasure)->setEnabled(lastMeasureDistancePixels_ > 0.0);
    model->item(kImageWidth)->setEnabled(imageSize_.has_value());
    model->item(kImageHeight)->setEnabled(imageSize_.has_value());
}

double CalibratePanel::calcPixelSize(int source) const
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

void CalibratePanel::onSourceChanged(int index)
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

void CalibratePanel::applyCalibration()
{
    viewport_->setPixelCalibration(calcPixelSize(currentSource_), unitEdit_->text().toStdString());
}

} // namespace givqt
