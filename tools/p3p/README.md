# Persona 3 Portable companion tooling

The P3P source package lives under [`packages/Persona3Portable/`](../../packages/Persona3Portable/).
The redesigned layout is the proposed **v1.1.0 main P3P companion**, pending
upstream artwork, rights and device QA review.

## Layout and artwork generation

There is now exactly **one** editable source of truth for the layout:
[`layout_v1_1.json`](layout_v1_1.json), in ordinary, reviewable JSON.
`gen_manifest.py` copies and validates that source into the packaged
`dualscreen/manifest.json`. The old v1.0 page-building implementation has
been retired; regeneration cannot silently overwrite the redesign.

```sh
python3 tools/p3p/gen_manifest.py
python3 -m pip install Pillow==12.3.0
python3 tools/p3p/v1810/prepare_fonts_for_checks.py --output /tmp/p3p-inter
P3P_INTER_FONT_DIR=/tmp/p3p-inter python3 tools/p3p/gen_thor_assets.py
P3P_INTER_FONT_DIR=/tmp/p3p-inter python3 tools/p3p/make_readme_previews.py
```

The 62 custom UI PNGs are generated from the source scripts under
`tools/p3p/v1810`. Of these, 55 recreate graphics from the previously
tested community layout at exact PNG-file or exact decoded-pixel level.
The remaining seven are newly designed original procedural alternatives to
historic Start-screen backgrounds and five wordmark images. They are **not**
byte-identical to the older package and deliberately do not reproduce a
commercial game logo. No game artwork or font binaries are stored here.

The generator uses pinned official Inter 4.1 OTFs under the
[SIL Open Font License](https://openfontlicense.org/), and verifies their
hashes before local rendering.

## Native P3P reader

The C++ reader is located at `native/modules/0100DCD01525A000.cpp`,
with `p3p_reader.h` and helper headers. It supports the Persona 3 Portable
base game version 1.0.0, title ID `0100DCD01525A000`, build
`663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000`.

The module reads the player's game resources during play; the package does
not bundle decrypted files, protected textures, keys, fonts, saves or ROM
contents. The game font is read at runtime; `p3p_font.txt` is just a marker.
The reader uses game-menu button actions for item usage, equipment and
choices rather than writing to guest game memory directly.

To compile a local standalone native module against an Eden Duo source
checkout (with Ninja, CMake and required C++ headers installed):

```sh
cmake -S native/modules -B build-mods -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEDEN_SOURCE_ROOT="/absolute/path/to/eden-duo" -DEDEN_P3P_STANDALONE=ON \
  -DBUILD_TESTING=ON
cmake --build build-mods --parallel 4
ctest --test-dir build-mods -R '^dsmod-p3p$' --output-on-failure
```

Cross-compile for Android ARM64 with the Android NDK and the matching
platform; strip binary debug sections before release packaging.
See the [package README](../../packages/Persona3Portable/README.md)
for the exact `tools/build_dualscreen_package.py` packaging command.

## Other utility tools

- `gen_pins.py`: generate supported native code fingerprint pins using
  a decrypted game executable supplied privately by the user.
- `nso.py`: bounded NSO decompression/relocation helper.
- `cpk.py`, `assets.py`, `saves.py`: read private game resource structures
  locally; no extracted game data is ever committed.
- `gen_thor_assets.py`: generate/check all 62 custom UI PNG files.
- `make_readme_previews.py`: render **illustrative previews**, not a claim of
  screenshot evidence.

## Device QA

The original v1.8.10 redesign with its legacy images passed preliminary
testing on the AYN Thor with source-built Linux/Android native modules. The
v1.1.0 source package has **seven new, source-owned graphics**. Their
composition and every live game-state screen, especially Battle, need a
fresh hardware check before upstream merge.
