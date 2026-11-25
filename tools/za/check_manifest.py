#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Sickops
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks for packages/PokemonLegendsZA/dualscreen/manifest.json.

Nothing here needs the runtime: it catches the mistakes that would otherwise only show up as a
blank label on the second screen.

  * every `bind`, `bind_text`, `max_bind`, `x_bind`, `y_bind`, `need_bind` and `hide_bind` names a
    point the native module publishes (or a runtime point: `@flag:`, `@nav.`, `@scroll:`);
  * every `on_tap` / `on_hold` names an action in `actions`, every `page` action targets a page
    that exists, and every `nav_order` id exists on its page;
  * every label/value/button string is covered by the packaged Latin-1 font;
  * text width is measured from the actual MFNT advances and stays inside the canvas, and a rect stays inside
    too (a small negative rect is allowed only when an x_bind/y_bind places it).

Run: python tools/za/check_manifest.py
"""
import json
import re
import sys
import struct
import copy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "packages/PokemonLegendsZA/dualscreen/manifest.json"
MODULE = ROOT / "native/modules/0100F43008C44000.cpp"

CANVAS = (1240, 1080)
FONT = set(chr(c) for c in range(0x20, 0x180)) | set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:-./%+, ")

ENTITY_FIELDS = ["icon", "sname", "v", "name", "sp", "nat", "lvl", "hp", "hpmax", "g", "shiny", "item", "form",
                 "ball", "nature", "xp", "xpmax", "selected"]


def published_points() -> set[str]:
    """Every `za.*` point the module's Sample() publishes, with the slot templates expanded."""
    source = MODULE.read_text(encoding="utf-8")
    points = set(re.findall(r'"(za\.[A-Za-z0-9_.]+)"', source))
    bindings=json.loads((ROOT/'tools/za/ui_bindings.generated.json').read_text('utf-8'))
    for key,spec in bindings.items():
        if '{i}' in spec[1]:points.update(key+str(i) for i in range(spec[6]))
        else:points.add(key)
    # PublishEntity is called with "za.p" / "za.b" prefixes and builds "<prefix><n>.<field>".
    points.update(f"za.cmd.attack{i}" for i in range(4))
    points.update({"za.quest.body.rows","za.map.body.rows"})
    points.update(f"za.moves.{field}" for field in ("category","power","accuracy"))
    points.update(f"za.dimension.type{i}" for i in range(3))
    points.update(f"za.type{i}.name" for i in range(18))
    points.discard("za.p")
    points.discard("za.b")
    for prefix, slots in (("za.p", 6), ("za.b", 30)):
        for slot in range(slots):
            for field in ENTITY_FIELDS:
                points.add(f"{prefix}{slot}.{field}")
    points.update(f"za.p{slot}.{key}{move}" for slot in range(6) for move in range(4) for key in "cr")
    points.update(f"za.p{slot}.deployed" for slot in range(6))
    for prefix, count, fields in [
        ("za.progress.r",3,["valid","level","points","item","icon","reward"]),
        ("za.rl",50,["v","selected","label","points","icon","reward"]),
        ("za.dim",50,["v","shown","kind","selected","x","y"]),
        ("za.mq",128,["v","shown","side","x","y","selected"]),
        ("za.landmark",32,["v","shown","x","y"]),
        ("za.mg",144,["x","y","count"]),
        ("za.mc",272,["name","type"]),
        ("za.quest.body.",128,["text","style","icon"]),
        ("za.map.body.",128,["text","style","icon"]),
        ("za.dp",60,["icon","name"]),
        ("za.nav.slot",3,["id","name","active"]),
        ("za.box.tile",32,["number","selected"]),
        ("za.gl",80,["label","live"]),
        ("za.go",64,["label","icon","mission"]),
        ("za.ge",64,["from_icon","to_icon","from","to","condition","item","stock"]),
        ("za.sk",30,["key"]),
        ("za.sr",1024,["icon","name","context"]),
        ("za.cp",20,["zone","names","count","recommended"]),
        ("za.bf",4,["id","count","name","pending","value"]),
        ("za.bt",1000,["name","context","icon","selected","eligible","hp","hpmax","status"]),
        ("za.gw",18,["type","factor"]),
        ("za.gc",24,["name","factor","strong","ready"]),
        ("za.boxdetail.m",4,["name","type","pp"]),
        ("za.moves.e",4,["name","type","selected"]),
        ("za.lm",192,["v","id","type","selected","name","origin"]),
        ("za.d",512,["number","selected","v","sp","icon","owned","state","available","name"]),
        ("za.i",1024,["selected","v","id","count","pouch","favorite","held","name"]),
        ("za.q",1024,["v","title","state","number","selected","category"]),
        ("za.hm",1024,["id","name","description","stock"]),
        ("za.zp",128,["v","icon","name","levels","state"]),
        ("za.cafe",24,["v","shown","x","y"]),("za.zone",20,["v","shown","number","x","y"]),("za.center",12,["v","shown","x","y"])]:
        for i in range(count):
            for field in fields: points.add(f"{prefix}{i}.{field}")
    for field in ["v","icon","name","sname","lvl","hp","hpmax","g","nature","sp","item","type1","type2","moves_ready"]:
        points.add(f"za.sel.{field}")
    for i in range(4):
        for field in ["id","name","type","pp","ppmax","category","power","accuracy","recharge","charging","recharge_text"]: points.add(f"za.sel.m{i}.{field}")
    for field in ["sp","icon","name","state","owned","type1","type2","height","weight","description"]:
        points.add(f"za.detail.{field}")
    # The per-generation counters are formatted at runtime ("za.dex.g%d" / "za.dex.g%d.max").
    for gen in range(9):
        points.add(f"za.dex.g{gen}")
        points.add(f"za.dex.g{gen}.max")
    return points



def expanded_widgets(widgets):
    """Validate the actual repeated cells, not only the first row's template."""
    def substitute(value, index):
        if isinstance(value,str):
            return re.sub(r"\{i([+-]\d+)?\}",lambda m:str(index+int(m.group(1) or 0)),value)
        if isinstance(value,list): return [substitute(v,index) for v in value]
        if isinstance(value,dict): return {k:substitute(v,index) for k,v in value.items()}
        return value
    for source in widgets:
        count=source.get("repeat",0)
        if not count:
            yield source
            continue
        if not isinstance(count,int) or not 0<count<=2048:
            raise ValueError(f"invalid static repeat {count!r}")
        for i in range(count):
            w=substitute(copy.deepcopy(source),i)
            x,y,rw,rh=w["rect"]
            cols=source.get("repeat_cols",0)
            if cols:
                x+=(i%cols)*source.get("repeat_dx",0)
                y+=(i//cols)*source.get("repeat_row_dy",0)
            else:
                x+=i*source.get("repeat_dx",0)
                y+=i*source.get("repeat_dy",0)
            w["rect"]=[x,y,rw,rh]
            yield w

def check(manifest=None):
    if manifest is None:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    points = published_points()
    metrics_path = MANIFEST.parent / manifest['font'].removeprefix('file:')
    metrics = metrics_path.read_bytes()
    line_height = struct.unpack_from('<I', metrics, 0x1c)[0]
    first = struct.unpack_from('<I', metrics, 0x28)[0]
    glyph_count = struct.unpack_from('<I', metrics, 0x20)[0] - 1
    if line_height <= 0 or first + (glyph_count+1)*14 > len(metrics):
        return ["invalid MFNT metrics"]
    font = {chr(c) for c in range(0x20,0x20+glyph_count)}
    def text_width(text, scale):
        return sum(struct.unpack_from('<H', metrics, first + (ord(c)-0x20+1)*14 + 12)[0]
                   for c in text if c in font) * scale * 5 / line_height
    errors = []

    def is_runtime_point(name: str) -> bool:
        return (name in {"module_ready", "module_error", "module_error_message"} or
                name.startswith("@flag:") or name.startswith("@nav.") or
                name.startswith("@scroll:") or name.startswith("@scroll_"))

    def check_point(name, where, allow_missing=False):
        if not name:
            return
        base = name[1:] if name.startswith("!") else name
        base = base.replace("{i}", "0")
        if is_runtime_point(base):
            return
        if base not in points and not allow_missing:
            errors.append(f"{where}: unknown point {name!r}")

    def check_text(text, where):
        bad = sorted({c for c in text if c not in font and c not in "\n\r"})
        if bad:
            errors.append(f"{where}: glyphs the packaged font lacks: {bad!r} in {text!r}")

    pages = {page["id"]: page for page in manifest["pages"]}
    if len(pages) != len(manifest["pages"]):
        errors.append("duplicate page id")
    actions = manifest.get("actions", {})

    for page in manifest["pages"]:
        pid = page["id"]
        ids = set()
        for w in expanded_widgets(page["widgets"]):
            wid = w.get("id")
            if wid:
                if wid in ids: errors.append(f"{pid}: duplicate widget id {wid!r}")
                ids.add(wid)
            for key in ("bind", "bind_text", "max_bind", "x_bind", "y_bind"):
                if key in w:
                    check_point(w[key], f"{pid}/{wid or w.get('type')}.{key}")
            if isinstance(w.get("payload"),str) and w["payload"].startswith("$") and w["payload"]!="$tap":
                check_point(w["payload"][1:],f"{pid}/{wid}.payload")
            for key in ("need_bind", "hide_bind"):
                if key in w:
                    check_point(w[key], f"{pid}/{wid or w.get('type')}.{key}")
            for key in ("on_tap", "on_hold", "on_swipe_left", "on_swipe_right", "on_swipe_up", "on_swipe_down"):
                if key in w:
                    if w[key] not in actions:
                        errors.append(f"{pid}/{wid or w.get('type')}: unknown action {w[key]!r}")
            if "hide_eq" in w and not isinstance(w["hide_eq"], int):
                errors.append(f"{pid}/{wid or w.get('type')}: hide_eq must be an integer")
            text = w.get("text")
            if isinstance(text, str) and text:
                check_text(text, f"{pid}/{wid or w.get('type')}.text")
                scale = w.get("text_scale", 3)
                width = text_width(text, scale)
                x, y, rw, rh = w.get("rect", [0, 0, 0, 0])
                if w.get("type") == "button":
                    # DrawButton centres the caption in the rect, it does not use align's anchor.
                    if width > rw - 8:
                        errors.append(f"{pid}/{wid}: caption {text!r} is {width} px wide in a "
                                      f"{rw} px button")
                elif "x_bind" not in w:
                    align = w.get("align", "left")
                    left = x - (width // 2 if align == "center" else width if align == "right" else 0)
                    if left < 0 or left + width > CANVAS[0]:
                        errors.append(f"{pid}/{wid or w.get('type')}: text {text!r} spans "
                                      f"{left}..{left + width} px")
            rect = w.get("rect")
            if rect and "x_bind" not in w and "y_bind" not in w and not w.get("scroll"):
                x, y, rw, rh = rect
                if x < 0 or y < 0 or x + rw > CANVAS[0] or y + rh > CANVAS[1]:
                    errors.append(f"{pid}/{wid or w.get('type')}: rect {rect} leaves the canvas")
        for wid in page.get("nav_order", []):
            if wid not in ids:
                errors.append(f"{pid}: nav_order names {wid!r}, which no widget has")
        for nav in ("nav_shortcut_0","nav_shortcut_1","nav_shortcut_2","nav_more"):
            if nav not in ids or nav not in actions:
                errors.append(f"{pid}: missing fixed navigation button {nav!r}")

    for rule in manifest.get("enforce", []):
        if rule.get("action") not in actions:
            errors.append(f"enforce: unknown action {rule.get('action')!r}")

    for name, action in actions.items():
        kind = action.get("kind")
        if kind == "page":
            target = action.get("page")
            if target not in pages:
                errors.append(f"action {name}: page {target!r} does not exist")
        elif kind == "module":
            if action.get("action") in {"guide_locations","guide_combat","guide_dex_pin","guide_evolutions",
                                       "quest_pin","quest_load_map","guide_open","guide_remove","guide_map","zone_missing_toggle",
                                       "guide_capture","guide_capture_map","guide_item","guide_item_back","map_panel","map_group",
                                       "map_choice","nav_more","nav_edit","nav_shortcut","nav_slot","nav_pick","box_picker",
                                       "search_open","search_mode","search_keyboard","search_key","search_digit",
                                       "search_clear","search_erase","search_pick","bag_help","bag_favor","bag_favorite_pick","bag_target",
                                       "bag_fiche","bag_hold_team","bag_hold_box","bag_hold_browse","bag_hold_clear","bag_hold_end","bag_hold_apply","bag_heal_apply","moves_bag",
                                       "map_recenter","team_heal_open","team_heal_pick","team_heal_back"}:
                continue
            if action.get("action") == "native_map_travel":
                if not manifest.get("load_plan"):
                    errors.append(f"action {name}: native travel requires the load plan")
                continue
            if action.get("action") not in {"moves_team", "moves_box", "moves_slot", "moves_pick", "moves_apply_guest", "native_box_exchange_guest", "native_box_prepare", "native_box_party", "native_box_cancel", "nav_next", "nav_prev", "box_details", "box_prev", "box_next", "box_set", "box_follow",
                                            "refresh", "dex_gen", "dex_filter", "dex_prev", "dex_next", "bag_category", "bag_prev", "bag_next",
                                            "research_open", "research_pick", "research_close", "dimension_pick", "box_pick", "box_swap_pick", "box_swap_confirm", "box_swap_cancel", "bag_pick", "zone_pick", "map_tap", "map_quest_pick", "landmark_pick", "map_mark_start", "map_mark_clear", "map_mark_at", "map_control", "map_pan", "map_pinch", "cafe_pick", "attack", "team_select", "send_out", "center_pick", "center_close", "center_travel", "native_map_travel", "travel_cancel", "dex_pick", "quest_pick", "quest_category", "quest_map"}:
                errors.append(f"action {name}: module action {action.get('action')!r} is not "
                              f"implemented by the module")

    for bind in manifest.get("page_binds",[]):
        check_point(bind.get("point"),"page_bind")
        check_point(bind.get("ready_bind"),"page_bind.ready_bind")
        for branch in ["when_equal","when_not_equal"]:
            target=bind.get(branch,{}).get("page")
            if target and target not in pages: errors.append(f"page_bind: unknown page {target!r}")
    for table, names in manifest.get("tables", {}).items():
        for entry in names:
            check_text(entry, f"table {table}")

    if manifest.get("title_id") != "0100F43008C44000":
        errors.append("title_id is not Pokemon Legends Z-A")

    return errors


def main() -> int:
    errors = check()
    if errors:
        for line in errors:
            print("ERROR", line)
        print(f"{len(errors)} problem(s)")
        return 1
    print("za-manifest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
