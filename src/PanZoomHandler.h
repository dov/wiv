#pragma once
//
// PanZoomHandler.h - drag-to-pan / scroll-to-zoom-at-cursor for a 2D
// Orthographic + LookAt camera. No rotation logic at all (VSG has no
// built-in 2D pan/zoom manipulator - Trackball is 3D-orbit only).
//
#include <vsg/all.h>

#include <functional>

namespace giv
{

class PanZoomHandler : public vsg::Inherit<vsg::Visitor, PanZoomHandler>
{
public:
    explicit PanZoomHandler(vsg::ref_ptr<vsg::Camera> camera);

    void apply(vsg::ButtonPressEvent& event) override;
    void apply(vsg::ButtonReleaseEvent& event) override;
    void apply(vsg::MoveEvent& event) override;
    void apply(vsg::ScrollWheelEvent& event) override;

    // Called (if set) from every MoveEvent with the cursor's current world-
    // space (x,y) position, for a status-bar readout - purely observational,
    // no effect on pan/zoom behavior.
    std::function<void(double, double)> onCursorMove;

    // Called (if set) after a drag-pan or scroll-wheel-zoom actually changes
    // the camera, so a caller can keep external UI (scrollbars) in sync.
    std::function<void()> onViewChanged;

    // Called (if set) on every ButtonPressEvent with the click's world-space
    // (x,y), the vsg button number (1=left, 2=middle, 3=right), and the Qt
    // keyboard-modifiers bitmask - feeds the json-rpc pick_coordinate command.
    std::function<void(double, double, int, int)> onClick;

private:
    vsg::ref_ptr<vsg::Camera> camera_;
    bool dragging_ = false;
    int32_t lastX_ = 0;
    int32_t lastY_ = 0;

    // Right-mouse-button drag-to-zoom: the anchor (mouse-down position)
    // stays fixed for the whole drag so the zoom always pivots around
    // where the drag started, not wherever the cursor currently is.
    bool zoomDragging_ = false;
    int32_t zoomAnchorX_ = 0;
    int32_t zoomAnchorY_ = 0;
    int32_t zoomLastY_ = 0;

    void pan(int32_t dxPix, int32_t dyPix, const VkExtent2D& extent);
    void zoom(double factor, int32_t screenX, int32_t screenY, const VkExtent2D& extent);
    static bool shiftHeld();

    // Shared with apply(MoveEvent&)'s cursor-readout math: screen pixel ->
    // world-space (x,y), y-flipped to match the rest of the app's convention.
    bool screenToWorld(int32_t screenX, int32_t screenY, const vsg::ref_ptr<vsg::Window>& window,
                        double& outX, double& outY) const;
};

} // namespace giv
