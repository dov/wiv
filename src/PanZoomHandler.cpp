#include "PanZoomHandler.h"

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
    if (event.button == 2)
    {
        dragging_ = true;
        lastX_ = event.x;
        lastY_ = event.y;
    }
}

void PanZoomHandler::apply(vsg::ButtonReleaseEvent& event)
{
    if (event.button == 2) dragging_ = false;
}

void PanZoomHandler::apply(vsg::MoveEvent& event)
{
    auto window = event.window.ref_ptr();
    if (window && onCursorMove)
    {
        auto extent = window->extent2D();
        auto ortho = camera_->projectionMatrix.cast<vsg::Orthographic>();
        auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
        if (ortho && lookAt && extent.width > 0 && extent.height > 0)
        {
            double worldPerPixelX = (ortho->right - ortho->left) / static_cast<double>(extent.width);
            double worldPerPixelY = (ortho->top - ortho->bottom) / static_cast<double>(extent.height);
            double worldX = lookAt->center.x + (static_cast<double>(event.x) - extent.width * 0.5) * worldPerPixelX;
            double worldY = -(lookAt->center.y + (extent.height * 0.5 - static_cast<double>(event.y)) * worldPerPixelY);
            onCursorMove(worldX, worldY);
        }
    }

    if (!window) return;

    int32_t dx = event.x - lastX_;
    int32_t dy = event.y - lastY_;
    lastX_ = event.x;
    lastY_ = event.y;

    // lastX_/lastY_ must stay current on every move (not just while dragging)
    // since ScrollWheelEvent carries no cursor position of its own and relies
    // on these for the zoom-at-cursor anchor.
    if (!dragging_) return;

    if (dx == 0 && dy == 0) return;
    pan(dx, dy, window->extent2D());
}

void PanZoomHandler::apply(vsg::ScrollWheelEvent& event)
{
    auto window = event.window.ref_ptr();
    if (!window) return;

    // delta.y > 0 is scroll-up (zoom in); delta.z > 0 is scroll-out (zoom out)
    // per vsg::ScrollWheelEvent's documented convention.
    double amount = static_cast<double>(event.delta.y) - static_cast<double>(event.delta.z);
    if (amount == 0.0) return;

    double factor = std::pow(0.9, amount);

    // vsg doesn't give pointer position on ScrollWheelEvent, so fall back to
    // lastX_/lastY_, which apply(MoveEvent&) keeps current on every hover
    // move (not just while dragging); (0,0) only if a scroll arrives before
    // any move has ever been reported, which zooms around the top-left.
    auto extent = window->extent2D();
    zoom(factor, lastX_, lastY_, extent);
}

void PanZoomHandler::pan(int32_t dxPix, int32_t dyPix, const VkExtent2D& extent)
{
    auto ortho = camera_->projectionMatrix.cast<vsg::Orthographic>();
    auto lookAt = camera_->viewMatrix.cast<vsg::LookAt>();
    if (!ortho || !lookAt || extent.width == 0 || extent.height == 0) return;

    double worldPerPixelX = (ortho->right - ortho->left) / static_cast<double>(extent.width);
    double worldPerPixelY = (ortho->top - ortho->bottom) / static_cast<double>(extent.height);

    double shiftX = -static_cast<double>(dxPix) * worldPerPixelX;
    double shiftY = static_cast<double>(dyPix) * worldPerPixelY;

    vsg::dvec3 shift(shiftX, shiftY, 0.0);
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

    double shiftX = halfW * sxNorm * (1.0 - factor);
    double shiftY = halfH * syNorm * (1.0 - factor);

    vsg::dvec3 shift(shiftX, shiftY, 0.0);
    lookAt->eye += shift;
    lookAt->center += shift;

    halfW *= factor;
    halfH *= factor;

    // Clamp to avoid degenerate/inverted extents on extreme zoom-in.
    halfW = std::max(halfW, 1e-6);
    halfH = std::max(halfH, 1e-6);

    ortho->left = -halfW;
    ortho->right = halfW;
    ortho->bottom = -halfH;
    ortho->top = halfH;
}

} // namespace giv
