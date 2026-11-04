"""Tiny helper for the demo generators: write numpy point sets as .giv datasets."""
import colorsys
import numpy as np


def hexcolor(r, g, b):
    return "#%02x%02x%02x" % (int(r * 255), int(g * 255), int(b * 255))


def hsv_hex(h, s=1.0, v=1.0):
    return hexcolor(*colorsys.hsv_to_rgb(h % 1.0, s, v))


def write_points(f, xy, name, color, mark_size=1.5, mark="fcircle", alpha=None):
    """Write one marks-only dataset of an (N,2) array."""
    f.write(f"$path {name}\n$noline\n$marks {mark}\n$mark_size {mark_size}\n")
    f.write(f"$color {color}" + (f"/{alpha}" if alpha is not None else "") + "\n")
    np.savetxt(f, xy, fmt="%.5g")
    f.write("\n")


def write_buckets(f, xy, t, colorfn, nbuckets, name, **kw):
    """Split points by t in [0,1) into nbuckets datasets, coloured by colorfn(t)."""
    idx = np.clip((t * nbuckets).astype(int), 0, nbuckets - 1)
    for b in range(nbuckets):
        sel = xy[idx == b]
        if len(sel):
            write_points(f, sel, f"{name}/{b:03d}", colorfn((b + 0.5) / nbuckets), **kw)


class SpoolWriter:
    """Collects points for several named datasets chunk by chunk.

    Each dataset is spooled to its own temp file so memory stays flat no matter
    how many points are generated; finish() concatenates them into the output as
    one contiguous dataset each.
    """

    def __init__(self, tmpdir):
        import os
        self.dir, self.meta, self.order = tmpdir, {}, []
        self._os = os

    def add_dataset(self, key, name, color, **kw):
        self.meta[key] = (name, color, kw)
        self.order.append(key)
        open(self._path(key), "wb").close()

    def _path(self, key):
        return self._os.path.join(self.dir, f"spool_{key}.txt")

    def append(self, key, xy):
        if len(xy):
            with open(self._path(key), "ab") as f:
                np.savetxt(f, xy, fmt="%.5g")

    def finish(self, out):
        for key in self.order:
            name, color, kw = self.meta[key]
            path = self._path(key)
            if self._os.path.getsize(path) == 0:
                continue
            write_points(out, np.empty((0, 2)), name, color, **kw)
            out.flush()
            with open(path, "rb") as src:
                while True:
                    buf = src.read(1 << 22)
                    if not buf:
                        break
                    out.buffer.write(buf) if hasattr(out, "buffer") else out.write(buf.decode())
            out.write("\n")
            self._os.remove(path)
