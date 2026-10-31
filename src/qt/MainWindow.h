#pragma once
//
// MainWindow.h - the wiv application shell: menu bar, status bar, wrapping
// a central VulkanViewport. Deliberately no toolbar or side panel - wiv
// aims to match giv's own look/behavior, which has neither.
//
#include <QMainWindow>
#include <QDateTime>
#include <QList>
#include <QStringList>

#include "ExifReader.h"

#include <memory>
#include <string>
#include <vector>

class QLabel;
class QAction;
class QMenu;
class QDialog;
class QDockWidget;
class QShortcut;
class QShowEvent;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QTimer;

namespace givqt
{

class VulkanViewport;
class MarkTreeView;
class ContrastPanel;
class ColorMapBar;
class ColorTablePanel;
class InfoPanel;
class RpcServer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Loads `paths` into the viewport (used for files given on the command
    // line at startup).
    void loadFiles(const std::vector<std::string>& paths);

    // Starts the json-rpc remote-control server (see RpcServer.h) on
    // 127.0.0.1:port. Not started unless explicitly requested (--rpc-port),
    // unlike giv's own always-on-by-default json-rpc server - an image
    // viewer shouldn't open a listening socket by default. Returns false on
    // failure (e.g. the port is already in use).
    bool startRpcServer(int port);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    VulkanViewport* viewport_ = nullptr;

    // Single status-bar label, mirroring giv's own w_info_label: shows
    // "Loaded: <file>" right after a load, then gets overwritten by the
    // cursor position (and, mid-measurement, the caliper distance) on every
    // mouse move - see the connections built in the constructor.
    QLabel* infoLabel_ = nullptr;
    QString lastCursorText_;
    QString lastMeasureText_;
    QString lastSliceText_; // "Slice N/D" for the current multi-slice image, empty otherwise - see sliceChanged
    QString loadedBaseName_;
    QStringList loadedPaths_; // absolute paths of the file(s) last passed to loadFilesInternal()
    QString lastLoadSummary_;

    QAction* menuBarAction_ = nullptr;
    QAction* subPixelAction_ = nullptr;
    QAction* horizontalFlipAction_ = nullptr;
    QAction* verticalFlipAction_ = nullptr;
    QAction* verticalZoomLockAction_ = nullptr;
    QAction* showImageAction_ = nullptr;
    QAction* colorMapBarAction_ = nullptr;
    QAction* autoContrastAction_ = nullptr;
    QAction* autoReloadAction_ = nullptr;
    ColorMapBar* colorMapBar_ = nullptr;
    QTimer* autoReloadTimer_ = nullptr;
    std::pair<qint64, qint64> autoReloadStamp_{0, 0}; // (newest mtime ms, total size) at last poll
    QAction* balloonAction_ = nullptr;
    QAction* forceOpaqueAction_ = nullptr;
    QAction* pixelGridAction_ = nullptr;
    QAction* autoFitAction_ = nullptr;
    QAction* rememberViewAction_ = nullptr;
    QAction* nextImageAction_ = nullptr;
    QAction* previousImageAction_ = nullptr;
    QAction* nextFileImageAction_ = nullptr;
    QAction* previousFileImageAction_ = nullptr;
    QAction* nextSliceAction_ = nullptr;
    QAction* previousSliceAction_ = nullptr;
    QAction* showMarksAction_ = nullptr;
    QAction* measureDistanceAction_ = nullptr;
    QAction* contrastAction_ = nullptr;
    QAction* colorTableAction_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;
    QList<QAction*> bindableActions_; // every menu action whose shortcuts the user may rebind

    // Mark Browser (View > Mark Browser, "o"): a dock the user can float.
    MarkTreeView* markTreeView_ = nullptr;
    QDockWidget* markBrowserDock_ = nullptr;

    // Tools > Adjust Contrast / Color Table: docks the user can float,
    // refreshed whenever they appear or the displayed image changes.
    ContrastPanel* contrastPanel_ = nullptr;
    ColorTablePanel* colorTablePanel_ = nullptr;
    QDockWidget* contrastDock_ = nullptr;
    QDockWidget* colorTableDock_ = nullptr;

    // View > Image Info ("i"): giv's info panel, a dock the user can float.
    // While visible it is rebuilt on image/slice/scene changes and by
    // infoRefreshTimer_ (for state with no change signal: zoom, contrast).
    InfoPanel* infoPanel_ = nullptr;
    QDockWidget* infoDock_ = nullptr;
    QTimer* infoRefreshTimer_ = nullptr;
    // EXIF of the displayed file, re-read only when the path or mtime changes
    // (the info panel rebuilds twice a second).
    mutable QString exifPath_;
    mutable QDateTime exifMtime_;
    mutable std::vector<giv::ExifEntry> exifEntries_;

    std::unique_ptr<RpcServer> rpcServer_;

    void buildMenus();
    void buildStatusBar();
    void registerBindableActions();
    void buildInfoDock();

    // Common setup for a dock whose View-menu toggle is its toggleViewAction():
    // names it (saveState() needs that), adds it hidden, and gives it its own
    // toggle shortcut that is live only while it floats. A floating dock is a
    // separate window, so the main window's action shortcut doesn't reach it;
    // and while docked, a second shortcut would be ambiguous with the action's.
    void registerDock(QDockWidget* dock, const QString& objectName, Qt::DockWidgetArea area);
    // Copies each registered dock's toggle-action shortcuts to its floating shortcut.
    void applyDockShortcuts();
    std::vector<std::pair<QDockWidget*, QShortcut*>> dockShortcuts_;
    void showAboutDialog();

    std::vector<std::string> lastPaths_;

    // Sibling-file browsing (Next/Previous Image) for a single plain file
    // loaded via File > Open: the sorted list of giv-compatible files in
    // that file's directory, and the loaded file's index within it. Only
    // populated for a single-file load (see updateDirectoryFileList()) -
    // cleared for multi-file opens and for scenes that provide their own
    // $image list (VulkanViewport::imageCount() takes priority - see
    // goNextImage()/goPreviousImage()).
    QStringList directoryFiles_;
    // Explicit browse list from a multi-file/directory loadFiles() call; takes
    // precedence over the sibling-file scan while the loaded file is in it.
    QStringList browseList_;
    int directoryFileIndex_ = -1;

    // showEvent()'s guard for the deferred initial-load auto-fit - see its
    // doc comment.
    bool firstShow_ = true;

    // Combines lastCursorText_/lastMeasureText_/lastSliceText_ into the
    // status-bar label's text - see the cursorWorldPosition/
    // measurementChanged/sliceChanged connections built in the constructor.
    QString statusLineText() const;

    void openFiles();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);
    void loadFilesInternal(const std::vector<std::string>& paths);
    void reloadFiles();
    void setAutoReload(bool enable);

    // Rescans the directory of `loadedPath` for giv-compatible files (see
    // isGivCompatibleFile() in MainWindow.cpp) and updates directoryFiles_/
    // directoryFileIndex_ to match. No-op (clears both) unless exactly one
    // file was just loaded.
    void updateDirectoryFileList(const std::vector<std::string>& paths);
    void toggleFullScreen();

    // Next/Previous Image handlers wired to the QActions instead of directly
    // to VulkanViewport::nextImage()/previousImage(): page through
    // directoryFiles_; with no sibling files, fall back to the current
    // scene's own $image references (PgUp/PgDown always page those).
    void goNextImage();
    void goPreviousImage();
    void updateNavigationActionsEnabled();

    // Enables/disables nextSliceAction_/previousSliceAction_ for the
    // currently-displayed image's slice count (viewport_->sliceCount()) -
    // called whenever the displayed image changes.
    void updateSliceActionsEnabled();

    void buildMarkBrowserDock();

    void buildContrastDock();
    void buildColorTableDock();
    void updateContrastToolsEnabled();

    void refreshInfoPanel();
    QString buildInfoText() const;
};

} // namespace givqt
