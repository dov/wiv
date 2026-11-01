#pragma once
//
// ThumbnailStrip.h - a horizontal strip of thumbnails for the files of the
// folder being browsed, shown under the canvas. Thumbnails are decoded off
// the GUI thread, only for the items currently visible, and shared with the
// Open dialog's disk cache (see decodeFileThumbnail()).
//
#include <QColor>
#include <QListWidget>
#include <QSet>
#include <QStringList>

namespace givqt
{

class ThumbnailStrip : public QListWidget
{
    Q_OBJECT

public:
    static constexpr int kThumbSize = 88;

    explicit ThumbnailStrip(QWidget* parent = nullptr);

    // Shows `files` with `current` highlighted and scrolled into view.
    // Rebuilds the items only when the list itself changed.
    void setBackgroundColor(const QColor& color);
    void setFiles(const QStringList& files, int current);

signals:
    void fileClicked(int index);
    void stepRequested(int direction); // wheel over the strip: +1 next, -1 previous

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void loadVisibleThumbnails();
    void updateCentering();

    QColor background_ = Qt::white;
    QStringList files_;
    QSet<int> requested_; // rows already queued, or done
    int generation_ = 0;  // invalidates results of a previous file list
};

} // namespace givqt
