// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Instanced thick-line rendering: each instance is one line segment,
// expanded from a shared unit quad into a screen-aligned-in-world-space
// rectangle between p0 and p1 with the given half width.
//
// Instances with inStyle.y != 0 are drawn as "capsules": the quad is grown
// past both ends (and by one pixel across) and lines.frag carves out round
// joins/caps - or butt/square caps at the real ends of a path - with an
// analytic anti-aliased edge. inStyle.y == 0 keeps the plain rectangle,
// used for SVG strokes.

layout(location = 0) in vec2 inCorner;        // base quad corner: x in [-1,1] along segment, y in [-1,1] across
layout(location = 1) in vec4 inP0P1;          // xy = segment start, zw = segment end
layout(location = 2) in vec4 inWidthDash;     // x = half width in screen pixels, y = dash-on length, z = dash-off length, w = cumulative length at p0
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inStyle;         // x = depth slot (see lines.frag), y = cap/join flags (see lines.frag; 0 = plain rectangle)

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

// Set once per frame by GivViewer::render() (see giv::ViewParams): the
// current world-units-per-screen-pixel scale of the orthographic view.
// Sizes that giv specifies in constant device pixels (mark sizes, line
// widths, arrowhead geometry) are stored in *pixels* in the vertex/
// instance buffers and converted to world units here, so zooming never
// has to rewrite and re-upload a single byte of per-primitive data.
layout(set = 0, binding = 0) uniform ViewParams
{
    vec4 params; // x = world units per screen pixel; y = force-opaque (giv's
                 // 'a' toggle, do_no_transparency); zw unused
    vec4 params2; // x = live quiver length factor, y = live line width factor, z = alpha multiplier
} vp;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out float fragAlong;
layout(location = 2) out vec2 fragDash;
layout(location = 3) out vec4 fragGeom;               // pixels: x = along (0 at p0), y = across, z = segment length, w = half width
layout(location = 4) flat out vec2 fragStyle;
layout(location = 5) flat out vec2 fragCumLen;        // world units: cumulative length at p0, segment length

void main()
{
    vec2 p0 = inP0P1.xy;
    vec2 p1 = inP0P1.zw;
    // dashOff < 0 flags a quiver shaft, built at ds.quiverScale: stretch it by
    // the live quiver zoom so a drag needs no scene rebuild.
    if (inWidthDash.z < 0.0) p1 = p0 + (p1 - p0) * vp.params2.x;
    // giv draws every line/outline/quiver shaft at a constant device-pixel
    // width regardless of zoom, so inWidthDash.x is a *pixel* half-width.
    float wpp = vp.params.x;
    float halfWidth = inWidthDash.x * wpp * vp.params2.y;

    vec2 dir = p1 - p0;
    float segLen = length(dir);
    vec2 dirN = segLen > 1e-6 ? dir / segLen : vec2(1.0, 0.0);
    vec2 normal = vec2(-dirN.y, dirN.x);

    bool capsule = inStyle.y > 0.5;
    float margin = capsule ? wpp : 0.0;      // one pixel of room for the anti-aliased edge
    float ext = capsule ? halfWidth + margin : 0.0;
    float along = inCorner.x < 0.0 ? -ext : segLen + ext;
    float across = inCorner.y * (halfWidth + margin);

    vec2 worldPos = p0 + dirN * along + normal * across;

    gl_Position = pc.projection * pc.modelview * vec4(worldPos, 0.0, 1.0);
    fragColor = vec4(inColor.rgb, vp.params.y > 0.5 ? 1.0 : min(inColor.a * vp.params2.z, 1.0));
    fragAlong = inWidthDash.w + along;
    fragDash = inWidthDash.yz;
    fragGeom = vec4(along, across, segLen, halfWidth) / wpp;
    fragStyle = inStyle;
    fragCumLen = vec2(inWidthDash.w, segLen);
}
