#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bake the package-authored img/*.png shapes the D2 port needs (D2 port, 2026-10-25).

The runtime has no rounded rect, no shadow and no image clipping, so the approved D2 mockup's
geometry (the design-mockup renderer: dir_d2.py, battle_d2.py, kit.py, kit2.py) is
rebuilt from small 9-slice tiles drawn here with the same PIL calls the mockup renderer uses:

  rr<R>.png        white rounded rect, corner radius R, (2R+2)^2, slice [R,R,R,R] -- tinted per use
  sh<R>_<B>_<A>.png  soft drop shadow of an R-radius rect (GaussianBlur B, alpha A), slice R+M
  cm<R>.png        white inverse corner (R x R, transparent quarter disc) -- masks a picture's
                   square corner to the radius-R rounded frame the mockup clips it to
  chevron2.png     kit.chevron(): the X-menu cursor two-tone (white ring, #1F1D1C core)
  lock88.png / lock56.png   kit2.lock_tag(): white disc + two-tone native lock glyph
  cmd_<key>.png    battlekit.cmd_glyph(): command glyph as a white alpha mask (tinted per command)
  move_pp_tail.png  flat left edge + right semicircle, white mask for the native move PP backing
  key_x.png / key_y.png  native shortcut glyph as invert(luminance)*source alpha, white RGB for a dark key area
  key_wedge.png    native24x44 shortcut diagonal's black portion as a white alpha mask
  shell_red_bands.png  dir_d2.shell()

Pure geometry except chevron2 / lock / cmd_* / key_* (game-texture derivatives, same status as the
existing disc_*.png bakes -- see ASSETS.provenance.json). Needs the mockup texture cache
(/tmp/swsh_tex, tools/mockup/extract_arcs.py).

    SW_MOCKUP_DIR=<mockup renderer dir> python3 tools/pokemon-sword/bake_assets.py
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
import os
MOCK = os.environ.get("SW_MOCKUP_DIR", "")   # the design-mockup renderer directory (kit.py etc.)
if MOCK:
    sys.path.insert(0, MOCK)

from PIL import Image, ImageDraw, ImageFilter, ImageOps, ImageChops  # noqa: E402

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


def native_key_mask(key, texture_cache=Path("/tmp/swsh_tex")):
    """Native black X/Y on white key plate -> white glyph mask, exact42x42 canvas.

    X/Y source alpha describes the same rounded plate, not the letter. Inverting only
    luminance and multiplying by that original alpha preserves the native letter/AA.
    """
    if key not in ("X", "Y"):
        raise ValueError("Only native battle shortcut X/Y glyphs are supported")
    path = Path(texture_cache) / "battle_commandSelect_01_eng" / f"button_icon_{key}_00^t.png"
    with Image.open(path) as source:
        source = source.convert("RGBA")
        if source.size != (42, 42):
            raise ValueError(f"Unexpected native shortcut size: {source.size}")
        alpha = ImageChops.multiply(ImageOps.invert(source.convert("L")), source.getchannel("A"))
        out = Image.new("RGBA", source.size, (255, 255, 255, 0))
        out.putalpha(alpha)
    return out


def native_key_wedge(texture_cache=Path("/tmp/swsh_tex")):
    """Native24x44 shortcut diagonal's black portion, white RGB for color tinting."""
    path = Path(texture_cache) / "battle_commandSelect_01_eng" / "diagonal_44_blackwhite_00^r.png"
    with Image.open(path) as source:
        source = source.convert("RGBA")
        if source.size != (24, 44):
            raise ValueError(f"Unexpected native shortcut diagonal size: {source.size}")
        alpha = ImageChops.multiply(ImageOps.invert(source.convert("L")), source.getchannel("A"))
        out = Image.new("RGBA", source.size, (255, 255, 255, 0))
        out.putalpha(alpha)
    return out


def native_max_move_body(texture_cache=Path("/tmp/swsh_tex")):
    """Sword's native Max gradient fitted to its 600:88 rounded move body."""
    path = Path(texture_cache) / "battle_skillSelect_00_eng" / "battle_gskill_bg_00^o.png"
    with Image.open(path) as source:
        out = source.convert("RGBA").resize((640, 94), Image.Resampling.BILINEAR)
    alpha = Image.new("L", (2560, 376), 0)
    ImageDraw.Draw(alpha).rounded_rectangle((0, 0, 2559, 375), radius=188, fill=255)
    out.putalpha(ImageChops.multiply(out.getchannel("A"), alpha.resize(out.size, Image.Resampling.LANCZOS)))
    return out


def battle_control_bakes(texture_cache=Path("/tmp/swsh_tex")):
    """New native-style controls; shared by full regeneration and selective integration."""
    made = {
        "move_max_body.png": (native_max_move_body(texture_cache),
            "battle_skillSelect_00_eng.arc#battle_gskill_bg_00^o native64x8 Max gradient, "
            "bilinear fit640x94 with native600:88 body radius47 supersampled alpha mask"),
        "move_pp_tail.png": (move_pp_tail(),
            "PIL rectangle + right semicircle radius47 (4x supersampled), white144x94 alpha mask; "
            "native battle_button_skill_00 PP backing x465..600 scaled from600px to640px width"),
        "key_wedge.png": (native_key_wedge(texture_cache),
            "battle_commandSelect_01_eng.arc#diagonal_44_blackwhite_00^r, native24x44 black portion "
            "as invert(luminance)*source alpha, white RGB; no resize or geometry change")
    }
    for key in ("X", "Y"):
        made[f"key_{key.lower()}.png"] = (native_key_mask(key, texture_cache),
            f"battle_commandSelect_01_eng.arc#button_icon_{key}_00^t, native42x42 black letter on white key plate "
            "converted via invert(luminance)*source alpha to a white glyph mask; no resize/redraw")
    return made


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
    import kit
    import kit2
    import battlekit
    import dir_d2

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
    # chevron (two-tone cursor), native size 124
    c = Image.new("RGBA", (124, 124), (0, 0, 0, 0))
    kit.chevron(c, (62, 62), 124)
    c.save(OUT / "chevron2.png")
    made["img/chevron2.png"] = ("tools/mockup/kit.py chevron(): xmenu_top_00_eng.arc#cursor_black_00^t two-tone "
                                "(white ring, #1F1D1C core), baked flat (the runtime has no two-tone shader)")
    for d in (88, 56):
        im = Image.new("RGBA", (d + 40, d + 40), (0, 0, 0, 0))
        kit2.lock_tag(im, ((d + 40) / 2, (d + 40) / 2), d)
        im = im.crop(im.getbbox())
        im.save(OUT / f"lock{d}.png")
        made[f"img/lock{d}.png"] = (f"tools/mockup/kit2.py lock_tag(d={d}): white disc + status_bg_00.arc#icon_lock^t "
                                    "two-tone (#1F1D1C glyph, white ring), the D2 'direct control pending' marker")
    # "back" is deliberately excluded: raid_command()'s key=="back" branch is a plain pill+text
    # label (same as every other Back button in gen_manifest.py), never the glyph capsule, so
    # cmd_back.png was baked and never referenced every build -- stop baking it (2026-10-30).
    for key in ("fight", "pokemon", "bag", "run", "cheer"):
        g = battlekit.cmd_glyph(key, 120, (255, 255, 255))
        g.save(OUT / f"cmd_{key}.png")
        made[f"img/cmd_{key}.png"] = (f"tools/mockup/battlekit.py cmd_glyph('{key}'): battle_commandSelect_00_eng.arc"
                                      f"#{battlekit.CMD[key][0]} inverted to a white line mask (the native "
                                      "command capsule draws the glyph's lines in the command colour)")
    for name, (im, source) in battle_control_bakes().items():
        im.save(OUT / name)
        made["img/" + name] = source
    wc = Image.new("L", (48 * 4, 102 * 4), 0)
    ImageDraw.Draw(wc).polygon([(20 * 4, 0), (48 * 4, 0), (26 * 4, 102 * 4), (0, 102 * 4)], fill=255)
    wci = Image.new("RGBA", (48, 102), (255, 255, 255, 0)); wci.putalpha(wc.resize((48, 102), Image.LANCZOS))
    wci.save(OUT / "wedge_chip.png")
    made["img/wedge_chip.png"] = ("PIL polygon, dir_a.page_dex's dex-chip wedge (x0+34,20)-(x0+62,20)-(x0+40,122)-"
                                  "(x0+14,122), white mask tinted per chip")
    dir_d2.shell().convert("RGB").save(OUT / "shell_red_bands.png")
    made["img/shell_red_bands.png"] = ("dir_d2.shell() (tools/mockup/kit.py field()) -- flat ImageDraw polygons, "
                                       "Rotom red + the two diagonal bands every D2 page shares; no extracted texture")

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
    # panel_r44.png is superseded by rr44 + sh44 (D2 port)
    prov["files"].pop("img/panel_r44.png", None)
    (OUT / "panel_r44.png").unlink(missing_ok=True)
    # cmd_back.png and the pruned RADII/MASKS/SHADOWS entries above (rr7/rr25/rr66/rr85/rr92/
    # rr110, cm85, sh27_6_70, sh66_6_70): generator review 2026-10-30 found all 10 unreferenced
    # in the regenerated manifest.json (0 grep hits each) -- drop the stale files/provenance
    # entries any earlier bake left on disk, same treatment as panel_r44.png above.
    for stale in ("cmd_back", "rr7", "rr25", "rr66", "rr85", "rr92", "rr110", "cm85",
                  "sh27_6_70", "sh66_6_70", "rr43", "sh43_6_70", "sh33_6_70"):
        prov["files"].pop(f"img/{stale}.png", None)
        (OUT / f"{stale}.png").unlink(missing_ok=True)
    prov["_note"] = ("Package-authored img/*.png, generated offline by tools/pokemon-sword/bake_assets.py with the "
                     "design-mockup toolkit's own primitives (PIL) against the extracted romfs texture cache -- never "
                     "shipped as raw extracted game files. Every other picture in manifest.json is romfs: (the "
                     "player's own game files) or a plain vector widget.")
    prov_path.write_text(json.dumps(prov, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print("baked", len(made), "files into", OUT)


if __name__ == "__main__":
    main()
