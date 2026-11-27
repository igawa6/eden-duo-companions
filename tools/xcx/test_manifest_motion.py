# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Motion and haptics contract of the XCX companion manifest (WORKFLOW step 9b).

Checks the shipped manifest against the rules of PACKAGE_FORMAT.md and the runtime parser
(ParseHaptics, ParsePageBindTarget, flags/persist_flags), without a runtime:
  - five manifest pages (Map, Party, Skell, Settings, Battle) in the order of the module's page
    numbers, each with the module-drawn picture and only its own tap rects;
  - page_binds: a 180 ms fade set and an instant set (reduced motion), one entry per page each;
  - the Reduced motion flag is persisted and a Settings row flips it;
  - haptics: respect_system, valid kinds and strengths, hold/refused set; the two HUD-on-top toggles (hidden by default);
  - nothing loops: no widget animation groups (idle cost rule), except the Map rail's press flash
    (xcx-rail-press-*: a 70 ms fade gated on the module's tap window, closed at rest).
"""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[2]
manifest_path = root / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
m = json.loads(manifest_path.read_text())
derived_expr = {d["name"]: d["expr"] for d in m.get("derived", [])}
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


KINDS = {"tap", "write", "select", "drag", "drop", "marker", "refused", "hold", "swipe", "nav"}
STRENGTHS = {"off", "light", "click", "confirm", "heavy", "reject"}
TRANSITIONS = {"slide_up", "slide_down", "grow", "shrink", "fade", "none"}
EASINGS = {"linear", "ease_in", "ease_out", "ease_in_out"}

check(m.get("format") == 1 and m.get("min_runtime", 0) >= 25, "format 1, min_runtime >= 25")
check(m.get("anim_hz") == 60, "anim_hz 60")

h = m.get("haptics", {})
check(h.get("enabled") is True and h.get("respect_system") is True,
      "haptics enabled with respect_system (the off switch is Android's system touch feedback)")
for k, v in h.items():
    if k in ("enabled", "respect_system"):
        continue
    check(k in KINDS, f"haptics kind {k}")
    check(v in STRENGTHS, f"haptics strength {v}")
check(h.get("hold") == "heavy" and h.get("refused") == "reject" and h.get("write") == "confirm",
      "hold heavy, refused reject, write confirm")
check(h.get("tap") in ("light", "click"), "tap is subtle")

actions = m["actions"]
for name, a in actions.items():
    if "haptic" in a:
        check(a["haptic"] in STRENGTHS, f"action {name} haptic")
check(actions["return_hold"].get("haptic") is None,
      "return_hold: an action haptic never applies to a hold, haptics.hold does")
check(actions["skell_button"].get("haptic") == "confirm" and actions["switch_set"].get("haptic") == "confirm",
      "submitted native actions play confirm")
for n in range(5):
    check(actions[f"page_{n}"]["action"] == "page" and actions[f"page_{n}"]["argument"] == n,
          f"page_{n} action argument")

# Pages, in the module's page order.
ids = [p["id"] for p in m["pages"]]
check(ids == ["map", "party", "skell", "settings", "battle"], "pages in module order")
check(len(set(ids)) == 5, "unique page ids")
cw, ch = m["canvas_w"], m["canvas_h"]
tab_actions = {"page_0", "page_1", "page_2", "page_3", "page_4"}
for p in m["pages"]:
    # Scrolled pictures (the Party page's Gear list, test_manifest_profile.py) live in a scroll region.
    scrolled = [w for w in p["widgets"] if w["type"] == "image" and "scroll" in w]
    images = [w for w in p["widgets"] if w["type"] == "image" and "scroll" not in w]
    check(len(images) == 1 and images[0]["src_bind"] == "xcx.frame" and
          images[0]["rect"] == [0, 0, cw, ch], f"{p['id']}: the module picture")
    # The Map page's native map widgets (tools/xcx/test_manifest_map.py) are drawn under the picture.
    maps = [w for w in p["widgets"] if w["type"] == "map"]
    check(p["widgets"][len(maps)] is images[0] and all(w["type"] == "map" for w in p["widgets"][:len(maps)]),
          f"{p['id']}: picture drawn first (after any native maps)")
    check(not maps or p["id"] == "map", f"{p['id']}: maps only on the Map page")
    for w in p["widgets"]:
        x, y, ww, hh = w["rect"]
        if w in scrolled:  # taller than the canvas by design; its region must be inside it
            region = next(r for r in p.get("scrolls", []) if r["id"] == w["scroll"])
            x, y, ww, hh = region["rect"]
        check(0 <= x and 0 <= y and x + ww <= cw and y + hh <= ch, f"{p['id']}: rect in canvas")
        for key in ("on_tap", "on_hold"):
            if key in w:
                check(w[key] in actions, f"{p['id']}: {key} {w[key]} exists")
        press = p["id"] == "map" and str(w.get("id", "")).startswith("xcx-rail-press-")
        check("anim" not in w or (press and w["anim"]["ms"] <= 150 and "xcx.rail_press ==" in derived_expr.get(w["anim"]["bind"], "")),
              f"{p['id']}: no widget animation (nothing loops at rest)")
    taps = {w.get("on_tap") for w in p["widgets"]}
    check(tab_actions <= taps, f"{p['id']}: all five tabs")
    check("no_auto_leave" not in p, f"{p['id']}: reachable by page_binds")
by_id = {p["id"]: {w.get("on_tap") for w in p["widgets"]} for p in m["pages"]}
by_page_widgets = {p["id"]: p["widgets"] for p in m["pages"]}
check({"settings_motion"} <= by_id["settings"] and not {"zoom_0", "zoom_1", "zoom_2"} & by_id["settings"],
      "settings rows (the zoom row is gone: pinch replaces it)")
check("switch_set" in by_id["party"] and "switch_set" in by_id["battle"] and "switch_set" not in by_id["map"],
      "Set plate on Party and Battle only")
check({"find_self_over", "find_self_home", "find_self_world", "find_self_nla", "find_self_barracks"} <= by_id["map"],
      "Find Self on Map, one per native map")
check({"map_tab_over", "map_tab_area"} <= by_id["map"], "mode tabs on Map")
check(all("skell_button" in by_id[p] for p in ("map", "skell", "battle")), "Skell button on Map, Skell and Battle")
holds = {p["id"] for p in m["pages"] if any(w.get("on_hold") == "return_hold" for w in p["widgets"])}
check(holds == {"map", "skell", "battle"}, "Return hold on Map, Skell and Battle")

# page_binds.
binds = m["page_binds"]
check(len(binds) == 15, "fifteen page_binds")
for point, kind in (("xcx.pg_fade", "fade"), ("xcx.pg_cut", "none"), ("xcx.pg_auto", "fade")):
    mine = [b for b in binds if b["point"] == point]
    check(sorted(b["equals"] for b in mine) == [0, 1, 2, 3, 4], f"{point}: one bind per page")
    for b in mine:
        t = b["when_equal"]
        check(t["page"] == ids[b["equals"]], f"{point}={b['equals']} targets {ids[b['equals']]}")
        check(t.get("transition") == kind, f"{point}: transition {kind}")
        check(t["transition"] in TRANSITIONS and t.get("easing", "ease_out") in EASINGS, "valid names")
        d = t.get("duration_ms", 0)
        check(0 <= d <= 2000, "duration in runtime range")
        if point == "xcx.pg_auto":
            check(d == 250, "automatic page change fades 250 ms (BATTLE-UX s1 A)")
        elif kind == "fade":
            check(150 <= d <= 300, "page fade 150-300 ms")
        else:
            check(d == 0, "cut is instant")
        check("when_not_equal" not in b, "one-directional bind")

# Reduced motion flag.
check(m.get("flags", {}).get("motion.reduced") == 0, "motion.reduced defaults to 0")
check("motion.reduced" in m.get("persist_flags", []), "motion.reduced persisted")
sm = actions["settings_motion"]
check(sm["kind"] == "flag" and sm["flag"] == "motion.reduced" and sm.get("cycle") == 2,
      "settings_motion flips the flag")
check("settings" not in m, "the module draws the Settings page; no generated @settings page")
# HUD on top (owner 2026-11-23, now in scope): two persisted toggles, hidden by default, one row each.
for action, flag, y in (("settings_hud_map", "xcx.hud_hide_map", 116), ("settings_hud_fuel", "xcx.hud_hide_fuel", 228)):
    a = actions.get(action, {})
    check(a.get("kind") == "flag" and a.get("flag") == flag and a.get("cycle") == 2, f"{action} flips {flag}")
    check(m.get("flags", {}).get(flag) == 1 and flag in m.get("persist_flags", []), f"{flag} defaults to 1, persisted")
    check(any(w.get("on_tap") == action and w.get("rect") == [40, y, 1160, 88] for w in by_page_widgets["settings"]),
          f"{action} row on the Settings page")

if failures:
    for f in failures:
        print("FAIL:", f, file=sys.stderr)
    sys.exit(1)
print("xcx manifest motion contract OK")
