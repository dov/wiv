#pragma once
//
// MainWindow.h - the wiv application shell: menu bar, toolbar, dataset dock
// panel, status bar, wrapping a central VulkanViewport.
//
#include <QMainWindow>

#include <string>
#include <vector>

class QLabel;
class QAction;
class QMenu;

namespace givqt
{

class VulkanViewport;
class DatasetPanel;

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
    DatasetPanel* datasetPanel_ = nullptr;

    QLabel* fpsLabel_ = nullptr;
    QLabel* cursorLabel_ = nullptr;
    QLabel* countsLabel_ = nullptr;

    QAction* balloonAction_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;

    void buildMenusAndToolbar();
    void buildDockPanel();
    void buildStatusBar();

    void openFiles();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);
    void loadFilesInternal(const std::vector<std::string>& paths);
};

} // namespace givqt
