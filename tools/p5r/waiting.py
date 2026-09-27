"""Loading / "waiting for gameplay" art: three Phantom Thieves silhouettes over a red slash,
the game's own loading hearts, the calling-card emblem, and the P5 dialogue font.

compose(kit) -> (src, w, h) of one pre-rendered 1240x1000 image (the runtime samples nearest,
so everything is drawn here at final size with a proper filter). Every image is an Art (recipe.py):
PNG mode writes it, module mode ships its recipe.
"""
import math  # noqa: F401
from pathlib import Path

from recipe import Art

from p5r_paths import FONT_DIR as FONT  # noqa: E402,F401
SRC = {
    'partypanel': 'EN/BATTLE/GUI/P5_BATTLE_PARTYPANEL.SPD',
    'nowload': 'EN/FIELD/PANEL/WIPE/SYMBOL/NOWLOADING.SPD',
    'saferoom': 'EN/FIELD/SAFETY_ROOM/P5_AZITO_SAFEROOM.SPD',
    'camp00': 'EN/INIT/P5CAMP_00SPD.SPD',
}
RED = (229, 25, 28, 255)
W, H = 1240, 1000


def _sprite(kind, sid):
    return Art.sprite(kind, sid)


def _fit(img, h):
    k = h / img.height
    return img.resize(max(1, round(img.width * k)), h)


def _tint(img, rgb, alpha=1.0):
    assert alpha == 1.0
    return img.recolor(tuple(rgb[:3]) + (255,))


def _text(kit, text, cap, rgb, track=0.0):
    return Art.text(text, cap, tuple(rgb[:3]) + (255,), track)


def _paste_rot(base, img, cx, cy, deg):
    r = img.rotate(deg)
    return base.over(r, int(cx - r.width / 2), int(cy - r.height / 2))


def compose(kit, name='waiting_art'):
    im = Art.new(W, H)

    # Calling-card logo (white line art), large and dark red, as a watermark behind everything.
    logo = _sprite('saferoom', 33)
    im = _paste_rot(im, _tint(_fit(logo, 600), (95, 10, 16)), 1030, 230, -10)
    im = _paste_rot(im, _tint(_fit(logo, 300), (70, 8, 12)), 130, 880, 14)

    # Red slash band behind the trio, with a thin black cut through it.
    im = im.polygon([(-20, 330), (W + 20, 180), (W + 20, 560), (-20, 690)], RED)
    im = im.polygon([(-20, 648), (W + 20, 512), (W + 20, 524), (-20, 662)], (16, 12, 14, 255))

    # Joker (centre, largest), Ryuji left, Morgana right; black drop shadow for the P5 cut-out look.
    trio = [(2, 300, 330, 475, 4), (1, 380, 620, 440, -2), (3, 290, 905, 470, -5)]
    for sid, h, cx, cy, deg in trio:
        s = _fit(_sprite('partypanel', sid), h)
        im = _paste_rot(im, _tint(s, (0, 0, 0)), cx + 14, cy + 14, deg)
        im = _paste_rot(im, s, cx, cy, deg)

    # The game's own loading-screen hearts.
    hearts = [(3, 110, 140, 250, -18), (4, 64, 1085, 610, 14), (5, 40, 190, 700, 10),
              (4, 52, 470, 225, -8), (5, 34, 780, 250, 20), (3, 84, 1120, 380, 10)]
    for sid, h, cx, cy, deg in hearts:
        im = _paste_rot(im, _fit(_sprite('nowload', sid), h), cx, cy, deg)

    # Title and subtitle in the decoded EN dialogue font.
    t1 = _text(kit, 'Stealing Your Heart', 78, (255, 255, 255), track=1)
    pw, ph = t1.width + 90, t1.height + 36
    plate = Art.new(pw, ph).polygon([(18, 0), (pw, 6), (pw - 18, ph), (0, ph - 8)], (0, 0, 0, 255))
    plate = plate.over(t1, 45, 14)
    im = _paste_rot(im, plate, W // 2, 790, 2)
    t2 = _text(kit, 'Preparing the heist...', 44, (255, 110, 170))
    im = _paste_rot(im, t2, W // 2 + 40, 900, 2)

    src = kit._save(im, name, {
        'source': 'composite', 'parts': [
            f"{SRC['partypanel']} #1 #2 #3 (silhouettes)",
            f"{SRC['nowload']} #3 #4 #5 (loading hearts)",
            f"{SRC['saferoom']} #33 (calling-card logo, tinted)",
            'EN/FONT/FONT0.FNT (text)'],
        'size': [W, H]})
    return src, W, H


def take_your_time(kit, height=46):
    img = _fit(_sprite('nowload', 2), height)
    src = kit._save(img, 'take_your_time', {'source': SRC['nowload'], 'sprite_id': 2, 'size': list(img.size)})
    return src, img.width, img.height


def compose_mismatch(kit, version='v1.0.2', name='mismatch_art'):
    """Wrong game build: the trio greyed out behind a torn slash, the calling card crossed out."""
    im = Art.new(W, H)
    logo = _sprite('saferoom', 33)
    im = _paste_rot(im, _tint(_fit(logo, 600), (60, 60, 64)), 1030, 230, -10)
    im = _paste_rot(im, _tint(_fit(logo, 300), (44, 44, 48)), 130, 880, 14)

    # The slash is torn in two: dark grey halves offset along a jagged cut, red seam between them.
    im = im.polygon([(-20, 330), (560, 250), (600, 330), (540, 420), (620, 520), (-20, 690)], (58, 52, 56, 255))
    im = im.polygon([(610, 246), (W + 20, 180), (W + 20, 560), (650, 612), (590, 516), (668, 410), (628, 330)],
                    (58, 52, 56, 255))
    im = im.line([(560, 250), (600, 330), (540, 420), (620, 520), (590, 690)], RED, 10)

    trio = [(2, 300, 330, 475, 4), (1, 380, 620, 440, -2), (3, 290, 905, 470, -5)]
    for sid, h, cx, cy, deg in trio:
        s = _fit(_sprite('partypanel', sid), h)
        im = _paste_rot(im, _tint(s, (0, 0, 0)), cx + 14, cy + 14, deg)
        im = _paste_rot(im, _tint(s, (120, 114, 118)), cx, cy, deg)

    # Big red X over the trio (P5CAMP_00SPD #654, the confidant "x" stamp).
    x = _fit(_sprite('camp00', 654), 300)
    im = _paste_rot(im, _tint(x, (0, 0, 0)), W // 2 + 10, 470, 0)
    im = _paste_rot(im, _tint(x, RED[:3]), W // 2, 460, 0)

    t1 = _text(kit, 'Contract Unsealed', 64, (255, 255, 255), track=1)
    pw, ph = t1.width + 90, t1.height + 24
    plate = Art.new(pw, ph).polygon([(18, 0), (pw, 6), (pw - 18, ph), (0, ph - 8)], RED).over(t1, 45, 8)
    im = _paste_rot(im, plate, W // 2, 755, -2)
    im = _paste_rot(im, _text(kit, 'Patch mismatch.', 40, (255, 255, 255), track=1), W // 2, 870, -2)
    im = _paste_rot(im, _text(kit, f'Update to {version} to seal the pact.', 36, (255, 110, 170), track=4),
                    W // 2, 940, -2)

    src = kit._save(im, name, {
        'source': 'composite', 'parts': [
            f"{SRC['partypanel']} #1 #2 #3 (silhouettes, greyed)",
            f"{SRC['saferoom']} #33 (calling-card logo, grey)",
            'EN/INIT/P5CAMP_00SPD.SPD #654 (X stamp)',
            'EN/FONT/FONT0.FNT (text)'],
        'size': [W, H], 'version': version})
    return src, W, H


# ---------------------------------------------------------------- minimal variants (v0.8)
def _trio_minimal(im, cy, height, rgb):
    """Three small flat silhouettes side by side (Ryuji, Joker, Morgana), no rotation or shadow."""
    parts = [_tint(_fit(_sprite('partypanel', sid), h), rgb) for sid, h in ((2, int(height * 0.8)), (1, height), (3, int(height * 0.78)))]
    gap = 18
    total = sum(p.width for p in parts) + gap * 2
    x = (W - total) // 2
    for p in parts:
        im = im.over(p, x, cy + height - p.height)
        x += p.width + gap
    return im


def compose_minimal(kit, name='waiting_min'):
    im = _trio_minimal(Art.new(W, H), 300, 150, (235, 235, 235))
    im = im.rectangle((W // 2 - 150, 478, W // 2 + 150, 481), RED)
    t1 = _text(kit, 'Stealing Your Heart', 30, (240, 240, 240), track=1)
    im = im.over(t1, (W - t1.width) // 2, 505)
    t2 = _text(kit, 'Preparing the heist...', 22, (150, 142, 146), track=1)
    im = im.over(t2, (W - t2.width) // 2, 560)
    src = kit._save(im, name, {'source': 'composite', 'parts': [
        f"{SRC['partypanel']} #1 #2 #3 (silhouettes)", 'EN/FONT/FONT0.FNT (text)'], 'size': [W, H]})
    return src, W, H


def compose_mismatch_minimal(kit, version='v1.0.2', name='mismatch_min'):
    im = _trio_minimal(Art.new(W, H), 300, 150, (92, 86, 90))
    im = im.rectangle((W // 2 - 150, 478, W // 2 + 150, 481), RED)
    t1 = _text(kit, 'Contract Unsealed', 30, (229, 25, 28), track=1)
    im = im.over(t1, (W - t1.width) // 2, 505)
    t2 = _text(kit, 'Patch mismatch.', 22, (200, 194, 198), track=1)
    im = im.over(t2, (W - t2.width) // 2, 560)
    t3 = _text(kit, f'Update to {version} to seal the pact.', 22, (150, 142, 146), track=3)
    im = im.over(t3, (W - t3.width) // 2, 600)
    src = kit._save(im, name, {'source': 'composite', 'parts': [
        f"{SRC['partypanel']} #1 #2 #3 (silhouettes, grey)", 'EN/FONT/FONT0.FNT (text)'],
        'size': [W, H], 'version': version})
    return src, W, H
