#!/usr/bin/env python3
"""merge_base_v08.py <canonical manifest> <out manifest> <snippet.json>...
Merges the START MENU manifest snippets into the base manifest:
module_outputs / module_outputs_add (append, dedup), actions (add; identical re-definitions ok,
different ones fail), page_binds (append), map (top-level merge), flags (union of defaults; a clash
with different defaults fails), enforce (append, dedup by action; all rules share ONE gate),
enforce_gate (must be identical across snippets: the single pdrv.pending gate)."""
import json, sys
m = json.load(open(sys.argv[1]))
for sp in sys.argv[3:]:
    s = json.load(open(sp))
    for o in s.get('module_outputs', []) + s.get('module_outputs_add', []):
        if o not in m['module_outputs']: m['module_outputs'].append(o)
    for k, v in s.get('actions', {}).items():
        assert k not in m['actions'] or m['actions'][k] == v, f'action clash {k} in {sp}'
        m['actions'][k] = v
    m['page_binds'] += s.get('page_binds', [])
    for k, v in s.get('map', {}).items(): m.setdefault('map', {})[k] = v
    for k, v in s.get('flags', {}).items():
        f = m.setdefault('flags', {})
        assert k not in f or f[k] == v, f'flag default clash {k} in {sp}'
        f[k] = v
    for r in s.get('enforce', []):
        e = m.setdefault('enforce', [])
        old = [x for x in e if x['action'] == r['action']]
        assert not old or old[0] == r, f'enforce clash {r["action"]} in {sp}'
        if not old: e.append(r)
    if 'enforce_gate' in s:
        assert m.get('enforce_gate', s['enforce_gate']) == s['enforce_gate'], f'enforce_gate clash in {sp}'
        m['enforce_gate'] = s['enforce_gate']
    print(sp, 'merged')
if m.get('enforce'):
    assert m.get('enforce_gate') == {'point': 'pdrv.pending', 'max': 1}, m.get('enforce_gate')
json.dump(m, open(sys.argv[2], 'w'), indent=1, ensure_ascii=False)
