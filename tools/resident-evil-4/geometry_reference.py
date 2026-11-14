# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent fixture-only equivalent of the former prepare_map hit-mesh output.

No decoded data is stored here. The public selectors contain room ids, file/entry
numbers and scales only. Read both SAT arrays and the first BIN root at test time.
"""
from pathlib import Path
import re
import struct

HEADINGS = {0x10d:-40, 0x117:-70, 0x222:-90, 0x226:-90, 0x227:-90,
            0x228:-90, 0x22a:-90, 0x21a:-90, 0x21b:-90, 0x503:-70}


def container(blob):
    count = struct.unpack_from('<I', blob)[0]
    offsets = struct.unpack_from(f'<{count}I', blob, 16)
    return [(blob[16+count*4+i*4:20+count*4+i*4].rstrip(b'\0'),
             blob[start:offsets[i+1] if i+1<count else len(blob)])
            for i, start in enumerate(offsets)]


def sat(blob):
    base = struct.unpack_from('<I', blob, 4)[0]
    _, _, nv, nn, ne, _, np, nf, ns, _, _ = struct.unpack_from('<BB9H', blob, base)
    at = base+20
    vertices = [struct.unpack_from('<3f', blob, at+i*12) for i in range(nv)]
    at += (nv+nn+ne)*12
    triangles = [struct.unpack_from('<3H', blob, at+i*20) for i in range(nf+ns)]
    return vertices, triangles


def decode(rom, root):
    selectors = (root/'native/modules/re4_map_art_tables.h').read_text()
    result = []
    files = {}
    for key, file, entry, scale in re.findall(r'\{"([^"]+)", "([^"]+)", (\d+), (\d+),', selectors):
        if file not in files:
            files[file] = container((rom/'bio4/ss/cmn'/file).read_bytes())
        entries = container(files[file][int(entry)][1])
        models = [b for tag,b in entries if tag==b'BIN']
        meshes = [sat(b) for tag,b in entries if tag==b'MHT']
        local, world = meshes[0][0], meshes[1][0]
        triangles = meshes[1][1]
        head = struct.unpack_from('<Q', models[0])[0]
        part,parent,_,_,tx,ty,tz = struct.unpack_from('<4B3f', models[0], head)
        assert models[0][0x29]==1 and (part,parent)==(0,255)
        room = int(key.split('/')[1],16)
        scale = int(scale)
        count = max(max(t) for t in triangles)+1
        vertices = [list(w)+[scale*(v[0]+tx), -scale*(v[2]+tz), scale*(v[1]+ty)]
                    for w,v in zip(world[:count],local[:count])]
        result.append(dict(area=key.replace('/','-'), vertices=vertices,
            triangles=[list(t) for t in triangles],
            narrow=len(models)>1 or room in (0x107,0x10f,0x217,0x320),
            heading=HEADINGS.get(room,0),
            affine=key.startswith('stage5/') and all(abs(l[0]-w[0]/10)<1 and abs(l[2]-w[2]/10)<1
                                                    for l,w in zip(local,world)),
            ox=scale*tx, oz=scale*tz))
    return result
