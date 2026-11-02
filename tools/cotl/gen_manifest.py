#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates packages/CultOfTheLamb/dualscreen/manifest.json.

The manifest is never edited by hand. Format: docs/PACKAGE_FORMAT.md.
Font: the game font (Lapture) through the module; cap height about 6 px per text_scale step.
"""
import json
import os
import sys

W, H = 1240, 1080
TAB_H = 100

# Text scales (text_scale): Cult tab, tab bar, window titles
S_TITLE = 6
S_NAME = 5
S_BODY = 4
S_SMALL = 3
# List tabs (Problems, Quests, Followers): one step larger
L_NAME = 6
L_BODY = 5
L_SMALL = 4

# Colours (0xAARRGGBB as text)
BG = "#FF161214"
PANEL = "#FF231C1F"
POPUP = "#FF2E2328"
# Slightly brighter panel borders
BORDER = "#FF54464D"
BORDER_POPUP = "#FF6E5C65"
BORDER_PX = 3
BAR_BG = "#FF3A3034"
TEXT = "#FFEDE3D6"
MUTED = "#FF9A8C84"
ACCENT = "#FFC8283C"
QUEST = "#FF4F8FD6"
OFFER = "#FFF0C43C"  # offered quests, quest icon in the inventory
FAITH = "#FFE8C547"
HUNGER = "#FFD9873A"
ILLNESS = "#FF7FB24A"
HAPPY = "#FF6FA8DC"
DIRT = "#FFB07A45"

TABS = [
    ("cult", "Cult"),
    ("problems", "Problems"),
    ("quests", "Quests"),
    ("followers", "Followers"),
    ("inventory", "Items"),
    ("dungeon", "Dungeon"),
]

# Problem kinds in the order of ProblemKind (native/modules/cotl_problems.h): name, colour
PROBLEM_KINDS = [
    ("Starving", "#FFC8283C"), ("Ill", "#FFC8283C"), ("Injured", "#FFC8283C"),
    ("Hungry", "#FFD9873A"), ("Cursed", "#FFC8283C"), ("Dissent", "#FFD9873A"),
    ("Exhausted", "#FFD9873A"), ("Old", "#FF6E625C"),
]

# Day phases (DayPhase) of 240 game minutes each: name, colour
PHASES = [
    ("Dawn", "#FFE7A6A1"), ("Morning", "#FFF0D24A"), ("Afternoon", "#FFE8913A"),
    ("Dusk", "#FF8E4FA8"), ("Night", "#FF2B3A78"),
]

DUNGEON_TAB = [pid for pid, _ in TABS].index("dungeon")

ROW_H = 156
MAX_ROWS = 150  # upper limit of the list templates


def tab_bar(active):
    """Tab bar at the top; the active page is highlighted."""
    widgets = [{"type": "rect", "rect": [0, 0, W, TAB_H], "bg": PANEL, "_plain": True},
               {"type": "rect", "rect": [0, TAB_H - BORDER_PX, W, BORDER_PX], "bg": BORDER}]
    tab_w = W // len(TABS)
    for i, (page_id, label) in enumerate(TABS):
        on = page_id == active
        # Outside a run the tab sets the flag "tab"; the page follows through page_binds
        # (g.view), so the package knows where to return after the run. During a run the tab switches
        # directly without changing the remembered tab.
        for need, action in (("!g.in_run", f"tab.{i}"), ("g.in_run", f"go.{page_id}")):
            widgets.append({
                "type": "button", "id": f"tab.{page_id}",
                "rect": [i * tab_w + 4, 8, tab_w - 8, TAB_H - 16],
                "text": label, "align": "center", "text_scale": S_BODY,
                "color": TEXT if on else MUTED, "bg": ACCENT if on else PANEL, "border": 0,
                "on_tap": action, "need_bind": need,
            })
        # Counter at the top right of the tab, visible on every page (also in the dungeon)
        count = {"problems": "prob.total", "quests": "quest.count"}.get(page_id)
        if count:
            widgets.append({"type": "value", "rect": [i * tab_w + tab_w - 14, 12, 0, 0],
                            "bind": count, "align": "right", "text_scale": S_SMALL,
                            "color": TEXT if on else "#FFF08A5D",
                            "hide_bind": count, "hide_eq": 0})
    return widgets


def page(page_id, widgets, scrolls=None):
    # Rects without an outline: the default (light) frame flickers at the edge on the Thor.
    # DrawRect draws a frame of max(1, frame) px while color != 0, hence color 0.
    # Visible borders are therefore two filled areas: a light border with the panel on top.
    out = []
    for wd in widgets:
        plain = wd.pop("_plain", False)
        outline = wd.pop("_outline", False)
        if wd.get("type") == "rect":
            wd.setdefault("frame", 0)
            wd.setdefault("color", "#00000000")
            border = {PANEL: BORDER, POPUP: BORDER_POPUP}.get(wd.get("bg"))
            if border and not plain:
                x, y, rw, rh = wd["rect"]
                inner = {k: v for k, v in wd.items()
                         if k not in ("on_tap", "payload", "select_group", "input_block")}
                inner["rect"] = [x + BORDER_PX, y + BORDER_PX, rw - 2 * BORDER_PX,
                                 rh - 2 * BORDER_PX]
                wd["bg"] = border
                out += [wd, inner]
                continue
        if outline:
            # Border around a bar: an area behind it with the same conditions and repeats
            x, y, rw, rh = wd["rect"]
            back = {k: v for k, v in wd.items()
                    if k in ("need_bind", "hide_bind", "hide_eq", "repeat", "repeat_bind",
                             "repeat_dy", "scroll")}
            out.append({"type": "rect", "rect": [x - 2, y - 2, rw + 4, rh + 4], "bg": BORDER,
                        "frame": 0, "color": "#00000000", **back})
        out.append(wd)
    widgets = out
    for wd in widgets:
        # Centred labels/values take x as the centre: written here as a box [x, y, w, h]
        # and converted to the middle of the box
        if wd.get("type") in ("label", "value") and wd.get("align") == "center" and \
                not wd.get("_centered"):
            x, y, rw, rh = wd["rect"]
            wd["rect"] = [x + rw // 2, y, rw, rh]
            wd["_centered"] = True
    p = {"id": page_id, "widgets": widgets}
    if scrolls:
        p["scrolls"] = scrolls
    return p


def background(page_id):
    return [{"type": "rect", "rect": [0, 0, W, H], "bg": BG}] + tab_bar(page_id)


def list_row(top, count_bind, scroll_id, row_h=ROW_H):
    """Template helper: the widget repeats per entry in the scroll region."""
    def row(widget):
        widget = dict(widget)
        x, y, rw, rh = widget["rect"]
        widget["rect"] = [x, top + y, rw, rh]
        widget.update({"repeat": MAX_ROWS, "repeat_bind": count_bind, "repeat_dy": row_h,
                       "scroll": scroll_id})
        return widget
    return row


def scroll(scroll_id, top, count_bind, row_h=ROW_H, bottom=H):
    return {"id": scroll_id, "rect": [0, top, W, bottom - top], "count_bind": count_bind,
            "row_h": row_h, "bar": "#80EDE3D6", "bar_track": "#22000000", "bar_w": 8}


def empty_label(text, need):
    return {"type": "label", "rect": [0, 540, W, 40], "text": text, "align": "center",
            "text_scale": L_BODY, "color": MUTED, "need_bind": need}


def popup(flag, close_action, top=TAB_H + 40, bottom=H - 40, need=None):
    """Floating window; visible while @flag:<flag> != -1 (need: while this value is set).
    Catches all taps."""
    vis = {"need_bind": need} if need else {"hide_bind": f"@flag:{flag}", "hide_eq": -1}
    return vis, [
        {"type": "rect", "rect": [0, TAB_H, W, H - TAB_H], "bg": "#B0000000", "input_block": True,
         "on_tap": close_action, **vis},
        {"type": "rect", "rect": [40, top, W - 80, bottom - top], "bg": POPUP,
         "on_tap": close_action, **vis},
        {"type": "label", "rect": [40, bottom - 56, W - 80, 30], "text": "Tap to close",
         "align": "center", "text_scale": S_SMALL, "color": MUTED, **vis},
    ]


# ---- Cult -------------------------------------------------------------------------------------

def labeled_bar(y, label, bind, max_bind, color, pct_bind):
    return [
        {"type": "rect", "rect": [30, y, W - 60, 132], "bg": PANEL},
        {"type": "label", "rect": [54, y + 22, 700, 50], "text": label, "text_scale": S_NAME,
         "color": TEXT},
        {"type": "value", "rect": [W - 54, y + 22, 0, 0], "bind": pct_bind, "suffix": "%",
         "align": "right", "text_scale": S_NAME, "color": TEXT},
        {"type": "bar", "rect": [54, y + 66, W - 108, 50], "bind": bind, "max_bind": max_bind,
         "color": color, "bg": BAR_BG, "frame": 0, "_outline": True},
    ]


def stat_tile(x, y, w_, label, bind, color):
    return [
        {"type": "rect", "rect": [x, y, w_, 110], "bg": PANEL},
        {"type": "label", "rect": [x + 24, y + 20, w_ - 48, 34], "text": label,
         "text_scale": S_SMALL, "color": MUTED},
        {"type": "value", "rect": [x + 24, y + 58, 0, 0], "bind": bind, "text_scale": S_NAME,
         "color": color},
    ]


def storage_tile(x, y, w_, label, key, color):
    """Silo fill level: name, "n / max" and bar; only visible if there is a silo."""
    gate = {"need_bind": f"cult.has_{key}"}
    return [
        {"type": "rect", "rect": [x, y, w_, 110], "bg": PANEL, **gate},
        {"type": "label", "rect": [x + 24, y + 25, 300, 34], "text": label,
         "text_scale": S_SMALL, "color": MUTED, **gate},
        {"type": "value", "rect": [x + w_ - 24, y + 20, 0, 0], "bind": f"cult.{key}",
         "max_bind": f"cult.{key}_cap", "max_sep": " / ", "align": "right",
         "text_scale": S_BODY, "color": TEXT, **gate},
        {"type": "bar", "rect": [x + 24, y + 64, w_ - 48, 28], "bind": f"cult.{key}",
         "max_bind": f"cult.{key}_cap", "color": color, "bg": BAR_BG, "frame": 0,
         "_outline": True, **gate},
    ]


def cult_page():
    w = background("cult")
    # Header row: cult name and cult level
    w += [
        {"type": "rect", "rect": [30, TAB_H + 8, W - 60, 74], "bg": PANEL},
        {"type": "label", "rect": [54, TAB_H + 35, 800, 50], "bind_text": "cult.name",
         "text_scale": S_NAME, "color": TEXT},
        {"type": "value", "rect": [W - 54, TAB_H + 35, 0, 0], "bind": "cult.level",
         "text": "Cult Level ", "align": "right", "text_scale": S_NAME, "color": FAITH},
    ]
    w += labeled_bar(190, "Faith", "cult.faith", "cult.faith_max", FAITH, "cult.faith_pct")
    w += labeled_bar(330, "Hunger", "cult.hunger", "cult.hunger_max", HUNGER, "cult.hunger_pct")
    # As in the game (IllnessBar.get_IllnessNormalized = 1 - Count/Max): full = healthy
    w += labeled_bar(470, "Health", "cult.health", "cult.illness_max", ILLNESS, "cult.health_pct")
    # Day: shared panel; border around the day bar (the phases are semi-transparent, so
    # the panel colour lies once more between the border and the phases)
    w += [
        {"type": "rect", "rect": [30, 610, W - 60, 168], "bg": PANEL},
        {"type": "rect", "rect": [52, 674, W - 104, 48], "bg": BORDER},
        {"type": "rect", "rect": [54, 676, W - 108, 44], "bg": PANEL, "_plain": True},
        {"type": "value", "rect": [54, 632, 0, 0], "bind": "g.day", "text": "Day ",
         "text_scale": S_NAME, "color": TEXT},
        {"type": "label", "rect": [W - 54, 632, 0, 40], "text_bind": "g.phase", "align": "right",
         "text_map": {str(k): name for k, (name, _) in enumerate(PHASES)},
         "text_scale": S_NAME, "color": TEXT},
    ]
    # Day bar: five phases of 240 game minutes each (TimeManager.GAME_MINUTES_PER_PHASE),
    # past part in full colour, rest muted; white mark at the current time
    bar_x, bar_y, bar_w, bar_h = 54, 676, W - 108, 44
    seg_w = bar_w / 5
    for k, (name, color) in enumerate(PHASES):
        x = round(bar_x + k * seg_w)
        sw = round(bar_x + (k + 1) * seg_w) - x
        dim = "#55" + color[3:]
        w.append({"type": "bar", "rect": [x, bar_y, sw, bar_h], "bind": f"g.seg{k}", "max": 240,
                  "color": color, "bg": dim, "frame": 0})
        w.append({"type": "label", "rect": [x, bar_y + bar_h + 10, sw, 30], "text": name,
                  "align": "center", "text_scale": S_SMALL, "color": MUTED})
    w.append({"type": "rect", "rect": [bar_x - 3, bar_y - 8, 6, bar_h + 16], "bg": TEXT,
              "x_bind": "g.time", "x_scale": bar_w / 1200})
    # Follower counts (ill/old as counted in the Problems tab), dirt (poop + vomit) and
    # corpses in the base
    tiles = [("Followers", "fol.count", TEXT), ("Ill", "prob.n1", "#FFC8283C"),
             ("Old age", "prob.n7", MUTED), ("Dirt", "cult.dirt", DIRT),
             ("Bodies", "cult.corpses", "#FFC8283C")]
    tw = (W - 60 - (len(tiles) - 1) * 20) // len(tiles)
    for k, (label, bind, color) in enumerate(tiles):
        w += stat_tile(30 + k * (tw + 20), 790, tw, label, bind, color)
    # Silo fill levels
    sw2 = (W - 60 - 20) // 2
    w += storage_tile(30, 916, sw2, "Seed silos", "seed", ILLNESS)
    w += storage_tile(30 + sw2 + 20, 916, sw2, "Fertiliser silos", "fert", HUNGER)
    return page("cult", w)


# ---- Probleme ---------------------------------------------------------------------------------

def problems_page():
    w = background("problems")
    # Counter per problem kind (two rows of 4), only visible when count > 0; tapping filters
    bw, bh, gap = 296, 64, 8
    for k, (label, color) in enumerate(PROBLEM_KINDS):
        # Position from prob.cx/cy: only existing kinds take up slots, without gaps
        x, y = 20, TAB_H + 14
        hide = {"hide_bind": f"prob.n{k}", "hide_eq": 0, "x_bind": f"prob.cx{k}",
                "y_bind": f"prob.cy{k}"}
        w.append({"type": "rect", "rect": [x, y, bw, bh], "bg": color,
                  "on_tap": f"prob.f{k}", **hide})
        w.append({"type": "label", "rect": [x + 24, y + 22, bw - 90, 34], "text": label,
                  "text_scale": L_SMALL, "color": TEXT, **hide})
        w.append({"type": "value", "rect": [x + bw - 24, y + 22, 0, 0], "bind": f"prob.n{k}",
                  "align": "right", "text_scale": L_SMALL, "color": TEXT, **hide})
        # Active filter: white frame; tapping again clears the filter
        only_k = {"hide_bind": "@flag:prob.filter", "keep_min": k, "keep_max": k,
                  "x_bind": f"prob.cx{k}", "y_bind": f"prob.cy{k}"}
        w.append({"type": "rect", "rect": [x - 4, y - 4, bw + 8, bh + 8], "bg": "#00000000",
                  "color": TEXT, "frame": 4, "on_tap": "prob.fclear", **only_k})
    top = TAB_H + 14 + 2 * (bh + gap) + 6
    w.append(empty_label("Everyone is fine", "!prob.total"))
    row = list_row(top, "prob.count", "prob.list")
    w += [
        # Tapping shows all of the follower's problems
        row({"type": "rect", "rect": [20, 6, W - 40, ROW_H - 12], "bg": PANEL,
             "on_tap": "prob.select", "payload": "{i}", "select_group": "prob"}),
        row({"type": "label", "rect": [44, 30, 600, 48], "bind_text": "prob.name{i}",
             "text_scale": L_NAME, "color": TEXT}),
        row({"type": "value", "rect": [44, 98, 0, 0], "bind": "prob.level{i}", "text": "Lv ",
             "text_scale": L_BODY, "color": MUTED}),
        row({"type": "label", "rect": [200, 98, 600, 36], "bind_text": "prob.reason{i}",
             "text_scale": L_BODY, "color": TEXT}),
    ]
    # Coloured badge of the shown kind: one template per kind, only visible for the matching kind
    for k, (label, color) in enumerate(PROBLEM_KINDS):
        only_k = {"hide_bind": "prob.kind{i}", "keep_min": k, "keep_max": k}
        w.append(row({"type": "rect", "rect": [W - 340, 42, 300, 70], "bg": color, **only_k}))
        w.append(row({"type": "label", "rect": [W - 340, 67, 300, 36], "text": label,
                      "align": "center", "text_scale": L_BODY, "color": TEXT, **only_k}))
    # Window: all problems of the tapped follower, most urgent first
    vis, win = popup("prob.open", "prob.close", top=TAB_H + 40, bottom=H - 30)
    w += win
    w += [
        {"type": "label", "rect": [80, TAB_H + 70, 800, 50], "bind_text": "prob.sel_name",
         "text_scale": L_NAME, "color": TEXT, **vis},
        {"type": "value", "rect": [80, TAB_H + 140, 0, 0], "bind": "prob.sel_level", "text": "Lv ",
         "text_scale": L_BODY, "color": MUTED, **vis},
        {"type": "label", "rect": [0, 560, W, 40], "text": "No problems right now",
         "align": "center", "text_scale": L_BODY, "color": MUTED, "need_bind": "prob.sel_none",
         **vis},
    ]
    stop, srh = TAB_H + 210, 100
    srow = list_row(stop, "prob.sel_count", "prob.sel_list", srh)
    w.append(srow({"type": "rect", "rect": [70, 6, W - 140, srh - 12], "bg": PANEL}))
    for k, (_, color) in enumerate(PROBLEM_KINDS):
        only_k = {"hide_bind": "prob.sel_kind{i}", "keep_min": k, "keep_max": k}
        w.append(srow({"type": "rect", "rect": [70, 6, 14, srh - 12], "bg": color, **only_k}))
    w.append(srow({"type": "label", "rect": [108, 40, W - 220, 40],
                   "bind_text": "prob.sel_reason{i}", "text_scale": L_BODY, "color": TEXT}))
    scrolls = [scroll("prob.list", top, "prob.count"),
               {**scroll("prob.sel_list", stop, "prob.sel_count", srh, bottom=H - 90),
                "show_bind": "prob.sel_open"}]
    return page("problems", w, scrolls)


# ---- Quests -----------------------------------------------------------------------------------

def quests_page():
    w = background("quests")
    top = TAB_H + 10
    rh = 190
    w.append(empty_label("No open quests", "!quest.count"))
    row = list_row(top, "quest.count", "quest.list", rh)
    w += [
        row({"type": "rect", "rect": [20, 6, W - 40, rh - 12], "bg": PANEL,
             "on_tap": "quest.select", "payload": "{i}", "select_group": "quest"}),
        # Offered quests: yellow stripe on the left
        row({"type": "rect", "rect": [20, 6, 12, rh - 12], "bg": OFFER,
             "hide_bind": "quest.offered{i}", "hide_eq": 0}),
        row({"type": "label", "rect": [50, 30, 1100, 36], "bind_text": "quest.title{i}",
             "text_scale": L_SMALL, "color": MUTED}),
        row({"type": "label", "rect": [50, 76, 1140, 48], "bind_text": "quest.text{i}",
             "text_scale": L_NAME, "color": TEXT, "wrap_width": 1140, "max_lines": 1}),
        row({"type": "label", "rect": [50, 136, 480, 36], "bind_text": "quest.giver{i}",
             "text_scale": L_BODY, "color": QUEST}),
        row({"type": "label", "rect": [560, 136, 340, 36], "bind_text": "quest.more{i}",
             "text_scale": L_BODY, "color": MUTED}),
        row({"type": "label", "rect": [W - 50, 136, 0, 36], "bind_text": "quest.due{i}",
             "align": "right", "text_scale": L_BODY, "color": HUNGER}),
    ]
    # Window with all steps of the tapped quest
    vis, win = popup("quest.open", "quest.close", top=TAB_H + 40, bottom=H - 30)
    w += win
    w += [
        {"type": "label", "rect": [80, TAB_H + 70, W - 160, 50], "bind_text": "quest.sel_title",
         "text_scale": L_NAME, "color": TEXT, **vis},
        {"type": "label", "rect": [80, TAB_H + 140, 600, 40], "bind_text": "quest.sel_giver",
         "text_scale": L_BODY, "color": QUEST, **vis},
        {"type": "label", "rect": [W - 80, TAB_H + 140, 0, 40], "bind_text": "quest.sel_due",
         "align": "right", "text_scale": L_BODY, "color": HUNGER, **vis},
    ]
    stop, srh = TAB_H + 210, 120
    srow = list_row(stop, "quest.step_count", "quest.steps", srh)
    open_step = {"hide_bind": "quest.step_done{i}", "hide_eq": 1}
    done_step = {"hide_bind": "quest.step_done{i}", "hide_eq": 0}
    w += [
        srow({"type": "rect", "rect": [70, 6, W - 140, srh - 12], "bg": PANEL, **vis}),
        srow({"type": "label", "rect": [100, 30, W - 200, 80], "bind_text": "quest.step{i}",
              "wrap_width": W - 200, "max_lines": 2, "line_gap": 18, "text_scale": L_BODY,
              "color": TEXT, **vis, **open_step}),
        srow({"type": "label", "rect": [100, 30, W - 320, 80], "bind_text": "quest.step{i}",
              "wrap_width": W - 320, "max_lines": 2, "line_gap": 18, "text_scale": L_BODY,
              "color": MUTED, **vis, **done_step}),
        srow({"type": "label", "rect": [W - 100, 30, 0, 36], "text": "done", "align": "right",
              "text_scale": L_BODY, "color": ILLNESS, **vis, **done_step}),
    ]
    scrolls = [scroll("quest.list", top, "quest.count", rh),
               {**scroll("quest.steps", stop, "quest.step_count", srh, bottom=H - 90),
                "show_bind": "quest.steps_open"}]
    return page("quests", w, scrolls)


# ---- Follower ---------------------------------------------------------------------------------

def followers_page():
    w = background("followers")
    head = TAB_H + 16
    top = TAB_H + 64
    bars = [(560, "Fed", HUNGER, "sat"), (720, "Loyalty", FAITH, "loyal"),
            (880, "Ill", ILLNESS, "ill"), (1040, "Happy", HAPPY, "happy")]
    for x, label, _, _ in bars:
        w.append({"type": "label", "rect": [x, head, 150, 34], "text": label,
                  "text_scale": L_SMALL, "color": MUTED})
    w.append(empty_label("No followers yet", "!fol.count"))
    row = list_row(top, "fol.count", "fol.list")
    w += [
        row({"type": "rect", "rect": [20, 6, W - 40, ROW_H - 12], "bg": PANEL,
             "on_tap": "fol.select", "payload": "{i}", "select_group": "fol"}),
        row({"type": "label", "rect": [44, 30, 500, 48], "bind_text": "fol.name{i}",
             "text_scale": L_NAME, "color": TEXT}),
        row({"type": "value", "rect": [44, 98, 0, 0], "bind": "fol.level{i}", "text": "Lv ",
             "text_scale": L_BODY, "color": MUTED}),
        row({"type": "label", "rect": [200, 98, 340, 36], "bind_text": "fol.role{i}",
             "text_scale": L_BODY, "color": MUTED}),
        row({"type": "label", "rect": [560, 100, 600, 34], "text_bind": "fol.group{i}",
             "text_map": {"1": "On mission", "2": "Imprisoned"}, "text_scale": L_SMALL,
             "color": ACCENT}),
    ]
    for x, _, color, key in bars:
        w.append(row({"type": "bar", "rect": [x, 50, 140, 34], "bind": f"fol.{key}{{i}}",
                      "max": 100, "color": color, "bg": BAR_BG, "frame": 0,
                      "_outline": True}))

    # Detail box of the tapped follower at the bottom; tapping closes it
    dvis = {"hide_bind": "@flag:fol.open", "hide_eq": -1}
    dtop = H - 340
    w += [
        {"type": "rect", "rect": [20, dtop, W - 40, 320], "bg": POPUP, "on_tap": "fol.close",
         **dvis},
        {"type": "label", "rect": [50, dtop + 24, 800, 50], "bind_text": "fol.sel_name",
         "text_scale": S_TITLE, "color": TEXT, **dvis},
        {"type": "label", "rect": [50, dtop + 100, W - 100, 200], "bind_text": "fol.sel_detail",
         "wrap_width": W - 100, "max_lines": 4, "line_gap": 18, "text_scale": L_BODY,
         "color": TEXT, **dvis},
        # Button for the Thoughts window
        {"type": "rect", "rect": [W - 330, dtop + 20, 290, 70], "bg": QUEST,
         "on_tap": "fol.thoughts", **dvis},
        {"type": "label", "rect": [W - 330, dtop + 45, 290, 36], "text": "Thoughts",
         "align": "center", "text_scale": L_BODY, "color": TEXT, **dvis},
    ]
    # Thoughts window: thoughts of the tapped follower with faith modifier
    tvis, win = popup("fol.thoughts", "fol.thoughts_close", top=TAB_H + 40, bottom=H - 30)
    w += win
    w += [
        {"type": "label", "rect": [80, TAB_H + 70, 700, 50], "bind_text": "fol.sel_name",
         "text_scale": S_TITLE, "color": TEXT, **tvis},
        {"type": "label", "rect": [80, TAB_H + 140, 400, 34], "text": "Thoughts",
         "text_scale": L_SMALL, "color": MUTED, **tvis},
        {"type": "label", "rect": [0, 560, W, 40], "text": "No thoughts right now",
         "align": "center", "text_scale": L_BODY, "color": MUTED, "need_bind": "!fol.th_count",
         **tvis},
    ]
    ttop, trh = TAB_H + 190, 76
    trow = list_row(ttop, "fol.th_count", "fol.th_list", trh)
    w += [
        trow({"type": "label", "rect": [80, 18, 900, 40], "bind_text": "fol.th_text{i}",
              "text_scale": L_BODY, "color": TEXT, **tvis}),
        trow({"type": "label", "rect": [W - 80, 18, 0, 40], "bind_text": "fol.th_mod{i}",
              "align": "right", "text_scale": L_BODY, "color": FAITH, **tvis}),
    ]
    scrolls = [scroll("fol.list", top, "fol.count"),
               {**scroll("fol.th_list", ttop, "fol.th_count", trh, bottom=H - 100),
                "show_bind": "fol.th_open"}]
    return page("followers", w, scrolls)


# ---- Inventar ---------------------------------------------------------------------------------

# Sections as in the game menu (InventoryMenu, native/modules/cotl_inventory_menu_1_5_27.inc)
INV_GROUPS = ["Currency", "Food", "Items"]
# Smaller tiles at the same font size: 6 columns
INV_COLS, INV_TILE_W, INV_TILE_H, INV_GAP = 6, 188, 222, 10
INV_HEAD = 76  # height of the section heading


def inv_section_h(g):
    """Height of section g in the scroll content (0 if empty), as an expression."""
    n = f"inv.n{g}"
    return f"min({n}, 1) * ({INV_HEAD} + ceil({n} / {INV_COLS}) * {INV_TILE_H + INV_GAP})"


def inventory_page():
    w = background("inventory")
    top = TAB_H + 14
    tile_w, tile_h, gap = INV_TILE_W, INV_TILE_H, INV_GAP
    w.append(empty_label("Inventory is empty", "!inv.count"))

    def placed(widget, g, repeat):
        widget = dict(widget)
        x, y, rw, rh = widget["rect"]
        widget["rect"] = [20 + x, top + y, rw, rh]
        # The heading is a repeat too: the host only moves repeat templates with the
        # Scroll-Region
        widget.update({"scroll": "inv.grid", "y_bind": f"inv.off{g}", "repeat": 1})
        if repeat:
            widget.update({"repeat": MAX_ROWS * 2, "repeat_bind": f"inv.n{g}",
                           "repeat_cols": INV_COLS, "repeat_dx": tile_w + gap,
                           "repeat_row_dy": tile_h + gap})
        return widget

    for g, title in enumerate(INV_GROUPS):
        key = f"inv.g{g}."
        def tile(widget, g=g):
            widget = dict(widget)
            widget["rect"] = [widget["rect"][0], INV_HEAD + widget["rect"][1],
                              *widget["rect"][2:]]
            return placed(widget, g, True)
        quest = {"hide_bind": key + "quest{i}", "hide_eq": 0}
        w += [
            placed({"type": "label", "rect": [4, 18, 800, 44], "text": title,
                    "text_scale": L_NAME, "color": FAITH,
                    "hide_bind": f"inv.n{g}", "hide_eq": 0}, g, False),
            tile({"type": "rect", "rect": [0, 0, tile_w, tile_h], "bg": PANEL,
                  "on_tap": "inv.select", "payload": f"{{i+{g * 1000}}}",
                  "select_group": "inv"}),
            # Icon from the game files (module:cotl:item/<ITEM_TYPE>, native/modules/cotl_icons.cpp)
            tile({"type": "image", "rect": [(tile_w - 90) // 2, 48, 90, 90],
                  "src_bind": key + "icon{i}"}),
            tile({"type": "value", "rect": [16, 19, 0, 0], "bind": key + "qty{i}", "text": "x",
                  "text_scale": L_BODY, "color": TEXT}),
            tile({"type": "label", "rect": [6, 146, tile_w - 12, 68],
                  "bind_text": key + "name{i}", "align": "center", "wrap_width": tile_w - 12,
                  "max_lines": 2, "line_gap": 14, "text_scale": L_SMALL, "color": TEXT}),
            # Quest icon at the top right: an open collect/deliver quest needs the item
            tile({"type": "rect", "rect": [tile_w - 50, 14, 36, 36], "bg": OFFER, **quest}),
            tile({"type": "label", "rect": [tile_w - 50, 21, 36, 26], "text": "!",
                  "align": "center", "text_scale": L_BODY, "color": BG, **quest}),
        ]

    # Window with the description of the tapped item
    vis, win = popup("inv.open", "inv.close", top=TAB_H + 120, bottom=H - 120)
    ptop = TAB_H + 120
    w += win
    w += [
        {"type": "rect", "rect": [80, ptop + 40, 200, 200], "bg": PANEL, **vis},
        {"type": "image", "rect": [95, ptop + 55, 170, 170], "src_bind": "inv.sel_icon", **vis},
        {"type": "label", "rect": [320, ptop + 60, W - 400, 50], "bind_text": "inv.sel_name",
         "text_scale": S_TITLE, "color": TEXT, **vis},
        {"type": "value", "rect": [320, ptop + 150, 0, 0], "bind": "inv.sel_qty",
         "text": "Owned: ", "text_scale": L_BODY, "color": MUTED, **vis},
        {"type": "label", "rect": [80, ptop + 290, W - 160, 300], "bind_text": "inv.sel_desc", "color_markup": True,
         "wrap_width": W - 160, "max_lines": 6, "line_gap": 18, "text_scale": L_BODY,
         "color": TEXT, **vis},
    ]
    scrolls = [{"id": "inv.grid", "rect": [0, top, W, H - top],
                "content_h_bind": "inv.content_h", "bar": "#80EDE3D6",
                "bar_track": "#22000000", "bar_w": 8}]
    return page("inventory", w, scrolls)


# ---- Dungeon ----------------------------------------------------------------------------------

# Heart images: edge length and count per row (580 px wide column on the left)
HEART_PX, HEART_COLS = 50, 11


# Map area; must match kMapAreaW/kMapAreaH in native/modules/cotl_publish.h
MAP_X, MAP_Y, MAP_W, MAP_H = 40, 392, 1160, 380
# Room heights like kMapSizes in native/modules/cotl_dungeon_map.h; width 8/5, gap half the height
MAP_SIZES = [80, 72, 64]
ROOM_CUR = "#FFEDE3D6"   # current room: light, with a yellow frame
ROOM_SEEN = "#FF8C7F76"  # besucht
ROOM_DIM = "#FF3C3337"   # only discovered (in-game map: half opacity)
ROOM_UNKNOWN = "#FF4A4448"  # undiscovered neighbour with a question mark
# Room tiles of the in-game map: "MapPlayer" (74 x 72, grey with a rough black edge, stretched as
# 9-slice) and "Map_You" (205 x 123, light, current room; aspect ratio like the rooms)
TILE = {"type": "image", "src": "module:cotl:mapicon/MapPlayer", "fill": "slice",
        "slice": [14, 14, 14, 14]}
TILE_CUR = {"type": "image", "src": "module:cotl:mapicon/Map_You"}
# Player figure on the current room: lamb head (sprite in resources.assets, atlas Coop_Indicator).
# The game's map icon sits in the MiniMap prefab, which cannot be read.
MAP_PLAYER = "Coop_Indicator_Lamb"


def map_widgets():
    """Rooms and door stubs per size; only the templates of the chosen size are visible."""
    w = [{"type": "rect", "rect": [MAP_X - 10, MAP_Y - 10, MAP_W + 20, MAP_H + 20], "bg": PANEL,
          "need_bind": "g.in_run"},
         {"type": "label", "rect": [0, MAP_Y + MAP_H // 2 - 20, W, 40], "text": "No map yet",
          "align": "center", "text_scale": L_BODY, "color": MUTED, "need_bind": "map.empty"}]
    for k, u in enumerate(MAP_SIZES):
        rw, rh, gap = u * 8 // 5, u, u // 2
        slen, t = (gap + 1) // 2, max(4, u // 6)
        gate = f"map.is{k}"

        def each(widget, count, xb, yb, gate=gate):
            widget = dict(widget)
            x, y, ww, wh = widget["rect"]
            widget["rect"] = [MAP_X + x, MAP_Y + y, ww, wh]
            widget.update({"repeat": 200, "repeat_bind": count, "x_bind": xb, "y_bind": yb,
                           "need_bind": gate, "scroll": "map.clip"})
            return widget

        def room(widget):
            return each(widget, f"map.count{k}", "map.x{i}", "map.y{i}")

        def stubs(d, size):
            # Stubs of visited rooms light, of only-discovered rooms faded
            return [each({"type": "rect", "rect": [0, 0, *size], "bg": color,
                          "hide_bind": f"map.{d}dim{{i}}", "hide_eq": hide},
                         f"map.{d}count{k}", f"map.{d}x{{i}}", f"map.{d}y{{i}}")
                    for color, hide in ((ROOM_SEEN, 1), (ROOM_DIM, 0))]

        icon = rh * 7 // 10   # Raumsymbol
        me = rh * 17 // 20    # player figure
        qi = max(2, rh // 8)  # question mark rooms: smaller by qi per side
        qs = max(2, u // 13)  # text scale of the question mark
        ext = gap - slen + qi  # door up to a question mark room (MapExtLen)
        w += stubs("h", (slen, t)) + stubs("v", (t, slen)) + stubs("he", (ext, t)) +             stubs("ve", (t, ext)) + [
            # Room tiles of the in-game map (MapIcons atlas): visited grey, only discovered half
            # transparent, current room light
            room({**TILE, "rect": [0, 0, rw, rh], "hide_bind": "map.vis{i}", "hide_eq": 0}),
            room({**TILE, "rect": [0, 0, rw, rh], "tint": "#80FFFFFF",
                  "hide_bind": "map.dim{i}", "hide_eq": 0}),
            room({**TILE_CUR, "rect": [0, 0, rw, rh], "hide_bind": "map.cur{i}", "hide_eq": 0}),
            # Room icon like the in-game map (GenerateRoom.MapIcon), only discovered: half transparent
            room({"type": "image", "rect": [(rw - icon) // 2, (rh - icon) // 2, icon, icon],
                  "src_bind": "map.icon{i}", "hide_bind": "map.has_icon{i}", "hide_eq": 0,
                  "tint_bind": "map.dim{i}", "tint_colors": ["#FFFFFFFF", "#80FFFFFF"]}),
            # Undiscovered neighbour of a visited room: grey, slightly smaller room with "?"
            room({**TILE, "rect": [qi, qi, rw - 2 * qi, rh - 2 * qi], "tint": "#FF8A8A8A",
                  "hide_bind": "map.q{i}", "hide_eq": 0}),
            room({"type": "label", "rect": [0, (rh - 5 * qs) // 2, rw, 5 * qs], "text": "?",
                  "align": "center", "text_scale": qs, "color": TEXT,
                  "hide_bind": "map.q{i}", "hide_eq": 0}),
            # Player figure on the current room
            room({"type": "image", "rect": [(rw - me) // 2, (rh - me) // 2, me, me],
                  "src": "module:cotl:mapicon/" + MAP_PLAYER,
                  "hide_bind": "map.cur{i}", "hide_eq": 0}),
        ]
    return w


def dungeon_page():
    w = background("dungeon")
    w.append({"type": "label", "rect": [0, 540, W, 40], "text": "Not in a dungeon run",
              "align": "center", "text_scale": L_BODY, "color": MUTED, "need_bind": "!g.in_run"})
    run = {"need_bind": "g.in_run"}
    # Hearts on the left as on the HUD (images from heart.atlas): red with empty containers, spirit, blue,
    # black, fire and ice hearts; rows of HEART_COLS
    y = TAB_H + 14
    w.append({"type": "rect", "rect": [30, y - 6, 618, 270], "bg": PANEL, **run})
    w.append({"type": "label", "rect": [54, y + 14, 500, 36], "text": "Hearts", "text_scale": L_SMALL,
              "color": MUTED, **run})
    w.append({"type": "image", "rect": [54, y + 48, HEART_PX, HEART_PX], "src_bind": "run.heart{i}",
              "repeat": 44, "repeat_bind": "run.heart_count", "repeat_cols": HEART_COLS,
              "repeat_dx": HEART_PX + 2, "repeat_row_dy": HEART_PX + 2, **run})
    # Equipment on the right, one row per item
    x = 660
    w.append({"type": "label", "rect": [x + 4, y + 14, 500, 36], "text": "Equipment", "text_scale": L_SMALL,
              "color": MUTED, **run})
    gear = [("Weapon", "run.weapon", "run.weapon_lvl", "run.has_weapon", "run.weapon_icon"),
            ("Curse", "run.curse", "run.curse_lvl", "run.has_curse", "run.curse_icon"),
            ("Relic", "run.relic", None, "run.has_relic", "run.relic_icon")]
    for k, (label, name, lvl, need, icon) in enumerate(gear):
        gy = y + 44 + k * 76
        w += [
            # Tapping shows stats and text like the in-game card
            {"type": "rect", "rect": [x, gy, W - x - 40, 68], "bg": PANEL, "on_tap": "run.select",
             "payload": str(100 + k), **run},
            {"type": "label", "rect": [x + 24, gy + 28, 110, 30], "text": label,
             "text_scale": S_SMALL, "color": MUTED, **run},
            # Icon from the game files (WeaponsIcons or relic atlas)
            {"type": "image", "rect": [x + 124, gy + 4, 60, 60], "src_bind": icon,
             "need_bind": need},
            {"type": "label", "rect": [x + 196, gy + 26, 270, 36], "bind_text": name,
             "text_scale": L_SMALL, "color": TEXT, "need_bind": need},
            {"type": "label", "rect": [x + 196, gy + 26, 270, 36], "text": "None",
             "text_scale": L_SMALL, "color": MUTED, "need_bind": need.replace("has_", "none_")},
        ]
        if lvl:
            w.append({"type": "value", "rect": [W - 64, gy + 26, 0, 0], "bind": lvl,
                      "text": "Lv ", "align": "right", "text_scale": L_SMALL, "color": FAITH,
                      "need_bind": need})
    w += map_widgets()
    # Tarot cards as a grid below the map; tapping shows the effect
    ty = MAP_Y + MAP_H + 18
    w.append({"type": "label", "rect": [40, ty, 500, 36], "text": "Tarot cards",
              "text_scale": L_SMALL, "color": MUTED, **run})
    w.append({"type": "label", "rect": [40, ty + 50, 600, 40], "text": "No tarot cards yet",
              "text_scale": L_BODY, "color": MUTED, "need_bind": "run.no_tarot"})
    top = ty + 44
    cols, cw, ch, gap = 3, 380, 100, 10

    def card(widget):
        widget = dict(widget)
        x0, y0, rw, rh = widget["rect"]
        widget["rect"] = [40 + x0, top + y0, rw, rh]
        widget.update({"repeat": 60, "repeat_bind": "run.tarot_count", "repeat_cols": cols,
                       "repeat_dx": cw + gap, "repeat_row_dy": ch + gap, "scroll": "run.tarot"})
        return widget

    w += [
        card({"type": "rect", "rect": [0, 0, cw, ch], "bg": PANEL, "on_tap": "run.select",
              "payload": "{i}", "select_group": "tarot"}),
        # Card front (WeaponCard.atlas, 312 x 497)
        card({"type": "image", "rect": [14, 8, 53, 84], "src_bind": "run.tarot_icon{i}"}),
        card({"type": "label", "rect": [82, 22, cw - 96, ch - 26], "bind_text": "run.tarot_name{i}",
              "wrap_width": cw - 96, "max_lines": 2, "line_gap": 14,
              "text_scale": L_SMALL, "color": TEXT}),
    ]
    vis, win = popup("run.open", "run.close", top=TAB_H + 200, bottom=H - 200, need="run.sel_open")
    w += win
    w += [
        {"type": "label", "rect": [80, TAB_H + 240, W - 160, 50], "bind_text": "run.sel_name",
         "text_scale": S_TITLE, "color": TEXT, **vis},
        {"type": "label", "rect": [80, TAB_H + 296, W - 160, 36], "bind_text": "run.sel_stats",
         "text_scale": L_SMALL, "color": FAITH, **vis},
        {"type": "label", "rect": [80, TAB_H + 350, W - 160, 200], "bind_text": "run.sel_text", "color_markup": True,
         "wrap_width": W - 160, "max_lines": 5, "line_gap": 18, "text_scale": L_BODY,
         "color": TEXT, **vis},
        {"type": "label", "rect": [80, TAB_H + 590, W - 160, 90], "bind_text": "run.sel_lore",
         "wrap_width": W - 160, "max_lines": 2, "line_gap": 14, "text_scale": L_SMALL,
         "color": MUTED, **vis},
    ]
    # Map section: clips rooms at the edge; content as large as the area, so no
    # scrolling (large floors follow the current room, native/modules/cotl_dungeon_map.h)
    scrolls = [{"id": "map.clip", "rect": [MAP_X, MAP_Y, MAP_W, MAP_H], "content_h": MAP_H,
                "fling": False, "show_bind": "g.in_run"},
               {"id": "run.tarot", "rect": [0, top, W, H - top], "count_bind": "run.tarot_count",
                "row_h": ch + gap, "cols": cols, "bar": "#80EDE3D6", "bar_track": "#22000000",
                "bar_w": 8, "show_bind": "g.in_run"}]
    return page("dungeon", w, scrolls)


# ---- Placeholders and waiting -------------------------------------------------------------------

def placeholder_page(page_id, text):
    w = background(page_id)
    w.append({"type": "label", "rect": [0, 540, W, 40], "text": text, "align": "center",
              "text_scale": L_BODY, "color": MUTED})
    return page(page_id, w)


def waiting_page():
    return page("waiting", [
        {"type": "rect", "rect": [0, 0, W, H], "bg": BG},
        {"type": "label", "rect": [0, 500, W, 60], "text": "The Red Crown awaits", "align": "center",
         "text_scale": S_TITLE, "color": MUTED},
    ])


# ---- Values, actions, whole document -----------------------------------------------------------

def derived():
    d = [
        # At the title screen a DataManager object already exists: ready only with a loaded save
        {"name": "g.ready", "expr": "dm != 0 && g.loaded != 0"},
        # Maximums from the game code: CultFaithManager.MAX_FAITH = 85, HungerBar.get_MAX_HUNGER = 100
        {"name": "cult.faith_max", "terms": [], "add": 85},
        {"name": "cult.hunger_max", "terms": [], "add": 100},
        {"name": "cult.faith_pct", "expr": "clamp(cult.faith * 100 / 85, 0, 100)", "floor": True},
        {"name": "cult.hunger_pct", "expr": "clamp(cult.hunger, 0, 100)", "floor": True},
        {"name": "cult.health", "expr": "max(cult.illness_max - cult.illness, 0)"},
        {"name": "cult.health_pct",
         "expr": "clamp(100 - cult.illness * 100 / max(cult.illness_max, 1), 0, 100)", "floor": True},
        # Page shown: -1 waiting, in a run the Dungeon tab, otherwise the last chosen tab
        {"name": "g.view",
         "expr": f"g.ready ? (g.in_run ? {DUNGEON_TAB} : @flag:tab) : -1"},
        {"name": "view.wait", "expr": "g.view == -1"},
        # Dungeon map: chosen grid size, empty map in a run
        *[{"name": f"map.is{k}", "expr": f"map.sz == {k} && map.count > 0"}
          for k in range(len(MAP_SIZES))],
        {"name": "map.empty", "expr": "g.in_run && map.count == 0"},
        # In a run: placeholder "None" per equipment item, "No tarot cards yet"
        *[{"name": f"run.none_{k}", "expr": f"g.in_run && run.has_{k} == 0"}
          for k in ("weapon", "curse", "relic")],
        {"name": "run.no_tarot", "expr": "g.in_run && run.tarot_count == 0"},
        *[{"name": f"view.{k}", "expr": f"g.view == {k}"} for k in range(len(TABS))],
        # Scroll regions of the windows only while a window is open
        {"name": "fol.th_open", "expr": "@flag:fol.thoughts != -1"},
        {"name": "quest.steps_open", "expr": "@flag:quest.open != -1"},
        {"name": "prob.sel_open", "expr": "@flag:prob.open != -1"},
        {"name": "prob.sel_none", "expr": "@flag:prob.open != -1 && prob.sel_count == 0"},
        # Inventory: start of the sections in the scroll content and its total height
        {"name": "inv.off0", "terms": [], "add": 0},
        {"name": "inv.off1", "expr": inv_section_h(0)},
        {"name": "inv.off2", "expr": f"{inv_section_h(0)} + {inv_section_h(1)}"},
        {"name": "inv.content_h",
         "expr": f"{inv_section_h(0)} + {inv_section_h(1)} + {inv_section_h(2)} + 20"},
    ]
    # Problem filter: slot = number of existing kinds before it; 4 per row (296 + 8 wide, 64 + 8 high)
    for k in range(len(PROBLEM_KINDS)):
        pos = " + ".join([f"min(prob.n{j}, 1)" for j in range(k)]) or "0"
        d.append({"name": f"prob.pos{k}", "expr": pos})
        d.append({"name": f"prob.cx{k}",
                  "expr": f"(prob.pos{k} - 4 * floor(prob.pos{k} / 4)) * 304"})
        d.append({"name": f"prob.cy{k}", "expr": f"floor(prob.pos{k} / 4) * 72"})
    # Past minutes per day phase (0-240) for the day bar
    for k in range(len(PHASES)):
        d.append({"name": f"g.seg{k}", "expr": f"clamp(g.time - {k * 240}, 0, 240)"})
    return d


def actions():
    acts = {f"go.{pid}": {"kind": "page", "page": pid, "transition": "fade", "duration_ms": 120}
            for pid, _ in TABS}
    acts["fol.select"] = {"kind": "flag", "flag": "fol.open", "value": "$payload"}
    acts["fol.close"] = {"kind": "flag", "flag": "fol.open", "value": -1}
    acts["fol.thoughts"] = {"kind": "flag", "flag": "fol.thoughts", "value": 1}
    acts["fol.thoughts_close"] = {"kind": "flag", "flag": "fol.thoughts", "value": -1}
    for k in range(len(TABS)):
        acts[f"tab.{k}"] = {"kind": "flag", "flag": "tab", "value": k}
    # The module holds the tarot window: it closes with the run (native/modules/01002E7016C46000.cpp)
    acts["run.select"] = {"kind": "module", "action": "run.select", "argument": "$payload"}
    acts["run.close"] = {"kind": "module", "action": "run.close", "argument": 0}
    acts["quest.select"] = {"kind": "flag", "flag": "quest.open", "value": "$payload"}
    acts["quest.close"] = {"kind": "flag", "flag": "quest.open", "value": -1}
    acts["prob.select"] = {"kind": "flag", "flag": "prob.open", "value": "$payload"}
    acts["prob.close"] = {"kind": "flag", "flag": "prob.open", "value": -1}
    acts["inv.select"] = {"kind": "flag", "flag": "inv.open", "value": "$payload"}
    acts["inv.close"] = {"kind": "flag", "flag": "inv.open", "value": -1}
    for k in range(len(PROBLEM_KINDS)):
        acts[f"prob.f{k}"] = {"kind": "flag", "flag": "prob.filter", "value": k}
    acts["prob.fclear"] = {"kind": "flag", "flag": "prob.filter", "value": -1}
    return acts


def manifest():
    return {
        "format": 1,
        "name": "Cult of the Lamb dual screen",
        "title_id": "01002E7016C46000",
        "min_runtime": 22,  # content_h_bind of the inventory scroll region
        "canvas_w": W,
        "canvas_h": H,
        "background": BG,
        # Game font (umlauts, apostrophe, lowercase): metrics and atlas from the module
        "font": "file:cotl_font.txt",
        "font_atlas": "module:cotl:font",
        "flags": {"fol.open": -1, "fol.thoughts": -1, "quest.open": -1, "prob.filter": -1,
                  "inv.open": -1, "prob.open": -1, "tab": 0},
        "derived": derived(),
        "actions": actions(),
        "page_binds": [{"point": "view.wait", "equals": 1, "when_equal": {"page": "waiting"}}] + [
            {"point": f"view.{k}", "equals": 1,
             "when_equal": {"page": pid, "transition": "fade", "duration_ms": 120}}
            for k, (pid, _) in enumerate(TABS)],
        "pages": [
            waiting_page(),
            cult_page(),
            problems_page(),
            quests_page(),
            followers_page(),
            inventory_page(),
            dungeon_page(),
        ],
    }


def write_manifest(path):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest(), f, indent=1, ensure_ascii=False)
        f.write("\n")


if __name__ == "__main__":
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "packages", "CultOfTheLamb", "dualscreen", "manifest.json")
    write_manifest(out)
    print(out)
