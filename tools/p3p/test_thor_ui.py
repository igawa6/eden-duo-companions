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

    # Other pages remain source-derived: every original action, label
    # binding and image-data binding stays attached to its original widget.
    # Persona adds ten themed elements; S.Link, Equip and Item cards add
    # decorative route-gated accent strips. Actions/bind order is unchanged.
    for page_id, old in original_pages.items():
        if page_id in ('home', 'battle'):
            continue
        new = pages[page_id]
        added = {'personas': 10, 'links': 6, 'equip': 8,
                 'items': 2}.get(page_id, 0)
        assert len(new['widgets']) == len(old['widgets']) + added, page_id
        assert [w['on_tap'] for w in new['widgets'] if w.get('on_tap')] == [
            w['on_tap'] for w in old['widgets'] if w.get('on_tap')], page_id
        assert [w['bind_text'] for w in new['widgets']
                if w.get('bind_text')] == [
                w['bind_text'] for w in old['widgets']
                if w.get('bind_text')], page_id
        assert [w['bind'] for w in new['widgets'] if w.get('bind')] == [
            w['bind'] for w in old['widgets'] if w.get('bind')], page_id

    persona_widgets = pages['personas']['widgets']
    for i in range(12):
        controls = [w for w in persona_widgets
                    if w.get('on_tap') == f'persona.select.{i}']
        assert len(controls) == 1
        assert controls[0]['rect'] == [26, 102+72*i, 460, 68]
        names = [w for w in persona_widgets
                 if w.get('bind_text') == f'persona.{i}.line']
        assert names
        assert all(w['rect'] == [66, 104+72*i, 390, 64] for w in names)
        assert all(w.get('fit_text') for w in names)
        # Tap bounds cover the entire name row.
        assert controls[0]['rect'][0] <= names[0]['rect'][0]
        assert controls[0]['rect'][0] + 460 >= names[0]['rect'][0] + 390

    # Persona details now use the final design's layout proportions.
    use = [w for w in persona_widgets if w.get('need_bind') == 'persona.can_use']
    assert len(use) == 2
    assert all(w['rect'] == [982, 170, 220, 50] for w in use)
    # A long Persona title must not geometrically collide with EQUIP.
    for w in persona_widgets:
        if w.get('bind_text') == 'pd.name':
            assert w['rect'][1] + w['rect'][3] <= use[0]['rect'][1]
        if w.get('bind_text') == 'pd.arcana':
            assert w['rect'][0] + w['rect'][2] < use[0]['rect'][0]
    assert any(w.get('on_tap') == 'persona.use' and w.get('text') == 'EQUIP'
               for w in use)
    assert all(w['rect'] == [527, 120, 665, 48]
               for w in persona_widgets if w.get('bind_text') == 'pd.name')
    assert all(w['rect'] == [527, 171, 430, 32]
               for w in persona_widgets if w.get('bind_text') == 'pd.arcana')
    assert any(w.get('bind') == 'pd.id'
               and w.get('rect') == [526, 215, 248, 248]
               for w in persona_widgets)
    for i in range(5):
        assert any(w.get('type') == 'bar'
                   and w.get('bind') == f'pd.stat.{i}'
                   and w['rect'] == [894, 235+49*i, 220, 21]
                   for w in persona_widgets)
    for i in range(8):
        assert all(w['rect'] == [534+344*(i//4), 711+38*(i%4), 311, 37]
                   and w.get('fit_text')
                   for w in persona_widgets
                   if w.get('bind_text') == f'pd.skill.{i}')
    for bind, y in [('pd.card', 879), ('pd.nextskill', 925)]:
        assert all(w['rect'] == [532, y, 660, 37] and w.get('fit_text')
                   for w in persona_widgets if w.get('bind_text') == bind)
    for section in ('AFFINITIES', 'SKILLS'):
        labels = [w for w in persona_widgets if w.get('text') == section]
        assert len(labels) == 2
        assert {w.get('hide_eq') for w in labels} == {0, 1}
    panels = [w for w in persona_widgets
              if w.get('rect') == [504, 104, 716, 864]]
    assert len(panels) == 2
    assert {w.get('hide_eq') for w in panels} == {0, 1}
    assert any(w.get('src_format') == 'module:p3p:persona/%d@248'
               for w in persona_widgets if w.get('bind') == 'pd.id')

    skill_widgets = pages['skills']['widgets']
    for i in range(10):
        controls = [w for w in skill_widgets
                    if w.get('on_tap') == f'member.select.{i}']
        assert len(controls) == 1
        assert controls[0]['rect'] == [
            33+240*(i%5), 100+74*(i//5), 236, 72]
    assert all(w['rect'] == [146, 310, 780, 44]
               for w in skill_widgets
               if w.get('bind_text') == 'skill.{i}.name')
    assert all(w['rect'] == [146, 360, 1060, 32]
               for w in skill_widgets
               if w.get('bind_text') == 'skill.{i}.help')
    # Keep the game's original skill icon lookup. A tentative sprite ID is
    # not a verified fix for Support skills such as Rakukaja.
    assert [w.get('src_format') for w in skill_widgets
            if w.get('bind') == 'skill.{i}.icon'] == [
                w.get('src_format') for w in original_pages['skills']['widgets']
                if w.get('bind') == 'skill.{i}.icon']

    # Route-specific accents repeat with the existing link repeater and
    # must never introduce touch actions or lose data bindings.
    link_widgets = pages['links']['widgets']
    for rect in ([36, 104, 12, 290], [56, 198, 1120, 3],
                 [56, 236, 1120, 3]):
        accents = [w for w in link_widgets
                   if w.get('rect') == list(rect)
                   and w.get('need_bind') == 'link.{i}.present']
        assert len(accents) == 2
        assert {w.get('hide_eq') for w in accents} == {0, 1}
        assert all(w.get('src') == 'module:p3p:fill'
                   and not w.get('on_tap') for w in accents)
    for w in link_widgets:
        if w.get('bind_text') == 'link.{i}.people':
            assert w.get('max_lines') == 4 and w.get('fit_text')

    equip_widgets = pages['equip']['widgets']
    for i in range(10):
        controls = [w for w in equip_widgets
                    if w.get('on_tap') == f'member.select.{i}']
        assert len(controls) == 1
        assert controls[0]['rect'] == [
            33+240*(i%5), 100+74*(i//5), 236, 72]
    assert sum(w.get('on_tap') == 'equip.pick'
               for w in equip_widgets) == 4
    assert sum(w.get('on_tap') == 'equip.choose'
               for w in equip_widgets) == 1
    for y in (306, 472, 638, 804):
        accents = [w for w in equip_widgets
                   if w.get('rect') == [36, y, 12, 154]
                   and w.get('need_bind') == 'equip.ready']
        assert len(accents) == 2
        assert {w.get('hide_eq') for w in accents} == {0, 1}

    item_widgets = pages['items']['widgets']
    row_accents = [w for w in item_widgets
                   if w.get('rect') == [36, 196, 14, 96]
                   and w.get('need_bind') == 'item.{i}.present']
    assert len(row_accents) == 2
    assert {w.get('hide_eq') for w in row_accents} == {0, 1}
    for i in range(4):
        controls = [w for w in item_widgets
                    if w.get('on_tap') == f'item.use.{i}']
        labels = [w for w in item_widgets
                  if w.get('bind_text') == f'use.{i}.name']
        assert len(controls) == 1 and len(labels) == 1
        assert controls[0]['rect'] == [60+232*i, 848, 220, 100]
        assert labels[0]['rect'] == controls[0]['rect']
        assert labels[0].get('wrap_width') == 204
        assert labels[0].get('fit_text') is True
    for i in range(3):
        assert sum(w.get('on_tap') == f'item.tab.{i}'
                   for w in item_widgets) == sum(
                       w.get('on_tap') == f'item.tab.{i}'
                       for w in original_pages['items']['widgets'])
    assert sum(w.get('on_tap') == 'item.pick'
               for w in item_widgets) == 1
    assert sum(w.get('on_tap') == 'item.cancel'
               for w in item_widgets) == 1

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
