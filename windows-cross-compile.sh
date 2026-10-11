#!/bin/bash
# Cross compiles wiv for Windows from Fedora (MinGW) and builds the NSIS
# installer. Usage: ./windows-cross-compile.sh [--no-installer]
# Needs `sudo ./install-mingw-deps.sh` once; vsg/vsgXchange/spdlog are built
# automatically on first run (see tools/build-mingw-deps.sh).
set -euo pipefail
cd "$(dirname "$0")"

for t in x86_64-w64-mingw32-g++ makensis cmake ninja glslangValidator; do
  command -v "$t" >/dev/null || {
    echo "Missing '$t' - run: sudo ./install-mingw-deps.sh" >&2
    exit 1
  }
done
[ -d /usr/x86_64-w64-mingw32/sys-root/mingw/lib/cmake/Qt6 ] || {
  echo "MinGW Qt6 not found - run: sudo ./install-mingw-deps.sh" >&2
  exit 1
}

if [ "${1:-}" = "--no-installer" ]; then
  exec tools/build-mingw.sh
else
  exec tools/build-mingw.sh --installer
fi
