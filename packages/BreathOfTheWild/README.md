# The Legend of Zelda: Breath of the Wild companion

Title `01007EF00011E000`, game versions 1.6.0 (`8E9978D50BDD20B4…`) and 1.9.0
(`CD57B23FA4BBAD65…`), with or without the DLC.
Companion 1.1.0 for Eden Duo 1.3.2 (runtime 22) or newer.

The Sheikah Slate moves to the second screen: equip, eat, plan and travel without opening the
pause menu.

- **Ready**: a round map that follows Link, with location, time, temperature, weather, rupees
  and the active quest; equipped weapon, bow, shield and arrows with durability; runes; touch
  grids and hold-to-pick for gear and runes; shrine-compass and low-hearts food cards.
- **Inventory**: every category in the game's own order and sort, item details, equip, remove,
  eat, swipe to switch weapons, hold materials or food to cook. A broken weapon is replaced by
  the previous secondary weapon.
- **Map**: pan-and-zoom Hyrule with the game's own icons and place names; pins and stamps saved
  to the game; layers; travel to any activated shrine or tower (and the Travel Medallion with
  the DLC).
- **Quests, Trackers and Recipes**, plus title, loading and pause views.
- Any other game version shows a notice and reads nothing.

The package ships no game art. Icons, the map, the font and all text are read from the player's
own game files at runtime. The PNGs in `dualscreen/assets/` are drawn by code (frames, buttons,
rings and markers); `dualscreen/ASSETS.provenance.json` declares them.

## Layout

| Path | What it is |
|---|---|
| `dualscreen/manifest.json` | Pages, widgets, actions and bindings. |
| `dualscreen/8E9978D50BDD20B4.json` | Per-build data file (empty object; the module reads everything live). |
| `dualscreen/load-plans/<build id>.json` | Guest load plan: the helper code plus two patched call sites, gated to each exact build. |
| `dualscreen/assets/` | Code-drawn UI images. |
| `native/modules/01007EF00011E000.cpp`, `botw_*.h` | The native module (one translation unit) and its readers, pages and action bridge. |
| `native/modules/botw_*_test.cpp` | Host unit tests. |
| `native/helper/botw/` | Guest helper (AArch64 assembly) that the load plan installs. |
| `tools/botw/` | Rebuild scripts for the load plan and the release modules. |

This README sits outside `dualscreen/` on purpose: the packager archives `dualscreen/` as is,
and excludes source documentation from the portable archive.

## Rebuilding

Release modules, with the 1.0.0 flags (stripped hashes are listed in the script):

```sh
EDEN_SOURCE_ROOT=/path/to/eden-duo ANDROID_NDK=/path/to/ndk/28.2.13676358 \
    tools/botw/build_modules.sh build-botw
```

Or through CMake with the other modules (`native/README.md`): target `dsmod-breath-of-the-wild`,
and with `-DBUILD_TESTING=ON` one `dsmod-botw-*` test per `botw_*_test.cpp`.

Load plan (needs clang and the LLVM binutils):

```sh
tools/botw/build_helper_plan.py \
    --output packages/BreathOfTheWild/dualscreen/load-plans/8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000.json
```

Package:

```sh
tools/build_dualscreen_package.py --package packages/BreathOfTheWild --output dist \
    --version 1.0.0 --name "Breath of the Wild companion" --min-runtime 22 \
    --module android-arm64-v8a=build-botw/android-arm64-v8a/01007EF00011E000.so \
    --module linux-x86_64=build-botw/linux-x86_64/01007EF00011E000.so \
    --build-id 8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000
tools/compact_zip.py dist/01007EF00011E000.dsmod.zip
```

With the byte-identical modules this reproduces the published
`01007EF00011E000-BreathOfTheWild-1.0.0.dsmod.zip`
(sha256 `a6d9ad51a1249662dbde616a488eed8e3cb43ee65d6a1b7f019ea19d28118121`).

The manifest and the code-drawn assets are committed as package source. The scripts that first
generated them worked on local research data and game files, so they are not part of this
repository.

For the 1.9.0 load plan, pass `--build-id CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000`
to `tools/botw/build_helper_plan.py`. The default remains the byte-identical 1.6.0 plan.

After packaging with `tools/build_dualscreen_package.py`, run `tools/compact_zip.py` on the
archive so its manifest stays below the 4 MiB installer limit.
