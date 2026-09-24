#version 450
#extension GL_ARB_separate_shader_objects : enable

// Instanced thick-line rendering: each instance is one line segment,
// expanded from a shared unit quad into a screen-aligned-in-world-space
// rectangle between p0 and p1 with the given half width.

layout(location = 0) in vec2 inCorner;        // base quad corner: x in [-1,1] along segment, y in [-1,1] across
layout(location = 1) in vec4 inP0P1;          // xy = segment start, zw = segment end
layout(location = 2) in vec4 inWidthDash;     // x = half width, y = dash-on length, z = dash-off length, w = cumulative length at p0
layout(location = 3) in vec4 inColor;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out float fragAlong;
layout(location = 2) out vec2 fragDash;

void main()
{
    vec2 p0 = inP0P1.xy;
    vec2 p1 = inP0P1.zw;
    float halfWidth = inWidthDash.x;

    vec2 dir = p1 - p0;
    float segLen = length(dir);
    vec2 dirN = segLen > 1e-6 ? dir / segLen : vec2(1.0, 0.0);
    vec2 normal = vec2(-dirN.y, dirN.x);

    float t = (inCorner.x + 1.0) * 0.5; // 0 at p0, 1 at p1
    vec2 worldPos = mix(p0, p1, t) + normal * (inCorner.y * halfWidth);

    gl_Position = pc.projection * pc.modelview * vec4(worldPos, 0.0, 1.0);
    fragColor = inColor;
    fragAlong = inWidthDash.w + t * segLen;
    fragDash = inWidthDash.yz;
}
