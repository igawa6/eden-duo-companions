"""Where the P5R page generators find their inputs.

Nothing here ships in the package: these are the builder's own game files (and data derived from
them locally). Every path can be set by an environment variable; unset ones default to a fixed
name under P5R_WORK (default ./p5r-work). See tools/p5r/README.md for what each input is and how
to produce it.
"""
import os
from pathlib import Path


def _path(var: str, default: Path) -> Path:
    value = os.environ.get(var)
    return Path(value).expanduser() if value else default


WORK = _path('P5R_WORK', Path('p5r-work'))
# Unpacked romfs of the game (the ALL_USEU tree; a sibling PATCH1/ tree, if present, overrides it).
ROMFS = _path('P5R_ROMFS', WORK / 'romfs' / 'ALL_USEU')
# fnt_decode.py output for EN/FONT/FONT0.FNT (EN_FONT_FONT0_atlas.png + EN_FONT_FONT0_metrics.json).
FONT_DIR = _path('P5R_FONT_DIR', WORK / 'font')
# spd_extract.py output of EN/FIELD/PANEL/P5MINIMAP_01.SPD (NNNN_*.png sprites), for map_gen_af.py.
MINIMAP_SPRITES = _path('P5R_MINIMAP_SPRITES', FONT_DIR / 'minimap')
# cmmFormat.ctd (the confidant table) extracted from the game's init data.
CMM_FORMAT = _path('P5R_CMM_FORMAT', WORK / 'cmmFormat.ctd')
# Thieves Den track list: JSON rows built from EN/INIT/MYPTABLE.BIN (see music_art.py).
TRACKS = _path('P5R_TRACKS', WORK / 'thieves_den_tracks.json')
# Field road-map export (maps.json + per-map composed PNGs) and the pre-crop area bounds, for map_gen_af.py.
ROADMAP_EXPORT = _path('P5R_ROADMAP_EXPORT', WORK / 'roadmap_export')
MAP_AREAS = _path('P5R_MAP_AREAS', WORK / 'map_areas_060.json')
# The game's main executable image (build D4B1...), for the static tables map_gen_af.py reads.
MAIN_IMG = _path('P5R_MAIN_IMG', WORK / 'main-d4b1.img')
# map_gen_af.py output directory (snippet.json + map_recipes.json), consumed by map_merge_af.py.
MAPGEN = _path('P5R_MAPGEN', WORK / 'mapout_mod')
# Canonical package in this repository.
PACKAGE = Path(__file__).resolve().parents[2] / 'packages' / 'Persona5Royal'
