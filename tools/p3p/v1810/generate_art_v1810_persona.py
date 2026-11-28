#!/usr/bin/env python3
"""Rebuild the final v1.8.10 Persona-detail panels from the historical v1.8 source.

These two original UI-only surfaces are byte-identical to v1.8.10 FINAL when
the 7-pixel header underline correction is restored. No fonts or game art used.
"""
from __future__ import annotations
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

GOLD = (255, 182, 53)
NAVY = (8, 48, 123)
BLUE_LINE = (57, 117, 183)

def persona_surface(path: Path, female: bool) -> None:
    im = Image.new('RGBA', (1240, 1080), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im, 'RGBA')
    line = (175, 69, 98) if female else BLUE_LINE
    sheet = (255, 246, 250) if female else (247, 253, 255)
    pale = (255, 225, 236) if female else (207, 239, 255)
    dr.rounded_rectangle((504, 106, 1216, 970), radius=15, fill=sheet+(222,))
    dr.line([(523, 206), (1198, 206)], fill=line+(135,), width=3)
    # Final alignment fix: +7px relative to the original v1.8 design script.
    dr.line([(527, 211), (587, 211)], fill=GOLD+(255,), width=5)
    dr.rounded_rectangle((521, 226, 775, 470), radius=8, fill=pale+(175,))
    dr.rounded_rectangle((786, 226, 1200, 471), radius=8, fill=(255, 255, 255, 92))
    for row in range(1, 5):
        yy = 226+49*row
        dr.line([(795, yy), (1191, yy)], fill=line+(44,), width=2)
    dr.line([(521, 540), (1201, 540)], fill=line+(62,), width=2)
    for idx in range(9):
        xx=520+75*idx
        dr.rounded_rectangle((xx, 552, xx+72, 636), radius=6,
                             fill=((240, 250, 255, 130) if not female else (255, 240, 246, 140)))
    dr.line([(521, 701), (1200, 701)], fill=line+(95,), width=3)
    for rr in range(4):
        yy = 710+38*rr
        if rr%2 == 0:
            dr.rounded_rectangle((520, yy, 1201, yy+37), radius=5, fill=pale+(90,))
    dr.line([(864, 711), (864, 858)], fill=line+(76,), width=2)
    dr.line([(521, 872), (1200, 872)], fill=line+(90,), width=2)
    dr.line([(521, 918), (1200, 918)], fill=line+(46,), width=2)
    im.save(path)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    persona_surface(args.output / 'persona_detail_blue.png', False)
    persona_surface(args.output / 'persona_detail_red.png', True)
    print('Generated 2 exact Persona-detail UI surfaces')

if __name__ == '__main__':
    main()
