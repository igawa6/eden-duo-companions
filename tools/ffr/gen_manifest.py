#!/usr/bin/env python3
"""Generates packages/FinalFantasyResonance/dualscreen/manifest.json.
The module draws every page (ffr_pages.cpp); the manifest shows that picture and places invisible tap areas
whose rectangles follow the layout constants in ffr_pages.h."""
import json
import os

W, H, TAB_Y, HEADER_H = 1240, 1080, 968, 112
CARD_Y, CARD_H, CARD_GAP = 132, 200, 14
ROW_Y, ROW_H, ROWS = 222, 84, 7
MAP = [30, HEADER_H + 16, W - 60, TAB_Y - (HEADER_H + 16) - 16]  # kMapX, kMapY, kMapW, kMapH
TOWN_N, WORLD_N, MAX_ICONS = 4096, 4096, 64
# The town picture reaches TOWN_PAD past the capture's square on the left and right (the menu background), so a view as wide as
# the widget (1180 x 824) never runs off it: keep in step with kTownPad in the module.
TOWN_PAD = 904
ISLAND_SIZES, ISLANDS_PER_CLASS = [1024, 724, 512, 362, 256, 181], 12  # kIslandSizes in the module
BUTTONS = {"a": "A", "b": "B", "up": "DUp", "down": "DDown", "left": "DLeft", "right": "DRight"}


def tap(rect, action, need=None, **extra):
    w = {"type": "rect", "rect": rect, "color": "#00000000", "on_tap": action}
    if need:
        w["need_bind"] = need
    w.update(extra)
    return w


def main():
    actions, widgets = {}, [{"type": "image", "rect": [0, 0, W, H], "src_bind": "ffr.page", "src_hold": True}]
    for i in range(4):
        actions[f"tab{i}"] = {"kind": "module", "action": "tab", "argument": i}
        widgets.append(tap([i * W // 4, TAB_Y, W // 4, H - TAB_Y], f"tab{i}", "ffr.map_tab" if i == 0 else "ffr.in_game"))  # Map: only with a map picture
    for i in range(4):
        actions[f"member{i}"] = {"kind": "module", "action": "member", "argument": i}
        widgets.append(tap([30, CARD_Y + i * (CARD_H + CARD_GAP), W - 60, CARD_H], f"member{i}", "ffr.party_list"))
    actions["back"] = {"kind": "module", "action": "back", "argument": 0}
    widgets.append(tap([0, 112, W, TAB_Y - 112], "back", "ffr.party_detail"))
    # The live map: one widget for a town or dungeon capture, one for the world map's islands. The module publishes the
    # player every tick (ffr.map.px / py / yaw) and the pictures as module keys; pinch and the +/- buttons zoom.
    # The world's ocean: 4 x 4 tiles of the game's ocean texture, mirrored in turn so they meet without seams
    ocean = [{"src": f"module:ffr:ocean:{tx % 2}{ty % 2}", "x0": tx * 1024, "y0": ty * 1024, "x1": tx * 1024 + 1024, "y1": ty * 1024 + 1024}
             for ty in range(4) for tx in range(4)]
    for name, area, gate in (("ffr_town_map", "ffr_town", "ffr.map.town"), ("ffr_world_map", "ffr_world", "ffr.map.world")):
        w = {"type": "map", "id": name, "rect": MAP, "area": area, "area_label": False, "pan_zoom": True,
             "min_zoom": 0.6, "max_zoom": 10, "need_bind": gate, "bg": "#FF0A131B",
             "marker_x_bind": "ffr.map.px", "marker_y_bind": "ffr.map.py", "marker_rotate_bind": "ffr.map.yaw",
             "marker_src": "module:ffr:ui:Icon/MAP/ICON_MAP_Player", "marker_size": [64, 64],
             "view_rect_x0_bind": "ffr.map.vx0", "view_rect_y0_bind": "ffr.map.vy0",
             "view_rect_x1_bind": "ffr.map.vx1", "view_rect_y1_bind": "ffr.map.vy1", "view_rect_pad": 0}
        if area == "ffr_town":
            w["image_bind"] = "ffr.map.base"
            w["min_zoom"] = 1.0  # the whole town at most, as the game's map: the picture fills the frame at every zoom
        else:
            w["overlays"] = ocean
            w["bg"] = "#FF485756"  # the ocean's own tone where no tile is drawn
        widgets.append(w)
        bx = MAP[0] + MAP[2] - 16 - 88
        for i, (key, factor, y) in enumerate((("in", 1.25, MAP[1] + 16), ("out", 0.8, MAP[1] + 120), ("home", 0, MAP[1] + MAP[3] - 104))):
            act = f"{name}_{key}"
            actions[act] = {"kind": "view_reset", "view": name} if key == "home" else {"kind": "view_zoom_step", "view": name, "factor": factor}
            widgets.append({"type": "image", "rect": [bx, y, 88, 88], "src": f"module:ffr:btn:{key}", "need_bind": gate})
            widgets.append(tap([bx - 6, y - 6, 100, 100], act, gate, haptic="light"))
    # The battle page: the module hit-tests the tap point against the layout it drew (cards, the command window, Back)
    actions["btap"] = {"kind": "module", "action": "btap", "argument": "$payload"}
    widgets.append(tap([0, HEADER_H, W, TAB_Y - HEADER_H], "btap", "ffr.battle_menu", payload="$tap"))
    tw = (W - 60) // 4
    for i in range(4):
        actions[f"cat{i}"] = {"kind": "module", "action": "cat", "argument": i}
        widgets.append(tap([30 + i * tw, 128, tw, 76], f"cat{i}", "ffr.items"))
    actions["scroll_down"] = {"kind": "module", "action": "scroll", "argument": 1}
    actions["scroll_up"] = {"kind": "module", "action": "scroll", "argument": -1}
    for r in range(ROWS):
        actions[f"row{r}"] = {"kind": "module", "action": "row", "argument": r}
        widgets.append(tap([30, ROW_Y + r * ROW_H, W - 60, ROW_H], f"row{r}", "ffr.items",
                           on_swipe_up="scroll_down", on_swipe_down="scroll_up"))
    # The presses a battle tap becomes: the module raises one ffr.drv.<button> gate per sample (Drive in the module)
    enforce = []
    for key, button in BUTTONS.items():
        actions[f"ffr.press.{key}"] = {"kind": "button", "button": button, "frames": 4, "enabled_bind": f"ffr.drv.{key}"}
        enforce.append({"action": f"ffr.press.{key}", "every_ms": 1})
    icons = {"group": "icon", "count": MAX_ICONS, "x": "ffr.map.icon{i}.x", "y": "ffr.map.icon{i}.y",
             "icon_src_bind": "ffr.map.icon{i}.src", "size": 52}
    islands = [{"group": f"isl{c}", "count": ISLANDS_PER_CLASS, "x": f"ffr.map.isl{c}_{{i}}.x", "y": f"ffr.map.isl{c}_{{i}}.y",
                "icon_src_bind": f"ffr.map.isl{c}_{{i}}.src", "size_world": size} for c, size in enumerate(ISLAND_SIZES)]
    # the objective (the minimap entry the game marks as the quest target), over the other icons, in both maps
    goal = {"group": "goal", "count": 1, "x": "ffr.map.goal{i}.x", "y": "ffr.map.goal{i}.y", "icon_src_bind": "ffr.map.goal{i}.src",
            "size": 72}
    map_cfg = {"style": {"background": "#FF0A131B"}, "areas": {
        "ffr_town": {"min": [-TOWN_PAD, 0], "max": [TOWN_N + TOWN_PAD, TOWN_N], "image": "module:ffr:blank", "clamp_view": True,
                     "dynamic_markers": [icons, goal]},
        "ffr_world": {"min": [0, 0], "max": [WORLD_N, WORLD_N], "image": "module:ffr:blank",
                      "clamp_view": True, "dynamic_markers": islands + [goal]},
    }}
    manifest = {
        "format": 1,
        "title_id": "0100606026F12000",
        "name": "FINAL FANTASY RESONANCE dual screen",
        "min_runtime": 25,
        "requires_module": True,
        "canvas_w": W,
        "canvas_h": H,
        "background": "#FF0A131B",
        "nav": False,
        "_note": "Generated by tools/ffr/gen_manifest.py. Every picture is composed by the module from the player's own game files.",
        "actions": actions,
        "enforce": enforce,
        "enforce_gate": {"point": "ffr.drv.pending", "max": 1},
        "map": map_cfg,
        "pages": [{"id": "main", "widgets": widgets}],
    }
    out = os.path.join(os.path.dirname(__file__), "..", "..", "packages", "FinalFantasyResonance", "dualscreen", "manifest.json")
    with open(out, "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    print("wrote", os.path.normpath(out), len(widgets), "widgets")


if __name__ == "__main__":
    main()
