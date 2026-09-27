#!/usr/bin/env python3
"""Compare live before/after bottom-screen captures.

  cmp_live.py <shots dir> <label_before> <label_after>

The live page animates (the Samus marker pulses, the chase wash, the load animation), so two runs
never capture the same animation phase at the same wall-clock moment. For every capture of the
"after" run this finds a byte-identical capture (RGBA) of the "before" run in the same page state
(map* vs map*/back, menu* vs menu*), and otherwise reports the closest one with the differing pixel
count and RGB bounding box."""
import sys
from pathlib import Path

from PIL import Image, ImageChops


def load(p):
    return Image.open(p).convert("RGBA")


def state(name):
    return "menu" if name.startswith("menu") else "map"


root = Path(sys.argv[1])
a_dir, b_dir = root / sys.argv[2], root / sys.argv[3]
before = {p.name: load(p) for p in sorted(a_dir.glob("*-bottom.png"))}
after = {p.name: load(p) for p in sorted(b_dir.glob("*-bottom.png"))}
matched = 0
for name, img in after.items():
    candidates = [(n, im) for n, im in before.items() if state(n) == state(name)]
    hit = next((n for n, im in candidates if im.tobytes() == img.tobytes()), None)
    if hit:
        print(f"after {name:20s} == before {hit:20s} IDENTICAL ({img.size[0]}x{img.size[1]} RGBA)")
        matched += 1
        continue
    best = None
    for n, im in candidates:
        d = ImageChops.difference(im.convert("RGB"), img.convert("RGB"))
        px = sum(1 for p in d.getdata() if p != (0, 0, 0))
        if best is None or px < best[0]:
            best = (px, n, d.getbbox())
    print(f"after {name:20s} closest before {best[1]:20s} DIFF {best[0]} px, bbox {best[2]}")
print(f"{matched}/{len(after)} after-captures have a byte-identical before-capture")
