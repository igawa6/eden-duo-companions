#!/usr/bin/env python3
"""Decode Atlus P3/P4/P5 .FNT (Huffman-compressed indexed glyphs) to a PNG atlas + metrics JSON.
Format per Meloman19/PersonaEditor (research reference, no code copied verbatim).
usage: fnt_decode.py in.FNT out_prefix"""
import struct, sys, json
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
hsz, fsz = struct.unpack_from('<ii', d, 0)
count, w, h, bsz = struct.unpack_from('<4H', d, 14)
last = struct.unpack_from('<i', d, 24)[0]
bpp = bsz * 8 // (w * h); ncol = 1 << bpp
p = hsz
pal = [tuple(d[p+4*i:p+4*i+4]) for i in range(ncol)]; p += 4 * ncol
wsz = struct.unpack_from('<I', d, p)[0]; p += 4
cuts = [(d[p+2*i], d[p+2*i+1]) for i in range(wsz // 2)]; p += wsz
usz = struct.unpack_from('<I', d, p)[0]; p += 4 + usz
p += 4 * count  # reserved
ch = struct.unpack_from('<iiiiHHiii', d, p)
chsz, dsz, csz, _, bpg, _, gtc, gps, usize = ch; p += chsz
dic = [struct.unpack_from('<3H', d, p + 6*i) for i in range(dsz // 6)]; p += dsz
p += 4 * gtc
comp = d[p:p+csz]
glyphs, cur, node = [], [], 0
for byte in comp:
    for _ in range(8):
        node = dic[node][(byte & 1) + 1]; byte >>= 1
        if dic[node][1] == 0:
            if len(cur) == bpg: glyphs.append(bytes(cur)); cur = []
            cur.append(dic[node][2]); node = 0
if len(cur) == bpg: glyphs.append(bytes(cur))
def px(g):
    if bpp == 8: return list(g)
    o = []
    for b in g: o += [b & 15, b >> 4]   # "Reverse" nibble order
    return o
cols = 16; rows = (len(glyphs) + cols - 1) // cols
atlas = Image.new('LA', (cols * w, rows * h))
for i, g in enumerate(glyphs):
    v = px(g); gi = Image.new('LA', (w, h))
    gi.putdata([(255, pal[c][0]) for c in v])
    atlas.paste(gi, ((i % cols) * w, (i // cols) * h))
atlas.save(out + '_atlas.png')
json.dump({'src': src, 'cell_w': w, 'cell_h': h, 'bpp': bpp, 'glyphs': len(glyphs), 'declared': count,
           'cuts': cuts[:len(glyphs)]}, open(out + '_metrics.json', 'w'))
print(f'{src}: {len(glyphs)}/{count} glyphs {w}x{h} {bpp}bpp pal={pal[:3]}..{pal[-1]}')
