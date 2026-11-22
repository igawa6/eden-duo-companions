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
    # Rebuilt screens must retain the upstream pause overlay, including the
    # input-blocking layer that prevents taps during paused/error states.
    original_pages = {p['id']: p for p in original['pages']}
    for page_id in ('home', 'battle'):
        expected = [w for w in original_pages[page_id]['widgets']
                    if w.get('need_bind') == 'live.paused']
        actual = [w for w in pages[page_id]['widgets']
                  if w.get('need_bind') == 'live.paused']
        assert len(expected) == 3
        assert actual == expected, f'Lost pause/input block on {page_id}'
        assert any(w.get('input_block') for w in actual)

    # Preserve error messaging as well as live pause state on the Main page.
    assert [w for w in pages['home']['widgets']
            if w.get('need_bind') == 'module_error'] == [
                w for w in original_pages['home']['widgets']
                if w.get('need_bind') == 'module_error']

    # Other pages remain source-derived: every original touch action stays in
    # the same order and each generated repeater/bind retains its slot.
    for page_id, old in original_pages.items():
        if page_id in ('home', 'battle'):
            continue
        new = pages[page_id]
        assert len(new['widgets']) == len(old['widgets']), page_id
        assert [w.get('on_tap') for w in new['widgets']] == [
            w.get('on_tap') for w in old['widgets']], page_id
        assert [w.get('bind_text') for w in new['widgets']] == [
            w.get('bind_text') for w in old['widgets']], page_id

    # Persona's action and its backing card move together to make room for
    # progression text. They must not overlap the dynamic Persona name.
    persona_widgets = pages['personas']['widgets']
    use = [w for w in persona_widgets if w.get('need_bind') == 'persona.can_use'
           and w.get('rect') == [966, 104, 250, 66]]
    assert len(use) == 2
    pd_name = [w for w in persona_widgets if w.get('bind_text') == 'pd.name']
    assert pd_name and all(w.get('rect') == [527, 120, 418, 48]
                           for w in pd_name)
    assert all(w['rect'][0]+w['rect'][2] < use[0]['rect'][0]
               for w in pd_name)
    for bind in ('pd.card', 'pd.nextskill'):
        assert all(w.get('fit_text') for w in persona_widgets
                   if w.get('bind_text') == bind)

    # Keep the skill icon source attached to its original live binding. A
    # tentative sprite ID is not a verified replacement for Support skills.
    assert [w.get('src_format') for w in pages['skills']['widgets']
            if w.get('bind') == 'skill.{i}.icon'] == [
                w.get('src_format') for w in original_pages['skills']['widgets']
                if w.get('bind') == 'skill.{i}.icon']

    # Calendar must keep all 42 day values (including the sixth week).
    for i in range(42):
        bind = f'cal.{i}.text'
        assert sum(w.get('bind_text') == bind
                   for w in pages['calendar']['widgets']) == sum(
                       w.get('bind_text') == bind
                       for w in original_pages['calendar']['widgets'])

    print('Thor UI source port: structural checks passed')


if __name__ == '__main__':
    main()
