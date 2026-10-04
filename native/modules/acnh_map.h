// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ACNH 3.0.3 island map generator (map lane). Re-does the game's NookPhone-map "photograph"
// (FieldUnitMapDrawer UI-map pass) from the player's own romfs + the island bytes:
//   * geometry: the real field models (FieldOutsideParts coast acres +
//   FieldLandMakingUnitModelParam
//     land-making units, layer 0 terrain + layer 1 roads) from Model/*.Nin_NX_NVN.zs (BFRES),
//     raster top-down, highest surface wins (the pass's depth test);
//   * colour: the compiled Park_UBER gsys_assign_user2 fragment programs (decoded, see
//     research/acnh/impl/map/COLORS.md): per-material const_color params, height gradient
//     t = sat((y - 0) / 35);
//   * composition: LMap P_Map_00 material black #0a0a0a / white #ffffff applied in linear space;
//     RT alpha 0 (rivers, hidden coast rock) shows the page's sea (transparent pixel).
// Nothing decoded is cached on disk or shipped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "acnh_map_core.h"

namespace acnh {

// ---- island input ---------------------------------------------------------------------------
// GSaveMain (main.dat) offsets of the Smmh 655400_655362 schema = every 3.0.x save
// (research/acnh/schema/SCHEMA.md). The live lane copies the same byte ranges from RAM.
inline constexpr uint32_t kSaveLandMakingMap =
    0x30AB00; // Land.MainField.LandMakingMap.Map[112][96]
inline constexpr std::size_t kLandMakingSize = 112u * 96u * 0xEu;
inline constexpr uint32_t kSaveStructureList = 0x32F700; // MainFieldStructure.StructureList[47]
inline constexpr std::size_t kStructureSize = 47u * 0x14u;
inline constexpr uint32_t kSaveFieldBlockData = 0x32FAAC; // OutsideField.FieldBlockData 9x8 u16
inline constexpr std::size_t kFieldBlockSize = 0x90;
inline constexpr uint32_t kSaveNpcHouseList = 0x481D20; // Land.NpcHouseList.HouseList[10]
inline constexpr std::size_t kNpcHouseStride = 0x12E8;  // +0 u32 HouseLevel, +0x1C4 s8 NpcList[0]
inline constexpr std::size_t kNpcHouseListSize = 10 * kNpcHouseStride;

struct IslandInput {
    std::vector<uint8_t>
        land_making; // kLandMakingSize: x-major [112][96] x {PartsInfoList[2], CliffLevel}
    std::vector<uint8_t> structures; // kStructureSize: 47 x 0x14
    std::vector<uint8_t>
        field_blocks; // kFieldBlockSize: [8 rows][9 cols] u16 FieldOutsideParts ids
    // optional (icons only): NpcHouseList.HouseList (kNpcHouseListSize) -> villager house owner +
    // level; hotel state from the island event flags (-1 unknown, 0 none, 1 HotelConstruction,
    // 2 HotelBuilt)
    std::vector<uint8_t> npc_houses;
    int hotel_state = -1;
    bool Valid() const {
        return land_making.size() == kLandMakingSize && structures.size() == kStructureSize &&
               field_blocks.size() == kFieldBlockSize;
    }
    /// Copies the three ranges out of a decrypted 3.0.x main.dat.
    bool FromMainDat(std::span<const uint8_t> main_dat);
};

// ---- output geometry ------------------------------------------------------------------------
// The map image covers world X [64, 1376) x Z [64, 1216) at 0.625 px per world unit (820x720):
// the 7x6 island acres (world 160..1280 x 160..1120) plus the coast ring incl. the pier.
struct MapView {
    float x0 = 64.f, z0 = 64.f, px_per_unit = 0.625f;
    int w = 820, h = 720;
    bool raw_rt = false; // true: emit the UI-map RT itself (colour sRGB-encoded, alpha as written)
    bool lmap = true;    // apply the LMap layout passes (patterns, thresholds, warps)
    bool operator==(const MapView&) const = default;
};
inline constexpr MapView kMapView{};
/// The game's own UI-map render target framing (640x640 at 0.5 px/unit centred on (720, 640)),
/// used by the tests to compare with the Python reference and the live capture.
inline constexpr MapView kGameRtView{80.f, 0.f, 0.5f, 640, 640, true};
/// The composed map in the game's capture framing (for the pixel compare with live captures).
inline constexpr MapView kGameMapView{80.f, 0.f, 0.5f, 640, 640, false};

inline float MapPx(float world, float origin, float scale) {
    return (world - origin) * scale;
}

// ---- zoomable page view (mapzoom lane) ---------------------------------------------------------
// The Map page shows the island box world X 60..1300 x Z 106..1175 (acres 1..8 / 1..7 plus the
// coast, widened west so the hotel on its western pier is whole, as on the game's Map app) in a
// runtime map widget of 804 x 693 canvas px, pinch/drag zoomable. The runtime draws pictures
// nearest-neighbour and publishes no zoom value, so sharpness at every zoom comes from pictures
// rendered at the displayed resolution: the base (zoom 1, exactly 1:1) and square tiles of the same
// box at 2x and 3x, which the page shows from a zoom threshold on (map widget groups with
// min_zoom). World positions on the page: x = world X, y = -world Z (y up).
inline constexpr float kZoomX0 = 60.f, kZoomZ0 = 106.f, kZoomW = 1240.f, kZoomH = 1069.f;
inline constexpr int kZoomBaseW = 804, kZoomBaseH = 693; // canvas px of the box at zoom 1
inline constexpr float kZoomPpu = 804.f / 1240.f;        // canvas px per world unit, zoom 1
inline constexpr float kZoomTile = 310.f;                // world units per tile side
inline constexpr int kZoomTilesX = 4, kZoomTilesY = 4;   // tiles per level (last row overhangs)
inline constexpr int kZoomMargin = 4;                    // px rendered past each tile edge
/// The base view (zoom 1): 804 x 693 px of the box.
MapView ZoomBaseView();
/// Tile t = ty * 4 + tx of zoom level z (2 or 3) incl. kZoomMargin px on every side.
MapView ZoomTileView(int z, int t);
/// Renders the page's base (cached, incremental: IslandMapGenerator) or a tile (z >= 2,
/// one-off), with the acre grid dashes under the island. False for a bad z / t.
bool GenerateZoomImage(Romfs& romfs, const IslandInput& in, int z, int t, Image& out);
/// The hotel's pier art (LMapIconHotel, the island's stage / side) drawn INTO a render of `view` at
/// its map size (kHotelW x kHotelH layout units at 0.8 per world unit): the pier art lies on the
/// island's jetty, so it has to zoom with the island. The hotel icon itself is a zoom-band marker
/// like the other icons (r7 mapfix).
void ZoomDrawHotel(Romfs& romfs, const IslandInput& in, const MapView& view, Image& im);

// ---- icon zoom bands (r7 mapfix, owner 2026-10-02) ---------------------------------------------
// Icons (houses, facilities, the hotel, bridges, inclines) and the selection highlight grow with
// the map zoom like the island, capped at kIconCap x their zoom-1 size. Runtime 15 publishes no
// zoom value and its map groups have only a min_zoom, and a world-sized marker (size_world) grows
// without a cap, so the page stacks one constant-size marker per band: band b is shown from zoom
// IconBandScale(b) on, at IconBandScale(b) x the zoom-1 size, drawn over the smaller bands (the
// same art scaled about its centre covers them). Every band's picture is rendered at its displayed
// size (no runtime scaling). Steps of 1/8 = 12.5 %.
inline constexpr int kIconBands = 9;
inline constexpr float kIconCap = 2.0f;
inline constexpr float IconBandScale(int b) {
    return 1.0f + static_cast<float>(b) / 8.0f;
}
/// The hotel icon inside its pier composite (MapIcon w / h of the hotel = the composite).
inline constexpr float kHotelIconOfComposite[2] = {64.0f / 196.0f, 64.0f / 136.0f};
/// The acre grid dashes under the island pixels of `im` (a render of `view`).
void ZoomGridUnder(Image& im, const MapView& view);

// ---- icons ----------------------------------------------------------------------------------
enum class MapIconKind : uint8_t {
    Office,
    Museum,
    Market,
    Tailor,
    Airport,
    Camp,
    InfoTent,
    Ship,
    Hotel,
    NpcHouse,
    PlayerHouse,
    Slope,
    Bridge,
    Player,
    Count
};
std::string_view MapIconName(MapIconKind kind); // "office", "npchouse", ... (image key part)

struct MapIcon {
    MapIconKind kind;
    int slot;              // StructureList index
    uint16_t structure_id; // StructureIndexUniqueID (StructureInfoParam UniqueID)
    float x, y;            // centre in map px (view of MapIcons())
    float w, h;    // the icon image's size in view px (rotated bounding box for bridges/inclines)
    int angle;     // StructureList Angle (0..3, x 90 degrees)
    float rot = 0; // icon rotation on screen, degrees counter-clockwise (nn::ui2d rotZ, y up)
    int pattern = -1;  // bridges: BridgePattern 0..5 = 03,04,05,Diagonal025,030,035 (-1 unknown)
    bool plan = false; // bridges / inclines: StructureList byte 7 bit 1 (planned: outline frame)
    int stage = -1;    // Type frame (houses: HouseLevel, hotel: 0 plot / 1 / 2), -1 = most built
    int owner = -1;    // villager houses: NpcHouseList.HouseList[i].NpcList[0] (villager slot)
    std::string key;   // image key of this icon ("map/icon/<kind>?..."), drawn at w x h
};
/// Buildings / bridges / inclines from StructureList, positions in `view` px (code: map world =
/// BaseUnit * 5 + 2.5 for houses / facilities / inclines, bridges by their pattern).
/// Bridge patterns need StructureBridgeParam: pass the romfs once (tables are cached).
std::vector<MapIcon> MapIcons(const IslandInput& in, const MapView& view = kMapView,
                              Romfs* romfs = nullptr);

// ---- facility names (r7 mapfix) -------------------------------------------------------------
/// Inputs of the Map app's facility name rule beyond the stage: the Land event flags its
/// renovation test 0x2a60460 reads (int values; <= 0 / unknown = not under renovation) and the
/// hotel flags. -1 = not read (treated as 0 / "not set").
struct FacilityFlags {
    int office_construction1 = -1; ///< OfficeConstruction1 (> 0: Resident Services renovating)
    int market_construction2 = -1; ///< MarketConstruction2 (> 0 with shop stage 2)
    int museum_construction1 = -1; ///< MuseumConstruction1 (>= 1 with museum stage 2)
    int museum_construction2 = -1; ///< MuseumConstruction2 (>= 1 with stage 3)
    int museum_construction3 = -1; ///< MuseumConstruction3 (> 0 with stage 4)
    int hotel_construction = -1;   ///< HotelConstruction (bool flag)
    int hotel_built = -1;          ///< HotelBuilt (bool flag)
};
/// The STR_Common label of a facility's name balloon (0x2e21adc, table 0x55bb290), "" = no name:
/// Airport 1001_00; RS tent 1002_00; Resident Services 1002_01, 1002_02 "(const.)" while
/// OfficeConstruction1 > 0; shop by stage 1 1004_02 Site, 2 1004_00 or 1004_03 renovating
/// (MarketConstruction2 > 0), 3 1004_01; museum 1 1003_02 tent site, 2 1003_00 tent, 3-5 1003_01,
/// and 1003_03 "(under construction)" at 2/3/4 while MuseumConstruction1/2/3 is set; tailor 1
/// 1005_01 site, 2 1005_00; campsite 1 1006_01, 2-3 1006_00; hotel 1007_01 "Hotel Site" while it is
/// begun but not complete (begun 0x15cebf4 = HotelConstruction || HotelBuilt, complete 0x15cec58 =
/// HotelBuilt && !HotelConstruction), else 1007_00. `stage` = the facility's stage (0x2ff51a8:
/// ShopLevel, MuseumLevel, TailorLevel, CampSiteLevel; ignored for the others).
std::string MapFacilityNameLabel(MapIconKind kind, int stage, const FacilityFlags& flags);
/// The hotel icon's Type frame (0x2e21728 case 15): Hotel00 (site art) while the hotel is begun but
/// not complete, Hotel01 otherwise. `hotel_state` as IslandInput (1 HotelConstruction, 2
/// HotelBuilt).
inline int HotelTypeFrame(int hotel_state) {
    return hotel_state == 1 ? 0 : hotel_state >= 2 ? 1 : hotel_state;
}

// ---- generator ------------------------------------------------------------------------------
struct MapStats {
    double ms_total = 0, ms_geometry = 0, ms_raster = 0, ms_compose = 0;
    int acres_baked = 0, acres_cached = 0;
    std::size_t geometry_bytes = 0, cache_bytes = 0;
    std::vector<std::string> unsupported; // materials whose UI-map colour is albedo-based (skipped)
};

/// Keeps decoded model geometry (compact triangles) and per-acre raster blocks between calls, so
/// a terraform change re-bakes only the acres whose input bytes changed. Thread-safe (one mutex).
class IslandMapGenerator {
public:
    IslandMapGenerator();
    ~IslandMapGenerator();
    IslandMapGenerator(const IslandMapGenerator&) = delete;
    IslandMapGenerator& operator=(const IslandMapGenerator&) = delete;

    bool Generate(Romfs& romfs, const IslandInput& in, Image& out, const MapView& view = kMapView,
                  MapStats* stats = nullptr);
    /// One render of `view` that keeps the cached view's acre blocks / composition untouched (zoom
    /// tiles): geometry and material rules are shared, the blocks are baked into scratch.
    bool GenerateOnce(Romfs& romfs, const IslandInput& in, Image& out, const MapView& view,
                      MapStats* stats = nullptr);

private:
    bool GenerateLocked(Romfs& romfs, const IslandInput& in, Image& out, const MapView& view,
                        MapStats* stats);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Icon art from the LMapIcon<X> layouts (P_Icon_00: texture mask lerped black->white, x vertex
/// colour gradient), at the texture's native size.
bool MapIconImage(Romfs& romfs, MapIconKind kind, Image& out);
/// The same at a Type-animation frame of P_Icon_00 (`stage` >= 0), e.g. villager house HouseLevel.
bool MapIconImageStage(Romfs& romfs, MapIconKind kind, int stage, Image& out);
/// The same with the material colours replaced (null = the layout's), e.g. the Attention colours.
bool MapIconImageColours(Romfs& romfs, MapIconKind kind, int stage, const Rgba* black,
                         const Rgba* white, Image& out);

// ---- image keys (core dispatcher hook) ------------------------------------------------------
/// The live lane hands over the island bytes for a revision (sample thread); MapLoadImage
/// (asset worker) renders `map/island/<rev>?z=...` from the most recent submission.
void MapSubmitIsland(uint32_t rev, IslandInput in);
/// Keys: "module:acnh/map/island/<rev>?z=1" (zoom base) / "?z=2|3&t=<tile>",
/// "module:acnh/map/icon/<kind>" (the "module:acnh/" prefix is optional). Returns false for
/// unknown keys (also the island without ?z=, which nothing requests) / no island yet.
bool MapLoadImage(Romfs& romfs, std::string_view key, Image& out);
/// The module's Fonts for the selection names (null = a private instance, tests); also forgets
/// the island submissions (module create / destroy: a new reader restarts its revisions).
void MapSetFonts(Fonts* fonts);

/// The hotel pier art side of an island (FieldBlockData): 0 SE, 1 SW, 2 no jetty acre.
int MapPierSide(const std::vector<uint8_t>& field_blocks);

} // namespace acnh
