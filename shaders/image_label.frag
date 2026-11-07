// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Label-picking variant of image.frag: the sprite's flat label color where
// the texture is (mostly) opaque, nothing elsewhere. Hard edged and always
// fully opaque so the id readback is never blended (see marks_label.frag);
// the faint tail of a blur halo is not pickable.

layout(set = 0, binding = 0) uniform sampler2D imageTex;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragLabelColor;
layout(location = 0) out vec4 outColor;

void main()
{
    if (texture(imageTex, fragUV).a < 0.5) discard;
    outColor = vec4(fragLabelColor.rgb, 1.0);
}
