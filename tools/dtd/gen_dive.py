#!/usr/bin/env python3
"""Generate the Dave the Diver companion package (packages/DaveTheDiver), v0.1.x: Dive page.

Run from the repo root:
    python3 tools/dtd/gen_dive.py
    python3 tools/build_dualscreen_package.py --package packages/DaveTheDiver --output dist \
        --version 0.1.0 --min-runtime 17

Chains verified on device (AYN Thor, Eden Duo 1.1.0, NCE), game v1.0.6.1267 (build F74B633E480E4E16):
  IL2CPP v31 Il2CppClass.parent = +0x58, static_fields = +0xB8 (explicit: the runtime's
  static_fields auto-detection picks the wrong offset on this Unity version).
  weight/max, O2 and depth matched the game HUD.
Unverified (shown on the DEBUG page until checked): O2 warning thresholds, max O2, depth limit,
overload threshold.
"""
import copy, json, re
from pathlib import Path

PKG = Path(__file__).resolve().parents[2] / "packages" / "DaveTheDiver"
ROOT = PKG / "dualscreen"
import sys as _sys
_sys.path.insert(0, str(Path(__file__).resolve().parent))
# Every picture on the bottom screen is the game's own, read from the player's game files by the
# module (module:icon/, module:frame/ via pictures.idx); the package ships no pictures at all.
RED_RGB, GREY_RGB = (200, 30, 58), (111, 133, 151)

# The game's own 9-slice UI frames (read live from the player's game files, like the pictures;
# listed in pictures.idx). slice = {left, top, right, bottom} source px from Sprite.m_Border.
# Eden draws 9-slice corners at their own size, so x2 frames are composed at half size and
# scaled up 2x from the top-left corner (crisp, nearest sampling).
GAME_FRAMES = {
    "UI_EdgeFrame_Stroke": [17, 17, 17, 17],   # the corner-cut outline (white: tinted)
    "UI_Inven_TitleFrame": [17, 18, 17, 17],   # the inventory's yellow frame (tabs, top buttons)
    # 0.38.34: the dive HUD's weapon slots (InGameAtlas): slanted boxes + their outlines; the sides (the slant) kept,
    # the middle stretched to the card's width
    "UI_WP_Dagger_Box": [22, 10, 22, 10], "UI_WP_Dagger_Stroke": [21, 9, 21, 9],
    "UI_WP_Item_Box": [22, 10, 22, 10], "UI_WP_Item_Stroke": [21, 9, 21, 9],
    "UI_WP_Main_Box": [22, 10, 22, 10], "UI_WP_Main_Stroke": [21, 9, 21, 9],
    "UI_Sushi_Pop_Panel":  [30, 30, 30, 30],   # the sushi bar's dark wooden panel
    "UI_Btn_Orange":       [19, 18, 19, 20],   # the game's orange action button
    "UI_Btn_Inactive":     [19, 18, 19, 20],   # ... and its grey one
    "UI_Sushi_Recipe_Menu_box": [10, 10, 10, 10],   # the game's recipe / ingredient cell
    "UI_Sushi_item_Bg":    [10, 10, 10, 10],   # the dark square behind a picture in those cells
    "UI_EdgeFrame":        [17, 17, 17, 17],   # the corner-cut shape, filled (white: tinted) -- card backgrounds
    "UI_btn_pink":         [8, 0, 8, 0],       # the game's pink OK button
    "UI_Sushi_Amount_Box": [6, 6, 6, 6],       # the number box between the amount arrows
    "UI_Sushi_Btn_Pink64":  [31, 0, 31, 0],    # the sushi bar's pink OK button (a pill)
    "UI_Sushi_Btn_Black64": [31, 0, 31, 0],    # ... and its dark one (Close, or nothing to do)
    "UI_Sushi_Menu_PopAmountBg": [10, 10, 10, 10],          # the dark number box of the amount picker
    "UI_SushiBar_Storage_PopFocusLine": [10, 10, 10, 10],   # its outline (white: tinted orange)
    # the brush stroke behind a dish's name on the menu (white: tinted pink). Its border marks the
    # whole width as ends (77 + 171 = 248), so a 9-slice leaves nothing to stretch and the middle
    # goes missing: it is stretched whole instead.
    "UI_Sushi_MenuTitleBg": None,
    "UI_Sushi_SpecialMission_GaugeOut": [20, 0, 20, 0],   # a gauge's dark track (depth, cargo)
    "UI_Sushi_FarmGauge":  [10, 0, 10, 0],     # ... and its fill (white: tinted)
    "UI_O2_Gauge_re":      None,               # the HUD's O2 disc (white: tinted; revealed bottom-up)
    "UI_O2_Stroke_re":     None,               # ... its ring
    "UI_O2_DAlarm_re":     None,               # ... the glow behind it when air is low
    "UI_btn_brown":        [8, 0, 8, 0],       # the game's brown Close button
    # 0.38.12: the village map's frame (154x70, the pale paper border with its line; slices guessed from the size)
    "UI_Map_DetailMap_FrmBg": [24, 24, 24, 24],
    "UI_Map_Scroll_Deco":  None,               # 80x936: the scroll's roller at each end
    # 0.37.63: the phone's plain shapes (white: tinted), as the game builds its phone apps from them
    "Roundsquare_24":      [24, 22, 24, 22],   # 80x48: a calculator key
    "Roundsquare_24_Stroke": [24, 22, 24, 22], # ... its outline
    "Box_30rad":           [30, 30, 30, 30],   # 66x66: the phone's body
    "Box_4rad":            [8, 8, 8, 8],       # 24x24: the display
    "Box_8rad":            [12, 12, 12, 12],   # 32x32
    # 0.37.74: the game's EcoWatcher pictures (phone atlas + its own bundle); slices guessed from the sizes
    "EcoWatcher_bg":       None,               # 604x1078: the app's background, stretched
    "EcoWatcher_RankBox":  [24, 24, 24, 24],   # 550x170: the blue research card
    "EcoWatcher_RankBoxDeco": None,            # 194x170: the faint emblem on its right
    "EcoWatcher_RankGaugeOut": [10, 0, 10, 0], # 30x20: the card's gauge track
    "EcoWatcher_CellBox":  [12, 12, 12, 12],   # 32x32: a mission cell
    "EcoWatcher_Cell_Focus": [12, 12, 12, 12], # 32x32: the cursor's outline
    # 0.37.94: Duff's Weapon Shop (phone atlas; PhoneAtlas_Point for the camo); slices guessed from the sizes
    "UI_WeaponCraft_Duff_BG": None,            # 150x87: the camo, tiled
    "UI_WeaponCraft_Logo": None,               # 604x72: the pink "Duff's Weapon Shop" title
    # 0.37.104: iDiver (phone atlas): the row bar and its outline
    "iDiver_ListBar":      [12, 12, 12, 12],   # 170x40
    "iDiver_ListBar_Stroke": [12, 12, 12, 12], # 170x40
    "UI_WeaponListBox":    [24, 96, 24, 20],   # 0.37.98: the top keeps its outlined picture box; only the plain rows under it stretch   # 264x168: a weapon card (developed)
    "UI_WeaponListBox02":  [24, 20, 24, 20],   # ... the blueprint one (undeveloped)
    "UI_WeaponListBox_Select": [24, 96, 24, 20],
    "UI_WeaponListBox_Select.02": [24, 20, 24, 20],
    "UI_WeaponListDesignBox": [16, 16, 16, 16],# 164x60: the picture box with the blueprint lines
    "UI_WeaponCraft_Cell_Btn_Blue": [14, 14, 14, 14],   # 40x40: the card's strip / buttons
    "UI_WeaponCraft_Cell_Btn_Pink": [14, 14, 14, 14],
    "UI_WeaponCraft_Cell_Btn_Grey": [14, 14, 14, 14],
    "UI_WeaponCraft_Cell_Btn_Green": [14, 14, 14, 14],
    "Weapon_Check":        None,               # 22x17: a check mark
    "UI_btn_gray":         [8, 0, 8, 0],       # ... and its grey one (Delete Menu, Swap)   # the brush stroke behind a dish's name on the menu (white: tinted pink)
}
# the game's "Acquired" panel (pause menu): blue body, lighter header band, thin cyan edge
ACQ_BODY, ACQ_HEAD, ACQ_EDGE = "#FF123E6E", "#FF2B78BC", "#FF5FD3FF"
SDIM, SDIM_E = "#FFE6DCD2", "#FFB8AEA6"     # secondary text on the light sushi cells (full / empty)
def game_frame(r, name, x2=True, tint=None, **kw):
    x, y, w, h = (int(v) for v in r)
    # module:frame/ = the picture at its own shape (module:icon/ would centre it on a square)
    out = {"type": "image", "src": f"module:frame/{name}", "color": "#FFFFFFFF", **kw}
    if GAME_FRAMES[name]:
        out.update({"fill": "slice", "slice": GAME_FRAMES[name]})
    if tint:
        out["tint"] = tint
    if x2:
        out.update({"rect": [x, y, w // 2, h // 2], "scale_bind": "k_x2", "pivot": [0, 0]})
    else:
        out["rect"] = [x, y, w, h]
    return out
RED_C, GREY_C, BLUE_C = "#FFC81E3A", "#FF8FA3B4", "#FF3C8FE3"
# Text size: everything up to scale 5 is drawn TEXT_BUMP steps bigger (the big numbers stay as they
# are). 1 until 0.24; 2 from 0.25 (the game font, and larger text for the small screen).
TEXT_BUMP = 2
def drawn(sc): return sc + TEXT_BUMP if sc <= 5 else sc
BTN_SHADE = "#C0401018"                      # the dark edge around white button text (as in the game)
def game_button(r, text, action, kind="orange", gate=None, sc=4, sub=None, tint=None, col=None):
    """The game's button picture with our text; the transparent rect on top takes the tap.
    ok = the pink (A) OK, brown = (B) Close, grey2 = Delete Menu / Swap, the rest the dive prompts' buttons."""
    g = {"need_bind": gate} if gate else {}
    name = {"orange": "UI_Btn_Orange", "ok": "UI_btn_pink", "brown": "UI_btn_brown", "grey2": "UI_btn_gray"}.get(kind, "UI_Btn_Inactive")
    white = kind in ("ok", "brown")
    col = col or ("#FFFFFFFF" if white else "#FF2E2A28" if kind == "grey2" else INK)
    sh = {"outline": BTN_SHADE, "outline_px": 2} if white else {}
    s1 = drawn(sc)                                # the size it is drawn at
    if sub:                                       # title and a smaller line under it, centred together
        sc = sc - 1; s1 = drawn(sc)
        ty = r[1] + (r[3] - (5 * s1 + 8 + 5 * drawn(2))) // 2
    else:
        ty = r[1] + (r[3] - 5 * s1) // 2
    fr = game_frame(r, name, x2=r[3] >= 80, tint=tint, **g)
    out = [fr, {"type": "rect", "rect": list(r), "bg": "#00000000", "color": "#00000000", "frame": 0,
                "on_tap": action, "noframe": True, **g}]
    out.append(label(r[0] + r[2] // 2, ty, text, col, sc, align="center", **sh, **g))
    if sub:
        out.append(label(r[0] + r[2] // 2, ty + 5 * s1 + 8, sub, col, 2, align="center", **sh, **g))
    return out
AMOUNT_ORANGE = "#FFFF9F2E"
def stepper(x, y, bind, dec, inc, gate):
    """The game's amount picker (Add Ingredients): a grey arrow either side of a dark number box
    with an orange outline (UI_SushiBar_Storage_Arrow, UI_Sushi_Menu_PopAmountBg, ..._PopFocusLine)."""
    g = {"need_bind": gate}
    tap = {"type": "rect", "bg": "#00000000", "color": "#00000000", "frame": 0, "noframe": True, **g}
    return [game_frame([x + 120, y, 160, 76], "UI_Sushi_Menu_PopAmountBg", x2=False, **g),
            game_frame([x + 120, y, 160, 76], "UI_SushiBar_Storage_PopFocusLine", tint=AMOUNT_ORANGE, **g),
            value([x + 200, y + 16, 0, 0], bind, TXT, 5, align="center", **g),
            {"type": "image", "rect": [x + 44, y + 2, 56, 72], "src": "module:frame/UI_SushiBar_Storage_Arrow",
             "color": "#FFFFFFFF", "flip_x": True, **g},
            {"type": "image", "rect": [x + 300, y + 2, 56, 72], "src": "module:frame/UI_SushiBar_Storage_Arrow",
             "color": "#FFFFFFFF", **g},
            {**tap, "rect": [x, y, 116, 76], "on_tap": dec}, {**tap, "rect": [x + 284, y, 116, 76], "on_tap": inc}]
# The DEBUG button on DIVE (raw values, discard test, diagnostics). The pages are still built;
# set this to True and rebuild to bring the button back (it then sits over the watch).
SHOW_DEBUG = False
SHOW_DIAG = False      # beta (0.38.0): the DIAGNOSTICS page and the map's DIAG button are off; True again for probe rounds
WCHEST = "#FFFF5A4A"   # weapon chests: the chest picture tinted red
MARKERS = 160          # chest markers on the map (the module's kMpKeep)
MISSION_MARKS = 16   # loose mission items (module kMm)
AMMO_MARKS, POD_MARKS = 24, 24   # 0.37.22 (module kAm, kPd)
FISH_MARKS = 32      # fish (module kFm): live fish, then spawn spots
BUILD16 = "F74B633E480E4E16"
VERSION = "1.0.0"
BEI_ICON = "Bei32"    # 0.38.54: the green Bei coin (index run)
# 0.37.36: the escape pod picture (Pod_ex is the pod hanging on its cables; rows 244..400 of 400 are the pod)
POD_SRC = [0.0, 0.61, 1.0, 1.0]

LB, IG = "main+0x9DB1EA8", "main+0x9DB0400"     # LootBox_TypeInfo, InGameManager_TypeInfo
PARENT, STATICS = "0x58", "0xB8"
LBI = [LB, PARENT, STATICS, "0x0"]              # -> &SingletonNoMono<LootBox>.s_Instance
IGI = [IG, PARENT, STATICS, "0x0"]              # -> &Singleton<InGameManager> instance
BH = IGI + ["0xA0", "0x298"]                    # -> &PlayerCharacter.m_BreathHandler
DH = BH + ["0x28"]                              # -> &PlayerBreathHandler.m_CharacterDepth

points = {}
def pt(name, typ, chain, note):
    points[name] = {"type": typ, "ptr": chain, "offset": "0x0", "_v": note}

# verified
pt("bh", "u64", BH, "PlayerBreathHandler* (non-zero while the diver exists)")
pt("o2", "f32", BH + ["0xA0"], "PlayerBreathHandler.m_HP = O2 [verified: HUD 85 -> 85.0]")
pt("depth", "f32", DH + ["0x38"], "CharacterDepthHandler.m_PlayerDepth [verified: 9.3 m]")
pt("weight", "f32", LBI + ["0x2C"], "LootBox.<weight>k__BackingField [verified: 0.0]")
pt("weight_max", "f32", LBI + ["0x18"], "LootBox.m_WeightMax [verified: 9.0]")
# unverified, shown on DEBUG
pt("o2_warn", "f32", BH + ["0x74"], "PlayerBreathHandler.m_WarningOxygen")
pt("o2_urgent", "f32", BH + ["0x78"], "PlayerBreathHandler.m_UrgentWarningOxygen")
pt("o2_maxparam", "f32", BH + ["0xA4"], "PlayerBreathHandler.m_MaxHPParameter")
pt("o2_custommax", "f32", BH + ["0xBC"], "PlayerBreathHandler.m_CustomMaxHP")
pt("depth_max", "f32", DH + ["0x48"], "CharacterDepthHandler.m_MaxDepth")
pt("depth_warn", "f32", DH + ["0x50"], "CharacterDepthHandler.m_WarnLine")
pt("depth_danger", "f32", DH + ["0x54"], "CharacterDepthHandler.m_DangerLine")
pt("overload", "f32", LBI + ["0x40"], "LootBox.<overloadedThreshold>k__BackingField")


# ---- Tasks (0.3.1): MissionHUD.s_Current -> in-progress slots -> UIDataText -> EzText.m_text ----
# MissionHUD is non-generic: statics on its own class (+0xB8), s_Current @ +0x0 [verified: class name].
# Slots: Dictionary<int, MissionHUDSlot> @ +0x80, entries @ +0x18, Entry stride 0x18, value @ +0x10,
# array data @ +0x20 [verified: two MissionHUDSlot entries for two tasks].
# Text: UIDataText._elements (List @ +0x48) -> _items @ +0x10 -> [0] @ +0x20 -> Element._ezText @ +0x18
#       -> EzText._textTMProUGUI @ +0x30 -> TMP_Text.m_text @ +0xD8 (backup: m_TextBackingArray @ +0x6C8, UTF-32).  Element._textKey @ +0x10 is the localisation key (fallback/debug).
HUD = ["main+0x9DB2820", STATICS, "0x0"]          # -> &MissionHUD.s_Current
TXT_HOPS = ["0x48", "0x10", "0x20"]               # UIDataText -> &first Element
pt("hud_cls", "cstring", HUD + ["0x0", "0x10"], "class name of s_Current (expect MissionHUD)")
def slot(i):   # -> &entries[i].value
    return HUD + ["0x80", "0x18", hex(0x30 + i * 0x18)]
MAX_TASKS, MAX_COND = 3, 6   # 0.37.8: up to 6 rows per mission (the module's kMConds)
for i in range(MAX_TASKS):
    pt(f"t{i}", "string", slot(i) + ["0x50"] + TXT_HOPS + ["0x18", "0x30", "0xD8"], "task title text")
    pt(f"tk{i}", "string", slot(i) + ["0x50"] + TXT_HOPS + ["0x10"], "task title key")
    pt(f"tb{i}", "utf32", slot(i) + ["0x50"] + TXT_HOPS + ["0x18", "0x30", "0x6C8", "0x20"], "backing array (UTF-32)")
    pt(f"tcls{i}", "cstring", slot(i) + ["0x50"] + TXT_HOPS + ["0x18", "0x0", "0x10"], "class of _ezText (expect EzText)")
    pt(f"tmcls{i}", "cstring", slot(i) + ["0x50"] + TXT_HOPS + ["0x18", "0x30", "0x0", "0x10"], "expect TextMeshProUGUI")
    pt(f"cn{i}", "s32", slot(i) + ["0x58", "0x18"], "conditions List._size")
    cond = slot(i) + ["0x58", "0x10", {"offset": "0x20", "stride": 8, "index": "$i"}]
    points[f"c{i}_"] = {"type": "string", "count": MAX_COND, "offset": "0x0", "_v": "condition text",
                        "ptr": cond + ["0x38"] + TXT_HOPS + ["0x18", "0x30", "0xD8"]}
    points[f"cd{i}_"] = {"type": "u64", "count": MAX_COND, "offset": "0x0", "_v": "m_MissionConditionData (row in use)",
                         "ptr": cond + ["0x90"]}
    points[f"clen{i}_"] = {"type": "s32", "count": MAX_COND, "offset": "0x0", "_v": "condition text length (Il2CppString +0x10)",
                           "ptr": cond + ["0x38"] + TXT_HOPS + ["0x18", "0x30", "0xD8", "0x10"]}
    # MissionConditionData (round6): Target 0x20, NowCount 0x28, MaxCount 0x2C, IsShowHUD 0x30, IsShowCount 0x31 (bools; read as one s32)
    for key, off, note in [("cnow", "0x28", "NowCount"), ("cmax", "0x2C", "MaxCount"),
                           ("ctgt", "0x20", "Target"), ("cflags", "0x30", "IsShowHUD | IsShowCount<<8 | IsNewClear<<16")]:
        points[f"{key}{i}_"] = {"type": "s32", "count": MAX_COND, "offset": "0x0", "_v": note,
                                "ptr": cond + ["0x90", off]}
    points[f"n{i}_"] = {"type": "string", "count": MAX_COND, "offset": "0x0", "_v": "condition count text",
                        "ptr": cond + ["0x40"] + TXT_HOPS + ["0x18", "0x30", "0xD8"]}

def ex(name, expr, **kw):
    return {"name": name, "expr": expr, **kw}
derived = [
    # row 0 always shown when the mission exists; extra rows only when they carry condition data (pooled rows)
    *[ex(f"cv{i}_{j}", f"cn{i} > {j}" + (f" && cd{i}_{j} != 0" if j else "")) for i in range(MAX_TASKS) for j in range(MAX_COND)],
    *[ex(f"mv{i}", f"cn{i} > 0") for i in range(MAX_TASKS)],      # mission slot in use
    # completed = count reached; rows with no count (e.g. "Make 100 Gold") never get a tick from this
    *[ex(f"cdone{i}_{j}", f"cv{i}_{j} && cmax{i}_{j} > 0 && cnow{i}_{j} >= cmax{i}_{j}") for i in range(MAX_TASKS) for j in range(MAX_COND)],
    *[ex(f"copen{i}_{j}", f"cv{i}_{j} && !(cmax{i}_{j} > 0 && cnow{i}_{j} >= cmax{i}_{j})") for i in range(MAX_TASKS) for j in range(MAX_COND)],
    # count text only where the game shows it (IsShowCount = byte 1 of the flags word)
    *[ex(f"ccnt{i}_{j}", f"cv{i}_{j} && floor(cflags{i}_{j} / 256) % 256 != 0") for i in range(MAX_TASKS) for j in range(MAX_COND)],
    *[ex(f"cnto{i}_{j}", f"ccnt{i}_{j} && copen{i}_{j}") for i in range(MAX_TASKS) for j in range(MAX_COND)],
    *[ex(f"cntd{i}_{j}", f"ccnt{i}_{j} && cdone{i}_{j}") for i in range(MAX_TASKS) for j in range(MAX_COND)],
    *[ex(f"cdnc{i}_{j}", f"cdone{i}_{j} && !ccnt{i}_{j}") for i in range(MAX_TASKS) for j in range(MAX_COND)],
    {"name": "in_dive", "all_nonzero": ["bh"]},
    ex("o2_i", "o2", round=True),
    ex("o2_x10", "o2*10", round=True),
    # tank size from the module (PlayerBreathHandler.get_MaxHP, round 10); 100 until it is read
    ex("o2_cap", "o2.ok && o2.max_x10 > 0 ? o2.max_x10 / 10 : max(100, o2)"),
    ex("o2_known", "in_dive && o2.ok && o2.max_x10 > 0"), ex("o2_unknown", "in_dive && !(o2.ok && o2.max_x10 > 0) && o2_state == 0"),
    ex("o2_cap_i", "o2_cap", round=True),
    ex("o2_pct", "clamp(o2 * 100 / max(1, o2_cap), 0, 100)", round=True),
    ex("o2_warn_i", "o2_warn", round=True), ex("o2_urgent_i", "o2_urgent", round=True),
    ex("o2_max_x10", "max(1000, o2*10)", round=True),           # provisional until max O2 is known
    ex("o2_state", "o2 <= o2_urgent ? 2 : (o2 <= o2_warn ? 1 : 0)"),
    ex("o2_alert", "in_dive && o2_state > 0"), ex("o2_calm", "in_dive && o2_state == 0"),
    ex("o2_tank", "o2_known && o2_state == 0"), ex("o2_alarm", "in_dive && o2_state > 0"),
    ex("o2_ok", "in_dive && o2_state == 0"), ex("o2_low", "in_dive && o2_state == 1"), ex("o2_crit", "in_dive && o2_state == 2"),
    ex("zero", "0"), ex("one", "1"), ex("k_x2", "2000"),
    ex("depth_r", "round(max(depth, 0)*10)"), ex("depth_i", "floor(depth_r/10)"), ex("depth_d", "depth_r % 10"),  # round like the HUD (24.08 -> 24.1); 0.37.55: never below 0 (just above the surface read "-1.-5m")
    ex("depth_x10", "depth*10", round=True), ex("depth_max_i", "depth_max", round=True),
    ex("depth_max_x10", "max(1, depth_max*10)", round=True),
    ex("w_r", "round(weight*10)"), ex("w_i", "floor(w_r/10)"), ex("w_d", "w_r % 10"),
    ex("wm_r", "round(weight_max*10)"), ex("wm_i", "floor(wm_r/10)"), ex("wm_d", "wm_r % 10"),
    ex("w_x10", "weight*10", round=True), ex("wm_x10", "max(1, weight_max*10)", round=True),
    {"name": "overweight", "cmp": "gt", "a": "weight", "b": "weight_max"},
    ex("deep", "in_dive && !scene.village && depth > depth_max && depth_max > 0"), ex("shallow", "in_dive && !scene.village && !(depth > depth_max && depth_max > 0)"),
    ex("dnum", "in_dive && !scene.village"), ex("dvq", "in_dive && scene.village"),   # 0.38.5: depth ??? in the village
    ex("cargo_ok", "in_dive && !(weight > weight_max)"),
    ex("cargo_over", "in_dive && weight > weight_max"),
    # debug readouts x100
    *[ex(n + "_x100", n + "*100", round=True) for n in
      ["o2_warn", "o2_urgent", "o2_maxparam", "o2_custommax", "depth_max", "depth_warn", "depth_danger", "overload"]],
]

BG, CARD, DIM, TXT, ACC = "#FF071521", "#FF0E2436", "#FF6F8597", "#FFE8F1F7", "#FF5CD6FF"
CARD_DIM = "#FF0A1A28"   # an empty ingredient box
GOLD_W = "#FFFFD54A"
PINK = "#FFFF5C8A"   # the game's sushi-portion colour
OK, WARN, CRIT, OVER = "#FF34C3F0", "#FFFF4D5E", "#FFC81E3A", "#FFFF4D5E"   # low = red like the game

def rect(r, bg, **kw):  return {"type": "rect", "rect": r, "bg": bg, "color": "#00000000", **kw}
def label(x, y, t, c=DIM, s=3, **kw): return {"type": "label", "rect": [x, y, 0, 0], "text": t, "color": c, "text_scale": s, **kw}
def value(r, b, c=TXT, s=4, **kw): return {"type": "value", "rect": r, "bind": b, "color": c, "text_scale": s, **kw}
def bar(r, b, mx, c, **kw):
    """A gauge as the game draws one: its dark track, and its fill tinted `c` growing from the left."""
    x, y, w, h = r
    track = game_frame([x, y - 3, w, h + 6], "UI_Sushi_SpecialMission_GaugeOut", x2=False, **kw)
    fill = {"type": "bar", "rect": [x + 4, y + 2, w - 8, h - 4], "bind": b, "max_bind": mx, "color": "#00000000", "bg": "#00000000",
            "frame": 0, "image": "module:frame/UI_Sushi_FarmGauge", "fill": "slice", "slice": [10, 0, 10, 0], "tint": c, **kw}
    return [track, fill]
D = {"need_bind": "in_dive"}
def tlabel(x, y, key, c=TXT, sc=3, **kw):
    return {"type": "label", "rect": [x, y, 0, 0], "bind_text": key, "color": c, "text_scale": sc, **kw}
GOLD = "#FFFFD54A"

# ---- page chrome: Dave in the header, the tabs along the bottom (styled after the game's UI).
# The pictures (Dave, the O2 bottle, the cargo box, a sushi, the mission mark) are the game's own,
# read from the player's game files by the module (ui.ic_*); the package ships none of them.
INK = "#FF071521"                                        # dark text on a lit tab / button
TAB_OFF_BG, TAB_OFF_EDGE, TAB_OFF_TXT = "#FF0E2436", "#FF2C5A8A", "#FF9FB6CC"
TOP = 100                                                # content starts here ...
BOTTOM = 922                                             # ... and ends here (tabs below)
TAB_Y, TAB_H = 934, 120
# 0.37.0: two tabs -- DIVE (its MAP / CARGO / MISSIONS along the top) and SUSHI
TABS = [("dive", "DIVE", "to_dive", "ui.ic_dave"),   # 0.37.118: Dave (was the mask)
        ("sushi", "SUSHI", "to_sushi", "ui.ic_sushi"),
        # 0.37.62: the phone's apps (the calculator first); 0.37.63: its picture is the game's Telephone app icon
        ("phone", "APPS", "to_phone", "module:icon/UI_MainMenu_Phone")]   # 0.37.114: APPS (was PHONE); 0.37.115: the HUD's little phone

def tabbar(active):
    n = len(TABS)
    out, tw = [], (1196 - (n - 1) * 10) // n     # 0.37.10: 22 .. 1218 -- the same margin both sides (FILTER flush right)
    for k, (key, title, act, ic) in enumerate(TABS):
        x, on = 22 + k * (tw + 10), key == active
        out += [# the game's inventory frame (yellow); our fill inside it (lit = cyan)
                game_frame([x, TAB_Y, tw, TAB_H], "UI_Inven_TitleFrame"),
                {"type": "rect", "rect": [x + 9, TAB_Y + 9, tw - 18, TAB_H - 18], "bg": ACC if on else TAB_OFF_BG,
                 "color": "#00000000", "frame": 0, "on_tap": act},
                {"type": "image", "rect": [x + tw // 2 - 130, TAB_Y + 20, 80, 80], "color": "#FFFFFFFF",
                 **({"src": ic} if ic.startswith("module:") else {"src_bind": ic})},
                label(x + tw // 2 + 30, TAB_Y + (52 if len(title) > 6 else 50), title, INK if on else TAB_OFF_TXT,
                      2 if len(title) > 6 else 3, align="center")]
    return out

CLOCK_X = 1048                                           # the watch: top right, the same spot on every page
CLOCK_R = 1170                                           # ... its text ends here (pictures to the right)

def clock():
    """The game's date and weather on top, the time of day under them: floating text, same spot on every page."""
    g = {"need_bind": "clock.ok"}
    # the pictures are the game's own (Icon_Weather_*, UI_Watch_Icon_*), read from the player's game files
    # right-aligned against the pictures, so the larger text grows to the left (never off the screen)
    return [tlabel(CLOCK_R, 18, "clock.date", TXT, 2, align="right", **g),
            {"type": "image", "rect": [CLOCK_R + 8, 10, 40, 40], "src_bind": "clock.wsrc", "color": "#FFFFFFFF", "need_bind": "clock.has_wx"},
            {"type": "image", "rect": [CLOCK_R + 6, 50, 44, 44], "src_bind": "clock.tsrc", "color": "#FFFFFFFF", "need_bind": "clock.has_time"},
            tlabel(CLOCK_R, 60, "clock.ttxt", GOLD, 2, align="right", **g)]

def gold(x=840):   # 0.38.46: closer to the date / weather (was 740), a little lower (user)
    """The player's gold (PlayerInfoSave.m_Gold) with the game's coin, left of the watch."""
    g = {"need_bind": "gold.ok"}
    return [{"type": "image", "rect": [x, 33, 44, 44], "src": "module:icon/Coin32", "color": "#FFFFFFFF", **g},
            tlabel(x + 52, 37, "gold.txt", GOLD, 4, **g)]

def header(title=None, plain=False, money=True, dave=True):
    """Dave (the game's own portrait: in his diving gear, or without the mask at the sushi bar),
    the page title and the watch. 0.37.117: dave=False (the phone pages) -- no portrait, only the gold and the watch."""
    out = [{"type": "image", "rect": [24, 16, 72, 72], "color": "#FFFFFFFF",
            **({"src": "module:icon/Mission_Dave"} if plain else {"src_bind": "ui.ic_dave"})}] if dave else []
    if title:
        out.append(label(108, 30, title, TXT, 5))
    return out + clock() + (gold() if money else [])

def seg(r, text, on_tap, lit, gate=None, bind=None):
    """A segmented-control button: lit (ACC, dark text) or plain. Two copies when `gate` decides."""
    out = []
    for is_lit, g in ((True, gate), (False, ("!" + gate) if gate else None)) if gate else ((lit, None),):
        w = {"type": "rect", "rect": [r[0] + 5, r[1] + 5, r[2] - 10, r[3] - 10], "bg": ACC if is_lit else TAB_OFF_BG,
             "color": "#00000000", "frame": 0, "on_tap": on_tap, "noframe": True}
        f = game_frame(r, "UI_Inven_TitleFrame", x2=False)
        if g:
            f["need_bind"] = g
        t = (value([r[0] + r[2] // 2, r[1] + 16, 0, 0], bind, INK if is_lit else TAB_OFF_TXT, 3, text=text, align="center")
             if bind else label(r[0] + r[2] // 2, r[1] + 16, text, INK if is_lit else TAB_OFF_TXT, 3, align="center"))
        if g:
            w["need_bind"] = g; t["need_bind"] = g
        out += [f, w, t]
    return out

def phone_bar():
    """0.37.118: BACK and CLOSE in the phone pages' header (they were at the bottom of each app): BACK is one B on the
    top screen (back to APPS here when diving or with the phone closed), CLOSE presses B until the phone is closed."""
    return [*game_button([24, 20, 190, 64], "BACK", "ph_back", kind="brown", sc=3, gate="!ph_nav"),
            *game_button([24, 20, 190, 64], "BACK", "to_phone", kind="brown", sc=3, gate="ph_nav"),
            *game_button([228, 20, 220, 64], "CLOSE", "ph_close", kind="grey2", sc=3, gate="!in_dive"),
            *game_button([228, 20, 220, 64], "CLOSE", "to_phone", kind="grey2", sc=3, gate="in_dive")]

def dive_tabs(active):
    """0.37.0: DIVE's own tabs right of Dave (like SUSHI's RECIPES / INGREDIENTS): MAP, CARGO, MISSIONS."""
    return (seg([24, 22, 150, 62], "MAP", "to_dive", active == "map") +
            seg([184, 22, 180, 62], "CARGO", "to_cargo", active == "cargo") +
            seg([374, 22, 240, 62], "MISSIONS", "to_tasks", active == "tasks") + hud_tab())

def hud_tab():
    """0.38.46: the HideHUD switch as a tab after MISSIONS (lit = the top HUD shown) (user; was on APPS)."""
    out, r = [], [624, 22, 130, 62]
    for _lit in (True, False):
        for _w in seg(r, "HUD", "hud_hide", _lit):
            _w["need_bind"] = "!hh.on" if _lit else "hh.on"
            out.append(_w)
    return out

dive = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *dive_tabs("map"),

        *([{"type": "button", "rect": [1050, 26, 160, 54], "text": "DEBUG", "on_tap": "to_debug", "color": DIM, "text_scale": 2}]
          if SHOW_DEBUG else []),
]
N = {"need_bind": "!in_dive"}
WHITE = "#FFFFFFFF"
             # out of the water: the cards stay, the numbers read 0

# Top of DIVE: the O2 gauge on the left; depth and cargo stacked beside it (the rest of the page
# is for the missions and the pinned recipe).
# O2 card: the game's own round O2 gauge (UI_O2_Gauge_re / _Stroke_re / _DAlarm_re).
GX, GY, GS = 56, 108, 252                  # gauge position and size
GAUGE0 = len(dive)
TINT = {"tint": OK, "tint_bind": "o2_state", "tint_colors": [OK, WARN, CRIT]}
dive += [rect([26, TOP, 312, 292], CARD),
         # low air: the gauge's card pulses red behind the gauge (like the game's HUD)
         {"type": "rect", "rect": [26, TOP, 312, 292], "bg": "#C0B0142A", "color": GOLD_W, "frame": 4, "pulse": True, "need_bind": "o2_alert"},
         # the game's own O2 HUD pictures: the alarm glow (low air), the dark disc, the disc filled
         # bottom-up by O2 % in the state's colour, and the ring
         {"type": "image", "rect": [GX - 4, GY - 4, GS + 8, GS + 8], "src": "module:frame/UI_O2_DAlarm_re", "color": "#C0FFFFFF",
          "tint": WARN, "tint_bind": "o2_state", "tint_colors": [WARN, WARN, CRIT], "need_bind": "o2_alarm"},
         {"type": "image", "rect": [GX + 12, GY + 12, GS - 24, GS - 24], "src": "module:frame/UI_O2_Gauge_re", "color": "#FFFFFFFF",
          "tint": "#FF0B1E30"},
         # (a bar filling upward with the disc as its picture: Eden's image fill only takes a tint's
         # alpha, a bar's picture takes its whole colour)
         {"type": "bar", "rect": [GX + 12, GY + 12, GS - 24, GS - 24], "bind": "o2_pct", "max": 100, "fill_dir": "up",
          "image": "module:frame/UI_O2_Gauge_re", "color": "#00000000", "bg": "#00000000", "frame": 0, **TINT, "need_bind": "in_dive"},
         {"type": "image", "rect": [GX + 8, GY + 8, GS - 16, GS - 16], "src": "module:frame/UI_O2_Stroke_re", "color": "#FFFFFFFF"},
         # like the game's HUD: a big number in the middle of the disc, "O2" above it (the "O" ends and
         # the "2" starts a fixed gap either side of the middle), the state under it. The whole DIVE
         # page is drawn in the HUD's digital font (the module switches fonts with the page).
         label(GX + GS // 2 + 1, GY + 52, "O", WHITE, 4, align="right"),
         label(GX + GS // 2 + 3, GY + 62, "2", WHITE, 2),   # 0.37.119: its foot level with the O's (was GY + 70)
         value([GX + GS // 2, GY + 104, 0, 0], "o2_i", WHITE, 12, align="center", **D),
         label(GX + GS // 2, GY + 104, "0", WHITE, 12, align="center", **N),
         label(GX + GS // 2, GY + GS - 4, "NOT DIVING", DIM, 2, align="center", **N),
         label(GX + GS // 2, GY + GS - 4, "NORMAL", OK, 2, align="center", need_bind="o2_ok"),
         label(GX + GS // 2, GY + GS - 4, "LOW AIR!", GOLD_W, 2, align="center", need_bind="o2_low"),
         label(GX + GS // 2, GY + GS - 4, "OUT OF AIR!", GOLD_W, 2, align="center", need_bind="o2_crit")]

GAUGE1 = len(dive)
# Depth card (beside the gauge, top)
DX, DW, DH = 350, 864, 140
CY = TOP                                   # depth 100 .. 240, cargo 252 .. 392
dive += [rect([DX, CY, DW, DH], CARD), label(DX + 24, CY + 14, "DEPTH", DIM, 3),
         # unsized text is anchored at rect x; right-aligned text grows left from it
         value([DX + 330, CY + 40, 0, 0], "depth_i", TXT, 9, align="right", need_bind="dnum"),
         label(DX + 330, CY + 40, "???", TXT, 9, align="center", need_bind="dvq"),
         value([DX + 330, CY + 40, 0, 0], "depth_d", TXT, 9, text=".", suffix="m", need_bind="dnum"),
         label(DX + 330, CY + 40, "0", TXT, 9, align="right", **N), label(DX + 330, CY + 40, ".0m", TXT, 9, **N),
         label(DX + 680, CY + 52, "0 m", TXT, 4, **N),
         *bar([DX + 24, CY + 104, DW - 48, 20], "zero", "one", ACC, **N),
         label(DX + DW - 24, CY + 14, "NORMAL DEPTH", DIM, 3, align="right", need_bind="shallow"),
         label(DX + DW - 24, CY + 14, "CRITICAL DEPTH", CRIT, 3, align="right", need_bind="deep"),
         label(DX + 560, CY + 56, "LIMIT", DIM, 3),
         value([DX + 680, CY + 52, 0, 0], "depth_max_i", TXT, 4, suffix=" m", **D),
         *bar([DX + 24, CY + 104, DW - 48, 20], "depth_x10", "depth_max_x10", ACC, need_bind="shallow"),
         *bar([DX + 24, CY + 104, DW - 48, 20], "depth_x10", "depth_max_x10", CRIT, need_bind="deep")]

# Cargo card (beside the gauge, bottom)
KY = CY + DH + 12
dive += [rect([DX, KY, DW, DH], CARD), label(DX + 24, KY + 14, "CARGO (KG)", DIM, 3),
         label(DX + 330, KY + 40, "0", TXT, 9, align="right", **N), label(DX + 330, KY + 40, ".0", TXT, 9, **N),
         label(DX + 680, KY + 56, "/0", DIM, 4, align="right", **N), label(DX + 680, KY + 56, ".0", DIM, 4, **N),
         *bar([DX + 24, KY + 104, DW - 48, 20], "zero", "one", OK, **N)]
for gate, col in [("cargo_ok", TXT), ("cargo_over", OVER)]:
    g = {"need_bind": gate}
    dive += [value([DX + 330, KY + 40, 0, 0], "w_i", col, 9, align="right", **g),
             value([DX + 330, KY + 40, 0, 0], "w_d", col, 9, text=".", **g),
             value([DX + 680, KY + 56, 0, 0], "wm_i", DIM, 4, text="/", align="right", **g),
             value([DX + 680, KY + 56, 0, 0], "wm_d", DIM, 4, text=".", **g)]
dive += [label(DX + DW - 24, KY + 14, "OVERWEIGHT", OVER, 3, align="right", need_bind="cargo_over"),
         *bar([DX + 24, KY + 104, DW - 48, 20], "w_x10", "wm_x10", OK, need_bind="cargo_ok"),
         *bar([DX + 24, KY + 104, DW - 48, 20], "w_x10", "wm_x10", OVER, need_bind="cargo_over"),
         ]

# 0.31.0 MAP view: one card under the header holds everything. The O2 gauge moves inside it (the same
# widgets, inset), depth and cargo become compact cards under it, and the rest is the map.
# Eden's hide_bind only hides with hide_eq (or keep_min/keep_max): "hide_bind" alone never hides.
HIDE_ON_MAP = {"hide_bind": "dive.map", "hide_eq": 1}      # the MISSIONS view's widgets
HIDE_OFF_MAP = {"hide_bind": "dive.map", "hide_eq": 0}     # the MAP view's widgets
for w in dive[GAUGE0:]:
    w.update(HIDE_ON_MAP)
def shifted(ws, ox, oy, hide):
    out = []
    for w in copy.deepcopy(ws):
        r = w["rect"]; w["rect"] = [r[0] + ox, r[1] + oy, *r[2:]]; w.update(hide); out.append(w)
    return out
MAPCARD = {**rect([26, TOP, 1188, 916 - TOP], CARD), "need_bind": "dive.map"}
RX, RW = 832, 1226 - 832                               # 0.37.5: the right column, to the screen's edge   # the right column: from the map's edge (under the
                                                      # header's coin) to the card's, 394 wide
gauge_map = [w for w in shifted(dive[GAUGE0:GAUGE1], RX + (RW - 312) // 2 - 26, 12, HIDE_OFF_MAP)   # centred
             if not (w.get("type") == "rect" and (w.get("bg") == CARD or w.get("pulse")))]   # no card, no alarm box:
                                                                                        # low air shows on the disc
# (0.37.5: no card behind the map page -- the map gets the room)
dive += gauge_map
MV = HIDE_OFF_MAP
CX, CW, CH = RX, RW, 118          # 0.37.1: shorter (room for the pinned ingredients)
CM = CX + CW // 2 - 12                     # the numbers: whole part ends here, decimals start
CDY = TOP + 304                            # compact depth card (below the gauge)
dive += [label(CX + 16, CDY + 6, "DEPTH", DIM, 2, **MV),
         value([CX + CW - 16, CDY + 6, 0, 0], "depth_max_i", TXT, 2, text="LIMIT ", suffix=" m", align="right", **D, **MV),
         label(CX + CW - 16, CDY + 6, "LIMIT 0 m", TXT, 2, align="right", **N, **MV),
         value([CM, CDY + 32, 0, 0], "depth_i", TXT, 7, align="right", need_bind="dnum", **MV),
         {**label(CM, CDY + 32, "???", TXT, 7, align="center"), "need_bind": "dvq", **MV},   # 0.38.5: the village
         value([CM, CDY + 32, 0, 0], "depth_d", TXT, 7, text=".", suffix="m", need_bind="dnum", **MV),
         label(CM, CDY + 32, "0", TXT, 7, align="right", **N, **MV), label(CM, CDY + 32, ".0m", TXT, 7, **N, **MV),
         *bar([CX + 16, CDY + 96, CW - 32, 16], "zero", "one", ACC, **N, **MV),
         *bar([CX + 16, CDY + 96, CW - 32, 16], "depth_x10", "depth_max_x10", ACC, need_bind="shallow", **MV),
         *bar([CX + 16, CDY + 96, CW - 32, 16], "depth_x10", "depth_max_x10", CRIT, need_bind="deep", **MV)]
CKY = CDY + CH + 6                        # compact cargo card
dive += [label(CX + 16, CKY + 6, "CARGO (KG)", DIM, 2, **MV),
         label(CM, CKY + 32, "0", TXT, 7, align="right", **N, **MV), label(CM, CKY + 32, ".0", TXT, 7, **N, **MV),
         label(CX + CW - 16, CKY + 48, "/0.0", DIM, 3, align="right", **N, **MV),
         *bar([CX + 16, CKY + 96, CW - 32, 16], "zero", "one", OK, **N, **MV)]
for gate, col in [("cargo_ok", TXT), ("cargo_over", OVER)]:
    g = {"need_bind": gate, **MV}
    dive += [value([CM, CKY + 32, 0, 0], "w_i", col, 7, align="right", **g),
             value([CM, CKY + 32, 0, 0], "w_d", col, 7, text=".", **g),
             value([CX + CW - 56, CKY + 48, 0, 0], "wm_i", DIM, 3, text="/", align="right", **g),
             value([CX + CW - 56, CKY + 48, 0, 0], "wm_d", DIM, 3, text=".", **g)]
dive += [*bar([CX + 16, CKY + 96, CW - 32, 16], "w_x10", "wm_x10", OK, need_bind="cargo_ok", **MV),
         *bar([CX + 16, CKY + 96, CW - 32, 16], "w_x10", "wm_x10", OVER, need_bind="cargo_over", **MV)]
LEGY = CKY + CH + 14                       # what the map's marks mean, and DIAG

# Pinned recipe (pin one on SUSHI > RECIPES): per ingredient what the restaurant has in storage,
# what this dive carries ("+ 2 this dive"), and the total against the target servings.
RY = 652                                   # 652 .. 916: up to 2 pinned recipes, 128 px each
LOWER0 = len(dive)                         # from here to the pin prompt: hidden while the map shows
dive += [{**rect([26, RY, 1188, 264], CARD), "on_tap": "to_sushi"},
         label(50, RY + 40, "PINNED RECIPES", DIM, 2, need_bind="!pin.on"),
         label(50, RY + 90, "Pin a recipe on SUSHI to track its ingredients here", DIM, 3, need_bind="!pin.on")]
PIN_STEP = 128
for p in range(2):
    y, P = RY + 8 + p * PIN_STEP, f"pin{p}."
    g = {"need_bind": P + "on"}
    # tap a pinned recipe: UNPIN it or OPEN its panel on SUSHI
    dive += [{"type": "rect", "rect": [30, y - 4, 1180, PIN_STEP], "bg": "#00000000", "color": "#00000000", "frame": 0,
              "on_tap": f"pin_pick_{p}", **g},
             {"type": "image", "rect": [36, y - 2, 60, 60], "src_bind": P + "isrc", "color": "#FFFFFFFF", **g},
             tlabel(110, y + 6, P + "name", GOLD, 3, wrap_width=560, max_lines=1, **g),
             value([960, y + 6, 0, 0], P + "canall", OK, 3, align="right", need_bind=P + "done"),
             value([960, y + 6, 0, 0], P + "canall", CRIT, 3, align="right", need_bind=P + "notdone"),
             value([960, y + 6, 0, 0], P + "target", DIM, 3, text=" / ", suffix=" servings", **g)]
    if p:
        dive += [{"type": "rect", "rect": [40, y - 4, 1160, 2], "bg": "#FF1B3A55", "color": "#00000000", **g}]
    for k in range(4):
        # one ingredient on one line: picture, have/need (have = storage + this dive), name.
        # two per line; a recipe with 3-4 ingredients uses a second line
        x, cy, gk = 110 + (k % 2) * 544, y + (36 if k < 2 else 80), {"need_bind": f"{P}on{k}"}
        dive += [{"type": "image", "rect": [x, cy, 42, 42], "src_bind": f"{P}nisrc{k}", "color": "#FFFFFFFF", **gk},
                 value([x + 84, cy + 11, 0, 0], f"{P}have{k}", OK, 3, align="right", need_bind=f"{P}ok{k}"),
                 value([x + 84, cy + 11, 0, 0], f"{P}have{k}", CRIT, 3, align="right", need_bind=f"{P}short{k}"),
                 value([x + 84, cy + 11, 0, 0], f"{P}need{k}", DIM, 3, text="/", **gk),
                 tlabel(x + 146, cy + 11, f"{P}nn{k}", TXT, 3, wrap_width=384, max_lines=1, **gk)]


# DEBUG page: raw values for the unverified fields
dbg = [rect([0, 0, 1240, 1080], BG), label(40, 30, "DEBUG", ACC, 5),
       {"type": "button", "rect": [1040, 24, 170, 56], "text": "BACK", "on_tap": "to_dive", "color": DIM, "text_scale": 3},
       label(40, 100, "raw values x100 - compare with the game and report back", DIM, 2),
       label(40, 1030, f"Dave the Diver companion {VERSION}  -  game v1.0.6.1267", DIM, 2)]
y = 150
for name, key in [("O2", "o2_x10"), ("O2 warning", "o2_warn_x100"), ("O2 urgent", "o2_urgent_x100"),
                  ("O2 tank max (x10)", "o2.max_x10"), ("O2 base (x10)", "o2.base_x10"),
                  ("Depth (x10)", "depth_x10"), ("Depth max", "depth_max_x100"), ("O2 equip slots", "o2.listn"),
                  ("O2 equip slot", "o2.idx"), ("Weight (x10)", "w_x10"), ("Weight max (x10)", "wm_x10"),
                  ("O2 state 0/1/2", "o2_state"), ("O2 item bonus (x10)", "o2.item_x10"), ("O2 read stage 0-4", "o2.stage"),
                  ("In dive", "in_dive")]:
    dbg += [label(60, y, name, DIM, 3), value([620, y, 0, 0], key, TXT, 3)]
    y += 50
dbg += [{"type": "label", "rect": [60, y - 8, 0, 0], "bind_text": "o2.keys", "color": ACC, "text_scale": 2, "wrap_width": 1150, "max_lines": 1}]
dbg += [label(60, y + 10, "Mission 1 conditions: now / max / target / flags / text length", DIM, 2)]
for j in range(MAX_COND):
    yy = y + 50 + j * 40
    dbg += [label(60, yy, f"row {j + 1}", DIM, 2)] + [value([x, yy, 0, 0], f"{k}0_{j}", TXT, 2, need_bind=f"cv0_{j}")
                                                     for x, k in [(220, "cnow"), (400, "cmax"), (580, "ctgt"), (760, "cflags"), (1000, "clen")]]

# MISSIONS page: the game's own mission tracker, read as text (styled after the in-game list)
def tlabel(x, y, key, c=TXT, sc=3, **kw):
    return {"type": "label", "rect": [x, y, 0, 0], "bind_text": key, "color": c, "text_scale": sc, **kw}
GOLD, DONE, STRIKE = "#FFFFD54A", "#FF4CC85A", "#B08FA3B4"   # strike: soft grey-blue, partly see-through
derived += [ex(f"nopic{i}", f"mv{i} && !m.haspic{i}") for i in range(MAX_TASKS)]
tp = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *dive_tabs("tasks"),
      label(40, 500, "No missions in progress", DIM, 4, need_bind="no_mis")]
# 0.37.8: each mission card is as tall as its rows (up to 6) and the cards stack under each other:
# mr{i} = rows in use, my{i} = how far down card i sits (the cards above it and their gaps)
M_HEAD, M_ROW, M_PAD, M_GAP = 78, 50, 10, 10
for i in range(MAX_TASKS):
    derived.append(ex(f"mr{i}", " + ".join(f"(cv{i}_{j} ? 1 : 0)" for j in range(MAX_COND))))
    derived.append(ex(f"my{i}", " + ".join(f"(mv{k} ? {M_HEAD + M_PAD + M_GAP} + {M_ROW} * mr{k} : 0)" for k in range(i)) or "0"))
    for n in range(1, MAX_COND + 1):
        derived.append(ex(f"mrn{i}_{n}", f"mv{i} && mr{i} == {n}"))
y = TOP + 6
for i in range(MAX_TASKS):
    g = {"need_bind": f"mv{i}"}
    mw = []
    for n in range(1, MAX_COND + 1):
        mw.append(rect([26, y, 1188, M_HEAD + M_ROW * n + M_PAD], CARD, need_bind=f"mrn{i}_{n}"))
    mw += [{"type": "image", "rect": [52, y + 14, 48, 48], "src": "module:icon/Mission_MainIcon_28", "color": "#FFFFFFFF", "need_bind": f"nopic{i}"},
           # the character who gave the mission (their portrait, when the game names one)
           {"type": "image", "rect": [44, y + 8, 60, 60], "src_bind": f"m.pic{i}", "color": "#FFFFFFFF", **g},
           # 0.37.20: the mission's kind, with the game's own tracker icons (MissionType: 0 main, 1 side)
           {"type": "image", "rect": [112, y + 24, 30, 30], "src_bind": f"m.sym{i}", "color": "#FFFFFFFF", "need_bind": f"mtsym{i}"},   # 0.37.21: the tracker's own
           {"type": "image", "rect": [112, y + 24, 30, 30], "src": "module:icon/Mission_MainIcon_28", "color": "#FFFFFFFF", "need_bind": f"mtmain{i}"},
           {"type": "image", "rect": [112, y + 24, 30, 30], "src": "module:icon/Mission_SubIcon_28", "color": "#FFFFFFFF", "need_bind": f"mtside{i}"},
           tlabel(152, y + 24, f"t{i}", GOLD, 4, wrap_width=880 - 42 - 150, max_lines=1, **g),
          ]
    # 0.37.20: PIN puts this mission's items on the map (one at a time; tap again to unpin)
    derived += [ex(f"mtsym{i}", f"mv{i} && m.hassym{i}"),
                ex(f"mtmain{i}", f"mv{i} && !m.hassym{i} && m.type{i} == 0"), ex(f"mtside{i}", f"mv{i} && !m.hassym{i} && m.type{i} != 0")]
    for _w in seg([1050, y + 12, 140, 56], "PIN", f"m_pin_{i}", False, gate=f"mpin_on{i}"):
        if _w.get("need_bind") == f"!mpin_on{i}":
            _w["need_bind"] = f"mpin_off{i}"
        mw.append(_w)
    derived += [ex(f"mpin_on{i}", f"mv{i} && m.pin{i}"), ex(f"mpin_off{i}", f"mv{i} && !m.pin{i} && m.canpin{i}")]   # 0.37.47: no PIN on a mission the map cannot show
    for j in range(MAX_COND):
        yy = y + M_HEAD + 8 + j * M_ROW
        o, k = {"need_bind": f"copen{i}_{j}"}, {"need_bind": f"cdone{i}_{j}"}
        # the game's mission tracker: its check box, with its tick on top once done
        mw += [{"type": "image", "rect": [80, yy - 4, 36, 36], "src": "module:icon/Mission_CheckBox", "color": "#FFFFFFFF",
                "need_bind": f"cv{i}_{j}"},
               {"type": "image", "rect": [80, yy - 4, 36, 36], "src": "module:icon/Mission_Check", "color": "#FFFFFFFF", "tint": DONE, **k},
               # the item the condition asks for (the game's own picture; nothing when it names no item)
               {"type": "image", "rect": [128, yy - 8, 44, 44], "src_bind": f"m.ic{i}_{j}", "color": "#FFFFFFFF", **o},
               {"type": "image", "rect": [128, yy - 8, 44, 44], "src_bind": f"m.ic{i}_{j}", "color": "#80FFFFFF", **k},
               ]
        # the text sits right after the picture, or where the picture would be when there is none
        for x0, sfx, pic in ((182, "p", f"m.hasic{i}_{j}"), (128, "n", f"!m.hasic{i}_{j}")):
            q = f"{i}_{j}{sfx}"
            derived.extend([ex(f"xo{q}", f"copen{i}_{j} && {pic}"), ex(f"xd{q}", f"cdone{i}_{j} && {pic}"),
                            ex(f"xc{q}", f"cntd{i}_{j} && {pic}"), ex(f"xn{q}", f"cdnc{i}_{j} && {pic}")])
            mw += [tlabel(x0, yy, f"c{i}_{j}", TXT, 3, wrap_width=740, max_lines=1, need_bind=f"xo{q}"),
                   tlabel(x0, yy, f"c{i}_{j}", DIM, 3, wrap_width=740, max_lines=1, need_bind=f"xd{q}"),
                   # finished: a line through the text only (16 px per character at this size)
                   {"type": "bar", "rect": [x0 - 4, yy + 12, 48 * 15, 3], "bind": f"clen{i}_{j}", "max": 48,
                    "color": STRIKE, "bg": "#00000000", "frame": 0, "need_bind": f"xd{q}"}]
        mw += [
               tlabel(1170, yy, f"n{i}_{j}", "#FFFF6B6B", 3, align="right", need_bind=f"cnto{i}_{j}"),
               tlabel(1170, yy, f"n{i}_{j}", DONE, 3, align="right", need_bind=f"cntd{i}_{j}")]
    for w in mw:
        w["y_bind"], w["y_scale"] = f"my{i}", 1
    tp += mw

# 0.38.58: under the tracker's cards, the other active missions (MissionManager's list, as the game's mission list
# picks them). 0.38.61: each one is a card like the tracker's -- the giver, the kind, the title, PIN, and the task's rows
# (tick box, item picture, the game's words, the count) -- read from the mission data (round 68: MissionHUDCondition).
AM_SHOW, AM_ROWS = 6, 4                          # the module's kAmShow / kAmRow
derived.append(ex("mend", " + ".join(f"(mv{k} ? {M_HEAD + M_PAD + M_GAP} + {M_ROW} * mr{k} : 0)" for k in range(MAX_TASKS))))
derived.append(ex("no_mis", "!mv0 && !om.on0"))
for c in range(AM_SHOW):
    # card c sits under the tracker's cards and the other cards before it (each as tall as its rows)
    derived.append(ex(f"oy{c}", "mend" + "".join(f" + (om.on{k} ? {M_HEAD + M_PAD + M_GAP} + {M_ROW} * om.nr{k} : 0)" for k in range(c))))
    derived.append(ex(f"amc{c}", f"om.on{c} && (oy{c} + {TOP + 6} + {M_HEAD + M_PAD} + {M_ROW} * om.nr{c}) <= {BOTTOM}"))   # only cards that fit above the tabs
    y = TOP + 6
    G_ = {"need_bind": f"amc{c}"}
    aw = []
    for n in range(0, AM_ROWS + 1):
        derived.append(ex(f"amn{c}_{n}", f"amc{c} && om.nr{c} == {n}"))
        aw.append(rect([26, y, 1188, M_HEAD + M_ROW * n + M_PAD], CARD, need_bind=f"amn{c}_{n}"))
    aw += [{"type": "image", "rect": [44, y + 8, 60, 60], "src_bind": f"om.pic{c}", "color": "#FFFFFFFF", "need_bind": f"amp{c}"},
           {"type": "image", "rect": [52, y + 14, 48, 48], "src": "module:icon/Mission_MainIcon_28", "color": "#FFFFFFFF", "need_bind": f"amnp{c}"},
           {"type": "image", "rect": [112, y + 24, 30, 30], "src": "module:icon/Mission_MainIcon_28", "color": "#FFFFFFFF", "need_bind": f"amk0{c}"},
           {"type": "image", "rect": [112, y + 24, 30, 30], "src": "module:icon/Mission_SubIcon_28", "color": "#FFFFFFFF", "need_bind": f"amk1{c}"},
           {"type": "image", "rect": [112, y + 24, 30, 30], "src": "module:icon/Mission_VIPIcon", "color": "#FFFFFFFF", "need_bind": f"amk2{c}"},
           tlabel(152, y + 24, f"om.t{c}", GOLD, 4, wrap_width=880 - 42 - 150, max_lines=1, **G_)]
    derived += [ex(f"amp{c}", f"amc{c} && om.haspic{c}"), ex(f"amnp{c}", f"amc{c} && om.nopic{c}"),
                *(ex(f"amk{kd}{c}", f"amc{c} && om.k{kd}_{c}") for kd in range(3)),
                ex(f"ampon{c}", f"amc{c} && om.pon{c}"), ex(f"ampoff{c}", f"amc{c} && om.poff{c}")]
    for lit, nb in ((True, f"ampon{c}"), (False, f"ampoff{c}")):
        for _w in seg([1050, y + 12, 140, 56], "PIN", f"am_pin_{c}", lit):
            _w["need_bind"] = nb
            aw.append(_w)
    for r in range(AM_ROWS):   # the rows, as the tracker's cards draw theirs
        yy = y + M_HEAD + 8 + r * M_ROW
        q = f"{c}_{r}"
        derived += [ex(f"arv{q}", f"amc{c} && om.rv{q}"), ex(f"aro{q}", f"amc{c} && om.ro{q}"), ex(f"ard{q}", f"amc{c} && om.rd{q}"),
                    ex(f"ari{q}", f"amc{c} && om.rhi{q}"), ex(f"arno{q}", f"amc{c} && om.rno{q}"), ex(f"arnd{q}", f"amc{c} && om.rnd{q}")]
        for x0, sfx, pic in ((182, "p", f"om.rhi{q}"), (128, "n", f"!om.rhi{q}")):
            derived += [ex(f"axo{q}{sfx}", f"aro{q} && {pic}"), ex(f"axd{q}{sfx}", f"ard{q} && {pic}")]
            aw += [tlabel(x0, yy, f"om.rt{q}", TXT, 3, wrap_width=740, max_lines=1, need_bind=f"axo{q}{sfx}"),
                   tlabel(x0, yy, f"om.rt{q}", DIM, 3, wrap_width=740, max_lines=1, need_bind=f"axd{q}{sfx}"),
                   {"type": "bar", "rect": [x0 - 4, yy + 12, 48 * 15, 3], "bind": f"om.rl{q}", "max": 48,
                    "color": STRIKE, "bg": "#00000000", "frame": 0, "need_bind": f"axd{q}{sfx}"}]
        aw += [{"type": "image", "rect": [80, yy - 4, 36, 36], "src": "module:icon/Mission_CheckBox", "color": "#FFFFFFFF", "need_bind": f"arv{q}"},
               {"type": "image", "rect": [80, yy - 4, 36, 36], "src": "module:icon/Mission_Check", "color": "#FFFFFFFF", "tint": DONE, "need_bind": f"ard{q}"},
               {"type": "image", "rect": [128, yy - 8, 44, 44], "src_bind": f"om.ri{q}", "color": "#FFFFFFFF", "need_bind": f"ari{q}"},
               tlabel(1170, yy, f"om.rn{q}", "#FFFF6B6B", 3, align="right", need_bind=f"arno{q}"),
               tlabel(1170, yy, f"om.rn{q}", DONE, 3, align="right", need_bind=f"arnd{q}")]
    for w in aw:
        w["y_bind"], w["y_scale"] = f"oy{c}", 1
    tp += aw

# compact mission strip on the Dive page: one row per mission that still has open conditions,
# showing its first open condition; finished missions drop out and the rows close up.
for i in range(MAX_TASKS):
    for j in range(MAX_COND):
        prior = "".join(f" && !copen{i}_{k}" for k in range(j))
        derived.append(ex(f"sfirst{i}_{j}", f"copen{i}_{j}{prior}"))
        derived.append(ex(f"scnt{i}_{j}", f"sfirst{i}_{j} && ccnt{i}_{j}"))
    derived.append(ex(f"mopen{i}", f"mv{i} && (" + " || ".join(f"copen{i}_{j}" for j in range(MAX_COND)) + ")"))
    # with a mission pinned, DIVE keeps the pinned one and the main story ones (the game's "!")
    derived.append(ex(f"ms{i}", f"mopen{i} && (m.pin{i} || m.main{i} || !m.anypin)"))
    derived.append(ex(f"mgold{i}", f"ms{i} && !m.pin{i}"))
    derived.append(ex(f"mpinr{i}", f"ms{i} && m.pin{i}"))
    for j in range(MAX_COND):
        derived.append(ex(f"sf{i}_{j}", f"sfirst{i}_{j} && ms{i}"))
        derived.append(ex(f"sc{i}_{j}", f"scnt{i}_{j} && ms{i}"))
for i in range(MAX_TASKS):
    derived.append(ex(f"spos{i}", " + ".join(f"(ms{k} ? 1 : 0)" for k in range(i)) or "0"))
derived.append(ex("sany", " || ".join(f"ms{i}" for i in range(MAX_TASKS))))
MY = 404                                   # mission strip on the Dive page: 404 .. 640
dive += [rect([26, MY, 1188, 236], CARD), label(50, MY + 18, "MISSIONS", DIM, 3),
         label(50, MY + 100, "No missions in progress (or not read yet)", DIM, 3, need_bind="!sany"),
         label(1190, MY + 20, "pinned + main", DIM, 2, align="right", need_bind="m.anypin")]
for i in range(MAX_TASKS):
    yy, pos = MY + 60, {"y_bind": f"spos{i}", "y_scale": 58}
    dive += [tlabel(50, yy, f"t{i}", GOLD, 3, wrap_width=560, max_lines=1, need_bind=f"mgold{i}", **pos),
             tlabel(50, yy, f"t{i}", ACC, 3, wrap_width=560, max_lines=1, need_bind=f"mpinr{i}", **pos)]
    for j in range(MAX_COND):
        dive += [tlabel(640, yy, f"c{i}_{j}", TXT, 3, wrap_width=440, max_lines=1, need_bind=f"sf{i}_{j}", **pos),
                 tlabel(1190, yy, f"n{i}_{j}", "#FFFF6B6B", 3, align="right", need_bind=f"sc{i}_{j}", **pos)]

# the prompt
PS = {"need_bind": "pinsel.on"}
PPY = (1080 - 300) // 2              # centred on the screen
pin_prompt = [{**rect([0, 0, 1240, 1080], "#B0000000", **PS), "on_tap": "pin_cancel"},
         {**rect([120, PPY, 1000, 300], "#FF0B1B2A", **PS), "on_tap": "noop"},
         game_frame([120, PPY, 1000, 300], "UI_EdgeFrame_Stroke", tint=GOLD, **PS),
         {"type": "image", "rect": [150, PPY + 30, 80, 80], "src_bind": "pinsel.isrc", "color": "#FFFFFFFF", **PS},
         label(250, PPY + 34, "PINNED RECIPE", DIM, 2, **PS),
         tlabel(250, PPY + 64, "pinsel.name", GOLD, 4, wrap_width=840, max_lines=1, **PS),
         *game_button([150, PPY + 160, 290, 100], "UNPIN", "pin_unpin", gate="pinsel.on"),
         *game_button([475, PPY + 160, 290, 100], "OPEN RECIPE", "pin_open", gate="pinsel.on", sc=3),
         *game_button([800, PPY + 160, 290, 100], "CANCEL", "pin_cancel", kind="grey", gate="pinsel.on")]
LOWER1 = len(dive)

for w in dive[LOWER0:LOWER1]:
    assert "hide_bind" not in w, w
    w.update(HIDE_ON_MAP)

# MISSIONS | MAP on the header: the lower half is either the missions + pinned recipes or the map
derived += [ex("dv0", "dive.map == 0"), ex("dv1", "dive.map == 1"),
            ex("map.show", "dive.map && map.ok && !scene.village"), ex("map.wait", "dive.map && !map.ok && !scene.village"),
            ex("map.rec", "0"),   # 0.38.12: the record chamber keeps the drawn map (user); its panel is off
            ex("map.vill", "dive.map && scene.village"),
            *[ex(f"vmon{t}", f"map.vill && vm.on{t}") for t in range(1, 16)], ex("vmpin", "map.vill && vm.pin"),
            # 0.38.14: buildings at their size (whole map) or x3 (zoomed, only those that fit inside); marks either way
            *[e for t in range(1, 16) for e in (ex(f"vmb{t}", f"map.vill && !vm.zoomed && vm.on{t}"), ex(f"vmb{t}z", f"map.vill && vm.zoomed && vm.fit{t}"),
                                                ex(f"vmi{t}", f"map.vill && !vm.zoomed && vm.on{t}"), ex(f"vmi{t}z", f"map.vill && vm.zoomed && vm.on{t}"))],
            ex("vmz1", "map.vill && vm.zoomed"),
            ex("vmd", "map.vill && in_dive"),   # 0.38.5: the Sea People Village (MermanVillage) has no dive map
            ex("mlim", "map.show && map.lim_on"), ex("mterr", "map.show && map.terrain"),
            *[ex(f"mt{k}", f"map.show && map.t{k}on") for k in range(6)],
            ex("msea", "map.show && map.sea_on"), ex("mkall1", "map.show && mk.all"), ex("mkall0", "map.show && !mk.all"),
            ex("mzoom1", "map.show && map.zoomed"),
            # the toggles under the map (0.36.17: each on / off on its own; mk.f0 chests, f1 O2, f2 weapons, f3 missions)
            ex("fchest", "mk.f0 != 0"), ex("fo2", "mk.f1 != 0"), ex("fwpn", "mk.f2 != 0"), ex("fmis", "m.anypin"),   # 0.37.20: mission marks = the pinned mission's (no MISSION filter)  (0.37.82: EcoWatcher map marks on the back burner)
            *[ex(f"mkflt{k}", f"map.show && mk.f{k} != 0") for k in range(4)],
            *[ex(f"mkflt{k}n", f"map.show && mk.f{k} == 0") for k in range(4)],
            *[e for i in range(MARKERS) for e in (ex(f"mkb{i}", f"map.show && fchest && mk.on{i} && mk.t{i} == 1"),
                                                  ex(f"mkw{i}", f"map.show && fwpn && mk.on{i} && mk.t{i} == 3"),
                                                  ex(f"mko{i}", f"map.show && fo2 && mk.on{i} && mk.t{i} == 2"),
                                                  ex(f"mkm{i}", f"map.show && fmis && mk.on{i} && mk.t{i} == 4"))],
            *[ex(f"mmv{k}", f"map.show && fmis && mm.on{k}") for k in range(MISSION_MARKS)],
            *[ex(f"amv{k}", f"map.show && mk.f4 != 0 && am.on{k}") for k in range(AMMO_MARKS)],
            *[e for k in range(POD_MARKS) for e in (ex(f"pdv{k}", f"map.show && mk.f5 != 0 && pd.on{k} && !pd.used{k}"),
                                                    ex(f"pdu{k}", f"map.show && mk.f5 != 0 && pd.on{k} && pd.used{k}"))],
            *[e for k in range(FISH_MARKS) for e in (ex(f"fmr{k}", f"map.show && fm.on{k} && fm.t{k} == 1"),
                                                     ex(f"fmm{k}", f"map.show && fmis && fm.on{k} && fm.t{k} == 2"),
                                                     ex(f"fmra{k}", f"map.show && fm.on{k} && fm.t{k} == 3"),
                                                     ex(f"fmma{k}", f"map.show && fmis && fm.on{k} && fm.t{k} == 4"))]]
# (0.37.0: the DIVE / MAP switch is gone -- the map is DIVE's main view; the module keeps dive.map at 1)

# The dive map (0.29.0): the level as the game bounds it, Dave where the camera follows him (his height
# from the depth the game shows), the part on screen, the depth limit and marks every 50 m. The module
# publishes positions in thousandths of the level's height (map.dx from the middle, map.ny from the top).
MAPY = TOP                                  # the map's titles; the boxes run to the card's foot
LOH = 100                                   # 0.38.26: the loadout strip under the map
FTY = 916 - 4 - LOH                         # the map's foot (0.38.26: shorter, the loadout under it)
MY0 = TOP + 14
MH = FTY - MY0
MK = MH / 1000                              # px per thousandth
MX0 = 14 + 86                               # room for the depth marks on the left (0.37.5: no card, wider)
MW = RX - 16 - MX0                          # up to the right column
XK = MW / 475                               # px per thousandth across: the Blue Hole is 124 x 261 units
                                            # (half width 237.5 thousandths of its height), so the map is
                                            # drawn MAP_STRETCH times wider than true
MAP_STRETCH = round(XK / MK, 2)
MCX = MX0 + MW // 2                         # the box's middle
# 0.38.9: the village map across the whole card (was the map box's 716): the right column gives way to a strip below
# 0.38.12: a little narrower, centred, inside the game's scroll frame (paper border + a roller each side)
VLW = 1110                                   # 0.38.14: wider (was 1060)
VX0 = (1240 - VLW) // 2
VMH = VLW * 804 // 1858                      # the village map at its own shape (1858x804)
VMY = 96 + (832 - (VMH + 248)) // 2 + 78   # 0.38.17: title, map and strip centred between the header and the tabs
VFP = 14                                     # the paper border around the picture
def vm_covers():
    x0, y0, x1, y1 = VX0 - VFP, VMY - VFP, VX0 + VLW + VFP, VMY + VMH + VFP
    g = {"need_bind": "map.vill", "noframe": True}
    out = [rect([6, 96, 1228, y0 - 96], ACQ_BODY, frame=0), rect([6, y1, 1228, 928 - y1], ACQ_BODY, frame=0),
           rect([6, y0, x0 - 6, y1 - y0], ACQ_BODY, frame=0), rect([x1, y0, 1234 - x1, y1 - y0], ACQ_BODY, frame=0),
           # the paper band (the game's frame colour) and its line
           rect([x0, y0, x1 - x0, VFP], VM_PAPER, frame=0), rect([x0, y1 - VFP, x1 - x0, VFP], VM_PAPER, frame=0),
           rect([x0, y0, VFP, y1 - y0], VM_PAPER, frame=0), rect([x1 - VFP, y0, VFP, y1 - y0], VM_PAPER, frame=0)]
    return [{**w, **g} for w in out] + vm_line()

def vm_line():
    """0.38.14: the game's dark green line inside the paper, its corners cut (a 2 px line; the cut drawn in 2 px steps)."""
    x0, y0, x1, y1, c, t = VX0 - VFP + 5, VMY - VFP + 5, VX0 + VLW + VFP - 5, VMY + VMH + VFP - 5, 10, 2
    g = {"need_bind": "map.vill"}
    out = [rect([x0 + c, y0, x1 - x0 - 2 * c, t], VM_LINE, frame=0), rect([x0 + c, y1 - t, x1 - x0 - 2 * c, t], VM_LINE, frame=0),
           rect([x0, y0 + c, t, y1 - y0 - 2 * c], VM_LINE, frame=0), rect([x1 - t, y0 + c, t, y1 - y0 - 2 * c], VM_LINE, frame=0)]
    for i in range(0, c, 2):
        out += [rect([x0 + i, y0 + c - i - t, t, t], VM_LINE, frame=0), rect([x1 - i - t, y0 + c - i - t, t, t], VM_LINE, frame=0),
                rect([x0 + i, y1 - c + i, t, t], VM_LINE, frame=0), rect([x1 - i - t, y1 - c + i, t, t], VM_LINE, frame=0)]
    return [{**w, **g, "noframe": True} for w in out]
VM_PAPER, VM_LINE, VM_TINT = "#FFC6D4BF", "#FF739584", "#FFE4EEDF"   # 0.38.13: sampled from the game's map screen
VMS = VLW / 1858                              # the game's map pixels -> ours
VMI = 44                                     # a round mark (60 px in the game's map)
# 0.38.8: the game's building types (VillageBuildingBehaviour.EBuildingType) -> its map pictures (UI_Map_DetailMap_*,
# sizes as in the game's files) and round marks (UI_Map_*)
VM_PIECES = {1: ("Icon12_Board", 84, 89, "Icon12_Board"), 2: ("Icon06_Forge", 206, 154, "Icon06_Forge"),
             3: ("Icon07_Game", 196, 189, "Icon07_Game"), 4: ("Icon05_Food", 135, 164, "Icon05_Food"),
             5: ("Icon05_Seed", 117, 146, "Icon05_Seed"), 6: ("Icon02_Portal", 106, 150, "Icon02_Portal"),
             7: ("Icon02_Portal", 106, 150, "Icon02_Portal"), 8: ("Icon04_Head", 447, 317, "Icon04_Head"),
             9: ("Icon03_Temple", 236, 317, "Icon03_Temple"), 10: ("Icon11_Square", 124, 206, "Icon11_Square"),
             11: ("Icon01_Cave", 88, 86, "Icon01_Cave"), 12: ("Icon09_Mirror", 53, 86, "Icon09_Mirror"),
             13: ("Icon08_Farm", 168, 44, "Icon08_Farm"), 14: ("Icon10_Gate", 48, 143, "Icon10_Gate"),
             15: ("Icon02_Portal", 106, 150, "Icon02_Portal")}
def _idx_sizes():   # the pictures' own sizes, from the package's index (the game's files)
    out = {}
    try:
        for l in open(PKG / "dualscreen" / "pictures.idx", encoding="ascii", errors="replace"):
            f = l.split()
            if len(f) >= 7 and f[0] == "S": out[f[1]] = (int(f[5]), int(f[6]))
    except OSError:
        pass
    return out
_vsz = _idx_sizes()
VM_PIECES = {t: (pic, *_vsz.get("UI_Map_DetailMap_" + pic, (w, h)), ic) for t, (pic, w, h, ic) in VM_PIECES.items()}
B = {"x_scale": XK, "y_scale": MK}
MS = {"need_bind": "map.show"}
VW, VH = round(87 * XK), round(49 * MK)     # the screen's part: about 22.7 x 12.7 units
dive += [label(MCX, TOP + 380, "The map appears while diving", DIM, 3, align="center", need_bind="map.wait"),
         # 0.38.6: the game's own village map (UI_Map_DetailMap + its detail layer, 1858x804), fitted to the box's width;
         # the buildings and Dave's pin come later (their places read live from the game's mini-map)
         # 0.38.13: the game's colours (from its map screen): the pale green paper (its white frame picture, tinted),
         # the dark green line inside it, and the rollers (936 px tall: the pieced loader, stretched)
         {**game_frame([VX0 - VFP, VMY - VFP, VLW + 2 * VFP, VMH + 2 * VFP], "UI_Map_DetailMap_FrmBg", x2=False, tint=VM_PAPER), "need_bind": "map.vill"},

         label(MCX, TOP + 360, "RECORD CHAMBER", ACC, 4, align="center", need_bind="map.rec"),
         label(MCX, TOP + 420, "No dive map here", DIM, 3, align="center", need_bind="map.rec"),
         {"type": "image", "src": f"module:bg/UI_Map_DetailMap@{VLW}x{VMH}", "color": VM_TINT, "rect": [VX0, VMY, VLW, VMH], "pan_zoom": True, "min_zoom": 1, "max_zoom": 3, "view_zoom_bind": "vm.zoom", "view_cx_bind": "vm.zx", "view_cy_bind": "vm.zy", "view_idle_ms": 500, "view_reset_bind": "vm.vreset", "need_bind": "map.vill"},
         {"type": "image", "src": f"module:bg/UI_Map_DetailMap_Detail@{VLW}x{VMH}", "color": VM_TINT, "rect": [VX0, VMY, VLW, VMH], "pan_zoom": True, "min_zoom": 1, "max_zoom": 3, "view_zoom_bind": "vm.zoom", "view_cx_bind": "vm.zx", "view_cy_bind": "vm.zy", "view_idle_ms": 500, "view_reset_bind": "vm.vreset", "need_bind": "map.vill"},
         # 0.38.9: the buildings over the mist layer (it hid them), a little faded as on the game's map; each stands on its
         # place (the game's area point = the building's foot); its round mark just above it; then Dave's pin (centred)
         *[{"type": "image", "src": f"module:frame/UI_Map_DetailMap_{pic}", "color": "#D8FFFFFF",
            "rect": [VX0 - round(w * VMS * zk) // 2, VMY - round(h * VMS * zk), round(w * VMS * zk), round(h * VMS * zk)],
            "x_bind": f"vm.x{t}", "y_bind": f"vm.y{t}", "x_scale": VLW / 1000, "y_scale": VMH / 1000, "need_bind": f"vmb{t}{'z' if zk > 1 else ''}"}
           for zk in (1, 3) for t, (pic, w, h, _) in VM_PIECES.items()],
         # 0.38.16: zoomed, a building may reach past the map: the card's own colour covers all round it, then the
         # paper border, its line, the rollers and the title go back on top (no building pops in and out any more)
         *vm_covers(),
         {"type": "image", "src": f"module:bg/UI_Map_Scroll_Deco@40x{VMH + 2 * VFP + 60}s", "color": "#FFFFFFFF",
          "rect": [VX0 - VFP - 34, VMY - VFP - 30, 40, VMH + 2 * VFP + 60], "need_bind": "map.vill"},
         {"type": "image", "src": f"module:bg/UI_Map_Scroll_Deco@40x{VMH + 2 * VFP + 60}s", "color": "#FFFFFFFF",
          "rect": [VX0 + VLW + VFP - 6, VMY - VFP - 30, 40, VMH + 2 * VFP + 60], "need_bind": "map.vill"},
         # 0.38.56: no title (user): Bei and Credit sit in its place
         *[{"type": "image", "src": f"module:frame/UI_Map_{ic}", "color": "#FFFFFFFF",
            # 0.38.23: the gate's mark as the game draws it -- right of its tower, low by its foot (0.38.22 photos); the
            # tower itself stays on its point at the map's left edge (it was moved with the mark: the gap)
            "rect": ([VX0 - VMI // 2 + round(0.025 * VLW), VMY - VMI // 2 - 20, VMI, VMI] if t == 14 else
                     [VX0 - VMI // 2, VMY - round(h * VMS * zk) - VMI // 2 - 4, VMI, VMI]),
            "x_bind": f"vm.x{t}", "y_bind": f"vm.y{t}", "x_scale": VLW / 1000, "y_scale": VMH / 1000, "need_bind": f"vmi{t}{'z' if zk > 1 else ''}"}
           for zk in (1, 3) for t, (pic, w, h, ic) in VM_PIECES.items()],
         {"type": "image", "src": "module:frame/UI_Map_Dave_LocationBG", "color": "#FFFFFFFF", "rect": [VX0 - 25, VMY - 34, 50, 68],
          "x_bind": "vm.px", "y_bind": "vm.py", "x_scale": VLW / 1000, "y_scale": VMH / 1000, "need_bind": "vmpin"},
         {"type": "image", "src": "module:icon/Mission_Dave_Diver", "color": "#FFFFFFFF", "rect": [VX0 - 20, VMY - 30, 40, 40],
          "x_bind": "vm.px", "y_bind": "vm.py", "x_scale": VLW / 1000, "y_scale": VMH / 1000, "need_bind": "vmpin"},
         # 0.38.14: tap the map: x3 around Dave / back to the whole village (as the dive map)
         # 0.38.17: no zoom on the village map (user)
         # under the map: O2, depth (??? as the game shows it here) and cargo, in place of the right column
         label(220, VMY + VMH + 70, "O2", DIM, 3, align="center", need_bind="map.vill"),
         value([220, VMY + VMH + 110, 0, 0], "o2_i", TXT, 9, align="center", need_bind="vmd"),
         label(620, VMY + VMH + 70, "DEPTH", DIM, 3, align="center", need_bind="map.vill"),
         label(620, VMY + VMH + 110, "???", TXT, 9, align="center", need_bind="map.vill"),
         label(1020, VMY + VMH + 70, "CARGO (KG)", DIM, 3, align="center", need_bind="map.vill"),
         value([1004, VMY + VMH + 110, 0, 0], "w_i", TXT, 9, align="right", need_bind="vmd"),
         value([1004, VMY + VMH + 110, 0, 0], "w_d", TXT, 9, text=".", need_bind="vmd"),
         value([1130, VMY + VMH + 136, 0, 0], "wm_i", DIM, 3, text="/", align="right", need_bind="vmd"),
         value([1130, VMY + VMH + 136, 0, 0], "wm_d", DIM, 3, text=".", need_bind="vmd"),
         # the water, the sea level line, the depth marks, the limit
         {"type": "rect", "rect": [MX0, MY0, MW, MH], "bg": "#FF0D3A5C", "color": ACC, "frame": 2, **MS},
         # the dive's own shape: open water / rock from the game's pathfinding grid (drawn by the module)
         # 0.36.2: the picture follows the module's view (whole level, or x3 around Dave after a tap)
         {"type": "image", "rect": [MX0, MY0, MW, MH], "src_bind": "map.img", "color": "#FFFFFFFF",
          "pan_zoom": True, "min_zoom": 1, "max_zoom": 3, "view_zoom_bind": "map.zoom",
          "view_cx_bind": "map.zx", "view_cy_bind": "map.zy", "view_idle_ms": 500,
          "view_reset_bind": "map.vreset", "need_bind": "mterr"},   # 0.37.16: snapped to the module's view every tick (the marks never lag the picture)
         {"type": "rect", "rect": [MX0, MY0, MW, MH], "bg": "#00000000", "color": ACC, "frame": 2, "need_bind": "mterr"},
         {"type": "rect", "rect": [MX0, MY0, MW, 3], "bg": OK, "color": "#00000000", "frame": 0, "y_bind": "map.sea", **B, "need_bind": "msea"},
         *[w for k in range(6) for w in (
             {"type": "rect", "rect": [MX0 - 12, MY0 - 1, 12, 3], "bg": DIM, "color": "#00000000", "frame": 0, "y_bind": f"map.t{k}", **B, "need_bind": f"mt{k}"},
             {**label(MX0 - 18, MY0 - 10, f"{50 * k} m", DIM, 1, align="right"), "y_bind": f"map.t{k}", **B, "need_bind": f"mt{k}"})],
         {"type": "rect", "rect": [MX0, MY0 - 1, MW, 3], "bg": CRIT, "color": "#00000000", "frame": 0, "y_bind": "map.lim", **B, "need_bind": "mlim"},
         {**label(MX0 + MW - 8, MY0 - 28, "LIMIT", CRIT, 1, align="right"), "y_bind": "map.lim", **B, "need_bind": "mlim"},
         # 0.35.1 (test): the chests the module found, under Dave
         *[w for i in range(MARKERS) for w in (
             {"type": "image", "rect": [MCX - 14, MY0 - 10, 28, 20], "src": "module:icon/CargoBox_Thumbnail", "color": "#FFFFFFFF",
              "x_bind": f"mk.x{i}", "y_bind": f"mk.y{i}", **B, "need_bind": f"mkb{i}"},
             # 0.37.36: the O2 box as it stands in the water (the game's O2Box_Head on O2Box_Body)
             {"type": "image", "rect": [MCX - 10, MY0 - 6, 20, 24], "src": "module:frame/O2Box_Body", "color": "#FFFFFFFF",
              "x_bind": f"mk.x{i}", "y_bind": f"mk.y{i}", **B, "need_bind": f"mko{i}"},
             {"type": "image", "rect": [MCX - 10, MY0 - 17, 21, 12], "src": "module:frame/O2Box_Head", "color": "#FFFFFFFF",
              "x_bind": f"mk.x{i}", "y_bind": f"mk.y{i}", **B, "need_bind": f"mko{i}"},
             {"type": "image", "rect": [MCX - 14, MY0 - 10, 28, 20], "src": "module:icon/CargoBox_Thumbnail", "color": "#FFFFFFFF", "tint": WCHEST,
              "x_bind": f"mk.x{i}", "y_bind": f"mk.y{i}", **B, "need_bind": f"mkw{i}"},
             # 0.36.9: mission items (the game's mission icon)
             {"type": "image", "rect": [MCX - 12, MY0 - 12, 24, 24], "src": "module:icon/Mission_MainIcon_28", "color": "#FFFFFFFF",
              "x_bind": f"mk.x{i}", "y_bind": f"mk.y{i}", **B, "need_bind": f"mkm{i}"})],
         # 0.36.12: loose items a mission in progress asks for (the module matches them to the mission's targets)
         # 0.37.77: a pinned EcoWatcher goal's pickup (starfish ...) shows its own picture (mm.ic, else the mission icon)
         *[{"type": "image", "rect": [MCX - 14, MY0 - 14, 28, 28], "src_bind": f"mm.ic{k}", "color": "#FFFFFFFF",
            "x_bind": f"mm.x{k}", "y_bind": f"mm.y{k}", **B, "need_bind": f"mmv{k}"} for k in range(MISSION_MARKS)],
         # 0.37.22: ammo pickups (the game's Ammo picture) and escape pods (dimmed once used)
         *[{"type": "image", "rect": [MCX - 12, MY0 - 12, 24, 24], "src": "module:icon/Ammo_Thumbnail", "color": "#FFFFFFFF",
            "x_bind": f"am.x{k}", "y_bind": f"am.y{k}", **B, "need_bind": f"amv{k}"} for k in range(AMMO_MARKS)],
         *[w for k in range(POD_MARKS) for w in (
             {"type": "image", "rect": [MCX - 9, MY0 - 15, 17, 29], "src": "module:frame/Icon_Radios01", "color": "#FFFFFFFF",
              "x_bind": f"pd.x{k}", "y_bind": f"pd.y{k}", **B, "need_bind": f"pdv{k}"},
             {"type": "image", "rect": [MCX - 9, MY0 - 15, 17, 29], "src": "module:frame/Icon_Radios01", "color": "#70FFFFFF",
              "x_bind": f"pd.x{k}", "y_bind": f"pd.y{k}", **B, "need_bind": f"pdu{k}"})],
         # 0.37.7: fish -- the pinned recipe's (their own picture) and the missions' (the mission mark)
         *[w for k in range(FISH_MARKS) for w in (
             {"type": "image", "rect": [MCX - 20, MY0 - 16, 40, 32], "src_bind": f"fm.src{k}", "color": "#FFFFFFFF",
              "x_bind": f"fm.x{k}", "y_bind": f"fm.y{k}", **B, "need_bind": f"fmr{k}"},
             {"type": "image", "rect": [MCX - 12, MY0 - 12, 24, 24], "src": "module:icon/Mission_MainIcon_28", "color": "#FFFFFFFF",
              "x_bind": f"fm.x{k}", "y_bind": f"fm.y{k}", **B, "need_bind": f"fmm{k}"},
             # 0.37.11: where the game spawns that fish (none swimming there yet): the same marks, dimmed
             {"type": "image", "rect": [MCX - 16, MY0 - 13, 32, 26], "src_bind": f"fm.src{k}", "color": "#80FFFFFF",
              "x_bind": f"fm.x{k}", "y_bind": f"fm.y{k}", **B, "need_bind": f"fmra{k}"},
             {"type": "image", "rect": [MCX - 10, MY0 - 10, 20, 20], "src": "module:icon/Mission_MainIcon_28", "color": "#80FFFFFF",
              "x_bind": f"fm.x{k}", "y_bind": f"fm.y{k}", **B, "need_bind": f"fmma{k}"})],
         # Dave (the game's own portrait of him); the module still publishes the screen's part
         # (map.vdx / map.vny) -- its box was dropped in 0.33.0
         # 0.38.22: the village map's pin (the game's UI_Map_Dave_LocationBG with his portrait), its tip on Dave (user)
         # 0.38.29: no pin arrow on the dive map -- his portrait centred on his spot (the arrow made him look lower, user)
         {"type": "image", "rect": [MCX - 22, MY0 - 22, 44, 44], "src": "module:icon/Mission_Dave_Diver", "color": "#FFFFFFFF",
          "x_bind": "map.dx", "y_bind": "map.ny", **B, **MS},
         # tap the map: x3 around Dave / back to the whole level
         {"type": "rect", "rect": [MX0, MY0, MW, MH], "bg": "#00000000", "color": "#00000000", "frame": 0,
          "on_tap": "map_zoom", "noframe": True, **MS},
         {**label(MX0 + MW - 10, MY0 + MH - 30, "x3", ACC, 2, align="right"), "need_bind": "mzoom1"}]
# 0.37.4: one FILTER button at the foot of the right column (under the pinned ingredients); a tap opens a
# list over the column: ALL (every mark on / off at once), CHEST, O2, WEAPON, MISSION (each on / off with its
# picture), DONE; a tap outside closes it. A gold dot on the button while some marks are hidden.
_tabs = [("CHEST", {"src": "module:icon/CargoBox_Thumbnail", "w": 28, "h": 20}),
         ("O2", {"src": "module:frame/O2Box_Body", "w": 24, "h": 28}),
         ("WEAPON", {"src": "module:icon/CargoBox_Thumbnail", "w": 28, "h": 20, "tint": WCHEST}),
         # 0.37.22 (round 36): ammo pickups and escape pods (bits 4 and 5; bit 3 is the mission pin's)
         ("AMMO", {"src": "module:icon/Ammo_Thumbnail", "w": 28, "h": 28}),
         ("POD", {"src": "module:frame/Icon_Radios01", "w": 18, "h": 30})]
_TBITS = [0, 1, 2, 4, 5]
derived += [ex("mkmenu1", "map.show && mk.menu != 0"), ex("mkmenu0", "map.show && mk.menu == 0"),
            ex("mkfltany", "map.show && (mk.f0 == 0 || mk.f1 == 0 || mk.f2 == 0 || mk.f4 == 0 || mk.f5 == 0)"),
            ex("mkallon", "mkmenu1 && mk.f0 != 0 && mk.f1 != 0 && mk.f2 != 0 && mk.f4 != 0 && mk.f5 != 0"),
            ex("mkalloff", "mkmenu1 && !(mk.f0 != 0 && mk.f1 != 0 && mk.f2 != 0 && mk.f4 != 0 && mk.f5 != 0)")]
_FB = [RX + 8, 916 - 6 - 72, RW - 16, 72]     # (the filter panel still opens in the right column)
# 0.38.26: the FILTER button on the map's top-right corner, smaller and see-through (user)
_FBS = [MX0 + MW - 150 - 8, MY0 + 8, 150, 52]
for _w in seg(_FBS, "FILTER", "mk_menu", False):
    _w["need_bind"] = "mkmenu0"; _w.update(HIDE_OFF_MAP)
    if _w.get("type") == "image": _w["color"] = "#99FFFFFF"
    if _w.get("type") == "rect" and _w.get("bg", "").endswith(TAB_OFF_BG[3:]): _w["bg"] = "#990E2436"
    if _w.get("type") == "label": _w["text_scale"] = 2; _w["rect"] = [_w["rect"][0], _FBS[1] + 14, 0, 0]
    dive.append(_w)
dive.append({"type": "rect", "rect": [_FBS[0] + _FBS[2] - 18, _FBS[1] + 8, 10, 10], "bg": GOLD, "color": "#00000000", "frame": 0,
             "need_bind": "mkfltany", **HIDE_OFF_MAP})
_PW, _RH = 236, 76                           # 0.38.27: a smaller panel, dropping down on the map under FILTER (0.38.29: narrow and tall rows, user)
_ROWS = [("ALL", None, "mk_every", "mkallon", "mkalloff")] + \
        [(n, ic, f"mk_toggle_{_TBITS[k]}", f"mkm{_TBITS[k]}on", f"mkm{_TBITS[k]}off") for k, (n, ic) in enumerate(_tabs)]
_PH = len(_ROWS) * _RH + 16 + _RH
_PX, _PY0 = _FBS[0] + _FBS[2] - _PW, _FBS[1] + _FBS[3] + 6
# a catcher over the whole card (tap outside = close), then the panel
dive.append({"type": "rect", "rect": [0, TOP, 1240, 916 - TOP], "bg": "#60000000", "color": "#00000000", "frame": 0,
             "on_tap": "mk_menu", "noframe": True, "need_bind": "mkmenu1", **HIDE_OFF_MAP})
dive.append({**rect([_PX, _PY0, _PW, _PH], "#F80B1B2A"), "on_tap": "noop", "color": ACC, "frame": 2,
             "need_bind": "mkmenu1", **HIDE_OFF_MAP})
for _k, (_name, _ic, _act, _on, _off) in enumerate(_ROWS):
    _r = [_PX + 10, _PY0 + 10 + _k * _RH, _PW - 20, _RH - 6]
    for _lit in (True, False):
        for _w in seg(_r, _name, _act, _lit):
            _w["need_bind"] = _on if _lit else _off
            if _w.get("type") == "label":   # 0.38.33: centred in the row, both ways (user)
                _w["text_scale"] = 2; _w["align"] = "center"; _w["rect"] = [_r[0] + _r[2] // 2, _r[1] + _r[3] // 2 - 14, 0, 0]
            _w.update(HIDE_OFF_MAP)
            dive.append(_w)
    if _ic:
        _img = {"type": "image", "rect": [_r[0] + 30 - _ic["w"] // 2, _r[1] + _r[3] // 2 - _ic["h"] // 2, _ic["w"], _ic["h"]],
                "src": _ic["src"], "color": "#FFFFFFFF", "need_bind": "mkmenu1", **HIDE_OFF_MAP}
        if "tint" in _ic:
            _img["tint"] = _ic["tint"]
        if "src_rect" in _ic:
            _img["src_rect"] = _ic["src_rect"]
        dive.append(_img)
for _k in _TBITS:
    derived += [ex(f"mkm{_k}on", f"mkmenu1 && mk.f{_k} != 0"), ex(f"mkm{_k}off", f"mkmenu1 && mk.f{_k} == 0")]
_rd = [_PX + 10, _PY0 + 10 + len(_ROWS) * _RH + 2, _PW - 20, _RH - 6]
for _w in seg(_rd, "DONE", "mk_menu", False):
    _w["need_bind"] = "mkmenu1"; _w.update(HIDE_OFF_MAP)
    if _w.get("type") == "label":
        _w["align"] = "center"; _w["rect"] = [_rd[0] + _rd[2] // 2, _rd[1] + _rd[3] // 2 - 14 - 4 * TEXT_BUMP // 2, 0, 0]
    dive.append(_w)

# under the compact cards: what the marks mean (and DIAG when that page is on)
LX, LS = RX + 12, 30
LEGY -= 14
# 0.37.1: the pinned recipes' ingredients, one per line: picture, have / need (have = storage + this
# dive; cyan when enough, red when short) and the name. Two pinned recipes sharing one share a line.
_PY, _PL = LEGY + 8 + 34, 42       # 0.37.2: more room under the cargo card, bigger lines (0.37.20: under HUNT)
_PN = min(8, (916 - 6 - _PY) // _PL)   # 0.38.26: to the card's foot (FILTER moved onto the map)
_hunt0 = len(dive)
# 0.37.73: hunt.on -- a pinned recipe or a pinned EcoWatcher mission (its goals join the list and the fish marks)
dive += [label(RX + 12, _PY - 30, "HUNT", GOLD, 3, need_bind="hunt.on", **HIDE_OFF_MAP),
         label(RX + 12, _PY + 14, "Pin a recipe on SUSHI to track it here", DIM, 1, need_bind="!hunt.on", **HIDE_OFF_MAP)]
for j in range(_PN):
    y, gk = _PY + j * _PL, {"need_bind": f"pl.on{j}", **HIDE_OFF_MAP}
    dive += [{"type": "rect", "rect": [RX, y - 2, RW, _PL - 2], "bg": "#00000000", "color": "#00000000", "frame": 0,
              "on_tap": f"pl_open_{j}", "noframe": True, **gk},     # tap: the recipe's panel on SUSHI (unpin there)
             {"type": "image", "rect": [RX + 6, y, 38, 38], "src_bind": f"pl.isrc{j}", "color": "#FFFFFFFF", **gk},
             value([RX + 98, y + 6, 0, 0], f"pl.have{j}", OK, 3, align="right", need_bind=f"pl.ok{j}", **HIDE_OFF_MAP),
             value([RX + 98, y + 6, 0, 0], f"pl.have{j}", CRIT, 3, align="right", need_bind=f"pl.short{j}", **HIDE_OFF_MAP),
             value([RX + 98, y + 6, 0, 0], f"pl.need{j}", DIM, 3, text="/", **gk),
             tlabel(RX + 150, y + 6, f"pl.nn{j}", TXT, 3, wrap_width=RW - 154, max_lines=1, **gk)]
VQ = {"need_bind": "map.vill"}
# 0.38.53: beside the page title (user): Credit on its left, Bei on its right, each with the game's own icon
# (UI_MermanVill_Trust_Icon_24 / the Bei coin -- drawn once they are in pictures.idx); the right column stays as it was
_TY = VMY - 78
# 0.38.55: Bei (coin, then amount) just left of the title, Credit (icon, then n/max) just right of it (user). The title
# spans about x 415..825; the Bei block is shifted right by bei.dx so its amount always ends near the title.
_BX = 596 - 7 * 23 - 52
_dx = {"x_bind": "bei.dx", "x_scale": 1}
dive += [{"type": "image", "rect": [_BX, _TY - 8, 44, 44], "src": "module:icon/" + BEI_ICON, "color": "#FFFFFFFF", "need_bind": "vbei", **_dx},
         {**tlabel(_BX + 52, _TY, "bei.txt", "#FFFFFFFF", 4, need_bind="vbei"), **_dx},   # 0.38.56: white, as the game
         {"type": "image", "rect": [650, _TY - 8, 44, 44], "src": "module:icon/UI_MermanVill_Trust_Icon_52", "color": "#FFFFFFFF", "need_bind": "vtr"},
         tlabel(702, _TY, "trust.txt", TXT, 4, need_bind="vtr")]
derived += [ex("vbei", "map.vill && gold.ok"), ex("vtr", "map.vill && trust.ok")]
# 0.38.26: the loadout (Reddit idea) under the map, across its width -- as the game's HUD slots: B the knife, A the
# harpoon, R the gun (pictures from the game's item table, read from the save's equipped slots); charms / sub-helpers
_LY = FTY + 10
_LH = 916 - 6 - _LY
# 0.38.27: from the DIVE tab's left edge to the map's right edge, no charms (user)
_LX0, _LX1 = 22, MX0 + MW
_LW = (_LX1 - _LX0 - 2 * 10) // 3
# 0.38.29: two cards (user): B the knife, A the weapon -- the harpoon tip by default; a tap shows the gun and presses R
# (the game's swap), a tap again the tip (lo.side, kept by the module)
# 0.38.31: three cards (user): B the knife, Y the item (the save's two item slots; a tap shows the other and presses L),
# A the weapon
# 0.38.34: the game's own HUD slots (UI_WP_*: B Dagger_Box, Y Item_Box 103x70, A Main_Box 147x70, each with its outline)
# 0.38.35: at the game's own shape (scaled to the strip's height), close together, the A card ending at the map's right
# edge (user). The weapon pictures fill a box inside the card (_LBOX; the module pads each picture to that box's shape).
_HUD = {"B": ("UI_WP_Dagger_Box", "UI_WP_Dagger_Stroke", 103), "Y": ("UI_WP_Item_Box", "UI_WP_Item_Stroke", 103),
        "A": ("UI_WP_Main_Box", "UI_WP_Main_Stroke", 147)}
_LGAP = 6
# 0.38.36: across the map's whole width (user): the B card's top-left corner on the map's left edge (its slant may reach
# past it), A ending at the map's right edge; widths keep the game's 103 : 103 : 147, the pictures keep their size
_LSL = 12                                          # the box's slant at the foot (the top-left corner sits this far in)
_x0 = MX0 - _LSL
_span = _LX1 - _x0 - 2 * _LGAP
_ws = {"B": round(_span * 103 / 353), "Y": round(_span * 103 / 353)}
_ws["A"] = _span - _ws["B"] - _ws["Y"]
x = _x0
_LBOX = {"B": (101, 56), "A": (157, 56)}           # the module's "@101x56" / "@157x56" (the pictures' box shape)
for n, btn in enumerate(("B", "Y", "A")):
    _box, _stroke, _gw = _HUD[btn]
    _LW = _ws[btn]
    g = {"need_bind": "lo_show", **HIDE_OFF_MAP}
    if btn != "B":
        dive.append({"type": "rect", "rect": [x, _LY, _LW, _LH], "bg": "#00000000", "color": "#00000000", "frame": 0,
                     "on_tap": "lo_swap" if btn == "A" else "lo_item", "noframe": True, **g})
    dive.append({**game_frame([x, _LY, _LW, _LH], _box, x2=False), **g})
    if btn == "Y":
        # loadout1: slightly enlarge the existing 84x48 item canvas (about 10%).
        _r = [x + (_LW - 139) // 2, _LY + 2, 139, 79]
        for key, gate in (("lo.s1", "lo_y0"), ("lo.s2", "lo_y1")):
            dive.append({"type": "image", "rect": _r, "src_bind": key, "color": "#FFFFFFFF", "need_bind": gate, **HIDE_OFF_MAP})
        # 0.38.49: no "NO ITEM" text; an empty slot is simply an empty card (user)
        # 0.38.40: the item's count (the HUD's xN) at the foot, right, as the gun's ammo
        for key, gate in (("lo.sn1", "lo_yc0"), ("lo.sn2", "lo_yc1")):
            dive.append(tlabel(x + _LW - 30, _LY + _LH - 30, key, "#FFFFFFFF", 2, align="right", outline="#FF10263E",
                               outline_px=2, need_bind=gate, **HIDE_OFF_MAP))
    else:
        bw, bh = _LBOX[btn]
        # loadout1: enlarge the existing padded canvas uniformly; no sprite stretching.
        iw, ih = round(bw * 1.4), round(bh * 1.4)
        _r = [x + (_LW - iw) // 2, _LY + 1, iw, ih]
        _pics = [("lo.k", "lo_k")] if btn == "B" else [("lo.hh", "lo_w0"), ("lo.g", "lo_w1")]
        for key, gate in _pics:
            dive.append({"type": "image", "rect": _r, "src_bind": key, "color": "#FFFFFFFF", "need_bind": gate, **HIDE_OFF_MAP})
    dive.append({**game_frame([x + 1, _LY + 2, _LW - 2, _LH - 4], _stroke, x2=False), **g})
    # loadout2: plain outlined letters, drawn AFTER the card artwork and strokes.
    dive.append({**label(x + 42, _LY + 12, btn, "#FFFFFFFF", 3, align="center",
                         outline="#FF10263E", outline_px=1), **g})
    if btn == "A":
        # loadout3: count only, no badge; visible only when the gun is displayed.
        ag = {"need_bind": "lo_ammo", **HIDE_OFF_MAP}
        dive.append(tlabel(x + _LW - 30, _LY + _LH - 30, "lo.ammo", "#FFFFFFFF", 2,
                           align="right", outline="#FF10263E", outline_px=2, **ag))
        # 0.38.38: the level at the foot, left (the HUD's "Lv.N"): the tip's with the harpoon out, the gun's with the gun
        for key, gate in (("lo.lvt", "lo_lvt"), ("lo.lvg", "lo_lvg")):
            dive.append(tlabel(x + 40, _LY + _LH - 30, key, "#FFFFFFFF", 2, outline="#FF10263E", outline_px=2,
                               need_bind=gate, **HIDE_OFF_MAP))
    x += _LW + _LGAP
# item card: the second item only when there is one (else the first stays)
# 0.38.49: with one item, a tap shows the empty second slot (so you can tell which one is in hand with the HUD off)
derived += [ex("lo_y1s", "lo_show && lo.yside != 0 && lo.s2on"), ex("lo_y0s", "lo_show && lo.yside == 0 && lo.s1on"),
            ex("lo_y0", "lo_y0s"), ex("lo_y1", "lo_y1s"), ex("lo_ynone", "lo_show && !lo.s1on && !lo.s2on"),
            ex("lo_yboth", "lo_show && lo.s1on && lo.s2on")]
derived += [ex("lo_w0s", "lo_show && lo.side == 0"), ex("lo_w1s", "lo_show && lo.side != 0"),
            ex("lo_w0", "lo_w0s && lo.hhon"), ex("lo_w1", "lo_w1s && lo.gon"),
            ex("lo_ammo", "lo_w1 && lo.ammoon"),
            ex("lo_lvt", "lo_w0 && lo.lvton"), ex("lo_lvg", "lo_w1 && lo.lvgon"),
            ex("lo_yc0", "lo_y0 && lo.sn1on"), ex("lo_yc1", "lo_y1 && lo.sn2on")]
derived += [ex("lo_show", "in_dive && !scene.village")] + [ex(f"lo_{k}", f"in_dive && !scene.village && lo.{k}on") for k in ("k", "h", "hh", "g", "c1", "c2", "c3", "s1", "s2")]
if SHOW_DIAG:
    dive += game_button([MX0 + 6, FTY - 60, 110, 50], "DIAG", "to_diag", kind="grey2", gate="dive.map", sc=2)   # 0.38.7: also in the village
# 0.37.4: the FILTER list goes over everything else on the map page (the ingredient lines included)
_pop = [w for w in dive if w.get("need_bind") in ("mkmenu1", "mkallon", "mkalloff") or str(w.get("need_bind", "")).startswith("mkm") and str(w.get("need_bind", "")).endswith(("on", "off")) and w.get("need_bind") not in ("mkmenu0",)]
dive[:] = [w for w in dive if not any(w is q for q in _pop)] + _pop
# 0.37.6: a mission row just completed: a box in the middle of the screen (like the discard prompt) for a
# few seconds -- the game's check mark, DONE! and the row's own text from the mission tracker
# (0.37.8: narrower, no check mark, and the fill has the frame's cut corners -- no square box behind it)
_TW2, _TH2 = 600, 170
_TX2, _TY2 = (1240 - _TW2) // 2, (1080 - _TH2) // 2 - 40
TQ = {"need_bind": "toast.on"}
dive += [game_frame([_TX2, _TY2, _TW2, _TH2], "UI_EdgeFrame", tint="#FF0B1B2A", **TQ),
         game_frame([_TX2, _TY2, _TW2, _TH2], "UI_EdgeFrame_Stroke", tint=DONE, **TQ),
         {**label(620, _TY2 + 30, "DONE!", DONE, 4, align="center"), "need_bind": "toast_task"},
         # 0.38.40: the row that finished the whole mission -> MISSION COMPLETE and the mission's own title (user)
         {**label(620, _TY2 + 18, "MISSION COMPLETE!", GOLD, 4, align="center"), "need_bind": "toast_full"}]
derived += [ex("toast_task", "toast.on && !toast.full"), ex("toast_full", "toast.on && toast.full")]
for _a in range(MAX_TASKS):
    derived.append(ex(f"toastm{_a}", f"toast_full && toast.m == {_a}"))
    dive.append(tlabel(620, _TY2 + 64, f"t{_a}", TXT, 3, align="center", wrap_width=_TW2 - 50, max_lines=1, need_bind=f"toastm{_a}"))
for _a in range(MAX_TASKS):
    for _b in range(MAX_COND):
        _k = _a * MAX_COND + _b          # the module's toast.k (mission * 6 + row)
        derived.append(ex(f"toast{_k}", f"toast.on && toast.k == {_k}"))
        dive.append(tlabel(620, _TY2 + 100, f"c{_a}_{_b}", TXT, 3, align="center", wrap_width=_TW2 - 50, max_lines=1,
                           need_bind=f"toast{_k}"))
# 0.38.40: the catch pop-up (the game's is hidden by HideHUD): picture, name, rank, stars, weight, at the top of the
# page for 2.5 s; several queue up. Taps go through (no catcher).
# 0.38.43: the game's own catch plate (UI_Catch_Plate 290x141, drawn 2x) in the middle of the screen (user): the name on
# its orange band, the picture in the dark box below-left, rank + stars, the weight with the game's weight mark
_CW, _CH = 580, 282
_CX, _CY = 14, TOP + 8              # 0.38.50: from the left, right under the tabs (user)
CQ = {"need_bind": "ct.on"}
_INK = "#FF1C2633"
derived += [ex("ct_i", "floor(ct.wt / 10)"), ex("ct_d", "ct.wt % 10"), ex("ct_rk", "ct.on && ct.rank > 0"),
            ex("ct_st", "ct.on && ct.grade >= 0")]
# 0.38.44: matched to the game's plate (photo): the dark picture box over the plate's lower-left (DeadBG tinted dark),
# the name in dark brown on the band, "Rank N" + stars on the blue part's first line, the dark weight mark + kg below
_BOX = "#FF17213A"
# 0.38.45: rows moved into the plate's light-blue part (y 137..262 of 282 on device): Rank + stars at +150, the weight
# at +206; the red weight box dropped (the game shows only a small mark there)
_ct = [{"type": "image", "rect": [_CX, _CY, _CW, _CH], "src": "module:frame/UI_Catch_Plate", "color": "#FFFFFFFF", **CQ},
       tlabel(_CX + 30, _CY + 50, "ct.name", "#FF4A2A14", 5, wrap_width=_CW - 90, max_lines=1, **CQ),   # 0.38.48: centred in the band
       # 0.38.46: the dark box under the orange band only (it rose into the band), the picture centred in it
       # 0.38.48: no dark box -- the game shows only the picture on the plate's own blue (user); DeadBG is its dead-catch
       # backing, not the picture box
       {"type": "image", "rect": [_CX + 36, _CY + 142, 116, 116], "src_bind": "ct.isrc", "color": "#FFFFFFFF", **CQ},
       label(_CX + 200, _CY + 152, "Rank", "#FF3A6EA8", 4, **CQ),
       value([_CX + 296, _CY + 152, 0, 0], "ct.rank", "#FF3A6EA8", 4, need_bind="ct_rk"),
       {"type": "pips", "rect": [_CX + 344, _CY + 150, 34, 34], "src": "module:icon/StarIcon_Big", "bind": "ct.grade",
        "max": 3, "gap": 4, "color": "#FFFFFFFF", "bg": "#FF8A9AAE", "need_bind": "ct_st"},
       value([_CX + 250, _CY + 214, 0, 0], "ct_i", _INK, 5, align="right", **CQ),   # 0.38.47: lower, centred in its strip
       value([_CX + 250, _CY + 214, 0, 0], "ct_d", _INK, 5, text=".", suffix=" kg", **CQ)]
# 0.38.50: the plate slides in from the left when it appears (one group: every piece moves together)
for _w in _ct:
    _w["anim"] = {"bind": "ct.on", "group": "ctplate", "from": "left", "ms": 220, "box": [0, _CY, 1240, _CH]}
dive += _ct
dive += pin_prompt   # last, so it sits over the missions and pinned cards

# CARGO page: one row per catch, like the game's Acquired list (native module, native/modules/dtd).
# Rows are tall for easy tapping and scroll by drag; SORT orders them (the module keeps the game's
# own order for discarding through the menu).
CARGO_MAX = 40                             # the module's kMaxRows
ROW_H, ROW_GAP = 76, 8
SORTS = ["NEWEST", "WEIGHT", "NAME", "QUALITY"]
cg = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *dive_tabs("cargo"),
      label(40, 110, "Module not loaded", CRIT, 3, need_bind="!cargo.alive"),
      tlabel(40, 150, "module_error_message", CRIT, 2, wrap_width=1160, max_lines=2, need_bind="!cargo.alive"),
      # 0.37.0: the weight sits at the end of the SORT row (the header holds DIVE's tabs now)
      value([1110, TOP + 6, 0, 0], "w_i", TXT, 3, align="right", need_bind="in_dive"),
      value([1110, TOP + 6, 0, 0], "w_d", TXT, 3, text=".", need_bind="in_dive"),
      value([1150, TOP + 38, 0, 0], "wm_i", "#FFB8D4EC", 2, text="/ ", align="right", need_bind="in_dive"),
      value([1150, TOP + 38, 0, 0], "wm_d", "#FFB8D4EC", 2, text=".", suffix="kg", need_bind="in_dive"),
      label(40, 500, "Nothing in the cargo box", DIM, 4, need_bind="cargo.empty"),
      label(30, TOP + 20, "SORT", DIM, 3)]
derived += [ex("cargo.empty", "cargo.alive && cargo.ready && row.n == 0")]
for k, name in enumerate(SORTS):
    derived.append(ex(f"csort{k}", f"cargo.sort == {k}"))
    cg += seg([130 + k * 220, TOP + 4, 210, 58], name, f"cargo_sort_{k}", False, gate=f"csort{k}")
# (0.37.4: the "tap a catch to discard it" hint is gone)
LIST_TOP = TOP + 88
rows = {"repeat": CARGO_MAX, "repeat_bind": "row.n", "repeat_dy": ROW_H + ROW_GAP, "scroll": "cargo"}
for i in range(CARGO_MAX):
    derived += [ex(f"rw_i{i}", f"floor(row.wt{i} / 10)"), ex(f"rw_d{i}", f"row.wt{i} % 10"),
                ex(f"rk{i}", f"row.info{i} && row.rank{i} > 0"), ex(f"rkn{i}", f"row.info{i} && row.rank{i} <= 0"),
                ex(f"st{i}", f"row.on{i} && row.grade{i} >= 0"), ex(f"cv{i}", f"row.on{i} && row.carve{i} > 0"),
                ex(f"noinfo{i}", f"row.on{i} && !row.info{i}"), ex(f"dsel{i}", f"dc.picked && dc.pick == {i}")]
yy, g = LIST_TOP, {"need_bind": "row.on{i}", **rows}
cg += [{**rect([26, yy, 1174, ROW_H], CARD, **g), "on_tap": "dc_pick", "payload": "{i}"},   # tap a catch to discard it
       {**game_frame([26, yy, 1174, ROW_H], "UI_EdgeFrame_Stroke", tint=RED_C), "need_bind": "dsel{i}", **rows},
       # the game's own picture of the catch (read from the game files)
       {"type": "image", "rect": [36, yy + 6, 64, 64], "src_bind": "row.isrc{i}", "color": "#FFFFFFFF", **g},
       label(194, yy + 28, "RANK", DIM, 2, align="right", **g),
       value([214, yy + 22, 0, 0], "row.rank{i}", ACC, 4, need_bind="rk{i}", **rows),
       label(214, yy + 22, "-", ACC, 4, need_bind="rkn{i}", **rows),
       tlabel(262, yy + 22, "row.name{i}", TXT, 4, wrap_width=480, max_lines=1, need_bind="row.info{i}", **rows),
       value([262, yy + 22, 0, 0], "row.id{i}", DIM, 4, text="item ", need_bind="noinfo{i}", **rows),
       {"type": "pips", "rect": [760, yy + 20, 36, 36], "src": "module:icon/StarIcon_Big", "bind": "row.grade{i}",
        "max": 3, "gap": 6, "color": "#FFFFFFFF", "bg": "#FF46505E", "need_bind": "st{i}", **rows},
       value([960, yy + 22, 0, 0], "row.carve{i}", PINK, 4, text="x", align="right", need_bind="cv{i}", **rows),
       value([1100, yy + 22, 0, 0], "rw_i{i}", TXT, 4, align="right", need_bind="row.info{i}", **rows),
       value([1100, yy + 22, 0, 0], "rw_d{i}", TXT, 4, text=".", suffix="kg", need_bind="row.info{i}", **rows)]
cargo_scroll = {"id": "cargo", "rect": [26, LIST_TOP, 1190, BOTTOM - LIST_TOP], "count_bind": "row.n",
                "row_h": ROW_H + ROW_GAP, "cols": 1, "pad": 8, "bar": ACC, "bar_track": "#FF16334A", "bar_w": 8}
# Discard: tap a row, then DISCARD. With the game patch the catch is removed straight from the
# cargo; without it the companion opens the game's menu and presses the buttons itself.
derived += [ex("dc_hint", "row.n > 0 && !dc.picked && !dc.busy && !dc.has_status"),
            ex("dc_show", "!dc.picked && dc.has_status"),
            ex("dcm_menu", "dc.picked && !dc.direct"), ex("dcm_direct", "dc.picked && dc.direct")]
# confirm prompt: in the middle of the screen, built like the DIVE page's unpin prompt
DQ = {"need_bind": "dc.picked"}
DPY = (1080 - 300) // 2
cg += [{**rect([0, 0, 1240, 1080], "#B0000000", **DQ), "on_tap": "dc_cancel"},
       {**rect([120, DPY, 1000, 300], "#FF0B1B2A", **DQ), "on_tap": "noop"},
       game_frame([120, DPY, 1000, 300], "UI_EdgeFrame_Stroke", tint=RED_C, **DQ),
       # the game's own picture of the catch
       {"type": "image", "rect": [150, DPY + 30, 80, 80], "src_bind": "dc.pick_isrc", "color": "#FFFFFFFF", "need_bind": "dc.has_icon"},
       tlabel(250, DPY + 56, "dc.pick_name", TXT, 4, wrap_width=840, max_lines=1, **DQ),
       *game_button([150, DPY + 160, 440, 100], "DISCARD", "dc_go", gate="dc.picked"),
       *game_button([650, DPY + 160, 440, 100], "CANCEL", "dc_cancel", kind="grey", gate="dc.picked"),
       {**rect([26, BOTTOM - 120, 1188, 120], "#F80A1A28", need_bind="dc_show"), "on_tap": "noop"},
       tlabel(46, BOTTOM - 100, "dc.status", GOLD, 4, wrap_width=1150, max_lines=2, need_bind="dc_show")]

# SUSHI pages (native module): RECIPES -- unlocked recipes with what each needs vs. what is in
# storage (tonight's menu first) -- and INGREDIENTS -- the sushi bar's ingredient storage.
# Both sit under the SUSHI tab; the header switches between them.
def sushi_switch(active):
    return (seg([24, 22, 180, 62], "RECIPES", "to_sushi", active == "rec") +
            seg([214, 22, 274, 62], "INGREDIENTS", "to_ingredients", active == "ing"))

REC_COLS, REC_MAX, NEEDS = 2, 120, 4      # REC_MAX matches the module's kMaxRecipes
REC_W, REC_H, REC_GAP = 580, 200, 10
REC_TOP, REC_BOTTOM = TOP + 76, BOTTOM   # 0.37.20: the SORT row above the list
su = [rect([0, 0, 1240, 1080], BG), *header(plain=True, dave=False), *sushi_switch("rec"),
      label(40, 110, "Module not loaded", CRIT, 3, need_bind="!cargo.alive"),
      label(40, 500, "No recipes read yet - open after loading a save", DIM, 3, need_bind="rec.none")]
derived += [ex("rec.none", "cargo.alive && !rec.ready")]
# 0.37.20: SORT, as on CARGO -- the module orders the list (MENU = tonight's menu first, then servings)
REC_SORTS = ["MENU", "PRICE", "AMOUNT", "NAME"]
su += [label(30, TOP + 20, "SORT", DIM, 3)]
for k, name in enumerate(REC_SORTS):
    derived.append(ex(f"rsort{k}", f"rec.sort == {k}"))
    su += seg([130 + k * 220, TOP + 4, 210, 58], name, f"rec_sort_{k}", False, gate=f"rsort{k}")
# every unlocked recipe, two cards per row; drag the list to scroll (only the visible rows are built)
rgrid = {"repeat": REC_MAX, "repeat_bind": "rec.n", "repeat_cols": REC_COLS, "repeat_dx": REC_W + REC_GAP,
         "repeat_row_dy": REC_H + REC_GAP, "scroll": "rec"}
x, yy, g = 30, REC_TOP, {"need_bind": "rec.on{i}", **rgrid}
# like the game's recipe cell: the dish on a light square, the details on a darker panel beside it
RL = 200
MENU_PINK = "#FFEE3F8C"
derived += [ex(f"rec.offmenu{i}", f"!rec.menu{i}") for i in range(REC_MAX)]
su += [{"type": "rect", "rect": [x, yy, REC_W, REC_H], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "rec_pick", "payload": "{i}", "noframe": True, **g},
       game_frame([x + RL - 8, yy, REC_W - RL + 8, REC_H], "UI_Sushi_Recipe_Menu_box", tint="#FF8C8C8C", **g),
       game_frame([x, yy, RL, REC_H], "UI_Sushi_Recipe_Menu_box", tint="#FFE6E6E6", **g),
       # the dish, as the game draws it on its menu (read from the game files)
       {"type": "image", "rect": [x + 23, yy + 48, 154, 108], "src_bind": "rec.isrc{i}", "color": "#FFFFFFFF", **g},
       label(x + RL // 2, yy + 16, "ON THE MENU", INK, 1, align="center", need_bind="rec.menu{i}", **rgrid),
       label(x + RL // 2, yy + REC_H - 30, "PINNED", INK, 2, align="center", need_bind="rec.pinned{i}", **rgrid),
       tlabel(x + RL + 14, yy + 16, "rec.name{i}", GOLD, 3, wrap_width=REC_W - RL - 30, max_lines=2, need_bind="rec.offmenu{i}", **rgrid),
       # on tonight's menu: the name on the menu's pink brush stroke, as the game shows it
       game_frame([x + RL + 4, yy + 8, REC_W - RL - 12, 60], "UI_Sushi_MenuTitleBg", x2=False, tint=MENU_PINK, need_bind="rec.menu{i}", **rgrid),
       tlabel(x + RL + 24, yy + 24, "rec.name{i}", "#FFFFFFFF", 3, wrap_width=REC_W - RL - 50, max_lines=1,
              outline=BTN_SHADE, outline_px=2, need_bind="rec.menu{i}", **rgrid),
       # the price of a plate (RecipeEntity.Price) with the game's coin
       {"type": "image", "rect": [x + RL + 14, yy + 100, 36, 36], "src": "module:icon/Coin32", "color": "#FFFFFFFF",
        "need_bind": "rec.hasprice{i}", **rgrid},
       value([x + RL + 58, yy + 102, 0, 0], "rec.price{i}", GOLD, 4, need_bind="rec.hasprice{i}", **rgrid),
       value([x + RL + 14, yy + REC_H - 40, 0, 0], "rec.can{i}", OK, 3, text="can make ", need_bind="rec.cando{i}", **rgrid),
       label(x + RL + 14, yy + REC_H - 40, "cannot make", CRIT, 3, need_bind="rec.cannot{i}", **rgrid)]
rec_scroll = {"id": "rec", "rect": [30, REC_TOP, REC_COLS * (REC_W + REC_GAP) + 6, REC_BOTTOM - REC_TOP], "count_bind": "rec.n",
              "row_h": REC_H + REC_GAP, "cols": REC_COLS, "pad": 10, "bar": ACC, "bar_track": "#FF16334A", "bar_w": 8}

# the panel for one recipe (tap a card): what it needs, and PIN TO DIVE with a target in servings
PX0, PY0, PW, PH = 50, TOP + 20, 1140, BOTTOM - TOP - 40
S = {"need_bind": "recsel.on"}
su += [{**rect([PX0 + 8, PY0 + 8, PW - 16, PH - 16], "#FF2A1F1B", **S), "on_tap": "noop"},
       game_frame([PX0, PY0, PW, PH], "UI_Sushi_Pop_Panel", **S),
       {"type": "image", "rect": [PX0 + 30, PY0 + 24, 120, 120], "src_bind": "recsel.isrc", "color": "#FFFFFFFF", **S},
       tlabel(PX0 + 176, PY0 + 34, "recsel.name", GOLD, 4, wrap_width=700, max_lines=1, need_bind="recsel_off"),
       game_frame([PX0 + 160, PY0 + 18, 620, 66], "UI_Sushi_MenuTitleBg", x2=False, tint=MENU_PINK, need_bind="recsel.menu"),
       tlabel(PX0 + 186, PY0 + 34, "recsel.name", "#FFFFFFFF", 4, wrap_width=560, max_lines=1, outline=BTN_SHADE, outline_px=2,
              need_bind="recsel.menu"),
       value([PX0 + 176, PY0 + 92, 0, 0], "recsel.can", OK, 3, text="can make ", need_bind="recsel.cando"),
       label(PX0 + 176, PY0 + 92, "cannot make", CRIT, 3, need_bind="recsel.cannot"),
       {"type": "image", "rect": [PX0 + 560, PY0 + 86, 32, 32], "src": "module:icon/Coin32", "color": "#FFFFFFFF", "need_bind": "recsel.hasprice"},
       value([PX0 + 602, PY0 + 92, 0, 0], "recsel.price", GOLD, 3, suffix=" a plate", need_bind="recsel.hasprice"),
       label(PX0 + 176, PY0 + 130, "ON THE MENU TONIGHT", ACC, 2, need_bind="recsel.menu"),
       # CLOSE (the game's brown Close) and REMOVE (its grey Delete Menu), stacked at the top right
       *game_button([PX0 + PW - 260, PY0 + 22, 230, 88], "CLOSE", "rec_close", kind="brown", gate="recsel.on", sc=3),
       label(PX0 + 30, PY0 + 180, "NEEDS - IN STORAGE", DIM, 3, **S)]
for k in range(NEEDS):
    ry = PY0 + 236 + k * 64
    r = {"need_bind": f"recsel.on{k}"}
    su += [{"type": "image", "rect": [PX0 + 30, ry - 12, 48, 48], "src_bind": f"recsel.nisrc{k}", "color": "#FFFFFFFF", **r},
           tlabel(PX0 + 96, ry, f"recsel.nn{k}", TXT, 3, wrap_width=700, max_lines=1, **r),
           value([PX0 + PW - 140, ry, 0, 0], f"recsel.have{k}", OK, 3, align="right", need_bind=f"recsel.ok{k}"),
           value([PX0 + PW - 140, ry, 0, 0], f"recsel.have{k}", CRIT, 3, align="right", need_bind=f"recsel.short{k}"),
           value([PX0 + PW - 140, ry, 0, 0], f"recsel.qty{k}", DIM, 3, text=" / ", **r)]
# ADD TO MENU (round 22): the game's own AddMenu, called at the sushi bar through the load plan
AY = PY0 + 470
derived += [ex("recsel_off", "recsel.on && !recsel.menu"), ex("menu_off", "recsel.on && !menu.ready"), ex("menu_why_bar", "recsel.on && !menu.live && !tut.on"), ex("menu_why_tut", "recsel.on && tut.on"),
            ex("menu_why_busy", "recsel.on && menu.live && menu.busy"),
            ex("menu_why_short", "recsel.on && menu.live && !menu.busy && recsel.can <= 0"),
            ex("menu_msg", "recsel.on && menu.has_status")]
# the game's pink OK pill (UI_Sushi_Btn_Pink64, 2x: 88 px high); the dark pill when it cannot be done now
BTN_H, DARK_T = 88, "#FF2E2A28"
su += [*game_button([PX0 + 30, AY + 6, 460, BTN_H], "ADD TO MENU", "menu_add", kind="ok", gate="menu.ready"),
       *game_button([PX0 + 30, AY + 6, 460, BTN_H], "ADD TO MENU", "noop", kind="grey2", gate="menu_off", sub=" "),
       label(PX0 + 260, AY + 6 + 51, "at the sushi bar only", DARK_T, 2, align="center", need_bind="menu_why_bar"),
       # 0.38.52: during Bancho's tutorial the game's own menu must be used (round 66; a bottom-screen add soft-locked it)
       label(PX0 + 260, AY + 6 + 51, "finish the sushi tutorial first", DARK_T, 2, align="center", need_bind="menu_why_tut"),
       label(PX0 + 260, AY + 6 + 51, "adding...", DARK_T, 2, align="center", need_bind="menu_why_busy"),
       label(PX0 + 260, AY + 6 + 51, "not enough ingredients", DARK_T, 2, align="center", need_bind="menu_why_short"),
       label(PX0 + 540, AY - 6, "PLATES", SDIM, 2, **S),
       *stepper(PX0 + 520, AY + 24, "menu.amt", "menu_dec", "menu_inc", "recsel.on"),
       tlabel(PX0 + 30, AY + 114, "menu.status", GOLD, 2, wrap_width=1060, max_lines=1, need_bind="menu_msg"),
       # SWAP: put this dish in place of one already on tonight's menu (the game's own Swap)
       *game_button([PX0 + 940, AY + 16, 170, 88], "SWAP", "menu_swap_open", kind="grey2", gate="menu.can_swap", sc=3),
       # REMOVE: take it off the menu (the game's own Delete Menu); asks once more before doing it
       *game_button([PX0 + PW - 260, PY0 + 120, 230, 88], "REMOVE", "menu_remove", kind="grey2", gate="menu.rm_plain", sc=3),
       *game_button([PX0 + PW - 260, PY0 + 120, 230, 88], "TAP AGAIN", "menu_remove", kind="grey2", gate="menu.rm_armed", sc=3,
                    sub="to remove", tint="#FFFF7A7A", col="#FF5A0E16")]
BY = PY0 + PH - 150                        # pin row along the bottom of the panel
derived += [ex("recsel_pin", "recsel.on && recsel.pinned"), ex("recsel_unpin", "recsel.on && !recsel.pinned && !pin.on"),
            ex("recsel_repl", "recsel.on && !recsel.pinned && pin.on")]
su += [*game_button([PX0 + 30, BY + 6, 460, BTN_H], "PIN TO DIVE", "rec_pin", kind="ok", gate="recsel_unpin"),
       *game_button([PX0 + 30, BY + 6, 460, BTN_H], "PINNED TO DIVE", "rec_pin", kind="grey2", gate="recsel_pin", sub="tap to unpin"),
       *game_button([PX0 + 30, BY + 6, 460, BTN_H], "PIN TO DIVE", "rec_pin", kind="ok", gate="recsel_repl", sub="replaces the pinned recipe"),
       label(PX0 + 540, BY - 6, "TARGET SERVINGS", SDIM, 2, **S),
       *stepper(PX0 + 520, BY + 24, "pin.target", "pin_dec", "pin_inc", "recsel.on")]
# the SWAP chooser: over the ADD and pin rows, the dishes on tonight's menu with their pictures
SW = {"need_bind": "menu.swap_open"}
SY = AY - 14
su += [{**rect([PX0 + 12, SY, PW - 24, PY0 + PH - SY - 12], "#FF061420", **SW), "on_tap": "noop"},
       {"type": "rect", "rect": [PX0 + 12, SY, PW - 24, PY0 + PH - SY - 12], "bg": "#00000000", "color": GOLD, "frame": 3, **SW},
       label(PX0 + 36, SY + 30, "SWAP FOR WHICH DISH?", GOLD, 3, **SW),
       *game_button([PX0 + PW - 290, SY + 8, 250, 88], "CANCEL", "menu_swap_close", kind="brown", gate="menu.swap_open", sc=3)]
# one dish per row, full width; more than fit scroll (drag the list)
SWL_Y, SWL_H, SW_ROW = SY + 100, PY0 + PH - SY - 12 - 100 - 8, 106
derived.append(ex("swap_n", "menu.swap_open ? menu.sn : 0"))
srow = {"repeat": 8, "repeat_bind": "swap_n", "repeat_dy": SW_ROW, "scroll": "swap"}
su += [{"type": "rect", "rect": [PX0 + 30, SWL_Y, PW - 84, SW_ROW - 10], "bg": TAB_OFF_BG, "color": ACC, "frame": 3,
        "on_tap": "menu_swap", "payload": "{i}", **srow},
       {"type": "image", "rect": [PX0 + 44, SWL_Y + 8, 80, 80], "src_bind": "menu.sisrc{i}", "color": "#FFFFFFFF", **srow},
       tlabel(PX0 + 144, SWL_Y + 34, "menu.sname{i}", TXT, 3, wrap_width=700, max_lines=1, **srow),
       value([PX0 + PW - 80, SWL_Y + 36, 0, 0], "menu.splates{i}", GOLD, 3, text="plates ", align="right", **srow)]
# Added to the menu: the game's pink brush banner with the dish's name, for a moment (menu.splash)
SPL = {"need_bind": "menu.splash"}
SPY = 440
su += [game_frame([110, SPY, 1020, 150], "UI_Sushi_MenuTitleBg", tint="#FFEE3F8C", **SPL),
       label(620, SPY + 24, "ON THE MENU TONIGHT", "#FFFFE3F0", 2, align="center", **SPL),
       tlabel(620, SPY + 62, "menu.splash_name", "#FFFFFFFF", 6, align="center", wrap_width=860, max_lines=1, **SPL)]
swap_scroll = {"id": "swap", "rect": [PX0 + 24, SWL_Y, PW - 48, SWL_H], "count_bind": "swap_n", "show_bind": "menu.swap_open",
               "row_h": SW_ROW, "cols": 1, "pad": 0, "bar": GOLD, "bar_track": "#FF16334A", "bar_w": 8}

# INGREDIENTS: a grid of boxes like the game's storage (picture, rank, count), three across so the
# names fit. IN STOCK / ALL filter it; tap a box for the recipes that use that ingredient.
ING_COLS, ING_MAX = 3, 200          # ING_MAX matches the module's kMaxIngredients
TILE_W, TILE_H, TILE_GAP = 384, 160, 10
ING_TOP, ING_BOTTOM = TOP, BOTTOM
ING_DIM_TINT = "#FF505050"          # empty stacks: the picture darkened
ig = [rect([0, 0, 1240, 1080], BG), *header(plain=True, money=False, dave=False), *sushi_switch("ing"),
      label(40, 110, "Module not loaded", CRIT, 3, need_bind="!cargo.alive"),
      label(40, 500, "Ingredient storage not read yet - open after loading a save", DIM, 3, need_bind="ing.none")]
derived += [ex("ing.none", "cargo.alive && !ing.ready"), ex("ingf1", "ing.filter == 1"), ex("ingf0", "ing.filter == 0")]
# 0.37.23: one ALL toggle (lit = every stack, dark = only what is in stock)
# 0.37.118: no ALL toggle (every stack shows; the empty ones dimmed)
step = TILE_W + TILE_GAP
row_dy = TILE_H + TILE_GAP
# one template per tile part; the runtime builds only the rows inside the viewport
grid = {"repeat": ING_MAX, "repeat_bind": "ing.n", "repeat_cols": ING_COLS, "repeat_dx": step,
        "repeat_row_dy": row_dy, "scroll": "ing"}
x, yy = 28, ING_TOP
F, E = {"need_bind": "ing.full{i}", **grid}, {"need_bind": "ing.empty{i}", **grid}
ig += [{**rect([x, yy, TILE_W, TILE_H], CARD, **F), "on_tap": "ing_pick", "payload": "{i}"},
       {**rect([x, yy, TILE_W, TILE_H], CARD_DIM, **E), "on_tap": "ing_pick", "payload": "{i}"},
       game_frame([x, yy, TILE_H, TILE_H], "UI_Sushi_item_Bg", **grid),
       {"type": "image", "rect": [x + 20, yy + 20, 120, 120], "src_bind": "ing.isrc{i}", "color": "#FFFFFFFF", **F},
       {"type": "image", "rect": [x + 20, yy + 20, 120, 120], "src_bind": "ing.isrc{i}", "color": ING_DIM_TINT, **E},
       tlabel(x + 174, yy + 14, "ing.name{i}", TXT, 3, wrap_width=TILE_W - 184, max_lines=2, **F),
       tlabel(x + 174, yy + 14, "ing.name{i}", SDIM_E, 3, wrap_width=TILE_W - 184, max_lines=2, **E),
       label(x + 252, yy + 112, "RANK", SDIM, 2, align="right", need_bind="ing.rkon{i}", **grid),
       value([x + 272, yy + 108, 0, 0], "ing.rk{i}", GOLD, 3, **F),
       value([x + 272, yy + 108, 0, 0], "ing.rk{i}", SDIM_E, 3, **E),
       value([x + TILE_W - 14, yy + 104, 0, 0], "ing.count{i}", TXT, 4, text="x", align="right", **F),
       value([x + TILE_W - 14, yy + 104, 0, 0], "ing.count{i}", SDIM_E, 4, text="x", align="right", **E)]
ing_scroll = {"id": "ing", "rect": [28, ING_TOP, ING_COLS * step + 14, ING_BOTTOM - ING_TOP], "count_bind": "ing.n",
              "row_h": row_dy, "cols": ING_COLS, "pad": 10, "bar": ACC, "bar_track": "#FF16334A", "bar_w": 8}
# the recipes that use the tapped ingredient (a panel over the grid; CLOSE or tap outside the list)
SEL = 8                              # the module's kSelRecipes
PX0, PY0, PW, PH = 50, TOP + 20, 1140, BOTTOM - TOP - 40
S = {"need_bind": "ingsel.on"}
ig += [{**rect([PX0 + 8, PY0 + 8, PW - 16, PH - 16], "#FF2A1F1B", **S), "on_tap": "noop"},
       game_frame([PX0, PY0, PW, PH], "UI_Sushi_Pop_Panel", **S),
       {"type": "image", "rect": [PX0 + 30, PY0 + 24, 120, 120], "src_bind": "ingsel.isrc", "color": "#FFFFFFFF", **S},
       tlabel(PX0 + 176, PY0 + 34, "ingsel.name", GOLD, 4, wrap_width=640, max_lines=1, **S),
       label(PX0 + 280, PY0 + 100, "RANK", DIM, 3, align="right", **S), value([PX0 + 302, PY0 + 96, 0, 0], "ingsel.rk", GOLD, 4, **S),
       value([PX0 + 420, PY0 + 96, 0, 0], "ingsel.count", TXT, 4, text="in storage x", **S),
       *game_button([PX0 + PW - 260, PY0 + 22, 230, 88], "CLOSE", "ing_close", kind="brown", gate="ingsel.on", sc=3),
       label(PX0 + 30, PY0 + 176, "USED IN", DIM, 3, **S),
       label(PX0 + 30, PY0 + 226, "No unlocked recipe uses it", DIM, 3, need_bind="ingsel.none")]
for k in range(SEL):
    ry = PY0 + 226 + k * 70
    r = {"need_bind": f"ingsel.ron{k}"}
    ig += [{"type": "image", "rect": [PX0 + 30, ry - 14, 60, 60], "src_bind": f"ingsel.risrc{k}", "color": "#FFFFFFFF", **r},
           tlabel(PX0 + 106, ry, f"ingsel.rname{k}", TXT, 3, wrap_width=640, max_lines=1, **r),
           value([PX0 + PW - 30, ry, 0, 0], f"ingsel.rcan{k}", OK, 3, text="can make ", align="right", need_bind=f"ingsel.rcando{k}"),
           label(PX0 + PW - 30, ry, "cannot make", CRIT, 3, align="right", need_bind=f"ingsel.rcannot{k}")]

# DISCARD TEST page (probe): is the game's Acquired list found, focused, and what does its
# "... is selected. Discard?" popup say. Used to build the discard button sequence safely.
dp = [rect([0, 0, 1240, 1080], BG), label(40, 30, "DISCARD TEST", ACC, 5),
      {"type": "button", "rect": [1040, 24, 170, 56], "text": "BACK", "on_tap": "to_debug", "color": DIM, "text_scale": 3},
      label(40, 120, "1. Dive, open the menu with the Acquired list.", DIM, 3),
      label(40, 160, "2. Move the cursor over a few rows; press A once, then B.", DIM, 3),
      label(40, 200, "3. Photograph this page at each step.", DIM, 3)]
yy = 280
for name, key in [("Search state (2 = found)", "ui.state"), ("Acquired list found", "ui.found"),
                  ("Heap searched (MB)", "ui.scan_mb"), ("Full passes", "ui.passes"), ("List focused", "ui.focused"),
                  ("Class matches", "ui.cands"), ("O2 unchanged (samples)", "dc.o2_still")]:
    dp += [label(60, yy, name, DIM, 3), value([700, yy, 0, 0], key, TXT, 3)]
    yy += 50
dp += [label(60, yy + 20, "Popup text:", DIM, 3), tlabel(60, yy + 60, "ui.confirm", GOLD, 3, wrap_width=1120, max_lines=2),
       label(60, yy + 160, "List numbers (cursor is one of these):", DIM, 3),
       tlabel(60, yy + 200, "ui.scroll", TXT, 2, wrap_width=1120, max_lines=2),
       label(60, yy + 270, "Search details:", DIM, 3),
       tlabel(60, yy + 310, "ui.cls", TXT, 2, wrap_width=1120, max_lines=1),
       tlabel(60, yy + 345, "ui.range", TXT, 2, wrap_width=1120, max_lines=2),
       tlabel(60, yy + 380, "ui.subs", GOLD, 2, wrap_width=1120, max_lines=1),
       tlabel(60, yy + 415, "ui.cand_info", TXT, 2, wrap_width=1120, max_lines=3)]
dbg += [{"type": "button", "rect": [700, 24, 320, 56], "text": "DISCARD TEST", "on_tap": "to_dtest", "color": DIM, "text_scale": 3},
        *([{"type": "button", "rect": [330, 24, 350, 56], "text": "DIAGNOSTICS", "on_tap": "to_diag", "color": DIM, "text_scale": 3}]
          if SHOW_DIAG else [])]

# DIAGNOSTICS page: the small status lines that used to sit under CARGO / SUSHI (for photos when
# something does not work)
dg = [rect([0, 0, 1240, 1080], BG), label(40, 30, "DIAGNOSTICS", ACC, 5),
      {"type": "button", "rect": [1040, 24, 170, 56], "text": "BACK", "on_tap": "to_dive", "color": DIM, "text_scale": 3},
      # 0.37.49: shows the loading picture by hand (tap it to close); the line: the loading signal (0.37.51) and the logo
      {"type": "button", "rect": [700, 24, 320, 56], "text": "TITLE TEST", "on_tap": "ld_test", "color": DIM, "text_scale": 3},
      label(40, 1030, "title", DIM, 2), tlabel(360, 1030, "ld.diag", TXT, 2, wrap_width=860, max_lines=2)]
yy = 110
# 0.37.65: round 42 -- the phone's open app (Mail probe) replaces the map probe lines (the map work is done)
# 0.38.1: the ammo lines back on top (the false ammo marks); the apps' lines off (the apps are done)
# 0.38.11 (tester build): the map's marks -- boxes (chests, O2, weapons), pods, ammo / pickups, the filters
for title, key in [("other missions", "om.diag"), ("cargo lift", "cg.lift"), ("hide hud", "hh.diag"), ("live loadout", "lv.diag"), ("items probe", "lv.diag2"), ("loadout", "lo.diag"), ("boxes", "mk.p11"), ("pods", "mk.p13"), ("ammo / pickups", "mk.p14"), ("village map", "vm.diag"),
                   ("pictures", "icon.diag")]:
    dg += [label(40, yy, title, DIM, 2),
           value([360, yy, 0, 0], key, TXT, 2) if key == "game.mode" else tlabel(360, yy, key, TXT, 2, wrap_width=840, max_lines=6 if key in ("vm.diag", "lo.diag", "lv.diag", "hh.diag", "cg.lift", "om.diag") else 7 if key in ("mk.p11", "mk.p13", "mk.p14", "mk.podd") else 4 if key in ("mk.p3", "mk.p11", "mk.p12", "mk.p14", "ing.diag", "eco.diag", "eco.mkdiag", "eco.pkdiag", "shop.trkdiag", "idv.diag", "fm.diag", "fm.diag2", "fm.diag3") else 2)]
    yy += 180 if key in ("vm.diag", "lo.diag", "lv.diag", "hh.diag", "cg.lift", "om.diag") else 200 if key in ("mk.p11", "mk.p13", "mk.p14", "mk.podd") else 130 if key in ("mk.p3", "mk.p11", "mk.p12", "mk.p14", "ing.diag", "eco.diag", "eco.mkdiag", "eco.pkdiag", "shop.trkdiag", "idv.diag", "fm.diag", "fm.diag2", "fm.diag3") else 72 if key.startswith("mk.") else 49

# ---- 0.37.62: PHONE -- the phone's apps. APPS (the app icons) and the CALCULATOR, laid out like the game's
# (its own key pictures: UI_Calculator_Icon_*, read from the player's game files). The arithmetic is the
# module's (calc_key); nothing in the game is read or changed.
def phone_tabs(active):
    """APPS, then the open app's own tab (0.37.67: CALCULATOR or MAIL)."""
    app = {"calc": ("CALCULATOR", "to_calc", 300), "mail": ("MAIL", "to_mail", 170),
           "eco": ("ECOWATCHER", "to_eco", 300), "shop": ("WEAPON SHOP", "to_shop", 320),
           "idv": ("IDIVER", "to_idv", 220), "call": ("CALL", "to_call", 170)}.get(active, ("CALCULATOR", "to_calc", 300))
    return (seg([108, 22, 160, 62], "APPS", "to_phone", active == "apps") +
            seg([278, 22, app[2], 62], app[0], app[1], active != "apps"))

# 0.37.64: APPS as the game's phone home screen shows them: the app's own icon with its name under it, only the
# apps the companion is getting (in the game's order). Built ones open; the rest are dimmed until they are added.
PHONE_APPS = [("AppIcon_Duff", "Weapon Shop", "to_shop"), ("AppIcon_iDiver", "iDiver", "to_idv"), ("AppIcon_Telephone", "Call", "to_call"),
              ("AppIcon_Message", "Mail", "to_mail"), ("AppIcon_Calculator", "Calculator", "to_calc"), ("AppIcon_Ecowatcher", "Eco watcher", "to_eco")]
ph = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar()]
for n, (icon, name, act) in enumerate(PHONE_APPS):
    cx, y = 207 + (n % 3) * 413, 176 + (n // 3) * 380   # 0.37.114: centred in the page (was 140)
    ph += [{"type": "image", "rect": [cx - 110, y, 220, 220], "src": f"module:icon/{icon}",
            "color": "#FFFFFFFF" if act else "#FF6A7480"},
           label(cx, y + 236, name, TXT if act else DIM, 4, align="center")]
    if act:
        ph.append({"type": "rect", "rect": [cx - 190, y - 10, 380, 320], "bg": "#00000000", "color": "#00000000", "frame": 0,
                   "on_tap": act, "noframe": True})
    else:
        ph.append(label(cx, y + 290, "COMING SOON", DIM, 2, align="center"))
# 0.38.39: HideHUD -- an optional switch: hides the top screen's O2 / depth panel, weapon slots, mission list and catch /
# pickup pop-ups (the game's objects scaled to 0 by the helper; no game files changed). Off when the game starts.
# 0.37.66: the Mail probe (round 42) on APPS -- DIAG is only reachable while diving, the phone only on land.
# Tap Mail: the panel lists what the game's Mail screen holds (open Mail on the top screen first); tap it to close.
PP = {"need_bind": "ph.show"}
ph += [{**rect([20, 100, 1200, 820], "#F8071521", **PP), "on_tap": "ph_probe", "noframe": True},
       label(50, 120, "MAIL PROBE  (open Mail on the top screen; tap here to close)", ACC, 3, **PP),
       tlabel(50, 190, "ph.diag", TXT, 3, wrap_width=1140, max_lines=2, **PP)]
for i in range(6):
    ph.append(tlabel(50, 300 + i * 100, f"ph.mail{i}", TXT, 3, wrap_width=1140, max_lines=2, **PP))

# 0.37.63: the game's calculator look (colours from the game's own screen): a light phone body, "HASIO", the olive
# display with the whole sum on one line, rounded keys -- light grey (digits, C, +/-, %), dark grey (operations),
# salmon (delete), orange (=); dark labels on the light keys, white signs on the dark ones.
CALC_BODY, CALC_LCD_RIM, CALC_LCD, CALC_LCD_TXT = "#FFE1E1E1", "#FFD5D5C6", "#FFB1B3A1", "#FF3F4636"
CALC_KEY, CALC_OPK, CALC_DEL, CALC_EQ, CALC_INK, CALC_BRAND = "#FFADA9A2", "#FF6A6662", "#FFD49986", "#FFDA6940", "#FF4D4A46", "#FFA8A49D"
CALC_KEYS = [  # (label or icon, key number, colour, sign colour) -- 4 columns, 5 rows, as on the game's screen
    [("C", 19, CALC_KEY, CALC_INK), ("UI_Calculator_Icon_PlusMinus", 17, CALC_KEY, CALC_INK),
     ("UI_Calculator_Icon_Percent", 16, CALC_KEY, CALC_INK), ("UI_Calculator_Icon_Division", 14, CALC_OPK, "#FFFFFFFF")],
    [("7", 7, CALC_KEY, CALC_INK), ("8", 8, CALC_KEY, CALC_INK), ("9", 9, CALC_KEY, CALC_INK), ("UI_Calculator_Icon_Multiply", 13, CALC_OPK, "#FFFFFFFF")],
    [("4", 4, CALC_KEY, CALC_INK), ("5", 5, CALC_KEY, CALC_INK), ("6", 6, CALC_KEY, CALC_INK), ("UI_Calculator_Icon_Minus", 12, CALC_OPK, "#FFFFFFFF")],
    [("1", 1, CALC_KEY, CALC_INK), ("2", 2, CALC_KEY, CALC_INK), ("3", 3, CALC_KEY, CALC_INK), ("UI_Calculator_Icon_Plus", 11, CALC_OPK, "#FFFFFFFF")],
    [("UI_Calculator_Icon_Del", 18, CALC_DEL, "#FFFFFFFF"), ("0", 0, CALC_KEY, CALC_INK), (".", 10, CALC_KEY, CALC_INK),
     ("UI_Calculator_Icon_Equals", 15, CALC_EQ, "#FFFFFFFF")]]
KX0, KY0, KW, KH, KGX, KGY = 56, 344, 270, 100, 16, 14
cc = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar(),
      game_frame([26, 108, 1188, 808], "Box_30rad", x2=False, tint=CALC_BODY),
      {**rect([26, 108, 1188, 808], "#00000000"), "on_tap": "noop", "noframe": True},
      label(66, 124, "HASIO", CALC_BRAND, 3),
      game_frame([56, 176, 1128, 150], "Box_4rad", x2=False, tint=CALC_LCD_RIM),
      game_frame([72, 190, 1096, 122], "Box_4rad", x2=False, tint=CALC_LCD),
      tlabel(1146, 214, "calc.line", CALC_LCD_TXT, 14, align="right", need_bind="!calc.long"),
      tlabel(1146, 228, "calc.line", CALC_LCD_TXT, 9, align="right", need_bind="calc.long")]
for r, row in enumerate(CALC_KEYS):
    for c, (t, k, col, ink) in enumerate(row):
        x, y = KX0 + c * (KW + KGX), KY0 + r * (KH + KGY)
        cc += [game_frame([x, y, KW, KH], "Roundsquare_24", x2=False, tint=col),
               {"type": "rect", "rect": [x, y, KW, KH], "bg": "#00000000", "color": "#00000000", "frame": 0,
                "on_tap": f"calc_{k}", "noframe": True}]
        if 11 <= k <= 14:   # the pending operation keeps a light outline
            cc.append(game_frame([x, y, KW, KH], "Roundsquare_24_Stroke", x2=False, tint="#FFF3E3B8", need_bind=f"calc_op{k - 10}"))
        if t.startswith("UI_"):
            cc.append({"type": "image", "rect": [x + KW // 2 - 34, y + KH // 2 - 34, 68, 68], "src": f"module:icon/{t}",
                       "color": "#FFFFFFFF", "tint": ink})
        else:
            cc.append(label(x + KW // 2, y + (KH - 50) // 2, t, ink, 10, align="center"))
derived += [ex(f"calc_op{k}", f"calc.op == {k}") for k in range(1, 5)]

# ---- 0.37.67: MAIL -- the game's Mail app, read from its own screen (DavePhoneConroller -> MailPanel): the list as the
# phone shows it, in its order, unread ones marked N and brighter (as the game greys the read ones), the phone's
# cursor row outlined. Text keys are looked up in the game's own text table. View only for now.
MAIL_ROW_H, MAIL_ROWS, MAIL_Y, MAIL_GAP = 92, 16, 116, 6   # 0.37.84: a scrolling list (drag it, as CARGO); 16 = kMailRows
MAIL_BOTTOM = 826
ML_BG, ML_ROW, ML_ROW_READ, ML_INK, ML_SUB, ML_CUR = "#FFE6E6EE", "#FFFFFFFF", "#FFF1F1F5", "#FF2E2E52", "#FF8A8AA0", "#FF5A5AE0"
ma = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar(),
      game_frame([26, 104, 1188, 812], "Box_30rad", x2=False, tint=ML_BG),
      {**rect([26, 104, 1188, 812], "#00000000"), "on_tap": "noop", "noframe": True},
      label(620, 420, "Open Mail on your phone once to load it here", ML_SUB, 3, align="center", need_bind="!mail.ok"),
      label(620, 470, "(on the boat or at the sushi bar)", ML_SUB, 2, align="center", need_bind="!mail.ok")]
mrows = {"repeat": MAIL_ROWS, "repeat_bind": "mail.rows", "repeat_dy": MAIL_ROW_H + MAIL_GAP, "scroll": "mail"}
y = MAIL_Y
g = {"need_bind": "mail.on{i}", **mrows}
# 0.37.70: the rows use the same rounded shape as the cursor's outline, so no corner sticks out past it
ma += [{**game_frame([44, y, 1144, MAIL_ROW_H], "Roundsquare_24", x2=False, tint=ML_ROW), "need_bind": "mail.new{i}", **mrows},
       {**game_frame([44, y, 1144, MAIL_ROW_H], "Roundsquare_24", x2=False, tint=ML_ROW_READ), "need_bind": "mail_read{i}", **mrows},
       {**game_frame([44, y, 1144, MAIL_ROW_H], "Roundsquare_24_Stroke", x2=False, tint=ML_CUR), "need_bind": "mail.cur{i}", **mrows},
       {"type": "image", "rect": [56, y + 14, 64, 64], "src_bind": "mail.icon{i}", "color": "#FFFFFFFF", **g},
       tlabel(136, y + 12, "mail.t{i}", ML_INK, 3, wrap_width=880, max_lines=1, need_bind="mail.new{i}", **mrows),
       tlabel(136, y + 12, "mail.t{i}", ML_SUB, 3, wrap_width=880, max_lines=1, need_bind="mail_read{i}", **mrows),
       tlabel(136, y + 52, "mail.s{i}", ML_SUB, 2, wrap_width=760, max_lines=1, **g),
       tlabel(1172, y + 52, "mail.d{i}", ML_SUB, 2, align="right", **g),
       {**label(1172, y + 12, "N", "#FFFF3C78", 3, align="right"), "need_bind": "mail.new{i}", **mrows},
       # 0.37.68: tap -> the companion opens this mail on the top screen with the game's buttons
       {"type": "rect", "rect": [44, y, 1144, MAIL_ROW_H], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "mail_open", "payload": "{i}", "noframe": True, **g}]
mail_scroll = {"id": "mail", "rect": [40, MAIL_Y, 1164, MAIL_BOTTOM - MAIL_Y], "count_bind": "mail.rows",
               "row_h": MAIL_ROW_H + MAIL_GAP, "cols": 1, "pad": 0, "bar": ML_CUR, "bar_track": "#FFD0D0DC", "bar_w": 8}
derived += [ex(f"mail_read{i}", f"mail.on{i} && !mail.new{i}") for i in range(MAIL_ROWS)]
# 0.37.84: BACK goes to APPS here when diving or when the phone is closed (nothing to go back from on the top screen)
derived += [ex("ph_nav", "in_dive || ph.closed")]
ma += [label(620, 856, "Tap a mail to open it on the top screen", ML_SUB, 2, align="center", need_bind="mail_hint"),
       tlabel(620, 852, "ph.status", ML_INK, 3, align="center", wrap_width=1000, max_lines=1, need_bind="!mail_hint"),
       # 0.37.69: BACK (one B on the top screen: closes the open mail) and CLOSE (B until the phone is closed)
       # 0.37.83: diving -> BACK / CLOSE only go back to APPS here (the phone cannot open under water)
]   # 0.37.70: no STOP -- BACK / CLOSE stop a running job first
derived += [ex("mail_hint", "mail.ok && !ph.busy && !ph.has_status")]

# ---- 0.37.72: ECOWATCHER -- the game's EcoWatcher list, read from the game (MissionManager): the missions in progress
# (READY when the goal is met and it waits to be delivered), then COMPLETED; the research rank and points on top.
# A tap opens that mission on the top screen (the phone driven with the game's buttons). View only otherwise.
ECO_ROW_H, ECO_ROWS, ECO_Y, ECO_GAP = 92, 14, 224, 6   # 0.38.5: all 14 rows (the module's kEcoRows), scrolled
ECO_BOTTOM = 836
EC_OK, EC_READY = "#FF2FA84F", "#FFFF9F2E"
# 0.37.74: the game's look -- its background, the blue research card with gauge and RP icon, pale cells with the
# app's emblem, the cursor outline, RP icon + points, the NEW / ready / CLEAR marks (all the game's own pictures)
EW_INK, EW_DIM, EW_CELL, EW_CELL_DONE, EW_RED = "#FF3D4466", "#FF7C86A6", "#FFDDEBFA", "#FFCADCF0", "#FFE0465A"
def ew_pic(r, name, **kw):
    return {"type": "image", "rect": list(r), "src": f"module:frame/{name}", "color": "#FFFFFFFF", **kw}
ec = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar(),
      game_frame([26, 104, 1188, 812], "Box_30rad", x2=False, tint="#FFBFDDF7"),
      # 0.37.100: 604x1078 -- too big for one live read (it never drew); the pieced loader, centre cut to the box
      # 0.37.101: flush with the main card -- its whole rect, corners rounded like Box_30rad's (30 px)
      {"type": "image", "src": "module:bg/EcoWatcher_bg@1188x812r30", "color": "#FFFFFFFF", "rect": [26, 104, 1188, 812]},
      {**rect([26, 104, 1188, 812], "#00000000"), "on_tap": "noop", "noframe": True},
      label(620, 420, "EcoWatcher is not on your phone yet", EW_INK, 3, align="center", need_bind="!eco.ok"),
      game_frame([44, 118, 1152, 96], "EcoWatcher_RankBox", x2=False, need_bind="eco.ok"),
      game_frame([1078, 118, 110, 96], "EcoWatcher_RankBoxDeco", x2=False, need_bind="eco.ok"),
      value([72, 124, 0, 0], "eco.lv", "#FFFFFFFF", 4, text="RESEARCH RANK ", need_bind="eco.ok"),
      ew_pic([72, 170, 30, 30], "EcoWatcher_RankIcon", need_bind="eco.ok"),
      tlabel(112, 170, "eco.rptxt", "#FFFFFFFF", 3, need_bind="eco.ok"),
      game_frame([440, 174, 600, 24], "EcoWatcher_RankGaugeOut", x2=False, need_bind="eco.ok"),
      {"type": "bar", "rect": [444, 178, 592, 16], "bind": "eco.rp_in", "max_bind": "eco.rp_span", "color": "#00000000",
       "bg": "#00000000", "frame": 0, "image": "module:frame/EcoWatcher_RankGaugeIn", "fill": "slice", "slice": [8, 0, 8, 0],
       "need_bind": "eco.ok"},
      ]
erows = {"repeat": ECO_ROWS, "repeat_bind": "eco.rows", "repeat_dy": ECO_ROW_H + ECO_GAP, "scroll": "eco"}
_e0 = len(ec)
for i in ["{i}"]:   # 0.38.5: one template row (the runtime repeats it), was 6 fixed rows + UP / DOWN
    y = ECO_Y
    g = {"need_bind": f"eco.on{i}"}
    ec += [game_frame([44, y, 1152, ECO_ROW_H], "EcoWatcher_CellBox", x2=False, tint=EW_CELL, need_bind=f"eco_live{i}"),
           game_frame([44, y, 1152, ECO_ROW_H], "EcoWatcher_CellBox", x2=False, tint=EW_CELL_DONE, need_bind=f"eco_done{i}"),
           game_frame([44, y, 1152, ECO_ROW_H], "EcoWatcher_CellBox", x2=False, tint="#FFFFFFFF", need_bind=f"eco.cur{i}"),
           game_frame([44, y, 1152, ECO_ROW_H], "EcoWatcher_Cell_Focus", x2=False, need_bind=f"eco.cur{i}"),
           ew_pic([56, y + 31, 30, 30], "EcoWatcher_Cell_MarkLogo", **g),
           tlabel(98, y + 10, f"eco.t{i}", EW_INK, 3, wrap_width=760, max_lines=1, need_bind=f"eco_live{i}"),
           tlabel(98, y + 10, f"eco.t{i}", EW_DIM, 3, wrap_width=760, max_lines=1, need_bind=f"eco_done{i}"),
           ew_pic([866, y + 12, 48, 20], "EcoWatcher_Cell_MarkNew", need_bind=f"eco.new{i}"),
           ew_pic([1070, y + 31, 30, 30], "EcoWatcher_Cell_MarkCollect", need_bind=f"eco_ready{i}"),
           ew_pic([956, y + 23, 103, 46], "EcoWatcher_Cell_MarkClear", need_bind=f"eco_done{i}"),
           ew_pic([1110, y + 32, 28, 28], "EcoWatcher_Cell_Icon", need_bind=f"eco.rp{i}"),
           value([1180, y + 31, 0, 0], f"eco.rp{i}", EW_INK, 3, text="", align="right", need_bind=f"eco.rp{i}"),
           label(60, y + 32, "COMPLETE", "#FFFFFFFF", 3, need_bind=f"eco.div{i}"),
           {**rect([250, y + 46, 940, 3], "#FFFFFFFF", frame=0), "noframe": True, "need_bind": f"eco.div{i}"}]
    # the goals as the game's row shows them: the picture and NOW / MAX (red until met, then green)
    for q in range(3):
        gx = 98 + q * 200
        gq = {"need_bind": f"eco.gon{i}_{q}"}
        ec += [{"type": "image", "rect": [gx, y + 52, 30, 30], "src_bind": f"eco.gi{i}_{q}", "color": "#FFFFFFFF", **gq},
               tlabel(gx + 36, y + 54, f"eco.gc{i}_{q}", EW_RED, 2, need_bind=f"eco_gshort{i}_{q}"),
               tlabel(gx + 36, y + 54, f"eco.gc{i}_{q}", EC_OK, 2, need_bind=f"eco.gok{i}_{q}")]
    ec.append({"type": "rect", "rect": [44, y, 1152, ECO_ROW_H], "bg": "#00000000", "color": "#00000000", "frame": 0,
               "on_tap": "eco_open", "payload": str(i), "noframe": True, **g})
    # PIN: the goals go to the DIVE map's hunt list and marks (fish, and now pickups too)
    # 0.37.76: the white pill gets an outline (it vanished on the white cursor card)
    ec += [game_frame([930, y + 22, 130, 48], "Roundsquare_24", x2=False, tint="#FFFFFFFF", need_bind=f"eco_pinoff{i}"),
           game_frame([930, y + 22, 130, 48], "Roundsquare_24_Stroke", x2=False, tint="#FFB4C6E0", need_bind=f"eco_pinoff{i}"),
           game_frame([930, y + 22, 130, 48], "Roundsquare_24", x2=False, tint="#FFFF3C78", need_bind=f"eco.pin{i}"),
           label(995, y + 32, "PIN", EW_INK, 2, align="center", need_bind=f"eco_pinoff{i}"),
           label(995, y + 32, "PINNED", "#FFFFFFFF", 2, align="center", need_bind=f"eco.pin{i}"),
           {"type": "rect", "rect": [926, y + 12, 138, 68], "bg": "#00000000", "color": "#00000000", "frame": 0,
            "on_tap": "eco_pin", "payload": str(i), "noframe": True, "need_bind": f"eco.canpin{i}"}]
for w in ec[_e0:]:
    w.update(erows)
eco_scroll = {"id": "eco", "rect": [40, ECO_Y, 1164, ECO_BOTTOM - ECO_Y], "count_bind": "eco.rows",
              "row_h": ECO_ROW_H + ECO_GAP, "cols": 1, "pad": 0, "bar": "#FF5A7BD0", "bar_track": "#FFCADCF0", "bar_w": 8}
derived += [d for i in range(ECO_ROWS) for d in (
    ex(f"eco_pinoff{i}", f"eco.canpin{i} && !eco.pin{i}"),
    *(ex(f"eco_gshort{i}_{q}", f"eco.gon{i}_{q} && !eco.gok{i}_{q}") for q in range(3)))]
derived += [d for i in range(ECO_ROWS) for d in (
    ex(f"eco_live{i}", f"eco.on{i} && eco.st{i} != 2"), ex(f"eco_done{i}", f"eco.on{i} && eco.st{i} == 2"),
    ex(f"eco_prog{i}", f"eco.on{i} && eco.st{i} == 0"), ex(f"eco_ready{i}", f"eco.on{i} && eco.st{i} == 1"))]
ec += [label(620, 856, "Tap a mission to open it on the top screen", ML_SUB, 2, align="center", need_bind="mail_hint"),
       tlabel(620, 852, "ph.status", ML_INK, 3, align="center", wrap_width=1000, max_lines=1, need_bind="!mail_hint"),
       # 0.37.83: diving -> BACK / CLOSE only go back to APPS here (the phone cannot open under water)
       ]   # 0.38.5: UP / DOWN gone (the list scrolls)

# ---- 0.37.89: WEAPON SHOP -- Duff's Weapon Craft list, read from the save (the same root crafts the game lists, in its
# order): the weapon's picture and name, develop % (undeveloped) / CRAFTABLE / NEW, its price and materials (have / need).
# A tap moves the shop's cursor on the top screen to that weapon (its details show there); nothing is crafted.
# 0.37.93: the game's look -- a dark teal shop with Duff's pink title band, a 2-column grid of blueprint cards (a picture
# box with the blueprint drawing until developed, the name, then DEVELOP % / CAN BE CRAFTED / NOT ENOUGH MATERIALS as the
# game's strip), plus what the game shows on its side panel: materials, price, and TRACK / CRAFT.
# 0.37.97: as the game draws it -- the camo (PhoneBg_Duff) behind everything, the full-width pink bar
# (UI_WeaponTree_TopBar) with Duff's title (UI_WeaponTree_TopLogo) on its left, and the game's cards at their own shape
# (UI_WeaponListBox: developed -- the dark card with the outlined picture box built in; UI_WeaponListBox02: blueprint --
# the blue grid card, the white drawing straight on it), picture at the top, name centred, then the strip / develop %.
# Under each card: its materials (have / need), the price, TRACK / CRAFT (what the game shows on its side panel).
# 0.37.98: two columns as in the game; the card drawn at the game's 2x pixel size (528 wide = 264 x 2, so nothing
# stretches across) and only made taller, so materials, price and TRACK / CRAFT sit inside its border too.
# 0.37.99: materials and price off the cards (the top screen shows them while diving), TRACK / CRAFT centred, cards
# shorter and spaced out like the game's, the scroll bar on the main card's right edge.
SH_COLS, SH_W, SH_GX, SH_GY = 2, 528, 28, 24
SH_AH = 336                                      # the card picture's own height at 2x (168 x 2): the picture, the name
SH_H = 316
SH_ROWS, SH_X, SH_Y, SH_BOTTOM = 24, 72, 184, 826
SH_BG, SH_CARD, SH_CARDE, SH_INK, SH_SUB, SH_CUR = "#FF173F4A", "#FF1F5260", "#FF3E8CA0", "#FFFFFFFF", "#FFA9C6CE", "#FFFFC93C"
SH_PINK = "#FFF04E8C"
sh = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar(),
      game_frame([26, 104, 1188, 812], "Box_30rad", x2=False, tint=SH_BG),
      {"type": "image", "src": "module:bg/PhoneBg_Duff@1188x812r30", "color": "#FFFFFFFF", "rect": [26, 104, 1188, 812]},
      {**rect([26, 104, 1188, 812], "#00000000"), "on_tap": "noop", "noframe": True},
      # (UI_WeaponTree_TopBar is 1366 px wide: the pieced loader)
      {"type": "image", "src": "module:bg/UI_WeaponTree_TopBar@1188x60r30st", "color": "#FFFFFFFF", "rect": [26, 104, 1188, 60]},
      {"type": "image", "src": "module:frame/UI_WeaponTree_TopLogo", "color": "#FFFFFFFF", "rect": [46, 111, 260, 46]},
      label(620, 420, "No weapon plans yet", SH_SUB, 3, align="center", need_bind="!shop.ok")]
srows = {"repeat": SH_ROWS, "repeat_bind": "shop.rows", "repeat_cols": SH_COLS, "repeat_dx": SH_W + SH_GX,
         "repeat_row_dy": SH_H + SH_GY, "scroll": "shop"}
x, y = SH_X, SH_Y
g = {"need_bind": "shop.on{i}", **srows}
def sh_card(name, bind):                         # the game's card picture at 2x, taller (GAME_FRAMES slices)
    return {**game_frame([x, y, SH_W, SH_H], name), "need_bind": bind, **srows}
def ay(f): return y + round(SH_AH * f)           # a height on the card's top part, as a share of it (measured on the game's)
PX, PW = x + round(SH_W * 0.19), round(SH_W * 0.62)      # the picture box: 18..82 % across, 7..45 % down
sy = y + 200                                     # the strip / develop % row
sh += [sh_card("UI_WeaponListBox", "shop_card{i}"), sh_card("UI_WeaponListBox02", "shop_bpc{i}"),
       sh_card("UI_WeaponListBox_Select", "shop_cardsel{i}"), sh_card("UI_WeaponListBox_Select.02", "shop_bpcsel{i}"),
       {"type": "image", "rect": [PX, ay(0.08), PW, ay(0.44) - ay(0.08)], "src_bind": "shop.ic{i}", "color": "#FFFFFFFF", **g},
       tlabel(x + SH_W // 2, ay(0.49), "shop.t{i}", SH_INK, 2, align="center", wrap_width=SH_W - 60, max_lines=1, **g),
       {**game_frame([x + 104, sy, SH_W - 208, 34], "UI_WeaponCraft_Cell_Btn_Blue", x2=False), "need_bind": "shop.can{i}", **srows},
       {**label(x + SH_W // 2, sy + 7, "CAN BE CRAFTED", "#FFFFFFFF", 2, align="center"), "need_bind": "shop.can{i}", **srows},
       {**game_frame([x + 94, sy, SH_W - 188, 34], "UI_WeaponCraft_Cell_Btn_Pink", x2=False), "need_bind": "shop.short{i}", **srows},
       {**label(x + SH_W // 2, sy + 7, "NOT ENOUGH MATERIALS", "#FFFFFFFF", 2, align="center"), "need_bind": "shop.short{i}", **srows},
       value([x + 130, sy + 2, 0, 0], "shop.pct{i}", SH_INK, 3, suffix="%", need_bind="shop_dev{i}", **srows),
       {"type": "bar", "rect": [x + 216, sy + 10, 190, 12], "bind": "shop.pct{i}", "max": 100, "color": "#FFFFFFFF",
        "bg": "#FF0E2A33", "frame": 0, "need_bind": "shop_dev{i}", **srows}]
by = y + 248                                     # inside the card, under the strip: TRACK (+ CRAFT), centred
sh += [{"type": "rect", "rect": [x, y, SH_W, SH_H], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "shop_open", "payload": "{i}", "noframe": True, **g}]
for tx, gate in ((SH_W // 2 - 151, "shop.can{i}"), (SH_W // 2 - 80, "!shop.can{i}")):   # pair / TRACK alone
    k = "p" if gate.startswith("shop") else "s"
    sh += [{**game_frame([x + tx, by, 160, 40], "UI_WeaponCraft_Cell_Btn_Grey", x2=False), "need_bind": f"shop_tof{k}{{i}}", **srows},
           {**game_frame([x + tx, by, 160, 40], "UI_WeaponCraft_Cell_Btn_Pink", x2=False), "need_bind": f"shop_ton{k}{{i}}", **srows},
           {**label(x + tx + 80, by + 9, "TRACK", "#FFFFFFFF", 2, align="center"), "need_bind": f"shop_tof{k}{{i}}", **srows},
           {"type": "image", "rect": [x + tx + 12, by + 12, 22, 17], "src": "module:frame/Weapon_Check", "color": "#FFFFFFFF",
            "need_bind": f"shop_ton{k}{{i}}", **srows},
           {**label(x + tx + 92, by + 9, "TRACKED", "#FFFFFFFF", 2, align="center"), "need_bind": f"shop_ton{k}{{i}}", **srows},
           {"type": "rect", "rect": [x + tx - 4, by - 4, 168, 48], "bg": "#00000000", "color": "#00000000", "frame": 0,
            "on_tap": "shop_track", "payload": "{i}", "noframe": True, "need_bind": "shop_top{i}" if k == "p" else "shop_tops{i}", **srows}]
cx = SH_W // 2 + 21
sh += [{**game_frame([x + cx, by, 130, 40], "UI_WeaponCraft_Cell_Btn_Blue", x2=False), "need_bind": "shop.can{i}", **srows},
       {**label(x + cx + 65, by + 9, "CRAFT", "#FFFFFFFF", 2, align="center"), "need_bind": "shop.can{i}", **srows},
       {"type": "rect", "rect": [x + cx - 4, by - 4, 138, 48], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "shop_craft", "payload": "{i}", "noframe": True, "need_bind": "shop.can{i}", **srows}]
derived += [d for i in range(SH_ROWS) for d in (
    ex(f"shop_tofp{i}", f"shop.on{i} && !shop.trk{i} && shop.can{i}"), ex(f"shop_tonp{i}", f"shop.trk{i} && shop.can{i}"),
    ex(f"shop_tops{i}", f"shop.on{i} && !shop.can{i}"), ex(f"shop_top{i}", f"shop.on{i} && shop.can{i}"),
    ex(f"shop_tofs{i}", f"shop.on{i} && !shop.trk{i} && !shop.can{i}"), ex(f"shop_tons{i}", f"shop.trk{i} && !shop.can{i}"))]
# 0.37.99: the scroll bar on the main card's right edge
shop_scroll = {"id": "shop", "rect": [40, SH_Y, 1156, SH_BOTTOM - SH_Y], "count_bind": "shop.rows",
               "row_h": SH_H + SH_GY, "cols": SH_COLS, "pad": 0, "bar": SH_CUR, "bar_track": "#FF0E2A33", "bar_w": 8}
derived += [d for i in range(SH_ROWS) for d in (
    ex(f"shop_dev{i}", f"shop.on{i} && shop.st{i} == 0"), ex(f"shop_trkoff{i}", f"shop.on{i} && !shop.trk{i}"),
    ex(f"shop_card{i}", f"shop.on{i} && shop.st{i} != 0 && !shop.cur{i}"), ex(f"shop_bpc{i}", f"shop.on{i} && shop.st{i} == 0 && !shop.cur{i}"),
    ex(f"shop_cardsel{i}", f"shop.on{i} && shop.st{i} != 0 && shop.cur{i}"), ex(f"shop_bpcsel{i}", f"shop.on{i} && shop.st{i} == 0 && shop.cur{i}"))]
derived += [ex("shop_hint", "mail_hint && !shop.confirm"), ex("shop_stat", "!mail_hint && !shop.confirm")]
sh += [label(620, 856, "Tap a weapon to show it on the top screen", SH_SUB, 2, align="center", need_bind="shop_hint"),
       tlabel(620, 852, "ph.status", SH_INK, 3, align="center", wrap_width=1000, max_lines=1, need_bind="shop_stat"),
       # 0.37.91: the game's craft popup is up -> answer it from here
       *game_button([700, 838, 230, 64], "CONFIRM", "shop_confirm", kind="ok", sc=3, gate="shop.confirm"),
       *game_button([944, 838, 230, 64], "CANCEL", "shop_cancel", kind="grey2", sc=3, gate="shop.confirm"),
       ]

# ---- 0.37.102: IDIVER -- Dave's gear upgrades, read from the game (SubEquipmentManager + the SubEquipment table): one
# row per piece of gear in the game's order -- its picture, name, level, what the next level raises (now > next), the
# price; tap -> the cursor to it on the top screen; UPGRADE -> A there (the game's detail), then CONFIRM / CANCEL here.
# First pass: the look is plain until the game's iDiver pictures are indexed.
# 0.37.103: laid out like the game's rows -- picture box on the left with the price under it, the name, then
# Lv.N and the number it raises >>> Lv.N+1 and the next number in yellow (a weapon: its name now >>> next).
# 0.37.104: the game's look -- iDiver_BG behind (flush, rounded like the card), the top bar iDiver_Top_BG with
# iDiver_Logo, each row on iDiver_ListBar (its name strip the same bar again), the cursor iDiver_ListBar_Stroke in
# yellow, the gear's own picture (iDiver_Icon_*), the >> arrow iDiver_Level_Arrow01.
# 0.37.105: the top bar flush with the card (its whole picture stretched, top corners rounded like the card), two
# columns of taller cards; the game's level-up screen gets an OK here.
# 0.37.106: each card as the game's row, measured on its screen at the same size (its row is 559 px wide there,
# ours 566): the lavender card, the darker name strip right of the picture box, the picture box with the price band
# under the picture, Lv / number on the left, the arrow, the next Lv / number in yellow; colours as the game draws them.
# The game's own row has no button (A selects): UPGRADE sits where its "A Select" is, in the name strip.
# 0.37.107: one column (the same card, wider: the numbers spread out, the names on one line)
IV_W, IV_H, IV_GX, IV_GY, IV_X, IV_Y, IV_ROWS = 1140, 143, 12, 12, 46, 188, 10
IV_BG, IV_INK = "#FF1B3557", "#FFFFFFFF"
IV_CARD, IV_STRIP, IV_BOX, IV_PRICE, IV_NAME, IV_NEXT, IV_SEL, IV_SUB = (
    "#FF8492B6", "#FF6E82A5", "#FF4D6B92", "#FF164467", "#FF00223A", "#FFFEE400", "#FFF0D254", "#FFDCE6F5")
iv = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar(),
      game_frame([26, 104, 1188, 812], "Box_30rad", x2=False, tint=IV_BG),
      {"type": "image", "src": "module:bg/iDiver_BG@1188x812r30", "color": "#FFFFFFFF", "rect": [26, 104, 1188, 812]},
      {**rect([26, 104, 1188, 812], "#00000000"), "on_tap": "noop", "noframe": True},
      {"type": "image", "src": "module:bg/iDiver_Top_BG@1188x70r30st", "color": "#FFFFFFFF", "rect": [26, 104, 1188, 70]},
      {"type": "image", "src": "module:frame/iDiver_Logo", "color": "#FFFFFFFF", "rect": [52, 116, 174, 46]},
      label(620, 420, "Open iDiver on your phone once to load it here", IV_INK, 3, align="center", need_bind="!idv.ok")]
vrows = {"repeat": IV_ROWS, "repeat_bind": "idv.rows", "repeat_dy": IV_H + IV_GY, "scroll": "idv"}
x, y = IV_X, IV_Y
g = {"need_bind": "idv.on{i}", **vrows}
PB = [x + IV_W - 140, y + 8, 132, 34]                   # the pill in the name strip (where the game shows "A Select")
iv += [{**game_frame([x, y, IV_W, IV_H], "Box_4rad", x2=False, tint=IV_CARD), **g},
       {**game_frame([x + 128, y + 3, IV_W - 134, 44], "Box_4rad", x2=False, tint=IV_STRIP), **g},
       {**game_frame([x + 5, y + 4, 118, IV_H - 8], "Box_4rad", x2=False, tint=IV_BOX), **g},
       {**game_frame([x + 5, y + 108, 118, IV_H - 112], "Box_4rad", x2=False, tint=IV_PRICE), **g},
       {**game_frame([x - 2, y - 2, IV_W + 4, IV_H + 4], "iDiver_ListBar_Stroke", tint=IV_SEL), "need_bind": "idv.cur{i}", **vrows},
       {"type": "image", "rect": [x + 24, y + 16, 80, 80], "src_bind": "idv.ic{i}", "color": "#FFFFFFFF", **g},
       {"type": "image", "rect": [x + 12, y + 113, 24, 24], "src": "module:icon/Coin32", "color": "#FFFFFFFF", "need_bind": "idv_nm{i}", **vrows},
       tlabel(x + 40, y + 114, "idv.ptxt{i}", IV_INK, 2, need_bind="idv_pok{i}", **vrows),
       tlabel(x + 40, y + 114, "idv.ptxt{i}", "#FFFF3B4E", 2, need_bind="idv_poor{i}", **vrows),
       tlabel(x + 142, y + 12, "idv.t{i}", IV_NAME, 3, wrap_width=700, max_lines=1, **g),
       value([x + 142, y + 54, 0, 0], "idv.lv{i}", IV_INK, 3, text="Lv.", **g),
       tlabel(x + 142, y + 92, "idv.scur{i}", IV_INK, 3, wrap_width=440, max_lines=1, **g),
       {"type": "image", "rect": [x + 590, y + 72, 68, 42], "src": "module:frame/iDiver_Level_Arrow01", "color": "#FFFFFFFF", "need_bind": "idv_nm{i}", **vrows},
       value([x + 680, y + 54, 0, 0], "idv.nlv{i}", IV_NEXT, 3, text="Lv.", need_bind="idv_nm{i}", **vrows),
       tlabel(x + 680, y + 92, "idv.snxt{i}", IV_NEXT, 3, wrap_width=440, max_lines=1, need_bind="idv_nm{i}", **vrows),
       {"type": "rect", "rect": [x, y, IV_W, IV_H], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "idv_open", "payload": "{i}", "noframe": True, **g},
       # UPGRADE (can afford) / LOCKED / MAX; short of gold -> the price in red, as the game shows it
       {**game_frame(PB, "UI_WeaponCraft_Cell_Btn_Blue", x2=False), "need_bind": "idv_can{i}", **vrows},
       {**label(PB[0] + PB[2] // 2, PB[1] + 7, "UPGRADE", "#FFFFFFFF", 2, align="center"), "need_bind": "idv_can{i}", **vrows},
       {"type": "rect", "rect": [PB[0] - 6, PB[1] - 6, PB[2] + 12, PB[3] + 12], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "idv_up", "payload": "{i}", "noframe": True, "need_bind": "idv_can{i}", **vrows},
       {**game_frame(PB, "UI_WeaponCraft_Cell_Btn_Grey", x2=False), "need_bind": "idv_lock{i}", **vrows},
       {**label(PB[0] + PB[2] // 2, PB[1] + 7, "LOCKED", "#FFFFFFFF", 2, align="center"), "need_bind": "idv_lock{i}", **vrows},
       {**game_frame(PB, "UI_WeaponCraft_Cell_Btn_Grey", x2=False), "need_bind": "idv_max{i}", **vrows},
       {**label(PB[0] + PB[2] // 2, PB[1] + 7, "MAX", "#FFFFFFFF", 2, align="center"), "need_bind": "idv_max{i}", **vrows}]
# (the scroll area starts a little above the first card: its cursor border sits 2 px outside it -- 0.37.106 cut it off)
idv_scroll = {"id": "idv", "rect": [38, IV_Y - 6, 1164, 826 - IV_Y + 6], "count_bind": "idv.rows",
              "row_h": IV_H + IV_GY, "cols": 1, "pad": 0, "bar": IV_SEL, "bar_track": "#FF0E2A33", "bar_w": 8}
derived += [d for i in range(IV_ROWS) for d in (
    ex(f"idv_nm{i}", f"idv.on{i} && idv.st{i} != 2"), ex(f"idv_can{i}", f"idv.on{i} && idv.st{i} == 0"),
    ex(f"idv_poor{i}", f"idv.on{i} && idv.st{i} == 1"), ex(f"idv_max{i}", f"idv.on{i} && idv.st{i} == 2"),
    ex(f"idv_lock{i}", f"idv.on{i} && idv.st{i} == 3"), ex(f"idv_pok{i}", f"idv.on{i} && idv.st{i} != 2 && idv.st{i} != 1"))]
derived += [ex("idv_hint", "mail_hint && !idv.confirm && !idv.lvup"), ex("idv_stat", "!mail_hint && !idv.confirm && !idv.lvup")]
iv += [label(620, 856, "Tap a gear to show it on the top screen", IV_SUB, 2, align="center", need_bind="idv_hint"),
       tlabel(620, 852, "ph.status", IV_INK, 3, align="center", wrap_width=1000, max_lines=1, need_bind="idv_stat"),
       *game_button([700, 838, 230, 64], "CONFIRM", "idv_confirm", kind="ok", sc=3, gate="idv.confirm"),
       *game_button([944, 838, 230, 64], "CANCEL", "idv_cancel", kind="grey2", sc=3, gate="idv.confirm"),
       # 0.37.105: the game's level-up screen after buying -> OK (its A)
       *game_button([822, 838, 230, 64], "OK", "idv_ok", kind="ok", sc=3, gate="idv.lvup"),
       ]

# ---- 0.37.109: CALL -- the game's Contacts app, read from its own list (ContactsPanel -> ContactsListScroller): the
# contacts in its order, each with its portrait and name; tap -> the cursor to it on the top screen; CALL -> X there
# (the game calls; BACK = its B hangs up). First pass: the game's colours (from its screen), portraits by their names.
CL_ROW_H, CL_GAP, CL_Y, CL_ROWS = 100, 8, 190, 24
CL_BG, CL_ROW, CL_SEL, CL_INK, CL_SUB, CL_PILL = "#FFE2EAB8", "#FFF1F1E4", "#FFFBF5F6", "#FF3A3A36", "#FF6E7A5A", "#FFF2EFD2"
cl = [rect([0, 0, 1240, 1080], BG), *header(dave=False), *phone_bar(),
      game_frame([26, 104, 1188, 812], "Box_30rad", x2=False, tint=CL_BG),
      # 0.37.113: the game's own Contacts background (Address_BG, an 8x1078 gradient strip), stretched, corners rounded
      {"type": "image", "src": "module:bg/Address_BG@1188x812r30s", "color": "#FFFFFFFF", "rect": [26, 104, 1188, 812]},
      {**rect([26, 104, 1188, 812], "#00000000"), "on_tap": "noop", "noframe": True},
      # 0.37.112: the title a size bigger
      game_frame([420, 110, 400, 74], "Roundsquare_24", x2=False, tint=CL_PILL),
      # 0.37.111: the game's contacts-book picture (AppIcon_Address), as its Contacts title shows it
      # 0.37.113: the plain book as the game's title shows it (Address_Icon, 41x44)
      {"type": "image", "rect": [446, 117, 56, 60], "src": "module:frame/Address_Icon", "color": "#FFFFFFFF"},
      label(650, 126, "Contacts", CL_INK, 4, align="center"),
      label(620, 420, "No contacts yet", CL_SUB, 3, align="center", need_bind="!call.ok")]
crows = {"repeat": CL_ROWS, "repeat_bind": "call.rows", "repeat_dy": CL_ROW_H + CL_GAP, "scroll": "call"}
y = CL_Y
g = {"need_bind": "call.on{i}", **crows}
cl += [{**game_frame([44, y, 1144, CL_ROW_H], "Roundsquare_24", x2=False, tint=CL_ROW), "need_bind": "call_off{i}", **crows},
       {**game_frame([44, y, 1144, CL_ROW_H], "Roundsquare_24", x2=False, tint=CL_SEL), "need_bind": "call.cur{i}", **crows},
       {"type": "image", "rect": [66, y + 10, 80, 80], "src_bind": "call.ic{i}", "color": "#FFFFFFFF", **g},
       tlabel(170, y + 32, "call.t{i}", CL_INK, 3, wrap_width=700, max_lines=1, **g),
       # 0.37.113: the game's NEW mark (Address_New)
       {"type": "image", "rect": [128, y + 6, 32, 32], "src": "module:frame/Address_New", "color": "#FFFFFFFF", "need_bind": "call.new{i}", **crows},
       {"type": "rect", "rect": [44, y, 1144, CL_ROW_H], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "call_open", "payload": "{i}", "noframe": True, **g},
       {**game_frame([1000, y + 20, 170, 60], "UI_WeaponCraft_Cell_Btn_Green", x2=False), **g},
       {"type": "image", "rect": [1018, y + 30, 40, 40], "src": "module:icon/AppIcon_Telephone", "color": "#FFFFFFFF", **g},
       {**label(1110, y + 38, "CALL", "#FFFFFFFF", 2, align="center"), **g},
       {"type": "rect", "rect": [994, y + 14, 182, 72], "bg": "#00000000", "color": "#00000000", "frame": 0,
        "on_tap": "call_call", "payload": "{i}", "noframe": True, **g}]
call_scroll = {"id": "call", "rect": [40, CL_Y - 4, 1156, 826 - CL_Y + 4], "count_bind": "call.rows",
               "row_h": CL_ROW_H + CL_GAP, "cols": 1, "pad": 0, "bar": "#FF7DBE5A", "bar_track": "#FFC8D6A0", "bar_w": 8}
derived += [ex(f"call_off{i}", f"call.on{i} && !call.cur{i}") for i in range(CL_ROWS)]
cl += [label(620, 856, "Tap a contact to show it on the top screen", CL_SUB, 2, align="center", need_bind="mail_hint"),
       tlabel(620, 852, "ph.status", CL_INK, 3, align="center", wrap_width=1000, max_lines=1, need_bind="!mail_hint"),
       ]

# Cards get the screen border's blue outline (the same shape, thinner), drawn right over them.
SKIP = {"type", "rect", "bg", "color", "frame", "on_tap", "payload", "pulse", "src", "fill", "slice", "tint", "scale_bind", "pivot"}
for page in (dive, tp, cg, su, ig, ph, cc, ma, ec, sh, iv, cl):
    out = []
    for w in page:
        out.append(w)
        r = w.get("rect")
        if w.pop("noframe", False):
            continue
        if (w.get("type") == "rect" and w.get("bg") in (CARD, CARD_DIM) and w.get("color") == "#00000000"
                and r and 100 <= r[2] < 1240 and r[3] >= 50):
            if page is su or page is ig:
                f = game_frame(r, "UI_Sushi_Recipe_Menu_box", tint="#FF8C8C8C" if w.get("bg") == CARD_DIM else "#FFB4B4B4")
            else:
                fill = game_frame(r, "UI_EdgeFrame", tint=w["bg"], x2=r[3] >= 70)
                fill.update({k: v for k, v in w.items() if k not in SKIP})
                out.append(fill)
                w["bg"] = "#00000000"          # the rect stays only for its tap
                f = game_frame(r, "UI_EdgeFrame_Stroke", tint=BLUE_C, x2=r[3] >= 70)
            f.update({k: v for k, v in w.items() if k not in SKIP})
            out.append(f)
    page[:] = out

# Text size: everything up to scale 5 is one step bigger (the big numbers stay as they are).
for page in (dive, dbg, tp, cg, su, ig, dp, dg, ph, cc, ma, ec, sh, iv, cl):
    for w in page:
        w.pop("noframe", None)
        if "text_scale" in w:
            w["text_scale"] = drawn(w["text_scale"])

# 0.38.9: the map view's right column (gauge, depth / cargo cards, hunt list) is not drawn in the village (the map
# takes the card's width there)
_nv = {}
for w in dive:
    if w.get("hide_bind") == "dive.map" and w.get("hide_eq") == 0 and w.get("rect") and w["rect"][0] >= RX - 30:
        nb = w.get("need_bind")
        key = "nv_" + re.sub(r"[^A-Za-z0-9_]", "_", nb or "all")
        if key not in _nv:
            _nv[key] = ex(key, f"!scene.village && ({nb})" if nb else "!scene.village")
        w["need_bind"] = key
derived += list(_nv.values())
manifest = {"format": 1, "title_id": "010097F018538000", "name": "Dave the Diver dual screen",
            "_about": "Dave the Diver companion: live oxygen, depth and carry weight while diving. Values read from the game's own objects; ships no game assets.",
            # discard without the menu: Eden applies this code patch when the game starts (only to
            # this exact build; tools/dtd/discard_patch)
            "load_plan": "plans/<build>.json",
            "min_runtime": 17, "canvas_w": 1240, "canvas_h": 1080, "background": BG, "nav": False,
            # the game's own lettering (its TextMeshPro font), read from the player's game files by the
            # module (pictures.idx "F" line); without it the companion keeps its built-in font
            "font": "module:fontdata", "font_atlas": "module:font",
            "derived": derived,
            "actions": {"to_debug": {"kind": "page", "page": "debug", "transition": "fade", "duration_ms": 150},
                        "to_dive": {"kind": "page", "page": "dive", "transition": "fade", "duration_ms": 150},
                        "to_tasks": {"kind": "page", "page": "tasks", "transition": "fade", "duration_ms": 150},
                        **{f"am_pin_{c}": {"kind": "module", "action": "am_pin", "argument": c} for c in range(AM_SHOW)},
                        "to_cargo": {"kind": "page", "page": "cargo", "transition": "fade", "duration_ms": 150},
                        "to_sushi": {"kind": "page", "page": "sushi", "transition": "fade", "duration_ms": 150},
                        "to_ingredients": {"kind": "page", "page": "ingredients", "transition": "fade", "duration_ms": 150},
                        "to_dtest": {"kind": "page", "page": "dtest", "transition": "fade", "duration_ms": 150},
                        "to_phone": {"kind": "page", "page": "phone", "transition": "fade", "duration_ms": 150},
                        "to_calc": {"kind": "page", "page": "calc", "transition": "fade", "duration_ms": 150},
                        "to_mail": {"kind": "page", "page": "mail", "transition": "fade", "duration_ms": 150},
                        "to_eco": {"kind": "page", "page": "eco", "transition": "fade", "duration_ms": 150},
                        "to_shop": {"kind": "page", "page": "shop", "transition": "fade", "duration_ms": 150},
                        "to_idv": {"kind": "page", "page": "idv", "transition": "fade", "duration_ms": 150},
                        "to_call": {"kind": "page", "page": "call", "transition": "fade", "duration_ms": 150},
                        "call_open": {"kind": "module", "action": "call_open", "argument": "$payload"},
                        "call_call": {"kind": "module", "action": "call_call", "argument": "$payload"},
                        "idv_open": {"kind": "module", "action": "idv_open", "argument": "$payload"},
                        "idv_up": {"kind": "module", "action": "idv_up", "argument": "$payload"},
                        "idv_confirm": {"kind": "module", "action": "idv_confirm"},
                        "idv_cancel": {"kind": "module", "action": "idv_cancel"},
                        "idv_ok": {"kind": "module", "action": "idv_ok"},
                        "shop_open": {"kind": "module", "action": "shop_open", "argument": "$payload"},
                        "shop_craft": {"kind": "module", "action": "shop_craft", "argument": "$payload"},
                        "shop_track": {"kind": "module", "action": "shop_track", "argument": "$payload"},
                        "shop_confirm": {"kind": "module", "action": "shop_confirm"},
                        "shop_cancel": {"kind": "module", "action": "shop_cancel"},
                        "eco_open": {"kind": "module", "action": "eco_open", "argument": "$payload"},
                        "eco_pin": {"kind": "module", "action": "eco_pin", "argument": "$payload"},
                        "eco_scroll": {"kind": "module", "action": "eco_scroll", "argument": "$payload"},   # 0.37.84: was missing
                        **{f"calc_{k}": {"kind": "module", "action": "calc_key", "argument": k} for k in range(20)},
                        "ph_probe": {"kind": "module", "action": "ph_probe"},
                        # 0.37.68: the phone apps, driven with the game's buttons (one press per request, see PhoneJobStep)
                        "mail_open": {"kind": "module", "action": "mail_open", "argument": "$payload"},
                        "ph_stop": {"kind": "module", "action": "ph_stop"},
                        "ph_back": {"kind": "module", "action": "ph_back"},
                        "ph_close": {"kind": "module", "action": "ph_close"},
                        **{f"ph_press_{k}": {"kind": "button", "button": b, "frames": 4, "enabled_bind": f"ph.p_{k}"}
                           for k, b in (("a", "A"), ("b", "B"), ("plus", "Plus"), ("up", "DUp"), ("down", "DDown"),
                                        ("left", "DLeft"), ("right", "DRight"), ("zl", "ZL"), ("x", "X"))},
                        **({"to_diag": {"kind": "page", "page": "diag", "transition": "fade", "duration_ms": 150}}
                           if SHOW_DIAG else {}),
                        # module actions; "$payload" is the tapped row / box (its "{i}")
                        "dc_pick": {"kind": "module", "action": "dc_pick", "argument": "$payload"},
                        "ing_pick": {"kind": "module", "action": "ing_pick", "argument": "$payload"},
                        "ing_close": {"kind": "module", "action": "ing_close"},
                        "noop": {"kind": "module", "action": "noop"},
                        "ld_test": {"kind": "module", "action": "ld_test"},
                        "rec_pick": {"kind": "module", "action": "rec_pick", "argument": "$payload"},
                        **{f"dive_view_{k}": {"kind": "module", "action": "dive_view", "argument": k} for k in range(2)},
                        **{f"mk_all_{k}": {"kind": "module", "action": "mk_all", "argument": k} for k in range(2)},
                        **{f"mk_toggle_{k}": {"kind": "module", "action": "mk_toggle", "argument": k} for k in range(6)},
                        "mk_menu": {"kind": "module", "action": "mk_menu"},
                        "mk_every": {"kind": "module", "action": "mk_every"},
                        "map_zoom": {"kind": "module", "action": "map_zoom", "argument": 0},
                        "vm_zoom": {"kind": "module", "action": "vm_zoom", "argument": 0},
                        **{a: {"kind": "module", "action": a} for a in ("rec_close", "rec_pin", "pin_dec", "pin_inc", "pin_clear",
                                                                        "menu_add", "menu_dec", "menu_inc", "menu_remove",
                                                                        "menu_swap_open", "menu_swap_close")},
                        "menu_swap": {"kind": "module", "action": "menu_swap", "argument": "$payload"},
                        **{f"pin_pick_{p}": {"kind": "module", "action": "pin_pick", "argument": p} for p in range(2)},
                        **{f"pl_open_{j}": {"kind": "module", "action": "pl_open", "argument": j} for j in range(8)},
                        **{a: {"kind": "module", "action": a} for a in ("pin_unpin", "pin_open", "pin_cancel")},
                        **{f"m_pin_{i}": {"kind": "module", "action": "m_pin", "argument": i} for i in range(MAX_TASKS)},
                        **{f"cargo_sort_{k}": {"kind": "module", "action": "cargo_sort", "argument": k} for k in range(len(SORTS))},
                        **{f"rec_sort_{k}": {"kind": "module", "action": "rec_sort", "argument": k} for k in range(4)},
                        **{f"ing_filter_{k}": {"kind": "module", "action": "ing_filter", "argument": k} for k in range(3)},
                        # 0.38.28: the loadout's A / R slots: the game's own weapon swap (R), only while diving
                        "lo_swap": {"kind": "module", "action": "lo_swap"},
                        "lo_press_r": {"kind": "button", "button": "R", "frames": 4, "enabled_bind": "lo.p_r"},
                        "lo_item": {"kind": "module", "action": "lo_item"},
                        "hud_hide": {"kind": "module", "action": "hud_hide"},
                        "ld_continue": {"kind": "button", "button": "A", "frames": 4, "enabled_bind": "ld.menu"},
                        "lo_press_l": {"kind": "button", "button": "L", "frames": 4, "enabled_bind": "lo.p_l"},
                        "dc_go": {"kind": "module", "action": "dc_go"},
                        "dc_cancel": {"kind": "module", "action": "dc_cancel"},
                        # button presses the module asks for (one sample each), see DiscardStep
                        "dc_press_a": {"kind": "button", "button": "A", "frames": 4, "enabled_bind": "dc.p_a"},
                        "dc_press_b": {"kind": "button", "button": "B", "frames": 4, "enabled_bind": "dc.p_b"},
                        "dc_press_plus": {"kind": "button", "button": "Plus", "frames": 4, "enabled_bind": "dc.p_plus"},
                        "dc_press_down": {"kind": "button", "button": "DDown", "frames": 4, "enabled_bind": "dc.p_down"}},
            "enforce_gate": {"point": "dc.busy", "max": 1},
            "enforce": [{"action": a, "every_ms": 16} for a in ("dc_press_a", "dc_press_b", "dc_press_plus", "dc_press_down",
                                                                 *(f"ph_press_{k}" for k in ("a", "b", "plus", "up", "down", "left", "right", "zl", "x")), "lo_press_r", "lo_press_l")],
            "pages": [{"id": "dive", "title": "Dive", "widgets": dive},
                      {"id": "debug", "title": "Debug", "widgets": dbg},
                      {"id": "tasks", "title": "Tasks probe", "widgets": tp},
                      {"id": "cargo", "title": "Cargo", "widgets": cg, "scrolls": [cargo_scroll]},
                      {"id": "sushi", "title": "Recipes", "widgets": su, "scrolls": [rec_scroll, swap_scroll]},
                      {"id": "ingredients", "title": "Ingredients", "widgets": ig, "scrolls": [ing_scroll]},
                      {"id": "dtest", "title": "Discard test", "widgets": dp},
                      {"id": "phone", "title": "Phone", "widgets": ph},
                      {"id": "calc", "title": "Calculator", "widgets": cc},
                      {"id": "mail", "title": "Mail", "widgets": ma, "scrolls": [mail_scroll]},
                      {"id": "eco", "title": "EcoWatcher", "widgets": ec, "scrolls": [eco_scroll]},
                      {"id": "shop", "title": "Weapon Shop", "widgets": sh, "scrolls": [shop_scroll]},
                      {"id": "idv", "title": "iDiver", "widgets": iv, "scrolls": [idv_scroll]},
                      {"id": "call", "title": "Call", "widgets": cl, "scrolls": [call_scroll]},
                      *([{"id": "diag", "title": "Diagnostics", "widgets": dg}] if SHOW_DIAG else [])],
            # the bottom screen follows the player: SUSHI at the restaurant, DIVE in the sea
            # (game.mode from the module; it switches on arrival, after that the tabs are yours)
            # every page change goes through the module ("nav" -> ui.page, plus arriving at the sushi bar
            # or in the sea and OPEN RECIPE): it follows the page to switch fonts (DIVE wears the HUD's)
            "page_binds": []}
data = {"_game": "DAVE THE DIVER (Switch) 010097F018538000, update v1.0.6.1267",
        "_build_id": "F74B633E480E4E161F80A7C16A467F2A",
        "_layout": "IL2CPP v31: Il2CppClass.parent +0x58, static_fields +0xB8 (explicit; verified on device)",
        "points": points}

cg += [dict(w) for w in _ct]   # 0.38.40: on CARGO too
# the tab bar along the bottom of the main pages (INGREDIENTS belongs to SUSHI)
for page, active in ((dive, "dive"), (cg, "dive"), (su, "sushi"), (ig, "sushi"), (tp, "dive"), (ph, "phone"), (cc, "phone"), (ma, "phone"), (ec, "phone"), (sh, "phone"), (iv, "phone"), (cl, "phone")):   # 0.37.90: + WEAPON SHOP; 0.37.105: + IDIVER
    bar_w = tabbar(active)
    for w in bar_w:
        if "text_scale" in w:
            w["text_scale"] = drawn(w["text_scale"])
    page.extend(bar_w)

# the outer frame, drawn right over each page's background:
#  SUSHI / INGREDIENTS: the sushi bar's wooden panel (the game's own picture, its dark inside is the page);
#  every other page: the game's "Acquired" panel -- blue body, lighter header band, thin cyan edge.
ACQ = [rect([0, 0, 1240, 1080], ACQ_BODY), rect([6, 6, 1228, 90], ACQ_HEAD),
       {"type": "rect", "rect": [2, 2, 1236, 1076], "bg": "#00000000", "color": ACQ_EDGE, "frame": 3}]
WOOD = [game_frame([0, 0, 1240, 1080], "UI_Sushi_Pop_Panel")]
# 0.37.115: APPS is the game's phone -- its dark body (the colour of the game's own phone picture, PhoneBG) as the
# frame with the header in its top bezel, its wallpaper (PhoneBg, the centre, corners rounded) as the screen
# 0.37.116: every phone page (APPS and each app) wears the phone's body over the whole screen, as the sushi pages wear
# the wooden panel -- no blue around it; the edge a little lighter, as the game's phone picture has it
PHONE_BODY, PHONE_EDGE = "#FF2A2B38", "#FF4A4C5C"
PHONE = [rect([0, 0, 1240, 1080], PHONE_BODY),
         {"type": "rect", "rect": [2, 2, 1236, 1076], "bg": "#00000000", "color": PHONE_EDGE, "frame": 3}]
PHONE_SCREEN = [{"type": "image", "src": "module:bg/PhoneBg@1192x812r24", "color": "#FFFFFFFF", "rect": [24, 102, 1192, 812]}]
PHONE_PAGES = ("phone", "calc", "mail", "eco", "shop", "idv", "call")
for pg in manifest["pages"]:
    pg["widgets"][1:1] = [dict(w) for w in (WOOD if pg["id"] in ("sushi", "ingredients") else
                                            PHONE + (PHONE_SCREEN if pg["id"] == "phone" else []) if pg["id"] in PHONE_PAGES else ACQ)]

# 0.37.49: the loading picture -- the game's own title logo (read from the player's game files, 1474x656,
# drawn at half size by the module) over a plain background, on top of every page while ld.show is set.
# 0.37.85: as big on the device as the game's own logo on the top screen (photo: ~680 of 1770 px there; the bottom
# screen shows 1240 wide as ~1140 px) -> ~740 wide (0.37.84's 560 was too small, 0.37.60's 1000 too big)
LOGO_W, LOGO_H = 740, 329
LOGO_Y = 390 - LOGO_H // 2          # its middle near 36% of the height, as on the top screen
LOADING = [{"type": "rect", "rect": [0, 0, 1240, 1080], "bg": BG, "color": "#00000000", "frame": 0,
            "on_tap": "ld_test", "need_bind": "ld.show"},
           # 0.37.59: the title menu's own illustration (DR_Illust, the centre at the screen's shape) behind the logo;
           # until it can be read only the plain background shows
           {"type": "image", "src": "module:bg/DR_Illust", "color": "#FFFFFFFF", "rect": [0, 0, 1240, 1080], "need_bind": "ld.show"},
           {"type": "image", "src": "module:logo/DaveTheDiver_TitleLogo", "color": "#FFFFFFFF",
            "rect": [(1240 - LOGO_W) // 2, LOGO_Y, LOGO_W, LOGO_H], "need_bind": "ld.show"},
           # 0.37.60: "Eden Duo" under the logo in the game's font (as in the developer's other companion packages)
           label(620, LOGO_Y + LOGO_H + 28, "Eden Duo", "#FFFFFFFF", 5, align="center",
                 outline=BTN_SHADE, outline_px=3, need_bind="ld.show"),
           # 0.38.41: on the title menu, CONTINUE -- presses A (the game's menu starts on Continue) (user)
           *game_button([430, LOGO_Y + LOGO_H + 93, 380, 104], "Continue", "ld_continue", kind="ok", gate="ld.menu", sc=7)]   # 0.38.45: 5% smaller, as the game writes it   # 0.38.43: right under Eden Duo
for pg in manifest["pages"]:
    pg["widgets"].extend(dict(w) for w in LOADING)
    # 0.37.56: a page's scroll bar is drawn over its widgets -- hide it (and its gestures) under the loading picture
    for sc in pg.get("scrolls", []):
        if "show_bind" not in sc:
            sc["show_bind"] = "!ld.show"
        else:
            gate = f"{sc['id']}_scroll_vis"
            manifest["derived"].append(ex(gate, f"{sc['show_bind']} && !ld.show"))
            sc["show_bind"] = gate

# Page changes: the module keeps ui.page (its kPageDive / kPageSushi follow this page order) and a
# page bind per page shows it. The tabs and buttons ask the module ("nav" + the page's number).
PAGE_IDS = [pg["id"] for pg in manifest["pages"]]
assert PAGE_IDS.index("phone") == 7 and PAGE_IDS.index("eco") == 10 and PAGE_IDS.index("shop") == 11   # (the module counts it in kPages)   # the module's kPagePhone / kPageEco
assert PAGE_IDS.index("idv") == 12 and PAGE_IDS.index("call") == 13   # 0.37.102 / 0.37.109
assert PAGE_IDS.index("dive") == 0 and PAGE_IDS.index("sushi") == 4 and len(PAGE_IDS) == (15 if SHOW_DIAG else 14), PAGE_IDS
# (the module's kPages (12) must cover them: 11 without the DIAGNOSTICS page)
for name, act in manifest["actions"].items():
    if act.get("kind") == "page":
        manifest["actions"][name] = {"kind": "module", "action": "nav", "argument": PAGE_IDS.index(act["page"])}
manifest["page_binds"] = [{"point": "ui.page", "equals": k, "when_equal": {"page": pid, "transition": "fade", "duration_ms": 150}}
                          for k, pid in enumerate(PAGE_IDS)]
ROOT.mkdir(parents=True, exist_ok=True)
(ROOT / f"{BUILD16}.json").write_text(json.dumps(data, indent=1) + "\n")
(ROOT / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
pkg = json.loads((PKG / "package.json").read_text())
pkg.update({"name": "Dave the Diver dual screen", "version": VERSION, "min_runtime": 17})
(PKG / "package.json").write_text(json.dumps(pkg, indent=1) + "\n")
print("wrote", ROOT)

