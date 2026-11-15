#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Fab
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate packages/KirbysReturnToDreamLandDeluxe/dualscreen/manifest.json and its effect plans. gen_manifest.py [--dev] [--check]"""
import argparse
import json
import math
import sys
import tempfile
from pathlib import Path

PKG = Path(__file__).resolve().parents[2] / "packages" / "KirbysReturnToDreamLandDeluxe" / "dualscreen"
OUT = PKG  # where the generated files go (a scratch folder with --check)
GENERATED = ("manifest.json", "shatter.txt", "finale.txt", "soul.txt")
# the game's 1.0.0 build, the only one the module reads
BUILD_ID = "FA24EA3C3A512C072A2062556C9C9692B3E6ACE1000000000000000000000000"
# The order the game's own souvenir menu uses.

# keep in sync with package/dualscreen/kirby_font.txt; the cap is 5 x a label's text_scale, so the runtime draws it 1:1.
# The Arenas' timer font (Step ArenaTimer T_Timer: GLB-FOT-KurokaneStd-EB-OL-M, white edged in the material's black, 0 4 31
# linear, which the screen shows as 002262)
FONT_SPEC = "FOT-KurokaneStd-EB:45/002262"
LANGS = {
    "AmericanEnglish": ["US", "English"], "BritishEnglish": ["EU", "English"],
    "Japanese": ["JP", "Japanese"], "French": ["EU", "French"], "CanadianFrench": ["US", "French"],
    "German": ["EU", "German"], "Italian": ["EU", "Italian"], "Spanish": ["EU", "Spanish"],
    "LatinAmericanSpanish": ["US", "Spanish"], "Dutch": ["EU", "Dutch"],
    "Portuguese": ["US", "Portuguese"], "BrazilianPortuguese": ["US", "Portuguese"],
    "Korean": ["KR", "Korean"], "SimplifiedChinese": ["CN", "Chinese"],
    "TraditionalChinese": ["TW", "Chinese"],
}
COLOUR_BITS = 7  # native Module::ColourBits
# Options tab settings, all 0 by default (animated background, every HUD counter shown)
# the Options tab's settings, then the Heal and Magolor Blast popups seen per file (native Module::HealSeen, BlastSeen)
# (Kirby's colour last, per file: a preset's index in seven bits each, native Module::ColourBit)
PIN_SLOTS, PIN_BITS = 5, 7  # native Module::PinSlots, PinBits
OPTION_FLAGS = ["still", "hide_spheres", "hide_lives", "hide_stars", "hide_souvenir", "heal_seen_0", "heal_seen_1", "heal_seen_2",
                "blast_seen_0", "blast_seen_1", "blast_seen_2", "hide_magic", "hide_energy"] + \
               [f"col_f{f}_bit_{b}" for f in range(3) for b in range(COLOUR_BITS)] + \
               [f"pin_f{f}_s{k}_bit_{b}" for f in range(3) for k in range(PIN_SLOTS) for b in range(PIN_BITS)]
# the flags' set rules in blocks of seven, a colour or a pin slot each (native Module::StoreBlock)
STORE_BLOCK = 7
STORE_BLOCKS = (len(OPTION_FLAGS) + STORE_BLOCK) // STORE_BLOCK


def store_block(i):
    return (i + 1) // STORE_BLOCK


def option_flag(name):
    return "opt." + name.replace("_", ".")

RESEARCH_ACTIONS = ["scan_player", "ptrscan", "statescan", "bytefind", "findseq", "basefind", "vscan", "vtscan", "hscan", "layoutscan", "trace", "render", "fxwatch", "objfind", "panewatch", "bgmwatch", "abilitystar", "starhunt", "newhunt", "newforce", "soul_scream"]

WHITE = "#FFFFFFFF"  # images are multiplied by the widget colour (default #FFE6ECF2)


def rect(x, y, w, h, bg):
    return {"type": "rect", "rect": [x, y, w, h], "bg": bg, "color": "#00000000"}


# PauseBg's star icons on a lattice, spaced 1.3 times the pause screen's (400 px along a line, lines
# (128, 380) apart) so they sit less close; the module slides them right and down (689, 494) px per
# 240-frame loop, one lattice step, and turns them a full circle anticlockwise
BG_STEP, BG_LINE, BG_SLIDE = 520, (169, 494), (689, 494)
BG_ICON = 330
# the stage page's background in the mode's colours (story, extra, Magolor Epilogue): the ground, then its icons
PLAY_BASE = ["#FF4553A6", "#FFA65A5C", "#FF227D8C", "#FFB4B4BC", "#FFE9C54A"]
PLAY_ICON = ["#FF2E3E9E", "#FFA33847", "#FF64B7A9", "#FF8E8E99", "#FFC99D25"]  # fading to white while the game loads (icon_tints)


def icon_tints():
    # per mode 16 steps from its icon colour to white (play.tint = mode * 16 + step), so they fade
    def mix(c, k):
        a, rgb = c[1:3], [int(c[i:i + 2], 16) for i in (3, 5, 7)]
        return "#" + a + "".join(f"{round(v + (255 - v) * k / 15):02X}" for v in rgb)
    return [mix(c, k) for c in PLAY_ICON for k in range(16)]


def background(icon="bg.mask"):
    widgets = [rect(0, 0, 1240, 1080, base) | {"hide_bind": f"play.bg.{mode}", "hide_eq": 0}
               for mode, base in enumerate(PLAY_BASE)]
    r = BG_ICON // 2
    for k in range(-2, 4):
        for i in range(-3, 4):
            cx, cy = 100 + i * BG_STEP + k * BG_LINE[0], -100 + k * BG_LINE[1]
            # every icon that can show somewhere along the slide
            if cx + r < -BG_SLIDE[0] or cx - r > 1240 or cy + r < -BG_SLIDE[1] or cy - r > 1080:
                continue
            widgets.append({"type": "image", "color": WHITE, "src": "", "src_bind": icon,
                            "rect": [cx - r, cy - r, BG_ICON, BG_ICON],
                            "tint_bind": "play.tint", "tint_colors": icon_tints(),
                            "rotate_bind": "bg.r", "x_bind": "bg.dx", "y_bind": "bg.dy"})
    return widgets


# The stage page ("Game" tab), drawn by the module (KirbyModes::PlayBar and friends). Controls, in
# KirbyModes::PlayButtons' order: picture rect, touch rect.
PLAY_BAR = [0, 0, 1240, 260]
AMIIBO = [-40, 75, 740, 149]  # KirbyModes::AmiiboBox
ROUND, SOUVENIR, TAB = 300, 780, [358, 110]  # KirbyModes::RoundSize, SouvenirSize, StageTabSize
BATTERY = [280, 10, 42, 66]  # KirbyModes::BatteryBox: the user's frame over the module's charge
# the round buttons' pair and the souvenir's disc (11.7 px above its box's centre) share a middle line
PLAY_CONTROLS = [([66, 269, ROUND, ROUND], [86, 289, 260, 260]),  # pause
                 ([66, 627, ROUND, ROUND], [86, 647, 260, 260]),  # drop ability
                 ([446, 220, SOUVENIR, SOUVENIR], [512, 286, 648, 648]),  # souvenir
                 (AMIIBO, [0, 104, 560, 106]),  # amiibo
                 ([8, 983, *TAB], [14, 985, 346, 95]), ([378, 983, *TAB], [384, 985, 346, 95]),
                 ([748, 983, *TAB], [754, 985, 346, 95])]  # tabs: the shown one raised (play.ty.<n> drops the others)
GAME_CONTROLS = 3  # pause, drop ability, souvenir: the Game tab's section
# the Epilogue's tabs keep the full width (no colour ribbon there; KirbyModes::TabSize)
EPI_TABS = [[8, 983, 400, 110], [420, 983, 400, 110], [832, 983, 400, 110]]
# The Abilities tab's section (KirbyModes::Ability*Size): the description, the page counter, the moves
# window (two slots that slide past each other) and the arrows; the counter in the margin right of the
# description leaves the moves window the room below it
ABL_INFO, ABL_PILL, ABL_MOVES = [220, 230, 800, 196], [1030, 299, 190, 58], [122, 436, 996, 542]
ABL_ARROWS = [[8, 636, 110, 142], [1122, 636, 110, 142]]
# The Options tab's section (KirbyModes::Option*Size): the list's rows, a page's panel over which the HUD
# counters' lines or the cheat (label, ability card between its arrows, Apply) sit; module tap codes
OPT_ROWS = [[30, 248 + 136 * i, 1180, 104] for i in range(3)]
OPT_PANEL, OPT_BACK = [30, 248, 1180, 700], [58, 262, 56, 72]
OPT_LINES = [[70, 388 + 72 * i, 1100, 64] for i in range(4)]
OPT_LABEL, OPT_CARD, OPT_APPLY = [70, 442, 1100, 64], [660, 384, 180, 180], [950, 384, 180, 180]
OPT_ARROWS = [[592, 438, 56, 72], [852, 438, 56, 72]]
# The Epilogue's Magic tab: the Abilities tab's description and counter higher (the Epilogue's bar is lower), and
# under them a cream band with the learnt techniques' strip looping along it, one cell per technique
EPIM_INFO, EPIM_PILL = [220, 136, 800, 196], [1030, 205, 190, 58]
EPIM_BAND, EPIM_CELL, EPIM_CELLS = [0, 344, 1240, 88], [0, 344, 150, 88], 10  # KirbyModes::TechniqueCellSize, native EpiStripCells
EPIM_CREAM, EPIM_GOLD = "#FFFFFEF7", "#FFC19346"
# The Epilogue's cheats page, one row each with a line under it (native EpiCheatRules, on the panel): the gauge
# refill (label, Apply), then the magic points (label, their card between its arrows, -100 / +100 and -1000 /
# +1000, Apply; KirbyModes::EpiApplySize, DoubleArrowSize)
EPO_FILL_LABEL, EPO_FILL_APPLY = [70, 388, 1100, 64], [940, 370, 200, 100]
EPO_POINTS_LABEL, EPO_CARD, EPO_POINTS_APPLY = [70, 540, 1100, 64], [575, 504, 136, 136], [940, 522, 200, 100]
EPO_ARROWS = [[509, 536, 56, 72], [721, 536, 56, 72]]
EPO_ARROWS2 = [[420, 536, 81, 72], [785, 536, 81, 72]]
PLAY_LOCK = "#73000000"  # over the page while Kirby cannot act
PLAY_FADE_MS = 300


# Magolor Epilogue's page (KirbyModes::Epilogue*, Gauge*): the stage page's background in its colours, its
# bar, Pause (Continue while paused), the special moves, the special gauge as a ring and the tabs. Controls in
# EpilogueStep's tap order: picture rect, touch rect.
EPI_BAR, EPI_BATTERY = [0, 0, 1240, 196], [1084, 8, 42, 66]  # KirbyModes::EpilogueBarSize, EpiBatteryBox
# the Arenas' run time over the bar: centred in the band's plain part, between the star and the checker, and in
# KirbyModes::EpiTextSlot's height (the font's glyphs carry their edge, so the label's white leaves them as they are)
ARENA_TIME = {"type": "label", "rect": [400, 67, 0, 0], "bind_text": "arena.time", "text_scale": 9, "align": "center", "color": WHITE}
EPI_ROUND, EPI_SPECIAL = 330, 231  # KirbyModes::EpiRoundSize, SpecialSize
# (tap code, picture rect, touch rect): Pause over a 2 x 2 grid of the gauge's buttons (Magolor Cannon, Black Hole,
# then Heal and Magolor Blast); EpilogueStep's tap codes (3..5 are the tabs)
def epi_grid(col, row):
    x, y = 44 + col * 246, 512 + row * 240
    return [x, y, EPI_SPECIAL, EPI_SPECIAL], [x + 15, y + 15, EPI_SPECIAL - 30, EPI_SPECIAL - 30]


EPI_CONTROLS = [(0, [116, 176, EPI_ROUND, EPI_ROUND], [138, 198, 286, 286]),
                (1, *epi_grid(0, 0)), (2, *epi_grid(1, 0)), (6, *epi_grid(0, 1)), (7, *epi_grid(1, 1))]
GAUGE, GAUGE_SIZE, GAUGE_SEGMENTS = [603, 248], 600, 60  # KirbyModes::GaugeSize, GaugeSegments
GAUGE_INNER, GAUGE_OUTER = 192, 288


def gauge_segment_box(k):
    # KirbyModes::GaugeSegmentBox: the wedge's corners, half a degree wider each side, 2 px margin
    a0 = math.radians(-90 + 360 * k / GAUGE_SEGMENTS - 0.5)
    a1 = math.radians(-90 + 360 * (k + 1) / GAUGE_SEGMENTS + 0.5)
    pts = [(GAUGE_SIZE / 2 + r * math.cos(a), GAUGE_SIZE / 2 + r * math.sin(a)) for r in (GAUGE_INNER, GAUGE_OUTER) for a in (a0, a1)]
    l, t = math.floor(min(p[0] for p in pts)) - 2, math.floor(min(p[1] for p in pts)) - 2
    return [l, t, math.ceil(max(p[0] for p in pts)) + 2 - l, math.ceil(max(p[1] for p in pts)) + 2 - t]


EPI_POPUP, EPI_POPUP_BUTTON = [40, 40, 1160, 1000], [330, 772]
EPI_PRELOAD = 90  # the Epilogue page's pictures the module hands over as epipre.<n> (native EpiPreload)


def epi_preload():
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    return [clear | {"src_bind": f"epipre.{n}"} for n in range(EPI_PRELOAD)]


def epilogue_page():
    def gen(key):
        return f"module:kirby:gen/files/{key}"

    # in play only (not on the Epilogue's title screen), fading out between two rooms like the stage page
    fade = {"bind": "epi.vis", "group": "epi_controls", "from": "fade", "ms": PLAY_FADE_MS, "easing": "ease_in_out",
            "box": [0, 0, 1240, 1080]}
    image = {"type": "image", "color": WHITE, "anim": fade}
    game = {"x_bind": "epi.sx"}  # the Game tab's section, sliding across when the tab changes
    widgets = background("bg.gear")
    widgets.append(image | {"rect": EPI_BAR, "src": "", "src_bind": "epi.bar"})
    widgets.append(image | {"rect": EPI_BATTERY, "src": "file:art/battery.png", "hide_bind": "epi.battery", "hide_eq": 0})
    _, pause, _ = EPI_CONTROLS[0]
    # Pause gives way to Continue while the game's pause menu is open
    widgets.append(image | {"rect": pause, "src": "", "src_bind": "epi.rbtn.0", "scale_bind": "epi.scale.0",
                            "hide_bind": "epi.paused", "hide_eq": 1} | game)
    for slot, (tap, r, _) in enumerate(EPI_CONTROLS[1:]):
        widgets.append(image | {"rect": r, "src": "", "src_bind": f"epi.sp.{slot}", "scale_bind": f"epi.scale.{tap}"} | game)
    # the gauge: its fill, the sparkles drifting round it, the empty track over each segment past its level,
    # the glow of a full one, its rims and the flask; a locked one is a single greyed picture
    gx, gy = GAUGE
    ring = image | {"rect": [gx, gy, GAUGE_SIZE, GAUGE_SIZE]}
    widgets.append(ring | {"src": gen("epigauge/charging"), "hide_bind": "epi.full", "hide_eq": 1} | game)
    widgets.append(ring | {"src": gen("epigauge/full"), "hide_bind": "epi.full", "hide_eq": 0} | game)
    # the last blow's charge: the full gauge in every colour, turning, faded in (FinaleStep)
    rain = {"hide_bind": "efx.rain", "hide_eq": 0, "tint_bind": "efx.ra", "tint_colors": [f"#{a * 17:02X}FFFFFF" for a in range(16)]}
    widgets.append(ring | {"src": gen("epigauge/rainbow"), "spin": 60} | rain | game)
    widgets.append(ring | {"src": gen("epigauge/particles1"), "spin": 9} | game)
    widgets.append(ring | {"src": gen("epigauge/particles2"), "spin": -6} | game)
    for k in range(GAUGE_SEGMENTS):
        x, y, w, h = gauge_segment_box(k)
        widgets.append(image | {"rect": [gx + x, gy + y, w, h], "src": gen(f"epigauge/empty/{k}"), "hide_bind": "epi.deg",
                                "keep_max": k * 360 // GAUGE_SEGMENTS} | game)
    # the part of the level's own segment still empty: one segment's track turned to the level (degrees)
    widgets.append(ring | {"src": gen("epigauge/edge"), "rotate_bind": "epi.deg", "pivot": [GAUGE_SIZE // 2, GAUGE_SIZE // 2],
                           "hide_bind": "epi.edge_on", "hide_eq": 0} | game)
    widgets.append(ring | {"src": gen("epigauge/glow"), "tint_bind": "epi.glow",
                           "tint_colors": [f"#{a * 17:02X}FFFFFF" for a in range(16)], "hide_bind": "epi.full", "hide_eq": 0} | game)
    widgets.append(ring | {"src": gen("epigauge/front")} | game)
    widgets.append(ring | {"src": gen("epigauge/locked"), "hide_bind": "epi.gauge_on", "hide_eq": 1} | game)
    # and the magic overflowing it: two layers of flames turning against each other and pulsing, sparks in the halo
    m = (GAUGE_AURA - GAUGE_SIZE) // 2
    aura = image | {"rect": [gx - m, gy - m, GAUGE_AURA, GAUGE_AURA]} | rain | game
    widgets += [aura | {"src": gen("epiaura/0"), "spin": 25, "scale_bind": "efx.as0"},
                aura | {"src": gen("epiaura/1"), "spin": -35, "scale_bind": "efx.as1"},
                aura | {"src": gen("epiaura/2"), "spin": 12}]
    # the Magic tab: Magolor's moves in his pause screen's look, laid out as the stage page's Abilities tab
    widgets += [image | {"rect": EPIM_INFO, "src": "", "src_bind": "epim.info", "x_bind": "epim.sx"},
                image | {"rect": EPIM_PILL, "src": "", "src_bind": "epim.pill", "x_bind": "epim.sx"}]
    x, y, w, h = EPIM_BAND
    band = {"anim": fade, "x_bind": "epim.sx"}
    widgets += [rect(x, y, w, h, EPIM_CREAM) | band, rect(x, y, w, 3, EPIM_GOLD) | band, rect(x, y + h - 3, w, 3, EPIM_GOLD) | band]
    widgets += [image | {"rect": EPIM_CELL, "src": "", "src_bind": f"epim.cell.{i}", "x_bind": f"epim.cell.{i}.x",
                         "hide_bind": f"epim.cell.{i}.on", "hide_eq": 0} for i in range(EPIM_CELLS)]
    widgets += [image | {"rect": ABL_MOVES, "src": "", "src_bind": f"epim.{s}.page", "x_bind": f"epim.{s}.x"} for s in "ab"]
    widgets += [image | {"rect": r, "src": "", "src_bind": f"epim.arrow.{i}", "x_bind": "epim.sx", "scale_bind": f"epim.scale.{i}"}
                for i, r in enumerate(ABL_ARROWS)]
    # the Options tab: the stage page's, with Magolor's HUD counters and cheats
    widgets += [image | {"rect": r, "src": "", "src_bind": f"epo.row.{i}", "x_bind": "epo.lx", "scale_bind": f"epo.scale.{i}",
                         "hide_bind": "epo.list_on", "hide_eq": 0} for i, r in enumerate(OPT_ROWS)]
    page = image | {"x_bind": "epo.px"}
    for kind, shown in ((1, "epo.is_hud"), (2, "epo.is_cheat")):
        widgets.append(page | {"rect": OPT_PANEL, "src": "", "src_bind": f"epo.panel.{kind}", "hide_bind": shown, "hide_eq": 0})
    widgets.append(page | {"rect": OPT_BACK, "src": "", "src_bind": "epo.back", "scale_bind": "epo.scale.3"})
    hud, cheat = {"hide_bind": "epo.is_hud", "hide_eq": 0}, {"hide_bind": "epo.is_cheat", "hide_eq": 0}
    widgets += [page | hud | {"rect": r, "src": "", "src_bind": f"epo.line.{i}"} for i, r in enumerate(OPT_LINES[:2])]
    widgets += [page | cheat | {"rect": EPO_FILL_LABEL, "src": "", "src_bind": "epo.label.0"},
                page | cheat | {"rect": EPO_FILL_APPLY, "src": "", "src_bind": "epo.apply.0", "scale_bind": "epo.scale.7"},
                page | cheat | {"rect": EPO_POINTS_LABEL, "src": "", "src_bind": "epo.label.1"},
                page | cheat | {"rect": EPO_CARD, "src": "", "src_bind": "epo.card"},
                page | cheat | {"rect": EPO_ARROWS[0], "src": "", "src_bind": "epo.back", "scale_bind": "epo.scale.4"},
                page | cheat | {"rect": EPO_ARROWS[1], "src": "", "src_bind": "epo.next", "scale_bind": "epo.scale.5"},
                page | cheat | {"rect": EPO_POINTS_APPLY, "src": "", "src_bind": "epo.apply.1", "scale_bind": "epo.scale.6"},
                page | cheat | {"rect": EPO_ARROWS2[0], "src": "", "src_bind": "epo.back2", "scale_bind": "epo.scale.8"},
                page | cheat | {"rect": EPO_ARROWS2[1], "src": "", "src_bind": "epo.next2", "scale_bind": "epo.scale.9"}]
    for t, (x, y, w, h) in enumerate(EPI_TABS):
        widgets.append(image | {"rect": [x, y, w, h], "src": "", "src_bind": f"epi.tab.{t}", "y_bind": f"epi.ty.{t}",
                                "scale_bind": f"epi.scale.{3 + t}"})
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    widgets += [clear | {"src_bind": key} for key in ("epi.bar_next", "epim.info_next", "epim.pill_next", "epo.row.0_next", "epo.card_next",
                                                     "epo.line.0_next", "epo.line.1_next", "epo.panel.1", "epo.panel.2", "epo.label.0",
                                                     "epo.label.1", "epo.back", "epo.next", "epo.back2", "epo.next2")]
    widgets.append(boost("epi.boost"))
    touch = {"type": "button", "text": "", "bg": "#00000000", "color": "#00000000", "need_bind": "epi.vis"}
    for tap, _, r in EPI_CONTROLS:
        widgets.append(touch | {"rect": r, "on_tap": f"epi_tap_{tap}", "need_bind": "epi.in_game"})
    for t, (x, y, w, h) in enumerate(EPI_TABS):
        widgets.append(touch | {"rect": [x + 6, y + 2, w - 12, h - 2], "on_tap": f"epi_tap_{3 + t}"})
    for i, (x, y, w, h) in enumerate(ABL_ARROWS):
        widgets.append(touch | {"rect": [x + 10, y + 10, w - 20, h - 20], "on_tap": f"epim_tap_{i}", "hide_bind": "epim.shown", "hide_eq": 0})
    in_list, in_hud, in_cheat = ({"hide_bind": f"epo.in_{v}", "hide_eq": 0} for v in ("list", "hud", "cheat"))
    widgets += [touch | in_list | {"rect": r, "on_tap": f"epo_tap_{i}"} for i, r in enumerate(OPT_ROWS)]
    widgets += [touch | {"rect": [40, 250, 110, 100], "on_tap": "epo_tap_3", "hide_bind": "epo.in_page", "hide_eq": 0}]
    widgets += [touch | in_hud | {"rect": [40, r[1] - 4, 1160, 72], "on_tap": f"epo_tap_{10 + i}"} for i, r in enumerate(OPT_LINES[:2])]
    # (the arrows' touch areas reach 4 px either side and 20 px above and below)
    widgets += [touch | in_cheat | {"rect": [x - 4, y - 20, w + 8, h + 40], "on_tap": f"epo_tap_{code}"}
                for code, (x, y, w, h) in zip((20, 21, 24, 25), EPO_ARROWS + EPO_ARROWS2)]
    widgets += [touch | in_cheat | {"rect": EPO_POINTS_APPLY, "on_tap": "epo_tap_22"}, touch | in_cheat | {"rect": EPO_FILL_APPLY, "on_tap": "epo_tap_23"}]
    widgets.append(rect(0, 0, 1240, 1080, PLAY_LOCK) | {"hide_bind": "epi.lock", "hide_eq": 0})
    # Continue, in Pause's place over the lock while the pause menu is open (in the Game tab: it slides with it)
    (_, pause, r), paused = EPI_CONTROLS[0], {"hide_bind": "epi.paused", "hide_eq": 0}
    widgets.append(image | {"rect": pause, "src": "", "src_bind": "epi.rbtn.1", "scale_bind": "epi.scale.resume"} | paused | game)
    widgets.append(touch | {"rect": r, "on_tap": "epi_resume_tap", "hide_bind": "epi.resume", "hide_eq": 0})
    # Heal's popup over the lock, once per file, and its button (KirbyModes::PopupSize, PopupButtonAt)
    pop = {"type": "image", "color": WHITE, "anim": fade | {"bind": "epi.popup", "group": "epi_popup"}}
    (x, y, w, h), (bx, by) = EPI_POPUP, EPI_POPUP_BUTTON
    widgets.append(pop | {"rect": [x, y, w, h], "src": "", "src_bind": "epi.pop"})
    widgets.append(pop | {"rect": [x + bx, y + by, 500, 122], "src": "", "src_bind": "epi.popbtn", "scale_bind": "epi.scale.pop"})
    widgets.append(touch | {"rect": [x + bx + 20, y + by + 8, 460, 106], "on_tap": "epi_pop_tap", "need_bind": "epi.popup"})
    return fin_shaken(widgets) + epi_preload()


def play_page():
    def gen(key):
        return f"module:kirby:gen/files/{key}"

    pictures = [{"src": "", "src_bind": "play.rbtn.0"}, None, {"src": "", "src_bind": "play.souv"},
                {"src": gen("amiibo")}] + [{"src": "", "src_bind": f"play.tab.{t}"} for t in range(3)]
    # every control fades in while a stage or hub is up (play.vis) and out between two rooms, where only
    # the background stays
    fade = {"bind": "play.vis", "group": "play_controls", "from": "fade", "ms": PLAY_FADE_MS, "easing": "ease_in_out",
            "box": [0, 0, 1240, 1080]}
    widgets = background()
    for i, ((x, y, w, h), _) in enumerate(PLAY_CONTROLS):
        common = {"type": "image", "color": WHITE, "rect": [x, y, w, h], "scale_bind": f"play.scale.{i}", "anim": fade}
        # the Game tab's controls slide with their section, the tabs drop when not shown
        if i < GAME_CONTROLS:
            common["x_bind"] = "play.sx"
        if i == 0:
            # Pause gives way to Continue while the game's pause menu is open
            common |= {"hide_bind": "play.paused", "hide_eq": 1}
        elif i >= 4:
            common["y_bind"] = f"play.ty.{i - 4}"
        if i == 1:
            # the drop button, greyed out while there is nothing Kirby can drop
            widgets += [common | {"src": "", "src_bind": "play.rbtn.1", "hide_bind": "play.vis_drop", "hide_eq": 0},
                        common | {"src": "", "src_bind": "play.rbtn.2", "hide_bind": "play.vis_nodrop", "hide_eq": 0}]
        elif i == 3:
            # under the bar, which it completes; greyed out of a stage, where the game takes no amiibo
            widgets.append(common | pictures[i])
            widgets.append(common | {"src": gen("amiibo/off"), "hide_bind": "play.vis_noamiibo", "hide_eq": 0})
            widgets.append({"type": "image", "color": WHITE, "rect": PLAY_BAR, "src": "", "src_bind": "play.bar", "anim": fade})
            # the device's battery (runtime 21 publishes it; older runtimes and battery-less devices show none)
            widgets.append({"type": "image", "color": WHITE, "rect": BATTERY, "src": "file:art/battery.png", "anim": fade,
                            "hide_bind": "play.battery", "hide_eq": 0})
        else:
            widgets.append(common | pictures[i])
    # the Abilities tab: its section slides in from the right (abl.sx), each moves page by its own slot's x
    abl = {"type": "image", "color": WHITE, "anim": fade}
    widgets += [abl | {"rect": ABL_INFO, "src": "", "src_bind": "abl.info", "x_bind": "abl.sx"},
                abl | {"rect": ABL_PILL, "src": "", "src_bind": "abl.pill", "x_bind": "abl.sx"}]
    widgets += [abl | {"rect": ABL_MOVES, "src": "", "src_bind": f"abl.{s}.page", "x_bind": f"abl.{s}.x"} for s in "ab"]
    widgets += [abl | {"rect": r, "src": "", "src_bind": f"abl.arrow.{i}", "x_bind": "abl.sx", "scale_bind": f"abl.scale.{i}"}
                for i, r in enumerate(ABL_ARROWS)]
    # the Options tab: the list slides with opt.lx, the pages with opt.px
    opt = {"type": "image", "color": WHITE, "anim": fade}
    widgets += [opt | {"rect": r, "src": "", "src_bind": f"opt.row.{i}", "x_bind": "opt.lx", "scale_bind": f"opt.scale.{i}",
                       "hide_bind": "opt.list_on", "hide_eq": 0} for i, r in enumerate(OPT_ROWS)]
    page = opt | {"x_bind": "opt.px"}
    for kind, shown in ((1, "opt.is_hud"), (2, "opt.is_cheat")):
        widgets.append(page | {"rect": OPT_PANEL, "src": "", "src_bind": f"opt.panel.{kind}", "hide_bind": shown, "hide_eq": 0})
    widgets.append(page | {"rect": OPT_BACK, "src": "", "src_bind": "opt.back", "scale_bind": "opt.scale.3"})
    hud, cheat = {"hide_bind": "opt.is_hud", "hide_eq": 0}, {"hide_bind": "opt.is_cheat", "hide_eq": 0}
    widgets += [page | hud | {"rect": r, "src": "", "src_bind": f"opt.line.{i}"} for i, r in enumerate(OPT_LINES)]
    widgets += [page | cheat | {"rect": OPT_LABEL, "src": "", "src_bind": "opt.label"},
                page | cheat | {"rect": OPT_CARD, "src": "", "src_bind": "opt.card"},
                page | cheat | {"rect": OPT_ARROWS[0], "src": "", "src_bind": "opt.back", "scale_bind": "opt.scale.4"},
                page | cheat | {"rect": OPT_ARROWS[1], "src": "", "src_bind": "opt.next", "scale_bind": "opt.scale.5"},
                page | cheat | {"rect": OPT_APPLY, "src": "", "src_bind": "opt.apply", "scale_bind": "opt.scale.6"}]
    widgets += colour_ribbon(fade)
    # the next counters and slot, handed to the host ahead so they swap in without a blank frame
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    # (and the Options pages' own pictures, loaded with the page so a page never opens empty)
    widgets += [clear | {"src_bind": key} for key in ("play.bar_next", "play.souv_next", "abl.info_next", "abl.pill_next", "play.rbtn.3",
                                                     "opt.row.0_next", "opt.card_next", *(f"opt.line.{i}_next" for i in range(4)),
                                                     "opt.panel.1", "opt.panel.2", "opt.label", "opt.apply", "opt.back", "opt.next")]
    widgets.append(boost("play.boost"))
    # names the runtime's battery values in the manifest, so it publishes them (the module reads them)
    widgets.append({"type": "value", "rect": [0, 0, 1, 1], "bind": "@device.battery_pct", "color": "#00000000",
                    "hide_bind": "@device.charging", "hide_eq": 2})
    # each section's touch areas only while it is shown, so they never take another section's taps
    for i, (_, touch) in enumerate(PLAY_CONTROLS):
        widgets.append({"type": "button", "rect": touch, "text": "", "bg": "#00000000", "color": "#00000000",
                        "on_tap": f"play_tap_{i}"} | ({"hide_bind": "play.in_game", "hide_eq": 0} if i < GAME_CONTROLS else {}))
    widgets.append({"type": "button", "rect": COLOUR_RIBBON, "text": "", "bg": "#00000000", "color": "#00000000", "on_tap": "col_tap_0",
                    "need_bind": "col.kirby"})
    for i, (x, y, w, h) in enumerate(ABL_ARROWS):
        widgets.append({"type": "button", "rect": [x + 10, y + 10, w - 20, h - 20], "text": "", "bg": "#00000000",
                        "color": "#00000000", "on_tap": f"abl_tap_{i}", "hide_bind": "abl.shown", "hide_eq": 0})
    # the Options tab's controls, each where its view shows
    touch = {"type": "button", "text": "", "bg": "#00000000", "color": "#00000000"}
    in_list, in_hud, in_cheat = ({"hide_bind": f"opt.in_{v}", "hide_eq": 0} for v in ("list", "hud", "cheat"))
    widgets += [touch | in_list | {"rect": r, "on_tap": f"opt_tap_{i}"} for i, r in enumerate(OPT_ROWS)]
    widgets += [touch | {"rect": [40, 250, 110, 100], "on_tap": "opt_tap_3", "hide_bind": "opt.in_page", "hide_eq": 0}]
    widgets += [touch | in_hud | {"rect": [40, r[1] - 4, 1160, 72], "on_tap": f"opt_tap_{10 + i}"} for i, r in enumerate(OPT_LINES)]
    widgets += [touch | in_cheat | {"rect": [x - 20, y - 20, w + 40, h + 40], "on_tap": f"opt_tap_{20 + i}"}
                for i, (x, y, w, h) in enumerate(OPT_ARROWS)]
    widgets.append(touch | in_cheat | {"rect": OPT_APPLY, "on_tap": "opt_tap_22"})
    widgets.append(rect(0, 0, 1240, 1080, PLAY_LOCK) | {"hide_bind": "play.lock", "hide_eq": 0})
    # Continue, in Pause's place over the lock while the pause menu is open: its push, then Plus
    (x, y, w, h), touch = PLAY_CONTROLS[0]
    paused = {"hide_bind": "play.paused", "hide_eq": 0}
    widgets.append({"type": "image", "color": WHITE, "rect": [x, y, w, h], "src": "", "src_bind": "play.rbtn.3",
                    "x_bind": "play.sx", "scale_bind": "play.scale.resume"} | paused)
    widgets.append({"type": "button", "rect": touch, "text": "", "bg": "#00000000", "color": "#00000000",
                    "on_tap": "play_resume_tap", "need_bind": "play.in_game"} | paused)
    return widgets + colour_layer()


# Kirby's colour menu (user's sketch 2026-10-28; KirbyModes::Colour*, native ColourMenuStep): the ribbon at the bottom right
# of Kirby's pages, and the popup over the page. Every popup picture is scaled about the popup's centre (col.pop, its pivot
# there); the slots, Apply and Back get their push and the slots their scroll as offsets from the module. The frame is
# drawn over the slots, leaving their window open, and input blocks over it keep a hidden slot from a tap.
COLOUR_RIBBON = [1118, 912, 112, 168]  # KirbyModes::ColourRibbonSize
COLOUR_AT, COLOUR_SIZE = (40, 40), (1160, 1000)  # native ColourLeft / ColourTop, KirbyModes::ColourPopupSize
COLOUR_SLOTS = 86  # KirbyColour::Slots (a slot without a colour shows a "?"; the module drops its taps)
COLOUR_COLUMNS, COLOUR_FIRST, COLOUR_PITCH = 7, (73, 186), (139, 136)  # KirbyModes::ColourColumns, ColourFirst, ColourPitch
COLOUR_SLOT, COLOUR_RING = (118, 120), 7  # KirbyModes::ColourSlotSize, ColourSlotRing
COLOUR_BAR, COLOUR_APPLY = (36, 36, 888, 140), (906, 34, 1126, 148)  # KirbyModes::ColourBar, ColourApplyBox
COLOUR_WINDOW, COLOUR_TRACK, COLOUR_THUMB = (36, 164, 1062, 964), (1070, 180, 1118, 948), (48, 160)  # ColourWindow, ColourTrack, ColourThumbSize
COLOUR_BACK = (1112, 958, 66, 148)  # KirbyModes::ColourBackCircle, ColourBackSize


def colour_ribbon(fade):
    # (not for the Arenas' other characters)
    return [{"type": "image", "color": WHITE, "rect": COLOUR_RIBBON, "src": "", "src_bind": "col.ribbon", "scale_bind": "col.scale.ribbon",
             "anim": fade, "need_bind": "col.kirby"}]


def colour_layer():
    ox, oy = COLOUR_AT
    cx, cy = ox + COLOUR_SIZE[0] / 2, oy + COLOUR_SIZE[1] / 2
    shown = {"hide_bind": "col.on", "hide_eq": 0}

    def popped(x0, y0, x1, y1, scale="col.pop"):
        x, y = ox + x0, oy + y0
        return {"type": "image", "color": WHITE, "rect": [x, y, x1 - x0, y1 - y0], "scale_bind": scale, "pivot": [cx - x, cy - y]} | shown

    touch = {"type": "button", "text": "", "bg": "#00000000", "color": "#00000000"}
    block = {"type": "rect", "bg": "#00000000", "color": "#00000000", "input_block": True} | shown
    wx0, wy0, wx1, wy1 = COLOUR_WINDOW
    widgets = [{"type": "rect", "rect": [0, 0, 1240, 1080], "bg": PLAY_LOCK, "color": "#00000000", "input_block": True,
                "anim": {"bind": "col.on", "group": "col_dim", "from": "fade", "ms": 200, "easing": "ease_in_out"}},
               popped(wx0, wy0, wx1, wy1) | {"src": "module:kirby:gen/files/colwhite"}]
    slots = []
    for i in range(COLOUR_SLOTS):
        x0 = COLOUR_FIRST[0] + (i % COLOUR_COLUMNS) * COLOUR_PITCH[0]
        y0 = COLOUR_FIRST[1] + (i // COLOUR_COLUMNS) * COLOUR_PITCH[1]
        slot = popped(x0 - COLOUR_RING, y0 - COLOUR_RING, x0 + COLOUR_SLOT[0] + COLOUR_RING, y0 + COLOUR_SLOT[1] + COLOUR_RING, f"col.s{i}.s")
        widgets.append(slot | {"src": "", "src_bind": f"col.slot.{i}", "x_bind": f"col.s{i}.x", "y_bind": f"col.s{i}.y",
                               "need_bind": f"col.sv.{i}"})
        slots.append(touch | shown | {"rect": [ox + x0, oy + y0, *COLOUR_SLOT], "y_bind": "col.scroll", "on_tap": f"col_tap_{10 + i}",
                                      "need_bind": f"col.sv.{i}"})
    widgets += slots
    widgets.append(popped(0, 0, *COLOUR_SIZE) | {"src": "module:kirby:gen/files/colframe"})
    # the frame's taps stop there (outside the slots' window)
    widgets += [block | {"rect": r} for r in ([ox, oy, COLOUR_SIZE[0], wy0], [ox, oy + wy1, COLOUR_SIZE[0], COLOUR_SIZE[1] - wy1],
                                               [ox, oy + wy0, wx0, wy1 - wy0], [ox + wx1, oy + wy0, COLOUR_SIZE[0] - wx1, wy1 - wy0])]
    widgets.append(popped(*COLOUR_BAR) | {"src": "", "src_bind": "col.header"})
    widgets.append(popped(*COLOUR_APPLY, "col.a.s") | {"src": "", "src_bind": "col.apply", "x_bind": "col.a.x", "y_bind": "col.a.y"})
    tx0, ty0 = COLOUR_TRACK[:2]
    widgets.append(popped(tx0, ty0, tx0 + COLOUR_THUMB[0], ty0 + COLOUR_THUMB[1]) | {"src": "module:kirby:gen/files/colthumb", "y_bind": "col.ty"})
    bx, by, _, bs = COLOUR_BACK
    widgets.append(popped(bx - bs // 2, by - bs // 2, bx + bs // 2, by + bs // 2, "col.b.s") |
                   {"src": "module:kirby:gen/files/colback", "x_bind": "col.b.x", "y_bind": "col.b.y"})
    # dragging the slots or the scroll track scrolls them
    live = {"need_bind": "col.live"}
    widgets.append({"type": "rect", "rect": [ox + wx0, oy + wy0, wx1 - wx0, wy1 - wy0], "bg": "#00000000", "color": "#00000000",
                    "on_pan": "col_pan"} | live)
    widgets.append({"type": "rect", "rect": [ox + wx1, oy + wy0, COLOUR_SIZE[0] - 36 - wx1, wy1 - wy0], "bg": "#00000000",
                    "color": "#00000000", "on_pan": "col_thumb"} | live)
    widgets.append(touch | {"rect": [ox + COLOUR_APPLY[0], oy + COLOUR_APPLY[1], COLOUR_APPLY[2] - COLOUR_APPLY[0],
                                     COLOUR_APPLY[3] - COLOUR_APPLY[1]], "on_tap": "col_tap_1"} | live)
    widgets.append(touch | {"rect": [ox + bx - 62, oy + by - 62, 124, 124], "on_tap": "col_tap_2"} | live)
    return widgets


def colour_actions():
    return {"col_tap_0": {"kind": "module", "action": "col_tap", "argument": 0, "enabled_bind": "col.can_open"},
            "col_tap_1": {"kind": "module", "action": "col_tap", "argument": 1, "enabled_bind": "col.can_apply"},
            "col_tap_2": {"kind": "module", "action": "col_tap", "argument": 2, "enabled_bind": "col.live"},
            **{f"col_tap_{10 + i}": {"kind": "module", "action": "col_tap", "argument": 10 + i, "enabled_bind": "col.live"}
               for i in range(COLOUR_SLOTS)},
            "col_pan": {"kind": "module", "action": "col_pan", "argument": "$payload", "enabled_bind": "col.live"},
            "col_thumb": {"kind": "module", "action": "col_thumb", "argument": "$payload", "enabled_bind": "col.live"}}


# The screen cut by a sword (Grand Doomer's last blows, user 2026-10-24): four cuts across the page, each one
# splitting the pieces it crosses, then the last blow breaks every piece into shards over the starry sky. Three sets
# of cuts (user 2026-10-25), each also mirrored across the page's middle by the module (KirbyFx::Mirror), which
# picks one of the twelve at random; a mirrored piece keeps its widget, moved by its offsets. The module reads the
# sets from shatter.txt (KirbyFx::ParsePlan) and draws each piece cut out of the page.
FX_SETS = [[((-100, 150), (1340, 790)), ((-100, 930), (1340, 230)), ((440, -100), (760, 1180)), ((-100, 600), (1340, 450)),
            ((-100, 720), (1340, 340))],
           [((260, -100), (980, 1180)), ((-100, 400), (1340, 290)), ((1120, -100), (330, 1180)), ((-100, 840), (1340, 700)),
            ((-100, 230), (1340, 880))],
           [((-100, 600), (1340, 170)), ((660, -100), (430, 1180)), ((-100, 280), (1340, 960)), ((960, -100), (1120, 1180)),
            ((-100, 880), (1340, 300))]]
FX_SHARD_AREA = 42000


def fx_area(poly):
    return abs(sum(p[0] * q[1] - q[0] * p[1] for p, q in zip(poly, poly[1:] + poly[:1]))) / 2


def fx_split(poly, a, b):
    # the convex polygon's parts on either side of the line a->b
    side = lambda p: (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
    parts = [[], []]
    for p, q in zip(poly, poly[1:] + poly[:1]):
        sp, sq = side(p), side(q)
        if sp >= 0:
            parts[0].append(p)
        if sp <= 0:
            parts[1].append(p)
        if sp * sq < 0:
            t = sp / (sp - sq)
            m = (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)
            parts[0].append(m)
            parts[1].append(m)
    return [part for part in parts if len(part) >= 3 and fx_area(part) > 1]


def fx_box(poly):
    x0, y0 = max(0, math.floor(min(p[0] for p in poly)) - 3), max(0, math.floor(min(p[1] for p in poly)) - 3)
    x1, y1 = min(1240, math.ceil(max(p[0] for p in poly)) + 3), min(1080, math.ceil(max(p[1] for p in poly)) + 3)
    return [x0, y0, x1 - x0, y1 - y0]


def fx_plan(cuts):
    stages = [[[(0, 0), (1240, 0), (1240, 1080), (0, 1080)]]]
    for a, b in cuts[:4]:
        stages.append([part for poly in stages[-1] for part in fx_split(poly, a, b)])
    rng = __import__("random").Random(7)
    shards = []
    for i, poly in enumerate(stages[-1]):
        todo = [poly]
        while todo:
            p = todo.pop()
            if fx_area(p) <= FX_SHARD_AREA:
                shards.append((i, p))
                continue
            cx, cy = (sum(v[0] for v in p) / len(p), sum(v[1] for v in p) / len(p))
            for _ in range(20):
                ang = rng.uniform(0, math.pi)
                ox, oy = cx + rng.uniform(-30, 30), cy + rng.uniform(-30, 30)
                parts = fx_split(p, (ox, oy), (ox + math.cos(ang), oy + math.sin(ang)))
                if len(parts) == 2 and min(map(fx_area, parts)) > FX_SHARD_AREA / 5:
                    break
            todo += parts if len(parts) == 2 else []
            if len(parts) != 2:
                shards.append((i, p))
    return stages[1:], shards


FX_STREAK = [80, 80, 80, 80, 150]  # the streaks' widths (KirbyFx::StreakWidth), the last blow's wider
FX_WIDE_STREAK = 400  # Magolor's first cut, after the swords pushed against each other (the module's FxWideStreak)
FX_ALPHA = [f"#{round(k * 255 / 15):02X}FFFFFF" for k in range(16)]
FX_STREAK_FRAMES = 4  # KirbyFx::StreakTicks


# The Epilogue's last blow (user 2026-10-26, KirbyModule's FinaleStep): the gash carved from the top down along this
# line (its opening as the cut's width), its picture's box (the light reaches 250 px past the opening), the blade's
# point, and the aura's square round the gauge (KirbyModes::GaugeAuraSize)
FIN_CUT, FIN_OPEN = ((655, -40), (585, 1120)), 150
FIN_GASH_BOX, FIN_TIP, GAUGE_AURA = [260, 0, 720, 1080], 260, 760


def fin_halves():
    # the page's two halves either side of the opening (its sides measured square to the cut)
    (ax, ay), (bx, by) = FIN_CUT
    k = FIN_OPEN / 2 * math.hypot(bx - ax, by - ay) / (by - ay)
    x_at = lambda y: ax + (y - ay) * (bx - ax) / (by - ay)
    return [[(0, 0), (x_at(0) - k, 0), (x_at(1080) - k, 1080), (0, 1080)],
            [(x_at(0) + k, 0), (1240, 0), (1240, 1080), (x_at(1080) + k, 1080)]]


def fin_shaken(widgets):
    # the Epilogue page trembles while Magolor's sword pushes against the Crown: as fx_shaken, on efx.dx / efx.dy
    # (the module adds the shake to the offsets the page already has, FinaleShakeOffsets)
    out = []
    for w in widgets:
        if w.get("type") == "image" and w["rect"][2] > 1:
            w = {"x_bind": "efx.dx", "y_bind": "efx.dy"} | w
        out.append(w)
    return out


def finale_layer():
    def gen(key):
        return f"module:kirby:gen/files/{key}"

    img = {"type": "image", "color": WHITE, "src": ""}
    # the starry sky once the halves fall, kept to the credits; the halves, cut from the page as it was; the gash a bar
    # filling downward with Magolor's descent; the blade's point at its lower end
    widgets = [img | {"rect": [0, 0, 1240, 1080], "src": gen("wsky"), "hide_bind": "efx.sky", "hide_eq": 0, "input_block": True}]
    for i, poly in enumerate(fin_halves()):
        n = f"efx.h.{i}"
        widgets.append(img | {"rect": fx_box(poly), "src_bind": n, "x_bind": n + ".x", "y_bind": n + ".y", "rotate_bind": n + ".r",
                              "hide_bind": "efx.fall", "hide_eq": 0})
    widgets.append({"type": "bar", "rect": FIN_GASH_BOX, "bind": "efx.carve", "max": 1000, "fill_dir": "down", "image": gen("efin/gash"),
                    "color": "#00000000", "bg": "#00000000", "frame": 0, "hide_bind": "efx.gash", "hide_eq": 0})
    widgets.append(img | {"rect": [0, 0, FIN_TIP, FIN_TIP], "src": gen("efin/tip"), "x_bind": "efx.tip.x", "y_bind": "efx.tip.y",
                          "hide_bind": "efx.tip", "hide_eq": 0})
    return widgets


def fx_shaken(widgets):
    # the page shakes as a whole: a widget without an offset of its own takes fx.dx / fx.dy, the module adds
    # the shake to the offsets it publishes (KirbyFx::ShakeX / ShakeY); the ground's colour stays put
    out = []
    for w in widgets:
        if w.get("type") == "image" and w["rect"][2] > 1:
            w = {"x_bind": "fx.dx", "y_bind": "fx.dy"} | w
        out.append(w)
    return out


def fx_layer():
    plans = [fx_plan(cuts) for cuts in FX_SETS]
    img = {"type": "image", "color": WHITE, "src": ""}
    # the starry sky behind the pieces, from the first cut until the room changes (fading back to the page)
    widgets = [img | {"rect": [0, 0, 1240, 1080], "src": "module:kirby:gen/files/wsky", "hide_bind": "fx.sky", "hide_eq": 0,
                      "tint_bind": "fx.sky", "tint_colors": FX_ALPHA}]
    # each set's pieces after each cut (fx.show = set * 8 + cut), then its shards (set * 8 + 5)
    for s, (stages, shards) in enumerate(plans):
        for k, pieces in enumerate(stages, 1):
            for i, poly in enumerate(pieces):
                n = f"fx.p.{k}.{i}"
                widgets.append(img | {"rect": fx_box(poly), "src_bind": n, "x_bind": n + ".x", "y_bind": n + ".y", "rotate_bind": n + ".r",
                                      "hide_bind": "fx.show", "keep_min": s * 8 + k, "keep_max": s * 8 + k})
        for i, (_, poly) in enumerate(shards):
            n = f"fx.s.{i}"
            widgets.append(img | {"rect": fx_box(poly), "src_bind": n, "x_bind": n + ".x", "y_bind": n + ".y", "rotate_bind": n + ".r",
                                  "scale_bind": n + ".s", "tint_bind": n + ".a", "tint_colors": FX_ALPHA,
                                  "hide_bind": "fx.show", "keep_min": s * 8 + 5, "keep_max": s * 8 + 5})
        for j, ((ax, ay), (bx, by)) in enumerate(FX_SETS[s] + FX_SETS[s][:1]):
            # (then the first cut's wide streak, its own widget)
            wide = j == len(FX_STREAK)
            length, width, n = math.hypot(bx - ax, by - ay), FX_WIDE_STREAK if wide else FX_STREAK[j], f"fx.w.{s}" if wide else f"fx.t.{s}.{j}"
            cx, cy = (ax + bx) / 2, (ay + by) / 2
            widgets.append(img | {"rect": [round(cx - length / 2), round(cy - width / 2), round(length), width],
                                  "rotate": math.degrees(math.atan2(by - ay, bx - ax)), "src_bind": n, "x_bind": n + ".x",
                                  "y_bind": n + ".y", "rotate_bind": n + ".r", "tint_bind": n + ".a", "tint_colors": FX_ALPHA,
                                  "hide_bind": n + ".a", "hide_eq": 0})
    widgets.append(img | {"rect": [0, 0, 1240, 1080], "src": "module:kirby:gen/files/fx/white", "tint_bind": "fx.flash",
                          "tint_colors": FX_ALPHA, "hide_bind": "fx.flash", "hide_eq": 0})
    # every picture of the cut page handed to the host before its cut, so none shows blank
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": "", "hide_bind": "fx.preload", "hide_eq": 0}
    widgets += [clear | {"src_bind": f"fx.p.{k}.{i}"} for k in range(1, 5) for i in range(max(len(p[0][k - 1]) for p in plans))]
    widgets += [clear | {"src_bind": f"fx.s.{i}"} for i in range(max(len(p[1]) for p in plans))]
    widgets += [clear | {"src_bind": f"fx.pre.{j}.{f}"} for j in range(len(FX_STREAK)) for f in range(FX_STREAK_FRAMES)]
    widgets += [clear | {"src_bind": f"fx.prew.{f}"} for f in range(FX_STREAK_FRAMES)]
    # the page takes no touch while it is cut
    widgets.append({"type": "rect", "rect": [0, 0, 1240, 1080], "bg": "#00000000", "color": "#00000000", "input_block": True,
                    "hide_bind": "fx.on", "hide_eq": 0})
    return widgets


def write_fx_plan(page):
    # shatter.txt: each set's cuts, pieces and shards, then the page's pictures and grounds (w lines, fields split by |)
    # as KirbyFx::ParsePlan reads them, so the module can draw the page it cuts
    pts = lambda poly: " ".join(f"{x:.2f},{y:.2f}" for x, y in poly)
    lines = []
    for cuts in FX_SETS:
        stages, shards = fx_plan(cuts)
        lines.append("set")
        lines += [f"cut {a[0]} {a[1]} {b[0]} {b[1]} {w}" for (a, b), w in zip(cuts, FX_STREAK)]
        for k, pieces in enumerate(stages):
            lines += [f"piece {k + 1} {' '.join(map(str, fx_box(p)))} {pts(p)}" for p in pieces]
        lines += [f"shard {i} {' '.join(map(str, fx_box(p)))} {pts(p)}" for i, p in shards]
    lines += page_lines(page)
    (OUT / "shatter.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def page_lines(page):
    # a page's pictures and grounds (w lines, fields split by |) as KirbyFx::ParsePlan reads them
    lines = []
    for w in page:
        kind = w.get("type", "label")
        if kind not in ("image", "rect") or w["rect"][2] <= 1 or w["rect"][3] <= 1:
            continue
        colour = w.get("bg", "#00000000") if kind == "rect" else w.get("tint", w.get("color", "#FFE6ECF2"))
        opt = lambda key: str(w[key]) if key in w else ""
        lines.append("|".join(["w", kind[0], ",".join(map(str, w["rect"])), colour[1:], w.get("src", ""), w.get("src_bind", ""),
                               w.get("hide_bind", ""), opt("keep_min"), opt("keep_max"), opt("hide_eq"), w.get("need_bind", ""),
                               (w.get("anim") or {}).get("bind", ""), w.get("x_bind", ""), opt("x_scale"), w.get("y_bind", ""),
                               opt("y_scale"), opt("rotate"), w.get("rotate_bind", ""), w.get("scale_bind", ""), w.get("tint_bind", ""),
                               ",".join(c[1:] for c in w.get("tint_colors", []))]))
    return lines


def write_fin_plan(page):
    # finale.txt: the last blow's cut and the page's two halves, then the Epilogue page's pictures (KirbyFx::ParsePlan)
    pts = lambda poly: " ".join(f"{x:.2f},{y:.2f}" for x, y in poly)
    (a, b) = FIN_CUT
    lines = ["set", f"cut {a[0]} {a[1]} {b[0]} {b[1]} {FIN_OPEN}"]
    lines += [f"piece 1 {' '.join(map(str, fx_box(p)))} {pts(p)}" for p in fin_halves()]
    lines += page_lines(page)
    (OUT / "finale.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


# The Arenas' page (user's sketch 2026-10-22, cards 2026-10-24): the Epilogue's bar in yellow, Pause and Remove
# ability upper left, the souvenir lower right; the player's card lower left (blue panel, figure, plate) level with
# the souvenir, the boss's card upper right (red panel, picture cut to it, plate) sliding in and out from the right.
# Controls in the stage page's tap order (pause, drop ability, souvenir): picture rect, touch rect.
ARENA_CONTROLS = [([60, 196, 296, 296], [75, 211, 266, 266]), ([396, 230, 236, 236], [409, 243, 210, 210]),
                  ([660, 591, 480, 480], [680, 611, 439, 439])]
ARENA_PANELS = [[0, 604, 640, 420], [640, 116, 600, 400]]  # KirbyModes::PlayerPanelSize, BossPanelSize
ARENA_PLATES = [[0, 936, 480, 104], [580, 456, 660, 100]]  # KirbyModes::PlayerPlateSize, BossPlateSize
# (the figures' pictures are their panels' size; the panels and plates are drawn longer past their outer side,
# KirbyModes::ArenaCardExtend, the player's to the left and the boss's to the right)
ARENA_EXTEND = 160


# the sparks' pictures round the cards, panel and plate (KirbyModes::VsSparksBox, from the panels)
ARENA_SPARKS = [[0, 0, 640, 456], [-80, 0, 680, 456]]


def arena_sparks(i):
    x, y, w, h = ARENA_SPARKS[i]
    return [ARENA_PANELS[i][0] + x, ARENA_PANELS[i][1] + y, w, h]


def arena_wide(r, player):
    x, y, w, h = r
    return [x - ARENA_EXTEND, y, w + ARENA_EXTEND, h] if player else [x, y, w + ARENA_EXTEND, h]
ARENA_CARD_MS = 300
ARENA_VS = [420, 340, 400, 400]  # KirbyModes::VsBurstAt, VsSize


def arena_page():
    fade = {"bind": "play.vis", "group": "arena_controls", "from": "fade", "ms": PLAY_FADE_MS, "easing": "ease_in_out",
            "box": [0, 0, 1240, 1080]}
    image = {"type": "image", "color": WHITE, "anim": fade}
    widgets = background()
    # Magolor Soul's sweep takes the page apart element by element (SOUL_ELEMENTS): each one's widgets noted as built
    groups = {SOUL_GROUND: list(widgets)}
    widgets.append(image | {"rect": EPI_BAR, "src": "", "src_bind": "arena.bar"})
    widgets.append(image | {"rect": EPI_BATTERY, "src": "file:art/battery.png", "hide_bind": "arena.battery", "hide_eq": 0})
    widgets.append(ARENA_TIME | {"anim": fade})
    groups[5] = widgets[-3:-1]
    # the VS scene (KirbyModes::VsScene) moves the cards, fades them (16 steps) and shows the burst and the sparks
    vs_alpha = {"tint_bind": "arena.vs.alpha", "tint_colors": [f"#{a * 17:02X}FFFFFF" for a in range(16)]}
    moved = [{"x_bind": f"arena.{who}.dx", "y_bind": f"arena.{who}.dy"} | vs_alpha for who in ("p", "b")]
    card = {"type": "image", "color": WHITE, "anim": {"bind": "arena.card_on", "group": "arena_card", "from": "right", "ms": ARENA_CARD_MS,
                                                      "easing": "ease_in_out", "box": [0, 0, 1240, 1080]}} | moved[1]
    # the player's card fades with the controls, but stays through the VS scene
    pcard = {"type": "image", "color": WHITE, "anim": {"bind": "arena.pcard_on", "group": "arena_pcard", "from": "fade", "ms": PLAY_FADE_MS,
                                                       "easing": "ease_in_out", "box": [0, 0, 1240, 1080]}} | moved[0]
    sparks = {"type": "image", "color": WHITE, "src": "", "hide_bind": "arena.sparks_on", "hide_eq": 0}
    groups[4] = [card | {"rect": arena_wide(ARENA_PANELS[1], False), "src": "", "src_bind": "arena.panel.1"},
                 card | {"rect": ARENA_PANELS[1], "src": "", "src_bind": "arena.boss"},
                 card | {"rect": arena_wide(ARENA_PLATES[1], False), "src": "", "src_bind": "arena.plate.1"}]
    groups[6] = [pcard | {"rect": arena_wide(ARENA_PANELS[0], True), "src": "", "src_bind": "arena.panel.0"},
                 pcard | {"rect": ARENA_PANELS[0], "src": "", "src_bind": "arena.kirby", "hide_bind": "arena.kirby_on", "hide_eq": 0},
                 pcard | {"rect": arena_wide(ARENA_PLATES[0], True), "src": "", "src_bind": "arena.plate.0"}]
    widgets += groups[4] + groups[6] + [sparks | moved[1] | {"rect": arena_sparks(1), "src_bind": "arena.spark.1"},
                                        sparks | moved[0] | {"rect": arena_sparks(0), "src_bind": "arena.spark.0"}]
    vs = {"type": "image", "color": WHITE, "rect": ARENA_VS, "src": "", "scale_bind": "arena.vs.scale", "hide_bind": "arena.vs_on",
          "hide_eq": 0} | vs_alpha
    widgets += [vs | {"src_bind": "arena.vs"}, vs | {"src_bind": "arena.spark.2", "hide_bind": "arena.sparks_on"}]
    (pause, _), (drop, _), (souv, _) = ARENA_CONTROLS
    # Pause gives way to Continue while the game's pause menu is open; the drop button greyed out with nothing to drop
    groups[3] = [image | {"rect": pause, "src": "", "src_bind": "play.rbtn.0", "scale_bind": "play.scale.0", "hide_bind": "play.paused",
                          "hide_eq": 1}]
    groups[2] = [image | {"rect": drop, "src": "", "src_bind": "play.rbtn.1", "scale_bind": "play.scale.1", "hide_bind": "play.vis_drop",
                          "hide_eq": 0},
                 image | {"rect": drop, "src": "", "src_bind": "play.rbtn.2", "scale_bind": "play.scale.1", "hide_bind": "play.vis_nodrop",
                          "hide_eq": 0}]
    groups[1] = [image | {"rect": souv, "src": "", "src_bind": "play.souv", "scale_bind": "play.scale.2"}]
    groups[0] = colour_ribbon(fade)
    widgets += groups[3] + groups[2] + groups[1] + groups[0]
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    widgets += [clear | {"src_bind": key} for key in ("arena.bar_next", "play.souv_next", "play.rbtn.3")]
    widgets.append(boost("play.boost"))
    touch = {"type": "button", "text": "", "bg": "#00000000", "color": "#00000000"}
    widgets += [touch | {"rect": r, "on_tap": f"play_tap_{i}"} for i, (_, r) in enumerate(ARENA_CONTROLS)]
    widgets.append(touch | {"rect": COLOUR_RIBBON, "on_tap": "col_tap_0", "need_bind": "col.kirby"})
    widgets += soul_layer(write_soul_plan(groups))
    widgets.append(rect(0, 0, 1240, 1080, PLAY_LOCK) | {"hide_bind": "play.lock", "hide_eq": 0})
    # (not while Magolor Soul's sweep has the page)
    paused = {"hide_bind": "play.paused", "hide_eq": 0, "need_bind": "soul.calm"}
    widgets.append(image | {"rect": pause, "src": "", "src_bind": "play.rbtn.3", "scale_bind": "play.scale.resume"} | paused)
    widgets.append(touch | {"rect": ARENA_CONTROLS[0][1], "on_tap": "play_resume_tap"} | paused)
    return widgets + colour_layer()



# Magolor Soul's second form on the True Arena's page (user's design 2026-11-01, KirbyModule's SoulStep): the two sand
# tornadoes take the page's elements one by one (the first the ribbon, the souvenir, Drop and Pause, the second the
# cards and the bar), the ground left still behind them; the giant lasers burn the ground away to the starry sky;
# Magolor's words come and go over it; at 15% of its life the scream, then the hand circle that gives the Ultra Sword
SOUL_ELEMENTS = 7  # 0 ribbon, 1 souvenir, 2 drop, 3 pause; 4 boss card, 5 bar, 6 player card
SOUL_GROUND = 7
SOUL_WORDS = 4
SOUL_ALPHA = [f"#{a * 17:02X}FFFFFF" for a in range(16)]


def write_soul_plan(groups):
    # soul.txt: per element (and the ground) its box, then its pictures and grounds as KirbyFx::ParsePlan reads them
    boxes, lines = [], []
    for g in list(range(SOUL_ELEMENTS)) + [SOUL_GROUND]:
        rects = [w["rect"] for w in groups[g] if w.get("type", "label") in ("image", "rect")]
        x0, y0 = min(r[0] for r in rects), min(r[1] for r in rects)
        x1, y1 = max(r[0] + r[2] for r in rects), max(r[1] + r[3] for r in rects)
        if g == SOUL_GROUND:
            x0, y0, x1, y1 = 0, 0, 1240, 1080
        boxes.append([x0, y0, x1 - x0, y1 - y0])
        lines.append(f"group {g} {x0} {y0} {x1 - x0} {y1 - y0}")
        lines += page_lines(groups[g])
    (OUT / "soul.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return boxes[:SOUL_ELEMENTS]


def soul_layer(boxes):
    img = {"type": "image", "color": WHITE, "src": ""}
    faded = lambda n: {"tint_bind": n, "tint_colors": SOUL_ALPHA, "hide_bind": n, "hide_eq": 0}
    widgets = [img | {"rect": r, "src_bind": f"soul.patch.{i}", "hide_bind": f"soul.patch_on.{i}", "hide_eq": 0} for i, r in enumerate(boxes)]
    widgets += [img | {"rect": r, "src_bind": f"soul.piece.{i}", "x_bind": f"soul.e{i}.x", "y_bind": f"soul.e{i}.y",
                       "rotate_bind": f"soul.e{i}.r", "scale_bind": f"soul.e{i}.s"} | faded(f"soul.e{i}.a") for i, r in enumerate(boxes)]
    widgets += [img | {"rect": [190, -110, 860, 1300], "src_bind": f"soul.tor.{k}", "x_bind": f"soul.tor.{k}.x",
                       "hide_bind": f"soul.tor_on.{k}", "hide_eq": 0} for k in range(2)]
    widgets.append(img | {"rect": [0, 0, 1240, 1080], "src": "module:kirby:gen/files/wsky", "hide_bind": "soul.sky", "hide_eq": 0})
    widgets.append(img | {"rect": [580, 0, 80, 1080], "src_bind": "soul.v", "hide_bind": "soul.v_on", "hide_eq": 0})
    beam = img | {"src_bind": "soul.beam", "y_bind": "soul.beam.y", "tint_bind": "soul.beam.a", "tint_colors": SOUL_ALPHA,
                  "hide_bind": "soul.beam_on", "hide_eq": 0}
    widgets += [beam | {"rect": [-100, 0, 720, 1340]}, beam | {"rect": [620, 0, 720, 1340], "flip_x": True}]
    widgets += [img | {"rect": [0, 0, 640, 120], "src_bind": f"soul.w.{k}", "x_bind": f"soul.w.{k}.x", "y_bind": f"soul.w.{k}.y",
                       "rotate_bind": f"soul.w.{k}.r"} | faded(f"soul.w.{k}.a") for k in range(SOUL_WORDS)]
    widgets.append(img | {"rect": [20, 420, 1200, 240], "src_bind": "soul.scream", "x_bind": "soul.scream.x", "y_bind": "soul.scream.y",
                          "rotate_bind": "soul.scream.r", "scale_bind": "soul.scream.s"} | faded("soul.scream.a"))
    # (the controls are gone: the page takes no touch but the circle's)
    widgets.append({"type": "rect", "rect": [0, 0, 1240, 1080], "bg": "#00000000", "color": "#00000000", "input_block": True,
                    "hide_bind": "soul.block", "hide_eq": 0})
    widgets.append(img | {"rect": [220, 140, 800, 800], "src": "module:kirby:gen/files/soulring", "rotate_bind": "soul.ring.r",
                          "scale_bind": "soul.ring.s"} | faded("soul.ring.a"))
    widgets.append(img | {"rect": [360, 465, 520, 150], "src_bind": "soul.tap", "scale_bind": "soul.tap.s"} | faded("soul.tap.a"))
    widgets.append({"type": "button", "rect": [220, 140, 800, 800], "text": "", "bg": "#00000000", "color": "#00000000", "on_tap": "soul_tap",
                    "need_bind": "soul.can_tap"})
    widgets.append(img | {"rect": [0, 0, 1240, 1080], "src": "module:kirby:gen/files/fx/white"} | faded("soul.flash"))
    # the next pictures handed to the host ahead, so none shows blank
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": "", "hide_bind": "soul.ahead", "hide_eq": 0}
    widgets += [clear | {"src_bind": f"soul.next.{k}"} for k in range(6)]
    return widgets

# MainTitle N_Start at 1.5 px per layout unit (the module draws it from the game's own parts)
START_BTN = [275, 473, 690, 135]


def title_page():
    x, y, w, h = START_BTN
    return [
        {"type": "image", "color": WHITE, "rect": [0, 0, 1240, 1080], "src": "", "src_bind": "title.bg"},
        # mirrors the game's own button: its Show/Push/Hide scale and fade
        {"type": "image", "color": WHITE, "rect": START_BTN, "src": "", "src_bind": "title.btn",
         "scale_bind": "title.btn_scale", "tint_bind": "title.btn_alpha",
         "tint_colors": [f"#{a * 17:02X}FFFFFF" for a in range(16)]},
        {"type": "button", "rect": [x + 20, y, w - 40, h], "text": "", "bg": "#00000000", "color": "#00000000",
         "on_tap": "title_start"},
        boost("title.boost"),
    ] + files_preload()


# The host republishes a page at 30 Hz unless one of its own animations runs. Flipping `gate`
# moves an invisible 1 px group for a moment, so the module's short animations get 60 Hz.
def boost(gate):
    return {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": "", "src_bind": "fix.1",
            "anim": {"bind": gate, "group": gate, "from": "fade", "ms": 220}}


# The host loads a module picture when a widget draws it, one at a time: invisible 1 px copies of
# the file page's pictures on the dark and title pages get them loaded before the file select opens.
FILE_PRELOAD = 48  # predicted pictures the module publishes as files.pre.<n> (native PreloadSlots)
RAINBOW_STEPS = 90  # rainbow rows per turn (native RainbowSteps)


def files_preload():
    keys = [f"files.{i}.{part}" for i in range(3)
            for part in ("panel", "rainbow", "kirby", "label", "pct", "modes", "modepct")]
    keys += [f"files.pre.{n}" for n in range(FILE_PRELOAD)]
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    # cursor, dim, flash and the rainbow's steps (KirbyModule::FixedStep), handed over once drawn
    keys += [f"fix.{n}" for n in range(3 + RAINBOW_STEPS)]
    return [clear | {"src_bind": k} for k in keys]


# file select: three slots drawn by the module (keep in sync with native/kirby_module.cpp)
FILE_X, FILE_W, FILE_H, FILE_YS = 76, 1088, 292, [72, 394, 716]
FILE_CURSOR = 10


def files_page():
    widgets = [{"type": "image", "color": WHITE, "rect": [0, 0, 1240, 1080], "src": "", "src_bind": "title.bg"},
               boost("files.boost")]
    x, w, h, m = FILE_X, FILE_W, FILE_H, FILE_CURSOR
    for i, y in enumerate(FILE_YS):
        p = f"files.{i}."
        cx, cy = x + w // 2, y + h // 2

        # every part of a file scales about the button's centre, like the game's N_Push
        def part(rect, **extra):
            return {"type": "image", "color": WHITE, "rect": rect, "src": "", "scale_bind": p + "scale",
                    "pivot": [cx - rect[0], cy - rect[1]]} | extra

        widgets += [
            # a complete file's rainbow: one row per step, stretched under its panel
            part([x, y, w, h], src_bind=p + "rainbow", hide_bind=p + "complete", hide_eq=0),
            part([x, y, w, h], src_bind=p + "panel"),
            part([x + 64, y + 76, 960, 184], src_bind=p + "modes"),
            part([x + 64 + 480 - 190, y + 186, 380, 64], src_bind=p + "modepct"),
            part([x + w - 88 - 240, y + 16, 240, 60], src_bind=p + "pct"),
            # FileSelectBtnCursor: fades in, blinks, bursts on a touch selection
            part([x - m, y - m, w + 2 * m, h + 2 * m], src="", src_bind="fix.0",
                 scale_bind=p + "outline_scale", tint_bind=p + "outline",
                 tint_colors=[f"#{round(a * 255 / 31):02X}FFFFFF" for a in range(32)], hide_bind=p + "outline", hide_eq=0),
            part([x + 86, y + 8, 63, 72], src_bind=p + "kirby"),
            part([x + 152, y + 20, 240, 54], src_bind=p + "label"),
            # N_Unfocus darkens the other files; P_Frash flashes the chosen one
            part([x, y, w, h], src="", src_bind="fix.1", hide_bind=p + "dim", hide_eq=0),
            part([x, y, w, h], src="", src_bind="fix.2", tint_bind=p + "flash",
                 tint_colors=[f"#{round(a * 50 / 15):02X}FFFFFF" for a in range(16)], hide_bind=p + "flash", hide_eq=0),
            # the touch area is the button as drawn when unfocused (0.92, inside the texture's margin)
            {"type": "button", "rect": [x + 48, y + 24, w - 96, h - 48], "text": "", "bg": "#00000000",
             "color": "#00000000", "on_tap": f"file_tap_{i}"},
        ]
    # the chosen file's mode page, loaded before the mode select opens
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF"}
    widgets += max_preload()
    widgets += [clear | {"src": "", "src_bind": "modes.page"}, clear | {"src": "", "src_bind": "modes.level"},
                clear | {"src": MAGOLOR_SRC}]
    return widgets


# mode select: one picture drawn by the module, Magolor from the player's own file (native
# KirbyModes::MagolorBox)
MAGOLOR = [851, 252, 206, 206]
MAGOLOR_LEVEL = [961, 395, 114, 54]  # native KirbyModes::LevelBox, over the picture
MAGOLOR_SRC = "module:kirby:gen/files/magolor"  # cut out of the game's own pause picture at runtime


def modes_page():
    return [
        {"type": "image", "color": WHITE, "rect": [0, 0, 1240, 1080], "src": "", "src_bind": "modes.page"},
        {"type": "image", "color": WHITE, "rect": MAGOLOR, "src": MAGOLOR_SRC, "hide_bind": "modes.magolor", "hide_eq": 0},
        {"type": "image", "color": WHITE, "rect": MAGOLOR_LEVEL, "src": "", "src_bind": "modes.level",
         "hide_bind": "modes.magolor", "hide_eq": 0},
    ] + max_preload() + [{"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF",
                          "src": "module:kirby:gen/files/mainmenu"}]


# the main mode's own menu: one picture drawn by the module, and the section under the cursor in two
# sets of pictures (window and title): the windows slide past each other, the titles crossfade; the
# runtime slides a widget group from one fixed side, so each set has a group per side and the module
# shows the one it needs
ADV_PANEL = [28, 184, 1184, 828]  # KirbyModes::PanelBox
ADV_TITLE = [390, 8, 480, 66]  # KirbyModes::TitleBox
ADV_PANEL_BOX = [0, 181, 1240, 847]  # between the bars
ADV_SLIDE_MS = 300
MENU_TAP = [238, 478, 764, 240]  # the small button (KirbyModes::MenuButtonAt, 442 x 146 at 1.6 x 1.08)
PULSE = ["#%02XFFFFFF" % round(255 * i / 15) for i in range(16)]  # the cursor's alpha steps
EPI_MAGOLOR = [187, 322, 300, 300]  # KirbyModes::EpilogueMagolor
EPI_LEVEL = [357, 560, 130, 62]  # KirbyModes::EpilogueLevel


def main_menu_page():
    widgets = [{"type": "image", "color": WHITE, "rect": [0, 0, 1240, 1080], "src": "module:kirby:gen/files/mainmenu"}]
    for s in "ab":
        for side in ("left", "right", "top", "bottom"):
            widgets.append({"type": "image", "color": WHITE, "rect": ADV_PANEL, "src": "", "src_bind": f"adv.{s}.panel",
                            "hide_bind": f"adv.{s}.{side}", "hide_eq": 0, "scale_bind": f"adv.{s}.scale",
                            "anim": {"bind": f"adv.{s}.open", "group": f"adv_{s}_{side}", "from": side,
                                     "ms": ADV_SLIDE_MS, "easing": "ease_in_out", "box": ADV_PANEL_BOX}})
            # a small button's cursor outline, pulsing over it
            widgets.append({"type": "image", "color": WHITE, "rect": ADV_PANEL, "src": "", "src_bind": f"adv.{s}.cursor",
                            "hide_bind": f"adv.{s}.{side}_b", "hide_eq": 0, "scale_bind": f"adv.{s}.scale",
                            "tint_bind": f"adv.{s}.pulse", "tint_colors": PULSE,
                            "anim": {"bind": f"adv.{s}.open", "group": f"adv_{s}_{side}", "from": side,
                                     "ms": ADV_SLIDE_MS, "easing": "ease_in_out", "box": ADV_PANEL_BOX}})
            # on the Epilogue's page, Magolor's picture and his level ride with it
            # (the picture's shadow: itself in translucent black, a little down and right)
            shadow = [EPI_MAGOLOR[0] + 6, EPI_MAGOLOR[1] + 8, EPI_MAGOLOR[2], EPI_MAGOLOR[3]]
            for rect, src in ((shadow, {"src": MAGOLOR_SRC, "tint": "#59000000"}), (EPI_MAGOLOR, {"src": MAGOLOR_SRC}),
                              (EPI_LEVEL, {"src": "", "src_bind": f"adv.{s}.level"})):
                widgets.append({"type": "image", "color": WHITE, "rect": rect, **src,
                                "hide_bind": f"adv.{s}.{side}_m", "hide_eq": 0,
                                "anim": {"bind": f"adv.{s}.open", "group": f"adv_{s}_{side}", "from": side,
                                         "ms": ADV_SLIDE_MS, "easing": "ease_in_out", "box": ADV_PANEL_BOX}})
        widgets.append({"type": "image", "color": WHITE, "rect": ADV_TITLE, "src": "", "src_bind": f"adv.{s}.title",
                        "hide_bind": f"adv.{s}.shown", "hide_eq": 0,
                        "anim": {"bind": f"adv.{s}.open", "group": f"adv_{s}_title", "from": "fade",
                                 "ms": ADV_SLIDE_MS, "easing": "ease_in_out"}})
    # a tap on the small button under the cursor enters it (and plays its Push; A on the top screen does not)
    widgets.append({"type": "button", "rect": MENU_TAP, "text": "", "bg": "#00000000", "color": "#00000000",
                    "on_tap": "menu_tap", "hide_bind": "adv.tap_on", "hide_eq": 0})
    # (last: the host loads the page's own pictures first)
    widgets += epi_preload()
    return widgets


# Magolor's rainbow MAX, one picture per rainbow row, loaded ahead like the files' rainbow
def max_preload():
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    return [clear | {"src_bind": f"maxpre.{n}"} for n in range(RAINBOW_STEPS)]


# KirbyModes::MapSlots: Pop Star's worlds 1-5 and Lor, Halcandra's worlds 6-7 and Lor, the other planet's
# icon (x, y, picture size, disc diameter, icon); the cursor ring reaches CURSOR_MARGIN px past the disc
LEVEL_ICON = "module:kirby:gen/files/lvicon/{}"  # LvMapIcon<name>^u, drawn ahead
WORLD_SLOTS = [(618, 427, 108, 97.2, "Lv1"), (919, 565, 108, 97.2, "Lv2"), (835, 857, 108, 97.2, "Lv3"),
               (379, 867, 108, 97.2, "Lv4"), (303, 577, 108, 97.2, "Lv5"), (622, 645, 171, 153.9, "Ship"),
               (672, 820, 126, 113.4, "Lv6"), (628, 557, 126, 113.4, "Lv7"), (362, 921, 126, 113.4, "Ship"),
               (1154, 1000, 128, 124, None)]  # KirbyModes::OtherPlanetSize
CURSOR_MARGIN = 10
WMAP_FADE_MS = 200


# the world map's world selection: the module's picture (sky, planet, bar) and the planet's icons in two
# sets that crossfade when Kirby flies to the other planet; each icon bounces when the cursor reaches it
# and pushes when touched (Kirby then enters it, or flies to the other planet); the cursor ring pulses over it
def world_map_page():
    def centred(x, y, size):
        return [round(x - size / 2), round(y - size / 2), round(size), round(size)]

    widgets = []
    for s in "ab":
        fade = {"bind": f"wmap.{s}.open", "group": f"wmap_{s}", "from": "fade", "ms": WMAP_FADE_MS,
                "easing": "ease_in_out", "box": [0, 0, 1240, 1080]}
        widgets.append({"type": "image", "color": WHITE, "rect": [0, 0, 1240, 1080], "src": "", "src_bind": f"wmap.{s}.page",
                        "anim": fade})
        for i, (x, y, size, _, icon) in enumerate(WORLD_SLOTS[:-1]):
            widgets.append({"type": "image", "color": WHITE, "rect": centred(x, y, size), "src": LEVEL_ICON.format(icon),
                            "hide_bind": f"wmap.{s}.icon.{i}", "hide_eq": 0, "scale_bind": f"wmap.scale.{i}", "anim": fade})
    x, y, size, _, _ = WORLD_SLOTS[-1]
    other = len(WORLD_SLOTS) - 1
    widgets.append({"type": "image", "color": WHITE, "rect": centred(x, y, size), "src": "", "src_bind": "wmap.other",
                    "hide_bind": f"wmap.icon.{other}", "hide_eq": 0, "scale_bind": f"wmap.scale.{other}"})
    for i, (x, y, _, disc, _) in enumerate(WORLD_SLOTS):
        widgets.append({"type": "image", "color": WHITE, "rect": centred(x, y, math.ceil(disc + 2 * CURSOR_MARGIN)),
                        "src": f"module:kirby:gen/files/wcur/{round(disc * 10)}", "hide_bind": f"wmap.cur.{i}", "hide_eq": 0,
                        "scale_bind": f"wmap.scale.{i}", "tint_bind": "wmap.pulse", "tint_colors": PULSE})
    for i, (x, y, _, disc, _) in enumerate(WORLD_SLOTS):
        widgets.append({"type": "button", "rect": centred(x, y, disc), "text": "", "bg": "#00000000", "color": "#00000000",
                        "on_tap": f"wmap_tap_{i}", "hide_bind": f"wmap.icon.{i}", "hide_eq": 0})
    return widgets


def spray_popup():
    # Kirby's colours a goal unlocked (KirbyModule's SprayStep), over the dimmed world map or hub page, and its button
    fade = {"bind": "spray.popup", "group": "spray_popup", "from": "fade", "ms": PLAY_FADE_MS, "easing": "ease_in_out",
            "box": [0, 0, 1240, 1080]}
    pop = {"type": "image", "color": WHITE, "src": "", "anim": fade}
    (x, y, w, h), (bx, by) = EPI_POPUP, EPI_POPUP_BUTTON
    return [rect(0, 0, 1240, 1080, PLAY_LOCK) | {"hide_bind": "spray.popup", "hide_eq": 0, "input_block": True},
            pop | {"rect": [x, y, w, h], "src_bind": "spray.pop"},
            pop | {"rect": [x + bx, y + by, 500, 122], "src_bind": "spray.popbtn", "scale_bind": "spray.scale.pop"},
            {"type": "button", "rect": [x + bx + 20, y + by + 8, 460, 106], "text": "", "bg": "#00000000", "color": "#00000000",
             "on_tap": "spray_pop_tap", "need_bind": "spray.popup"}]


# Merry Magoland's plaza page (user's sketch 2026-11-04; KirbyMago, KirbyGates, native MagolandStep): the title bar with
# its arrows (they change section, wrapping round), the section's name and dots, the missions count; the Minigames section's
# ten attraction doors (GateDecoModel's order, where the sketch has them) and Samurai Kirby 100's globe; the colour ribbon
MAGO_ARROWS = [[12, 14, 116, 116], [759, 14, 116, 116]]  # KirbyMago::ArrowSize
MAGO_TITLE, MAGO_DOTS, MAGO_COUNT = [165, 6, 560, 130], [353, 158, 164, 36], [880, 0, 360, 128]  # KirbyMago::*Size
MAGO_DOOR = (330, 216)  # KirbyGates::DoorSize
MAGO_DOORS = [(155, 531), (925, 929), (931, 374), (621, 285), (155, 743), (1085, 550), (310, 919), (311, 353), (1084, 744), (623, 983)]
MAGO_GLOBE = [404, 366, 380, 480]  # KirbyGates::GlobeSize, centred on (594, 606)
MAGO_TAPS = [0, 1, 2, 3] + [10 + i for i in range(len(MAGO_DOORS))] + [20]  # native MagoTaps
# the Missions section (user's sketch 2026-11-09; KirbyMago sizes, native MagoMissionsStep): the set bar with its arrows
# and the pins' count, the sets' dots, the set's done count; the rows in the scroll region mago.list (rows 1 px tall
# there: its follow_bind sets the offset the scrollbar's drag asks for), each with its pin; the scrollbar; the card
MIS_BAR, MIS_PINS, MIS_DOTS, MIS_CLEAR = [170, 185, 900, 92], [900, 195, 130, 72], [410, 272, 420, 40], [530, 318, 180, 44]
MIS_ARROWS = [[92, 192, 60, 76], [1088, 192, 60, 76]]
MIS_LIST, MIS_ROW, MIS_PITCH, MIS_ROWS = [0, 372, 710, 708], [10, 378, 652, 100], 88, 20  # native MisTop, MisView, MisPitch, MagoRows
MIS_PIN = [508, 400, 56, 56]  # on the row's right, 33 px further on the selected one (mago.pinx.<i>)
MIS_BAR_W = 24  # the region's own scrollbar (its thumb and track the module's pictures), inside its rect's right edge
MIS_CARD = [738, 290, 480, 650]  # KirbyMago::CardSize, its paper clear of the spray's ribbon


# the Masks section: Remove and the 86 masks in the game's misaligned rows, 11 a row (native KirbyMago::MaskCentre)
MASK_SLOTS, MASK_COLS, MASK_PIC = 87, 11, 128
MASK_PITCH, MASK_LINE, MASK_SHIFT, MASK_LEFT, MASK_TOP = 117.9 * 0.9, 109.4 * 0.9, 36 * 0.9, 73.25, 256


def mask_centre(slot):
    row, col = divmod(slot, MASK_COLS)
    return MASK_LEFT + (row % 2) * MASK_SHIFT + col * MASK_PITCH, MASK_TOP + row * MASK_LINE


# the Cards section: the stamp card picture (KirbyMago::StampCardSize, centred on the page) and the bar under it
STAMP_CARD, STAMP_BAR = [620 - 643, 196, 1286, 724], [20, 946, 1080, 110]


def magoland_page():
    img = {"type": "image", "color": WHITE, "src": ""}
    touch = {"type": "button", "text": "", "bg": "#00000000", "color": "#00000000"}
    sec0 = {"hide_bind": "mago.sec0", "hide_eq": 0}
    globe = {"hide_bind": "mago.globe_on", "hide_eq": 0}
    w = [img | {"rect": [0, 0, 1240, 1080], "src_bind": "mago.bg"}]
    # the Missions' drags: the list's and the scrollbar's (a strip wider than the bar, for a finger), both scrolled by the
    # module (follow_bind), which so always knows the offset; the rows' and pins' taps pass through. Before the scrolled
    # rows: their culling changes the later widgets' indices, which a pan's target must keep.
    pan = {"type": "rect", "bg": "#00000000", "color": "#00000000", "need_bind": "mago.list.on"}
    w += [pan | {"rect": [MIS_LIST[0], MIS_LIST[1], MIS_LIST[2] - MIS_BAR_W - 16, MIS_LIST[3]], "on_pan": "mago_pan"},
          pan | {"rect": [MIS_LIST[2] - MIS_BAR_W - 16, MIS_LIST[1], MIS_BAR_W + 30, MIS_LIST[3]], "on_pan": "mago_slide"}]
    w += [img | {"rect": r, "src_bind": f"mago.arrow.{i}", "scale_bind": f"mago.scale.{i}"} for i, r in enumerate(MAGO_ARROWS)]
    w += [img | {"rect": MAGO_TITLE, "src_bind": "mago.title"}, img | {"rect": MAGO_DOTS, "src_bind": "mago.dots"},
          img | {"rect": MAGO_COUNT, "src_bind": "mago.count"}]
    for i, (x, y) in enumerate(MAGO_DOORS):
        w.append(img | sec0 | {"rect": [x - MAGO_DOOR[0] // 2, y - MAGO_DOOR[1] // 2, *MAGO_DOOR], "src_bind": f"mago.door.{i}",
                               "scale_bind": f"mago.scale.{10 + i}"})
    w.append(img | globe | {"rect": MAGO_GLOBE, "src_bind": "mago.globe", "scale_bind": "mago.scale.20"})
    sec1 = {"hide_bind": "mago.sec1", "hide_eq": 0}
    w += [img | sec1 | {"rect": MIS_BAR, "src_bind": "mago.mset"}, img | sec1 | {"rect": MIS_PINS, "src_bind": "mago.mpins", "x_bind": "mago.mpins.dx"},
          img | sec1 | {"rect": MIS_DOTS, "src_bind": "mago.mdots"}, img | sec1 | {"rect": MIS_CLEAR, "src_bind": "mago.mclear"}]
    w += [img | sec1 | {"rect": r, "src_bind": f"mago.marrow.{i}", "scale_bind": f"mago.scale.{2 + i}"} for i, r in enumerate(MIS_ARROWS)]
    # (only on their section: the rows' taps would otherwise catch the doors under them)
    rows = sec1 | {"repeat": MIS_ROWS, "repeat_dy": MIS_PITCH, "repeat_bind": "mago.rows", "scroll": "mago.list"}
    w.append(img | rows | {"rect": MIS_ROW, "src_bind": "mago.row.{i}", "scale_bind": "mago.rs.{i}"})
    w.append(img | rows | {"rect": MIS_PIN, "src_bind": "mago.pin.{i}", "x_bind": "mago.pinx.{i}", "need_bind": "mago.pin.on.{i}"})
    w.append(img | sec1 | {"rect": MIS_CARD, "src_bind": "mago.mcard"})
    sec2 = {"hide_bind": "mago.sec2", "hide_eq": 0}
    for i in range(MASK_SLOTS):
        x, y = mask_centre(i)
        w.append(img | sec2 | {"rect": [round(x - MASK_PIC / 2), round(y - MASK_PIC / 2), MASK_PIC, MASK_PIC], "src_bind": f"mago.mask.{i}",
                               "scale_bind": f"mago.mks.{i}", "x_bind": f"mago.mkx.{i}"})
    sec3 = {"hide_bind": "mago.sec3", "hide_eq": 0}
    w += [img | sec3 | {"rect": STAMP_CARD, "src_bind": "mago.scard"}, img | sec3 | {"rect": STAMP_BAR, "src_bind": "mago.sbar"}]
    w.append(img | {"rect": COLOUR_RIBBON, "src_bind": "col.ribbon", "scale_bind": "col.scale.ribbon", "need_bind": "col.kirby"})
    # the next name, dots and count, handed to the host before they show
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    w += [clear | {"src_bind": key} for key in ("mago.title_next", "mago.dots_next", "mago.count_next", "mago.mset_next", "mago.mpins_next",
                                                "mago.mdots_next", "mago.mclear_next", "mago.mcard_next", "mago.scard_next", "mago.sbar_next")]
    w.append(clear | {"src_bind": "mago.row.{i}_next", "repeat": MIS_ROWS, "repeat_dy": 0})
    w.append(clear | {"src_bind": "mago.mask.{i}_next", "repeat": MASK_SLOTS, "repeat_dy": 0})
    w.append(boost("mago.boost"))
    w += [touch | {"rect": [x + 8, y + 8, ww - 16, hh - 16], "on_tap": f"mago_tap_{i}"} for i, (x, y, ww, hh) in enumerate(MAGO_ARROWS)]
    w += [touch | sec0 | {"rect": [x - 125, y - 75, 250, 150], "on_tap": f"mago_tap_{10 + i}"} for i, (x, y) in enumerate(MAGO_DOORS)]
    w.append(touch | globe | {"rect": [434, 396, 320, 420], "on_tap": "mago_tap_20"})
    w += [touch | sec1 | {"rect": [x - 14, y - 6, ww + 28, hh + 12], "on_tap": f"mago_tap_{2 + i}"} for i, (x, y, ww, hh) in enumerate(MIS_ARROWS)]
    w.append(touch | rows | {"rect": [MIS_ROW[0], MIS_ROW[1] + 8, MIS_ROW[2] - 12, MIS_ROW[3] - 16], "on_tap": "mago_row_{i}"})
    w.append(touch | rows | {"rect": [MIS_PIN[0] - 10, MIS_PIN[1] - 10, MIS_PIN[2] + 20, MIS_PIN[3] + 20], "x_bind": "mago.pinx.{i}",
                             "need_bind": "mago.pin.on.{i}", "on_tap": "mago_pin_{i}"})
    for i in range(MASK_SLOTS):
        x, y = mask_centre(i)
        w.append(touch | sec2 | {"rect": [round(x - 46), round(y - 46), 92, 92], "on_tap": f"mago_mask_{i}"})

    w.append(touch | {"rect": COLOUR_RIBBON, "on_tap": "col_tap_0", "need_bind": "col.kirby"})
    w.append(rect(0, 0, 1240, 1080, PLAY_LOCK) | {"hide_bind": "mago.lock", "hide_eq": 0})
    return w + colour_layer()


# the page while an attraction's match is played (the user's sketch 2026-11-11): KirbyMago's game page, its pictures drawn
# by the module (GameStep); the rows and card are the Missions section's
GAME_TICKET, GAME_PINS, GAME_TOUR = [620 - 290, -14, 580, 210], [10, 168, 1220, 600], [10, 790, 1220, 262]
GAME_ROW_X, GAME_ROW_Y, GAME_ROW_PITCH = 24, 258, 96
GAME_CARD = [752, 180, 413, 559]  # KirbyMago::CardSize at 0.86
GAME_TOUR_X, GAME_TOUR_Y, GAME_TOUR_PITCH = 272, 951, 232  # the first card's centre, KirbyMago::TourPitch
GAME_CARD_W, GAME_CARD_H, GAME_CURSOR_W, GAME_CURSOR_H = 164, 160, 178, 174  # KirbyMago::TourCardSize, TourCursorSize


def game_page():
    img = {"type": "image", "color": WHITE, "src": ""}
    touch = {"type": "button", "text": "", "bg": "#00000000", "color": "#00000000"}
    tour = {"need_bind": "game.tour.on"}
    w = [img | {"rect": [0, 0, 1240, 1080], "src_bind": "game.bg"}, img | {"rect": GAME_PINS, "src_bind": "game.pinsbox"}]
    for i in range(5):
        w.append(img | {"rect": [GAME_ROW_X, GAME_ROW_Y + GAME_ROW_PITCH * i, 652, 100], "src_bind": f"game.row.{i}", "scale_bind": f"game.rs.{i}",
                        "need_bind": f"game.row.on.{i}"})
    w.append(img | {"rect": GAME_CARD, "src_bind": "game.card", "need_bind": "game.card.on"})
    w.append(img | {"rect": GAME_TOUR, "src_bind": "game.tourbox"})
    for i in range(4):
        x = GAME_TOUR_X + GAME_TOUR_PITCH * i
        w.append(img | tour | {"rect": [x - GAME_CARD_W // 2, GAME_TOUR_Y - GAME_CARD_H // 2, GAME_CARD_W, GAME_CARD_H], "src_bind": f"game.tcard.{i}"})
        if i < 3:
            w.append(img | tour | {"rect": [x + GAME_TOUR_PITCH // 2 - 14, GAME_TOUR_Y - 19, 28, 38], "src_bind": "game.tarrow"})
    w.append(img | tour | {"rect": [GAME_TOUR_X - GAME_CURSOR_W // 2, GAME_TOUR_Y - GAME_CURSOR_H // 2, GAME_CURSOR_W, GAME_CURSOR_H],
                           "src_bind": "game.tcursor", "x_bind": "game.cursor.x"})
    w.append(img | {"rect": GAME_TICKET, "src_bind": "game.ticket"})
    clear = {"type": "image", "rect": [0, 0, 1, 1], "tint": "#00FFFFFF", "src": ""}
    w += [clear | {"src_bind": key} for key in ("game.ticket_next", "game.pinsbox_next", "game.tourbox_next", "game.card_next")]
    w.append(clear | {"src_bind": "game.row.{i}_next", "repeat": 5, "repeat_dy": 0})
    w.append(clear | {"src_bind": "game.tcard.{i}_next", "repeat": 4, "repeat_dy": 0})
    w += [touch | {"rect": [GAME_ROW_X, GAME_ROW_Y + GAME_ROW_PITCH * i + 8, 640, 84], "on_tap": f"game_row_{i}", "need_bind": f"game.row.on.{i}"}
          for i in range(5)]
    return w


# attract demo and movie: the second screen goes dark and shows the game's logo bottom-right
LOGO = [800, 750, 400, 290]


def movie_cover():
    # a film in a game (KirbyModule's play.movie): the page goes dark at once (a fade showed the page's last picture), the
    # logo fades in, as on the attract demo; it takes no touch
    fade = {"bind": "play.movie", "group": "movie", "from": "fade", "ms": 250, "easing": "ease_in_out", "box": LOGO}
    return [rect(0, 0, 1240, 1080, "#FF000000") | {"hide_bind": "play.movie", "hide_eq": 0, "input_block": True},
            {"type": "image", "color": WHITE, "rect": LOGO, "src": "", "src_bind": "title.logo", "anim": fade}]


CREDIT_CANVAS = 1000  # a card's picture height (KirbyModule's CreditCanvas): the card at its top, transparent below


def credits_layer():
    # the DSMod's credits over the game's staff roll (KirbyModule's CreditsStep): black at once, the starry sky fading
    # in, then credits.txt's cards rising, each a 1000-wide picture moved up from below the page
    cards = sum(1 for line in (PKG / "credits.txt").read_text(encoding="utf-8").splitlines()
                if line.strip().startswith("["))
    fade = {"bind": "credits.on", "group": "credits", "from": "fade", "ms": 500, "easing": "ease_in_out", "box": [0, 0, 1240, 1080]}
    img = {"type": "image", "color": WHITE}
    # (no black when they follow the Epilogue's last blow, its sky already up: credits.black)
    return [rect(0, 0, 1240, 1080, "#FF000000") | {"hide_bind": "credits.black", "hide_eq": 0},
            img | {"rect": [0, 0, 1240, 1080], "src": "module:kirby:gen/files/wsky", "anim": fade, "input_block": True}] + [
        img | {"rect": [120, 1080, 1000, CREDIT_CANVAS], "src": f"module:kirby:gen/files/credits/{i}", "y_bind": f"credits.y.{i}",
               "hide_bind": f"credits.card.{i}", "hide_eq": 0} for i in range(cards)]


def attract_page():
    return [
        rect(0, 0, 1240, 1080, "#FF000000"),
        {"type": "image", "color": WHITE, "rect": LOGO, "src": "", "src_bind": "title.logo"},
    ] + files_preload()


def manifest(dev):
    actions = {
        # the stage page's controls: the module plays the push and presses the game's buttons (the
        # souvenir: X, its hold counter topped up so the item comes out at once)
        **{f"play_tap_{i}": {"kind": "module", "action": "play_tap", "argument": i, "enabled_bind": f"play.can.{i}"}
           for i in range(len(PLAY_CONTROLS))},
        "soul_tap": {"kind": "module", "action": "soul_tap", "enabled_bind": "soul.can_tap"},
        **colour_actions(),
        # the Options tab's controls (tap codes: OptionsStep in native/kirby_module.cpp)
        **{f"opt_tap_{i}": {"kind": "module", "action": "opt_tap", "argument": i, "enabled_bind": "opt.can"}
           for i in (0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22)},
        # a setting the module changed: its persisted flag is set on the next enforce tick (set, not toggled: a rule may
        # run twice while the gate is open)
        **{f"opt_set_{i}_{v}": {"kind": "flag", "flag": option_flag(name), "value": bool(v), "enabled_bind": f"opt.set.{i}.{v}"}
           for i, name in enumerate(OPTION_FLAGS) for v in (0, 1)},
        # their rules only run behind these runtime flags: one for any flag to store, one per block
        "store_on": {"kind": "flag", "flag": "kirby.store", "value": True, "enabled_bind": "store.on"},
        "store_off": {"kind": "flag", "flag": "kirby.store", "value": False},
        **{f"store_blk_{k}": {"kind": "flag", "flag": f"kirby.store.{k}", "value": True, "enabled_bind": f"store.blk.{k}"}
           for k in range(STORE_BLOCKS)},
        **{f"store_blk_off_{k}": {"kind": "flag", "flag": f"kirby.store.{k}", "value": False} for k in range(STORE_BLOCKS)},
        "play_resume_tap": {"kind": "module", "action": "play_resume", "enabled_bind": "play.paused"},
        # Magolor Epilogue's page: Pause, Magolor Cannon, Black Hole, the tabs, Heal, Magolor Blast; Continue
        **{f"epi_tap_{i}": {"kind": "module", "action": "epi_tap", "argument": i, "enabled_bind": f"epi.can.{i}"} for i in range(8)},
        "epi_resume_tap": {"kind": "module", "action": "epi_resume", "enabled_bind": "epi.resume"},
        **{f"epim_tap_{i}": {"kind": "module", "action": "epim_tap", "argument": i, "enabled_bind": "epim.can"} for i in range(2)},
        # the Epilogue's Options tab (tap codes: OptionsStep, 23 the gauge refill)
        **{f"epo_tap_{i}": {"kind": "module", "action": "epo_tap", "argument": i, "enabled_bind": "epo.can"}
           for i in (0, 1, 2, 3, 10, 11, 20, 21, 22, 23, 24, 25)},
        "epi_pop_tap": {"kind": "module", "action": "epi_pop", "enabled_bind": "epi.popup"},
        "spray_pop_tap": {"kind": "module", "action": "spray_pop", "enabled_bind": "spray.popup"},
        # the Abilities tab's arrows: the module plays their push and slides the moves page
        **{f"abl_tap_{i}": {"kind": "module", "action": "abl_tap", "argument": i, "enabled_bind": "abl.can"} for i in range(2)},
        # the Options tab's settings (persisted flags, read by the module as @flag:<name>)
        **{f"opt_{name}": {"kind": "flag", "flag": option_flag(name)} for name in OPTION_FLAGS},
        # each setting's value handed to the module (it cannot read flags), on the enforce rules below
        **{f"opt_sync_{i}_{v}": {"kind": "module", "action": "opt", "argument": i * 2 + v}
           for i in range(len(OPTION_FLAGS)) for v in (0, 1)},
        # the module presses buttons through the enforce rules: kirby.press opens the gate for one
        # tick and kirby.want_<button> picks which rule's press goes through
        "press_x": {"kind": "button", "button": "X", "frames": 8, "enabled_bind": "kirby.want_x"},
        "press_a": {"kind": "button", "button": "A", "frames": 4, "enabled_bind": "kirby.want_a"},
        "press_up": {"kind": "button", "button": "DUp", "frames": 4, "enabled_bind": "kirby.want_up"},
        "press_down": {"kind": "button", "button": "DDown", "frames": 4, "enabled_bind": "kirby.want_down"},
        # Black Hole: Up and X together
        "press_upx": {"kind": "button", "button": "DUp+X", "frames": 8, "enabled_bind": "kirby.want_upx"},
        **{f"press_{b.lower()}": {"kind": "button", "button": b, "frames": 4, "enabled_bind": f"kirby.want_{b.lower()}"}
           for b in ("Plus", "Minus", "Y")},
        # the world map's walk: longer presses, one step each
        **{f"press_map_{d}": {"kind": "button", "button": f"D{d.capitalize()}", "frames": 10,
                              "enabled_bind": f"kirby.want_map_{d}"} for d in ("up", "down", "left", "right")},
        # a tap plays the button's push and lets the module press A (a pad press does not animate)
        "title_start": {"kind": "module", "action": "title_start", "enabled_bind": "title.ready"},
        "menu_tap": {"kind": "module", "action": "menu_tap", "enabled_bind": "adv.tap_on"},
        **{f"file_tap_{i}": {"kind": "module", "action": "file_tap", "argument": i, "enabled_bind": "files.on"}
           for i in range(3)},
        **{f"wmap_tap_{i}": {"kind": "module", "action": "wmap_tap", "argument": i, "enabled_bind": "wmap.on"}
           for i in range(len(WORLD_SLOTS))},
        **{f"mago_tap_{i}": {"kind": "module", "action": "mago_tap", "argument": i, "enabled_bind": "mago.can"} for i in MAGO_TAPS},
        **{f"mago_{what}_{i}": {"kind": "module", "action": f"mago_{what}", "argument": i, "enabled_bind": "mago.can"}
           for what in ("row", "pin") for i in range(MIS_ROWS)},
        **{f"mago_mask_{i}": {"kind": "module", "action": "mago_mask", "argument": i, "enabled_bind": "mago.can"} for i in range(MASK_SLOTS)},
        **{f"game_row_{i}": {"kind": "module", "action": "game_row", "argument": i} for i in range(5)},
        "mago_slide": {"kind": "module", "action": "mago_slide", "argument": "$payload", "enabled_bind": "mago.can"},
        "mago_pan": {"kind": "module", "action": "mago_pan", "argument": "$payload", "enabled_bind": "mago.can"},
    }
    play = fx_shaken(play_page())
    write_fx_plan(play)
    epilogue = epilogue_page()
    write_fin_plan(epilogue)
    probe = []
    if dev:
        actions.update({name: {"kind": "module", "action": name} for name in RESEARCH_ACTIONS})
        # logs the canvas position of every touch that misses the real buttons
        actions["tap_probe"] = {"kind": "flag", "flag": "tap_probe"}
        probe = [{"type": "button", "rect": [0, 0, 1240, 1080], "text": "", "bg": "#00000000",
                  "color": "#00000000", "on_tap": "tap_probe"}]
    return {
        "format": 1,
        "title_id": "01006B601380E000",
        "name": "Kirby's Return to Dream Land Deluxe dual screen",
        "min_runtime": 17,
        "requires_module": True,
        "module": {"abi": 1, "build_ids": [BUILD_ID]},
        "canvas_w": 1240,
        "canvas_h": 1080,
        "background": "#FFB5F9FC",
        "font": "file:kirby_font.txt",
        "font_atlas": f"module:kirby:font/{FONT_SPEC}",
        "msbt": {
            alias: f"romfs:/msg/Ark/{{REGION}}_{{LANG}}/{path}.msbt"
            for alias, path in (("CarryItem", "Name/CarryItem"), ("Pause", "Pause"), ("Cmn", "Cmn"),
                                ("Option", "Option"))
        },
        # the module opens this gate for one tick per button it presses (see the press_* actions)
        "enforce_gate": {"point": "kirby.press", "max": 1},
        "flags": {**{option_flag(name): 0 for name in OPTION_FLAGS}, "kirby.store": 0,
                  **{f"kirby.store.{k}": 0 for k in range(STORE_BLOCKS)}},
        "persist_flags": [option_flag(name) for name in OPTION_FLAGS],
        "enforce": [{"action": b, "every_ms": 16} for b in ("press_x", "press_a", "press_up", "press_down", "press_map_up",
                                                             "press_map_down", "press_map_left", "press_map_right",
                                                             "press_plus", "press_minus", "press_y", "press_upx")] +
                   # sets first, so the values handed back are the new ones; a rule skipped on its flag costs nothing,
                   # a refused one is logged, so only the blocks with a flag to store run theirs
                   [{"action": "store_on", "every_ms": 16}] +
                   [rule for k in range(STORE_BLOCKS) for rule in
                    [{"action": f"store_blk_{k}", "every_ms": 16, "flag": "kirby.store", "value": True}] +
                    [{"action": f"opt_set_{i}_{v}", "every_ms": 16, "flag": f"kirby.store.{k}", "value": True}
                     for i in range(len(OPTION_FLAGS)) if store_block(i) == k for v in (0, 1)] +
                    [{"action": f"store_blk_off_{k}", "every_ms": 16, "flag": f"kirby.store.{k}", "value": True}]] +
                   [{"action": "store_off", "every_ms": 16, "flag": "kirby.store", "value": True}] +
                   [{"action": f"opt_sync_{i}_{v}", "every_ms": 16, "flag": option_flag(name), "value": bool(v)}
                    for i, name in enumerate(OPTION_FLAGS) for v in (0, 1)],
        "msbt_lang": LANGS,
        "msbt_lang_fallback": ["US", "English"],
        "actions": actions,
        # in order (on one tick the last switch wins): a page stays up until the next one is there (the
        # game's loading screens keep the last); entering the title menu during the attract demo lands on
        # the attract page
        "page_binds": [{"point": "play.on", "equals": 1,
                        "when_equal": {"page": "main", "transition": "fade", "duration_ms": 250}},
                       {"point": "wmap.on", "equals": 1,
                        "when_equal": {"page": "worldmap", "transition": "fade", "duration_ms": 250}},
                       {"point": "title.on", "equals": 1,
                        "when_equal": {"page": "title", "transition": "fade", "duration_ms": 250}},
                       {"point": "title.attract", "equals": 1,
                        "when_equal": {"page": "attract", "transition": "fade", "duration_ms": 600},
                        "when_not_equal": {"page": "title", "transition": "fade", "duration_ms": 600}},
                       {"point": "files.ready", "equals": 1,
                        "when_equal": {"page": "files", "transition": "fade", "duration_ms": 250},
                        "when_not_equal": {"page": "title", "transition": "fade", "duration_ms": 250}},
                       {"point": "modes.ready", "equals": 1,
                        "when_equal": {"page": "modes", "transition": "fade", "duration_ms": 250}},
                       {"point": "modes.main", "equals": 1,
                        "when_equal": {"page": "mainmenu", "transition": "fade", "duration_ms": 250}},
                       {"point": "epi.on", "equals": 1,
                        "when_equal": {"page": "epilogue", "transition": "fade", "duration_ms": 250}},
                       {"point": "arena.on", "equals": 1,
                        "when_equal": {"page": "arena", "transition": "fade", "duration_ms": 250}},
                       {"point": "curtain.on", "equals": 1,
                        "when_equal": {"page": "curtain", "transition": "fade", "duration_ms": 250}},
                       {"point": "mago.on", "equals": 1,
                        "when_equal": {"page": "magoland", "transition": "fade", "duration_ms": 250}},
                       {"point": "game.on", "equals": 1,
                        "when_equal": {"page": "game", "transition": "fade", "duration_ms": 250},
                        "when_not_equal": {"page": "magoland", "transition": "fade", "duration_ms": 250}}],
        # the first page is shown until the title menu exists: the dark one while the game boots
        "pages": [{"id": "attract", "widgets": attract_page()},
                  {"id": "title", "widgets": title_page()},
                  {"id": "files", "widgets": files_page()},
                  {"id": "modes", "widgets": modes_page()},
                  {"id": "mainmenu", "widgets": main_menu_page() + movie_cover() + credits_layer()},
                  {"id": "worldmap", "widgets": world_map_page() + spray_popup() + movie_cover() + credits_layer()},
                  {"id": "main", "widgets": probe + play + fx_layer() + spray_popup() + movie_cover() + credits_layer()},
                  {"id": "epilogue", "widgets": epilogue + finale_layer() + movie_cover() + credits_layer()},
                  {"id": "arena", "widgets": arena_page() + movie_cover() + credits_layer()},
                  # Past Adventures, the Jukebox and the Theater: the Theater's stage, its curtain closed
                  {"id": "curtain", "widgets": [{"type": "image", "color": WHITE, "rect": [0, 0, 1240, 1080],
                                                 "src": "module:kirby:gen/files/curtain"}] + movie_cover() + credits_layer()},
                  {"id": "game", "widgets": game_page()},
                  {"id": "magoland", "widgets": magoland_page(),
                   "scrolls": [{"id": "mago.list", "rect": MIS_LIST, "row_h": 1, "count_bind": "mago.list.h", "follow_bind": "mago.list.follow",
                                "reset_bind": "mago.list.reset", "show_bind": "mago.list.on", "bar": "#FF7F4F1D", "bar_w": MIS_BAR_W,
                                "bar_src": "module:kirby:gen/files/mago/mthumb", "bar_track_src": "module:kirby:gen/files/mago/mtrack"}]}],
    }


def main():
    global OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", action="store_true")
    ap.add_argument("--check", action="store_true", help="compare with the committed files instead of writing them")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as scratch:
        if args.check:
            OUT = Path(scratch)
        out = OUT / "manifest.json"
        out.write_text(json.dumps(manifest(args.dev), indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        if not args.check:
            print(out)
            return
        stale = [name for name in GENERATED if (OUT / name).read_bytes() != (PKG / name).read_bytes()]
    if stale:
        sys.exit("out of date (run tools/kirby/gen_manifest.py): " + ", ".join(stale))
    print("manifest.json and the effect plans are up to date")


if __name__ == "__main__":
    main()
