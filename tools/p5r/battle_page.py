#!/usr/bin/env python3
"""BATTLE page v0.9 (ui_battle) -- TACTICAL board + FOLLOW view, read-only (no game actions).

Imported by build_gameart.py: `battle_page.build(kit, m)` returns the page widgets and registers the
derived values, runtime flags, flag actions and module outputs it needs in manifest `m`.

Module outputs used:
enemy.E.{present,name,level,hp,hp_max,sp,sp_max,down,ailment,aff.K}, bparty.P.{present,hp,hp_max,sp,
sp_max,down,ailment,persona.*,skill.K.*}, battle.turn.{side,actor}, battle.target.{side,slot}.
Everything the module does not publish yet stays hidden; the old party.P.* rows are the fallback for
the party tiles (face, name, HP/SP numbers).

Layout (canvas 1240 x 1080, bottom tab bar from y 972 added by build_menu.py):
  header   : BATTLE banner, status, TACTICAL | FOLLOW toggle (runtime flag bt.mode)
  TACTICAL : 5 enemy tiles (top) / DETAIL of the tapped tile (middle) / 4 party tiles (bottom,
             just above the tab bar) -- enemies and party are never adjacent (user, v0.9.2).
             Runtime flag bt.sel: 0..4 enemy E, 10..13 party P; the selected tile has a gold frame.
  FOLLOW   : an opaque full-body panel drawn over the tactical board while the game has a focus:
             the target of the target cursor (battle.target.*), else the acting unit
             (battle.turn.*). Without a focus (or before the module publishes it) the panel is
             hidden and the tactical board shows through = the fallback.

Missing-value rules the gating relies on (the Eden Duo runtime): a derived value is MISSING when any
source is missing (select only looks at the chosen branch); need_bind on a missing point hides;
hide_bind reads a missing point as 0.
"""
from __future__ import annotations

from pathlib import Path

import menu_art as art
import p5style
from p5style import BG, DIM, INK, RED, TILE, TILE_HI, Kit, cap_px
from recipe import Art

from p5r_paths import ROMFS  # noqa: E402,F401
EXTRA_SPD = {'btlbtn': 'EN/BATTLE/GUI/BTL_BTN.SPD', 'damage': 'EN/BATTLE/GUI/DAMAGE_DATA.SPD'}

BLACK = '#FF000000'
HP_NAT = '#FF66FDDC'   # native HUD HP teal (same sample as build_gameart / build_menu)
SP_NAT = '#FFFF64E8'   # native HUD SP pink
HP_BG = '#FF1F3F3A'
SP_BG = '#FF3F1F3A'
CYAN = '#FF3FE0E8'
GREY = '#FF3A3236'
GOLD = '#FFFFC21A'      # P5 yellow: selected-tile frame (red was too subtle, user v0.9.2)
GOLD_GLOW = '#66FFC21A'
W = 1240

# party member id -> P5_BATTLE_PARTYPANEL battle face (バトル顔); Futaba (8) never fights
FACE_SPRITE = {1: 61, 2: 62, 3: 63, 4: 64, 5: 65, 6: 66, 7: 67, 9: 68, 10: 177}
# element order phys gun fire ice elec wind psy nuke bless curse -> ICON.DDS cell of that element
ELEM_CELL = [19, 20, 21, 22, 24, 23, 28, 27, 25, 26]
# ailment id (battle2.md: status bit index + 1) -> BTL_BTN.SPD word plate (装弾数バステ_*); 173 is a
# second "Forget" plate, so Hunger and the ids without a plate get a text plate (AILMENT_TEXT).
AILMENT_SPRITE: dict[int, int] = {1: 166, 2: 167, 3: 168, 4: 169, 5: 170, 6: 171, 7: 172,
                                  9: 174, 10: 175, 11: 176, 12: 177}
AILMENT_TEXT = {8: 'HUNGER', 13: 'DESPERATION', 16: 'LUST', 17: 'WRATH', 18: 'ENVY', 19: 'VANITY'}


class Page:
    def __init__(self, kit: Kit, m: dict):
        self.kit, self.m = kit, m
        self.derived = m.setdefault('derived', [])
        self._names = {d['name'] for d in self.derived}
        self.flags = m.setdefault('flags', {})
        self.w: list = []
        for key, rel in EXTRA_SPD.items():
            p5style.SPD_SOURCES[key] = rel

    # ------------------------------------------------------------ derived helpers
    def dv(self, name, **kw):
        if name not in self._names:
            self._names.add(name)
            if 'else_' in kw:
                kw['else'] = kw.pop('else_')
            self.derived.append(dict(name=name, **kw))
        return name

    def eq(self, a, b):
        key = a.replace('@flag:', 'f.')
        return self.dv(f'bt.eq.{key}.{b}', cmp='eq', a=a, b=b)

    def both(self, *names):
        return self.dv('bt.and.' + '+'.join(names), all_nonzero=list(names))

    def flag(self, name, default):
        self.flags[name] = default
        act = f'{name}.set'
        self.m['actions'][act] = dict(kind='flag', flag=name, value='$payload')
        return act


def build(kit: Kit, m: dict) -> list:
    pg = Page(kit, m)
    dv, eq, both = pg.dv, pg.eq, pg.both
    T, IMG, V = Kit.text, Kit.image, Kit.value
    w = pg.w

    # ================================================================ art
    def face_set(size, prefix):
        srcs = [''] * 11
        for mid, sid in FACE_SPRITE.items():
            srcs[mid], _, _ = kit.sprite('partypanel', sid, f'bt/{prefix}{mid}', height=size,
                                         box=(size, size), note='battle face (バトル顔)')
        return srcs

    FACE_S, FACE_M, FACE_L = face_set(84, 'fs'), face_set(96, 'fm'), face_set(132, 'fl')

    def elem_colour(e):
        px = [q for q in art.icon_cell(ELEM_CELL[e]).getdata() if q[3] > 200 and max(q[:3]) > 90]
        return '#FF%02X%02X%02X' % tuple(sorted(c[i] for c in px)[len(px) // 2] for i in range(3))

    ECOL = [elem_colour(e) for e in range(10)]

    def elem_icons(h, box, prefix):
        # every set is addressed by a {i} template (packed weakness row, affinity rows): fixed ids
        # <prefix><e>. (1.0.2 gave only 'em' a fixed id; the 'ed'/'eb' rows then templated a generic
        # module id that has no '0.png' to replace, so every element drew icon 0 = PHYS.)
        return [kit.sprite('camp00', 319 + e, f'bt/{prefix}{e}', height=h, box=box, color=ECOL[e],
                           note='camp element icon tinted with the ICON.DDS element colour',
                           rid=f'{prefix}{e}')[0]
                for e in range(10)]

    def elem_template(icons):
        # 'file:ui/bt/ed0.png' -> 'file:ui/bt/ed{i}.png'; 'module:p5r:ed0' -> 'module:p5r:ed{i}'
        t = icons[0].replace('0.png', '{i}.png') if icons[0].endswith('.png') else icons[0][:-1] + '{i}'
        assert t != icons[0] and all(t.replace('{i}', str(e)) == icons[e] for e in range(10)), icons
        return t

    EL_MINI = elem_icons(24, (34, 26), 'em')        # enemy tiles: known weaknesses
    EL_MID = elem_icons(56, (104, 60), 'ed')        # tactical detail
    EL_BIG = elem_icons(80, (200, 86), 'eb')        # follow view (enemy)

    def aff_sets(bk_h, st_h, prefix):
        """Index = aff + 1: -1 unknown '?', 0 normal (dash, no backing), 1 Dr .. 5 Wk, 6 unknown
        (analysis encoding)."""
        backs, bw, bh = kit.sprite_set('camp00', [665, 666, 667, 668, 669, 670], f'bt/{prefix}b',
                                       height=bk_h)
        stamps, sw, sh = kit.sprite_set('camp00', [329, 330, 331, 332, 333, 485], f'bt/{prefix}s',
                                        height=st_h)
        # backings 665..670 = drain repel null resist weak unknown
        # native Analyze / persona status: a KNOWN normal affinity carries no stamp at all
        back = [backs[5], ''] + backs[0:5] + [backs[5]]
        stamp = [stamps[5], ''] + stamps[0:5] + [stamps[5]]
        return back, bw, bh, stamp, sw, sh

    AFF_M = aff_sets(44, 38, 'am')
    AFF_L = aff_sets(80, 66, 'al')

    AIL_M = [''] * 64
    AIL_S = [''] * 64
    for aid, sid in AILMENT_SPRITE.items():
        AIL_M[aid] = kit.sprite('btlbtn', sid, f'bt/ailm{aid}', height=46, box=(120, 46))[0]
        AIL_S[aid] = kit.sprite('btlbtn', sid, f'bt/ails{aid}', height=34, box=(90, 34))[0]
    def text_plate(word, box, name):
        # white word on a black skewed plate, fitted into the ailment box (no stretching)
        bw_, bh_ = box
        px = bh_ * 0.45
        im = kit.text_art(word, px)
        if im.width > bw_ - 12:
            im = kit.text_art(word, px * (bw_ - 12) / im.width)
        plate_ = kit.art_of[kit.shape('tile', bw_, bh_, skew=6)]
        # (was: paste black through the white plate's alpha onto a transparent box = recolour black)
        out = Art.new(*box).over(plate_.recolor(BLACK), 0, 0)
        out = out.over(im, (bw_ - im.width) // 2, (bh_ - im.height) // 2)
        return kit._save(out, name, {'source': 'EN/FONT/FONT0.FNT (pre-rendered)', 'text': word})

    for aid, word in AILMENT_TEXT.items():
        AIL_M[aid] = text_plate(word, (120, 46), f'bt/ailtm{aid}')
        AIL_S[aid] = text_plate(word, (90, 34), f'bt/ailts{aid}')
    AIL_M_WH, AIL_S_WH = (120, 46), (90, 34)
    WEAK, WKW, WKH = kit.sprite('damage', 95, 'bt/weak_mark', height=30,
                                note='DAMAGE_DATA "WEAK" hit mark: skill hits a known weakness')
    WEAKL, WKLW, WKLH = kit.sprite('damage', 95, 'bt/weak_mark_l', height=40)
    HP_T, HTW, HTH = kit.sprite('camp00', 261, 'bt/hp_t', height=26, color=HP_NAT)
    SP_T, STW, STH = kit.sprite('camp00', 262, 'bt/sp_t', height=26, color=SP_NAT)
    HP_TL, HTLW, HTLH = kit.sprite('camp00', 261, 'bt/hp_tl', height=36, color=HP_NAT)
    SP_TL, STLW, STLH = kit.sprite('camp00', 262, 'bt/sp_tl', height=36, color=SP_NAT)
    LV_S, LVW, LVH = kit.sprite('camp00', 337, 'bt/lv_s', height=26)
    LV_L, LVLW, LVLH = kit.sprite('camp00', 337, 'bt/lv_l', height=34)
    DOWN_S = kit.text_image('DOWN', 22, 'bt/down_s')
    DOWN_L = kit.text_image('DOWN', 30, 'bt/down_l')
    DOWN_XS = kit.text_image('DOWN', 16, 'bt/down_xs')
    KO_S = kit.text_image('KO', 22, 'bt/ko_s')
    KO_L = kit.text_image('KO', 30, 'bt/ko_l')
    # arcana list labels (P5CAMP_00SPD 379 + arcana; 403 World, 868/880 Faith/Councillor)
    ARC = {a: 379 + a for a in range(1, 22)}
    ARC.update({24: 403, 29: 868, 30: 880, 31: 868})
    ah = 40
    cells = {a: Kit.fit(kit.load_sprite('camp00', s), height=ah) for a, s in ARC.items()}
    arc_w = max(im.width for im in cells.values())
    ARC_SRC = [''] * 32
    for a, im in cells.items():
        ARC_SRC[a] = kit._save(Kit.pad(im, arc_w, ah, 'left'), f'bt/arc{a}',
                               {'source': 'EN/INIT/P5CAMP_00SPD.SPD', 'sprite_id': ARC[a],
                                'size': [arc_w, ah]})
    # ICON.DDS skill-type cells (index = cell + 1; only the skill cells 19..33 are shipped)
    ICW, ICH = 78, 26
    ICW_L, ICH_L = 102, 34
    SKI = [''] * 35
    SKI_L = [''] * 35
    for c in range(19, 34):
        cell = Art.icon(c)
        prov = {'source': 'EN/FONT/ICON.DDS', 'cell': c}
        SKI[c + 1] = kit._save(cell.resize(ICW, ICH), f'bt/ic{c}', prov)
        SKI_L[c + 1] = kit._save(cell.resize(ICW_L, ICH_L), f'bt/icl{c}', prov)
    clear = lambda ww, hh: kit.shape('rect', ww, hh, fill='#00000000')  # noqa: E731

    def ail(bind, srcs, x, y, wh, need):
        return [kit.by_value(bind, srcs, x, y, *wh, need=need)] if AILMENT_SPRITE else []

    def plate(x, y, ww, hh, color, skew=8, **kw):
        return IMG(kit.shape('tile', ww, hh, skew=skew), x, y, ww, hh, color=color, **kw)

    # ================================================================ state
    SEL = pg.flag('bt.sel', 0)       # 0..4 enemy E, 10..13 party P
    MODE = pg.flag('bt.mode', 0)     # 0 TACTICAL, 1 FOLLOW
    C0 = dv('bt.c0', terms=[], add=0)
    CM1 = dv('bt.cm1', terms=[], add=-1)
    FOLLOW = eq('@flag:bt.mode', 1)
    # FOLLOW focus: target cursor first, then the acting unit; party units are 10 + P
    ts1, ts2 = eq('battle.target.side', 1), eq('battle.target.side', 2)
    as1, as2 = eq('battle.turn.side', 1), eq('battle.turn.side', 2)
    tgt_a = dv('bt.tgt_a', terms=[['battle.target.slot', 1]], add=10)
    act_a = dv('bt.act_a', terms=[['battle.turn.actor', 1]], add=10)
    f3 = dv('bt.f3', select=as1, then=act_a, else_=CM1)
    f2 = dv('bt.f2', select=as2, then='battle.turn.actor', else_=f3)
    f1 = dv('bt.f1', select=ts1, then=tgt_a, else_=f2)
    f0 = dv('bt.f0', select=ts2, then='battle.target.slot', else_=f1)
    # battle2.md (m_battle2): battle.target.* is the native cursor while a unit is choosing
    # (battle.phase 1) and the action's targets while it acts (phase 2). FOLLOW shows the target
    # only while a PARTY member is choosing (command menu / skill or item list / target select);
    # while an action executes and on enemy turns it shows the acting unit (the attacking enemy,
    # not the party member it targets). Slot -2 = "all": follow the acting unit then.
    TSEL = dv('bt.tsel', all_nonzero=[dv('bt.tsany', any_nonzero=[ts1, ts2]),
                                      dv('bt.tsge', cmp='ge', a='battle.target.slot', b=0),
                                      as1, eq('battle.phase', 1)])
    FOCUS = dv('bt.focus', select=TSEL, then=f0, else_=f2)
    # the focused unit must exist (a stale cursor on a slot the game emptied would show a blank panel)
    fp = C0
    for code, pres in [(E, f'enemy.{E}.present') for E in range(5)] + \
                      [(10 + P, f'party.{P}.present') for P in range(4)]:
        fp = dv(f'bt.fp{code}', select=eq(FOCUS, code), then=pres, else_=fp)
    FVALID = dv('bt.fvalid', all_nonzero=[dv('bt.fge', cmp='ge', a=FOCUS, b=0), fp])
    FSHOW = both(FOLLOW, FVALID)
    TGT = TSEL
    FTARGET = both(FSHOW, TGT)
    FTURN = both(FSHOW, dv('bt.tgtnone', cmp='eq', a=TGT, b=0))
    FFOCUS = dv('bt.ffocus', select=FSHOW, then=FOCUS, else_=CM1)   # focus while FOLLOW shows it

    def fx(code):
        return both(FSHOW, eq(FOCUS, code))

    def tx(code, present):
        return both(eq('@flag:bt.sel', code), present)

    # a live enemy's KNOWN weakness per element (enemy.E.aff.e == 5 on any present enemy)
    WK = []
    for e in range(10):
        per = []
        for E in range(5):
            raw = dv(f'bt.ewr.{E}.{e}', cmp='eq', a=f'enemy.{E}.aff.{e}', b=5)
            per.append(dv(f'bt.ew.{E}.{e}', select=f'enemy.{E}.present', then=raw, else_=C0))
        WK.append(dv(f'bt.wk.{e}', any_nonzero=per))

    # ================================================================ header
    w.append(IMG(kit.shape('rect', W, 10), 0, 0, W, 10, color=RED))
    w.append(IMG(kit.shape('banner', 700, 92), 0, 24, 700, 92, color=RED))
    w.append(T(36, 70 - cap_px(7) // 2, 'BATTLE', 7))
    w.append(T(1208, 22, bind_text='status', scale=3, color=RED, align='right'))
    TGW, TGH, TGY = 214, 56, 58
    for i, (label, x) in enumerate((('TACTICAL', 776), ('FOLLOW', 1000))):
        w.append(IMG(kit.shape('button', TGW, TGH), x, TGY, TGW, TGH, color=GREY, on_tap=MODE,
                     payload=str(i)))
        w.append(IMG(kit.shape('button', TGW, TGH), x, TGY, TGW, TGH, color=RED,
                     hide_bind='@flag:bt.mode', keep_min=i, keep_max=i))
        w.append(T(x + TGW // 2, TGY + (TGH - cap_px(5)) // 2, label, 5, INK, align='center',
                   on_tap=MODE, payload=str(i)))

    TACTICAL = eq('@flag:bt.mode', 0)

    def sel_frame(x, y, ww, hh, code, pr):
        # gold frame (outer glow + solid 8 px border) around the selected tile. TACTICAL only: the
        # FOLLOW panel starts at y 124 but the enemy-row frame reaches up to y 117, so 1.0.2 left a
        # gold strip above the FOLLOW view (the frame was gated on the selection flag alone).
        g = dict(need_bind=both(pr, TACTICAL), hide_bind='@flag:bt.sel', keep_min=code, keep_max=code)
        return [plate(x - 11, y - 11, ww + 22, hh + 22, GOLD_GLOW, skew=14, **g),
                plate(x - 7, y - 7, ww + 14, hh + 14, GOLD, skew=12, **g)]

    # ================================================================ TACTICAL: enemy tiles
    EY, EH, EW, EP = 128, 206, 232, 242
    for E in range(5):
        x = 20 + E * EP
        pr = f'enemy.{E}.present'
        p = f'enemy.{E}.'
        w += sel_frame(x, EY, EW, EH, E, pr)
        w.append(kit.tile(x, EY, EW, EH, need=pr, skew=10))
        w.append(dict(type='label', rect=[x + 18, EY + 12, EW - 30, 60], text='', bind_text=p + 'name',
                      text_scale=4, color=INK, wrap_width=EW - 34, max_lines=2, line_gap=4, need_bind=pr))
        w.append(IMG(LV_S, x + 18, EY + 74, LVW, LVH, need_bind=pr))
        w.append(V(x + 24 + LVW, EY + 72, p + 'level', 4, INK, need_bind=pr))
        w.append(IMG(DOWN_S[0], x + EW - DOWN_S[1] - 16, EY + 66, DOWN_S[1], DOWN_S[2], color=RED,
                     need_bind=both(pr, p + 'down')))
        w.append(dict(type='bar', rect=[x + 18, EY + 106, EW - 40, 10], bind=p + 'hp',
                      max_bind=p + 'hp_max', bg=HP_BG, color=HP_NAT, need_bind=both(pr, p + 'hp_max')))
        w.append(IMG(HP_T, x + 18, EY + 124, HTW, HTH, need_bind=pr))
        w.append(V(x + 24 + HTW, EY + 124, p + 'hp', 4, HP_NAT, need_bind=pr))
        w += ail(p + 'ailment', AIL_S, x + EW - AIL_S_WH[0] - 12, EY + 120, AIL_S_WH, pr)
        # known weaknesses, packed (only elements whose aff is Wk)
        w.append(dict(type='image', src='file:ui/bt/em{i}.png' if kit.mode == 'png' else 'module:p5r:em{i}', rect=[x + 16, EY + 152, 34, 26],
                      color=INK, repeat=10, repeat_cols=6, repeat_dx=35, repeat_row_dy=27, pack=True,
                      hide_bind=f'enemy.{E}.aff.{{i}}', keep_min=5, keep_max=5, need_bind=pr))
        w.append(dict(type='image', src=clear(EW, EH), rect=[x, EY, EW, EH], color='#00000000',
                      on_tap=SEL, payload=str(E), need_bind=pr))
    w.append(T(620, EY + 90, 'No enemy data yet.', 4, DIM, align='center', hide_bind='enemy.count',
               keep_max=0))

    # ================================================================ TACTICAL: party tiles
    PY, PH, PW, PP = 832, 130, 290, 300   # bottom row, just above the tab bar (y 972)
    for P in range(4):
        x = 20 + P * PP
        pr = f'party.{P}.present'
        b = f'bparty.{P}.'
        w += sel_frame(x, PY, PW, PH, 10 + P, pr)
        w.append(kit.tile(x, PY, PW, PH, need=pr, skew=10))
        w.append(kit.by_value(f'party.{P}.id', FACE_S, x + 12, PY + 24, 84, 84, need=pr))
        w.append(T(x + 104, PY + 12, bind_text=f'party.{P}.name', scale=4, need_bind=pr))
        # fallback numbers (party.P.*), covered by the bparty layer once it is published
        w.append(IMG(HP_T, x + 104, PY + 46, HTW, HTH, need_bind=pr))
        w.append(V(x + PW - 22, PY + 44, f'party.{P}.hp', 5, HP_NAT, align='right', need_bind=pr))
        w.append(IMG(SP_T, x + 104, PY + 84, STW, STH, need_bind=pr))
        w.append(V(x + PW - 30, PY + 82, f'party.{P}.sp', 5, SP_NAT, align='right', need_bind=pr))
        bp = both(pr, b + 'present')
        w.append(IMG(kit.shape('rect', PW - 112, PH - 44), x + 100, PY + 44, PW - 112, PH - 44,
                     color=TILE, need_bind=bp))
        for yy, stat, ink, bg, tag in ((46, 'hp', HP_NAT, HP_BG, (HP_T, HTW, HTH)),
                                       (84, 'sp', SP_NAT, SP_BG, (SP_T, STW, STH))):
            w.append(IMG(tag[0], x + 104, PY + yy, tag[1], tag[2], need_bind=bp))
            w.append(V(x + PW - 22, PY + yy - 2, b + stat, 5, ink, align='right',
                       need_bind=bp))
            w.append(dict(type='bar', rect=[x + 104, PY + yy + 29, PW - 136, 5], bind=b + stat,
                          max_bind=b + stat + '_max', bg=bg, color=ink,
                          need_bind=both(bp, b + stat + '_max')))
        w.append(IMG(DOWN_XS[0], x + 14, PY + PH - DOWN_XS[2] - 4, DOWN_XS[1], DOWN_XS[2], color=RED,
                     need_bind=both(bp, b + 'down')))
        # bparty.P.ko (battle2.md: status bit 19 or HP 0) -- incapacitated, the native HUD greys it
        w.append(IMG(kit.shape('tile', PW, PH, skew=10), x, PY, PW, PH, color='#99000000',
                     need_bind=both(bp, b + 'ko')))
        w.append(IMG(KO_S[0], x + 14, PY + PH - KO_S[2] - 6, KO_S[1], KO_S[2], color=RED,
                     need_bind=both(bp, b + 'ko')))
        w += ail(b + 'ailment', AIL_S, x + 8, PY + PH - AIL_S_WH[1] - 6, AIL_S_WH, bp)
        w.append(dict(type='image', src=clear(PW, PH), rect=[x, PY, PW, PH], color='#00000000',
                      on_tap=SEL, payload=str(10 + P), need_bind=pr))

    # ================================================================ DETAIL builders
    def aff_row(prefix, x0, y0, pitch, icons, iw, ih, sets, gate, cols=10, row_dy=0):
        back, bw, bh, stamp, sw, sh = sets
        # stamps only once the module publishes the row (a missing value would read as 0 = normal)
        has = dv(f'bt.has.{prefix}aff', cmp='ge', a=f'{prefix}aff.0', b=-1)
        sg = both(gate, has)
        # one repeat template per layer (element e = {i}); keeps the manifest small (4 MiB cap)
        rp = dict(repeat=10, repeat_cols=cols, repeat_dx=pitch, repeat_row_dy=row_dy)
        bnd = f'{prefix}aff.{{i}}'
        cx = x0 + (pitch - 12) // 2
        return [dict(type='image', src=elem_template(icons),
                     rect=[x0 + (pitch - 12 - iw) // 2, y0, iw, ih], color=INK, need_bind=gate, **rp),
                dict(type='image', src=back[0], rect=[cx - bw // 2, y0 + ih + 2, bw, bh], bind=bnd,
                     add=1, src_names=back, color=RED, need_bind=sg, **rp),
                dict(type='image', src=stamp[0], rect=[cx - sw // 2, y0 + ih + 2 + (bh - sh) // 2, sw, sh],
                     bind=bnd, add=1, src_names=stamp, color=INK, need_bind=sg, **rp)]

    def cover(y0, gate, big):
        # opaque backing: hides the 'tap a tile' hint under a shown detail
        hh = 780 if big else 450
        return IMG(kit.shape('rect', 1150, hh), 36, y0 + 10, 1150, hh, color=BLACK, need_bind=gate)

    def enemy_detail(E, y0, gate, big):
        p = f'enemy.{E}.'
        out = [cover(y0, gate, big)]
        ns = 8 if big else 7
        out.append(dict(type='label', rect=[44, y0 + 16, 900, cap_px(ns) + 20], text='', bind_text=p + 'name',
                        text_scale=ns, color=INK, wrap_width=900, max_lines=1, need_bind=gate))
        lv, lw, lh = (LV_L, LVLW, LVLH) if big else (LV_S, LVW, LVH)
        out.append(IMG(lv, 990, y0 + 30, lw, lh, need_bind=gate))
        out.append(V(1000 + lw, y0 + 22, p + 'level', 7 if big else 6, INK, need_bind=gate))
        ry = y0 + (110 if big else 92)
        dn = DOWN_L if big else DOWN_S
        out.append(IMG(dn[0], 44, ry, dn[1], dn[2], color=RED, need_bind=both(gate, p + 'down')))
        out += ail(p + 'ailment', AIL_M, 60 + dn[1], ry - 8, AIL_M_WH, gate)
        hy = ry + (60 if big else 48)
        vs = 6 if big else 5
        for x0, stat, ink, bg, tag in ((44, 'hp', HP_NAT, HP_BG, (HP_TL, HTLW, HTLH)),
                                       (640, 'sp', SP_NAT, SP_BG, (SP_TL, STLW, STLH))):
            q = p + stat
            out.append(IMG(tag[0], x0, hy + 4, tag[1], tag[2], need_bind=gate))
            out.append(dict(type='value', rect=[x0 + tag[1] + 16, hy, 400, cap_px(vs) + 12], bind=q,
                            max_bind=q + '_max', max_sep=' / ', text='', text_scale=vs, color=ink,
                            need_bind=both(gate, q + '_max')))
            out.append(V(x0 + tag[1] + 16, hy, q, vs, ink, hide_bind=q + '_max', keep_max=0,
                         need_bind=gate))
            out.append(dict(type='bar', rect=[x0, hy + 50, 540, 12], bind=q, max_bind=q + '_max', bg=bg,
                            color=ink, need_bind=both(gate, q + '_max')))
        ay = hy + (100 if big else 84)
        if big:
            out += aff_row(p, 30, ay, 238, EL_BIG, 200, 86, AFF_L, gate, cols=5, row_dy=200)
            ly = ay + 410
        else:
            out += aff_row(p, 26, ay, 119, EL_MID, 104, 60, AFF_M, gate)
            ly = ay + 120
        out.append(T(44, ly, '?  = not yet discovered (the game\'s own Analyze knowledge)', 3, DIM,
                     need_bind=both(gate, dv(f'bt.has.{p}aff', cmp='ge', a=f'{p}aff.0', b=-1))))
        return out

    # WEAK marks: one image per element e, drawn only where the skill's ICON.DDS cell is that
    # element's cell (src_names by icon value) and only while e is a known weakness of a live enemy
    # (need WK[e]); the variant gate rides on hide_bind (selection flag / follow focus == 10 + P).
    def weak_marks(src, w_, h_):
        return [[src if c == ELEM_CELL[e] else '' for c in range(40)] for e in range(10)]

    WEAK_N = weak_marks(WEAK, WKW, WKH)
    WEAK_L = weak_marks(WEAKL, WKLW, WKLH)
    TAG_N = [kit.sprite('camp00', 284, 'bt/cost_sp', height=26, color=SP_NAT)[0],
             kit.sprite('camp00', 285, 'bt/cost_hp', height=26, color=HP_NAT)[0]]
    TAG_L = [kit.sprite('camp00', 284, 'bt/cost_spl', height=34, color=SP_NAT)[0],
             kit.sprite('camp00', 285, 'bt/cost_hpl', height=34, color=HP_NAT)[0]]
    TAGW = [kit.art_of[t].size for t in (TAG_N[0], TAG_L[0])]

    def ally_detail(P, y0, gate, big, selv):
        pr = f'party.{P}.'
        b = f'bparty.{P}.'
        bg_ = both(gate, b + 'present')
        hasp = dv(f'bt.hasp.{P}', cmp='ge', a=b + 'persona.level', b=1)
        pg_ = both(bg_, hasp)   # persona row published
        out = [cover(y0, gate, big)]
        fs, faces = (132, FACE_L) if big else (96, FACE_M)
        out.append(kit.by_value(pr + 'id', faces, 40, y0 + 14, fs, fs, need=gate))
        nx = 40 + fs + 20
        out.append(T(nx, y0 + 18, bind_text=pr + 'name', scale=7 if big else 6, need_bind=gate))
        # HP / SP: party.P numbers underneath, bparty.P (with max + bars) over them when published
        vs = 6 if big else 5
        for k, (stat, ink, bg, tag) in enumerate((('hp', HP_NAT, HP_BG, (HP_TL, HTLW, HTLH)),
                                                  ('sp', SP_NAT, SP_BG, (SP_TL, STLW, STLH)))):
            yy = y0 + (80 if big else 70) + k * (70 if big else 50)
            out.append(IMG(tag[0], nx, yy + 4, tag[1], tag[2], need_bind=gate))
            out.append(V(nx + tag[1] + 16, yy, pr + stat, vs, ink, need_bind=gate))
            out.append(IMG(kit.shape('rect', 420, 56), nx + tag[1] + 8, yy - 4, 420, 56, color=BLACK,
                           need_bind=bg_))
            out.append(dict(type='value', rect=[nx + tag[1] + 16, yy, 400, cap_px(vs) + 12], bind=b + stat,
                            max_bind=b + stat + '_max', max_sep=' / ', text='', text_scale=vs, color=ink,
                            need_bind=bg_))
            out.append(dict(type='bar', rect=[nx + tag[1] + 16, yy + cap_px(vs) + (16 if big else 8), 380, 6],
                            bind=b + stat,
                            max_bind=b + stat + '_max', bg=bg, color=ink,
                            need_bind=both(bg_, b + stat + '_max')))
        dn = DOWN_L if big else DOWN_S
        out.append(IMG(dn[0], 40, y0 + 18 + fs, dn[1], dn[2], color=RED, need_bind=both(bg_, b + 'down')))
        ko = KO_L if big else KO_S
        out.append(IMG(ko[0], 40, y0 + 18 + fs, ko[1], ko[2], color=RED, need_bind=both(bg_, b + 'ko')))
        out += ail(b + 'ailment', AIL_M, 40, y0 + fs + (54 if big else 46), AIL_M_WH, bg_)
        # persona (right column)
        px = 660
        out.append(T(px, y0 + 18, 'PERSONA', 3, RED, need_bind=pg_))
        out.append(dict(type='image', src=ARC_SRC[1], rect=[px, y0 + 44, arc_w, ah], bind=b + 'persona.arcana',
                        src_names=ARC_SRC, color=CYAN, need_bind=pg_))
        out.append(IMG(LV_S, px, y0 + 98, LVW, LVH, need_bind=pg_))
        out.append(V(px + LVW + 8, y0 + 94, b + 'persona.level', 5, CYAN, need_bind=pg_))
        out.append(dict(type='label', rect=[px + LVW + 70, y0 + 96, 1200 - px - LVW - 70, 40], text='',
                        bind_text=b + 'persona.name', text_scale=5, color=CYAN,
                        wrap_width=1200 - px - LVW - 70, max_lines=1, need_bind=pg_))
        ay = y0 + (250 if big else 172)
        out += aff_row(b + 'persona.', 26, ay, 119, EL_MID, 104, 60, AFF_M, pg_)
        # skills: native battle SKILL rows (icon, name, cost) + WEAK mark on known weaknesses
        sy = ay + 124
        rows, cols = (8, 1) if big else (4, 2)
        rh = 50 if big else 40
        cw = 1180 if big else 590
        sc = 5 if big else 4
        iw, ih, icons = (ICW_L, ICH_L, SKI_L) if big else (ICW, ICH, SKI)
        wk, wkw, wkh = (WEAKL, WKLW, WKLH) if big else (WEAK, WKW, WKH)
        wkn = WEAK_L if big else WEAK_N
        tags, (tw, th) = (TAG_L, TAGW[1]) if big else (TAG_N, TAGW[0])
        # skill rows = repeat templates over K ({i}); row K exists while skill.count >= K + 1
        rpt = dict(repeat=8, repeat_cols=cols, repeat_dx=cw, repeat_row_dy=rh)
        for K in (0,):
            x, y = 30, sy
            q = f'{b}skill.{{i}}.'
            rg = dict(need_bind=bg_, hide_bind=b + 'skill.count', keep_min='{i}+1', **rpt)
            out.append(plate(x, y, cw - 14, rh - 6, TILE, skew=6, **rg))
            out.append(dict(type='image', src=icons[20], rect=[x + 10, y + (rh - 6 - ih) // 2, iw, ih],
                            bind=q + 'icon', add=1, src_names=icons, color=INK, **rg))
            out.append(dict(type='label', rect=[x + iw + 20, y + (rh - 6 - cap_px(sc)) // 2, cw - iw - 260,
                                                cap_px(sc) + 14], text='', bind_text=q + 'name', text_scale=sc,
                            color=INK, wrap_width=cw - iw - 260, max_lines=1, **rg))
            for e in range(10):
                out.append(dict(type='image', src=wk, rect=[x + cw - 150 - wkw, y + (rh - 6 - wkh) // 2, wkw, wkh],
                                bind=q + 'icon', src_names=wkn[e], color=INK, need_bind=WK[e],
                                hide_bind=selv[0], keep_min=selv[1], keep_max=selv[1], **rpt))
            cy = y + (rh - 6 - cap_px(sc)) // 2
            out.append(V(x + cw - 30 - tw - 6, cy, q + 'cost', sc, INK, align='right', **rg))
            out.append(dict(type='image', src=tags[0], rect=[x + cw - 30 - tw, cy + cap_px(sc) - th + 2, tw, th],
                            bind=q + 'cost_hp', src_names=tags, color=INK, **rg))
        out.append(T(620, ay + 40, 'Persona and skills appear once the game reports them.', 4, DIM,
                     align='center', need_bind=gate, hide_bind=hasp, keep_max=0))
        return out

    # ================================================================ TACTICAL: detail
    DY = 348   # between the enemy row (ends 334) and the party row (starts 832)
    w.append(plate(16, DY, 1208, 468, BLACK, skew=14))
    w.append(IMG(kit.shape('rect', 1176, 3), 32, DY + 6, 1176, 3, color=RED))
    w.append(T(620, DY + 200, 'Tap an enemy or an ally for details.', 5, DIM, align='center'))
    for E in range(5):
        w += enemy_detail(E, DY, tx(E, f'enemy.{E}.present'), False)
    for P in range(4):
        w += ally_detail(P, DY, tx(10 + P, f'party.{P}.present'), False, ('@flag:bt.sel', 10 + P))

    # ================================================================ FOLLOW panel
    FY = 124
    w.append(IMG(kit.shape('rect', W, 972 - FY), 0, FY, W, 972 - FY, color=BG, need_bind=FSHOW))
    w.append(plate(16, FY + 4, 1208, 972 - FY - 12, BLACK, skew=14, need_bind=FSHOW))
    w.append(IMG(kit.shape('rect', 1176, 3), 32, FY + 10, 1176, 3, color=RED, need_bind=FSHOW))
    for E in range(5):
        w += enemy_detail(E, FY + 44, fx(E), True)
    for P in range(4):
        w += ally_detail(P, FY + 44, fx(10 + P), True, (FFOCUS, 10 + P))
    w.append(T(44, FY + 24, 'TARGET', 4, RED, need_bind=FTARGET))
    w.append(T(44, FY + 24, 'ACTING NOW', 4, CYAN, need_bind=FTURN))
    # FOLLOW chosen but the game gives no focus yet: say so over the tactical board header
    w.append(T(712, 22, 'follow: waiting for a turn', 3, DIM,
               need_bind=FOLLOW, hide_bind=FSHOW, keep_max=0))

    # ================================================================ outputs
    outs = m.setdefault('module_outputs', [])
    keys = ['battle.turn.side', 'battle.turn.actor', 'battle.target.side', 'battle.target.slot',
            'battle.phase', 'enemy.count']
    for E in range(5):
        keys += [f'enemy.{E}.{f}' for f in ('present', 'name', 'level', 'hp', 'hp_max', 'sp', 'sp_max',
                                             'down', 'ailment')]
        keys += [f'enemy.{E}.aff.{k}' for k in range(10)]
    for P in range(4):
        keys += [f'bparty.{P}.{f}' for f in ('present', 'roster', 'eroster', 'id', 'name', 'hp', 'sp',
                                              'hp_max', 'sp_max', 'down', 'ko', 'ailment', 'persona.name',
                                              'persona.level', 'persona.arcana', 'skill.count')]
        keys += [f'bparty.{P}.persona.aff.{k}' for k in range(10)]
        keys += [f'bparty.{P}.skill.{K}.{f}' for K in range(8) for f in ('name', 'icon', 'cost', 'cost_hp')]
    for k in keys:
        if k not in outs:
            outs.append(k)
    return w
