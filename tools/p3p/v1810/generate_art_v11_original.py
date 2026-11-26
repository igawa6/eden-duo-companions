#!/usr/bin/env python3
"""Original, procedurally drawn title backgrounds and generic word lettering.

These seven assets replace the unrecovered historical graphics. They are NOT
pixel-identical recreations of the v1.8.10 FINAL archive. They deliberately
avoid reproducing the game's commercial logo, extracting game art, or copying
unlicensed source textures. The labels use an ordinary OFL Inter typeface.

Original geometric UI drawings authored for the P3P companion redesign.
Released under the repository's GPL-3.0 license with this source code.
"""
from __future__ import annotations

import argparse
import math
import os
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H, SCALE = 1240, 1080, 2
ROUTES = (
    dict(base=(24, 156, 230, 255), shade=(12, 68, 148, 255),
         light=(123, 215, 250, 255), ink=(10, 40, 112, 255)),
    dict(base=(226, 67, 118, 255), shade=(120, 22, 62, 255),
         light=(255, 171, 205, 255), ink=(90, 13, 55, 255)),
)
WORDS = {
    "word_battle.png": ("BATTLE", (1531, 207)),
    "word_map.png": ("MAP", (868, 207)),
    "word_talk.png": ("TALK", (1038, 207)),
    "word_persona3.png": ("PERSONA 3", (2252, 214)),
    "word_portable.png": ("PORTABLE", (2102, 214)),
}
GOLD = (255, 192, 62, 255)


def title_background(route: int) -> Image.Image:
    """Original route-toned moon graphic, drawn from circles and ribbons."""
    c = ROUTES[route]
    navy=(3, 22, 60, 255) if route == 0 else (60, 11, 44, 255)
    moon=(196, 227, 249, 255) if route == 0 else (255, 202, 228, 255)
    dot=(148, 197, 237, 150) if route == 0 else (234, 143, 191, 150)
    canvas = Image.new("RGBA", (W * SCALE, H * SCALE), navy)
    d = ImageDraw.Draw(canvas)
    moon_x,moon_y,radius = 904*SCALE,472*SCALE,329*SCALE
    d.ellipse((moon_x-radius, moon_y-radius, moon_x+radius, moon_y+radius),
              fill=moon)
    # Independent, seeded crater geometry; no source moon imagery.
    rng=random.Random(12052026+route)
    for _ in range(64):
        angle=rng.random()*2*math.pi
        radius_factor=math.sqrt(rng.random())*.91
        px=moon_x+int(math.cos(angle)*radius*radius_factor)
        py=moon_y+int(math.sin(angle)*radius*radius_factor)
        rr=rng.choice((9,10,12,16,22,27))*SCALE
        d.ellipse((px-rr,py-rr,px+rr,py+rr),fill=dot)
    # Route color appears only as a lower-right accent; keep text legible.
    for color,offset in ((c["base"],0),(GOLD,24)):
        d.polygon([(W*SCALE,(890+offset)*SCALE),
                   (W*SCALE,(935+offset)*SCALE),
                   ((910-offset)*SCALE,H*SCALE),
                   ((865-offset)*SCALE,H*SCALE)],fill=color)
    d.rectangle((0,0,28*SCALE,H*SCALE),fill=GOLD)
    return canvas.resize((W,H),Image.Resampling.LANCZOS)

def title_word(word: str, dimensions: tuple[int,int], font_path: Path) -> Image.Image:
    """Neutral sans typography, explicitly NOT a copy of the game logo."""
    w,h=dimensions
    ss=2
    canvas=Image.new("RGBA",(w*ss,h*ss),(0,0,0,0))
    face=ImageFont.truetype(str(font_path),int(h*ss*.72))
    d=ImageDraw.Draw(canvas)
    bbox=d.textbbox((0,0),word,font=face,stroke_width=0)
    actual_w=bbox[2]-bbox[0]
    actual_h=bbox[3]-bbox[1]
    x=(canvas.width-actual_w)//2-bbox[0]
    y=(canvas.height-actual_h)//2-bbox[1]
    # Common white UI title treatment. No trademarked original outline.
    d.text((x,y),word,font=face,fill=(255,255,255,255))
    return canvas.resize((w,h),Image.Resampling.LANCZOS)


def generate_all(output: Path, font_path: Path) -> list[Path]:
    if not font_path.is_file():
        raise FileNotFoundError(f"Pinned official Inter OTF required: {font_path}")
    output.mkdir(parents=True, exist_ok=True)
    result=[]
    for route in (0,1):
        path=output/f"bg_start_{route}.png"
        title_background(route).save(path)
        result.append(path)
    for name,(word,size) in WORDS.items():
        path=output/name
        title_word(word,size,font_path).save(path)
        result.append(path)
    assert len(result)==7
    return result


def main() -> None:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--font",type=Path,default=Path(os.environ.get(
        "P3P_INTER_FONT_DIR","/usr/share/fonts/opentype/inter"))/"Inter-BlackItalic.otf")
    args=p.parse_args()
    items=generate_all(args.output,args.font)
    print(f"Generated {len(items)} wholly procedural replacement graphics (not historical exact matches)")


if __name__=="__main__":
    main()
