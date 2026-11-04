#!/usr/bin/env python3
"""Spiral galaxy: stars along logarithmic spiral arms plus a central bulge.

Colour runs from hot blue-white arms to a warm yellow-orange core.
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from givout import write_points, write_buckets, hsv_hex


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", "--num-points", type=int, default=1_000_000)
    ap.add_argument("--arms", type=int, default=4)
    ap.add_argument("--twist", type=float, default=1.6, help="spiral pitch: radians per e-fold of radius (default %(default)s)")
    ap.add_argument("-o", "--output", default="-")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mark-size", type=float, default=1.0)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)

    n_bulge = a.num_points // 5
    n_disk = a.num_points - n_bulge
    # disk: logarithmic spiral arms; angular scatter shrinks with the arm
    # being "tight" near the core and widens outwards
    r = (0.04 + rng.gamma(2.2, 0.16, n_disk)).clip(0.04, 1.0)
    arm = rng.integers(0, a.arms, n_disk)
    ang = arm * 2 * np.pi / a.arms + a.twist * np.log(r / 0.04) + rng.normal(0, 0.22, n_disk)
    x = r * np.cos(ang) + rng.normal(0, 0.012, n_disk)
    y = r * np.sin(ang) + rng.normal(0, 0.012, n_disk)
    disk = np.stack([x, y], axis=1)
    # bulge: 3D-ish gaussian blob
    bulge = rng.normal(0, 0.07, (n_bulge, 2))

    f = sys.stdout if a.output == "-" else open(a.output, "w")
    f.write(f"# spiral galaxy, {a.num_points} stars\n")
    f.write("$scale_marks 0\n")
    rd = np.hypot(disk[:, 0], disk[:, 1])
    # blue (h=.6) at the rim -> yellow (h=.12) towards the centre
    write_buckets(f, disk * 500, np.clip(1 - rd / 1.0, 0, 0.999),
                  lambda t: hsv_hex(0.6 - 0.48 * t, 0.7 - 0.2 * t, 1.0),
                  24, "disk", mark_size=a.mark_size, alpha=0.6)
    write_points(f, bulge * 500, "bulge", "#ffd27f", a.mark_size, alpha=0.6)
    if f is not sys.stdout:
        f.close()


if __name__ == "__main__":
    main()
