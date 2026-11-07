# Image-format plugins for wiv, loaded at runtime via dlopen (see
# ImagePluginHost) - never linked into the `wiv` executable itself.
# Each plugin is built as a bare-named shared library (no "lib" prefix)
# so ImagePluginHost's sorted directory scan sees plain format names.

function(wiv_add_plugin name)
  add_library(wiv_plugin_${name} SHARED ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/wiv_plugin_common.cpp)
  set_target_properties(wiv_plugin_${name} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "$<IF:$<BOOL:${WIN32}>,wiv_${name},${name}>"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
  )
  target_include_directories(wiv_plugin_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(WIN32)
    install(TARGETS wiv_plugin_${name} RUNTIME DESTINATION lib/wiv/plugins)
  else()
    install(TARGETS wiv_plugin_${name} LIBRARY DESTINATION lib/wiv/plugins)
  endif()
endfunction()

wiv_add_plugin(stbimage stbimage/stbimage_plugin.cpp)
target_include_directories(wiv_plugin_stbimage PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

# Takes over JPEG decoding from stbimage (see jpegturbo_plugin.cpp's doc
# comment for why: SIMD-accelerated decode matters for $image cycling's
# decode-on-demand path) - "jpegturbo" sorts before "stbimage" in
# ImagePluginHost's first-match-wins dispatch, so this one always wins.
find_package(JPEG REQUIRED)
wiv_add_plugin(jpegturbo jpegturbo/jpegturbo_plugin.cpp)
target_link_libraries(wiv_plugin_jpegturbo PRIVATE JPEG::JPEG)

wiv_add_plugin(pgm pgm/pgm_plugin.cpp)

# Radiance HDR and PFM (own parsers, no external deps).
wiv_add_plugin(hdr hdr/hdr_plugin.cpp)

find_package(TIFF REQUIRED)
wiv_add_plugin(tiff tiff/tiff_plugin.cpp)
target_link_libraries(wiv_plugin_tiff PRIVATE TIFF::TIFF)

find_package(WebP CONFIG REQUIRED)
wiv_add_plugin(webp webp/webp_plugin.cpp)
target_link_libraries(wiv_plugin_webp PRIVATE WebP::webp WebP::webpdemux)

wiv_add_plugin(npy npy/npy_plugin.cpp)

# NIfTI (.nii/.nii.gz) and OpenRaster/Krita (.ora/.kra): small self-contained
# readers that only need zlib (already pulled in by libtiff/freetype).
find_package(ZLIB REQUIRED)
wiv_add_plugin(nifti nifti/nifti_plugin.cpp)
target_link_libraries(wiv_plugin_nifti PRIVATE ZLIB::ZLIB)
wiv_add_plugin(ora ora/ora_plugin.cpp)
target_include_directories(wiv_plugin_ora PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(wiv_plugin_ora PRIVATE ZLIB::ZLIB)

wiv_add_plugin(dicom dicom/dicom_plugin.cpp)

# cfitsio ships a CMake config (CFITSIO::cfitsio) via vcpkg, but Fedora's
# cfitsio-devel package only provides a pkg-config file - fall back to that
# when the CMake config isn't found (e.g. building against the system lib).
find_package(cfitsio CONFIG QUIET)
if(NOT cfitsio_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CFITSIO REQUIRED IMPORTED_TARGET cfitsio)
endif()
wiv_add_plugin(fits fits/fits_plugin.cpp)
# Fedora's mingw cfitsio puts fitsio.h in include/cfitsio/, which its CMake
# config doesn't export.
find_path(WIV_CFITSIO_INCLUDE fitsio.h PATH_SUFFIXES cfitsio)
if(WIV_CFITSIO_INCLUDE)
  target_include_directories(wiv_plugin_fits PRIVATE ${WIV_CFITSIO_INCLUDE})
endif()
target_link_libraries(wiv_plugin_fits PRIVATE $<IF:$<TARGET_EXISTS:CFITSIO::cfitsio>,CFITSIO::cfitsio,PkgConfig::CFITSIO>)

# DDS textures (block decoding by the vendored bcdec) and Windows icons.
wiv_add_plugin(dds dds/dds_plugin.cpp)
target_include_directories(wiv_plugin_dds PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

wiv_add_plugin(ico ico/ico_plugin.cpp)
target_include_directories(wiv_plugin_ico PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

# HEIC/HEIF/AVIF (libheif) and camera RAW (LibRaw) are optional: Fedora's
# MinGW repos don't package libheif, and a build without the -devel packages
# should still work, just without those formats.
find_package(PkgConfig QUIET)
find_package(libheif CONFIG QUIET)
if(TARGET heif)
  set(WIV_HEIF_TARGET heif)
elseif(PkgConfig_FOUND)
  pkg_check_modules(LIBHEIF IMPORTED_TARGET libheif)
  if(LIBHEIF_FOUND)
    set(WIV_HEIF_TARGET PkgConfig::LIBHEIF)
  endif()
endif()
if(WIV_HEIF_TARGET)
  wiv_add_plugin(heif heif/heif_plugin.cpp)
  target_link_libraries(wiv_plugin_heif PRIVATE ${WIV_HEIF_TARGET})
else()
  message(STATUS "libheif not found - building without the HEIC/HEIF/AVIF plugin")
endif()

# JPEG XL (libjxl) is optional too.
if(PkgConfig_FOUND)
  pkg_check_modules(LIBJXL IMPORTED_TARGET libjxl libjxl_threads)
endif()
if(LIBJXL_FOUND)
  wiv_add_plugin(jxl jxl/jxl_plugin.cpp)
  target_link_libraries(wiv_plugin_jxl PRIVATE PkgConfig::LIBJXL)
else()
  message(STATUS "libjxl not found - building without the JPEG XL plugin")
endif()

if(PkgConfig_FOUND)
  pkg_check_modules(LIBRAW IMPORTED_TARGET libraw)
endif()
if(LIBRAW_FOUND)
  wiv_add_plugin(raw raw/raw_plugin.cpp)
  target_link_libraries(wiv_plugin_raw PRIVATE PkgConfig::LIBRAW JPEG::JPEG)
else()
  message(STATUS "LibRaw not found - building without the camera RAW plugin")
endif()

# OpenEXR is optional too.
find_package(OpenEXR CONFIG QUIET)
if(TARGET OpenEXR::OpenEXR)
  wiv_add_plugin(exr exr/exr_plugin.cpp)
  target_link_libraries(wiv_plugin_exr PRIVATE OpenEXR::OpenEXR)
else()
  message(STATUS "OpenEXR not found - building without the OpenEXR plugin")
endif()

# HDF5 (2D arrays as grayscale, 3D as colour or slices) is optional too.
find_package(HDF5 COMPONENTS C QUIET)
# When cross compiling, FindHDF5 can latch onto the host's h5cc wrapper and
# report "found" with no usable target library; only accept a real config.
if(HDF5_FOUND AND (NOT CMAKE_CROSSCOMPILING OR HDF5_DIR))
  wiv_add_plugin(hdf5 hdf5/hdf5_plugin.cpp)
  if(TARGET hdf5::hdf5)
    target_link_libraries(wiv_plugin_hdf5 PRIVATE hdf5::hdf5)
  else()
    target_include_directories(wiv_plugin_hdf5 PRIVATE ${HDF5_INCLUDE_DIRS})
    target_link_libraries(wiv_plugin_hdf5 PRIVATE ${HDF5_LIBRARIES})
  endif()
else()
  message(STATUS "HDF5 not found - building without the HDF5 plugin")
endif()

# netCDF (2D-4D variables as grayscale pages/slices) is optional too. Fedora's
# netcdf-devel ships a CMake config (netCDF::netcdf) and a pkg-config file.
find_package(netCDF CONFIG QUIET)
if(NOT TARGET netCDF::netcdf AND PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
  pkg_check_modules(NETCDF IMPORTED_TARGET netcdf)
endif()
if(TARGET netCDF::netcdf)
  wiv_add_plugin(netcdf netcdf/netcdf_plugin.cpp)
  target_link_libraries(wiv_plugin_netcdf PRIVATE netCDF::netcdf)
elseif(TARGET PkgConfig::NETCDF)
  wiv_add_plugin(netcdf netcdf/netcdf_plugin.cpp)
  target_link_libraries(wiv_plugin_netcdf PRIVATE PkgConfig::NETCDF)
else()
  message(STATUS "netCDF not found - building without the netCDF plugin")
endif()
