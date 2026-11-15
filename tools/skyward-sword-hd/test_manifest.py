#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regressions for generated SSHD bindings; no game files required."""
import importlib.util
import json
import re
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('sshd_manifest', Path(__file__).with_name('gen_manifest.py'))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


def json_text(widget):
    return json.dumps(widget, sort_keys=True)


def value(expr, points):
    if '?' in expr:
        condition, rest = expr.split('?', 1)
        yes, no = rest.split(':', 1)
        return value(yes if value(condition, points) else no, points)
    # These generated expressions contain only integer comparisons and pouch references.
    expr = re.sub(r'pouch\.id\d', lambda m: str(points[m[0]]), expr).replace('&&', ' and ')
    return eval(expr, {'__builtins__': {}}, {})


class ManifestTest(unittest.TestCase):
    def test_crystal_rim_hides_icon_and_digits_when_reader_is_invalid(self):
        manifest, _ = generator.build()
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')
        icon = next(w for w in ready['widgets'] if w.get('src', '').endswith('/uk_genki_00'))
        self.assertEqual(icon['need_bind'], 'sshd.crystals.valid')
        derived = {v['name']: v for v in manifest['derived']}
        for i in range(2):
            digit = next(w for w in ready['widgets'] if w.get('bind') == f'crystals.d{i}')
            self.assertIn('sshd.crystals.valid', derived[digit['need_bind']]['expr'])

    def test_effective_hearts_include_life_medals_without_using_current_health(self):
        manifest, data = generator.build()
        derived = {v['name']: v for v in manifest['derived']}
        self.assertEqual(data['points']['hearts.base_q']['addr'], 'main+0x5af0056')
        self.assertNotIn('hearts.max_q', data['points'])
        expr = derived['hearts.max_q']['expr']
        for base, medals, expected in [(32, 0, 32), (32, 1, 36), (32, 2, 40),
                                       (40, 0, 40), (40, 2, 48), (72, 2, 80),
                                       (80, 2, 80)]:
            points = {f'pouch.id{i}': 100 for i in range(8)}  # Heart Medal gives no capacity.
            points.update({f'pouch.id{i}': 114 for i in range(medals)})
            effective = expr.replace('hearts.base_q', str(base))
            effective = re.sub(r'pouch\.id\d', lambda m: str(points[m[0]]), effective)
            self.assertEqual(eval(effective, {'__builtins__': {},
                                             'clamp': lambda n, lo, hi: min(max(n, lo), hi)}),
                             expected)

    def test_shield_scans_every_pouch_slot_without_raw_point_collision(self):
        manifest, data = generator.build()
        derived = {v['name']: v for v in manifest['derived']}
        self.assertFalse(set(data['points']) & set(derived))
        expr = derived['shield.id']['expr']
        for slot in range(8):
            for shield in range(116, 126):
                points = {f'pouch.id{i}': 153 for i in range(8)}
                points[f'pouch.id{slot}'] = shield
                self.assertEqual(value(expr, points), shield)
        self.assertEqual(value(expr, {f'pouch.id{i}': 153 for i in range(8)}), 0)

    def test_gear_is_equipment_only_and_bag_holds_pouch_collection_settings(self):
        manifest, _ = generator.build()
        pages = {p['id']: p for p in manifest['pages']}
        taps = lambda pg: [w['on_tap'] for w in pages[pg]['widgets'] if 'on_tap' in w]
        # Back to Ready, or to Flying in the Sky (dev.30); dev.37: the Sword / Shield column takes no tap
        self.assertEqual(taps('gear'), ['go_ready', 'go_fly'])
        for pg in ('gpouch', 'coll', 'settings'):
            self.assertEqual(taps(pg)[:3], ['go_gpouch', 'go_coll', 'go_set'])
        self.assertIn('go_gpouch', taps('ready'))
        self.assertIn('go_gpouch', taps('fly'))
        for pg in ('ready', 'fly'):
            self.assertNotIn('go_coll', taps(pg))
            self.assertTrue(any(w.get('src', '').endswith('/Bag') for w in pages[pg]['widgets']))
        settings_tab = [w for w in pages['settings']['widgets'] if w.get('rect', [0, 99])[1] < 40]
        self.assertTrue(any(w.get('src', '').endswith(':txt/optrow/28/Options') for w in settings_tab))  # dev.46 alt B
        tab_rects = [w['rect'] for w in pages['coll']['widgets'] if w.get('on_tap') in ('go_gpouch', 'go_coll', 'go_set')]
        self.assertTrue(all(r[2] >= 293 and r[3] >= 96 for r in tab_rects))

    def test_gear_has_title_bar_and_equip_hub(self):
        manifest, _ = generator.build()
        gear = next(p for p in manifest['pages'] if p['id'] == 'gear')['widgets']
        self.assertTrue(any(w.get('src', '').endswith(':txt/optrow/30/Gear') and w['rect'][1] < 128 for w in gear))  # dev.46 alt B
        hub = [w for w in gear if w.get('drop_action') == 'bitem_equip']
        self.assertEqual(len(hub), 1)
        self.assertEqual(hub[0]['accept_group'], 'bwheel')
        ring = [w for w in gear if w.get('select_group') == 'bwheel']
        self.assertEqual(sorted(w['payload'] for w in ring), list(range(8)))
        self.assertTrue(all(w['draggable'] and w['need_bind'] == f"sshd.w{w['payload']}.on" for w in ring))
        hx, hy, hw, hh = hub[0]['rect']
        for w in ring:  # ring items stay outside the hub so a tap on one never hits the hub
            x, y, ww, wh = w['rect']
            self.assertTrue(x + ww <= hx or x >= hx + hw or y + wh <= hy or y >= hy + hh)
        self.assertTrue(any(w.get('src_bind') == 'sshd.bcur.src' for w in gear))
        self.assertTrue(any(w.get('bind') == 'pouch.rid' for w in gear))
        act = manifest['actions']['bitem_equip']
        self.assertEqual((act['kind'], act['argument']), ('module', '$payload'))

    def test_pouch_hub_equips_and_item_check_scrolls_three_columns(self):
        manifest, data = generator.build()
        page = next(p for p in manifest['pages'] if p['id'] == 'gpouch')
        hub = [w for w in page['widgets'] if w.get('drop_action') == 'pouch_equip']
        self.assertEqual(len(hub), 1)
        self.assertEqual(hub[0]['accept_group'], 'pouch')
        ring = [w for w in page['widgets'] if w.get('select_group') == 'pouch' and isinstance(w.get('payload'), int)]
        self.assertEqual(sorted(w['payload'] for w in ring), list(range(8)))
        self.assertTrue(all(w['rect'][2] >= 124 for w in ring))  # bigger ring icons (were 104)
        region = {r['id']: r for r in page['scrolls']}['icheck']
        self.assertEqual(region['cols'], 3)
        cells = [w for w in page['widgets'] if w.get('scroll') == 'icheck' and w.get('drop_action') == 'stock_to_{i}' and w['type'] == 'rect']
        self.assertEqual(len(cells), 1)
        cell = cells[0]
        self.assertEqual((cell['repeat'], cell['repeat_cols']), (60, 3))
        self.assertGreaterEqual(cell['rect'][2], 104)  # touch targets at least the old 104 x 72
        self.assertGreaterEqual(cell['rect'][3], 72)
        self.assertEqual(region['row_h'], cell['repeat_row_dy'])
        self.assertLessEqual(region['rect'][0], cell['rect'][0])
        self.assertLessEqual(cell['rect'][0] + 3 * cell['repeat_dx'], region['rect'][0] + region['rect'][2])
        # every row reachable: content (20 rows) is taller than the viewport, last row fits after scrolling
        content = 20 * region['row_h'] + region['pad']
        self.assertGreater(content, region['rect'][3])
        last_bottom = cell['rect'][1] - region['rect'][1] + 19 * region['row_h'] + cell['rect'][3]
        self.assertLessEqual(last_bottom, content)
        self.assertEqual(data['points']['pouch.r']['addr'], f"main+{generator.SLOT + 0x53B0:#x}")
        derived = {d['name']: d for d in manifest['derived']}
        for slot in range(8):
            points = {f'pouch.id{i}': 153 for i in range(8)}
            points[f'pouch.id{slot}'] = 72
            expr = derived['pouch.rid']['expr'].replace('pouch.r ', f'{slot} ')
            self.assertEqual(value(expr, points), 72)

    def test_ready_right_column_is_zr_and_r_corners_and_minimap_zoomed_out(self):
        manifest, _ = generator.build()
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        colx = generator.W - 16 - generator.CB
        right = [w for w in ready if w.get('rect', [0])[0] >= colx and w['type'] != 'map']
        planks = [w for w in right if '/plank/corner/' in w.get('src', '')]
        left = [w for w in ready if w.get('rect', [99])[0] == 16 and '/plank/corner/' in w.get('src', '')]
        self.assertEqual(sorted(w['rect'][1] for w in planks), sorted(w['rect'][1] for w in left))
        self.assertFalse(any('sshd.sc1' in json_text(w) or 'sshd.sc2' in json_text(w) or 'sshd.sc3' in json_text(w)
                             for w in ready))
        taps = {w['on_tap']: w['rect'] for w in right if 'on_tap' in w}
        self.assertEqual(set(taps), {'go_gear', 'l_toggle', 'spin_h', 'spin_v'})
        self.assertLess(taps['go_gear'][1], taps['l_toggle'][1])  # ZR top, R bottom
        zr = next(w for w in ready if w.get('src', '').endswith('/tr_zrBtn_00'))
        r = next(w for w in ready if w.get('src', '').endswith('/tr_rBtn_00'))
        self.assertLess(zr['rect'][1], 228)
        self.assertGreater(r['rect'][1] + r['rect'][3], generator.H - 16 - generator.CB)
        zricon = next(w for w in ready if w.get('src_bind') == 'sshd.sc0.src')
        self.assertLess(zricon['rect'][1], 228)
        ricon = next(w for w in ready if w.get('bind') == 'pouch.rid')
        # glyph badges on the inner corner: ZR bottom-left of its button, R top-left of its button;
        # the item icon centred in the button and larger than the old 104 px, digits clear of the glyph
        cb = generator.CB
        for glyph, icon, top, bottom_half, nd in [(zr, zricon, taps['go_gear'][1], True, 'sshd.sc0.nd'),
                                                  (r, ricon, taps['l_toggle'][1], False, 'pouch.rnd')]:
            gx, gy, gw, gh = glyph['rect']
            # the badge straddles the frame corner: centred on it, half outside, half inside
            self.assertEqual((gx + gw / 2, gy + gh / 2), (colx if bottom_half else 16 + cb, top + cb if bottom_half else top))
            self.assertEqual(gx + gw / 2, colx if bottom_half else 16 + cb)
            if bottom_half:
                self.assertGreater(gy + gh / 2, top + cb * 3 / 4)
            else:
                self.assertLess(gy + gh / 2, top + cb / 4)
            ix, iy, iw, ih = icon['rect']
            self.assertGreater(iw, 104)
            self.assertEqual((ix + iw / 2, iy + ih / 2), ((colx if bottom_half else 16) + cb / 2, top + cb / 2))
            self.assertTrue((colx if bottom_half else 16) <= ix and ix + iw <= (colx if bottom_half else 16) + cb and top <= iy and iy + ih <= top + cb)
            for d in (w for w in ready if str(w.get('bind', '')).startswith(nd)):
                dx, dy, dw, dh = d['rect']
                self.assertTrue(dx >= gx + gw or dy >= gy + gh or dy + dh <= gy)
                self.assertTrue(dx + dw <= (colx if bottom_half else 16) + cb and dy + dh <= top + cb)
        self.assertEqual(ricon['need_bind'], 'pouch.ron')
        derived = {d['name']: d for d in manifest['derived']}
        self.assertIn('pouch.ron', derived['pouch.rcnt']['expr'])
        # swipe / stick pads stay clear of every corner
        for pad in (w for w in ready if str(w.get('id', '')).startswith(('swipe_', 'stickpad_'))):
            px, py, pw, ph = pad['rect']
            for c in planks + left:
                cx, cy = c['rect'][:2]
                cb = generator.CB
                self.assertTrue(px >= cx + cb + 8 or px + pw <= cx - 8 or py >= cy + cb + 8 or py + ph <= cy - 8)
        windows = {w['follow_window'] for w in ready if w['type'] == 'map' and w['area'] != 'GEN'
                   and w['area'] not in generator.REALMS and w['area'] != 'F023'}
        self.assertEqual(windows, {generator.READY_WINDOW})
        # dev.34: the generic frame's window is in its own frame units
        self.assertEqual({w['follow_window'] for w in ready if w['type'] == 'map' and w['area'] == 'GEN'},
                         {generator.GEN_READY_WINDOW})
        self.assertTrue(1.4 <= generator.READY_WINDOW / 8000 <= 1.6)
        mapfull = next(p for p in manifest['pages'] if p['id'] == 'mapfull')['widgets']
        self.assertIn(8000 * 1.8, {w.get('follow_window') for w in mapfull})  # Map page unchanged

    def test_ready_left_corners_shield_bash_and_l_toggle(self):
        manifest, _ = generator.build()
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        cb, g = generator.CB, generator.CORNER_GLYPH
        by = generator.H - 16 - cb
        taps = {w['on_tap']: w for w in ready if 'on_tap' in w and w.get('on_tap') in ('shield_bash', 'l_toggle')}
        self.assertEqual(set(taps), {'shield_bash', 'l_toggle'})
        self.assertEqual(taps['shield_bash']['rect'], [16, 16, cb, cb])
        self.assertNotIn('on_hold', taps['shield_bash'])  # dev.37: no inspector behind the shield
        self.assertEqual(taps['l_toggle']['rect'], [generator.W - 16 - cb, by, cb, cb])
        acts = manifest['actions']
        self.assertEqual(acts['shield_bash'], {'kind': 'module', 'action': 'shield_bash', 'argument': 0})
        self.assertEqual(acts['l_toggle'], {'kind': 'module', 'action': 'l_toggle', 'argument': 0})
        self.assertNotIn('open_pouch', acts)  # the leftover pouch list is gone
        self.assertNotIn('pouch', {p['id'] for p in manifest['pages']})
        # no text on either corner except the refusal flash; no beacon count / Markers label
        left = [w for w in ready if w.get('rect', [999])[0] < 16 + cb and
                (w['rect'][1] < 16 + cb or w['rect'][1] >= by)]
        texts = [w['src'] for w in left if ':txt/' in w.get('src', '')]
        # dev.40: "Put away" instead of "Not now" when an item in Link's hands is the reason; dev.46 P01:
        # both on the refusal pill under the plank, nothing across the shield socket any more
        self.assertEqual(texts, [])
        under = [w['src'] for w in ready if ':txt/doinfo/' in w.get('src', '') and w['rect'][0] < 16 + cb
                 and 16 + cb <= w['rect'][1] < 265]
        self.assertEqual(under, [generator.K + 'txt/doinfo/22/Not now', generator.K + 'txt/doinfo/22/Put away'])
        self.assertFalse(any('sshd.bc.n' in json_text(w) for w in ready))
        # centred socket 156 + icon 132, like ZR / R
        for top, icon_match in [(16, lambda w: w.get('bind') == 'shield.id'),
                                (by, lambda w: w.get('src', '').endswith('/tr_cameraMark_00'))]:
            centre = ((16 if top == 16 else generator.W - 16 - cb) + cb / 2, top + cb / 2)
            sock = next(w for w in ready if w.get('src', '').endswith(f':socket/{generator.CORNER_SOCKET}')
                        and w['rect'][1] < top + cb and w['rect'][1] >= top and abs(w['rect'][0] + w['rect'][2] / 2 - centre[0]) < 1)
            icon = next(w for w in ready if icon_match(w))
            for wd, size in ((sock, generator.CORNER_SOCKET), (icon, generator.CORNER_ICON)):
                x, y, w, h = wd['rect']
                self.assertEqual((w, h), (size, size))
                self.assertEqual((x + w / 2, y + h / 2), centre)
        # L corner (dev.37): the eye while the game sees L held or the lock is on (sshd.l.eye), else the
        # current sword (none without one); L badge on the inner (top-right) corner
        eye = next(w for w in ready if w.get('src', '').endswith('/tr_cameraMark_00'))
        self.assertEqual(eye['need_bind'], 'sshd.l.eye')
        sword = next(w for w in ready if w.get('src_bind') == 'sshd.sword.src')
        derived = {d['name']: d for d in manifest['derived']}
        self.assertEqual(derived[sword['need_bind']]['expr'], 'sshd.sword.has && !sshd.l.eye')
        # the lock: the native locked-door keyhole (art doorkey) straddling the frame's top-left corner
        lock = next(w for w in ready if w.get('src') == generator.K + 'doorkey')
        self.assertEqual(lock['need_bind'], 'sshd.l.on')
        lx, ly, lw, lh = lock['rect']
        self.assertEqual((lw, lh), (generator.L_LOCK, generator.L_LOCK))
        self.assertEqual(ly + lh / 2, by)  # straddles the frame's top edge
        self.assertTrue(generator.W - 16 <= lx + lw <= generator.W)  # at its left corner, on the canvas
        x, y, w, h = sword['rect']
        self.assertEqual((x + w / 2, y + h / 2), (generator.W - 16 - cb / 2, by + cb / 2))
        badge = next(w for w in ready if w.get('src', '').endswith('/tr_lBtn_01'))
        bx, byy, bw, bh = badge['rect']
        self.assertEqual((bw, bh), (g, g))
        # straddles the frame's top-right corner (half outside, half inside), clear of the footer
        self.assertEqual((bx + bw / 2, byy + bh / 2), (generator.W - 16 - cb, by))
        self.assertLessEqual(byy + bh, 930)
        # the virtual L hold: dev.45 the guest helper holds L (Dev45Test); no runtime enforce rule
        self.assertNotIn('enforce', manifest)
        self.assertNotIn('enforce_gate', manifest)
        self.assertNotIn('l_hold', acts)
        # the stick pad (swipe = sword) still covers the minimap circle
        pad = next(w for w in ready if w.get('id') == 'stickpad_0')
        px, py, pw, ph = pad['rect']
        self.assertTrue(px <= generator.MX - generator.MR and px + pw >= generator.MX + generator.MR)
        self.assertTrue(py <= generator.MY - generator.MR and py + ph >= generator.MY + generator.MR)

    def test_no_tap_can_press_a_native_menu_button(self):
        # dev.37 item 5 (Thor round 2: the native Pouch wheel was open after two Map taps): the only
        # game buttons the package can press are the right-stick slash pulses (stick fallback) and
        # the owner-approved virtual L of the L lock (dev.45: held by the guest helper). No action
        # presses R / ZR / Plus / Minus / X / Y / A / B / ZL or a D-pad direction, so no companion tap
        # can open the Pouch / Item wheel, the pause menu or the native map. (The module's helper ops
        # press nothing either: pouch / equip ops call the game's own routines, the bash adds L3.)
        manifest, _ = generator.build()
        buttons = {k: v['button'] for k, v in manifest['actions'].items() if v.get('kind') == 'button'}
        self.assertEqual(buttons, {'slash_up': 'RStick(0,1)', 'slash_down': 'RStick(0,-1)',
                                   'slash_left': 'RStick(-1,0)', 'slash_right': 'RStick(1,0)'})
        self.assertEqual({a['kind'] for a in manifest['actions'].values()},
                         {'button', 'module', 'page', 'flag', 'view_reset'})
        self.assertNotIn('enforce', manifest)  # dev.45: the L lock's L comes from the helper
        used = set()
        for p in manifest['pages']:
            for w in p['widgets']:
                for k in ('on_tap', 'on_hold', 'drop_action', 'on_swipe_up', 'on_swipe_down', 'on_swipe_left',
                          'on_swipe_right'):
                    if k in w:
                        used.add(w[k])
        self.assertNotIn('l_hold', used)
        # the Ready Map footer button is a plain page switch (live: Thor log tap (619,1000) -> go_map)
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        hit = [w for w in ready if 'on_tap' in w and w['rect'][0] <= 619 < w['rect'][0] + w['rect'][2]
               and w['rect'][1] <= 997 < w['rect'][1] + w['rect'][3]]
        self.assertEqual([w['on_tap'] for w in hit], ['go_map'])
        self.assertEqual(manifest['actions']['go_map']['kind'], 'page')

    def test_gear_sword_and_shield_are_display_only(self):
        # dev.37 (owner 2026-11-12): no tap or drag on the Gear page's sword / shield, and no
        # Equipment inspector page or action anywhere
        manifest, _ = generator.build()
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        self.assertNotIn('equip', pages)
        self.assertNotIn('open_equip', manifest['actions'])
        self.assertNotIn('close_pouch', manifest['actions'])
        self.assertNotIn('open_equip', json_text(manifest))
        column = [40, 160, 360, 700]  # the Sword / Shield panel

        def inside(w):
            x, y, ww, hh = w.get('rect', [-1, -1, 0, 0])
            return x >= column[0] and y >= column[1] and x + ww <= column[0] + column[2] + 16 and \
                y + hh <= column[1] + column[3] + 16
        col = [w for w in pages['gear'] if inside(w)]
        self.assertTrue(any(w.get('src_bind') == 'sshd.sword.src' for w in col))
        self.assertTrue(any(w.get('bind') == 'shield.id' for w in col))
        for w in col:
            for k in ('on_tap', 'on_hold', 'draggable', 'drop_action', 'select_group'):
                self.assertNotIn(k, w)
        for page in pages.values():
            for w in page:
                if w.get('bind') == 'shield.id' or w.get('src_bind') == 'sshd.sword.src':
                    self.assertFalse(w.get('draggable'))

    def test_ready_spin_buttons_sit_between_the_right_corners(self):
        manifest, _ = generator.build()
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        cb, colx = generator.CB, generator.W - 16 - generator.CB
        zr_bottom, r_top = 16 + cb, generator.H - 16 - cb
        buttons = {w['on_tap']: w for w in ready if w.get('on_tap') in ('spin_h', 'spin_v')}
        self.assertEqual(set(buttons), {'spin_h', 'spin_v'})
        h, v = buttons['spin_h']['rect'], buttons['spin_v']['rect']
        self.assertLess(h[1], v[1])  # horizontal above vertical
        for x, y, w, hh in (h, v):  # inside the column, clear of both corners, touch target >= 160
            self.assertTrue(x >= colx and x + w <= colx + cb)
            self.assertTrue(y >= zr_bottom + 16 and y + hh <= r_top - 16)
            self.assertGreaterEqual(min(w, hh), 160)
        self.assertLessEqual(h[1] + h[3], v[1])
        for act, stick in (('spin_h', 'tr_rStick_04'), ('spin_v', 'tr_rStick_02')):
            self.assertEqual(manifest['actions'][act], {'kind': 'module', 'action': act, 'argument': 0})
            sock = next(w for w in ready if w.get('id') == act + '_socket')
            self.assertEqual(sock['rect'][2], sock['rect'][3])  # round socket, not a plank
            self.assertIn(':socket/', sock['src'])
            x, y, w, hh = buttons[act]['rect']
            inside = [w2 for w2 in ready if w2.get('type') == 'image' and x <= w2['rect'][0] < x + w and
                      y <= w2['rect'][1] < y + hh]
            # native art only: the controller tutorial's right stick, native arrows, native words
            self.assertTrue(any(w2['src'].endswith('/' + stick) for w2 in inside))
            self.assertTrue(all(w2['src'].startswith(generator.K) for w2 in inside))
            self.assertFalse(any(b in w2['src'] for w2 in inside for b in ('zlBtn', 'lStick', 'aBtn')))
            # native icons only: no caption on the button; "Not now" flashes just below it on refusal
            self.assertFalse(any(':txt/' in w2['src'] for w2 in inside))
            # dev.46 P01: on the refusal pill under the socket (dev.48: PILL_GAP px under it)
            below = [w2 for w2 in ready if ':txt/' in w2.get('src', '') and x <= w2['rect'][0] < x + w and
                     y + hh <= w2['rect'][1] < y + hh + 40]
            self.assertEqual([(w2['src'].rsplit('/', 1)[1], w2['need_bind']) for w2 in below],
                             [('Not now', 'sshd.spin.notnow'), ('Put away', 'sshd.spin.putaway')])
            pill = [w2 for w2 in ready if ':dopill/' in w2.get('src', '') and w2['rect'][1] == y + hh + generator.PILL_GAP]
            self.assertEqual([w2['need_bind'] for w2 in pill], ['sshd.spin.refused'])
        self.assertFalse(any(w.get('src', '').endswith('/Spin') for w in ready))
        # re-centred: three equal gaps between the ZR corner, the two buttons and the R corner
        gaps = [h[1] - zr_bottom, v[1] - (h[1] + h[3]), r_top - (v[1] + v[3])]
        self.assertLessEqual(max(gaps) - min(gaps), 1)
        # the stick / swipe pads never reach the column
        for pad in (w for w in ready if str(w.get('id', '')).startswith(('swipe_', 'stickpad_'))):
            self.assertLessEqual(pad['rect'][0] + pad['rect'][2], colx - 8)

    def test_ready_dpad_cluster_is_display_only_and_gated_per_layer(self):
        manifest, _ = generator.build()
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        dp = [w for w in ready if ':dpad/' in w.get('src', '') or ':txt/hud/' in w.get('src', '')]
        binds = {w['need_bind'] for w in dp}
        self.assertEqual(binds, {f'sshd.dp.{k}' for k in (
            'base_c', 'base_d', 'base_h', 'up', 'down', 'left', 'right', 'eye', 'fi', 'fis0', 'fis1', 'fih0',
            'fih1', 'bird', 'target', 'look', 'dowse', 'back', 'help', 'close')})
        for w in dp:
            # every layer hidden at level 0 and drawn at the native pane's alpha level otherwise
            self.assertEqual(w['tint_bind'], w['need_bind'])
            self.assertEqual(len(w['tint_colors']), 16)
            self.assertEqual(w['tint_colors'][15], '#FFFFFFFF')
            self.assertEqual(w['tint_colors'][8], '#88FFFFFF')
            for k in ('on_tap', 'on_hold', 'drop_action', 'draggable', 'payload'):
                self.assertNotIn(k, w)  # display only
        # no tap target of any kind over the cluster
        xs = [w['rect'][0] for w in dp] + [w['rect'][0] + w['rect'][2] for w in dp]
        ys = [w['rect'][1] for w in dp] + [w['rect'][1] + w['rect'][3] for w in dp]
        box = (min(xs), min(ys), max(xs), max(ys))
        for w in ready:
            if 'on_tap' in w or 'on_hold' in w:
                x, y, ww, hh = w['rect']
                self.assertTrue(x >= box[2] or x + ww <= box[0] or y >= box[3] or y + hh <= box[1], w)
        # native art and captions only, the target's art from the module's checked key
        self.assertTrue(all(w['src'].startswith(generator.K) for w in dp))
        target = next(w for w in dp if w.get('src_bind') == 'sshd.dp.tsrc')
        self.assertEqual(target['need_bind'], 'sshd.dp.target')
        caps = {w['src'].rsplit('/', 1)[1]: w['need_bind'] for w in dp if ':txt/hud/' in w['src']}
        self.assertEqual(caps, {'Look': 'sshd.dp.look', 'Dowse': 'sshd.dp.dowse', 'Back': 'sshd.dp.back',
                                'Help': 'sshd.dp.help', 'Close': 'sshd.dp.close'})
        # in the left pane between the corners, below the rupees; the stamina wheel is gone
        cb = generator.CB
        self.assertGreaterEqual(box[1], 16 + cb + 16)
        self.assertLessEqual(box[3], generator.H - 16 - cb - 8)
        self.assertGreaterEqual(box[0], 0)
        rupee = next(w for w in ready if w.get('src', '').endswith('/tr_rupy_00'))
        digits = [w for w in ready if str(w.get('bind', '')).startswith('ru.d')]
        self.assertTrue(all(d['rect'][1] + d['rect'][3] <= box[1] for d in digits + [rupee]))
        self.assertFalse(any(':stamina/' in w.get('src', '') for w in ready))
        # cross pieces share one centre; Fi below, eye above, Help to the right (native layout)
        def centre(w):
            x, y, ww, hh = w['rect']
            return x + ww / 2, y + hh / 2
        base = next(w for w in dp if w['need_bind'] == 'sshd.dp.base_c')
        cx, cy = centre(base)
        for k in ('base_d', 'base_h', 'up', 'right', 'down'):
            w = next(w for w in dp if w['need_bind'] == 'sshd.dp.' + k)
            self.assertLessEqual(max(abs(a - b) for a, b in zip(centre(w), (cx, cy))), 1)
        fi, eye = (next(w for w in dp if w['need_bind'] == 'sshd.dp.' + k) for k in ('fi', 'eye'))
        self.assertGreater(centre(fi)[1], cy + 60)
        self.assertLess(centre(eye)[1], cy - 50)
        help_ = next(w for w in dp if w['need_bind'] == 'sshd.dp.help')
        self.assertGreater(help_['rect'][0], cx + base['rect'][2] / 2 - 10)

    def test_stamina_ring_drains_clockwise_and_hides_when_full(self):
        manifest, _ = generator.build()
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        derived = {d['name']: d for d in manifest['derived']}
        segs = [w for w in ready if ':stamseg/' in w.get('src', '')]
        self.assertEqual(len(segs), generator.STAM_SEGMENTS)
        track = next(w for w in ready if ':stamtrack/' in w.get('src', ''))
        self.assertEqual(track['need_bind'], 'stam.ring')
        # dev.34: the track turns red with the native exhausted palette (0 stamina: no segment left)
        self.assertEqual((track['bind'], track['src_format']),
                         ('sshd.stam.red', generator.K + 'stamtrack/%d/%d/%%d' % generator.STAM_R))
        self.assertEqual(derived['stam.ring']['expr'], 'stamina < 1000000')

        def shown(stamina):
            out = []
            for w in segs:
                expr = derived[w['need_bind']]['expr'].replace('stam.ring', str(int(stamina < 1000000)))
                if eval(expr.replace('&&', ' and ').replace('stamina', str(stamina)), {'__builtins__': {}}):
                    out.append(w['rotate'])
            return out
        self.assertEqual(shown(1000000), [])  # full: hidden like the native gauge
        self.assertEqual(shown(0), [])
        half = shown(500000)
        self.assertEqual(len(half), 50)
        self.assertGreater(min(half), 180)  # the kept half is the last half of the clockwise turn
        self.assertEqual(len(shown(990000)), 99)
        self.assertEqual(min(shown(990000)), 3.6 * 1.5)  # the first sliver after 12 o'clock went first
        self.assertEqual(len(shown(5000)), 1)
        self.assertGreater(shown(5000)[0], 356)  # last sliver just left of 12 o'clock
        # every segment turns about the minimap centre
        for w in segs:
            x, y = w['rect'][:2]
            px, py = w['pivot']
            self.assertEqual((x + px, y + py), (generator.MX, generator.MY))
        # ring geometry: outside the map frame's rim, inside the corner exclusion zones, under the
        # compass and the rim counts (drawn before them), clear of the crystal / cube counters
        rin, rout = generator.STAM_R
        self.assertGreater(rin, generator.MR + 18)
        cb = generator.CB
        self.assertGreater(generator.MX - rout - 4, 16 + cb + 8)
        self.assertLess(generator.MX + rout + 4, generator.W - 16 - cb - 8)
        idx = {id(w): i for i, w in enumerate(ready)}
        compass = next(w for w in ready if w.get('src', '').endswith(':compass'))
        counters = [w for w in ready if w.get('src', '').endswith('/uk_genki_00') or
                    w.get('src', '').endswith('/ts_GdsCube_00') or
                    str(w.get('bind', '')).startswith('crystals.d') or w.get('bind') == 'sshd.cubes']
        for w in [compass] + counters:
            self.assertGreater(idx[id(w)], max(idx[id(s)] for s in segs))
        for w in counters:
            x, y, ww, hh = w['rect']
            shifts = [0, -27] if str(w.get('bind', '')).startswith('crystals.d') else [0]
            for sh in shifts:
                # nearest point of the counter's box to the map centre stays outside the ring
                nx = min(max(generator.MX, x + sh), x + sh + ww)
                ny = min(max(generator.MY, y), y + hh)
                self.assertGreater(((nx - generator.MX) ** 2 + (ny - generator.MY) ** 2) ** 0.5, rout + 4, w)
        # the area title sits below the ring
        titles = [w for w in ready if ':txt/title/' in w.get('src', '') and str(w.get('need_bind', '')).startswith('stage.is_')]
        self.assertTrue(titles)
        self.assertTrue(all(w['rect'][1] >= generator.MY + rout + 4 for w in titles))

    def test_collection_is_the_native_grid_without_scrolling(self):
        manifest, data = generator.build()
        page = next(p for p in manifest['pages'] if p['id'] == 'coll')
        ws = page['widgets']
        self.assertNotIn('scrolls', page)  # fits the page like the native screen
        self.assertFalse(any('scroll' in w for w in ws))
        # three native panels: green bugs, purple quest items, tan treasures
        panels = {int(w['src'].rsplit('/', 1)[1]): w['rect'] for w in ws if ':cpanel/' in w.get('src', '')}
        self.assertEqual(set(panels), {0, 1, 2})
        g, pu, tan = panels[0], panels[1], panels[2]
        self.assertLess(g[0] + g[2], tan[0] + 24 + 1)       # bugs left of treasures
        self.assertLess(g[1], pu[1])                        # quest row below the bugs
        self.assertAlmostEqual(g[0], pu[0], delta=1)
        for x, y, w, h in panels.values():                  # between the tabs and the footer
            self.assertTrue(x >= 0 and x + w <= generator.W and y >= 128 and y + h <= 930)
        # 12 bug + 4 quest + 16 treasure slots; 4 x 3, 4 x 1 and 4 x 4 grids
        slots = [w for w in ws if ':cslot/' in w.get('src', '')]
        by_kind = {k: [w['rect'] for w in slots if w['src'].endswith(f'/{k}')] for k in range(3)}
        self.assertEqual([len(by_kind[k]) for k in range(3)], [12, 4, 16])
        for k, (cols, rows) in {0: (4, 3), 1: (4, 1), 2: (4, 4)}.items():
            self.assertEqual(len({r[0] for r in by_kind[k]}), cols)
            self.assertEqual(len({r[1] for r in by_kind[k]}), rows)
            px, py, pw, ph = panels[k]
            for x, y, w, h in by_kind[k]:                   # every slot inside its panel's window
                self.assertTrue(px + 24 <= x and x + w <= px + pw and py <= y and y + h <= py + ph - 24)
        # each item: its native icon when the flag says displayed, else the native silhouette
        derived = {d['name']: d for d in manifest['derived']}
        for prefix, n in (('B', 12), ('T', 16), ('Q', 4)):
            for k in range(n):
                on = f'coll.{prefix}{k}.on'
                icon = [w for w in ws if w.get('need_bind') == on and ':sil/' not in w['src'] and ':cnum/' not in w['src']]
                sil = [w for w in ws if w.get('need_bind') == on + '.no']
                self.assertEqual(len(icon), 1)
                self.assertEqual(len(sil), 1)
                self.assertEqual(sil[0]['src'], icon[0]['src'].replace(generator.K, generator.K + 'sil/'))
                self.assertEqual(derived[on + '.no']['expr'], f"!({derived[on]['expr']})")
        icons = {w['src'] for w in ws if str(w.get('need_bind', '')).startswith('coll.T') and
                 w['need_bind'].endswith('.on') and ':cnum/' not in w['src']}
        self.assertEqual(len(icons), 16)
        # counts bottom-right of each bug / treasure icon in the game's number font, crystals while collecting
        counts = [w for w in ws if ':cnum/' in w.get('src', '')]
        self.assertEqual(len(counts), 12 + 16 + 1)
        self.assertTrue(all(w['src_format'].endswith('/%d') for w in counts))
        crystal = next(w for w in counts if w['bind'] == 'sshd.crystals')
        self.assertIn('!(', derived[crystal['need_bind']]['expr'])  # hidden once the quest is finished
        # four red check marks, one per quest item, shown once given
        checks = [w for w in ws if ':ccheck/' in w.get('src', '')]
        self.assertEqual(sorted(w['need_bind'] for w in checks), [f'coll.Q{k}.done' for k in range(4)])
        # flags: item-flag words in the save slot, story flags in the live block
        self.assertEqual(data['points']['iflag.9f2']['addr'], f"main+{generator.SLOT + 0x9F2:#x}")
        self.assertEqual(data['points']['sflag.12']['addr'], 'main+0x182e20a')

    def test_collection_flag_bits_follow_the_item_flag_table(self):
        manifest, _ = generator.build()
        derived = {d['name']: d for d in manifest['derived']}

        def shown(name, points):
            expr = derived[name]['expr']
            for k, v in points.items():
                expr = expr.replace(k, str(v))
            expr = expr.replace('&&', ' and ').replace('||', ' or ').replace('!(', ' not (')
            return eval(expr, {'__builtins__': {}, 'floor': lambda x: int(x // 1)})
        zero = {'iflag.9f2': 0, 'iflag.9f4': 0, 'iflag.9f6': 0, 'iflag.9fc': 0,
                'sflag.12': 0, 'sflag.28': 0, 'sflag.34': 0, 'sflag.38': 0, 'sflag.3e': 0}
        # Blessed Butterfly (146) = byte 0x9F3 bit 7 = word 0x9F2 bit 15; Goddess Plume (176) = 0x9F7 bit 1
        self.assertFalse(shown('coll.B1.on', zero))
        self.assertTrue(shown('coll.B1.on', {**zero, 'iflag.9f2': 1 << 15}))
        self.assertTrue(shown('coll.T15.on', {**zero, 'iflag.9f6': 1 << 9}))
        self.assertFalse(shown('coll.T15.on', {**zero, 'iflag.9f6': 1 << 8}))
        # Deku Hornet by story flag 122 (live byte 0x13 bit 7), not its item flag
        self.assertTrue(shown('coll.B0.on', {**zero, 'sflag.12': 1 << 15}))
        self.assertFalse(shown('coll.B0.on', {**zero, 'iflag.9f2': 1 << 12}))
        # Rattle: owned by item flag 160 (0x9FD bit 5), checked by story flag 387 (byte 0x34 bit 5)
        self.assertTrue(shown('coll.Q2.on', {**zero, 'iflag.9fc': 1 << 13}))
        self.assertFalse(shown('coll.Q2.done', {**zero, 'iflag.9fc': 1 << 13}))
        self.assertTrue(shown('coll.Q2.done', {**zero, 'sflag.34': 1 << 5}))

    def test_map_markers_follow_the_contract(self):
        manifest, _ = generator.build()
        areas = manifest['map']['areas']
        for key, a in areas.items():
            st = key[:-2] if key.endswith('_m') else key
            dms = a['dynamic_markers']
            # the Sky's thunderhead and light columns lie under everything; then the cone, so every
            # other marker and Link's head draw over it
            under = [d for d in dms if d['group'] in ('thunderhead', 'columns')]
            self.assertEqual([d['group'] for d in dms[:len(under)]], [d['group'] for d in under])
            self.assertEqual(bool(under), st in ('F020', 'F023'))
            cone = dms[len(under)]
            self.assertEqual(cone['group'], 'cone')
            self.assertEqual((cone['kind'], cone['hide_when_kind'], cone['icon_src_bind']),
                             ('sshd.cone.on', 0, 'sshd.cone.src'))
            self.assertEqual(cone['size'], 400)  # kConeCanvas
            # dev.37: right after it, the prefetch marker (module sshd_cone.h): the wanted picture is
            # requested off screen while the cone keeps the last loaded one
            pf = dms[len(under) + 1]
            self.assertEqual(pf, generator.CONE_PREFETCH)
            self.assertEqual((pf['kind'], pf['hide_when_kind'], pf['icon_src_bind'], pf['size']),
                             ('sshd.cone.pf', 0, 'sshd.cone.pfsrc', cone['size']))
            self.assertEqual((pf['x'], pf['y']), ('sshd.cone.pfu', 'sshd.cone.pfv'))
            # the cone sits on Link: the same binds as the page's Link marker
            if st in ('F020', 'F023'):
                self.assertEqual((cone['x'], cone['y']), ('sky.u', 'sky.v'))
            elif st == 'GEN':
                self.assertEqual((cone['x'], cone['y']), ('sshd.gen.lu', 'sshd.gen.lv'))
            else:
                self.assertEqual(cone['x'], generator.ubind(st))
                self.assertEqual(cone['y'], generator.vbind(st))
            groups = [d['group'] for d in dms]
            self.assertEqual('open_chests' in groups, st in generator.OPEN_CHEST_STAGES)
            # Goddess tags only on the Sky (native MapSky template); never cube positions
            self.assertEqual('goddess_chests' in groups, st in ('F020', 'F023'))
            self.assertFalse(any('cube' in g for g in groups))
            # dev.27: bird statues everywhere (the module applies the native activation rule), door
            # bars on every non-Sky map (the module draws none in dungeons or the Sky), the
            # neighbouring-area plates only on the Map page's field copies, the Sky layers on F020
            if st == 'F023':  # dev.35: the Sky's frame; its ring, columns, cone and Goddess tags
                self.assertEqual(groups, ['thunderhead', 'columns', 'cone', 'conepf', 'goddess_chests', 'statues',
                                          'balloons'])
                self.assertEqual((a['min'], a['max'], a['image']), (areas['F020']['min'], areas['F020']['max'],
                                                                     areas['F020']['image']))
                continue
            if st in generator.REALMS:  # dev.34: the native trial map draws Link's head and cone only
                self.assertEqual(groups, ['cone', 'conepf'])
                continue
            self.assertIn('statues', groups)
            self.assertEqual('doors' in groups, st != 'F020')
            self.assertEqual('places' in groups, key.endswith('_m') and key[:-2] in generator.PLACE_STAGES)
            # a Map page copy ("<stage>_m") exists for exactly the plate and dungeon stages (dev.33:
            # stage names such as F301_1 also end in "_" + a character) and the generic frame (dev.35)
            if key.endswith('_m'):
                self.assertTrue(key[:-2] in generator.PLACE_STAGES or key[:-2] in generator.DUNGEONS or key == 'GEN_m')
            else:
                self.assertEqual(key + '_m' in areas,
                                 key in generator.PLACE_STAGES or key in generator.DUNGEONS or key == 'GEN')
            self.assertEqual('island_names' in groups, st == 'F020')
            if st == 'F020':
                self.assertEqual(groups[-1], 'island_names')  # names over every icon
            for d in dms:
                if d['group'] in ('statues', 'doors', 'places', 'columns', 'island_names', 'thunderhead'):
                    self.assertEqual((d['hide_when_kind'], d['kind'].split('.')[0]), (0, 'sshd'))
                    self.assertTrue(d['icon_src_bind'].endswith('.src'))
            self.assertFalse(any(g in ('save', 'exits') for g in groups))
        # the Map page shows the plate copies; the Ready minimap the plain areas
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        for w in pages['mapfull']:
            if w.get('type') == 'map':
                st = w['area'][:-2] if w['area'].endswith('_m') else w['area']
                self.assertEqual(w['area'].endswith('_m'),
                                 st in generator.PLACE_STAGES or st in generator.DUNGEONS or st == 'GEN')
        # the Fly page frames the whole Sky picture, so no tag is cut at the top edge
        fly = next(p for p in manifest['pages'] if p['id'] == 'fly')['widgets']
        sky = next(w for w in fly if w.get('type') == 'map')
        (u0, v0), (u1, v1) = areas['F020']['min'], areas['F020']['max']
        win = sky['follow_window']
        rw, rh = sky['rect'][2:]
        self.assertGreaterEqual(win, u1 - u0)
        self.assertGreaterEqual(win * rh / rw, v1 - v0)

    def test_back_footer_says_back_and_maps_use_the_native_plate_and_compass(self):
        manifest, _ = generator.build()
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        self.assertFalse(any('Back to' in w.get('src', '') for ws in pages.values() for w in ws))
        for pg in ('mapfull', 'gear', 'gpouch', 'coll', 'settings'):
            self.assertTrue(any(w.get('src', '').endswith(':txt/choice/30/Back') for w in pages[pg]), pg)  # dev.46 alt A
        for pg in ('mapfull', 'fly'):
            ws = pages[pg]
            self.assertFalse(any(':ribbon/' in w.get('src', '') for w in ws))
            plates = [w for w in ws if ':areaplate/' in w.get('src', '')]
            self.assertTrue(plates)
            for p in plates:  # the title sits on its own plate, in the plate's text style
                # dev.34: the map plates carry the native MapText name (ctitle); GEN's is bound
                title = next(w for w in ws if (':txt/plate/' in w.get('src', '') or ':ctitle/plate/' in w.get('src', '')
                                               or w.get('src_bind') == 'sshd.gen.ptext') and
                             w.get('need_bind') == p.get('need_bind'))
                x, y, w, h = p['rect']
                tx, ty, tw, th = title['rect']
                self.assertTrue(x < tx and tx + tw < x + w and y <= ty and ty + th <= y + h)
            self.assertEqual(sum(1 for w in ws if w.get('src', '').endswith(':compass')), 1)
        # every area has a plate colour
        self.assertEqual({st for st, *_ in generator.AREAS} - set(generator.REGION_COLOR), set())
        # Ready title: glyph ink centred between the circle's bottom and the footer planks
        ready = pages['ready']
        t = next(w for w in ready if w.get('src', '').endswith(':txt/title/34/Sealed Grounds'))
        ink = (t['rect'][1] + generator.TITLE_INK[0] + t['rect'][1] + generator.TITLE_INK[1]) / 2
        self.assertLessEqual(abs(ink - sum(generator.TITLE_GAP) / 2), 1)

    def test_label_fit_zones_cover_what_the_pages_draw_over_their_maps(self):
        """dev.28: the module keeps neighbour plates (Map page) and island names (Fly page) clear of
        the pictures drawn over the map viewport; its zone table (native/modules/01002DA013484000.cpp,
        plate-avoid block) must cover every such picture and the map widgets' rects."""
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text()
        block = cpp.split('// plate-avoid (begin)')[1].split('// plate-avoid (end)')[0]
        box = r'\{(\d+), (\d+), (\d+), (\d+)\}'
        compass = tuple(map(int, re.search(r'kMapCompass' + box, block).groups()))
        zoom = tuple(map(int, re.search(r'kMapReset' + box, block).groups()))  # dev.38: Reset where Zoom was
        title_w = {st: int(w) for st, w in re.findall(r'\{"(F\d+)", (\d+)\}', block)}
        fly_zones = [tuple(map(int, m)) for m in re.findall(box, block.split('kFlyAvoid')[1])]
        self.assertEqual(set(title_w), set(generator.PLACE_STAGES))

        def inside(r, zones):
            x, y, w, h = r
            return any(z[0] <= x and z[1] <= y and x + w <= z[2] and y + h <= z[3] for z in zones)

        def over(r, view):
            x, y, w, h = r
            return x < view[0] + view[2] and view[0] < x + w and y < view[1] + view[3] and view[1] < y + h

        manifest, _ = generator.build()
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        # Map page, per plate area: the widgets drawn after its map over the viewport (the stage's
        # own title plate and text, the compass, the Zoom plank); other stages' plates, dungeon
        # floor tabs and the world-overview pictures (never shown with a field map) are not
        mp = pages['mapfull']
        for st in generator.PLACE_STAGES:
            maps = [i for i, w in enumerate(mp) if w.get('type') == 'map' and w['area'] == st + '_m']
            self.assertEqual(len(maps), 1)  # dev.38: one pan / zoom view (the near / far pair before)
            view = mp[maps[0]]['rect']
            zones = [compass, zoom, (32, 26, 32 + title_w[st], 146)]
            for w in mp[max(maps) + 1:]:
                nb = w.get('need_bind', '')
                if (nb.startswith('stage.is_') and nb != 'stage.is_' + st) or nb.startswith(('floor.', 'sshd.ft', 'map.region_', 'map.anchor_', 'gen.')):
                    continue
                if w.get('type') in ('image', 'text', 'rect') and over(w['rect'], view):
                    self.assertTrue(inside(w['rect'], zones), (st, w.get('src', w.get('type')), w['rect']))
        # Fly page: everything drawn after the Sky map over its viewport
        fly = pages['fly']
        i = next(i for i, w in enumerate(fly) if w.get('type') == 'map')
        view = fly[i]['rect']
        for w in fly[i + 1:]:
            if w.get('type') in ('image', 'text', 'rect') and over(w['rect'], view):
                self.assertTrue(inside(w['rect'], fly_zones), (w.get('src', w.get('type')), w['rect']))

    def test_top_hud_hiding_settings_default_hidden_and_toggle(self):
        # owner 2026-11-11: top D-pad / rupees hidden on top by default, Bag > Options shows them again
        manifest, _ = generator.build()
        settings = next(p for p in manifest['pages'] if p['id'] == 'settings')['widgets']
        text = json.dumps(settings)
        self.assertNotIn('not available', text)
        derived = {v['name']: v for v in manifest['derived']}
        for act, flag, label in (('hud_dpad_toggle', 'hud.dpad', 'Top D-pad'), ('hud_rupee_toggle', 'hud.rupee', 'Top rupees')):
            self.assertEqual(manifest['flags'][flag], 0)  # 0 = Off = the native top copy hidden
            self.assertIn(flag, manifest['persist_flags'])
            self.assertEqual(manifest['actions'][act], {'kind': 'flag', 'flag': flag})
            row = next(w for w in settings if w.get('on_tap') == act)
            self.assertTrue(row['rect'][1] + row['rect'][3] <= 930)  # above the Back footer
            inside = lambda w: row['rect'][1] <= w['rect'][1] < row['rect'][1] + row['rect'][3]
            labels = [w for w in settings if w.get('src', '').endswith(':txt/plank/26/' + label)]
            self.assertEqual(len(labels), 1)
            self.assertTrue(inside(labels[0]))
            off = next(w for w in settings if w.get('need_bind') == flag + '.off')
            on = next(w for w in settings if w.get('need_bind') == flag + '.on')
            self.assertTrue(off['src'].endswith('/26/Off') and on['src'].endswith('/26/On') and inside(off) and inside(on))
            self.assertEqual((derived[flag + '.off']['cmp'], derived[flag + '.off']['a']), ('eq', '@flag:' + flag))
            self.assertEqual((derived[flag + '.on']['cmp'], derived[flag + '.on']['a']), ('ne', '@flag:' + flag))
        rows = sorted(w['rect'][1] for w in settings if w.get('on_tap') in ('dmap_toggle', 'hud_toggle', 'hud_dpad_toggle', 'hud_rupee_toggle'))
        self.assertTrue(all(b - a >= 120 for a, b in zip(rows, rows[1:])))  # rows do not overlap

    def test_dev30_dungeon_markers_floor_tabs_and_back(self):
        """dev.30: dungeon maps draw the contract's markers in native pane order, the Map page's
        floor tabs pick a floor directly (module floor_pick, one per tab slot, kFloorTabs = 5),
        Link's head leaves the Map page off his floor, the far zoom fits the dungeon, the Isle of
        Songs has a map, and Back returns to the gameplay page of the moment."""
        manifest, _ = generator.build()
        areas, acts = manifest['map']['areas'], manifest['actions']
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        for st in generator.DUNGEONS:
            for key in (st, st + '_m'):
                groups = [d['group'] for d in areas[key]['dynamic_markers']]
                # dev.34: Sky Keep adds its Triforce pieces on top
                self.assertEqual(groups, ['cone', 'conepf', 'dungeon_chests', 'marks', 'doors', 'statues', 'objective',
                                          'door_keys'] + (['triforce'] if st == 'D003' else []))
        tf = next(d for d in areas['D003']['dynamic_markers'] if d['group'] == 'triforce')
        self.assertEqual((tf['count'], tf['icon_src_bind'], tf['size']), (3, 'sshd.tf.src', round(26 * 84 / 25)))
        sizes = {d['group']: d for d in areas['D100']['dynamic_markers']}
        self.assertEqual(sizes['doors']['count'], 24)  # dev.33: Sky Keep's other tiles' bars too
        self.assertEqual(sizes['dungeon_chests']['size'], generator.OPEN_CHESTS['size'])  # same 100 px art
        for g, src in (('door_keys', 'sshd.dk.src'), ('marks', 'sshd.mx.src')):
            self.assertEqual(sizes[g]['icon_src_bind'], src)
        self.assertNotIn('D100', generator.OPEN_CHEST_STAGES)  # dungeon chests come from live objects
        # the floor tabs
        mp = pages['mapfull']
        tabs = [w for w in mp if str(w.get('on_tap', '')).startswith('floor_pick_')]
        self.assertEqual(len(tabs), generator.FLOOR_TABS)
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text()
        self.assertIn(f'constexpr int kFloorTabs = {generator.FLOOR_TABS};', cpp)
        for i, w in enumerate(tabs):
            self.assertEqual(acts[w['on_tap']], {'kind': 'module', 'action': 'floor_pick', 'argument': i})
            self.assertEqual(w['need_bind'], f'sshd.ft{i}.on')
            self.assertGreaterEqual(w['rect'][1], 146)  # under the title plate
            self.assertLessEqual(w['rect'][1] + w['rect'][3], generator.RESET_PLANK[1])  # above Reset
        self.assertTrue(any(w.get('src_bind') == f'sshd.ft{i}.src' for w in mp for i in range(5)))
        self.assertNotIn('map.floorsel', manifest['flags'])
        # Link's head on the Map page: the "_away" twin draws a blank marker
        derived = {d['name']: d for d in manifest['derived']}
        # dev.38: one view (big_D301) for both twins; the pinch zooms out to the whole dungeon
        here, away = [w for w in mp if w.get('id') == 'big_D301']
        self.assertEqual((here['area'], away['area']), ('D301_m', 'D301_m'))
        self.assertTrue(away['marker_src'].endswith(':blank') and here['marker_src'].endswith('tr_linkPIcon_00'))
        self.assertIn('sshd.lk.away == 0', derived[here['need_bind']]['expr'])
        self.assertIn('sshd.lk.away == 1', derived[away['need_bind']]['expr'])
        a = areas['D301_m']
        whole = max(a['max'][0] - a['min'][0], (a['max'][1] - a['min'][1]) * here['rect'][2] / here['rect'][3])
        self.assertGreaterEqual(here['follow_window'] / here['min_zoom'], whole)
        self.assertTrue(any(w.get('type') == 'map' and w['area'] == 'F010r' for w in pages['ready']))
        # Back: Ready on the ground, Flying in the Sky
        for pg in ('gear', 'gpouch', 'coll', 'settings', 'mapfull'):
            backs = {w['on_tap']: w.get('need_bind') for w in pages[pg] if w.get('on_tap') in ('go_ready', 'go_fly')}
            self.assertEqual(backs, {'go_ready': 'page.not_fly', 'go_fly': 'page.is_fly'})
        self.assertEqual(acts['go_fly']['page'], 'fly')

    def test_dev31_balloons_landmarks_and_beedle(self):
        """dev.31: NPC balloons on the Fly page and on field / Skyloft maps (Ready minimap and the
        Map page copies alike), landmark glyphs on field maps, Beedle's airshop on Skyloft and the
        Sky; each group's slots match the module (kBalloonSlots 4, kCaveSlots 2)."""
        manifest, _ = generator.build()
        areas = manifest['map']['areas']
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text()
        self.assertIn('kBalloonSlots = 4, kCaveSlots = 2, kKikwiSlots = 5, kTargetSlots = 16, kBugSlots = 16;', cpp)
        self.assertEqual({d['group']: d for d in areas['F100']['dynamic_markers']}['kikwi']['count'], 5)
        groups = lambda key: [d['group'] for d in areas[key]['dynamic_markers']]
        sky = {d['group']: d for d in areas['F020']['dynamic_markers']}
        self.assertEqual(sky['balloons']['count'], 4)
        self.assertEqual(sky['balloons']['icon_src_bind'], 'sshd.nb{i}.src')
        self.assertEqual(sky['balloons']['size'], round(80 * generator.SKY_UNIT))
        self.assertEqual(sky['beedle']['icon_src_bind'], 'sshd.bs{i}.src')
        g = groups('F020')
        self.assertLess(g.index('beedle'), g.index('balloons'))
        self.assertLess(g.index('balloons'), g.index('island_names'))
        for st in ('F000', 'F200', 'F302', 'F100', 'F300'):
            for key in (st, st + '_m') if st + '_m' in areas else (st,):
                gg = groups(key)
                self.assertEqual(gg[-1] if key == st else gg[-2], 'balloons', key)
                self.assertIn('caves', gg)
                self.assertEqual('beedle' in gg, st == 'F000')
        field = {d['group']: d for d in areas['F200']['dynamic_markers']}
        self.assertEqual(field['balloons']['size'], round(80 * generator.FIELD_UNIT))
        self.assertEqual(field['caves']['icon_src_bind'], 'sshd.cv.src')
        for st in generator.DUNGEONS:
            self.assertNotIn('balloons', groups(st))
        for st in ('S000', 'S100', 'S200', 'S300'):
            self.assertNotIn('balloons', groups(st))

    def test_dev31_sky_keep_tile_map(self):
        """dev.31: Sky Keep is one tile-map area for D003_0..8 (art skykeep via sshd.dmap.src), Link
        and his cone in the tile frame the module publishes (sshd.sk.lu / lv), the picture's box the
        art's (kSkyKeep*, dev.32: tile 7000, pitch 7000, 0.075 px per unit, +x right)."""
        manifest, _ = generator.build()
        areas = manifest['map']['areas']
        self.assertNotIn('D003_0', areas)
        a = areas['D003']
        w = int(generator.f32(21000.0) * generator.f32(0.075)) + 2 * generator.PAD  # = the art's picture width
        self.assertEqual(w, 1655)
        self.assertAlmostEqual(a['max'][0] - a['min'][0], w / 0.075, places=0)
        self.assertAlmostEqual(a['min'][0], -10500 - generator.PAD / 0.075, places=2)
        self.assertAlmostEqual(a['max'][1], 10500 + generator.PAD / 0.075, places=2)
        cone = a['dynamic_markers'][0]
        self.assertEqual((cone['x'], cone['y']), ('sshd.sk.lu', 'sshd.sk.lv'))
        derived = {d['name']: d for d in manifest['derived']}
        for i in range(9):
            self.assertIn(f'stage.id == {generator.stage_u64(f"D003_{i}")}', derived['stage.is_D003']['expr'])
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        maps = [w for pg in ('ready', 'mapfull') for w in pages[pg] if w.get('type') == 'map' and w['area'].startswith('D003')]
        self.assertTrue(maps)
        for m in maps:
            self.assertEqual((m['marker_x_bind'], m['marker_y_bind'], m['image_bind']), ('sshd.sk.lu', 'sshd.sk.lv', 'sshd.dmap.src'))
        art_h = (Path(__file__).resolve().parents[2] / 'native/modules/sshd_art.h').read_text()
        self.assertIn('kSkyKeepTile = 7000.0f, kSkyKeepPitch = 7000.0f, kSkyKeepPpu = 0.075f;', art_h)


    def test_field_maps_native_orientation_and_state_pictures(self):
        # dev.32: the overworld fields draw +x right / +z down like the native map (the art's
        # flip_h / flip_v), so Link and his cone use x and -z there; the Sealed Grounds keep -x / z.
        manifest, _ = generator.build()
        art = (Path(__file__).resolve().parents[2] / 'native/modules/sshd_art.cpp').read_text()
        for st in ('F000', 'F100', 'F200', 'F300', 'S000', 'S100', 'S200', 'S300'):
            self.assertIn(st, generator.NATIVE_XZ)
            self.assertEqual((generator.ubind(st), generator.vbind(st)), ('link.x', 'link.vf'))
            self.assertRegex(art, r'\{"%s", [^\n]*, (true|false), true, true\}' % st)
        self.assertEqual((generator.ubind('F401'), generator.vbind('F401')), ('link.u', 'link.v'))
        self.assertRegex(art, r'\{"F402", -20480.32f, 7684.18f, -7573.23f, 15558.50f, 0.07f, false\}')
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        maps = [w for pg in ('ready', 'mapfull') for w in pages[pg] if w.get('type') == 'map']
        binds = {(m['area'].removesuffix('_m'), m.get('image_bind')) for m in maps}
        for st in ('F300', 'F401'):
            self.assertIn((st, 'sshd.fmap.src'), binds)
        self.assertIn(('F000', None), binds)
        self.assertIn(('D100', 'sshd.dmap.src'), binds)
        # the area's own picture stays the fallback for an empty bound key
        self.assertEqual(manifest['map']['areas']['F300']['image'], generator.K + 'map/F300')

    def test_dev33_stamina_red_and_flying_dpad(self):
        manifest, _ = generator.build()
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        # the ring's segments switch to the exhausted red variant with the module's palette reader
        segs = [w for w in pages['ready'] if ':stamseg/' in w.get('src', '')]
        self.assertEqual(len(segs), generator.STAM_SEGMENTS)
        for w in segs:
            self.assertEqual(w['bind'], 'sshd.stam.red')
            self.assertTrue(w['src'].endswith('/0'))
            self.assertEqual(w['src_format'], w['src'][:-1] + '%d')
        # the flying page mirrors the same native D-pad layers (bird on Down in freefall), display only
        ready_dp = [w for w in pages['ready'] if ':dpad/' in w.get('src', '') or ':txt/hud/' in w.get('src', '')]
        fly = pages['fly']
        fly_dp = [w for w in fly if ':dpad/' in w.get('src', '') or ':txt/hud/' in w.get('src', '')]
        self.assertEqual(sorted(w['need_bind'] for w in fly_dp), sorted(w['need_bind'] for w in ready_dp))
        self.assertIn('sshd.dp.bird', {w['need_bind'] for w in fly_dp})
        for w in fly_dp:
            for k in ('on_tap', 'on_hold', 'drop_action', 'draggable', 'payload'):
                self.assertNotIn(k, w)
        x0, y0, x1, y1 = generator.FLY_DPAD_BOX
        for w in fly_dp:
            x, y, ww, hh = w['rect']
            self.assertTrue(x >= x0 and y >= y0 and x + ww <= x1 and y + hh <= y1, w)
        # inside the sky map, clear of the wings, the beacon tray, the compass and the footer taps
        sky = next(w for w in fly if w.get('id') == 'sky_map')
        sx, sy, sw, sh = sky['rect']
        self.assertTrue(x0 >= sx and y0 >= sy and x1 <= sx + sw and y1 <= sy + sh)
        for w in fly:
            if w in fly_dp or w is sky or 'rect' not in w or w['rect'][2] >= 1200:
                continue
            x, y, ww, hh = w['rect']
            self.assertTrue(x >= x1 or x + ww <= x0 or y >= y1 or y + hh <= y0, w)

    def test_dev33_targets_bugs_door_slots_and_new_areas(self):
        manifest, _ = generator.build()
        areas = manifest['map']['areas']
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text()
        self.assertIn('constexpr int kStatueSlots = 8, kDoorSlots = 24,', cpp)
        groups = lambda key: [d['group'] for d in areas[key]['dynamic_markers']]
        # Skyloft (Sky class): bugs under the door bars and statues, the red targets above them, Beedle on top
        f000 = groups('F000')
        self.assertLess(f000.index('bugs'), f000.index('doors'))
        self.assertLess(f000.index('statues'), f000.index('targets'))
        self.assertLess(f000.index('targets'), f000.index('beedle'))
        # the field class rejects the red targets; its bugs sit under the bars / statues
        for st in ('F100', 'F200', 'F300', 'F401', 'F301_4'):
            g = groups(st)
            self.assertNotIn('targets', g)
            self.assertLess(g.index('bugs'), g.index('doors'))
        for st in generator.DUNGEONS:
            self.assertNotIn('bugs', groups(st))
        self.assertNotIn('bugs', groups('F020'))
        tg = {d['group']: d for d in areas['F000']['dynamic_markers']}
        self.assertEqual((tg['targets']['count'], tg['bugs']['count']), (16, 16))
        self.assertEqual(tg['targets']['icon_src_bind'], 'sshd.tg{i}.src')
        self.assertEqual(tg['bugs']['icon_src_bind'], 'sshd.bg.src')
        # the Shipyard and the sailing Sand Sea have their own pictures (native orientation), the Sand
        # Sea's sea-chart state through the module's field picture bind
        for st in ('F301_4', 'F301_1'):
            self.assertIn(st, areas)
            self.assertIn(st, generator.NATIVE_XZ)
            row = next(a for a in generator.AREAS if a[0] == st)
            w = int((row[2] - row[1]) * row[5]) + 2 * generator.PAD
            h = int((row[4] - row[3]) * row[5]) + 2 * generator.PAD
            self.assertLessEqual(w * h * 4, 16 * 1024 * 1024)
            self.assertIn(row[6], generator.TITLE_W)
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        mini = {w['area']: w for w in ready if w.get('type') == 'map'}
        self.assertEqual(mini['F301_1'].get('image_bind'), 'sshd.fmap.src')
        self.assertIn('{"F301_4", -38500.0f, 19800.0f, -17800.0f, 21200.0f, 0.04f, false, true, true}',
                      (Path(__file__).resolve().parents[2] / 'native/modules/sshd_art.cpp').read_text())


class Dev34Test(unittest.TestCase):
    """dev.34: native map names (item 7) and the generic sub-stage frame (item 2)."""

    # 0-Common/007-MapText.msbt (en_US), MAP_<id>_S: the one-line names (dumped 2026-11-12)
    MSBT_S = {0: 'Skyloft', 1: 'Sealed Grounds', 2: 'Faron Woods', 5: 'Eldin Volcano', 8: 'Lanayru Desert',
              9: 'Lanayru Gorge', 10: 'Lanayru Sand Sea', 11: 'Skyview Temple', 12: 'Earth Temple',
              13: 'Lanayru Mining Facility', 14: 'Ancient Cistern', 15: 'Sandship', 16: 'Fire Sanctuary',
              17: 'Sky Keep', 18: "Farore's Silent Realm", 19: "Nayru's Silent Realm", 20: "Din's Silent Realm",
              21: "The Goddess's Silent Realm", 30: 'Isle of Songs', 40: 'Lanayru Caves', 44: 'Shipyard'}

    def test_every_area_title_is_the_native_map_name(self):
        for st, *_r, title in generator.AREAS:
            self.assertEqual(title, self.MSBT_S[generator.MAP_NAME_ID[st]], st)
        manifest, _ = generator.build()
        mp = next(p for p in manifest['pages'] if p['id'] == 'mapfull')['widgets']
        for st, *_r, title in generator.AREAS:  # the Map page plate: MAP_<id> with its line breaks
            self.assertTrue(any(w.get('src', '').endswith(f':ctitle/plate/34/{generator.TITLE_W[title]}/67/'
                                                           f'MAP_{generator.MAP_NAME_ID[st]:02d}')
                                and w.get('need_bind') == 'stage.is_' + st for w in mp), st)

    def test_silent_realms_follow_the_native_trial_maps(self):
        # live dev.34: S100 / S200 a small blob (TRIAL_UPX), S000 the whole realm, S300 no realm
        manifest, _ = generator.build()
        areas = manifest['map']['areas']
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        self.assertEqual(areas['S300']['image'], generator.K + 'blank')
        self.assertEqual(areas['S100']['image'], generator.K + 'map/S100')
        for st in generator.REALMS:
            self.assertEqual([d['group'] for d in areas[st]['dynamic_markers']], ['cone', 'conepf'])
            self.assertEqual(generator.REGION_COLOR[st], 6)  # the purple trial plate
            mini = next(w for w in pages['ready'] if w.get('id') == 'mini_' + st)
            big = {w['follow_window'] for w in pages['mapfull'] if w.get('type') == 'map' and w['area'] == st}
            self.assertEqual(len(big), 1)  # no zoom in a trial: near = far
            if st == generator.REALM_WHOLE:
                self.assertEqual(mini['follow_window'], 2 * generator.S000_SPAN)
                self.assertEqual(big, {2 * generator.S000_SPAN})
            else:
                self.assertEqual(mini['follow_window'], generator.TRIAL_UPX * 2 * generator.MR)

    def test_generic_frame_matches_the_module_and_art(self):
        manifest, _ = generator.build()
        gen = manifest['map']['areas']['GEN']
        self.assertEqual(gen['min'], [-generator.GEN_EXT, -generator.GEN_EXT])
        self.assertEqual(gen['max'], [generator.GEN_EXT, generator.GEN_EXT])
        # the picture: kGenPx = 2 kGenHalf * 0.0625 + 2 kMapPad
        self.assertEqual(int(2 * generator.GEN_HALF * generator.GEN_PPU) + 2 * generator.PAD, 1600)
        root = Path(__file__).resolve().parents[2] / 'native/modules'
        hdr = (root / 'sshd_art.h').read_text()
        self.assertIn('kGenHalf = %.1ff' % generator.GEN_HALF, hdr)
        self.assertIn('kGenPx = 1600', hdr)
        cpp = (root / '01002DA013484000.cpp').read_text()
        self.assertIn('kGenTitleW = %d, kGenPlateW = %d, kGenPlateTextW = %d' %
                      (generator.GEN_TITLE_W, generator.GEN_PLATE_W, generator.GEN_PLATE_TEXT_W), cpp)
        groups = [d['group'] for d in gen['dynamic_markers']]
        self.assertEqual(groups, ['cone', 'conepf', 'doors', 'statues'])
        derived = {d['name']: d['expr'] for d in manifest['derived'] if 'expr' in d}
        self.assertEqual(derived['stage.blank'], 'stage.unknown && !sshd.gen.on')
        self.assertEqual(derived['stage.show_GEN'], 'stage.unknown && sshd.gen.on')
        ready = next(p for p in manifest['pages'] if p['id'] == 'ready')['widgets']
        self.assertTrue(any(w.get('id') == 'mini_GEN' and w['image_bind'] == 'sshd.gen.src' for w in ready))
        # every surveyed stage is either a dedicated area / alias / the Sky or a generic row (no blank disc)
        rows = re.findall(r'\{"(\w+)", ', (root / 'sshd_stages.inc').read_text())
        dedicated = {st for st, *_ in generator.AREAS} | set(sum(generator.ALIASES.values(), [])) | {'F020', 'F023'}
        survey = {'B100', 'B100_1', 'B101', 'B101_1', 'B200', 'B201', 'B201_1', 'B210', 'B300', 'B301', 'B400',
                  'D000', 'Demo', 'F021', 'F023', 'F405'} | {f'F{n:03d}r' for n in (1, 2, 4, 5, 6, 7, 8, 9, 11, 12, 13,
                                                                                   14, 15, 16, 17, 18, 19)}
        self.assertEqual(survey - set(rows) - dedicated, set())
        self.assertEqual(set(rows) & dedicated, set())

    def test_generic_stage_floor_tabs(self):
        # dev.35: the Map page draws the generic frame as its own area GEN_m (the module's floor-tab
        # pick needs the page's view), with Link's head only on his own floor (the "_away" copies
        # take a blank marker), and the native floor tabs are the dungeons' ft0..ft4 widgets
        manifest, _ = generator.build()
        areas = manifest['map']['areas']
        self.assertEqual(areas['GEN_m'], areas['GEN'])
        mp = next(p for p in manifest['pages'] if p['id'] == 'mapfull')['widgets']
        # dev.38: one pan / zoom view (big_GEN) for both twins (the near / far pairs before)
        gen = {w['need_bind']: w for w in mp if w.get('type') == 'map' and w['area'].startswith('GEN')}
        self.assertEqual(set(gen), {'map.show_GEN', 'map.show_GEN_away'})
        self.assertEqual({w['id'] for w in gen.values()}, {'big_GEN'})
        self.assertTrue(all(w['area'] == 'GEN_m' and w['image_bind'] == 'sshd.gen.src' for w in gen.values()))
        self.assertTrue(gen['map.show_GEN_away']['marker_src'].endswith('blank'))
        self.assertTrue(gen['map.show_GEN']['marker_src'].endswith('tr_linkPIcon_00'))
        derived = {d['name']: d['expr'] for d in manifest['derived'] if 'expr' in d}
        self.assertEqual(derived['map.show_GEN_away'], 'stage.show_GEN && sshd.lk.away == 1')
        self.assertEqual(derived['map.show_GEN'], 'stage.show_GEN && sshd.lk.away == 0')
        tabs = [w for w in mp if w.get('on_tap', '').startswith('floor_pick_')]
        self.assertEqual(len(tabs), generator.FLOOR_TABS)
        # the module: the generic stage's tabs come from the same publisher, its pick from GEN_m
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text()
        self.assertIn('ApplyFloorPick(inst, host, c, st, "GEN_m")', cpp)
        self.assertIn('const bool ok = (c.dungeon && c.flags_ok) || c.gen;', cpp)


class Dev38Test(unittest.TestCase):
    """dev.38 (owner 2026-11-12): the Map page and the Fly page maps pan with a drag and zoom with a
    pinch (the runtime's own pan_zoom on the map widget), clamped to the picture, with a native
    Reset plank that glides the view home; the old Zoom toggle and the region paintings are gone."""

    @classmethod
    def setUpClass(cls):
        cls.manifest, _ = generator.build()
        cls.pages = {p['id']: p['widgets'] for p in cls.manifest['pages']}
        cls.areas = cls.manifest['map']['areas']
        cls.acts = cls.manifest['actions']
        cls.derived = {d['name']: d.get('expr') for d in cls.manifest['derived']}

    def whole(self, w):
        a = self.areas[w['area']]
        return max(a['max'][0] - a['min'][0], (a['max'][1] - a['min'][1]) * w['rect'][2] / w['rect'][3])

    def test_map_page_views_pan_zoom_from_the_near_view(self):
        mp = self.pages['mapfull']
        maps = [w for w in mp if w.get('type') == 'map']
        fixed = {'big_S100', 'big_S200', 'big_S300'}  # the native trial maps do not zoom
        by_id = {}
        for w in maps:
            by_id.setdefault(w['id'], []).append(w)
            self.assertEqual(w['rect'], list(generator.MAP_RECT))
            self.assertTrue(self.areas[w['area']]['clamp_view'])  # a moved view stays on its picture
            if w['id'] in fixed:
                self.assertNotIn('pan_zoom', w)
                continue
            self.assertTrue(w['pan_zoom'])
            self.assertNotIn('view_idle_ms', w)  # no glide back by itself: only Reset does that
            # home = the old near view: the follow window, centred on Link
            st = w['id'][4:]
            near = {'GEN': generator.GEN_NEAR_WINDOW, 'F023': generator.THUNDERHEAD_NEAR_WINDOW,
                    'S000': 2 * generator.S000_SPAN}.get(st, generator.MAP_NEAR_WINDOW)
            self.assertEqual(w['follow_window'], near)
            self.assertTrue(w['marker_x_bind'] and w['marker_y_bind'])
            # out: at least the whole picture (the old far view showed span * 1.05 across); in: closer
            # than the old near view
            self.assertLessEqual(w['min_zoom'], 1.0)
            self.assertGreaterEqual(w['follow_window'] / w['min_zoom'], min(self.whole(w), w['follow_window']) - 1e-6)
            self.assertGreaterEqual(w['max_zoom'], generator.MAP_ZOOM_IN_MIN)
            self.assertLessEqual(w['max_zoom'], generator.MAP_ZOOM_IN_CAP)
        # one view per area: the "_away" twins share the id, the area and the limits
        for vid, ws in by_id.items():
            keys = {json.dumps({k: w.get(k) for k in ('area', 'rect', 'follow_window', 'pan_zoom', 'min_zoom', 'max_zoom')})
                    for w in ws}
            self.assertEqual(len(keys), 1, vid)
            self.assertLessEqual(len(ws), 2)
        self.assertEqual({vid for vid, ws in by_id.items() if len(ws) == 2},
                         {'big_' + st for st in generator.DUNGEONS} | {'big_GEN'})
        self.assertEqual(set(by_id), {'big_' + st for st, *_ in generator.AREAS} | {'big_GEN', 'big_F023'})
        # the old near / far toggle, the region paintings and their anchors are gone
        self.assertNotIn('map.far', self.manifest['flags'])
        self.assertFalse(any(k in self.acts for k in ('map_zoom', 'map_recenter')))
        text = json.dumps(mp)
        for gone in ('map.near', 'map.far', 'map.region_', 'map.anchor_', 'worldMap', '/26/Zoom'):
            self.assertNotIn(gone, text)
        self.assertFalse(any('map.near' in (e or '') or 'map.far' in (e or '') for e in self.derived.values()))

    def test_reset_plank_glides_the_shown_view_home(self):
        mp = self.pages['mapfull']
        x, y = generator.RESET_PLANK
        maps = {w['id']: w for w in mp if w.get('type') == 'map'}
        last_map = max(i for i, w in enumerate(mp) if w.get('type') == 'map')
        taps = [(i, w) for i, w in enumerate(mp) if str(w.get('on_tap', '')).startswith('map_reset_')]
        pannable = {vid for vid, w in maps.items() if w.get('pan_zoom')}
        self.assertEqual({self.acts[w['on_tap']]['view'] for _, w in taps}, pannable)
        for i, w in taps:
            self.assertGreater(i, last_map)  # drawn (and hit) above the map
            act = self.acts[w['on_tap']]
            self.assertEqual(act['kind'], 'view_reset')
            self.assertEqual(w['rect'], [x, y, generator.RESET_W, generator.RESET_H])
            # shown with the view it resets: the same area's stage bind as its map widget(s)
            m = maps[act['view']]
            area_bind = 'stage.is_F023' if act['view'] == 'big_F023' else 'stage.show_' + act['view'][4:]
            self.assertEqual(w['need_bind'], area_bind)
            self.assertTrue(m['need_bind'] == area_bind or area_bind in self.derived[m['need_bind']])
            # inside the map rect: a drag that starts on the plank pans the map like anywhere else
            mx, my, mw, mh = m['rect']
            self.assertTrue(mx <= x and x + generator.RESET_W <= mx + mw and my <= y and y + generator.RESET_H <= my + mh)
        # dev.46 (alt A): the opaque choice plank inside the tap rect, its pressed look on press id 1
        plank = [w for w in mp if w.get('src', '').endswith(f':choice/{generator.RESET_W}/{generator.RESET_ART_H}/0/1')]
        label = [w for w in mp if w.get('src', '').endswith(':txt/choice/26/Reset')]
        self.assertEqual(len(plank), 1)
        self.assertEqual(len(label), 1)
        self.assertEqual(plank[0]['need_bind'], 'map.pannable')
        self.assertEqual(label[0]['need_bind'], 'map.pannable')
        self.assertEqual(set(self.derived['map.pannable'].split(' || ')), {w['need_bind'] for _, w in taps})
        self.assertTrue(all(w['select_group'] == 'sshd.press' and w['payload'] == generator.PRESS['map_reset'] for _, w in taps))
        # the Reset label is drawn 1:1 at its measured width, centred on the plank
        self.assertEqual(label[0]['rect'][2], generator.TW[('plank', 26, 'Reset')])
        self.assertLessEqual(abs(label[0]['rect'][0] + label[0]['rect'][2] / 2 + generator.INK_DX - (x + generator.RESET_W / 2)), 1)
        # the Back footer is outside every map rect: a drag there never pans, a tap always goes back
        backs = [w for w in mp if w.get('on_tap') in ('go_ready', 'go_fly')]
        for b in backs:
            self.assertGreaterEqual(b['rect'][1], generator.MAP_RECT[1] + generator.MAP_RECT[3])

    def test_fly_page_sky_pans_and_resets(self):
        fly = self.pages['fly']
        (sky,) = [w for w in fly if w.get('type') == 'map']
        self.assertEqual(sky['id'], 'sky_map')
        self.assertTrue(sky['pan_zoom'] and self.areas['F020']['clamp_view'])
        self.assertEqual(sky['follow_window'], generator.SKY_FOLLOW)  # home: the whole Sky with the bird
        self.assertEqual(sky['min_zoom'], 1.0)
        self.assertGreaterEqual(sky['max_zoom'], generator.MAP_ZOOM_IN_MIN)
        self.assertEqual(self.acts['sky_reset'], {'kind': 'view_reset', 'view': 'sky_map'})
        (tap,) = [w for w in fly if w.get('on_tap') == 'sky_reset']
        fx, fy = generator.FLY_RESET
        self.assertEqual(tap['rect'], [fx, fy, generator.RESET_W, generator.RESET_H])
        self.assertGreater(fly.index(tap), fly.index(sky))
        self.assertTrue(any(w.get('src', '').endswith(':txt/choice/26/Reset') for w in fly))  # dev.46 alt A
        self.assertEqual((tap['select_group'], tap['payload']), ('sshd.press', generator.PRESS['fly_reset']))

    def test_map_drags_never_reach_the_sword_stick(self):
        # the right-stick pad is the Ready page's only: the module reads @view_pan_x/y:stickpad_<i>,
        # and no Map / Fly page pan_zoom widget carries a stickpad id or sits on the Ready page
        for pg, ws in self.pages.items():
            pads = [w for w in ws if str(w.get('id', '')).startswith('stickpad_')]
            self.assertEqual(bool(pads), pg == 'ready', pg)
            for w in ws:
                if w.get('pan_zoom') and pg != 'ready':
                    self.assertTrue(w['id'].startswith('big_') or w['id'] == 'sky_map', w['id'])
        ready_pz = {w['id'] for w in self.pages['ready'] if w.get('pan_zoom')}
        self.assertEqual(ready_pz, {'stickpad_0', 'stickpad_1'})
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text()
        self.assertEqual(set(re.findall(r'"@view_pan_[xy]:(\w+)"', cpp)), {'stickpad_0', 'stickpad_1'})
        # and a pan / pinch / Reset is no game input: Reset is a view_reset, never a button
        for k, a in self.acts.items():
            if k.startswith(('map_reset_', 'sky_reset')):
                self.assertEqual(a['kind'], 'view_reset')

    def test_swipe_pad_drag_trail_and_refusal(self):
        # dev.38 (owner Thor feedback 2026-11-12): the pad stays the pan pad (the module tells the
        # finger from the glide, sshd_stroke.h PanStroke), only with the stick helper, released at
        # once (view_idle_ms 1), never zooming; the gold trail; a refused swipe says "Not now"
        ready = self.pages['ready']
        pads = [w for w in ready if str(w.get('id', '')).startswith('stickpad_')]
        self.assertEqual(len(pads), 2)
        for w in pads:
            self.assertEqual((w['pan_zoom'], w['min_zoom'], w['max_zoom'], w['view_idle_ms']), (True, 1, 1, 1))
            self.assertEqual(w['need_bind'], 'sshd.stick.ready')
            self.assertEqual(w['color'], '#00000000')
            self.assertFalse(any(k in w for k in ('on_tap', 'drop_action', 'draggable', 'select_group')))
        # the trail: one native sparkle per dot, gold at 16 alpha levels, positioned / sized / faded
        # by the module from the minimap centre (its anchor, module kStrokeAnchor), never taking input
        self.assertIn('kStrokeAnchor[2] = {%d, %d}' % (generator.MX, generator.MY),
                      (Path(__file__).resolve().parents[2] / 'native/modules/01002DA013484000.cpp').read_text())
        dots = [w for w in ready if str(w.get('x_bind', '')).startswith('sshd.tr')]
        self.assertEqual(len(dots), generator.TRAIL_DOTS)
        cpp = (Path(__file__).resolve().parents[2] / 'native/modules/sshd_stroke.h').read_text()
        self.assertIn(f'kTrailPoints = {generator.TRAIL_DOTS};', cpp)
        for k, w in enumerate(dots):
            self.assertTrue(w['src'].endswith(':sq/DoButton/tr_kirakira_00'))
            self.assertEqual((w['x_bind'], w['y_bind'], w['scale_bind'], w['tint_bind']),
                             (f'sshd.tr{k}.x', f'sshd.tr{k}.y', f'sshd.tr{k}.s', f'sshd.tr{k}.a'))
            self.assertEqual(len(w['tint_colors']), 16)
            self.assertTrue(w['tint_colors'][0].startswith('#00') and w['tint_colors'][15].upper() == '#FFFFE787')
            self.assertEqual(w['need_bind'], f'tr{k}.on')
            self.assertEqual(self.derived[f'tr{k}.on'], f'sshd.tr.on && sshd.tr{k}.a > 0')
            self.assertEqual(w['rect'][0] + w['rect'][2] // 2, 0)  # centred on the finger
            self.assertFalse(any(key in w for key in ('on_tap', 'draggable', 'on_hold')))
        # drawn after the pads (over the minimap and the left pane)
        self.assertGreater(ready.index(dots[0]), max(ready.index(p) for p in pads))
        refused = [w for w in ready if w.get('need_bind') == 'sshd.swipe.refused']
        # dev.46: the word on the refusal pill (alt A's one refusal component)
        self.assertEqual(len(refused), 2)
        self.assertTrue(refused[0]['src'].endswith(':dopill/%d/%d' % (generator.PILL_W, generator.PILL_H)))
        self.assertTrue(refused[1]['src'].endswith(':txt/doinfo/%d/Not now' % generator.PILL_CAP))
        x, y, w_, h = refused[0]['rect']
        self.assertTrue(generator.MX - generator.MR < x and x + w_ < generator.MX + generator.MR)
        # a Map / Fly page drag: no pan pad, no trail there
        for k, a in self.acts.items():
            if k.startswith(('map_reset_', 'sky_reset')):
                self.assertEqual(a['kind'], 'view_reset')



class Dev39Test(unittest.TestCase):
    """dev.39: on runtime 25 the gold trail is drawn under the finger (@view_touch_x/y:stickpad_<i>, read
    by the module only); on runtime 23..24 dev.38's trail from the minimap centre stays. The package
    needs min_runtime 23 (swipe-to-slash verified on 23+); the pads, the trail widgets and the stick input
    are unchanged, and the optional finger keys stay out of the manifest."""

    @classmethod
    def setUpClass(cls):
        cls.manifest, _ = generator.build()
        cls.ready = next(p['widgets'] for p in cls.manifest['pages'] if p['id'] == 'ready')
        root = Path(__file__).resolve().parents[2] / 'native/modules'
        cls.cpp = (root / '01002DA013484000.cpp').read_text()
        cls.stroke = (root / 'sshd_stroke.h').read_text()

    def test_module_pad_rects_match_the_manifest(self):
        # the finger trail is kept inside the pads the module knows: the same rects and canvas
        pads = {w['id']: w['rect'] for w in self.ready if str(w.get('id', '')).startswith('stickpad_')}
        m = re.search(r'kPadRects\[2\] = \{\{([\d, ]+)\}, \{([\d, ]+)\}\};', self.cpp)
        self.assertIsNotNone(m)
        self.assertEqual([int(v) for v in m[1].split(',')], pads['stickpad_0'])
        self.assertEqual([int(v) for v in m[2].split(',')], pads['stickpad_1'])
        c = re.search(r'kCanvas = \{0, 0, (\d+), (\d+)\};', self.cpp)
        self.assertEqual((int(c[1]), int(c[2])), (self.manifest['canvas_w'], self.manifest['canvas_h']))
        # and the sparkle's size: half the drawn box at scale 1000
        self.assertIn(f'kDotHalf = {generator.TRAIL_PX // 2};', self.stroke)

    def test_finger_keys_are_read_by_the_module_only_with_a_fallback(self):
        # both pads, through get_f64 with a NaN fallback (an older runtime answers the fallback)
        self.assertEqual(set(re.findall(r'"@view_touch_[xy]:(\w+)"', self.cpp)), {'stickpad_0', 'stickpad_1'})
        self.assertRegex(self.cpp, r'get_f64\(host->userdata, kTouch\[i\]\[0\], NAN\)')
        # the manifest never names them: pages on runtime 23..24 load exactly as before
        self.assertNotIn('@view_touch', json.dumps(self.manifest))
        self.assertEqual(self.manifest['min_runtime'], 23)
        # the trail is chosen per stroke by whether finger positions came, the dev.38 one kept as the fallback
        self.assertIn('inst->trail = under ? inst->ftrail.Trail() : inst->stroke.Trail();', self.cpp)

    def test_trail_widgets_unchanged(self):
        dots = [w for w in self.ready if str(w.get('x_bind', '')).startswith('sshd.tr')]
        self.assertEqual(len(dots), generator.TRAIL_DOTS)
        for w in dots:
            self.assertEqual(w['rect'], [-generator.TRAIL_PX // 2, -generator.TRAIL_PX // 2, generator.TRAIL_PX,
                                         generator.TRAIL_PX])
            self.assertFalse(any(k in w for k in ('on_tap', 'draggable', 'on_hold', 'pan_zoom')))


class Dev40Test(unittest.TestCase):
    """dev.40 (owner Thor report: Gear / Bag equips said "Not now"): every refusal logs and publishes its
    reason; a spin or shield bash refused for an item in Link's hands says "Put away" instead of "Not
    now", in the same place; the Gear / Bag drops and moves keep their gates in the module."""

    @classmethod
    def setUpClass(cls):
        cls.manifest, _ = generator.build()
        cls.pages = {p['id']: p['widgets'] for p in cls.manifest['pages']}
        root = Path(__file__).resolve().parents[2] / 'native'
        cls.cpp = (root / 'modules/01002DA013484000.cpp').read_text()
        cls.helper = (root / 'helper/sshd_stick_helper.S').read_text()

    def refusal_texts(self, page, prefix):
        return [w for w in self.pages[page] if ':txt/' in w.get('src', '') and str(w.get('need_bind', '')).startswith(prefix)]

    def test_put_away_shares_the_not_now_place(self):
        for prefix, n in (('sshd.spin.', 4), ('sshd.bash.', 2)):
            ws = self.refusal_texts('ready', prefix)
            self.assertEqual(len(ws), n)
            by_place = {}
            for w in ws:
                word = w['src'].rsplit('/', 1)[1]
                self.assertEqual(w['need_bind'], prefix + ('putaway' if word == 'Put away' else 'notnow'))
                cap = generator.PILL_SHIELD_CAP if prefix == 'sshd.bash.' else generator.PILL_CAP  # dev.48
                self.assertEqual(w['rect'][2], generator.TW[('plank', cap, word)])  # drawn 1:1
                cx = w['rect'][0] + w['rect'][2] / 2
                by_place.setdefault((round(cx / 4), w['rect'][1]), set()).add(word)  # dev.46: cap-centred, +-1 px
                self.assertGreaterEqual(w['rect'][0], 0)
                self.assertLessEqual(w['rect'][0] + w['rect'][2], self.manifest['canvas_w'])
            self.assertTrue(all(v == {'Not now', 'Put away'} for v in by_place.values()))
            # the module publishes exactly one of the two per refusal
            self.assertIn(f'"{prefix}putaway"', self.cpp)
            self.assertIn(f'"{prefix}notnow"', self.cpp)

    def test_gear_and_bag_drops_reach_the_module(self):
        acts = self.manifest['actions']
        self.assertEqual(acts['bitem_equip'], {'kind': 'module', 'action': 'bitem_equip', 'argument': '$payload'})
        self.assertEqual(acts['pouch_equip'], {'kind': 'module', 'action': 'pouch_equip', 'argument': '$payload'})
        self.assertNotIn('enabled_bind', acts['bitem_equip'])  # the module decides (queue / reason)
        self.assertNotIn('enabled_bind', acts['pouch_equip'])
        for page in ('gear', 'gpouch'):
            words = [w['src'].rsplit('/', 1)[1] for w in self.refusal_texts(page, 'sshd.pouch.refused')]
            self.assertEqual(words, ['Not now'])

    def test_reasons_are_logged_and_published(self):
        for key in ('sshd.why.pouch', 'sshd.why.bitem', 'sshd.why.spin', 'sshd.refuse.why', 'sshd.refuse.n',
                    'sshd.menu.open', 'sshd.pouch.queued', 'sshd.spin.queued'):
            self.assertIn(f'"{key}"', self.cpp)
        # dev.43: the line is GateLine's, Refuse passes "refused" ("sshd: refused <act> (<why>...): held ...")
        # (dev.44: the menu field names the layout controller's state and its source instead of a raw word)
        self.assertIn('"sshd: %s %s (%s%s): held 0x%x bsel %d act 0x%llx wheel %d menu %s %s (run %d, open %d, unread %d)', self.cpp)
        self.assertIn('GateLine(inst, "refused", act, why, note);', self.cpp)
        # the helper's ZR equip takes the sword drawn and refuses the native wheel's attack states
        self.assertRegex(self.helper, r'cmp\s+w9, #SSHD_HELD_SWORD\n\s+b\.ne\s+8f')
        self.assertRegex(self.helper, r'cmp\s+w9, #0x68\n\s+b\.eq\s+8f\n\s+cmp\s+w9, #0x9a')
        # the put-away word: the B bit (0x400) while +0x1C counts frames, counted down per frame
        self.assertRegex(self.helper, r'ldr\s+w10, \[x9, #0x1C\][^\n]*\n\s+cbz\s+w10, 6f\n\s+ldr\s+w10, \[x20\]\n'
                                      r'\s+orr\s+w10, w10, #0x400')
        self.assertIn('kOffPutAway = 0x1C', self.cpp)


class Dev41Test(unittest.TestCase):
    """dev.41 (legibility minimums): primary controls are at
    least 112 x 112, dense tabs at least 88 on the short side with 12 px between them; the helper's
    ZR equip re-checks the native wheel on the game thread; unbound published keys are either gone
    or marked diagnostics only."""

    @classmethod
    def setUpClass(cls):
        cls.manifest, _ = generator.build()
        cls.pages = {p['id']: p['widgets'] for p in cls.manifest['pages']}
        root = Path(__file__).resolve().parents[2]
        cls.cpp = (root / 'native/modules/01002DA013484000.cpp').read_text()
        cls.helper = (root / 'native/helper/sshd_stick_helper.S').read_text()

    def taps(self, page, pred):
        return [w for w in self.pages[page] if w.get('on_tap') and pred(str(w['on_tap']))]

    def test_bag_tabs_are_112_tall(self):
        for page in ('gpouch', 'coll', 'settings'):
            tabs = self.taps(page, lambda a: a in ('go_gpouch', 'go_coll', 'go_set'))
            self.assertEqual(len(tabs), 3)
            for w in tabs:
                self.assertGreaterEqual(w['rect'][2], 112)
                self.assertGreaterEqual(w['rect'][3], 112)
            # no other tap target starts inside the tab row
            row_end = max(w['rect'][1] + w['rect'][3] for w in tabs)
            others = [w for w in self.pages[page] if w.get('on_tap') and w not in tabs]
            for w in others:
                x, y, ww, hh = w['rect']
                self.assertFalse(y < row_end and x < 16 + 3 * generator.TAB_W + 24 and y + hh > 16, (page, w['on_tap'], w['rect']))

    def test_reset_planks_are_112_tall_and_fit_their_art(self):
        for page, act in (('mapfull', 'map_reset_'), ('fly', 'sky_reset')):
            taps = self.taps(page, lambda a: a.startswith(act))
            self.assertTrue(taps)
            for w in taps:
                self.assertGreaterEqual(w['rect'][2], 112)
                self.assertGreaterEqual(w['rect'][3], 112)
            # dev.46 (alt A): the choice plank's visible body lies inside every tap rect (FIT C11)
            planks = [w for w in self.pages[page] if w.get('src', '').endswith(f':choice/{generator.RESET_W}/{generator.RESET_ART_H}/0/1')]
            self.assertEqual(len(planks), 1)
            px, py, pw, ph = planks[0]['rect']
            v0, v1 = generator.CHOICE_VIS[ph]
            for w in taps:
                x, y, ww, hh = w['rect']
                self.assertTrue(x <= px and y <= py + v0 and px + pw <= x + ww and py + v1 <= y + hh)
        # the module's label-fit zones follow the planks (plate-avoid block)
        x, y = generator.RESET_PLANK
        self.assertIn(f'kMapReset{{{x}, {y}, {x + generator.RESET_W + 16}, {y + generator.RESET_H + 16}}}', self.cpp)
        fx, fy = generator.FLY_RESET
        self.assertIn(f'{{{fx}, {fy}, {fx + generator.RESET_W + 16}, {fy + generator.RESET_H + 16}}}}};', self.cpp)

    def test_floor_tabs_spacing(self):
        tabs = sorted(self.taps('mapfull', lambda a: a.startswith('floor_pick_')), key=lambda w: w['rect'][1])
        self.assertEqual(len(tabs), generator.FLOOR_TABS)
        for w in tabs:
            self.assertGreaterEqual(min(w['rect'][2], w['rect'][3]), 112)
        for a, b in zip(tabs, tabs[1:]):
            self.assertGreaterEqual(b['rect'][1] - (a['rect'][1] + a['rect'][3]), 12)
        # Link's head stays centred on its tab's art
        heads = [w for w in self.pages['mapfull'] if w.get('src', '').endswith(':lyt/Map2D/tr_linkPIcon_00')
                 and str(w.get('need_bind', '')).startswith('sshd.ft')]
        arts = [w for w in self.pages['mapfull'] if str(w.get('src_bind', '')).startswith('sshd.ft')]
        for head, art in zip(heads, arts):
            self.assertEqual(head['rect'][1] + head['rect'][3] // 2, art['rect'][1] + art['rect'][3] // 2)

    def test_helper_equip_rechecks_the_native_wheel(self):
        self.assertRegex(self.helper, r'mov\s+w21, #10\n\s+mov\s+w9, #0x63B5\n\s+ldrb\s+w9, \[x25, x9\][^\n]*\n'
                                      r'\s+cbnz\s+w9, 8f\n\s+mov\s+w21, #6')

    def test_unbound_keys_dropped_or_diagnostics_only(self):
        text = json.dumps(self.manifest)
        for gone in ('sshd.dp.slot', 'sshd.dp.logical', 'sshd.dp.utext', 'sshd.dp.htext'):
            self.assertNotIn(f'"{gone}"', self.cpp)
        for key in ('sshd.l.blk', 'sshd.ply.act', 'sshd.live.n', 'sshd.story.why'):
            self.assertNotIn(f'"{key}"', text)  # never bound by the pages
            line = next(l for l in self.cpp.splitlines() if f'"{key}"' in l)
            self.assertIn('diagnostics only', line)



class Dev42Test(unittest.TestCase):
    """dev.42: the wrong-build notice lives on the first page (the one another
    build stays on); Item Check cells meet the dense-grid minimum; the no-helper swipe fallback is
    gated by the module's refusals; the footer contrast variant is prepared but off."""

    @classmethod
    def setUpClass(cls):
        cls.manifest, cls.data = generator.build()
        cls.pages = {p['id']: p['widgets'] for p in cls.manifest['pages']}
        root = Path(__file__).resolve().parents[2]
        cls.cpp = (root / 'native/modules/01002DA013484000.cpp').read_text()
        cls.art = (root / 'native/modules/sshd_art.cpp').read_text()

    def test_first_page_carries_the_wrong_build_notice(self):
        first = self.manifest['pages'][0]
        self.assertEqual(first['id'], 'standby')
        notice = [w for w in first['widgets'] if w.get('need_bind') == 'ui.wrong_build']
        rect = [w for w in notice if w['type'] == 'rect']
        self.assertEqual(len(rect), 1)
        self.assertEqual(rect[0]['rect'], [0, 0, generator.W, generator.H])
        self.assertTrue(rect[0]['input_block'])
        labels = [w['text'] for w in notice if w['type'] == 'label']
        self.assertEqual(labels, ['This companion supports', 'Skyward Sword HD 1.0.1 only'])
        # drawn last, over the module-art standby picture; built-in font only (no module art there)
        self.assertEqual(first['widgets'][-len(notice):], notice)
        for w in notice:
            self.assertNotIn('src', w)
        # the bind needs no per-build point: build_match is the host's own value
        derived = {d['name']: d for d in self.manifest['derived']}
        self.assertEqual(derived['ui.wrong_build'], {'name': 'ui.wrong_build', 'cmp': 'eq', 'a': 'build_match', 'b': 0})
        self.assertNotIn('build_match', self.data['points'])
        # page.mode reads per-build points, so another build never leaves the first page
        self.assertIn('link.x', derived['page.mode']['expr'])
        self.assertIn('link.x', self.data['points'])
        # no other page needs the notice (another build cannot reach it)
        for pid, ws in self.pages.items():
            if pid != 'standby':
                self.assertFalse([w for w in ws if w.get('need_bind') == 'ui.wrong_build'], pid)

    def test_item_check_cells_meet_the_dense_grid_minimum(self):
        gp = self.pages['gpouch']
        drags = [w for w in gp if w.get('draggable') and w.get('scroll') == 'icheck']
        self.assertEqual(len(drags), 1)
        d = drags[0]
        x, y, w, h = d['rect']
        self.assertGreaterEqual(min(w, h), 88)
        cw, ch = d['repeat_dx'], d['repeat_row_dy']
        self.assertGreaterEqual(cw - w, 12)
        self.assertGreaterEqual(ch - h, 12)
        # the drop target is the whole cell and holds the icon; the socket art is centred on both
        drop = next(v for v in gp if v.get('drop_action') == 'stock_to_{i}' and v['type'] == 'rect')
        dx, dy, dw, dh = drop['rect']
        self.assertTrue(dx <= x and dy <= y and x + w <= dx + dw and y + h <= dy + dh)
        socket = next(v for v in gp if v.get('scroll') == 'icheck' and ':socket/' in v.get('src', ''))
        sx, sy, sw, sh = socket['rect']
        self.assertEqual((sx * 2 + sw, sy * 2 + sh), (x * 2 + w, y * 2 + h))
        self.assertEqual((dx * 2 + dw, dy * 2 + dh), (x * 2 + w, y * 2 + h))
        self.assertTrue(socket['src'].endswith(f'socket/{sw}'))
        # S6: the storage counts use 40 px digits (>= 22 px measured, outline included)
        digits = [v for v in gp if v.get('scroll') == 'icheck' and ':digit/' in v.get('src', '')]
        self.assertEqual(len(digits), 2)
        for v in digits:
            self.assertGreaterEqual(v['rect'][3], 40)
            self.assertTrue(dx <= v['rect'][0] and v['rect'][0] + v['rect'][2] <= dx + dw)
            self.assertTrue(v['rect'][1] + v['rect'][3] <= dy + dh)

    def test_swipe_fallback_actions_are_gated_by_the_module(self):
        acts = self.manifest['actions']
        for name in ('slash_up', 'slash_down', 'slash_left', 'slash_right'):
            self.assertEqual(acts[name]['enabled_bind'], 'sshd.swipe.fallback')
        # the module publishes it from the same refusals as the helper path
        self.assertIn('"sshd.swipe.fallback"', self.cpp)
        body = self.cpp.split('int FallbackWhy(', 1)[1].split('\n}\n', 1)[0]
        for gate in ('FreeWhy(', 'OnLoftwing(', 'SilentRealm(', 'g.player == 0', 'SpinHands(', 'HasSword('):
            self.assertIn(gate, body)
        # the swipe surfaces still show only without a ready helper
        ready = self.pages['ready']
        swipes = [w for w in ready if str(w.get('id', '')).startswith('swipe_')]
        self.assertEqual(len(swipes), 2)
        for w in swipes:
            self.assertEqual(w['need_bind'], 'stick.fallback')

    def test_footer_contrast_variant_is_gone(self):
        # dev.48: the owner kept the footer look (2026-11-14); the unused high-contrast variant is removed
        self.assertFalse(hasattr(generator, 'FOOTER_HI_CONTRAST'))
        text = json.dumps(self.manifest)
        self.assertNotIn('plank/foothi/', text)
        self.assertNotIn('txt/plankhi/', text)
        self.assertNotIn('foothi', self.art)
        self.assertNotIn('plankhi', self.art)
        self.assertIn('variant == "foot"', self.art)  # plank/foot stays (the Options rows use it)

    def test_no_private_pointer_in_the_data_file(self):
        self.assertNotRegex(json.dumps(self.data), r'(?i)research|contracts/|analysis/')

    def test_unused_itemname_key_removed(self):
        self.assertNotIn('"itemname"', self.art)


class Dev43Test(unittest.TestCase):
    """dev.43 (Thor RC: a "Not now" over the swipe pad with no log line): every path that shows a
    refusal logs one line through Refuse (the swipe once per stroke, the L corner, a dropped queued
    sequence, the game thread's spin refusal); the no-helper fallback logs its gate changes and the
    debounced menu gate logs each open / close with the raw word."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.cpp = (root / 'native/modules/01002DA013484000.cpp').read_text()

    def test_every_refusal_path_logs(self):
        for call in ('Refuse(inst, "swipe", no_sword ? kWhyNoSword : LBlockWhy(blocked), note);',
                     'Refuse(inst, "l_toggle", LBlockWhy(why), note);',
                     'Refuse(inst, "queued sequence", why, note);',  # dev.45: why = state / hands / helper
                     'Refuse(inst, "spin", inst->spin_why, note);',
                     'GateLine(inst, "swipe fallback", "off", fwhy, "");',
                     '"sshd: menu gate %s: state %s (%s) stage %.8s (the gate was %s %d samples)"'):  # dev.44 form
            self.assertIn(call, self.cpp)
        # once per stroke: the line is guarded by the stroke's own refused flag
        swipe = self.cpp[self.cpp.index('// the game cannot slash now'):]
        swipe = swipe[:swipe.index('inst->stroke_refused = true;')]
        self.assertIn('if (!inst->stroke_refused) {', swipe)
        # every "Not now" flash is set next to a log line (pouch / spin / bash / swipe)
        for m in re.finditer(r'inst->(seq|pouch|bash|swipe)_flash = ', self.cpp):
            near = self.cpp[max(0, m.start() - 1400):m.start() + 900]
            self.assertTrue('Refuse(' in near or 'SpinRefuse(' in near, self.cpp[m.start():m.start() + 80])


class Dev44Test(unittest.TestCase):
    """dev.44 (Thor RC: "L lock paused (2)" for seconds in plain play): main+0x192D0E4 was a slot of the
    game thread's stack. The menu gate reads the layout controller's state (dLytControlGame_c +0x120
    against StateID_Normal) from the helper's game-thread sample, stamped with the loop heartbeat, and
    falls back to reading it directly; nothing gates on the stack slot any more."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.cpp = (root / 'native/modules/01002DA013484000.cpp').read_text()
        cls.helper = (root / 'native/helper/sshd_stick_helper.S').read_text()
        cls.plan_tool = (root / 'tools/skyward-sword-hd/build_helper_plan.py').read_text()

    def test_no_gate_reads_the_stack_slot(self):
        self.assertNotIn('kMenuFlag', self.cpp)
        self.assertNotIn('0x192D0E4;', self.cpp)
        self.assertIn('constexpr uint64_t kLytControlGame = 0x183AC40;', self.cpp)
        self.assertIn('constexpr uint64_t kCtrlState = 0x120;', self.cpp)
        self.assertIn('constexpr uint64_t kStateNormal = 0x164F988;', self.cpp)

    def test_helper_samples_the_state_each_frame(self):
        loop = self.helper[self.helper.index('sshd_loop_hook:'):self.helper.index('sshd_draw_hook:')]
        # right after the loop heartbeat: the controller, its state, the code, the distance, the stamp last
        self.assertRegex(loop, r'str\s+w9, \[x19, #0x68\][^\n]*\n(?:\s*//[^\n]*\n)?\s+mov\s+w11, #2')
        self.assertRegex(loop, r'ldr\s+x10, \[x10, :lo12:sshd_lyt_control_game\]\n\s+cbz\s+x10, 46f\n'
                               r'\s+ldr\s+x10, \[x10, #0x120\]')
        self.assertRegex(loop, r'str\s+w11, \[x19, #0x6C\]\n\s+str\s+w12, \[x19, #0x74\]\n'
                               r'\s+add\s+x10, x19, #0x70\n\s+stlr\s+w9, \[x10\]')
        self.assertIn("'sshd_lyt_control_game': ('main', LYT_CONTROL_GAME)", self.plan_tool)
        self.assertIn("'sshd_state_normal': ('main', STATE_NORMAL)", self.plan_tool)
        self.assertIn('assert len(relocs) == 36', self.plan_tool)  # dev.48: + the stick hook's event manager
        self.assertIn("'isStateNormal check moved'", self.plan_tool)

    def test_module_prefers_a_fresh_sample(self):
        self.assertIn('constexpr uint32_t kOffMenuCode = 0x6C, kOffMenuStamp = 0x70, kOffMenuDelta = 0x74;', self.cpp)
        self.assertIn('return loop_alive && stamp != 0 && lag >= -1 && lag <= 1 && code <= static_cast<uint32_t>(kMenuNone);',
                      self.cpp)
        # the gates: a menu (2) and an unreadable state (7) are told apart
        lb = self.cpp[self.cpp.index('int LBlocked('):self.cpp.index('int LBlockWhy(')]
        self.assertIn('if (inst->menu_open)\n        return 2;\n    if (inst->menu_unread)\n        return 7;', lb)
        self.assertIn('case 7: return kWhyUnread;', self.cpp)
        self.assertIn('constexpr int kMenuUnreadGrace = 30;', self.cpp)
        self.assertIn('constexpr int kMenuOpenRun = 3;', self.cpp)  # dev.40 debounce kept
        self.assertIn('constexpr int kMenuCloseRun = 2;', self.cpp)



class Dev45Test(unittest.TestCase):
    """dev.45 (Thor dev.44): the L lock's virtual L comes from the guest helper on the game thread, not
    the runtime's enforce rule (one host log line per 50 ms press, 9.2k lines in 10 min); every swipe
    ends fed, replayed (staged) or refused, with a line (two strokes were dropped silently)."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.cpp = (root / 'native/modules/01002DA013484000.cpp').read_text()
        cls.helper = (root / 'native/helper/sshd_stick_helper.S').read_text()
        cls.plan = json.loads((root / 'packages/SkywardSwordHD/dualscreen/load-plans' /
                               (generator.BUILD + '0' * 24 + '.json')).read_text())

    def test_no_enforce_rule(self):
        manifest, _ = generator.build()
        self.assertNotIn('enforce', manifest)
        self.assertNotIn('enforce_gate', manifest)
        self.assertNotIn('l_hold', manifest['actions'])

    def test_stick_hook_holds_l(self):
        stick = self.helper[self.helper.index('sshd_stick_hook:'):self.helper.index('sshd_loop_hook:')]
        code = re.sub(r'[ \t]*//[^\n]*\n', '', stick)
        # the copy for the module carries the helper's L too; the game's word gets bit 0x4000 (npad bit 6);
        # dev.48: not in a menu (Dev48Test) and not during an event; the copy is stored after every button
        self.assertRegex(code, r'ldr\s+w10, \[x9, #0x20\]\n\s+cbz\s+w10, 7f\n'
                               r'\s+adrp\s+x10, sshd_event_manager\n\s+ldr\s+x10, \[x10, :lo12:sshd_event_manager\]\n'
                               r'\s+cbz\s+x10, 7f\n\s+ldr\s+w10, \[x10, #0x248\]\n\s+cbnz\s+w10, 7f\n'
                               r'\s+orr\s+w11, w11, #0x40\n\s+ldr\s+w10, \[x20\]\n\s+orr\s+w10, w10, #0x4000\n'
                               r'\s+str\s+w10, \[x20\]\n7:\n')
        self.assertRegex(code, r'\n6:\n\s+str\s+w11, \[x9, #0x14\]\n')
        self.assertNotIn('str     w24, [x9, #0x14]', stick)
        # the hook only ORs: physical L passes through untouched
        self.assertNotRegex(stick, r'bic\s+w1[01]')

    def test_loop_hook_counts_the_hold_down(self):
        loop = self.helper[self.helper.index('sshd_loop_hook:'):self.helper.index('sshd_draw_hook:')]
        self.assertRegex(loop, r'ldr\s+w9, \[x19, #0x20\][^\n]*\n\s+cbz\s+w9, 45f[^\n]*\n\s+sub\s+w9, w9, #1\n'
                               r'\s+str\s+w9, \[x19, #0x20\]\n45:')
        # before the configured check fails nothing runs (the magic gate stays first)
        self.assertLess(loop.index('b.ne    9f'), loop.index('#0x20]'))

    def test_module_refreshes_the_hold(self):
        self.assertIn('constexpr uint32_t kOffLHold = 0x20;', self.cpp)
        self.assertIn('constexpr uint32_t kLHoldFrames = 8;', self.cpp)
        lt = self.cpp[self.cpp.index('void LTick('):self.cpp.index('// Shield bash')]
        self.assertIn('inst->ext.store_u32(inst->ext.userdata, kOffLHold, hold ? kLHoldFrames : 0u);', lt)
        cf = self.cpp[self.cpp.index('void Configure('):self.cpp.index('EdenDsmodBool OnAction(')]
        self.assertIn('ReleaseInputs(*ext);', cf)  # dev.48: kOffLHold among every virtual button
        ds = self.cpp[self.cpp.index('void Destroy('):self.cpp.index('void Destroy(') + 600]
        self.assertIn('ReleaseInputs(inst->ext);', ds)
        self.assertIn('for (const uint32_t off : {kOffTtl, kOffBash, kOffPutAway, kOffLHold, kOffStab})', self.cpp)

    def test_code_budget(self):
        cave = next(p for p in self.plan['patches'] if p['expected'] == '00' * (len(p['replacement']) // 2))
        size = len(cave['replacement']) // 2
        self.assertEqual(size, 1684)   # dev.45 1620 + the R3 pulse / expiry (9) + dev.48 menu / event skips (7)
        self.assertLessEqual(size, 1712)  # the .text padding (build_helper_plan.py asserts the room)
        self.assertEqual(len(cave['relocations']), 6)  # the three mailbox adrp+add pairs, unchanged by dev.45

    def test_every_stroke_has_an_outcome(self):
        se = self.cpp[self.cpp.index('void SwipeEnded('):self.cpp.index('void StickMirror(void* instance, const EdenDsmodHostApi* host) {')]
        self.assertIn('constexpr int kLateFed = 4;', self.cpp)
        for line in ('"sshd: swipe %u fed (%d ticks%s)"', '"sshd: swipe %u flick replays (the stroke waited %d ticks for L)"',
                     '"sshd: swipe %u fed only %d ticks after the L release: its flick replays"',
                     '"sshd: swipe %u flick waits for the stick (sequence %u in flight)"',
                     'Refuse(inst, "swipe", kWhyShort, msg);', 'Refuse(inst, "swipe", kWhyBusy, msg);'):
            self.assertIn(line, se)
        lt = self.cpp[self.cpp.index('void LTick('):self.cpp.index('// Shield bash')]
        self.assertIn('Refuse(inst, "swipe", why, note);', lt)            # a late flick that cannot play
        self.assertIn('Refuse(inst, "swipe", kWhyBusy, note);', lt)       # one that waited too long
        sp = self.cpp[self.cpp.index('void SpinPoll('):self.cpp.index('// Writes a stick sequence table')]
        self.assertIn('Refuse(inst, "swipe", code == 5 ? kWhyEvent : code == 6 ? kWhyItem : kWhyGame, note);', sp)
        self.assertIn('"too short"', self.cpp)


class Dev46Test(unittest.TestCase):
    """dev.46 UI polish (owner 2026-11-15: "use alt A, but top bar like bag pouch and gear use alt B. for
    ux spec i only accept p01 and p02, also no beacon text with marker vertically center"): the FIT-RULES
    automatic checks (the UX spec's text fit rules, its checklist section) that apply to the
    changed widgets: the choice planks (footer, Fly footer, Back, Reset), the Options rows (Bag tabs,
    Gear header), the refusal pill (P01), the Gear sword / shield labels (P02) and the Fly beacon plate.
    Ink boxes are static: a txt/ picture's ink is its width - 8 (4 px pad each side) wide and its cap box
    REAL_CAP x cap tall around CAP_CY (rendered "HIH", dsmod-sshd-art-test); contrast (C7) is measured on
    the live captures (legibility self-check), not here."""
    REAL_CAP = 1.133  # rendered capital height / the txt/ cap parameter (normal_00)
    SOCKETS = (':socket/', ':sq/DoButton/uc_', ':sq/DoButton/tr_rupy_00', ':sq/DoButton/tr_zrBtn_00',
               ':sq/DoButton/tr_rBtn_00', ':sq/DoButton/tr_lBtn_01', ':sq/DoButton/tr_rStick_')

    @classmethod
    def setUpClass(cls):
        cls.manifest, _ = generator.build()
        cls.pages = {p['id']: p['widgets'] for p in cls.manifest['pages']}
        cls.derived = {d['name']: d for d in cls.manifest['derived']}
        root = Path(__file__).resolve().parents[2] / 'native/modules'
        cls.cpp = (root / '01002DA013484000.cpp').read_text()
        cls.art = (root / 'sshd_art.cpp').read_text()

    # ---- geometry of the new art
    @staticmethod
    def kind(w):
        src = w.get('src', '')
        for k in ('choice', 'optrow', 'dopill'):
            if f':{k}/' in src:
                return k
        return None

    def body(self, w):
        """visible body (x0, y0, x1, y1) and safe box of a choice / optrow / dopill picture"""
        x, y, ww, hh = w['rect']
        k = self.kind(w)
        if k == 'choice':
            v0, v1 = generator.CHOICE_VIS[hh]
            m = round(hh * 11 / 120)
            vis = (x + m, y + v0, x + ww - m, y + v1)
            rim = round(hh * 20 / 120)  # the gold rim ends 20 px in at 120 px (offline render)
            safe = (x + rim, y + v0 + round(hh / 10), x + ww - rim, y + v1 - round(hh / 10))
        elif k == 'optrow':
            v0, v1 = generator.OPTROW_VIS[hh]
            vis = (x + 8, y + v0, x + ww - 8, y + v1)
            safe = (vis[0] + 6, vis[1] + 6, vis[2] - 6, vis[3] - 6)
        else:
            vis = (x + 2, y + 2, x + ww - 2, y + hh - 2)
            safe = vis
        return vis, safe

    def ink(self, lab):
        """cap box of a txt/ label: (x0, y0, x1, y1), its centre and the cap parameter"""
        x, y, ww, hh = lab['rect']
        cap = int(lab['src'].split('/')[-2])
        cx, cy = x + ww / 2 + generator.INK_DX, y + generator.CAP_CY[cap]
        half = self.REAL_CAP * cap / 2
        return (cx - (ww - 8) / 2, cy - half, cx + (ww - 8) / 2, cy + half), (cx, cy), cap

    def labels(self, page, styles=('choice', 'optrow', 'doinfo')):
        return [w for w in self.pages[page] if any(f':txt/{s}/' in w.get('src', '') for s in styles)]

    def container(self, page, lab):
        (x0, y0, x1, y1), (cx, cy), _ = self.ink(lab)
        for w in self.pages[page]:
            if self.kind(w) and not str(w.get('need_bind', '')).startswith('press.'):
                (a0, b0, a1, b1), _ = self.body(w)
                if a0 <= cx <= a1 and b0 <= cy <= b1:
                    return w
        return None

    # ---- C1..C6: every new label sits in its plank / row / pill, cap-centred, padded, legible size
    def test_c1_c3_labels_are_cap_centred_in_their_container(self):
        n = 0
        for page in self.pages:
            for lab in self.labels(page):
                w = self.container(page, lab)
                self.assertIsNotNone(w, (page, lab['src']))                                # C1
                (vx0, vy0, vx1, vy1), (sx0, sy0, sx1, sy1) = self.body(w)
                (ix0, iy0, ix1, iy1), (cx, cy), cap = self.ink(lab)
                self.assertLessEqual(abs(cx - (vx0 + vx1) / 2), 1.5, (page, lab['src']))   # C3
                self.assertLessEqual(abs(cy - (vy0 + vy1) / 2), 1.5, (page, lab['src']))
                if self.kind(w) == 'dopill':
                    # R14: ink >= 12 px from the pill's edges across, >= 10 down (the pill's own rule)
                    self.assertTrue(ix0 - vx0 >= 12 - 2 and vx1 - ix1 >= 12 - 2, (page, lab['src']))
                    self.assertTrue(iy0 - vy0 >= 10 - 2 - 4 and vy1 - iy1 >= 10 - 2 - 4, (page, lab['src']))
                else:
                    # C2 / C4: pad >= 8 across and >= 6 down to the safe box (inside the rim)
                    self.assertTrue(ix0 - sx0 >= 8 and sx1 - ix1 >= 8, (page, lab['src'], ix0 - sx0, sx1 - ix1))
                    self.assertTrue(iy0 - sy0 >= 6 and sy1 - iy1 >= 6, (page, lab['src'], iy0 - sy0, sy1 - iy1))
                # C6: W1 labels >= 20, W2 >= 18, refusal words (read in play) >= 22, nothing below 15
                self.assertGreaterEqual(cap, 22 if ':txt/doinfo/' in lab['src'] else 26 if 'Reset' in lab['src'] else 28)
                n += 1
        self.assertGreaterEqual(n, 20)

    # ---- C5 / C15 / R14: one refusal component, never over a socket, icon, badge or count
    def test_c5_c15_one_refusal_pill_clear_of_the_art(self):
        refusal = ('sshd.bash.', 'sshd.spin.', 'sshd.swipe.', 'sshd.pouch.')
        for page, ws in self.pages.items():
            gated = [w for w in ws if str(w.get('need_bind', '')).startswith(refusal) and w.get('type') == 'image']
            pills = [w for w in gated if self.kind(w) == 'dopill']
            words = [w for w in gated if ':txt/' in w['src']]
            self.assertEqual({w['src'] for w in pills} - {generator.K + f'dopill/{generator.PILL_W}/{generator.PILL_H}',
                                                          generator.K + f'dopill/{generator.PILL_W}/{generator.PILL_SHIELD_H}'}, set())
            # no bare word left (dev.48: font 24, the Ready shield pill font 22)
            self.assertTrue(all(':txt/doinfo/%d/' % (generator.PILL_SHIELD_CAP if w['need_bind'].startswith('sshd.bash.')
                                                      else generator.PILL_CAP) in w['src'] for w in words), page)
            for wd in words:
                self.assertIs(self.container(page, wd), next(p for p in pills if self.container(page, wd) is p), page)
            art = [w for w in ws if any(s in w.get('src', '') for s in self.SOCKETS) or ':digit/' in w.get('src', '')]
            for p in pills:
                x, y, ww, hh = p['rect']
                self.assertTrue(x >= 12 and x + ww <= generator.W - 12, (page, p['rect']))   # R14 screen margin
                for a in art:
                    ax, ay, aw, ah = a['rect']
                    if a.get('x_bind') == 'ru.shift':
                        ax += -2 * 27  # the rupee digits centre on x 120 (ru.shift)
                    hit = x - 4 < ax + aw and ax < x + ww + 4 and y - 4 < ay + ah and ay < y + hh + 4
                    # the rupee icon's art starts 5 px into its rect (y 265, review note)
                    if a['src'].endswith('tr_rupy_00'):
                        hit = hit and y + hh > ay + 5
                    self.assertFalse(hit, (page, p['rect'], a['src'], a['rect']))
        # P01 places: under the Shield plank and under each spin socket; the swipe on the minimap
        # (dev.48: the shield pill fills y 213..265, the spin pills PILL_GAP under their sockets)
        ready = self.pages['ready']
        tops = sorted((w['rect'][0], w['rect'][1], w['need_bind']) for w in ready if self.kind(w) == 'dopill')
        self.assertEqual([t[2] for t in tops], ['sshd.bash.refused', 'sshd.swipe.refused', 'sshd.spin.refused', 'sshd.spin.refused'])
        self.assertEqual(tops[0][:2], (12, 213))
        self.assertLessEqual(tops[0][1] + generator.PILL_SHIELD_H, 265)   # clear of the rupee icon
        self.assertEqual([t[1] for t in tops[2:]], [generator.SPIN_TOP[0] + generator.SPIN_D + generator.PILL_GAP,
                                                    generator.SPIN_TOP[1] + generator.SPIN_D + generator.PILL_GAP])
        # the spin pills stay right of the stamina ring (radius STAM_R[1] around the minimap centre)
        for x, y, _ in tops[2:]:
            dy = max(abs(generator.MY - y), abs(generator.MY - (y + generator.PILL_H)))
            ring_x = generator.MX + (generator.STAM_R[1] ** 2 - min(dy, generator.STAM_R[1]) ** 2) ** 0.5
            self.assertGreaterEqual(x - ring_x, 12)

    # ---- C11 / C12: tap rects contain their visible plank, primary >= 112, gaps >= 12
    def test_c11_c12_tap_rects_contain_the_planks(self):
        nav = ('go_gear', 'go_map', 'go_gpouch', 'go_ready', 'go_fly', 'go_coll', 'go_set')
        for page, ws in self.pages.items():
            taps = [w for w in ws if str(w.get('on_tap', '')).startswith(nav + ('map_reset_', 'sky_reset'))
                    and not (page == 'ready' and w['rect'][2] == generator.CB)]  # the corners: unchanged
            planks = [w for w in ws if self.kind(w) in ('choice', 'optrow') and not str(w.get('need_bind', '')).startswith('press.')
                      and not (page == 'gear' and self.kind(w) == 'optrow')]  # the Gear header is a title, not a button
            for p in planks:
                (vx0, vy0, vx1, vy1), _ = self.body(p)
                mine = [t for t in taps if t['rect'][0] <= vx0 and t['rect'][1] <= vy0 and
                        vx1 <= t['rect'][0] + t['rect'][2] and vy1 <= t['rect'][1] + t['rect'][3]]
                self.assertTrue(mine, (page, p['src']))
                for t in mine:
                    self.assertGreaterEqual(min(t['rect'][2], t['rect'][3]), 112)
            rects = {tuple(t['rect']) for t in taps}
            for a in rects:
                for b in rects:
                    if a < b:
                        gx = max(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2]))
                        gy = max(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]))
                        self.assertGreaterEqual(max(gx, gy), 12, (page, a, b))
        # the footer row: three 250 x 134 taps, 17 px apart, 16 px from the L / R corner taps
        ready = [w for w in self.pages['ready'] if w.get('on_tap') in ('go_gear', 'go_map', 'go_gpouch') and w['rect'][1] == 930]
        self.assertEqual([w['rect'] for w in ready], [[x, 930, 250, 134] for x in generator.READY_FOOT_X])
        self.assertEqual(ready[0]['rect'][0] - (16 + generator.CB), 16)
        self.assertEqual((generator.W - 16 - generator.CB) - (ready[2]['rect'][0] + 250), 16)

    # ---- C14 / C19: weight order; one footer line
    def test_c14_c19_weight_order_and_one_footer_line(self):
        w1, w2 = [], []
        for page in self.pages:
            for lab in self.labels(page, ('choice', 'optrow')):
                cap = int(lab['src'].split('/')[-2])
                (w2 if 'Reset' in lab['src'] else w1).append(cap)
        self.assertGreaterEqual(min(w1), max(w2))                                        # C14 fonts
        self.assertGreater(generator.TAB_W * generator.TAB_ART_H, generator.RESET_W * generator.RESET_ART_H)  # C14 plates
        # C19: Ready, Fly and Back labels cap-centred on the same line, the planks ending on y 1064
        lines = set()
        for page in ('ready', 'fly', 'gear', 'mapfull', 'gpouch', 'coll', 'settings'):
            for w in self.pages[page]:
                if self.kind(w) == 'choice' and w['rect'][1] > 900:
                    (_, vy0, _, vy1), _ = self.body(w)
                    self.assertEqual(vy1, generator.FOOT_BOTTOM)
            for lab in self.labels(page, ('choice',)):
                if lab['rect'][1] > 900:
                    lines.add(self.ink(lab)[1][1])
        self.assertEqual(len(lines), 1, lines)
        # the Ready and Fly footer fonts stay as before (CF-6 / P10 not adopted): 30 and 32
        caps = {lab['src'].split('/')[-2] for page in ('ready', 'fly') for lab in self.labels(page, ('choice',))
                if lab['rect'][1] > 900}
        self.assertEqual(caps, {'30', '32'})

    # ---- C16: every pressed look exists, its flag clears on a page change
    def test_c16_pressed_overlays_and_flag(self):
        seen = set()
        for page, ws in self.pages.items():
            for w in ws:
                nb = str(w.get('need_bind', ''))
                if nb.startswith('press.'):
                    self.assertTrue(w['src'].split('/')[-1] == '2' or w['src'].endswith('/2/1'), w['src'])
                    seen.add(int(nb[6:]))
                    self.assertEqual(self.derived[nb], {'name': nb, 'cmp': 'eq', 'a': 'press.id', 'b': int(nb[6:])})
        self.assertEqual(seen, set(generator.PRESS.values()))
        # the buttons that stay on the page select their id; nothing else uses the group
        sel = {(page, w['payload']) for page, ws in self.pages.items() for w in ws if w.get('select_group') == 'sshd.press'}
        self.assertEqual({p for _, p in sel}, {generator.PRESS['map_reset'], generator.PRESS['fly_reset']})
        i = self.cpp.index('void PressTick(')
        pt = self.cpp[i:self.cpp.index('void Tick(void* instance', i)]
        for key in ('"sshd.ms"', '"sshd.press.hold"', '"sshd.press.diag"'):
            self.assertIn(key, pt)
        self.assertIn('constexpr int kPressLookMs = 150;', self.cpp)
        self.assertIn('press = num("SSHD_DIAG_PRESS");', self.cpp)
        # the countdown, run tick by tick with the runtime's rules (hold_last_nonzero + hold_gate: hold the
        # source while the gate is open, else the held value, else the source; "@sel:" -1 when nothing is
        # selected; a page change clears the selection)
        d = self.derived
        held = {}

        def tick(ms, sel, diag=0):
            v = {'sshd.ms': ms, '@sel:sshd.press': sel, 'sshd.press.hold': 150, 'sshd.press.diag': diag}
            v['press.on'], v['press.off'] = int(sel >= 0), int(sel < 0)
            for n in ('press.t_on', 'press.t_off', 'press.last'):
                src, gate = v[d[n]['hold_last_nonzero']], v[d[n]['hold_gate']]
                if gate:
                    held[n] = src
                v[n] = held.get(n, src)
            self.assertEqual(d['press.id']['expr'], 'press.last > 0 && sshd.ms - (press.on ? press.t_on : press.t_off) < '
                                                   'sshd.press.hold ? press.last : sshd.press.diag')
            age = ms - (v['press.t_on'] if sel >= 0 else v['press.t_off'])
            v['press.id'] = v['press.last'] if v['press.last'] > 0 and age < 150 else diag
            return v['press.id']

        self.assertEqual(tick(1, -1, diag=4), 4)                    # never pressed: the diagnostics id (live 20261115T0748)
        held.clear()
        ids = [tick(t, -1) for t in range(1, 2000, 16)]           # idle: nothing lit
        self.assertTrue(all(i == 0 for i in ids))
        lit = [tick(t, 1) for t in range(2000, 2400, 16)]          # Reset tapped at 2000 (selects 1)
        self.assertEqual(lit[:9], [1] * 9)                         # 150 ms of the pressed look ...
        self.assertTrue(all(i == 0 for i in lit[10:]))             # ... then the plank alone
        again = [tick(t, -1) for t in range(2400, 2800, 16)]       # a second tap clears the selection
        self.assertEqual(again[:9], [1] * 9)
        self.assertTrue(all(i == 0 for i in again[10:]))
        self.assertEqual(tick(2800, -1, diag=4), 4)                 # SSHD_DIAG_PRESS lights its id ...
        self.assertEqual(tick(2816, 1, diag=4), 1)                  # ... while a live press still shows
        # no action waits for it: the footer / Back / tab actions are the plain page switches
        for a in ('go_gear', 'go_map', 'go_gpouch', 'go_ready', 'go_fly', 'go_coll', 'go_set'):
            self.assertEqual(self.manifest['actions'][a]['kind'], 'page')
            self.assertEqual(self.manifest['actions'][a]['duration_ms'], 150)

    # ---- C17 (static part): the open tab is the row's focus state, the others the plain row
    def test_c17_bag_tabs_open_tab_is_the_lit_row(self):
        for page, active in (('gpouch', 'Pouch'), ('coll', 'Collection'), ('settings', 'Options')):
            rows = [w for w in self.pages[page] if self.kind(w) == 'optrow' and w['rect'][1] < 40
                    and not str(w.get('need_bind', '')).startswith('press.')]
            self.assertEqual(len(rows), 3)
            states = [w['src'].rsplit('/', 1)[1] for w in rows]
            self.assertEqual(states, ['1' if n == active else '3' for n, _ in generator.BAG_TABS])
        # the Gear header is the same row family, with no tap
        hdr = [w for w in self.pages['gear'] if self.kind(w) == 'optrow']
        self.assertEqual(len(hdr), 1)
        self.assertFalse(any(w.get('on_tap') and w['rect'][1] < 112 for w in self.pages['gear']))

    # ---- no old plank under the changed buttons; the floor tabs keep their art key
    def test_old_footer_and_tab_planks_are_gone(self):
        text = json.dumps(self.manifest)
        for gone in (f'plank/foot/{generator.READY_FOOT_W}', f'plank/foot/{generator.W - 48}/130', 'plank/foot/596/120',
                     f'plank/foot/{generator.TAB_W}/96', f'plank/corner/{generator.TAB_W}/96', f'plank/corner/{generator.W - 48}/96',
                     f'plank/corner/{generator.RESET_W}/{generator.RESET_H}', 'txt/plank/26/Not now', 'txt/plank/26/Put away'):
            self.assertNotIn(gone, text)
        self.assertIn('floortab/0/0', text)
        # the floor tab art uses the clamped 3-slice (dev.46: no 80-unit stretch of the end)
        ft = self.art[self.art.index('std::optional<Img> Art::FloorTab('):self.art.index('std::optional<Img> Art::Choice(')]
        self.assertIn('Clamp3(TevA(tex, b, w, a), cap, bh, bw)', ft)
        self.assertNotIn('static_cast<int>(80 * U)', ft)

    # ---- item 5: the Fly beacon plate has no word, the marker and count centred on it
    def test_fly_beacon_plate_marker_centred_without_word(self):
        fly = self.pages['fly']
        self.assertFalse(any('Beacons' in w.get('src', '') for w in fly))
        plate = next(w for w in fly if w.get('src', '').endswith(':plank/corner/230/150'))
        mark = next(w for w in fly if w.get('src', '').endswith(':beacon/84'))
        count = next(w for w in fly if w.get('bind') == 'sshd.bc.left')
        body_cy = plate['rect'][1] + 150 / 2
        for w in (mark, count):
            self.assertEqual(w['rect'][1] + w['rect'][3] / 2, body_cy)

    # ---- P02: the shield socket up 38, its label up 56 (inside the panel), Sword up 28
    def test_p02_gear_shield_label_inside_the_panel(self):
        ge = self.pages['gear']
        panel = next(w for w in ge if w.get('src', '').endswith(':plank/corner/360/700'))
        inner_bottom = panel['rect'][1] + 700 - 18
        shield = next(w for w in ge if w.get('src', '').endswith(':txt/plank/26/Shield'))
        sword = next(w for w in ge if w.get('src', '').endswith(':txt/plank/26/Sword'))
        sock = next(w for w in ge if w.get('src', '').endswith(':socket/150') and w['rect'][0] == 153)
        self.assertEqual((shield['rect'][1], sock['rect'][1], sword['rect'][1]), (836 - 56, 680 - 38, 600 - 28))
        (_, iy0, _, iy1), _, _ = self.ink(shield)
        self.assertLessEqual(iy1, inner_bottom - 4)                    # R4: off the rim
        (_, sy0, _, sy1), _, _ = self.ink(sword)
        self.assertLess(sy1, sock['rect'][1])                           # Sword clear of the raised socket


class Dev47Test(unittest.TestCase):
    def test_map_tap_only_on_disc_and_preserves_pad_gestures(self):
        manifest, _ = generator.build()
        pages = {p['id']: p['widgets'] for p in manifest['pages']}
        ready = pages['ready']
        taps = [w for w in ready if w.get('on_tap') == 'sword_stab']
        self.assertGreater(len(taps), 60)
        self.assertEqual(manifest['actions']['sword_stab'],
                         {'kind': 'module', 'action': 'sword_stab', 'argument': 0})
        for w in taps:
            x, y, width, height = w['rect']
            for xx in (x, x + width):
                for yy in (y, y + height):
                    self.assertLessEqual((xx - generator.MX)**2 + (yy - generator.MY)**2, generator.MR**2)
            self.assertNotIn('pan_zoom', w)
            self.assertNotIn('input_block', w)
            self.assertNotIn('draggable', w)
            self.assertNotIn('select_group', w)  # dev.48: the rim's look follows the module (Dev48Test)
        frame = next(w for w in ready if w.get('bind') == 'sshd.stab.lit')
        self.assertEqual(frame['src_names'], [generator.K + 'frame', generator.K + 'frame/pressed'])
        self.assertTrue(any(w.get('id') == 'stickpad_0' and w['pan_zoom'] for w in ready))
        for page, ws in pages.items():
            if page != 'ready':
                self.assertFalse(any(w.get('on_tap') == 'sword_stab' for w in ws))

    def test_scroll_strip_excludes_every_selectable_or_draggable_cell(self):
        manifest, _ = generator.build()
        regions = 0
        for page in manifest['pages']:
            for scroll in page.get('scrolls', []):
                regions += 1
                x, y, width, height = scroll['rect']
                self.assertGreaterEqual(scroll['bar_w'], 64)
                self.assertLessEqual(x + width, 1196)  # inside the panel's gold rim
                strip_left = x + width - scroll['bar_w']
                cells = [w for w in page['widgets'] if w.get('scroll') == scroll['id']]
                for w in cells:
                    if w.get('draggable') or w.get('select_group') or w.get('drop_action'):
                        right = w['rect'][0] + w['rect'][2] + (w.get('repeat_cols', 1) - 1) * w.get('repeat_dx', 0)
                        self.assertLessEqual(right + 12, strip_left)
                icons = [w for w in cells if w.get('draggable')]
                self.assertTrue(icons)
                self.assertGreaterEqual(icons[0]['rect'][2], 88)
        self.assertEqual(regions, 1)


class Dev48Test(unittest.TestCase):
    """dev.48 release hardening: the helper adds no virtual button in a menu (and no L in an event); the
    taller refusal pill (geometry from the offline render of the pill and its words); the wider Item
    Check scroll strip; the lock badge on the 12 px canvas margin; the map rim lit by the module only;
    the behaviour-neutral clean-ups."""
    # Offline render (dsmod-sshd-art-test, the player's romfs): rows / cols of the words' ink (alpha >= 0.5,
    # the drop shadow and the descender included) in their txt/doinfo/<font>/<word> pictures, and the
    # refusal pill's navy body (dopill/216/<h>: rows 9 .. h - 10, cols 19 .. 196; dev.48 DoPill keeps the
    # 44 px pill's rim). Capital stems (alpha >= 0.9 face): font 24 = 23 px, font 22 = 21 px.
    INK = {(24, 'Not now'): ((5, 149), (13, 40)), (24, 'Put away'): ((5, 165), (13, 44)),
           (22, 'Not now'): ((4, 137), (12, 36)), (22, 'Put away'): ((4, 152), (12, 39))}
    NAVY_TOP, NAVY_BOTTOM, NAVY_X0, NAVY_X1 = 9, 9, 19, 196  # navy rows NAVY_TOP .. h - NAVY_BOTTOM - 1
    STEM = {24: 23, 22: 21}

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.root = root
        cls.manifest, _ = generator.build()
        cls.pages = {p['id']: p['widgets'] for p in cls.manifest['pages']}
        cls.cpp = (root / 'native/modules/01002DA013484000.cpp').read_text()
        cls.helper = (root / 'native/helper/sshd_stick_helper.S').read_text()
        cls.hud = (root / 'native/modules/sshd_hudhide.cpp').read_text()
        cls.hudh = (root / 'native/modules/sshd_hudhide.h').read_text()
        cls.art = (root / 'native/modules/sshd_art.cpp').read_text()
        cls.plan_tool = (root / 'tools/skyward-sword-hd/build_helper_plan.py').read_text()
        cls.plan = json.loads((root / 'packages/SkywardSwordHD/dualscreen/load-plans' /
                               (generator.BUILD + '0' * 24 + '.json')).read_text())

    # ---- A1: the helper's virtual buttons
    def test_a1_no_virtual_button_in_a_menu(self):
        stick = self.helper[self.helper.index('sshd_stick_hook:'):self.helper.index('sshd_loop_hook:')]
        code = re.sub(r'[ \t]*//[^\n]*\n', '', stick)
        # the menu sample gates all four ORs: a non-zero sample jumps past them to the copy's store
        self.assertRegex(code, r'mov\s+w11, w24\n\s+ldr\s+w10, \[x9, #0x6C\]\n\s+cbnz\s+w10, 6f\n')
        gate = code.index('cbnz    w10, 6f')
        for bit in ('#0x4000', '#0x10000', '#0x20000', '#0x400'):
            self.assertGreater(code.index(bit), gate, bit)
            self.assertLess(code.index(bit), code.index('\n6:\n'), bit)
        # flag-free: no cmp / ccmp / cset / adds / subs / b.<cond> in the stick hook
        self.assertNotRegex(code, r'\b(cmp|ccmp|cmn|tst|cset|adds|subs|ands)\b|\bb\.[a-z]{2}\b')
        # the counters still run down in the loop hook (a menu never extends a press)
        loop = self.helper[self.helper.index('sshd_loop_hook:'):self.helper.index('sshd_draw_hook:')]
        for off in ('0x18', '0x1C', '0x20', '0x24'):
            self.assertRegex(loop, r'ldr\s+w9, \[x19, #%s\][^\n]*\n\s+cbz' % off)

    def test_a1_plan_and_budget(self):
        cave = next(p for p in self.plan['patches'] if p['expected'] == '00' * (len(p['replacement']) // 2))
        self.assertLessEqual(len(cave['replacement']) // 2, 1712)
        self.assertIn("'sshd_event_manager': ('main', 0x182DED8)", self.plan_tool)
        for name in ("expected orr w8,w25,w5", "'Pane::Draw prologue moved'", "'isStateNormal check moved'",
                     "'StateID_Normal moved'", "'text padding is not empty'", "'no room in .text padding'"):
            self.assertIn(name, self.plan_tool)  # the hard asserts stay

    # ---- A2: a queued sequence stages only in free play
    def test_a2_queued_sequence_needs_free_play(self):
        lt = self.cpp[self.cpp.index('void LTick('):self.cpp.index('// Shield bash')]
        self.assertIn('if (ready && !(blocked != 0 && state_ok)) {', lt)
        self.assertIn('blocked == 0 && hands && SeqStage(', lt)
        self.assertNotIn('state_ok && hands && SeqStage(', lt)

    # ---- A3: the refusal pill
    def pills(self):
        for page, ws in self.pages.items():
            for p in (w for w in ws if ':dopill/' in w.get('src', '')):
                words = [w for w in ws if ':txt/doinfo/' in w.get('src', '') and
                         p['rect'][0] <= w['rect'][0] + w['rect'][2] / 2 <= p['rect'][0] + p['rect'][2] and
                         p['rect'][1] <= w['rect'][1] + generator.CAP_CY[int(w['src'].split('/')[-2])] <= p['rect'][1] + p['rect'][3]]
                yield page, p, words

    def test_a3_pill_size_and_words(self):
        self.assertEqual((generator.PILL_W, generator.PILL_H, generator.PILL_CAP), (216, 64, 24))
        self.assertGreaterEqual(generator.PILL_H - self.NAVY_TOP - self.NAVY_BOTTOM, 45)  # navy interior
        self.assertGreaterEqual(self.STEM[generator.PILL_CAP], 22)  # the face's capital stem
        self.assertIn('constexpr int kPillBase = 44;', self.art)
        n = 0
        for page, p, words in self.pills():
            px, py, pw, ph = p['rect']
            shield = p['need_bind'] == 'sshd.bash.refused'
            self.assertEqual(ph, generator.PILL_SHIELD_H if shield else generator.PILL_H, page)
            self.assertTrue(words, (page, p['rect']))
            for w in words:
                font, word = int(w['src'].split('/')[-2]), w['src'].rsplit('/', 1)[1]
                self.assertEqual(font, generator.PILL_SHIELD_CAP if shield else generator.PILL_CAP)
                (c0, c1), (r0, r1) = self.INK[(font, word)]
                x0, x1 = w['rect'][0] + c0, w['rect'][0] + c1 + 1
                y0, y1 = w['rect'][1] + r0, w['rect'][1] + r1 + 1
                self.assertGreaterEqual(y0 - py, 10, (page, word))            # R14: ink to the pill edge
                self.assertGreaterEqual(py + ph - y1, 10, (page, word))
                self.assertGreaterEqual(x0 - px, 12, (page, word))
                self.assertGreaterEqual(px + pw - x1, 12, (page, word))
                self.assertGreaterEqual(x0 - (px + self.NAVY_X0), 4, (page, word))   # R4: off the gold rim
                self.assertGreaterEqual(px + self.NAVY_X1 + 1 - x1, 4, (page, word))
                self.assertGreaterEqual(y0 - (py + self.NAVY_TOP), 4, (page, word))
                bottom = py + ph - self.NAVY_BOTTOM - y1
                if shield and word == 'Put away':
                    self.assertEqual(bottom, 2)  # recorded: the "y" descender + shadow in the 52 px shield pill
                else:
                    self.assertGreaterEqual(bottom, 4, (page, word))
                n += 1
        self.assertEqual(n, 9)  # shield 2, two spins 2 each, swipe, Gear, Pouch

    def test_a3_pills_overlap_nothing(self):
        ready = self.pages['ready']
        # the Ready shield pill: below the plank's body (to y 212), above the rupee icon's art (from y 265)
        shield = next(p for pg, p, _ in self.pills() if p['need_bind'] == 'sshd.bash.refused')
        self.assertGreaterEqual(shield['rect'][1], 16 + generator.CB)
        self.assertLessEqual(shield['rect'][1] + shield['rect'][3], 265)
        art = (':socket/', ':sq/DoButton/uc_', ':sq/DoButton/tr_', ':digit/', ':doorkey', ':dpad/', ':sq/MenuPause/tr_')
        for page, p, _ in self.pills():
            x, y, w, h = p['rect']
            self.assertTrue(x >= 12 and x + w <= generator.W - 12)
            for a in self.pages[page]:
                src = a.get('src', '')
                if not any(k in src for k in art) or a.get('repeat'):
                    continue
                ax, ay, aw, ah = a['rect']
                if a.get('x_bind') == 'ru.shift':
                    ax += -2 * 27
                if src.endswith('tr_rupy_00'):
                    ay += 5   # its art starts 5 px into the rect
                if src.endswith('doorkey'):
                    ax, ay, aw, ah = ax + 14, ay + 14, aw - 28, ah - 28  # the keyhole's art (offline bbox)
                hit = x < ax + aw and ax < x + w and y < ay + ah and ay < y + h
                self.assertFalse(hit, (page, p['rect'], src, a['rect']))
        # Gear / Pouch: centred between the ring's top socket and the hub
        for page, cx, cy, r in (('gear', 820, 530, 290), ('gpouch', 372, 528, 282)):
            p = next(q for pg, q, _ in self.pills() if pg == page)
            top, hub = cy - r + 75, cy - 110
            self.assertLessEqual(abs(p['rect'][1] + p['rect'][3] / 2 - (top + hub) / 2), 1)

    # ---- A4: the Item Check scroll strip
    def test_a4_wider_scroll_strip(self):
        page = next(p for p in self.manifest['pages'] if p['id'] == 'gpouch')
        sc = page['scrolls'][0]
        x, y, w, h = sc['rect']
        strip0, strip1 = x + w - sc['bar_w'], x + w
        self.assertTrue(88 <= sc['bar_w'] <= 96, sc['bar_w'])
        self.assertLessEqual(strip1, 1208 - 18)          # inside the panel's safe box (body to 1208, rim 18)
        cells = [c for c in page['widgets'] if c.get('scroll') == sc['id']]
        icons = [c for c in cells if c.get('draggable')]
        self.assertEqual(icons[0]['rect'][2], 92)         # icons stay 92 (steward condition)
        for c in cells:
            if c.get('draggable') or c.get('drop_action') or c.get('select_group'):
                left = c['rect'][0]
                right = left + c['rect'][2] + (c.get('repeat_cols', 1) - 1) * c.get('repeat_dx', 0)
                self.assertGreaterEqual(left, x)          # every cell fully inside the region
                self.assertLessEqual(right, 1088)
                self.assertGreaterEqual(strip0 - right, 12)
        self.assertGreaterEqual(icons[0]['repeat_dx'] - icons[0]['rect'][2], 12)  # gaps between icons

    # ---- A5: the lock badge
    def test_a5_lock_badge_margin(self):
        key = next(w for w in self.pages['ready'] if w.get('src', '').endswith(':doorkey'))
        self.assertEqual(key['rect'][0] + key['rect'][2], generator.W - 12)
        self.assertEqual(generator.L_LOCK_X, 12)

    # ---- A6: the map rim follows the module
    def test_a6_rim_lit_by_the_module(self):
        frame = next(w for w in self.pages['ready'] if w.get('src', '').endswith(':frame'))
        self.assertEqual(frame['bind'], 'sshd.stab.lit')
        names = {d['name'] for d in self.manifest['derived']}
        self.assertFalse(any(n.startswith('stab.') for n in names))
        self.assertNotIn('sshd.stab.press', json.dumps(self.manifest))
        self.assertIn('inst->stab_at = ClockMs(inst);', self.cpp)
        self.assertIn('"sshd.stab.lit", inst->stab_at >= 0 && ms - inst->stab_at < hold ? 1 : 0', self.cpp)

    # ---- behaviour-neutral fixes
    def test_neutral_fixes(self):
        # a refused stroke drops its vector; the stab's reasons; stab and bash exclude each other
        self.assertIn('e.store_u32(e.userdata, kOffTtl, 0);', self.cpp[self.cpp.index('void StickMirror(void* instance, const EdenDsmodHostApi* host) {'):])
        self.assertIn('kWhyNotDrawn = 19', self.cpp)
        self.assertIn('kWhySwim = 20', self.cpp)
        self.assertIn('"sword not drawn",', self.cpp)
        stab = self.cpp[self.cpp.index('bool SwordStab('):self.cpp.index('// The native stamina gauge')]
        self.assertIn('kOffBash, &bash) || bash != 0', stab)
        bash = self.cpp[self.cpp.index('bool ShieldBash('):self.cpp.index('// Link in water')]
        self.assertIn('kOffStab, &left) || left != 0', bash)
        # the mailbox offsets in one place
        block = self.cpp[self.cpp.index('// Right-stick mirror mailbox'):self.cpp.index('constexpr int kSwipeFlash')]
        for name in ('kOffBash = 0x18', 'kOffPutAway = 0x1C', 'kOffLHold = 0x20', 'kOffStab = 0x24'):
            self.assertIn(name, block)
            self.assertEqual(self.cpp.count('constexpr uint32_t ' + name), 1)
        # the top-HUD hide reads no stack slot; the legacy replay takes the module's menu state
        self.assertNotIn('0x192D0E4', self.hud)
        self.assertNotIn('kMenuFlag', self.hud)
        self.assertNotIn('bool menu = false;  //', self.hudh)
        self.assertIn('inst->hide.Step(show_dpad, show_rupee, helper_ok, t, legacy, inst->menu_open)', self.cpp)

    def test_no_process_labels_or_private_folders(self):
        files = [self.root / 'native/modules' / f for f in ('01002DA013484000.cpp', 'sshd_art.cpp', 'sshd_hudhide.cpp')]
        files += [self.root / 'tools/skyward-sword-hd/gen_manifest.py', self.root / 'native/helper/sshd_stick_helper.S']
        for f in files:
            text = f.read_text()
            self.assertNotRegex(text, r'steward S\d|hardening [HM]\d|audit R\d|owner design call|20261115-ui-polish', f.name)

if __name__ == '__main__':
    unittest.main()
