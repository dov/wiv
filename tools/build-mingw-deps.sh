#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dov Grobgeld
# Builds the libraries that Fedora doesn't package for MinGW - vsg,
# vsgXchange, spdlog and the optional image codecs (LibRaw, libde265/libheif,
# libjxl) - and installs them into $MINGW_DEPS_PREFIX
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
LIBRAW_VERSION=${LIBRAW_VERSION:-0.21.4}
LIBDE265_TAG=${LIBDE265_TAG:-v1.0.15}
LIBHEIF_TAG=${LIBHEIF_TAG:-v1.19.5}
LIBJXL_TAG=${LIBJXL_TAG:-v0.11.1}
AOM_TAG=${AOM_TAG:-v3.11.0}
SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw

mkdir -p "$SRC" "$MINGW_DEPS_PREFIX"

fetch() { # name url tag [--recurse-submodules]
  if [ -d "$SRC/$1/.git" ] && [ "$(git -C "$SRC/$1" describe --tags --exact-match 2>/dev/null)" != "$3" ]; then
    rm -rf "$SRC/$1"   # different tag requested
  fi
  if [ ! -d "$SRC/$1/.git" ]; then
    git clone --depth 1 ${4:+$4 --shallow-submodules} --branch "$3" "$2" "$SRC/$1"
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

# --- Optional image codecs (each has a wiv plugin that is skipped if absent) ---

# LibRaw: autotools release tarball.
if [ ! -f "$MINGW_DEPS_PREFIX/lib/pkgconfig/libraw.pc" ]; then
  if [ ! -d "$SRC/LibRaw-$LIBRAW_VERSION" ]; then
    curl -fL "https://www.libraw.org/data/LibRaw-$LIBRAW_VERSION.tar.gz" | tar -xz -C "$SRC"
  fi
  (cd "$SRC/LibRaw-$LIBRAW_VERSION" &&
   ./configure --host=x86_64-w64-mingw32 --prefix="$MINGW_DEPS_PREFIX" \
     --disable-examples --disable-openmp --disable-lcms --disable-jasper \
     --enable-shared --disable-static &&
   make -j"$(nproc)" && make install)
fi

# HEIC/AVIF: libde265 (HEVC) + libaom (AV1 decoder only) under libheif.
fetch libde265 https://github.com/strukturag/libde265.git "$LIBDE265_TAG"
# --exclude-libs: otherwise the DLL auto-exports libgcc_eh (_Unwind_Resume) and
# clashes with the static libgcc when linking against it.
build libde265 -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc -static-libstdc++ -Wl,--exclude-libs,ALL"
fetch aom https://aomedia.googlesource.com/aom "$AOM_TAG"
build aom -DENABLE_DOCS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF \
  -DENABLE_TOOLS=OFF -DCONFIG_AV1_ENCODER=0 -DAOM_TARGET_CPU=generic \
  -DBUILD_SHARED_LIBS=ON
fetch libheif https://github.com/strukturag/libheif.git "$LIBHEIF_TAG"
build libheif -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DWITH_EXAMPLES=OFF \
  -DENABLE_PLUGIN_LOADING=OFF -DWITH_LIBDE265=ON -DWITH_AOM_DECODER=ON \
  -DWITH_AOM_ENCODER=OFF -DWITH_X265=OFF -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF \
  -DWITH_SvtEnc=OFF -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF \
  -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF

# JPEG XL (bundles highway, brotli, skcms as submodules).
fetch libjxl https://github.com/libjxl/libjxl.git "$LIBJXL_TAG" --recurse-submodules
build libjxl -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF \
  -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_MANPAGES=OFF \
  -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF \
  -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_VIEWERS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF \
  -DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF -DJPEGXL_ENABLE_JPEGLI=OFF \
  -DJPEGXL_FORCE_SYSTEM_BROTLI=OFF -DJPEGXL_FORCE_SYSTEM_HWY=OFF

echo "Installed vsg, vsgXchange, spdlog and image codecs into $MINGW_DEPS_PREFIX"
