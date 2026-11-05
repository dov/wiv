# Building wiv

This page covers building wiv on Linux and Windows, cross compiling for
Windows from Fedora, and producing the Windows installer. For what the
application does, see [README.md](README.md).

## Common requirements

- CMake 3.21 or newer (3.28 or newer keeps vsgQt's example apps and install
  rules out of the build) and Ninja
- Network access at configure time: CMake fetches
  [vsgQt](https://github.com/vsg-dev/vsgQt) v0.4.0 from GitHub
- `glslangValidator`, to compile the shaders at build time. vcpkg provides it
  on Linux; on Windows it comes from the Vulkan SDK on `PATH` (or vcpkg).

Dependencies come from [vcpkg](https://vcpkg.io), through the manifest
`vcpkg.json`: `vsg` (with its `windowing` feature), `vsgxchange` (with
`freetype`), `tiff`, `libjpeg-turbo`, `libwebp`, `zlib`, `spdlog`, `cfitsio`
and, as a host tool, `glslang` (which provides `glslangValidator`). The first
configure builds them, which takes a while. Qt 6 is the exception: on Linux it
comes from the system packages, and on Windows from vcpkg (`qtbase`).

## Linux

Install Qt 6 (with the Widgets, Concurrent and Network modules), a C++20
compiler and the X11/Vulkan development headers that vcpkg's `vsg` windowing
feature needs from your distribution. vcpkg itself must be checked out and
bootstrapped, with `VCPKG_ROOT` pointing at it; the `linux-*` presets use its
toolchain file.

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset linux-release
cmake --build --preset linux-release
```

The binary is `build/linux-release/wiv`, with the image plugins in
`build/linux-release/plugins`. wiv finds the plugins through the compile-time
build directory, so if you copy the binary elsewhere, point `WIV_PLUGIN_DIR`
at the plugin directory when running it. The vcpkg shared libraries live in
`build/linux-release/vcpkg_installed` and are found through the binary's rpath,
so they need to stay where they are as well.

```sh
build/linux-release/wiv examples/example.giv
```

A debug build uses the `linux-debug` presets in the same way. At run time,
font family lookup calls the system's `fc-match`, so install fontconfig.

## Windows

### Prerequisites

- Visual Studio with the C++ workload
- CMake and Ninja (both ship with Visual Studio)
- The Vulkan SDK, for `glslangValidator`
- vcpkg checked out at `c:\vcpkg`, which is where the `windows-release`
  preset looks for the toolchain file

The vcpkg manifest baseline is pinned to the same commit as the reference application, so Qt6
is restored from the shared binary cache
(`%LOCALAPPDATA%\vcpkg\archives`) instead of being rebuilt. The preset also
places vcpkg build trees under `C:\vbt` to keep paths short.

### Build

Open a Visual Studio developer prompt (x64) and run:

```bat
cmake --preset windows-release
cmake --build --preset windows-release
build\windows-release\wiv.exe examples\example.giv
```

## Windows installer

The installer is built with [NSIS](https://nsis.sourceforge.io). The script
`Installers\BuildInstaller.bat` configures the toolchain, builds, stages the
files and runs `makensis`.

### Prerequisites

- Everything listed under [Windows](#windows)
- NSIS installed at `C:\Program Files (x86)\NSIS` (the default location)
- Git on `PATH`, used to read the short commit id

`BuildInstaller.bat` sets up the Visual Studio environment itself, using the
paths defined near the top of the script (`VS_VCVARS`, `VS_NINJA` and
`MAKENSIS`). Edit them if your Visual Studio edition or NSIS location differs.

### Build steps

By default the script builds from `build\windows-RelWithDebInfo`, so the
installed executable has a matching PDB for reading crash dumps. Configure that
directory once before the first run:

```bat
cmake -S . -B build\windows-RelWithDebInfo -G Ninja ^
      --toolchain c:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
      -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
      -DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/vbt
```

Then build the installer:

```bat
Installers\BuildInstaller.bat
```

To use a different build directory, such as the one from the `windows-release`
preset, pass it as the first argument:

```bat
Installers\BuildInstaller.bat build\windows-release
```

The script performs these steps:

1. Builds the project in the chosen build directory.
2. Runs `cmake --install` into `Installers\tmp`, giving a layout of `bin\`
   (executable, DLLs and Qt plugins), `lib\wiv\plugins\`, `share\wiv\shaders\`
   and `examples\`.
3. Runs `makensis` on `Installers\wiv.nsi`, passing the version from
   `vcpkg.json` and the short commit id.
4. Copies `wiv.pdb` next to the installer.

The results are in the `Installers` directory:

- `Installwiv-v<version>-<commit>.exe`: the installer
- `Installwiv-v<version>-<commit>.pdb`: the matching debug symbols, which are
  not shipped inside the installer

The `--copy-w` option additionally copies both files to a network share. The
destination is hard-coded as `DEST` in the script, so adjust it first.

### What the installer does

It installs machine-wide into `C:\Program Files\wiv` (it asks for elevation),
and registers an uninstaller.

## Windows cross build from Fedora (MinGW)

This builds the Windows executable and installer on Linux without Visual
Studio or vcpkg.

```sh
sudo ./install-mingw-deps.sh      # one time: dnf packages (Qt6, Vulkan, NSIS, ...)
./windows-cross-compile.sh        # build, stage and create the installer
```

On the first run, `tools/build-mingw-deps.sh` builds `vsg`, `vsgXchange` and
`spdlog` (the other libraries come from the dnf packages) into a dependency prefix. It is invoked automatically by the build,
and you can also run it directly. Set `MINGW_DEPS_PREFIX` to use a prefix other
than `/usr/local/mingw64`.

`windows-cross-compile.sh` is a wrapper around `tools/build-mingw.sh`. Pass
`--no-installer` to build only the staged tree. Output:

- `build/mingw-release/stage/`: the staged install tree, with every non-system
  DLL the executable and plugins depend on copied into `bin/`
- `Installers/Installwiv-v<version>-<commit>.exe`: the installer, built with
  `makensis` from the same `Installers/wiv.nsi`

The toolchain file is `cmake/mingw-w64-x86_64.cmake`.

## Version and commit id

The version comes from `vcpkg.json`. The commit id shown in Help > About is
baked in at build time (`cmake/GenBuildInfo.cmake`), so a new commit triggers a
rebuild of the info file, and the installer file name carries the same short
commit id.

## Troubleshooting

- **Window creation fails silently on Linux:** the `vsg` port must be built
  with its `windowing` feature. It is requested in `vcpkg.json`, so this only
  happens if the manifest was altered.
- **Image formats don't load:** the plugin directory wasn't found. Set
  `WIV_PLUGIN_DIR`.
- **`glslangValidator` not found:** vcpkg normally provides it (the `glslang`
  host dependency). On Windows you can instead install the Vulkan SDK and put
  it on `PATH`.
- **Qt gets rebuilt on Windows:** the vcpkg binary cache isn't being picked up.
  Check that the manifest baseline in `vcpkg.json` is unchanged and that
  `%LOCALAPPDATA%\vcpkg\archives` holds the cached packages.
