# wiv feature list

A reference for feature-comparing wiv against other image viewers without
rescanning the repo. Compiled from the source tree (menus, plugins, parser,
RPC server) as of 2026-11-05; update it when features change.

Key bindings below are defaults; most View/File/Tools actions are rebindable
in Edit > Preferences > Keyboard.

## Overview

- Vulkan-rendered (VulkanSceneGraph) viewer with a Qt 6 UI.
- Per-image settings persisted in a SQLite database (see "Remembered state").
- Dual purpose: a raster image viewer **and** a viewer for the giv vector
  format (`.giv`), compatible with the original giv
  (<https://github.com/dov/giv>).
- Platforms: Linux (native), Windows (MSVC/vcpkg build, MinGW cross-build,
  NSIS installer).
- Image formats are loaded by runtime-loaded plugins (`dlopen`/`LoadLibrary`),
  found via `WIV_PLUGIN_DIR` or `<prefix>/lib/wiv/plugins`.

## File formats

| Format | Extensions | Plugin / source | Notes |
| --- | --- | --- | --- |
| PNG, JPEG, BMP, TGA, GIF, PSD | `.png .jpg .jpeg .bmp .tga .gif .psd` | `stbimage` (vendored stb_image) | 16-bit PNG keeps raw samples; animated GIF/APNG |
| JPEG (fast path) | `.jpg .jpeg` | `jpegturbo` | libjpeg-turbo; reduced-size (1/2,1/4,1/8) fast previews |
| TIFF | `.tif .tiff` | `tiff` (libtiff) | multi-page; orientation-aware; raw sample data |
| WebP | `.webp` | `webp` | animated WebP |
| HEIC/HEIF/AVIF | `.heic .heif .hif .avif .avifs` | `heif` (libheif, optional) | AVIF sequences need libheif 1.20+ |
| JPEG XL | `.jxl` | `jxl` (libjxl, optional) | animated JXL plays (frames coalesced; rotated animations show first frame only) |
| Camera RAW | `.3fr .arw .cr2 .cr3 .crw .dcr .dng .erf .fff .gpr .iiq .kdc .mef .mos .mrw .nef .nrw .orf .pef .raf .raw .rw2 .rwl .sr2 .srf .srw .x3f` | `raw` (LibRaw, optional) | |
| PGM/PPM | `.pgm .ppm` | `pgm` | binary; 8/16-bit grayscale keeps raw samples |
| Radiance HDR, PFM | `.hdr .pfm` | `hdr` (own RGBE/PFM parsers) | all Radiance axis orientations; float data clamped + sRGB-encoded for display; grayscale PFM keeps float samples |
| OpenEXR | `.exr` | `exr` (OpenEXR, optional) | single-part RGBA/Y, clamped + sRGB-encoded for display |
| DDS | `.dds` | `dds` | BC1–BC5, BC7, uncompressed (vendored bcdec) |
| ICO/CUR | `.ico .cur` | `ico` | PNG and DIB entries |
| HDF5 | `.h5 .hdf5 .he5` | `hdf5` (libhdf5, optional) | every numeric 2D/3D dataset is a page (PgUp/PgDn); 2D = gray (raw u8/u16/float samples), 3D = colour (last axis 3/4, first axis 3/4, or Image-spec `INTERLACE_MODE`) or slices (heuristic); content-sniffed |
| netCDF | `.nc .nc4 .cdf .netcdf` | `netcdf` (libnetcdf, optional) | every numeric 2D-4D variable is a page; 3D/4D leading axes = slices; _FillValue becomes NaN, scale_factor/add_offset applied, ascending latitude flipped north-up; classic files content-sniffed |
| NIfTI | `.nii .nii.gz` | `nifti` (own parser on zlib) | 2D/3D volumes, z = slice axis; scl_slope/inter applied; non-u8/u16 data becomes float |
| OpenRaster, Krita | `.ora .kra` | `ora` (zlib + stb_image) | shows the flattened merged image; layers not composited |
| FITS | `.fits .fit` | `fits` | float/int samples; NAXIS=3 cubes as slices |
| DICOM | `.dcm` | `dicom` | multi-frame as slices |
| NumPy | `.npy` | `npy` | 2D and 3D arrays (3D = slices) |
| SVG | `.svg` | built-in (nanosvg + expat) | shapes loaded as vector datasets; `<image>` elements; multilingual `<text>`; SMIL animation (see Animation) |
| giv vector | `.giv` | built-in parser | see "giv vector format" |

- Content sniffing: an explicitly named file with an unrecognised suffix is
  tried against plugins' content sniffers.
- EXIF Orientation applied centrally for all plugins.
- Non-ASCII paths supported on Windows.

## Image viewing

- Pan: middle-drag (optionally left-drag too, see Mouse settings; also
  keyboard arrow-style pan actions); zoom: right-drag, wheel at cursor, +/-
  keys, Up/Down. A drag keeps the grabbed point exactly under the cursor,
  also when a scrollbar clips the viewport.
- Content smaller than the window (zoomed out past "fill") stays centered
  along that axis.
- Fit to window (`F`/`Ctrl+0`, animated), actual pixels 1:1 (`1`), step zoom
  (`Ctrl+PgUp/PgDn`), double-click toggles 1:1 / fit.
- Zoom filtering: nearest-neighbour when zoomed in, bilinear when zoomed out.
- Pixel grid (`G`) and sub-pixel coordinate display.
- Rotate left/right in 90° steps (`L`/`R`, animated), horizontal/vertical flip.
- Keyboard pan (`Ctrl+arrows`) glides over ~0.15 s; rotation and pan glides
  follow the zoom-duration setting (0 = instant).
- Touchpad: pinch to zoom (where the platform delivers native gestures) and a
  horizontal two-finger swipe to navigate; vertical two-finger scroll zooms.
- Theme (Edit > Preferences > General): follow the system, light or dark. Uses
  the platform's colour scheme when supported, otherwise a built-in Fusion
  light/dark palette.
- Vertical zoom lock; auto-fit and "remember view per file" (preference: fit /
  remember per file / keep zoom and pan when switching images).
- Sticky View menu: Auto Contrast, Auto Fit / Remember View Per File, Force
  Opaque, Pixel Grid, Sub-pixel Coordinates, horizontal/vertical flip,
  Vertical Zoom Lock, Colormap Bar and Show Marks are saved when toggled and
  restored at startup. Not persisted on purpose: Balloon Tooltips (reset on
  every load), Image, Menu Bar, Auto Reload, Loop Animation.
- Force opaque (`A`) — ignore alpha channel.
- Glyph Appearance panel (View, `Ctrl+G`): global live multipliers for mark size, line width, quiver length and opacity (no scene rebuild; unused rows hidden). Gestures: Ctrl+LMB drag vertical = marks, horizontal = quiver length; Ctrl+RMB drag horizontal = line width, vertical = opacity; Alt+wheel = quivers. Force Opaque overrides opacity.
- Export (File > Export..., `--export`, RPC `export_image`): renders the current view off-screen at any size to PNG/JPEG/BMP/TIFF/WebP.
- Configurable background colour, MSAA (off/2x/4x).
- Full screen (`F11`), hideable menu bar (`Ctrl+M`).
- Drag-and-drop of files onto the window (incl. a native Windows drop target).
- Mouse settings: wheel = zoom or next/previous image (Ctrl+wheel always
  zooms), back/forward buttons (navigate / step zoom / none, swappable),
  tilt-wheel navigation, double-click action. Alt+wheel scales quivers.

## Browsing and navigation

- Next/previous/first/last image in the folder (`Shift+Up`/`Shift+Down`/
  `Backspace`, `Home`, `End`, Left/Right, `Space`; rebindable).
- Thumbnail strip under the canvas (`T`): multi-row grid, sideways scroll,
  adjustable size, decoded off-thread, shares disk cache with Open dialog.
- Background prefetch of neighbouring images with cancellable decodes and
  fast low-res previews, so holding the arrow key stays responsive.
- Multi-page TIFF paging (`PgUp/PgDn`, `Alt+Left/Right`).
- Multi-image `.giv` files cycled with Shift+Up/Down.
- Recent files menu.
- Status bar: switching images shows "Image changed to NAME (n/count)" for
  3 s, and the load message includes the page/slice the image opens on
  (`Slice N/M`) immediately.

## Animation

- GIF, APNG, animated WebP, animated JPEG XL, AVIF sequences play automatically, honouring
  per-frame delays and loop count; 768 MiB frame budget (larger animations
  show first frame only).
- Controls: play/pause (`K`/`Space`), stop (`X`), restart (`Shift+X`),
  next/previous frame (`.`/`,`), ±10 frames (`>`/`<`), faster/slower
  (`]`/`[`, 1/8x–8x), normal speed (`\`), loop toggle (`Shift+K`).
- Animation bar/scrubber; status bar shows `Frame N/F`.
- Vector animation in `.giv` via `$frame`/`$fps`/`$loop` (wiv extension).
- Animated SVG: SMIL `<animate>`, `<set>`, `<animateTransform>` and `<animateMotion>`
  (values/from/to/by, keyTimes/keyPoints, calcMode linear/discrete/spline, additive,
  fill=freeze, repeatCount, `rotate="auto"`, `<mpath>`, begin offsets and `id.begin/end`
  syncbase). Numbers, colors, lengths (`50` vs `90%`) and same-shape path data are
  interpolated; a `width="100%"` root gets an 800x600 default viewport. Frames (30 fps
  over one full cycle) are rendered on demand and cached. Motion paths follow arcs
  exactly. Not supported: event-based `begin` (click, mouseover...), `end`.
- CSS in SVG: `<style>` rules with `*`, element, `.class`, `#id`, compound, descendant
  and `>` selectors (rules with pseudo-classes or attribute selectors are ignored) are
  applied to the elements, and CSS animations - `@keyframes` with the `animation`
  shorthand or longhands - are converted to the equivalent SMIL animations: any
  presentation or geometry property plus `transform` (translate/scale/rotate/skew/matrix,
  `transform-origin`, `transform-box: fill-box`), duration, delay (also negative),
  iteration-count, direction (alternate/reverse), fill-mode, and timing functions per
  keyframe interval (`steps()` is treated as linear). CSS variables and `calc()` are not
  evaluated; percentages inside `translate()` are not supported.
- `stroke-dasharray` / `stroke-dashoffset` (any pattern length, animatable) and all CSS
  color names. Round/square line caps are not drawn, so zero-length dashes ("dots")
  vanish.
- SVG `<text>`/`<tspan>`: laid out with HarfBuzz (shaping, ligatures, kerning, variable-font
  weights), FriBidi (bidirectional text, `direction="rtl"`) and fontconfig (family lookup and
  per-character fallback for scripts the font lacks), then drawn as filled glyph outlines.
  Outlines scale, rotate and skew with the drawing, need no glyph atlas (new fonts cost
  nothing), and can be animated like any shape. Supports text-anchor, fill/stroke and their
  opacities, font family/size/weight/style, letter-spacing, `xml:lang`, `<tspan>` lines via
  x/y/dy, per-`<tspan>` styling within a line (one text-anchor for the whole line),
  `textLength` with `lengthAdjust` spacing/spacingAndGlyphs, and `<textPath>` (path data
  in `d`, `startOffset` in units or %, glyphs placed at their midpoints and turned to the
  tangent). Not supported: word-spacing, text-decoration, per-glyph x/y/rotate lists. (Windows has no fontconfig: it uses the Fonts folder
  with a fixed fallback list.)
- `.giv` text marks (`t`/`T` lines) use the same engine: shaped, bidi-aware outlines with
  giv's 1-9 anchor rule, `\n` multi-line, `$text_angle` rotation, and Pango-style `$font`
  descriptions (family, Bold/Italic/Light..., size). Pixel-sized labels (the default) keep
  their size under zoom; `$scale_font` labels scale with the drawing. No glyph atlas, so a
  new font costs nothing; the caliper label uses it too.
- `$pango_markup`: labels are parsed as Pango markup by a built-in parser - `<b> <i> <u>
  <s> <big> <small> <sub> <sup> <tt>` and `<span>` with font/family/size (1024ths of a pt,
  keywords, pt/px)/style/weight/foreground/background/alpha/underline (single, double,
  low, colored)/strikethrough/rise/letter_spacing, plus entities. Malformed markup is
  shown literally. Balloons are unaffected.
- `$text_style shadow` with `$shadow_color` / `$shadow_offset`: the label is drawn once
  more underneath, shifted by the offset (pixels; data units with `$scale_font`).

## Scientific / pixel-value tools

- Raw sample retention (U8/U16/float) for grayscale formats, so contrast
  operates on real dynamic range, not quantised RGBA8.
- Adjust Contrast panel (`C`): histogram with draggable min/max handles,
  min/max and center/window fields, auto contrast; live view.
- Whole-volume histogram toggle for multi-slice data.
- Contrast stretch and colormap applied on the GPU.
- Color Table / pseudo-colour (`P`): None, Invert, Low Contrast, Rainbow,
  Red Temperature, Blue-Green-Red-Yellow, Blue-White, Green-Red-Blue-White,
  Equalize and Normalize (tables derived from the displayed 8-bit image)
  (ported from giv); colormap bar under the canvas showing min..max mapping.
- Multi-slice volumes (FITS cubes, DICOM multi-frame, 3D npy, netCDF): slice
  stepping, `Slice N/M` in status bar, per-slice histogram.
- Tools > Measure Distance submenu (`Z` toggles): Diagonal, Horizontal and Vertical caliper modes (mode remembered), draggable jaws with highlight and crosshair/hand cursors, status-bar messages.
- Count Marks (`N`): caliper rectangle that counts the `.giv` marks inside it.
- Calibrate Pixel Size panel (`S`): set real-world units per pixel from a
  known value, last measurement, or image width/height; caliper then reads in
  real units.
- Balloon tooltips (`B`) showing the label/text of the `.giv` mark under the
  cursor (giv behaviour). Not verified whether they show raster pixel values.

## Remembered state

Stored in `state.db` (SQLite, WAL mode) next to `wiv.ini` (e.g.
`~/.config/wiv/` on Linux), implemented in `src/ImageStateStore.{h,cpp}`.

- Per image: contrast window (min/max), colormap and its on/off flag,
  pan/zoom (centre plus device-pixels-per-unit scale, so a resized window
  keeps the zoom), and the slice of a volume.
- Per multi-image scene (the files it was loaded from): which image was
  showing.
- Keys are canonical absolute paths, so `../x.fits` and `/abs/x.fits` are the
  same image. A record is ignored when the file's size or modification time
  has changed since it was saved.
- The saved contrast window is restored only with Auto Contrast off (with it
  on, images are stretched to their own range); the colormap is restored
  either way.
- Everything above is read/written only while **Remember View Per File** is
  on; with it off the database is not even opened. (Contrast/colormap are
  still remembered within a run.)
- The least recently used entries beyond 2000 are pruned; several wiv
  instances can share the file.

## Metadata and file operations

- Image Info dock (`I` or `Alt+Enter`): file/image properties plus EXIF (JPEG, TIFF/DNG,
  PNG eXIf, WebP): camera, exposure, GPS; header attributes of Radiance HDR
  (EXPOSURE, comments, ...), PFM (byte order, scale) and OpenEXR (channels,
  compression, windows, chromaticities, string/float attributes).
- File menu: Open (`Ctrl+O`), Reload (`Enter`), Copy (`Ctrl+C`), Rename
  (`F2`), Delete (`Del`), Open File Location (`W`),
  Quit (`Q`).
- Auto Reload: polls file and reloads when it changes (500 ms).
- Open in External App (`E`) / Open With submenu: configurable apps
  (`Ctrl+1..9`), system default, choose application, per-extension rules;
  installed-app picker.
- Custom Open dialog: inline thumbnails for every supported type (raster,
  SVG, `.giv`), directory tree sidebar with places/Recent/Bookmarks/drives,
  list/detail modes, grid zoom, disk-cached thumbnails, remembers
  position.

## giv vector format (`.giv`)

- mmap text parser, no line-length limit, fast path for bare `x y` lines;
  verified on 10M-point files (parse ~0.3 s, draw essentially free).
- GPU-instanced marks, lines (dashed) and fills; separate draw per class.
- Directives parsed: `$arrow $balloon $color $def_style $font $fps $frame
  $hide $image $line $line_cap $linedash $loop $lw $marks $mark_size $name
  $noline $nomark $outline_color $pango_markup $path $polygon $quiver_color
  $quiver_head $quiver_scale $scale_font $scale_marks $shadow_color $shadow_offset
  $style $text_angle $text_size $text_style $title $vlock`, plus `$background` (wiv extension: viewport colour while the file is loaded), ellipses and beziers.
- `$svgmarks` draws the SVG at every dataset point (native size, origin on the point, replaces the mark shape); `$svg` draws it once at the origin.
- Ellipses (`e x y w h angle`) follow giv: full width/height, angle in radians, positive = clockwise on screen.
- Lines are drawn as capsules: round joins, caps per `$line_cap`, analytic anti-aliased edges, translucent overlaps blend once (SVG strokes keep plain rectangles).
- SVG gradient fills and strokes are rasterized (nanosvgrast, up to 4x oversampled) and drawn as textured sprites.
- Parsed but ignored: `$marks_file $low_contrast $hflip/$vflip`
  (and `no*`), `$pixelsize`.
- Marks: circle, filled circle, square, filled square (+ others per parser).
- Lines: width, dash patterns (multi-value patterns), caps, arrows.
- Polygons (earcut-triangulated, so concave polygons fill correctly), holes.
- Quivers with heads and scale.
- Text with numeric-keypad alignment, rotation, size, font via fontconfig (HarfBuzz/FriBidi outlines).
- Colours: X11 names, `name/alpha` suffix syntax, hex.
- `$image` embeds raster images (one pixel = one world unit).
- Mark Browser dock (`O`): checkbox tree with a top-level node per source file, then the `$path` hierarchy, to show/hide
  files, datasets or groups; Show Marks toggle (`M`).
- Multiple `.giv` files merged into one scene.
- Remote control via JSON-RPC (see below).

## Remote control

- `--rpc-port PORT`: JSON-RPC server on 127.0.0.1 (off by default).
- Methods: `ping`, `help`, `load_file`, `giv_string` (optionally `-append`),
  `get_transformation`, `set_transformation`, `pick_coordinate` (Esc aborts it), `list_actions`, `trigger_action`, `resize_window`, `set_colormap`, `set_contrast`, `mouse_move`, `export_image`, `screenshot`.
- Remote client mode: `--remote "method params"` (or `--json_method`/`--json_params`, `--remote-host`, `--remote-port`) sends one command to a running instance and exits.

## Preferences (Edit > Preferences)

- **General**: background colour, antialiasing, behaviour when switching
  images.
- **Mouse**: wheel, side buttons, tilt, double-click.
- **External Apps**: app list, extension rules.
- **Keyboard**: rebind actions.
- Settings persisted via `QSettings("wiv","wiv")` (INI file); per-image
  state lives in `state.db` (see "Remembered state").

## Build dependencies

Besides Qt 6, VSG/vsgXchange and spdlog: SQLite3 (system package on Linux;
vcpkg `sqlite3` on Windows; `mingw64-sqlite` for the MinGW cross build).

## Command line

```
wiv [--geometry WxH] [--rpc-port|--port PORT] [--join] [--auto-reload] [-n]
    [--zoom scale sx sy] [--export FILE] [--log_file FILE] [file ...]
wiv --remote "method params" [--remote-host HOST] [--remote-port PORT]
```

Env vars: `WIV_PLUGIN_DIR`, `WIV_CONTINUOUS=1`, `WIV_TIMING=1`,
`WIV_CAPTURE=1` (Windows desktop capture diagnostic).

## Diagnostics

- Timestamped logs in `~/.wiv/logs/` capturing user actions and stdout/stderr.
- FPS and per-stage render timing in status bar / stderr.

## Not supported (useful for comparison)

Absence is inferred from the menus and source layout, not exhaustively
proven; re-check before relying on an item.

- No image editing (crop, resize, adjust colour, annotate); export only renders the current view.
- No slideshow mode, no print support, no ICC colour management.
- No video playback.
- No EXIF editing; EXIF only for JPEG/TIFF/PNG/WebP.
- No remote/network file loading beyond local paths.
- No bundled font; text depends on fontconfig (Linux) / system fonts.
