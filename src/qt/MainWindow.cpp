#include "MainWindow.h"

#include "CalibrateDialog.h"
#include "ImagePluginHost.h"
#include "MarkTreeView.h"
#include "OpenFileDialog.h"
#include "PreferencesDialog.h"
#include "VulkanViewport.h"

#include <spdlog/spdlog.h>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QShowEvent>
#include <QStatusBar>
#include <QVBoxLayout>

namespace givqt
{

namespace
{
constexpr int kMaxRecentFiles = 8;

// True if `path` is a wiv-openable file - mirrors
// OpenFileDialog.cpp's isSupportedWivFile(): a .giv scene, an .svg, or
// anything giv::ImagePluginHost::isSupported() claims (suffix-only, never
// decodes).
bool isGivCompatibleFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare("giv", Qt::CaseInsensitive) == 0) return true;
    if (suffix.compare("svg", Qt::CaseInsensitive) == 0) return true;
    return giv::ImagePluginHost::isSupported(path.toStdString());
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("wiv");
    resize(1024, 768);

    viewport_ = new VulkanViewport(this);
    setCentralWidget(viewport_);

    viewport_->setBackgroundColor(PreferencesDialog::loadBackgroundColor());

    buildMenus();
    buildStatusBar();

    connect(viewport_, &VulkanViewport::sceneLoaded, this, [this]() {
        if (markTreeView_) markTreeView_->rebuildFromScene();
    });
    connect(viewport_, &VulkanViewport::cursorWorldPosition, this, [this](double x, double y) {
        lastCursorText_ = QString("(%1, %2)").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2);
        infoLabel_->setText(lastMeasureText_.isEmpty() ? lastCursorText_ : QString("%1 %2").arg(lastCursorText_, lastMeasureText_));
    });
    connect(viewport_, &VulkanViewport::measurementChanged, this, [this](QString text) {
        lastMeasureText_ = text;
        infoLabel_->setText(lastMeasureText_.isEmpty() ? lastCursorText_ : QString("%1 %2").arg(lastCursorText_, lastMeasureText_));
    });
    connect(viewport_, &VulkanViewport::imageChanged, this, [this](int index, int count, QString filename) {
        updateNavigationActionsEnabled();
        // Sole owner of the title/status-label text for both the initial
        // load and every next/previousImage() step (loadFilesInternal only
        // updates loadedBaseName_, before calling viewport_->loadFiles() -
        // see its comment - so this always sees the current file list).
        if (count == 0)
        {
            setWindowTitle(QString("wiv: %1").arg(loadedBaseName_));
            infoLabel_->setText(QString("Loaded: %1").arg(loadedBaseName_));
            return;
        }
        QString baseName = QFileInfo(filename).fileName();
        setWindowTitle(QString("wiv: %1").arg(baseName));
        lastCursorText_.clear();
        lastMeasureText_.clear();
        infoLabel_->setText(QString("Loading %1").arg(baseName));
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
    auto reloadAction = fileMenu->addAction("&Reload", this, &MainWindow::reloadFiles);
    reloadAction->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});

    fileMenu->addSeparator();
    auto quitAction = fileMenu->addAction("&Quit", qApp, &QApplication::quit);
    quitAction->setShortcuts({QKeySequence::Quit, QKeySequence("Q")});

    auto viewMenu = menuBar()->addMenu("&View");
    auto zoomInAction = viewMenu->addAction("Zoom In", viewport_, &VulkanViewport::zoomIn);
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    auto zoomOutAction = viewMenu->addAction("Zoom Out", viewport_, &VulkanViewport::zoomOut);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    auto fitAction = viewMenu->addAction("Fit to Window", viewport_, &VulkanViewport::fitToWindow);
    fitAction->setShortcuts({QKeySequence("Ctrl+0"), QKeySequence("F")});

    balloonAction_ = viewMenu->addAction("Balloon Tooltips", viewport_, &VulkanViewport::toggleBalloon);
    balloonAction_->setCheckable(true);
    balloonAction_->setShortcut(QKeySequence("B"));

    forceOpaqueAction_ = viewMenu->addAction("Force Opaque", viewport_, &VulkanViewport::toggleForceOpaque);
    forceOpaqueAction_->setCheckable(true);
    forceOpaqueAction_->setShortcut(QKeySequence("A"));

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
    // giv's do_auto_fit_marks: on (default), next/previousImage() re-fits
    // the view to each new image (see VulkanViewport::switchToImage());
    // off, it preserves whatever zoom/pan the user was at across the
    // switch instead.
    autoFitAction_ = viewMenu->addAction("Auto Fit", viewport_, &VulkanViewport::toggleAutoFit);
    autoFitAction_->setCheckable(true);
    autoFitAction_->setChecked(true);

    nextImageAction_ = viewMenu->addAction("Next Image", this, &MainWindow::goNextImage);
    nextImageAction_->setShortcuts({QKeySequence("Shift+Up"), QKeySequence("Space"), QKeySequence("Right")});
    nextImageAction_->setEnabled(false);
    previousImageAction_ = viewMenu->addAction("Previous Image", this, &MainWindow::goPreviousImage);
    previousImageAction_->setShortcuts({QKeySequence("Shift+Down"), QKeySequence("Backspace"), QKeySequence("Left")});
    previousImageAction_->setEnabled(false);

    auto toolsMenu = menuBar()->addMenu("&Tools");
    measureDistanceAction_ = toolsMenu->addAction("Measure Distance Diagonal", viewport_, &VulkanViewport::toggleMeasureDistance);
    measureDistanceAction_->setCheckable(true);
    measureDistanceAction_->setShortcut(QKeySequence("Z"));

    toolsMenu->addAction("Calibrate Pixel Size...", this, [this]() {
        spdlog::info("Calibrate Pixel Size dialog opened");
        CalibrateDialog dlg(this, viewport_->pixelSize(), QString::fromStdString(viewport_->pixelSizeUnit()),
                            viewport_->lastMeasureDistancePixels(), viewport_->currentImageSize());
        connect(&dlg, &CalibrateDialog::calibrationChanged, this, [this](double pixelSize, QString unit) {
            viewport_->setPixelCalibration(pixelSize, unit.toStdString());
        });
        dlg.exec();
        spdlog::info("Calibrate Pixel Size dialog closed");
    });

    auto editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction("Preferences...", this, [this]() {
        spdlog::info("Preferences dialog opened");
        PreferencesDialog dlg(this);
        connect(&dlg, &PreferencesDialog::backgroundColorChanged, viewport_, &VulkanViewport::setBackgroundColor);
        dlg.exec();
        spdlog::info("Preferences dialog closed");
    });

    auto helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("About wiv", this, [this]() {
        QMessageBox::about(this, "About wiv", "wiv - Vulkan-based, giv-format-compatible 2D vector viewer.");
    });
}

void MainWindow::buildStatusBar()
{
    infoLabel_ = new QLabel(this);
    infoLabel_->setAlignment(Qt::AlignCenter);
    // giv packs this label full-width below the canvas (gtk_box_pack_start
    // into a GtkVBox), so its text sits centered under the window rather
    // than tucked in a corner - addWidget() with a stretch factor mirrors
    // that, unlike addPermanentWidget() which hugs the right edge.
    statusBar()->addWidget(infoLabel_, 1);
}

void MainWindow::openFiles()
{
    spdlog::info("User invoked File > Open...");
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
    // Default to the process's current directory the first time this dialog
    // ever opens (no "lastOpenDir" recorded yet), rather than whatever
    // QFileDialog itself would otherwise default to.
    QString lastDir = settings.value("lastOpenDir", QDir::currentPath()).toString();

    OpenFileDialog dlg(this, &settings);
    dlg.setDirectory(lastDir);
    if (dlg.exec() != QDialog::Accepted)
    {
        spdlog::info("Open dialog cancelled");
        return;
    }

    QStringList files = dlg.selectedFiles();
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

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!firstShow_) return;
    firstShow_ = false;

    // The very first fit computed at load time (VulkanViewport::
    // rebuildSceneGraph()'s isInitialLoad path, since loadFiles() is called
    // before show() - see main.cpp) is only ever a rough placeholder: the
    // embedded Vulkan window's true on-screen size isn't reliably known until
    // the window has actually been shown/laid out. Queued rather than called
    // directly - showEvent() itself still fires before that layout has fully
    // settled - so this runs once the event loop regains control; then
    // ensureExtentSettled() (see its doc comment) waits for the window's real
    // size to actually be reflected before fitToWindow() runs - the exact
    // same fitToWindow() the "Fit to Window" menu action calls, so a
    // subsequent manual Fit to Window is then a no-op.
    QMetaObject::invokeMethod(
        viewport_,
        [this]() {
            viewport_->ensureExtentSettled();
            viewport_->fitToWindow();
        },
        Qt::QueuedConnection);
}

void MainWindow::loadFilesInternal(const std::vector<std::string>& paths)
{
    {
        QStringList list;
        for (const auto& p : paths) list << QString::fromStdString(p);
        spdlog::info("Loading {} file(s): {}", paths.size(), list.join(", ").toStdString());
    }
    // Set before calling viewport_->loadFiles() below, not after: that call
    // emits imageChanged synchronously (see VulkanViewport::loadFiles()),
    // and its handler (in the constructor) reads loadedBaseName_ for the
    // no-$image-references title/status text - it needs to already be
    // current by then, not the previous load's value.
    QStringList baseNames;
    for (const auto& p : paths) baseNames << QFileInfo(QString::fromStdString(p)).fileName();
    loadedBaseName_ = baseNames.join(", ");
    updateDirectoryFileList(paths);

    QString error;
    if (!viewport_->loadFiles(paths, &error))
    {
        spdlog::error("Failed to load file(s): {}", error.toStdString());
        QMessageBox::critical(this, "wiv", QString("Failed to load file(s):\n%1").arg(error));
        return;
    }
    lastPaths_ = paths;
    balloonAction_->setChecked(false);
    measureDistanceAction_->setChecked(false);
    spdlog::info("Loaded {} file(s) successfully", paths.size());
}

void MainWindow::reloadFiles()
{
    if (lastPaths_.empty()) return;
    spdlog::info("User invoked File > Reload");
    loadFilesInternal(lastPaths_);
}

void MainWindow::updateDirectoryFileList(const std::vector<std::string>& paths)
{
    if (paths.size() != 1)
    {
        directoryFiles_.clear();
        directoryFileIndex_ = -1;
        return;
    }

    QFileInfo loadedInfo(QString::fromStdString(paths.front()));
    QDir dir = loadedInfo.absoluteDir();

    QStringList files;
    for (const auto& entry : dir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase))
    {
        if (isGivCompatibleFile(entry.absoluteFilePath())) files << entry.absoluteFilePath();
    }

    directoryFiles_ = files;
    directoryFileIndex_ = directoryFiles_.indexOf(loadedInfo.absoluteFilePath());
}

void MainWindow::updateNavigationActionsEnabled()
{
    bool canNavigate = viewport_->imageCount() > 1 || directoryFiles_.size() > 1;
    nextImageAction_->setEnabled(canNavigate);
    previousImageAction_->setEnabled(canNavigate);
}

void MainWindow::goNextImage()
{
    spdlog::info("User invoked Next Image");
    if (viewport_->imageCount() > 1)
    {
        viewport_->nextImage();
        return;
    }
    if (directoryFiles_.size() < 2) return;
    directoryFileIndex_ = (directoryFileIndex_ + 1) % directoryFiles_.size();
    spdlog::info("Directory navigation: switching to {}", directoryFiles_[directoryFileIndex_].toStdString());
    loadFilesInternal({directoryFiles_[directoryFileIndex_].toStdString()});
}

void MainWindow::goPreviousImage()
{
    spdlog::info("User invoked Previous Image");
    if (viewport_->imageCount() > 1)
    {
        viewport_->previousImage();
        return;
    }
    if (directoryFiles_.size() < 2) return;
    directoryFileIndex_ = (directoryFileIndex_ - 1 + directoryFiles_.size()) % directoryFiles_.size();
    spdlog::info("Directory navigation: switching to {}", directoryFiles_[directoryFileIndex_].toStdString());
    loadFilesInternal({directoryFiles_[directoryFileIndex_].toStdString()});
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
    spdlog::info("User opened Mark Browser");
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
    spdlog::info("Mark Browser placement changed to {}", asPanel ? "panel" : "dialog");

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
