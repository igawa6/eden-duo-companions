#!/usr/bin/env python3
"""Extract Dread minimap data per area from the .bmmap files, richer than the merged .geo:

  <area>.geo            base shape   (aNavmeshGeos + mapOccluderGeos)
  <area>.<layer>.geo    colour region layers (the layer name is retained as an explicit kind)
  <area>.features.json  every icon feature with its sIconId type + world pos + bounds
  <area>.grid.json      gridDef world bounds (== areas.json) for the visited-mask transform

.geo binary format (matches the renderer's rasteriser):
  u32 vertex_count, u32 index_count,
  u16[vertex_count*2] xy normalised 0..65535 over the grid bbox,
  u32[index_count] triangle indices.

Run: venv/bin/python3 dread_map_extract.py [out_dir]
"""
import os
import json
import struct
import sys
from pathlib import Path

from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures.formats.bmmap import Bmmap

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
AREAS = ["s010_cave", "s020_magma", "s030_baselab", "s040_aqua", "s050_forest",
         "s060_quarantine", "s070_basesanc", "s080_shipyard", "s090_skybase"]

# bmmap category -> emitted colour-layer name. Base shape is navmesh+occluders.
BASE_CATS = ["aNavmeshGeos", "mapOccluderGeos"]
COLOR_LAYERS = {
    "heat": "mapHeatRoomGeos",
    "freeze": "mapFreezeRoomGeos",
    "nofreeze": "mapNoFreezeRoomGeos",
    "water": "mapWaterPoolGeos",
    "emmy": "mapEmmyRoomGeos",
    "vignette": "mapVignetteGeos",
}
# categories that are point/box features with an sIconId
FEATURE_CATS = ["mapDoors", "mapBlockages", "mapUsables", "mapItems", "mapProps",
                "mapCentralUnits", "mapBosses", "mapTransportSigns"]


def iter_geos(cat):
    """Yield (aVertex, aIndex) for a geo category, whether it is a list of geos or a
    name->{id->SGeoData} dict (occluders) or a name->SGeoData dict (room types)."""
    if cat is None:
        return
    if hasattr(cat, "keys"):
        for _, v in cat.items():
            if hasattr(v, "keys") and "aVertex" in v:
                yield v["aVertex"], v["aIndex"]
            elif hasattr(v, "keys"):
                for _, gd in v.items():  # occluder: id -> SGeoData
                    if hasattr(gd, "keys") and "aVertex" in gd:
                        yield gd["aVertex"], gd["aIndex"]
    elif isinstance(cat, list):
        for gd in cat:
            if hasattr(gd, "keys") and "aVertex" in gd:
                yield gd["aVertex"], gd["aIndex"]


def build_geo(root, cats, mn, mx):
    span_x = (mx[0] - mn[0]) or 1.0
    span_y = (mx[1] - mn[1]) or 1.0
    verts, indices = [], []
    for cat in cats:
        for av, ai in iter_geos(root.get(cat)):
            base = len(verts)
            for v in av:
                nx = int(max(0, min(65535, (v[0] - mn[0]) / span_x * 65535)))
                ny = int(max(0, min(65535, (v[1] - mn[1]) / span_y * 65535)))
                verts.append((nx, ny))
            for idx in ai:
                indices.append(base + int(idx))
    return verts, indices


def write_geo(path, verts, indices):
    with open(path, "wb") as f:
        f.write(struct.pack("<II", len(verts), len(indices)))
        for x, y in verts:
            f.write(struct.pack("<HH", x, y))
        for i in indices:
            f.write(struct.pack("<I", i))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("map-extract")
    out.mkdir(parents=True, exist_ok=True)
    fte = FileTreeEditor(ExtractedRomFs(ROMFS), Game.DREAD)
    summary = {}
    for area in AREAS:
        bm = fte.get_parsed_asset(f"maps/levels/c10_samus/{area}/{area}.bmmap", type_hint=Bmmap)
        R = bm.raw.Root
        g = R["gridDef"]
        mn, mx = g["vGridMin"], g["vGridMax"]
        grid = {"min": [float(mn[0]), float(mn[1])], "max": [float(mx[0]), float(mx[1])]}
        json.dump(grid, open(out / f"{area}.grid.json", "w"))

        bv, bi = build_geo(R, BASE_CATS, mn, mx)
        write_geo(out / f"{area}.geo", bv, bi)
        layers = {"base": {"tris": len(bi) // 3, "verts": len(bv)}}
        for lname, cat in COLOR_LAYERS.items():
            lv, li = build_geo(R, [cat], mn, mx)
            if li:
                write_geo(out / f"{area}.{lname}.geo", lv, li)
                layers[lname] = {"tris": len(li) // 3, "verts": len(lv)}

        features = []
        for cat in FEATURE_CATS:
            c = R.get(cat)
            if not hasattr(c, "keys"):
                continue
            for name, v in c.items():
                if not hasattr(v, "keys") or "vPos" not in v:
                    continue
                feat = {"cat": cat, "name": str(name),
                        "pos": [float(v["vPos"][0]), float(v["vPos"][1])]}
                for f in ("sIconId", "sLeftIconId", "sRightIconId"):
                    if f in v and v[f] is not None:
                        feat[f] = str(v[f])
                features.append(feat)
        json.dump(features, open(out / f"{area}.features.json", "w"))
        summary[area] = {"grid": grid, "layers": layers, "features": len(features)}
        print(f"{area}: base {layers['base']['tris']} tris, "
              f"colour layers {sorted(k for k in layers if k!='base')}, {len(features)} features")
    json.dump(summary, open(out / "summary.json", "w"), indent=1)
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
