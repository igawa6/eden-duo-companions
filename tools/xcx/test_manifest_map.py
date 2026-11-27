#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Map page's native widgets: structure, world boxes, binds the module publishes, actions."""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
MODULE = (ROOT / "native/modules/0100453019AA8000.cpp").read_text()
m = json.loads(MANIFEST.read_text())
page = next(p for p in m["pages"] if p["id"] == "map")
widgets = {w["id"]: w for w in page["widgets"] if "id" in w}
order = [w.get("id", "") for w in page["widgets"]]
fails = []


def check(ok, why):
    if not ok:
        fails.append(why)


# the page picture is drawn after (on top of) the four maps
for wid in ("xcx-map-over", "xcx-map-home", "xcx-map-world", "xcx-map-nla", "xcx-map-barracks"):
    check(wid in widgets, f"missing widget {wid}")
    check(order.index(wid) < order.index("xcx-page"), f"{wid} must be under the page picture")
    w = widgets[wid]
    check(w["type"] == "map" and w["rect"] == [0, 0, 860, 960] and w.get("pan_zoom") is True, f"{wid} basics")
    check(w["area"] in m["map"]["areas"], f"{wid} area")
# markers: world widgets bind the same published position, heading and picture
for wid in ("xcx-map-over", "xcx-map-world", "xcx-map-nla"):
    w = widgets[wid]
    check(w["marker_x_bind"] == "xcx.pl_x" and w["marker_y_bind"] == "xcx.pl_y", f"{wid} marker binds")
    check(w["marker_rotate_bind"] == "xcx.player_yaw_deg", f"{wid} marker rotation")
    check(w["marker_src"].startswith("module:xcx:map/arrow/"), f"{wid} marker picture")
    check(w.get("follow_window", 0) > 0, f"{wid} follows the marker")
wb = widgets["xcx-map-barracks"]
check(wb.get("marker_x_bind") == "xcx.pl_x" and wb.get("marker_y_bind") == "xcx.pl_y", "the Barracks map binds the marker")
check(wb.get("marker_rotate_bind") == "xcx.player_yaw_deg" and wb["marker_src"].startswith("module:xcx:map/arrow/"),
      "the Barracks marker turns with the heading")
check(wb.get("follow_window", 0) == 0, "the Barracks map shows the whole interior (no follow)")
# the runtime hides the live marker on a no_pin area (mod_ui_map_widget.cpp), found live 2026-11-23
check(m["map"]["areas"]["xcx_barracks"].get("no_pin") is not True, "the Barracks area must not be no_pin (it hides the arrow)")
check("marker_x_bind" not in widgets["xcx-map-home"], "Overworld in the Barracks / unmapped areas has no marker")
# areas: boxes in widget world units (x, y = -z); the world ones span map01
for name in ("xcx_over", "xcx_area"):
    a = m["map"]["areas"][name]
    check(a["min"] == [-15000, -13750] and a["max"] == [15000, 13750], f"{name} box")
# overlays: contiguous, inside their area, every bound name published by the module
def check_overlays(wid, cols, rows, key_prefix, show_prefix):
    ov = [o for o in widgets[wid]["overlays"] if o["src_bind"].startswith(key_prefix)]
    check(len(ov) == cols * rows, f"{wid} {key_prefix} count {len(ov)}")
    for n, o in enumerate(ov):
        check(o["src_bind"] == f"{key_prefix}{n}", f"{wid} overlay {n} key")
        check(o["x1"] > o["x0"] and o["y1"] > o["y0"], f"{wid} overlay {n} box")
        if n % cols + 1 < cols:
            check(abs(ov[n + 1]["x0"] - o["x1"]) < 1e-2, f"{wid} overlay {n} x seam")
        if n + cols < len(ov):
            check(abs(ov[n + cols]["y1"] - o["y0"]) < 1e-2, f"{wid} overlay {n} y seam")
        check(o["show_bind"].startswith(show_prefix), f"{wid} overlay {n} gate")
check_overlays("xcx-map-world", 6, 6, "xcx.mh.", "xcx.mhs.")
check_overlays("xcx-map-world", 2, 2, "xcx.ml.", "xcx.mls.")
check_overlays("xcx-map-over", 2, 2, "xcx.ml.", "xcx.mls.")
check_overlays("xcx-map-home", 2, 2, "xcx.ml.", "xcx.mls.")
check("marker_x_bind" not in widgets["xcx-map-home"], "the home view has no marker")
check_overlays("xcx-map-nla", 4, 4, "xcx.mn.", "xcx.mns.")
ov = widgets["xcx-map-world"]["overlays"]
hi = [o for o in ov if o["src_bind"].startswith("xcx.mh.")]
check(abs(hi[0]["x0"] + 15000) < 1e-2 and abs(hi[0]["y1"] - 13750) < 1e-2 and abs(hi[-1]["x1"] - 15000) < 1e-2 and abs(hi[-1]["y0"] + 13750) < 1e-2,
      "hi-res overlays cover map01")
# the module publishes every name the manifest binds
names = set()
for w in widgets.values():
    for key in ("marker_x_bind", "marker_y_bind", "marker_rotate_bind", "need_bind"):
        if key in w:
            names.add(w[key])
    for o in w.get("overlays", []):
        names.add(o["src_bind"])
        names.add(o["show_bind"])
for a in m["map"]["areas"].values():
    for d in a.get("dynamic_markers", []):
        for key in ("x", "y", "kind"):
            names.add(d[key].replace("{i}", "0"))
        names.add(d["icon_src_bind"])
# module_error: a host point (the wrong-build refusal); derived names are checked with their widgets
for n in sorted(names - {"module_error"} - {d["name"] for d in m["derived"]}):
    stem = re.sub(r"\.\d+$", "", n)
    patterns = [n, stem + ".%d", stem]
    check(any(('"' + p + '"') in MODULE for p in patterns), f"the module never publishes {n}")
# actions: tabs write the persisted flag, Find Self resets the visible map's own view
check("xcx.map_mode" in m["persist_flags"] and m["flags"]["xcx.map_mode"] == 0, "map mode flag is persisted")
check(m["actions"]["map_tab_over"] == {"kind": "flag", "flag": "xcx.map_mode", "value": 0, "haptic": "light"}, "Overworld tab action")
check(m["actions"]["map_tab_area"]["value"] == 1, "Area tab action")
for key, view in [("over", "xcx-map-over"), ("home", "xcx-map-home"), ("world", "xcx-map-world"), ("nla", "xcx-map-nla"), ("barracks", "xcx-map-barracks")]:
    a = m["actions"][f"find_self_{key}"]
    check(a["kind"] == "view_reset" and a["view"] == view and a["haptic"] == "light", f"find_self_{key}")
    check(any(w.get("on_tap") == f"find_self_{key}" and w.get("need_bind") == f"xcx.mw.{key}" for w in page["widgets"]), f"find self rect {key}")
# the right rail (owner 2026-11-27): a row tap switches to that member (module rail_switch, confirm haptic);
# the rows are outside every map widget and are gated on the module's xcx.rail_tap<i> (another member, no
# finger on the map), so the controlled row and a pan / pinch have no tap target; never the Party page.
check(not any(w.get("on_tap", "").startswith("party_") for w in page["widgets"]), "the rail no longer jumps to the Party page")
derived = {d["name"]: d["expr"] for d in m["derived"]}
rails = [w for w in page["widgets"] if w.get("on_tap", "").startswith("rail_")]
check(len(rails) == 8, f"rail rects {len(rails)}")
for w in rails:
    i = int(w["on_tap"][-1])
    a = m["actions"][w["on_tap"]]
    check(a == {"kind": "module", "action": "rail_switch", "argument": i, "haptic": "confirm"}, f"rail action {i}")
    check(w["rect"][0] >= 860, f"rail rect {i} must not overlap the map widgets")
    check(w["rect"][3] >= 112, f"rail rect {i} height")
    expr = derived.get(w["need_bind"], "")
    check(f"xcx.rail_tap{i}" in expr and "xcx.page == 0" in expr and "xcx.data" in expr, f"rail gate {i}: {expr}")
check('"xcx.rail_tap%d"' in MODULE and '"@view_touch_down:xcx-map-over"' in MODULE, "the module publishes the rail gate")
# the press flash (2026-11-27 rail polish): per row and layout a fading rect (70 ms) and an instant one for
# Reduced motion, over the row inside its tap rect, under the tap rects, taking no touches
flashes = [w for w in page["widgets"] if str(w.get("id", "")).startswith("xcx-rail-press-")]
check(len(flashes) == 48, f"rail press flashes {len(flashes)}")
first_tap = min(order.index(w["id"]) if "id" in w else page["widgets"].index(w) for w in rails)
for w in flashes:
    kind, layout, i = re.match(r"xcx-rail-press-([fr])([sn])(\d)-\d$", w["id"]).groups()
    i = int(i)
    tap = next(r for r in rails if r["on_tap"] == f"rail_{i}" and r["need_bind"] == f"xcx.map_rail_{layout}{i}")
    x, y, ww, hh = w["rect"]
    tx, ty, tw, th = tap["rect"]
    check(tx <= x and x + ww <= tx + tw and ty <= y and y + hh <= ty + th, f"flash {w['id']} outside its row")
    check(page["widgets"].index(w) < page["widgets"].index(tap) and order.index("xcx-page") < page["widgets"].index(w),
          f"flash {w['id']} order")
    check(not any(k in w for k in ("on_tap", "on_hold", "input_block", "tap_block")), f"flash {w['id']} takes touches")
    gate = w["anim"]["bind"] if kind == "f" else w["need_bind"]
    expr = derived.get(gate, "")
    check(f"xcx.rail_press == {i}" in expr and "xcx.page == 0" in expr, f"flash gate {gate}: {expr}")
    check(("!xcx.motion_reduced" in expr) == (kind == "f") and "xcx.motion_reduced" in expr, f"flash motion {gate}")
    if kind == "f":
        check(w["anim"]["from"] == "fade" and 50 <= w["anim"]["ms"] <= 150, f"flash anim {w['id']}")
    else:
        check("anim" not in w, f"reduced flash {w['id']} animates")
check('"xcx.rail_press"' in MODULE, "the module publishes the press row")
check(not any(a in m["actions"] for a in ("zoom_0", "zoom_1", "zoom_2", "find_self")), "old zoom actions are gone")
settings = next(p for p in m["pages"] if p["id"] == "settings")
check(not any(w.get("on_tap", "").startswith("zoom_") for w in settings["widgets"]), "Settings zoom rects are gone")
if fails:
    print("\n".join(fails))
    sys.exit(1)
print("manifest map: all checks passed")
