#version 450
#extension GL_ARB_separate_shader_objects : enable

// A single non-instanced textured quad, used to display one $image
// reference's bitmap. Same push-constant convention as every other wiv
// shader (fill.vert/marks.vert/lines.vert), even though a static image
// doesn't strictly need the modelview half of it.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) out vec2 fragUV;

void main()
{
    gl_Position = pc.projection * pc.modelview * vec4(inPosition, 0.0, 1.0);
    fragUV = inUV;
}
