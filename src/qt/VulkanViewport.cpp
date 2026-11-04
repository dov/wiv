#include "VulkanViewport.h"
#include "ToggleCapture.h"

#include <QFileInfo>
#include <QSettings>
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

#include <QCoreApplication>
#include <QGuiApplication>
#ifdef WIV_HAVE_XCB
#include <xcb/xcb.h>
#endif
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QGridLayout>
#include <QLabel>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
        auto extent = viewportExtent();
        if (extent.width == 0) return 1.0f;
        return static_cast<float>((projection_->right - projection_->left) / static_cast<double>(extent.width));
    };

    viewer_->isResizeSettling = [this]() { return checkResizeSettling(); };
    viewer_->beforeFrame = [this]() { return onAnimationFrameHook(); };
    animTimer_ = new QTimer(this);
    animTimer_->setSingleShot(true);
    animTimer_->setTimerType(Qt::PreciseTimer);
    connect(animTimer_, &QTimer::timeout, this, &VulkanViewport::advanceAnimation);
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
    clip_ = new QWidget(this);
    clip_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    clip_->setMinimumSize(1, 1);
    container_ = QWidget::createWindowContainer(window_, clip_);
    layout->addWidget(clip_, 0, 0);
    clip_->installEventFilter(this);
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
        auto extent = viewportExtent();
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
        auto extent = viewportExtent();
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
    resetAnimation();
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

// When the render window is moved or resized, the X server keeps the old
// contents until the swapchain has been rebuilt and a new frame presented
// (tens of milliseconds - see resizeSettling_). With the default NorthWest
// bit gravity those contents travel with the window's top-left corner, so a
// dock appearing to the left of the viewport makes the stale image jump right
// by the dock's width and back again once the real frame arrives: a visible
// flicker. StaticGravity pins the old contents to the screen instead, so they
// simply stay put (partly covered) until replaced - what already happens when
// a dock appears on the right, where the window's origin does not move.
void VulkanViewport::keepContentsInPlaceOnResize()
{
#ifdef WIV_HAVE_XCB
    if (!window_ || QGuiApplication::platformName() != "xcb") return;
    auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11 || !x11->connection()) return;
    const WId id = window_->winId();
    if (id == bitGravityWindow_) return; // already set for this native window
    const uint32_t gravity = XCB_GRAVITY_STATIC;
    xcb_change_window_attributes(x11->connection(), static_cast<xcb_window_t>(id), XCB_CW_BIT_GRAVITY, &gravity);
    xcb_flush(x11->connection());
    bitGravityWindow_ = id;
    spdlog::info("Render window {:#x}: bit gravity set to static", static_cast<unsigned long>(id));
#endif
}

void VulkanViewport::handleViewportResize()
{
    keepContentsInPlaceOnResize();
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

    applyExtent(viewportExtent());
}

VkExtent2D VulkanViewport::viewportExtent() const
{
    VkExtent2D full = window_->windowAdapter->extent2D();
    if (!clip_) return full;
    const double dpr = devicePixelRatioF();
    // The size of the mask actually applied (not the live widget size), so the
    // camera changes together with what is visible; see applyMask().
    const int logicalW = visW_ > 0 ? visW_ : clip_->width();
    const int logicalH = visH_ > 0 ? visH_ : clip_->height();
    const auto visW = static_cast<uint32_t>(std::max(0L, std::lround(logicalW * dpr)));
    const auto visH = static_cast<uint32_t>(std::max(0L, std::lround(logicalH * dpr)));
    if (visW > 0 && visW < full.width) full.width = visW;
    if (visH > 0 && visH < full.height) full.height = visH;
    return full;
}

// Keeps the native render window at a fixed, screen-sized height (and the
// widget's width) so that changes of the visible height never resize it - a
// resize makes Windows show the new area black until the swapchain has been
// rebuilt and a frame presented - and masks it down to the visible area.
void VulkanViewport::layoutRenderWindow()
{
    if (!clip_ || !container_ || !window_) return;
    if (toggleCaptureEnabled()) spdlog::info("[trace] layout clip {}x{} mask {}x{} settled={}", clip_->width(), clip_->height(), maskW_, maskH_, layoutSettled_);
    // Fixed-size native window, at least as big as the biggest screen in both
    // directions, so neither layout changes nor resizing the application window
    // resize it (see clip_'s doc comment).
    int screenW = 0, screenH = 0;
    for (const QScreen* s : QGuiApplication::screens())
    {
        screenW = std::max(screenW, s->geometry().width());
        screenH = std::max(screenH, s->geometry().height());
    }
    const int w = clip_->width();
    const int h = clip_->height();
    const QRect want(0, 0, std::max(w, screenW), std::max(h, screenH));
    if (container_->geometry() != want) container_->setGeometry(want); // resizes the window only if the size changed

    // While the window is first being shown it is laid out several times in a
    // row; forcing a repaint and a synchronous frame for each of those would be
    // visible as flicker. Until it has settled just follow the layout.
    if (!layoutSettled_)
    {
        if (isVisible() && !layoutSettleTimerStarted_)
        {
            layoutSettleTimerStarted_ = true;
            QTimer::singleShot(500, this, [this]() { layoutSettled_ = true; });
        }
        visW_ = w;
        visH_ = h;
        maskW_ = w;
        maskH_ = h;
        window_->setMask(QRegion(0, 0, w, h));
        handleViewportResize();
        return;
    }
    if (w < maskW_ || h < maskH_)
    {
        // Shrinking: what gets uncovered (e.g. the thumbnail strip) is drawn by
        // Qt, which paints it a little later than this resize. Paint it first,
        // underneath the render window, and only then shrink the mask, so it is
        // revealed complete instead of after a blank gap.
        if (!maskShrinkPending_)
        {
            maskShrinkPending_ = true;
            QTimer::singleShot(0, this, [this]() {
                maskShrinkPending_ = false;
                if (QWidget* top = window()) top->repaint();
                applyMask(); // also refits the camera, so it never precedes the strip
            });
        }
        return;
    }
    applyMask();
}

void VulkanViewport::applyMask()
{
    if (!clip_ || !window_) return;
    const int w = clip_->width();
    const int h = clip_->height();
    // The order matters for the result to look like one step: first bring the
    // camera to the new visible size and present a frame at it - into the part
    // of the window that is still masked, or that stays visible - and only then
    // change the mask, so what is revealed (or kept) already holds the final
    // image instead of the previous frame followed by a jump.
    visW_ = w;
    visH_ = h;
    if (toggleCaptureEnabled()) spdlog::info("[trace] applyMask {}x{}: camera refit", w, h);
    handleViewportResize();
    if (isVisible())
    {
        viewer_->request();
        viewer_->render();
        if (toggleCaptureEnabled()) spdlog::info("[trace] applyMask: frame rendered+presented");
    }
    maskW_ = w;
    maskH_ = h;
    window_->setMask(QRegion(0, 0, w, h));
    if (toggleCaptureEnabled()) spdlog::info("[trace] applyMask: mask set {}x{}", w, h);
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
    const bool animating = viewAnim_.active;
    syncRenderExtent(extent);
    // A running fit animation was aimed at the old extent (e.g. the scrollbars
    // vanishing as it starts changes the viewport size): aim it at the new one
    // rather than dropping it and stranding the view part-way.
    FitView retarget;
    if (animating && viewAnim_.refit && computeFitView(retarget))
        viewAnim_.to = retarget;
    else if (animating && !viewAnim_.refit && lastWidth_ > 0 && lastHeight_ > 0)
    {
        // A turn of a user's view (not a fit) keeps its scale: the half-extents
        // follow the new viewport size instead of being aimed at a fit.
        const double rx = static_cast<double>(extent.width) / lastWidth_;
        const double ry = static_cast<double>(extent.height) / lastHeight_;
        viewAnim_.from.halfW *= rx;
        viewAnim_.to.halfW *= rx;
        viewAnim_.from.halfH *= ry;
        viewAnim_.to.halfH *= ry;
    }
    else
        cancelViewAnimation();

    const double dpr = devicePixelRatioF();
    const QSize widgetPx(static_cast<int>(std::lround(width() * dpr)), static_cast<int>(std::lround(height() * dpr)));

    // A view that is just "fit to window" stays one.
    if (!viewDirty_ && !preserveView_ && hasScene_)
    {
        fitContentToWindow();
        resizeRef_.valid = false;
        lastWidgetPx_ = widgetPx;
        return;
    }

    // Otherwise the visible half-extents grow/shrink to match the new size,
    // centered on the same point (rather than re-fitting the whole view).
    double oldHalfW = (projection_->right - projection_->left) * 0.5;
    if (lastWidth_ > 0 && lastHeight_ > 0 && oldHalfW > 0.0)
    {
        double scale = lastWidth_ / (2.0 * oldHalfW); // pixels per world unit
        if (!preserveView_ && !lastWidgetPx_.isEmpty() && !widgetPx.isEmpty())
        {
            // Scale with the window so the same part of the image stays visible:
            // by the smaller of the width/height ratios, so everything that was
            // visible still is (with extra margin on the other axis).
            if (!resizeRef_.valid || std::abs(scale - resizeAppliedScale_) > 1e-6 * resizeAppliedScale_)
                resizeRef_ = {true, lastWidgetPx_, scale}; // a resize begins, or the view was changed since the last one
            const double ratio = std::min(widgetPx.width() / static_cast<double>(resizeRef_.widgetPx.width()),
                                          widgetPx.height() / static_cast<double>(resizeRef_.widgetPx.height()));
            scale = resizeRef_.scale * ratio;
            resizeAppliedScale_ = scale;
        }
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
    lastWidgetPx_ = widgetPx;
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
        applyExtent(viewportExtent());
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
        applyExtent(viewportExtent());
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
    std::vector<int> newLoadedImageFrameSel;
    std::vector<int> sceneImageToLoaded(newScene.images.size(), -1); // scene.images index -> loaded index
    std::filesystem::path givDir = paths.empty() ? std::filesystem::path() : giv::utf8ToPath(paths.front()).parent_path();
    for (size_t imgIdx = 0; imgIdx < newScene.images.size(); ++imgIdx)
    {
        const std::string& imgRef = newScene.images[imgIdx];
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

        sceneImageToLoaded[imgIdx] = static_cast<int>(newLoadedImageNames.size());
        newLoadedImageNames.push_back(giv::pathToUtf8(resolved));
        newLoadedImageFrameSel.push_back(imgIdx < newScene.imageFrameSel.size() ? newScene.imageFrameSel[imgIdx] : -1);
    }

    // The image in effect at each $frame: the last one set at or before it,
    // else the file's static one (the first $image outside any frame).
    std::vector<int> newMarkFrameImage(static_cast<size_t>(newScene.frameCount), -1);
    {
        int effective = newScene.baseImage >= 0 ? sceneImageToLoaded[static_cast<size_t>(newScene.baseImage)] : -1;
        for (size_t f = 0; f < newMarkFrameImage.size(); ++f)
        {
            if (f < newScene.frameImage.size() && newScene.frameImage[f] >= 0)
                effective = sceneImageToLoaded[static_cast<size_t>(newScene.frameImage[f])];
            newMarkFrameImage[f] = effective;
        }
    }

    scene_ = std::move(newScene);
    // $pixelsize directive (see SceneData::pixelSize) - only touch the
    // calibration if the file actually specified one, so a file that
    // doesn't mention it leaves whatever the user last dialed in (via Tools
    // > Calibrate Pixel Size) alone, matching giv's own cb_set_pixelsize
    // (fired only when the directive is present).
    if (scene_.pixelSize) setPixelCalibration(*scene_.pixelSize, scene_.pixelSizeUnit);
    loadedImageNames_ = std::move(newLoadedImageNames);
    loadedImageFrameSel_ = std::move(newLoadedImageFrameSel);
    markFrameImage_ = std::move(newMarkFrameImage);
    hasScene_ = true;
    animFrame_ = 0; // the build below then shows $frame 0 (see startMarkAnimation())
    currentImageIndex_ = 0;
    viewKeyFallback_ = paths.empty() ? std::string() : paths.front();
    scenePaths_ = paths;
    // With Remember View Per File, a multi-image scene reopens on the image it was left on.
    if (rememberView_ && loadedImageNames_.size() > 1 && !scenePaths_.empty())
        if (auto* store = stateStore())
            if (auto saved = store->loadImageIndex(scenePaths_); saved && *saved >= 0 && *saved < static_cast<int>(loadedImageNames_.size()))
            {
                currentImageIndex_ = *saved;
                spdlog::info("Restored remembered image {}/{}", currentImageIndex_ + 1, loadedImageNames_.size());
            }
    decodeCurrentImage();

    auto buildStart = std::chrono::steady_clock::now();
    if (!rebuildSceneGraph(error, /*isInitialLoad=*/true)) return false;
    tryRestoreView();
    double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();

    spdlog::info(
        "Scene loaded: {} dataset(s), {} point(s), {} image reference(s) resolved (parse {:.1f} ms, scene build {:.1f} ms)",
        scene_.datasets.size(), totalPoints, loadedImageNames_.size(), parseMs, buildMs);

    emit sceneLoaded();
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
    startMarkAnimation(); // no-op, bar-wise, for a scene without $frame
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
    // [perf] phase timing, logged once at the end (also on the early error returns it is simply omitted).
    using PerfClock = std::chrono::steady_clock;
    const auto perfStart = PerfClock::now();
    auto perfLast = perfStart;
    double perfWait = 0, perfBuild = 0, perfCompile = 0, perfRender = 0;
    const auto perfLap = [&perfLast]() {
        const auto now = PerfClock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - perfLast).count();
        perfLast = now;
        return ms;
    };
    // The global "View Marks" switch hides every dataset without disturbing
    // the per-dataset isVisible flags the Mark Browser tree maintains - so
    // apply it only for the duration of this build, then restore.
    std::vector<bool> savedVisible(scene_.datasets.size());
    for (size_t i = 0; i < scene_.datasets.size(); ++i)
    {
        savedVisible[i] = scene_.datasets[i].isVisible;
        const int f = scene_.datasets[i].frame;
        const bool inFrame = f < 0 || !hasMarkFrames() || f == animFrame_;
        scene_.datasets[i].isVisible = savedVisible[i] && globalShowMarks_ && inFrame;
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
        perfLap();
        sceneGraph = builder.build(scene_, shaderDir_, displayImages_);
        perfBuild = perfLap();
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
    perfLap(); // everything up to here is SceneBuilder::build() and the visibility bookkeeping
    if (everBuiltSceneGraph_ && !waitForGpuIdle())
    {
        if (error) *error = "GPU is not responding; try again";
        return false;
    }
    perfWait = perfLap();
    everBuiltSceneGraph_ = true;

    // Drop the previous build's event handlers (pan/zoom + balloon + caliper)
    // but keep the CloseHandler added once in the constructor.
    auto& handlers = viewer_->getEventHandlers();
    handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                   [](const vsg::ref_ptr<vsg::Visitor>& h) {
                                       return h.cast<giv::BalloonController>() || h.cast<giv::CaliperTool>();
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
        camera_ = vsg::Camera::create(projection_, lookAt, vsg::ViewportState::create(viewportExtent()));
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

    // Created once and kept across rebuilds: a mark animation rebuilds the
    // scene every frame, which must not cancel a zoom motion or drag under
    // way. (camera_ is never replaced once it exists.)
    if (!panZoom_)
    {
        panZoom_ = giv::PanZoomHandler::create(camera_);
        panZoom_->leftDragPans = [this]() { return mouse_.leftDragPans && !measureEnabled_; };
        panZoom_->viewportExtent = [this]() { return viewportExtent(); };
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
    }

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
    caliperTool_ = giv::CaliperTool::create(camera_, options_);
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
    caliperTool_->viewportExtent = [this]() { return viewportExtent(); };
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
    animTexture_ = builder.animatedTexture();
    sampleParams_ = builder.sampleParams();
    sampleTexture_ = builder.sampleTexture();
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
    const double perfSetup = perfLap(); // handlers, camera, picker, caliper, render graph objects
    try
    {
        viewer_->compile();
        perfCompile = perfLap();
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
        perfLap();
        try
        {
            viewer_->render();
            perfRender = perfLap();
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

    perfLap();
    updateScrollBars();
    const double perfScrollBars = perfLap();
    const auto* perfImg = displayImages_.empty() ? nullptr : &displayImages_.front();
    spdlog::info("[perf] rebuild total {:.1f} ms: gpu-wait {:.1f}, build {:.1f}, setup {:.1f}, compile {:.1f}, first-render {:.1f}, "
                 "scrollbars {:.1f} (initial={}, texture {}x{})",
                 std::chrono::duration<double, std::milli>(PerfClock::now() - perfStart).count(), perfWait, perfBuild, perfSetup,
                 perfCompile, perfRender, perfScrollBars, isInitialLoad, perfImg ? perfImg->width : 0, perfImg ? perfImg->height : 0);
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
    if (watched == clip_ && event->type() == QEvent::Resize) layoutRenderWindow();
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
            const QPoint d = we->angleDelta();
            // A tilt wheel reports horizontal steps; touchpads were handled above.
            if (mouse_.tiltZooms && d.x() != 0 && d.y() == 0 && panZoom_ && window_->windowAdapter)
            {
                // Horizontal scroll left (+x) zooms in, right zooms out; a mouse that sends its
                // wheel as horizontal scroll while a side button is held zooms with that button.
                const double factor = std::pow(0.9, d.x() / 120.0 * (we->modifiers() & Qt::ShiftModifier ? 5.0 : 1.0));
                const QPointF p = we->position() * window_->devicePixelRatio();
                panZoom_->zoomAbout(factor, static_cast<int32_t>(p.x()), static_cast<int32_t>(p.y()), viewportExtent());
                viewer_->request();
                return true;
            }
            if (mouse_.tiltNavigates && d.x() != 0 && d.y() == 0)
            {
                tiltAccum_ += d.x();
                while (std::abs(tiltAccum_) >= 120)
                {
                    emit swipeNavigate(tiltAccum_ > 0 ? -1 : 1);
                    tiltAccum_ -= tiltAccum_ > 0 ? 120 : -120;
                }
                return true;
            }
            // Wheel set to navigate: Ctrl+wheel still zooms (falls through to vsg).
            if (mouse_.wheel == MouseSettings::Wheel::Navigate && !(we->modifiers() & Qt::ControlModifier) &&
                d.y() != 0)
            {
                wheelNavAccum_ += d.y();
                while (std::abs(wheelNavAccum_) >= 120)
                {
                    emit swipeNavigate(wheelNavAccum_ > 0 ? -1 : 1);
                    wheelNavAccum_ -= wheelNavAccum_ > 0 ? 120 : -120;
                }
                return true;
            }
            break;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::MiddleButton)
            {
                // A wheel click without a drag; the drag itself pans (handled by vsg).
                if (event->type() == QEvent::MouseButtonPress)
                    middlePressPos_ = me->position();
                else if (mouse_.middleClick == MouseSettings::MiddleClick::FullScreen && hasScene_ &&
                         (me->position() - middlePressPos_).manhattanLength() < 4)
                    emit toggleFullScreenRequested();
                break;
            }
            if (me->button() != Qt::BackButton && me->button() != Qt::ForwardButton) break;
            // Swallowed (press and release) so PanZoomHandler::onClick never sees them.
            if (event->type() == QEvent::MouseButtonPress && hasScene_)
            {
                bool forward = me->button() == Qt::ForwardButton;
                if (mouse_.swapSideButtons) forward = !forward;
                switch (mouse_.sideButtons)
                {
                case MouseSettings::SideButtons::Navigate: emit swipeNavigate(forward ? 1 : -1); break;
                case MouseSettings::SideButtons::StepZoom: forward ? stepZoomIn() : stepZoomOut(); break;
                case MouseSettings::SideButtons::Nothing: break;
                }
            }
            return true;
        }
        case QEvent::MouseButtonDblClick: {
            // Left double-click toggles 1:1 / fit. Swallowed so vsgQt doesn't
            // turn it into a second press; not while measuring, where the
            // left button belongs to the caliper.
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::MiddleButton && me->modifiers() == Qt::NoModifier && hasScene_ && mouse_.doubleClickToggles &&
                mouse_.middleClick == MouseSettings::MiddleClick::Nothing)
            {
                // Middle double-click toggles the same way (a middle drag pans).
                toggleActualFit(me->position() * window_->devicePixelRatio());
                return true;
            }
            if (me->button() == Qt::LeftButton && me->modifiers() == Qt::NoModifier && hasScene_ && !measureEnabled_ &&
                mouse_.doubleClickToggles)
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
                                    viewportExtent(), /*animate=*/false);
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
    // X11 fills pixelDelta with the angle delta for a plain mouse wheel, so a pixel
    // delta only marks a touchpad when it differs from the angle delta.
    const bool touchpad =
        we->phase() != Qt::NoScrollPhase || (!we->pixelDelta().isNull() && we->pixelDelta() != we->angleDelta());
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
    if (loadedImages_.empty() || displayImages_.empty() || !currentImageSize_ || loadedImages_.front().isPreview()) return {};
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
    uint8_t r, g, b, a;
    if (raw.sampleType != WIV_SAMPLE_NONE)
    {
        // The GPU does the stretch, so work out the colour the shader shows for this one sample.
        const size_t rawIdx = static_cast<size_t>(raw.width) * raw.height * currentSlice_ + sampleIdx;
        const uint32_t rgba = giv::displayRgbaAt(raw, img.display.min, img.display.max, img.display.colormap,
                                                  img.display.colormapEnabled, rawIdx);
        r = rgba & 0xff, g = (rgba >> 8) & 0xff, b = (rgba >> 16) & 0xff, a = 255;
    }
    else
    {
        const uint8_t* p = &img.rgba[sampleIdx * 4]; // img (displayImages_) already holds only the current slice's pixels
        r = p[0], g = p[1], b = p[2], a = p[3];
    }

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
    const auto extent = viewportExtent();
    if (extent.width == 0 || extent.height == 0) return;
    // pan() takes the drag a mouse would make: the content follows it, so
    // moving the view right is a drag to the left.
    double dx = -fx * extent.width, dy = -fy * extent.height;
    // A key repeat while a glide is under way extends it by what is left.
    if (panAnim_.active)
    {
        dx += panAnim_.totalX - panAnim_.appliedX;
        dy += panAnim_.totalY - panAnim_.appliedY;
    }
    cancelViewAnimation();
    panZoom_->cancelZoom();
    const double duration = panZoom_->zoomDuration() * 0.6;
    if (duration <= 0.0)
    {
        panZoom_->panByPixels(static_cast<int32_t>(std::lround(dx)), static_cast<int32_t>(std::lround(dy)), extent);
        viewDirty_ = true;
        viewer_->request();
        updateScrollBars();
        return;
    }
    panAnim_.totalX = dx;
    panAnim_.totalY = dy;
    panAnim_.appliedX = panAnim_.appliedY = 0;
    panAnim_.duration = duration;
    panAnim_.clock.start();
    panAnim_.active = true;
    if (!panAnimTimer_)
    {
        panAnimTimer_ = new QTimer(this);
        panAnimTimer_->setInterval(8);
        connect(panAnimTimer_, &QTimer::timeout, this, &VulkanViewport::tickPanAnimation);
    }
    panAnimTimer_->start();
    tickPanAnimation();
}

void VulkanViewport::tickPanAnimation()
{
    if (!panAnim_.active || !panZoom_ || !window_ || !window_->windowAdapter)
    {
        cancelViewAnimation();
        return;
    }
    const double u = std::min(panAnim_.clock.elapsed() / 1000.0 / panAnim_.duration, 1.0);
    const double e = 1.0 - (1.0 - u) * (1.0 - u) * (1.0 - u); // ease-out: responsive to a keypress
    const int tx = static_cast<int>(std::lround(panAnim_.totalX * e));
    const int ty = static_cast<int>(std::lround(panAnim_.totalY * e));
    const int dx = tx - panAnim_.appliedX, dy = ty - panAnim_.appliedY;
    panAnim_.appliedX = tx;
    panAnim_.appliedY = ty;
    if (dx || dy)
    {
        panZoom_->panByPixels(dx, dy, viewportExtent());
        viewDirty_ = true;
        viewer_->request();
        updateScrollBars();
    }
    if (u >= 1.0) cancelViewAnimation();
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

void VulkanViewport::rememberSlice()
{
    if (!rememberView_) return;
    if (auto* store = stateStore()) store->saveSlice(currentImageName(), currentSlice_);
}

void VulkanViewport::nextSlice()
{
    const int count = sliceCount();
    if (count < 2) return;
    currentSlice_ = (currentSlice_ + 1) % count;
    rememberSlice();
    recomputeDisplayImage();
    QString error;
    if (!applySlice(&error))
        spdlog::error("Failed to switch slice: {}", error.toStdString());
    emit sliceChanged(currentSlice_, count);
}

void VulkanViewport::previousSlice()
{
    const int count = sliceCount();
    if (count < 2) return;
    currentSlice_ = (currentSlice_ - 1 + count) % count;
    rememberSlice();
    recomputeDisplayImage();
    QString error;
    if (!applySlice(&error))
        spdlog::error("Failed to switch slice: {}", error.toStdString());
    emit sliceChanged(currentSlice_, count);
}

void VulkanViewport::upgradePreview(const std::string& path, giv::LoadedImage full)
{
    if (!isShowingPreview() || currentImageName() != path || currentPage_ != 0) return;
    imageCache_.put(path, 0, std::move(full));
    upgradePreviewFromCache();
    startAnimationIfAny();
}

bool VulkanViewport::upgradePreviewFromCache()
{
    if (!isShowingPreview()) return true;
    if (!imageCache_.contains(currentImageName())) return false;
    const auto start = std::chrono::steady_clock::now();
    if (!decodeImageAt(currentImageIndex_, 0, /*allowPreview=*/false)) return false;
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
    {
        spdlog::error("Failed to show full-resolution image: {}", error.toStdString());
        return true;
    }
    updateScrollBars();
    viewer_->request();
    spdlog::info("Preview replaced by full image {} ({:.0f} ms)", currentImageName(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

void VulkanViewport::cachePrefetchedImage(const std::string& path, giv::LoadedImage image)
{
    imageCache_.put(path, 0, std::move(image));
}

bool VulkanViewport::decodeImageAt(int index, int page, bool allowPreview)
{
    resetAnimation();
    if (index < 0 || index >= static_cast<int>(loadedImageNames_.size())) return false;
    const std::string& name = loadedImageNames_[static_cast<size_t>(index)];
    const auto perfStart = std::chrono::steady_clock::now();
    const auto perfMs = [&perfStart]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - perfStart).count();
    };
    // Always put a low-resolution preview up first when one is prefetched,
    // even if the full image is cached too: uploading a full-resolution
    // texture takes far longer than a step should. The owner swaps the full
    // image in once stepping pauses (upgradePreviewFromCache()). With no
    // prefetched preview and no cached full image, decode a preview right here
    // if the plugin can do so cheaply; the owner then fetches the full decode.
    const bool fullCached = imageCache_.contains(name);
    if (allowPreview && page == 0)
    {
        const giv::LoadedImage* cached = previewCache_.peek(name);
        std::optional<giv::LoadedImage> preview;
        if (cached) preview = *cached;
        else if (!fullCached && !noPreview_.count(name)) preview = giv::ImagePluginHost::loadPreview(name);
        if (preview)
        {
            currentImageIndex_ = index;
            currentPage_ = 0;
            currentSlice_ = 0;
            currentImageSize_ = std::make_pair(static_cast<double>(preview->fullWidth), static_cast<double>(preview->fullHeight));
            loadedImages_ = {std::move(*preview)};
            recomputeDisplayImage();
            emit sliceChanged(currentSlice_, sliceCount());
            emit pageChanged(currentPage_, pageCount());
            spdlog::info("[perf] decodeImageAt: {} preview {}x{} (full {}x{}) in {:.1f} ms", cached ? "prefetched" : "synchronous",
                         loadedImages_.front().width, loadedImages_.front().height, loadedImages_.front().fullWidth,
                         loadedImages_.front().fullHeight, perfMs());
            return true;
        }
        if (!cached && !fullCached) noPreview_.insert(name);
    }
    const giv::LoadedImage* img = imageCache_.get(name, page);
    if (!img) return false;
    const double perfGet = perfMs();
    currentImageIndex_ = index;
    currentPage_ = page;
    // A volume reopens on the slice it was left on (with Remember View Per File).
    currentSlice_ = 0;
    if (rememberView_ && img->depth > 1 && page == 0)
        if (auto* store = stateStore())
            if (auto saved = store->loadSlice(name); saved && *saved >= 0 && *saved < img->depth)
            {
                currentSlice_ = *saved;
                spdlog::info("Restored remembered slice {}/{}", currentSlice_ + 1, img->depth);
            }
    loadedImages_ = {*img};
    currentImageSize_ = std::make_pair(static_cast<double>(img->width), static_cast<double>(img->height));
    // "$image f.gif frame=N": show frame N of the multi-frame file.
    const int sel = static_cast<size_t>(index) < loadedImageFrameSel_.size() ? loadedImageFrameSel_[static_cast<size_t>(index)] : -1;
    if (sel >= 0 && page == 0)
    {
        if (selAnimPath_ != name)
        {
            selAnim_ = giv::ImagePluginHost::loadAnimation(name, kAnimationBudgetBytes, nullptr);
            selAnimPath_ = name;
        }
        if (selAnim_ && selAnim_->width == img->width && selAnim_->height == img->height && selAnim_->frameCount() > 0)
            loadedImages_.front().rgba = selAnim_->frames[std::min(static_cast<size_t>(sel), selAnim_->frameCount() - 1)];
        else
            spdlog::warn("{}: no frame {} to show (not a multi-frame file?)", name, sel);
    }
    const double perfCopy = perfMs();
    recomputeDisplayImage();
    const double perfRecompute = perfMs();
    emit sliceChanged(currentSlice_, sliceCount());
    emit pageChanged(currentPage_, pageCount());
    spdlog::info("[perf] decodeImageAt: full {}x{} {} - get {:.1f} ms, copy {:.1f} ms, display copy {:.1f} ms, total {:.1f} ms",
                 img->width, img->height, fullCached ? "from cache" : "decoded synchronously", perfGet, perfCopy - perfGet,
                 perfRecompute - perfCopy, perfMs());
    return true;
}

// ---------------------------------------------------------------------------
// Animation playback
// ---------------------------------------------------------------------------

void VulkanViewport::resetAnimation()
{
    const bool hadAnimation = anim_ != nullptr;
    if (animCancel_) animCancel_->store(1);
    animCancel_.reset();
    if (animWatcher_)
    {
        // The worker finishes (or notices the cancel) on its own; its result is simply never looked at.
        animWatcher_->disconnect(this);
        animWatcher_->deleteLater();
        animWatcher_ = nullptr;
    }
    anim_.reset();
    animPath_.clear();
    animTexture_ = nullptr;
    animTextureDirty_ = 0;
    // The timer, frame and play state belong to the marks' $frame sections if
    // the scene has them: navigating between images leaves those running.
    if (!hasMarkFrames())
    {
        if (animTimer_) animTimer_->stop();
        animFrame_ = 0;
        animPlaysDone_ = 0;
        animPlaying_ = false;
        if (hadAnimation) emitAnimationChanged();
    }
}

void VulkanViewport::emitAnimationChanged()
{
    emit animationChanged(animFrame_, animationFrameCount(), animPlaying_, animSpeed_);
}

void VulkanViewport::startAnimationIfAny()
{
    if (loadedImages_.empty() || isShowingPreview() || currentPage_ != 0 || pageCount() > 1) return;
    if (hasMarkFrames()) return; // the marks' frame clock wins
    const std::string path = currentImageName();
    if (path.empty() || animPath_ == path) return; // decoding, playing, or already found to be a still
    if (!giv::ImagePluginHost::mayBeAnimated(path)) return;

    animPath_ = path;
    animCancel_ = std::make_shared<std::atomic<int>>(0);
    auto cancel = animCancel_;
    animWatcher_ = new QFutureWatcher<std::shared_ptr<const giv::Animation>>(this);
    connect(animWatcher_, &QFutureWatcherBase::finished, this, [this, w = animWatcher_]() {
        auto anim = w->result();
        w->deleteLater();
        animWatcher_ = nullptr;
        onAnimationDecoded(anim);
    });
    animWatcher_->setFuture(QtConcurrent::run([path, cancel]() {
        // std::atomic<int> is layout-compatible with the plain int the plugin host polls.
        return giv::ImagePluginHost::loadAnimation(path, kAnimationBudgetBytes,
                                                   reinterpret_cast<const volatile int*>(cancel.get()));
    }));
}

void VulkanViewport::onAnimationDecoded(const std::shared_ptr<const giv::Animation>& anim)
{
    // A still (or an animation we cannot hold): animPath_ stays set so that this file is not tried again.
    if (!anim || loadedImages_.empty() || isShowingPreview() || currentImageName() != animPath_) return;
    giv::LoadedImage& image = loadedImages_.front();
    if (anim->width != image.width || anim->height != image.height)
    {
        spdlog::warn("Animation frames are {}x{} but the still is {}x{}; not animating", anim->width, anim->height,
                     image.width, image.height);
        return;
    }
    spdlog::info("Animation: {} frames, {}x{}, {} plays", anim->frameCount(), anim->width, anim->height,
                 anim->loopCount ? std::to_string(anim->loopCount) : "endless");
    anim_ = anim;
    animFrame_ = 0;
    animPlaysDone_ = 0;

    // One rebuild to swap in a texture that can be rewritten per frame.
    image.animated = true;
    image.rgba = anim->frames[0];
    recomputeDisplayImage();
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
    {
        spdlog::error("Failed to set up animation: {}", error.toStdString());
        anim_.reset();
        image.animated = false;
        return;
    }
    viewer_->request();

    animPlaying_ = true;
    animClock_.start();
    animDueMs_ = 0;
    scheduleNextAnimationFrame();
    emitAnimationChanged();
}

// Makes frame `frame` the picture: the shared pixel buffers (so pixel readout,
// rebuilds and copies see it) and the live texture.
void VulkanViewport::showAnimationFrame(int frame)
{
    if (!anim_ && hasMarkFrames())
    {
        if (frame < 0 || frame >= scene_.frameCount) return;
        animFrame_ = frame;
        const int wantImage = static_cast<size_t>(frame) < markFrameImage_.size() ? markFrameImage_[static_cast<size_t>(frame)] : -1;
        if (wantImage >= 0 && (wantImage != currentImageIndex_))
        {
            if (!decodeImageAt(wantImage))
                spdlog::error("Failed to decode {} for frame {}", loadedImageNames_[static_cast<size_t>(wantImage)], frame);
        }
        if (scene_.frameProvider)
        {
            // Frames are generated on demand: swap in this frame's datasets.
            std::erase_if(scene_.datasets, [](const giv::Dataset& ds) { return ds.frame >= 0; });
            for (giv::Dataset& ds : scene_.frameProvider->datasets(frame)) scene_.datasets.push_back(std::move(ds));
        }
        QString error;
        if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
            spdlog::error("Failed to show mark frame {}: {}", frame, error.toStdString());
        return;
    }
    if (!anim_ || loadedImages_.empty() || frame < 0 || frame >= animationFrameCount()) return;
    animFrame_ = frame;
    const giv::PixelBuffer& pixels = anim_->frames[static_cast<size_t>(frame)];
    loadedImages_.front().rgba = pixels;
    if (!displayImages_.empty()) displayImages_.front().rgba = pixels;
    if (animTexture_ && animTexture_->dataSize() == pixels.size())
    {
        std::memcpy(animTexture_->dataPointer(), pixels.data(), pixels.size());
        // vsg keeps a GPU copy per frame in flight and refreshes the one for the frame
        // being recorded, so the change has to be flagged on each of the next few frames.
        animTextureDirty_ = 4;
    }
    viewer_->request();
}

// Called by GivViewer at the start of each rendered frame.
bool VulkanViewport::onAnimationFrameHook()
{
    if (renderGraph_ && window_ && window_->windowAdapter)
    {
        // The render area must always cover the whole (fixed-size) window, not
        // just its visible part, so the rest is cleared to the background colour
        // rather than left black: a render graph created from the camera's
        // viewport starts out with the visible size, and nothing widens it until
        // the next layout change (seen with animated images, which start
        // rendering before any such change).
        const VkExtent2D full = window_->windowAdapter->extent2D();
        auto& area = renderGraph_->renderArea;
        if (area.offset.x != 0 || area.offset.y != 0 || area.extent.width != full.width || area.extent.height != full.height)
        {
            area.offset = {0, 0};
            area.extent = full;
            renderGraph_->previous_extent = full;
        }
    }
    if (animTextureDirty_ <= 0 || !(animTexture_ || sampleParams_ || sampleTexture_)) return false;
    if (animTexture_) animTexture_->dirty();
    if (sampleParams_) sampleParams_->dirty();
    if (sampleTexture_) sampleTexture_->dirty();
    return --animTextureDirty_ > 0;
}

int VulkanViewport::animationDelayMs(int frame) const
{
    if (anim_) return anim_->delaysMs[static_cast<size_t>(frame)];
    if (static_cast<size_t>(frame) < scene_.frameDelayMs.size() && scene_.frameDelayMs[static_cast<size_t>(frame)] > 0)
        return scene_.frameDelayMs[static_cast<size_t>(frame)];
    const double fps = scene_.fps.value_or(24.0);
    return std::max(1, static_cast<int>(std::lround(1000.0 / fps)));
}

int VulkanViewport::animationLoopCount() const
{
    return anim_ ? anim_->loopCount : scene_.loopCount;
}

// Starts a scene's $frame sections from frame 0, playing; called after a load.
void VulkanViewport::startMarkAnimation()
{
    if (animTimer_) animTimer_->stop();
    animFrame_ = 0;
    animPlaysDone_ = 0;
    animPlaying_ = false;
    if (!hasMarkFrames()) { emitAnimationChanged(); return; }
    if (scene_.frameCount > 1)
    {
        animPlaying_ = true;
        animClock_.start();
        animDueMs_ = 0;
        scheduleNextAnimationFrame();
    }
    emitAnimationChanged();
}

void VulkanViewport::scheduleNextAnimationFrame()
{
    if (!hasAnimation() || !animPlaying_) return;
    const double delay = animationDelayMs(animFrame_) / animSpeed_;
    // Aim for the previous frame's due time plus its delay, not "now plus delay", so
    // slow frames don't make the whole animation run late - unless we're far behind.
    animDueMs_ += static_cast<qint64>(delay);
    qint64 wait = animDueMs_ - animClock_.elapsed();
    if (wait < -200) { animDueMs_ = animClock_.elapsed(); wait = 0; }
    animTimer_->start(static_cast<int>(std::max<qint64>(wait, 1)));
}

void VulkanViewport::advanceAnimation()
{
    if (!hasAnimation() || !animPlaying_) return;
    int next = animFrame_ + 1;
    if (next >= animationFrameCount())
    {
        ++animPlaysDone_;
        if (!animLoop_ || (animationLoopCount() > 0 && animPlaysDone_ >= animationLoopCount()))
        {
            animPlaying_ = false; // stays on the last frame
            emitAnimationChanged();
            return;
        }
        next = 0;
    }
    showAnimationFrame(next);
    scheduleNextAnimationFrame();
    emitAnimationChanged();
}

void VulkanViewport::toggleAnimationPlay()
{
    if (!hasAnimation()) return;
    if (animPlaying_)
    {
        animPlaying_ = false;
        animTimer_->stop();
    }
    else
    {
        if (animFrame_ >= animationFrameCount() - 1 && (!animLoop_ || animPlaysDone_ > 0))
        {
            showAnimationFrame(0); // a finished run plays again from the start
            animPlaysDone_ = 0;
        }
        animPlaying_ = true;
        animDueMs_ = animClock_.elapsed();
        scheduleNextAnimationFrame();
    }
    emitAnimationChanged();
}

void VulkanViewport::stopAnimation()
{
    if (!hasAnimation()) return;
    animPlaying_ = false;
    animTimer_->stop();
    animPlaysDone_ = 0;
    showAnimationFrame(0);
    emitAnimationChanged();
}

void VulkanViewport::rewindAnimation()
{
    if (!hasAnimation()) return;
    animPlaysDone_ = 0;
    showAnimationFrame(0);
    if (animPlaying_)
    {
        // Keep playing, with the first frame's full delay ahead of it.
        animTimer_->stop();
        animDueMs_ = animClock_.elapsed();
        scheduleNextAnimationFrame();
    }
    emitAnimationChanged();
}

void VulkanViewport::restartAnimation()
{
    if (!hasAnimation()) return;
    animTimer_->stop();
    animPlaysDone_ = 0;
    showAnimationFrame(0);
    animPlaying_ = true;
    animDueMs_ = animClock_.elapsed();
    scheduleNextAnimationFrame();
    emitAnimationChanged();
}

void VulkanViewport::seekAnimationFrame(int frame)
{
    if (!hasAnimation() || frame == animFrame_) return;
    animPlaying_ = false;
    animTimer_->stop();
    showAnimationFrame(std::clamp(frame, 0, animationFrameCount() - 1));
    emitAnimationChanged();
}

void VulkanViewport::stepAnimationFrame(int dir)
{
    if (!hasAnimation()) return;
    animPlaying_ = false;
    animTimer_->stop();
    const int n = animationFrameCount();
    // One frame wraps around; a bigger jump stops at the first/last frame.
    showAnimationFrame(std::abs(dir) == 1 ? ((animFrame_ + dir) % n + n) % n : std::clamp(animFrame_ + dir, 0, n - 1));
    emitAnimationChanged();
}

void VulkanViewport::setAnimationSpeed(double speed)
{
    animSpeed_ = std::clamp(speed, 0.125, 8.0);
    if (animPlaying_)
    {
        // Re-time the frame being waited on.
        animDueMs_ = animClock_.elapsed();
        scheduleNextAnimationFrame();
    }
    emitAnimationChanged();
}

void VulkanViewport::setAnimationLooping(bool loop)
{
    animLoop_ = loop;
    emitAnimationChanged();
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
        // Raw samples are windowed and colormapped by the GPU (image_samples.frag): just describe how.
        const ContrastState state = currentContrastState();
        display.display = {state.min, state.max, state.colormapId, state.colormapEnabled, currentSlice_};
    }
    displayImages_ = {std::move(display)};
}

QImage VulkanViewport::currentViewImage(int maxSize)
{
    if (loadedImages_.empty()) return QImage();
    const giv::LoadedImage& src = loadedImages_.front();
    if (src.isPreview() || src.width <= 0 || src.height <= 0) return QImage();

    giv::PixelBuffer rgba;
    if (src.sampleType != WIV_SAMPLE_NONE)
    {
        const ContrastState state = currentContrastState();
        giv::renderDisplayRgba(src, state.min, state.max, state.colormapId, state.colormapEnabled, rgba, currentSlice_);
    }
    else
        rgba = src.rgba;
    QImage full(rgba.data(), src.width, src.height, src.width * 4, QImage::Format_RGBA8888);
    return full.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation).copy();
}

// Contrast state is per image - and per page of a multi-page file (TIFF, HDF5
// datasets), whose pages can have entirely different value ranges.
std::string VulkanViewport::contrastKey() const
{
    std::string key = giv::ImageStateStore::canonicalPath(currentImageName());
    if (currentPage_ > 0) key += "#page" + std::to_string(currentPage_);
    return key;
}

VulkanViewport::ContrastState VulkanViewport::currentContrastState()
{
    static const ContrastState kEmpty{};
    if (loadedImages_.empty()) return kEmpty;

    const giv::LoadedImage& src = loadedImages_.front();
    const std::string path = contrastKey();
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
        // What the user set for this file in an earlier run. The contrast
        // window only comes back with Auto Contrast off, as within a run.
        if (auto* store = currentPage_ == 0 ? stateStore() : nullptr) // later pages are remembered within the run only
            if (auto saved = store->loadContrast(path))
            {
                if (saved->colormapId >= 0 && saved->colormapId < static_cast<int>(giv::colormaps::Id::Count))
                {
                    defaultState.colormapId = static_cast<giv::colormaps::Id>(saved->colormapId);
                    defaultState.colormapEnabled = saved->colormapEnabled;
                }
                if (!autoContrast_ && saved->max > saved->min)
                {
                    defaultState.min = saved->min;
                    defaultState.max = saved->max;
                }
            }
        it = contrastState_.emplace(path, defaultState).first;
    }
    return it->second;
}

// Shows displayImages_ after a contrast/colormap change: a raw-sample image
// only needs its window/colormap uniform buffer rewritten (the GPU does the
// rest); anything else rebuilds the scene.
bool VulkanViewport::applyDisplayImage(QString* error)
{
    if (sampleParams_ && !displayImages_.empty())
    {
        giv::SceneBuilder::fillSampleParams(*sampleParams_, displayImages_.front());
        animTextureDirty_ = 4; // see showAnimationFrame()
        viewer_->request();
        return true;
    }
    return rebuildSceneGraph(error, /*isInitialLoad=*/false);
}

// Shows the slice just selected in displayImages_: copies it into the
// volume's live texture; else rebuilds the scene.
bool VulkanViewport::applySlice(QString* error)
{
    if (sampleTexture_ && sceneBuilder_ && !displayImages_.empty() && sceneBuilder_->setSampleSlice(displayImages_.front()))
    {
        animTextureDirty_ = 4;
        viewer_->request();
        return true;
    }
    return rebuildSceneGraph(error, /*isInitialLoad=*/false);
}

void VulkanViewport::setContrast(float min, float max)
{
    if (loadedImages_.empty() || loadedImages_.front().sampleType == WIV_SAMPLE_NONE) return;

    ContrastState state = currentContrastState();
    state.min = min;
    state.max = max;
    contrastState_[contrastKey()] = state;
    if (auto* store = currentPage_ == 0 ? stateStore() : nullptr)
        store->saveContrast(currentImageName(), {state.min, state.max, static_cast<int>(state.colormapId), state.colormapEnabled});
    lastContrastMin_ = min;
    lastContrastMax_ = max;
    hasLastContrast_ = true;

    const auto perfStart = std::chrono::steady_clock::now();
    recomputeDisplayImage();
    const auto perfRendered = std::chrono::steady_clock::now();
    QString error;
    if (!applyDisplayImage(&error))
        spdlog::error("Failed to apply contrast: {}", error.toStdString());
    const auto perfApplied = std::chrono::steady_clock::now();
    emit contrastChanged();
    const auto perfMs = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    spdlog::info("[perf] contrast change: render {:.1f} ms, apply {:.1f} ms, signal {:.1f} ms", perfMs(perfStart, perfRendered),
                 perfMs(perfRendered, perfApplied), perfMs(perfApplied, std::chrono::steady_clock::now()));
}

void VulkanViewport::setColormap(giv::colormaps::Id id, bool enabled)
{
    if (loadedImages_.empty() || loadedImages_.front().sampleType == WIV_SAMPLE_NONE) return;

    ContrastState state = currentContrastState();
    state.colormapId = id;
    state.colormapEnabled = enabled;
    contrastState_[contrastKey()] = state;
    if (auto* store = currentPage_ == 0 ? stateStore() : nullptr)
        store->saveContrast(currentImageName(), {state.min, state.max, static_cast<int>(state.colormapId), state.colormapEnabled});
    lastColormapId_ = id;
    lastColormapEnabled_ = enabled;

    const auto perfStart = std::chrono::steady_clock::now();
    recomputeDisplayImage();
    const auto perfRendered = std::chrono::steady_clock::now();
    QString error;
    if (!applyDisplayImage(&error))
        spdlog::error("Failed to apply color table: {}", error.toStdString());
    const auto perfApplied = std::chrono::steady_clock::now();
    emit contrastChanged();
    const auto perfMs = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    spdlog::info("[perf] color table change: render {:.1f} ms, apply {:.1f} ms, signal {:.1f} ms", perfMs(perfStart, perfRendered),
                 perfMs(perfRendered, perfApplied), perfMs(perfApplied, std::chrono::steady_clock::now()));
}

std::array<uint32_t, 256> VulkanViewport::computeHistogram(bool wholeVolume) const
{
    std::array<uint32_t, 256> histogram{};
    if (loadedImages_.empty()) return histogram;

    const giv::LoadedImage& img = loadedImages_.front();
    if (img.sampleType == WIV_SAMPLE_NONE) return histogram;

    const float range = img.sampleMax - img.sampleMin;
    const size_t pixelCount = static_cast<size_t>(img.width) * img.height;
    const size_t first = wholeVolume ? 0 : pixelCount * static_cast<size_t>(currentSlice_);
    const size_t last = wholeVolume ? pixelCount * static_cast<size_t>(std::max(1, img.depth)) : first + pixelCount;
    for (size_t i = first; i < last; ++i)
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
    if (rememberView_ && !scenePaths_.empty())
        if (auto* store = stateStore()) store->saveImageIndex(scenePaths_, index);
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
    if (!renderGraph_)
    {
        if (toggleCaptureEnabled()) spdlog::info("[trace] syncRenderExtent: NO renderGraph_");
        return;
    }

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
    // `extent` is the visible part of the window, which may be smaller than the
    // window itself (see viewportExtent()): draw into that part only, but clear
    // the whole surface so that area uncovered by growing the visible region
    // already shows the background colour.
    const VkExtent2D full = window_->windowAdapter->extent2D();
    renderGraph_->renderArea.offset = {0, 0};
    renderGraph_->renderArea.extent = full;
    renderGraph_->previous_extent = full;
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
    auto extent = viewportExtent();
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
    const auto extent = viewportExtent();
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
void VulkanViewport::startViewAnimation(const FitView& to, double rotFrom, bool refit)
{
    auto lookAt = camera_ ? camera_->viewMatrix.cast<vsg::LookAt>() : vsg::ref_ptr<vsg::LookAt>();
    const double duration = panZoom_ ? panZoom_->zoomDuration() * 1.5 : 0.0;
    if (!lookAt || !projection_ || duration <= 0.0)
    {
        if (refit) fitContentToWindow();
        else viewer_->request();
        return;
    }
    cancelViewAnimation();
    if (panZoom_) panZoom_->cancelZoom();
    viewAnim_.from = {lookAt->center.x, lookAt->center.y, (projection_->right - projection_->left) * 0.5,
                      (projection_->top - projection_->bottom) * 0.5};
    viewAnim_.to = to;
    viewAnim_.refit = refit;
    viewAnim_.rotFrom = rotFrom;
    if (flipProjection_) flipProjection_->extraAngle = rotFrom;
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
        const bool refit = viewAnim_.refit;
        cancelViewAnimation(); // also drops any leftover rotation
        if (refit) fitContentToWindow();
        return;
    }
    const double e = u * u * (3.0 - 2.0 * u); // smoothstep: eases in and out
    if (flipProjection_) flipProjection_->extraAngle = viewAnim_.rotFrom * (1.0 - e);
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
    const bool wasRotating = flipProjection_ && flipProjection_->extraAngle != 0.0;
    viewAnim_.active = false;
    viewAnim_.rotFrom = 0.0;
    panAnim_.active = false;
    if (viewAnimTimer_) viewAnimTimer_->stop();
    if (panAnimTimer_) panAnimTimer_->stop();
    if (wasRotating)
    {
        flipProjection_->extraAngle = 0.0; // a cut-short turn snaps to its final orientation
        if (viewer_) viewer_->request();
    }
}

// window_->resize() (called from resizeEvent() to keep the embedded native
// window in sync with its container) only *posts* that resize: the platform
// plugin round-trips it (an X11 ConfigureRequest/ConfigureNotify exchange)
// before viewportExtent() actually reflects it - observed
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
        const auto extent = viewportExtent();
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
    auto extent = viewportExtent();
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

    // Zoomed out past "fill", an axis whose content is smaller than the
    // window stays centered instead of drifting wherever the zoom left it.
    if (!viewAnim_.active)
    {
        const double contentCenterX = (minXyd + maxXyd) * 0.5;
        const double contentCenterY = -(minYyd + maxYyd) * 0.5;
        bool centered = false;
        if ((maxXyd - minXyd) * scaleX < extent.width - 0.5 && lookAt->center.x != contentCenterX)
        {
            lookAt->eye.x = lookAt->center.x = contentCenterX;
            centered = true;
        }
        if ((maxYyd - minYyd) * scaleY < extent.height - 0.5 && lookAt->center.y != contentCenterY)
        {
            lookAt->eye.y = lookAt->center.y = contentCenterY;
            centered = true;
        }
        if (centered) viewer_->request();
    }

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
    auto extent = viewportExtent();
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
    return width > 0.0 ? viewportExtent().width / width : 0.0;
}

double VulkanViewport::fitScale() const
{
    if (!window_ || !window_->windowAdapter) return 0.0;
    double minX, minY, maxX, maxY;
    currentFitBoundsYDown(minX, minY, maxX, maxY);
    const auto extent = viewportExtent();
    double dataW = std::max(maxX - minX, 2e-3), dataH = std::max(maxY - minY, 2e-3);
    rotatedSize(dataW, dataH);
    return containScale(extent.width, extent.height, dataW, dataH);
}

void VulkanViewport::stepZoomIn() { stepZoom(true); }
void VulkanViewport::stepZoomOut() { stepZoom(false); }

void VulkanViewport::stepZoom(bool in)
{
    if (!hasScene_ || !panZoom_ || !window_ || !window_->windowAdapter) return;
    const auto extent = viewportExtent();
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
                        viewportExtent(), /*animate=*/true, /*replaceMotion=*/true);
}

giv::ImageStateStore* VulkanViewport::stateStore()
{
    // With Remember View Per File off the database is neither opened, read nor written.
    if (!rememberView_) return nullptr;
    if (!stateStore_)
    {
        const QSettings settings(QSettings::IniFormat, QSettings::UserScope, "wiv", "wiv");
        const QString dir = QFileInfo(settings.fileName()).absolutePath();
        stateStore_ = std::make_unique<giv::ImageStateStore>((dir + "/state.db").toStdString());
    }
    return stateStore_->isOpen() ? stateStore_.get() : nullptr;
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
    return giv::ImageStateStore::canonicalPath(name.empty() ? viewKeyFallback_ : name);
}

// Remembers the displayed file's view if the user changed it (a fit view is
// the default and not worth keeping - and would be wrong after a resize).
void VulkanViewport::saveCurrentView()
{
    if (!rememberView_ || !hasScene_ || !camera_ || !projection_ || !window_ || !window_->windowAdapter) return;
    const std::string key = viewKey();
    if (key.empty()) return;
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    const auto extent = viewportExtent();
    const double width = projection_->right - projection_->left;
    const double height = projection_->top - projection_->bottom;
    if (!viewDirty_ || !lookAt || extent.width == 0 || extent.height == 0 || width <= 0.0 || height <= 0.0)
    {
        savedViews_.erase(key);
        if (auto* store = stateStore()) store->clearView(key);
        return;
    }
    const SavedView view{lookAt->center.x, lookAt->center.y, extent.width / width, extent.height / height};
    savedViews_[key] = view;
    if (auto* store = stateStore()) store->saveView(key, {view.centerX, view.centerY, view.scaleX, view.scaleY});
}

// Puts back the view remembered for the file now displayed, if any. The
// scale is stored in device pixels per world unit, so the zoom survives a
// window resize since. Mirrors the tail of fitToBounds().
bool VulkanViewport::tryRestoreView()
{
    if (!rememberView_ || !camera_ || !projection_ || !window_ || !window_->windowAdapter) return false;
    auto it = savedViews_.find(viewKey());
    if (it == savedViews_.end())
    {
        // Not seen this run: maybe in an earlier one.
        auto* store = stateStore();
        auto saved = store ? store->loadView(viewKey()) : std::nullopt;
        if (!saved) return false;
        it = savedViews_.emplace(viewKey(), SavedView{saved->centerX, saved->centerY, saved->scaleX, saved->scaleY}).first;
    }
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    const auto extent = viewportExtent();
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
        scene_.frameCount = 0;
        scene_.frameProvider.reset();
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
    auto extent = viewportExtent();
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
    auto extent = viewportExtent();
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
    cancelViewAnimation();
    quarterTurns_ = (quarterTurns_ + quarters) & 3;
    spdlog::info("Rotate view: {} degrees", quarterTurns_ * 90);
    if (flipProjection_) flipProjection_->quarterTurns = quarterTurns_;
    if (!hasScene_) return;

    // The turn glides: the projection starts at the old orientation (extra
    // angle) and eases to the new one. A fit follows along unless the user
    // has dialed in a view of their own, which just turns about its centre.
    const int signedQuarters = quarters == 3 ? -1 : quarters;
    double rotFrom = 0.0;
    if (flipProjection_)
        rotFrom = -(flipProjection_->flipX != flipProjection_->flipY ? -1.0 : 1.0) * signedQuarters * (vsg::PI * 0.5);

    FitView target;
    auto lookAt = camera_ ? camera_->viewMatrix.cast<vsg::LookAt>() : vsg::ref_ptr<vsg::LookAt>();
    if (!viewDirty_)
    {
        if (computeFitView(target)) startViewAnimation(target, rotFrom, /*refit=*/true);
        else fitContentToWindow();
    }
    else if (lookAt && projection_)
    {
        target = {lookAt->center.x, lookAt->center.y, (projection_->right - projection_->left) * 0.5,
                  (projection_->top - projection_->bottom) * 0.5};
        startViewAnimation(target, rotFrom, /*refit=*/false);
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
