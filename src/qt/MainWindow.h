#pragma once
//
// MainWindow.h - the wiv application shell: menu bar, status bar, wrapping
// a central VulkanViewport. Deliberately no toolbar or side panel - wiv
// aims to match giv's own look/behavior, which has neither.
//
#include <QMainWindow>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

class QLabel;
class QAction;
class QMenu;
class QDialog;
class QDockWidget;
class QShowEvent;
class QDragEnterEvent;
class QDropEvent;

namespace givqt
{

class VulkanViewport;
class MarkTreeView;
class ContrastDialog;
class ColorTableDialog;
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
    QString lastLoadSummary_;

    QAction* balloonAction_ = nullptr;
    QAction* forceOpaqueAction_ = nullptr;
    QAction* autoFitAction_ = nullptr;
    QAction* nextImageAction_ = nullptr;
    QAction* previousImageAction_ = nullptr;
    QAction* nextSliceAction_ = nullptr;
    QAction* previousSliceAction_ = nullptr;
    QAction* showMarksAction_ = nullptr;
    QAction* markBrowserPanelAction_ = nullptr;
    QAction* measureDistanceAction_ = nullptr;
    QAction* contrastAction_ = nullptr;
    QAction* colorTableAction_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;

    // Mark Browser: one shared MarkTreeView content widget, hosted in either
    // a standalone (non-modal) window or a docked side panel - see
    // showMarkBrowser()/setMarkBrowserPlacement(). Only one of the two
    // containers exists at a time.
    MarkTreeView* markTreeView_ = nullptr;
    QDialog* markBrowserDialog_ = nullptr;
    QDockWidget* markBrowserDock_ = nullptr;
    bool markBrowserAsPanel_ = false;

    // Tools > Adjust Contrast / Color Table: non-modal, lazily created (see
    // showMarkBrowser()'s pattern), kept alive across show()/hide() so their
    // state survives, and refreshed whenever the displayed image changes.
    ContrastDialog* contrastDialog_ = nullptr;
    ColorTableDialog* colorTableDialog_ = nullptr;

    std::unique_ptr<RpcServer> rpcServer_;

    void buildMenus();
    void buildStatusBar();

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

    // Rescans the directory of `loadedPath` for giv-compatible files (see
    // isGivCompatibleFile() in MainWindow.cpp) and updates directoryFiles_/
    // directoryFileIndex_ to match. No-op (clears both) unless exactly one
    // file was just loaded.
    void updateDirectoryFileList(const std::vector<std::string>& paths);

    // Next/Previous Image handlers wired to the QActions instead of directly
    // to VulkanViewport::nextImage()/previousImage(): prefer paging through
    // the current scene's own $image references when there's more than one
    // (VulkanViewport::imageCount() > 1), otherwise fall back to paging
    // through directoryFiles_.
    void goNextImage();
    void goPreviousImage();
    void updateNavigationActionsEnabled();

    // Enables/disables nextSliceAction_/previousSliceAction_ for the
    // currently-displayed image's slice count (viewport_->sliceCount()) -
    // called whenever the displayed image changes.
    void updateSliceActionsEnabled();

    void showMarkBrowser();
    void setMarkBrowserPlacement(bool asPanel);

    void showContrastDialog();
    void showColorTableDialog();
    void updateContrastToolsEnabled();
};

} // namespace givqt
