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

private:
    vsg::ref_ptr<vsg::Camera> camera_;
    bool dragging_ = false;
    int32_t lastX_ = 0;
    int32_t lastY_ = 0;

    void pan(int32_t dxPix, int32_t dyPix, const VkExtent2D& extent);
    void zoom(double factor, int32_t screenX, int32_t screenY, const VkExtent2D& extent);
};

} // namespace giv
