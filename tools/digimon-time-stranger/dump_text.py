#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds text122.json (the game's English text tables) from YOUR OWN copy of the game.

  python dump_text.py <romfs folder or app_text01.nx64.mvgl> [more archives ...] -o text122.json

Reads every "text/<sheet>" member (MDB1 archive, LZ4 members, EXPA tables) and writes
{"text/<sheet>:<table>": [[id, string], ...]}. The package build uses it to fill in and then reference the
game's text, so neither the package nor this repository holds any of it.
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from expa import parse


def lz4(src, n):
    o = bytearray(); i = 0
    while i < len(src):
        t = src[i]; i += 1; l = t >> 4
        if l == 15:
            while True:
                b = src[i]; i += 1; l += b
                if b != 255: break
        o += src[i:i + l]; i += l
        if i >= len(src): break
        off = src[i] | (src[i + 1] << 8); i += 2; ml = t & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4; st = len(o) - off
        for k in range(ml): o.append(o[st + k])
    return bytes(o[:n])


def members(path):
    m = open(path, 'rb').read()
    if m[:4] != b'MDB1':
        return
    fc, nc, dc, ds = struct.unpack_from('<IIIQ', m, 4)
    ta = 0x20; na = ta + fc * 16; da = na + nc * 0x80
    for i in range(1, min(fc, nc)):
        did = struct.unpack_from('<I', m, ta + i * 16 + 4)[0]
        if did >= dc: continue
        rec = m[na + i * 0x80:na + (i + 1) * 0x80]
        ext = rec[:4].split(b'\0')[0].decode(errors='replace').strip()
        name = rec[4:].split(b'\0')[0].decode('utf8', 'replace').replace('\\', '/')
        off, full, comp = struct.unpack_from('<QII', m, da + did * 16)
        b = m[ds + off:ds + off + comp]
        yield ext, name, (b if comp == full else lz4(b, full))


def main():
    args = sys.argv[1:]
    out = 'text122.json'
    if '-o' in args:
        k = args.index('-o'); out = args[k + 1]; del args[k:k + 2]
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.startswith('app_text') and f.endswith('.mvgl')]
        else:
            paths.append(a)
    res = {}
    for p in paths:
        for ext, name, data in members(p):
            if ext != 'mbe' or not name.startswith('text/') or data[:4] != b'EXPA': continue
            for tab, rows in parse(data).items():
                key = f"{name}:{tab}"
                if key in res: continue
                res[key] = [[str(r[0]) if r and r[0] is not None else '', r[1] if len(r) > 1 else None] for r in rows]
    json.dump(res, open(out, 'w', encoding='utf8'), ensure_ascii=False)
    print(f"{out}: {len(res)} tables, {sum(len(v) for v in res.values())} rows")


if __name__ == '__main__':
    main()
