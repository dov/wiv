# Third-party components

wiv itself is licensed under the GNU GPL version 3 (see [COPYING](COPYING)).
It includes and uses the following third-party software.

## Bundled in this repository (`third_party/`)

| Component | Use | License |
| --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) (Sean Barrett) | PNG, JPEG, BMP, TGA, GIF, PSD decoding | MIT or public domain (Unlicense), see the end of `third_party/stb_image.h` |
| [nanosvg / nanosvgrast](https://github.com/memononen/nanosvg) (Mikko Mononen) | SVG parsing and gradient rasterization | zlib license, see the headers in `third_party/` |
| [bcdec](https://github.com/iOrange/bcdec) (Sergii Kudlai) | DDS block-compression decoding | MIT or public domain (Unlicense), see the end of `third_party/bcdec.h` |
| [QtAwesome](https://github.com/gamecreature/QtAwesome) (Rick Blommers) | Icon loader for Qt | MIT, see `third_party/QtAwesome/LICENSE.md` |
| [Font Awesome Free](https://fontawesome.com) 6 | Toolbar and menu icons (fonts in `third_party/QtAwesome/QtAwesome/fonts`) | Icons CC BY 4.0, fonts SIL OFL 1.1, code MIT; see `third_party/QtAwesome/LICENSE.md` |
| `X11-rgb.txt` | X11 colour names for `.giv` files | X Consortium license (see the file header) |

## Libraries linked or loaded at run time

These are not distributed in the source tree; they are obtained from your
system or from vcpkg at build time. Each remains under its own license.

- [Qt 6](https://www.qt.io) (Core, Gui, Widgets, Concurrent, Network)
- [VulkanSceneGraph](https://vulkanscenegraph.org) and vsgXchange
- [spdlog](https://github.com/gabime/spdlog), [SQLite](https://sqlite.org),
  [Expat](https://libexpat.github.io), [zlib](https://zlib.net)
- [HarfBuzz](https://harfbuzz.github.io), [FriBidi](https://github.com/fribidi/fribidi),
  [Fontconfig](https://www.fontconfig.org), FreeType
- Image-format plugin libraries: libjpeg-turbo, libtiff, libwebp, cfitsio, and,
  where installed, libheif, libjxl, LibRaw, OpenEXR and HDF5

## Format compatibility

The `.giv` file format and the JSON-RPC protocol originate in
[giv](https://github.com/dov/giv).
