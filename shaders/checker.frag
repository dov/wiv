// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in flat float fragDark;
layout(location = 1) in flat vec3 fragFlat;
layout(location = 0) out vec4 outColor;

// Cell size in screen pixels: fixed, so the pattern neither balloons when
// zooming in nor aliases when zooming out.
const float kCell = 8.0;

// Two close shades each; the light pair matches a white canvas, the dark
// pair a dark theme's.
const vec3 kLightA = vec3(1.00);
const vec3 kLightB = vec3(0.86);
const vec3 kDarkA = vec3(0.16);
const vec3 kDarkB = vec3(0.26);

void main()
{
    if (fragFlat.r >= 0.0)
    {
        outColor = vec4(fragFlat, 1.0);
        return;
    }
    ivec2 c = ivec2(floor(gl_FragCoord.xy / kCell));
    bool odd = ((c.x + c.y) & 1) != 0;
    vec3 a = fragDark > 0.5 ? kDarkA : kLightA;
    vec3 b = fragDark > 0.5 ? kDarkB : kLightB;
    outColor = vec4(odd ? b : a, 1.0);
}
