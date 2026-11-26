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
    """Draw stylized abstract circles/ribbons, no external texture sources."""
    c = ROUTES[route]
    canvas = Image.new("RGBA", (W * SCALE, H * SCALE), c["base"])
    d = ImageDraw.Draw(canvas)
    def poly(points, color):
        d.polygon([(x*SCALE, y*SCALE) for x, y in points], fill=color)

    # Deliberately original composition: two diagonal ribbons and a radial dial.
    poly([(770,0),(1240,0),(1240,1080),(510,1080)], c["light"])
    poly([(965,0),(1240,0),(1240,1080),(690,1080)], c["shade"])
    poly([(1090,0),(1180,0),(450,1080),(360,1080)], GOLD)
    poly([(1155,0),(1225,0),(495,1080),(425,1080)], c["ink"])
    cx,cy=835*SCALE,510*SCALE
    for radius,alpha,width in [(376,32,4),(307,115,3),(250,54,3)]:
        rr=radius*SCALE
        d.ellipse((cx-rr,cy-rr,cx+rr,cy+rr),
                  outline=(255,255,255,alpha),width=width*SCALE)
    # Seeded microstars give repeatable texture without a source photograph.
    rng=random.Random(12052026+route)
    for _ in range(440):
        x,y=rng.randrange(120,1170),rng.randrange(100,1030)
        rr=rng.choice((1,1,1,2))
        alpha=rng.randrange(25,85)
        d.ellipse((x*SCALE-rr*SCALE,y*SCALE-rr*SCALE,
                   x*SCALE+rr*SCALE,y*SCALE+rr*SCALE),
                  fill=(255,255,255,alpha))
    for i in range(48):
        angle=math.pi*i/24
        r1,r2=319*SCALE,334*SCALE
        x1=cx+int(math.cos(angle)*r1);y1=cy+int(math.sin(angle)*r1)
        x2=cx+int(math.cos(angle)*r2);y2=cy+int(math.sin(angle)*r2)
        d.line((x1,y1,x2,y2),fill=(255,255,255,105),width=3*SCALE)
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
