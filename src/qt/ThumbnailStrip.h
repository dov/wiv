// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// ThumbnailStrip.h - a multi-row grid of thumbnails, scrolled sideways for the files of the
// folder being browsed, shown under the canvas. Thumbnails are decoded off
// the GUI thread, only for the items currently visible, and shared with the
// Open dialog's disk cache (see decodeFileThumbnail()).
//
#include <QColor>
#include <QImage>
#include <QListWidget>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>

namespace givqt
{

class ThumbnailStrip : public QListWidget
{
    Q_OBJECT

public:
    static constexpr int kDefaultThumbSize = 88;
    static constexpr int kMinThumbSize = 40;
    static constexpr int kMaxThumbSize = 256; // the decode size

    explicit ThumbnailStrip(QWidget* parent = nullptr);
    ~ThumbnailStrip() override;

    // Shows `files` with `current` highlighted and scrolled into view.
    // Rebuilds the items only when the list itself changed.
    void setBackgroundColor(const QColor& color);
    void setFiles(const QStringList& files, int current);
    // Forgets the items so the next setFiles() rebuilds them and re-requests every
    // thumbnail (the disk cache regenerates those whose file changed).
    void invalidate() { files_.clear(); }
    // Thumbnail edge in device-independent pixels; clamped, and persisted by the caller.
    void setThumbSize(int size);
    // One zoom step, as a Ctrl+wheel notch: +1 larger, -1 smaller.
    void stepThumbSize(int direction);
    int thumbSize() const { return thumbSize_; }
    // Height of exactly one row of thumbnails (the pane's minimum).
    int rowHeight() const;

    // Shows `image` as row's thumbnail (it is stored in the disk cache by the caller).
    void setThumbnail(int row, const QImage& image);

signals:
    void updateFromViewRequested(int row); // context menu on the open image's thumbnail
    void fileClicked(int index);
    void stepRequested(int direction); // wheel over the strip: +1 next, -1 previous
    void thumbSizeChanged(int size);   // Ctrl+wheel zoom

protected:
    QSize sizeHint() const override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // Drags the file under the cursor out to other programs (as a file URL).
    void startDrag(Qt::DropActions supportedActions) override;

private:
    // Re-aims the decoders at what is on screen now: exposed rows first (nearest the middle of the
    // view first), then a margin of half a viewport each side; queued decodes for rows that have
    // scrolled out of that range are dropped and running ones are told to stop.
    void loadVisibleThumbnails();
    void cancelJob(int row);   // stop (or ignore) the decode of one row, if any
    void cancelAllJobs();      // ... of every row
    void jobDone(int row, int gen, int serial, const std::shared_ptr<std::atomic<bool>>& token, const QImage& image);
    // Centers the thumbnails when they don't fill the strip's width (left margin on the viewport).
    void updateCentering();
    // Drops the cached thumbnails of `rows` (on disk and in memory) and decodes them again.
    void regenerate(const QList<int>& rows);

    int thumbSize_ = kDefaultThumbSize;
    QColor background_ = Qt::white;
    QStringList files_;
    QSet<int> requested_; // rows already queued, or done
    QSet<int> regenerate_; // rows whose next decode must bypass the disk cache
    bool ignoreClick_ = false; // the release ending a double-click must not select again
    int generation_ = 0;  // invalidates results of a previous file list

    // Decode scheduling. The queue is shared with the worker threads (which pop the best-ranked
    // job themselves, so re-ranking is just rewriting the ranks); jobs_ is the GUI thread's view of
    // the rows that are queued or running, with the token that cancels each.
    struct Queue;
    struct JobInfo
    {
        std::shared_ptr<std::atomic<bool>> token;
        bool fresh = false;
    };
    std::shared_ptr<Queue> queue_;
    QHash<int, JobInfo> jobs_;
    QThreadPool pool_;
    QTimer scrollTimer_; // coalesces the per-pixel scroll signals into one re-prioritisation
};

} // namespace givqt
