# Test suite: GoogleTest + CTest (see ../Testing.md).
#
#   wiv_unit_tests  - pure unit tests (parser, colors, markup, SVG paths, ...)
#   wiv_ui_tests    - in-process GUI tests; label "ui", need a display + Vulkan
#
# Both link wiv_lib, the same object files the wiv executable is made of.

add_executable(wiv_unit_tests
  test_color.cpp
  test_parser.cpp
  test_markup_svgpath_colormaps.cpp
  test_examples.cpp
)
target_link_libraries(wiv_unit_tests PRIVATE wiv_lib GTest::gtest GTest::gtest_main)
target_compile_definitions(wiv_unit_tests PRIVATE WIV_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
add_test(NAME wiv_unit_tests COMMAND wiv_unit_tests)
set_tests_properties(wiv_unit_tests PROPERTIES LABELS "unit")

add_executable(wiv_ui_tests
  ui_test_main.cpp
  UITestDriver.cpp
  test_ui_viewer.cpp
  test_ui_export.cpp
)
target_link_libraries(wiv_ui_tests PRIVATE wiv_lib Qt6::Test GTest::gtest)
target_include_directories(wiv_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(wiv_ui_tests PRIVATE WIV_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
# Run the GUI binaries under a private X server (xvfb-run) when available, so
# they neither need nor disturb the desktop session; set WIV_TEST_XVFB=OFF to
# use the real $DISPLAY instead.
option(WIV_TEST_XVFB "Run GUI tests under xvfb-run when it is installed" ON)
find_program(WIV_XVFB_RUN xvfb-run)
set(WIV_GUI_RUNNER "")
if(WIV_TEST_XVFB AND WIV_XVFB_RUN AND NOT WIN32)
  set(WIV_GUI_RUNNER ${WIV_XVFB_RUN} -a -s "-screen 0 1280x1024x24")
endif()

# Plain add_test (no discovery): the GUI binary is never executed at build time.
add_test(NAME wiv_ui_tests COMMAND ${WIV_GUI_RUNNER} $<TARGET_FILE:wiv_ui_tests>)
set_tests_properties(wiv_ui_tests PROPERTIES LABELS "ui" RESOURCE_LOCK gui TIMEOUT 300)

add_test(NAME wiv_cli_export
  COMMAND ${WIV_GUI_RUNNER} ${CMAKE_COMMAND} -DWIV=$<TARGET_FILE:wiv>
          -DINPUT=${CMAKE_SOURCE_DIR}/examples/example.giv -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cli_export.png
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_export.cmake)
set_tests_properties(wiv_cli_export PROPERTIES LABELS "ui" RESOURCE_LOCK gui TIMEOUT 120
  ENVIRONMENT "HOME=${CMAKE_CURRENT_BINARY_DIR}/home;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/home/data")

add_custom_target(wiv_tests_all DEPENDS wiv_unit_tests wiv_ui_tests wiv)
