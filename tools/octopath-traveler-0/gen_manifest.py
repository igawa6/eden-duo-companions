#!/usr/bin/env python3
"""Generate the native exploration, battle and settings companion (Switch 1.0.5)."""
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = '9FE60A212CFF9B9A3656AB63D4F882E500000000000000000000000000000000'


def map_areas():
    ring = dict(group='ot0_rings', count=8, x='ot0.rg{i}.x', y='ot0.rg{i}.y', kind='ot0.rg{i}.on', hide_when_kind=0,
                tint_bind='ot0.rg{i}.tint', size=212,
                frames=[f'module:ot0:ringf/{k}' for k in range(24)], frame_ms=50)
    marks = dict(group='ot0_icons', count=32, x='ot0.mk{i}.x', y='ot0.mk{i}.y', kind='ot0.mk{i}.on', hide_when_kind=0,
                 icon_src_bind='ot0.mk{i}.src', size=96)
    player = dict(group='ot0_player', count=1, x='ot0.pl.x', y='ot0.pl.y', kind='ot0.pl.on', hide_when_kind=0,
                  icon_src_bind='ot0.pl.src', size=56)
    return dict(areas=dict(ot0map=dict(min=[0, 0], max=[704, 772], clamp_view=True, no_pin=True,
                                       dynamic_markers=[ring, marks, player])))


def manifest():
    widgets = [dict(type='image', rect=[0, 0, 1240, 1080], src_bind='ot0.screen',
                    color='#FFFFFFFF', need_bind='!module_error')]
    # Native glyphs reuse fixed cached textures; animate alpha without decoding
    # or rasterizing the full screen. Metadata comes from the exact current frame.
    actions = {}
    bp_colors = [f'#{int(90 + i * 11):02X}FFFFFF' for i in range(16)]  # alpha ramp; pip art keeps its colour
    # Map pane: a runtime map widget over the module's revealed-map picture. Its world is the
    # picture (704 x 772, y up); the runtime keeps every marker on its world point while the
    # view is pinched/dragged. The bound view rect is the home view (fixed native scale around
    # the player); the corner button resets a moved view to it.
    widgets.append(dict(type='map', id='ot0_map', rect=[44, 122, 704, 772], area='ot0map',
                        image_bind='ot0.map.pic', pan_zoom=True, min_zoom=1, max_zoom=8,
                        view_rect_x0_bind='ot0.map.v.x0', view_rect_y0_bind='ot0.map.v.y0',
                        view_rect_x1_bind='ot0.map.v.x1', view_rect_y1_bind='ot0.map.v.y1',
                        area_label=False, need_bind='ot0.map.on'))
    # Fetches the next map picture off-screen (1 px, near-transparent) so the map widget can
    # switch to it only once decoded: no blank frame while walking.
    widgets.append(dict(type='image', rect=[0, 0, 1, 1], src_bind='ot0.map.next', color='#01FFFFFF',
                        need_bind='ot0.map.on'))
    actions['map_reset'] = dict(kind='view_reset', view='ot0_map')
    widgets.append(dict(type='rect', id='map_reset', rect=[628, 774, 112, 112], bg='#C0101418',
                        color='#80D6D0C4', frame=2, on_tap='map_reset', need_bind='ot0.map.on'))
    widgets.append(dict(type='image', rect=[662, 808, 44, 44], src='module:ot0:glyph/player/ffffffff',
                        need_bind='ot0.map.on'))
    # Battle BP pulses only (native BP disc glyph, alpha-animated).
    for i in range(45):
        widgets.append(dict(type='image', rect=[0,0,64,64],
            src_bind=f'ot0.fx{i}.src',
            x_bind=f'ot0.fx{i}.x', y_bind=f'ot0.fx{i}.y',
            scale_bind=f'ot0.fx{i}.scale', tint_bind='ot0.pulse',
            tint_colors=bp_colors, need_bind=f'ot0.fx{i}.bp'))
    # Keep failed image keys eligible for the host's bounded asset retries.
    # The later fallback overlay covers them until a successful decode clears the error.
    for slot in range(8):
        action = f'select_{slot}'
        actions[action] = dict(kind='module', action='select', argument=slot)
        widgets.append(dict(type='rect', id=f'member_{slot}', rect=[0, 0, 312, 160],
                            x_bind=f'ot0.p{slot}.x', y_bind=f'ot0.p{slot}.y',
                            bg='#00000000', color='#00000000', frame=0,
                            on_tap=action, need_bind=f'ot0.p{slot}.on'))
        widgets.append(dict(type='rect',id=f'map_member_{slot}',
                            rect=[792+(216 if slot>=4 else 0),0,216,202],
                            y_bind=f'ot0.mp{slot}.y',bg='#00000000',color='#00000000',frame=0,
                            on_tap=action,need_bind=f'ot0.mp{slot}.on'))
    # Battle page: tapping a compact foe row focuses that foe (companion-side only).
    for row in range(5):
        action = f'focus_foe_{row}'
        actions[action] = dict(kind='module', action='foe', argument=row)  # rt22 module args: int or $payload only
        widgets.append(dict(type='rect', id=f'foe_{row}', rect=[12, 0, 576, 130], y_bind=f'ot0.foe{row}.y',
                            bg='#00000000', color='#00000000', frame=0,
                            on_tap=action, need_bind=f'ot0.foe{row}.on'))
    actions['detail_mode'] = dict(kind='module', action='detail_mode', argument=0)
    widgets.append(dict(type='rect', id='detail_mode', rect=[864,16,232,112],
                        bg='#00000000', color='#00000000', frame=0,
                        on_tap='detail_mode', need_bind='ot0.combat'))
    # Party page sub-tabs (drawn by the module): Stats | Equipment.
    for index, name in enumerate(['stats', 'equipment']):
        actions[f'party_{name}'] = dict(kind='module', action='party_view', argument=index)
        widgets.append(dict(type='rect', id=f'party_{name}', rect=[16 + 256 * index, 0, 244, 112],
                            bg='#00000000', color='#00000000', frame=0,
                            on_tap=f'party_{name}', need_bind='ot0.party_page'))
    actions['page_map'] = dict(kind='module', action='page', argument=0)
    actions['page_party'] = dict(kind='module', action='page', argument=1)
    # Top-screen party HUD: the settings page flag is the switch on hosts with live "@flag:"
    # reads (Eden Duo 1.3.2+). Older hosts keep their native HUD visible; these actions
    # are retained as diagnostic preferences only.
    actions['hud_hide_on'] = dict(kind='module', action='hud_hide', argument=1)
    actions['hud_hide_off'] = dict(kind='module', action='hud_hide', argument=0)
    actions['open_settings'] = dict(kind='page', page='@settings', transition='fade', duration_ms=150)
    # Footer: Map | Party | Items | Quests (no local map: Party | Items | Quests); rectangles mirror the module's.
    actions['page_items'] = dict(kind='module', action='page', argument=2)
    actions['page_quests'] = dict(kind='module', action='page', argument=3)
    for index, page in enumerate(['map', 'party', 'items', 'quests']):
        widgets.append(dict(type='rect', id=f'tab_{page}', rect=[16 + 274 * index, 956, 262, 112],
                            bg='#00000000', color='#00000000', frame=0,
                            on_tap=f'page_{page}', need_bind='ot0.standard_map_tab'))
    for index, page in enumerate(['party', 'items', 'quests']):
        widgets.append(dict(type='rect', id=f'tab_{page}_nomap', rect=[16 + 364 * index, 956, 352, 112],
                            bg='#00000000', color='#00000000', frame=0,
                            on_tap=f'page_{page}', need_bind='ot0.nomap_tabs'))
    # Quests page: one module image per row; the runtime scrolls them
    # (drag/fling, clipped to the list area, scroll bar), back to the top when the list changes.
    widgets.append(dict(type='image', rect=[44, 214, 1152, 128], src_bind='ot0.quests.row{i}',
                        need_bind='ot0.quests_page', repeat=64, repeat_bind='ot0.quests.count',
                        repeat_dy=128, scroll='quests'))
    # Items page: the native category tab column and the list's page buttons.
    for index in range(12):
        actions[f'items_tab_{index}'] = dict(kind='module', action='items_tab', argument=index)
        widgets.append(dict(type='rect', id=f'items_tab_{index}', rect=[22 + 124 * (index % 2), 106 + 124 * (index // 2), 112, 112],
                            bg='#00000000', color='#00000000', frame=0,
                            on_tap=f'items_tab_{index}', need_bind='ot0.items_page'))
    for index, (name, rect) in enumerate([('prev', [860, 0, 112, 112]), ('next', [1092, 0, 132, 112])]):
        actions[f'items_{name}'] = dict(kind='module', action='items_page_step', argument=index)
        widgets.append(dict(type='rect', id=f'items_{name}', rect=rect, bg='#00000000', color='#00000000',
                            frame=0, on_tap=f'items_{name}', need_bind='ot0.items_page'))
    for action in ['heal','heal_apply','heal_cancel']:
        actions[action]=dict(kind='module',action=action,argument=0)
    for name,rect,action,bind in [
        ('heal_entry',[636,0,140,112],'heal','ot0.heal_header'),
        ('heal_cancel',[44,782,328,112],'heal_cancel','ot0.heal_open'),
        ('heal_confirm',[400,782,348,112],'heal_apply','ot0.heal_confirm')]:
        widgets.append(dict(type='rect',id=name,rect=rect,bg='#00000000',color='#00000000',
                            frame=0,on_tap=action,need_bind=bind))
    # Footer settings square (drawn by the module with the native Miscellaneous glyph): the host
    # settings page uses the same native materials and fonts as the companion.
    widgets.append(dict(type='rect', id='tab_settings', rect=[1112,956,112,112],
                        bg='#00000000', color='#00000000', frame=0,
                        on_tap='open_settings', need_bind='ot0.party_tab'))
    widgets.append(dict(type='rect', id='battle_settings', rect=[1112,16,112,112],
                        bg='#00000000', color='#00000000', frame=0,
                        on_tap='open_settings', need_bind='ot0.combat'))
    # All fallback pixels/text are host-authored, before any game font or asset decoder.
    gate = 'ot0.fallback'
    widgets.append(dict(type='rect', rect=[0, 0, 1240, 1080], bg='#FF14181D',
                        color='#FF414953', frame=4, need_bind=gate))
    for y, text, scale, color in [
        (170, 'COMPANION UNAVAILABLE', 8, '#FFFFFFFF'),
        (320, 'OCTOPATH TRAVELER 0', 6, '#FFD6D0C4'),
        (440, 'SUPPORTED UPDATE: 1.0.5', 5, '#FFFFFFFF'),
        (510, 'BUILD: 9FE60A212CFF9B9A', 5, '#FFD6D0C4'),
        (680, 'USE THE SUPPORTED GAME UPDATE', 5, '#FFFFFFFF'),
        (750, 'COMPANION VIEW IS UNAVAILABLE', 5, '#FFD6D0C4'),
    ]:
        widgets.append(dict(type='label', rect=[620, y, 0, 0], text=text,
                            text_scale=scale, align='center', color=color, need_bind=gate))
    widgets.append(dict(type='label', rect=[120, 870, 1000, 120], bind_text='module_error_message',
                        text_scale=4, color='#FFD6D0C4', wrap_width=1000,
                        max_lines=3, need_bind='module_error'))
    derived = [dict(name=gate, expr='module_error == 1 || ot0.asset_error == 1'),
               dict(name='ot0.map_tab', expr='ot0.map_available == 1 && ot0.battle != 1'),
               dict(name='ot0.party_tab', expr='ot0.ready == 1 && ot0.battle != 1'),
               dict(name='ot0.standard_map_tab',expr='ot0.map_tab == 1'),
               dict(name='ot0.nomap_tabs',expr='ot0.party_tab == 1 && ot0.map_tab != 1'),
               dict(name='ot0.heal_header',expr='ot0.heal_available == 1 && ot0.map_page == 1')]
    for i in range(45):
        derived.append(dict(name=f'ot0.fx{i}.bp',
            expr=f"'ot0.fx{i}.on' == 1 && 'ot0.fx{i}.player' == 0 && "
                 "'ot0.fallback' != 1 && '@flag:ot0.reduced_motion' != 1"))
    # The host page protocol and persisted flag names remain unchanged. Original
    # game fonts/materials replace the built-in placeholder settings artwork.
    flags = {'ot0.hide_party_hud': 1, 'ot0.haptics': 1, 'ot0.reduced_motion': 0}
    settings_widgets = []
    for bits in range(8):
        values = [(bits >> i) & 1 for i in range(3)]
        name = f'ot0.settings_{bits}'
        derived.append(dict(name=name, expr=' && '.join(
            f"'@flag:{flag}' == {value}" for flag, value in zip(flags, values))))
        settings_widgets.append(dict(type='image', rect=[0,0,1240,1080],
            src='module:ot0:settings/' + ''.join(map(str, values)), need_bind=name))
    for i, flag in enumerate(flags):
        action = '@settings:' + flag
        actions[action] = dict(kind='flag', flag=flag)
        settings_widgets.append(dict(type='rect', id=f'setting_{i}', rect=[48,228+i*190,1144,148],
            bg='#00000000', color='#00000000', frame=0, on_tap=action))
    actions['settings_back'] = dict(kind='page', page='@back', transition='fade', duration_ms=150)
    settings_widgets.append(dict(type='rect', id='settings_back', rect=[16,956,1208,112],
        bg='#00000000', color='#00000000', frame=0, on_tap='settings_back'))
    # Every page needs a host-rendered fallback if native fonts/art cannot load.
    settings_widgets.extend(copy.deepcopy(widget) for widget in widgets
                            if widget.get('need_bind') in (gate, 'module_error'))
    for action in ('open_settings', 'settings_back'):
        actions[action + '_still'] = {**actions[action], 'transition':'none', 'duration_ms':0}

    def feedback_controls(items):
        out = []
        for widget in items:
            if 'on_tap' not in widget:
                out.append(widget)
                continue
            action = widget['on_tap']
            motion_modes = (False, True) if action in ('open_settings', 'settings_back') else (None,)
            for reduced in motion_modes:
                for feedback in (False, True):
                    entry = copy.deepcopy(widget)
                    condition = []
                    condition.append("'ot0.fallback' != 1")
                    if 'need_bind' in widget:
                        condition.append(f"'{widget['need_bind']}' != 0")
                    condition.append(f"'@flag:ot0.haptics' {'==' if feedback else '!='} 1")
                    if reduced is not None:
                        condition.append(f"'@flag:ot0.reduced_motion' {'==' if reduced else '!='} 1")
                    name = f'ot0.touch_{len(derived)}'
                    derived.append(dict(name=name, expr=' && '.join(condition)))
                    # MOTION-AND-HAPTICS: a confirmed native write (Quick Heal apply) gets the firmer
                    # confirm feedback; every other touch stays light.
                    strength = 'confirm' if action == 'heal_apply' else 'light'
                    entry.update(id=name, need_bind=name, haptic=strength if feedback else 'off')
                    if reduced:
                        entry['on_tap'] = action + '_still'
                    out.append(entry)
        return out

    widgets = feedback_controls(widgets)
    settings_widgets = feedback_controls(settings_widgets)
    return dict(format=1, title_id='01005270232F2000', name='Octopath Traveler 0 companion',
                min_runtime=22, nav=False, requires_module=True,
                module=dict(abi=1, build_ids=[BUILD]), load_plan='guest/<build>/load-plan.json', canvas_w=1240, canvas_h=1080,
                background='#FF000000',
                derived=derived, flags=flags, persist_flags=list(flags),
                anim_hz=60,
                haptics=dict(enabled=True, respect_system=True, tap='off', write='off',
                             select='off', refused='reject', hold='off', swipe='off', nav='off'),
                # Owner rule 2026-10-25: native HUD moved to the bottom screen is hidden on top by
                # default; this setting shows it again. Restored whenever the companion is hidden.
                module_tick_hidden=True,
                map=map_areas(), actions=actions, pages=[dict(id='party', title='Party', widgets=widgets,
                                            scrolls=[dict(id='quests', rect=[44, 214, 1152, 700],
                                                          content_h_bind='ot0.quests.h', show_bind='ot0.quests_page',
                                                          reset_bind='ot0.quests.gen', bar='#B0D6D0C4',
                                                          bar_track='#30000000', bar_w=6)]),
                                       dict(id='@settings', title='Settings', no_auto_leave=True,
                                            widgets=settings_widgets)],
                _about='Generated by tools/octopath-traveler-0/gen_manifest.py. '
                       'Native assets/fonts decoded from owned romfs; no game data shipped. '
                       'Current-section revealed minimap, native protagonist parts, adaptive Formation, battle page (live resources, native computed attributes/statuses, Auto/Overview selection details, revealed weaknesses, shields, BP) and lifecycle; native equipment-shop comparisons, computed resource gauges and native settings. '
                       'Direct field Healing Skills use native learned/target/cost rules and consume SP once, with bottom-only confirmation. '
                       'Optional moved top-screen minimap/party HUD hide (setting, default on, full visible-marker readiness required). No hidden weaknesses or markers.')


if __name__ == '__main__':
    path = ROOT / 'packages/OctopathTraveler0/dualscreen/manifest.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest(), indent=2) + '\n')
    print(path)
