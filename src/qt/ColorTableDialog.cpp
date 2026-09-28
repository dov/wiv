#include "ColorTableDialog.h"

#include "Colormaps.h"
#include "ImagePluginHost.h"
#include "VulkanViewport.h"

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QListWidget>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSize>
#include <QVBoxLayout>

namespace givqt
{

namespace
{
constexpr int kSwatchWidth = 200;
constexpr int kSwatchHeight = 16;

QPixmap swatchFor(giv::colormaps::Id id)
{
    QImage img(kSwatchWidth, kSwatchHeight, QImage::Format_RGB32);
    const uint8_t* lut = giv::colormaps::lut(id);
    for (int x = 0; x < kSwatchWidth; ++x)
    {
        int idx = x * 255 / (kSwatchWidth - 1);
        QColor c(lut[idx * 3 + 0], lut[idx * 3 + 1], lut[idx * 3 + 2]);
        for (int y = 0; y < kSwatchHeight; ++y)
            img.setPixelColor(x, y, c);
    }
    return QPixmap::fromImage(img);
}

} // namespace

ColorTableDialog::ColorTableDialog(VulkanViewport* viewport, QWidget* parent) : QDialog(parent), viewport_(viewport)
{
    setWindowTitle("Color Table");
    resize(260, 320);

    list_ = new QListWidget(this);
    list_->setIconSize(QSize(kSwatchWidth, kSwatchHeight));

    // "None" first (giv's PseudoColorNone), then every static/procedural
    // colormap in Colormaps::Id order.
    for (int i = 0; i < static_cast<int>(giv::colormaps::Id::Count); ++i)
    {
        auto id = static_cast<giv::colormaps::Id>(i);
        auto* item = new QListWidgetItem(QIcon(swatchFor(id)), giv::colormaps::name(id), list_);
        item->setData(Qt::UserRole, i);
    }

    connect(list_, &QListWidget::currentRowChanged, this, &ColorTableDialog::onSelectionChanged);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list_);
    setLayout(layout);

    refreshForImage();
}

void ColorTableDialog::refreshForImage()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    if (!img || img->sampleType == WIV_SAMPLE_NONE)
    {
        setEnabled(false);
        return;
    }
    setEnabled(true);

    VulkanViewport::ContrastState state = viewport_->currentContrastState();
    int row = state.colormapEnabled ? static_cast<int>(state.colormapId) : static_cast<int>(giv::colormaps::Id::None);
    QSignalBlocker blocker(list_);
    list_->setCurrentRow(row);
}

void ColorTableDialog::onSelectionChanged()
{
    QListWidgetItem* item = list_->currentItem();
    if (!item) return;
    auto id = static_cast<giv::colormaps::Id>(item->data(Qt::UserRole).toInt());
    viewport_->setColormap(id, id != giv::colormaps::Id::None);
}

} // namespace givqt
