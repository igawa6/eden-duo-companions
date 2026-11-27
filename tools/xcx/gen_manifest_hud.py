#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the top-screen HUD toggles into the XCX package manifest (idempotent).

Settings page rows (module picture, native/modules/xcx_view.cpp page 3):
- "Hide minimap on top screen"      -> flag xcx.hud_hide_map  (row rect y 116, 88 tall)
- "Hide Skell fuel gauge on top"    -> flag xcx.hud_hide_fuel (row rect y 228, 88 tall)
Both default to 1 (hidden while the companion runs: DESIGN-TASTE "HUD on top" rule) and persist.
A tap cycles the flag 0/1; the module reads it live (@flag:...) and applies or restores the
game's own Display Settings value (contracts/OPEN-SKELL-HUD.md).

usage: gen_manifest_hud.py [manifest.json]
"""
import json
import pathlib
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
ROWS = [("settings_hud_map", "xcx.hud_hide_map", 116), ("settings_hud_fuel", "xcx.hud_hide_fuel", 228)]


def main(path):
    path = pathlib.Path(path)
    m = json.loads(path.read_text())
    for action, flag, _ in ROWS:
        m["actions"][action] = {"kind": "flag", "flag": flag, "cycle": 2, "haptic": "click"}
        m["flags"][flag] = 1
        if flag not in m["persist_flags"]:
            m["persist_flags"].append(flag)
    for page in m["pages"]:
        if page["id"] != "settings":
            continue
        ws = [w for w in page["widgets"] if w.get("on_tap") not in [a for a, _, _ in ROWS]]
        for action, _, y in ROWS:
            ws.append({"type": "rect", "rect": [40, y, 1160, 88], "bg": "#00000000", "color": "#00000000",
                       "frame": 0, "on_tap": action, "need_bind": "xcx.settings_page"})
        page["widgets"] = ws
    path.write_text(json.dumps(m, indent=2) + "\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
