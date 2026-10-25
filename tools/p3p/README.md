# Persona 3 Portable tools

Scripts for the read-only Persona 3 Portable companion (`0100DCD01525A000`). The module is
`native/modules/0100DCD01525A000.cpp` with `p3p_reader.h`; it supports only the base game build
`663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000` and refuses any other.

The package is asset-free. Names and skill data are read at runtime from the player's own game
(the loaded tables in memory and `data_EN/umd0.cpk`), never shipped. Text is drawn in the game's
own font: `p3p_font.h` reads `font/font0.fnt` from `init.bin` in `data_EN/umd0.cpk` and decodes it
with the shared Atlus FNT decoder (`p5r_font.h`); `p3p_font.txt` in the package is only a marker.

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
  -DEDEN_SOURCE_ROOT="/absolute/path/to/eden-duo" -DEDEN_P3P_STANDALONE=ON
ninja -C build-mods dsmod-p3p p3p-module-test
build-mods/p3p-module-test
```

`EDEN_P3P_STANDALONE` configures only the P3P targets. Package the stripped module with
`tools/build_dualscreen_package.py --package packages/Persona3Portable` as for the other games.
