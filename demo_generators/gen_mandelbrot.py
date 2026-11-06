#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Mandelbrot set as a point cloud: one point per pixel centre of a regular
grid (about N points in total), kept only if it lies in or near the set and
coloured by escape-time. Points inside the set are drawn black.
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from givout import write_points, hsv_hex


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--num-points", type=int, default=1_000_000,
                    help="approximate grid size (default %(default)s)")
    ap.add_argument("--iter", type=int, default=200, help="max iterations (default %(default)s)")
    ap.add_argument("--center", type=float, nargs=2, default=(-0.65, 0.0), metavar=("RE", "IM"))
    ap.add_argument("--width", type=float, default=3.2, help="width in the complex plane")
    ap.add_argument("-o", "--output", default="-")
    ap.add_argument("--mark-size", type=float, default=1.5)
    a = ap.parse_args()

    aspect = 2 / 3
    nx = int(np.sqrt(a.num_points / aspect))
    ny = int(nx * aspect)
    h = a.width * aspect
    xs = a.center[0] + np.linspace(-a.width / 2, a.width / 2, nx)
    ys = a.center[1] + np.linspace(-h / 2, h / 2, ny)
    c = (xs[None, :] + 1j * ys[:, None])
    z = np.zeros_like(c)
    esc = np.zeros(c.shape, dtype=np.int32)
    alive = np.ones(c.shape, dtype=bool)
    for i in range(1, a.iter + 1):
        z[alive] = z[alive] ** 2 + c[alive]
        gone = alive & (np.abs(z) > 2)
        esc[gone] = i
        alive &= ~gone

    pixel = a.width / nx
    X = np.broadcast_to(xs[None, :], c.shape) / pixel
    Y = np.broadcast_to(ys[:, None], c.shape) / pixel * -1   # Y down
    f = sys.stdout if a.output == "-" else open(a.output, "w")
    f.write(f"# Mandelbrot set, {nx}x{ny} grid, {a.iter} iterations\n")
    f.write("$scale_marks 1\n")
    write_points(f, np.stack([X[alive], Y[alive]], 1), "inside", "black", a.mark_size, mark="fsquare")
    for it in np.unique(esc[esc > 0]):
        sel = esc == it
        t = (it % 64) / 64
        write_points(f, np.stack([X[sel], Y[sel]], 1), f"escape/{it:04d}",
                     hsv_hex(0.66 + 0.9 * t, 0.9, 1.0 if it > 2 else 0.5), a.mark_size, mark="fsquare")
    if f is not sys.stdout:
        f.close()


if __name__ == "__main__":
    main()
