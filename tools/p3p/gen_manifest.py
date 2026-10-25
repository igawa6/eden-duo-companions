#!/usr/bin/env python3
"""Generate the asset-free read-only P3P preview layout."""
import json
from pathlib import Path
from saves import BUILD

ROOT = Path(__file__).resolve().parents[2]
COLORS = {'ink': '#FFF8EDF3', 'muted': '#FFC0B2C4', 'accent': '#FFFF71AA', 'panel': '#FF282032'}

def label(rect, text='', bind=None, scale=3, ready=False):
    w = {'type': 'label', 'rect': rect, 'text': text, 'color': COLORS['ink'], 'text_scale': scale}
    if bind: w['bind_text'] = bind
    if ready: w['need_bind'] = 'live.ready'
    return w

SKILL_ROWS = 96  # 0x108770 asserts fewer than 0x61 entries

def member_buttons():
    """Member buttons (two rows of five) selecting ui.member; shared by Skills and Equip."""
    widgets = []
    for i in range(10):
        p = f'member.{i}'
        rect = [40 + (i % 5) * 235, 215 + (i // 5) * 52, 225, 44]
        for selected in (0, 1):
            widgets.append({'type': 'button', 'rect': rect, 'text': '', 'on_tap': f'member.select.{i}',
                            'need_bind': p + '.present', 'bg': '#FF8C315B' if selected else COLORS['panel'],
                            'hide_bind': p + '.selected', 'hide_eq': 1 - selected})
        widgets.append(dict(label([rect[0] + 12, rect[1] + 12, rect[2] - 24, 22], bind=p + '.tab', scale=2),
                            need_bind=p + '.present'))
    return widgets

def equip_widgets(outputs):
    """The selected member's four slots with the native Status panel's figures."""
    widgets = member_buttons()
    outputs += ['equip.ready', 'equip.header']
    widgets.append(dict(label([40, 330, 1160, 36], bind='equip.header', scale=3), need_bind='equip.ready'))
    for i, slot in enumerate(('WEAPON', 'BODY', 'FEET', 'ACCESSORY')):  # native Equip labels
        p = f'equip.{i}'; outputs += [p + '.name', p + '.stats']
        y = 390 + i * 110
        widgets.append({'type': 'rect', 'rect': [40, y, 1160, 96], 'bg': COLORS['panel'], 'frame': 0,
                        'need_bind': 'equip.ready'})
        widgets.append(dict(label([60, y + 12, 400, 22], slot, scale=2), need_bind='equip.ready',
                            color=COLORS['accent']))
        widgets.append(dict(label([60, y + 46, 700, 30], bind=p + '.name'), need_bind='equip.ready'))
        widgets.append(dict(label([800, y + 46, 380, 30], bind=p + '.stats'), need_bind='equip.ready')
                       | {'color': COLORS['muted']})
    return widgets

ITEM_ROWS = 256  # the module publishes at most this many rows

def items_widgets(outputs, scrolls):
    """Inventory as the native camp Item tabs list it (Use / Equip / Key), with counts."""
    outputs += ['item.ready', 'item.count', 'item.header', 'item.tab']
    widgets = []
    for i, caption in enumerate(('Use', 'Equip', 'Key')):
        for selected in (0, 1):
            widgets.append({'type': 'button', 'rect': [40 + i * 200, 215, 190, 44], 'text': caption,
                            'on_tap': f'item.tab.{i}', 'text_scale': 2, 'color': COLORS['ink'],
                            'bg': '#FF8C315B' if selected else COLORS['panel'],
                            'hide_bind': 'item.tab', 'keep_min': i if selected else -1,
                            'keep_max': i if selected else 99, **({} if selected else {'hide_eq': i})})
    widgets.append(dict(label([680, 225, 520, 30], bind='item.header', scale=2), need_bind='item.ready'))
    scrolls.append({'id': 'items', 'rect': [40, 280, 1170, 710], 'count_bind': 'item.count', 'row_h': 60,
                    'bar': COLORS['accent'], 'bar_track': '#33FFFFFF', 'bar_w': 6, 'show_bind': 'item.ready',
                    'reset_bind': 'item.tab'})
    outputs += [f'item.{i}.{k}' for i in range(ITEM_ROWS) for k in ('present', 'name', 'count')]
    row = {'repeat': ITEM_ROWS, 'repeat_dy': 60, 'scroll': 'items'}
    widgets.append(dict(row, type='rect', rect=[40, 280, 1150, 52], bg=COLORS['panel'], frame=0,
                        need_bind='item.{i}.present'))
    widgets.append(dict(row, **label([60, 292, 800, 30], bind='item.{i}.name'), need_bind='item.{i}.present'))
    widgets.append(dict(row, **label([1000, 292, 180, 30], bind='item.{i}.count'), need_bind='item.{i}.present')
                   | {'color': COLORS['muted']})
    return widgets

def personas_widgets(outputs):
    """Stock list (tap to pick) and the native PERSONA -> Details panel for the pick."""
    widgets = []
    for i in range(12):
        p = f'persona.{i}'; outputs += [p + '.present', p + '.line', p + '.picked']
        rect = [40, 215 + i * 62, 420, 54]
        for picked in (0, 1):
            widgets.append({'type': 'button', 'rect': rect, 'text': '', 'on_tap': f'persona.select.{i}',
                            'need_bind': p + '.present', 'bg': '#FF8C315B' if picked else COLORS['panel'],
                            'hide_bind': p + '.picked', 'hide_eq': 1 - picked})
        widgets.append(dict(label([56, 233 + i * 62, 390, 22], bind=p + '.line', scale=2), need_bind=p + '.present'))
    outputs += ['pd.ready', 'pd.name', 'pd.arcana', 'pd.next', 'pd.card', 'pd.nextskill', 'ui.persona']
    x = 490
    ready = {'need_bind': 'pd.ready'}
    widgets += [dict(label([x, 215, 710, 36], bind='pd.name', scale=3), **ready),
                dict(label([x, 258, 710, 26], bind='pd.arcana', scale=2), **ready) | {'color': COLORS['muted']}]
    for s, name in enumerate(('ST', 'MA', 'EN', 'AG', 'LU')):
        y = 300 + s * 40; outputs.append(f'pd.stat.{s}')
        widgets += [dict(label([x, y, 60, 26], name, scale=2), **ready) | {'color': COLORS['accent']},
                    dict({'type': 'bar', 'rect': [x + 70, y + 4, 520, 16], 'bind': f'pd.stat.{s}', 'max': 99,
                          'color': '#FF78C9FF', 'bg': '#FF403349'}, **ready),
                    dict({'type': 'value', 'rect': [x + 610, y, 80, 26], 'bind': f'pd.stat.{s}', 'text_scale': 2,
                          'color': COLORS['ink']}, **ready)]
    for e, name in enumerate(('SL', 'ST', 'PI', 'FI', 'IC', 'EL', 'WI', 'LI', 'DK')):
        outputs.append(f'pd.aff.{e}')
        cx = x + e * 78
        widgets += [dict({'type': 'rect', 'rect': [cx, 510, 72, 76], 'bg': COLORS['panel'], 'frame': 0}, **ready),
                    dict(label([cx + 8, 518, 60, 22], name, scale=2), **ready) | {'color': COLORS['muted']},
                    dict(label([cx + 8, 550, 60, 26], bind=f'pd.aff.{e}', scale=2), **ready) | {'color': COLORS['accent']}]
    widgets.append(dict(label([x, 610, 700, 28], bind='pd.next', scale=2), **ready))
    for k in range(8):
        outputs.append(f'pd.skill.{k}')
        # Native Details order: slots 0-3 down the left column, 4-7 down the right.
        widgets.append(dict(label([x + (k // 4) * 360, 660 + (k % 4) * 46, 340, 28], bind=f'pd.skill.{k}', scale=2), **ready))
    widgets += [dict(label([x, 870, 710, 28], bind='pd.card', scale=2), **ready),
                dict(label([x, 910, 710, 28], bind='pd.nextskill', scale=2), **ready) | {'color': COLORS['muted']}]
    return widgets

def links_widgets(outputs, scrolls):
    """The native S.LINK list: rank, arcana, link name, partner, rank pips and status."""
    rows = 28  # one link per arcana at most (0x1d4310 walks arcana 0..27)
    outputs += ['link.ready', 'link.count']
    scrolls.append({'id': 'links', 'rect': [40, 215, 1170, 780], 'count_bind': 'link.count', 'row_h': 96,
                    'bar': COLORS['accent'], 'bar_track': '#33FFFFFF', 'bar_w': 6, 'show_bind': 'link.ready'})
    row = {'repeat': rows, 'repeat_dy': 96, 'scroll': 'links'}
    need = {'need_bind': 'link.{i}.present'}
    outputs += [f'link.{i}.{k}' for i in range(rows)
                for k in ('present', 'arcana', 'name', 'partner', 'rank', 'rank_text', 'status')]
    return [dict(row, type='rect', rect=[40, 215, 1150, 86], bg=COLORS['panel'], frame=0, **need),
            dict(row, **label([60, 227, 220, 26], bind='link.{i}.rank_text', scale=2), **need),
            dict(row, **label([60, 259, 220, 30], bind='link.{i}.arcana', scale=3), **need) | {'color': COLORS['accent']},
            dict(row, **label([300, 227, 560, 30], bind='link.{i}.name', scale=3), **need),
            dict(row, **label([300, 265, 560, 24], bind='link.{i}.partner', scale=2), **need) | {'color': COLORS['muted']},
            dict(row, type='pips', rect=[880, 233, 290, 18], bind='link.{i}.rank', max=10,
                 color='#FFFF71AA', bg='#FF403349', **need),
            dict(row, **label([880, 263, 290, 24], bind='link.{i}.status', scale=2), **need) | {'color': COLORS['accent']}]

def calendar_widgets(outputs):
    """Month grid with today, holidays and full moons; previous/next month buttons."""
    outputs += ['cal.ready', 'cal.title', 'cal.next']
    ready = {'need_bind': 'cal.ready'}
    widgets = [dict({'type': 'button', 'rect': [40, 215, 160, 50], 'text': 'PREV', 'on_tap': 'calendar.prev',
                     'text_scale': 2, 'color': COLORS['ink'], 'bg': COLORS['panel']}, **ready),
               dict(label([240, 225, 760, 36], bind='cal.title', scale=4), **ready),
               dict({'type': 'button', 'rect': [1040, 215, 160, 50], 'text': 'NEXT', 'on_tap': 'calendar.next',
                     'text_scale': 2, 'color': COLORS['ink'], 'bg': COLORS['panel']}, **ready)]
    for i, name in enumerate(('SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT')):
        widgets.append(dict(label([40 + i * 166 + 12, 285, 150, 24], name, scale=2), **ready)
                       | {'color': COLORS['accent'] if i == 0 else COLORS['muted']})
    for c in range(42):
        p = f'cal.{c}'; outputs += [p + '.' + k for k in ('present', 'text', 'today', 'holiday', 'full')]
        x, y = 40 + (c % 7) * 166, 320 + (c // 7) * 100
        need = {'need_bind': p + '.present'}
        widgets += [dict({'type': 'rect', 'rect': [x, y, 156, 90], 'bg': COLORS['panel'], 'frame': 0}, **need),
                    dict({'type': 'rect', 'rect': [x, y, 156, 90], 'bg': '#FF8C315B', 'frame': 0, 'need_bind': p + '.today'}),
                    dict(label([x + 12, y + 10, 130, 30], bind=p + '.text', scale=3), **need) | {'color': COLORS['ink']},
                    dict(label([x + 12, y + 10, 130, 30], bind=p + '.text', scale=3), need_bind=p + '.holiday') | {'color': COLORS['accent']},
                    dict(label([x + 12, y + 54, 140, 24], 'FULL MOON', scale=1), need_bind=p + '.full') | {'color': '#FFFFE08A'}]
    widgets.append(dict(label([40, 925, 1160, 30], bind='cal.next', scale=3), **ready))
    return widgets

def stats_widgets(outputs):
    """Active Persona stats (accessory part shown separately), next EXP, and the
    protagonist's social stat ranks."""
    widgets = member_buttons()
    outputs += ['stat.ready', 'stat.header', 'stat.next', 'social.ready']
    widgets.append(dict(label([40, 330, 1160, 36], bind='stat.header', scale=3), need_bind='stat.ready'))
    for i, name in enumerate(('ST', 'MA', 'EN', 'AG', 'LU')):
        p = f'stat.{i}'; outputs += [p + '.value', p + '.bonus']
        y = 392 + i * 56
        widgets += [dict(label([60, y, 80, 30], name), need_bind='stat.ready', color=COLORS['accent']),
                    {'type': 'bar', 'rect': [150, y + 4, 700, 22], 'bind': p + '.value', 'max': 99,
                     'color': '#FF78C9FF', 'bg': '#FF403349', 'need_bind': 'stat.ready'},
                    {'type': 'value', 'rect': [880, y, 100, 30], 'bind': p + '.value', 'text_scale': 3,
                     'color': COLORS['ink'], 'need_bind': 'stat.ready'},
                    dict(label([990, y, 200, 30], bind=p + '.bonus'), need_bind='stat.ready', color=COLORS['accent'])]
    widgets.append(dict(label([60, 680, 600, 30], bind='stat.next'), need_bind='stat.ready'))
    for i, name in enumerate(('ACADEMICS', 'CHARM', 'COURAGE')):
        p = f'social.{i}'; outputs += [p + '.rank', p + '.name']
        y = 750 + i * 70
        widgets += [dict(label([60, y, 300, 30], name), need_bind='social.ready', color=COLORS['accent']),
                    dict(label([380, y, 420, 30], bind=p + '.name'), need_bind='social.ready'),
                    {'type': 'pips', 'rect': [820, y, 360, 26], 'bind': p + '.rank', 'max': 6,
                     'color': '#FFFF71AA', 'bg': '#FF403349', 'need_bind': 'social.ready'}]
    return widgets

def skills_widgets(outputs, scrolls):
    """Member buttons and the selected member's native Skill list."""
    widgets = member_buttons()
    outputs += ['skill.ready', 'skill.count', 'skill.header']
    widgets.append(dict(label([40, 330, 1160, 36], bind='skill.header', scale=3), need_bind='skill.ready'))
    scrolls.append({'id': 'skills', 'rect': [40, 380, 1170, 576], 'count_bind': 'skill.count', 'row_h': 72,
                    'bar': COLORS['accent'], 'bar_track': '#33FFFFFF', 'bar_w': 6, 'show_bind': 'skill.ready',
                    'reset_bind': 'ui.member'})
    outputs += [f'skill.{i}.{k}' for i in range(SKILL_ROWS) for k in ('present', 'name', 'cost')]
    row = {'repeat': SKILL_ROWS, 'repeat_dy': 72, 'scroll': 'skills'}
    widgets.append(dict(row, type='rect', rect=[40, 380, 1150, 60], bg=COLORS['panel'], frame=0,
                        need_bind='skill.{i}.present'))
    widgets.append(dict(row, **label([60, 396, 800, 30], bind='skill.{i}.name'), need_bind='skill.{i}.present'))
    widgets.append(dict(row, **label([1000, 396, 180, 30], bind='skill.{i}.cost'), need_bind='skill.{i}.present')
                   | {'color': COLORS['muted']})
    return widgets

def main():
    outputs = ['live.ready', 'live.date', 'live.money', 'status', 'ui.page', 'ui.member']
    # Tabs tell the module which page is open (ui_open); page_binds follow its ui.page, so
    # heavy lists are read only while their page shows.
    tabs = (('home', 'STATUS', 'Status'), ('personas', 'PERSONAS', 'Personas'),
            ('links', 'SOCIAL LINKS', 'S.Link'), ('skills', 'SKILLS', 'Skills'),
            ('equip', 'EQUIP', 'Equip'), ('stats', 'STATS', 'Stats'), ('items', 'ITEMS', 'Items'),
            ('calendar', 'CALENDAR', 'Calendar'))
    actions = {f'ui.open.{n}': {'kind': 'module', 'action': 'ui_open', 'argument': n}
               for n in range(1, len(tabs) + 1)}
    actions.update({f'member.select.{i}': {'kind': 'module', 'action': 'member_select', 'argument': i}
                    for i in range(10)})
    actions.update({f'item.tab.{i}': {'kind': 'module', 'action': 'item_tab', 'argument': i} for i in range(3)})
    actions.update({f'persona.select.{i}': {'kind': 'module', 'action': 'persona_select', 'argument': i} for i in range(12)})
    actions['calendar.prev'] = {'kind': 'module', 'action': 'calendar_shift', 'argument': 0}
    actions['calendar.next'] = {'kind': 'module', 'action': 'calendar_shift', 'argument': 1}
    page_binds = [{'point': 'ui.page', 'equals': n, 'when_equal': {'page': page_id}}
                  for n, (page_id, _, _) in enumerate(tabs, 1)]
    pages = []
    for page_id, title, _ in tabs:
        widgets = [label([40, 28, 900, 42], 'PERSONA 3 PORTABLE / ' + title, scale=4)]
        for i, (target, _, caption) in enumerate(tabs):
            widgets.append({'type': 'button', 'rect': [40 + i * 146, 90, 136, 55], 'text': caption,
                            'on_tap': f'ui.open.{i + 1}', 'text_scale': 2, 'color': COLORS['ink'],
                            'bg': '#FF8C315B' if page_id == target else COLORS['panel']})
        widgets += [label([40, 165, 850, 30], bind='live.date', ready=True),
                    label([925, 165, 280, 30], bind='live.money', ready=True),
                    {'type': 'label', 'rect': [60, 440, 1120, 80], 'text': 'Waiting for supported gameplay',
                     'text_scale': 4, 'color': COLORS['muted'], 'hide_bind': 'live.ready', 'hide_eq': 1},
                    label([40, 1020, 1160, 24], bind='status', scale=2),
                    label([40, 1050, 1160, 20], 'Read-only preview | 2D field/camp support | Battle and dungeon exploration pending', scale=2),
                    dict(label([40, 700, 1160, 36], bind='module_error_message', scale=3), need_bind='module_error')]
        if page_id == 'calendar':
            widgets += calendar_widgets(outputs)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets})
            continue
        if page_id == 'links':
            scrolls = []
            widgets += links_widgets(outputs, scrolls)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets, 'scrolls': scrolls})
            continue
        if page_id == 'personas':
            widgets += personas_widgets(outputs)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets})
            continue
        if page_id == 'items':
            scrolls = []
            widgets += items_widgets(outputs, scrolls)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets, 'scrolls': scrolls})
            continue
        if page_id == 'stats':
            widgets += stats_widgets(outputs)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets})
            continue
        if page_id == 'equip':
            widgets += equip_widgets(outputs)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets})
            continue
        if page_id == 'skills':
            scrolls = []
            widgets += skills_widgets(outputs, scrolls)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets, 'scrolls': scrolls})
            continue
        count = {'home': 10}[page_id]
        prefix = {'home': 'member'}[page_id]
        for i in range(count):
            p = f'{prefix}.{i}'; outputs.append(p + '.present')
            y = 220 + i * {'home': 78, 'personas': 60, 'links': 28}[page_id]
            if page_id == 'home':
                outputs += [p + '.' + k for k in ('name', 'tab', 'selected', 'stats', 'hp', 'sp', 'max_hp', 'max_sp')]
                row = [label([50, y, 600, 26], bind=p + '.name'), label([670, y, 530, 26], bind=p + '.stats', scale=2)]
                for k, x, color in (('hp', 50, '#FFFF71AA'), ('sp', 670, '#FF78C9FF')):
                    row.append({'type': 'bar', 'rect': [x, y + 38, 530, 10], 'bind': p + '.' + k,
                                'max_bind': p + '.max_' + k, 'color': color, 'bg': '#FF403349'})
            else:
                outputs.append(p + '.line'); row = [label([50, y, 1140, 28], bind=p + '.line', scale=3 if page_id == 'personas' else 2)]
            for w in row: w['need_bind'] = p + '.present'
            widgets += row
        pages.append({'id': page_id, 'title': title, 'widgets': widgets})
    result = {'format': 1, 'title_id': '0100DCD01525A000', 'name': 'Persona 3 Portable - Read-only Preview',
              'min_runtime': 19, 'requires_module': True, 'poll_hz': 60,
              'canvas_w': 1240, 'canvas_h': 1080, 'background': '#FF17131E',
              'module_outputs': outputs, 'module': {'abi': 1, 'build_ids': [BUILD], 'libraries': {}},
              'actions': actions, 'page_binds': page_binds, 'pages': pages}
    dest = ROOT / 'packages/Persona3Portable/dualscreen/manifest.json'
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(result, indent=2) + '\n')

if __name__ == '__main__': main()
