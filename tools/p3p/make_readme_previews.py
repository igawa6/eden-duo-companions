#!/usr/bin/env python3
"""Generate sample-layout images for README from our own procedural art.

These pictures are labeled ILLUSTRATIVE PREVIEWS. They are NOT screenshots
captured from a running game or emulator. Live content is represented by
neutral example labels; replace the README images with real AYN Thor
screenshots after the complete v1.1.0 package passes device QA.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[2]
ASSETS=ROOT/"packages/Persona3Portable/dualscreen/img"
OUTPUT=ROOT/"packages/Persona3Portable/docs"


def create_preview(route:int,font_path:Path)->Image.Image:
    bg=Image.open(ASSETS/f"bg_start_{route}.png").convert("RGBA")
    w,h=bg.size
    overlay=Image.new("RGBA",(w,h))
    dr=ImageDraw.Draw(overlay)
    theme=(21,72,149,245) if route==0 else (125,30,83,245)
    dr.rounded_rectangle((58,180,592,826),radius=24,fill=(246,251,255,237))
    dr.rounded_rectangle((58,180,592,288),radius=22,fill=theme)
    font_b=ImageFont.truetype(str(font_path),52)
    font_s=ImageFont.truetype(str(font_path),27)
    font_sm=ImageFont.truetype(str(font_path),19)
    dr.text((85,192),"SECOND SCREEN",font=font_b,fill="white")
    labels=["MAIN","PERSONA","ITEMS","SKILLS","STATUS","MAP"]
    for i,label in enumerate(labels):
        yy=325+i*71
        dr.rounded_rectangle((87,yy,557,yy+57),radius=11,
                             fill=(217,234,249,255) if i%2==0 else (232,242,255,255))
        dr.text((118,yy+12),label,font=font_s,fill=(9,44,100,255))
        dr.ellipse((506,yy+20,520,yy+34),fill=theme)
    dr.text((65,938),"ILLUSTRATIVE DESIGN PREVIEW • NOT DEVICE CAPTURE",
            font=font_sm,fill=(255,255,255,230))
    dr.text((65,979),"AYN THOR   /   PERSONA 3 PORTABLE   /   UI 1.1",
            font=font_sm,fill=(255,255,255,190))
    return Image.alpha_composite(bg,overlay).convert("RGB")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output",type=Path,default=OUTPUT)
    p.add_argument("--font",type=Path,default=Path(os.environ.get(
        "P3P_INTER_FONT_DIR","/usr/share/fonts/opentype/inter"))/"Inter-BlackItalic.otf")
    args=p.parse_args()
    if not args.font.is_file():
        raise FileNotFoundError(args.font)
    args.output.mkdir(parents=True,exist_ok=True)
    for route,name in [(0,"male-route-preview.png"),(1,"female-route-preview.png")]:
        create_preview(route,args.font).save(args.output/name,optimize=True)
    print("Wrote two correctly labeled illustrative preview images")


if __name__=="__main__":
    main()
