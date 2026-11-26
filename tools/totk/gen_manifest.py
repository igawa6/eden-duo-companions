#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate packages/TearsOfTheKingdom/dualscreen/manifest.json and the per-build data file.

Standard library only:  tools/totk/gen_manifest.py [package dir]
All pictures, the font and the item text are read at runtime through the `user:` source (`user:art/...`, the player's
own art pack made by tools/totk/make_art.py); without it the pages still work with the built-in font and flat colours.
"""
import json
import sys
from pathlib import Path

TITLE_ID = "0100F2C0115B6000"
BUILD16 = "277178B7DBA1B6D4"  # first 8 bytes of the 1.4.3 main build id 277178b7dba1b6d4949a84778d4abc58
W, H = 1240, 1080
ART = "user:art/"

INK, GOLD, DIM = "#FFF2EBD3", "#FFD9B96B", "#FF8C9A9E"
PANEL, FRAME, BG = "#D0121B1F", "#FF3D4F55", "#FF0B1114"
RED, YEL, GREEN, PURPLE = "#FFE8504A", "#FFF2D45C", "#FF7FE88B", "#FF9B6BD0"
DARK = "#FF2C3538"


def rect(x, y, w, h, bg, color=0, frame=2, **kw):
    d = {"type": "rect", "rect": [x, y, w, h], "bg": bg}
    if color:
        d["color"] = color
        d["frame"] = frame
    d.update(kw)
    return d


def panel(x, y, w, h):
    return rect(x, y, w, h, PANEL, FRAME, 2)


def text(x, y, w, h, s, scale=5, color=INK, **kw):
    return {"type": "label", "rect": [x, y, w, h], "text": s, "text_scale": scale, "color": color, **kw}


def btext(x, y, w, h, bind, scale=5, color=INK, **kw):
    return {"type": "label", "rect": [x, y, w, h], "bind_text": bind, "text_scale": scale, "color": color, **kw}


def img(x, y, w, h, src, **kw):
    return {"type": "image", "rect": [x, y, w, h], "src": src if src.startswith(("file:", "user:")) else ART + src, **kw}


def simg(x, y, w, h, bind, **kw):
    """An item icon: the module publishes the icon's index, art/icons/<index>.png."""
    return {"type": "image", "rect": [x, y, w, h], "bind": bind, "src_format": ART + "icons/%d.png", **kw}


def fimg(x, y, w, h, bind, pattern, **kw):
    """A gauge frame picked by value: art/ui/<pattern> with the bound number."""
    return {"type": "image", "rect": [x, y, w, h], "bind": bind, "src_format": ART + "ui/" + pattern, **kw}


def ctext(cx, y, h, bind, scale=5, color=INK, **kw):
    """Centred text: the runtime centres on the rect's x, so give the centre."""
    return btext(cx, y, 0, h, bind, scale, color, align="center", **kw)


NAV = [("status", "ui.status"), ("map", "ui.map"), ("inv", "ui.items"), ("info", "ui.info")]


def nav(active):
    out = []
    for n, (pid, key) in enumerate(NAV):
        x = 30 + n * 300
        out.append({"type": "button", "rect": [x, 1000, 280, 66], "on_tap": "to_" + pid, "pill": True,
                    "bg": "#FF2F4A44" if pid == active else "#FF182226", "color": GOLD if pid == active else FRAME,
                    "text": ""})
        out.append(ctext(x + 140, 1015, 40, key, 6, INK if pid == active else DIM))
    return out


def status_page():
    w = [rect(0, 0, W, H, BG)]
    # header
    w += [btext(40, 26, 620, 70, "totk.layer.text", 14, GOLD, need_bind="totk.pos.valid"),
          btext(40, 104, 620, 40, "totk.pos.text", 5, DIM, need_bind="totk.pos.valid"),
          btext(40, 40, 900, 50, "ui.waiting", 8, GOLD, need_bind="ui.nodata"),
          img(850, 24, 64, 64, "ui/rupee.png"), btext(930, 28, 290, 60, "totk.rupees.text", 11, INK),
          img(856, 96, 44, 44, "ui/poe.png"), btext(930, 100, 290, 40, "totk.poes.text", 7, DIM)]
    # hearts
    w.append(panel(30, 150, 1180, 290))
    cols = {"repeat_cols": 10, "repeat_dx": 68, "repeat_row_dy": 66}
    w.append(img(56, 170, 62, 62, "ui/heart_empty.png", tint="#FF4A3F43", repeat=40, need_bind="totk.h.{i}.on", **cols))
    w.append(fimg(56, 170, 62, 62, "totk.h.{i}.q", "heart_%d.png", repeat=40, need_bind="totk.h.{i}.n", **cols))
    w.append(fimg(56, 170, 62, 62, "totk.h.{i}.q", "heartx_%d.png", repeat=40, need_bind="totk.h.{i}.y", **cols))
    w.append(img(56, 170, 62, 62, "ui/heart_broken.png", tint=PURPLE, repeat=40, need_bind="totk.h.{i}.k", **cols))
    w += [btext(800, 168, 400, 120, "totk.hearts.text", 22, INK, need_bind="totk.hearts.valid"),
          btext(800, 292, 400, 50, "totk.hearts.max_text", 9, DIM, need_bind="totk.hearts.valid"),
          btext(800, 350, 400, 40, "totk.hearts.extra_text", 8, YEL, need_bind="totk.hearts.valid")]
    # stamina and battery
    w.append(panel(30, 460, 590, 210))
    wc = {"repeat_cols": 5, "repeat_dx": 110, "repeat_row_dy": 96}
    w.append(img(55, 480, 90, 90, "ui/stamina_ring.png", tint=DARK, repeat=10, need_bind="totk.sw.{i}.on", **wc))
    w.append(fimg(55, 480, 90, 90, "totk.sw.{i}.q", "ring_%d.png", repeat=10, need_bind="totk.sw.{i}.n", **wc))
    w.append(fimg(55, 480, 90, 90, "totk.sw.{i}.q", "ringx_%d.png", repeat=10, need_bind="totk.sw.{i}.g", **wc))
    w.append(panel(30, 690, 590, 120))
    bc = {"repeat_cols": 5, "repeat_dx": 112, "repeat_row_dy": 70}
    w.append(img(55, 712, 100, 64, "ui/battery.png", tint="#FF4A5558", repeat=10, need_bind="totk.bat.{i}.on", **bc))
    w.append(fimg(55, 712, 100, 64, "totk.bat.{i}.q", "bat_%d.png", repeat=10, need_bind="totk.bat.{i}.n", **bc))
    w.append(fimg(55, 712, 100, 64, "totk.bat.{i}.q", "batx_%d.png", repeat=10, need_bind="totk.bat.{i}.g", **bc))
    # sages
    w.append(panel(30, 830, 590, 150))
    for k, key in enumerate(("Wind", "Fire", "Water", "Electric", "Soul")):
        x = 52 + k * 114
        w.append(img(x, 846, 92, 92, f"ui/sage_{key}.png", tint_bind=f"totk.sage.{k}.joined", tint_colors=["#FF3A4448", INK]))
        w.append(rect(x + 36, 936, 20, 6, GOLD, need_bind=f"totk.sage.{k}.summoned"))
        w.append(ctext(x + 46, 946, 30, f"totk.sage.{k}.cd", 5, GOLD))
    # equipment
    w.append(panel(650, 460, 560, 520))
    for n, key in enumerate(("weapon", "bow", "shield")):
        y = 480 + n * 110
        w.append(rect(670, y, 96, 96, "#FF182226", FRAME, 2))
        w.append(simg(674, y + 4, 88, 88, f"totk.eq.{key}.ix"))
        w.append(btext(786, y + 8, 400, 64, f"totk.eq.{key}.name", 6, INK, wrap_width=400, max_lines=2))
        w.append(btext(786, y + 66, 400, 30, f"totk.eq.{key}.dur", 5, DIM))
    for n, key in enumerate(("head", "upper", "lower")):
        x = 670 + n * 110
        w.append(rect(x, 812, 96, 96, "#FF182226", FRAME, 2))
        w.append(simg(x + 4, 816, 88, 88, f"totk.eq.{key}.ix"))
    w.append(rect(1000, 812, 96, 96, "#FF182226", FRAME, 2))
    w.append(simg(1004, 816, 88, 88, "totk.ability.ix"))
    w.append(btext(1200, 925, 0, 50, "totk.ability.name", 6, GOLD, align="right"))
    w += nav("status")
    return {"id": "status", "title": "Status", "widgets": w}


def map_page():
    w = [rect(0, 0, W, H, "#FF05080A"),
         {"type": "map", "area_bind": "totk.map.area", "area": "Ground", "rect": [0, 0, W, 930], "area_label": False,
          "pan_zoom": True, "max_zoom": 16, "id": "totk_map", "marker_x_bind": "totk.map.x",
          "marker_y_bind": "totk.map.y", "marker_src": ART + "ui/map_player.png", "marker_size": [84, 84],
          "marker_tint": "#FFFFE45C", "marker_rotate_bind": "totk.pos.heading", "need_bind": "totk.pos.valid",
          "view_rect_x0_bind": "totk.map.vx0", "view_rect_y0_bind": "totk.map.vy0",
          "view_rect_x1_bind": "totk.map.vx1", "view_rect_y1_bind": "totk.map.vy1"},
         ctext(620, 450, 60, "ui.nopos", 8, GOLD, need_bind="ui.nopos_on"),
         rect(0, 940, W, 54, "#E00B1114")]
    labels = [("ui.auto", "map_auto"), ("ui.sky", "map_sky"), ("ui.ground", "map_ground"), ("ui.depths", "map_depths"),
              ("ui.recenter", "map_recenter")]
    for n, (key, act) in enumerate(labels):
        x = 20 + n * 238
        w.append({"type": "button", "rect": [x, 944, 226, 46], "on_tap": act, "bg": "#FF182226", "color": FRAME,
                  "text": "", "pill": True})
        w.append(ctext(x + 113, 951, 34, key, 5, INK))
    w += nav("map")
    return {"id": "map", "title": "Map", "widgets": w}


TABS = ["weapons", "bows", "shields", "armor", "materials", "food", "zonai", "keyItems"]


def inv_page():
    w = [rect(0, 0, W, H, BG)]
    for i, k in enumerate(TABS):
        x = 30 + i * 148
        w.append({"type": "button", "rect": [x, 20, 140, 84], "on_tap": f"tab_{i}", "text": "", "bg": "#FF182226",
                  "color": FRAME})
        w.append(rect(x, 20, 140, 84, "#FF3A5A52", GOLD, 3, need_bind=f"ui.tab{i}"))
        w.append(img(x + 42, 30, 56, 56, f"ui/cat_{k}.png", tint=INK, need_bind=f"ui.tab{i}"))
        w.append(img(x + 42, 30, 56, 56, f"ui/cat_{k}.png", tint=DIM, need_bind=f"ui.ntab{i}"))
    cell = {"repeat": 12, "repeat_cols": 4, "repeat_dx": 184, "repeat_row_dy": 236}
    w.append({"type": "button", "rect": [30, 130, 176, 228], "text": "", "bg": "#FF141D21", "color": FRAME,
              "on_tap": "sel_{i}", "need_bind": "totk.inv.{i}.on", **cell})
    w.append(rect(30, 130, 176, 228, 0, GOLD, 4, need_bind="totk.inv.{i}.sel", **cell))
    w.append(simg(58, 142, 120, 120, "totk.inv.{i}.ix", need_bind="totk.inv.{i}.on", **cell))
    w.append(rect(34, 134, 14, 14, GOLD, need_bind="totk.inv.{i}.eq", **cell))
    w.append(btext(120, 270, 0, 60, "totk.inv.{i}.name", 4, INK, wrap_width=164, max_lines=2, align="center", **cell))
    w.append(btext(118, 328, 0, 30, "totk.inv.{i}.info", 5, GOLD, align="center", **cell))
    w.append({"type": "button", "rect": [30, 880, 300, 64], "text": "", "bg": "#FF182226", "color": FRAME,
              "on_tap": "page_next", "pill": True})
    w.append(ctext(180, 894, 40, "totk.inv.page", 6, INK))
    # detail
    w.append(panel(780, 130, 430, 814))
    w.append(simg(895, 150, 200, 200, "totk.sel.ix"))
    w.append(btext(995, 366, 0, 110, "totk.sel.name", 7, GOLD, wrap_width=390, max_lines=3, align="center"))
    w.append(ctext(995, 480, 40, "totk.sel.info", 7, INK))
    w.append(btext(800, 540, 390, 390, "totk.sel.desc", 4, INK, wrap_width=390, max_lines=16))
    w += nav("inv")
    return {"id": "inv", "title": "Items", "widgets": w}


def info_page():
    ok = GREEN
    rows = [("build_ok", "totk.build_ok"), ("manager", "totk.mgr_ok"), ("int flags", "totk.int_count"),
            ("tables plausible", "totk.tables_ok"), ("player actor", "totk.actor_ok"),
            ("pouch partition", "totk.pouch_ok"), ("hearts flags", "totk.hearts.valid"),
            ("position", "totk.pos.valid")]
    texts = [("rupees", "totk.rupees.text"), ("stamina wheels", "totk.stamina.text"), ("battery cells", "totk.energy.text"),
             ("hearts", "totk.hearts.text")]
    w = [rect(0, 0, W, H, BG), text(60, 40, 1120, 70, "MODULE INFO", 12, GOLD)]
    for n, (t, b) in enumerate(rows):
        y = 140 + n * 66
        w.append(text(60, y, 400, 50, t, 6))
        w.append({"type": "value", "rect": [450, y, 150, 50], "bind": b, "text_scale": 6, "color": ok})
    for n, (t, b) in enumerate(texts):
        y = 140 + n * 66
        w.append(text(660, y, 300, 50, t, 6))
        w.append(btext(960, y, 240, 50, b, 6, GREEN))
    w.append(text(60, 720, 1120, 240, "1 = yes, 0 = no. Read-only companion for TotK 1.4.3. Pictures, font and "
                  "item text come from the art pack you build from your own game with tools/totk/make_art.py "
                  "(read from the user: folder, art/). Offsets and flag names: TotK Native (MIT, see NOTICE-TotKNative.txt).", 5, DIM,
                  wrap_width=1120, max_lines=6))
    w += nav("info")
    return {"id": "info", "title": "Info", "widgets": w}


def unsupported():
    """Shown over every page while the module is not running (another game version): module_ready is 0."""
    return [rect(0, 0, W, H, "#F00B1114", need_bind="!module_ready", input_block=True),
            text(620, 430, 0, 80, "Requires game version 1.4.3", 10, GOLD, align="center", need_bind="!module_ready"),
            text(620, 540, 0, 50, "This game version is not supported by the companion.", 5, INK, align="center",
                 need_bind="!module_ready")]


def page_action(page):
    return {"kind": "page", "page": page, "transition": "fade", "duration_ms": 120}


def main():
    pkg = Path(sys.argv[1] if len(sys.argv) > 1 else "packages/TearsOfTheKingdom") / "dualscreen"
    pkg.mkdir(parents=True, exist_ok=True)
    actions = {"to_" + p: page_action(p) for p, _ in NAV}
    for i in range(8):
        actions[f"tab_{i}"] = {"kind": "flag", "flag": "inv_tab", "value": i}
    for i in range(12):
        actions[f"sel_{i}"] = {"kind": "flag", "flag": "inv_sel", "value": i}
    actions["page_next"] = {"kind": "flag", "flag": "inv_page", "cycle": 60}
    for i, n in enumerate(("auto", "sky", "ground", "depths")):
        actions["map_" + n] = {"kind": "flag", "flag": "map_layer", "value": i}
    actions["map_recenter"] = {"kind": "view_reset", "view": "totk_map"}
    derived = [{"name": f"ui.tab{i}", "cmp": "eq", "a": "@flag:inv_tab", "b": i} for i in range(8)]
    derived += [{"name": f"ui.ntab{i}", "cmp": "ne", "a": "@flag:inv_tab", "b": i} for i in range(8)]
    derived += [{"name": "ui.nodata", "cmp": "eq", "a": "totk.ready", "b": 0},
                {"name": "ui.nopos_on", "cmp": "eq", "a": "totk.pos.valid", "b": 0}]
    area = lambda n: {"image": ART + f"map/{n}.png", "min": [-6000, -5000], "max": [6000, 5000], "clamp_view": True}
    manifest = {
        "format": 1, "title_id": TITLE_ID, "name": "Tears of the Kingdom companion", "min_runtime": 22,
        "requires_module": True, "module": {"abi": 1, "build_ids": [BUILD16]},
        "_about": "Read-only status, map position and inventory from the game's GameData manager and player actor; "
                  "pictures, font and item text come from the optional art pack (README.md, make_art.py).",
        "canvas_w": W, "canvas_h": H, "background": BG,
        "font": ART + "font/game.mfnt", "font_atlas": ART + "font/game.png",
        "flags": {"inv_tab": 0, "inv_page": 0, "inv_sel": 0, "map_layer": 0},
        "derived": derived,
        "pages": [dict(p, widgets=p["widgets"] + unsupported()) for p in (status_page(), map_page(), inv_page(), info_page())],
        "actions": actions,
        "map": {"areas": {"Ground": area("ground"), "Sky": area("sky"), "UnderGround": area("underground")}},
        "haptics": True,
    }
    (pkg / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    data = {"_game": "The Legend of Zelda: Tears of the Kingdom 1.4.3 (v720896), 0100F2C0115B6000",
            "_build_id": "277178b7dba1b6d4949a84778d4abc58",
            "_note": "Empty on purpose: the native module reads the GameData manager live (see README.md)."}
    (pkg / f"{BUILD16}.json").write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
