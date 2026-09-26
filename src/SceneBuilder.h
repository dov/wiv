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
#include "ImagePluginHost.h"

#include <vsg/all.h>

#include <string>
#include <unordered_map>

namespace giv
{

// Carries the view's current world-units-per-screen-pixel scale to the
// marks/lines/fill vertex shaders, as a one-vec4 uniform buffer bound (as
// descriptor set 0) by every batch this builder produces.
//
// giv sizes marks, line widths and arrowheads in constant *device pixels*,
// not world units (see giv-data.cc default_mark_size/default_line_width and
// GivPainterCairo::set_line_width - cairo's CTM is left at identity), while
// our vertex shaders expand all of those in world space. Rather than
// recomputing every affected instance's world-space size on the CPU and
// re-uploading the instance buffers whenever the view zooms - O(number of
// marks/segments) work plus a full buffer transfer per zoom step, which
// dominated frame time on multi-million-point files - the buffers store the
// sizes in pixels and the shaders multiply by this single per-frame scalar.
//
// Marks with `$scale_marks 1` (giv's non-default do_scale_marks == TRUE) are
// excluded from that conversion: they keep a world-space size and grow and
// shrink with zoom. They're distinguished in the shader by the sign of the
// stored half-size - see marks.vert and SceneBuilder::build().
class ViewParams : public vsg::Inherit<vsg::Object, ViewParams>
{
public:
    // .x = world units per screen pixel; .y = force-opaque flag (giv's 'a'
    // key, do_no_transparency: forces every mark/line/fill color's alpha to
    // 1.0 in the shaders rather than its $color/alpha value); z/w unused (a
    // vec4 because std140 pads a uniform block's members to 16 bytes
    // anyway).
    vsg::ref_ptr<vsg::vec4Value> value;

    // Call once per frame - not just on zoom, see the implementation - with
    // the current world-units-per-pixel scale (assumed isotropic: x and y
    // scale equally since the view is always fit to the window aspect).
    void update(float worldPerPixel);

    // Toggled by the 'A' shortcut / View menu; takes effect on the next
    // update() call.
    void setForceOpaque(bool forceOpaque) { forceOpaque_ = forceOpaque; }
    bool forceOpaque() const { return forceOpaque_; }

private:
    bool forceOpaque_ = false;
};

// Switches every loaded $image's texture sampler between nearest and
// linear filtering based on the current world-units-per-pixel scale,
// matching giv's own gtk_image_viewer zoom-dependent filter switch
// (gtk-image-viewer.c's view_changed(): scale_x < 1.0 -> bilinear, else
// -> nearest - "0-order interpolation" on zoom-in). Since a VkSampler's
// filter mode is baked in at creation, the switch is done by picking
// between two pre-built descriptor-set variants (one per sampler) via a
// vsg::Switch per image, rather than mutating a sampler in place. One
// world unit == one image pixel (giv's $image has no placement/
// calibration directives), so worldPerPixel <= 1 means each image pixel
// covers at least one screen pixel (zoomed in or at 1:1) -> nearest;
// worldPerPixel > 1 means the image is minified -> linear.
class ImageFilterAnimator : public vsg::Inherit<vsg::Object, ImageFilterAnimator>
{
public:
    std::vector<vsg::ref_ptr<vsg::Switch>> filterSwitches; // one per loaded image

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
    // `images` are the already-resolved-and-decoded $image references (see
    // ImagePluginHost), in the same order as `scene.images`; entries that
    // failed to load should simply be omitted by the caller. Each is drawn
    // as a textured quad, added to the scene graph *before* marks/lines/
    // fill/text so vector data always draws on top.
    vsg::ref_ptr<vsg::Group> build(const SceneData& scene, const std::string& shaderDir,
                                    const std::vector<LoadedImage>& images = {});

    // Valid after build(); non-null only if `images` was non-empty. Has one
    // child per loaded image (vsg::Switch::setSingleChildOn to cycle which
    // one is displayed - see giv's shift-Up/shift-Down image cycling).
    vsg::ref_ptr<vsg::Switch> imageSwitch() const { return imageSwitch_; }

    // Valid after build(); non-null only if `images` was non-empty.
    vsg::ref_ptr<ImageFilterAnimator> imageFilterAnimator() const { return imageFilterAnimator_; }

    // Valid after build(); never null. Must be updated once per frame with
    // the view's current world-units-per-pixel scale.
    vsg::ref_ptr<ViewParams> viewParams() const { return viewParams_; }

    // A second scene graph geometrically identical to the one returned by
    // build(), but painted with one flat, non-antialiased "label color" per
    // dataset (dataset index + 1 packed into RGB) instead of its real color -
    // see labelColorFor() in SceneBuilder.cpp. Meant to be rendered
    // off-screen (see LabelPicker) so a single pixel readback under the
    // mouse cursor can be decoded back into a dataset index for the 'b'
    // balloon/tooltip feature, matching giv's label-image approach
    // (giv-widget.gob's w_label_image / GivPainterAgg::do_paint_by_index).
    // Valid after build(); never null, but may have no children if the
    // scene has no markable geometry.
    vsg::ref_ptr<vsg::Group> labelGraph() const { return labelGraph_; }

private:
    vsg::ref_ptr<vsg::Options> options_;
    std::unordered_map<std::string, vsg::ref_ptr<vsg::Font>> fontCache_;
    vsg::ref_ptr<ViewParams> viewParams_;
    vsg::ref_ptr<vsg::Group> labelGraph_;
    vsg::ref_ptr<vsg::Switch> imageSwitch_;
    vsg::ref_ptr<ImageFilterAnimator> imageFilterAnimator_;

    vsg::ref_ptr<vsg::Font> resolveFont(const std::string& fontSpec, double& outSize);
};

} // namespace giv
