#!/usr/bin/env python3
"""Writes packages/ChainedEchoes/dualscreen/manifest.json (Chained Echoes 1.41 companion, battle B1
slice: accepted design redesign 2026-10-09, battle-b1 / battle-early layouts, turn strip T1).

The module composes every picture from the player's romfs (native/modules/ce_pages.cpp) and
publishes which one to show; this manifest only stacks them and places the tap targets. Geometry
comes from native/modules/ce_pages.h (namespace geo).

Pages:
- "main" (Field Home slice): Field Home / Board / placeholder pictures come from the module too;
  the live player marker is a widget moved by ce.fmx / ce.fmy, the page buttons are module actions
  (fpage, btile, bsec) gated by ce.fhome / ce.fsub / ce.fboard.
- "main": one full-canvas picture (ce.page; a prefetch widget under it loads the next picture so
  a change never blanks), the loading dim, battle tap targets (enemy chips, target card / bestiary
  sheet), the "Battle HUD" button outside battles and a hold-anywhere gesture for the settings.
  A host-font notice covers everything when no module instance runs (an unsupported build).
  Sky Armor battles: three gear-row targets (arm, then fire the game's R press).
- "main", Skills page: Back, member picker / swipe, plan editing on the passive slots (drag or
  tap-tap a learned passive onto an empty slot; module-local plan, no game writes), gated by ce.sk.*
  (contracts/skill-readers.md). Game over: ce.gameover dims the kept battle picture.
- "main", Crystals page (ce_crystal_reader / ce_page_crystals): Back, member picker / swipe, category
  chips, the crystal grid (image widgets: tap selects, drag lifts the cell), slot rows (tap-tap or drop
  plans a crystal; module-local, never written to the game), equipment names (Transfer), the three
  bottom buttons, the filter / transfer-target pickers; gated by ce.cr.* (contracts/crystal-readers.md).
- "@settings" (package-owned): SHOW/HIDE switches for the game's top-screen turn order strip,
  party HP/TP panel, Overdrive gauge, Ultra Move bar and Sky Armor gear badge, default HIDE,
  persisted; flag-gated enforce entries re-send each switch to the module twice a second (contracts/hud-hide.md restore rules live in the module)."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEO = (ROOT / "native/modules/ce_pages.h").read_text()
G = {k: int(v) for k, v in re.findall(r"\b([A-Z][A-Za-z0-9]+) = (\d+)\b", GEO)}
# Sky Armor page geometry (native/modules/ce_page_skyarmor.h namespace geo)
G.update({k: int(v) for k, v in re.findall(r"\b([A-Z][A-Za-z0-9]+) = (\d+)\b",
                                            (ROOT / "native/modules/ce_page_skyarmor.h").read_text())})
# Field Home slice geometry (native/modules/ce_page_field.h, namespace geo)
FGEO = (ROOT / "native/modules/ce_page_field.h").read_text()
F = {k: int(v) for k, v in re.findall(r"\b([A-Z][A-Za-z0-9]+) = (\d+)\b", FGEO)}
# Skills page geometry (native/modules/ce_page_skills.h, namespace geo)
SK = {k: int(v) for k, v in re.findall(r"\b([A-Z][A-Za-z0-9]+) = (\d+)\b",
                                       (ROOT / "native/modules/ce_page_skills.h").read_text())}
# Crystals page geometry (native/modules/ce_page_crystals.h, namespace geo)
CR = {k: int(v) for k, v in re.findall(r"\b([A-Z][A-Za-z0-9]+) = (\d+)\b",
                                       (ROOT / "native/modules/ce_page_crystals.h").read_text())}
W, H = 1240, 1080
CLEAR = "#00000000"


def snap(v):
    """ce_draw::Snap: nearest multiple of 3."""
    q = (v + 1) // 3 if v >= 0 else -((-v + 1) // 3)
    return q * 3


cr_snap = snap


def picker_cell(n, i):
    """ce_page_skills::PickerCell."""
    if n <= 4:
        return 24 + i * 300, 288
    pitch = snap((1192 + 12) // n - 1)
    x0 = snap(24 + (1192 - (n * pitch - 12)) // 2)
    return x0 + i * pitch, pitch - 12


def skills_widgets():
    """Skills page (ce_skill_reader / ce_page_skills): Back, member picker and swipe, plan editing
    on the passive slots (drag a learned passive, or tap it then an empty slot; tap a planned slot to
    clear it). Plans are module-local: no game writes. Every gate is published only while the shown
    picture is a Skills picture and the native Skills menu is closed (read-only mirror)."""
    out = [{"type": "rect", "rect": [0, SK["SkBodyY"], W, SK["SkBodyH"]], "color": CLEAR,
            "on_swipe_left": "sk_next", "on_swipe_right": "sk_prev", "need_bind": "ce.sk.swipe"},
           {"id": "sk_back", "type": "rect", "rect": [SK["SkBackX"], SK["SkBackY"], SK["SkBackW"], SK["SkBackH"]],
            "color": CLEAR, "on_tap": "fpage_0", "need_bind": "ce.sk.back"}]
    for n in range(1, SK["SkMaxMembers"] + 1):
        for i in range(n):
            x, w = picker_cell(n, i)
            out.append({"id": f"sk_m{n}_{i}", "type": "rect", "rect": [x, SK["SkPickY"], w, SK["SkPickH"]],
                        "color": CLEAR, "on_tap": f"sk_member_{i}", "need_bind": f"ce.sk.m{n}_{i}"})
    slots = [(SK["SkSlotX"], SK["SkSlotY0"] + s * SK["SkSlotDy"], SK["SkSlotW"], SK["SkSlotH"], 0) for s in range(3)]
    slots += [(SK["SkClassX0"] + i * SK["SkClassDx"], SK["SkClassY"], SK["SkClassW"], SK["SkClassH"], 1) for i in range(2)]
    pad = SK["SkGlowPad"]
    for s, (x, y, w, h, kind) in enumerate(slots):
        # valid-drop glow while a spare passive is dragged or held selected (tap, then tap a slot)
        for g in ("d", "s"):
            out.append({"type": "image", "rect": [x - pad, y - pad, w + 2 * pad, h + 2 * pad],
                        "src": f"module:ce:skglow:{kind}", "need_bind": f"sk.glow{g}{s}"})
        out.append({"id": f"sk_slot{s}", "type": "rect", "rect": [x, y, w, h], "color": CLEAR,
                    "drop_action": f"sk_plan_{s}", "accept_group": "skp", "highlight_color": "#55FFAD1B",
                    "need_bind": f"ce.sk.e{s}"})
        out.append({"id": f"sk_plan{s}", "type": "rect", "rect": [x, y, w, h], "color": CLEAR,
                    "on_tap": f"sk_unplan_{s}", "need_bind": f"ce.sk.pl{s}"})
    # spare passives: layout A (full rows) / B (compact grid); the drag ghost is the row itself
    rows = [("a", i, SK["SkSlotX"], SK["SkFullY0"] + i * SK["SkFullDy"], SK["SkSlotW"], SK["SkSlotH"])
            for i in range(2)]
    rows += [("b", i, SK["SkGridX0"] + (i % 2) * SK["SkGridDx"], SK["SkGridY0"] + (i // 2) * SK["SkGridDy"],
              SK["SkGridW"], SK["SkGridH"]) for i in range(8)]
    for lay, i, x, y, w, h in rows:
        out.append({"id": f"sk_l{lay}{i}", "type": "image", "rect": [x, y, w, h], "bind": f"ce.sk.pid{i}",
                    "src_format": f"module:ce:skrow:{lay}:%d", "draggable": True, "drag_scale": 1.0,
                    "payload": f"$ce.sk.pid{i}", "select_group": "skp", "highlight_src": f"module:ce:sksel:{lay}",
                    "drag_under_src": f"module:ce:skshadow:{lay}", "drag_under_rect": [12, 18, 0, 0],
                    "need_bind": f"ce.sk.l{lay}{i}"})
    return out


def skills_derived():
    d = [{"name": "sk.selon", "cmp": "ge", "a": "@sel:skp", "b": 1000}]
    for s in range(5):
        d.append({"name": f"sk.glowd{s}", "all_nonzero": ["@drag", f"ce.sk.e{s}"]})
        d.append({"name": f"sk.glows{s}", "all_nonzero": ["sk.selon", f"ce.sk.e{s}"]})
    return d
def cr_picker_cell(n, i):
    """ce_page_crystals::PickerCell (the Skills page convention)."""
    if n <= 4:
        return 24 + i * 300, 288
    pitch = cr_snap((1192 + 12) // n - 1)
    x0 = cr_snap(24 + (1192 - (n * pitch - 12)) // 2)
    return x0 + i * pitch, pitch - 12


def crystals_widgets():
    """Crystals page. Every gate is published by the module only while a Crystals picture is shown;
    plan editing gates are withheld while the native Equipment menu / smith fusion is open (mirror)."""
    c = CR
    out = [{"type": "rect", "rect": [0, c["CrBodyY"], W, c["CrBodyH"]], "color": CLEAR,
            "on_swipe_left": "cr_next", "on_swipe_right": "cr_prev", "need_bind": "ce.cr.swipe"},
           {"id": "cr_back", "type": "rect", "rect": [c["CrBackX"], c["CrBtnY"], c["CrBtnW"], c["CrBtnH"]],
            "color": CLEAR, "on_tap": "cr_back", "need_bind": "ce.cr.back"}]
    for n in range(1, c["CrMaxMembers"] + 1):
        for i in range(n):
            x, w = cr_picker_cell(n, i)
            out.append({"id": f"cr_m{n}_{i}", "type": "rect", "rect": [x, c["CrPickY"], w, c["CrPickH"]],
                        "color": CLEAR, "on_tap": f"cr_member_{i}", "need_bind": f"ce.cr.m{n}_{i}"})
    for k in range(5):
        out.append({"id": f"cr_chip{k}", "type": "rect",
                    "rect": [c["CrChipX"] + k * c["CrChipDx"], c["CrChipY"], c["CrChipW"], c["CrChipW"]],
                    "color": CLEAR, "on_tap": f"cr_chip_{k}", "need_bind": "ce.cr.chip"})
    out += [{"id": "cr_up", "type": "rect", "rect": [c["CrScrollX"], c["CrScrollY"], c["CrScrollW"], c["CrScrollH"] // 2],
             "color": CLEAR, "on_tap": "cr_scroll_0", "need_bind": "ce.cr.su"},
            {"id": "cr_down", "type": "rect",
             "rect": [c["CrScrollX"], c["CrScrollY"] + c["CrScrollH"] // 2, c["CrScrollW"], c["CrScrollH"] // 2],
             "color": CLEAR, "on_tap": "cr_scroll_1", "need_bind": "ce.cr.sd"}]
    # slot rows (weapon 0-3, armor 4-7): tap = plan the selected crystal / clear a plan; drop = plan
    # the dragged one. Rows move with the equipment (y from the module).
    for k in range(8):
        out.append({"id": f"cr_slot{k}", "type": "rect", "rect": [c["CrRowX"], 3, c["CrRowW"], 60], "color": CLEAR,
                    "y_bind": f"ce.cr.sy{k}", "on_tap": f"cr_slot_{k}", "drop_action": f"cr_drop_{k}",
                    "highlight_color": "#66FFAD1B", "need_bind": f"ce.cr.s{k}"})
    for p in range(2):
        out.append({"id": f"cr_piece{p}", "type": "rect", "rect": [c["CrRowX"] - 6, -6, c["CrRowW"] + 12, 54],
                    "color": CLEAR, "y_bind": f"ce.cr.py{p}", "on_tap": f"cr_piece_{p}", "need_bind": f"ce.cr.pc{p}"})
    # the crystal grid: each cell is an image widget showing the module's cell picture, so a drag
    # lifts the cell itself (payload = grid cell), with a drop shadow under it
    for i in range(c["CrGridCols"] * c["CrGridRows"]):
        r, k = divmod(i, c["CrGridCols"])
        out.append({"id": f"cr_cell{i}", "type": "image",
                    "rect": [c["CrGridX"] + k * c["CrPitch"], c["CrGridY"] + r * c["CrPitch"], c["CrCell"], c["CrCell"]],
                    "bind": f"ce.cr.g{i}", "src_format": "module:ce:crc:%d", "draggable": True, "drag_scale": 1.0,
                    "payload": f"$ce.cr.p{i}", "on_tap": f"cr_sel_{i}", "drag_under_src": "module:ce:crshadow",
                    "drag_under_rect": [12, 24, 0, 0], "need_bind": f"ce.cr.gv{i}"})
    xs = [c["CrBtn0"], c["CrBtn1"], c["CrBtn2"]]
    for b in range(3):
        out.append({"id": f"cr_btn{b}", "type": "rect", "rect": [xs[b], c["CrBtnY"], c["CrBtnW"], c["CrBtnH"]],
                    "color": CLEAR, "on_tap": f"cr_btn_{b}", "need_bind": f"ce.cr.b{b}"})
    # filter picker / transfer targets: one-tap cells over the body
    for i in range(c["CrPickCols"] * c["CrPickRows"]):
        r, k = divmod(i, c["CrPickCols"])
        rect = [c["CrPickCellX"] + k * c["CrPickCellDx"], c["CrPickCellY"] + 36 + r * c["CrPickCellDy"],
                c["CrPickCell"], c["CrPickCell"]]
        if i < 12:
            out.append({"id": f"cr_f{i}", "type": "rect", "rect": rect, "color": CLEAR, "on_tap": f"cr_filt_{i}",
                        "need_bind": f"ce.cr.f{i}"})
        out.append({"id": f"cr_t{i}", "type": "rect", "rect": rect, "color": CLEAR, "on_tap": f"cr_tgt_{i}",
                    "need_bind": f"ce.cr.t{i}"})
    return out


def crystals_actions(actions):
    c = CR
    actions["cr_back"] = {"kind": "module", "action": "cr_back", "argument": 0}
    actions["cr_next"] = {"kind": "module", "action": "cr_next", "argument": 0}
    actions["cr_prev"] = {"kind": "module", "action": "cr_prev", "argument": 0}
    for i in range(c["CrMaxMembers"]):
        actions[f"cr_member_{i}"] = {"kind": "module", "action": "cr_member", "argument": i}
    for k in range(5):
        actions[f"cr_chip_{k}"] = {"kind": "module", "action": "cr_chip", "argument": k}
    for k in range(2):
        actions[f"cr_scroll_{k}"] = {"kind": "module", "action": "cr_scroll", "argument": k}
    for k in range(8):
        actions[f"cr_slot_{k}"] = {"kind": "module", "action": "cr_slot", "argument": k}
        actions[f"cr_drop_{k}"] = {"kind": "module", "action": f"cr_drop{k}", "argument": "$payload"}
    for p in range(2):
        actions[f"cr_piece_{p}"] = {"kind": "module", "action": "cr_piece", "argument": p}
    for i in range(c["CrGridCols"] * c["CrGridRows"]):
        actions[f"cr_sel_{i}"] = {"kind": "module", "action": "cr_sel", "argument": i}
    for b in range(3):
        actions[f"cr_btn_{b}"] = {"kind": "module", "action": "cr_btn", "argument": b}
    for i in range(12):
        actions[f"cr_filt_{i}"] = {"kind": "module", "action": "cr_filt", "argument": i}
    for i in range(c["CrPickCols"] * c["CrPickRows"]):
        actions[f"cr_tgt_{i}"] = {"kind": "module", "action": "cr_tgt", "argument": i}


def crystals_nav():
    c = CR
    return ([f"cr_m{n}_{i}" for n in range(1, c["CrMaxMembers"] + 1) for i in range(n)] +
            [f"cr_chip{k}" for k in range(5)] + [f"cr_cell{i}" for i in range(c["CrGridCols"] * c["CrGridRows"])] +
            ["cr_up", "cr_down"] + [f"cr_piece{p}" for p in range(2)] + [f"cr_slot{k}" for k in range(8)] +
            [f"cr_f{i}" for i in range(12)] + [f"cr_t{i}" for i in range(c["CrPickCols"] * c["CrPickRows"])] +
            [f"cr_btn{b}" for b in range(3)] + ["cr_back"])

main = [
    {"type": "rect", "rect": [0, 0, W, H], "color": CLEAR, "on_hold": "open_settings", "hold_ms": 700},
    # prefetch: the wanted picture decodes here (covered by the shown one)
    {"type": "image", "rect": [0, 0, 4, 4], "src_bind": "ce.next"},
    # the picture; a page change (Home <-> Crystals / Skills / Formation) drops ce.pagev for 6 ticks
    # (100 ms) so the old page fades out and the new one in (UX-SPEC 3.8, like the settings fade)
    {"type": "image", "rect": [0, 0, W, H], "src_bind": "ce.page", "need_bind": "ce.pagev",
     "anim": {"bind": "ce.pagev", "group": "pagefade", "from": "fade", "ms": 100, "easing": "ease_out"}},
    # cross-fade (UX-SPEC 3.8): the previous picture laid over the new one at full opacity, then
    # faded out (anim gate closes, ~120 ms): actor change in battle, gear / pilot change in Sky Armor,
    # a plan step ticking on Crystals / Skills. The old picture is still in the image cache.
    {"type": "image", "rect": [0, 0, W, H], "src_bind": "ce.xf.src", "need_bind": "ce.xf.need",
     "anim": {"bind": "ce.xf.a", "group": "xfade", "from": "fade", "ms": 120, "easing": "ease_out"}},
    # turn strip: when a turn passes the new strip (a strip-sized picture) slides in from the right
    # over the old one (~200 ms); one widget per strip position (band shown / Ultra row only /
    # no band / Sky Armor), its own anim group so each clips to its rect
    {"type": "image", "rect": [0, 0, 4, 4], "src_bind": "ce.strip.next"},
    *[{"type": "image", "rect": [G["StripX"], G["StripY"] + dy, G["StripW"], G["StripH"]], "src_bind": "ce.strip.src",
       "need_bind": f"ce.strip.v{k}",
       "anim": {"bind": "ce.strip.a", "group": f"strip{k}", "from": "right", "ms": 200, "easing": "ease_out"}}
      for k, dy in enumerate((0, -G["BandUltraLift"], -G["BandFullLift"], -18))],
    # battle motion overlays (module-driven positions; UX-SPEC 3.8): Overdrive tick + marker glide,
    # one 150 ms flash on a zone change, the native hand cursor bobbing beside the selected chip,
    # and one pulse on the Ultra button when it becomes ready
    {"type": "rect", "rect": [0, 135, 3, 24], "bg": "#FFFFFFFF", "color": CLEAR, "x_bind": "ce.odt.x",
     "need_bind": "ce.odm"},
    {"type": "image", "rect": [0, 150, 45, 45], "src": "module:ce:sprite/systemgfx/marker@3", "x_bind": "ce.odm.x",
     "need_bind": "ce.odm"},
    {"type": "rect", "rect": [24, 135, 1192, 24], "bg": "#88B6F5D2", "color": CLEAR, "need_bind": "ce.od.flash",
     "anim": {"bind": "ce.od.flash", "group": "odflash", "from": "fade", "ms": 75}},
    {"type": "rect", "rect": [24, 135, 1192, 24], "bg": "#99FF5A3C", "color": CLEAR, "need_bind": "ce.od.flashh",
     "anim": {"bind": "ce.od.flashh", "group": "odflashh", "from": "fade", "ms": 75}},
    *[{"type": "image", "rect": [0, 0, 4, 4], "src": f"module:ce:sprite/systemgfx/handCursor_{f}@3"} for f in range(5)],
    {"type": "image", "rect": [0, 0, 69, 54], "bind": "ce.cur.f", "src_format": "module:ce:sprite/systemgfx/handCursor_%d@3",
     "x_bind": "ce.cur.x", "y_bind": "ce.cur.y", "need_bind": "ce.cur"},
    {"type": "rect", "rect": [G["UltraX"], G["UltraY"], G["UltraW"], G["UltraH"]], "bg": "#66FFAD1B", "color": CLEAR,
     "need_bind": "ce.ultra.pulse", "anim": {"bind": "ce.ultra.pulse", "group": "ultrapulse", "from": "fade", "ms": 150}},
    # Field Home: the live player marker over the map picture (moves without a new picture)
    *[{"type": "image", "rect": [0, 0, 4, 4], "src": f"module:ce:fmark:{d}"} for d in range(5)],
    {"type": "image", "rect": [0, 0, F["FMarkerBox"], F["FMarkerBox"]], "bind": "ce.fmd",
     "src_format": "module:ce:fmark:%d", "x_bind": "ce.fmx", "y_bind": "ce.fmy", "need_bind": "ce.fmark"},
    # loading: the last view stays, dimmed 60 %, waitingCircle 6x (UX-SPEC 4.9)
    {"type": "rect", "rect": [0, 0, W, H], "bg": "#99050609", "color": CLEAR, "need_bind": "ce.loading",
     "input_block": True},
    {"type": "image", "rect": [1120, 960, 60, 60], "src": "module:ce:sprite/systemgfx/waitingCircle@6",
     "need_bind": "ce.loading"},
    # battle: tap the target card (bestiary unlocked) to open / close the full sheet. The module
    # publishes the gate of the card geometry on screen (ce_pages::HitGeometry of the shown picture):
    # width (early / linked card, sheet) x height (band shown / Ultra row only / prologue: the
    # height also fixes the top: the card bottom stays at CardY + its base height).
    *[{"type": "rect", "rect": [G["CardX"], G["CardY"] + base - hh, ww, hh], "color": CLEAR,
       "on_tap": "sheet", "need_bind": f"ce.tap.c{ww}_{hh}"}
      for ww, base in ((G["CardW"], G["CardH"]), (G["LinkedCardW"], G["LinkedCardH"]),
                       (G["LinkedCardWideW"], G["LinkedCardH"]),
                       (G["SheetW"], G["CardH"]), (G["SheetW"], G["LinkedCardH"]))
      for hh in (base, base + G["BandUltraLift"], base + G["BandFullLift"])],
    # enemy chips (alive enemies, in battle order): select into the target card (local only); each
    # cell sits where the shown picture drew it (ce.chipx<i> / ce.chipy)
    {"id": "chip{i}", "type": "rect", "rect": [0, 0, G["ChipW"], G["ChipH"]], "color": CLEAR,
     "x_bind": "ce.chipx{i}", "y_bind": "ce.chipy",
     "on_tap": "sel_chip_{i}", "need_bind": "ce.chip{i}", "repeat": G["MaxChips"], "repeat_dx": 0},
    # Ultra Move button (drawn by the module in the band): one ZR press, present only while the
    # game would take ZR (bar full, a party member's command window open, no target selection)
    {"id": "ultra", "type": "rect", "rect": [G["UltraX"], G["UltraY"], G["UltraW"], G["UltraH"]],
     "color": CLEAR, "on_tap": "ultra_zr", "need_bind": "ce.ultra.ok"},
    # Sky Armor battle: tap-twice gear shift. Only the gear one R press reaches is armable
    # (ce.sky.arm<g>); the armed row then presses the game's own R (ce.sky.fire<g>, re-checked as
    # the button action's enabled_bind). contracts/skyarmor-readers.md "Gear shift action".
    *[{"id": f"gear_arm{g}", "type": "rect",
       "rect": [G["GearRowX"], G["GearRowY0"] + g * G["GearRowDy"], G["GearRowW"], G["GearRowH"]],
       "color": CLEAR, "on_tap": f"gear_arm_{g}", "need_bind": f"ce.sky.arm{g}"} for g in range(3)],
    *[{"id": f"gear_fire{g}", "type": "rect",
       "rect": [G["GearRowX"], G["GearRowY0"] + g * G["GearRowDy"], G["GearRowW"], G["GearRowH"]],
       "color": CLEAR, "on_tap": f"gear_fire_{g}", "need_bind": f"ce.sky.fire{g}"} for g in range(3)],
    # Field Home: three buttons (Crystals / Skills / Board) and the status pane (-> Board)
    {"id": "f_crystals", "type": "rect", "rect": [F["FBtn0"], F["FBtnY"], F["FBtnW"], F["FBtnH"]],
     "color": CLEAR, "on_tap": "fpage_1", "need_bind": "ce.fhome"},
    {"id": "f_skills", "type": "rect", "rect": [F["FBtn1"], F["FBtnY"], F["FBtnW"], F["FBtnH"]],
     "color": CLEAR, "on_tap": "fpage_2", "need_bind": "ce.fhome"},
    {"id": "f_board", "type": "rect", "rect": [F["FBtn2"], F["FBtnY"], F["FBtnW"], F["FBtnH"]],
     "color": CLEAR, "on_tap": "fpage_3", "need_bind": "ce.fhome"},
    {"type": "rect", "rect": [F["FPaneX"], F["FPaneY"], F["FPaneW"], F["FPaneH"]],
     "color": CLEAR, "on_tap": "fpage_3", "need_bind": "ce.fhome"},
    # field sub-pages: Back bottom-left; the Board's tiles (7 x 8 window) and section arrows
    {"id": "f_back", "type": "rect", "rect": [F["FBtn0"], F["FBtnY"], F["FBtnW"], F["FBtnH"]],
     "color": CLEAR, "on_tap": "fpage_0", "need_bind": "ce.fsub"},
    {"id": "b_tile{i}", "type": "rect", "rect": [F["BTileX0"], F["BTileY0"], F["BTile"], F["BTile"]],
     "color": CLEAR, "on_tap": "btile_{i}", "need_bind": "ce.bt{i}", "repeat": F["BCols"] * F["BRows"],
     "repeat_dx": F["BTile"], "repeat_cols": F["BCols"], "repeat_row_dy": F["BTile"]},
    {"id": "b_prev", "type": "rect", "rect": [0, 0, 520, 108], "color": CLEAR, "on_tap": "bsec_0",
     "need_bind": "ce.bprev"},
    {"id": "b_next", "type": "rect", "rect": [720, 0, 520, 108], "color": CLEAR, "on_tap": "bsec_1",
     "need_bind": "ce.bnext"},
    # Skills page (sub-page of Field Home)
    *skills_widgets(),
    # game over: the battle picture stays, dimmed, without actions, until the game leaves the
    # wipe-out / Game Over screen (ce_reader state 6)
    {"type": "rect", "rect": [0, 0, W, H], "bg": "#99050609", "color": CLEAR, "need_bind": "ce.gameover",
     "input_block": True},
    # Crystals page (sub-page of Field Home; native Equipment / smith fusion mirror)
    *crystals_widgets(),
    # outside battles: the settings button drawn in the holding picture
    {"id": "cog", "type": "rect",
     "rect": [G["SettingsBtnX"], G["SettingsBtnY"], G["SettingsBtnW"], G["SettingsBtnH"]],
     "color": CLEAR, "on_tap": "open_settings", "need_bind": "ce.cog"},
    # no module instance (another game version or a load error): host font notice
    {"type": "rect", "rect": [0, 0, W, H], "bg": "#FF211C20", "color": CLEAR, "need_bind": "!module_ready",
     "input_block": True},
    {"type": "label", "rect": [W // 2, 420, 0, 90], "text": "UNSUPPORTED VERSION", "align": "center",
     "text_scale": 6, "color": "#FFFFAD1B", "need_bind": "!module_ready"},
    {"type": "label", "rect": [W // 2, 540, 0, 50], "text": "This companion needs Chained Echoes 1.41.",
     "align": "center", "text_scale": 4, "color": "#FFFFF8E3", "need_bind": "!module_ready"},
    {"type": "label", "rect": [W // 2, 610, 0, 50], "text": "The game still runs; the companion stays off.",
     "align": "center", "text_scale": 3, "color": "#FFC4B795", "need_bind": "!module_ready"},
]

# Settings: (row label, flag, module action)
HUD = [("ctb", "hud.ctb", "hud_ctb"), ("party", "hud.party", "hud_party"), ("od", "hud.od", "hud_od"),
       ("ultra", "hud.ultra", "hud_ultra"), ("gear", "hud.gear", "hud_gear")]
# settings:<k> picture bits (native/modules/0100C510166F0000.cpp): od 1, party 2, ctb 4, ultra 8, gear 16
SET_BIT = {"hud.od": 1, "hud.party": 2, "hud.ctb": 4, "hud.ultra": 8, "hud.gear": 16}


def set_expr(flip=None):
    """set.k (or set.k with one switch flipped): the settings picture for the current switches."""
    return " + ".join(f"(@flag:{f} ? {0 if f == flip else b} : {b if f == flip else 0})"
                      for f, b in sorted(SET_BIT.items(), key=lambda kv: -kv[1]))


# Preload only the pictures one tap away (each switch flipped): five full-canvas pictures plus the
# shown one stay well inside the runtime's 64 MiB module-image budget. Preloading all 32 (or the
# earlier 16) combinations overflowed it, so the LRU evicted the shown picture while the others
# landed and the page stayed blank on its first open (merged-candidate regression, 2026-10-09).
settings = [{"type": "image", "rect": [0, 0, 4, 4], "bind": f"set.n{i}",
             "src_format": "module:ce:settings:%d"} for i in range(len(HUD))]
settings.append({"type": "image", "rect": [0, 0, W, H], "bind": "set.k",
                 "src_format": "module:ce:settings:%d"})
nav = []
for i, (key, flag, _) in enumerate(HUD):
    y = G["SetRowY0"] + i * G["SetRowDy"]
    for label, x, value in (("show", G["SetShowX"], 0), ("hide", G["SetHideX"], 1)):
        wid = f"set_{key}_{label}"
        settings.append({"id": wid, "type": "rect", "rect": [x, y, G["SetBtnW"], G["SetBtnH"]],
                         "color": CLEAR, "on_tap": f"{key}_{label}"})
        nav.append(wid)
settings.append({"id": "set_back", "type": "rect", "rect": [G["BackX"], G["BackY"], G["BackW"], G["BackH"]],
                 "color": CLEAR, "on_tap": "close_settings"})
nav.append("set_back")

actions = {"open_settings": {"kind": "page", "page": "@settings", "transition": "fade", "duration_ms": 200},
           "close_settings": {"kind": "page", "page": "main", "transition": "fade", "duration_ms": 200},
           "sheet": {"kind": "module", "action": "sheet", "argument": 0},
           "ultra_zr": {"kind": "button", "button": "ZR", "frames": 6, "enabled_bind": "ce.ultra.ok", "haptic": "heavy"}}
for i in range(G["MaxChips"]):
    actions[f"sel_chip_{i}"] = {"kind": "module", "action": "sel_chip", "argument": i}
for g in range(3):
    actions[f"gear_arm_{g}"] = {"kind": "module", "action": "gear_arm", "argument": g, "haptic": "confirm"}
    actions[f"gear_fire_{g}"] = {"kind": "button", "button": "R", "frames": 4, "enabled_bind": f"ce.sky.fire{g}",
                                 "haptic": "heavy"}
for i in range(4):
    actions[f"fpage_{i}"] = {"kind": "module", "action": "fpage", "argument": i}
for i in range(F["BCols"] * F["BRows"]):
    actions[f"btile_{i}"] = {"kind": "module", "action": "btile", "argument": i}
for i in range(2):
    actions[f"bsec_{i}"] = {"kind": "module", "action": "bsec", "argument": i}
for i in range(SK["SkMaxMembers"]):
    actions[f"sk_member_{i}"] = {"kind": "module", "action": "sk_member", "argument": i}
actions["sk_next"] = {"kind": "module", "action": "sk_next", "argument": 0}
actions["sk_prev"] = {"kind": "module", "action": "sk_prev", "argument": 0}
for s_ in range(5):
    actions[f"sk_plan_{s_}"] = {"kind": "module", "action": f"sk_plan{s_}", "argument": "$payload"}
    actions[f"sk_unplan_{s_}"] = {"kind": "module", "action": f"sk_unplan{s_}", "argument": 0}
crystals_actions(actions)
enforce = []
for key, flag, act in HUD:
    actions[f"{key}_show"] = {"kind": "flag", "flag": flag, "value": 0}
    actions[f"{key}_hide"] = {"kind": "flag", "flag": flag, "value": 1}
    # a module action's argument must be a constant: one hide and one show action per switch,
    # re-sent while the flag holds that value
    actions[f"{act}_on"] = {"kind": "module", "action": act, "argument": 1}
    actions[f"{act}_off"] = {"kind": "module", "action": act, "argument": 0}
    enforce += [{"action": f"{act}_on", "every_ms": 500, "flag": flag, "value": True},
                {"action": f"{act}_off", "every_ms": 500, "flag": flag, "value": False}]

manifest = {
    "format": 1,
    "title_id": "0100C510166F0000",
    "name": "Chained Echoes dual screen",
    "min_runtime": 18,
    "requires_module": True,
    "canvas_w": W,
    "canvas_h": H,
    "background": "#FF211C20",
    "_note": "Generated by tools/chained-echoes/gen_manifest.py. Pictures and the CE font are decoded "
             "by the module from the player's romfs.",
    "font": "file:ce_font.txt",
    "font_atlas": "module:ce:font/CE",
    # the package owns @settings, so its flags declare their own defaults and persistence
    "flags": {flag: 1 for _, flag, _ in HUD},
    "persist_flags": [flag for _, flag, _ in HUD],
    "derived": [{"name": "set.k", "expr": set_expr()}] +
               [{"name": f"set.n{i}", "expr": set_expr(flag)} for i, (_, flag, _) in enumerate(HUD)] +
               skills_derived(),
    "nav": {"color": "#FFFFAD1B", "frame": 5},
    # UX-SPEC 3.8 haptics: light tick on taps / selections / drags, a confirm on a drop, a double
    # short on a refused action or drop, heavy on a fired hold (settings); per-action overrides below
    "haptics": {"enabled": True, "respect_system": True, "tap": "light", "select": "light", "drag": "light",
                "drop": "confirm", "refused": "reject", "hold": "heavy", "nav": "light"},
    "actions": actions,
    "enforce": enforce,
    "pages": [
        {"id": "main", "nav_order": ["cog", "ultra"] + [f"chip{i}" for i in range(G["MaxChips"])] +
         [f"gear_{k}{g}" for g in range(3) for k in ("arm", "fire")] +
            ["f_crystals", "f_skills", "f_board", "b_prev", "b_next", "b_tile{i}", "f_back"] +
            [f"sk_m{n}_{i}" for n in range(1, SK["SkMaxMembers"] + 1) for i in range(n)] +
            [f"sk_l{lay}{i}" for lay, k in (("a", 2), ("b", 8)) for i in range(k)] +
            [f"sk_slot{s_}" for s_ in range(5)] + [f"sk_plan{s_}" for s_ in range(5)] + ["sk_back"] + crystals_nav(),
         "widgets": main},
        {"id": "@settings", "nav_order": nav, "widgets": settings},
    ],
}
out = ROOT / "packages/ChainedEchoes/dualscreen/manifest.json"
out.write_text(json.dumps(manifest, indent=1) + "\n")
(out.parent / "ce_font.txt").write_text("CE\n")
print("wrote", out)
