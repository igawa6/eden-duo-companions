#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate packages/MonsterSanctuary/dualscreen/manifest.json.

Layout follows the approved D2 "Banners+" r3 mockups: the
design is drawn on a 310 x 270 virtual canvas at 4x = 1240 x 1080, so every coordinate below is
given in mockup units and multiplied by S here. This first slice draws the windows, rows and
text with manifest widgets and the built-in font; the game's own sprites, glyphs and font come
from the player's romfs in a later pass (module pictures).

Glyph rule: no button glyph unless the physical button does that action. The
companion's commits are touch taps through the guest helper, so it draws no glyphs at all.

    python3 tools/monster-sanctuary/gen_manifest.py
"""
from __future__ import annotations

import json
import re
from pathlib import Path

S = 4
VW, VH = 310, 270
OUT = Path(__file__).resolve().parents[2] / 'packages/MonsterSanctuary/dualscreen/manifest.json'

# palette sampled from the game UI
P = dict(win_fill='#354455', win_edge='#192631', win_line='#a9e2de', hdr='#222426',
         strip='#0b0c0e', lvl='#ffa400', skp='#68ffee',
         val='#ffff00', hp='#5bee00', mp='#7b4dff', sec='#e8ebee', band='#161b20', white='#ffffff',
         dim='#5f6b77', bg='#0c0e0f', weak='#ff7a5c', res='#9ad0ff',
         # mana as text: the mana colour lightened 55 % toward white (LEGIBILITY: >= 4.5:1, 5.2:1 on
         # win_fill, 10:1 on the strips; the game's own mana shades, ~#8f48ff, reach only 2.2:1 on
         # the window); the mana bars keep mp
         mp_txt='#c4afff')


# The targeted monster's colour pulse (SelectionView.StartTween: GlowingColor -> Color.gray over
# 0.5 s, PulseEndless): GLOW_STEPS tints the module picks with e<i>.glow. GLOW_FROM is the
# TargetSelector SelectionView.GlowingColor read live (glow= in the reader log).
GLOW_FROM = (191, 191, 191)  # read live 2026-11-09: glow=12566463 (0xBFBFBF)
GLOW_STEPS = 6


def glow_ramp():
    out = []
    for k in range(GLOW_STEPS):
        f = k / (GLOW_STEPS - 1)
        r, g, b = (round(a + (128 - a) * f) for a in GLOW_FROM)
        out.append(f'#FF{r:02X}{g:02X}{b:02X}')
    return out


GLOW = glow_ramp()


def c(hex_rgb: str, a: int = 255) -> str:
    return f'#{a:02X}{hex_rgb.lstrip("#").upper()}'


def r(x, y, w, h):
    return [round(x * S), y * S, w * S, h * S]


def rect(x, y, w, h, col, a=255, **kw):
    return dict(type='rect', rect=r(x, y, w, h), color='#00000000', bg=c(col, a), frame=0, **kw)


def frame(x, y, w, h, col, px=1, **kw):
    return dict(type='rect', rect=r(x, y, w, h), color=c(col), frame=px * S // 2, **kw)


# English native LocaDataEntry.String values, indexed as positions in pictures.idx.
NATIVE = {
    'HP': 'health', 'MP': 'mana', 'Mana:': 'mana', 'Combo:': 'combo',
    'Damage:': 'damage', 'Total Completion -': 'completion', 'Items': 'items',
    'Resist': 'resist', 'Switch': 'switch', 'Weak': 'weak',
    'Enemy turn': 'enemyturn', 'Options': 'options', 'On': 'on', 'Off': 'off', 'Exploration': 'exploration',
    'Combat': 'combat', 'Map': 'map', 'Team': 'team', 'Equipment': 'equipment',
    'Weapon': 'weapon', 'Accessories': 'accessories', 'Skillpoints': 'skillpoints',
    'Explore Ability': 'ability', 'Back': 'back', 'Close': 'close', 'Loading...': 'loading',
    'Lvl': 'lvl', 'Lv': 'lvl',
}

LINE = {'bold': 14, 'regular': 12, 'small': 12}
TITLE_FONTS = {4: 'bold', 3: 'regular', 2: 'small'}


def txt_key(font, w, align, text):
    return f"module:ms:txt:{font}:{S}:{w}:{align[0]}:{text}"


def label(x, y, w, text=None, bind=None, col='#ffffff', scale=3, align='left', font=None, **kw):
    """Text in the game's own font (mockup sizes: every font at 4x), as a module picture exactly
    w mockup px wide; the module applies the mockups' fit rule (font -> regular -> small -> ...).
    A bound text uses src_bind "T:<font>:<w>:<align>:<value name>": the module publishes that
    name as the picture key of the value's current text ("T:<font>/<k>:..." renders it k times
    instead of 4)."""
    if text in NATIVE:
        bind, text = 'l.' + NATIVE[text], None
    font = font or TITLE_FONTS.get(scale, 'regular')
    kw.pop('max_lines', None)
    d = dict(type='image', rect=r(x, y - 1, w, LINE[font]), tint=c(col))
    if text is not None:
        d['src'] = txt_key(font, w, align, text)
    if bind is not None:
        d['src_bind'] = f"T:{font}:{w}:{align[0]}:{bind}"
    d.update(kw)
    return d


def wrapped(x, y, w, lines, bind, col='#ffffff', font='regular', center=False, **kw):
    """Bound text word-wrapped by the module into a fixed box of `lines` lines (src_bind
    "W:<font>:<w>:<lines>[c]:<value>"): one native point per line ("|" in the value), no
    ellipsis unless even the small font needs more lines than the box has."""
    d = dict(type='image', rect=r(x, y, w, lines * LINE[font]), tint=c(col),
             src_bind=f"W:{font}:{w}:{lines}{'c' if center else ''}:{bind}")
    d.update(kw)
    return d


def value(x, y, w, bind, col='#ffffff', scale=3, align='left', **kw):
    for k in ('max_bind', 'max_sep', 'div', 'suffix'):
        kw.pop(k, None)
    return label(x, y, w, bind=bind, col=col, scale=scale, align=align, **kw)


def spr(name, k=4):
    """A sprite from the player's romfs, scaled k times (module:ms:spr<k>:...)."""
    return f'module:ms:spr{k}:{name}'


def img(x, y, w, h, src=None, src_bind=None, **kw):
    d = dict(type='image', rect=r(x, y, w, h))
    if src:
        d['src'] = src
    if src_bind:
        d['src_bind'] = src_bind
    d.update(kw)
    return d


def sliced(x, y, w, h, name, inset, **kw):
    """9-slice of a native sprite at 4x (ui/layout 8 px corners, ui/menuItem 2 px)."""
    return img(x, y, w, h, spr(name), fill='slice', slice=[inset * S] * 4, **kw)


def selector(x, y, w, h, blink=False, **kw):
    """ui/Selector: the white corner bracket on the four corners of the selected row. With
    blink, the brackets sway like the game's own SelectionView arrows (horizontal, 1 px, 0.5 s):
    the module publishes sel.d (canvas px outward, 0 while still / Reduce motion)."""
    corners = [(x - 2, y - 2, {}, -1, -1), (x + w - 5, y - 2, dict(flip_x=True), 1, -1),
               (x - 2, y + h - 5, dict(flip_y=True), -1, 1),
               (x + w - 5, y + h - 5, dict(flip_x=True, flip_y=True), 1, 1)]
    out = []
    for cx, cy, flips, sx, sy in corners:
        d = img(cx, cy, 7, 7, spr('ui/Selector'), **flips, **kw)
        if blink:
            d.update(x_bind='sel.d', x_scale=sx)
        out.append(d)
    return out


def window(x, y, w, h, title=None, title_bind=None, **kw):
    """The game's teal window (ui/layout, 9-slice); dark header band (ui/menuHeader)."""
    out = [sliced(x, y, w, h, 'ui/layout', 8, **kw)]
    if title or title_bind:
        out.append(sliced(x + 3, y + 3, w - 6, 15, 'ui/menuHeader', 2, **kw))
        out.append(label(x + 3, y + 4, w - 6, text=title, bind=title_bind, align='center', font='bold', **kw))
    return out


def row(x, y, w, h, on_bind=None, blink=False, **kw):
    """ui/menuItem row; ui/menuItemOpaque + Selector brackets when selected (blink: the
    brackets and the row's glow follow the game's own selection animation)."""
    out = [sliced(x, y, w, h, 'ui/menuItem', 2, **kw)]
    if on_bind:
        g = {**kw, 'need_bind': on_bind}
        out.append(sliced(x, y, w, h, 'ui/menuItemOpaque', 2, **g))
        out += selector(x, y, w, h, blink=blink, **g)
    return out


def strip(x, y, w, h, a=235, **kw):
    return rect(x, y, w, h, P['strip'], a, **kw)


def bar(x, y, w, h, bind, max_bind, col, **kw):
    return dict(type='bar', rect=r(x, y, w, h), bind=bind, max_bind=max_bind, color=c(col),
                bg=c(P['strip']), frame=0, **kw)


# ---------------------------------------------------------------- tab bar
TAB_W = [94, 72, 72, 72]


def tabs(active: str):
    """[Exploration|Combat] Map Team (gear). The first tab follows the game state."""
    out = [rect(0, 240, VW, 30, P['band']), rect(0, 240, VW, 1, P['win_line'])]
    x = 0
    for i, (pid, w) in enumerate(zip(['home', 'map', 'team', 'opts'], TAB_W)):
        on = pid == active
        if on:
            out.append(sliced(x + 2, 243, w - 4, 25, 'ui/menuItemOpaque', 2))
            out += selector(x + 2, 243, w - 4, 25)
        col = P['white'] if on else P['sec']
        if pid == 'home':
            out.append(label(x + 2, 249, w - 4, text='Exploration', col=col, align='center', font='bold',
                             need_bind='!ms.combat'))
            out.append(label(x + 2, 249, w - 4, text='Combat', col=col, align='center', font='bold',
                             need_bind='ms.combat'))
        elif pid == 'opts':
            # companion-drawn gear (assets/ui-companion/gear.png in the module-picture pass); a
            # text cog meanwhile, no button glyph
            out.append(img(x + w / 2 - 6, 249, 12, 12, 'file:gear.png', tint=c(P['white'] if on else P['sec'])))
        else:
            out.append(label(x + 2, 249, w - 4, text=pid.capitalize(), col=col, align='center', font='bold'))
        if pid == 'home':
            out.append(dict(type='rect', rect=r(x, 240, w, 30), color='#00000000', bg='#00000000', frame=0,
                            on_tap='tab_home', need_bind='!ms.combat'))
            out.append(dict(type='rect', rect=r(x, 240, w, 30), color='#00000000', bg='#00000000', frame=0,
                            on_tap='tab_home_battle', need_bind='ms.combat'))
        else:
            out.append(dict(type='rect', rect=r(x, 240, w, 30), color='#00000000', bg='#00000000', frame=0,
                            on_tap=f'tab_{pid}'))
        x += w
    return out


def dialogue_dim():
    """NPC dialogue (r3 states): the page dimmed and not touchable, the tab bar stays live."""
    return [rect(0, 0, VW, 240, '#000000', 150, input_block=True, need_bind='ms.dim')]


def wrong_build():
    """Wrong game build (r3 states): the module is refused, so only host-drawn text can show;
    module_error / module_error_message come from the host (OctopathTraveler0 / BOTW pattern)."""
    g = dict(need_bind='module_error')
    def hlabel(x, y, text=None, bind_text=None, col=P['white'], scale=3, w=0):
        d = dict(type='label', rect=r(x, y, w, 0), text=text or '', text_scale=scale, color=c(col),
                 align='center', **g)
        if bind_text:
            d['bind_text'] = bind_text
        if w:
            d.update(wrap_width=w * S - 32, max_lines=3 if bind_text else 2, text_center_h=0)
        return d
    return [rect(0, 0, VW, VH, '#1d2a33', input_block=True, **g),
            rect(0, 200, VW, 70, '#16222a', **g),
            rect(60, 70, 190, 130, P['win_edge'], **g),
            frame(60, 70, 190, 130, P['win_line'], 1, **g),
            rect(63, 73, 184, 15, P['hdr'], **g),
            hlabel(155, 76, 'Monster Sanctuary', scale=4),
            rect(66, 92, 178, 28, P['win_fill'], **g),
            hlabel(155, 96, 'ver 2.1.0.32', scale=4),
            hlabel(155, 108, 'Companion', col=P['sec'], scale=4),
            rect(66, 124, 178, 46, P['win_fill'], **g),
            # what the player needs to recover, essential text. A wrong build is refused before
            # the module opens, so module:ms:font never loads and the host draws its built-in
            # font: 5 rows x text_scale = 25 px caps at scale 5, 20 px advance. The 672 px wrap
            # holds 33 characters a line; every host message fits 3 lines at the 40 px pitch
            # (105 px of the 184 px box); "Update the game ..." is 620 px, one line
            hlabel(155, 128, bind_text='module_error_message', col=P['lvl'], scale=5, w=176),
            hlabel(155, 174, 'Update the game to ver 2.1.0.32', col=P['sec'], scale=5, w=176)]


def tap(x, y, w, h, action, cue=None, **kw):
    """An invisible touch target over the widgets drawn at (x, y, w, h). A tap rect larger than
    its row (LEGIBILITY touch minimums) passes the drawn row as cue=(x, y, w, h): the result
    feedback outline stays on the row (fb_cues)."""
    d = dict(type='rect', rect=r(x, y, w, h), color='#00000000', bg='#00000000', frame=0,
             on_tap=action, **kw)
    if cue is not None:
        d['_cue'] = r(*cue)
    return d


# ---------------------------------------------------------------- result feedback
# Owner decision 2026-11-12 (FEEDBACK-PROPOSAL.md Option 1): no toast. A success is the game's
# own state change; while the game has not answered, the tapped row pulses amber; a refusal (or no
# answer) pulses it red and the reason takes the page's own single-line text slot. The module names
# the row (0100A0F00DA68000.cpp FeedbackKey, fb.<key>.b / fb.<key>.n); every tap widget gets its
# cue here (fb_cues), gated like the tap itself. Reduce motion: the same outline, still.
# rect.pulse is the runtime's breathing outline (a boolean on a rect: alpha 20..100 %, ~0.7 s; the
# object form is for map markers only)
FB_FRAME = 4                      # canvas px outline (the Dave the Diver / VA-11 alert precedent)
FB_COLORS = (('b', P['lvl']), ('n', P['weak']))   # busy amber, refused red
FB_DERIVED: dict[str, str] = {}
MOTION_FLAG = '@flag:ms.reduce_motion'


def fb_key(action):
    """The row key the module publishes for a tap action (FeedbackKey), None for none."""
    import re
    for pat, fmt in ((r'follow_(\d+)', 'fp{}'), (r'team_focus_(\d+)', 'tm{}'), (r'eq_pick_(\d+)', 'q{}'),
                     (r'tp_(\d+)', 'tq{}'), (r'b_cmd_(\d+)', 's{}'), (r'b_rail_(\d+)', 'r{}'),
                     (r'b_itemx_(\d+)', 'ix{}'), (r'b_switch_(\d+)', 'w{}'), (r'b_tgt_(\d+)', 'e{}'),
                     (r'b_ally_(\d+)', 'a{}'), (r'b_skst_(\d+)', 'ss{}'), (r'b_lvl_(\d+)', 'lv{}')):
        m = re.fullmatch(pat, action)
        if m:
            return fmt.format(m.group(1))
    m = re.fullmatch(r'b_items(\d)_(\d+)', action)
    if m:
        return f'sk{m.group(1)}_{m.group(2)}'
    m = re.fullmatch(r'b_info_(\d+)', action)
    if m:
        i = int(m.group(1))
        return f'a{i}' if i < 8 else f'ic{i - 8}'
    if action in ('b_back', 'b_subback'):
        return 'bk'
    if action == 'b_level':
        return 'lvy'
    return None


def fb_cues(pages):
    """After each tap widget, its row's busy / refused outline: same rect, x/y binds, scroll and
    layout gate as the tap; shown only for the row the module names, while the tap's own gate
    holds; pulsing, or still under Reduce motion."""
    for page in pages:
        out = []
        for wd in page['widgets']:
            out.append(wd)
            cue_rect = wd.pop('_cue', None) or wd.get('rect')
            key = fb_key(wd.get('on_tap', '')) if 'on_tap' in wd else None
            if key is None:
                continue
            gate = wd.get('need_bind')
            for kind, col in FB_COLORS:
                for still in (False, True):
                    name = f"fx.{key}.{kind}{'s' if still else ''}" + (f'.{gate}' if gate else '')
                    expr = f"fb.{key}.{kind}" + (f' && {gate}' if gate else '') + \
                        f" && {'' if still else '!'}{MOTION_FLAG}"
                    assert FB_DERIVED.setdefault(name, expr) == expr, name
                    cue = dict(type='rect', rect=list(cue_rect), color=c(col), bg='#00000000', frame=FB_FRAME,
                               need_bind=name)
                    if not still:
                        cue['pulse'] = True
                    for k in ('x_bind', 'y_bind', 'scroll', 'repeat', 'hide_bind', 'keep_min', 'keep_max'):
                        if k in wd:
                            cue[k] = wd[k]
                    out.append(cue)
        page['widgets'] = out
    return pages


def button(x, y, w, h, text, action, on_bind=None, tap_rect=None, **kw):
    """A small native-looking button (ui/menuItem row + label) with a tap target; tap_rect
    (x, y, w, h) enlarges the target around the drawn button (LEGIBILITY: primary controls
    >= 112 x 112 canvas px = 28 x 28 mockup px)."""
    out = row(x, y, w, h, on_bind=on_bind, **kw)
    out.append(label(x + 2, y + (h - 10) // 2, w - 4, text=text, align='center', **kw))
    if tap_rect is None:
        out.append(tap(x, y, w, h, action, **kw))
    else:
        out.append(tap(*tap_rect, action, cue=(x, y, w, h), **kw))
    return out


def scene_bg(dim=165, **kw):
    out = [img(0, 0, 480, 270, spr('bg/DailySky'), **kw), img(-40, 40, 480, 154, spr('bg/mountains1'), **kw)]
    for x in range(-20, VW, 125):
        out.append(img(x, 120, 125, 100, spr('bg/FirForest'), **kw))
    out.append(rect(0, 215, VW, 55, '#203018', **kw))
    if dim:
        out.append(rect(0, 0, VW, VH, '#000000', dim, **kw))
    return out


# The four Spectral Familiars of the game's own starter pick, in its order (Wolf, Eagle, Toad,
# Lion), standing in a row on the title / loading scene and playing their native "idle" clips
# (sc.m<i>: the module publishes the clip frame; Reduce motion keeps frame 0).
FAMILIARS = ('SpectralWolf', 'SpectralEagle', 'SpectralToad', 'SpectralLion')


def familiars(y, need_bind):
    """64 px native boxes at 6x (96 mockup px), bottom-centred, 77 px apart."""
    return [img(-9 + i * 77, y, 96, 96, src_bind=f'sc.m{i}', need_bind=need_bind) for i in range(4)]


def title_scene():
    """Title / start (and the save-slot menu, same game state): the game's own backdrop, logo
    (ui/Title at 3x) and the four familiars idling; "Press [A] to start" composed from the native
    strings like the title's own prompt, only while the MainMenu waits for that press (A starts
    the game, so its glyph is allowed); the version text the title shows. No stale game data."""
    g = dict(need_bind='ms.nogame')
    w = [rect(0, 0, VW, 240, P['bg'], input_block=True, **g)]
    w += scene_bg(0, **g)
    w.append(img(11, 10, 288, 42, spr('ui/Title', 3), **g))
    w += familiars(108, 'ms.nogame')
    # blinks like the native one (its FlashRenderer: 32 frames on, 32 off; module ti.off)
    w.append(strip(0, 204, VW, 18, 150, need_bind='ti.show'))
    w.append(label(0, 207, VW, text='{s:press} {i:ui/ButtonASwitch} {s:tostart}', align='center', font='bold',
                   need_bind='ti.show'))
    w.append(label(196, 226, 110, bind='ti.ver', col=P['sec'], align='right', font='small', need_bind='ti.ver.on'))
    return w


def loading():
    """Loading: the same native backdrop at dusk with the familiars idling and the game's own
    "Loading..." string; nothing from the previous scene."""
    g = dict(need_bind='ms.loading')
    w = [rect(0, 0, VW, 240, P['bg'], input_block=True, **g)]
    w += scene_bg(110, **g)
    w += familiars(108, 'ms.loading')
    w.append(strip(0, 204, VW, 18, 150, **g))
    w.append(label(0, 207, VW, text='Loading...', align='center', font='bold', **g))
    return w


def no_game():
    return title_scene()


def turn1_banner():
    """Complete native rule, read from the bundle and split into two fitted lines at runtime, in
    its own band under the combo strip ("Keeper Battle  ? ? ?"), 50..75. The middle band uses the
    T1 geometry meanwhile, so the rule covers no list row."""
    g = dict(need_bind='b.t1')
    return [strip(0, 50, VW, 25, **g),
            label(8, 53, VW - 16, bind='l.turn1a', font='small', **g),
            label(8, 63, VW - 16, bind='l.turn1b', font='small', **g)]


# ---------------------------------------------------------------- pages
# Teleporter list (mockup units): 88 / 120 px rows, 12 px gaps; one row per native destination in
# a scroll region (drag / fling, the host's scroll list) filling the panel under its header
TP_ROW, TP_ROOM_ROW, TP_GAP = 22, 30, 3
TP_MINI = (44, 11)  # a repeated area's crystal map (176 x 44 canvas px, the module's TeleportMini)
TP_ROWS = 24        # ms_publish.h TeleportRows
TP_LIST = (204, 36, 100, 198)  # the scroll region (x, y, w, h)
FIELD_DESC_LINES = 5  # the follower description box (88..148), then the minimap (152..215)


def page_field():
    w = scene_bg(165)
    w += window(2, 2, 150, 236)
    for i in range(6):
        y = 6 + i * 38
        g = dict(need_bind=f'p{i}.v')
        # the current follower is the selected row; a tap makes that monster the follower (only
        # while exploring, the helper's FIELD_GATE: elsewhere the rows show the native disabled dim)
        w += row(6, y, 142, 36, on_bind=f'p{i}.fol', **g)
        w.append(img(8, y + 2, 32, 32, src_bind=f'p{i}.spr', need_bind=f'p{i}.spr.on'))
        w.append(label(42, y + 4, 104, bind=f'p{i}.ab', **g))
        w.append(label(42, y + 20, 104, bind=f'p{i}.name', col=P['sec'], **g))
        w.append(rect(6, y, 142, 36, P['strip'], 110, need_bind=f'fp{i}.lock'))
        w.append(tap(6, y, 142, 36, f'follow_{i}', need_bind=f'fp{i}.ok'))
    w += window(154, 2, 154, 236, 'Explore Ability')
    g = dict(need_bind='f.v')
    # follower card: the sprite (native size, its own 64 px column) with name and ability to its
    # right, so neither text overlaps it; the description below, one native point per line
    w.append(img(158, 21, 64, 64, src_bind='f.spr', need_bind='f.spr.on'))
    w.append(label(224, 34, 80, bind='f.name', col=P['sec'], **g))
    w.append(label(224, 50, 80, bind='f.ab', col=P['skp'], font='bold', **g))
    w.append(wrapped(158, 88, 146, FIELD_DESC_LINES, 'f.abdw', **g))
    w.append(rect(160, 150, 142, 1, P['win_line']))
    w.append(dict(type='image', rect=r(158, 152, 146, 63), src_bind='m.mini', need_bind='m.v'))
    w.append(img(158, 152, 8, 8, src_bind='m.mdot.pic', need_bind='m.mdot.on', x_bind='m.mdot.x', y_bind='m.mdot.y'))
    w.append(strip(158, 216, 146, 18))
    w.append(label(158, 219, 146, bind='m.area', align='center', need_bind='m.v'))
    # a refusal's reason takes the area-name strip while it lasts
    w.append(strip(158, 216, 146, 18, 255, need_bind='h.no'))
    w.append(label(158, 219, 146, bind='h.msg', col=P['weak'], align='center', need_bind='h.no'))
    return dict(id='field', widgets=w + dialogue_dim() + loading() + no_game() + tabs('home') + wrong_build())


TP_FLAG = '@flag:ms.tpc'   # session-only: the Teleporter panel collapsed (not in persist_flags)
MAP_Y, MAP_H = 18, 176
MAP_VIEWS = (('map-n', 200, 'm.pic', 'm.dot', '!' + TP_FLAG), ('map-w', VW, 'm.picw', 'm.dotw', TP_FLAG))
TP_SPARKLES = 24  # module SparkleSlots


def page_map():
    w = [rect(0, 0, VW, 240, P['bg'])]
    # the map: one picture of the whole explored map at the fitted view, panned and zoomed by the
    # host (one-finger drag, two-finger pinch, smooth, clamped to the picture); the narrow view
    # beside the Teleporter panel, the wide one while that panel is collapsed. The native player
    # dot ping follows the view (the module places it from @view_zoom/@view_pan_x/y:<id>).
    for vid, width, pic, dot, gate in MAP_VIEWS:
        w.append(dict(type='image', id=vid, rect=r(0, MAP_Y, width, MAP_H), src_bind=pic,
                      need_bind=f'm.v.{vid}', pan_zoom=True, min_zoom=1, max_zoom=8))
        # the native teleporter sparkle (Rect2x2 particles), animated over the picture (module:
        # m.tp<l|w><i>, placed like the dot; a 24 x 36 box, the tile's centre at (12, 26))
        tp = 'm.tpl' if vid == 'map-n' else 'm.tpw'
        for i in range(TP_SPARKLES):
            w.append(img(0, MAP_Y, 24, 36, src_bind=f'{tp}{i}.pic', need_bind=f'{tp}{i}.on.{vid}', x_bind=f'{tp}{i}.x',
                         y_bind=f'{tp}{i}.y'))
        w.append(img(0, MAP_Y, 12, 12, src_bind=f'{dot}.pic', need_bind=f'{dot}.on.{vid}', x_bind=f'{dot}.x',
                     y_bind=f'{dot}.y'))
        # back to the whole map: shown only while the view is zoomed or panned; native row art and
        # the game's own "Map" word (no controller glyph, no repurposed icon)
        w += button(width - 44, MAP_Y + MAP_H - 20, 40, 18, 'Map', f'map_reset_{vid}', need_bind=f'm.reset.{vid}',
                    tap_rect=(width - 44, MAP_Y + MAP_H - 28, 40, 28))
    w.append(strip(0, 0, VW, 18))
    # the game's own figure only (MapMenu.areaInfos); before the native map was opened once,
    # the strip shows the area name instead of an invented percentage
    w.append(label(0, 3, 200, text='Total Completion -', align='right', need_bind='m.pct10'))
    w.append(label(204, 3, 100, bind='m.pct', need_bind='m.pct10'))
    w.append(label(0, 3, VW, bind='m.area', align='center', need_bind='m.v', hide_bind='m.pct10', keep_max=0))
    w += row(2, 198, 196, 26, need_bind='m.hint')
    w.append(img(6, 204, 16, 13.5, src_bind='m.hint.sprite', need_bind='m.hint'))
    w.append(label(26, 205, 168, bind='m.hint.ability', col=P['skp'], need_bind='m.hint'))
    w.append(label(2, 228, 196, bind='m.help', align='center', font='small', need_bind='m.v', hide_bind='h.no',
                   keep_max=0))
    # a refusal's reason takes the gesture-hint line while it lasts
    w.append(label(2, 228, 196, bind='h.msg', col=P['weak'], align='center', need_bind='h.no'))
    # the Teleporter panel: its header collapses / expands it (session flag ms.tpc); the native
    # ArrowDown points down while expanded and up while collapsed
    body = dict(need_bind='!' + TP_FLAG)
    w += [sliced(200, 18, 108, 222, 'ui/layout', 8, **body)]
    w.append(sliced(203, 21, 102, 15, 'ui/menuHeader', 2))
    w.append(label(203, 22, 90, text='Teleporter', align='center', font='bold'))
    w.append(img(292, 25, 7, 7, spr('ui/ArrowDown'), **body))
    w.append(img(292, 25, 7, 7, spr('ui/ArrowDown'), flip_y=True, need_bind=TP_FLAG))
    # the header band is 15 px; the tap reaches up over the completion strip (no tap there) to
    # the primary-control minimum without covering the first row
    w.append(tap(200, 8, 108, 28, 'tp_toggle'))
    first = len(w)
    for i in range(TP_ROWS):
        for height, gate in ((TP_ROW, f'tq{i}.single'), (TP_ROOM_ROW, f'tq{i}.room.on')):
            w += row(204, 36, 100, height, on_bind=f'tp.tq{i}.single.here' if height == TP_ROW else f'tp.tq{i}.room.here',
                     need_bind=f'tp.{gate}', y_bind=f'tq{i}.dy')
        w.append(label(207, 36 + (TP_ROW - LINE['small']) // 2, 94, bind=f'tq{i}.name', font='small',
                       need_bind=f'tp.tq{i}.single', y_bind=f'tq{i}.dy'))
        w.append(label(207, 39, 94, bind=f'tq{i}.name', font='small', need_bind=f'tp.tq{i}.room.on', y_bind=f'tq{i}.dy'))
        # a repeated area (two crystals): where this one sits in the area (module picture, the
        # native map colours and teleporter mark) and the native map's name of a place in its
        # room when there is one -- no internal scene suffix, no invented name
        w.append(img(207, 51, TP_MINI[0], TP_MINI[1], src_bind=f'tq{i}.mini', need_bind=f'tp.tq{i}.room.on',
                     y_bind=f'tq{i}.dy'))
        # the place name: the small font at 3x (18 px caps, the secondary minimum), rendered at
        # that scale by the module into its 195 x 36 rect (no host resampling), fit rule small ->
        # "..." in the 65 native px beside the crystal map ("Monster Farm" fits)
        place = label(255, 52, 49, bind=f'tq{i}.label', font='small', col=P['sec'],
                      need_bind=f'tp.tq{i}.label.on', y_bind=f'tq{i}.dy')
        place['src_bind'] = f'T:small/3:65:l:tq{i}.label'
        place['rect'][2:] = [65 * 3, LINE['small'] * 3]
        w.append(place)
        for height, gate in ((TP_ROW, f'tq{i}.single.tap'), (TP_ROOM_ROW, f'tq{i}.room.tap')):
            w.append(tap(204, 36, 100, height, f'tp_{i}', need_bind=f'tp.{gate}', y_bind=f'tq{i}.dy'))
    # every row widget scrolls in the list region (a region places only repeat widgets)
    for wd in w[first:]:
        wd.update(scroll='tp.list', repeat=1)
    w.append(rect(*TP_LIST, P['strip'], 120, need_bind='tp.m.untp'))
    scrolls = [{'id': 'tp.list', 'rect': r(*TP_LIST), 'content_h_bind': 'm.tp.h', 'show_bind': 'tp.open',
                'bar': c(P['win_line'], 200), 'bar_track': c(P['strip'], 120), 'bar_w': 6}]
    return dict(id='map', scrolls=scrolls,
                widgets=w + dialogue_dim() + loading() + no_game() + tabs('map') + wrong_build())


def map_derived():
    """Gates of the map page: each map view with its panel state, the reset buttons while the
    host view is away from home, and the Teleporter list only while the panel is expanded."""
    out = []
    out.append({'name': 'ti.show', 'expr': 'ti.press && !ti.off'})
    for vid, _w, _pic, dot, gate in MAP_VIEWS:
        out.append({'name': f'm.v.{vid}', 'expr': f'm.v && {gate}'})
        out.append({'name': f'{dot}.on.{vid}', 'expr': f'{dot}.on && {gate}'})
        tp = 'm.tpl' if vid == 'map-n' else 'm.tpw'
        for i in range(TP_SPARKLES):
            out.append({'name': f'{tp}{i}.on.{vid}', 'expr': f'{tp}{i}.on && {gate}'})
        out.append({'name': f'm.reset.{vid}', 'expr': f"m.v && {gate} && 'view_custom:{vid}'"})
    open_ = '!' + TP_FLAG
    for i in range(TP_ROWS):
        for g in ('single', 'room.on', 'label.on', 'single.tap', 'room.tap', 'single.here', 'room.here'):
            out.append({'name': f'tp.tq{i}.{g}', 'expr': f'tq{i}.{g} && {open_}'})
    out.append({'name': 'tp.m.untp', 'expr': f'm.v && !m.tpok && {open_}'})
    out.append({'name': 'tp.open', 'expr': f'tq0.v && {open_}'})
    return out


TEAM_ROWS = 7              # Team page rows: the active 3 and the next 4 of the team
EQUIP_SLOTS = (0, 2, 3, 4)  # Weapon, Accessory 1-3 (Weapon 2 / Accessory 4 are rare-only)


def page_team():
    w = scene_bg(165)
    w += window(2, 2, 150, 236, 'Team')
    for i in range(TEAM_ROWS):
        # the active 3, then the rest of the team
        y = 20 + i * 30 + (3 if i >= 3 else 0)
        g = dict(need_bind=f'p{i}.v')
        w += row(6, y, 142, 29, on_bind=f'tsel{i}', **g)
        w.append(img(7, y + 1, 32, 27, src_bind=f'p{i}.rowspr', need_bind=f'p{i}.spr.on'))
        w.append(label(40, y + 2, 104, bind=f'p{i}.name', **g))
        w.append(label(40, y + 16, 20, text='Lvl', col=P['lvl'], **g))
        w.append(value(58, y + 16, 24, f'p{i}.lv', col=P['lvl'], **g))
        # shift icons: the native 16 px sprite at 3x = 48 px (LEGIBILITY: information icons >= 40 px)
        w.append(img(84, y + 15, 12, 12, spr('icons/icon_light_passive', 3), need_bind=f'p{i}.light'))
        w.append(img(84, y + 15, 12, 12, spr('icons/icon_dark_passive', 3), need_bind=f'p{i}.dark'))
        w.append(dict(type='rect', rect=r(6, y, 142, 29), color='#00000000', bg='#00000000', frame=0,
                      on_tap=f'team_focus_{i}', need_bind=f'p{i}.v'))
    w.append(rect(8, 112, 138, 1, P['win_line']))
    w += window(154, 2, 154, 236, 'Equipment')
    g = dict(need_bind='t.v')
    w.append(label(158, 20, 146, bind='t.name', align='center', **g))
    w.append(label(160, 34, 30, text='HP', col=P['hp'], **g))
    w.append(value(190, 34, 110, 't.hptxt', **g))
    w.append(label(160, 46, 30, text='MP', col=P['mp_txt'], **g))
    w.append(value(190, 46, 110, 't.mptxt', **g))
    w.append(label(160, 58, 80, text='Skillpoints', col=P['skp'], need_bind='t.sp'))
    w.append(value(250, 58, 50, 't.sp', col=P['skp'], need_bind='t.sp'))
    slots = ['Weapon', None, 'Accessories', None, None, None]
    y = 86
    for k in EQUIP_SLOTS:
        if slots[k]:
            w.append(rect(158, y - 11, 146, 10, P['hdr']))
            w.append(label(162, y - 11, 140, text=slots[k], col=P['sec'], scale=2))
        w += row(158, y, 146, 23, on_bind=f'qsel{k}')
        w.append(label(162, y + 5, 138, bind=f't.g{k}', need_bind=f't.g{k}.on'))
        w.append(label(162, y + 5, 138, text='-', col=P['dim'], need_bind=f'!t.g{k}.on'))
        # a tap opens the picker of what the inventory holds for this slot; outside exploring
        # (battle, pause menu, cinematic) the game refuses equipping, so the slot shows dimmed
        # like a native disabled row, not tappable
        w.append(rect(158, y, 146, 23, P['strip'], 110, need_bind='t.lock'))
        w.append(tap(158, y, 146, 23, f'eq_slot_{k}', need_bind='t.edit'))
        y += 24 + (10 if k == 0 else 0)
    # swap: the next tap on a team row trades places with the focused monster (dimmed and not
    # tappable outside exploring, where the game refuses a team swap). The tap reaches 4 px above
    # and below the drawn button (584 x 112 canvas px, the primary-control minimum): slot 4 ends
    # at 191, the tab bar starts at 240; off while the picker's Close button sits over it
    w += row(158, 198, 146, 20, on_bind='t.swap', need_bind='t.v')
    w.append(label(160, 198 + 5, 142, text='Swap', align='center', need_bind='t.v'))
    w.append(rect(158, 198, 146, 20, P['strip'], 110, need_bind='t.lock'))
    w.append(tap(158, 194, 146, 28, 'swap_mode', cue=(158, 198, 146, 20), need_bind='t.swaptap'))
    w.append(label(6, 222, 142, text='Tap a monster to swap', col=P['val'], align='center', need_bind='t.swap',
                   hide_bind='h.no', keep_max=0))
    # a refusal's reason takes that hint line while it lasts
    w.append(rect(6, 220, 142, 15, P['win_fill'], need_bind='h.no'))
    w.append(label(6, 222, 142, bind='h.msg', col=P['weak'], align='center', need_bind='h.no'))
    # the equipment picker (over the team list)
    q = dict(need_bind='q.open')
    w.append(rect(2, 2, 150, 236, P['bg'], **q))  # opaque: the team list must not show through
    w += window(2, 2, 150, 236, 'Equip', **q)
    for i in range(8):
        y = 20 + i * 27
        g = dict(need_bind=f'q{i}.v')
        w += row(6, y, 142, 24, **g)
        w.append(label(10, y + 6, 104, bind=f'q{i}.name', **g, hide_bind=f'q{i}.eq.on', keep_max=0))
        w.append(label(10, y + 1, 104, bind=f'q{i}.name', need_bind=f'q{i}.eq.on'))
        w.append(value(116, y + 6, 28, f'q{i}.n', col=P['sec'], align='right', **g, hide_bind=f'q{i}.eq.on', keep_max=0))
        w.append(value(116, y + 1, 28, f'q{i}.n', col=P['sec'], align='right', need_bind=f'q{i}.eq.on'))
        # W1: another owned monster (team, bench or farm) already has this item equipped
        w.append(img(10, y + 11, 16, 13.5, src_bind=f'q{i}.eqspr', need_bind=f'q{i}.eqspr.on'))
        w.append(label(28, y + 12, 116, bind=f'q{i}.eqtxt', font='small', col=P['skp'], need_bind=f'q{i}.eq.on'))
        w.append(tap(6, y, 142, 24, f'eq_pick_{i}', **g))
    # Reuse the right panel's action area while picking; reserve the whole left list for items.
    w.append(rect(158, 198, 146, 28, P['win_fill'], **q))
    w += button(158, 198, 146, 28, 'Close', 'eq_close', **q)
    return dict(id='team', widgets=w + dialogue_dim() + loading() + no_game() + tabs('team') + wrong_build())


# ---------------------------------------------------------------- battle
# Owner round 2 (2026-11-12): the page mirrors the native top screen. The player's three active
# monsters sit in a party bar along the top (PARTY_H), the combo strip under it; the middle band
# is the native command rail on the left (only the commands the game offers this turn, the game's
# own icons and row art, the active command highlighted like native) and, on the right, what the
# selected command shows natively: the skill list (Attack), the item window (Items), the switch
# list (Switch Monster) or the info card (Monster Info); the target list while targeting.
PARTY_H = 34
COMBO_Y = PARTY_H
RAIL_TABS = 5      # the rail holds at most Attack, Items, Monster Info, Switch Monster, Retreat
RAIL_X, RAIL_W = 2, 100
PANE_X, PANE_W = 104, 204   # the right pane's window
ROW_X, ROW_W = PANE_X + 4, PANE_W - 8
# The middle band (rail | pane) between the combo strip and the detail strip comes in two
# geometries. NORMAL is the round-2 layout. T1 is the Keeper duel's first turn: the native turn-1
# rule gets its own band under the combo strip (T1_BAND) and the lists shrink to fit what is
# left, so the rule never covers a row.
MID_BOTTOM = 186
T1_BAND = 25       # the rule's band, 50..75: two small-font lines, joined to the combo strip
MID_NORMAL = dict(t1=False, top=COMBO_Y + 17, rp=25, rh=24,
                  s_dy=23, s_h=22, s_ico=3, s_txt=4, s_mpi=7,
                  e_dy=39, e_h=38, e_spr=3, e_sz=32, e_name=2, e_hp=16, e_wr=23,
                  i_dy=18, i_h=17, i_ico=0.5, i_isz=16, i_txt=2, i_font='bold',
                  w_dy=37, w_h=36, w_spr=2, w_sz=32, w_name=5, w_info=20,
                  c_sz=64, c_y=20, c_wr=90, c_chip=104, c_chs=28,
                  t_h=94, t_spr=2, t_sz=32, t_name=36, t_lv=49, t_hp=62, t_wk=68, t_rs=80)
MID_T1 = dict(t1=True, top=COMBO_Y + 16 + T1_BAND + 1, rp=20, rh=19,
              s_dy=18, s_h=17, s_ico=0.5, s_txt=2, s_mpi=5,
              e_dy=30, e_h=29, e_spr=0.5, e_sz=28, e_name=0, e_hp=13, e_wr=17,
              i_dy=14, i_h=13, i_ico=0.5, i_isz=12, i_txt=0.5, i_font='regular',
              w_dy=29, w_h=28, w_spr=0, w_sz=28, w_name=2, w_info=15,
              c_sz=48, c_y=18, c_wr=70, c_chip=82, c_chs=26,
              t_h=85, t_spr=1, t_sz=28, t_name=30, t_lv=42, t_hp=54, t_wk=59, t_rs=71)
# Owner round 3: the target cards stand side by side like the monsters on the battlefield (the
# module publishes e<i>.x / a<i>.x: each card's slot, left to right by Monster.CombatPos.x)
CARD_W, CARD_PITCH = 64, 66        # three cards in the pane's row width (196)
# the skill window's other column as an icon strip: a 22 px cell (88 canvas px, the dense touch
# minimum; the module's SkillStripPx), drawn as a 20 px icon cell like the native one with a 2 px
# gap on the rows' side (steward re-check R1: the 20 px cell was an 80 px tap)
SKILL_STRIP = 22
SKILL_STRIP_ART = SKILL_STRIP - 2
Y_LEVEL = '{i:ui/ButtonYSwitch} {s:selectlevel}'
LAYOUT_DERIVED: dict[str, str] = {}


def layout_gate(widgets, t1):
    """NORMAL widgets hide while b.t1 (the module publishes it 0/1 in every state); T1 widgets
    show only while it is 1. A widget that already has a hide gate (keep_max 0) gets a derived
    one: hidden while that gate is set or the layout is the other one."""
    for wd in widgets:
        hb = wd.get('hide_bind')
        if hb is None:
            wd['hide_bind'] = 'b.t1'
            wd['keep_min' if t1 else 'keep_max'] = 1 if t1 else 0
            continue
        assert wd.get('keep_max') == 0 and 'keep_min' not in wd and not hb.startswith('!'), wd
        name = f'L{int(t1)}.{hb}'
        LAYOUT_DERIVED[name] = f"{'!' if t1 else ''}b.t1 || {hb}"
        wd['hide_bind'] = name
    return widgets


def party_bar():
    """The player's monsters on the field, like the native command window's portrait strip:
    sprite, name, HP and mana bars, mana. The acting monster's card is lit; while targeting, the
    native target's card carries the selector and a tap on an ally card confirms it natively."""
    w = [strip(0, 0, VW, PARTY_H)]
    for i in range(3):
        x = 2 + i * 102
        g = dict(need_bind=f'a{i}.v')
        w.append(sliced(x, 1, 100, 32, 'ui/menuItem', 2, **g))
        w.append(sliced(x, 1, 100, 32, 'ui/menuItemOpaque', 2, need_bind=f'a{i}.cur'))
        w += selector(x, 1, 100, 32, blink=True, need_bind=f'a{i}.tsel')
        w.append(img(x + 1, 1, 32, 32, src_bind=f'a{i}.spr', need_bind=f'a{i}.spr.on'))
        w.append(label(x + 34, 3, 64, bind=f'a{i}.name', **g))
        w.append(bar(x + 34, 16, 62, 2, f'a{i}.hp', f'a{i}.hpmax', P['hp'], **g))
        w.append(bar(x + 34, 19, 62, 2, f'a{i}.mp', f'a{i}.mpmax', P['mp'], **g))
        w.append(value(x + 34, 22, 50, f'a{i}.mp', col=P['white'], scale=2, font='small', **g))
        # information icons >= 40 px (LEGIBILITY): the native 8 px sprite at 5x
        w.append(img(x + 87, 22, 10, 10, spr('ui/icon_mana', 5), **g))
        w.append(tap(x, 1, 100, 32, f'b_ally_{i}', need_bind=f'tgt.a{i}'))
        # Monster Info: the card shows this ally (the native cursor moves there)
        w.append(tap(x, 1, 100, 32, f'b_info_{i}', need_bind=f'inf.a{i}'))
    return w


def battle_mid(L):
    """The middle band in geometry L: the command rail (left) and the pane (right)."""
    top = L['top']
    wh = MID_BOTTOM - top
    w = []
    # -- the native command rail: the game's own rows; the active command lit (menuItemOpaque +
    # the Selector, swaying like the native SelectionView); a tap selects that command natively
    w += window(RAIL_X, top, RAIL_W, wh)
    for i in range(RAIL_TABS):
        y = top + 4 + i * L['rp']
        g = dict(need_bind=f'r{i}.v')
        w += row(RAIL_X + 4, y, RAIL_W - 8, L['rh'], on_bind=f'r{i}.sel', blink=True, **g)
        w.append(img(RAIL_X + 6, y + (L['rh'] - 16) / 2, 16, 16, src_bind=f'r{i}.ico', need_bind=f'r{i}.ico.on'))
        # the command name like the native rail: one line right after the icon, the text centred
        # on the icon's centre (measured live: the small font's ink centre then sits on the
        # 16 px icon's centre, small gaps 2026-11-12)
        w.append(label(RAIL_X + 24, y + (L['rh'] - 12) / 2, RAIL_W - 30, bind=f'r{i}.name', font='small', **g))
        # the tap fills the row pitch (the 1 px gap below the row too)
        w.append(tap(RAIL_X + 4, y, RAIL_W - 8, L['rp'], f'b_rail_{i}', need_bind=f'r{i}.tap',
                     cue=(RAIL_X + 4, y, RAIL_W - 8, L['rh'])))
    # -- Attack: the acting monster's skills (player's turn, not targeting, no submenu open). The
    # native window has three columns (commands, skills, the skills' overflow): the column with
    # the native cursor is drawn as rows, the other skill column as an icon strip on its native
    # side (as the item window; module: sk.w1 / sk.w2 widths, s.ex the rows' x shift, ss.x the
    # strip's x)
    cmd = dict(need_bind='b.cmd')
    w += window(PANE_X, top, PANE_W, wh, None, **cmd)
    w.append(sliced(PANE_X + 3, top + 3, PANE_W - 6, 15, 'ui/menuHeader', 2, **cmd))
    # the acting monster's name; centred left of the "[Y] Select Level" control while it shows
    w.append(label(PANE_X + 3, top + 4, PANE_W - 6, bind='b.cname', align='center', font='bold', need_bind='b.cmdt'))
    w.append(label(PANE_X + 3, top + 4, PANE_W - 84, bind='b.cname', align='center', font='bold', need_bind='b.lvly'))
    rows_y = top + 18
    sdy, sh = L['s_dy'], L['s_h']
    for ncols in (1, 2):
        rw = ROW_W - (ncols - 1) * SKILL_STRIP
        for i in range(5):
            y = rows_y + i * sdy
            g = dict(need_bind=f's{i}.v.{ncols}', x_bind='s.ex')
            w += row(ROW_X, y, rw, sh, on_bind=f's{i}.sel.{ncols}', blink=True, **g)
            w.append(img(ROW_X + 4, y + L['s_ico'], 16, 16, src_bind=f's{i}.ico', **g))
            # the level number on the icon (MenuListItem.iconInfo), bottom right like native
            w.append(label(ROW_X + 13, y + L['s_ico'] + 8, 10, bind=f's{i}.badge', font='small', align='right',
                           need_bind=f's{i}.badge.{ncols}', x_bind='s.ex'))
            # the name with the native type / element icons drawn right after the rendered text
            w.append(label(ROW_X + 24, y + L['s_txt'], rw - 70, bind=f's{i}.label', font='bold', **g))
            # not enough mana: dimmed like the native disabled row
            w.append(rect(ROW_X, y, rw, sh, P['strip'], 110, need_bind=f's{i}.dim.{ncols}', x_bind='s.ex'))
            w.append(value(ROW_X + rw - 44, y + L['s_txt'], 28, f's{i}.mp', col=P['white'], align='right', **g))
            w.append(img(ROW_X + rw - 15, y + L['s_mpi'] - 1, 10, 10, spr('ui/icon_mana', 5), **g))
            # touch commits the skill through the native menu (the top cursor moves there first)
            w.append(tap(ROW_X, y, rw, sdy, f'b_cmd_{i}', cue=(ROW_X, y, rw, sh), **g))
    for i in range(5):
        y = rows_y + i * sdy
        g = dict(need_bind=f'ss{i}.v', x_bind='ss.x')
        aw = SKILL_STRIP_ART
        ix = (aw - 16) / 2
        w.append(sliced(ROW_X, y, aw, sh, 'ui/menuItem', 2, **g))
        w.append(img(ROW_X + ix, y + L['s_ico'], 16, 16, src_bind=f'ss{i}.ico', **g))
        w.append(label(ROW_X + ix + 6, y + L['s_ico'] + 8, 10, bind=f'ss{i}.badge', font='small', align='right',
                       need_bind=f'ss{i}.badge.on', x_bind='ss.x'))
        w.append(rect(ROW_X, y, aw, sh, P['strip'], 110, need_bind=f'ss{i}.dim', x_bind='ss.x'))
        # the tap is the whole cell, the gap to the rows included (the rows' own taps start after it)
        w.append(tap(ROW_X, y, SKILL_STRIP, sdy, f'b_skst_{i}', cue=(ROW_X, y, aw, sh), **g))
    # the focused skill has levels: "[Y] Select Level" at the window's top right, where the native
    # hint sits beside the window (Y does exactly this natively; GLYPH-AUDIT.md). The tap reaches
    # up into the combo strip (nothing to tap there) and keeps the 28 px minimum.
    w += button(PANE_X + PANE_W - 76, top + 3, 72, 14, Y_LEVEL, 'b_level', need_bind='b.lvly',
                tap_rect=(PANE_X + PANE_W - 78, top - 9, 76, 28))
    # -- targeting: the target cards side by side, left to right as the monsters stand on the
    # battlefield (the native target cursor's order); enemies, or the own side for a skill that
    # targets it (b.tgta)
    tg = dict(need_bind='b.tgt')
    w += window(PANE_X, top, PANE_W, wh, 'Choose target', **tg)
    w += target_cards(L, rows_y, 'e')
    w += target_cards(L, rows_y, 'a')
    for wd in w:
        nb = wd.get('need_bind', '')
        if (nb.startswith('s') and (nb[1:2].isdigit() or nb[1:2] == 's')) or nb in ('b.cmd', 'b.cmdt', 'b.lvly'):
            wd['hide_bind'] = 'b.tgt'
            wd['keep_max'] = 0
    return w


def target_cards(L, rows_y, side):
    """Three cards in the target window: sprite, name, level, HP, Weak / Resist (native sign,
    word and element icons), the W2 hit highlight; each at its battlefield slot (x_bind
    <side><i>.x). Enemies (e) show while targeting them and on the enemy turn (e<i>.show); allies
    (a) only while a skill targets the own side (ta<i> = b.tgta && a<i>.v). A tap confirms that
    target natively; the native cursor's card carries the selector and pulses."""
    w = []
    h = L['t_h']
    for i in range(3):
        k = f'{side}{i}'
        gate = f'{k}.show' if side == 'e' else f't{k}'
        g = dict(need_bind=gate, x_bind=f'{k}.x')
        sel = f'{k}.sel' if side == 'e' else f'{k}.tsel'
        # the card, lit with the Selector brackets for the native target; the brackets sway
        # around the card's slot (the module publishes <side><i>.xl / .xr = slot -/+ sel.d)
        w.append(sliced(ROW_X, rows_y, CARD_W, h, 'ui/menuItem', 2, **g))
        w.append(sliced(ROW_X, rows_y, CARD_W, h, 'ui/menuItemOpaque', 2, need_bind=f'{gate}.sel', x_bind=f'{k}.x'))
        for cd in selector(ROW_X, rows_y, CARD_W, h, need_bind=f'{gate}.sel'):
            cd['x_bind'] = f'{k}.xr' if cd.get('flip_x') else f'{k}.xl'
            w.append(cd)
        if side == 'e':
            # W2: the targeted skill hits this enemy's weakness (frame) / resistance (muted)
            w.append(frame(ROW_X, rows_y, CARD_W, h, P['hp'], 1, need_bind=f'{gate}.hitw', x_bind=f'{k}.x'))
            w.append(rect(ROW_X, rows_y, CARD_W, h, P['strip'], 120, need_bind=f'{gate}.hitr', x_bind=f'{k}.x'))
        sz = L['t_sz']
        # the targeted monster pulses GlowingColor -> gray like the native target
        w.append(img(ROW_X + (CARD_W - sz) / 2, rows_y + L['t_spr'], sz, sz, src_bind=f'{k}.spr',
                     need_bind=f'{gate}.spr', x_bind=f'{k}.x', flip_x=side == 'e',
                     tint='#FFFFFFFF', tint_bind=f'{k}.glow', tint_colors=GLOW))
        w.append(label(ROW_X + 2, rows_y + L['t_name'], CARD_W - 4, bind=f'{k}.name', font='bold', align='center', **g))
        w.append(label(ROW_X + 14, rows_y + L['t_lv'], 14, text='Lv', col=P['lvl'], **g))
        w.append(value(ROW_X + 30, rows_y + L['t_lv'], 22, f'{k}.lv', col=P['lvl'], **g))
        w.append(bar(ROW_X + 4, rows_y + L['t_hp'], CARD_W - 8, 3, f'{k}.hp', f'{k}.hpmax', P['hp'], **g))
        # W3 compact: "[sign] Weak [icons]" / "[sign] Resist [icons]", native words and icons
        w.append(label(ROW_X + 2, rows_y + L['t_wk'], CARD_W - 4, bind=f'{k}.wk', font='small',
                       need_bind=f'{gate}.wk', x_bind=f'{k}.x'))
        w.append(label(ROW_X + 2, rows_y + L['t_rs'], CARD_W - 4, bind=f'{k}.rs', font='small',
                       need_bind=f'{gate}.rs', x_bind=f'{k}.x'))
        # while choosing a target, a tap on the card confirms it natively; gated on the native
        # selector taking that monster (<side><i>.tok)
        w.append(tap(ROW_X, rows_y, CARD_W, h, f'b_tgt_{i}' if side == 'e' else f'b_ally_{i}',
                     need_bind=f'tgt.{k}' if side == 'e' else f'tgt.t{k}', x_bind=f'{k}.x'))
        CARD_DERIVED.update({f'{gate}.sel': f'{gate} && {sel}', f'{gate}.spr': f'{gate} && {k}.spr.on',
                             f'{gate}.wk': f'{gate} && {k}.wk.on', f'{gate}.rs': f'{gate} && {k}.rs.on'})
        if side == 'e':
            CARD_DERIVED.update({f'{gate}.hitw': f'{gate} && {k}.hitw', f'{gate}.hitr': f'{gate} && {k}.hitr'})
        else:
            CARD_DERIVED.update({gate: f'b.tgta && {k}.v', f'tgt.{gate}': f'b.tgt && b.tgta && {k}.tok'})
    return w


CARD_DERIVED: dict[str, str] = {}


def level_popup(L):
    """Y on a skill with levels: the native level popup (CombatUIController.LevelSelect) over the
    skill window -- one row per level, highest first (icon with its level number, name with the
    native type / element icons, the cost); the native cursor's row lit, rows the monster cannot
    use dimmed. A tap picks that level the native way (its A: SelectAction); Back = its B."""
    top = L['top']
    wh = MID_BOTTOM - top
    sub = dict(need_bind='b.lvl')
    w = [rect(PANE_X, top, PANE_W, wh, P['bg'], **sub)]
    w += window(PANE_X, top, PANE_W, wh, '{s:selectlevel}', **sub)
    rows_y = top + 18
    sdy, sh = L['s_dy'], L['s_h']
    for i in range(5):
        y = rows_y + i * sdy
        g = dict(need_bind=f'lv{i}.v')
        w += row(ROW_X, y, ROW_W, sh, on_bind=f'lv{i}.sel', blink=True, **g)
        w.append(img(ROW_X + 4, y + L['s_ico'], 16, 16, src_bind=f'lv{i}.ico', need_bind=f'lv{i}.ico.on'))
        w.append(label(ROW_X + 13, y + L['s_ico'] + 8, 10, bind=f'lv{i}.badge', font='small', align='right',
                       need_bind=f'lv{i}.badge.on'))
        w.append(label(ROW_X + 24, y + L['s_txt'], ROW_W - 70, bind=f'lv{i}.label', font='bold', **g))
        w.append(rect(ROW_X, y, ROW_W, sh, P['strip'], 110, need_bind=f'lv{i}.dim'))
        w.append(value(ROW_X + ROW_W - 44, y + L['s_txt'], 28, f'lv{i}.mp', col=P['white'], align='right', **g))
        w.append(img(ROW_X + ROW_W - 15, y + L['s_mpi'] - 1, 10, 10, spr('ui/icon_mana', 5), **g))
        w.append(tap(ROW_X, y, ROW_W, sdy, f'b_lvl_{i}', cue=(ROW_X, y, ROW_W, sh), **g))
    for wd in w:
        wd['hide_bind'] = 'b.tgt'
        wd['keep_max'] = 0
    return w


def item_window(L):
    """Items: the native item window as the game draws it -- every item it lists, in its order,
    the cursor's column expanded (icon, name, count) and the other columns as icon strips
    (module: it.w<n> columns, it.ex the expanded column's x shift, sk<k>.x each strip's x). A
    tap on a row or an icon uses that item natively (the top cursor moves there first)."""
    top = L['top']
    wh = MID_BOTTOM - top
    sub = dict(need_bind='b.items')
    w = [rect(PANE_X, top, PANE_W, wh, P['bg'], **sub)]  # opaque over the skill list
    w += window(PANE_X, top, PANE_W, wh, 'Items', **sub)
    rows_y = top + 18
    dy, h, isz = L['i_dy'], L['i_h'], L['i_isz']
    for ncols in (1, 2, 3):
        rw = ROW_W - (ncols - 1) * 20
        for i in range(6):
            y = rows_y + i * dy
            g = dict(need_bind=f'ix{i}.v.{ncols}', x_bind='it.ex')
            w += row(ROW_X, y, rw, h, on_bind=f'ix{i}.sel.{ncols}', **g)
            w.append(img(ROW_X + 2, y + L['i_ico'], isz, isz, src_bind=f'ix{i}.ico', **g))
            w.append(label(ROW_X + 4 + isz, y + L['i_txt'], rw - isz - 52, bind=f'ix{i}.name', font=L['i_font'], **g))
            w.append(label(ROW_X + rw - 48, y + L['i_txt'], 44, bind=f'ix{i}.info', align='right', **g))
            w.append(rect(ROW_X, y, rw, h, P['strip'], 110, need_bind=f'ix{i}.dim.{ncols}', x_bind='it.ex'))
            w.append(tap(ROW_X, y, rw, dy, f'b_itemx_{i}', cue=(ROW_X, y, rw, h), **g))
    for k in range(2):
        for i in range(6):
            y = rows_y + i * dy
            g = dict(need_bind=f'sk{k}.{i}.v', x_bind=f'sk{k}.x')
            w.append(sliced(ROW_X, y, 18, h, 'ui/menuItem', 2, **g))
            w.append(img(ROW_X + 1, y + L['i_ico'], isz, isz, src_bind=f'sk{k}.{i}.ico', **g))
            w.append(rect(ROW_X, y, 18, h, P['strip'], 110, need_bind=f'sk{k}.{i}.dim', x_bind=f'sk{k}.x'))
            # the strip's whole column pitch (ItemStripPx 80) and row pitch
            w.append(tap(ROW_X, y, 20, dy, f'b_items{k}_{i}', cue=(ROW_X, y, 18, h), **g))
    return w


def switch_window(L):
    """Switch Monster: the native list of the backup monsters (sprite, name, level); a tap
    switches that monster in natively."""
    top = L['top']
    wh = MID_BOTTOM - top
    sub = dict(need_bind='b.switch')
    w = [rect(PANE_X, top, PANE_W, wh, P['bg'], **sub)]
    w += window(PANE_X, top, PANE_W, wh, 'Switch', **sub)
    rows_y = top + 18
    for i in range(3):
        y = rows_y + i * L['w_dy']
        g = dict(need_bind=f'w{i}.v')
        # the native cursor's row (the D-pad on the top moves it too)
        w += row(ROW_X, y, ROW_W, L['w_h'], on_bind=f'w{i}.sel', blink=True, **g)
        w.append(img(ROW_X + 2, y + L['w_spr'], L['w_sz'], L['w_sz'], src_bind=f'w{i}.spr', need_bind=f'w{i}.spr.on'))
        w.append(label(ROW_X + 40, y + L['w_name'], 150, bind=f'w{i}.name', font='bold', **g))
        w.append(label(ROW_X + 40, y + L['w_info'], 40, bind=f'w{i}.info', col=P['lvl'], need_bind=f'w{i}.info.on'))
        # the native switch card's weaknesses / resistances (sign, word, element icons)
        w.append(label(ROW_X + 82, y + L['w_info'], 110, bind=f'w{i}.wr', need_bind=f'w{i}.wr.on'))
        w.append(rect(ROW_X, y, ROW_W, L['w_h'], P['strip'], 110, need_bind=f'w{i}.v', hide_bind=f'w{i}.ok', keep_max=0))
        w.append(tap(ROW_X, y, ROW_W, L['w_h'], f'b_switch_{i}', **g))
    return w


def info_card(L):
    """Monster Info: the native MonsterInfo card of the monster under the native cursor --
    headline ("Level 5 Vaero"), the monster, health, mana, crit chance / damage, weak / resist."""
    top = L['top']
    wh = MID_BOTTOM - top
    sub = dict(need_bind='b.info')
    w = [rect(PANE_X, top, PANE_W, wh, P['bg'], **sub)]
    w += window(PANE_X, top, PANE_W, wh, None, title_bind='b.ihead', **sub)
    sz, y0 = L['c_sz'], top + L['c_y']
    w.append(img(ROW_X, y0, sz, sz, src_bind='b.ispr', need_bind='b.ispr.on'))
    x = ROW_X + sz + 6
    vw = PANE_X + PANE_W - 6 - x
    rows = (('ui/icon_health', 'b.ihp', P['hp']), ('ui/icon_mana', 'b.imp', P['mp_txt']))
    for k, (icon, bind, col) in enumerate(rows):
        y = y0 + k * 13
        # the native 8 x 8 / 8 x 7 sprites at 5x (information icons >= 40 px)
        w.append(img(x, y + 1, 10, 10 if icon == 'ui/icon_mana' else 8.75, spr(icon, 5), **sub))
        w.append(label(x + 13, y, vw - 13, bind=bind, col=col, need_bind=f'{bind}.on'))
    for k, (title, bind) in enumerate((('critchance', 'b.icrit'), ('critdamage', 'b.icd'))):
        y = y0 + 26 + k * 13
        w.append(label(x, y, 66, text='{s:' + title + '}', col=P['sec'], font='small', **sub))
        w.append(label(x + 66, y, vw - 66, bind=bind, col=P['val'], align='right', need_bind=f'{bind}.on'))
    w.append(label(ROW_X, top + L['c_wr'], ROW_W, bind='b.iwr', need_bind='b.iwr.on'))
    # the enemies the native cursor browses too (the allies are the party bar's cards): a tap
    # moves the native cursor there and the card shows that monster
    cy, cs = top + L['c_chip'], L['c_chs']
    for i in range(3):
        x = ROW_X + i * (cs + 6)
        g = dict(need_bind=f'inf.e{i}')
        w.append(sliced(x, cy, cs, cs, 'ui/menuItem', 2, **g))
        w.append(sliced(x, cy, cs, cs, 'ui/menuItemOpaque', 2, need_bind=f'e{i}.isel'))
        w += selector(x, cy, cs, cs, blink=True, need_bind=f'e{i}.isel')
        w.append(img(x + 1, cy + 1, cs - 2, cs - 2, src_bind=f'e{i}.spr', need_bind=f'inf.e{i}.spr', flip_x=True))
        w.append(tap(x, cy, cs, cs, f'b_info_{8 + i}', **g))
    return w


def check_t1_fit(widgets):
    """Every T1 widget lies below the rule band and inside the middle band, so the rule covers
    nothing and every row stays whole and tappable."""
    for wd in widgets:
        y, h = wd['rect'][1] / S, wd['rect'][3] / S
        if wd.get('on_tap') == 'b_level':
            continue  # an invisible touch target reaching up over a band with nothing to tap
        assert y >= MID_T1['top'] - 1e-6 and y + h <= MID_BOTTOM + 1e-6, wd


def check_skill_strip(widgets, t1):
    """The skill strip's cells (steward re-check R1): both of the strip's sides stay inside the
    pane's rows (ss.x = 0 or the rows' width minus the cell), the tap is at least 88 canvas px wide
    and, in NORMAL, 88 px tall. On the Keeper's first turn the row pitch is 18 px (72 canvas px,
    the owner-accepted L7 rows), so the cell's height follows it there."""
    taps = [wd for wd in widgets if str(wd.get('on_tap', '')).startswith('b_skst_')]
    assert len(taps) == 5, len(taps)
    for wd in taps:
        x, y, w, h = wd['rect']
        assert w >= 88 and (t1 or h >= 88), wd
        for dx in (0, (ROW_W - SKILL_STRIP) * S):
            assert ROW_X * S <= x + dx and x + dx + w <= (ROW_X + ROW_W) * S, wd


def check_normal_fit(widgets):
    for wd in widgets:
        y, h = wd['rect'][1] / S, wd['rect'][3] / S
        if wd.get('type') == 'image' and wd.get('x_bind') == 'sel.d':
            continue  # the selector brackets sit 2 px outside their row
        if wd.get('on_tap') == 'b_level':
            continue  # an invisible touch target reaching up over the combo strip (no taps there)
        assert y >= MID_NORMAL['top'] - 2 - 1e-6 and y + h <= MID_BOTTOM + 2 + 1e-6, wd


BACK_B = '{i:ui/ButtonBSwitch} {s:back}'
DESC_DERIVED: dict[str, str] = {}


def desc_page_count(key, gate, **kw):
    """The page count of a paged description ("2/3", module <key>.pg) at the end of its line: the
    strip's right end (<key>.pgw), or left of the window Back while it shows (<key>.pgn; the
    module then wraps the pages at 216 px)."""
    out = []
    for suffix, x in (('pgw', 284), ('pgn', 224)):
        DESC_DERIVED[f'{key}.{suffix}c'] = f'{gate} && {key}.{suffix}'
        out.append(label(x, 202, 20, bind=f'{key}.pg', font='small', col=P['sec'], align='right',
                         need_bind=f'{key}.{suffix}c', **kw))
    return out


def page_battle():
    w = scene_bg(150)
    w += party_bar()
    # combo strip (r3 combo_strip): the game's own Combo / Damage figures
    y0 = COMBO_Y
    w.append(strip(0, y0, VW, 16))
    w.append(label(4, y0 + 1, 46, text='Combo:', font='bold', need_bind='b.v'))
    w.append(label(50, y0 + 1, 28, bind='b.combo', col=P['val'], font='bold', need_bind='b.v'))
    w.append(label(80, y0 + 2, 46, text='Damage:', align='right', need_bind='b.dmg.on'))
    w.append(label(128, y0 + 2, 36, bind='b.dmg', col=P['val'], need_bind='b.dmg.on'))
    w.append(label(166, y0 + 2, 64, text='Keeper Battle', col=P['lvl'], need_bind='b.keeper'))
    # Keeper battle: unrevealed opponents (not yet on the field) are only counted, never named
    w.append(label(232, y0 + 2, 74, bind='b.unknown', col=P['sec'], align='right', need_bind='b.keeper'))
    # the middle band: NORMAL, and the Keeper duel's first turn with its own rule band (T1)
    for L in (MID_NORMAL, MID_T1):
        band = battle_mid(L) + item_window(L) + switch_window(L) + info_card(L) + level_popup(L)
        (check_t1_fit if L['t1'] else check_normal_fit)(band)
        check_skill_strip(band, L['t1'])
        w += layout_gate(band, L['t1'])
    # detail strip (bottom): the highlighted skill, the targeted enemy, or the highlighted item
    w.append(strip(0, 188, VW, 52))
    w.append(label(6, 190, 178, bind='b.sname', font='bold', need_bind='b.cmd', hide_bind='b.railfocus', keep_max=0))
    # the cost ends left of the window Back (the level popup has one)
    w.append(label(186, 191, 34, text='Mana:', need_bind='b.cmd', hide_bind='b.railfocus', keep_max=0))
    w.append(value(220, 191, 28, 'b.smp', col=P['val'], need_bind='b.cmd', hide_bind='b.railfocus', keep_max=0))
    # the hovered skill's (or level's) native description (SkillTooltip), one line; a longer one
    # is shown whole a line at a time (the module turns the line every 3.5 s, a tap on the strip
    # turns it now) with its page count at the right ("1/2"; owner ruling R3, 2026-11-14)
    w.append(wrapped(6, 202, 298, 1, 'b.sdesc', need_bind='b.sdesc.cmd', hide_bind='b.railfocus', keep_max=0))
    w += desc_page_count('b.sdesc', 'b.sdesc.cmd', hide_bind='b.railfocus', keep_max=0)
    # cursor on the command rail: the command's name over its native description (SkillTooltip)
    w.append(label(6, 190, 298, bind='b.rname', font='bold', need_bind='b.railfocus'))
    w.append(wrapped(6, 202, 298, 1, 'b.rdesc', need_bind='b.railfocus'))
    w += desc_page_count('b.rdesc', 'b.railfocus')
    w.append(label(6, 190, 200, bind='b.tname', font='bold', need_bind='b.tgt'))
    w.append(label(6, 202, 34, text='Weak', col=P['weak'], need_bind='b.tweak.on'))
    w.append(label(40, 202, 110, bind='b.tweak', need_bind='b.tweak.on'))
    w.append(label(156, 202, 40, text='Resist', col=P['res'], need_bind='b.tres.on'))
    w.append(label(198, 202, 106, bind='b.tres', need_bind='b.tres.on'))
    # the item window's ItemTooltip (moved off the top): icon, name, type, description
    w.append(img(6, 189, 12, 12, src_bind='b.tipico', need_bind='b.tip'))
    w.append(label(21, 190, 128, bind='b.tipname', font='bold', need_bind='b.tip'))
    # (the type ends left of the window Back at 250)
    w.append(label(150, 191, 96, bind='b.tiptype', col=P['sec'], align='right', need_bind='b.tip'))
    w.append(wrapped(6, 202, 298, 1, 'b.tipdesc', need_bind='b.tip'))
    w += desc_page_count('b.tipdesc', 'b.tip')
    # a paged description: a tap on the strip (left of Back, the W5 rows below have no taps)
    # turns to its next line; 246 x 52 (984 x 208 canvas px)
    w.append(tap(0, 188, 246, 52, 'b_desc', need_bind='b.desc.pg'))
    w.append(label(6, 198, 298, text='Enemy turn', col=P['sec'], align='center', need_bind='!b.turn'))
    # W5: the Combat Result screen's five factors as they stand now, in the native order and with
    # the native labels, each value right after its label (two rows: Enemy Difficulty, Combat
    # Duration / Healthiness, Buffs / Debuffs, Execution). Widths: the measured small-font labels.
    for x, lw, vw, key, title, y in ((4, 74, 32, 'b.diff', 'difficulty', 216),
                                      (116, 74, 60, 'b.dur', 'duration', 216),
                                      (4, 54, 30, 'b.heal', 'healthiness', 228),
                                      (94, 72, 32, 'b.buffs', 'buffs', 228),
                                      (204, 47, 55, 'b.exec', 'execution', 228)):
        w.append(label(x, y, lw, text='{s:' + title + '}', font='small', need_bind='b.exec.on'))
        w.append(label(x + lw + 1, y, vw, bind=key, font='small', col=P['val'], need_bind='b.exec.on'))
    # Back: what B does natively there -- target / Monster Info: cancel the selector; the item
    # window and the Switch Monster list: their own cancel back to the commands. B does exactly
    # that, so its native glyph is shown (GLYPH-AUDIT.md).
    # a refusal's reason takes the description strip's first lines while it lasts (left of Back)
    w.append(strip(0, 188, 246, 26, 255, need_bind='h.no'))
    w.append(label(6, 191, 238, bind='h.msg', col=P['weak'], font='bold', need_bind='h.no'))
    # the tap covers the strip's right end, 28 px tall (the drawn button stays 16)
    w += button(250, 190, 56, 16, BACK_B, 'b_back', need_bind='b.back', tap_rect=(250, 188, 58, 28))
    w += button(250, 190, 56, 16, BACK_B, 'b_subback', need_bind='b.subback', tap_rect=(250, 188, 58, 28))
    return dict(id='battle', widgets=w + turn1_banner() + tabs('home') + wrong_build())


def battle_derived():
    """The item window's per-width row gates (it.w<n>) and the strip cells' dim gates."""
    out = []
    for n in (1, 2, 3):
        for i in range(6):
            out.append({'name': f'ix{i}.v.{n}', 'expr': f'ix{i}.v && it.w{n}'})
            out.append({'name': f'ix{i}.sel.{n}', 'expr': f'ix{i}.sel && it.w{n}'})
            out.append({'name': f'ix{i}.dim.{n}', 'expr': f'ix{i}.v && !ix{i}.ok && it.w{n}'})
    for k in range(2):
        for i in range(6):
            out.append({'name': f'sk{k}.{i}.dim', 'expr': f'sk{k}.{i}.v && !sk{k}.{i}.ok'})
    # the skill window's rows per width (sk.w<n>) and its icon strip's dim cells
    for n in (1, 2):
        for i in range(5):
            out.append({'name': f's{i}.v.{n}', 'expr': f's{i}.v && sk.w{n}'})
            out.append({'name': f's{i}.sel.{n}', 'expr': f's{i}.sel && sk.w{n}'})
            out.append({'name': f's{i}.dim.{n}', 'expr': f's{i}.dim && sk.w{n}'})
            out.append({'name': f's{i}.badge.{n}', 'expr': f's{i}.badge.on && sk.w{n}'})
    # (the level popup's rows: the module publishes lv<i>.dim itself)
    for i in range(5):
        out.append({'name': f'ss{i}.dim', 'expr': f'ss{i}.v && !ss{i}.ok'})
    out.append({'name': 'b.sdesc.cmd', 'expr': 'b.cmd && b.sdesc.on'})
    out.append({'name': 'b.cmdt', 'expr': 'b.cmd && !b.lvly'})
    out += [{'name': k, 'expr': v} for k, v in sorted(DESC_DERIVED.items())]
    # the target cards' gates (target_cards)
    out += [{'name': k, 'expr': v} for k, v in sorted(CARD_DERIVED.items())]
    return out


def option_row(y, text, on_bind, action):
    out = row(6, y, VW - 12, 32, on_bind=on_bind)
    out.append(label(12, y + 9, 200, text=text))
    out.append(label(212, y + 9, 90, text='On', col=P['hp'], align='right', need_bind=on_bind))
    out.append(label(212, y + 9, 90, text='Off', col=P['sec'], align='right', need_bind='!' + on_bind))
    out.append(tap(6, y, VW - 12, 32, action))
    return out


def page_opts():
    w = scene_bg(165)
    w += window(2, 2, VW - 4, 236, 'Options')
    # the native battle windows the Combat page mirrors (command menu, skill / item tooltips,
    # item window, switch list, Monster Info card), hidden on top while On; persisted flags
    w += option_row(24, 'Hide battle menu on top', '@flag:ms.hide_menu', 'opt_hide')
    w += option_row(58, 'Hide combo counter on top', '@flag:ms.hide_combo', 'opt_combo')
    # still sprites, no selection / map blink (a persisted runtime flag the module reads)
    w += option_row(92, 'Reduce motion', '@flag:ms.reduce_motion', 'opt_motion')
    # touch feedback on the device (tap / confirm / refusal), a persisted runtime flag
    w += option_row(126, 'Haptics', '@flag:ms.haptics', 'opt_haptics')
    w.append(label(12, 172, VW - 24, text='The menu keeps working with the buttons.', col=P['sec'], scale=2))
    w.append(label(12, 186, VW - 24, text='Needs the in-game helper', col=P['sec'], scale=2, need_bind='!h.alive'))
    return dict(id='opts', widgets=w + tabs('opts') + wrong_build())


# ---------------------------------------------------------------- haptics (WORKFLOW 9b)
HAPTIC_FLAG = '@flag:ms.haptics'
HAPTIC_DERIVED: dict[str, str] = {}


def haptic_variants(pages):
    """Haptics On/Off (Options, persisted flag ms.haptics): every touch widget gets a twin with
    "haptic": "off" (a widget override silences its tap and its refusal too); the original shows
    while the flag is 1, the twin while it is 0. A widget's existing hide gate is folded into a
    derived expr so both gates hold."""
    for page in pages:
        out = []
        for wd in page['widgets']:
            if 'on_tap' not in wd:
                out.append(wd)
                continue
            hb = wd.get('hide_bind')
            if hb is None:
                cond = None
            elif wd.get('keep_max') == 0 and 'keep_min' not in wd:
                cond = f'({LAYOUT_DERIVED[hb]})' if hb in LAYOUT_DERIVED else hb   # hidden while set
                tag = ''
            elif wd.get('keep_min') == 1 and 'keep_max' not in wd:
                cond = f'!{hb}'                                                    # hidden while 0
                tag = 'n'  # the inverted gate needs its own name (NORMAL and T1 share b.t1)
            else:
                raise AssertionError(wd)
            twin = dict(wd, haptic='off')
            for d, on in ((wd, True), (twin, False)):
                d.pop('keep_min', None)
                d.pop('keep_max', None)
                if cond is None:
                    d['hide_bind'] = HAPTIC_FLAG
                    d['keep_min' if on else 'keep_max'] = 1 if on else 0
                else:
                    name = f"H{int(on)}{tag}.{hb}"
                    expr = f"{'!' if on else ''}{HAPTIC_FLAG} || {cond}"
                    assert HAPTIC_DERIVED.setdefault(name, expr) == expr, name
                    HAPTIC_DERIVED[name] = expr
                    d['hide_bind'] = name
                    d['keep_max'] = 0
            out += [wd, twin]
        page['widgets'] = out
    return pages


# per-action strength: the direct native commits confirm, selections tick lightly, the rest
# (tabs, rail commands, Back, Options toggles) use the package's tap strength
CONFIRM_ACTIONS = ('follow_', 'b_cmd_', 'b_skst_', 'b_lvl_', 'b_item', 'b_switch_', 'b_tgt_', 'b_ally_', 'tp_',
                   'eq_pick_')
LIGHT_ACTIONS = ('team_focus_', 'eq_slot_', 'eq_close', 'swap_mode', 'tp_toggle',
                 'b_info_')


# A derived expr reads an unpublished name as missing, and missing spreads through ! and &&
# (mod_expr.h), so a gate like "x.v && !x.ok" never turns on when ok is published only as 1.
# Every name an expr negates is a flag, a "cmp" derived value (always 0/1) or one the module
# publishes as 0/1 whenever it is read (listed here).
ALWAYS_PUBLISHED = {'b.t1', 'b.lvly', 'q.open', 'ti.off', 'm.tpok'}
ALWAYS_PUBLISHED_RE = re.compile(r'(ix\d|sk\d\.\d|ss\d)\.ok')


def check_negations(derived):
    cmp_names = {d['name'] for d in derived if 'cmp' in d}
    for d in derived:
        for name in re.findall(r'!\s*([@A-Za-z_][\w.:@]*)', d.get('expr', '')):
            assert (name.startswith('@flag:') or name in cmp_names or name in ALWAYS_PUBLISHED or
                    ALWAYS_PUBLISHED_RE.fullmatch(name)), (d, name)


def manifest():
    pages = haptic_variants(fb_cues([page_field(), page_map(), page_team(), page_battle(), page_opts()]))
    actions = {
        'tab_home': {'kind': 'page', 'page': 'field', 'transition': 'fade', 'duration_ms': 150,
                     'enabled_bind': '!ms.combat'},
        'tab_home_battle': {'kind': 'page', 'page': 'battle', 'transition': 'fade', 'duration_ms': 150,
                            'enabled_bind': 'ms.combat'},
        'tab_map': {'kind': 'page', 'page': 'map', 'transition': 'fade', 'duration_ms': 150},
        'tab_team': {'kind': 'page', 'page': 'team', 'transition': 'fade', 'duration_ms': 150},
        'tab_opts': {'kind': 'page', 'page': 'opts', 'transition': 'fade', 'duration_ms': 150},
    }
    def mod(name, arg=0):
        return {'kind': 'module', 'action': name, 'argument': arg}

    for i in range(TEAM_ROWS):
        actions[f'team_focus_{i}'] = mod('team_focus', i)
    actions['opt_motion'] = {'kind': 'flag', 'flag': 'ms.reduce_motion'}
    for i in range(RAIL_TABS):
        actions[f'b_rail_{i}'] = mod('b_rail', i)
    for i in range(6):
        actions[f'follow_{i}'] = mod('follow', i)
    for i in range(5):
        actions[f'b_cmd_{i}'] = mod('b_cmd', i)
        actions[f'b_skst_{i}'] = mod('b_skst', i)
        actions[f'b_lvl_{i}'] = mod('b_lvl', i)
    actions['b_desc'] = mod('b_desc')
    for i in range(3):
        actions[f'b_switch_{i}'] = mod('b_switch', i)
    # the item window: argument = slot * 8 + row (slot 0 the expanded column, 1.. the strips)
    for i in range(6):
        actions[f'b_itemx_{i}'] = mod('b_item', i)
        for k in range(2):
            actions[f'b_items{k}_{i}'] = mod('b_item', (k + 1) * 8 + i)
    for i in range(3):
        actions[f'b_tgt_{i}'] = mod('b_tgt', i)
        actions[f'b_ally_{i}'] = mod('b_ally', i)
        actions[f'b_info_{i}'] = mod('b_info', i)
        actions[f'b_info_{8 + i}'] = mod('b_info', 8 + i)
    for i in range(TP_ROWS):
        actions[f'tp_{i}'] = mod('tp', i)
    for i in range(8):
        actions[f'eq_pick_{i}'] = mod('eq_pick', i)
    for k in EQUIP_SLOTS:
        actions[f'eq_slot_{k}'] = mod('eq_slot', k)
    # (h_detach / h_cache are diagnostics-build actions, ms_config.h: not shipped)
    for name in ('b_back', 'b_subback', 'b_level', 'swap_mode', 'eq_close'):
        actions[name] = mod(name)
    actions['opt_hide'] = {'kind': 'flag', 'flag': 'ms.hide_menu'}
    actions['opt_combo'] = {'kind': 'flag', 'flag': 'ms.hide_combo'}
    actions['tp_toggle'] = {'kind': 'flag', 'flag': 'ms.tpc'}
    for vid, *_ in MAP_VIEWS:
        actions[f'map_reset_{vid}'] = {'kind': 'view_reset', 'view': vid}
    for name, act in actions.items():
        if name.startswith(CONFIRM_ACTIONS) and name != 'tp_toggle':
            act['haptic'] = 'confirm'
        elif name.startswith(LIGHT_ACTIONS):
            act['haptic'] = 'light'
    actions['opt_haptics'] = {'kind': 'flag', 'flag': 'ms.haptics'}
    # in combat the first tab is the battle page
    pages[0]['_note'] = 'tab_home lands here outside combat; page_binds switch to battle in combat'
    derived = [
        {'name': 'ms.nogame', 'cmp': 'le', 'a': 'ms.state', 'b': 1},
        # exploring (ms::State::Exploring): the only state where the game takes a team swap,
        # equipping or a follower change (helper FIELD_GATE, module FieldEdit)
        {'name': 'ms.explore', 'cmp': 'eq', 'a': 'ms.state', 'b': 2},
    ]
    for i in range(6):  # the Exploration page's follower rows, before the feedback gates read them
        derived.append({'name': f'fp{i}.ok', 'expr': f'p{i}.v && ms.explore'})
        derived.append({'name': f'fp{i}.lock', 'expr': f'p{i}.v && !ms.explore'})
    derived += battle_derived() + map_derived()
    # the battle middle band's layout gates (layout_gate)
    derived += [{'name': k, 'expr': v} for k, v in sorted(LAYOUT_DERIVED.items())]
    # the haptics twins' gates (haptic_variants)
    derived += [{'name': k, 'expr': v} for k, v in sorted(HAPTIC_DERIVED.items())]
    # the result-feedback cues' gates (fb_cues), after every gate they read
    derived += [{'name': k, 'expr': v} for k, v in sorted(FB_DERIVED.items())]
    for i in range(TEAM_ROWS):  # the Team page's rows (the model publishes up to p11)
        derived.append({'name': f'p{i}.light', 'cmp': 'eq', 'a': f'p{i}.shift', 'b': 1})
        derived.append({'name': f'p{i}.dark', 'cmp': 'eq', 'a': f'p{i}.shift', 'b': 2})
        derived.append({'name': f'tsel{i}', 'cmp': 'eq', 'a': 't.i', 'b': i})
    for k in EQUIP_SLOTS:
        derived.append({'name': f'qsel{k}', 'cmp': 'eq', 'a': 'q.slot', 'b': k})
    # the Team page's Swap / Equipment slots: the game takes both only while exploring
    derived.append({'name': 't.edit', 'expr': 't.v && ms.explore'})
    derived.append({'name': 't.lock', 'expr': 't.v && !ms.explore'})
    derived.append({'name': 't.swaptap', 'expr': 't.edit && !q.open'})
    # the enemy/ally target taps (battle_mid / battle_subs): tappable only while targeting AND
    # that slot actually holds a monster, so an empty card in a <3-enemy fight is not a live tap
    for i in range(3):
        # ... and the native selector takes that monster (<side><i>.tok: in TargetSelector.targets
        # or on a group action's side, the helper's TARGET_PICK test; steward re-check R4)
        derived.append({'name': f'tgt.e{i}', 'expr': f'b.tgt && e{i}.tok'})
        derived.append({'name': f'tgt.a{i}', 'expr': f'b.tgt && a{i}.tok'})
        # Monster Info browse: allies on the party bar, enemies as chips under the card
        derived.append({'name': f'inf.a{i}', 'expr': f'b.info && a{i}.v'})
        derived.append({'name': f'inf.e{i}', 'expr': f'b.info && e{i}.v'})
        derived.append({'name': f'inf.e{i}.spr', 'expr': f'b.info && e{i}.spr.on'})
    check_negations(derived)
    return {
        'format': 1,
        'title_id': '0100A0F00DA68000',
        'name': 'Monster Sanctuary dual screen',
        'min_runtime': 22,
        'requires_module': True,
        # the format-2 guest helper (native/helper/ms_guest_helper.S), built per game build
        'load_plan': 'guest/<build>/load-plan.json',
        'canvas_w': VW * S,
        'canvas_h': VH * S,
        'background': c(P['bg']),
        'font': 'file:ms_font.txt',
        'font_atlas': 'module:ms:font',
        '_note': 'Generated by tools/monster-sanctuary/gen_manifest.py (D2 "Banners+" r3 layout).',
        'nav': False,
        # ms.hide_menu / ms.hide_combo: the Options hide toggles, persisted (default On / Off: a
        # fresh profile and one upgraded from a package without them both start from these);
        # ms.tpc: the Map page's Teleporter panel collapsed, this session only
        'flags': {'ms.reduce_motion': 0, 'ms.haptics': 1, 'ms.hide_menu': 1, 'ms.hide_combo': 0, 'ms.tpc': 0},
        'persist_flags': ['ms.reduce_motion', 'ms.haptics', 'ms.hide_menu', 'ms.hide_combo'],
        # WORKFLOW 9b / MOTION-AND-HAPTICS.md: taps click, direct native commits confirm (action
        # overrides), refusals reject; no swipe haptic (a swipe ignores widget overrides, so the
        # Haptics Off option could not silence it); haptics only on the player's own touches
        'haptics': {'enabled': True, 'respect_system': True, 'tap': 'click', 'write': 'confirm',
                    'swipe': 'off', 'refused': 'reject'},
        'derived': derived,
        'actions': actions,
        'page_binds': [
            {'point': 'ms.combat', 'equals': 1, 'when_equal': {'page': 'battle', 'transition': 'fade',
                                                                'duration_ms': 150},
             'when_not_equal': {'page': 'field', 'transition': 'fade', 'duration_ms': 150}},
        ],
        'pages': pages,
    }


if __name__ == '__main__':
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(manifest(), indent=1) + '\n')
    print(OUT)
