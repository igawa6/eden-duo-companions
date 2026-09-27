"""Thieves Den music-player art (EN/MYPALACE/SOUND) for the MUSIC page and the bottom-bar mini player.

Sprites are cut straight out of the SPR0 containers (same layout as spd_extract.py:
texture entry 0x30 = id, _, offset, size, w, h, name; sprite entry 0xA0 = id, texture id, x/y/w/h at
+0x20, SJIS name at +0x70), in memory, so the build needs nothing but the unpacked romfs.

MENU.SPD             41 sprites: 0 "MUSIC" logo, 1-4 red category slabs, 5-8 / 34-37 category words
MUSIC_TITLE_001.SPD 107 sprites: sprite i = the title art the Thieves Den list draws for row i of
                     mypSoundNameTable (EN/INIT/MYPTABLE.BIN, the P5R_TRACKS JSON);
                     7 rows (83-86, 90, 91, 99) are fully transparent (tracks the list never shows).
"""
from __future__ import annotations

import io
import json
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

from recipe import Art

from p5r_paths import ROMFS  # noqa: E402
MENU_SPD = 'EN/MYPALACE/SOUND/MENU.SPD'
TITLE_SPD = 'EN/MYPALACE/SOUND/MUSIC_TITLE_001.SPD'
from p5r_paths import TRACKS  # noqa: E402

_cache: dict[str, dict[int, Image.Image]] = {}


def spd(rel: str) -> dict[int, Image.Image]:
    if rel in _cache:
        return _cache[rel]
    d = (ROMFS / rel).read_bytes()
    tc, sc = struct.unpack_from('<HH', d, 0x14)
    to, so = struct.unpack_from('<II', d, 0x18)
    tex = {}
    for i in range(tc):
        tid, _, off, size, _w, _h = struct.unpack_from('<IIIIII', d, to + i * 0x30)
        tex[tid] = Image.open(io.BytesIO(d[off:off + size])).convert('RGBA')
    out = {}
    for i in range(sc):
        b = so + i * 0xA0
        sid, tid = struct.unpack_from('<II', d, b)
        x, y, w, h = struct.unpack_from('<IIII', d, b + 0x20)
        if w and h and tid in tex:
            out[sid] = tex[tid].crop((x, y, x + w, y + h))
    _cache[rel] = out
    return out


def menu(sid: int) -> Art:
    return Art.sprite('musmenu', sid)


def title(i: int) -> Art | None:
    """Native title art of Thieves Den row i, alpha-cropped; None for a blank row."""
    im = spd(TITLE_SPD).get(i)
    if im is None:
        return None
    bb = im.getchannel('A').getbbox()
    # crop horizontally only: every row keeps the native 38 px line box, so all titles share one
    # baseline and one glyph scale (a vertical crop would rescale titles without descenders)
    return Art.sprite('mustitle', i).crop((bb[0], 0, bb[2], im.height)) if bb and bb[2] - bb[0] >= 8 else None


def tracks() -> list[dict]:
    rows = json.loads(TRACKS.read_text())
    for r in rows:
        # the table's two non-ASCII titles decode as U+FFFD pairs (a star / an ampersand-like mark);
        # the art shows them, the text fallback only needs to stay printable
        r['clean'] = r['name'].replace('��', ' & ' if 'Billiards' in r['name'] else '').strip()
    return rows


def fade_right(im: Image.Image, w: int, fade: int = 56) -> Image.Image:
    """Crop to width w; if the art is wider, fade its last `fade` px out (a graphic ellipsis)."""
    if im.width <= w:
        return im
    im = im.crop((0, 0, w, im.height))
    a = im.getchannel('A')
    ramp = Image.new('L', (fade, im.height))
    ramp.putdata([round(255 * (1 - x / (fade - 1))) for _y in range(im.height) for x in range(fade)])
    tail = ImageChops.multiply(a.crop((w - fade, 0, w, im.height)), ramp)
    a.paste(tail, (w - fade, 0))
    im.putalpha(a)
    return im


def disc(size: int, label=(229, 25, 28, 255), idle=False) -> Art:
    """Vinyl record (generated, 4x supersampled): black disc, groove rings, red label, spindle hole.
    idle=True = the stopped look (grey label)."""
    s = 4
    n = size * s
    im = Art.new(n, n).ellipse([0, 0, n - 1, n - 1], (255, 255, 255, 255))   # white rim (P5 outline)
    r0 = int(n * 0.03)
    im = im.ellipse([r0, r0, n - 1 - r0, n - 1 - r0], (12, 10, 11, 255))
    for k, frac in enumerate((0.42, 0.36, 0.30, 0.24)):
        r = int(n * frac)
        c = n // 2
        im = im.ellipse_outline([c - r, c - r, c + r, c + r], (52, 44, 48, 255) if k % 2 else (36, 30, 33, 255),
                                max(1, s))
    c = n // 2
    im = im.ellipse([c - int(n * 0.19), c - int(n * 0.19), c + int(n * 0.19), c + int(n * 0.19)], (12, 10, 11, 255))
    lr = int(n * 0.16)
    im = im.ellipse([c - lr, c - lr, c + lr, c + lr], (110, 100, 104, 255) if idle else label)
    # a notch in the label (asymmetric, shows the spin)
    im = im.pieslice([c - lr, c - lr, c + lr, c + lr], 20, 70, (12, 10, 11, 255))
    hr = max(2 * s, int(n * 0.025))
    im = im.ellipse([c - hr, c - hr, c + hr, c + hr], (255, 255, 255, 255))
    return im.resize(size, size)
