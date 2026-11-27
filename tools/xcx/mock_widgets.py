#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the runtime's map widget, for review renders only (the real widget is native).

Reads what `xcx-preview ... map` wrote (<name>.json, <name>-page.pam, the overlay pictures) and
composites what the widget should show: the overlays over the blank base at the widget's view (the
simulated drawn view, else the follow framing), the marker arrow at (x, -z) rotated by the published
heading, the Skell icon, then the page picture on top. It does not claim to match the runtime's
pixels; it shows what the module hands the widget.

usage: mock_widgets.py OUT_DIR PNG_DIR
"""
import json
import math
import pathlib
import sys

from PIL import Image

SCALE = 6144 / 30000
HALF_X, HALF_Z = 15000.0, 13750.0
HI_W, HI_H = 1024, 1024
NLA_X0, NLA_Z0, NLA_TILE = -3480.0, 6190.0, 700.0
BG = (14, 19, 33, 255)


def open_pam(path):
    data = pathlib.Path(path).read_bytes()
    head, _, body = data.partition(b"ENDHDR\n")
    fields = dict(line.split(b" ", 1) for line in head.split(b"\n")[1:] if b" " in line)
    w, h = int(fields[b"WIDTH"]), int(fields[b"HEIGHT"])
    return Image.frombytes("RGBA", (w, h), body[: w * h * 4])


def texel_box(u0, v0, u1, v1):
    return (u0 / SCALE - HALF_X, -(v1 / SCALE - HALF_Z), u1 / SCALE - HALF_X, -(v0 / SCALE - HALF_Z))


def box(name):
    kind, n = name.split(".")[1], int(name.split(".")[2])
    if kind == "mh":
        tx, ty = n % 6, n // 6
        return texel_box(tx * HI_W, ty * HI_H, (tx + 1) * HI_W, ty * HI_H + (512 if ty == 5 else HI_H))
    if kind == "ml":
        tx, ty = n % 2, n // 2
        return texel_box(tx * 3072, ty * 2816, (tx + 1) * 3072, (ty + 1) * 2816)
    tx, ty = n % 4, n // 4
    x0, z0 = NLA_X0 + tx * NLA_TILE, NLA_Z0 + ty * NLA_TILE
    return (x0, -(z0 + NLA_TILE), x0 + NLA_TILE, -z0)


def main(out_dir, png_dir):
    out_dir, png_dir = pathlib.Path(out_dir), pathlib.Path(png_dir)
    png_dir.mkdir(parents=True, exist_ok=True)
    arrow = open_pam(out_dir / "icon-arrow.pam").convert("RGBA")
    skell = open_pam(out_dir / "icon-skell.pam").convert("RGBA")
    for js in sorted(out_dir.glob("*.json")):
        info = json.loads(js.read_text())
        name = info["name"]
        canvas = Image.new("RGBA", (1240, 1080), BG)
        widget = info["widget"]
        view = info["view"]
        pl = (info["pl_x"], info["pl_y"])
        has_marker = pl[0] is not None
        if widget == "barracks":
            pic = open_pam(out_dir / f"{name}-barracks.pam").convert("RGBA")
            k = min(860 / pic.width, 960 / pic.height)
            pic = pic.resize((round(pic.width * k), round(pic.height * k)), Image.LANCZOS)
            canvas.alpha_composite(pic, ((860 - pic.width) // 2, (960 - pic.height) // 2))
        elif widget in ("over", "home", "world", "nla"):
            if view["ppw"] > 0:
                cx, cy, ppw = view["cx"], view["cy"], view["ppw"]
            else:  # follow framing: the follow window across the widget width, centred on the marker
                follow = {"over": 15000, "home": 30000, "world": 2400, "nla": 520}[widget]
                ppw = 860 / follow
                cx, cy = (pl if has_marker else (0.0, 0.0))
            for key in info["overlays_shown"]:
                kind = key.split(".")[1]
                idx = key.split(".")[2]
                tile = open_pam(out_dir / f"{name}-{kind}{idx}.pam").convert("RGBA")
                x0, y0, x1, y1 = box(key)
                w, h = round((x1 - x0) * ppw), round((y1 - y0) * ppw)
                if w < 1 or h < 1:
                    continue
                tile = tile.resize((w, h), Image.BILINEAR)
                layer = Image.new("RGBA", (1240, 1080), (0, 0, 0, 0))
                layer.paste(tile, (round(430 + (x0 - cx) * ppw), round(480 - (y1 - cy) * ppw)))
                canvas.alpha_composite(layer)
            if has_marker:
                mx, my = 430 + (pl[0] - cx) * ppw, 480 - (pl[1] - cy) * ppw
                icon = arrow.resize((72, 72), Image.LANCZOS).rotate(-info.get("yaw_deg", 0), resample=Image.BICUBIC)
                canvas.alpha_composite(icon, (round(mx - 36), round(my - 36)))
        # the widget clips to its rect
        page = open_pam(out_dir / f"{name}-page.pam").convert("RGBA")
        clip = canvas.crop((0, 0, 860, 960))
        base = Image.new("RGBA", (1240, 1080), BG)
        base.alpha_composite(clip, (0, 0))
        # beyond the rect the module's own picture covers everything (opaque), so compose it on top
        base.alpha_composite(page)
        base.convert("RGB").save(png_dir / f"{name}.png")
        print(name)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
