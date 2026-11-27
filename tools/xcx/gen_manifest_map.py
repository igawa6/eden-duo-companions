#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Map page's native map widgets into the XCX package manifest (idempotent).

The widget world is x = game x, y = -game z. The picture boxes must match xcx_maptiles.cpp / xcx_map.h
(tools/xcx/test_manifest_map.py checks them against the numbers below, and the C++ test checks the same
numbers on its side).

usage: gen_manifest_map.py [manifest.json]
"""
import json
import pathlib
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"

SCALE = 6144 / 30000          # map01 texels per metre
HALF_X, HALF_Z = 15000.0, 13750.0
HI_W, HI_H, HI_COLS, HI_ROWS = 1024, 1024, 6, 6
LO_COLS, LO_ROWS = 2, 2
NLA_COLS, NLA_ROWS, NLA_TILE, NLA_X0, NLA_Z0 = 4, 4, 700.0, -3480.0, 6190.0
BARRACKS = (1960, 1280)
ARROW = "module:xcx:map/arrow/0/0/0"
BLANK = "module:xcx:map/blank/0/0/0"
BARRACKS_KEY = "module:xcx:map/b/0/0/0"
MAP_RECT = [0, 0, 860, 960]
BG = "#FF0E1321"


def box_from_texels(u0, v0, u1, v1):
    x0, x1 = u0 / SCALE - HALF_X, u1 / SCALE - HALF_X
    z0, z1 = v0 / SCALE - HALF_Z, v1 / SCALE - HALF_Z
    return [round(x0, 3), round(-z1, 3), round(x1, 3), round(-z0, 3)]


def overlay(src_bind, box, show):
    return {"src_bind": src_bind, "x0": box[0], "y0": box[1], "x1": box[2], "y1": box[3], "show_bind": show, "opacity": 1}


def hi_overlays():
    out = []
    for n in range(HI_COLS * HI_ROWS):
        tx, ty = n % HI_COLS, n // HI_COLS
        height = 512 if ty == HI_ROWS - 1 else HI_H
        out.append(overlay(f"xcx.mh.{n}", box_from_texels(tx * HI_W, ty * HI_H, (tx + 1) * HI_W, ty * HI_H + height), f"xcx.mhs.{n}"))
    return out


def lo_overlays():
    out = []
    for n in range(LO_COLS * LO_ROWS):
        tx, ty = n % LO_COLS, n // LO_COLS
        out.append(overlay(f"xcx.ml.{n}", box_from_texels(tx * 3072, ty * 2816, (tx + 1) * 3072, (ty + 1) * 2816), f"xcx.mls.{n}"))
    return out


def nla_overlays():
    out = []
    for n in range(NLA_COLS * NLA_ROWS):
        tx, ty = n % NLA_COLS, n // NLA_COLS
        x0, z0 = NLA_X0 + tx * NLA_TILE, NLA_Z0 + ty * NLA_TILE
        out.append(overlay(f"xcx.mn.{n}", [x0, -(z0 + NLA_TILE), x0 + NLA_TILE, -z0], f"xcx.mns.{n}"))
    return out


SKELL = {"group": "skell", "count": 1, "x": "xcx.sk_x{i}", "y": "xcx.sk_y{i}", "kind": "xcx.sk_kind{i}",
         "hide_when_kind": 0, "icon_src_bind": "xcx.msk", "size": 32}


def areas():
    world = {"image": BLANK, "min": [-HALF_X, -HALF_Z], "max": [HALF_X, HALF_Z], "no_pin": False, "dynamic_markers": [SKELL]}
    nla_min = [NLA_X0, -(NLA_Z0 + NLA_ROWS * NLA_TILE)]
    nla_max = [NLA_X0 + NLA_COLS * NLA_TILE, -NLA_Z0]
    return {
        "xcx_over": dict(world),
        "xcx_area": dict(world),
        "xcx_nla": {"image": BLANK, "min": nla_min, "max": nla_max, "no_pin": False, "dynamic_markers": [SKELL]},
        "xcx_barracks": {"image": BARRACKS_KEY, "min": [0, 0], "max": [BARRACKS[0], BARRACKS[1]], "no_pin": False},  # no_pin would hide the arrow
    }


def marker_keys():
    return {"marker_x_bind": "xcx.pl_x", "marker_y_bind": "xcx.pl_y", "marker_src": ARROW, "marker_size": [72, 72],
            "marker_rotate_bind": "xcx.player_yaw_deg"}


def widgets():
    base = {"type": "map", "rect": MAP_RECT, "bg": BG, "area_label": False}
    over = dict(base, id="xcx-map-over", area="xcx_over", follow_window=15000, pan_zoom=True, min_zoom=0.5, max_zoom=6.0,
                **marker_keys(), overlays=lo_overlays(), need_bind="xcx.mw.over")
    home = dict(base, id="xcx-map-home", area="xcx_over", follow_window=30000, pan_zoom=True, min_zoom=0.5, max_zoom=6.0,
                overlays=lo_overlays(), need_bind="xcx.mw.home")
    world = dict(base, id="xcx-map-world", area="xcx_area", follow_window=2400, pan_zoom=True, min_zoom=0.35, max_zoom=3.0,
                 **marker_keys(), overlays=lo_overlays() + hi_overlays(), need_bind="xcx.mw.world")
    nla = dict(base, id="xcx-map-nla", area="xcx_nla", follow_window=520, pan_zoom=True, min_zoom=0.5, max_zoom=4.0,
               **marker_keys(), overlays=nla_overlays(), need_bind="xcx.mw.nla")
    # the Barracks: whole interior at zoom 1 (no follow), the arrow in the picture's own units
    barracks = dict(base, id="xcx-map-barracks", area="xcx_barracks", pan_zoom=True, min_zoom=1.0, max_zoom=6.0,
                    **marker_keys(), need_bind="xcx.mw.barracks")
    return [over, home, world, nla, barracks]


def rect(r, on_tap, need):
    return {"type": "rect", "rect": r, "bg": "#00000000", "color": "#00000000", "frame": 0, "on_tap": on_tap, "need_bind": need}


GENERATED_PREFIXES = ("xcx-map-",)


def main(path):
    path = pathlib.Path(path)
    m = json.loads(path.read_text())
    # areas
    m["map"] = {"areas": areas()}
    # actions
    for name in ["zoom_0", "zoom_1", "zoom_2", "find_self"]:
        m["actions"].pop(name, None)
    m["actions"]["map_tab_over"] = {"kind": "flag", "flag": "xcx.map_mode", "value": 0, "haptic": "light"}
    m["actions"]["map_tab_area"] = {"kind": "flag", "flag": "xcx.map_mode", "value": 1, "haptic": "light"}
    for key, view in [("over", "xcx-map-over"), ("home", "xcx-map-home"), ("world", "xcx-map-world"), ("nla", "xcx-map-nla"), ("barracks", "xcx-map-barracks")]:
        m["actions"][f"find_self_{key}"] = {"kind": "view_reset", "view": view, "haptic": "light"}
    # flags
    m["flags"]["xcx.map_mode"] = 0
    if "xcx.map_mode" not in m["persist_flags"]:
        m["persist_flags"].append("xcx.map_mode")
    # pages
    for page in m["pages"]:
        if page["id"] == "map":
            ws = [w for w in page["widgets"] if not str(w.get("id", "")).startswith(GENERATED_PREFIXES)]
            ws = [w for w in ws if w.get("on_tap") not in ("find_self", "find_self_over", "find_self_home", "find_self_world", "find_self_nla", "find_self_barracks", "map_tab_over", "map_tab_area")]
            # the page picture stays on top of the maps (they come first); chrome taps after it
            ws = widgets() + ws
            ws.append(rect([16, 72, 196, 88], "map_tab_over", "xcx.map_page"))
            ws.append(rect([220, 72, 196, 88], "map_tab_area", "xcx.map_page"))
            for key in ("over", "home", "world", "nla", "barracks"):
                ws.append(rect([716, 824, 136, 120], f"find_self_{key}", f"xcx.mw.{key}"))
            page["widgets"] = ws
        if page["id"] == "settings":
            page["widgets"] = [w for w in page["widgets"] if w.get("on_tap") not in ("zoom_0", "zoom_1", "zoom_2")]
    path.write_text(json.dumps(m, indent=2) + "\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
