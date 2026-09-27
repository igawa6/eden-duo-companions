#!/usr/bin/env python3
"""Room classes from the game's own minimap room models (fixed vertex->world transform).

Dread renders each area's `system/minimap/maproom/<area>/<area>.bcmdl` (material
vcminimaproom_generic, vertex colour passed straight through) into the minimap's "map room"
target, and its compositing shader (system/shd/pp_minimap_nx.bshdat) reads the vertex colour as
the room class:
    r > 0            -> E.M.M.I. zone room   (g_uEmmyRoomClr)
    g > 0, b == 0    -> special room         (g_uSpecialRoomClr,   gold:  Save / Map / Network+AccessPoint)
    g > 0, b >  0    -> transport room       (g_uTransportRoomClr, purple: Train / Elevator / Transport)
    b > 0 only       -> normal room          (g_uNormalRoomClr)
Only the >0 tests matter for the class; the different R / B intensity levels (25..255) are a
per-room identity so that the shader's neighbour-colour discontinuity test (bIsRoomBorder) draws a
border between adjacent rooms of the same class (an L-shaped room is several same-coloured quads).

Vertex -> world transform (verified against the station/E.M.M.I. icons of all 9 areas):
    world = raw_vertex_pos + submesh.transform(3f) + joint[jMap[0]].world_pos
The engine places the model instance itself at world (0, 0, 200) (loader main+0xE99E18 ->
SetPosition(main+0x143578) with {0,0,200.0f}), i.e. NO gridDef-derived offset; the XY placement is
entirely model-internal.  The submesh's `unk0` 19 floats are NOT a transform to apply: they are the
world-space AABB of the submesh = [row-major 4x4 with the AABB centre as translation][size x, y, z].
For every area  bbox_centre(raw) + submesh.transform + joint.pos == unk0 translation, which is what
the game's culling/camera uses; the joint `minimap` is at (3050, 550, 100) in every area and the
per-area `transform` compensates.  Using the raw positions (the previous version) shifts every
area by that per-area vector (e.g. s010_cave by (+3436.5, -282.4)).

This script never writes the manifest in place: it emits a sidecar JSON with gold_polys /
transport_polys / emmy_polys per area (one polygon = one triangle, 6 floats, world units, same
format as before) and prints the per-area counts plus an icon-containment self-check.  Pass
--write-manifest PATH to write a merged copy of the manifest to PATH."""
import os
import argparse, gzip, json, math, struct
from collections import Counter, defaultdict
from pathlib import Path
from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game
from mercury_engine_data_structures.formats.bcmdl import Bcmdl

ROMFS = Path(os.environ.get("DREAD_ROMFS", "romfs"))  # extracted romfs (contains packs/, system/, textures/)
MANIFEST = Path(os.environ.get("DREAD_LEGACY_PKG", "legacy/dualscreen")) / "manifest.json"  # an asset-carrying (pre-1.0.0) package dir; never the release package
OUT_DEFAULT = Path("room_classes.json")

SPECIAL_ICONS = {"UsableStationSave", "UsableStationMap", "UsableStationNetwork", "UsableAccessPoint"}
TRANSPORT_ICONS = {"UsableTrain", "UsableElevator", "UsableTransport"}
EMMY_ICON_PREFIXES = ("DoorEmmy", "PropEmmyValve")


def unz(b):
    return gzip.decompress(b) if b[:2] == b"\x1f\x8b" else b


def room_class(c):
    """Shader classification of one vertex colour (r, g, b in 0..1)."""
    if c[0] > 0.0:
        return "emmy"
    if c[1] > 0.0:
        return "transport" if c[2] > 0.0 else "special"
    if c[2] > 0.0:
        return "normal"
    return None


def joint_world_pos(raw, name):
    """World position of a joint = sum of its own and all ancestors' positions (rot/scale are all
    identity in these models; assert so the assumption is visible if a future model differs)."""
    joints = {j["data"]["name"]["str"]: j["data"] for j in raw["joints_toc"]["subtoc_entries"]}
    px = py = pz = 0.0
    while name is not None:
        j = joints[name]
        t = j["transform"]
        assert list(t["rot"]) == [0.0, 0.0, 0.0] and list(t["scale"]) == [1.0, 1.0, 1.0], (name, t)
        px += t["pos"][0]; py += t["pos"][1]; pz += t["pos"][2]
        name = j["parent"]["str"] if j["parent"] else None
    return px, py, pz


def load(fte, area):
    """Returns (world_xy per vertex, rgb per vertex, triangles, info dict)."""
    r = fte.get_parsed_asset(f"system/minimap/maproom/{area}/{area}.bcmdl", type_hint=Bcmdl).raw
    assert len(r["submeshes"]) == 1 and len(r["vertex_info"]) == 1 and len(r["tri_info"]) == 1, area
    sm = dict(r["submeshes"][0])
    u = list(sm["unk0"])                    # 4x4 row-major (translation = AABB centre) + size xyz
    tr = list(sm["transform"])              # per-submesh vertex offset
    info = dict(r["submesh_info_tocs"][0]["submesh_infos"][0])
    assert info["skinning_type"] == 0 and len(info["jMap"]) == 1, area
    joint_names = [j["data"]["name"]["str"] for j in r["joints_toc"]["subtoc_entries"]]
    jx, jy, jz = joint_world_pos(r, joint_names[info["jMap"][0]])
    dx, dy, dz = tr[0] + jx, tr[1] + jy, tr[2] + jz

    vi = dict(r["vertex_info"][0]); buf = unz(vi["verts"]["buf"]); n = vi["count"]
    infos = {d["semantic"]: d for d in (dict(i) for i in vi["infos"])}
    raw_pos = [struct.unpack_from("<fff", buf, infos[0]["offset"] + i * 12) for i in range(n)]
    col = [struct.unpack_from("<ffff", buf, infos[5]["offset"] + i * 16)[:3] for i in range(n)]
    pos = [(p[0] + dx, p[1] + dy) for p in raw_pos]

    ti = dict(r["tri_info"][0]); tb = unz(ti["tri_buffer_offset"]["buf"]); cnt = ti["idx_count"]
    fmt = "<%dH" % cnt if len(tb) == cnt * 2 else "<%dI" % cnt
    idx = struct.unpack(fmt, tb[: struct.calcsize(fmt)])
    tris = [(idx[i], idx[i + 1], idx[i + 2]) for i in range(0, cnt - 2, 3)]

    # self-check: the transformed bbox must reproduce the submesh's stored world AABB (unk0)
    xs = [p[0] for p in pos]; ys = [p[1] for p in pos]; zs = [p[2] + dz for p in raw_pos]
    centre = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2)
    size = (max(xs) - min(xs), max(ys) - min(ys))
    assert all(abs(centre[i] - u[[3, 7, 11][i]]) < 0.5 for i in range(3)), (area, centre, u[3], u[7], u[11])
    assert abs(size[0] - u[16]) < 0.5 and abs(size[1] - u[17]) < 0.5, (area, size, u[16], u[17])
    meta = {"offset": [dx, dy, dz], "submesh_transform": tr, "joint_pos": [jx, jy, jz],
            "bbox_min": [min(xs), min(ys)], "bbox_max": [max(xs), max(ys)]}
    return pos, col, tris, meta


def point_in_tri(p, a, b, c, eps):
    """Inside test with `eps` world units of slack on every edge (icons sit on room borders)."""
    def s(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
    area = s(a, b, c)
    if area == 0:
        return False
    for d, e0, e1 in ((s(p, a, b), a, b), (s(p, b, c), b, c), (s(p, c, a), c, a)):
        if d / area < -eps * math.hypot(e0[0] - e1[0], e0[1] - e1[1]) / abs(area):
            return False
    return True


def icon_check(A, pos, col, tris):
    """For every station / E.M.M.I. icon of the area, the set of room classes of the triangles it
    touches (50-unit tolerance).  Returns (ok, total, mismatches)."""
    ok = total = 0; bad = []
    for ic in A.get("icons", []):
        k = ic.get("i") or ""
        if k in SPECIAL_ICONS: want = "special"
        elif k in TRANSPORT_ICONS: want = "transport"
        elif k.startswith(EMMY_ICON_PREFIXES): want = "emmy"
        else: continue
        total += 1
        got = {room_class(col[t[0]]) for t in tris
               if point_in_tri((ic["x"], ic["y"]), pos[t[0]], pos[t[1]], pos[t[2]], 50.0)}
        if want in got: ok += 1
        else: bad.append((k, ic["x"], ic["y"], sorted(g for g in got if g)))
    return ok, total, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", type=Path, default=MANIFEST, help="manifest to read areas/icons from (never written)")
    ap.add_argument("--out", type=Path, default=OUT_DEFAULT, help="sidecar JSON with the per-area polygons")
    ap.add_argument("--write-manifest", type=Path, default=None, help="write a merged COPY of the manifest here")
    args = ap.parse_args()

    fte = FileTreeEditor(ExtractedRomFs(ROMFS), Game.DREAD)
    man = json.load(open(args.manifest))
    result = {}
    tot_ok = tot_n = 0
    for area, A in man["map"]["areas"].items():
        pos, col, tris, meta = load(fte, area)
        polys = {"gold_polys": [], "transport_polys": [], "emmy_polys": []}
        levels = Counter()
        for t in tris:
            cs = {tuple(col[k]) for k in t}
            assert len(cs) == 1, (area, t, cs)          # flat-coloured per triangle
            c = col[t[0]]
            cl = room_class(c)
            levels[(cl, tuple(int(round(v * 255)) for v in c))] += 1
            poly = [round(v, 1) for k in t for v in pos[k]]
            if cl == "emmy": polys["emmy_polys"].append(poly)
            elif cl == "special": polys["gold_polys"].append(poly)
            elif cl == "transport": polys["transport_polys"].append(poly)
        ok, n, bad = icon_check(A, pos, col, tris)
        tot_ok += ok; tot_n += n
        result[area] = dict(polys, transform=meta)
        nrm = sum(v for (cl, _), v in levels.items() if cl == "normal")
        print(f"{area}: {len(tris)} tris -> special {len(polys['gold_polys'])}, transport "
              f"{len(polys['transport_polys'])}, emmy {len(polys['emmy_polys'])}, normal {nrm}; "
              f"offset ({meta['offset'][0]:.1f}, {meta['offset'][1]:.1f}) = submesh.transform "
              f"({meta['submesh_transform'][0]:.1f}, {meta['submesh_transform'][1]:.1f}) + joint "
              f"({meta['joint_pos'][0]:.0f}, {meta['joint_pos'][1]:.0f}); bbox {[round(v) for v in meta['bbox_min']]}"
              f"..{[round(v) for v in meta['bbox_max']]} vs grid {A['min']}..{A['max']}")
        print(f"   icons in expected class: {ok}/{n}" + (f"; misses: {bad}" if bad else ""))
        print("   colour levels: " + ", ".join(f"{cl}{rgb}x{v}" for (cl, rgb), v in sorted(levels.items(), key=lambda kv: (str(kv[0][0]), kv[0][1]))))
    print(f"TOTAL icons in expected class: {tot_ok}/{tot_n}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    json.dump(result, open(args.out, "w"), indent=1)
    print(f"wrote {args.out}")
    if args.write_manifest:
        for area, R in result.items():
            man["map"]["areas"][area].update({k: R[k] for k in ("gold_polys", "transport_polys", "emmy_polys")})
        json.dump(man, open(args.write_manifest, "w"), indent=1)
        print(f"wrote merged manifest copy {args.write_manifest}")


if __name__ == "__main__":
    main()
