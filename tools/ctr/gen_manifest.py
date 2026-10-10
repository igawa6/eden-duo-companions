#!/usr/bin/env python3
"""Writes packages/CrashTeamRacingNitroFueled/dualscreen/manifest.json (owner-approved layout A:
ranking column with names, track minimap with racer heads, big rank, LAP and the race timer;
track-theme background). Picture sizes come from native/modules/ctr_art.h, which composes them.

Pages:
- "main": the race page, with layers over it instead of page binds (binds fire only on edges, and
  a race starting while @settings is open must still show on BACK): "Waiting for Race" with the
  settings cog outside races, the module's "update required" picture on a known older release,
  and a host-font notice when no module instance runs (an unknown build or a load error).
- "@settings" (the package's own, in the game's Options look): five SHOW/HIDE switches for the
  game's own top-screen HUD parts, default HIDE, persisted. Opened by holding the screen or the
  cog; flag-gated enforce entries re-send each switch to the module twice a second."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ART_H = (ROOT / "native/modules/ctr_art.h").read_text()
C = {k: int(v) for k, v in re.findall(r"\b([A-Z][A-Za-z]+) = (\d+)\b", ART_H)}

W, H = C["CanvasW"], C["CanvasH"]
ROW_Y, ROW_DY = C["RankRowY"], C["RankRowDy"]


def timer():
    """Stopwatch + "M:SS:CC" in fixed character slots tm.c0..c7 (right aligned; c0 only from
    10:00:00, when the stopwatch moves left of it)."""
    widths = [C["TimeDigitW"], C["TimeDigitW"], C["TimeColonW"], C["TimeDigitW"], C["TimeDigitW"],
              C["TimeColonW"], C["TimeDigitW"], C["TimeDigitW"]]
    right, y, icon = 962, 22, C["TimerIconPx"]
    xs, x = [], right
    for w in reversed(widths):
        x -= w
        xs.insert(0, x)
    # the digits' ink sits low in their boxes (bottom aligned): lift them so their middle meets
    # the stopwatch's (icon rows y+2 .. y+2+icon)
    digit_y = y - 17
    out = [
        {"type": "image", "rect": [xs[1] - icon - 6, y + 2, icon, icon], "src": "module:ctr:timer",
         "need_bind": "tm.v", "hide_bind": "tm.c0v", "keep_max": 0},
        {"type": "image", "rect": [xs[0] - icon - 6, y + 2, icon, icon], "src": "module:ctr:timer",
         "need_bind": "tm.c0v"},
    ]
    for k, (cx, w) in enumerate(zip(xs, widths)):
        # tm.c<k>v exists only while the timer shows (the published values are rebuilt each tick)
        out.append({"type": "image", "rect": [cx, digit_y, w, C["TimeH"]], "src_bind": f"tm.c{k}",
                    "need_bind": f"tm.c{k}v"})
    return out


def rows(widget):
    """A position slot's widget, one per row."""
    return dict(widget, repeat=8, repeat_dy=ROW_DY)


def cards(widget):
    """A racer card's widget (rc0..7 in draw order), placed at row 0 and moved by the module's
    published slide offset."""
    return dict(widget, repeat=8, x_bind="rc{i}.x", y_bind="rc{i}.y")


race = [
    {"type": "image", "rect": [0, 0, W, H], "src_bind": "ctr.bg"},
    {"type": "image", "rect": [C["MapBoxX"], C["MapBoxY"], C["MapBoxW"], C["MapBoxH"]], "src_bind": "ctr.map"},
    # ranking column: racer cards (the player's on its white tab) slide between the rows when
    # positions change; the position numbers stay in their rows, over the cards
    cards({"type": "image", "rect": [14, ROW_Y + 10, 450, 112], "src": "module:ctr:tab", "need_bind": "rc{i}.me"}),
    cards({"type": "image", "rect": [40, ROW_Y, 118, 118], "src_bind": "rc{i}.p", "need_bind": "rc{i}.v"}),
    cards({"type": "image", "rect": [176, ROW_Y + 23, C["NameW"], C["NameH"]], "src_bind": "rc{i}.n",
           "need_bind": "rc{i}.v"}),
    cards({"type": "image", "rect": [128, ROW_Y + 78, 48, 48], "src": "module:ctr:flag", "need_bind": "rc{i}.fin"}),
    rows({"type": "image", "rect": [16, ROW_Y + 52, C["DigitW"], C["DigitH"]], "src": "module:ctr:digit:w:{i+1}",
          "need_bind": "rw{i}.v", "hide_bind": "rw{i}.me", "keep_max": 0}),
    rows({"type": "image", "rect": [16, ROW_Y + 52, C["DigitW"], C["DigitH"]], "src": "module:ctr:digit:g:{i+1}",
          "need_bind": "rw{i}.me"}),
    # map markers (positions are canvas pixels published by the module), the player's on top
    {"type": "image", "rect": [0, 0, C["HeadPx"], C["HeadPx"]], "x_bind": "mk{i}.x", "y_bind": "mk{i}.y",
     "src_bind": "mk{i}.h", "need_bind": "mk{i}.v", "repeat": 8},
    {"type": "image", "rect": [0, 0, C["PlayerMarkerPx"], C["PlayerMarkerPx"]], "x_bind": "pm.x", "y_bind": "pm.y",
     "src_bind": "pm.h", "need_bind": "pm.v"},
    {"type": "image", "rect": [W - C["RankW"] - 24, H - C["RankH"] - 14, C["RankW"], C["RankH"]],
     "src_bind": "ctr.rank", "need_bind": "ctr.mev"},
    {"type": "image", "rect": [W - C["LapW"] - 24, 18, C["LapW"], C["LapH"]], "src_bind": "ctr.lap",
     "need_bind": "ctr.lapv"},
] + timer()

# the whole canvas holds for the settings (drawn first: no picture covers a hold target)
hold = {"type": "rect", "rect": [0, 0, W, H], "color": "#00000000", "on_hold": "open_settings", "hold_ms": 700}
cog = C["CogPx"]
not_ready = "!module_ready"
main = [hold] + race + [
    {"type": "image", "rect": [0, 0, W, H], "src": "module:ctr:loading", "need_bind": "!ctr.race"},
    # outside races: the settings button (also the controller's focus target on this page)
    {"id": "cog", "type": "image", "rect": [W - cog - 34, 34, cog, cog], "src": "module:ctr:cog",
     "need_bind": "!ctr.race", "on_tap": "open_settings"},
    {"type": "image", "rect": [0, 0, W, H], "src_bind": "ctr.wrong", "need_bind": "ctr.wrongv", "input_block": True},
    {"type": "rect", "rect": [0, 0, W, H], "bg": "#FF0A46A0", "color": "#00000000", "need_bind": not_ready,
     "input_block": True},
    {"type": "label", "rect": [W // 2, 400, 0, 90], "text": "UPDATE REQUIRED", "align": "center",
     "text_scale": 7, "color": "#FFFFCE2E", "need_bind": not_ready},
    {"type": "label", "rect": [W // 2, 530, 0, 50], "text": "This companion needs game version 1.0.15.",
     "align": "center", "text_scale": 4, "color": "#FFFFFFFF", "need_bind": not_ready},
    {"type": "label", "rect": [W // 2, 600, 0, 50], "text": "The installed version is not supported.",
     "align": "center", "text_scale": 4, "color": "#FFFFFFFF", "need_bind": not_ready},
]

# owner settings: the game's top-screen HUD parts (module action, flag), hidden by default.
# Order = the module's set:row:<i> labels (ctr_art.cpp).
HUD = [("hud_lead", "hud.lead"), ("hud_pos", "hud.pos"), ("hud_map", "hud.map"), ("hud_lap", "hud.lap"),
       ("hud_time", "hud.time")]
SET_X, SET_Y, SET_DY = 170, 100, 165
settings_page = [{"type": "image", "rect": [0, 0, W, H], "src": "module:ctr:set:bg"}]
for i, (_, f) in enumerate(HUD):
    rect = [SET_X, SET_Y + i * SET_DY, C["SetRowW"], C["SetRowH"]]
    settings_page += [
        {"type": "image", "rect": rect, "src": f"module:ctr:set:row:{i}:1", "need_bind": f"@flag:{f}"},
        {"type": "image", "rect": rect, "src": f"module:ctr:set:row:{i}:0", "need_bind": f"!@flag:{f}"},
        {"id": f"set_{i}", "type": "rect", "rect": [SET_X + 100, SET_Y + i * SET_DY + 30, 680, 165],
         "color": "#00000000", "on_tap": f"toggle_{f}"},
    ]
# BACK: the footer's right end (and the title row)
settings_page += [
    {"id": "set_back", "type": "rect", "rect": [900, H - 96, W - 900, 96], "color": "#00000000",
     "on_tap": "close_settings"},
    {"type": "rect", "rect": [0, 0, W, 100], "color": "#00000000", "on_tap": "close_settings"},
]

actions = {"open_settings": {"kind": "page", "page": "@settings", "transition": "fade", "duration_ms": 200},
           "close_settings": {"kind": "page", "page": "main", "transition": "fade", "duration_ms": 200}}
enforce = []
for a, f in HUD:
    actions[f"toggle_{f}"] = {"kind": "flag", "flag": f}
    # a module action's argument is a constant (or $payload): a hide and a show action per switch,
    # re-sent while the flag holds that value
    actions[f"{a}_on"] = {"kind": "module", "action": a, "argument": 1}
    actions[f"{a}_off"] = {"kind": "module", "action": a, "argument": 0}
    enforce += [{"action": f"{a}_on", "every_ms": 500, "flag": f, "value": True},
                {"action": f"{a}_off", "every_ms": 500, "flag": f, "value": False}]

manifest = {
    "format": 1,
    "title_id": "0100F9F00C696000",
    "name": "Crash Team Racing Nitro-Fueled dual screen",
    "min_runtime": 18,
    "requires_module": True,
    "canvas_w": W,
    "canvas_h": H,
    "background": "#FF0A46A0",
    "module_tick_hidden": False,
    "_note": "Generated by tools/ctr/gen_manifest.py. Pictures are composed by the module from the player's romfs.",
    # the package owns @settings, so its flags declare their own defaults and persistence
    "flags": {f: 1 for _, f in HUD},
    "persist_flags": [f for _, f in HUD],
    # controller focus mode (LS+RS): the game's orange frame
    "nav": {"color": "#FFFF9800", "frame": 5},
    "actions": actions,
    "enforce": enforce,
    "pages": [
        {"id": "main", "nav_order": ["cog"], "widgets": main},
        {"id": "@settings", "nav_order": [f"set_{i}" for i in range(len(HUD))] + ["set_back"],
         "widgets": settings_page},
    ],
}
out = ROOT / "packages/CrashTeamRacingNitroFueled/dualscreen/manifest.json"
out.write_text(json.dumps(manifest, indent=1) + "\n")
print("wrote", out)
