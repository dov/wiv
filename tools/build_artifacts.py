#!/usr/bin/env python3
"""Build the distributable artifacts of the current wiv version on Linux.

  ./build_artifacts.py [--only installer,source,appimage] [--skip ...]
                       [--out DIR] [--native]

Artifacts (all written to dist/ by default, named with <version>-<sha>):
  installer  Windows NSIS installer, MinGW cross build (Installers/CrossBuildInstaller.sh)
  source     Source tarball of HEAD (git archive)
  appimage   Linux AppImage (linuxdeploy + Qt plugin)

The version comes from vcpkg.json, the sha from HEAD.

Portability: an AppImage bundles everything except glibc, so it runs on any
distro whose glibc is at least as new as the one it was BUILT on.  The
AppImage is therefore built by default inside an Ubuntu 22.04 container
(packaging/appimage.Containerfile, glibc 2.35, with official Qt 6.8); --native
builds on this machine instead.  A snap is deliberately not built: it needs the
same old-glibc build, only reaches snapd distros, and Vulkan under strict
confinement is fragile.  The glibc floor of the result is printed.

Environment: VCPKG_ROOT must point at a bootstrapped vcpkg (Linux preset).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALL = ["installer", "source", "appimage"]
TOOLS_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "wiv-build-tools"
LINUXDEPLOY_URL = "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_QT_URL = "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def out(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, text=True,
                          stdout=subprocess.PIPE, **kw).stdout.strip()


def version():
    m = re.search(r'"version"\s*:\s*"([^"]+)"', (ROOT / "vcpkg.json").read_text())
    if not m:
        sys.exit("No version found in vcpkg.json")
    return m.group(1)


def tag():
    return f"{version()}-{out(['git', '-C', ROOT, 'rev-parse', '--short=6', 'HEAD'])}"


def warn_if_dirty():
    st = out(["git", "-C", ROOT, "status", "--porcelain", "--", "src", "CMakeLists.txt",
              "resources", "shaders", "vcpkg.json", "cmake", "tools"])
    if st:
        print("WARNING: uncommitted/untracked changes in build inputs; the installer and\n"
              "AppImage use the working tree, the source tarball uses HEAD only:\n" + st + "\n",
              file=sys.stderr)


def need(*tools):
    missing = [t for t in tools if not shutil.which(t)]
    if missing:
        sys.exit("Missing tools: " + ", ".join(missing))


# --------------------------------------------------------------------------
def build_installer(dist: Path):
    need("x86_64-w64-mingw32-g++", "makensis", "cmake", "ninja")
    run([ROOT / "Installers/CrossBuildInstaller.sh", "--copy-to", dist])


def build_source(dist: Path):
    name = f"wiv-{tag()}"
    target = dist / f"{name}-src.tar.gz"
    run(["git", "-C", ROOT, "archive", "--format=tar.gz", f"--prefix={name}/",
         "-o", target, "HEAD"])
    print(f"Built: {target}  (note: vsgQt is fetched from GitHub at configure time)")


def fetch_tool(url: str) -> Path:
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    p = TOOLS_DIR / url.rsplit("/", 1)[1]
    if not p.exists():
        print(f"Downloading {url}")
        urllib.request.urlretrieve(url, p)
        p.chmod(0o755)
    return p


def glibc_floor(appdir: Path) -> str:
    """Highest GLIBC_x.y symbol version needed by any ELF in the AppDir."""
    best = (0, 0)
    for f in appdir.rglob("*"):
        if not f.is_file() or f.is_symlink():
            continue
        with open(f, "rb") as fh:
            if fh.read(4) != b"\x7fELF":
                continue
        txt = subprocess.run(["objdump", "-T", str(f)], text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
        for m in re.finditer(r"GLIBC_(\d+)\.(\d+)", txt):
            best = max(best, (int(m[1]), int(m[2])))
    return f"{best[0]}.{best[1]}"


def build_appimage(dist: Path):
    qmake = shutil.which("qmake6") or shutil.which("qmake")
    need("cmake", "ninja", "objdump")
    if not qmake:
        sys.exit("Missing qmake6 (Qt 6 development files)")
    vcpkg = os.environ.get("VCPKG_ROOT")
    if not vcpkg:
        sys.exit("VCPKG_ROOT is not set")
    build = ROOT / "build" / os.environ.get("WIV_BUILD_NAME", "appimage")
    appdir = build / "AppDir"

    # Prefer the distro's .pc files over /usr/local ones: a stale hand-built
    # LibRaw there (linked to a long-gone libjasper) otherwise gets bundled.
    pcp = ":".join(["/usr/lib64/pkgconfig", "/usr/lib/pkgconfig", "/usr/share/pkgconfig"]
                   + ([os.environ["PKG_CONFIG_PATH"]] if os.environ.get("PKG_CONFIG_PATH") else []))
    os.environ["PKG_CONFIG_PATH"] = pcp
    (build / "CMakeCache.txt").unlink(missing_ok=True)  # drop cached pkg-config results
    run(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={vcpkg}/scripts/buildsystems/vcpkg.cmake",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=/usr",
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"])
    run(["cmake", "--build", build])
    shutil.rmtree(appdir, ignore_errors=True)
    run(["cmake", "--install", build], env={**os.environ, "DESTDIR": str(appdir)})

    # Built with a newer GCC than the target distros may have: ship its libstdc++
    # (linuxdeploy's exclude list would otherwise leave the system one in use).
    if os.environ.get("WIV_BUNDLE_LIBSTDCXX"):
        for lib in ("libstdc++.so.6", "libgcc_s.so.1"):
            real = Path(out(["g++", f"-print-file-name={lib}"])).resolve()
            shutil.copy(real, appdir / "usr/lib" / lib)
        # linuxdeploy treats these as "system" libs, but the newer HarfBuzz is
        # required (older distros lack hb_draw_funcs) and a bare system may lack the others.
        ldd = subprocess.run(["ldd", appdir / "usr/bin/wiv"], text=True,
                             stdout=subprocess.PIPE).stdout
        for name in ("libharfbuzz.so.0", "libfribidi.so.0", "libexpat.so.1", "libOpenGL.so.0"):
            m = re.search(rf"{re.escape(name)} => (\S+)", ldd)
            if not m:
                sys.exit(f"{name} not found by ldd on usr/bin/wiv")
            shutil.copy(Path(m[1]).resolve(), appdir / "usr/lib" / name)

    # Desktop entry + icon (the app has none of its own on Linux).
    desktop = build / "wiv.desktop"
    desktop.write_text(
        "[Desktop Entry]\nType=Application\nName=wiv\nComment=Image and data viewer\n"
        "Exec=wiv %F\nIcon=wiv\nCategories=Graphics;Viewer;\nTerminal=false\n")
    icon = ROOT / "resources/wiv-app-icon.png"
    iconlink = build / "wiv.png"
    shutil.copy(icon, iconlink)

    linuxdeploy = fetch_tool(LINUXDEPLOY_URL)
    fetch_tool(LINUXDEPLOY_QT_URL)  # discovered by linuxdeploy next to itself

    # Let linuxdeploy resolve the vcpkg-built libs (vsg, tiff, ...) and the
    # libraries the dlopen'ed image plugins need.
    libdirs = [str(p) for p in (build / "vcpkg_installed").glob("*/lib") if "debug" not in p.parts]
    libdirs.append(out([qmake, "-query", "QT_INSTALL_LIBS"]))
    plugin_libs = sorted((appdir / "usr/lib/wiv/plugins").glob("*.so"))
    env = {**os.environ,
           "APPIMAGE_EXTRACT_AND_RUN": "1",
           "NO_STRIP": "1",  # linuxdeploy's old strip chokes on modern .relr.dyn sections
           "QMAKE": qmake,
           "LD_LIBRARY_PATH": ":".join(libdirs + [str(build / "plugins")]
                                       + ([os.environ["LD_LIBRARY_PATH"]]
                                          if os.environ.get("LD_LIBRARY_PATH") else [])),
           "LINUXDEPLOY_OUTPUT_VERSION": tag(),
           "PATH": f"{TOOLS_DIR}:{os.environ['PATH']}"}
    cmd = [linuxdeploy, "--appdir", appdir, "--plugin", "qt",
           "-d", desktop, "-i", iconlink, "--output", "appimage"]
    for so in plugin_libs:
        cmd += ["--library", so]
    run(cmd, cwd=build, env=env)

    made = sorted(build.glob("*.AppImage"), key=lambda p: p.stat().st_mtime)[-1]
    target = dist / f"wiv-{tag()}-x86_64.AppImage"
    shutil.move(made, target)
    print(f"Built: {target}")
    print(f"Minimum glibc needed by this AppImage: {glibc_floor(appdir)} "
          f"(runs on distros with glibc >= that)")


def appimage_in_container(dist: Path):
    """Build the AppImage inside an Ubuntu 22.04 container (glibc 2.35 floor)."""
    engine = shutil.which("podman") or shutil.which("docker")
    if not engine:
        sys.exit("The container build needs podman or docker (or pass --native)")
    vcpkg = os.environ.get("VCPKG_ROOT")
    if not vcpkg:
        sys.exit("VCPKG_ROOT is not set")
    image = "wiv-appimage-builder:ubuntu22.04"
    run([engine, "build", "-t", image, "-f", ROOT / "packaging/appimage.Containerfile",
         ROOT / "packaging"])

    # The host vcpkg's binary needs the host's glibc, so give the container its
    # own cheap clone (shares objects with the host one) and bootstrap it there.
    cache = ROOT / "build" / "container-cache"
    for d in ("home", "vcpkg-archives"):
        (cache / d).mkdir(parents=True, exist_ok=True)
    clone = ROOT / "build" / "container-vcpkg"
    if not clone.exists():
        run(["git", "clone", "--shared", "--quiet", vcpkg, clone])
        # the host's baseline commit must be present; it is, via the shared objects

    uid = [] if engine.endswith("docker") else ["--userns=keep-id"]
    if engine.endswith("docker"):
        uid = ["--user", f"{os.getuid()}:{os.getgid()}"]
    script = ("set -e; git config --global --add safe.directory '*'; "
              "[ -x /vcpkg/vcpkg ] || /vcpkg/bootstrap-vcpkg.sh -disableMetrics; "
              "cd /src && tools/build_artifacts.py --native --only appimage --out /dist")
    run([engine, "run", "--rm", "--security-opt", "label=disable", *uid,
         "-v", f"{ROOT}:/src", "-v", f"{dist}:/dist", "-v", f"{clone}:/vcpkg",
         "-v", f"{vcpkg}:{vcpkg}:ro", "-v", f"{cache}:/cache",
         "-e", "VCPKG_ROOT=/vcpkg", "-e", "HOME=/cache/home", "-e", "XDG_CACHE_HOME=/cache",
         "-e", "VCPKG_DEFAULT_BINARY_CACHE=/cache/vcpkg-archives",
         "-e", "WIV_BUILD_NAME=appimage-container", "-e", "WIV_BUNDLE_LIBSTDCXX=1",
         image, "bash", "-c", script])


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--only", help="comma separated subset of: " + ",".join(ALL))
    ap.add_argument("--skip", help="comma separated artifacts to skip")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="output directory (default dist/)")
    ap.add_argument("--native", action="store_true",
                    help="build the AppImage on this machine instead of in the Ubuntu 22.04 "
                         "container; the result then needs this machine's glibc or newer")
    a = ap.parse_args()

    todo = a.only.split(",") if a.only else list(ALL)
    todo = [t for t in todo if t not in (a.skip.split(",") if a.skip else [])]
    bad = [t for t in todo if t not in ALL]
    if bad:
        ap.error("unknown artifact(s): " + ",".join(bad))

    dist = Path(a.out).resolve()
    dist.mkdir(parents=True, exist_ok=True)
    print(f"wiv {tag()} -> {dist}")
    warn_if_dirty()

    failed = []
    for t in todo:
        print(f"\n===== {t} =====", flush=True)
        try:
            if t == "installer":
                build_installer(dist)
            elif t == "source":
                build_source(dist)
            elif t == "appimage":
                if a.native:
                    build_appimage(dist)
                else:
                    appimage_in_container(dist)
        except (subprocess.CalledProcessError, SystemExit) as e:
            print(f"!! {t} failed: {e}", file=sys.stderr)
            failed.append(t)

    print("\n===== summary =====")
    for f in sorted(dist.iterdir()):
        if tag() in f.name:
            print(f"  {f}  ({f.stat().st_size / 1e6:.1f} MB)")
    if failed:
        sys.exit("FAILED: " + ", ".join(failed))


if __name__ == "__main__":
    main()
