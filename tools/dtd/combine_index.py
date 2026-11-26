#!/usr/bin/env python3
"""The shipped index (every line unchanged, first) + the running game's index after it, its bundle ids
moved up by 10000 and its texture ids by 100000. The module reads a name's first line and falls back to the
next one of the same name when that bundle cannot be opened.

    python3 combine_index.py <shipped pictures.idx> <running pictures.idx> <out>
"""
import math, sys

# Unity 6 stores a block height of 1 GOB as 0. Such a texture's data has the 1-GOB padded size, not the
# plain size; read plain it comes out block-scrambled (the sushi card frames). Decide by the data size (by the size of its data).
_BLOCK = {48: (4, 16), 49: (5, 16), 50: (6, 16), 51: (8, 16), 4: (1, 4), 1: (1, 1), 3: (1, 3)}
def _gobs_fixed(w, h, fmt, size, gobs):
    if gobs != 0 or fmt not in _BLOCK: return gobs
    bs, bp = _BLOCK[fmt]
    bw, bh = -(-w // bs), -(-h // bs)
    plain = bw * bh * bp
    pad1 = -(-bw * bp // 64) * 64 * -(-bh // 8) * 8
    # size == pad1 == plain (small textures: UI_Sushi_Recipe_Menu_box, the sushi card) cannot be told apart by size;
    # Unity 6 writes 0 for 1 GOB, so 1 GOB as well (read plain, the sushi cards came out striped)
    return 1 if size == pad1 else gobs
old = open(sys.argv[1], encoding="latin-1").read()
new = open(sys.argv[2], encoding="latin-1").read().split("\n")
assert old.startswith("DTDP") and new[0].startswith("DTDP")
ol = old.rstrip("\n").split("\n")
assert max(int(l.split()[1]) for l in ol if l.startswith("B ")) < 10000
assert max(int(l.split()[1]) for l in ol if l.startswith("T ")) < 100000
out = []; fixed = 0
for l in new[1:]:
    if not l.strip(): continue
    f = l.split(" ")
    if f[0] == "B": f[1] = str(int(f[1]) + 10000)
    elif f[0] == "T":
        f[1] = str(int(f[1]) + 100000); f[2] = str(int(f[2]) + 10000)
        g = _gobs_fixed(int(f[5]), int(f[6]), int(f[7]), int(f[4]), int(f[8]))
        if g != int(f[8]): f[8] = str(g); fixed += 1
    elif f[0] in ("S", "F"): f[2] = str(int(f[2]) + 100000)
    else: sys.exit(f"unknown line: {l[:60]}")
    out.append(" ".join(f))
data = old.rstrip("\n") + "\n" + "\n".join(out) + "\n"
open(sys.argv[3], "w", encoding="latin-1", newline="\n").write(data)
print(f"{len(ol)} shipped lines + {len(out)} running-game lines ({fixed} textures read as 1 GOB), {len(data)} bytes")
