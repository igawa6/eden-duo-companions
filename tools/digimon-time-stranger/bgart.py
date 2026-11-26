# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Background art for the companion (0.31.0): teal hex mesh, plus a data-rain corner for the Digimon summary page.
All drawn here from code: no game pixels. Seeds are fixed so every build draws the same picture."""
import math
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter

W, H = 1240, 1080
HEX_R = 34
CYAN = (43, 228, 255)          # the summary ring's own colour
PAL = {"top": (6, 28, 46), "bot": (3, 13, 26), "hex": (40, 150, 190), "hexfill": (35, 160, 200),
       "topglow": (22, 140, 190), "streak": (70, 215, 235)}
# Rain zone on the summary page: right side under the ring and the bottom-right block. Checked against 69 Digimon
# summaries (long "Wild in" lists included): no text ever reaches it.
RAIN_BOX = (846, 400, W, H)
GLYPHS = {"0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"]}


def _grad(top, bot):
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return im


def _glow(im, cx, cy, r, col, a):
    g = Image.new("RGB", (W, H), (0, 0, 0))
    ImageDraw.Draw(g).ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
    g = g.filter(ImageFilter.GaussianBlur(r / 2.2))
    return ImageChops.add(im, Image.eval(g, lambda v: int(v * a)))


def _vignette(im, s=0.55):
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).ellipse([-W * .25, -H * .3, W * 1.25, H * 1.3], fill=255)
    m = m.filter(ImageFilter.GaussianBlur(160))
    return Image.composite(im, Image.eval(im, lambda v: int(v * (1 - s))), m)


def _hexes():
    h = HEX_R * math.sqrt(3)
    for j in range(-1, int(H / h) + 2):
        for i in range(-1, int(W / (1.5 * HEX_R)) + 2):
            cx = i * 1.5 * HEX_R
            cy = j * h + (h / 2 if i % 2 else 0)
            yield cx, cy, [(cx + HEX_R * math.cos(math.pi / 3 * k), cy + HEX_R * math.sin(math.pi / 3 * k))
                           for k in range(6)]


def _glyph(d, x, y, c, col, s):
    for r, row in enumerate(GLYPHS[c]):
        for k, ch in enumerate(row):
            if ch == "1":
                d.rectangle([x + k * s, y + r * s, x + k * s + s - 1, y + r * s + s - 1], fill=col)


def draw_bg():
    random.seed(7)
    im = _grad(PAL["top"], PAL["bot"])
    d = ImageDraw.Draw(im, "RGBA")
    for cx, cy, pts in _hexes():
        dist = math.hypot(cx - W * .8, cy - H * .15) / W
        a = max(14, int(120 * (1 - dist * 1.3)))
        d.polygon(pts, outline=PAL["hex"] + (a,))
        if random.random() < .04:
            d.polygon(pts, fill=PAL["hexfill"] + (a // 3,))
    im = _glow(im, int(W * .8), int(H * .15), 380, PAL["topglow"], .5)
    im = _vignette(im)
    d = ImageDraw.Draw(im, "RGBA")
    for y in range(0, H, 4):                       # faint scanlines
        d.line([(0, y), (W, y)], fill=(0, 0, 0, 10))
    random.seed(7)
    for _ in range(14):                            # faint light streaks
        x = random.randrange(40, W - 40, 34)
        y0 = random.randint(0, H - 300)
        n = random.randint(120, 320)
        for k in range(n):
            d.point((x, y0 + k), fill=PAL["streak"] + (int(38 * (k / n) ** 2),))
    return im.convert("RGBA")


def draw_dx_rain():
    """Summary page only: data falling down the hex columns out of the ring, pooling in a corner glow.
    Returned cropped to RAIN_BOX (place it there)."""
    import numpy as np
    random.seed(11)
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    for _, _, pts in _hexes():
        d.polygon(pts, outline=CYAN + (70,))
        if random.random() < .16:
            d.polygon(pts, fill=CYAN + (random.randint(14, 34),))
    for x in sorted({int(i * 1.5 * HEX_R) for i in range(16, 25)}):
        top = 410 if x >= 1160 else 892
        for _ in range(3):
            n = random.randint(7, 18)
            y0 = random.randint(top - 200, H - 40)
            for j in range(n):
                y = y0 + j * 14
                if y < top or y > H - 14:
                    continue
                a = 255 if j == n - 1 else int(170 * (j / n) ** 1.8) + 12
                _glyph(d, x - 3, y, random.choice("01"), CYAN + (a,), 2)
    g = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(g).ellipse([980, 760, 1400, 1200], fill=CYAN + (60,))
    lay = Image.alpha_composite(g.filter(ImageFilter.GaussianBlur(90)), lay)
    m = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(m)
    md.rectangle([1160, 400, W, H], fill=255)
    md.rectangle([846, 892, W, H], fill=255)
    m = m.filter(ImageFilter.GaussianBlur(22))
    ramp = Image.new("L", (W, H))
    rd = ImageDraw.Draw(ramp)
    for x in range(W):
        rd.line([(x, 0), (x, H)], fill=int(255 * min(1, max(0, (x - 800) / 300)) ** 0.8))
    m = ImageChops.multiply(m, ramp)
    a = np.asarray(lay).copy()
    a[..., 3] = (a[..., 3].astype(int) * np.asarray(m) // 255).astype("uint8")
    return Image.fromarray(a).crop(RAIN_BOX)


def draw_bg_even():
    """0.32.0 background for every page but the summary: one even teal-navy, the hex mesh at one strength,
    a faint glow across the top only (it fades out by mid-screen), no shading anywhere."""
    random.seed(7)
    im = Image.new("RGB", (W, H), (9, 34, 56))
    d = ImageDraw.Draw(im, "RGBA")
    for cx, cy, pts in _hexes():
        d.polygon(pts, outline=PAL["hex"] + (72,))
        if random.random() < .05:
            d.polygon(pts, fill=PAL["hexfill"] + (random.randint(10, 22),))
    g = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(g)
    for y in range(0, 560):
        gd.line([(0, y), (W, y)], fill=(30, 140, 190, int(80 * (1 - y / 560) ** 2)))
    im = Image.alpha_composite(im.convert("RGBA"), g).convert("RGB")
    d = ImageDraw.Draw(im, "RGBA")
    for cx, cy, pts in _hexes():
        if cy < 560:
            d.polygon(pts, outline=PAL["hex"] + (int(90 * (1 - cy / 560) ** 2),))
    for y in range(0, H, 4):
        d.line([(0, y), (W, y)], fill=(0, 0, 0, 8))
    random.seed(7)
    for _ in range(14):
        x = random.randrange(40, W - 40, 34)
        y0 = random.randint(0, H - 300)
        n = random.randint(120, 320)
        for k in range(n):
            d.point((x, y0 + k), fill=PAL["streak"] + (int(38 * (k / n) ** 2),))
    return im.convert("RGBA")


def _chamfer_box(w, h, c, fill, edge, s=2):
    import numpy as np
    ww, hh, cc = w * s, h * s, c * s
    poly = [(cc, 0), (ww, 0), (ww, hh - cc), (ww - cc, hh), (0, hh), (0, cc)]
    im = Image.new("RGBA", (ww, hh), (0, 0, 0, 0))
    ImageDraw.Draw(im).polygon(poly, fill=fill)
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh)
    for k in range(hh):
        sd.line([(0, k), (ww, k)], fill=CYAN[:3] + (int(40 * (1 - k / hh) ** 2),))
    m = Image.new("L", im.size, 0)
    ImageDraw.Draw(m).polygon(poly, fill=255)
    sh.putalpha(Image.fromarray(np.minimum(np.asarray(sh.split()[3]), np.asarray(m))))
    im = Image.alpha_composite(im, sh)
    d = ImageDraw.Draw(im)
    d.line(poly + [poly[0]], fill=edge, width=2 * s)
    d.line([(cc + 10 * s, 5 * s), (cc + 150 * s, 5 * s)], fill=CYAN[:3] + (150,), width=2 * s)
    return im, d, s


def draw_locbar(w, h, details_w):
    """The location bar: cut corners like the buttons, cyan edge, highlight dash, a round pin badge on the
    left and a divider before the right-hand details."""
    im, d, s = _chamfer_box(w, h, 18, (11, 38, 71, 245), CYAN + (255,))
    hh = h * s
    bx, by, br = 46 * s, hh // 2, 19 * s
    d.ellipse([bx - br, by - br, bx + br, by + br], fill=(6, 22, 40, 255), outline=CYAN + (255,), width=2 * s)
    pw = 11 * s
    d.ellipse([bx - pw * .62, by - pw * .95, bx + pw * .62, by + pw * .3], fill=CYAN + (255,))
    d.polygon([(bx - pw * .5, by), (bx + pw * .5, by), (bx, by + pw * .95)], fill=CYAN + (255,))
    d.ellipse([bx - pw * .25, by - pw * .6, bx + pw * .25, by - pw * .1], fill=(6, 22, 40, 255))
    x = (w - details_w) * s
    d.line([(x, 12 * s), (x, hh - 12 * s)], fill=(29, 77, 115, 255), width=2 * s)
    return im.resize((w, h), Image.LANCZOS)


def draw_live_dot(sz=26):
    im = Image.new("RGBA", (sz * 4, sz * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    s = sz * 4
    d.ellipse([s * .05, s * .05, s * .95, s * .95], fill=CYAN + (80,))
    im = im.filter(ImageFilter.GaussianBlur(s * .08))
    d = ImageDraw.Draw(im)
    d.ellipse([s * .32, s * .32, s * .68, s * .68], fill=(150, 245, 255, 255))
    return im.resize((sz, sz), Image.LANCZOS)


def draw_unknown_boss(sz=150):
    """Round stand-in for a boss with no dex picture: a dark disc with a cyan ring and a question mark."""
    s = 4
    im = Image.new("RGBA", (sz * s, sz * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = sz * s
    d.ellipse([2 * s, 2 * s, r - 2 * s, r - 2 * s], fill=(6, 22, 40, 255), outline=CYAN + (200,), width=3 * s)
    # question mark from strokes (our own drawing)
    cx, cy = r / 2, r * .44
    d.arc([cx - r * .16, cy - r * .2, cx + r * .16, cy + r * .1], 200, 90, fill=CYAN + (255,), width=int(r * .055))
    d.line([(cx, cy + r * .1), (cx, cy + r * .2)], fill=CYAN + (255,), width=int(r * .055))
    d.ellipse([cx - r * .035, cy + r * .28, cx + r * .035, cy + r * .35], fill=CYAN + (255,))
    return im.resize((sz, sz), Image.LANCZOS)
