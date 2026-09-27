#!/usr/bin/env python3
"""For every after-capture WITHOUT a byte-identical before-capture: is there a before-capture that is
byte-identical everywhere outside the Samus player marker (the only element that animates on the
still map: its pulse phase depends on the capture moment)? Marker box from the diffs:
1240x1080 canvas (598..645, 672..732) -> masked (590,664)-(653,740); 640x740 aux-window readback
(309..333, 463..494) -> masked (305,455)-(337,498): the marker sprite, whichever save."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw
BOX = {(1240, 1080): (590, 664, 653, 740), (640, 740): (305, 455, 337, 498)}
root = Path(sys.argv[1]); a, b = root / sys.argv[2], root / sys.argv[3]
def load(p): return Image.open(p).convert("RGBA")
before = {p.name: load(p) for p in sorted(a.glob("*-bottom.png"))}
def masked(im):
    im = im.copy(); ImageDraw.Draw(im).rectangle(BOX[im.size], fill=(0, 0, 0, 0)); return im.tobytes()
ok = total = 0
for p in sorted(b.glob("*-bottom.png")):
    img = load(p)
    kind = "menu" if p.name.startswith("menu") else "map"
    cands = [(n, im) for n, im in before.items() if ("menu" if n.startswith("menu") else "map") == kind]
    if any(im.tobytes() == img.tobytes() for _, im in cands):
        continue
    total += 1
    hit = next((n for n, im in cands if masked(im) == masked(img)), None)
    ok += hit is not None
    print(f"after {p.name:20s} {'== before ' + hit + ' outside the Samus marker' if hit else 'NO masked match'}")
print(f"{ok}/{total} non-identical after-captures are identical outside the animated Samus marker")
