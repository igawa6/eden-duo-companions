// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Legend of Zelda: Skyward Sword HD (01002DA013484000) companion art, decoded at runtime from
// the player's own romfs. Skyward Sword HD keeps the Wii-era formats: U8 archives, GX TPL
// textures, RFNT fonts, LZ11-compressed stage archives and KCL collision meshes. None of them is
// understood by the host, so this module decodes them and composes the bottom-screen pieces
// (planks, sockets, the round minimap frame, styled labels and the per-area map) from them.
// Nothing here is shipped: every picture is built from the game's files when it is asked for.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sshd_plate.h"

namespace sshd {

/// Reads one whole romfs file (path without a prefix, e.g. "Layout/DoButton.arc").
using RomfsReader = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;

/// Straight-alpha RGBA, 0..1 floats.
struct Img {
    int w = 0, h = 0;
    std::vector<float> p;
    Img() = default;
    Img(int w_, int h_) : w(w_), h(h_), p(static_cast<size_t>(w_) * h_ * 4, 0.0f) {}
    float* at(int x, int y) { return &p[(static_cast<size_t>(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &p[(static_cast<size_t>(y) * w + x) * 4]; }
    bool empty() const { return w <= 0 || h <= 0; }
    std::vector<uint8_t> Rgba8() const;
};

struct Col {
    float r, g, b, a = 1.0f;
};
Col Hex(std::string_view s);

// Container formats.
/// `limit`: decode only that many leading bytes (an archive's prefix).
std::optional<std::vector<uint8_t>> Lz11(const std::vector<uint8_t>& in, size_t limit = SIZE_MAX);
struct U8Entry {
    std::string path;
    uint32_t off, size;
};
/// need_data = false also lists members past the end of `d` (a prefix holding only the node table).
std::vector<U8Entry> U8List(const std::vector<uint8_t>& d, bool need_data = true);
std::optional<std::vector<uint8_t>> U8Get(const std::vector<uint8_t>& d, std::string_view path);
std::optional<Img> TplDecode(const std::vector<uint8_t>& d);
std::optional<Img> GxDecode(const uint8_t* data, size_t avail, int w, int h, int fmt);

// Image operations (PIL equivalents used by the accepted mockups).
Img Resize(const Img& s, int w, int h);
Img Tint(const Img& s, Col blk, Col wht, float a = 1.0f);
Img AMask(const Img& s, Col c, float a = 1.0f);
Img Fade(const Img& s, float a);
Img FlipH(const Img& s);
Img FlipV(const Img& s);
Img Rot90(const Img& s); // counter-clockwise, like PIL rotate(90, expand=True)
Img Crop(const Img& s, int x, int y, int w, int h);
Img Quad(const Img& s);
void Over(Img& dst, const Img& src, int x, int y);
void OverCentered(Img& dst, const Img& src, float cx, float cy);
Img Fit(const Img& s, int size);
Img SizeW(const Img& s, int w); // keep aspect

/// A styled game-font label, as the game draws its two-pane text (shadow + gradient main).
struct Font;
/// A neighbouring-area plate's size (Art::PlaceMetrics): text width / height, band width / height.
struct PlaceSize {
    int tw = 0, th = 0, bw = 0, bh = 0;
};
class Art {
public:
    explicit Art(RomfsReader read) : read_(std::move(read)) {}
    std::optional<Img> Layout(std::string_view arc, std::string_view member);
    std::optional<Img> Text(std::string_view style, float cap, std::string_view text);
    std::optional<Img> Background(int x, int y, int w, int h);
    std::optional<Img> MinimapFrame(bool pressed = false);
    std::optional<Img> Plank(int w, int h, std::string_view variant);
    std::optional<Img> Socket(int s);
    std::optional<Img> Compass();
    std::optional<Img> Beacon(int s);
    std::optional<Img> Digit(int d, int h);
    // room_mask: bit n = draw rarc/<stage>_r<nn>.arc, including Sky Keep (r00 in each stage).
    // ~0 = every room. dev.32 native colours (map colour analysis): every
    // upward map-tagged KCL / KCL2 prism (and the stage's world-space actor collision, e.g.
    // Skyloft's lake) through its room-local PLC map class onto the shared Common map textures,
    // with the stage's gradation height bands and edge tint. full_mask: the rooms drawn in full
    // colour (dungeon: visited); the other drawn rooms take the native unvisited Map shade.
    // state bit 0 (kMapPast): the native past variant (Lanayru time-shift backgrounds).
    std::optional<Img> Map(std::string_view stage, uint32_t room_mask = 0xFFFFFFFFu, int floor = 0,
                           bool paper = true, uint32_t full_mask = 0xFFFFFFFFu, int state = 0);
    /// The native map class (0..30, or -1 = not on the map) of a PLC record's words (code0,
    /// code1, code3) as dBgS::GetMapCode; `trial` = a Silent Realm trial (target-through -> 30).
    static int MapClass(uint32_t code0, uint32_t code1, uint32_t code3, bool trial);
    /// dev.34: the extent (x0, x1, z0, z1) of a stage's drawn map faces (the generic frames' source).
    std::optional<std::array<float, 4>> MapBounds(std::string_view stage);
    /// Tests: keep the per-pixel map classes (0 = not drawn) of the next Map pictures.
    void KeepMapClasses(bool on) { keep_classes_ = on; }
    const std::vector<uint8_t>& LastMapClasses() const { return last_classes_; }
    /// Tests (dev.36): the next Map pictures' native edge line coverage per pixel (0..255; all 0 on
    /// fields and dungeons, which draw the dev.32 darkened rim instead).
    const std::vector<uint8_t>& LastMapEdges() const { return last_edges_; }
    /// Sky Keep (dev.31): the native 3 x 3 tile map. cells[i] (row-major from the top left) is the
    /// D003_<n> room in that cell or -1 for the empty one; each room is drawn in its tile at its own
    /// world coordinates (kSkyKeep*), Link's room only when `cur_only` (no Dungeon Map). dev.32:
    /// the tiles are the native map dummy (oarc/D3MapDummy.arc) in native colours.
    std::optional<Img> SkyKeep(const int (&cells)[9], int cur, bool cur_only);
    std::optional<Img> SkyMap();
    std::optional<std::string> ItemName(int id);
    /// A map name of 0-Common/007-MapText.msbt (e.g. "MAP_02" -> "Faron Woods").
    std::optional<std::string> MapText(std::string_view label);
    /// Map marker art (dev.27, map marker analysis), each on a square canvas
    /// whose centre is the marker's world anchor: the Sky's Goddess tag (MapSky P_goddessC_00), a
    /// bird statue (N_saveIcon_00 in type colour 0 green / 1 orange / 2 yellow; sky = MapSky's
    /// 10-unit dot), a door bar turned `deg` (N_door_000), a neighbouring-area plate placed outside
    /// its mark along screen direction `deg` (mapPlace_00 + MAP_xx text), the thunderhead
    /// (N_cloudBig_00, entered = cloudAlpha frame 1), a surface light column (N_areaLight_00 in
    /// region colour 0 Faron / 1 Eldin / 2 Lanayru) and a Sky island name right of its island.
    std::optional<Img> GoddessTag();
    std::optional<Img> Statue(int type, bool sky);
    std::optional<Img> Door(int deg);
    /// Dungeon map markers (dev.30, dungeon marker analysis): the small-key
    /// lock over a door bar (N_doorKey_00), the red X of a scripted mark (N_mark_00), a closed
    /// dungeon chest (N_treasure_000, drawn like "openchest"), the boss door's objective (kind
    /// 0 Zelda N_zelda_00, 1 / 2 / 3 the green / red / blue sacred flame N_swordFire_01 / _00 /
    /// _02) and a floor tab (Map2D mapFloorBtn_00) for native floor -3..4 (0 = 1F), sel = the lit
    /// tab. (No entrance arrow: its rotation is not calibrated, so it is not drawn.)
    std::optional<Img> DoorKey();
    std::optional<Img> Mark();
    /// dev.34: Sky Keep's Triforce piece (MapSky N_triforce_00), 26-unit canvas.
    std::optional<Img> Triforce();
    /// dev.33: the Sky class's red common target (MapSky N_commonIcon_000; big = the 15-unit
    /// Academy / Bazaar variant) and a Bug Medal bug location (N_mushi_01), 20-unit canvases.
    std::optional<Img> Target(int big);
    std::optional<Img> Bug();
    std::optional<Img> ClosedChest();
    std::optional<Img> Objective(int kind);
    std::optional<Img> FloorTab(int floor, int sel);
    /// dev.46 (owner pick: alt A): the native choice plank (System2D messageBtn_00 / MenuPause
    /// N_options_00, the same members as the floor tabs), a clamped 3-slice: 40-unit ends never
    /// stretched, the end's last column repeated across the middle; height 68 units x 1.25.
    /// state 0 normal, 1 selected (N_on lit), 2 pressed (lit rim + N_flash), 3 disabled (dimmed);
    /// opaque = the body at full alpha (on the light map paper).
    std::optional<Img> Choice(int w, int h, int state, bool opaque);
    /// dev.46 (owner pick: alt B for the top bars): the Options screen row (MenuOptions options_00
    /// tr_optionsBtn_00, 1240x120 at 1080p): its 64 px rounded end, the mirror, the dark name part's
    /// column repeated (cut, not stretched); built at 120 px and scaled to h. state 0 normal,
    /// 1 selected (the row's focus flash W_flash, tr_optionsBtnFlash_00), 2 pressed (stronger flash),
    /// 3 an unselected tab (the plain row at 62 %).
    std::optional<Img> OptRow(int w, int h, int state);
    /// dev.46: the refusal pill: the HUD do-prompt window (DoButton basicInfo_00 W_bgP_00 body
    /// #14052D opaque, W_bgP_01 gold frame), its corners as laid out, straight runs from the corners'
    /// last column.
    std::optional<Img> DoPill(int w, int h);
    /// dev.31 (NPC map mark analysis): an NPC speech balloon by its
    /// MapMark icon kind (Sky MapSky 17 Dodoh N_roulette_00, 19 Peater N_takegiri_00, 20 Strich
    /// N_kobunB_00, 23 Fledge N_sebasun_00; field MapField 18 Thrill Digger Mogma N_konsai_00,
    /// 21 Thunder Dragon N_rairyu_00, 22 Gortram N_troccoGoron_00): the shared frame
    /// nh_mapBaloon_01 (mirrored half, turned so its tail meets the mark) over its shadow, the
    /// portrait over its shadow, all at the panes' own offsets from the mark; 80-unit canvas whose
    /// centre is the mark.
    std::optional<Img> Balloon(int icon);
    /// A field landmark glyph (MapField N_cave_00: nh_cave_00 over nh_caveSdw_00 at the group's
    /// 0.66 scale and alpha 238), 20-unit canvas.
    std::optional<Img> Cave();
    /// Beedle's airshop (MapSky N_terryShop_00 at 0.9: P_house_00 nh_terry_01 over its shadow
    /// nh_terry_sdw_01); face = 1 adds the N_terryOnOff_00 portrait (nh_terry_00 over its shadow).
    /// 48-unit canvas centred on the shop.
    std::optional<Img> BeedleShop(int face);
    /// The entrance arrow (MapField N_start_000: P_start_00 tr_start_01 in green over the darker
    /// P_start_01, 8 units from the anchor, pointing at it at deg 0) turned `deg` clockwise about
    /// the anchor; 40-unit canvas.
    std::optional<Img> StartArrow(int deg);
    /// A Kikwi face (MapField N_morijin_00: nh_moriJin_00 20 units; elder = N_morijin_01:
    /// nh_moriJin_01 25 x 34 at 0.8), each over its shadow at the group's 0.9 scale; 40-unit canvas.
    std::optional<Img> Kikwi(int elder);
    std::optional<Img> Place(int label, int deg, int region);
    std::optional<Img> Thunderhead(int entered);
    std::optional<Img> Column(int region);
    std::optional<Img> IsleName(int idx, int gap);
    /// The plate's text and band size for a label (what Place draws, without drawing it): the
    /// module fits the plate on screen with it (sshd_plate.h).
    std::optional<PlaceSize> PlaceMetrics(int label);
    /// The box an island name covers, relative to its island (canvas px, y down).
    std::optional<Box> IsleNameBox(int idx, int gap);
    std::optional<Img> StaminaWheel(int pct, int s);
    /// Native D-pad cluster pieces (dpad/<piece>[/<arg>]), each at the texture's own resolution
    /// with the layout's material and vertex colours baked in (sshd_dpad.h names the panes).
    std::optional<Img> Dpad(std::string_view piece, int arg);
    /// Ready stamina ring around the minimap: one segment centred on 12 o'clock spanning
    /// +-half_mdeg / 1000 degrees between the radii (the manifest rotates copies about the map
    /// centre), and the full backing track. variant 0 = the gauge's green, 1 = its exhausted red.
    std::optional<Img> StaminaSegment(int rin, int rout, int half_mdeg, int variant);
    std::optional<Img> StaminaTrack(int rin, int rout, int variant = 0);
    std::optional<Img> LoftMarker();
    std::optional<Img> AltGauge(int h);
    std::optional<Img> Ribbon(int w, int h);
    /// The map's area-name plate (Map2D map_00 N_inOut_00) in a region colour (0..6, see
    /// kRegionCols), w x h px; the title text is a separate "plate" style label.
    std::optional<Img> AreaPlate(int w, int h, int region);
    /// The neighbouring-area plate (MapField mapPlace_00): a striped band in a region colour.
    std::optional<Img> PlacePlate(int w, int h, int region);
    /// Collection panels (MenuPause pause_01): kind 0 bugs (green), 1 quest items (purple),
    /// 2 treasures (tan); the picture is (w + 24) x (h + 24) with the drop shadow down-left.
    std::optional<Img> CollPanel(int w, int h, int kind);
    std::optional<Img> CollSlot(int s, int kind);
    /// Link's view cone (MapField P_sight_01) turned `deg` clockwise from map-up on a
    /// kConeCanvas square centred on Link's head (the 84 px map marker).
    std::optional<Img> Cone(int deg);
    /// Builds the image for a key such as "lyt/DoButton/tr_rupy_00" (without "module:sshd:").
    std::optional<Img> Build(std::string_view key);
    const std::string& LastError() const { return error_; }
    /// Tests (dev.42): the cap height the last ctitle/ name needed to fit its box (before the
    /// kMinPlateCap floor); below kMinPlateCap means the name overflows its box.
    float LastFitCap() const { return last_fit_cap_; }
    /// The smallest cap a ctitle/ map name is drawn at (the legibility minimum for secondary text, 18 px).
    static constexpr float kMinPlateCap = 18.0f;
    /// Bytes the art caches hold (romfs files, decoded layout textures, parsed stage geometry, the
    /// Common map materials): diagnostics for the module's memory log (dev.33).
    size_t CacheBytes() const;
    /// Parsed stage geometry kept for at most this many stages, least recently used dropped (dev.33).
    static constexpr size_t kStageCacheMax = 4;
    /// Byte budgets of the romfs file cache and the decoded layout texture cache (dev.33, LRU).
    static constexpr size_t kFileCacheBytes = 24u << 20, kTextureCacheBytes = 24u << 20;

private:
    bool PlaceLines(int label, std::vector<Img>& lines, int& tw, int& th);
    bool IsleLines(int idx, std::vector<Img>& lines, int& th);
    const std::vector<uint8_t>* File(const std::string& path);
    void CacheTexture(const std::string& key, const Img& im);
    const Font* GetFont(bool special);
    std::optional<Img> PaperBase(int w, int h);
    std::vector<std::pair<std::string, std::vector<U8Entry>>> layouts_;
    std::vector<std::pair<std::string, Img>> textures_;
    struct StageTriangles;
    std::shared_ptr<StageTriangles> StageTris(const std::string& st);
    std::vector<std::vector<std::array<float, 2>>> RailPaths(const std::string& st);
    std::vector<std::pair<std::string, std::shared_ptr<StageTriangles>>> triangles_; // LRU, newest last
    void CacheStage(const std::string& st, std::shared_ptr<StageTriangles> rooms);
    struct MapRes;
    const MapRes* MapResources();
    std::shared_ptr<MapRes> map_res_;
    bool map_res_failed_ = false;
    bool keep_classes_ = false;
    float yband_[2] = {-1e30f, 1e30f}; // dev.34 gmapf: the drawn floor's height band
    int board_[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1}; // Sky Keep's cells for Map("D003")
    int board_cur_ = -1;
    bool board_cur_only_ = false;
    std::vector<uint8_t> last_classes_;
    std::vector<uint8_t> last_edges_;
    RomfsReader read_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files_;
    std::vector<std::pair<bool, std::shared_ptr<Font>>> fonts_;
    std::string error_;
    float last_fit_cap_ = 0.0f;
    std::vector<std::pair<int, std::string>> item_names_;
    std::vector<std::pair<std::string, std::string>> map_text_;
};

/// Per-area map pictures. The world box is the combined KCL bounding box of the stage's rooms
/// (the map art notes); the picture maps world (x, z) to pixel ((xmax - x) * ppu + pad,
/// (zmax - z) * ppu + pad), unless flip_h / flip_v selects the opposite axis. Map-widget units
/// default to u = -x, v = z (y up); the manifest uses the same orientation and bounds.
struct AreaMap {
    const char* stage;
    float xmin, xmax, zmin, zmax;
    float ppu;      // picture pixels per world unit
    bool sky_floor; // unused since dev.32 (the cloud barrier is not map-tagged in its PLC)
    bool flip_v = false; // the native map has +z down: dungeons (live D100 2026-11-07) and, since dev.32, the overworld fields
    bool flip_h = false; // +x right on the native map: dungeons, Lanayru subareas (live F301/F302/F303), fields (dev.32);
                         // only the Sealed Grounds (F401 / F402) keep +x left / +z up (native cone calibration)
    uint8_t rot = 0;  // dev.34 generic frames: the stage's MapArea yaw in quarter turns (map x = x cos - z sin,
                      // map z = x sin + z cos); the extent is in those map coordinates
    bool gen = false; // dev.34: a generic sub-stage frame (kGenPx square picture, see GenStage)
};
inline constexpr int kMapPad = 40;
inline constexpr int kMapPast = 1; // Art::Map state bit: the native past variant
inline constexpr int kMapF406 = 2; // Art::Map state bit (F402): layer 7's world-space oarc/F406Col (dev.33)
/// dev.35: Art::Map state bits 8..12 = the loaded layer + 1 (0 = not given): a stage whose layer
/// archive registers world-space water (LayerActors, e.g. F100_1's WaterSf pool) draws that layer's.
inline constexpr int kMapLayerShift = 8;
/// The stage has layer-dependent map actors (the module then passes the loaded layer in the state).
bool LayerActors(std::string_view stage);
/// Sky Keep's tile map (dev.31): one tile per cell, kSkyKeepTile world units square centred on the
/// room stage's origin, cells kSkyKeepPitch apart; the picture's frame is u = x + (col - 1) * pitch,
/// v = -z - (row - 1) * pitch (+x right, +z down like the native tiles), kSkyKeepPpu px per unit.
/// dev.32: the native tiles are the 6000-unit squares (plus 500-unit door stubs) of the map dummy,
/// about 7000 units apart (native board capture: squares 175 px at a 202 px pitch).
inline constexpr float kSkyKeepTile = 7000.0f, kSkyKeepPitch = 7000.0f, kSkyKeepPpu = 0.075f;
inline constexpr int kConeCanvas = 400; // px of a cone/<deg> picture (the manifest's marker size)
std::span<const AreaMap> AreaMaps();
/// The stage's map area: a dedicated one (kAreas), else its generic frame (dev.34), else null.
const AreaMap* FindArea(std::string_view stage);

/// dev.34 (the stage survey): every romfs stage without a dedicated
/// area (Skyloft interiors, boss rooms, springs, Lake Floria, the Volcano Summit, the Lanayru
/// sub-stages, Hylia's Realm ...). The native map draws the current stage's own map faces, titled
/// by its STIF map-name id, turned by its MapArea yaw; the companion draws them the same way into
/// one square "GEN" frame: map coordinates (mx, mz) = the yaw turn of (x, z); frame u = s (mx -
/// cx), v = -s (mz - cz) with s = kGenHalf / half (the extent's larger half-span plus 6 %).
struct GenStage {
    const char* stage;
    float x0, x1, z0, z1; // drawn-face extent in map coordinates (0 when the stage has none)
    int8_t name_id;       // 0-Common/007-MapText.msbt MAP_<id>, -1 = no title
    uint8_t region;       // plate colour (kRegionCols) from the STIF area
    uint8_t rot;          // MapArea yaw, quarter turns
    uint8_t ctx;          // STIF map context (+0x11): 0 field, 1 dungeon, 3 interior, 4 sky
    uint8_t geo;          // 1 = has drawn faces, 0 = paper only (cutscene / sky arenas)
};
inline constexpr float kGenHalf = 12160.0f; // GEN frame half-size (frame units)
inline constexpr int kGenPx = 1600;         // GEN picture side: 2 kGenHalf * 0.0625 + 2 kMapPad
std::span<const GenStage> GenStages();
const GenStage* FindGen(std::string_view stage);
/// The GEN frame of a stage: centre (map coordinates) and scale.
struct GenFrame {
    double cx = 0, cz = 0, half = kGenHalf, s = 1;
};
GenFrame GenFrameOf(const GenStage& g);

} // namespace sshd
