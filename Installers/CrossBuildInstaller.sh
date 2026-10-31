#!/bin/bash
# Builds the wiv Windows installer on Linux by MinGW cross compilation and
# NSIS (makensis). Linux counterpart of BuildInstaller.bat.
#
#   CrossBuildInstaller.sh [--copy-to DIR]
#
# Steps: cross build -> cmake --install + DLL/plugin staging into
# build/mingw-release/stage (tools/build-mingw.sh) -> copy to Installers/tmp
# -> makensis. --copy-to also copies the installer to DIR.
# Needs `sudo ./install-mingw-deps.sh` once (see windows-cross-compile.sh).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BUILD=$ROOT/build/mingw-release
STAGE=$BUILD/stage

COPY_TO=
while [ $# -gt 0 ]; do
  case $1 in
    --copy-to) COPY_TO=${2:?--copy-to needs a directory}; shift 2 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

for t in x86_64-w64-mingw32-g++ makensis cmake ninja glslangValidator; do
  command -v "$t" >/dev/null || {
    echo "Missing '$t' - run: sudo ./install-mingw-deps.sh" >&2
    exit 1
  }
done

VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$ROOT/vcpkg.json" | head -1)
SHORTSHA1=$(git -C "$ROOT" rev-parse --short=6 HEAD)
echo "wiv $VERSION ($SHORTSHA1) from $BUILD"

echo "=== Building and staging ==="
"$ROOT/tools/build-mingw.sh"

echo "=== Staging ==="
cd "$HERE"
rm -rf tmp
cp -r "$STAGE" tmp

echo "=== makensis ==="
makensis -V2 -DVERSION="$VERSION" -DSHORTSHA1="$SHORTSHA1" -DSTAGING=tmp wiv.nsi

# The .nsi decides the exact file name (and its case).
INSTALLER=$(ls -t "$HERE"/[Ii]nstall[Vv]giv-v"$VERSION"-"$SHORTSHA1".exe | head -1)
echo
echo "Built: $INSTALLER"

if [ -n "$COPY_TO" ]; then
  mkdir -p "$COPY_TO"
  cp -f "$INSTALLER" "$COPY_TO/"
  echo "Copied to $COPY_TO"
fi
