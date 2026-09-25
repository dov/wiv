#include "MainWindow.h"

#include "MarkTreeView.h"
#include "PreferencesDialog.h"
#include "VulkanViewport.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QVBoxLayout>

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
        if (markTreeView_) markTreeView_->rebuildFromScene();
    });
    connect(viewport_, &VulkanViewport::cursorWorldPosition, this, [this](double x, double y) {
        cursorLabel_->setText(QString("(%1, %2)").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2));
    });
    connect(viewport_, &VulkanViewport::frameStats, this, [this](double fps) {
        fpsLabel_->setText(QString("%1 fps").arg(fps, 0, 'f', 1));
    });
    connect(viewport_, &VulkanViewport::imageChanged, this, [this](int index, int count, QString filename) {
        nextImageAction_->setEnabled(count > 1);
        previousImageAction_->setEnabled(count > 1);
        if (count == 0)
        {
            imageLabel_->clear();
            return;
        }
        imageLabel_->setText(QString("image %1/%2: %3").arg(index + 1).arg(count).arg(QFileInfo(filename).fileName()));
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

    viewMenu->addSeparator();
    showMarksAction_ = viewMenu->addAction("Show Marks", viewport_, &VulkanViewport::toggleShowMarks);
    showMarksAction_->setCheckable(true);
    showMarksAction_->setChecked(true);
    showMarksAction_->setShortcut(QKeySequence("M"));

    viewMenu->addAction("Mark Browser...", this, &MainWindow::showMarkBrowser);

    markBrowserPanelAction_ = new QAction("Mark Browser as Panel", this);
    markBrowserPanelAction_->setCheckable(true);
    connect(markBrowserPanelAction_, &QAction::toggled, this, &MainWindow::setMarkBrowserPlacement);
    viewMenu->addAction(markBrowserPanelAction_);

    viewMenu->addSeparator();
    nextImageAction_ = viewMenu->addAction("Next Image", viewport_, &VulkanViewport::nextImage);
    nextImageAction_->setShortcut(QKeySequence("Shift+Up"));
    nextImageAction_->setEnabled(false);
    previousImageAction_ = viewMenu->addAction("Previous Image", viewport_, &VulkanViewport::previousImage);
    previousImageAction_->setShortcut(QKeySequence("Shift+Down"));
    previousImageAction_->setEnabled(false);

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
    imageLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(countsLabel_);
    statusBar()->addPermanentWidget(imageLabel_);
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

void MainWindow::showMarkBrowser()
{
    if (!markTreeView_) markTreeView_ = new MarkTreeView(viewport_);
    markTreeView_->rebuildFromScene();

    if (markBrowserAsPanel_)
    {
        if (!markBrowserDock_)
        {
            markBrowserDock_ = new QDockWidget("Mark Browser", this);
            markBrowserDock_->setWidget(markTreeView_);
            addDockWidget(Qt::LeftDockWidgetArea, markBrowserDock_);
        }
        markBrowserDock_->show();
        markBrowserDock_->raise();
    }
    else
    {
        if (!markBrowserDialog_)
        {
            // Non-modal, resizable, and left open across further interaction
            // with the main window - matching giv's own Mark Browser dialog
            // (giv-mark-tree-dialog.gob: transient-for + destroy-with-parent,
            // but not a blocking gtk_dialog_run).
            markBrowserDialog_ = new QDialog(this);
            markBrowserDialog_->setWindowTitle("Mark Browser");
            markBrowserDialog_->resize(400, 350);
            auto layout = new QVBoxLayout(markBrowserDialog_);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addWidget(markTreeView_);
        }
        markBrowserDialog_->show();
        markBrowserDialog_->raise();
        markBrowserDialog_->activateWindow();
    }
}

void MainWindow::setMarkBrowserPlacement(bool asPanel)
{
    if (markBrowserAsPanel_ == asPanel) return;

    bool wasVisible = (markBrowserDialog_ && markBrowserDialog_->isVisible()) || (markBrowserDock_ && markBrowserDock_->isVisible());

    // Detach the shared tree view before tearing down whichever container
    // currently owns it, then destroy that (now-empty) container.
    if (markTreeView_) markTreeView_->setParent(nullptr);
    if (markBrowserDialog_)
    {
        markBrowserDialog_->deleteLater();
        markBrowserDialog_ = nullptr;
    }
    if (markBrowserDock_)
    {
        removeDockWidget(markBrowserDock_);
        markBrowserDock_->deleteLater();
        markBrowserDock_ = nullptr;
    }

    markBrowserAsPanel_ = asPanel;
    if (wasVisible) showMarkBrowser();
}

} // namespace givqt
