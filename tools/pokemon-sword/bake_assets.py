#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate authored package geometry, without game files or a texture cache.

Runtime controls are module:sw:ui images. Existing authored shell and wedge artwork are
preserved as source inputs; this tool regenerates the rounded tiles, shadows and loading art.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import sw_variants  # noqa: E402
# Shared art: both variants' packages carry the same bake (make_package.py refuses a Shield img/
# that differs from Sword's). main() takes --variant (default sword).
OUT = ROOT / f"packages/{sw_variants.get().package_dir}/dualscreen/img"
from PIL import Image, ImageDraw, ImageFilter  # noqa: E402

# Radii/masks/shadows actually referenced by gen_manifest.py's box()/disc()/shadow()/corners()
# calls (keep in sync -- verified by grepping each basename in the regenerated manifest.json,
# not just the source: a dropped box() radius still leaves the old rr<R>.png import an orphan).
# 7/25/66/85/92/110 (RADII), 85 (MASKS) and (27,6,70)/(66,6,70) (SHADOWS) were pruned 2026-10-30
# (generator review): 0 references, ~29KB regenerated/shipped for nothing every bake.
RADII = [4, 12, 18, 22, 27, 28, 32, 33, 36, 44, 46, 47, 53, 55, 58, 64, 72, 104]
SHADOWS = [(36, 6, 70), (44, 6, 70), (32, 5, 60)]
MASKS = [18, 30]  # cm64 pruned 2026-11-01: only the removed raid glyph capsule used it
SHADOW_MARGIN = 20


def rr(r):
    n = 2 * r + 2
    s = 4
    im = Image.new("L", (n * s, n * s), 0)
    ImageDraw.Draw(im).rounded_rectangle((0, 0, n * s - 1, n * s - 1), radius=r * s, fill=255)
    a = im.resize((n, n), Image.LANCZOS)
    out = Image.new("RGBA", (n, n), (255, 255, 255, 0))
    out.putalpha(a)
    return out


def shadow(r, blur, alpha):
    m = SHADOW_MARGIN
    n = 2 * r + 2 + 2 * m
    im = Image.new("L", (n, n), 0)
    ImageDraw.Draw(im).rounded_rectangle((m, m, n - m - 1, n - m - 1), radius=r, fill=alpha)
    im = im.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.putalpha(im)
    return out


def corner_mask(r):
    s = 4
    im = Image.new("L", (r * s, r * s), 255)
    ImageDraw.Draw(im).pieslice((0, 0, 2 * r * s - 1, 2 * r * s - 1), 180, 270, fill=0)
    a = im.resize((r, r), Image.LANCZOS)
    out = Image.new("RGBA", (r, r), (255, 255, 255, 0))
    out.putalpha(a)
    return out


def move_pp_tail():
    """Native PP backing x465..600 at640/600:144x94, flat left and radius47 right end."""
    width, height, radius, scale = 144, 94, 47, 4
    alpha = Image.new("L", (width * scale, height * scale), 0)
    draw = ImageDraw.Draw(alpha)
    draw.rectangle((0, 0, (width - radius) * scale - 1, height * scale - 1), fill=255)
    draw.ellipse(((width - height) * scale, 0, width * scale - 1, height * scale - 1), fill=255)
    out = Image.new("RGBA", (width, height), (255, 255, 255, 0))
    out.putalpha(alpha.resize(out.size, Image.LANCZOS))
    return out


def loading_ball(size=120, ss=4):
    """The native world-load indicator (top screen, bottom-right; G0/states/26-loading.png) redrawn
    flat: a Poke Ball seen edge-on mid-spin -- grey left half, white right half, dark seam band,
    black-ringed white button, thin black rim. The manifest spins it (`spin`)."""
    n = size * ss
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c, r = n / 2, n / 2 - ss
    d.ellipse((c - r, c - r, c + r, c + r), fill=(0, 0, 0, 255))
    ri = r - 2 * ss
    d.pieslice((c - ri, c - ri, c + ri, c + ri), 90, 270, fill=(133, 130, 122, 255))
    d.pieslice((c - ri, c - ri, c + ri, c + ri), -90, 90, fill=(255, 255, 255, 255))
    bw = 22 * ss  # seam band, darker core line
    d.rectangle((c - bw / 2, 0, c + bw / 2, n), fill=(30, 30, 30, 255))
    d.rectangle((c - 3 * ss, 0, c + 3 * ss, n), fill=(0, 0, 0, 255))
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse((c - r, c - r, c + r, c + r), fill=255)
    im.putalpha(Image.composite(im.getchannel("A"), Image.new("L", (n, n), 0), mask))
    d = ImageDraw.Draw(im)
    for rad, col in ((27, (0, 0, 0, 255)), (18, (255, 255, 255, 255))):
        q = rad * ss
        d.ellipse((c - q, c - q, c + q, c + q), fill=col)
    return im.resize((size, size), Image.LANCZOS)


def party_drag_ghost(w=1192, h=128, pad=16, ss=2):
    """The lifted Pokemon-page row while it is dragged: the row's own #F0F0F0 pill (r = h/2) on a
    soft drop shadow, flat ImageDraw shapes (the runtime draws it under the drag ghost)."""
    W, H = (w + 2 * pad) * ss, (h + 2 * pad) * ss
    sh = Image.new("L", (W, H), 0)
    ImageDraw.Draw(sh).rounded_rectangle((pad * ss, (pad + 6) * ss, W - pad * ss, H - (pad - 6) * ss),
                                         radius=h // 2 * ss, fill=90)
    sh = sh.filter(ImageFilter.GaussianBlur(8 * ss))
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    im.putalpha(sh)
    body = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(body).rounded_rectangle((pad * ss, pad * ss, W - pad * ss, H - pad * ss),
                                           radius=h // 2 * ss, fill=(240, 240, 240, 255),
                                           outline=(210, 210, 210, 255), width=2 * ss)
    im = Image.alpha_composite(im, body)
    return im.resize((w + 2 * pad, h + 2 * pad), Image.LANCZOS)


def main():
    global OUT
    import argparse
    ap = argparse.ArgumentParser(description="Bake the package-authored img/*.png of one variant's package.")
    sw_variants.add_argument(ap)
    OUT = ROOT / f"packages/{sw_variants.get(ap.parse_args().variant).package_dir}/dualscreen/img"
    OUT.mkdir(parents=True, exist_ok=True)
    made = {}
    for r in RADII:
        rr(r).save(OUT / f"rr{r}.png")
        made[f"img/rr{r}.png"] = f"PIL rounded_rectangle radius {r} (4x supersampled), white 9-slice tile"
    for r, b, a in SHADOWS:
        shadow(r, b, a).save(OUT / f"sh{r}_{b}_{a}.png")
        made[f"img/sh{r}_{b}_{a}.png"] = (f"PIL rounded_rectangle radius {r} alpha {a} + GaussianBlur({b}) -- "
                                         "the mockup's panel/capsule drop shadow as a 9-slice")
    for r in MASKS:
        corner_mask(r).save(OUT / f"cm{r}.png")
        made[f"img/cm{r}.png"] = f"PIL pieslice inverse corner radius {r}, white -- clips a picture to a rounded frame"
    move_pp_tail().save(OUT / "move_pp_tail.png")
    made["img/move_pp_tail.png"] = "Authored rectangle and semicircle, supersampled white alpha mask"
    wc = Image.new("L", (48 * 4, 102 * 4), 0)
    ImageDraw.Draw(wc).polygon([(20 * 4, 0), (48 * 4, 0), (26 * 4, 102 * 4), (0, 102 * 4)], fill=255)
    wci = Image.new("RGBA", (48, 102), (255, 255, 255, 0)); wci.putalpha(wc.resize((48, 102), Image.LANCZOS))
    wci.save(OUT / "wedge_chip.png")
    made["img/wedge_chip.png"] = ("PIL polygon, dir_a.page_dex's dex-chip wedge (x0+34,20)-(x0+62,20)-(x0+40,122)-"
                                  "(x0+14,122), white mask tinted per chip")
    for name in ("shell_red_bands.png", "wedge.png"):
        if not (OUT / name).exists():
            raise SystemExit(f"Missing authored source asset: {name}")
        made["img/" + name] = "Authored flat geometric polygons"


    loading_ball().save(OUT / "loading_ball.png")
    made["img/loading_ball.png"] = ("loading_ball() -- flat ImageDraw ellipses/rects redrawing the native world-load "
                                    "Poke Ball indicator (G0/states/26-loading.png); no extracted texture")
    party_drag_ghost().save(OUT / "party_drag_ghost.png")
    made["img/party_drag_ghost.png"] = ("party_drag_ghost() -- flat ImageDraw rounded rect + blurred shadow: the lifted "
                                        "Pokemon-page row under the drag ghost; no extracted texture")
    prov_path = OUT.parent / "ASSETS.provenance.json"
    prov = json.loads(prov_path.read_text())
    for k, v in made.items():
        prov["files"][k] = {"origin": "generated", "source": v}
    prov["_note"] = "Authored geometric package images. Runtime game images are loaded from the player's romfs."
    prov_path.write_text(json.dumps(prov, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print("baked", len(made), "files into", OUT)


if __name__ == "__main__":
    main()
