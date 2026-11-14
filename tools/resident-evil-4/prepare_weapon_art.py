#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate native case BIN models and prepare numeric shape metadata.
Art is rendered by the module at runtime; this generator emits no images.
Requires Pillow and NumPy. Inputs stay local; no screen crops or generated artwork.
"""
import argparse, struct, sys, json
from pathlib import Path
import numpy as np
from PIL import Image
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('research', type=Path)
p.add_argument('romfs', type=Path)
p.add_argument('--case', action='store_true', help='also prepare placed case models and measured native model extents')
a = p.parse_args()
sys.dont_write_bytecode = True
sys.path.insert(0, str(a.research / 'assets/tools'))
import pack_fen
rom = a.romfs
out = Path(__file__).resolve().parents[2] / 'packages/ResidentEvil4/dualscreen/assets'
arc = (rom / 'ss/eng/ss_pzzl.dat').read_bytes()
textures = dict(pack_fen.iter_textures(str(rom / 'imagepackhd/3b000001.pack.fen'), skip_blank=False))

DIMS = {}
CASE_RENDER = a.case
def render(id):
    """Native XY case plane, Z depth; primitive5 triangles,6 strip,8 quads.
 Native lime placeholder/specular passes are omitted; no lighting substitution.
 """
    # Native pieceTblInit aliases (source lineage and Switch piece table verified).
    mdl = (33 if id==64 else 53 if id in (23,53,109) else
           18 if id in (*range(18,23),168) else 1 if id in (1,2,14) else
           8 if id in (8,9,10) else 6 if id in (6,25,28) else
           0 if id in (0,4,7,24,26,32,106,160) else id)
    start = struct.unpack_from('<I', arc, (mdl * 2 + 4) * 4)[0]
    end = struct.unpack_from('<I', arc, (mdl * 2 + 5) * 4)[0]
    b = arc[start:end]
    tid = 33 if id==64 else 149 if id in (149,151) else 53 if id==109 else id
    to = struct.unpack_from('<I', arc, (tid * 2 + 5) * 4)[0]
    tpl = arc[to:]
    ntex = struct.unpack_from('<I', tpl, 4)[0]
    tex = []
    for i in range(ntex):
        h = struct.unpack_from('<I', tpl, 16 + i * 16)[0]
        ref = struct.unpack_from('<I', tpl, h + 8)[0]
        pack, idx = struct.unpack_from('<II', tpl, ref)
        assert pack == 989855745
        tex.append(np.array(textures[idx]))
    nv = struct.unpack_from('<H', b, 88)[0]
    pos = np.frombuffer(b, dtype='<f4', count=nv * 3, offset=struct.unpack_from('<Q', b, 72)[0]).reshape(-1, 3).copy()
    uv = np.frombuffer(b, dtype='<f4', count=nv * 2, offset=struct.unpack_from('<Q', b, 24)[0]).reshape(-1, 2)
    xy = pos[:, :2]
    lo = xy.min(axis=0)
    hi = xy.max(axis=0)
    DIMS[id] = (round((hi[0]-lo[0])/100), round((hi[1]-lo[1])/100))
    # Rasterize placed art directly at the largest cell scale, from native triangles/HD textures.
    cw,ch=(DIMS[id][0]*120,DIMS[id][1]*120) if CASE_RENDER else (400,200)
    if CASE_RENDER and not (0<cw<=1800 and 0<ch<=960):raise ValueError('not a case piece')
    scale = min((cw-8) / (hi[0] - lo[0]), (ch-8) / (hi[1] - lo[1])) if CASE_RENDER else min(380 / (hi[0] - lo[0]), 180 / (hi[1] - lo[1]))
    xy = (xy - (hi + lo) / 2) * scale
    xy[:, 0] += cw/2
    xy[:, 1] = -xy[:, 1] + ch/2
    canvas = np.zeros((ch, cw, 4), dtype=np.uint8)
    zbuf = np.full((ch, cw), -np.inf)
    off = struct.unpack_from('<Q', b, 48)[0]
    vbase = 0
    for part in range(struct.unpack_from('<H', b, 42)[0]):
        mat = b[off + 12]
        size = struct.unpack_from('<I', b, off + 24)[0]
        p = off + 32
        nr = struct.unpack_from('<I', b, p)[0]
        p += 4
        for r in range(nr):
            mode, count = struct.unpack_from('<HH', b, p)
            p += 4
            ix = list(range(vbase, vbase + count))
            vbase += count
            if mode == 6:
                tris = [(ix[j], ix[j + 1], ix[j + 2]) for j in range(count - 2)]
            elif mode == 5:
                tris = [tuple(ix[j:j + 3]) for j in range(0, count, 3)]
            elif mode == 8:
                tris = [t for j in range(0, count, 4) for t in [(ix[j], ix[j + 1], ix[j + 2]), (ix[j], ix[j + 2], ix[j + 3])]]
            elif mode == 4:
                tris = [tuple(ix[j:j + 3]) for j in range(0, count, 3)]
            else:
                raise ValueError(mode)
            assert mat < len(tex), (hex(id), mat, len(tex))
            texture = tex[mat]
            th, tw = texture.shape[:2]
            if tuple(texture[0, 0, :3]) == (231, 255, 51) and np.all(texture[:, :, :3] == texture[0, 0, :3]):
                continue
            for ids in tris:
                a, c, d = xy[list(ids)]
                den = (c[1] - d[1]) * (a[0] - d[0]) + (d[0] - c[0]) * (a[1] - d[1])
                if abs(den) < 1e-07:
                    continue
                x0 = max(0, int(min(a[0], c[0], d[0])))
                x1 = min(cw-1, int(max(a[0], c[0], d[0]) + 1))
                y0 = max(0, int(min(a[1], c[1], d[1])))
                y1 = min(ch-1, int(max(a[1], c[1], d[1]) + 1))
                if x1 < x0 or y1 < y0:
                    continue
                yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
                xx = xx + 0.5
                yy = yy + 0.5
                w0 = ((c[1] - d[1]) * (xx - d[0]) + (d[0] - c[0]) * (yy - d[1])) / den
                w1 = ((d[1] - a[1]) * (xx - d[0]) + (a[0] - d[0]) * (yy - d[1])) / den
                w2 = 1 - w0 - w1
                z = w0 * pos[ids[0], 2] + w1 * pos[ids[1], 2] + w2 * pos[ids[2], 2]
                mask = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (z >= zbuf[y0:y1 + 1, x0:x1 + 1])
                u = w0 * uv[ids[0], 0] + w1 * uv[ids[1], 0] + w2 * uv[ids[2], 0]
                v = w0 * uv[ids[0], 1] + w1 * uv[ids[1], 1] + w2 * uv[ids[2], 1]
                pixels = texture[np.clip((v * th).astype(int), 0, th - 1), np.clip((u * tw).astype(int), 0, tw - 1)].copy()
                pixels[:, :, 3] = 255
                canvas[y0:y1 + 1, x0:x1 + 1][mask] = pixels[mask]
                zbuf[y0:y1 + 1, x0:x1 + 1][mask] = z[mask]
        off += 32 + size
    assert vbase == nv, (id, vbase, nv)
    return Image.fromarray(canvas)
IDS = [3, 33, 64, 35, 37, 39, 41, 42, 44, 45, 46, 47, 48, 52, 53, 54, 55, 56, 148, 23, 109, 62, 82, 1, 2, 14, 65, 16, 71, 83]
if a.case: IDS=range(256)
supported=[]
for id in IDS:
    try:
        image = render(id)
    except (AssertionError, ValueError, IndexError, struct.error):
        if not a.case: raise
        continue
    if a.case:
        w,h=DIMS[id]
        if not (0<w<=15 and 0<h<=8):continue
        box=image.getbbox()
        if not box:continue
        supported.append(id)
if a.case:
    existing=set(supported)
    rows=json.loads((a.research/'analysis/20261101-local-readers/piece-table.json').read_text())['rows']
    rows=[v for v in rows if v[0] in existing]
    header=['// SPDX-License-Identifier: GPL-3.0-or-later','#pragma once','#include <array>','namespace re4 {','inline std::array<unsigned,2> CaseSize(unsigned id) {',' switch(id) {']
    header += [f' case {v[0]}:return {{{v[1]},{v[2]}}};' for v in rows]
    header += [' default:return {0,0};',' }','}','inline bool CaseArtSupported(unsigned id){return CaseSize(id)[0]!=0;}','}']
    (Path(__file__).resolve().parents[2]/'native/modules/re4_case_art.h').write_text('\n'.join(header)+'\n')
print(f'Prepared {len(IDS)} native weapon models')
