#pragma once
//
// OpenFileDialog.h - wiv's File > Open dialog: a QFileDialog subclass (so
// it keeps every bit of the native look - Look-in bar, back/forward/up/new-
// folder buttons, File name/Files of type fields) with three additions
// modeled on ~/xjet/git/the reference application's own thumbnail-file-dialog:
//
//   - inline thumbnails painted directly into the file list/table rows
//     (ThumbnailDelegate, in the .cpp) for every filetype wiv can load -
//     see giv::ImagePluginHost::isSupported() plus the .giv/.svg special
//     cases - decoded off the GUI thread and streamed in as they finish;
//   - a directory-tree sidebar with quick-access places, Recent, Bookmarks
//     and My Computer/drives, replacing QFileDialog's flat built-in one;
//   - List/Detail view-mode buttons plus a grid zoom (Ctrl+wheel or the
//     zoom buttons), matching the reference dialog's toolbar.
//
// Unlike the reference application's version this has no 3D-mesh-specific pieces (no VSG
// device, no "Load as proxy", no regenerate/inspect context menu): wiv's
// thumbnails are plain CPU decodes - raster images via ImagePluginHost,
// .svg via nanosvg, and .giv scenes via GivThumbnailRenderer (a lightweight
// QPainter rasterizer, independent of the VSG/Vulkan SceneBuilder pipeline
// so it can decode off-thread with no GPU device) - not GPU-rendered mesh
// previews, so none of the reference application's mesh-thumbnail machinery is needed.
//
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>

class QListView;
class QTreeView;
class QToolButton;
class QPushButton;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QSettings;
class QLabel;
class QTimer;
namespace fa { class QtAwesome; }

namespace givqt
{

enum class ThumbnailState { Queued, Ready, Failed };

class ThumbnailDelegate;

class OpenFileDialog : public QFileDialog
{
    Q_OBJECT

public:
    enum class ViewMode { List, Detail };

    // `settings` is not owned; pass the same QSettings("wiv","wiv") instance
    // MainWindow already uses for its own preferences.
    explicit OpenFileDialog(QWidget* parent, QSettings* settings);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void zoomIn();
    void zoomOut();
    void updateThumbnails();
    void onDirectoryChanged(const QString& path);
    void onPlaceClicked(const QModelIndex& index);
    void onDirectoryEnteredForRecents(const QString& path);
    void onAcceptedForRecents();
    void onPlacesContextMenu(const QPoint& pos);
    void onRemoveSelectedRecent();
    void onAddBookmarkClicked();
    void onViewModeButtonClicked(ViewMode mode);

public slots:
    void fillThumbnailCache(const QString& directory);

private:
    void adjustGridSize();
    void setThumbnailCache(const QString& filePath, const QImage& thumbnail);
    void saveViewMode();
    void loadViewMode();
    void buildPlacesModel();
    void populateDrives();
    QIcon placesFolderIcon() const;
    void installPlacesTree();
    void refreshRecentNode();
    void addRecentDirectory(const QString& path);
    void removeRecentDirectory(const QString& path);
    void refreshBookmarksNode();
    void addBookmark(const QString& path);
    void removeBookmark(const QString& path);
    bool confirmBookmarkRemoval(const QString& path);
    void installViewModeToolbar();
    void applyViewMode(ViewMode mode);
    void updateViewModeButtonChecks();
    void updateLoadingOverlay();
    void requestThumbnail(const QString& path, quint64 generation);
    void onThumbnailReady(const QString& path, const QImage& image, bool success, quint64 generation);

    static constexpr int kMaxRecents = 10;

    int thumbnailSize_ = 96;
    ViewMode viewMode_ = ViewMode::List;
    ThumbnailDelegate* listThumbnailDelegate_ = nullptr;
    ThumbnailDelegate* treeThumbnailDelegate_ = nullptr;
    QListView* listView_ = nullptr;
    QTreeView* treeView_ = nullptr;

    QTreeView* placesTree_ = nullptr;
    QSplitter* leftSplitter_ = nullptr;
    QPushButton* addBookmarkButton_ = nullptr;
    QStandardItemModel* placesModel_ = nullptr;
    QStandardItem* recentNode_ = nullptr;
    QStandardItem* bookmarksNode_ = nullptr;
    QStandardItem* myComputerNode_ = nullptr;

    QSettings* settings_ = nullptr;
    fa::QtAwesome* awesome_ = nullptr;

    QToolButton* listModeButton_ = nullptr;
    QToolButton* detailModeButton_ = nullptr;

    // Coalesces per-thumbnail viewport repaints so a burst of async decodes
    // finishing together triggers one paint instead of one per arrival.
    QTimer* thumbnailRepaintTimer_ = nullptr;

    QFileSystemWatcher* fileSystemWatcher_ = nullptr;

    // Accessed by ThumbnailDelegate::paint() (friend) - main thread only.
    QHash<QString, QImage> thumbnailCache_;
    QHash<QString, ThumbnailState> thumbnailState_;

    // Bumped every time fillThumbnailCache() switches directories, so an
    // async decode still in flight for a directory the user has since left
    // is recognized as stale and its result dropped.
    quint64 generation_ = 0;
    QString lastFillDirectory_;

    // False while the directory listing is still being gathered/scanned -
    // drives the "Loading..." overlay so the freshly-opened dialog isn't
    // blank for however long a large directory takes to enumerate.
    bool directoryLoaded_ = false;
    QSet<QString> loadedDirectories_;
    QLabel* loadingOverlay_ = nullptr;

    friend class ThumbnailDelegate;
};

} // namespace givqt
