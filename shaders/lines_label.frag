// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Label-picking variant of lines.frag: the same capsule shape, but hard
// edged (no anti-aliasing, no blending) and without the depth bookkeeping.

layout(location = 0) in vec4 fragColor;
layout(location = 1) in float fragAlong;
layout(location = 2) in vec2 fragDash;
layout(location = 3) in vec4 fragGeom;
layout(location = 4) flat in vec2 fragStyle;
layout(location = 5) flat in vec2 fragCumLen;

layout(location = 0) out vec4 outColor;

float capDist(vec2 v, float hw, int cap)
{
    if (cap == 1) return max(v.y, hw + abs(v.x));
    if (cap == 2) return max(v.y, abs(v.x));
    return length(v);
}

int capStyle(int flags, int endBit)
{
    if ((flags & endBit) == 0) return 0;
    if ((flags & 8) != 0) return 1;
    if ((flags & 16) != 0) return 2;
    return 0;
}

void main()
{
    int flags = int(fragStyle.y + 0.5);
    if (flags != 0)
    {
        float s = fragGeom.x;
        float d = abs(fragGeom.y);
        float len = fragGeom.z;
        float hw = fragGeom.w;
        float dist = d;
        if (s < 0.0) dist = capDist(vec2(s, d), hw, capStyle(flags, 2));
        else if (s > len) dist = capDist(vec2(s - len, d), hw, capStyle(flags, 4));
        if (dist > hw) discard;
    }

    float off = fragDash.y;
    if (off > 0.0)
    {
        float on = fragDash.x;
        float period = on + off;
        float along = fragCumLen.x + clamp(fragAlong - fragCumLen.x, 0.0, fragCumLen.y);
        float phase = mod(along, period);
        if (phase > on) discard;
    }
    outColor = fragColor;
}
