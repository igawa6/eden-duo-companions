// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ACNH map icons (positions from StructureList, art from the LMapIcon<X> layouts) and the
// map lane's image keys (module:acnh/map/island/<rev>, module:acnh/map/icon/<kind>).

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <unordered_map>

#include "acnh_bcsv.h"
#include "acnh_draw.h"
#include "acnh_font.h"
#include "acnh_lyt.h"
#include "acnh_map.h"
#include "acnh_ui_art.h"

namespace acnh {
namespace {

struct IconDef {
    MapIconKind kind;
    std::string_view key;    // image key part
    std::string_view layout; // Layout/<layout>.Nin_NX_NVN.zs
    std::string_view pane;   // the icon pane in that layout
    float w, h;              // its size (layout units; LMap draws at 0.8 px per world unit)
    bool latest_stage;       // see DecodeStage
};
// Pane sizes from the LMapIcon* bflyt (DATA): P_Icon_00 64x64 (facilities, hotel), 92x82 airport,
// 42x36 villager house, 52x36 player house, 32x16 incline, 26x18 bridge (window pane),
// P_Player_00 48x64.
constexpr std::array<IconDef, static_cast<size_t>(MapIconKind::Count)> kIcons{{
    {MapIconKind::Office, "office", "LMapIconOffice", "P_Icon_00", 64, 64, true},
    {MapIconKind::Museum, "museum", "LMapIconMuseum", "P_Icon_00", 64, 64, true},
    {MapIconKind::Market, "market", "LMapIconMarket", "P_Icon_00", 64, 64, true},
    {MapIconKind::Tailor, "tailor", "LMapIconTailor", "P_Icon_00", 64, 64, true},
    {MapIconKind::Airport, "airport", "LMapIconAirport", "P_Icon_00", 92, 82, true},
    {MapIconKind::Camp, "camp", "LMapIconCamp", "P_Icon_00", 64, 64, true},
    {MapIconKind::InfoTent, "infotent", "LMapIconInfoTent", "P_Icon_00", 64, 64, true},
    {MapIconKind::Ship, "ship", "LMapIconShip", "P_Icon_00", 64, 64, true},
    // hotel: layout default (00 plot frame) = what the live map shows before HHP is built (MOCKS
    // r2)
    {MapIconKind::Hotel, "hotel", "LMapIconHotel", "P_Icon_00", 64, 64, false},
    {MapIconKind::NpcHouse, "npchouse", "LMapIconNPCHouse", "P_Icon_00", 42, 36, true},
    {MapIconKind::PlayerHouse, "playerhouse", "LMapIconPlayerHouse", "P_Icon_00", 52, 36, true},
    {MapIconKind::Slope, "slope", "LMapIconSlope", "P_Icon_00", 32, 16, true},
    {MapIconKind::Bridge, "bridge", "LMapIconBridge", "P_Icon_00", 26, 18, true},
    {MapIconKind::Player, "player", "LMapIconPlayer", "P_Player_00", 48, 64, false},
}};

// StructureInfoParam UniqueID -> icon (CODE, research/acnh/impl/fix/map.md "icons"): the
// StructureList reader 0x1260490 turns the u16 uid into the StructureInfoParam row (uid == row for
// 0..29, uid 42 = row 37 Hotel); the Map app's icon lists (static init 0x2a6c6b0) are rows 1..8
// player houses, 9..18 villager houses and the facilities 19 Market, 20 Office, 21 Museum,
// 22 Airport, 23 TanukichiTent, 24 Tailor, 25 CampSite, 28 TsunekichiShop (ship), 37 Hotel;
// bridges 26 and inclines 27 have their own lists.
std::optional<MapIconKind> StructureIcon(uint16_t uid) {
    if (uid >= 1 && uid <= 8)
        return MapIconKind::PlayerHouse;
    if (uid >= 9 && uid <= 18)
        return MapIconKind::NpcHouse;
    switch (uid) {
    case 19:
        return MapIconKind::Market;
    case 20:
        return MapIconKind::Office;
    case 21:
        return MapIconKind::Museum;
    case 22:
        return MapIconKind::Airport;
    case 23:
        return MapIconKind::InfoTent;
    case 24:
        return MapIconKind::Tailor;
    case 25:
        return MapIconKind::Camp;
    case 26:
        return MapIconKind::Bridge;
    case 27:
        return MapIconKind::Slope;
    case 28:
        return MapIconKind::Ship;
    case 42:
        return MapIconKind::Hotel;
    default:
        return std::nullopt;
    }
}

// Layout default texture is the "<stem>00" plot frame; the <Layout>_Type.bflan swaps 01/02 at
// runtime (code picks the stage - not located). PROVISIONAL: most-built stage that exists.
bool Texture(const Layout& layout, const std::string& name, Image& out) {
    const auto t = layout.Texture(name);
    if (!t)
        return false;
    out = *t;
    return true;
}

bool DecodeStage(const Layout& layout, const std::string& tex, Image& out) {
    const auto caret = tex.find('^');
    const std::string base = tex.substr(0, caret),
                      suffix = caret == std::string::npos ? "" : tex.substr(caret);
    if (base.size() > 2 && std::isdigit(static_cast<unsigned char>(base[base.size() - 1])) &&
        std::isdigit(static_cast<unsigned char>(base[base.size() - 2]))) {
        const std::string stem = base.substr(0, base.size() - 2);
        for (int st = 9; st >= 0; --st) {
            char num[3] = {static_cast<char>('0' + st / 10), static_cast<char>('0' + st % 10), 0};
            if (Texture(layout, stem + num + suffix, out))
                return true;
        }
    }
    return Texture(layout, tex, out);
}

uint8_t LinToSrgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const float e = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(std::clamp(e, 0.f, 1.f) * 255.f));
}
uint8_t LinToSrgb8(uint8_t v) {
    return LinToSrgb(v / 255.f);
}

// LMapIconHotel: the pier art under the plot frame. LMapIconHotel_TypePier.bflan frames (DATA):
// 0 SE00 / 1 SE01 / 2 SW00 / 3 SW01 = texture MapFullHotelIconPier<dir><stage>^s with P_Pier_00 x
// 89 / 89 / -87 / -88 and N_Icon_00 x 97.4 / 103.8 / -85.9 / -108.5 (y 29 / 54, layout y up).
// The stage follows the icon (00 plot frame = the layout default this lane draws). Code: frame
// value (this+0x1c734, 0x2e1c4b0..0x2e1c534) = SE 1 / SW 3 with the island flag HotelConstruction,
// SE 2 / SW 4 with HotelBuilt (frame = value - 1: LEAD; the built stage is not drawn yet).
// Pier = P_Pier_00 (#5b3d1b) + P_PierPattern_00 (#674925 through mapPatternRock^s), linear
// colours shown as sRGB like every LMap icon pane. The icon image is a 196 x 136 layout-unit
// canvas with the hotel icon at its centre (the icon sits on the structure position, as on the
// live map), so a page draws it like the other icons.
} // namespace

int MapPierSide(const std::vector<uint8_t>& field_blocks) {
    // the Map app (setup 0x2e19ff0, 0x2e1c3c4) asks 0x12871c0 for the island's jetty coast acre:
    // FieldOutsideParts 137/138 (FldOutSEBridge01/00) -> SE art, 135/136 (FldOutSWBridge01/00) ->
    // SW art, none -> no pier. The field deck models pair the same ids (StrcHotelA00Deck*).
    for (size_t i = 0; i + 1 < field_blocks.size(); i += 2) {
        const int id = field_blocks[i] | (field_blocks[i + 1] << 8);
        if (id == 137 || id == 138)
            return 0;
        if (id == 135 || id == 136)
            return 1;
    }
    return 2;
}

namespace {
// The hotel's map anchor (CODE, r7 mapfix): the facility record of the hotel (map icon list row 37)
// does not use its StructureList position; 0x2ffb7f8 takes (x, y, z) from the static table
// 0x4083db4 (stride 0x18) indexed by [0x1bcc], which 0x2fed2c4 sets to 4 - (id - 135) for the
// island's jetty coast acre (FieldOutsideParts 135 FldOutSWBridge01 -> 4, 136 SWBridge00 -> 3, 137
// SEBridge01 -> 2, 138 SEBridge00 -> 1; 0 = none). The LMapIconHotel part sits at that point;
// inside it the icon is at N_Icon_00 (x by the TypePier frame, y 54 layout units). LIVE (s5c, jetty
// 135): icon centre world (121.7, 1000.0) measured on the NookPhone Map vs (121.9, 1000.0) from
// this rule.
constexpr float kHotelAnchor[5][2] = {{0, 0}, {1200, 1010}, {1190, 1060}, {230, 1010}, {260, 1070}};
int HotelAnchorIndex(const std::vector<uint8_t>& field_blocks) {
    for (size_t i = 0; i + 1 < field_blocks.size(); i += 2) {
        const int id = field_blocks[i] | (field_blocks[i + 1] << 8);
        if (id >= 135 && id <= 138)
            return 4 - (id - 135);
    }
    return 0;
}
constexpr int kHotelW = 196, kHotelH = 136;
// N_Icon_00 of LMapIconHotel by TypePier frame (DATA): 0 SE00 97.4, 1 SE01 103.8, 2 SW00 -85.9,
// 3 SW01 -108.5 (x); y 54 (layout y up)
constexpr float HotelIconX(bool west, bool built) {
    return west ? (built ? -108.5f : -85.9f) : (built ? 103.8f : 97.4f);
}
constexpr float kHotelIconY = 54.f;
// side 0 SE, 1 SW, -1 none; built: the pier stage 01 (TypePier frames 1 SE01 / 3 SW01: pier x 89 /
// -88, icon x 103.8 / -108.5) for HotelBuilt (pier value SE 2 / SW 4, frame = value - 1: LEAD)
bool HotelWithPier(Romfs& rf, const Image& icon, int side, bool built, Image& out) {
    const bool west = side == 1;
    const auto layout = LoadLayout(rf, "LMapIconHotel");
    Image mask, rock;
    const std::string pier =
        std::string{"MapFullHotelIconPier"} + (west ? "SW" : "SE") + (built ? "01^s" : "00^s");
    if (!layout || !Texture(*layout, pier, mask) || !Texture(*layout, "mapPatternRock^s", rock) ||
        mask.w <= 0 || rock.w <= 0 || rock.h <= 0)
        return false;
    const float pier_x = west ? (built ? -88.f : -87.f) : 89.f;
    const float icon_x = HotelIconX(west, built);
    const float pier_y = 29.f, icon_y = kHotelIconY;
    out.w = kHotelW;
    out.h = kHotelH;
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    const float cx = kHotelW / 2.f, cy = kHotelH / 2.f; // icon centre
    if (side < 0)
        mask.h = 0; // no jetty acre: the icon alone, same canvas
    const int x0 = static_cast<int>(std::lround(cx + (pier_x - icon_x) - mask.w / 2.f));
    const int y0 = static_cast<int>(std::lround(cy - (pier_y - icon_y) - mask.h / 2.f));
    const uint8_t base[3] = {LinToSrgb8(0x5b), LinToSrgb8(0x3d), LinToSrgb8(0x1b)};
    const uint8_t pat[3] = {LinToSrgb8(0x67), LinToSrgb8(0x49), LinToSrgb8(0x25)};
    for (int y = 0; y < mask.h; ++y)
        for (int x = 0; x < mask.w; ++x) {
            const int ox = x0 + x, oy = y0 + y;
            if (ox < 0 || oy < 0 || ox >= out.w || oy >= out.h)
                continue;
            const uint8_t a = mask.rgba[(size_t(y) * mask.w + x) * 4 + 3];
            if (!a)
                continue;
            const float r = rock.rgba[(size_t(y % rock.h) * rock.w + x % rock.w) * 4 + 3] / 255.f;
            uint8_t* d = out.rgba.data() + (size_t(oy) * out.w + ox) * 4;
            for (int c = 0; c < 3; ++c)
                d[c] = static_cast<uint8_t>(std::lround(base[c] + (pat[c] - base[c]) * r));
            d[3] = a;
        }
    draw::Composite(out, icon, static_cast<int>(std::lround(cx - icon.w / 2.f)),
                    static_cast<int>(std::lround(cy - icon.h / 2.f)));
    return true;
}

} // namespace

std::string MapFacilityNameLabel(MapIconKind kind, int stage, const FacilityFlags& f) {
    switch (kind) {
    case MapIconKind::Airport:
        return "1001_00";
    case MapIconKind::InfoTent:
        return "1002_00";
    case MapIconKind::Office:
        return f.office_construction1 > 0 ? "1002_02" : "1002_01";
    case MapIconKind::Market:
        // renovation 0x2a58a70: shop stage 2 && MarketConstruction2 > 0
        return stage == 1   ? "1004_02"
               : stage == 2 ? (f.market_construction2 > 0 ? "1004_03" : "1004_00")
               : stage == 3 ? "1004_01"
                            : "";
    case MapIconKind::Museum: {
        // renovation 0x2a5a970: stage 2 && MuseumConstruction1 >= 1, 3 && MuseumConstruction2 >= 1,
        // 4 && MuseumConstruction3 > 0
        const bool renov = (stage == 2 && f.museum_construction1 >= 1) ||
                           (stage == 3 && f.museum_construction2 >= 1) ||
                           (stage == 4 && f.museum_construction3 > 0);
        if (stage == 1)
            return "1003_02";
        if (stage == 2)
            return renov ? "1003_03" : "1003_00";
        if (stage >= 3 && stage <= 5)
            return renov ? "1003_03" : "1003_01";
        return "";
    }
    case MapIconKind::Tailor:
        return stage == 1 ? "1005_01" : stage == 2 ? "1005_00" : "";
    case MapIconKind::Camp:
        return stage == 1 ? "1006_01" : stage == 2 || stage == 3 ? "1006_00" : "";
    case MapIconKind::Hotel: {
        // begun = 0x15cebf4 (HotelConstruction || HotelBuilt), complete = 0x15cec58
        // (!HotelConstruction && HotelBuilt): a begun, not complete hotel is the "Hotel Site"
        const bool work = f.hotel_construction > 0, built = f.hotel_built > 0;
        return (work || built) && !(built && !work) ? "1007_01" : "1007_00";
    }
    default:
        return "";
    }
}

std::string_view MapIconName(MapIconKind kind) {
    const auto i = static_cast<size_t>(kind);
    return i < kIcons.size() ? kIcons[i].key : std::string_view{};
}

namespace {
// StructureBridgeParam: UniqueID -> BridgePattern (enum column = CRC32 of the name), in the code's
// order 03, 04, 05, Diagonal025, Diagonal030, Diagonal035 (State-frame tables 0x4081370/0x4081384;
// the order = the bcsv's first rows, LEAD)
struct IconTables {
    std::mutex mu;
    bool loaded = false;
    std::unordered_map<uint16_t, int> bridge_pattern;
    // r7 mapfix (DATA, verified live): the LMap layout places each icon kind's parts in a group
    // pane (N_Icon<X>_00): every group sits at (-2, +2) layout units except N_IconBridge_00 (0, 0),
    // so the game draws every icon but the bridges 2.5 world units left of and above its structure
    // point. World units per LMap layout unit from the acre grid of the same layout
    // (P_LineSide_00/01: 128 units per 160-unit acre = 1.25). Per kind: the group offset in world
    // units (x, z; layout y up).
    bool lmap = false;
    float world_per_unit = 1.25f;
    std::array<std::array<float, 2>, static_cast<size_t>(MapIconKind::Count)> group{};
};
IconTables& Tables() {
    static IconTables t;
    return t;
}
constexpr uint32_t kColUniqueID = 0x54706054;      // CRC32("UniqueID u16")
constexpr uint32_t kColBridgePattern = 0x74be6041; // BridgePattern (enum)

void LoadIconTables(Romfs& rf) {
    auto& t = Tables();
    std::lock_guard lk{t.mu};
    if (t.loaded)
        return;
    t.loaded = true;
    // LMap icon groups (see IconTables)
    if (const auto lmap = LoadLayout(rf, "LMap")) {
        const Bflyt& tr = lmap->Tree();
        const int l0 = tr.Find("P_LineSide_00"), l1 = tr.Find("P_LineSide_01");
        if (l0 >= 0 && l1 >= 0 && std::fabs(tr.panes[l0].y - tr.panes[l1].y) > 1.f) {
            // the acre grid's rows are 160 world units apart (acre = 32 BaseUnits x 5)
            t.world_per_unit = 160.f / std::fabs(tr.panes[l0].y - tr.panes[l1].y);
            static constexpr std::pair<MapIconKind, std::string_view> kGroup[] = {
                {MapIconKind::Office, "N_IconOffice_00"},
                {MapIconKind::Museum, "N_IconMuseum_00"},
                {MapIconKind::Market, "N_IconMarket_00"},
                {MapIconKind::Tailor, "N_IconTailor_00"},
                {MapIconKind::Airport, "N_IconAirport_00"},
                {MapIconKind::Camp, "N_IconCamp_00"},
                {MapIconKind::InfoTent, "N_IconInfoTent_00"},
                {MapIconKind::Ship, "N_IconShip_00"},
                {MapIconKind::Hotel, "N_IconHotel_00"},
                {MapIconKind::NpcHouse, "N_IconNPCHouse_00"},
                {MapIconKind::PlayerHouse, "N_IconPlayerHouse_00"},
                {MapIconKind::Slope, "N_IconSlope_00"},
                {MapIconKind::Bridge, "N_IconBridge_00"},
                {MapIconKind::Player, "N_MapArea_00"}};
            bool all = true;
            for (const auto& [kind, pane] : kGroup) {
                const int i = tr.Find(pane);
                if (i < 0) {
                    all = false;
                    continue;
                }
                t.group[static_cast<size_t>(kind)] = {tr.panes[i].x * t.world_per_unit,
                                                      -tr.panes[i].y * t.world_per_unit};
            }
            t.lmap = all;
        }
    }
    std::vector<uint8_t> b;
    Bcsv bc;
    if (!rf.Read("Bcsv/StructureBridgeParam.bcsv", b) || !bc.Parse(std::move(b)) ||
        !bc.Has(kColUniqueID) || !bc.Has(kColBridgePattern))
        return;
    static constexpr std::array<std::string_view, 6> kPat{
        "03", "04", "05", "Diagonal025", "Diagonal030", "Diagonal035"};
    for (size_t r = 0; r < bc.Rows(); ++r) {
        const uint32_t h = bc.U(r, kColBridgePattern);
        for (size_t p = 0; p < kPat.size(); ++p)
            if (Crc32(kPat[p]) == h)
                t.bridge_pattern[static_cast<uint16_t>(bc.U(r, kColUniqueID))] =
                    static_cast<int>(p);
    }
}
std::array<float, 3> GroupOffset(MapIconKind kind) { // world x, z, world units per layout unit
    auto& t = Tables();
    std::lock_guard lk{t.mu};
    const auto& g = t.group[static_cast<size_t>(kind)];
    return {g[0], g[1], t.world_per_unit};
}

// The icon's Type animation moves P_Icon_00 with an FLPA curve on some kinds (DATA:
// LMapIconNPCHouse_Type / LMapIconPlayerHouse_Type y +4 from frame 1 on, LMapIconOffice_Type y -8
// at frame 1; the other kinds have none). Translation in layout units (x, y up) at `frame`.
std::array<float, 2> TypeShift(Romfs& rf, std::string_view layout_name, float frame) {
    const auto layout = LoadLayout(rf, layout_name);
    const Bflan* a = layout ? layout->Anim(std::string{layout_name} + "_Type") : nullptr;
    if (!a)
        return {0.f, 0.f};
    return {a->At("P_Icon_00", "FLPA", 0, 0, frame).value_or(0.f),
            a->At("P_Icon_00", "FLPA", 0, 1, frame).value_or(0.f)};
}

int BridgePattern(uint16_t id) {
    auto& t = Tables();
    std::lock_guard lk{t.mu};
    const auto it = t.bridge_pattern.find(id);
    return it == t.bridge_pattern.end() ? -1 : it->second;
}

// LMapIconBridge_State (DATA): frames 0-2 rot 0, 3-5 rot 90, 6-8 rot -45, 9-11 rot 45; icon width
// 26/34/42 (straight) or 34/39/44 (diagonal) x 18, plan frame 36/44/52 or 44/49/54 x 26.
// Frame from code (0x2e20cbc): Angle 0/2 -> {0,1,2,9,10,11}[pattern], Angle 1/3 -> {3..8}[pattern].
struct BridgeGeom {
    float rot, w, h, frame_w, frame_h;
};
BridgeGeom BridgeShape(int pattern, int angle) {
    const int p = pattern >= 0 && pattern < 6 ? pattern : 1; // unknown -> 04 (most common length)
    static constexpr float kW[6] = {26, 34, 42, 34, 39, 44}, kF[6] = {36, 44, 52, 44, 49, 54};
    const bool odd = (angle & 1) != 0, diag = p >= 3;
    const float rot = diag ? (odd ? -45.f : 45.f) : (odd ? 90.f : 0.f);
    return {rot, kW[p], 18.f, kF[p], 26.f};
}
// LMapIconSlope_State (DATA): frame 0 rotZ 180, 1 rotZ 0, 2 rotZ -90, 3 rotZ 90; frame from code
// (0x2fd24ac): Angle 0 -> 0, 2 -> 1, 1 -> 2, 3 -> 3  =>  rotZ = 180 + 90 * Angle (mod 360).
float SlopeRot(int angle) {
    return static_cast<float>((180 + 90 * (angle & 3)) % 360);
}
void RotatedBox(float w, float h, float rot, float& bw, float& bh) {
    const double r = rot * 3.14159265358979323846 / 180.0;
    const double c = std::fabs(std::cos(r)), s = std::fabs(std::sin(r));
    bw = static_cast<float>(w * c + h * s);
    bh = static_cast<float>(w * s + h * c);
}
} // namespace

std::vector<MapIcon> MapIcons(const IslandInput& in, const MapView& v, Romfs* romfs) {
    std::vector<MapIcon> out;
    if (in.structures.size() != kStructureSize)
        return out;
    if (romfs)
        LoadIconTables(*romfs);
    const float k = v.px_per_unit / 0.8f; // LMap layout units are 0.8 px per world unit
    // villager house i -> owner slot / level (NpcHouseList, code 0x25641e0 / 0x25640d0)
    auto house_owner = [&](int i) -> int {
        const size_t o = size_t(i) * kNpcHouseStride + 0x1C4;
        return in.npc_houses.size() == kNpcHouseListSize ? static_cast<int8_t>(in.npc_houses[o])
                                                         : -1;
    };
    auto house_level = [&](int i) -> int {
        if (in.npc_houses.size() != kNpcHouseListSize)
            return -1;
        const size_t o = size_t(i) * kNpcHouseStride;
        const uint32_t lv = in.npc_houses[o] | (in.npc_houses[o + 1] << 8) |
                            (in.npc_houses[o + 2] << 16) | (uint32_t(in.npc_houses[o + 3]) << 24);
        return lv < 3 ? static_cast<int>(lv) : 0; // clamped like the code (< 3 else 0)
    };
    for (int i = 0; i < 47; ++i) {
        const uint8_t* e = in.structures.data() + i * 0x14;
        const uint16_t uid = static_cast<uint16_t>(e[0] | (e[1] << 8));
        const auto kind = StructureIcon(uid);
        if (!uid || !kind || (e[7] & 1)) // byte 7 bit 0: invalid record (reader 0x1260490)
            continue;
        const int16_t bx = static_cast<int16_t>(e[2] | (e[3] << 8));
        const int16_t bz = static_cast<int16_t>(e[4] | (e[5] << 8));
        const int angle = static_cast<int8_t>(e[6]) & 3;
        const uint16_t variant =
            static_cast<uint16_t>(e[8] | (e[9] << 8)); // Slope/BridgeParam UniqueID
        const IconDef& d = kIcons[static_cast<size_t>(*kind)];
        MapIcon ic{*kind, i, uid, 0, 0, d.w * k, d.h * k, angle};
        // map world position (fmadd X, 5.0, 2.5: 0x2ffb14c houses / 0x2ffb3c0 facilities /
        // 0x2fd368c inclines). Hotel and ship use their own code (not decoded): BaseUnit * 5 (LEAD,
        // the hotel pier composite was matched live on that)
        float wx = bx * 5.f + 2.5f, wz = bz * 5.f + 2.5f;
        std::string key{"map/icon/"};
        key += d.key;
        switch (*kind) {
        case MapIconKind::Hotel:
        case MapIconKind::Ship:
            wx = bx * 5.f, wz = bz * 5.f;
            if (*kind == MapIconKind::Hotel) {
                // the icon centre = the code's anchor for the jetty acre + N_Icon_00 (see
                // kHotelAnchor); the TypePier art follows the side and stage like the pier
                // composite (HotelWithPier)
                if (const int a = HotelAnchorIndex(in.field_blocks); a > 0 && romfs) {
                    const auto g = GroupOffset(MapIconKind::Hotel);
                    const bool west = a >= 3, built = in.hotel_state >= 2;
                    wx = kHotelAnchor[a][0] + HotelIconX(west, built) * g[2];
                    wz = kHotelAnchor[a][1] - kHotelIconY * g[2];
                }
                // the key draws the pier composite: kHotelW x kHotelH layout units with the 64-unit
                // icon at its centre (HotelWithPier), so the icon's size is the composite's
                ic.w = kHotelW * k, ic.h = kHotelH * k;
                ic.stage = in.hotel_state;
                key += "?side=" + std::to_string(MapPierSide(in.field_blocks));
                if (in.hotel_state >= 0)
                    key += "&stage=" + std::to_string(in.hotel_state);
            }
            break;
        case MapIconKind::NpcHouse: {
            const int h = uid - 9;
            ic.owner = house_owner(h);
            ic.stage = house_level(h);
            if (ic.stage >= 0)
                key += "?stage=" + std::to_string(ic.stage);
            break;
        }
        case MapIconKind::Bridge: {
            // centre (0x2a61250): diagonal 025 / 035 -> ((X, Z) + off[q]) * 5 + 2.5 with
            // q = quadrant of Angle, off (0x3f58020) = (-1,0), (0,0), (0,-1), (-1,-1); others X * 5
            ic.pattern = BridgePattern(variant);
            ic.plan = (e[7] & 2) != 0;
            if (ic.pattern == 3 || ic.pattern == 5) {
                static constexpr int kOff[4][2] = {{-1, 0}, {0, 0}, {0, -1}, {-1, -1}};
                wx = (bx + kOff[angle][0]) * 5.f + 2.5f;
                wz = (bz + kOff[angle][1]) * 5.f + 2.5f;
            } else {
                wx = bx * 5.f, wz = bz * 5.f;
            }
            const BridgeGeom g = BridgeShape(ic.pattern, angle);
            ic.rot = g.rot;
            float bw, bh;
            RotatedBox(ic.plan ? g.frame_w : g.w, ic.plan ? g.frame_h : g.h, g.rot, bw, bh);
            ic.w = bw * k, ic.h = bh * k;
            key += "?p=" + std::to_string(ic.pattern < 0 ? 1 : ic.pattern) +
                   "&a=" + std::to_string(angle);
            if (ic.plan)
                key += "&plan=1";
            break;
        }
        case MapIconKind::Slope: {
            ic.plan = (e[7] & 2) != 0;
            ic.rot = SlopeRot(angle);
            float bw, bh;
            RotatedBox(ic.plan ? 40.f : 32.f, ic.plan ? 24.f : 16.f, ic.rot, bw, bh);
            ic.w = bw * k, ic.h = bh * k;
            key += "?a=" + std::to_string(angle);
            if (ic.plan)
                key += "&plan=1";
            break;
        }
        default:
            break;
        }
        // the LMap group pane offset of this kind and the Type animation's icon translation (r7
        // mapfix, DATA; see IconTables / TypeShift). The ship's position code differs (0x2ffb7bc,
        // not decoded: LEAD) and is left as before.
        if (romfs && *kind != MapIconKind::Ship) {
            const auto g = GroupOffset(*kind);
            wx += g[0];
            wz += g[1];
            if (*kind == MapIconKind::NpcHouse || *kind == MapIconKind::PlayerHouse ||
                *kind == MapIconKind::Office) {
                // Type frame: villager houses = the drawn stage (HouseLevel; latest art when
                // unknown), player houses = the latest art (house level not read: LEAD), the office
                // 1 (code 0x2e21728: frame 1 for facility 1 at every stage)
                const float frame = *kind == MapIconKind::NpcHouse && ic.stage >= 0
                                        ? static_cast<float>(ic.stage)
                                    : *kind == MapIconKind::Office ? 1.f
                                                                   : 1e6f;
                const auto sh = TypeShift(*romfs, d.layout, frame);
                wx += sh[0] * g[2];
                wz -= sh[1] * g[2];
            }
        }
        ic.x = MapPx(wx, v.x0, v.px_per_unit);
        ic.y = MapPx(wz, v.z0, v.px_per_unit);
        ic.key = std::move(key);
        out.push_back(std::move(ic));
    }
    return out;
}

bool MapIconImage(Romfs& rf, MapIconKind kind, Image& out) {
    return MapIconImageStage(rf, kind, -1, out);
}

// stage >= 0: the texture of P_Icon_00 at that frame of <Layout>_Type (FLTP; NPC houses NPC00/01/02
// = HouseLevel, hotel Hotel00 / Hotel01); -1: the most built stage that exists
bool MapIconImageStage(Romfs& rf, MapIconKind kind, int stage, Image& out) {
    return MapIconImageColours(rf, kind, stage, nullptr, nullptr, out);
}

// black / white = the material colours to use instead of the layout's (the icons' Attention
// animation recolours P_Icon_00: LMapIcon*_Attention FLMC black 233,62,6 / white 255,209,19)
bool MapIconImageColours(Romfs& rf, MapIconKind kind, int stage, const Rgba* black_over,
                         const Rgba* white_over, Image& out) {
    const auto idx = static_cast<size_t>(kind);
    if (idx >= kIcons.size())
        return false;
    const auto layout = LoadLayout(rf, kIcons[idx].layout);
    if (!layout)
        return false;
    const Bflyt& L = layout->Tree();
    const LytPane* pane = nullptr;
    for (const auto& p : L.panes)
        if (p.name == kIcons[idx].pane)
            pane = &p; // the last pane of that name
    if (!pane)
        return false;
    const LytMaterial* mat = nullptr;
    if (pane->tag == "pic1" && pane->material >= 0) {
        mat = &L.materials[pane->material];
    } else { // window pane (bridge): the content material, not the frame
        for (const auto& m : L.materials)
            if (m.name.rfind("P_Icon_00", 0) == 0 && !m.tex.empty() &&
                m.tex[0].find("Frame") == std::string::npos) {
                mat = &m;
                break;
            }
    }
    if (!mat || mat->tex.empty())
        return false;
    Image tex;
    std::string staged;
    if (stage >= 0)
        staged = layout->AnimTexture(std::string{kIcons[idx].layout} + "_Type",
                                     static_cast<float>(stage), mat->name);
    if (!staged.empty()) {
        if (!Texture(*layout, staged, tex))
            return false;
    } else if (!(kIcons[idx].latest_stage ? DecodeStage(*layout, mat->tex[0], tex)
                                          : Texture(*layout, mat->tex[0], tex))) {
        return false;
    }
    if (tex.w <= 0 || tex.h <= 0)
        return false;
    // an override recolours rgb only (the Attention FLMC animates the colour channels)
    Rgba black = mat->black, white = mat->white;
    if (black_over)
        black = Rgba{black_over->r, black_over->g, black_over->b, mat->black.a};
    if (white_over)
        white = Rgba{white_over->r, white_over->g, white_over->b, mat->white.a};
    const auto Ch = [](const Rgba& c, int k) {
        return k == 0 ? c.r : k == 1 ? c.g : k == 2 ? c.b : c.a;
    };
    // out = black + (white - black) * texture (colour from R, alpha from A), x vertex colour
    // gradient top (vtx[0]) -> bottom (vtx[2]).
    out.w = tex.w;
    out.h = tex.h;
    out.rgba.assign(size_t(tex.w) * tex.h * 4, 0);
    for (int y = 0; y < tex.h; ++y) {
        const float g = tex.h > 1 ? float(y) / float(tex.h - 1) : 0.f;
        for (int x = 0; x < tex.w; ++x) {
            const uint8_t* s = tex.rgba.data() + (size_t(y) * tex.w + x) * 4;
            uint8_t* d = out.rgba.data() + (size_t(y) * tex.w + x) * 4;
            const float kc = s[0] / 255.f, ka = s[3] / 255.f;
            for (int c = 0; c < 4; ++c) {
                const float bl = Ch(black, c) / 255.f, wh = Ch(white, c) / 255.f;
                float val = bl + (wh - bl) * (c < 3 ? kc : ka);
                const float vt = Ch(pane->vtx[0], c) / 255.f, vb = Ch(pane->vtx[2], c) / 255.f;
                val *= vt + (vb - vt) * g;
                // the LMap icon panes output linear colour (the map RT is sRGB): material
                // #e92206 (pin) shows as #f5662a on the live map, NPC house #ff9005 as #ffc324
                d[c] = c < 3 ? LinToSrgb(val)
                             : static_cast<uint8_t>(std::lround(std::clamp(val, 0.f, 1.f) * 255.f));
            }
        }
    }
    return true;
}

// ---- bridges / inclines (LMapIconBridge / LMapIconSlope) ------------------------------------
namespace {
constexpr float kIconScale = 2.f; // px per layout unit of the rotated bridge / incline images

std::array<float, 4> Bilinear(const Image& t, float x, float y) { // texel centres at +0.5, clamped
    x -= 0.5f, y -= 0.5f;
    const float fx = std::floor(x), fy = std::floor(y), ax = x - fx, ay = y - fy;
    const int x0 = std::clamp(static_cast<int>(fx), 0, t.w - 1),
              x1 = std::clamp(static_cast<int>(fx) + 1, 0, t.w - 1);
    const int y0 = std::clamp(static_cast<int>(fy), 0, t.h - 1),
              y1 = std::clamp(static_cast<int>(fy) + 1, 0, t.h - 1);
    std::array<float, 4> o{};
    for (int c = 0; c < 4; ++c) {
        auto at = [&](int xx, int yy) { return t.rgba[(size_t(yy) * t.w + xx) * 4 + c] / 255.f; };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * ax;
        const float bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * ax;
        o[c] = top + (bot - top) * ay;
    }
    return o;
}

// material colour of an LMap icon texel (as MapIconImage): black + (white - black) * (R, R, R, A),
// linear colour shown sRGB-encoded
void PutTint(uint8_t* d, const std::array<float, 4>& t, const Rgba& black, const Rgba& white) {
    const float b[4] = {black.r / 255.f, black.g / 255.f, black.b / 255.f, black.a / 255.f};
    const float w[4] = {white.r / 255.f, white.g / 255.f, white.b / 255.f, white.a / 255.f};
    for (int c = 0; c < 3; ++c)
        d[c] = LinToSrgb(b[c] + (w[c] - b[c]) * t[0]);
    d[3] = static_cast<uint8_t>(
        std::lround(std::clamp(b[3] + (w[3] - b[3]) * t[3], 0.f, 1.f) * 255.f));
}

// wnd1 with flag 0xb (HorizontalNoContent, one frame material): the left frame texture over
// [0, W - R) (columns past the texture's width repeat its last column: LEAD), mirrored over the
// right R units; no content quad.
Image WindowBar(const Image& tex, const Rgba& black, const Rgba& white, float W, float H, float R) {
    Image im;
    im.w = static_cast<int>(std::lround(W * kIconScale));
    im.h = static_cast<int>(std::lround(H * kIconScale));
    im.rgba.assign(size_t(im.w) * im.h * 4, 0);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float lx = (x + 0.5f) / kIconScale, ly = (y + 0.5f) / kIconScale;
            float tx = lx < W - R ? lx : W - lx;
            tx = std::min(tx, tex.w - 0.5f);
            const auto t = Bilinear(tex, tx, ly * tex.h / H);
            PutTint(im.rgba.data() + (size_t(y) * im.w + x) * 4, t, black, white);
        }
    return im;
}

// a pic1 texture stretched to its pane (W x H layout units)
Image PaneImage(const Image& tex, const Rgba& black, const Rgba& white, float W, float H) {
    Image im;
    im.w = static_cast<int>(std::lround(W * kIconScale));
    im.h = static_cast<int>(std::lround(H * kIconScale));
    im.rgba.assign(size_t(im.w) * im.h * 4, 0);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const auto t = Bilinear(tex, (x + 0.5f) * tex.w / im.w, (y + 0.5f) * tex.h / im.h);
            PutTint(im.rgba.data() + (size_t(y) * im.w + x) * 4, t, black, white);
        }
    return im;
}

// rotated by `deg` counter-clockwise on screen (nn::ui2d rotZ, y up) into its bounding box
Image RotateCcw(const Image& src, float deg) {
    if (deg == 0.f)
        return src;
    const double r = deg * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    Image out;
    out.w = static_cast<int>(std::ceil(std::fabs(src.w * c) + std::fabs(src.h * s) - 1e-6));
    out.h = static_cast<int>(std::ceil(std::fabs(src.w * s) + std::fabs(src.h * c) - 1e-6));
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    // destination (y down) -> source: a visual counter-clockwise turn by r maps source offsets
    // (x, y) to (x cos + y sin, -x sin + y cos); inverse = (x cos - y sin, x sin + y cos)
    const double ox = out.w / 2.0, oy = out.h / 2.0, sx = src.w / 2.0, sy = src.h / 2.0;
    const double inv[6] = {c, -s, sx - c * ox + s * oy, s, c, sy - s * ox - c * oy};
    draw::AffineComposite(out, src, inv);
    return out;
}

bool BridgeImage(Romfs& rf, int pattern, int angle, bool plan, Image& out) {
    const auto layout = LoadLayout(rf, "LMapIconBridge");
    if (!layout)
        return false;
    const Bflyt& L = layout->Tree();
    const int mi = L.Material(plan ? "P_Frame_00LT" : "P_Icon_00LT");
    Image tex;
    if (mi < 0 || L.materials[mi].tex.empty() || !Texture(*layout, L.materials[mi].tex[0], tex))
        return false;
    const BridgeGeom g = BridgeShape(pattern, angle);
    const Image bar =
        plan ? WindowBar(tex, L.materials[mi].black, L.materials[mi].white, g.frame_w, g.frame_h,
                         8.f)
             : WindowBar(tex, L.materials[mi].black, L.materials[mi].white, g.w, g.h, 4.f);
    out = RotateCcw(bar, g.rot);
    return !out.Empty();
}

bool SlopeImage(Romfs& rf, int angle, bool plan, Image& out) {
    const auto layout = LoadLayout(rf, "LMapIconSlope");
    if (!layout)
        return false;
    const Bflyt& L = layout->Tree();
    const int mi = L.Material(plan ? "P_Frame_00" : "P_Icon_00");
    Image tex;
    if (mi < 0 || L.materials[mi].tex.empty() || !Texture(*layout, L.materials[mi].tex[0], tex))
        return false;
    const Image pane =
        plan ? PaneImage(tex, L.materials[mi].black, L.materials[mi].white, 40.f, 24.f)
             : PaneImage(tex, L.materials[mi].black, L.materials[mi].white, 32.f, 16.f);
    out = RotateCcw(pane, SlopeRot(angle));
    return !out.Empty();
}
} // namespace

// ---- image keys ---------------------------------------------------------------------------
namespace {
// map/ressel?n=<percent-encoded UTF-8 name>&f=<ui|ui_kr|ui_cn|ui_tw>&st=<house level>: the selected
// resident's highlight as ONE kResSelBox square picture centred on the house (the page draws it as
// a map marker, so it pans / zooms with the island at a constant size): the page's former widgets
// in the same art and placement: glow (MenuDevice Maru192blur^s #ffbb00e0, 110 px), the villager
// house icon at 1.3x its map size, the name plate (LMapIconName MapNameBase^s #fff1c3, its
// #04554f50 copy 4 px lower) whose top is 94 px above the house, the name (Seurat-B or the
// language's SystemB CJK face, 26 px em, #252422) centred 63 px above the house. The plate is 200
// px wide, wider for a long name (name + 60 px, the composer rule), at most the box.
constexpr int kResSelBox = 320;

std::string PercentDecode(std::string_view v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size() &&
            std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
            std::isxdigit(
                static_cast<unsigned char>(v[i + 2]))) { // exactly "%XX" (no sign, no "%4G")
            int n = 0;
            if (std::from_chars(v.data() + i + 1, v.data() + i + 3, n, 16).ec == std::errc{}) {
                o.push_back(static_cast<char>(n));
                i += 2;
                continue;
            }
        }
        o.push_back(v[i]);
    }
    return o;
}

// the name plate (LMapIconName MapNameBase^s #fff1c3 over its #04554f50 copy 4 px lower), 60 px
// tall with its top at `top`, 200 px wide or the name + 60 px, the name centred in it
void NamePlate(Romfs& rf, Image& out, const Image& txt, double top) {
    const int c = out.w / 2;
    const int pw = std::clamp(txt.Empty() ? 200 : txt.w + 60, 200, out.w - 4), ph = 60;
    if (const auto lay = LoadLayout(rf, "LMapIconName")) {
        if (const auto t = lay->Texture("MapNameBase^s")) {
            const Image base = draw::Resize(*t, pw, ph);
            const double py = top + ph / 2.0;
            draw::PasteC(out, draw::MaskTint(base, Rgba{0x04, 0x55, 0x4f, 0x50}), c, py + 4);
            draw::PasteC(out, draw::MaskTint(base, Rgba{0xff, 0xf1, 0xc3, 0xff}), c, py);
        }
    }
    if (!txt.Empty())
        draw::PasteC(out, txt, c, top + 31);
}

std::mutex font_mu;
Fonts* shared_fonts = nullptr;    ///< the module's (MapSetFonts)
std::unique_ptr<Fonts> own_fonts; ///< without one (tests): a private instance

bool NameText(Romfs& rf, std::string_view face, const std::string& name, Image& txt) {
    if (name.empty())
        return false;
    Fonts* fonts;
    {
        std::lock_guard lk{font_mu};
        if (!shared_fonts && !own_fonts)
            own_fonts = std::make_unique<Fonts>(rf);
        fonts = shared_fonts ? shared_fonts : own_fonts.get();
    }
    return fonts->Text(face, name, 26, Rgba{0x25, 0x24, 0x22, 0xff}, txt); // Fonts locks itself
}

// map/facsel?k=<icon kind>&n=<name>&f=<face>&w=&h=[&side=&stage=]: a selected facility as the
// game's Map app shows it (SetTarget 0x2e175f0): the icon in its Attention colours (the game blinks
// them on a 30-frame loop; the runtime has no per-marker blink, so the picture keeps them) at its
// map size w x h, and the name balloon (STR_Common name by facility and stage, 0x2e21adc) above it,
// in one kResSelBox square centred on the icon. r7 mapfix: &part=1 the highlighted icon alone (w x
// h = its displayed size: the page shows one per zoom band), &part=2 the name plate alone, placed
// for the icon at &cap=<percent> of w x h (the largest band, so the plate never covers the icon at
// any band); no part = both (the old picture).
bool FacilitySelImage(Romfs& rf, std::string_view key, Image& out) {
    ArtKey q;
    if (!ArtKey::Parse(key, q))
        return false;
    const std::string kind = q.Str("k"), name = PercentDecode(q.Str("n")), face = q.Str("f");
    const int w = q.Int("w", 0), h = q.Int("h", 0),
              stage = q.Int("stage", -1); // &side= is not used
    const int part = q.Int("part", 0), cap = std::clamp(q.Int("cap", 100), 0, 400);
    const IconDef* def = nullptr;
    for (const auto& d : kIcons)
        if (d.key == kind)
            def = &d;
    if (!def || w <= 0 || h <= 0 || w > kResSelBox || h > kResSelBox)
        return false;
    out = Image{kResSelBox, kResSelBox};
    const int c = kResSelBox / 2;
    if (part != 2) {
        static constexpr Rgba kAttnBlack{233, 62, 6, 255}, kAttnWhite{255, 209, 19, 255};
        Image icon;
        if (!MapIconImageColours(rf, def->kind,
                                 def->kind == MapIconKind::Hotel ? HotelTypeFrame(stage) : -1,
                                 &kAttnBlack, &kAttnWhite, icon))
            return false;
        if (def->kind == MapIconKind::Hotel && part == 0) {
            Image composite;
            // the icon alone on the pier canvas: the pier is drawn in the island pictures
            if (HotelWithPier(rf, icon, -1, stage >= 2, composite))
                icon = std::move(composite);
        }
        draw::PasteC(out, draw::SizeTo(icon, 0, w, h), c, c);
    }
    if (part == 1)
        return true;
    // the icon's own height (the hotel's is the 64-unit icon inside its pier canvas unless part=2,
    // whose w x h are the icon's)
    const double ih =
        (def->kind == MapIconKind::Hotel && part == 0 ? h * 64.0 / kHotelH : h) * cap / 100.0;
    Image txt;
    NameText(rf, face.empty() ? "ui" : face, name, txt);
    NamePlate(rf, out, txt, c - ih / 2 - 6 - 60);
    return true;
}

bool ResidentSelImage(Romfs& rf, std::string_view key, Image& out) {
    ArtKey q;
    if (!ArtKey::Parse(key, q))
        return false;
    const std::string name = PercentDecode(q.Str("n")), f = q.Str("f");
    const std::string face = f == "ui_kr" || f == "ui_cn" || f == "ui_tw" ? f : "ui";
    const int stage = q.Int("st", -1), player_house = q.Int("h", 0);
    // r7 mapfix: &part=1 glow + house alone at &sc=<percent> (one picture per zoom band), &part=2
    // the plate alone, placed for the house at &cap=<percent> (the largest band: the plate never
    // covers it)
    const int part = q.Int("part", 0);
    const double sc = std::clamp(q.Int("sc", 100), 50, 300) / 100.0;
    const double cap = std::clamp(q.Int("cap", 100), 0, 300) / 100.0;
    const int c = kResSelBox / 2;
    out = Image{kResSelBox, kResSelBox};
    constexpr double k = kZoomPpu / 0.8 * 1.3;
    if (part != 2) {
        // glow
        if (const auto lay = LoadLayout(rf, "MenuDevice")) {
            if (const auto t = lay->Texture("Maru192blur^s")) {
                const int g = static_cast<int>(std::lround(110 * sc));
                draw::PasteC(out,
                             draw::MaskTint(draw::Resize(*t, g, g), Rgba{0xff, 0xff, 0xff, 0xe0}),
                             c, c);
            }
        }
        // the house at 1.3x its map size (42 x 36 layout units x 1 / 0.8 world units x kZoomPpu; a
        // player's house 52 x 36)
        Image house;
        if (player_house
                ? MapIconImage(rf, MapIconKind::PlayerHouse, house)
                : MapIconImageStage(rf, MapIconKind::NpcHouse, std::clamp(stage, -1, 2), house))
            draw::PasteC(out,
                         draw::SizeTo(house, 0, std::round((player_house ? 52 : 42) * k * sc),
                                      std::round(36 * k * sc)),
                         c, c);
    }
    if (part == 1)
        return true;
    // name plate + name: its top 94 px above the house centre at 1x (house half height 36 k / 2),
    // moved up by the extra half height of the house at `cap`
    Image txt;
    NameText(rf, face, name, txt);
    NamePlate(rf, out, txt, c - 94 - std::round(36 * k * (cap - 1.0) / 2.0));
    return true;
}

struct IslandSlot {
    std::mutex mu;
    bool have = false;
    uint32_t rev = 0;
    IslandInput in;
    bool have_base = false; // zoom page base view (z = 1)
    uint32_t base_rev = 0;
    Image base;
};
IslandSlot& Slot() {
    static IslandSlot s;
    return s;
}
} // namespace

void ZoomDrawHotel(Romfs& rf, const IslandInput& in, const MapView& v, Image& im) {
    for (const auto& ic : MapIcons(in, v, &rf)) {
        if (ic.kind != MapIconKind::Hotel || ic.key.empty())
            continue;
        const int w = static_cast<int>(std::lround(ic.w)), h = static_cast<int>(std::lround(ic.h));
        const int x0 = static_cast<int>(std::lround(ic.x - ic.w / 2)),
                  y0 = static_cast<int>(std::lround(ic.y - ic.h / 2));
        if (w <= 0 || h <= 0 || x0 >= im.w || y0 >= im.h || x0 + w <= 0 || y0 + h <= 0)
            continue;
        Image art;
        if (!MapLoadImage(rf,
                          ic.key + (ic.key.find('?') != std::string::npos ? "&" : "?") +
                              "part=2&w=" + std::to_string(w) + "&h=" + std::to_string(h),
                          art))
            continue;
        draw::Composite(im, art, x0, y0);
    }
}

void MapSetFonts(Fonts* fonts) {
    {
        std::lock_guard lk{font_mu};
        shared_fonts = fonts;
    }
    auto& s = Slot();
    std::lock_guard lk{s.mu};
    s.have = s.have_base = false;
    s.rev = s.base_rev = 0;
    s.in = {};
    s.base = {};
}

void MapSubmitIsland(uint32_t rev, IslandInput in) {
    auto& s = Slot();
    std::lock_guard lk{s.mu};
    s.rev = rev;
    s.in = std::move(in);
    s.have = true;
}

bool MapLoadImage(Romfs& rf, std::string_view key, Image& out) {
    if (key.starts_with("module:"))
        key.remove_prefix(7);
    if (key.starts_with("acnh/"))
        key.remove_prefix(5);
    if (key.starts_with("map/icon/")) {
        key.remove_prefix(9);
        // ?rot=90: odd-Angle bridges / inclines (StructureList Angle; the reference renderer's
        // PIL rotate(90) = counter-clockwise). ?s= / ?w=&h=: display size (draw::SizeTo, the
        // linear-light area average), applied after the rotation.
        ArtKey q;
        if (!ArtKey::Parse(key, q))
            return false;
        key = q.path;
        const int pier_side = q.Int("side", -1), pattern = q.Int("p", -1), angle = q.Int("a", -1);
        const int plan = q.Int("plan", 0), stage = q.Int("stage", -1), part = q.Int("part", 0),
                  rot = q.Int("rot", 0);
        const double s = q.Int("s", 0), w = q.Int("w", 0), h = q.Int("h", 0);
        if (rot % 90 != 0 || s < 0 || w < 0 || h < 0 || s > 1024 || w > 1024 || h > 1024 ||
            stage > 9 || pattern > 5)
            return false;
        for (const auto& d : kIcons)
            if (d.key == key) {
                LoadIconTables(rf);
                if (d.kind == MapIconKind::Bridge && (pattern >= 0 || angle >= 0 || plan)) {
                    // ?p=<BridgePattern 0..5>&a=<Angle>&plan=1: LMapIconBridge at its State frame
                    if (!BridgeImage(rf, pattern, std::max(angle, 0), plan != 0, out))
                        return false;
                } else if (d.kind == MapIconKind::Slope && (angle >= 0 || plan)) {
                    if (!SlopeImage(rf, std::max(angle, 0), plan != 0, out))
                        return false;
                } else if (!MapIconImageStage(
                               rf, d.kind,
                               d.kind == MapIconKind::Hotel ? HotelTypeFrame(stage) : stage, out)) {
                    return false;
                }
                // the hotel: &part=1 the icon alone (a zoom-band map marker), &part=2 the pier art
                // alone (drawn into the island pictures at map scale), else the pier composite
                if (d.kind == MapIconKind::Hotel && part != 1) {
                    if (part == 2)
                        std::fill(out.rgba.begin(), out.rgba.end(), uint8_t{0});
                    // SE vs SW from the game's code: the Map app (setup 0x2e19ff0, 0x2e1c3c4)
                    // asks 0x12871c0 for the island's jetty coast acre: FieldOutsideParts 137/138
                    // (FldOutSEBridge01/00) -> SE art, 135/136 (FldOutSWBridge01/00) -> SW art,
                    // none -> no pier. The field deck models pair the same ids (StrcHotelA00Deck*).
                    int side = pier_side; // ?side= (the page's choice by map.hotel.side)
                    if (side < 0) {
                        auto& sl = Slot();
                        std::lock_guard lk{sl.mu};
                        side = MapPierSide(sl.in.field_blocks);
                    }
                    Image composite;
                    if (HotelWithPier(rf, out, side <= 1 ? side : -1, stage >= 2, composite))
                        out = std::move(composite);
                }
                for (int r = ((rot / 90) % 4 + 4) % 4; r > 0; --r)
                    out = draw::Rotate90(out);
                out = draw::SizeTo(out, s, w, h);
                return !out.Empty();
            }
        return false;
    }
    if (key.starts_with("map/ressel"))
        return ResidentSelImage(rf, key, out);
    if (key.starts_with("map/facsel"))
        return FacilitySelImage(rf, key, out);
    if (key == "map/none") { // a transparent 2 x 2 picture (tap targets drawn by the map widget)
        out = Image{2, 2};
        return true;
    }
    if (!key.starts_with("map/island/"))
        return false;
    key.remove_prefix(11);
    ArtKey q;
    if (!ArtKey::Parse(key, q))
        return false;
    const std::string_view rev_text = q.path;
    uint32_t rev = 0;
    if (std::from_chars(rev_text.data(), rev_text.data() + rev_text.size(), rev).ec != std::errc{})
        return false;
    // the zoom page's pictures only (the whole-island picture without ?z= had no reader left)
    const int z = q.Int("z", 0), t = q.Int("t", 0);
    if (z < 1)
        return false;
    // ?z=1 base (804 x 693 px of the island box, cached), ?z=2|3&t=<0..15> a tile
    auto& s = Slot();
    IslandInput zin;
    {
        std::lock_guard lk{s.mu};
        if (!s.have)
            return false;
        if (z == 1 && s.have_base && s.base_rev == s.rev && rev <= s.rev) {
            out = s.base;
            return true;
        }
        zin = s.in;
        rev = s.rev;
    }
    if (!GenerateZoomImage(rf, zin, z, t, out))
        return false;
    if (z == 1) {
        std::lock_guard lk{s.mu};
        s.base = out;
        s.base_rev = rev;
        s.have_base = true;
    }
    return true;
}

} // namespace acnh
