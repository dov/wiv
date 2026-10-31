#pragma once
//
// PanZoomHandler.h - drag-to-pan / scroll-to-zoom-at-cursor for a 2D
// Orthographic + LookAt camera. No rotation logic at all (VSG has no
// built-in 2D pan/zoom manipulator - Trackball is 3D-orbit only).
//
#include <vsg/all.h>

#include <chrono>
#include <algorithm>
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

    // Called (if set) when a wheel step starts or retargets a zoom motion, so
    // the owner can call advance() every frame until it returns false.
    std::function<void()> onZoomStarted;

    // The duration of the motion a wheel step zooms with: it accelerates for
    // a quarter of it, keeps its speed for half, and decelerates for the last
    // quarter. 0 zooms at once.
    void setZoomDuration(double seconds) { zoomDuration_ = std::max(0.0, seconds); }
    double zoomDuration() const { return zoomDuration_; }

    // Moves the view along the zoom motion to the present time. Returns
    // whether the motion goes on, in which case another frame is needed.
    bool advance();

    // Zooms the view by `factor` (<1 zooms in, as everywhere in this file)
    // about the screen point (x, y), in device pixels. `animate` runs it over
    // the zoom duration like a wheel step; otherwise it is applied at once
    // (pinch gestures, which are already continuous). `replaceMotion` makes
    // an animated zoom start from the present view rather than compound with
    // the destination of one under way - for callers that computed `factor`
    // against the present view's scale.
    void zoomAbout(double factor, int32_t x, int32_t y, const VkExtent2D& extent, bool animate = true,
                   bool replaceMotion = false);

    // Pans as a mouse drag of (dxPix, dyPix) device pixels would.
    void panByPixels(int32_t dxPix, int32_t dyPix, const VkExtent2D& extent) { pan(dxPix, dyPix, extent); }

    // Stops a zoom motion under way, leaving the view where it is. To be
    // called when something else sets the view.
    void cancelZoom() { zoomActive_ = false; }

    // giv's View > Vertical Zoom Lock: wheel/drag zoom changes only the X scale.
    bool verticalLock = false;

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

    // The zoom motion: the view moves from where it was at zoomStart_ by the
    // factor zoomTotal_ (a power of the profile) about the screen point
    // zoomX_/zoomY_; zoomApplied_ is the part of it applied so far.
    using Clock = std::chrono::steady_clock;
    double zoomDuration_ = 0.25;
    bool zoomActive_ = false;
    Clock::time_point zoomStart_;
    double zoomTotal_ = 1.0;
    double zoomApplied_ = 1.0;
    int32_t zoomX_ = 0;
    int32_t zoomY_ = 0;
    VkExtent2D zoomExtent_{};

    static double zoomProfile(double u);

    void pan(int32_t dxPix, int32_t dyPix, const VkExtent2D& extent);
    void zoom(double factor, int32_t screenX, int32_t screenY, const VkExtent2D& extent);
    static bool shiftHeld();

    // Shared with apply(MoveEvent&)'s cursor-readout math: screen pixel ->
    // world-space (x,y), y-flipped to match the rest of the app's convention.
    bool screenToWorld(int32_t screenX, int32_t screenY, const vsg::ref_ptr<vsg::Window>& window,
                        double& outX, double& outY) const;
};

} // namespace giv
