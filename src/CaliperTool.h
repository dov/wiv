#pragma once
//
// CaliperTool.h - Tools > Measure Distance Diagonal: giv's caliper tool
// (giv-win.cc's MEASURE_TYPE_DIAGONAL), styled after the reference application's CaliperView
// (src/CaliperView.cpp there): a bar between two bracket-shaped jaws, each of
// the three parts independently pick-sensitive.
//
// Press-drag-release on empty space places a brand new caliper: the press
// point drops the first jaw, dragging positions the second jaw live, release
// fixes it (a release back at the press point - no drag at all - discards
// the degenerate 0-length caliper, matching CaliperView's
// isDegenerateCaliper() handling). Once a caliper exists, pressing on a jaw
// drags just that jaw; pressing on the bar translates both jaws together;
// pressing on empty space again starts a brand new measurement. The caliper
// stays on screen until the tool is toggled off.
//
// Implemented as a small self-contained overlay subgraph (bar + two
// jaw-bracket outlines + a distance label rotated parallel to the bar),
// rebuilt via vsg::Builder on every button-press/move/zoom rather than
// SceneBuilder's instanced mark/line batches: those are optimized for
// whole-file redraw cost, not per-move updates, and the caliper is always
// just a handful of quads. Every size is expressed in screen pixels and
// re-baked from the current zoom on every rebuild, so the caliper's on-screen
// size stays constant as the view zooms (matching giv/the reference application) even
// though the underlying geometry is plain world-space triangles.
//
#include <vsg/all.h>

#include <functional>
#include <string>

namespace giv
{

// Which distance the caliper reports, as in giv's Tools > Measure Distance
// Diagonal/Horizontal/Vertical and the reference application's ECaliperMode. In Horizontal /
// Vertical mode the second jaw is constrained to the first jaw's row / column
// (the raw, unconstrained position is kept, so switching back to Diagonal
// restores the free caliper) and an axis marker line is drawn through each
// jaw, perpendicular to the measuring direction, plus a thin guide from the
// constrained jaw to where the cursor actually is.
enum class MeasureMode
{
    Diagonal,
    Horizontal,
    Vertical
};

class CaliperTool : public vsg::Inherit<vsg::Visitor, CaliperTool>
{
public:
    // `camera` supplies the current Orthographic projection + LookAt used to
    // map window pixels to world coordinates (see PanZoomHandler, whose
    // window-to-world math this mirrors). `font` (may be null, in which case
    // no label is drawn) is used for the floating distance label.
    CaliperTool(vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Options> options);

    // The overlay subgraph - add as a child of the main view alongside the
    // regular scene graph.
    vsg::ref_ptr<vsg::Group> root() const { return root_; }

    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    // Switches the measuring mode, redrawing an existing caliper in it.
    void setMode(MeasureMode mode);
    MeasureMode mode() const { return mode_; }

    void apply(vsg::ButtonPressEvent& event) override;
    void apply(vsg::ButtonReleaseEvent& event) override;
    void apply(vsg::MoveEvent& event) override;

    // Re-bakes the current caliper (if any) against the camera's current
    // zoom - call after anything that changes the projection (zoom in/out,
    // scroll-wheel zoom, fit-to-window, resize) so the caliper's on-screen
    // size stays pixel-constant even when nothing dragged it. No-op if
    // there's no caliper to redraw.
    void refresh();

    // Tools > Calibrate Pixel Size (giv's GivCalibrateDialog / cb_calib_changed):
    // `pixelSize` real-world units per image pixel (default 1.0, i.e. raw
    // pixels), `unit` its display suffix (e.g. "mm", default empty). Rebakes
    // the current label immediately, so an in-progress or already-placed
    // caliper reflects the new calibration without needing to be re-dragged.
    void setPixelSize(double pixelSize, const std::string& unit);
    double pixelSize() const { return pixelSize_; }
    const std::string& unit() const { return unit_; }

    // Raw on-screen distance (in image pixels, pre-calibration) of the
    // current/most recently completed caliper - 0 if none exists yet. Feeds
    // the Calibrate dialog's "Last measure" source option (giv's
    // last_measure_distance_in_pixels).
    double lastDistancePixels() const { return lastDistPx_; }

    // Fired whenever the live/fixed measurement text changes (empty once
    // disabled or before any caliper exists) - for a status-bar readout.
    std::function<void(const std::string&)> onMeasurementText;

    // The extent, in device pixels, that the camera's projection actually spans
    // - the visible part of the render window, which is smaller than the window
    // itself (see VulkanViewport::viewportExtent()). Mouse positions and the
    // world-units-per-pixel scale must be computed against this one, or the
    // caliper lands at a different scale and place than the cursor. Falls back
    // to the extent of the window the last event came from.
    std::function<VkExtent2D()> viewportExtent;

    // Called after root_'s children change, so the owner can push the new
    // geometry to the GPU (vsg::Viewer::compile()).
    std::function<void()> onNeedsCompile;

    // Called at the very start of rebuildGeometry()/clearGeometry(), before
    // either touches jawStateGroup_'s children or replaces root_'s - both
    // drop vsg::ref_ptrs to GPU objects (pipelines, vertex/index buffers)
    // that a previous frame's VkCommandBuffer may still be executing on, and
    // dropping the last ref synchronously destroys the underlying Vulkan
    // object right then, out from under that in-flight command buffer. The
    // owner should block (with a bounded timeout) until the GPU has actually
    // retired those frames and return true, or return false to have this
    // update skipped entirely for now (old geometry stays on screen one more
    // frame) rather than risk that destroy-while-in-use. Mirrors
    // VulkanViewport::rebuildSceneGraph()'s identical fence-wait, for the
    // same reasoning/VUIDs - see its doc comment. A null callback means
    // "always safe to proceed" (used only where the owner has no viewer to
    // wait on, e.g. tests).
    std::function<bool()> waitForGpuIdle;

private:
    // Which part a press landed on - mirrors CaliperView::getIntersection's
    // caliper_part_id (0/1 = jaw at p0_/p1_, 2 = bar); -1 = no hit/none.
    enum : int
    {
        kNoPart = -1,
        kJaw0 = 0,
        kJaw1 = 1,
        kBar = 2
    };

    vsg::ref_ptr<vsg::Camera> camera_;
    vsg::ref_ptr<vsg::Options> options_;
    vsg::ref_ptr<vsg::Group> root_;

    // Kept alive across a single in-progress gesture (a whole drag's worth of
    // mouse-move calls) so its shader/pipeline cache is reused instead of
    // rebuilding a GraphicsPipeline on every mouse move; reset (dropped) at
    // the start of every new press, to bound how many per-position quad
    // entries its internal cache accumulates.
    vsg::ref_ptr<vsg::Builder> builder_;

    // Both jaws' filled triangle mesh (see CaliperJawShape.h) shares this one
    // StateGroup/pipeline, built once in the constructor and reused for the
    // tool's entire lifetime - only its child geometry (a fresh
    // vsg::VertexIndexDraw with the two jaws' current vertex positions) is
    // replaced on every rebuild. Builder::createStateGroup() itself builds a
    // brand new GraphicsPipeline every call (no internal cache like its
    // createQuad()), so doing that per mouse-move would recompile a pipeline
    // on every drag step.
    vsg::ref_ptr<vsg::StateGroup> jawStateGroup_;
    // The distance label's glyph triangles (see buildLabelTriangles), swapped in
    // the same way as the jaws' mesh and for the same reason.
    vsg::ref_ptr<vsg::StateGroup> labelStateGroup_;

    MeasureMode mode_ = MeasureMode::Diagonal;
    bool enabled_ = false;
    bool haveCaliper_ = false;    // a *completed* (released) diagonal exists
    int draggingPart_ = kNoPart;  // part being dragged, or kNoPart
    vsg::dvec2 p0_{0.0, 0.0};
    vsg::dvec2 p1_{0.0, 0.0};
    vsg::dvec2 dragStartMouse_{0.0, 0.0};
    vsg::dvec2 dragStartP0_{0.0, 0.0};
    vsg::dvec2 dragStartP1_{0.0, 0.0};
    VkExtent2D lastExtent_{0, 0};

    double pixelSize_ = 1.0; // real-world units per image pixel - see setPixelSize()
    std::string unit_;
    double lastDistPx_ = 0.0; // see lastDistancePixels()

    // p1_ with the Horizontal/Vertical constraint applied (p1_ itself is the
    // raw jaw position, which may differ from it).
    vsg::dvec2 effectiveP1() const;

    vsg::dvec2 windowToWorld(int32_t x, int32_t y) const;
    VkExtent2D currentExtent() const;
    double worldPerPixel() const;

    // Hit-tests `world` against the current caliper's three parts (jaw
    // circles at p0_/p1_, else the bar rectangle between them), in world
    // units derived from the current zoom - see CaliperView::getIntersection.
    int hitTest(const vsg::dvec2& world) const;

    void rebuildGeometry();
    void clearGeometry();
};

} // namespace giv
