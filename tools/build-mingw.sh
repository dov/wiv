#!/bin/bash
# Cross compiles wiv for 64-bit Windows from Linux (Fedora MinGW) and
# optionally builds the NSIS installer.
#
#   tools/build-mingw.sh [--installer]
#
# Prerequisites: `sudo ./install-mingw-deps.sh`, then
# `tools/build-mingw-deps.sh` (once). Output: build/mingw-release/, a
# staged flat install tree in build/mingw-release/stage/ (exe + DLLs +
# shaders/ + plugins/ + platforms/) and, with --installer,
# Installers/Installwiv-v<ver>-<sha>.exe.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build/mingw-release
STAGE=$BUILD/stage
export MINGW_DEPS_PREFIX=${MINGW_DEPS_PREFIX:-/usr/local/mingw64}
SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw
OBJDUMP=x86_64-w64-mingw32-objdump

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$BUILD"

rm -rf "$STAGE"
cmake --install "$BUILD" --prefix "$STAGE"

# Copy every non-system DLL that the exe, plugins and Qt platform plugin
# (transitively) import into the staging dir.
find_dll() {
  local n=$1 d
  for d in "$MINGW_DEPS_PREFIX/bin" "$SYSROOT/bin" "$BUILD" "$BUILD/plugins"; do
    for f in "$d/$n" "$d/${n,,}"; do
      [ -f "$f" ] && { echo "$f"; return 0; }
    done
  done
  return 1
}
declare -A seen
queue=("$STAGE/wiv.exe" "$STAGE"/plugins/*.dll "$STAGE"/platforms/*.dll)
while [ ${#queue[@]} -gt 0 ]; do
  cur=${queue[0]}; queue=("${queue[@]:1}")
  while read -r dll; do
    [ -n "${seen[${dll,,}]:-}" ] && continue
    seen[${dll,,}]=1
    if path=$(find_dll "$dll"); then
      [ -f "$STAGE/$dll" ] || cp "$path" "$STAGE/$dll"
      queue+=("$path")
    fi  # else: a Windows system DLL (kernel32, vulkan-1 ships with drivers...)
  done < <($OBJDUMP -p "$cur" | sed -n 's/^\s*DLL Name: //p')
done

# Qt's other runtime-loaded plugins used by the app (icons/images).
QTPLUGINS=$SYSROOT/lib/qt6/plugins
for sub in styles imageformats iconengines; do
  [ -d "$QTPLUGINS/$sub" ] || continue
  mkdir -p "$STAGE/$sub"
  cp "$QTPLUGINS/$sub"/*.dll "$STAGE/$sub/" 2>/dev/null || true
done

echo "Staged Windows build in $STAGE"

if [ "${1:-}" = "--installer" ]; then
  VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$ROOT/vcpkg.json" | head -1)
  SHORTSHA1=$(git -C "$ROOT" rev-parse --short=6 HEAD)
  cd "$ROOT/Installers"
  rm -rf tmp
  cp -r "$STAGE" tmp
  makensis -V2 -DVERSION="$VERSION" -DSHORTSHA1="$SHORTSHA1" -DSTAGING=tmp wiv.nsi
  echo "Built: $ROOT/Installers/Installwiv-v$VERSION-$SHORTSHA1.exe"
fi
