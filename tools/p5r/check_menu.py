#!/usr/bin/env python3
"""check_menu.py <dualscreen dir> [canonical manifest]: static checks of the START MENU package.
- every file: src / src_names / sprite_map / src_format target (for the declared icon cells) exists
- every on_tap names an action; every page action names a page
- canonical actions, page_binds, module_outputs and non-menu pages survive (ids + actions)"""
import json, re, sys
from pathlib import Path
root = Path(sys.argv[1]); m = json.loads((root / 'manifest.json').read_text())
canon = json.loads(Path(sys.argv[2] if len(sys.argv) > 2 else
        Path(__file__).resolve().parents[2] / 'packages/Persona5Royal/dualscreen/manifest.json').read_text())
bad = []
rec_ids = set()
if (root / 'p5r_art.rec').exists():  # asset-free (module mode): recipe ids
    rec_ids = {l.split(' ', 1)[0] for l in (root / 'p5r_art.rec').read_text().splitlines()[1:]
               if l and l[0] not in '@#'}
def chk(src, where):
    if src and src.startswith('file:') and not (root / src[5:]).exists():
        bad.append(f'missing {src} ({where})')
    if src and src.startswith('module:p5r:') and src[11:] not in rec_ids:
        bad.append(f'no recipe {src} ({where})')
pages = {p['id'] for p in m['pages']}
for p in m['pages']:
    for w in p['widgets']:
        for k in ('src', 'empty_src', 'marker_src', 'highlight_src'):
            v = w.get(k, '')
            if '{i}' in v and w.get('repeat'):  # repeat template: one file per element index
                for i in range(int(w['repeat'])):
                    chk(v.replace('{i}', str(i)), p['id'])
            else:
                chk(v, p['id'])
        for s in w.get('src_names', []):
            chk(s, p['id'])
        if w.get('on_tap') and w['on_tap'] not in m['actions']:
            bad.append(f"{p['id']}: on_tap {w['on_tap']} has no action")
# v0.8: every ui.open tap names a page id with a ui.page page_bind; every module action is one the
# module dispatches
ui_ids = {int(b['equals']): b['when_equal']['page'] for b in m.get('page_binds', [])
          if b.get('point') == 'ui.page'}
for pid in ui_ids.values():
    if pid not in pages:
        bad.append(f'ui.page bind -> missing page {pid}')
MODULE_ACTIONS = {'persona_change', 'skill_use', 'persona_select', 'skill_select', 'persona_skill_select',
                  'item_tab', 'item_select', 'item_use', 'equip_select', 'equip_change', 'confidant_select',
                  'ui_open', 'equip_pick', 'item_use_sel', 'equip_change_sel', 'persona_change_sel',
                  'skill_use_sel',
                  'request_tab', 'calendar_select', 'calendar_month'}  # ui_b REQUEST tabs / CALENDAR day pick + L/R: m_data2 module
for n, a in m['actions'].items():
    if a.get('kind') == 'module' and a.get('action') not in MODULE_ACTIONS:
        bad.append(f'action {n}: unknown module action {a.get("action")}')
for p in m['pages']:
    for w in p['widgets']:
        if w.get('on_tap') == 'ui.open' and int(w.get('payload', -99)) not in ui_ids:
            bad.append(f"{p['id']}: ui.open payload {w.get('payload')} has no ui.page bind")
for k, v in m.get('sprite_map', {}).items():
    chk(v, 'sprite_map ' + k)
for k, c in m.get('composites', {}).items():
    for L in c.get('layers', []):
        chk(L.get('src', ''), 'composite ' + k)
        chk(L.get('mask_src', ''), 'composite ' + k)
chk(m.get('map', {}).get('atlas', ''), 'map atlas')
for k, a in m.get('map', {}).get('areas', {}).items():
    if not a.get('image', '').startswith('composite:'):
        chk(a.get('image', ''), 'area ' + k)
for n, a in m['actions'].items():
    if a.get('kind') == 'page' and a['page'] not in pages:
        bad.append(f'action {n} -> missing page {a["page"]}')
for n, a in canon['actions'].items():
    if n not in m['actions']:
        bad.append(f'canonical action {n} lost')
for b in canon['page_binds']:
    tgt = [b.get('when_equal', {}).get('page'), b.get('when_not_equal', {}).get('page')]
    for t in tgt:
        if t and t not in pages:
            bad.append(f'page_bind target {t} missing')
for o in canon.get('module_outputs', []):
    if o not in m['module_outputs']:
        bad.append(f'output {o} dropped')
for p in canon['pages']:
    if p['id'] not in pages:
        bad.append(f'canonical page {p["id"]} lost')
print('\n'.join(bad) if bad else 'OK', f"({len(m['pages'])} pages, {len(m['actions'])} actions, "
      f"{len(m.get('derived', []))} derived, {len(m.get('flags', {}))} flags)")
sys.exit(1 if bad else 0)
