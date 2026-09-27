#!/usr/bin/env python3
"""Bottom screen = ONE page: the game's pause/status header across the top and the MAP filling the
rest below it; while no map is available (title screen, the moment after an area switch) the page
shows a replica of the game's own loading screen instead.

Header (geometry measured off a 1920x1080 top-screen capture of the game,
scaled x0.8 for the 1240 px canvas): the AREA NAME plate (the game's areanames_*.bctex, one per
area, picked by `area_index`), the grey-teal line whose outer flats sit high and whose centre flat
dips, joined by DECO_DIAGONAL sprites (left one mirrored) with the short dashes under the flats;
beneath it, small readouts pushed out to the sides: energy-fragment icon + one tank bar per energy
tank (purple while that tank still holds energy, blank once drained) + "cur/max" hugging the last
tank on the left, "ITEMS xx%" under the centre flat, missile icon + "cur / max" on the right.
No panel box. Everything the game draws from its own sprite sheets is addressed by UV into the
game's own textures (sprites_czdr-rwk.bmsss / sprites_loadingscreentileset.bmsss), never copied.

Run: python3 dread_hud_page.py [package dualscreen dir]   (default: packages/MetroidDread/dualscreen)
"""
import json
import sys
from pathlib import Path

PKG = Path(sys.argv[1]) if len(sys.argv) > 1 else \
    Path(__file__).resolve().parents[2] / "packages" / "MetroidDread" / "dualscreen"
CANVAS_W, CANVAS_H = 1240, 1080
PANEL_BACKGROUND = "#FF0B0F14"
# The blue belongs to room geometry; empty map space uses the neutral panel background.
MAP_BACKGROUND = PANEL_BACKGROUND

CZ = "romfs:/textures/gui/textures/czdr-rwk.bctex"  # the menu/status-screen sheet (1024x512)
# UVs from gui/scripts/sprites_czdr-rwk.bmsss
UV_DECO_DIAGONAL = [0.375977, 0.007812, 0.419922, 0.203125]
# energy fragment icon, 0..3 quarters lit then FULL (matched against the decoded czdr-rwk sheet)
UV_ENERGYFRAGMENTS = [
    [0.037109375, 0.30078125, 0.0751953125, 0.345703125],   # _0
    [0.076171875, 0.30078125, 0.1142578125, 0.345703125],   # _1
    [0.115234375, 0.30078125, 0.1533203125, 0.345703125],   # _2
    [0.154296875, 0.30078125, 0.1923828125, 0.345703125],   # _3
    [0.1943359375, 0.30078125, 0.232421875, 0.345703125],   # FULL
]
UV_ENERGYTANK_FULL = [0.1552734375, 0.556640625, 0.1943359375, 0.66015625]
UV_ENERGYTANK_BLANK = [0.19921875, 0.552734375, 0.240234375, 0.6640625]
UV_MISSILEICON_MENU = [0.006836, 0.556641, 0.053711, 0.648438]
UV_PBICON_MENU = [0.058594, 0.556641, 0.101562, 0.640625]

LOADING = "romfs:/textures/gui/textures/loadingscreentileset.bctex"  # 1024x256
# UVs from gui/scripts/sprites_loadingscreentileset.bmsss (tiles 192-197 on the sprite grid)
UV_LOAD_A = [0.916016, 0.0, 0.999023, 0.332031]          # 85x85 glyph tiles
UV_LOAD_H = [0.833008, 0.0, 0.916016, 0.332031]
UV_LOAD_C = [0.75, 0.0, 0.833008, 0.332031]
UV_WHEEL_IN = [0.0, 0.0, 0.146484, 0.585938]             # 150x150 concentric wheel rings
UV_WHEEL_MID = [0.146484, 0.0, 0.292969, 0.585938]
UV_WHEEL_OUT = [0.292969, 0.0, 0.439453, 0.585938]
# MARKER_REMOVE (sprite 049 on the czdr-rwk grid): the red X, 86x85 at (538,260) of 1024x512
UV_MARKER_REMOVE = [0.525390625, 0.5078125, 0.609375, 0.673828125]
MISMATCH_RED = "#FFEF0031"                # the X's own red (239,0,49), sampled off the sprite

# Area id -> the game's name plate texture (areanames_<name>.bctex, 1024x128, text in rows 15..109).
AREA_PLATE = {
    "s010_cave": "artaria", "s020_magma": "cataris", "s030_baselab": "dairon",
    "s040_aqua": "burenia", "s050_forest": "ghavoran", "s060_quarantine": "elun",
    "s070_basesanc": "ferenia", "s080_shipyard": "hanubia", "s090_skybase": "itorash",
}
PLATE_SRC_RECT = [0.0, 15 / 128, 1.0, 109 / 128]

NUMCOL = "#FFFFFFFF"      # readout digits (white, as on the top screen)
LINE = "#FF849293"        # the header line, sampled off the top screen: (132,146,147)


def main():
    w = []
    CX = CANVAS_W // 2
    PY, PH = 16, 156

    # ---------------- header (visible once the map is ready) ----------------
    # Area name plate where the status screen says "SAMUS": one image per area, the runtime's
    # `area_index` (areas sorted by id, -1 until the map is ready) picks which one shows.
    for i, area in enumerate(sorted(AREA_PLATE)):
        w.append({"type": "image", "rect": [CX - 240, PY, 480, 44],
                  "src": f"romfs:/textures/gui/textures/areanames_{AREA_PLATE[area]}.bctex",
                  "src_rect": PLATE_SRC_RECT, "hide_bind": "area_index", "keep_min": i, "keep_max": i})
    LY, TH, DIP, JW = PY + 50, 5, 22, 46      # line top, thickness, centre dip, join width
    L0, L1 = CX - 260, CX - 164               # left flat
    C0, C1 = CX - 118, CX + 118               # centre (lower) flat
    R0, R1 = CX + 164, CX + 260               # right flat
    w.append({"type": "rect", "rect": [L0, LY, L1 - L0, TH], "bg": LINE, "color": "#00000000"})
    w.append({"type": "rect", "rect": [R0, LY, R1 - R0, TH], "bg": LINE, "color": "#00000000"})
    w.append({"type": "rect", "rect": [C0, LY + DIP, C1 - C0, TH], "bg": LINE, "color": "#00000000"})
    # the joins: DECO_DIAGONAL squashed to the shallow slope is a hairline, so stack four copies
    # one px apart to bring it up to the flats' thickness (still one crisp edge). The sprite's
    # stroke is inset ~9% from its box edges, so the box overhangs each flat by 5 px.
    JO = 5
    for dy in range(4):
        w.append({"type": "image", "rect": [L1 - JO, LY + dy, JW + 2 * JO, DIP + 2], "color": LINE,
                  "src": CZ, "src_rect": UV_DECO_DIAGONAL, "flip_x": True})   # backslash join
        w.append({"type": "image", "rect": [R0 - JW - JO, LY + dy, JW + 2 * JO, DIP + 2],
                  "color": LINE, "src": CZ, "src_rect": UV_DECO_DIAGONAL})    # slash join
    # the short dashes tucked under the outer flats next to each join
    w.append({"type": "rect", "rect": [L1 - 27, LY + 10, 24, 3], "bg": LINE, "color": "#00000000"})
    w.append({"type": "rect", "rect": [R0 + 3, LY + 10, 24, 3], "bg": LINE, "color": "#00000000"})

    # --- the readouts: the game's SAMUS-screen header (gui/scripts/samusmenucomposition.bmscp,
    # Content.STATS-ENERGYFRAGMENTS / Stats-deco-EnergyTank / LifeLabel / MissileIcon / BombIcon /
    # MissileLabel / BombLabel), mapped with this header's own transform: X = CX + 1536 (u - 0.5),
    # Y = 864 v - 27.8 (LineL is 0.0625 x 1920 wide in the GUI, 96 px here). The game draws ONE
    # static tank and "%i/%i" (main+0xB6D9FC) left-aligned after it -- never a row of tanks -- so
    # the energy block has a fixed width whatever the tank count. Digits: digital_small, 13 px at
    # 720p -> scale 4. The fragment icon follows the game's state (module int energy_frag_state:
    # 0..4 = SamusEnergyFragments_<n>, 5 = _FULL once ITEM_TOTAL_LIFE_SHARDS == 16; _4 and _FULL
    # are tinted #B4F0F8 by the skin).
    TINT = "#FFB4F0F8"
    for k, (uv, tint) in enumerate([(UV_ENERGYFRAGMENTS[0], None), (UV_ENERGYFRAGMENTS[1], None),
                                    (UV_ENERGYFRAGMENTS[2], None), (UV_ENERGYFRAGMENTS[3], None),
                                    (UV_ENERGYFRAGMENTS[0], TINT), (UV_ENERGYFRAGMENTS[4], TINT)]):
        f = {"type": "image", "rect": [CX - 256, 96, 35, 22], "src": CZ, "src_rect": uv}
        if tint:
            f["color"] = tint
        f.update({"hide_bind": "energy_frag_state", "keep_min": k, "keep_max": k})
        w.append(f)
    w.append({"type": "image", "rect": [CX - 219, 89, 29, 38], "src": CZ,
              "src_rect": UV_ENERGYTANK_FULL})
    w.append({"type": "value", "rect": [CX - 188, 99, 0, 0], "bind": "energy",
              "max_bind": "energy_max", "max_sep": "/", "text_scale": 4, "color": NUMCOL})
    # --- ITEMS (the map screen's Items label: digital_medium -> scale 6, colour #B4F0F8). On the
    # game's own Items row it would sit on LifeLabel (the two screens share this header, not the
    # row), so it takes the power-bomb row, centred, on BombLabel's baseline.
    w.append({"type": "value", "rect": [CX, 134, 0, 0], "bind": "items_pct", "text": "ITEMS ",
              "suffix": "%", "text_scale": 6, "align": "center", "color": TINT})
    # --- missiles, and power bombs on the row below once acquired (label hidden below 1 max) ---
    w.append({"type": "image", "rect": [CX + 127, 82, 46, 45], "src": CZ, "src_rect": UV_MISSILEICON_MENU})
    w.append({"type": "value", "rect": [CX + 177, 99, 0, 0], "bind": "missile",
              "max_bind": "missile_max", "max_sep": " / ", "text_scale": 4, "color": NUMCOL})
    w.append({"type": "image", "rect": [CX + 128, 133, 43, 42], "src": CZ,
              "src_rect": UV_PBICON_MENU, "hide_bind": "pbomb_max", "keep_min": 1})
    w.append({"type": "value", "rect": [CX + 177, 144, 0, 0], "bind": "pbomb",
              "max_bind": "pbomb_max", "max_sep": " / ", "text_scale": 4, "color": NUMCOL,
              "hide_bind": "pbomb_max", "keep_min": 1})
    # every header widget waits for the map (a second gate beside each one's own hide rule)
    for wd in w:
        wd["need_bind"] = "map_ready"

    # ---------------- the map fills the rest of the screen, BELOW the header ----------------
    w.append({"type": "map", "rect": [16, PY + PH + 12, CANVAS_W - 32, CANVAS_H - (PY + PH + 12) - 16],
              "area": "s010_cave", "color": "#FF7FD4E8", "bg": MAP_BACKGROUND, "text_scale": 4,
              "area_label": False,
              "hidden_icons": ["PropWarlotus"],
              "room_bind": "scenario", "id": "dread_map", "pan_zoom": True,
              "min_zoom": 0.25, "max_zoom": 6.0, "marker_x_bind": "player_x",
              "marker_y_bind": "player_y", "marker_icon": "Samus", "follow_window": 6000.0,
              "actor_x_bind": "emmi_x", "actor_y_bind": "emmi_y", "actor_icon": "Emmy",
              "actor_reveal_required": True,
              "need_bind": "map_ready"})

    # A pill-shaped RESET VIEW button in the map's lower-right corner: taps glide the map back
    # to the follow view (zoom 1, no pan) instead of snapping.
    w.append({"type": "button", "rect": [CANVAS_W - 16 - 196, CANVAS_H - 16 - 20 - 56, 196, 56],
              "text": "RESET VIEW", "text_scale": 4, "align": "center", "pill": True,
              "bg": "#C00B0F14", "color": "#FF7FD4E8", "on_tap": "map_reset",
              "id": "map_reset_btn", "need_bind": "map_ready",
              "hide_bind": "view_custom:dread_map", "hide_eq": 0})

    # ---------------- pause-menu companion: mission log + abilities (menu_open == 1) ----------------
    # While the game's own Start menu is up, the map area shows the mission log (newest first,
    # the game's own text) on the left and Samus's abilities on the right, acquired ones bright.
    MY = PY + PH + 12
    mp = [{"type": "rect", "rect": [0, MY, CANVAS_W, CANVAS_H - MY], "bg": PANEL_BACKGROUND,
           "color": "#00000000"}]
    mp.append({"type": "label", "rect": [40, MY + 16, 0, 0], "text": "MISSION LOG", "text_scale": 6,
               "color": "#FF7FD4E8"})
    mp.append({"type": "rect", "rect": [40, MY + 56, 560, 3], "bg": LINE, "color": "#00000000"})
    for i in range(10):
        mp.append({"type": "label", "rect": [40, MY + 76 + i * 60, 0, 0], "bind_text": f"mlog_{i}",
                   "text_scale": 4, "color": NUMCOL if i == 0 else "#FFB9C4CC",
                   "hide_bind": "mlog_count", "keep_min": i + 1})
    mp.append({"type": "label", "rect": [660, MY + 16, 0, 0], "text": "SAMUS", "text_scale": 6,
               "color": "#FF7FD4E8"})
    mp.append({"type": "rect", "rect": [660, MY + 56, 540, 3], "bg": LINE, "color": "#00000000"})
    # The game's own SAMUS menu sections (GUI_SAMUSMENU_SECTION_* / _NAME_*); base gear the
    # player always has is drawn without a flag.
    SECTIONS = [
        ("BEAM", [(None, "POWER BEAM"), ("ITEM_WEAPON_WIDE_BEAM", "WIDE BEAM"),
                  ("ITEM_WEAPON_PLASMA_BEAM", "PLASMA BEAM"), ("ITEM_WEAPON_WAVE_BEAM", "WAVE BEAM"),
                  ("ITEM_WEAPON_HYPER_BEAM", "HYPER BEAM"), ("ITEM_WEAPON_CHARGE_BEAM", "CHARGE BEAM"),
                  ("ITEM_WEAPON_DIFFUSION_BEAM", "DIFFUSION BEAM"),
                  ("ITEM_WEAPON_GRAPPLE_BEAM", "GRAPPLE BEAM")]),
        ("MISSILE", [(None, "MISSILE"), ("ITEM_WEAPON_SUPER_MISSILE", "SUPER MISSILE"),
                     ("ITEM_WEAPON_ICE_MISSILE", "ICE MISSILE"), ("ITEM_MULTILOCKON", "STORM MISSILE")]),
        ("AEION", [("ITEM_OPTIC_CAMOUFLAGE", "PHANTOM CLOAK"), ("ITEM_GHOST_AURA", "FLASH SHIFT"),
                   ("ITEM_SONAR", "PULSE RADAR")]),
        ("SUIT", [(None, "POWER SUIT"), ("ITEM_VARIA_SUIT", "VARIA SUIT"),
                  ("ITEM_GRAVITY_SUIT", "GRAVITY SUIT"), ("ITEM_HYPER_SUIT", "METROID SUIT")]),
        ("MORPH BALL", [("ITEM_MORPH_BALL", "MORPH BALL")]),
        ("BOMB", [("ITEM_WEAPON_BOMB", "BOMB"), ("ITEM_WEAPON_LINE_BOMB", "CROSS BOMB"),
                  ("ITEM_WEAPON_POWER_BOMB", "POWER BOMB")]),
        ("MISC.", [("ITEM_MAGNET_GLOVE", "SPIDER MAGNET"), ("ITEM_SPEED_BOOSTER", "SPEED BOOSTER"),
                   ("ITEM_DOUBLE_JUMP", "SPIN BOOST"), ("ITEM_SPACE_JUMP", "SPACE JUMP"),
                   ("ITEM_SCREW_ATTACK", "SCREW ATTACK")]),
    ]
    ROW, HEAD = 30, 44
    by_name = dict(SECTIONS)
    columns = [[(n, by_name[n]) for n in ("BEAM", "MISSILE", "BOMB")],
               [(n, by_name[n]) for n in ("AEION", "SUIT", "MORPH BALL", "MISC.")]]
    for ci, col in enumerate(columns):
        px, py = 660 + ci * 280, MY + 72
        for title, entries in col:
            mp.append({"type": "label", "rect": [px, py, 0, 0], "text": title, "text_scale": 4,
                       "color": "#FF7FD4E8"})
            mp.append({"type": "rect", "rect": [px, py + 24, 240, 2], "bg": LINE, "color": "#00000000"})
            py += HEAD
            for item, name in entries:
                if item is None:
                    mp.append({"type": "label", "rect": [px + 12, py, 0, 0], "text": name,
                               "text_scale": 4, "color": NUMCOL})
                else:
                    mp.append({"type": "label", "rect": [px + 12, py, 0, 0], "text": name,
                               "text_scale": 4, "color": NUMCOL, "hide_bind": f"pw_{item}",
                               "keep_min": 1})
                    mp.append({"type": "label", "rect": [px + 12, py, 0, 0], "text": name,
                               "text_scale": 4, "color": "#FF3A4650", "hide_bind": f"pw_{item}",
                               "keep_max": 0})
                py += ROW
            py += 14
    for wd in mp:
        wd["need_bind"] = "menu_open"
    w += mp

    # ---------------- chase warning: a red frame breathing around the whole panel ----------------
    # Shown only while the runtime reports a live EMMI at full alert (`emmi_chase`); the zone rooms
    # and the EMMI marker pulse on the map itself (runtime), this is the panel-wide alarm.
    # a faint translucent red wash over the whole panel (peak alpha ~11 %), plus the frame
    # Both take the game's own zone-closed colour, vEmmyZoneClosedColor #950000 (CMinimapManager
    # tunable +0xC0, what the area map paints a chased zone with); the wash keeps its thin alpha.
    w.append({"type": "rect", "rect": [0, 0, CANVAS_W, CANVAS_H], "bg": "#1C950000",
              "color": "#00000000", "pulse": True, "need_bind": "emmi_chase", "id": "chase_wash"})
    w.append({"type": "rect", "rect": [0, 0, CANVAS_W, CANVAS_H], "bg": "#00000000",
              "color": "#FF950000", "frame": 18, "pulse": True, "need_bind": "emmi_chase",
              "id": "chase_frame"})

    # ---------------- loading screen while map_ready == 0 ----------------
    # Black, the three concentric wheel rings turning at the centre (inner and outer clockwise,
    # middle the other way), "Synchronizing environment..." beneath, the three glyph tiles in a
    # row under that -- the game's own loading screen pieces (loadingscreentileset.bctex).
    ld = []
    ld.append({"type": "rect", "rect": [0, 0, CANVAS_W, CANVAS_H], "bg": "#FF000000",
               "color": "#00000000"})
    CY = CANVAS_H // 2 - 60
    WS = 240                                  # wheel diameter on the panel (150 px sprite x1.6)
    LOADCOL = "#FF7FD4E8"                     # the game's map cyan-blue, not white
    for uv, spin in ((UV_WHEEL_OUT, 30.0), (UV_WHEEL_MID, -45.0), (UV_WHEEL_IN, 60.0)):
        ld.append({"type": "image", "rect": [CX - WS // 2, CY - WS // 2, WS, WS], "src": LOADING,
                   "src_rect": uv, "spin": spin, "color": LOADCOL})
    ld.append({"type": "label", "rect": [CX, CY + WS // 2 + 36, 0, 0],
               "text": "Synchronizing environment...", "text_scale": 5, "align": "center",
               "color": LOADCOL})
    GS, GAP = 64, 24                          # glyph tile size and spacing
    gx = CX - (3 * GS + 2 * GAP) // 2
    for i, uv in enumerate((UV_LOAD_A, UV_LOAD_H, UV_LOAD_C)):
        ld.append({"type": "image", "rect": [gx + i * (GS + GAP), CY + WS // 2 + 84, GS, GS],
                   "src": LOADING, "src_rect": uv, "color": LOADCOL})
    for wd in ld:
        wd["hide_bind"] = "map_ready"
        wd["hide_eq"] = 1
        wd["need_bind"] = "build_match"    # only for the build the package knows (2.1.0)
    w = ld + w

    # ---------------- version warning while build_match == 0 ----------------
    # The package carries offsets for one executable (data file 646761F643AFEBB3.json = 2.1.0);
    # on any other build nothing can be read, so instead of the wheel the loading page shows the
    # game's own red X (MARKER_REMOVE) trembling in place, the warning text and the three glyph
    # tiles in the X's red.
    vm = []
    vm.append({"type": "rect", "rect": [0, 0, CANVAS_W, CANVAS_H], "bg": "#FF000000",
               "color": "#00000000"})
    XS = 200                                  # the X on the panel (86 px sprite x2.3)
    vm.append({"type": "image", "rect": [CX - XS // 2, CY - XS // 2, XS, XS], "src": CZ,
               "src_rect": UV_MARKER_REMOVE, "shake": 3, "color": "#FFFFFFFF"})
    vm.append({"type": "label", "rect": [CX, CY + WS // 2 + 36, 0, 0],
               "text": "Protocol Mismatch Detected", "text_scale": 5, "align": "center",
               "color": MISMATCH_RED})
    vm.append({"type": "label", "rect": [CX, CY + WS // 2 + 36 + 44, 0, 0],
               "text": "Use Patch v2.1.0 to Proceed.", "text_scale": 4, "align": "center",
               "color": MISMATCH_RED})
    for i, uv in enumerate((UV_LOAD_A, UV_LOAD_H, UV_LOAD_C)):
        vm.append({"type": "image", "rect": [gx + i * (GS + GAP), CY + WS // 2 + 84 + 44, GS, GS],
                   "src": LOADING, "src_rect": uv, "color": MISMATCH_RED})
    for wd in vm:
        wd["hide_bind"] = "build_match"
        wd["hide_eq"] = 1
    w = vm + w

    man = json.load(open(PKG / "manifest.json"))
    man["background"] = PANEL_BACKGROUND
    man["pages"] = [{"id": "main", "widgets": w}]
    man.setdefault("actions", {})["map_reset"] = {"kind": "view_reset", "view": "dread_map"}
    # In-game signal: the package's "scenario" sequence (Game.GetScenarioID) returns "" on the
    # title / file select and the area id during play; the runtime gates map_ready on it. Poll it
    # every 2 s so the map yields to the loading page soon after a return to the main menu.
    man["sequences"]["scenario"]["every_ms"] = 2000
    man["sequences"].pop("ingame", None)
    # User-selected softer presentation across the map and its overlays; HUD stays opaque.
    # 0.7 is a visual starting point, not a measured native minimap alpha.
    man["map"]["style"]["opacity"] = 0.7
    # Cell shading as the game's minimap mesh builder does it (main+0xE95194): a seen cell is
    # drawn at 0.4x (0x3ECCCCCD), a walked cell at 1.0x. visited_gain 420 is the tuned value the
    # 1.0.9+ packages ship (walked rooms read clearly brighter over the per-state visited tint).
    man["map"]["style"]["revealed_dim"] = 102
    man["map"]["style"]["visited_gain"] = 420
    # Room fill = vNormalRoomColor #001330 (the compositing shader's g_uNormalRoomClr).
    man["map"]["style"]["room_fill"] = "#FF001330"
    # The map-water update clamps baked geometry to live bounds before rasterization.
    # Preserve the requested seen/walked brightness distinction on the water layer.
    man["map"]["style"]["water_clip_live"] = True
    man["map"]["style"]["water_full_visible"] = False
    # The game's cell reveal fades in over 1.0 of its clock (**(main+0x1CBA0E8)+0x2C), which
    # advances 1.0 per second (read live: +1.00 per 60 ticks) -> 60 ticks at poll_hz 60.
    man["map"]["style"]["fade_ticks"] = 60
    if not str(man["map"].get("atlas", "")).startswith("file:"):
        # Asset-free package (1.0.0+): labels that are game strings bind to the game's own
        # localization (the same conversion dread_af/dread_af_manifest.py applies).
        sys.path.insert(0, str(Path(__file__).resolve().parent / "dread_af"))
        from dread_af_manifest import bind_loc_labels
        bind_loc_labels(man)
    with open(PKG / "manifest.json", "w") as f:
        json.dump(man, f, indent=1)
        f.write("\n")
    print(f"single main page: {len(w)} widgets (loading screen + header {PH} px + map below)")


if __name__ == "__main__":
    main()
