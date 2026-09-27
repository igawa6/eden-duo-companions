#!/usr/bin/env python3
"""Add minimap DOORS to each area's icon list in the DSMod manifest.

The door SPRITES (DoorPowerL/R, DoorChargeL/R, DoorEmmyOpen/Closed, ...) are already in
manifest map.icons, and blockages already render, but the mapDoors features were never added to
the per-area icon lists. Parse mapDoors from each .bmmap via MEDS and append an icon per door side
at its box centre (single-sided doors get one icon at vPos). Idempotent: strips any prior Doors
entries first. Run: venv/bin/python3 dread_add_doors.py
"""
import os
import json
from pathlib import Path

from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures.formats.bmmap import Bmmap

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
PKG = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen"))  # an asset-carrying (pre-1.0.0) package dir; never the release package
AREAS = ["s010_cave", "s020_magma", "s030_baselab", "s040_aqua", "s050_forest",
         "s060_quarantine", "s070_basesanc", "s080_shipyard", "s090_skybase"]
FLT_MAX = 3.0e38  # oBox sentinel for "no this side" (single-sided doors)


def box_center(box):
    """SBox {Min:[x,y], Max:[x,y]} -> (cx,cy) or None if the FLT_MAX sentinel."""
    if box is None:
        return None
    mn, mx = box.get("Min"), box.get("Max")
    if mn is None or mx is None or abs(mn[0]) > FLT_MAX or abs(mx[0]) > FLT_MAX:
        return None
    return ((mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5)


def box_rect(box):
    """Single SBox {Min,Max} -> [minx,miny,maxx,maxy], or None if absent/FLT_MAX sentinel."""
    if box is None or not hasattr(box, "get"):
        return None
    mn, mx = box.get("Min"), box.get("Max")
    if mn is None or mx is None:
        return None
    if any(abs(float(c)) > FLT_MAX for c in (mn[0], mn[1], mx[0], mx[1])):
        return None
    return [float(mn[0]), float(mn[1]), float(mx[0]), float(mx[1])]


def box_union_rect(*boxes):
    """Union of one or more SBox {Min,Max} into [minx,miny,maxx,maxy], or None (skips FLT_MAX)."""
    xs, ys = [], []
    for b in boxes:
        if b is None or not hasattr(b, "get"):
            continue
        mn, mx = b.get("Min"), b.get("Max")
        if mn is None or mx is None:
            continue
        if any(abs(float(c)) > FLT_MAX for c in (mn[0], mn[1], mx[0], mx[1])):
            continue
        xs += [float(mn[0]), float(mx[0])]
        ys += [float(mn[1]), float(mx[1])]
    return [min(xs), min(ys), max(xs), max(ys)] if xs else None


def main():
    fte = FileTreeEditor(ExtractedRomFs(ROMFS), Game.DREAD)
    man = json.load(open(PKG / "manifest.json"))
    known = set(man["map"]["icons"].keys())
    for area in AREAS:
        if area not in man["map"]["areas"]:
            continue
        bm = fte.get_parsed_asset(f"maps/levels/c10_samus/{area}/{area}.bmmap", type_hint=Bmmap)
        doors = bm.raw.Root.get("mapDoors") or {}
        icons = man["map"]["areas"][area]["icons"]
        icons[:] = [i for i in icons if i.get("k") != "Doors"]  # idempotent: drop old doors
        added = 0
        for name, d in doors.items():
            if not hasattr(d, "keys"):
                continue
            vpos = d.get("vPos")
            if vpos is None:
                continue
            # The door must cover the WHOLE pipe like the in-game minimap, so each leaf gets half
            # of the UNION of oBoxL+oBoxR, split at the door centre: L spans union-min..centre and R
            # spans centre..union-max, abutting mid-pipe with no gap (the raw leaf boxes sit at the
            # outer edges with an ~100-unit opening between them, which left the pipe uncovered).
            # Single-sided doors (FLT_MAX sentinel) get the full fallback/box.
            u = box_union_rect(d.get("oBoxL"), d.get("oBoxR"))
            if u is None:
                u = [float(vpos[0]) - 150, float(vpos[1]) - 150,
                     float(vpos[0]) + 150, float(vpos[1]) + 150]
            ucx = (u[0] + u[2]) * 0.5
            sides = ((d.get("sLeftIconId"), [u[0], u[1], ucx, u[3]]),
                     (d.get("sRightIconId"), [ucx, u[1], u[2], u[3]]))
            have_both = all(i and str(i) in known for i, _ in sides)
            for icon, r in sides:
                if not icon or str(icon) not in known:
                    continue
                if not have_both:
                    r = u  # single glyph: cover the whole box itself
                # x,y = the DOOR centre (not the leaf centre) so both halves share one fog check
                # and appear together once the doorway is revealed, like the in-game minimap.
                # "n" = the mapDoors key == the scenario actor name, so the runtime can match the
                # door's live "<actor>:DOOR:Opened" blackboard prop and swap in the opened glyph.
                icons.append({"k": "Doors", "i": str(icon),
                              "x": ucx, "y": (u[1] + u[3]) * 0.5, "bx": r, "n": str(name)})
                added += 1
        seals = []
        for _, d in doors.items():
            if not hasattr(d, "keys"):
                continue
            r = box_union_rect(d.get("oBoxL"), d.get("oBoxR"))
            if r is None:  # single-sided / no box: fall back to a small patch at vPos
                vp = d.get("vPos")
                if vp is not None:
                    r = [float(vp[0]) - 100, float(vp[1]) - 150,
                         float(vp[0]) + 100, float(vp[1]) + 150]
            if r:
                seals.append(r)
        for _, b in (bm.raw.Root.get("mapBlockages") or {}).items():
            if hasattr(b, "keys"):
                r = box_union_rect(b.get("oBox"))
                if r:
                    seals.append(r)
        man["map"]["areas"][area]["seals"] = seals
        print(f"{area}: +{added} door icons ({len(doors)} doors), +{len(seals)} seals")
    json.dump(man, open(PKG / "manifest.json", "w"), indent=1)
    print("manifest updated")


if __name__ == "__main__":
    main()
