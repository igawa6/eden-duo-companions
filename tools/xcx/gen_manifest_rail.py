#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Map rail's press flash into the XCX package manifest (idempotent; 2026-11-27 rail polish).

A tap on a party row of the Map page's right rail (module action rail_switch) publishes that row on
xcx.rail_press for 80 ms (accepted or refused). Over each row a translucent white rect then lights the row:
a widget anim fading in and out over 70 ms, or, while the player's Reduced motion flag is set
(xcx.motion_reduced), the same rects shown at once without a fade. Each flash is three rects (a body and a
strip at each side, short of the cut corners) that fill the row plate (x 886..1222) in both rail layouts
(xcx_view.cpp: with a Skell 112 px rows every 122 px, before it 200 px rows every 232 px), above the page picture and below the tap rects; they take no touches.

usage: gen_manifest_rail.py [manifest.json]
"""
import json
import pathlib
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
PREFIX = "xcx-rail-press-"
DERIVED = "xcx.rail_p"
LAYOUTS = {"s": (122, 112, "xcx.has_skell"), "n": (232, 200, "!xcx.has_skell")}
CUT = 18  # RowPlate's chamfer


def pieces(y, h):
    """The plate's area as three rects: the left strip below the top-left cut, the body, the right strip above
    the bottom-right cut."""
    return [[886, y + CUT, CUT - 2, h - CUT - 2], [884 + CUT, y + 2, 340 - 2 * CUT, h - 4],
            [1224 - CUT, y + 2, CUT - 2, h - CUT - 2]]


FLASH_BG = "#33FFFFFF"
FADE_MS = 70


def derived():
    out = []
    for key, (_, _, gate) in LAYOUTS.items():
        for i in range(4):
            base = f"xcx.page == 0 && {gate} && xcx.rail_press == {i}"
            out.append({"name": f"{DERIVED}f_{key}{i}", "expr": base + " && !xcx.motion_reduced"})
            out.append({"name": f"{DERIVED}r_{key}{i}", "expr": base + " && xcx.motion_reduced"})
    return out


def widgets():
    out = []
    for key, (pitch, height, _) in LAYOUTS.items():
        for i in range(4):
            for n, rect in enumerate(pieces(16 + pitch * i, height)):
                out.append({"type": "rect", "rect": rect, "bg": FLASH_BG, "color": "#00000000", "frame": 0,
                            "id": f"{PREFIX}f{key}{i}-{n}",
                            "anim": {"bind": f"{DERIVED}f_{key}{i}", "group": f"{DERIVED}f_{key}{i}", "from": "fade",
                                     "ms": FADE_MS, "easing": "ease_out"}})
                out.append({"type": "rect", "rect": list(rect), "bg": FLASH_BG, "color": "#00000000", "frame": 0,
                            "id": f"{PREFIX}r{key}{i}-{n}", "need_bind": f"{DERIVED}r_{key}{i}"})
    return out


def apply(m):
    page = next(p for p in m["pages"] if p["id"] == "map")
    w = [x for x in page["widgets"] if not str(x.get("id", "")).startswith(PREFIX)]
    at = next(i for i, x in enumerate(w) if x.get("id") == "xcx-page") + 1
    page["widgets"] = w[:at] + widgets() + w[at:]
    m["derived"] = [d for d in m["derived"] if not d["name"].startswith(DERIVED + "f_")
                    and not d["name"].startswith(DERIVED + "r_")] + derived()
    return m


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    m = apply(json.loads(path.read_text()))
    path.write_text(json.dumps(m, indent=2) + "\n")


if __name__ == "__main__":
    main()
