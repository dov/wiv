#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Chirikov standard map phase portrait.

    p' = p + K sin(theta)      theta' = theta + p'     (both mod 2*pi)

Orbits from a grid of initial conditions are iterated; each orbit gets its own
colour, showing KAM tori, island chains and the chaotic sea as K grows.
"""
import argparse, os, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from givout import SpoolWriter, hsv_hex

TWO_PI = 2 * np.pi


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--num-points", type=int, default=1_000_000,
                    help="total number of points (default %(default)s)")
    ap.add_argument("-K", type=float, default=0.97, help="kick strength (default %(default)s)")
    ap.add_argument("--orbits", type=int, default=400, help="number of orbits (default %(default)s)")
    ap.add_argument("-o", "--output", default="-", help="output .giv file (default stdout)")
    ap.add_argument("--chunk", type=int, default=5_000_000,
                    help="points generated per in-memory chunk (default %(default)s)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mark-size", type=float, default=1.0)
    a = ap.parse_args()

    rng = np.random.default_rng(a.seed)
    steps = max(1, a.num_points // a.orbits)
    chunk_steps = max(1, a.chunk // a.orbits)
    th = rng.uniform(0, TWO_PI, a.orbits)
    p = rng.uniform(0, TWO_PI, a.orbits)

    f = sys.stdout if a.output == "-" else open(a.output, "w")
    f.write(f"# standard map K={a.K}, {a.orbits} orbits x {steps} steps\n")
    with tempfile.TemporaryDirectory(dir=None if a.output == "-" else
                                     os.path.dirname(os.path.abspath(a.output))) as tmp:
        sp = SpoolWriter(tmp)
        for o in range(a.orbits):
            sp.add_dataset(o, f"orbit/{o:04d}", hsv_hex(o * 0.618034, 0.8, 1.0),
                           mark_size=a.mark_size)
        done = 0
        while done < steps:
            n = min(chunk_steps, steps - done)
            out = np.empty((n, a.orbits, 2))
            for i in range(n):
                p = (p + a.K * np.sin(th)) % TWO_PI
                th = (th + p) % TWO_PI
                out[i, :, 0], out[i, :, 1] = th, p
            for o in range(a.orbits):
                sp.append(o, out[:, o, :])
            done += n
            print(f"\r{done * a.orbits}/{steps * a.orbits}", end="", file=sys.stderr)
        print(file=sys.stderr)
        sp.finish(f)
    if f is not sys.stdout:
        f.close()


if __name__ == "__main__":
    main()
