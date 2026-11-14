# Resident Evil 4 companion

Title `010099A00BC1E000`, game 1.0.2 / update v131072
(build `82C2A04A21D3E0B850F273D4149C9003…`).
Companion 1.0.0 for Eden Duo 1.4.0 (runtime 25) or newer; any other build shows a notice and reads nothing.

The bottom screen keeps the Field map, weapons and inventory within reach while you play.

- **Field map**: pan and zoom through village, castle, island and Separate Ways rooms in their
  native placement, with the live player marker and facing. The legend follows the native map:
  doors Open / Locked / Have key, rooms Entered / Not entered / Sealed, destination and typewriters.
  Unmapped rooms show an availability message.
- **Treasures and blue medallions**: markers use native treasure-map ownership and progress gates
  by default, including Separate Ways treasure maps. Settings includes the treasure and medallion
  spoiler toggle. Separate Ways keeps the native per-area treasure selection.
- **Four weapon slots**: hold a slot to pick an owned weapon or clear it, drag between slots to
  swap assignments, and tap to equip through the native weapon refresh helper. Equipping is
  guarded by the current game state and live ownership. Missing weapons show “Not in case”.
- **Ammo**: loaded ammo mirrors the native HUD for proven weapons, with reserve counts where
  supported. Unproven weapon variants show unavailable rather than a guessed count.
- **Case**: the live attaché case capacity, item positions and rotations, native item art and
  selected item names. **Key Items** shows owned keys and treasures with category paging and
  native names.
- **Ashley**: a Wait / Follow bubble appears when the native escort and command rules allow it.
- **Settings**: the moved top-screen health and ammo HUD elements are hidden by default, with
  settings to show them again. Shooting-range sessions use a dimmed view and refuse slot equip.
  Mercenaries shows unavailable.

The package ships no game art. Map, item art, icons, the font and all text are read from the
player's own game files at runtime. The PNGs in `dualscreen/assets` are drawn by code;
`dualscreen/ASSETS.provenance.json` declares them.

## Layout

| Path | What it is |
|---|---|
| `dualscreen/manifest.json` | Generated pages, widgets, actions, state and spoiler bindings. |
| `dualscreen/ASSETS.provenance.json` | Code-drawn file declarations and runtime asset sources. |
| `dualscreen/assets/` | Code-drawn highlights, badges and transparent map placeholders. |
| `dualscreen/guest/<build id>/load-plan.json` | Build-pinned HUD and equip guest helpers and patched call sites. |
| `native/modules/010099A00BC1E000.cpp`, `re4_*.h` / `.cpp` | Readers, actions, native file decoders, runtime art and font generation. |
| `native/modules/re4*_test.cpp`, `010099A00BC1E000_test.cpp` | Host tests and offline native-file comparisons. |
| `native/guest/re4_hud_tick.c`, `re4_equip_tick.c` | Guest helper implementations. |
| `tools/resident-evil-4/` | Manifest, metadata and asset generators, provenance checks and golden tests. |

This README sits outside `dualscreen/`. The packager copies `dualscreen/` and generates the
archive's root `package.json`; it does not include this README or use the source metadata's version.

## Rebuilding

Use your own extracted game files and the local map decoder and approved component renderer.
The generators need Pillow and NumPy. Start with an empty asset directory in a separate staging
copy so that old extracted images cannot enter the package. Run the full chain in order:

```sh
python3 tools/resident-evil-4/prepare_assets.py <local-inputs>
python3 tools/resident-evil-4/prepare_map.py <local-inputs> <bio4>/ss/cmn/map_obj1.dat
python3 tools/resident-evil-4/prepare_weapon_art.py <local-inputs> <bio4>
python3 tools/resident-evil-4/prepare_weapon_art.py <local-inputs> <bio4> --case
python3 tools/resident-evil-4/prepare_item_names.py <bio4>/etc/core.udas --output <test-item-names-header>
python3 tools/resident-evil-4/prepare_r5.py <local-inputs>
```

Item-name headers may only be generated in external test scratch. The name golden test
compares all runtime names against an independent Python decode of `RE4_TEST_ROMFS` and
skips when that fixture is absent. Production names are decoded at runtime.
`repo_hygiene_test.py` forbids tracked name tables and requires fewer than 20 distinct
native names of at least nine characters across tracked RE4 files (fixture check); single
references in comments are allowed. Collision meshes are decoded per stage on the asset worker and atomically published.
Until the stage is ready, the map pose and its dependent room/floor bindings stay unavailable.
The title preloads village geometry; stage transitions request a fresh worker key before map
art. Retained meshes are capped at 1 MiB per stage (one previous stage may briefly be held
by a sample); input map containers are capped at 2 MiB. No collision header is generated.
`clean_build_test.py --sdk <eden-duo-source>` builds a committed `git archive` without
generators or RomFS. The geometry golden requires the private fixture and skips with exit 77 when it is
absent. Unit tests skip only fixture-dependent map groups; other regressions still run.
Failed mesh reads retry with a new worker key and a one-second backoff.
Failed item-name reads retry on the next native stage change; successfully loaded names
remain cached. State 21 is retired; later state numbers retain their existing values.
The 33 transparent 1x1 map PNGs remain required base-image sources referenced by
`map.areas.*.image`; room outlines are separate runtime overlays. They contain no native
pixels. Removing them requires replacing those base-image references at the same time.
Numeric room bounds, state tables, item shapes and composition recipes are generated
metadata; native pixels and glyph metrics are decoded from the player's files. Generate twice
and compare file hashes, including the manifest, provenance and generated headers. Never edit
the manifest by hand.

Build against the Eden Duo SDK using `-DEDEN_SOURCE_ROOT=<eden-duo-source>` as described in
`native/README.md`. Build target `dsmod-resident-evil-4` for Linux and Android arm64 in Release
mode. Enable `BUILD_TESTING` and build the RE4 test targets before running:

```sh
ctest --test-dir <linux-build> -R 'dsmod-(resident-evil-4|re4-)' --output-on-failure
python3 tools/resident-evil-4/package_test.py
python3 -m unittest discover -s tools/resident-evil-4 -p '*_test.py'
```

Native-file golden tests need `RE4_TEST_ROMFS`, `RE4_TEST_GOLDENS` and
`RE4_TEST_PICKER_GOLDEN` pointing to your own matching inputs and offline references.
These checks do not replace live rendering or device verification.

`tools/resident-evil-4/gen_hud_helper.py --elf <main.elf> --nso <main> --output <load-plan.json>`
regenerates the guest load plan with clang and LLVM tools. It refuses mismatched executable
hashes, build IDs and hook instructions. Package with `tools/build_dualscreen_package.py`,
passing `--version 1.0.0 --min-runtime 25`, both stripped module libraries and the full supported
build ID. Release versioning is supplied by the packager.

## Credits

- Companion and Eden Duo by [igawa6](https://github.com/igawa6), with contributors recorded in the repository history.
- [adonis-singh/re4](https://github.com/adonis-singh/re4), the public decompilation, used as a reference for native behavior and data structures.
- Resident Evil 4 and its game art belong to Capcom. This fan companion is not affiliated with or endorsed by Capcom.

Companion code: GPL-3.0-or-later. Existing third-party license notices remain with their sources.
