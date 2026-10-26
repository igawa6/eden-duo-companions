# Persona 3 Portable tools

Scripts for the Persona 3 Portable companion (`0100DCD01525A000`). The module is
`native/modules/0100DCD01525A000.cpp` with `p3p_reader.h`; it supports only the base game build
`663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000` and refuses any other.

## Compatibility and validation

- Game version: **1.0.0 (base game)**, title ID `0100DCD01525A000`, exact build above only.
- All companion features were manually tested on an **AYN Thor running Eden Duo 1.3.2.2**.
- The package declares **runtime 19** as its minimum. Compatibility with older Eden Duo
  versions has not been validated.
- The UI font and help text use **English game resources**. Some names follow the loaded
  language tables, but full multilingual support is not claimed. Dialogue text that cannot
  be decoded exactly is omitted.

## Runtime resources and interaction

The package is asset-free. Names and skill data are read at runtime from the player's own game
(the loaded tables in memory and `data_EN/umd0.cpk`), never shipped. Text is drawn in the game's
own font: `p3p_font.h` reads `font/font0.fnt` from `init.bin` in `data_EN/umd0.cpk` and decodes it
with the shared Atlus FNT decoder (`p5r_font.h`); `p3p_font.txt` in the package is only a marker.
The look follows the native camp menu: `p3p_art.h` decodes the game's TMX textures and SPR0 sprite
sheets from the same archives (the camp menu lettering and icons in `init_free.bin`, Persona artwork
and camp portraits in `data/umd0.cpk`) and serves them as `module:p3p:` images, resampled to the
size the manifest draws them at. As in the native menu, the backdrop (`module:p3p:bg/<route>`, drawn
by the module) and the accents are blue or pink to match the protagonist's route.
Help lines (items, equipment, skills) come from the game's `init/datMsg.pak` message tables in
`init.bin` (`p3p_help.h`), read once in the background.
While the game shows a choice list, the talk page opens by itself with the current line and the
options, following the game's cursor (`p3p_talk.h`); it reads the open message window and the
game's own loaded message file, and leaves out any text it cannot reproduce exactly. Tapping an
option moves the game's cursor to it and tapping it again chooses it: the manifest presses the
D-pad and A, as the player would, only while every option is spelled out. On the Items page's Use
tab, tapping an item and then a party member uses it through the game's own camp menu
(`p3p_drive.h`): the module reads the menu each sample and asks for one button press at a time,
which the manifest's `enforce` rules make, then backs out to where the player was. On the Equip
page, tapping a slot lists what the game's Equip screen offers that member (the game's own rule,
`EquipChoices`), and tapping one equips it the same way. On the Personas page, "Use this Persona"
switches the protagonist to the picked one through the game's PERSONA screen. The module itself
never writes to the game.

| Script | What it does |
|---|---|
| `gen_manifest.py` | Writes `packages/Persona3Portable/dualscreen/manifest.json`. Standard library only. |
| `gen_pins.py <main NSO>` | Rewrites `native/modules/p3p_pins.inc`, the FNV-1a fingerprints of the game code the module relies on. Needs your own decrypted `main` from the supported build. |
| `nso.py` | Decompresses and hash-checks an NSO (`--relocate` applies relative relocations). Needs system `liblz4.so.1`. |
| `saves.py` | Bounded decoder for the save container; holds the supported build ID used by the generators. |
| `cpk.py` | CRI CPK table-of-contents reader and CRILAYLA decompressor (`--entry`, `--inventory`). |
| `assets.py` | Bounded readers for the startup PAK layouts and fixed-width name tables. |

Game files, saves, keys and anything derived from them stay outside the repository.

## Building

```sh
python3 tools/p3p/gen_manifest.py
cmake -S native/modules -B build-mods -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEDEN_SOURCE_ROOT="/absolute/path/to/eden-duo" -DEDEN_P3P_STANDALONE=ON \
  -DBUILD_TESTING=ON
cmake --build build-mods --parallel 4
ctest --test-dir build-mods -R '^dsmod-p3p$' --output-on-failure
```

`EDEN_P3P_STANDALONE` configures only the P3P targets. Package the stripped module with
`tools/build_dualscreen_package.py --package packages/Persona3Portable` as for the other games.
With `BUILD_TESTING=ON`, the default build includes `p3p-module-test`, registered with CTest
as `dsmod-p3p` in both standalone and full configurations. With testing off, the executable
remains available as an explicit build target.
