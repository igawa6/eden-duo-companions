#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Append English LocaDataEntry.String positions to pictures.idx (no game text shipped).
Usage: python build_string_index.py ROMFS [pictures.idx]
Run after build_picture_index.py. Positions are for the supported v524288 build only.
"""
import struct
import sys
from pathlib import Path
import UnityPy

# Native localization keys, not replacement display text.
KEYS = {
    **{name: name for name in ('neutral', 'earth', 'wind', 'fire', 'water', 'physical', 'magical')},
    'turn1': 'The Keeper who has first turn may only use two of their three chosen Monsters during their first turn as a drawback.',
    'health': 'Health', 'mana': 'Mana', 'combo': 'Combo', 'damage': 'Damage', 'completion': 'Total Completion', 'items': 'Items', 'resist': 'Resist', 'switch': 'Switch', 'weak': 'Weak',
    'enemyturn': 'Enemy Turn', 'options': 'Options', 'on': 'On', 'off': 'Off', 'unavailable': 'Not Available',
    'exploration': 'Exploration', 'combat': 'Combat', 'map': 'Map', 'team': 'Team',
    'equipment': 'Equipment', 'weapon': 'Weapon', 'accessories': 'Accessories',
    'skillpoints': 'Skillpoints', 'ability': 'Explore Ability', 'done': 'Done',
    'back': 'Back', 'close': 'Close', 'loading': 'Loading...', 'lvl': 'Lvl',
    # Combat Result screen factor labels (owner round 1, W5: the live Execution breakdown)
    'execution': 'Execution:', 'duration': 'Combat Duration:', 'healthiness': 'Healthiness:', 'turns': 'turns',
    'difficulty': 'Enemy Difficulty:', 'buffs': 'Buffs / Debuffs:',
    # owner round 2: the title prompt ("Press" [A] "to start", composed like the native one) and
    # the Monster Info card's crit labels
    'press': 'Press', 'tostart': 'to start', 'critchance': 'Crit Chance', 'critdamage': 'Crit Damage',
    # owner round 3: the native "[Y] Select Level" hint's caption (the level popup)
    'selectlevel': 'Select Level',
}
root = Path(sys.argv[1])
idx = Path(sys.argv[2] if len(sys.argv) > 2 else 'packages/MonsterSanctuary/dualscreen/pictures.idx')
lines = [s for s in idx.read_text().splitlines() if not s.startswith('Q ')]
node = next(int(s.split()[1]) for s in lines if s.split()[0] == 'N' and s.split()[2] == 'sharedassets0.assets')
env = UnityPy.load(str(root / 'Data/data.unity3d'))
obj = next(o for o in env.objects if o.assets_file.name == 'sharedassets0.assets' and o.path_id == 11375)
raw = obj.get_raw_data()
for name, key in KEYS.items():
    b = key.encode()
    needle = struct.pack('<I', len(b)) + b
    at = raw.find(needle)
    assert at >= 0 and at % 4 == 0, name
    # LocaDataEntry: Key, String, French, Spanish, ... (aligned Unity strings).
    at = (at + len(needle) + 3) & ~3
    n = struct.unpack_from('<I', raw, at)[0]
    assert 0 < n <= 200 and raw[at + 4:at + 4 + n].decode('utf8') == key, name
    lines.append(f'Q {name} {node} {obj.byte_start + at} {n}')
idx.write_text('\n'.join(lines) + '\n')
print(f'{len(KEYS)} native string positions -> {idx}')
