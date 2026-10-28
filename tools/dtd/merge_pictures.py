#!/usr/bin/env python3
"""Add a few pictures from a newer pictures.idx to the one in use, leaving every line already there exactly as it is.

    python3 merge_pictures.py <index in use> <newer index> "<name pattern>" [out]

Only "S" lines whose names match the pattern (and are not in the index already) are added, with their texture ("T")
and bundle ("B") lines renumbered after the existing ones. A bundle already listed is reused.
Refused: a texture stored with gobs 0 whose data size is exactly the Switch 1-GOB block-linear size but not the plain
size -- the Unity 6 case that came out block-scrambled in 0.37.61 (its layout cannot be told from the index alone).
"""
import re, sys

if len(sys.argv) < 4:
    sys.exit(__doc__)
old_p, new_p, pat = sys.argv[1], sys.argv[2], re.compile(sys.argv[3])
out_p = sys.argv[4] if len(sys.argv) > 4 else old_p
old = open(old_p, encoding="latin-1").read()
assert old.startswith("DTDP"), "not a pictures.idx"
new = open(new_p, encoding="latin-1").read().split("\n")
oldl = old.rstrip("\n").split("\n")
o_names = {l.split()[1].lower() for l in oldl if l.startswith("S ")}
o_bpath = {l.split()[2]: l.split()[1] for l in oldl if l.startswith("B ")}
nb = max(int(l.split()[1]) for l in oldl if l.startswith("B ")) + 1
nt = max(int(l.split()[1]) for l in oldl if l.startswith("T ")) + 1
NB = {l.split()[1]: l for l in new if l.startswith("B ")}
NT = {l.split()[1]: l for l in new if l.startswith("T ")}
BLOCK = {4: (4, 1), **{f: (s, s) for f, s in zip(range(48, 60), (4, 5, 6, 8, 10, 12) * 2)}}
add_b, add_t, add_s, bmap, tmap, refused = [], [], [], {}, {}, []
for l in new:
    if not l.startswith("S "): continue
    f = l.split()
    if not pat.search(f[1]) or f[1].lower() in o_names: continue
    t = NT[f[2]].split()
    tid, bid, off, size, tw, th, fmt, gobs = t[1], t[2], int(t[3]), int(t[4]), int(t[5]), int(t[6]), int(t[7]), int(t[8])
    if gobs == 0 and fmt in BLOCK:
        bw, bh = BLOCK[fmt]; nx, ny = -(-tw // bw), -(-th // bh)
        if size == (-(-nx // 4) * 4) * (-(-ny // 8) * 8) * 16 and size != nx * ny * 16:
            refused.append(f[1]); continue
    if bid not in bmap:
        path = NB[bid].split()[2]
        if path in o_bpath: bmap[bid] = o_bpath[path]
        else:
            bmap[bid] = str(nb); add_b.append(f"B {nb} {path} {NB[bid].split()[3]}"); nb += 1
    if tid not in tmap:
        tmap[tid] = str(nt); add_t.append(f"T {nt} {bmap[bid]} " + " ".join(t[3:])); nt += 1
    add_s.append("S " + f[1] + " " + tmap[tid] + " " + " ".join(f[3:]))
    print("add", f[1], f"{tw}x{th} fmt {fmt} gobs {gobs}")
for r in refused: print("refused (1-GOB layout risk):", r)
open(out_p, "w", encoding="latin-1", newline="\n").write(old.rstrip("\n") + "\n" + "\n".join(add_b + add_t + add_s) + "\n")
print(f"{len(add_s)} pictures added ({len(add_t)} textures, {len(add_b)} bundles); the {len(oldl)} existing lines unchanged")
