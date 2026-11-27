#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the non-gameplay states into the XCX package manifest (idempotent; contracts/STATES-1.md).

- Wrong build / module refused: the host never creates the module for a build other than 1.0.2
  (module.build_ids) and publishes module_error / module_error_message instead (PACKAGE_FORMAT 3.2,
  "Failures do not stop the page"). Every page gets the same authored refusal on top, shown only while
  module_error is set: navy backdrop that absorbs touches, a warning mark, the host's reason, the
  supported version, the fix. Host font (the module, and with it the game font, is not running);
  English. Nothing else on the page can be tapped then (the tab/action binds are module values).
- Title / Loading are drawn by the module (xcx_view.cpp TitleArt / LoadingPlate); while they show,
  the module publishes xcx.has_skell = xcx.no_skell = xcx.data = 0, so no tab or action rect is live.
- Settings: the Reduced motion row moved up with the removal of the two "not available yet" rows.

usage: gen_manifest_states.py [manifest.json]
"""
import json
import pathlib
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
PREFIX = "xcx-refuse-"
MOTION_ROW = [40, 536, 1160, 88]  # xcx_view.cpp page 3: the toggle box at y 548..612

NAVY, PINK, CYAN, WHITE, GREY = "#FF0E1321", "#FFF88898", "#FF78F8F8", "#FFF8F8F8", "#FFC8D0DA"


def refusal():
    gate = {"need_bind": "module_error"}
    w = [
        {"type": "rect", "rect": [0, 0, 1240, 1080], "bg": NAVY, "color": "#00000000", "frame": 0,
         "input_block": True},
        # the warning mark: a pink-framed square with an exclamation mark drawn from two rects (design B's
        # hex icon, host-drawn; the host font has no "!" glyph, seen live 2026-11-25)
        {"type": "rect", "rect": [560, 120, 120, 120], "bg": "#00000000", "color": PINK, "frame": 3},
        {"type": "rect", "rect": [610, 142, 20, 52], "bg": PINK, "color": "#00000000", "frame": 0},
        {"type": "rect", "rect": [610, 204, 20, 16], "bg": PINK, "color": "#00000000", "frame": 0},
        {"type": "label", "rect": [620, 320, 0, 0], "text": "Companion not available", "text_scale": 7,
         "color": WHITE, "align": "center"},
        {"type": "rect", "rect": [80, 460, 520, 280], "bg": "#FF141B2C", "color": PINK, "frame": 2},
        {"type": "label", "rect": [108, 486, 0, 0], "text": "Detected", "text_scale": 4, "color": PINK,
         "align": "left"},
        {"type": "label", "rect": [108, 546, 0, 0], "text": "", "bind_text": "module_error_message",
         "text_scale": 4, "color": WHITE, "align": "left", "wrap_width": 464, "max_lines": 4},
        {"type": "rect", "rect": [640, 460, 520, 280], "bg": "#FF141B2C", "color": CYAN, "frame": 2},
        {"type": "label", "rect": [668, 486, 0, 0], "text": "Supported", "text_scale": 4, "color": CYAN,
         "align": "left"},
        {"type": "label", "rect": [668, 566, 0, 0], "text": "Ver. 1.0.2", "text_scale": 7, "color": WHITE,
         "align": "left"},
        {"type": "label", "rect": [620, 800, 0, 0], "text": "Use game version 1.0.2 to use the companion.",
         "text_scale": 5, "color": WHITE, "align": "center"},
        {"type": "label", "rect": [620, 870, 0, 0], "text": "The game keeps running on the main screen.",
         "text_scale": 4, "color": GREY, "align": "center"},
    ]
    for n, x in enumerate(w):
        x["id"] = f"{PREFIX}{n:02d}"
        x.update(gate)
    return w


def main(path):
    path = pathlib.Path(path)
    m = json.loads(path.read_text())
    for page in m["pages"]:
        ws = [w for w in page["widgets"] if not str(w.get("id", "")).startswith(PREFIX)]
        for w in ws:
            if page["id"] == "settings" and w.get("on_tap") == "settings_motion":
                w["rect"] = list(MOTION_ROW)
        page["widgets"] = ws + refusal()  # last = drawn on top of the page picture and its rects
    path.write_text(json.dumps(m, indent=2) + "\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
