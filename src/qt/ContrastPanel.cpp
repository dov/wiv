// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ContrastPanel.h"

#include "ColorTablePanel.h"
#include "Colormaps.h"
#include "HistogramWidget.h"
#include "ImagePluginHost.h"
#include "VulkanViewport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QIcon>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace givqt
{

ContrastPanel::ContrastPanel(VulkanViewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport)
{

    auto* grid = new QGridLayout();
    minMaxRadio_ = new QRadioButton(this);
    centerWindowRadio_ = new QRadioButton(this);
    minMaxRadio_->setChecked(true);

    minEdit_ = new QLineEdit(this);
    maxEdit_ = new QLineEdit(this);
    centerEdit_ = new QLineEdit(this);
    windowEdit_ = new QLineEdit(this);
    for (QLineEdit* e : {minEdit_, maxEdit_, centerEdit_, windowEdit_})
        e->setValidator(new QDoubleValidator(e));

    grid->addWidget(minMaxRadio_, 0, 0);
    grid->addWidget(new QLabel("Min:", this), 0, 1);
    grid->addWidget(minEdit_, 0, 2);
    grid->addWidget(new QLabel("Max:", this), 0, 3);
    grid->addWidget(maxEdit_, 0, 4);

    grid->addWidget(centerWindowRadio_, 1, 0);
    grid->addWidget(new QLabel("Center:", this), 1, 1);
    grid->addWidget(centerEdit_, 1, 2);
    grid->addWidget(new QLabel("Window:", this), 1, 3);
    grid->addWidget(windowEdit_, 1, 4);

    connect(minMaxRadio_, &QRadioButton::toggled, this, &ContrastPanel::onRadioToggled);

    auto* updateButton = new QPushButton("Update", this);
    connect(updateButton, &QPushButton::clicked, this, &ContrastPanel::onUpdateClicked);
    grid->addWidget(updateButton, 2, 0, 1, 2);

    colormapCombo_ = new QComboBox(this);
    colormapCombo_->setIconSize(QSize(80, 14));
    for (int i = 0; i < static_cast<int>(giv::colormaps::Id::Count); ++i)
    {
        auto id = static_cast<giv::colormaps::Id>(i);
        colormapCombo_->addItem(QIcon(colormapSwatch(id, 80, 14)), giv::colormaps::name(id), i);
    }
    connect(colormapCombo_, &QComboBox::activated, this, &ContrastPanel::onColormapPicked);
    grid->addWidget(new QLabel("Color table:", this), 3, 0, 1, 2);
    grid->addWidget(colormapCombo_, 3, 2, 1, 3);

    gammaEdit_ = new QLineEdit("1", this);
    gammaEdit_->setValidator(new QDoubleValidator(0.05, 20.0, 4, gammaEdit_));
    gammaEdit_->setToolTip("Display gamma: above 1 brightens the midtones, below 1 darkens them");
    connect(gammaEdit_, &QLineEdit::editingFinished, this, &ContrastPanel::onGammaEdited);
    auto* gammaReset = new QPushButton("Reset", this);
    connect(gammaReset, &QPushButton::clicked, this, &ContrastPanel::onGammaReset);
    grid->addWidget(new QLabel("Gamma:", this), 4, 0, 1, 2);
    grid->addWidget(gammaEdit_, 4, 2);
    grid->addWidget(gammaReset, 4, 3, 1, 2);

    auto* contrastBox = new QGroupBox("Contrast", this);
    contrastBox->setLayout(grid);

    strengthSlider_ = new QSlider(Qt::Vertical, this);
    strengthSlider_->setRange(0, 100);
    strengthSlider_->setValue(0);
    connect(strengthSlider_, &QSlider::valueChanged, this,
            [this](int v) { histogram_->setStrength(v / 100.0); });

    histogram_ = new HistogramWidget(this);
    connect(viewport_, &VulkanViewport::cursorWorldPosition, this, [this](double x, double y) {
        double v;
        bool ok = viewport_->nativeSampleAt(x, y, v);
        histogram_->setCursorValue(ok, v);
    });
    connect(histogram_, &HistogramWidget::rangeChanged, this, &ContrastPanel::onHandlesDragged);
    connect(histogram_, &HistogramWidget::gammaChanged, this, [this](float g) {
        gammaEdit_->setText(QString::number(g, 'g', 4));
        viewport_->setGamma(g);
    });

    auto* histoLayout = new QGridLayout();
    histoLayout->addWidget(strengthSlider_, 0, 0);
    histoLayout->addWidget(histogram_, 0, 1);
    volumeHistogramCheck_ = new QCheckBox("Whole volume", this);
    volumeHistogramCheck_->setToolTip("Histogram of every slice instead of just the current one");
    volumeHistogramCheck_->hide();
    connect(volumeHistogramCheck_, &QCheckBox::toggled, this, &ContrastPanel::updateHistogram);
    histoLayout->addWidget(volumeHistogramCheck_, 1, 1);
    auto* histoBox = new QGroupBox("Histogram", this);
    histoBox->setLayout(histoLayout);

    auto* autoButton = new QPushButton("Auto Contrast", this);
    autoButton->setToolTip("Stretch to the 0.1%-99.9% percentiles of the current slice");
    connect(autoButton, &QPushButton::clicked, this, &ContrastPanel::onAutoContrast);
    auto* fullButton = new QPushButton("Full range", this);
    fullButton->setToolTip("Stretch to the data's absolute min and max");
    connect(fullButton, &QPushButton::clicked, this, &ContrastPanel::onFullRange);
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch(1);
    buttonRow->addWidget(fullButton);
    buttonRow->addWidget(autoButton);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(contrastBox);
    // The histogram keeps its natural height; spare dock height goes below.
    histoBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    histogram_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(histoBox);
    layout->addLayout(buttonRow);
    layout->addStretch(1);
    setLayout(layout);

    onRadioToggled();
    refreshForImage();
}

void ContrastPanel::updateHistogram()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img || img->sampleType == WIV_SAMPLE_NONE) return;
    const bool wholeVolume = !volumeHistogramCheck_->isHidden() && volumeHistogramCheck_->isChecked();
    histogram_->setHistogram(viewport_->computeHistogram(wholeVolume), img->sampleMin, img->sampleMax);
}

void ContrastPanel::refreshForSlice()
{
    if (!volumeHistogramCheck_->isHidden() && volumeHistogramCheck_->isChecked()) return;
    updateHistogram();
}

void ContrastPanel::refreshForImage()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img || img->sampleType == WIV_SAMPLE_NONE)
    {
        setEnabled(false);
        return;
    }
    setEnabled(true);

    volumeHistogramCheck_->setVisible(viewport_->sliceCount() > 1);
    updateHistogram();

    VulkanViewport::ContrastState state = viewport_->currentContrastState();
    histogram_->setContrastRange(state.min, state.max);
    minEdit_->setText(QString::number(state.min, 'g', 6));
    maxEdit_->setText(QString::number(state.max, 'g', 6));
    centerEdit_->setText(QString::number((state.min + state.max) / 2.0, 'g', 6));
    windowEdit_->setText(QString::number(state.max - state.min, 'g', 6));

    gammaEdit_->setText(QString::number(state.gamma, 'g', 4));
    histogram_->setGamma(state.gamma);

    {
        QSignalBlocker blocker(colormapCombo_);
        colormapCombo_->setCurrentIndex(
            state.colormapEnabled ? static_cast<int>(state.colormapId) : static_cast<int>(giv::colormaps::Id::None));
    }
}

void ContrastPanel::onRadioToggled()
{
    bool useMinMax = minMaxRadio_->isChecked();
    minEdit_->setEnabled(useMinMax);
    maxEdit_->setEnabled(useMinMax);
    centerEdit_->setEnabled(!useMinMax);
    windowEdit_->setEnabled(!useMinMax);
}

void ContrastPanel::onUpdateClicked()
{
    float min, max;
    if (minMaxRadio_->isChecked())
    {
        min = minEdit_->text().toFloat();
        max = maxEdit_->text().toFloat();
    }
    else
    {
        float center = centerEdit_->text().toFloat();
        float window = windowEdit_->text().toFloat();
        min = center - window / 2.0f;
        max = center + window / 2.0f;
    }
    applyContrast(min, max);
}

void ContrastPanel::onAutoContrast()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img) return;
    const auto r = viewport_->clippedRange();
    applyContrast(r.first, r.second);
}

void ContrastPanel::onGammaEdited()
{
    bool ok = false;
    const double g = gammaEdit_->text().toDouble(&ok);
    if (!ok || g <= 0.0)
    {
        gammaEdit_->setText(QString::number(viewport_->currentContrastState().gamma, 'g', 4));
        return;
    }
    histogram_->setGamma(static_cast<float>(g));
    viewport_->setGamma(static_cast<float>(g));
}

void ContrastPanel::onGammaReset()
{
    gammaEdit_->setText("1");
    histogram_->setGamma(1.0f);
    viewport_->setGamma(1.0f);
}

void ContrastPanel::onFullRange()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img) return;
    applyContrast(img->sampleMin, img->sampleMax);
}

void ContrastPanel::onHandlesDragged(float min, float max)
{
    minEdit_->setText(QString::number(min, 'g', 6));
    maxEdit_->setText(QString::number(max, 'g', 6));
    centerEdit_->setText(QString::number((min + max) / 2.0, 'g', 6));
    windowEdit_->setText(QString::number(max - min, 'g', 6));
    viewport_->setContrast(min, max);
}

void ContrastPanel::applyContrast(float min, float max)
{
    minEdit_->setText(QString::number(min, 'g', 6));
    maxEdit_->setText(QString::number(max, 'g', 6));
    centerEdit_->setText(QString::number((min + max) / 2.0, 'g', 6));
    windowEdit_->setText(QString::number(max - min, 'g', 6));
    histogram_->setContrastRange(min, max);
    viewport_->setContrast(min, max);
}


void ContrastPanel::onColormapPicked(int row)
{
    auto id = static_cast<giv::colormaps::Id>(colormapCombo_->itemData(row).toInt());
    viewport_->setColormap(id, id != giv::colormaps::Id::None);
    emit colormapChanged();
}

} // namespace givqt
