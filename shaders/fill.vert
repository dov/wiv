#version 450
#extension GL_ARB_separate_shader_objects : enable

// Flat-shaded, non-instanced triangle soup used for filled polygons and
// arrow/quiver-head glyphs. Not part of the original shader list in the
// plan (marks/lines only) - added because polygon fill and arrowheads need
// a plain triangle pipeline too.

layout(location = 0) in vec2 inPosition;      // world-space anchor (an arrowhead vertex uses its arrow's tip)
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inOffsetPixels;  // xy = screen-pixel offset from inPosition; (0,0) for plain polygon fill. zw = a quiver head's shaft vector (0 otherwise)

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

void main()
{
    // Arrow/quiver heads are shaped in constant screen pixels around their
    // tip (see addArrowHead), so their offsets are resolved to world units
    // here rather than being rewritten on the CPU on every zoom.
    // A quiver head's tip is stretched by the live quiver zoom along its
    // shaft vector (zw), matching lines.vert's shaft.
    vec2 worldPos = inPosition + inOffsetPixels.zw * (vp.params2.x - 1.0) + inOffsetPixels.xy * (vp.params.x * vp.params2.y);
    gl_Position = pc.projection * pc.modelview * vec4(worldPos, 0.0, 1.0);
    fragColor = vec4(inColor.rgb, vp.params.y > 0.5 ? 1.0 : min(inColor.a * vp.params2.z, 1.0));
}
