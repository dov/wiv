#include "VulkanViewport.h"

#include "GivParser.h"
#include "ImagePluginHost.h"

#include <vsg/all.h>
#include <vsgXchange/all.h>
#include <vsgXchange/freetype.h>

#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace givqt
{

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
    // first/cleared frame forever). MAILBOX never blocks the caller, so a
    // slow/stuck compositor handshake can't freeze rendering.
    traits_->swapchainPreferences.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

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
    // requests - only for benchmarking the render path (the fps readout is
    // meaningless in the default on-demand mode, which idles at 0 fps).
    viewer_->continuousUpdate = std::getenv("WIV_CONTINUOUS") != nullptr;

    viewer_->worldPerPixel = [this]() -> float {
        if (!projection_ || !window_ || !window_->windowAdapter) return 1.0f;
        auto extent = window_->windowAdapter->extent2D();
        if (extent.width == 0) return 1.0f;
        return static_cast<float>((projection_->right - projection_->left) / static_cast<double>(extent.width));
    };
    viewer_->onFrameStats = [this](double fps) {
        std::cerr << "wiv: " << fps << " fps\n";
        emit frameStats(fps);
    };
    viewer_->addEventHandler(vsg::CloseHandler::create(viewer_));

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

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    container_ = QWidget::createWindowContainer(window_, this);
    layout->addWidget(container_);
    setLayout(layout);

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

    // Resolve and load $image references. Mirrors giv's own
    // cb_image_reference: a filename is used as-is if it already resolves
    // (absolute, or relative to the current working directory); otherwise
    // it's retried relative to the directory of the first loaded .giv file.
    std::vector<giv::LoadedImage> loadedImages;
    std::vector<std::string> newLoadedImageNames;
    std::vector<std::pair<double, double>> newLoadedImageSizes;
    std::filesystem::path givDir = paths.empty() ? std::filesystem::path() : std::filesystem::path(paths.front()).parent_path();
    for (const auto& imgRef : newScene.images)
    {
        std::filesystem::path resolved(imgRef);
        if (!std::filesystem::exists(resolved) && !givDir.empty())
            resolved = givDir / imgRef;

        auto loaded = giv::ImagePluginHost::load(resolved.string());
        if (!loaded)
            continue;

        newLoadedImageNames.push_back(resolved.string());
        newLoadedImageSizes.emplace_back(static_cast<double>(loaded->width), static_cast<double>(loaded->height));
        loadedImages.push_back(std::move(*loaded));
    }

    scene_ = std::move(newScene);
    loadedImages_ = std::move(loadedImages);
    loadedImageNames_ = std::move(newLoadedImageNames);
    loadedImageSizes_ = std::move(newLoadedImageSizes);
    hasScene_ = true;
    currentImageIndex_ = 0;

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

    giv::SceneBuilder builder(options_);
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
    if (!isInitialLoad) viewer_->deviceWaitIdle();

    // Drop the previous build's event handlers (pan/zoom + balloon) but keep
    // the CloseHandler added once in the constructor.
    auto& handlers = viewer_->getEventHandlers();
    handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                   [](const vsg::ref_ptr<vsg::Visitor>& h) { return h.cast<giv::PanZoomHandler>() || h.cast<giv::BalloonController>(); }),
                   handlers.end());

    if (isInitialLoad)
    {
        double minXyd, minYyd, maxXyd, maxYyd;
        currentFitBoundsYDown(minXyd, minYyd, maxXyd, maxYyd);

        auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 1.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
        projection_ = vsg::Orthographic::create(-100.0, 100.0, -100.0, 100.0, 0.01, 100.0);
        camera_ = vsg::Camera::create(projection_, lookAt, vsg::ViewportState::create(window_->windowAdapter->extent2D()));

        fitToBounds(minXyd, -maxYyd, maxXyd, -minYyd);
    }

    panZoom_ = giv::PanZoomHandler::create(camera_);
    panZoom_->onCursorMove = [this](double x, double y) { emit cursorWorldPosition(x, y); };
    viewer_->addEventHandler(panZoom_);

    bool wasBalloonEnabled = isInitialLoad ? false : balloonEnabled_;

    labelPicker_ = giv::LabelPicker::create(window_->windowAdapter, camera_, builder.labelGraph());
    balloonOverlay_ = giv::BalloonOverlay::create(options_, shaderDir_);
    balloonController_ = giv::BalloonController::create(&scene_, labelPicker_, balloonOverlay_);
    viewer_->addEventHandler(balloonController_);
    viewer_->balloonController = balloonController_;
    viewer_->balloonOverlay = balloonOverlay_;

    viewer_->viewParams = builder.viewParams();
    viewer_->imageFilterAnimator = builder.imageFilterAnimator();

    imageSwitch_ = builder.imageSwitch();
    if (imageSwitch_) imageSwitch_->setSingleChildOn(static_cast<size_t>(currentImageIndex_));

    auto renderGraph = vsg::createRenderGraphForView(window_->windowAdapter, camera_, sceneGraph);
    renderGraph->addChild(balloonOverlay_->view());
    auto commandGraph = vsg::CommandGraph::create(window_->windowAdapter);
    commandGraph->addChild(renderGraph);

    viewer_->assignRecordAndSubmitTaskAndPresentation({labelPicker_->commandGraph(), commandGraph});
    viewer_->compile();

    balloonEnabled_ = wasBalloonEnabled;
    balloonController_->setEnabled(wasBalloonEnabled);
    labelPicker_->setEnabled(wasBalloonEnabled);
    if (!wasBalloonEnabled) balloonOverlay_->hide();

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
    rebuildSceneGraph(nullptr, /*isInitialLoad=*/false);
}

void VulkanViewport::setShowMarks(bool show)
{
    if (globalShowMarks_ == show) return;
    globalShowMarks_ = show;
    if (hasScene_) rebuildSceneGraph(nullptr, /*isInitialLoad=*/false);
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
    if (loadedImageNames_.size() < 2 || !imageSwitch_) return;
    currentImageIndex_ = (currentImageIndex_ + 1) % static_cast<int>(loadedImageNames_.size());
    imageSwitch_->setSingleChildOn(static_cast<size_t>(currentImageIndex_));
    fitToWindow(); // matches giv's do_auto_fit_marks (default on): re-fit on every image switch
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
}

void VulkanViewport::previousImage()
{
    if (loadedImageNames_.size() < 2 || !imageSwitch_) return;
    currentImageIndex_ = (currentImageIndex_ - 1 + static_cast<int>(loadedImageNames_.size())) % static_cast<int>(loadedImageNames_.size());
    imageSwitch_->setSingleChildOn(static_cast<size_t>(currentImageIndex_));
    fitToWindow(); // matches giv's do_auto_fit_marks (default on): re-fit on every image switch
    emit imageChanged(currentImageIndex_, imageCount(), QString::fromStdString(currentImageName()));
}

void VulkanViewport::fitToBounds(double minX, double minY, double maxX, double maxY)
{
    if (!projection_ || !window_ || !window_->windowAdapter) return;

    double centerX = (minX + maxX) * 0.5;
    double centerY = (minY + maxY) * 0.5;

    double dataW = std::max(maxX - minX, 2e-3);
    double dataH = std::max(maxY - minY, 2e-3);
    auto extent = window_->windowAdapter->extent2D();
    double canvasW = static_cast<double>(extent.width);
    double canvasH = static_cast<double>(extent.height);

    double scaleX = (canvasW - 2.0 * autoFitMarginPx_) / dataW;
    double scaleY = (canvasH - 2.0 * autoFitMarginPx_) / dataH;
    double scale = std::min(scaleX, scaleY);
    if (scale <= 0.0) scale = std::min(canvasW, canvasH) / std::max(dataW, dataH);

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
}

void VulkanViewport::fitToWindow()
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

    if (currentImageIndex_ >= 0 && currentImageIndex_ < static_cast<int>(loadedImageSizes_.size()))
    {
        const auto& size = loadedImageSizes_[currentImageIndex_];
        minX = std::min(minX, 0.0);
        maxX = std::max(maxX, size.first);
        minY = std::min(minY, 0.0);
        maxY = std::max(maxY, size.second);
    }

    if (minX > maxX || minY > maxY)
    {
        minX = -100.0;
        maxX = 100.0;
        minY = -100.0;
        maxY = 100.0;
    }
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
    viewer_->request();
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
    viewer_->request();
}

void VulkanViewport::toggleBalloon()
{
    if (!labelPicker_ || !balloonOverlay_ || !balloonController_) return;
    balloonEnabled_ = !balloonEnabled_;
    balloonController_->setEnabled(balloonEnabled_);
    viewer_->request();
}

bool VulkanViewport::balloonEnabled() const
{
    return balloonEnabled_;
}

void VulkanViewport::setBackgroundColor(const QColor& color)
{
    if (!window_ || !window_->windowAdapter) return;
    window_->windowAdapter->clearColor() = vsg::vec4(color.redF(), color.greenF(), color.blueF(), 1.0f);
    viewer_->request();
}

void VulkanViewport::setAutoFitMarginPx(double px)
{
    autoFitMarginPx_ = px;
}

} // namespace givqt
