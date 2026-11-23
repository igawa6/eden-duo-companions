#!/usr/bin/env python3
"""Regenerate two original v1.8.10 footer-lettering assets byte-for-byte.

Requires a locally installed InterDisplay Bold Italic OTF. The font binary is
NOT bundled or redistributed. No image bytes, game assets, or game files are
embedded in this source.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = Path(os.environ.get('P3P_INTER_FONT_DIR', '/usr/share/fonts/opentype/inter')) / 'InterDisplay-BoldItalic.otf'
CANVAS = (1240, 1080)
INK = (86, 199, 248, 77)
LETTERS = (
    ('calendar_footer_lettering.png', 'CALENDAR', (568, 850)),
    ('links_footer_lettering.png', 'S.LINK', (823, 811)),
)

def generate(directory: Path) -> None:
    if not FONT.is_file():
        raise RuntimeError(
            'This historical reconstruction requires locally installed '
            'InterDisplay Bold Italic OTF; no font binary is distributed.'
        )
    directory.mkdir(parents=True, exist_ok=True)
    font = ImageFont.truetype(str(FONT), 118)
    for name, letters, position in LETTERS:
        image = Image.new('RGBA', CANVAS, (0, 0, 0, 0))
        ImageDraw.Draw(image).text(position, letters, font=font, fill=INK)
        image.save(directory / name)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.output)
    print('Generated 2 exact v1.8.10 footer lettering PNGs')

if __name__ == '__main__':
    main()
