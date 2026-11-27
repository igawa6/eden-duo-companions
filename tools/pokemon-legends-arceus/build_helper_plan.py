#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble PLA 1.1.1's format-2 helper; verify player's NSO via PLA_NSO/--nso.

Uses Sword's NSO segment reader and the same ELF relocation -> plan technique.
No game data is copied except the required single hook instruction identity.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/pokemon-sword'))
from sw_sigscan import nso_segments

BUILD = 'AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E000000000000000000000000'
OUTPUT = ROOT / f'packages/PokemonLegendsArceus/dualscreen/guest/{BUILD}/load-plan.json'
HOOK = 0x19e5ff0
EXPECTED = '68f25f39'  # ldrb w8,[x19,#0x7fc]
SYMBOLS = {
    'pla_mailbox': ('mailbox', 0), 'pla_type': ('main', 0x415d618),
    'pla_resume': ('main', HOOK + 4), 'pla_next': ('main', 0x24777e4),
    'pla_previous': ('main', 0x24774e0), 'pla_toggle': ('main', 0x24772f8),
}
KINDS = {'R_AARCH64_CALL26': 'aarch64_branch26', 'R_AARCH64_JUMP26': 'aarch64_branch26',
         'R_AARCH64_ADR_PREL_PG_HI21': 'aarch64_adrp', 'R_AARCH64_ADD_ABS_LO12_NC': 'aarch64_add_lo12'}


def assemble():
    with tempfile.TemporaryDirectory(prefix='pla-helper-') as tmp:
        obj, raw = Path(tmp)/'helper.o', Path(tmp)/'helper.bin'
        subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(ROOT/'native/helper/pla_guest_helper.S'), '-o', str(obj)], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(obj), str(raw)], check=True)
        code = raw.read_bytes()
        rel = subprocess.check_output(['llvm-readelf', '-Wr', str(obj)], text=True)
        syms = subprocess.check_output(['llvm-readelf', '-Ws', str(obj)], text=True)
    assert re.search(r'\b0000000000000000\s+\d+\s+NOTYPE\s+GLOBAL\s+DEFAULT\s+\d+\s+pla_entry\b', syms), syms
    assert 0 < len(code) <= 65536 and len(code) % 4 == 0
    for at in range(0, len(code), 4):
        word, = struct.unpack_from('<I', code, at)
        assert word & 0xff000000 != 0xd4000000
        assert word & 0xff000000 != 0xd5000000 or word == 0xd503201f
    relocations = []
    pattern = r'([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+(R_AARCH64_\S+)\s+[0-9a-fA-F]+\s+(\S+)\s+([+-])\s+([0-9a-fA-F]+)'
    for line in rel.splitlines():
        if 'R_AARCH64_' not in line:
            continue
        match = re.fullmatch(pattern, line.strip())
        assert match, line
        off, kind, symbol, sign, add = match.groups()
        assert kind in KINDS and symbol in SYMBOLS and sign == '+'
        target, base = SYMBOLS[symbol]
        relocations.append(dict(offset=int(off,16), kind=KINDS[kind], target=target, addend=base+int(add,16)))
    assert len(relocations) == 10, relocations
    return dict(bytes=code.hex(), relocations=relocations)


def generate(nso):
    seg, bss, build = nso_segments(Path(nso).read_bytes())
    if build != BUILD:
        raise ValueError(f'unsupported build {build}')
    start, size, text = seg[0]
    def check(at, expected):
        assert start <= at < at+len(bytes.fromhex(expected)) <= start+size
        if text[at-start:at-start+len(bytes.fromhex(expected))].hex() != expected:
            raise ValueError(f'instruction mismatch at main+{at:#x}')
    check(HOOK, EXPECTED)
    # Context: live lookup, null guard, hook, move live view, input checks.
    check(0x19e5fe8, 'd1f9e597')
    check(0x19e5ff4, 'f50300aa')
    for at, expected in [(0x24777e4,'ff0301d1'), (0x24774e0,'ff0301d1'), (0x24772f8,'ff8301d1')]:
        check(at, expected)
    return dict(format=2, title_id='01001F5010DFA000', build_id=BUILD, module='main',
                layout=dict(segments=[dict(location=m,size=s) for m,s,_ in seg], bss_size=bss),
                mailbox_size=4096, mailbox_epoch_offset=4092,
                patches=[dict(offset=HOOK, expected=EXPECTED, replacement='00000014',
                    relocations=[dict(offset=0,kind='aarch64_branch26',target='code',addend=0)])],
                guest_code=assemble())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nso', default=os.environ.get('PLA_NSO'))
    parser.add_argument('--output', type=Path, default=OUTPUT)
    parser.add_argument('--check', action='store_true')
    args=parser.parse_args()
    if not args.nso:
        parser.error('pass --nso or PLA_NSO (player-owned exefs/main)')
    plan=generate(args.nso)
    if args.check:
        assert json.loads(args.output.read_text()) == plan, 'plan differs; regenerate'
    else:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(plan,indent=1)+'\n')
    print(f'PLA plan verified: {len(bytes.fromhex(plan["guest_code"]["bytes"]))} code bytes, {len(plan["guest_code"]["relocations"])} relocations')

if __name__ == '__main__':
    main()
