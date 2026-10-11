#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragWorldPos;
layout(location = 1) in flat float fragWorldPerPixel;
layout(location = 0) out vec4 outColor;

// giv draws every pixel boundary twice: first in half-transparent black at
// width 3, then in the pixel grid color (red, alpha 0.5) at width 1.
const vec3 kGridColor = vec3(1.0, 0.0, 0.0);
const float kGridAlpha = 0.5;

void main()
{
    // Distance in screen pixels to the nearest pixel boundary.
    vec2 d = abs(fragWorldPos - round(fragWorldPos)) / fragWorldPerPixel;
    float m = min(d.x, d.y);

    float aBlack = kGridAlpha * (1.0 - smoothstep(1.0, 2.0, m));
    float aGrid = kGridAlpha * (1.0 - smoothstep(0.0, 1.0, m));

    // "over" composite of the grid color on the black underlay.
    float a = aGrid + aBlack * (1.0 - aGrid);
    if (a < 0.002) discard;
    outColor = vec4(kGridColor * aGrid / a, a);
}
