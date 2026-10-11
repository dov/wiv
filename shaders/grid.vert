#version 450
#extension GL_ARB_separate_shader_objects : enable

// Pixel grid (giv's View > Pixel Grid, 'g'): one quad over the displayed
// $image, with the grid lines themselves drawn in grid.frag at every integer
// world coordinate (one world unit == one image pixel). Toggling the grid
// and the min-zoom cutoff both live here: when the grid is off or the view
// is zoomed out too far the quad is pushed outside the clip volume, so
// nothing is rasterized and no scene graph change is needed.

layout(location = 0) in vec2 inPosition;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(set = 0, binding = 0) uniform ViewParams
{
    vec4 params; // x = world units per screen pixel; z = show pixel grid
} vp;

layout(location = 0) out vec2 fragWorldPos;
layout(location = 1) out flat float fragWorldPerPixel;

// giv: pixel_grid_min_zoom = 5, drawn when scale_x > 5 screen pixels per
// image pixel.
const float kMinZoom = 5.0;

void main()
{
    float wpp = vp.params.x;
    bool visible = vp.params.z > 0.5 && wpp > 0.0 && wpp < 1.0 / kMinZoom;
    gl_Position = visible ? pc.projection * pc.modelview * vec4(inPosition, 0.0, 1.0)
                          : vec4(2.0, 2.0, 0.0, 1.0);
    fragWorldPos = inPosition;
    fragWorldPerPixel = wpp;
}
