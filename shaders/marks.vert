#version 450
#extension GL_ARB_separate_shader_objects : enable

// Instanced mark rendering: one draw call renders every mark in a batch.
// binding 0 is the shared unit-quad base geometry (per-vertex);
// bindings 1-2 carry per-instance data (position/size/shape, color).

layout(location = 0) in vec2 inCorner;            // base quad corner in [-1,1], per-vertex
layout(location = 1) in vec4 inPosSizeMode;        // xy = world position, z = half size, w = mark mode
layout(location = 2) in vec4 inColor;              // per-instance RGBA

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) out vec2 fragCorner;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragMode;

void main()
{
    vec2 worldPos = inPosSizeMode.xy + inCorner * inPosSizeMode.z;
    gl_Position = pc.projection * pc.modelview * vec4(worldPos, 0.0, 1.0);
    fragCorner = inCorner;
    fragColor = inColor;
    fragMode = inPosSizeMode.w;
}
