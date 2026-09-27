#include "PanZoomHandler.h"

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
    if (event.button == 2)
    {
        dragging_ = true;
        lastX_ = event.x;
        lastY_ = event.y;
    }
    else if (event.button == 3)
    {
        zoomDragging_ = true;
        zoomAnchorX_ = event.x;
        zoomAnchorY_ = event.y;
        zoomLastY_ = event.y;
    }
}

void PanZoomHandler::apply(vsg::ButtonReleaseEvent& event)
{
    if (event.button == 2) dragging_ = false;
    if (event.button == 3) zoomDragging_ = false;
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

    // A ButtonReleaseEvent can be lost - e.g. a right/middle-button drag that
    // ends off this window (over a scrollbar, a menu, outside the app
    // entirely) never delivers its release back here, since nothing grabs
    // the pointer for the duration of the drag - which would otherwise latch
    // dragging_/zoomDragging_ true forever, turning every future mouse move
    // (not just deliberate drags) into an unwanted pan/zoom. event.mask
    // reports which buttons are *actually* still down on every move, so
    // resync against it rather than trusting the flags alone.
    if (dragging_ && !(event.mask & vsg::BUTTON_MASK_2)) dragging_ = false;
    if (zoomDragging_ && !(event.mask & vsg::BUTTON_MASK_3)) zoomDragging_ = false;

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
            zoom(factor, zoomAnchorX_, zoomAnchorY_, window->extent2D());
            if (onViewChanged) onViewChanged();
        }
    }

    if (!dragging_) return;

    if (dx == 0 && dy == 0) return;
    pan(dx, dy, window->extent2D());
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
    auto extent = window->extent2D();
    zoom(factor, lastX_, lastY_, extent);
    if (onViewChanged) onViewChanged();
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
