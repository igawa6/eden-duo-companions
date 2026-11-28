#!/usr/bin/env python3
"""Automated build-time layout & regression validator for PokemonLegendsZA."""
import json
from pathlib import Path

MANIFEST_PATH = Path(__file__).resolve().parents[2] / "packages/PokemonLegendsZA/dualscreen/manifest.json"
m = json.loads(MANIFEST_PATH.read_text())

# 1. Paged native font configuration
assert m.get("font_page_h") == 1024, f"Expected font_page_h=1024, got {m.get('font_page_h')}"
assert "{p}" in str(m.get("font_atlas", "")), f"Expected paged {{p}} font_atlas, got {m.get('font_atlas')}"

# 2. Settings & flags wiring for Move layout toggle
assert m.get("flags", {}).get("za_move_grid") == 0, "Missing za_move_grid default flag"
assert "za_move_grid" in m.get("persist_flags", []), "za_move_grid must persist"
assert any(s.get("flag") == "za_move_grid" for s in m.get("settings", [])), "Missing Move layout setting"

# 3. Zero scrollbar collisions across all pages
collisions = []
for page in m["pages"]:
    scrolls = {sc["id"]: sc for sc in page.get("scrolls", [])}
    for sid, sc in scrolls.items():
        sx, sy, sw, sh = sc["rect"]
        bar_x0 = sx + sw - sc.get("bar_w", 7)
        for w in page["widgets"]:
            if w.get("scroll") != sid:
                continue
            r = w["rect"]
            if r[2] > 0 and r[0] + r[2] > bar_x0 - 8:
                collisions.append((page["id"], sid, r, bar_x0))
assert not collisions, f"Scrollbar collisions found: {collisions}"

# 4. Critical restored bindings
pages = {p["id"]: p for p in m["pages"]}
prog_binds = {w.get("bind") for w in pages["progress"]["widgets"] if w.get("type") == "value"}
assert "za.research.points" in prog_binds, "Missing numeric za.research.points inside All levels modal!"
team_needs = {w.get("need_bind") for w in pages["team"]["widgets"]}
assert "za.team.grid_mode" in team_needs and "za.team.diam_mode" in team_needs, "Missing 2x2 / ABXY Diamond mode binds!"
assert "za.sel.m0.grid_on" in team_needs and "za.sel.m0.diam_on" in team_needs, "Missing move grid_on/diam_on binds!"

# 5. Native text_center_h active on key badges & tabbar
center_h_count = sum(1 for p in m["pages"] for w in p["widgets"] if w.get("text_center_h", 0) > 0)
assert center_h_count >= 50, f"Expected native text_center_h across dock/badges, got {center_h_count}"

print(f"test_za_layout: PASS (pages={len(m['pages'])}, scroll_collisions=0, text_center_h={center_h_count}, font_page_h=1024)")
