#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Wraps a screenshot of a window's client area in a generic window frame
(title bar with three buttons, thin border, soft shadow), so it stands out from
a page instead of blending into it.

    tools/decorate_window.py IN.png [-o OUT.png] [-t TITLE] [--width 1000]

Without -o the input file is overwritten. --width downscales a wider image
first (LANCZOS). The result is a PNG with a transparent margin for the shadow.
Also importable: decorate(path, title, out=None, width=None). Needs Pillow.
"""
import argparse, os


def decorate(path, title, out=None, width=None):
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
    im = Image.open(path).convert("RGB")
    if width and im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    bar, pad = 32, 24
    w, h = im.width + 2, im.height + bar + 1
    win = Image.new("RGB", (w, h), (170, 170, 175))          # 1px border colour
    d = ImageDraw.Draw(win)
    d.rectangle([1, 1, w - 2, bar], fill=(236, 236, 238))     # title bar
    d.line([1, bar, w - 2, bar], fill=(200, 200, 204))
    for i, col in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        cx = 18 + i * 22
        d.ellipse([cx - 7, bar // 2 - 7, cx + 7, bar // 2 + 7], fill=col)
    font = None
    for f in ("/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
              "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "C:/Windows/Fonts/segoeui.ttf"):
        if os.path.exists(f):
            font = ImageFont.truetype(f, 14)
            break
    font = font or ImageFont.load_default()
    tw = d.textlength(title, font=font)
    d.text(((w - tw) / 2, bar // 2 - 9), title, fill=(70, 70, 75), font=font)
    win.paste(im, (1, bar + 1))
    canvas = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rectangle([pad, pad + 6, pad + w, pad + h + 6], fill=(0, 0, 0, 90))
    canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(9)))
    canvas.paste(win.convert("RGBA"), (pad, pad))
    canvas.save(out or path, optimize=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output")
    ap.add_argument("-t", "--title", default="wiv")
    ap.add_argument("--width", type=int, help="downscale images wider than this")
    a = ap.parse_args()
    decorate(a.input, a.title, a.output, a.width)


if __name__ == "__main__":
    main()
