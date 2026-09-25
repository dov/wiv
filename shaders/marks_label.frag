#version 450
#extension GL_ARB_separate_shader_objects : enable

// Hard-edged (no antialiasing) variant of marks.frag, used only for the
// offscreen label-picking pass (see LabelPicker). Balloon/tooltip lookup
// works by reading back a single pixel from a render of the scene where
// every dataset is painted in a unique flat "label color" (id+1 packed into
// RGB - see SceneBuilder::labelColorFor); any AA-blended edge alpha would
// mix two datasets' colors at shape boundaries and corrupt that mapping, so
// this shader reproduces marks.frag's shape tests but always discards or
// writes fully opaque, matching giv's own non-antialiased label-image render
// (GivPainterAgg::render_scanlines_bin_solid, do_paint_by_index path).

layout(location = 0) in vec2 fragCorner;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragMode;

layout(location = 0) out vec4 outColor;

void main()
{
    int mode = int(fragMode + 0.5);
    float d = length(fragCorner);

    if (mode == 0) // filled circle
    {
        if (d > 1.0) discard;
    }
    else if (mode == 2) // stroked circle (ring)
    {
        if (d > 1.0 || d < 0.72) discard;
    }
    else if (mode == 3) // stroked square (ring)
    {
        vec2 a = abs(fragCorner);
        float d2 = max(a.x, a.y);
        if (d2 < 0.72) discard;
    }
    // mode 1 (fsquare) and 4 (pixel) are already solid quads - no discard.

    outColor = vec4(fragColor.rgb, 1.0);
}
