# Digimon Story Time Stranger: Time Stranger Field Guide

Generator for the Time Stranger Field Guide package (`packages/TimeStrangerFieldGuide`, game update
1.2.2, build `135C83CDEA06AEC2`). Native module: `native/modules/010062E01FE0C000.cpp` (CMake target
`dsmod-dsts`).

Nothing from the game is kept here: no pictures, no text and no game data.

- **Pictures and the font** are read by the module from the player's own `app_0.nx64.mvgl` and
  `app_text01.nx64.mvgl` at run time.
- **Game text** (names, mission objectives, descriptions) is stored only as references: 64-bit FNV-1a hashes of the
  game's strings. The module finds the strings in the player's text archive and fills them in
  (`public_text.py`).
- **Game data** (item prices and descriptions, chests, wild Digimon, side-quest rewards and the "wild in"
  lists) is read by the module from the player's own game tables (`010062E01FE0C000_data.inc`). The
  package carries identifiers only: which item ids sit in which item group, which map and field ids
  make up each area, which quest id each side-quest page shows.

The guide's own notes are in `guide_data.tok.json`: unlock conditions, boss advice, tips, the type
chart and how to get each Digimon. Every piece of game text inside them is a token, `⟦<hash><case>⟧`,
never the text itself.

## Build

The build needs Python 3 with Pillow and NumPy, plus your own copy of the game for its text.

```sh
# 1. The game's English text tables, from your own game (the romfs file app_text01.nx64.mvgl)
python3 dump_text.py <romfs>/app_text01.nx64.mvgl -o text122.json

# 2. The package folder (this is packages/TimeStrangerFieldGuide)
DSTS_TEXT_JSON=text122.json python3 build_dsts_package.py 010062E01FE0C000 \
  --package-dir ../../packages/TimeStrangerFieldGuide
```

The text dump is needed at build time only. The build fills the tokens in to lay the pages out, then
takes the game text out of the package again. With the same text dump, the output is byte-for-byte
the same every time.

The module builds with the repository's CMake (see `native/README.md`), target `dsmod-dsts`. The
release script packs it into the archive (`tools/build_release.sh`, `GAMES=dsts`, `DSTS_LINUX_SO`,
`DSTS_ANDROID_SO`). For a quick local archive with the module inside:

```sh
DSTS_TEXT_JSON=text122.json DSTS_MODULE_SO=<android .so> DSTS_MODULE_SO_LINUX=<linux .so> \
  python3 build_dsts_package.py 010062E01FE0C000
```

## Tests

- `dsmod-dsts-test` (CTest `dsmod-dsts`) is a synthetic test. It needs no game files.
- `check_gamedata.py` checks the module's game data against an independent Python reading of your
  own game tables, for every page, item and Digimon:

  ```sh
  python3 check_gamedata.py ../../packages/TimeStrangerFieldGuide/dualscreen/manifest.json \
    <build>/dsmod-dsts-test <romfs>/app_0.nx64.mvgl <romfs>/app_text01.nx64.mvgl
  ```

## Versions

Only update 1.2.2 (build `135C83CDEA06AEC2`) is supported. On any other version the host refuses the
module, and every page shows "Needs game version 1.2.2" with the host's reason.

| File | What it is |
|---|---|
| `build_dsts_package.py` | Pages, live memory points, layout, drawn art and packaging (`--personal`: the author's private variant with baked pictures; it needs folders that are not in this repository) |
| `public_text.py` | Takes game text out of the manifest (references that the module fills in) |
| `guidedata.py`, `guide_data.tok.json` | The guide's own notes with game text as tokens, plus the identifier index |
| `guide2status.json` | Dex number to the game's Digimon id (identifiers) |
| `bgart.py` | Backgrounds, location bar and other art, drawn from code |
| `dump_text.py`, `expa.py` | Read the game's text tables (MDB1 archive, EXPA tables) from your own files |
| `check_gamedata.py` | Reference check of the module's game data layer |
| `rbpaths.py` | Tree-walk helper for the (disabled) Field Guide probe |
| `tokenize_data.py` | Author tool: re-tokenizes the working data files |
