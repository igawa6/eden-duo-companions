#!/usr/bin/env python3
"""Asset-free AYN Thor UI layout prototype for the Persona 3 Portable companion.

This is an editable source port of the community v1.8.10 theme, not a byte-for-byte
replacement. The local test mod used procedural PNG artwork; this preview uses only
the runtime's existing module:p3p: artwork and fill texture, so no game assets
are distributed in the repository.

Usage (from the repository root):
    python3 tools/p3p/gen_manifest.py
    python3 tools/p3p/thor_ui.py

This changes only pages/widgets in the manifest and deliberately preserves the
native module, actions, game outputs, and page bindings.
"""
from __future__ import annotations
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'packages/Persona3Portable/dualscreen/manifest.json'
BLUE = '#FF0B3A91'
RED = '#FF6A0A1E'
WHITE = '#FFFFFFFF'
GOLD = '#FFFFD984'
ACCENTS = [BLUE, RED]
MENU = (('SKILL', 4), ('ITEM', 7), ('PERSONA', 2), ('EQUIP', 5),
        ('STATUS', 6), ('S.LINK', 3), ('CALENDAR', 8), ('MAP', 10))


def image(rect, source, **kwargs):
    return dict(type='image', rect=rect, src=source, **kwargs)


def label(rect, text='', bind=None, scale=5, color=BLUE, **kwargs):
    w = dict(type='label', rect=rect, text=text, text_scale=scale, color=color, **kwargs)
    if bind is not None:
        w['bind_text'] = bind
    return w


def shape(rect, color, **kwargs):
    return image(rect, 'module:p3p:fill', tint=color, **kwargs)


def by_route(make):
    """Create gated copies; no derived values or module state are changed."""
    return [dict(make(color), hide_bind='ui.route', hide_eq=(1 if route == 0 else 0))
            for route, color in enumerate(ACCENTS)]


def main_menu(p):
    # One vertical list, each visible label combined with its tap button.
    w = [dict(type='image', rect=[0, 0, 1240, 1080], bind='ui.route',
              src_format='module:p3p:bg/%d')]
    w += [image([52, 12, 330, 68], 'module:p3p:spr/9@68')]
    w += [dict(type='image', rect=[540, 140, 910, 910], bind='live.lead',
               src_format='module:p3p:bust/%d@910', need_bind='live.lead')]
    w += [label([1130, 24, 0, 40], bind='live.date', scale=5, color=WHITE,
                align='right', outline=BLUE, outline_px=2, need_bind='live.ready'),
          label([1130, 62, 0, 36], bind='live.money', scale=4, color=GOLD,
                align='right', need_bind='live.ready')]
    for i, (name, target) in enumerate(MENU):
        y = 112 + 70 * i
        rect = [44, y, 564, 68]
        w += by_route(lambda accent: shape([64, y + 25, 15, 15], accent))
        w.append(dict(type='button', rect=rect, text=name, text_scale=7,
                      text_center_h=68, align='left', text_inset=70,
                      italic=True, color=WHITE, outline=BLUE, outline_px=2,
                      bg='#00000000', border=0, on_tap=f'ui.open.{target}'))
    # Reserve a gap between the eight menu entries and party status.
    w += [shape([50, 752, 570, 86], '#DFEBF9FF'),
          label([68, 758, 500, 32], bind='party.0.name', scale=5,
                need_bind='party.0.present')]
    for stat, x, end, color in [
        ('hp', 68, 320, '#FFF7A33A'),
        ('sp', 334, 574, '#FFC862DE')]:
        w += [label([x, 795, 48, 34], stat.upper(), scale=4),
              dict(type='value', rect=[end, 795, 0, 34], bind='party.0.'+stat,
                   max_bind='party.0.max_'+stat, max_sep=' / ',
                   color=BLUE, text_scale=4, align='right'),
              dict(type='bar', rect=[x+54, 807, end-x-60, 12],
                   bind='party.0.'+stat, max_bind='party.0.max_'+stat,
                   color=color, bg=WHITE, frame=0)]
    w += [label([60, 1034, 700, 30], bind='status', scale=3,
                color=WHITE, need_bind='live.ready')]
    p['widgets'] = w


def battle(p):
    w = [dict(type='image', rect=[0, 0, 1240, 1080],
              bind='ui.route', src_format='module:p3p:bg/%d'),
         label([52, 18, 450, 56], 'BATTLE', scale=8, color=GOLD,
               italic=True, outline=BLUE, outline_px=2),
         label([1216, 24, 0, 40], bind='live.date', scale=5,
               color=WHITE, align='right')]
    for i in range(4):
        y = 112 + 76*i
        stem = f'party.{i}'
        need = {'need_bind': stem+'.present'}
        w += [shape([52, y, 1176, 68], '#DAFFFFFF', **need)]
        w += by_route(lambda accent: shape([44, y, 400, 68], accent, **need))
        w += [label([70, y+12, 342, 44], bind=stem+'.name', color=GOLD,
                    scale=6, italic=True, fit_text=True, wrap_width=342,
                    text_min_scale=4, max_lines=1, **need),
              label([520, y+9, 200, 32], bind=stem+'.ailment', scale=4,
                    color='#FFB3154C', **need)]
        for stat, x, end, color in [
            ('hp', 454, 790, '#FFF7A33A'),
            ('sp', 827, 1163, '#FFC862DE')]:
            w += [label([x, y+7, 55, 32], stat.upper(), scale=4, **need),
                  dict(type='value', rect=[end, y+7, 0, 32],
                       bind=stem+'.'+stat, max_bind=stem+'.max_'+stat,
                       max_sep=' / ', color=BLUE, text_scale=4,
                       align='right', **need),
                  dict(type='bar', rect=[x-12, y+45, 344, 14],
                       bind=stem+'.'+stat, max_bind=stem+'.max_'+stat,
                       color=color, bg=BLUE, frame=0, **need)]
    w += [label([66, 437, 380, 42], 'ENEMIES', scale=6, color=GOLD,
                italic=True, outline=BLUE, outline_px=2)]
    for i in range(5):
        y = 496 + i*96
        stem = f'en.{i}'
        need = {'need_bind': stem+'.present'}
        w += [shape([52, y, 1176, 86], '#D5FFFFFF', **need),
              label([74, y+4, 426, 36], bind=stem+'.name', scale=5,
                    color=BLUE, fit_text=True, text_min_scale=3,
                    wrap_width=426, max_lines=1, **need),
              label([74, y+44, 426, 32], bind=stem+'.info', scale=4,
                    color=BLUE, fit_text=True, text_min_scale=3,
                    wrap_width=426, max_lines=1, **need),
              label([505, y+4, 226, 32], bind=stem+'.hp_text', scale=4,
                    color=BLUE, fit_text=True, text_min_scale=3,
                    wrap_width=226, max_lines=1, **need),
              dict(type='bar', rect=[505, y+47, 216, 16],
                   bind=stem+'.hp', max_bind=stem+'.max_hp',
                   color='#FFF7A33A', bg=BLUE, frame=0, **need)]
        for e in range(9):
            w += [label([770+e*48, y+28, 0, 32],
                        bind=f'{stem}.aff.{e}', scale=4,
                        color=BLUE, align='center', **need)]
    w += [label([44, 1012, 1150, 32], bind='status', scale=3,
                color=WHITE, need_bind='live.ready')]
    p['widgets'] = w


def normalize_other_pages(p):
    for w in p['widgets']:
        if w.get('type') == 'label' and w.get('bind_text','').endswith('.name'):
            if w.get('rect', [0,0,0,0])[2] > 0:
                w['fit_text'] = True
                w.setdefault('wrap_width', w['rect'][2])
                w.setdefault('text_min_scale', 3)
                w.setdefault('max_lines', 1)
        if w.get('type') == 'button' and w.get('text') == 'Back':
            w['text_scale'] = 6
        if w.get('bind_text') == 'live.date':
            w['text_scale'] = 5
    if p['id'] == 'personas':
        for w in p['widgets']:
            bind = w.get('bind_text','')
            if bind in ('pd.card','pd.nextskill'):
                w['rect'] = [466, 888 if bind == 'pd.card' else 930, 720, 36]
                w['text_scale'] = 4
                w['fit_text'] = True
                w['wrap_width'] = 720
                w['text_min_scale'] = 3
    if p['id'] == 'calendar':
        for w in p['widgets']:
            if w.get('bind_text') == 'cal.next':
                w['fit_text'] = True
                w['text_min_scale'] = 3


def apply(path=MANIFEST):
    data = json.loads(path.read_text(encoding='utf-8'))
    original = copy.deepcopy(data)
    assert len(data['pages']) == 12 and len(data['module_outputs']) == 2798
    pages = {p['id']: p for p in data['pages']}
    main_menu(pages['home'])
    battle(pages['battle'])
    for page in data['pages']:
        if page['id'] not in ('home', 'battle'):
            normalize_other_pages(page)
    data['name'] = 'Persona 3 Portable live companion — Thor UI source preview'
    for key in ('module_outputs','module','actions','page_binds','enforce','enforce_gate'):
        assert data[key] == original[key], 'Unexpected change to '+key
    assert len(data['pages']) == len(original['pages'])
    assert not any(w.get('src','').startswith('file:img/')
                   for p in data['pages'] for w in p['widgets'])
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False)+'\n',
                    encoding='utf-8')


if __name__ == '__main__':
    apply()
