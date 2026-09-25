#version 450
#extension GL_ARB_separate_shader_objects : enable

// Flat-shaded, non-instanced triangle soup in plain world coordinates,
// used for the balloon tooltip's background box (see BalloonOverlay).
// This is what fill.vert used to be before it grew the pixel-constant
// arrowhead offset attribute (which the overlay has no use for, and whose
// view-params uniform the overlay would otherwise have to bind too).

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) out vec4 fragColor;

void main()
{
    gl_Position = pc.projection * pc.modelview * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
