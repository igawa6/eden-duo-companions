#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build-pinned native HUD visibility helper. Only the mailbox is written by the module.

The game's UI manager supplies the frame clock. TimePartUI and MissionTraceUI register
live instances from their own Update methods. The helper calls GameObject.SetActive;
only objects it found active are owned/restored. The minimap and hotbar are untouched.
A frame lease restores owned objects if companion updates stop, including unload.
Mailbox: +0 signature, +4 beat, +8 lease, +12 requested mask, +16 owned mask;
+24 time Component/class/native identity; +48 tracker GameObject/class/native identity;
+80 and +104 owned GameObject/native/class identity triples. Mask bits: time=1, tracker=2.
Hook/setter RVAs and prologues were checked against the extracted v851968 main NSO.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / 'packages/MyTimeAtSandrock/dualscreen'
BUILD = 'B74DFD27FC9DA1B4E19D06E3D40DD370' + '0' * 32
CAVE = 0x54DF4F0
HOOKS = [('manager', 0x2AE16D0), ('time', 0x29819D0), ('tracker', 0x2E9EB60)]


def branch(site, target):
    delta = target - site
    assert delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27)
    return struct.pack('<I', 0x14000000 | ((delta // 4) & 0x3FFFFFF))


def assemble(base, source):
    lines, rel, pending = [], [], ''
    for raw in source.strip().splitlines():
        line = raw.strip()
        if not line:
            continue
        label, pending = pending, ''
        if ':' in line.split()[0]:
            label, line = line.split(':', 1)
            label += ':'
            line = line.strip()
        if not line:
            pending = label
            continue
        if line.startswith('MB '):
            reg = line.split()[1]
            rel.append(4 * len(lines))
            lines += [f'{label} adrp {reg}, #0', f'add {reg}, {reg}, #0']
        elif '@' in line:
            op, target = line.split('@')
            lines.append(f'{label} {op.strip()} #{int(target, 16) - (base + 4 * len(lines))}')
        else:
            lines.append(f'{label} {line}')
    with tempfile.TemporaryDirectory(prefix='mtas-hud-') as tmp:
        src, obj, out = [Path(tmp) / n for n in ('helper.s', 'helper.o', 'helper.bin')]
        src.write_text('\n'.join(lines) + '\n')
        subprocess.run(['llvm-mc', '-triple=aarch64', '-filetype=obj', str(src), '-o', str(obj)], check=True)
        linked = Path(tmp) / 'helper.elf'
        subprocess.run(['ld.lld', '-Ttext=' + hex(base), '--entry=' + hex(base), str(obj), '-o', str(linked)], check=True)
        subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(linked), str(out)], check=True)
        data = out.read_bytes()
        symbols = subprocess.check_output(['llvm-nm', str(obj)], text=True)
        labels = {parts[2]: int(parts[0], 16) for row in symbols.splitlines()
                  if len(parts := row.split()) == 3 and parts[1].lower() == 't'}
    assert len(data) == 4 * len(lines)
    return data, rel, labels


def read_nso(path):
    import lz4.block
    raw = Path(path).read_bytes()
    assert raw[:4] == b'NSO0' and raw[0x40:0x60].hex().upper() == BUILD
    segments = [struct.unpack_from('<III', raw, 0x10 + i * 0x10) for i in range(3)]
    compressed = struct.unpack_from('<III', raw, 0x60)
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    fo, _, size = segments[0]
    text = raw[fo:fo + compressed[0]]
    if flags & 1:
        text = lz4.block.decompress(text, uncompressed_size=size)
    return segments, struct.unpack_from('<I', raw, 0x3C)[0], text


def source(original):
    # Each thunk replays one position-independent prologue instruction before rejoining.
    return f'''
manager:
    stp x29, x30, [sp, #-64]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x0, x1, [sp, #48]
    bl step
    ldp x0, x1, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    .word {original[0]}
    b @2ae16d4
time:
    stp x16, x17, [sp, #-16]!
    MB x16
    str x0, [x16, #24]
    ldr x17, [x0]
    str x17, [x16, #32]
    ldr x17, [x0, #16]
    str x17, [x16, #40]
    ldp x16, x17, [sp], #16
    .word {original[1]}
    b @29819d4
tracker:
    stp x16, x17, [sp, #-16]!
    MB x16
    ldr x17, [x0, #120]
    str x17, [x16, #48]
    cbz x17, tracker_end
    ldr x17, [x17]
    str x17, [x16, #56]
    ldr x17, [x0, #120]
    ldr x17, [x17, #16]
    str x17, [x16, #64]
tracker_end:
    ldp x16, x17, [sp], #16
    .word {original[2]}
    b @2e9eb64
step:
    stp x29, x30, [sp, #-16]!
    MB x19
    mov w0, #0x4831
    movk w0, #0x4d54, lsl #16
    str w0, [x19]
    ldr w0, [x19, #4]
    add w0, w0, #1
    str w0, [x19, #4]
    mov w20, wzr
    ldr w0, [x19, #8]
    cbz w0, no_lease
    sub w0, w0, #1
    str w0, [x19, #8]
    ldr w20, [x19, #12]
no_lease:
    mov x21, xzr
    tbz w20, #0, time_ready
    ldp x0, x1, [x19, #24]
    ldr x2, [x19, #40]
    bl valid
    cbz x0, time_ready
    mov x1, xzr
    bl @24071d0
    mov x21, x0
time_ready:
    add x22, x19, #80
    bl update
    str w0, [x19, #16]
    mov x21, xzr
    tbz w20, #1, tracker_ready
    ldp x0, x1, [x19, #48]
    ldr x2, [x19, #64]
    bl valid
    mov x21, x0
tracker_ready:
    add x22, x19, #104
    bl update
    ldr w1, [x19, #16]
    orr w0, w1, w0, lsl #1
    str w0, [x19, #16]
    ldp x29, x30, [sp], #16
    ret
valid:
    cbz x0, valid_end
    cbz x2, invalid
    ldr x3, [x0]
    cmp x3, x1
    b.ne invalid
    ldr x3, [x0, #16]
    cmp x3, x2
    b.eq valid_end
invalid:
    mov x0, xzr
valid_end:
    ret
update:
    stp x29, x30, [sp, #-16]!
    ldr x0, [x22]
    ldr x1, [x22, #16]
    ldr x2, [x22, #8]
    bl valid
    cbz x0, released
    cmp x0, x21
    b.eq hide
    mov x1, xzr
    bl @2408570
    cbnz w0, released
    ldr x0, [x22]
    mov w1, #1
    mov x2, xzr
    bl @2408500
released:
    str xzr, [x22]
hide:
    cbz x21, no_owner
    ldr x0, [x21, #16]
    cbz x0, no_owner
    mov x0, x21
    mov x1, xzr
    bl @2408570
    cbz w0, owner_check
    str x21, [x22]
    ldr x0, [x21, #16]
    str x0, [x22, #8]
    ldr x0, [x21]
    str x0, [x22, #16]
    mov x0, x21
    mov w1, wzr
    mov x2, xzr
    bl @2408500
owner_check:
    ldr x0, [x22]
    cmp x0, x21
    cset w0, eq
    b update_end
no_owner:
    str xzr, [x22]
    mov w0, wzr
update_end:
    ldp x29, x30, [sp], #16
    ret
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', required=True, help='Player-extracted main NSO')
    args = parser.parse_args()
    segments, bss, text = read_nso(args.verify)
    # Prologues are pinned below after verifying the shipped instructions.
    original = [struct.unpack_from('<I', text, offset)[0] for _, offset in HOOKS]
    assert original == [0xFC1B0FE8, 0xA9BE7BFD, 0x6DBA23E9], original
    code, reloc, labels = assemble(CAVE, source(original))
    assert CAVE + len(code) <= segments[1][1]
    relocations = []
    for offset in reloc:
        relocations.extend([{'offset': offset, 'kind': 'aarch64_adrp', 'target': 'mailbox', 'addend': 0},
                            {'offset': offset + 4, 'kind': 'aarch64_add_lo12', 'target': 'mailbox', 'addend': 0}])
    offsets = [CAVE + labels[name] for name, _ in HOOKS]
    plan = {'format': 1, 'title_id': '0100B63016916000', 'build_id': BUILD, 'module': 'main',
            'layout': {'segments': [{'location': mo, 'size': size} for _, mo, size in segments],
                       'bss_size': bss}, 'mailbox_size': 0x1000,
            'patches': [{'offset': site, 'expected': struct.pack('<I', word).hex(),
                         'replacement': branch(site, target).hex()}
                        for (_, site), word, target in zip(HOOKS, original, offsets)],
            'payloads': [{'offset': CAVE, 'expected': bytes(len(code)).hex(),
                          'bytes': code.hex(), 'relocations': relocations}]}
    padded = text + bytes(segments[1][1] - len(text))
    assert not any(padded[CAVE:CAVE + len(code)])
    out = PKG / 'guest' / BUILD / 'load-plan.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=1) + '\n')
    print(f'verified {len(code)} helper bytes, {len(HOOKS)} native hooks; wrote {out}')


if __name__ == '__main__':
    main()
