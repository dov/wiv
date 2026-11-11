# Build environment for the wiv AppImage: Ubuntu 22.04 (glibc 2.35) is the
# oldest base that official Qt 6.8 binaries support, so the AppImage runs on
# any distro with glibc >= 2.35.  Used by build_artifacts.py.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git curl ca-certificates zip unzip tar pkg-config file patchelf \
    python3 python3-pip autoconf automake libtool bison flex perl binutils \
    desktop-file-utils libfuse2 \
    libvulkan-dev libx11-dev libx11-xcb-dev libxrandr-dev libxinerama-dev libxcursor-dev \
    libxi-dev libxext-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb1-dev \
    libxcb-keysyms1-dev libxcb-xkb-dev libxcb-cursor-dev libxcb-icccm4-dev \
    libxcb-image0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-xinerama0-dev \
    libxcb-randr0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-shm0-dev \
    libgl1-mesa-dev libegl1-mesa-dev libdbus-1-dev libfontconfig-dev libfreetype-dev \
    libfribidi-dev libharfbuzz-dev libsqlite3-dev libexpat1-dev \
    libheif-dev libraw-dev libopenexr-dev libhdf5-dev libnetcdf-dev \
 && rm -rf /var/lib/apt/lists/*
# Recent cmake/ninja (vsgQt wants cmake >= 3.28) and Qt 6.8 from the Qt installer mirrors.
RUN pip3 install --no-cache-dir cmake ninja aqtinstall
# download.qt.io is flaky and aqt's fallback mirrors are sometimes broken: retry.
RUN for i in 1 2 3 4 5 6; do \
      aqt install-qt linux desktop 6.8.3 linux_gcc_64 --outputdir /opt/Qt --archives qtbase qtsvg qtwayland icu && exit 0; \
      echo "aqt attempt $i failed, retrying"; sleep 10; \
    done; exit 1
# Extra tools vcpkg's autotools-based ports (xcb/x11 stack) ask for.
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf-archive libltdl-dev gperf xutils-dev nasm python3-jinja2 python3-mako \
 && rm -rf /var/lib/apt/lists/*
# The code needs C++20 <format> (GCC 13) and HarfBuzz >= 4 (hb_draw_funcs); 22.04 ships
# GCC 11 / HarfBuzz 2.7.  GCC 13's libstdc++ is bundled into the AppImage by build_artifacts.py.
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common gpg-agent dirmngr \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update && apt-get install -y --no-install-recommends gcc-13 g++-13 \
 && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
 && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-13 100 \
 && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100 \
 && rm -rf /var/lib/apt/lists/*
RUN pip3 install --no-cache-dir meson \
 && curl -fsSL https://github.com/harfbuzz/harfbuzz/releases/download/8.5.0/harfbuzz-8.5.0.tar.xz | tar xJ -C /tmp \
 && meson setup /tmp/hb-build /tmp/harfbuzz-8.5.0 --prefix=/usr/local --libdir=lib -Dbuildtype=release \
      -Dtests=disabled -Ddocs=disabled -Dbenchmark=disabled -Dglib=disabled -Dgobject=disabled -Dicu=disabled \
 && ninja -C /tmp/hb-build install && rm -rf /tmp/hb-build /tmp/harfbuzz-8.5.0 && ldconfig
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV CMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64 \
    PATH=/opt/Qt/6.8.3/gcc_64/bin:$PATH
# HDF5 1.14 and netCDF-C 4.9 from source: the netCDF plugin's direct multi-core chunk reader needs
# H5Dchunk_iter (HDF5 >= 1.14), and 22.04 ships 1.10.  They go into /usr/local, which HDF5_ROOT and
# CMAKE_PREFIX_PATH (below) put ahead of the apt libhdf5-dev/libnetcdf-dev above.  These are the last
# layers so that changing them does not invalidate the Qt/GCC/HarfBuzz layers.  HDF5's high-level
# library is what netCDF-4 links; the rest of the optional features are off to keep the bundle small.
RUN apt-get update && apt-get install -y --no-install-recommends zlib1g-dev \
 && rm -rf /var/lib/apt/lists/* \
 && curl -fsSL https://github.com/HDFGroup/hdf5/releases/download/hdf5_1.14.6/hdf5-1.14.6.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/hdf5-1.14.6 -B /tmp/hdf5-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib \
      -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_TESTING=OFF \
      -DHDF5_BUILD_EXAMPLES=OFF -DHDF5_BUILD_TOOLS=OFF -DHDF5_BUILD_HL_LIB=ON -DHDF5_BUILD_CPP_LIB=OFF \
      -DHDF5_BUILD_FORTRAN=OFF -DHDF5_BUILD_JAVA=OFF -DHDF5_ENABLE_Z_LIB_SUPPORT=ON \
      -DHDF5_ENABLE_SZIP_SUPPORT=OFF -DHDF5_ENABLE_PARALLEL=OFF \
 && cmake --build /tmp/hdf5-build && cmake --install /tmp/hdf5-build \
 && rm -rf /tmp/hdf5-build /tmp/hdf5-1.14.6 && ldconfig
RUN curl -fsSL https://github.com/Unidata/netcdf-c/archive/refs/tags/v4.9.3.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/netcdf-c-4.9.3 -B /tmp/nc-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_PREFIX_PATH=/usr/local \
      -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
      -DNETCDF_ENABLE_NETCDF_4=ON -DENABLE_NETCDF_4=ON \
      -DNETCDF_BUILD_UTILITIES=OFF -DBUILD_UTILITIES=OFF \
      -DNETCDF_ENABLE_TESTS=OFF -DENABLE_TESTS=OFF \
      -DNETCDF_ENABLE_DAP=OFF -DENABLE_DAP=OFF \
      -DNETCDF_ENABLE_BYTERANGE=OFF -DENABLE_BYTERANGE=OFF \
      -DNETCDF_ENABLE_NCZARR=OFF -DENABLE_NCZARR=OFF \
      -DNETCDF_ENABLE_PLUGINS=OFF -DENABLE_PLUGINS=OFF \
      -DNETCDF_ENABLE_HDF4=OFF -DENABLE_HDF4=OFF \
 && cmake --build /tmp/nc-build && cmake --install /tmp/nc-build \
 && rm -rf /tmp/nc-build /tmp/netcdf-c-4.9.3 && ldconfig
ENV HDF5_ROOT=/usr/local \
    CMAKE_PREFIX_PATH=/usr/local:/opt/Qt/6.8.3/gcc_64
