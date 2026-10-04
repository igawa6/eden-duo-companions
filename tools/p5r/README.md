# Persona 5 Royal page generators

These scripts generate `packages/Persona5Royal/` (`dualscreen/manifest.json` and
`dualscreen/p5r_art.rec`) from the base manifest and the snippets in this directory.

The package is **asset-free**. `p5r_art.rec` is a table of small recipes: "cut sprite N of this SPD
sheet, scale it, tint it, draw this text in the game font". The P5R module in this repository
(`p5r_recipes.h`, `p5r_romfs_assets.cpp`) replays those recipes against the player's own romfs at
runtime. The generators evaluate every recipe with Pillow while they build, so they read the same
game files the module will read. That is why a rebuild needs your own unpacked copy of the game.

## Pipeline

`build_pkg_af.sh <out dir>` runs every step:

1. `merge_base_v08.py`: `base_v07_manifest.json` plus `snippets/*.json` become the working base
   manifest (outputs, actions, flags, enforce rules).
2. `build_gameart.py`: the in-game-style pages (waiting, party, talk, field, analysis, social).
   It imports `p5style.py`, `recipe.py`, `battle_page.py` (BATTLE) and `waiting.py`.
3. `map_merge_af.py`: merges the area-map snippet made by `map_gen_af.py`.
4. `build_menu.py`: the START MENU replica (SKILL, ITEM, EQUIP, PERSONA, STATS, CONFIDANT, REQUEST,
   CALENDAR) and the MUSIC page. It uses `menu_art.py` and `music_art.py`.
5. `prune_derived.py`: drops derived values that nothing reads.
   `preload_art.py`: the pages that open the START MENU carry its art as invisible 1x1 widgets,
   and the hub carries the art of the eight menu pages, so the host has built those images before
   the page opens. Without them the first open after loading a save faded in with missing tiles
   and money digits. It replays recipes to keep one page's art under the host's image budget.
6. Strips the modules and copies them in. `sync_v08.py` pins their sha256 and the version.
   `set_min_runtime.py` sets `min_runtime`.
7. `check_menu.py`: static checks. Every `module:p5r:<id>` has a recipe and every tap names an
   action.

Environment for `build_pkg_af.sh`: `LINUX_SO`, `ANDROID_SO` (P5R module builds; required),
`PKG_VERSION` (default 1.0.0), `STRIP` (default `llvm-strip`).

## Inputs

All inputs are set in `p5r_paths.py`, each through an environment variable. An unset variable
defaults to a name under `P5R_WORK` (default `./p5r-work`):

| Variable | Contents | How to get it |
|----------|----------|---------------|
| `P5R_ROMFS` | Unpacked romfs, the `ALL_USEU` tree. A sibling `PATCH1/` overrides it, as the update does. | Unpack the game's CPKs. |
| `P5R_FONT_DIR` | `EN_FONT_FONT0_atlas.png` and `EN_FONT_FONT0_metrics.json` | `fnt_decode.py <romfs>/EN/FONT/FONT0.FNT <dir>/EN_FONT_FONT0` |
| `P5R_CMM_FORMAT` | `cmmFormat.ctd`, the confidant table | Extract it from the game's init data. |
| `P5R_TRACKS` | Thieves Den track list, JSON rows `{i, cue, f1, f2, name}` | Build it from `EN/INIT/MYPTABLE.BIN`. |
| `P5R_MAPGEN` | `map_gen_af.py <dir> module` output (`snippet.json`, `map_recipes.json`) | See below. |

`map_gen_af.py` itself also reads `P5R_ROADMAP_EXPORT` (the field road-map export: `maps.json`
plus composed PNGs), `P5R_MAP_AREAS` (the v0.6 area bounds), `P5R_MINIMAP_SPRITES` (the
`spd_extract.py` output of `EN/FIELD/PANEL/P5MINIMAP_01.SPD`) and `P5R_MAIN_IMG` (the decompressed
main executable of build D4B1..., for its icon tables). The road-map exporter and the extractors
for `cmmFormat.ctd` and the track list are research tooling and are not part of this repository.
Everything they produce is game data, so none of it is committed here.

`spd_extract.py`, `fnt_decode.py` and `trim_fnt.py` are the small format helpers the steps above
refer to. `trim_fnt.py` is only needed for the older PNG mode (`ASSET_MODE=png`).

## Reproducibility

With the inputs above and the release modules, `build_pkg_af.sh` reproduces the committed
`manifest.json`, `p5r_art.rec` and `package.json` byte for byte. `map_gen_af.py` reproduces the map
snippet that the package was built from.
