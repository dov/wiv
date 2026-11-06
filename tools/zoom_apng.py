#!/usr/bin/env python3
"""Records a smooth zoom into a point of a dataset as an APNG, driving a real wiv.

    tools/zoom_apng.py demo_generators/barnsley_10M.giv --target -1.121 -4.728 \
        --zoom 50 -o barnsley_zoom.apng

Starts wiv with --rpc-port and a throwaway config directory, fits the file to
the window, then for each frame sets the view (set_transformation) and grabs
the whole window (screenshot "window", so the menus and the thumbnail strip with
the other files of the directory are included).  Frames are assembled with
ffmpeg.  Needs a desktop session (X11) and ffmpeg.
An output ending in .webp gives a (much smaller) lossy animated WebP instead of an APNG.
Use --mode export to render only the plot with export_image (no UI).
"""
import argparse, json, os, shutil, subprocess, sys, tempfile, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 8766


def call(method, *params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": list(params)}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}/", body)
    r = json.load(urllib.request.urlopen(req, timeout=120))
    if "error" in r:
        raise RuntimeError(f"{method}: {r['error']['message']}")
    return r["result"]


def smooth(t):
    return t * t * (3 - 2 * t)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--target", nargs=2, type=float, required=True, metavar=("X", "Y"))
    ap.add_argument("--zoom", type=float, default=50, help="final magnification relative to the fitted view")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--hold", type=int, default=15, help="frames to hold at the start and at the end")
    ap.add_argument("--width", type=int, default=800, help="scale the output to this width (0 = keep)")
    ap.add_argument("--decorate", metavar="TITLE", help="wrap every frame in the docs window frame (tools/decorate_window.py) with this title")
    ap.add_argument("--quality", type=int, default=75, help="WebP quality 0-100 (output ending in .webp)")
    ap.add_argument("--geometry", default="1000x700")
    ap.add_argument("--mode", choices=["window", "export"], default="window")
    ap.add_argument("--wiv", default=os.path.join(ROOT, "build", "linux-release", "wiv"))
    ap.add_argument("--keep-frames", help="directory to keep the PNG frames in")
    ap.add_argument("-o", "--output", default="zoom.apng")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="zoom_apng_")
    frames = args.keep_frames or os.path.join(tmp, "frames")
    os.makedirs(frames, exist_ok=True)
    cfg = os.path.join(tmp, "cfg")
    os.makedirs(cfg)
    env = dict(os.environ, XDG_CONFIG_HOME=cfg, XDG_DATA_HOME=cfg, XDG_CACHE_HOME=cfg)
    proc = subprocess.Popen([args.wiv, "--rpc-port", str(PORT), "--geometry", args.geometry, args.file],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                call("ping")
                break
            except Exception:
                time.sleep(0.2)
        else:
            sys.exit("wiv did not start")
        time.sleep(2.0)
        # Known state: thumbnail strip on, view fitted to the window.
        call("trigger_action", "ThumbnailStrip", True)
        time.sleep(1.0)
        call("trigger_action", "FittoWindow")
        time.sleep(1.0)
        s0, _, cx0, cy0 = call("get_transformation")
        tx, ty = args.target[0], -args.target[1]  # wiv's view y is the negative of the data y
        print(f"start view: scale {s0:g}, centre ({cx0:g}, {cy0:g})", file=sys.stderr)

        n = args.frames
        seq = [0.0] * args.hold + [i / (n - 1) for i in range(n)] + [1.0] * args.hold
        last = None
        for k, t in enumerate(seq):
            path = os.path.join(frames, f"f{k:04d}.png")
            if t != last:
                e = smooth(t)
                s = s0 * (args.zoom ** e)
                # The target's screen offset from the window centre shrinks
                # linearly (in pixels) to zero, so it ends up centred.
                f = s0 / s * (1 - e)
                call("set_transformation", s, s, tx + (cx0 - tx) * f, ty + (cy0 - ty) * f)
                if args.mode == "window":
                    call("screenshot", path, "window")
                else:
                    w, h = (int(v) for v in args.geometry.split("x"))
                    call("export_image", path, w, h)
                last = t
            else:
                shutil.copy(os.path.join(frames, f"f{k - 1:04d}.png"), path)
            print(f"\rframe {k + 1}/{len(seq)}", end="", file=sys.stderr)
        print(file=sys.stderr)
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except Exception:
            proc.kill()

    vf = ["-vf", f"scale={args.width}:-2:flags=lanczos"] if args.width else []
    if args.decorate:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from decorate_window import decorate
        # Decorate (and downscale) each distinct frame once; held frames are copies.
        done = {}
        names = sorted(f for f in os.listdir(frames) if f.endswith(".png"))
        for i, f in enumerate(names):
            src = os.path.join(frames, f)
            key = open(src, "rb").read()
            if key in done:
                shutil.copy(done[key], src)
            else:
                decorate(src, args.decorate, width=args.width or None)
                done[key] = src
            print(f"\rdecorating {i + 1}/{len(names)}", end="", file=sys.stderr)
        print(file=sys.stderr)
        vf = []
    if args.output.lower().endswith(".webp"):  # lossy animated WebP: far smaller than APNG for noisy scatter
        enc = ["-c:v", "libwebp_anim", "-lossless", "0", "-q:v", str(args.quality), "-compression_level", "6", "-loop", "0"]
    else:
        enc = ["-plays", "0", "-pred", "mixed", "-f", "apng"]
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(args.fps), "-i",
                    os.path.join(frames, "f%04d.png")] + vf + enc + [args.output], check=True)
    print(f"wrote {args.output}", file=sys.stderr)
    if not args.keep_frames:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
