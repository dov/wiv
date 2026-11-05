cmake_minimum_required(VERSION 3.21)
project(wiv CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MINGW AND NOT EXISTS "${MINGW_DEPS_PREFIX}/lib/cmake/vsg/vsgConfig.cmake"
   AND NOT EXISTS "${MINGW_DEPS_PREFIX}/lib64/cmake/vsg/vsgConfig.cmake")
  # Cross build: vsg/vsgXchange/spdlog aren't packaged for MinGW; build them once.
  message(STATUS "Building MinGW dependencies (tools/build-mingw-deps.sh)...")
  execute_process(COMMAND ${CMAKE_COMMAND} -E env MINGW_DEPS_PREFIX=${MINGW_DEPS_PREFIX}
                          ${CMAKE_SOURCE_DIR}/tools/build-mingw-deps.sh
                  RESULT_VARIABLE _deps_rc)
  if(NOT _deps_rc EQUAL 0)
    message(FATAL_ERROR "tools/build-mingw-deps.sh failed")
  endif()
endif()
find_package(vsg CONFIG REQUIRED)
find_package(vsgXchange CONFIG REQUIRED)
# (before Qt6, whose cross config rewrites the find root path)
find_package(spdlog CONFIG REQUIRED)
# System sqlite3 on Linux, the vcpkg port on Windows.
find_package(SQLite3 REQUIRED)
find_package(EXPAT REQUIRED)
# Text layout for SVG <text>: HarfBuzz (shaping, glyph outlines), FriBidi (bidi),
# fontconfig (font lookup and fallback; Linux only - Windows uses its Fonts dir).
find_package(PkgConfig REQUIRED)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
if(NOT WIN32)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
if(MINGW AND MINGW_DEPS_PREFIX)
  # Qt6's cross config replaced CMAKE_FIND_ROOT_PATH; put our prefix back so the
  # plugins' pkg-config lookups (libheif, libjxl, LibRaw) can find their libs.
  list(APPEND CMAKE_FIND_ROOT_PATH ${MINGW_DEPS_PREFIX})
endif()

# The system-wide vsgQt install (/usr/local) was built against Qt5, so it
# can't be used by this (Qt6) app - vsgQt is a thin library, so build it
# from source against our own Qt6 + the system vsg instead of vendoring a
# Qt6 build of all of Qt itself via vcpkg.
include(FetchContent)
set(QT_PACKAGE_NAME Qt6 CACHE STRING "" FORCE)
# EXCLUDE_FROM_ALL keeps vsgQt's example apps and install rules out of the
# build/install (wiv still builds the vsgQt library it links). Needs 3.28+.
set(WIV_VSGQT_EXCLUDE "")
if(NOT CMAKE_VERSION VERSION_LESS 3.28)
  set(WIV_VSGQT_EXCLUDE EXCLUDE_FROM_ALL)
endif()
FetchContent_Declare(vsgQt
  GIT_REPOSITORY https://github.com/vsg-dev/vsgQt.git
  GIT_TAG v0.4.0
  GIT_SHALLOW TRUE
  ${WIV_VSGQT_EXCLUDE}
)
FetchContent_MakeAvailable(vsgQt)

# ---------------------------------------------------------------------------
# Shader compilation (GLSL -> SPIR-V) via glslangValidator
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
if(NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "glslangValidator not found - required to compile shaders/*.vert/*.frag to SPIR-V")
endif()

set(WIV_SHADER_SOURCES
  ${CMAKE_SOURCE_DIR}/shaders/marks.vert
  ${CMAKE_SOURCE_DIR}/shaders/marks.frag
  ${CMAKE_SOURCE_DIR}/shaders/marks_label.frag
  ${CMAKE_SOURCE_DIR}/shaders/lines.vert
  ${CMAKE_SOURCE_DIR}/shaders/lines.frag
  ${CMAKE_SOURCE_DIR}/shaders/fill.vert
  ${CMAKE_SOURCE_DIR}/shaders/fill.frag
  ${CMAKE_SOURCE_DIR}/shaders/image.vert
  ${CMAKE_SOURCE_DIR}/shaders/image.frag
  ${CMAKE_SOURCE_DIR}/shaders/image_samples.frag
  ${CMAKE_SOURCE_DIR}/shaders/grid.vert
  ${CMAKE_SOURCE_DIR}/shaders/grid.frag
)

set(WIV_SPIRV_BINARIES)
foreach(shader ${WIV_SHADER_SOURCES})
  get_filename_component(shader_name ${shader} NAME)
  set(spirv "${CMAKE_BINARY_DIR}/shaders/${shader_name}.spv")
  add_custom_command(
    OUTPUT ${spirv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLANG_VALIDATOR} -V ${shader} -o ${spirv}
    DEPENDS ${shader}
    COMMENT "Compiling SPIR-V shader ${shader_name}"
  )
  list(APPEND WIV_SPIRV_BINARIES ${spirv})
endforeach()

add_custom_target(wiv_shaders DEPENDS ${WIV_SPIRV_BINARIES})

# ---------------------------------------------------------------------------
# Third party
# ---------------------------------------------------------------------------
# EXCLUDE_FROM_ALL: QtAwesome installs its own library and CMake export set,
# which has no business landing in wiv's install prefix. It still builds,
# since wiv links it (used to theme the Open dialog's icons).
add_subdirectory(third_party/QtAwesome EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(wiv
  src/main.cpp
  src/Logging.cpp
  src/GivColor.cpp
  src/GivParser.cpp
  src/GivThumbnailRenderer.cpp
  src/SvgLoader.cpp
  src/SvgDom.cpp
  src/SvgAnimation.cpp
  src/SvgPath.cpp
  src/SvgCss.cpp
  src/TextLayout.cpp
  src/TextMarkup.cpp
  src/SceneBuilder.cpp
  src/CaliperTool.cpp
  src/PanZoomHandler.cpp
  src/LabelPicker.cpp
  src/OffscreenTarget.cpp
  src/BalloonController.cpp
  src/ImagePluginHost.cpp
  src/Colormaps.cpp
  src/ExifReader.cpp
  src/ImageStateStore.cpp
  src/DisplayImage.cpp
  src/qt/GivViewer.cpp
  src/qt/VulkanViewport.cpp
  src/qt/PreferencesDialog.cpp
  src/qt/ExternalApps.cpp
  src/qt/InstalledApps.cpp
  src/qt/CalibratePanel.cpp
  src/qt/ColorMapBar.cpp
  src/qt/HistogramWidget.cpp
  src/qt/ContrastPanel.cpp
  src/qt/InfoPanel.cpp
  src/qt/ColorTablePanel.cpp
  src/qt/GlyphSizeDialog.cpp
  src/qt/MainWindow.cpp
  src/qt/ToggleCapture.cpp
  src/qt/PrefetchPool.cpp
  src/qt/MarkTreeView.cpp
  src/qt/OpenFileDialog.cpp
  src/qt/RpcServer.cpp
  src/qt/ThumbnailStrip.cpp
  src/qt/RemoteClient.cpp
  resources/wiv.qrc
)
if(WIN32)
  # wiv.rc embeds the app icon (id 1) and the .giv file icon (id 2).
  target_sources(wiv PRIVATE src/qt/WinDropTarget.cpp resources/wiv.rc resources/wiv.manifest)
  # rc.exe doesn't report include dependencies; without this, editing an
  # icon doesn't rebuild the resource and the exe keeps the stale icon.
  set_source_files_properties(resources/wiv.rc PROPERTIES
    OBJECT_DEPENDS
      "${CMAKE_SOURCE_DIR}/resources/wiv-logo.ico;${CMAKE_SOURCE_DIR}/resources/file-giv-logo.ico")
  target_link_libraries(wiv PRIVATE ole32 shell32 uuid)
endif()

add_dependencies(wiv wiv_shaders)

target_include_directories(wiv PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(wiv PRIVATE vsg::vsg vsgXchange::vsgXchange vsgQt::vsgQt Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Network QtAwesome spdlog::spdlog SQLite::SQLite3 EXPAT::EXPAT PkgConfig::HARFBUZZ PkgConfig::FRIBIDI)
if(NOT WIN32)
  target_link_libraries(wiv PRIVATE PkgConfig::FONTCONFIG)
endif()

# X11 only: the render window's bit gravity (see VulkanViewport::keepContentsInPlaceOnResize()).
if(UNIX AND NOT APPLE)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb)
    if(XCB_FOUND)
      target_link_libraries(wiv PRIVATE PkgConfig::XCB)
      target_compile_definitions(wiv PRIVATE WIV_HAVE_XCB)
    endif()
  endif()
endif()

# Version / commit info shown in Help > About. Generated at BUILD time by an
# always-out-of-date target (the header is rewritten only when it changes), so
# a new commit rebuilds just BuildInfo.cpp - no reconfigure needed.
file(READ ${CMAKE_SOURCE_DIR}/vcpkg.json _vcpkg_json)
string(JSON WIV_VERSION GET "${_vcpkg_json}" version)
set(WIV_BUILDINFO_HEADER ${CMAKE_BINARY_DIR}/generated/BuildInfo_gen.h)
add_custom_target(wiv_buildinfo
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DHEADER_PATH=${WIV_BUILDINFO_HEADER}
    -DVERSION=${WIV_VERSION}
    -P ${CMAKE_SOURCE_DIR}/cmake/GenBuildInfo.cmake
  BYPRODUCTS ${WIV_BUILDINFO_HEADER}
  COMMENT "Updating build info header"
  VERBATIM)
set_source_files_properties(src/BuildInfo.cpp PROPERTIES OBJECT_DEPENDS ${WIV_BUILDINFO_HEADER})
target_sources(wiv PRIVATE src/BuildInfo.cpp)
target_include_directories(wiv PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(wiv wiv_buildinfo)

target_compile_definitions(wiv PRIVATE
  WIV_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
  WIV_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
)

if(MSVC)
  # M_PI etc. from <cmath>
  target_compile_definitions(wiv PRIVATE _USE_MATH_DEFINES)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(wiv PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

if(WIN32)
  # GUI subsystem: no console window. Keep the plain main() entry point
  # instead of requiring WinMain.
  set_target_properties(wiv PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(wiv PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_link_options(wiv PRIVATE -Wl,-subsystem,windows -Wl,-e,mainCRTStartup)
  endif()
endif()


if(WIN32)
  # Qt needs its platform plugin next to the exe (vcpkg's applocal step only
  # copies DLLs linked at build time, not runtime-loaded plugins).
  if(MINGW)
    # Cross build with Fedora's mingw64-qt6-qtbase: <sysroot>/lib/qt6/plugins.
    find_file(WIV_QWINDOWS_PLUGIN qwindows.dll
      PATHS ${CMAKE_FIND_ROOT_PATH} ${CMAKE_PREFIX_PATH}
      PATH_SUFFIXES lib/qt6/plugins/platforms plugins/platforms
      NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  else()
    find_file(WIV_QWINDOWS_PLUGIN qwindows.dll
      PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms
      NO_DEFAULT_PATH)
  endif()
  add_custom_command(TARGET wiv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:wiv>/platforms
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WIV_QWINDOWS_PLUGIN} $<TARGET_FILE_DIR:wiv>/platforms/
  )
endif()
if(WIN32)
  # Installed layout (see AppPaths.h): bin/ holds wiv.exe, every runtime DLL
  # that vcpkg's applocal step put in the build tree and Qt's platforms/
  # plugin dir; shaders in share/wiv/, image plugins in lib/wiv/plugins/
  # (installed by src/plugins), examples/ at the top. The NSIS installer
  # (Installers/) packages this prefix.
  install(TARGETS wiv RUNTIME DESTINATION bin)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders DESTINATION share/wiv)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/examples/ DESTINATION examples)
  install(CODE "
    file(GLOB _wiv_dlls \"${CMAKE_BINARY_DIR}/*.dll\")
    file(INSTALL \${_wiv_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")
    # Third-party DLLs the image plugins depend on (cfitsio, tiff, ...) land
    # in plugins/ next to the plugins themselves; install them into bin/,
    # where ImagePluginHost adds the exe directory to the DLL search path.
    file(GLOB _wiv_plugin_deps \"${CMAKE_BINARY_DIR}/plugins/*.dll\")
    list(FILTER _wiv_plugin_deps EXCLUDE REGEX \"/wiv_[^/]*\$\")
    file(INSTALL \${_wiv_plugin_deps} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")
    file(INSTALL \"${CMAKE_BINARY_DIR}/platforms/qwindows.dll\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin/platforms\")
  ")
  # App-local MSVC runtime, so no separate vc_redist install is needed.
  # (MinGW cross builds: tools/build-mingw.sh copies the DLLs instead.)
  if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
  endif()
else()
  install(TARGETS wiv RUNTIME DESTINATION bin)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders DESTINATION share/wiv)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/examples/ DESTINATION share/wiv/examples)
endif()

# ---------------------------------------------------------------------------
# Image-format plugins (dlopen-loaded .so files, see src/ImagePluginHost)
# ---------------------------------------------------------------------------
add_subdirectory(src/plugins)
