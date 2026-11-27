#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Battle tab (design B-Battle-2): 5-slot tab bar on every page, the Battle page's tap rects
against the module's drawing code, touch sizes, and the Battle Commands button-press fallback."""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
m = json.loads((ROOT / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json").read_text())
MODULE = (ROOT / "native/modules/0100453019AA8000.cpp").read_text()
VIEW = (ROOT / "native/modules/xcx_view.cpp").read_text()
BATTLE = (ROOT / "native/modules/xcx_view_battle.cpp").read_text()
fails = []


def check(ok, why):
    if not ok:
        fails.append(why)


TABS = {("xcx.has_skell", (16, 964, 249, 112), "page_0"), ("xcx.has_skell", (277, 964, 249, 112), "page_1"),
        ("xcx.has_skell", (538, 964, 249, 112), "page_2"), ("xcx.has_skell", (799, 964, 249, 112), "page_4"),
        ("xcx.has_skell", (1060, 964, 164, 112), "page_3"),
        ("xcx.no_skell", (16, 964, 336, 112), "page_0"), ("xcx.no_skell", (364, 964, 336, 112), "page_1"),
        ("xcx.no_skell", (712, 964, 336, 112), "page_4"), ("xcx.no_skell", (1060, 964, 164, 112), "page_3")}
for p in m["pages"]:
    tabs = {(w.get("need_bind"), tuple(w["rect"]), w.get("on_tap")) for w in p["widgets"]
            if w.get("rect", [0, 0])[1] == 964 and str(w.get("on_tap", "")).startswith("page_")}
    check(tabs == TABS, f"{p['id']}: tab rects {sorted(tabs - TABS)} / missing {sorted(TABS - tabs)}")
# the drawn tab bar: same order and widths
check("order{0, 1, 2, 4, 3}" in VIEW and "p == 3 ? 164 : skell ? 249 : 336" in VIEW and "x += width + 12" in VIEW,
      "xcx_view.cpp Tabs: order Map Party Skell Battle gear, widths 249/336/164, gap 12")

battle = next(p for p in m["pages"] if p["id"] == "battle")
check(m["pages"].index(battle) == 4, "Battle is module page 4")
taps = [w for w in battle["widgets"] if "on_tap" in w and not w["on_tap"].startswith("page_")]
by = {w["on_tap"]: w for w in taps}
cards = [by.get(f"bpick_{i}") for i in range(4)]
check(all(cards), "four card rects")
cx = re.search(r"CardX\{(\d+), (\d+)\}", BATTLE)
cy = re.search(r"CardY\{(\d+), (\d+)\}", BATTLE)
cw = re.search(r"CardW = (\d+), CardH = (\d+)", BATTLE)
if cx and cy and cw and all(cards):
    for i, w in enumerate(cards):
        want = [int(cx.group(1 + i % 2)), int(cy.group(1 + i // 2)), int(cw.group(1)), int(cw.group(2))]
        check(w["rect"] == want, f"card {i} rect {w['rect']} != drawn {want}")
        check(w["need_bind"] == "xcx.battle_page", f"card {i} gate")
else:
    check(False, "card geometry constants in xcx_view_battle.cpp")
for action, panel in (("battle_commands", "Panel(image, h, 452, 696, 432, 112)"),
                      ("switch_set", "Panel(image, h, 24, 820, 420, 112)")):
    check(action in by and panel in BATTLE, f"{action}: rect and drawn plate")
    if action in by:
        nums = [int(n) for n in re.findall(r"\d+", panel)]
        check(by[action]["rect"] == nums, f"{action} rect matches the drawn plate")
check("Panel(image, h, 900, 820, 308, 112)" in VIEW and by.get("skell_button", {}).get("rect") == [900, 820, 308, 112],
      "Skell button rect = drawn plate, same place as Map / Skell")
check(by.get("skell_button", {}).get("on_hold") == "return_hold", "Return hold on the Battle page's Skell button")
# touch sizes and no overlapping hit rects (the tabs are below y 960)
for w in taps:
    x, y, ww, hh = w["rect"]
    check(ww >= 112 and hh >= 112, f"{w['on_tap']}: hit rect >= 112")
    check(y + hh <= 960, f"{w['on_tap']}: above the tab bar")
for i, a in enumerate(taps):
    for b in taps[i + 1:]:
        ax, ay, aw, ah = a["rect"]
        bx, by_, bw, bh = b["rect"]
        check(ax + aw <= bx or bx + bw <= ax or ay + ah <= by_ or by_ + bh <= ay,
              f"{a['on_tap']} overlaps {b['on_tap']}")
# gates: every need_bind is derived or published
derived = {d["name"] for d in m["derived"]}
for w in battle["widgets"]:
    n = w.get("need_bind")
    if n:
        check(n in derived or n == "module_error" or f'"{n}"' in MODULE, f"gate {n} exists")  # module_error: host point
check({"xcx.battle_page", "xcx.battle_set_button", "xcx.battle_skell_button", "xcx.battle_cmd_button"} <= derived,
      "battle derived gates")
# actions: the selection stays on the page; Battle Commands = one + press, only with input
acts = m["actions"]
for i in range(4):
    check(acts[f"bpick_{i}"] == {"kind": "module", "action": "bpick", "argument": i, "haptic": "light"}, f"bpick_{i}")
check('a == "bpick"' in MODULE, "module handles bpick")
bc = acts.get("battle_commands", {})
check(bc.get("kind") == "button" and bc.get("button") == "Plus" and 2 <= bc.get("frames", 0) <= 10,
      "battle_commands presses + for a few ticks")
check(bc.get("enabled_bind") == "xcx.battle_cmd_ok" and '"xcx.battle_cmd_ok"' in MODULE,
      "battle_commands gated on the published input-ready value")
buttons = [k for k, v in acts.items() if v.get("kind") == "button"]
check(buttons == ["battle_commands"], f"the only input-substitute action is Battle Commands: {buttons}")
check(acts["page_4"] == {"kind": "module", "action": "page", "argument": 4, "haptic": "light"}, "page_4 action")
check("arg <= 4" in MODULE, "module accepts page 4")
if fails:
    print("\n".join(fails))
    sys.exit(1)
print("manifest battle: all checks passed")
