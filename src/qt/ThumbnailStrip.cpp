// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "ThumbnailStrip.h"

#include "OpenFileDialog.h"

#include "ImageDownsample.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QMenu>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QScrollBar>
#include <QWheelEvent>
#include <QThread>

#include <algorithm>
#include <mutex>
#include <vector>

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
        if (px < side)
            p.drawImage(0, 0, giv::downscaleToFit(image.copy(src), px, px)); // area average: no aliasing
        else
            p.drawImage(QRect(0, 0, px, px), image, src);
    }
    else
    {
        const QImage scaled = giv::downscaleToFit(image, px, px);
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

struct ThumbnailStrip::Queue
{
    struct Job
    {
        int row = 0;
        QString path;
        bool fresh = false;
        int serial = 0;
        int gen = 0;
        int rank = 0; // lower runs first
        std::shared_ptr<std::atomic<bool>> token;
    };
    std::mutex mutex;
    std::vector<Job> pending;
};

namespace
{
constexpr int kMarginRank = 1 << 20; // ranks at or above this are the off-screen margin, behind everything exposed
}

ThumbnailStrip::ThumbnailStrip(QWidget* parent) : QListWidget(parent), queue_(std::make_shared<Queue>())
{
    // Each in-flight decode holds a full-resolution image, so the worker count is bounded (and a
    // core is left for the GUI) rather than using one thread per core.
    pool_.setMaxThreadCount(std::clamp(QThread::idealThreadCount() - 1, 2, 8));
    scrollTimer_.setSingleShot(true);
    scrollTimer_.setInterval(0);
    connect(&scrollTimer_, &QTimer::timeout, this, [this]() { loadVisibleThumbnails(); });

    setViewMode(QListView::IconMode);
    setFlow(QListView::TopToBottom); // fills a column, then wraps to the next: several rows, scrolled sideways
    setWrapping(true);
    setMovement(QListView::Static);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
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

    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        // A double-click counts as its single click: the first click already
        // selected and scrolled the strip, so the item now under the cursor is another one.
        if (ignoreClick_) return;
        emit fileClicked(row(item));
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { scrollTimer_.start(); });
}

ThumbnailStrip::~ThumbnailStrip()
{
    cancelAllJobs();
    pool_.clear();
    pool_.waitForDone(); // workers post their results back to this object
}

void ThumbnailStrip::mousePressEvent(QMouseEvent* event)
{
    ignoreClick_ = false;
    QListWidget::mousePressEvent(event);
}

void ThumbnailStrip::mouseDoubleClickEvent(QMouseEvent* event)
{
    ignoreClick_ = true;
    QListWidget::mouseDoubleClickEvent(event);
}

void ThumbnailStrip::startDrag(Qt::DropActions)
{
    QListWidgetItem* it = currentItem();
    if (!it) return;
    const int r = row(it);
    if (r < 0 || r >= files_.size()) return;
    const QString path = files_[r];
    if (!QFileInfo::exists(path)) return;

    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(path)});
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap pm = it->data(kThumbRole).value<QPixmap>();
    if (!pm.isNull()) {
        drag->setPixmap(pm.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setHotSpot(QPoint(32, 32));
    }
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

void ThumbnailStrip::setFiles(const QStringList& files, int current)
{
    if (files != files_)
    {
        cancelAllJobs();
        files_ = files;
        ++generation_;
        requested_.clear();
        regenerate_.clear();
        clear();
        for (const QString& f : files)
        {
            auto* item = new QListWidgetItem(this);
            item->setToolTip(QFileInfo(f).fileName());
            item->setSizeHint(gridSize());
        }
    }
    updateCentering();
    if (current >= 0 && current < count())
    {
        setCurrentRow(current);
        scrollToItem(item(current), QAbstractItemView::PositionAtCenter);
    }
    loadVisibleThumbnails();
}

void ThumbnailStrip::updateCentering()
{
    // Items fill a column of `perColumn` cells, then wrap to the next column.
    const int cell = gridSize().width();
    const int cellH = gridSize().height();
    const int perColumn = std::max(1, height() / std::max(1, cellH));
    const int columns = (count() + perColumn - 1) / perColumn;
    const int content = columns * cell;
    const int avail = width();
    const int margin = (count() > 0 && content < avail) ? (avail - content) / 2 : 0;
    if (viewportMargins().left() != margin)
        setViewportMargins(margin, 0, 0, 0);
}

int ThumbnailStrip::rowHeight() const
{
    return thumbSize_ + 12 + horizontalScrollBar()->sizeHint().height() + 4;
}

void ThumbnailStrip::stepThumbSize(int direction)
{
    setThumbSize(thumbSize_ + (direction > 0 ? 1 : -1) * std::max(4, thumbSize_ / 8));
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
    updateCentering();
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

void ThumbnailStrip::cancelJob(int row)
{
    const auto it = jobs_.find(row);
    if (it == jobs_.end()) return;
    const JobInfo info = it.value();
    jobs_.erase(it);
    info.token->store(true); // a running decode that hasn't loaded yet gives up; its result is ignored
    {
        const std::lock_guard<std::mutex> lock(queue_->mutex);
        std::erase_if(queue_->pending, [&](const Queue::Job& j) { return j.token == info.token; });
    }
    requested_.remove(row); // so it is asked for again when it is next exposed
    if (info.fresh) regenerate_.insert(row);
}

void ThumbnailStrip::cancelAllJobs()
{
    {
        const std::lock_guard<std::mutex> lock(queue_->mutex);
        queue_->pending.clear();
    }
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
    {
        it->token->store(true);
        requested_.remove(it.key());
        if (it->fresh) regenerate_.insert(it.key());
    }
    jobs_.clear();
}

void ThumbnailStrip::jobDone(int row, int gen, int serial, const std::shared_ptr<std::atomic<bool>>& token, const QImage& image)
{
    const auto it = jobs_.find(row);
    if (it != jobs_.end() && it->token == token) jobs_.erase(it);
    if (token->load() || gen != generation_ || row >= count() || image.isNull()) return;
    if (item(row)->data(kSerialRole).toInt() != serial) return; // regenerated meanwhile
    item(row)->setData(kImageRole, image);
    item(row)->setData(kThumbRole, makeSquareThumbnail(image, thumbSize_, devicePixelRatioF(), background_));
    viewport()->update();
}

void ThumbnailStrip::loadVisibleThumbnails()
{
    if (!isVisible())
    {
        cancelAllJobs();
        return;
    }
    const QRect view = viewport()->rect();
    const int margin = view.width() / 2; // a little beyond the visible edge so scrolling doesn't show blanks
    const int viewCentre = view.center().x();

    // Items fill columns top to bottom, so the rows near the view are a contiguous block; only those
    // are examined (with a couple of columns of slack), then placed exactly by their real rectangle.
    const int cellW = std::max(1, gridSize().width());
    const int perColumn = std::max(1, view.height() / std::max(1, gridSize().height()));
    const int scrollX = horizontalScrollBar()->value();
    const int firstCol = std::max(0, (scrollX - margin) / cellW - 2);
    const int lastCol = (scrollX + view.width() + margin) / cellW + 2;
    const int firstRow = std::min(count(), firstCol * perColumn);
    const int endRow = std::min(count(), (lastCol + 1) * perColumn);

    QHash<int, int> wanted; // row -> rank
    for (int i = firstRow; i < endRow; ++i)
    {
        const QRect r = visualItemRect(item(i));
        const int dist = std::abs(r.center().x() - viewCentre);
        if (r.intersects(view)) wanted.insert(i, dist);
        else if (r.adjusted(-margin, 0, margin, 0).intersects(view)) wanted.insert(i, kMarginRank + dist);
    }

    // Anything queued or running for rows no longer wanted is abandoned: all effort goes to what is exposed.
    QList<int> stale;
    for (auto it = jobs_.cbegin(); it != jobs_.cend(); ++it)
        if (!wanted.contains(it.key())) stale.append(it.key());
    for (int row : stale) cancelJob(row);

    std::vector<Queue::Job> added;
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it)
    {
        const int i = it.key();
        if (requested_.contains(i)) continue; // done, or queued/running (re-ranked below)
        requested_.insert(i);
        Queue::Job job;
        job.row = i;
        job.path = files_[i];
        job.fresh = regenerate_.remove(i);
        job.serial = item(i)->data(kSerialRole).toInt();
        job.gen = generation_;
        job.rank = it.value();
        job.token = std::make_shared<std::atomic<bool>>(false);
        jobs_.insert(i, JobInfo{job.token, job.fresh});
        added.push_back(std::move(job));
    }
    const size_t nAdded = added.size();
    {
        const std::lock_guard<std::mutex> lock(queue_->mutex);
        for (Queue::Job& j : queue_->pending) // rows that stayed wanted may have changed rank
            if (const auto w = wanted.constFind(j.row); w != wanted.cend()) j.rank = w.value();
        for (Queue::Job& j : added) queue_->pending.push_back(std::move(j));
    }

    // One worker task per new job. A task takes whichever pending job currently ranks best, so the
    // job it ends up running is the most urgent one, not the one it was submitted for.
    for (size_t k = 0; k < nAdded; ++k)
    {
        pool_.start([q = queue_, this]() {
            Queue::Job job;
            {
                const std::lock_guard<std::mutex> lock(q->mutex);
                if (q->pending.empty()) return; // the job this task was for was dropped
                auto best = std::min_element(q->pending.begin(), q->pending.end(), [](const Queue::Job& a, const Queue::Job& b) {
                    return a.rank != b.rank ? a.rank < b.rank : a.row < b.row;
                });
                job = std::move(*best);
                *best = std::move(q->pending.back());
                q->pending.pop_back();
            }
            QImage image;
            if (!job.token->load(std::memory_order_relaxed)) image = decodeFileThumbnail(job.path, job.fresh, job.token.get());
            QMetaObject::invokeMethod(
                this, [this, row = job.row, gen = job.gen, serial = job.serial, token = job.token, image]() { jobDone(row, gen, serial, token, image); },
                Qt::QueuedConnection);
        });
    }
}

void ThumbnailStrip::regenerate(const QList<int>& rows)
{
    for (int i : rows)
    {
        if (i < 0 || i >= count()) continue;
        cancelJob(i);
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

void ThumbnailStrip::setThumbnail(int row, const QImage& image)
{
    if (row < 0 || row >= count() || image.isNull()) return;
    cancelJob(row);
    regenerate_.remove(row);
    QListWidgetItem* it = item(row);
    it->setData(kSerialRole, it->data(kSerialRole).toInt() + 1); // drop any decode still in flight
    it->setData(kImageRole, image);
    it->setData(kThumbRole, makeSquareThumbnail(image, thumbSize_, devicePixelRatioF(), background_));
    requested_.insert(row);
    viewport()->update();
}

void ThumbnailStrip::contextMenuEvent(QContextMenuEvent* event)
{
    QListWidgetItem* under = itemAt(event->pos());
    QMenu menu(this);
    // Only the open image has a current view; .giv/.svg are scenes, not images with a contrast window.
    const bool isOpenImage = under && row(under) == currentRow() && !files_[row(under)].endsWith(".giv", Qt::CaseInsensitive) &&
                             !files_[row(under)].endsWith(".svg", Qt::CaseInsensitive);
    QAction* fromView = isOpenImage ? menu.addAction(tr("Update Thumbnail from Current View")) : nullptr;
    QAction* one = menu.addAction(tr("Regenerate Thumbnail"));
    one->setEnabled(under != nullptr);
    QAction* all = menu.addAction(tr("Regenerate All Thumbnails"));
    all->setEnabled(count() > 0);
    QAction* chosen = menu.exec(event->globalPos());
    if (fromView && chosen == fromView)
    {
        emit updateFromViewRequested(row(under));
    }
    else if (chosen == one && under)
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
        if (dy != 0) stepThumbSize(dy > 0 ? 1 : -1);
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
    updateCentering();
    loadVisibleThumbnails();
}

void ThumbnailStrip::showEvent(QShowEvent* event)
{
    QListWidget::showEvent(event);
    if (currentRow() >= 0) scrollToItem(currentItem(), QAbstractItemView::PositionAtCenter);
    loadVisibleThumbnails();
}

QSize ThumbnailStrip::sizeHint() const
{
    // One row of thumbnails: the pane opens just tall enough to fit them.
    return QSize(QListWidget::sizeHint().width(), rowHeight());
}

} // namespace givqt
