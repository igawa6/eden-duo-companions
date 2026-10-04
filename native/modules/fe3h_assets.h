// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem: Three Houses (010055D009F78000, 1.2.0): the asset-free art side of the module.
// Every image is decoded at runtime from the player's own romfs (fe3h_romfs + fe3h_g1t); no
// pixel or string of the game ships. Called from load_image on the runtime's asset worker.
//
// Image keys (after an optional "module:" prefix):
//   fe3h:portrait/<pid>            roster portrait: LINKDATA 6108 (KT-gz G1T, 128 x 256x256 BC3),
//                                  texture index = person id (pid < 128).
//   fe3h:face/<pid>/<expr>[/<part>]
//                                  dialogue portrait, 512x512 BC3, from LINKDATA 6123 (ptbl of
//                                  1043 KT-gz G1Ts). The index is the game's own table chain
//                                  (code 0x3E6980 -> 0x6A0EF0 -> 0x5EAF40, see FaceIndex):
//                                    asset   = fixed_persondata (id 12) s0[pid] +0x18 (u16)
//                                    row     = s1[asset] +0x08 (Part I) / +0x04 (Part II; <0 ->
//                                              the Part I value), s16 rows = int16[11] + u8 flag
//                                    face    = s16[row][FaceSlot(expr)] (slot 0 when the row's
//                                              flag +0x16 is 0 or the slot is empty)
//                                  <expr> is the game's expression argument 0..13 (0 = default);
//                                  <part> 1 (default) or 2 = Part II (save byte S+0x2449F == 1,
//                                  the game's own test in 0x3E6820). Not modelled: the
//                                  per-person overrides inside 0x3E6980 (battle outfits/classes,
//                                  Byleth/Jeralt/Dimitri special cases) and faces >= 0x413 (the
//                                  Cindered Shadows face file) -> false.
//   fe3h:facerow/<row>/<expr>      the same from a s16 row directly (0..599).
//   fe3h:faceidx/<n>               6123 child n raw.
//   fe3h:bmap/<stage>[/<flag>]     battle overview minimap: LINKDATA 6122 (nx/ui/still/minimap.bin,
//                                  ptbl of 48 KT-gz G1Ts, 1024x1024 BC3 or 1024x512 / 512x1024).
//                                  index = MinimapIndex(stage, flag): the game's rule in
//                                  0x10A140 / 0x10A730 (loader 0x69BC30 opens id 0x17EA, 0x69BF40
//                                  bounds-checks and stores the index), with
//                                    stage = u32 at scene descriptor SC+4 (static main+0x1B47128)
//                                    flag  = u8 at main+0x1AFA7DC (minimap UI object at
//                                            main+0x1ACA298, +0x30544; set through
//                                            0x10A140, which the map-change script commands
//                                            0x596F40..0x598750 call; live check pending)
//                                  Tile (tx, ty) of the stage covers pixels
//                                  [72 + 24*tx, 72 + 24*(tx+1)) (MinimapTileOrigin/Px; checked
//                                  against every stage's OFNI width/height).
//   fe3h:abbey/<n>                 monastery minimap: LINKDATA 6074 (patch4 401_abbey_minimap,
//                                  6 BC3 textures: 0/1 Garreg Mach 1024^2, 2/3 512^2, 4 Abyss, 5).
//   fe3h:unitface/<pid>/<cls>/<g>/<slot>[/<expr>[/<part>]]
//                                  a battle unit's dialogue portrait by the game's full rule
//                                  0x3E6980 (UnitFaceRow): generic soldiers get their CLASS's
//                                  face row (classdata s0 +8 male / +0xA female) plus a per-unit
//                                  variant (0x46230: male slot % 8, female slot % 4, ...).
//                                  pid = unit+0x24, cls = unit+0x4B, g = unit+0x104 (raw byte;
//                                  > 2 = use the person's gender persondata +0x26), slot =
//                                  unit+0xF0. Named characters come out as fe3h:face would.
//                                  Unflipped: the native forecast shows the enemy side mirrored.
//   fe3h:font/latin                the game's Latin font atlas (LINKDATA 72, 4096x512, white +
//                                  coverage alpha): the manifest's font_atlas (fe3h_font.h).
//   fe3h:fontglyph/<index>         one 48x52 cell of that atlas by UTF8TBL glyph index
//                                  (0..509); 438..476 are the pad-button glyphs (FontGlyph*).
//   fe3h:ui/<id>, fe3h:ui/<entry>/<id>
//                                  one sprite of the common UI atlas (LINKDATA 6026..6037, one per
//                                  language, default 6027 = ENG_U), cropped by the game's own
//                                  rect table: main+0xD52D64 (getter 0x6A84E0, GOT 0x1AB0B78),
//                                  725 x {u16 tex, id, x, y, w, h, texW, texH}, read from guest
//                                  memory (needs the host's read_memory). Id meanings: UiSprite*.
//   fe3h:g1tcrop/<link>/<tex>/<x>/<y>/<w>/<h>
//                                  a pixel rect of texture <tex> of a UI G1T (LINKDATA 6026..6145
//                                  only), checked against the texture size.
//   fe3h:ui2d/<arc>/<texture>      a texture of a LayoutKit archive (LINKDATA 30776..30827, 31527:
//                                  SARC + __Combined.bntx, fe3h_ui2d.h), by its BNTX name with or
//                                  without the "^q"/"^t" suffix, e.g. fe3h:ui2d/30799/
//                                  btl_win_name_blue_l.
//   fe3h:still/<pid>/<cls>/<g>/<slot>[/<1|2>]
//                                  the unit's forecast still (256^2), the game's rule in 0x144140:
//                                  face row (UnitFaceRow) < 0x64 -> roster sheet 6108 texture row
//                                  (atlas id 0xF6F3 + row, 0x5D9830); generic rows 0x64..0x225 ->
//                                  LINKDATA 6126 child row - 0x64, 0x233..0x255 -> 31523 child
//                                  row - 0x233 (0x6A1660, loader types 4 / 0x15 in 0x69F128);
//                                  named DLC rows 0x226..0x232 -> sprite 0x796C + row (0x5D5AC0;
//                                  group 9 = LINKDATA 31503 textures 5..17). Anna (0x416): the
//                                  game picks row 0x227 (DLC unit) or 0x30 (merchant, 6108 #48)
//                                  by 0x3E6500 (save flag [GOT 0x1AA7930]+0x64 bit 6 and her
//                                  record byte +0xB0 == 0x10; no save -> 0x227); the key gives
//                                  0x227 -- use portrait/48 for the merchant form. Constance
//                                  (0x412) and pid 3 variants (0x3E6980 flag paths) are not
//                                  modelled: their base row is used.
//   fe3h:dport/<army>/<face key>, fe3h:dport_s/<army>/<face key>
//                                  the forecast's portrait diamond (208^2 / 88^2) composed from
//                                  the btl_confirm layout (Compose in fe3h_assets.cpp); army 0
//                                  player blue, 1 enemy red, 2 ally green, 3 other yellow (small:
//                                  0 / 1 only); <face key> = a still/portrait/face/faceidx/
//                                  facerow/unitface key without "fe3h:".
//   fe3h:sprite/<id>[/<lang>]      a sprite of the game's atlas manager by its GLOBAL id (the ids the
//                                  code passes to 0x5C35A0 / 0x5C3790): 0..0x2D5 common UI (table
//                                  main+0xD52D64, LINKDATA 6026 + lang), 0x5B8..0x6DE battle UI
//                                  (main+0xD588D4, 6050 + lang), 0x6DF..0x87E monastery UI
//                                  (main+0xD5F6C4, 6062 + lang), 0xF5E.. abbey map (0xD67EB4,
//                                  6074), 0x1EC9.. conversation UI (0xD77564, 6086: one sprite per
//                                  whole texture), 0x3DA7.. headers / vines (0xD96344, 6097),
//                                  0x7B54.. Abyss UI (0x11991C4, 31503), 0xF6F3.. roster / crests
//                                  (0x1199674, 6108), 0x1EEDD..0x1F4A1 map-unit sprites
//                                  (0x1291514, 6109); lang default 1 = ENG_U,
//                                  ignored by the one-file groups. Rect tables read from guest
//                                  memory, pinned by getter + GOT + the 0x5C35A0 dispatch words.
//   fe3h:bg/..., blur/..., win/..., patch/..., mirror/..., crop/..., num/...
//                                  the v5 redesign's composed keys (screen background, softened
//                                  boards, the purple map window, tab-group patches, the rope-less
//                                  Monastery board, crops, digit runs), computed with the mockups'
//                                  Pillow arithmetic: see LibraryImpl::Styled in fe3h_assets.cpp.
// Images are straight RGBA8; whole mip-0 textures except the cropped / composed kinds above.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace fe3h_assets {

inline constexpr uint32_t PortraitEntry = 6108;
inline constexpr uint32_t FaceEntry = 6123;
inline constexpr uint32_t MinimapEntry = 6122;
inline constexpr uint32_t AbbeyEntry = 6074;
inline constexpr uint32_t PersonDataEntry = 12;

inline constexpr uint32_t MinimapTileOrigin = 72; // px of tile 0's left / top edge
inline constexpr uint32_t MinimapTilePx = 24;

class LibraryImpl;
// Thread-safe; owns the romfs index + caches (images: LRU within image_budget bytes).
class Library {
public:
    explicit Library(size_t image_budget = size_t(48) << 20);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    LibraryImpl& Impl() {
        return *impl_;
    }

private:
    std::unique_ptr<LibraryImpl> impl_;
};
std::unique_ptr<Library> CreateLibrary(size_t image_budget = size_t(48) << 20);

// key without the "module:" prefix, e.g. "fe3h:portrait/5". Called on the asset worker thread.
bool LoadImage(Library&, const EdenDsmodHostApi* host, const char* key, std::vector<uint8_t>& rgba,
               uint32_t& w, uint32_t& h);

// ---- the game's index rules (pure; exposed for the reader and the tests) ---------------------

// Minimap (6122) index for a stage and the map-change flag; -1 when the game shows none
// (stages 0x2A..0x3B map to index 0 in code, a placeholder: reported as none) or it is out of
// range of the 48-entry file.
int MinimapIndex(uint32_t stage, bool flag);
// Expression argument -> s16 column (0x6A0EF0): -1/0xFF/>=14 -> 0, 0..5 -> same, 6 -> 4,
// 7..11 -> expr-1, 12 -> 3, 13 -> 2.
uint32_t FaceSlot(int32_t expr);
// 6123 child index for a person (the table chain above); false when not derivable.
bool FaceIndex(Library&, const EdenDsmodHostApi* host, uint32_t pid, int32_t expr, bool part2,
               uint32_t& index);

// ---- generic unit portraits (0x3E6980) ------------------------------------------------------
struct UnitFaceInput {
    uint32_t pid{};          // unit+0x24 (person id)
    uint32_t cls{};          // unit+0x4B (current class)
    uint32_t gender_byte{3}; // unit+0x104 raw (<= 2 overrides persondata +0x26)
    int32_t slot{-1};        // unit+0xF0 (unit list slot); -1 = no unit (no class override)
    bool part2{};            // save byte S+0x2449F == 1 (the game's 0x3E6820 test)
    bool in_battle{true};    // G+0x3D51 (the override applies on the battle map only)
    int32_t battle_map{-1};  // scene SC+8 (u16); 0x24 turns row 0x24 into 0x39
};
// s16 face row (0..599) for a unit; false when the rule gives none.
bool UnitFaceRow(Library&, const EdenDsmodHostApi* host, const UnitFaceInput& in, uint32_t& row);
// 6123 child index for a unit and an expression (0x6A0EF0 on UnitFaceRow).
bool UnitFaceIndex(Library&, const EdenDsmodHostApi* host, const UnitFaceInput& in, int32_t expr,
                   uint32_t& index);

// ---- common UI atlas sprites (fe3h:ui/...) --------------------------------------------------
inline constexpr uint32_t UiAtlasFirst = 6026, UiAtlasLast = 6037, UiAtlasEngU = 6027;
inline constexpr uint32_t UiRangeFirst = 6026, UiRangeLast = 6145; // fe3h:g1tcrop / uitex
// Forecast name plates (btl_confirm, arc 30799): window panes p_name_<side>_l, 432 x 50, frame
// size (200, 200, 50, 50): left cap <c>_l 200 x 50, stretched centre <c>_c 32 x 50, right cap
// <c>_r 200 x 50, untinted; <c> = btl_win_name_blue (player) / _red (enemy) / _ally / _third.
inline constexpr uint32_t NamePlateArc = 30799, NamePlateCapW = 200, NamePlateMidW = 32,
                          NamePlateH = 50;
inline constexpr const char* NamePlateColour[4] = {"blue", "red", "ally", "third"};
std::string StillKey(const UnitFaceInput& in);

// ---- pause-map / field-map sprites (global atlas ids, fe3h:sprite/<id>) ------------------------
// People markers (0x2B6F90 / 0x2B7230, per actor): roster member (S person +0xAC bit0) 0x7B7 blue
// dot, other named 0x7B8 gold dot, generic NPC with an event 0x7B9 white dot, other generic 0x7BA
// (drawn at 0.25 scale); facility NPCs FacilityMarkerSprite(cid). All 20 x 20 except noted.
inline constexpr uint32_t SpriteRosterDot = 0x7B7, SpriteNamedDot = 0x7B8, SpriteEventNpcDot = 0x7B9,
                          SpriteNpcDot = 0x7BA;
// Purple chevron (80 x 80, drawn 45 x 45 by 0x2B3EF0 over the persons listed at map object
// +0x7108, count +0x7104) and the player's 4-point star (28 x 40, rotated by the yaw: pause map
// 0x2B6760, and the same sprite in 0x3108A0 at 0.25 scale).
inline constexpr uint32_t SpriteMapChevron = 0x7B5, SpritePlayerStar = 0x7BD;
// Pause-map board (1134 x 1004, 6063 tex 6: purple paper + gold rope frame), set in 0x2B4288.
inline constexpr uint32_t SpriteMapBoard = 0x855;
// Board placement (0x2B41C0, 1080p): pos (46, 36), size 1133 x 1003 (Abyss map 0x37: sprite
// 0x7B8C, y 56). Floor labels (0x2B5330, not drawn on map 0x37; no Part I / II test): language
// 1 / 2 (ENG): big numeral sprite 0x748 + floor (0x749 "1" .. 0x74B "3", 52 x 80, drawn in a
// 42 x 66 rect by 0x5D7A60) + text GetText(0x442) = ENG_U msg part 1 #1090 "F" at size 44;
// languages 3..8: text 0x6DB..0x6DD ("1F".."3F", part 1 #1755..1757) at size 44 instead. Both
// tinted by colour index 0xBC (runtime table main+0x1B11CC0, u32 per index). 1080p positions:
inline constexpr float MapBoardPos[2] = {46.0f, 36.0f}, MapBoardSize[2] = {1133.0f, 1003.0f};
inline constexpr uint32_t SpriteFloorDigit0 = 0x748; // + floor 1..3
inline constexpr float FloorNumeralRect[3][4] = {
    {110, 161, 42, 66}, {1008, 344, 42, 66}, {1008, 854, 42, 66}};
inline constexpr float FloorLetterPos[3][2] = {{151, 180}, {1049, 363}, {1049, 873}}; // "F", 44
inline constexpr float FloorTextPos[3][2] = {{151, 180}, {1005, 363}, {1005, 873}};   // lang 3..8
// Activity-point gauge (HUD element ctor 0x322660, draw 0x322820; mode +0x54: 0 explore M+0x27,
// 1 teach M+0x28, 2 battle M+0x29, 3 generic). 1080p: origin O = (750, 25), frame 400 x 128 at O
// (explore 0x719 / teach 0x71C / battle 0x71A / generic 0x6F2: rail, diamond, circle, icon); bar
// track = sprite 0x726 stretched to O + (37, 14) 351 x 8, vertex colours top 0xFF393939, bottom
// 0xFF303030; fill = 0x726 over the same origin, width 351 * value / max, horizontal gradient
// GaugeFill[mode] (RGBA); value = font text "%d" (font class 0x20, cell 20.46 x 28, centred at
// O + (212, 50)) in colour GaugeText[mode] with outline GaugeOutline[mode]; (while animating: fill
// 0x727, glow 0x71B 420 x 152 at O + (-10, -12)).
inline constexpr uint32_t SpriteGaugeFrame[4] = {0x719, 0x71C, 0x71A, 0x6F2};
inline constexpr uint32_t SpriteGaugeBar = 0x726, SpriteGaugeBarAnim = 0x727, SpriteGaugeGlow = 0x71B;
inline constexpr float GaugeOrigin[2] = {750, 25}, GaugeBarRect[4] = {37, 14, 351, 8},
                       GaugeTextPos[2] = {212, 50};
inline constexpr uint32_t GaugeFill[4][2] = {// left, right RGBA
                                             {0x6825FFFF, 0x9173FFFF},
                                             {0x008C24FF, 0x18CE13FF},
                                             {0xCB2215FF, 0xEB6125FF},
                                             {0xFFCC00FF, 0xFFF073FF}};
inline constexpr uint32_t GaugeText[4] = {0xFBF2FFFF, 0xF5FFF2FF, 0xFFF2F2FF, 0xFFFEF2FF};
inline constexpr uint32_t GaugeOutline[4] = {0xBEA8FFFF, 0xA8FFAAFF, 0xFFAEA8FF, 0xFFE5A8FF};
// Free-day battle list row (0x63C730): type icon 0x79C (56 x 44) at R + (16, 6); battle-point cost
// badge 0x7F2 (128 x 80) drawn 116 x 72 at R + (649, -9); cost "%d" centred at R + (721, 13) font 2
// in 0xFFF2F2 / outline 0xFFAEA8, or 0xFF6200 without outline when the points are short.
inline constexpr uint32_t SpriteBattleTypeIcon = 0x79C, SpriteBattleCostBadge = 0x7F2;
// Composed gauge without its number: fe3h:gauge/<mode>/<value>/<max> (400 x 128).
std::string GaugeKey(uint32_t mode, uint32_t value, uint32_t max);
// Facility-NPC marker (0x5D9290(cid, 0x79E)); ids outside the supported slots return -1.
int FacilityMarkerSprite(uint32_t cid);
// Visual candidates only (NOT traced to the talk / Log code): 0x1F (31) purple arrow like the
// dialogue "next" mark, 0x26 (38) quill like the Log cursor.
inline constexpr uint32_t SpriteTalkNext = 0x1F, SpriteLogQuill = 0x26;
std::string SpriteKey(uint32_t id);
// Conversation UI (group 6 = LINKDATA 6086, sprite 0x1EC9 + texture #; NPC talk window T+0x1A28,
// draw 0x478C00): speaker diamond + name tab 572 x 504, purple 0x1ED7 / gold 0x1ED8 (by T+0x708),
// portrait mask 0x1ED9 (512 x 512); message paper 0x1ED6 (744 x 252), 0x1EDA / 0x1EDB (520 x 252);
// auto / fast / pause buttons 0x1ECD..0x1ECF (64 x 64); name plates 0x1ED1..0x1ED3 (668 x 120),
// 0x1EDC / 0x1EDD (576 x 88). Group 7 = 6097 headers / vine frames 0x3DA7..0x3DAC (Log: lead).
inline constexpr uint32_t SpriteTalkTabPurple = 0x1ED7, SpriteTalkTabGold = 0x1ED8,
                          SpriteTalkMask = 0x1ED9, SpriteTalkPaper = 0x1ED6;
// People "Occupants" panel (0x5C4EE0): paper 0x279 (1024^2 parchment) drawn at rect inset 4 px,
// masked by a 9-slice torn-edge sprite through 0x5C3950 with {left, mid, right, top, mid, bottom}
// texel slices = the sprite size: panel height >= 340 -> 0x259 (340 x 340: 160 | 20 | 160 both
// ways), else 0x25F (232 x 120: 108 | 16 | 108 across, 50 | 20 | 50 down); Abyss variants 0x7B54
// / 0x7B65; header underline 0x260 (460 x 40).
// Unit-window portrait (fe3h:sport[_s]/<army|grey>/<face>) and oval roster portrait
// (fe3h:oport/<face>); see LibraryImpl::Portrait. army < 0 -> "grey".
std::string SportKey(int army, const std::string& face_key, bool small = false);
std::string OportKey(const std::string& face_key);
// Adjutant inset of the unit Details portrait (0x135780 -> 0x5C8F40): fe3h:adjport/<face key>,
// 128 x 128. Frame = group-3 local 0x82 (global 0x63A, 128 x 128) 1:1 at (0, 0); the face (still,
// via 0x3E6980 row -> 0x5D5AC0) drawn 144 x 144 at (-7, -11), its alpha x the diamond mask local
// 0x83 (global 0x63B, 128 x 128, written to the stencil by the game) at (0, 0). Placement: canvas
// origin = portrait origin + (1, 136) (256 px portrait; the 200 px variant uses (136, 135)); the
// adjutant icon 0x5D9A90(type) (0x5C0 / 0x5C1 / 0x5C2, 96 x 96) is drawn 30 x 30 at canvas +
// (49, 90).
inline constexpr uint32_t SpriteAdjFrame = 0x63A, SpriteAdjMask = 0x63B;
std::string AdjportKey(const std::string& face_key);
// Battle list tags (group 3, 6051 tex 3, 284 x 84): 0x62A cream (normal item, 0x13D070), 0x62C
// the alternative when 0x5D9800(id) is true, 0x62B purple (cursor / selected, 0x13F3A0).
inline constexpr uint32_t SpriteTagNormal = 0x62A, SpriteTagSelected = 0x62B,
                          SpriteTagAlt = 0x62C;
// Composed: fe3h:panel/<w>/<h>, fe3h:panel_abyss/<w>/<h>.
inline constexpr uint32_t SpritePanelPaper = 0x279, SpritePanelMaskBig = 0x259,
                          SpritePanelMaskSmall = 0x25F, SpritePanelHeaderLine = 0x260;
// Composed (see LibraryImpl::Panel / Nine): fe3h:titlewin/<w>/<h> (titled window 0x5CB1C0 ->
// 0x5C4B50: paper x mask 0x255 + cog 0x79), fe3h:paperwin/<mask>/<w>/<h> (0x5CAF10 -> 0x5C4600,
// e.g. the unit Details profile paper, mask 0x253), fe3h:nine/<id>/<w>/<h>/<l>/<m>/<r>[/<t>/<m>/<b>]
// (the game's 9-slice / 3-slice of any sprite).
inline constexpr uint32_t SpriteTitleWinMask = 0x255, SpriteTitleWinCog = 0x79,
                          SpriteProfileMask = 0x253;
// Battalion gambit area grid (Details page 3/3, 0x5D3CD0(g, 1) at panel + (56, 268)):
// fe3h:gbrange/<g> (g = battalion record +0x27, 0..79), 531 x 147 (see LibraryImpl::GambitRange).
// Pieces (group 3): board 0x696, area cell 0x690, user marker 0x69C blue / 0x69D red, arrow 0x697.
inline constexpr uint32_t SpriteGambitGrid = 0x696, SpriteGambitCell = 0x690,
                          SpriteGambitUserBlue = 0x69C, SpriteGambitUserRed = 0x69D,
                          SpriteGambitArrow = 0x697;
inline constexpr uint32_t GambitGridW = 531, GambitGridH = 147;
std::string GambitRangeKey(uint32_t g);
// Phase banner (0x13C150 -> ui2d layout 30804 btl_phase, pane p_btl_txt_phase 1024 x 176 centred
// on 1920 x 1080): sprite 0x67D + phase (0 player blue, 1 enemy red, 2 ally green, 3 third army
// gold), 6051 tex 9 rects y = 0 / 180 / 360 / 540. IN: scale 1.75 -> 1.0 over frames 0..35, alpha
// 2 until 25 then 255 at 35; OUT: scale 1.0 -> 1.5 over 10 frames fading to 0.
inline constexpr uint32_t SpritePhaseBanner = 0x67D;
// Battle overview / Prep "Conditions" panel (0x138930; layout table 0x1AFAEF0; 1080p):
//   title plate 0x611 (768 x 152) at (0, 24); title centred x 384, y 82, glyph 38, max 26, 0x30
//     outline 0x31 w3;
//   TURN diamond 0x616 (208 x 208) at (728, 0) when turn != 0; "TURN" (msg 664) centred 832 y 53
//     glyph 28; number (0x5C80E0) centred 827 y 96 glyph 30 x 40; 0x30 / 0x31 w3;
//   Victory header 0x61E (384 x 72) at (282, 176), Defeat 0x61F at (282, 380); label (msg 598 /
//     599) centred 474, y 193 / 397, glyph 36, gradient 0x56 FFF5B8 -> 0x57 EACC68 outline 0x58
//     39301E (defeat 0x59 E3E5E7 -> 0x5A 818385, outline 0x5B 1A1A1A), w3;
//   paper 0x61D (852 x 140) at (49, 232) / (49, 436); condition text centred 474, glyph 30, line
//     26, colour 0x2E, top y 287 / 491 (one line) or 259 / 463 (more);
//   unit plates (0x139390) in slots (103, 599), (545, 599), (103, 699), (545, 699) for armies with
//     count > 0: 3-slice 0x610 at (x - 67, y + 7) 436 x 72 (fe3h:nine/1552/436/72/144/8/144),
//     house icon {0x295, 0x296, 0x298, 0x297}[army] 70 x 84 at (x, y), label (msg 880 + army:
//     Your / Enemy / Ally / Third Army) left (x + 75, y + 28) glyph 30 max 12, count right-aligned
//     x + 293, y + 28 glyph 22 x 30; 0x30 / 0x31 w3;
//   Untapped box 0x64E (182 x 108) at (745, 864) (or (943, 47) in the other mode); "Untapped
//     Units:" (msg 363) centred 836 y 876 glyph 28 max 6 (squeezed), colour 5 009E73; count
//     centred 836 y 922 glyph 21.92 x 30, colour 0x2E.
// Text: 0x5C5E40 w1 = max width in glyphs (squeezes), w3 align 0 / 1 / 2, w5 = gradient bottom.
inline constexpr uint32_t SpriteOverviewTitle = 0x611, SpriteOverviewTurn = 0x616,
                          SpriteOverviewVictory = 0x61E, SpriteOverviewDefeat = 0x61F,
                          SpriteOverviewPaper = 0x61D, SpriteOverviewUnitPlate = 0x610,
                          SpriteOverviewUntapped = 0x64E;
inline constexpr uint32_t OverviewArmyIcon[4] = {0x295, 0x296, 0x298, 0x297};
// Class icon (the small map-unit sprite beside the class name; 0x5CF7E0 -> consumer 0x6A2240):
// fe3h:clsicon/<cls>/<gender>/<face>/<part2>/<army>/<pid>[/<route_alt>], 32 x 32 (drawn 64 x 64
// point-sampled at 1080p). cls = unit+0x4B; gender = map unit +0x104 (or persondata +0x26); face =
// persondata +0x18 (asset); part2 = save +0x2449F == 1; army = map unit +0xF1 (0 off-map; 4 = no
// icon); pid = unit+0x24; route_alt = save +0x2449E == 3 && part2. Group-11 sprites of LINKDATA
// 6109 only (DLC art of groups 12 / 13 fails closed).
std::string ClassIconKey(uint32_t cls, uint32_t gender, uint32_t face, bool part2, uint32_t army,
                         uint32_t pid, bool route_alt = false);
// Battalion endurance icon (0x63E560 from 0x113940): endurance 0 -> 0x5FB, else 0x5F6 + tier,
// tier = min((cur - 1) / (max / 3), 2) (0x4169B0); 128 x 128 drawn 48 x 46 at row + (378, 5).
inline constexpr uint32_t SpriteEnduranceEmpty = 0x5FB, SpriteEnduranceTier0 = 0x5F6;
// Page-indicator dots (Units list 0x6851A0): 0x275 inactive / 0x276 current, 48 x 48 drawn 1:1,
// pitch 28, centre x = 960 + 28 * (i - (n - 1) / 2), centre y = 173 (ZL / ZR glyph centre + 89).
inline constexpr uint32_t SpritePageDot = 0x275, SpritePageDotCurrent = 0x276;
std::string DportKey(uint32_t army, const std::string& face_key, bool small = false);
inline constexpr uint64_t UiRectTable = 0xD52D64, UiRectGot = 0x1AB0B78, UiRectGetter = 0x6A84E0;
inline constexpr uint32_t UiRectGetterWords[4] = {0x9000A048, 0xF945BD08, 0x8B21D100, 0xD65F03C0};
// Skill category k (0 sword, 1 lance, 2 axe, 3 bow, 4 brawl, 5 reason, 6 faith, 7 authority,
// 8 heavy armor, 9 riding, 10 flying) -> tex0 icon: u32[11] main+0xD52848 (fn 0x5CEE40), > 10 -> 65.
inline constexpr uint32_t UiSkillIcon[11] = {65, 66, 67, 68, 69, 71, 70, 72, 407, 408, 409};
// Framed variant (fn 0x5CEE60, u32[11] main+0xD5281C).
inline constexpr uint32_t UiSkillIconFramed[11] = {73, 74, 75, 76, 77, 79, 78, 80, 81, 82, 83};
// Weapon rank 0..11 (E .. S+) -> letter sprite (fn 0x5CF000: 0x182 + min(rank, 11)).
inline constexpr uint32_t UiRankIcon(uint32_t rank) {
    return 0x182 + (rank < 11 ? rank : 11);
}
// Crest 0..21 -> tex1 crest sprite (122 + crest; the item-icon path adds 0x7A to the crest id).
inline constexpr int UiCrestIcon(uint32_t crest) {
    return crest <= 21 ? int(122 + crest) : -1;
}
// Inline pad-button glyphs (text ESC 'P' <base-36 digit>, fn 0x3948F0, u32[28] main+0xCE70F0):
// digit i -> 669 + i for i 0..24 (A B X Y, dpad, dpad L/R, dpad U/D, left, right, up, down, L,
// R, ZL, ZR, L-press, R-press, +, -, L-stick, R-stick, capture, SL, SR, HOME ... see the report);
// 27 -> 694 (beige set = 694..716); 25 / 26 -> atlas ids 7885 / 7887 outside this atlas.
inline constexpr uint32_t UiButtonA = 669, UiButtonB = 670, UiButtonX = 671, UiButtonY = 672,
                          UiButtonL = 680, UiButtonR = 681, UiButtonZL = 682, UiButtonZR = 683,
                          UiButtonPlus = 686, UiButtonMinus = 687;
// Weapon item (ids 10..509: fixed_data s0[id - 10]) -> icon sprite, the game's rule in
// 0x5D29A0 / 0x5D2A20 for weapon types 0..11; -1 for other item ranges (not decoded yet).
// Any thread and never blocking (the reader calls it on the tick thread): the rows are loaded by
// the first LoadImage on the asset worker; before that WeaponIconPending.
inline constexpr int WeaponIconPending = -2;
int UiWeaponItemIcon(Library&, uint32_t item_id, uint32_t uses);
std::string UiKey(uint32_t id, uint32_t entry = UiAtlasEngU);

// ---- the game's font (fe3h_font.h) ------------------------------------------------------------
// Pad-button glyph cells in the font atlas (UTF8TBL has no code point for them).
inline constexpr uint32_t FontGlyphA = 438, FontGlyphB = 439, FontGlyphX = 440, FontGlyphY = 441,
                          FontGlyphL = 442, FontGlyphR = 443, FontGlyphZL = 444, FontGlyphZR = 445,
                          FontGlyphSL = 446, FontGlyphSR = 447, FontGlyphPlus = 452,
                          FontGlyphMinus = 453, FontGlyphHome = 456, FontGlyphLStick = 473,
                          FontGlyphRStick = 474;
// decode_font body: `bytes` = the manifest `font` asset ("FE3HFONT serif" / "FE3HFONT sans");
// reads UTF8TBL from romfs and the metrics from guest memory. Any thread.
struct FontOut {
    uint32_t line_height{}, first_codepoint{};
    std::vector<EdenDsmodFontGlyph> glyphs;
};
bool DecodeFont(Library&, const EdenDsmodHostApi* host, const uint8_t* bytes, size_t size,
                FontOut& out);
inline constexpr const char* FontAtlasKey = "module:fe3h:font/latin";

// Published image-source strings ("module:fe3h:...").
std::string PortraitKey(uint32_t pid);
std::string FaceKey(uint32_t pid, int32_t expr = 0, bool part2 = false);
std::string BmapKey(uint32_t stage, bool flag = false);

} // namespace fe3h_assets
