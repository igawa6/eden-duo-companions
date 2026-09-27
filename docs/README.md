# Eden Duo: dual-screen companion runtime

Eden Duo is a fork of the Eden Nintendo Switch emulator. Its dual-screen mod runtime ("DSMod" in
the code) works like this: while a game runs on the main screen, a second screen shows a live **companion page**, such as a map, inventory, party status, a quest
log or a battle helper. The page is built from the game's own memory and the player's own game
files. It updates in real time, and the player can use it by touch.

Nothing in the emulator is specific to one game. Each game gets a **package**, which contains:

- `manifest.json`: a declarative description of the pages, widgets and data bindings, plus the
  actions that touches trigger.
- An optional per-build data file that says where values live in memory (pointer chains).
- An optional **native module**. This is a small shared library, written in C or C++ against a
  stable C ABI. It reads game state that is too complex for pointer chains, publishes named
  values, and can decode art straight from the game's romfs.

This documentation set is written for engineers who want to understand the method or build a
companion for a new game.

Documentation last checked against the source: 2026-09-27 (GMT+7), runtime version 12,
module ABI 1.

## Repositories

| Repository | Holds |
|---|---|
| [Eden Duo](https://github.com/igawa6/eden-duo) | The emulator, the runtime (`src/core/mods/`) and the native title-module sources (`src/core/mods/modules/`) |
| This companions repository | Package sources (`packages/<Game>/dualscreen/...`), tools (`tools/build_dualscreen_package.py`, `tools/build_release.sh`, `tools/compact_zip.py`, `tools/p5r/`, `tools/dread/`) and these docs |

Source paths such as `src/core/mods/mod_manifest.cpp` in these docs refer to the Eden Duo
repository. Paths under `packages/` and `tools/` refer to this repository.

## Components

```
                 ┌────────────────────── Eden Duo process ───────────────────────┐
                 │                                                                │
  game (guest) ──┼─► guest memory ◄──── reads ────┐                               │
                 │                                │                               │
  romfs (user's  │                     ┌──────────┴───────────┐   C ABI           │
  own dump) ─────┼──── read_romfs ────►│ ModRuntime            │◄──────────► native module
                 │                     │  (core/mods)          │  sample/tick   (per title .so,
                 │  CoreTiming 60 Hz ─►│  sample → derive →    │  publish_*      loaded from
                 │  "DSMod::Tick"      │  gestures → actions → │  on_action      the package)
                 │                     │  page binds → publish │  load_image
                 │                     │                       │  load_data
                 │                     └──────────┬───────────┘                   │
                 │                  RedrawJob     │  (low-priority worker)         │
                 │                     ┌──────────▼───────────┐                   │
                 │                     │ RenderPage (mod_ui)   │ CPU canvas or     │
                 │                     │ "DSModRedraw" thread  │ GPU quad list     │
                 │                     └──────────┬───────────┘                   │
                 │                                │ PublishUi / PublishGpuComposite│
                 │                     ┌──────────▼───────────┐                   │
                 │                     │ AuxRouting            │ tile diff, touch, │
                 │                     │ (video_core/dsmod)    │ haptics, dsm:u    │
                 │                     └──────────┬───────────┘                   │
                 │                                │ GPU thread (Vulkan)            │
                 │                     ┌──────────▼───────────┐                   │
                 │                     │ aux swapchain +       │                   │
                 │                     │ own present thread    │                   │
                 │                     └──────────┬───────────┘                   │
                 └────────────────────────────────┼──────────────────────────────┘
                                                  ▼
             desktop: SDL3 "Screen 2" window (eden-cli)   Android: Presentation on 2nd display
                                                  │
                                    touch ────────┘ back into AuxRouting → ModRuntime
```

| Component | Where (Eden Duo repository) | Role |
|---|---|---|
| `ModRuntime` | `src/core/mods/mod_runtime.h`; lifecycle and the tick in `mod_runtime.cpp`, split by area into `mod_*.cpp` (see [ARCHITECTURE.md](ARCHITECTURE.md)) | Discovers packages, runs the 60 Hz tick, samples memory, evaluates derived values, handles gestures and actions, and decides when to redraw. |
| Package discovery and parser | `mod_manifest.cpp` | `Discover`, the `min_runtime` gate, `ParseManifestJson` and the per-build data file. |
| Manifest types | `mod_types.h` plus `mod_types_points.h`, `mod_types_map.h`, `mod_types_text.h`, `mod_types_page.h`, `mod_types_action.h`, `mod_types_guest.h`, `mod_types_composite.h` | Data structs for pages, widgets, points, actions, the map and the `StateSnapshot`. |
| Renderer | `mod_ui.h`, `mod_ui.cpp` (`RenderPage`) and `mod_ui_*.cpp` (canvas, text, image, map widget, scroll, expansion, transitions, widget state) | Draws a page into a CPU canvas, or builds a GPU quad list for map widgets. |
| Redraw and publish | `mod_redraw.cpp` | The redraw decision, the `DSModRedraw` worker and the publish to `AuxRouting`. |
| Asset layer | `mod_assets.cpp`, `mod_nx_runtime.cpp`, `mod_nx_assets.cpp`, `mod_msbt.cpp`, `mod_map.cpp`, `engine_mercury.cpp`, `engine_ichigo.cpp` | Asset bytes (`file:`, `romfs:`, `module:`), the user's romfs art (BNTX, SARC, BFFNT and others), composites, game text (MSBT), the map rasteriser and per-engine decoders. |
| Module host | `mod_module.cpp` (loader), `mod_module_host.cpp`, `mod_module_services.cpp` | Verifies, stages and `dlopen`s the native module, and implements the host side of the C ABI and its extensions. |
| Module ABI | `dsmod_module_abi.h`, `dsmod_module_extensions.h` | Stable C interface. Has no emulator dependency. |
| Load plan | `mod_load_plan.cpp/.h` | Optional code patches and a guest mailbox, applied when the executable loads. |
| Dev tools | `mod_console.cpp`, `mod_re_tools.cpp` | Live console and reverse-engineering scanners. Built only with `EDEN_DSMOD_BUILD_DEV_TOOLS=ON`. |
| Transport | `src/video_core/dsmod/aux_routing.h` (`AuxRouting`) | Shared state between the runtime, the renderer and the frontend: pixels, dirty tiles, touch and haptics. |
| `dsm:u` service | `src/core/hle/service/dsm/` | HLE service so homebrew or guest code can query the aux display, read touch, or bind a VI layer to it. |
| Desktop window | `src/yuzu_cmd/emu_window/emu_window_sdl3_aux.cpp` | SDL3 second window for `eden-cli`. The Qt frontend has no aux support. |
| Android | `AuxPresentation.kt`, `DualScreenPackageInstaller.kt` | Presentation on the second display, touch and haptics, and the `.dsmod.zip` installer. |
| Title modules | `src/core/mods/modules/` | Standalone CMake project with one `.so` per title, named by title ID, plus the header-only module SDK (`dsmod_module_sdk.h`). |

## Build

### Desktop (`eden-cli`, Linux)

The development tree is built with Ninja and clang, in Release mode, with the SDL frontend
(`YUZU_CMD=ON`). Qt was switched off (`ENABLE_QT=OFF`) because of a Qt version skew on the build
machine, not because the dual-screen runtime needs it off.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DYUZU_CMD=ON -DENABLE_QT=OFF -DYUZU_TESTS=ON \
      -DEDEN_DSMOD_BUILD_DEV_TOOLS=ON
cmake --build build --target yuzu-cmd -j"$(nproc)"
# binary: build/bin/eden-cli
```

The option values above come from the working tree's `CMakeCache.txt`. The original configure
command was not recorded, so treat this line as a reconstruction. Set
`EDEN_DSMOD_BUILD_DEV_TOOLS=OFF` for a release-like build without the console and RE scanners.

Run it with a second screen:

```sh
build/bin/eden-cli --aux-window game.nsp      # real second window ("Screen 2")
build/bin/eden-cli --aux-virtual game.nsp     # windowless 1240x1080 aux canvas (headless)
```

### Android

The Gradle recipe below assumes NDK 28.2.13676358, CMake 3.31.6 and Java 17.

```sh
export ANDROID_HOME=~/android-sdk ANDROID_SDK_ROOT=~/android-sdk \
       ANDROID_NDK_ROOT=~/android-sdk/ndk/28.2.13676358
cd src/android
./gradlew assembleMainlineRelWithDebInfo     # arm64 (handheld)
./gradlew assembleChromeOSRelWithDebInfo     # x86_64 (emulator images)
# APK: app/build/outputs/apk/mainline/relWithDebInfo/
```

The application ID is `dev.igawa6.edenduo`. The `relWithDebInfo` build type installs beside it as
`dev.igawa6.edenduo.dev` ("Eden Duo Dev") and keeps the dev tools on. The signed `release` build
type passes `-DEDEN_DSMOD_BUILD_DEV_TOOLS=OFF`.

### Title modules

Title modules are **never** built into the APK or the emulator. Their sources live in the Eden Duo
repository under `src/core/mods/modules/`, which is a separate CMake project, described in
[MODULE_GUIDE.md](MODULE_GUIDE.md#4-build).

```sh
cmake -S src/core/mods/modules -B /tmp/mods -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/mods --target dsmod-p5r
```

## Install a package

A package ships as `<TITLEID>.dsmod.zip` or `<TITLEID>-<Name>-<version>.dsmod.zip`:

```
01005CA01580E000.dsmod.zip
├── package.json
└── dualscreen/
    ├── manifest.json
    ├── <BUILD16>.json                   # optional per-build data file
    ├── ...                              # optional package files (file: sources)
    └── modules/<platform>/<TITLEID>.so  # optional native module
```

- **Android:** open the game's Add-ons screen, choose Install, then **Dual-screen mods**, and pick
  the zip. The installer checks the archive, including the module SHA-256 values and title IDs,
  and extracts it to `load/<TITLEID>/DualScreen-<TITLEID>/`, or to `load/<TITLEID>/<Name>-<version>/`
  for the named form. It then removes the title's older installer-created package folders;
  hand-made folders without a `package.json` are left alone.
- **Desktop:** unzip the package into `<data dir>/load/<TITLEID>/<AnyName>/`, so that
  `dualscreen/manifest.json` sits directly under that folder. The Qt frontend's generic per-game
  "install mod" dialog copies folders to the same place, but it does not validate
  `package.json`. `eden-cli` has no package installer; its `--install` flag is for NSPs.
- Each mod folder can be enabled or disabled like any other add-on. When several dual-screen
  folders exist for one title, the first usable one in name order wins. There is no merging.

If a package declares a `min_runtime` newer than the emulator supports, the second screen shows
a built-in "UPDATE EDEN" page (the literal text on screen) and does not load the package.

## Where to go next

| Doc | Read it for |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Per-frame data flow, threads and locks, versioning, performance design, extension points |
| [PACKAGE_FORMAT.md](PACKAGE_FORMAT.md) | `package.json` and `manifest.json` reference with real examples |
| [MODULE_GUIDE.md](MODULE_GUIDE.md) | Writing a native per-title module, with Persona 5 Royal and Metroid Dread as worked examples |
| [PORTING_A_GAME.md](PORTING_A_GAME.md) | The end-to-end method for a new game, and the gotchas list |
| [GLOSSARY.md](GLOSSARY.md) | Terms |

## Legal boundary

A package ships no game code, art, fonts or text. Art and text are decoded at runtime from the
player's own game dump, through `romfs:` sources or a module's `read_romfs`. Build IDs and code
offsets are short identifiers, not game content.
