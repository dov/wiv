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

class SceneBuilder
{
public:
    explicit SceneBuilder(vsg::ref_ptr<vsg::Options> options);

    // Builds the full scene graph for `scene`. `shaderDir` is the directory
    // containing the precompiled marks/lines/fill .vert.spv/.frag.spv files.
    vsg::ref_ptr<vsg::Group> build(const SceneData& scene, const std::string& shaderDir);

private:
    vsg::ref_ptr<vsg::Options> options_;
    std::unordered_map<std::string, vsg::ref_ptr<vsg::Font>> fontCache_;

    vsg::ref_ptr<vsg::Font> resolveFont(const std::string& fontSpec, double& outSize);
};

} // namespace giv
