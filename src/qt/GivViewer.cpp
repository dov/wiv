#include "GivViewer.h"

#include <QCoreApplication>

namespace givqt
{

void GivViewer::render(double simulationTime)
{
    if (!continuousUpdate && requests.load() == 0) return;

    // Keep pixel-constant mark/line sizes in sync with the current zoom
    // level - see SceneBuilder.h's PixelSizeAnimator doc comment. Mirrors
    // the old main.cpp's updateMarkSizes() lambda, called once per frame.
    if (worldPerPixel)
    {
        float wpp = worldPerPixel();
        if (markSizeAnimator) markSizeAnimator->update(wpp);
        if (lineWidthAnimator) lineWidthAnimator->update(wpp);
        if (labelLineWidthAnimator) labelLineWidthAnimator->update(wpp);
        if (arrowVertexAnimator) arrowVertexAnimator->update(wpp);
    }

    if (advanceToNextFrame(simulationTime))
    {
        handleEvents();
        update();
        if (balloonOverlay && !windows().empty()) balloonOverlay->updateExtent(windows().front()->extent2D());
        recordAndSubmit();
        present();

        // Reads back this frame's off-screen label render (if the balloon
        // is toggled on) and updates the overlay for the *next* frame - see
        // BalloonController::update()'s doc comment for the resulting
        // one-frame latency.
        if (balloonController) balloonController->update(this);
    }
    else
    {
        if (status->cancel()) QCoreApplication::quit();
    }

    ++frameCount_;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - fpsWindowStart_).count();
    if (elapsed >= 1.0)
    {
        if (onFrameStats) onFrameStats(frameCount_ / elapsed);
        frameCount_ = 0;
        fpsWindowStart_ = now;
    }

    requests = 0;
}

} // namespace givqt
