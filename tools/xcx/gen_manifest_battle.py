#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Battle tab into the XCX package manifest (idempotent).

- the 5-slot tab bar on every page: Map / Party / Skell / Battle 249 wide + gear 164 (with a Skell),
  Map / Party / Battle 336 wide + gear (before a Skell is revealed); gaps 12, y 964, 112 tall;
- the Battle page (module page 4, manifest page index 4): the module picture, four card rects
  (select), the Set plate, the Skell button (tap + hold) and the Battle Commands plate;
- the Battle Commands action: a press of the game's + button (owner-approved fallback, the only
  input-substitute action in this companion), offered only while xcx.battle_cmd_ok is set;
- page_binds for page 4 (180 ms fade, instant set under Reduced motion);
- page_binds on xcx.pg_auto for every page (250 ms fade): the module's automatic Battle view
  (BATTLE-UX s1 option A) enters and leaves through them, without a haptic.

The rectangles must match native/modules/xcx_view.cpp (Tabs) and xcx_view_battle.cpp; the test
tools/xcx/test_manifest_battle.py checks both sides.

usage: gen_manifest_battle.py [manifest.json]
"""
import json
import pathlib
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"

TABS_SKELL = [(16, 249, 0), (277, 249, 1), (538, 249, 2), (799, 249, 4), (1060, 164, 3)]
TABS_NO_SKELL = [(16, 336, 0), (364, 336, 1), (712, 336, 4), (1060, 164, 3)]
CARDS = [(16, 16), (628, 16), (16, 252), (628, 252)]
CARD_W, CARD_H = 596, 216
SET_RECT = [24, 820, 420, 112]
SKELL_RECT = [900, 820, 308, 112]
COMMANDS_RECT = [452, 696, 432, 112]
PAGE_IDS = ["map", "party", "skell", "settings", "battle"]


def rect(r, **kw):
    w = {"type": "rect", "rect": list(r), "bg": "#00000000", "color": "#00000000", "frame": 0}
    w.update(kw)
    return w


def tab_rects():
    out = [rect((x, 964, w, 112), on_tap=f"page_{p}", need_bind="xcx.has_skell") for x, w, p in TABS_SKELL]
    out += [rect((x, 964, w, 112), on_tap=f"page_{p}", need_bind="xcx.no_skell") for x, w, p in TABS_NO_SKELL]
    return out


def is_tab(w):
    return w.get("type") == "rect" and w.get("rect", [0, 0])[1] == 964 and str(w.get("on_tap", "")).startswith("page_")


def main(path):
    path = pathlib.Path(path)
    m = json.loads(path.read_text())
    a = m["actions"]
    a["page_4"] = {"kind": "module", "action": "page", "argument": 4, "haptic": "light"}
    for i in range(4):
        a[f"bpick_{i}"] = {"kind": "module", "action": "bpick", "argument": i, "haptic": "light"}
    a["battle_commands"] = {"kind": "button", "button": "Plus", "frames": 6, "enabled_bind": "xcx.battle_cmd_ok",
                            "haptic": "light"}
    derived = {d["name"]: d for d in m["derived"]}
    for name, expr in (("xcx.battle_page", "xcx.page == 4 && xcx.data"),
                       ("xcx.battle_set_button", "xcx.page == 4 && xcx.data && xcx.helper"),
                       ("xcx.battle_skell_button", "xcx.page == 4 && xcx.data && xcx.has_skell && xcx.helper"),
                       ("xcx.battle_cmd_button", "xcx.page == 4 && xcx.data && xcx.battle_cmd_ok")):
        if name in derived:
            derived[name]["expr"] = expr
        else:
            m["derived"].append({"name": name, "expr": expr})
    pages = {p["id"]: p for p in m["pages"]}
    battle = pages.get("battle") or {"id": "battle", "name": "Battle", "widgets": []}
    battle["widgets"] = [{"id": "xcx-page", "type": "image", "rect": [0, 0, 1240, 1080], "src_bind": "xcx.frame",
                          "tint": "#FFFFFFFF"}]
    battle["widgets"] += [rect((x, y, CARD_W, CARD_H), on_tap=f"bpick_{i}", need_bind="xcx.battle_page")
                          for i, (x, y) in enumerate(CARDS)]
    battle["widgets"].append(rect(COMMANDS_RECT, on_tap="battle_commands", need_bind="xcx.battle_cmd_button"))
    battle["widgets"].append(rect(SET_RECT, on_tap="switch_set", need_bind="xcx.battle_set_button"))
    battle["widgets"].append(rect(SKELL_RECT, on_tap="skell_button", on_hold="return_hold", hold_ms=600,
                                  need_bind="xcx.battle_skell_button"))
    pages["battle"] = battle
    m["pages"] = [pages[i] for i in PAGE_IDS]
    for p in m["pages"]:
        widgets = [w for w in p["widgets"] if not is_tab(w)]
        # tabs right after the page picture (and any native maps under it), as before
        at = next(i for i, w in enumerate(widgets) if w.get("id") == "xcx-page") + 1
        p["widgets"] = widgets[:at] + tab_rects() + widgets[at:]
    binds = [b for b in m["page_binds"] if b["when_equal"]["page"] != "battle" and b["point"] != "xcx.pg_auto"]
    binds.append({"point": "xcx.pg_fade", "equals": 4,
                  "when_equal": {"page": "battle", "transition": "fade", "duration_ms": 180, "easing": "ease_out"}})
    binds.append({"point": "xcx.pg_cut", "equals": 4, "when_equal": {"page": "battle", "transition": "none", "duration_ms": 0}})
    for n, page in enumerate(PAGE_IDS):
        binds.append({"point": "xcx.pg_auto", "equals": n,
                      "when_equal": {"page": page, "transition": "fade", "duration_ms": 250, "easing": "ease_out"}})
    order = {"xcx.pg_fade": 0, "xcx.pg_cut": 1, "xcx.pg_auto": 2}
    binds.sort(key=lambda b: (order[b["point"]], b["equals"]))
    m["page_binds"] = binds
    path.write_text(json.dumps(m, indent=2) + "\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
