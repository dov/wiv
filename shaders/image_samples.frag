#version 450
#extension GL_ARB_separate_shader_objects : enable

// Raw single-channel samples (R8/R16 UNORM or R32F) shown through a contrast
// window and a 256-entry colormap, so changing either is a uniform write
// instead of a CPU re-render and texture upload. Mirrors the CPU stretch in
// DisplayImage.cpp: nearest-rounded 0..255 index, clamped, NaN -> 0.

layout(set = 0, binding = 0) uniform sampler2D samplesTex;

layout(set = 0, binding = 1) uniform Params
{
    // x: value of 1.0 from the sampler in sample units (255, 65535 or 1 for
    //    float); y: window min; z: 1/(max-min); w: offset (0.5 if max == min).
    vec4 window;
    vec4 colormap[256];
} params;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    float v = texture(samplesTex, fragUV).r * params.window.x;
    float t = (v - params.window.y) * params.window.z + params.window.w;
    float x = floor(t * 255.0 + 0.5);
    int idx = x > 0.0 ? (x < 255.0 ? int(x) : 255) : 0;
    outColor = params.colormap[idx];
}
