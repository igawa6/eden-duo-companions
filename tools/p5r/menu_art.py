#!/usr/bin/env python3
"""menu_art -- real-game-art helpers for the P5R START MENU pages (m_ui).

Everything here is cut from the unpacked romfs (P5R_ROMFS) or the already-extracted
P5CAMP_00SPD sprites (spd_extract.py) and pre-scaled to the exact rect it is
drawn at, because the runtime samples images nearest-neighbour. Every file gets a provenance entry.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image

from p5r_paths import CMM_FORMAT, ROMFS  # noqa: E402
ICON_DDS = ROMFS / 'EN/FONT/ICON.DDS'
CHARATEX = ROMFS / 'EN/CAMP/CHARATEX'
HEROTEX = ROMFS / 'BASE/CAMP/HEROTEX'
CARDTEX = ROMFS / 'EN/CAMP/CARDTEX'

# EN/FONT/ICON.DDS: 6 x 14 cells, pitch 126 x 45, content 122 x 41 at (3 + 126c, 3 + 45r)
ICON_COLS, ICON_ROWS = 6, 14


def icon_cell(n: int) -> Image.Image:
    im = Image.open(ICON_DDS).convert('RGBA')
    c, r = n % ICON_COLS, n // ICON_COLS
    return im.crop((3 + 126 * c, 3 + 45 * r, 3 + 126 * c + 122, 3 + 45 * r + 41))


def icon_nonempty(n: int) -> bool:
    cell = icon_cell(n)
    # an empty cell is the plain dark rounded plate: no bright pixels
    return any(max(p[:3]) > 90 and p[3] > 128 for p in cell.getdata())


def dds(path: Path) -> Image.Image:
    return Image.open(path).convert('RGBA')


def alpha_bands(img: Image.Image, min_gap: int = 8):
    """Vertical runs of rows that hold any alpha (name art / bust / silhouette of a CHARATEX)."""
    a = img.getchannel('A')
    W, H = img.size
    rows = [a.crop((0, y, W, y + 1)).getbbox() is not None for y in range(H)]
    runs, s = [], None
    for i, v in enumerate(rows + [False]):
        if v and s is None:
            s = i
        if not v and s is not None:
            runs.append((s, i))
            s = None
    return runs


def chara_parts(stem: str):
    """(name_art, bust) crops of EN/CAMP/CHARATEX/<stem>.DDS.

    Layout of every 768x3072 sheet: name word art in the first band (y < 300), the bust is the
    band that ends around y = 2020, the white silhouette fills the bottom. Igor has an extra arm
    band (y 537..968) that the native screen composites over the bust; it is left out."""
    im = dds(CHARATEX / f'{stem.upper()}.DDS')
    bands = alpha_bands(im)
    name = next(b for b in bands if b[0] < 300)
    bust = max((b for b in bands if 1800 < b[1] < 2100), key=lambda b: b[1] - b[0])
    n = im.crop((0, name[0], im.width, name[1]))
    n = n.crop(n.getbbox())
    b = im.crop((0, bust[0], im.width, bust[1]))
    b = b.crop(b.getbbox())
    return n, b, {'name_band': list(name), 'bust_band': list(bust)}


def cmm_chara_ids() -> dict[int, int]:
    """Confidant entry id -> CHARATEX number: cmmFormat.ctd row(id) + 0x2A (u16 BE).

    Rows are 0xBC bytes from 0x30 (FTD header: row count u32 BE at 0x28 = 38). Verified against the sheet name art:
    1 -> 30 Igor, 2 -> 3 Morgana, 8 -> 2 Ryuji, 6 -> 16 Sojiro, 35 -> 31 Maruki ..."""
    d = CMM_FORMAT.read_bytes()
    count = int.from_bytes(d[0x28:0x2C], 'big')
    out = {}
    for i in range(count):
        row = d[0x30 + 0xBC * i: 0x30 + 0xBC * (i + 1)]
        out[i] = int.from_bytes(row[0x2A:0x2C], 'big')
    return out


def fit(img: Image.Image, w=None, h=None) -> Image.Image:
    if w and h:
        k = min(w / img.width, h / img.height)
    elif w:
        k = w / img.width
    else:
        k = h / img.height
    return img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)


def tint(img: Image.Image, mul) -> Image.Image:
    """Multiply RGB by mul (r, g, b) in 0..255 -- bake a darker copy (runtime tint also works)."""
    r, g, b, a = img.split()
    r = r.point(lambda v: v * mul[0] // 255)
    g = g.point(lambda v: v * mul[1] // 255)
    b = b.point(lambda v: v * mul[2] // 255)
    return Image.merge('RGBA', (r, g, b, a))


def chara_parts_art(stem: str):
    """chara_parts() as recipe Art: (name_art, bust, bands), crops baked from the local sheet."""
    from recipe import Art
    rel = f'EN/CAMP/CHARATEX/{stem.upper()}.DDS'
    im = dds(CHARATEX / f'{stem.upper()}.DDS')
    bands = alpha_bands(im)
    name = next(b for b in bands if b[0] < 300)
    bust = max((b for b in bands if 1800 < b[1] < 2100), key=lambda b: b[1] - b[0])
    sheet = Art.dds(rel)
    n = sheet.crop((0, name[0], im.width, name[1])).bbox()
    b = sheet.crop((0, bust[0], im.width, bust[1])).bbox()
    return n, b, {'name_band': list(name), 'bust_band': list(bust)}
