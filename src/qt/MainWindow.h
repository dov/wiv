#pragma once
//
// MainWindow.h - the wiv application shell: menu bar, status bar, wrapping
// a central VulkanViewport. Deliberately no toolbar or side panel - wiv
// aims to match giv's own look/behavior, which has neither.
//
#include <QMainWindow>

#include <string>
#include <vector>

class QLabel;
class QAction;
class QMenu;
class QDialog;
class QDockWidget;

namespace givqt
{

class VulkanViewport;
class MarkTreeView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Loads `paths` into the viewport (used for files given on the command
    // line at startup).
    void loadFiles(const std::vector<std::string>& paths);

private:
    VulkanViewport* viewport_ = nullptr;

    // Single status-bar label, mirroring giv's own w_info_label: shows
    // "Loaded: <file>" right after a load, then gets overwritten by the
    // cursor position (and, mid-measurement, the caliper distance) on every
    // mouse move - see the connections built in the constructor.
    QLabel* infoLabel_ = nullptr;
    QString lastCursorText_;
    QString lastMeasureText_;
    QString loadedBaseName_;

    QAction* balloonAction_ = nullptr;
    QAction* forceOpaqueAction_ = nullptr;
    QAction* autoFitAction_ = nullptr;
    QAction* nextImageAction_ = nullptr;
    QAction* previousImageAction_ = nullptr;
    QAction* showMarksAction_ = nullptr;
    QAction* markBrowserPanelAction_ = nullptr;
    QAction* measureDistanceAction_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;

    // Mark Browser: one shared MarkTreeView content widget, hosted in either
    // a standalone (non-modal) window or a docked side panel - see
    // showMarkBrowser()/setMarkBrowserPlacement(). Only one of the two
    // containers exists at a time.
    MarkTreeView* markTreeView_ = nullptr;
    QDialog* markBrowserDialog_ = nullptr;
    QDockWidget* markBrowserDock_ = nullptr;
    bool markBrowserAsPanel_ = false;

    void buildMenus();
    void buildStatusBar();

    std::vector<std::string> lastPaths_;

    void openFiles();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);
    void loadFilesInternal(const std::vector<std::string>& paths);
    void reloadFiles();

    void showMarkBrowser();
    void setMarkBrowserPlacement(bool asPanel);
};

} // namespace givqt
