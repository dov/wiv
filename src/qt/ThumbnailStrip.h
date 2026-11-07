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
#include <QSet>
#include <QStringList>

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
    void loadVisibleThumbnails();
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
};

} // namespace givqt
