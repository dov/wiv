#include "MainWindow.h"

#include "PreferencesDialog.h"
#include "VulkanViewport.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>

namespace givqt
{

namespace
{
constexpr int kMaxRecentFiles = 8;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("wiv");
    resize(1024, 768);

    viewport_ = new VulkanViewport(this);
    setCentralWidget(viewport_);

    viewport_->setBackgroundColor(PreferencesDialog::loadBackgroundColor());
    viewport_->setAutoFitMarginPx(PreferencesDialog::loadAutoFitMarginPx());

    buildMenus();
    buildStatusBar();

    connect(viewport_, &VulkanViewport::sceneLoaded, this, [this]() {
        const auto& scene = viewport_->sceneData();
        size_t points = 0;
        for (const auto& ds : scene.datasets) points += ds.pointCount();
        countsLabel_->setText(QString("%1 dataset(s), %2 points").arg(scene.datasets.size()).arg(points));
    });
    connect(viewport_, &VulkanViewport::cursorWorldPosition, this, [this](double x, double y) {
        cursorLabel_->setText(QString("(%1, %2)").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2));
    });
    connect(viewport_, &VulkanViewport::frameStats, this, [this](double fps) {
        fpsLabel_->setText(QString("%1 fps").arg(fps, 0, 'f', 1));
    });
}

void MainWindow::buildMenus()
{
    // Force an in-window menu bar rather than letting Qt route it through a
    // platform "global menu" integration (e.g. appmenu-qt6/dbusmenu on some
    // Linux desktops) - with no consumer for that global menu present, the
    // menu bar would otherwise silently vanish instead of falling back to
    // being drawn locally.
    menuBar()->setNativeMenuBar(false);

    auto fileMenu = menuBar()->addMenu("&File");
    auto openAction = fileMenu->addAction("&Open...", this, &MainWindow::openFiles);
    openAction->setShortcut(QKeySequence::Open);

    recentFilesMenu_ = fileMenu->addMenu("Recent Files");
    updateRecentFilesMenu();

    fileMenu->addSeparator();
    auto quitAction = fileMenu->addAction("&Quit", qApp, &QApplication::quit);
    quitAction->setShortcut(QKeySequence::Quit);

    auto viewMenu = menuBar()->addMenu("&View");
    auto zoomInAction = viewMenu->addAction("Zoom In", viewport_, &VulkanViewport::zoomIn);
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    auto zoomOutAction = viewMenu->addAction("Zoom Out", viewport_, &VulkanViewport::zoomOut);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    auto fitAction = viewMenu->addAction("Fit to Window", viewport_, &VulkanViewport::fitToWindow);
    fitAction->setShortcut(QKeySequence("Ctrl+0"));

    balloonAction_ = viewMenu->addAction("Balloon Tooltips", viewport_, &VulkanViewport::toggleBalloon);
    balloonAction_->setCheckable(true);
    balloonAction_->setShortcut(QKeySequence("B"));

    auto editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction("Preferences...", this, [this]() {
        PreferencesDialog dlg(this);
        connect(&dlg, &PreferencesDialog::backgroundColorChanged, viewport_, &VulkanViewport::setBackgroundColor);
        connect(&dlg, &PreferencesDialog::autoFitMarginChanged, viewport_, &VulkanViewport::setAutoFitMarginPx);
        dlg.exec();
    });

    auto helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("About wiv", this, [this]() {
        QMessageBox::about(this, "About wiv", "wiv - Vulkan-based, giv-format-compatible 2D vector viewer.");
    });
}

void MainWindow::buildStatusBar()
{
    fpsLabel_ = new QLabel(this);
    cursorLabel_ = new QLabel(this);
    countsLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(countsLabel_);
    statusBar()->addPermanentWidget(cursorLabel_);
    statusBar()->addPermanentWidget(fpsLabel_);
}

void MainWindow::openFiles()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
    QString lastDir = settings.value("lastOpenDir").toString();

    QStringList files = QFileDialog::getOpenFileNames(this, "Open giv file(s)", lastDir, "giv files (*.giv);;All files (*)");
    if (files.isEmpty()) return;

    settings.setValue("lastOpenDir", QFileInfo(files.first()).absolutePath());

    std::vector<std::string> paths;
    for (const auto& f : files) paths.push_back(f.toStdString());
    loadFilesInternal(paths);

    for (const auto& f : files) addRecentFile(f);
}

void MainWindow::loadFiles(const std::vector<std::string>& paths)
{
    loadFilesInternal(paths);
    for (const auto& p : paths) addRecentFile(QString::fromStdString(p));
}

void MainWindow::loadFilesInternal(const std::vector<std::string>& paths)
{
    QString error;
    if (!viewport_->loadFiles(paths, &error))
    {
        QMessageBox::critical(this, "wiv", QString("Failed to load file(s):\n%1").arg(error));
        return;
    }
    balloonAction_->setChecked(false);
}

void MainWindow::updateRecentFilesMenu()
{
    recentFilesMenu_->clear();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
    QStringList recent = settings.value("recentFiles").toStringList();
    for (const auto& f : recent)
    {
        recentFilesMenu_->addAction(f, this, [this, f]() { loadFilesInternal({f.toStdString()}); });
    }
    recentFilesMenu_->setEnabled(!recent.isEmpty());
}

void MainWindow::addRecentFile(const QString& path)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
    QStringList recent = settings.value("recentFiles").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentFiles) recent.removeLast();
    settings.setValue("recentFiles", recent);
    updateRecentFilesMenu();
}

} // namespace givqt
