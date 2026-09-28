#!/usr/bin/env python3
"""Restyle the P5R companion in the game's own art (GAMEART pass).

usage: build_gameart.py <input manifest.json> <output dualscreen dir> [trimmed FONT0.FNT]

Takes the canonical manifest (whatever build_layout/build_battle_layout/build_maps produced),
keeps every non-page key (module, page_binds, actions, map, outputs) and rebuilds the pages with
p5style: same binds, same actions, same gates. Adds the SOCIAL page + `page.social` action and the
game font. Idempotent: rerun after the canonical generators.
"""
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from p5style import (BG, DIM, INK, RED, TILE, TILE_HI, Kit, cap_px, RECIPE_FILE)  # noqa: E402
from recipe import Art  # noqa: E402

src_manifest, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
fnt = Path(sys.argv[3]) if len(sys.argv) > 3 else Path('FONT0_320.FNT')  # png mode only (trim_fnt.py output)
m = json.loads(src_manifest.read_text())
old_pages = {p['id']: p for p in m['pages']}
ui = out_dir / 'ui'
if ui.exists():
    shutil.rmtree(ui)
if (out_dir / 'd').exists():
    shutil.rmtree(out_dir / 'd')
if (out_dir / RECIPE_FILE).exists():
    (out_dir / RECIPE_FILE).unlink()
kit = Kit(out_dir, fresh_table=True)
if kit.mode == 'png':
    (ui / 'font').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(fnt, ui / 'font/FONT0.FNT')
    kit.provenance['ui/font/FONT0.FNT'] = {
        'source': 'EN/FONT/FONT0.FNT', 'note': 'first 320 glyphs (ASCII + signs), same .FNT format; '
        'tools/trim_fnt.py; decoded at runtime by the P5R module (p5r_font.h)'}
    m['font'] = 'file:ui/font/FONT0.FNT'
    m['font_atlas'] = 'module:p5r_font:file:ui/font/FONT0.FNT'
else:
    # Asset-free: the host reads `font` bytes and hands them to the module's decode_font; the module
    # sees the recipe table's magic and decodes the game's own EN/FONT/FONT0.FNT from romfs instead.
    m['font'] = 'file:' + RECIPE_FILE
    m['font_atlas'] = 'module:p5r_font:romfs:EN/FONT/FONT0.FNT'
m['background'] = BG

T = kit.text
IMG = kit.image

# ------------------------------------------------------------------ shared art
MEMBER_HEAD = {1: 252, 2: 253, 3: 254, 4: 255, 5: 256, 6: 257, 7: 258, 9: 259}  # camp portraits


def heads(prefix, size):
    """src list indexed by party id 0..10 (Futaba 8 has no camp portrait; Kasumi 10 = battle face)."""
    srcs = [''] * 11
    for mid, sid in MEMBER_HEAD.items():
        srcs[mid], _, _ = kit.sprite('camp00', sid, f'{prefix}{mid}', box=(size, size), height=size)
    srcs[10], _, _ = kit.sprite('partypanel', 177, f'{prefix}10', box=(size, size), height=size,
                                note='Kasumi battle face (no camp portrait exists)')
    return srcs


HEAD_L = heads('hl', 132)
HEAD_S = heads('hs', 92)
HP_TAG, HPW, HPH = kit.sprite('camp00', 285, 'hp_tag', height=44)
SP_TAG, SPW, SPH = kit.sprite('camp00', 284, 'sp_tag', height=44)
HP_SM, HPSW, HPSH = kit.sprite('camp00', 261, 'hp_sm', height=28)
SP_SM, SPSW, SPSH = kit.sprite('camp00', 262, 'sp_sm', height=28)
LV_SM, LVW, LVH = kit.sprite('camp00', 337, 'lv_sm', height=34)
LV_BIG, LVBW, LVBH = kit.sprite('camp00', 335, 'lv_big', height=62)


# Native HUD HP / SP ink, sampled from the game's own camp STATS list and field party HUD
# (DATA13 native captures: HP digits/bars (102,253,220), SP (255,100,232)).
HP_NAT = '#FF66FDDC'
SP_NAT = '#FFFF64E8'
HP_SMN, _, _ = kit.sprite('camp00', 261, 'hp_sm_nat', height=28, color=HP_NAT)
SP_SMN, _, _ = kit.sprite('camp00', 262, 'sp_sm_nat', height=28, color=SP_NAT)
# The strip shows only in a dungeon (state.mode 2 = the game's FLD_CHECK_DUNGEON range), in battle
# (3) or the battle analysis (6): never in town (1) nor during dialogue (4, also mid-battle talks).
_strip_modes = []
for _v in (2, 3, 6):
    _n = f'ga.mode.{_v}'
    m.setdefault('derived', []).append(dict(name=_n, cmp='eq', a='state.mode', b=_v))
    _strip_modes.append(_n)
m['derived'].append(dict(name='ga.strip', any_nonzero=_strip_modes))
for _i in range(4):
    m['derived'].append(dict(name=f'ga.strip.{_i}', all_nonzero=['ga.strip', f'party.{_i}.present']))


def party_strip(y=826):
    """Compact 4-member strip (dungeon / battle only): portrait, name, HP/SP in the native teal/pink."""
    w = []
    for i in range(4):
        x = 32 + i * 298
        g = f'ga.strip.{i}'
        w.append(kit.tile(x, y, 284, 122, need=g, skew=10))
        w.append(kit.by_value(f'party.{i}.id', HEAD_S, x + 10, y + 16, 92, 92, need=g))
        w.append(T(x + 108, y + 14, bind_text=f'party.{i}.name', scale=4, need_bind=g))
        # font digits here: sprite digits cost a 1110-entry table per number (see p5style.number)
        w.append(IMG(HP_SMN, x + 108, y + 54, HPSW, HPSH, need_bind=g))
        w.append(kit.value(x + 262, y + 56, f'party.{i}.hp', 5, HP_NAT, align='right', need_bind=g))
        w.append(IMG(SP_SMN, x + 108, y + 88, SPSW, SPSH, need_bind=g))
        w.append(kit.value(x + 262, y + 90, f'party.{i}.sp', 5, SP_NAT, align='right', need_bind=g))
    return w


def hbtn(x, label, action, gate, w=204):
    """Compact page-action button in the header band (right of the title banner, under the status
    line). The bottom tab bar (MENU / MAP / music, build_menu.py) replaced the old 3-slot nav row, so
    page-specific actions (native-menu drivers, ANALYZE, CLOSE) live here, inside the page."""
    return kit.button(x, 84, w, 44, label, action, gate, scale=4)


pages = []

# ------------------------------------------------------------------ PARTY (live)
live = kit.header('PARTY')
live.append(T(40, 138, bind_text='live.date', scale=6, need_bind='date.ready'))
MONEY, MW, MH = kit.sprite('money', 32, 'money_word', height=60)
live.append(IMG(MONEY, 36, 190, MW, MH, need_bind='live.ready'))
live.append(kit.value(58 + MW, 200, 'live.money', 8, INK, prefix='¥', need_bind='live.ready'))
LEADER, LDW, LDH = kit.sprite('camp00', 493, 'leader', height=40)
HEAD_FRAME, HFW, HFH = kit.sprite('camp00', 260, 'head_frame', width=150)
for i in range(4):
    y = 284 + i * 166
    g = f'party.{i}.present'
    live.append(kit.tile(32, y, 1176, 152, need=g, skew=14))
    live.append(IMG(HEAD_FRAME, 40, y + 8, HFW, HFH, need_bind=g))
    live.append(kit.by_value(f'party.{i}.id', HEAD_L, 50, y + 10, 132, 132, need=g))
    live.append(T(206, y + 22, bind_text=f'party.{i}.name', scale=6, need_bind=g))
    live.append(IMG(LV_SM, 206, y + 92, LVW, LVH, need_bind=g))
    live += kit.number(f'party.{i}.level', 206 + LVW + 96, y + 80, 'm', 2, need=g, height=50,
                       fallback_scale=6)
    live.append(IMG(HP_TAG, 400, y + 84, HPW, HPH, need_bind=g))
    live += kit.number(f'party.{i}.hp', 400 + HPW + 190, y + 66, 'b', 3, need=g, height=72)
    live.append(IMG(SP_TAG, 740, y + 84, SPW, SPH, need_bind=g))
    live += kit.number(f'party.{i}.sp', 740 + SPW + 190, y + 66, 'b', 3, need=g, height=72)
live.append(IMG(LEADER, 1188 - LDW, 284 + 18, LDW, LDH, need_bind='party.0.present'))
pages.append(dict(id='live', title='PARTY', widgets=live))

# ------------------------------------------------------------------ DIALOGUE (choices only)
# The page only comes up while a choice is open (page_binds below). The prompt line is shown small;
# each choice is a big button. Tapping the highlighted choice confirms; tapping another moves the
# game's cursor straight to it. The move is computed from the LIVE cursor: every button carries one
# invisible tap area per cursor position s (visible only while dialogue.selected == s) whose action
# presses Down/Up exactly |i - s| times, so the physical D-pad never desynchronises it.
talk = [IMG(kit.shape('rect', 1240, 10), 0, 0, 1240, 10, color=RED)]
talk.append(T(40, 40, bind_text='dialogue.speaker', scale=4, color=RED, need_bind='dialogue.has_speaker'))
talk.append(dict(type='label', rect=[40, 84, 1160, 150], text='', bind_text='dialogue.text',
                 text_scale=4, color=DIM, wrap_width=1160, max_lines=4, line_gap=10,
                 need_bind='dialogue.ready'))
MAXC = 8
BY, BH, BG_ = 250, 92, 16
btn = kit.shape('tile', 1176, BH, fill='#FFFFFFFF', skew=10)
clear = kit.shape('rect', 1176, BH, fill='#00000000')
for i in range(MAXC):
    y = BY + i * (BH + BG_)
    g = f'choice.{i}.present'
    talk.append(IMG(btn, 32, y, 1176, BH, color=TILE_HI, need_bind=g))
    talk.append(IMG(btn, 32, y, 1176, BH, color=RED, need_bind=f'choice.{i}.selected'))
    talk.append(dict(type='label', rect=[72, y + (BH - cap_px(6)) // 2, 1100, cap_px(6) + 18], text='',
                     bind_text=f'choice.{i}.text', text_scale=6, color=INK, wrap_width=1100,
                     max_lines=1, need_bind=g))
    for cur in range(MAXC):
        act = 'choice.confirm' if cur == i else f'choice.move.{i - cur}'
        talk.append(dict(type='image', src=clear, rect=[32, y, 1176, BH], color='#00000000',
                         on_tap=act, need_bind=g, hide_bind='dialogue.selected',
                         keep_min=cur, keep_max=cur))
talk.append(T(620, 944, 'Tap a choice to highlight it, tap again to confirm.', 3, DIM,
              align='center', need_bind='controls.choice'))
for d in range(1, MAXC):
    m['actions'][f'choice.move.{d}'] = dict(kind='button', button='DDown', frames=5, repeat=d,
                                            enabled_bind='controls.choice')
    m['actions'][f'choice.move.{-d}'] = dict(kind='button', button='DUp', frames=5, repeat=d,
                                             enabled_bind='controls.choice')
pages.append(dict(id='dialogue', title='TALK', widgets=talk))

# ------------------------------------------------------------------ FIELD / MAP
old_field = old_pages['field']['widgets']
area = next(w for w in old_field if w.get('text_bind') == 'field.key')
mapw = dict(next(w for w in old_field if w.get('type') == 'map'))
field = kit.header('AREA MAP')
field.append(dict(type='label', rect=[40, 132, 0, 40], text='', text_scale=6, color=INK,
                  text_bind='field.key', text_map=area['text_map']))
field.append(T(1208, 144, bind_text='live.date', scale=4, color=DIM, align='right',
               need_bind='date.ready'))
field.append(IMG(kit.shape('rect', 1176, 4), 32, 186, 1176, 4, color=RED))
PIN, PW, PH = kit.sprite('minimap', 49, 'map_pin', height=46, color=RED,
                         note='minimap destination pin, recoloured red')
mapw.update(bg=BG, color=RED, marker_src=PIN, marker_size=[PW, PH], marker_anchor=[0.5, 1.0])
# Two map boxes, one shown at a time on ga.strip: in a dungeon the box stops above the party strip;
# anywhere the strip is hidden (town, dialogue) it runs down to the bottom bar instead of leaving the
# strip's slot empty. Separate ids, so each keeps its own pan/zoom.
MAP_Y, MAP_BOTTOM, STRIP_Y = 190, 948, 826
for mid, h, keep in (('p5r_map', MAP_BOTTOM - MAP_Y, dict(keep_max=0)),
                     ('p5r_map_strip', STRIP_Y - 16 - MAP_Y, dict(keep_min=1))):
    field.append(dict(mapw, id=mid, rect=[32, MAP_Y, 1176, h], hide_bind='ga.strip', **keep))
    field.append(T(620, MAP_Y + h // 2 - 20, 'Map unavailable for this area.', 5, DIM, align='center',
                   need_bind='controls.map_unavailable', hide_bind='ga.strip', **keep))
field += party_strip(STRIP_Y)
# native-menu drivers (press R / X in the game) moved from the old nav row into the header band
field += hbtn(780, 'GAME MAP', 'field.map', 'controls.field')
field += hbtn(1004, 'GAME MENU', 'field.menu', 'controls.field')
pages.append(dict(id='field', title='MAP', widgets=field))

# ------------------------------------------------------------------ BATTLE (v0.9 ui_battle)
# TACTICAL board (enemy tiles / party tiles / detail of the tapped tile) + FOLLOW view, read-only:
# see battle_page.py. The native Analyze screen still switches to the ANALYSIS page by page_bind.
import battle_page  # noqa: E402
pages.append(dict(id='battle', title='BATTLE', widgets=battle_page.build(kit, m)))

# ------------------------------------------------------------------ ANALYSIS
ana = kit.header('ENEMY ANALYSIS')
A = 'analysis.ready'
ana.append(T(36, 136, bind_text='analysis.name', scale=8, need_bind=A))
ana.append(IMG(LV_BIG, 36, 214, LVBW, LVBH, need_bind=A))
ana += kit.number('analysis.level', 36 + LVBW + 120, 204, 'a', 2, need=A, height=76)
ana.append(IMG(HP_TAG, 330, 226, HPW, HPH, need_bind=A))
ana += kit.number('analysis.hp', 330 + HPW + 200, 204, 'a', 3, need=A, height=76)
ana.append(IMG(SP_TAG, 760, 226, SPW, SPH, need_bind=A))
ana += kit.number('analysis.sp', 760 + SPW + 200, 204, 'a', 3, need=A, height=76)
ICONS, IW, IH = kit.sprite_set('camp00', list(range(319, 329)), 'el', height=84)
WORDS, WW, WH = kit.sprite_set('camp00', list(range(710, 720)), 'ew', height=44)
BACK, BW, BH = kit.sprite_set('camp00', [665, 666, 667, 668, 669, 670], 'bk', height=92)
STAMPS, SW, SH = kit.sprite_set('camp00', [329, 330, 331, 332, 333, 485], 'st', height=76)
_dash = Kit.fit(kit.load_sprite('camp00', 686), width=56)
STAMPS.insert(0, kit._save(Kit.pad(_dash, SW, SH), 'st_dash', {'source': 'EN/INIT/P5CAMP_00SPD.SPD',
                                                            'sprite_id': 686, 'size': [SW, SH]}))
# affinity: 0 normal, 1 drain, 2 repel, 3 null, 4 resist, 5 weak, 6 unknown
backing = [''] + BACK
TW, TH = 224, 252
for i in range(10):
    x, y = 32 + (i % 5) * 238, 322 + (i // 5) * 270
    b = f'analysis.affinity.{i}'
    ana.append(kit.tile(x, y, TW, TH, need=A, skew=10))
    ana.append(IMG(ICONS[i], x + (TW - IW) // 2, y + 18, IW, IH, need_bind=A))
    ana.append(IMG(WORDS[i], x + (TW - WW) // 2, y + 108, WW, WH, need_bind=A))
    ana.append(kit.by_value(b, backing, x + (TW - BW) // 2, y + 154, BW, BH, need=A, color=RED,
                            hide_bind=b, keep_max=6))
    ana.append(kit.by_value(b, STAMPS, x + (TW - SW) // 2, y + 162, SW, SH, need=A,
                            hide_bind=b, keep_max=6))
Q, QW, QH = kit.sprite('camp00', 485, 'q_small', height=40)
legend = '= not yet discovered'
ana.append(IMG(Q, 36, 864, QW, QH, need_bind=A))
ana.append(T(36 + QW + 12, 872, legend, 5, DIM, need_bind=A))
# Battle: native Analyze cycles enemy TYPES with L (prev) / R (next); shown only when count > 1
C = 'analyze.cycle'
ana += kit.button(640, 850, 170, 76, 'PREV', 'analyze.prev', C, scale=5)
ana.append(kit.value(900, 866, 'analyze.number', 5, INK, align='right', need_bind=C))
ana.append(T(925, 866, '/', 5, DIM, align='center', need_bind=C))
ana.append(kit.value(950, 866, 'analyze.count', 5, INK, need_bind=C))
ana += kit.button(1038, 850, 170, 76, 'NEXT', 'analyze.next', C, scale=5)
ana += hbtn(780, 'BATTLE', 'page.battle', 'battle.active')
ana += hbtn(1004, 'CLOSE', 'analysis.close', 'analysis.ready')
pages.append(dict(id='analysis', title='ANALYSIS', widgets=ana))

# ------------------------------------------------------------------ SOCIAL (new)
soc = kit.header('SOCIAL')
S = 'social.ready'
soc.append(T(620, 300, 'Social stats appear once the game reports them.', 5, DIM, align='center',
             hide_bind=S, keep_max=0))
STAR_ON, STW, STH = kit.sprite('camp00', 482, 'star', height=34)
STAR_OFF, _, _ = kit.sprite('camp00', 482, 'star_off', height=34, color='#FF4A3F44')
STATS = ['KNOWLEDGE', 'CHARM', 'PROFICIENCY', 'GUTS', 'KINDNESS']
for i, stat in enumerate(STATS):
    y = 132 + i * 64
    p = f'social.{i}.'
    soc.append(kit.tile(32, y, 1176, 56, need=S, skew=8, line=False,
                        fill=TILE if i % 2 == 0 else TILE_HI))
    soc.append(T(56, y + 16, stat, 5, INK, need_bind=S))
    for k in range(5):
        x = 360 + k * 40
        soc.append(IMG(STAR_OFF, x, y + 11, STW, STH, need_bind=S))
        soc.append(IMG(STAR_ON, x, y + 11, STW, STH, need_bind=S, hide_bind=p + 'rank',
                       keep_min=k + 1))
    soc.append(T(588, y + 16, bind_text=p + 'title', scale=5, color=RED, need_bind=S))
    # runtime image fill_bind fills bottom-up; the bar widget fills left-to-right
    soc.append(dict(type='bar', rect=[940, y + 20, 240, 16], bind=p + 'points', max_bind=p + 'goal',
                    bg='#FF3A3035', color=RED, need_bind=S))
# Confidant grid: 24 slots, packed so absent confidants leave no holes.
C = 'confidant.ready'
soc.append(IMG(kit.shape('rect', 1176, 4), 32, 460, 1176, 4, color=RED, need_bind=C))
CONF_WORD, CWW, CWH = kit.text_image('CONFIDANTS', 22, 'confidants_word')
soc.append(IMG(CONF_WORD, 40, 474, CWW, CWH, need_bind=C))
soc.append(kit.value(1208, 474, 'confidant.count', 4, DIM, align='right', need_bind=C,
                     suffix=' / 24'))
# Confidant cards: EN/CAMP/CARDTEX/C_CARDxx.DDS, xx = NAME.TBL arcana index - 1 in hex
# (1 Fool -> 00 ... 21 Judgement -> 14, 24 World -> 17, 29/31 Faith -> 1C/1E, 30 Councillor -> 1D).
# 22 Aeon / 23 ??? have no card: the arcana name label underneath shows through instead.
from p5r_paths import ROMFS  # noqa: E402
CARDTEX = ROMFS / 'EN/CAMP/CARDTEX'
CW, CH = 88, 176
CARDS = [''] * 32
for arc in range(1, 32):
    f = CARDTEX / f'C_CARD{arc - 1:02X}.DDS'
    if f.exists():
        card = Art.dds(f'EN/CAMP/CARDTEX/{f.name}').resize(CW, CH)
        CARDS[arc] = kit._save(card, f'card{arc:02d}', {'source': f'EN/CAMP/CARDTEX/{f.name}',
                                                        'size': [CW, CH], 'arcana': arc})
# No card art: a card-shaped placeholder carrying the arcana name (NAME.TBL 22 Aeon, 23 ???).
for arc, label in {22: 'Aeon', 23: '???'}.items():
    ph = Art.new(CW, CH, (255, 255, 255, 255)).rectangle((6, 6, CW - 7, CH - 7), (34, 26, 30, 255))
    t = kit.text_art(label, 22)
    ph = ph.over(t, (CW - t.width) // 2, (CH - t.height) // 2)
    CARDS[arc] = kit._save(ph, f'card{arc:02d}', {'source': 'generated placeholder', 'arcana': arc})
blank = kit._save(Art.new(CW, CH), 'card_none', {'source': 'generated'})
CARDS = [c or blank for c in CARDS]
RANKS = []
rank_digits = kit.digits('r', 34)
MAXW, MXW, MXH = kit.sprite('camp00', 657, 'rank_max', height=26)
RKW, RKH = max(rank_digits['w'], MXW), 34
for r in range(11):
    src = MAXW if r == 10 else rank_digits['srcs'][r]
    im = kit.art_of[src]
    RANKS.append(kit._save(Kit.pad(im, RKW, RKH, 'center'), f'rk{r}', {'source': 'composite',
                 'of': src[5:]}))
BADGE = kit.shape('tile', RKW + 14, RKH + 8, fill='#FF000000', skew=6)
DOUBT, DBW, DBH = kit.sprite('camp00', 651, 'stamp_doubt', width=CW - 4)
BROKEN, BRW, BRH = kit.sprite('camp00', 652, 'stamp_broken', width=CW - 4)
cell = dict(repeat=24, repeat_cols=12, repeat_dx=CW + 10, repeat_row_dy=CH + 14, pack=True,
            hide_bind='confidant.{i}.present', keep_min=1)
CX, CY = 32, 512


def rep(w, need=C):
    w = dict(w)
    w.update(cell)
    w['need_bind'] = need
    return w


soc.append(rep(dict(type='image', src=CARDS[1], rect=[CX, CY, CW, CH],
                    bind='confidant.{i}.arcana', src_names=CARDS, color=INK)))
soc.append(rep(IMG(BADGE, CX + CW - RKW - 10, CY - 6, RKW + 14, RKH + 8)))
soc.append(rep(dict(type='image', src=RANKS[0], rect=[CX + CW - RKW - 3, CY - 2, RKW, RKH],
                    bind='confidant.{i}.rank', src_names=RANKS, color=INK)))
soc.append(rep(IMG(DOUBT, CX + 2, CY + CH // 2 - DBH // 2, DBW, DBH), need='confidant.{i}.reversed'))
soc.append(rep(IMG(BROKEN, CX + 2, CY + CH // 2 - BRH // 2, BRW, BRH), need='confidant.{i}.broken'))
pages.append(dict(id='social', title='SOCIAL', widgets=soc))

# ------------------------------------------------------------------ WAITING (pre-gameplay / loading)
import waiting  # noqa: E402
# module_error=1 (the host's flag when the module refuses this game build, or cannot load) swaps
# the whole page for the "Contract Unsealed" art. That art is pre-rendered because the game font
# itself is decoded by the module, so it is unavailable exactly when this screen shows.
OK = dict(hide_bind='module_error', hide_eq=1)
BAD = dict(need_bind='module_error')
wait = [IMG(kit.shape('rect', 1240, 10), 0, 0, 1240, 10, color=RED)]
wsrc, ww, wh = waiting.compose_minimal(kit)
wait.append(IMG(wsrc, 0, 40, ww, wh, **OK))
wait.append(T(36, 1040, bind_text='status', scale=3, color=DIM, **OK))
if kit.mode == 'png':
    msrc, mw, mh = waiting.compose_mismatch_minimal(kit, version='v1.0.2')
    wait.append(IMG(msrc, 0, 40, mw, mh, **BAD))
else:
    # Asset-free: module_error means the module refused this build (or failed to load), so neither
    # its images nor the game font exist here -- runtime text only (the host's own font).
    wait.append(T(620, 545, 'Contract Unsealed', 8, RED, align='center', **BAD))
    wait.append(T(620, 610, 'Patch mismatch.', 5, DIM, align='center', **BAD))
    wait.append(T(620, 650, 'Update to v1.0.2 to seal the pact.', 5, DIM, align='center', **BAD))
wait.append(T(620, 1046, bind_text='module_error_message', scale=3, color=DIM, align='center', **BAD))
pages.insert(0, dict(id='waiting', title='LOADING', widgets=wait))
for b in m.get('page_binds', []):
    if b.get('point') == 'state.mode' and b.get('equals') == 0:
        b['when_equal'] = {'page': 'waiting', 'transition': 'fade', 'duration_ms': 250}
m['page_binds'] = [b for b in m['page_binds']
                   if not (b.get('point') == 'state.mode' and b.get('equals') == 4)
                   and b.get('point') != 'controls.choice']
m['page_binds'] += [
    {'point': 'controls.choice', 'equals': 1, 'when_equal': {'page': 'dialogue'},
     'when_not_equal': {'page': 'field'}, 'ready_bind': 'map.ready'},
    {'point': 'controls.choice', 'equals': 1, 'when_equal': {'page': 'dialogue'},
     'when_not_equal': {'page': 'live'}, 'ready_bind': '!map.ready'},
    # A negotiation choice closing mid-battle returns to the battle page (listed last: wins).
    {'point': 'controls.choice', 'equals': 1, 'when_equal': {'page': 'dialogue'},
     'when_not_equal': {'page': 'battle'}, 'ready_bind': 'battle.active'},
]

m['pages'] = pages
m['actions']['page.social'] = dict(kind='page', page='social', enabled_bind='live.ready')
outs = m.setdefault('module_outputs', [])
for k in ['social.ready', 'confidant.ready', 'confidant.count', 'dialogue.has_speaker'] + \
        [f'social.{i}.{f}' for i in range(5) for f in ('rank', 'points', 'next', 'name')] + \
        [f'confidant.{i}.{f}' for i in range(24) for f in ('present', 'arcana', 'name', 'rank', 'reversed', 'broken', 'max')]:
    if k not in outs:
        outs.append(k)
(out_dir / 'manifest.json').write_text(json.dumps(m, ensure_ascii=False, separators=(',', ':')) + '\n')
kit.write_provenance()
n = sum(len(p['widgets']) for p in pages)
print(f'{len(pages)} pages, {n} widgets, {len(kit.provenance)} assets')
