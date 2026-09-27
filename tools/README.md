# Tools

| Path | What it does |
|------|--------------|
| `build_dualscreen_package.py` | Packs a `packages/<Game>/` directory plus optional native modules into a reproducible `<TITLEID>.dsmod.zip`. It validates the package first: `file:` references, module sha256 pins, build IDs and JSON. |
| `compact_zip.py` | Rewrites `dualscreen/manifest.json` inside an archive without whitespace. The Android installer caps that file at 4 MiB. |
| `build_release.sh` | Builds all three release archives with their release names, `<TITLEID>-<Name>DS-<version>.dsmod.zip`. |
| `p5r/` | Persona 5 Royal page generators: manifest, art recipe table and package directory. See [`p5r/README.md`](p5r/README.md). |
| `dread/` | Metroid Dread page generator, the asset-free conversion and the Python reference for the module's map generator. See [`dread/README.md`](dread/README.md). |
| `links-awakening/` | Notes on the Link's Awakening package, which is maintained by hand. |

## Requirements

- Python 3.10 or newer.
- [Pillow](https://pypi.org/project/Pillow/), for `p5r/`, `dread/bctex_decode.py` and `dread/dread_af/cmp_live*.py`.
- [mercury-engine-data-structures](https://pypi.org/project/mercury-engine-data-structures/) (which brings `construct`), only for the Dread romfs tools in `dread/`.
- `llvm-strip`, for `p5r/build_pkg_af.sh`.
- `sha256sum`, `mktemp` and bash, for the shell scripts.

`build_dualscreen_package.py`, `compact_zip.py` and `build_release.sh` need only the Python standard library.

## Building the release archives

The native modules are built from the Eden Duo source tree. Each module's output is named after its title ID:

```sh
cmake -S src/core/mods/modules -B build-mods -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-mods dsmod-p5r dsmod-dread
llvm-strip --strip-all -o p5r-linux.so build-mods/01005CA01580E000.so
llvm-strip --strip-all -o dread-linux.so build-mods/010093801237C000.so
```

The module tree is a standalone CMake project. It takes fmt and nlohmann_json headers from Eden
Duo's CPM cache (`.cache/cpm`), so configure Eden Duo once first. Build the `android-arm64-v8a`
variants the same way with the Android NDK toolchain file (`-DANDROID_ABI=arm64-v8a
-DANDROID_STL=c++_static`). Then, from this repository:

```sh
P5R_LINUX_SO=p5r-linux.so P5R_ANDROID_SO=p5r-android.so \
DREAD_LINUX_SO=dread-linux.so DREAD_ANDROID_SO=dread-android.so \
tools/build_release.sh dist
```

`build_release.sh` takes the build IDs from each package's manifest and pins each module's sha256
in `package.json` and `dualscreen/manifest.json`. The archives are reproducible: the same inputs
give byte-identical zips.

## Game files

None of these tools needs game files to build a release archive from the committed packages. The
generators in `p5r/` and `dread/` read **your own** game files (an unpacked romfs) only to
regenerate a manifest or to check the modules' output. Their paths are set through environment
variables (`P5R_*`, `DREAD_ROMFS`). Nothing they read is written into a package except our own
layout values and romfs references.
