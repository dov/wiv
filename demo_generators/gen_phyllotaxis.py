#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Phyllotaxis (sunflower spiral): point k sits at radius c*sqrt(k), angle k*golden_angle.

Points are coloured by index (hue sweep) so the Fibonacci spirals show up.
"""
import argparse, math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from givout import write_buckets, hsv_hex

GOLDEN_ANGLE = math.pi * (3 - math.sqrt(5))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--num-points", type=int, default=20000)
    ap.add_argument("--angle", type=float, default=math.degrees(GOLDEN_ANGLE),
                    help="divergence angle in degrees (default: golden angle %(default).4f)")
    ap.add_argument("-o", "--output", default="-")
    ap.add_argument("--mark-size", type=float, default=4.0)
    a = ap.parse_args()

    k = np.arange(a.num_points)
    r = 10 * np.sqrt(k)
    th = k * math.radians(a.angle)
    xy = np.stack([r * np.cos(th), r * np.sin(th)], axis=1)

    f = sys.stdout if a.output == "-" else open(a.output, "w")
    f.write(f"# phyllotaxis, {a.num_points} points, angle {a.angle:.4f} deg\n")
    f.write("$scale_marks 1\n")
    write_buckets(f, xy, k / a.num_points, lambda t: hsv_hex(t * 0.9, 0.85, 1.0),
                  120, "seeds", mark_size=a.mark_size)
    if f is not sys.stdout:
        f.close()


if __name__ == "__main__":
    main()
