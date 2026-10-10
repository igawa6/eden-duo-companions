# Native companions

Game-specific C++ sources, tests and development tools live in [`modules/`](modules/).
They build independently of the emulator and are installed only through `.dsmod.zip` packages.
Link's Awakening is declarative and has no native module.

## Compatible host and dependencies

Use the igawa6 [Eden Duo](https://github.com/igawa6/eden-duo) checkout at `v1.2.0`
(commit `8c28fd5d745d1ba5756a85dee00f5587ce0b35ad`, runtime 19), or a tested SDK-compatible
successor.
The generic C ABI and header-only SDK stay in that repository. Select its absolute path with
`-DEDEN_SOURCE_ROOT=...`; the build fails clearly if the SDK is missing. A C ABI version alone
does not guarantee that every optional extension or helper is available in an older checkout.

Initialize its submodules and configure Eden Duo once to populate its dependency cache, following
the Eden Duo build instructions. Native builds reuse its fmt 12.1.0 and nlohmann/json 3.12.0
headers, zstd decoder sources and third-party BC/STB decoders. `EDEN_CPM_CACHE` can override the
cache location. No emulator library is linked. This source move does not change licenses.

Game-independent host headers remain available at their existing `core/mods/...` include paths.
Companion headers use local includes. Keep the native modules together: Fire Emblem uses Dread's
map utility and Persona 5's publication filter. These helpers are companion source, not host code.

## Linux and tests

Run from this repository:

```sh
cmake -S native/modules -B build-native-linux -G Ninja \
  -DEDEN_SOURCE_ROOT=/absolute/path/to/eden-duo \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-native-linux --parallel 4 --target \
  dsmod-p5r dsmod-dread dsmod-mk8d dsmod-wonder dsmod-acnh dsmod-fe3h dsmod-isaac \
  dsmod-lp dsmod-lp-pearl dsmod-ctr dsmod-dq3 dsmod-ce
```

To run the existing synthetic tests, configure another build with `-DBUILD_TESTING=ON`, build
its default target, then run `ctest --test-dir <build-directory> --output-on-failure`.
Tests requiring your own game dump skip without it, except `dsmod-isaac-reader`: it fails unless
its fixtures are configured (`ISAAC_ABP_IMAGE`, `ISAAC_REP_IMAGE`, `ISAAC_ROMFS`, `ISAAC_FILE`, from
the configure environment or `-D`), or `-DISAAC_FIXTURES_OPTIONAL=ON` reports it as skipped.
Test executables, fixtures and development tools stay in the source/build tree and are not
passed to the release package script.

## Android arm64

```sh
cmake -S native/modules -B build-native-android -G Ninja \
  -DEDEN_SOURCE_ROOT=/absolute/path/to/eden-duo \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_SDK_ROOT/ndk/28.2.13676358/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DEDEN_ACNH_BUILD_DIAGNOSTICS=OFF
cmake --build build-native-android --parallel 4 --target \
  dsmod-p5r dsmod-dread dsmod-mk8d dsmod-wonder dsmod-acnh dsmod-fe3h dsmod-isaac \
  dsmod-lp dsmod-lp-pearl dsmod-ctr dsmod-dq3 dsmod-ce
```

Keep this build outside Gradle's `.cxx` directories. Use `c++_static` so the installed module does
not require a separately distributed `libc++_shared.so`. ACNH diagnostics default to off.

| Target | Library |
|---|---|
| `dsmod-p5r` | `01005CA01580E000.so` |
| `dsmod-dread` | `010093801237C000.so` |
| `dsmod-mk8d` | `0100152000022000.so` |
| `dsmod-wonder` | `010015100B514000.so` |
| `dsmod-acnh` | `01006F8002326000.so` |
| `dsmod-fe3h` | `010055D009F78000.so` |
| `dsmod-isaac` | `010021C000B6A000.so` |
| `dsmod-lp` | `0100000011D90000.so` |
| `dsmod-lp-pearl` | `010018E011D92000.so` |
| `dsmod-ctr` | `0100F9F00C696000.so` |
| `dsmod-dq3` | `01003E601E324000.so` |
| `dsmod-ce` | `0100C510166F0000.so` |

Strip each platform's library with the matching toolchain's `llvm-strip --strip-all` and pass
only those libraries to [`tools/build_release.sh`](../tools/build_release.sh), as documented in
[`tools/README.md`](../tools/README.md). Existing published APKs, archives and release tags are
unchanged by this repository move.

## Source provenance

The module tree was transferred from igawa6/eden-duo commit
`60c1ef31b600511927f5fb10f0cd34918e99a7a2`, preserving existing copyright and license notices.
Only include paths, standalone build configuration and documentation change in the migration.
Use that immutable commit to locate source for the original companion releases; development
after the migration takes place here.
