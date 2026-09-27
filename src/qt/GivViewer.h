#pragma once
//
// GivViewer.h - vsgQt::Viewer subclass that layers wiv's per-frame work
// (per-frame view-scale uniform update, balloon-tooltip readback, timing
// diagnostics) onto vsgQt's QTimer-driven render() loop, replacing the manual
// while(viewer->advanceToNextFrame()) loop wiv used before it had a Qt
// event loop to drive it (see the old src/main.cpp).
//
#include "BalloonController.h"
#include "SceneBuilder.h"

#include <vsgQt/Viewer.h>

#include <chrono>
#include <functional>

namespace givqt
{

class GivViewer : public vsg::Inherit<vsgQt::Viewer, GivViewer>
{
public:
    using vsg::Inherit<vsgQt::Viewer, GivViewer>::Inherit;

    // Re-pointed by VulkanViewport every time a new file is loaded.
    vsg::ref_ptr<giv::BalloonController> balloonController;
    vsg::ref_ptr<giv::ViewParams> viewParams;
    vsg::ref_ptr<giv::ImageFilterAnimator> imageFilterAnimator;

    // Called once per frame with the current world-units-per-pixel scale, to
    // drive updateMarkSizes()'s callers - set by VulkanViewport since only it
    // knows the active vsg::Orthographic projection.
    std::function<float()> worldPerPixel;

    // Set by VulkanViewport; when non-null and returning true, render() skips
    // advancing/recording/submitting a frame this tick. window_->resize()
    // (called from VulkanViewport::resizeEvent()) only *posts* an X11
    // ConfigureRequest - windowAdapter->extent2D() keeps reporting the old
    // extent, and the swapchain isn't actually rebuilt, until the
    // ConfigureNotify round-trip completes asynchronously, on some later
    // iteration of this same timer-driven render loop (see
    // VulkanViewport::ensureExtentSettled()'s doc comment). A frame recorded
    // and submitted against the still-old swapchain in that window can still
    // be in flight when the round-trip lands and vsgQt::Window::resizeEvent()
    // rebuilds it - which was observed to hit vkDestroyFramebuffer/
    // RenderPass/Image "still in use by VkCommandBuffer" validation errors,
    // plus corrupted acquire/present semaphore state on the frames after
    // that. Holding off here until the extent actually matches what was
    // requested avoids the race instead of racing it.
    std::function<bool()> isResizeSettling;

    void render(double simulationTime = vsg::Viewer::UseTimeSinceStartPoint) override;

private:
    size_t frameCount_ = 0;
    double tAnimate_ = 0.0, tAdvance_ = 0.0, tUpdate_ = 0.0, tRecord_ = 0.0, tPresent_ = 0.0;
    std::chrono::steady_clock::time_point fpsWindowStart_ = std::chrono::steady_clock::now();
};

} // namespace givqt
