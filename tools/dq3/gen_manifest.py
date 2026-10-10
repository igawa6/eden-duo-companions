#!/usr/bin/env python3
"""gen_manifest.py [<out dir>] - Dragon Quest III HD-2D Remake companion (1240x1080): field cards, the
field tabs (Map / Party / Bag / Journal), the battle page and the system screens.

Writes manifest.json into packages/DragonQuest3HD2D/dualscreen/ (or <out dir>). The font
descriptor dq3_font.txt next to it is hand-written.

Layout: the owner-approved revision 2 "current-compact" design (research
design/redesign-2026-10/current-compact/render.py, CHANGES.md): the 0.9.1 layout and density with bigger
text paid for by tighter spacing (content box 36 / 32 .. 1204 / 1048, 8 px gaps, trimmed window and scrap
margins, 14 px dark-window / 26 x 24 px light-scrap padding, 18 px grid inset, 6 px cell gaps, 12 px
selection glow), the Bag as a list with a description pane, main controls >= 88 px. Two owner-approved
additions: the battle enemy card uses its right / bottom space (taller affinity grid, bigger text, the
Possible spells column), and the "Enemies" heading and the move lines under the battle party cards sit on
the game's own dark pieces (TravelInfo TitleBG strip, the dark window) instead of the bare scrap.

Cross-checks: the C++ side keeps its own copy of the page geometry and font styles (dq3_map.h,
dq3_page_layout.h, dq3_font.h FontStyle, dq3_battle_view.h bl::, dq3_system.h). check_native_layout()
reads them and stops the generator when they differ from the constants below; check_gauges() checks
every gauge / track pair and check_text() every static text's style.

Text: every game-font label is drawn at text_scale 3 against the module's atlas line height 15
(1:1 glyphs); a label's baseline is 15 px below its rect y. Sizes, weights and faces come from the
codepoint range (dq3_font.h): plain = Plantin Semibold 26, U+E000 + 0x100 * (k - 1) + c = style k.
Positions below are the design's Pillow baselines (anchor 'ls' / 'ms' / 'rs').
"""
import json, os, re, sys

W, H = 1240, 1080
# revision 2 colours (render.py): 0.9.1 hues, lightness moved for contrast
CREAM, SOFT, GOLD = '#FFF2EBD3', '#FFD6CEB8', '#FFECD292'
GREEN, BLUE = '#FF8CE268', '#FF78D4F4'
INK, INK2 = '#FF281C0E', '#FF4A361E'
HP_INK, MP_INK = '#FF18500E', '#FF0E3C82'
TARGET_INK, NEW_INK = '#FF682E00', '#FF782200'
STROKE = '#FF100E0A'
DIM = SOFT
BUILD = '4F41309B39EEBE5E1B8F2729B8105C3500000000000000000000000000000000'

# ---------------------------------------------------------------------------- geometry (render.py)
X0, Y0, X1, Y1 = 36, 32, 1204, 1048    # content box (0.9.1: 60 / 56 / 1180 / 1024)
CW = X1 - X0                            # 1168
G = 8                                   # gap between cards, windows and scraps
CARD_W = (CW - 3 * G) // 4              # 286
STEP = CARD_W + G                       # 294
CARD_H = 188
TAB_H = 88
FTAB_Y = Y1 - TAB_H                     # 960
FPAGE_Y = Y0 + CARD_H + G               # 228
FPAGE_B = FTAB_Y - G                    # 952
FPAGE_H = FPAGE_B - FPAGE_Y             # 724
WP = 14                                 # dark-window inner padding
LPX, LPY = 26, 24                       # light-scrap inner padding
TX = X0 + 98                            # field card text column (after the 10 + 80 px sprite column)
TW = X0 + CARD_W - WP - TX              # 174
BASE = 15                               # baseline below a text_scale-3 label's rect y
GI, GAP, GLOW = 18, 6, 12               # grid inset, cell gap, selection glow spread
ROWS = 320                              # list rows (module MaxRows)

# FontStyle numbers (native/modules/dq3_font.h) by (face, px): s = Plantin MT Pro Semibold, b = Bold,
# a = Avenir Next W1G Demi (the native HUD's HP / MP labels). ('s', 26) is the plain Value range.
STYLE = {('s', 26): 0, ('b', 26): 1, ('s', 24): 2, ('s', 28): 3, ('s', 30): 4, ('s', 32): 5, ('b', 24): 6,
         ('b', 28): 7, ('b', 30): 8, ('b', 32): 9, ('b', 34): 10, ('b', 36): 11, ('b', 40): 12, ('b', 44): 13,
         ('a', 24): 14, ('a', 26): 15, ('s', 22): 16, ('s', 20): 17, ('b', 22): 18, ('b', 54): 19}
# Pillow getmetrics() (ascent, descent) of the styles the battle view centres (dq3_battle_view.cpp Metrics)
METRIC = {('s', 20): (14, 7), ('s', 22): (16, 7), ('s', 24): (17, 8), ('s', 26): (19, 8), ('b', 26): (19, 8)}


def encode_style(text, k):
    """Latin-1 text in style range k (dq3_font.h: U+E000 + 0x100 * (k - 1) + c; k 0 = plain)."""
    if k == 0:
        return text
    return ''.join(chr(0xE000 + 0x100 * (k - 1) + ord(c)) if 0x20 <= ord(c) <= 0xFF else c for c in text)


def styled(text, st):
    return encode_style(text, STYLE[st])


def t(x, base, st, *, text=None, bind=None, color=CREAM, outline=1, align='left', **kw):
    """A game-font label with its baseline at `base` (Pillow anchor 'ls' / 'ms' / 'rs' at (x, base))."""
    w = {'type': 'label', 'rect': [round(x), round(base) - BASE, 0, 0], 'text_scale': 3, 'color': color}
    if align != 'left':
        w['align'] = align
    if text is not None:
        w['text'] = styled(text, st)
    if bind:
        w['bind_text'] = bind
        w['color_markup'] = True
    if outline:
        w['outline'] = STROKE
        w['outline_px'] = outline
    w.update(kw)
    return w


def img(rect, src=None, **kw):
    w = {'type': 'image', 'rect': [round(v) for v in rect], 'color': '#FFFFFFFF'}
    if src:
        w['src'] = src
    w.update(kw)
    return w


def rect(r, color, **kw):
    return dict({'type': 'rect', 'rect': [round(v) for v in r], 'bg': color, 'color': '#00000000', 'frame': 0}, **kw)


def tap(r, action, **kw):
    return dict({'type': 'rect', 'rect': [round(v) for v in r], 'color': '#00000000', 'bg': '#00000000', 'frame': 0,
                 'on_tap': action}, **kw)


def gauge(x, y, w, h, kind, bind, max_bind, **kw):
    """render.py gauge(): the game's gauge background (2 px rim, track/) around the bar rect and the fill
    (gauge/) laid out on the filled part."""
    return [img([x - 2, y - 2, w + 4, h + 4], f'module:dq3:track/{w + 4}x{h + 4}', **kw),
            dict({'type': 'bar', 'rect': [x, y, w, h], 'bind': bind, 'max_bind': max_bind,
                  'image': f'module:dq3:gauge/{kind}/{w}x{h}', 'fill': 'slice', 'slice': [0, 0, 0, 0], 'frame': 0,
                  'bg': '#00000000', 'color': '#00000000'}, **kw)]


def icon_steps(fmt, n):
    """src_thresholds for an icon int published + 1 (0 / missing = no image): value k + 1 -> fmt % k."""
    return [{'le': 0, 'src': ''}] + [{'le': k + 1, 'src': fmt % k} for k in range(n)]


# ============================================================================ field cards
# Owner 2026-10-10: the top strip has no blank space with fewer than four members. N = 1..4 active members
# (C+0xC0 == 1 sorted by C+0xC4, the module's live count) share the content box width equally:
# strip_w(N) = (CW - G * (N - 1)) / N (286 / 384 / 580 / 1168), same 188 px height and 8 px gaps. One widget set
# per N, gated by hide_bind dq3.strip kept at exactly N (the module publishes N, or 0 while the full map covers
# the strip). N 3-4: the 0.10.0 card (wider text column and gauges at N 3). N 2: "<job> · Lv N" on the name
# line, 12 px gauges across the card. N 1: the same name line, HP and MP side by side with 14 px gauges.
def strip_w(n):
    w = (CW - G * (n - 1)) // n
    assert w * n + G * (n - 1) == CW, n
    return w


CARDS_W = {n: strip_w(n) for n in range(1, 5)}
assert CARDS_W == {1: 1168, 2: 580, 3: 384, 4: 286}, CARDS_W
assert CARDS_W[4] == CARD_W
LV_ROOM = 268                           # wide cards: the job line after the name (name <= 200 px, TW(2) = 468)


def card(w, n):
    return dict(w, hide_bind='dq3.strip', keep_min=n, keep_max=n, repeat=n, repeat_dx=CARDS_W[n] + G,
                need_bind=w.get('need_bind', 'dq3.p{i}.on'))


def field_cards(n):
    cw = CARDS_W[n]
    tw = X0 + cw - WP - TX              # text column: 174 / 272 / 468 / 1056
    y = Y0
    c = lambda w: card(w, n)
    # card window + class sprite (+ gold outline for the member the Party tab shows) as one module image
    # (dq3_info.h fcard/<sprite>/<sel>/<w>): tapping shows the member on the Party tab; dragging it onto another
    # card swaps the two through the native Line-Up (only while dq3.party.drag<i>, i.e. the game is free)
    tp = dict(on_tap='party_pick', payload='{i}')
    out = [c(img([X0, y, cw, CARD_H], src_bind='dq3.p{i}.fcard', need_bind='dq3.p{i}.nodrag', **tp)),
           c(img([X0, y, cw, CARD_H], src_bind='dq3.p{i}.fcard', need_bind='dq3.party.drag{i}',
                 draggable=True, drag_scale=1.04, drop_action='party_drop_{i}', accept_group='dq3_cards',
                 highlight_color='#EAC56EFF', frame=3, **tp))]
    # name: Bold 32 at (tx, y + 40); KO red comes in the text as colour markup
    out.append(c(t(TX, y + 40, ('b', 32), bind='dq3.p{i}.name')))
    # status icons after the name (x offset = name width + 8 + k * 32, published by the module)
    for k in range(2):
        out.append(c(img([TX, y + 14, 28, 28], src_bind=f'dq3.p{{i}}.st{k}.src', x_bind=f'dq3.p{{i}}.st{k}.x',
                         need_bind=f'dq3.p{{i}}.st{k}.on')))
    if n >= 3:
        # "<job> · Lv N": Semibold 24, one line under the name
        out.append(c(t(TX, y + 70, ('s', 24), bind='dq3.p{i}.lv', color=SOFT, wrap_width=tw, max_lines=1)))
        rows = [(TX, y + 108 + k * 48, tw, 10) for k in range(2)]
    else:
        # wide: the job line on the name line, after the name and its icons (module offset dq3.p<i>.lvx)
        out.append(c(t(TX, y + 40, ('s', 24), bind='dq3.p{i}.lv', color=SOFT, x_bind='dq3.p{i}.lvx',
                       wrap_width=LV_ROOM, max_lines=1)))
        if n == 2:
            rows = [(TX, y + 100 + k * 52, tw, 12) for k in range(2)]
        else:
            half = (tw - 40) // 2
            rows = [(TX + k * (half + 40), y + 120, half, 14) for k in range(2)]
    for k, (lab, key, col, kind) in enumerate([('HP', 'hp', GREEN, 'hp'), ('MP', 'mp', BLUE, 'mp')]):
        rx, by, rw, gh = rows[k]
        out.append(c(t(rx, by, ('a', 24), text=lab, color=col)))
        # value: Semibold 30, right-aligned; HP-state colour markup from the module
        out.append(c(t(rx + rw, by + 1, ('s', 30), bind=f'dq3.p{{i}}.{key}_t', align='right')))
        out += [c(w) for w in gauge(rx, by + 8, rw, gh, kind, f'dq3.p{{i}}.{key}', f'dq3.p{{i}}.{key}_max')]
    return out


def widgets():
    out = [img([0, 0, W, H], f'module:dq3:scrap/{W}x{H}')]
    for n in range(1, 5):
        out += field_cards(n)
    return out


# ============================================================================ field tabs + Map page
FTABS = 4                               # Map . Party . Bag . Journal (owner option B)
LW = 384                                # Map page left column
MX = X0 + LW + G                        # 428
MW = X1 - MX                            # 776
FIN = 30                                # map frame inner inset
MB = 88                                 # map buttons, heal buttons
BANNER_H = 60
TITLE_ROOM = LW - 2 * 46 - 12           # 280: the banner text between the TitleBG end ornaments
MCARD_Y = FPAGE_Y + BANNER_H + G        # 296
MCARD_H = FPAGE_B - (2 * MB + G) - G - MCARD_Y   # 464
ZOOM_BOX = (157, 148, 173, 184, 171)    # "Area map", "Minimap", "Island map", "Region map", "World map" + 36
# no-map states (owner-approved mix, research design/redesign-2026-10/no-map; dq3_map.h NoMapState)
CARD_OBJ_Y, CARD_OBJ_H = 388, 72        # the card's objective window (dq3_map.h CardObjY / CardObjH)
TOWN_BTN_W = 161 + 60 + 52              # "Town Map" Bold 32 + icon + padding


def field_tab_widgets():
    """The command window (module art with the gold border, separators, icons and chevron) and labels."""
    slot = CW / FTABS
    out = [img([X0, FTAB_Y, CW, TAB_H], src_bind='dq3.ftabs', need_bind='dq3.ready')]
    for k in range(FTABS):
        out.append(t(0, FTAB_Y + TAB_H / 2 + 12, ('b', 34), bind=f'dq3.ftab{k}.t', color=SOFT,
                     x_bind=f'dq3.ftab{k}.x', need_bind='dq3.ready'))
        out.append(tap([X0 + k * slot + 4, FTAB_Y, slot - 8, TAB_H], f'ftab_{k}', need_bind='dq3.ready'))
    for w in out:
        w.update(hide_bind='dq3.m.full', hide_eq=1)
    return out


def map_widgets():
    g = dict(need_bind='dq3.mv.l')
    out = []
    # location banner: TravelInfo_TitleBG (end ornaments in 46 px corners, rim bands unscaled) and the title
    # in the largest of Bold 44 / 40 / 36 / 32 / 30 / 28 that fits; dq3.m.title.dy = round(px * 0.35)
    out.append(img([X0, FPAGE_Y, LW, BANNER_H], f'module:dq3:titlebg/{LW}x{BANNER_H}', **g))
    out.append(t(X0 + LW / 2, FPAGE_Y + BANNER_H / 2, ('b', 44), bind='dq3.m.title', color=GOLD, align='center',
                 y_bind='dq3.m.title.dy', **g))
    cy, ch = MCARD_Y, MCARD_H
    out.append(img([X0, cy, LW, ch], f'module:dq3:mlight/{LW}x{ch}', **g))
    ix, iy, iw = X0 + LPX, cy + LPY, LW - 2 * LPX
    out.append(img([ix, iy, 38, 38], src_bind='dq3.m.time_src', need_bind='dq3.m.time.on'))
    out.append(t(ix + 50, iy + 30, ('b', 32), bind='dq3.m.time', color=INK, outline=0, need_bind='dq3.m.time.on'))
    out.append(img([ix + 2, iy + 46, 34, 34], 'module:dq3:coin/0/34', **g))
    out.append(t(ix + 50, iy + 75, ('b', 32), bind='dq3.m.gold', color=INK, outline=0, **g))
    out.append(rect([ix, iy + 90, iw + 1, 1], '#FF967D55', **g))
    yy = iy + 102
    # town legend: the visible facility types of the current spot row (row k at dq3.m.l<k>.y: 48 px pitch,
    # 44 px with seven rows)
    for k in range(7):
        lg = dict(need_bind=f'dq3.m.l{k}.on', y_bind=f'dq3.m.l{k}.y')
        out.append(img([ix, yy, 38, 38], src_bind=f'dq3.m.l{k}.src', **lg))
        out.append(t(ix + 52, yy + 30, ('s', 32), bind=f'dq3.m.l{k}.name', color=INK, outline=0, **lg))
    # World geography is an overview; its quill still follows the live pawn.
    out.append(img([ix, yy, 38, 38], 'module:dq3:quill/38', need_bind='dq3.m.live.on'))
    out.append(t(ix + 52, yy + 30, ('s', 32), bind='dq3.m.live.label', color=INK, outline=0, need_bind='dq3.m.live.on'))
    # dungeon floors: visited floors only, the current one highlighted
    out.append(t(ix, yy + 28, ('b', 30), text='Floors visited', color=INK2, outline=0, need_bind='dq3.m.flo'))
    for k in range(4):
        ry = yy + 42 + k * 52
        out.append(img([ix - 6, ry, iw + 12, 48], f'module:dq3:selrow/{iw + 12}x48', need_bind=f'dq3.m.f{k}.here'))
        out.append(t(ix + 10, ry + 36, ('b', 32), bind=f'dq3.m.f{k}.name', color=INK, outline=0,
                     need_bind=f'dq3.m.f{k}.oth'))
        out.append(t(ix + 10, ry + 36, ('b', 32), bind=f'dq3.m.f{k}.name', color=GOLD, need_bind=f'dq3.m.f{k}.here'))
        out.append(t(ix + iw - 6, ry + 35, ('s', 28), text='you are here', align='right', need_bind=f'dq3.m.f{k}.hint'))
    # the place card's native objective (no-map B)
    out.append(t(ix, yy + 28, ('b', 30), text='Current objective', color=INK2, outline=0, need_bind='dq3.m.lobj.on'))
    out.append(t(ix, yy + 70, ('s', 32), bind='dq3.m.lobj', color=INK, outline=0, wrap_width=iw, max_lines=3,
                 line_gap=40 - BASE, need_bind='dq3.m.lobj.on'))
    # chests: under the floor list (dq3.m.chest.y = visited floors * 52)
    out.append(img([ix, yy + 50, 34, 34], 'module:dq3:treasure/34', need_bind='dq3.m.chest.on', y_bind='dq3.m.chest.y'))
    out.append(t(ix + 44, yy + 79, ('s', 30), bind='dq3.m.chest', color=INK, outline=0, need_bind='dq3.m.chest.on',
                 y_bind='dq3.m.chest.y'))
    # The two native healing actions: the game's own Misc-menu routine run by the guest load plan (no input,
    # no menu on the main screen), gated on a free field. The menu's result line replaces both buttons for a
    # few seconds (dq3.heal.msg.on), where the main screen used to show it.
    hb = dict(hide_bind='dq3.heal.msg.on', hide_eq=1)
    for k, label in enumerate(['Heal All', 'Handy Heal All']):
        by = cy + ch + G + k * (MB + G)
        out.append(img([X0, by, LW, MB], f'module:dq3:heal/{LW}x{MB}', **g, **hb))
        out.append(t(X0 + 60, by + MB / 2 + 11, ('b', 32), text=label, **g, **hb))
        out.append(tap([X0, by, LW, MB], f'heal_{k}', tap_enabled_bind='dq3.heal.ready', **g, **hb))
    hm = dict(need_bind='dq3.heal.msg.on')
    by, bh = cy + ch + G, 2 * MB + G
    out.append(img([X0, by, LW, bh], f'module:dq3:win/{LW}x{bh}', **hm))
    out.append(t(X0 + 24, by + 52, ('s', 28), bind='dq3.heal.msg', wrap_width=LW - 48, max_lines=4,
                 line_gap=36 - BASE, **hm))
    # no-map card (C): the composed title-flame page and the native objective in its window
    out.append(img([X0, FPAGE_Y, CW, FPAGE_H], src_bind='dq3.m.card', need_bind='dq3.mv.c'))
    out.append(t(X0, FPAGE_Y + CARD_OBJ_Y + CARD_OBJ_H / 2 + 11, ('b', 32), bind='dq3.m.obj', x_bind='dq3.m.obj.x',
                 need_bind='dq3.m.obj.on'))
    # place card (B): the place's still in the map frame, its name and region, the Town Map button (the full map
    # shows the place's town map); no Area map label, zoom, Reset or full-map controls without a map image
    p = dict(need_bind='dq3.mv.p')
    out.append(img([MX + FIN, FPAGE_Y + FIN, MW - 2 * FIN, FPAGE_H - 2 * FIN], src_bind='dq3.m.art', **p))
    out.append(img([MX, FPAGE_Y, MW, FPAGE_H], f'module:dq3:mframe/{MW}x{FPAGE_H}', **p))
    cxp, yb = MX + MW / 2, FPAGE_Y + FPAGE_H - FIN - MB - 30
    out.append(t(cxp, yb - 92, ('b', 54), bind='dq3.m.place', color=GOLD, outline=2, align='center', **p))
    out.append(t(cxp, yb - 46, ('s', 32), bind='dq3.m.region.t', outline=2, align='center', **p))
    bx2, by2 = cxp - TOWN_BTN_W / 2, FPAGE_Y + FPAGE_H - FIN - MB - 10
    tb = dict(need_bind='dq3.m.town.on')
    out.append(img([bx2, by2, TOWN_BTN_W, MB], f'module:dq3:townbtn/{TOWN_BTN_W}x{MB}', **tb))
    out.append(t(bx2 + 84, by2 + MB / 2 + 11, ('b', 32), bind='dq3.m.town', **tb))
    out.append(tap([bx2, by2, TOWN_BTN_W, MB], 'fullmap', **tb))
    # Every style shares the normal map box; fullscreen owns the entire bottom screen.
    boxes = (('n', MX, FPAGE_Y, MW, FPAGE_H), ('f', X0, Y0, CW, Y1 - Y0))
    for mode, bx, by, bw, bh in boxes:
        gm = dict(need_bind=f'dq3.mv.{mode}')
        # the hidden prefetch of the next map key (dq3_map_swap.h): 1 px, alpha 0, under the frame's fin, so the
        # host composes the new picture while dq3.m.img still shows the previous one
        out.append(img([bx + FIN // 2, by + FIN // 2, 1, 1], src_bind='dq3.m.img.next', color='#00FFFFFF',
                       id=f'dq3_map_next_{mode}', **gm))
        out.append(img([bx + FIN, by + FIN, bw - 2 * FIN, bh - 2 * FIN], src_bind='dq3.m.img',
                       id=f'dq3_map_{mode}', pan_zoom=True, min_zoom=1, max_zoom=8,
                       view_zoom_bind='dq3.m.view.zoom', view_cx_bind='dq3.m.view.center',
                       view_cy_bind='dq3.m.view.center', view_reset_bind='dq3.m.view.reset', **gm))
        out.append(img([bx, by, bw, bh], f'module:dq3:mframe/{bw}x{bh}', **gm))
    # Location and facility markers are part of the pannable map image. The overlay controls are drawn
    # opaque (window alpha 1.2 instead of 0.9) so the map does not show through.
    for mode, bx, by, bw, bh in boxes:
        gm = dict(need_bind=f'dq3.mv.{mode}')
        sy = by + bh - FIN - MB - 6
        sx = bx + FIN + 6
        # floor stepper: two 88 px halves, the floor name between
        sw = 2 * MB + 70
        out.append(img([sx, sy, sw, MB], src_bind='dq3.m.step.src', need_bind=f'dq3.m.step.{mode}'))
        out.append(t(sx + sw / 2, sy + MB / 2 + 13, ('b', 36), bind='dq3.m.step', color=GOLD, align='center',
                     need_bind=f'dq3.m.step.{mode}'))
        for dx, action in ((0, 'floor_up'), (sw - MB, 'floor_down')):
            out.append(tap([sx + dx, sy, MB, MB], action, need_bind=f'dq3.m.step.{mode}'))
        right = bx + bw - FIN - MB - 6
        top = by + FIN + 6
        if mode == 'n':
            out.append(img([right, top, MB, MB], f'module:dq3:mapbtn/{MB}', **gm))
        else:
            out.append(img([right, top, MB, MB], f'module:dq3:win/{MB}x{MB}/120', **gm))
            out.append(t(right + MB / 2, top + MB / 2 + 14, ('b', 44), text='×', align='center', **gm))
        out.append(tap([right, top, MB, MB], 'fullmap', **gm))
        # zoom label on a window sized to its text (dq3.m.zoom.box = 1 + the label's index in ZOOM_BOX)
        for k, zw in enumerate(ZOOM_BOX):
            out.append(img([bx + bw / 2 - zw / 2, top, zw, 44], f'module:dq3:win/{zw}x44/120', hide_bind='dq3.m.zoom.box',
                           keep_min=k + 1, keep_max=k + 1, **gm))
        out.append(t(bx + bw / 2, top + 31, ('s', 26), bind='dq3.m.zoom.label', align='center', **gm))
        rw = 124
        rx = right - (MB + G) - G - rw
        out.append(img([rx, sy, rw, MB], f'module:dq3:win/{rw}x{MB}/120', **gm))
        out.append(t(rx + rw / 2, sy + MB / 2 + 11, ('b', 30), text='Reset', align='center', **gm))
        out.append(tap([rx, sy, rw, MB], f'map_reset_{mode}', **gm))
        for dx, label, action, gate in ((-(MB + G), '+', 'map_in', 'dq3.m.zoom.in'), (0, '-', 'map_out', 'dq3.m.zoom.out')):
            x = right + dx
            out.append(img([x, sy, MB, MB], f'module:dq3:win/{MB}x{MB}/120', **gm))
            # Minus is ASCII for the native Latin font atlas.
            out.append(t(x + MB / 2, sy + MB / 2 + 14, ('b', 44), text=label, align='center', **gm))
            out.append(tap([x, sy, MB, MB], action, tap_enabled_bind=gate, **gm))
    return out


# ============================================================================ Party / Bag / Journal tabs
LINE, LINE2 = '#FFAA9164', '#FFCDB991'
SEP = '#FF6E6A5C'
ROW_SEP = '#FF4E4A3E'
SCROLL = dict(bar='#FF785A32', bar_track='#FFC8B48C', bar_w=6)
DARK_SCROLL = dict(bar='#FFE2C687', bar_track='#FF46423A', bar_w=6)
CELL_TEXT_PAD = 14                     # a cell name wraps at cell width - this (module CellTextPad)


def grid_geom(bx, bw, cols):
    """First cell x, cell width and scroll-bar x of a dark-window grid (render.py grid_geom)."""
    gx = bx + GI
    bar_x = bx + bw - 12 - DARK_SCROLL['bar_w']
    cell_w = (bar_x - 10 - gx - (cols - 1) * GAP) // cols
    return gx, cell_w, bar_x


def grid_scroll(sid, bx, bw, cols, count, show, reset, y0, cell_h, rows=3):
    """The grid viewport: the visible rows plus the glow margin; the bar at the window's right."""
    gx, cw, bar_x = grid_geom(bx, bw, cols)
    pitch = cell_h + GAP
    # the viewport ends where the next row starts, so no strip of it shows under the glow margin
    return dict({'id': sid, 'rect': [gx - GLOW, y0 - GLOW, bar_x + DARK_SCROLL['bar_w'] - (gx - GLOW),
                                     rows * pitch + GLOW],
                 'count_bind': count, 'cols': cols, 'row_h': pitch, 'pad': cell_h - pitch + 2 * GLOW,
                 'show_bind': show, 'reset_bind': reset}, **DARK_SCROLL)


def subtab_widgets(x, y, labels, widths, prefix, action, gate, **kw):
    """render.py subtabs(): 44 px boxes (= tap), Bold 26 gold on the selected one, Semibold 26 soft else."""
    out = []
    for k, (label, w) in enumerate(zip(labels, widths)):
        out.append(img([x, y, w, 44], f'module:dq3:box/{w}x44/sub', need_bind=f'{prefix}{k}.sel', **kw))
        out.append(t(x + w / 2, y + 22 + 9, ('b', 26), text=label, color=GOLD, align='center',
                     need_bind=f'{prefix}{k}.sel', **kw))
        out.append(t(x + w / 2, y + 22 + 9, ('s', 26), text=label, color=SOFT, align='center',
                     need_bind=f'{prefix}{k}.off', **kw))
        out.append(tap([x, y, w, 44], f'{action}_{k}', need_bind=gate, **kw))
        x += w + 8
    return out


# Party tab (option B): status scrap left, the member's window right
PLW = 566
PRX = X0 + PLW + G                      # 610
PRW = X1 - PRX                          # 594
PLX, PLY, PLIW = X0 + LPX, FPAGE_Y + LPY, PLW - 2 * LPX     # 62, 252, 514
SLOT_W, SLOT_H = (PLIW - 8) // 2, 80   # 253 x 80
SLOT_TW = SLOT_W - 58                   # 195 (module SlotTextW)
P_SUB_Y = FPAGE_Y + WP + 46             # 288
P_GRID_Y = P_SUB_Y + 44 + 12 + 6        # 350
P_CELL_H = 100
P_DET_Y = P_GRID_Y + 3 * (P_CELL_H + GAP) - GAP + 14   # 676
# Spells sub-tab (owner 2026-10-10, 0.10.2): a Bag-style list in the grid's place, the detail stays below
S_LX, S_TOP, S_RH = PRX + WP, P_GRID_Y, 50          # 624, 350, the Bag's row pitch
S_LW = grid_geom(PRX, PRW, 4)[2] + 6 - (S_LX - 8)    # 576: the viewport ends at the grid's scroll bar
S_ROWS = (P_DET_Y - 14 - S_TOP) // S_RH             # 6 visible rows
S_MP_W = 136                                        # "99 MP" Semibold 28 + 16 px gap
SPELL_NAME_W = S_LW - 54 - S_MP_W                   # 386 (module SpellNameW)
SUB_W = {'Carried': 130, 'Spells': 105, 'Items': 101, 'Equipment': 171, 'Important': 161}   # Bold 26 + 30


def party_detail_widgets():
    """The selection's detail (dq3.dt.*) under the member's grid: window box + icon, name, sub, fact,
    description (render.py detail())."""
    x, right = PRX + WP + 4, PRX + PRW - WP - 4
    y = P_DET_Y
    tx = x + 88
    g = dict(need_bind='dq3.dt.on')
    return [rect([x, y, right - x, 1], SEP, **g),
            img([x, y + 12, 72, 72], 'module:dq3:win/72x72', **g),
            img([x + 13, y + 25, 46, 46], bind='dq3.dt.ii', need_bind='dq3.dt.ion',
                src_thresholds=icon_steps('module:dq3:itemtype/%d/46', 12)),
            img([x + 13, y + 25, 46, 46], bind='dq3.dt.si', need_bind='dq3.dt.son',
                src_thresholds=icon_steps('module:dq3:spell/%d/46', 8)),
            img([x + 13, y + 25, 46, 46], bind='dq3.dt.si', need_bind='dq3.dt.zon',
                src_thresholds=icon_steps('module:dq3:spell/%d/46/g', 8)),
            t(tx, y + 44, ('b', 32), bind='dq3.dt.name', color=GOLD, **g),
            t(right, y + 42, ('s', 24), bind='dq3.dt.sub', color=SOFT, align='right', need_bind='dq3.dt.sub.on'),
            t(tx, y + 76, ('s', 26), bind='dq3.dt.f0', need_bind='dq3.dt.f0.on'),
            t(x, y + 124, ('s', 26), bind='dq3.dt.text', wrap_width=right - x, max_lines=4, line_gap=34 - BASE,
              need_bind='dq3.dt.text.on')]


def item_grid_widgets(bx, bw, cols, count, scroll, action, y0, cell_h):
    """The member's Carried grid: rows qg<field><i> (dq3_info_view.h); the Spells are a list (spell_list_widgets)."""
    gx, cw, _ = grid_geom(bx, bw, cols)
    rep = dict(repeat=ROWS, repeat_bind=count, repeat_cols=cols, repeat_dx=cw + GAP, repeat_row_dy=cell_h + GAP,
               scroll=scroll)
    box = [gx - GLOW, y0 - GLOW, cw + 2 * GLOW, cell_h + 2 * GLOW]
    return [img(box, f'module:dq3:ecell/-1/{cw}x{cell_h}/{cell_h - 14}/0/', **rep),
            img(box, f'module:dq3:ecell/-1/{cw}x{cell_h}/{cell_h - 14}/0/s', need_bind='qgx{i}', **rep),
            # item icon (36 px) centred above the name
            img([gx + (cw - 36) // 2, y0 + 8, 36, 36], bind='qgi{i}', need_bind='qgi{i}',
                src_thresholds=icon_steps('module:dq3:itemtype/%d/36', 12), **rep),
            # the name: one line (qgy = 13 px lower) or wrapped onto two (26 px pitch)
            t(gx + cw / 2, y0 + 55, ('s', 24), bind='qgn{i}', align='center', wrap_width=cw - CELL_TEXT_PAD,
              max_lines=2, line_gap=26 - BASE, y_bind='qgy{i}', **rep),
            dict(tap([gx, y0, cw, cell_h], action), payload='{i}', **rep)]


def spell_list_widgets():
    """The member's Spells as a list (rows qs<field><i>: n name, s / z spell icon + 1 (z greyed: battle-only or not
    learnt), m "N MP", x selected, l the separator above an unselected row), the Bag list's row style."""
    lx, top, rh = S_LX, S_TOP, S_RH
    rep = dict(repeat=ROWS, repeat_bind='dq3.ps.n', repeat_dy=rh, scroll='dq3.ps.list')
    return [rect([lx + 8, top, S_LW - 36, 1], ROW_SEP, need_bind='qsl{i}', **rep),
            img([lx - 8, top + 2 - 8, S_LW - 20 + 16, rh - 4 + 16], f'module:dq3:rowglow/{S_LW - 20}x{rh - 4}',
                need_bind='qsx{i}', **rep),
            img([lx + 8, top + 7, 36, 36], bind='qss{i}', need_bind='qss{i}',
                src_thresholds=icon_steps('module:dq3:spell/%d/36', 8), **rep),
            img([lx + 8, top + 7, 36, 36], bind='qsz{i}', need_bind='qsz{i}',
                src_thresholds=icon_steps('module:dq3:spell/%d/36/g', 8), **rep),
            t(lx + 54, top + 36, ('s', 30), bind='qsn{i}', **rep),
            t(lx + S_LW - 36, top + 36, ('s', 28), bind='qsm{i}', align='right', need_bind='qsm{i}.on', **rep),
            dict(tap([lx, top + 2, S_LW - 20, rh - 4], 'party_item'), payload='{i}', **rep)]


def party_tab_widgets():
    out = []
    gate = dict(need_bind='dq3.tab.party')
    on = dict(need_bind='dq3.pp.on')
    out.append(img([X0, FPAGE_Y, PLW, FPAGE_H], f'module:dq3:light/{PLW}x{FPAGE_H}', **gate))
    out.append(img([PRX, FPAGE_Y, PRW, FPAGE_H], f'module:dq3:win/{PRW}x{FPAGE_H}', **gate))
    x, y, w = PLX, PLY, PLIW
    # status: portrait frame + sprite (112), name, job . Lv, personality, Next lv
    out.append(img([x, y, 112, 112], src_bind='dq3.pp.portrait', **on))
    tx = x + 128
    out.append(t(tx, y + 30, ('b', 36), bind='dq3.pp.name', color=INK, outline=0, **on))
    out.append(t(tx, y + 62, ('s', 28), bind='dq3.pp.job', color=INK2, outline=0, **on))
    out.append(t(tx, y + 92, ('b', 28), bind='dq3.pp.pers', color=INK, outline=0, need_bind='dq3.pp.pers.on'))
    out.append(t(tx, y + 121, ('s', 26), bind='dq3.pp.next', color=INK2, outline=0, **on))
    # two-column stats (Resistances omitted: no native per-member value)
    names = ['Strength', 'Agility', 'Resilience', 'Wisdom', 'Luck', 'Attack', 'Defence', 'Max HP', 'Max MP', 'Gold']
    cw, rh, sy0 = (w - 20) / 2, 40, y + 132
    for k, n in enumerate(names):
        cx = x + (k % 2) * (cw + 20)
        cy = sy0 + (k // 2) * rh
        out.append(t(cx, cy + 30, ('s', 28), text=n, color=INK, outline=0, **on))
        out.append(t(cx + cw, cy + 30, ('b', 30), bind=f'dq3.pp.s{k}', color=INK, outline=0, align='right', **on))
        out.append(rect([cx, cy + rh - 2, cw, 1], LINE2, **on))
    # the six equipped slots (2 x 3): native slot names, item icon on a small dark window, item name
    ey = sy0 + 5 * rh + 6
    out.append(t(x, ey + 26, ('b', 28), bind='dq3.pp.eqh', color=INK2, outline=0, **on))
    gy = ey + 36
    for k in range(6):
        bx, by = x + (k % 2) * (SLOT_W + 8), gy + (k // 2) * (SLOT_H + 6)
        e = f'dq3.pp.e{k}'
        out.append(img([bx, by, SLOT_W, SLOT_H], f'module:dq3:box/{SLOT_W}x{SLOT_H}/slot', need_bind=f'{e}.off'))
        out.append(img([bx, by, SLOT_W, SLOT_H], f'module:dq3:box/{SLOT_W}x{SLOT_H}/slotsel', need_bind=f'{e}.sel'))
        out.append(img([bx + 6, by + 20, 40, 40], 'module:dq3:win/40x40/90', **on))
        out.append(img([bx + 10, by + 24, 32, 32], bind=f'{e}.i', need_bind=f'{e}.on',
                       src_thresholds=icon_steps('module:dq3:itemtype/%d/32', 12)))
        out.append(t(bx + 54, by + 32, ('s', 24), bind=f'{e}.l', color=INK2, outline=0, **on))
        out.append(t(bx + 54, by + 64, ('s', 24), bind=f'{e}.n', color=INK, outline=0, need_bind=f'{e}.on'))
        out.append(tap([bx, by, SLOT_W, SLOT_H], f'party_slot_{k}', need_bind=f'{e}.on'))
    # the member's dark window: "<Name>'s Items / Spells" + count, Carried / Spells, grid, detail
    pr = dict(need_bind='dq3.pr.on')
    out.append(img([PRX + WP, FPAGE_Y + WP + 4, 40, 32], src_bind='dq3.pr.head', **pr))
    out.append(t(PRX + WP + 48, FPAGE_Y + WP + 30, ('b', 32), bind='dq3.pr.title', color=GOLD, **pr))
    out.append(t(PRX + PRW - WP, FPAGE_Y + WP + 28, ('s', 24), bind='dq3.pr.count', color=SOFT, align='right', **pr))
    out += subtab_widgets(PRX + WP, P_SUB_Y, ['Carried', 'Spells'], [SUB_W['Carried'], SUB_W['Spells']], 'dq3.pr.t',
                          'party_tab', 'dq3.pr.on')
    gx = grid_geom(PRX, PRW, 4)[0]
    out.append(t(gx, P_GRID_Y + 34, ('s', 26), bind='dq3.pr.none', color=SOFT, need_bind='dq3.pr.empty'))
    out += item_grid_widgets(PRX, PRW, 4, 'dq3.pr.n', 'dq3.pr.grid', 'party_item', P_GRID_Y, P_CELL_H)
    out += spell_list_widgets()
    out += party_detail_widgets()
    return out


def party_scrolls():
    # the Spells list viewport: the rows plus the selection glow, as bag_scrolls()
    return [grid_scroll('dq3.pr.grid', PRX, PRW, 4, 'dq3.pr.n', 'dq3.pr.on', 'dq3.pr.key', P_GRID_Y, P_CELL_H),
            dict({'id': 'dq3.ps.list', 'rect': [S_LX - 8, S_TOP - 6, S_LW, S_ROWS * S_RH + 12], 'count_bind': 'dq3.ps.n',
                  'row_h': S_RH, 'pad': 12, 'show_bind': 'dq3.ps.on', 'reset_bind': 'dq3.pr.key'}, **DARK_SCROLL)]


# Bag tab: list (left) + description pane (right), sub-tabs in the title row
B_TOP = FPAGE_Y + WP + 44 + 12          # 298 first list row
B_LX, B_LW, B_RH = X0 + WP, 600, 50     # list x, width, row pitch
B_ROWS = (FPAGE_B - WP - B_TOP) // B_RH # 12 visible rows
B_NAME_W = B_LW - 54 - 110              # 436 (module BagNameW)
B_PX = B_LX + B_LW + 12                 # 662 description pane
B_PW = X1 - WP - B_PX                   # 528
B_PANE_NAME_W = B_PW - 108              # 420 (module BagPaneNameW)
CARRIED_BY_W = 122                      # Semibold 24 "Carried by"


def bag_tab_widgets():
    """The Bag tab: the three shared bags (dq3.bg.*) as a list with the selection's description (dq3.bd.*)."""
    g = dict(need_bind='dq3.bg.on')
    out = [img([X0, FPAGE_Y, CW, FPAGE_H], f'module:dq3:win/{CW}x{FPAGE_H}', need_bind='dq3.tab.bag'),
           img([X0 + WP, FPAGE_Y + WP + 4, 38, 31], 'module:dq3:ui/bag/38x31', **g),
           t(X0 + WP + 50, FPAGE_Y + WP + 30, ('b', 32), bind='dq3.bg.title', color=GOLD, **g),
           t(X1 - WP, FPAGE_Y + WP + 30, ('s', 26), bind='dq3.bg.count', color=SOFT, align='right', **g)]
    # sub-tabs after the bag's title (dq3.bg.subx = the title's width + 30)
    out += subtab_widgets(X0 + WP + 50, FPAGE_Y + WP, ['Items', 'Equipment', 'Important'],
                          [SUB_W['Items'], SUB_W['Equipment'], SUB_W['Important']], 'dq3.bg.b', 'bag_page', 'dq3.bg.on',
                          x_bind='dq3.bg.subx')
    out.append(t(B_LX + 10, B_TOP + 36, ('s', 28), bind='dq3.bg.none', color=SOFT, need_bind='dq3.bg.empty'))
    # list rows qb<field><i>: n name, i icon + 1, c "xN", x selected, l separator above the row
    lx, top, rh = B_LX, B_TOP, B_RH
    rep = dict(repeat=ROWS, repeat_bind='dq3.bg.n', repeat_dy=rh, scroll='dq3.bg.list')
    out += [rect([lx + 8, top, B_LW - 36, 1], ROW_SEP, need_bind='qbl{i}', **rep),
            img([lx - 8, top + 2 - 8, B_LW - 20 + 16, rh - 4 + 16], f'module:dq3:rowglow/{B_LW - 20}x{rh - 4}',
                need_bind='qbx{i}', **rep),
            img([lx + 10, top + 9, 32, 32], bind='qbi{i}', need_bind='qbi{i}',
                src_thresholds=icon_steps('module:dq3:itemtype/%d/32', 12), **rep),
            t(lx + 54, top + 36, ('s', 30), bind='qbn{i}', **rep),
            t(lx + B_LW - 36, top + 36, ('s', 28), bind='qbc{i}', align='right', **rep),
            dict(tap([lx, top + 2, B_LW - 20, rh - 4], 'bag_item'), payload='{i}', **rep)]
    # description pane
    px0, top = B_PX, B_TOP
    d = dict(need_bind='dq3.bd.on')
    out += [rect([px0 - 6, top, 1, FPAGE_B - WP - top], SEP, **g),
            img([px0 + 8, top + 4, 84, 84], 'module:dq3:win/84x84', **d),
            img([px0 + 23, top + 19, 54, 54], bind='dq3.bd.ii', need_bind='dq3.bd.ii',
                src_thresholds=icon_steps('module:dq3:itemtype/%d/54', 12)),
            t(px0 + 108, top + 38, ('b', 34), bind='dq3.bd.name', color=GOLD, **d),
            t(px0 + 108, top + 76, ('s', 26), bind='dq3.bd.held', need_bind='dq3.bd.held.on'),
            t(px0 + 8, top + 132, ('s', 24), text='Carried by', color=SOFT, **d),
            t(px0 + 8 + CARRIED_BY_W + 14, top + 132, ('s', 26), bind='dq3.bd.by', **d),
            rect([px0 + 8, top + 152, X1 - WP - (px0 + 8), 1], ROW_SEP, **d),
            t(px0 + 8, top + 194, ('s', 28), bind='dq3.bd.fact', color=GOLD, need_bind='dq3.bd.fact.on'),
            t(px0 + 8, top + 194, ('s', 28), bind='dq3.bd.text', wrap_width=B_PW - 16, max_lines=11, line_gap=36 - BASE,
              need_bind='dq3.bd.text.on', y_bind='dq3.bd.ty')]
    return out


def bag_scrolls():
    # viewport: the rows plus the selection glow (8 px); pad = the last row's glow below its pitch
    return [dict({'id': 'dq3.bg.list', 'rect': [B_LX - 8, B_TOP - 6, B_LW, B_ROWS * B_RH + 12], 'count_bind': 'dq3.bg.n',
                  'row_h': B_RH, 'pad': 12, 'show_bind': 'dq3.bg.on', 'reset_bind': 'dq3.bg.key'}, **DARK_SCROLL)]


# Journal: Next step / Traveller's Tips / Mini medals scrap (left), Records . Monsters window (right)
JLW = 470
JRX = X0 + JLW + G                      # 514
JRW = X1 - JRX                          # 690
JLX, JLY, JLIW = X0 + LPX, FPAGE_Y + LPY, JLW - 2 * LPX   # 62, 252, 418
J_TIPS_Y = JLY + 154                    # Traveller's Tips block (room for a two-line objective + sub line)
J_TIP_PITCH = 44
J_GRID_Y = FPAGE_Y + WP + 50            # 292
J_CELL_H = 92
J_GRID_X, J_CELL_W, J_BAR_X = grid_geom(JRX, JRW, 4)
J_DET_Y = J_GRID_Y + 3 * (J_CELL_H + GAP) - GAP + 14      # 594


def journal_tab_widgets():
    out = []
    gate = dict(need_bind='dq3.tab.journal')
    on = dict(need_bind='dq3.jn.on')
    out.append(img([X0, FPAGE_Y, JLW, FPAGE_H], f'module:dq3:light/{JLW}x{FPAGE_H}', **gate))
    out.append(img([JRX, FPAGE_Y, JRW, FPAGE_H], f'module:dq3:win/{JRW}x{FPAGE_H}', **gate))
    x, y, w = JLX, JLY, JLIW
    # Next step: the native objective banner (two lines) and its first sub line (dq3.jo.sub.y = 36 under a
    # two-line objective)
    out.append(img([x, y + 2, 34, 34], 'module:dq3:ui/nextguide/34x34', **on))
    out.append(t(x + 46, y + 32, ('b', 34), text='Next step', color=INK, outline=0, **on))
    out.append(t(x, y + 76, ('s', 30), bind='dq3.jo.main', color=INK, outline=0, wrap_width=w, max_lines=2,
                 line_gap=36 - BASE, need_bind='dq3.jo.on'))
    out.append(t(x, y + 112, ('s', 26), bind='dq3.jo.sub', color=INK2, outline=0, wrap_width=w, max_lines=1,
                 y_bind='dq3.jo.sub.y', need_bind='dq3.jo.sub.on'))
    out.append(t(x, y + 76, ('s', 28), text='No current objective.', color=INK2, outline=0, need_bind='dq3.jo.none'))
    yy = J_TIPS_Y
    out.append(rect([x, yy - 16, w, 1], LINE, **on))
    # Traveller's Tips: the native list (unread = "new"), four rows visible
    out.append(img([x, yy, 38, 38], 'module:dq3:itemicon/6/38', **on))
    out.append(t(x + 48, yy + 31, ('b', 34), text="Traveller's Tips", color=INK, outline=0, **on))
    rep = dict(repeat=ROWS, repeat_bind='dq3.jt.count', repeat_dy=J_TIP_PITCH, scroll='dq3.jt.list')
    out.append(t(x + 6, yy + 82, ('s', 30), bind='qtn{i}', color=INK, outline=0, wrap_width=w - 70, max_lines=1, **rep))
    out.append(t(x + w, yy + 82, ('b', 28), text='new', color=NEW_INK, outline=0, align='right', need_bind='qtw{i}', **rep))
    ly = yy + 82 + 4 * J_TIP_PITCH - 16
    out.append(rect([x, ly, w, 1], LINE, **on))
    # Mini medals and the next reward
    my = ly + 14
    out.append(img([x, my + 2, 36, 36], 'module:dq3:ui/medal/36x36', **on))
    out.append(t(x + 48, my + 32, ('b', 34), text='Mini medals', color=INK, outline=0, **on))
    out.append(t(x + w, my + 32, ('b', 32), bind='dq3.jm.count', color=INK, outline=0, align='right', **on))
    out.append(t(x, my + 74, ('s', 28), bind='dq3.jm.next', color=INK2, outline=0, wrap_width=w, max_lines=2,
                 line_gap=36 - BASE, need_bind='dq3.jm.next.on'))
    # Records . Monsters (dark window): scrollable 4-column grid and the selected monster
    out.append(t(JRX + WP, FPAGE_Y + WP + 30, ('b', 32), text='Records · Monsters', color=GOLD, **on))
    out.append(t(JRX + JRW - WP, FPAGE_Y + WP + 28, ('s', 26), bind='dq3.jr.counts', color=SOFT, align='right', **on))
    gx, cw, ch = J_GRID_X, J_CELL_W, J_CELL_H
    rep = dict(repeat=192, repeat_bind='dq3.jr.count', repeat_cols=4, repeat_dx=cw + GAP, repeat_row_dy=ch + GAP,
               scroll='dq3.jr.grid')
    box = [gx - GLOW, J_GRID_Y - GLOW, cw + 2 * GLOW, ch + 2 * GLOW]
    # one int per cell: the sprite index (ecell key), -1 = not recorded ("?" slime)
    out.append(img(box, bind='qc{i}', src_format=f'module:dq3:ecell/%d/{cw}x{ch}/{ch - 14}/0/', **rep))
    out.append(img([gx + (cw - 52) // 2, J_GRID_Y + (ch - 52) // 2, 52, 52], bind='qc{i}',
                   src_thresholds=[{'le': -1, 'src': 'module:dq3:shadowq/52'}, {'le': 1 << 30, 'src': ''}], **rep))
    out.append(img(box, f'module:dq3:ecell/-1/{cw}x{ch}/{ch - 14}/0/s', need_bind='qcs{i}', **rep))
    out.append(dict(tap([gx, J_GRID_Y, cw, ch], 'journal_mon'), payload='{i}', **rep))
    dy = J_DET_Y
    right = JRX + JRW - WP - 4
    out.append(rect([gx, dy, right - gx, 1], SEP, **on))
    d = dict(need_bind='dq3.jr.d.on')
    out.append(img([gx, dy + 16, 110, 110], src_bind='dq3.jr.d.sprite', **d))
    out.append(img([gx + 29, dy + 45, 52, 52], 'module:dq3:shadowq/52', need_bind='dq3.jr.d.q'))
    tx = gx + 124
    out.append(t(tx, dy + 42, ('b', 32), bind='dq3.jr.d.name', color=GOLD, **d))
    out.append(t(tx, dy + 76, ('s', 26), bind='dq3.jr.d.hab', wrap_width=right - tx, max_lines=2, line_gap=30 - BASE,
                 need_bind='dq3.jr.d.hab.on'))
    out.append(t(tx, dy + 136, ('s', 26), bind='dq3.jr.d.drop', need_bind='dq3.jr.d.more'))
    out.append(t(tx, dy + 166, ('s', 26), bind='dq3.jr.d.stat', color=SOFT, need_bind='dq3.jr.d.more'))
    out.append(t(gx, dy + 202, ('s', 26), bind='dq3.jr.d.flav', wrap_width=right - gx, max_lines=5, line_gap=30 - BASE,
                 need_bind='dq3.jr.d.flav.on'))
    return out


def journal_scrolls():
    x, w, yy = JLX, JLIW, J_TIPS_Y
    return [dict({'id': 'dq3.jt.list', 'rect': [x, yy + 82 - 32, w + 12, 4 * J_TIP_PITCH], 'count_bind': 'dq3.jt.count',
                  'row_h': J_TIP_PITCH, 'show_bind': 'dq3.jn.on'}, **SCROLL),
            grid_scroll('dq3.jr.grid', JRX, JRW, 4, 'dq3.jr.count', 'dq3.jn.on', 'dq3.jr.key', J_GRID_Y, J_CELL_H)]


# ============================================================================ system screens (owner option A)
# Design milestone 20261007T220416+0700-system-screens (research tools/render-system-screens.py): start-A,
# loading-A, wrongpatch-A as full-canvas module images (dq3_system.h, text drawn into them), and the
# wrong-patch host-safe fallback (flat colours, drawn shapes, the host's built-in A-Z font) for when the module
# is not running (host refusal: module_error) or cannot decode the game art (dq3.sys.flat).
SYS_OK = (420, 786, 220, 72)            # dq3_system.h SysOkX / SysOkY / SysOkW / SysOkH
FB_BG, FB_TAN, FB_LINE, FB_INK = '#FF141826', '#FFB09666', '#FF78603C', '#FF342616'
FB_INK2, FB_BOX, FB_CREAM, FB_GOLD = '#FF5A462A', '#FF101214', '#FFECE8DC', '#FFE2C687'


def system_derived():
    """Gates of the refusal screens (runtime 17 expr). A missing dq3.sys.* (module absent) is closed."""
    fb = '(module_error == 1 || dq3.sys.flat == 1)'
    return [{'name': 'dq3.wp.notice', 'expr': 'dq3.sys.wrong == 1 && @flag:dq3_wp_ok == 0'},
            {'name': 'dq3.wp.quiet', 'expr': 'dq3.sys.wrong == 1 && @flag:dq3_wp_ok == 1'},
            {'name': 'dq3.fb.on', 'expr': fb},
            {'name': 'dq3.fb.notice', 'expr': f'{fb} && @flag:dq3_wp_ok == 0'},
            {'name': 'dq3.fb.quiet', 'expr': f'{fb} && @flag:dq3_wp_ok == 1'},
            # the host's own refusal text exists only when the host refused the module
            {'name': 'dq3.fb.host', 'expr': 'module_error == 1 && @flag:dq3_wp_ok == 0'}]


def blabel(x, y, text, scale, color, gate, align='center', **kw):
    """Host built-in 3 x 5 font label (A-Z, digits, : - . / % + ,): 4 * scale px per character, 5 * scale tall."""
    assert all(c.isupper() or c.isdigit() or c in ' :-./%+,' for c in text), text
    w = {'type': 'label', 'rect': [x, y, 0, 0], 'text': text, 'text_scale': scale, 'color': color,
         'need_bind': gate, **kw}
    if align != 'left':
        w['align'] = align
    return w


def brect(r, gate, bg='#00000000', color='#00000000', frame=0, **kw):
    return {'type': 'rect', 'rect': list(r), 'bg': bg, 'color': color, 'frame': frame, 'need_bind': gate, **kw}


def system_widgets():
    full = [0, 0, W, H]
    # prewarm: the screen that can follow is drawn 1 x 1 px under the current one, so the host has decoded it
    # before it is needed (a title save load lasts ~3 s; composing a screen takes ~1 s on the asset worker)
    out = [img([0, 0, 1, 1], f'module:dq3:sys/loading/{W}x{H}', need_bind='dq3.sys.start'),
           img([0, 0, 1, 1], f'module:dq3:sys/start/{W}x{H}', need_bind='dq3.sys.loading'),
           img([0, 0, 1, 1], f'module:dq3:sys/quiet/{W}x{H}', need_bind='dq3.wp.notice'),
           img([0, 0, 1, 1], f'module:dq3:sys/wrong/{W}x{H}', need_bind='dq3.wp.quiet')]
    out += [img(full, f'module:dq3:sys/start/{W}x{H}', need_bind='dq3.sys.start'),
           img(full, f'module:dq3:sys/loading/{W}x{H}', need_bind='dq3.sys.loading'),
           img(full, f'module:dq3:sys/wrong/{W}x{H}', need_bind='dq3.wp.notice'),
           img(full, f'module:dq3:sys/quiet/{W}x{H}', need_bind='dq3.wp.quiet'),
           brect(SYS_OK, 'dq3.wp.notice', on_tap='wp_ok'),
           brect(full, 'dq3.wp.quiet', on_tap='wp_ok')]
    # host-safe fallback (wrongpatch-A-fallback): flat tan frame, box, drawn "?", built-in font
    on, notice, quiet = 'dq3.fb.on', 'dq3.fb.notice', 'dq3.fb.quiet'
    out += [brect(full, on, bg=FB_BG), brect([40, 40, W - 80, H - 80], on, bg=FB_TAN),
            brect([64, 64, W - 128, H - 128], on, color=FB_LINE, frame=3),
            blabel(W // 2, 130, 'DRAGON QUEST III', 12, FB_INK, on),
            blabel(W // 2, 230, 'HD-2D REMAKE - EDEN DUO COMPANION', 5, FB_INK2, on),
            blabel(W // 2, 940, 'SUPPORTED: VER. 1.1.0.0 - BUILD 4F41309B39EEBE5E', 4, FB_INK2, on)]
    bx, by, bw, bh = 110, 340, 1020, 520
    out += [brect([bx, by, bw, bh], notice, bg=FB_BOX, color=FB_CREAM, frame=4),
            brect([bx + 70, by + 110, 180, 180], notice, color=FB_CREAM, frame=5)]
    # "?" as the built-in font's 3 x 5 cells would draw it ({7, 1, 2, 0, 2}), 24 px per cell, centred in the ring
    qx, qy, c = bx + 70 + 90 - 36, by + 110 + 90 - 60, 24
    for cx, cy, cw in ((0, 0, 3), (2, 1, 1), (1, 2, 1), (1, 4, 1)):
        out.append(brect([qx + cx * c, qy + cy * c, cw * c, c], notice, bg=FB_CREAM))
    tx = bx + 310
    # scale 4 also fits when the module is running with its game font (dq3.sys.flat), which is wider than the
    # host's 3 x 5 cells
    out += [blabel(tx, by + 60, 'THIS VERSION IS NOT SUPPORTED', 4, FB_GOLD, notice, 'left'),
            blabel(tx, by + 130, 'THIS COMPANION IS MADE FOR', 4, FB_CREAM, notice, 'left'),
            blabel(tx, by + 166, 'VER. 1.1.0.0.', 4, FB_CREAM, notice, 'left'),
            blabel(tx, by + 216, 'UPDATE THE GAME TO USE IT.', 4, FB_CREAM, notice, 'left'),
            # the host's own reason (module_error_message), drawn by the host: verifiable, never invented
            {'type': 'label', 'rect': [tx, by + 270, 0, 0], 'bind_text': 'module_error_message', 'text_scale': 3,
             'color': '#FFB0AA96', 'wrap_width': bw - 340, 'max_lines': 3, 'need_bind': 'dq3.fb.host'}]
    ox, oy = tx, by + 380
    out += [brect([ox, oy, 200, 68], notice, bg=FB_BOX, color=FB_CREAM, frame=3, on_tap='wp_ok'),
            brect([ox + 18, oy + 12, 44, 44], notice, bg=FB_CREAM),
            blabel(ox + 40, oy + 22, 'A', 5, FB_BOX, notice),
            blabel(ox + 132, oy + 19, 'OK', 6, FB_CREAM, notice),
            blabel(W // 2, 520, 'THE COMPANION IS OFF FOR THIS GAME.', 5, FB_INK, quiet),
            blabel(W // 2, 580, 'TAP ANYWHERE TO SHOW THE NOTICE AGAIN.', 4, FB_INK2, quiet),
            brect(full, quiet, on_tap='wp_ok')]
    return out


def field_actions():
    acts = {f'ftab_{k}': {'kind': 'module', 'action': 'ftab', 'argument': k} for k in range(FTABS)}
    for k in range(4):
        acts[f'party_drop_{k}'] = {'kind':'module','action':f'partydrop{k}','argument':'$payload',
                                 'enabled_bind':'dq3.party.reorder'}
    acts['party_pick'] = {'kind': 'module', 'action': 'partypick', 'argument': '$payload'}
    for k in range(2):
        acts[f'party_tab_{k}'] = {'kind': 'module', 'action': 'partytab', 'argument': k}
    for k in range(6):
        acts[f'party_slot_{k}'] = {'kind': 'module', 'action': 'partyslot', 'argument': k}
    for k in range(3):
        acts[f'bag_page_{k}'] = {'kind': 'module', 'action': 'bagpage', 'argument': k}
    acts['party_item'] = {'kind': 'module', 'action': 'partyitem', 'argument': '$payload'}
    acts['bag_item'] = {'kind': 'module', 'action': 'bagitem', 'argument': '$payload'}
    acts['journal_mon'] = {'kind': 'module', 'action': 'journalmon', 'argument': '$payload'}
    for k, b in enumerate(('X', 'DRight', 'DDown', 'A', 'DLeft', 'B')):
        for p in range(2):
            acts[f'party_press_{k}_{p}'] = {'kind': 'button', 'button': b, 'frames': 5,
                'counter': f'dq3_party_button{k}', 'target': p, 'modulo': 2,
                'enabled_bind': f'dq3.party.s{k}p{p}'}
    acts['floor_up'] = {'kind': 'module', 'action': 'floor', 'argument': -1}
    acts['floor_down'] = {'kind': 'module', 'action': 'floor', 'argument': 1}
    acts['map_in'] = {'kind': 'module', 'action': 'mapzoom', 'argument': -1}
    acts.update({f'heal_{k}': {'kind': 'module', 'action': 'heal', 'argument': k,
                             'enabled_bind': 'dq3.heal.ready'} for k in range(2)})
    for mode in ('n', 'f'):
        acts[f'map_reset_{mode}'] = {'kind': 'view_reset', 'view': f'dq3_map_{mode}'}
    acts['map_out'] = {'kind': 'module', 'action': 'mapzoom', 'argument': 1}
    acts['fullmap'] = {'kind': 'module', 'action': 'fullmap', 'argument': 0}
    return acts


# ============================================================================ battle page
# render.py battle_tactical() / battle_follow(): Tactical / Follow toggle, the Enemies box (2 x 5 cells,
# centred rows), the detail scraps on a selection, the party cards with their moves. Follow's enemy turn
# (dq3.b.swap) puts the party cards on top and the Enemies box at the bottom.
HEAD_H = 88
TABS_W = 400
TABS_X = X1 - TABS_W                    # 804
ENEMY_Y = Y0 + HEAD_H + G               # 128
BOX_H, BARE_H = 244, 172                # Enemies box with names (2 rows of 100 px) / without (party mode)
CELL_H, BARE_CELL_H = 100, 64
ENEMY_CW = int((CW - 2 * GI - 4 * GAP) / 5)   # 221
SHORT_H, FULL_H = 150, 280
MOVES_H = 92
PARTY_BOTTOM = Y1 - MOVES_H             # 956
SHORT_Y = PARTY_BOTTOM - SHORT_H        # 806
# Follow enemy-turn layout (dq3_battle_view.h bl::Swap*)
SWAP_PARTY_BOTTOM = ENEMY_Y + SHORT_H   # 278
SWAP_DETAIL_Y = SWAP_PARTY_BOTTOM + MOVES_H + 4   # 374
SWAP_ENEMY_Y = Y1 - BOX_H               # 804: the box ends on the page bottom
STRIP_W, STRIP_H = 252, 48              # the "Enemies" heading's TitleBG strip
STRIP_Y = Y0 + HEAD_H - STRIP_H         # 72 (Tactical: left of the toggle)
SWAP_STRIP_Y = SWAP_ENEMY_Y - G - STRIP_H     # 748
NORMAL_DETAIL_Y = ENEMY_Y + BOX_H + G   # 380
NORMAL_DETAIL_H = SHORT_Y - G - NORMAL_DETAIL_Y   # 418
SWAP_DETAIL_H = SWAP_STRIP_Y - 6 - SWAP_DETAIL_Y  # 368
PARTY_DETAIL_Y = ENEMY_Y + BARE_H + G   # 308
AFF_W, AFF_H = 172, 116                 # affinity cell (2 x 3 grid, the owner's bigger enemy card)
# affinity element icon (0.10.1, owner "why frizz sizz bang use same icon"): module:dq3:elem/<k>/AFF_ICON at
# (AFF_IX, AFF_IY) of the cell, the family name Semibold 30 right of it, the state word under both
AFF_ICON, AFF_IX, AFF_IY, AFF_NX, AFF_NY = 56, 8, 8, 70, 46
ENEMY_SPELL_W = 139                     # dq3_battle_view.h bl::EnemySpellW (column 151, row 290)
NORMAL = dict(hide_bind='dq3.b.swap', hide_eq=1)
SWAP = dict(hide_bind='dq3.b.swap', keep_min=1)


def battle_widgets():
    out = [img([0, 0, W, H], f'module:dq3:scrap/{W}x{H}')]
    # header: manual Tactical or native acting-unit Follow
    out.append(img([TABS_X, Y0, TABS_W, HEAD_H], src_bind='dq3.b.tabs'))
    out.append(t(0, Y0 + HEAD_H / 2 + 12, ('b', 34), bind='dq3.b.tab0', color=GOLD, x_bind='dq3.b.tab0x'))
    out.append(t(0, Y0 + HEAD_H / 2 + 12, ('b', 34), bind='dq3.b.tab1', color=SOFT, x_bind='dq3.b.tab1x'))
    for k in range(2):
        out.append(tap([TABS_X + k * TABS_W // 2, Y0, TABS_W // 2, HEAD_H], f'bfollow_{k}'))
    # "Enemies" on the game's TravelInfo TitleBG strip (owner: a native dark backing, not the bare scrap)
    for sy, gate in ((STRIP_Y, NORMAL), (SWAP_STRIP_Y, SWAP)):
        out.append(img([X0, sy, STRIP_W, STRIP_H], f'module:dq3:titlebg/{STRIP_W}x{STRIP_H}', **gate))
        out.append(t(X0 + STRIP_W / 2, sy + STRIP_H / 2 + 12, ('b', 34), text='Enemies', align='center', **gate))
    out.append(img([X0, ENEMY_Y, CW, BOX_H], f'module:dq3:win/{CW}x{BOX_H}', need_bind='dq3.b.names', **NORMAL))
    out.append(img([X0, ENEMY_Y, CW, BARE_H], f'module:dq3:win/{CW}x{BARE_H}', need_bind='dq3.b.bare', **NORMAL))
    out.append(img([X0, SWAP_ENEMY_Y, CW, BOX_H], f'module:dq3:win/{CW}x{BOX_H}', need_bind='dq3.b.names', **SWAP))
    # enemy cells: composed cell (outline / glow + sprite) GLOW px larger on every side, tap rect, name
    for kind, h in (('n', CELL_H), ('b', BARE_CELL_H)):
        out.append(img([-GLOW, -GLOW, ENEMY_CW + 2 * GLOW, h + 2 * GLOW], src_bind='dq3.e{i}.img', x_bind='dq3.e{i}.x',
                       y_bind='dq3.e{i}.y', need_bind=f'dq3.e{{i}}.on_{kind}', repeat=10))
        out.append(tap([0, 0, ENEMY_CW, h], 'bsel_e{i}', x_bind='dq3.e{i}.x', y_bind='dq3.e{i}.y',
                       need_bind=f'dq3.e{{i}}.on_{kind}', repeat=10))
    out.append(t(0, BASE, ('s', 26), bind='dq3.e{i}.name', align='center', x_bind='dq3.e{i}.lx', y_bind='dq3.e{i}.ly',
                 need_bind='dq3.e{i}.on_n', repeat=10))
    for n in range(1, 5):
        out += battle_party(n)
    out += enemy_detail(NORMAL_DETAIL_Y, NORMAL_DETAIL_H, NORMAL)
    out += enemy_detail(SWAP_DETAIL_Y, SWAP_DETAIL_H, SWAP)
    out += party_detail()
    return out


def battle_party(n):
    """The battle party row for N members (owner 2026-10-10, same rule as the field strip): N cards of
    strip_w(N) across the content box. Gates: dq3.b.pn = N (both layouts), dq3.b.pl = N in the normal layout
    and N + 4 in the Follow enemy-turn swap (the module publishes both). Rows are centred on the card
    (module offsets nx / st<k>.x are relative to the card centre); N 1-2 put HP and MP side by side."""
    cw = CARDS_W[n]
    rep = dict(repeat=n, repeat_dx=cw + G)
    ALL = dict(hide_bind='dq3.b.pn', keep_min=n, keep_max=n)
    NRM = dict(hide_bind='dq3.b.pl', keep_min=n, keep_max=n)
    SWP = dict(hide_bind='dq3.b.pl', keep_min=n + 4, keep_max=n + 4)
    out = []
    full_y = PARTY_BOTTOM - FULL_H
    out.append(img([X0, full_y, cw, FULL_H], f'module:dq3:win/{cw}x{FULL_H}', need_bind='dq3.bp{i}.full', **rep, **ALL))
    out.append(img([X0, SHORT_Y, cw, SHORT_H], f'module:dq3:win/{cw}x{SHORT_H}', need_bind='dq3.bp{i}.short',
                   **rep, **NRM))
    swap_short_y = SWAP_PARTY_BOTTOM - SHORT_H
    out.append(img([X0, swap_short_y, cw, SHORT_H], f'module:dq3:win/{cw}x{SHORT_H}',
                   need_bind='dq3.bp{i}.short', **rep, **SWP))
    grow_h = Y1 - SHORT_Y
    out.append(img([X0, SHORT_Y, cw, grow_h], f'module:dq3:win/{cw}x{grow_h}', need_bind='dq3.bp{i}.grow', **rep, **ALL))
    out.append(img([X0, SHORT_Y, cw, grow_h], f'module:dq3:outline/{cw}x{grow_h}', need_bind='dq3.bp{i}.grow',
                   **rep, **ALL))
    cx = X0 + cw // 2
    out.append(img([cx - 80, full_y + 14, 160, 116], src_bind='dq3.bp{i}.sp_full', need_bind='dq3.bp{i}.full', **rep, **ALL))
    out.append(img([cx - 80, SHORT_Y + SHORT_H - 6, 160, 84], src_bind='dq3.bp{i}.sp_below', need_bind='dq3.bp{i}.grow',
                   **rep, **ALL))
    out.append(tap([X0, full_y, cw, FULL_H], 'bsel_p{i}', need_bind='dq3.bp{i}.full', **rep, **ALL))
    out.append(tap([X0, SHORT_Y, cw, grow_h], 'bsel_p{i}', need_bind='dq3.bp{i}.tap_s', **rep, **NRM))
    out.append(tap([X0, swap_short_y, cw, SHORT_H], 'bsel_p{i}', need_bind='dq3.bp{i}.tap_s', **rep, **SWP))
    # rows relative to the name baseline nb: name Bold 34 at nb, HP / MP labels at nb + 47 + k * 42,
    # values at nb + 49, bars at nb + 33 (render.py battle_card)
    nb = dict(y_bind='dq3.bp{i}.ny', need_bind='dq3.bp{i}.on', **rep, **ALL)
    out.append(t(cx, 0, ('b', 34), bind='dq3.bp{i}.name', align='center', x_bind='dq3.bp{i}.nx', **nb))
    for k in range(3):
        out.append(img([cx, -27, 30, 30], src_bind=f'dq3.bp{{i}}.st{k}.src', x_bind=f'dq3.bp{{i}}.st{k}.x',
                       need_bind=f'dq3.bp{{i}}.st{k}.on', y_bind='dq3.bp{i}.ny', **rep, **ALL))
    px0, pw = X0 + WP, cw - 2 * WP
    if n >= 3:
        cols = [(px0, 38 + k * 42, pw) for k in range(2)]
    else:
        # wide: HP | MP side by side on the first row, centred block of at most 2 x 460 px
        bw = min((pw - 40) // 2, 460)
        bx = cx - (2 * bw + 40) // 2
        cols = [(bx + k * (bw + 40), 44, bw) for k in range(2)]
    for k, (lb, key, col, kind) in enumerate([('HP', 'hp', GREEN, 'hp'), ('MP', 'mp', BLUE, 'mp')]):
        x0, r, w = cols[k]
        out.append(t(x0, r + 9, ('a', 24), text=lb, color=col, **nb))
        out.append(t(x0 + w, r + 11, ('b', 32), bind=f'dq3.bp{{i}}.{key}_t', align='right', **nb))
        out += gauge(x0 + 44, r - 5, w - 44 - 62, 10, kind, f'dq3.bp{{i}}.{key}', f'dq3.bp{{i}}.{key}_max', **nb)
    # moves under the cards on a dark window piece: command, down arrow, target (only while it is known)
    for my, gate in ((PARTY_BOTTOM + 2, NRM), (SWAP_PARTY_BOTTOM + 2, SWP)):
        mv = dict(need_bind='dq3.bp{i}.mv', **rep, **gate)
        out.append(img([X0, my, cw, MOVES_H - 2], f'module:dq3:win/{cw}x{MOVES_H - 2}', **mv))
        out.append(t(cx, my + 32, ('b', 32), bind='dq3.bp{i}.cmd', align='center', **mv))
        out.append(img([cx - 12, my + 38, 24, 24], 'module:dq3:down/24', **mv))
        out.append(t(cx, my + 80, ('s', 28), bind='dq3.bp{i}.tgt', color=GOLD, align='center', **mv))
    return out


def light_inner(box):
    """render.py enemy_detail(): (x + 26, y + 20, w - 52, h - 42)."""
    x, y, w, h = box
    return x + LPX, y + LPY - 4, w - 2 * LPX, h - 2 * LPY + 6


def enemy_detail(y, h, gate):
    """The enemy scrap (dq3.ed.*), normal (y 380, 418 tall) or the Follow enemy-turn copy (y 374, 368 tall).
    Affinities: a 2 x 3 grid of 172 x 116 cells (family icon + name, the companion word or the "?" slime) and
    Possible spells in two columns to its right; the sprite and the three stats on the left."""
    box = (X0, y, CW, h)
    g = dict(need_bind='dq3.b.m_enemy', **gate)
    out = [img(box, f'module:dq3:light/{CW}x{h}', **g)]
    x, iy, w, ih = light_inner(box)
    tx, right = x + 270, x + w
    st_top = iy + ih - 3 * 38
    out.append(img([x, iy, 240, st_top - iy - 4], src_bind='dq3.ed.sprite', **g))
    for k, (lb, key) in enumerate([('Attack', 'atk'), ('Defence', 'def'), ('Agility', 'agi')]):
        sb = st_top + 30 + k * 38
        out.append(t(x + 10, sb, ('s', 28), text=lb, color=INK2, outline=0, **g))
        out.append(t(x + 236, sb, ('b', 30), bind=f'dq3.ed.{key}', color=INK, outline=0, align='right', **g))
        # the game's buff / debuff status icons for this stat (module x offset left of the value)
        for j in range(2):
            out.append(img([x + 236, sb - 23, 28, 28], src_bind=f'dq3.ed.{key}.fx{j}.src', x_bind=f'dq3.ed.{key}.fx{j}.x',
                           need_bind=f'dq3.ed.{key}.fx{j}.on', **gate))
    out.append(t(tx, iy + 34, ('b', 40), bind='dq3.ed.name', color=INK, outline=0, **g))
    out.append(t(tx, iy + 34, ('s', 28), bind='dq3.ed.lv', color=INK2, outline=0, x_bind='dq3.ed.lvx', **g))
    # "N in group" / "Alone"; while the enemy acts (Follow) its action > target instead
    out.append(t(right, iy + 32, ('s', 28), bind='dq3.ed.group', color=INK2, outline=0, align='right',
                 need_bind='dq3.ed.group.on', **gate))
    a = dict(need_bind='dq3.ed.act.on', **gate)
    out.append(img([right, iy + 8, 26, 26], 'module:dq3:chev/26', x_bind='dq3.ed.act.ar0', **a))
    out.append(t(right, iy + 32, ('b', 30), bind='dq3.ed.act.cmd', color=INK, outline=0, x_bind='dq3.ed.act.cmdx', **a))
    tg = dict(need_bind='dq3.ed.act.tgt.on', **gate)
    out.append(img([right, iy + 8, 26, 26], 'module:dq3:chev/26', x_bind='dq3.ed.act.ar1', **tg))
    out.append(t(right, iy + 32, ('b', 30), bind='dq3.ed.act.tgt', color=TARGET_INK, outline=0, x_bind='dq3.ed.act.tgtx',
                 **tg))
    out.append(t(tx, iy + 76, ('a', 26), text='HP', color=HP_INK, outline=0, **g))
    out.append(t(right, iy + 77, ('b', 34), bind='dq3.ed.hp_t', color=INK, outline=0, align='right', **g))
    out += gauge(tx + 48, iy + 62, ENEMY_HP_W, 12, 'hp', 'dq3.ed.hp', 'dq3.ed.hp_max', **g)
    # affinities: 2 rows x 3 columns
    gy = iy + 92
    gw, gh = 3 * AFF_W, 2 * AFF_H
    out.append(img([tx, gy, gw + 1, gh + 1], f'module:dq3:affgrid/{gw}x{gh}/3/2', **g))
    for k in range(6):
        cx0, cy0 = tx + (k % 3) * AFF_W, gy + (k // 3) * AFF_H
        out.append(img([cx0 + AFF_IX, cy0 + AFF_IY, AFF_ICON, AFF_ICON], src_bind=f'dq3.ed.a{k}.icon', **g))
        out.append(t(cx0 + AFF_NX, cy0 + AFF_NY, ('s', 30), bind=f'dq3.ed.a{k}.name', color=INK, outline=0, **g))
        out.append(t(cx0 + AFF_W / 2, cy0 + AFF_H - 18, ('b', 32), bind=f'dq3.ed.a{k}.val', color=INK, outline=0,
                     align='center', **g))
        out.append(img([cx0 + AFF_W // 2 - 18, cy0 + AFF_H - 52, 36, 36], 'module:dq3:shadowq/36',
                       need_bind=f'dq3.ed.a{k}.q', **gate))
    # Possible spells: two columns right of the grid
    sx = tx + gw + 28
    col = (right - sx) / 2
    assert int(col) - 12 == ENEMY_SPELL_W, (col, ENEMY_SPELL_W)
    out.append(t(sx, gy + 32, ('b', 32), text='Possible spells', color=INK2, outline=0, **g))
    # names two per row, 40 px rows; a name too wide for a column takes the whole row (module x / y offsets)
    for k in range(8):
        out.append(t(sx, gy + 76, ('s', 28), bind=f'dq3.ed.sp{k}.name', color=INK, outline=0,
                     x_bind=f'dq3.ed.sp{k}.x', y_bind=f'dq3.ed.sp{k}.y', need_bind=f'dq3.ed.sp{k}.on', **gate))
    out.append(t(sx, gy + 76, ('s', 28), text='None', color=INK2, outline=0, need_bind='dq3.ed.sp.none', **gate))
    out.append(t(sx, gy + 76, ('s', 28), text='Unavailable', color=INK2, outline=0, need_bind='dq3.ed.sp.unavailable',
                 **gate))
    return out


ENEMY_HP_W = (X0 + LPX + CW - 2 * LPX) - (X0 + LPX + 270 + 48) - 170 - 20   # 608: room for "4500 / 4500"
PARTY_DETAIL_BOX = (X0, PARTY_DETAIL_Y, CW, SHORT_Y - G - PARTY_DETAIL_Y)
PD_VIS = 9


def party_detail():
    box = PARTY_DETAIL_BOX
    g = dict(need_bind='dq3.b.m_party')
    out = [img(box, f'module:dq3:light/{box[2]}x{box[3]}', **g)]
    x, iy, w, ih = box[0] + LPX, box[1] + LPY, box[2] - 2 * LPX, box[3] - 2 * LPY
    colw = (w - 40) // 2
    rx = x + colw + 40
    out.append(t(x, iy + 30, ('b', 36), bind='dq3.pd.name', color=INK, outline=0, **g))
    # name > move > target (module x offsets after the measured name)
    out.append(img([x, iy + 6, 26, 26], 'module:dq3:chev/26', x_bind='dq3.pd.ar0', need_bind='dq3.pd.mv'))
    out.append(t(x, iy + 30, ('b', 30), bind='dq3.pd.cmd', color=INK, outline=0, x_bind='dq3.pd.cmdx', need_bind='dq3.pd.mv'))
    out.append(img([x, iy + 6, 26, 26], 'module:dq3:chev/26', x_bind='dq3.pd.ar1', need_bind='dq3.pd.mvt'))
    out.append(t(x, iy + 30, ('b', 30), bind='dq3.pd.tgt', color=TARGET_INK, outline=0, x_bind='dq3.pd.tgtx',
                 need_bind='dq3.pd.mvt'))
    for k, (lb, key, col, kind) in enumerate([('HP', 'hp', HP_INK, 'hp'), ('MP', 'mp', MP_INK, 'mp')]):
        yy = iy + 80 + k * 64
        out.append(t(x, yy, ('a', 26), text=lb, color=col, outline=0, **g))
        out.append(t(x + colw, yy, ('b', 32), bind=f'dq3.pd.{key}_t', color=INK, outline=0, align='right', **g))
        out += gauge(x, yy + 12, colw, 12, kind, f'dq3.pd.{key}', f'dq3.pd.{key}_max', **g)
    sw = (colw - 24) // 2
    sy0 = iy + 196
    for k, lb in enumerate(['Attack', 'Defence', 'Agility', 'Wisdom', 'Strength', 'Luck']):
        sx = x + (k % 2) * (sw + 24)
        sb = sy0 + (k // 2) * 44 + 28
        out.append(t(sx, sb, ('s', 28), text=lb, color=INK2, outline=0, **g))
        out.append(t(sx + sw, sb, ('b', 30), bind=f'dq3.pd.s{k}', color=INK, outline=0, align='right', **g))
        if k < 3:  # Attack / Defence / Agility: the game's buff / debuff status icons left of the value
            for j in range(2):
                out.append(img([sx + sw, sb - 23, 28, 28], src_bind=f'dq3.pd.s{k}.fx{j}.src',
                               x_bind=f'dq3.pd.s{k}.fx{j}.x', need_bind=f'dq3.pd.s{k}.fx{j}.on'))
    out.append(rect([rx - 20, iy, 1, ih], '#FFBEA578', **g))
    out.append(t(rx, iy + 30, ('b', 30), bind='dq3.pd.job', color=INK, outline=0, **g))
    out.append(t(rx + colw, iy + 30, ('s', 28), bind='dq3.pd.lv', color=INK2, outline=0, align='right', **g))
    out.append(t(rx, iy + 72, ('b', 28), text='Spells', color=INK2, outline=0, **g))
    out.append(t(rx, iy + 110, ('s', 28), text='No spells learnt', color=INK2, outline=0, need_bind='dq3.pd.sp.none'))
    rows = dict(repeat=128, repeat_bind='dq3.pd.sp.count', repeat_dy=40, scroll='dq3.pd.spells')
    out.append(t(rx, iy + 110, ('s', 28), bind='dq3.pd.sp{i}.name', color=INK, outline=0, **rows))
    out.append(t(rx + colw - 30, iy + 110, ('b', 26), bind='dq3.pd.sp{i}.mp', color=MP_INK, outline=0, align='right',
                 **rows))
    return out


def battle_scrolls():
    box = PARTY_DETAIL_BOX
    x, iy, w = box[0] + LPX, box[1] + LPY, box[2] - 2 * LPX
    colw = (w - 40) // 2
    rx = x + colw + 40
    return [{'id': 'dq3.pd.spells', 'rect': [rx - 8, iy + 110 - 30, colw + 8, PD_VIS * 40], 'count_bind': 'dq3.pd.sp.count',
             'row_h': 40, 'bar': '#FF785A32', 'bar_track': '#FFC8B48C', 'bar_w': 6, 'show_bind': 'dq3.b.m_party',
             'reset_bind': 'dq3.pd.name'}]


def battle_actions():
    acts = {f'bsel_e{i}': {'kind': 'module', 'action': 'bsel', 'argument': i} for i in range(10)}
    acts.update({f'bsel_p{k}': {'kind': 'module', 'action': 'bsel', 'argument': 100 + k} for k in range(4)})
    acts.update({f'bfollow_{k}': {'kind': 'module', 'action': 'bfollow', 'argument': k} for k in range(2)})
    return acts


def manifest():
    return {
        'format': 1,
        'title_id': '01003E601E324000',
        'name': 'Dragon Quest III HD-2D Remake dual screen',
        'min_runtime': 18,
        'nav': False,
        'requires_module': True,
        'module': {'abi': 1, 'build_ids': [BUILD]},
        # the guest load plan (tools/dq3/gen_load_plan.py): Heal All / Handy Heal All on the game thread
        'load_plan': 'guest/<build>/load-plan.json',
        'canvas_w': W,
        'canvas_h': H,
        'background': '#FF000000',
        'font': 'file:dq3_font.txt',
        'font_atlas': 'module:dq3:font/1',
        '_about': 'Generated by tools/dq3/gen_manifest.py - edit the generator, not this file. Asset-free: '
                  'art = module:dq3:* keys decoded from the player\'s Nicola-Switch.pak; the font is the '
                  'game\'s Plantin MT Pro and Avenir Next rasterised by the module.',
        '_contract': 'Module outputs: dq3.tab, dq3.tab.party/journal, dq3.ftab*, dq3.m.*, dq3.mv* (field tabs and Map page, dq3_map.h); '
                     'dq3.ready/empty, dq3.p<i>.on/name/lv/hp_t/mp_t/hp/hp_max/mp/mp_max/fcard/nodrag, dq3.p<i>.st<k>.on/src/x; '
                     'native healing: dq3.heal.ready, dq3.heal.msg / dq3.heal.msg.on (the game routine through the load plan); '
                     'native button drive: dq3.party.reorder, '
                     'dq3.party.drag<i>, dq3.party.s<k>p<p>, dq3.input.pending (gate ints are published only while non-zero); '
                     'battle page: dq3.battle, dq3.b.*, dq3.e<i>.*, dq3.bp<k>.*, dq3.ed.*, dq3.pd.* (see '
                     'native/modules/01003E601E324000.cpp, dq3_battle_view.h). dq3.tab.bag; Party tab dq3.pp.* / pr.*, Bag tab dq3.bg.*, '
                     'the Party grid rows qg* and detail dq3.dt.*, the Bag list rows qb* and pane dq3.bd.*, Journal tab dq3.jn / jo / jt / jm / jr (dq3_info_view.h). Diagnostics, published only with EDEN_DSMOD_DQ3_ALL_OUTPUTS=1 '
                     '(no widget binds them): dq3.state, dq3.held, dq3.count, dq3.why, dq3.world, dq3.sample_us, dq3.b.view_us. '
                     'Taps: bsel, bfollow, ftab, floor, mapzoom, fullmap, partypick (top card), partytab, partyitem, partyslot, '
                     'bagpage, bagitem, journalmon, partydrop0..3 (top-card drag payload = source slot), heal (0 Heal All, 1 Handy).',
        'actions': {**battle_actions(), **field_actions(), 'wp_ok': {'kind': 'flag', 'flag': 'dq3_wp_ok'}},
        'flags': {'dq3_wp_ok': 0},
        'derived': system_derived(),
        'enforce_gate': {'point': 'dq3.input.pending', 'max': 1},
        'enforce': [{'action': f'party_press_{k}_{p}', 'every_ms': 50} for k in range(6) for p in range(2)],
        'page_binds': [{'point': 'dq3.battle', 'equals': 1, 'when_equal': {'page': 'battle', 'transition': 'fade',
                                                                         'duration_ms': 150},
                        'when_not_equal': {'page': 'field', 'transition': 'fade', 'duration_ms': 150}}],
        'pages': [{'id': 'field', 'title': 'Party',
                   'widgets': widgets() + field_tab_widgets() + map_widgets() + party_tab_widgets() +
                              bag_tab_widgets() + journal_tab_widgets() + system_widgets(),
                   'scrolls': party_scrolls() + bag_scrolls() + journal_scrolls()},
                  {'id': 'battle', 'title': 'Battle', 'widgets': battle_widgets(), 'scrolls': battle_scrolls()}],
    }


def check_native_layout(modules):
    """The C++ copies of the page geometry and font styles must equal the constants above."""
    def constants(name):
        with open(os.path.join(modules, name), encoding='utf-8') as f:
            text = f.read()
        found = {}
        for decl in re.findall(r'inline constexpr int ([^;]+);', text):
            for part in decl.split(','):
                m = re.fullmatch(r'\s*(\w+)\s*=\s*(-?\d+)\s*', part)
                if m:
                    found[m.group(1)] = int(m.group(2))
        return found
    mp, pl, bv, sy = (constants(n) for n in ('dq3_map.h', 'dq3_page_layout.h', 'dq3_battle_view.h', 'dq3_system.h'))
    want = {
        'dq3_map.h': (mp, dict(MapX0=X0, MapY0=Y0, MapX1=X1, MapPageY=FPAGE_Y, MapPageH=FPAGE_H, FullMapY=Y0,
                                FullMapH=Y1 - Y0, MapLeftW=LW, MapGap=G, MapFin=FIN, MapTitleRoom=TITLE_ROOM,
                                MapCardY=MCARD_Y, MapButton=MB, CardW=CW, CardH=FPAGE_H, CardObjY=CARD_OBJ_Y,
                                CardObjH=CARD_OBJ_H)),
        'dq3_page_layout.h': (pl, dict(FTabX0=X0, FTabW=CW, FTabH=TAB_H, FieldTabs=FTABS,
                                       PartyCellW=grid_geom(PRX, PRW, 4)[1], CellTextPad=CELL_TEXT_PAD,
                                       SlotTextW=SLOT_TW, JournalWrapW=JLIW, MaxRows=ROWS, BagNameW=B_NAME_W,
                                       BagPaneNameW=B_PANE_NAME_W, BagSubGap=30, SpellNameW=SPELL_NAME_W, FieldCardW=CARD_W, FieldCardH=CARD_H,
                                       FieldTextW=TW, StripGap=G)),
        'dq3_battle_view.h': (bv, dict(X0=X0, Y0=Y0, X1=X1, Y1=Y1, G=G, HeadH=HEAD_H, EnemyY=ENEMY_Y,
                                       BoxNames=BOX_H, BoxBare=BARE_H, CellNames=CELL_H, CellBare=BARE_CELL_H,
                                       Gi=GI, Gap=GAP, Mv=MOVES_H, PartyBottom=PARTY_BOTTOM, ShortH=SHORT_H,
                                       FullH=FULL_H, TabsW=TABS_W, EnemySpellW=ENEMY_SPELL_W,
                                       SwapPartyBottom=SWAP_PARTY_BOTTOM, SwapDetailY=SWAP_DETAIL_Y,
                                       SwapEnemyY=SWAP_ENEMY_Y, NormalDetailH=NORMAL_DETAIL_H,
                                       SwapDetailH=SWAP_DETAIL_H, EnemyCellW=ENEMY_CW,
                                       EnemySpellCol=ENEMY_SPELL_W + 12, EnemySpellRowH=40, AffIconPx=AFF_ICON)),
        'dq3_system.h': (sy, dict(zip(('SysOkX', 'SysOkY', 'SysOkW', 'SysOkH'), SYS_OK))),
    }
    for header, (table, values) in want.items():
        for name, value in values.items():
            assert table.get(name) == value, f'{header} {name} = {table.get(name)}, generator {value}'
    # FontStyle enumerators: Value (Semibold 26, plain), then Bold<px> / Semi<px> / Avenir<px>
    with open(os.path.join(modules, 'dq3_font.h'), encoding='utf-8') as f:
        body = re.search(r'enum class FontStyle : u32 \{(.*?)\};', f.read(), re.S).group(1)
    face = {'Bold': 'b', 'Semi': 's', 'Avenir': 'a'}
    enum = {}
    for name, value in re.findall(r'(\w+) = (\d+)', body):
        if name == 'Count':
            continue
        m = re.fullmatch(r'(Bold|Semi|Avenir)(\d+)', name)
        key = ('s', 26) if name == 'Value' else (face[m.group(1)], int(m.group(2)))
        assert key not in enum, f'FontStyle {name}: duplicate {key}'
        enum[key] = int(value)
    assert enum == STYLE, f'FontStyle {sorted(enum.items())} != STYLE {sorted(STYLE.items())}'
    # Pillow metrics the battle view uses for its centred enemy names
    with open(os.path.join(modules, 'dq3_battle_view.cpp'), encoding='utf-8') as f:
        text = f.read()
    for name, a, d in re.findall(r'\{FontStyle::(\w+), (\d+), (\d+)\}', text):
        m = re.fullmatch(r'(Bold|Semi|Avenir)(\d+)', name)
        key = ('s', 26) if name == 'Value' else (face[m.group(1)], int(m.group(2)))
        assert METRIC[key] == (int(a), int(d)), f'battle view metric {name} ({a}, {d}) != METRIC[{key}] {METRIC[key]}'


# Gate ints the module publishes only while non-zero (a missing value reads as 0 / closed).
ZERO_OMITTED = re.compile(r'dq3\.(heal\.msg\.on|party\.s\d+p\d|party\.drag(\d|\{i\})|'
                          r'p(\d|\{i\})\.nodrag|input\.pending)$')


def check_zero_gates(m):
    """No bind may test one of ZERO_OMITTED for 0 (equals 0 / hide_eq 0 / an enforce range from 0)."""
    def walk(o):
        if isinstance(o, dict):
            names = [v for v in o.values() if isinstance(v, str) and ZERO_OMITTED.match(v)]
            if names:
                for k, v in o.items():
                    assert not (('eq' in k or 'equal' in k or k == 'min') and v == 0), f'{names[0]}: {k} = 0'
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(m)


def check_gauges(m):
    """Every gauge bar (module:dq3:gauge/<kind>/<w>x<h>, fill laid out on the filled part) is keyed at its
    own rect size and sits exactly in the well of a track image drawn 2 px larger on each side with the
    matching track/<w+4>x<h+4> key (dq3_art.h: the game's 2 px rim at 1:1), with the same repeat / visibility."""
    seen = 0
    def walk(o):
        nonlocal seen
        if isinstance(o, dict):
            for key in ('widgets', 'items', 'children'):
                ws = o.get(key)
                if isinstance(ws, list):
                    for i, w in enumerate(ws):
                        if not (isinstance(w, dict) and w.get('type') == 'bar'
                                and str(w.get('image', '')).startswith('module:dq3:gauge/')):
                            continue
                        x, y, bw, bh = w['rect']
                        assert w['image'].endswith(f'/{bw}x{bh}'), (w['image'], w['rect'])
                        assert w.get('fill') == 'slice' and w.get('slice') == [0, 0, 0, 0] and w.get('frame') == 0, w
                        t = ws[i - 1] if i else {}
                        assert t.get('src') == f'module:dq3:track/{bw + 4}x{bh + 4}', (t.get('src'), w['image'])
                        assert t.get('rect') == [x - 2, y - 2, bw + 4, bh + 4], (t.get('rect'), w['rect'])
                        for k in ('repeat', 'repeat_dx', 'repeat_dy', 'hide_bind', 'hide_eq', 'keep_min', 'keep_max', 'need_bind', 'show_bind', 'y_bind'):
                            assert t.get(k) == w.get(k), (k, t.get(k), w.get(k))
                        seen += 1
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(m)
    assert seen >= 8, seen



# widest family name at Semibold 30 (Pillow getlength, PlantinMTPro-Semibold: Woosh 92.06, Crack 84.38) and the
# Bold 32 cap height above a baseline
AFF_NAME_W, AFF_CAP = 93, 24


def check_affinity(m):
    """0.10.1 element icons: in both enemy-detail copies every one of the six cells has its own icon image
    (src_bind dq3.ed.a<k>.icon, keyed elem/<k>/AFF_ICON by the module) of AFF_ICON x AFF_ICON inside the cell
    with a 4 px margin, the family name right of the icon and inside the cell, the state word (and the "?"
    slime) below the icon, all text >= 24 px."""
    battle = m['pages'][1]['widgets']
    grids = [w for w in battle if str(w.get('src', '')).startswith('module:dq3:affgrid/')]
    assert len(grids) == 2, len(grids)
    for grid in grids:
        gx, gy = grid['rect'][0], grid['rect'][1]
        gate = {k: grid.get(k) for k in ('hide_bind', 'hide_eq', 'show_bind', 'keep_min', 'keep_max')}
        same = [w for w in battle if {k: w.get(k) for k in gate} == gate]
        for k in range(6):
            cx0, cy0 = gx + (k % 3) * AFF_W, gy + (k // 3) * AFF_H
            icons = [w for w in same if w.get('src_bind') == f'dq3.ed.a{k}.icon']
            names = [w for w in same if w.get('bind_text') == f'dq3.ed.a{k}.name']
            vals = [w for w in same if w.get('bind_text') == f'dq3.ed.a{k}.val']
            qs = [w for w in same if w.get('need_bind') == f'dq3.ed.a{k}.q']
            assert len(icons) == len(names) == len(vals) == len(qs) == 1, (k, len(icons), len(names), len(vals), len(qs))
            x, y, w, h = icons[0]['rect']
            assert w == h == AFF_ICON, icons[0]
            assert cx0 + 4 <= x and x + w <= cx0 + AFF_W - 4 and cy0 + 4 <= y and y + h <= cy0 + AFF_H - 4, (k, icons[0])
            nx, nb = names[0]['rect'][0], names[0]['rect'][1] + BASE
            assert nx >= x + w + 6 and nx + AFF_NAME_W <= cx0 + AFF_W - 4 and nb - AFF_CAP >= cy0, (k, names[0])
            vb = vals[0]['rect'][1] + BASE
            assert vb - AFF_CAP > y + h and vb <= cy0 + AFF_H - 4, (k, vals[0])
            assert qs[0]['rect'][1] >= y + h, (k, qs[0])


def check_party_strip(m):
    """Owner 2026-10-10 dynamic strip: for every N = 1..4 the field cards (gate dq3.strip) and the battle party
    row (gates dq3.b.pn / dq3.b.pl) repeat exactly N times at strip_w(N) + G and span the content box with no
    blank space; every top-card / battle-card tap and drag target exists once per N at the card rect; no
    per-member card widget is left outside a strip gate."""
    field, battle = m['pages'][0]['widgets'], m['pages'][1]['widgets']
    for n in range(1, 5):
        cw = strip_w(n)
        assert X0 + (n - 1) * (cw + G) + cw == X1, n
        fw = [w for w in field if w.get('hide_bind') == 'dq3.strip' and w.get('keep_min') == n]
        assert fw and all(w.get('keep_max') == n and w.get('repeat') == n and w.get('repeat_dx') == cw + G
                          for w in fw), n
        picks = [w for w in fw if w.get('on_tap') == 'party_pick']
        assert len(picks) == 2 and all(w['rect'] == [X0, Y0, cw, CARD_H] for w in picks), (n, picks)
        assert sum(1 for w in picks if w.get('draggable') and w.get('drop_action') == 'party_drop_{i}') == 1, n
        for w in fw:   # every card piece stays inside its own card
            x, y, ww, hh = w['rect']
            if 'x_bind' not in w:
                assert X0 <= x and x + ww <= X0 + cw and Y0 <= y and y + hh <= Y0 + CARD_H, (n, w)
        bw = [w for w in battle if w.get('hide_bind') in ('dq3.b.pn', 'dq3.b.pl') and w.get('keep_min') in (n, n + 4)]
        assert bw and all(w.get('repeat') == n and w.get('repeat_dx') == cw + G and
                          w.get('keep_max') == w.get('keep_min') for w in bw), n
        taps = [w for w in bw if w.get('on_tap') == 'bsel_p{i}']
        assert len(taps) == 3 and all(w['rect'][0] == X0 and w['rect'][2] == cw for w in taps), (n, taps)
        for w in bw:
            if w['type'] == 'image' and str(w.get('src', '')).startswith('module:dq3:win/'):
                assert w['rect'][0] == X0 and w['rect'][2] == cw and w['src'].startswith(f'module:dq3:win/{cw}x'), w
    for page, pre in ((field, 'dq3.p{i}.'), (battle, 'dq3.bp{i}.')):
        for w in page:
            if any(isinstance(v, str) and v.startswith(pre) for v in w.values()):
                assert w.get('hide_bind') in ('dq3.strip', 'dq3.b.pn', 'dq3.b.pl'), w


def check_layout(m):
    """Revision 2 geometry: every static rect inside the canvas, every tap of a main control >= 88 px, the
    Bag list's visible rows, and no text below the owner's 24 px floor (FontStyle sizes)."""
    for page in m['pages']:
        for w in page['widgets']:
            x, y, ww, hh = w['rect']
            templated = any(k in w for k in ('x_bind', 'y_bind'))
            if not templated:
                assert x >= 0 and y >= 0 and x + ww <= W and y + hh <= H, (page['id'], w)
            if w.get('on_tap', '').startswith(('ftab_', 'heal_', 'map_', 'floor_', 'fullmap', 'bfollow_')):
                assert min(ww, hh) >= 88, ('main control', w)
    assert B_ROWS == 12, B_ROWS
    small = [k for k in STYLE if k[1] < 24 and k not in (('s', 22), ('s', 20), ('b', 22))]
    assert not small, small


def check_spell_list(m):
    """Owner 2026-10-10 (0.10.2): the Party Spells sub-tab is a list in the grid's place (top) with the detail kept
    below. Every row widget repeats on dq3.ps.n at the Bag pitch inside the dq3.ps.list viewport; the viewport stays
    in the member's dark window above the detail and ends at the grid's scroll bar; one tap per row (party_item, the
    row's payload, the Bag row height); name / MP at the Bag sizes (>= 24 px); the name room equals the module's;
    no spell field is left on the grid rows (qg*s / qg*z / qg*m)."""
    page = m['pages'][0]
    field = page['widgets']
    rows = [w for w in field if w.get('repeat_bind') == 'dq3.ps.n']
    assert rows and all(w.get('repeat') == ROWS and w.get('repeat_dy') == S_RH == B_RH and w.get('scroll') == 'dq3.ps.list'
                        and 'repeat_cols' not in w for w in rows), rows
    sc = [r for r in page['scrolls'] if r['id'] == 'dq3.ps.list']
    assert len(sc) == 1, sc
    vx, vy, vw, vh = sc[0]['rect']
    assert sc[0]['count_bind'] == 'dq3.ps.n' and sc[0]['row_h'] == S_RH and sc[0]['show_bind'] == 'dq3.ps.on', sc
    assert vx >= PRX + 6 and vx + vw <= grid_geom(PRX, PRW, 4)[2] + DARK_SCROLL['bar_w'], sc
    assert vy >= P_SUB_Y + 44 and vy + vh <= P_DET_Y and vh >= 6 * S_RH, (sc, P_DET_Y)
    for w in rows:   # the first row's pieces sit in the viewport
        x, y, ww, hh = w['rect']
        assert vx <= x and x + ww <= vx + vw and vy <= y and y + hh <= vy + S_RH + 12, w
    taps = [w for w in rows if w.get('on_tap')]
    assert len(taps) == 1 and taps[0]['on_tap'] == 'party_item' and taps[0].get('payload') == '{i}', taps
    assert taps[0]['rect'][3] == S_RH - 4 == B_RH - 4, taps
    names = [w for w in rows if w.get('bind_text') == 'qsn{i}']
    mps = [w for w in rows if w.get('bind_text') == 'qsm{i}']
    assert len(names) == len(mps) == 1, (names, mps)
    nb = [w for w in field if w.get('bind_text') == 'qbn{i}'][0]
    assert names[0]['rect'][1] - S_TOP == nb['rect'][1] - B_TOP, (names, nb)
    assert names[0]['rect'][0] + SPELL_NAME_W + S_MP_W - 36 == mps[0]['rect'][0], (names, mps)   # name room, MP column
    for k in ('qss{i}', 'qsz{i}'):
        ic = [w for w in rows if w.get('bind') == k]
        assert len(ic) == 1 and ic[0]['rect'][2] >= 32 and ic[0]['rect'][0] + ic[0]['rect'][2] < names[0]['rect'][0], k
    assert any(w.get('need_bind') == 'qsx{i}' for w in rows) and any(w.get('need_bind') == 'qsl{i}' for w in rows)
    grid = [w for w in field if w.get('repeat_bind') == 'dq3.pr.n']
    assert not any(w.get('bind') in ('qgs{i}', 'qgz{i}') or w.get('bind_text') == 'qgm{i}' for w in grid), \
        'spell fields left on the Carried grid'
    det = [w for w in field if w.get('need_bind') == 'dq3.dt.on']
    assert det and min(w['rect'][1] for w in det) >= vy + vh, 'the detail must stay below the list'


def check_map_prefetch(m):
    """0.10.2 map flash fix (dq3_map_swap.h): per map box one hidden prefetch image (dq3.m.img.next, 1 x 1, alpha 0,
    inside the frame's fin, same gate as the map) drawn before the shown map image (dq3.m.img) and its frame."""
    field = m['pages'][0]['widgets']
    for mode, (bx, by) in (('n', (MX, FPAGE_Y)), ('f', (X0, Y0))):
        gate = f'dq3.mv.{mode}'
        nxt = [i for i, w in enumerate(field) if w.get('src_bind') == 'dq3.m.img.next' and w.get('need_bind') == gate]
        shown = [i for i, w in enumerate(field) if w.get('src_bind') == 'dq3.m.img' and w.get('need_bind') == gate]
        frame = [i for i, w in enumerate(field) if str(w.get('src', '')).startswith('module:dq3:mframe/')
                 and w.get('need_bind') == gate]
        assert len(nxt) == len(shown) == len(frame) == 1, (mode, nxt, shown, frame)
        w = field[nxt[0]]
        x, y, ww, hh = w['rect']
        assert (ww, hh) == (1, 1) and w['color'] == '#00FFFFFF' and not w.get('pan_zoom'), w
        assert bx <= x < bx + FIN and by <= y < by + FIN, w
        assert nxt[0] < shown[0] < frame[0], (mode, nxt, shown, frame)
    assert sum(1 for w in field if w.get('src_bind') == 'dq3.m.img.next') == 2


def check_text(m):
    """Every static label is encoded in one of the FontStyle ranges (no codepoint outside the atlas)."""
    for page in m['pages']:
        for w in page['widgets']:
            s = w.get('text')
            if w.get('type') != 'label' or s is None or 'text_scale' not in w or w.get('text_scale') != 3:
                continue
            for c in s:
                cp = ord(c)
                assert 0x20 <= cp <= 0xFF or 0xE000 <= cp < 0xE000 + 0x100 * (max(STYLE.values())), (w, hex(cp))

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    check_native_layout(os.path.join(here, '..', '..', 'native', 'modules'))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', '..', 'packages', 'DragonQuest3HD2D', 'dualscreen')
    os.makedirs(out, exist_ok=True)
    m = manifest()
    check_zero_gates(m)
    check_gauges(m)
    check_layout(m)
    check_party_strip(m)
    check_affinity(m)
    check_spell_list(m)
    check_map_prefetch(m)
    check_text(m)
    with open(os.path.join(out, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(m, f, indent=1, ensure_ascii=False)
        f.write('\n')


if __name__ == '__main__':
    main()
