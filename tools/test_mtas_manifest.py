#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check both Sandrock canvases, repeated viewports and touch geometry."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
m = json.loads((ROOT / 'packages/MyTimeAtSandrock/dualscreen/manifest.json').read_text())
assert m['min_runtime'] == 25
assert m['canvas_alt'] == {'w': 1080, 'h': 1620, 'when': 'portrait'}
assert len(m['pages']) == 10
# Essential: task/item/blueprint/recipe/NPC/rival names, mission objectives and
# requirements listed below. Other labels retain the secondary floor.
# Also guard fit-shrink floors so essential names never fall below scale 5.
ESSENTIAL_BINDS = {
    'mission_detail.item_name', 'mission_detail.item_text', 'mission_detail.title',
    'mission_detail.objective', 'mission{i}.title', 'item_detail.name', 'item{i}.name',
    'blueprint_detail.name', 'bp{i}.name', 'recipe_detail.name', 'recipe{i}.name',
    'npc_detail.name', 'npc{i}.name', 'rank{i}.name',
}
count = 0
for page in m['pages']:
    ids = set()
    scrolls = {s['id']: s for s in page.get('scrolls', [])}
    for region in scrolls.values():
        x, y, w, h = region['rect_alt']
        assert x >= 0 and y >= 0 and w > 0 and h > 0
        assert x+w <= 1080 and y+h <= 1496, (page['id'], region)
    # Independent list/detail viewports must not occupy each other's interaction area.
    for i, a in enumerate(scrolls.values()):
        for b in list(scrolls.values())[i+1:]:
            ax, ay, aw, ah = a['rect_alt']; bx, by, bw, bh = b['rect_alt']
            assert min(ax+aw, bx+bw) <= max(ax, bx) or min(ay+ah, by+bh) <= max(ay, by), (page['id'], a['id'], b['id'])
    buttons = []
    for widget in page['widgets']:
        assert widget['id'] not in ids, (page['id'], widget['id'])
        ids.add(widget['id'])
        x, y, w, h = widget['rect_alt']
        assert w > 0 and h > 0
        if widget['type'] == 'label':
            essential = widget.get('bind_text') in ESSENTIAL_BINDS
            floor = 5 if essential else 4
            assert widget['text_scale'] >= floor, (page['id'], widget['id'], 'text floor')
            if widget.get('fit_text'):
                assert widget.get('text_min_scale', widget['text_scale']) >= floor, (page['id'], widget['id'], 'fit floor')
            assert widget['wrap_width_alt'] == w, (page['id'], widget['id'], 'portrait text width')
            if widget.get('bind_text') in ('item_detail.name', 'blueprint_detail.name'):
                assert widget['max_lines'] == 3
                for rect in ('rect', 'rect_alt'):
                    assert widget[rect][3] >= 107, (page['id'], widget['id'], 'three-line name height')
            x -= w if widget.get('align') == 'right' else w//2 if widget.get('align') == 'center' else 0
        # Bled native backings and a map-relative camera-cone origin are intentional.
        bleed = widget.get('src', '').startswith('module:mtas:ui:I_Social_icon_Di_04:') and widget['rect'][0] < 0
        cone = widget.get('rotate_bind') == 'map.camera_yaw'
        assert bleed or cone or (x >= 0 and y >= 0 and x+w <= 1080 and y+h <= 1620), (page['id'], widget['id'], widget['rect_alt'])
        if widget.get('on_tap'):
            assert w >= 112 and h >= 112, (page['id'], widget['id'], widget['rect_alt'])
            # Feedback variants and calendar states are mutually exclusive copies.
            key = (tuple(widget['rect_alt']), widget.get('payload'), widget['on_tap'].removesuffix('_reduced'))
            if key not in [k for k, _ in buttons]:
                buttons.append((key, widget))
        if widget.get('scroll'):
            assert widget['scroll'] in scrolls
            assert widget.get('repeat', 0) >= 1, (page['id'], widget['id'], 'scroll template required')
            if widget.get('repeat', 1) > 1:
                region = scrolls[widget['scroll']]
                assert widget.get('repeat_cols', 1) == region.get('cols', 1)
                # All repeated columns fit; overflow along the scroll axis is clipped by design.
                sx, _, sw, _ = region['rect_alt']
                cols = widget.get('repeat_cols', 1)
                assert x >= sx and x+w+(cols-1)*widget.get('repeat_dx', 0) <= sx+sw, (page['id'], widget['id'], 'repeat width')
        count += 1
    # Discrete controls outside scroll regions must not obscure another control.
    for i, (_, a) in enumerate(buttons):
        for _, b in buttons[i+1:]:
            if a.get('scroll') or b.get('scroll'): continue
            if a['rect_alt'] == b['rect_alt']: continue
            ga, gb = a.get('need_bind', ''), b.get('need_bind', '')
            if ga.startswith('calendar') and gb.startswith('calendar') and ga[:9] != gb[:9]: continue
            ax, ay, aw, ah = a['rect_alt']; bx, by, bw, bh = b['rect_alt']
            assert min(ax+aw, bx+bw) <= max(ax, bx) or min(ay+ah, by+bh) <= max(ay, by), (page['id'], a['id'], b['id'], 'touch overlap')
for action, factor in [('mtas_map_zoom_in', 1.25), ('mtas_map_zoom_out', 0.8)]:
    assert m['actions'][action] == {'kind': 'view_zoom_step', 'view': 'mtas_world_map', 'factor': factor}
    controls = [w for p in m['pages'] if p['id'] == 'map' for w in p['widgets'] if w.get('on_tap') == action]
    assert controls
    for control in controls:
        for rect in ('rect', 'rect_alt'):
            assert control[rect][2:] == [112, 112]
        assert control['need_bind'].startswith('map.outdoor')
inventory = next(p for p in m['pages'] if p['id'] == 'inventory')
fades = [w for w in inventory['widgets'] if w.get('src') == 'module:mtas:gradient:fade']
derived = {d['name']: d.get('expr', '') for d in m['derived']}
for fade in fades:
    expr = derived[fade['need_bind']]
    assert ' != 0)' in expr and ' < @scroll_max:' in expr
print(f'PASS: {len(m["pages"])} pages, {count} widgets, alternate text/scrolls, zoom actions; bounds, repeat widths, touch floors and unintended touch/view overlaps')

# Hardening: readable rival names and native-size chat/gift checks on either canvas.
for page in m['pages']:
    for widget in page['widgets']:
        if widget.get('bind_text') == 'rank{i}.name':
            assert widget['fit_text'] and widget['text_min_scale'] == 5
            assert widget['max_lines'] == 2 and widget['line_gap'] == 10
            assert widget['repeat_row_dy'] >= 84
            for rect in ('rect', 'rect_alt'):
                assert widget[rect][3] >= 70
        if page['id'] == 'social' and widget.get('need_bind') in ('npc{i}.chatted_today', 'npc{i}.gifted_today') and 'MissionGou' in widget.get('src', ''):
            assert widget['rect'][2:] == widget['rect_alt'][2:] == [40, 40]
print('PASS: essential names/objectives scale >= 5 with fit floor >= 5; secondary >= 4; rival spacing and Social checks on both canvases')
