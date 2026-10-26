#!/usr/bin/env python3
"""Generate the asset-free read-only P3P preview layout."""
import json
from pathlib import Path
from saves import BUILD

ROOT = Path(__file__).resolve().parents[2]
# Colours follow the native camp menu: dark text on light panels over the route's backdrop
# (module:p3p:bg, drawn by the module), white text on the dark title band and selected rows,
# yellow titles. ACCENT marks text/tints that take the route's accent (split_route below).
ACCENT = 'ACCENT'
ACCENTS = ['#FF1F4FB5', '#FFB3154C']  # [male (blue), female (pink)], picked by ui.route
COLORS = {'ink': '#FF2B1530', 'muted': '#FF6E5A78', 'light': '#FFFFFFFF', 'title': '#FFFFC43C',
          'accent': ACCENT, 'panel': '#00000000'}

# Text sizes follow the P5R companion on the same 1240x1080 canvas (cap height = 5 px per
# step): 3 for status lines, 4 for body text, 5 for key values and list rows, 6 for buttons and
# page titles. Nothing below 3, so it stays readable on a handheld's second screen.
SIZE = {'small': 3, 'body': 4, 'value': 5, 'button': 6, 'title': 6}

# Route-coloured panels are the module's 8x8 white picture, tinted [male, female].
THEME = {'panel': ['#D9FFFFFF', '#D9FFFFFF'],    # rows and buttons
         'select': ['#FF0E2460', '#FF6A0A1E'],   # picked row / tab / today
         'line': ['#FF0E2460', '#FF6A0A1E']}     # scrollbars
HP, SP, BAR_BG = '#FFF29A1C', '#FF8C5CF0', '#40000000'

# Camp sheet sprites (c_main_01.spr) by index, with their HD sizes for the aspect ratio.
SPRITES = {'MAIN': (9, 916, 176), 'PERSONA': (204, 836, 96), 'SKILL': (297, 816, 152),
           'ITEM': (275, 988, 192), 'EQUIP': (146, 892, 172), 'STATUS': (302, 700, 104),
           'CALENDAR': (406, 828, 84), 'S.LINK': (10, 264, 40),
           'weapon': (172, 196, 56), 'body': (173, 140, 64), 'feet': (174, 120, 56), 'accessory': (175, 252, 60)}
ELEMENTS = range(223, 232)          # Sl St Pi Fi Ic El Wi Li Dk, 80x64 each
WEEKDAYS = (612, 613, 614, 615, 616, 617, 618)  # SUN .. SAT, 136x40
FULL_MOON = 555

def fill(rect, kind, **extra):
    """A flat panel in the route's colour (the module's 8x8 white picture, tinted)."""
    return {'type': 'image', 'rect': rect, 'src': 'module:p3p:fill', 'tint': THEME[kind][0],
            'tint_bind': 'ui.route', 'tint_colors': THEME[kind]} | extra

def sprite(name, x, y, h, align='left', tint=None):
    """A camp-sheet sprite drawn h px high (the module resamples to exactly that height)."""
    n, sw, sh = SPRITES[name]
    w = round(sw * h / sh)
    x = x - w // 2 if align == 'center' else x - w if align == 'right' else x
    widget = {'type': 'image', 'rect': [x, y, w, h], 'src': f'module:p3p:spr/{n}@{h}'}
    if tint: widget['tint'] = tint
    return widget

def split_route(widgets):
    """Expands ACCENT colours: a widget without its own hide gate becomes one copy per route
    (gated on ui.route); an image takes tint_bind instead; anything else gets the female accent."""
    out = []
    for w in widgets:
        if w.get('tint') == ACCENT:
            w = w | {'tint': ACCENTS[0], 'tint_bind': 'ui.route', 'tint_colors': ACCENTS}
        if w.get('color') != ACCENT:
            out.append(w)
        elif 'hide_bind' in w:
            out.append(w | {'color': ACCENTS[1]})
        else:
            out += [w | {'color': c, 'hide_bind': 'ui.route', 'hide_eq': 1 - r} for r, c in enumerate(ACCENTS)]
    return out

def label(rect, text='', bind=None, scale=SIZE['body'], ready=False):
    w = {'type': 'label', 'rect': rect, 'text': text, 'color': COLORS['ink'], 'text_scale': scale}
    if bind: w['bind_text'] = bind
    if ready: w['need_bind'] = 'live.ready'
    return w

def button(rect, caption, action, scale=SIZE['button'], kind='panel', **extra):
    """A tap target: a route-coloured panel with its game-font caption centred on it. Returns a
    widget list; `extra` (gates) applies to every part."""
    parts = [fill(rect, kind)]
    ink = COLORS['light'] if kind == 'select' else COLORS['ink']
    parts.append({'type': 'button', 'rect': rect, 'text': caption, 'on_tap': action, 'text_scale': scale,
                  'align': 'center', 'text_center_h': rect[3], 'color': ink, 'bg': '#00000000',
                  'border': 0})
    return [p | extra for p in parts]

def toggle(rect, caption, action, bind, on, scale=SIZE['button']):
    """A tab/row button whose panel shows `select` while bind == on (else `panel`)."""
    return (button(rect, caption, action, scale, 'select', hide_bind=bind, keep_min=on, keep_max=on)
            + button(rect, caption, action, scale, 'panel', hide_bind=bind, hide_eq=on))

def chrome(title, home=False):
    """Shared frame, as the native camp menu: the route's wash, a title band with the page's big
    italic lettering, a faint copy of it as a watermark; at the bottom a Back button to the hub
    and the reader's status line. The hub has no Back button."""
    widgets = [{'type': 'image', 'rect': [0, 0, 1240, 1080], 'bind': 'ui.route', 'src_format': 'module:p3p:bg/%d'}]
    if title in SPRITES:
        n, sw, sh = SPRITES[title]
        mark_w = min(round(sw * 260 / sh), 1200); mark_h = round(mark_w * sh / sw)
        widgets += [{'type': 'image', 'rect': [1240 - mark_w, 990 - mark_h, mark_w, mark_h],
                     'src': f'module:p3p:spr/{n}@{mark_h}', 'tint': '#30FFFFFF'},
                    sprite(title, 44, 12, 62, tint=COLORS['title'])]
    else:  # no camp lettering for this page: the game font
        widgets.append(label([44, 20, 760, 48], title, scale=SIZE['title']) | {'color': COLORS['title']})
    if not home:
        widgets.append(label([1216, 26, 0, 40], bind='live.date', scale=SIZE['body'], ready=True)
                       | {'align': 'right', 'color': COLORS['light']})
        widgets += button([36, 982, 280, 84], 'Back', 'ui.open.1')
    x = 44 if home else 336
    # The status line only once values are live; until then the centred label says why.
    widgets += [label([x, 1012, 1216 - x, 32], bind='status', scale=SIZE['small'], ready=True) | {'color': COLORS['muted']},
                # The reader's reason while values are unavailable (e.g. loading, unsupported scene).
                {'type': 'label', 'rect': [620, 470, 0, 60], 'text': '', 'bind_text': 'status', 'align': 'center',
                 'wrap_width': 1100, 'text_scale': SIZE['value'], 'color': COLORS['muted'],
                 'hide_bind': 'live.ready', 'hide_eq': 1},
                dict(label([44, 700, 1192, 40], bind='module_error_message', scale=SIZE['body']), need_bind='module_error')]
    return widgets

# Hub buttons in the native camp menu's order (SKILL, ITEM, PERSONA, EQUIP, STATUS, S.LINK),
# then the calendar and map, all in the game font; the number is the module's ui_open page.
MENU = (('SKILL', 4), ('ITEM', 7), ('PERSONA', 2), ('EQUIP', 5), ('STATUS', 6), ('S.LINK', 3), ('CALENDAR', 8),
        ('MAP', 10))

def bars(p, x, y, w, need):
    """A member's HP and SP bars side by side."""
    half = (w - 16) // 2
    return [{'type': 'bar', 'rect': [x, y, half, 14], 'bind': p + '.hp', 'max_bind': p + '.max_hp',
             'color': HP, 'bg': BAR_BG, 'frame': 0} | need,
            {'type': 'bar', 'rect': [x + half + 16, y, half, 14], 'bind': p + '.sp', 'max_bind': p + '.max_sp',
             'color': SP, 'bg': BAR_BG, 'frame': 0} | need]

def home_widgets(outputs):
    """The hub: date, moon and money; the lead's camp portrait; the party's HP/SP; the menu."""
    outputs += ['ui.route', 'live.lead', 'live.moon_spr']
    widgets = [{'type': 'image', 'rect': [700, 92, 520, 520], 'bind': 'live.lead',
                'src_format': 'module:p3p:bust/%d@520', 'need_bind': 'live.lead'},
               label([1150, 10, 0, 40], bind='live.date', scale=SIZE['value'], ready=True) | {'align': 'right', 'color': COLORS['light']},
               label([1150, 50, 0, 32], bind='live.money', scale=SIZE['body'], ready=True) | {'align': 'right', 'color': COLORS['title']},
               {'type': 'image', 'rect': [1164, 18, 56, 56], 'bind': 'live.moon_spr',
                'src_format': 'module:p3p:spr/%d@56', 'need_bind': 'live.moon_spr'}]
    for i in range(4):  # the active party (S+0x4b90), in slot order
        p = f'party.{i}'; y = 108 + i * 122
        need = {'need_bind': p + '.present'}
        widgets += [fill([36, y, 660, 112], 'panel', **need),
                    label([56, y + 8, 620, 40], bind=p + '.name', scale=SIZE['value']) | need,
                    label([676, y + 12, 0, 32], bind=p + '.ailment') | need | {'align': 'right', 'color': COLORS['accent']},
                    label([56, y + 52, 620, 32], bind=p + '.stats', scale=SIZE['body']) | need | {'color': COLORS['muted']}]
        widgets += bars(p, 56, y + 90, 620, need)
    for i, (caption, page) in enumerate(MENU):
        wide = i == len(MENU) - 1 and len(MENU) % 2
        rect = [36 + (i % 2) * 600, 616 + (i // 2) * 92, 1188 if wide else 588, 84]
        widgets += button(rect, caption, f'ui.open.{page}')
    return widgets

SKILL_ROWS = 96  # 0x108770 asserts fewer than 0x61 entries

def member_buttons():
    """Member buttons (two rows of five) selecting ui.member; shared by Skills, Equip and Stats."""
    widgets = []
    for i in range(10):
        p = f'member.{i}'
        rect = [36 + (i % 5) * 240, 104 + (i // 5) * 74, 230, 64]
        need = {'need_bind': p + '.present'}
        widgets += [fill(rect, 'select', **need, hide_bind=p + '.selected', hide_eq=0),
                    fill(rect, 'panel', **need, hide_bind=p + '.selected', hide_eq=1),
                    {'type': 'button', 'rect': rect, 'text': '', 'on_tap': f'member.select.{i}', 'bg': '#00000000',
                     'border': 0} | need,
                    *[label([rect[0] + rect[2] // 2, rect[1], 0, rect[3]], bind=p + '.tab', scale=SIZE['body'])
                      | {'align': 'center', 'text_center_h': rect[3], 'fit_text': True,
                         'wrap_width': rect[2] - 16, 'text_min_scale': SIZE['small']} | gate
                      for gate in ({'need_bind': p + '.present', 'hide_bind': p + '.selected', 'hide_eq': 1},
                                   {'need_bind': p + '.selected', 'color': COLORS['light']})]]
    return widgets

def header(bind, ready):
    """The selected member line under the member buttons."""
    return label([44, 258, 1192, 36], bind=bind) | {'need_bind': ready, 'color': COLORS['accent']}

def equip_widgets(outputs):
    """The selected member's four slots with the native Status panel's figures."""
    widgets = member_buttons()
    outputs += ['equip.ready', 'equip.header']
    ready = {'need_bind': 'equip.ready'}
    widgets.append(header('equip.header', 'equip.ready'))
    for i, slot in enumerate(('weapon', 'body', 'feet', 'accessory')):  # native Equip lettering
        p = f'equip.{i}'; outputs += [p + '.name', p + '.stats']
        y = 306 + i * 166
        widgets += [fill([36, y, 1188, 154], 'panel', **ready),
                    sprite(slot, 40, y + 14, 36, tint=COLORS['accent']) | ready,
                    label([56, y + 66, 1160, 44], bind=p + '.name', scale=SIZE['value']) | ready,
                    label([1200, y + 14, 0, 32], bind=p + '.stats') | ready | {'color': COLORS['muted'], 'align': 'right'}]
    return widgets

ITEM_ROWS = 256  # the module publishes at most this many rows

def items_widgets(outputs, scrolls):
    """Inventory as the native camp Item tabs list it (Use / Equip / Key), with counts."""
    outputs += ['item.ready', 'item.count', 'item.header', 'item.tab']
    widgets = []
    for i, caption in enumerate(('Use', 'Equip', 'Key')):
        widgets += toggle([36 + i * 288, 104, 276, 76], caption, f'item.tab.{i}', 'item.tab', i, SIZE['value'])
    widgets.append(label([1216, 104, 0, 76], bind='item.header', scale=SIZE['value'])
                   | {'need_bind': 'item.ready', 'align': 'right', 'text_center_h': 76, 'color': COLORS['accent']})
    scrolls.append({'id': 'items', 'rect': [36, 196, 1188, 772], 'count_bind': 'item.count', 'row_h': 86,
                    'bar': THEME['line'][0], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'item.ready',
                    'reset_bind': 'item.tab'})
    outputs += [f'item.{i}.{k}' for i in range(ITEM_ROWS) for k in ('present', 'name', 'count')]
    row = {'repeat': ITEM_ROWS, 'repeat_dy': 86, 'scroll': 'items'}
    need = {'need_bind': 'item.{i}.present'}
    widgets += [dict(row, **fill([36, 196, 1164, 76], 'panel'), **need),
                dict(row, **label([56, 196, 900, 76], bind='item.{i}.name', scale=SIZE['value']), **need) | {'text_center_h': 76},
                dict(row, **label([1176, 196, 0, 76], bind='item.{i}.count', scale=SIZE['value']), **need)
                | {'color': COLORS['muted'], 'align': 'right', 'text_center_h': 76}]
    return widgets

def personas_widgets(outputs):
    """Stock list (tap to pick) and the native PERSONA -> Details panel for the pick, with the
    Persona's camp artwork and the game's element icons."""
    widgets = []
    for i in range(12):
        p = f'persona.{i}'; outputs += [p + '.present', p + '.line', p + '.picked']
        rect = [36, 104 + i * 72, 392, 64]
        need = {'need_bind': p + '.present'}
        widgets += [fill(rect, 'select', **need, hide_bind=p + '.picked', hide_eq=0),
                    fill(rect, 'panel', **need, hide_bind=p + '.picked', hide_eq=1),
                    {'type': 'button', 'rect': rect, 'text': '', 'on_tap': f'persona.select.{i}', 'bg': '#00000000',
                     'border': 0} | need,
                    *[label([52, rect[1], 360, 64], bind=p + '.line')
                      | {'text_center_h': 64, 'fit_text': True, 'wrap_width': 360, 'text_min_scale': SIZE['small']} | gate
                      for gate in ({'need_bind': p + '.present', 'hide_bind': p + '.picked', 'hide_eq': 1},
                                   {'need_bind': p + '.picked', 'color': COLORS['light']})]]
    outputs += ['pd.ready', 'pd.id', 'pd.name', 'pd.arcana', 'pd.next', 'pd.card', 'pd.nextskill', 'ui.persona']
    x, w = 448, 768
    ready = {'need_bind': 'pd.ready'}
    widgets += [{'type': 'image', 'rect': [x - 36, 176, 360, 360], 'bind': 'pd.id',
                 'src_format': 'module:p3p:persona/%d@360'} | ready,
                label([x, 104, w, 44], bind='pd.name', scale=SIZE['value']) | ready,
                label([x, 150, w, 32], bind='pd.arcana') | ready | {'color': COLORS['accent']}]
    sx = x + 328  # stats beside the artwork
    for s_, name in enumerate(('St', 'Ma', 'En', 'Ag', 'Lu')):
        y = 206 + s_ * 58; outputs.append(f'pd.stat.{s_}')
        widgets += [label([sx, y, 60, 36], name, scale=SIZE['value']) | ready | {'color': COLORS['accent']},
                    {'type': 'bar', 'rect': [sx + 70, y + 8, 270, 22], 'bind': f'pd.stat.{s_}', 'max': 99,
                     'color': SP, 'bg': BAR_BG, 'frame': 0} | ready,
                    {'type': 'value', 'rect': [x + w, y, 0, 36], 'bind': f'pd.stat.{s_}', 'text_scale': SIZE['value'],
                     'align': 'right', 'color': COLORS['ink']} | ready]
    cell = w // 9
    for e, n in enumerate(ELEMENTS):
        outputs.append(f'pd.aff.{e}')
        cx = x + e * cell
        widgets += [fill([cx, 524, cell - 6, 96], 'panel', **ready),
                    {'type': 'image', 'rect': [cx + (cell - 6 - 50) // 2, 532, 50, 40],
                     'src': f'module:p3p:spr/{n}@40', 'tint': COLORS['ink']} | ready,
                    label([cx + (cell - 6) // 2, 578, 0, 32], bind=f'pd.aff.{e}') | ready
                    | {'color': COLORS['accent'], 'align': 'center'}]
    widgets.append(label([x, 636, w, 32], bind='pd.next') | ready)
    for k in range(8):
        outputs.append(f'pd.skill.{k}')
        # Native Details order: slots 0-3 down the left column, 4-7 down the right.
        widgets.append(label([x + (k // 4) * 390, 684 + (k % 4) * 46, 378, 32], bind=f'pd.skill.{k}') | ready)
    widgets += [label([x, 874, w, 32], bind='pd.card') | ready,
                label([x, 916, w, 32], bind='pd.nextskill') | ready | {'color': COLORS['muted']}]
    return widgets

def links_widgets(outputs, scrolls):
    """The native S.LINK list: rank, arcana, link name, partner, rank pips and status."""
    rows = 28  # one link per arcana at most (0x1d4310 walks arcana 0..27)
    outputs += ['link.ready', 'link.count']
    scrolls.append({'id': 'links', 'rect': [36, 104, 1188, 864], 'count_bind': 'link.count', 'row_h': 110,
                    'bar': THEME['line'][0], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'link.ready'})
    row = {'repeat': rows, 'repeat_dy': 110, 'scroll': 'links'}
    need = {'need_bind': 'link.{i}.present'}
    outputs += [f'link.{i}.{k}' for i in range(rows)
                for k in ('present', 'arcana', 'name', 'partner', 'rank', 'rank_text', 'status')]
    return [dict(row, **fill([36, 104, 1164, 100], 'panel'), **need),
            dict(row, **label([56, 116, 270, 40], bind='link.{i}.arcana', scale=SIZE['value']), **need) | {'color': COLORS['accent']},
            dict(row, **label([56, 162, 270, 32], bind='link.{i}.rank_text'), **need),
            dict(row, **label([330, 116, 520, 40], bind='link.{i}.name', scale=SIZE['value']), **need)
            | {'fit_text': True, 'wrap_width': 520, 'text_min_scale': SIZE['body']},
            dict(row, **label([330, 162, 520, 32], bind='link.{i}.partner'), **need) | {'color': COLORS['muted']},
            dict(row, type='pips', rect=[850, 128, 330, 22], bind='link.{i}.rank', max=10,
                 color=COLORS['accent'], bg=BAR_BG, **need),
            dict(row, **label([870, 162, 310, 32], bind='link.{i}.status'), **need) | {'color': COLORS['accent']}]

def battle_page(outputs):
    """Shown automatically during battles: party HP/SP and the encounter's enemies, with
    affinities only once analyzed (as the native Analyze panel)."""
    outputs += ['battle.active', 'en.count']
    widgets = [{'type': 'image', 'rect': [0, 0, 1240, 1080], 'bind': 'ui.route', 'src_format': 'module:p3p:bg/%d'},
               label([44, 20, 760, 48], 'BATTLE', scale=SIZE['title']) | {'color': COLORS['title']},
               label([1216, 26, 0, 40], bind='live.date') | {'align': 'right', 'color': COLORS['light']}]
    for i in range(4):
        p = f'party.{i}'; y = 102 + i * 80
        need = {'need_bind': p + '.present'}
        widgets += [fill([36, y, 1188, 72], 'panel', **need),
                    label([56, y + 6, 560, 36], bind=p + '.name') | need,
                    label([420, y + 6, 360, 36], bind=p + '.ailment') | need | {'color': COLORS['accent']},
                    label([1200, y + 6, 0, 36], bind=p + '.stats') | need | {'align': 'right', 'color': COLORS['muted']}]
        widgets += bars(p, 56, y + 50, 1140, need)
    widgets.append(label([44, 430, 400, 36], 'Enemies') | {'color': COLORS['accent']})
    for e, n in enumerate(ELEMENTS):  # one header row of element icons over the affinity columns
        widgets.append({'type': 'image', 'rect': [760 + e * 50 - 20, 430, 40, 32], 'src': f'module:p3p:spr/{n}@32',
                        'tint': COLORS['ink'], 'need_bind': 'en.0.present'})
    for i in range(5):
        p = f'en.{i}'; y = 474 + i * 100
        outputs += [p + '.' + k for k in ('present', 'name', 'info', 'hp_text', 'hp', 'max_hp', 'analyzed', 'ailment')]
        need = {'need_bind': p + '.present'}
        widgets += [fill([36, y, 1188, 92], 'panel', **need),
                    label([56, y + 8, 420, 40], bind=p + '.name', scale=SIZE['value']) | need
                    | {'fit_text': True, 'wrap_width': 420, 'text_min_scale': SIZE['body']},
                    label([56, y + 54, 420, 32], bind=p + '.info') | need | {'color': COLORS['muted']},
                    label([466, y + 54, 0, 32], bind=p + '.ailment') | need | {'color': COLORS['accent'], 'align': 'right'},
                    label([476, y + 8, 250, 32], bind=p + '.hp_text') | need,
                    {'type': 'bar', 'rect': [476, y + 56, 240, 16], 'bind': p + '.hp', 'max_bind': p + '.max_hp',
                     'color': HP, 'bg': BAR_BG, 'frame': 0} | need]
        for k in range(9):
            outputs.append(f'{p}.aff.{k}')
            widgets.append(label([760 + k * 50, y + 30, 0, 32], bind=f'{p}.aff.{k}') | need
                           | {'color': COLORS['accent'], 'align': 'center'})
    widgets.append(label([44, 1012, 1192, 32], bind='status', scale=SIZE['small']) | {'color': COLORS['muted']})
    return {'id': 'battle', 'title': 'BATTLE', 'widgets': widgets}

def calendar_widgets(outputs):
    """Month grid with today, holidays and full moons; previous/next month buttons. The month,
    weekdays and moon are the camp calendar's own lettering."""
    outputs += ['cal.ready', 'cal.title', 'cal.next', 'cal.month_spr']
    ready = {'need_bind': 'cal.ready'}
    widgets = (button([36, 104, 220, 72], 'Prev', 'calendar.prev', scale=SIZE['value'], **ready)
               + button([1004, 104, 220, 72], 'Next', 'calendar.next', scale=SIZE['value'], **ready)
               + [{'type': 'image', 'rect': [620 - 186, 110, 372, 60], 'bind': 'cal.month_spr',
                   'src_format': 'module:p3p:spr/%d@60', 'tint': ACCENT, 'need_bind': 'cal.month_spr'}])
    for i, n in enumerate(WEEKDAYS):
        widgets.append({'type': 'image', 'rect': [36 + i * 170 + 81 - 48, 190, 95, 28], 'src': f'module:p3p:spr/{n}@28',
                        'tint': COLORS['accent'] if i == 0 else COLORS['muted']} | ready)
    for c in range(42):
        p = f'cal.{c}'; outputs += [p + '.' + k for k in ('present', 'text', 'today', 'holiday', 'full')]
        x, y = 36 + (c % 7) * 170, 228 + (c // 7) * 114
        need = {'need_bind': p + '.present'}
        widgets += [fill([x, y, 162, 106], 'panel', **need),
                    fill([x, y, 162, 106], 'select', need_bind=p + '.today'),
                    label([x + 14, y + 10, 140, 44], bind=p + '.text', scale=SIZE['value']) | need
                    | {'hide_bind': p + '.today', 'hide_eq': 1},
                    label([x + 14, y + 10, 140, 44], bind=p + '.text', scale=SIZE['value'])
                    | {'need_bind': p + '.holiday', 'hide_bind': p + '.today', 'hide_eq': 1, 'color': COLORS['accent']},
                    label([x + 14, y + 10, 140, 44], bind=p + '.text', scale=SIZE['value'])
                    | {'need_bind': p + '.today', 'color': COLORS['light']},
                    {'type': 'image', 'rect': [x + 112, y + 56, 44, 44], 'src': f'module:p3p:spr/{FULL_MOON}@44',
                     'tint': ACCENT, 'need_bind': p + '.full'}]
    widgets.append(label([44, 926, 1192, 40], bind='cal.next') | ready)
    return widgets

def stats_widgets(outputs):
    """Active Persona stats (accessory part shown separately), next EXP, and the
    protagonist's social stat ranks."""
    widgets = member_buttons()
    outputs += ['stat.ready', 'stat.header', 'stat.next', 'social.ready']
    ready = {'need_bind': 'stat.ready'}
    widgets.append(header('stat.header', 'stat.ready'))
    widgets.append(fill([36, 302, 1188, 362], 'panel', **ready))
    for i, name in enumerate(('St', 'Ma', 'En', 'Ag', 'Lu')):
        p = f'stat.{i}'; outputs += [p + '.value', p + '.bonus']
        y = 318 + i * 60
        widgets += [label([48, y, 90, 40], name, scale=SIZE['value']) | ready | {'color': COLORS['accent']},
                    {'type': 'bar', 'rect': [150, y + 8, 680, 26], 'bind': p + '.value', 'max': 99,
                     'color': SP, 'bg': BAR_BG, 'frame': 0} | ready,
                    {'type': 'value', 'rect': [920, y, 0, 40], 'bind': p + '.value', 'text_scale': SIZE['value'],
                     'align': 'right', 'color': COLORS['ink']} | ready,
                    label([944, y, 260, 40], bind=p + '.bonus', scale=SIZE['value']) | ready | {'color': COLORS['accent']}]
    widgets.append(label([48, 614, 800, 40], bind='stat.next', scale=SIZE['value']) | ready)
    for i, name in enumerate(('Academics', 'Charm', 'Courage')):
        p = f'social.{i}'; outputs += [p + '.rank', p + '.name']
        y = 690 + i * 92
        gate = {'need_bind': 'social.ready'}
        widgets += [fill([36, y - 16, 1188, 76], 'panel', **gate),
                    label([48, y, 320, 40], name, scale=SIZE['value']) | gate | {'color': COLORS['accent']},
                    label([380, y, 420, 40], bind=p + '.name', scale=SIZE['value']) | gate,
                    {'type': 'pips', 'rect': [820, y + 4, 380, 32], 'bind': p + '.rank', 'max': 6,
                     'color': COLORS['accent'], 'bg': BAR_BG} | gate]
    return widgets

def skills_widgets(outputs, scrolls):
    """Member buttons and the selected member's native Skill list."""
    widgets = member_buttons()
    outputs += ['skill.ready', 'skill.count', 'skill.header']
    widgets.append(header('skill.header', 'skill.ready'))
    scrolls.append({'id': 'skills', 'rect': [36, 304, 1188, 664], 'count_bind': 'skill.count', 'row_h': 83,
                    'bar': THEME['line'][0], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'skill.ready',
                    'reset_bind': 'ui.member'})
    outputs += [f'skill.{i}.{k}' for i in range(SKILL_ROWS) for k in ('present', 'name', 'cost', 'icon')]
    row = {'repeat': SKILL_ROWS, 'repeat_dy': 83, 'scroll': 'skills'}
    widgets.append(dict(row, **fill([36, 304, 1164, 74], 'panel'), need_bind='skill.{i}.present'))
    widgets.append(dict(row, type='image', rect=[52, 317, 48, 48], bind='skill.{i}.icon',
                        src_format='module:p3p:spr/%d@48', tint=ACCENT, need_bind='skill.{i}.icon'))
    widgets.append(dict(row, **label([116, 304, 780, 74], bind='skill.{i}.name', scale=SIZE['value']),
                        need_bind='skill.{i}.present') | {'text_center_h': 74})
    widgets.append(dict(row, **label([1176, 304, 0, 74], bind='skill.{i}.cost', scale=SIZE['value']),
                        need_bind='skill.{i}.present') | {'color': COLORS['muted'], 'align': 'right', 'text_center_h': 74})
    return widgets

MAP_X, MAP_Y, MAP_SIZE = 188, 104, 864  # p3p_map.h Size

def map_widgets(outputs):
    """The current Tartarus floor as the game's minimap has revealed it (module picture), with
    the leader's arrow moved and turned by the module (map.px / map.py / map.rot)."""
    outputs += ['map.ready', 'map.key', 'map.why', 'map.px', 'map.py', 'map.rot', 'map.floor']
    ready = {'need_bind': 'map.ready'}
    size = MAP_SIZE

    def icon(p, sprite, h, tint=None):
        """A minimap-sheet icon (p3p_art mm/<n>) centred on the module's map position p.x/p.y."""
        outputs.extend([p + '.on', p + '.x', p + '.y'])
        w = {'type': 'image', 'rect': [MAP_X - h // 2, MAP_Y - h // 2, h, h], 'src': f'module:p3p:mm/{sprite}@{h}',
             'x_bind': p + '.x', 'y_bind': p + '.y', 'need_bind': p + '.on'}
        return w | ({'tint': tint} if tint else {})
    # The game's minimap icons: the stairs, chests and teleporter in the route's dark colour (they
    # are white on the game's dark minimap), Shadows (orange) and the wandering marker (green).
    icons = [icon('map.stairs', 4, 64, ACCENT), icon('map.tele', 5, 64, ACCENT), icon('map.wander', 12, 80)]
    icons += [icon(f'map.chest.{i}', 10, 64, ACCENT) for i in range(32)]
    icons += [icon(f'map.shadow.{i}', 9, 80) for i in range(24)]
    return [label([170, 20, 300, 48], bind='map.floor', scale=SIZE['title']) | ready | {'color': COLORS['light']},
            fill([MAP_X - 8, MAP_Y - 8, size + 16, size + 16], 'select', **ready),
            {'type': 'image', 'rect': [MAP_X, MAP_Y, size, size], 'src_bind': 'map.key'} | ready,
            *icons,  # each gated on its own .on, published only with map.ready
            # Sprite 107 is a right-pointing triangle; -90 turns it to north for heading 0.
            {'type': 'image', 'rect': [MAP_X - 22, MAP_Y - 22, 44, 44], 'src': 'module:p3p:spr/107@44',
             'x_bind': 'map.px', 'y_bind': 'map.py', 'rotate': -90, 'rotate_bind': 'map.rot',
             'tint': COLORS['title']} | ready,
            # Outside Tartarus: why there is no map (once the game itself is readable).
            {'type': 'label', 'rect': [620, 470, 0, 60], 'text': '', 'bind_text': 'map.why', 'align': 'center',
             'wrap_width': 1100, 'text_scale': SIZE['value'], 'color': COLORS['muted'],
             'need_bind': 'live.ready', 'hide_bind': 'map.ready', 'hide_eq': 1}]

def main():
    outputs = ['live.ready', 'live.date', 'live.money', 'status', 'ui.page', 'ui.member']
    outputs += [f'member.{i}.{k}' for i in range(10)
                for k in ('present', 'name', 'tab', 'selected', 'stats', 'hp', 'sp', 'max_hp', 'max_sp')]
    outputs += [f'party.{i}.{k}' for i in range(4)
                for k in ('present', 'name', 'stats', 'hp', 'sp', 'max_hp', 'max_sp', 'ailment')]
    # Tabs tell the module which page is open (ui_open); page_binds follow its ui.page, so
    # heavy lists are read only while their page shows.
    # Titles name the camp menu's lettering for each page (SPRITES).
    tabs = (('home', 'MAIN', 'Status'), ('personas', 'PERSONA', 'Personas'),
            ('links', 'S.LINK', 'S.Link'), ('skills', 'SKILL', 'Skills'),
            ('equip', 'EQUIP', 'Equip'), ('stats', 'STATUS', 'Stats'), ('items', 'ITEM', 'Items'),
            ('calendar', 'CALENDAR', 'Calendar'))
    # Module page numbers: tabs are 1-8 in order, 9 is the battle page, 10 the map.
    numbered = [(n, page_id) for n, (page_id, _, _) in enumerate(tabs, 1)] + [(10, 'map')]
    actions = {f'ui.open.{n}': {'kind': 'module', 'action': 'ui_open', 'argument': n} for n, _ in numbered}
    actions.update({f'member.select.{i}': {'kind': 'module', 'action': 'member_select', 'argument': i}
                    for i in range(10)})
    actions.update({f'item.tab.{i}': {'kind': 'module', 'action': 'item_tab', 'argument': i} for i in range(3)})
    actions.update({f'persona.select.{i}': {'kind': 'module', 'action': 'persona_select', 'argument': i} for i in range(12)})
    actions['calendar.prev'] = {'kind': 'module', 'action': 'calendar_shift', 'argument': 0}
    actions['calendar.next'] = {'kind': 'module', 'action': 'calendar_shift', 'argument': 1}
    page_binds = [{'point': 'ui.page', 'equals': n, 'when_equal': {'page': page_id}} for n, page_id in numbered]
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
    pages.append({'id': 'map', 'title': 'MAP', 'widgets': chrome('MAP') + map_widgets(outputs)})
    for page in pages:
        page['widgets'] = split_route(page['widgets'])
    result = {'format': 1, 'title_id': '0100DCD01525A000', 'name': 'Persona 3 Portable - Read-only Preview',
              'min_runtime': 19, 'requires_module': True, 'poll_hz': 60,
              'canvas_w': 1240, 'canvas_h': 1080, 'background': '#FFFFB8CE',
              # The game font from the player's romfs (module decode_font/load_image, p3p_font.h);
              # p3p_font.txt is a marker holding no game data.
              'font': 'file:p3p_font.txt', 'font_atlas': 'module:p3p_font',
              'module_outputs': outputs, 'module': {'abi': 1, 'build_ids': [BUILD], 'libraries': {}},
              'actions': actions, 'page_binds': page_binds, 'pages': pages}
    dest = ROOT / 'packages/Persona3Portable/dualscreen/manifest.json'
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(result, indent=2) + '\n')

if __name__ == '__main__': main()
