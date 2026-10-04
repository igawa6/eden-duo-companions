# Metroid Dread asset-free tooling

The Dread package ships no game-derived data. The module (`native/modules/dread_mapgen.*` in
this repository) builds the map geometry and the map-areas object from the player's romfs at game
load.

- `dread_af_manifest.py`: converts an old asset-carrying manifest (pre-1.0.0) into the asset-free
  one. `bind_loc_labels()` is also used by `../dread_hud_page.py`.
- `check_generated.py`: compares module-generated data with a reference (byte-identical .geo files,
  exact doubles).
- `python_reference/gen_ref.py`: Python reference generator, built on mercury-engine-data-structures
  and the bakers in `tools/dread/`. The C++ module port must match it. `python_reference/loc.py`
  writes `us_english.json` (the game's localization table) for `dread_af_manifest.py`.
  `chk_*.py` check single fields against an old package in `before/`.
- `gen_dread_rfl_types.py`, `dread_rfl_dump.py`: generate the module's level-data type table
  (`dread_rfl_types.inc` in this repository).
- `cmp_live.py`, `cmp_live_masked.py`: compare live captures, masking the pulsing Samus marker.

Set `DREAD_ROMFS` to your extracted romfs, the directory that contains `packs/`, `system/` and
`textures/`.
