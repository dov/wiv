// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "PanZoomHandler.h"
#include "ViewFlip.h"

#include <QGuiApplication>

#include <algorithm>
#include <cmath>

namespace giv
{

PanZoomHandler::PanZoomHandler(vsg::ref_ptr<vsg::Camera> camera) : camera_(camera) {}

void PanZoomHandler::apply(vsg::ButtonPressEvent& event)
{
    // giv uses middle-mouse-button-drag to pan (standard image-viewer
    // convention); button 2 is the middle button in vsg's raw button
    // numbering (1=left, 2=middle, 3=right), matching vsg::BUTTON_MASK_2.
    if (event.button == 1 && onMarkZoom && (QGuiApplication::keyboardModifiers() & Qt::ControlModifier))
    {
        // Ctrl+left-drag scales the pixel-sized marks.
        markZoomDragging_ = true;
        markZoomStartX_ = markZoomLastX_ = event.x;
        markZoomStartY_ = markZoomLastY_ = event.y;
        markZoomAxis_ = MarkZoomAxis::Undecided;
        event.handled = true; // keep other tools (caliper) from starting a gesture
    }
    else if (event.button == 2 || (event.button == 1 && leftDragPans && leftDragPans()))
    {
        dragging_ = true;
        dragButton_ = event.button;
        lastX_ = event.x;
        lastY_ = event.y;
    }
    else if (event.button == 3 && onLineWidthZoom && (QGuiApplication::keyboardModifiers() & Qt::ControlModifier))
    {
        // Ctrl+right-drag scales line widths instead of zooming the canvas.
        lineWidthDragging_ = true;
        lineWidthStartX_ = lineWidthLastX_ = event.x;
        lineWidthStartY_ = lineWidthLastY_ = event.y;
        lineWidthAxis_ = MarkZoomAxis::Undecided;
        event.handled = true;
    }
    else if (event.button == 3)
    {
        zoomActive_ = false;
        zoomDragging_ = true;
        zoomAnchorX_ = event.x;
        zoomAnchorY_ = event.y;
        zoomLastY_ = event.y;
    }

    if (onClick)
    {
        auto window = event.window.ref_ptr();
        double worldX = 0.0, worldY = 0.0;
        if (window && screenToWorld(event.x, event.y, window, worldX, worldY))
            onClick(worldX, worldY, static_cast<int>(event.button),
                    static_cast<int>(QGuiApplication::keyboardModifiers()));
    }
}

void PanZoomHandler::apply(vsg::ButtonReleaseEvent& event)
{
    if (event.button == dragButton_) dragging_ = false;
    if (event.button == 3) zoomDragging_ = false;
    if (event.button == 3) lineWidthDragging_ = false;
    if (event.button == 1) markZoomDragging_ = false;
}

void PanZoomHandler::apply(vsg::MoveEvent& event)
{
    auto window = event.window.ref_ptr();
    if (window && onCursorMove)
    {
        double worldX = 0.0, worldY = 0.0;
        if (screenToWorld(event.x, event.y, window, worldX, worldY)) onCursorMove(worldX, worldY);
    }

    if (!window) return;

    // A ButtonReleaseEvent can be lost - e.g. a right/middle-button drag that
    // ends off this window (over a scrollbar, a menu, outside the app
    // entirely) never delivers its release back here, since nothing grabs
    // the pointer for the duration of the drag - which would otherwise latch
    // dragging_/zoomDragging_ true forever, turning every future mouse move
    // (not just deliberate drags) into an unwanted pan/zoom. event.mask
    // reports which buttons are *actually* still down on every move, so
    // resync against it rather than trusting the flags alone.
    if (dragging_ && !(event.mask & (dragButton_ == 1 ? vsg::BUTTON_MASK_1 : vsg::BUTTON_MASK_2))) dragging_ = false;
    if (zoomDragging_ && !(event.mask & vsg::BUTTON_MASK_3)) zoomDragging_ = false;

    if (markZoomDragging_ && !(event.mask & vsg::BUTTON_MASK_1)) markZoomDragging_ = false;
    if (lineWidthDragging_ && !(event.mask & vsg::BUTTON_MASK_3)) lineWidthDragging_ = false;

    if (lineWidthDragging_)
    {
        constexpr int32_t kAxisLockPixels = 5;
        if (lineWidthAxis_ == MarkZoomAxis::Undecided)
        {
            int32_t totalDx = event.x - lineWidthStartX_;
            int32_t totalDy = event.y - lineWidthStartY_;
            if (std::max(std::abs(totalDx), std::abs(totalDy)) >= kAxisLockPixels)
            {
                lineWidthAxis_ = std::abs(totalDy) >= std::abs(totalDx) ? MarkZoomAxis::Vertical : MarkZoomAxis::Horizontal;
                lineWidthLastX_ = lineWidthStartX_;
                lineWidthLastY_ = lineWidthStartY_;
            }
        }
        if (lineWidthAxis_ != MarkZoomAxis::Undecided)
        {
            const double speed = shiftHeld() ? 5.0 : 1.0;
            if (lineWidthAxis_ == MarkZoomAxis::Horizontal)
            {
                int32_t widthDx = event.x - lineWidthLastX_;
                lineWidthLastX_ = event.x;
                if (widthDx != 0 && onLineWidthZoom) onLineWidthZoom(std::pow(1.01, static_cast<double>(widthDx) * speed));
            }
            else
            {
                int32_t alphaDy = event.y - lineWidthLastY_;
                lineWidthLastY_ = event.y;
                if (alphaDy != 0 && onAlphaZoom) onAlphaZoom(std::pow(1.01, -static_cast<double>(alphaDy) * speed));
            }
        }
    }

    if (markZoomDragging_)
    {
        // Axis lock: the first kAxisLockPixels of motion pick vertical (mark
        // size) or horizontal (quiver length); the other axis is then ignored
        // until the button is released, so a sloppy diagonal drag changes
        // only one thing.
        constexpr int32_t kAxisLockPixels = 5;
        if (markZoomAxis_ == MarkZoomAxis::Undecided)
        {
            int32_t totalDx = event.x - markZoomStartX_;
            int32_t totalDy = event.y - markZoomStartY_;
            if (std::max(std::abs(totalDx), std::abs(totalDy)) >= kAxisLockPixels)
            {
                markZoomAxis_ = std::abs(totalDy) >= std::abs(totalDx) ? MarkZoomAxis::Vertical : MarkZoomAxis::Horizontal;
                // Count the motion so far, so the drag doesn't lose its first pixels.
                markZoomLastX_ = markZoomStartX_;
                markZoomLastY_ = markZoomStartY_;
            }
        }
        if (markZoomAxis_ != MarkZoomAxis::Undecided)
        {
            const double speed = shiftHeld() ? 5.0 : 1.0;
            if (markZoomAxis_ == MarkZoomAxis::Vertical)
            {
                int32_t markDy = event.y - markZoomLastY_;
                markZoomLastY_ = event.y;
                if (markDy != 0 && onMarkZoom) onMarkZoom(std::pow(1.01, -static_cast<double>(markDy) * speed));
            }
            else
            {
                int32_t markDx = event.x - markZoomLastX_;
                markZoomLastX_ = event.x;
                if (markDx != 0 && onQuiverZoom) onQuiverZoom(std::pow(1.01, static_cast<double>(markDx) * speed));
            }
        }
    }

    int32_t dx = event.x - lastX_;
    int32_t dy = event.y - lastY_;
    lastX_ = event.x;
    lastY_ = event.y;

    // lastX_/lastY_ must stay current on every move (not just while dragging)
    // since ScrollWheelEvent carries no cursor position of its own and relies
    // on these for the zoom-at-cursor anchor.
    if (zoomDragging_)
    {
        int32_t zoomDy = event.y - zoomLastY_;
        zoomLastY_ = event.y;
        if (zoomDy != 0)
        {
            // Dragging up (dy<0) zooms in, dragging down zooms out, at a
            // rate tuned to feel smooth over typical drag distances.
            double speed = shiftHeld() ? 5.0 : 1.0;
            double amount = -static_cast<double>(zoomDy) * 0.01 * speed;
            double factor = std::pow(0.9, amount);
            zoom(factor, zoomAnchorX_, zoomAnchorY_, extentOf(window));
            if (onViewChanged) onViewChanged();
        }
    }

    if (!dragging_) return;

    if (dx == 0 && dy == 0) return;
    pan(dx, dy, extentOf(window));
    if (onViewChanged) onViewChanged();
}

void PanZoomHandler::apply(vsg::ScrollWheelEvent& event)
{
    auto window = event.window.ref_ptr();
    if (!window) return;

    // delta.y > 0 is scroll-up (zoom in); delta.z > 0 is scroll-out (zoom out)
    // per vsg::ScrollWheelEvent's documented convention.
    double amount = static_cast<double>(event.delta.y) - static_cast<double>(event.delta.z);
    if (amount == 0.0) return;

    double factor = std::pow(0.9, amount * (shiftHeld() ? 5.0 : 1.0));

    // vsg doesn't give pointer position on ScrollWheelEvent, so fall back to
    // lastX_/lastY_, which apply(MoveEvent&) keeps current on every hover
    // move (not just while dragging); (0,0) only if a scroll arrives before
    // any move has ever been reported, which zooms around the top-left.
    zoomAbout(factor, lastX_, lastY_, extentOf(window));
}

void PanZoomHandler::zoomAbout(double factor, int32_t x, int32_t y, const VkExtent2D& extent, bool animate,
                               bool replaceMotion)
{
    if (!animate || zoomDuration_ <= 0.0)
    {
        zoomActive_ = false;
        zoom(factor, x, y, extent);
        if (onViewChanged) onViewChanged();
        return;
    }

    // A new motion from the present view to the destination of the previous
    // one (if it is still under way) zoomed once more.
    zoomTotal_ = (zoomActive_ && !replaceMotion ? zoomTotal_ / zoomApplied_ : 1.0) * factor;
    zoomApplied_ = 1.0;
    zoomX_ = x;
    zoomY_ = y;
    zoomExtent_ = extent;
    zoomStart_ = Clock::now();
    zoomActive_ = true;
    if (onZoomStarted) onZoomStarted();
}

double PanZoomHandler::zoomProfile(double u)
{
    constexpr double a = 0.25;
    constexpr double v = 1.0 / (1.0 - a);
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    if (u < a) return v * u * u / (2.0 * a);
    if (u > 1.0 - a) return 1.0 - v * (1.0 - u) * (1.0 - u) / (2.0 * a);
    return v * (0.5 * a + (u - a));
}

bool PanZoomHandler::advance()
{
    if (!zoomActive_) return false;
    const double elapsed = std::chrono::duration<double>(Clock::now() - zoomStart_).count();
    const double u = std::min(elapsed / zoomDuration_, 1.0);
    const double wanted = std::pow(zoomTotal_, zoomProfile(u));
    zoom(wanted / zoomApplied_, zoomX_, zoomY_, zoomExtent_);
    zoomApplied_ = wanted;
    if (u >= 1.0) zoomActive_ = false;
    if (onViewChanged) onViewChanged();
    return zoomActive_;
}

bool PanZoomHandler::screenToWorld(int32_t screenX, int32_t screenY, const vsg::ref_ptr<vsg::Window>& window,
                                    double& outX, double& outY) const
{
    auto extent = extentOf(window);
    auto ortho = camera_->projectionMatrix.cast<vsg::Orthographic>();
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    if (!ortho || !lookAt || extent.width == 0 || extent.height == 0) return false;

    double worldPerPixelX = (ortho->right - ortho->left) / static_cast<double>(extent.width);
    double worldPerPixelY = (ortho->top - ortho->bottom) / static_cast<double>(extent.height);
    const vsg::dvec2 o = screenToViewOffset(camera_, (static_cast<double>(screenX) - extent.width * 0.5) * worldPerPixelX,
                                            (extent.height * 0.5 - static_cast<double>(screenY)) * worldPerPixelY);
    outX = lookAt->center.x + o.x;
    outY = -(lookAt->center.y + o.y);
    return true;
}

VkExtent2D PanZoomHandler::extentOf(const vsg::ref_ptr<vsg::Window>& window) const
{
    return viewportExtent ? viewportExtent() : window->extent2D();
}

bool PanZoomHandler::shiftHeld()
{
    // Queried directly from Qt rather than tracked via vsg key events -
    // vsgQt's keyboard map only maps Qt::Key_Shift (which Qt reports for
    // both physical shift keys) to KEY_Shift_L, with a distinct right-shift
    // vsg keysym left unimplemented.
    return QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
}

void PanZoomHandler::pan(int32_t dxPix, int32_t dyPix, const VkExtent2D& extent)
{
    auto ortho = camera_->projectionMatrix.cast<vsg::Orthographic>();
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    if (!ortho || !lookAt || extent.width == 0 || extent.height == 0) return;

    double worldPerPixelX = (ortho->right - ortho->left) / static_cast<double>(extent.width);
    double worldPerPixelY = (ortho->top - ortho->bottom) / static_cast<double>(extent.height);

    const vsg::dvec2 o =
        screenToViewOffset(camera_, static_cast<double>(dxPix) * worldPerPixelX, -static_cast<double>(dyPix) * worldPerPixelY);

    vsg::dvec3 shift(-o.x, -o.y, 0.0);
    lookAt->eye += shift;
    lookAt->center += shift;
}

void PanZoomHandler::zoom(double factor, int32_t screenX, int32_t screenY, const VkExtent2D& extent)
{
    auto ortho = camera_->projectionMatrix.cast<vsg::Orthographic>();
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    if (!ortho || !lookAt || extent.width == 0 || extent.height == 0) return;

    double halfW = (ortho->right - ortho->left) * 0.5;
    double halfH = (ortho->top - ortho->bottom) * 0.5;

    double sxNorm = 2.0 * static_cast<double>(screenX) / static_cast<double>(extent.width) - 1.0;
    double syNorm = 1.0 - 2.0 * static_cast<double>(screenY) / static_cast<double>(extent.height);

    // giv's vertical zoom lock: zoom only along X, leaving the Y scale and
    // pan untouched.
    const vsg::dvec2 o = screenToViewOffset(camera_, halfW * sxNorm * (1.0 - factor),
                                            verticalLock ? 0.0 : halfH * syNorm * (1.0 - factor));

    vsg::dvec3 shift(o.x, o.y, 0.0);
    lookAt->eye += shift;
    lookAt->center += shift;

    halfW *= factor;
    if (!verticalLock) halfH *= factor;

    // Clamp to avoid degenerate/inverted extents on extreme zoom-in.
    halfW = std::max(halfW, 1e-6);
    halfH = std::max(halfH, 1e-6);

    ortho->left = -halfW;
    ortho->right = halfW;
    ortho->bottom = -halfH;
    ortho->top = halfH;
}

} // namespace giv
