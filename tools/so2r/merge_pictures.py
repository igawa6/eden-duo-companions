#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 jadesigns.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add a few pictures from a newer pictures.idx to the one in use, leaving every line already there exactly as it is.

    python3 tools/so2r/merge_pictures.py <index in use> <newer index> "<name pattern>" [out]

Only "S" lines whose names match the pattern (and are not in the index already) are added, with their texture ("T")
and bundle ("B") lines renumbered after the existing ones and appended at the end. A bundle already listed is reused.
The existing text is kept byte for byte (the new lines only follow it).
"""
import re
import sys

if len(sys.argv) < 4:
    sys.exit(__doc__)
old_p, new_p, pat = sys.argv[1], sys.argv[2], re.compile(sys.argv[3])
out_p = sys.argv[4] if len(sys.argv) > 4 else old_p
old = open(old_p, encoding="latin-1", newline="").read()
assert old.startswith("SO2P"), "not an SO2R pictures.idx"
new = open(new_p, encoding="latin-1").read().split("\n")
oldl = old.rstrip("\n").split("\n")
o_names = {l.split()[1].lower() for l in oldl if l.startswith("S ")}
o_bpath = {l.split()[2]: l.split()[1] for l in oldl if l.startswith("B ")}
nb = max(int(l.split()[1]) for l in oldl if l.startswith("B ")) + 1
nt = max(int(l.split()[1]) for l in oldl if l.startswith("T ")) + 1
NB = {l.split()[1]: l for l in new if l.startswith("B ")}
NT = {l.split()[1]: l for l in new if l.startswith("T ")}
add_b, add_t, add_s, bmap, tmap = [], [], [], {}, {}
for l in new:
    if not l.startswith("S "):
        continue
    f = l.split()
    if not pat.search(f[1]) or f[1].lower() in o_names:
        continue
    t = NT[f[2]].split()
    tid, bid = t[1], t[2]
    if bid not in bmap:
        b = NB[bid].split()
        if b[2] in o_bpath:
            bmap[bid] = o_bpath[b[2]]
        else:
            bmap[bid] = str(nb)
            add_b.append(" ".join(["B", str(nb)] + b[2:]))
            nb += 1
    if tid not in tmap:
        tmap[tid] = str(nt)
        add_t.append(" ".join(["T", str(nt), bmap[bid]] + t[3:]))
        nt += 1
    add_s.append(" ".join(["S", f[1], tmap[tid]] + f[3:]))
    print("add", f[1])
if not add_s:
    sys.exit("nothing to add")
body = old if old.endswith("\n") else old + "\n"
open(out_p, "w", encoding="latin-1", newline="\n").write(body + "\n".join(add_b + add_t + add_s) + "\n")
print(f"{len(add_s)} pictures added ({len(add_t)} textures, {len(add_b)} bundles); the {len(oldl)} existing lines unchanged")
