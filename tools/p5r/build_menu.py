#!/usr/bin/env python3
"""P5R bottom-screen START MENU (camp menu) replica -- m_ui pass.

usage: build_menu.py <dualscreen dir>

Runs AFTER build_gameart.py (and the mapfog merge) on the same dualscreen dir: it reads that
manifest, replaces the PARTY page (id "live", which every existing action/page_bind already targets)
with the START MENU hub, and adds the sub-pages SKILL / ITEM / EQUIP / PERSONA / STATS / CONFIDANT
(+ confidant detail) / REQUEST / CALENDAR. All other pages, actions, page_binds, outputs are kept
as they are. Idempotent (rerun after build_gameart.py).

Data comes from the P5R module's outputs (persona, items, social readers). Fields that no
module publishes yet simply stay hidden (every widget is gated on its group's *.ready / present /
count), so the package is safe to ship before the readers land.

Companion-side UI state (which member / row / tab / page is highlighted) lives in runtime flags
`ui.*` (manifest "flags" defaults, "ui.set.*" flag actions carrying the tapped row as $payload) and
V12 derived comparisons (`ui.eq.<flag>.<v>`), so no module round-trip is needed for navigation.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
from p5style import BG, DIM, INK, RED, TILE, TILE_HI, Kit, cap_px, rgba  # noqa: E402
import menu_art as art  # noqa: E402
from recipe import Art  # noqa: E402

out_dir = Path(sys.argv[1])
man_path = out_dir / 'manifest.json'
m = json.loads(man_path.read_text())
kit = Kit(out_dir, subdir='ui/m')
# the gameart pass already wrote ui/PROVENANCE.json; ours goes to ui/m/PROVENANCE.json
T = Kit.text
IMG = Kit.image
V = Kit.value
W, H = m['canvas_w'], m['canvas_h']
BLACK = '#FF000000'
WHITE = '#FFFFFFFF'
# native dimmed ITEM row (E3D770 style 5): grey name / icon, grey count plate
GREY_INK, GREY_ICON, GREY_PLATE = '#FF6E6468', '#FF5A5A5A', '#FF6E6E6E'
DARKRED = '#FF4A0A0E'
CYAN = '#FF3FE0E8'   # native highlight accent (cursor plate on the SKILL/PERSONA lists)
HP_INK = '#FF4FE3B0'  # native camp HP digits (teal-green)
SP_INK = '#FFE36FE0'  # native camp SP digits (pink)
# ui_a: exact native HUD / camp STATS ink, sampled from native captures (DATA13 STATS list + field HUD)
HP_NAT = '#FF66FDDC'  # teal (tosca) HP digits / bars
SP_NAT = '#FFFF64E8'  # pink SP digits / bars

derived = [d for d in m.get('derived', []) if not d.get('name', '').startswith('ui.')]
m['derived'] = derived
flags = m.setdefault('flags', {})
actions = m['actions']
_dnames = set()


def dv(name, **kw):
    if name not in _dnames:
        _dnames.add(name)
        derived.append(dict(name=name, **kw))
    return name


def flag(name, default=0):
    flags[name] = default
    act = f'ui.set.{name}'
    actions[act] = dict(kind='flag', flag=name, value='$payload')
    return act


def eq(fname, v):
    return dv(f'ui.eq.{fname}.{v}', cmp='eq', a=f'@flag:{fname}', b=v)


def both(*names):
    return dv('ui.and.' + '+'.join(names), all_nonzero=list(names))


def spr(sid, name, h=None, w=None, color=None, box=None, align='center'):
    return kit.sprite('camp00', sid, name, height=h, width=w, color=color, box=box, align=align)


def save(img, name, prov, rid=None):
    """Large art (busts, hero backdrops) is flat-shaded 2D art: a 128-colour palette with alpha
    keeps it visually identical at a fraction of the size (the runtime decodes paletted PNG).
    Module mode ships the recipe instead (decoded at full colour on the device)."""
    src = kit._save(img, name, prov, rid)
    if kit.mode == 'png' and img.width * img.height >= 120_000:
        path = out_dir / src[5:]
        img.im.quantize(128, method=Image.Quantize.FASTOCTREE).save(path, optimize=True)
        kit.provenance[src[5:]]['palette'] = 128
    return src


# ------------------------------------------------------------------ sprite numbers (no tables)
# A digit k of v is floor(v/10^k) - 10*floor(v/10^(k+1)): derived points, the image picks one of 10
# cells by value; leading zeros hide on floor(v/10^k) >= 1. n is the game's own cap (LV 99, HP/SP
# 999, money 9,999,999), so the top digit is floor(v/10^(n-1)) itself: 2(n-1) derived per number.
def sprite_num(bind, ds, right, y, n, need=None, color=INK, comma=None):
    key = bind.replace('.', '_')
    q = [bind] + [dv(f'ui.q.{key}.{k}', terms=[[bind, 10.0 ** -k]], add=1e-6, floor=True)
                  for k in range(1, n)]
    gate = dict(need_bind=need) if need else {}
    out = []
    x = right
    for k in range(n):
        d = q[k] if k == n - 1 else dv(f'ui.d.{key}.{k}', terms=[[q[k], 1], [q[k + 1], -10]])
        if comma and k in (3, 6):  # 9,893,651: a comma before the thousands AND the millions
            csrc, cw, ch = comma
            x -= cw
            out.append(IMG(csrc, x, y + ds['h'] - ch, cw, ch, hide_bind=q[k], keep_min=1, color=color,
                           **gate))
        x -= ds['pitch'] if k else ds['w']
        wd = dict(type='image', src=ds['srcs'][0], rect=[x, y, ds['w'], ds['h']], bind=d,
                  src_names=ds['srcs'], color=color, **gate)
        if k:
            wd.update(hide_bind=q[k], keep_min=1)
        out.append(wd)
    return out


def digit_set(key, base, h):
    return kit.digits(key, h, base)


# ------------------------------------------------------------------ shared art
def hero(stem, h, name, mul=(255, 255, 255)):
    im = Art.dds(f'BASE/CAMP/HEROTEX/{stem}.DDS').bbox().fit(h=h)
    if mul != (255, 255, 255):
        im = im.tint(mul)
    return save(im, name, {'source': f'BASE/CAMP/HEROTEX/{stem}.DDS', 'crop': 'alpha bbox',
                           'size': list(im.size), 'tint': list(mul)}), im.width, im.height


def slab(w, h, poly, name, fill=DARKRED):
    s = 4
    im = Art.new(w * s, h * s).polygon([(x * s, y * s) for x, y in poly], fill).resize(w, h)
    return save(im, name, {'source': 'generated (menu slab polygon)', 'poly': poly})


# Party heads (camp portraits), same ids as party.N.id / pstat.M.id
MEMBER_HEAD = {1: 252, 2: 253, 3: 254, 4: 255, 5: 256, 6: 257, 7: 258, 9: 259}


def heads(prefix, size):
    srcs = [''] * 11
    for mid, sid in MEMBER_HEAD.items():
        srcs[mid], _, _ = spr(sid, f'{prefix}{mid}', h=size, box=(size, size))
    srcs[10], _, _ = kit.sprite('partypanel', 177, f'{prefix}10', height=size, box=(size, size),
                                note='Kasumi battle face (no camp portrait exists)')
    srcs[8], _, _ = kit.sprite('partypanel', 176, f'{prefix}8', height=size, box=(size, size),
                               note='Futaba: battle-panel face (no camp portrait exists)')
    return srcs


HEAD_M = heads('hm', 96)
HEAD_S = heads('hs', 64)
FRAME, FRW, FRH = spr(260, 'hframe', w=118)  # black camp portrait plate
HP_T, HPW, HPH = spr(261, 'hp_t', h=26)
SP_T, SPW, SPH = spr(262, 'sp_t', h=26)
LV_T, LVW, LVH = spr(337, 'lv_t', h=30)
DG_M = digit_set('m', 264, 40)     # camp small italic digits
DG_B = digit_set('b', 274, 58)     # camp LV/HP big italic digits
DG_Y = kit.digits('y', 64, 211, pitch=0.62)  # camp money digits (お金数字), native tight pitch
COMMA_Y = spr(221, 'y_comma', h=22)
YEN, YW, YH = spr(210, 'yen', h=104)

SELF = {}  # page id -> list


def rule():
    return IMG(kit.shape('rect', W, 10), 0, 0, W, 10, color=RED)


def chrome(word, hero_art=None, status=True):
    """Sub-page frame: red rule, hero silhouette (dark red on black like the native backdrop),
    red title band carrying the native camp word art."""
    w = [rule()]
    if hero_art:
        src, hw, hh, x, y = hero_art
        w.append(IMG(src, x, y, hw, hh))
    w.append(IMG(kit.shape('banner', 600, 100), 0, 22, 600, 100, color=RED))
    src, ww, wh = word
    w.append(IMG(src, 34, 22 + (100 - wh) // 2, ww, wh, color=INK))
    if status:
        w.append(T(1208, 36, bind_text='status', scale=3, color=RED, align='right'))
        w.append(T(1208, 70, bind_text='live.date', scale=4, color=INK, align='right',
                   need_bind='date.ready'))
    # native-menu action in progress / last result (module pdrv.text)
    w.append(T(624, 100, bind_text='pdrv.text', scale=3, color=CYAN, w=584, need_bind='pdrv.show'))
    return w


# ---- v0.8 navigation through the module (lazy outputs): every START MENU page switch is module
# action ui_open(P); the module publishes ui.page = P and the page_binds below switch the page.
PAGE_ID = {'live': 1, 'm_skill': 2, 'm_item': 3, 'm_equip': 4, 'm_persona': 5, 'm_stats': 6,
           'm_confidant': 7, 'm_cfdetail': 8, 'm_request': 9, 'm_calendar': 10, 'm_equipc': 11,
           'field': 20, 'battle': 21, 'social': 22}
actions['ui.open'] = dict(kind='module', action='ui_open', argument='$payload')
for _n, _a, _g in [('item.use_sel', 'item_use_sel', 'item.sel.can_use'),
                   ('skill.use_sel', 'skill_use_sel', 'skill.sel.can_use'),
                   ('equip.pick', 'equip_pick', 'live.ready'),
                   ('equip.change_sel', 'equip_change_sel', 'equip.cand.can'),
                   ('persona.change_sel', 'persona_change_sel', 'persona.sel.can_change')]:
    actions[_n] = dict(kind='module', action=_a, argument='$payload', enabled_bind=_g)


def go(pid):
    """(action, payload) of a navigation tap to page pid."""
    return 'ui.open', str(PAGE_ID[pid])


def button(x, y, w, h, label, action, gate=None, scale=5, color=RED, ink=INK, payload=None):
    src = kit.shape('button', w, h)
    a = dict(on_tap=action)
    if payload is not None:
        a['payload'] = str(payload)
    if gate:
        a['need_bind'] = gate
    return [IMG(src, x, y, w, h, color=color, **a),
            T(x + w // 2, y + (h - cap_px(scale)) // 2, label, scale, ink, align='center', **a)]


BAR_Y = 972  # top of the bottom tab bar (build in the assemble step, every page but 'waiting')


SCROLLS = {}  # page id -> scroll regions


def page(pid, title, widgets):
    SELF[pid] = dict(id=pid, title=title, widgets=widgets)
    if pid in SCROLLS:
        SELF[pid]['scrolls'] = SCROLLS[pid]


def region(pid, rid, rect, count, row_h, show=None, reset=None, cols=1):
    """Drag-to-scroll viewport (scroll.md) on page pid; returns its id for the templates."""
    d = dict(id=rid, rect=list(rect), count_bind=count, row_h=row_h, bar='#FFE5191C',
             bar_track='#33FFFFFF', bar_w=6)
    if show:
        d['show_bind'] = show
    if reset:
        d['reset_bind'] = reset
    if cols > 1:
        d['cols'] = cols
    SCROLLS.setdefault(pid, []).append(d)
    return rid


def rp(widgets, count, dy, cap, scroll=None, dx=0):
    """Turn row-0 widgets into repeat templates ("{i}" = element index; scroll.md)."""
    for w in widgets:
        w.update(repeat=cap, repeat_bind=count)
        if dy:
            w['repeat_dy'] = dy
        if dx:
            w['repeat_dx'] = dx
        if scroll:
            w['scroll'] = scroll
    return widgets


def ikeep(k):
    """keep_min/keep_max that follow the element index: '{i}' / '{i}+k'."""
    return '{i}' if k == 0 else f'{{i}}+{k}'


def ipay(k):
    return '{i}' if k == 0 else f'{{i+{k}}}'


def pageact(name, pid, gate='live.ready'):
    actions[name] = dict(kind='page', page=pid, enabled_bind=gate)
    return name


def plate(x, y, w, h, color=WHITE, skew=8, **kw):
    return IMG(kit.shape('tile', w, h, skew=skew), x, y, w, h, color=color, **kw)


# ================================================================== HUB (replaces PARTY; id "live")
MENU = [  # (word sprite on black backing, page id, native left x at 1920)
    (233, 'm_skill', 805), (234, 'm_item', 885), (235, 'm_equip', 800), (236, 'm_persona', 720),
    (238, 'm_stats', 770), (240, 'm_confidant', 540), (237, 'm_request', 640),
    (452, 'm_calendar', 680)]
HELP = {'m_skill': 28, 'm_item': 29, 'm_equip': 30, 'm_persona': 31, 'm_stats': 32,
        'm_calendar': 34}
hub = [rule()]
# ui_a (#9): the eight commands are a 2 x 4 GRID of big tappable tiles (native command word art on
# tilted P5 plates: black face, white rim, red drop shadow), in the native order row by row.
hub.append(IMG(slab(W, H - 10, [(0, 0), (W, 0), (W, 380), (0, 700)], 'hub_slab2'), 0, 10, W, H - 10))
TOP, TW, TH = hero('P5_CAMPTOP_2D', 900, 'hub_hero_d', (70, 14, 18))
hub.append(IMG(TOP, W - TW + 60, 110, TW, TH))
hub.append(T(36, 30, bind_text='live.date', scale=5, color=INK, need_bind='date.ready'))
hub.append(T(1208, 30, bind_text='status', scale=3, color=RED, align='right'))
MN, MNW, MNH = spr(55, 'menu_word_s', h=44)
MA, MAW, MAH = spr(68, 'main_word_s', h=56)
HDR_Y = 86  # plates' top: clear space under the date / status line (y 30)
hub.append(plate(1224 - MNW - MAW - 48, HDR_Y, MNW + MAW + 40, 70, color=BLACK, skew=10))
hub.append(IMG(MN, 1224 - MNW - MAW - 28, HDR_Y + 8, MNW, MNH))
hub.append(IMG(MA, 1224 - MAW - 16, HDR_Y + 4, MAW, MAH))
GT_W, GT_H, GT_X, GT_DX, GT_DY = 584, 158, 24, 608, 172
# The grid sits centred between the header (money + MENU: MAIN plates end at HDR_Y + 70) and the
# bottom tab bar (its real top, BAR_Y): equal gap above and below.
HDR_BOTTOM, NAV_TOP = HDR_Y + 70, BAR_Y
GT_Y = HDR_BOTTOM + (NAV_TOP - HDR_BOTTOM - (3 * GT_DY + GT_H)) // 2
GT_TILT = [-2.5, 2.0, 1.8, -2.2, -2.0, 2.4, 2.2, -1.8]


def grid_tile(sid, name, tilt, face=(0, 0, 0, 255)):
    """One command tile, pre-composed from the native word sprite (4x supersampled polygons).
    face = plate colour (black; red for the native cursor highlight)."""
    s = 4
    w, h = GT_W - 24, GT_H - 24
    k = 22 * s
    para = lambda ox, oy, ww, hh: [(ox + k, oy), (ox + ww, oy), (ox + ww - k, oy + hh), (ox, oy + hh)]
    im = (Art.new((w + 16) * s, (h + 16) * s)
          .polygon(para(12 * s, 12 * s, w * s, h * s), RED)                   # drop shadow
          .polygon(para(0, 0, w * s, h * s), (255, 255, 255, 255))            # rim
          .polygon(para(7 * s, 6 * s, (w - 14) * s, (h - 12) * s), face)      # face
          .resize(w + 16, h + 16))
    word = Kit.fit(kit.load_sprite('camp00', sid), height=96)
    if word.width > w - 90:
        word = Kit.fit(word, width=w - 90)
    im = im.over(word, (w - word.width) // 2 + 4, (h - word.height) // 2 + 2)
    im = im.rotate(tilt)
    out = Art.new(GT_W, GT_H).over(im.crop(((im.width - GT_W) // 2, (im.height - GT_H) // 2,
                                             (im.width + GT_W) // 2, (im.height + GT_H) // 2)))
    return save(out, name, {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': sid,
                            'note': 'command word on a generated tilted plate', 'tilt': tilt,
                            'size': [GT_W, GT_H]})


for i, (sid, pid, nx) in enumerate(MENU):
    x, y = GT_X + (i % 2) * GT_DX, GT_Y + (i // 2) * GT_DY
    src = grid_tile(sid, f'hub_tile_{pid}', GT_TILT[i])
    pageact(f'page.{pid}', pid)  # kept for older references; the hub navigates via ui_open
    hub.append(IMG(src, x, y, GT_W, GT_H, on_tap='ui.open', payload=str(PAGE_ID[pid]),
                   need_bind='live.ready'))
    hub.append(IMG(src, x, y, GT_W, GT_H, color='#FF6A5A60', hide_bind='live.ready', keep_max=0))
    # native command cursor (module menu.cursor, 0 SKILL .. 7 CALENDAR) lights its tile red
    hsrc = grid_tile(sid, f'hub_tile_{pid}_hi', GT_TILT[i], face=rgba(RED))
    hub.append(IMG(hsrc, x, y, GT_W, GT_H, on_tap='ui.open', payload=str(PAGE_ID[pid]),
                   need_bind=both('live.ready', dv('ui.hub.native', cmp='eq', a='state.mode', b=5)),
                   hide_bind='menu.cursor', keep_min=i, keep_max=i))
if 'menu.cursor' not in m.setdefault('module_outputs', []):
    m['module_outputs'].append('menu.cursor')
# money: top-left in the header, on the same line as the MENU: MAIN plate (y HDR_Y..HDR_Y + 70)
YEN_S, YSW, YSH = spr(210, 'yen_s', h=66)
hub.append(plate(16, HDR_Y, 440, 70, color=BLACK, skew=10, need_bind='live.ready'))
hub.append(IMG(YEN_S, 30, HDR_Y + 2, YSW, YSH, need_bind='live.ready'))
hub += sprite_num('live.money', DG_Y, 30 + YSW + 350, HDR_Y + 3, 7, need='live.ready', comma=COMMA_Y)
page('live', 'MENU', hub)

# ================================================================== member column (shared)
# The native camp menus list a ROSTER, not the active party (module roster.* / eroster.*):
# SKILL and ITEM targets = 7E3970 (protagonist + joined members, 9 max), STATS and EQUIP =
# 7E3BB0 (same + Futaba last, 10 max). Row N here = native list row N (the drivers walk to it).
MC_X, MC_Y, MC_W, MC_H, MC_P = 24, 132, 400, 60, 64
HP_S, HPSW, HPSH = spr(261, 'hp_s', h=20)
SP_S, SPSW, SPSH = spr(262, 'sp_s', h=20)
LV_S, LVSW, LVSH = spr(337, 'lv_s', h=22)


def member_tabs(fname, lst='roster'):
    """Native member column: compact plates (face, LV, name, HP/SP, PARTY/NAVI tag), one repeat
    template per widget over `lst`.count; a tap selects the row (flag `fname` = row)."""
    act = flag(fname)
    g = f'{lst}.ready'
    p = f'{lst}.{{i}}.'
    x, y, w, h = MC_X, MC_Y, MC_W, MC_H
    sel = dict(hide_bind=f'@flag:{fname}', keep_min='{i}', keep_max='{i}')
    out = [plate(x, y, w, h, color=BLACK, skew=10, need_bind=g),
           plate(x - 5, y - 3, w + 10, h + 6, color=CYAN, skew=10, need_bind=g, **sel),
           plate(x, y, w, h, color=BLACK, skew=10, need_bind=g, **sel),
           dict(type='image', src=HEAD_S[1], rect=[x + 10, y - 2, 64, 64], bind=p + 'id',
                src_names=HEAD_S, color=INK, need_bind=g),
           IMG(LV_S, x + 84, y + 8, LVSW, LVSH, need_bind=g),
           V(x + 90 + LVSW, y + 4, p + 'level', 4, INK, need_bind=g),
           T(x + 170, y + 6, bind_text=p + 'name', scale=4, need_bind=g),
           IMG(HP_S, x + 84, y + 36, HPSW, HPSH, need_bind=g),
           V(x + 90 + HPSW, y + 33, p + 'hp', 4, HP_INK, need_bind=g),
           IMG(SP_S, x + 200, y + 36, SPSW, SPSH, need_bind=g),
           V(x + 206 + SPSW, y + 33, p + 'sp', 4, SP_INK, need_bind=g),
           T(x + w - 14, y + 8, 'PARTY', 3, RED, align='right', need_bind=g, hide_bind=p + 'party',
             keep_min=1, keep_max=1),
           T(x + w - 14, y + 8, 'LEADER', 3, CYAN, align='right', need_bind=g, hide_bind=p + 'party',
             keep_min=2)]
    if lst == 'eroster':
        out.append(T(x + w - 14, y + 8, 'NAVI', 3, CYAN, align='right', need_bind=g,
                     hide_bind=p + 'navi', keep_min=1))
    tap = kit.shape('rect', w, h, fill='#00000000')
    out.append(dict(type='image', src=tap, rect=[x, y, w, h], color='#00000000', on_tap=act,
                    payload='{i}', need_bind=g))
    return rp(out, f'{lst}.count', MC_P, 10)


# ui_a: roomy member list (bigger rows, drag-to-scroll viewport; scroll.md). Same flag / tap /
# payload semantics as member_tabs (flag `fname` = roster row), so page binds stay unchanged.
HP_L, HPLW, HPLH = spr(261, 'hp_l', h=24, color=HP_NAT)
SP_L, SPLW, SPLH = spr(262, 'sp_l', h=24, color=SP_NAT)
LV_L, LVLW, LVLH = spr(337, 'lv_l', h=26)
HEAD_L = heads('hl', 80)


def member_list(pid, fname, lst='roster', rect=(24, 132, 472, 640), row_h=84, pitch=92, hpsp=True):
    act = flag(fname)
    g = f'{lst}.ready'
    x, y, w, _ = rect
    rid = region(pid, f'mem_{fname}', rect, f'{lst}.count', pitch, show=g)
    p = f'{lst}.{{i}}.'
    h = row_h
    sel = dict(hide_bind=f'@flag:{fname}', keep_min='{i}', keep_max='{i}')
    ty = y + (10 if hpsp else (h - cap_px(5)) // 2)
    out = [plate(x, y, w - 10, h, color=BLACK, skew=10, need_bind=g),
           plate(x - 4, y - 3, w - 2, h + 6, color=CYAN, skew=10, need_bind=g, **sel),
           plate(x + 2, y + 2, w - 14, h - 4, color=BLACK, skew=10, need_bind=g, **sel),
           IMG(kit.shape('rect', 8, h - 8), x + 14, y + 4, 8, h - 8, color=RED, need_bind=g, **sel),
           dict(type='image', src=HEAD_L[1], rect=[x + 24, y + 2, 80, 80], bind=p + 'id',
                src_names=HEAD_L, color=INK, need_bind=g),
           T(x + 116, ty, bind_text=p + 'name', scale=5, need_bind=g),
           IMG(LV_L, x + w - 150, ty + 4, LVLW, LVLH, need_bind=g),
           V(x + w - 30, ty, p + 'level', 5, INK, align='right', need_bind=g)]
    if hpsp:
        out += [IMG(HP_L, x + 116, y + 52, HPLW, HPLH, need_bind=g),
                V(x + 124 + HPLW, y + 48, p + 'hp', 5, HP_NAT, need_bind=g),
                IMG(SP_L, x + 232, y + 52, SPLW, SPLH, need_bind=g),
                V(x + 240 + SPLW, y + 48, p + 'sp', 5, SP_NAT, need_bind=g)]
    tag = dict(need_bind=g, hide_bind=p + 'party')
    tx, tyy = x + w - 30, y + h - 26
    out += [T(tx, tyy, 'PARTY', 3, RED, align='right', keep_min=1, keep_max=1, **tag),
            T(tx, tyy, 'LEADER', 3, CYAN, align='right', keep_min=2, **tag)]
    if lst == 'eroster':
        out.append(T(tx, tyy, 'NAVI', 3, CYAN, align='right', need_bind=g, hide_bind=p + 'navi',
                     keep_min=1))
    tap = kit.shape('rect', w - 10, h, fill='#00000000')
    out.append(dict(type='image', src=tap, rect=[x, y, w - 10, h], color='#00000000', on_tap=act,
                    payload='{i}', need_bind=g))
    return rp(out, f'{lst}.count', pitch, 10, scroll=rid)


# ------------------------------------------------------------------ ICON.DDS cells (list icons)
# Contract (items.md / persona.md): `icon` = ICON.DDS cell (row*6+col), -1 = none. Shipped as
# ui/m/ic/<cell+1>.png and drawn by value with src_format + add=1, so -1 resolves to ic/0.png, which
# does not exist: nothing is drawn. Rows are gated on their list's count, so an icon of an
# unpublished row is never read.
ICW, ICH = 84, 28
NICON = art.ICON_COLS * art.ICON_ROWS
for n in range(NICON):
    if art.icon_nonempty(n):
        save(Art.icon(n).resize(ICW, ICH), f'ic/{n + 1}',
             {'source': 'EN/FONT/ICON.DDS', 'cell': n, 'file_index': 'cell + 1',
              'grid': '6 cols x 126x45 pitch, 122x41 content', 'size': [ICW, ICH]}, rid=f'i{n + 1}')
ICON_FMT = 'file:ui/m/ic/%d.png' if kit.mode == 'png' else 'module:p5r:i%d'


def icon(bind, x, y, **gate):
    return dict(type='image', src=ICON_FMT % 1, rect=[x, y, ICW, ICH], bind=bind, add=1,
                src_format=ICON_FMT, color=INK, **gate)


def rows_gate(need, count, idx):
    """Row idx of a list exists: the group gate plus count > idx (no per-row derived value)."""
    return dict(need_bind=need, hide_bind=count, keep_min=idx + 1)


def row_plate(x, y, w, h, need, count, idx, fname, key, act=None, sel=None, payload=None):
    """List row: dark plate; red while flag `fname` (or the module echo `sel`) == key. key encodes
    member/tab too, so a stale selection from another member/tab never lights a row here; the tap
    runs `act` with key as $payload."""
    out = [plate(x, y, w, h, color=TILE, skew=6, **rows_gate(need, count, idx)),
           plate(x, y, w, h, color=RED, skew=6, need_bind=need, hide_bind=sel or f'@flag:{fname}',
                 keep_min=key, keep_max=key)]
    if act:
        tap = kit.shape('rect', w, h, fill='#00000000')
        out.append(dict(type='image', src=tap, rect=[x, y, w, h], color='#00000000', on_tap=act,
                        payload=str(key if payload is None else payload), **rows_gate(need, count, idx)))
    return out


def target_buttons(action, gate, x=40, y=884, label='USE ON', allow_all=False):
    """Target row (payload = native SKILL/ITEM roster row; 9 = no target), under the help box."""
    out = [T(x, y + 26, label, 4, DIM, need_bind=gate)]
    bx = x + 150
    g = dict(need_bind=gate)
    tap = kit.shape('rect', 72, 76, fill='#00000000')
    out += rp([plate(bx, y, 72, 76, color=RED, skew=8, **g),
               dict(type='image', src=HEAD_S[1], rect=[bx + 4, y + 6, 64, 64], bind='roster.{i}.id',
                    src_names=HEAD_S, color=INK, **g),
               dict(type='image', src=tap, rect=[bx, y, 72, 76], color='#00000000', on_tap=action,
                    payload='{i}', **g)], 'roster.count', 0, 9, dx=80)
    if allow_all:
        ax = bx + 9 * 80 + 6
        out.append(plate(ax, y, 80, 76, color=RED, skew=8, need_bind=gate))
        out.append(T(ax + 40, y + 24, 'ALL', 4, INK, align='center', need_bind=gate))
        tap = kit.shape('rect', 80, 76, fill='#00000000')
        out.append(dict(type='image', src=tap, rect=[ax, y, 80, 76], color='#00000000', on_tap=action,
                        payload='9', need_bind=gate))
    return out


def selected(need, fname, key):
    return dict(need_bind=need, hide_bind=f'@flag:{fname}', keep_min=key, keep_max=key)


def desc_box(y, h=96):
    return [plate(24, y, 1192, h, color=BLACK, skew=10),
            IMG(kit.shape('rect', 1160, 3), 40, y + 6, 1160, 3, color=RED)]


def wrap(x, y, bind, w, lines, scale=4, color=INK, **kw):
    return dict(type='label', rect=[x, y, w, cap_px(scale) * lines + 40], text='', bind_text=bind,
                text_scale=scale, color=color, wrap_width=w, max_lines=lines, line_gap=6, **kw)


def pending(text, ready, x=620, y=540):
    """Shown only while the page's data group is not ready (module not publishing it yet)."""
    return T(x, y, text, 4, DIM, align='center', hide_bind=ready, keep_max=0)


# ================================================================== SKILL
# native: pick a member (left: the SKILL roster, reserves included), list "icon name .... cost"
# (right, drag to scroll), help of the highlighted skill, USE ON a roster member.
SK_P, SK_ROWS_MAX, SKR = 58, 64, 64   # row pitch; rows per member (module SkillRowsPublished)
sk = chrome(spr(742, 'w_skill', h=80), (*hero('P5_CAMPSKILL_2D01', 700, 'bg_skill', (70, 14, 18)),
                                        560, 250))
sk += desc_box(784, 92)
sk += member_list('m_skill', 'ui.skm', 'roster', rect=(24, 132, 472, 640))  # ui_a (#7)
# persona.md: a row tap calls module action skill_select (argument M*64+K), which publishes the
# native help text of that skill as skill.desc; the highlight follows the module's echo skill.sel.
sk_act = 'skill.select'
actions[sk_act] = dict(kind='module', action='skill_select', argument='$payload', enabled_bind='skill.ready')
for M in range(10):
    gm = both('skill.ready', eq('ui.skm', M))
    cnt = f'skill.{M}.count'
    rid = region('m_skill', f'sk{M}', [500, 132, 716, 640], cnt, SK_P, show=gm, reset='@flag:ui.skm')
    y = 132
    p = f'skill.{M}.{{i}}.'
    tap = kit.shape('rect', 716, 52, fill='#00000000')
    row = [plate(500, y, 716, 52, color=TILE, skew=6, need_bind=gm),
           plate(500, y, 716, 52, color=RED, skew=6, need_bind=gm, hide_bind='skill.sel',
                 keep_min=ikeep(M * SKR), keep_max=ikeep(M * SKR)),
           dict(type='image', src=tap, rect=[500, y, 716, 52], color='#00000000', on_tap=sk_act,
                payload=ipay(M * SKR), need_bind=gm),
           icon(p + 'icon', 520, y + 12, need_bind=gm),
           # native (E96A20, both camp draw sites): no SKILL row is ever dimmed, whatever the SP;
           # the confirm refuses what the member cannot pay (skill.M.K.usable -> USE ON gate)
           T(614, y + 13, bind_text=p + 'name', scale=5, need_bind=gm),
           plate(1040, y + 8, 160, 36, color=WHITE, skew=8, need_bind=gm),
           V(1136, y + 11, p + 'cost', 5, BLACK, align='right', need_bind=gm),
           dict(type='value', rect=[1188, y + 15, 0, 24], bind=p + 'cost_hp', names=['SP', 'HP'],
                text='', text_scale=4, color=BLACK, align='right', need_bind=gm)]
    sk += rp(row, cnt, SK_P, SK_ROWS_MAX, scroll=rid)
    sk.append(T(858, 300, 'No usable skills.', 5, DIM, align='center', need_bind=gm, hide_bind=cnt,
                keep_max=0))
sk.append(wrap(60, 800, 'skill.desc', 1120, 2, need_bind='skill.ready'))
# USE ON <member>: the selected row of the member shown (skill_use_sel T; module checks SP / field)
sk += target_buttons('skill.use_sel', both('skill.sel.can_use',
                                            dv('ui.sk.same', cmp='eq', a='@flag:ui.skm',
                                               b='skill.sel.member')))
sk.append(pending('Skill lists appear once the game reports them.', 'skill.ready', 828, 400))
page('m_skill', 'SKILL', sk)

# ================================================================== ITEM
# ui_b layout: the item list (tabs kept, drag to scroll) fills the left ~60%; the right column is
# the ITEM target roster (roster.*, native 7E3970 order = item_use_sel S) with camp portraits and
# HP/SP. Using an item, two ways (both end in the module's item_use_sel S -> native menu drive):
#  - tap to use: tap a row (module item_select, echo item.sel.key), then tap a member;
#  - drag and drop: the SELECTED row grows a grip on its icon; drag the grip onto a member.
# Why only the selected row drags: a drop runs ONE action with the dragged payload, and the module
# actions are item_use (needs item AND target in one argument) / item_use_sel S (target only, uses
# the stored selection). The member plate's drop action is item_use_sel S, so the dragged item must
# be the selected one; the grip exists only there. Every other touch on the list scrolls it
# (scroll.md: a draggable widget takes the gesture only where it is).
IT_TABS = 6
TAB_ART = [886, 887, 888, 889, 891, 892]  # art_notes.md (m_items): tab T label sprites
IT_L, IT_R = 24, 736          # list column x0 / x1 (left ~60%)
IT_W = IT_R - IT_L
TG_X, TG_W = 752, 464         # target column
it = chrome(spr(194, 'w_item', h=80), (*hero('P5_CAMPITEM_2D01', 700, 'bg_item', (70, 14, 18)),
                                       560, 250))
tab_act = 'item.tab'  # module item_tab (only the viewed tab's rows are published); echo item.view


def iv(t):
    return dv(f'ui.iv.{t}', cmp='eq', a='item.view', b=t)


NEW_B, NBW, NBH = spr(612, 'new_badge_s', h=24)
row_act = 'item.select'  # module item_select T*1000+N; echo item.sel.key = T*1000+N
# tabs: 2 rows x 3 columns (user request), native label art per tab
IT_TC, IT_TG, TBH = 3, 6, 46
TBW = (IT_W - (IT_TC - 1) * IT_TG) // IT_TC
for t in range(IT_TABS):
    x = IT_L + (t % IT_TC) * (TBW + IT_TG)
    ty = 134 + (t // IT_TC) * (TBH + IT_TG)
    g = dict(need_bind='item.ready', hide_bind='item.tab.count', keep_min=t + 1)
    it.append(plate(x, ty, TBW, TBH, color=TILE_HI, skew=8, **g))
    it.append(plate(x, ty, TBW, TBH, color=RED, skew=8, need_bind='item.ready', hide_bind='item.view',
                    keep_min=t, keep_max=t))
    src, tw, th = spr(TAB_ART[t], f'itab_s{t}', h=30, w=TBW - 24)
    it.append(IMG(src, x + (TBW - tw) // 2, ty + (TBH - th) // 2, tw, th, **g))
    tap = kit.shape('rect', TBW, TBH, fill='#00000000')
    it.append(dict(type='image', src=tap, rect=[x, ty, TBW, TBH], color='#00000000', on_tap=tab_act,
                   payload=str(t), **g))
IT_P, IT_CAP, IT_H = 62, 160, 56  # row pitch; module row cap per tab; row height
IT_Y, IT_VH = 246, 520            # list viewport (below the 2x3 tabs)
GRIP = kit.shape('tile', 112, IT_H, skew=6)
_gd = Art.new(10, 32)
for _x in (1, 6):
    for _y in (2, 13, 24):
        _gd = _gd.ellipse([_x, _y, _x + 3, _y + 3], (255, 255, 255, 255))
GRIP_DOTS = save(_gd, 'it_grip', {'source': 'generated (drag grip dots)'})
# the card that follows the finger while an item is dragged (drag_under_src: drawn unclipped)
DRAG_CARD = slab(300, 64, [(12, 0), (300, 0), (288, 64), (0, 64)], 'it_dragcard', fill=RED)
for t in range(IT_TABS):
    gt = both('item.ready', iv(t))
    cnt = f'item.tab.{t}.count'
    rid = region('m_item', f'items{t}', [IT_L, IT_Y, IT_W, IT_VH], cnt, IT_P, show=gt, reset='item.view')
    y = IT_Y
    p = f'item.tab.{t}.{{i}}.'
    tap = kit.shape('rect', IT_W, IT_H, fill='#00000000')
    selk = dict(hide_bind='item.sel.key', keep_min=ikeep(t * 1000), keep_max=ikeep(t * 1000))
    row = [plate(IT_L, y, IT_W, IT_H, color=TILE, skew=6, need_bind=gt),
           plate(IT_L, y, IT_W, IT_H, color=RED, skew=6, need_bind=gt, **selk),
           dict(type='image', src=tap, rect=[IT_L, y, IT_W, IT_H], color='#00000000', on_tap=row_act,
                payload=ipay(t * 1000), need_bind=gt),
           # grip behind the icon of the selected row (the drag handle)
           # (only when the native menu would accept the row: item.sel.can_use)
           IMG(GRIP, IT_L + 4, y, 112, IT_H, color=BLACK, need_bind=both(gt, 'item.sel.can_use'),
               **selk),
           IMG(GRIP_DOTS, IT_L + 8, y + 12, 10, 32, color=INK, need_bind=both(gt, 'item.sel.can_use'),
               **selk),
           # native row style (item.tab.T.N.grey, 7D92A0): dimmed icon, name and count plate for
           # items the camp menu cannot use (no field-use bit); every other row is drawn normally
           icon(p + 'icon', IT_L + 18, y + 14, need_bind=gt, hide_bind=p + 'grey', keep_max=0),
           icon(p + 'icon', IT_L + 18, y + 14, need_bind=gt, hide_bind=p + 'grey', keep_min=1)
           | dict(color=GREY_ICON),
           IMG(NEW_B, IT_L + 2, y - 4, NBW, NBH, need_bind=gt, hide_bind=p + 'new', keep_min=1),
           T(IT_L + 126, y + 15, bind_text=p + 'name', scale=5, w=IT_W - 270, need_bind=gt,
             hide_bind=p + 'grey', keep_max=0),
           T(IT_L + 126, y + 15, bind_text=p + 'name', scale=5, w=IT_W - 270, color=GREY_INK,
             need_bind=gt, hide_bind=p + 'grey', keep_min=1),
           plate(IT_R - 128, y + 8, 116, 40, color=WHITE, skew=8, need_bind=gt, hide_bind=p + 'grey',
                 keep_max=0),
           plate(IT_R - 128, y + 8, 116, 40, color=GREY_PLATE, skew=8, need_bind=gt,
                 hide_bind=p + 'grey', keep_min=1),
           T(IT_R - 112, y + 16, 'x', 4, BLACK, need_bind=gt),
           V(IT_R - 26, y + 12, p + 'qty', 5, BLACK, align='right', need_bind=gt),
           # the drag handle: only on the selected row (see above); payload = the row key
           dict(type='image', src=tap, rect=[IT_L, y, 116, IT_H], color='#00000000', draggable=True,
                drag_scale=1.0, payload=ipay(t * 1000), drag_under_src=DRAG_CARD,
                drag_under_rect=[-8, -4, 300, 64], need_bind=both(gt, 'item.sel.can_use'), **selk)]
    it += rp(row, cnt, IT_P, IT_CAP, scroll=rid)
    it.append(T(IT_L + IT_W // 2, 400, 'No items.', 5, DIM, align='center', need_bind=gt, hide_bind=cnt,
                keep_max=0))
# help of the selected item (native text box under the list)
it.append(plate(IT_L, 778, IT_W, 186, color=BLACK, skew=10))
it.append(IMG(kit.shape('rect', IT_W - 32, 3), IT_L + 16, 784, IT_W - 32, 3, color=RED))
it.append(T(IT_L + 24, 796, bind_text='item.sel.name', scale=5, color=RED, w=IT_W - 48,
            need_bind='item.sel.ready'))
it.append(wrap(IT_L + 24, 840, 'item.sel.desc', IT_W - 48, 3, need_bind='item.sel.ready'))
it.append(T(IT_L + 24, 820, 'Tap an item, then tap a member.', 4, DIM,
            hide_bind='item.sel.ready', keep_max=0))
# the native list refuses this row (buzzer): no USE / target plates (item.sel.can_use is 0)
it.append(T(IT_L + IT_W - 24, 796, "Can't use this here.", 4, GREY_INK, align='right',
            need_bind='item.sel.ready', hide_bind='item.sel.usable', keep_max=0))
# ---- target roster (right): face, name, LV, HP x/max, SP x/max; tap or drop = item_use_sel S
IT_TGP, IT_TGH = 80, 74
HEAD_I = heads('hi', 72)
it.append(plate(TG_X, 134, TG_W, 50, color=BLACK, skew=8, need_bind='roster.ready'))
it.append(T(TG_X + 24, 146, 'USE ON', 4, RED, need_bind='roster.ready'))
it.append(T(TG_X + TG_W - 20, 146, bind_text='item.sel.name', scale=4, w=300, align='right',
            need_bind='item.sel.can_use'))
for S in range(9):
    actions[f'item.drop.{S}'] = dict(kind='module', action='item_use_sel', argument=S,
                                     enabled_bind='item.sel.can_use')
actions['item.drop.9'] = dict(kind='module', action='item_use_sel', argument=9,
                              enabled_bind='item.sel.can_use')
g = 'roster.ready'
q = 'roster.{i}.'
y = 196
tgt_tap = kit.shape('rect', TG_W, IT_TGH, fill='#00000000')
it += rp([plate(TG_X, y, TG_W, IT_TGH, color=TILE, skew=8, need_bind=g),
          plate(TG_X, y, TG_W, IT_TGH, color='#FF3A1418', skew=8, need_bind=both(g, 'item.sel.can_use')),
          dict(type='image', src=HEAD_I[1], rect=[TG_X + 12, y + 3, 72, 72], bind=q + 'id',
               src_names=HEAD_I, color=INK, need_bind=g),
          T(TG_X + 96, y + 8, bind_text=q + 'name', scale=4, w=210, need_bind=g),
          IMG(LV_S, TG_X + 318, y + 12, LVSW, LVSH, need_bind=g),
          V(TG_X + 324 + LVSW, y + 8, q + 'level', 4, INK, need_bind=g),
          T(TG_X + TG_W - 18, y + 48, 'PARTY', 3, RED, align='right', need_bind=g, hide_bind=q + 'party',
            keep_min=1, keep_max=1),
          T(TG_X + TG_W - 18, y + 48, 'LEADER', 3, CYAN, align='right', need_bind=g, hide_bind=q + 'party',
            keep_min=2),
          IMG(HP_S, TG_X + 96, y + 46, HPSW, HPSH, need_bind=g),
          V(TG_X + 102 + HPSW, y + 40, q + 'hp', 5, HP_INK, need_bind=g),
          IMG(SP_S, TG_X + 250, y + 46, SPSW, SPSH, need_bind=g),
          V(TG_X + 256 + SPSW, y + 40, q + 'sp', 5, SP_INK, need_bind=g),
          # tap = use the selected item on this member; drop = the same with the dragged (selected) item
          dict(type='image', src=tgt_tap, rect=[TG_X, y, TG_W, IT_TGH], color='#00000000',
               on_tap='item.use_sel', payload='{i}', drop_action='item.drop.{i}',
               highlight_color='#80FFFFFF', need_bind=g)], 'roster.count', IT_TGP, 9)
# no-target items (S = 9, e.g. Goho-M): one ALL / USE plate under the roster
ay = 196 + 9 * IT_TGP + 2
it.append(plate(TG_X, ay, TG_W, 46, color=RED, skew=8, need_bind='item.sel.can_use'))
it.append(T(TG_X + TG_W // 2, ay + 10, 'USE (NO TARGET / ALL)', 4, INK, align='center',
            need_bind='item.sel.can_use'))
it.append(dict(type='image', src=kit.shape('rect', TG_W, 46, fill='#00000000'), rect=[TG_X, ay, TG_W, 46],
               color='#00000000', on_tap='item.use_sel', payload='9', drop_action='item.drop.9',
               highlight_color='#80FFFFFF', need_bind='item.sel.can_use'))
it.append(pending('The inventory appears once the game reports it.', 'item.ready', IT_L + IT_W // 2, 480))
page('m_item', 'ITEM', it)

# ================================================================== EQUIP
# native: member (left), five slots WEAPON*MELEE / WEAPON*RANGED / PROTECTOR / ACCESSORY / OUTFIT
EQ_SLOTS = [(0, [75, 70]), (4, [75, 71]), (1, [72]), (2, [79]), (3, [80])]  # (items.md slot S, words)
eq_ = chrome(spr(83, 'w_equip', h=80), (*hero('P5_CAMPEQUIP_2D01', 640, 'bg_equip', (70, 14, 18)),
                                        380, 300))
eq_ += member_list('m_equip', 'ui.eqm', 'eroster', rect=(24, 132, 472, 640))  # ui_a (#7)
eq_ += desc_box(784, 170)
STAR, STW, STH = spr(482, 'star_w', h=26)
AT_W = {k: spr(s, f'eqst{s}', h=30) for k, s in (('atk', 98), ('acc', 99), ('def', 100), ('eva', 101))}
eqs_act = 'equip.pick'  # module equip_pick M*10+R: candidate list + candidate page
for si, (S, labels) in enumerate(EQ_SLOTS):
    y = 134 + si * 128
    eq_.append(plate(500, y, 460, 44, color=BLACK, skew=8))
    x = 516
    for k, lid in enumerate(labels):
        src, lw, lh = spr(lid, f'eqw{lid}', h=34)
        if k:
            eq_.append(IMG(STAR, x, y + 9, STW, STH))
            x += STW + 8
        eq_.append(IMG(src, x, y + 5, lw, lh))
        x += lw + 8
    eq_.append(plate(500, y + 48, 716, 64, color=WHITE, skew=8, need_bind='equip.ready'))
    tap = kit.shape('rect', 716, 112, fill='#00000000')
    for M in range(10):
        eq_.append(IMG(kit.shape('rect', 8, 64), 486, y + 48, 8, 64, color=RED,
                       need_bind='equip.ready', hide_bind='equip.sel', keep_min=M * 10 + si,
                       keep_max=M * 10 + si))
        eq_.append(dict(type='image', src=tap, rect=[500, y, 716, 112], color='#00000000',
                        on_tap=eqs_act, payload=str(M * 10 + si),
                        need_bind=both('equip.ready', 'pdrv.idle', eq('ui.eqm', M))))
    pair = ('atk', 'acc') if S in (0, 4) else (('def', 'eva') if S == 1 else ())
    for M in range(10):
        g = both('equip.ready', eq('ui.eqm', M))
        p = f'equip.{M}.{S}.'
        eq_.append(icon(p + 'icon', 522, y + 66, need_bind=g))
        eq_.append(T(618, y + 64, bind_text=p + 'name', scale=5, color=BLACK, need_bind=g))
        for j, key in enumerate(pair):
            src, lw, lh = AT_W[key]
            has = dict(need_bind=g, hide_bind=p + ('has_atk' if key in ('atk', 'acc') else 'has_def'),
                       keep_min=1)
            eq_.append(IMG(src, 976 + j * 120, y + 5, lw, lh, **has))
            eq_.append(V(1090 + j * 120, y + 8, p + key, 4, INK, align='right', **has))
        eq_.append(wrap(60, 806, p + 'desc', 1120, 3, need_bind=g, hide_bind='equip.sel',
                        keep_min=M * 10 + si, keep_max=M * 10 + si))
eq_.append(pending('Equipment appears once the game reports it.', 'equip.ready', 620, 870))
eq_.append(T(620, 880, 'Tap a slot to change it.', 4, DIM, align='center', need_bind='equip.ready',
             hide_bind='equip.sel', keep_max=-1))
page('m_equip', 'EQUIP', eq_)

# ================================================================== EQUIP candidates (native list)
EC_P, EC_CAP = 64, 40
ec = chrome(spr(83, 'w_equip', h=80))
EC = 'equip.cand.ready'
ec.append(T(24, 140, 'Choose equipment (tap to equip through the game menu)', 4, DIM, need_bind=EC))
rid = region('m_equipc', 'cand', [24, 186, 1192, 774], 'equip.cand.count', EC_P, show=EC,
             reset='equip.sel')
y = 186
q = 'equip.cand.{i}.'
tap = kit.shape('rect', 1192, 58, fill='#00000000')
row = [plate(24, y, 1192, 58, color=TILE, skew=6, need_bind=EC),
       plate(24, y, 1192, 58, color=DARKRED, skew=6, need_bind=EC, hide_bind=q + 'equipped', keep_min=1),
       icon(q + 'icon', 44, y + 15, need_bind=EC),
       T(144, y + 16, bind_text=q + 'name', scale=5, need_bind=EC),
       T(620, y + 20, 'EQUIPPED', 4, CYAN, need_bind=EC, hide_bind=q + 'equipped', keep_min=1)]
for j, key in enumerate(('atk', 'acc')):
    has = dict(need_bind=EC, hide_bind=q + 'has_atk', keep_min=1)
    src, lw, lh = AT_W[key]
    row += [IMG(src, 760 + j * 160, y + 14, lw, lh, **has),
            V(900 + j * 160, y + 16, q + key, 4, INK, align='right', **has)]
for j, key in enumerate(('def', 'eva')):
    has = dict(need_bind=EC, hide_bind=q + 'has_def', keep_min=1)
    src, lw, lh = AT_W[key]
    row += [IMG(src, 760 + j * 160, y + 14, lw, lh, **has),
            V(900 + j * 160, y + 16, q + key, 4, INK, align='right', **has)]
row += [T(1110, y + 20, 'x', 4, DIM, need_bind=EC), V(1196, y + 16, q + 'qty', 5, INK, align='right', need_bind=EC),
        dict(type='image', src=tap, rect=[24, y, 1192, 58], color='#00000000', on_tap='equip.change_sel',
             payload='{i}', need_bind=both('equip.cand.can', EC), hide_bind=q + 'can', keep_min=1)]
ec += rp(row, 'equip.cand.count', EC_P, EC_CAP, scroll=rid)
ec.append(T(620, 540, 'Nothing else can be equipped here.', 4, DIM, align='center', need_bind=EC,
            hide_bind='equip.cand.count', keep_max=1))
ec.append(pending('Loading the candidate list...', EC))
page('m_equipc', 'EQUIP', ec)

# ================================================================== PERSONA
# native: list "arcana  LV  name" (current persona marked), detail of the highlighted one.
PS_ROWS = 12
ARC_LIST = {a: 379 + a for a in range(1, 22)}
ARC_LIST.update({24: 403, 29: 868, 30: 880, 31: 868})
ah = 40
arc_cells = {a: Kit.fit(kit.load_sprite('camp00', s), height=ah) for a, s in ARC_LIST.items()}
arc_w = max(im.width for im in arc_cells.values())
arc_srcs = [''] * 32
for a, im in arc_cells.items():
    arc_srcs[a] = save(Kit.pad(im, arc_w, ah, 'right'), f'arcl{a}',
                       {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': ARC_LIST[a],
                        'note': 'list arcana label (リスト_アルカナ名)', 'size': [arc_w, ah]})
ps = chrome(spr(685, 'w_persona', h=80))
ps_act = 'persona.select'  # module persona_select row; echo persona.sel
PR = 'persona.ready'
PS_P, PS_H = 66, 60  # ui_a (#7): taller rows, larger type
ahl = 54  # larger list arcana labels for the roomier rows (same sprites as arc_srcs)
arc_cells_l = {a: Kit.fit(kit.load_sprite('camp00', sid), height=ahl) for a, sid in ARC_LIST.items()}
arc_wl = max(im.width for im in arc_cells_l.values())
arc_srcs_l = [''] * 32
for a, im in arc_cells_l.items():
    arc_srcs_l[a] = save(Kit.pad(im, arc_wl, ahl, 'right'), f'arcll{a}',
                         {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': ARC_LIST[a],
                          'note': 'list arcana label, large', 'size': [arc_wl, ahl]})
rid = region('m_persona', 'plist', [24, 132, 578, 736], 'persona.count', PS_P, show=PR)
y = 132
p = 'persona.{i}.'
tap = kit.shape('rect', 576, PS_H, fill='#00000000')
ps += rp([plate(24, y, 576, PS_H, color=TILE, skew=6, need_bind=PR),
          plate(24, y, 576, PS_H, color=RED, skew=6, need_bind=PR, hide_bind='persona.sel', keep_min='{i}',
                keep_max='{i}'),
          dict(type='image', src=tap, rect=[24, y, 576, PS_H], color='#00000000', on_tap=ps_act,
               payload='{i}', need_bind=PR),
          dict(type='image', src=arc_srcs_l[1], rect=[32, y + 3, arc_wl, ahl], bind=p + 'arcana',
               src_names=arc_srcs_l, color=INK, need_bind=PR),
          V(44 + arc_wl + 52, y + 12, p + 'level', 6, INK, align='right', need_bind=PR),
          T(44 + arc_wl + 64, y + 18, bind_text=p + 'name', scale=5, need_bind=PR),  # 'Izanagi-no-Okami Picaro' fits
          # current persona: cyan edge inside the viewport (a mark left of the rect is clipped)
          IMG(kit.shape('rect', 8, PS_H), 24, y, 8, PS_H, color=CYAN, need_bind=PR,
              hide_bind='persona.current', keep_min='{i}', keep_max='{i}')],
         'persona.count', PS_P, PS_ROWS, scroll=rid)
# affinity header: the coloured ICON.DDS element cells the native persona status screen draws, in its
# order phys, gun, fire, ice, elec, wind, psy, nuke, bless, curse (matched against the native capture
# native_04_persona_status_kodama.png)
# Element icons: the full camp status icons (P5CAMP_00SPD 319..328 = phys gun fire ice elec wind psy
# nuke bless curse, the same set the Analyze page uses). The ICON.DDS cells used before are cut to a
# 122x41 plate by design, which read as cropped art at this size.
AFW, AFH = 104, 64
AFF_IC = []
# tinted with each element's own colour, sampled from the game's ICON.DDS cell of that element
AFF_CELLS = [19, 20, 21, 22, 24, 23, 28, 27, 25, 26]
for e, sid in enumerate(range(319, 329)):
    _px = [q for q in art.icon_cell(AFF_CELLS[e]).getdata() if q[3] > 200 and max(q[:3]) > 90]
    _col = '#FF%02X%02X%02X' % tuple(sorted(c[i] for c in _px)[len(_px) // 2] for i in range(3))
    _src, _w, _h = kit.sprite('camp00', sid, f'aff{e}', height=AFH - 4, box=(AFW, AFH), color=_col)
    AFF_IC.append(_src)
BACKS, BKW, BKH = kit.sprite_set('camp00', [665, 666, 667, 668, 669, 670], 'pbk', height=48)
STAMPS, STW2, STH2 = kit.sprite_set('camp00', [329, 330, 331, 332, 333, 485], 'pst', height=40)
backing, stamps = [''] + BACKS, [''] + STAMPS
stl = [spr(s, f'pstat{s}', h=34) for s in (340, 341, 342, 343, 344)]
NEXTL = spr(336, 'nextlevel_s', h=30)
ps.append(plate(608, 132, 608, 820, color=BLACK, skew=14, need_bind=PR))
# One detail panel (perf 2026-09-25): the module republishes the persona.sel row as persona.det.*
# (persona.det.shown = a stock row 0..11 is selected), instead of 12 panels gated per row.
for _ in range(1):
    g = both(PR, 'persona.det.shown')
    p = 'persona.det.'
    ps.append(dict(type='image', src=arc_srcs[1], rect=[624, 152, arc_w, ah], bind=p + 'arcana',
                   src_names=arc_srcs, color=RED, need_bind=g))
    ps.append(T(640 + arc_w, 150, bind_text=p + 'name', scale=7, need_bind=g))
    ps.append(IMG(LV_T, 624, 214, LVW, LVH, need_bind=g))
    ps.append(V(624 + LVW + 12, 208, p + 'level', 6, INK, need_bind=g))
    ps.append(IMG(NEXTL[0], 820, 214, NEXTL[1], NEXTL[2], need_bind=g))
    ps.append(V(1196, 208, p + 'next', 5, INK, align='right', need_bind=g))
    for k, (src, lw, lh) in enumerate(stl):
        y = 266 + k * 48
        key = p + ('st', 'ma', 'en', 'ag', 'lu')[k]
        ps.append(IMG(src, 624, y, lw, lh, need_bind=g))
        ps.append(V(740, y + 2, key, 5, INK, align='right', need_bind=g))
        ps.append(dict(type='bar', rect=[756, y + 10, 440, 16], bind=key, max_const=99, bg='#FF3A3035',
                       color=RED, need_bind=g))
    for e in range(10):
        x, y = 624 + (e % 5) * 116, 508 + (e // 5) * 104
        b = p + f'aff.{e}'
        ps.append(IMG(AFF_IC[e], x, y, AFW, AFH, need_bind=g))
        ps.append(dict(type='image', src=backing[1], rect=[x + (AFW - BKW) // 2, y + 56, BKW, BKH], bind=b,
                       src_names=backing, color=RED, need_bind=g, hide_bind=b, keep_min=1, keep_max=6))
        ps.append(dict(type='image', src=stamps[1], rect=[x + (AFW - STW2) // 2, y + 60, STW2, STH2], bind=b,
                       src_names=stamps, color=INK, need_bind=g, hide_bind=b, keep_min=1, keep_max=6))
    for k in range(8):
        x, y = 620 + (k % 2) * 298, 716 + (k // 2) * 58
        q = p + f'skill.{k}.'
        sg = dict(need_bind=g, hide_bind=p + 'skill_count', keep_min=k + 1)
        ps.append(icon(q + 'icon', x, y + 12, **sg))
        ps.append(T(x + ICW + 8, y + 14, bind_text=q + 'name', scale=4, w=292 - ICW - 8, **sg))
ps.append(pending('Personas appear once the game reports them.', PR))
ps.append(T(908, 540, 'Tap a persona for details.', 4, DIM, align='center', need_bind=PR,
            hide_bind='persona.sel', keep_max=-1))
# EQUIP the highlighted persona (native Change Persona drive); in the page, under the list
ps += button(24, 884, 576, 72, 'EQUIP PERSONA', 'persona.change_sel', 'persona.sel.can_change', scale=5,
             payload=0)
page('m_persona', 'PERSONA', ps)

# ================================================================== STATS
# native: the STATS roster (7E3BB0: joined members + Futaba NAVI last) on the left, the detail of
# the highlighted member on the right: LV / NEXT, HP x/max, SP x/max, persona, attack/defense.
st = chrome(spr(737, 'w_stats', h=80), (*hero('P5_CAMPPARTY_2D', 600, 'bg_stats', (70, 14, 18)),
                                        600, 380))
# ui_a (#6): roomy, drag-scrollable roster (10 rows: eroster incl. Futaba NAVI) on the left
st += member_list('m_stats', 'ui.pm', 'eroster', rect=(24, 132, 500, 740), row_h=86, pitch=94)
SX = 540  # detail panel
st.append(plate(SX, 132, 1216 - SX, 828, color=BLACK, skew=14, need_bind='pstat.ready'))
NL, NLW, NLH = spr(336, 'nextlevel', h=36)
ATKS = [spr(591, 'melee_atk', h=36), spr(592, 'ranged_atk', h=36), spr(100, 'defense_w', h=36)]
HP_NT, HNW, HNH = spr(588, 'hp_nat', h=34, color=HP_NAT)
SP_NT, SNW, SNH = spr(589, 'sp_nat', h=34, color=SP_NAT)
# Baton Pass rank (sprites 904 "Baton Pass", 905 RANK, 906..908 digits 1..3, 909 MAX) and Down Shots
# (1052 "Down Shots", digits 1053 = 0, 906..908 = 1..3), like the native STATS detail page.
BT_W, BTW, BTH = spr(904, 'baton_w', h=44)
BT_R, BRW, BRH = spr(905, 'baton_rank', h=32)
BT_D, BDW, BDH = kit.sprite_set('camp00', [1053, 906, 907, 908], 'bdig', height=74)
BT_M, BMW, BMH = spr(909, 'baton_max', h=44)
DS_W, DSW, DSH = spr(1052, 'downshot_w', h=44)
DS_S, DSSW, DSSH = kit.sprite_set('camp00', [1053, 906, 907, 908], 'dsig', height=44)
for M in range(10):
    g = both('pstat.ready', eq('ui.pm', M))
    p = f'pstat.{M}.'
    x0 = SX + 30
    st.append(T(x0, 152, bind_text=f'eroster.{M}.name', scale=7, need_bind=g))
    st.append(IMG(LV_T, x0, 230, LVW, LVH, need_bind=g))
    st += sprite_num(p + 'level', DG_B, x0 + LVW + 110, 214, 2, need=g)
    st.append(IMG(NL, x0 + 250, 228, NLW, NLH, need_bind=g))
    st.append(V(1190, 226, p + 'next', 6, INK, align='right', need_bind=g))
    for yy, stat, ink, bg, tag in ((296, 'hp', HP_NAT, '#FF1F3F3A', (HP_NT, HNW, HNH)),
                                   (380, 'sp', SP_NAT, '#FF3F1F3A', (SP_NT, SNW, SNH))):
        st.append(IMG(tag[0], x0, yy + 4, tag[1], tag[2], need_bind=g))
        st.append(dict(type='value', rect=[x0 + tag[1] + 16, yy, 0, 40], bind=p + stat,
                       max_bind=p + stat + '_max', max_sep=' / ', text='', text_scale=6, color=ink,
                       need_bind=g))
        st.append(dict(type='bar', rect=[x0, yy + 54, 1190 - x0, 12], bind=p + stat, max_bind=p + stat + '_max',
                       bg=bg, color=ink, need_bind=g))
    pg = dict(need_bind=g, hide_bind=p + 'persona_level', keep_min=1)
    st.append(dict(type='image', src=arc_srcs[1], rect=[x0, 464, arc_w, ah], bind=p + 'persona_arcana',
                   src_names=arc_srcs, color=CYAN, **pg))
    st.append(IMG(LV_T, x0 + arc_w + 16, 468, LVW, LVH, **pg))
    st.append(V(x0 + arc_w + LVW + 22, 462, p + 'persona_level', 6, CYAN, **pg))
    st.append(T(x0 + arc_w + LVW + 116, 466, bind_text=p + 'persona_name', scale=6, color=CYAN, **pg))
    for k, ((src, lw, lh), key) in enumerate(zip(ATKS, ['melee', 'ranged', 'defense'])):
        yy = 528 + k * 50
        kg = dict(need_bind=g, hide_bind=p + key, keep_min=1)
        st.append(IMG(src, x0, yy, lw, lh, **kg))
        st.append(V(x0 + 330, yy + 2, p + key, 6, INK, align='right', **kg))
        if key == 'ranged':
            st.append(dict(type='value', rect=[x0 + 348, yy + 6, 0, 30], bind=p + 'rounds', text='x',
                           text_scale=5, color=DIM, need_bind=g, hide_bind=p + 'rounds', keep_min=1))
    # native STATS detail: TOTAL EXP (protagonist) / persona EXP (allies) = pstat.M.exp
    st.append(IMG(kit.shape('rect', 1190 - x0, 3), x0, 690, 1190 - x0, 3, color=RED, need_bind=g))
    st.append(T(x0, 710, 'TOTAL EXP', 5, RED, need_bind=g, hide_bind=f'eroster.{M}.id', keep_max=1))
    st.append(T(x0, 710, 'EXP', 5, RED, need_bind=g, hide_bind=f'eroster.{M}.id', keep_min=2))
    st.append(V(1190, 704, p + 'exp', 7, INK, align='right', need_bind=g))
    # Baton Pass rank: pstat.M.baton (1..3, 3 = MAX); hidden while 0 / not published
    bg_ = dict(need_bind=g, hide_bind=p + 'baton', keep_min=1)
    st.append(plate(x0 - 10, 780, 330, 164, color=WHITE, skew=16, **bg_))
    st.append(plate(x0 - 4, 786, 318, 152, color=BLACK, skew=16, **bg_))
    st.append(IMG(BT_W, x0 + 20, 796, BTW, BTH, **bg_))
    st.append(IMG(BT_R, x0 + 20, 880, BRW, BRH, **bg_))
    st.append(dict(type='image', src=BT_D[1], rect=[x0 + 30 + BRW, 850, BDW, BDH], bind=p + 'baton',
                   src_names=BT_D, color=INK, **bg_))
    st.append(IMG(BT_M, x0 + 40 + BRW + BDW, 870, BMW, BMH, need_bind=g, hide_bind=p + 'baton_max', keep_min=1))
    # Down Shots: pstat.M.downshot / downshot_max (native "Down Shots 3/3"); hidden while max is 0
    dg_ = dict(need_bind=g, hide_bind=p + 'downshot_max', keep_min=1)
    dx = x0 + 350
    st.append(plate(dx, 800, 1200 - dx, 124, color=BLACK, skew=16, **dg_))
    st.append(IMG(kit.shape('rect', 1200 - dx - 40, 3), dx + 20, 810, 1200 - dx - 40, 3, color=RED, **dg_))
    st.append(IMG(DS_W, dx + 24, 840, DSW, DSH, **dg_))
    st.append(dict(type='image', src=DS_S[1], rect=[dx + 60 + DSW, 834, DSSW, DSSH], bind=p + 'downshot',
                   src_names=DS_S, color=INK, **dg_))
    st.append(T(dx + 70 + DSW + DSSW, 852, '/', 5, DIM, **dg_))
    st.append(V(dx + 98 + DSW + DSSW, 852, p + 'downshot_max', 5, DIM, **dg_))
# Native STATS list footer: party Technical RANK 1..4 (4 = MAX) + its red help line (stats_extra.md)
tg = dict(need_bind='pstat.ready', hide_bind='pstat.technical', keep_min=1)
st.append(T(560, 754, 'TECHNICAL RANK', 4, RED, **tg))
st.append(V(800, 748, 'pstat.technical', 5, INK, **tg))
st.append(T(836, 754, 'MAX', 4, RED, need_bind='pstat.ready', hide_bind='pstat.technical_max', keep_min=1))
st.append(dict(type='label', rect=[920, 758, 280, 24], text='', bind_text='pstat.technical_help', text_scale=3,
               color=RED, wrap_width=300, max_lines=1, **tg))
st.append(pending('Stats appear once the game reports them.', 'pstat.ready', 878, 400))
# the old SOCIAL page (social stats + confidant cards) stays reachable from here, in the page
st += button(24, 884, 490, 72, 'SOCIAL STATS', 'ui.open', 'live.ready', scale=5, payload=PAGE_ID['social'])
page('m_stats', 'STATS', st)

# ================================================================== CONFIDANT art
chara = art.cmm_chara_ids()
chara[12] = 18  # Strength: cmmFormat row+0x2A = 19 is a 4x4 dummy (BASE); the sheet is C_CHARA_18
stems = sorted(p.stem.lower() for p in art.CHARATEX.glob('C_CHARA_*.DDS'))
BUST_W, BUST_H, NAME_W, NAME_H = 600, 600, 440, 150
thumbs, busts, names = {}, {}, {}
_chara = {}
for stem in stems:
    n_img, b_img, bands = _chara[stem] = art.chara_parts_art(stem)
    prov = {'source': f'EN/CAMP/CHARATEX/{stem.upper()}.DDS', **bands}
    bi = b_img.fit(h=BUST_H)
    if bi.width > BUST_W:
        bi = b_img.fit(w=BUST_W)
    busts[stem] = save(Kit.pad(bi, BUST_W, BUST_H, 'left'), f'cf/{stem}_bust',
                       {**prov, 'part': 'bust', 'size': [BUST_W, BUST_H]})
    ni = n_img.fit(w=NAME_W, h=NAME_H)
    names[stem] = save(Kit.pad(ni, NAME_W, NAME_H, 'left'), f'cf/{stem}_name',
                       {**prov, 'part': 'name art', 'size': [NAME_W, NAME_H]})
    face = b_img.crop((0, 0, b_img.width, min(b_img.height, b_img.width)))
    thumbs[stem] = save(face.fit(w=84).crop((0, 0, 84, 84)), f'cf/{stem}_thumb',
                        {**prov, 'part': 'bust, top square', 'size': [84, 84]})
m.setdefault('sprite_map', {})
for stem in stems:
    m['sprite_map'][stem] = busts[stem]


def by_id(d):
    return ['' if not chara.get(i) else d.get(f'c_chara_{chara[i]:02d}', '') for i in range(max(chara) + 1)]


THUMB_BY_ID, BUST_BY_ID, NAME_BY_ID = by_id(thumbs), by_id(busts), by_id(names)
CW, CH = 105, 210
cards = [''] * 32
for a in range(1, 32):
    f = art.CARDTEX / f'C_CARD{a - 1:02X}.DDS'
    if f.exists():
        cards[a] = save(Art.dds(f'EN/CAMP/CARDTEX/{f.name}').resize(CW, CH), f'cf/card{a}',
                        {'source': f'EN/CAMP/CARDTEX/{f.name}', 'arcana': a, 'size': [CW, CH]})
ARC_BIG = {a: 841 + a for a in range(1, 22)}
ARC_BIG.update({24: 865, 29: 867, 31: 867, 30: 879})
big_h = 70
big_imgs = {a: Kit.fit(kit.load_sprite('camp00', s), height=big_h) for a, s in ARC_BIG.items()}


def rimmed(im, r=3):
    """The detail plate is white: the plain-white labels (Faith / Councillor sprites) get the black
    rim the other big arcana names already carry, so they stay readable."""
    b = im.im.tobytes()
    if any(b[i + 3] > 128 and max(b[i], b[i + 1], b[i + 2]) < 90 for i in range(0, len(b), 4)):
        return im
    return im.rim(r)


big_imgs = {a: rimmed(im) for a, im in big_imgs.items()}
bw = max(i.width for i in big_imgs.values())
arc_big = [''] * 32
for a, im in big_imgs.items():
    arc_big[a] = save(Kit.pad(im, bw, big_h, 'left'), f'arcb{a}',
                      {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': ARC_BIG[a],
                       'note': 'big arcana name (white, black rim)', 'size': [bw, big_h]})
RANKD = digit_set('rk', 274, 56)

# ================================================================== CONFIDANT (one page: list + detail)
# ui_b: left = drag-scrollable confidant list (face, arcana label art, person name, rank), right =
# the selected confidant's detail (m_social confidant.sel.*): name bar, tarot card, ARCANA / RANK /
# stars, rank story, drag-scrollable abilities ("function" list, NEXT entries marked) and the help
# text of the highlighted ability. A row tap = module confidant_select (echo confidant.sel.index).
cf = chrome(spr(731, 'w_confidant', h=80), (*hero('P5_CAMPCOOP_2D02', 700, 'bg_coop', (70, 14, 18)),
                                            700, 250))
RANK_W, RWW, RWH = spr(476, 'rank_w', h=26)
MAXW, MXW, MXH = spr(478, 'rank_max', h=40)
RANKS = digit_set('rks', 274, 46)
sel_act = 'cf.select'
actions[sel_act] = dict(kind='module', action='confidant_select', argument='$payload',
                        enabled_bind='confidant.ready')
pageact('page.m_cfdetail', 'm_cfdetail', 'confidant.ready')
# list faces (70 px, top square of the bust) and header heads, cut from the same sheets
face70, head_s = {}, {}
HEAD_W, HEAD_H = 170, 200
for stem in stems:
    _, b_img, bands = _chara[stem]
    prov = {'source': f'EN/CAMP/CHARATEX/{stem.upper()}.DDS', **bands}
    face = b_img.crop((0, 0, b_img.width, min(b_img.height, b_img.width)))
    face70[stem] = save(face.fit(w=70).crop((0, 0, 70, 70)), f'cf/{stem}_f70',
                        {**prov, 'part': 'bust, top square', 'size': [70, 70]})
    hd = b_img.crop((0, 0, b_img.width, min(b_img.height, round(b_img.width * HEAD_H / HEAD_W))))
    hd = hd.fit(w=HEAD_W)
    head_s[stem] = save(Kit.pad(hd.crop((0, 0, HEAD_W, min(HEAD_H, hd.height))), HEAD_W, HEAD_H, 'left'),
                        f'cf/{stem}_head', {**prov, 'part': 'bust, head crop', 'size': [HEAD_W, HEAD_H]})
F70_BY_ID, HEAD_BY_ID = by_id(face70), by_id(head_s)
ARC_LH = 36  # list arcana label art, left-aligned (the persona list's set is right-aligned)
_arcl = {a: Kit.fit(kit.load_sprite('camp00', s_), height=ARC_LH) for a, s_ in ARC_LIST.items()}
ARC_LW = max(im.width for im in _arcl.values())
arc_left = [''] * 32
for a, im in _arcl.items():
    arc_left[a] = save(Kit.pad(im, ARC_LW, ARC_LH, 'left'), f'arcll{a}',
                       {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': ARC_LIST[a],
                        'note': 'list arcana label, left-aligned', 'size': [ARC_LW, ARC_LH]})
CL_X, CL_W, CL_Y, CL_VH, CL_P, CL_H = 24, 532, 132, 832, 80, 74
rid = region('m_confidant', 'cfl', [CL_X, CL_Y, CL_W, CL_VH], 'confidant.count', CL_P, show='confidant.ready')
y = CL_Y
q = 'confidant.{i}.'
g = 'confidant.ready'
tap = kit.shape('rect', CL_W, CL_H, fill='#00000000')
csel = dict(hide_bind='confidant.sel.index', keep_min='{i}', keep_max='{i}')
cf += rp([plate(CL_X, y, CL_W, CL_H, color=TILE, skew=8, need_bind=g),
          plate(CL_X, y, CL_W, CL_H, color=RED, skew=8, need_bind=g, **csel),
          dict(type='image', src=F70_BY_ID[8], rect=[CL_X + 12, y + 2, 70, 70], bind=q + 'id',
               src_names=F70_BY_ID, color=INK, need_bind=g),
          dict(type='image', src=arc_left[1], rect=[CL_X + 96, y + 4, ARC_LW, ARC_LH], bind=q + 'arcana',
               src_names=arc_left, color=INK, need_bind=g),
          dict(type='label', rect=[CL_X + 96, y + 44, 300, 26], text='', bind_text=q + 'person',
               text_scale=4, color=INK, need_bind=g),
          IMG(RANK_W, CL_X + 404, y + 14, RWW, RWH, need_bind=g, hide_bind=q + 'max', keep_max=0),
          dict(type='image', src=RANKS['srcs'][0], rect=[CL_X + 412 + RWW, y + 4, RANKS['w'], RANKS['h']],
               bind=q + 'rank', src_names=RANKS['srcs'], color=INK, need_bind=g, hide_bind=q + 'max',
               keep_max=0),
          IMG(MAXW, CL_X + CL_W - 26 - MXW, y + 16, MXW, MXH, need_bind=g, hide_bind=q + 'max', keep_min=1),
          dict(type='image', src=tap, rect=[CL_X, y, CL_W, CL_H], color='#00000000', on_tap=sel_act,
               payload='{i}', need_bind=g)], 'confidant.count', CL_P, 24, scroll=rid)
cf.append(pending('Confidants appear once the game reports them.', 'confidant.ready', 290, 500))
# ---- detail (right)
S_ = 'confidant.sel.ready'
DX, DW = 572, 644
cf.append(plate(DX, 132, DW, 262, color=WHITE, skew=14, need_bind=S_))
cf.append(dict(type='image', src=HEAD_BY_ID[8], rect=[DX + DW - HEAD_W - 12, 190, HEAD_W, HEAD_H],
               bind='confidant.sel.id', src_names=HEAD_BY_ID, color=INK, need_bind=S_))
cf.append(plate(DX, 132, DW, 50, color=BLACK, skew=10, need_bind=S_))
cf.append(dict(type='label', rect=[DX + DW // 2, 142, 0, 30], text='', bind_text='confidant.sel.person',
               text_scale=5, color=INK, align='center', need_bind=S_))
CDW, CDH = 90, 180
cards_s = [''] * 32
for a in range(1, 32):
    f = art.CARDTEX / f'C_CARD{a - 1:02X}.DDS'
    if f.exists():
        cards_s[a] = save(Art.dds(f'EN/CAMP/CARDTEX/{f.name}').resize(CDW, CDH), f'cf/cards{a}',
                          {'source': f'EN/CAMP/CARDTEX/{f.name}', 'arcana': a, 'size': [CDW, CDH]})
cf.append(dict(type='image', src=cards_s[1], rect=[DX + 20, 196, CDW, CDH], bind='confidant.sel.arcana',
               src_names=cards_s, color=INK, need_bind=S_))
ARC_W, AWW, AWH = spr(479, 'arcana_w', h=30, color=BLACK)
big_s = 56
arc_big_s = [''] * 32
bws = max(round(im.width * big_s / big_h) for im in big_imgs.values())
for a, im in big_imgs.items():
    arc_big_s[a] = save(Kit.pad(Kit.fit(im, height=big_s), bws, big_s, 'left'), f'arcbs{a}',
                        {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': ARC_BIG[a],
                         'note': 'big arcana name (white, black rim)', 'size': [bws, big_s]})
AX = DX + 126
cf.append(IMG(ARC_W, AX, 210, AWW, AWH, need_bind=S_))
cf.append(dict(type='image', src=arc_big_s[1], rect=[AX + AWW + 10, 196, bws, big_s],
               bind='confidant.sel.arcana', src_names=arc_big_s, color=INK, need_bind=S_))
RK2, R2W, R2H = spr(476, 'rank_blk', h=28, color=BLACK)
cf.append(IMG(RK2, AX, 290, R2W, R2H, need_bind=S_, hide_bind='confidant.sel.rank', keep_max=9))
RKB = digit_set('rkb', 274, 70)
rkb_black = [save(Kit.recolor(kit.art_of[s], BLACK), f'rkbk{i}',
                  {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': 274 + i, 'note': 'recoloured black'})
             for i, s in enumerate(RKB['srcs'])]
cf.append(dict(type='image', src=rkb_black[0], rect=[AX + R2W + 10, 262, RKB['w'], RKB['h']],
               bind='confidant.sel.rank', src_names=rkb_black, color=INK, need_bind=S_,
               hide_bind='confidant.sel.rank', keep_max=9))
MAXB, MBW, MBH = spr(1074, 'rank_maxb', h=64)
cf.append(IMG(MAXB, AX, 264, MBW, MBH, need_bind=S_, hide_bind='confidant.sel.rank', keep_min=10))
STAR_B, SBW, SBH = spr(482, 'star_bs', h=24, color=BLACK)
STAR_G, _, _ = spr(482, 'star_gs', h=24, color='#FFB4B4B4')
for k in range(10):
    x = AX + k * 30
    cf.append(IMG(STAR_G, x, 352, SBW, SBH, need_bind=S_))
    cf.append(IMG(STAR_B, x, 352, SBW, SBH, need_bind=S_, hide_bind='confidant.sel.rank', keep_min=k + 1))
# rank story
cf.append(plate(DX, 402, DW, 110, color=BLACK, skew=12, need_bind=S_))
cf.append(wrap(DX + 26, 416, 'confidant.sel.story', DW - 52, 3, need_bind=S_))
# abilities (drag to scroll) + help of the highlighted one
ab_act = flag('ui.ab')
ABC = 'confidant.sel.ability.count'
AB_Y, AB_VH, AB_P = 522, 276, 46
cf.append(plate(DX, AB_Y - 4, DW, AB_VH + 8, color=BLACK, skew=10, need_bind=S_))
rid = region('m_confidant', 'abil', [DX + 14, AB_Y, DW - 28, AB_VH], ABC, AB_P, show=S_,
             reset='confidant.sel.index')
y = AB_Y
q = 'confidant.sel.ability.{i}.'
AW_ = DW - 28
tap = kit.shape('rect', AW_, 40, fill='#00000000')
cf += rp([plate(DX + 14, y, AW_, 40, color=TILE, skew=6, need_bind=S_),
          plate(DX + 14, y, AW_, 40, color=RED, skew=6, need_bind=S_, hide_bind='@flag:ui.ab', keep_min='{i}',
                keep_max='{i}'),
          dict(type='image', src=tap, rect=[DX + 14, y, AW_, 40], color='#00000000', on_tap=ab_act,
               payload='{i}', need_bind=S_),
          dict(type='value', rect=[DX + 44, y + 10, 0, 26], bind=q + 'rank', text='', text_scale=4,
               color=CYAN, align='center', need_bind=S_),
          T(DX + 80, y + 10, bind_text=q + 'name', scale=4, w=AW_ - 150, need_bind=S_),
          T(DX + DW - 30, y + 12, 'NEXT', 3, DIM, align='right', need_bind=S_, hide_bind=q + 'unlocked',
            keep_max=0),
          T(DX + 80, y + 10, '???', 4, DIM, need_bind=S_, hide_bind=q + 'hidden', keep_min=1)],
         ABC, AB_P, 20, scroll=rid)
cf.append(plate(DX, 808, DW, 156, color=BLACK, skew=12, need_bind=S_))
cf.append(IMG(kit.shape('rect', DW - 40, 3), DX + 20, 814, DW - 40, 3, color=RED, need_bind=S_))
cf += rp([wrap(DX + 26, 826, q + 'desc', DW - 52, 4, need_bind=S_, hide_bind='@flag:ui.ab',
               keep_min='{i}', keep_max='{i}')], ABC, 0, 20)
cf.append(pending('Tap a confidant.', S_, DX + DW // 2, 500))
page('m_confidant', 'CONFIDANT', cf)
# the old separate detail page id stays reachable (ui.page 8 / page.m_cfdetail) and just leads back
cd = chrome(spr(731, 'w_confidant2', h=80))
cd += button(436, 500, 368, 80, 'CONFIDANT', 'ui.open', 'live.ready', scale=6, payload=PAGE_ID['m_confidant'])
page('m_cfdetail', 'CONFIDANT', cd)

# ================================================================== REQUEST
# ui_b: native tabs Recent / Progress / Difficulty (sprites 494-496) above the native column heads
# Progress / Title / Difficulty (507-509); every request row (drag to scroll) carries its state plate,
# title and the native difficulty letter (514 + request.K.grade: ? D C B A S-MAX; before m_data2's
# module: request.K.difficulty with state 1 as "?"). A tab tap is module action request_tab P
# (0 Recent, 1 Progress, 2 Difficulty); the module re-orders request.K.* like the native tab and
# echoes request.tab (m_data2).
RQ_P, RQ_CAP = 64, 100
rq = chrome(spr(506, 'w_request', h=80), (*hero('P5_CAMPMISSION_2D', 600, 'bg_request', (70, 14, 18)),
                                          700, 360))
rq_act = flag('ui.rq')
RR = 'request.ready'
actions['rq.sort'] = dict(kind='module', action='request_tab', argument='$payload', enabled_bind=RR)
RQT = [(494, 'Recent'), (495, 'Progress'), (496, 'Difficulty')]
TX = 24
for k, (sid, _) in enumerate(RQT):
    _im = Kit.fit(kit.load_sprite('camp00', sid).bbox(), height=40)  # 495 carries empty margin
    src, tw, th = save(_im, f'rqtab{k}', {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': sid,
                                          'crop': 'alpha bbox'}), _im.width, _im.height
    w_ = tw + 60
    rq.append(plate(TX, 134, w_, 54, color=TILE_HI, skew=10, need_bind=RR))
    rq.append(plate(TX, 134, w_, 54, color=RED, skew=10, need_bind=RR, hide_bind='request.tab',
                    keep_min=k, keep_max=k))
    rq.append(IMG(src, TX + 30, 134 + (54 - th) // 2, tw, th, need_bind=RR))
    rq.append(dict(type='image', src=kit.shape('rect', w_, 54, fill='#00000000'), rect=[TX, 134, w_, 54],
                   color='#00000000', on_tap='rq.sort', payload=str(k), need_bind=RR))
    TX += w_ + 8
# column heads (native: white plates, black lettering)
for sid, x, w_ in [(507, 24, 190), (508, 222, 770), (509, 1000, 216)]:
    src, tw, th = spr(sid, f'rqhead{sid}', h=32, color=BLACK)
    rq.append(plate(x, 196, w_, 42, color=WHITE, skew=8, need_bind=RR))
    rq.append(IMG(src, x + (w_ - tw) // 2, 196 + (42 - th) // 2, tw, th, need_bind=RR))
done_src, SSW, SSH = spr(504, 'rqst_done', h=40)
st_srcs = [''] * 256
st_srcs[5] = done_src
# state 1 = the native "?" request (target not identified; live DATA13: exactly the 5 native "?" rows):
# icon 497 (black "?") on its base 498 (white), and "?" (514) instead of the difficulty letter
_qb = Kit.fit(kit.load_sprite('camp00', 498), height=SSH)
_qi = Kit.recolor(Kit.fit(kit.load_sprite('camp00', 497), height=SSH - 8), BLACK)
_qb = _qb.over(_qi, (_qb.width - _qi.width) // 2, 4)
st_srcs[1] = save(Kit.pad(_qb, SSW, SSH, 'left'), 'rqst_q', {'source': 'EN/INIT/P5CAMP_00SPD.SPD',
                                                          'sprite_ids': [498, 497], 'note': 'state 1 "?"'})
for sids, sts, nm in (((743, 744), (2, 3, 4), 'rqst_idd'), ((801, 802), (6,), 'rqst_unk')):
    _b = Kit.recolor(Kit.fit(kit.load_sprite('camp00', sids[0]), height=SSH), BLACK)
    _t = Kit.fit(kit.load_sprite('camp00', sids[1]), height=SSH)
    _b = _b.over(_t, 0, 0) if _t.size == _b.size else _b.over(Kit.pad(_t, _b.width, _b.height), 0, 0)
    _src = save(Kit.pad(_b, max(SSW, _b.width), SSH, 'left'), nm,
                {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_ids': list(sids),
                 'note': 'progress plate (base black, lettering white: colours not verified)'})
    for st_ in sts:
        st_srcs[st_] = _src
NEW_R, NRW, NRH = spr(512, 'rq_new', h=30)
# difficulty letters, one common cell (value = request.K.difficulty; anything past 5 draws nothing)
dsrcs, DLW, DLH = kit.sprite_set('camp00', [514, 515, 516, 517, 518, 519], 'rqdiff', height=50)
diff_srcs = dsrcs + [''] * 250
RQ_Y, RQ_VH = 246, 484
rid = region('m_request', 'rq', [24, RQ_Y, 1192, RQ_VH], 'request.count', RQ_P, show=RR,
             reset='request.tab')
y = RQ_Y
q = 'request.{i}.'
tap = kit.shape('rect', 1192, 58, fill='#00000000')
rq += rp([plate(24, y, 1192, 58, color=TILE, skew=6, need_bind=RR),
          plate(24, y, 1192, 58, color=RED, skew=6, need_bind=RR, hide_bind='@flag:ui.rq', keep_min='{i}',
                keep_max='{i}'),
          dict(type='image', src=tap, rect=[24, y, 1192, 58], color='#00000000', on_tap=rq_act,
               payload='{i}', need_bind=RR),
          dict(type='image', src=done_src, rect=[40, y + 9, SSW, SSH], bind=q + 'state',
               src_names=st_srcs, color=INK, need_bind=RR),
          T(234, y + 16, bind_text=q + 'name', scale=5, w=750, need_bind=RR),
          # m_data2 module: the native grade (state 1 already "?"). Older module (no request.extra.*):
          # the raw byte, "?" for state 1. The fallback pair is pushed out of the list's clip rect
          # (x_bind request.extra.ready * 4000) as soon as the grade exists, so they never overlap.
          dict(type='image', src=dsrcs[0], rect=[1108 - DLW // 2, y + 4, DLW, DLH], bind=q + 'grade',
               src_names=diff_srcs, color=INK, need_bind=both(RR, 'request.extra.ready')),
          dict(type='image', src=dsrcs[0], rect=[1108 - DLW // 2, y + 4, DLW, DLH], bind=q + 'difficulty',
               src_names=diff_srcs, color=INK, need_bind=RR, hide_bind=q + 'state', hide_eq=1,
               x_bind='request.extra.ready', x_scale=4000),
          IMG(dsrcs[0], 1108 - DLW // 2, y + 4, DLW, DLH, need_bind=RR, hide_bind=q + 'state', keep_min=1,
              keep_max=1, x_bind='request.extra.ready', x_scale=4000),
          IMG(NEW_R, 30, y - 4, NRW, NRH, need_bind=RR, hide_bind=q + 'new', keep_min=1)],
         'request.count', RQ_P, RQ_CAP, scroll=rid)
rq.append(plate(24, 740, 1192, 224, color=BLACK, skew=10, need_bind=RR))
rq.append(IMG(kit.shape('rect', 1160, 3), 40, 746, 1160, 3, color=RED, need_bind=RR))
TGT, TGW, TGH = spr(511, 'rq_target', h=40)
sel = dict(need_bind=RR, hide_bind='@flag:ui.rq', keep_min='{i}', keep_max='{i}')
rq += rp([IMG(TGT, 48, 758, TGW, TGH, **sel),
          T(64 + TGW, 766, bind_text=q + 'target', scale=5, **sel),
          T(64, 812, bind_text=q + 'name', scale=5, color=RED, w=1100, **sel),
          wrap(64, 856, q + 'desc', 1120, 3, **sel)], 'request.count', 0, RQ_CAP)
rq.append(T(620, 400, 'No requests.', 5, DIM, align='center', need_bind=RR, hide_bind='request.count',
            keep_max=0))
rq.append(pending('Requests appear once the game reports them.', RR))
page('m_request', 'REQUEST', rq)

# ================================================================== CALENDAR (tilted native month grid)
# ui_b: the month grid is tilted like the native camp calendar (rows rise ~5 deg to the right; the
# game's own digit / weekday / TODAY art, pre-rotated), plan / deadline rows drawn as the native dots
# and range lines from the per-cell bitmasks (one pre-composed image per mask value: no derived
# bit maths), and under it the current day's detail next to the native Day Job / Night Job panels
# (calendar.job.{day,night}.K.label, m_data2).
# 1.0.3: the grid shows calendar.view.* (the selected day's month; native L / R = module action
# calendar_month), every day is a tap target (calendar_select) with the gold selection frame, and a
# past day shows its plans plus the native Daily Log (calendar.sel.log.*) instead of the job panels.
import math  # noqa: E402
ca = chrome(spr(452, 'w_calendar', h=80))
C_ = 'calendar.ready'
CAL_A = 5.0                               # tilt, degrees (native grid rises to the right)
_ca, _sa = math.cos(math.radians(CAL_A)), math.sin(math.radians(CAL_A))
CCW, CCH = 164, 86                        # cell pitch along / across the tilted rows
CX0, CY0 = 112, 298                       # centre of cell (col 0, row 0)


def cal_at(col, row):
    return (CX0 + col * CCW * _ca + row * CCH * _sa, CY0 - col * CCW * _sa + row * CCH * _ca)


def tilt(im):
    return im.rotate(CAL_A)


def put(src, w_, h_, cx, cy, **kw):
    return IMG(src, round(cx - w_ / 2), round(cy - h_ / 2), w_, h_, **kw)


for d, sid in enumerate([550, 551, 552, 553, 554, 555, 556]):
    col = RED if d == 0 else (CYAN if d == 6 else INK)
    im = tilt(Kit.recolor(Kit.fit(kit.load_sprite('camp00', sid), height=40), col))
    src = save(im, f'cal/wk{d}', {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': sid,
                                  'note': f'weekday, rotated {CAL_A} deg'})
    cx, cy = cal_at(d, -0.82)
    ca.append(put(src, im.width, im.height, cx, cy, need_bind=C_))
# day numbers 1..31 from the calendar digits (535-544), rotated, one common cell
cal_digits = [Kit.fit(kit.load_sprite('camp00', 535 + i), height=74) for i in range(10)]
day_imgs = {}
for dnum in range(1, 32):
    ims = [cal_digits[int(c)] for c in str(dnum)]
    im = Art.new(sum(i.width for i in ims) - 8 * (len(ims) - 1), 74)
    x = 0
    for i in ims:
        im = im.over(i, x, 0)
        x += i.width - 8
    day_imgs[dnum] = im
dw0 = max(i.width for i in day_imgs.values())
day_imgs = {d: tilt(Kit.pad(im, dw0, 74, 'center')) for d, im in day_imgs.items()}
dw, dh = day_imgs[1].size
day_srcs = [''] + [save(day_imgs[d], f'cal/t{d}',
                        {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_ids': [535 + int(c) for c in str(d)],
                         'note': f'calendar digits composed, rotated {CAL_A} deg'}) for d in range(1, 32)]
# plan / deadline mask images: bit 0 single dot, 1 range start, 2 middle, 3 end (| 0x10 = active)
MKW, MKH = CCW, 14


def mask_img(v, rgb):
    s = 4
    im = Art.new(MKW * s, MKH * s)
    c = (*rgb, 255)
    mid, cy, r, t = MKW * s // 2, MKH * s // 2, 5 * s, 2 * s
    if v & 2:
        im = im.rectangle([mid, cy - t, MKW * s, cy + t], c)
    if v & 4:
        im = im.rectangle([0, cy - t, MKW * s, cy + t], c)
    if v & 8:
        im = im.rectangle([0, cy - t, mid, cy + t], c)
    if v & 11:
        im = im.ellipse([mid - r, cy - r, mid + r, cy + r], c)
    return tilt(im.resize(MKW, MKH))


mark_srcs, dl_srcs = [], []
for v in range(32):
    lo = v & 15
    mark_srcs.append(save(mask_img(lo, (255, 255, 255)), f'cal/mk{v}',
                          {'source': 'generated (native plan dots / range line)', 'mask': v}) if lo else '')
    dl_srcs.append(save(mask_img(lo, (229, 25, 28)), f'cal/dl{v}',
                        {'source': 'generated (deadline dots / range line, active only)', 'mask': v})
                   if lo and v & 16 else '')
MW2, MH2 = mask_img(15, (0, 0, 0)).size
mark_srcs += [''] * 224
dl_srcs += [''] * 224
# today: red tilted plate + native TODAY art, placed by derived x/y (tilted grid maths)
dv('ui.cal.tcell', terms=[['calendar.view.first_weekday', 1], ['calendar.day', 1]], add=-1)
TODAY_ON = both(C_, 'calendar.view.today')   # the grid shows today's month
dv('ui.cal.trow', terms=[['ui.cal.tcell', 1 / 7]], add=1e-6, floor=True)
dv('ui.cal.tcol', terms=[['ui.cal.tcell', 1], ['ui.cal.trow', -7]])
dv('ui.cal.tx', terms=[['ui.cal.tcol', CCW * _ca], ['ui.cal.trow', CCH * _sa]], add=0.5, floor=True)
dv('ui.cal.ty', terms=[['ui.cal.tcol', -CCW * _sa], ['ui.cal.trow', CCH * _ca]], add=0.5, floor=True)
today_at = dict(x_bind='ui.cal.tx', x_scale=1, y_bind='ui.cal.ty', y_scale=1)
_tp = Art.new(CCW - 16, CCH - 6).polygon([(14, 0), (CCW - 16, 0), (CCW - 30, CCH - 6), (0, CCH - 6)],
                                          (255, 255, 255, 255))
_tp = tilt(_tp)
TODAY_BG = save(_tp, 'cal/today_bg', {'source': 'generated (tilted plate)'})
ca.append(dict(put(TODAY_BG, _tp.width, _tp.height, *cal_at(0, 0), color=RED, need_bind=TODAY_ON), **today_at))
for c in range(42):
    col, row = c % 7, c // 7
    cx, cy = cal_at(col, row)
    day = dv(f'ui.cal.day.{c}', terms=[['calendar.view.first_weekday', -1]], add=c + 1)
    # only cells 28.. can hold a day past the month's end (29..31): those check days_in_month
    inm = dv(f'ui.cal.in.{c}', cmp='le', a=day, b='calendar.view.days_in_month') if c >= 28 else C_
    # weekdays before today are grey: view.cut = today (today's month), 32 (earlier), 0 (later)
    past = dv(f'ui.cal.past.{c}', cmp='lt', a=day, b='calendar.view.cut')
    future = dv(f'ui.cal.fut.{c}', cmp='ge', a=day, b='calendar.view.cut')
    hol = f'calendar.cell.{c}.holiday'
    base = RED if col == 0 else (CYAN if col == 6 else INK)
    img = dict(type='image', src=day_srcs[1], rect=[round(cx - dw / 2), round(cy - dh / 2), dw, dh], bind=day,
               src_names=day_srcs, hide_bind=day, keep_min=1, keep_max=31)
    # native: Sundays red and Saturdays cyan all month; weekdays before today grey; a holiday red
    if col in (0, 6):
        ca.append(dict(img, color=base, need_bind=inm))
    else:
        ca.append(dict(img, color='#FF8A8286', need_bind=both(inm, past)))
        ca.append(dict(img, color=base, need_bind=both(inm, future)))
    if col != 0:  # holiday: red over the base colour, past or not
        ca.append(dict(img, color=RED, need_bind=both(inm, hol)))
    # plan rows (white dots / range lines) and ACTIVE deadlines (red) under the day, by mask value
    mx, my = cx + 36 * _sa, cy + 36 * _ca
    ca.append(dict(type='image', src=mark_srcs[1] or mark_srcs[2], rect=[round(mx - MW2 / 2), round(my - MH2 / 2), MW2, MH2],
                   bind=f'calendar.cell.{c}.mark', src_names=mark_srcs, color=INK,
                   need_bind=f'calendar.cell.{c}.mark'))
    dx_, dy_ = cx + 46 * _sa, cy + 46 * _ca
    ca.append(dict(type='image', src=dl_srcs[17], rect=[round(dx_ - MW2 / 2), round(dy_ - MH2 / 2), MW2, MH2],
                   bind=f'calendar.cell.{c}.deadline', src_names=dl_srcs, color=INK,
                   need_bind=f'calendar.cell.{c}.deadline'))
# today's number again (white, over the red plate) and the TODAY art under it
x0, y0 = cal_at(0, 0)
ca.append(dict(type='image', src=day_srcs[1], rect=[round(x0 - dw / 2), round(y0 - dh / 2), dw, dh],
               bind='calendar.day', src_names=day_srcs, color=INK, need_bind=TODAY_ON, **today_at))
_td = tilt(Kit.fit(kit.load_sprite('camp00', 557), height=30))
TODAY_W = save(_td, 'cal/today_w', {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': 557,
                                    'note': f'TODAY, rotated {CAL_A} deg'})
_tt = Art.new(_td.width + 16, 34).polygon([(8, 0), (_td.width + 16, 0), (_td.width + 8, 34), (0, 34)],
                                          (255, 255, 255, 255))
_tt = tilt(_tt)
TODAY_T = save(_tt, 'cal/today_tag', {'source': 'generated (tilted tag)'})
tcx, tcy = x0 + 44 * _sa, y0 + 44 * _ca
ca.append(dict(put(TODAY_T, _tt.width, _tt.height, tcx, tcy, color=BLACK, need_bind=TODAY_ON), **today_at))
ca.append(dict(put(TODAY_W, _td.width, _td.height, tcx, tcy, color=INK, need_bind=TODAY_ON), **today_at))
# ---- day pick (m_data2 calendar_select M*100+D): every in-month cell is a tap target; the picked day
# (today by default, the native cursor) gets the gold selection frame used on the other pages and the
# panel below shows that day. (1.0.2 pruned the ui.cal.key.* payloads: prune_derived now keeps
# "$name" references.)
actions['cal.sel'] = dict(kind='module', action='calendar_select', argument='$payload', enabled_bind=C_)
SEL_OK = both(C_, 'calendar.sel.ready')
_fr = Art.new((CCW - 8) * 4, (CCH - 2) * 4).polygon_outline(
    [(56, 0), ((CCW - 8) * 4, 0), ((CCW - 8) * 4 - 56, (CCH - 2) * 4), (0, (CCH - 2) * 4)], (255, 255, 255, 255), 20)
_fr = tilt(_fr.resize(CCW - 8, CCH - 2))
SEL_FR = save(_fr, 'cal/sel_frame', {'source': 'generated (tilted cursor frame)'})
dv('ui.cal.scell', terms=[['calendar.view.first_weekday', 1], ['calendar.sel.day', 1]], add=-1)
dv('ui.cal.srow', terms=[['ui.cal.scell', 1 / 7]], add=1e-6, floor=True)
dv('ui.cal.scol', terms=[['ui.cal.scell', 1], ['ui.cal.srow', -7]])
dv('ui.cal.sx', terms=[['ui.cal.scol', CCW * _ca], ['ui.cal.srow', CCH * _sa]], add=0.5, floor=True)
dv('ui.cal.sy', terms=[['ui.cal.scol', -CCW * _sa], ['ui.cal.srow', CCH * _ca]], add=0.5, floor=True)
same_m = dv('ui.cal.smonth', cmp='eq', a='calendar.sel.month', b='calendar.view.month')
GOLD = '#FFFFC21A'  # P5 yellow selection frame (battle page, v0.9.2)
ca.append(dict(put(SEL_FR, _fr.width, _fr.height, *cal_at(0, 0), color=GOLD, need_bind=both(SEL_OK, same_m)),
               x_bind='ui.cal.sx', x_scale=1, y_bind='ui.cal.sy', y_scale=1))
_ctap = kit.shape('rect', CCW - 8, CCH - 4, fill='#00000000')
for c in range(42):
    cx, cy = cal_at(c % 7, c // 7)
    day = f'ui.cal.day.{c}'
    key = dv(f'ui.cal.key.{c}', terms=[['calendar.view.month', 100], [day, 1]])
    inm = f'ui.cal.in.{c}' if c >= 28 else C_
    ca.append(dict(type='image', src=_ctap, rect=[round(cx - (CCW - 8) / 2), round(cy - (CCH - 4) / 2), CCW - 8,
                                                 CCH - 4], color='#00000000', on_tap='cal.sel', payload='$' + key,
                   need_bind=inm, hide_bind=day, keep_min=1, keep_max=31))
# ---- month L / R (native: L / R page the grid April .. March, same day; greyed at the ends)
actions['cal.month'] = dict(kind='module', action='calendar_month', argument='$payload', enabled_bind=C_)
MY, MKW_, MKH_ = 676, 64, 50
for k, (label, x, can) in enumerate((('L', 780, 'calendar.view.can_prev'), ('R', 1136, 'calendar.view.can_next'))):
    ca.append(plate(x, MY, MKW_, MKH_, color='#FF3A3236', skew=8, need_bind=C_))
    ca.append(plate(x, MY, MKW_, MKH_, color=INK, skew=8, need_bind=both(C_, can)))
    ca.append(T(x + MKW_ // 2, MY + (MKH_ - cap_px(5)) // 2, label, 5, BLACK, align='center', need_bind=C_))
    ca.append(dict(type='image', src=kit.shape('rect', MKW_ + 40, MKH_ + 30, fill='#00000000'),
                   rect=[x - 20, MY - 15, MKW_ + 40, MKH_ + 30], color='#00000000', on_tap='cal.month',
                   payload=str(2 * k - 1), need_bind=both(C_, can)))
ca.append(dict(type='value', rect=[1000, MY + (MKH_ - cap_px(7)) // 2, 0, cap_px(7) + 21], bind='calendar.view.month',
               text='', text_scale=7, color=INK, align='center', need_bind=C_,
               names=['', 'JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC']))
# ---- the picked day's detail (left) + Day Job / Night Job (native panels), on the native white band.
# m_data2 module: calendar.sel.* (default = today); older module: today (live.date, calendar.today.*).
PY = 776
OFF = dict(x_bind='calendar.sel.ready', x_scale=4000)   # today fallback: pushed off-canvas once calendar.sel.* exists
ca.append(IMG(slab(1240, 200, [(0, 16), (1240, 0), (1240, 188), (0, 200)], 'cal_band', fill='#FFFFFFFF'),
              0, PY - 8, 1240, 200, need_bind=C_))
ca.append(plate(24, PY, 380, 48, color=BLACK, skew=10, need_bind=C_))
ca.append(T(214, PY + 12, bind_text='live.date', scale=5, color=INK, align='center', need_bind=C_, **OFF))
ca.append(V(100, PY + 10, 'calendar.sel.month', 5, INK, align='right', need_bind=SEL_OK))
ca.append(T(104, PY + 10, '/', 5, INK, need_bind=SEL_OK))
ca.append(V(120, PY + 10, 'calendar.sel.day', 5, INK, need_bind=SEL_OK))
ca.append(dict(type='value', rect=[180, PY + 10, 120, 36], bind='calendar.sel.weekday', text='', text_scale=5,
               names=['(Sun)', '(Mon)', '(Tue)', '(Wed)', '(Thu)', '(Fri)', '(Sat)'], color=INK,
               need_bind=SEL_OK))
ca.append(T(292, PY + 14, 'TODAY', 4, RED, need_bind=SEL_OK, hide_bind='calendar.sel.today', keep_min=1))
DIMK = '#FF6E6468'
tq = 'calendar.today.{i}.'
ca.append(T(44, PY + 62, 'No plans.', 4, DIMK, need_bind=C_, hide_bind='calendar.today.count', keep_max=0, **OFF))
ca += rp([T(44, PY + 62, bind_text=tq + 'label', scale=4, color=BLACK, w=360, need_bind=C_,
            hide_bind=tq + 'kind', keep_max=0, **OFF),
          T(44, PY + 62, bind_text=tq + 'label', scale=4, color=RED, w=360, need_bind=C_,
            hide_bind=tq + 'kind', keep_min=1, **OFF)], 'calendar.today.count', 30, 4)
sq = 'calendar.sel.plan.{i}.'
ca.append(T(44, PY + 62, 'No plans.', 4, DIMK, need_bind=SEL_OK, hide_bind='calendar.sel.plan.count', keep_max=0,
            x_bind='calendar.sel.past', x_scale=4000))
ca += rp([T(44, PY + 62, bind_text=sq + 'label', scale=4, color=BLACK, w=360, need_bind=SEL_OK,
            hide_bind=sq + 'kind', keep_max=0),
          T(44, PY + 62, bind_text=sq + 'label', scale=4, color=RED, w=360, need_bind=SEL_OK,
            hide_bind=sq + 'kind', keep_min=1)], 'calendar.sel.plan.count', 30, 4)
_bl = Art.new(40, 40).ellipse([0, 0, 39, 39], (255, 255, 255, 255))
BULLET = save(_bl.resize(10, 10), 'cal/bullet', {'source': 'generated (list bullet)'})
NOT_PAST = dv('ui.cal.notpast', cmp='eq', a='calendar.sel.past', b=0)   # missing reads as 0
JOBS_ON = both(C_, NOT_PAST)
for k, (sid, key, x) in enumerate([(545, 'dayjob', 420), (546, 'nightjob', 824)]):
    src, jw, jh = spr(sid, f'cal_job{k}', h=48)
    ca.append(plate(x + jw - 20, PY + 16, 392 - jw + 10, 18, color=BLACK, skew=6, need_bind=JOBS_ON))
    ca.append(IMG(src, x, PY, jw, jh, need_bind=JOBS_ON))
    for pre, gate in ((f'calendar.sel.{key}.', SEL_OK), (f'calendar.{key}.', both(C_, 'calendar.jobs.ready'))):
        extra = dict(x_bind='calendar.sel.past', x_scale=4000) if pre.startswith('calendar.sel') else OFF
        # (native: an empty job panel stays empty)
        ca += rp([IMG(BULLET, x + 20, PY + 71, 10, 10, color=BLACK, need_bind=gate, **extra),
                  T(x + 36, PY + 62, bind_text=pre + '{i}.name', scale=4, color=BLACK, w=350, need_bind=gate,
                    **extra)], pre + 'count', 30, 3)
# ---- past day: the native Daily Log (77AF2C) in place of the job panels: the notebook's own
# "Daily Log" heading (camp 829), two ruled lines (87F8F0 line 0 / 1; red + the camp underline 831
# for Palace / Mementos / Treasure rows) and the red stickers 832 bus / 833 Morgana / 834 Done!!.
LOG_ON = both(SEL_OK, 'calendar.sel.past', 'calendar.sel.log.ready')
_lt = Kit.fit(kit.load_sprite('camp00', 829).crop((50, 30, 360, 145)), height=56)
LOG_T = save(_lt, 'cal/log_title', {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': 829,
                                    'crop': [50, 30, 360, 145], 'note': 'Daily Log heading'})
ca.append(IMG(LOG_T, 420, PY - 6, _lt.width, _lt.height, need_bind=LOG_ON))
_ul = Kit.recolor(Kit.fit(kit.load_sprite('camp00', 831), width=440), RED)
LOG_UL = save(_ul, 'cal/log_ul', {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': 831,
                                  'note': 'Daily Log underline (red rows), 77EE30'})
for k in range(2):
    ly = PY + 62 + 52 * k
    q = f'calendar.sel.log.{k}.'
    ca.append(IMG(kit.shape('rect', 520, 3), 440, ly + 40, 520, 3, color='#FFB4B4B4', need_bind=LOG_ON))
    ca.append(IMG(LOG_UL, 450, ly + 30, _ul.width, _ul.height, need_bind=both(LOG_ON, q + 'red')))
    ca.append(T(460, ly, bind_text=q + 'label', scale=4, color=BLACK, w=500, need_bind=LOG_ON,
                hide_bind=q + 'red', keep_max=0))
    ca.append(T(460, ly, bind_text=q + 'label', scale=4, color=RED, w=500, need_bind=both(LOG_ON, q + 'red')))
for sid, key, x in ((832, 'bus', 990), (833, 'morgana', 990), (834, 'done', 1110)):
    _st = Kit.recolor(Kit.fit(kit.load_sprite('camp00', sid), height=150 if sid == 834 else 110), RED)
    src = save(_st, f'cal/log_{key}', {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': sid,
                                       'note': 'Daily Log sticker (red), 77AF2C'})
    ca.append(IMG(src, x, PY + 36 if sid != 834 else PY + 20, _st.width, _st.height,
                  need_bind=both(LOG_ON, f'calendar.sel.log.{key}')))
ca.append(pending('The calendar appears once the game reports the date.', C_))
page('m_calendar', 'CALENDAR', ca)

# ================================================================== MUSIC (Thieves Den player, read-only)
# Opened from the bottom bar's mini player on any page. Current track (bgm.*) +
# the 107 Thieves Den tracks as a drag-scrollable list with the playing one lit (bgm.index). Titles
# are the game's own title art (EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD, sprite i = table row i); the 7
# rows without art use the table's name in the game font. No playback control.
import music_art as mus  # noqa: E402

BGM_ON = dv('ui.bgm.on', all_nonzero=['bgm.ready', 'bgm.playing'])
BGM_KNOWN = both(BGM_ON, 'bgm.known')
BGM_UNK = both(BGM_ON, dv('ui.bgm.unk', cmp='eq', a='bgm.known', b=0))
BGM_NOIDX = both(BGM_KNOWN, dv('ui.bgm.noidx', cmp='lt', a='bgm.index', b=0))
BGM_NUM = dv('ui.bgm.num', terms=[['bgm.index', 1]], add=1)
TRK = mus.tracks()
NTRK = len(TRK)
MUS_LIST_W, MUS_BIG_W, MUS_MINI_W = 944, 830, 420


def _title_img(i, h, w, color=(255, 255, 255, 255)):
    """Title art of row i at cap-ish height h (native art scaled; text for blank rows), faded at w."""
    im = mus.title(i)
    if im is None:
        im = kit.text_art(TRK[i]['clean'], int(h * 0.72))
        prov = {'source': 'EN/FONT/FONT0.FNT (text)', 'text': TRK[i]['clean'],
                'table': 'EN/INIT/MYPTABLE.BIN mypSoundNameTable row %d' % i}
    else:
        if h != im.height:
            im = im.resize(max(1, round(im.width * h / im.height)), h)
        prov = {'source': mus.TITLE_SPD, 'sprite_id': i, 'note': 'Thieves Den title art', 'h': h}
    if color != (255, 255, 255, 255):
        im = Kit.recolor(im, '#%02X%02X%02X%02X' % (color[3], *color[:3]))
    im = im.fade_right(w)
    if im.height > h:
        im = Kit.fit(im, height=h)
    # the runtime stretches an image to its rect: every cell is padded to exactly w x h
    return Kit.pad(im, w, h, 'left'), prov


def title_set(tag, h, w):
    srcs = []
    for i in range(NTRK):
        im, prov = _title_img(i, h, w)
        prov = dict(prov, row=i, cue=TRK[i]['cue'], fade_at=w)
        # the list set is addressed by a {i} template: fixed recipe ids l<i> in module mode
        src = save(im, f'mus/{tag}/{i}', prov, rid=f'{tag}{i}' if tag == 'l' else None)
        if kit.mode == 'png':
            # white art on alpha: a 32-entry palette is visually identical at a fraction of the size
            im.im.quantize(32, method=Image.Quantize.FASTOCTREE).save(out_dir / src[5:], optimize=True)
            kit.provenance[src[5:]]['palette'] = 32
        srcs.append(src)
    return srcs


MT_LIST = title_set('l', 38, MUS_LIST_W)   # list rows: the native 38 px line box, no resampling
MT_BIG = title_set('b', 54, MUS_BIG_W)     # now playing
MT_MINI = title_set('m', 30, MUS_MINI_W)   # bottom-bar mini player
DISC_BIG = save(mus.disc(220), 'mus/disc220', {'source': 'generated (vinyl record)'})
DISC_BIG_I = save(mus.disc(220, idle=True), 'mus/disc220i', {'source': 'generated (vinyl record, idle)'})
DISC_S = save(mus.disc(64), 'mus/disc64', {'source': 'generated (vinyl record)'})
DISC_S_I = save(mus.disc(64, idle=True), 'mus/disc64i', {'source': 'generated (vinyl record, idle)'})
DISC_XS = save(mus.disc(40), 'mus/disc40', {'source': 'generated (vinyl record)'})
_logo = Kit.fit(mus.menu(0), height=132)
MUS_LOGO = save(_logo, 'mus/logo', {'source': mus.MENU_SPD, 'sprite_id': 0, 'note': 'MUSIC logo'})


def rim_panel(w, h, name, face=(0, 0, 0, 255), k=26):
    """Tilted P5 panel: red drop shadow, white rim, black face (4x supersampled)."""
    s = 4
    K = k * s
    para = lambda ox, oy, ww, hh: [(ox + K, oy), (ox + ww, oy), (ox + ww - K, oy + hh), (ox, oy + hh)]
    im = (Art.new(w * s, h * s)
          .polygon(para(10 * s, 10 * s, (w - 12) * s, (h - 12) * s), RED)
          .polygon(para(0, 0, (w - 12) * s, (h - 12) * s), (255, 255, 255, 255))
          .polygon(para(5 * s, 5 * s, (w - 22) * s, (h - 22) * s), face))
    return save(im.resize(w, h), name, {'source': 'generated (tilted rim panel)'})


# One MUSIC page per origin page (music_<origin>): its BACK returns to that origin. The runtime has
# no page history, and a tap runs one action: a flag/selection set by the mini-player tap cannot
# also switch the page (selections are cleared on a page switch; flag-driven page_binds would miss
# re-opens from the same page). Copies share every asset; only widget records are duplicated.
MUSIC_ORIGINS = {}  # music page id -> origin page id


def music_page(origin):
    pid = f'music_{origin}'
    MUSIC_ORIGINS[pid] = origin
    actions[f'page.{pid}'] = dict(kind='page', page=pid)
    mu = [rule()]
    mu.append(IMG(kit.shape('banner', 560, 96), 0, 34, 560, 96, color=RED))
    mu.append(IMG(MUS_LOGO, 20, 14, _logo.width, _logo.height))
    mu.append(T(1208, 36, bind_text='status', scale=3, color=RED, align='right'))
    mu.append(T(1208, 70, bind_text='live.date', scale=4, color=INK, align='right', need_bind='date.ready'))
    # ---- now playing
    NP_Y = 160
    mu.append(IMG(rim_panel(1200, 272, 'mus/np_panel'), 20, NP_Y, 1200, 272))
    mu.append(dict(type='image', src=DISC_BIG, rect=[60, NP_Y + 20, 220, 220], spin=45, need_bind=BGM_ON))
    mu.append(IMG(DISC_BIG_I, 60, NP_Y + 20, 220, 220, hide_bind=BGM_ON, keep_max=0))
    TX = 316
    mu.append(T(TX, NP_Y + 30, 'NOW PLAYING', 4, RED, need_bind=BGM_ON))
    mu.append(T(TX, NP_Y + 30, 'NO MUSIC', 4, DIM, hide_bind=BGM_ON, keep_max=0))
    mu.append(dict(type='image', src=MT_BIG[0], rect=[TX, NP_Y + 76, MUS_BIG_W, 54], bind='bgm.index',
                   src_names=MT_BIG, need_bind=BGM_KNOWN))
    mu.append(dict(type='label', rect=[TX, NP_Y + 84, MUS_BIG_W, 90], text='', bind_text='bgm.title',
                   text_scale=7, color=INK, wrap_width=MUS_BIG_W, max_lines=1, need_bind=BGM_NOIDX))
    mu.append(dict(type='value', rect=[TX, NP_Y + 84, 0, 40], bind='bgm.cue', text='BGM ', text_scale=7,
                   color=INK, need_bind=BGM_UNK))
    mu.append(T(TX, NP_Y + 88, 'The game is silent right now.', 5, DIM, need_bind='bgm.ready', hide_bind=BGM_ON,
                keep_max=0))
    mu.append(IMG(kit.shape('rect', 840, 3), TX, NP_Y + 158, 840, 3, color=RED))
    mu.append(T(TX, NP_Y + 180, 'TRACK', 4, RED, need_bind=BGM_KNOWN, hide_bind='bgm.index', keep_min=0))
    mu.append(V(TX + 116, NP_Y + 176, BGM_NUM, 5, INK, need_bind=BGM_KNOWN, hide_bind='bgm.index', keep_min=0))
    mu.append(T(TX + 196, NP_Y + 180, f'/ {NTRK}', 4, DIM, need_bind=BGM_KNOWN, hide_bind='bgm.index',
                keep_min=0))
    mu.append(T(TX + 360, NP_Y + 180, 'CUE', 4, RED, need_bind=BGM_ON))
    mu.append(V(TX + 432, NP_Y + 176, 'bgm.cue', 5, INK, need_bind=BGM_ON))
    mu.append(T(TX + 600, NP_Y + 180, 'PLAYING', 4, CYAN, need_bind=BGM_ON))
    mu.append(T(TX + 600, NP_Y + 180, 'STOPPED', 4, DIM, need_bind='bgm.ready', hide_bind='bgm.playing',
                keep_max=0))
    mu.append(T(TX, NP_Y + 180, 'Music info appears once the game reports it.', 4, DIM,
                hide_bind='bgm.ready', keep_max=0))
    # ---- track list
    LH_Y = 452
    mu.append(plate(24, LH_Y, 560, 44, color=BLACK, skew=10))
    mu.append(T(44, LH_Y + 12, f'THIEVES DEN  -  {NTRK} TRACKS', 4, INK))
    mu.append(T(1208, LH_Y + 14, 'Read-only: the game picks the music.', 3, DIM, align='right'))
    ML_Y, ML_H, ML_P = 504, 456, 56
    rid = region(pid, 'tracks', [24, ML_Y, 1192, ML_H], '', ML_P)
    lit = both(BGM_ON, 'bgm.known')
    lit_i = dict(hide_bind='bgm.index', keep_min='{i}', keep_max='{i}')
    mu += rp([plate(24, ML_Y, 1182, 50, color=TILE, skew=6),
              plate(24, ML_Y, 1182, 50, color=RED, skew=6, need_bind=lit, **lit_i),
              T(100, ML_Y + 15, '{i+1}', 4, DIM, align='right'),
              dict(type='image', src='file:ui/m/mus/l/{i}.png' if kit.mode == 'png' else 'module:p5r:l{i}',
                   rect=[124, ML_Y + 6, MUS_LIST_W, 38], color=INK),
              dict(type='image', src=DISC_XS, rect=[1150, ML_Y + 5, 40, 40], spin=60, need_bind=lit, **lit_i)],
             '', ML_P, NTRK, scroll=rid)
    page(pid, 'MUSIC', mu)

# ================================================================== assemble
MENU_PAGES = ['m_skill', 'm_item', 'm_equip', 'm_equipc', 'm_persona', 'm_stats', 'm_confidant',
              'm_cfdetail', 'm_request', 'm_calendar']
pages = [SELF['live'] if p['id'] == 'live' else p for p in m['pages'] if p['id'] not in MENU_PAGES]
pages += [SELF[pid] for pid in MENU_PAGES]

# ================================================================== bottom tab bar (every page)
# [ MENU | BACK ]  [ MAP ]  [ (disc) music mini player ]   -- one consistent 3-slot bar at BAR_Y.
#  left : MENU (pages outside the start menu: field / battle / analysis / social / dialogue -> hub),
#         BACK (start-menu sub-pages -> hub, candidates -> EQUIP; hub -> the game-state page:
#         analysis while the native analysis is open, battle in battle, else the field map;
#         music_<origin> page -> <origin>).
#  mid  : MAP (ui_open 20, the field map page); greyed while map.ready = 0.
#  right: mini player (bgm.*): spinning disc + the current title art, 'BGM <cue>' for an unknown cue,
#         idle look while bgm.ready/playing = 0 (also before the module publishes bgm.* at all).
#         A tap opens music_<this page> (plain page action: ui_open only accepts the module's known
#         page ids, and ui.page keeps the previous page's lazy lists alive while the music is shown).
# The 'waiting' (loading) page has no bar: nothing to navigate to before gameplay.
BAR_H = H - BAR_Y
SL_L, SL_M, SL_R = (16, 292), (320, 292), (624, 600)
BTN_Y, BTN_H = BAR_Y + 10, 84
BAR_BG = kit.shape('rect', W, BAR_H)
GREY = '#FF3A3236'
ORIGINS = list(PAGE_ID) + ['analysis', 'dialogue']
actions['page.analysis_nav'] = dict(kind='page', page='analysis')
actions['page.dialogue_nav'] = dict(kind='page', page='dialogue')
for _o in ORIGINS:
    music_page(_o)
    pages.append(SELF[f'music_{_o}'])


def bar_btn(slot, label, action, payload=None, gate=None):
    x, w = slot
    src = kit.shape('button', w, BTN_H)
    a = dict(on_tap=action)
    if payload is not None:
        a['payload'] = str(payload)
    if gate:
        a['need_bind'] = gate
    return [IMG(src, x, BTN_Y, w, BTN_H, color=RED, **a),
            T(x + w // 2, BTN_Y + (BTN_H - cap_px(6)) // 2, label, 6, INK, align='center', **a)]


def bar_off(slot, label, gate):
    """Disabled look of a slot (shown while `gate` is 0 / missing)."""
    x, w = slot
    src = kit.shape('button', w, BTN_H)
    return [IMG(src, x, BTN_Y, w, BTN_H, color=GREY, hide_bind=gate, keep_max=0),
            T(x + w // 2, BTN_Y + (BTN_H - cap_px(6)) // 2, label, 6, DIM, align='center',
              hide_bind=gate, keep_max=0)]


def left_slot(pid):
    if pid == 'live':  # hub BACK -> the page of the current game state
        m6 = dv('ui.gs.m6', cmp='eq', a='state.mode', b=6)
        n6 = dv('ui.gs.n6', cmp='ne', a='state.mode', b=6)
        nb = dv('ui.gs.nb', cmp='eq', a='battle.active', b=0)
        return (bar_off(SL_L, 'BACK', 'live.ready')
                + bar_btn(SL_L, 'BACK', 'page.analysis_nav', None, both('live.ready', m6))
                + bar_btn(SL_L, 'BACK', 'ui.open', PAGE_ID['battle'], both('live.ready', 'battle.active', n6))
                + bar_btn(SL_L, 'BACK', 'ui.open', PAGE_ID['field'], both('live.ready', nb, n6)))
    if pid in MUSIC_ORIGINS:  # BACK -> the page the mini player was tapped on
        o = MUSIC_ORIGINS[pid]
        if o in PAGE_ID:
            return bar_btn(SL_L, 'BACK', 'ui.open', PAGE_ID[o])
        return bar_btn(SL_L, 'BACK', f'page.{o}_nav')
    if pid == 'm_equipc':
        return bar_off(SL_L, 'BACK', 'live.ready') + bar_btn(SL_L, 'BACK', 'ui.open', PAGE_ID['m_equip'], 'live.ready')
    if pid in MENU_PAGES:
        return bar_off(SL_L, 'BACK', 'live.ready') + bar_btn(SL_L, 'BACK', 'ui.open', PAGE_ID['live'], 'live.ready')
    return bar_off(SL_L, 'MENU', 'live.ready') + bar_btn(SL_L, 'MENU', 'ui.open', PAGE_ID['live'], 'live.ready')


MINI_BG = rim_panel(SL_R[1], 92, 'mus/mini_panel', k=18)
MINI_BG_ON = rim_panel(SL_R[1], 92, 'mus/mini_panel_on', face=rgba('#FF3A0A0E'), k=18)


def mini_player(pid):
    x, w = SL_R
    y = BAR_Y + 6
    here = pid in MUSIC_ORIGINS
    out = [IMG(MINI_BG_ON if here else MINI_BG, x, y, w, 92),
           dict(type='image', src=DISC_S, rect=[x + 30, y + 10, 64, 64], spin=45, need_bind=BGM_ON),
           IMG(DISC_S_I, x + 30, y + 10, 64, 64, hide_bind=BGM_ON, keep_max=0),
           T(x + 110, y + 14, 'NOW PLAYING', 3, RED, need_bind=BGM_ON),
           T(x + 110, y + 14, 'MUSIC', 3, DIM, hide_bind=BGM_ON, keep_max=0),
           dict(type='image', src=MT_MINI[0], rect=[x + 110, y + 38, MUS_MINI_W, 30], bind='bgm.index',
                src_names=MT_MINI, need_bind=BGM_KNOWN),
           dict(type='label', rect=[x + 110, y + 40, MUS_MINI_W, 40], text='', bind_text='bgm.title',
                text_scale=5, color=INK, wrap_width=MUS_MINI_W, max_lines=1, need_bind=BGM_NOIDX),
           dict(type='value', rect=[x + 110, y + 40, MUS_MINI_W, 30], bind='bgm.cue', text='BGM ',
                text_scale=5, color=INK, need_bind=BGM_UNK),
           T(x + 110, y + 42, 'Not playing', 5, DIM, hide_bind=BGM_ON, keep_max=0)]
    if not here:
        tap = kit.shape('rect', w, 92, fill='#00000000')
        out.append(dict(type='image', src=tap, rect=[x, y, w, 92], color='#00000000',
                        on_tap=f'page.music_{pid}'))
    return out


for pg in pages:
    pid = pg['id']
    if pid == 'waiting':
        continue
    assert pid in ORIGINS or pid in MUSIC_ORIGINS, pid
    w = [IMG(BAR_BG, 0, BAR_Y, W, BAR_H, color='#FF0B0809'),
         IMG(kit.shape('rect', W, 4), 0, BAR_Y, W, 4, color=RED)]
    w += left_slot(pid)
    w += bar_off(SL_M, 'MAP', 'map.ready') + bar_btn(SL_M, 'MAP', 'ui.open', PAGE_ID['field'], 'map.ready')
    w += mini_player(pid)
    pg['widgets'] = pg['widgets'] + w
# Partial redraw dirties a widget's declared rect only: a bound left-aligned label/value with a zero
# width never repaints when just its text changes (row names after an item is used up, the drive
# status). Give those an explicit extent up to the canvas edge.
for pg in pages:
    if pg['id'] not in MENU_PAGES and pg['id'] != 'live':
        continue
    for w in pg['widgets']:
        r = w.get('rect')
        dyn = w.get('bind_text') or (w.get('type') == 'value' and w.get('bind'))
        if (w.get('type') in ('label', 'value') and dyn and r and r[2] == 0
                and w.get('align', 'left') == 'left' and not w.get('wrap_width')):
            r[2] = max(1, min(640 if w.get('type') == 'label' else 240, W - r[0]))
m['pages'] = pages
# ui.page binds first (the state.mode / controls.choice binds after them win a same-tick clash);
# the field/menu mode binds hold while a native-menu drive runs (it opens and closes the camp menu)
binds = [b for b in m.get('page_binds', []) if b.get('point') != 'ui.page']
for b in binds:
    if b.get('point') == 'state.mode' and b.get('equals') in (1, 2, 5):
        b['ready_bind'] = '!pdrv.busy'
m['page_binds'] = [dict(point='ui.page', equals=v, when_equal=dict(page=pid))
                   for pid, v in PAGE_ID.items()] + binds
outs = m.setdefault('module_outputs', [])
for k in ['pstat.ready', 'item.ready', 'item.tab.count', 'skill.ready', 'persona.ready', 'persona.count',
          'persona.current', 'request.ready', 'request.count', 'calendar.ready', 'calendar.month',
          'calendar.day', 'calendar.first_weekday', 'calendar.days_in_month', 'confidant.sel.ready',
          'confidant.sel.index', 'confidant.sel.id', 'confidant.sel.arcana', 'confidant.sel.rank',
          'confidant.sel.person', 'confidant.sel.portrait', 'confidant.sel.portrait_key',
          'confidant.sel.story', 'confidant.sel.profile', 'confidant.sel.ability.count',
          'ui.page', 'pdrv.busy', 'pdrv.idle', 'pdrv.show', 'pdrv.text', 'item.sel.key',
          'item.sel.can_use', 'skill.sel.member', 'skill.sel.can_use', 'persona.sel',
          'persona.sel.can_change', 'equip.sel', 'equip.cand.can', 'skill.sel',
          'bgm.ready', 'bgm.playing', 'bgm.cue', 'bgm.title', 'bgm.known', 'bgm.index', 'bgm.change',
          'calendar.view.month', 'calendar.view.first_weekday', 'calendar.view.days_in_month',
          'calendar.view.today', 'calendar.view.cut', 'calendar.view.can_prev', 'calendar.view.can_next',
          'calendar.sel.log.ready', 'calendar.sel.log.0.label', 'calendar.sel.log.0.red',
          'calendar.sel.log.1.label', 'calendar.sel.log.1.red', 'calendar.sel.log.bus',
          'calendar.sel.log.morgana', 'calendar.sel.log.done']:
    if k not in outs:
        outs.append(k)
man_path.write_text(json.dumps(m, ensure_ascii=False, separators=(',', ':')) + '\n')
kit.write_provenance()
n = sum(len(p['widgets']) for p in pages)
print(f'{len(pages)} pages, {n} widgets, {len(derived)} derived, {len(kit.provenance)} assets; '
      + ', '.join(f"{p['id']}={len(p['widgets'])}" for p in pages))
