#!/usr/bin/env python3
"""Generate the asset-free read-only P3P preview layout."""
import json
from pathlib import Path
from saves import BUILD

ROOT = Path(__file__).resolve().parents[2]
COLORS = {'ink': '#FFF8EDF3', 'muted': '#FFC0B2C4', 'accent': '#FFFF71AA', 'panel': '#FF282032'}

# Text sizes follow the P5R companion on the same 1240x1080 canvas (cap height = 5 px per
# step): 3 for status lines, 4 for body text, 5 for key values and list rows, 6 for buttons and
# page titles. Nothing below 3, so it stays readable on a handheld's second screen.
SIZE = {'small': 3, 'body': 4, 'value': 5, 'button': 6, 'title': 6}
SELECTED = '#FF8C315B'

def label(rect, text='', bind=None, scale=SIZE['body'], ready=False):
    w = {'type': 'label', 'rect': rect, 'text': text, 'color': COLORS['ink'], 'text_scale': scale}
    if bind: w['bind_text'] = bind
    if ready: w['need_bind'] = 'live.ready'
    return w

def button(rect, caption, action, scale=SIZE['button'], bg=None):
    """A tap target with its caption centred in the box."""
    return {'type': 'button', 'rect': rect, 'text': caption, 'on_tap': action, 'text_scale': scale,
            'align': 'center', 'text_center_h': rect[3], 'color': COLORS['ink'], 'bg': bg or COLORS['panel']}

def chrome(title, home=False):
    """Shared frame (as P5R's menu pages): title and date on top; at the bottom a large Back
    button to the hub and the reader's status line. The hub has no Back button."""
    widgets = [label([24, 22, 760, 48], title, scale=SIZE['title'])]
    if not home:
        widgets.append(label([1216, 30, 0, 40], bind='live.date', scale=SIZE['body'], ready=True) | {'align': 'right'})
        widgets.append(button([16, 982, 292, 84], 'Back', 'ui.open.1'))
    x = 24 if home else 330
    widgets += [label([x, 1012, 1216 - x, 32], bind='status', scale=SIZE['small']) | {'color': COLORS['muted']},
                # The reader's reason while values are unavailable (e.g. loading, unsupported scene).
                {'type': 'label', 'rect': [620, 470, 0, 60], 'text': '', 'bind_text': 'status', 'align': 'center',
                 'wrap_width': 1100, 'text_scale': SIZE['value'], 'color': COLORS['muted'],
                 'hide_bind': 'live.ready', 'hide_eq': 1},
                dict(label([24, 700, 1192, 40], bind='module_error_message', scale=SIZE['body']), need_bind='module_error')]
    return widgets

# Hub buttons in the native camp menu's order (SKILL, ITEM, PERSONA, EQUIP, STATUS, S.LINK),
# then the calendar; the number is the module's ui_open page.
MENU = (('Skills', 4), ('Items', 7), ('Personas', 2), ('Equip', 5), ('Stats', 6), ('S.Link', 3), ('Calendar', 8))

def home_widgets(outputs):
    """The hub: date and money, the party's HP/SP, and the menu buttons."""
    widgets = [label([24, 92, 800, 40], bind='live.date', scale=SIZE['value'], ready=True),
               label([1216, 92, 0, 40], bind='live.money', scale=SIZE['value'], ready=True) | {'align': 'right'}]
    for i in range(4):  # the first four of the roster (the active battle party is not traced yet)
        p = f'member.{i}'; y = 160 + i * 100
        need = {'need_bind': p + '.present'}
        widgets += [label([40, y, 580, 40], bind=p + '.name', scale=SIZE['value']) | need,
                    label([640, y + 4, 576, 32], bind=p + '.stats', scale=SIZE['body']) | need]
        for k, x, color in (('hp', 40, '#FFFF71AA'), ('sp', 640, '#FF78C9FF')):
            widgets.append({'type': 'bar', 'rect': [x, y + 52, 560, 16], 'bind': p + '.' + k,
                            'max_bind': p + '.max_' + k, 'color': color, 'bg': '#FF403349'} | need)
    for i, (caption, page) in enumerate(MENU):
        wide = i == len(MENU) - 1 and len(MENU) % 2
        rect = [24 + (i % 2) * 608, 572 + (i // 2) * 102, 1192 if wide else 584, 90]
        widgets.append(button(rect, caption, f'ui.open.{page}'))
    return widgets

SKILL_ROWS = 96  # 0x108770 asserts fewer than 0x61 entries

def member_buttons():
    """Member buttons (two rows of five) selecting ui.member; shared by Skills and Equip."""
    widgets = []
    for i in range(10):
        p = f'member.{i}'
        rect = [24 + (i % 5) * 240, 92 + (i // 5) * 74, 230, 64]
        for selected in (0, 1):
            widgets.append({'type': 'button', 'rect': rect, 'text': '', 'on_tap': f'member.select.{i}',
                            'need_bind': p + '.present', 'bg': SELECTED if selected else COLORS['panel'],
                            'hide_bind': p + '.selected', 'hide_eq': 1 - selected})
        widgets.append(dict(label([rect[0] + rect[2] // 2, rect[1], 0, rect[3]], bind=p + '.tab', scale=SIZE['body'])
                            | {'align': 'center', 'text_center_h': rect[3], 'fit_text': True,
                               'wrap_width': rect[2] - 16, 'text_min_scale': SIZE['small']},
                            need_bind=p + '.present'))
    return widgets

def equip_widgets(outputs):
    """The selected member's four slots with the native Status panel's figures."""
    widgets = member_buttons()
    outputs += ['equip.ready', 'equip.header']
    ready = {'need_bind': 'equip.ready'}
    widgets.append(label([24, 252, 1192, 36], bind='equip.header') | ready | {'color': COLORS['muted']})
    for i, slot in enumerate(('WEAPON', 'BODY', 'FEET', 'ACCESSORY')):  # native Equip labels
        p = f'equip.{i}'; outputs += [p + '.name', p + '.stats']
        y = 304 + i * 166
        widgets += [{'type': 'rect', 'rect': [24, y, 1192, 154], 'bg': COLORS['panel'], 'frame': 0} | ready,
                    label([48, y + 14, 500, 32], slot) | ready | {'color': COLORS['accent']},
                    label([48, y + 66, 1144, 44], bind=p + '.name', scale=SIZE['value']) | ready,
                    label([1192, y + 14, 0, 32], bind=p + '.stats') | ready | {'color': COLORS['muted'], 'align': 'right'}]
    return widgets

ITEM_ROWS = 256  # the module publishes at most this many rows

def items_widgets(outputs, scrolls):
    """Inventory as the native camp Item tabs list it (Use / Equip / Key), with counts."""
    outputs += ['item.ready', 'item.count', 'item.header', 'item.tab']
    widgets = []
    for i, caption in enumerate(('Use', 'Equip', 'Key')):
        for selected in (0, 1):
            widgets.append(button([24 + i * 292, 92, 280, 76], caption, f'item.tab.{i}', scale=SIZE['value'],
                                  bg=SELECTED if selected else None)
                           | {'hide_bind': 'item.tab', 'keep_min': i if selected else -1,
                              'keep_max': i if selected else 99, **({} if selected else {'hide_eq': i})})
    widgets.append(label([1216, 92, 0, 76], bind='item.header', scale=SIZE['value'])
                   | {'need_bind': 'item.ready', 'align': 'right', 'text_center_h': 76, 'color': COLORS['muted']})
    scrolls.append({'id': 'items', 'rect': [24, 188, 1200, 780], 'count_bind': 'item.count', 'row_h': 86,
                    'bar': COLORS['accent'], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'item.ready',
                    'reset_bind': 'item.tab'})
    outputs += [f'item.{i}.{k}' for i in range(ITEM_ROWS) for k in ('present', 'name', 'count')]
    row = {'repeat': ITEM_ROWS, 'repeat_dy': 86, 'scroll': 'items'}
    need = {'need_bind': 'item.{i}.present'}
    widgets += [dict(row, type='rect', rect=[24, 188, 1176, 76], bg=COLORS['panel'], frame=0, **need),
                dict(row, **label([48, 188, 900, 76], bind='item.{i}.name', scale=SIZE['value']), **need) | {'text_center_h': 76},
                dict(row, **label([1176, 188, 0, 76], bind='item.{i}.count', scale=SIZE['value']), **need)
                | {'color': COLORS['muted'], 'align': 'right', 'text_center_h': 76}]
    return widgets

def personas_widgets(outputs):
    """Stock list (tap to pick) and the native PERSONA -> Details panel for the pick."""
    widgets = []
    for i in range(12):
        p = f'persona.{i}'; outputs += [p + '.present', p + '.line', p + '.picked']
        rect = [24, 92 + i * 72, 400, 64]
        for picked in (0, 1):
            widgets.append({'type': 'button', 'rect': rect, 'text': '', 'on_tap': f'persona.select.{i}',
                            'need_bind': p + '.present', 'bg': SELECTED if picked else COLORS['panel'],
                            'hide_bind': p + '.picked', 'hide_eq': 1 - picked})
        widgets.append(label([40, rect[1], 368, 64], bind=p + '.line')
                       | {'need_bind': p + '.present', 'text_center_h': 64, 'fit_text': True, 'wrap_width': 368,
                          'text_min_scale': SIZE['small']})
    outputs += ['pd.ready', 'pd.name', 'pd.arcana', 'pd.next', 'pd.card', 'pd.nextskill', 'ui.persona']
    x, w = 448, 768
    ready = {'need_bind': 'pd.ready'}
    widgets += [label([x, 92, w, 44], bind='pd.name', scale=SIZE['value']) | ready,
                label([x, 142, w, 32], bind='pd.arcana') | ready | {'color': COLORS['muted']}]
    for s, name in enumerate(('St', 'Ma', 'En', 'Ag', 'Lu')):
        y = 190 + s * 46; outputs.append(f'pd.stat.{s}')
        widgets += [label([x, y, 70, 32], name) | ready | {'color': COLORS['accent']},
                    {'type': 'bar', 'rect': [x + 80, y + 6, 580, 22], 'bind': f'pd.stat.{s}', 'max': 99,
                     'color': '#FF78C9FF', 'bg': '#FF403349'} | ready,
                    {'type': 'value', 'rect': [x + w, y, 0, 32], 'bind': f'pd.stat.{s}', 'text_scale': SIZE['body'],
                     'align': 'right', 'color': COLORS['ink']} | ready]
    cell = w // 9
    for e, name in enumerate(('Sl', 'St', 'Pi', 'Fi', 'Ic', 'El', 'Wi', 'Li', 'Dk')):
        outputs.append(f'pd.aff.{e}')
        cx = x + e * cell
        widgets += [{'type': 'rect', 'rect': [cx, 430, cell - 6, 92], 'bg': COLORS['panel'], 'frame': 0} | ready,
                    label([cx + (cell - 6) // 2, 438, 0, 28], name, scale=SIZE['small']) | ready
                    | {'color': COLORS['muted'], 'align': 'center'},
                    label([cx + (cell - 6) // 2, 476, 0, 32], bind=f'pd.aff.{e}') | ready
                    | {'color': COLORS['accent'], 'align': 'center'}]
    widgets.append(label([x, 538, w, 32], bind='pd.next') | ready)
    for k in range(8):
        outputs.append(f'pd.skill.{k}')
        # Native Details order: slots 0-3 down the left column, 4-7 down the right.
        widgets.append(label([x + (k // 4) * 390, 590 + (k % 4) * 46, 378, 32], bind=f'pd.skill.{k}') | ready)
    widgets += [label([x, 790, w, 32], bind='pd.card') | ready,
                label([x, 836, w, 32], bind='pd.nextskill') | ready | {'color': COLORS['muted']}]
    return widgets

def links_widgets(outputs, scrolls):
    """The native S.LINK list: rank, arcana, link name, partner, rank pips and status."""
    rows = 28  # one link per arcana at most (0x1d4310 walks arcana 0..27)
    outputs += ['link.ready', 'link.count']
    scrolls.append({'id': 'links', 'rect': [24, 92, 1200, 876], 'count_bind': 'link.count', 'row_h': 110,
                    'bar': COLORS['accent'], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'link.ready'})
    row = {'repeat': rows, 'repeat_dy': 110, 'scroll': 'links'}
    need = {'need_bind': 'link.{i}.present'}
    outputs += [f'link.{i}.{k}' for i in range(rows)
                for k in ('present', 'arcana', 'name', 'partner', 'rank', 'rank_text', 'status')]
    return [dict(row, type='rect', rect=[24, 92, 1176, 100], bg=COLORS['panel'], frame=0, **need),
            dict(row, **label([48, 104, 270, 40], bind='link.{i}.arcana', scale=SIZE['value']), **need) | {'color': COLORS['accent']},
            dict(row, **label([48, 150, 270, 32], bind='link.{i}.rank_text'), **need),
            dict(row, **label([330, 104, 520, 40], bind='link.{i}.name', scale=SIZE['value']), **need)
            | {'fit_text': True, 'wrap_width': 520, 'text_min_scale': SIZE['body']},
            dict(row, **label([330, 150, 520, 32], bind='link.{i}.partner'), **need) | {'color': COLORS['muted']},
            dict(row, type='pips', rect=[850, 116, 330, 22], bind='link.{i}.rank', max=10,
                 color='#FFFF71AA', bg='#FF403349', **need),
            dict(row, **label([870, 150, 310, 32], bind='link.{i}.status'), **need) | {'color': COLORS['accent']}]

def battle_page(outputs):
    """Shown automatically during battles: party HP/SP and the encounter's enemies, with
    affinities only once analyzed (as the native Analyze panel)."""
    outputs += ['battle.active', 'en.count']
    widgets = [label([24, 22, 760, 48], 'BATTLE', scale=SIZE['title']),
               label([1216, 30, 0, 40], bind='live.date') | {'align': 'right'}]
    for i in range(4):
        p = f'member.{i}'; y = 92 + i * 82
        need = {'need_bind': p + '.present'}
        widgets += [label([40, y, 580, 36], bind=p + '.name') | need,
                    label([640, y, 576, 36], bind=p + '.stats') | need,
                    {'type': 'bar', 'rect': [40, y + 44, 560, 16], 'bind': p + '.hp', 'max_bind': p + '.max_hp',
                     'color': '#FFFF71AA', 'bg': '#FF403349'} | need,
                    {'type': 'bar', 'rect': [640, y + 44, 560, 16], 'bind': p + '.sp', 'max_bind': p + '.max_sp',
                     'color': '#FF78C9FF', 'bg': '#FF403349'} | need]
    widgets.append(label([24, 424, 400, 36], 'Enemies') | {'color': COLORS['accent']})
    for i in range(5):
        p = f'en.{i}'; y = 470 + i * 102
        outputs += [p + '.' + k for k in ('present', 'name', 'info', 'hp_text', 'hp', 'max_hp', 'analyzed')]
        need = {'need_bind': p + '.present'}
        widgets += [{'type': 'rect', 'rect': [24, y, 1192, 94], 'bg': COLORS['panel'], 'frame': 0} | need,
                    label([40, y + 8, 420, 40], bind=p + '.name', scale=SIZE['value']) | need
                    | {'fit_text': True, 'wrap_width': 420, 'text_min_scale': SIZE['body']},
                    label([40, y + 54, 420, 32], bind=p + '.info') | need | {'color': COLORS['muted']},
                    label([476, y + 8, 250, 32], bind=p + '.hp_text') | need,
                    {'type': 'bar', 'rect': [476, y + 56, 240, 16], 'bind': p + '.hp', 'max_bind': p + '.max_hp',
                     'color': '#FFFF71AA', 'bg': '#FF403349'} | need]
        for k, name in enumerate(('Sl', 'St', 'Pi', 'Fi', 'Ic', 'El', 'Wi', 'Li', 'Dk')):
            outputs.append(f'{p}.aff.{k}')
            cx = 760 + k * 50
            widgets += [label([cx, y + 10, 0, 28], name, scale=SIZE['small']) | need | {'color': COLORS['muted'], 'align': 'center'},
                        label([cx, y + 50, 0, 32], bind=f'{p}.aff.{k}') | need | {'color': COLORS['accent'], 'align': 'center'}]
    widgets.append(label([24, 1012, 1192, 32], bind='status', scale=SIZE['small']) | {'color': COLORS['muted']})
    return {'id': 'battle', 'title': 'BATTLE', 'widgets': widgets}

def calendar_widgets(outputs):
    """Month grid with today, holidays and full moons; previous/next month buttons."""
    outputs += ['cal.ready', 'cal.title', 'cal.next']
    ready = {'need_bind': 'cal.ready'}
    widgets = [button([24, 92, 220, 72], 'Prev', 'calendar.prev', scale=SIZE['value']) | ready,
               label([620, 92, 0, 72], bind='cal.title', scale=SIZE['title']) | ready | {'align': 'center', 'text_center_h': 72},
               button([996, 92, 220, 72], 'Next', 'calendar.next', scale=SIZE['value']) | ready]
    for i, name in enumerate(('Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat')):
        widgets.append(label([24 + i * 170 + 81, 180, 0, 32], name) | ready
                       | {'align': 'center', 'color': COLORS['accent'] if i == 0 else COLORS['muted']})
    for c in range(42):
        p = f'cal.{c}'; outputs += [p + '.' + k for k in ('present', 'text', 'today', 'holiday', 'full')]
        x, y = 24 + (c % 7) * 170, 222 + (c // 7) * 116
        need = {'need_bind': p + '.present'}
        widgets += [{'type': 'rect', 'rect': [x, y, 162, 108], 'bg': COLORS['panel'], 'frame': 0} | need,
                    {'type': 'rect', 'rect': [x, y, 162, 108], 'bg': SELECTED, 'frame': 0, 'need_bind': p + '.today'},
                    label([x + 14, y + 10, 140, 44], bind=p + '.text', scale=SIZE['value']) | need,
                    label([x + 14, y + 10, 140, 44], bind=p + '.text', scale=SIZE['value'])
                    | {'need_bind': p + '.holiday', 'color': COLORS['accent']},
                    label([x + 81, y + 70, 0, 28], 'Full Moon', scale=SIZE['small'])
                    | {'need_bind': p + '.full', 'color': '#FFFFE08A', 'align': 'center'}]
    widgets.append(label([24, 926, 1192, 40], bind='cal.next') | ready)
    return widgets

def stats_widgets(outputs):
    """Active Persona stats (accessory part shown separately), next EXP, and the
    protagonist's social stat ranks."""
    widgets = member_buttons()
    outputs += ['stat.ready', 'stat.header', 'stat.next', 'social.ready']
    ready = {'need_bind': 'stat.ready'}
    widgets.append(label([24, 252, 1192, 36], bind='stat.header') | ready | {'color': COLORS['muted']})
    for i, name in enumerate(('St', 'Ma', 'En', 'Ag', 'Lu')):
        p = f'stat.{i}'; outputs += [p + '.value', p + '.bonus']
        y = 306 + i * 60
        widgets += [label([48, y, 90, 40], name, scale=SIZE['value']) | ready | {'color': COLORS['accent']},
                    {'type': 'bar', 'rect': [150, y + 8, 680, 26], 'bind': p + '.value', 'max': 99,
                     'color': '#FF78C9FF', 'bg': '#FF403349'} | ready,
                    {'type': 'value', 'rect': [920, y, 0, 40], 'bind': p + '.value', 'text_scale': SIZE['value'],
                     'align': 'right', 'color': COLORS['ink']} | ready,
                    label([944, y, 260, 40], bind=p + '.bonus', scale=SIZE['value']) | ready | {'color': COLORS['accent']}]
    widgets.append(label([48, 616, 800, 40], bind='stat.next', scale=SIZE['value']) | ready)
    for i, name in enumerate(('Academics', 'Charm', 'Courage')):
        p = f'social.{i}'; outputs += [p + '.rank', p + '.name']
        y = 690 + i * 92
        widgets += [label([48, y, 320, 40], name, scale=SIZE['value']) | {'need_bind': 'social.ready', 'color': COLORS['accent']},
                    label([380, y, 420, 40], bind=p + '.name', scale=SIZE['value']) | {'need_bind': 'social.ready'},
                    {'type': 'pips', 'rect': [820, y + 4, 380, 32], 'bind': p + '.rank', 'max': 6,
                     'color': '#FFFF71AA', 'bg': '#FF403349', 'need_bind': 'social.ready'}]
    return widgets

def skills_widgets(outputs, scrolls):
    """Member buttons and the selected member's native Skill list."""
    widgets = member_buttons()
    outputs += ['skill.ready', 'skill.count', 'skill.header']
    widgets.append(dict(label([24, 252, 1192, 36], bind='skill.header', scale=SIZE['body']), need_bind='skill.ready')
                   | {'color': COLORS['muted']})
    scrolls.append({'id': 'skills', 'rect': [24, 304, 1200, 664], 'count_bind': 'skill.count', 'row_h': 83,
                    'bar': COLORS['accent'], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'skill.ready',
                    'reset_bind': 'ui.member'})
    outputs += [f'skill.{i}.{k}' for i in range(SKILL_ROWS) for k in ('present', 'name', 'cost')]
    row = {'repeat': SKILL_ROWS, 'repeat_dy': 83, 'scroll': 'skills'}
    widgets.append(dict(row, type='rect', rect=[24, 304, 1176, 74], bg=COLORS['panel'], frame=0,
                        need_bind='skill.{i}.present'))
    widgets.append(dict(row, **label([48, 304, 860, 74], bind='skill.{i}.name', scale=SIZE['value']),
                        need_bind='skill.{i}.present') | {'text_center_h': 74})
    widgets.append(dict(row, **label([1176, 304, 0, 74], bind='skill.{i}.cost', scale=SIZE['value']),
                        need_bind='skill.{i}.present') | {'color': COLORS['muted'], 'align': 'right', 'text_center_h': 74})
    return widgets

def main():
    outputs = ['live.ready', 'live.date', 'live.money', 'status', 'ui.page', 'ui.member']
    outputs += [f'member.{i}.{k}' for i in range(10)
                for k in ('present', 'name', 'tab', 'selected', 'stats', 'hp', 'sp', 'max_hp', 'max_sp')]
    # Tabs tell the module which page is open (ui_open); page_binds follow its ui.page, so
    # heavy lists are read only while their page shows.
    tabs = (('home', 'PERSONA 3 PORTABLE', 'Status'), ('personas', 'PERSONAS', 'Personas'),
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
    # The module switches to the battle page (ui.page 9) while a battle runs.
    page_binds.append({'point': 'ui.page', 'equals': 9, 'when_equal': {'page': 'battle'}})
    pages = []
    for page_id, title, _ in tabs:
        widgets = chrome(title, home=page_id == 'home')
        if page_id == 'home':
            widgets += home_widgets(outputs)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets})
            continue
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
    pages.append(battle_page(outputs))
    result = {'format': 1, 'title_id': '0100DCD01525A000', 'name': 'Persona 3 Portable - Read-only Preview',
              'min_runtime': 19, 'requires_module': True, 'poll_hz': 60,
              'canvas_w': 1240, 'canvas_h': 1080, 'background': '#FF17131E',
              # The game font from the player's romfs (module decode_font/load_image, p3p_font.h);
              # p3p_font.txt is a marker holding no game data.
              'font': 'file:p3p_font.txt', 'font_atlas': 'module:p3p_font',
              'module_outputs': outputs, 'module': {'abi': 1, 'build_ids': [BUILD], 'libraries': {}},
              'actions': actions, 'page_binds': page_binds, 'pages': pages}
    dest = ROOT / 'packages/Persona3Portable/dualscreen/manifest.json'
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(result, indent=2) + '\n')

if __name__ == '__main__': main()
