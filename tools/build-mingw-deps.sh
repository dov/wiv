#!/bin/bash
# Builds the libraries that Fedora doesn't package for MinGW - vsg,
# vsgXchange and spdlog - and installs them into $MINGW_DEPS_PREFIX
# (default /usr/local/mingw64, same prefix giv uses). Run once, after
# `sudo ./install-mingw-deps.sh`.
#
#   tools/build-mingw-deps.sh
#
# Env overrides: MINGW_DEPS_PREFIX, VSG_TAG, VSGXCHANGE_TAG, SPDLOG_TAG,
# MINGW_DEPS_SRC (where sources are cloned/built, default ~/.cache/wiv-mingw).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN=$ROOT/cmake/mingw-w64-x86_64.cmake
export MINGW_DEPS_PREFIX=${MINGW_DEPS_PREFIX:-/usr/local/mingw64}
SRC=${MINGW_DEPS_SRC:-$HOME/.cache/wiv-mingw}
VSG_TAG=${VSG_TAG:-v1.1.16}
VSGXCHANGE_TAG=${VSGXCHANGE_TAG:-v1.1.14}
SPDLOG_TAG=${SPDLOG_TAG:-v1.15.3}
SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw

mkdir -p "$SRC" "$MINGW_DEPS_PREFIX"

fetch() { # name url tag
  if [ -d "$SRC/$1/.git" ] && [ "$(git -C "$SRC/$1" describe --tags --exact-match 2>/dev/null)" != "$3" ]; then
    rm -rf "$SRC/$1"   # different tag requested
  fi
  if [ ! -d "$SRC/$1/.git" ]; then
    git clone --depth 1 --branch "$3" "$2" "$SRC/$1"
  fi
}

build() { # name [cmake args...]
  local name=$1; shift
  cmake -S "$SRC/$name" -B "$SRC/$name/build-mingw" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$MINGW_DEPS_PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "$@"
  cmake --build "$SRC/$name/build-mingw"
  cmake --install "$SRC/$name/build-mingw"
}

VULKAN_ARGS=(
  -DVulkan_INCLUDE_DIR=$SYSROOT/include
  -DVulkan_LIBRARY=$SYSROOT/lib/libvulkan-1.dll.a
)

fetch spdlog https://github.com/gabime/spdlog.git "$SPDLOG_TAG"
build spdlog -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF \
  -DSPDLOG_BUILD_SHARED=OFF

fetch VulkanSceneGraph https://github.com/vsg-dev/VulkanSceneGraph.git "$VSG_TAG"
build VulkanSceneGraph "${VULKAN_ARGS[@]}" -DBUILD_SHARED_LIBS=ON

fetch vsgXchange https://github.com/vsg-dev/vsgXchange.git "$VSGXCHANGE_TAG"
# Keep the optional heavyweight loaders out; wiv only needs freetype/images.
build vsgXchange "${VULKAN_ARGS[@]}" -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_GDAL=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_assimp=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenImageIO=ON

echo "Installed vsg, vsgXchange and spdlog into $MINGW_DEPS_PREFIX"
