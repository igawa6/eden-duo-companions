#!/usr/bin/env python3
"""Regression checks for repeated Party move gates and multi-battle roster capacity."""
import unittest
import re
import subprocess
import sys
from pathlib import Path

import gen_manifest as gen


class ManifestTests(unittest.TestCase):
    def test_battle_target_controls_and_gender(self):
        for action in ('targetconfirm', 'targetback'):
            self.assertEqual(self.manifest['actions'][action]['enabled_bind'], 'lp.bt.target.input')
            self.assertTrue(any(w.get('on_tap') == action for w in self.widgets))
        for index in range(4):
            self.assertTrue(any(w.get('src_bind') == f'lp.bt.fld.active{index}.gendericon'
                                for w in self.widgets))
        self.assertIn('lp.bt.menu.opening != 1', self.derived['lp.bt.busy.dim'])

    def test_dex_mode_toggle_and_dynamic_title(self):
        self.assertEqual(self.manifest['actions']['dexmode']['action'], 'dex_mode')
        titles = [w for w in self.widgets if w.get('bind_text') == 'lp.dex.title']
        self.assertEqual(len(titles), 1)
        self.assertEqual(titles[0]['on_tap'], 'dexmode')

    def test_battle_active_positions_and_volatile_sections(self):
        def depends_on(gate, target):
            if gate == target:
                return True
            return any(depends_on(token, target)
                       for token in re.findall(r"[A-Za-z0-9_.]+", self.derived.get(gate, ''))
                       if token in self.derived or token == target)
        for index in range(4):
            rows = [w for w in self.widgets if w.get('src_bind') == f'lp.bt.fld.active{index}.icon']
            self.assertTrue(rows)
            # The pane is now also gated on lp.cfg.pane (shown/hidden); the original gate must
            # still be an ingredient of whatever combined need_bind the card ends up with.
            self.assertTrue(all(depends_on(w.get('need_bind', ''), f'lp.bt.fld.active{index}.on')
                                for w in rows))
        for side in ('own', 'foe'):
            rows = [w for w in self.widgets if w.get('bind_text') == f'lp.bt.{side}.effects']
            self.assertTrue(rows)
            self.assertTrue(all(w.get('wrap_width', 0) > 0 and w.get('max_lines', 0) >= 26 for w in rows))

    @classmethod
    def setUpClass(cls):
        cls.manifest = gen.manifest()
        cls.widgets = [w for page in cls.manifest['pages'] for w in page['widgets']]
        cls.derived = {d['name']: d['expr'] for d in cls.manifest['derived']}

    def test_party_card_drag_and_move_popup(self):
        field = next(page for page in self.manifest['pages'] if page['id'] == 'field')
        for index in range(6):
            taps = [w for w in field['widgets'] if w.get('on_tap') == f'sel{index}']
            self.assertEqual(len(taps), 1)
            self.assertTrue(taps[0]['draggable'])
            # Either group would re-enable runtime tap-then-tap drop handling.
            self.assertFalse(taps[0].get('select_group'))
            self.assertFalse(taps[0].get('accept_group'))
            self.assertEqual(taps[0]['payload'], str(index))
            self.assertEqual(self.manifest['actions'][f'sel{index}']['action'], 'select')
            self.assertEqual(self.manifest['actions'][f'sel{index}']['argument'], index)
            self.assertEqual(taps[0]['rect'][2:], [550, 132])
            self.assertEqual(taps[0]['drop_action'], f'swap{index}')
            self.assertIn('swap', taps[0]['x_bind'])
            self.assertIn('swap', taps[0]['y_bind'])
        move = next(w for w in field['widgets'] if w.get('on_tap') == 'partymove')
        self.assertTrue(move['draggable'])
        self.assertEqual(move['drop_action'], 'moveswap{i}')
        self.assertEqual(move['payload'], '$lp.sel.m{i}.payload')
        self.assertFalse(move.get('select_group'))
        self.assertFalse(move.get('accept_group'))
        for index in range(4):
            self.assertEqual(self.manifest['actions'][f'moveswap{index}']['enabled_bind'], 'lp.fld.ok')
        popup_pp = next(w for w in field['widgets'] if w.get('bind') == 'lp.party.move.pp')
        self.assertEqual(popup_pp['max_bind'], 'lp.party.move.ppmax')
        self.assertEqual(popup_pp['max_sep'], '/')
        for theme in (0, 1):
            expression = self.derived['lp.party.move.wz'].replace('lp.party.move.type', '4').replace('@flag:platinum_theme', str(theme))
            self.assertEqual(eval(expression, {'__builtins__': {}}), 4 + 100 * theme)
        original_pp = next(w for w in field['widgets'] if w.get('bind') == 'lp.sel.m{i}.pp')
        self.assertEqual(popup_pp['text_scale'], original_pp['text_scale'])
        self.assertTrue(any(w.get('input_block') and w.get('need_bind') == 'lp.party.move.on'
                            for w in field['widgets']))

    def test_party_swap_layers_share_pose_and_preserve_sprite_hop(self):
        field = next(page for page in self.manifest['pages'] if page['id'] == 'field')
        for index in range(6):
            layers = [w for w in field['widgets'] if w.get('x_bind') == f'lp.an.swap{index}.x']
            self.assertGreater(len(layers), 20)  # full plate, text, HP and touch target
            sprites = [w for w in layers if w.get('src_bind') == f'lp.p{index}.icon']
            self.assertEqual(len(sprites), 1)
            for layer in layers:
                expected = f'lp.an.swap{index}_sprite.y' if layer is sprites[0] else f'lp.an.swap{index}.y'
                self.assertEqual(layer['y_bind'], expected)
            self.assertEqual(self.derived[f'lp.an.swap{index}_sprite.y'],
                             f'@flag:lp_anim ? lp.am.swap{index}.y + lp.an.pl{index}.y : 0')
        poses = [name for name in self.derived if name.startswith('lp.an.swap')]
        self.assertEqual(len(poses), 18)  # x/y per card, plus one composed sprite y per card

    def test_all_party_plate_layers_require_a_valid_slot(self):
        # Existing selection/health gates must not bypass the outer slot-valid gate.
        # In particular lp.selN.on only compares the selected index, not party count.
        def depends_on(gate, target):
            if gate == target:
                return True
            return any(depends_on(token, target)
                       for token in re.findall(r"[A-Za-z0-9_.]+", self.derived.get(gate, ''))
                       if token in self.derived or token == target)

        field = next(page for page in self.manifest['pages'] if page['id'] == 'field')
        for index in range(6):
            layers = [w for w in field['widgets'] if w.get('x_bind') == f'lp.an.swap{index}.x']
            self.assertTrue(layers)
            self.assertTrue(all(depends_on(w.get('need_bind', ''), f'lp.p{index}.valid')
                                for w in layers))
        # Repeated acquisition gates need no redundant derived aliases.
        self.assertNotIn('lp.poketch.acquired.with.lp.poketch.acquired', self.derived)

    def test_generation_is_repeatable_without_accumulating_fit_rows(self):
        first = gen.manifest()
        counts = (len(gen.FITS), len(gen.CATALOG_FITS))
        self.assertEqual(gen.manifest(), first)
        self.assertEqual((len(gen.FITS), len(gen.CATALOG_FITS)), counts)

    def test_optimized_python_preserves_all_generated_registries(self):
        # Assertions disappear under -O; registration must remain functional for real builds.
        script = ('import gen_manifest as g; g.manifest(); '
                  'print(len(g.ANIM_DERIVED), len(g.WIDTHS), len(g.SUMS), '
                  'len(g.FITS), len(g.CATALOG_FITS))')
        cwd = Path(gen.__file__).parent
        normal = subprocess.check_output([sys.executable, '-c', script], cwd=cwd)
        optimized = subprocess.check_output([sys.executable, '-O', '-c', script], cwd=cwd)
        self.assertEqual(optimized, normal)
        self.assertTrue(all(int(count) > 0 for count in normal.split()))

    def test_party_moves_have_concrete_visibility_inputs(self):
        # The runtime expands widget templates, but derived expressions are global.
        # A literal {i} there silently hides every populated move row.
        for name, expr in self.derived.items():
            self.assertNotIn('{i}', name)
            self.assertNotIn('{i}', expr)
        rows = [w for w in self.widgets if w.get('bind_text') == 'lp.sel.m{i}.name']
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row['repeat'], 4)
        for index in range(4):
            gate = row['need_bind'].replace('{i}', str(index))
            self.assertEqual(self.derived[gate],
                             f'lp.party.selected == 1 && (lp.sel.details.with.lp.sel.m{index}.id) != 0')
            self.assertEqual(self.derived[f'lp.sel.details.with.lp.sel.m{index}.id'],
                             f'lp.sel.details == 1 && (lp.sel.m{index}.id) != 0')
        self.assertEqual(row['hide_bind'], 'lp.ftab1.on')

    def test_egg_summary_and_health_details_are_hidden(self):
        def depends_on(gate, target):
            if gate == target:
                return True
            expr = self.derived.get(gate, '')
            return any(depends_on(token, target) for token in expr.replace('(', ' ').replace(')', ' ').split()
                       if token in self.derived or token == target)

        sensitive = {'lp.sel.level', 'lp.sel.hpmax', 'lp.sel.atk', 'lp.sel.def', 'lp.sel.spa',
                     'lp.sel.spd', 'lp.sel.spe', 'lp.sel.ability', 'lp.sel.ability.desc', 'lp.sel.status',
                     'lp.sel.m{i}.name'}
        rows = [w for w in self.widgets if w.get('bind', w.get('bind_text')) in sensitive]
        self.assertTrue(rows)
        for row in rows:
            gate = row.get('need_bind', '').replace('{i}', '0')
            self.assertTrue(depends_on(gate, 'lp.sel.details'), row)
        for index in range(6):
            rows = [w for w in self.widgets if w.get('bind') == f'lp.p{index}.hp']
            self.assertTrue(rows)
            for row in rows:
                self.assertTrue(depends_on(row.get('need_bind', ''), f'lp.p{index}.details'), row)

    def test_map_cloud_stays_anchored_and_hides_offscreen(self):
        values = {'lp.map.sel.wx': 600, 'lp.map.sel.wy': 400, 'lp.map.sel.half': 100,
                  'lp.map.sel.top': 0, 'lp.map.sel.vis': 1, '@map_view_x:townmap': 36, '@map_view_y:townmap': 92,
                  '@map_view_w:townmap': 1168, '@map_view_h:townmap': 756}
        def project(cx, cy, ppw, top=0):
            values.update({'@map_view_cx:townmap': cx, '@map_view_cy:townmap': cy,
                           '@map_view_ppw:townmap': ppw, 'lp.map.sel.top': top})
            for name in ['lp.map.sel.x', 'lp.map.sel.y', 'lp.map.cloud.on']:
                expr = re.sub(r'@[A-Za-z0-9_.:]+|lp\.[A-Za-z0-9_.]+',
                              lambda match: repr(values[match.group()]), self.derived[name])
                values[name] = eval(expr.replace('&&', ' and '), {'__builtins__': {}})
            return values['lp.map.sel.x'], values['lp.map.sel.y'], values['lp.map.cloud.on']
        self.assertEqual(project(600, 400, 1), (620, 470, True))
        self.assertEqual(project(550, 420, 2), (720, 510, True))
        self.assertEqual(project(10000, -10000, 3), (-27580, -30730, False))
        self.assertEqual(project(-10000, 10000, 3), (32420, 29270, False))
        # A point still in the map can have its cloud obscured by the objective banner.
        self.assertFalse(project(600, 150, 1, top=104)[2])
        self.assertTrue(project(600, 400, 1)[2])  # selection reappears when panned back
        cloud = [w for w in self.widgets if w.get('x_bind') == 'lp.map.sel.x']
        self.assertTrue(cloud)
        self.assertTrue(all(w.get('need_bind') == 'lp.map.cloud.on' for w in cloud))

    def test_map_guidance_uses_compact_centered_native_font_layouts(self):
        labels = [w for w in self.widgets if w.get('bind_text') == 'lp.map.goal.message']
        self.assertEqual({w['text_scale'] for w in labels}, {4, 5})
        for row in labels:
            self.assertEqual(row['wrap_width'], 1048)
            self.assertEqual(row['max_lines'], 3)
            self.assertEqual(row['y_bind'], 'lp.map.goal.text.y')
            self.assertFalse(row.get('fit_text', False))
        for height in (64, 76, 89, 108, 129):
            gate = f'lp.map.goal.message.on.with.lp.map.goal.height{height}'
            self.assertIn(gate, self.derived)
            self.assertTrue(any(w.get('need_bind') == gate for w in self.widgets))
        self.assertTrue(any('lp.map.sel.top' in expr for expr in self.derived.values()))

    def test_bag_command_keeps_native_icon_for_both_players_and_themes(self):
        rows = [w for w in self.widgets if w.get('bind') == 'lp.bt.bag.style']
        # 9 regions of the nine-sliced Bag body, twice over: the small command stack (pane shown)
        # and the pane-hidden curve's Bag button (owner mockup b) share the same art logic.
        self.assertEqual(len(rows), 18)
        expected = ['module:lp:ui/sharedui/btl_bt_command_01_body_03_02',
                    'module:lp:ui/sharedui/btl_bt_command_01_body_03_01']
        expected += [name.replace('module:lp:', 'module:lp:classic/', 1) for name in expected]
        for row in rows:
            self.assertEqual(row['src_names'], expected)
            self.assertNotIn('src', row)
        expression = self.derived['lp.bt.bag.style']
        for male in (0, 1):
            for theme in (0, 1):
                index = eval(expression.replace('lp.player.sex', str(male))
                             .replace('@flag:platinum_theme', str(theme)), {'__builtins__': {}})
                self.assertTrue(expected[index].endswith('_01' if male else '_02'))
                self.assertEqual(':classic/' in expected[index], bool(theme))
        self.assertFalse(any(w.get('src', '').endswith('btl_bt_command_01_body_03') for w in self.widgets))

    def test_both_opposing_parties_fit_the_scrolled_roster(self):
        rows = [w for w in self.widgets if w.get('bind_text') == 'lp.bt.ep{i}.name']
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row['repeat'], 12)
            self.assertEqual(row['repeat_bind'], 'lp.bt.ep.rows')
            self.assertTrue(row['scroll'])

    # ---- battle pane hide/restore (owner-approved 2026-11-02, battle-pane-hide-mockups) ---------

    def test_pane_hide_flag_setting_and_actions_match_owner_decisions(self):
        # Manifest-only gate: module need not read it (same pattern as platinum_theme).
        self.assertEqual(self.manifest['flags'].get('lp_pane_hidden'), 0)
        setting = next(s for s in self.manifest['settings'] if s['flag'] == 'lp_pane_hidden')
        self.assertEqual(setting['type'], 'toggle')
        self.assertEqual(setting['default'], 0)
        self.assertEqual(len(setting.get('choices', [])), 2)  # "on = pane shown", via custom labels
        self.assertEqual(self.manifest['actions']['pane_hide'],
                         {'kind': 'flag', 'flag': 'lp_pane_hidden', 'value': 1, 'haptic': 'click'})
        self.assertEqual(self.manifest['actions']['pane_show'],
                         {'kind': 'flag', 'flag': 'lp_pane_hidden', 'value': 0, 'haptic': 'click'})
        # No new pages/page_binds: a manifest-only gate inside the existing battle/moves pages.
        self.assertEqual({p['id'] for p in self.manifest['pages']},
                         {'wait', 'field', 'map', 'route', 'poketch', 'poketch_full',
                          'pokedex', 'field_bag', 'battle', 'moves', 'balls'})

    def test_pane_hidden_state_never_true_in_a_double(self):
        self.assertEqual(self.derived['lp.cfg.pane'], '@flag:lp_pane_hidden == 0 || lp.bt.dbl == 1')
        self.assertEqual(self.derived['lp.bt.pane_arrow'], 'lp.cfg.pane == 1 && lp.bt.sgl == 1')
        for flag, dbl, pane_shown in ((0, 0, True), (1, 0, False), (0, 1, True), (1, 1, True)):
            expr = (self.derived['lp.cfg.pane'].replace('@flag:lp_pane_hidden', str(flag))
                    .replace('lp.bt.dbl', str(dbl)).replace('||', ' or ').replace('&&', ' and '))
            self.assertEqual(eval(expr, {'__builtins__': {}}), pane_shown, (flag, dbl))
            arrow_expr = (self.derived['lp.bt.pane_arrow']
                          .replace('lp.cfg.pane', '1' if pane_shown else '0').replace('lp.bt.sgl', str(1 - dbl))
                          .replace('&&', ' and '))
            arrow_shown = eval(arrow_expr, {'__builtins__': {}})
            # the fold arrow (hide control) only appears on a shown single-battle card
            self.assertEqual(bool(arrow_shown), pane_shown and dbl == 0, (flag, dbl))

    def test_pane_hidden_big_layout_reuses_todays_command_slots(self):
        # owner mockup b (Direction A): one big Battle button, Bag / Pokémon on a curve, Run
        # lowest and centred -- the exact command slots/binds/actions the small stack already uses.
        battle = next(p for p in self.manifest['pages'] if p['id'] == 'battle')
        for k, rect, action in ((0, gen.HIDDEN_BIG, 'go_moves'), (2, gen.HIDDEN_LEFT, 'cmd_bag'),
                                (1, gen.HIDDEN_RIGHT, 'cmd_pokemon'), (3, gen.HIDDEN_RUN, 'cmd_run')):
            taps = [w for w in battle['widgets'] if w.get('on_tap') == action and w.get('rect') == rect]
            self.assertTrue(taps, (action, rect))
            for w in taps:
                self.assertIn(f'lp.bt.cmd{k}.available', w['need_bind'])
                self.assertTrue(w['need_bind'].startswith('not.lp.cfg.pane'))
            picked = [w for w in battle['widgets']
                     if f'lp.bt.cmd{k}.picked' in (w.get('need_bind') or '') and 'not.lp.cfg.pane' in w['need_bind']]
            self.assertTrue(picked, k)
        cont = [w for w in battle['widgets'] if w.get('on_tap') == 'battlecontinue'
               and w.get('rect', [None, None])[:2] == gen.HIDDEN_CONT[:2]]
        self.assertTrue(cont)
        self.assertTrue(all('not.lp.cfg.pane' in w['need_bind'] for w in cont))
        # the quick-ball X button is unconditional either way (owner: "the X quick-ball button stays")
        ball_taps = [w for w in battle['widgets'] if w.get('on_tap') == 'cmd_ball']
        self.assertTrue(ball_taps)
        self.assertTrue(all(w['need_bind'] == 'lp.bt.ball.any' for w in ball_taps))
        # Regression (found live, harness 2026-11-02): the hidden Continue bar must be drawn after
        # the hidden busy veil, or the veil swallows every tap on it while a message is waiting.
        # battle_dims()'s busy rect has no on_tap, so find the hidden busy_veil(0) input blocker
        # (full width, starts at the body top, hidden while lp.bt.input==1) and require every
        # hidden-state Continue widget to come later in the page's (painter's order) widget list.
        # companions #10 fix (b): the veil's x0 split now follows lp.cfg.pane_card (never true in
        # Safari, since safari_ball_window() replaces the wide card in both pane states), not the
        # raw lp.cfg.pane toggle -- same gate shape, different name.
        veil_indices = [i for i, w in enumerate(battle['widgets'])
                        if w.get('type') == 'rect' and w.get('input_block') and w.get('hide_bind') == 'lp.bt.input'
                        and w.get('rect', [None])[0] == 0 and w.get('need_bind') == '!lp.cfg.pane_card']
        self.assertTrue(veil_indices)
        cont_indices = [battle['widgets'].index(w) for w in cont]
        self.assertTrue(all(ci > vi for ci in cont_indices for vi in veil_indices),
                        "hidden Continue must be drawn after the hidden busy veil")

    def test_pane_fold_arrow_and_folded_tab_on_both_pages(self):
        for page_id, pane_w in (('battle', gen.STRIP_W), ('moves', gen.MOVE_PANE_W)):
            page = next(p for p in self.manifest['pages'] if p['id'] == page_id)
            # companions #10 fix (b): the battle page's whole tabbed card -- including its own
            # fold arrow / folded tab -- is additionally gated off in Safari (safari_ball_window()
            # takes its place in both pane states, page_battle()'s "!lp.bt.safari" wrap on `pane`).
            # The moves page never applies (none of the four Safari commands opens a move list).
            def expect(need_bind, base):
                if page_id == 'battle':
                    self.assertTrue(need_bind.startswith('not.lp.bt.safari.with.'), need_bind)
                    self.assertIn(base.replace('!', 'not.'), need_bind)
                else:
                    self.assertEqual(need_bind, base)

            arrows = [w for w in page['widgets'] if w.get('on_tap') == 'pane_hide']
            self.assertTrue(arrows, page_id)
            for w in arrows:
                expect(w['need_bind'], 'lp.bt.pane_arrow')
            tabs = [w for w in page['widgets'] if w.get('on_tap') == 'pane_show']
            self.assertTrue(tabs, page_id)
            for w in tabs:
                expect(w['need_bind'], '!lp.cfg.pane')
            # owner: "only show arrow" -- the folded tab carries no tab-name label of its own
            self.assertFalse(any(w.get('type') == 'label' and w.get('on_tap') == 'pane_show' for w in page['widgets']))

            # exact icon positions, from the same formula battle_pane() uses -- not just a gate
            # match, since the hidden layout's own (ungated-until-wrapped) Back arrow icon can
            # coincidentally end up on the same "!lp.cfg.pane" gate as the folded tab's icon.
            fold_x, fold_y = 24 + pane_w - gen.FOLD_ARROW_W - 12, gen.BODY_Y - 20
            arrow_rect = [fold_x + (gen.FOLD_ARROW_W - gen.FOLD_ICON_W) // 2,
                         fold_y + (gen.FOLD_ARROW_H - gen.FOLD_ICON_H) // 2, gen.FOLD_ICON_W, gen.FOLD_ICON_H]
            tab_rect = [24 + (gen.FOLD_TAB_W - gen.FOLD_ICON_W) // 2,
                       gen.BODY_Y + (gen.FOLD_TAB_H - gen.FOLD_ICON_H) // 2, gen.FOLD_ICON_W, gen.FOLD_ICON_H]
            arrow_icon = next(w for w in page['widgets']
                              if w.get('rect') == arrow_rect and 'cmn_ico_arrow_01' in w.get('src', ''))
            tab_icon = next(w for w in page['widgets']
                            if w.get('rect') == tab_rect and 'cmn_ico_arrow_01' in w.get('src', ''))
            expect(arrow_icon['need_bind'], 'lp.bt.pane_arrow')
            expect(tab_icon['need_bind'], '!lp.cfg.pane')
            self.assertFalse(arrow_icon.get('flip_x'))   # hide: left-pointing fold arrow (the game's ◀)
            self.assertTrue(tab_icon.get('flip_x'))      # restore: right-pointing

    def test_move_grid_hidden_reuses_todays_move_actions_and_long_press_restores_pane(self):
        moves = next(p for p in self.manifest['pages'] if p['id'] == 'moves')
        for k, (gx, gy) in enumerate(gen.GRID_CELLS):
            taps = [w for w in moves['widgets'] if w.get('on_tap') == f'move{k}'
                   and w.get('rect', [None, None])[:2] == [gx, gy]]
            self.assertTrue(taps, k)
            for w in taps:
                self.assertEqual(w.get('on_hold'), 'pane_show')
                self.assertIn(f'lp.bt.m{k}.id', w['need_bind'])
            picked = [w for w in moves['widgets']
                     if f'lp.bt.m{k}.picked' in (w.get('need_bind') or '') and 'not.lp.cfg.pane' in w['need_bind']]
            self.assertTrue(picked, k)
        back = [w for w in moves['widgets'] if w.get('on_tap') == 'go_battle' and w.get('rect') == gen.HIDDEN_MOVES_BACK]
        self.assertTrue(back)
        self.assertTrue(all(w['need_bind'] == '!lp.cfg.pane' for w in back))

    def test_both_themes_render_the_pane_hidden_bag_button(self):
        # the hidden layout's Bag button (curve-left) must still theme-switch like the small stack's
        battle = next(p for p in self.manifest['pages'] if p['id'] == 'battle')
        rows = [w for w in battle['widgets'] if w.get('bind') == 'lp.bt.bag.style'
               and w.get('rect', [0])[0] == gen.HIDDEN_LEFT[0] and w.get('rect', [0, 0])[1] == gen.HIDDEN_LEFT[1]]
        self.assertTrue(rows)
        for row in rows:
            self.assertNotIn('src', row)
            self.assertEqual(len(row['src_names']), 4)


if __name__ == '__main__':
    unittest.main()
