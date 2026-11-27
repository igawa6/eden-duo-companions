#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-gameplay states (contracts/STATES-1.md): the wrong-build refusal on every page, the
Title/Loading tab gating in the module, and the Settings page after the two placeholder rows went."""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
m = json.loads((ROOT / "packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/manifest.json").read_text())
MODULE = (ROOT / "native/modules/0100453019AA8000.cpp").read_text()
VIEW = (ROOT / "native/modules/xcx_view.cpp").read_text()
fails = []


def check(ok, why):
    if not ok:
        fails.append(why)


# The refusal: built for exactly one build; every page carries the same overlay, gated on module_error.
check(m["module"]["build_ids"] == ["2A720C7CE5C84905C1C6847211F670FBB31842B6000000000000000000000000"],
      "module lists exactly build 1.0.2")
check(m["load_plan"].startswith("guest/2A720C7CE5C84905C1C6847211F670FBB31842B6"), "load plan keyed by 1.0.2")
check(m["requires_module"] is True, "requires_module")
for p in m["pages"]:
    ref = [w for w in p["widgets"] if str(w.get("id", "")).startswith("xcx-refuse-")]
    check(len(ref) >= 8, f"{p['id']}: refusal present")
    check(all(w.get("need_bind") == "module_error" for w in ref), f"{p['id']}: refusal gated on module_error")
    check(p["widgets"][-len(ref):] == ref, f"{p['id']}: refusal drawn last (on top)")
    bg = ref[0] if ref else {}
    check(bg.get("rect") == [0, 0, 1240, 1080] and bg.get("input_block") is True,
          f"{p['id']}: refusal backdrop covers the page and absorbs touches")
    texts = " ".join(w.get("text", "") for w in ref)
    check("1.0.2" in texts and any(w.get("bind_text") == "module_error_message" for w in ref),
          f"{p['id']}: supported version and the host's reason")
    check(texts.isascii(), f"{p['id']}: English text")
    # every tap target outside the refusal needs a module value (closed while the module is refused)
    for w in p["widgets"]:
        if w.get("on_tap") or w.get("on_hold"):
            check(w.get("need_bind", "").startswith("xcx."), f"{p['id']}: {w.get('on_tap')} gated on a module value")

# Title / Loading: the module closes both tab sets and every page action (xcx.data) while not live.
check(re.search(r'"xcx\.has_skell", tabs && skell', MODULE) is not None, "has_skell gated on the stage")
check(re.search(r'"xcx\.no_skell", tabs && !skell', MODULE) is not None, "no_skell gated on the stage")
check(re.search(r'"xcx\.data", tabs && data\.valid', MODULE) is not None, "xcx.data gated on the stage")
check("if (game_action && stage != xcx::Stage::Live)\n            return false;" in MODULE,
      "module refuses game actions when not live")
for d in m["derived"]:
    if d["name"] in ("xcx.party_page", "xcx.map_page", "xcx.set_button", "xcx.battle_page"):
        check("xcx.data" in d["expr"], f"{d['name']} needs xcx.data")

# Settings: no placeholder rows; the Reduced motion tap row sits on the drawn toggle (box y 548..612).
check("not available yet" not in VIEW, "no 'not available yet' rows")
check("Follow player on area change" not in VIEW and "Show clock on Map" not in VIEW, "placeholder rows removed")
mot = [w for p in m["pages"] if p["id"] == "settings" for w in p["widgets"] if w.get("on_tap") == "settings_motion"]
check(len(mot) == 1 and mot[0]["rect"][1] <= 548 and mot[0]["rect"][1] + mot[0]["rect"][3] >= 612,
      "motion row covers the toggle")
check("1032, 548" in VIEW, "toggle box at y 548")
# Settings "Companion Ver.": the package version, passed in by CMake from this manifest (no literal).
check(re.fullmatch(r"\d+\.\d+\.\d+", m.get("version", "")) is not None, "manifest version is x.y.z")
check('"Ver. " XCX_COMPANION_VERSION' in VIEW and not re.search(r'"Ver\. 0\.\d+\.\d+"', VIEW),
      "companion version comes from the package")
CMAKE = (ROOT / "native/modules/CMakeLists.txt").read_text()
check("string(JSON XCX_COMPANION_VERSION GET" in CMAKE, "CMake reads the manifest version")

if fails:
    print("\n".join("FAIL: " + f for f in fails))
    sys.exit(1)
print("xcx manifest states OK")
