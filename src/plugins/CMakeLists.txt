# Image-format plugins for wiv, loaded at runtime via dlopen (see
# ImagePluginHost) - never linked into the `wiv` executable itself.
# Each plugin is built as a bare-named shared library (no "lib" prefix)
# so ImagePluginHost's sorted directory scan sees plain format names.

function(wiv_add_plugin name)
  add_library(wiv_plugin_${name} SHARED ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/wiv_plugin_common.cpp)
  set_target_properties(wiv_plugin_${name} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
  )
  target_include_directories(wiv_plugin_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  install(TARGETS wiv_plugin_${name} LIBRARY DESTINATION lib/wiv/plugins)
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

find_package(TIFF REQUIRED)
wiv_add_plugin(tiff tiff/tiff_plugin.cpp)
target_link_libraries(wiv_plugin_tiff PRIVATE TIFF::TIFF)

find_package(WebP CONFIG REQUIRED)
wiv_add_plugin(webp webp/webp_plugin.cpp)
target_link_libraries(wiv_plugin_webp PRIVATE WebP::webp)

wiv_add_plugin(npy npy/npy_plugin.cpp)

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
target_link_libraries(wiv_plugin_fits PRIVATE $<IF:$<TARGET_EXISTS:CFITSIO::cfitsio>,CFITSIO::cfitsio,PkgConfig::CFITSIO>)
