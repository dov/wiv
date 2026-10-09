cmake_minimum_required(VERSION 3.21)
project(wiv CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(vsg CONFIG REQUIRED)
find_package(vsgXchange CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
find_package(spdlog CONFIG REQUIRED)

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
  src/SceneBuilder.cpp
  src/CaliperTool.cpp
  src/PanZoomHandler.cpp
  src/LabelPicker.cpp
  src/BalloonController.cpp
  src/ImagePluginHost.cpp
  src/Colormaps.cpp
  src/DisplayImage.cpp
  src/qt/GivViewer.cpp
  src/qt/VulkanViewport.cpp
  src/qt/PreferencesDialog.cpp
  src/qt/CalibrateDialog.cpp
  src/qt/HistogramWidget.cpp
  src/qt/ContrastDialog.cpp
  src/qt/ColorTableDialog.cpp
  src/qt/MainWindow.cpp
  src/qt/MarkTreeView.cpp
  src/qt/OpenFileDialog.cpp
  src/qt/RpcServer.cpp
)

add_dependencies(wiv wiv_shaders)

target_include_directories(wiv PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(wiv PRIVATE vsg::vsg vsgXchange::vsgXchange vsgQt::vsgQt Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Network QtAwesome spdlog::spdlog)

target_compile_definitions(wiv PRIVATE
  WIV_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
  WIV_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins"
)

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
  find_file(WIV_QWINDOWS_PLUGIN qwindows.dll
    PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms
    NO_DEFAULT_PATH)
  add_custom_command(TARGET wiv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:wiv>/platforms
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WIV_QWINDOWS_PLUGIN} $<TARGET_FILE_DIR:wiv>/platforms/
  )
endif()
if(WIN32)
  # Flat layout next to wiv.exe (see AppPaths.h): shaders/, plugins/,
  # platforms/ and every runtime DLL that vcpkg's applocal step put in the
  # build tree. The NSIS installer (Installers/) packages this prefix.
  install(TARGETS wiv RUNTIME DESTINATION .)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders DESTINATION .)
  install(CODE "
    file(GLOB _wiv_dlls \"${CMAKE_BINARY_DIR}/*.dll\")
    file(INSTALL \${_wiv_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
    file(GLOB _wiv_plugin_dlls \"${CMAKE_BINARY_DIR}/plugins/*.dll\")
    file(INSTALL \${_wiv_plugin_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/plugins\")
    file(INSTALL \"${CMAKE_BINARY_DIR}/platforms/qwindows.dll\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/platforms\")
  ")
  # App-local MSVC runtime, so no separate vc_redist install is needed.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  include(InstallRequiredSystemLibraries)
else()
  install(TARGETS wiv RUNTIME DESTINATION bin)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders DESTINATION share/wiv)
endif()

# ---------------------------------------------------------------------------
# Image-format plugins (dlopen-loaded .so files, see src/ImagePluginHost)
# ---------------------------------------------------------------------------
add_subdirectory(src/plugins)
