#!/usr/bin/env python3
"""SPR0 multi-texture sprite extractor. usage: spd_extract.py file.spd out_dir
texture entry 0x30: id u32, _, _ ,offset u32 @+8, size @+0xC, w @+0x10, h @+0x14, name @+0x20
sprite entry 0xA0: sprite id @+0, texture id @+4, x/y/w/h @+0x20, name (SJIS) @+0x70"""
import struct, sys, os, io, json
from PIL import Image
d = open(sys.argv[1], 'rb').read(); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
tc, sc = struct.unpack_from('<HH', d, 0x14); to, so = struct.unpack_from('<II', d, 0x18)
tex = {}
for i in range(tc):
    tid, _, off, size, w, h = struct.unpack_from('<IIIIII', d, to + i * 0x30)
    name = d[to+i*0x30+0x20:to+i*0x30+0x30].split(b'\0')[0].decode('ascii', 'replace')
    tex[tid] = (off, size, name)
cache, index = {}, []
for i in range(sc):
    b = so + i * 0xA0
    sid, tid = struct.unpack_from('<II', d, b); x, y, w, h = struct.unpack_from('<IIII', d, b + 0x20)
    nm = d[b+0x70:b+0xA0].split(b'\0')[0].decode('shift_jis', 'replace').strip()
    index.append(dict(id=sid, tex=tid, x=x, y=y, w=w, h=h, name=nm))
    if not w or not h or tid not in tex: continue
    if tid not in cache:
        off, size, _ = tex[tid]; im = Image.open(io.BytesIO(d[off:off+size])); cache[tid] = im.convert('RGBA')
    safe = ''.join(c if c.isalnum() else '_' for c in nm)
    cache[tid].crop((x, y, x + w, y + h)).save(f'{out}/{sid:04d}_{safe}.png')
json.dump(dict(textures={k: v[2] for k, v in tex.items()}, sprites=index), open(f'{out}/_index.json', 'w'), ensure_ascii=False, indent=0)
print(sys.argv[1], tc, 'textures', sc, 'sprites')
