#include "CaliperTool.h"
#include "CaliperJawShape.h"
#include "SceneBuilder.h"
#include "ViewFlip.h"

#include <spdlog/spdlog.h>

#include <vsg/utils/Builder.h>

#include <array>
#include <cmath>
#include <format>

namespace giv
{

namespace
{
constexpr double kBarWidthPx = 12.0;
constexpr double kJawSizePx = 36.0; // half-height of the jaw bracket
constexpr double kJawHitRadiusPx = 40.0;
constexpr double kBarHitHalfWidthPx = 20.0;
constexpr double kGuideWidthPx = 3.0; // axis marker lines in Horizontal/Vertical mode
constexpr double kMinDragPx = 3.0;    // below this, a release is a degenerate no-op click
constexpr double kLabelFillFrac = 0.9; // label glyph height, as a fraction of the bar's own width

// Matches the reference application's CaliperView.h: caliperColorEnds (jaws, dark brown) and
// caliperColorBar (bar, yellow/orange) - both traced back to giv's own
// draw_caliper() cairo colors (0x50,0x2d,0x16 / 0xff,0xaa,0x00).
const vsg::vec4 kJawColor{0.314f, 0.176f, 0.086f, 0.8f};
const vsg::vec4 kBarColor{1.0f, 0.666f, 0.0f, 0.6f};
// the reference application's caliperColorHighlight: the part being hovered or dragged.
const vsg::vec4 kHighlightColor{1.0f, 0.82f, 0.38f, 0.80f};
const vsg::vec4 kLabelColor{0.0f, 0.0f, 0.0f, 1.0f}; // matches giv's own black caliper label text

inline vsg::vec3 toVec3(const vsg::dvec2& v, float z = 0.0f)
{
    return vsg::vec3(static_cast<float>(v.x), static_cast<float>(v.y), z);
}
} // namespace

CaliperTool::CaliperTool(vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::Options> options)
    : camera_(camera), options_(options)
{
    root_ = vsg::Group::create();
    builder_ = vsg::Builder::create();

    // Built once and reused for the tool's whole lifetime - see
    // jawStateGroup_'s doc comment in the header.
    vsg::StateInfo jawState;
    jawState.lighting = false;
    jawState.two_sided = true;
    jawState.blending = true;
    jawStateGroup_ = builder_->createStateGroup(jawState);
    labelStateGroup_ = builder_->createStateGroup(jawState);
}

void CaliperTool::setEnabled(bool enabled)
{
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    haveCaliper_ = false;
    draggingPart_ = kNoPart;
    hoverPart_ = kNoPart;
    builder_ = vsg::Builder::create(); // drop the per-move quad cache
    clearGeometry();
    updateCursor();
    if (onMeasurementText) onMeasurementText(std::string());
}

void CaliperTool::updateCursor()
{
    Cursor wanted = Cursor::Default;
    if (enabled_)
    {
        if (draggingPart_ != kNoPart) wanted = haveCaliper_ ? Cursor::ClosedHand : Cursor::Crosshair;
        else if (haveCaliper_ && hoverPart_ != kNoPart) wanted = Cursor::OpenHand;
        else wanted = Cursor::Crosshair;
    }
    if (wanted == cursor_) return;
    cursor_ = wanted;
    if (onCursorChanged) onCursorChanged(cursor_);
}

void CaliperTool::updateHover(const vsg::dvec2& world)
{
    const int part = haveCaliper_ ? hitTest(world) : kNoPart;
    if (part != hoverPart_)
    {
        hoverPart_ = part;
        rebuildGeometry();
    }
    updateCursor();
}

void CaliperTool::setMode(MeasureMode mode)
{
    if (mode_ == mode) return;
    mode_ = mode;
    refresh();
}

vsg::dvec2 CaliperTool::effectiveP1() const
{
    switch (mode_)
    {
        case MeasureMode::Horizontal: return {p1_.x, p0_.y};
        case MeasureMode::Vertical: return {p0_.x, p1_.y};
        default: return p1_;
    }
}

void CaliperTool::clearGeometry()
{
    if (root_->children.empty()) return;
    if (waitForGpuIdle && !waitForGpuIdle()) return;
    root_->children.clear();
    if (onNeedsCompile) onNeedsCompile();
}

VkExtent2D CaliperTool::currentExtent() const
{
    return viewportExtent ? viewportExtent() : lastExtent_;
}

vsg::dvec2 CaliperTool::windowToWorld(int32_t x, int32_t y) const
{
    const VkExtent2D extent = currentExtent();
    auto ortho = camera_ ? camera_->projectionMatrix.cast<vsg::Orthographic>() : vsg::ref_ptr<vsg::Orthographic>();
    auto lookAt = camera_ ? camera_->viewMatrix.cast<vsg::LookAt>() : vsg::ref_ptr<vsg::LookAt>();
    if (!ortho || !lookAt || extent.width == 0 || extent.height == 0) return {0.0, 0.0};

    double worldPerPixelX = (ortho->right - ortho->left) / static_cast<double>(extent.width);
    double worldPerPixelY = (ortho->top - ortho->bottom) / static_cast<double>(extent.height);
    const vsg::dvec2 o = screenToViewOffset(camera_, (static_cast<double>(x) - extent.width * 0.5) * worldPerPixelX,
                                            (extent.height * 0.5 - static_cast<double>(y)) * worldPerPixelY);
    return {lookAt->center.x + o.x, lookAt->center.y + o.y};
}

double CaliperTool::worldPerPixel() const
{
    const VkExtent2D extent = currentExtent();
    auto ortho = camera_ ? camera_->projectionMatrix.cast<vsg::Orthographic>() : vsg::ref_ptr<vsg::Orthographic>();
    if (!ortho || extent.width == 0) return 1.0;
    return (ortho->right - ortho->left) / static_cast<double>(extent.width);
}

int CaliperTool::hitTest(const vsg::dvec2& world) const
{
    double wpp = worldPerPixel();
    double jawRadius = kJawHitRadiusPx * wpp;

    // Jaws take priority over the bar - they're the smaller targets and sit
    // right at the bar's own endpoints.
    const vsg::dvec2 p1 = effectiveP1();
    if (vsg::length(world - p0_) <= jawRadius) return kJaw0;
    if (vsg::length(world - p1) <= jawRadius) return kJaw1;

    vsg::dvec2 d = p1 - p0_;
    double dist = vsg::length(d);
    if (dist < 1e-9) return kNoPart;
    vsg::dvec2 dir = d / dist;
    vsg::dvec2 perp(-dir.y, dir.x);
    if (dir.x < 0.0) perp = -perp; // mirrors rebuildGeometry()'s own flip

    vsg::dvec2 rel = world - p0_;
    double along = vsg::dot(rel, dir);
    double across = vsg::dot(rel, perp);
    // The bar itself only occupies across in [0, barFullWidth] (see
    // rebuildGeometry()'s barCenter/barSize - it hangs to one side of the
    // p0_/p1_ line, not straddling it); kBarHitHalfWidthPx is just a grab
    // margin added on both sides of that visual footprint.
    double barFullWidth = 2.0 * kBarWidthPx * wpp;
    double margin = kBarHitHalfWidthPx * wpp;
    if (along >= 0.0 && along <= dist && across >= -margin && across <= barFullWidth + margin) return kBar;

    return kNoPart;
}

void CaliperTool::apply(vsg::ButtonPressEvent& event)
{
    if (!enabled_ || event.handled || event.button != 1) return;
    auto window = event.window.ref_ptr();
    if (!window) return;
    lastExtent_ = window->extent2D();

    vsg::dvec2 world = windowToWorld(event.x, event.y);
    builder_ = vsg::Builder::create(); // fresh cache for this gesture

    if (haveCaliper_)
    {
        int part = hitTest(world);
        if (part != kNoPart)
        {
            draggingPart_ = part;
            dragStartMouse_ = world;
            dragStartP0_ = p0_;
            dragStartP1_ = p1_;
            updateCursor();
            event.handled = true;
            return;
        }
    }

    // Start a brand new measurement: press drops the first jaw, drag (see
    // apply(MoveEvent&)) positions the second jaw, release fixes it.
    p0_ = p1_ = world;
    haveCaliper_ = false;
    draggingPart_ = kJaw1;
    hoverPart_ = kNoPart;
    dragStartMouse_ = world;
    dragStartP0_ = world;
    dragStartP1_ = world;
    updateCursor();
    rebuildGeometry();
    event.handled = true;
}

void CaliperTool::apply(vsg::ButtonReleaseEvent& event)
{
    if (event.button != 1 || draggingPart_ == kNoPart) return;

    bool wasNewPlacement = !haveCaliper_;
    const int releasedPart = draggingPart_;
    draggingPart_ = kNoPart;

    if (wasNewPlacement)
    {
        double distPx = vsg::length(effectiveP1() - p0_) / std::max(worldPerPixel(), 1e-12);
        if (distPx < kMinDragPx)
        {
            // A press with no real drag - discard the degenerate 0-length
            // caliper (matches CaliperView::isDegenerateCaliper()).
            haveCaliper_ = false;
            hoverPart_ = kNoPart;
            clearGeometry();
            updateCursor();
            if (onMeasurementText) onMeasurementText(std::string());
            return;
        }
        haveCaliper_ = true;
    }

    // The cursor is still over the part that was just dragged (the drag keeps
    // its grab offset), so it stays highlighted until the cursor leaves it.
    hoverPart_ = releasedPart;
    updateCursor();

    const vsg::dvec2 p1 = effectiveP1();
    double dist = vsg::length(p1 - p0_);
    spdlog::info(
        "Caliper measurement: ({:.3f}, {:.3f}) -> ({:.3f}, {:.3f}), distance = {:.3f}",
        p0_.x, p0_.y, p1.x, p1.y, dist);
}

void CaliperTool::apply(vsg::MoveEvent& event)
{
    if (!enabled_) return;
    auto window = event.window.ref_ptr();
    if (!window) return;
    lastExtent_ = window->extent2D();
    vsg::dvec2 world = windowToWorld(event.x, event.y);
    if (draggingPart_ == kNoPart)
    {
        updateHover(world);
        return;
    }

    // Offset-preserving drag for every part: the point of the jaw/bar under
    // the cursor at press time stays under the cursor, rather than snapping
    // that endpoint straight to the cursor (which jumped on the first move
    // whenever the press wasn't exactly on the endpoint). For a brand new
    // jaw1 placement dragStartP1_ == dragStartMouse_ (see
    // apply(ButtonPressEvent&)), so this reduces to plain absolute tracking
    // there, as intended.
    vsg::dvec2 delta = world - dragStartMouse_;
    if (draggingPart_ == kJaw0) p0_ = dragStartP0_ + delta;
    else if (draggingPart_ == kJaw1) p1_ = dragStartP1_ + delta;
    else // kBar - translate both jaws rigidly
    {
        p0_ = dragStartP0_ + delta;
        p1_ = dragStartP1_ + delta;
    }
    rebuildGeometry();
}

void CaliperTool::refresh()
{
    if (!enabled_ || (!haveCaliper_ && draggingPart_ == kNoPart)) return;
    rebuildGeometry();
}

void CaliperTool::setPixelSize(double pixelSize, const std::string& unit)
{
    pixelSize_ = pixelSize;
    unit_ = unit;
    refresh();
}

void CaliperTool::rebuildGeometry()
{
    double wpp = worldPerPixel();
    if (wpp <= 0.0) return;

    // Must happen before jawStateGroup_->children.clear()/root_->children
    // are touched below - see waitForGpuIdle's doc comment.
    if (waitForGpuIdle && !waitForGpuIdle()) return;

    const vsg::dvec2 p1 = effectiveP1();
    vsg::dvec2 d = p1 - p0_;
    double dist = std::sqrt(d.x * d.x + d.y * d.y);
    // `dir` is the raw p0_->p1_ direction and stays that way for the rest of
    // this function: addJawMesh(p0_, dir)/addJawMesh(p1_, -dir) below is
    // already correct - each jaw's local x axis points from its own endpoint
    // toward the *other* jaw, so the hook (at local x<0) always lands outward
    // - for either orientation, so flipping `dir` itself would swap that and
    // draw both hooks pointing inward instead (see rejected earlier attempt).
    vsg::dvec2 dir = dist > 1e-9 ? d / dist : vsg::dvec2(1.0, 0.0);
    vsg::dvec2 perp(-dir.y, dir.x);

    // Keep the caliper right-side up: the jaws' pointed tip sits at
    // endpoint - perp*scale (see addJawMesh below), so it points screen-down
    // exactly when dir.x >= 0. When the bar points leftward (dir.x < 0) that
    // tip would point up instead - flip only `perp` (the bar's offset side
    // and the jaws' curve/tip both key off it) to bring it back down; this
    // never touches `dir`, so the jaws stay correctly outward-pointing.
    // hitTest() mirrors this same perp-only flip for the bar's hit-band.
    if (dir.x < 0.0) perp = -perp;

    vsg::StateInfo state;
    state.lighting = false;
    state.two_sided = true;
    state.blending = true;

    auto newRoot = vsg::Group::create();
    auto addQuad = [&](const vsg::dvec2& center, const vsg::dvec2& dx, const vsg::dvec2& dy, const vsg::vec4& color, float z = 0.01f) {
        vsg::GeometryInfo info;
        info.position = toVec3(center, z);
        info.dx = toVec3(dx);
        info.dy = toVec3(dy);
        info.color = color;
        newRoot->addChild(builder_->createQuad(info, state));
    };

    // The bar between the two jaws (yellow/orange - the "move both" hotspot).
    // Hangs entirely to one side of the p0_/p1_ center line - its near edge
    // sits exactly on the line, the bar itself sits "above" it (world +perp)
    // - rather than straddling the line, matching CaliperView's own bar
    // placement (local y in [-0.03, 0], i.e. entirely to one side of the
    // hot-spot line).
    vsg::dvec2 barSize = perp * (2.0 * kBarWidthPx * wpp);
    vsg::dvec2 barCenter = (p0_ + p1) * 0.5 + barSize * 0.5;
    addQuad(barCenter, d, barSize, highlightedPart() == kBar ? kHighlightColor : kBarColor);

    // Count Marks: the rectangle the jaws span, outlined in the bar's color.
    if (mode_ == MeasureMode::CountMarks)
    {
        const double thick = kGuideWidthPx * wpp;
        const vsg::dvec2 lo(std::min(p0_.x, p1.x), std::min(p0_.y, p1.y));
        const vsg::dvec2 hi(std::max(p0_.x, p1.x), std::max(p0_.y, p1.y));
        const vsg::dvec2 mid = (lo + hi) * 0.5;
        const double w = hi.x - lo.x, h = hi.y - lo.y;
        addQuad({mid.x, lo.y}, {w + thick, 0.0}, {0.0, thick}, kBarColor);
        addQuad({mid.x, hi.y}, {w + thick, 0.0}, {0.0, thick}, kBarColor);
        addQuad({lo.x, mid.y}, {thick, 0.0}, {0.0, h + thick}, kBarColor);
        addQuad({hi.x, mid.y}, {thick, 0.0}, {0.0, h + thick}, kBarColor);
    }

    // Horizontal / Vertical: an axis marker through each jaw, perpendicular
    // to the measuring direction and spanning the whole view, and a thin guide
    // from the constrained jaw to the raw (cursor-side) position - the reference application's
    // updateGuides(), as giv's own full-height/width measure bands.
    if (mode_ == MeasureMode::Horizontal || mode_ == MeasureMode::Vertical)
    {
        auto lookAt = camera_ ? camera_->viewMatrix.cast<vsg::LookAt>() : vsg::ref_ptr<vsg::LookAt>();
        const vsg::dvec2 viewCenter = lookAt ? vsg::dvec2(lookAt->center.x, lookAt->center.y) : p0_;
        const double span = 4.0 * std::max(lastExtent_.width, lastExtent_.height) * wpp;
        const double thick = kGuideWidthPx * wpp;
        const bool horizontal = mode_ == MeasureMode::Horizontal;
        for (const vsg::dvec2& jaw : {p0_, p1})
        {
            if (horizontal)
                addQuad({jaw.x, viewCenter.y}, {thick, 0.0}, {0.0, span}, kBarColor);
            else
                addQuad({viewCenter.x, jaw.y}, {span, 0.0}, {0.0, thick}, kBarColor);
        }
        vsg::dvec2 g = p1_ - p1;
        double glen = vsg::length(g);
        if (glen > 1e-9)
        {
            vsg::dvec2 gdir = g / glen;
            addQuad((p1 + p1_) * 0.5, g, vsg::dvec2(-gdir.y, gdir.x) * (0.6 * thick), kJawColor, 0.015f);
        }
    }

    // Both jaws' filled triangle meshes (hardcoded shape - see
    // CaliperJawShape.h), one vsg::VertexIndexDraw each so that either can be
    // drawn in the highlight color, swapped into the persistent
    // jawStateGroup_ (see its doc comment in the header for why this doesn't
    // go through addQuad()/builder_).
    {
        constexpr size_t kTrisPerJaw = kJawShapeTriangles.size();
        constexpr size_t kVertCount = kTrisPerJaw * 3;

        // Jaw at `endpoint` points outward along `localX`, away from the
        // other jaw - its flat attachment edge (local x=0) sits exactly on
        // `endpoint` - see CaliperJawShape.h's doc comment.
        auto makeJaw = [&](const vsg::dvec2& endpoint, const vsg::dvec2& localX, const vsg::vec4& color) {
            auto vertices = vsg::vec3Array::create(kVertCount);
            auto normals = vsg::vec3Array::create(kVertCount);
            auto texcoords = vsg::vec2Array::create(kVertCount);
            auto indices = vsg::uintArray::create(kVertCount);

            size_t vi = 0;
            double scale = kJawSizePx * wpp;
            // Flipped in y relative to the raw STL data (see
            // CaliperJawShape.h) - the source mesh's hook faces the wrong
            // way for this 2D view. Negating local.y here mirrors the
            // triangle, which also reverses its winding, so the vertex
            // emission order below is swapped (1,2 -> 2,1) to restore it.
            auto toWorld = [&](const vsg::dvec2& local) { return endpoint + localX * (local.x * scale) - perp * (local.y * scale); };
            for (const auto& tri : kJawShapeTriangles)
                for (int k : {0, 2, 1})
                {
                    vertices->set(vi, toVec3(toWorld(tri[k]), 0.01f));
                    normals->set(vi, vsg::vec3(0.0f, 0.0f, 1.0f));
                    texcoords->set(vi, vsg::vec2(0.0f, 0.0f));
                    indices->set(vi, static_cast<uint32_t>(vi));
                    ++vi;
                }

            auto colors = vsg::vec4Array::create(1, color);
            auto vid = vsg::VertexIndexDraw::create();
            vsg::DataList arrays{vertices, normals, texcoords, colors};
            vid->assignArrays(arrays);
            vid->assignIndices(indices);
            vid->indexCount = static_cast<uint32_t>(kVertCount);
            vid->instanceCount = 1;
            return vid;
        };

        jawStateGroup_->children.clear();
        jawStateGroup_->addChild(makeJaw(p0_, dir, highlightedPart() == kJaw0 ? kHighlightColor : kJawColor));
        jawStateGroup_->addChild(makeJaw(p1, -dir, highlightedPart() == kJaw1 ? kHighlightColor : kJawColor));
    }
    newRoot->addChild(jawStateGroup_);

    lastDistPx_ = dist;
    std::string label = mode_ == MeasureMode::CountMarks
                            ? std::format("{} marks in rectangle", countMarks ? countMarks(p0_, p1) : 0)
                            : std::format("{:.3f}{}", dist * pixelSize_, unit_);

    // The count goes to the status bar only; a text label would not fit on
    // the (possibly tiny) bar.
    if (mode_ != MeasureMode::CountMarks)
    {
        // Parallel to the bar; flip 180 degrees when it points leftward so
        // the text never renders upside down (independent of the jaws'
        // perp-only flip above, since `dir` itself is never touched there).
        double angle = std::atan2(dir.y, dir.x);
        if (std::fabs(std::cos(angle)) < 1e-6)
            angle = vsg::PI * 0.5; // vertical bar: always read bottom-to-top
        else if (std::cos(angle) < 0.0)
            angle += vsg::PI;

        // Centered in the bar (above its own z=0.01f), almost filling the
        // bar's own width (2*kBarWidthPx) in y.
        const double fsize = 2.0 * kBarWidthPx * kLabelFillFrac * wpp;
        FontSpec font;
        font.weight = 700;
        std::vector<vsg::vec2> offsets;
        // buildLabelTriangles rotates clockwise on screen; this world is y up.
        buildLabelTriangles(label, font, fsize, 5, -angle, offsets);

        labelStateGroup_->children.clear();
        if (!offsets.empty())
        {
            const size_t n = offsets.size();
            auto vertices = vsg::vec3Array::create(static_cast<uint32_t>(n));
            auto normals = vsg::vec3Array::create(static_cast<uint32_t>(n));
            auto texcoords = vsg::vec2Array::create(static_cast<uint32_t>(n));
            auto indices = vsg::uintArray::create(static_cast<uint32_t>(n));
            const vsg::vec3 center = toVec3(barCenter, 0.02f);
            for (size_t i = 0; i < n; ++i)
            {
                vertices->set(i, vsg::vec3(center.x + offsets[i].x, center.y + offsets[i].y, center.z));
                normals->set(i, vsg::vec3(0.0f, 0.0f, 1.0f));
                texcoords->set(i, vsg::vec2(0.0f, 0.0f));
                indices->set(i, static_cast<uint32_t>(i));
            }
            auto colors = vsg::vec4Array::create(1, kLabelColor);
            auto vid = vsg::VertexIndexDraw::create();
            vsg::DataList arrays{vertices, normals, texcoords, colors};
            vid->assignArrays(arrays);
            vid->assignIndices(indices);
            vid->indexCount = static_cast<uint32_t>(n);
            vid->instanceCount = 1;
            labelStateGroup_->addChild(vid);
        }
        newRoot->addChild(labelStateGroup_);
    }

    root_->children = newRoot->children;
    if (onNeedsCompile) onNeedsCompile();
    if (onMeasurementText) onMeasurementText(label);
}

} // namespace giv
