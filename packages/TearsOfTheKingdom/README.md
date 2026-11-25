# The Legend of Zelda: Tears of the Kingdom companion

Title `0100F2C0115B6000`, game version 1.4.3 (update v720896), main NSO build id
`277178b7dba1b6d4949a84778d4abc58` (package gate: first 8 bytes `277178B7DBA1B6D4`).
Companion 1.2.0, runtime 22 or newer. Any other build: the module does not load, nothing is read.

Read-only companion in the game's own look, four pages (1240x1080):

* **Estado / Status**: place (Cielo / Superficie / Subsuelo) and position, rupees and poes, hearts as heart icons in
  quarters (bonus hearts yellow, gloom hearts purple) with the number, stamina wheels, battery, equipped weapon, bow,
  shield and armor with icons and in-game names, the active ability, and the five sages (portraits, cooldown).
* **Mapa / Map**: the game's own Sky / Surface / Depths map art, follows your layer or pick one, Link's marker with heading,
  drag and pinch, "Centrar" returns to Link.
* **Objetos / Items**: the eight pouch tabs (bows tab includes arrows), 4x3 icon grid, page button, detail panel with the
  game's name and description. Read-only: tapping selects, it never changes the game.
* **Info**: diagnostics (build, manager, tables, actor, raw rupee / stamina / battery text).

Art pack (optional, per user, never part of the package or the repository): fonts, UI sprites, item icons, localized
item names and descriptions and the map are made from YOUR game files by `tools/totk/make_art.py`:

```sh
python3.14 tools/totk/make_art.py --nup "<game>.nup" --nup-py <peanuts-native>/dm/tools/nup.py --out <dir outside git> --lang USes
tools/totk/push_art.sh <dir>            # -> <game folder>/peanuts/TearsOfTheKingdom/dualscreen/art (reinstalling the companion removes it)
```

`--lang` picks the in-game language (USes, EUes, USen, ...). Needs Python 3.14, Pillow and texture2ddecoder; takes about
15 seconds and ~35 MB. Without the art pack the companion still runs (built-in font, no pictures). The art tool ports
TotK Native's SARC/BYML/MSBT/BNTX readers, UI texture table and map tile selection (MIT, `dualscreen/NOTICE-TotKNative.txt`).
How pictures are bound: the module publishes icon *indexes* (`art/text/icons.tsv`, `art/icons/<n>.png`), so the manifest
contains no game names.

Origin: the reader is ported from TotK Native (https://github.com/AurelioB/totk-android, MIT, Copyright (c) 2026
MacNativeGames contributors; license in `dualscreen/NOTICE-TotKNative.txt`): `games/totk/runtime/companion_snapshot.c`
(`TYPES[]` table offsets, `value()/lookup()`, `player()`, `player_actor()`, `pouch()`, `CATEGORIES[]`, `sages()`) and
`companion.h`. Their addresses are `main + offset` with `main` the NSO load base, the same base the DSMod runtime
reports as `main_base`, so offsets transfer unchanged.

Verification status: the singleton slot, hash index and Int/Float/Bool layouts were found independently by static
analysis here and agree with TotK Native; the hearts hashes read from Player.pack equal MurmurHash3 of
"PlayerStatus.Life/MaxLife/ExtraLife/BreakLife"; all ~25 flag names used exist in `GameDataList.Product.140` with the
expected types. The module is unit tested on a synthetic layout. It has **not run against the live game**.
Not used from TotK Native: its code-word SHA-256 guard (Peanuts rewrites code) and all item actions.

## How the values are found

All offsets are relative to the start of the main NSO image (text at 0). The analysis tools are not
in this repository (they read game files).

1. **GameData manager.** The string `gmd::GameDataMgr` (rodata 0x2BEF9D2) is the heap name used by
   the manager initializer (code 0x7878A8), which stores the heap in `mgr+0xCE0`. The singleton
   pointer is at `main+0x3AD66B0` (.bss); code reaches it through the GOT slot at `main+0x395DFF0`,
   which holds `0x3AD66B0`.
2. **Type blocks.** The initializer (0x788500..0x788D00) builds one block per value type, 0x60 bytes
   apart from `mgr+8`, each filled by a loader that names its type: Bool +0x08, BoolArray +0x68,
   Int +0xC8, IntArray +0x128, Float +0x188, FloatArray +0x1E8, Enum +0x248, ..., Vector3 +0x3C8.
3. **Lookup.** The game's getters (Bool 0x45620, Int 0x166E0, Float 0x5B92D0, Vector3 0x5F4CF0, generic
   0x3CFFE8) show the algorithm. A block's field "X" is the block offset + 0x18. Hash index at
   `*(mgr+X)`: u64 slots `{u32 hash, u32 record index}`, capacity `u32 mgr+X+0xC`, start slot
   `hash % capacity`, linear probe with wrap, hash 0 = empty. Records: count `u32 mgr+X-0x10`, array
   `*(mgr+X-0x8)`. Int X=0xE0, 0x28-byte records, s32 value at +0x1C. Float X=0x1A0, 0x28 bytes, f32 at
   +0x1C. Bool X=0x20, 0x18 bytes, u8 at +0x16. Vector3 X=0x3E0, 0x40 bytes, 3 x f32 at +0x18.
4. **Flag hashes** are MurmurHash3 (seed 0) of the dotted flag name (names from TotK Native; my CRC32 guess was wrong). `GameData/GameDataList.Product.140.byml.zs` (zstd with
   the `zs.zsdic` dictionary from `Pack/ZsDic.pack.zs`, big-endian BYML) lists hashes, types and
   defaults but no names. Hearts: `Pack/Actor/Player.pack.zs` ->
   `Life/LifeGameDataSyncParam/Player.game__life__LifeGameDataSyncParam.bgyml` holds
   `LifeGameDataHash 0x31AB5580`, `MaxLifeGameDataHash 0xFBE01DA1`, `ExtraLifeGameDataHash 0x8703CB91`,
   `BreakLifeGameDataHash 0xD6C8BCA9`: Int flags with default 12 (3 hearts), so the unit is quarter
   hearts. The page shows `life / 4`.
5. **Sanity gates in the module** (`native/modules/totk_gamedata.h`, `0100F2C0115B6000.cpp`): build id,
   singleton pointer non-null and 8-byte aligned, Int record count in 5000..8000 (the byml has 5675),
   life/max plausibility (max 1..60 hearts). A failed gate publishes nothing.

## Remaining work

* **Map coordinates:** x east, z south, map y = -z, world -6000..6000 x -5000..5000 (TotK Native's bounds).
* **Current stamina**, item names (msbt), pins/stamps (`MapData.IconData.*` are readable, not shown), item actions.
* **Stamina** shows capacity only (as in TotK Native's snapshot).

## Layout

| Path | What it is |
|---|---|
| `dualscreen/manifest.json` | Generated by `tools/totk/gen_manifest.py`: two pages (Status, Info). |
| `dualscreen/NOTICE-TotKNative.txt` | MIT notice for the ported parts. |
| `dualscreen/277178B7DBA1B6D4.json` | Per-build data file (empty; the module reads live). |
| `native/modules/totk_gamedata.h`, `0100F2C0115B6000.cpp`, `0100F2C0115B6000_test.cpp` | Reader, module, synthetic test. |
| `tools/totk/build_module.sh` | Builds the linux-x86_64 and android-arm64-v8a modules. |

Build and pack:

```sh
EDEN_SOURCE_ROOT=<dir whose src/core/mods has dsmod_module_abi.h> ANDROID_NDK=<ndk r28c> \
    tools/totk/build_module.sh build-totk
python3 tools/totk/gen_manifest.py
python3 tools/build_dualscreen_package.py --package packages/TearsOfTheKingdom --output dist \
    --version 1.2.0 --name "Tears of the Kingdom companion" --min-runtime 22 --build-id 277178B7DBA1B6D4 \
    --module linux-x86_64=build-totk/linux-x86_64/0100F2C0115B6000.so \
    --module android-arm64-v8a=build-totk/android-arm64-v8a/0100F2C0115B6000.so
mv dist/0100F2C0115B6000.dsmod.zip dist/0100F2C0115B6000-TearsOfTheKingdom-1.2.0.dsmod.zip
```
