# SPDX-FileCopyrightText: Copyright 2026 Eden_Duo_Mode contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Layout fixes the localized words need, used by gen_labels.py.

The top bar of the battle pages draws the label "Turn" (left aligned, text_scale 8, y 116) and the
turn number (text_scale 10, y 111) 78 units to its right, left aligned. The game's own words are
wider than the English one: about 84 units for TURN (Korean, Japanese, Chinese) and 105 for TURNO
(Spanish, Italian) at text_scale 7, against about 70 for "Turn" at 8, so the number was drawn over
the last letter, and a two digit number had no room at all.

Label: text_scale 6 (TURNO then ends about 90 units from its start).
Number: text_scale 7 (two digits, e.g. 99, are about 30 units wide) and right aligned, its right
edge 126 units from the label's start, 5 units before the "PLAYER PHASE" text next to it. A right
aligned number grows to the left, so it never runs into the phase text, and 99 still leaves room
after the longest word.

Both texts keep the baseline the original layout had. The original pair (scale 8 at y 116, scale 10
at y 111) shares a baseline, so the baseline is y + 2.5 * scale: the label moves down 5 for its 2
smaller sizes, the number 8 for its 3 smaller ones (7.5).
"""

LABEL_SCALE, LABEL_DY = 6, 5
NUMBER_SCALE, NUMBER_DY = 7, 8
NUMBER_SHIFT = 48  # 126 - 78: the number's right edge, from where its left edge was


def fix_turn_overlap(manifest):
    """Applies the fix to every page; returns the number of widgets changed."""
    changed = 0

    def walk(o):
        nonlocal changed
        if isinstance(o, dict):
            if o.get('type') == 'label' and o.get('text') == 'Turn' and o.get('text_scale') == 8 \
                    and o.get('align') != 'center':
                o['text_scale'] = LABEL_SCALE
                rect = o['rect']
                o['rect'] = [rect[0], rect[1] + LABEL_DY] + rect[2:]
                changed += 1
            elif o.get('type') == 'value' and o.get('bind') == 'bt.turn' and o.get('text_scale') == 10:
                rect = o['rect']
                o['rect'] = [rect[0] + NUMBER_SHIFT, rect[1] + NUMBER_DY] + rect[2:]
                o['text_scale'] = NUMBER_SCALE
                o['align'] = 'right'
                changed += 1
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    for page in manifest['pages']:
        walk(page['widgets'])
    return changed
