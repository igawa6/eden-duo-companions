# The Legend of Zelda: Tears of the Kingdom companion

Title `0100F2C0115B6000`, game version 1.4.3 (update v720896), main NSO build id
`277178b7dba1b6d4949a84778d4abc58` (package gate: first 8 bytes `277178B7DBA1B6D4`). Companion 1.0.0, runtime 22 or
newer. On any other game version the module does not load, nothing is read, and every page shows
"Requires game version 1.4.3".

A read-only companion in the game's own look. It reads the running game and never changes it.

* **Status**: the place (Sky, Surface or Depths) and Link's position, rupees and poes, hearts as heart icons in quarters
  (bonus hearts yellow, gloom hearts purple) with the number, stamina wheels, battery, the equipped weapon, bow, shield
  and armor with icons and in-game names, the active ability, and the five sages (portraits, cooldown).
* **Map**: the game's own Sky / Surface / Depths maps; the layer follows Link or you pick one (Auto, Sky, Surface,
  Depths); Link's marker with heading; drag and pinch; Center returns to Link.
* **Items**: the eight pouch tabs (the bows tab includes arrows), a 4x3 icon grid with durability or count, a page
  button, and a detail panel with the game's name and description. Tapping only selects.
* **Info**: diagnostics (build, manager, tables, actor, pouch, raw rupee / stamina / battery / hearts text).

## The art pack (yours, built from your own game)

The package ships no game assets. The font, UI sprites, item icons, item names and descriptions and the three maps are
read from an **art pack** that you build from your own game and put in the title's `user:` folder, so it survives
reinstalling the companion and works in Eden Duo and in Peanuts Native. Without it the companion still runs (built-in
font, no pictures, item ids instead of names).

1. Dump the RomFS of Tears of the Kingdom 1.4.3 with the update installed (Eden Duo: long-press the game, Dump RomFS).
   If you have the base RomFS and the update's RomFS separately, pass both, base first: later folders win.
2. Build the pack (Python 3.14, `pip install pillow texture2ddecoder`; about 15 seconds, ~35 MB):

   ```sh
   python3.14 tools/totk/make_art.py --romfs <RomFS folder> [--romfs <update RomFS folder>] --out <folder> [--lang USen]
   ```

   `--lang` picks the in-game language of the item text: `USen` (default), `EUen`, `USes`, `EUes`, `EUfr`, `EUde`,
   `EUit`, `JPja`, and so on (the page labels follow: English, or Spanish for `USes` / `EUes`). `--out` gets an `art/`
   folder; a Peanuts Native `.nup` also works: `--nup <game>.nup --nup-py <Peanuts Native dm/tools/nup.py>`.
3. Copy `<folder>/art` into the title's `user:` folder, `<user folder>/art/...`:

   | Host | User folder |
   |---|---|
   | Eden Duo, Android | `/sdcard/Android/data/<Eden Duo app id>/files/dualscreen/user/0100F2C0115B6000/` |
   | Eden Duo, Linux | `~/.local/share/eden/dualscreen/user/0100F2C0115B6000/` |
   | Eden Duo, Windows | `%APPDATA%\eden\dualscreen\user\0100F2C0115B6000\` |
   | Peanuts Native | `<app private files>/peanuts-rt/dualscreen/user/0100F2C0115B6000/` |

   Over adb: `tools/totk/push_art.sh eden <folder> <Eden Duo app id>` or
   `tools/totk/push_art.sh peanuts <folder> <Peanuts app id>` (Peanuts needs a debuggable build for `run-as`).
4. Restart the game. The font is read when the companion starts, so start the game after copying the pack.

How pictures are bound: the module publishes icon *indexes* (`art/text/icons.tsv`, `art/icons/<n>.png`), and the
manifest names pictures with printf patterns, so it contains no game names.

## How the values are found

All offsets are relative to the start of the main NSO image. Several were found independently here and agree with TotK
Native; the rest (the Vector3 value offset, the flag names, the actor walk, the pouch categories) are ported from it.

1. **GameData manager.** The string `gmd::GameDataMgr` (rodata 0x2BEF9D2) is the heap name used by the manager
   initializer (code 0x7878A8). The singleton pointer is at `main+0x3AD66B0` (.bss); code reaches it through the GOT
   slot at `main+0x395DFF0`.
2. **Type blocks.** The initializer builds one block per value type, 0x60 bytes apart from `mgr+8`: Bool +0x08,
   BoolArray +0x68, Int +0xC8, IntArray +0x128, Float +0x188, FloatArray +0x1E8, Enum +0x248, ..., Vector3 +0x3C8.
3. **Lookup.** The game's getters (Bool 0x45620, Int 0x166E0, Float 0x5B92D0) show the algorithm: a hash index of
   `{u32 hash, u32 record}` slots (open addressing, linear probing with wrap, hash 0 = empty), capacity at +0xC of the
   index pointer's block, records `count`/`array` just before it. Int records are 0x28 bytes with the s32 at +0x1C,
   Float the same, Bool 0x18 bytes with the byte at +0x16.
4. **Flag hashes** are MurmurHash3 (seed 0) of the dotted flag name (`PlayerStatus.Life`, `Pouch.Weapon.Content.Name`,
   `MapData.CurrentLayer`, ...). The hearts hashes in `Pack/Actor/Player.pack` (`LifeGameDataSyncParam`) equal the
   MurmurHash3 of `PlayerStatus.Life / MaxLife / ExtraLife / BreakLife`, which cross-checks both analyses. Life is in
   quarter hearts (default 12 = three hearts).
5. **Player and pouch.** The "Player" actor is found through the scene module at `main+0x3ADD238` (+0x1E8 scene,
   component 13, resident actors); position at actor +0x2B4, rotation at +0x2C0. The pouch partition is the u32 at
   +0x188 of the manager at `main+0x3A81678`.
6. **Gates** (`native/modules/totk_gamedata.h`, `0100F2C0115B6000.cpp`): the build id, a non-null aligned singleton, an
   Int record count of 5000..8000 (the game's list has 5675) and plausible life values. A failed gate publishes nothing.

## Limits

* Stamina and battery show their capacity (as in TotK Native), not the current charge.
* Read-only: no equipping, eating or taking items out (the DSMod ABI has no safe guest calls).
* Item names are the game's own in the language of the art pack.
* Map pins and stamps are readable but not shown yet.
* Tested on Peanuts Native on an AYN Thor; see the pull request for what has been confirmed on a device.

## Layout

| Path | What it is |
|---|---|
| `dualscreen/manifest.json` | Generated by `tools/totk/gen_manifest.py`: four pages, the unsupported-version overlay. |
| `dualscreen/277178B7DBA1B6D4.json` | Per-build data file (empty; the module reads live). |
| `dualscreen/NOTICE-TotKNative.txt` | MIT notice for the parts ported from TotK Native. |
| `native/modules/totk_gamedata.h`, `0100F2C0115B6000.cpp`, `0100F2C0115B6000_test.cpp` | Reader, module, synthetic test (target `dsmod-totk`). |
| `tools/totk/` | `make_art.py` (the art pack), `push_art.sh`, `build_module.sh`, `gen_manifest.py`. |

Build and pack:

```sh
EDEN_SOURCE_ROOT=<Eden Duo checkout> ANDROID_NDK=<ndk r28c> tools/totk/build_module.sh build-totk
python3 tools/totk/gen_manifest.py
GAMES=totk TOTK_LINUX_SO=build-totk/linux-x86_64/0100F2C0115B6000.so \
  TOTK_ANDROID_SO=build-totk/android-arm64-v8a/0100F2C0115B6000.so tools/build_release.sh dist
```

Screenshots for the main README (1240x1792, see `screenshots/breath-of-the-wild/`): with the art pack installed, take
one of the bottom screen on each of Status, Map and Items.
