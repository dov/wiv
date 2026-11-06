# Testing

wiv's tests use GoogleTest + CTest and live in `tests/`. They link `wiv_lib`
(an OBJECT library holding everything but `main()`), i.e. the very objects the
`wiv` executable is built from.

| CTest name       | Label  | What it is |
|------------------|--------|------------|
| `wiv_unit_tests` | `unit` | Pure unit tests, no display: colour parsing, the `.giv` parser, Pango markup, SVG path flattening, colormaps, and a smoke test that loads every `examples/*.giv` / `*.svg`. Runs in well under a second. |
| `wiv_ui_tests`   | `ui`   | In-process GUI tests: a real `MainWindow` + Vulkan viewport is driven through `tests/UITestDriver` (menu actions, synthesized mouse/wheel input, screenshots, off-screen export). |
| `wiv_cli_export` | `ui`   | Black-box: runs the real `wiv --export` and checks it wrote an image. |

## Running

```bash
cmake --preset linux-release          # tests are on by default (WIV_BUILD_TESTS=ON)
cmake --build build/linux-release --target wiv_tests_all -j
ctest --test-dir build/linux-release --output-on-failure

ctest --test-dir build/linux-release -L unit       # fast, no display needed
ctest --test-dir build/linux-release -L ui         # GUI tests only
./build/linux-release/tests/wiv_ui_tests --gtest_filter='UIViewer.Flip*'
```

`WIV_BUILD_TESTS` defaults to OFF when cross-compiling.

### Display

GUI tests need an X display and a Vulkan device (a software one such as
Lavapipe works). When `xvfb-run` is installed CTest runs them inside a private
virtual X server, so no windows appear on your desktop. Configure with
`-DWIV_TEST_XVFB=OFF` to use the real `$DISPLAY` instead (useful together with
running the binary by hand to watch a test).

## Writing GUI tests

* `tests/ui_test_main.cpp` owns the single `QApplication` and points `HOME` /
  `XDG_*` at a scratch directory, so tests never touch your preferences, recent
  files or logs. `UITestDriver` also clears wiv's `QSettings` for each test,
  because some actions (e.g. *Show Marks*) persist.
* `UITestDriver::loadGivText()` writes a scene to the scratch dir and loads it;
  `trigger(id)` fires a menu action (ids from `MainWindow::actionIds()`);
  `screenshot()` grabs the viewport; `waitForColor()` polls until a colour shows
  up (rendering is asynchronous - prefer it over fixed sleeps).
* Input goes to the embedded Vulkan `QWindow` (`renderWindow()`), not to the
  widget. Default mouse settings: plain wheel *navigates*, **Ctrl+wheel**
  zooms, **middle-drag** pans.
* Assert on pixel colours with a tolerance, never golden images - those break
  across GPUs and drivers.
