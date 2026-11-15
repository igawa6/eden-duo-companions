#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check generated UI has no visible words outside the runtime label registry."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UiLabels(unittest.TestCase):
    def test_navigation_captions_and_targets(self):
        manifest = json.loads((ROOT / 'packages/MyTimeAtSandrock/dualscreen/manifest.json').read_text())
        pages = {page['id']: page for page in manifest['pages']}
        for page, action, key in [('inventory', 'nav_ingredients', 'ui.1291'),
                                  ('ingredients', 'nav_inventory', 'ui.1078'),
                                  ('npc', 'nav_social', 'ui.20263')]:
            widgets = pages[page]['widgets']
            button = next(w for w in widgets if w.get('on_tap') == action and w['rect'][1] < 900)
            x, y, width, height = button['rect']
            self.assertGreaterEqual(min(width, height), 112)
            self.assertFalse(button.get('bind_text'))
            caption = next(w for w in widgets if w.get('bind_text') == key and
                           w['type'] == 'label' and y <= w['rect'][1] < y + height)
            self.assertEqual(caption['rect'][0], x + width // 2)
            veil = next(i for i, w in enumerate(widgets) if w.get('id') == 'loading_veil')
            self.assertLess(widgets.index(caption), veil)

    def test_fit_preserves_readability_and_deadline_bounds(self):
        manifest = json.loads((ROOT / 'packages/MyTimeAtSandrock/dualscreen/manifest.json').read_text())
        for page in manifest['pages']:
            for widget in page['widgets']:
                if widget['type'] == 'label':
                    self.assertGreaterEqual(widget['text_scale'], 4)
                    if widget.get('bind_text') in {'today.tracked_left_text', 'today.hero_left_text',
                                                   'npc_detail.name', 'npc_detail.hearts_text',
                                                   'item_detail.count_text', 'ws.rank_text'}:
                        self.assertGreaterEqual(widget['text_scale'], 7)
                    self.assertEqual(widget['text_min_scale'], widget['text_scale'])
                    self.assertGreater(widget['wrap_width'], 0)
                    cap = widget['text_scale'] * 5
                    pitch = cap + widget.get('line_gap', widget['text_scale'] * 3)
                    self.assertLessEqual(cap + (widget['max_lines'] - 1) * pitch,
                                         widget['rect'][3], (page['id'], widget.get('bind_text')))
        mission = next(p for p in manifest['pages'] if p['id'] == 'mission')
        deadline = next(w for w in mission['widgets'] if w.get('bind_text') == 'mission{i}.left_text')
        self.assertGreaterEqual(deadline['text_scale'], 5)
        self.assertEqual(deadline['max_lines'], 2)
        self.assertEqual(deadline['wrap_width'], deadline['rect'][2])

    def test_machine_values_use_positive_gates(self):
        manifest = json.loads((ROOT / 'packages/MyTimeAtSandrock/dualscreen/manifest.json').read_text())
        for page in manifest['pages']:
            for widget in page['widgets']:
                if widget.get('bind_text', '').startswith('machines.'):
                    self.assertFalse(widget['need_bind'].startswith('!'))

    def test_visible_text_and_runtime_keys(self):
        header = (ROOT / 'native/modules/mtas_ui.h').read_text()
        native_ids = re.search(r'UiGameIds\[\]\{([^}]+)', header)[1]
        keys = {'ui.' + key for key in re.findall(r'\d+', native_ids)}
        keys |= {'ui.' + key for key in re.findall(r'^    \{"([a-z_]+)",', header, re.M)}
        manifest = json.loads((ROOT / 'packages/MyTimeAtSandrock/dualscreen/manifest.json').read_text())
        count = 0
        for page in manifest['pages']:
            for widget in page['widgets']:
                self.assertNotRegex(widget.get('text', ''), r'[^0-9 .?+×/():<>\-]', page['id'])
                key = widget.get('bind_text', '')
                if key.startswith('ui.'):
                    self.assertIn(key, keys, page['id'])
                    count += 1
        self.assertGreater(count, 50)
        print(f'{count} visible UI bindings covered; no literal words')


if __name__ == '__main__':
    unittest.main()
