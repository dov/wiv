#version 450
#extension GL_ARB_separate_shader_objects : enable

// Mark shapes, matching giv's MARK_TYPE_* enum:
//   0 = fcircle (filled circle)   1 = fsquare (filled square)
//   2 = circle  (stroked circle)  3 = square  (stroked square)
//   4 = pixel   (small filled square)

layout(location = 0) in vec2 fragCorner;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragMode;

layout(location = 0) out vec4 outColor;

void main()
{
    int mode = int(fragMode + 0.5);
    float d = length(fragCorner);

    if (mode == 0)
    {
        if (d > 1.0) discard;
        outColor = fragColor;
    }
    else if (mode == 1 || mode == 4)
    {
        outColor = fragColor;
    }
    else if (mode == 2)
    {
        if (d > 1.0 || d < 0.72) discard;
        outColor = fragColor;
    }
    else // mode == 3, stroked square
    {
        vec2 a = abs(fragCorner);
        if (max(a.x, a.y) < 0.72) discard;
        outColor = fragColor;
    }
}
