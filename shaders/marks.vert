#version 450
#extension GL_ARB_separate_shader_objects : enable

// Instanced mark rendering: one draw call renders every mark in a batch.
// binding 0 is the shared unit-quad base geometry (per-vertex);
// bindings 1-2 carry per-instance data (position/size/shape, color).

layout(location = 0) in vec2 inCorner;            // base quad corner in [-1,1], per-vertex
layout(location = 1) in vec4 inPosSizeMode;        // xy = world position, z = half size (<0: in screen pixels), w = mark mode
layout(location = 2) in vec4 inColor;              // per-instance RGBA

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

layout(location = 0) out vec2 fragCorner;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragMode;

void main()
{
    // A negative half-size encodes "this many screen pixels" (giv's
    // do_scale_marks == false default); a positive one is a world-space
    // half-size ($scale_marks 1, i.e. marks that grow/shrink with zoom).
    float halfSize = inPosSizeMode.z;
    if (halfSize < 0.0) halfSize = -halfSize * vp.params.x;

    vec2 worldPos = inPosSizeMode.xy + inCorner * halfSize;
    gl_Position = pc.projection * pc.modelview * vec4(worldPos, 0.0, 1.0);
    fragCorner = inCorner;
    fragColor = vec4(inColor.rgb, vp.params.y > 0.5 ? 1.0 : inColor.a);
    fragMode = inPosSizeMode.w;
}
