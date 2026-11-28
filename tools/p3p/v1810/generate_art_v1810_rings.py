#!/usr/bin/env python3
"""Rebuild the two exact-pixel, original v1.8.10 circular UI decorations.

Neither source PNG is embedded or read. The image is rasterized from circles
and lines at 2x resolution, then Lanczos-downsampled. Rounding corrections
are limited to specific angles where floating-point endpoint truncation in
the historical drawing path shifts a high-resolution coordinate by 1 pixel.

This source produces byte-for-byte identical *RGBA pixels*, but PNG encoding
bytes can vary across Pillow/zlib versions.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw

CANVAS = 800
SS = 2
CENTER = 400
WHITE = (255, 255, 255)


def _endpoints(angle: int, inner: int, outer: int) -> list[tuple[int, int]]:
    theta = math.radians(angle)
    return [
        (
            int((CENTER + r * math.cos(theta)) * SS),
            int((CENTER + r * math.sin(theta)) * SS),
        )
        for r in (inner, outer)
    ]


def _make_ring_a() -> Image.Image:
    """The 60 fine ticks and thin outline used by the original home dial."""
    image = Image.new("RGBA", (CANVAS * SS, CANVAS * SS))
    draw = ImageDraw.Draw(image)
    draw.ellipse((12, 12, 1588, 1588), outline=(*WHITE, 110), width=6)
    for angle in range(0, 360, 6):
        points = _endpoints(angle, 360, 388)
        # Legacy render-rounding reconstruction, validated against every
        # decoded pixel of the final v1.8.10 package.
        if angle in (0, 30):
            points = [(x, y - 1) for x, y in points]
        elif angle in (240, 270):
            points = [(x + 1, y) for x, y in points]
        draw.line(points, fill=(*WHITE, 170), width=8)
    return image.resize((CANVAS, CANVAS), Image.Resampling.LANCZOS)


def _make_ring_b() -> Image.Image:
    """The 12 wider cardinal ticks; no outlines or external textures."""
    image = Image.new("RGBA", (CANVAS * SS, CANVAS * SS))
    draw = ImageDraw.Draw(image)
    for angle in range(0, 360, 30):
        points = _endpoints(angle, 320, 388)
        if angle == 30:
            x, y = points[-1]
            points[-1] = (x, y - 1)
        elif angle == 270:
            points = [(x + 1, y) for x, y in points]
        draw.line(points, fill=(*WHITE, 120), width=20)
    return image.resize((CANVAS, CANVAS), Image.Resampling.LANCZOS)


def generate_all(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    _make_ring_a().save(output / "ring_a.png")
    _make_ring_b().save(output / "ring_b.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate_all(args.output)
    print("Regenerated both exact-pixel v1.8.10 ring decorations")


if __name__ == "__main__":
    main()
