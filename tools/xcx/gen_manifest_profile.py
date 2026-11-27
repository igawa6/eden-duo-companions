#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the Party page profile into the XCX package manifest (idempotent; contracts/PARTY-PROFILE-1.md).

Owner feedback 2026-11-27 item 2: the detail card of the Party page becomes the member's profile with two
tabs, Overview and Gear (xcx_view.cpp PartyProfile). The module draws both into the page picture; the Gear
list is its own picture (module key published as xcx.gear, height xcx.gear_h) that the host scrolls in the
region xcx-gear over the card body. The scroll position resets when the selected member changes
(xcx.gear_member). The two tab rects are 362 x 112 (the 112 px primary-control rule) and only exist on
the live Party page. Everything is inserted before the wrong-build refusal, which stays on top.
Round 2 (2026-11-27): the member list narrows to 420 px (Renderer::ListW) so the card is 748 wide and the
native stats table fits one row per stat; the list row and Set plate rects follow (LIST_W).

usage: gen_manifest_profile.py [manifest.json]
"""
import json
import pathlib
import sys

DEFAULT = pathlib.Path(__file__).resolve().parents[2] / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
PREFIX = "xcx-profile-"
CARD = (468, 24, 748, 844)
TAB_H = 112
BODY = (468, 148, 748, 708)  # the card body below the tabs, to 12 px above the card's bottom edge
TABS = [(480, 24, 362, 112), (842, 24, 362, 112)]
GEAR_W, GEAR_H = 748, 1716   # Renderer::GearListW / GearListH
LIST_W = 420                 # Renderer::ListW: the party rows (24, 24 + 180 i, LIST_W, 168) and the Set plate


def widgets():
    out = []
    for i, r in enumerate(TABS):
        out.append({"type": "rect", "rect": list(r), "bg": "#00000000", "color": "#00000000", "frame": 0,
                    "on_tap": "ptab_%d" % i, "need_bind": "xcx.party_page", "id": PREFIX + "tab%d" % i})
    out.append({"type": "image", "rect": [BODY[0], BODY[1], GEAR_W, GEAR_H], "src_bind": "xcx.gear",
                "need_bind": "xcx.gear_tab", "repeat": 1, "scroll": "xcx-gear", "id": PREFIX + "gear"})
    return out


def region():
    return {"id": "xcx-gear", "rect": list(BODY), "content_h_bind": "xcx.gear_h", "show_bind": "xcx.gear_tab",
            "reset_bind": "xcx.gear_member", "bar": "#FF78F8F8", "bar_track": "#33FFFFFF", "bar_w": 6}


def apply(m):
    party = next(p for p in m["pages"] if p["id"] == "party")
    w = [x for x in party["widgets"] if not str(x.get("id", "")).startswith(PREFIX)]
    at = next((i for i, x in enumerate(w) if str(x.get("id", "")).startswith("xcx-refuse-")), len(w))
    for x in w:  # the member list rows and the Set plate keep their x / y / height, narrowed to the list
        if x.get("on_tap") in ("party_0", "party_1", "party_2", "party_3", "switch_set") and x["rect"][0] == 24:
            x["rect"][2] = LIST_W
    party["widgets"] = w[:at] + widgets() + w[at:]
    party["scrolls"] = [s for s in party.get("scrolls", []) if s.get("id") != "xcx-gear"] + [region()]
    for i in range(2):
        m["actions"]["ptab_%d" % i] = {"kind": "module", "action": "ptab", "argument": i, "haptic": "light"}
    return m


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    m = apply(json.loads(path.read_text()))
    path.write_text(json.dumps(m, indent=2) + "\n")


if __name__ == "__main__":
    main()
