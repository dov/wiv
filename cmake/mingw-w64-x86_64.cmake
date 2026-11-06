# Toolchain file for cross compiling wiv to 64-bit Windows with Fedora's
# MinGW packages. Used by tools/build-mingw.sh and tools/build-mingw-deps.sh.
#
# MINGW_DEPS_PREFIX (env or cmake var) is where tools/build-mingw-deps.sh
# installs the libraries Fedora doesn't package (vsg, vsgXchange, spdlog).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER /usr/bin/${TOOLCHAIN_PREFIX}-gcc CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER /usr/bin/${TOOLCHAIN_PREFIX}-g++ CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER /usr/bin/${TOOLCHAIN_PREFIX}-windres CACHE FILEPATH "" FORCE)
# Qt6's toolchain chain-loads Fedora's /usr/share/mingw/toolchain-mingw64.cmake,
# which sets bare compiler names (shadowing the paths above, breaking project()
# in FetchContent subprojects on reconfigure) unless CC/CXX are in the env.
set(ENV{CC} /usr/bin/${TOOLCHAIN_PREFIX}-gcc)
set(ENV{CXX} /usr/bin/${TOOLCHAIN_PREFIX}-g++)

set(MINGW_SYSROOT /usr/${TOOLCHAIN_PREFIX}/sys-root/mingw)
if(NOT MINGW_DEPS_PREFIX)
  if(DEFINED ENV{MINGW_DEPS_PREFIX})
    set(MINGW_DEPS_PREFIX $ENV{MINGW_DEPS_PREFIX})
  else()
    set(MINGW_DEPS_PREFIX /usr/local/mingw64)
  endif()
endif()

set(CMAKE_FIND_ROOT_PATH ${MINGW_DEPS_PREFIX} ${MINGW_SYSROOT})
set(CMAKE_PREFIX_PATH ${MINGW_DEPS_PREFIX} ${MINGW_SYSROOT})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Host tools needed while cross compiling Qt6 apps (moc, rcc, uic).
set(QT_HOST_PATH /usr CACHE PATH "")
set(QT_HOST_PATH_CMAKE_DIR /usr/lib64/cmake CACHE PATH "")

# Static libgcc/libstdc++ so the exe doesn't need extra runtime DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# pkg-config must see the MinGW .pc files, not the host's.
set(ENV{PKG_CONFIG_LIBDIR}
    "${MINGW_SYSROOT}/lib/pkgconfig:${MINGW_SYSROOT}/share/pkgconfig:${MINGW_DEPS_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
set(ENV{PKG_CONFIG_PATH} "")  # host PKG_CONFIG_PATH (e.g. /usr/local) must not leak in
