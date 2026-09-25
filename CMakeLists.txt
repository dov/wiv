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
find_package(vsgQt CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

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
# Main executable
# ---------------------------------------------------------------------------
add_executable(wiv
  src/main.cpp
  src/GivColor.cpp
  src/GivParser.cpp
  src/SceneBuilder.cpp
  src/PanZoomHandler.cpp
  src/LabelPicker.cpp
  src/BalloonOverlay.cpp
  src/BalloonController.cpp
  src/qt/GivViewer.cpp
  src/qt/VulkanViewport.cpp
  src/qt/PreferencesDialog.cpp
  src/qt/MainWindow.cpp
)

add_dependencies(wiv wiv_shaders)

target_include_directories(wiv PRIVATE src)
target_link_libraries(wiv PRIVATE vsg::vsg vsgXchange::vsgXchange vsgQt::vsgQt Qt6::Core Qt6::Gui Qt6::Widgets)

target_compile_definitions(wiv PRIVATE
  WIV_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
)

if(UNIX AND NOT APPLE)
  target_link_libraries(wiv PRIVATE pthread)
endif()

install(TARGETS wiv RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders DESTINATION share/wiv)
