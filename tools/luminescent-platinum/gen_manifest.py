#!/usr/bin/env python3
"""Generate Diamond or Pearl companion manifests (field and battle: direction B).

The pages repeat a lot (six party plates, four move bars, three HP colours), so the manifest is
built here instead of by hand. Every picture is a `module:lp:` key the native module decodes from
the player's own romfs, and every label uses the game's text font (FOT-UDKakugoC80Pro-DB), so the
package ships no game art.

    python3 tools/luminescent-platinum/gen_manifest.py
    python3 tools/luminescent-platinum/gen_manifest.py --edition pearl

Each edition supports vanilla 1.3.0 and Luminescent Platinum from the same package. The
Diamond output stays in packages/LuminescentPlatinum; Pearl uses packages/LuminescentPlatinumPearl.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
EDITIONS = {
    "diamond": ("0100000011D90000", "94CEAE325C205C4B9D6F7235552F28FD" + "0" * 32,
                "LuminescentPlatinum", "Pokémon Brilliant Diamond / Luminescent Platinum dual screen"),
    "pearl": ("010018E011D92000", "38F59CBDA2EB9C44B72F94C4D25935A2" + "0" * 32,
              "LuminescentPlatinumPearl", "Pokémon Shining Pearl / Luminescent Platinum dual screen"),
}
TITLE, BUILD, PACKAGE_DIR, NAME = EDITIONS["diamond"]
PKG = ROOT / "packages" / PACKAGE_DIR
MIN_RUNTIME = 19
W, H = 1240, 1080

DARK = "#FF3C3C3C"
WHITE = "#FFFFFFFF"
PLATE_OUTLINE = "#FF1E325A"
CMD_OUTLINE = "#FF141420"
CREAM = "#FFFFFBEC"

# Motion: short fades only, no slides of whole pages and no grow/shrink (those scale the page and
# alias its text). A page transition blocks touches while it runs, so the ones the player causes
# by tapping stay at 150 ms or less; the game's own state changes may take a little longer.
FADE_START = {"transition": "fade", "duration_ms": 250, "easing": "ease_out"}   # start screen
FADE_FIELD = {"transition": "fade", "duration_ms": 180, "easing": "ease_out"}   # Party: nav tab, after a battle
FADE_TAB = {"transition": "fade", "duration_ms": 150, "easing": "ease_out"}     # the other nav tabs, Route, full Pokétch
FADE_BATTLE_IN = {"transition": "fade", "duration_ms": 220, "easing": "ease_in_out"}  # field -> battle
FADE_BATTLE = {"transition": "fade", "duration_ms": 120, "easing": "ease_out"}  # command <-> moves / balls, new turn
TAB_ANIM_MS = 140    # a card's tab underline growing from / shrinking into its centre
POPUP_ANIM_MS = 120  # a pop-up card fading in / out (its dim veil is instant)
START_ANIM_MS = 450  # start screen: silhouettes cross-fade to colour once


# Sprite motion (module lp_anim.h): the module publishes each sprite's pose as lp.am.<slot>.<channel>; the
# manifest gates every channel with the Animations setting (lp.an.* = the pose, or the rest value when off)
# and binds it to the sprite. Channels: x / y offset px, r degrees, s scale x1000, a fade (a tint index).
ANIM_FLAG = "lp_anim"
ANIM_REST = {"x": 0, "y": 0, "r": 0, "s": 1000, "a": 0}
ANIM_FADE_STEPS = 8  # lp_anim::FadeSteps: opaque .. a third
ANIM_FADE = ["#%02XFFFFFF" % round(255 - k * (255 - 0x55) / ANIM_FADE_STEPS) for k in range(ANIM_FADE_STEPS + 1)]
ANIM_DERIVED: dict[str, dict] = {}


def register(registry, key, value):
    """Declare generated data even under python -O; reject conflicting declarations."""
    if registry.setdefault(key, value) != value:
        raise ValueError(f"conflicting declaration: {key}")


def anim(slot, channel, src=None, gate=""):
    """The gated value of a sprite channel (a derived lp.an.<slot>.<channel>, declared once)."""
    name = f"lp.an.{slot}.{channel}"
    rest = ANIM_REST[channel]
    source = src or f"lp.am.{slot}.{channel}"
    expr = f"@flag:{ANIM_FLAG}{gate} ? {source} : {rest}"
    register(ANIM_DERIVED, name, {"name": name, "expr": expr})
    return name


def battle_motion(slot, w, h):
    """A battle sprite's binds: hit shake / twitch (x, y), faint sink (y) and fade (tint), the low-HP
    sway and the sleep bob; it turns about its feet."""
    return {"x_bind": anim(slot, "x"), "y_bind": anim(slot, "y"), "rotate_bind": anim(slot, "r"),
            "pivot": [w // 2, h - 4], "tint_bind": anim(slot, "a"), "tint_colors": ANIM_FADE}


def ui(bundle: str, sprite: str) -> str:
    return f"module:lp:ui/{bundle}/{sprite}"


def img(rect, src=None, **kw):
    w = {"type": "image", "rect": rect, "color": WHITE}
    if src:
        w["src"] = src
    w.update(kw)
    return w


def f32(v):
    """The shortest number that is the same float32 (the runtime reads src_rect as float)."""
    if float(v).is_integer():
        return int(v)
    t = struct.pack("<f", v)
    for d in range(1, 10):
        s = float(f"{v:.{d}g}")
        if struct.pack("<f", s) == t:
            return s
    return v


def sliced(r, src, size, insets, scale=1, crop=None, **kw):
    """Nine regions with uniformly scaled corners; runtime slice keeps corners at 1x.

    Stretch only straight edges and the centre, never icons or angled corner artwork.
    crop is a pixel rectangle (left, top, right, bottom) inside the source.
    """
    x, y, w, h = r
    iw, ih = size
    u, v, u1, v1 = crop or (0, 0, iw, ih)
    l, t, rr, b = insets
    sx, sy = [u, u + l, u1 - rr, u1], [v, v + t, v1 - b, v1]
    dx = [x, x + round(l * scale), x + w - round(rr * scale), x + w]
    dy = [y, y + round(t * scale), y + h - round(b * scale), y + h]
    out = []
    for row in range(3):
        for col in range(3):
            if dx[col+1] <= dx[col] or dy[row+1] <= dy[row]:
                continue
            out.append(img([dx[col], dy[row], dx[col+1]-dx[col], dy[row+1]-dy[row]], src,
                           src_rect=[f32(sx[col]/iw), f32(sy[row]/ih), f32(sx[col+1]/iw), f32(sy[row+1]/ih)],
                           **kw))
    return out


def label(rect, text=None, bind_text=None, scale=6, color=DARK, **kw):
    w = {"type": "label", "rect": rect, "text_scale": scale, "color": color}
    if text is not None:
        w["text"] = text
    if bind_text:
        w["bind_text"] = bind_text
    w.update(kw)
    return w


# ---- strings: every word on the pages is a row of lp_strings.tsv / LABEL_KEYS (lp_strings.py) ------
# t(key) binds a label to lp.t.<key>, which the module publishes in the game's language; tw(key,
# scale) is the module-measured width of that text (lp.t.<key>.w<scale>) for x_bind; tsum() a row of
# them added up. The generator writes the registry into native/modules/lp_strings_data.h.
import lp_strings as LS  # noqa: E402  (tools/luminescent-platinum, next to this file)

STRINGS = LS.load()
USED: dict[str, None] = {}
WIDTHS: dict[tuple, tuple] = {}  # (key, scale) -> (min_scale, wrap)
SUMS: dict[str, tuple] = {}      # name -> (scale, limit, keys)
HEADER = ROOT / "native" / "modules" / "lp_strings_data.h"


def t(key):
    """The module-published text of string `key` (lp.t.<key>)."""
    if key not in STRINGS:
        raise ValueError(f"unknown string {key!r}: add it to lp_strings.tsv or LABEL_KEYS")
    USED[key] = None
    return f"lp.t.{key}"


def tw(key, scale, min_scale=0, wrap=0):
    """The width (px) the module measures for string `key` drawn at `scale` (fit_text down to
    min_scale within `wrap` px when wrap > 0): lp.t.<key>.w<scale>, for x_bind."""
    t(key)
    spec = (min_scale or scale, wrap)
    register(WIDTHS, (key, scale), spec)
    return f"lp.t.{key}.w{scale}"


def tsum(name, keys, scale, limit=0):
    """The widths of `keys` at `scale` added up (lp.t.sum.<name>); with a limit, also whether they
    fit in it (lp.t.sum.<name>.fits)."""
    for k in keys:
        t(k)
    spec = (scale, limit, tuple(keys))
    register(SUMS, name, spec)
    return f"lp.t.sum.{name}"


def max_latin_width(key, scale):
    """The widest Latin-script column of a string at `scale` (Kakugo, the generator's font): a fixed
    box sized by it is checked against every language, Korean and Chinese too, by the fit report."""
    row = STRINGS[key]
    texts = [row.english] + [v for c, v in row.text.items() if v and c not in ("ko", "zh-Hans", "zh-Hant")]
    return max(text_width(s, scale) for s in texts)


def value(rect, bind, scale=6, color=DARK, **kw):
    w = {"type": "value", "rect": rect, "bind": bind, "text_scale": scale, "color": color}
    w.update(kw)
    return w


def rect(r, bg, **kw):
    w = {"type": "rect", "rect": r, "bg": bg, "color": "#00000000"}
    w.update(kw)
    return w


# Classic Platinum DS theme: the same panels in the DS palette (white windows, red headers,
# blue buttons). Colours not listed keep their value in both themes. Only panel() colours use it.
CLASSIC = {CREAM: "#FFF8F8F8", "#FF2B4C9E": "#FF9A9EA6", "#FFF3C716": "#FF5878A8", "#FF538ABE": "#FF4878D0",
           "#F2FAFCFF": "#FFF8F8F8", "#FFFAF6E8": "#FFF2F2F2", "#FFF0EAD6": "#FFE6E6E6",
           "#FF5E6E96": "#FF5A6070", "#FFF9F6ED": "#FFF8F8F8", "#FFD8EAF4": "#FFE8E8F0",
           "#F2FAFAFA": "#FFF8F8F8"}
# List rows (Bag, Pokédex): cream rows, the selected one darker blue; in classic, light grey rows and
# the DS's pale blue selection on the white window.
LIST_ROW, LIST_SEL, LIST_SEL_RING = "#FFF6F1E2", "#FFC9DDF0", "#FFE8A33C"
CLASSIC.update({LIST_ROW: "#FFECECEE", LIST_SEL: "#FFC0D8F8"})


THEME = "@flag:platinum_theme"  # 0 BDSP, 1 classic Platinum DS
# Native sprites the classic theme repaints (lp_assets ClassicRestyle keeps their outline).
THEMED_SPRITES = {f"ui/menutop/menu_bt_01_body_{n:02d}" for n in (1, 2, 3, 6, 9)} | {
    f"ui/sharedui/btl_bt_command_01_body_{n:02d}" for n in range(1, 6)} | {
    "ui/pokemon/cmn_bt_pokemon_01_body_01", "ui/pokemon/cmn_bt_pokemon_01_body_02",
    "ui/sharedui/btl_bt_quickitem_01_body", "ui/resident/pkc_img_poketch_01_01"}  # the ball window keeps its dark count field


def themed(color):
    """tint_bind for a panel colour: lp.theme 0 = BDSP, 1 = classic Platinum (none if unchanged)."""
    classic = CLASSIC.get(color, color)
    return {} if classic == color else {"tint_bind": THEME, "tint_colors": [color, classic]}


def panel(r, color=CREAM, band=None, band_h=56, shadow=True, **kw):
    """Native rounded mask, tinted like the reference panels; corners retain their radius."""
    x, y, w, h = r
    out = []
    mask = ui("sharedui", "cmn_pl_RS_01_r8_1111")
    if shadow:
        for pad, alpha in ((6, "12"), (3, "20"), (0, "38")):
            out += sliced([x-pad, y+6-pad, w+pad*2, h+pad*2], mask,
                          (24, 24), [8, 8, 8, 8], 2, color=f"#{alpha}000000", **kw)
    out += sliced(r, mask, (24, 24), [8, 8, 8, 8], 2, color=color, **themed(color), **kw)
    if band:
        out += sliced([x, y, w, band_h], ui("sharedui", "cmn_pl_RS_01_r8_1100"),
                      (24, 24), [8, 8, 8, 2], 2, color=band, **themed(band), **kw)
    return out


def on_band(*widgets):
    """Text on a yellow #FFF3C716 band: DARK in the BDSP theme; in the classic theme the band is slate
    blue (#FF5878A8; red would clash with the DS strips), so the text turns white with a dark outline.
    Two copies gated by the theme flag (the nav bar's pattern)."""
    out = []
    for widget in widgets:
        assert "hide_bind" not in widget
        out.append(dict(widget, hide_bind=THEME, hide_eq=1))
        out.append(dict(widget, color=WHITE, outline=BAND_OUTLINE, outline_px=3, hide_bind=THEME, hide_eq=0))
    return out


BAND_OUTLINE = "#FF383838"


def hp_bar(x, y, w, h, p):
    """Native track and gradient gauge, revealing the current HP fraction."""
    out = sliced([x, y, w, h], ui("sharedui", "btl_guage_hp_01_bg"),
                 (8, 6), [2, 2, 2, 2], h / 6)
    for zone, number in (("hpg", 1), ("hpy", 2), ("hpr", 3)):
        out.append({"type": "bar", "rect": [x, y, w, h], "bind": p + "hp", "max_bind": p + "hpmax",
                    "image": ui("sharedui", f"cmn_guage_01_body_{number:02d}"),
                    "color": WHITE, "bg": "#00000000", "frame": 0, "need_bind": p + zone})
    return out


# The modern backgrounds: a soft lavender gradient (field) or the battle navy, with one huge faint
# Poké Ball whose centre sits low right, most of it off-screen (the owner's pick of three, 2026-10-08).
BG_BALL = (1040, 900, 760)  # centre x, y and radius


def faint_ball(cx, cy, r, top, bottom=None, **kw):
    """The battle menu's quarter Poké Ball (sharedui/btl_deco_command_01, white with its band and
    button cut out) mirrored into a whole ball of radius ~r: one translucent layer per quarter, so
    nothing overlaps (top / bottom colours for the upper and lower halves)."""
    s = r / 298
    qw, qh = round(299 * s), round(300 * s)
    src = ui("sharedui", "btl_deco_command_01")
    out = []
    for fx, fy in ((False, False), (True, False), (False, True), (True, True)):
        extra = {}
        if fx:
            extra["flip_x"] = True
        if fy:
            extra["flip_y"] = True
        out.append(img([cx if fx else cx - qw, cy if fy else cy - qh, qw, qh], src,
                       color=(bottom or top) if fy else top, **extra, **kw))
    return out


def gradient(top, bottom, y0=0, y1=H, bands=30, **kw):
    """A vertical gradient in flat bands."""
    a = [int(top[i:i + 2], 16) for i in (3, 5, 7)]
    b = [int(bottom[i:i + 2], 16) for i in (3, 5, 7)]
    step = (y1 - y0) / bands
    out = []
    for k in range(bands):
        f = k / (bands - 1)
        c = "#FF" + "".join(f"{round(a[i] + (b[i] - a[i]) * f):02X}" for i in range(3))
        out.append(rect([0, round(y0 + k * step), W, round(step) + 1], c, **kw))
    return out


def background():
    on = {"need_bind": "lp.cfg.modern"}
    return ([rect([0, 0, W, H], "#FF8A96B9", bind="lp.bt.scene_revision")] +
            gradient("#FF96A2C4", "#FF808CB4", **on) + faint_ball(*BG_BALL, "#1CFFFFFF", **on) +
            classic_background(False))


def battle_background():
    on = {"need_bind": "lp.cfg.modern"}
    return ([rect([0, 0, W, H], "#FF26324A", **on)] + faint_ball(*BG_BALL, "#12FFFFFF", **on) +
            classic_background(True))


# ---- classic Platinum DS background: pale grey lines, a red strip at the top and the bottom (green in
# battle) and the DS's red Poké Ball in the middle --------------------------------------------------
# The ball is faint_ball() over the battle menu's own Poké Ball quarter (sharedui/btl_deco_command_01).
CLASSIC_BG, CLASSIC_LINE = "#FFDCDCE0", "#FFE8E8EC"
STRIPS = {False: ("#FFD84838", "#FF8C2820", "#FFF08878"),   # field: red, its dark edge, its light line
          True: ("#FF40A050", "#FF1E5C2C", "#FF84D08C")}    # battle: green
FIELD_TOP_H = 48                       # field: the top strip (the bottom one is the nav bar)
BATTLE_TOP_H, BATTLE_BOTTOM_Y = 220, 1048  # battle: the strip behind the header cards, and below the pane
# the background ball is faint (the owner: "more faint / dim"): its top half a light red, its bottom white
CLASSIC_BALL_TOP, CLASSIC_BALL_BOTTOM = "#40E84838", "#90FFFFFF"


def strip(y, h, battle, edge_at_bottom, **kw):
    """A DS menu strip: the colour, a dark edge line toward the page and a light line inside it."""
    color, edge, light = STRIPS[battle]
    ey = y + h - 4 if edge_at_bottom else y
    ly = y + h - 10 if edge_at_bottom else y + 6
    return [rect([0, y, W, h], color, **kw), rect([0, ey, W, 4], edge, **kw), rect([0, ly, W, 3], light, **kw)]


def classic_background(battle):
    on = {"need_bind": "lp.cfg.classic"}
    out = [rect([0, 0, W, H], CLASSIC_BG, **on), rect([0, 0, W, 6], CLASSIC_LINE, repeat=90, repeat_dy=12, **on)]
    if battle:
        out += strip(0, BATTLE_TOP_H, True, True, **on) + strip(BATTLE_BOTTOM_Y, H - BATTLE_BOTTOM_Y, True, False, **on)
        out += faint_ball(W // 2, (BATTLE_TOP_H + BATTLE_BOTTOM_Y) // 2, 298, CLASSIC_BALL_TOP, CLASSIC_BALL_BOTTOM, **on)
    else:
        # the bottom strip is the nav bar's own (field_navigation draws it red in this theme)
        out += strip(0, FIELD_TOP_H, False, True, **on)
        out += faint_ball(W // 2, (NAV_Y - 14) // 2, 298, CLASSIC_BALL_TOP, CLASSIC_BALL_BOTTOM, **on)
    return out


def party_plate(x, y, w, h, p, *, slot=None):
    """A party plate as the game draws it: blue body, icon over its left edge, name, HP, level.
    With a slot index the plate is selectable (tap, drag to swap) and shows the cursor."""
    body = sliced([x, y, w, h], ui("pokemon", "cmn_bt_pokemon_01_body_01"),
                  (412, 94), [44, 20, 44, 20], h / 94)
    if slot is not None:
        i = slot
        for widget in body:
            widget.update(need_bind=f"lp.sel{i}.off")
        body += sliced([x, y, w, h], ui("pokemon", "cmn_bt_pokemon_01_body_02"),
                       (412, 94), [44, 20, 44, 20], h / 94, need_bind=f"lp.sel{i}.on")
        # Native selection comes from selN. Runtime selection groups also enable
        # tap-then-tap drops, so omit both groups to reserve swapping for real drags.
        body.append(rect([x, y, w, h], "#00000000", on_tap=f"sel{i}", payload=str(i),
                         draggable=True, drag_scale=1.04, drop_action=f"swap{i}"))
    tx = x + 138
    out = sliced([x + 6, y + 10, w - 4, h], ui("pokemon", "cmn_bt_pokemon_01_shadow"),
                 (36, 10), [8, 3, 8, 3], h / 94, color="#80FFFFFF") + body + [
        img([x + 8, y + 6, 112, 112], src_bind=p + "icon", tap_block=False,
            **({} if slot is None else
               {"y_bind": anim(f"pl{slot}", "y", "lp.am.sel.y", f" && lp.sel == {slot}")})),
        label([tx, y + int(h * 0.12), w - (tx - x) - 70, 0], bind_text=p + "name", scale=6,
              color=WHITE, outline=PLATE_OUTLINE, outline_px=3, fit_text=True, text_min_scale=4,
              wrap_width=w - (tx - x) - 70),
        img([x + w - 58, y + (10 if slot is not None else int(h * 0.14)), 40, 40], src_bind=p + "gendericon"),
        label([tx, y + int(h * 0.46), 40, 0], bind_text=t("hp"), scale=3, color="#FFF4C428", outline=DARK,
              outline_px=2, _avail=46, need_bind=p+"details"),
    ]
    # the HP row sits midway between the name and the numbers
    detail_start = len(out)
    # The whole card is draggable; the HP gauge can use its full original width.
    out += hp_bar(tx + 50, y + int(h * 0.48), w - (tx - x) - 78,
                  max(12, int(h * 0.10)), p)
    out += [
        value([tx, y + int(h * 0.72), 0, 0], p + "hp", scale=5, color=WHITE, outline=PLATE_OUTLINE,
              outline_px=3, max_bind=p + "hpmax", max_sep="/"),
        label([x + w - 210, y + int(h * 0.72), 0, 0], bind_text=t("level_prefix"), scale=5, color=WHITE,
              outline=PLATE_OUTLINE, outline_px=3, _avail=110),
        value([x + w - 30, y + int(h * 0.72), 0, 0], p + "level", scale=5, color=WHITE,
              outline=PLATE_OUTLINE, outline_px=3, align="right"),
        img([x + 94, y + 84, 36, 36], src_bind=p + "itemicon"),
    ]
    gated_group(out[detail_start:], p+"details")
    if slot is not None:
        out += brackets([x, y, w, h], f"lp.sel{slot}.on")  # the game's cursor on the selected plate
        # Every layer travels together, including labels, health, cursor and touch target.
        shared = {channel: anim(f"swap{slot}", channel) for channel in ("x", "y")}
        for widget in out:
            for channel in ("x", "y"):
                previous = widget.get(channel + "_bind")
                if previous:
                    # The selected sprite additionally hops; other layers reuse the same pose.
                    source = f"lp.am.swap{slot}.{channel} + {previous}"
                    widget[channel + "_bind"] = anim(f"swap{slot}_sprite", channel, source)
                else:
                    widget[channel + "_bind"] = shared[channel]
    return out


def move_body(x, y, w, h, p):
    """The native move button's shadow and type-coloured 9-slice body (shown while p+"id" is set)."""
    out = sliced([x + 6, y + 8, w, h], ui("sharedui", "btl_bt_waza_01_shadow"),
                 (24, 20), [8, 6, 8, 6], h / 76, color="#90FFFFFF", need_bind=p + "id")
    out += sliced([x, y, w, h], None, (418, 76), [80, 10, 30, 10], h / 76,
                  bind=p + "wz", src_format="module:lp:wz/%d", need_bind=p + "id")
    # the classic bodies have an empty cap: draw the type icon there (cropped from the BDSP body);
    # p+"cicon" (a move, classic theme) gates it, so a caller's hide_bind stays the only hide gate
    k = h / 76
    out.append(img([x + round(12*k), y + round(8*k), round(54*k), round(54*k)], bind=p + "type",
                   src_format="module:lp:wz/%d", src_rect=WZ_ICON, need_bind=p + "cicon"))
    return out


# The type icon in the move button body's left cap (418x76 btl_bt_waza_01_body_XX): a square crop.
WZ_ICON = [f32(12/418), f32(8/76), f32(66/418), f32(62/76)]


def move_bar(x, y, w, h, p, *, on_tap=None, pill=True):
    """Type-coloured move bar: type icon cap on the left, name, type pill, PP.

    pill=True is the compact row (the Party Moves tab, the Bag's PP pop-up: name on top, the type
    tag and PP under it); pill=False is the large row with the native type pill below the name."""
    out = move_body(x, y, w, h, p)
    if on_tap:
        out.append(rect([x, y, w, h], "#00000000", on_tap=on_tap, need_bind=p + "id"))
    tx = x + round(80*h/76) + 24
    if pill:
        out += [
            # the bar's frame leaves about y+13..y+93 inside (h=112): name, then tag and PP 7 px clear
            label([tx, y+16, w-(tx-x)-36, 0], bind_text=p+"name", scale=6,
                  color=DARK, need_bind=p+"id", fit_text=True, text_min_scale=5,
                  wrap_width=w-(tx-x)-36, max_lines=1),
            img([tx, y+56, 138, 30], src_bind=p+"tag", need_bind=p+"id"),
            value([x+w-36, y+56, 0, 0], p+"pp", scale=6, color=DARK,
                  max_bind=p+"ppmax", max_sep="/", align="right", need_bind=p+"id"),
        ]
    else:
        # Native type pill below the name; leave generous padding inside the angled frame.
        out += [
            label([tx, y+40, w-(tx-x)-36, 0], bind_text=p+"name", scale=7,
                  color=DARK, need_bind=p+"id", fit_text=True, text_min_scale=4,
                  wrap_width=w-(tx-x)-36),
            img([tx, y+h-86, 184, 40], src_bind=p+"tag", need_bind=p+"id"),
            value([x+w-36, y+h-87, 0, 0], p+"pp", scale=8, color=DARK,
                  max_bind=p+"ppmax", max_sep="/", align="right", need_bind=p+"id"),
        ]
    return out


def command_button(r, idx, key, scale, on_tap):
    """A battle command button. The body sprite (252x78) ends in its icon; the frame part is
    stretched (9-slice), the icon end is scaled uniformly so the ring keeps its shape."""
    x, y, w, h = r
    src = ui("sharedui", f"btl_bt_command_01_body_{idx:02d}")
    if idx == 5:
        # Back: the arrow icon end is cropped away, so the frame's own right edge (the sprite's last
        # 12 px) is drawn as a separate cap to close the border.
        cap = round(12 * h / 78)
        body = (sliced([x, y, w - cap, h], src, (252, 78), [12, 12, 0, 12], h/78, crop=(0, 0, 152, 78)) +
                sliced([x + w - cap, y, cap, h], src, (252, 78), [0, 12, 0, 12], h/78, crop=(240, 0, 252, 78)))
    else:
        body = sliced([x, y, w, h], src, (252, 78), [12, 12, 100, 12], h/78)
    if idx == 3:
        # Both native Bag designs include line art; body_03 alone is intentionally
        # blank. One selector combines player sex with the current theme.
        names = [ui("sharedui", f"btl_bt_command_01_body_03_{variant:02d}") for variant in (2, 1)]
        for widget in body:
            widget.pop("src", None)
            widget.update(bind="lp.bt.bag.style", src_names=names + [
                name.replace("module:lp:", "module:lp:classic/", 1) for name in names])
    out = (sliced([x + 6, y + 8, w, h], ui("sharedui", "btl_bt_command_01_shadow"),
                   (44, 40), [12, 12, 12, 12], h / 78, color="#C0FFFFFF") + body + [
        rect([x, y, w, h], "#00000000", on_tap=on_tap),
        label([x + int(h * 0.25), y, int(w - h * (100/78 + 0.25) - 12), 0],
              bind_text=t(key), scale=scale, color=WHITE, outline=CMD_OUTLINE, outline_px=4,
              fit_text=True, text_min_scale=6, wrap_width=int(w - h * (100/78 + 0.25) - 12), max_lines=1,
              text_center_h=h),
    ])

    if idx == 5:
        out.append(img([x+22,y+18,(h-36)//2,h-36],ui("sharedui","cmn_ico_arrow_01")))
    return out


def mon_strip(x, y, w, p, own):
    """Compact equal-width native cards with the HP value inline beside its gauge."""
    out = panel([x, y, w, 180], color="#F2FAFAFA",
                band="#FF538ABE" if own else "#FFD96660", band_h=64, need_bind="lp.cfg.modern")
    out += panel([x, y, w, 180], color="#FFF0F0E8",
                 band="#FF4F7850" if own else "#FF50524B", band_h=64, need_bind="lp.cfg.classic")
    out += [
        label([x+24, y+18, w-234, 0], bind_text=p+"name", scale=7, color=WHITE,
              fit_text=True, text_min_scale=5, wrap_width=w-234),
        img([x+w-202, y+16, 32, 32], src_bind=p+"gendericon"),
        # "Lv." right before the level: the module measures "100" in the language's face (the Korean
        # and Chinese digits are wider)
        label([x+w-24-12, y+22, 0, 0], bind_text=t("level_prefix"), scale=6, color=WHITE, align="right",
              x_bind=tw("num_100", 6), x_scale=-1, _avail=56),
        value([x+w-24, y+22, 0, 0], p+"level", scale=6, color=WHITE, align="right"),
        img([x+20, y+72, 104, 104], src_bind=p+"icon", **battle_motion("bo" if own else "bf", 104, 104)),
        img([x+136, y+82, 142, 30], src_bind=p+"tag1"),
        img([x+294, y+82, 142, 30], src_bind=p+"tag2"),
        label([x+136, y+135, 42, 0], bind_text=t("hp"), scale=5, color="#FFF4C428", outline=DARK, outline_px=2,
              fit_text=True, text_min_scale=4, wrap_width=42, max_lines=1),
    ]
    out += hp_bar(x+180, y+140, w-376, 18, p)
    out.append(value([x+w-24, y+135, 0, 0], p+"hp", scale=5, color=DARK,
                     max_bind=p+"hpmax", max_sep="/", align="right"))
    for widget in out:
        widget["hide_bind"] = "lp.bt.scene_revision"
        widget["hide_eq"] = -1  # revision is nonnegative; dependency keeps the header painted
    return out


# The double-battle overview: which view position (0 near 1, 1 far 1, 2 near 2, 3 far 2) sits where:
# yours (near) in the left column, the foes (far) in the right one; each column top to bottom in the
# game screen's left-to-right order (near 1, near 2; far 2, far 1).
DBL_ROWS = [[0, 3], [2, 1]]
DBL_SIDE = {1: ("#FF538ABE", "#FF4F7850"), 0: ("#FFD96660", "#FF50524B")}  # yours / the foe's accent
DBL_ACT = "#FFE8A33C"
EFF_PILLS = [("e.good", "#FFC83C32"), ("e.even", "#FF8A8E96"), ("e.weak", "#FF3C78C8"), ("e.none", "#FF30343E")]


def dbl_card(x, y, w, h, v):
    """One battler of a double: icon, name, condition, level, HP bar and value, the move in play's
    factor against it; dimmed when its spot is empty or fainted; tap to focus it."""
    p = f"lp.bt.d{v}."
    on = {"need_bind": p+"on"}
    own = v % 2 == 0
    out = panel([x, y, w, h], color="#F2FAFAFA", **on)
    # accent bar: the side's colour, orange for the one choosing / the target under the cursor
    out += panel([x+8, y+12, 10, h-24], color=DBL_SIDE[1 if own else 0][0], shadow=False, **on)
    out += panel([x+8, y+12, 10, h-24], color=DBL_ACT, shadow=False, need_bind=p+"act")
    # the name row: name, condition, Lv.; the HP row: gauge (yours: and the HP value), then the factor
    # pill of the move in play at the right end (at most "¼×": 57 px at 5)
    lvx = x+w-16-text_width("100", 5)-8
    cw = lvx-LV_RESERVE_5-12-(x+322)
    out += [img([x+24, y+2, 80, 80], src_bind=p+"icon", need_bind=p+"valid", **battle_motion(f"d{v}", 80, 80)),
            label([x+112, y+6, 200, 0], bind_text=p+"name", scale=5, color=DARK, fit_text=True, text_min_scale=4,
                  wrap_width=168, max_lines=1, text_center_h=32, **on),
            img([x+286, y+6, 26, 26], src_bind=p+"gendericon", **on),
            label([x+322, y+9, cw, 0], bind_text=p+"cond", scale=5, color=PANE_TEXT, color_markup=True,
                  fit_text=True, text_min_scale=4, wrap_width=cw, max_lines=1, text_center_h=26, **on),
            label([x+w-16-8, y+10, 0, 0], bind_text=t("level_prefix"), scale=5, color=PANE_LABEL, align="right",
                  x_bind=tw("num_100", 5), x_scale=-1, need_bind=p+"lvon", _avail=LV_RESERVE_5),
            value([x+w-16, y+10, 0, 0], p+"level", scale=5, color=DARK, align="right", need_bind=p+"lvon"),
            label([x+112, y+50, 0, 0], bind_text=t("hp"), scale=3, color="#FFF4C428", outline=DARK, outline_px=2,
                  need_bind=p+"valid", _avail=32)]
    # every card: gauge + HP value (as the single-battle header shows the foe's), then the factor pill
    out += hp_bar(x+146, y+54, 200, 14, p)
    out.append(value([x+358, y+48, 0, 0], p+"hp", scale=5, color=DARK, max_bind=p+"hpmax", max_sep="/",
                     need_bind=p+"hpon"))
    for gate, colour in EFF_PILLS:
        out.append(label([x+w-16, y+43, 0, 0], bind_text=p+"eff", scale=5, color=WHITE, align="right", bg=colour,
                         pill=True, pad=6, auto_w=True, need_bind=p+gate))
    out.append(rect([x, y, w, h], "#8CE8ECF2", need_bind=p+"dim"))  # an empty / fainted spot
    out.append(rect([x, y, w, h], "#00000000", on_tap=f"btfocus{v}", **on))
    out += brackets([x, y, w, h], p+"focus", cs=34)
    out += brackets([x+4, y+4, w-8, h-8], p+"target", cs=22)
    out.append(rect([x, y, w, h], "#8CE8ECF2", need_bind=p+"target.dim"))
    for widget in out:
        widget.setdefault("need_bind", p+"on")  # the HP track: only for a spot the battle has
    return out


def double_overview():
    """The 2x2 overview of a double in the header's place: yours on the left, the foes on the right."""
    gap, rh = 12, 84
    cw = (W - 48 - gap) // 2
    out = []
    for r, row in enumerate(DBL_ROWS):
        for c, v in enumerate(row):
            out += dbl_card(24 + c * (cw + gap), 24 + r * (rh + gap), cw, rh, v)
    for widget in out:
        widget["hide_bind"] = "lp.bt.scene_revision"
        widget["hide_eq"] = -1
    return out


def battle_status():
    """The header: one card per side in a single battle; the 2x2 overview in a double."""
    width = (W-48-24)//2
    single = mon_strip(24, 24, width, "lp.bt.own.", True) + mon_strip(48+width, 24, width, "lp.bt.foe.", False)
    gates = {"lp.cfg.modern": "lp.bt.sgl.modern", "lp.cfg.classic": "lp.bt.sgl.classic"}
    for widget in single:
        need = widget.get("need_bind")
        if need is None:
            widget["need_bind"] = "lp.bt.sgl"
        elif need in gates:
            widget["need_bind"] = gates[need]
        # the HP gauges keep their colour gates: the module clears them in a double
    return single + double_overview()


def brackets(r, bind, cs=44):
    """The game's red corner-bracket cursor around rect r, shown while `bind` is set."""
    x, y, w, h = r
    out = []
    for i, (cx, cy) in enumerate([(x - 10, y - 10), (x + w - cs + 10, y - 10), (x - 10, y + h - cs + 10),
                                  (x + w - cs + 10, y + h - cs + 10)]):
        out.append(img([cx, cy, cs, cs], ui("sharedui", f"cmn_frm_cursor_04_{i}"), need_bind=bind))
    return out


BUSY_DIM = "#55101420"


def battle_dims():
    """The battle pages' dims, over the whole screen but drawn under the left pane and the header
    cards, which stay readable and usable in front of them: subtle while the game is animating (no
    menu has focus) or a drive is running (busy_veil), darker while the game's Bag or Pokémon list
    is open (bag_note, switch_overlay). One at a time."""
    return [rect([0, 0, W, H], BUSY_DIM, need_bind="lp.bt.busy.dim"),
            rect([0, 0, W, H], OVERLAY_DIM, need_bind="lp.bt.overlay.dim")]


BATTLE_DIM_DERIVED = [
    {"name": "lp.bt.overlay.dim", "expr": "lp.bt.bagwin == 1 || lp.bt.sw.on == 1"},
    {"name": "lp.bt.busy.dim", "expr": "lp.bt.input != 1 && lp.bt.menu.opening != 1 && lp.bt.bagwin != 1 && lp.bt.sw.on != 1"}]


def busy_veil(x0=0):
    # While the game is animating or a drive is running, taps would do nothing: swallow touches
    # (battle_dims dims the page). It starts at x0, right of the left pane, and under the header
    # cards, so the pane's tabs, swipes and scrolling and the cards keep working during animations.
    return [rect([x0, 220, W-x0, H-220], "#00000000", hide_bind="lp.bt.input", hide_eq=1, input_block=True)]


# The wait / start page (item 9, alternative B, chosen by the owner 2026-10-07): the title screen's own logo over
# the Giratina box wallpaper, Dialga / Giratina / Palkia as dark silhouettes while the companion
# starts, in colour once it is ready (no prompt: the game's own title screen asks for A).
START_READY = "lp.start.ready"
START_BOOT = "lp.start.boot"
# "Starting …" names the game being started: Luminescent Platinum, or vanilla Brilliant Diamond
START_BOOT_LP, START_BOOT_BD = "lp.start.boot.lp", "lp.start.boot.bd"
# Ready once the catalog is read, with or without Luminescent Platinum: lp.catalog is published only
# for the supported build (BD 1.3.0), and vanilla BD is a supported mode (contracts/vanilla-bd130.md).
START_DERIVED = [{"name": START_READY, "expr": "lp.supported == 1 && lp.catalog == 1"},
                 {"name": START_BOOT, "expr": "!(lp.supported == 1 && lp.catalog == 1)"},
                 {"name": START_BOOT_LP, "expr": "lp.lumi == 1 && lp.catalog != 1"},
                 {"name": START_BOOT_BD, "expr": "lp.supported == 1 && lp.lumi == 0 && lp.catalog != 1"}]
# Wallpaper 30 (596x505) without its frame, cropped to the page's aspect.
START_WALL = [f32(31 / 596), f32(20 / 505), f32(565 / 596), f32(485 / 505)]
# Party icons are padded square (module:lp:icon); squares measured from the BD 1.3.0 pokemon_l icons:
# Dialga 196x238 -> 238 at 1.6x, Giratina 216x184 -> 216 at 1.9x (30 px padding under it), Palkia
# 190x215 -> 215 at 1.6x; all three stand on y = 940.
START_MONS = [("483/0/0/0", [40, 559, 381, 381]), ("484/0/0/0", [838, 596, 344, 344]),
              ("487/0/0/0", [415, 560, 410, 410])]


def page_wait_lineup():
    w = [rect([0, 0, W, H], "#FF1C0C12"),
         img([0, 0, W, H], ui("texturemass", "box_pl_box_01_30"), src_rect=START_WALL),
         rect([0, 0, W, H], "#960C040A")]
    # the top darkens toward the logo, the bottom toward the prompt (12 px bands, as the mockup)
    w += [rect([0, 12 * k, W, 12], f"#{round(120 * (1 - k / 29)):02X}000000") for k in range(30)]
    w += [img([240, 50, 760, 432], "module:lp:tex/startlogo", src_bind="lp.tex.logo")]
    for key, (x, y, s, _) in START_MONS:
        pad = round(s * 0.015)
        w += [img([x - pad, y - pad, s + 2 * pad, s + 2 * pad], f"module:lp:mask/icon/{key}", color="#8CFF78AA",
                  need_bind=START_BOOT),
              img([x, y, s, s], f"module:lp:mask/icon/{key}", color="#FF180A1E", need_bind=START_BOOT),
              img([x, y, s, s], f"module:lp:icon/{key}", need_bind=START_READY)]
    w += [rect([0, 900 + 12 * k, W, 12], f"#{round(230 * k / 14):02X}080208") for k in range(15)]
    w += [img([284, 974, 72, 72], ui("sharedui", "cmn_net_monbo_load_01"), spin=40, need_bind=START_BOOT),
          label([W // 2 + 40, 990, 580, 0], bind_text=t("start_starting"), scale=7, color=WHITE,
                outline=PLATE_OUTLINE, outline_px=4, align="center", fit_text=True, text_min_scale=4,
                wrap_width=580, max_lines=1, need_bind=START_BOOT_LP),
          label([W // 2 + 40, 990, 580, 0], bind_text=t("start_starting_sp" if TITLE == EDITIONS["pearl"][0] else "start_starting_bd"), scale=7, color=WHITE,
                outline=PLATE_OUTLINE, outline_px=4, align="center", fit_text=True, text_min_scale=4,
                wrap_width=580, max_lines=1, need_bind=START_BOOT_BD)]
    # Vanilla Brilliant Diamond 1.3.0 (no Luminescent Platinum romfs) is a supported mode: the same page,
    # with BD's own logo (module:lp:tex/logo) and "Starting Brilliant Diamond…".
    # Motion: once the companion is ready the silhouettes and "Starting…" fade out while the colour
    # line-up fades in (two groups over one box: the three Pokémon and the "Starting…" row).
    # Shown already ready, the page appears as it is (groups start at rest).
    for widget in w:
        gate = widget.get("need_bind")
        if gate in (START_BOOT_LP, START_BOOT_BD):
            gate = START_BOOT  # the boot group fades as one; need_bind only picks the game's name
        if gate in (START_BOOT, START_READY):
            widget["anim"] = {"bind": gate, "group": gate, "from": "fade", "ms": START_ANIM_MS,
                              "easing": "ease_in_out", "box": START_ANIM_BOX}
    return {"id": "wait", "widgets": w}


START_ANIM_BOX = [30, 548, W - 60, 516]  # the line-up (y 553..970) and the "Starting…" row (..1060)


NAV_Y = 966
NAV_TABS = [("nav_party", 0, 2, "menu_ico_01_02"), ("nav_dex", 4, 1, "menu_ico_01_01"), ("bag", 5, 3, "bag"),
            ("nav_map", 1, 6, "menu_ico_01_06"), ("nav_poketch", 3, 9, None)]
NAV_W = 232                       # a nav button
NAV_NAME_W = NAV_W - 56 - 10 - 16  # its name: fit to this at 6, down to 4
NAV_SELECTED = ["lp.fld.view == 0", "lp.fld.view == 4", "lp.fld.view == 5",
                "lp.fld.view == 1 || lp.fld.view == 2", "lp.fld.view == 3"]


GAME_FONT = Path(os.environ.get(
    "LP_GAME_FONT", "lp-work/FOT-UDKakugoC80Pro-DB.otf"))
_FONTS = {}


def text_width(text, scale):
    """Width of `text` as the runtime draws it (Canvas::MeasureText): glyph advances of the font atlas,
    which is rasterised at a 48 px CAP height, scaled by 5 * text_scale / 48 -- i.e. the font at the
    size whose cap height is 5 * text_scale. The layout depends on it, so a missing font or PIL stops
    the generator (LP_GAME_FONT overrides the font's path)."""
    if scale not in _FONTS:
        try:
            from PIL import ImageFont
        except ImportError:
            raise SystemExit("gen_manifest: PIL (Pillow) is needed to measure text with the game font")
        if not GAME_FONT.is_file():
            raise SystemExit(f"gen_manifest: the game font {GAME_FONT} is missing (set LP_GAME_FONT to its path)")
        ref = ImageFont.truetype(str(GAME_FONT), 100)
        cap = ref.getbbox("H")[3] - ref.getbbox("H")[1]
        _FONTS[scale] = ImageFont.truetype(str(GAME_FONT), max(1, round(5 * scale * 100 / cap)))
    return round(_FONTS[scale].getlength(text))


# "Lv." before a level at 5 (the double-battle cards): the widest prefix of the Latin columns plus
# room for the Korean / Chinese faces' own Latin (the fit report checks it in every language)
LV_RESERVE_5 = max_latin_width("level_prefix", 5) + 10
HAIR = "#FFC8CCD6"  # section hairlines


def title_rule(tx, ty, rule_y, x_end, key, scale, room, gap=14, color=None, min_scale=None, **kw):
    """A title at (tx, ty) and a 2 px hairline at rule_y from `gap` px after the title to x_end. The
    title fits `room` px (down to min_scale); the module measures it in the game's language
    (lp.t.<key>.w<scale>) and the line's first piece starts after it (x_bind). Its second piece,
    from tx + gap + room on, never moves, so the line always reaches x_end however long the title."""
    m = min_scale or max(4, scale - 1)
    wb = tw(key, scale, m, room)
    return [label([tx, ty, room, 0], bind_text=t(key), scale=scale, color=color or NAVY, fit_text=True, text_min_scale=m,
                  wrap_width=room, max_lines=1, **kw),
            rect([tx+gap, rule_y, room, 2], HAIR, x_bind=wb, **kw),
            rect([tx+gap+room, rule_y, max(0, x_end-(tx+gap+room)), 2], HAIR, **kw)]


def field_navigation():
    """Direction 2: the game's X-menu buttons along the bottom are the page's only bar (no title
    bar above the content). The selected button rises in a white frame; settings sit at the end."""
    # The bar itself: a dark strip under the tabs with a light top edge and a divider between each
    # tab, so the row reads as tabs; the selected tab rises out of it in its white frame.
    # (classic Platinum: the background's red bottom strip)
    out = [rect([0, NAV_Y-14, W, H-NAV_Y+14], "#FF222B42", hide_bind=THEME, hide_eq=1),
           rect([0, NAV_Y-14, W, 4], "#FF8C9AC0", hide_bind=THEME, hide_eq=1)]
    out += strip(NAV_Y-14, H-NAV_Y+14, False, False, hide_bind=THEME, hide_eq=0)
    for k, (key, view, body, icon) in enumerate(NAV_TABS):
        x, width = 12 + k*244, NAV_W
        for state, y, height in (("off", NAV_Y+8, 94), ("on", NAV_Y-4, 108)):
            bind = f"lp.nav{k}.on" if state == "on" else f"!lp.nav{k}.on"
            if state == "on":
                out += panel([x-6, y-6, width+12, height+12], color=WHITE, shadow=True, need_bind=bind)
            dim = {} if state == "on" else {"tint": "#FF8C8C96"}  # unselected: darker, not washed out
            out += sliced([x, y, width, height], ui("menutop", f"menu_bt_01_body_{body:02d}"),
                          (170, 137), [12, 12, 12, 12], .9, need_bind=bind, **dim)
            # icon and name centred together in the button; the name at 6 in both states, so it does
            # not jump when selected, fit down to 4 in NAV_NAME_W. The module measures the name in the
            # game's language: icon and name move left by half of it (positions here are for an
            # empty name).
            scale = 6
            shift = {"x_bind": tw(key, scale, 4, NAV_NAME_W), "x_scale": -0.5}
            ix = x + (width - (56 + 10)) // 2
            iy = y + (height-56)//2
            if icon:
                # the selected icon art for both states, dimmed when unselected
                source = {"src_bind": "lp.nav.bag.on"} if icon == "bag" else {"src": ui("menutop", icon+"_on")}
                source.update(dim)
                iw, ih = (42, 56) if view == 1 else (64, 51) if view == 4 else (56, 56)
                out.append(img([ix+(56-iw)//2, iy+(56-ih)//2, iw, ih], need_bind=bind, **source, **shift))
            else:
                out += [img([ix+1, iy, 54, 56], ui("resident", "pkc_img_poketch_01_01"), need_bind=bind, **dim, **shift),
                        rect([ix+7, iy+3, 40, 29], "#FF91B878" if state == "on" else "#FF6E8A60", need_bind=bind,
                             **shift)]
            out.append(label([ix+66, y+height//2-15, NAV_NAME_W, 0], bind_text=t(key),
                             scale=scale, color=WHITE if state == "on" else "#FFE2E5EC", outline="#FF283048", outline_px=3,
                             fit_text=True, text_min_scale=4, wrap_width=NAV_NAME_W, max_lines=1, need_bind=bind,
                             **shift))
        out.append(rect([x-6, NAV_Y-10, width+12, H-NAV_Y+10], "#00000000", on_tap=f"fview{view}"))
    for widget in out:
        if "hide_bind" not in widget:  # the theme-specific bar base keeps its own
            widget["hide_bind"] = "lp.bt.scene_revision"
            widget["hide_eq"] = -1
    return out


FLY_W = 160
# The Map's name cloud: its white capsule ends CLOUD_BOTTOM px above the selected place; the whole
# cloud stays attached to its world location and is hidden when it would leave the map viewport.
CLOUD_BOTTOM = 40


def fly_button(x, y, action, need, key="fly", width=FLY_W):
    """The orange Fly button (Map and Route): 88 tall, label 6 in white on a brown outline."""
    return panel([x, y, width, 88], color="#FFE8A33C", shadow=False, on_tap=action, need_bind=need) + [
        label([x+width//2, y, width-20, 0], bind_text=t(key), scale=6, color=WHITE, align="center",
              outline="#FF8A4A10", outline_px=3, fit_text=True, text_min_scale=4, wrap_width=width-20, max_lines=1,
              text_center_h=88, on_tap=action, need_bind=need)]


# @map_view_* is published after native module sampling. Runtime 17+ recomputes derived
# expressions that reference @ keys after PublishMapState, so the cloud uses the drawn view.
# Keep the cloud anchored to its place. Hide it instead of clamping it to a screen edge.
MAP_CLOUD_DERIVED = [
    {"name": "lp.map.sel.x", "expr": "@map_view_x:townmap + @map_view_w:townmap / 2 + (lp.map.sel.wx - @map_view_cx:townmap) * @map_view_ppw:townmap"},
    {"name": "lp.map.sel.y", "expr": "@map_view_y:townmap + @map_view_h:townmap / 2 - (lp.map.sel.wy - @map_view_cy:townmap) * @map_view_ppw:townmap"},
    {"name": "lp.map.cloud.on", "expr": "lp.map.sel.vis == 1 && @map_view_ppw:townmap > 0 && lp.map.sel.x - lp.map.sel.half >= @map_view_x:townmap && lp.map.sel.x + lp.map.sel.half <= @map_view_x:townmap + @map_view_w:townmap && lp.map.sel.y - 100 >= @map_view_y:townmap + lp.map.sel.top && lp.map.sel.y <= @map_view_y:townmap + @map_view_h:townmap"},
]


def page_map():
    """The Town Map in the runtime's map widget: pinch to zoom, drag to pan, tap a place to select it
    (name cloud; Route Pokémon and Fly then act on it). A legend explains the map's pieces."""
    w = background()
    w += panel([24, 22, W-48, 928], band="#FFF3C716") + on_band(
        label([48, 32, 420, 0], bind_text=t("map_sinnoh"), scale=6, fit_text=True, text_min_scale=5, wrap_width=420,
              max_lines=1),
        label([W-48, 32, 690, 0], bind_text="lp.map.header.name", scale=6, align="right",
              fit_text=True, text_min_scale=4, wrap_width=690, max_lines=1)) + [
        # the picture's places span x 226..1066 of 1266: the base view (lp.map.v.*) is the full height
        # at the widget's aspect, centred, so only sea and cloud margins left and right are cropped
        {"type": "map", "id": "townmap", "rect": [36, 92, 1168, 756], "area": "townmap",
         "image_bind": "lp.map.image", "pan_zoom": True, "min_zoom": 1.0, "max_zoom": 4.0,
         "view_rect_x0_bind": "lp.map.v.x0", "view_rect_y0_bind": "lp.map.v.y0",
         "view_rect_x1_bind": "lp.map.v.x1", "view_rect_y1_bind": "lp.map.v.y1",
         "area_label": False, "on_map_tap": "map_tap"}]
    # the panel and the map need the Town Map; the background's faint balls keep their theme gate
    for widget in w:
        if widget.get("rect", [0, 0])[1] >= 22 and widget.get("type") != "rect":
            widget.setdefault("need_bind", "lp.map.acquired")
    acq = {"need_bind": "lp.map.acquired"}
    # Buttons (bottom right) act on the selected place: Route Pokémon (where you are without one) and
    # Fly when the game would let you fly there. Taps ignore x_bind, so they stay put here.
    # its width fits the English label at 5 (3 px outline room); other languages fit it down to 4, and
    # the legend keeps room for its longest row at 4 (the fit report checks both)
    rw = 12 + 56 + 10 + text_width(STRINGS["map_route_pokemon"].english, 5) + 36
    rx, by = W-36-rw, 856
    w += panel([rx, by, rw, 88], color="#FF538ABE", shadow=False, on_tap="maproute", **acq) + [
        img([rx+12, by+16, 56, 56], ui("sharedui", "btl_ico_ball_01_01"), on_tap="maproute", **acq),
        label([rx+78, by, rw-78-8, 0], bind_text=t("map_route_pokemon"), scale=5, color=WHITE, outline="#FF283048",
              outline_px=3, fit_text=True, text_min_scale=4, wrap_width=rw-78-8, max_lines=1, text_center_h=88,
              on_tap="maproute", **acq)]
    # Fly sits above Route Pokémon, clear of the legend
    w += fly_button(W-36-FLY_W, by-12-88, "mapfly", "lp.map.sel.fly")
    # Legend (bottom left, one line, centred on the Route Pokémon button): your head is "You", then
    # what the map's pieces mean, with 28 px swatches. Each piece follows the names before it (the
    # module adds up their widths in the game's language: lp.t.sum.legend<s>_<k>); at 5 when the
    # whole row fits before Route Pokémon, else at 4 (lp.map.legend5 / lp.map.legend4).
    lx = 44
    w += [img([lx, by+24, 40, 40], src_bind="lp.map.head", **acq)]
    items = [("road", "map_route"), ("map_ico_symbol_01_01_01", "map_city"), ("map_ico_symbol_02_01_01", "map_town"),
             ("map_ico_symbol_03_01_01", "map_special"), ("gray", "map_not_visited")]
    names = ["map_you"] + [k for _, k in items]
    fixed = 46 + 24 + len(items) * (28 + 8 + 22) - 22  # everything but the names
    room = rx - 16 - lx - fixed  # px left for the names
    for s in (5, 4):
        gate = {"need_bind": f"lp.map.legend{s}"}
        ty = by + 44 - (5 * s) // 2  # the caps centred on the button's middle
        w.append(label([lx+46, ty, 0, 0], bind_text=t("map_you"), scale=s, color="#FF4A4E58", **gate))
        tsum(f"legend{s}", names, s, limit=room)
        ix0, sy = lx + 46 + 24, by + 30  # sy: the 28 px swatches' top
        for k, (key, name) in enumerate(items):
            ix = ix0 + k * (28 + 8 + 22)
            at = {"x_bind": tsum(f"legend{s}_{k}", names[:k+1], s), **gate}
            if key == "road":
                w.append(rect([ix, sy+9, 28, 10], "#FFF2992E", **at))
            elif key == "gray":
                w += panel([ix, sy, 28, 28], color="#FF8E9298", shadow=False, **at)
            else:
                w.append(img([ix, sy, 28, 28], ui("map", key), **at))
            w.append(label([ix+28+8, ty, 0, 0], bind_text=t(name), scale=s, color="#FF4A4E58", **at))
    # Native story guidance stays at the top while the map itself pans/zooms.
    goal = {"need_bind": "lp.map.goal.message.on"}
    # Runtime18 has no height bind. Exact bounded panels follow the module's native-font
    # wrapped height; short guidance is64px, with balanced vertical padding around text/flag.
    for height in (64, 76, 89, 108, 129):
        variant = panel([48, 100, 1144, height], color="#FFF9F6E8", shadow=False,
                        need_bind=f"lp.map.goal.height{height}")
        gated_group(variant, "lp.map.goal.message.on")
        w += variant
    w += [img([68, 100, 32, 40], ui("map", "map_ico_goal_01"), y_bind="lp.map.goal.flag.y", **goal)]
    for scale in (5, 4):
        text = [label([124, 100, 1048, 0], bind_text="lp.map.goal.message", scale=scale, color=DARK,
                      wrap_width=1048, max_lines=3, y_bind="lp.map.goal.text.y",
                      need_bind=f"lp.map.goal.text{scale}")]
        gated_group(text, "lp.map.goal.message.on")
        w += text
    # The selected place's name cloud: a capsule sized to its text (auto_w), the name centred in it,
    # over the place wherever the map is panned or zoomed (derived values project it through the
    # runtime's @map_view_* in the late derived pass). The capsule ends CLOUD_BOTTOM above the place; its tail (24x14)
    # hangs from it, its white part opening into the capsule.
    cl = {"x_bind": "lp.map.sel.x", "y_bind": "lp.map.sel.y", "need_bind": "lp.map.cloud.on"}
    cy = -CLOUD_BOTTOM - 14 - 25  # the label's top: the inner capsule is 14 px around the 25 px caps
    w += [label([0, cy, 0, 0], bind_text="lp.map.sel.name", scale=5, align="center", color=DARK, bg=DARK,
                pill=True, pad=17, auto_w=True, **cl),
          rect([-12, -CLOUD_BOTTOM, 24, 14], DARK, **cl),
          label([0, cy, 0, 0], bind_text="lp.map.sel.name", scale=5, align="center", color=DARK, bg=WHITE,
                pill=True, pad=14, auto_w=True, **cl),
          rect([-9, -CLOUD_BOTTOM-3, 18, 14], WHITE, **cl)]
    w += toast(36, 760, 720)
    w += panel([24, 22, W-48, 928], need_bind="lp.map.unavailable")
    w.append(label([620, 440, 1100, 0], bind_text=t("map_not_available"), scale=7, align="center",
                   wrap_width=1100, max_lines=2, need_bind="lp.map.unavailable"))
    w += field_navigation()
    return {"id": "map", "widgets": w}


APP_DY = 84                        # the Pokétch app list's row pitch
APP_NAME_W = 228                   # an app's name in its row (12 px from the highlight's left edge)
LCD_X, LCD_Y = 300 + 123, 6 + 56   # native casing (940x962) at 1:1, its 640x480 display opening
FULL_S = 1.875                     # full screen: 8 px dots -> 15 px, 640x480 -> 1200x900
FULL_X, FULL_Y = (W - 1200) // 2, (H - 900) // 2


def corner_mark(x, y, size, color, inward, **kw):
    """Four small corner ticks: outward = enter full screen, inward = leave it."""
    t, a = max(3, size // 12), size // 3
    out = []
    for cx, cy, dx, dy in ((x, y, 1, 1), (x+size, y, -1, 1), (x, y+size, 1, -1), (x+size, y+size, -1, -1)):
        if inward:  # ticks point to the corner from inside
            cx, cy = cx + dx * a, cy + dy * a
            dx, dy = -dx, -dy
        out += [rect([min(cx, cx + dx * a), cy if dy > 0 else cy - t, a, t], color, **kw),
                rect([cx if dx > 0 else cx - t, min(cy, cy + dy * a), t, a], color, **kw)]
    return out


def lcd_layer(x0, y0, s, full):
    """The display at scale s and everything on it: the picture, the app overlays, and one touch
    grid (40 x 30 cells of 16 LCD px; the module finds the control under a touch). A long press
    toggles full screen; in full screen a swipe changes app (left / right) or leaves (down)."""
    S = lambda v: round(v * s)
    p = "lp.pktx." if full else "lp.pkt."
    sonar = "sonarx" if full else "sonar"
    w = [img([x0, y0, S(640), S(480)], src_bind=p+"fallback", hide_bind=THEME, hide_eq=1),
         img([x0, y0, S(640), S(480)], src_bind=p+"fallbackc", hide_bind=THEME, hide_eq=0),
         img([x0, y0, S(640), S(480)], src_bind=p+"image", hide_bind=THEME, hide_eq=1),
         img([x0, y0, S(640), S(480)], src_bind=p+"imagec", hide_bind=THEME, hide_eq=0)]
    # Sonar, map highlights and rotated arrow share the native monitor texture and point scaling.
    gesture = {"on_hold": "pkt_full2", "hold_ms": 600}
    if full:
        gesture.update(on_swipe_left="pkt_next", on_swipe_right="pkt_prev", on_swipe_down="pkt_full0", swipe_px=90)
    w.append(rect([x0, y0, S(640), S(480)], "#00000000", need_bind="lp.poketch.acquired", **gesture))
    cell = S(16)
    w.append(rect([x0, y0, cell, cell], "#00000000", on_tap="pkt_touch", payload="{i}",
                  need_bind="lp.poketch.acquired", repeat=40*30, repeat_cols=40, repeat_dx=cell, repeat_row_dy=cell))
    scroll = {"id": sonar, "rect": [x0, y0+S(60), S(640), S(360)], "count_bind": "lp.pkt.one", "row_h": S(360)}
    return w, scroll


def page_poketch():
    """The independent Pokétch: the companion's own app list on the left, the native casing and
    separate red app button on the right. Every screen is drawn by the module from the game's
    Pokétch art and save data; the game's own watch is never opened, switched or cropped."""
    w = background()
    w += panel([16, 22, 270, 928], color="#F2FAFCFF", band="#FFF3C716", need_bind="lp.poketch.acquired")
    w += on_band(label([36, 36, 230, 0], bind_text=t("poketch_apps"), scale=5, fit_text=True, text_min_scale=4,
                       wrap_width=230, max_lines=1, need_bind="lp.poketch.acquired"))
    # app rows: 84 apart, the highlight 76 tall, the whole row a tap target; names at 5 within 228 px
    # (the highlight's inside: 12 px from its left edge, 6 px short of its right one), else at 4
    rep = {"repeat": 20, "repeat_bind": "lp.poketch.count", "repeat_dy": APP_DY, "scroll": "apps"}
    r = "lp.poketch.app{i}."
    w += [rect([28, 94, 246, 76], "#FFFFEC96", need_bind=r+"selected", **rep),
          # (a name too long even at 4 takes two lines at 4, centred in the row)
          label([40, 94, APP_NAME_W, 0], bind_text=r+"name", scale=5, fit_text=True, text_min_scale=4,
                wrap_width=APP_NAME_W, max_lines=2, text_center_h=76, _fit=[f"poketch_app_{k}" for k in range(20)],
                **rep),
          rect([28, 90, 246, APP_DY], "#00000000", on_tap="apppick{i}", **rep)]
    # the casing (taller than the "not available" panel), its display and its app button only with a
    # Pokétch: nothing of it shows around or through that panel
    watch = [img([300, 6, 940, 962], ui("resident", "pkc_img_poketch_01_01"))]
    layer, sonar = lcd_layer(LCD_X, LCD_Y, 1.0, False)
    watch += layer
    watch += sliced([1116, 286, 108, 286], ui("resident", "pkc_bt_poketch_01_body_01"), (82, 82),
                    [30, 30, 30, 30], 1, on_tap="pkt_next")
    watch.append(rect([1106, 276, 128, 306], "#00000000", on_tap="pkt_next"))
    w += gated_group(watch, "lp.poketch.acquired")
    # a quiet "full screen" mark on the casing under the display's corner (or long-press the display)
    w += corner_mark(LCD_X+640-44, LCD_Y+480+14, 34, "#99FFFFFF", False, need_bind="lp.poketch.acquired")
    w.append(rect([LCD_X+640-70, LCD_Y+480, 90, 64], "#00000000", on_tap="pkt_full1", need_bind="lp.poketch.acquired"))
    w += panel([16, 22, W-32, 928], need_bind="lp.poketch.unavailable")
    w.append(label([620, 440, 1100, 0], bind_text=t("poketch_not_available"), scale=7, align="center",
                   wrap_width=1100, max_lines=2, need_bind="lp.poketch.unavailable"))
    # the companion's settings (the theme), bottom right of the Pokétch page: in front of the "not
    # available" panel too, so the theme can be changed before the player has a Pokétch
    tw = max_latin_width("poketch_theme", 5) + 6  # the widest Latin name; any language fits it down to 4
    bw = 16 + 48 + 10 + tw + 20
    w += panel([W-16-bw, 872, bw, 64], color="#FF5E6E96", shadow=True, on_tap="open_settings")
    w += [img([W-16-bw+16, 880, 48, 48], ui("menutop", "menu_ico_01_09_on"), on_tap="open_settings"),
          label([W-16-bw+74, 872, tw, 0], bind_text=t("poketch_theme"), scale=5, color=WHITE, outline="#FF283048",
                outline_px=3, fit_text=True, text_min_scale=4, wrap_width=tw, max_lines=1, text_center_h=64,
                on_tap="open_settings")]
    w += field_navigation()
    w += toast(423, 710, 720)
    return {"id": "poketch", "widgets": w,
            "scrolls": [{"id": "apps", "rect": [28, 90, 246, 848], "count_bind": "lp.poketch.count", "follow_bind": "lp.poketch.sel", "follow_open": True,
                         "row_h": APP_DY, "bar": "#FF5E6E96", "bar_w": 8}, sonar]}


def page_poketch_full():
    """The display alone, as big as the screen allows at an even dot size, in a thin casing-coloured
    rim. Leaving is deliberately quiet: swipe down or long-press the display, or tap the faint mark
    in the corner. Swipe left / right changes app; its name shows faintly below."""
    w = [rect([0, 0, W, H], "#FF060708")]
    w += panel([FULL_X-10, FULL_Y-10, 1220, 920], color="#FF2B4C9E", shadow=False)
    layer, sonar = lcd_layer(FULL_X, FULL_Y, FULL_S, True)
    w += layer
    w.append(label([W//2, FULL_Y+900+30, 0, 0], bind_text="lp.pkt.name", scale=4, color="#55FFFFFF", align="center"))
    w += corner_mark(W-FULL_X-34, FULL_Y+900+24, 30, "#55FFFFFF", True)
    w.append(rect([W-170, FULL_Y+900+4, 170, H-(FULL_Y+900+4)], "#00000000", on_tap="pkt_full0"))
    w += toast(W//2-360, 860, 720)
    return {"id": "poketch_full", "widgets": w, "scrolls": [sonar]}


EMPTY, EMPTY_HINT = "#FF8A8E96", "#FF6A6E78"  # empty states: the line at 6, its hint at 5
TOAST_H, TOAST_H2 = 72, 84
TOAST_WRAPS = (680, 512, 432)  # module ToastWraps: it measures each toast against these
TOAST_ANIM_MS = 160  # the toast's slide; keep it under the module's ToastTailMs (250)


def toast(x, y, w):
    """A short message after an action (the module clears it after three seconds). One line at 5, or
    at 4 when it is wider; a message still too wide for one line takes two at 4, centred in a panel
    TOAST_H2 tall that grows upwards (the module's lp.bag.toast.two.<wrap>), so its descenders keep
    clear of the bottom edge."""
    wrap = w - 40
    assert wrap in TOAST_WRAPS, "add the toast's wrap width to the module's ToastWraps"
    two = f"lp.bag.toast.two.{wrap}"
    # Motion: the toast rises out of its own slot (a box just around the taller panel and its shadow)
    # and sinks back into it; the module keeps the text ToastTailMs (> TOAST_ANIM_MS) after
    # lp.bag.toast.on drops, so it leaves with its message.
    top2 = y + TOAST_H - TOAST_H2
    anim = {"anim": {"bind": "lp.bag.toast.on", "group": "toast", "from": "bottom", "ms": TOAST_ANIM_MS,
                     "easing": "ease_out", "box": [x - 6, top2 - 6, w + 12, TOAST_H2 + 18]}}
    out = []
    for top, h, gate in ((y, TOAST_H, {"hide_bind": two, "hide_eq": 1}),
                         (top2, TOAST_H2, {"hide_bind": two, "hide_eq": 0})):
        out += panel([x, top, w, h], color="#FFFFF4C8", need_bind="lp.bag.toast.on", **gate, **anim) + [
            label([x+20, top, wrap, 0], bind_text="lp.bag.toast", scale=5, fit_text=True, text_min_scale=4,
                  wrap_width=wrap, max_lines=2, text_center_h=h, need_bind="lp.bag.toast.on", **gate, **anim)]
    return out


def bag_popup(title="lp.bag.popup.title"):
    """Bag Use: pick the Pokémon (and, for one-move PP items, the move) in a pop-up over the page."""
    w = [rect([0, 0, W, H], "#99101420", need_bind="lp.bag.popup.any", input_block=True)]
    w += panel([150, 150, W-300, 700], band="#FFF3C716", need_bind="lp.bag.popup.any")
    w += on_band(label([180, 166, W-360, 0], bind_text=title, scale=5, fit_text=True,
                       text_min_scale=4, wrap_width=W-360, max_lines=1, need_bind="lp.bag.popup.any",
                       _fit=["popup_give_hp", "popup_pp_up", "popup_pp_restore", "popup_use_on"]))
    for i in range(6):
        px, py = 180 + (i % 2) * 450, 240 + (i // 2) * 160
        pp = f"lp.p{i}."
        # shown while picking a member, for party slots that exist (hp_bar keeps its colour gates)
        # name at 6, a 240x18 HP bar, then the status tag (none when healthy) and HP on one line
        tile = panel([px, py, 430, 140], color="#FFE6EEF8", shadow=False) + [
              img([px+8, py+10, 112, 112], src_bind=pp+"icon"),
              label([px+130, py+14, 280, 0], bind_text=pp+"name", scale=6, fit_text=True, text_min_scale=5,
                    wrap_width=280, max_lines=1),
              img([px+130, py+94, 124, 30], src_bind=pp+"sick"),
              value([px+410, py+96, 0, 0], pp+"hp", scale=5, max_bind=pp+"hpmax", max_sep="/", align="right", need_bind=pp+"details")]
        for widget in tile:
            widget.update(need_bind="lp.bag.popup.party", hide_bind=pp+"valid", hide_eq=0)
        health = hp_bar(px+130, py+62, 240, 18, pp)
        for widget in health:
            widget.update(hide_bind="lp.bag.popup.party", hide_eq=0)
            if "need_bind" not in widget: widget["need_bind"] = pp+"valid"
        gated_group(health, pp+"details")
        tile += health
        for widget in tile:
            if widget.get("bind") == pp+"hp" or widget.get("max_bind") == pp+"hpmax":
                gated_group([widget], pp+"details")
        w += tile
        w.append(rect([px, py, 430, 140], "#00000000", on_tap=f"bagtarget{i}", need_bind="lp.bag.popup.party"))
    for k in range(4):
        my = 226 + k * 130  # the last row ends at 728, above Cancel
        # shown while picking a move, for move slots that hold a move
        for widget in move_bar(200, my, W-400, 112, f"lp.bag.m{k}."):
            w.append(dict(widget, hide_bind="lp.bag.popup.moves", hide_eq=0))
        w.append(rect([200, my, W-400, 112], "#00000000", on_tap=f"bagmove{k}", need_bind=f"lp.bag.m{k}.id",
                      hide_bind="lp.bag.popup.moves", hide_eq=0))
    # Cancel: 300x88, bottom right inside the pop-up (it ends 30 px above the panel's bottom, 850)
    cw, ch = 300, 88
    cx, cy = W-150-30-cw, 850-30-ch
    assert cy > 226 + 3*130 + 112 and cy > 240 + 2*160 + 140, "Cancel overlaps the move rows or the party tiles"
    w += panel([cx, cy, cw, ch], color="#FF538ABE", shadow=False, on_tap="bagcancel", need_bind="lp.bag.popup.any")
    w.append(label([cx+cw//2, cy, cw-24, 0], bind_text=t("cancel"), scale=6, color=WHITE, align="center",
                   outline="#FF283048", outline_px=3, fit_text=True, text_min_scale=4, wrap_width=cw-24, max_lines=1,
                   text_center_h=ch, on_tap="bagcancel", need_bind="lp.bag.popup.any"))
    # Motion: the card fades in over the veil, which itself is instant (a full-screen fade would
    # redraw the whole page every frame); the box is the card with its shadow.
    for widget in w[1:]:
        widget["anim"] = {"bind": "lp.bag.popup.any", "group": "popup", "from": "fade", "ms": POPUP_ANIM_MS,
                          "easing": "ease_out", "box": [144, 150, W-288, 712]}
    return w


# the Bag's pockets in the module's order (their names are the game's labels: lp.bag.pocket)
BAG_POCKETS = [f"pocket_{n}" for n in ("medicine", "balls", "battle", "berries", "other", "tms", "treasures", "key",
                                       "extra")]


def pocket_icon(i, rect_, on, **kw):
    """The game's pocket tile (icon baked in; _02 = selected). LP's Extra Items pocket has no tile in
    the game, so it gets the same white tile with a plus drawn in the tiles' line colour."""
    x, y, s, _ = rect_
    if i < 8:
        return [img(rect_, ui("resident", f"bag_ico_tab_01_{i+1:02d}_{'02' if on else '01'}"), **kw)]
    pad = s // 10
    out = panel([x+pad//2, y+pad//2, s-pad, s-pad], color="#FFF8F8FC", shadow=False, **kw)
    ink, t = ("#FFE04848" if on else "#FF7468D0"), max(4, s // 12)
    out += [rect([x+s//4, y+(s-t)//2, s//2, t], ink, **kw), rect([x+(s-t)//2, y+s//4, t, s//2], ink, **kw)]
    if on:
        out += [rect([x+pad//2, y+pad//2, s-pad, 4], "#FFE04848", **kw), rect([x+pad//2, y+s-pad//2-4, s-pad, 4], "#FFE04848", **kw)]
    return out


def bag_header():
    """The pocket picker, after the game's own Bag header: yellow band, arrows, a row of pocket icons
    and the current pocket's name in an orange title bar (the item list also swipes pockets).
    Returns the widgets and the list area x, y, width, height."""
    w = panel([24, 12, 1192, 140], color="#FFF3C716", shadow=True)
    step = 100
    # Two layouts, one shown (the module publishes lp.bag.l9 / lp.bag.l8 and their tab states):
    # Luminescent Platinum's nine tabs with Extra Items, vanilla Brilliant Diamond's own eight.
    for gate, n in (("lp.bag.l9", len(BAG_POCKETS)), ("lp.bag.l8", len(BAG_POCKETS) - 1)):
        x0 = 620 - (n * step) // 2
        xr = x0+n*step+8
        w += [img([x0-44, 36, 36, 60], ui("resident", "bag_ico_arw_tab_01_L"), on_tap="bagprev", need_bind=gate),
              img([xr, 36, 36, 60], ui("resident", "bag_ico_arw_tab_01_L"), flip_x=True, on_tap="bagnext",
                  need_bind=gate),
              # 90x110 tap targets around the arrows (outside the pocket tiles)
              rect([x0-98, 12, 90, 110], "#00000000", on_tap="bagprev", need_bind=gate),
              rect([xr, 12, 90, 110], "#00000000", on_tap="bagnext", need_bind=gate)]
        for i in range(n):
            x = x0 + i * step
            on, off = f"{gate}.tab{i}.on", f"{gate}.tab{i}.off"
            w += pocket_icon(i, [x+9, 26, 82, 82], False, need_bind=off)
            w += pocket_icon(i, [x+2, 19, 96, 96], True, need_bind=on)
            w.append(rect([x, 18, step, 100], "#00000000", on_tap=f"bagpocket{i}", need_bind=gate))
    w += panel([340, 118, 560, 46], color="#FFE58A2E", shadow=False)
    w.append(label([620, 124, 520, 0], bind_text="lp.bag.pocket", scale=5, align="center", color=WHITE,
                   outline="#FF8A4A10", outline_px=3, fit_text=True, text_min_scale=4, wrap_width=520, max_lines=1,
                   _fit=BAG_POCKETS))
    return w, 24, 176, 660, 772


def page_bag(w):
    head, lx, ly, lw, lh = bag_header()
    w += head
    # left: the pocket's items (the header names the pocket; swipe the list to change pocket)
    w += panel([lx, ly, lw, lh], band="#FFF3C716")
    top = ly + 68
    rep = {"repeat": 2048, "repeat_bind": "lp.bag.count", "repeat_dy": 84, "scroll": "bagitems"}
    r = "lp.bag{i}."
    rw = lw - 32
    w += panel([lx+16, top+3, rw, 74], color=LIST_ROW, shadow=False, **rep)
    # the selected row: a darker fill inside a 4 px orange ring (inside the row, so the scroll clip
    # never cuts it)
    w += panel([lx+16, top+3, rw, 74], color=LIST_SEL_RING, shadow=False, need_bind=r+"selected", **rep)
    w += panel([lx+20, top+7, rw-8, 66], color=LIST_SEL, shadow=False, need_bind=r+"selected", **rep)
    w += [
          img([lx+30, top+10, 58, 58], src_bind=r+"icon", **rep),
          label([lx+104, top+25, rw-232, 0], bind_text=r+"name", scale=6, fit_text=True, text_min_scale=4,
                wrap_width=rw-232, **rep),
          label([lx+16+rw-20, top+25, 0, 0], bind_text=r+"quantity", scale=6, align="right", **rep),
          rect([lx+16, top, rw, 78], "#00000000", on_tap="bagselect", payload="{i}",
               on_swipe_left="bagnext", on_swipe_right="bagprev", swipe_px=80, **rep),
          # an empty pocket: the list says so (the empty-state pattern), the detail shows the pocket
          label([lx+lw//2, ly+lh//2-20, lw-40, 0], bind_text=t("bag_no_items"), scale=6, align="center", color=EMPTY,
                wrap_width=lw-40, max_lines=2, need_bind="lp.bag.empty")]
    # right: the selected item and what you can do with it
    dx, dw = lx + lw + 20, 1216 - (lx + lw + 20)
    w += panel([dx, ly, dw, lh], band="#FFF3C716")
    by = ly + lh - 120  # the action button (Use or Open in Bag): 104 tall, 16 above the panel's bottom
    s = 160  # the empty pocket's tile (60 px art), centred in the panel under the band
    for i in range(len(BAG_POCKETS)):
        w += pocket_icon(i, [dx+(dw-s)//2, ly+56+(lh-56-s)//2, s, s], False, need_bind=f"lp.bag.tab{i}.on",
                         hide_bind="lp.bag.empty", hide_eq=0)
    w += on_band(value([dx+dw-20, ly+14, 0, 0], "lp.money", scale=5, align="right", group=True))
    # name at 7 (the largest text), "In the Bag ×N" under it, then the description at 6: up to 7 lines
    # (the longest item text takes 6), ending at by-102, above the extra line (by-56) and the button
    full = {"hide_bind": "lp.bag.empty", "hide_eq": 1}
    w += [img([dx+20, ly+82, 128, 128], src_bind="lp.bag.selected.icon"),
          label([dx+166, ly+92, dw-186, 0], bind_text="lp.bag.selected.name", scale=7, fit_text=True,
                text_min_scale=5, wrap_width=dw-186, max_lines=2),
          label([dx+166, ly+164, dw-186, 0], bind_text="lp.bag.inbag", scale=5, color="#FF5A6070", fit_text=True,
                text_min_scale=4, wrap_width=dw-186, max_lines=1, _fit=["bag_in_bag"], **full),
          label([dx+24, ly+232, dw-44, 0], bind_text="lp.bag.description", scale=6, wrap_width=dw-44, max_lines=7)]
    assert ly+232 + 6*5 + 6*(6*5+6*3) <= by-56, "the description's 7th line reaches the extra line"
    bx, bw, bh = dx + 20, dw - 40, 104
    w += panel([bx, by, bw, bh], color="#FF538ABE", shadow=False, on_tap="baguse", need_bind="lp.bag.can_use")
    w += [label([bx+bw//2, by, bw-20, 0], bind_text=t("use"), scale=6, color=WHITE, align="center", fit_text=True,
                text_min_scale=5, wrap_width=bw-20, max_lines=1, text_center_h=bh, on_tap="baguse",
                need_bind="lp.bag.can_use")]
    # items only the game's own Bag can use: open that Bag on the item (main screen), in Use's place
    w += panel([bx, by, bw, bh], color="#FFE8A33C", shadow=False, on_tap="bagopen", need_bind="lp.bag.can_open")
    w += [label([bx+bw//2, by, bw-20, 0], bind_text=t("bag_open_in_bag"), scale=6, color=WHITE, align="center",
                fit_text=True, text_min_scale=5, wrap_width=bw-20, max_lines=1, text_center_h=bh, on_tap="bagopen",
                need_bind="lp.bag.can_open")]
    w.append(label([dx+24, by-56, dw-44, 0], bind_text="lp.bag.extra", scale=5, color=NAVY, wrap_width=dw-44,
                   max_lines=1, fit_text=True, text_min_scale=4, need_bind="lp.bag.extra.on",
                   _fit=["bag_repel_active", "bag_battery", "bag_bp"]))
    w += toast(dx+20, by-180, dw-40)
    scroll = {"id": "bagitems", "rect": [lx+16, top, rw, ly+lh-16-top], "count_bind": "lp.bag.count",
              "reset_bind": "lp.bag.pocket.id", "row_h": 84, "bar": "#FF8A96B9", "bar_w": 6,
              "show_bind": "!lp.bag.popup.any"}  # the bar would draw over the party pop-up
    w += field_navigation()
    w += bag_popup()
    return {"id": "field_bag", "widgets": w, "scrolls": [scroll]}


def page_inventory(view):
    # These pages are independent widgets, with no guest viewport or native menu driver.
    w=background()
    if view==5:
        return page_bag(w)
    w+=panel([24,16,1192,84])
    ball=ui("zukan","dex_deco_list_01_01")
    # title: the Pokédex icon (X-menu art) and the dex name
    w+=panel([36,28,400,58],color="#FFD8EAF4",shadow=False,on_tap="dexmode",need_bind="lp.pokedex.acquired")
    w+=[img([44,32,64,51],ui("menutop","menu_ico_01_01_on"),need_bind="lp.pokedex.acquired"),
        label([122,36,250,0],bind_text="lp.dex.title",scale=6,color=DARK,fit_text=True,text_min_scale=4,
              wrap_width=250,max_lines=1,need_bind="lp.pokedex.acquired",on_tap="dexmode"),
        label([420,44,36,0],text="<>",scale=3,color=NAVY,align="right",on_tap="dexmode",need_bind="lp.pokedex.acquired")]
    # seen / obtained: icon + count in a pill each, right-aligned next to Sort
    for px,src,key in ((452,"module:lp:dex/seen","lp.dex.seen"),(636,ball,"lp.dex.obtained")):
        w+=panel([px,28,168,58],color="#FFEDF1F6",shadow=False,need_bind="lp.pokedex.acquired")
        w+=[img([px+12,34,46,46],src,need_bind="lp.pokedex.acquired"),
            value([px+152,38,0,0],key,scale=6,align="right",need_bind="lp.dex.ready")]
    w+=panel([820,28,372,58],color="#FFD8EAF4",shadow=False,on_tap="dexsort",need_bind="lp.pokedex.acquired")
    # "Sort" on the left of the pill, the order on the right: each fits its half (down to 4)
    w+=[label([840,28,160,0],bind_text=t("dex_sort"),scale=6,color=DARK,outline=DARK,outline_px=1,fit_text=True,
              text_min_scale=4,wrap_width=160,max_lines=1,text_center_h=58,need_bind="lp.pokedex.acquired"),
        label([1172,28,170,0],bind_text="lp.dex.sort",scale=6,align="right",color=DARK,outline=DARK,outline_px=1,
              fit_text=True,text_min_scale=4,wrap_width=170,max_lines=1,text_center_h=58,
              on_tap="dexsort",need_bind="lp.pokedex.acquired",
              _fit=["dex_sort_number","dex_sort_az","dex_sort_caught"])]
    # the detail card (left, wide) and the species list (right, narrow); both run down to the nav bar
    cx, cy, cw, ch = DEX_CARD
    lx, ly, lw, lh = DEX_LIST
    w+=panel(DEX_CARD)+panel(DEX_LIST)
    # Entry / Weak. / Stats / Evolution / Area, swiped left / right (or tapped in the header)
    w+=page_tabs(cx+12,ly+12,cw-24,DEX_TABS,"lp.dexp","dexpage",suffix="")
    w.append(rect(DEX_RECT,"#00000000",on_swipe_left="dexpage10",on_swipe_right="dexpage11",swipe_px=70,
                  need_bind="lp.dex.valid"))
    x0, xr = DEX_X0, DEX_X0+DEX_W   # the tab pages' content: x0 .. xr
    # ---- Entry: the species, its profile, the Pokédex text and its forms (one scrolling page) ----
    ent = lambda ws, y_bind=None: scrolled(ws, "dexentry", "lp.dexp0", y_bind)
    hx, hw = DEX_HERO_X, xr-DEX_HERO_X  # the column right of the sprite
    w+=ent([img([x0,DEX_TOP+8,DEX_SPRITE,DEX_SPRITE],src_bind="lp.dex.selected.icon",y_bind=anim("dex","y"),
                scale_bind=anim("dex","s",gate=" && @scroll:dexentry == 0"),pivot=[DEX_SPRITE//2,DEX_SPRITE]),
            label([hx,DEX_TOP+12,hw,0],bind_text="lp.dex.selected.name",scale=7,fit_text=True,text_min_scale=6,
                  wrap_width=hw,max_lines=1),
            # a form's name (Heat Rotom, Sandy Cloak) under the species name; the category under that
            label([hx,DEX_TOP+70,hw,0],bind_text="lp.dex.form.name",scale=5,color=NAVY,fit_text=True,
                  text_min_scale=4,wrap_width=hw,max_lines=1,need_bind="lp.dex.form.on",_fit_kind="dex_form"),
            img([hx,DEX_TOP+150,184,40],src_bind="lp.dex.type1"),
            img([hx+196,DEX_TOP+150,184,40],src_bind="lp.dex.type2"),
            img([hx,DEX_TOP+204,40,40],src=ball,need_bind="lp.dex.state.caught"),
            img([hx,DEX_TOP+204,40,40],src="module:lp:dex/seen",hide_bind="lp.dex.state.caught",hide_eq=1),
            label([hx+52,DEX_TOP+211,hw-52,0],bind_text="lp.dex.selected.state",scale=5,wrap_width=hw-52,max_lines=1,
                  _fit=["dex_state_caught","dex_state_seen"])])
    w+=ent([label([hx,DEX_TOP+70,hw,0],bind_text="lp.dex.category",scale=5,color=GREY_TEXT,fit_text=True,
                  text_min_scale=4,wrap_width=hw,max_lines=1,_fit_kind="dex_category")],"lp.dex.cat.dy")
    # profile: height | weight | gender, captions at 5 over values at 6 on a light panel
    py = DEX_PROFILE_Y
    w+=ent(panel([x0,py,DEX_W,DEX_PROFILE_H],color=LIST_ROW,shadow=False))
    for (px_, pw), key, bind in ((DEX_CELLS[0],"dex_height","lp.dex.height"),(DEX_CELLS[1],"dex_weight","lp.dex.weight"),
                                 (DEX_CELLS[2],"dex_gender",None)):
        w+=ent([label([px_,py+14,pw,0],bind_text=t(key),scale=5,color=GREY_TEXT,fit_text=True,text_min_scale=4,
                      wrap_width=pw,max_lines=1)])
        if bind:
            w+=ent([label([px_,py+52,pw,0],bind_text=bind,scale=6,fit_text=True,text_min_scale=5,wrap_width=pw,
                          max_lines=1,_fit_kind="dex_"+key[4:])])
    gx, gw = DEX_CELLS[2]
    w+=ent([label([gx,py+56,gw//2,0],bind_text="lp.dex.g.mtext",scale=5,color=MALE_BLUE,wrap_width=gw//2,max_lines=1,
                  need_bind="lp.dex.g.bar",_fit_kind="dex_gender"),
            label([gx+gw,py+56,gw//2,0],bind_text="lp.dex.g.ftext",scale=5,color=FEMALE_RED,align="right",
                  need_bind="lp.dex.g.bar"),
            {"type":"bar","rect":[gx,py+96,gw,12],"bind":"lp.dex.g.male","max":1000,"color":MALE_BLUE,
             "bg":FEMALE_RED,"frame":0,"need_bind":"lp.dex.g.bar"},
            label([gx,py+52,gw,0],bind_text=t("dex_genderless"),scale=6,fit_text=True,text_min_scale=5,wrap_width=gw,
                  max_lines=1,need_bind="lp.dex.g.none")])
    # the Pokédex text at 6, as long as it is (the module measures it at DEX_W: lp.dex.fm.dy)
    w+=ent([label([x0,DEX_DESC_Y,DEX_W,0],bind_text="lp.dex.description",scale=6,wrap_width=DEX_W,max_lines=20)])
    # Forms: one chip per form the game has (Rotom, Shellos, Deoxys...); tap one to show it
    fy = DEX_DESC_Y+DEX_FORM_GAP
    w+=ent(section(x0,fy,DEX_W,"dex_forms"),"lp.dex.fm.dy")
    for widget in w[-3:]:
        widget["repeat_bind"] = "lp.dex.forms.any"
    fr={"repeat":DEX_FORM_MAX,"repeat_bind":"lp.dex.fm.n","repeat_cols":DEX_FORM_COLS,"repeat_dx":DEX_FORM_DY,
        "repeat_row_dy":DEX_FORM_DY,"scroll":"dexentry","y_bind":"lp.dex.fm.dy"}
    cy0, cs = fy+WK_SEC_H, DEX_FORM_CELL
    w+=panel([x0,cy0,cs,cs],color=LIST_ROW,shadow=False,**fr)
    w+=panel([x0,cy0,cs,cs],color=LIST_SEL,shadow=False,need_bind="lp.dex.fm{i}.sel",**fr)
    w+=[img([x0+8,cy0+8,cs-16,cs-16],src_bind="lp.dex.fm{i}.icon",**fr),
        rect([x0,cy0,cs,cs],"#00000000",on_tap="dexform",payload="{i}",**fr)]
    # Weak to / Resists / No effect: three columns each, the sections under the rows above them (offsets
    # from the counts); the page scrolls when it is taller than the card (measured by the module)
    k={"repeat":1,"repeat_bind":"lp.dexp1","scroll":"dexweak"}
    yr={"y_bind":"lp.dex.res.dy"}; yi={"y_bind":"lp.dex.imm.dy"}
    # section headers with room above them (WK_*: the module's WeakGridAt; positions for one row each)
    ry,iy=weak_grid_ys(DEX_WEAK_TOP)
    for y,key,title,colour,n,extra in ((DEX_WEAK_TOP,"weak","weak_to","#FFC83C32",18,{}),
                                       (ry,"res","resists","#FF3E4658",18,yr),(iy,"imm","no_effect",None,6,yi)):
        w+=section(x0,y,DEX_W,title,**k,**extra)
        w+=weak_tags(x0,y+WK_SEC_H,DEX_WEAK_DX,"lp.dex.",key,n,colour,
                     {"need_bind":"lp.dexp1","scroll":"dexweak",**extra},cols=DEX_WEAK_COLS)
        w+=[label([x0,y+WK_SEC_H+4,0,0],bind_text=t("nothing"),scale=5,color=GREY_TEXT,repeat=1,
                  repeat_bind=f"lp.dex.{key}.none",scroll="dexweak",hide_bind="lp.dexp1",hide_eq=0,_avail=DEX_W,
                  **extra)]
    # Stats page: base stats, total, the species' abilities, then Training and Breeding; one scrolling list
    rs={"repeat":7,"repeat_bind":"lp.dex.bs.count","repeat_dy":62,"scroll":"dexstats","need_bind":"lp.dexp2"}
    w+=[label([x0,212,180,0],bind_text="lp.dex.bs{i}.name",scale=5,fit_text=True,text_min_scale=4,wrap_width=180,
              max_lines=1,_fit=["hp","stat_attack","stat_defense","stat_sp_atk","stat_sp_def","stat_speed",
                                "stat_total"],**rs),
        value([x0+256,212,0,0],"lp.dex.bs{i}.v",scale=5,align="right",**rs),
        {"type":"bar","rect":[x0+272,218,DEX_W-272,22],"bind":"lp.dex.bs{i}.v","max_bind":"lp.dex.bs{i}.max",
         "color":"#FF5E98D4","bg":"#FFE0E4EC","frame":0,**rs}]
    # all abilities as one flowing text (navy names, grey descriptions): no gap after a short one
    rb={"repeat":1,"repeat_bind":"lp.dex.ab.any","scroll":"dexstats","need_bind":"lp.dexp2"}
    w+=section(x0,DEX_AB_Y,DEX_W,"abilities",**rb)
    w+=[label([x0,DEX_AB_Y+WK_SEC_H,DEX_W,0],bind_text="lp.dex.ab.text",scale=5,wrap_width=DEX_W,max_lines=40,
              color="#FF5A6070",color_markup=True,**rb)]
    # Training and Breeding under the abilities (lp.dex.tr.dy: the abilities text's height), a caption at 5
    # and the value at 6 on each row
    ty = DEX_TRAIN_Y
    rows = [("dex_training", [("dex_ev_yield","ev"),("dex_catch_rate","catch"),("dex_friendship","friend"),
                              ("dex_base_exp","exp"),("dex_growth","growth")]),
            ("dex_breeding", [("dex_egg_groups","egg"),("dex_hatch","hatch")])]
    vx = x0+DEX_TR_LABEL_W+16
    for title, items in rows:
        w+=scrolled(section(x0,ty,DEX_W,title),"dexstats","lp.dexp2","lp.dex.tr.dy")
        ty+=WK_SEC_H
        for key, bind in items:
            fit = {"_fit_kind": f"dex_{bind}"} if bind in ("growth","egg","ev","hatch") else {}
            # the EV yield (up to three stats) takes the whole width on a line of its own
            tall = bind == "ev"
            vx_, vy = (x0, ty+DEX_TR_TALL) if tall else (vx, ty)
            w+=scrolled([label([x0,ty+4,DEX_TR_LABEL_W,0],bind_text=t(key),scale=5,color=PANE_LABEL,fit_text=True,
                               text_min_scale=4,wrap_width=DEX_TR_LABEL_W,max_lines=1),
                         label([vx_,vy,xr-vx_,0],bind_text=f"lp.dex.tr.{bind}",scale=6,fit_text=True,text_min_scale=5,
                               wrap_width=xr-vx_,max_lines=1,**fit)],"dexstats","lp.dexp2","lp.dex.tr.dy")
            ty+=DEX_TR_DY+(DEX_TR_TALL if tall else 0)
        ty+=DEX_TR_GAP
    assert ty-DEX_TR_GAP-DEX_TR_DY+40 == DEX_STATS_END, (ty, DEX_STATS_END)
    # Evolution: the whole chain as a tree (depth = indent), each member's way in navy under its name;
    # the selected species highlighted; tap a member to show it (one not seen yet is a silhouette)
    ev={"repeat":DEX_EVO_MAX,"repeat_bind":"lp.dex.ev.n","repeat_dy":DEX_EVO_DY,"scroll":"dexevo"}
    e="lp.dex.ev{i}."
    ey, eh = DEX_TOP+8, DEX_EVO_DY-8
    nx = x0+16+DEX_EVO_ICON+12
    w+=panel([x0,ey,DEX_W,eh],color=LIST_ROW,shadow=False,**ev)
    w+=panel([x0,ey,DEX_W,eh],color=LIST_SEL,shadow=False,need_bind=e+"sel",**ev)
    w+=[rect([x0+16-28,ey+20,4,eh-40],NAVY,x_bind=e+"dx",need_bind=e+"child",**ev),
        img([x0+16,ey+(eh-DEX_EVO_ICON)//2,DEX_EVO_ICON,DEX_EVO_ICON],src_bind=e+"icon",x_bind=e+"dx",
            need_bind=e+"seen",**ev),
        img([x0+16,ey+(eh-DEX_EVO_ICON)//2,DEX_EVO_ICON,DEX_EVO_ICON],src_bind=e+"icon",x_bind=e+"dx",
            color=SILHOUETTE,hide_bind=e+"seen",hide_eq=1,**ev),
        label([nx,ey+10,DEX_EVO_TEXT_W,0],bind_text=e+"name",scale=6,fit_text=True,text_min_scale=5,
              wrap_width=DEX_EVO_TEXT_W,max_lines=1,x_bind=e+"dx",_fit_kind="dex_evo_name",**ev),
        # the way in at 5 when it fits two lines there (the module measures it: .big), else at 4
        label([nx,ey+54,DEX_EVO_TEXT_W,0],bind_text=e+"cond",scale=5,color=NAVY,wrap_width=DEX_EVO_TEXT_W,
              max_lines=2,x_bind=e+"dx",need_bind=e+"big",**ev),
        label([nx,ey+54,DEX_EVO_TEXT_W,0],bind_text=e+"cond",scale=4,color=NAVY,wrap_width=DEX_EVO_TEXT_W,
              max_lines=2,x_bind=e+"dx",hide_bind=e+"big",hide_eq=1,_fit_kind="dex_evo",**ev),
        rect([x0,ey,DEX_W,eh],"#00000000",on_tap="dexevo",payload="{i}",**ev)]
    w+=scrolled([label([x0+DEX_W//2,ey+DEX_EVO_DY+16,DEX_W,0],bind_text=t("evo_none"),scale=5,color=GREY_TEXT,
                       align="center",wrap_width=DEX_W,max_lines=1)],"dexevo","lp.dex.ev.none")
    # Area: where it lives in the wild, a row per place and way (as the Route page); tap a row to see
    # the place on the Map
    ar={"repeat":DEX_AREA_MAX,"repeat_bind":"lp.dex.ar.n","repeat_dy":DEX_AREA_DY,"scroll":"dexarea"}
    a_="lp.dex.ar{i}."
    ah = DEX_AREA_DY-8
    aw = DEX_W-40-56
    w+=panel([x0,ey,DEX_W,ah],color=LIST_ROW,shadow=False,**ar)
    w+=[label([x0+20,ey+12,aw,0],bind_text=a_+"name",scale=6,fit_text=True,text_min_scale=5,wrap_width=aw,
              max_lines=1,_fit_kind="dex_area",**ar),
        label([x0+20,ey+56,aw,0],bind_text=a_+"way",scale=5,color=NAVY,wrap_width=aw,max_lines=2,
              need_bind=a_+"big",**ar),
        label([x0+20,ey+56,aw,0],bind_text=a_+"way",scale=4,color=NAVY,wrap_width=aw,max_lines=2,
              hide_bind=a_+"big",hide_eq=1,_fit_kind="dex_area_way",**ar),
        img([xr-58,ey+(ah-56)//2,42,56],ui("menutop","menu_ico_01_06_on"),need_bind=a_+"map",**ar),
        rect([x0,ey,DEX_W,ah],"#00000000",on_tap="dexarea",payload="{i}",**ar)]
    w+=scrolled([label([x0+DEX_W//2,DEX_TOP+300,DEX_W,0],bind_text=t("dex_area_none"),scale=6,color=EMPTY,
                       align="center",wrap_width=DEX_W,max_lines=2)],"dexarea","lp.dex.ar.none")
    # list rows: DEX_DY apart, an 80 px row and an 88 px tap target; the number at 5, the name at 6 (min 5),
    # the caught ball at the end
    rep={"repeat":1010,"repeat_bind":"lp.dex.count","repeat_dy":DEX_DY,"scroll":"dexlist"};r="lp.dex{i}."
    rx, rw = lx+12, lw-24
    w+=panel([rx,ly+16,rw,DEX_DY-8],color=LIST_ROW,shadow=False,**rep)
    w+=panel([rx,ly+16,rw,DEX_DY-8],color=LIST_SEL,shadow=False,need_bind=r+"selected",**rep)
    w+=[
        # the number fits its column: Japanese draws digits wider (vanilla BD's jpn face)
        label([rx+12,ly+42,DEX_NAME_X-18,0],bind_text=r+"number",scale=5,fit_text=True,text_min_scale=3,
              wrap_width=DEX_NAME_X-18,max_lines=1,**rep),
        label([rx+DEX_NAME_X,ly+38,DEX_NAME_W,0],bind_text=r+"name",scale=6,wrap_width=DEX_NAME_W,fit_text=True,
              text_min_scale=5,max_lines=1,**rep),
        img([rx+rw-40,ly+40,32,32],ball,need_bind=r+"obtained",**rep),
        rect([rx,ly+12,rw,DEX_DY],"#00000000",on_tap="dexselect",payload="{i}",**rep)]
    # Locked acquisition state uses one clear card, without an empty split behind the notice.
    w+=panel([24,16,1192,926],need_bind="lp.pokedex.unavailable")
    w+=[label([620,440,1100,0],bind_text=t("dex_not_available"),scale=7,align="center",wrap_width=1100,max_lines=2,need_bind="lp.pokedex.unavailable")]
    bar={"bar":"#FF8A96B9","bar_w":6}
    scrolls=[{"id":"dexlist","rect":[lx,ly+8,lw,lh-16],"count_bind":"lp.dex.count","reset_bind":"lp.dex.sort.id",
              "follow_bind":"lp.dex.selected.row","row_h":DEX_DY,**bar}]
    # the tab pages: lengths in 10 px rows, measured by the module; a new species starts at the top
    for k,sid in enumerate(("dexentry","dexweak","dexstats","dexevo","dexarea")):
        scrolls.append({"id":sid,"rect":DEX_RECT,"count_bind":f"lp.dex.{sid[3:]}.rows","row_h":10,
                        "reset_bind":"lp.dex.selected.key","show_bind":f"lp.dexp{k}",**bar})
    w+=field_navigation()
    return {"id":"pokedex","widgets":w,"scrolls":scrolls}


def page_route():
    # the title in the band (as on the Map); a toolbar row under it (the subtitle, Fly here on the
    # right); then the encounter cards. The Map nav tab goes back to the map.
    ty = 86        # the toolbar row (88 tall)
    top = ty + 104  # the cards
    w = background()
    w += panel([24, 22, W-48, 928], band="#FFF3C716") + on_band(
        label([48, 32, 0, 0], bind_text="lp.route.name", scale=6)) + [
        label([48, ty, W-48-200-16-48, 0], bind_text=t("route_subtitle"), scale=4, fit_text=True, text_min_scale=3,
              wrap_width=W-48-200-16-48, max_lines=1, text_center_h=88)]
    # empty states (one pattern on every page): centred in the cards' area, 6 in grey, a hint at 5
    ey = (top + 936) // 2 - 40
    w += [label([W//2, ey, 1100, 0], bind_text=t("route_reading"), scale=6, color=EMPTY, align="center",
                wrap_width=1100, max_lines=1, hide_bind="lp.route.ready", hide_eq=1)]
    none = {"need_bind": "lp.route.ready", "hide_bind": "lp.route.count", "keep_max": 0}
    w += [label([W//2, ey, 1100, 0], bind_text=t("route_none"), scale=6, color=EMPTY, align="center",
                wrap_width=1100, max_lines=1, **none),
          label([W//2, ey+54, 1100, 0], bind_text=t("route_none_hint"), scale=5, color=EMPTY_HINT,
                align="center", wrap_width=1100, max_lines=1, **none)]
    # a reached town Fly can take you to (the module checks badge, HM and arrival)
    w += fly_button(W-48-200, ty, "fly", "lp.route.fly", key="fly_here", width=200)
    rep = {"repeat": 128, "repeat_bind": "lp.route.count", "repeat_cols": 4,
           "repeat_dx": 291, "repeat_row_dy": 270, "scroll": "route"}
    p = "lp.route{i}."
    w += panel([48, top, 271, 256], color="#FFFAF6E8", shadow=False, **rep) + [
        img([129, top+8, 108, 108], src_bind=p+"icon", **rep),
        label([183, top+122, 255, 0], bind_text=p+"name", scale=6, align="center", fit_text=True,
              text_min_scale=4, wrap_width=255, **rep),
        label([183, top+174, 255, 0], bind_text=p+"level", scale=5, align="center", wrap_width=255, max_lines=1,
              _fit=["route_level_range"], **rep),
        # the method at body size in navy ("Poké Radar required", the widest, is 248 px at 5)
        label([183, top+212, 255, 0], bind_text=p+"method", scale=5, color=NAVY, align="center",
              wrap_width=255, max_lines=1, fit_text=True, text_min_scale=4,
              _fit=["route_grass", "route_grass_day", "route_grass_night", "route_swarm", "route_radar_required"],
              **rep)]
    w += toast(W//2-360, 860, 720)
    w += field_navigation()
    return {"id": "route", "widgets": w, "scrolls": [
        {"id": "route", "rect": [48, top, 1144, 936-top], "count_bind": "lp.route.rows",
         "row_h": 270, "bar": "#FF8A96B9", "bar_w": 6}]}


NAVY = "#FF35567A"
PARTY_DESC_W = 536  # the party card's ability description width (module: PartyDescW)
# The Pokédex page (module: the Dex* constants of the same names). The detail card on the left is the wide
# one, the species list on the right only as wide as a number, a name and the caught ball.
DEX_CARD, DEX_LIST = [24, 124, 776, 818], [816, 124, 400, 818]
DEX_TABS = ["dex_tab_entry", "tab_weak", "stats", "dex_tab_evo", "dex_area"]
DEX_RECT_TOP = 190                     # the tab pages' scroll region (DexRectTop)
DEX_RECT = [24, DEX_RECT_TOP, 776, 748]
DEX_TOP = 196                          # the pages' content starts at DEX_TOP + 8
DEX_X0, DEX_W = 44, 736                # the pages' content column (DexW: the Pokédex text's width)
DEX_DY, DEX_NAME_X, DEX_NAME_W = 88, 96, 238   # list rows: pitch (80 px row), the name's x and room
DEX_SPRITE, DEX_HERO_X = 240, 300      # Entry: the sprite, the column right of it
DEX_PROFILE_Y, DEX_PROFILE_H = 460, 124
DEX_CELLS = [(64, 176), (252, 196), (464, 308)]  # height, weight, gender: x and width
DEX_DESC_Y = 604                       # the Pokédex text (DexDescY)
DEX_FORM_GAP, DEX_FORM_CELL, DEX_FORM_DY, DEX_FORM_COLS, DEX_FORM_MAX = 44, 112, 124, 6, 32
DEX_WEAK_COLS, DEX_WEAK_DX = 3, 248    # the Weak. page's grid (DexWeakCols)
DEX_AB_Y = 660                         # Stats: the Abilities header (its text WK_SEC_H below)
DEX_TRAIN_Y = DEX_AB_Y + 50 + 44       # the Training header for an empty abilities text (+ lp.dex.tr.dy)
DEX_TR_LABEL_W, DEX_TR_DY, DEX_TR_GAP, DEX_TR_TALL = 250, 56, 30, 40
DEX_STATS_END = 1300                   # the Breeding rows' bottom for an empty abilities text (DexStatsEnd)
DEX_EVO_DY, DEX_EVO_ICON, DEX_EVO_INDENT, DEX_EVO_MAX = 128, 88, 56, 16
DEX_EVO_TEXT_W = DEX_W - 16 - DEX_EVO_ICON - 12 - 16 - 2 * DEX_EVO_INDENT  # name / way at the deepest indent
DEX_AREA_DY, DEX_AREA_MAX = 136, 96
MALE_BLUE, FEMALE_RED = "#FF2F6CC0", "#FFC8323C"
SILHOUETTE = "#FF5A6070"               # an evolution not seen yet
GREY_TEXT = "#FF6A6E78"  # the lightest grey for text on cream
# The Weak to / Resists / No effect grids (Pokédex Weak. tab, party card Weak. tab; module: Wk*, WeakGridAt):
# a section header, the type tags WK_SEC_H below it, two to a row WK_ROW_DY apart, the next header WK_GAP
# after the last row (40 px under its tags). An empty section keeps one row ("Nothing"). The manifest
# places Resists / No effect for one row each; the module publishes the offsets and the scroll length.
WK_SEC_H, WK_ROW_DY, WK_GAP, WK_TAG_H, WK_PAD = 50, 50, 26, 36, 12
DEX_WEAK_TOP = 204                            # screen y (module: DexWeakTop)
PARTY_WEAK_TOP, PARTY_WEAK_RECT = 444, 392    # from the party card's top (module: PartyWeakTop, PartyWeakRect)


def weak_grid_ys(top):
    """The Resists and No effect header y for one row in each section above them."""
    res_y = top + WK_SEC_H + WK_ROW_DY + WK_GAP
    return res_y, res_y + WK_SEC_H + WK_ROW_DY + WK_GAP


def weak_tags(x, y, dx, p, key, n, colour, extra, cols=2):
    """The Weak. tabs' type tags with their factor, `cols` to a row (the party card 2, the Pokédex
    DEX_WEAK_COLS): 166x36 tags, the factor at body size right after its tag, 4× / ¼× bold in their
    own colour."""
    g = {"repeat": n, "repeat_bind": p+key+".count", "repeat_cols": cols, "repeat_dx": dx, "repeat_row_dy": WK_ROW_DY,
         **extra}
    out = [img([x, y, 166, WK_TAG_H], src_bind=p+key+"{i}.tag", **g)]
    if colour:
        f, s = p+key+"{i}.factor", p+key+"{i}.strong"
        out += [label([x+172, y+6, 0, 0], bind_text=f, scale=5, color=colour, hide_bind=s, hide_eq=1, **g),
                label([x+172, y+6, 0, 0], bind_text=f, scale=5, color=colour, outline=colour, outline_px=1,
                      hide_bind=s, hide_eq=0, **g)]
    return out


def page_tabs(x, y, w, names, bind_prefix, action, suffix=".on", gate=None, tap_top=16):
    """The header of swipeable card pages: text tabs on a hairline, the current one bold with a thick
    underline; any page can be tapped (the swipe area itself is added by the caller).
    gate: a bind that must be 1 for this header to show (two headers with different tab counts).
    tap_top: how far above the labels the (invisible) tap target starts; it ends 54 px below them."""
    need = {"need_bind": gate} if gate else {}
    hide = {"hide_bind": gate, "hide_eq": 0} if gate else {}
    out = [rect([x, y+47, w, 2], "#FFD8DCE4", **need)]
    seg = w // len(names)
    under = min(120, seg - 8)  # four tabs in a narrow card: the underline stays inside its tab
    for k, name in enumerate(names):
        sx, on, tap = x + k * seg, f"{bind_prefix}{k}{suffix}", f"{action}{k}"
        # Motion: the new tab's underline grows out from its centre while the old one shrinks into
        # its own (a moving clip over a 6 px strip: crisp, and cheap to redraw).
        mid = f"tabmid.{bind_prefix}{k}"
        grow = {"anim": {"bind": on, "from": f"widget:{mid}", "ms": TAB_ANIM_MS, "easing": "ease_out"}}
        # both states at the same size and place, so the text does not jump; the current one is bold
        fit = {"fit_text": True, "text_min_scale": 4, "wrap_width": seg - 12, "max_lines": 1, "text_center_h": 25}
        out += [rect([sx, y-tap_top, seg, tap_top+54], "#00000000", on_tap=tap, **need),
                label([sx + seg//2, y+8, 0, 0], bind_text=t(name), scale=5, align="center", color="#FF6A6E78",
                      hide_bind=on, hide_eq=1, **fit, **need),
                label([sx + seg//2, y+8, 0, 0], bind_text=t(name), scale=5, align="center", color=DARK, outline=DARK,
                      outline_px=1, need_bind=on, **fit, **hide),
                rect([sx+seg//2-2, y+42, 4, 6], "#00000000", id=mid),
                rect([sx+seg//2-under//2, y+42, under, 6], "#FFE8A33C", need_bind=on, **hide, **grow)]
    return out


def card_button(r, text_or_bind, action, need, scale=4):
    """A button on a card: a white chip with a navy outline (the party card's field moves)."""
    x, y, w, h = r
    text = dict(bind_text=text_or_bind) if text_or_bind.startswith("lp.") else dict(bind_text=t(text_or_bind))
    common = dict(on_tap=action, need_bind=need)
    return panel([x, y, w, h], color=NAVY, shadow=False, **common) + \
        panel([x+3, y+3, w-6, h-6], color=WHITE, shadow=False, **common) + [
        label([x+w//2, y+(h-5*scale)//2-2, w-12, 0], scale=scale, color=NAVY, align="center", fit_text=True,
              text_min_scale=max(4, scale-1), wrap_width=w-12, max_lines=1, **text, **common)]


GROUP_DERIVED = {}


def gated_group(widgets, bind):
    """Gate every widget in a group while preserving its existing theme/state conditions."""
    for widget in widgets:
        need = widget.get("need_bind")
        if need == bind:
            continue
        if need:
            gate = bind + ".with." + need.replace("!", "not.")
            # Derived expressions are evaluated before repeated widgets expand {i}.
            # Publish one concrete gate per row; the widget can keep its template.
            indices = range(widget.get("repeat", 1)) if "{i}" in need else [None]
            for index in indices:
                row_gate = gate.replace("{i}", str(index)) if index is not None else gate
                row_need = need.replace("{i}", str(index)) if index is not None else need
                row_bind = bind.replace("{i}", str(index)) if index is not None else bind
                GROUP_DERIVED[row_gate] = {"name": row_gate, "expr": f"{row_bind} == 1 && ({row_need}) != 0"}
            widget["need_bind"] = gate
        else:
            widget["need_bind"] = bind
    return widgets


def page_field():
    """Direction B: native party plates on the left and selected-member card on the right."""
    w = background()
    card_start = len(w)
    for i in range(6):
        w += gated_group(party_plate(30, 22+i*150, 550, 132, f"lp.p{i}.", slot=i), f"lp.p{i}.valid")
    w.append(label([36, 916, 538, 0], bind_text=t("party_touch_hint"), scale=3,
                   color=GREY_TEXT, wrap_width=538, max_lines=2, need_bind="lp.party.selected"))
    w.append(rect([0, 0, 610, 944], "#00000000", input_block=True, need_bind="lp.party.reordering"))
    x, y, width = 618, 22, 592
    w += panel([x, y, width, 922], band="#FFF3C716") + on_band(
        label([x+24, y+12, width-190, 0], bind_text="lp.sel.name", scale=6,
              fit_text=True, text_min_scale=5, wrap_width=width-190, max_lines=1),
        label([x+width-144, y+17, 0, 0], bind_text=t("level_prefix"), scale=5, _avail=60, need_bind="lp.sel.details"),
        value([x+width-24, y+17, 0, 0], "lp.sel.level", scale=5, align="right", need_bind="lp.sel.details")) + [
        img([x+12, y+68, 210, 210], src_bind="lp.sel.icon", y_bind=anim("sel", "y"),
            scale_bind=anim("sel", "s", gate=" && @drag == 0"), pivot=[105, 210]),
        img([x+242, y+78, 148, 32], src_bind="lp.sel.tag1"),
        img([x+404, y+78, 148, 32], src_bind="lp.sel.tag2")]
    detail_start = len(w)
    # the held item beside the types' column: caption in grey, icon, name (two lines at 4 when long)
    lx, vx = x+242, x+340
    iy = y+126
    # the caption fits the 90 px before the icon (down to 3)
    w += [label([lx, iy-4, 90, 0], bind_text=t("party_held_item"), scale=4, color="#FF6A6E78", fit_text=True,
                text_min_scale=3, wrap_width=90, max_lines=1, text_center_h=20),
          img([vx-4, iy-10, 40, 40], src_bind="lp.sel.itemicon"),
          label([vx+42, iy-2, width-(vx-x)-66, 0], bind_text="lp.sel.itemname", scale=5, fit_text=True,
                text_min_scale=4, wrap_width=width-(vx-x)-66, max_lines=2)]
    # the status under the Pokémon (the right column holds the field moves)
    w.append(label([x+117, y+284, 220, 0], bind_text="lp.sel.status", scale=5, color="#FF5A6070", align="center",
                   fit_text=True, text_min_scale=4, wrap_width=220, max_lines=1,
                   _fit=["healthy", "status_asleep", "status_poisoned", "status_burned", "status_frozen",
                         "status_paralyzed", "fainted"]))
    # the field moves this Pokémon can use (HP is on its plate on the left): Teleport, Dig, Flash...,
    # up to four in two rows of two (e.g. Soft-Boiled, Flash, Teleport, Sweet Scent); they end above
    # the tabs' tap targets (y+324)
    # (from 10 px right of the picture: "Sweet Scent" is 147 px at 5)
    fx = x+232
    cwid, chh = (x + width - 24 - fx - 12) // 2, 64
    for k in range(4):
        w += card_button([fx + (k % 2)*(cwid+12), y+186 + (k // 2)*(chh+8), cwid, chh], f"lp.sel.fw{k}.name",
                         f"fwuse{k}", f"lp.sel.fw{k}.on", scale=5)
    # Lower card: Stats / Moves / Weaknesses, swiped left / right (or tapped in the header)
    w += page_tabs(x+20, y+332, width-40, ["stats", "moves", "tab_weak"], "lp.ftab", "ftab", tap_top=8)
    w.append(rect([x+20, y+392, width-40, 520], "#00000000", on_swipe_left="ftab10", on_swipe_right="ftab11", swipe_px=80))
    # Stats: a 2 x 3 grid (max HP first), then the ability and what it does
    st = {"need_bind": "lp.ftab0.on"}
    cw, ch = (width-48-12) // 2, 72
    for k, (name, stat) in enumerate((("hp", "hpmax"), ("stat_attack", "atk"), ("stat_defense", "def"),
                                      ("stat_sp_atk", "spa"), ("stat_sp_def", "spd"), ("stat_speed", "spe"))):
        cx, cy = x+24 + (k % 2) * (cw+12), y+404 + (k // 2) * (ch+8)
        # the name fits what the value (up to 3 digits at 6) leaves
        nw = cw - 20 - 20 - 72
        w += panel([cx, cy, cw, ch], color="#FFF0EAD6", shadow=False, **st) + [
            label([cx+20, cy, nw, 0], bind_text=t(name), scale=5, color="#FF5A6070", fit_text=True, text_min_scale=4,
                  wrap_width=nw, max_lines=1, text_center_h=ch, **st),
            value([cx+cw-20, cy+16, 0, 0], "lp.sel."+stat, scale=6, align="right", **st)]
    ay = y+404 + 3*(ch+8) + 10
    # a section rule so the ability reads as its own part of the card, with room above it
    ay += 18
    w += title_rule(x+32, ay-4, ay+12, x+width-28, "ability", 5, 300, gap=8, **st)
    w += [label([x+28, ay+44, width-56, 0], bind_text="lp.sel.ability", scale=6, fit_text=True, text_min_scale=4,
                wrap_width=width-56, **st),
          # at body size when it fits three lines (most do), else at 4: the module measures it at
          # PARTY_DESC_W (lp.sel.ability.big)
          label([x+28, ay+96, width-56, 0], bind_text="lp.sel.ability.desc", scale=5, wrap_width=width-56,
                max_lines=3, hide_bind="lp.sel.ability.big", hide_eq=0, **st),
          label([x+28, ay+96, width-56, 0], bind_text="lp.sel.ability.desc", scale=4, wrap_width=width-56,
                max_lines=5, hide_bind="lp.sel.ability.big", hide_eq=1, **st)]
    assert width-56 == PARTY_DESC_W, "the module measures the ability description at this width"
    # (field moves are used from the chips in the header, not from these rows)
    for widget in move_bar(x+24, y+404, width-48, 112, "lp.sel.m{i}.", on_tap="partymove"):
        if widget.get("on_tap") == "partymove":
            widget.update(payload="$lp.sel.m{i}.payload", draggable=True,
                          drag_scale=1.04, drop_action="moveswap{i}")
        widget.update(hide_bind="lp.ftab1.on", hide_eq=0, repeat=4, repeat_dy=124)
        w.append(widget)
    # Weak.: the caption, then Weak to / Resists / No effect as on the Pokédex (two to a row, the later
    # sections under the rows above them); the whole tab scrolls when it is taller than the card (only
    # repeat rows follow a scroll: single widgets are one-row lists)
    sc = {"scroll": "weak", "need_bind": "lp.ftab2.on"}
    one = {"repeat": 1, "repeat_bind": "lp.ftab2.on", **sc}
    w += [label([x+28, y+404, width-56, 0], bind_text=t("party_not_included"), scale=4, color=GREY_TEXT,
                fit_text=True, text_min_scale=3, wrap_width=width-56, max_lines=1, **one)]
    ry, iy = weak_grid_ys(PARTY_WEAK_TOP)
    for top, key, title, colour, n, extra in (
            (PARTY_WEAK_TOP, "weak", "weak_to", "#FFC83C32", 18, {}),
            (ry, "res", "resists", RESIST_GREY, 18, {"y_bind": "lp.sel.res.dy"}),
            (iy, "immune", "no_effect", None, 6, {"y_bind": "lp.sel.imm.dy"})):
        w += section(x+28, y+top, width-56, title, **one, **extra)
        w += weak_tags(x+28, y+top+WK_SEC_H, 272, "lp.sel.", key, n, colour, {**sc, **extra})
        w += [label([x+28, y+top+WK_SEC_H+4, 0, 0], bind_text=t("nothing"), scale=5, color=GREY_TEXT, repeat=1,
                    repeat_bind=f"lp.sel.{key}.none", _avail=width-56, **sc, **extra)]
    w += [label([x+28, y+PARTY_WEAK_TOP+WK_SEC_H+4, width-56, 0], bind_text=t("party_no_types"), scale=5,
                color=GREY_TEXT, fit_text=True, text_min_scale=4, wrap_width=width-56, max_lines=1,
                repeat=1, repeat_bind="lp.sel.weak.unavailable", **sc)]
    # the first field move (Soft-Boiled / Milk Drink share HP; Dig, Teleport, Flash... run in the game)
    w += toast(x+20, y+840, width-40)
    # Eggs retain only their generic identity and native Egg icon. Hide summary controls too.
    gated_group(w[detail_start:], "lp.sel.details")
    w.append(label([x+244, y+148, width-268, 0], bind_text=t("party_egg_info"), scale=5,
                   color=GREY_TEXT, wrap_width=width-268, max_lines=5, need_bind="lp.sel.egg"))
    # Hide all member-card content, including tabs and touch targets, when there is no member.
    gated_group(w[card_start:], "lp.party.selected")
    for key, gate in (("party_empty", "lp.party.empty"), ("party_unavailable", "lp.party.unavailable")):
        w += panel([24, 22, W-48, 922], need_bind=gate)
        w += [label([W//2, 440, 1100, 0], bind_text=t(key), scale=7, align="center",
                    wrap_width=1100, max_lines=2, need_bind=gate)]
    w += field_navigation()
    w += bag_popup("lp.bag.popup.title.f")
    # Inspection is local companion state; it never opens a native menu or uses a field move.
    popup = [rect([0, 0, W, H], "#99101420", input_block=True)]
    popup += panel([150, 170, W-300, 680])
    # Reuse the Party tab's exact 536x112 move card, including current/max PP.
    popup += move_bar((W-536)//2, 190, 536, 112, "lp.party.move.")
    popup += [img([180, 340, 64, 40], src_bind="lp.party.move.category"),
              label([280, 340, 180, 0], bind_text=t("power"), scale=5),
              label([470, 340, 130, 0], bind_text="lp.party.move.power", scale=6),
              label([640, 340, 240, 0], bind_text=t("accuracy"), scale=5),
              label([900, 340, 140, 0], bind_text="lp.party.move.accuracy", scale=6),
              label([180, 410, W-360, 0], bind_text="lp.party.move.desc", scale=5,
                    wrap_width=W-360, max_lines=7)]
    popup += panel([450, 746, 340, 76], color="#FF538ABE", shadow=False, on_tap="partymoveclose")
    popup += [label([620, 746, 300, 0], bind_text=t("party_popup_close"), scale=6, color=WHITE,
                    align="center", text_center_h=76, on_tap="partymoveclose")]
    gated_group(popup, "lp.party.move.on")
    w += popup
    return {"id": "field", "widgets": w,
            # the module's length: to the last row's bottom + WK_PAD, in 10 px rows from the rect's top
            "scrolls": [{"id": "weak", "rect": [x+24, y+PARTY_WEAK_RECT, width-48, 912-PARTY_WEAK_RECT],
                         "count_bind": "lp.sel.weak.rows", "row_h": 10, "show_bind": "lp.weak.scroll",
                         "bar": "#FF8A96B9", "bar_w": 6}]}


# Equal top status cards; party and native commands begin below them.
STRIP_W = 700
TYPE_COLORS = ["#FFA3A39F", "#FFFFA202", "#FF95C9FF", "#FF994DCF", "#FFAB7939", "#FFBCB889",
               "#FF9FA424", "#FF6E4570", "#FF6AAED3", "#FFFF612C", "#FF2992FF", "#FF42BF24",
               "#FFFFDB00", "#FFFF637F", "#FF42BFFF", "#FF5462D6", "#FF504C4B", "#FFFFB1FF"]
CMD_X = [800, 768, 768, 800]           # left edges: a gentle arc
CMD_W, CMD_H, CMD_GAP = 410, 118, 22
BODY_Y = 236
BALL_Y, BALL_H = BODY_Y, 104
# Keep the approved battle command positions.
CMD_Y0 = 419
MOVE_PANE_W, MOVE_W = 496, 640
MOVE_X = [570, 546, 546, 570]
# a move button's PP: large on the right, its right edge MOVE_PP_GAP px before the button's; the name
# and the effectiveness line wrap before its widest value
MOVE_PP_SCALE, MOVE_PP_GAP = 7, 36
MOVE_PP_W = max(text_width(f"{n}/{n}", MOVE_PP_SCALE) for n in ("40", "48", "64", "88"))
MOVE_EFF_GREY = "#FF3A3F4A"  # "Effective", "No effect", "1× · ½×" on the button's light end
# inside a move button (CMD_H tall; its frame leaves about y+19..y+104): the name (6) and the
# effectiveness (5) as one block; the PP (7) on the name's line, so the A key (bottom corner) fits under it
MOVE_NAME_Y, MOVE_EFF_Y, MOVE_PP_Y = 20, 60, 26
BACK_W, BACK_H = 350, 88
BODY_BOTTOM = BODY_Y + 800
BACK_Y = BODY_BOTTOM - BACK_H
MOVE_SHORTCUT_GAP = CMD_Y0 - (BALL_Y + BALL_H)
MOVE_Y0 = BACK_Y - MOVE_SHORTCUT_GAP - (4 * CMD_H + 3 * CMD_GAP)
# the command stack in the game's own order (its cursor index): Battle, Pokémon, Bag, Run
COMMANDS = [("cmd_battle", 1, "go_moves"), ("cmd_pokemon", 2, "cmd_pokemon"), ("bag", 3, "cmd_bag"),
            ("cmd_run", 4, "cmd_run")]


# ---- battle left pane: Foe / You / Field, swiped or tapped, each page scrolls -------------------
# Layout constants shared with the module (0100000011D90000.cpp, PublishPane): px from the content
# top. Sections after a list of variable length are placed by y_bind offsets the module publishes.
PANE_PAD, SEC_H = 12, 50
COND_Y, FOE_TOP, GRID_DY = 132, 252, 46
STATS_TOP, OWN_STAT_DY, CHIP_DY = 200, 52, 98
MOVE_DESC_Y = 470
FOE_MOVE_DY = 50
FLD_EFF_Y, FLD_EFF_DY, PARTY_DY, PARTY_NAME_W = 342, 44, 80, 190
BODY = 5  # body text scale in the pane (the module measures scroll lengths at this scale)
PANE_GREY, PANE_TEXT, PANE_LABEL, PANE_RED = "#FF8A8E96", "#FF4A4E58", "#FF5A6070", "#FFC83C32"
RESIST_GREY = "#FF3E4658"  # resist factors (Pokédex Weak., the pane's Resists)
# the module's PaneGeoms: suffix -> (content width, type-grid columns, move chips, Field tab)
PANE_GEOMS = {"n": (448, 2, False, False), "w": (652, 3, True, True)}


def scrolled(widgets, scroll, rb, y_bind=None):
    """Put widgets into a scroll region: single widgets become one-element lists (only lists follow a
    scroll) shown while `rb` is 1 (the tab); y_bind moves them as a whole."""
    for widget in widgets:
        if "repeat" not in widget:
            widget["repeat"], widget["repeat_bind"] = 1, rb
        widget["scroll"] = scroll
        if y_bind:
            widget["y_bind"] = y_bind
    return widgets


SECTION_ROOM = 300  # a section title fits this (at 6, down to 5); the rule starts after it


def section(x, y, w, key, **kw):
    """A NAVY section header on a hairline rule (the rule starts after the title)."""
    return title_rule(x, y, y+16, x+w, key, 6, min(SECTION_ROOM, w - 60), gap=14, min_scale=5, **kw)


def stage_chip(x1, y, p, **kw):
    """A stat stage chip ending at x1 ("+2" red, "-1" blue), shown only for a nonzero stage."""
    out = []
    for gate, colour in (("up", "#FFD0503C"), ("down", "#FF3C78C8")):
        # at least 32 px tall: the rounded mask's corners are 2x8 px, a shorter chip turns into an ellipse
        out += panel([x1-60, y, 60, 34], color=colour, shadow=False, need_bind=p+gate, **kw)
        out.append(label([x1-30, y+5, 0, 0], bind_text=p+"stage", scale=4, color=WHITE, align="center",
                         need_bind=p+gate, **kw))
    return out


def battle_tabs(x, y, w, names, gate=None, ons=None, taps=None):
    """The pane's tab header: the battlers' names (binds) and Field; the current one bold with an
    underline. Tab k is lp.bt.t<k>, tapped as btab<k> (ons / taps: other binds and actions)."""
    need = {"need_bind": gate} if gate else {}
    hide = {"hide_bind": gate, "hide_eq": 0} if gate else {}
    out = [rect([x, y+51, w, 2], "#FFD8DCE4", **need)]
    seg = w // len(names)
    under = min(150, seg - 16)
    for k, name in enumerate(names):
        sx, on = x + k * seg, ons[k] if ons else f"lp.bt.t{k}"
        text = dict(bind_text=name) if name.startswith("lp.") else dict(bind_text=t(name))
        out += [rect([sx, y, seg, 52], "#00000000", on_tap=taps[k] if taps else f"btab{k}", **need),
                label([sx + seg//2, y+3, 0, 0], scale=5, align="center", color="#FF8A8E96", fit_text=True,
                      text_min_scale=3, wrap_width=seg-16, max_lines=1, text_center_h=34, hide_bind=on, hide_eq=1,
                      **text, **need),
                label([sx + seg//2, y+1, 0, 0], scale=6, align="center", color=DARK, fit_text=True,
                      text_min_scale=3, wrap_width=seg-16, max_lines=1, text_center_h=38, need_bind=on,
                      **text, **hide),
                rect([sx+seg//2-under//2, y+45, under, 6], "#FFE8A33C", need_bind=on, **hide)]
    return out


def type_grid(cx, gy, cw, cols, p, key, sid, yb=None, factor=True):
    """A Weak to / Resists / No effect grid: type tags with their factor, `cols` per row."""
    g = {"repeat": 18, "repeat_bind": p+key+".count", "repeat_cols": cols, "repeat_dx": cw // cols,
         "repeat_row_dy": GRID_DY, "scroll": sid}
    if yb:
        g["y_bind"] = yb
    out = [img([cx, gy, 156, 34], src_bind=p+key+"{i}.tag", **g)]
    if factor:
        # body size; 4× bold (outlined in its own colour; the module never sets strong for a resist).
        # A triple type's "1/8×" drops to 4.
        colour, fw = PANE_RED if key == "wk" else RESIST_GREY, cw // cols - 160
        f, s = p+key+"{i}.f", p+key+"{i}.strong"
        for bold in (0, 1):
            out.append(label([cx+162, gy+6, fw, 0], bind_text=f, scale=BODY, color=colour, fit_text=True,
                             text_min_scale=4, wrap_width=fw, max_lines=1, hide_bind=s, hide_eq=1-bold,
                             **({"outline": colour, "outline_px": 1} if bold else {}), **g))
    return out


def battle_pane(x, w, sfx):
    """The battle view's left card. Tabs: the foe (types and the caught ball, condition and item,
    speed, matchups, base stats, abilities), you (types, condition and item, battle stats with
    stages, your moves and the picked move, ability) and on the command page Field (weather, terrain,
    effects, your party, the opponent's party). Swipe left / right or tap a tab; each page scrolls
    when it is taller than the card. The command page has the move chips (tap one to see it).
    Returns (widgets, scrolls)."""
    y, h = BODY_Y, 800
    cx, cw = x+24, w-48
    cx1, cy = cx+cw, y+76+PANE_PAD
    cw_, cols, chips, field = PANE_GEOMS[sfx]
    assert cw_ == cw, "the module's PaneGeoms must match the pane width"
    ids = [f"bt{name}_{sfx}" for name in ("foe", "own", "field")]
    tabs = [f"lp.bt.t{k}" for k in range(3)]
    out = panel([x, y, w, h], color="#F2FAFCFF")
    names = ["lp.bt.foe.name", "lp.bt.own.name"]
    # a double: one tab for the focused battler (a card of the overview), then Field
    dn, dsel, dtap = ["lp.bt.dname"], ["lp.bt.dsel0"], ["btab20"]
    if field:
        out += battle_tabs(x+12, y+10, w-24, names + ["field"], gate="lp.bt.tabs.sf")
        out += battle_tabs(x+12, y+10, w-24, names, gate="lp.bt.tabs.sn")
        out += battle_tabs(x+12, y+10, w-24, dn + ["field"], gate="lp.bt.tabs.df", ons=dsel + ["lp.bt.dsel1"],
                           taps=dtap + ["btab2"])
        out += battle_tabs(x+12, y+10, w-24, dn, gate="lp.bt.tabs.dn", ons=dsel, taps=dtap)
    else:
        # the move page: Move first (the highlighted move), then the battlers
        out += battle_tabs(x+12, y+10, w-24, ["tab_move"] + names, gate="lp.bt.sgl",
                           ons=["lp.bt.t3", "lp.bt.t0", "lp.bt.t1"], taps=["btab3", "btab0", "btab1"])
        out += battle_tabs(x+12, y+10, w-24, ["tab_move"] + dn, gate="lp.bt.dbl", ons=["lp.bt.t3"] + dsel,
                           taps=["btab3"] + dtap)
    for widget in out:
        widget.setdefault("hide_bind", "lp.bt.scene_revision")
        widget.setdefault("hide_eq", -1)  # revision is nonnegative; the dependency repaints the card
    out.append(rect([x, y+68, w, h-68], "#00000000", on_swipe_left="btab10", on_swipe_right="btab11", swipe_px=70))
    # type tags under the name band: 160x35 on the wide pane; the narrow one 148x32 (two fit beside the
    # caught ball), a third type (p+"ty3on") drops all three to 126x27
    tag_w, tag_h = (160, 35) if sfx == "w" else (148, 32)

    def band(p, modern, classic):
        """Name band (Lv. on the right), the battle types, the condition and held item line."""
        out = panel([cx, cy, cw, 64], color=modern, shadow=False, need_bind="lp.cfg.modern")
        out += panel([cx, cy, cw, 64], color=classic, shadow=False, need_bind="lp.cfg.classic")
        lvw = max_latin_width("level_prefix", 6) + 12 + text_width("100", 6) + 30  # the Lv. part's reserve
        out += [label([cx+20, cy, cw-lvw-80, 0], bind_text=p+"name", scale=7, color=WHITE, fit_text=True,
                      text_min_scale=4, wrap_width=cw-lvw-80, max_lines=1, text_center_h=64),
                img([cx+cw-lvw-48, cy+16, 32, 32], src_bind=p+"gendericon"),
                label([cx+cw-20-12, cy+17, 0, 0], bind_text=t("level_prefix"), scale=6, color=WHITE, align="right",
                      x_bind=tw("num_100", 6), x_scale=-1, _avail=lvw-30-text_width("100", 6)),
                value([cx+cw-20, cy+17, 0, 0], p+"level", scale=6, color=WHITE, align="right")]
        if sfx == "w":
            out += [img([cx+k*(tag_w+8), cy+80+(35-tag_h)//2, tag_w, tag_h], src_bind=p+f"ty{k+1}") for k in range(3)]
        else:
            out += [img([cx+k*(tag_w+8), cy+80+(35-tag_h)//2, tag_w, tag_h], src_bind=p+f"ty{k+1}",
                        hide_bind=p+"ty3on", hide_eq=1) for k in range(2)]
            out += [img([cx+k*134, cy+84, 126, 27], src_bind=p+f"ty{k+1}", need_bind=p+"ty3on") for k in range(3)]
        cyl = cy+COND_Y
        # condition and item on one line: the module picks the largest scale (5, 4, 3) at which
        # both fit this pane (cs<s>.<sfx>) and the item icon's x after the condition (itemx.<sfx>)
        for s in (5, 4, 3):
            gate, ic = f"{p}cs{s}.{sfx}", 8*s
            out += [label([cx, cyl, cw, 0], bind_text=p+"cond", scale=s, color=PANE_TEXT, color_markup=True,
                          wrap_width=cw, max_lines=1, text_center_h=25, need_bind=gate),
                    img([cx-2, cyl+12-ic//2, ic, ic], src_bind=p+"itemicon", x_bind=f"{p}itemx.{sfx}",
                        need_bind=gate),
                    label([cx+ic+2, cyl, cw-ic-2, 0], bind_text=p+"itemname", scale=s, color=PANE_TEXT,
                          fit_text=True, text_min_scale=3, wrap_width=cw-ic-2, max_lines=1, text_center_h=25,
                          x_bind=f"{p}itemx.{sfx}", need_bind=gate)]
        return out

    # FOE -----------------------------------------------------------------------------------------
    sid, rb, p = ids[0], tabs[0], "lp.bt.foe."
    fo = band(p, "#FFD96660", "#FF50524B")
    # caught before: the Poké Ball the Pokédex list shows, right on the types row
    fo += [img([cx1-50, cy+77, 42, 42], ui("zukan", "dex_deco_list_01_01"), need_bind=p+"caughtv"),
           label([cx, cy+180, cw, 0], bind_text=p+"speed", scale=BODY, color=PANE_TEXT, color_markup=True,
                 fit_text=True, text_min_scale=3, wrap_width=cw, max_lines=1, text_center_h=25)]
    fo += section(cx, cy+FOE_TOP, cw, "weak_to")
    fo.append(label([cx, cy+FOE_TOP+SEC_H+4, 0, 0], bind_text=t("nothing"), scale=BODY, color=PANE_GREY,
                    need_bind=p+"wk.none", _avail=cw))
    out += scrolled(fo, sid, rb)
    out += type_grid(cx, cy+FOE_TOP+SEC_H, cw, cols, p, "wk", sid)
    for key, title, yb in (("rs", "resists", "y1"), ("im", "no_effect", "y2")):
        yb = f"{p}{yb}.{sfx}"
        sec = section(cx, cy, cw, title)
        sec.append(label([cx, cy+SEC_H+4, 0, 0], bind_text=t("nothing"), scale=BODY, color=PANE_GREY,
                         need_bind=p+key+".none", _avail=cw))
        out += scrolled(sec, sid, rb, yb)
        out += type_grid(cx, cy+SEC_H, cw, cols, p, key, sid, yb, factor=key == "rs")
    # its battle stats (before and after their stage), as on the You page
    yb = f"{p}y3.{sfx}"
    bs = section(cx, cy, cw, "stats")
    bs.append(label([cx, cy+SEC_H+6*OWN_STAT_DY+6, cw, 0], bind_text=p+"accev", scale=BODY, color=PANE_TEXT,
                    color_markup=True, need_bind=p+"accev.on"))
    out += scrolled(bs, sid, rb, yb)
    gb = {"repeat": 6, "repeat_bind": p+"st.count", "repeat_dy": OWN_STAT_DY, "scroll": sid, "y_bind": yb}
    by, q = cy+SEC_H, p+"st{i}."
    out += [label([cx, by+12, cw-200, 0], bind_text=q+"name", scale=BODY, color=PANE_LABEL, fit_text=True,
                  text_min_scale=4, wrap_width=cw-200, max_lines=1,
                  _fit=["hp", "stat_attack", "stat_defense", "stat_sp_atk", "stat_sp_def", "stat_speed"], **gb),
            label([cx1-76, by+8, 0, 0], bind_text=q+"v", scale=6, align="right", color=DARK, **gb),
            rect([cx, by+49, cw, 1], "#FFE0E4EC", **gb)]
    out += stage_chip(cx1, by+8, q, **gb)
    # its moves: type, name, how they hit your Pokémon, PP
    yb = f"{p}y5.{sfx}"
    out += scrolled(section(cx, cy, cw, "moves"), sid, p+"mv.on", yb)
    gm = {"repeat": 4, "repeat_bind": p+"mv.count", "repeat_dy": FOE_MOVE_DY, "scroll": sid, "y_bind": yb}
    my, q = cy+SEC_H, p+"mv{i}."
    ppw, effw = text_width("40/40", BODY), text_width("¼×", BODY)
    nmw = cw - 48 - ppw - effw - 40
    # the type as the move button's own icon cap (cropped from the BDSP button body: the classic
    # theme's bodies have no icon, so this stays BDSP art in both themes, like the type tags)
    out += [img([cx, my+3, 36, 36], bind=q+"type", src_format="module:lp:wz/%d", src_rect=WZ_ICON, **gm),
            label([cx+48, my+4, nmw, 0], bind_text=q+"name", scale=BODY, color=DARK, fit_text=True,
                  text_min_scale=4, wrap_width=nmw, max_lines=1, text_center_h=34, **gm),
            label([cx1-ppw-20, my+4, 0, 0], bind_text=q+"eff", scale=BODY, align="right", color=PANE_LABEL,
                  text_center_h=34, hide_bind=q+"effgood", hide_eq=1, **gm),
            label([cx1-ppw-20, my+4, 0, 0], bind_text=q+"eff", scale=BODY, align="right", color=PANE_RED,
                  text_center_h=34, need_bind=q+"effgood", **gm),
            value([cx1, my+6, 0, 0], q+"pp", scale=BODY, max_bind=q+"ppmax", max_sep="/", align="right", **gm),
            rect([cx, my+FOE_MOVE_DY-3, cw, 1], "#FFE0E4EC", **gm)]
    # its ability (the one in battle when the reader has it, else the species' possible ones), last:
    # one flowing text
    yb = f"{p}y4.{sfx}"
    ab = section(cx, cy, cw, "ability", need_bind=p+"ab.known")
    ab += section(cx, cy, cw, "abilities", hide_bind=p+"ab.known", hide_eq=1)
    ab.append(label([cx, cy+SEC_H, cw, 0], bind_text=p+"ab.text", scale=BODY, wrap_width=cw, max_lines=40,
                    color=PANE_TEXT, color_markup=True))
    out += scrolled(ab, sid, rb, yb)
    effects = section(cx, cy, cw, "effects")
    effects.append(label([cx,cy+SEC_H,cw,0],bind_text=p+"effects",scale=BODY,wrap_width=cw,max_lines=40,color=PANE_TEXT))
    out += scrolled(effects, sid, p+"effects.on", f"{p}effects.y.{sfx}")

    # YOU -----------------------------------------------------------------------------------------
    sid, rb, p = ids[1], tabs[1], "lp.bt.own."
    yo = band(p, "#FF538ABE", "#FF4F7850")
    yo += section(cx, cy+STATS_TOP, cw, "stats")
    yo.append(label([cx, cy+STATS_TOP+SEC_H+6*OWN_STAT_DY+6, cw, 0], bind_text=p+"accev", scale=BODY,
                    color=PANE_TEXT, color_markup=True, need_bind=p+"accev.on"))
    out += scrolled(yo, sid, rb)
    gs = {"repeat": 6, "repeat_bind": p+"st.count", "repeat_dy": OWN_STAT_DY, "scroll": sid}
    sy, q = cy+STATS_TOP+SEC_H, p+"st{i}."
    out += [label([cx, sy+12, cw-200, 0], bind_text=q+"name", scale=BODY, color=PANE_LABEL, fit_text=True,
                  text_min_scale=4, wrap_width=cw-200, max_lines=1, **gs),
            label([cx1-76, sy+8, 0, 0], bind_text=q+"v", scale=6, align="right", color=DARK, **gs),
            rect([cx, sy+49, cw, 1], "#FFE0E4EC", **gs)]
    out += stage_chip(cx1, sy+8, q, **gs)
    # the moves: chips (command page), then the picked move as the move card had it
    ym = p+"ym"
    out += scrolled(section(cx, cy, cw, "moves" if chips else "tab_move"), sid, "lp.bt.mv.on", ym)
    if chips:
        chw = (cw - 12) // 2
        m = "lp.bt.m{i}."
        rep = {"repeat": 4, "repeat_bind": "lp.bt.chip.count", "repeat_cols": 2, "repeat_dx": chw+12,
               "repeat_row_dy": CHIP_DY, "scroll": sid, "y_bind": ym}
        d = cy + SEC_H
        chh = CHIP_DY - 8
        chip = panel([cx, d, chw, chh], color="#FFFAF6E8", shadow=False, hide_bind=m+"shown", hide_eq=1)
        chip += panel([cx, d, chw, chh], color="#FFDCE8F2", shadow=False, need_bind=m+"shown")
        # the name on its own line (as large as it fits), the type tag and the matchup (factor form) under it
        ew = chw - 12 - 140 - 10 - 12
        chip += [label([cx+12, d+8, chw-24, 0], bind_text=m+"name", scale=6, color=DARK, fit_text=True,
                       text_min_scale=4, wrap_width=chw-24, max_lines=1, text_center_h=30),
                 img([cx+12, d+50, 140, 30], src_bind=m+"tag"),
                 label([cx+162, d+50, ew, 0], bind_text=m+"effs", scale=BODY, color="#FF5A5A5A", fit_text=True,
                       text_min_scale=4, wrap_width=ew, max_lines=1, text_center_h=30,
                       hide_bind=m+"effgood", hide_eq=1),
                 label([cx+162, d+50, ew, 0], bind_text=m+"effs", scale=BODY, color=PANE_RED, fit_text=True,
                       text_min_scale=4, wrap_width=ew, max_lines=1, text_center_h=30, need_bind=m+"effgood"),
                 rect([cx, d, chw, chh], "#00000000", on_tap="btinspect", payload="{i}",
                      on_swipe_left="btab10", on_swipe_right="btab11", swipe_px=70)]
        for widget in chip:
            widget.update(rep)
        out += chip
    def move_card(d):
        """The picked move: type-coloured name band, category, type tag, STAB, Power / Accuracy /
        PP, its effectiveness, the description."""
        pk = "lp.bt.pk."
        band_ = panel([cx, d, cw, 64], color=TYPE_COLORS[0], shadow=False, need_bind=pk+"ok")
        for widget in band_:
            widget.update(tint_bind=pk+"type", tint_colors=TYPE_COLORS)
        mv = band_ + panel([cx, d, cw, 64], color=NAVY, shadow=False, hide_bind=pk+"ok", hide_eq=1)
        mv += [label([cx+20, d, cw-124, 0], bind_text=pk+"name", scale=7, color=WHITE, fit_text=True,
                     text_min_scale=4, wrap_width=cw-124, max_lines=1, text_center_h=64),
               img([cx+cw-84, d+14, 64, 36], src_bind=pk+"catkey"),
               img([cx, d+80, 184, 40], src_bind=pk+"tag")]
        # same type as your Pokémon (1.5x)
        mv += panel([cx+200, d+80, 104, 40], color="#FFE8A33C", shadow=False, need_bind=pk+"stab")
        mv.append(label([cx+252, d+80, 96, 0], bind_text=t("stab"), scale=BODY, color=WHITE, align="center",
                        fit_text=True, text_min_scale=3, wrap_width=96, max_lines=1, text_center_h=40,
                        need_bind=pk+"stab"))
        # PP and how it hits, a gap, then Power and Accuracy
        for name, oy, colour in (("pp", 136, "#FFF0EAD6"), ("power", 272, "#FFF0EAD6"), ("accuracy", 332, "#FFFAF6E8")):
            ry = d+oy
            mv += panel([cx, ry, cw, 56], color=colour, shadow=False)
            mv.append(label([cx+20, ry, cw-200, 0], bind_text=t(name), scale=BODY, color="#FF7A7A7A", fit_text=True,
                            text_min_scale=4, wrap_width=cw-200, max_lines=1, text_center_h=56))
            if name == "pp":
                mv.append(value([cx1-20, ry+13, 0, 0], pk+"pp", scale=6, align="right", max_bind=pk+"ppmax", max_sep="/"))
            else:
                mv.append(label([cx1-20, ry+13, 0, 0], bind_text=pk+("powertxt" if name == "power" else "acctxt"),
                                scale=6, align="right"))
        mv += panel([cx, d+196, cw, 56], color="#FFF9F6ED", shadow=False)
        mv += [label([cx+20, d+211, cw-40, 0], bind_text=pk+"efftxt", scale=BODY, color="#FF5A5A5A", fit_text=True,
                     text_min_scale=4, wrap_width=cw-40, max_lines=1, hide_bind=pk+"effgood", hide_eq=1),
               label([cx+20, d+211, cw-40, 0], bind_text=pk+"efftxt", scale=BODY, color=PANE_RED, fit_text=True,
                     text_min_scale=4, wrap_width=cw-40, max_lines=1, need_bind=pk+"effgood")]
        mv += section(cx, d+MOVE_DESC_Y-SEC_H, cw, "description")
        mv.append(label([cx, d+MOVE_DESC_Y, cw, 0], bind_text=pk+"desc", scale=BODY, wrap_width=cw, max_lines=30,
                        color=PANE_TEXT))
        return mv

    if chips:
        out += scrolled(move_card(cy), sid, "lp.bt.mv.on", f"{p}yd.{sfx}")
    ab = section(cx, cy, cw, "ability")
    ab.append(label([cx, cy+SEC_H, cw, 0], bind_text=p+"ab.text", scale=BODY, wrap_width=cw, max_lines=30,
                    color=PANE_TEXT, color_markup=True))
    out += scrolled(ab, sid, rb, f"{p}ya.{sfx}")
    effects = section(cx, cy, cw, "effects")
    effects.append(label([cx,cy+SEC_H,cw,0],bind_text=p+"effects",scale=BODY,wrap_width=cw,max_lines=40,color=PANE_TEXT))
    out += scrolled(effects, sid, p+"effects.on", f"{p}effects.y.{sfx}")

    scroll_tabs = [("foe", "lp.bt.foe.rk"), ("own", "lp.bt.own.rk")]  # the battler shown (and its spot)
    # MOVE (move page): the highlighted move's card
    if not field:
        out += scrolled(move_card(cy), f"btmove_{sfx}", "lp.bt.t3")
    # FIELD (command page) ------------------------------------------------------------------------
    if field:
        sid, rb, p = ids[2], tabs[2], "lp.bt.fld."
        fl = panel([cx, cy, cw, 64], color="#FF5E6E96", shadow=False)
        fl += [label([cx+20, cy, cw-260, 0], bind_text=t("field"), scale=7, color=WHITE, fit_text=True,
                      text_min_scale=5, wrap_width=cw-260, max_lines=1, text_center_h=64),
               label([cx1-20, cy+17, 220, 0], bind_text=p+"turn", scale=6, color=WHITE, align="right",
                     fit_text=True, text_min_scale=4, wrap_width=220, max_lines=1, _fit=["turn"])]
        # the names' column: the widest Latin name at 5 and a gap (any language fits it down to 4)
        lw = max(max_latin_width("weather", BODY), max_latin_width("terrain", BODY)) + 24
        for k, (name, bind) in enumerate((("weather", "weather"), ("terrain", "terrain"))):
            fl += [label([cx, cy+84+k*44, lw-16, 0], bind_text=t(name), scale=BODY, color=PANE_LABEL, fit_text=True,
                         text_min_scale=4, wrap_width=lw-16, max_lines=1),
                   label([cx1, cy+84+k*44, cw-lw, 0], bind_text=p+bind, scale=BODY, color=DARK, align="right",
                         fit_text=True, text_min_scale=4, wrap_width=cw-lw, max_lines=1,
                         _fit=["weather_" + w for w in ("none", "sun", "rain", "hail", "sand", "heavy_rain",
                                                        "extreme_sun", "strong_winds")] if name == "weather" else
                              ["terrain_" + w for w in ("none", "grassy", "misty", "electric", "psychic")])]
        for i in range(4):
            ax, aw = cx + i*(cw//4), cw//4-8
            q = p+f"active{i}."
            fl += panel([ax,cy+174,aw,94],color="#FFE1EDF8" if i<2 else "#FFF8E5E3",shadow=False,need_bind=q+"on")
            fl += [img([ax+(aw-60)//2,cy+174,60,60],src_bind=q+"icon",need_bind=q+"on"),
                   img([ax+aw-28,cy+208,24,24],src_bind=q+"gendericon",need_bind=q+"on"),
                   label([ax+4,cy+238,aw-8,0],bind_text=q+"name",scale=4,fit_text=True,text_min_scale=3,
                         wrap_width=aw-8,max_lines=1,need_bind=q+"on")]
        fl += section(cx, cy+FLD_EFF_Y-SEC_H, cw, "effects")
        fl.append(label([cx, cy+FLD_EFF_Y+4, 0, 0], bind_text=t("none_effects"), scale=BODY, color=PANE_GREY,
                        need_bind=p+"e.none", _avail=cw))
        out += scrolled(fl, sid, rb)
        ge = {"repeat": 8, "repeat_bind": p+"e.count", "repeat_dy": FLD_EFF_DY, "scroll": sid}
        out += [label([cx, cy+FLD_EFF_Y+4, cw-230, 0], bind_text=p+"e{i}.name", scale=BODY, color=DARK, fit_text=True,
                      text_min_scale=4, wrap_width=cw-230, max_lines=1,
                      _fit=["effect_" + e for e in ("trick_room", "magic_room", "wonder_room", "gravity", "imprison",
                                                    "ion_deluge", "fairy_lock", "neutralizing_gas")], **ge),
                label([cx1, cy+FLD_EFF_Y+4, 210, 0], bind_text=p+"e{i}.turns", scale=BODY, color=PANE_LABEL,
                      align="right", fit_text=True, text_min_scale=4, wrap_width=210, max_lines=1,
                      _fit=["turns_left_one", "turns_left_other", "effect_active"], **ge)]

        def party_rows(q, rows_bind, yb, own):
            """Icon, name with its type stamps after it, HP (yours) or level (theirs) and the HP bar;
            a fainted member is dimmed."""
            g = {"repeat": 6 if own else 12, "repeat_bind": rows_bind, "repeat_dy": PARTY_DY, "scroll": sid, "y_bind": yb}
            py = cy+SEC_H
            row = [img([cx, py, 60, 60], src_bind=q+"icon"),
                   label([cx+70, py+2, PARTY_NAME_W, 0], bind_text=q+"name", scale=BODY, color=DARK, fit_text=True,
                         text_min_scale=3, wrap_width=PARTY_NAME_W, max_lines=1, text_center_h=28)]
            # the stamps follow the name (x_bind per row: the module measures the name)
            row += [img([cx+70+k*136, py+2, 130, 28], src_bind=q+f"ty{k+1}", x_bind=q+"tx") for k in range(2)]
            if own:
                row.append(value([cx1, py+4, 0, 0], q+"hp", scale=BODY, max_bind=q+"hpmax", max_sep="/", align="right"))
            else:
                row += [value([cx1, py+4, 0, 0], q+"level", scale=BODY, align="right"),
                        label([cx1 - 8, py+4, 0, 0], bind_text=t("level_prefix"), scale=BODY,
                              color=PANE_LABEL, align="right", x_bind=tw("num_100", BODY), x_scale=-1, _avail=60)]
            gated_group(row[4:], q+"details")
            row.append(img([cx+38, py+36, 24, 24], src_bind=q+"gendericon"))
            health = hp_bar(cx+70, py+42, cw-70, 14, q)
            for widget in health:
                widget.update(g)
            gated_group(health, q+"details")
            row += health
            # fainted: a dark veil over the row
            row.append(rect([cx-6, py-4, cw+12, PARTY_DY-4], "#8C30343E", need_bind=q+"fainted"))
            for widget in row:
                widget.update(g)
            return row

        out += scrolled(section(cx, cy, cw, "your_party"), sid, rb, p+"yp")
        out += party_rows("lp.bt.pt{i}.", "lp.bt.pt.rows", p+"yp", True)
        out += scrolled(section(cx, cy, cw, "enemy_party"), sid, p+"ep.on", p+"ye")
        out += party_rows("lp.bt.ep{i}.", "lp.bt.ep.rows", p+"ye", False)
        scroll_tabs.append(("field", "lp.bt.seq"))

    vp = [x+12, y+76, w-24, h-92]
    # show_bind <tab> (lp.bt.t<k>, 0 or 1): the shown tab's bar only
    scrolls = [{"id": ids[k], "rect": vp, "count_bind": f"lp.bt.rows.{name}.{sfx}", "row_h": 10,
                "reset_bind": reset, "show_bind": tabs[k], "bar": "#FF8A96B9", "bar_w": 6}
               for k, (name, reset) in enumerate(scroll_tabs)]
    if not field:
        scrolls.append({"id": f"btmove_{sfx}", "rect": vp, "count_bind": f"lp.bt.rows.move.{sfx}", "row_h": 10,
                        "reset_bind": "lp.bt.pk.rk", "show_bind": "lp.bt.t3", "bar": "#FF8A96B9", "bar_w": 6})
    return out, scrolls


# ---- the game's own Bag / Pokémon windows over the battle menu (top screen) --------------------
# The module publishes lp.bt.bagwin while the Bag is open and lp.bt.sw.* (switch suggestions) while
# the Pokémon list is open, also the forced one after a faint.
BAGWIN_H, BAGWIN_TOP = BODY_BOTTOM - BODY_Y, 200  # the Bag panel beside the pane, as tall as the pane; its content block's top
SW_HEAD, SW_CARD_H, SW_GAP, SW_LINE = 108, 222, 8, 38
OVERLAY_DIM = "#B00C1018"  # the page under a centred overlay


def overlay_motion(widgets, gate, group, box):
    """A centred overlay card fades in (POPUP_ANIM_MS); its touch blocker (the first widget) is instant."""
    for widget in widgets[1:]:
        widget["anim"] = {"bind": gate, "group": group, "from": "fade", "ms": POPUP_ANIM_MS,
                          "easing": "ease_out", "box": box}
    return widgets


def bag_note(x):
    """While the game's Bag is open in battle: a panel right of the battle pane (from x to the
    screen's right margin, under the header cards) saying to look at the main screen; the pane
    stays visible and usable. The page is dimmed under the pane and the header cards (battle_dims);
    the area behind the panel swallows touches (the command buttons there do nothing while the Bag is
    open)."""
    w, h = W - 24 - x, BAGWIN_H
    y = BODY_Y
    on = {"need_bind": "lp.bt.bagwin"}
    tw = w - 48
    out = [rect([x - 14, y - 14, W - (x - 14), H - (y - 14)], "#00000000", input_block=True, **on)]
    out += panel([x, y, w, h], color=CREAM, band="#FFF3C716", band_h=56, **on)
    out += [label([x+24, y+11, tw, 0], bind_text=t("bag"), scale=6, color=DARK, fit_text=True, text_min_scale=5,
                  wrap_width=tw, max_lines=1, **on),
            img([x+(w-144)//2, y+BAGWIN_TOP, 144, 144], src_bind="lp.bt.bagwin.icon", **on),
            # up to three lines on the narrow command page, one on the move page
            label([x+24, y+BAGWIN_TOP+172, tw, 0], bind_text=t("bagnote_title"), scale=6, color=DARK,
                  wrap_width=tw, max_lines=3, **on),
            label([x+24, y+BAGWIN_TOP+316, tw, 0], bind_text=t("bagnote_hint"), scale=5,
                  color=PANE_LABEL, wrap_width=tw, max_lines=3, **on)]
    return overlay_motion(out, "lp.bt.bagwin", "bagwin", [x-16, y-16, w+32, h+40])


def switch_overlay(x):
    """Switch suggestions while the game's Pokémon list is open: a panel right of the battle pane
    (from x to the screen's right margin, under the header cards), so the pane stays visible and
    usable. Up to three of your benched Pokémon, best first, each with its icon, name, HP and up to
    three short reasons. The page is dimmed under the pane and the header cards (battle_dims); the
    area behind the panel swallows touches (the command buttons there do nothing while the list is
    open)."""
    w = W - 24 - x
    y = BODY_Y
    h = BODY_BOTTOM - BODY_Y  # as tall as the pane
    assert SW_HEAD + 3 * (SW_CARD_H + SW_GAP) <= h, "the switch cards overflow the panel"
    on = {"need_bind": "lp.bt.sw.on"}
    out = [rect([x - 14, y - 14, W - (x - 14), H - (y - 14)], "#00000000", input_block=True, **on)]
    out += panel([x, y, w, h], color="#FFF4F7FC", band="#FF538ABE", band_h=56, **on)
    out += [label([x+24, y+11, w-48, 0], bind_text=t("switch_suggestions"), scale=6, color=WHITE, outline="#FF283048",
                  outline_px=3, fit_text=True, text_min_scale=5, wrap_width=w-48, max_lines=1, **on),
            label([x+24, y+66, w-48, 0], bind_text="lp.bt.sw.vs", scale=5, color=PANE_LABEL, fit_text=True,
                  text_min_scale=4, wrap_width=w-48, max_lines=1, _fit=["switch_vs"], **on),
            label([x+w//2, y+300, w-48, 0], bind_text=t("switch_none"), scale=6, color=PANE_LABEL,
                  align="center", wrap_width=w-48, max_lines=2, need_bind="lp.bt.sw.none")]
    cx, cw = x+16, w-32
    for k in range(3):
        p = f"lp.bt.sw{k}."
        cy = y + SW_HEAD + k * (SW_CARD_H + SW_GAP)
        card = {"need_bind": p+"on"}
        tx = cx+124
        name_w = cw-(tx-cx)-16-(96 if k == 0 else 0)
        out += panel([cx, cy, cw, SW_CARD_H], color="#FFE6EEF8", shadow=False, **card)
        out += [img([cx+10, cy+4, 104, 104], src_bind=p+"icon", **card),
                img([cx+80, cy+78, 26, 26], src_bind=p+"gendericon", **card),
                label([tx, cy+16, name_w, 0], bind_text=p+"name", scale=6, color=DARK, fit_text=True,
                      text_min_scale=4, wrap_width=name_w, max_lines=1, **card),
                label([tx, cy+62, 0, 0], bind_text=t("hp"), scale=4, color="#FFF4C428", outline=DARK, outline_px=2,
                      _avail=40, **card)]
        bar = hp_bar(tx+44, cy+66, cw-(tx-cx)-44-20, 14, p)
        for widget in bar:
            widget.setdefault("need_bind", p+"on")  # the track; the gauges keep their colour gates
        out += bar
        if k == 0:
            out.append(label([cx+cw-16, cy+18, 0, 0], bind_text=t("switch_best"), scale=4, color=WHITE, bg="#FFE8A33C",
                             pill=True, pad=6, auto_w=True, align="right", _avail=96, **card))
        for r in range(3):
            out.append(label([cx+20, cy+108+r*SW_LINE, cw-40, 0], bind_text=p+f"r{r}", scale=6, color=DARK,
                             color_markup=True, fit_text=True, text_min_scale=5, wrap_width=cw-40, max_lines=1,
                             _fit=["switch_immune", "switch_resists", "switch_super", "switch_faster", "switch_weak",
                                   "switch_low_hp", "switch_even"], **card))
    return overlay_motion(out, "lp.bt.sw.on", "switch", [x-16, y-16, w+32, h+40])


def page_battle():
    w = battle_background()
    pane, scrolls = battle_pane(24, STRIP_W, "w")
    # the game's X shortcut: first ball in the native bag list with the X key glyph, above the command stack
    bx, by, bw, bh = 1046, BALL_Y, 164, BALL_H
    w += sliced([bx, by, bw, bh], ui("sharedui", "btl_bt_quickitem_01_body"),
                (244, 61), [18, 14, 18, 14], 1.4, need_bind="lp.bt.ball.any")
    w += [rect([bx, by, bw, bh], "#00000000", on_tap="cmd_ball", need_bind="lp.bt.ball.any"),
          img([bx + 18, by + 8, 88, 88], src_bind="lp.bt.ball0.icon", need_bind="lp.bt.ball.any"),
          img([bx + bw - 68, by + bh - 46, 59, 42], ui("sharedui", "btl_pl_ico_keyguide_01_01"),
              need_bind="lp.bt.ball.any")]
    for k, (text, idx, act) in enumerate(COMMANDS):
        y = CMD_Y0 + k * (CMD_H + CMD_GAP)
        w += gated_group(command_button([CMD_X[k], y, CMD_W, CMD_H], idx, text, 8, act),
                   f"lp.bt.cmd{k}.available")
        w += brackets([CMD_X[k], y, CMD_W, CMD_H], f"lp.bt.cmd{k}.picked")
        w += boxed_key(CMD_X[k]+CMD_W-68, y+CMD_H-46, "A", need_bind=f"lp.bt.cmd{k}.picked")
    # the dims over the buttons and the background, under the pane and the header cards
    w += battle_dims() + busy_veil(24 + STRIP_W)
    w += pane
    w += battle_status()
    w += bag_note(24 + STRIP_W + 20) + switch_overlay(24 + STRIP_W + 20)
    # Draw after the dim and touch veil: the cards and Continue stay fully readable.
    # Run ends at y=957; preserve that stack and put the dialogue control directly below it.
    cx, cy, cw, ch = CMD_X[3], CMD_Y0 + 4 * (CMD_H + CMD_GAP), CMD_W, 70
    cont = panel([cx, cy, cw, ch], color="#FF50524B", shadow=False,
                 on_tap="battlecontinue", need_bind="lp.bt.continue")
    cont += [label([cx+24, cy, cw-108, 0], bind_text=t("cmd_continue"), scale=6,
                   color=WHITE, fit_text=True, text_min_scale=4, wrap_width=cw-108,
                   max_lines=1, text_center_h=ch, on_tap="battlecontinue", need_bind="lp.bt.continue")]
    cont += boxed_key(cx+cw-78, cy+(ch-42)//2, "A", on_tap="battlecontinue", need_bind="lp.bt.continue")
    w += cont
    return {"id": "battle", "widgets": w, "scrolls": scrolls}


def boxed_key(x, y, key, **kw):
    return [img([x,y,59,42], ui("sharedui","btl_pl_ico_keyguide_01_01"), **kw),
            rect([x+8,y+8,43,26], "#FF50524B", **kw),
            label([x+29,y+11,0,0], text=key, scale=4, color=WHITE, align="center", **kw)]


def shortcut_back(r, action):
    """Left native arrow, centered label and the shared lower-right boxed B."""
    x,y,width,height = r
    out = command_button(r,5,"back",6,action)
    for widget in out:
        if widget.get("bind_text") == t("back"):
            widget["rect"] = [x+height+8,y,width-height-88,0]
            widget["wrap_width"] = width-height-88
            widget["text_min_scale"] = 4
    out += boxed_key(x+width-68,y+height-46,"B")
    return out


def page_moves():
    """Battle-sized detail card (the swipeable left pane, Move page first) and curved move stack;
    one tap executes a move."""
    w = battle_background()
    pane, scrolls = battle_pane(24, MOVE_PANE_W, "n")
    for k in range(4):
        x, y, mw, mh = MOVE_X[k], MOVE_Y0+k*(CMD_H+CMD_GAP), MOVE_W, CMD_H
        mp = f"lp.bt.m{k}."
        w += move_body(x, y, mw, mh, mp)
        tx = x+round(80*mh/76)+24
        # left: the name, and under it how the move hits the foe (a double: each target, "1× · 2×"),
        # as one block centred in the frame; right: the PP large, vertically centred, ending
        # MOVE_PP_GAP px before the button's edge (the A key sits below it, in the bottom corner)
        ppx = x+mw-MOVE_PP_GAP
        nw = ppx-MOVE_PP_W-28-tx
        ny, ey = y+MOVE_NAME_Y, y+MOVE_EFF_Y
        w += [rect([x,y,mw,mh], "#00000000", on_tap=f"move{k}", need_bind=mp+"id"),
              label([tx,ny,nw,0], bind_text=mp+"name", scale=6,
                    fit_text=True, text_min_scale=5, max_lines=1, wrap_width=nw, need_bind=mp+"id"),
              value([ppx,y+MOVE_PP_Y,0,0], mp+"pp", scale=MOVE_PP_SCALE, color=DARK, max_bind=mp+"ppmax",
                    max_sep="/", align="right", need_bind=mp+"id"),
              label([tx,ey,nw,0], bind_text=mp+"eff", scale=5, color=MOVE_EFF_GREY,
                    fit_text=True, text_min_scale=4, max_lines=1, wrap_width=nw, hide_bind=mp+"effgood", hide_eq=1,
                    need_bind=mp+"id"),
              label([tx,ey,nw,0], bind_text=mp+"eff", scale=5, color=PANE_RED,
                    fit_text=True, text_min_scale=4, max_lines=1, wrap_width=nw, need_bind=mp+"effgood")]
        w += brackets([x,y,mw,mh], mp+"picked")
        w += boxed_key(x+mw-68, y+mh-46, "A", need_bind=mp+"picked")
    w += shortcut_back([W-30-BACK_W,BACK_Y,BACK_W,BACK_H], "go_battle")
    # the dims over the buttons and the background, under the pane and the header cards
    w += battle_dims() + busy_veil(24 + MOVE_PANE_W)
    w += pane
    w += battle_status()
    w += bag_note(24 + MOVE_PANE_W + 20) + switch_overlay(24 + MOVE_PANE_W + 20)
    # Target selection follows the native gray-out rules; overview cards submit a single
    # target, while spread moves use one explicit A confirmation. Native B cancels.
    w += panel([W-30-BACK_W, BACK_Y-102, BACK_W, 88], color="#FF50524B", shadow=False,
               on_tap="targetconfirm", need_bind="lp.bt.target.spread")
    w += [label([W-30-BACK_W+20, BACK_Y-102, BACK_W-108, 0], bind_text=t("cmd_continue"),
                scale=6, color=WHITE, fit_text=True, text_min_scale=4, wrap_width=BACK_W-108,
                max_lines=1, text_center_h=88, on_tap="targetconfirm", need_bind="lp.bt.target.spread")]
    w += boxed_key(W-30-78, BACK_Y-80, "A", on_tap="targetconfirm", need_bind="lp.bt.target.spread")
    w += gated_group(shortcut_back([W-30-BACK_W, BACK_Y, BACK_W, BACK_H], "targetback"), "lp.bt.target.on")
    return {"id": "moves", "widgets": w, "scrolls": scrolls}


def ball_button(r, key, action):
    x, y, width, height = r
    out = sliced(r, ui("sharedui", "btl_bt_quickitem_01_body"), (244, 61),
                 [14, 12, 14, 12], height/61)
    out += [rect(r, "#00000000", on_tap=action),
            label([x+width//2, y, width-40, 0], bind_text=t(key), scale=8, color=WHITE, align="center", fit_text=True,
                  text_min_scale=5, wrap_width=width-40, max_lines=1, text_center_h=height)]
    return out


def page_balls():
    """Complete native selector border and an external upper-left B shortcut."""
    w = battle_background() + battle_status()
    w += shortcut_back([24,BODY_Y,BACK_W,96], "ballback")
    x, y, pw, ph = 24, BODY_Y+96+16, 1192, 712
    pad, arrow_w, arrow_h = 48, 27, 48
    src = ui("sharedui", "btl_pl_quickitem_01")
    w += sliced([x,y,pw,ph], src, (237,109), [14,14,14,14], 2.5)
    w += [rect([x+35,y+35,pw-70,ph-70], "#FFF4F4F4")]
    hx, hy = x+pad+arrow_w+pad, y+pad
    hw, hh = pw-2*(2*pad+arrow_w), 132
    count_w = round(112*hh/55)
    # Keep all four native selector edges and their shadow inside the crop.
    w += sliced([hx,hy,hw,hh], src, (237,109), [10,10,112,10], hh/55,
                crop=(49,17,192,72))
    w += [img([hx+20,hy+18,96,96], src_bind="lp.bt.ball.icon"),
          label([hx+132,hy+42,hw-count_w-152,0], bind_text="lp.bt.ball.name", scale=8,
                color="#FF080808", fit_text=True, text_min_scale=5, wrap_width=hw-count_w-152),
          label([hx+hw-count_w+34,hy+38,0,0], text="×", scale=9, color=WHITE),
          value([hx+hw-30,hy+38,0,0], "lp.bt.ball.count", scale=10, color=WHITE, align="right"),
          img([x+pad,hy+(hh-arrow_h)//2,arrow_w,arrow_h], ui("sharedui","cmn_ico_arrow_01"),
              tint="#FF293663", on_tap="ballprev", need_bind="lp.bt.ball.multi"),
          img([x+pw-pad-arrow_w,hy+(hh-arrow_h)//2,arrow_w,arrow_h], ui("sharedui","cmn_ico_arrow_01"),
              tint="#FF293663", flip_x=True, on_tap="ballnext", need_bind="lp.bt.ball.multi"),
          label([x+76,hy+hh+32,pw-152,0], bind_text="lp.bt.ball.desc", scale=8,
                color="#FF080808", wrap_width=pw-152, fit_text=True, text_min_scale=7)]
    bw, bh = 420,96
    bx, uy = x+(pw-bw)//2, y+ph-pad-bh
    # 120x180 tap targets around the arrows; a swipe on the selector or the description changes ball
    ay = hy+hh//2-90
    w += [rect([x+pad+arrow_w//2-60,ay,120,180], "#00000000", on_tap="ballprev", need_bind="lp.bt.ball.multi"),
          rect([x+pw-pad-arrow_w//2-60,ay,120,180], "#00000000", on_tap="ballnext", need_bind="lp.bt.ball.multi"),
          rect([hx,hy,hw,uy-16-hy], "#00000000", on_swipe_left="ballnext", on_swipe_right="ballprev", swipe_px=80,
               need_bind="lp.bt.ball.multi")]
    w += ball_button([bx,uy,bw,bh], "use", "balluse")
    w += brackets([bx,uy,bw,bh], "lp.bt.ball.focus", cs=44)
    w += boxed_key(bx+bw-68, uy+bh-46, "A", need_bind="lp.bt.ball.focus")
    w += [rect([0,220,W,H-220], "#55101420", hide_bind="lp.bt.ball.input", hide_eq=1, input_block=True)]
    return {"id":"balls", "widgets":w}


def actions():
    a = {
        "open_settings": {"kind": "page", "page": "@settings", **FADE_TAB},

        "targetconfirm": {"kind": "module", "action": "targetconfirm", "enabled_bind": "lp.bt.target.input"},
        "targetback": {"kind": "module", "action": "targetback", "enabled_bind": "lp.bt.target.input"},
        "battlecontinue": {"kind": "module", "action": "battlecontinue", "enabled_bind": "lp.bt.continue"},
        "go_moves": {"kind": "module", "action": "battleview", "argument": 1, "enabled_bind": "lp.bt.input"},
        "go_battle": {"kind": "module", "action": "battleview", "argument": 0, "enabled_bind": "lp.bt.input"},
        # Pokémon / Bag / Run use native windows; Poké Ball opens the recreated bottom panel.
        "cmd_pokemon": {"kind": "module", "action": "command", "argument": 1, "enabled_bind": "lp.bt.input"},
        "cmd_bag": {"kind": "module", "action": "command", "argument": 2, "enabled_bind": "lp.bt.input"},
        "cmd_run": {"kind": "module", "action": "command", "argument": 3, "enabled_bind": "lp.bt.input"},
        "cmd_ball": {"kind": "module", "action": "ballkey", "argument": 0, "enabled_bind": "lp.bt.input"},
        # one A / X press, requested by the module for exactly one sample
        "pdrv.press.a": {"kind": "button", "button": "A", "frames": 5, "enabled_bind": "pdrv.a"},
        "pdrv.press.b": {"kind": "button", "button": "B", "frames": 5, "enabled_bind": "pdrv.b"},
        "pdrv.press.x": {"kind": "button", "button": "X", "frames": 5, "enabled_bind": "pdrv.x"},
        "pdrv.press.plus": {"kind": "button", "button": "Plus", "frames": 5, "enabled_bind": "pdrv.plus"},
        "pdrv.press.dup": {"kind": "button", "button": "DUp", "frames": 5, "enabled_bind": "pdrv.dup"},
        "pdrv.press.ddown": {"kind": "button", "button": "DDown", "frames": 5, "enabled_bind": "pdrv.ddown"},
        "pdrv.press.dleft": {"kind": "button", "button": "DLeft", "frames": 5, "enabled_bind": "pdrv.dleft"},
        "pdrv.press.dright": {"kind": "button", "button": "DRight", "frames": 5, "enabled_bind": "pdrv.dright"},
    }
    for action in ("ballprev", "ballnext", "balluse", "ballback"):
        a[action] = {"kind": "module", "action": action, "enabled_bind": "lp.bt.ball.input"}
    for k in range(4):
        a[f"move{k}"] = {"kind": "module", "action": "move", "argument": k, "enabled_bind": "lp.bt.input"}
    for k in range(6):
        a[f"sel{k}"] = {"kind": "module", "action": "select", "argument": k}
        a[f"swap{k}"] = {"kind": "module", "action": f"swap{k}", "argument": "$payload", "enabled_bind": "lp.fld.ok"}
    for k in range(9):
        a[f"bagpocket{k}"]={"kind":"module","action":"bag_pocket","argument":k}
    a["bagnext"]={"kind":"module","action":"bag_pocket","argument":10}
    a["bagprev"]={"kind":"module","action":"bag_pocket","argument":11}
    # one action each for the repeated list rows: the row index arrives as the payload
    a["bagselect"]={"kind":"module","action":"bag_select","argument":"$payload"}
    a["dexselect"]={"kind":"module","action":"dex_select","argument":"$payload"}
    a["dexsort"]={"kind":"module","action":"dex_sort","argument":0}
    a["dexmode"]={"kind":"module","action":"dex_mode","argument":0}
    for k in (0, 1, 2, 3, 4, 10, 11):
        a[f"dexpage{k}"] = {"kind": "module", "action": "dex_page", "argument": k}
    # the Pokédex card: a form chip, an evolution member, a place (shown on the Map)
    a["dexform"] = {"kind": "module", "action": "dex_form", "argument": "$payload"}
    a["dexevo"] = {"kind": "module", "action": "dex_evo", "argument": "$payload"}
    a["dexarea"] = {"kind": "module", "action": "dex_area", "argument": "$payload"}
    for k in (0, 1, 2, 10, 11):
        a[f"ftab{k}"] = {"kind": "module", "action": "fieldtab", "argument": k}
    for k in range(4):
        a[f"moveswap{k}"] = {"kind": "module", "action": f"moveswap{k}", "argument": "$payload", "enabled_bind": "lp.fld.ok"}
    a["partymove"] = {"kind": "module", "action": "party_move", "argument": "$payload"}
    a["partymoveclose"] = {"kind": "module", "action": "party_move_close", "argument": 0}
    # the battle pane: tabs 0 Foe, 1 You, 2 Field; 10 / 11 = swipe to the next / previous.
    # No enabled_bind: the pane works while the game animates (it never presses a button).
    for k in (0, 1, 2, 3, 10, 11, 20):  # 3: the move page's Move tab; 20: a double's focused-battler tab
        a[f"btab{k}"] = {"kind": "module", "action": "battletab", "argument": k}
    for k in range(4):  # a card of the double-battle overview (a view position)
        a[f"btfocus{k}"] = {"kind": "module", "action": "battlefocus", "argument": k}
    a["btinspect"] = {"kind": "module", "action": "battleinspect", "argument": "$payload"}
    a["map_tap"] = {"kind": "module", "action": "map_tap", "argument": 0}
    a["baguse"] = {"kind": "module", "action": "bag_use", "argument": 0}
    a["bagopen"] = {"kind": "module", "action": "bag_open", "argument": 0}
    a["fly"] = {"kind": "module", "action": "fly", "argument": 0}
    a["maproute"] = {"kind": "module", "action": "map_route", "argument": 0}
    for k in range(4):
        a[f"fwuse{k}"] = {"kind": "module", "action": "field_waza", "argument": k}
    a["mapfly"] = {"kind": "module", "action": "map_fly", "argument": 0}
    a["bagcancel"] = {"kind": "module", "action": "bag_cancel", "argument": 0}
    for k in range(6):
        a[f"bagtarget{k}"] = {"kind": "module", "action": "bag_target", "argument": k}
    for k in range(4):
        a[f"bagmove{k}"] = {"kind": "module", "action": "bag_move", "argument": k}
    for k in range(20):
        a[f"apppick{k}"] = {"kind": "module", "action": "apppick", "argument": k}
    a["pkt_next"] = {"kind": "module", "action": "pkt_next", "argument": 0}
    a["pkt_prev"] = {"kind": "module", "action": "pkt_prev", "argument": 0}
    a["pkt_touch"] = {"kind": "module", "action": "pkt_touch", "argument": "$payload"}
    for k in range(3):
        a[f"pkt_full{k}"] = {"kind": "module", "action": "pkt_full", "argument": k}
    for view in sorted({tab[1] for tab in NAV_TABS}):
        a[f"fview{view}"] = {"kind": "module", "action": "fieldview", "argument": view}
    # Haptics: every tap ticks "light" (the manifest's haptics.tap: tabs, nav, list rows, pockets,
    # pane tabs, Back, the map); two tiers differ. A refused module action buzzes "reject"
    # (haptics.refused); a hold is "heavy" and a swipe "light" (the runtime's own kinds).
    click = ["go_moves", "cmd_pokemon", "cmd_bag", "cmd_ball",  # open a game menu, decide nothing yet
             "pkt_touch", "pkt_next"]                           # a Pokétch key, its red button
    confirm = (["cmd_run", "balluse", "fly", "mapfly", "baguse", "bagopen"]
               + [f"move{k}" for k in range(4)] + [f"fwuse{k}" for k in range(4)]
               + [f"bagtarget{k}" for k in range(6)] + [f"bagmove{k}" for k in range(4)])
    for name in click:
        a[name]["haptic"] = "click"
    for name in confirm:
        a[name]["haptic"] = "confirm"
    return a


def manifest():
    # A second edition/build in the same process must not retain stale declarations or
    # append duplicate fit rows from a previous build (or standalone page preview).
    for registry in (ANIM_DERIVED, USED, WIDTHS, SUMS, GROUP_DERIVED, FITS, CATALOG_FITS):
        registry.clear()
    result = {
        "format": 1,
        "title_id": TITLE,
        "name": NAME,
        "_about": "Direction 2 (BDSP X-menu, tabs along the bottom). Art and font are decoded from the player's romfs by the module.",
        "requires_module": True,
        "min_runtime": MIN_RUNTIME,
        "canvas_w": W,
        "canvas_h": H,
        "background": "#FF8A96B9",
        "font": "file:lp_font.txt",
        # paged (runtime 17): Korean and Chinese atlases span several 2048x1024 pages
        "font_atlas": "module:lp:font/FOT-UDKakugoC80Pro-DB:48/{p}",
        "font_page_h": 1024,
        "module": {"abi": 1, "build_ids": [BUILD], "libraries": {}},
        # guest stub serving the module's game-thread calls (Fly, field moves, Escape Rope...),
        # generated by gen_load_plan.py
        "load_plan": "guest/<build>/load-plan.json",
        # The module always hides the top-screen battle menus: runtime 18 runs modules before it
        # publishes flags and derived values, so a settings toggle could never reach it.
        "flags": {"platinum_theme": 0, ANIM_FLAG: 1},
        # the Town Map picture (1266x732 px) as a map area: world = picture px, y up
        "map": {"areas": {"townmap": {"min": [0, 0], "max": [1266, 732], "clamp_view": True, "no_pin": True,
            "dynamic_markers": [
                {"count": 1, "x": "lp.map.goal.x", "y": "lp.map.goal.y", "show_bind": "lp.map.goal.on",
                 "icon_src_bind": "lp.map.goal.icon", "size": 48, "anchor": [0.5, 1]},
                {"count": 1, "x": "lp.map.me.x", "y": "lp.map.me.y", "show_bind": "lp.map.me.on",
                 "icon_src_bind": "lp.map.head", "size": 64, "anchor": [0.5, 0.5]}
            ]}}},
        "settings": [{"flag": "platinum_theme", "label": "Classic Platinum theme", "type": "toggle", "default": 0},
                     {"flag": ANIM_FLAG, "label": "Animations", "type": "toggle", "default": 1}],
        "pages": [page_wait_lineup(), page_field(), page_map(), page_route(), page_poketch(), page_poketch_full(), page_inventory(4), page_inventory(5), page_battle(), page_moves(), page_balls()],
        "page_binds": [
            {"point": "lp.page", "equals": 0, "when_equal": {"page": "wait", **FADE_START}},
            {"point": "lp.page", "equals": 1, "when_equal": {"page": "field", **FADE_FIELD}},
            {"point": "lp.page", "equals": 3, "when_equal": {"page": "map", **FADE_TAB}},
            {"point": "lp.page", "equals": 4, "when_equal": {"page": "route", **FADE_TAB}},
            {"point": "lp.page", "equals": 5, "when_equal": {"page": "poketch", **FADE_TAB}},
            {"point":"lp.page","equals":6,"when_equal":{"page":"pokedex", **FADE_TAB}},
            {"point":"lp.page","equals":7,"when_equal":{"page":"field_bag", **FADE_TAB}},
            {"point": "lp.page", "equals": 8, "when_equal": {"page": "poketch_full", **FADE_TAB}},
            {"point": "lp.page", "equals": 2, "when_equal": {"page": "battle", **FADE_BATTLE_IN}},
            {"point": "lp.bt.view", "equals": 1, "when_equal": {"page": "moves", **FADE_BATTLE}},
            {"point": "lp.bt.view", "equals": 0, "when_equal": {"page": "battle", **FADE_BATTLE}},
            {"point": "lp.bt.ball.focus", "equals": 1, "when_equal": {"page": "balls", **FADE_BATTLE},
             "when_not_equal": {"page": "battle", **FADE_BATTLE}},
            # a new turn (the command menu takes focus again) returns to the command page
            {"point": "lp.bt.cmd.focus", "equals": 1, "when_equal": {"page": "battle", **FADE_BATTLE}},
        ],
        "derived": [
            {"name": "lp.bt.bag.style", "expr": "lp.player.sex + 2 * @flag:platinum_theme"},
            {"name": "lp.cfg.classic", "expr": "@flag:platinum_theme"},
            {"name": "lp.cfg.modern", "expr": "@flag:platinum_theme == 0"},
            # the single-battle header per theme (a double shows its overview there)
            {"name": "lp.bt.sgl.modern", "expr": "lp.bt.sgl == 1 && @flag:platinum_theme == 0"},
            {"name": "lp.bt.sgl.classic", "expr": "lp.bt.sgl == 1 && @flag:platinum_theme != 0"},
            {"name": "lp.ftab0.on", "expr": "lp.fld.tab == 0"},
            # the Weaknesses list scrolls (and shows its bar) only without the pop-up over it
            {"name": "lp.weak.scroll", "expr": "lp.party.selected == 1 && lp.fld.tab == 2 && lp.bag.popup.any == 0"},
            {"name": "lp.ftab1.on", "expr": "lp.fld.tab == 1"},
            {"name": "lp.ftab2.on", "expr": "lp.fld.tab == 2"},
            # the Map legend at 5 when its names fit in the game's language, else at 4
            {"name": "lp.map.legend5", "expr": "lp.map.acquired == 1 && lp.t.sum.legend5.fits == 1"},
            {"name": "lp.map.legend4", "expr": "lp.map.acquired == 1 && lp.t.sum.legend5.fits == 0"},
        ] + [{"name": f"lp.nav{k}.on", "expr": selected} for k, selected in enumerate(NAV_SELECTED)]
        + [{"name": f"lp.bt.cmd{k}.picked", "expr": f"lp.bt.cmd.focus == 1 && lp.bt.cmd.index == {k} && lp.bt.cmd{k}.available == 1"} for k in range(4)]
        # the unselected plate body only for a slot that holds a Pokémon (lp.sel is clamped to one)
        + [{"name": f"lp.sel{k}.{state}", "expr": expression}
           for k in range(6) for state, expression in (("on", f"lp.sel == {k}"),
                                                       ("off", f"lp.sel != {k} && lp.p{k}.valid"))]
        + [{"name": f"{p}{k}.wz", "expr": f"{p}{k}.type + 100 * @flag:platinum_theme"}
           for p in ("lp.bt.m", "lp.sel.m", "lp.bag.m") for k in range(4)]
        + [{"name": f"{p}{k}.cicon", "expr": f"{p}{k}.id != 0 && @flag:platinum_theme != 0"}
           for p in ("lp.bt.m", "lp.sel.m", "lp.bag.m") for k in range(4)]
        + [{"name": "lp.party.move.wz", "expr": "lp.party.move.type + 100 * @flag:platinum_theme"},
           {"name": "lp.party.move.cicon", "expr": "lp.party.move.id != 0 && @flag:platinum_theme != 0"}]
        + BATTLE_DIM_DERIVED
        + MAP_CLOUD_DERIVED
        + list(ANIM_DERIVED.values())
        + START_DERIVED
        + list(GROUP_DERIVED.values()),
        "actions": actions(),
        "enforce": [{"action": "pdrv.press.a", "every_ms": 1}, {"action": "pdrv.press.b", "every_ms": 1},
                    {"action": "pdrv.press.x", "every_ms": 1}, {"action": "pdrv.press.plus", "every_ms": 1},
                    {"action": "pdrv.press.dup", "every_ms": 1}, {"action": "pdrv.press.ddown", "every_ms": 1},
                    {"action": "pdrv.press.dleft", "every_ms": 1}, {"action": "pdrv.press.dright", "every_ms": 1}],
        "enforce_gate": {"point": "pdrv.pending", "max": 1},
        # the controller stays with the game (no second-screen focus mode)
        "nav": False,
        # light for navigation and selection; actions() raises menu buttons to click, commits to confirm
        "haptics": {"enabled": True, "respect_system": True, "tap": "light", "select": "light", "drag": "light",
                    "drop": "confirm", "write": "confirm", "marker": "light", "refused": "reject",
                    "hold": "heavy", "swipe": "light"},
    }

    for page in result["pages"]:
        for widget in page["widgets"]:
            key = widget.get("src", "").removeprefix("module:lp:")
            if key in THEMED_SPRITES:
                # the settings flag picks the BDSP or the classic Platinum texture
                widget["bind"] = THEME
                widget["src_names"] = [widget["src"], "module:lp:classic/" + key]
    collect_fits(result["pages"])
    for page in result["pages"]:
        if page["id"] in ("field", "map", "route", "poketch"):
            for widget in page["widgets"]:
                if widget["rect"][1] < 100 and widget.get("type") != "rect":
                    widget.setdefault("hide_bind", "lp.bt.scene_revision")
                    widget.setdefault("hide_eq", -1)
    return result


FITS: list[tuple] = []
CATALOG_FITS: list[tuple] = []
# labels that show the game's names: every name of that kind is checked where they are drawn
CATALOG_BINDS = [
    ("move", r"^lp\.(bt\.m\d|bt\.m\{i\}|sel\.m\{i\}|bag\.m\d|bt\.pk|bt\.foe\.mv\{i\}|party\.move)\.name$"),
    ("item", r"^lp\.(bag\{i\}\.name|bag\.selected\.name|sel\.itemname|bt\.(own|foe)\.itemname|bt\.ball\.name)$"),
    ("ability", r"^lp\.sel\.ability$"),
    ("species", r"^lp\.((dex\{i\}|route\{i\}|p\d|bt\.(own|foe)|bt\.d\d|bt\.pt\{i\}|bt\.ep\{i\}|bt\.sw\d|sel|"
                r"dex\.selected)\.name|bt\.dname)$"),
]


# labels the module fills from our rows (toasts, ratings): the rows they show
EFF_KEYS = ["eff_super", "eff_neutral", "eff_not_very", "no_effect"]
KEY_BINDS = [
    (r"^lp\.dex\.title$", ["dex_national", "dex_sinnoh"]),
    (r"^lp\.bag\.toast$", ["toast_fly_hint", "toast_cant_open", "toast_opening_bag", "toast_repel_active",
                            "toast_repel_used", "toast_cant_now", "toast_using", "toast_revived", "toast_no_effect",
                            "ppup_toast", "toast_recovered", "toast_used_on", "toast_cant_use_here", "toast_wait",
                            "toast_used", "party_reordered"]),
    (r"^lp\.bt\.m(\d|\{i\})\.eff$", EFF_KEYS + ["status_short"]),
    (r"^lp\.bt\.m(\d|\{i\})\.effs$", ["status_short"]),
    (r"^lp\.bt\.pk\.efftxt$", EFF_KEYS + ["status_move"]),
]


def collect_fits(pages):
    """Where each string is drawn, for the fit report (lp_strings_data.h Fits): every label bound to
    lp.t.<key>, and the rows a module-composed label shows (_fit). Strips the generator-only keys
    (_avail: the room of a label that neither wraps nor fits)."""
    seen = set()
    for page in pages:
        for widget in page["widgets"]:
            avail = widget.pop("_avail", 0)
            keys = list(widget.pop("_fit", []))
            kind = widget.pop("_fit_kind", None)
            if kind and widget.get("type") == "label":
                spec = (kind, widget.get("text_scale", 6),
                        widget.get("text_min_scale", 6) if widget.get("fit_text") else widget.get("text_scale", 6),
                        widget.get("wrap_width", 0), widget.get("max_lines", 1), page["id"])
                if spec not in seen:
                    seen.add(spec)
                    CATALOG_FITS.append(spec)
            if widget.get("type") != "label":
                continue
            bind = widget.get("bind_text", "")
            if bind.startswith("lp.t.") and bind.count(".") == 2:
                keys.insert(0, bind[5:])
                if bind[5:] in ("start_starting_bd", "start_starting_sp"):
                    keys = ["start_starting_bd", "start_starting_sp"]
            scale = widget.get("text_scale", 6)
            fit = widget.get("fit_text", False)
            wrap = widget.get("wrap_width", 0)
            lines = widget.get("max_lines", 0) if wrap else 1
            spec_w = wrap or avail
            for pattern, rows in KEY_BINDS:
                if re.match(pattern, bind):
                    keys += rows
            for kind, pattern in CATALOG_BINDS:
                if wrap and re.match(pattern, bind):
                    spec = (kind, scale, widget.get("text_min_scale", scale) if fit else scale, wrap, lines, page["id"])
                    if spec not in seen:
                        seen.add(spec)
                        CATALOG_FITS.append(spec)
            if not keys:
                continue
            for key in keys:
                t(key)
                spec = (key, scale, widget.get("text_min_scale", scale) if fit else scale, spec_w, lines, page["id"])
                if spec not in seen:
                    seen.add(spec)
                    FITS.append(spec)


def main():
    global TITLE, BUILD, NAME, PKG
    ap = argparse.ArgumentParser()
    ap.add_argument("--edition", choices=EDITIONS, default="diamond",
                    help="game title/profile to generate; vanilla and Luminescent share each edition's package")
    ap.add_argument("--package-dir", type=Path,
                    help="override the generated package directory (defaults to packages/<edition package>)")
    ap.add_argument("--version", default="1.0.0", help="package release version")
    ap.add_argument("--lib", action="append", default=[], metavar="PLATFORM=PATH",
                    help="pin a built module (copied to dualscreen/modules/<platform>/), e.g. linux-x86_64=build/0100000011D90000.so")
    args = ap.parse_args()
    TITLE, BUILD, package_dir, NAME = EDITIONS[args.edition]
    PKG = args.package_dir if args.package_dir is not None else ROOT / "packages" / package_dir
    d = PKG / "dualscreen"
    d.mkdir(parents=True, exist_ok=True)
    m = manifest()
    for spec in args.lib:
        platform, path = spec.split("=", 1)
        data = Path(path).read_bytes()
        rel = f"modules/{platform}/{TITLE}.so"
        (d / rel).parent.mkdir(parents=True, exist_ok=True)
        (d / rel).write_bytes(data)
        m["module"]["libraries"][platform] = {"path": rel, "sha256": hashlib.sha256(data).hexdigest()}
    # compact: the manifest is machine-made and large (indenting it alone added ~0.9 MB)
    (d / "manifest.json").write_text(json.dumps(m, separators=(",", ":"), ensure_ascii=False) + "\n",
                                     encoding="utf-8")
    (d / "lp_font.txt").write_text(
        "# Game text font for the companion pages: <font in Data/resources.assets>:<pixel size>.\n"
        "# The module rasterises it from the player's own romfs; nothing is shipped.\n"
        "FOT-UDKakugoC80Pro-DB:48\n", encoding="utf-8")
    pkg = {"format": 1, "type": "dual-screen-mod", "title_id": TITLE,
           "name": NAME, "version": args.version, "requires_module": True,
           "module": m["module"], "min_runtime": MIN_RUNTIME}
    (PKG / "package.json").write_text(json.dumps(pkg, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    n = sum(len(p["widgets"]) for p in m["pages"])
    print(f"wrote {d / 'manifest.json'}: {len(m['pages'])} pages, {n} widgets")
    # the module's string table: every row, the widths and sums the pages position by, the fit specs
    widths = [(k, s, mn, wrap) for (k, s), (mn, wrap) in WIDTHS.items()]
    sums = [(name, scale, limit, keys) for name, (scale, limit, keys) in SUMS.items()]
    if LS.write_header(HEADER, STRINGS, widths, FITS, sums, CATALOG_FITS):
        print(f"{HEADER.name} changed: rebuild the module (then run this again to pin it)")
    print(f"strings: {len(STRINGS)} rows, {len(USED)} bound by the pages, {len(widths)} widths, {len(FITS)} fit specs")


if __name__ == "__main__":
    main()
