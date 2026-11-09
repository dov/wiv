// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Transparency checkerboard: one quad over the displayed image (or SVG
// bounds), drawn underneath it so that translucent pixels show a pattern
// instead of a flat colour. The pattern itself is made in checker.frag from
// the screen position. Whether it shows at all (and light or dark) is the
// ViewParams second vec4's .w, so changing the mode needs no scene rebuild:
// when it is 0 the quad is pushed outside the clip volume.

layout(location = 0) in vec2 inPosition;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(set = 0, binding = 0) uniform ViewParams
{
    vec4 params;  // x = world units per screen pixel
    vec4 params2; // w = backdrop: 0 = off, 1 = light checker, 2 = dark checker,
                  //     >= 100: flat colour, w = 100 + (r<<14 | g<<7 | b), 7 bits each
} vp;

layout(location = 0) out flat float fragDark;
layout(location = 1) out flat vec3 fragFlat; // flat colour, or negative red for "checker"

void main()
{
    bool visible = vp.params2.w > 0.5;
    gl_Position = visible ? pc.projection * pc.modelview * vec4(inPosition, 0.0, 1.0)
                          : vec4(2.0, 2.0, 0.0, 1.0);
    fragDark = vp.params2.w > 1.5 ? 1.0 : 0.0;
    fragFlat = vec3(-1.0);
    if (vp.params2.w >= 100.0)
    {
        int rgb = int(vp.params2.w + 0.5) - 100; // 21 bits: exact in a float
        fragFlat = vec3(float((rgb >> 14) & 127), float((rgb >> 7) & 127), float(rgb & 127)) / 127.0;
    }
}
