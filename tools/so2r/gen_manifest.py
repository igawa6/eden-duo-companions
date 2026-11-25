#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate packages/StarOceanTheSecondStoryR/dualscreen/manifest.json.

    python3 tools/so2r/gen_manifest.py [--diag]

0.3.0: a tab bar (Party | Map, + Diag in the diag build) and the MAP page: the game's own area
minimap or world map (pictures read from the player's game files through pictures.idx), the
player where the game's minimap puts it, pan / pinch zoom and Recenter. The map areas (one per
map picture, sized in picture pixels) are generated from pictures.idx.

0.2.0: the PARTY page in the game's own look -- the game's font (FOT-Chiaro, read by the module
from the player's romfs), each member's own dot portrait, the LEADER label, the Fol coin and the
game's HP / MP gauge colours, all pictures read from the player's game files (module:pic/...).
Names come from the game's text tables. The diag build adds a DIAG page. Every value comes from
the native module (native/modules/so2r); this file only lays them out. All published names start
with "so2r." (checked against every package in the repository).
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "packages" / "StarOceanTheSecondStoryR" / "dualscreen" / "manifest.json"

VERSION = "1.0.0"
TITLE = "010065301A2E0000"
BUILD = "D214BA64D902AEDA"
MIN_RUNTIME = 22          # derived expr, value group, image fill/slice, bar image (PACKAGE_FORMAT.md section 6)
ROWS = 8
DIAG_LINES = 38
# the tabs: page id, the game's word for it (a module point, our fallback until the game's is found)
# 0.9.10: three main tabs at the bottom (Status / Equipment / Arts / Skills now live in Party's details)
TABS = [("party", "so2r.t.party"), ("map", "so2r.t.map"), ("missions", "so2r.t.missions"), ("battle", "so2r.t.battle")]   # 0.9.70 + Battle
# a member's details: the subtabs (the game's words) -- Status, Equipment, Special Arts/Spells, Skills
SUBTABS = ["so2r.t.status", "so2r.t.equipment", "so2r.t.arts", "so2r.t.skills"]
FOOT_Y = 976              # the tab bar along the bottom; pages draw above it

BG = "#FF0A1424"          # the camp menu's deep blue
HEADER = "#FF13284A"
TEXT = "#FFF4F1E8"
DIM = "#FF9DB3D1"
GOLD = "#FFF2CF63"
HPC = "#FFF0913A"         # the camp menu's HP label orange
MPC = "#FF5CA8F0"         # the camp menu's MP label blue
TRACK = "#C0050C18"
CLEAR = "#00000000"
WHITE = "#FFFFFFFF"
SEL = "#FF3FA9EE"          # 0.9.28 the game's menu highlight (its light blue bar)
BOLD = {}                  # 0.9.30: the game's menu words are plain white, not bold
MENU = "#FFFFFFFF"         # the game's menu white
LINE = "#E0F4F1E8"
# 0.9.96 the bonus gauge's numbers (r70/r71): white text in the font material "Aldrich-Regular SDF_battle_effect_guts"
# -- face (1,1,1), outline (1, 0.651, 0.2588) and glow (1, 0.4902, 0, 0.5882)
# 0.9.99 the face is tinted by its picture "battle_guts_color" (r72: pale cream at the top to yellow at the foot); the
# companion's text has one fill colour, so the picture's average
BONUS_NUM = "#FFFFFD93"
BONUS_LINE = "#FFFFA642"
BONUS_GLOW = "#96FF7D00"
BONUS_MAX = BONUS_NUM


def rule(x, y, w, **kw):
    """The game's white line under a heading."""
    return {"type": "rect", "rect": [x, y, w, 2], "bg": LINE, "color": LINE, "frame": 0, "noframe": True, **kw}


def tab(x, y, w, h, word, scale, on_tap, sel_bind=None, active=False, gate=None, text=None):
    """A tab the game's way (0.9.30): its pill frame (list_item_unselected, like the member cards), the game's
    striped light blue when chosen (list_item_selected); plain white words. The game's frames have 10 px of
    clear margin above and below, so they are drawn 10 px taller each way."""
    g = {"need_bind": gate} if gate else {}
    out = [image(x, y - 10, w, h + 20, pic("list_item_unselected"), fill="slice",
                 slice=game_slice("list_item_unselected"), **g)]
    if active:
        out.append(image(x, y - 10, w, h + 20, pic("list_item_selected"), fill="slice",
                         slice=game_slice("list_item_selected"), **g))
    elif sel_bind:
        out.append(image(x, y - 10, w, h + 20, pic("list_item_selected"), fill="slice",
                         slice=game_slice("list_item_selected"), need_bind=sel_bind))
    words = {"bind_text": word} if word else {}      # the Diag tab has its own text, no game word
    out.append(label(x + w // 2, y + (h - scale * 6) // 2, text, scale, MENU, align="center",
                     wrap_width=w - 20, max_lines=1, fit_text=True, text_min_scale=max(3, scale - 2), **words, **g))
    out.append({"type": "rect", "rect": [x, y, w, h], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                "on_tap": on_tap, **g})
    return out


def switch_button(action, gate, x=40, w=300):
    """The game's Y: Switch Pages -- bottom left, its words bigger."""
    y = DET_Y + 646             # 0.9.31 higher (the lists above end by 636)
    return tab(x, y, w, 56, "so2r.t.swpage", 5, action, gate=gate)


# 0.9.63 the Status subtab's two buttons, Switch Pages and Leader, centred under the friendship list's rule
# (40 .. 620): 270 wide, 20 apart
STATUS_BTN_W, STATUS_BTN_X = 270, 330 - 270 - 10


def stats_column(gate, elem_gate, py=None):
    """0.9.30 the game's stats column (its Equipment / Special Arts screens): HP, MP, the nine stats with their
    icons, then the elements -- white words, the value at the right, the game's ruled line under every row."""
    st = {"need_bind": gate}
    px, py = 840, (DET_Y + 16) if py is None else py
    W = 360
    out = []

    def row(cy, h, gate_kw):
        return image(px - 8, cy + h - 6, W + 8, 8, pic("camp_ruledline"), fill="slice",
                     slice=game_slice("camp_ruledline"), **gate_kw)
    for j, (word, bind) in enumerate((("so2r.t.hp", "so2r.s.hpmax"), ("so2r.t.mp", "so2r.s.mpmax"))):
        cy = py + j * 46
        out += [label(px, cy + 6, None, 5, MENU, bind_text=word, **st),
                {"type": "value", "rect": [px + W - 8, cy + 2, 0, 0], "bind": bind, "align": "right", "text_scale": 6,
                 "color": MENU, **st},
                row(cy, 46, st)]
    for si in range(9):
        cy = py + 92 + si * 46
        out += [image(px, cy + 4, 36, 36, pic(f"icon_status_{STAT_ICONS[si]}"), **st),
                label(px + 46, cy + 6, None, 5, MENU, bind_text=f"so2r.t.st{si}", **st),
                {"type": "value", "rect": [px + W - 8, cy + 2, 0, 0], "bind": f"so2r.s.st{si}", "align": "right",
                 "text_scale": 6, "color": MENU, **st},
                row(cy, 46, st)]
    for k in range(ELEMENTS):
        cy = py + 92 + 9 * 46 + 6 + k * 37
        on = {"need_bind": f"{elem_gate}{k}"}
        out += [image(px, cy + 2, 32, 32, src_bind=f"so2r.s.elic{k}", **on),
                label(px + W - 8, cy + 4, None, 5, MENU, bind_text=f"so2r.s.elw{k}", align="right", wrap_width=280,
                      max_lines=1, fit_text=True, text_min_scale=3, **on),
                row(cy, 37, on)]
    return out


IDX = ROOT / "packages" / "StarOceanTheSecondStoryR" / "dualscreen" / "pictures.idx"
_BORDERS = None


def game_slice(name):
    """The game's own 9-slice insets for a picture (Sprite.m_Border in pictures.idx), as [l, t, r, b]."""
    global _BORDERS
    if _BORDERS is None:
        _BORDERS = {}
        for line in IDX.read_text(encoding="utf-8").splitlines():
            f = line.split()
            if len(f) == 12 and f[0] == "S":
                l, b, r, t = (int(x) for x in f[8:12])
                _BORDERS[f[1]] = [l, t, r, b]
    if name not in _BORDERS:
        raise SystemExit(f"pictures.idx has no 9-slice borders for {name}: rebuild it with build_picture_index.py")
    return _BORDERS[name]


def pic(name):
    return f"module:pic/{name}"


def rect(x, y, w, h, bg, **kw):
    return {"type": "rect", "rect": [x, y, w, h], "bg": bg, "color": CLEAR, **kw}


def image(x, y, w, h, src=None, **kw):
    out = {"type": "image", "rect": [x, y, w, h], "color": WHITE}
    if src:
        out["src"] = src
    out.update(kw)
    return out


def label(x, y, text=None, scale=5, color=TEXT, **kw):
    w = {"type": "label", "rect": [x, y, 0, 0], "color": color, "text_scale": scale}
    if text is not None:
        w["text"] = text
    w.update(kw)
    return w


def legend(x, y):
    """The game's Guide Map legend: its icons and its words (SYS_0514/0515/0516/0518)."""
    out = [rect(x + 12, y - 4, 1100, 48, "#B0050C18", pill=True, need_bind="so2r.map.ok")]
    items = [("icon_map_scenario_event", "so2r.t.dest"), ("icon_map_sub_event", "so2r.t.sub"),
             ("icon_map_pa_event", "so2r.t.pa"), ("icon_map_limit", "so2r.t.limit")]
    for i, (icon, word) in enumerate(items):
        ix = x + 28 + i * 275
        out.append(image(ix, y, 40, 40, pic(icon), need_bind="so2r.map.ok"))
        out.append(label(ix + 48, y + 4, None, 5, TEXT, bind_text=word, need_bind="so2r.map.ok"))
    return out


def tabs(diag):
    return TABS      # 0.9.70 Diag moved to the Guide Map sidebar (diag builds), its tab's place to Battle


def header(diag, active):
    """The main tabs, large, along the bottom (the game's words, its menu colours), and the Fol at the right.
    0.9.28: Party always opens the roster (a member's details are closed)."""
    # 0.9.49 no solid strip of its own; 0.9.50 the dotted panel moved out of the bar (dots_bottom, behind the page), so
    # the tabs float on the page's own background
    out = []
    for i, (pid, word) in enumerate(tabs(diag)):
        out += tab(16 + i * 238, FOOT_Y + 10, 230, 84, word, 7, "so2r.roster" if pid == "party" else f"so2r.to_{pid}",
                   active=pid == active, text=None if word else "Diag")
    out += [
        # 0.9.154 placed by the module just left of the number (so2r.fol.cx)
        image(0, FOOT_Y + 22, 60, 60, pic("camp_icon_money"), x_bind="so2r.fol.cx", need_bind="so2r.ok"),
        {"type": "value", "rect": [1224, FOOT_Y + 26, 0, 0], "bind": "so2r.fol", "group": True,
         "align": "right", "text_scale": 8, "color": TEXT, "need_bind": "so2r.ok"},
    ]
    return out


BATTLE_ILL_H = 552           # 0.9.93 the Battle page card's picture rows (the module's kBattleIllH)
CARD_W, CARD_DX = 272, 284   # 0.9.16: the game's 220 / 230 at 952 / 772 (the module's kRosterW / kRosterDx); the roster's tall cards (the game's Party Formation look); drag sideways for more


def dots_top():
    """0.9.48 the game's dotted blue panel (field_mode_background) across the top of a page, behind everything: at
    the page's width and its own proportions (square dots), its bright corner at the top left, fading down."""
    return [image(0, 0, 1240, round(1240 * 368 / 976), pic("field_mode_background"))]


def dots_bottom():
    """0.9.50 the same panel at the bottom of a page, behind everything, like dots_top turned upside down: its dots at
    the screen's bottom edge, fading up into the page's background (no edge of its own), its bright corner at the
    right by the Fol (0.9.48)."""
    h = round(1240 * 368 / 976)
    return [image(0, 1080 - h, 1240, h, pic("field_mode_background"), flip_x=True, flip_y=True)]


SLASH_X = 16 + CARD_W - 20 - 66     # 0.9.57 the roster's slash column: "/9999" at scale 4 fits after it


def party_rows():
    """The roster like the game's Party Formation: a tall card per member -- the character's
    illustration (read from the game's charaillust bundles), LEADER / GUEST, the level half circle
    (the game's camp_level_gauze pictures, filled to the next level), HP / MP with the game's gauges;
    tap one for their details."""
    # 0.9.16 the game's own geometry (uiscene UICampCharacterStatusPresenter, research r38/r39): the
    # panel is 772 game pixels tall, a card 220 wide every 230; the picture is the module's "@ill:"
    # (the card's 220 x 690 mask from its top, the character placed exactly as the game places them),
    # drawn at the same scale as the card. The tag, the half circle, Lv, HP and MP over its lower part.
    y0, h = 16, FOOT_Y - 8 - 16
    k = h / 772
    rep = {"repeat": ROWS, "repeat_dx": CARD_DX, "need_bind": "so2r.ro{i}", "scroll": "roster"}
    cx = 16 + CARD_W // 2
    fy = lambda f: y0 + int(h * f)
    return [
        image(16, y0, CARD_W, h, pic("camp_member_background"), fill="slice", slice=game_slice("camp_member_background"), **rep),
        # the game's Party Formation picture (charaillust_NNNN through the card's mask)
        image(16, y0, CARD_W, round(690 * k), src_bind="so2r.ill{i}", **rep),
        image(26, fy(0.615), 150, 40, pic("camp_icon_label_leader"), **{**rep, "need_bind": "so2r.rlead{i}"}),
        label(101, fy(0.615) + 10, None, 4, TEXT, bind_text="so2r.t.leader", align="center", wrap_width=140,
              max_lines=1, fit_text=True, text_min_scale=3, **{**rep, "need_bind": "so2r.rlead{i}"}),
        image(26, fy(0.615), 150, 40, pic("camp_icon_label_guest"), **{**rep, "need_bind": "so2r.rguest{i}"}),
        label(101, fy(0.615) + 10, None, 4, TEXT, bind_text="so2r.t.guest", align="center", wrap_width=140,
              max_lines=1, fit_text=True, text_min_scale=3, **{**rep, "need_bind": "so2r.rguest{i}"}),
        # the level half circle (the point fits inside the picture's padding), Lv and the level inside it
        image(cx - 121, fy(0.645) - 16, 242, 149, src_bind="so2r.lvarc{i}", **rep),
        label(cx, fy(0.715), None, 5, TEXT, bind_text="so2r.t.lv", align="center", **rep),
        {"type": "value", "rect": [cx, fy(0.75), 0, 0], "bind": "so2r.lv{i}", "align": "center", "text_scale": 8,
         "color": TEXT, **rep},
        label(36, fy(0.835), None, 5, HPC, bind_text="so2r.t.hp", **rep),
        # 0.9.57 like the game's Party Formation: the value large in white, "/max" smaller in the gauge's colour
        # right after it (the slash at a fixed column, the maximum padded to four places by the module)
        {"type": "value", "rect": [SLASH_X, fy(0.835) - 8, 0, 0], "bind": "so2r.hp{i}", "align": "right",
         "text_scale": 6, "color": TEXT, **rep},
        label(SLASH_X + 2, fy(0.835) + 4, None, 4, HPC, bind_text="so2r.hpmx{i}", **rep),
        {"type": "bar", "rect": [36, fy(0.87), CARD_W - 40, 16], "bind": "so2r.hp{i}", "max_bind": "so2r.hpmax{i}",
         "image": pic("gauge_color_hp_01"), "color": HPC, "bg": TRACK, "frame": 0, **rep},
        label(36, fy(0.905), None, 5, MPC, bind_text="so2r.t.mp", **rep),
        {"type": "value", "rect": [SLASH_X, fy(0.905) - 8, 0, 0], "bind": "so2r.mp{i}", "align": "right",
         "text_scale": 6, "color": TEXT, **rep},
        label(SLASH_X + 2, fy(0.905) + 4, None, 4, MPC, bind_text="so2r.mpmx{i}", **rep),
        {"type": "bar", "rect": [36, fy(0.94), CARD_W - 40, 16], "bind": "so2r.mp{i}", "max_bind": "so2r.mpmax{i}",
         "image": pic("gauge_color_mp"), "color": MPC, "bg": TRACK, "frame": 0, **rep},
        # tap a member: their details
        {"type": "rect", "rect": [16, y0, CARD_W, h], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
         "on_tap": "so2r.open{i}", **rep},
    ]


# sideways scrolling over the cards actually shown (the module publishes their widths: roster members x 284 - 12,
# picker members x 304 - 8)
ROSTER_SCROLL = {"id": "roster", "rect": [16, 16, 1208, FOOT_Y - 24], "axes": "x", "content_w_bind": "so2r.rostw",
                 "show_bind": "so2r.v.roster"}
PICK_SCROLL = {"id": "pick", "rect": [16, 84, 1208, 120], "axes": "x", "content_w_bind": "so2r.cardsw",
               "show_bind": "so2r.v.det"}


MAP_RECT = (16, 212, 1208, 752)   # the module's kMapRect* (keep in step)
MARKS = 64                         # the module's kMaxMarks
CARDS = 4                          # the party strip on the map page: the battle members (four at most)


def party_cards():
    """The map page's compact party: the battle members (four at most, the party's order) -- portrait, name,
    Lv, HP / MP with the game's gauges, the LEADER / GUEST tag; tap one for their Party details.
    0.9.25: the cards share the whole width by how many there are (so2r.bn, 1..4); everything inside the card's
    edge; a bigger tag."""
    return member_cards()


def member_cards(x0=16, total=1208, counts=None, nm=None):
    """The map page's party cards; 0.9.71 also the Battle page's (x0 / total / counts, and nm: a point's name for
    this page's -- (what, n, i) -> name)."""
    out = []
    ch, y, gap = 184, 16, 8
    if nm is None:
        def nm(what, n, i):
            return {"on": f"so2r.v.b{n}on{i}", "lead": f"so2r.v.b{n}lead{i}", "guest": f"so2r.v.b{n}guest{i}",
                    "tap": f"so2r.bopen{i}"}.get(what, f"so2r.b{what}{i}")
    for n in (counts or range(1, CARDS + 1)):
        cw = (total - gap * (n - 1)) // n
        for i in range(n):
            x = x0 + i * (cw + gap)
            on = {"need_bind": nm("on", n, i)}
            bx = x + 152                     # the gauges' left edge (right of the portrait and the tag)
            bw = cw - 152 - 18
            out += [
                image(x, y, cw, ch, pic("list_item_unselected"), fill="slice", slice=game_slice("list_item_unselected"),
                      **on),
                label(x + 18, y + 14, None, 6, TEXT, bind_text=nm("name", n, i), wrap_width=cw - 130, max_lines=1,
                      fit_text=True, text_min_scale=4, **on),
                label(x + cw - 96, y + 20, None, 4, DIM, bind_text="so2r.t.lv", **on),
                {"type": "value", "rect": [x + cw - 18, y + 14, 0, 0], "bind": nm("lv", n, i), "align": "right",
                 "text_scale": 6, "color": TEXT, **on},
                image(x + 18, y + 52, 76, 76, src_bind=nm("dot", n, i), **on),
                # the game's LEADER / GUEST tag, bigger, under the portrait (inside the card)
                image(x + 12, y + ch - 50, 132, 36, pic("camp_icon_label_leader"), need_bind=nm("lead", n, i)),
                label(x + 82, y + ch - 44, None, 4, TEXT, bind_text="so2r.t.leader", align="center", wrap_width=110,
                      max_lines=1, fit_text=True, text_min_scale=3, need_bind=nm("lead", n, i)),
                image(x + 12, y + ch - 50, 132, 36, pic("camp_icon_label_guest"), need_bind=nm("guest", n, i)),
                label(x + 82, y + ch - 44, None, 4, TEXT, bind_text="so2r.t.guest", align="center", wrap_width=110,
                      max_lines=1, fit_text=True, text_min_scale=3, need_bind=nm("guest", n, i)),
                label(bx, y + 62, None, 4, HPC, bind_text="so2r.t.hp", **on),
                {"type": "value", "rect": [x + cw - 18, y + 54, 0, 0], "bind": nm("hp", n, i), "align": "right",
                 "text_scale": 6, "color": TEXT, **on},
                {"type": "bar", "rect": [bx, y + 94, bw, 10], "bind": nm("hp", n, i), "max_bind": nm("hpmax", n, i),
                 "image": pic("gauge_color_hp_01"), "color": HPC, "bg": TRACK, "frame": 0, **on},
                label(bx, y + 118, None, 4, MPC, bind_text="so2r.t.mp", **on),
                {"type": "value", "rect": [x + cw - 18, y + 110, 0, 0], "bind": nm("mp", n, i), "align": "right",
                 "text_scale": 6, "color": TEXT, **on},
                {"type": "bar", "rect": [bx, y + 150, bw, 10], "bind": nm("mp", n, i), "max_bind": nm("mpmax", n, i),
                 "image": pic("gauge_color_mp"), "color": MPC, "bg": TRACK, "frame": 0, **on},
                # tap: that member's Party details (the module flips so2r.goparty; a page_bind opens Party)
                {"type": "rect", "rect": [x, y, cw, ch], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                 "on_tap": nm("tap", n, i), **on},
            ]
    return out


# 0.9.18 the standard Map layout: the info sidebar at the left, the map beside it in the same kind of
# panel (no separate frame); the picture inside the panel's lines (the module's kMapNarrow*, keep in step)
# 0.9.54 shorter by the "Next objective" card under both (OBJ), between them and the tabs
OBJ_H = 72                 # 0.9.56 (was 96): the sidebar and the map 24 px taller
OBJ_Y = FOOT_Y - 8 - OBJ_H
SIDE = (16, 212, 330, OBJ_Y - 8 - 212)
MAP_NARROW = (358, 212, 866, OBJ_Y - 8 - 212)
# 0.9.22 two cards again: the sidebar on the game's plain dark panel, the map on its own panel with the Guide
# Map grid behind it; the picture fills the map card inside its lines (the module's kMapNarrow*, keep in step)
MAP_INNER = (366, 222, 850, OBJ_Y - 8 - 212 - 20)
# the game's quick menu (the field's d-pad / X): its word, the button pressed
QUICK = [("so2r.t.map", "DUp"), ("so2r.t.qitems", "DLeft"), ("so2r.t.qic", "DDown"), ("so2r.t.qheal", "DRight"),
         ("so2r.t.qcamp", "X"), ("so2r.t.qpa", "Y")]          # 0.9.25 + Switch PA (Y)
LEGEND = [("icon_map_scenario_event", "so2r.t.dest"), ("icon_map_sub_event", "so2r.t.sub"),
          ("icon_map_pa_event", "so2r.t.pa"), ("icon_map_limit", "so2r.t.limit")]


def sidebar(diag=False):
    """The quick menu (each button presses the game's own) from the top of the sidebar. 0.9.54: the place's
    name moved onto the map (map_place), the "Next objective" into its own card under both (objective_card)."""
    x, y, w, h = SIDE
    on = {"need_bind": "so2r.v.side"}
    # 0.9.57 no panel: the buttons float on the page like the tabs; 0.9.58 the panel's own top and bottom lines
    # kept (its top / bottom border strips, sliced sideways only), level with the map card's
    l, t, r, b = game_slice("details_background")
    out = [image(x, y, w, t, pic("details_background"), fill="slice", slice=[l, 0, r, 0], src_rect=[0, 0, 1, t / 512],
                 **on),
           image(x, y + h - b, w, b, pic("details_background"), fill="slice", slice=[l, 0, r, 0],
                 src_rect=[0, 1 - b / 512, 1, 1], **on)]
    # 0.9.142 + the HideHUD switch (lit while the top screen's HUD is hidden); 0.9.70 diag builds: Diag under it
    rows = list(QUICK) + [(None, "hud")] + ([(None, "diag")] if diag else [])
    # 0.9.135 taller: the buttons share the sidebar's height (24 px clear of its top and bottom lines)
    pitch = (h - 48) // len(rows)
    bh = pitch - 8
    top = y + (h - ((len(rows) - 1) * pitch + bh)) // 2      # 0.9.56 centred in the sidebar
    for j, (word, btn) in enumerate(rows):
        by = top + j * pitch
        # 0.9.67 the game's own glow band (field_information_get_background_glow), stretched
        # 0.9.68 mirrored: its straight end at the left, on the panel's line, the fade toward the map
        # 0.9.122 back to the main tabs' buttons (the player's choice over the glow bands)
        if btn == "hud":
            out += tab(x + 12, by + 6, w - 24, bh - 12, None, 6, "so2r.hud_hide", sel_bind="so2r.hh.on",
                       gate="so2r.v.side", text="Hide HUD")
            continue
        out += tab(x + 12, by + 6, w - 24, bh - 12, word, 6, f"so2r.qtap{j}" if word else "so2r.to_diag",
                   gate="so2r.v.side", text=None if word else "Diag")
    return out


def map_place():
    """0.9.54 where the player is (World Map, or the town / dungeon's name) and its floor, on the map at its top
    left, outlined in the panel's dark so it reads over the picture; the map's width for long names."""
    ix, iy, iw, ih = MAP_INNER
    dark = {"outline": "#FF07101E", "outline_px": 3}
    return [label(ix + 16, iy + 12, None, 6, TEXT, bind_text="so2r.map.head", wrap_width=iw - 32, max_lines=1,
                  fit_text=True, text_min_scale=4, need_bind="so2r.v.sidemap", **dark),
            label(ix + 16, iy + 62, None, 5, GOLD, bind_text="so2r.map.floor", need_bind="so2r.v.sidefloor", **dark)]


def objective_card():
    """0.9.24 the Guide Map's "Next objective" (the game's heading and the objective's own text). 0.9.56 (the user's
    layout): no panel, floating on the page like the tabs; the heading centred under the sidebar, the line between
    the sidebar and the map, the objective centred under the map, both at the same size (one line; a long
    objective shrinks to fit)."""
    sx, _, sw, _ = SIDE
    mx, _, mw, _ = MAP_NARROW
    y, h = OBJ_Y, OBJ_H
    on = {"need_bind": "so2r.map.hasobj"}
    ty = y + (h - 40) // 2
    gap = (sx + sw + mx) // 2
    return [label(sx + sw // 2, ty, None, 6, GOLD, bind_text="so2r.t.nextobj", align="center", wrap_width=sw - 24,
                  max_lines=1, fit_text=True, text_min_scale=4, **on),
            {"type": "rect", "rect": [gap - 1, y + (h - 64) // 2, 2, 64], "bg": DIM, "color": DIM, "frame": 0,
             "noframe": True, **on},
            label(mx + mw // 2, ty, None, 6, TEXT, bind_text="so2r.map.obj", align="center", wrap_width=mw - 32,
                  max_lines=1, fit_text=True, text_min_scale=4, **on)]


def map_page(diag):
    """0.9.18: the battle members along the top; the info sidebar at the left and the map beside it."""
    x, y, w, h = MAP_NARROW
    ix, iy, iw, ih = MAP_INNER
    gate = "so2r.v.mapn"
    out = [rect(0, 0, 1240, 1080, BG)] + dots_top() + dots_bottom() + [
           # the map's card: the game's dark panel (its lines), the Guide Map's grid behind the picture
           image(x, y, w, h, pic("details_background"), fill="slice", slice=game_slice("details_background"),
                 need_bind="so2r.ok"),
           # 0.9.21 the Guide Map's background behind everything: the module's "@grid:72" (the game's grid
           # tile at the game's cell size, its dark lines on the sea blue), tiled
           image(x + 6, y + 8, w - 12, h - 16, "module:pic/@grid:72", fill="tile", need_bind="so2r.ok"),
           # the picture: the module's canvas (the area turned like the game draws it, padded to this shape),
           # shown through the module's view inside the panel's lines (the image is cut at its own rect)
           {"type": "image", "id": "mapn", "rect": [ix, iy, iw, ih], "src_bind": "so2r.map.img", "color": WHITE,
            "pan_zoom": True, "min_zoom": 1, "max_zoom": 16, "view_zoom_bind": "so2r.map.zoom",
            "view_cx_bind": "so2r.map.zx", "view_cy_bind": "so2r.map.zy", "view_idle_ms": 500,
            "view_reset_bind": "so2r.map.vreset", "need_bind": gate},
           # the game's own minimap icons, placed (canvas pixels) and tinted by the module
           image(-22, -22, 44, 44, src_bind="so2r.mk.src{i}", x_bind="so2r.mk.x{i}", y_bind="so2r.mk.y{i}",
                 need_bind="so2r.mk.on{i}", scale_bind="so2r.mk.s{i}", repeat=MARKS),
           # the player: the game's own minimap icon, placed and turned by the module
           image(-32, -32, 64, 64, pic("icon_map_player"), x_bind="so2r.map.ax", y_bind="so2r.map.ay",
                 rotate_bind="so2r.map.adeg", need_bind="so2r.v.arrow"),
           # tap the map: the world: Minimap / Whole map; towns: whole area / the game's minimap size
           {"type": "rect", "rect": [ix, iy, iw, ih], "bg": CLEAR, "color": CLEAR, "frame": 0,
            "on_tap": "so2r.map_toggle", "noframe": True, "need_bind": gate}]
    out += sidebar(diag) + map_place() + objective_card() + header(diag, "map") + party_cards() + [
        label(x + w // 2, y + h // 2 - 70, "No map here", 9, DIM, align="center", need_bind="so2r.v.nomap"),
        label(x + w // 2, y + h // 2 + 20, None, 5, DIM, align="center", bind_text="so2r.map.why", need_bind="so2r.v.nomap",
              wrap_width=820, max_lines=2),
    ]
    return {"id": "map", "title": "Map", "widgets": out + notices(diag)}


def notices(diag):
    out = [
        label(620, 470, "Waiting for the game", 9, DIM, align="center", need_bind="so2r.v.wait"),
        label(620, 620, None, 5, GOLD, align="center", bind_text="module_error_message",
              wrap_width=1100, max_lines=3, need_bind="module_error"),
    ]
    if diag:
        out.append(label(620, 560, None, 5, DIM, align="center", bind_text="so2r.why",
                         need_bind="so2r.v.wait"))
    return out


def details_top():
    """A member's details: Back to the roster, the four subtabs (the game's words) and the party to
    choose from (the subtab stays when the member changes, the member when the subtab changes)."""
    d = {"need_bind": "so2r.v.det"}
    out = []
    # 0.9.28 the four subtabs alone, the same size and words, the game's colours (the Party tab goes back)
    sw = (1208 - 8 * (len(SUBTABS) - 1)) // len(SUBTABS)        # 0.9.60 five across
    for t, word in enumerate(SUBTABS):
        out += tab(16 + t * (sw + 8), 8, sw, 64, word, 5, f"so2r.sub{t}", sel_bind=f"so2r.sub{t}", gate="so2r.v.det")
    # the picker: a card per member like the Guide Map's (picture and name); drag sideways for more
    pr = {"repeat": ROWS, "repeat_dx": CARD_DX, "scroll": "pick", "need_bind": "so2r.po{i}"}
    out += [
        image(16, 84, CARD_W, 120, pic("list_item_unselected"), fill="slice", slice=game_slice("list_item_unselected"), **pr),
        image(16, 84, CARD_W, 120, pic("list_item_selected"), fill="slice", slice=game_slice("list_item_selected"),
              **{**pr, "need_bind": "so2r.psel{i}"}),   # 0.9.30 the game's striped light blue
        image(24, 92, 104, 104, src_bind="so2r.dot{i}", **pr),
        label(140, 124, None, 7, TEXT, bind_text="so2r.name{i}", wrap_width=164, max_lines=1, fit_text=True,
              text_min_scale=4, **pr),
        {"type": "rect", "rect": [16, 84, CARD_W, 120], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
         "on_tap": "so2r.pick{i}", **pr},
    ]
    return out


ELEMENTS = 6   # the element row's slots (the game's list, read at run time; 6 in this build)
STAT_ICONS = ["ATK", "DEF", "HIT", "AVD", "INT", "LUC", "STM", "CRT", "GUTS"]   # the game's icon_status_* pictures


DET_Y = 216            # a member's details: the panel under the subtabs and the picker (to FOOT_Y)
DET_H = FOOT_Y - 8 - DET_Y


def leader_button():
    """0.9.60 the game's Leader (X in Party Formation) for the chosen member: a tap makes the companion press the
    game's own Camp Menu > Party Formation > X for them, each press checked against the game's screens. 0.9.62 on
    the Status subtab, right of Switch Pages. Only a battle member who isn't the leader or a guest; dim
    otherwise; light blue while the companion is pressing."""
    y = DET_Y + 646
    bx, bw, bh = STATUS_BTN_X + STATUS_BTN_W + 20, STATUS_BTN_W, 56
    # 0.9.63 the leader's own: the game's striped light blue (like a chosen tab), white words
    out = [image(bx, y - 10, bw, bh + 20, pic("list_item_unselected"), fill="slice", slice=game_slice("list_item_unselected"),
                 need_bind="so2r.f.on"),
           image(bx, y - 10, bw, bh + 20, pic("list_item_selected"), fill="slice", slice=game_slice("list_item_selected"),
                 need_bind="so2r.f.busy"),
           image(bx, y - 10, bw, bh + 20, pic("list_item_selected"), fill="slice", slice=game_slice("list_item_selected"),
                 need_bind="so2r.f.lead")]
    for gate, col in (("so2r.f.can", MENU), ("so2r.f.not", DIM), ("so2r.f.busy", MENU), ("so2r.f.lead", MENU)):
        out.append(label(bx + bw // 2, y + (bh - 30) // 2, None, 5, col, bind_text="so2r.t.setlead", align="center",
                         wrap_width=bw - 20, max_lines=1, fit_text=True, text_min_scale=3, need_bind=gate))
    out.append({"type": "rect", "rect": [bx, y, bw, bh], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                "on_tap": "so2r.lead", "need_bind": "so2r.f.can"})
    return out


def status_widgets():
    """The Status subtab (stats; Switch Pages: talents, friendship, favorite food) and the Equipment
    subtab (the game's slot icons and the element row), in the game's words."""
    out = []
    s = {"need_bind": "so2r.s.st0on"}
    p0 = {"need_bind": "so2r.s.p0"}
    p1 = {"need_bind": "so2r.s.p1"}
    p2 = {"need_bind": "so2r.s.p2"}
    y = DET_Y
    out += [
        # 0.9.131 no dark panel: the subtab floats on the page's blue background
        image(40, y + 24, 96, 96, src_bind="so2r.s.dot", **s),
        label(156, y + 28, None, 8, TEXT, bind_text="so2r.s.name", **s),
        label(156, y + 84, None, 6, DIM, bind_text="so2r.t.lv", **s),
        {"type": "value", "rect": [210, y + 78, 0, 0], "bind": "so2r.s.lv", "text_scale": 8, "color": TEXT, **s},
        label(310, y + 84, None, 6, DIM, bind_text="so2r.t.exp", **s),
        {"type": "value", "rect": [390, y + 78, 0, 0], "bind": "so2r.s.exp", "text_scale": 7, "color": TEXT, **s},
        # Next Level: the EXP still needed (the game's own table)
        label(310, y + 128, None, 5, DIM, bind_text="so2r.t.next", need_bind="so2r.s.hasnext"),
        {"type": "value", "rect": [560, y + 122, 0, 0], "bind": "so2r.s.next", "text_scale": 6, "color": TEXT,
         "need_bind": "so2r.s.hasnext"},
        # 0.9.128 the game's look: the word nearer the numbers, the maximum after the slash smaller, in the word's colour
        label(890, y + 34, None, 6, HPC, bind_text="so2r.t.hp", **s),
        {"type": "value", "rect": [1094, y + 26, 0, 0], "bind": "so2r.s.hp", "align": "right", "text_scale": 7,
         "color": TEXT, **s},
        {"type": "value", "rect": [1098, y + 38, 0, 0], "bind": "so2r.s.hpmax", "text": "/", "text_scale": 5,
         "color": HPC, **s},
        label(890, y + 84, None, 6, MPC, bind_text="so2r.t.mp", **s),
        {"type": "value", "rect": [1094, y + 76, 0, 0], "bind": "so2r.s.mp", "align": "right", "text_scale": 7,
         "color": TEXT, **s},
        {"type": "value", "rect": [1098, y + 88, 0, 0], "bind": "so2r.s.mpmax", "text": "/", "text_scale": 5,
         "color": MPC, **s},
        # the ailments on now (Poison, Paralysis ...; the game's words), in a row
        label(700, y + 128, None, 5, "#FFE07070", bind_text="so2r.s.bdw{i}", need_bind="so2r.s.bdon{i}",
              bg="#B0050C18", pill=True, auto_w=True, repeat=3, repeat_dx=170),
        # the stats (the game's Status words), two columns
        label(40, y + 166, None, 6, MENU, bind_text="so2r.t.status", **p0),
    ]
    for k in range(9):
        cx, cy = 40 + (k % 3) * 400, y + 216 + (k // 3) * 60
        out += [image(cx, cy - 4, 40, 40, pic(f"icon_status_{STAT_ICONS[k]}"), **p0),
                label(cx + 50, cy, None, 6, DIM, bind_text=f"so2r.t.st{k}", **p0),
                {"type": "value", "rect": [cx + 270, cy - 4, 0, 0], "bind": f"so2r.s.st{k}", "align": "right",
                 "text_scale": 7, "color": TEXT, **p0}]
    # the element row under the stats (the game's Element: its order, battle_icon_element_* and its words)
    out.append(label(40, y + 420, None, 6, MENU, bind_text="so2r.t.element", need_bind="so2r.s.elrow"))
    # two rows like the game's Status (Fire / Water, Wind / Earth, Light / Dark): icon, then the word
    for k in range(ELEMENTS):
        ex, ey = 40 + (k // 2) * 390, y + 476 + (k % 2) * 64
        on = {"need_bind": f"so2r.s.elon{k}"}
        out += [rect(ex, ey, 370, 52, "#80223A5C", pill=True, **on),
                image(ex + 8, ey + 4, 44, 44, src_bind=f"so2r.s.elic{k}", **on),
                label(ex + 350, ey + 10, None, 5, TEXT, bind_text=f"so2r.s.elw{k}", align="right", wrap_width=280,
                      max_lines=1, fit_text=True, text_min_scale=3, **on),
                label(ex + 350, ey + 10, None, 5, "#FFE07070", bind_text=f"so2r.s.elw{k}", align="right",
                      wrap_width=280, max_lines=1, fit_text=True, text_min_scale=3, need_bind=f"so2r.s.elbad{k}")]
    # the Equipment subtab, like the game's Equipment screen: each slot (the game's words), its icon
    # (icon_item_* by category; the weapon's by the character's weapon), the item or the game's None;
    # beside it one column of HP / MP, the stats and the elements
    out.append(label(40, y + 14, None, 6, MENU, bind_text="so2r.t.equipment", **p2))
    for k in range(7):          # 0.9.30 the game's card height (its frame 60 high; the sprite has 10 px margins)
        cy = y + 62 + k * 97
        out += [label(40, cy, None, 5, MENU, bind_text=f"so2r.t.eq{k}", **p2),
                image(40, cy + 18, 680, 80, pic("list_item_unselected"), fill="slice",
                      slice=game_slice("list_item_unselected"), **p2),
                # 0.9.135 a tap: the game's Equipment screen on the chosen member and this slot (lighting up)
                image(40, cy + 18, 680, 80, pic("list_item_selected"), fill="slice", slice=game_slice("list_item_selected"),
                      need_bind=f"so2r.bt.lit{12 + k}"),
                image(54, cy + 36, 44, 44, src_bind=f"so2r.s.eqic{k}", need_bind=f"so2r.s.eqicon{k}"),
                label(110, cy + 40, None, 6, MENU, bind_text=f"so2r.s.eq{k}", wrap_width=590, max_lines=1,
                      fit_text=True, text_min_scale=4, need_bind=f"so2r.s.has{k}"),
                label(110, cy + 40, None, 6, MENU, bind_text="so2r.t.none", need_bind=f"so2r.s.eqno{k}"),
                {"type": "rect", "rect": [40, cy + 18, 680, 80], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                 "on_tap": f"so2r.keq{k}", **p2}]
    # 0.9.132 the member's whole picture at the right in place of the stats (as on the Skills pages)
    out.append(image(BODY_X, BODY_Y, BODY_W, BODY_H, src_bind="so2r.s.body", need_bind="so2r.s.p2"))
    # page 2 (the game's Y: Switch Pages), 0.9.28 the game's way: Friendship Level at the left; Talents at the
    # right (one column) and Favorite Food under them; each heading with the game's white line
    out += [label(40, y + 160, None, 6, MENU, bind_text="so2r.t.friend", **p1), rule(40, y + 204, 580, **p1),
            label(660, y + 160, None, 6, MENU, bind_text="so2r.t.talents", **p1), rule(660, y + 204, 540, **p1),
            label(660, y + 218, None, 5, TEXT, bind_text="so2r.s.tlw{i}", need_bind="so2r.s.tlon{i}",
                  repeat=10, repeat_dy=36),
            label(660, y + 590, None, 6, MENU, bind_text="so2r.t.food", **p1), rule(660, y + 634, 540, **p1),
            label(660, y + 648, None, 6, TEXT, bind_text="so2r.s.food", wrap_width=540, max_lines=1, fit_text=True,
                  text_min_scale=4, **p1)]
    for k in range(7):
        fy = y + 224 + k * 62
        on = {"need_bind": f"so2r.s.fron{k}"}
        out += [image(40, fy - 6, 52, 52, src_bind=f"so2r.s.frdot{k}", **on),
                label(104, fy, None, 5, TEXT, bind_text=f"so2r.s.frname{k}", wrap_width=200, max_lines=1,
                      fit_text=True, text_min_scale=3, **on),
                ] + [w for hk in range(5) for w in (
                    image(330 + hk * 58, fy - 4, 50, 48, pic("camp_status_favorability_off"), **on),
                    {**image(330 + hk * 58, fy - 4, 50, 48, pic("camp_status_favorability_on"), **on),
                     "fill_bind": f"so2r.s.ht{k}_{hk}", "max": 8})]
    # the switch (the game's Y: Switch Pages): bottom left, bigger words
    out += switch_button("so2r.spage", "so2r.s.st0on", x=STATUS_BTN_X, w=STATUS_BTN_W)
    return out


# 0.9.128 the Skills / Combat Skill lists: narrower (the stats column beside them, as on the Special Arts/Spells
# subtab) and dragged up / down for the rest (the module sends 32 rows at most)
SK_X0, SK_NW, SK_LX, SK_LW, SK_CX, SK_CW = 40, 500, 552, 96, 660, 120
SK_TOP, SK_PITCH, SK_ROWS = DET_Y + 128, 44, 32
SK_VIEW_H = 11 * SK_PITCH + 4


def skill_scroll(sid, count_bind, gate, view_h=SK_VIEW_H):
    return {"id": sid, "rect": [SK_X0, SK_TOP, SK_CX + SK_CW + 24 - SK_X0, view_h], "count_bind": count_bind,
            "row_h": SK_PITCH, "cols": 1, "pad": 0, "bar": "#FF5CD6FF", "bar_track": "#FF16334A", "bar_w": 8,
            "reset_bind": "so2r.k.key", "show_bind": gate}


BODY_W, BODY_H = 392, 740      # 0.9.130 the module's kBodyW / kBodyH (keep in step)
BODY_X, BODY_Y = 1216 - BODY_W, DET_Y + 8
SKILL_ILL_H = 508          # 0.9.129 the module's kSkillIllH (keep in step)


def skill_member_card():
    """0.9.129 the Skills / Combat Skill pages: the chosen member's Party Formation card (party_rows, the Party page's
    own card) beside the list, its lower part moved up to fit the panel (the picture's mask cut to SKILL_ILL_H)."""
    k_card = (FOOT_Y - 24) / 772
    cut = round((690 - SKILL_ILL_H) * k_card)
    dx, dy = 880 - 16, DET_Y + 12 - 16
    names = {"so2r.ro{i}": "so2r.kc.on", "so2r.rlead{i}": "so2r.kc.lead"}
    out = []
    for w in party_rows():
        w = dict(w)
        for key in ("repeat", "repeat_dx", "scroll"):
            w.pop(key, None)
        if w.get("on_tap") or w.get("need_bind") == "so2r.rguest{i}":
            continue
        r = list(w["rect"])
        if w.get("src_bind") == "so2r.ill{i}":
            r[3] -= cut
        elif r[1] <= 16 and r[3] > 500:          # the card's background
            r[3] -= cut
        else:
            r[1] -= cut
        r[0] += dx
        r[1] += dy
        w["rect"] = r
        for key in ("need_bind", "bind", "max_bind", "bind_text", "src_bind"):
            v = w.get(key)
            if isinstance(v, str) and "{i}" in v:
                w[key] = names.get(v, v.replace("so2r.", "so2r.kc.", 1).replace("{i}", ""))
        out.append(w)
    return out


ART_ROWS_SEEN = 7         # 0.9.133 the Special Arts/Spells list above its L / R / Improve cards
SKILL_SCROLLS = [skill_scroll("skl", "so2r.k.cnt", "so2r.k.p0"), skill_scroll("cmb", "so2r.c.cnt", "so2r.k.p1"),
                 skill_scroll("art", "so2r.a.cnt", "so2r.k.p3", ART_ROWS_SEEN * SK_PITCH + 4)]


def game_skill_rows(y, gate, name_k, lv_k, cost_k, has_k, on_k, cost_word, sid, rows=SK_ROWS, lv_text=False):
    """0.9.124 the Improve list in the mod's card style (the Guide Map sidebar's buttons: the game's pill frame
    list_item_unselected, drawn 10 px taller each way): each skill a bar with its name, then the Lv and the cost in
    their own frames; the column heads in frames too. 0.9.128 a scroll of up to 32 rows (11 seen)."""
    x0, nw, lx, lw, cx, cw = SK_X0, SK_NW, SK_LX, SK_LW, SK_CX, SK_CW
    rh, cy = 40, SK_TOP
    g = {"need_bind": gate}
    rp = {"repeat": rows, "repeat_dy": SK_PITCH, "scroll": sid}
    def box(x, yy, w, h, **kw):
        return image(x, yy - 10, w, h + 20, pic("list_item_unselected"), fill="slice",
                     slice=game_slice("list_item_unselected"), **kw)
    out = [box(lx, y + 80, lw, 40, **g), box(cx, y + 80, cw, 40, **g),
           label(lx + lw // 2, y + 86, None, 4, MENU, bind_text="so2r.t.lv", align="center", **g),
           label(cx + cw // 2, y + 86, None, 4, MENU, bind_text=cost_word, align="center", **g)]
    on = {"need_bind": f"{on_k}{{i}}"}
    out += [box(x0, cy, nw, rh, **on, **rp), box(lx, cy, lw, rh, **on, **rp), box(cx, cy, cw, rh, **on, **rp),
            label(x0 + 20, cy + 4, None, 5, MENU, bind_text=f"{name_k}{{i}}", wrap_width=nw - 40, max_lines=1,
                  fit_text=True, text_min_scale=3, **on, **rp),
            # 0.9.133 the arts' Lv is the game's "1 / 2" (a text)
            (label(lx + lw // 2, cy + 4, None, 5, MENU, bind_text=f"{lv_k}{{i}}", align="center", **on, **rp) if lv_text else
             {"type": "value", "rect": [lx + lw // 2, cy + 2, 0, 0], "bind": f"{lv_k}{{i}}", "align": "center",
              "text_scale": 5, "color": MENU, **on, **rp}),
            {"type": "value", "rect": [cx + cw // 2, cy + 2, 0, 0], "bind": f"{cost_k}{{i}}", "align": "center",
             "text_scale": 5, "color": MENU, "need_bind": f"{has_k}{{i}}", **rp}]
    return out


def skills_widgets():
    """The Special Arts/Spells subtab and the Skills subtab (Skills: SP, Combat Skills: BP, IC /
    Specialty / Super Specialty; Switch Pages), with the stats beside them -- every name the game's."""
    out = []
    DX = 0                      # 0.9.123 back at the left (0.9.122 centred them)
    k = {"need_bind": "so2r.k.on"}
    p0 = {"need_bind": "so2r.k.p0"}
    p1 = {"need_bind": "so2r.k.p1"}
    p2 = {"need_bind": "so2r.k.p2"}
    p3 = {"need_bind": "so2r.k.p3"}
    y = DET_Y
    out += [
        # 0.9.131 no dark panel: the subtab floats on the page's blue background
        # 0.9.125 the member's picture and name only on the Special Arts/Spells subtab (the Skills pages have the
        # member buttons above)
        # page 1: Skills, SP left to spend, then each skill: name, Lv, SP to improve
        # 0.9.122 the Skills subtab's pages without the stats, centred (DX)
        # 0.9.126 the game's menu white; Skills down by the Lv / SP heads; SP over the Lv frame, the points over theirs
        label(52, y + 72, None, 6, MENU, bind_text="so2r.t.skills", **p0),   # 0.9.127 at the card's edge, a little up
        label(SK_LX + SK_LW // 2, y + 40, None, 6, MENU, bind_text="so2r.t.sp", align="center", **p0),
        {"type": "value", "rect": [SK_CX + SK_CW // 2, y + 34, 0, 0], "bind": "so2r.k.sp", "align": "center", "text_scale": 7,
         "color": MENU, **p0},
    ]
    out += game_skill_rows(y, "so2r.k.p0", "so2r.k.n", "so2r.k.lv", "so2r.k.c", "so2r.k.hc", "so2r.k.on", "so2r.t.sp", "skl")
    # page 2: special arts / spells -- 0.9.133 in the Skills pages' style (their head, BP over the Lv frame, the
    # rows in the mod's frames, scrolled), the Lv the game's "1 / 2"
    out += [label(52, y + 72, None, 6, MENU, bind_text="so2r.t.arts", wrap_width=548, max_lines=1, fit_text=True,
                  text_min_scale=3, **p3),
            label(SK_LX + SK_LW // 2, y + 40, None, 6, MENU, bind_text="so2r.t.bp", align="center", **p3),
            {"type": "value", "rect": [SK_CX + SK_CW // 2, y + 34, 0, 0], "bind": "so2r.k.bp", "align": "center",
             "text_scale": 7, "color": MENU, **p3}]
    out += game_skill_rows(y, "so2r.k.p3", "so2r.a.n", "so2r.a.lvt", "so2r.a.c", "so2r.a.hc", "so2r.a.on", "so2r.t.bp",
                           "art", rows=16, lv_text=True)
    # the arts set on the L / R buttons -- 0.9.122 the Battle page's L / R cards (the game's button icon, the art's
    # name centred; a tap opens the game's Special Arts/Spells, lighting up)
    # 0.9.124 the L / R cards shorter, and Improve under them as a third card the same size (lighting up the same way)
    bx, bw, bh = 40, 500, 80
    for b in range(3):
        by = y + 470 + b * (bh + 12)
        out += [image(bx, by, bw, bh, pic("list_item_unselected"), fill="slice", slice=game_slice("list_item_unselected"), **p3),
                image(bx, by, bw, bh, pic("list_item_selected"), fill="slice", slice=game_slice("list_item_selected"),
                      need_bind=f"so2r.bt.lit{(2, 3, 11)[b]}")]
        if b < 2:
            out += [image(bx + 14, by + (bh - 64) // 2, 64, 64, pic(("button_cell_6_0", "button_cell_1_4")[b]), **p3),
                    label(bx + bw // 2 + 20, by + (bh - 40) // 2, None, 6, MENU, bind_text=f"so2r.a.eq{b}", align="center",
                          wrap_width=bw - 140, max_lines=1, fit_text=True, text_min_scale=3, **p3)]
        else:
            out.append(label(bx + bw // 2, by + (bh - 40) // 2, None, 6, MENU, bind_text="so2r.t.improve", align="center",
                             wrap_width=bw - 40, max_lines=1, fit_text=True, text_min_scale=3, **p3))
        out.append({"type": "rect", "rect": [bx, by, bw, bh], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                    "on_tap": ("so2r.kart0", "so2r.kart1", "so2r.kimp2")[b], **p3})   # 0.9.133 the chosen member
    # 0.9.133 no stats beside the list: the member's picture (below) as on the Skills pages
    # 0.9.130 the player's choice: the member's whole picture alone (no card, no tags) -- the module's "@body:"
    out.append(image(BODY_X, BODY_Y, BODY_W, BODY_H, src_bind="so2r.kc.body", need_bind="so2r.kc.on"))
    # page 3: combat skills -- BP left to spend, each released one: name, Lv, BP to improve
    out += [label(52, y + 72, None, 6, MENU, bind_text="so2r.t.combat", wrap_width=548, max_lines=1, fit_text=True,
                  text_min_scale=3, **p1),         # 0.9.126 as the Skills page
            label(SK_LX + SK_LW // 2, y + 40, None, 6, MENU, bind_text="so2r.t.bp", align="center", **p1),
            {"type": "value", "rect": [SK_CX + SK_CW // 2, y + 34, 0, 0], "bind": "so2r.k.bp", "align": "center", "text_scale": 7,
             "color": MENU, **p1}]
    out += game_skill_rows(y, "so2r.k.p1", "so2r.c.n", "so2r.c.lv", "so2r.c.c", "so2r.c.hc", "so2r.c.on", "so2r.t.bp", "cmb")
    # page 4: IC | Specialty | Super Specialty (learned by anyone in the party bright, the rest dim;
    # IC / Specialty: the chosen member's level; Super Specialty: the party's level)
    for col, (pre, word, n) in enumerate((("ic", "so2r.t.ic", 12), ("sk", "so2r.t.spec", 12), ("ss", "so2r.t.super", 12))):
        x0 = 40 + col * 396
        # 0.9.126 the head in the game's menu white with the game's ruled line under it
        out += [label(x0, y + 24, None, 6, MENU, bind_text=word, wrap_width=360, max_lines=1, fit_text=True,
                      text_min_scale=4, **p2),
                image(x0 - 8, y + 64, 376, 8, pic("camp_ruledline"), fill="slice", slice=game_slice("camp_ruledline"), **p2)]
        for r in range(n):
            cy = y + 80 + r * 44
            out += [label(x0, cy, None, 5, TEXT, bind_text=f"so2r.k.{pre}n{r}", wrap_width=270, max_lines=1,
                          fit_text=True, text_min_scale=3, need_bind=f"so2r.k.{pre}b{r}"),
                    label(x0, cy, None, 5, "#FF5E7090", bind_text=f"so2r.k.{pre}n{r}", wrap_width=270, max_lines=1,
                          fit_text=True, text_min_scale=3, need_bind=f"so2r.k.{pre}d{r}")]
            show = f"so2r.k.{pre}h{r}" if pre != "ss" else f"so2r.k.{pre}b{r}"
            out += [label(x0 + 290, cy + 4, None, 4, DIM, bind_text="so2r.t.lv", need_bind=show),
                    {"type": "value", "rect": [x0 + 370, cy - 2, 0, 0], "bind": f"so2r.k.{pre}l{r}", "align": "right",
                     "text_scale": 5, "color": TEXT, "need_bind": show}]
    out += switch_button("so2r.kpage", "so2r.k.sw")
    # 0.9.122 Improve beside Switch Pages: the game's Camp Menu > Improve > this page's own row
    # (0.9.123 not on the IC / Specialty page; the Special Arts/Spells subtab: Improve > Special Arts/Spells, under L / R)
    for pg, row in (("so2r.k.p0", 0), ("so2r.k.p1", 1)):
        out += tab(360, DET_Y + 646, 300, 56, "so2r.t.improve", 5, f"so2r.kimp{row}", gate=pg,
                   sel_bind=f"so2r.bt.lit{9 + row}")      # 0.9.124 lights up when tapped
    return out


def missions_page(diag):
    """The game's Missions menu: Challenge Missions (its four rank tabs) and Guild Missions, each a list
    in the game's list frames with the game's Complete / Incomplete."""
    out = [rect(0, 0, 1240, 1080, BG)] + dots_top() + dots_bottom() + header(diag, "missions")   # 0.9.51 dots_top
    m = {"need_bind": "so2r.m.on"}
    # 0.9.31 the game's look: its pill frames, its striped blue on the chosen tab, plain white words
    for t, word in enumerate(("so2r.t.cmis", "so2r.t.gmis")):
        out += tab(16 + t * 608, 16, 600, 60, word, 6, f"so2r.msec{t}", sel_bind=f"so2r.m.sec{t}", gate="so2r.m.on")
    for t in range(4):     # Challenge Missions: the game's four rank tabs (its own words)
        out += tab(16 + t * 304, 92, 296, 56, f"so2r.m.tw{t}", 6, f"so2r.mtab{t}", sel_bind=f"so2r.m.tab{t}",
                   gate="so2r.m.tabw")
    # the list: the game's taller rows (frame 72), one text size, a long name on two lines; drag to scroll
    y0, dy, nrows = 168, 84, 80
    fh = 72
    scroll = {"id": "mis", "rect": [16, y0, 1208, FOOT_Y - 8 - y0], "count_bind": "so2r.m.n", "row_h": dy, "cols": 1, "pad": 0,
              "bar": "#FF5CD6FF", "bar_track": "#FF16334A", "bar_w": 8, "reset_bind": "so2r.m.key", "show_bind": "so2r.m.on"}
    rp = {"repeat": nrows, "repeat_dy": dy, "scroll": "mis"}
    out += [
        image(16, y0 - 10, 960, fh + 20, pic("list_item_unselected"), fill="slice", slice=game_slice("list_item_unselected"),
              need_bind="so2r.m.on{i}", **rp),
        image(984, y0 - 10, 220, fh + 20, pic("list_item_unselected"), fill="slice",
              slice=game_slice("list_item_unselected"), need_bind="so2r.m.on{i}", **rp),
        label(36, y0, None, 6, MENU, bind_text="so2r.m.n{i}", wrap_width=760, max_lines=2, line_gap=4,
              text_center_h=fh, need_bind="so2r.m.on{i}", **rp),
        # the progress, like the game's (count / target)
        label(956, y0 + (fh - 30) // 2, None, 6, MENU, bind_text="so2r.m.c{i}", align="right", need_bind="so2r.m.on{i}",
              **rp),
        label(1094, y0 + (fh - 30) // 2, None, 6, "#FF4FE8B0", bind_text="so2r.t.done", align="center", wrap_width=200,
              max_lines=1, fit_text=True, text_min_scale=4, need_bind="so2r.m.ok{i}", **rp),
        label(1094, y0 + (fh - 30) // 2, None, 6, "#FF8E9AB0", bind_text="so2r.t.notdone", align="center", wrap_width=200,
              max_lines=1, fit_text=True, text_min_scale=4, need_bind="so2r.m.ng{i}", **rp),
        # 0.9.64 tap a mission: the companion opens it in the game's own Missions list (Camp Menu > Missions, the tab,
        # the cursor on it), pressing the game's buttons; the row in the game's striped light blue meanwhile
        {"type": "rect", "rect": [16, y0, 1188, fh], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
         "on_tap": "so2r.mrow{i}", "need_bind": "so2r.m.on{i}", **rp}]
    out.insert(len(out) - 6, image(16, y0 - 10, 960, fh + 20, pic("list_item_selected"), fill="slice",
                                   slice=game_slice("list_item_selected"), need_bind="so2r.m.sel{i}", **rp))
    out.append(label(620, 500, None, 7, TEXT, bind_text="so2r.t.nomis", align="center", need_bind="so2r.m.none"))
    page = {"id": "missions", "title": "Missions", "widgets": out + notices(diag), "scrolls": [scroll]}
    return page


def battle_page(diag):
    """0.9.70 the Battle page: the leader featured at the left (the Party Formation picture, bigger, cut at the
    page's foot), the other battle members across the top right (three at most: pixel portrait, name, Lv, HP / MP
    with the game's gauges), the leader's L / R arts (a tap: L / R in a battle; outside one the game's Special
    Arts/Spells), and the battle commands (a tap in a battle: X, the game's command menu)."""
    out = [rect(0, 0, 1240, 1080, BG)] + dots_top() + dots_bottom() + header(diag, "battle")
    on = {"need_bind": "so2r.bt.on"}
    # 0.9.71 the leader: the Party page's roster card itself (party_rows), its first card, bound to the leader
    # 0.9.72 in a battle the controlled member; LEADER only when that is the leader
    lead = {"so2r.ro{i}": "so2r.bt.on", "so2r.rlead{i}": "so2r.bt.islead"}
    # 0.9.93 the card is shorter (its lower part moved up by BATTLE_CUT, the picture's mask cut to BATTLE_ILL_H
    # rows: the module's "@ill:<NNNN>:<H>") to leave room under it for the bonus gauge and its effects
    k_card = (FOOT_Y - 24) / 772
    cut = round((690 - BATTLE_ILL_H) * k_card)
    for w in party_rows():
        w = dict(w)
        for key in ("repeat", "repeat_dx", "scroll"):
            w.pop(key, None)
        if w.get("on_tap") or w.get("need_bind") == "so2r.rguest{i}":
            continue
        r = list(w["rect"])
        if w.get("src_bind") == "so2r.ill{i}":
            w["src_bind"] = "so2r.bt.illc"
            r[3] -= cut
        elif r[1] <= 16 and r[3] > 500:          # the card's background
            r[3] -= cut
        else:
            r[1] -= cut
        w["rect"] = r
        for key in ("need_bind", "bind", "max_bind", "bind_text", "src_bind"):
            v = w.get(key)
            if isinstance(v, str) and "{i}" in v:
                w[key] = lead.get(v, v.replace("so2r.", "so2r.bt.", 1).replace("{i}", ""))
        out.append(w)
    px, pw = 16, CARD_W
    rx, rw, gap = px + pw + 16, 1224 - (px + pw + 16), 12
    # the other battle members: the Guide Map page's party cards, three across
    out += member_cards(rx, rw, (3,), lambda what, n, i: {"on": f"so2r.bo.on{i}", "lead": "so2r.never",
                        "guest": f"so2r.bo.guest{i}", "tap": f"so2r.bo.tap{i}"}.get(what, f"so2r.bo.{what}{i}"))
    # 0.9.157 the ailments on each member (the game's battle icons, battle_icon_abnormal_*): a column of up to three
    # beside the pixel portrait on the member cards, and down the right edge of the controlled member's card under its
    # bonus effects
    cwm = (rw - 8 * 2) // 3
    for i in range(3):
        x = rx + i * (cwm + 8)
        for j in range(3):
            out.append(image(x + 102, 16 + 50 + j * 42, 40, 40, src_bind=f"so2r.bo{i}.abi{j}",
                             need_bind=f"so2r.bo{i}.abon{j}"))
    for j in range(3):
        out.append(image(px + pw - 64, 200 + j * 56, 52, 52, src_bind=f"so2r.bt.abi{j}", need_bind=f"so2r.bt.abon{j}"))
    # 0.9.86 a tap on a card switches control to that member
    for w in out:
        if str(w.get("on_tap", "")).startswith("so2r.bo.tap"):
            w["on_tap"] = "so2r.bsw" + w["on_tap"][len("so2r.bo.tap"):]
    cy, ch = 16, 184
    # 0.9.107 the player's layout: four rows under the member cards. On the right, under the third card and its
    # width (Ashton's), the attack (A), the counter (B), then the L and R arts; on their left, from the first of
    # these cards to the second, the game's battle commands (Items, Spells, Strategy, Escape) -- each drawn like the
    # member cards. The cards' 184 rows don't fit four times over the Y row and the assault members, so the rows
    # share the room left (the assault characters a little smaller).
    mgap = 8                                                  # member_cards' gap
    cw3 = (rw - 2 * mgap) // 3
    col_x, col_w = rx + 2 * (cw3 + mgap), cw3                 # under the third card
    cmd_x, cmd_w = rx, 2 * cw3 + mgap                         # under the first two
    ch4 = 170                                                 # the assault members' area (their taps)
    y0 = FOOT_Y - 6 - ch4
    ry0, rgap = cy + ch + 12, 12
    span_end = y0 - 18                                        # 0.9.110 both columns end here (the Y row's foot)
    rh_r = (span_end - ry0 - 3 * rgap) // 4                   # the right column: A, B, L, R (taller)
    rh_m = (span_end - ry0 - 4 * rgap) // 5                   # the middle: the four commands and Y (shorter)
    def btn_icon(name, x, yy, size, always=False):
        """0.9.112 the game's own button icon (text_icon_switch, r73): bright in a battle, dimmed outside one
        (always: the L / R arts, which work outside one too)"""
        if always:
            return [image(x, yy, size, size, pic(name), **on)]
        return [image(x, yy, size, size, pic(name), need_bind="so2r.bt.inb"),
                image(x, yy, size, size, pic(name), tint=DIM, need_bind="so2r.bt.outb")]
    def card(x, yy, w, h, lit=None, **kw):
        """0.9.121 lit: the button's number -- the main tabs' blue (list_item_selected) over it while tapped"""
        c = image(x, yy, w, h, pic("list_item_unselected"), fill="slice", slice=game_slice("list_item_unselected"), **kw)
        if lit is None:
            return c
        return [c, image(x, yy, w, h, pic("list_item_selected"), fill="slice", slice=game_slice("list_item_selected"),
                         need_bind=f"so2r.bt.lit{lit}")]
    def tap(x, yy, w, h, act):
        return {"type": "rect", "rect": [x, yy, w, h], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                "on_tap": act, "need_bind": "so2r.bt.inb"}
    for j in range(4):
        # the right column, under the third card: A, B, L, R -- as tall as the middle's five rows together
        yy = ry0 + j * (rh_r + rgap)
        if j < 2:
            icon, word, act = (("button_cell_0_1", "so2r.t.batk", "so2r.ba"), ("button_cell_0_2", "so2r.t.bctr", "so2r.bb"))[j]
            out += card(col_x, yy, col_w, rh_r, lit=j, **on) + btn_icon(icon, col_x + 14, yy + (rh_r - 64) // 2, 64) + [
                    label(col_x + col_w // 2 + 20, yy + (rh_r - 34) // 2, None, 5, MENU, bind_text=word, align="center",
                          wrap_width=col_w - 90, max_lines=1, fit_text=True, text_min_scale=3, **on),
                    tap(col_x, yy, col_w, rh_r, act)]
        else:
            b2 = j - 2
            # 0.9.122 laid out like A / B: the icon at the left, the art's name centred (no "Button")
            out += card(col_x, yy, col_w, rh_r, lit=j, **on) + btn_icon(("button_cell_6_0", "button_cell_1_4")[b2],
                                                                   col_x + 14, yy + (rh_r - 64) // 2, 64, always=True) + [
                    label(col_x + col_w // 2 + 20, yy + (rh_r - 34) // 2, None, 5, MENU, bind_text=f"so2r.bt.art{b2}",
                          align="center", wrap_width=col_w - 100, max_lines=1, fit_text=True, text_min_scale=3, **on),
                    # 0.9.111 the L / R taps outside a battle too (the game's Special Arts/Spells), as before 0.9.107
                    dict(tap(col_x, yy, col_w, rh_r, f"so2r.bart{b2}"), need_bind="so2r.bt.on")]
        # the middle, across the first two cards: Items, Spells, Strategy, Escape
        yy = ry0 + j * (rh_m + rgap)
        word = ("so2r.t.qitems", "so2r.t.bspell", "so2r.t.bstrat", "so2r.t.bescape")[j]
        out += card(cmd_x, yy, cmd_w, rh_m, lit=4 + j, **on) + [
                label(cmd_x + cmd_w // 2, yy + (rh_m - 40) // 2, None, 7, MENU, bind_text=word, align="center",
                      wrap_width=cmd_w - 40, max_lines=1, fit_text=True, text_min_scale=4, need_bind="so2r.bt.inb"),
                label(cmd_x + cmd_w // 2, yy + (rh_m - 40) // 2, None, 7, DIM, bind_text=word, align="center",
                      wrap_width=cmd_w - 40, max_lines=1, fit_text=True, text_min_scale=4, need_bind="so2r.bt.outb"),
                tap(cmd_x, yy, cmd_w, rh_m, f"so2r.bcmd{j}")]
    # 0.9.72 the battle's Y (its orders); 0.9.110 the middle's fifth row, under Escape
    bh2 = rh_m
    y = ry0 + 4 * (rh_m + rgap)
    # Y: the 1.0.1 sheet's cell 5,1 (the player's matching, 0.9.119's box #1)
    out += card(cmd_x, y, cmd_w, bh2, lit=8, **on) + btn_icon("button_cell_5_1", cmd_x + 16, y + (bh2 - 60) // 2, 60) + [
            # 0.9.81 the order now in force (the game's words)
            label(cmd_x + cmd_w // 2 + 20, y + (bh2 - 36) // 2, None, 6, TEXT, bind_text="so2r.bt.order", align="center",
                  wrap_width=cmd_w - 120, max_lines=1, fit_text=True, text_min_scale=3),
            tap(cmd_x, y, cmd_w, bh2, "so2r.by")]
    # 0.9.89 the bonus gauge; 0.9.95 bigger, centred under the controlled member's card; its effects over the
    # card's top (as in 0.9.90). The picture is 240 game units across (r69): the count at its centre (6 units
    # down), "/ next" 15 right and 45 down (MaxCountParent); colours from the prefab's texts (r70).
    # 0.9.97 bigger: the picture as wide as the card (its soft glow may reach a little past the space; the
    # sphere itself stays inside), centred in the space between the card and the tab bar
    gs = 240
    u = gs / 240
    card_bot = 16 + (FOOT_Y - 24) - cut
    gy = (card_bot + FOOT_Y) // 2 - gs // 2
    gx = px + (pw - gs) // 2
    ts_n, ts_m = 9, 4                                      # the count twice the "/ next" (r70: 48 / 24)
    out.append(image(gx, gy, gs, gs, src_bind="so2r.bn.pic", need_bind="so2r.bn.on"))
    # 0.9.96 the game's look: the glow drawn first as a wide soft line, then the white face with its orange line
    for line, px_n, px_m in ((BONUS_GLOW, 6, 4), (BONUS_LINE, 2, 2)):
        out += [{"type": "value", "rect": [gx + gs // 2, gy + gs // 2 - 29, 0, 0], "bind": "so2r.bn.pt",
                 "align": "center", "text_scale": ts_n, "color": BONUS_NUM, "outline": line, "outline_px": px_n,
                 "need_bind": "so2r.bn.txt"},
                label(gx + gs // 2 + round(15 * u), gy + gs // 2 + 26, None, ts_m, BONUS_MAX,
                      bind_text="so2r.bn.next", align="center", outline=line, outline_px=px_m, need_bind="so2r.bn.txt")]
    ex, ew = px + 8, pw - 16
    for k in range(3):                                     # 0.9.97 a little larger
        ey = 22 + k * 56
        a = {"need_bind": f"so2r.bn.e{k}"}
        out += [image(ex, ey, ew, 50, pic("battle_bonus_effect_background"), fill="slice",
                      slice=game_slice("battle_bonus_effect_background"), **a),
                image(ex + 4, ey + 4, 42, 42, src_bind=f"so2r.bn.ei{k}", **a),
                label(ex + 52, ey + 8, None, 5, MENU, bind_text=f"so2r.bn.et{k}", wrap_width=ew - 56, max_lines=1,
                      fit_text=True, text_min_scale=3, **a)]
    # 0.9.82 the orders (Y tapped here, or the game's own list open): the four in the game's words, 2 x 2 where the
    # assault members stand (they step aside meanwhile); the one in force striped like the game's chosen row
    oy0 = y + bh2 + 24
    ow, oh = (rw - gap) // 2, (FOOT_Y - oy0 - 30 - 16) // 2
    for k in range(1, 5):
        ox = rx + ((k - 1) % 2) * (ow + gap)
        oy = oy0 + ((k - 1) // 2) * (oh + 26)
        out += [image(ox, oy - 10, ow, oh + 20, pic("list_item_selected"), fill="slice",
                      slice=game_slice("list_item_selected"), need_bind=f"so2r.bt.ocur{k}"),
                image(ox, oy - 10, ow, oh + 20, pic("list_item_unselected"), fill="slice",
                      slice=game_slice("list_item_unselected"), need_bind=f"so2r.bt.ooff{k}"),
                label(ox + ow // 2, oy + (oh - 36) // 2, None, 5, TEXT, bind_text=f"so2r.t.ord{k}", align="center",
                      wrap_width=ow - 30, max_lines=1, fit_text=True, text_min_scale=3, need_bind=f"so2r.bt.oon{k}"),
                {"type": "rect", "rect": [ox, oy, ow, oh], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                 "on_tap": f"so2r.bord{k}", "need_bind": f"so2r.bt.oon{k}"}]
    # 0.9.73 assault actions, each with its d-pad direction; a tap presses that direction on the battle field
    # 0.9.77 as the game shows them (bottom left of its battle screen): side by side, the member's full-body pixel
    # character with the gauge ring at its feet, the name under it -- left, down, right, up (the game's order)
    gap4 = 8
    cw4 = (rw - 3 * gap4) // 4
    for slot, d in enumerate((1, 2, 3, 0)):
        x = rx + slot * (cw4 + gap4)
        a = {"need_bind": f"so2r.as.on{d}"}
        # 0.9.109 at their 0.9.106 size (198), feet and names where they were: the picture's empty top reaches up
        # behind (over) the Y row -- the player's choice over shorter button rows
        pic_h = 198
        py = FOOT_Y - 6 - 44 - pic_h + 2
        out += [image(x + (cw4 - pic_h) // 2, py, pic_h, pic_h, src_bind=f"so2r.as.dot{d}", **a),
                # 0.9.83 the ring where the game puts it: on the character, low and to the left of its middle
                # (the game: centre ~22 px left of the sprite's middle and ~33 px above its feet, ~40 px across, at
                # the same scale as these characters)
                image(x + cw4 // 2 - 22 - 22, py + pic_h - 33 - 22, 44, 44, src_bind=f"so2r.as.ring{d}", **a),
                label(x + cw4 // 2, py + pic_h, None, 5, MENU, bind_text=f"so2r.as.name{d}", align="center",
                      wrap_width=cw4 - 16, max_lines=1, fit_text=True, text_min_scale=3, **a),
                {"type": "rect", "rect": [x, y0, cw4, ch4], "bg": CLEAR, "color": CLEAR, "frame": 0, "noframe": True,
                 "on_tap": f"so2r.bas{d}", **a}]
    return {"id": "battle", "title": "Battle", "widgets": out + notices(diag)}


def diag_page():
    widgets = [rect(0, 0, 1240, 1080, BG)] + dots_bottom() + header(True, "diag")
    widgets.append(label(20, 12, None, 4, TEXT, bind_text="so2r.d.l{i}", wrap_width=1200,
                         max_lines=1, fit_text=True, text_min_scale=3, repeat=DIAG_LINES, repeat_dy=25))
    return {"id": "diag", "title": "Diag", "widgets": widgets, "no_auto_leave": True}   # 0.9.144 not left by page_binds


TITLE_LOGO_W, TITLE_LOGO_H = 900, 668   # 0.9.145 the title logo's box (logo_title 1024 x 760, fitted, centred)


def title_page():
    """0.9.144 the title screen: the game's title logo on the game's dotted panel, nothing else (no tabs) -- the
    companion shows once the game starts (page_binds on so2r.v.title). The logo is read from the player's own game
    files (pictures.idx "title_logo", r80); without it the panel alone."""
    x, y = (1240 - TITLE_LOGO_W) // 2, (1080 - TITLE_LOGO_H) // 2
    return {"id": "title", "title": "Title",
            # 0.9.145 (r80) the game's own title background (uiscene title_background, filling the page) and logo
            # 0.9.147 composed by the module as the top screen: background, globe, logo
            "widgets": [rect(0, 0, 1240, 1080, BG), image(0, 0, 1240, 1080, "module:pic/@titlebg")]
                       # 0.9.152 a button pressing A (as Dave the Diver's title page), in the mod's card style, lit when
                       # tapped; the word is the game's Continue
                       + tab(420, 700, 400, 80, "so2r.t.cont", 7, "so2r.title_a", sel_bind="so2r.bt.lit19",
                             gate="so2r.title.ready")}   # 0.9.155 shown with the title picture, not before


def build(diag):
    # Party: the roster; tap a member for their details (subtabs Status / Equipment / Arts/Spells / Skills)
    party = {"id": "party", "title": "Party",
             "widgets": [rect(0, 0, 1240, 1080, BG)] + dots_top() + dots_bottom() + party_rows() + details_top() + status_widgets() + leader_button()
                        + skills_widgets() + header(diag, "party") + notices(diag),
             "scrolls": [ROSTER_SCROLL, PICK_SCROLL] + SKILL_SCROLLS}
    # 0.9.144 the title page first: what shows before the game starts
    pages = [title_page(), party, map_page(diag), missions_page(diag), battle_page(diag)] + ([diag_page()] if diag else [])
    # 0.9.147 the party's whole pictures asked for on every page (2 px, nearly clear, in the top-left corner), so the
    # Equipment / Skills pages find them decoded instead of loading on the first visit
    for pg in pages[1:]:
        pg["widgets"] += [image(k * 2, 0, 2, 2, color="#01FFFFFF", src_bind=f"so2r.warm{k}") for k in range(8)]
    m = {
        "format": 1,
        "title_id": TITLE,
        "name": "Star Ocean: The Second Story R dual screen",
        "_about": f"Star Ocean: The Second Story R companion {VERSION}{' (diag)' if diag else ''}: "
                  "the title screen, the party and each member's status, equipment, arts and skills, the Guide Map with the "
                  "player's position and the game's icons, the missions and the battle, read live from the game's own objects "
                  "and drawn with the game's font, words and pictures read from the player's own game files. Ships no game "
                  "assets.",
        "min_runtime": MIN_RUNTIME,
        "requires_module": True,
        "load_plan": "plans/<build>.json",      # the HideHUD helper (tools/so2r/hidehud)
        "canvas_w": 1240,
        "canvas_h": 1080,
        "background": BG,
        "nav": False,
        "font": "module:fontdata",
        "font_atlas": "module:font",
        "haptics": {"enabled": True, "respect_system": True, "tap": "light", "refused": "off"},
        "derived": [
            {"name": "so2r.v.wait", "expr": "module_ready == 1 && so2r.ok == 0"},
            {"name": "so2r.v.arrow", "expr": "so2r.map.ok == 1 && so2r.map.aon == 1"},
            {"name": "so2r.v.nomap", "expr": "module_ready == 1 && so2r.ok == 1 && so2r.map.ok == 0"},
            # 0.9.18 the Map page: the sidebar and the map beside it (the standard layout)
            {"name": "so2r.v.mapn", "expr": "so2r.map.ok == 1"},
            # 0.9.25 the Map page's strip: card i of n (the cards share the width by their count)
            *[{"name": f"so2r.v.b{n}{w}{i}", "expr": f"so2r.bn == {n} && so2r.b{w}{i} == 1"}
              for n in range(1, CARDS + 1) for i in range(n) for w in ("on", "lead", "guest")],
            {"name": "so2r.v.side", "expr": "so2r.ok == 1"},
            {"name": "so2r.v.sidemap", "expr": "so2r.ok == 1 && so2r.map.ok == 1"},
            {"name": "so2r.v.sidefloor", "expr": "so2r.ok == 1 && so2r.map.hasfloor == 1"},
        ],
        "actions": {},
        "pages": pages,
        # 0.9.17 a tap on the Map page's party strip opens that member's Party details: the module flips
        # so2r.goparty, and either edge switches to the Party page
        "page_binds": [{"point": "so2r.goparty", "equals": 1, "when_equal": {"page": "party", "transition": "none"},
                        "when_not_equal": {"page": "party", "transition": "none"}},
                       # 0.9.144 the page follows the game: the title screen -> Title, the game started -> Guide Map,
                       # a battle -> Battle, back on the field / world map -> Guide Map
                       {"point": "so2r.v.title", "equals": 1, "when_equal": {"page": "title", "transition": "fade"},
                        "when_not_equal": {"page": "map", "transition": "fade"}},
                       {"point": "so2r.v.inbattle", "equals": 1, "when_equal": {"page": "battle", "transition": "none"}},
                       {"point": "so2r.v.onmap", "equals": 1, "when_equal": {"page": "map", "transition": "none"}}],
    }
    m["actions"] = {f"so2r.to_{pid}": {"kind": "page", "page": pid, "transition": "none"} for pid, _ in tabs(diag)}
    if diag:
        m["actions"]["so2r.to_diag"] = {"kind": "page", "page": "diag", "transition": "none"}   # 0.9.70 from the sidebar
    for a in ("so2r.map_mini", "so2r.map_whole", "so2r.map_toggle") + tuple(f"so2r.pick{i}" for i in range(ROWS)) + tuple(f"so2r.open{i}" for i in range(ROWS)) + ("so2r.roster",) + tuple(f"so2r.sub{t}" for t in range(len(SUBTABS))) + ("so2r.lead",) + ("so2r.spage", "so2r.kpage", "so2r.msec0", "so2r.msec1") + tuple(f"so2r.mtab{t}" for t in range(4)) + tuple(f"so2r.bopen{i}" for i in range(CARDS)) + tuple(f"so2r.mrow{i}" for i in range(80)) + ("so2r.bart0", "so2r.bart1", "so2r.by") + tuple(f"so2r.bas{d}" for d in range(4)) + tuple(f"so2r.bcmd{j}" for j in range(4)) + tuple(f"so2r.bord{k}" for k in range(1, 5)) + tuple(f"so2r.bsw{i}" for i in range(3)) + ("so2r.ba", "so2r.bb") + tuple(f"so2r.kimp{r}" for r in range(3)) + ("so2r.kart0", "so2r.kart1") + tuple(f"so2r.keq{k}" for k in range(7)) + ("so2r.hud_hide", "so2r.title_a"):   # 0.9.85 the orders, 0.9.86 switch
        m["actions"][a] = {"kind": "module", "action": a, "argument": 0}
    # 0.9.19 the quick menu: a tap tells the module (so2r.qtap<n>); the module asks for one press at a time
    # (so2r.q.f<n> = 1: d-pad up / left / down / right, X, 5 = B to close what is open) and these rules make
    # it; every press also toggles the flag so2r.qp, which the module reads back so each press is made once
    m["enforce"] = []
    for j, btn in enumerate([b for _, b in QUICK] + ["B", "A", "L", "R"]):   # 0.9.64 + A, L, R (7, 8, 9)
        m["actions"][f"so2r.q_{btn}"] = {"kind": "button", "button": btn, "frames": 4, "enabled_bind": f"so2r.q.f{j}"}
        m["enforce"].append({"action": f"so2r.q_{btn}", "every_ms": 60})    # 0.9.134 (100; 0.9.66 200)
    # 0.9.86 the battle's character switch: ZR held (~0.4 s), then ZR with the left stick down / up ("LY-" / "LY+")
    for j, (nm_, btn, fr) in enumerate((("ZR", "ZR", 24), ("ZRdown", "ZR+LY-", 10), ("ZRup", "ZR+LY+", 10)), start=10):
        m["actions"][f"so2r.q_{nm_}"] = {"kind": "button", "button": btn, "frames": fr, "enabled_bind": f"so2r.q.f{j}"}
        # 0.9.155 in step with the acknowledgement (60 ms since 0.9.134): at 100 the acknowledgement could come first
        # and drop or delay the ZR press, so the switch stepped past the member tapped
        m["enforce"].append({"action": f"so2r.q_{nm_}", "every_ms": 60})
    m["actions"]["so2r.q_ack"] = {"kind": "flag", "flag": "so2r.qp", "enabled_bind": "so2r.q.any"}
    m["enforce"].append({"action": "so2r.q_ack", "every_ms": 60})          # 0.9.134 (100)
    # the rules only run while a press is wanted (nothing is checked or logged the rest of the time)
    m["enforce_gate"] = {"point": "so2r.q.any", "max": 1}
    for j in range(len(QUICK)):
        m["actions"][f"so2r.qtap{j}"] = {"kind": "module", "action": f"so2r.qtap{j}", "argument": 0}
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diag", action="store_true")
    a = ap.parse_args()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    m = build(a.diag)

    def no_nulls(o, path):   # 0.9.29: the runtime refuses a manifest with a null where a string goes
        if o is None:
            raise SystemExit(f"gen_manifest: null at {path}")
        for k, v in (o.items() if isinstance(o, dict) else enumerate(o) if isinstance(o, list) else []):
            no_nulls(v, f"{path}/{k}")
    no_nulls(m, "")
    OUT.write_text(json.dumps(m, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} ({'diag' if a.diag else 'beta'})")


if __name__ == "__main__":
    main()
