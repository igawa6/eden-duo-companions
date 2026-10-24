#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate native collection pages and the temporary held Echo grid.

The module owns validated inventory/context and native-localized resource state.
The held picker overrides any local page, without changing that page's scroll
region. Field maps remain gated by a matching live source. All artwork,
font glyphs and messages come from the player's own game/session.
"""
import json
import math
from stamp_manifest import pieces as stamp_pieces
from quick_slot_manifest import pieces as quick_slot_pieces
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "packages/EchoesOfWisdom/dualscreen/manifest.json"
BUILD_ID = "9F221E1AFA65BA5443E955158AB97DA871E5E41F000000000000000000000000"
# Filled native footer caps continue to the lower screen edge.
BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, BUTTON_GAP = 12, 948, 397, 132, 12
# HUD-slot rail beside the map panel; discs match the module's disc asset.
RAIL_X, RAIL_Y, RAIL_STEP, DISC = 1110, 274, 274, 196
# Map viewport inside the panel; must equal MapX/MapY/MapW/MapH in the module.
MAP_RECT = [24, 106, 930, 842]
def cost_box(percent):
    """Size and triangle-centre offset of the module's cost/<n>/<dim>/<percent> arc."""
    s = 1.1 * percent / 100
    radius, tri = 84 * s, math.ceil(34 * s)
    w = math.ceil(2 * radius * math.sin(math.radians(48)) + tri * 2)
    h = math.ceil(radius * (1 - math.cos(math.radians(48))) + tri * 1.6)
    return w, h, tri * .8


def cost_rect(cell_x, cell_w, apex_y, percent):
    w, h, apex = cost_box(percent)
    return [round(cell_x + cell_w / 2 - w / 2), round(apex_y - apex), w, h]


# Inspection pages sit 150px higher than the earlier header layout; the native
# name begins at the module's InspectTop and the detail column stops at 862.
TOP = 150
INSPECT_TOP, DETAIL_END = 382, 818
DETAIL_X, DETAIL_W = 820, 392


def manifest():
    actions = {
        "select_echo": {
            "kind": "module", "action": "select", "argument": "$payload",
            "enabled_bind": "echo.touch.ready", "haptic": "off",
        }
    }
    for name,enabled in (("warp_open","echo.warp.ready"),("warp_select","echo.warp.panel"),("warp_travel","echo.warp.confirm.ready"),("warp_close","echo.warp.panel")):
        actions[name]={"kind":"module","action":name,"argument":"$payload" if name=="warp_select" else 0,"enabled_bind":enabled,"haptic":"off"}
    actions["dungeon_floor_menu"]={"kind":"module","action":"dungeon_floor_menu","argument":0,"enabled_bind":"echo.dungeon.ready","haptic":"off"}
    actions["dungeon_floor"]={"kind":"module","action":"dungeon_floor","argument":"$payload","enabled_bind":"echo.dungeon.ready","haptic":"off"}
    # Host view_reset restores the active image's bound home without guest input.
    for scope, gate in (("field", "echo.map"), ("dungeon", "echo.dungeon")):
        for slot in range(64):
            actions[f"map_reset_{scope}_{slot}"] = {
                "kind": "view_reset", "view": f"echo_{scope}_map_{slot}",
                "enabled_bind": f"{gate}.slot.{slot}.reset", "haptic": "off"}
    actions["prologue_inspect"]={"kind":"module","action":"prologue_inspect","argument":"$payload","enabled_bind":"echo.prologue.ready","haptic":"off"}
    actions["current_echo"] = {
        "kind": "module", "action": "current_echo", "argument": 0,
        "enabled_bind": "echo.rail.y.valid", "haptic": "off",
    }
    actions["inspect"] = {
        "kind": "module", "action": "inspect", "argument": "$payload",
        "enabled_bind": "echo.inspect.ready", "haptic": "off",
    }
    for i in range(2):
        actions[f"notebook_category_{i}"]={"kind":"module","action":"notebook_category","argument":i,"enabled_bind":"echo.inspect.input","haptic":"off"}
    for name, argument in (("equipment", 1), ("items", 2), ("notebook", 3), ("map", 4)):
        actions[f"page_{name}"] = {
            "kind": "module", "action": "page", "argument": argument,
            "enabled_bind": "echo.map.enabled" if name == "map" else "echo.home.enabled",
            "haptic": "off",
        }
    for name, action, argument in (("sort_next","sort",0),("sort_previous","sort",1),("favorite_selected","favorite_selected","$payload"),("equip_echo","equip_echo","$payload"),("inventory_action","inventory_action","$payload")):
        actions[name]={"kind":"module","action":action,"argument":argument,"enabled_bind":"echo.touch.input" if name.startswith("sort") or name=="favorite_remove" else "echo.inspect.input","haptic":"off"}
    for kind in (0,1,4,5,6):
        actions[f"stamp_kind_{kind}"]={"kind":"module","action":"stamp_kind","argument":kind,"enabled_bind":"echo.stamp.ready","haptic":"off"}
    actions["stamp_tap"]={"kind":"module","action":"stamp_tap","argument":"$payload","enabled_bind":"echo.stamp.panel","haptic":"off"}
    for mode in range(5):
        actions[f"sort_to_{mode}"]={"kind":"module","action":"sort_to","argument":mode,
            "enabled_bind":"echo.touch.input","haptic":"off"}
    quick_bridge = quick_slot_pieces()
    bridge = stamp_pieces()
    actions.update(bridge["actions"])
    actions.update(quick_bridge["actions"])
    enforce = list(bridge["enforce"])+list(quick_bridge["enforce"])
    for name, direction in (("left", "-1,0"), ("right", "1,0"), ("neutral", "0,0")):
        key = f"drive_{name}"
        actions[key] = {
            "kind": "button", "button": f"RStick({direction})", "frames": 2,
            "enabled_bind": f"echo.drive.{name}", "haptic": "off",
        }
        enforce.append({"action": key, "every_ms": 0})

    for name,button in (("hold","DRight"),("hold_left","DLeft"),("x","X"),("y","Y")):
        actions[f"native_{name}"]={"kind":"button","button":button,"frames":2,"enabled_bind":f"echo.input.{name}","haptic":"off"}
        enforce.append({"action":f"native_{name}","every_ms":0})

    for name,button in (("plus","Plus"),("r","R"),("up","DUp"),("down","DDown"),("left","DLeft"),("right","DRight"),("a","A"),("b","B")):
        actions[f"menu_{name}"]={"kind":"button","button":button,"frames":2,"enabled_bind":f"echo.menu.input.{name}","haptic":"off"}
        enforce.append({"action":f"menu_{name}","every_ms":0})

    # Native warp driver owns these bounded pulses. Every abort/cancel must
    # publish neutral; companion selection never writes the game's position.
    for name,button in (("minus","Minus"),("a","A"),("b","B"),
                        ("neutral","LStick(0,0)"),("west","LStick(-1,0)"),("east","LStick(1,0)"),
                        ("north","LStick(0,1)"),("south","LStick(0,-1)"),
                        ("northwest","LStick(-0.70710678,0.70710678)"),
                        ("northeast","LStick(0.70710678,0.70710678)"),
                        ("southwest","LStick(-0.70710678,-0.70710678)"),
                        ("southeast","LStick(0.70710678,-0.70710678)")):
        actions["native_warp_"+name]={"kind":"button","button":button,"frames":8 if name in ("minus","a","b") else 2,
            "enabled_bind":"echo.warp.input."+name,"haptic":"off"}
        enforce.append({"action":"native_warp_"+name,"every_ms":0})

    def repeated(rect, **kwargs):
        return {
            "type": "image", "rect": rect,
            "repeat": 127, "repeat_bind": "echo.ui.count", "repeat_cols": 6,
            "repeat_dx": 204, "repeat_row_dy": 250, "scroll": "echo_grid",
            "need_bind": "echo.touch.echoes", **kwargs,
        }

    waiting = [
        # Asset-worker bootstrap; synchronous font/data callbacks only consume
        # ready immutable resources. This avoids rasterization on the UI thread.
        {"type": "image", "rect": [0, 0, 1, 1], "src_bind": "echo.bootstrap"},
        {"type": "image", "rect": [160, 213, 920, 654],
         "src": "romfs:/region_common/ui/Event_Title.arc#timg/__Combined.bntx#TitleWW_00^w"},
    ]
    held = [
        {"type":"image", "rect":[0,0,1240,1080], "src":"module:echoes/assets/held-background"},
        {"type":"image", "rect":[12,16,960,112], "src_bind":"echo.sort.tabs.image", "need_bind":"echo.sort.ready"},
        *[widget for i in range(5) for widget in (
            {"type":"image", "rect":[12+192*i,16,192,112], "src_bind":f"echo.sort.mode.{i}.image", "need_bind":"echo.sort.ready"},
            {"type":"image", "rect":[12+192*i,16,192,112], "on_tap":f"sort_to_{i}", "need_bind":"echo.sort.ready", "tap_enabled_bind":"echo.touch.input", "haptic":"off"})],
        {"type":"image", "rect":[990,28,238,64], "src_bind":"echo.sort.count.image", "need_bind":"echo.touch.ready"},
        repeated([16,168,188,242], src="module:echoes/assets/actor/188/242"),
        repeated([16,168,188,242], src="module:echoes/assets/actor-focus/188/242", need_bind="echo.cell.{i}.echo_selected"),
        repeated([30,204,160,160], src_bind="echo.cell.{i}.icon"),
        repeated(cost_rect(16,188,190,95), src_bind="echo.cell.{i}.cost"),
        repeated([16,168,188,242], src="module:echoes/assets/cursor/188/242", need_bind="echo.cell.{i}.echo_selected"),
        repeated([16,168,188,242], id="echo_cell_{i}", payload="$echo.cell.{i}.token", on_tap="select_echo", tap_enabled_bind="echo.touch.input", need_bind="echo.touch.echo.input", haptic="off"),
        {"type":"image", "rect":[0,910,1240,128], "src":"module:echoes/assets/name-banner"},
        {"type":"image", "rect":[48,942,1144,64], "src_bind":"echo.selected.heading", "need_bind":"echo.touch.ready"},
        {"type":"image", "rect":[420,1030,400,48], "src_bind":"echo.footer.instruction", "need_bind":"echo.touch.ready"},
    ]

    def held_machine(rect,**extra):
        return {"type":"image","rect":rect,"repeat":6,"repeat_bind":"echo.ui.count","repeat_cols":3,"repeat_dx":410,"repeat_row_dy":370,"scroll":"automaton_grid","need_bind":"echo.touch.machines",**extra}
    held += [
        {"type":"image","rect":[24,16,220,112],"src_bind":"echo.held.category","need_bind":"echo.touch.machines"},
        held_machine([16,168,388,350],src="module:echoes/assets/actor/388/350"),
        held_machine([16,168,388,350],src="module:echoes/assets/actor-focus/388/350",need_bind="echo.cell.{i}.machine_selected"),
        held_machine([70,198,280,280],src_bind="echo.cell.{i}.icon"),
        held_machine([16,168,388,350],src="module:echoes/assets/cursor/388/350",need_bind="echo.cell.{i}.machine_selected"),
        held_machine([16,168,388,350],id="held_machine_{i}",payload="$echo.cell.{i}.token",on_tap="select_echo",tap_enabled_bind="echo.touch.input",haptic="off"),
    ]

    def navigation(back=False):
        widgets=[]
        for index,name in enumerate(("back",) if back else ("equipment","items","notebook")):
            x=12 if back else BUTTON_X+index*(BUTTON_W+BUTTON_GAP)
            width=1216 if back else BUTTON_W
            rect=[x,BUTTON_Y,width,BUTTON_H]
            widgets.extend([
                {"type":"image", "rect":rect, "src":f"module:echoes/assets/bottom-button/{width}/{BUTTON_H}"},
                {"type":"image", "rect":[x+(width-381)//2,BUTTON_Y+24,381,100], "src_bind":"echo.back.image" if back else f"echo.nav.{index+1}.image"},
                {"type":"image", "rect":rect, "on_tap":"page_map" if back else f"page_{name}", "tap_enabled_bind":"echo.map.enabled" if back else "echo.home.enabled", "haptic":"off"},
            ])
        return widgets

    actions["quick_equip"]={"kind":"module","action":"quick_equip","argument":"$payload","enabled_bind":"echo.quick.equip.ready","haptic":"off"}
    for slot in range(3):
        actions[f"quick_assign_{slot}"]={"kind":"module","action":f"quick_assign_{slot}","argument":"$payload","enabled_bind":"echo.quick.assign.ready","haptic":"off"}

    def rail(assign=False):
        widgets=[]
        for k,cy in enumerate((280,551,799)):
            cx=RAIL_X+(14 if k==0 else -18 if k==1 else 0)
            left,top=cx-DISC//2,cy-DISC//2
            prefix=f"echo.quick.{k}."
            widgets += [
                {"type":"image","rect":[left,top,DISC,DISC],"src":f"module:echoes/assets/disc/{DISC}/{DISC}","need_bind":"echo.quick.visible"},
                {"type":"image","rect":[cx-104,cy-104,208,208],"src":"module:echoes/assets/quick-ring","need_bind":prefix+"active"},
                {"type":"image","rect":[cx-75,cy-75,150,150],"src_bind":prefix+"icon","need_bind":prefix+"valid"},
                {"type":"image","rect":cost_rect(left,DISC,top+6,110),"src_bind":prefix+"cost","need_bind":prefix+"valid"},
                {"type":"image","rect":[left-14,top+DISC-70,76,76],"src":"module:echoes/assets/badge/y/76/76","need_bind":prefix+"active"},
            ]
            target={"type":"image","rect":[left,top,DISC,DISC],"need_bind":"echo.quick.visible","haptic":"off"}
            if assign:
                target.update(drop_action=f"quick_assign_{k}",accept_group="echo_shortcuts",highlight_src="module:echoes/assets/quick-ring",tap_enabled_bind="echo.quick.assign.ready")
            else:
                target.update(on_tap="quick_equip",payload="$"+prefix+"token",tap_enabled_bind="echo.quick.equip.ready")
            widgets.append(target)
        return widgets

    for badge in ("x","y","a"):
        actions["combat_native_"+badge]={"kind":"button","button":badge.upper(),"frames":2,
            "enabled_bind":"echo.combat.input_ready","haptic":"off"}
    combat_rail=[widget for index,badge,cx,cy in ((3,"x",1124,280),(1,"y",1092,551),(4,"a",1110,822)) for widget in (
        {"type":"image","rect":[cx-98,cy-98,196,196],"src":"module:echoes/assets/disc/196/196","need_bind":"echo.combat.visible"},
        {"type":"image","rect":[cx-75,cy-75,150,150],"src_bind":f"echo.combat.{index}.icon","need_bind":"echo.combat.visible"},
        {"type":"image","rect":[cx-112,cy+28,76,76],"src":f"module:echoes/assets/badge/{badge}/76/76","need_bind":"echo.combat.visible"},
        {"type":"image","rect":[cx-98,cy-98,196,196],"on_tap":"combat_native_"+badge,
         "tap_enabled_bind":"echo.combat.input_ready","need_bind":"echo.combat.visible","haptic":"off"})]

    inspection_pages=[]
    for page_id in ("equipment","items","notebook"):
        scroll_id=f"inspect_{page_id}"
        def inspect_repeat(rect,**extra):
            return {"type":"image", "rect":rect, "repeat":512,"repeat_bind":"echo.inspect.count", "repeat_cols":5,"repeat_dx":154,"repeat_row_dy":154,"scroll":scroll_id,"need_bind":"echo.inspect.populated",**extra}
        widgets=[
            {"type":"image","rect":[0,0,1240,1080],"src":"module:echoes/assets/background"},
            {"type":"image","rect":[24,22,460,48],"src_bind":"echo.inspect.category","need_bind":"echo.inspect.ready"},
            {"type":"image","rect":[324,24,460,40],"src_bind":"echo.inspect.count.image","need_bind":"echo.inspect.populated"},
            {"type":"image","rect":[24,76,836,40],"src":"module:echoes/assets/name-line","need_bind":"echo.inspect.ready"},
            inspect_repeat([24,122,142,142],src="module:echoes/assets/cell/142/142"),
            inspect_repeat([33,135,124,124],src_bind="echo.inspect.cell.{i}.icon"),
            inspect_repeat(cost_rect(24,142,130,80),src_bind="echo.inspect.cell.{i}.cost"),
            inspect_repeat([24,122,142,142],src="module:echoes/assets/cursor/142/142",need_bind="echo.inspect.cell.{i}.selected"),
            inspect_repeat([62,214,100,48],src_bind="echo.inspect.cell.{i}.quantity_src"),
            inspect_repeat([26,208,48,48],src_bind="echo.inspect.cell.{i}.favorite"),
            inspect_repeat([102,200,64,64],src_bind="echo.inspect.cell.{i}.equipped_icon",need_bind="echo.inspect.cell.{i}.equipped"),
            inspect_repeat([24,122,142,142],id=f"{page_id}_cell_{{i}}",payload="$echo.inspect.cell.{i}.token",on_tap="inspect",tap_enabled_bind="echo.inspect.input",haptic="off"),
            {"type":"image","rect":[882,92,268,268],"src_bind":"echo.inspect.icon","need_bind":"echo.inspect.populated"},
            *[{"type":"image","rect":[DETAIL_X,INSPECT_TOP,DETAIL_W,height],"src_bind":"echo.inspect.heading","need_bind":f"echo.inspect.description.{height}.show"} for height in (64,136,180)],
            {"type":"image","rect":[DETAIL_X,0,DETAIL_W,40],"y_bind":"echo.inspect.line.y","src":"module:echoes/assets/name-line","need_bind":"echo.inspect.populated"},
            # Runtime scroll transforms and clipping apply to repeated widgets,
            # including a single description image. Keep it above the actions.
            *[{"type":"image","repeat":1,"rect":[DETAIL_X,INSPECT_TOP+height+56,DETAIL_W,1024],"src_bind":"echo.inspect.description","scroll":f"{scroll_id}_description_{height}","need_bind":f"echo.inspect.description.{height}.show"} for height in (64,136,180)],
            *([{"type":"image","rect":[820,836,112,112],"src_bind":"echo.inspect.favorite.image","on_tap":"favorite_selected","payload":"$echo.inspect.selected.token","need_bind":"echo.inspect.favorite.known","tap_enabled_bind":"echo.inspect.input","haptic":"off"},
               {"type":"image","rect":[944,836,268,112],"src":"module:echoes/assets/button/268/112","need_bind":"echo.inspect.populated"},
               {"type":"image","rect":[944,872,268,48],"src_bind":"echo.inspect.action.image","need_bind":"echo.inspect.populated"},
               {"type":"image","rect":[944,836,268,112],"on_tap":"equip_echo","payload":"$echo.inspect.selected.token","need_bind":"echo.inspect.populated","tap_enabled_bind":"echo.inspect.equip.ready","haptic":"off"}] if page_id=="notebook" else [
               {"type":"image","rect":[820,836,392,112],"src":"module:echoes/assets/button/392/112","need_bind":"echo.inspect.action.show"},
               {"type":"image","rect":[856,872,320,48],"src_bind":"echo.inspect.action.image","need_bind":"echo.inspect.action.show"},
               {"type":"image","rect":[820,836,392,112],"on_tap":"inventory_action","payload":"$echo.inspect.selected.token","need_bind":"echo.inspect.action.show","tap_enabled_bind":"echo.inspect.action.ready","haptic":"off"}]),
            {"type":"image","rect":[160,100,920,654],"src":"romfs:/region_common/ui/Event_Title.arc#timg/__Combined.bntx#TitleWW_00^w","need_bind":"echo.inspect.empty"},
            *navigation(back=True),
        ]
        description_scrolls=[{"id":f"{scroll_id}_description_{height}","rect":[DETAIL_X,INSPECT_TOP+height+56,DETAIL_W+20,DETAIL_END-(INSPECT_TOP+height+56)],"content_h_bind":"echo.inspect.description.h","show_bind":f"echo.inspect.description.{height}.show","reset_bind":"echo.inspect.detail.epoch","fling":True,"bar_w":8,"bar":"#FFF3D99D","bar_track":"#00151B25"} for height in (64,136,180)]
        if page_id=="notebook":
            widgets=[
                {"type":"image","rect":[0,0,1240,1080],"src":"module:echoes/assets/background"},
                {"type":"image","rect":[24,8,440,112],"src_bind":"echo.notebook.tabs","need_bind":"echo.inspect.ready"},
                *[w for i in range(2) for w in (
                    {"type":"image","rect":[24+i*220,8,220,112],"src_bind":f"echo.notebook.tab.{i}","need_bind":"echo.inspect.ready"},
                    {"type":"image","rect":[24+i*220,8,220,112],"on_tap":f"notebook_category_{i}","tap_enabled_bind":"echo.inspect.input","need_bind":"echo.inspect.ready","haptic":"off"})],
                {"type":"image","rect":[474,44,460,40],"src_bind":"echo.inspect.count.image","need_bind":"echo.inspect.populated"},

            ]
            def echo_cell(rect,**extra):
                return {"type":"image","rect":rect,"repeat":512,"repeat_bind":"echo.inspect.count","repeat_cols":7,"repeat_dx":131,"repeat_row_dy":130,"scroll":scroll_id,"need_bind":"echo.notebook.echoes",**extra}
            widgets += [
                echo_cell([24,132,128,128],src="module:echoes/assets/cell/128/128"),
                echo_cell([24,132,128,128],id="notebook_cell_{i}",src_bind="echo.inspect.cell.{i}.icon",payload="$echo.inspect.cell.{i}.token",on_tap="inspect",tap_enabled_bind="echo.inspect.input",select_group="echo_shortcuts",haptic="off"),
                echo_cell(cost_rect(24,128,142,72),src_bind="echo.inspect.cell.{i}.cost"),
                echo_cell([24,132,128,128],src="module:echoes/assets/cursor/128/128",need_bind="echo.inspect.cell.{i}.echo_selected"),
                echo_cell([26,222,36,36],src_bind="echo.inspect.cell.{i}.favorite"),
                echo_cell([98,214,48,48],src_bind="echo.inspect.cell.{i}.equipped_icon",need_bind="echo.inspect.cell.{i}.echo_equipped"),
            ]
            def machine_cell(rect,**extra):
                return {"type":"image","rect":rect,"repeat":6,"repeat_bind":"echo.inspect.count","repeat_cols":3,"repeat_dx":310,"repeat_row_dy":260,"scroll":"inspect_automatons","need_bind":"echo.notebook.machines",**extra}
            widgets += [
                machine_cell([24,132,300,250],src="module:echoes/assets/cell/300/250"),
                machine_cell([59,142,230,230],id="automaton_cell_{i}",src_bind="echo.inspect.cell.{i}.icon",payload="$echo.inspect.cell.{i}.token",on_tap="inspect",tap_enabled_bind="echo.inspect.input",select_group="echo_shortcuts",haptic="off"),
                machine_cell([24,132,300,250],src="module:echoes/assets/cursor/300/250",need_bind="echo.inspect.cell.{i}.machine_selected"),
                machine_cell([256,314,48,48],src_bind="echo.inspect.cell.{i}.equipped_icon",need_bind="echo.inspect.cell.{i}.machine_equipped"),
            ]
            for height in (64,136,180):
                desc_y=680+height+64
                widgets += [
                    {"type":"image","rect":[24,680,580,height],"src_bind":"echo.inspect.heading","need_bind":f"echo.inspect.description.{height}.show"},
                    {"type":"image","rect":[24,desc_y-50,930,24],"src":"module:echoes/assets/name-line","need_bind":f"echo.inspect.description.{height}.show"},
                    {"type":"image","repeat":1,"rect":[24,desc_y,930,1024],"src_bind":"echo.inspect.description","scroll":f"{scroll_id}_description_{height}","need_bind":f"echo.inspect.description.{height}.show"},
                ]
            widgets += [
                {"type":"image","rect":[0,0,64,64],"x_bind":"echo.inspect.favorite.x","y_bind":"echo.inspect.favorite.y","src_bind":"echo.inspect.favorite.image","need_bind":"echo.inspect.favorite.known"},
                {"type":"image","rect":[0,0,112,112],"x_bind":"echo.inspect.favorite.x","y_bind":"echo.inspect.favorite.y","on_tap":"favorite_selected","payload":"$echo.inspect.selected.token","need_bind":"echo.inspect.favorite.known","tap_enabled_bind":"echo.inspect.input","haptic":"off"},
                *rail(assign=True),*navigation(back=True),
            ]
            description_scrolls=[{"id":f"{scroll_id}_description_{height}","rect":[24,680+height+64,930,948-(680+height+64)],"content_h_bind":"echo.inspect.description.h","show_bind":f"echo.inspect.description.{height}.show","reset_bind":"echo.inspect.detail.epoch","fling":True,"bar_w":8,"bar":"#FFF3D99D","bar_track":"#00151B25"} for height in (64,136,180)]
        inspection_pages.append({"id":page_id,"name":"","widgets":widgets,"scrolls":[{"id":scroll_id,"rect":[16,132,938,518] if page_id=="notebook" else [16,110,788,838],"row_h":130 if page_id=="notebook" else 154,"cols":7 if page_id=="notebook" else 5,"show_bind":"echo.notebook.echoes" if page_id=="notebook" else "echo.inspect.populated","reset_bind":"echo.inspect.epoch","follow_bind":"echo.inspect.index","fling":True,"bar_w":12,"bar":"#FFF3D99D","bar_track":"#00151B25"},*([{ "id":"inspect_automatons","rect":[16,130,938,528],"row_h":260,"cols":3,"show_bind":"echo.notebook.machines","reset_bind":"echo.inspect.epoch","follow_bind":"echo.inspect.index","fling":True,"bar_w":12,"bar":"#FFF3D99D"}] if page_id=="notebook" else []),*description_scrolls]})
    map_page = {
        "id": "map", "name": "", "widgets": [
            {"type": "image", "rect": [0, 0, 1240, 1080],
             "src": "module:echoes/assets/background"},
            # Framed board like the Link's Awakening map page, in the native
            # Dictionary rope-pattern window with the rose heading and line.
            {"type": "image", "rect": [8, 8, 962, 948],
             "src": "module:echoes/assets/panel/962/948"},
            {"type": "image", "rect": [24, 24, 930, 64], "src_bind": "echo.map.heading"},
            {"type": "image", "rect": [36, 84, 694, 32], "src": "module:echoes/assets/map-line", "need_bind":"echo.map.field_scope"},
            {"type": "image", "rect": [36, 84, 906, 32], "src": "module:echoes/assets/map-line", "need_bind":"echo.dungeon.supported"},
            # A fresh widget identity prevents the host's last-good image
            # cache carrying terrain across uncertain save/source changes.
            # Explicit widgets also keep input lookup and rendering IDs equal.
            *[{"type": "image", "id": f"echo_field_map_{slot}",
               "rect": MAP_RECT, "src_bind": "echo.map.image",
               "need_bind": f"echo.map.slot.{slot}.visible", "pan_zoom": True,
               "min_zoom": 1, "max_zoom": 4,
               "view_zoom_bind": "echo.map.view.zoom",
               "view_reset_bind": f"echo.map.slot.{slot}.visible",
               "view_cx_bind": "echo.map.view.center",
               "view_cy_bind": "echo.map.view.center", "on_tap":"stamp_tap", "payload":"$tap"} for slot in range(64)],
            *[{"type":"image","id":f"echo_dungeon_map_{slot}",
               "rect":MAP_RECT,"src_bind":"echo.dungeon.image",
               "need_bind":f"echo.dungeon.slot.{slot}.visible","pan_zoom":True,
               "min_zoom":1,"max_zoom":4,"view_zoom_bind":"echo.dungeon.view.zoom",
               "view_reset_bind":f"echo.dungeon.slot.{slot}.visible",
               "view_cx_bind":"echo.dungeon.view.cx","view_cy_bind":"echo.dungeon.view.cy"}
              for slot in range(64)],
            *[{"type":"image","rect":[0,0,345,48],"src_bind":f"echo.map.label.{i}.image","x_bind":f"echo.map.label.{i}.x","y_bind":f"echo.map.label.{i}.y","need_bind":f"echo.map.label.{i}.visible"} for i in range(26)],
            *[{"type":"image","rect":[0,0,64,64],"src_bind":f"echo.dungeon.icon.{i}.image","x_bind":f"echo.dungeon.icon.{i}.x","y_bind":f"echo.dungeon.icon.{i}.y","need_bind":f"echo.dungeon.icon.{i}.visible"} for i in range(64)],
            *[{"type":"image","rect":[0,0,28,28],"src_bind":f"echo.entrance.{i}.image","x_bind":f"echo.entrance.{i}.x","y_bind":f"echo.entrance.{i}.y","need_bind":f"echo.entrance.{i}.visible"} for i in range(96)],
            *[{"type":"image","rect":[0,0,112,112],"src_bind":f"echo.quest.{i}.image","x_bind":f"echo.quest.{i}.x","y_bind":f"echo.quest.{i}.y","need_bind":f"echo.quest.{i}.visible"} for i in range(32)],
            *[{"type":"image","rect":[0,0,28,28],"src_bind":f"echo.landmark.{i}.image","x_bind":f"echo.landmark.{i}.x","y_bind":f"echo.landmark.{i}.y","need_bind":f"echo.landmark.{i}.visible"} for i in range(6)],
            # Exact native unlocked waypoint positions. The module additionally
            # gates revealed coverage, current scope and the pan/pinch transform.
            *[widget for slot in range(64) for widget in (
                {"type":"image","rect":[0,0,56,56],"src":"module:echoes/assets/warp",
                 "x_bind":f"echo.warp.{slot}.x","y_bind":f"echo.warp.{slot}.y","need_bind":f"echo.warp.{slot}.visible"},
                {"type":"image","rect":[0,0,112,112],"src":"module:echoes/assets/cursor/112/112",
                 "x_bind":f"echo.warp.{slot}.hit_x","y_bind":f"echo.warp.{slot}.hit_y","need_bind":f"echo.warp.{slot}.selected"},
                {"type":"image","rect":[0,0,112,112],"on_tap":"warp_select","payload":f"$echo.warp.{slot}.token",
                 "x_bind":f"echo.warp.{slot}.hit_x","y_bind":f"echo.warp.{slot}.hit_y","need_bind":f"echo.warp.{slot}.visible",
                 "tap_enabled_bind":"echo.warp.panel","haptic":"off"})],
            {"type": "image", "id": "echo_player", "rect": [0, 0, 64, 64],
             "src": "module:echoes/assets/player", "x_bind": "echo.map.marker.x",
             "y_bind": "echo.map.marker.y", "need_bind": "echo.map.marker.visible"},
            {"type":"image","rect":[66,204,820,584],"src":"romfs:/region_common/ui/Event_Title.arc#timg/__Combined.bntx#TitleWW_00^w","need_bind":"echo.map.waiting"},
            *[{"type":"image","rect":[0,0,60,60],"src_bind":f"echo.stamp.{slot}.icon","x_bind":f"echo.stamp.{slot}.x","y_bind":f"echo.stamp.{slot}.y","need_bind":f"echo.stamp.{slot}.visible"} for slot in range(24)],
            # Reset stays above map content, clear of the accepted footer. The
            # repeated hit layers dispatch only to the currently visible image ID.
            {"type":"image","rect":[40,842,144,80],"src":"module:echoes/assets/bottom-button/144/80","need_bind":"echo.map.reset.show"},
            {"type":"image","rect":[40,842,144,80],"src_bind":"echo.map.reset.image","need_bind":"echo.map.reset.show"},
            *[{"type":"image","rect":[40,822,144,112],"repeat":64,
               "on_tap":f"map_reset_{scope}_{{i}}","need_bind":f"{gate}.slot.{{i}}.reset","haptic":"off"}
              for scope, gate in (("field","echo.map"),("dungeon","echo.dungeon"))],
            # Native rupee icon + count occupy the right HUD's clear header area.
            # Missing/invalid inventory observations hide both, including a zero.
            {"type":"image","rect":[1000,30,60,60],
             "src":"romfs:/region_common/ui/GameMain.arc#timg/__Combined.bntx#Rupee_00^w","need_bind":"echo.rupees.valid"},
            {"type":"image","rect":[1064,30,160,60],"src_bind":"echo.rupees.image","need_bind":"echo.rupees.valid"},
            # Native quick-travel launcher shares the area-name row with pins.
            {"type":"image","rect":[746,16,80,80],"src":"module:echoes/assets/button/80/80","need_bind":"echo.warp.closed"},
            {"type":"image","rect":[758,28,56,56],"src":"module:echoes/assets/warp-dark","need_bind":"echo.warp.closed"},
            {"type":"image","rect":[730,0,112,112],"on_tap":"warp_open","need_bind":"echo.warp.closed","haptic":"off"},
            # Native map pins are companion-local annotations; opening the tray
            # leaves map drag/pinch available. Tapping an existing pin removes it.
            {"type":"image","rect":[858,16,80,80],"src":"module:echoes/assets/button/80/80","need_bind":"echo.stamp.closed"},
            {"type":"image","rect":[868,26,60,60],"src":"module:echoes/assets/pin/1","need_bind":"echo.stamp.closed"},
            {"type":"image","rect":[842,0,112,112],"on_tap":"stamp_kind_1","need_bind":"echo.stamp.closed","haptic":"off"},
            # Pin choices occupy the HUD rail as a popup; terrain keeps its full viewport.
            {"type":"image","rect":[982,132,250,818],"src":"module:echoes/assets/panel/250/818","need_bind":"echo.stamp.panel"},
            *[widget for index,kind in enumerate((1,4,5,6)) for widget in (
                {"type":"image","rect":[988+(index%2)*124,222+(index//2)*136,112,112],"src":"module:echoes/assets/button/112/112","need_bind":"echo.stamp.panel"},
                {"type":"image","rect":[1014+(index%2)*124,248+(index//2)*136,60,60],"src":f"module:echoes/assets/pin/{kind}","need_bind":"echo.stamp.panel"},
                {"type":"image","rect":[988+(index%2)*124,222+(index//2)*136,112,112],"src":"module:echoes/assets/cursor/112/112","need_bind":f"echo.stamp.kind.{kind}"},
                {"type":"image","rect":[988+(index%2)*124,222+(index//2)*136,112,112],"on_tap":f"stamp_kind_{kind}","need_bind":"echo.stamp.panel","haptic":"off"})],
            {"type":"image","rect":[988,764,236,112],"src":"module:echoes/assets/button/236/112","need_bind":"echo.stamp.panel"},
            {"type":"image","rect":[988,800,236,48],"src_bind":"echo.stamp.back.image","need_bind":"echo.stamp.panel"},
            {"type":"image","rect":[988,764,236,112],"on_tap":"stamp_kind_0","need_bind":"echo.stamp.panel","haptic":"off"},
            # Warp uses the same right-side popup convention as personal pins.
            {"type":"image","rect":[982,132,250,818],"src":"module:echoes/assets/panel/250/818","need_bind":"echo.warp.panel"},
            {"type":"image","rect":[1080,278,56,56],"src":"module:echoes/assets/warp","need_bind":"echo.warp.panel"},
            {"type":"image","rect":[988,596,236,112],"src":"module:echoes/assets/button/236/112","need_bind":"echo.warp.confirm.show"},
            {"type":"image","rect":[988,632,236,48],"src_bind":"echo.warp.confirm.image","need_bind":"echo.warp.confirm.show"},
            {"type":"image","rect":[988,596,236,112],"on_tap":"warp_travel","need_bind":"echo.warp.confirm.show","tap_enabled_bind":"echo.warp.confirm.ready","haptic":"off"},
            {"type":"image","rect":[988,764,236,112],"src":"module:echoes/assets/button/236/112","need_bind":"echo.warp.panel"},
            {"type":"image","rect":[988,800,236,48],"src_bind":"echo.warp.back.image","need_bind":"echo.warp.panel"},
            {"type":"image","rect":[988,764,236,112],"on_tap":"warp_close","need_bind":"echo.warp.panel","haptic":"off"},
            {"type":"image","rect":[778,16,176,80],"src":"module:echoes/assets/button/176/80","need_bind":"echo.dungeon.ready"},
            {"type":"image","rect":[778,38,176,36],"src_bind":"echo.dungeon.floor.current","need_bind":"echo.dungeon.ready"},
            {"type":"image","rect":[926,50,18,12],"src":"module:echoes/assets/dropdown-chevron","need_bind":"echo.dungeon.ready"},
            {"type":"image","rect":[778,0,176,112],"on_tap":"dungeon_floor_menu","need_bind":"echo.dungeon.ready","haptic":"off"},
            *[{"type":"image","rect":[766,112,188,min(818,max(160,n*88+24))],"src":f"module:echoes/assets/panel/188/{min(818,max(160,n*88+24))}","need_bind":f"echo.dungeon.floor.menu.{n}"} for n in range(1,11)],
            *[{"type":"image","rect":[772,124,176,80],"repeat":10,"repeat_bind":"echo.dungeon.floor.rows",
               "repeat_cols":1,"repeat_row_dy":88,"scroll":"dungeon_floors","need_bind":"echo.dungeon.floor.menu",**extra}
              for extra in (
                {"src":"module:echoes/assets/button/176/80"},
                {"rect":[772,146,176,36],"src_bind":"echo.dungeon.floor.{i}.image"},
                {"src":"module:echoes/assets/cursor/176/80","need_bind":"echo.dungeon.floor.{i}.selected"},
                {"on_tap":"dungeon_floor","payload":"$echo.dungeon.floor.{i}.token","haptic":"off"})],
            *rail(),*combat_rail,
            *navigation(),
        ],
        "scrolls":[{"id":"dungeon_floors","rect":[772,124,182,796],"row_h":88,"cols":1,
                    "show_bind":"echo.dungeon.floor.menu","reset_bind":"echo.dungeon.floor.epoch",
                    "follow_bind":"echo.dungeon.floor.index","fling":True,"bar_w":8,
                    "bar":"#FFF3D99D","bar_track":"#00151B25"}],
    }
    # Opening Link uses the map composition without exploration or Zelda actions.
    for badge in ("x", "y", "a"):
        actions["prologue_native_"+badge]={"kind":"button","button":badge.upper(),"frames":2,
            "enabled_bind":"echo.prologue.link","haptic":"off"}
    prologue_widgets = [
        {"type":"image","rect":[0,0,1240,1080],"src":"module:echoes/assets/background"},
        {"type":"image","rect":[8,8,962,1064],"src":"module:echoes/assets/panel/962/1064"},
        {"type":"image","rect":[24,24,930,64],"src_bind":"echo.prologue.heading"},
        {"type":"image","rect":[36,84,906,32],"src":"module:echoes/assets/map-line"},
        {"type":"image","rect":[72,286,834,594],"src":"romfs:/region_common/ui/Event_Title.arc#timg/__Combined.bntx#TitleWW_00^w"},
        *[widget for index,badge,cx,cy in ((3,"x",1124,280),(1,"y",1092,551),(4,"a",1110,822)) for widget in (
            {"type":"image","rect":[cx-98,cy-98,196,196],"src":"module:echoes/assets/disc/196/196","need_bind":"echo.prologue.link"},
            {"type":"image","rect":[cx-75,cy-75,150,150],"src_bind":f"echo.prologue.{index}.icon","need_bind":"echo.prologue.link"},
            {"type":"image","rect":[cx-112,cy+28,76,76],"src":f"module:echoes/assets/badge/{badge}/76/76","need_bind":"echo.prologue.link"},
            {"type":"image","rect":[cx-98,cy-98,196,196],"on_tap":"prologue_native_"+badge,
             "tap_enabled_bind":"echo.prologue.link","need_bind":"echo.prologue.link","haptic":"off"})],
    ]
    prologue_page={"id":"prologue","name":"","widgets":prologue_widgets}
    # A manifest reload may retain any previous page. Resource warmup must not
    # depend on visiting the initial waiting page, including a reload mid-hold.
    for widgets in [held, prologue_widgets, map_page["widgets"], *[page["widgets"] for page in inspection_pages]]:
        widgets.insert(0, {"type": "image", "rect": [0, 0, 1, 1], "src_bind": "echo.bootstrap"})
    result = {
        "format": 1, "title_id": "01008CF01BAAC000",
        "name": "Echoes of Wisdom companion", "min_runtime": 22,
        "canvas_w": 1240, "canvas_h": 1080, "background": "#FF000000",
        "requires_module": True, "nav": False, "poll_hz": 60, "anim_hz": 60,
        "module": {"abi": 1, "build_ids": [BUILD_ID]},
        # Guest helper: sets Y through the native setter (tools/echoes-of-wisdom/hook.S).
        "load_plan": "load-plans/<build>.json", "load_plan_module": "main",
        "font": "module:echoes/body-font",
        "font_atlas": "module:echoes/body-atlas?p={p}", "font_page_h": 512,
        "_about": "Native Equipment, Items and Notebooks with Echo/Automaton shortcuts and native held grids "
                  "for update 1.1.0. "
                  "Physical Right/Left D-pad holds own native selection and release. "
                  "Touch jumps to verified native selections and refreshes the original carousel; sort tabs request one native sort operation. "
                  "Field terrain follows native discovery, story and weather; supported locations show Zelda’s native marker. "
                  "No game artwork, strings or font files are bundled.",
        "_development": "Selected native Notebook/Map/ActorSelect redesign; mock geometry reviewed. "
                        "Desktop and Thor NCE field/held selection exercised; major-dungeon live qualification pending.",
        "flags": {**bridge["flags"],**quick_bridge["flags"]},
        "persist_flags": bridge["persist_flags"]+quick_bridge["persist_flags"],
        "derived": [
            *bridge["derived"],*quick_bridge["derived"],
            *[{"name":f"echo.dungeon.floor.menu.{n}","expr":f"echo.dungeon.floor.menu && echo.dungeon.floor.rows == {n}"} for n in range(1,11)],
            *[{"name":f"{gate}.slot.{slot}.reset","expr":f"{gate}.slot.{slot}.visible && 'view_custom:echo_{scope}_map_{slot}'"} for scope,gate in (("field","echo.map"),("dungeon","echo.dungeon")) for slot in range(64)],
            {"name":"echo.map.reset.show","terms":[f"{gate}.slot.{slot}.reset" for gate in ("echo.map","echo.dungeon") for slot in range(64)]},
            *[{"name":f"echo.prologue.description.{height}.show","expr":f"echo.prologue.ready && echo.prologue.name.h == {height}"} for height in (64,136,180)],
            {"name":"echo.enforce.any","expr":"echo.drive.any || echo.stamp.enforce.any || echo.warp.input.any || echo.quickslot.enforce.any"},
            {"name":"echo.warp.confirm.show","expr":"echo.warp.panel && echo.warp.selected"},
            {"name":"echo.stamp.closed","expr":"echo.stamp.ready && !echo.stamp.panel"},
            *[{"name":f"echo.stamp.kind.{kind}","expr":f"echo.stamp.panel && echo.stamp.kind == {kind}"} for kind in (1,4,5,6)],
            {"name":"echo.map.waiting","expr":"echo.home.ready && !echo.map.ready"},
            {"name": "echo.touch.ready", "expr": "echo.ui.ready && echo.ui.held"},
            {"name":"echo.touch.input","expr":"echo.touch.ready && !echo.auto.busy"},
            {"name":"echo.touch.echoes","expr":"echo.touch.ready && !echo.ui.automaton"},
            {"name":"echo.touch.echo.input","expr":"echo.touch.input && !echo.ui.automaton"},
            {"name":"echo.touch.machines","expr":"echo.touch.ready && echo.ui.automaton"},
            {"name":"echo.notebook.echoes","expr":"echo.inspect.populated && !echo.notebook.automatons"},
            {"name":"echo.notebook.machines","expr":"echo.inspect.populated && echo.notebook.automatons"},
            {"name":"echo.inspect.input","expr":"echo.inspect.ready && !echo.auto.busy"},
            {"name": "echo.home.enabled", "expr": "echo.home.ready && !echo.ui.held"},
            {"name": "echo.map.enabled", "expr": "echo.home.ready && !echo.ui.held"},
            {"name": "echo.inspect.ready", "expr": "echo.home.ready && echo.page >= 1 && echo.page <= 3 && !echo.ui.held"},
            {"name": "echo.inspect.populated", "expr": "echo.inspect.ready && echo.inspect.count > 0"},
            {"name": "echo.inspect.empty", "expr": "echo.inspect.ready && echo.inspect.count == 0"},
            *[{"name": f"echo.inspect.description.{height}.show",
               "expr": f"echo.inspect.populated && echo.inspect.name.h == {height}"}
              for height in (64, 136, 180)],
        ],
        "enforce_gate": {"point": "echo.enforce.any", "max": 1},
        "enforce": enforce, "actions": actions,
        "pages": [
            {"id": "waiting", "name": "", "widgets": waiting},
            *inspection_pages, map_page,
            {"id": "held", "name": "", "widgets": held,
             "scrolls": [{"id": "echo_grid", "rect": [8, 156, 1232, 762],
                          "row_h": 250, "cols": 6, "show_bind": "echo.touch.echoes",
                          "reset_bind": "echo.ui.epoch", "follow_bind": "echo.ui.index",
                          "fling": True, "bar_w": 8,
                          "bar": "#FFE8C873", "bar_track": "#00201B19"},
                         {"id":"automaton_grid","rect":[8,156,1232,762],"row_h":370,"cols":3,"show_bind":"echo.touch.machines","reset_bind":"echo.ui.epoch","follow_bind":"echo.ui.index","fling":True,"bar_w":8,"bar":"#FFE8C873"}]},
            prologue_page,
        ],
        "page_binds": [
            {"point": "echo.page", "equals": index, "when_equal": {"page": name}}
            for index, name in enumerate(("waiting", "equipment", "items", "notebook", "map", "held", "prologue"))
        ],
    }
    # Expanded visible repeats change while scrolling. Stable IDs prevent the
    # frozen host from reusing another widget's last decoded fallback image.
    for page in result["pages"]:
        for index,widget in enumerate(page["widgets"]):
            widget.setdefault("id",f"echo_{page['id']}_piece_{index}"+("_{i}" if "repeat" in widget else ""))
    return result


def main():
    # Feedback passes run last over the finished manifest: haptics (one shared
    # vocabulary) and motion (short eased transitions, always on). Each owns
    # its own file so layout code stays untouched.
    import feedback_haptics, feedback_motion, feedback_popups
    result = feedback_popups.apply(feedback_motion.apply(feedback_haptics.apply(manifest())))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(OUT.relative_to(ROOT))


if __name__ == "__main__":
    main()
