#include "ContrastDialog.h"

#include "ColorTableDialog.h"
#include "Colormaps.h"
#include "HistogramWidget.h"
#include "ImagePluginHost.h"
#include "VulkanViewport.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDialogButtonBox>
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

ContrastDialog::ContrastDialog(VulkanViewport* viewport, QWidget* parent) : QDialog(parent), viewport_(viewport)
{
    setWindowTitle("Giv Contrast");
    resize(420, 380);

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

    connect(minMaxRadio_, &QRadioButton::toggled, this, &ContrastDialog::onRadioToggled);

    auto* updateButton = new QPushButton("Update", this);
    connect(updateButton, &QPushButton::clicked, this, &ContrastDialog::onUpdateClicked);
    grid->addWidget(updateButton, 2, 0, 1, 2);

    colormapCombo_ = new QComboBox(this);
    colormapCombo_->setIconSize(QSize(80, 14));
    for (int i = 0; i < static_cast<int>(giv::colormaps::Id::Count); ++i)
    {
        auto id = static_cast<giv::colormaps::Id>(i);
        colormapCombo_->addItem(QIcon(colormapSwatch(id, 80, 14)), giv::colormaps::name(id), i);
    }
    connect(colormapCombo_, &QComboBox::activated, this, &ContrastDialog::onColormapPicked);
    grid->addWidget(new QLabel("Color table:", this), 3, 0, 1, 2);
    grid->addWidget(colormapCombo_, 3, 2, 1, 3);

    auto* contrastBox = new QGroupBox("Contrast", this);
    contrastBox->setLayout(grid);

    strengthSlider_ = new QSlider(Qt::Vertical, this);
    strengthSlider_->setRange(0, 100);
    strengthSlider_->setValue(100);
    connect(strengthSlider_, &QSlider::valueChanged, this,
            [this](int v) { histogram_->setStrength(v / 100.0); });

    histogram_ = new HistogramWidget(this);
    connect(histogram_, &HistogramWidget::rangeChanged, this, &ContrastDialog::onHandlesDragged);

    auto* histoLayout = new QGridLayout();
    histoLayout->addWidget(strengthSlider_, 0, 0);
    histoLayout->addWidget(histogram_, 0, 1);
    auto* histoBox = new QGroupBox("Histogram", this);
    histoBox->setLayout(histoLayout);

    auto* buttons = new QDialogButtonBox(this);
    auto* autoButton = buttons->addButton("Auto Contrast", QDialogButtonBox::ActionRole);
    connect(autoButton, &QPushButton::clicked, this, &ContrastDialog::onAutoContrast);
    buttons->addButton(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(buttons, &QDialogButtonBox::clicked, this, [this, buttons](QAbstractButton* b) {
        if (buttons->buttonRole(b) != QDialogButtonBox::ActionRole) close();
    });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(contrastBox);
    layout->addWidget(histoBox, /*stretch=*/1);
    layout->addWidget(buttons);
    setLayout(layout);

    onRadioToggled();
    refreshForImage();
}

void ContrastDialog::refreshForImage()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img || img->sampleType == WIV_SAMPLE_NONE)
    {
        setEnabled(false);
        return;
    }
    setEnabled(true);

    histogram_->setHistogram(viewport_->computeHistogram(), img->sampleMin, img->sampleMax);

    VulkanViewport::ContrastState state = viewport_->currentContrastState();
    histogram_->setContrastRange(state.min, state.max);
    minEdit_->setText(QString::number(state.min, 'g', 6));
    maxEdit_->setText(QString::number(state.max, 'g', 6));
    centerEdit_->setText(QString::number((state.min + state.max) / 2.0, 'g', 6));
    windowEdit_->setText(QString::number(state.max - state.min, 'g', 6));

    {
        QSignalBlocker blocker(colormapCombo_);
        colormapCombo_->setCurrentIndex(
            state.colormapEnabled ? static_cast<int>(state.colormapId) : static_cast<int>(giv::colormaps::Id::None));
    }
}

void ContrastDialog::onRadioToggled()
{
    bool useMinMax = minMaxRadio_->isChecked();
    minEdit_->setEnabled(useMinMax);
    maxEdit_->setEnabled(useMinMax);
    centerEdit_->setEnabled(!useMinMax);
    windowEdit_->setEnabled(!useMinMax);
}

void ContrastDialog::onUpdateClicked()
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

void ContrastDialog::onAutoContrast()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img) return;
    applyContrast(img->sampleMin, img->sampleMax);
}

void ContrastDialog::onHandlesDragged(float min, float max)
{
    minEdit_->setText(QString::number(min, 'g', 6));
    maxEdit_->setText(QString::number(max, 'g', 6));
    centerEdit_->setText(QString::number((min + max) / 2.0, 'g', 6));
    windowEdit_->setText(QString::number(max - min, 'g', 6));
    viewport_->setContrast(min, max);
}

void ContrastDialog::applyContrast(float min, float max)
{
    minEdit_->setText(QString::number(min, 'g', 6));
    maxEdit_->setText(QString::number(max, 'g', 6));
    centerEdit_->setText(QString::number((min + max) / 2.0, 'g', 6));
    windowEdit_->setText(QString::number(max - min, 'g', 6));
    histogram_->setContrastRange(min, max);
    viewport_->setContrast(min, max);
}


void ContrastDialog::onColormapPicked(int row)
{
    auto id = static_cast<giv::colormaps::Id>(colormapCombo_->itemData(row).toInt());
    viewport_->setColormap(id, id != giv::colormaps::Id::None);
    emit colormapChanged();
}

} // namespace givqt
