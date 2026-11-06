#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the package's own images (frames, rank diamond, small icons) as PNG.

  gen_ui_art.py [output-dir]   (default: packages/CultOfTheLamb/dualscreen/ui)

Everything here is our own pixel art built from patterns, no game graphics. Icons are white and
are tinted in the manifest (tint). Standard library only.
"""
import os
import struct
import sys
import zlib

# Colours as in the manifest (0xRRGGBB)
GOLD = 0xC9A24A
GOLD_LINE = 0x6E5730
CARD = 0x211720
DARK = 0x160F14
DIM_LINE = 0x4A3A40
DIM_INNER = 0x2E2228
DIM_FILL = 0x1B1319
RED = 0xA31F34
FAINT = 0x5E4E54

FRAME = 48      # edge length of the 9-slice frames; slice 12 per side
INNER = 8       # distance of the thin inner line from the edge
ICON_SCALE = 8  # 7×7 pattern → 56 px

ICONS = {
    "heart": [".##.##.", "#######", "#######", "#######", ".#####.", "..###..", "...#..."],
    "bowl": ["..#.#..", ".#.#...", "#######", "#######", ".#####.", "..###..", "......."],
    "skull": [".#####.", "#######", "#..#..#", "#######", ".##.##.", ".#####.", ".#.#.#."],
    "smile": [".#####.", "#.....#", "#.#.#.#", "#.....#", "#.#.#.#", "#..#..#", ".#####."],
    "flame": ["...#...", "..##...", "..###..", ".####..", ".#####.", "#######", ".#####."],
    "person": ["..###..", "..###..", "...#...", ".#####.", "#.###.#", "..#.#..", "..#.#.."],
    "glass": ["#######", ".#####.", "..###..", "...#...", "..###..", ".#####.", "#######"],
    "dung": ["...#...", "..###..", ".#####.", "..###..", ".#####.", "#######", "......."],
    "grave": ["..###..", ".#####.", ".##.##.", ".#...#.", ".##.##.", ".#####.", "#######"],
    "zz": ["#####..", "...#...", "..#....", "#####..", "....###", ".....#.", "....###"],
    "scroll": [".######", "#.....#", ".#....#", ".#.##.#", ".#....#", ".######", "......."],
    "seed": ["...#...", "..###..", ".#####.", ".#####.", "..###..", "...#...", "...#..."],
    "diamond": ["...#...", "..###..", ".#####.", "#######", ".#####.", "..###..", "...#..."],
    "silhouette": ["..###..", ".#####.", ".#####.", "..###..", ".#####.", "#######", "#######"],
    "sword": ["......#", ".....#.", "....#..", "#..#...", ".##....", ".##....", "#..#..."],
}
DIAMOND = ICONS["diamond"]


def rgba(c, a=255):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255, a)


CLEAR = (0, 0, 0, 0)
WHITE = (255, 255, 255, 255)


def write_png(path, px):
    """px: rows of (r, g, b, a)"""
    h, w = len(px), len(px[0])
    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + \
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def read_png(path):
    """Reads only what write_png writes (RGBA 8 bit, Filter 0)."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + 4 * w
    return [[tuple(raw[y * stride + 1 + 4 * x:y * stride + 5 + 4 * x]) for x in range(w)]
            for y in range(h)]


def frame(outer, outer_px, inner, fill, size=FRAME):
    px = [[rgba(fill)] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            d = min(x, y, size - 1 - x, size - 1 - y)
            if d < outer_px:
                px[y][x] = outer
            elif inner is not None and d == INNER:
                px[y][x] = rgba(inner)
    return px


def pattern(rows, on, scale=ICON_SCALE):
    return [[on if rows[y // scale][x // scale] == "#" else CLEAR for x in range(7 * scale)]
            for y in range(7 * scale)]


def write_all(out):
    os.makedirs(out, exist_ok=True)
    images = {
        "frame_gold": frame(rgba(GOLD), 2, GOLD_LINE, CARD),
        "frame_gold_sel": frame(rgba(GOLD), 5, GOLD_LINE, CARD),
        "frame_gold_dark": frame(rgba(GOLD), 2, GOLD_LINE, DARK),
        "frame_dim": frame(rgba(DIM_LINE), 2, DIM_INNER, DIM_FILL),
        # white edge, empty middle: tinted over a panel (badges, chips)
        "frame_line": [[WHITE if min(x, y, FRAME - 1 - x, FRAME - 1 - y) < 3 else CLEAR
                        for x in range(FRAME)] for y in range(FRAME)],
        "diamond": pattern(DIAMOND, rgba(RED)),
        # placeholder while no follower image is loaded (empty_src, not tinted)
        "portrait_empty": pattern(ICONS["silhouette"], rgba(FAINT)),
    }
    for name, rows in ICONS.items():
        images["icon_" + name] = pattern(rows, WHITE)
    for name, px in images.items():
        write_png(os.path.join(out, name + ".png"), px)
    return sorted(images)


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "packages", "CultOfTheLamb", "dualscreen", "ui")
    print("\n".join(write_all(out)))
