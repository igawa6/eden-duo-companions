#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Skyward Sword HD companion package (packages/SkywardSwordHD/dualscreen).

The eight companion pages share runtime-decoded art. Writes manifest.json and the
per-build data file 5441AF3C89F97E1D.json. All pictures are `module:sshd:` keys that the native
module (native/modules/01002DA013484000.cpp, sshd_art.cpp) builds from the player's own romfs.

usage: gen_manifest.py [PACKAGE_DIR]
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PKG = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'packages', 'SkywardSwordHD')
TITLE = '01002DA013484000'
VERSION = '1.0.0'
BUILD = '5441AF3C89F97E1D25A8CCF14C742BF82336D8A8'
W, H = 1240, 1080
# dev.46 (owner pick 2026-11-15: "use alt A, but top bar like bag pouch and gear use alt B", from
# the button mockups): every button is the native choice plank (art
# choice/<w>/<h>/<state>[/1]: System2D messageBtn_00 / MenuPause N_options_00, the floor tabs' own
# family; state 0 normal, 1 selected, 2 pressed, 3 disabled; /1 = opaque body on the map paper) with
# its label in the pause Options style ('choice'); the Bag tab bar and the Gear header are the
# Options screen row (art optrow/<w>/<h>/<state>, label style 'optrow'). No footer uses the old
# plank/foot art any more (dev.48 removed the unused high-contrast footer variant). Visible bodies inside the
# pictures (offline render, alpha >= 0.5 down the centre column): choice rows 7..112 of 120 (6..94 of
# 100), optrow rows 2..96 of 104. Labels are centred on their cap box (FIT-RULES R1), not on the
# picture: CAP_CY = rows from a txt/ picture's top to the centre of a capital (rendered "HIH").
CHOICE_VIS = {120: (7, 112), 100: (6, 94)}
OPTROW_VIS = {104: (2, 96)}
CAP_CY = {22: 24.5, 24: 27.0, 26: 29.5, 28: 32.0, 30: 34.0, 32: 36.0}
INK_DX = 1  # a txt/ picture's ink sits 1 px right of its centre (the drop shadow widens it on the right)
FOOT_H, FOOT_BOTTOM = 120, 1064  # footer / Back planks end on the L / R corner planks' bottom
FOOT_TAP_Y, FOOT_TAP_H = 930, 1064 - 930
READY_FOOT_X, READY_FOOT_W = (228, 495, 762), 250  # 17 px gaps, 16 px to the L / R corner taps
TAB_ART_Y, TAB_ART_H = 14, 104  # the Bag tab rows' visible top on y 16, like dev.45's tabs
RESET_ART_H = 100  # the Reset plank inside its 200 x 112 tap rect, opaque on the paper
# P01 (owner: "for ux spec i only accept p01 and p02"): one refusal pill (art dopill/<w>/<h>, the HUD
# do-prompt window) for every "Not now" / "Put away", the word in the prompt's own style ('doinfo').
# dev.48: taller, and the word larger: font 24, whose capital stem measures 23 px of face (dev.46's
# font 22 measured 21, below the 22 px essential cap, and its ink came within 3 px of the gold rim).
# The window keeps the 44 px pill's rim (9 px bands top and bottom, sshd_art DoPill) and grows its
# navy body: 64 px outer = 46 px of navy; the word's ink stays 18 px from the top edge, 14 px from the
# bottom (the "y" of "Put away" with its shadow) and 4+ px from the rim. The Ready shield pill alone
# keeps font 22 in a 52 px pill: the gap between the Shield plank (body to y 212) and the rupee icon
# (from y 265) is 53 px (PILL_SHIELD).
PILL_W, PILL_H, PILL_CAP = 216, 64, 24
PILL_SHIELD_H, PILL_SHIELD_CAP = 52, 22
# pressed look (~150 ms, derived press.* on the module clock sshd.ms): one id per button; lit when its tap
# selects / clears it in "@sel:sshd.press" (only the buttons whose tap stays on the page) or SSHD_DIAG_PRESS names it
PRESS = {'map_reset': 1, 'fly_reset': 2, 'ready_gear': 3, 'ready_map': 4, 'ready_bag': 5, 'fly_gear': 6,
         'fly_bag': 7, 'back': 8, 'tab_Pouch': 9, 'tab_Collection': 10, 'tab_Options': 11}
# Bag > Pouch Item Check storage cells (dev.42): drag icon, its socket, count digit height
ICHECK_ICON, ICHECK_SOCKET, ICHECK_DIGIT_H = 92, 104, 40
ICHECK_PITCH, ICHECK_BAR_X, ICHECK_BAR_W = 108, 1100, 90  # dev.48: cell pitch, scroll strip x / width
MX, MY, MR = 620, 430, 350
CB = 196
# Ready right-column corners: socket + item centred in the button, glyph as a corner badge (owner 2026-11-11)
CORNER_SOCKET, CORNER_ICON, CORNER_GLYPH = 156, 132, 56
# Ready spin buttons between the corners (round socket diameter, tops of the two buttons): no
# caption (owner 2026-11-11), so the two sockets split the column between the ZR corner (bottom
# 16 + CB) and the R corner (top H - 16 - CB) into three equal gaps
SPIN_D = 170
_SPIN_GAP = ((H - 16 - CB) - (16 + CB) - 2 * SPIN_D) / 3
SPIN_TOP = (round(16 + CB + _SPIN_GAP), round(16 + CB + 2 * _SPIN_GAP + SPIN_D))
PILL_GAP = 8  # dev.48: socket bottom to refusal pill top (R11 gutter)
# Ready L-toggle corner: the off-state sword (hm_sword*, 125 x 384 art) drawn at 45 degrees
L_SWORD = (50, 154)
L_LOCK, L_LOCK_X = 64, 12  # the L corner's lock badge (art doorkey: the native locked-door keyhole), px
# Ready stamina ring (owner 2026-11-11, "circle big outline around map circle"): radii just outside
# the map frame's rim (MR + 18), 100 segments of 3.6 degrees, each +-2.15 degrees so neighbours
# overlap (no seams). Must match the stamseg / stamtrack boxes in native/modules/sshd_art.cpp.
STAM_R = (372, 392)
STAM_HALF_MDEG = 2150
STAM_SEGMENTS = 100
# Ready area title (owner 2026-11-11: vertically centred between the circle and the footer tabs):
# the glyphs' ink (rows 14..55 of the 67 px txt/title/34 label, caps + shadow) is centred between
# the circle's visible bottom (the frame rim MY + MR + 18 and its drop shadow, about 12 px more)
# and the top of the footer planks (930 + their 2 px rim).
TITLE_INK = (14, 55)
TITLE_GAP = (MY + MR + 18 + 12, 930 + 2)
TITLE_Y = round(sum(TITLE_GAP) / 2 - sum(TITLE_INK) / 2)
# Native compass (sshd_art Compass: 128 layout units, the disc 88): drawn so the disc keeps the
# old cap's size, centred where the old cap was
COMPASS = 150
RUPEE_Y = 290           # rupee icon centre in the left pane (up 50 px to make room for the D-pad)
CRYSTALS = (300, 20)    # crystal icon; its digits follow at +66 / +18, clear of the ring
# Ready D-pad cluster (display only): cross centre, and scale against the textures' own pixels
# (1.1 x the textures; the native top screen draws the 112 px cross textures at about 104 px)
DP = (98, 690)
FLY_DP = (1030, 730)  # the flying page's copy (dev.33), bottom right of the sky map
FLY_DPAD_BOX = (930, 450, 1215, 902)  # its screen zone (x0, y0, x1, y1), also in the module's kFlyAvoid
DP_S = 1.1
DP_CAP = 26  # HUD caption font size (native T_helpBtn_00, scaled with the cluster)
ALPHA_LEVELS = ['#00FFFFFF'] + [f'#{i * 17:02X}FFFFFF' for i in range(1, 16)]  # sshd.dp.* levels 0..15
K = 'module:sshd:'

# Must match kAreas / kMapPad in native/modules/sshd_art.cpp.
PAD = 40
AREAS = [
    # stage, xmin, xmax, zmin, zmax, ppu, title
    ('F401', -20480.32, 7684.18, -7573.23, 15558.50, 0.07, 'Sealed Grounds'),
    ('F000', -12000.00, 16000.00, -16500.00, 10500.00, 0.065, 'Skyloft'),
    ('F100', -20205.19, 15220.30, -13870.62, 17657.00, 0.055, 'Faron Woods'),
    ('F200', -27285.70, 32744.30, -51405.40, 16608.50, 0.029, 'Eldin Volcano'),
    ('F300', -42247.30, 18011.90, -17744.30, 24101.20, 0.033, 'Lanayru Desert'),
    ('F301', -5000.00, 17500.00, -13000.00, 11000.00, 0.0843, 'Lanayru Sand Sea'),
    ('F302', -32653.30, 25319.10, -14750.40, 22875.10, 0.04, 'Lanayru Gorge'),
    ('F303', -6500.00, 5945.44, -6213.61, 6700.00, 0.15, 'Lanayru Caves'),
    # dev.33 (static map analysis C5): the Shipyard (one room, +x right / +z
    # down from the hero's native capture; Gortram's balloon) and the sailing Sand Sea (its sea-chart
    # state is the past background, module sshd.fmap.src)
    ('F301_4', -38500.00, 19800.00, -17800.00, 21200.00, 0.04, 'Shipyard'),
    ('F301_1', -141400.00, 8800.00, -46200.00, 55800.00, 0.0155, 'Lanayru Sand Sea'),
    # dungeons: all rooms in one stage-world frame (the map art notes "Dungeons"); bboxes =
    # all room.kcl vertices of <id>_stg_l0 (2026-11-07 open-items pass); ppu keeps each picture under
    # 16 MiB. The native dungeon maps have +z at the top (flip_v, live Skyview Temple capture).
    ('D100', -12360.00, 9789.19, -20613.68, 9144.76, 0.07, 'Skyview Temple'),
    ('D200', -12300.81, 12374.02, -19320.00, 17557.12, 0.06, 'Earth Temple'),
    ('D101', -18220.10, 12400.00, -13649.60, 7030.71, 0.075, 'Ancient Cistern'),
    ('D201', -14268.70, 11420.00, -23078.00, 9030.00, 0.068, 'Fire Sanctuary'),
    ('D300', -9100.00, 11100.00, -26100.00, 5000.00, 0.075, 'Lanayru Mining Facility'),
    ('D301', -15000.00, 8300.00, -10000.00, 10000.00, 0.08, 'Sandship'),
    # Sky Keep (dev.31): the native 3 x 3 tile map of rooms D003_0..8, one picture in the tile
    # frame (sshd_art.h kSkyKeep*: dev.32 the native map dummy's tiles, 7000 units with their door
    # stubs, pitch 7000, 0.075 px per unit, u = x + (col - 1) pitch, v = -z - (row - 1) pitch); the
    # module draws the live arrangement into it and publishes Link in that frame (sshd.sk.lu / lv)
    ('D003', -10500.00, 10500.00, -10500.00, 10500.00, 0.075, 'Sky Keep'),
    # Isle of Songs interior (dev.30; the native map shows the room)
    ('F010r', -3731.92, 3731.92, -3744.08, 5489.41, 0.2, 'Isle of Songs'),
    # Native 0-Common/007-MapText.msbt MAP_18_S..MAP_21_S; overworld-style frames.
    # dev.34 (item 1, live native trial maps of all four realms): S100 / S200 show the realm only as
    # a small blob under Link (about 325 world units per top-screen px, no zoom): small pictures
    # (0.02 px per unit), every view at TRIAL_UPX; S000 shows the whole realm (Skyloft's stage
    # view, no zoom): its own picture at 0.04, every view the whole realm; S300 shows no realm at
    # all, only Link: a blank picture (REALM_* below)
    ('S000', -20000.00, 25000.00, -26314.60, 25000.00, 0.04, "The Goddess's Silent Realm"),
    ('S100', -18243.00, 15217.40, -13852.70, 17545.30, 0.02, "Farore's Silent Realm"),
    ('S200', -27285.70, 32744.30, -51405.40, 16608.50, 0.02, "Din's Silent Realm"),
    ('S300', -42247.30, 18011.90, -17744.30, 24101.20, 0.02, "Nayru's Silent Realm"),
]
# dev.34 (item 7, stage survey): each area's native map name = its STIF map-name
# id (stage.bzs STIF +0x12) in 0-Common/007-MapText.msbt. The Ready title is MAP_<id>_S (one line;
# the AREAS title must equal it, test_manifest.py), the Map page plate MAP_<id> with the game's own
# line breaks (art ctitle: the native plate draws "Farore's / Silent Realm" on two lines).
MAP_NAME_ID = {'F401': 1, 'F000': 0, 'F100': 2, 'F200': 5, 'F300': 8, 'F301': 10, 'F302': 9, 'F303': 40,
               'F301_4': 44, 'F301_1': 10, 'D100': 11, 'D200': 12, 'D101': 14, 'D201': 16, 'D300': 13,
               'D301': 15, 'D003': 17, 'F010r': 30, 'S000': 21, 'S100': 18, 'S200': 20, 'S300': 19}
# dev.34 (item 2): the generic sub-stage frame (module PublishGenMap, sshd_art.h kGenHalf / kGenPx):
# one square picture of kGenPx px for 2 * GEN_HALF frame units at 0.0625 px per unit plus the pad.
GEN_HALF, GEN_PPU = 12160.0, 0.0625
GEN_EXT = GEN_HALF + PAD / GEN_PPU
GEN_READY_WINDOW = 14000  # frame units across the Ready minimap (a room about fills the circle)
GEN_TITLE_W, GEN_PLATE_W, GEN_PLATE_TEXT_W = 760, 680, 520  # module kGenTitleW / kGenPlateW / kGenPlateTextW
# dev.34 (item 1): Silent Realms (native trial map: the realm a ~94 px blob on the 1920 px top screen
# for S100's 30600 drawn units, Link's head and cone over it, the plate purple (map type 8), no zoom)
REALMS = ('S000', 'S100', 'S200', 'S300')
REALM_BLOB = ('S100', 'S200')  # the realm a small blob at TRIAL_UPX
REALM_WHOLE = 'S000'           # the whole realm in every view
REALM_NONE = 'S300'            # no realm drawn
S000_SPAN = 26000              # S000's drawn faces (x -9539..14466, z -13631..7395) with a margin
TRIAL_UPX = 200  # world units per page px: the realm blob keeps its native size against Link's 84 px head (native: ~50 px head, 325 units per px)
# dungeon stage -> dungeon-flag scene index (slot+0xA64 + 16*index: u16[8]; [0] bit 0x02 = Dungeon Map,
# 0x80 = boss key; [7] = rooms visited). Indices from ssrando MAP_TO_FLAGINDEX; Skyview map bit
# confirmed live 2026-11-07 (clearing it hides the native map layout).
DUNGEON_INDEX = {'D100': 11, 'D101': 12, 'D200': 14, 'D201': 15, 'D300': 17, 'D301': 18, 'D003': 20}
DUNGEONS = set(DUNGEON_INDEX)
# dev.32: field areas whose picture follows a live state (module PublishFieldMap: the Lanayru past
# variant in F300, the Sealed Grounds by the temple's own geometry in F402)
FIELD_STATE = {'F300', 'F401', 'F301_1'}
# native +x right / +z down, live map captures (D100 / D200 / D300 / Isle of Songs: dev.30; Sky Keep's
# tiles, live D003_0: dev.31; overworld fields and their Silent Realms: dev.32, native F000 / F200 / F300
# captures and offline renders of the map colour analysis). The Sealed
# Grounds keep +x left / +z up (native view-cone calibration).
NATIVE_XZ = {'F000', 'F100', 'F200', 'F300', 'F301', 'F302', 'F303', 'F301_4', 'F301_1', 'D100', 'D200', 'D300', 'D101', 'D201', 'D301',
             'F010r', 'D003', 'S000', 'S100', 'S200', 'S300'}
FLIP = DUNGEONS | NATIVE_XZ
# Stages drawn with another stage's picture: F402 is the Sealed Grounds by the temple (same world
# coordinates as F401; the game's own map titles it Sealed Grounds; live 2026-11-07, hero save).
ALIASES = {'F401': ['F402'], 'D201': ['D201_1'], 'D300': ['D300_1'], 'D301': ['D301_1'],
           'D003': [f'D003_{i}' for i in range(9)]}
# The sky (stage F020): picture built by SkyMap() from the stage's island actors.
SKY = ('F020', -180000.0, 180000.0, -140000.0, 110000.0, 0.0055)
# Fly page view: the whole Sky picture (X 360000 + margins across 1200 px; its 250000 Z rows fit the
# 882 px panel), so no sky marker sits outside the view or is cut at the top edge (owner
# 2026-11-11). The old 300000 window (about the native scale, 530000 across 1920 px) framed only
# 220500 units of Z, so tags north of Link were clipped at the top.
SKY_FOLLOW = 380000
# dev.35 (item 5): Inside the Thunderhead (F023) shares the Sky's world frame (its room repeats the
# Sky's RockSky / IslTrea / MapMark actors at the same x, z) and the native map draws it as the sky
# map (live dev.35 hero F023: the same islands, rocks and thunderhead ring as the native F020 map,
# the whole sky, Link inside the ring, title MAP_57 on the teal plate). The companion draws the Sky
# picture with Link at (x, -z): the Ready minimap and the Map page's near view around Link, the far
# view the whole Sky like the native.
THUNDERHEAD_STAGE, THUNDERHEAD_NAME_ID, THUNDERHEAD_WINDOW = 'F023', 57, 150000
# Altitude gauge: Link y mapped linearly between the native gauge's cloud (bottom) and sun (top).
# Fitted live 2026-11-07 (owner label, three readings against the HUD gauge): about -31000 at
# the cloud and +33000 at the sun. MEDIUM: three points, linearity not proven.
ALT_LO, ALT_HI = -31000.0, 33000.0
# Rendered widths of the title labels (txt/title/34/<name>, 67 px tall), measured with
# dsmod-sshd-art-test; the image is drawn 1:1, centred under the minimap.
TITLE_W = {'Lanayru Sand Sea': 523, 'Shipyard': 263, 'Lanayru Gorge': 429, 'Lanayru Caves': 429, 'Eldin Volcano': 408, 'Lanayru Desert': 445, 'Sealed Grounds': 445, 'Skyloft': 217, 'Faron Woods': 365, 'Skyview Temple': 475, 'Earth Temple': 390,
           'Ancient Cistern': 455, 'Fire Sanctuary': 429, 'Lanayru Mining Facility': 710, 'Sandship': 267,
           'Sky Keep': 275, 'Isle of Songs': 374, "The Goddess's Silent Realm": 784, "Farore's Silent Realm": 613, "Din's Silent Realm": 532,
           "Nayru's Silent Realm": 603, 'The Sky': 243}
FOLLOW_WINDOW = 8000  # base window: the Map page's near view shows 1.8x this
# dev.38 Map page (owner 2026-11-12: pinch zoom, drag, Reset): one map rect for every area, the old
# near views as each view's home (zoom 1, follows Link while untouched), the Reset plank where the
# Zoom plank was (module kMapReset, plate-avoid block)
MAP_RECT = (20, 20, W - 40, 884)
MAP_NEAR_WINDOW = FOLLOW_WINDOW * 1.8
GEN_NEAR_WINDOW = GEN_READY_WINDOW * 1.4
THUNDERHEAD_NEAR_WINDOW = THUNDERHEAD_WINDOW * 1.4
# dev.41 (legibility minimums: primary controls >= 112 x 112): the Reset tap rect is 200 x 112 and the
# plank (frame box = tap rect + 16) 216 x 128; the Map page's plank moved up 22 px to keep its bottom
RESET_W, RESET_H = 200, 112
RESET_PLANK = (W - 236, 778)        # the Map page's Reset (module kMapReset)
FLY_RESET = (W - 236, 190)          # the Fly page's Reset, under the compass (module kFlyAvoid)
# Pinch limits (pan_view): out to the whole picture (centred once the view outgrows it: the area's
# clamp_view), in until a picture px covers MAP_MAX_MAG screen px, but never less than
# MAP_ZOOM_IN_MIN x the home view (closer than the old near view) and at most MAP_ZOOM_IN_CAP.
# The pictures are already at the module's 16 MiB per-picture cap (sshd_art.cpp kAreas ppu).
MAP_MAX_MAG, MAP_ZOOM_IN_MIN, MAP_ZOOM_IN_CAP = 3.0, 2.0, 4.0
# Ready minimap: 1.5x the old 8000-unit window (owner 2026-11-11, "zoom out a bit"). The runtime draws
# Link, beacons and Goddess Chests with the same follow transform, so they stay in place.
READY_WINDOW = 12000  # world units across the 700 px minimap
# dev.38 Ready swipe pad: the gold trail (sshd::Stroke::kTrailPoints dots; alpha level 0..15 -> the
# game's gold at that alpha, the native HUD rims' gold (255, 231, 135))
TRAIL_DOTS, TRAIL_PX = 24, 88
TRAIL_TINTS = [f'#{i * 17:02X}FFE787' for i in range(16)]

ITEM_ICONS = ['uc_bomb_00', 'uc_bowA_00', 'uc_hookShot_00', 'uc_beetleA_00', 'uc_pachinkoA_00',
               'tr_vacuumIcon_00', 'tr_whip_00', 'uc_bowA_00', 'uc_netA_00']
ITEM_ICONS = {
    70: 'uc_medalHunter_00', 72: 'uc_bottleFairy_00', 74: 'uc_bottleHoly_00', 78: 'uc_bottleKusuriA_00',
    79: 'uc_bottleKusuriB_00', 81: 'uc_bottleKusuriC_00', 84: 'uc_bottleGutsA_00', 85: 'uc_bottleGutsB_00',
    86: 'uc_bottleAirA_00', 87: 'uc_bottleAirB_00', 88: 'uc_bottleFairy_00', 65: 'uc_bottleMutekiA_00',
    66: 'uc_bottleMutekiB_00', 54: 'uc_bottleWater_00', 55: 'uc_bottleKinokoA_00', 36: 'uc_bottleKinokoB_00',
    100: 'uc_medalHeart_00', 101: 'uc_medalLucky_00', 102: 'uc_medalReturn_00', 103: 'uc_medalDurable_00',
    104: 'uc_medalDemon_00', 114: 'uc_medalLife_00', 126: 'uc_bottleRepairA_00', 127: 'uc_bottleRepairB_00',
    194: 'uc_bottleRepairC_00', 128: 'uc_spareSeedA_00', 129: 'uc_spareSeedB_00', 130: 'uc_spareSeedC_00',
    131: 'uc_spareQuiverA_00', 132: 'uc_spareQuiverB_00', 133: 'uc_spareQuiverC_00', 134: 'uc_spareBombBagA_00',
    135: 'uc_spareBombBagB_00', 136: 'uc_spareBombBagC_00', 153: 'uc_bottleEmpty_00', 195: 'uc_bottlePumpkinA_00',
    196: 'uc_bottlePumpkinB_00', 116: 'uc_shieldWoodA_00', 117: 'uc_shieldWoodB_00', 118: 'uc_shieldWoodC_00',
    119: 'uc_shieldIronA_00', 120: 'uc_shieldIronB_00', 121: 'uc_shieldIronC_00', 122: 'uc_shieldHolyA_00',
    123: 'uc_shieldHolyB_00', 124: 'uc_shieldHolyC_00', 125: 'uc_shieldHylia_00',
}
SHIELDS = ['uc_shieldWoodA_00', 'uc_shieldWoodB_00', 'uc_shieldWoodC_00', 'uc_shieldIronA_00',
           'uc_shieldIronB_00', 'uc_shieldIronC_00', 'uc_shieldHolyA_00', 'uc_shieldHolyB_00',
           'uc_shieldHolyC_00', 'uc_shieldHylia_00']  # item ids 0x74..0x7D in cheat-list order

# Sky Goddess tags as the native draws them (map-marker contract 2026-11-11): the MapSky tag
# (nh_goddessTBox_00, module text sshd.gc.src) on each Goddess Chest whose cube is struck and which
# is still closed (sshd.gc<i>.on, module PublishGoddessChests); never cube positions, Sky only.
GODDESS_CHESTS = {'group': 'goddess_chests', 'count': 22, 'x': 'sshd.gc{i}.u', 'y': 'sshd.gc{i}.v',
                  'kind': 'sshd.gc{i}.on', 'hide_when_kind': 0, 'icon_src_bind': 'sshd.gc.src', 'size': 48}
# Opened ordinary chests (contract: LIVE high): the native opened-chest icon (art openchest, 23
# layout units = 80 of its 100 px; drawn at the native 16-unit size against the 25-unit head =
# 84 px) where the module finds the chest's flag set (sshd.ch<i>.on; only the stages whose BZS was
# decoded, layer 0; everything else hidden).
OPEN_CHESTS = {'group': 'open_chests', 'count': 5, 'x': 'sshd.ch{i}.u', 'y': 'sshd.ch{i}.v',
               'kind': 'sshd.ch{i}.on', 'hide_when_kind': 0, 'icon_src_bind': 'sshd.ch.src', 'size': 68}
OPEN_CHEST_STAGES = ('F401', 'F020')  # F402 draws with F401's picture; dungeons: DUNGEON_CHESTS
# dev.27 live map objects (module PublishLiveMarkers walks the game's own object tree like the
# native map, map marker analysis). Marker art is drawn at 4 px per layout
# unit on a square canvas centred on the object; the field pages draw a layout unit at 3.36 px
# (the 25-unit head = 84 px), the Fly page at about 2.1 px (the 23-unit Goddess tag = 48 px).
FIELD_UNIT, SKY_UNIT = 84 / 25, 48 / 23
# bird statues: activated (checkStatueFlag: the scene flag params & 0xFF) or Map-revealed in a
# dungeon, in the statue type's colour (art statue/<type>/<sky>, 16-unit canvas)
STATUES = {'group': 'statues', 'count': 8, 'x': 'sshd.st{i}.u', 'y': 'sshd.st{i}.v', 'kind': 'sshd.st{i}.on',
           'hide_when_kind': 0, 'icon_src_bind': 'sshd.st{i}.src', 'size': round(16 * FIELD_UNIT)}
SKY_STATUES = {**STATUES, 'size': round(16 * SKY_UNIT * 1.0)}
# door bars (art door/<deg>, 30-unit canvas, the bar 26 x 13 units); dungeons up to 16 (Skyview
# draws 13)
DOORS = {'group': 'doors', 'count': 24, 'x': 'sshd.dr{i}.u', 'y': 'sshd.dr{i}.v', 'kind': 'sshd.dr{i}.on',
         'hide_when_kind': 0, 'icon_src_bind': 'sshd.dr{i}.src', 'size': round(30 * FIELD_UNIT)}
# dungeon markers (dev.30, module PublishDungeonMarkers; dungeon marker analysis):
# small-key locks over their bars (art doorkey, 30-unit canvas), chests opened / closed (art
# openchest / closedchest, drawn like OPEN_CHESTS), the red X of scripted type-0 marks (art mark,
# 20-unit canvas) and the boss door's objective (art objective/<kind>, 64-unit canvas)
KEYS = {'group': 'door_keys', 'count': 8, 'x': 'sshd.dk{i}.u', 'y': 'sshd.dk{i}.v', 'kind': 'sshd.dk{i}.on',
        'hide_when_kind': 0, 'icon_src_bind': 'sshd.dk.src', 'size': round(30 * FIELD_UNIT)}
DUNGEON_CHESTS = {'group': 'dungeon_chests', 'count': 16, 'x': 'sshd.dc{i}.u', 'y': 'sshd.dc{i}.v',
                  'kind': 'sshd.dc{i}.on', 'hide_when_kind': 0, 'icon_src_bind': 'sshd.dc{i}.src', 'size': 68}
MARKS = {'group': 'marks', 'count': 6, 'x': 'sshd.mx{i}.u', 'y': 'sshd.mx{i}.v', 'kind': 'sshd.mx{i}.on',
         'hide_when_kind': 0, 'icon_src_bind': 'sshd.mx.src', 'size': round(20 * FIELD_UNIT)}
# dev.34 (item 9): Sky Keep's Triforce pieces (module kSkyKeepIcons 't': d3icon Item records, drawn
# with the Dungeon Map while the piece's scene flag is clear; art triforce, 26-unit canvas)
TRIFORCE = {'group': 'triforce', 'count': 3, 'x': 'sshd.tf{i}.u', 'y': 'sshd.tf{i}.v', 'kind': 'sshd.tf{i}.on',
            'hide_when_kind': 0, 'icon_src_bind': 'sshd.tf.src', 'size': round(26 * FIELD_UNIT)}
OBJECTIVE = {'group': 'objective', 'count': 1, 'x': 'sshd.ob{i}.u', 'y': 'sshd.ob{i}.v', 'kind': 'sshd.ob{i}.on',
             'hide_when_kind': 0, 'icon_src_bind': 'sshd.ob{i}.src', 'size': round(64 * FIELD_UNIT)}
# neighbouring-area plates (art place/<label>/<deg>/<region>: a 900 px canvas drawn 1:1, the plate
# outside its mark as the native places it); the Map page only (on the Ready minimap they would
# cover most of the circle)
PLACES = {'group': 'places', 'count': 4, 'x': 'sshd.pl{i}.u', 'y': 'sshd.pl{i}.v', 'kind': 'sshd.pl{i}.on',
          'hide_when_kind': 0, 'icon_src_bind': 'sshd.pl{i}.src', 'size': 900}
# Sky layers (module PublishSkyLayers): the thunderhead at its native world size (capture 07: the
# 281.6-unit cloud ring is about 82800 units in radius; 176500 for the 300-unit art canvas drew the
# ring at 80 % of that on the live Fly page, so the canvas is 220000 units), the
# surface light columns (art column/<region>; 46 px native flowers, scaled like the map), and
# the discovered island names (art islename/<i>/<gap>, a 640 px canvas drawn 1:1)
THUNDERHEAD = {'group': 'thunderhead', 'count': 1, 'x': 'sshd.th.u', 'y': 'sshd.th.v', 'kind': 'sshd.th.on',
               'hide_when_kind': 0, 'icon_src_bind': 'sshd.th.src', 'size_world': 220000}
COLUMNS = {'group': 'columns', 'count': 3, 'x': 'sshd.col{i}.u', 'y': 'sshd.col{i}.v', 'kind': 'sshd.col{i}.on',
           'hide_when_kind': 0, 'icon_src_bind': 'sshd.col{i}.src', 'size': 72}
ISLAND_NAMES = {'group': 'island_names', 'count': 7, 'x': 'sshd.isl{i}.u', 'y': 'sshd.isl{i}.v',
                'kind': 'sshd.isl{i}.on', 'hide_when_kind': 0, 'icon_src_bind': 'sshd.isl{i}.src', 'size': 640}
# dev.31 (module PublishLiveMarkers; NPC map mark analysis): NPC speech
# balloons of portrait map marks whose native condition holds (art balloon/<icon>, an 80-unit
# canvas centred on the mark: the frame's tail meets it), field landmark glyphs (art cave, 20-unit
# canvas) and Beedle's airshop (art beedle/1, 48-unit canvas) by day
BALLOONS = {'group': 'balloons', 'count': 4, 'x': 'sshd.nb{i}.u', 'y': 'sshd.nb{i}.v', 'kind': 'sshd.nb{i}.on',
            'hide_when_kind': 0, 'icon_src_bind': 'sshd.nb{i}.src', 'size': round(80 * FIELD_UNIT)}
SKY_BALLOONS = {**BALLOONS, 'size': round(80 * SKY_UNIT)}
CAVES = {'group': 'caves', 'count': 2, 'x': 'sshd.cv{i}.u', 'y': 'sshd.cv{i}.v', 'kind': 'sshd.cv{i}.on',
         'hide_when_kind': 0, 'icon_src_bind': 'sshd.cv.src', 'size': round(20 * FIELD_UNIT)}
BEEDLE = {'group': 'beedle', 'count': 1, 'x': 'sshd.bs{i}.u', 'y': 'sshd.bs{i}.v', 'kind': 'sshd.bs{i}.on',
          'hide_when_kind': 0, 'icon_src_bind': 'sshd.bs{i}.src', 'size': round(48 * FIELD_UNIT)}
SKY_BEEDLE = {**BEEDLE, 'size': round(48 * SKY_UNIT)}
# dev.33 (Skyloft analysis): Skyloft's red common targets (Sky class: type-4
# marks of icon 0 / 255 / >= 25, the Academy 2 and the Bazaar 3; art target/<big>, 20-unit canvas,
# module sshd.tg0..15) and the Bug Medal's bug locations (art bug, 20-unit canvas, sshd.bg0..15), drawn
# under the door bars and statues like the native category order
TARGETS = {'group': 'targets', 'count': 16, 'x': 'sshd.tg{i}.u', 'y': 'sshd.tg{i}.v', 'kind': 'sshd.tg{i}.on',
           'hide_when_kind': 0, 'icon_src_bind': 'sshd.tg{i}.src', 'size': round(20 * FIELD_UNIT)}
BUGS = {'group': 'bugs', 'count': 16, 'x': 'sshd.bg{i}.u', 'y': 'sshd.bg{i}.v', 'kind': 'sshd.bg{i}.on',
        'hide_when_kind': 0, 'icon_src_bind': 'sshd.bg.src', 'size': round(20 * FIELD_UNIT)}
# Kikwi faces (art kikwi/<elder>, 40-unit canvas; module sshd.kk0..4)
KIKWI = {'group': 'kikwi', 'count': 5, 'x': 'sshd.kk{i}.u', 'y': 'sshd.kk{i}.v', 'kind': 'sshd.kk{i}.on',
         'hide_when_kind': 0, 'icon_src_bind': 'sshd.kk{i}.src', 'size': round(40 * FIELD_UNIT)}
# field maps that can show neighbouring-area plates (the native draws them on field maps only)
PLACE_STAGES = ('F401', 'F100', 'F200', 'F300', 'F301', 'F302', 'F303')


# Map page floor tabs (art floortab: 130 x 76 layout units at 2 px), left column under the title
# plate (its board ends at y 146), clear of the footer; must match the module's kFloorTabs (5)
FLOOR_TABS = 5
FLOOR_TAB_W, FLOOR_TAB_H = 182, 106
# dev.41 (legibility minimums): each tab's tap rect is 112 tall (the 106 px native art centred in it) with
# 12 px between rects
FLOOR_TAB_TAP_H = 112
FLOOR_TAB_X, FLOOR_TAB_Y, FLOOR_TAB_STEP = 30, 156, 124


def cone_marker(xb, yb):
    """Link's view cone (art cone/<deg>, module PublishCone): one pre-turned picture centred on
    Link, drawn before (under) the head marker; hidden with sshd.cone.on = 0."""
    return {'group': 'cone', 'count': 1, 'x': xb, 'y': yb, 'kind': 'sshd.cone.on', 'hide_when_kind': 0,
            'icon_src_bind': 'sshd.cone.src', 'size': 400}


# dev.37: the cone's prefetch (module sshd_cone.h): the angle Link turned to, requested from the host
# by this marker far outside every view (sshd.cone.pfu / pfv = 1e7 map units) while the cone marker
# keeps the last loaded picture, so the cone never blanks while a picture loads
CONE_PREFETCH = {'group': 'conepf', 'count': 1, 'x': 'sshd.cone.pfu', 'y': 'sshd.cone.pfv', 'kind': 'sshd.cone.pf',
                 'hide_when_kind': 0, 'icon_src_bind': 'sshd.cone.pfsrc', 'size': 400}

# Slot of the live save file in main .bss (verified live).
SLOT = 0x5AEAD54


def stage_u64(name):
    b = name.encode('ascii').ljust(8, b'\0')
    return struct.unpack('<Q', b)[0]


def f32(x):
    return float(struct.unpack('<f', struct.pack('<f', x))[0])


def area_box(xmin, xmax, zmin, zmax, ppu):
    w = int(f32(f32(xmax) - f32(xmin)) * f32(ppu)) + 2 * PAD
    h = int(f32(f32(zmax) - f32(zmin)) * f32(ppu)) + 2 * PAD
    u0 = -xmax - PAD / ppu
    v1 = zmax + PAD / ppu
    return [round(u0, 3), round(v1 - h / ppu, 3)], [round(u0 + w / ppu, 3), round(v1, 3)]


def area_box_flip(xmin, xmax, zmin, zmax, ppu):
    # flip_v picture: pixel_y = (z - zmin) * ppu + PAD, marker v = -z (top of the picture = max v)
    w = int(f32(f32(xmax) - f32(xmin)) * f32(ppu)) + 2 * PAD
    h = int(f32(f32(zmax) - f32(zmin)) * f32(ppu)) + 2 * PAD
    u0 = -xmax - PAD / ppu
    v1 = -zmin + PAD / ppu
    return [round(u0, 3), round(v1 - h / ppu, 3)], [round(u0 + w / ppu, 3), round(v1, 3)]


def pan_view(area, ppu, win, rect=MAP_RECT):
    """dev.38: the pan_zoom keys of a map widget whose home is a follow_window of `win` map units
    across `rect` over `area`'s picture (`ppu` picture px per map unit). Zoom 1 = the home view.
    min_zoom: the whole picture fits (never above 1); max_zoom: a picture px spans MAP_MAX_MAG
    screen px, kept within MAP_ZOOM_IN_MIN .. MAP_ZOOM_IN_CAP."""
    rw, rh = rect[2], rect[3]
    sx, sy = area['max'][0] - area['min'][0], area['max'][1] - area['min'][1]
    whole = max(sx, sy * rw / rh)
    zmin = math.floor(min(1.0, win / whole) * 1000) / 1000
    zmax = round(min(MAP_ZOOM_IN_CAP, max(MAP_ZOOM_IN_MIN, MAP_MAX_MAG * ppu * win / rw)), 2)
    return {'pan_zoom': True, 'min_zoom': zmin, 'max_zoom': zmax}


def vbind(st):
    return 'sshd.sk.lv' if st == 'D003' else 'link.vf' if st in FLIP else 'link.v'


def ubind(st):
    """Link's map u: Sky Keep's tile frame (module), x where the native map has +x right, else -x."""
    return 'sshd.sk.lu' if st == 'D003' else 'link.x' if st in NATIVE_XZ else 'link.u'


def img(src, x, y, w, h, **kw):
    d = {'type': 'image', 'src': src, 'rect': [x, y, w, h]}
    d.update(kw)
    return d


def shield_icon(x, y, w, h):
    return img(K + 'sq/DoButton/uc_shieldWoodA_00', x, y, w, h, bind='shield.id', add=-116,
               src_names=[K + 'sq/DoButton/' + n for n in SHIELDS], need_bind='shield.valid')


def sword_icon(x, y, w, h):
    return img(K + 'lyt/MenuPause/hm_swordA_00', x, y, w, h, src_bind='sshd.sword.src',
               need_bind='sshd.sword.has')


def wrong_build_notice():
    """The unsupported-build notice: a full-screen block and two lines in the runtime's built-in
    font (4 px x text_scale per character), shown while build_match is 0."""
    out = [{'type': 'rect', 'rect': [0, 0, W, H], 'bg': '#F01A1810', 'color': '#00000000',
            'frame': 0, 'input_block': True, 'need_bind': 'ui.wrong_build'}]
    for i, line in enumerate(WRONG_BUILD_TEXT):
        tw = len(line) * 4 * 6
        out.append({'type': 'label', 'rect': [(W - tw) // 2, 470 + i * 60, tw, 40], 'text_scale': 6,
                    'color': '#FFE6D29C', 'need_bind': 'ui.wrong_build', 'text': line})
    return out


WRONG_BUILD_TEXT = ['This companion supports', 'Skyward Sword HD 1.0.1 only']


def digit_widget(x, y, w, h, bind, **kw):
    return img(K + f'digit/0/{h}', x, y, w, h, bind=bind, src_format=K + f'digit/%d/{h}', **kw)


POUCH_SOURCES = [K + 'sq/DoButton/' + ITEM_ICONS.get(i, 'uc_bottleEmpty_00') for i in range(200)]


def pouch_icon(i, x, y, size):
    return img(POUCH_SOURCES[0], x, y, size, size, bind=f'pouch.id{i}', src_names=POUCH_SOURCES,
               need_bind=f'pouch.icon{i}')


def pouch_digits(i, x, y):
    return [digit_widget(x + k * 27, y, 33, 44, bind=f'pouch.n{i}d{k}', need_bind=f'pouch.cnt{i}')
            for k in range(2)]


def text_img(style, cap, text, cx, y, w, h=62, **kw):
    return img(f'{K}txt/{style}/{cap}/{text}', cx - w // 2, y, w, h, **kw)


def cap_label(style, cap, text, cx, cy, **kw):
    """dev.46: a txt/ label drawn 1:1 with its cap box centred on (cx, cy) (FIT-RULES R1 / R2)."""
    w = TW[('plank', cap, text)]
    return img(f'{K}txt/{style}/{cap}/{text}', round(cx - w / 2 - INK_DX), round(cy - CAP_CY[cap]), w, CAP_H[cap], **kw)


def choice_button(x, y, w, h, text, cap, press, opaque=False, **kw):
    """dev.46 alt A: the choice plank picture at (x, y, w, h), its pressed look (shown while the module
    lights `press`) and the label cap-centred on the plank's visible body. The tap rects are the
    caller's (several per plank where need_binds pick the action)."""
    v0, v1 = CHOICE_VIS[h]
    sfx = '/1' if opaque else ''
    return [img(f'{K}choice/{w}/{h}/0{sfx}', x, y, w, h, **kw),
            img(f'{K}choice/{w}/{h}/2{sfx}', x, y, w, h, need_bind=f'press.{PRESS[press]}'),
            cap_label('choice', cap, text, x + w / 2, y + (v0 + v1) / 2, **kw)]


def refusal_pill(cx, cy, gate, words, h=PILL_H, cap=PILL_CAP):
    """dev.46 P01: the one refusal component: the do-prompt pill centred on (cx, cy) while the module's
    `gate` (sshd.<what>.refused, the union of the words' binds) holds, each word ('Not now' /
    'Put away', its bind) cap-centred on it. dev.48: PILL_H x font PILL_CAP (the shield pill passes its
    own h / cap)."""
    out = [img(f'{K}dopill/{PILL_W}/{h}', round(cx - PILL_W / 2), round(cy - h / 2), PILL_W, h,
               need_bind=gate)]
    out += [cap_label('doinfo', cap, word, cx, cy, need_bind=b) for word, b in words]
    return out


# The native area-name plate's region colour (sshd_art kRegionCols: Map2D map_00_type / MapField
# mapPlace_00_color keys): 0 brown, 1 forest green, 2 volcano red, 3 desert yellow, 4 light green,
# 5 teal, 6 purple. Faron green is the live Sealed Grounds plate (capture 05, 2026-11-11); Eldin red
# and Lanayru yellow follow the key colours; the sky / Skyloft / Sky Keep picks are unverified.
REGION_COLOR = {**{st: 1 for st in ('F401', 'F100', 'D100', 'D101')},
                **{st: 2 for st in ('F200', 'D200', 'D201')},
                **{st: 3 for st in ('F300', 'F301', 'F302', 'F303', 'F301_4', 'F301_1', 'D300', 'D301')},
                **{st: 5 for st in ('F000', 'F020', 'F010r')},
                **{st: 6 for st in ('S000', 'S100', 'S200', 'S300')},  # dev.34: the native trial plate is purple
                'D003': 6}


def area_plate(title, x, y, h=120, **kw):
    """The native area-name plate (art areaplate/<w>/<h>/<region>) with the title in the plate's own
    text style, sized to the title (native board = title + about 200 px at this height). Returns the
    widgets; the plate's board box is (x, y) .. (x + w - 16 * h / 112, y + 102.4 * h / 112)."""
    region = kw.pop('region')
    tw = TITLE_W[title]
    w = max(480, tw + 200)
    k = h / 112
    bw = w - int(16 * k)
    label = kw.pop('label', None)
    text = (img(K + f'ctitle/plate/34/{tw}/67/{label}', x + bw // 2 - tw // 2, int(y + 51.2 * k - 67 / 2), tw, 67, **kw)
            if label else text_img('plate', 34, title, x + bw // 2, int(y + 51.2 * k - 67 / 2), tw, 67, **kw))
    return [img(K + f'areaplate/{w}/{h}/{region}', x, y, w, h, **kw), text]


def dpad_cluster(at=None):
    """The native top-screen D-pad cluster, display only (no taps). Each layer is one native pane
    the module mirrors (sshd_dpad.cpp): need_bind = its level (0 hidden), tint = its effective
    alpha. Positions are the layouts' own (DoButton remoConBtn_03 / nunBtn_03 / remoConBtn_05):
    eye 32 units above the cross, Fi / Loftwing 40 below, the selected target's socket 82 above
    with the Look / Dowse / Back caption where the eye sits, Help / Close right of the cross."""
    cx, cy = at or DP
    s = DP_S
    uc = 112 * s / 51.3  # canvas px per Cross layout unit (the 112 px base is a 51.3-unit pane)
    ud = 112 * s / 52.0  # Dowsing layout units (its base pane is 52 units)

    def layer(key, w, h, ccx, ccy, bind):
        return img(K + 'dpad/' + key, int(round(ccx - w / 2)), int(round(ccy - h / 2)), w, h,
                   need_bind=bind, tint_bind=bind, tint_colors=ALPHA_LEVELS)

    def caption(text, x, ccy, bind, center=True):
        w, h = TW[('hud', DP_CAP, text)], CAP_H[DP_CAP]
        x0 = x - w // 2 if center else x
        return img(f'{K}txt/hud/{DP_CAP}/{text}', x0, int(round(ccy - h / 2)), w, h,
                   need_bind=bind, tint_bind=bind, tint_colors=ALPHA_LEVELS)

    cross = int(round(112 * s))
    tsock = int(round(72 * ud))
    out = [
        # Up: the selected dowsing target on its native socket (spoiler-gated by the module)
        layer('tsocket', tsock, tsock, cx + 2, cy - 82 * ud, 'sshd.dp.target'),
        dict(layer('target/0', *(2 * [int(round(52 * ud * 172 / 160))]), cx + 2, cy - 82 * ud, 'sshd.dp.target'),
             src_bind='sshd.dp.tsrc'),
        layer('eye', int(round(22 * ud)), int(round(22 * ud)), cx, cy - 32 * ud, 'sshd.dp.eye'),
    ]
    for text, bind in (('Look', 'sshd.dp.look'), ('Dowse', 'sshd.dp.dowse'), ('Back', 'sshd.dp.back')):
        out.append(caption(text, int(round(cx + 3 * ud)), cy - 35 * ud, bind))
    out += [layer('base', cross, cross, cx, cy, b) for b in ('sshd.dp.base_c', 'sshd.dp.base_d', 'sshd.dp.base_h')]
    # lit arms over the dim base (a lit arm is display only, never an offered action)
    lw, lh = int(round(64 * s)), int(round(48 * s))
    out.append(img(K + 'dpad/left', int(round(cx - (31 + 25.8) * s)), int(round(cy - 22 * s)), lw, lh,
                   need_bind='sshd.dp.left', tint_bind='sshd.dp.left', tint_colors=ALPHA_LEVELS))
    out.append(layer('down', int(round(176 * s)), int(round(176 * s)), cx, cy, 'sshd.dp.down'))
    out.append(layer('up', cross, cross, cx, cy, 'sshd.dp.up'))
    out.append(layer('right', cross, cross, cx, cy, 'sshd.dp.right'))
    for text, bind in (('Help', 'sshd.dp.help'), ('Close', 'sshd.dp.close')):
        out.append(caption(text, cx + int(round(58 * s)), cy, bind, center=False))
    # Down: Fi (shadow, coloured outline, Fi, attention halos in the native order) or the Loftwing
    fy = cy + 40.5 * uc
    fi = 32.4 * uc
    for key, k in (('fis0', 1.22), ('fis1', 1.05), ('fi', 1.0), ('fih0', 1.0), ('fih1', 1.0)):
        out.append(layer(key, int(round(fi * k)), int(round(fi * k)), cx, fy, 'sshd.dp.' + key))
    bw = 49.2 * uc * 140 / 128  # the picture pads the 128 x 80 bird by 6 px for its shadow
    out.append(layer('bird', int(round(bw)), int(round(bw * 92 / 140)), cx, cy + 38.7 * uc, 'sshd.dp.bird'))
    return out


def stamina_ring():
    """Stamina (main+0x6244B58, 0..1,000,000) as a ring just outside the minimap rim: the native
    gauge's backing track and 100 rotated copies of one segment, emptying clockwise from 12 o'clock
    like the native wheel (segment i, clockwise from the top, shows while the remaining fraction
    covers it). Hidden when full, as the native gauge is."""
    rin, rout = STAM_R
    h = math.radians(STAM_HALF_MDEG / 1000)
    sw = 2 * math.ceil(rout * math.sin(h)) + 4
    sh = math.ceil(rout - rin * math.cos(h)) + 4
    # dev.34 (item 5, live hero F100 B dash): at 0 stamina the native wheel is red while the ring has
    # no segment left, so the backing track follows the same red selector (variant 1)
    out = [img(K + f'stamtrack/{rin}/{rout}/0', MX - rout - 4, MY - rout - 4, 2 * rout + 8, 2 * rout + 8,
               need_bind='stam.ring', bind='sshd.stam.red', src_format=K + f'stamtrack/{rin}/{rout}/%d')]
    step = 360 / STAM_SEGMENTS
    for i in range(STAM_SEGMENTS):
        # dev.33: the segment's colour follows the native gauge palette (module sshd.stam.red: the
        # land wheel's red selector frame 2 while recovering from exhaustion, the
        # reader analysis); variant 0 = green, 1 = the exhausted red
        out.append(img(K + f'stamseg/{rin}/{rout}/{STAM_HALF_MDEG}/0', MX - sw // 2, MY - rout - 2, sw, sh,
                       rotate=round((i + 0.5) * step, 3), pivot=[sw // 2, rout + 2], need_bind=f'stam.s{i}',
                       bind='sshd.stam.red', src_format=K + f'stamseg/{rin}/{rout}/{STAM_HALF_MDEG}/%d'))
    return out


def stamina_derived():
    out = [{'name': 'stam.ring', 'expr': 'stamina < 1000000'}]
    for i in range(STAM_SEGMENTS):
        need = int(round((STAM_SEGMENTS - 0.5 - i) * 1000000 / STAM_SEGMENTS))
        out.append({'name': f'stam.s{i}', 'expr': f'stam.ring && stamina >= {need}'})
    return out


def build():
    widgets = []
    widgets.append(img(K + 'bg/0/0/1240/1080', 0, 0, W, H))
    areas = {}
    for st, xmin, xmax, zmin, zmax, ppu, title in AREAS:
        mn, mx = (area_box_flip if st in FLIP else area_box)(xmin, xmax, zmin, zmax, ppu)
        if st in NATIVE_XZ:  # match native +x-right map orientation
            width = mx[0] - mn[0]
            mn[0] = round(xmin - PAD / ppu, 3)
            mx[0] = round(mn[0] + width, 3)
        areas[st] = {'min': mn, 'max': mx, 'image': K + ('blank' if st == REALM_NONE else 'map/' + st), 'no_pin': False}
        # the view cone first, so the chests, beacons and Link's head draw over it
        areas[st]['dynamic_markers'] = [cone_marker(ubind(st), vbind(st)), CONE_PREFETCH]
        if st in OPEN_CHEST_STAGES:
            areas[st]['dynamic_markers'].append(OPEN_CHESTS)
        if st in ('F401', 'F000', 'F100', 'F200', 'F300', 'F301', 'F302', 'F303'):  # player beacons (module PublishBeacons)
            areas[st]['dynamic_markers'].append({'group': 'beacons', 'count': 5, 'x': 'sshd.bc{i}.u', 'y': 'sshd.bc{i}.v',
                                                 'kind': 'sshd.bc{i}.on', 'hide_when_kind': 0,
                                                 'icon_src_bind': 'sshd.bc.src', 'size': 64})
        if st in REALMS:
            pass  # dev.34: the native trial map draws Link's head and cone only
        elif st in DUNGEONS:
            # dungeons (dev.30) in the native mapIcon_00 pane order: chests, red X marks, door bars,
            # statues, the boss door's objective, then the small-key locks over the bars
            areas[st]['dynamic_markers'] += [DUNGEON_CHESTS, MARKS, DOORS, STATUES, OBJECTIVE, KEYS] + \
                ([TRIFORCE] if st == 'D003' else [])
        else:
            # door bars, then the bird statues (over the bars, under Link's head); field and Skyloft
            # maps (dev.31): landmark glyphs, Beedle's airshop (Skyloft) and NPC balloons on top
            areas[st]['dynamic_markers'] += ([BUGS] if st.startswith('F') else []) + [DOORS, STATUES]
            if st.startswith('F'):
                areas[st]['dynamic_markers'] += ([TARGETS] if st == 'F000' else []) + [CAVES, KIKWI] + \
                    ([BEEDLE] if st == 'F000' else []) + [BALLOONS]
        if st in PLACE_STAGES:  # the Map page's own copy with the neighbouring-area plates
            areas[st + '_m'] = {**areas[st], 'dynamic_markers': areas[st]['dynamic_markers'] + [PLACES]}
        if st in DUNGEONS:  # the Map page's own copy: its view tells the module the page is open
            areas[st + '_m'] = dict(areas[st])
        widgets.append({
            'type': 'map', 'id': 'mini_' + st, 'area': st, 'rect': [MX - MR, MY - MR, 2 * MR, 2 * MR],
            'bg': '#FFD9C48E', 'area_label': False,
            'follow_window': TRIAL_UPX * 2 * MR if st in REALM_BLOB + (REALM_NONE,) else
            2 * S000_SPAN if st == REALM_WHOLE else READY_WINDOW,
            'marker_x_bind': ubind(st), 'marker_y_bind': vbind(st),
            'marker_src': K + 'lyt/Map2D/tr_linkPIcon_00', 'marker_size': [84, 84],
            'need_bind': 'stage.show_' + st,
            **({'image_bind': 'sshd.dmap.src'} if st in DUNGEONS else {'image_bind': 'sshd.fmap.src'} if st in FIELD_STATE else {}),
        })
    # dev.34: every other stage (module PublishGenMap, sshd_stages.inc): its own map faces in the
    # square GEN frame, Link in that frame, the view cone, door bars and bird statues
    areas['GEN'] = {'min': [-GEN_EXT, -GEN_EXT], 'max': [GEN_EXT, GEN_EXT], 'image': K + 'blank', 'no_pin': False,
                    'dynamic_markers': [cone_marker('sshd.gen.lu', 'sshd.gen.lv'), CONE_PREFETCH, DOORS, STATUES]}
    # dev.35: the Map page's own copy: its view tells the module the page is open (floor tab picks)
    areas['GEN_m'] = dict(areas['GEN'])
    widgets.append({
        'type': 'map', 'id': 'mini_GEN', 'area': 'GEN', 'rect': [MX - MR, MY - MR, 2 * MR, 2 * MR],
        'bg': '#FFD9C48E', 'area_label': False, 'follow_window': GEN_READY_WINDOW,
        'marker_x_bind': 'sshd.gen.lu', 'marker_y_bind': 'sshd.gen.lv',
        'marker_src': K + 'lyt/Map2D/tr_linkPIcon_00', 'marker_size': [84, 84],
        'need_bind': 'stage.show_GEN', 'image_bind': 'sshd.gen.src'})
    # dev.35: Inside the Thunderhead: the Sky picture around Link (area F023 = the Sky's frame)
    widgets.append({
        'type': 'map', 'id': 'mini_F023', 'area': THUNDERHEAD_STAGE, 'rect': [MX - MR, MY - MR, 2 * MR, 2 * MR],
        'bg': '#FFD9C48E', 'area_label': False, 'follow_window': THUNDERHEAD_WINDOW,
        'marker_x_bind': 'sky.u', 'marker_y_bind': 'sky.v',
        'marker_src': K + 'lyt/Map2D/tr_linkPIcon_00', 'marker_size': [84, 84], 'need_bind': 'stage.is_F023'})
    # a stage the module cannot place (unreadable state): blank parchment disc behind the frame
    widgets.append({'type': 'rect', 'rect': [MX - MR, MY - MR, 2 * MR, 2 * MR], 'bg': '#FFD9C48E',
                    'color': '#00000000', 'frame': 0, 'need_bind': 'stage.blank'})
    # dev.48: the rim's lit look while the module's sshd.stab.lit holds (a stab it staged), never on a
    # refused tap (dev.47 lit it from the tap's own selection)
    widgets.append(img(K + 'frame', MX - 400, MY - 400, 800, 800,
                       bind='sshd.stab.lit', src_names=[K + 'frame', K + 'frame/pressed']))
    # stamina: a ring just outside the map rim, under the compass and the rim counts (stamina_ring)
    widgets.extend(stamina_ring())
    # the native map compass on the rim at -55 degrees, centred where the mockup's cap was
    widgets.append(img(K + 'compass', 821 - COMPASS // 2, 143 - COMPASS // 2, COMPASS, COMPASS))
    # Gratitude Crystals on the upper-left minimap rim; unknown data hides the whole counter.
    # Moved up-left of the stamina ring (owner 2026-11-11: the ring must not cover the rim counts).
    cxx, cxy = CRYSTALS
    widgets.append(img(K + 'lyt/MenuPause/uk_genki_00', cxx, cxy, 64, 64,
                       need_bind='sshd.crystals.valid'))
    for i in range(2):
        widgets.append(digit_widget(cxx + 66 + i * 27, cxy + 18, 33, 44, bind=f'crystals.d{i}',
                                   x_bind='crystals.shift', need_bind=f'crystals.show{i}'))
    # Goddess Cubes struck in the current region (Faron / Eldin / Lanayru, 9 each) on the upper-right
    # rim; hidden outside those regions and when the flags cannot be read (module PublishCubes).
    widgets.append(img(K + 'sq/DoButton/ts_GdsCube_00', 852, 26, 64, 64, need_bind='sshd.cubes.valid'))
    widgets.append(digit_widget(918, 44, 33, 44, bind='sshd.cubes', need_bind='sshd.cubes.valid'))
    for st, *_r, title in AREAS:  # centred in the gap below the circle (TITLE_Y)
        widgets.append(text_img('title', 34, title, MX, TITLE_Y, TITLE_W[title], 67,
                                need_bind='stage.is_' + st))
    # dev.34: the generic stage's native name (MAP_<id>_S), centred in a fixed box
    widgets.append(img(K + 'blank', MX - GEN_TITLE_W // 2, TITLE_Y, GEN_TITLE_W, 67, src_bind='sshd.gen.title',
                       need_bind='gen.titled'))
    widgets.append(img(K + f'ctitle/title/34/{GEN_TITLE_W}/67/MAP_{THUNDERHEAD_NAME_ID:02d}_S', MX - GEN_TITLE_W // 2,
                       TITLE_Y, GEN_TITLE_W, 67, need_bind='stage.is_F023'))
    # top-left: the shield (the pouch entry with a shield id, 116..125), centred and enlarged like
    # the ZR / R corners, no text (owner 2026-11-11). A tap is a shield bash: an owner-approved
    # virtual L3 press through the helper (module ShieldBash; "Not now" without a usable shield,
    # in menus / events / flight / Silent Realms; dev.40: "Put away" with an item in Link's hands).
    # dev.37 (owner 2026-11-12): no long press; the sword / shield Equipment inspector is gone.
    tcx, tcy = 16 + CB // 2, 16 + CB // 2
    widgets.append(img(K + f'plank/corner/{CB}/{CB}', 16, 16, CB + 16, CB + 16))
    widgets.append(img(K + f'socket/{CORNER_SOCKET}', tcx - CORNER_SOCKET // 2, tcy - CORNER_SOCKET // 2,
                       CORNER_SOCKET, CORNER_SOCKET, id='tl_socket'))
    widgets.append(shield_icon(tcx - CORNER_ICON // 2, tcy - CORNER_ICON // 2, CORNER_ICON, CORNER_ICON))
    widgets.append({'type': 'rect', 'rect': [16, 16, CB, CB], 'color': '#00000000', 'frame': 0,
                    'on_tap': 'shield_bash', 'haptic': 'light'})
    # dev.46 P01: the refusal on the pill under the plank, 12 px in from the screen edge, no longer
    # across the shield socket. dev.48: 52 px tall (y 213..265), font 22: the whole gap between the
    # plank's body (to y 212) and the rupee icon (from y 265); the 64 px pill does not fit here
    widgets += refusal_pill(12 + PILL_W / 2, 213 + PILL_SHIELD_H / 2, 'sshd.bash.refused',
                            [('Not now', 'sshd.bash.notnow'), ('Put away', 'sshd.bash.putaway')],
                            h=PILL_SHIELD_H, cap=PILL_SHIELD_CAP)
    by = H - 16 - CB
    # bottom-right: the L corner (owner 2026-11-11; a virtual L hold, owner-approved for this feature
    # only). Button-only SSHD gives the right stick to the free camera while L is held. dev.37 (owner
    # Thor feedback 2026-11-12): the icon FOLLOWS L: eye (the D-pad's own look icon, DoButton
    # tr_cameraMark_00) while the game sees L held, the current sword stage (as on Gear; empty
    # without a sword) while not. A tap LOCKS L (module LToggle): eye + the native lock glyph (the
    # maps' locked-door keyhole, MapField nh_mapDoorKey_00, art doorkey) on the frame's top edge at
    # its right corner, and the module has the guest helper hold L on the game thread (dev.45; dev.37..44 used
    # a runtime enforce rule, one host log line per 50 ms press); the lock survives menus, events,
    # doors and loads (paused, then held again) until tapped again.
    # The L glyph badge stays on the inner, screen-facing corner (top-left), like ZR / R.
    lx = W - 16 - CB
    lcx, lcy = lx + CB // 2, by + CB // 2
    widgets.append(img(K + f'plank/corner/{CB}/{CB}', lx, by, CB + 16, CB + 16))
    widgets.append(img(K + f'socket/{CORNER_SOCKET}', lcx - CORNER_SOCKET // 2, lcy - CORNER_SOCKET // 2,
                       CORNER_SOCKET, CORNER_SOCKET, id='l_socket'))
    widgets.append(img(K + 'sq/DoButton/tr_cameraMark_00', lcx - CORNER_ICON // 2, lcy - CORNER_ICON // 2,
                       CORNER_ICON, CORNER_ICON, need_bind='sshd.l.eye'))
    # the sword art is tall (1 : 3); drawn diagonally so it fills the round socket
    widgets.append(img(K + 'lyt/MenuPause/hm_swordA_00', lcx - L_SWORD[0] // 2, lcy - L_SWORD[1] // 2, *L_SWORD,
                       src_bind='sshd.sword.src', need_bind='l.sword', rotate=45))
    # the L badge straddles the frame's top-left corner (owner 2026-11-11: half outside, half inside)
    widgets.append(img(K + 'sq/DoButton/tr_lBtn_01', lx - CORNER_GLYPH // 2, by - CORNER_GLYPH // 2,
                       CORNER_GLYPH, CORNER_GLYPH))
    # the lock badge straddles the frame's top edge at its right corner (shown while locked), 12 px
    # from the screen edge (dev.48: the canvas margin; dev.47 sat 4 px from it)
    widgets.append(img(K + 'doorkey', W - L_LOCK_X - L_LOCK, by - L_LOCK // 2, L_LOCK, L_LOCK, need_bind='sshd.l.on'))
    widgets.append({'type': 'rect', 'rect': [lx, by, CB, CB], 'color': '#00000000', 'frame': 0,
                    'on_tap': 'l_toggle', 'haptic': 'light'})
    # ZR top-right / R bottom-left corners (dev.47 swap; owner 2026-11-11), each the native HUD's
    # item: ZR = the equipped B-item (slot+0x53AE, tier icon + ammo; module PublishShortcuts sc0),
    # R = the R item (lastUsedPouchItemSlot slot+0x53B0 -> pouch entry, icon + count; empty /
    # invalid slot = empty socket). The button glyph is a badge on the inner, screen-facing corner
    # (ZR bottom-left, R top-right), so the socket and item sit centred and larger; the count stays
    # bottom-right. A tap opens where each is changed: ZR -> Gear (ZR hub), R -> Bag on its Pouch tab
    # (R hub).
    colx = W - 16 - CB
    # glyph badges straddle the frame corner facing the screen centre (owner 2026-11-11: centred on
    # the corner, half outside, half inside): ZR bottom-left, R top-right after the swap
    for y, gy, glyph, act in [(16, 16 + CB - CORNER_GLYPH // 2, 'tr_zrBtn_00', 'go_gear'),
                              (by, by - CORNER_GLYPH // 2, 'tr_rBtn_00', 'go_gpouch')]:
        x = colx if act == 'go_gear' else 16
        cx, cy = x + CB // 2, y + CB // 2
        widgets.append(img(K + f'plank/corner/{CB}/{CB}', x, y, CB + 16, CB + 16))
        widgets.append(img(K + f'socket/{CORNER_SOCKET}', cx - CORNER_SOCKET // 2, cy - CORNER_SOCKET // 2,
                           CORNER_SOCKET, CORNER_SOCKET, id='zr_socket' if act == 'go_gear' else 'r_socket'))
        ix, iy = cx - CORNER_ICON // 2, cy - CORNER_ICON // 2
        if act == 'go_gear':
            widgets.append(img(K + 'sq/DoButton/uc_bomb_00', ix, iy, CORNER_ICON, CORNER_ICON,
                               src_bind='sshd.sc0.src', need_bind='sshd.sc0.on'))
            nd, need = 'sshd.sc0.nd', 'sc0.count'
        else:
            widgets.append(img(POUCH_SOURCES[0], ix, iy, CORNER_ICON, CORNER_ICON, bind='pouch.rid',
                               src_names=POUCH_SOURCES, need_bind='pouch.ron'))
            nd, need = 'pouch.rnd', 'pouch.rcnt'
        for k in range(2):
            widgets.append(digit_widget(x + CB - 70 + k * 27, y + CB - 44 - 10, 33, 44, bind=f'{nd}{k}',
                                        need_bind=need))
        widgets.append({'type': 'rect', 'rect': [x, y, CB, CB], 'color': '#00000000', 'frame': 0,
                        'on_tap': act, 'haptic': 'light'})
        widgets.append(img(K + 'sq/DoButton/' + glyph, (x if act == 'go_gear' else x + CB) - CORNER_GLYPH // 2, gy, CORNER_GLYPH, CORNER_GLYPH))
    # spin attacks between the ZR and L corners (owner 2026-11-11; the right column since the dev.47
    # swap): two round buttons, the native
    # socket with the controller tutorial's own right-stick art (DoButton controllerTutorial_00:
    # tr_rStick_04 tilting left-right, tr_rStick_02 up-down) and its swing arrows; the physical input
    # for a spin is exactly those right-stick flicks. A tap stages the flick sequence for the helper
    # (module SpinAction; refused with "Not now" outside free play on foot with a sword, dev.40 "Put
    # away" with an item in Link's hands; a tap while a spin plays queues one more); a soft glow while
    # it plays.
    for (top, act, stick, sdx, arrows) in [(SPIN_TOP[0], 'spin_h', 'tr_rStick_04', 0, [('th_arrow_04', 0, -62, 92)]),
                                           (SPIN_TOP[1], 'spin_v', 'tr_rStick_02', 8,
                                            [('th_arrow_11', -58, -24, 46), ('th_arrow_10', -58, 24, 46)])]:
        cx, cy = colx + CB // 2, top + SPIN_D // 2
        widgets.append(img(K + 'softglow', cx - 110, cy - 110, 220, 220, need_bind='sshd.spin.busy'))
        widgets.append(img(K + f'socket/{SPIN_D}', cx - SPIN_D // 2, top, SPIN_D, SPIN_D,
                           id=act + '_socket', need_bind='sshd.sword.has'))
        widgets.append(img(K + 'sq/DoButton/' + stick, cx + sdx - 50, cy - 50 + 10, 100, 100, need_bind='sshd.sword.has'))
        for name, dx, dy, s in arrows:
            widgets.append(img(K + 'sq/DoButton/' + name, cx + dx - s // 2, cy + dy - s // 2, s, s,
                               need_bind='sshd.sword.has'))
        # no caption (owner 2026-11-11: native icons only); "Not now" (dev.40: "Put away" when an
        # item in Link's hands is the reason) shows below for a moment after a refused tap, in the
        # gap the caption used to take
        # dev.46 P01: on the refusal pill under the socket, its right end 12 px in from the screen
        # edge (the stamina ring ends 12 px left of it). dev.48: the 64 px pill starts 8 px under the
        # socket (y 495 / 771): the lower one ends 5 px above the L corner's lock badge (y 836)
        widgets += refusal_pill(W - 12 - PILL_W / 2, top + SPIN_D + PILL_GAP + PILL_H / 2, 'sshd.spin.refused',
                                [('Not now', 'sshd.spin.notnow'), ('Put away', 'sshd.spin.putaway')])
        widgets.append({'type': 'rect', 'id': act, 'rect': [colx, top, CB, SPIN_D], 'color': '#00000000',
                        'frame': 0, 'on_tap': act, 'haptic': 'light', 'need_bind': 'sshd.sword.has'})
    # left pane: rupees (native HUD digits th_Number_00..09), centred at x 120
    widgets.append(img(K + 'sq/DoButton/tr_rupy_00', 120 - 30, RUPEE_Y - 30, 60, 60))
    for i in range(4):
        widgets.append(digit_widget(120 + i * 27, RUPEE_Y + 44, 33, 44, bind=f'ru.d{i}', x_bind='ru.shift', need_bind=f'ru.show{i}'))
    # the native D-pad cluster where the stamina wheel was (owner 2026-11-11; stamina is the ring)
    widgets.extend(dpad_cluster())
    # footer destinations: Gear (equipment), Map, Bag (opens the Bag section on its Pouch tab)
    # dev.46 (alt A): choice planks ending on the corner planks' bottom line (y 1064), tap rects
    # 250 x 134 with 17 px gaps (dev.45: 252 x 130, 14 px)
    fw = READY_FOOT_W
    for x, name in zip(READY_FOOT_X, ('Gear', 'Map', 'Bag')):
        widgets += choice_button(x, FOOT_BOTTOM - CHOICE_VIS[FOOT_H][1], fw, FOOT_H, name, 30, 'ready_' + name.lower())
        widgets.append({'type': 'rect', 'rect': [x, FOOT_TAP_Y, fw, FOOT_TAP_H], 'color': '#00000000', 'frame': 0,
                        'on_tap': {'Gear': 'go_gear', 'Map': 'go_map', 'Bag': 'go_gpouch'}[name], 'haptic': 'light'})
    # swipe surface: the minimap/title column and the left pane, not the corners/column/footer
    for i, r in enumerate([[16 + CB + 8, 0, W - 2 * (16 + CB + 8), 916], [0, 16 + CB + 8, 16 + CB + 8, by - 16 - CB - 16]]):
        # fallback without the stick helper: 4-way swipe = fixed right-stick flick
        widgets.append({'type': 'rect', 'id': f'swipe_{i}', 'rect': r, 'color': '#00000000', 'frame': 0,
                        'swipe_px': 60, 'on_swipe_up': 'slash_up', 'on_swipe_down': 'slash_down',
                        'on_swipe_left': 'slash_left', 'on_swipe_right': 'slash_right',
                        'need_bind': 'stick.fallback'})
        # with the helper: drag = continuous right stick (hold+drag guides, fast stroke slashes).
        # The module reads the pad's pan (@view_pan_x/y:stickpad_<i>, runtime 22); a held finger
        # keeps it, a lift glides it home. dev.38 (owner Thor feedback 2026-11-12, "sometime cannot
        # swipe to slash"): the module tells the finger from the glide by the glide's own step
        # (sshd_stroke.h PanStroke), so a stroke that lands while the last one still glides home is
        # fed from its first pixel (dev.17..37 dropped it).
        widgets.append({'type': 'rect', 'id': f'stickpad_{i}', 'rect': r, 'color': '#00000000', 'frame': 0,
                        'pan_zoom': True, 'min_zoom': 1, 'max_zoom': 1, 'view_idle_ms': 1,
                        'need_bind': 'sshd.stick.ready'})
    # dev.47: a still tap on the round map requests R3. Tap-only bands leave gesture
    # ownership with the existing stick pads, including their fallback swipes. No old tap action.
    # Inscribed 10 px bands keep the title, rim and corners outside the stab hit area.
    dp = dpad_cluster()
    dp_right = max(w['rect'][0] + w['rect'][2] for w in dp)
    dp_top = min(w['rect'][1] for w in dp)
    dp_bottom = max(w['rect'][1] + w['rect'][3] for w in dp)
    for y in range(MY - MR, MY + MR, 10):
        dy = max(abs(y - MY), abs(y + 10 - MY))
        half = int(math.sqrt(max(0, MR * MR - dy * dy)))
        left = max(MX - half, dp_right) if y < dp_bottom and y + 10 > dp_top else MX - half
        if half and left < MX + half:
            widgets.append({'type': 'rect', 'rect': [left, y, MX + half - left, 10],
                            'color': '#00000000', 'frame': 0, 'on_tap': 'sword_stab', 'haptic': 'light'})
    # dev.38 gold swipe trail (module PublishTrail, sshd_stroke.h): TRAIL_DOTS soft glows along the
    # stroke's path, the native sparkle (DoButton tr_kirakira_00, the glint the HUD uses on keys and
    # rupees) tinted the game's gold, largest and brightest at the head, fading out after the lift.
    # dev.39: on runtime 25 the module reads where the finger is (@view_touch_x/y:stickpad_<i>) and
    # draws the trail under it, kept inside the pads (sshd_stroke.h FingerTrail); runtime 22..24 tell a
    # module the pad's pan only, so there the trail is the stroke's shape drawn out of the minimap
    # centre (Link's head), like the sword's swing (dev.38). Same widgets either way.
    # Images never take input: taps and the pad underneath work as before.
    for k in range(TRAIL_DOTS):
        widgets.append(img(K + 'sq/DoButton/tr_kirakira_00', -TRAIL_PX // 2, -TRAIL_PX // 2, TRAIL_PX, TRAIL_PX,
                           x_bind=f'sshd.tr{k}.x', y_bind=f'sshd.tr{k}.y', scale_bind=f'sshd.tr{k}.s',
                           tint_bind=f'sshd.tr{k}.a', tint_colors=TRAIL_TINTS, need_bind=f'tr{k}.on'))
    # the swipe refused (a menu, an event, no sword ...): "Not now" over the pad, dev.46 on the refusal
    # pill (alt A's one refusal component; dev.45: bare text, 1.37:1 on the minimap paper)
    widgets += refusal_pill(MX, MY + MR - 120 + 27, 'sshd.swipe.refused', [('Not now', 'sshd.swipe.refused')])

    digits = []
    digits += [
        {'name': 'crystals.d0', 'expr': 'floor(sshd.crystals / 10)'},
        {'name': 'crystals.d1', 'expr': 'sshd.crystals % 10'},
        {'name': 'crystals.shift', 'expr': 'sshd.crystals < 10 ? -27 : 0'},
        {'name': 'crystals.show0', 'expr': 'sshd.crystals.valid && sshd.crystals >= 10'},
        {'name': 'crystals.show1', 'expr': 'sshd.crystals.valid'},
    ]
    n = '(rupees >= 1000 ? 4 : (rupees >= 100 ? 3 : (rupees >= 10 ? 2 : 1)))'
    digits.append({'name': 'ru.n', 'expr': n})
    digits.append({'name': 'ru.shift', 'expr': '-(ru.n * 27) / 2', 'floor': True})
    for i in range(4):
        cases = []
        for k in range(4, i, -1):
            cases.append(f'ru.n == {k} ? floor(rupees / {10 ** (k - 1 - i)}) % 10')
        digits.append({'name': f'ru.d{i}', 'expr': ' : '.join(cases) + ' : 0'})
        digits.append({'name': f'ru.show{i}', 'cmp': 'gt', 'a': 'ru.n', 'b': i})
    stage_derived = [{'name': 'stage.is_' + st, 'expr': ' || '.join(
        f'stage.id == {stage_u64(s)}' for s in [st] + ALIASES.get(st, []))} for st, *_ in AREAS]
    stage_derived.append({'name': 'stage.unknown', 'expr': ' && '.join(
        f'stage.id != {stage_u64(st)}' for st in [a[0] for a in AREAS] + ['F020', THUNDERHEAD_STAGE] +
        sum(ALIASES.values(), []))})
    stage_derived.append({'name': 'stage.is_F023', 'cmp': 'eq', 'a': 'stage.id', 'b': stage_u64(THUNDERHEAD_STAGE)})
    # dungeon layout spoiler gate: the Dungeon Map flag is not found; the layout is shown only when
    # the player turns "Dungeon layout before the Map" on in Settings (default off: blank paper)
    stage_derived.append({'name': 'dmap.on', 'cmp': 'ne', 'a': '@flag:dmap.layout', 'b': 0})
    for st, *_ in AREAS:
        # dungeons: always drawn; the module picks the picture (visited rooms / full layout)
        stage_derived.append({'name': 'stage.show_' + st, 'expr': 'stage.is_' + st})
    stage_derived.append({'name': 'stage.show_GEN', 'expr': 'stage.unknown && sshd.gen.on'})
    stage_derived.append({'name': 'gen.titled', 'expr': 'stage.show_GEN && sshd.gen.titled'})
    stage_derived.append({'name': 'stage.blank', 'expr': 'stage.unknown && !sshd.gen.on'})
    derived = [
        {'name': 'link.u', 'expr': '-link.x'},
        {'name': 'link.v', 'expr': 'link.z'},
        {'name': 'link.vf', 'expr': '-link.z'},
        # Base capacity excludes carried Life Medals (item 114), each worth four quarters.
        # Native HUD caps the effective capacity at 20 hearts; hearts remain top-screen only.
        {'name': 'hearts.max_q', 'expr': 'clamp(hearts.base_q + 4 * (' +
         ' + '.join(f'(pouch.id{i} == 114)' for i in range(8)) + '), 0, 80)'},
        *stamina_derived(),
        {'name': 'sc0.count', 'all_nonzero': ['sshd.sc0.on', 'sshd.sc0.hasn']},
        {'name': 'arrows.d0', 'expr': 'floor(arrows / 10) % 10'},
        {'name': 'arrows.d1', 'expr': 'arrows % 10'},
        # shield = the pouch entry holding a shield id (pouch ids share the item enum; batch5)
        {'name': 'shield.id', 'expr': '(pouch.id0 >= 116 && pouch.id0 <= 125) ? pouch.id0 : (pouch.id1 >= 116 && pouch.id1 <= 125) ? pouch.id1 : (pouch.id2 >= 116 && pouch.id2 <= 125) ? pouch.id2 : (pouch.id3 >= 116 && pouch.id3 <= 125) ? pouch.id3 : (pouch.id4 >= 116 && pouch.id4 <= 125) ? pouch.id4 : (pouch.id5 >= 116 && pouch.id5 <= 125) ? pouch.id5 : (pouch.id6 >= 116 && pouch.id6 <= 125) ? pouch.id6 : (pouch.id7 >= 116 && pouch.id7 <= 125) ? pouch.id7 : 0'},
        {'name': 'shield.dur', 'expr': '(pouch.id0 >= 116 && pouch.id0 <= 125) ? pouch.n0 : (pouch.id1 >= 116 && pouch.id1 <= 125) ? pouch.n1 : (pouch.id2 >= 116 && pouch.id2 <= 125) ? pouch.n2 : (pouch.id3 >= 116 && pouch.id3 <= 125) ? pouch.n3 : (pouch.id4 >= 116 && pouch.id4 <= 125) ? pouch.n4 : (pouch.id5 >= 116 && pouch.id5 <= 125) ? pouch.n5 : (pouch.id6 >= 116 && pouch.id6 <= 125) ? pouch.n6 : (pouch.id7 >= 116 && pouch.id7 <= 125) ? pouch.n7 : 0'},
        {'name': 'shield.valid', 'expr': 'shield.id >= 116 && shield.id <= 125'},
        {'name': 'shield.d0', 'expr': 'floor(shield.dur / 10) % 10'},
        {'name': 'shield.d1', 'expr': 'shield.dur % 10'},
        {'name': 'stick.fallback', 'cmp': 'eq', 'a': 'sshd.stick.ready', 'b': 0},
        *({'name': f'tr{k}.on', 'expr': f'sshd.tr.on && sshd.tr{k}.a > 0'} for k in range(TRAIL_DOTS)),
        {'name': 'l.sword', 'expr': 'sshd.sword.has && !sshd.l.eye'},
        {'name': 'ui.wrong_build', 'cmp': 'eq', 'a': 'build_match', 'b': 0},
    ] + stage_derived + digits

    def slash(x, y):
        # dev.42: the module's refusals (sshd.swipe.fallback, FallbackWhy) gate the
        # fallback flicks; unpublished = refused (the runtime fails an enabled_bind closed)
        return {'kind': 'button', 'button': f'RStick({x},{y})', 'frames': 6, 'haptic': 'light',
                'enabled_bind': 'sshd.swipe.fallback'}

    manifest = {
        'format': 1,
        'title_id': TITLE,
        'name': 'Skyward Sword HD companion',
        '_about': 'The Legend of Zelda: Skyward Sword HD (1.0.1) dual-screen companion: Ready, flight, '
                  'maps, gear, and a bag with pouch, collection and settings, with title standby and a '
                  'right-stick guide/slash helper. Every picture is decoded at runtime from the '
                  "player's own romfs; the package ships no game art.",
        'version': VERSION,
        'min_runtime': 23,
        'requires_module': True,
        'module': {'abi': 1, 'build_ids': [BUILD + '0' * 24], 'libraries': {}},
        'load_plan': 'load-plans/<build>.json',
        'load_plan_module': 'main',
        'canvas_w': W, 'canvas_h': H,
        'background': '#FF2F3225',
        'nav': False,
        'module_tick_hidden': False,
        'map': {'areas': areas},
        'derived': derived,
        'actions': {
            # 4-way flick = sword slash: a short right-stick pulse in that direction (a held
            # stick only poses the sword). Diagonals and guided strokes need the later helper.
            'slash_up': slash(0, 1), 'slash_down': slash(0, -1),
            'slash_left': slash(-1, 0), 'slash_right': slash(1, 0),
            # L-hold toggle: the module's tap handler. The virtual L itself (owner-approved for the
            # L lock only) is held by the guest helper on the game thread (dev.45, mailbox +0x20), not
            # by a runtime enforce rule (dev.37..44: a press every 50 ms, logged by the host each time)
            'l_toggle': {'kind': 'module', 'action': 'l_toggle', 'argument': 0},
            'sword_stab': {'kind': 'module', 'action': 'sword_stab', 'argument': 0},
            'shield_bash': {'kind': 'module', 'action': 'shield_bash', 'argument': 0},
        },
        'haptics': True,
        'pages': [{'id': 'ready', 'title': 'Ready', 'widgets': widgets}],
    }
    data = {
        '_game': 'The Legend of Zelda: Skyward Sword HD (1.0.1, update v65536), 01002DA013484000',
        '_build_id': BUILD,
        '_source': 'Static main .bss addresses verified against the running game: Link position by moonjump/teleport writes, rupees '
                   'and hearts by writes the HUD reflected.',
        'points': {
            'link.x': {'type': 'f32', 'addr': 'main+0x623E7C4'},
            'link.y': {'type': 'f32', 'addr': 'main+0x623E7C8'},
            'link.z': {'type': 'f32', 'addr': 'main+0x623E7CC'},
            'rupees': {'type': 'u16', 'addr': f'main+{SLOT + 0xA5E:#x}'},
            'hearts.q': {'type': 'u16', 'addr': f'main+{SLOT + 0x5306:#x}'},
            'hearts.base_q': {'type': 'u16', 'addr': f'main+{SLOT + 0x5302:#x}'},
            # main+0x5AEB51C = slot+0x7C8 (a cheat list's prose says +0x6C8; its address is +0x7C8)
            'arrows': {'type': 'u8', 'addr': f'main+{SLOT + 0xA62:#x}'},
            'stage.id': {'type': 'u64', 'addr': f'main+{SLOT + 0x531C:#x}'},
            **{f'dfl.{i}': {'type': 'u16', 'addr': f'main+{SLOT + 0xA64 + 16 * i:#x}'} for i in sorted(set(DUNGEON_INDEX.values()))},
            'wings': {'type': 'u16', 'addr': 'main+0x6186B32'},
            'stamina': {'type': 'u32', 'addr': 'main+0x6244B58'},
            'stage.name': {'type': 'cstring', 'addr': f'main+{SLOT + 0x531C:#x}'},
        },
    }
    data_points = {}
    fly = []
    fly.append(img(K + 'bg/0/0/1240/1080', 0, 0, W, H))
    # sky picture: +x right, +z down (native sky map), so map units u = x, v = -z
    _, sxmin, sxmax, szmin, szmax, sppu = SKY
    sw = int(f32(f32(sxmax) - f32(sxmin)) * f32(sppu)) + 2 * PAD
    sh = int(f32(f32(szmax) - f32(szmin)) * f32(sppu)) + 2 * PAD
    u0, v1 = sxmin - PAD / sppu, -szmin + PAD / sppu
    areas[SKY[0]] = {'min': [round(u0, 3), round(v1 - sh / sppu, 3)], 'max': [round(u0 + sw / sppu, 3), round(v1, 3)],
                     'image': K + 'map/F020', 'no_pin': False,
                     'dynamic_markers': [THUNDERHEAD, COLUMNS, cone_marker('sky.u', 'sky.v'), CONE_PREFETCH, OPEN_CHESTS,
                                         GODDESS_CHESTS, SKY_STATUES,
                                         {'group': 'beacons', 'count': 5, 'x': 'sshd.bc{i}.u', 'y': 'sshd.bc{i}.v',
                                          'kind': 'sshd.bc{i}.on', 'hide_when_kind': 0,
                                          'icon_src_bind': 'sshd.bc.src', 'size': 64}, SKY_BEEDLE, SKY_BALLOONS,
                                         ISLAND_NAMES]}
    # dev.35: Inside the Thunderhead: the same picture and frame; the thunderhead ring, the light
    # columns, Link's cone, the Goddess tags (the Sky's scene, STIF flag index 21 in both stages),
    # the bird statues and the Sky-class NPC balloons of its live map marks (native F023: two
    # portrait balloons inside the ring)
    areas[THUNDERHEAD_STAGE] = {'min': areas[SKY[0]]['min'], 'max': areas[SKY[0]]['max'], 'image': K + 'map/F020',
                                'no_pin': False,
                                'dynamic_markers': [THUNDERHEAD, COLUMNS, cone_marker('sky.u', 'sky.v'), CONE_PREFETCH, GODDESS_CHESTS,
                                                    SKY_STATUES, SKY_BALLOONS]}
    fly.append({'type': 'rect', 'rect': [12, 12, 1216, 898], 'bg': '#FFCEBE8D', 'color': '#FF937544', 'frame': 4})
    fly.append({'type': 'map', 'id': 'sky_map', 'area': 'F020', 'rect': [20, 20, 1200, 882], 'bg': '#FFD9C48E',
                'area_label': False, 'follow_window': SKY_FOLLOW, 'marker_x_bind': 'sky.u',
                'marker_y_bind': 'sky.v', 'marker_src': K + 'loft', 'marker_size': [110, 110],
                # dev.38: drag / pinch like the Map page; home = the whole Sky with the bird on it
                **pan_view(areas[SKY[0]], sppu, SKY_FOLLOW, (20, 20, 1200, 882))})
    fly += area_plate('The Sky', 32, 26, region=REGION_COLOR['F020'])
    # dev.38: Reset under the compass (module kFlyAvoid): glides the Sky view home
    # dev.46 (alt A): the choice plank, opaque on the paper, centred in the unchanged tap rect; the tap
    # also selects its press id (select_group sshd.press) for the pressed look (derived press.*)
    fly += choice_button(FLY_RESET[0], FLY_RESET[1] + (RESET_H - RESET_ART_H) // 2, RESET_W, RESET_ART_H, 'Reset', 26,
                         'fly_reset', opaque=True)
    fly.append({'type': 'rect', 'rect': [FLY_RESET[0], FLY_RESET[1], RESET_W, RESET_H], 'color': '#00000000', 'frame': 0,
                'on_tap': 'sky_reset', 'haptic': 'light', 'select_group': 'sshd.press', 'payload': PRESS['fly_reset']})
    manifest['actions']['sky_reset'] = {'kind': 'view_reset', 'view': 'sky_map'}
    fly.append(img(K + 'compass', W - 100 - COMPASS // 2, 96 - COMPASS // 2, COMPASS, COMPASS))
    # altitude gauge (native sun / column / cloud) with the Loftwing riding on it
    GT, GH = 180, 440
    fly.append(img(K + f'altgauge/{GH}', 40, GT, 120, GH))
    fly.append(img(K + 'lyt/DoButton/th_skyBird_02', 60, GT + GH - 30 - 40, 80, 60, y_bind='alt.dy'))
    # Loftwing dash feathers (u16 main+0x6186B32: 3 -> 2 -> 1 per X dash, refills; live 2026-11-07)
    for i in range(3):
        fly.append(img(K + 'lyt/DoButton/th_skyWing_01', 300 + i * 96, 790, 84, 92, bind=f'wing.on{i}',
                       src_names=[K + 'wingoff', K + 'lyt/DoButton/th_skyWing_01'], need_bind='wing.valid'))
    # Accepted flying tray: native beacons remaining (5 minus placed), display-only.
    manifest['derived'].append({'name': 'sshd.bc.left', 'expr': '5 - sshd.bc.n'})
    # dev.46 (owner: "no beacon text with marker vertically center"): no "Beacons" word; the marker and
    # the count centred on the plate's body (744..894, centre 819)
    fly.append(img(K + 'plank/corner/230/150', 40, 744, 246, 166))
    fly.append(img(K + 'beacon/84', 70, 819 - 36, 72, 72))
    fly.append(digit_widget(200, 819 - 24, 36, 48, bind='sshd.bc.left'))
    # footer: Gear / Bag (Bag opens on its Pouch tab)
    # dev.46 (alt A): choice planks on the Ready footer's line (y 1064), tap rects 596 x 134 (dev.45: 120 tall from 944)
    fw = (W - 32 - 16) // 2
    for i, name in enumerate(['Gear', 'Bag']):
        x = 16 + i * (fw + 16)
        fly += choice_button(x, FOOT_BOTTOM - CHOICE_VIS[FOOT_H][1], fw, FOOT_H, name, 32, 'fly_' + name.lower())
        fly.append({'type': 'rect', 'rect': [x, FOOT_TAP_Y, fw, FOOT_TAP_H], 'color': '#00000000', 'frame': 0,
                    'on_tap': 'go_gear' if name == 'Gear' else 'go_gpouch', 'haptic': 'light'})
    # dev.33: the native D-pad cluster on the flying page too (the top-screen copy is hidden by
    # default): in flight it mirrors Help / the eye, in freefall the Loftwing on Down (the
    # reader analysis "Bird", two boots: the native call icon shows in the visible
    # freefall phase). Bottom right of the sky map, clear of the wings and the compass
    # (FLY_DPAD_BOX, the module's kFlyAvoid entry for the island names).
    fly.extend(dpad_cluster(FLY_DP))
    # same stick pad on the flying page: on the Loftwing the right stick is not the sword, so
    # the pad stays (it also steers nothing harmful); swipes keep their fallback.
    manifest['pages'].append({'id': 'fly', 'title': 'Flying', 'widgets': fly})
    known = ' || '.join(f'pouch.id{{i}} == {k}' for k in ITEM_ICONS)
    for i in range(8):
        manifest['derived'] += [
            {'name': f'pouch.has{i}', 'expr': f'pouch.id{i} > 0 && pouch.id{i} < 200'},
            {'name': f'pouch.icon{i}', 'expr': known.replace('{i}', str(i))},
            # counts: satchels/quivers/bags and potions carry a count; shields show durability elsewhere
            {'name': f'pouch.cnt{i}', 'expr': f'pouch.has{i} && pouch.n{i} > 1 && !(pouch.id{i} >= 116 && pouch.id{i} <= 125)'},
            {'name': f'pouch.n{i}d0', 'expr': f'floor(pouch.n{i} / 10) % 10'},
            {'name': f'pouch.n{i}d1', 'expr': f'pouch.n{i} % 10'},
        ]
        data_points[f'pouch.id{i}'] = {'type': 'u16', 'addr': f'main+{SLOT + 0x7C0 + 4 * i:#x}'}
        data_points[f'pouch.n{i}'] = {'type': 'u16', 'addr': f'main+{SLOT + 0x7C2 + 4 * i:#x}'}
    # Scene gate: before a file is loaded (title / Quest Logs after boot) the live save slot is
    # empty (stage id 0, live 2026-11-07); show standby there. Ready / flying only in gameplay.
    # Pause menus keep the gameplay page (owner decision). Save-and-quit back to the title keeps the
    # old slot contents, so the title gate below uses the scene start name instead.
    standby = [img(K + 'bg/0/0/1240/1080', 0, 0, W, H),
               img(K + 'lyt/Map2D/th_skyloftMap_00', W // 2 - 220, 266, 440, 560),
               text_img('title', 34, 'Skyward Sword HD', W // 2, 900, 552, 67)]
    # any other game build: the module refuses it and no points load, so page.mode never resolves
    # and the companion stays on this first page (dev.42: the notice lived on Ready,
    # which another build never reaches). Built-in block font, as no module art exists there.
    standby += wrong_build_notice()
    manifest['pages'].insert(0, {'id': 'standby', 'title': 'Standby', 'widgets': standby})
    # Title gate (2026-11-07 gaps-fix, main .bss diffs boot-title / file select / title after save-and-quit
    # vs gameplay / pause / map, confirmed over two quit cycles): main+0x182A7F2 holds the current
    # start-entrance name, "TitleStart" on the title and Quest Logs, "DefaultStart"-style names in play.
    # u32 at +0x182A7F4 == 'tleS' (0x53656C74) only on the title side.
    data_points['scene.name4'] = {'type': 'u32', 'addr': 'main+0x182a7f4'}
    manifest['derived'] += [
        {'name': 'scene.title', 'expr': 'scene.name4 == 1399221364'},
        {'name': 'sword.none', 'expr': '!sshd.sword.has'},
        {'name': 'page.mode', 'expr': '(scene.title || stage.id == 0 || abs(link.x) + abs(link.z) < 1) ? 0 : (stage.is_F020 ? 2 : 1)'}]
    for act in ('spin_h', 'spin_v'):
        manifest['actions'][act] = {'kind': 'module', 'action': act, 'argument': 0}
    manifest['page_binds'] = [
        {'point': 'page.mode', 'equals': m, 'when_equal': {'page': pg, 'transition': 'fade', 'duration_ms': 200}}
        for m, pg in ((0, 'standby'), (1, 'ready'), (2, 'fly'))]
    manifest['derived'] += [
        {'name': 'sky.u', 'expr': 'link.x'},
        {'name': 'wing.valid', 'expr': 'wings <= 3'},
        {'name': 'wing.on0', 'expr': 'wings > 0'},
        {'name': 'wing.on1', 'expr': 'wings > 1'},
        {'name': 'wing.on2', 'expr': 'wings > 2'},
        {'name': 'sky.v', 'expr': '-link.z'},
        {'name': 'stage.is_F020', 'cmp': 'eq', 'a': 'stage.id', 'b': stage_u64('F020')},
        # 0 at the cloud, -(GH-110) px at the sun
        {'name': 'alt.dy', 'expr': f'-clamp((link.y - ({ALT_LO})) / ({ALT_HI - ALT_LO}), 0, 1) * {GH - 110}',
         'floor': True},
    ]
    # dev.38: a moved view stays on its picture (the window is kept inside [min, max], centred on an
    # axis it outgrows); applies to pan_zoom widgets only, so the Ready minimaps are unchanged
    for a in areas.values():
        a['clamp_view'] = True
    build_pages(manifest, data_points)
    data['points'].update(data_points)
    return manifest, data



# Rendered widths (dsmod-sshd-art-test) of the txt/ labels used by the footer pages; drawn 1:1.
TW = {('title', 34, 'Gear'): 136, ('title', 34, 'Map'): 122, ('title', 34, 'Collection'): 292,
      ('title', 34, 'Settings'): 242, ('plank', 28, 'Pouch'): 134,
      ('plank', 28, 'Collection'): 219, ('plank', 30, 'Back'): 118,
      ('plank', 26, 'Reset'): 112, ('plank', 30, 'Bugs'): 114, ('plank', 30, '1F'): 66, ('plank', 30, 'B1'): 67,
      ('plank', 30, 'B2'): 68, ('plank', 30, 'B3'): 67,
      ('plank', 30, 'Treasures'): 226, ('plank', 26, 'Shown'): 131, ('plank', 26, 'Hidden'): 147,
      ('plank', 26, 'Dungeon layout before the Map'): 621,
      ('plank', 26, 'Top D-pad'): 210, ('plank', 26, 'Top rupees'): 221, ('plank', 26, 'Simple top-screen HUD'): 460, ('plank', 26, 'Off'): 78, ('plank', 26, 'On'): 63, ('plank', 26, 'No reader yet'): 273,
      ('plank', 26, 'Sword'): 127, ('plank', 26, 'Shield'): 127, ('plank', 26, 'Item Check'): 230,
      ('plank', 26, 'Not now'): 167, ('plank', 26, 'Put away'): 185, ('plank', 26, 'Spin'): 93, ('plank', 28, 'Options'): 168,
      ('plank', 30, 'Gear'): 119, ('plank', 30, 'Map'): 105, ('plank', 30, 'Bag'): 92, ('plank', 32, 'Gear'): 127,
      ('plank', 32, 'Bag'): 98, ('plank', 22, 'Not now'): 141, ('plank', 22, 'Put away'): 156,
      ('plank', 24, 'Not now'): 154, ('plank', 24, 'Put away'): 170,
      ('hud', 26, 'Help'): 99, ('hud', 26, 'Close'): 111, ('hud', 26, 'Look'): 104, ('hud', 26, 'Dowse'): 128,
      ('hud', 26, 'Back'): 102}
CAP_H = {22: 45, 24: 50, 26: 54, 28: 58, 30: 62, 32: 66, 34: 67}  # rendered txt/ heights per font size
# (a style changes the colours only: 'choice', 'optrow' and 'doinfo' labels have the plank widths)


def t(style, cap, text, cx, y, **kw):
    h = {26: 54, 28: 58, 30: 62, 34: 67}[cap]
    return text_img(style, cap, text, cx, y, TW[(style, cap, text)], h, **kw)


def back_footer():
    # "Back" plank across the footer (owner 2026-11-11: just "Back", the native word)
    # Back returns to the gameplay page of the moment: Flying in the Sky, else Ready (dev.30: Back
    # from Bag > Options in flight showed Ready with an empty map circle)
    # dev.46 (alt A): the choice plank ending on y 1064 like the Ready footer, tap rect 1208 x 134
    rect = [16, FOOT_TAP_Y, W - 32, FOOT_TAP_H]
    return choice_button(16, FOOT_BOTTOM - CHOICE_VIS[FOOT_H][1], W - 32, FOOT_H, 'Back', 30, 'back') + [
        {'type': 'rect', 'rect': rect, 'color': '#00000000', 'frame': 0,
         'on_tap': 'go_ready', 'haptic': 'light', 'need_bind': 'page.not_fly'},
        {'type': 'rect', 'rect': rect, 'color': '#00000000', 'frame': 0,
         'on_tap': 'go_fly', 'haptic': 'light', 'need_bind': 'page.is_fly'}]


# Bag section tabs (the footer's Bag opens Pouch). The game has no cog or settings icon: the native
# candidates (pouch-wheel knob ring, pouch-wheel disc, harp, Fi; 2026-11-11 candidate sheet) read as
# items, so the third tab carries the pause menu's own word for its settings, "Options"
# (MenuPause pause_00 B_options_00), in the plank font like the other two tabs.
TAB_W = (W - 32 - 24) // 3
BAG_TABS = [('Pouch', 'go_gpouch'), ('Collection', 'go_coll'), ('Options', 'go_set')]
# Bag > Options rows for the top-screen HUD copies the bottom screen shows (action, flag, label)
TOP_HIDE = [('hud_dpad_toggle', 'hud.dpad', 'Top D-pad'), ('hud_rupee_toggle', 'hud.rupee', 'Top rupees')]


def bag_tabs(active):
    # dev.46 (owner: "top bar like bag pouch and gear use alt B"): the Options screen row, the open tab
    # in the row's focus flash, eased out of the label band (state 1), the others the row at 62 % (state 3,
    # the approved mockup: the Options screen's only focus cue besides the flash is its bracket cursor);
    # label 'optrow' cap-centred on the visible row
    out = []
    x = 16
    v0, v1 = OPTROW_VIS[TAB_ART_H]
    for name, act in BAG_TABS:
        out.append(img(K + f'optrow/{TAB_W}/{TAB_ART_H}/{1 if name == active else 3}', x, TAB_ART_Y, TAB_W, TAB_ART_H))
        out.append(img(K + f'optrow/{TAB_W}/{TAB_ART_H}/2', x, TAB_ART_Y, TAB_W, TAB_ART_H,
                       need_bind=f'press.{PRESS["tab_" + name]}'))
        out.append(cap_label('optrow', 28, name, x + TAB_W / 2, TAB_ART_Y + (v0 + v1) / 2))
        # dev.41 (legibility minimums): the tap rect is the plank's full 112 px height
        out.append({'type': 'rect', 'rect': [x, 16, TAB_W, 112], 'color': '#00000000', 'frame': 0, 'on_tap': act,
                    'haptic': 'light'})
        x += TAB_W + 12
    return out


def build_pages(manifest, data_points):
    acts = manifest['actions']
    for a, pg in [('go_ready', 'ready'), ('go_gear', 'gear'), ('go_gpouch', 'gpouch'), ('go_coll', 'coll'),
                  ('go_set', 'settings'), ('go_map', 'mapfull'), ('go_fly', 'fly')]:
        acts[a] = {'kind': 'page', 'page': pg, 'transition': 'fade', 'duration_ms': 150}
    # dev.46: the pressed look's countdown (module PressTick publishes the clock sshd.ms, the look's
    # length sshd.press.hold and the diagnostics id sshd.press.diag; the runtime gives a module no tap
    # and no "@sel:"): press.on / press.off = a button of the group sshd.press is selected or not;
    # press.t_on holds the clock while nothing is selected, so it freezes at the tap that selects;
    # press.t_off holds it while selected, so it freezes at the tap that clears (a second tap on the
    # same button); press.last = the last id selected; press.id lights that id until hold ms later, and
    # otherwise the diagnostics id (0 = none)
    manifest['derived'] += [
        {'name': 'press.on', 'cmp': 'ge', 'a': '@sel:sshd.press', 'b': 0},
        {'name': 'press.off', 'cmp': 'lt', 'a': '@sel:sshd.press', 'b': 0},
        {'name': 'press.t_on', 'hold_last_nonzero': 'sshd.ms', 'hold_gate': 'press.off'},
        {'name': 'press.t_off', 'hold_last_nonzero': 'sshd.ms', 'hold_gate': 'press.on'},
        {'name': 'press.last', 'hold_last_nonzero': '@sel:sshd.press', 'hold_gate': 'press.on'},
        {'name': 'press.id', 'expr': 'press.last > 0 && sshd.ms - (press.on ? press.t_on : press.t_off) < sshd.press.hold ? '
                                     'press.last : sshd.press.diag'},
    ]
    manifest['derived'] += [{'name': f'press.{i}', 'cmp': 'eq', 'a': 'press.id', 'b': i}
                            for i in sorted(PRESS.values())]
    acts['dmap_toggle'] = {'kind': 'flag', 'flag': 'dmap.layout'}
    acts['hud_toggle'] = {'kind': 'flag', 'flag': 'hud.simple'}
    # top-screen HUD copies the bottom shows better (owner 2026-11-11): flag 0 = Off = the native top
    # copy hidden (the default), 1 = shown; the module's ApplyTopHide reads them (sshd_hudhide.h)
    for key, flag, _ in TOP_HIDE:
        acts[key] = {'kind': 'flag', 'flag': flag}
    manifest.setdefault('flags', {}).update({'dmap.layout': 0, 'hud.simple': 0,
                                             **{flag: 0 for _, flag, _ in TOP_HIDE}})
    manifest.setdefault('persist_flags', []).extend(['dmap.layout', 'hud.simple'] + [flag for _, flag, _ in TOP_HIDE])
    manifest['derived'] += [{'name': 'dmap.off', 'cmp': 'eq', 'a': '@flag:dmap.layout', 'b': 0},
                            {'name': 'hud.on', 'cmp': 'eq', 'a': '@flag:hud.simple', 'b': 0},
                            {'name': 'hud.off', 'cmp': 'ne', 'a': '@flag:hud.simple', 'b': 0},
                            {'name': 'page.is_fly', 'cmp': 'eq', 'a': 'page.mode', 'b': 2},
                            {'name': 'page.not_fly', 'cmp': 'ne', 'a': 'page.mode', 'b': 2}]
    for _, flag, _ in TOP_HIDE:
        manifest['derived'] += [{'name': flag + '.off', 'cmp': 'eq', 'a': '@flag:' + flag, 'b': 0},
                                {'name': flag + '.on', 'cmp': 'ne', 'a': '@flag:' + flag, 'b': 0}]
    bg = img(K + 'bg/0/0/1240/1080', 0, 0, W, H)

    # --- Map: the current area's map, Link's head, drag to pan / pinch to zoom (runtime pan_zoom on the
    # map widget, the follow view as its home) and the native-styled Reset plank (dev.38, owner
    # 2026-11-12: "make map can manipulated like pinch zoom, drag pane etc and add reset button to
    # default"). One view per area (widget id big_<area>; the dungeons' and the generic frame's
    # "_away" twin shares it, so a floor pick keeps the view). The home is the old near view
    # (Link-centred, following him while untouched); the pinch goes out to the whole area (at least
    # the old far view) and in to pan_view's limit; Reset glides the view home.
    mp = [bg, {'type': 'rect', 'rect': [12, 12, W - 24, 900], 'bg': '#FFCEBE8D', 'color': '#FF937544', 'frame': 4}]
    areas = manifest['map']['areas']
    resets = []  # (view id, the bind that shows it)
    for st, xmin, xmax, zmin, zmax, ppu, title in AREAS:
        # S000: twice the realm, so the whole realm stays in view wherever Link stands; S100 / S200:
        # the native-size blob, S300 no realm: fixed like the native trial map (no pan / zoom)
        win = (2 * S000_SPAN if st == REALM_WHOLE else TRIAL_UPX * (W - 40)) if st in REALMS else MAP_NEAR_WINDOW
        pannable = st not in REALMS or st == REALM_WHOLE
        key = st + '_m' if st in PLACE_STAGES or st in DUNGEONS else st
        vid = 'big_' + st
        for away in ((0, 1) if st in DUNGEONS else (0,)):
            w = {'type': 'map', 'id': vid, 'area': key, 'rect': list(MAP_RECT),
                 'bg': '#FFD9C48E', 'area_label': False, 'follow_window': win,
                 **({'image_bind': 'sshd.dmap.src'} if st in DUNGEONS else {'image_bind': 'sshd.fmap.src'} if st in FIELD_STATE else {}),
                 'marker_x_bind': ubind(st), 'marker_y_bind': vbind(st),
                 'marker_src': K + ('blank' if away else 'lyt/Map2D/tr_linkPIcon_00'),
                 'marker_size': [84, 84],
                 'need_bind': (f'map.show_{st}' + ('_away' if away else '')) if st in DUNGEONS else 'stage.show_' + st}
            if pannable:
                w.update(pan_view(areas[key], ppu, win))
            mp.append(w)
            if st in DUNGEONS:
                # Link's head only on the floor shown (a picked floor tab hides it, as the native
                # basement tabs omit Link)
                manifest['derived'].append({'name': f'map.show_{st}' + ('_away' if away else ''),
                                            'expr': f'stage.show_{st} && sshd.lk.away == {away}'})
        if pannable:
            resets.append((vid, 'stage.show_' + st))
    # dev.34: the generic stage frame (dev.35: the Map page's own area GEN_m for the module's floor-tab
    # pick), Link's head only on his own floor
    for away in (0, 1):
        sfx = '_away' if away else ''
        mp.append({'type': 'map', 'id': 'big_GEN', 'area': 'GEN_m', 'rect': list(MAP_RECT),
                   'bg': '#FFD9C48E', 'area_label': False, 'follow_window': GEN_NEAR_WINDOW, 'image_bind': 'sshd.gen.src',
                   'marker_x_bind': 'sshd.gen.lu', 'marker_y_bind': 'sshd.gen.lv',
                   'marker_src': K + ('blank' if away else 'lyt/Map2D/tr_linkPIcon_00'), 'marker_size': [84, 84],
                   'need_bind': f'map.show_GEN{sfx}', **pan_view(areas['GEN_m'], GEN_PPU, GEN_NEAR_WINDOW)})
        manifest['derived'].append({'name': f'map.show_GEN{sfx}', 'expr': f'stage.show_GEN && sshd.lk.away == {away}'})
    resets.append(('big_GEN', 'stage.show_GEN'))
    # dev.35: Inside the Thunderhead: around Link on the Sky picture, out to the whole Sky
    mp.append({'type': 'map', 'id': 'big_F023', 'area': THUNDERHEAD_STAGE, 'rect': list(MAP_RECT),
               'bg': '#FFD9C48E', 'area_label': False, 'follow_window': THUNDERHEAD_NEAR_WINDOW,
               'marker_x_bind': 'sky.u', 'marker_y_bind': 'sky.v',
               'marker_src': K + 'lyt/Map2D/tr_linkPIcon_00', 'marker_size': [84, 84],
               'need_bind': 'stage.is_F023', **pan_view(areas[THUNDERHEAD_STAGE], SKY[5], THUNDERHEAD_NEAR_WINDOW)})
    resets.append(('big_F023', 'stage.is_F023'))
    # the native area-name plate (owner 2026-11-11) in the area's region colour, and the native compass
    for st, *_r, title in AREAS:
        mp += area_plate(title, 32, 26, region=REGION_COLOR[st], need_bind='stage.is_' + st,
                         label=f'MAP_{MAP_NAME_ID[st]:02d}')
    # dev.34: the generic stage's plate in its STIF area colour, the native MAP_<id> text
    gk = 120 / 112
    mp.append(img(K + f'areaplate/{GEN_PLATE_W}/120/1', 32, 26, GEN_PLATE_W, 120, src_bind='sshd.gen.plate',
                  need_bind='gen.titled'))
    mp.append(img(K + 'blank', 32 + (GEN_PLATE_W - int(16 * gk)) // 2 - GEN_PLATE_TEXT_W // 2, int(26 + 51.2 * gk - 43),
                  GEN_PLATE_TEXT_W, 86, src_bind='sshd.gen.ptext', need_bind='gen.titled'))
    # dev.35: Inside the Thunderhead's plate (native teal, MAP_57 on two lines)
    mp.append(img(K + f'areaplate/{GEN_PLATE_W}/120/{REGION_COLOR["F020"]}', 32, 26, GEN_PLATE_W, 120,
                  need_bind='stage.is_F023'))
    mp.append(img(K + f'ctitle/plate/34/{GEN_PLATE_TEXT_W}/86/MAP_{THUNDERHEAD_NAME_ID:02d}',
                  32 + (GEN_PLATE_W - int(16 * gk)) // 2 - GEN_PLATE_TEXT_W // 2, int(26 + 51.2 * gk - 43),
                  GEN_PLATE_TEXT_W, 86, need_bind='stage.is_F023'))
    mp.append(img(K + 'compass', W - 100 - COMPASS // 2, 96 - COMPASS // 2, COMPASS, COMPASS))
    # Reset (bottom right of the map, where the Zoom plank was): the native plank with the word
    # "Reset"; a tap glides the shown area's view home (view_reset), a drag that starts on it pans
    # the map like anywhere else. Shown wherever the map can be moved (not in Farore's / Din's /
    # Nayru's trials: their maps are fixed like the native).
    manifest['derived'].append({'name': 'map.pannable', 'expr': ' || '.join(b for _, b in resets)})
    # dev.46 (alt A): the choice plank, opaque on the paper, centred in the unchanged tap rect; each
    # tap rect also selects the press id (select_group sshd.press) for the pressed look (derived press.*)
    mp += choice_button(RESET_PLANK[0], RESET_PLANK[1] + (RESET_H - RESET_ART_H) // 2, RESET_W, RESET_ART_H, 'Reset', 26,
                        'map_reset', opaque=True, need_bind='map.pannable')
    for vid, bind in resets:
        acts['map_reset_' + vid[4:]] = {'kind': 'view_reset', 'view': vid}
        mp.append({'type': 'rect', 'rect': [RESET_PLANK[0], RESET_PLANK[1], RESET_W, RESET_H], 'color': '#00000000', 'frame': 0,
                   'on_tap': 'map_reset_' + vid[4:], 'haptic': 'light', 'need_bind': bind,
                   'select_group': 'sshd.press', 'payload': PRESS['map_reset']})
    # Native floor tabs (dev.30): one per active floor of the native floor table (Cistern and
    # Sanctuary 1F/B1, Sandship 1F/B1/B2/B3), top to bottom, in the native MapFloorBtn art
    # (module floortab/<floor>/<lit>). A tap shows that floor directly (module floor_pick: the
    # pick holds until the Map page closes or the stage changes, else the tab follows Link's floor);
    # Link's head marks his own floor beside its tab.
    for i in range(FLOOR_TABS):
        x, y = FLOOR_TAB_X, FLOOR_TAB_Y + i * FLOOR_TAB_STEP
        on = f'sshd.ft{i}.on'
        acts[f'floor_pick_{i}'] = {'kind': 'module', 'action': 'floor_pick', 'argument': i}
        mp.append(img(K + 'floortab/0/0', x, y, FLOOR_TAB_W, FLOOR_TAB_H, src_bind=f'sshd.ft{i}.src', need_bind=on))
        mp.append(img(K + 'lyt/Map2D/tr_linkPIcon_00', x + FLOOR_TAB_W - 6, y + (FLOOR_TAB_H - 56) // 2, 56, 56,
                      need_bind=f'sshd.ft{i}.here'))
        ty = y - (FLOOR_TAB_TAP_H - FLOOR_TAB_H) // 2
        mp.append({'type': 'rect', 'rect': [x, ty, FLOOR_TAB_W, FLOOR_TAB_TAP_H], 'color': '#00000000', 'frame': 0,
                   'on_tap': f'floor_pick_{i}', 'haptic': 'light', 'need_bind': on})
    # beacons: no reader -> omitted (CONTENT-SPEC: no data without a reader)
    mp += back_footer()
    manifest['pages'].append({'id': 'mapfull', 'title': 'Map', 'widgets': mp})

    # --- Gear (equipment only): a "Gear" title bar, sword (current only) and shield (display only), and the owned
    # B-item wheel as a ring around a hub that shows the current ZR item (the native HUD's ZR
    # "Items" icon). The bar's right end shows the current R item (pouch wheel's last-used slot).
    # Tap a ring item then the hub, or drag it onto the hub, to make it the ZR item: the module
    # stages the player's own ZR-wheel commit for the helper (01002DA013484000.cpp EquipAction),
    # refused unless Link's hands are free in free gameplay.
    import math
    ge = [bg]
    # dev.46 (owner: "top bar like bag pouch and gear use alt B"): the header is the Options screen
    # row like the Bag tab bar (visible 16..110), "Gear" in the row's white label, cap-centred; the R
    # glyph and the R item keep their places
    v0, v1 = OPTROW_VIS[TAB_ART_H]
    ge.append(img(K + f'optrow/{W - 32}/{TAB_ART_H}/0', 16, TAB_ART_Y, W - 32, TAB_ART_H))
    ge.append(cap_label('optrow', 30, 'Gear', W / 2, TAB_ART_Y + (v0 + v1) / 2))
    ge.append(img(K + 'sq/DoButton/tr_rBtn_00', W - 196, 16 + 48 - 30, 60, 60))
    ge.append(img(K + 'socket/84', W - 128, 16 + 48 - 42, 84, 84))
    ge.append(img(POUCH_SOURCES[0], W - 122, 16 + 48 - 36, 72, 72, bind='pouch.rid', src_names=POUCH_SOURCES,
                  need_bind='pouch.ron'))
    ge.append(img(K + 'plank/corner/360/700', 40, 160, 376, 716))
    ge.append(img(K + 'socket/220', 118, 330, 220, 220, need_bind='sword.none'))
    ge.append(sword_icon(166, 200, 125, 384))
    # dev.46 P02 (owner: "for ux spec i only accept p01 and p02"): the shield socket up 38 px and its
    # label up 56 px, so "Shield" sits inside the panel (dev.45: on its bottom rim, y 850..881 over
    # the edge at 852); "Sword" up 28 px with it (the crop), so the raised socket keeps its gap
    ge.append(t('plank', 26, 'Sword', 228, 600 - 28))
    ge.append(img(K + 'socket/150', 153, 680 - 38, 150, 150))
    ge.append(shield_icon(166, 693 - 38, 124, 124))
    ge.append(t('plank', 26, 'Shield', 228, 836 - 56))
    # dev.37 (owner 2026-11-12): the Sword / Shield column is display only (no tap, no drag)
    cx, cy, r = 820, 530, 290
    ge.append(img(K + 'socket/220', cx - 110, cy - 110, 220, 220))
    ge.append(img(K + 'sq/DoButton/uc_bomb_00', cx - 78, cy - 92, 156, 156, src_bind='sshd.bcur.src',
                  need_bind='sshd.bcur.on'))
    ge.append(img(K + 'sq/DoButton/tr_zrBtn_00', cx - 32, cy + 50, 64, 64))
    ge.append({'type': 'rect', 'rect': [cx - 110, cy - 110, 220, 220], 'color': '#00000000', 'frame': 0,
               'drop_action': 'bitem_equip', 'accept_group': 'bwheel', 'haptic': 'confirm'})
    for i in range(8):  # clockwise from the top, native wheel order (module PublishShortcuts)
        a = -math.pi / 2 + i * math.pi / 4
        x, y = int(cx + r * math.cos(a)), int(cy + r * math.sin(a))
        ge.append(img(K + 'softglow', x - 95, y - 95, 190, 190, need_bind=f'bsel.w{i}'))
        ge.append(img(K + 'socket/150', x - 75, y - 75, 150, 150))
        ge.append(img(K + 'sq/DoButton/uc_bomb_00', x - 62, y - 62, 124, 124, src_bind=f'sshd.w{i}.src',
                      need_bind=f'sshd.w{i}.on', payload=i, select_group='bwheel', draggable=True,
                      haptic='light'))
        ge.append(img(K + 'sq/MenuPause/tr_zrBtnSmall_00', x + 30, y - 76, 46, 46, need_bind=f'sshd.w{i}.eq'))
        manifest['derived'].append({'name': f'bsel.w{i}', 'cmp': 'eq', 'a': '@sel:bwheel', 'b': i})
    # dev.46: a refused drop says "Not now" on the refusal pill (alt A's one component), between the top
    # socket and the hub. dev.48: centred in that gap (y 315..420)
    ge += refusal_pill(cx, round((cy - r + 75 + cy - 110) / 2), 'sshd.pouch.refused',
                       [('Not now', 'sshd.pouch.refused')])
    ge += back_footer()
    manifest['pages'].append({'id': 'gear', 'title': 'Gear', 'widgets': ge})
    acts['bitem_equip'] = {'kind': 'module', 'action': 'bitem_equip', 'argument': '$payload'}

    # --- Bag / Pouch: the 8 pouch slots on a ring (slot+0x7C0) around a hub showing the current R
    # item, beside the Item Check storage (slot+0x7E0, 60 slots, 3 columns, scrolled). Drag (or tap,
    # then tap the target) like the native Item Check: a ring item onto another ring slot rearranges,
    # ring <-> storage stores / retrieves / exchanges; a ring item onto the hub makes it the R item
    # (the slot the native pouch wheel opens on). The module stages each drop for the helper, which
    # runs the game's own routines on the game thread (01002DA013484000.cpp PouchAction /
    # EquipAction); outside free gameplay the drop is refused. Locked pouch slots take no drops.
    gp = [bg] + bag_tabs('Pouch')
    pcx, pcy, pr = 372, 528, 282
    gp.append(img(K + 'socket/220', pcx - 110, pcy - 110, 220, 220))
    gp.append(img(POUCH_SOURCES[0], pcx - 75, pcy - 90, 150, 150, bind='pouch.rid', src_names=POUCH_SOURCES,
                  need_bind='pouch.ron'))
    gp.append(t('plank', 28, 'Pouch', pcx, pcy - 29, need_bind='pouch.roff'))
    gp.append(img(K + 'sq/DoButton/tr_rBtn_00', pcx - 32, pcy + 50, 64, 64))
    gp.append({'type': 'rect', 'rect': [pcx - 110, pcy - 110, 220, 220], 'color': '#00000000', 'frame': 0,
               'drop_action': 'pouch_equip', 'accept_group': 'pouch', 'haptic': 'confirm'})
    # native pouch wheel positions (live 2026-11-09, three swaps): slots 0-3 top, left, right,
    # bottom; the expansion slots 4-7 top-right, bottom-left, top-left, bottom-right
    for i, deg in enumerate((0, 270, 90, 180, 45, 225, 315, 135)):
        a = -math.pi / 2 + math.radians(deg)
        x, y = int(pcx + pr * math.cos(a)), int(pcy + pr * math.sin(a))
        gp.append(img(K + 'softglow', x - 95, y - 95, 190, 190, need_bind=f'psel.p{i}'))
        gp.append(img(K + 'socket/150', x - 75, y - 75, 150, 150, need_bind=f'pouch.open{i}'))
        gp.append(img(K + 'socket/150', x - 75, y - 75, 150, 150, need_bind=f'pouch.locked{i}',
                      tint='#60FFFFFF'))
        # empty unlocked slot: drop target only
        gp.append({'type': 'rect', 'rect': [x - 75, y - 75, 150, 150], 'color': '#00000000', 'frame': 0,
                   'drop_action': f'pouch_to_{i}', 'accept_group': 'pouch', 'haptic': 'confirm',
                   'need_bind': f'pouch.open{i}'})
        icon = pouch_icon(i, x - 62, y - 62, 124)
        icon.update({'payload': i, 'select_group': 'pouch', 'draggable': True,
                     'drop_action': f'pouch_to_{i}', 'accept_group': 'pouch', 'haptic': 'confirm'})
        gp.append(icon)
        gp += pouch_digits(i, x + 10, y + 26)
        gp.append(img(K + 'sq/MenuPause/tr_rBtnSmall_00', x + 30, y - 76, 46, 46, need_bind=f'pouch.rb{i}'))
    # Item Check storage, 3 x 20 in a scroll region (one repeat template per layer, {i} = storage
    # slot). Scroll by dragging the gaps around the icons, an empty slot or the bar; an icon drags.
    # dev.48: the scroll strip ICHECK_BAR_W (90) px wide, 1100..1190, inside the panel's safe box
    # (body 744..1208, rim 18), 12 px right of the cells: 108 px pitch (dev.47: 116), cells 764..1088,
    # icons ICHECK_ICON 92 with 16 px between them, every cell inside the region
    sx0, sy0, cw, ch = 764, 204, ICHECK_PITCH, 116
    wx, ww = 760, ICHECK_BAR_X + ICHECK_BAR_W - 760
    gp.append(img(K + 'plank/corner/464/780', 744, 136, 480, 796))
    gp.append(t('plank', 26, 'Item Check', 744 + 240, 140))
    gp.append({'type': 'rect', 'rect': [wx, 200, ww, 708], 'color': '#00000000', 'bg': '#18000000', 'frame': 0})
    icheck = {'id': 'icheck', 'rect': [wx, 200, ww, 708], 'cols': 3, 'row_h': ch, 'pad': 8,
              'bar': '#C0CFAA6A', 'bar_track': '#40000000', 'bar_w': ICHECK_BAR_W}
    # The strip (the region's rightmost ICHECK_BAR_W px) contains no selectable / draggable cell.
    # Runtime 22 therefore gives every gesture starting there exclusively to the scroll region.
    grid = {'repeat': 60, 'repeat_cols': 3, 'repeat_dx': cw, 'repeat_row_dy': ch, 'scroll': 'icheck'}
    gp.append(img(K + 'softglow', sx0 + 2, sy0 - 4, cw - 4, ch + 8, need_bind='psel.s{i}', **grid))
    # dev.42: the drag cell (the icon) is ICHECK_ICON square, at least the 88 px dense-grid minimum
    # with 16+ px between neighbours (dev.48 pitch 108); its socket grows with it, and the counts
    # use ICHECK_DIGIT_H tall digits (the 22 px essential cap, outline included)
    so, ic, dh = ICHECK_SOCKET, ICHECK_ICON, ICHECK_DIGIT_H
    dw, dp = round(dh * 0.75), round(dh * 0.6)
    gp.append(img(K + f'socket/{so}', sx0 + (cw - so) // 2, sy0 + (ch - so) // 2, so, so, **grid))
    gp.append({'type': 'rect', 'rect': [sx0, sy0, cw, ch], 'color': '#00000000', 'frame': 0,
               'drop_action': 'stock_to_{i}', 'accept_group': 'pouch', 'haptic': 'confirm', **grid})
    gp.append(img(POUCH_SOURCES[0], sx0 + (cw - ic) // 2, sy0 + (ch - ic) // 2, ic, ic, bind='stock.id{i}',
                  src_names=POUCH_SOURCES, need_bind='stock.icon{i}', payload='{i+100}', select_group='pouch',
                  draggable=True, drop_action='stock_to_{i}', accept_group='pouch', haptic='confirm', **grid))
    for k in range(2):
        gp.append(digit_widget(sx0 + cw - 6 - dp - dw + k * dp, sy0 + ch - 4 - dh, dw, dh, bind=f'stock.n{{i}}d{k}',
                               need_bind='stock.cnt{i}', **grid))
    # dev.48: the pill centred between the ring's top socket and the hub (y 321..418)
    gp += refusal_pill(pcx, round((pcy - pr + 75 + pcy - 110) / 2), 'sshd.pouch.refused',
                       [('Not now', 'sshd.pouch.refused')])
    gp += back_footer()
    manifest['pages'].append({'id': 'gpouch', 'title': 'Pouch', 'scrolls': [icheck], 'widgets': gp})
    acts['pouch_equip'] = {'kind': 'module', 'action': 'pouch_equip', 'argument': '$payload'}
    # current R item: the file's lastUsedPouchItemSlot (slot+0x53B0, 8 = none)
    data_points['pouch.r'] = {'type': 'u8', 'addr': f'main+{SLOT + 0x53B0:#x}'}
    manifest['derived'] += [
        {'name': 'pouch.rid', 'expr': ' : '.join(f'pouch.r == {i} ? pouch.id{i}' for i in range(8)) + ' : 0'},
        {'name': 'pouch.ron', 'expr': 'pouch.r < 8 && (' + ' || '.join(
            f'pouch.rid == {k}' for k in sorted(ITEM_ICONS)) + ')'},
        {'name': 'pouch.roff', 'expr': '!pouch.ron'},
        # Ready R corner count, like the pouch slots: above 1 only, never a shield's durability
        {'name': 'pouch.rn', 'expr': ' : '.join(f'pouch.r == {i} ? pouch.n{i}' for i in range(8)) + ' : 0'},
        {'name': 'pouch.rcnt', 'expr': 'pouch.ron && pouch.rn > 1 && !(pouch.rid >= 116 && pouch.rid <= 125)'},
        {'name': 'pouch.rnd0', 'expr': 'floor(pouch.rn / 10) % 10'},
        {'name': 'pouch.rnd1', 'expr': 'pouch.rn % 10'}]
    for i in range(8):
        acts[f'pouch_to_{i}'] = {'kind': 'module', 'action': f'pouch_to_{i}', 'argument': '$payload',
                                 'enabled_bind': 'sshd.pouch.ok'}
        manifest['derived'] += [
            {'name': f'pouch.open{i}', 'expr': f'sshd.pouch.slots > {i}'},
            {'name': f'pouch.locked{i}', 'expr': f'sshd.pouch.slots <= {i}'},
            {'name': f'pouch.rb{i}', 'expr': f'pouch.r == {i} && pouch.has{i}'},
            {'name': f'psel.p{i}', 'cmp': 'eq', 'a': '@sel:pouch', 'b': i}]
    ids = sorted(ITEM_ICONS)
    spans = [[ids[0], ids[0]]]
    for k in ids[1:]:
        if k == spans[-1][1] + 1:
            spans[-1][1] = k
        else:
            spans.append([k, k])
    known = ' || '.join(f'stock.id{{j}} == {a}' if a == b else f'(stock.id{{j}} >= {a} && stock.id{{j}} <= {b})'
                        for a, b in spans)
    for j in range(60):
        acts[f'stock_to_{j}'] = {'kind': 'module', 'action': f'stock_to_{j}', 'argument': '$payload',
                                 'enabled_bind': 'sshd.pouch.ok'}
        manifest['derived'] += [
            {'name': f'psel.s{j}', 'cmp': 'eq', 'a': '@sel:pouch', 'b': 100 + j},
            {'name': f'stock.icon{j}', 'expr': known.replace('{j}', str(j))},
            {'name': f'stock.cnt{j}', 'expr': f'stock.id{j} > 0 && stock.id{j} < 200 && stock.n{j} > 1 && '
                                              f'!(stock.id{j} >= 116 && stock.id{j} <= 125)'},
            {'name': f'stock.n{j}d0', 'expr': f'floor(stock.n{j} / 10) % 10'},
            {'name': f'stock.n{j}d1', 'expr': f'stock.n{j} % 10'}]
        data_points[f'stock.id{j}'] = {'type': 'u16', 'addr': f'main+{SLOT + 0x7E0 + 4 * j:#x}'}
        data_points[f'stock.n{j}'] = {'type': 'u16', 'addr': f'main+{SLOT + 0x7E2 + 4 * j:#x}'}

    # --- Collection: owned bugs and treasures as native icon grids with counts, in one scroll
    # region. Counts are 7-bit fields in the item-flag u16 words slot+0xA34..0xA4F (lo = bits 0-6,
    # hi = bits 7-13), mapped against the native Collection screen on hero (all 28 values) + two
    # pokes for the duplicate values (2026-11-07). Icons: the native Collection's pattern
    # animations in Layout/DoButton (commonIcon_00_bugs: tr_bugIcon_00..11; commonIcon_01_sozai:
    # tr_sozaiIcon_03 + uk_so*_00), matched to the item names (ids 161-176) by their art.
    BUGS = [(0xA4C, 0), (0xA4A, 1), (0xA46, 0), (0xA44, 1), (0xA4E, 1), (0xA4A, 0), (0xA48, 1), (0xA44, 0),
            (0xA4E, 0), (0xA4C, 1), (0xA48, 0), (0xA46, 1)]
    TREAS = [(0xA42 - 2 * (k // 2), k % 2) for k in range(16)]
    TREAS_ICONS = ['uk_soaHachisu_00', 'uk_sobHaneA_00', 'uk_socTumble_00', 'tr_sozaiIcon_03',
                   'uk_soeKoseki_00', 'uk_sofHana_00', 'uk_sogMagatamaA_00', 'uk_sohMagatamaB_00',
                   'uk_soiPuyo_00', 'uk_sojClaw_00', 'uk_solHorn_00', 'uk_sokDokuroA_00',
                   'uk_somEvil_00', 'uk_sonHaneB_00', 'uk_sooDokuroB_00', 'uk_sopGoddess_00']
    for w in sorted({a for a, _ in BUGS + TREAS}):
        data_points[f'coll.w{w:x}'] = {'type': 'u16', 'addr': f'main+{SLOT + w:#x}'}
    # The native Collection screen (owner 2026-11-11: its exact design), Layout MenuPause pause_01 in
    # its own arrangement scaled to the page: the green bug panel 4 x 3 (Null_01, 340 x 256 units at
    # (-187, 32)), the purple quest-item row of 4 below it (W_bg_02, 340 x 94 at (-187, -135)) and the
    # tan treasure panel 4 x 4 (Null_02, 340 x 340 at (187, -12)); 74-unit slot bases, 64-unit icons,
    # counts in the icon's bottom-right (commonIcon T_comNumber_00 at (+20, -20)). Everything fits the
    # page (714 x 342 units at COLL_S px per unit), so there is no scroll region.
    # Shown like the native (dLytPauseDisp01_c): an icon is drawn when its item flag is set (the Deku
    # Hornet: story flag 122), otherwise its black silhouette; quest items per their have / given
    # flags with the red check mark once given (Gratitude Crystals: count while collecting, check
    # when the quest is finished). Item flags: slot+0x9E4 + (US 805A9BD8 offset ^ 1)
    # (skywardsword-tools itemflags.txt; the same mapping as the bow / net / sword bits above);
    # story flags: the live block main+0x182E1F8 + (US 805A9AD8 offset ^ 1), as the cubes.
    COLL_S = 1.65
    ccx0, ccy0 = W // 2, 136 + 784 // 2  # page point of layout (0, -11), the arrangement's centre

    def cpt(ux, uy):
        return ccx0 + ux * COLL_S, ccy0 - (uy + 11) * COLL_S

    def cpx(u):
        return int(round(u * COLL_S))
    # item-flag words (u16, little-endian: byte b of word w is bit 8 * (b - w))
    for off in (0x9F2, 0x9F4, 0x9F6, 0x9FC):
        data_points[f'iflag.{off:x}'] = {'type': 'u16', 'addr': f'main+{SLOT + off:#x}'}
    for off in (0x12, 0x28, 0x34, 0x38, 0x3E):
        data_points[f'sflag.{off:x}'] = {'type': 'u16', 'addr': f'main+{0x182E1F8 + off:#x}'}

    def iflag(item):
        # item ids 141..146 -> byte 0x9F3 bits 2..7; 147..152 -> 0x9F4 bits 0..5; 158..160 -> 0x9FD
        # bits 3..5; 161..166 -> 0x9F5 bits 2..7; 167..174 -> 0x9F6 bits 0..7; 175..176 -> 0x9F7 bits 0..1
        byte, bit = (0x9F3, item - 139) if item <= 146 else (0x9F4, item - 147) if item <= 152 else \
            (0x9FD, item - 155) if item <= 160 else (0x9F5, item - 159) if item <= 166 else \
            (0x9F6, item - 167) if item <= 174 else (0x9F7, item - 175)
        word = byte & ~1
        return f'(floor(iflag.{word:x} / {1 << (bit + 8 * (byte - word))}) % 2 == 1)'

    def sflag(byte, bit):
        word = byte & ~1
        return f'(floor(sflag.{word:x} / {1 << (bit + 8 * (byte - word))}) % 2 == 1)'
    BUG_ITEMS = [143, 146, 149, 152, 142, 145, 148, 151, 141, 144, 147, 150]  # native order
    co = [bg] + bag_tabs('Collection')
    panels = [(0, -187, 32, 340, 256), (1, -187, -135, 340, 94), (2, 187, -12, 340, 340)]
    for kind, ux, uy, uw, uh in panels:
        px, py = cpt(ux - uw / 2, uy + uh / 2)
        pw, ph = cpx(uw), cpx(uh)
        co.append(img(K + f'cpanel/{pw}/{ph}/{kind}', int(round(px)) - 24, int(round(py)), pw + 24, ph + 24))
    slot, icon = cpx(74), cpx(64)
    cap = 24  # count font size: the native digits stand about a quarter of the slot tall (capture 06)

    def cell(kind, ux, uy, src, shown, count=None, count_shown=None, check=None):
        x, y = cpt(ux, uy)
        x, y = int(round(x)), int(round(y))
        out = [img(K + f'cslot/{slot}/{kind}', x - slot // 2, y - slot // 2, slot, slot),
               img(src, x - icon // 2, y - icon // 2, icon, icon, need_bind=shown),
               img(K + 'sil/' + src[len(K):], x - icon // 2 + 2, y - icon // 2 + 2, icon, icon,
                   need_bind=shown + '.no')]
        if count:
            # right / bottom edges of the native 40-unit number box at (+20, -20)
            bw, bh = 56, 50  # cnum/24 box (two digits wide + slack), measured with dsmod-sshd-art-test
            out.append(img(K + f'cnum/{cap}/0', x + cpx(38) - bw, y + cpx(38) - bh, bw, bh,
                           bind=count, src_format=K + f'cnum/{cap}/%d', need_bind=count_shown))
        if check:
            cw, ch = cpx(28), cpx(20)
            cx, cy = x + cpx(23), y + cpx(26)
            out.append(img(K + f'ccheck/{cw}/{ch}', cx - cw // 2, cy - ch // 2, cw, ch, need_bind=check))
        return out
    bug_x, bug_y = (-303, -225, -149, -71), (108, 32, -44)
    sz_x, sz_y = (71, 149, 225, 303), (102, 25, -49, -126)
    for prefix, cells, grid, items, icons, kind in (
            ('B', BUGS, (bug_x, bug_y), BUG_ITEMS, [K + f'sq/MiniGameBugs/tr_bugIcon_{k:02d}' for k in range(12)], 0),
            ('T', TREAS, (sz_x, sz_y), list(range(161, 177)), [K + 'sq/DoButton/' + n for n in TREAS_ICONS], 2)):
        for k, (w, hi) in enumerate(cells):
            nm = f'coll.{prefix}{k}'
            have = sflag(0x13, 7) if items[k] == 143 else iflag(items[k])  # Deku Hornet: story flag 122
            manifest['derived'] += [
                {'name': nm, 'expr': f'floor(coll.w{w:x} / {128 if hi else 1}) % 128'},
                {'name': nm + '.on', 'expr': have},
                {'name': nm + '.on.no', 'expr': f'!({have})'}]
            co += cell(kind, grid[0][k % 4], grid[1][k // 4], icons[k], nm + '.on', nm, nm + '.on')
    # quest items: Cawlin's Letter, Beedle's Insect Cage, Rattle, Gratitude Crystals
    quests = [('uk_kobunALetter_00', f'{sflag(0x3F, 1)} || {sflag(0x3F, 4)}', iflag(158)),
              ('uk_terryCage_00', sflag(0x38, 1), sflag(0x38, 0)),
              ('uk_garagara_00', sflag(0x34, 5), iflag(160)),
              ('uk_genki_00', f'sshd.crystals.valid && sshd.crystals > 0 && {sflag(0x29, 6)}',
               'sshd.crystals.valid && sshd.crystals > 0')]
    for k, (art, given, have) in enumerate(quests):
        nm = f'coll.Q{k}'
        manifest['derived'] += [
            {'name': nm + '.on', 'expr': f'({given}) || ({have})'},
            {'name': nm + '.on.no', 'expr': f'!(({given}) || ({have}))'},
            {'name': nm + '.done', 'expr': given}]
        crystal = art == 'uk_genki_00'
        if crystal:
            manifest['derived'].append({'name': nm + '.n', 'expr': f'{nm}.on && !({given})'})
        co += cell(1, (-304, -226, -148, -70)[k], -135, K + 'sq/MenuPause/' + art, nm + '.on',
                   'sshd.crystals' if crystal else None, nm + '.n' if crystal else None, nm + '.done')
    co += back_footer()
    manifest['pages'].append({'id': 'coll', 'title': 'Collection', 'widgets': co})

    # --- Settings (Bag, "Options" tab): dungeon-layout spoiler toggle, the native Simple HUD, and the
    # top-screen D-pad / rupee copies (Off = hidden on top, the default; On = the native copy again)
    se = [bg] + bag_tabs('Options')
    se.append(img(K + f'plank/corner/{W - 120}/110', 60, 200, W - 104, 126))
    se.append(t('plank', 26, 'Dungeon layout before the Map', 60 + 40 + 310, 200 + 55 - 27))  # off = visited rooms only
    se.append(img(K + 'plank/foot/200/80', W - 300, 215, 216, 96))
    se.append(t('plank', 26, 'Shown', W - 300 + 100, 215 + 40 - 27, need_bind='dmap.on'))
    se.append(t('plank', 26, 'Hidden', W - 300 + 100, 215 + 40 - 27, need_bind='dmap.off'))
    se.append({'type': 'rect', 'rect': [60, 200, W - 120, 110], 'color': '#00000000', 'frame': 0,
               'on_tap': 'dmap_toggle', 'haptic': 'light'})
    # top-screen HUD: the game's own Options > Screen Display "Simple" (module ApplyHud; restored on Off
    # and when the companion unloads). All-or-nothing: the game offers no per-element switch.
    se.append(img(K + f'plank/corner/{W - 120}/110', 60, 350, W - 104, 126))
    se.append(t('plank', 26, 'Simple top-screen HUD', 60 + 40 + 230, 350 + 55 - 27))
    se.append(img(K + 'plank/foot/200/80', W - 300, 365, 216, 96))
    se.append(t('plank', 26, 'Off', W - 300 + 100, 365 + 40 - 27, need_bind='hud.on'))
    se.append(t('plank', 26, 'On', W - 300 + 100, 365 + 40 - 27, need_bind='hud.off'))
    se.append({'type': 'rect', 'rect': [60, 350, W - 120, 110], 'color': '#00000000', 'frame': 0,
               'on_tap': 'hud_toggle', 'haptic': 'light'})
    for k, (act, flag, label) in enumerate(TOP_HIDE):
        y = 500 + 150 * k
        se.append(img(K + f'plank/corner/{W - 120}/110', 60, y, W - 104, 126))
        se.append(t('plank', 26, label, 100 + TW[('plank', 26, label)] // 2, y + 55 - 27))
        se.append(img(K + 'plank/foot/200/80', W - 300, y + 15, 216, 96))
        se.append(t('plank', 26, 'Off', W - 300 + 100, y + 15 + 40 - 27, need_bind=flag + '.off'))
        se.append(t('plank', 26, 'On', W - 300 + 100, y + 15 + 40 - 27, need_bind=flag + '.on'))
        se.append({'type': 'rect', 'rect': [60, y, W - 120, 110], 'color': '#00000000', 'frame': 0,
                   'on_tap': act, 'haptic': 'light'})
    se += back_footer()
    manifest['pages'].append({'id': 'settings', 'title': 'Settings', 'widgets': se})


def main():
    manifest, data = build()
    d = os.path.join(PKG, 'dualscreen')
    os.makedirs(d, exist_ok=True)
    mpath = os.path.join(d, 'manifest.json')
    # keep the module block the packager stamped (hashes) if present
    if os.path.exists(mpath):
        old = json.load(open(mpath))
        if old.get('module', {}).get('libraries'):
            manifest['module'] = old['module']
    with open(mpath, 'w') as f:
        json.dump(manifest, f, indent=1)
        f.write('\n')
    with open(os.path.join(d, BUILD[:16] + '.json'), 'w') as f:
        json.dump(data, f, indent=1)
        f.write('\n')
    print('wrote', mpath)


if __name__ == '__main__':
    main()
