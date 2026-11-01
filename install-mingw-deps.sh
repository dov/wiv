#!/bin/sh
# Installs the Fedora packages needed to cross compile wiv for Windows
# (tools/build-mingw.sh). Run with: sudo ./install-mingw-deps.sh
set -e

dnf install -y \
    cmake ninja-build git glslang file binutils \
    qt6-qtbase-devel qt6-qtbase-private-devel \
    mingw64-filesystem \
    mingw64-gcc mingw64-gcc-c++ mingw64-binutils mingw64-crt \
    mingw64-winpthreads mingw64-headers \
    mingw64-qt6-qtbase mingw64-qt6-qtsvg mingw64-qt6-qttools \
    mingw64-vulkan-headers mingw64-vulkan-loader \
    mingw64-freetype mingw64-libjpeg-turbo mingw64-libtiff \
    mingw64-libwebp mingw64-libpng mingw64-zlib mingw64-bzip2 \
    mingw64-cfitsio mingw64-LibRaw \
    mingw32-nsis
