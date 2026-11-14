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

Release modules (stripped hashes are listed in the script):

```sh
EDEN_SOURCE_ROOT=/path/to/eden-duo ANDROID_NDK=/path/to/ndk/28.2.13676358 \
    tools/botw/build_modules.sh build-botw
```

Or through CMake with the other modules (`native/README.md`): target `dsmod-breath-of-the-wild`,
and with `-DBUILD_TESTING=ON` one `dsmod-botw-*` test per `botw_*_test.cpp`.

Load plans, one per supported game build (needs clang and the LLVM binutils):

```sh
tools/botw/build_helper_plan.py \
    --output packages/BreathOfTheWild/dualscreen/load-plans/8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000.json
tools/botw/build_helper_plan.py --build-id CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000 \
    --output packages/BreathOfTheWild/dualscreen/load-plans/CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000.json
```

Package:

```sh
tools/build_dualscreen_package.py --package packages/BreathOfTheWild --output dist \
    --version 1.1.0 --min-runtime 22 \
    --module linux-x86_64=build-botw/linux-x86_64/01007EF00011E000.so \
    --module android-arm64-v8a=build-botw/android-arm64-v8a/01007EF00011E000.so \
    --build-id 8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000 \
    --build-id CD57B23FA4BBAD65803D9788C01821EE00000000000000000000000000000000
tools/compact_zip.py dist/01007EF00011E000.dsmod.zip
```

With the byte-identical modules this reproduces the published
`01007EF00011E000-BreathOfTheWild-1.1.0.dsmod.zip`
(sha256 `d15996f09ea850fe406d94545d530c61b6bc66680b36efe96578d13367ff998b`).

The manifest and the code-drawn assets are committed as package source. The scripts that first
generated them worked on local research data and game files, so they are not part of this
repository.

After packaging with `tools/build_dualscreen_package.py`, run `tools/compact_zip.py` on the
archive so its manifest stays below the 4 MiB installer limit.
