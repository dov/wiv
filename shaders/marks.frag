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

    // giv/AGG rasterizes marks with software anti-aliasing, giving mark
    // circles/rings smooth analytic edges. Our SDF-style discard-based
    // shapes are otherwise hard-edged even with MSAA (MSAA only smooths
    // the quad's geometric boundary, not a `discard` cutoff inside it), so
    // apply a ~1-pixel smoothstep across each shape edge using screen-space
    // derivatives (fwidth), and blend alpha instead of a hard discard.
    float aa = fwidth(d) * 0.5 + 1e-6;

    if (mode == 0) // filled circle
    {
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, d);
        if (alpha <= 0.0) discard;
        outColor = vec4(fragColor.rgb, fragColor.a * alpha);
    }
    else if (mode == 1 || mode == 4)
    {
        outColor = fragColor;
    }
    else if (mode == 2) // stroked circle (ring)
    {
        float outer = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, d);
        float inner = smoothstep(0.72 - aa, 0.72 + aa, d);
        float alpha = outer * inner;
        if (alpha <= 0.0) discard;
        outColor = vec4(fragColor.rgb, fragColor.a * alpha);
    }
    else // mode == 3, stroked square (ring)
    {
        vec2 a = abs(fragCorner);
        float d2 = max(a.x, a.y);
        float aa2 = fwidth(d2) * 0.5 + 1e-6;
        float alpha = smoothstep(0.72 - aa2, 0.72 + aa2, d2);
        if (alpha <= 0.0) discard;
        outColor = vec4(fragColor.rgb, fragColor.a * alpha);
    }
}
