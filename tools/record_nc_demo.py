#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
"""Records the netCDF demo animation of the README by driving a real wiv with
the real mouse (xdotool) and recording the screen (ffmpeg x11grab), so the
cursor, the Color table drop-down and the histogram handles are all visible.

    tools/record_nc_demo.py --dry-run          # check where the mouse will go
    tools/record_nc_demo.py                    # record docs/images/huge-nc-file.webp

The story (extra_examples/earth_ir.nc, 470 megapixels):
  1. full-range view, colour table None
  2. open the Color table drop-down, choose Blue Green Red Yellow
  3. drag the histogram's contrast handles to the final window
  4. drag the gamma handle to the final gamma
  5. move the mouse to image pixel (16000, 12000)
  6. double-click: zoom to 1:1 about the pointer
  7. double-click: zoom back out to the fitted view

Needs an X11 session (XWayland works when the pointer is over the window),
xdotool, xwininfo (x11-utils), ffmpeg with libwebp_anim, Pillow.  The mouse is
yours for the duration of the recording: do not touch it.

All screen positions are in window-client pixels of a 1280x960 window with the
Image Info and Adjust Contrast docks on the right (measured from a fresh config).  If your dock layout differs, run --dry-run, look at the markers
in the saved picture and adjust the LAYOUT constants below (or pass
--use-my-config to start wiv with your own settings and dock arrangement).
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 8767

# ---- layout of the 1280x960 window (client pixels, device pixel ratio 1) -----
GEOMETRY = (1280, 960)
COMBO = (1070, 643)            # Color table drop-down
HIST_Y_RANGE = 812             # row of the blue min/max handles in the histogram
HIST_Y_GAMMA = 866             # row of the white gamma triangle
HIST_X0, HIST_K = 842.6, 0.6372  # histogram x = HIST_X0 + (value - FULL_MIN) * HIST_K
RESET_BUTTON = (1150, 680)       # gamma Reset
FULL_RANGE_BUTTON = (1103, 909)  # histogram "Full range"
FIT_LEFT, FIT_TOP, FIT_SIDE = 14, 26, 748  # where the fitted square image sits in the viewport
# ---- the story --------------------------------------------------------------
IMAGE_SIDE = 21696
FULL = (-3.16194, 628.987)     # full range of earth_ir.nc
WINDOW = (-7.59807, 300.383)   # final contrast window
GAMMA = 1.727                  # final gamma
TARGET = (16000, 12000)        # image pixel that the pointer visits and 1:1 zooms about
COLORMAP = "Blue Green Red Yellow"
COLORMAP_INDEX = 7             # position in the drop-down (None = 0), see Colormaps.h


def rpc(method, *params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": list(params)}).encode()
    r = json.load(urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{PORT}/", body), timeout=120))
    if "error" in r:
        raise RuntimeError(f"{method}: {r['error']['message']}")
    return r["result"]


def xdo(*a):
    subprocess.run(["xdotool", *map(str, a)], check=True)


def smooth(t):
    return t * t * (3 - 2 * t)


class Screen:
    """Mouse helper in window-client coordinates."""

    def __init__(self, ox, oy):
        self.ox, self.oy = ox, oy

    def pos(self):
        out = subprocess.check_output(["xdotool", "getmouselocation", "--shell"], text=True)
        d = dict(line.split("=") for line in out.split())
        return int(d["X"]) - self.ox, int(d["Y"]) - self.oy

    def jump(self, x, y):
        xdo("mousemove", round(self.ox + x), round(self.oy + y))

    def glide(self, x, y, dur=0.8):
        x0, y0 = self.pos()
        n = max(2, int(dur * 50))
        t0 = time.time()
        for i in range(1, n + 1):
            e = smooth(i / n)
            self.jump(x0 + (x - x0) * e, y0 + (y - y0) * e)
            time.sleep(max(0, t0 + dur * i / n - time.time()))

    def click(self):
        xdo("click", 1)

    def double_click(self):
        xdo("click", "--repeat", 2, "--delay", 90, 1)

    def drag(self, x, y, dur=1.0):
        xdo("mousedown", 1)
        time.sleep(0.15)
        self.glide(x, y, dur)
        time.sleep(0.1)
        xdo("mouseup", 1)


def hist_x(value):
    return HIST_X0 + (value - FULL[0]) * HIST_K


def gamma_x(lo, hi, gamma):
    return hist_x(lo) + (hist_x(hi) - hist_x(lo)) * 0.5 ** gamma


def image_to_view(px, py):
    s = FIT_SIDE / IMAGE_SIDE
    return FIT_LEFT + px * s, FIT_TOP + py * s


def find_window(pid, size, timeout=30):
    """(x, y) of the client area of wiv's top-level window, via xwininfo."""
    end = time.time() + timeout
    while time.time() < end:
        r = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(pid)], capture_output=True, text=True)
        for wid in r.stdout.split():
            info = subprocess.run(["xwininfo", "-id", wid], capture_output=True, text=True).stdout
            g = lambda k: int(re.search(rf"{k}:\s+(-?\d+)", info).group(1))
            if g("Width") == size[0] and g("Height") == size[1]:
                return wid, g("Absolute upper-left X"), g("Absolute upper-left Y")
        time.sleep(0.3)
    sys.exit(f"no {size[0]}x{size[1]} wiv window found (is this an X11 session?)")


def markers_picture(path, out):
    from PIL import Image, ImageDraw
    im = Image.open(path).convert("RGB")
    d = ImageDraw.Draw(im)
    pts = {"combo": COMBO,
           "min handle": (hist_x(FULL[0]), HIST_Y_RANGE), "max handle": (hist_x(FULL[1]), HIST_Y_RANGE),
           "min target": (hist_x(WINDOW[0]), HIST_Y_RANGE), "max target": (hist_x(WINDOW[1]), HIST_Y_RANGE),
           "gamma start": (gamma_x(*WINDOW, 1.0), HIST_Y_GAMMA), "gamma target": (gamma_x(*WINDOW, GAMMA), HIST_Y_GAMMA),
           "image target": image_to_view(*TARGET)}
    for name, (x, y) in pts.items():
        d.line([x - 8, y, x + 8, y], fill=(255, 0, 255), width=2)
        d.line([x, y - 8, x, y + 8], fill=(255, 0, 255), width=2)
        d.text((x + 10, y - 6), name, fill=(255, 0, 255))
    im.save(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?", default=os.path.join(ROOT, "extra_examples", "earth_ir.nc"))
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "docs", "images", "huge-nc-file.webp"))
    ap.add_argument("--wiv", default=os.path.join(ROOT, "build", "linux-release", "wiv"))
    ap.add_argument("--dry-run", action="store_true",
                    help="start wiv, set the initial state, put the pointer on the image target and save "
                         "dry-run.png with the planned positions marked; do not record")
    ap.add_argument("--use-my-config", action="store_true",
                    help="keep your own wiv settings and dock layout instead of a throwaway config")
    ap.add_argument("--fps", type=int, default=20)
    ap.add_argument("--width", type=int, default=900, help="output width, including the window frame")
    ap.add_argument("--quality", type=int, default=70, help="WebP quality 0-100")
    ap.add_argument("--title", default="wiv - earth_ir.nc")
    ap.add_argument("--keep", metavar="DIR", help="keep the raw recording and the PNG frames here")
    args = ap.parse_args()

    for tool in ("xdotool", "xwininfo", "ffmpeg"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")
    if os.environ.get("XDG_SESSION_TYPE") == "wayland":
        print("warning: Wayland session; pointer control only works over XWayland windows", file=sys.stderr)

    tmp = tempfile.mkdtemp(prefix="nc_demo_")
    # DPR 1 so that screen pixels equal the layout constants above (Qt would otherwise follow Xft.dpi)
    env = dict(os.environ, QT_QPA_PLATFORM="xcb", QT_ENABLE_HIGHDPI_SCALING="0")
    if not args.use_my_config:
        cfg = os.path.join(tmp, "cfg")
        os.makedirs(cfg)
        env.update(XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=cfg, XDG_CACHE_HOME=cfg)
    proc = subprocess.Popen([args.wiv, "--rpc-port", str(PORT), "--geometry", f"{GEOMETRY[0]}x{GEOMETRY[1]}", args.file],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rec = None
    try:
        for _ in range(150):
            try:
                rpc("ping")
                break
            except Exception:
                time.sleep(0.2)
        else:
            sys.exit("wiv did not start")
        wid, ox, oy = find_window(proc.pid, GEOMETRY)
        xdo("windowmove", wid, 100, 100)  # fully on screen, away from panels
        xdo("windowfocus", wid)
        time.sleep(1.0)
        _, ox, oy = find_window(proc.pid, GEOMETRY)
        scr = Screen(ox, oy)
        print(f"window client area at {ox},{oy}", file=sys.stderr)

        # Known initial state: docks on, full-range, no colour table, fitted.
        time.sleep(3.0)  # the 470 MP image loads; the contrast dock can only be opened after that
        if not args.use_my_config:
            # A fresh config starts with both docks hidden.  Their toggles are not RPC actions,
            # so press their shortcuts (I = Image Info, C = Adjust Contrast).  With
            # --use-my-config the docks are assumed to be open already (a key press would close them).
            for key in ("i", "c"):
                xdo("key", key)
                time.sleep(0.8)
            time.sleep(1.0)
        rpc("set_colormap", "None")
        rpc("trigger_action", "FittoWindow")
        # Use the panel's own buttons so that the histogram handles agree with the contrast
        # (the RPC set_contrast does not refresh the dock): Reset = gamma 1, then Full range.
        scr.glide(*RESET_BUTTON, 0.3)
        scr.click()
        scr.glide(*FULL_RANGE_BUTTON, 0.3)
        scr.click()
        rpc("set_colormap", "None")
        time.sleep(1.5)

        if args.dry_run:
            scr.glide(*image_to_view(*TARGET), 0.5)
            time.sleep(0.8)
            shot = os.path.join(tmp, "shot.png")
            rpc("screenshot", shot, "window")
            out = os.path.abspath("dry-run.png")
            markers_picture(shot, out)
            print(f"wrote {out}: check the markers (and the pixel coordinates in the status bar; "
                  f"should read about {TARGET[0]}, {TARGET[1]})", file=sys.stderr)
            return

        raw = os.path.join(tmp, "rec.mkv")
        rec = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab", "-framerate", str(args.fps),
                                "-video_size", f"{GEOMETRY[0]}x{GEOMETRY[1]}", "-draw_mouse", "1",
                                "-i", f"{env.get('DISPLAY', ':0')}+{ox},{oy}",
                                "-c:v", "libx264", "-qp", "0", "-preset", "ultrafast", "-pix_fmt", "yuv444p", raw],
                               stdin=subprocess.PIPE)
        time.sleep(1.5)  # 1. initial view

        # 2. colour table
        scr.glide(*COMBO, 1.0)
        time.sleep(0.3)
        scr.click()
        time.sleep(0.8)
        for _ in range(COLORMAP_INDEX):
            xdo("key", "Down")
            time.sleep(0.12)
        time.sleep(0.5)
        xdo("key", "Return")
        time.sleep(1.2)

        # 3. contrast window: max handle first, then min handle
        scr.glide(hist_x(FULL[1]), HIST_Y_RANGE, 0.9)
        scr.drag(hist_x(WINDOW[1]), HIST_Y_RANGE, 1.4)
        time.sleep(0.4)
        scr.glide(hist_x(FULL[0]), HIST_Y_RANGE, 0.6)
        scr.drag(hist_x(WINDOW[0]), HIST_Y_RANGE, 0.6)
        time.sleep(1.0)

        # 4. gamma
        scr.glide(gamma_x(*WINDOW, 1.0), HIST_Y_GAMMA, 0.8)
        scr.drag(gamma_x(*WINDOW, GAMMA), HIST_Y_GAMMA, 1.2)
        time.sleep(1.2)

        # 5. to the target pixel
        scr.glide(*image_to_view(*TARGET), 1.4)
        time.sleep(1.0)
        # 6. zoom to 1:1
        scr.double_click()
        time.sleep(2.5)
        # 7. and back out
        scr.double_click()
        time.sleep(2.5)
        scr.glide(scr.pos()[0] + 40, scr.pos()[1] + 30, 0.6)
        time.sleep(1.0)

        rec.stdin.write(b"q")
        rec.stdin.flush()
        rec.wait(30)
        rec = None

        frames = args.keep and os.path.abspath(args.keep) or os.path.join(tmp, "frames")
        os.makedirs(frames, exist_ok=True)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", raw, os.path.join(frames, "f%04d.png")], check=True)
        if args.keep:
            shutil.copy(raw, args.keep)
    finally:
        if rec:
            rec.terminate()
        proc.terminate()
        try:
            proc.wait(5)
        except Exception:
            proc.kill()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from decorate_window import decorate
    names = sorted(f for f in os.listdir(frames) if f.endswith(".png"))
    done = {}
    for i, f in enumerate(names):  # identical (held) frames are decorated once
        src = os.path.join(frames, f)
        key = open(src, "rb").read()
        if key in done:
            shutil.copy(done[key], src)
        else:
            decorate(src, args.title, width=args.width - 50)
            done[key] = src
        print(f"\rdecorating {i + 1}/{len(names)}", end="", file=sys.stderr)
    print(file=sys.stderr)
    enc = ["-c:v", "libwebp_anim", "-lossless", "0", "-q:v", str(args.quality), "-compression_level", "6", "-loop", "0"]
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(args.fps), "-i",
                    os.path.join(frames, "f%04d.png")] + enc + [args.output], check=True)
    print(f"wrote {args.output} ({os.path.getsize(args.output) / 1e6:.1f} MB, {len(names)} frames)", file=sys.stderr)
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
