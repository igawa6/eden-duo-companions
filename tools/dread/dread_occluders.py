#!/usr/bin/env python3
"""Bake mapOccluderGeos without merging an actor's alternative collision stages.
The title module resolves each collider's enabled state; the generic renderer only needs its
unique name and triangles. Owner and exact 64-bit collider keys remain package metadata.
Idempotent (replaces the list). Run: venv/bin/python3 dread_occluders.py"""
import os
import json
from pathlib import Path

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
MANIFEST = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen")) / "manifest.json"  # an asset-carrying (pre-1.0.0) package dir; never the release package
AREAS = ["s010_cave","s020_magma","s030_baselab","s040_aqua","s050_forest",
         "s060_quarantine","s070_basesanc","s080_shipyard","s090_skybase"]

def build_occluders(actors):
    out = []
    for actor, geos in actors.items():
        for collider, geo in geos.items():
            vertices = geo.get("aVertex") or []
            indices = geo.get("aIndex") or []
            triangles = []
            for k in range(0, len(indices) - 2, 3):
                triangle = indices[k:k + 3]
                if min(triangle) < 0 or max(triangle) >= len(vertices):
                    continue
                triangles.append([round(float(vertices[i][axis]), 1)
                                  for i in triangle for axis in (0, 1)])
            if triangles:
                key = f"0x{int(collider):016X}"
                out.append({"n": f"{actor}#{key[2:]}", "owner": str(actor),
                            "collider": key, "t": triangles})
    return out


def main():
    from mercury_engine_data_structures.romfs import ExtractedRomFs
    from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
    from mercury_engine_data_structures.game_check import Game
    from mercury_engine_data_structures.formats.bmmap import Bmmap

    fte = FileTreeEditor(ExtractedRomFs(ROMFS), Game.DREAD)
    man = json.load(open(MANIFEST))
    for area in AREAS:
        if area not in man["map"]["areas"]:
            continue
        bm = fte.get_parsed_asset(f"maps/levels/c10_samus/{area}/{area}.bmmap", type_hint=Bmmap)
        occ = bm.raw.Root.get("mapOccluderGeos") or {}
        out = build_occluders(occ)
        tri_total = sum(len(item["t"]) for item in out)
        man["map"]["areas"][area]["occluders"] = out
        print(f"{area}: {len(out)} occluder colliders, {tri_total} tris")
    json.dump(man, open(MANIFEST, "w"), indent=1)
    print("manifest updated (occluders)")

if __name__ == "__main__":
    main()
