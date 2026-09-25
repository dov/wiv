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

namespace givqt
{

class VulkanViewport;

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

    QLabel* fpsLabel_ = nullptr;
    QLabel* cursorLabel_ = nullptr;
    QLabel* countsLabel_ = nullptr;
    QLabel* imageLabel_ = nullptr;

    QAction* balloonAction_ = nullptr;
    QAction* nextImageAction_ = nullptr;
    QAction* previousImageAction_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;

    void buildMenus();
    void buildStatusBar();

    void openFiles();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);
    void loadFilesInternal(const std::vector<std::string>& paths);
};

} // namespace givqt
