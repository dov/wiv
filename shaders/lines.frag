#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 fragColor;
layout(location = 1) in float fragAlong;
layout(location = 2) in vec2 fragDash;

layout(location = 0) out vec4 outColor;

void main()
{
    float on = fragDash.x;
    float off = fragDash.y;
    if (off > 0.0)
    {
        float period = on + off;
        float phase = mod(fragAlong, period);
        if (phase > on) discard;
    }
    outColor = fragColor;
}
