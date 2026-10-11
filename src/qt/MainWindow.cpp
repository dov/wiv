#include "MainWindow.h"

#include "CalibrateDialog.h"
#include "ColorTableDialog.h"
#include "ContrastDialog.h"
#include "ImagePluginHost.h"
#include "InfoDialog.h"
#include "MarkTreeView.h"
#include "OpenFileDialog.h"
#include "PreferencesDialog.h"
#include "RpcServer.h"
#include "VulkanViewport.h"
#ifdef _WIN32
#include "WinDropTarget.h"
#endif

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
#include <QTimer>
#include <QThread>
#include <QDateTime>
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
    connect(viewport_, &VulkanViewport::filesDropped, this, [this](QStringList files) {
        std::vector<std::string> paths;
        for (const QString& f : files) paths.push_back(f.toStdString());
        loadFiles(paths);
    });

    viewport_->setBackgroundColor(PreferencesDialog::loadBackgroundColor());

    buildMenus();
    buildStatusBar();

    connect(viewport_, &VulkanViewport::sceneLoaded, this, [this]() {
        if (markTreeView_) markTreeView_->rebuildFromScene();
        refreshInfoDialog();
        const auto& scene = viewport_->sceneData();
        if (!scene.datasets.empty())
        {
            size_t totalPoints = 0;
            for (const auto& ds : scene.datasets) totalPoints += ds.pointCount();
            lastLoadSummary_ = QString("Loaded %1: %2 dataset(s), %3 point(s)")
                                    .arg(loadedBaseName_)
                                    .arg(scene.datasets.size())
                                    .arg(totalPoints);
        }
        else
        {
            lastLoadSummary_.clear();
        }
    });
    connect(viewport_, &VulkanViewport::cursorWorldPosition, this, [this](double x, double y) {
        lastCursorText_ = QString("(%1, %2)").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2) + viewport_->pixelValueText(x, y);
        infoLabel_->setText(statusLineText());
    });
    connect(viewport_, &VulkanViewport::measurementChanged, this, [this](QString text) {
        lastMeasureText_ = text;
        infoLabel_->setText(statusLineText());
    });
    connect(viewport_, &VulkanViewport::sliceChanged, this, [this](int slice, int count) {
        lastSliceText_ = count > 1 ? QString("Slice %1/%2").arg(slice + 1).arg(count) : QString();
        infoLabel_->setText(statusLineText());
        refreshInfoDialog();
    });
    connect(viewport_, &VulkanViewport::imageChanged, this, [this](int index, int count, QString filename) {
        updateNavigationActionsEnabled();
        updateSliceActionsEnabled();
        updateContrastToolsEnabled();
        if (contrastDialog_ && contrastDialog_->isVisible()) contrastDialog_->refreshForImage();
        if (colorTableDialog_ && colorTableDialog_->isVisible()) colorTableDialog_->refreshForImage();
        refreshInfoDialog();
        // Sole owner of the title/status-label text for both the initial
        // load and every next/previousImage() step (loadFilesInternal only
        // updates loadedBaseName_, before calling viewport_->loadFiles() -
        // see its comment - so this always sees the current file list).
        if (count == 0)
        {
            setWindowTitle(QString("wiv: %1").arg(loadedBaseName_));
            infoLabel_->setText(lastLoadSummary_.isEmpty() ? QString("Loaded: %1").arg(loadedBaseName_)
                                                            : lastLoadSummary_);
            return;
        }
        QString baseName = QFileInfo(filename).fileName();
        // Name the loaded file first so a multi-image .giv is identifiable;
        // the current $image is appended as "[n/count: image]".
        QString label = loadedBaseName_;
        if (count > 1) label += QString(" [%1/%2: %3]").arg(index + 1).arg(count).arg(baseName);
        else if (baseName != loadedBaseName_) label += QString(" [%1]").arg(baseName);
        setWindowTitle(QString("wiv: %1").arg(label));
        lastCursorText_.clear();
        lastMeasureText_.clear();
        infoLabel_->setText(QString("Loading %1").arg(label));
    });
}

MainWindow::~MainWindow() = default;

bool MainWindow::startRpcServer(int port)
{
    rpcServer_ = std::make_unique<RpcServer>(this, viewport_, this);
    if (!rpcServer_->start(static_cast<quint16>(port)))
    {
        rpcServer_.reset();
        return false;
    }
    return true;
}

QString MainWindow::statusLineText() const
{
    QStringList parts;
    if (!lastCursorText_.isEmpty()) parts << lastCursorText_;
    if (!lastMeasureText_.isEmpty()) parts << lastMeasureText_;
    if (!lastSliceText_.isEmpty()) parts << lastSliceText_;
    return parts.join(" ");
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
    auto actualPixelsAction = viewMenu->addAction("Actual Pixels (1:1)", viewport_, &VulkanViewport::zoomActualPixels);
    actualPixelsAction->setShortcut(QKeySequence("1"));

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

    auto markBrowserAction = viewMenu->addAction("Mark Browser...", this, &MainWindow::showMarkBrowser);
    markBrowserAction->setShortcut(QKeySequence("O"));

    markBrowserPanelAction_ = new QAction("Mark Browser as Panel", this);
    markBrowserPanelAction_->setCheckable(true);
    connect(markBrowserPanelAction_, &QAction::toggled, this, &MainWindow::setMarkBrowserPlacement);
    viewMenu->addAction(markBrowserPanelAction_);

    viewMenu->addSeparator();
    auto infoAction = viewMenu->addAction("Image Info...", this, &MainWindow::toggleInfoDialog);
    infoAction->setShortcut(QKeySequence("I"));

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
    // Pages through the $image references of the current file (e.g. a
    // multi-image .giv); Next/Previous Image above move between files.
    nextFileImageAction_ = viewMenu->addAction("Next Image in File", viewport_, &VulkanViewport::nextImage);
    nextFileImageAction_->setShortcut(QKeySequence("PgDown"));
    nextFileImageAction_->setEnabled(false);
    previousFileImageAction_ = viewMenu->addAction("Previous Image in File", viewport_, &VulkanViewport::previousImage);
    previousFileImageAction_->setShortcut(QKeySequence("PgUp"));
    previousFileImageAction_->setEnabled(false);

    // Slice navigation (giv's plain Up/Down) for multi-slice images (npy 3D
    // arrays, FITS NAXIS=3, multi-frame DICOM) - distinct from Next/Previous
    // Image's shift-Up/Down $image cycling above.
    nextSliceAction_ = viewMenu->addAction("Next Slice", viewport_, &VulkanViewport::nextSlice);
    nextSliceAction_->setShortcut(QKeySequence("Up"));
    nextSliceAction_->setEnabled(false);
    previousSliceAction_ = viewMenu->addAction("Previous Slice", viewport_, &VulkanViewport::previousSlice);
    previousSliceAction_->setShortcut(QKeySequence("Down"));
    previousSliceAction_->setEnabled(false);

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

    toolsMenu->addSeparator();
    contrastAction_ = toolsMenu->addAction("Adjust Contrast...", this, &MainWindow::showContrastDialog);
    colorTableAction_ = toolsMenu->addAction("Color Table...", this, &MainWindow::showColorTableDialog);
    contrastAction_->setEnabled(false);
    colorTableAction_->setEnabled(false);

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
    browseList_.clear();
    loadFilesInternal(paths);

    for (const auto& f : files) addRecentFile(f);
}

void MainWindow::loadFiles(const std::vector<std::string>& paths)
{
    // Directories expand to their giv-compatible files (sorted). More than
    // one resulting file is browsed one at a time via Next/Previous Image
    // (like images) rather than merged into a single scene.
    QStringList expanded;
    for (const auto& p : paths)
    {
        QFileInfo info(QString::fromStdString(p));
        if (info.isDir())
        {
            for (const auto& entry : QDir(info.absoluteFilePath()).entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase))
                if (isGivCompatibleFile(entry.absoluteFilePath())) expanded << entry.absoluteFilePath();
        }
        else
        {
            expanded << QString::fromStdString(p);
        }
    }
    if (expanded.isEmpty())
    {
        spdlog::warn("No giv-compatible files found in the given path(s)");
        return;
    }

    {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
        OpenFileDialog::recordRecentDirectory(settings, QFileInfo(expanded.first()).absolutePath());
    }

    browseList_.clear();
    if (expanded.size() > 1)
    {
        for (auto& f : expanded) f = QFileInfo(f).absoluteFilePath();
        browseList_ = expanded;
        loadFilesInternal({expanded.first().toStdString()});
        addRecentFile(expanded.first());
        return;
    }
    loadFilesInternal({expanded.first().toStdString()});
    addRecentFile(expanded.first());
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!firstShow_) return;
    firstShow_ = false;

#ifdef _WIN32
    // Custom OLE drop target (no "Copy" label); see WinDropTarget.h.
    installFileDropTarget(this, [this](QStringList files) {
        std::vector<std::string> paths;
        for (const QString& f : files) paths.push_back(f.toStdString());
        loadFiles(paths);
    });
#endif

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
    loadedPaths_.clear();
    for (const auto& p : paths) loadedPaths_ << QFileInfo(QString::fromStdString(p)).absoluteFilePath();
    updateDirectoryFileList(paths);

    QString error;
    if (!viewport_->loadFiles(paths, &error))
    {
        spdlog::error("Failed to load file(s): {}", error.toStdString());
        // QMessageBox::critical(parent, ...)'s own centering-over-parent
        // logic isn't reliable under every window manager (observed: FVWM
        // placed it well off-center) - position it explicitly instead of
        // trusting that.
        //
        // This can run for a command-line file (main.cpp calls
        // window.loadFiles() right after window.show(), before app.exec()
        // has processed a single event), in which case frameGeometry() below
        // would still reflect this window's pre-placement default (window
        // manager placement is asynchronous - a ConfigureNotify that hasn't
        // arrived yet) rather than where it actually ends up on screen. Pump
        // events briefly first so that has a chance to arrive - the same
        // class of lag VulkanViewport::ensureExtentSettled() waits out, but
        // with no equivalent "is it done yet" signal to poll for here, so
        // this is just a fixed, short, best-effort wait.
        for (int i = 0; i < 20; ++i)
        {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QThread::msleep(5);
        }

        QMessageBox box(QMessageBox::Critical, "wiv", QString("Failed to load file(s):\n%1").arg(error), QMessageBox::Ok, this);
        box.adjustSize();
        // move() *after* show() rather than before: some window managers
        // apply their own initial-placement policy at map time regardless of
        // any position set beforehand, but still honor an explicit move
        // request against an already-mapped window.
        box.show();
        // frameGeometry() (not rect()/geometry()) on both sides: move()
        // positions a top-level widget's frame (including window-manager
        // decorations), but rect()'s size is the client area only, which
        // otherwise under-counts the dialog's own title bar and skews the
        // result off-center by about half of it.
        box.move(frameGeometry().center() - QPoint(box.frameGeometry().width() / 2, box.frameGeometry().height() / 2));
        box.exec();
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
    int listIdx = browseList_.indexOf(loadedInfo.absoluteFilePath());
    if (listIdx >= 0)
    {
        directoryFiles_ = browseList_;
        directoryFileIndex_ = listIdx;
        return;
    }
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
    bool multiImage = viewport_->imageCount() > 1;
    nextFileImageAction_->setEnabled(multiImage);
    previousFileImageAction_->setEnabled(multiImage);
}

void MainWindow::updateSliceActionsEnabled()
{
    bool canNavigate = viewport_->sliceCount() > 1;
    nextSliceAction_->setEnabled(canNavigate);
    previousSliceAction_->setEnabled(canNavigate);
}

void MainWindow::goNextImage()
{
    spdlog::info("User invoked Next Image");
    if (directoryFiles_.size() < 2)
    {
        // No sibling files: page through the file's own images instead.
        if (viewport_->imageCount() > 1) viewport_->nextImage();
        return;
    }
    directoryFileIndex_ = (directoryFileIndex_ + 1) % directoryFiles_.size();
    spdlog::info("Directory navigation: switching to {}", directoryFiles_[directoryFileIndex_].toStdString());
    loadFilesInternal({directoryFiles_[directoryFileIndex_].toStdString()});
}

void MainWindow::goPreviousImage()
{
    spdlog::info("User invoked Previous Image");
    if (directoryFiles_.size() < 2)
    {
        // No sibling files: page through the file's own images instead.
        if (viewport_->imageCount() > 1) viewport_->previousImage();
        return;
    }
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

void MainWindow::showContrastDialog()
{
    spdlog::info("User opened Adjust Contrast dialog");
    if (!contrastDialog_)
    {
        contrastDialog_ = new ContrastDialog(viewport_, this);
        connect(contrastDialog_, &ContrastDialog::colormapChanged, this, [this] {
            if (colorTableDialog_) colorTableDialog_->refreshForImage();
        });
    }
    contrastDialog_->refreshForImage();
    contrastDialog_->show();
    contrastDialog_->raise();
    contrastDialog_->activateWindow();
}

void MainWindow::toggleInfoDialog()
{
    if (infoDialog_ && infoDialog_->isVisible())
    {
        infoDialog_->close();
        return;
    }
    spdlog::info("User opened Image Info dialog");
    if (!infoDialog_)
    {
        infoDialog_ = new InfoDialog(this);
        infoDialog_->setAttribute(Qt::WA_DeleteOnClose, false);
        // Zoom/pan and contrast/colormap changes have no signal to hook, so
        // poll while visible; InfoDialog::setInfo() ignores unchanged text.
        infoRefreshTimer_ = new QTimer(infoDialog_);
        infoRefreshTimer_->setInterval(500);
        connect(infoRefreshTimer_, &QTimer::timeout, this, &MainWindow::refreshInfoDialog);
    }
    infoRefreshTimer_->start();
    connect(infoDialog_, &QDialog::finished, infoRefreshTimer_, &QTimer::stop, Qt::UniqueConnection);
    refreshInfoDialog();
    infoDialog_->show();
    infoDialog_->raise();
    infoDialog_->activateWindow();
}

void MainWindow::refreshInfoDialog()
{
    if (!infoDialog_ || !infoDialog_->isVisible()) return;
    QString name = QFileInfo(QString::fromStdString(viewport_->currentImageName())).fileName();
    if (name.isEmpty()) name = loadedBaseName_;
    infoDialog_->setInfo(QString("Info: %1").arg(name), buildInfoText());
}

namespace
{
QString humanBytes(qint64 n)
{
    if (n < 1024) return QString("%1 B").arg(n);
    double v = n;
    const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
    int u = -1;
    while (v >= 1024.0 && u < 3)
    {
        v /= 1024.0;
        ++u;
    }
    return QString("%1 %2 (%3 bytes)").arg(v, 0, 'f', 1).arg(units[u]).arg(n);
}

const char* sampleTypeName(WivSampleType t)
{
    switch (t)
    {
        case WIV_SAMPLE_U8: return "uint8";
        case WIV_SAMPLE_U16: return "uint16";
        case WIV_SAMPLE_FLOAT: return "float32";
        default: return "none (RGBA8 only)";
    }
}
}

QString MainWindow::buildInfoText() const
{
    // Rich text: each section is a heading followed by a two-column table.
    QString t;
    bool inTable = false;
    auto closeTable = [&]() {
        if (inTable) t += "</table>";
        inTable = false;
    };
    auto row = [&](const QString& key, const QString& value) {
        t += QString("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(key.toHtmlEscaped(), value.toHtmlEscaped());
    };
    auto section = [&](const QString& title) {
        closeTable();
        t += QString("<h3>%1</h3><table cellspacing='0' cellpadding='3'>").arg(title.toHtmlEscaped());
        inTable = true;
    };

    // ---- Current image ----
    const giv::LoadedImage* img = viewport_->currentImage();
    if (img)
    {
        section("Image");
        const QString path = QString::fromStdString(viewport_->currentImageName());
        QFileInfo fi(path);
        row("Path", fi.absoluteFilePath());
        if (fi.exists())
        {
            row("File size", humanBytes(fi.size()));
            row("Modified", fi.lastModified().toString(Qt::ISODate).replace('T', ' '));
        }
        // "\u00d7" is giv's multiplication sign.
        row("Resolution", QString("%1\u00d7%2").arg(img->width).arg(img->height));
        row("Megapixels", QString::number(double(img->width) * img->height / 1e6, 'f', 2));
        const auto& size = viewport_->currentImageSize();
        if (viewport_->pixelSize() != 1.0 || !viewport_->pixelSizeUnit().empty())
        {
            const QString unit = QString::fromStdString(viewport_->pixelSizeUnit());
            row("Pixel size", QString("%1 %2").arg(viewport_->pixelSize(), 0, 'g', 6).arg(unit));
            if (size)
                row("Physical size", QString("%1\u00d7%2 %3")
                                          .arg(size->first * viewport_->pixelSize(), 0, 'g', 6)
                                          .arg(size->second * viewport_->pixelSize(), 0, 'g', 6)
                                          .arg(unit));
        }
        row("Sample type", sampleTypeName(img->sampleType));
        if (img->sampleType != WIV_SAMPLE_NONE)
        {
            row("min max", QString("%1 %2").arg(img->sampleMin, 0, 'g', 6).arg(img->sampleMax, 0, 'g', 6));
            const auto state = viewport_->currentContrastState();
            row("Contrast window", QString("%1 .. %2").arg(state.min, 0, 'g', 6).arg(state.max, 0, 'g', 6));
            row("Color table", state.colormapEnabled ? QString::fromLatin1(giv::colormaps::name(state.colormapId))
                                                      : QString("gray"));
        }
        row("Slices", img->depth > 1 ? QString("%1 (showing %2)").arg(img->depth).arg(viewport_->currentSlice() + 1)
                                       : QString("1"));
        const qint64 mem = qint64(img->rgba.size()) + qint64(img->samples.size());
        row("Decoded memory", humanBytes(mem));
    }

    // ---- Scene (.giv marks) ----
    const auto& scene = viewport_->sceneData();
    if (!scene.datasets.empty())
    {
        section("Marks");
        for (const auto& path : loadedPaths_) row("Path", path);
        size_t points = 0, visible = 0, sprites = 0, texts = 0;
        for (const auto& ds : scene.datasets)
        {
            points += ds.pointCount();
            texts += ds.texts.size();
            if (ds.isVisible) ++visible;
            if (ds.isSprite) ++sprites;
        }
        row("Datasets", QString("%1 (%2 visible)").arg(scene.datasets.size()).arg(visible));
        row("Points", QString::number(points));
        if (texts) row("Text labels", QString::number(texts));
        if (sprites) row("Sprites", QString::number(sprites));
        if (scene.hasBounds())
            row("Bounds", QString("x %1..%2, y %3..%4")
                              .arg(scene.minX, 0, 'g', 6).arg(scene.maxX, 0, 'g', 6)
                              .arg(scene.minY, 0, 'g', 6).arg(scene.maxY, 0, 'g', 6));
    }
    closeTable();
    return t;
}

void MainWindow::showColorTableDialog()
{
    spdlog::info("User opened Color Table dialog");
    if (!colorTableDialog_)
    {
        colorTableDialog_ = new ColorTableDialog(viewport_, this);
        connect(colorTableDialog_, &ColorTableDialog::colormapChanged, this, [this] {
            if (contrastDialog_) contrastDialog_->refreshForImage();
        });
    }
    colorTableDialog_->refreshForImage();
    colorTableDialog_->show();
    colorTableDialog_->raise();
    colorTableDialog_->activateWindow();
}

void MainWindow::updateContrastToolsEnabled()
{
    const giv::LoadedImage* img = viewport_->currentImage();
    bool hasRawSamples = img && img->sampleType != WIV_SAMPLE_NONE;
    contrastAction_->setEnabled(hasRawSamples);
    colorTableAction_->setEnabled(hasRawSamples);
}

} // namespace givqt
