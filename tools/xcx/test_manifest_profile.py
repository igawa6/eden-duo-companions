#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the Party page profile in the XCX manifest (gen_manifest_profile.py; contracts/PARTY-PROFILE-1.md):
the generator is idempotent, the two tab rects meet the 112 px rule and sit inside the detail card, below
the refusal; the Gear list image scrolls in its region with the module's height and reset values."""
import copy
import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json"
spec = importlib.util.spec_from_file_location("gen", pathlib.Path(__file__).with_name("gen_manifest_profile.py"))
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


m = json.loads(MANIFEST.read_text())
check(gen.apply(copy.deepcopy(m)) == m, "generator not idempotent (run gen_manifest_profile.py)")
party = next(p for p in m["pages"] if p["id"] == "party")
ids = [str(w.get("id", "")) for w in party["widgets"]]
tabs = [w for w in party["widgets"] if str(w.get("id", "")).startswith("xcx-profile-tab")]
check(len(tabs) == 2, "two profile tabs")
cx, cy, cw, ch = gen.CARD
for t in tabs:
    x, y, w, h = t["rect"]
    check(w >= 112 and h >= 112, "tab below 112 px")
    check(cx <= x and x + w <= cx + cw and cy <= y and y + h <= cy + ch, "tab outside the card")
    check(t["need_bind"] == "xcx.party_page" and t["on_tap"] in m["actions"], "tab gate / action")
check(tabs[0]["rect"][0] + tabs[0]["rect"][2] <= tabs[1]["rect"][0], "tabs overlap")
for i in range(2):
    a = m["actions"].get("ptab_%d" % i, {})
    check(a.get("kind") == "module" and a.get("action") == "ptab" and a.get("argument") == i, "ptab action")
gear = next((w for w in party["widgets"] if w.get("id") == "xcx-profile-gear"), None)
check(gear is not None and gear["src_bind"] == "xcx.gear" and gear["need_bind"] == "xcx.gear_tab" and
      gear["scroll"] == "xcx-gear" and gear["rect"][2:] == [gen.GEAR_W, gen.GEAR_H], "gear image")
region = next((s for s in party.get("scrolls", []) if s["id"] == "xcx-gear"), None)
check(region is not None and region["content_h_bind"] == "xcx.gear_h" and region["show_bind"] == "xcx.gear_tab" and
      region["reset_bind"] == "xcx.gear_member", "gear region")
if region:
    x, y, w, h = region["rect"]
    check(y >= tabs[0]["rect"][1] + tabs[0]["rect"][3] and y + h <= cy + ch and x >= cx and x + w <= cx + cw,
          "region outside the card body")
refuse = [i for i, s in enumerate(ids) if s.startswith("xcx-refuse-")]
mine = [i for i, s in enumerate(ids) if s.startswith("xcx-profile-")]
check(refuse and mine and max(mine) < min(refuse), "refusal not on top")
# the Set plate and the list rows span the list column (Renderer::ListW) and end before the card
check(any(w.get("on_tap") == "switch_set" and w["rect"] == [24, 756, gen.LIST_W, 112] for w in party["widgets"]), "Set plate")
for i in range(4):
    check(any(w.get("on_tap") == "party_%d" % i and w["rect"] == [24, 24 + 180 * i, gen.LIST_W, 168]
              for w in party["widgets"]), "party row %d" % i)
check(24 + gen.LIST_W + 12 <= cx, "list overlaps the card")
view = (ROOT / "native/modules/xcx_view.h").read_text()
check("GearListW = %d" % gen.GEAR_W in view and "ListW = %d" % gen.LIST_W in view, "generator and view widths differ")
if failures:
    print("\n".join("FAIL: " + f for f in failures))
    sys.exit(1)
print("manifest profile: ok")
