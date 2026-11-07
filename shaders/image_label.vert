// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Label-picking variant of image.vert, used for raster sprites (SVG shapes
// with a blur filter): the quad additionally carries its dataset's label
// color (see SceneBuilder::labelColorFor).

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inLabelColor;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelview;
} pc;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragLabelColor;

void main()
{
    gl_Position = pc.projection * pc.modelview * vec4(inPosition, 0.0, 1.0);
    fragUV = inUV;
    fragLabelColor = inLabelColor;
}
