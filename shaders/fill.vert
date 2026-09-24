#version 450
#extension GL_ARB_separate_shader_objects : enable

// Flat-shaded, non-instanced triangle soup used for filled polygons and
// arrow/quiver-head glyphs. Not part of the original shader list in the
// plan (marks/lines only) - added because polygon fill and arrowheads need
// a plain triangle pipeline too.

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
