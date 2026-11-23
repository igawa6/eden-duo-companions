#!/usr/bin/env python3
"""Generate the asset-free Persona 3 Portable companion layout."""
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

def drive_bar(outputs):
    """While the game's menu is being driven (item use, equipment), and for a few seconds after:
    what happened."""
    outputs += ['drive.busy', 'drive.show', 'drive.text']
    show = {'need_bind': 'drive.show'}
    return [fill([36, 860, 1188, 108], 'select', **show),
            label([620, 860, 0, 108], bind='drive.text', scale=SIZE['value'])
            | show | {'align': 'center', 'text_center_h': 108, 'color': COLORS['light']}]

def equip_widgets(outputs, scrolls):
    """The selected member's four slots with the native Status panel's figures. Tapping a slot
    lists what the game's Equip screen would offer; tapping one equips it through that screen."""
    widgets = member_buttons()
    outputs += ['equip.ready', 'equip.header']
    ready = {'need_bind': 'equip.ready'}
    widgets.append(header('equip.header', 'equip.ready'))
    for i, slot in enumerate(('weapon', 'body', 'feet', 'accessory')):  # native Equip lettering
        p = f'equip.{i}'; outputs += [p + '.name', p + '.stats', p + '.help']
        y = 306 + i * 166
        widgets += [fill([36, y, 1188, 154], 'panel', **ready),
                    sprite(slot, 40, y + 14, 36, tint=COLORS['accent']) | ready,
                    label([56, y + 56, 1160, 44], bind=p + '.name', scale=SIZE['value']) | ready,
                    label([56, y + 108, 1160, 32], bind=p + '.help', scale=SIZE['small']) | ready | {'color': COLORS['muted']},
                    label([1200, y + 14, 0, 32], bind=p + '.stats') | ready | {'color': COLORS['muted'], 'align': 'right'},
                    {'type': 'image', 'rect': [36, y, 1188, 154], 'src': 'module:p3p:fill', 'tint': '#00000000',
                     'on_tap': 'equip.pick', 'payload': str(i), 'need_bind': 'equip.can_pick'}]
    # The choices, over the slots (the game's own list for that member and slot).
    outputs += ['equip.can_pick', 'equip.picked', 'eqc.on', 'eqc.count', 'eqc.title']
    outputs += [f'eqc.{i}.{k}' for i in range(64) for k in ('present', 'name', 'count', 'worn')]
    on = {'need_bind': 'eqc.on'}
    widgets += [fill([36, 296, 1188, 676], 'select', **on),
                label([60, 312, 1140, 44], bind='eqc.title', scale=SIZE['value']) | on | {'color': COLORS['light']}]
    scrolls.append({'id': 'eqc', 'rect': [56, 372, 1148, 480], 'count_bind': 'eqc.count', 'row_h': 96,
                    'bar': '#FFFFFFFF', 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'eqc.on', 'reset_bind': 'equip.picked'})
    row = {'repeat': 64, 'repeat_dy': 96, 'scroll': 'eqc'}
    need = {'need_bind': 'eqc.{i}.present'}
    widgets += [dict(row, **fill([56, 372, 1124, 86], 'panel'), **need),
                dict(row, **label([80, 372, 800, 86], bind='eqc.{i}.name', scale=SIZE['value']), **need) | {'text_center_h': 86},
                dict(row, **label([1160, 372, 0, 86], bind='eqc.{i}.count', scale=SIZE['body']), **need)
                | {'text_center_h': 86, 'align': 'right', 'color': COLORS['muted']},
                dict(row, type='image', rect=[56, 372, 1124, 86], src='module:p3p:fill', tint='#00000000',
                     on_tap='equip.choose', payload='{i}', **need)]
    widgets += button([924, 872, 276, 84], 'Cancel', 'equip.cancel', SIZE['value'], **on)
    widgets += drive_bar(outputs)
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
    scrolls.append({'id': 'items', 'rect': [36, 196, 1188, 772], 'count_bind': 'item.count', 'row_h': 104,
                    'bar': THEME['line'][0], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'item.ready',
                    'reset_bind': 'item.tab'})
    # Each row: name and count, with the game's help line under them (native Item menu text).
    outputs += [f'item.{i}.{k}' for i in range(ITEM_ROWS) for k in ('present', 'name', 'count', 'help')]
    row = {'repeat': ITEM_ROWS, 'repeat_dy': 104, 'scroll': 'items'}
    need = {'need_bind': 'item.{i}.present'}
    widgets += [dict(row, **fill([36, 196, 1164, 96], 'panel'), **need),
                dict(row, **label([56, 202, 900, 44], bind='item.{i}.name', scale=SIZE['value']), **need),
                dict(row, **label([56, 252, 1120, 32], bind='item.{i}.help', scale=SIZE['small']), **need)
                | {'color': COLORS['muted']},
                dict(row, **label([1176, 202, 0, 44], bind='item.{i}.count', scale=SIZE['value']), **need)
                | {'color': COLORS['muted'], 'align': 'right'}]
    # Use tab: tap an item, then who to use it on; the module walks the game's camp menu to do it.
    outputs += ['item.can_pick', 'item.picked', 'item.pick.on', 'item.pick.name']
    outputs += [f'use.{k}.{f}' for k in range(4) for f in ('present', 'name')]
    widgets += [dict(row, **fill([36, 196, 14, 96], 'select'), need_bind='item.pick.on', hide_bind='item.picked',
                     keep_min='{i}', keep_max='{i}'),
                dict(row, type='image', rect=[36, 196, 1164, 96], src='module:p3p:fill', tint='#00000000',
                     on_tap='item.pick', payload='{i}', need_bind='item.can_pick')]
    panel = {'need_bind': 'item.pick.on'}
    widgets += [fill([36, 776, 1188, 192], 'select', **panel),
                label([60, 790, 1140, 44], bind='item.pick.name', scale=SIZE['value']) | panel | {'color': COLORS['light']}]
    for k in range(4):
        widgets += button([60 + k * 232, 848, 220, 100], '', f'item.use.{k}', SIZE['value'], need_bind=f'use.{k}.present')
        widgets.append(label([170 + k * 232, 848, 0, 100], bind=f'use.{k}.name', scale=SIZE['value'])
                       | {'need_bind': f'use.{k}.present', 'align': 'center', 'text_center_h': 100, 'fit_text': True,
                          'wrap_width': 200, 'text_min_scale': SIZE['body']})
    widgets += button([996, 848, 204, 100], 'Cancel', 'item.cancel', SIZE['value'], **panel)
    widgets += drive_bar(outputs)
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
    widgets += [label([x, 874, w - 360, 32], bind='pd.card') | ready | {'fit_text': True, 'wrap_width': w - 360, 'text_min_scale': SIZE['small']},
                label([x, 916, w - 360, 32], bind='pd.nextskill') | ready | {'color': COLORS['muted']}]
    # Switch to the picked Persona through the game's own PERSONA screen (p3p_drive.h).
    outputs.append('persona.can_use')
    widgets += button([x + w - 340, 868, 340, 88], 'Use this Persona', 'persona.use', SIZE['value'], need_bind='persona.can_use')
    widgets += drive_bar(outputs)
    return widgets

def links_widgets(outputs, scrolls):
    """The native S.LINK list: rank, arcana, link name, partner, rank pips, status and the
    Details screen's line for the rank."""
    rows = 28  # one link per arcana at most (0x1d4310 walks arcana 0..27)
    outputs += ['link.ready', 'link.count']
    scrolls.append({'id': 'links', 'rect': [36, 104, 1188, 864], 'count_bind': 'link.count', 'row_h': 300,
                    'bar': THEME['line'][0], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'link.ready'})
    row = {'repeat': rows, 'repeat_dy': 300, 'scroll': 'links'}
    need = {'need_bind': 'link.{i}.present'}
    outputs += [f'link.{i}.{k}' for i in range(rows)
                for k in ('present', 'arcana', 'name', 'partner', 'rank', 'rank_text', 'status', 'help', 'people')]
    return [dict(row, **fill([36, 104, 1164, 290], 'panel'), **need),
            dict(row, **label([56, 116, 270, 40], bind='link.{i}.arcana', scale=SIZE['value']), **need) | {'color': COLORS['accent']},
            dict(row, **label([56, 162, 270, 32], bind='link.{i}.rank_text'), **need),
            dict(row, **label([330, 116, 520, 40], bind='link.{i}.name', scale=SIZE['value']), **need)
            | {'fit_text': True, 'wrap_width': 520, 'text_min_scale': SIZE['body']},
            dict(row, **label([330, 162, 520, 32], bind='link.{i}.partner'), **need) | {'color': COLORS['muted']},
            dict(row, type='pips', rect=[850, 128, 330, 22], bind='link.{i}.rank', max=10,
                 color=COLORS['accent'], bg=BAR_BG, **need),
            dict(row, **label([870, 162, 310, 32], bind='link.{i}.status'), **need) | {'color': COLORS['accent']},
            # The native Details line for the link's rank (cmmHelp[F].bmd).
            dict(row, **label([56, 204, 1120, 28], bind='link.{i}.help', scale=SIZE['small']), **need)
            | {'color': COLORS['muted'], 'fit_text': True, 'wrap_width': 1120, 'text_min_scale': SIZE['small']},
            # The members' profiles (one per line, as the Details screen lists them).
            dict(row, **label([56, 244, 1120, 144], bind='link.{i}.people', scale=SIZE['small']), **need)
            | {'wrap_width': 1120, 'max_lines': 4, 'line_gap': 10}]

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
    widgets.append(label([44, 1012, 1192, 32], bind='status', scale=SIZE['small'], ready=True) | {'color': COLORS['muted']})
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
    scrolls.append({'id': 'skills', 'rect': [36, 304, 1188, 664], 'count_bind': 'skill.count', 'row_h': 104,
                    'bar': THEME['line'][0], 'bar_track': '#33FFFFFF', 'bar_w': 8, 'show_bind': 'skill.ready',
                    'reset_bind': 'ui.member'})
    outputs += [f'skill.{i}.{k}' for i in range(SKILL_ROWS) for k in ('present', 'name', 'cost', 'icon', 'help')]
    row = {'repeat': SKILL_ROWS, 'repeat_dy': 104, 'scroll': 'skills'}
    widgets.append(dict(row, **fill([36, 304, 1164, 96], 'panel'), need_bind='skill.{i}.present'))
    widgets.append(dict(row, **label([116, 360, 1060, 32], bind='skill.{i}.help', scale=SIZE['small']),
                        need_bind='skill.{i}.present') | {'color': COLORS['muted']})
    widgets.append(dict(row, type='image', rect=[52, 310, 48, 48], bind='skill.{i}.icon',
                        src_format='module:p3p:spr/%d@48', tint=ACCENT, need_bind='skill.{i}.icon'))
    widgets.append(dict(row, **label([116, 310, 780, 44], bind='skill.{i}.name', scale=SIZE['value']),
                        need_bind='skill.{i}.present'))
    widgets.append(dict(row, **label([1176, 310, 0, 44], bind='skill.{i}.cost', scale=SIZE['value']),
                        need_bind='skill.{i}.present') | {'color': COLORS['muted'], 'align': 'right'})
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

def talk_page(outputs):
    """Shown automatically while the game shows a choice list (module ui.page 12): who is
    speaking, the current line, and the options with the game's cursor. The player
    can tap an option to move the game's cursor to it and tap it again to choose; text the module
    cannot reproduce exactly is left out, and then only the main screen chooses."""
    outputs += ['talk.ready', 'talk.text_ok', 'talk.text', 'talk.has_speaker', 'talk.speaker', 'talk.choosing',
                'talk.choices_ok', 'talk.why', 'talk.selected', 'controls.choice']
    widgets = [{'type': 'image', 'rect': [0, 0, 1240, 1080], 'bind': 'ui.route', 'src_format': 'module:p3p:bg/%d'},
               label([44, 20, 760, 48], 'TALK', scale=SIZE['title']) | {'color': COLORS['title']},
               label([1216, 26, 0, 40], bind='live.date', ready=True) | {'align': 'right', 'color': COLORS['light']},
               fill([36, 100, 1188, 300], 'panel', need_bind='talk.ready'),
               label([72, 124, 1100, 40], bind='talk.speaker', scale=SIZE['value'])
               | {'need_bind': 'talk.has_speaker', 'color': COLORS['accent']},
               label([72, 184, 1100, 200], bind='talk.text', scale=SIZE['value'])
               | {'need_bind': 'talk.text_ok', 'wrap_width': 1100, 'max_lines': 4, 'line_gap': 18},
               # Choices the module cannot spell out (names, numbers it does not reproduce).
               label([620, 600, 0, 48], 'Choose on the main screen', scale=SIZE['button'])
               | {'align': 'center', 'need_bind': 'talk.choosing', 'hide_bind': 'talk.choices_ok', 'hide_eq': 1}]
    for i in range(5):  # the game's selections have at most five options
        p = f'choice.{i}'; y = 430 + i * 108
        outputs += [p + '.present', p + '.text', p + '.selected']
        need = {'need_bind': p + '.present'}
        text = label([72, y, 1100, 96], bind=p + '.text', scale=SIZE['button']) | need | {
            'text_center_h': 96, 'fit_text': True, 'wrap_width': 1100, 'max_lines': 1, 'text_min_scale': SIZE['body']}
        widgets += [fill([36, y, 1188, 96], 'panel', **need),
                    fill([36, y, 1188, 96], 'select', need_bind=p + '.selected'),
                    text | {'hide_bind': p + '.selected', 'hide_eq': 1},
                    text | {'need_bind': p + '.selected', 'color': COLORS['light']}]
        # Tap targets, one per position of the game's cursor (talk.selected): tapping another
        # option moves the cursor there, tapping the highlighted one chooses it (A).
        for c in range(5):
            widgets.append({'type': 'image', 'rect': [36, y, 1188, 96], 'src': 'module:p3p:fill', 'tint': '#00000000',
                            'on_tap': 'choice.confirm' if c == i else f'choice.move.{i - c}',
                            'need_bind': 'controls.choice', 'hide_bind': 'talk.selected', 'keep_min': c, 'keep_max': c})
    widgets.append(label([620, 978, 0, 28], 'Tap an option to highlight it, tap it again to choose', scale=SIZE['small'])
                   | {'align': 'center', 'color': COLORS['muted'], 'need_bind': 'controls.choice'})
    widgets.append(label([44, 1012, 1192, 32], bind='status', scale=SIZE['small'], ready=True) | {'color': COLORS['muted']})
    return {'id': 'talk', 'title': 'TALK', 'widgets': widgets}

def start_page():
    """Shown on the title screen and before a save is loaded (module ui.page 11): game art and a
    prompt, no buttons (the P5R companion's waiting page)."""
    widgets = [{'type': 'image', 'rect': [0, 0, 1240, 1080], 'bind': 'ui.route', 'src_format': 'module:p3p:bg/%d'},
               label([44, 20, 1150, 48], 'PERSONA 3 PORTABLE', scale=SIZE['title']) | {'color': COLORS['title']},
               {'type': 'image', 'rect': [320, 150, 600, 600], 'src': 'module:p3p:persona/1@600'},
               label([620, 800, 0, 60], 'Start or load a game to begin', scale=SIZE['title']) | {'align': 'center'},
               dict(label([44, 700, 1172, 40], bind='module_error_message', scale=SIZE['body']), need_bind='module_error')]
    return {'id': 'start', 'title': 'START', 'widgets': widgets}

def paused_overlay():
    """While the game is briefly unreadable in play (loading, scenes), the module republishes the
    last values with live.paused: grey them out, block touches on them and tag them."""
    paused = {'need_bind': 'live.paused'}
    # The grey wash also swallows taps and drags, so greyed-out buttons and lists cannot be used.
    # The tag sits centred at the top of the wash, clear of the title lettering (CALENDAR runs to x 655).
    return [{'type': 'image', 'rect': [24, 90, 1216, 880], 'src': 'module:p3p:fill', 'tint': '#B3D9D9DE',
             'input_block': True} | paused,
            fill([470, 104, 300, 52], 'select', **paused),
            label([620, 104, 0, 52], bind='live.pause', scale=SIZE['value']) | paused
            | {'align': 'center', 'text_center_h': 52, 'color': COLORS['title']}]

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
    # Item use: the module picks the row and member, then asks for one press at a time (pdrv.*);
    # the enforce rules below make the press while its gate is up (one sample).
    actions['item.pick'] = {'kind': 'module', 'action': 'item_pick', 'argument': '$payload', 'enabled_bind': 'item.can_pick'}
    actions['item.cancel'] = {'kind': 'module', 'action': 'item_cancel', 'argument': 0}
    actions['equip.pick'] = {'kind': 'module', 'action': 'equip_pick', 'argument': '$payload', 'enabled_bind': 'equip.can_pick'}
    actions['equip.choose'] = {'kind': 'module', 'action': 'equip_choose', 'argument': '$payload', 'haptic': 'confirm'}
    actions['equip.cancel'] = {'kind': 'module', 'action': 'equip_cancel', 'argument': 0}
    actions['persona.use'] = {'kind': 'module', 'action': 'persona_use', 'argument': 0, 'enabled_bind': 'persona.can_use',
                              'haptic': 'confirm'}
    for k in range(4):
        actions[f'item.use.{k}'] = {'kind': 'module', 'action': 'item_use', 'argument': k, 'enabled_bind': f'use.{k}.present',
                                    'haptic': 'confirm'}
    for key, pad in (('a', 'A'), ('b', 'B'), ('x', 'X'), ('l', 'L'), ('up', 'DUp'), ('down', 'DDown')):
        actions[f'pdrv.press.{key}'] = {'kind': 'button', 'button': pad, 'frames': 8, 'enabled_bind': f'pdrv.{key}'}
    # Choice taps (talk page): the game's own D-pad and A, as the player would press them.
    actions['choice.confirm'] = {'kind': 'button', 'button': 'A', 'frames': 6, 'enabled_bind': 'controls.choice',
                                 'haptic': 'confirm'}
    for k in range(1, 5):
        for sign, pad in ((1, 'DDown'), (-1, 'DUp')):
            actions[f'choice.move.{sign * k}'] = {'kind': 'button', 'button': pad, 'frames': 6, 'repeat': k,
                                                 'enabled_bind': 'controls.choice'}
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
            scrolls = []
            widgets += equip_widgets(outputs, scrolls)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets, 'scrolls': scrolls})
            continue
        if page_id == 'skills':
            scrolls = []
            widgets += skills_widgets(outputs, scrolls)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets, 'scrolls': scrolls})
            continue
    pages.append(battle_page(outputs))
    pages.append({'id': 'map', 'title': 'MAP', 'widgets': chrome('MAP') + map_widgets(outputs)})
    # ... and to the talk page (ui.page 12) while a choice list is open.
    pages.append(talk_page(outputs))
    page_binds.append({'point': 'ui.page', 'equals': 12, 'when_equal': {'page': 'talk'}})
    for page in pages:
        page['widgets'] += paused_overlay()
    # First page = the page shown at boot (page binds only fire on a change, mod_pages.cpp).
    pages.insert(0, start_page())
    page_binds.append({'point': 'ui.page', 'equals': 11, 'when_equal': {'page': 'start'}})
    outputs += ['live.paused', 'live.pause'] + [f'pdrv.{k}' for k in ('a', 'b', 'x', 'l', 'up', 'down', 'pending')]
    for page in pages:
        page['widgets'] = split_route(page['widgets'])
    result = {'format': 1, 'title_id': '0100DCD01525A000', 'name': 'Persona 3 Portable live companion',
              'min_runtime': 19, 'requires_module': True, 'poll_hz': 60,
              'canvas_w': 1240, 'canvas_h': 1080, 'background': '#FFFFB8CE',
              # The game font from the player's romfs (module decode_font/load_image, p3p_font.h);
              # p3p_font.txt is a marker holding no game data.
              'font': 'file:p3p_font.txt', 'font_atlas': 'module:p3p_font',
              'module_outputs': outputs, 'module': {'abi': 1, 'build_ids': [BUILD], 'libraries': {}},
              'actions': actions, 'page_binds': page_binds, 'pages': pages,
              'enforce': [{'action': f'pdrv.press.{k}', 'every_ms': 1} for k in ('a', 'b', 'x', 'l', 'up', 'down')],
              'enforce_gate': {'point': 'pdrv.pending', 'max': 1}}
    dest = ROOT / 'packages/Persona3Portable/dualscreen/manifest.json'
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(result, indent=2) + '\n')

if __name__ == '__main__': main()
