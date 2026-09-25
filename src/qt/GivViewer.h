#pragma once
//
// GivViewer.h - vsgQt::Viewer subclass that layers wiv's per-frame work
// (pixel-constant mark/line-size animation, balloon-tooltip readback, fps
// accounting) onto vsgQt's QTimer-driven render() loop, replacing the manual
// while(viewer->advanceToNextFrame()) loop wiv used before it had a Qt
// event loop to drive it (see the old src/main.cpp).
//
#include "BalloonController.h"
#include "BalloonOverlay.h"
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
    vsg::ref_ptr<giv::BalloonOverlay> balloonOverlay;
    vsg::ref_ptr<giv::PixelSizeAnimator> markSizeAnimator;
    vsg::ref_ptr<giv::PixelSizeAnimator> lineWidthAnimator;
    vsg::ref_ptr<giv::PixelSizeAnimator> labelLineWidthAnimator;
    vsg::ref_ptr<giv::ArrowVertexAnimator> arrowVertexAnimator;
    vsg::ref_ptr<giv::ImageFilterAnimator> imageFilterAnimator;

    // Called once per frame with the current world-units-per-pixel scale, to
    // drive updateMarkSizes()'s callers - set by VulkanViewport since only it
    // knows the active vsg::Orthographic projection.
    std::function<float()> worldPerPixel;

    // Called once per frame with the just-computed instantaneous fps, for a
    // status-bar readout.
    std::function<void(double)> onFrameStats;

    void render(double simulationTime = vsg::Viewer::UseTimeSinceStartPoint) override;

private:
    size_t frameCount_ = 0;
    std::chrono::steady_clock::time_point fpsWindowStart_ = std::chrono::steady_clock::now();
};

} // namespace givqt
