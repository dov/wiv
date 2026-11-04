#!/usr/bin/env python3
"""Barnsley fern: iterated function system with four affine maps.

Many independent walkers are iterated in parallel with numpy; points are
coloured by which map produced them (stem / leaflets / main frond).
"""
import argparse, os, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from givout import SpoolWriter

# (a b c d e f), probability  -- classic Barnsley coefficients
MAPS = np.array([
    [0.00, 0.00, 0.00, 0.16, 0.0, 0.00],
    [0.85, 0.04, -0.04, 0.85, 0.0, 1.60],
    [0.20, -0.26, 0.23, 0.22, 0.0, 1.60],
    [-0.15, 0.28, 0.26, 0.24, 0.0, 0.44],
])
PROBS = np.array([0.01, 0.85, 0.07, 0.07])
COLORS = ["#8b5a2b", "#228b22", "#32cd32", "#9acd32"]


def step(p, rng):
    k = rng.choice(4, size=len(p), p=PROBS)
    m = MAPS[k]
    x, y = p[:, 0], p[:, 1]
    return np.stack([m[:, 0] * x + m[:, 1] * y + m[:, 4],
                     m[:, 2] * x + m[:, 3] * y + m[:, 5]], axis=1), k.astype(np.int8)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--num-points", type=int, default=1_000_000,
                    help="number of points (default %(default)s)")
    ap.add_argument("-o", "--output", default="-", help="output .giv file (default stdout)")
    ap.add_argument("--chunk", type=int, default=5_000_000,
                    help="points generated per in-memory chunk (default %(default)s)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mark-size", type=float, default=1.0)
    a = ap.parse_args()

    rng = np.random.default_rng(a.seed)
    walkers = 10000
    total_steps = -(-a.num_points // walkers)
    chunk_steps = max(1, a.chunk // walkers)
    p = rng.random((walkers, 2))
    for _ in range(20):                     # burn-in
        p, _k = step(p, rng)

    f = sys.stdout if a.output == "-" else open(a.output, "w")
    f.write(f"# Barnsley fern, {a.num_points} points\n")
    names = {1: "main", 2: "left", 3: "right", 0: "stem"}
    with tempfile.TemporaryDirectory(dir=None if a.output == "-" else
                                     os.path.dirname(os.path.abspath(a.output))) as tmp:
        sp = SpoolWriter(tmp)
        for k in (1, 2, 3, 0):
            sp.add_dataset(k, f"fern/{names[k]}", COLORS[k], mark_size=a.mark_size)
        done = 0
        while done < total_steps:
            n = min(chunk_steps, total_steps - done)
            pts = np.empty((n, walkers, 2))
            which = np.empty((n, walkers), dtype=np.int8)
            for i in range(n):
                p, which[i] = step(p, rng)
                pts[i] = p
            pts = pts.reshape(-1, 2)
            which = which.reshape(-1)
            remaining = a.num_points - done * walkers
            pts, which = pts[:remaining], which[:remaining]
            pts[:, 1] *= -1                 # giv has Y pointing down
            for k in names:
                sp.append(k, pts[which == k])
            done += n
            print(f"\r{min(done * walkers, a.num_points)}/{a.num_points}", end="", file=sys.stderr)
        print(file=sys.stderr)
        sp.finish(f)
    if f is not sys.stdout:
        f.close()


if __name__ == "__main__":
    main()
