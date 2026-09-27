#include "VulkanViewport.h"

#include "GivParser.h"
#include "ImagePluginHost.h"

#include <vsg/all.h>
#include <vsgXchange/all.h>
#include <vsgXchange/freetype.h>

#include <QGridLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace givqt
{

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

    // The default FIFO present mode makes vkQueuePresentKHR block until the
    // next vblank; under some compositors/drivers (observed: proprietary
    // NVIDIA) that wait never wakes up if the window is occluded or not yet
    // fully composited at the moment of the first present, permanently
    // hanging the GUI thread inside the driver (window shows only its very
    // first/cleared frame forever). FIFO_RELAXED still paces to vblank (so it
    // doesn't tear) in the common case, but - unlike plain FIFO - drops the
    // wait and presents immediately if a frame is running late, which is the
    // scenario that produced the original hang. WIV_PRESENT_MODE lets this
    // be overridden (mailbox/fifo/fifo_relaxed/immediate) in case a given
    // driver still misbehaves with FIFO_RELAXED at startup.
    traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
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
        double newCenterX = minXyd + static_cast<double>(value) / scaleX + halfW;
        double shift = newCenterX - lookAt->center.x;
        lookAt->eye.x += shift;
        lookAt->center.x += shift;
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
        double newCenterY = contentMaxY - static_cast<double>(value) / scaleY - halfH;
        double shift = newCenterY - lookAt->center.y;
        lookAt->eye.y += shift;
        lookAt->center.y += shift;
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
    shaderDir_ = WIV_SHADER_DIR;
}

void VulkanViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!camera_ || !projection_ || !window_ || !window_->windowAdapter) return;

    // vsgQt::Window::resizeEvent (fired on the embedded QWindow itself, as
    // Qt's layout resizes the container to match this widget) already calls
    // windowAdapter->resize() to keep the swapchain in sync; here we only
    // need to keep the camera's viewport/ortho extents matching the new
    // *device-pixel* size (extent2D(), not this QWidget's logical width()/
    // height() - they differ under devicePixelRatio() != 1).
    auto extent = window_->windowAdapter->extent2D();
    if (extent.width == 0 || extent.height == 0) return;

    if (camera_->viewportState) camera_->viewportState->set(0, 0, extent.width, extent.height);

    // Preserve the current pixels-per-world-unit scale (rather than
    // re-fitting/rescaling the whole view): the visible half-extents just
    // grow/shrink to match the new size, centered on the same point.
    double oldHalfW = (projection_->right - projection_->left) * 0.5;
    if (lastWidth_ > 0 && lastHeight_ > 0 && oldHalfW > 0.0)
    {
        double scale = lastWidth_ / (2.0 * oldHalfW); // pixels per world unit
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

bool VulkanViewport::loadFiles(const std::vector<std::string>& paths, QString* error)
{
    if (!window_ || !window_->windowAdapter)
    {
        if (error) *error = "Vulkan viewport is not yet initialized";
        return false;
    }

    giv::SceneData newScene;
    auto parseStart = std::chrono::steady_clock::now();
    size_t totalPoints = 0;
    for (const auto& f : paths)
    {
        // Mirrors giv's own load_file (giv-win.gob): only .giv/.marks/.svg
        // go through the text parser - anything else is a bare image file
        // and is loaded directly, the same way a $image reference inside a
        // .giv file is (see the loadedImages loop below).
        std::string ext = std::filesystem::path(f).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".giv" || ext == ".marks" || ext == ".svg")
        {
            giv::GivParser parser;
            std::string parseError;
            if (!parser.parseFile(f, newScene, parseError))
            {
                if (error) *error = QString::fromStdString(parseError);
                return false;
            }
        }
        else
        {
            newScene.images.push_back(f);
        }
    }
    for (const auto& ds : newScene.datasets) totalPoints += ds.pointCount();
    double parseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parseStart).count();
    std::cerr << "wiv: parsed " << newScene.datasets.size() << " dataset(s), " << totalPoints << " points in " << parseMs << " ms\n";

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
    std::filesystem::path givDir = paths.empty() ? std::filesystem::path() : std::filesystem::path(paths.front()).parent_path();
    for (const auto& imgRef : newScene.images)
    {
        std::filesystem::path resolved(imgRef);
        if (!std::filesystem::exists(resolved) && !givDir.empty())
            resolved = givDir / imgRef;

        if (!std::filesystem::exists(resolved) || !giv::ImagePluginHost::isSupported(resolved.string()))
            continue;

        newLoadedImageNames.push_back(resolved.string());
    }

    scene_ = std::move(newScene);
    loadedImageNames_ = std::move(newLoadedImageNames);
    hasScene_ = true;
    currentImageIndex_ = 0;
    decodeCurrentImage();

    auto buildStart = std::chrono::steady_clock::now();
    if (!rebuildSceneGraph(error, /*isInitialLoad=*/true)) return false;
    double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
    std::cerr << "wiv: scene build took " << buildMs << " ms\n";

    emit sceneLoaded();
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
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
        sceneGraph = builder.build(scene_, shaderDir_, loadedImages_);
    }
    catch (const std::exception& e)
    {
        for (size_t i = 0; i < scene_.datasets.size(); ++i) scene_.datasets[i].isVisible = savedVisible[i];
        if (error) *error = QString("failed to build scene: %1").arg(e.what());
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
    // first build).
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
    if (!isInitialLoad)
    {
        constexpr uint64_t kFenceTimeoutNs = 2'000'000'000; // 2 seconds
        for (size_t relativeFrameIndex = 0; relativeFrameIndex <= 2; ++relativeFrameIndex)
        {
            VkResult result = viewer_->waitForFences(relativeFrameIndex, kFenceTimeoutNs);
            if (result != VK_SUCCESS)
            {
                std::cerr << "wiv: GPU didn't finish frame -" << relativeFrameIndex
                          << " within " << (kFenceTimeoutNs / 1000000) << " ms (VkResult=" << result
                          << "); skipping this scene rebuild rather than risking a hang or destroying in-flight GPU resources\n";
                if (error) *error = "GPU is not responding; try again";
                return false;
            }
        }
    }

    // Drop the previous build's event handlers (pan/zoom + balloon + caliper)
    // but keep the CloseHandler added once in the constructor.
    auto& handlers = viewer_->getEventHandlers();
    handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                   [](const vsg::ref_ptr<vsg::Visitor>& h) {
                                       return h.cast<giv::PanZoomHandler>() || h.cast<giv::BalloonController>() || h.cast<giv::CaliperTool>();
                                   }),
                   handlers.end());

    if (isInitialLoad)
    {
        double minXyd, minYyd, maxXyd, maxYyd;
        currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);

        auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 1.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
        projection_ = vsg::Orthographic::create(-100.0, 100.0, -100.0, 100.0, 0.01, 100.0);
        camera_ = vsg::Camera::create(projection_, lookAt, vsg::ViewportState::create(window_->windowAdapter->extent2D()));

        // Default to "fill" (cover) on a fresh load: the loaded image/scene
        // fills the window edge-to-edge (cropping any overflowing axis)
        // rather than "fit" (contain), which would letterbox it.
        fitToBounds(minXyd, -maxYyd, maxXyd, -minYyd, /*fill=*/true);
    }

    panZoom_ = giv::PanZoomHandler::create(camera_);
    panZoom_->onCursorMove = [this](double x, double y) { emit cursorWorldPosition(x, y); };
    panZoom_->onViewChanged = [this]() {
        fillFitActive_ = false; // manual zoom/pan is a real departure from the resting fill-fit - see its doc comment
        updateScrollBars();
    };
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
        labelPicker_->updateScene(builder.labelGraph());
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
    caliperTool_->onNeedsCompile = [this]() { viewer_->compile(); viewer_->request(); };
    caliperTool_->onMeasurementText = [this](const std::string& text) { emit measurementChanged(QString::fromStdString(text)); };
    caliperTool_->setEnabled(wasMeasureEnabled);
    viewer_->addEventHandler(caliperTool_);
    measureEnabled_ = wasMeasureEnabled;

    viewer_->viewParams = builder.viewParams();
    viewer_->viewParams->setForceOpaque(forceOpaque_);
    viewer_->imageFilterAnimator = builder.imageFilterAnimator();

    // loadedImages_ only ever holds the single currently-displayed image (see
    // decodeCurrentImage()), so the switch built from it has exactly one
    // child - always select it, regardless of currentImageIndex_ (which
    // indexes into loadedImageNames_, the full $image candidate list).
    imageSwitch_ = builder.imageSwitch();
    if (imageSwitch_) imageSwitch_->setSingleChildOn(0);

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
    // isInitialLoad reassigns camera_ to a brand-new vsg::Camera (see above) -
    // without this, a second isInitialLoad (e.g. opening another file after
    // the first) would leave mainView_ still pointing at the *old* camera,
    // so every subsequent zoom/pan/fit (which all mutate the new camera_)
    // would silently affect an orphaned object nothing ever renders with.
    mainView_->camera = camera_;
    mainView_->addChild(vsg::createHeadlight());
    mainView_->addChild(sceneGraph);
    mainView_->addChild(caliperTool_->root());

    auto renderGraph = vsg::RenderGraph::create(window_->windowAdapter, mainView_);
    auto commandGraph = vsg::CommandGraph::create(window_->windowAdapter);
    commandGraph->addChild(renderGraph);

    viewer_->assignRecordAndSubmitTaskAndPresentation({labelPicker_->commandGraph(), commandGraph});
    viewer_->compile();

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
        viewer_->render();
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
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        std::cerr << "wiv: " << error.toStdString() << "\n";
}

void VulkanViewport::setShowMarks(bool show)
{
    if (globalShowMarks_ == show) return;
    globalShowMarks_ = show;
    if (!hasScene_) return;
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
        std::cerr << "wiv: " << error.toStdString() << "\n";
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

bool VulkanViewport::decodeImageAt(int index)
{
    if (index < 0 || index >= static_cast<int>(loadedImageNames_.size())) return false;
    const giv::LoadedImage* img = imageCache_.get(loadedImageNames_[static_cast<size_t>(index)]);
    if (!img) return false;
    currentImageIndex_ = index;
    loadedImages_ = {*img};
    currentImageSize_ = std::make_pair(static_cast<double>(img->width), static_cast<double>(img->height));
    return true;
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
    currentImageSize_.reset();
}

// Shared tail of nextImage()/previousImage(): decodes `index` (via
// imageCache_, evicting the least-recently-used entry if it's a miss),
// rebuilds the scene graph so the new image's texture actually replaces the
// old one (only one is ever GPU-resident at a time - see loadedImages_'s
// doc comment), then - if autoFit_ (giv's do_auto_fit_marks, default on) -
// re-fits the view; otherwise the current zoom/pan is left untouched, only
// now framing the new image's content instead.
void VulkanViewport::switchToImage(int index)
{
    if (!decodeImageAt(index))
    {
        std::cerr << "wiv: failed to decode " << loadedImageNames_[static_cast<size_t>(index)] << "\n";
        return;
    }
    QString error;
    if (!rebuildSceneGraph(&error, /*isInitialLoad=*/false))
    {
        std::cerr << "wiv: " << error.toStdString() << "\n";
        return;
    }
    if (autoFit_)
    {
        fitContentToWindow(/*fill=*/true); // matches the load-time default - see fitToBounds()'s doc comment
    }
    else
    {
        // Preserved zoom/pan now frames different content than the resting
        // fill-fit it may have come from - see fillFitActive_'s doc comment.
        fillFitActive_ = false;
        updateScrollBars();
    }
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
}

void VulkanViewport::fitToBounds(double minX, double minY, double maxX, double maxY, bool fill)
{
    if (!projection_ || !window_ || !window_->windowAdapter) return;

    double centerX = (minX + maxX) * 0.5;
    double centerY = (minY + maxY) * 0.5;

    double dataW = std::max(maxX - minX, 2e-3);
    double dataH = std::max(maxY - minY, 2e-3);
    auto extent = window_->windowAdapter->extent2D();
    double canvasW = static_cast<double>(extent.width);
    double canvasH = static_cast<double>(extent.height);

    double scaleX = canvasW / dataW;
    double scaleY = canvasH / dataH;
    double scale = fill ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    if (scale <= 0.0)
        scale = fill ? std::max(canvasW, canvasH) / std::min(dataW, dataH) : std::min(canvasW, canvasH) / std::max(dataW, dataH);

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

    lastWidth_ = static_cast<int>(extent.width);
    lastHeight_ = static_cast<int>(extent.height);

    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::fitToWindow()
{
    // Explicit user action (View > Fit to Window / F / Ctrl+0): "contain",
    // not "fill" - the whole thing should become visible, unlike the
    // fill-by-default auto-fit on load/image-switch (see fitContentToWindow()
    // callers).
    fitContentToWindow(/*fill=*/false);
}

void VulkanViewport::fitContentToWindow(bool fill)
{
    if (!hasScene_) return;
    double minXyd, minYyd, maxXyd, maxYyd;
    currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);
    fillFitActive_ = fill;
    fitToBounds(minXyd, -maxYyd, maxXyd, -minYyd, fill);
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

    if (minX > maxX || minY > maxY)
    {
        minX = -100.0;
        maxX = 100.0;
        minY = -100.0;
        maxY = 100.0;
    }
}

void VulkanViewport::updateScrollBars()
{
    // Every zoom/pan/resize/fit path in this file ends by calling
    // updateScrollBars(), so this is the one hook that keeps the caliper's
    // on-screen size pixel-constant even when the view changes without the
    // mouse moving (e.g. the Zoom In/Out menu, scroll-wheel zoom, a window
    // resize) - see CaliperTool::refresh().
    if (caliperTool_) caliperTool_->refresh();

    if (!hScrollBar_ || !vScrollBar_) return;
    if (!hasScene_ || !camera_ || !projection_ || !window_ || !window_->windowAdapter || fillFitActive_)
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
    double viewLeftPx = (lookAt->center.x - halfW - minXyd) * scaleX;
    applyScrollBarRange(hScrollBar_, contentWidthPx, static_cast<double>(extent.width), viewLeftPx);

    double contentHeightPx = (maxYyd - minYyd) * scaleY;
    double viewTopPx = (contentMaxY - (lookAt->center.y + halfH)) * scaleY;
    applyScrollBarRange(vScrollBar_, contentHeightPx, static_cast<double>(extent.height), viewTopPx);
}

void VulkanViewport::zoomIn()
{
    if (!projection_) return;
    double factor = 0.8;
    double halfW = (projection_->right - projection_->left) * 0.5 * factor;
    double halfH = (projection_->top - projection_->bottom) * 0.5 * factor;
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    fillFitActive_ = false; // manual zoom is a real departure from the resting fill-fit - see its doc comment
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::zoomOut()
{
    if (!projection_) return;
    double factor = 1.25;
    double halfW = (projection_->right - projection_->left) * 0.5 * factor;
    double halfH = (projection_->top - projection_->bottom) * 0.5 * factor;
    projection_->left = -halfW;
    projection_->right = halfW;
    projection_->bottom = -halfH;
    projection_->top = halfH;
    fillFitActive_ = false; // manual zoom is a real departure from the resting fill-fit - see its doc comment
    viewer_->request();
    updateScrollBars();
}

void VulkanViewport::toggleBalloon()
{
    if (!labelPicker_ || !balloonController_) return;
    balloonEnabled_ = !balloonEnabled_;
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
    if (viewer_->viewParams) viewer_->viewParams->setForceOpaque(forceOpaque_);
    viewer_->request();
}

void VulkanViewport::toggleMeasureDistance()
{
    measureEnabled_ = !measureEnabled_;
    if (caliperTool_) caliperTool_->setEnabled(measureEnabled_);
    viewer_->request();
}

void VulkanViewport::setBackgroundColor(const QColor& color)
{
    if (!window_ || !window_->windowAdapter) return;
    window_->windowAdapter->clearColor() = vsg::vec4(color.redF(), color.greenF(), color.blueF(), 1.0f);
    viewer_->request();
}

} // namespace givqt
