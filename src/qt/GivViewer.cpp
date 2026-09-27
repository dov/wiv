#include "GivViewer.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

namespace givqt
{

namespace
{
// WIV_TIMING=1 breaks each frame down into its per-phase CPU cost and
// prints the per-frame averages once a second - benchmarking only.
const bool kTiming = std::getenv("WIV_TIMING") != nullptr;
using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
} // namespace

void GivViewer::render(double simulationTime)
{
    // Left pending (not consumed below) rather than dropped, so whatever
    // requested this frame still gets one once the resize settles - see
    // isResizeSettling's doc comment.
    if (isResizeSettling && isResizeSettling()) return;
    if (!continuousUpdate && requests.load() == 0) return;
    auto tPhase = Clock::now();

    // Hand this frame's zoom level to the vertex shaders, which size
    // pixel-constant marks/lines/arrowheads from it - see giv::ViewParams -
    // and pick each $image's nearest/linear sampler from it.
    if (worldPerPixel)
    {
        float wpp = worldPerPixel();
        if (viewParams) viewParams->update(wpp);
        if (imageFilterAnimator) imageFilterAnimator->update(wpp);
    }

    if (kTiming) { tAnimate_ += msSince(tPhase); tPhase = Clock::now(); }

    if (advanceToNextFrame(simulationTime))
    {
        if (kTiming) { tAdvance_ += msSince(tPhase); tPhase = Clock::now(); }
        handleEvents();
        update();
        if (kTiming) { tUpdate_ += msSince(tPhase); tPhase = Clock::now(); }
        recordAndSubmit();
        if (kTiming) { tRecord_ += msSince(tPhase); tPhase = Clock::now(); }
        present();
        if (kTiming) { tPresent_ += msSince(tPhase); tPhase = Clock::now(); }

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

    if (kTiming)
    {
        ++frameCount_;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fpsWindowStart_).count();
        if (elapsed >= 1.0 && frameCount_ > 0)
        {
            double n = static_cast<double>(frameCount_);
            std::cerr << "wiv: per-frame ms: animate=" << tAnimate_ / n << " advance=" << tAdvance_ / n
                      << " update=" << tUpdate_ / n << " record=" << tRecord_ / n << " present=" << tPresent_ / n << "\n";
            tAnimate_ = tAdvance_ = tUpdate_ = tRecord_ = tPresent_ = 0.0;
            frameCount_ = 0;
            fpsWindowStart_ = now;
        }
    }

    requests = 0;
}

} // namespace givqt
