#include "ThumbnailStrip.h"

#include "OpenFileDialog.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <algorithm>

namespace givqt
{

namespace
{
constexpr int kCellMargin = 6;      // between the cell edge and the thumbnail square
constexpr int kCornerRadius = 6;
constexpr int kThumbRole = Qt::UserRole + 1; // the prepared square QPixmap
constexpr int kImageRole = Qt::UserRole + 2; // the decoded thumbnail QImage it was made from
constexpr int kSerialRole = Qt::UserRole + 3; // bumped per (re)generation so a superseded decode's result is dropped
constexpr double kMaxCropAspect = 1.5;       // beyond this the whole image is shown, not cropped
constexpr int kDimAlpha = 26;                // 10% black over the inactive thumbnails

// Square thumbnail: a centre crop that fills the square when the image
// is close to square, otherwise the whole image letterboxed onto a neutral
// background so a panorama stays recognisable.
QPixmap makeSquareThumbnail(const QImage& image, int size, qreal dpr, const QColor& background)
{
    const int px = qRound(size * dpr);
    QImage out(px, px, QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double aspect = double(std::max(image.width(), image.height())) / std::max(1, std::min(image.width(), image.height()));
    if (aspect <= kMaxCropAspect)
    {
        const int side = std::min(image.width(), image.height());
        const QRect src((image.width() - side) / 2, (image.height() - side) / 2, side, side);
        p.drawImage(QRect(0, 0, px, px), image, src);
    }
    else
    {
        const QImage scaled = image.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((px - scaled.width()) / 2, (px - scaled.height()) / 2, scaled);
    }
    p.end();
    QPixmap pm = QPixmap::fromImage(out);
    pm.setDevicePixelRatio(dpr);
    return pm;
}

class StripDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const QRect r = option.rect.adjusted(kCellMargin, kCellMargin, -kCellMargin, -kCellMargin);
        const bool current = option.state & QStyle::State_Selected;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(r, kCornerRadius, kCornerRadius);
        painter->setClipPath(clip);
        const QPixmap pm = index.data(kThumbRole).value<QPixmap>();
        if (pm.isNull())
            painter->fillRect(r, option.palette.color(QPalette::Base));
        else
            painter->drawPixmap(r.topLeft(), pm);
        if (!current) painter->fillRect(r, QColor(0, 0, 0, kDimAlpha));
        painter->setClipping(false);
        if (current)
        {
            QPen pen(option.palette.color(QPalette::Highlight), 3);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(r).adjusted(-1.5, -1.5, 1.5, 1.5), kCornerRadius + 1, kCornerRadius + 1);
        }
        painter->restore();
    }
};
} // namespace

ThumbnailStrip::ThumbnailStrip(QWidget* parent) : QListWidget(parent)
{
    setViewMode(QListView::IconMode);
    setFlow(QListView::TopToBottom); // fills a column, then wraps to the next: several rows, scrolled sideways
    setWrapping(true);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);
    setIconSize(QSize(thumbSize_, thumbSize_));
    setGridSize(QSize(thumbSize_ + 12, thumbSize_ + 12));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(rowHeight());
    setFrameShape(QFrame::NoFrame);
    setItemDelegate(new StripDelegate(this));
    // Paint with the window colour (not the list's own Base colour) so the
    // strip blends with the rest of the window like the scroll bars do.
    viewport()->setBackgroundRole(QPalette::Window);
    viewport()->setAutoFillBackground(true);

    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { emit fileClicked(row(item)); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { loadVisibleThumbnails(); });
}

void ThumbnailStrip::setFiles(const QStringList& files, int current)
{
    if (files != files_)
    {
        files_ = files;
        ++generation_;
        requested_.clear();
        clear();
        for (const QString& f : files)
        {
            auto* item = new QListWidgetItem(this);
            item->setToolTip(QFileInfo(f).fileName());
            item->setSizeHint(gridSize());
        }
    }
    if (current >= 0 && current < count())
    {
        setCurrentRow(current);
        scrollToItem(item(current), QAbstractItemView::PositionAtCenter);
    }
    loadVisibleThumbnails();
}

int ThumbnailStrip::rowHeight() const
{
    return thumbSize_ + 12 + horizontalScrollBar()->sizeHint().height() + 4;
}

void ThumbnailStrip::setThumbSize(int size)
{
    size = std::clamp(size, kMinThumbSize, kMaxThumbSize);
    if (size == thumbSize_) return;
    thumbSize_ = size;
    setIconSize(QSize(size, size));
    setGridSize(QSize(size + 12, size + 12));
    setMinimumHeight(rowHeight());
    // Uniform item sizes are cached from the first item; drop the cache so the new size takes effect.
    setUniformItemSizes(false);
    for (int i = 0; i < count(); ++i)
    {
        item(i)->setSizeHint(gridSize());
        const QImage image = item(i)->data(kImageRole).value<QImage>();
        if (!image.isNull())
            item(i)->setData(kThumbRole, makeSquareThumbnail(image, thumbSize_, devicePixelRatioF(), background_));
    }
    setUniformItemSizes(true);
    scheduleDelayedItemsLayout();
    if (currentRow() >= 0) scrollToItem(currentItem(), QAbstractItemView::PositionAtCenter);
    viewport()->update();
    loadVisibleThumbnails();
    emit thumbSizeChanged(size);
}

// The colour transparent areas and letterboxing are drawn on - the canvas background.
void ThumbnailStrip::setBackgroundColor(const QColor& color)
{
    background_ = color;
    for (int i = 0; i < count(); ++i)
    {
        const QImage image = item(i)->data(kImageRole).value<QImage>();
        if (!image.isNull())
            item(i)->setData(kThumbRole, makeSquareThumbnail(image, thumbSize_, devicePixelRatioF(), background_));
    }
    viewport()->update();
}

void ThumbnailStrip::loadVisibleThumbnails()
{
    if (!isVisible()) return;
    const QRect view = viewport()->rect();
    for (int i = 0; i < count(); ++i)
    {
        if (requested_.contains(i)) continue;
        // Load a little beyond the visible edge so scrolling doesn't show blanks.
        if (!visualItemRect(item(i)).adjusted(-view.width() / 2, 0, view.width() / 2, 0).intersects(view)) continue;
        requested_.insert(i);
        const QString path = files_[i];
        const bool fresh = regenerate_.remove(i);
        const int serial = item(i)->data(kSerialRole).toInt();
        auto* watcher = new QFutureWatcher<QImage>(this);
        const int gen = generation_;
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, i, gen, serial]() {
            const QImage image = watcher->result();
            watcher->deleteLater();
            if (gen != generation_ || i >= count() || image.isNull()) return;
            if (item(i)->data(kSerialRole).toInt() != serial) return; // regenerated meanwhile
            item(i)->setData(kImageRole, image);
            item(i)->setData(kThumbRole, makeSquareThumbnail(image, thumbSize_, devicePixelRatioF(), background_));
            viewport()->update();
        });
        watcher->setFuture(QtConcurrent::run(decodeFileThumbnail, path, fresh));
    }
}

void ThumbnailStrip::regenerate(const QList<int>& rows)
{
    for (int i : rows)
    {
        if (i < 0 || i >= count()) continue;
        QListWidgetItem* it = item(i);
        it->setData(kSerialRole, it->data(kSerialRole).toInt() + 1);
        it->setData(kThumbRole, QVariant());
        it->setData(kImageRole, QVariant());
        requested_.remove(i);
        regenerate_.insert(i);
    }
    viewport()->update();
    loadVisibleThumbnails(); // the rest follow as they scroll into view
}

void ThumbnailStrip::contextMenuEvent(QContextMenuEvent* event)
{
    QListWidgetItem* under = itemAt(event->pos());
    QMenu menu(this);
    QAction* one = menu.addAction(tr("Regenerate Thumbnail"));
    one->setEnabled(under != nullptr);
    QAction* all = menu.addAction(tr("Regenerate All Thumbnails"));
    all->setEnabled(count() > 0);
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == one && under)
    {
        regenerate({row(under)});
    }
    else if (chosen == all)
    {
        QList<int> rows;
        for (int i = 0; i < count(); ++i) rows.append(i);
        regenerate(rows);
    }
    event->accept();
}

void ThumbnailStrip::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier)
    {
        const int dy = event->angleDelta().y();
        if (dy != 0) setThumbSize(thumbSize_ + (dy > 0 ? 1 : -1) * std::max(4, thumbSize_ / 8));
        event->accept();
        return;
    }
    const int dy = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    if (dy != 0) emit stepRequested(dy > 0 ? -1 : 1);
    event->accept();
}

void ThumbnailStrip::resizeEvent(QResizeEvent* event)
{
    QListWidget::resizeEvent(event);
    loadVisibleThumbnails();
}

void ThumbnailStrip::showEvent(QShowEvent* event)
{
    QListWidget::showEvent(event);
    if (currentRow() >= 0) scrollToItem(currentItem(), QAbstractItemView::PositionAtCenter);
    loadVisibleThumbnails();
}

} // namespace givqt
