#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the format-2 load plan for the Skyward Sword HD guest helper
(native/helper/sshd_stick_helper.S: right-stick mirror, Adventure Pouch ops, top-HUD hide list). Same mechanism as
tools/belmont/build_helper_plan.py: compile with clang (aarch64-none-elf), take .text bytes +
relocations, check the three hook sites against the player's own main NSO, emit the plan JSON that
mod_load_plan.cpp parses.

The game executable is never part of this repository: pass the dumped exefs/main NSO with
--nso or SSHD_NSO.

usage: build_helper_plan.py --nso <exefs/main> [--output PATH]
"""
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
SRC = ROOT / 'native/helper/sshd_stick_helper.S'
BUILD = '5441AF3C89F97E1D25A8CCF14C742BF82336D8A8' + '0' * 24
TITLE = 0x01002DA013484000
HOOK = 0xED6408       # Controller::Update, `orr w8, w25, w5` (stick)
RESUME = HOOK + 4
LOOP_HOOK = 0xE3A818  # main-loop epilogue, `adrp x8, <dSystem page>` (pouch ops)
LOOP_RESUME = LOOP_HOOK + 4
DRAW_HOOK = 0x1007440  # layout Pane::Draw entry, `str x21, [sp, #-0x30]!` (top-HUD hide list)
DRAW_RESUME = DRAW_HOOK + 4
LYT_CONTROL_GAME = 0x183AC40  # dLytControlGame_c* (the game's layout controller)
STATE_NORMAL = 0x164F988      # its static sFStateID_c StateID_Normal (name pointer at +8)
STATE_NORMAL_NAME = 0x1343131  # "dLytControlGame_c::StateID_Normal"
# main+offset targets (pouch analysis)
SYMBOLS = {'sshd_mailbox_base': ('mailbox', 0), 'sshd_resume': ('main', RESUME),
           'sshd_loop_resume': ('main', LOOP_RESUME), 'sshd_draw_resume': ('main', DRAW_RESUME),
           'sshd_dsystem_page': ('main', 0x2BF9EB0),          # page of the replaced adrp
           'sshd_file_manager': ('main', 0x2BF9CC0),          # FileManager* (file A at +0x10)
           'sshd_event_manager': ('main', 0x182DED8),         # EventManager* (state at +0x248)
           'sshd_swap_stock_and_pouch': ('main', 0xDE59E0),   # swapStockAndPouchItems(pouch, stock)
           # B-item equip (equip analysis): the player's ZR-wheel commit
           'sshd_player': ('main', 0x2BF9940),                # dAcPy* (rando PLAYER_PTR)
           'sshd_bitem_flags': ('main', 0x139B6DC),           # u32[9] item -> ownership flag id
           'sshd_has_item': ('main', 0x4DDCC0),               # flag id -> owned
           'sshd_item_withheld': ('main', 0xDDD2CC),          # flag id -> withheld by the story
           'sshd_set_b_item': ('main', 0xA17C9C),             # set B-item(player, item)
           # dev.44 menu sample: dLytControlGame_c::sInstance and its StateID_Normal (state at +0x120)
           'sshd_lyt_control_game': ('main', LYT_CONTROL_GAME),
           'sshd_state_normal': ('main', STATE_NORMAL)}
KINDS = {'R_AARCH64_JUMP26': 'aarch64_branch26', 'R_AARCH64_CALL26': 'aarch64_branch26',
         'R_AARCH64_ADR_PREL_PG_HI21': 'aarch64_adrp', 'R_AARCH64_ADD_ABS_LO12_NC': 'aarch64_add_lo12',
         'R_AARCH64_LDST64_ABS_LO12_NC': 'ldst64_lo12', 'R_AARCH64_LDST32_ABS_LO12_NC': 'ldst32_lo12',
         'R_AARCH64_LDST8_ABS_LO12_NC': 'ldst8_lo12'}
# B-wheel item -> ownership flag id, as the game's table main+0x139B6DC holds it (bombs, bow,
# clawshots, beetle, slingshot, gust bellows, whip, [boat cannon], bug net)
BITEM_FLAGS = (0x5C, 0x13, 0x14, 0x35, 0x34, 0x31, 0x89, 0x38, 0x47)


def nso(path):
    d = Path(path).read_bytes()
    assert d[:4] == b'NSO0'
    flags = struct.unpack_from('<I', d, 0xC)[0]
    segs = []
    for i in range(3):
        fo, mo, ds = struct.unpack_from('<III', d, 0x10 + i * 0x10)
        segs.append((fo, mo, ds))
    bss = struct.unpack_from('<I', d, 0x3C)[0]
    csz = struct.unpack_from('<III', d, 0x60)
    build = d[0x40:0x60].hex().upper()
    img = bytearray(segs[2][1] + segs[2][2] + bss)
    for i, (fo, mo, ds) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        data = lz4.block.decompress(raw, uncompressed_size=ds) if flags & (1 << i) else raw[:ds]
        img[mo:mo + ds] = data
    layout = {'segments': [{'location': mo, 'size': ds} for _, mo, ds in segs], 'bss_size': bss}
    return bytes(img), layout, build


def adrp(at, rd, target):
    """adrp xRd, page(target) at main+at"""
    pages = ((target & ~0xFFF) - (at & ~0xFFF)) >> 12
    return 0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5) | rd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nso', default=os.environ.get('SSHD_NSO'))
    ap.add_argument('--output', default=str(ROOT / 'packages/SkywardSwordHD/dualscreen/load-plans' / (BUILD + '.json')))
    a = ap.parse_args()
    if not a.nso:
        ap.error('pass --nso or set SSHD_NSO')
    img, layout, build = nso(a.nso)
    assert build == BUILD, f'build {build} is not 1.0.1'
    hook = img[HOOK:HOOK + 4]
    ins = struct.unpack('<I', hook)[0]
    # orr w8, w25, w5 = 0x2A050328 (ORR (shifted register), 32-bit, Rm=5, Rn=25, Rd=8)
    assert ins == 0x2A050328, f'main+{HOOK:#x} = {ins:#010x}, expected orr w8,w25,w5'
    loop_hook = img[LOOP_HOOK:LOOP_HOOK + 4]
    # adrp x8, main+0x2BF9000 at main+0xE3A818
    assert struct.unpack('<I', loop_hook)[0] == adrp(LOOP_HOOK, 8, 0x2BF9EB0), \
        f'main+{LOOP_HOOK:#x} = {loop_hook.hex()}, expected adrp x8, main+0x2bf9000'
    draw_hook = img[DRAW_HOOK:DRAW_HOOK + 4]
    # Pane::Draw (D-pad reader analysis, vtable slot +0x20 of pane and picture):
    # str x21, [sp, #-0x30]! ; stp x20, x19, [sp, #0x10] ; stp x29, x30, [sp, #0x20] ; add x29, sp, #0x20 ;
    # ldrb w8, [x0, #0xeb] (the visible bit)
    assert struct.unpack('<I', draw_hook)[0] == 0xF81D0FF5, f'main+{DRAW_HOOK:#x} = {draw_hook.hex()}'
    assert struct.unpack_from('<5I', img, DRAW_HOOK) == (0xF81D0FF5, 0xA9014FF4, 0xA9027BFD, 0x910083FD,
                                                          0x3943AC08), 'Pane::Draw prologue moved'
    for name, off, word in [('ldr x0, [x8, #0xeb0]', LOOP_HOOK + 4, 0xF9475900),
                            ('swapStockAndPouchItems prologue str x23, [sp, #-0x40]!', 0xDE59E0, 0xF81C0FF7),
                            ('set B-item prologue stp x22, x21, [sp, #-0x30]!', 0xA17C9C, 0xA9BD57F6),
                            ('item-flag check prologue stp x29, x30, [sp, #-0x10]!', 0x4DDCC0, 0xA9BF7BFD),
                            ('withheld check prologue stp x20, x19, [sp, #-0x20]!', 0xDDD2CC, 0xA9BE4FF4)]:
        got = struct.unpack_from('<I', img, off)[0]
        assert got == word, f'main+{off:#x} = {got:#010x}, expected {name}'
    assert struct.unpack_from('<9I', img, 0x139B6DC) == BITEM_FLAGS, 'B-item flag table moved'
    # dev.44 menu sample: the game's own isStateNormal check (main+0xb253fc): adrp x8, <controller page>;
    # ldr x8, [x8, #0xc40]; cbz x8; ldr x0, [x8, #0x120]; adrp x1, <StateID_Normal page>; add x1, x1, #0x988
    assert struct.unpack_from('<6I', img, 0xB253FC) == (0xB00068A8, 0xF9462108, 0xB4000148, 0xF9409100,
                                                         0xD0005941, 0x91262021), 'isStateNormal check moved'
    assert img[STATE_NORMAL_NAME:STATE_NORMAL_NAME + 34] == b'dLytControlGame_c::StateID_Normal\0', 'state name moved'
    # the state object's name pointer is a RELATIVE relocation (r_offset, R_AARCH64_RELATIVE, addend)
    assert struct.pack('<QQQ', STATE_NORMAL + 8, 0x403, STATE_NORMAL_NAME) in img, 'StateID_Normal moved'
    with tempfile.TemporaryDirectory() as t:
        obj, binf = Path(t, 'h.o'), Path(t, 'h.bin')
        subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(SRC), '-o', str(obj)], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(obj), str(binf)], check=True)
        code = binf.read_bytes()
        rel = subprocess.run(['llvm-readelf', '-r', str(obj)], check=True, capture_output=True, text=True).stdout
        syms = subprocess.run(['llvm-nm', str(obj)], check=True, capture_output=True, text=True).stdout
    entry = {m.group(2): int(m.group(1), 16) for m in re.finditer(r'([0-9a-f]+) T (\w+)', syms)}
    relocs = []
    pat = re.compile(r'([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)')
    for line in rel.splitlines():
        m = pat.fullmatch(line.strip())
        if not m:
            continue
        off, kind, sym, sign, add = m.groups()
        assert kind in KINDS and sym in SYMBOLS, line
        target, base = SYMBOLS[sym]
        relocs.append({'offset': int(off, 16), 'kind': KINDS[kind], 'target': target,
                       'addend': base + int(add, 16) * (1 if sign == '+' else -1)})
    # 2x mailbox adrp+add, 2 resume b, 3x adrp+ldr, adrp+add, 5 bl, adrp; stick sequence: 4x adrp+ldr
    # (event manager and player, at the start and on every frame of play); draw hook: mailbox adrp+add, resume b;
    # dev.44 menu sample: controller adrp+ldr, StateID_Normal adrp+add; dev.48 stick hook: event manager
    # adrp+ldr (the L lock's L not during an event)
    assert len(relocs) == 36, relocs
    # The main image is ~720 MiB (mostly .bss), so the companion code segment after it is out of
    # b/bl range (128 MiB) of the hook. The helper instead goes into the zero padding at the end
    # of .text's last page (inside the RX mapping, within range), as an ordinary checked patch;
    # its two main-relative branches are encoded here, only the mailbox address is relocated.
    text = layout['segments'][0]
    cave = (text['location'] + text['size'] + 15) & ~15
    assert cave + len(code) <= (text['location'] + text['size'] + 0xFFF) & ~0xFFF, 'no room in .text padding'
    assert img[cave:cave + len(code)] == bytes(len(code)), 'text padding is not empty'
    code = bytearray(code)

    def b(at, target):
        return struct.pack('<I', 0x14000000 | (((target - at) // 4) & 0x03FFFFFF))
    for r in [r for r in relocs if r['target'] == 'main']:
        at, ins = cave + r['offset'], struct.unpack_from('<I', code, r['offset'])[0]
        if r['kind'] == 'aarch64_branch26':
            word = struct.unpack('<I', b(at, r['addend']))[0] | (ins & 0x80000000)  # keep bl vs b
        elif r['kind'] == 'aarch64_adrp':
            word = adrp(at, ins & 31, r['addend'])
        elif r['kind'] == 'aarch64_add_lo12':
            word = (ins & ~(0xFFF << 10)) | ((r['addend'] & 0xFFF) << 10)
        else:  # ldr lo12, scaled by the access size
            shift = {'ldst64_lo12': 3, 'ldst32_lo12': 2, 'ldst8_lo12': 0}[r['kind']]
            assert r['addend'] % (1 << shift) == 0
            word = (ins & ~(0xFFF << 10)) | (((r['addend'] & 0xFFF) >> shift) << 10)
        code[r['offset']:r['offset'] + 4] = struct.pack('<I', word)
    relocs = [r for r in relocs if r['target'] != 'main']
    assert all(r['kind'] in ('aarch64_adrp', 'aarch64_add_lo12') for r in relocs), relocs
    code = bytes(code)
    plan = {
        'title_id': f'{TITLE:016X}', 'build_id': BUILD, 'module': 'main', 'format': 2,
        'layout': layout, 'mailbox_size': 4096, 'mailbox_epoch_offset': 4092,
        'patches': [{'offset': cave, 'expected': bytes(len(code)).hex(), 'replacement': code.hex(),
                     'relocations': relocs},
                    {'offset': HOOK, 'expected': hook.hex(),
                     'replacement': b(HOOK, cave + entry['sshd_stick_hook']).hex()},
                    {'offset': LOOP_HOOK, 'expected': loop_hook.hex(),
                     'replacement': b(LOOP_HOOK, cave + entry['sshd_loop_hook']).hex()},
                    {'offset': DRAW_HOOK, 'expected': draw_hook.hex(),
                     'replacement': b(DRAW_HOOK, cave + entry['sshd_draw_hook']).hex()}],
        # format 2 requires a companion code segment; ours only holds an unused `ret`
        'guest_code': {'bytes': 'c0035fd6', 'relocations': []},
    }
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=1) + '\n')
    print('wrote', out, len(code), 'code bytes')


if __name__ == '__main__':
    sys.exit(main())
