#!/usr/bin/env python3
"""Regenerate 15 pixel-exact v1.8.10 UI geometric assets without source PNGs.

The original custom geometry was reverse-engineered against the community-tested
v1.8.10 reference archive. A 3x antialiased canvas reproduces the exact pixels
of the blue/pink main, page and battle backgrounds, accent bands, and slants.
PNG *bytes* can differ across encoder/library versions; see the pixel verifier.

Requires Pillow; uses no external artwork, fonts, game files, or compiled modules.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from PIL import Image, ImageDraw

WIDTH, HEIGHT, SCALE = 1240, 1080, 3
GOLD = (251, 180, 55, 255)
NAVY = ((11, 58, 145, 255), (106, 10, 30, 255))
BASE = ((30, 166, 238, 255), (232, 70, 124, 255))
PALE = ((147, 212, 248, 255), (255, 184, 206, 255))

# The triangles and ribbons use native-canvas endpoints, scaled *before*
# rasterization. The page's leading ribbon deliberately has a different
# slope from its following gold/navy ribbons; unifying them changes pixels.
BANDS = {
    'main': (
        ('navy', ((1240, 536), (1240, 1080), (696, 1080))),
        ('gold', ((1240, 560), (1240, 640), (800, 1080), (720, 1080))),
        ('navy', ((1240, 640), (1240, 1080), (800, 1080))),
    ),
    'page': (
        ('navy', ((1240, 940), (1240, 954), (1100, 1080), (1086, 1080))),
        ('gold', ((1240, 954), (1240, 994), (1154, 1080), (1114, 1080))),
        ('navy', ((1240, 994), (1240, 1080), (1154, 1080))),
    ),
    'battle': (
        ('navy', ((1240, 966), (1240, 1080), (1126, 1080))),
        ('gold', ((1240, 978), (1240, 1010), (1170, 1080), (1138, 1080))),
        ('navy', ((1240, 1010), (1240, 1080), (1170, 1080))),
    ),
}
BACKDROPS = {
    'main': (780, 560, 44),
    'page': (820, 600, 28),
    'battle': (860, 1000, 44),
}

def polygon(draw: ImageDraw.ImageDraw, vertices, fill) -> None:
    draw.polygon([(x * SCALE, y * SCALE) for x, y in vertices], fill=fill)

def add_band(draw: ImageDraw.ImageDraw, page: str, route: int) -> None:
    for color, vertices in BANDS[page]:
        polygon(draw, vertices, GOLD if color == 'gold' else NAVY[route])

def generate_band(page: str, route: int) -> Image.Image:
    image = Image.new('RGBA', (WIDTH * SCALE, HEIGHT * SCALE))
    add_band(ImageDraw.Draw(image), page, route)
    return image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

def generate_background(page: str, route: int) -> Image.Image:
    image = Image.new('RGBA', (WIDTH * SCALE, HEIGHT * SCALE), BASE[route])
    draw = ImageDraw.Draw(image)
    diagonal_top, diagonal_bottom, left_stripe = BACKDROPS[page]
    polygon(draw, ((diagonal_top, 0), (WIDTH, 0),
                   (WIDTH, HEIGHT), (diagonal_bottom, HEIGHT)), PALE[route])
    draw.rectangle((0, 0, left_stripe * SCALE, HEIGHT * SCALE), fill=GOLD)
    if page != 'main':
        add_band(draw, page, route)
    return image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

def generate_slant(width: int, height: int) -> Image.Image:
    image = Image.new('RGBA', (width * SCALE, height * SCALE))
    draw = ImageDraw.Draw(image)
    draw.polygon(((26 * SCALE, 0), (width * SCALE, 0),
                  ((width - 26) * SCALE, height * SCALE), (0, height * SCALE)),
                 fill=(255, 255, 255, 255))
    return image.resize((width, height), Image.Resampling.LANCZOS)

def generate_moon_mask() -> Image.Image:
    # The circular mask was rendered independently at 4x resolution.
    scale = 4
    image = Image.new('RGBA', (660 * scale, 660 * scale))
    ImageDraw.Draw(image).ellipse((8 * scale, 8 * scale, 652 * scale, 652 * scale),
                                  fill=(255, 255, 255, 255))
    return image.resize((660, 660), Image.Resampling.LANCZOS)

def generate_all(destination: Path) -> list[Path]:
    destination.mkdir(parents=True, exist_ok=True)
    generated = []
    for page in ('main', 'page', 'battle'):
        for route in (0, 1):
            for name, image in (
                    (f'band_{page}_{route}.png', generate_band(page, route)),
                    (f'bg_{page}_{route}.png', generate_background(page, route))):
                path = destination / name
                image.save(path)
                generated.append(path)
    for name, width, height in (('slant.png', 600, 92),
                                ('slant_wide.png', 1200, 96)):
        path = destination / name
        generate_slant(width, height).save(path)
        generated.append(path)
    mask_path = destination / 'moon_mask.png'
    generate_moon_mask().save(mask_path)
    generated.append(mask_path)
    return generated

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    files = generate_all(args.output)
    print(f'Generated {len(files)} independently reconstructed geometric PNGs')

if __name__ == '__main__':
    main()
