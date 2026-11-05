#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 fragColor;
layout(location = 1) in float fragAlong;
layout(location = 2) in vec2 fragDash;
layout(location = 3) in vec4 fragGeom;               // pixels: x = along (0 at p0), y = across, z = segment length, w = half width
layout(location = 4) flat in vec2 fragStyle;         // x = depth slot, y = flags
layout(location = 5) flat in vec2 fragCumLen;        // world units: cumulative length at p0, segment length

layout(location = 0) out vec4 outColor;

// fragStyle.y flags (0 = plain rectangle, no anti-aliasing - SVG strokes):
//   1  capsule: round joins, analytic anti-aliased edge
//   2  p0 is the start of a path (use the cap style below instead of a round join)
//   4  p1 is the end of a path
//   8  cap style at path ends is butt
//  16  cap style at path ends is square   (neither 8 nor 16: round)
//
// Depth: the main pipeline writes depth to draw each dataset's lines once
// per pixel, so overlapping segments of one translucent line don't blend
// twice. fragStyle.x is a per-dataset slot that grows in draw order; it is
// turned into an exactly representable depth (reversed-Z, GREATER test, so
// a later dataset passes, and a second fragment of the same dataset fails).
//
// Anti-aliased edge pixels must not be blended on top of the solid body of a
// neighbouring segment (that leaves a faint seam inside translucent joints),
// so the batch is drawn twice with this shader: the default build draws only
// fully covered fragments, the FRINGE_PASS build (a second draw of the same
// instances) only partially covered ones, at a slightly smaller depth. A
// fringe fragment therefore loses against any solid fragment already drawn
// for the same dataset (or a later one), and never blocks one.

// Distance (to be compared with the half width) of point v = (along past
// the end, |across|) from a cap of the given style: 0 round, 1 butt, 2 square.
float capDist(vec2 v, float hw, int cap)
{
    if (cap == 1) return max(v.y, hw + abs(v.x));
    if (cap == 2) return max(v.y, abs(v.x));
    return length(v);
}

int capStyle(int flags, int endBit)
{
    if ((flags & endBit) == 0) return 0; // interior joint: round
    if ((flags & 8) != 0) return 1;
    if ((flags & 16) != 0) return 2;
    return 0;
}

void main()
{
    float cov = 1.0;
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
        cov = clamp(hw - dist + 0.5, 0.0, 1.0);
        if (cov <= 0.0) discard;
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

    uint depthBits = 0x3F000000u + uint(fragStyle.x) * 4u;
#ifdef FRINGE_PASS
    if (cov >= 0.99) discard;
    depthBits -= 2u;
#else
    if (cov < 0.99) discard;
#endif
    gl_FragDepth = uintBitsToFloat(depthBits);

    outColor = vec4(fragColor.rgb, fragColor.a * cov);
}
