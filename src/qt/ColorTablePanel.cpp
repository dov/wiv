// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ColorTablePanel.h"

#include "Colormaps.h"
#include "ImagePluginHost.h"
#include "VulkanViewport.h"

#include <QColor>
#include <QImage>
#include <QListWidget>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace givqt
{

QPixmap colormapSwatch(giv::colormaps::Id id, int width, int height)
{
    QImage img(width, height, QImage::Format_RGB32);
    const uint8_t* lut = giv::colormaps::lut(id);
    for (int x = 0; x < width; ++x)
    {
        int idx = width > 1 ? x * 255 / (width - 1) : 0;
        QColor c(lut[idx * 3 + 0], lut[idx * 3 + 1], lut[idx * 3 + 2]);
        for (int y = 0; y < height; ++y)
            img.setPixelColor(x, y, c);
    }
    return QPixmap::fromImage(img);
}

namespace
{
constexpr int kSwatchRole = Qt::UserRole + 1;
constexpr int kSwatchSrcWidth = 256;
constexpr int kSwatchHeight = 14;
constexpr int kPad = 4;

// Draws the colormap name on one line and a full-width swatch beneath it, so
// a row always fits the list's width (no horizontal scroll, whatever the
// dialog size or name length).
class ColormapDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return QSize(0, option.fontMetrics.height() + kSwatchHeight + 3 * kPad);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const bool selected = option.state & QStyle::State_Selected;
        if (selected)
            p->fillRect(option.rect, option.palette.highlight());
        else if (option.state & QStyle::State_MouseOver)
            p->fillRect(option.rect, option.palette.alternateBase());

        QRect r = option.rect.adjusted(kPad, kPad, -kPad, -kPad);
        p->setPen(selected ? option.palette.highlightedText().color() : option.palette.text().color());
        QRect textRect(r.left(), r.top(), r.width(), option.fontMetrics.height());
        p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                    option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, r.width()));

        QRect swatchRect(r.left(), textRect.bottom() + kPad, r.width(), kSwatchHeight);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        p->drawPixmap(swatchRect, index.data(kSwatchRole).value<QPixmap>());
        p->setPen(option.palette.mid().color());
        p->drawRect(swatchRect.adjusted(0, 0, -1, -1));
    }
};

} // namespace

ColorTablePanel::ColorTablePanel(VulkanViewport* viewport, QWidget* parent) : QWidget(parent), viewport_(viewport)
{
    setMinimumWidth(200);

    list_ = new QListWidget(this);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMouseTracking(true);
    list_->setItemDelegate(new ColormapDelegate(list_));

    // "None" first (giv's PseudoColorNone), then every static/procedural
    // colormap in Colormaps::Id order.
    for (int i = 0; i < static_cast<int>(giv::colormaps::Id::Count); ++i)
    {
        auto id = static_cast<giv::colormaps::Id>(i);
        auto* item = new QListWidgetItem(giv::colormaps::name(id), list_);
        item->setData(Qt::UserRole, i);
        item->setData(kSwatchRole, colormapSwatch(id, kSwatchSrcWidth, kSwatchHeight));
    }

    connect(list_, &QListWidget::currentRowChanged, this, &ColorTablePanel::onSelectionChanged);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list_);
    setLayout(layout);

    refreshForImage();
}

void ColorTablePanel::refreshForImage()
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

void ColorTablePanel::onSelectionChanged()
{
    QListWidgetItem* item = list_->currentItem();
    if (!item) return;
    auto id = static_cast<giv::colormaps::Id>(item->data(Qt::UserRole).toInt());
    viewport_->setColormap(id, id != giv::colormaps::Id::None);
    emit colormapChanged();
}

} // namespace givqt
