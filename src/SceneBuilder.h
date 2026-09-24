#pragma once
//
// SceneBuilder.h - turns a parsed giv::SceneData into a VSG scene graph.
//
// Design (see plan / README for rationale):
//  - Marks are rendered with one instanced draw for the *entire* file:
//    a shared unit quad + per-instance (position, half-size, shape-mode)
//    and (color) vertex buffers, expanded in marks.vert and shaded in
//    marks.frag (SDF-ish circle/square, filled or stroked).
//  - Polylines/polygon outlines/ellipse outlines/quiver shafts are rendered
//    with one instanced draw for the entire file: per-segment (p0,p1),
//    (half-width, dash-on, dash-off, cumulative-length) and (color),
//    expanded in lines.vert, dashed in lines.frag.
//  - Filled polygons and arrow/quiver heads are flat-shaded triangle soup
//    in one more draw (fill.vert/fill.frag), fan-triangulated on the CPU.
//  - Text uses vsg::Text/StandardLayout/CpuLayoutTechnique with a font
//    resolved via `fc-match` (Linux) from the giv $font spec.
//
#include "GivScene.h"

#include <vsg/all.h>

#include <string>
#include <unordered_map>

namespace giv
{

// Keeps a chosen vec4 component of selected instances at a constant
// on-screen (pixel) size as the view zooms. Marks and lines are both
// expanded in *world* space by their vertex shaders (marks.vert reads
// posSizeMode.z as a world half-size; lines.vert reads widthDash.x as a
// world half-width), but giv itself sizes them in constant device pixels
// (see giv-data.cc default_mark_size/default_line_width and
// GivPainterCairo::set_line_width - cairo's CTM is left at identity, so
// pixel/device units, not world units). So every time the view's world-
// units-per-pixel changes (i.e. on zoom) we recompute the world-space
// value that currently corresponds to the desired fixed pixel size and
// re-upload just the affected component of just the affected instances.
//
// Used for: (a) marks whose dataset did not set `$scale_marks 1` -
// giv's default do_scale_marks == FALSE (giv-data.cc: default_scale_marks)
// - component z of posSizeModeArray; marks with `$scale_marks 1` are
// excluded (left growing/shrinking with zoom, using their world-space
// mark_size directly). (b) line/outline/quiver half-width - component x
// of widthDashArray; giv has no scale-with-zoom option for line width at
// all, so *every* line instance is included.
class PixelSizeAnimator : public vsg::Inherit<vsg::Object, PixelSizeAnimator>
{
public:
    vsg::ref_ptr<vsg::vec4Array> array;
    int component = 2; // 0=x,1=y,2=z,3=w
    std::vector<uint32_t> indices;    // indices into `array`
    std::vector<float> pixelSize;     // desired size in screen pixels, parallel to `indices`

    // Call once per frame (or whenever the view may have zoomed) with the
    // current world-units-per-pixel scale (assumed isotropic - x and y
    // scale equally since the view is always fit to the window aspect).
    void update(float worldPerPixel);

private:
    float lastWorldPerPixel_ = -1.0f;
};

class SceneBuilder
{
public:
    explicit SceneBuilder(vsg::ref_ptr<vsg::Options> options);

    // Builds the full scene graph for `scene`. `shaderDir` is the directory
    // containing the precompiled marks/lines/fill .vert.spv/.frag.spv files.
    vsg::ref_ptr<vsg::Group> build(const SceneData& scene, const std::string& shaderDir);

    // Valid after build(); non-null only if the scene contains marks/lines
    // respectively.
    vsg::ref_ptr<PixelSizeAnimator> markSizeAnimator() const { return markSizeAnimator_; }
    vsg::ref_ptr<PixelSizeAnimator> lineWidthAnimator() const { return lineWidthAnimator_; }

private:
    vsg::ref_ptr<vsg::Options> options_;
    std::unordered_map<std::string, vsg::ref_ptr<vsg::Font>> fontCache_;
    vsg::ref_ptr<PixelSizeAnimator> markSizeAnimator_;
    vsg::ref_ptr<PixelSizeAnimator> lineWidthAnimator_;

    vsg::ref_ptr<vsg::Font> resolveFont(const std::string& fontSpec, double& outSize);
};

} // namespace giv
