#version 450
#extension GL_ARB_separate_shader_objects : enable

// Flat-shaded, non-instanced triangle soup used for filled polygons and
// arrow/quiver-head glyphs. Not part of the original shader list in the
// plan (marks/lines only) - added because polygon fill and arrowheads need
// a plain triangle pipeline too.

layout(location = 0) in vec2 inPosition;      // world-space anchor (an arrowhead vertex uses its arrow's tip)
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inOffsetPixels;  // screen-pixel offset from inPosition; (0,0) for plain polygon fill

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
} vp;

layout(location = 0) out vec4 fragColor;

void main()
{
    // Arrow/quiver heads are shaped in constant screen pixels around their
    // tip (see addArrowHead), so their offsets are resolved to world units
    // here rather than being rewritten on the CPU on every zoom.
    vec2 worldPos = inPosition + inOffsetPixels * vp.params.x;
    gl_Position = pc.projection * pc.modelview * vec4(worldPos, 0.0, 1.0);
    fragColor = vec4(inColor.rgb, vp.params.y > 0.5 ? 1.0 : inColor.a);
}
