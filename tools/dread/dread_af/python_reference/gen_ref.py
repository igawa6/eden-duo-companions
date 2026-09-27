#!/usr/bin/env python3
"""Reference generator (Python + MEDS): rebuild the Dread package's per-area map data from romfs,
to be compared field by field against the shipped manifest. The C++ module port must match this."""
import json, struct, sys, math
from pathlib import Path
sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "..", ".."))
from mercury_engine_data_structures.romfs import ExtractedRomFs
from mercury_engine_data_structures.file_tree_editor import FileTreeEditor
from mercury_engine_data_structures.game_check import Game

ROMFS = Path(__import__("os").environ.get("DREAD_ROMFS", "romfs"))
AREAS = ["s010_cave", "s020_magma", "s030_baselab", "s040_aqua", "s050_forest",
         "s060_quarantine", "s070_basesanc", "s080_shipyard", "s090_skybase"]
_fte = None


def fte():
    global _fte
    if _fte is None:
        _fte = FileTreeEditor(ExtractedRomFs(ROMFS), Game.DREAD)
    return _fte


_cache = {}


def asset(path):
    if path not in _cache:
        _cache[path] = fte().get_parsed_asset(path).raw
    return _cache[path]


def bmmap(area):
    return asset(f"maps/levels/c10_samus/{area}/{area}.bmmap").Root


def occluders(area):
    import dread_occluders
    return dread_occluders.build_occluders(bmmap(area).get("mapOccluderGeos") or {})


def vignettes(area):
    import dread_vignettes
    root = bmmap(area)
    out = []
    for name, g in (root.get("mapVignetteGeos") or {}).items():
        tris = []
        if hasattr(g, "get") and g.get("aVertex") is not None:
            tris = dread_vignettes.geo_tris(g)
        elif hasattr(g, "items"):
            for _, gg in g.items():
                tris += dread_vignettes.geo_tris(gg)
        if tris:
            out.append({"n": str(name), "t": tris})
    return out


def camera_rects(area):
    sc = asset(f"maps/levels/c10_samus/{area}/{area}.bmscc")
    rects = []
    for e in sc.layers[0].entries:
        polys = [[(float(pt.x), float(pt.y)) for pt in p.points] for p in e.data.polys
                 if len(p.points) >= 3]
        if not polys:
            continue
        xs = [pt[0] for poly in polys for pt in poly]
        ys = [pt[1] for poly in polys for pt in poly]
        rects.append([round(min(xs), 1), round(min(ys), 1), round(max(xs), 1), round(max(ys), 1)])
    return rects


def water_pools(area):
    import dread_water_table
    pools, triggers = dread_water_table.extract(fte(), area)
    return pools


def overview_regions(area):
    import dread_overview, dread_room_classes
    positions, colors, triangles, meta = dread_room_classes.load(fte(), area)
    return dread_overview.build_overview_regions(positions, colors, triangles)


FIELDS = {"occluders": occluders, "vignettes": vignettes, "camera_rects": camera_rects,
          "water_pools": water_pools, "overview_regions": overview_regions}


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def norm(v):
    if isinstance(v, bool) or v is None or isinstance(v, str):
        return v
    if isinstance(v, (int, float)):
        return f32(float(v))
    if isinstance(v, (list, tuple)):
        return [norm(x) for x in v]
    if isinstance(v, dict):
        return {k: norm(x) for k, x in v.items()}
    return v


def first_diff(a, b, path=""):
    if type(a) != type(b):
        return f"{path}: type {type(a).__name__} vs {type(b).__name__} ({str(a)[:60]} | {str(b)[:60]})"
    if isinstance(a, dict):
        if set(a) != set(b):
            return f"{path}: keys {sorted(set(a) ^ set(b))}"
        for k in a:
            d = first_diff(a[k], b[k], path + "/" + k)
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: len {len(a)} vs {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, f"{path}[{i}]")
            if d:
                return d
        return None
    return None if a == b else f"{path}: {a!r} vs {b!r}"


def room_categories_polys(area):
    import dread_room_classes
    pos, col, tris, meta = dread_room_classes.load(fte(), area)
    polys = {"emmi": [], "transport": [], "station": []}
    for t in tris:
        c = col[t[0]]
        cl = dread_room_classes.room_class(c)
        poly = [round(v, 1) for k in t for v in pos[k]]
        if cl == "emmy":
            polys["emmi"].append(poly)
        elif cl == "special":
            polys["station"].append(poly)
        elif cl == "transport":
            polys["transport"].append(poly)
    return polys


def geo_blobs(area):
    """{suffix: bytes} for <area>.geo and <area>.<layer>.geo exactly as the package ships them."""
    import dread_map_extract as X, io, dread_magnet
    R = bmmap(area)
    g = R["gridDef"]
    mn, mx = g["vGridMin"], g["vGridMax"]

    def pack(verts, idx):
        b = io.BytesIO()
        b.write(struct.pack("<II", len(verts), len(idx)))
        for x, y in verts:
            b.write(struct.pack("<HH", x, y))
        for i in idx:
            b.write(struct.pack("<I", i))
        return b.getvalue()
    out = {"": pack(*X.build_geo(R, X.BASE_CATS, mn, mx))}
    for lname, cat in X.COLOR_LAYERS.items():
        lv, li = X.build_geo(R, [cat], mn, mx)
        if li:
            out["." + lname] = pack(lv, li)
    return out


def magnet_blob(area):
    """<area>.magnet.geo as dread_magnet.py writes it (None when the area has no magnet walls)."""
    import dread_magnet as MG, io
    root = bmmap(area)
    ms = getattr(root, "mapMagnetSurfaces", None)
    g = root["gridDef"]
    mnx, mny = float(g["vGridMin"][0]), float(g["vGridMin"][1])
    mxx, mxy = float(g["vGridMax"][0]), float(g["vGridMax"][1])
    sx = (mxx - mnx) or 1.0
    sy = (mxy - mny) or 1.0
    half, upp, _ = MG.half_width(sx, sy)
    nv = lambda x, y: (int(max(0, min(65535, (x - mnx) / sx * 65535))),
                       int(max(0, min(65535, (y - mny) / sy * 65535))))
    verts, idx = [], []
    items = ms.items() if hasattr(ms, "items") else []
    for _, sd in items:
        pts = [(float(s.vPos[0]), float(s.vPos[1])) for s in sd.oPolyLine.oSegmentData]
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            dx, dy = x1 - x0, y1 - y0
            L = math.hypot(dx, dy) or 1.0
            h = half / (max(abs(dx), abs(dy)) / L or 1.0)
            px, py = -dy / L * h, dx / L * h
            b = len(verts)
            for qx, qy in [(x0 + px, y0 + py), (x1 + px, y1 + py), (x1 - px, y1 - py), (x0 - px, y0 - py)]:
                verts.append(nv(qx, qy))
            idx += [b, b + 1, b + 2, b, b + 2, b + 3]
    if not idx:
        return None
    b = io.BytesIO()
    b.write(struct.pack("<II", len(verts), len(idx)))
    for x, y in verts:
        b.write(struct.pack("<HH", x, y))
    for i in idx:
        b.write(struct.pack("<I", i))
    return b.getvalue()


SHIELD_DEFS = {"door_shield_plasma.bmsad", "doorshieldmissile.bmsad",
               "doorshieldsupermissile.bmsad", "doorwavebeam.bmsad"}
ICON_CATS = [("Items", "mapItems"), ("Blockages", "mapBlockages"), ("Usables", "mapUsables"),
             ("Props", "mapProps"), ("Bosses", "mapBosses"), ("CentralUnits", "mapCentralUnits")]


def brfld_actors(area):
    root = asset(f"maps/levels/c10_samus/{area}/{area}.brfld").Root
    out = []
    for lname, layer in root.pScenario.rEntitiesLayer.dctSublayers.items():
        for aname, actor in layer.dctActors.items():
            out.append((str(aname), float(actor.vPos[0]), float(actor.vPos[1]),
                        str(actor.oActorDefLink).split("/")[-1]))
    return out


def icons(area, known):
    import dread_add_doors as D
    R = bmmap(area)
    acts = None
    out = []
    # hint boxes (dread_item_boxes.py): the mapHintGeos rect containing each item, last one wins
    hints = {}
    for hname, g in (R.get("mapHintGeos") or {}).items():
        vs = [(float(p.x), float(p.y)) for p in g["aVertex"]]
        bb = [min(x for x, _ in vs), min(y for _, y in vs), max(x for x, _ in vs), max(y for _, y in vs)]
        for iname, v in R.get("mapItems").items():
            px, py = float(v["vPos"].x), float(v["vPos"].y)
            if bb[0] <= px <= bb[2] and bb[1] <= py <= bb[3]:
                hints[str(iname)] = bb
    for kind, cat in ICON_CATS:
        for name, v in (R.get(cat) or {}).items():
            x, y = float(v["vPos"][0]), float(v["vPos"][1])
            ic = {"k": kind, "i": str(v["sIconId"]), "x": round(x, 1), "y": round(y, 1)}
            if kind == "Items":
                ic["n"] = str(name)
                ob = v["oBox"]
                ic["pb"] = [float(ob.Min.x), float(ob.Min.y), float(ob.Max.x), float(ob.Max.y)]
                if str(name) in hints:
                    ic["hb"] = hints[str(name)]
            elif kind == "Blockages":
                if acts is None:
                    acts = brfld_actors(area)
                c = [a[0] for a in acts if a[1] == x and a[2] == y and a[3] in SHIELD_DEFS]
                if c:
                    ic["n"] = c[-1]
            out.append(ic)
    # doors (dread_add_doors.py)
    for name, d in (R.get("mapDoors") or {}).items():
        if not hasattr(d, "keys"):
            continue
        vpos = d.get("vPos")
        if vpos is None:
            continue
        u = D.box_union_rect(d.get("oBoxL"), d.get("oBoxR"))
        if u is None:
            u = [float(vpos[0]) - 150, float(vpos[1]) - 150, float(vpos[0]) + 150, float(vpos[1]) + 150]
        ucx = (u[0] + u[2]) * 0.5
        sides = ((d.get("sLeftIconId"), [u[0], u[1], ucx, u[3]]),
                 (d.get("sRightIconId"), [ucx, u[1], u[2], u[3]]))
        have_both = all(i and str(i) in known for i, _ in sides)
        for icon, r in sides:
            if not icon or str(icon) not in known:
                continue
            if not have_both:
                r = u
            out.append({"k": "Doors", "i": str(icon), "x": ucx, "y": (u[1] + u[3]) * 0.5,
                        "bx": r, "n": str(name)})
    # vignette tags (dread_vignettes.py)
    pos_lookup = {}
    for cat in ("mapDoors", "mapItems", "mapUsables", "mapProps", "mapBlockages", "mapCentralUnits",
                "mapBosses"):
        for k, v in (R.get(cat) or {}).items():
            p = v.get("vPos") if hasattr(v, "get") else None
            if p is not None:
                pos_lookup[str(k)] = (float(p[0]), float(p[1]))
    occ = R.get("mapVignetteOccludedIcons") or {}
    for vname, namelist in (occ.items() if hasattr(occ, "items") else []):
        for ent in (namelist or []):
            nm = str(ent).split(" (")[0]
            p = pos_lookup.get(nm) or pos_lookup.get(str(ent))
            if p is None:
                continue
            for it in out:
                if abs(it["x"] - p[0]) <= 200 and abs(it["y"] - p[1]) <= 200:
                    it["v"] = str(vname)
    return out


if __name__ == "__main__":
    man = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "before/dualscreen/manifest.json"))
    only = sys.argv[2].split(",") if len(sys.argv) > 2 else list(FIELDS)
    for area in AREAS:
        A = man["map"]["areas"][area]
        for f in only:
            got = norm(FIELDS[f](area))
            want = norm(A.get(f, []))
            d = first_diff(got, want)
            print(f"{area:16s} {f:18s} " + ("OK" if not d else "DIFF " + d))
