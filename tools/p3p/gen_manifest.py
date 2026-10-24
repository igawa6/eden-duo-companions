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

def skills_widgets(outputs, scrolls):
    """Member buttons (two rows of five) and the selected member's native Skill list."""
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
            ('links', 'SOCIAL LINKS', 'Social Links'), ('skills', 'SKILLS', 'Skills'))
    actions = {f'ui.open.{n}': {'kind': 'module', 'action': 'ui_open', 'argument': n}
               for n in range(1, len(tabs) + 1)}
    actions.update({f'member.select.{i}': {'kind': 'module', 'action': 'member_select', 'argument': i}
                    for i in range(10)})
    page_binds = [{'point': 'ui.page', 'equals': n, 'when_equal': {'page': page_id}}
                  for n, (page_id, _, _) in enumerate(tabs, 1)]
    pages = []
    for page_id, title, _ in tabs:
        widgets = [label([40, 28, 900, 42], 'PERSONA 3 PORTABLE / ' + title, scale=4)]
        for i, (target, _, caption) in enumerate(tabs):
            widgets.append({'type': 'button', 'rect': [40 + i * 295, 90, 280, 55], 'text': caption,
                            'on_tap': f'ui.open.{i + 1}', 'text_scale': 3, 'color': COLORS['ink'],
                            'bg': '#FF8C315B' if page_id == target else COLORS['panel']})
        widgets += [label([40, 165, 850, 30], bind='live.date', ready=True),
                    label([925, 165, 280, 30], bind='live.money', ready=True),
                    {'type': 'label', 'rect': [60, 440, 1120, 80], 'text': 'Waiting for supported gameplay',
                     'text_scale': 4, 'color': COLORS['muted'], 'hide_bind': 'live.ready', 'hide_eq': 1},
                    label([40, 1020, 1160, 24], bind='status', scale=2),
                    label([40, 1050, 1160, 20], 'Read-only preview | 2D field/camp support | Battle and dungeon exploration pending', scale=2),
                    dict(label([40, 700, 1160, 36], bind='module_error_message', scale=3), need_bind='module_error')]
        if page_id == 'skills':
            scrolls = []
            widgets += skills_widgets(outputs, scrolls)
            pages.append({'id': page_id, 'title': title, 'widgets': widgets, 'scrolls': scrolls})
            continue
        count = {'home': 10, 'personas': 12, 'links': 28}[page_id]
        prefix = {'home': 'member', 'personas': 'persona', 'links': 'link'}[page_id]
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
