#!/usr/bin/env python3
"""Offline structural checks for the optional Thor UI source port.

Run after gen_manifest.py; no game ROMs or emulator required.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import json

from thor_ui import MANIFEST, apply


def main():
    original = json.loads(MANIFEST.read_text(encoding='utf-8'))
    assert len(original['pages']) == 12
    with TemporaryDirectory() as directory:
        tmp = Path(directory) / 'manifest.json'
        tmp.write_text(json.dumps(original), encoding='utf-8')
        apply(tmp)
        result = json.loads(tmp.read_text(encoding='utf-8'))

    for key in ('module_outputs','module','actions','page_binds',
                'enforce','enforce_gate'):
        assert result[key] == original[key], f'Changed game integration: {key}'

    pages = {page['id']:page for page in result['pages']}
    assert len(pages) == 12
    assert len(result['module_outputs']) == 2798
    assert len(result['actions']) == len(original['actions'])
    assert len(result['page_binds']) == len(original['page_binds'])

    home = pages['home']['widgets']
    controls = [w for w in home if w.get('on_tap','').startswith('ui.open.')]
    assert len(controls) == 8
    rects = sorted(w['rect'] for w in controls)
    assert all(a[1]+a[3] <= b[1] for a,b in zip(rects,rects[1:]))
    assert rects[-1][1] + rects[-1][3] < 752
    assert {w['on_tap'] for w in controls} == {
        'ui.open.4','ui.open.7','ui.open.2','ui.open.5',
        'ui.open.6','ui.open.3','ui.open.8','ui.open.10'}

    defined = set(result['actions'])
    for page in result['pages']:
        for widget in page['widgets']:
            action = widget.get('on_tap')
            if action:
                assert action in defined, (page['id'], action)
            for field in ('src','mask_src'):
                ref = widget.get(field,'')
                assert not ref.startswith('file:img/'), (page['id'], ref)

    battle = pages['battle']['widgets']
    for i in range(4):
        assert any(w.get('bind_text') == f'party.{i}.name'
                   for w in battle)
    for i in range(5):
        assert any(w.get('bind_text') == f'en.{i}.name'
                   for w in battle)
    print('Thor UI source port: structural checks passed')


if __name__ == '__main__':
    main()
