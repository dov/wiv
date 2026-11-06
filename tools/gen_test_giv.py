#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Generate synthetic .giv files for wiv performance testing.

Usage:
    gen_test_giv.py points 10000000 -o big_scatter.giv
    gen_test_giv.py points 1000000 --clusters 20 -o clustered.giv
    gen_test_giv.py lines 1000 5000 -o long_polylines.giv

Modes:
    points   - a single dataset of N scattered points ($marks fcircle, $noline)
    lines    - N long polylines of `length` vertices each, for line-rendering
               stress testing (thick/dashed line batches)
"""
import argparse
import random
import sys


def gen_points(n, clusters, seed, extent):
    rng = random.Random(seed)
    print("# synthetic point cloud for wiv perf testing")
    print(f"$path scatter_{n}")
    print("$noline")
    print("$marks fcircle")
    print("$mark_size 2")
    print("$color steelblue")

    if clusters <= 1:
        for _ in range(n):
            x = rng.uniform(-extent, extent)
            y = rng.uniform(-extent, extent)
            print(f"{x:.4f} {y:.4f}")
    else:
        centers = [(rng.uniform(-extent, extent), rng.uniform(-extent, extent)) for _ in range(clusters)]
        spread = extent / (clusters ** 0.5) / 4.0
        for i in range(n):
            cx, cy = centers[i % clusters]
            x = rng.gauss(cx, spread)
            y = rng.gauss(cy, spread)
            print(f"{x:.4f} {y:.4f}")


def gen_lines(count, length, seed, extent):
    rng = random.Random(seed)
    colors = ["red", "green", "blue", "orange", "purple", "midnightblue"]
    for i in range(count):
        print(f"$path polyline_{i}")
        print(f"$color {colors[i % len(colors)]}")
        print("$lw 2")
        if i % 3 == 0:
            print("$linedash 10,5")
        x = rng.uniform(-extent, extent)
        y = rng.uniform(-extent, extent)
        for j in range(length):
            if j > 0:
                x += rng.uniform(-extent, extent) / length * 4
                y += rng.uniform(-extent, extent) / length * 4
            print(f"{x:.4f} {y:.4f}")
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    pp = sub.add_parser("points")
    pp.add_argument("count", type=int)
    pp.add_argument("--clusters", type=int, default=1)
    pp.add_argument("--extent", type=float, default=10000.0)
    pp.add_argument("--seed", type=int, default=1)
    pp.add_argument("-o", "--output")

    lp = sub.add_parser("lines")
    lp.add_argument("count", type=int)
    lp.add_argument("length", type=int)
    lp.add_argument("--extent", type=float, default=1000.0)
    lp.add_argument("--seed", type=int, default=1)
    lp.add_argument("-o", "--output")

    args = ap.parse_args()

    out = open(args.output, "w") if args.output else sys.stdout
    old_stdout = sys.stdout
    sys.stdout = out
    try:
        if args.mode == "points":
            gen_points(args.count, args.clusters, args.seed, args.extent)
        elif args.mode == "lines":
            gen_lines(args.count, args.length, args.seed, args.extent)
    finally:
        sys.stdout = old_stdout
        if args.output:
            out.close()


if __name__ == "__main__":
    main()
