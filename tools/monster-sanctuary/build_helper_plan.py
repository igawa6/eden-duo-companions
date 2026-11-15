#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the format-2 load plan for the Monster Sanctuary guest helper.

Compiles native/helper/ms_guest_helper.S (clang, aarch64-none-elf), takes its .text bytes and
relocations, resolves every external symbol to (target, addend) and writes
packages/MonsterSanctuary/dualscreen/guest/<build id>/load-plan.json (format 2, the schema
mod_load_plan.cpp parses; same technique as tools/belmont/build_helper_plan.py).

The player's own executable is read, never shipped: pass the merged exefs `main` NSO of
update v524288 with --nso or MS_NSO. Its header gives the layout; its LZ4 text segment is
decompressed here to check the hook's expected bytes and every called function's RVA against
the expected first instruction words below.

    MS_NSO=/path/to/exefs/main python3 tools/monster-sanctuary/build_helper_plan.py
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import lz4.block

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'native/helper/ms_guest_helper.S'
TITLE_ID = '0100A0F00DA68000'
BUILD_ID = '8EA928B6AA10C84F9E69129C45BCAAAF' + '0' * 32
OUT = ROOT / 'packages/MonsterSanctuary/dualscreen/guest' / BUILD_ID / 'load-plan.json'

HOOK = 0x4FABC0                       # GameStateManager.Update
HOOK_BYTES = bytes.fromhex('f44fbea9')  # stp x20, x19, [sp, #-0x20]!
# Render-time pass of the battle-menu hide: tk2dCamera.OnPreCull (after every Update, LateUpdate
# and tween of the frame, right before the camera culls). Entry symbol ms_guest_helper_precull.
PRECULL_HOOK = 0x13396A0
PRECULL_BYTES = bytes.fromhex('f30f1ef8')  # str x19, [sp, #-0x20]!
MAILBOX_SIZE = 4096
EPOCH_OFFSET = 4092

# symbol -> (target, addend). "main" addends are RVAs (main-relative).
SYMBOLS = {
    'ms_mailbox_base': ('mailbox', 0),
    'ms_resume': ('main', HOOK + 4),
    'ms_resume_precull': ('main', PRECULL_HOOK + 4),
    # TypeInfo slots (script.json; the same ones ms_reader.h reads)
    'ms_slot_game_controller': ('main', 0x4A9B980),
    'ms_slot_ui_controller': ('main', 0x4A9D240),
    'ms_slot_player_controller': ('main', 0x4A9CBE0),
    # the string literal "UnifyEngagementPrompt" (metadata usage slot; MainMenu.Start's
    # GameObject.Find argument, stringliteral.json)
    'ms_slot_lit_engagement_prompt': ('main', 0x4ADA8F0),
    'ms_gameobject_find': ('main', 0x1159890),                 # UnityEngine.GameObject.Find
    'ms_splash_is_finished': ('main', 0x14A3060),              # UnityEngine.Rendering.SplashScreen.get_isFinished
    'ms_menulist_select_item': ('main', 0x379460),            # MenuList.SelectMenuItem
    'ms_menulist_try_select_current': ('main', 0x37A1E0),     # MenuList.TrySelectCurrent
    'ms_targetselector_select_target': ('main', 0x5CCEA0),    # TargetSelector.SelectTarget
    'ms_targetselector_try_select_target': ('main', 0x5CE3B0),  # TargetSelector.TrySelectTarget
    'ms_combat_cancel_target_selection': ('main', 0x24B410),  # CombatController.CancelTargetSelection
    'ms_combat_try_select_monster_level': ('main', 0x252100),  # CombatController.TrySelectMonsterLevel
    'ms_popup_is_open': ('main', 0x4916E0),                   # PopupController.get_IsOpen
    'ms_map_confirm_teleporting': ('main', 0x369AC0),         # MapMenu.ConfirmTeleporting
    'ms_monstermanager_switch_monsters': ('main', 0x3CFE30),  # MonsterManager.SwitchMonsters
    'ms_inventoryitem_get_equipment': ('main', 0x3D86C0),     # InventoryItem.get_Equipment
    'ms_monster_get_equipment': ('main', 0x3855D0),           # Monster.get_Equipment
    'ms_equipment_weapon_slot_count': ('main', 0x4776A0),     # EquipmentManager.GetWeaponSlotCount
    'ms_equipment_accessory_slot_count': ('main', 0x4774D0),  # EquipmentManager.GetAccessorieSlotCount
    'ms_equipment_has_weapon_type': ('main', 0x478310),       # EquipmentManager.HasWeaponTypeEquipped
    'ms_equipment_has_accessory_type': ('main', 0x478530),    # EquipmentManager.HasAccessoryTypeEquipped
    'ms_equipment_equip': ('main', 0x4780E0),                 # EquipmentManager.Equip
    'ms_inventory_remove_item': ('main', 0x3DA5E0),           # InventoryManager.RemoveItem
    'ms_inventory_add_item': ('main', 0x3D9850),              # InventoryManager.AddItem
    'ms_follower_switch_monster': ('main', 0x60FF30),         # PlayerFollower.SwitchMonster
    'ms_component_get_transform': ('main', 0x114E7F0),        # Component.get_transform
    'ms_transform_get_local_scale': ('main', 0x14AFC70),      # Transform.get_localScale
    'ms_transform_set_local_scale': ('main', 0x14AFD80),      # Transform.set_localScale
}
# Called functions must still begin with a frame-opening instruction (stp/str/sub sp): a cheap
# check that the RVA still lands on a function entry of this build.
PROLOGUE_MASKS = [(0xFFC003E0, 0xA98003E0), (0xFFC003E0, 0x298003E0), (0xFFE003E0, 0xF81E03E0),
                  (0xFF0003FF, 0xD10003FF), (0xFFE00FE0, 0xFC000FE0),
                  (0xFFC003FF, 0xF9400000)]  # or a one-load getter (ldr x0, [x0, #imm])

RELOC_KINDS = {
    'R_AARCH64_CALL26': 'aarch64_branch26',
    'R_AARCH64_JUMP26': 'aarch64_branch26',
    'R_AARCH64_ADR_PREL_PG_HI21': 'aarch64_adrp',
    'R_AARCH64_ADD_ABS_LO12_NC': 'aarch64_add_lo12',
    'R_AARCH64_LDST64_ABS_LO12_NC': 'aarch64_ldr64_lo12',
    'R_AARCH64_LDST32_ABS_LO12_NC': 'aarch64_ldr32_lo12',
}


def nso_text(nso: bytes):
    if nso[:4] != b'NSO0':
        sys.exit('not an NSO0 file')
    flags = struct.unpack_from('<I', nso, 0xC)[0]
    seg = [struct.unpack_from('<4I', nso, 0x10 + i * 0x10) for i in range(3)]
    bss = seg[2][3]
    csize = struct.unpack_from('<3I', nso, 0x60)
    build_id = nso[0x40:0x60].hex().upper()
    off, mem, size, _ = seg[0]
    raw = nso[off:off + csize[0]]
    text = lz4.block.decompress(raw, uncompressed_size=size) if flags & 1 else raw
    layout = {'segments': [{'location': s[1], 'size': s[2]} for s in seg], 'bss_size': bss}
    return text, layout, build_id


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--nso', default=os.environ.get('MS_NSO'))
    ap.add_argument('--out', type=Path, default=OUT)
    a = ap.parse_args()
    if not a.nso:
        ap.error('pass --nso or set MS_NSO')
    text, layout, build_id = nso_text(Path(a.nso).read_bytes())
    if build_id != BUILD_ID:
        sys.exit(f'NSO build id {build_id} is not {BUILD_ID}')
    for at, want in ((HOOK, HOOK_BYTES), (PRECULL_HOOK, PRECULL_BYTES)):
        if text[at:at + 4] != want:
            sys.exit(f'hook bytes @ {at:#x} {text[at:at+4].hex()} != {want.hex()}')
    for name, (target, addend) in SYMBOLS.items():
        if target != 'main' or name.startswith('ms_slot') or name.startswith('ms_resume'):
            continue
        word = struct.unpack_from('<I', text, addend)[0]
        if not any(word & m == v & m for m, v in PROLOGUE_MASKS):
            sys.exit(f'{name} @ {addend:#x}: {word:08x} is not a function prologue')
    with tempfile.TemporaryDirectory() as td:
        obj, binf = Path(td) / 'h.o', Path(td) / 'h.bin'
        subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(SRC), '-o', str(obj)], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(obj), str(binf)], check=True)
        code = binf.read_bytes()
        rel = subprocess.run(['llvm-readelf', '-r', str(obj)], check=True, capture_output=True, text=True).stdout
        nm = subprocess.run(['llvm-nm', str(obj)], check=True, capture_output=True, text=True).stdout
    entries = {m.group(2): int(m.group(1), 16)
               for m in re.finditer(r'^([0-9a-f]+) T (ms_guest_helper_\w+)$', nm, re.M)}
    if entries.get('ms_guest_helper_entry') != 0 or 'ms_guest_helper_precull' not in entries:
        sys.exit(f'entry symbols: {entries}')
    pat = re.compile(r'([0-9a-f]+)\s+[0-9a-f]+\s+(R_AARCH64_\S+)\s+[0-9a-f]+\s+(\S+)\s+([+-])\s+([0-9a-f]+)')
    relocs = []
    for line in rel.splitlines():
        m = pat.fullmatch(line.strip())
        if not m:
            if re.match(r'\s*[0-9a-f]{8,}\s', line):
                sys.exit(f'unparsed relocation: {line}')
            continue
        off, kind, sym, sign, add = m.groups()
        if kind not in RELOC_KINDS:
            sys.exit(f'unhandled relocation {kind}')
        if sym not in SYMBOLS:
            sys.exit(f'undeclared symbol {sym}')
        extra = int(add, 16) * (1 if sign == '+' else -1)
        target, base = SYMBOLS[sym]
        relocs.append(dict(offset=int(off, 16), kind=RELOC_KINDS[kind], target=target, addend=base + extra))
    used = {r['addend'] for r in relocs}
    missing = [s for s, (t, ad) in SYMBOLS.items() if t == 'main' and ad not in used]
    if missing:
        sys.exit(f'declared but unused: {missing}')
    plan = {
        'title_id': TITLE_ID,
        'build_id': BUILD_ID,
        'module': 'main',
        'format': 2,
        'layout': layout,
        'mailbox_size': MAILBOX_SIZE,
        'mailbox_epoch_offset': EPOCH_OFFSET,
        'patches': [{
            'offset': at,
            'expected': want.hex(),
            'replacement': struct.pack('<I', 0x14000000).hex(),
            'relocations': [{'offset': 0, 'kind': 'aarch64_branch26', 'target': 'code', 'addend': entries[sym]}],
        } for at, want, sym in ((HOOK, HOOK_BYTES, 'ms_guest_helper_entry'),
                                (PRECULL_HOOK, PRECULL_BYTES, 'ms_guest_helper_precull'))],
        'guest_code': {'bytes': code.hex(), 'relocations': relocs},
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(plan, indent=1) + '\n')
    print(json.dumps({'plan': str(a.out.relative_to(ROOT)), 'code_bytes': len(code), 'relocations': len(relocs)}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
