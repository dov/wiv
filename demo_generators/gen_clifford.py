#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Generate a Clifford attractor as a .giv file, for showing off wiv/giv.

    gen_clifford.py -o clifford.giv
    gen_clifford.py -n 2000000 --alpha 0.08 -a -1.7 -b 1.3 -c -0.1 -d -1.2 -o c2.giv

The map is
    x' = sin(a*y) + c*cos(a*x)
    y' = sin(b*x) + d*cos(b*y)
Every point is drawn as a small translucent mark, so dense regions of the
attractor build up and glow while sparse filaments stay faint. The orbit is
split into --bands consecutive datasets, each given a hue along a gradient,
so the colors drift along the trajectory.

Zoom in: the structure is made of fine filaments at every scale.
"""
import argparse
import colorsys
import sys

import numpy as np


def clifford_orbit(n, a, b, c, d, x0=0.1, y0=0.0, burn=1000):
    # The orbit is sequential, but plain Python floats are fast enough for ~1M.
    from math import sin, cos
    xs = np.empty(n)
    ys = np.empty(n)
    x, y = x0, y0
    for i in range(-burn, n):
        x, y = sin(a * y) + c * cos(a * x), sin(b * x) + d * cos(b * y)
        if i >= 0:
            xs[i] = x
            ys[i] = y
    return xs, ys


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-n", "--points", type=int, default=1_000_000)
    p.add_argument("-a", type=float, default=-1.4)
    p.add_argument("-b", type=float, default=1.6)
    p.add_argument("-c", type=float, default=1.0)
    p.add_argument("-d", type=float, default=0.7)
    p.add_argument("--alpha", type=float, default=0.1, help="per-point opacity (default 0.1)")
    p.add_argument("--bands", type=int, default=48, help="number of hue bands (default 48)")
    p.add_argument("--hue-start", type=float, default=0.55, help="0..1 (default 0.55, blue)")
    p.add_argument("--hue-span", type=float, default=0.45, help="hue range covered (default 0.45)")
    p.add_argument("--size", type=float, default=1.0, help="mark size (pixels, or world units with --scale-marks 1)")
    p.add_argument("--scale-marks", type=int, choices=(0, 1), default=0,
                   help="$scale_marks: 0 = marks keep a constant pixel size, so zooming "
                        "separates the points; 1 = marks scale with zoom (default 0)")
    p.add_argument("--extent", type=float, default=1000.0, help="world width of the picture")
    p.add_argument("--background", default="black",
                   help="viewport background color, written as $background (default black)")
    p.add_argument("-o", "--output", default="-")
    args = p.parse_args()

    xs, ys = clifford_orbit(args.points, args.a, args.b, args.c, args.d)
    if not (np.isfinite(xs).all() and np.isfinite(ys).all()):
        sys.exit("orbit diverged")

    # Fit the orbit into a square of side --extent, centered on the origin.
    # giv's y axis points down, so flip y to keep the picture upright.
    span = max(np.ptp(xs), np.ptp(ys)) or 1.0
    s = args.extent / span
    xs = (xs - (xs.max() + xs.min()) / 2) * s
    ys = -(ys - (ys.max() + ys.min()) / 2) * s

    out = sys.stdout if args.output == "-" else open(args.output, "w")
    out.write(f"# Clifford attractor a={args.a} b={args.b} c={args.c} d={args.d}, "
              f"{args.points} points\n")
    if args.background:
        out.write(f"$background {args.background}\n")

    edges = np.linspace(0, args.points, args.bands + 1).astype(int)
    for k in range(args.bands):
        hue = (args.hue_start + args.hue_span * k / max(args.bands - 1, 1)) % 1.0
        r, g, b = (int(round(255 * v)) for v in colorsys.hsv_to_rgb(hue, 0.75, 1.0))
        # Each dataset starts from giv's defaults (lines on, marks off), so
        # the style must be repeated for every band.
        out.write(f"$path band_{k}\n$color #{r:02x}{g:02x}{b:02x}/{args.alpha}\n"
                  f"$noline\n$marks fcircle\n$mark_size {args.size}\n$scale_marks {args.scale_marks}\n")
        seg = np.column_stack((xs[edges[k]:edges[k + 1]], ys[edges[k]:edges[k + 1]]))
        np.savetxt(out, seg, fmt="%.3f")
        out.write("\n")
    if out is not sys.stdout:
        out.close()


if __name__ == "__main__":
    main()
