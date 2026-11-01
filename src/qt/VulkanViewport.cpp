#include "VulkanViewport.h"

#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QWheelEvent>
#include "AppPaths.h"
#include "PathUtils.h"

#include "DisplayImage.h"
#include "GivParser.h"
#include "ImagePluginHost.h"
#include "PreferencesDialog.h"
#include "ViewFlip.h"

#include <spdlog/spdlog.h>

#include <vsg/all.h>
#include <vsgXchange/all.h>
#include <vsgXchange/freetype.h>

#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QGridLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QThread>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>

namespace givqt
{

// Transparent widget laid over the embedded native Vulkan surface purely to
// receive file drops (same trick as the reference application's DragDropOverlay): Qt picks
// the drop target by its own widget z-order, not native window stacking.
class DropOverlay : public QWidget
{
public:
    explicit DropOverlay(QWidget* parent, std::function<void(QStringList)> onDrop)
        : QWidget(parent), onDrop_(std::move(onDrop))
    {
        setAcceptDrops(true);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override
    {
        QStringList files;
        for (const QUrl& url : event->mimeData()->urls())
            if (url.isLocalFile()) files << url.toLocalFile();
        if (files.isEmpty()) return;
        event->acceptProposedAction();
        onDrop_(files);
    }

private:
    std::function<void(QStringList)> onDrop_;
};

namespace
{
// Shared 1-D scrollbar sync: contentLenPx/viewLenPx/posPx are all in the same
// device-pixel-at-current-zoom units (see updateScrollBars() below for how
// each axis maps world coordinates into these). posPx is the position of the
// visible window's "start" edge (left edge for X, top edge for Y) measured
// from the content's own start edge, in that same axis's scrollbar-value
// direction. Hides (and zeroes) the bar whenever the content already fits the
// view - i.e. at or below the zoom level that fits the whole scene, there's
// nothing to scroll to, so the bar is fully hidden rather than left visible
// but disabled.
void applyScrollBarRange(QScrollBar* bar, double contentLenPx, double viewLenPx, double posPx)
{
    QSignalBlocker blocker(bar);
    if (contentLenPx <= viewLenPx + 0.5)
    {
        bar->setEnabled(false);
        bar->setRange(0, 0);
        bar->hide();
        return;
    }
    int page = std::max(1, static_cast<int>(std::lround(viewLenPx)));
    int maxVal = std::max(0, static_cast<int>(std::lround(contentLenPx - viewLenPx)));
    bar->setEnabled(true);
    bar->setPageStep(page);
    bar->setSingleStep(std::max(1, page / 10));
    bar->setRange(0, maxVal);
    bar->setValue(std::clamp(static_cast<int>(std::lround(posPx)), 0, maxVal));
    bar->show();
}
} // namespace

VulkanViewport::VulkanViewport(QWidget* parent) : QWidget(parent)
{
    traits_ = vsg::WindowTraits::create();
    traits_->windowTitle = "wiv";

    // See the original (pre-Qt) src/main.cpp for the rationale behind these
    // two non-default swapchain settings: giv composites colors without any
    // color management, so a UNORM (not sRGB) swapchain format is needed to
    // match its raw byte output; MSAA approximates giv/AGG's software
    // antialiasing.
    traits_->swapchainPreferences.surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    traits_->samples = VK_SAMPLE_COUNT_4_BIT;

    // FIFO is the only mode guaranteed never to tear: presents are queued and
    // flipped at vblank. FIFO_RELAXED was used previously to dodge a hang
    // (observed: proprietary NVIDIA) where a blocking present never woke up
    // for an occluded/not-yet-composited window, but it presents immediately
    // whenever a frame misses vblank, which shows up as visible tearing
    // during animated zoom. If the startup hang returns, WIV_PRESENT_MODE
    // (mailbox/fifo/fifo_relaxed/immediate) overrides this.
    traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (const char* mode = std::getenv("WIV_PRESENT_MODE"))
    {
        std::string m(mode);
        if (m == "mailbox") traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (m == "fifo") traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        else if (m == "fifo_relaxed") traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        else if (m == "immediate") traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // WIV_VALIDATE=1 turns on VK_LAYER_KHRONOS_validation (requires the
    // vulkan-validation-layers package) - for tracking down invalid Vulkan
    // API usage (bad descriptor/pipeline bindings, sync errors, etc.), which
    // is one plausible explanation for an otherwise-unexplained
    // VK_ERROR_DEVICE_LOST. Off by default: the validation layer adds
    // significant per-call overhead.
    if (std::getenv("WIV_VALIDATE") != nullptr) traits_->debugLayer = true;

    viewer_ = GivViewer::create(8);

    // Render on demand rather than unconditionally on every timer tick:
    // vsgQt::Window's own mouse/key/resize/expose handlers (and every
    // VulkanViewport method that changes the view - pan/zoom, fit,
    // visibility toggles, background color, $image cycling, balloon
    // toggle) already call viewer_->request() themselves, so nothing here
    // needs an extra nudge; this just stops GivViewer::render() from
    // recording/submitting/presenting a frame when nothing has requested
    // one (see its `!continuousUpdate && requests.load() == 0` check).
    // WIV_CONTINUOUS=1 forces a frame every timer tick regardless of
    // requests - only for benchmarking the render path (WIV_TIMING's
    // per-frame ms breakdown is meaningless in the default on-demand mode,
    // which idles between requests).
    viewer_->continuousUpdate = std::getenv("WIV_CONTINUOUS") != nullptr;

    viewer_->worldPerPixel = [this]() -> float {
        if (!projection_ || !window_ || !window_->windowAdapter) return 1.0f;
        auto extent = window_->windowAdapter->extent2D();
        if (extent.width == 0) return 1.0f;
        return static_cast<float>((projection_->right - projection_->left) / static_cast<double>(extent.width));
    };

    viewer_->isResizeSettling = [this]() { return checkResizeSettling(); };
    viewer_->addEventHandler(vsg::CloseHandler::create(viewer_));

    // Top-level (Qt::ToolTip) popup for the balloon tooltip - see
    // BalloonController.h for why this can't be a vsg scene node or a plain
    // child QWidget: wiv's Vulkan surface below is a native child window
    // (createWindowContainer()), which always composites above any sibling
    // QWidget regardless of z-order. `this` is passed only for Qt's
    // parent/child lifetime management; Qt::ToolTip still makes it an
    // independent top-level window, positioned in global screen coordinates
    // by BalloonController::update().
    balloonLabel_ = new QLabel(this, Qt::ToolTip | Qt::FramelessWindowHint);
    balloonLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    balloonLabel_->setStyleSheet("QLabel { background-color: rgba(255, 255, 0, 230); color: black; padding: 6px; }");
    balloonLabel_->hide();

    window_ = new vsgQt::Window(viewer_, traits_, static_cast<QWindow*>(nullptr));
    window_->setTitle("wiv");
    // Alt+wheel (giv's quiver-arrow zoom): Qt moves the delta to the x axis
    // when Alt is held, which vsgQt::Window ignores (it would report every
    // such event as scroll-up), so intercept it before vsgQt sees it.
    window_->installEventFilter(this);
#ifdef _WIN32
    // The window is created as a decorated top-level and only later embedded
    // via createWindowContainer(); on Windows the leftover frame margins offset
    // and oversize the child HWND (covering the menu bar), so make it frameless.
    window_->setFlags(Qt::FramelessWindowHint);
#endif

    // Create the Vulkan surface/device/swapchain while the QWindow is still
    // a plain top-level (unparented) window, *then* reparent/embed it via
    // createWindowContainer() below - matching the reference application's (proven-working,
    // same vsgQt library, same NVIDIA driver) Widget3D construction order.
    // Doing it the other way around - embedding first, initializing the
    // Vulkan surface against an already-reparented native window - is what
    // wiv did originally, and reliably hung the very first present() call
    // (main thread stuck forever in libnvidia-glcore.so's poll()).
    window_->initializeWindow();

    auto layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    container_ = QWidget::createWindowContainer(window_, this);
    layout->addWidget(container_, 0, 0);
    // The container (not this widget) is what actually sizes the native window
    // - scrollbars appearing/disappearing resize it without resizing us - so
    // its Resize events drive the camera too; see handleViewportResize().
    container_->installEventFilter(this);
    // Same grid cell, added after the container so it sits above it.
#ifndef _WIN32
    auto* dropOverlay = new DropOverlay(this, [this](QStringList f) { emit filesDropped(f); });
    layout->addWidget(dropOverlay, 0, 0);
    dropOverlay->raise();
#endif // Windows uses WinDropTarget on the top-level window instead.

    hScrollBar_ = new QScrollBar(Qt::Horizontal, this);
    vScrollBar_ = new QScrollBar(Qt::Vertical, this);
    hScrollBar_->setEnabled(false);
    vScrollBar_->setEnabled(false);
    hScrollBar_->hide();
    vScrollBar_->hide();
    layout->addWidget(vScrollBar_, 0, 1);
    layout->addWidget(hScrollBar_, 1, 0);
    setLayout(layout);

    // Scrollbar -> camera: reposition the view's left/top edge to the
    // dragged-to content-pixel offset, keeping the current zoom level fixed.
    // Mirrors giv's hadjustment/vadjustment_value_changed handlers
    // (gtk-image-viewer.c) but in Qt's plain int pixel-at-current-zoom units
    // rather than giv's normalized [0,1] adjustment range.
    connect(hScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!camera_ || !projection_ || !window_ || !window_->windowAdapter) return;
        auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
        auto extent = window_->windowAdapter->extent2D();
        if (!lookAt || extent.width == 0) return;
        double halfW = (projection_->right - projection_->left) * 0.5;
        double scaleX = extent.width / (2.0 * halfW);
        double minXyd, minYyd, maxXyd, maxYyd;
        currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);
        double newCenterX = flipHorizontal_ ? maxXyd - static_cast<double>(value) / scaleX - halfW
                                            : minXyd + static_cast<double>(value) / scaleX + halfW;
        double shift = newCenterX - lookAt->center.x;
        lookAt->eye.x += shift;
        lookAt->center.x += shift;
        viewDirty_ = true;
        viewer_->request();
    });
    connect(vScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
        if (!camera_ || !projection_ || !window_ || !window_->windowAdapter) return;
        auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
        auto extent = window_->windowAdapter->extent2D();
        if (!lookAt || extent.height == 0) return;
        double halfH = (projection_->top - projection_->bottom) * 0.5;
        double scaleY = extent.height / (2.0 * halfH);
        double minXyd, minYyd, maxXyd, maxYyd;
        currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);
        double contentMaxY = -minYyd; // world-space top edge, see currentFitBoundsYDown()/fitToBounds()
        double newCenterY = flipVertical_ ? -maxYyd + static_cast<double>(value) / scaleY + halfH
                                          : contentMaxY - static_cast<double>(value) / scaleY - halfH;
        double shift = newCenterY - lookAt->center.y;
        lookAt->eye.y += shift;
        lookAt->center.y += shift;
        viewDirty_ = true;
        viewer_->request();
    });

    options_ = vsg::Options::create();
    options_->paths = vsg::getEnvPaths("VSG_FILE_PATH");
    options_->add(vsgXchange::all::create());
    options_->setValue(vsgXchange::freetype::texel_margin_ratio, 0.5f);
    options_->setValue(vsgXchange::freetype::quad_margin_ratio, 0.25f);

#ifndef WIV_SHADER_DIR
#    define WIV_SHADER_DIR "shaders"
#endif
    shaderDir_ = giv::resourceDir("shaders", "share/wiv/shaders", WIV_SHADER_DIR);

    // Set the real (saved-preference) background color before the bootstrap
    // loadFiles({}) below builds the first RenderGraph: RenderGraph::
    // setClearValues() only ever reads window_->windowAdapter->clearColor()
    // once, at construction time (see its doc comment on setBackgroundColor()
    // below) - leaving it at VSG's own default (a bluish gray) would seed
    // every render until the next full rebuild with the wrong color, since
    // MainWindow's own setBackgroundColor() call (which mirrors this same
    // preference) only runs after this constructor returns.
    QColor initialBackground = PreferencesDialog::loadBackgroundColor();
    window_->windowAdapter->clearColor() =
        vsg::vec4(initialBackground.redF(), initialBackground.greenF(), initialBackground.blueF(), 1.0f);

    // Compile a valid (if empty) Vulkan scene graph up front, unconditionally
    // - not only when a file is passed on the command line. loadFiles()
    // (called with no paths, which it handles as a legitimate empty scene)
    // is what runs rebuildSceneGraph()'s isInitialLoad path, and with it
    // viewer_->compile(): that's what actually builds this window's Vulkan
    // device/surface/swapchain. Without this, running with no file argument
    // left those null (main.cpp only calls loadFiles() "if (!files.isEmpty())"),
    // and the first native-window resize event Qt delivers once the event
    // loop starts (from createWindowContainer() above reparenting/embedding
    // this window) segfaulted inside Xcb_Window::resize()'s buildSwapchain(),
    // which dereferences that still-null device/surface/physicalDevice.
    loadFiles({});
}

VulkanViewport::~VulkanViewport()
{
    // Unlike waitForGpuIdle() (bounded, used mid-session so a wedged GPU
    // can't hang rebuildSceneGraph() forever), this runs once at shutdown,
    // so an unconditional wait is fine - and necessary: with no destructor
    // at all, the members below (viewer_ first, per C++ reverse-declaration-
    // order) were destroyed while the GPU could still be mid-flight on the
    // previous frame's submit/present, which is undefined behavior (observed
    // as validation-layer heap corruption and a SIGSEGV inside
    // libVkLayer_khronos_validation.so on quit, on whichever thread happened
    // to touch the corrupted state next).
    if (viewer_) viewer_->deviceWaitIdle();
}

void VulkanViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    handleViewportResize();
}

void VulkanViewport::preserveViewOnNextResize(bool entering)
{
    if (!projection_ || lastWidth_ <= 0) return;
    preserveHalfW_ = (projection_->right - projection_->left) * 0.5;
    preserveHalfH_ = (projection_->top - projection_->bottom) * 0.5;
    preserveView_ = preserveHalfW_ > 0.0 && preserveHalfH_ > 0.0;
    if (!preserveView_) return;
    const double current = lastWidth_ / (2.0 * preserveHalfW_);
    preserveTarget_ = 0.0;
    if (entering)
        restoreScale_ = current;
    else if (restoreScale_ > 0.0 && appliedScale_ > 0.0 && std::abs(current - appliedScale_) < 1e-3 * appliedScale_)
        preserveTarget_ = restoreScale_; // view untouched while large: go back to exactly the old one
    if (!entering) restoreScale_ = 0.0;
    const int gen = ++preserveGen_;
    QTimer::singleShot(1500, this, [this, gen]() {
        if (gen == preserveGen_) preserveView_ = false;
    });
}

void VulkanViewport::handleViewportResize()
{
    if (!camera_ || !projection_ || !window_ || !window_->windowAdapter) return;

    // createWindowContainer()'s usual auto-sync of the embedded QWindow's
    // geometry to its container widget does not reliably fire here (observed:
    // window_->geometry() stuck at the WindowTraits-default 1280x1024 while
    // this widget/container_ had already been laid out to a smaller size) -
    // force it explicitly rather than relying on Qt to do it.
    if (window_->geometry().size() != container_->size()) window_->resize(container_->size());

    // vsgQt::Window::resizeEvent (fired on the embedded QWindow itself, as
    // Qt's layout resizes the container to match this widget) already calls
    // windowAdapter->resize() to keep the swapchain in sync; here we only
    // need to keep the camera's viewport/ortho extents matching the new
    // *device-pixel* size (extent2D(), not this QWidget's logical width()/
    // height() - they differ under devicePixelRatio() != 1).
    auto extent = window_->windowAdapter->extent2D();
    if (extent.width == 0 || extent.height == 0) return;

    // extent2D() can still be reporting the *old* size here - see
    // ensureExtentSettled()'s doc comment - in which case the swapchain
    // rebuild for this resize hasn't happened yet and is still pending,
    // asynchronously, on some later iteration of GivViewer's render loop.
    // Track the size we actually asked for so isResizeSettling() can hold
    // frames off until extent2D() catches up to it (see its doc comment for
    // why racing that rebuild instead is unsafe).
    // The native window is sized by container_, which is smaller than this
    // widget whenever a scrollbar is showing; waiting for this widget's size
    // would never match and always run out the settle timeout.
    int wantW = static_cast<int>(std::lround(container_->width() * devicePixelRatioF()));
    int wantH = static_cast<int>(std::lround(container_->height() * devicePixelRatioF()));
    if (static_cast<int>(extent.width) != wantW || static_cast<int>(extent.height) != wantH)
    {
        resizeSettling_ = true;
        pendingResizeExtent_ = QSize(wantW, wantH);
    }

    applyExtent(extent);
}

// Brings the camera/render graph and the projection in line with `extent`,
// preserving the current pixels-per-world-unit scale. Called from
// resizeEvent() and again from checkResizeSettling() once extent2D() has
// caught up: resizeEvent() often runs while extent2D() still reports the
// *old* size (see ensureExtentSettled()), so without that second pass the
// camera and projection stay built from the stale extent and the image is
// stretched (seen after toggling a dock widget, which resizes the viewport
// once with no later resizeEvent to fix it up).
void VulkanViewport::applyExtent(VkExtent2D extent)
{
    cancelViewAnimation(); // its targets were computed for the old extent
    syncRenderExtent(extent);

    // The visible half-extents just grow/shrink to match the new size,
    // centered on the same point (rather than re-fitting the whole view).
    double oldHalfW = (projection_->right - projection_->left) * 0.5;
    if (lastWidth_ > 0 && lastHeight_ > 0 && oldHalfW > 0.0)
    {
        double scale = lastWidth_ / (2.0 * oldHalfW); // pixels per world unit
        if (preserveView_)
        {
            scale = preserveTarget_ > 0.0
                        ? preserveTarget_
                        : std::min(extent.width / (2.0 * preserveHalfW_), extent.height / (2.0 * preserveHalfH_));
            appliedScale_ = scale;
        }
        double newHalfW = static_cast<double>(extent.width) / (2.0 * scale);
        double newHalfH = static_cast<double>(extent.height) / (2.0 * scale);
        projection_->left = -newHalfW;
        projection_->right = newHalfW;
        projection_->bottom = -newHalfH;
        projection_->top = newHalfH;
    }
    lastWidth_ = static_cast<int>(extent.width);
    lastHeight_ = static_cast<int>(extent.height);
    viewer_->request();
    updateScrollBars();
}

bool VulkanViewport::checkResizeSettling()
{
    if (!resizeSettling_) return false;
    if (!window_ || !window_->windowAdapter)
    {
        resizeSettling_ = false;
        return false;
    }
    auto extent = window_->windowAdapter->extent2D();
    if (static_cast<int>(extent.width) == pendingResizeExtent_.width() &&
        static_cast<int>(extent.height) == pendingResizeExtent_.height())
    {
        resizeSettling_ = false;
        settleTicks_ = 0;
        // resizeEvent() may have synced the camera to the pre-resize extent.
        applyExtent(window_->windowAdapter->extent2D());
        return false;
    }

    // On Windows the native child window's client size can settle on a value
    // that never exactly matches the requested one (DPI rounding / the
    // container's own sizing), which would hold off rendering forever. Nudge
    // the swapchain to re-read the surface size a few times, then accept
    // whatever extent the surface really has and resync the camera to it.
    ++settleTicks_;
    if (settleTicks_ % 10 == 0) window_->windowAdapter->resize();
    if (settleTicks_ > 60)
    {
        resizeSettling_ = false;
        settleTicks_ = 0;
        applyExtent(window_->windowAdapter->extent2D());
        return false;
    }
    return true;
}

bool VulkanViewport::loadFiles(const std::vector<std::string>& paths, QString* error)
{
    if (!window_ || !window_->windowAdapter)
    {
        if (error) *error = "Vulkan viewport is not yet initialized";
        return false;
    }

    saveCurrentView(); // the view of the file being replaced (see setRememberViewPerFile())

    giv::SceneData newScene;
    auto parseStart = std::chrono::steady_clock::now();
    size_t totalPoints = 0;
    std::vector<std::string> explicitImages; // bare image paths named by the user
    for (const auto& f : paths)
    {
        // Mirrors giv's own load_file (giv-win.gob): only .giv/.marks/.svg
        // go through the text parser - anything else is a bare image file
        // and is loaded directly, the same way a $image reference inside a
        // .giv file is (see the loadedImages loop below).
        std::string ext = giv::pathToUtf8(giv::utf8ToPath(f).extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".giv" || ext == ".marks" || ext == ".svg")
        {
            giv::GivParser parser;
            std::string parseError;
            if (!parser.parseFile(f, newScene, parseError))
            {
                spdlog::error("Failed to parse {}: {}", f, parseError);
                if (error) *error = QString::fromStdString(parseError);
                return false;
            }
        }
        else
        {
            // Unlike a $image reference discovered inside a .giv file (which
            // may legitimately not resolve - see the silent `continue` below),
            // this path was named directly (command line or File > Open): a
            // typo here should be reported, not silently produce an image-less
            // scene.
            if (!std::filesystem::exists(f))
            {
                if (error) *error = QString("could not open file: %1").arg(QString::fromStdString(f));
                return false;
            }
            newScene.images.push_back(f);
            explicitImages.push_back(f);
        }
    }
    for (const auto& ds : newScene.datasets) totalPoints += ds.pointCount();
    double parseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parseStart).count();

    // Resolve $image references to a candidate path list. Mirrors giv's own
    // cb_image_reference: a filename is used as-is if it already resolves
    // (absolute, or relative to the current working directory); otherwise
    // it's retried relative to the directory of the first loaded .giv file.
    // Deliberately *not* decoded here - only a cheap existence + extension
    // check - so that opening a folder of hundreds of images doesn't decode
    // (and hold in memory, and upload to the GPU) every one of them before
    // the window even shows; see decodeCurrentImage() for the actual, lazy,
    // decode-on-display path.
    std::vector<std::string> newLoadedImageNames;
    std::filesystem::path givDir = paths.empty() ? std::filesystem::path() : giv::utf8ToPath(paths.front()).parent_path();
    for (const auto& imgRef : newScene.images)
    {
        std::filesystem::path resolved = giv::utf8ToPath(imgRef);
        if (!std::filesystem::exists(resolved) && !givDir.empty())
            resolved = givDir / imgRef;

        const bool named = std::find(explicitImages.begin(), explicitImages.end(), imgRef) != explicitImages.end();
        if (!std::filesystem::exists(resolved))
            continue;
        if (!giv::ImagePluginHost::isSupported(giv::pathToUtf8(resolved)))
        {
            // A file the user named directly gets a content sniff (it may
            // have an unrecognized suffix) and an error if it still isn't
            // an image; a $image reference inside a .giv is skipped silently.
            if (!named)
                continue;
            if (!giv::ImagePluginHost::isSupportedExplicit(giv::pathToUtf8(resolved)))
            {
                if (error) *error = QString("unsupported file format: %1").arg(QString::fromStdString(imgRef));
                return false;
            }
        }

        newLoadedImageNames.push_back(giv::pathToUtf8(resolved));
    }

    scene_ = std::move(newScene);
    // $pixelsize directive (see SceneData::pixelSize) - only touch the
    // calibration if the file actually specified one, so a file that
    // doesn't mention it leaves whatever the user last dialed in (via Tools
    // > Calibrate Pixel Size) alone, matching giv's own cb_set_pixelsize
    // (fired only when the directive is present).
    if (scene_.pixelSize) setPixelCalibration(*scene_.pixelSize, scene_.pixelSizeUnit);
    loadedImageNames_ = std::move(newLoadedImageNames);
    hasScene_ = true;
    currentImageIndex_ = 0;
    decodeCurrentImage();

    auto buildStart = std::chrono::steady_clock::now();
    viewKeyFallback_ = paths.empty() ? std::string() : paths.front();
    if (!rebuildSceneGraph(error, /*isInitialLoad=*/true)) return false;
    tryRestoreView();
    double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();

    spdlog::info(
        "Scene loaded: {} dataset(s), {} point(s), {} image reference(s) resolved (parse {:.1f} ms, scene build {:.1f} ms)",
        scene_.datasets.size(), totalPoints, loadedImageNames_.size(), parseMs, buildMs);

    emit sceneLoaded();
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
    return true;
}

bool VulkanViewport::waitForGpuIdle()
{
    constexpr uint64_t kFenceTimeoutNs = 2'000'000'000; // 2 seconds
    for (size_t relativeFrameIndex = 0; relativeFrameIndex <= 2; ++relativeFrameIndex)
    {
        VkResult result = viewer_->waitForFences(relativeFrameIndex, kFenceTimeoutNs);
        if (result != VK_SUCCESS)
        {
            spdlog::error(
                "GPU didn't finish frame -{} within {} ms (VkResult={}); skipping this compile rather than risking a hang or destroying in-flight GPU resources",
                relativeFrameIndex, kFenceTimeoutNs / 1000000, static_cast<int>(result));
            return false;
        }
    }
    return true;
}

bool VulkanViewport::rebuildSceneGraph(QString* error, bool isInitialLoad)
{
    // The global "View Marks" switch hides every dataset without disturbing
    // the per-dataset isVisible flags the Mark Browser tree maintains - so
    // apply it only for the duration of this build, then restore.
    std::vector<bool> savedVisible(scene_.datasets.size());
    for (size_t i = 0; i < scene_.datasets.size(); ++i)
    {
        savedVisible[i] = scene_.datasets[i].isVisible;
        scene_.datasets[i].isVisible = savedVisible[i] && globalShowMarks_;
    }

    // A persistent SceneBuilder (not a fresh one per call): its build()
    // caches the view-params/image pipeline plumbing across calls (see its
    // doc comment), which only pays off if the same instance is reused for
    // every $image cycling step instead of starting from scratch each time.
    if (!sceneBuilder_) sceneBuilder_ = std::make_unique<giv::SceneBuilder>(options_);
    giv::SceneBuilder& builder = *sceneBuilder_;
    vsg::ref_ptr<vsg::Group> sceneGraph;
    try
    {
        sceneGraph = builder.build(scene_, shaderDir_, displayImages_);
    }
    catch (const std::exception& e)
    {
        for (size_t i = 0; i < scene_.datasets.size(); ++i) scene_.datasets[i].isVisible = savedVisible[i];
        if (error) *error = QString("failed to build scene: %1").arg(e.what());
        return false;
    }
    catch (const vsg::Exception& e)
    {
        // vsg::Exception (e.g. VK_ERROR_OUT_OF_DEVICE_MEMORY) does not derive
        // from std::exception, so the handler above misses it and the
        // uncaught throw terminates the process.
        for (size_t i = 0; i < scene_.datasets.size(); ++i) scene_.datasets[i].isVisible = savedVisible[i];
        if (error) *error = QString("failed to build scene: %1 (VkResult %2)").arg(QString::fromStdString(e.message)).arg(e.result);
        return false;
    }
    for (size_t i = 0; i < scene_.datasets.size(); ++i) scene_.datasets[i].isVisible = savedVisible[i];

    // A visibility-only rebuild replaces command/render graphs (and the
    // pipelines/descriptor sets/buffers they reference) that the GPU may
    // still be mid-flight on from a previous frame - dropping the old
    // vsg::ref_ptrs below would destroy those Vulkan objects out from under
    // an in-progress vkQueueSubmit/present, which is undefined behavior (and
    // was observed to eventually wedge the window: rendering kept running
    // but the Qt event loop stopped servicing input). Only needed once the
    // viewer has actually presented a frame before (i.e. never on the very
    // first build) - tracked by everBuiltSceneGraph_ below rather than this
    // call's own isInitialLoad, since isInitialLoad is true for every full
    // loadFiles() call (command-line, File>Open, directory navigation, ...),
    // not just the constructor's bootstrap one.
    //
    // This used to call viewer_->deviceWaitIdle(), which waits unconditionally
    // (no timeout) for every device to go idle, including the outstanding
    // present of the frame the GPU is mid-flight on. That present's
    // completion is itself signalled through the X11/DRI3 handshake with the
    // compositor - and was observed (proprietary NVIDIA driver) to never wake
    // up if the window was transiently occluded/not yet composited at that
    // exact moment, permanently wedging *this* thread inside the driver, and
    // with it the whole GUI (menus, Mark Browser, everything) since this
    // runs on the Qt GUI thread. Waiting on the specific frame fences instead
    // - with a timeout - bounds the wait: on timeout we just skip this
    // rebuild (old graph stays in place, nothing gets destroyed out from
    // under the GPU) rather than hanging forever. Triple buffering
    // (WindowTraits::swapchainPreferences.imageCount, see the constructor)
    // means up to 2 prior frames can still be in flight, on top of the one
    // just submitted - per RecordAndSubmitTask::fence()'s doc comment,
    // relativeFrameIndex 0 is *not* "nothing submitted yet", it's the most
    // recently submitted frame, quite possibly still in flight; starting
    // this loop at 1 (skipping 0) was exactly the bug the Vulkan validation
    // layer caught - vkDestroyPipeline/vkDestroyFence/vkFreeCommandBuffers
    // all firing on objects "currently in use by VkQueue/VkCommandBuffer".
    if (everBuiltSceneGraph_ && !waitForGpuIdle())
    {
        if (error) *error = "GPU is not responding; try again";
        return false;
    }
    everBuiltSceneGraph_ = true;

    // Drop the previous build's event handlers (pan/zoom + balloon + caliper)
    // but keep the CloseHandler added once in the constructor.
    auto& handlers = viewer_->getEventHandlers();
    handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                   [](const vsg::ref_ptr<vsg::Visitor>& h) {
                                       return h.cast<giv::PanZoomHandler>() || h.cast<giv::BalloonController>() || h.cast<giv::CaliperTool>();
                                   }),
                   handlers.end());

    // A brand-new camera/projection is only needed on the very first build
    // ever (camera_ still null) - every subsequent loadFiles() (File>Open,
    // directory navigation, ...) reuses the existing one so that, when
    // autoFit_ is off, the previously dialed-in zoom/pan carries over onto
    // the newly loaded content instead of resetting - matching the
    // non-autofit branch of switchToImage() further down this file.
    bool needsNewCamera = !camera_;
    if (needsNewCamera)
    {
        auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 1.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
        flipProjection_ = giv::FlipOrthographic::create(-100.0, 100.0, -100.0, 100.0, 0.01, 100.0);
        flipProjection_->flipX = flipHorizontal_;
        flipProjection_->flipY = flipVertical_;
        flipProjection_->quarterTurns = quarterTurns_;
        projection_ = flipProjection_;
        camera_ = vsg::Camera::create(projection_, lookAt, vsg::ViewportState::create(window_->windowAdapter->extent2D()));
    }

    if (isInitialLoad && (needsNewCamera || autoFit_))
    {
        double minXyd, minYyd, maxXyd, maxYyd;
        currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);

        // Rough placeholder only when needsNewCamera: window_->windowAdapter
        // ->extent2D() is not yet reliable here - loadFiles() runs before
        // window.show() (see main.cpp), and separately the embedded native
        // window doesn't always pick up its true on-screen size this early
        // either (see MainWindow::showEvent()'s doc comment).
        // MainWindow::showEvent() redoes this properly - via the exact same
        // fitToWindow() the "Fit to Window" menu action calls - once the
        // window is actually shown and both are guaranteed correct.
        fitToBounds(minXyd, -maxYyd, maxXyd, -minYyd);
    }

    panZoom_ = giv::PanZoomHandler::create(camera_);
    panZoom_->onCursorMove = [this](double x, double y) { emit cursorWorldPosition(x, y); };
    panZoom_->onViewChanged = [this]() {
        cancelViewAnimation(); // user input takes over
        viewDirty_ = true;
        updateScrollBars();
    };
    panZoom_->onClick = [this](double x, double y, int button, int modifiers) { emit clicked(x, y, button, modifiers); };
    panZoom_->verticalLock = verticalZoomLock_;
    // A wheel step zooms over a short motion: tick it from a timer (the
    // render loop is on demand) until it ends.
    zoomTimer_ = new QTimer(this);
    zoomTimer_->setInterval(8);
    connect(zoomTimer_, &QTimer::timeout, this, [this]() {
        if (!panZoom_->advance()) zoomTimer_->stop();
        viewer_->request();
    });
    panZoom_->onZoomStarted = [this]() { zoomTimer_->start(); };
    viewer_->addEventHandler(panZoom_);

    bool wasBalloonEnabled = isInitialLoad ? false : balloonEnabled_;

    // labelPicker_ owns a vsg::View (its off-screen pick render), and
    // vsg::View hands out viewIDs from a free-list that only grows if the
    // previous holder hasn't been destructed yet (see the mainView_ comment
    // below) - recreating it every rebuild was one more source of that
    // growth, and unlike the main scene it doesn't actually need it: it now
    // takes the freshly-built label graph via updateScene() instead of being
    // rebuilt from scratch. Construct it once and keep reusing it.
    if (!labelPicker_)
        labelPicker_ = giv::LabelPicker::create(window_->windowAdapter, camera_, builder.labelGraph());
    else
    {
        // needsNewCamera above may have just replaced camera_ with a
        // brand-new vsg::Camera - labelPicker_ itself is preserved across
        // rebuilds (see its own doc comment), so without this it would keep
        // rendering against the *previous* camera's projection/view forever.
        // See LabelPicker::updateCamera()'s doc comment.
        if (needsNewCamera) labelPicker_->updateCamera(camera_);
        labelPicker_->updateScene(builder.labelGraph());
    }
    balloonController_ = giv::BalloonController::create(&scene_, labelPicker_, balloonLabel_, window_);
    viewer_->addEventHandler(balloonController_);
    viewer_->balloonController = balloonController_;

    // Tools > Measure Distance Diagonal: recreated every rebuild (its camera_
    // ref must track a fresh camera_ on isInitialLoad), preserving whether it
    // was toggled on across the rebuild - mirrors wasBalloonEnabled above.
    bool wasMeasureEnabled = isInitialLoad ? false : measureEnabled_;
    if (!caliperFont_)
    {
        double unusedSize = -1.0;
        caliperFont_ = builder.resolveFont("Sans Bold 12", unusedSize);
    }
    caliperTool_ = giv::CaliperTool::create(camera_, options_, caliperFont_);
    // CaliperTool itself waits for the GPU (via waitForGpuIdle_ below)
    // before it ever drops the old geometry's GPU objects, so by the time
    // this runs it's already safe to compile the replacement.
    caliperTool_->onNeedsCompile = [this]() {
        try
        {
            viewer_->compile();
            viewer_->request();
        }
        catch (const vsg::Exception& e)
        {
            spdlog::error("caliper compile failed: {} (VkResult {})", e.message, e.result);
        }
    };
    caliperTool_->waitForGpuIdle = [this]() { return waitForGpuIdle(); };
    caliperTool_->onMeasurementText = [this](const std::string& text) { emit measurementChanged(QString::fromStdString(text)); };
    caliperTool_->setPixelSize(pixelSize_, pixelSizeUnit_);
    caliperTool_->setEnabled(wasMeasureEnabled);
    viewer_->addEventHandler(caliperTool_);
    measureEnabled_ = wasMeasureEnabled;

    viewer_->viewParams = builder.viewParams();
    viewer_->viewParams->setForceOpaque(forceOpaque_);
    viewer_->viewParams->setShowPixelGrid(showPixelGrid_);
    viewer_->imageFilterAnimator = builder.imageFilterAnimator();

    // loadedImages_ only ever holds the single currently-displayed image (see
    // decodeCurrentImage()), so the switch built from it has exactly one
    // child - always select it, regardless of currentImageIndex_ (which
    // indexes into loadedImageNames_, the full $image candidate list).
    imageSwitch_ = builder.imageSwitch();
    if (imageSwitch_) imageSwitch_->setSingleChildOn(0);
    applyShowImage();

    if (!isInitialLoad)
    {
        viewer_->recordAndSubmitTasks.clear();
        viewer_->presentations.clear();
    }

    // Reuse the same vsg::View across rebuilds instead of calling
    // vsg::createRenderGraphForView() (which always makes a fresh one).
    // vsg::View hands out viewIDs from a free-list, reusing a slot only once
    // every ref_ptr to the View that held it is gone - and every per-view GPU
    // resource (notably vsg::GraphicsPipeline::_implementation, indexed by
    // viewID with no bounds check in release builds) is sized against that
    // ID at compile time. Recreating the View on every Mark Browser toggle
    // meant the counter only ever grew - even the clear() above didn't help,
    // since it runs before this point, not after the old View's last
    // reference actually drops - and after enough toggles this crashed
    // inside GraphicsPipeline::vk(), indexing past the end of a vector sized
    // for a smaller viewID. Keeping one persistent View (viewID always 0)
    // sidesteps the lifetime question entirely: just swap its children.
    if (!mainView_)
    {
        mainView_ = vsg::View::create(camera_);
    }
    else
    {
        mainView_->children.clear();
    }
    // needsNewCamera reassigns camera_ to a brand-new vsg::Camera (see
    // above) - without this, that first reassignment would leave mainView_
    // still pointing at the *old* camera, so every subsequent zoom/pan/fit
    // (which all mutate the new camera_) would silently affect an orphaned
    // object nothing ever renders with. Harmless to reassign the same
    // reference on every other rebuild too.
    mainView_->camera = camera_;
    mainView_->addChild(vsg::createHeadlight());
    mainView_->addChild(sceneGraph);
    mainView_->addChild(caliperTool_->root());

    renderGraph_ = vsg::RenderGraph::create(window_->windowAdapter, mainView_);
    auto commandGraph = vsg::CommandGraph::create(window_->windowAdapter);
    commandGraph->addChild(renderGraph_);

    viewer_->assignRecordAndSubmitTaskAndPresentation({labelPicker_->commandGraph(), commandGraph});
    try
    {
        viewer_->compile();
    }
    catch (const vsg::Exception& e)
    {
        if (error) *error = QString("failed to compile scene: %1 (VkResult %2)").arg(QString::fromStdString(e.message)).arg(e.result);
        return false;
    }

    balloonEnabled_ = wasBalloonEnabled;
    balloonController_->setEnabled(wasBalloonEnabled);
    labelPicker_->setEnabled(wasBalloonEnabled);
    if (!wasBalloonEnabled) balloonLabel_->hide();

    if (isInitialLoad)
    {
        // Force the first frame to be recorded/presented right here, matching
        // the reference application's Widget3D construction order (compile() then one
        // render() call, all before the widget is ever shown - see
        // MainWindow::show() being called only after this in main.cpp).
        // Presenting into this embedded window for the first time *after*
        // it's already mapped/visible reliably hung forever inside the
        // NVIDIA driver's present() path (main thread stuck in
        // libnvidia-glcore.so's poll()); doing it once while still hidden
        // avoids that entirely.
        viewer_->request();
        try
        {
            viewer_->render();
        }
        catch (const vsg::Exception& e)
        {
            if (error) *error = QString("failed to render first frame: %1 (VkResult %2)").arg(QString::fromStdString(e.message)).arg(e.result);
            return false;
        }
    }
    else
    {
        viewer_->request();
    }

    updateScrollBars();
    return true;
}

void VulkanViewport::setDatasetsVisible(const std::vector<size_t>& indices, bool visible)
{
    if (!hasScene_) return;
    bool changed = false;
    for (size_t idx : indices)
    {
        if (idx >= scene_.datasets.size()) continue;
        if (scene_.datasets[idx].isVisible != visible)
        {
            scene_.datasets[idx].isVisible = visible;
            changed = true;
        }
    }
    if (!changed) return;
    spdlog::info("Dataset visibility changed for {} dataset(s), now {}", indices.size(), visible ? "visible" : "hidden");
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("{}", error.toStdString());
}

bool VulkanViewport::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == container_ && event->type() == QEvent::Resize) handleViewportResize();
    if (watched == window_)
    {
        switch (event->type())
        {
        case QEvent::Wheel: {
            auto* we = static_cast<QWheelEvent*>(event);
            if (we->modifiers() & Qt::AltModifier)
            {
                QPoint d = we->angleDelta();
                int amount = d.y() != 0 ? d.y() : d.x();
                if (amount != 0) scaleQuivers(amount > 0 ? 1.2 : 0.8);
                return true;
            }
            if (handleSwipe(we)) return true;
            break;
        }
        case QEvent::MouseButtonDblClick: {
            // Left double-click toggles 1:1 / fit. Swallowed so vsgQt doesn't
            // turn it into a second press; not while measuring, where the
            // left button belongs to the caliper.
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::MiddleButton && me->modifiers() == Qt::NoModifier && hasScene_)
            {
                // Middle double-click toggles the same way (a middle drag pans).
                toggleActualFit(me->position() * window_->devicePixelRatio());
                return true;
            }
            if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier && hasScene_ && !measureEnabled_)
            {
                toggleActualFit(me->position() * window_->devicePixelRatio());
                return true;
            }
            break;
        }
        case QEvent::NativeGesture: {
            // Touchpad pinch (macOS, Windows precision touchpads, Wayland; on
            // X11 only if the Qt/Xorg build delivers gestures).
            auto* ge = static_cast<QNativeGestureEvent*>(event);
            if (ge->gestureType() == Qt::BeginNativeGesture || ge->gestureType() == Qt::EndNativeGesture)
                spdlog::info("Native gesture {}", ge->gestureType() == Qt::BeginNativeGesture ? "begin" : "end");
            if (ge->gestureType() == Qt::ZoomNativeGesture && panZoom_ && window_->windowAdapter)
            {
                // value() is the relative change of scale; the projection's
                // half-extents scale inversely.
                const double factor = std::clamp(1.0 / (1.0 + ge->value()), 0.5, 2.0);
                const QPointF p = ge->position() * window_->devicePixelRatio();
                panZoom_->zoomAbout(factor, static_cast<int32_t>(p.x()), static_cast<int32_t>(p.y()),
                                    window_->windowAdapter->extent2D(), /*animate=*/false);
                viewer_->request();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// A horizontal two-finger touchpad swipe navigates images; vertical scrolls
// stay with the wheel zoom. Only scrolls that look like a touchpad's (they
// carry a scroll phase or pixel deltas - wheel steps carry neither) count.
// One swipe, momentum included, navigates once: events less than 250 ms
// apart belong to the same swipe, and all of them are swallowed so a stray
// vertical component doesn't zoom.
bool VulkanViewport::handleSwipe(QWheelEvent* we)
{
    const bool touchpad = we->phase() != Qt::NoScrollPhase || !we->pixelDelta().isNull();
    if (!touchpad) return false;
    const QPoint d = !we->pixelDelta().isNull() ? we->pixelDelta() : we->angleDelta();

    const bool inSwipe = swipeClock_.isValid() && swipeClock_.elapsed() < 250 && we->phase() != Qt::ScrollBegin;
    if (!inSwipe && std::abs(d.x()) <= 2 * std::abs(d.y())) return false;

    if (!inSwipe)
    {
        swipeAccum_ = 0.0;
        swipeFired_ = false;
    }
    swipeClock_.restart();
    swipeAccum_ += d.x();
    constexpr double kThreshold = 150.0;
    if (!swipeFired_ && std::abs(swipeAccum_) >= kThreshold)
    {
        swipeFired_ = true;
        emit swipeNavigate(swipeAccum_ < 0 ? 1 : -1);
    }
    return true;
}

void VulkanViewport::scaleQuivers(double factor)
{
    if (!hasScene_) return;
    for (auto& ds : scene_.datasets) ds.quiverScale *= factor;
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("{}", error.toStdString());
}

void VulkanViewport::setShowMarks(bool show)
{
    if (globalShowMarks_ == show) return;
    globalShowMarks_ = show;
    spdlog::info("Show Marks set to {}", show);
    if (!hasScene_) return;
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("{}", error.toStdString());
}

void VulkanViewport::toggleShowMarks()
{
    setShowMarks(!globalShowMarks_);
}

std::string VulkanViewport::currentImageName() const
{
    if (currentImageIndex_ < 0 || currentImageIndex_ >= static_cast<int>(loadedImageNames_.size())) return {};
    return loadedImageNames_[currentImageIndex_];
}

QString VulkanViewport::pixelValueText(double worldX, double worldY) const
{
    if (loadedImages_.empty() || displayImages_.empty() || !currentImageSize_) return {};
    const giv::LoadedImage& raw = loadedImages_.front();
    const giv::LoadedImage& img = displayImages_.front(); // what's actually on screen (post contrast/colormap)

    // worldX/worldY here are exactly what PanZoomHandler::apply(MoveEvent&)
    // hands to onCursorMove: worldX is the raw (y-up) VSG world X, but
    // worldY is already negated back to y-down there - and since
    // SceneBuilder places the image's top-left at world (0,0) extending to
    // (w,-h), both worldX and worldY are already image pixel coords as-is.
    int px = static_cast<int>(std::floor(worldX));
    int py = static_cast<int>(std::floor(worldY));
    if (px < 0 || py < 0 || px >= img.width || py >= img.height) return {};

    const size_t sampleIdx = static_cast<size_t>(py) * img.width + px;
    const uint8_t* p = &img.rgba[sampleIdx * 4]; // img (displayImages_) already holds only the current slice's pixels
    uint8_t r = p[0], g = p[1], b = p[2], a = p[3];

    if (raw.sampleType != WIV_SAMPLE_NONE)
    {
        // raw.samples holds every slice - offset into the currently-selected one.
        const size_t rawIdx = static_cast<size_t>(raw.width) * raw.height * currentSlice_ + sampleIdx;
        double native;
        switch (raw.sampleType)
        {
            case WIV_SAMPLE_U8: native = raw.samples[rawIdx]; break;
            case WIV_SAMPLE_U16: native = reinterpret_cast<const uint16_t*>(raw.samples.data())[rawIdx]; break;
            case WIV_SAMPLE_FLOAT: native = reinterpret_cast<const float*>(raw.samples.data())[rawIdx]; break;
            default: native = 0.0; break;
        }
        QString hex = QString("#%1%2%3")
                          .arg(r, 2, 16, QChar('0'))
                          .arg(g, 2, 16, QChar('0'))
                          .arg(b, 2, 16, QChar('0'))
                          .toUpper();
        return QString(" [%1] = %2").arg(native).arg(hex);
    }

    if (r == g && g == b)
        return QString(" [%1] = #%2").arg(r).arg(r, 2, 16, QChar('0')).toUpper();

    QString hex = QString("#%1%2%3")
                      .arg(r, 2, 16, QChar('0'))
                      .arg(g, 2, 16, QChar('0'))
                      .arg(b, 2, 16, QChar('0'))
                      .toUpper();
    if (a != 255)
        return QString(" [%1 %2 %3 %4] = %5%6")
            .arg(r, 3)
            .arg(g, 3)
            .arg(b, 3)
            .arg(a, 3)
            .arg(hex)
            .arg(QString("%1").arg(a, 2, 16, QChar('0')).toUpper());
    return QString(" [%1 %2 %3] = %4").arg(r, 3).arg(g, 3).arg(b, 3).arg(hex);
}

void VulkanViewport::nextImage()
{
    if (loadedImageNames_.size() < 2) return;
    switchToImage((currentImageIndex_ + 1) % static_cast<int>(loadedImageNames_.size()));
}

void VulkanViewport::previousImage()
{
    if (loadedImageNames_.size() < 2) return;
    switchToImage((currentImageIndex_ - 1 + static_cast<int>(loadedImageNames_.size())) % static_cast<int>(loadedImageNames_.size()));
}

void VulkanViewport::firstImage()
{
    if (loadedImageNames_.size() < 2 || currentImageIndex_ == 0) return;
    switchToImage(0);
}

void VulkanViewport::lastImage()
{
    const int last = static_cast<int>(loadedImageNames_.size()) - 1;
    if (last < 1 || currentImageIndex_ == last) return;
    switchToImage(last);
}

void VulkanViewport::panByFraction(double fx, double fy)
{
    if (!panZoom_ || !hasScene_ || !window_ || !window_->windowAdapter) return;
    const auto extent = window_->windowAdapter->extent2D();
    if (extent.width == 0 || extent.height == 0) return;
    cancelViewAnimation();
    panZoom_->cancelZoom();
    // pan() takes the drag a mouse would make: the content follows it, so
    // moving the view right is a drag to the left.
    panZoom_->panByPixels(static_cast<int32_t>(std::lround(-fx * extent.width)),
                          static_cast<int32_t>(std::lround(-fy * extent.height)), extent);
    viewDirty_ = true;
    viewer_->request();
    updateScrollBars();
}

// Shows page `page` of the current file. The view is left alone when the new
// page is the same size as the old one (the usual multi-page TIFF), so the
// user's zoom and pan carry across pages; a page of another size is re-fitted.
void VulkanViewport::gotoPage(int page)
{
    if (page < 0 || page >= pageCount() || page == currentPage_) return;
    const auto oldSize = currentImageSize_;
    if (!decodeImageAt(currentImageIndex_, page))
    {
        spdlog::error("Failed to decode page {} of {}", page + 1, currentImageName());
        return;
    }
    spdlog::info("Switched to page {}/{}", page + 1, pageCount());
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
    {
        spdlog::error("{}", error.toStdString());
        return;
    }
    if (oldSize != currentImageSize_)
        fitContentToWindow();
    else
        updateScrollBars();
    viewer_->request();
}

bool VulkanViewport::canStepPart(int dir) const
{
    if (pageCount() > 1) return dir > 0 ? currentPage_ + 1 < pageCount() : currentPage_ > 0;
    return sliceCount() > 1 || imageCount() > 1;
}

void VulkanViewport::stepPart(int dir)
{
    if (pageCount() > 1)
        gotoPage(currentPage_ + dir);
    else if (sliceCount() > 1)
        dir > 0 ? nextSlice() : previousSlice();
    else if (imageCount() > 1)
        dir > 0 ? nextImage() : previousImage();
}

void VulkanViewport::nextSlice()
{
    const int count = sliceCount();
    if (count < 2) return;
    currentSlice_ = (currentSlice_ + 1) % count;
    recomputeDisplayImage();
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("Failed to switch slice: {}", error.toStdString());
    emit sliceChanged(currentSlice_, count);
}

void VulkanViewport::previousSlice()
{
    const int count = sliceCount();
    if (count < 2) return;
    currentSlice_ = (currentSlice_ - 1 + count) % count;
    recomputeDisplayImage();
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("Failed to switch slice: {}", error.toStdString());
    emit sliceChanged(currentSlice_, count);
}

void VulkanViewport::cachePrefetchedImage(const std::string& path, giv::LoadedImage image)
{
    imageCache_.put(path, 0, std::move(image));
}

bool VulkanViewport::decodeImageAt(int index, int page)
{
    if (index < 0 || index >= static_cast<int>(loadedImageNames_.size())) return false;
    const giv::LoadedImage* img = imageCache_.get(loadedImageNames_[static_cast<size_t>(index)], page);
    if (!img) return false;
    currentImageIndex_ = index;
    currentPage_ = page;
    currentSlice_ = 0;
    loadedImages_ = {*img};
    currentImageSize_ = std::make_pair(static_cast<double>(img->width), static_cast<double>(img->height));
    recomputeDisplayImage();
    emit sliceChanged(currentSlice_, sliceCount());
    emit pageChanged(currentPage_, pageCount());
    return true;
}

void VulkanViewport::recomputeDisplayImage()
{
    if (loadedImages_.empty())
    {
        displayImages_.clear();
        return;
    }

    const giv::LoadedImage& src = loadedImages_.front();
    giv::LoadedImage display = src;
    if (src.sampleType != WIV_SAMPLE_NONE)
    {
        const ContrastState state = currentContrastState();
        giv::renderDisplayRgba(src, state.min, state.max, state.colormapId, state.colormapEnabled, display.rgba,
                                currentSlice_);
    }
    displayImages_ = {std::move(display)};
}

VulkanViewport::ContrastState VulkanViewport::currentContrastState()
{
    static const ContrastState kEmpty{};
    if (loadedImages_.empty()) return kEmpty;

    const giv::LoadedImage& src = loadedImages_.front();
    const std::string path = currentImageName();
    auto it = contrastState_.find(path);
    if (it == contrastState_.end())
    {
        ContrastState defaultState;
        // With Auto Contrast off (giv's do_auto_contrast), a new image keeps
        // the window last dialed in instead of stretching to its own range.
        defaultState.min = (!autoContrast_ && hasLastContrast_) ? lastContrastMin_ : src.sampleMin;
        defaultState.max = (!autoContrast_ && hasLastContrast_) ? lastContrastMax_ : src.sampleMax;
        defaultState.colormapId = lastColormapId_;
        defaultState.colormapEnabled = lastColormapEnabled_;
        it = contrastState_.emplace(path, defaultState).first;
    }
    return it->second;
}

void VulkanViewport::setContrast(float min, float max)
{
    if (loadedImages_.empty() || loadedImages_.front().sampleType == WIV_SAMPLE_NONE) return;

    ContrastState state = currentContrastState();
    state.min = min;
    state.max = max;
    contrastState_[currentImageName()] = state;
    lastContrastMin_ = min;
    lastContrastMax_ = max;
    hasLastContrast_ = true;

    recomputeDisplayImage();
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("Failed to apply contrast: {}", error.toStdString());
    emit contrastChanged();
}

void VulkanViewport::setColormap(giv::colormaps::Id id, bool enabled)
{
    if (loadedImages_.empty() || loadedImages_.front().sampleType == WIV_SAMPLE_NONE) return;

    ContrastState state = currentContrastState();
    state.colormapId = id;
    state.colormapEnabled = enabled;
    contrastState_[currentImageName()] = state;
    lastColormapId_ = id;
    lastColormapEnabled_ = enabled;

    recomputeDisplayImage();
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        spdlog::error("Failed to apply color table: {}", error.toStdString());
    emit contrastChanged();
}

std::array<uint32_t, 256> VulkanViewport::computeHistogram() const
{
    std::array<uint32_t, 256> histogram{};
    if (loadedImages_.empty()) return histogram;

    const giv::LoadedImage& img = loadedImages_.front();
    if (img.sampleType == WIV_SAMPLE_NONE) return histogram;

    const float range = img.sampleMax - img.sampleMin;
    const size_t pixelCount = static_cast<size_t>(img.width) * img.height;
    const size_t sliceOffset = pixelCount * static_cast<size_t>(currentSlice_);
    for (size_t i = sliceOffset; i < sliceOffset + pixelCount; ++i)
    {
        double v;
        switch (img.sampleType)
        {
            case WIV_SAMPLE_U8: v = img.samples[i]; break;
            case WIV_SAMPLE_U16: v = reinterpret_cast<const uint16_t*>(img.samples.data())[i]; break;
            case WIV_SAMPLE_FLOAT: v = reinterpret_cast<const float*>(img.samples.data())[i]; break;
            default: v = 0.0; break;
        }
        int idx = range != 0.0f ? static_cast<int>(std::floor((v - img.sampleMin) / range * 255.0)) : 0;
        if (idx >= 0 && idx < 256) ++histogram[static_cast<size_t>(idx)];
    }
    return histogram;
}

void VulkanViewport::decodeCurrentImage()
{
    // Best-effort: try currentImageIndex_ first, then scan forward (wrapping
    // once through the whole list) so a corrupt/unreadable file doesn't
    // block startup - mirrors the old eager-load behavior of simply omitting
    // any $image reference that failed to decode, but lazily (only actually
    // decoding as many candidates as it takes to find one that works).
    size_t n = loadedImageNames_.size();
    for (size_t attempt = 0; attempt < n; ++attempt)
    {
        int idx = static_cast<int>((static_cast<size_t>(currentImageIndex_) + attempt) % n);
        if (decodeImageAt(idx)) return;
    }
    loadedImages_.clear();
    displayImages_.clear();
    currentImageSize_.reset();
}

// Shared tail of nextImage()/previousImage(): decodes `index` (via
// imageCache_, evicting the least-recently-used entry if it's a miss),
// rebuilds the scene graph so the new image's texture actually replaces the
// old one (only one is ever GPU-resident at a time - see loadedImages_'s
// doc comment), then - if autoFit_ (giv's do_auto_fit_marks, default on) -
// re-fits the view (whole image visible, matching fitToWindow()); otherwise
// the current zoom/pan is left untouched, only now framing the new image's
// content instead.
void VulkanViewport::switchToImage(int index)
{
    saveCurrentView(); // the view of the image being left
    if (!decodeImageAt(index))
    {
        spdlog::error("Failed to decode {}", loadedImageNames_[static_cast<size_t>(index)]);
        return;
    }
    spdlog::info("Switched to image {}/{}: {}", index + 1, loadedImageNames_.size(), loadedImageNames_[static_cast<size_t>(index)]);
    QString error;
    deferScrollBarUpdate_ = autoFit_;
    bool rebuilt = rebuildSceneGraph(&error, /*isInitialLoad=*/false);
    deferScrollBarUpdate_ = false;
    if (!rebuilt)
    {
        spdlog::error("{}", error.toStdString());
        updateScrollBars();
        return;
    }
    if (tryRestoreView())
    {
        // back to where the user left this image
    }
    else if (autoFit_)
    {
        fitContentToWindow();
    }
    else
    {
        updateScrollBars();
    }
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
}

void VulkanViewport::syncRenderExtent(VkExtent2D extent)
{
    if (camera_ && camera_->viewportState) camera_->viewportState->set(0, 0, extent.width, extent.height);
    if (!renderGraph_) return;

    // vsg::RenderGraph::accept() compares getExtent() (the window's *actual
    // current* extent, read fresh every frame) against previous_extent (a
    // baseline frozen at whatever extent existed when renderGraph_ was
    // constructed - see rebuildSceneGraph()) and, if they differ, calls
    // resized(), which *proportionally rescales* the existing renderArea by
    // the (possibly non-uniform, per-axis) ratio between those two extents -
    // entirely independent of camera_->viewportState above. Snapping both
    // renderArea and previous_extent directly to the real, current extent
    // here - rather than leaving that stale baseline for accept() to rescale
    // from - is what actually keeps the render area's aspect ratio correct;
    // this is the mechanism that was producing a non-isotropic stretch no
    // amount of camera-side fixing could touch.
    renderGraph_->renderArea.offset = {0, 0};
    renderGraph_->renderArea.extent = extent;
    renderGraph_->previous_extent = extent;
    renderGraph_->viewportState->set(0, 0, extent.width, extent.height);
}

namespace
{
// "Contain": the scale (device pixels per world unit) at which data of size
// dataW x dataH just fits a canvasW x canvasH canvas.
double containScale(double canvasW, double canvasH, double dataW, double dataH)
{
    double scale = std::min(canvasW / dataW, canvasH / dataH);
    if (scale <= 0.0) scale = std::min(canvasW, canvasH) / std::max(dataW, dataH);
    return scale;
}
} // namespace

void VulkanViewport::fitToBounds(double minX, double minY, double maxX, double maxY)
{
    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    if (!projection_ || !window_ || !window_->windowAdapter) return;

    double centerX = (minX + maxX) * 0.5;
    double centerY = (minY + maxY) * 0.5;

    double dataW = std::max(maxX - minX, 2e-3);
    double dataH = std::max(maxY - minY, 2e-3);
    rotatedSize(dataW, dataH);
    auto extent = window_->windowAdapter->extent2D();
    double canvasW = static_cast<double>(extent.width);
    double canvasH = static_cast<double>(extent.height);

    syncRenderExtent(extent);

    // "Contain": scale to the smaller of scaleX/scaleY so the whole content
    // is visible, possibly with letterbox margins on one axis.
    double scale = containScale(canvasW, canvasH, dataW, dataH);

    double halfW = canvasW / (2.0 * scale);
    double halfH = canvasH / (2.0 * scale);

    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    if (lookAt)
    {
        lookAt->eye = vsg::dvec3(centerX, centerY, 1.0);
        lookAt->center = vsg::dvec3(centerX, centerY, 0.0);
    }
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;

    lastWidth_ = static_cast<int>(std::lround(canvasW));
    lastHeight_ = static_cast<int>(std::lround(canvasH));

    viewDirty_ = false; // a fit is the default view, nothing to remember
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::fitToWindow()
{
    spdlog::info("Fit to Window");
    fitContentToWindow();
}

// The view fitContentToWindow() would produce right now, without applying it
// (same maths as fitToBounds()).
bool VulkanViewport::computeFitView(FitView& out) const
{
    if (!hasScene_ || !window_ || !window_->windowAdapter) return false;
    double minX, minY, maxX, maxY;
    currentFitBoundsYDown(minX, minY, maxX, maxY);
    // fitToBounds()'s arguments: (minX, -maxY, maxX, -minY).
    const double bMinY = -maxY, bMaxY = -minY;
    const auto extent = window_->windowAdapter->extent2D();
    if (extent.width == 0 || extent.height == 0) return false;
    double dataW = std::max(maxX - minX, 2e-3), dataH = std::max(bMaxY - bMinY, 2e-3);
    rotatedSize(dataW, dataH);
    const double scale = containScale(extent.width, extent.height, dataW, dataH);
    out = {(minX + maxX) * 0.5, (bMinY + bMaxY) * 0.5, extent.width / (2.0 * scale), extent.height / (2.0 * scale)};
    return true;
}

void VulkanViewport::fitToWindowAnimated()
{
    spdlog::info("Fit to Window (animated)");
    FitView target;
    if (!computeFitView(target))
    {
        fitContentToWindow();
        return;
    }
    startViewAnimation(target);
}

// Glides the camera to `to`: zoom is interpolated geometrically (a constant
// ratio per unit time looks uniform), and the centre so that the world point
// that stays fixed on screen is the one a wheel zoom to the same scale would
// keep fixed - the move then reads as one zoom, not a zoom plus a slide.
// Ends by snapping to the exact fit, which also clears viewDirty_.
void VulkanViewport::startViewAnimation(const FitView& to)
{
    auto lookAt = camera_ ? camera_->viewMatrix.cast<vsg::LookAt>() : vsg::ref_ptr<vsg::LookAt>();
    const double duration = panZoom_ ? panZoom_->zoomDuration() * 1.5 : 0.0;
    if (!lookAt || !projection_ || duration <= 0.0)
    {
        fitContentToWindow();
        return;
    }
    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    viewAnim_.from = {lookAt->center.x, lookAt->center.y, (projection_->right - projection_->left) * 0.5,
                      (projection_->top - projection_->bottom) * 0.5};
    viewAnim_.to = to;
    viewAnim_.duration = duration;
    viewAnim_.clock.start();
    viewAnim_.active = true;
    if (!viewAnimTimer_)
    {
        viewAnimTimer_ = new QTimer(this);
        viewAnimTimer_->setInterval(8);
        connect(viewAnimTimer_, &QTimer::timeout, this, &VulkanViewport::tickViewAnimation);
    }
    viewAnimTimer_->start();
}

void VulkanViewport::tickViewAnimation()
{
    auto lookAt = camera_ ? camera_->viewMatrix.cast<vsg::LookAt>() : vsg::ref_ptr<vsg::LookAt>();
    if (!viewAnim_.active || !lookAt || !projection_)
    {
        cancelViewAnimation();
        return;
    }
    const double u = std::min(viewAnim_.clock.elapsed() / 1000.0 / viewAnim_.duration, 1.0);
    if (u >= 1.0)
    {
        cancelViewAnimation();
        fitContentToWindow();
        return;
    }
    const double e = u * u * (3.0 - 2.0 * u); // smoothstep: eases in and out
    const FitView &a = viewAnim_.from, &b = viewAnim_.to;
    const double halfW = a.halfW * std::pow(b.halfW / a.halfW, e);
    const double halfH = a.halfH * std::pow(b.halfH / a.halfH, e);
    const double dHalf = a.halfW - b.halfW;
    const double w = std::abs(dHalf) > 1e-9 * a.halfW ? (a.halfW - halfW) / dHalf : e;
    const double cx = a.centerX + (b.centerX - a.centerX) * w;
    const double cy = a.centerY + (b.centerY - a.centerY) * w;
    lookAt->eye = vsg::dvec3(cx, cy, 1.0);
    lookAt->center = vsg::dvec3(cx, cy, 0.0);
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::cancelViewAnimation()
{
    viewAnim_.active = false;
    if (viewAnimTimer_) viewAnimTimer_->stop();
}

// window_->resize() (called from resizeEvent() to keep the embedded native
// window in sync with its container) only *posts* that resize: the platform
// plugin round-trips it (an X11 ConfigureRequest/ConfigureNotify exchange)
// before window_->windowAdapter->extent2D() actually reflects it - observed
// to still read the old, larger, default-traits size for a little while
// after resizeEvent() has already returned. Calling fitToWindow() at the
// wrong moment doesn't just under/over-size the fit (which self-corrects
// once the projection is later redone against a matching extent) - it can
// leave camera_->viewportState and the projection built from *different*
// extents, which is a non-isotropic stretch, not just a wrong scale. So
// callers doing the very first fit right after the window is shown (see
// MainWindow::showEvent()) should call this first, to actually wait for that
// round trip rather than guessing at the timing.
void VulkanViewport::ensureExtentSettled()
{
    if (!window_ || !window_->windowAdapter) return;
    double wantW = static_cast<double>(container_->width()) * devicePixelRatioF();
    double wantH = static_cast<double>(container_->height()) * devicePixelRatioF();
    if (wantW <= 0.0 || wantH <= 0.0) return;

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        auto extent = window_->windowAdapter->extent2D();
        if (std::abs(static_cast<double>(extent.width) - wantW) < 1.5 &&
            std::abs(static_cast<double>(extent.height) - wantH) < 1.5)
            return;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QThread::msleep(5);
    }
    // Gave up after ~0.5s: proceed with whatever extent2D() reports rather
    // than hang forever - better a possibly-wrong fit than a frozen window.
}

void VulkanViewport::fitContentToWindow()
{
    if (!hasScene_) return;
    double minXyd, minYyd, maxXyd, maxYyd;
    currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);
    fitToBounds(minXyd, -maxYyd, maxXyd, -minYyd);
}

void VulkanViewport::currentFitBoundsYDown(double& minX, double& minY, double& maxX, double& maxY) const
{
    bool has = scene_.hasBounds();
    minX = has ? scene_.minX : 1e30;
    maxX = has ? scene_.maxX : -1e30;
    minY = has ? scene_.minY : 1e30;
    maxY = has ? scene_.maxY : -1e30;

    if (currentImageSize_)
    {
        minX = std::min(minX, 0.0);
        maxX = std::max(maxX, currentImageSize_->first);
        minY = std::min(minY, 0.0);
        maxY = std::max(maxY, currentImageSize_->second);
    }

    if (!scene_.textExtents.empty() && window_ && window_->windowAdapter && minX <= maxX && minY <= maxY)
    {
        // Text labels are pixel-sized, so their world extent depends on the
        // zoom, which in turn depends on the bounds: iterate to a fixed point.
        const auto extent = window_->windowAdapter->extent2D();
        if (extent.width > 0 && extent.height > 0)
        {
            const double bMinX = minX, bMaxX = maxX, bMinY = minY, bMaxY = maxY;
            for (int iter = 0; iter < 8; ++iter)
            {
                double w = std::max(maxX - minX, 2e-3), h = std::max(maxY - minY, 2e-3);
                rotatedSize(w, h);
                const double wpp = 1.0 / containScale(extent.width, extent.height, w, h);
                minX = bMinX; maxX = bMaxX; minY = bMinY; maxY = bMaxY;
                for (const auto& t : scene_.textExtents)
                {
                    const double k = t.scaled ? 1.0 : wpp;
                    minX = std::min(minX, t.x + t.l * k);
                    maxX = std::max(maxX, t.x + t.r * k);
                    // y-up offsets b..t -> data (y-down) range y-t..y-b
                    minY = std::min(minY, t.y - t.t * k);
                    maxY = std::max(maxY, t.y - t.b * k);
                }
            }
        }
    }

    if (minX > maxX || minY > maxY)
    {
        // Nothing loaded (no datasets, no image) - an empty unit-square
        // canvas, not an arbitrary large box, so "no file" mode reads as an
        // empty 0->1 canvas rather than a mysteriously oversized fit.
        minX = 0.0;
        maxX = 1.0;
        minY = 0.0;
        maxY = 1.0;
    }
}

void VulkanViewport::updateScrollBars()
{
    if (deferScrollBarUpdate_) return;
    // Every zoom/pan/resize/fit path in this file ends by calling
    // updateScrollBars(), so this is the one hook that keeps the caliper's
    // on-screen size pixel-constant even when the view changes without the
    // mouse moving (e.g. the Zoom In/Out menu, scroll-wheel zoom, a window
    // resize) - see CaliperTool::refresh().
    if (caliperTool_) caliperTool_->refresh();

    if (!hScrollBar_ || !vScrollBar_) return;
    if (!hasScene_ || !camera_ || !projection_ || !window_ || !window_->windowAdapter || quarterTurns_ != 0)
    {
        hScrollBar_->setEnabled(false);
        vScrollBar_->setEnabled(false);
        hScrollBar_->hide();
        vScrollBar_->hide();
        return;
    }
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    auto extent = window_->windowAdapter->extent2D();
    if (!lookAt || extent.width == 0 || extent.height == 0) return;

    double minXyd, minYyd, maxXyd, maxYyd;
    currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);
    // World-space content bounds: X unchanged, Y negated (see fitToBounds()'s
    // own call: fitToBounds(minXyd, -maxYyd, maxXyd, -minYyd)).
    double contentMaxY = -minYyd;

    double halfW = (projection_->right - projection_->left) * 0.5;
    double halfH = (projection_->top - projection_->bottom) * 0.5;
    double scaleX = extent.width / (2.0 * halfW);
    double scaleY = extent.height / (2.0 * halfH);

    double contentWidthPx = (maxXyd - minXyd) * scaleX;
    double viewLeftPx = flipHorizontal_ ? (maxXyd - (lookAt->center.x + halfW)) * scaleX
                                        : (lookAt->center.x - halfW - minXyd) * scaleX;
    applyScrollBarRange(hScrollBar_, contentWidthPx, static_cast<double>(extent.width), viewLeftPx);

    double contentHeightPx = (maxYyd - minYyd) * scaleY;
    double viewTopPx = flipVertical_ ? (lookAt->center.y - halfH + maxYyd) * scaleY
                                     : (contentMaxY - (lookAt->center.y + halfH)) * scaleY;
    applyScrollBarRange(vScrollBar_, contentHeightPx, static_cast<double>(extent.height), viewTopPx);
}

void VulkanViewport::zoomIn()
{
    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    viewDirty_ = true;
    spdlog::info("Zoom In");
    if (!projection_) return;
    double factor = 0.8;
    double halfW = (projection_->right - projection_->left) * 0.5 * factor;
    double halfH = (projection_->top - projection_->bottom) * 0.5 * factor;
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::zoomOut()
{
    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    viewDirty_ = true;
    spdlog::info("Zoom Out");
    if (!projection_) return;
    double factor = 1.25;
    double halfW = (projection_->right - projection_->left) * 0.5 * factor;
    double halfH = (projection_->top - projection_->bottom) * 0.5 * factor;
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::zoomActualPixels()
{
    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    viewDirty_ = true;
    spdlog::info("Zoom 1:1");
    if (!projection_ || !window_ || !window_->windowAdapter) return;
    auto extent = window_->windowAdapter->extent2D();
    if (extent.width == 0 || extent.height == 0) return;
    // One world unit == one image pixel, so half the extent in device pixels.
    double halfW = extent.width * 0.5;
    double halfH = extent.height * 0.5;
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    viewer_->request();
    updateScrollBars();
}

double VulkanViewport::viewScale() const
{
    if (!projection_ || !window_ || !window_->windowAdapter) return 0.0;
    const double width = projection_->right - projection_->left;
    return width > 0.0 ? window_->windowAdapter->extent2D().width / width : 0.0;
}

double VulkanViewport::fitScale() const
{
    if (!window_ || !window_->windowAdapter) return 0.0;
    double minX, minY, maxX, maxY;
    currentFitBoundsYDown(minX, minY, maxX, maxY);
    const auto extent = window_->windowAdapter->extent2D();
    double dataW = std::max(maxX - minX, 2e-3), dataH = std::max(maxY - minY, 2e-3);
    rotatedSize(dataW, dataH);
    return containScale(extent.width, extent.height, dataW, dataH);
}

void VulkanViewport::stepZoomIn() { stepZoom(true); }
void VulkanViewport::stepZoomOut() { stepZoom(false); }

void VulkanViewport::stepZoom(bool in)
{
    if (!hasScene_ || !panZoom_ || !window_ || !window_->windowAdapter) return;
    const auto extent = window_->windowAdapter->extent2D();
    const double current = viewScale();
    const double fit = fitScale();
    if (extent.width == 0 || extent.height == 0 || current <= 0.0 || fit <= 0.0) return;

    constexpr double kEps = 1e-3; // relative: "at" a stop
    std::vector<double> stops{fit, 1.0, 4.0};
    std::sort(stops.begin(), stops.end());
    stops.erase(std::unique(stops.begin(), stops.end(),
                            [](double a, double b) { return std::abs(a - b) <= kEps * std::max(a, b); }),
                stops.end());

    double target = 0.0;
    if (in)
    {
        for (double s : stops)
            if (s > current * (1.0 + kEps)) { target = s; break; }
    }
    else
    {
        for (auto it = stops.rbegin(); it != stops.rend(); ++it)
            if (*it < current * (1.0 - kEps)) { target = *it; break; }
    }
    if (target <= 0.0) return; // already at the outermost stop
    spdlog::info("Step zoom {} to {:.3f}", in ? "in" : "out", target);

    if (std::abs(target - fit) <= kEps * fit)
    {
        fitToWindowAnimated(); // also re-centres, which a plain zoom wouldn't
        return;
    }
    cancelViewAnimation();
    viewDirty_ = true;
    panZoom_->zoomAbout(current / target, static_cast<int32_t>(extent.width / 2), static_cast<int32_t>(extent.height / 2),
                        extent, /*animate=*/true, /*replaceMotion=*/true);
}

void VulkanViewport::toggleActualFit(QPointF devicePos)
{
    if (!hasScene_ || !panZoom_ || !window_ || !window_->windowAdapter) return;
    const double current = viewScale();
    if (current <= 0.0) return;
    if (std::abs(current - 1.0) < 1e-3)
    {
        fitToWindowAnimated();
        return;
    }
    cancelViewAnimation();
    viewDirty_ = true;
    panZoom_->zoomAbout(current, static_cast<int32_t>(devicePos.x()), static_cast<int32_t>(devicePos.y()),
                        window_->windowAdapter->extent2D(), /*animate=*/true, /*replaceMotion=*/true);
}

void VulkanViewport::setRememberViewPerFile(bool enable)
{
    rememberView_ = enable;
    spdlog::info("Remember View Per File set to {}", enable);
    if (!enable) savedViews_.clear();
}

std::string VulkanViewport::viewKey() const
{
    std::string name = currentImageName();
    return name.empty() ? viewKeyFallback_ : name;
}

// Remembers the displayed file's view if the user changed it (a fit view is
// the default and not worth keeping - and would be wrong after a resize).
void VulkanViewport::saveCurrentView()
{
    if (!rememberView_ || !hasScene_ || !camera_ || !projection_ || !window_ || !window_->windowAdapter) return;
    const std::string key = viewKey();
    if (key.empty()) return;
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    const auto extent = window_->windowAdapter->extent2D();
    const double width = projection_->right - projection_->left;
    const double height = projection_->top - projection_->bottom;
    if (!viewDirty_ || !lookAt || extent.width == 0 || extent.height == 0 || width <= 0.0 || height <= 0.0)
    {
        savedViews_.erase(key);
        return;
    }
    savedViews_[key] = {lookAt->center.x, lookAt->center.y, extent.width / width, extent.height / height};
}

// Puts back the view remembered for the file now displayed, if any. The
// scale is stored in device pixels per world unit, so the zoom survives a
// window resize since. Mirrors the tail of fitToBounds().
bool VulkanViewport::tryRestoreView()
{
    if (!rememberView_ || !camera_ || !projection_ || !window_ || !window_->windowAdapter) return false;
    auto it = savedViews_.find(viewKey());
    if (it == savedViews_.end()) return false;
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    const auto extent = window_->windowAdapter->extent2D();
    if (!lookAt || extent.width == 0 || extent.height == 0) return false;
    const SavedView& v = it->second;

    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    syncRenderExtent(extent);
    const double halfW = extent.width / (2.0 * v.scaleX);
    const double halfH = extent.height / (2.0 * v.scaleY);
    lookAt->eye = vsg::dvec3(v.centerX, v.centerY, 1.0);
    lookAt->center = vsg::dvec3(v.centerX, v.centerY, 0.0);
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    lastWidth_ = static_cast<int>(extent.width);
    lastHeight_ = static_cast<int>(extent.height);
    viewDirty_ = true; // a restored view is a user view
    viewer_->request();
    updateScrollBars();
    spdlog::info("Restored remembered view of {}", it->first);
    return true;
}

bool VulkanViewport::applyGivString(const std::string& text, bool append, QString* error)
{
    if (!append)
    {
        scene_.datasets.clear();
        scene_.minX = 1e30;
        scene_.minY = 1e30;
        scene_.maxX = -1e30;
        scene_.maxY = -1e30;
    }

    giv::GivParser parser;
    std::string parseError;
    parser.parseString(text, scene_, parseError);

    return rebuildSceneGraph(error, /*isInitialLoad=*/false);
}

void VulkanViewport::getTransformation(double& scaleX, double& scaleY, double& shiftX, double& shiftY) const
{
    scaleX = scaleY = 1.0;
    shiftX = shiftY = 0.0;
    if (!camera_ || !projection_ || !window_ || !window_->windowAdapter) return;

    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    auto extent = window_->windowAdapter->extent2D();
    if (!lookAt || extent.width == 0 || extent.height == 0) return;

    double halfW = (projection_->right - projection_->left) * 0.5;
    double halfH = (projection_->top - projection_->bottom) * 0.5;
    if (halfW <= 0.0 || halfH <= 0.0) return;

    scaleX = extent.width / (2.0 * halfW);
    scaleY = extent.height / (2.0 * halfH);
    shiftX = lookAt->center.x;
    shiftY = lookAt->center.y;
}

void VulkanViewport::setTransformation(double scaleX, double scaleY, double shiftX, double shiftY)
{
    if (panZoom_) panZoom_->cancelZoom();
    if (!camera_ || !projection_ || !window_ || !window_->windowAdapter) return;
    if (scaleX <= 0.0 || scaleY <= 0.0) return;

    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    auto extent = window_->windowAdapter->extent2D();
    if (!lookAt || extent.width == 0 || extent.height == 0) return;

    double halfW = extent.width / (2.0 * scaleX);
    double halfH = extent.height / (2.0 * scaleY);

    lookAt->eye = vsg::dvec3(shiftX, shiftY, 1.0);
    lookAt->center = vsg::dvec3(shiftX, shiftY, 0.0);
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;

    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::toggleBalloon()
{
    if (!labelPicker_ || !balloonController_) return;
    balloonEnabled_ = !balloonEnabled_;
    spdlog::info("Balloon Tooltips set to {}", balloonEnabled_);
    balloonController_->setEnabled(balloonEnabled_);
    viewer_->request();
}

bool VulkanViewport::balloonEnabled() const
{
    return balloonEnabled_;
}

void VulkanViewport::toggleForceOpaque()
{
    forceOpaque_ = !forceOpaque_;
    spdlog::info("Force Opaque set to {}", forceOpaque_);
    if (viewer_->viewParams) viewer_->viewParams->setForceOpaque(forceOpaque_);
    viewer_->request();
}

void VulkanViewport::toggleShowPixelGrid()
{
    showPixelGrid_ = !showPixelGrid_;
    spdlog::info("Pixel Grid set to {}", showPixelGrid_);
    if (viewer_->viewParams) viewer_->viewParams->setShowPixelGrid(showPixelGrid_);
    viewer_->request();
}

void VulkanViewport::toggleMeasureDistance()
{
    measureEnabled_ = !measureEnabled_;
    spdlog::info("Measure Distance Diagonal set to {}", measureEnabled_);
    if (caliperTool_) caliperTool_->setEnabled(measureEnabled_);
    viewer_->request();
}

void VulkanViewport::setPixelCalibration(double pixelSize, const std::string& unit)
{
    pixelSize_ = pixelSize;
    pixelSizeUnit_ = unit;
    spdlog::info("Pixel calibration set to {} per pixel, unit '{}'", pixelSize_, pixelSizeUnit_);
    if (caliperTool_) caliperTool_->setPixelSize(pixelSize_, pixelSizeUnit_);
    viewer_->request();
}

void VulkanViewport::setAutoFit(bool enable)
{
    autoFit_ = enable;
    spdlog::info("Auto Fit set to {}", enable);
    if (enable) fitContentToWindow();
}

void VulkanViewport::setFlipHorizontal(bool flip)
{
    flipHorizontal_ = flip;
    spdlog::info("Horizontal Flip set to {}", flip);
    if (flipProjection_) flipProjection_->flipX = flip;
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::setFlipVertical(bool flip)
{
    flipVertical_ = flip;
    spdlog::info("Vertical Flip set to {}", flip);
    if (flipProjection_) flipProjection_->flipY = flip;
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::rotatedSize(double& w, double& h) const
{
    if (quarterTurns_ & 1) std::swap(w, h);
}

void VulkanViewport::rotateView(int quarters)
{
    quarterTurns_ = (quarterTurns_ + quarters) & 3;
    spdlog::info("Rotate view: {} degrees", quarterTurns_ * 90);
    if (flipProjection_) flipProjection_->quarterTurns = quarterTurns_;
    if (!hasScene_) return;
    // The fit changes with the orientation; a view the user has dialed in
    // just turns about its centre.
    if (!viewDirty_)
        fitContentToWindow();
    else
    {
        viewer_->request();
        updateScrollBars();
    }
}

void VulkanViewport::setVerticalZoomLock(bool lock)
{
    verticalZoomLock_ = lock;
    spdlog::info("Vertical Zoom Lock set to {}", lock);
    if (panZoom_) panZoom_->verticalLock = lock;
}

void VulkanViewport::applyShowImage()
{
    if (!imageSwitch_) return;
    if (showImage_)
        imageSwitch_->setSingleChildOn(0);
    else
        imageSwitch_->setAllChildren(false);
}

void VulkanViewport::setShowImage(bool show)
{
    showImage_ = show;
    spdlog::info("Show Image set to {}", show);
    applyShowImage();
    viewer_->request();
}

void VulkanViewport::setAutoContrast(bool enable)
{
    autoContrast_ = enable;
    spdlog::info("Auto Contrast set to {}", enable);
    // Re-stretch the displayed image to its own range, like giv reloading
    // with do_auto_contrast on.
    if (enable && !loadedImages_.empty() && loadedImages_.front().sampleType != WIV_SAMPLE_NONE)
        setContrast(loadedImages_.front().sampleMin, loadedImages_.front().sampleMax);
}

void VulkanViewport::setBackgroundColor(const QColor& color)
{
    if (!window_ || !window_->windowAdapter) return;
    spdlog::info("Background color set to #{:02x}{:02x}{:02x}", color.red(), color.green(), color.blue());
    window_->windowAdapter->clearColor() = vsg::vec4(color.redF(), color.greenF(), color.blueF(), 1.0f);
    // RenderGraph::setClearValues() only reads window_->windowAdapter->
    // clearColor() once, at RenderGraph construction time - so once a scene
    // has been built, changing the window's clearColor() alone (above) has
    // no visible effect until the next full rebuildSceneGraph(). Re-apply it
    // to the already-built renderGraph_ (if any) so Preferences > Background
    // Color takes effect immediately.
    if (renderGraph_) renderGraph_->setClearValues(window_->windowAdapter->clearColor());
    viewer_->request();
}

} // namespace givqt
