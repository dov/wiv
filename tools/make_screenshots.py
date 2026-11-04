#!/usr/bin/env python3
"""Regenerates the screenshots in docs/images/ by remote-driving a real wiv.

    tools/make_screenshots.py [--wiv build/linux-release/wiv] [--only NAME ...]

Starts wiv with --rpc-port and a throwaway config directory (your own settings
are not touched), loads each example, sets up the view through the JSON-RPC
methods (load_file, trigger_action, set_colormap, resize_window, ...) and
saves the window with `screenshot`. Needs a desktop session (X11).
"""
import argparse, json, os, shutil, subprocess, sys, tempfile, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decorate_window import decorate  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EX = os.path.join(ROOT, "examples")
OUT = os.path.join(ROOT, "docs", "images")
PORT = 8765


def call(method, *params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": list(params)}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}/", body)
    r = json.load(urllib.request.urlopen(req, timeout=60))
    if "error" in r:
        raise RuntimeError(f"{method}: {r['error']['message']}")
    return r["result"]


def settle(t=0.6):
    time.sleep(t)


def load(name):
    call("load_file", os.path.join(EX, name))
    settle(1.0)
    call("trigger_action", "FittoWindow")
    settle()


def zoom_out(n=1):
    return lambda: [call("trigger_action", "ZoomOut") or settle(0.3) for _ in range(n)]


def contrast(lo, hi):
    return lambda: call("set_contrast", lo, hi)


def hover():
    """Parks the pointer over the middle of the drawing so its balloon shows."""
    settle(0.5)
    call("mouse_move")
    settle(1.2)


def both(*fs):
    return lambda: [f() for f in fs]


# name -> (example file, window size, actions to switch on, setup callable run
# after the fit). Extra zoom-outs where the fit ignores text extents.
def shot_defs():
    return [
        ("image-example", "lena-glasses.giv", (700, 650), [], None),
        ("balloon", "multi-line-balloon.giv", (1000, 600), ["BalloonTooltips"], hover),
        ("overview", "alpha-example.giv", (1000, 700), [], None),
        ("graph", "3x3-graph.giv", (800, 650), [], None),
        ("text-markup", "text-markup.giv", (900, 650), [], zoom_out(2)),
        ("bezier", "bezier.giv", (900, 650), [], None),
        ("arrows", "arrows.giv", (900, 650), [], None),
        ("quiver", "quiver.giv", (900, 650), [], None),
        ("voronoi", "voronoi.giv", (900, 650), [], None),
        ("polygon-holes", "polygonholes.giv", (900, 650), [], None),
        ("svg-text", "svg-text-advanced.svg", (900, 650), [], zoom_out(1)),
        ("mark-browser", "colors.giv", (1100, 650), ["MarkBrowser"], None),
        ("fits-contrast", "m51.fits", (1100, 700), ["AdjustContrast"], contrast(0.0, 0.15)),
        ("fits-colormap", "m51.fits", (1100, 700), ["ColorTable", "ColormapBar"],
         both(contrast(0.0, 0.15), lambda: call("set_colormap", "Rainbow"))),
        ("image-info", "mri-orange.fits", (1100, 700), ["ImageInfo"], None),
        ("pixel-grid", "mri-orange.fits", (900, 650), ["PixelGrid"],
         lambda: [call("set_contrast", 0, 600) if a == "_contrast" else call("trigger_action", a) or settle(0.4) for a in ["ActualPixels11"] + ["ZoomIn"] * 14 + ["_contrast"]]),
        # needs barnsley_10M.giv in the repo root:
        #   demo_generators/gen_barnsley.py -n 10000000 -o barnsley_10M.giv
        ("big-fern", "../barnsley_10M.giv", (900, 700), [], lambda: settle(4.0)),
        ("thumbnails", "alpha-example.giv", (1000, 700), ["ThumbnailStrip"], None),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wiv", default=os.path.join(ROOT, "build", "linux-release", "wiv"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--width", type=int, default=1000, help="downscale wider shots to this width (needs Pillow)")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    cfg = tempfile.mkdtemp(prefix="wiv-shots-")
    env = dict(os.environ, XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=cfg, XDG_CACHE_HOME=cfg)
    proc = subprocess.Popen([args.wiv, "--rpc-port", str(PORT), "--geometry", "1000x700"],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                if call("ping") == "pong":
                    break
            except Exception:
                time.sleep(0.2)
        else:
            sys.exit("wiv did not start")

        # Known starting state: no thumbnail strip unless a shot asks for it.
        call("trigger_action", "ThumbnailStrip", False)
        for name, example, size, actions, extra in shot_defs():
            if args.only and name not in args.only:
                continue
            thumbs = "ThumbnailStrip" in actions
            call("resize_window", *size)
            load(example)
            for a in actions:
                call("trigger_action", a, True)
            settle(0.5)
            if extra and name != "pixel-grid" and extra is not hover:
                extra()
            if extra and name == "pixel-grid":
                extra()
            if name != "pixel-grid":
                call("trigger_action", "FittoWindow")
            settle(1.0)
            if extra is hover:
                extra()
            path = os.path.join(OUT, name + ".png")
            call("screenshot", path)
            try:
                from PIL import Image
                decorate(path, "wiv - " + os.path.basename(example), width=args.width)
            except ImportError:
                pass
            print("wrote", os.path.relpath(path, ROOT))
            # Back to the neutral state for the next shot.
            for a in actions:
                if a != "ThumbnailStrip":
                    call("trigger_action", a, False)
            if thumbs:
                call("trigger_action", "ThumbnailStrip", False)
            call("set_colormap", "")
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(cfg, ignore_errors=True)


if __name__ == "__main__":
    main()
