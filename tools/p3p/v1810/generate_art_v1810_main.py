#!/usr/bin/env python3
"""Recreate v1.8.10 FINAL's three main-screen title/watermark graphics.

The tested design's main header uses the same historical Inter Black Italic
generator as the 31 v1.8 procedural assets. Its ghost lettering is at
x=47, bottom=970, alpha=74, independent of other screens' watermark layout.
No Inter font binary or source PNG is included in this repository.
"""
from __future__ import annotations
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from generate_art_v18_partial import draw_title_image, font_title

def draw_main_ghost(path: Path, female: bool) -> None:
    ss = 3
    image = Image.new('RGBA', (1240*ss, 1080*ss), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(str(font_title), 154*ss)
    word = 'MAIN'
    bb = draw.textbbox((0, 0), word, font=font)
    textw, texth = bb[2]-bb[0], bb[3]-bb[1]
    layer = Image.new('RGBA', (textw+30*ss, texth+15*ss), (0, 0, 0, 0))
    fill = (187, 31, 97, 74) if female else (13, 123, 220, 74)
    ImageDraw.Draw(layer).text((10*ss-bb[0], 6*ss-bb[1]), word,
                               font=font, fill=fill)
    if layer.width > 840*ss:
        layer = layer.resize((840*ss, layer.height), Image.Resampling.LANCZOS)
    image.alpha_composite(layer, (47*ss, 970*ss-layer.height))
    image.resize((1240, 1080), Image.Resampling.LANCZOS).save(path)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    draw_title_image('MAIN', args.output / 'ui_title_main.png')
    draw_main_ghost(args.output / 'ui_ghost_main_blue.png', False)
    draw_main_ghost(args.output / 'ui_ghost_main_red.png', True)
    print('Generated 3 exact MAIN-screen title and watermark PNGs')

if __name__ == '__main__':
    main()
