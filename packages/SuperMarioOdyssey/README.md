# Super Mario Odyssey companion

Title `0100000000010000`, game version 1.4.1 (build `B424BE150A8E7D78701CBE7A439D9EBF…`).
Companion 1.0.0 for Eden Duo 1.3.2 (runtime 22) or newer.

A travel-brochure Map/Tasks/Stats panel for the second screen.

- **Map**: native maps for all 17 kingdoms, including 22 scenario variants; live Mario position
  and facing arrow, an independent camera cone, pan/zoom, touched checkpoints, objectives, shops
  and minigame markers.
- Tap a touched checkpoint and Warp Here to start the game's native checkpoint travel directly.
  The native map menu stays closed when travelling from gameplay. Travel requires a supported
  home stage, native unlock state and an active second screen.
- Live coins, regional wallet, health, deposited moon total and the native unbanked moon row.
- **Tasks**: native Toadette task names/hints, availability, claimed state, scrolling and
  selection. All 61 tasks have native progress/readiness rules. Story scenarios, exact moon
  groups, regional shopping, quizzes and registered souvenirs follow the game's own predicates.
- **Stats**: visited-kingdom moon list and regional collection tallies, capture count and a
  horizontal strip of all discovered captures.
- Native fonts, tilted counter slabs, life ring, brochure legend and title/loading/pause states.
- Any other game build shows a notice and reads nothing.

The module hides the native coin, health, moon and compass panes and menu/map hints. They
restore when paused, the companion is hidden or the module is disabled. Contextual prompts remain
on the game screen. Visibility changes and travel requests use expected-value transactions; no
game code is patched.

The package ships no bundled images, fonts or extracted game data at all: every icon, map,
pictogram and glyph is decoded from the player's own game files at runtime (BFFNT/BNTX/SARC/Yaz0
decoders built into the module), and kingdom/task/capture tables are built from the player's own
romfs. `dualscreen/font/smo_font_spec.txt` is an 11-byte request marker, not font data.

Kingdom moon tallies count list entries, including Toadette's 61 Mushroom rewards. The global
hero tally follows the native deposited total, capped at 999; the moon row follows acquired units
not yet deposited. The regional HUD shows the live unspent wallet; Stats shows regional coins
collected.

English labels are used. Four regional-coin shapes are verified; other kingdoms use native
generic purple art. Android/NCE boots and loads the package; an Android save-account mismatch
that affected save-backed Tasks/Stats progress is fixed by the Eden Duo 1.3.2.2 host runtime.

## Layout

| Path | What it is |
|---|---|
| `dualscreen/manifest.json` | Pages, widgets, actions and bindings. |
| `dualscreen/font/smo_font_spec.txt` | Font request marker (no glyph data); the module decodes the six real native BFFNT fonts from the player's own romfs and merges them into one atlas. |
| `native/modules/0100000000010000.cpp`, `0100000000010000.h`, `smo_*.{h,cpp}` | The native module (one translation unit) and its readers and pages. |
| `native/modules/smo_*_test.cpp`, `0100000000010000_test.cpp` | Host unit tests. |

This README sits outside `dualscreen/` on purpose: the packager archives `dualscreen/` as is,
and keeping the README out leaves the archive identical to the published 1.0.0 zip.

## Rebuilding

Native module and tests, through CMake with the other companions (`native/README.md`): target
`dsmod-smo`, and with `-DBUILD_TESTING=ON` one `dsmod-smo-*` test per `smo_*_test.cpp`.

```sh
cmake -S native/modules -B build-native-linux -G Ninja \
    -DEDEN_SOURCE_ROOT=/path/to/eden-duo -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-native-linux --target dsmod-smo

cmake -S native/modules -B build-native-android -G Ninja \
    -DEDEN_SOURCE_ROOT=/path/to/eden-duo \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/ndk/28.2.13676358/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-native-android --target dsmod-smo
```

Strip both outputs (`llvm-strip --strip-all`) before packaging. Package:

```sh
tools/build_dualscreen_package.py --package packages/SuperMarioOdyssey --output dist \
    --version 1.0.0 --name "Super Mario Odyssey companion" --min-runtime 22 \
    --module linux-x86_64=out/smo-release/linux-x86_64/0100000000010000.so \
    --module android-arm64-v8a=out/smo-release/android-arm64-v8a/0100000000010000.so \
    --build-id B424BE150A8E7D78701CBE7A439D9EBF00000000000000000000000000000000
tools/compact_zip.py dist/0100000000010000.dsmod.zip
```

With the same stripped modules this reproduces the published
`0100000000010000-SuperMarioOdyssey-1.0.0.dsmod.zip`
(sha256 `b5571382663f0b97ac78f0cc7d959f0d78f062dc00b4e288e71bf141960a93fa`). The Linux module is
byte-for-byte reproducible from a clean build; the Android module differs only in its linker's
self-referential 20-byte `.note.gnu.build-id` value across separate clean builds (verified by a
byte-level diff against a second clean rebuild) -- every other byte, including all code and
data, is identical.

The manifest is committed as package source. The scripts that first generated the native
reference decoders this module ports (BYML/save, MSBT, BFFNT/BNTX) worked on local research data
and game files, so they are not part of this repository.
