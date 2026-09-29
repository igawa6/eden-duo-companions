#!/usr/bin/env python3
"""Generates the Mario Kart 8 Deluxe Eden Duo companion layout:
packages/MarioKart8Deluxe/dualscreen/manifest.json plus one data file per known build.
Edit the layout here, never the JSON.

Design v3 (packages 0.4.x, runtime 13), after the Wii U GamePad race screen of Mario Kart 8:
  - race pages: left, the glass rank table (one row per racer placed by its live rank) in two
    row formats, SHORT (icon + two item bubbles) and LONG (icon + bubble + small second bubble +
    full name); a finished racer shows the game's checkered flag where its items were. Right, the
    horn button (HORN mode) or the zoomed course map (MAP mode), and under it two wide buttons:
    mode switch and USE ITEM. Press-and-hold on the table toggles the row format, on the horn /
    map area the theme;
  - waiting, loading and wrong-version screens on the game's loading-screen background.
State = the runtime flags ui.map (mode), ui.dark (theme) and ui.long (row format). The derived
value ui.pg names the page for (version supported?, race phase, flags) and page_binds follow it,
so every toggle is a page switch and race entry lands on the last chosen combination. The row
format is not a page: both formats live on each race page as anim groups.

Asset-free: everything drawn comes from the player's romfs through module:mk8d:* keys, and the
font is the game's own turbo_MARIOFont. Only rect / pill primitives and colours are procedural.

Sections: builds / module contract, game art keys, themes, geometry, widget helpers, derived
values (all gating), page pieces, pages, actions, manifest, data files, bind check, output.
The bind check at the end refuses to write a manifest that references a value that is neither
derived here, a flag, a data-file constant nor a module output listed in MODULE_OUTPUTS.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.normpath(os.path.join(HERE, "..", "..", "packages", "MarioKart8Deluxe", "dualscreen"))
if len(sys.argv) > 1:                # output directory override (sync_to_companions.sh copy)
    PKG = os.path.abspath(sys.argv[1])

# ---- builds (the data-file name = the build id's first 8 bytes) -------------
# (build id, served by the module, mkc.build_ver, version text, data-file _source)
BUILDS = [
    ("2C336A9BCF79C304", 1, 400, "4.0.0", "4.0.0 (AArch64) base + update"),
    ("B5C39B5B7C62A88E", 0, 100, "1.0.0 (no update)", "1.0.0 base, no update"),
    ("6A85262F21B90364", 1, 303, "3.0.3", "3.0.3 (AArch32)"),
]
BUILD16 = BUILDS[0][0]
MODULE_BUILDS = [b for b, ok, _, _, _ in BUILDS if ok]           # manifest module.build_ids
SUPPORTED_TEXT = ", ".join(v for _, ok, _, v, _ in BUILDS if ok)  # "4.0.0, 3.0.3"
MIN_RUNTIME = 13

# mk.phase = SEQ+0x38 in a race scene, -1 elsewhere (verified live against the game). The data files carry the
# windows: loading card = 0..PHASE_LOAD_MAX, race = PHASE_RACE_MIN..PHASE_RACE_MAX.
PHASE_NAMES = ["loading", "loading screen", "fly-by", "intro end", "grid banner", "countdown",
               "racing", "local finished"]
PHASE_LOAD_MAX, PHASE_RACE_MIN, PHASE_RACE_MAX = 3, 4, 7

N_RACERS = 12                     # Mk8dReader::MaxRacers; one table row per racer

# Values the module publishes (repos/mk8d/src/core/mods/modules): mk8d_reader.cpp
# Reader::Publish(), the glue in 0100152000022000.cpp (PublishReadiness, PublishNameScales) and,
# mk.mock: only a private layout mock sets it, never the real module. module_error_message comes from the runtime.
MODULE_OUTPUTS = set("""
    mk.ready mk.scene mk.phase mk.course mk.cup mk.cc mk.mirror mk.mode mk.submode mk.laps_total
    mk.time_ms mk.course_folder mk.map_key mk.mapfit_ok mk.mapfit_key mk.pict_key mk.cup_key
    mk.course_name mk.cup_name mk.diag mk.sample_us mk.horn_ok mk.horn_busy mk.horn_count
    racers.count me.ready me.emblem_key me.slot me.rank me.finished me.lap me.coins me.item0
    me.item1 me.item0_count me.item1_count me.item_roulette me.item1_roulette me.item0_key
    me.item1_key
    mk.pict_ok mk.cup_ok mk.fit_img mk.map_img mk.name_ok mk.course_name_w
    rk.moving rk.pile rk.flip
    mc.row_y mc.row_dx mc.row_glow mc.icon mc.name mc.item0_key mc.item1_key mc.item_state
    mc.item1_state mc.finished mc.name_scale
    mk.mock module_error_message""".split())
RACER_FIELDS = """valid rank driver variant lap coins is_me map_x map_y map_ok icon name item0
    item1 item_state item1_state item0_count item1_count item0_key item1_key emblem_key mapf_x
    mapf_y finished name_scale row_y row_dx row_glow""".split()
MODULE_OUTPUTS |= {f"r{i}.{f}" for i in range(N_RACERS) for f in RACER_FIELDS}

# ---- game art (module:mk8d: keys, decoded from the player's romfs) ------------------
K = "module:mk8d:"
LYT_RACE = K + "lyt/cmn/race/"
LYT_MENU = K + "lyt/cmn/menu/"
CHECKER = LYT_MENU + "mn_Background_00/tc_MtCheck_64x64^s"       # 64x64, 2x2 white checks
GRAD_V = LYT_MENU + "mn_Background_00/ym_MenuBGGrd_00^r"         # 8x1048 white -> black
GLOSS = LYT_MENU + "mn_CompeInfo_00/tc_GlassHighLight_T_00^s"     # 64x64 white, alpha top->0
GRAD_H = LYT_RACE + "rc_RaceView_Cmn_00/tc_GrdAll_64x64^s"         # 64x64 alpha 0 (left) -> 1
SLOT_BASE = LYT_RACE + "rc_L_ItemBox_00/tc_ItemSlot_Base^s"      # 120x240 left half disc
SLOT_FRAME = LYT_RACE + "rc_L_ItemBox_00/tc_ItemSlot_Frame^s"    # 120x240 left half ring
FLAG = LYT_RACE + "rc_L_LapCoin_00/ym_LapFlag_00^f"              # 64x64 checkered flag
RING_HALF = K + "lytmat/cmn/race/rc_L_DRC_MapIconChara_00/P_ColorBG_00"   # 32x64 half ring
CROWN_HALF = K + "lytmat/cmn/race/rc_L_DRC_MapIconChara_00/P_MapIconCrown_00"  # 18x27 half
# The game's own loading screen (UI/cmn/common.sarc # cm_LoadScreen_00): the sticker pattern
# ym_LoadBGPtn_00^s (520x266, white + alpha; the layout tints it with vertex colour #E1E1E1 over
# the white P_FadeBG_00) and the band of black kart silhouettes ym_LoadChara_*_00 and
# "MARIOKART" letters ym_LoadLogo_*^s (vertex colour #000000) along N_Loop_00 (y -300 of 720).
LOAD_LYT = K + "lyt/cmn/common/cm_LoadScreen_00/"
LOAD_PTN = LOAD_LYT + "ym_LoadBGPtn_00^s"
LOAD_PTN_W, LOAD_PTN_H = 520, 266
# (texture, pane x, w, h) of N_Loop_00 (N_Chara_00/_01 and N_MARIOKART_00 at x -126), pane
# centres, read from blyt/cm_LoadScreen_00.bflyt
LOAD_BAND = [
    ("ym_LoadChara_Wro_00^s", -859, 102, 68), ("ym_LoadChara_Wlg_00^s", -628, 92, 74),
    ("ym_LoadChara_Pch_00^s", -374, 108, 60), ("ym_LoadChara_Mro_00^s", 232, 84, 54),
    ("ym_LoadChara_Kno_00^s", 455, 46, 52), ("ym_LoadChara_Dkg_00^s", 696, 117, 72),
    ("LoadChara_SplG_00^s", 1287, 77, 72), ("LoadChara_Lnk_00^s", 1518, 88, 74),
    ("ym_LoadChara_Nok_00^s", -741, 54, 56), ("ym_LoadChara_Knc_00^s", -505, 74, 52),
    ("ym_LoadChara_Kop_00^s", -235, 90, 106), ("ym_LoadChara_Ysi_00^s", 353, 78, 78),
    ("ym_LoadChara_Dsy_00^s", 558, 80, 62), ("ym_LoadChara_Lig_00^s", 838, 80, 58),
    ("LoadChara_Shz_00^s", 1400, 70, 52),
]
LOAD_LOGO = [("M", -128, 43), ("A", -89, 36), ("R", -54, 37), ("I", -29, 14), ("O", -5, 35),
             ("K", 29, 36), ("A", 63, 36), ("R", 98, 37), ("T", 132, 36)]   # (letter, x, w)
LOAD_GROUP_X = -126               # N_Chara_* / N_MARIOKART_00 group offset in N_Loop_00
# GamePad map blue: the game's menu blue, mn_Background_00 material P_BlueBG_00 white colour.
MAP_BLUE = "#FF128CD7"
# The item roulette (cosmetic): cycles these module:mk8d:item/<name> icons.
ROULETTE = ["Banana", "Koura", "Mush", "RedKoura", "Bomb", "Gesso", "Star", "Teresa",
            "Thunder", "Killer", "Flower", "Boomerang", "PackunFlower", "SuperHorn", "Togezo",
            "Coin"]
RL_SRC = [K + "item/" + n for n in ROULETTE]
N_RL = len(ROULETTE)
RL_STEP_MS = 110                  # roulette frame time

# ---- themes ------------------------------------------------------------------------------------
THEMES = {
    "light": dict(
        bg="#FFE3E5E9", check="#A6FFFFFF",
        plate="#FFFFFFFF", plate_sh="#FFC9CDD3", plate_hi="#FFFFFFFF", text="#FF26282C",
        sub="#FF6B7079", dim="#FF7A8089",
        bubble="#F0FFFFFF", bubble_rim="#FFD6DADF",
        bar_base="#FFC3C8D0", bar_tint="#FFFFFFFF", bar_gloss="#C8FFFFFF", bar_sep="#FF868C96",
        bar_hi="#FFFFFFFF", bar_left="#E6FFFFFF", bar_right="#FF3A3E45",
        num_fill="#FF3C3E43", num_rim="#FFFFFFFF", num_outer="#FF26282C",
        frame="#FF3D4046", bevel="#FF6A6E75", btn="#FF5A5F67", btn_blue="#FFBCE6FA",
        card="#FFF7F8FA", card_text="#FF26282C", ring="#FF2B2E34",
        mock="#FFE0303A",
        # loading screen (the game's colours): base P_FadeBG_00 white, pattern #E1E1E1, band black
        load_base="#FFFFFFFF", load_ptn="#FFE1E1E1", load_band="#FF000000",
        title_on_load="#FF4A4D53",
    ),
    "dark": dict(
        bg="#FF0A0F19", check="#12FFFFFF",
        plate="#FF182233", plate_sh="#FF070B12", plate_hi="#FF26344B", text="#FFF2F5F8",
        sub="#FFA7B6CC", dim="#FF8391A6",
        bubble="#FF111A2A", bubble_rim="#FF2A364A",
        bar_base="#FF2E3440", bar_tint="#FF77818F", bar_gloss="#70FFFFFF", bar_sep="#FF12161D",
        bar_hi="#FF6E7A8C", bar_left="#90FFFFFF", bar_right="#FF07090D",
        num_fill="#FFEEF1F5", num_rim=None, num_outer="#FF0A0D12",
        frame="#FF1C2230", bevel="#FF3A465A", btn="#FF222B3A", btn_blue="#FF0E2A44",
        card="#FF152033", card_text="#FFF2F5F8", ring="#FFF2F5F8",
        mock="#FFFF4A52",
        # dark loading screen = a documented transform of the game's colours: the base becomes
        # the theme background #0A0F19; the pattern keeps its contrast to the base but flips its
        # sign (light: #E1E1E1 = base - 0x1E -> dark: base + 0x1E = #28 2D 37); the black band
        # becomes the light UI grey #C8CED8 (luminance inverted)
        load_base="#FF0A0F19", load_ptn="#FF282D37", load_band="#FFC8CED8",
        title_on_load="#FFC8CED8",
    ),
}
ME_YELLOW = "#FFFFD21A"
ME_ROW = "#FFFFE11F"
ME_ROW_SH = "#FFD9A800"
ME_TEXT = "#FF2A1E00"
BAR_EDGE = "#FF4A4D53"
BAD_RED = "#FFE0303A"

# ---- geometry -----------------------------------------------------------------------------------
W, H = 1240, 1080                 # the aux canvas
RH = H // N_RACERS                # row pitch: 12 rows = 1080 -> 90
NW = 100                          # number cell width
PX = NW + 4                       # row plate x
ICON = 84                         # chara icon (tc_MapChara 64x64 at 1.31x)
BUB, BUB1 = 70, 36                # item bubble; LONG: second bubble small
NUM_SCALE = 8
# LONG names (0.4.0): the column is exactly as wide as "Rosalina" at scale 5 (MARIOFont advance
# 337 units -> 165 px); a longer name is drawn at the largest scale (5, 4, 3, 2) that fits that
# width. The module computes it from the same font's CWDH: r{i}.name_scale.
NAME_W = 170
NAME_SCALES = (5, 4, 3, 2)
FMT = {
    "short": dict(plate=6 + ICON + 2 + BUB + 4 + BUB + 10),                    # 246
    "long": dict(plate=6 + ICON + 2 + BUB + 12 + NAME_W + 12),                 # 356
}
FMT_MS = 320                      # row-format grow / shrink and content cross-fade
# Card-swap animation (1.0.0+, module glue mk8d_anim): every racer's card is placed at the
# module's displayed row position r{i}.row_y (row units, chases the racer's current rank; a new
# rank restarts the slide from where the card is drawn) and r{i}.row_dx (sideways drift, row
# units), both scaled by the row pitch. The timing lives in the module (Mk8dAnim::Config):
SWAP_NORMAL_MS = 150              # ordinary overtake: ease-out, the overtaking card drifts right
SWAP_PILEUP_MS = 100              # pile-up (>= SWAP_PILEUP_N cards moving): no drift, no glow
SWAP_PILEUP_N = 3
SWAP_DRIFT_ROWS = 0.30            # peak drift of the overtaking card (27 px at 90 px rows)
# the overtaking card's glow: r{i}.row_glow 1..3 (peak mid-slide) -> a white veil over the card
GLOW = ["#1EFFFFFF", "#3CFFFFFF", "#5AFFFFFF"]
# The module toggles rk.flip whenever a slide starts; a 1x1 px anim group restarts on it and runs
# one slide duration, so the runtime publishes the page every tick (60 Hz) during slides instead
# of every 2nd tick.
SWAP_60HZ = True
THEME_MS = 320                    # theme cross-fade (page fade)
BTN_Y, BTN_H = 922, 146           # the button row
HOLD_MS = 600                     # press-and-hold time for the theme / row-format gestures
TOP = 12                          # content area top
CONTENT_B = BTN_Y - 12            # content area bottom (910)
CHECK_PX = 128                    # background checker tile: tc_MtCheck_64x64 (2x2) drawn at 2x
CELL = CHECK_PX // 2              # one checker square (the horn's margin unit)
Y0 = -RH                          # row widgets: rect y = Y0 + offset, then + rank * RH
ROW_IX = PX + 6                   # row icon x
ROW_BX = ROW_IX + ICON + 2        # first item bubble x
ROW_BY = Y0 + (RH - 3 - BUB) // 2
# The course card: the course picture at 2x (288x162 visible part of the 304x256 texture;
# crop measured on the decoded picture), the text column right of the cup emblem.
PICT_W, PICT_H = 576, 324
PICT_UV = [8 / 304, 29 / 256, 296 / 304, 191 / 256]
TEXT_DX = 144                     # text column x inside the picture's width
# The course name column: scale 5, wrapped at COURSE_WRAP px (2 lines max). The runtime lays
# text out at advance * 5 * scale / cap px per glyph, cap = the font's cap height it measured
# from the atlas ("cap height 51" in the eden log for turbo_MARIOFont), and wraps only when the
# whole name is wider than the column: 1 line <=> int(units * 25 / 51) <= COURSE_WRAP.
COURSE_WRAP = PICT_W - TEXT_DX
FONT_CAP = 51
NAME2_UNITS = ((COURSE_WRAP + 1) * FONT_CAP - 1) // 25   # largest width (units) still on 1 line
NAME_PITCH = 5 * 5 + 5 * 3                                  # runtime line pitch at scale 5


def area(fmt):
    return PX + FMT[fmt]["plate"] + 12, W - 12       # content / button area x range


# ---- widget helpers -----------------------------------------------------------------------------
def rect(x, y, w, h, bg, color=0, **kw):
    d = {"type": "rect", "rect": [int(x), int(y), int(w), int(h)], "bg": bg, "color": color}
    d.update(kw)
    return d


def disc(x, y, w, h, bg, color=None, **kw):
    """Filled capsule / disc (the button widget's pill, 3 px frame in `color`, no caption)."""
    d = {"type": "button", "rect": [int(x), int(y), int(w), int(h)], "bg": bg,
         "color": color if color is not None else bg, "pill": True, "text": ""}
    d.update(kw)
    return d


def cdisc(cx, cy, d, bg, color=None, **kw):
    return disc(cx - d / 2, cy - d / 2, d, d, bg, color, **kw)


def label(x, y, text=None, scale=5, color="#FFFFFFFF", align=None, **kw):
    d = {"type": "label", "rect": [int(x), int(y), 0, 0], "text_scale": scale, "color": color}
    if text is not None:
        d["text"] = text
    if align:
        d["align"] = align
    d.update(kw)
    return d


def value(x, y, bind, scale=5, color="#FFFFFFFF", align=None, **kw):
    d = {"type": "value", "rect": [int(x), int(y), 0, 0], "bind": bind, "text_scale": scale,
         "color": color}
    if align:
        d["align"] = align
    d.update(kw)
    return d


def image(x, y, w, h, color="#FFFFFFFF", **kw):
    d = {"type": "image", "rect": [int(x), int(y), int(w), int(h)], "color": color}
    d.update(kw)
    return d


def rrect(x, y, w, h, r, bg, **kw):
    """Rounded rectangle: two crossing rects + four corner discs."""
    d = 2 * r
    return [rect(x + r, y, w - d, h, bg, **kw), rect(x, y + r, r, h - d, bg, **kw),
            rect(x + w - r, y + r, r, h - d, bg, **kw),
            disc(x, y, d, d, bg, **kw), disc(x + w - d, y, d, d, bg, **kw),
            disc(x, y + h - d, d, d, bg, **kw), disc(x + w - d, y + h - d, d, d, bg, **kw)]


def halves(x, y, d, src, color, **kw):
    """A round piece of game art stored as its left half (the layout mirrors it)."""
    h = int(d) // 2
    return [image(x, y, h, d, color, src=src, **kw),
            image(x + h, y, int(d) - h, d, color, src=src, flip_x=True, **kw)]


def centered(box_y, box_h, scale):
    """Top y of a label of text_scale `scale` centred in a box (cap height 5 * scale)."""
    return box_y + (box_h - 5 * scale) // 2


def gated(ws, **kw):
    """The same keys (a gate, an anim group, x/y binds) on every widget of a list."""
    return [dict(w, **kw) for w in ws]


def lerp_col(a, b, k):
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5, 7)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5, 7)]
    return "#" + "".join(f"{round(x + (y - x) * k):02X}" for x, y in zip(ca, cb))


def row(w, need="ui.r{i}", **extra):
    """A per-racer card widget, placed at the racer's displayed row position (the module's
    r{i}.row_y: row top = (row_y - 1) * RH; = the rank once the card has landed) and sideways
    drift (r{i}.row_dx)."""
    w = dict(w)
    w.update({"y_bind": "r{i}.row_y", "y_scale": RH, "x_bind": "r{i}.row_dx", "x_scale": RH,
              "repeat": N_RACERS, "need_bind": need})
    w.update(extra)
    return w


ME_ROW_POS = {"y_bind": "me.rank", "y_scale": RH, "need_bind": "ui.me"}   # the local number cell
# The local player's card is drawn once more on top of all cards: the card template with every
# r{i}.* value replaced by the module's mc.* copy of the local racer's own values (glue
# PublishLocalCard) and every gate by its local form (derived "L.*" below), one widget each.
LOCAL_GATES = {"ui.r{i}": "ui.me", "ui.ih0_{i}": "L.ih0", "ui.ir0_{i}": "L.ir0",
               "ui.ib1_{i}": "L.ib1", "ui.ih1_{i}": "L.ih1", "ui.ir1_{i}": "L.ir1",
               "ui.fin{i}": "L.fin"}
LOCAL_GATES.update({f"gl{k}_{{i}}": f"L.gl{k}" for k in (1, 2, 3)})


def const(name, v):
    return {"name": name, "terms": [], "add": v}


# ---- derived values: ALL gating lives here ------------------------------------------------------
derived = [
    const("c.zero", 0),
    # version: mkc.supported / mkc.build_ver come from the per-build data file (BUILDS)
    {"name": "ui.badver", "cmp": "eq", "a": "mkc.supported", "b": 0},
    {"name": "ui.okver", "cmp": "ne", "a": "mkc.supported", "b": 0},
]
# the detected-version label exists ONLY on the wrong-version screen, so only for the builds the
# module does not serve and for an unknown build (build_ver 0)
BADVER_BUILDS = [(ver, text) for _, ok, ver, text, _ in BUILDS if not ok] + [(0, "unknown build")]
derived += [{"name": f"bvx.{ver}", "cmp": "eq", "a": "mkc.build_ver", "b": ver}
            for ver, _ in BADVER_BUILDS]
derived += [{"name": f"bv.{ver}", "all_nonzero": ["ui.badver", f"bvx.{ver}"]}
            for ver, _ in BADVER_BUILDS]
derived += [
    # race phase windows (PHASE_* above; the bounds come from the data file)
    {"name": "ph.ge", "cmp": "ge", "a": "mk.phase", "b": "mkc.phase_race_min"},
    {"name": "ph.le", "cmp": "le", "a": "mk.phase", "b": "mkc.phase_race_max"},
    {"name": "mk.course_ok", "cmp": "ge", "a": "mk.course", "b": 0},
    {"name": "ph.race", "all_nonzero": ["ph.ge", "ph.le", "mk.course_ok"]},
    {"name": "ph.lge", "cmp": "ge", "a": "mk.phase", "b": 0},
    {"name": "ph.lle", "cmp": "le", "a": "mk.phase", "b": "mkc.phase_load_max"},
    {"name": "ph.load", "all_nonzero": ["ph.lge", "ph.lle"]},
    # ui.race / ui.load read a real 0 while mk.ready = 0 (published from boot), so the page
    # binds arm on "not racing" and see the edge into a race
    {"name": "ui.race", "select": "mk.ready", "then": "ph.race", "else": "c.zero"},
    {"name": "ui.load", "select": "mk.ready", "then": "ph.load", "else": "c.zero"},
    {"name": "ui.me", "all_nonzero": ["ui.race", "me.ready"]},
    {"name": "ui.course", "all_nonzero": ["mk.ready", "mk.course_ok"]},
    # 0.4.1: a course card only with its core data: the picture decoded (module glue
    # mk.pict_ok) and a course name (mk.name_ok); the cup emblem only when decoded (mk.cup_ok)
    {"name": "ui.card", "all_nonzero": ["ui.okver", "ui.course", "mk.pict_ok", "mk.name_ok"]},
    {"name": "ui.cupok", "all_nonzero": ["ui.card", "mk.cup_ok"]},
    {"name": "ui.cardcc", "all_nonzero": ["ui.card", "ui.cc"]},
    # 0.4.2: the course name's line count (module glue mk.course_name_w = the name's width in
    # font units, the page font's CWDH advances). The runtime wraps only a name wider than the
    # column, so 2 lines <=> width > NAME2_UNITS; the cup / class lines follow the name.
    {"name": "nm.two", "cmp": "gt", "a": "mk.course_name_w", "b": NAME2_UNITS},
    {"name": "nm.one", "cmp": "le", "a": "mk.course_name_w", "b": NAME2_UNITS},
    {"name": "ui.card1", "all_nonzero": ["ui.card", "nm.one"]},
    {"name": "ui.card2", "all_nonzero": ["ui.card", "nm.two"]},
    {"name": "ui.cup1", "all_nonzero": ["ui.cupok", "nm.one"]},
    {"name": "ui.cup2", "all_nonzero": ["ui.cupok", "nm.two"]},
    {"name": "ui.cc1", "all_nonzero": ["ui.cardcc", "nm.one"]},
    {"name": "ui.cc2", "all_nonzero": ["ui.cardcc", "nm.two"]},
    {"name": "ui.cc", "all_nonzero": ["mk.ready", "mk.cc"]},
    {"name": "me.has0", "cmp": "ge", "a": "me.item0", "b": 0},
    {"name": "me.has1", "cmp": "ge", "a": "me.item1", "b": 0},
    {"name": "ui.item0", "all_nonzero": ["ui.me", "me.has0"]},
    {"name": "ui.item1", "all_nonzero": ["ui.me", "me.has1"]},
    {"name": "ui.roulette", "all_nonzero": ["ui.me", "me.item_roulette"]},
    # horn: MK8D maps no button to the horn (verified live against the game); the module's guarded direct-write
    # action mk.horn_write is enabled while it publishes mk.horn_ok = 1 (the action's
    # enabled_bind and the horn's grey overlay read mk.horn_ok directly)
    # 0.4.2: a map image is used only once the module has decoded it (glue mk.fit_img /
    # mk.map_img, like mk.pict_ok): the fit crop when it decodes, else the whole map, else no
    # map tile at all (never an empty tile)
    {"name": "ui.fitok", "all_nonzero": ["mk.mapfit_ok", "mk.fit_img"]},
    {"name": "ui.fit", "all_nonzero": ["ui.race", "ui.fitok"]},
    {"name": "ui.mapraw", "all_nonzero": ["ui.race", "mk.map_img"]},
    # the page: flags ui.map (mode), ui.dark (theme), ui.long (row format)
    # the row format is NOT a page: both formats live on each race page as anim groups
    # (fmt_long / fmt_short / c_long / c_short), so switching animates instead of cutting
    {"name": "pg.race", "terms": [["@flag:ui.map", 1], ["@flag:ui.dark", 2]], "add": 1},  # 1..4
    {"name": "pg.pre", "terms": [["@flag:ui.dark", 1]], "add": 10},           # 10, 11
    {"name": "pg.load", "terms": [["@flag:ui.dark", 1]], "add": 30},          # 30, 31
    {"name": "pg.nr", "select": "ui.load", "then": "pg.load", "else": "pg.pre"},
    {"name": "pg.norm", "select": "ui.race", "then": "pg.race", "else": "pg.nr"},
    {"name": "ui.pg", "select": "ui.badver", "then": "pg.pre", "else": "pg.norm"},
    # "Race in progress - show" (companion started mid-race): one pill per mode x format
    {"name": "fl.m0", "cmp": "eq", "a": "@flag:ui.map", "b": 0},
    {"name": "fl.m1", "cmp": "ne", "a": "@flag:ui.map", "b": 0},
    {"name": "fl.l0", "cmp": "eq", "a": "@flag:ui.long", "b": 0},
    {"name": "fl.l1", "cmp": "ne", "a": "@flag:ui.long", "b": 0},
    {"name": "go.horn", "all_nonzero": ["ui.race", "fl.m0"]},
    {"name": "go.map", "all_nonzero": ["ui.race", "fl.m1"]},
    # roulette animation index (cosmetic): the race timer in RL_STEP_MS steps modulo the icon count
    {"name": "rl.a", "terms": [["mk.time_ms", 1.0 / RL_STEP_MS]], "add": 1e-6, "floor": True},
    {"name": "rl.b", "terms": [["rl.a", 1.0 / N_RL]], "add": 1e-6, "floor": True},
    {"name": "rl.i", "terms": [["rl.a", 1.0], ["rl.b", -float(N_RL)]]},
]
derived += [const(f"c.n{k}", k + 1) for k in range(N_RACERS)]     # the rank numerals 1..12
for i in range(N_RACERS):
    derived += [
        {"name": f"ui.r{i}", "all_nonzero": ["ui.race", f"r{i}.valid"]},
        {"name": f"ui.me{i}", "all_nonzero": [f"ui.r{i}", f"r{i}.is_me"]},
        {"name": f"st.nme{i}", "cmp": "eq", "a": f"r{i}.is_me", "b": 0},
        {"name": f"ui.o{i}", "all_nonzero": [f"ui.r{i}", f"st.nme{i}"]},
        {"name": f"ui.fin{i}", "all_nonzero": [f"ui.r{i}", f"r{i}.finished"]},
    ] + [
        # card-swap glow level of the overtaking card (module r{i}.row_glow, 0..3)
        {"name": f"gl{k}_{i}", "cmp": "eq", "a": f"r{i}.row_glow", "b": k} for k in (1, 2, 3)
    ] + [
        # LONG names: one label per scale, the module's r{i}.name_scale picks it
        {"name": f"ns{k}_{i}", "cmp": "eq", "a": f"r{i}.name_scale", "b": k} for k in NAME_SCALES
    ] + [
        {"name": f"ui.no{k}_{i}", "all_nonzero": [f"ui.o{i}", f"ns{k}_{i}"]} for k in NAME_SCALES
    ] + [
        # item slot 0: r{i}.item_state 0 empty, 1 roulette, 2 held (mk8d_reader.cpp)
        {"name": f"st.h0_{i}", "cmp": "eq", "a": f"r{i}.item_state", "b": 2},
        {"name": f"st.r0_{i}", "cmp": "eq", "a": f"r{i}.item_state", "b": 1},
        {"name": f"ui.ih0_{i}", "all_nonzero": [f"ui.r{i}", f"st.h0_{i}"]},
        {"name": f"ui.ir0_{i}", "all_nonzero": [f"ui.r{i}", f"st.r0_{i}"]},
        {"name": f"st.b1_{i}", "cmp": "ge", "a": f"r{i}.item1_state", "b": 1},
        {"name": f"st.h1_{i}", "cmp": "eq", "a": f"r{i}.item1_state", "b": 2},
        {"name": f"st.r1_{i}", "cmp": "eq", "a": f"r{i}.item1_state", "b": 1},
        {"name": f"ui.ib1_{i}", "all_nonzero": [f"ui.r{i}", f"st.b1_{i}"]},
        {"name": f"ui.ih1_{i}", "all_nonzero": [f"ui.r{i}", f"st.h1_{i}"]},
        {"name": f"ui.ir1_{i}", "all_nonzero": [f"ui.r{i}", f"st.r1_{i}"]},
        # map markers: zoomed (mapfit) and the whole-map fallback
        {"name": f"ui.m{i}", "all_nonzero": [f"ui.r{i}", f"r{i}.map_ok"]},
        {"name": f"ui.mme{i}", "all_nonzero": [f"ui.m{i}", f"r{i}.is_me"]},
        {"name": f"ui.mo{i}", "all_nonzero": [f"ui.m{i}", f"st.nme{i}"]},
        {"name": f"st.lead{i}", "cmp": "eq", "a": f"r{i}.rank", "b": 1},
        {"name": f"ui.lead{i}", "all_nonzero": [f"ui.m{i}", f"st.lead{i}"]},
        {"name": f"ui.mf{i}", "all_nonzero": [f"ui.m{i}", "ui.fitok"]},
        {"name": f"ui.mfo{i}", "all_nonzero": [f"ui.mf{i}", f"st.nme{i}"]},
        {"name": f"ui.mfme{i}", "all_nonzero": [f"ui.mf{i}", f"r{i}.is_me"]},
        {"name": f"ui.mfl{i}", "all_nonzero": [f"ui.mf{i}", f"st.lead{i}"]},
        # the leader when it is not the local player (redrawn above the local marker)
        {"name": f"ui.mflo{i}", "all_nonzero": [f"ui.mfl{i}", f"st.nme{i}"]},
        {"name": f"ui.lo{i}", "all_nonzero": [f"ui.lead{i}", f"st.nme{i}"]},
    ]


# the local player's card on top (mc.* = the local racer's values, module glue PublishLocalCard)
derived += [
    {"name": "st.mh0", "cmp": "eq", "a": "mc.item_state", "b": 2},
    {"name": "st.mr0", "cmp": "eq", "a": "mc.item_state", "b": 1},
    {"name": "st.mb1", "cmp": "ge", "a": "mc.item1_state", "b": 1},
    {"name": "st.mh1", "cmp": "eq", "a": "mc.item1_state", "b": 2},
    {"name": "st.mr1", "cmp": "eq", "a": "mc.item1_state", "b": 1},
    {"name": "L.ih0", "all_nonzero": ["ui.me", "st.mh0"]},
    {"name": "L.ir0", "all_nonzero": ["ui.me", "st.mr0"]},
    {"name": "L.ib1", "all_nonzero": ["ui.me", "st.mb1"]},
    {"name": "L.ih1", "all_nonzero": ["ui.me", "st.mh1"]},
    {"name": "L.ir1", "all_nonzero": ["ui.me", "st.mr1"]},
    {"name": "L.fin", "all_nonzero": ["ui.me", "mc.finished"]},
] + [
    {"name": f"st.mg{k}", "cmp": "eq", "a": "mc.row_glow", "b": k} for k in (1, 2, 3)
] + [
    {"name": f"L.gl{k}", "all_nonzero": ["ui.me", f"st.mg{k}"]} for k in (1, 2, 3)
] + [
    {"name": f"st.mn{k}", "cmp": "eq", "a": "mc.name_scale", "b": k} for k in NAME_SCALES
] + [
    {"name": f"L.nm{k}", "all_nonzero": ["ui.me", f"st.mn{k}"]} for k in NAME_SCALES
]


# ---- page pieces: backgrounds -------------------------------------------------------------------
def load_background(t):
    """The game's loading-screen background: the sticker pattern tiled 1:1 (every other row
    shifted by half a tile) over the base colour, and the kart / MARIOKART band near the bottom
    (static: the runtime has no cheap continuous scroll)."""
    tw, th = LOAD_PTN_W, LOAD_PTN_H
    ws = [rect(0, 0, W, H, t["load_base"])]
    for row0, dx in ((0, 0), (1, -tw // 2)):
        rows = [r for r in range(0, (H + th - 1) // th) if r % 2 == row0]
        if not rows:
            continue
        ws.append(image(dx, row0 * th, tw, th, t["load_ptn"], src=LOAD_PTN, repeat=4 * len(rows),
                        repeat_cols=4, repeat_dx=tw, repeat_row_dy=2 * th))
    band_y = H - 60                        # the band's centre line (60 px above the bottom)
    cx0 = W // 2 + LOAD_GROUP_X            # N_Loop_00 at the canvas centre
    for tex, px, w, h in LOAD_BAND:
        x = cx0 + px - w // 2
        if x + w < 0 or x > W:
            continue
        ws.append(image(x, band_y - h // 2, w, h, t["load_band"], src=LOAD_LYT + tex))
    for ch, px, w in LOAD_LOGO:
        x = cx0 + px - w // 2
        ws.append(image(x, band_y - 12, w, 24, t["load_band"], src=LOAD_LYT + f"ym_LoadLogo_{ch}^s"))
    return ws


def background(t):
    """Race pages: theme colour + the menu checker tiled over the whole canvas."""
    cols, rows = -(-W // CHECK_PX), -(-H // CHECK_PX)      # 10 x 9
    return [rect(0, 0, W, H, t["bg"]),
            image(0, 0, CHECK_PX, CHECK_PX, t["check"], src=CHECKER, repeat=cols * rows,
                  repeat_cols=cols, repeat_dx=CHECK_PX, repeat_row_dy=CHECK_PX)]


# ---- page pieces: the rank table ------------------------------------------------------------
def rank_bar(t, plate_w):
    """The glass rank table: a continuous bevelled steel/glass number column (per-cell gradient,
    glass highlight, bright left / dark right edge, separators), the empty row slots (the cards
    slide over them), the local player's yellow number cell (placed by me.rank: exact, it does not
    animate), numerals 1..12."""
    rep = {"repeat": N_RACERS, "repeat_dy": RH}
    ws = [rect(PX, 0, plate_w, RH - 3, t["plate"], **rep),
          rect(PX, RH - 5, plate_w, 2, t["plate_sh"], **rep),
          rect(PX, 0, plate_w, 2, t["plate_hi"], **rep),
          rect(0, 0, NW, H, t["bar_base"]),
          image(0, 0, NW, RH, t["bar_tint"], src=GRAD_V, src_rect=[0, 0.02, 1, 0.42], **rep)]
    me = ME_ROW_POS
    ws += [image(0, 2, NW, RH // 2, t["bar_gloss"], src=GLOSS, **rep),
           image(0, Y0, NW, RH, ME_YELLOW, src=GRAD_V, src_rect=[0, 0.0, 1, 0.32], **me),
           image(0, Y0 + 2, NW, RH // 2, "#80FFFFFF", src=GLOSS, **me),
           rect(0, 0, NW, 1, t["bar_hi"], **rep),
           rect(0, RH - 2, NW, 2, t["bar_sep"], **rep),
           image(0, 0, 26, H, t["bar_left"], src=GRAD_H, flip_x=True),
           rect(0, 0, 3, H, t["bar_left"]),
           image(NW - 22, 0, 22, H, "#40000000", src=GRAD_H),
           rect(NW - 3, 0, 3, H, t["bar_right"])]
    # numerals: an outline ring (dark; light theme adds a white rim) under the fill
    ny = centered(0, RH - 3, NUM_SCALE)
    ring = ((-1, 0), (1, 0), (0, -1), (0, 1), (-0.7, -0.7), (0.7, -0.7), (-0.7, 0.7), (0.7, 0.7))
    layers = [(4 if t["num_rim"] else 3, t["num_outer"])]
    if t["num_rim"]:
        layers.append((2, t["num_rim"]))
    for r, col in layers:
        for dx, dy in ring:
            ws.append(value(NW // 2 + round(dx * r), ny + round(dy * r), "c.n{i}",
                            scale=NUM_SCALE, color=col, align="center", **rep))
    ws.append(value(NW // 2, ny, "c.n{i}", scale=NUM_SCALE, color=t["num_fill"],
                    align="center", **rep))
    ws.append(rect(NW, 0, 4, H, BAR_EDGE))
    return ws


def card_plate(t, x, w, local):
    """A card's plate from x to x + w (unplaced, row top Y0): the theme plate, or the local
    player's yellow plate with its gloss."""
    if local:
        return [rect(x, Y0, w, RH - 3, ME_ROW),
                image(x, Y0, w, (RH - 3) // 2, "#70FFFFFF", src=GLOSS),
                rect(x, Y0, w, 2, "#FFFFF6B0"),
                rect(x, Y0 + RH - 5, w, 2, ME_ROW_SH)]
    return [rect(x, Y0, w, RH - 3, t["plate"]), rect(x, Y0 + RH - 5, w, 2, t["plate_sh"]),
            rect(x, Y0, w, 2, t["plate_hi"])]


def plate_ext(t, x0, x1):
    """The LONG format's empty row slots extension (x0..x1)."""
    w = x1 - x0
    rep = {"repeat": N_RACERS, "repeat_dy": RH}
    return [rect(x0, 0, w, RH - 3, t["plate"], **rep),
            rect(x0, RH - 5, w, 2, t["plate_sh"], **rep),
            rect(x0, 0, w, 2, t["plate_hi"], **rep)]


def bubble(x, y, d, t, held, roul, key, shown="ui.r{i}"):
    """A row's item bubble (the game's item-slot disc + ring halves): held item, or the roulette
    cycling with a yellow ring. Hidden once the racer has finished (the flag replaces it)."""
    fin = {"hide_bind": "r{i}.finished", "hide_eq": 1}
    ws = [row(w, shown, **fin) for w in halves(x, y, d, SLOT_BASE, t["bubble"])]
    ws += [row(w, shown, **fin) for w in halves(x, y, d, SLOT_FRAME, t["bubble_rim"])]
    inner = int(d * 0.80)
    o = (d - inner) / 2
    ws.append(row(image(x + o, y + o, inner, inner, src_bind=key), held, **fin))
    ws.append(row(image(x + o, y + o, inner, inner, bind="rl.i", src=RL_SRC[0],
                        src_names=RL_SRC), roul, **fin))
    ws += [row(w, roul, **fin) for w in halves(x, y, d, SLOT_FRAME, ME_YELLOW)]
    return ws


def rows_common(t):
    """Both formats: icon, the first item bubble, the finish flag."""
    ws = [row(image(ROW_IX, Y0 + (RH - 3 - ICON) // 2, ICON, ICON, src_bind="r{i}.icon"))]
    ws += bubble(ROW_BX, ROW_BY, BUB, t, "ui.ih0_{i}", "ui.ir0_{i}", "r{i}.item0_key")
    # finished: the race HUD's checkered flag (ym_LapFlag_00, 64x64 drawn 1:1) in the item spot,
    # like the flag column of the Wii U GamePad rows
    fy = Y0 + (RH - 3 - 64) // 2
    ws.append(row(image(ROW_BX + (BUB - 64) // 2, fy, 64, 64, src=FLAG), "ui.fin{i}"))
    return ws


def rows_short(t):
    """SHORT only: the second item bubble at full size."""
    return bubble(ROW_BX + BUB + 4, ROW_BY, BUB, t, "ui.ih1_{i}", "ui.ir1_{i}",
                  "r{i}.item1_key", shown="ui.ib1_{i}")


def rows_long(t, local):
    """LONG only: a small second bubble over the first one's corner and the full name, at the
    largest scale that keeps it within the "Rosalina" column (r{i}.name_scale)."""
    ws = bubble(ROW_BX + BUB - BUB1 + 8, ROW_BY + BUB - BUB1 + 4, BUB1, t, "ui.ih1_{i}",
                "ui.ir1_{i}", "r{i}.item1_key", shown="ui.ib1_{i}")
    nx = ROW_BX + BUB + 12
    for k in NAME_SCALES:
        ny = Y0 + centered(0, RH - 3, k)
        if local:
            ws.append(row(label(nx, ny, scale=k, color=ME_TEXT, bind_text="r{i}.name"),
                          f"L.nm{k}"))
        else:
            ws.append(row(label(nx, ny, scale=k, color=t["text"], bind_text="r{i}.name"),
                          f"ui.no{k}_{{i}}"))
    return ws


def glow(x, w):
    """The overtaking card's glow (module r{i}.row_glow 1..3): a white veil over the card."""
    return [row(rect(x, Y0, w, RH - 3, GLOW[k - 1]), f"gl{k}_{{i}}") for k in (1, 2, 3)]


def card_templates(t, local, g_short, g_long):
    """One racer's card as repeat templates, in draw order: plate (+ LONG extension), icon, item
    bubbles / finish flag, LONG names, the glow. local = the local player's single copy (yellow
    plate, dark name; r{i}.* -> mc.*, gates through LOCAL_GATES, no repeat)."""
    short_w = FMT["short"]["plate"]
    ext_x, ext_w = PX + short_w, FMT["long"]["plate"] - short_w
    ws = [row(w) for w in card_plate(t, PX, short_w, local)]
    ws += gated([row(w) for w in card_plate(t, ext_x, ext_w, local)], anim=g_long)
    ws += rows_common(t)
    ws += gated(rows_short(t), anim=g_short)
    ws += gated(rows_long(t, local), anim=g_long)
    ws += glow(PX, short_w)
    ws += gated(glow(ext_x, ext_w), anim=g_long)
    if local:
        ws = [dict(w, need_bind=LOCAL_GATES.get(w["need_bind"], w["need_bind"])) for w in ws]
        ws = [to_local(w) for w in ws]
        for w in ws:
            assert w["need_bind"] in LOCAL_GATES.values() or w["need_bind"].startswith("L.nm"), w
            assert "{i}" not in json.dumps(w), w
    return ws


def to_local(o):
    """A card template as the local player's single card: r{i}.<field> -> mc.<field>."""
    if isinstance(o, dict):
        return {k: to_local(v) for k, v in o.items() if k != "repeat"}
    if isinstance(o, list):
        return [to_local(v) for v in o]
    return o.replace("r{i}.", "mc.") if isinstance(o, str) else o


def cards(t, g_short, g_long):
    """The rank table's cards: every racer's card as repeat templates (layer by layer: all plates,
    then all icons, ...), then the local player's single card on top of all of them. Two crossing
    non-local cards are told apart by the overtaking card's drift and glow.
    Why not card by card (each racer's widgets emitted as their own 12 widget groups, which would
    let one card cover another whole): measured live, 12 separate copies of the item widgets made
    the runtime's dirty scan fall back to full-page redraws while many racers' items changed
    (60-85 full redraws per 5 s for ~10 s at the item boxes, vs 3-5 with templates), a runtime
    cost the one-frame z-order gain does not justify."""
    base = card_templates(t, False, g_short, g_long)
    return base + card_templates(t, True, g_short, g_long)


# ---- page pieces: horn, glyphs, buttons, map -------------------------------------------------
def horn_art(cx, cy, d, tap, grey=False):
    """The horn button (procedural: the game has no GamePad horn art): drop shadow, steel bezel
    (offset discs light upper-left / dark lower-right), dark inner rim, orange -> red face, the
    local driver's kart emblem at 70 % of the face, a soft highlight over the top third."""
    kw = {"on_tap": tap}
    u = d / 100.0
    ws = [cdisc(cx + 0.8 * u, cy + 1.8 * u, d + 2 * u, "#40000000"),
          cdisc(cx, cy, d, "#FF2C2E33", **kw)]
    for k, col in enumerate(["#FF6E747D", "#FF8C929B", "#FFB4B9C0", "#FFD8DCE1", "#FFF2F4F6"]):
        off = (2 - k) * 0.55 * u
        ws.append(cdisc(cx + off, cy + off * 1.3, d - 2 * u - k * 1.2 * u, col, **kw))
    ws.append(cdisc(cx + 0.5 * u, cy + 0.7 * u, d - 10 * u, "#FF9AA0A8", **kw))
    ws.append(cdisc(cx, cy, d - 12.5 * u, "#FFC9CDD3", **kw))
    ws.append(cdisc(cx, cy, d - 15 * u, "#FF1E2024", **kw))
    f = d - 17 * u
    steps = 22 if d > 200 else 8
    for k in range(steps):
        kk = k / (steps - 1)
        ws.append(cdisc(cx, cy - kk * 0.06 * f, f * (1 - 0.62 * kk),
                        lerp_col("#FFC40C0A", "#FFFF9A2A", kk ** 1.3), **kw))
    e = f * 0.70
    ws.append(image(cx - e * 0.4, cy - e * 0.4, e * 0.8, e * 0.8, src_bind="r{i}.icon",
                    need_bind="ui.me{i}", repeat=N_RACERS, **kw))
    ws.append(image(cx - e / 2, cy - e / 2, e, e, src_bind="me.emblem_key", need_bind="ui.me",
                    **kw))
    for wk, hk, yk, a in ((0.86, 0.36, 0.035, "#1CFFFFFF"), (0.78, 0.30, 0.05, "#20FFFFFF"),
                          (0.68, 0.24, 0.065, "#26FFFFFF"), (0.56, 0.16, 0.08, "#2CFFFFFF")):
        ws.append(disc(cx - f * wk / 2, cy - f / 2 + f * yk, f * wk, f * hk, a, **kw))
    if grey:
        # disabled look while the module's horn action is unavailable (mk.horn_ok != 1)
        ws.append(cdisc(cx, cy, f, "#7898A0A8", hide_bind="mk.horn_ok", hide_eq=1))
        # pressed look while the horn sounds (mk.horn_busy): the face darkens and sinks a little
        press = {"anim": {"bind": "mk.horn_busy", "group": f"horn_press_{int(cx)}",
                          "from": "fade", "ms": 90,
                          "box": [int(cx - d / 2), int(cy - d / 2), int(d), int(d)]}}
        ws += [cdisc(cx, cy + 0.6 * u, f, "#38000000", **press),
               cdisc(cx, cy + 0.6 * u, f * 0.92, "#20000000", **press)]
    return ws


def horn_glyph(cx, cy, s, tap=None):
    """Horn glyph (procedural; MK8D's UI archives have no horn icon besides the Super Horn
    item): mouthpiece, tube, stepped bell, yellow with a dark outline. s = width."""
    kw = {"on_tap": tap} if tap else {}
    parts = [("d", 0.00, 0.40, 0.16, 0.20), ("r", 0.10, 0.45, 0.44, 0.10)]
    for k in range(5):
        h = 0.16 + 0.11 * k
        parts.append(("r", 0.50 + 0.07 * k, 0.5 - h / 2, 0.08, h))
    parts.append(("d", 0.84, 0.08, 0.14, 0.84))
    x0, y0 = cx - s / 2, cy - s * 0.5
    ws = []
    for pad, col in ((0.035 * s, "#FF3A2600"), (0, "#FFFFD21F")):
        for kind, px, py, pw, ph in parts:
            x, y, w, h = x0 + px * s - pad, y0 + py * s - pad, pw * s + 2 * pad, ph * s + 2 * pad
            ws.append(disc(x, y, w, h, col, **kw) if kind == "d" else rect(x, y, w, h, col, **kw))
    ws.append(disc(x0 + 0.86 * s, y0 + 0.16 * s, 0.05 * s, 0.30 * s, "#80FFFFFF", **kw))
    return ws


def theme_glyph(cx, cy, dark, tap, s=1.0, bg="#FF5A5F67"):
    """Moon (light theme: tap -> dark) or sun (dark theme: tap -> light), from discs."""
    if not dark:
        return [cdisc(cx, cy, 44 * s, "#FFFFD21F", on_tap=tap),
                cdisc(cx + 12 * s, cy - 9 * s, 38 * s, bg, on_tap=tap)]
    ws = [cdisc(cx, cy, 30 * s, "#FFFFC928", on_tap=tap)]
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0), (-.7, -.7), (.7, -.7), (-.7, .7), (.7, .7)):
        ws.append(cdisc(cx + dx * 24 * s, cy + dy * 24 * s, 8 * s, "#FFFFC928", on_tap=tap))
    return ws


def button_frame(x, y, w, h, fill, t, tap):
    return (rrect(x, y, w, h, 18, t["frame"], on_tap=tap) +
            rrect(x + 3, y + 3, w - 6, h - 6, 16, t["bevel"], on_tap=tap) +
            rrect(x + 8, y + 8, w - 16, h - 16, 12, fill, on_tap=tap) +
            [image(x + 8, y + 8, w - 16, (h - 16) // 2, "#50FFFFFF", src=GLOSS, on_tap=tap)])


def buttons(t, mode, fmt):
    """The button row under the horn / map (0.4.0): mode switch and USE ITEM, both wide. The theme
    and the row format are press-and-hold gestures (runtime 13 on_hold)."""
    x0, x1 = area(fmt)
    gap = 12
    wb = (x1 - x0 - gap) // 2
    y, h = BTN_Y, BTN_H
    ws = []
    # mode switch: shows the other mode (the fit map in HORN mode, a small horn in MAP mode)
    xc = x0
    ws += button_frame(xc, y, wb, h, t["btn_blue"] if mode == "horn" else t["btn"], t, "ui.mode")
    m = h - 28
    if mode == "horn":
        ws.append(image(xc + (wb - m) // 2, y + 14, m, m, MAP_BLUE, src_bind="mk.mapfit_key",
                        need_bind="ui.fit", on_tap="ui.mode"))
        ws.append(image(xc + (wb - m) // 2, y + 14, m, m, MAP_BLUE, src_bind="mk.map_key",
                        need_bind="ui.mapraw", hide_bind="ui.fitok", hide_eq=1,
                        on_tap="ui.mode"))
    else:
        ws += horn_art(xc + wb / 2, y + h / 2, m, "ui.mode")
    # USE ITEM (presses L): the local item in the game's slot art + caption
    xd = xc + wb + gap
    ws += button_frame(xd, y, wb, h, t["btn"], t, "mk.use_item")
    bd = h - 34
    bx, by = xd + 22, y + 17
    ws += halves(bx, by, bd, SLOT_BASE, "#FFFFFFFF", on_tap="mk.use_item")
    ws += halves(bx, by, bd, SLOT_FRAME, "#FFB9BEC6", on_tap="mk.use_item")
    inner = int(bd * 0.8)
    io = (bd - inner) / 2
    ws.append(image(bx + io, by + io, inner, inner, src_bind="me.item0_key",
                    need_bind="ui.item0", hide_bind="me.item_roulette", hide_eq=1,
                    on_tap="mk.use_item"))
    ws.append(image(bx + io, by + io, inner, inner, bind="rl.i", src=RL_SRC[0],
                    src_names=RL_SRC, need_bind="ui.roulette", on_tap="mk.use_item"))
    ws += halves(bx, by, bd, SLOT_FRAME, ME_YELLOW, need_bind="ui.roulette")
    s1 = int(bd * 0.42)
    ws.append(cdisc(bx + bd - s1 / 3, by + bd - s1 / 3, s1, "#FFFFFFFF", "#FFB9BEC6",
                    need_bind="ui.item1"))
    ws.append(image(bx + bd - s1 / 3 - s1 * 0.4, by + bd - s1 / 3 - s1 * 0.4, s1 * 0.8,
                    s1 * 0.8, src_bind="me.item1_key", need_bind="ui.item1"))
    tx = bx + bd + (xd + wb - (bx + bd)) // 2
    ws.append(label(tx, y + 34, "USE", scale=6, align="center", color="#FFFFFFFF",
                    on_tap="mk.use_item"))
    ws.append(label(tx, y + 82, "ITEM", scale=6, align="center", color="#FFFFFFFF",
                    on_tap="mk.use_item"))
    return ws


def map_block(t, fmt):
    """The course map in the GamePad blue: the module's mapfit/<course> image (the map cut to its
    drawn extent, square, NEAREST) filling the map area, markers at r{i}.mapf_x/y (same box);
    fallback for a reader without mapfit: the whole map at the same size with r{i}.map_x/y."""
    x0, x1 = area(fmt)
    s = min(x1 - x0, CONTENT_B - TOP)
    mx, my = x0 + (x1 - x0 - s) // 2, TOP + (CONTENT_B - TOP - s) // 2
    mark, me_mark = (64, 72) if s > 700 else (56, 64)
    # The local ring hugs the icon: tc_MapChara content radius = 0.28 x the icon size (measured on
    # the decoded icons), the DRC ring art (P_ColorBG_00, 64 px when mirrored) has its solid band
    # between radius 22 and 28 of 32 -> ring size so that the band starts 3 px outside the face.
    ring = round((me_mark * 0.28 + 3) * 64 / 22)
    # the crown: the DRC map icon's crown half (18x27) at 1.25x, its foot resting on top of the
    # marker (the local ring's outer edge when the leader is the local player)
    cw, ch = 22, 34
    crown_foot = round(ring * 28 / 64) - 3
    # one map tile per format (both formats sit on the same page): the id names the format
    ws = [image(mx, my, s, s, MAP_BLUE, src_bind="mk.mapfit_key", need_bind="ui.fit",
                id=f"mk_map_{fmt}", on_tap="ui.mode"),
          image(mx, my, s, s, MAP_BLUE, src_bind="mk.map_key", need_bind="ui.mapraw",
                hide_bind="ui.fitok", hide_eq=1, on_tap="ui.mode")]
    # z-order: other markers, the local marker (hugging ring under its icon), the leader's own
    # marker again when it is not the local player (a leader under the local player stays
    # visible; the ring still shows around it), the crown on top above the leader.
    variants = (("r{i}.mapf_x", "r{i}.mapf_y", "ui.mfo{i}", "ui.mfme{i}", "ui.mfl{i}",
                 "ui.mflo{i}", {}),
                ("r{i}.map_x", "r{i}.map_y", "ui.mo{i}", "ui.mme{i}", "ui.lead{i}", "ui.lo{i}",
                 {"hide_bind": "ui.fitok", "hide_eq": 1}))
    for xb, yb, g_o, g_me, g_lead, g_lo, hide in variants:
        mk = {"x_bind": xb, "x_scale": s, "y_bind": yb, "y_scale": s, "repeat": N_RACERS}
        mk.update(hide)
        ws.append(image(mx - mark // 2, my - mark // 2, mark, mark, src_bind="r{i}.icon",
                        need_bind=g_o, **mk))
        ws += gated(halves(mx - ring // 2, my - ring // 2, ring, RING_HALF, t["ring"],
                           need_bind=g_me), **mk)
        ws.append(image(mx - me_mark // 2, my - me_mark // 2, me_mark, me_mark,
                        src_bind="r{i}.icon", need_bind=g_me, **mk))
        ws.append(image(mx - mark // 2, my - mark // 2, mark, mark, src_bind="r{i}.icon",
                        need_bind=g_lo, **mk))
        cy = my - crown_foot - ch
        ws += gated([image(mx - cw, cy, cw, ch, src=CROWN_HALF, need_bind=g_lead),
                     image(mx, cy, cw, ch, src=CROWN_HALF, flip_x=True, need_bind=g_lead)], **mk)
    return ws


def horn_block(fmt):
    """The horn: exactly one checker square of margin inside the right panel (table edge ..
    canvas edge, top .. the button row) on every side, counting its drop shadow (1.8 % right /
    down) and the horn tab below it (52 px past the bezel)."""
    px0 = PX + FMT[fmt]["plate"]
    left, right, top, bottom = px0 + CELL, W - CELL, CELL, BTN_Y - CELL
    tab = 52
    d = int(min((right - left) / 1.018, bottom - top - tab))
    cx = (left + right - 0.018 * d) / 2
    cy = top + (bottom - top - (d + tab)) / 2 + d / 2
    ws = horn_art(cx, cy, d, "mk.horn", grey=True)
    ty = cy + d // 2 - 50
    ws += [disc(cx - 100, ty + 6, 200, 96, "#40000000"),
           disc(cx - 100, ty, 200, 96, "#FF1E2024", on_tap="mk.horn"),
           disc(cx - 94, ty + 4, 188, 88, "#FF3E4249", on_tap="mk.horn"),
           disc(cx - 90, ty + 10, 180, 78, "#FF4E535B", on_tap="mk.horn"),
           disc(cx - 80, ty + 8, 160, 30, "#28FFFFFF", on_tap="mk.horn")]
    ws += horn_glyph(cx, ty + 48, 96, tap="mk.horn")
    return ws


def mock_label(x, y, t):
    """"MOCK DATA" while the DEV mock publishes (mk.mock = 1; the real reader never does). Kept
    in release builds on purpose: a mock .so that slipped into a package would show it."""
    return label(x, y, "MOCK DATA", scale=3, color=t["mock"], need_bind="mk.mock")


# ---- pages: race ----------------------------------------------------------------------------
def race_page(mode, theme):
    """One race page per mode x theme; the two row formats are anim groups on it:
    fmt_long (LONG plates + names; grows out of the SHORT table to the right, shrinks back),
    fmt_short (the second full-size bubble; cross-fades), c_short / c_long (the right-hand area of
    each format: horn or map + buttons; cross-fade). Holds: on the rank table -> ui.format, on
    the horn / map area -> ui.theme."""
    t = THEMES[theme]
    short_x = PX + FMT["short"]["plate"]
    long_x = PX + FMT["long"]["plate"]
    ws = background(t)
    if SWAP_60HZ:
        # 60 Hz page publishing while a card slides: an invisible 1x1 px anim group that restarts
        # on every rk.flip toggle (the module toggles it whenever a slide starts or restarts) and
        # runs one slide duration + one tick
        ws.append(rect(0, 0, 1, 1, 0, anim={"bind": "rk.flip", "group": "rk_live", "from": "fade",
                                             "ms": SWAP_NORMAL_MS + 17, "box": [0, 0, 1, 1]}))
    # invisible origin boxes for the widget-anim grow / shrink
    ws += [rect(0, 0, short_x, H, 0, id="tbl_short"), rect(0, 0, long_x, H, 0, id="tbl_long")]
    ws += rank_bar(t, FMT["short"]["plate"])
    g_long = {"bind": "fl.l1", "group": "fmt_long", "from": "widget:tbl_short", "ms": FMT_MS,
              "box": [0, 0, long_x, H]}
    g_short = {"bind": "fl.l0", "group": "fmt_short", "from": "fade", "ms": FMT_MS,
               "box": [PX, 0, FMT["short"]["plate"], H]}
    ws += gated(plate_ext(t, short_x, long_x), anim=g_long)
    ws += cards(t, g_short, g_long)
    for fmt, gate in (("short", "fl.l0"), ("long", "fl.l1")):
        x0, x1 = area(fmt)
        grp = {"bind": gate, "group": f"c_{fmt}", "from": "fade", "ms": FMT_MS,
               "box": [x0, 0, x1 - x0, H]}
        content = horn_block(fmt) if mode == "horn" else map_block(t, fmt)
        content += buttons(t, mode, fmt)
        for w in content:
            # a widget with its own anim (the horn press) keeps it; it gets the format gate
            ws.append(dict(w, need_bind=w.get("need_bind", gate)) if "anim" in w
                      else dict(w, anim=grp))
    # press-and-hold areas (runtime 13): drawn last (on top) but invisible; they take no taps
    for fmt, gate in (("short", "fl.l0"), ("long", "fl.l1")):
        tx = short_x if fmt == "short" else long_x
        ws.append(rect(0, 0, tx, H, 0, need_bind=gate, on_hold="ui.format", hold_ms=HOLD_MS,
                       id=f"hold_rows_{fmt}"))
        ws.append(rect(tx, 0, W - tx, BTN_Y - 6, 0, need_bind=gate, on_hold="ui.theme",
                       hold_ms=HOLD_MS, id=f"hold_theme_{fmt}"))
    ws.append(mock_label(PX + 10, H - 26, t))
    return ws


# ---- pages: cards (waiting, loading, wrong version) -------------------------------------------
def card(x, y, w, h, t, head_col, title, title_col="#FF2A1E00", **gate):
    """MK8-style card: dark steel frame + bevel, body, a glossy coloured title band."""
    ws = rrect(x, y, w, h, 22, t["frame"]) + rrect(x + 4, y + 4, w - 8, h - 8, 19, t["bevel"])
    ws += rrect(x + 10, y + 10, w - 20, h - 20, 14, t["card"])
    ws += rrect(x + 10, y + 10, w - 20, 84, 14, head_col)
    ws += [rect(x + 10, y + 70, w - 20, 24, head_col),
           image(x + 10, y + 10, w - 20, 44, "#90FFFFFF", src=GLOSS),
           rect(x + 10, y + 94, w - 20, 3, "#40000000"),
           label(x + w // 2, y + 10 + centered(0, 84, 7), title, scale=7, color=title_col,
                 align="center")]
    return gated(ws, **gate)


def corner_theme(t, dark):
    """The theme button on the waiting / loading pages (0.4.2: top-right, 16 px margin, keeps the
    kart band clear)."""
    x, y, s = W - 104, 16, 88
    return button_frame(x, y, s, s, t["btn"], t, "ui.theme") + \
        theme_glyph(x + s / 2, y + s / 2, dark, "ui.theme", 0.8, bg=t["btn"])


def course_block(t, cx, y, gate):
    """Course picture (2x, PICT_W x PICT_H) in a steel frame, cup emblem, course name, cup name,
    class. Shown only with `gate` (ui.card: picture decoded + name known); the cup disc + emblem
    only with a decoded emblem (ui.cupok), the class only with a class. No empty slots."""
    px = cx - PICT_W // 2
    tx = px + TEXT_DX
    ws = rrect(px - 10, y - 10, PICT_W + 20, PICT_H + 20, 10, t["frame"])
    ws += [image(px, y, PICT_W, PICT_H, src_bind="mk.pict_key", src_rect=PICT_UV),
           image(px, y, PICT_W, 70, "#40FFFFFF", src=GLOSS),
           label(tx, y + PICT_H + 36, scale=5, color=t["card_text"],
                 bind_text="mk.course_name", wrap_width=COURSE_WRAP, max_lines=2)]
    ws = gated(ws, need_bind=gate)
    # 0.4.2: the cup / class lines sit right under the name (1 line), or one pitch lower for a
    # 2-line name; the emblem centres on the text block.
    for lines, sfx in ((1, "1"), (2, "2")):
        dy = (lines - 1) * NAME_PITCH
        ey = y + PICT_H + 94 + dy // 2
        ws += [label(tx, y + PICT_H + 84 + dy, scale=4, color=t["sub"],
                     bind_text="mk.cup_name", need_bind="ui.card" + sfx),
               cdisc(px + 62, ey, 124, "#FF1E2024", need_bind="ui.cup" + sfx),
               image(px + 12, ey - 50, 100, 100, "#FFFFE9A8", src_bind="mk.cup_key",
                     need_bind="ui.cup" + sfx),
               value(tx, y + PICT_H + 124 + dy, "mk.cc", scale=4, color=t["sub"], suffix="cc",
                     need_bind="ui.cc" + sfx)]
    return ws


def big_flag(t, caption, spin=False):
    """The idle view (0.4.1): the game's finish flag (ym_LapFlag_00, 64x64) with a caption;
    everything else hidden. Shown whenever no course card can be shown (menus, unknown course,
    missing picture / name, a build the module does not serve).
    0.4.2: three flags in a row at integer scales of the 64x64 texture (nearest sampling stays
    crisp): the centre one 3x (192), the sides 2x (128), bottoms aligned, the left one mirrored."""
    cx = W // 2
    g = {"need_bind": "ui.okver", "hide_bind": "ui.card", "hide_eq": 1}
    bottom, gap = 590, 24
    ws = [image(cx - 96, bottom - 192, 192, 192, src=FLAG, **g),
          image(cx - 96 - gap - 128, bottom - 128, 128, 128, src=FLAG, flip_x=True, **g),
          image(cx + 96 + gap, bottom - 128, 128, 128, src=FLAG, **g)]
    if spin:
        for w in ws:
            w["spin"] = 90.0
    ws.append(label(cx, 640, caption, scale=9, color=t["title_on_load"], align="center", **g))
    return ws


def waiting_cards(t):
    cx = W // 2
    ws = card(cx - 380, 110, 760, 700, t, ME_YELLOW, "NEXT RACE", need_bind="ui.card")
    ws += course_block(t, cx, 250, "ui.card")
    ws.append(label(cx, 850, "Waiting for the race...", scale=5, color=t["title_on_load"],
                    align="center", need_bind="ui.card"))
    ws += big_flag(t, "WAITING FOR RACE")
    return ws


def badver_card(t):
    """Wrong game version. The module does not load on another build, so no module: art is
    available here: procedural shapes + the game font only (the font is a romfs: source)."""
    cx = W // 2
    g = {"need_bind": "ui.badver"}
    ws = card(cx - 440, 200, 880, 660, t, BAD_RED, "UNSUPPORTED GAME VERSION",
              title_col="#FFFFFFFF", **g)
    ws += gated([cdisc(cx, 390, 124, "#FF2C2E33"), cdisc(cx, 390, 112, BAD_RED),
                 label(cx, 390 - 30, "!", scale=12, color="#FFFFFFFF", align="center"),
                 label(cx, 480, "Mario Kart 8 Deluxe", scale=7, color=t["card_text"],
                       align="center"),
                 label(cx - 40, 570, "Detected:", scale=5, color=t["dim"], align="right"),
                 label(cx - 40, 630, "Supported:", scale=5, color=t["dim"], align="right"),
                 label(cx, 630, SUPPORTED_TEXT, scale=5, color=t["card_text"]),
                 label(cx, 720, "Install the 4.0.0 update to use the dual screen.", scale=4,
                       color=t["card_text"], align="center"),
                 label(cx, 780, scale=3, color=t["dim"], align="center",
                       bind_text="module_error_message", wrap_width=820, max_lines=2)], **g)
    # the detected build (per-build data file), only on this screen (bv.* include ui.badver)
    for ver, text in BADVER_BUILDS:
        ws.append(label(cx, 570, text, scale=5, color=BAD_RED, need_bind=f"bv.{ver}"))
    return ws


def pre_page(theme):
    t = THEMES[theme]
    dark = theme == "dark"
    ws = load_background(t)
    ws += waiting_cards(t)
    ws += badver_card(t)
    cx = W // 2
    for gate, mode in (("go.horn", "horn"), ("go.map", "map")):
        action = f"go.{mode}.{theme}"
        ws.append(disc(cx - 300, H - 250, 600, 100, "#FF3F4247", ME_YELLOW, need_bind=gate,
                       on_tap=action))
        ws.append(label(cx, H - 250 + centered(0, 100, 6), "Race in progress - show", scale=6,
                        align="center", color="#FFFFFFFF", need_bind=gate, on_tap=action))
    ws += corner_theme(t, dark)
    # discoverability of the race-page holds (unobtrusive, waiting screen only)
    ws.append(label(W // 2, 920, "In a race: hold the horn or map for light / dark,  hold the "
                    "ranking for names", scale=3, color=t["title_on_load"], align="center",
                    need_bind="ui.okver", hide_bind="ui.race", hide_eq=1))
    ws.append(mock_label(20, 20, t))
    return ws


def load_page(theme):
    t = THEMES[theme]
    cx = W // 2
    ws = load_background(t)
    ws += card(cx - 380, 110, 760, 780, t, ME_YELLOW, "LOADING", need_bind="ui.card")
    ws += course_block(t, cx, 250, "ui.card")
    ws.append(image(cx - 40, 770, 80, 80, src=FLAG, spin=240.0, need_bind="ui.card"))
    ws += big_flag(t, "LOADING", spin=True)
    ws += corner_theme(t, theme == "dark")
    ws.append(mock_label(20, 20, t))
    return ws


# (page id, title, widgets, ui.pg value)
PAGES = [("prerace_light", "Waiting", pre_page("light"), 10),
         ("prerace_dark", "Waiting (dark)", pre_page("dark"), 11),
         ("loading_light", "Loading", load_page("light"), 30),
         ("loading_dark", "Loading (dark)", load_page("dark"), 31)]
for theme_i, theme in enumerate(("light", "dark")):
    for mode_i, mode in enumerate(("horn", "map")):
        PAGES.append((f"race_{mode}_{theme}", f"Race: {mode} {theme}", race_page(mode, theme),
                      1 + mode_i + 2 * theme_i))

# ---- actions ------------------------------------------------------------------------------------
# Haptics (runtime 13 strengths, Android AuxPresentation): light = CONTEXT_CLICK tick,
# click = KEYBOARD_TAP, confirm = CONFIRM, heavy = LONG_PRESS. Taps tick; USE ITEM confirms;
# the horn clicks; a fired hold is "heavy" (the runtime's hold haptic, manifest haptics.hold).
actions = {
    "mk.use_item": {"kind": "button", "button": "L", "frames": 6, "enabled_bind": "ui.me",
                    "haptic": "confirm",
                    "_note": "L = the game's 'Use item' button (verified live)"},
    "mk.horn": {"kind": "module", "action": "mk.horn_write", "argument": 0,
                "enabled_bind": "mk.horn_ok", "haptic": "click",
                "_note": "no horn button in 4.0.0 (action mask 8 = 0): the module's "
                         "guarded direct write (mk.horn_ok / mk.horn_busy / mk.horn_count)"},
    "ui.mode": {"kind": "flag", "flag": "ui.map", "haptic": "light"},
    "ui.theme": {"kind": "flag", "flag": "ui.dark",
                 "_note": "press-and-hold on the horn / map panel (on_hold); haptic = hold"},
    "ui.format": {"kind": "flag", "flag": "ui.long",
                  "_note": "press-and-hold on the ranking (on_hold); haptic = hold"},
}
for theme in ("light", "dark"):
    for mode in ("horn", "map"):
        actions[f"go.{mode}.{theme}"] = {
            "kind": "page", "page": f"race_{mode}_{theme}", "transition": "fade",
            "duration_ms": 200}

# every page switch cross-fades (theme: the whole page fades light <-> dark)
page_binds = [{"point": "ui.pg", "equals": pg,
               "when_equal": {"page": pid, "transition": "fade", "duration_ms": THEME_MS}}
              for pid, _, _, pg in PAGES]

# ---- manifest -----------------------------------------------------------------------------------
manifest = {
    "format": 1,
    "title_id": "0100152000022000",
    "name": "Mario Kart 8 Deluxe dual screen",
    "min_runtime": MIN_RUNTIME,
    "requires_module": True,
    "module": {"abi": 1, "build_ids": MODULE_BUILDS},
    "canvas_w": W,
    "canvas_h": H,
    "background": THEMES["light"]["bg"],
    "font": "romfs:/UI/USen/font.sarc#turbo_MARIOFont.bffnt",
    "font_atlas": "romfs:/UI/USen/font.sarc#turbo_MARIOFont.bffnt",
    # published strings are self-contained facts: no references to private notes or paths
    "_contract": "Values published by the module 0100152000022000: mk.* (race context, "
                 "course / cup names and image keys, horn state), me.* (local player), "
                 "r0..r11.* (per racer: rank, icon, name, items, map position, finished); "
                 "module glue: mk.pict_ok / mk.cup_ok / mk.fit_img / mk.map_img / mk.name_ok, "
                 "mk.course_name_w, r{i}.name_scale; card-swap animation r{i}.row_y / r{i}.row_dx "
                 "(displayed row position / drift, row units) / r{i}.row_glow, rk.flip. "
                 "mk.mock comes only from the DEV mock.",
    "_design": "v3 (0.4.0, runtime 13): flags ui.map / ui.dark / ui.long; derived ui.pg picks "
               "the page (mode x theme); the row format is an anim group on each race page; "
               "holds (on_hold) toggle theme / format.",
    "_swap": f"card-swap animation: cards at r{{i}}.row_y / r{{i}}.row_dx (module); overtake "
             f"{SWAP_NORMAL_MS} ms ease-out with a {SWAP_DRIFT_ROWS:g}-row drift + glow on the "
             f"overtaking card; {SWAP_PILEUP_N}+ cards moving = pile-up, {SWAP_PILEUP_MS} ms, no "
             "drift; the number column and the local number cell stay on the exact ranks.",
    "flags": {"ui.map": 1, "ui.dark": 0, "ui.long": 0},  # MAP mode is the default
    "derived": derived,
    "actions": actions,
    "page_binds": page_binds,
    "haptics": {"enabled": True, "respect_system": True, "tap": "light", "hold": "heavy",
                "refused": "off"},
    "pages": [{"id": pid, "title": title, "widgets": ws} for pid, title, ws, _ in PAGES],
}


# ---- data files (one per build; data.json = any other build) ----------------------------------
def phase_span(a, b):
    return f"{a}..{b} ({', '.join(PHASE_NAMES[a:b + 1])})"


def data_file(build, supported, ver, source):
    return {
        "_game": "Mario Kart 8 Deluxe",
        "_build_id": build,
        "_source": source,
        "_note": "Per-build named constants (manifest derived). mkc.supported / mkc.build_ver "
                 "drive the wrong-version card; mk.phase windows: loading card "
                 f"{phase_span(0, PHASE_LOAD_MAX)}, race "
                 f"{phase_span(PHASE_RACE_MIN, PHASE_RACE_MAX)}.",
        "derived": [
            const("mkc.supported", supported),
            const("mkc.build_ver", ver),
            const("mkc.phase_load_max", PHASE_LOAD_MAX),
            const("mkc.phase_race_min", PHASE_RACE_MIN),
            const("mkc.phase_race_max", PHASE_RACE_MAX),
        ],
    }


DATA = {b + ".json": data_file(b, ok, ver, src) for b, ok, ver, _, src in BUILDS}
DATA["data.json"] = data_file("any other build", 0, 0, "fallback for unknown builds")


# ---- bind check: every name a widget, action, page bind or derived value reads must exist ------
def check_binds():
    known = set(MODULE_OUTPUTS) | {d["name"] for d in derived} | set(manifest["flags"])
    known |= {d["name"] for f in DATA.values() for d in f["derived"]}
    keys = ("bind", "need_bind", "hide_bind", "x_bind", "y_bind", "src_bind", "bind_text",
            "enabled_bind")
    refs = set()

    def add(n):
        n = n[len("@flag:"):] if n.startswith("@flag:") else n
        refs.update(n.replace("{i}", str(i)) for i in range(N_RACERS)) if "{i}" in n \
            else refs.add(n)

    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in keys and isinstance(v, str):
                    add(v)
                elif isinstance(v, (dict, list)):
                    walk(v)
        elif isinstance(o, list):
            for x in o:
                walk(x)
    walk(manifest["pages"])
    walk(manifest["actions"])
    for pb in page_binds:
        add(pb["point"])
    for d in derived:
        for k in ("a", "b", "select", "then", "else"):
            if isinstance(d.get(k), str):
                add(d[k])
        for n in d.get("all_nonzero", []):
            add(n)
        for n, _ in d.get("terms", []):
            add(n)
    missing = sorted(refs - known)
    unused = sorted(n for n in {d["name"] for d in derived} if n not in refs)
    ids = [(p["id"], w["id"]) for p in manifest["pages"] for w in p["widgets"] if "id" in w]
    dup_ids = sorted({x for x in ids if ids.count(x) > 1})
    names = [d["name"] for d in derived]
    dup_derived = sorted({n for n in names if names.count(n) > 1})
    problems = [f"{what}: {lst}" for what, lst in (
        ("reads unknown values", missing), ("unused derived values", unused),
        ("duplicate widget ids", dup_ids), ("duplicate derived names", dup_derived)) if lst]
    if problems:
        sys.exit("gen_manifest.py: " + "; ".join(problems))


# ---- output -------------------------------------------------------------------------------------
check_binds()
os.makedirs(PKG, exist_ok=True)
with open(os.path.join(PKG, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=1, ensure_ascii=False)
    f.write("\n")
for name, d in DATA.items():
    with open(os.path.join(PKG, name), "w") as f:
        json.dump(d, f, indent=1)
        f.write("\n")
print("pages:", ", ".join(f"{pid} {len(ws)}" for pid, _, ws, _ in PAGES),
      "| derived", len(derived))
