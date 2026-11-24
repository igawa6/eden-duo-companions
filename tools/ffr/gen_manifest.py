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
TOWN_N, WORLD_N, MAX_ICONS, MAX_GOALS = 4096, 4096, 64, 16
# The town picture reaches TOWN_PAD past the capture's square on the left and right (the menu background), so a view as wide as
# the widget (1180 x 824) never runs off it: keep in step with kTownPad in the module.
TOWN_PAD = 904
ISLAND_SIZES, ISLANDS_PER_CLASS = [1024, 724, 512, 362, 256, 181], 12  # kIslandSizes in the module
BUTTONS = {"a": "A", "b": "B", "up": "DUp", "down": "DDown", "left": "DLeft", "right": "DRight",
           "minus": "Minus", "zl": "ZL", "r": "R"}  # (Minus, ZL, R: a fast travel through the game's world map)
MAX_MARKS = 64  # the world map's places (kMaxMarks)


def tap(rect, action, need=None, **extra):
    w = {"type": "rect", "rect": rect, "color": "#00000000", "on_tap": action}
    if need:
        w["need_bind"] = need
    w.update(extra)
    return w


def main():
    resets = []  # the map views' home resets, run by the module (enforce)
    actions, widgets = {}, [{"type": "image", "rect": [0, 0, W, H], "src_bind": "ffr.page", "src_hold": True}]
    for i in range(4):
        actions[f"tab{i}"] = {"kind": "module", "action": "tab", "argument": i}
        # Map, Party, Items, Quests; none in a battle (it takes the whole screen). Map: only with a map picture
        widgets.append(tap([i * W // 4, TAB_Y, W // 4, H - TAB_Y], f"tab{i}", "ffr.map_tab" if i == 0 else "ffr.tabs"))
    for i in range(4):
        actions[f"member{i}"] = {"kind": "module", "action": "member", "argument": i}
        widgets.append(tap([30, CARD_Y + i * (CARD_H + CARD_GAP), W - 60, CARD_H], f"member{i}", "ffr.party_list"))
    actions["back"] = {"kind": "module", "action": "back", "argument": 0}
    widgets.append(tap([0, 112, W, TAB_Y - 112], "back", "ffr.party_detail"))
    # The live map: one widget for a town or dungeon capture, one for the world map's islands. The module publishes the
    # player every tick (ffr.map.px / py / yaw), the view around them (ffr.map.vx0 ..) and the pictures as module keys; drag and
    # pinch look around.
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
        if area == "ffr_world":  # a place tapped: its card (the module), from the places' markers only
            w.update({"on_marker_tap": "mark", "marker_tap_groups": ["lm"], "marker_hit_px": 44})
        if area == "ffr_town":
            w["image_bind"] = "ffr.map.base"
            # The game's menu art fixed behind the map (the capture picture is transparent around the area): only the map moves
            # when it follows the player, and the art always fills the widget. A see-through bg keeps the host drawing it.
            w["bg"] = "#00000000"
            widgets.append({"type": "image", "rect": MAP, "src": f"module:ffr:bg:{MAP[2]}x{MAP[3]}", "need_bind": gate})
        else:
            w["overlays"] = ocean
            w["bg"] = "#FF485756"  # the ocean's own tone where no tile is drawn
        widgets.append(w)
        # The buttons. While the map follows the player (the view at home: ffr.map.<kind>_follow), + and - set the module's own
        # zoom, and the map keeps following at it. Once the player drags or pinches to look around (ffr.map.<kind>_look), they
        # zoom that view, as a pinch would. Home takes the zoom on screen (pinch included) as the module's, then the module has
        # the host send its view home (an enforce rule on ffr.map.<kind>_reset): the map follows the player again at that zoom.
        bx = MAP[0] + MAP[2] - 16 - 88
        kind = "world" if area == "ffr_world" else "town"
        for key, factor, y in (("in", 1.25, MAP[1] + 16), ("out", 0.8, MAP[1] + 120), ("home", 0, MAP[1] + MAP[3] - 104)):
            act = f"{name}_{key}"
            widgets.append({"type": "image", "rect": [bx, y, 88, 88], "src": f"module:ffr:btn:{key}", "need_bind": gate})
            if key == "home":
                actions[act] = {"kind": "module", "action": "home", "argument": 0}
                actions[act + "_reset"] = {"kind": "view_reset", "view": name, "enabled_bind": f"ffr.map.{kind}_reset"}
                resets.append({"action": act + "_reset", "every_ms": 1})
                widgets.append(tap([bx - 6, y - 6, 100, 100], act, gate, haptic="light"))
                continue
            actions[act] = {"kind": "module", "action": "zoom", "argument": 1 if factor > 1 else -1}
            actions[act + "_look"] = {"kind": "view_zoom_step", "view": name, "factor": factor}
            widgets.append(tap([bx - 6, y - 6, 100, 100], act, f"ffr.map.{kind}_follow", haptic="light"))
            widgets.append(tap([bx - 6, y - 6, 100, 100], act + "_look", f"ffr.map.{kind}_look", haptic="light"))
    # The scope switch in a town or dungeon: World > the place (a breadcrumb; the shown one in gold), Back while the world shows,
    # and the card of a place tapped on the world map: Fast Travel, Cancel while it runs; elsewhere on the card closes it
    actions["mark"] = {"kind": "module", "action": "mark", "argument": "$payload"}
    actions["mark_close"] = {"kind": "module", "action": "mark", "argument": -1}
    actions["scope_world"] = {"kind": "module", "action": "scope", "argument": 1}
    actions["scope_here"] = {"kind": "module", "action": "scope", "argument": 0}
    actions["travel"] = {"kind": "module", "action": "travel", "argument": 0}
    actions["travel_cancel"] = {"kind": "module", "action": "travel_cancel", "argument": 0}
    sx, sy = MAP[0] + 16, MAP[1] + 16
    widgets.append({"type": "image", "rect": [sx, sy, 560, 72], "src_bind": "ffr.map.scope_src", "need_bind": "ffr.map.scope"})
    widgets.append(tap([sx - 6, sy - 6, 196 + 16, 84], "scope_world", "ffr.map.scope", haptic="light"))
    widgets.append(tap([sx + 210, sy - 6, 560 - 204, 84], "scope_here", "ffr.map.scope", haptic="light"))
    by = MAP[1] + MAP[3] - 16 - 76
    widgets.append({"type": "image", "rect": [sx, by, 420, 76], "src_bind": "ffr.map.back_src", "need_bind": "ffr.map.back"})
    widgets.append(tap([sx - 6, by - 6, 432, 88], "scope_here", "ffr.map.back", haptic="light"))
    cw, ch = 620, 124
    cx, cy = MAP[0] + 440, MAP[1] + MAP[3] - 16 - ch  # between Back (left) and Home (right)
    widgets.append({"type": "image", "rect": [cx, cy, cw, ch], "src_bind": "ffr.map.card_src", "need_bind": "ffr.map.card"})
    widgets.append(tap([cx, cy, cw - 224, ch], "mark_close", "ffr.map.card"))
    widgets.append(tap([cx + cw - 224, cy, 224, ch], "travel", "ffr.map.card_go", haptic="light"))
    widgets.append(tap([cx + cw - 224, cy, 224, ch], "travel_cancel", "ffr.map.card_cancel", haptic="light"))
    # The battle page (the whole screen below the header): the module hit-tests the tap point against the layout it drew
    actions["btap"] = {"kind": "module", "action": "btap", "argument": "$payload"}
    widgets.append(tap([0, HEADER_H, W, H - HEADER_H], "btap", "ffr.battle_menu", payload="$tap"))
    tw = (W - 60) // 4
    for i in range(4):
        actions[f"cat{i}"] = {"kind": "module", "action": "cat", "argument": i}
        widgets.append(tap([30 + i * tw, 128, tw, 76], f"cat{i}", "ffr.items"))
    actions["scroll_down"] = {"kind": "module", "action": "scroll", "argument": 1}
    actions["scroll_up"] = {"kind": "module", "action": "scroll", "argument": -1}
    # Below the categories, the module hit-tests the tap (its layout changes with the category: the item list and the party,
    # the equipment's members, slots and choices); a swipe scrolls the list
    actions["itap"] = {"kind": "module", "action": "itap", "argument": "$payload"}
    widgets.append(tap([0, ROW_Y, W, TAB_Y - ROW_Y], "itap", "ffr.items", payload="$tap",
                       on_swipe_up="scroll_down", on_swipe_down="scroll_up"))
    # The quests: the journal's tabs, its list and the chosen quest's Track, hit-tested by the module
    actions["qtap"] = {"kind": "module", "action": "qtap", "argument": "$payload"}
    widgets.append(tap([0, 128, W, TAB_Y - 128], "qtap", "ffr.quests", payload="$tap",
                       on_swipe_up="scroll_down", on_swipe_down="scroll_up"))
    # The presses a battle tap becomes: the module raises one ffr.drv.<button> gate per sample (Drive in the module)
    enforce = []
    for key, button in BUTTONS.items():
        actions[f"ffr.press.{key}"] = {"kind": "button", "button": button, "frames": 4, "enabled_bind": f"ffr.drv.{key}"}
        enforce.append({"action": f"ffr.press.{key}", "every_ms": 1})
    enforce += resets
    icons = {"group": "icon", "count": MAX_ICONS, "x": "ffr.map.icon{i}.x", "y": "ffr.map.icon{i}.y",
             "icon_src_bind": "ffr.map.icon{i}.src", "size": 52}
    islands = [{"group": f"isl{c}", "count": ISLANDS_PER_CLASS, "x": f"ffr.map.isl{c}_{{i}}.x", "y": f"ffr.map.isl{c}_{{i}}.y",
                "icon_src_bind": f"ffr.map.isl{c}_{{i}}.src", "size_world": size} for c, size in enumerate(ISLAND_SIZES)]
    # the objectives (the game's marked quest target in gold, the side quests in progress in blue), over the other icons, in
    # both maps; and for each one out of view, an arrow at the edge of the map in its colour, pointing the way (kMaxGoals)
    goal = {"group": "goal", "count": MAX_GOALS, "x": "ffr.map.goal{i}.x", "y": "ffr.map.goal{i}.y",
            "icon_src_bind": "ffr.map.goal{i}.src", "size": 72}
    marks = {"group": "lm", "count": MAX_MARKS, "x": "ffr.map.lm{i}.x", "y": "ffr.map.lm{i}.y",
             "icon_src_bind": "ffr.map.lm{i}.src", "size": 60}
    arrow = {"group": "arrow", "count": MAX_GOALS, "x": "ffr.map.arrow{i}.x", "y": "ffr.map.arrow{i}.y",
             "icon_src_bind": "ffr.map.arrow{i}.src", "size": 76}
    map_cfg = {"style": {"background": "#FF0A131B"}, "areas": {
        "ffr_town": {"min": [-TOWN_PAD, 0], "max": [TOWN_N + TOWN_PAD, TOWN_N], "image": "module:ffr:blank",
                     "dynamic_markers": [icons, goal, arrow]},
        "ffr_world": {"min": [0, 0], "max": [WORLD_N, WORLD_N], "image": "module:ffr:blank",
                      "dynamic_markers": islands + [marks, goal, arrow]},
    }}
    manifest = {
        "format": 1,
        "title_id": "0100606026F12000",
        "name": "FINAL FANTASY RESONANCE dual screen",
        "min_runtime": 25,
        "requires_module": True,
        # The guest helper (native/helper/ffr_guest_helper.S, tools/ffr/build_helper_plan.py): fast travel through the game's own code
        "load_plan": "guest/<build>/load-plan.json",
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
