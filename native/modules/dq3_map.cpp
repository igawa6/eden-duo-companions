// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Field Map page of the Dragon Quest III HD-2D Remake companion. Sources and rules: dq3_map.h.

#include "dq3_map.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "dq3_art.h"
#include "dq3_cooked.h"
#include "dq3_font.h"
#include "dq3_info.h"
#include "dq3_layout.h"
#include "dq3_sprites.h"
#include "dq3_tables.h"

namespace dq3 {
namespace {

struct ProgressValue {
    std::string_view name;
    u32 value;
};

struct WorldVariant { std::string_view id; int group, index; };

#include "dq3_map_pins.inc"
struct PlaceArt {
    std::string_view memory; ///< GOP_Memory row
    int first;               ///< its DAYTIME still in NoMapTexturePinsTable (EVENING, NIGHT follow)
    std::string_view region; ///< GOP_Text_Map region line
};
#include "dq3_nomap_pins.inc"

static_assert(std::size(MapUiPinsTable) == static_cast<std::size_t>(MapUi::Count));

using cooked::Le32;
using s64 = std::int64_t;
using detail::Get;

// ------------------------------------------------------------------------------ code pins
// Words copied from the 1.1.0.0 main image (research tools/dis-annotate.py listings in
// runs/slice3-map/re/). Every word must match before any Map page rule is used.
const std::vector<CodePin>& MapPins() {
    static const std::vector<CodePin> pins{
        // Native field and Misc menu selections used by the bounded healing input driver.
        {0xba6398, {0xb9409668, 0x7100151f, 0x540017a8}},
        {0xba00e8, {0xb9409408, 0x3902f01f, 0x37f82a68, 0xb940c809}},
        // Native formation confirm reads selected +b0 and cursor +94, then swaps through the game.
        {0xb9e908, {0xb940b014, 0xaa0003f3, 0xb9409408, 0x37f80234, 0x6b08029f, 0x540001c0}},
        // GetFacilityIconIndex: (type - 1) & 0xff <= 13 -> table[type - 1] (+15 unless w1)
        {0xce4130, {0x51000408, 0x12001d09, 0x7100353f, 0x54000148, 0xd001e129, 0x91042129, 0x1e25d001,
                    0x93401d08, 0xbc687920, 0x7200003f, 0x1e212801, 0x1e211c00, 0xd65f03c0, 0x1e3e1000}},
        // MapTimeHour: ldr s0,[x0,#0x30]; (int) s0 / 86400 remainder / 3600
        {0xa213c8, {0xbd403000, 0x5288a0e9, 0x72b845c9, 0x128eeecc, 0x1e380008, 0x9b297d09, 0xd360fd29,
                    0x0b080129, 0x13107d2a, 0x0b497d40, 0x528a3009, 0x72a00029, 0x1b09a008, 0x529678a9,
                    0x72b23449, 0x9b297d09, 0xd360fd29, 0x0b080129, 0x130b7d2a, 0x0b497d49, 0x5291112a,
                    0x72b1110a}},
        // IsDay: GetMapTimeFrame() != 3 (NIGHT)
        {0xbe5db8, {0xa9bf7bfd, 0x910003fd, 0x97f880c3, 0x12001c08, 0x71000d1f, 0x1a9f07e0, 0xa8c17bfd,
                    0xd65f03c0}},
        // Map screen time symbol: param = GetMapTimeFrame() == NIGHT ? 1 : 0
        {0xbd96e8, {0x97f8b279, 0x12001c08, 0x2f00e400, 0x1e2e1001, 0x71000d1f, 0x90025328, 0xf9405660,
                    0xf9445101, 0x1e200c20, 0x940420b9}},
        // Town / dungeon marker: (X - O.x) * H / S.x, (Y - O.y) * W / S.y, clamped, v = H - ...
        {0xbfa5b4, {0xbd413aa0, 0x1e203820, 0xbd413ea2, 0x5e0c0421, 0xf9403e68, 0xbd4146a4, 0xbd411503,
                    0x1e223821, 0xbd411102, 0xbd4142a5, 0x1e251865, 0x1e250800, 0x1e241844, 0x1e240821,
                    0x1e203864, 0x1e222020, 0x1e224c20, 0x1e202028, 0x2f00e401, 0x1e204c20, 0x1e232080,
                    0x1e234c82, 0x1e202088, 0x1e224c21}},
        // Interiors: row MapSpotIconIndex (+0x134) != 0 -> spot position [index - 1]
        {0xbfa204, {0xb9813415, 0x34000ad5, 0x97ffaeeb, 0x6f00e400, 0x12000001, 0x910043e2, 0x910003e3,
                    0xaa1403e0, 0xf9000bff, 0xf90003ff, 0xfd000fe0, 0xfd0007e0, 0x9403a7cf, 0xb9401be8,
                    0x2a1f03f4, 0x6b0802bf, 0x540012ac, 0xb9400be8, 0x6b0802bf, 0x5400124c, 0xf94003e8,
                    0x8b150d08, 0xf85f8108, 0xf9009a68, 0x97ff74cb}},
        // Spot row: +0x124 (day) or +0x12c (night)
        {0xce41d8, {0x91049008, 0x720002bf, 0x910023e0, 0x91002109, 0xaa1403e1, 0xaa1303e2, 0x9a891108,
                    0xf9400108, 0xf90007e8}},
        // IsTreasure: trigger +0x105 - 11 < 4
        {0x845a00, {0x39441408, 0x51002d08, 0x7100111f, 0x1a9f27e0, 0xd65f03c0}},
        // the trigger's flag +0x102 (passed to SetFlag)
        {0x845958, {0x79420675}},
        // NicolaDataAssetCommon = [G+0x4b8]
        {0x8e4180, {0xf9425c14}},
        // player icon angle = root world rotator Yaw (GetRotation 0x107d2e8 -> s2)
        {0x107d2e8, {0x2d408002, 0xbd400001, 0xd65f03c0}},
        {0xbfa518, {0x94120b74, 0x1e204040, 0xaa1503e0, 0x9470b43b}},
        // world extent: underground size +0x190, field size +0x178, counts +0x188 / +0x170,
        // origins +0x180 / +0x168
        {0xbd7a58, {0x91064008, 0x2d402508}},
        {0xbd7ab0, {0x97f0bdaa, 0x9105e008, 0x2d402508}},
        {0xbd7b3c, {0x97f0bd87, 0x91062008, 0x14000019}},
        {0xbd7b94, {0x97f0bd71, 0x9105c008, 0x14000003}},
        {0xbd7ccc, {0x97f0bd23, 0x91060008, 0x14000017}},
        {0xbd7d1c, {0x97f0bd0f, 0x9105a008, 0x14000003}},
        // FieldSymbolActor::GetMapIconTag used by FindPlayerStartByTag fallback category 0x1a.
        {0x88d1e8, {0x910a7008, 0xf9400100, 0xd65f03c0}},
        // Acquired world destinations, actor registry, anchor tag, icon cell and map variants.
        {0x876ed4, {0x9108c001, 0xaa0803e0, 0xa9007d1f, 0xaa0803f3}},
        {0x876eec, {0x91090281, 0xaa1303e0}},
        {0xbe7fb4, {0xf00251e9, 0x910ba129, 0xf9400129, 0xb4000529, 0xf9422534, 0xb40004f4}},
        {0x668398, {0xf9401408, 0x92401c29, 0xd37ced29, 0xf8696908, 0xf9400115, 0xb5000155}},
        {0xbe8050, {0x91091268, 0xf9400108, 0xeb15011f}},
        {0xbff188, {0x39458008, 0x394cd269, 0x6b09011f, 0x54fffde1}},
        {0xce40e0, {0x12001c08, 0x7100311f, 0x54000061, 0x1e3e1000, 0xd65f03c0, 0x1e230100, 0x1e221001, 0x7200003f, 0x1e210800, 0x1e2e1001, 0x1e212801, 0x1e201c20, 0x1e201001, 0x7200005f, 0x1e212801, 0x1e211c00, 0xd65f03c0}},
        {0x849e90, {0x528008e1, 0x9104a115, 0xaa1503e0, 0x9400b5b5, 0x7200001f, 0x52800901, 0xaa1503e0, 0x1a940694, 0x9400b5b0, 0x7200001f, 0x52800068, 0x52800921, 0xaa1503e0, 0x1a941114, 0x9400b5aa, 0x7200001f, 0x52800088, 0x52800961, 0xaa1503e0, 0x1a941114, 0x9400b5a4, 0x7200001f, 0x528000a8, 0x528009a1, 0xaa1503e0, 0x1a941114, 0x9400b59e, 0x7200001f, 0x528000c8, 0x1a941114}},
        {0xa17018, {0x12001c08, 0x2a0003f3, 0x51000509, 0x71000d3f, 0x540001c2, 0x2a1303e0, 0x97f8ccaa, 0x12000014, 0x910003e8, 0x2a1303e0, 0x97ffffa4, 0xb9400be8, 0x6b14011f, 0xf94003e0, 0x5400024d, 0xf8745813, 0x92607e74, 0x14000014}},
        // world map material (UIMap2Menu, 0xbfd8b8 / 0xbfd9b8): TextureMap = progress 0x88
        // (MAIN_ZOMACASTLE_DefeatZoma, 0x84a06c) ? UndergroundAfter (+0x130) : Underground (+0x128);
        // MapChange (the FrontWorldOverlay blend) = 0x84a2d8(1) = progress 0x5b (MAIN_VOLCANO_ThrowGaiaSword)
        {0x84a06c, {0xa9bf7bfd, 0x910003fd, 0x9400a8c2, 0xf9400008, 0x52801101, 0x9104a100, 0xa8c17bfd,
                    0x1400b53a}},
        {0x84a2d8, {0x51000408, 0x12001d09, 0x7100093f, 0x54000228, 0xa9be7bfd, 0xf9000bf3, 0x910003fd,
                    0x92401d08, 0xd2800b69, 0xf2a00429, 0xd37ced08, 0xf2c00c29, 0x9ac82533, 0x9400a81c,
                    0xf9400008, 0x2a1303e1, 0x9104a100, 0xf9400bf3, 0xa8c27bfd, 0x1400b493}},
        {0xbfd8b8, {0x97f131ed, 0x2a0003f6, 0x97ff6734, 0x720002df, 0x52802508, 0x52802609, 0x9a881128,
                    0xb001dd01, 0x910d4821, 0xf8686816}},
        {0xbfd9b8, {0x52800020, 0x97f13247, 0x2f00e400, 0x7200001f, 0x9001de21, 0x912f9021, 0x910023e0,
                    0x1e201d28}},
    };
    return pins;
}
constexpr std::array<int, 14> FacilityTable = {0, 2, 3, 5, 4, 1, 6, 10, 11, 7, 8, 9, 12, 13};

constexpr std::string_view SpotTypeNames[16] = {
    "NONE", "CHURCH", "SHOP_WEAPON", "SHOP_ARMOR", "SHOP_ITEM", "SHOP_EQUIP", "INN", "BAR",
    "ARENA", "DAMA_TEMPLE", "MEDAL_EXCHAGE", "WELL", "TABI_NO_TOBIRA", "MONSTER_ZOO", "BANK", "HOUSE"};
constexpr u8 SpotHouse = 15;

// Live layout (UE4 reflection / native offsets, verified live: runs/slice3-map/RESULT.md)
using layout::ObjClass, layout::ObjName, layout::PProgress, layout::WorldSubsystems;
constexpr u64 GDataCommon = 0x4b8;
constexpr u64 PGold = 0x28, PSearchBits = 0x128, PVisited = 0x250;
constexpr u64 LlmCurrent = 0x1b4, LlmLoadingInfo = 0x38, LsdPackage = 0x50, LsdTranslation = 0x90,
              LsdLoaded = 0x128, LevelActors = 0x98;
constexpr u64 PawnRoot = 0x130, RootLoc = 0x11c, RootRot = 0x128; // (pawn facing: +0x540)
constexpr u64 ActorTrigger = 0x258, TrigFlag = 0x102, TrigType = 0x105;
constexpr u64 TimeSeconds = 0x30;

std::mutex map_mutex;
std::shared_ptr<const MapData> map_cache;

bool Ends(std::string_view s, std::string_view t) {
    return s.size() >= t.size() && s.substr(s.size() - t.size()) == t;
}

std::string Hex4(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%x", v);
    return b;
}

} // namespace

// ------------------------------------------------------------------------------ pins / data
std::span<const MapTexturePin> MapImagePins() {
    return MapImagePinsTable;
}
int FindMapImage(std::string_view name) {
    const auto pins = MapImagePins();
    const auto it = std::lower_bound(pins.begin(), pins.end(), name,
                                     [](const MapTexturePin& p, std::string_view n) { return p.name < n; });
    if (it == pins.end() || it->name != name)
        return -1;
    return static_cast<int>(it - pins.begin());
}
const MapTexturePin& MapUiPin(MapUi id) {
    return MapUiPinsTable[static_cast<std::size_t>(id)];
}

std::string_view SpotLabel(u8 type) {
    static constexpr std::string_view labels[16] = {
        "", "Church", "Weapons", "Armour", "Items", "Equipment", "Inn", "Tavern", "Arena", "Abbey",
        "Mini Medals", "Well", "Teleportal", "Monster Menagerie", "Bank", ""};
    return type < 16 ? labels[type] : std::string_view{};
}

int FacilityCell(const MapRoots& roots, u8 type, bool large) {
    if (!roots.ok || type < 1 || type > 14)
        return -1;
    const int c = roots.facility_cell[static_cast<std::size_t>(type - 1)];
    return large ? c : c + 15;
}

std::optional<MapData> BuildMapData(std::span<const std::span<const u8>, 10> m, std::string* why) {
    const auto bad = [why](const std::string& r) -> std::optional<MapData> {
        if (why)
            *why = r;
        return std::nullopt;
    };
    static constexpr std::string_view RowCols[] = {
        "PrefixId", "NameTextID", "TownTelopTextID", "MapImage", "MapOriginalPos", "MapSize",
        "MapFloorGroup", "MapFloorSortNum", "MapSpotIconDayTimeID", "MapSpotIconNightID",
        "MapSpotIconIndex", "IsUnderground", "FieldSymbolId", "MemoryID"};
    const auto rows = ParseDataTable(m[0], m[1], nullptr, RowCols);
    const auto spots = ParseDataTable(m[2], m[3]);
    static constexpr std::string_view HourCols[] = {"InGameHour", "MapTimeFrame"};
    const auto hours = ParseDataTable(m[4], m[5], nullptr, HourCols);
    static constexpr std::string_view NounCols[] = {"ListNoun"};
    const auto nouns = ParseDataTable(
        m[6], m[7], [](std::string_view r) { return r.starts_with("TEXT_NOUN_ENGLISH_Map_"); }, NounCols);
    static constexpr std::string_view TextCols[] = {"Text"};
    const auto menu = ParseDataTable(
        m[8], m[9], {}, TextCols);
    if (!rows || !spots || !hours || !nouns || !menu)
        return bad(std::string{"table "} + (!rows ? "MapList" : !spots ? "SpotIcon" : !hours ? "Hour" : !nouns ? "Noun" : "Text_Map"));
    const auto text = [](const TableRow& r, std::string_view c) -> std::string {
        const auto it = r.find(c);
        return it == r.end() ? std::string{} : it->second.s;
    };
    const auto num = [](const TableRow& r, std::string_view c) -> s64 {
        const auto it = r.find(c);
        if (it == r.end())
            return 0;
        return it->second.kind == TableValue::Kind::Float ? static_cast<s64>(it->second.f) : it->second.i;
    };
    const auto vec = [](const TableRow& r, std::string_view c, float& a, float& b) {
        const auto it = r.find(c);
        if (it == r.end() || it->second.kind != TableValue::Kind::Vec2)
            return false;
        a = static_cast<float>(it->second.f);
        b = static_cast<float>(it->second.f2);
        return true;
    };
    MapData d;
    for (const auto& [id, r] : *rows) {
        MapRow row;
        row.prefix = text(r, "PrefixId");
        row.field_symbol = text(r, "FieldSymbolId");
        row.name_text = text(r, "NameTextID");
        row.telop_text = text(r, "TownTelopTextID");
        const std::string image = text(r, "MapImage");
        if (image.starts_with("/Game/")) {
            const auto dot = image.rfind('.');
            if (dot != std::string::npos)
                row.image = FindMapImage(std::string_view{image}.substr(dot + 1));
        }
        if (!vec(r, "MapOriginalPos", row.ox, row.oy) || !vec(r, "MapSize", row.sx, row.sy))
            return bad("row " + id + " vectors");
        row.floor_group = static_cast<int>(num(r, "MapFloorGroup"));
        row.floor_sort = static_cast<int>(num(r, "MapFloorSortNum"));
        row.spot_day = text(r, "MapSpotIconDayTimeID");
        row.spot_night = text(r, "MapSpotIconNightID");
        row.spot_index = static_cast<int>(num(r, "MapSpotIconIndex"));
        row.underground = num(r, "IsUnderground") != 0;
        row.memory = text(r, "MemoryID");
        d.rows.emplace(id, std::move(row));
    }
    for (const auto& [id, r] : *spots) {
        std::array<SpotEntry, 12> entries{};
        for (int k = 0; k < 12; ++k) {
            char col[40];
            std::snprintf(col, sizeof(col), "SpotType_%02d", k + 1);
            const std::string type = text(r, col);
            SpotEntry e;
            for (u8 t = 0; t < 16; ++t)
                if (type == "EMapSpotType::" + std::string{SpotTypeNames[t]})
                    e.type = t;
            const auto flag = [&](const char* fmt, u32& out) {
                char c[48];
                std::snprintf(c, sizeof(c), fmt, k + 1);
                const std::string name = text(r, c);
                for (const auto& p : ProgressValuesTable)
                    if (p.name == name) {
                        out = p.value;
                        return true;
                    }
                return name.empty();
            };
            std::snprintf(col, sizeof(col), "DispPositionX_%02d", k + 1);
            const auto ix = r.find(col);
            std::snprintf(col, sizeof(col), "DispPositionY_%02d", k + 1);
            const auto iy = r.find(col);
            if (ix == r.end() || iy == r.end() || ix->second.kind != TableValue::Kind::Float ||
                iy->second.kind != TableValue::Kind::Float)
                return bad("spot " + id + " position");
            e.x = static_cast<float>(ix->second.f);
            e.y = static_cast<float>(iy->second.f);
            // "Profress" is the table's own spelling; an unknown progress name hides the entry
            if (!flag("DispProfressFlagName_%02d", e.disp) || !flag("HideProgressFlagName_%02d", e.hide))
                e.type = 0;
            entries[static_cast<std::size_t>(k)] = e;
        }
        d.spots.emplace(id, entries);
    }
    for (int h = 0; h < 24; ++h) {
        char id[24];
        std::snprintf(id, sizeof(id), "HOUR_Hour%02d", h);
        const auto it = hours->find(id);
        if (it == hours->end())
            return bad(std::string{"hour "} + id);
        const std::string f = text(it->second, "MapTimeFrame");
        d.hour_frame[static_cast<std::size_t>(h)] = f == "EMapTimeFrame::DAYTIME" ? 1
                                                    : f == "EMapTimeFrame::EVENING" ? 2
                                                    : f == "EMapTimeFrame::NIGHT"   ? 3
                                                                                    : 0;
        if (num(it->second, "InGameHour") != h || d.hour_frame[static_cast<std::size_t>(h)] == 0)
            return bad(std::string{"hour value "} + id);
    }
    constexpr std::string_view Prefix = "TEXT_NOUN_ENGLISH_";
    for (const auto& [id, r] : *nouns) {
        const auto it = r.find("ListNoun");
        if (it != r.end() && it->second.kind == TableValue::Kind::Str)
            d.nouns.emplace("Txt_" + id.substr(Prefix.size()), it->second.u);
    }
    for (const auto& [id, r] : *menu) {
        const auto it = r.find("Text");
        if (it != r.end() && it->second.kind == TableValue::Kind::Str)
            d.menu.emplace(id, it->second.u);
    }
    if (d.rows.size() < 500 || d.spots.size() < 100 || d.nouns.size() < 100 ||
        !d.Row("MAPLIST_C01F0101") || !d.Noun("Txt_Map_Place_Name_Aliahan") ||
        !d.menu.contains("Txt_Map_Menu_World_Map"))
        return bad("sanity: rows " + std::to_string(d.rows.size()) + " spots " + std::to_string(d.spots.size()) +
                   " nouns " + std::to_string(d.nouns.size()) + " menu " + std::to_string(d.menu.size()));
    return d;
}

std::shared_ptr<const MapData> LoadMapData(const RangeReader& read, std::string* why) {
    {
        std::scoped_lock lock{map_mutex};
        if (map_cache)
            return map_cache;
    }
    const PakMember* pins[16] = {&GopMapListUasset, &GopMapListUexp,       &GopMap_SpotIconUasset,
                                 &GopMap_SpotIconUexp, &GopHourUasset,     &GopHourUexp,
                                 &NounAssetMember(),  &NounExpMember(),    &GopText_MapUasset,
                                 &GopText_MapUexp,    &GopRura_RiremitoUasset, &GopRura_RiremitoUexp,
                                 &GopMapGuideDataUasset, &GopMapGuideDataUexp, &GopMemoryUasset, &GopMemoryUexp};
    const auto bytes = ReadMembers(read, pins, why);
    if (!bytes)
        return nullptr;
    std::array<std::span<const u8>, 10> spans;
    for (std::size_t i = 0; i < spans.size(); ++i)
        spans[i] = (*bytes)[i];
    std::string reason;
    auto built = BuildMapData(spans, &reason);
    if (!built) {
        if (why)
            *why = "map table parse: " + reason;
        return nullptr;
    }
    const auto& ra = (*bytes)[10];
    const auto& re = (*bytes)[11];
    static constexpr std::string_view cols[] = {"FloorID", "MapIconPlayerStartTag", "MapLocationIconType", "AreaNameId"};
    const auto destinations = ParseDataTable(ra, re, nullptr, cols);
    if (!destinations) {
        if (why)
            *why = "destinations table parse";
        return nullptr;
    }
    for (const auto& [id, row] : *destinations) {
        DestinationRow dest;
        if (auto i = row.find("FloorID"); i != row.end()) dest.floor = i->second.s;
        if (auto i = row.find("MapIconPlayerStartTag"); i != row.end()) dest.tag = i->second.s;
        if (auto i = row.find("AreaNameId"); i != row.end()) dest.area = i->second.s;
        if (auto i = row.find("MapLocationIconType"); i != row.end())
            for (const auto& t : WorldIconTypes) if (t.name == i->second.s) dest.type = static_cast<int>(t.value);
        built->destinations.emplace(id, std::move(dest));
    }
    // GOP_Memory: MemoryID -> AreaNameId (the place of a map; the no-map place card)
    static constexpr std::string_view mem_cols[] = {"AreaNameId"};
    const auto memory = ParseDataTable((*bytes)[14], (*bytes)[15], nullptr, mem_cols);
    if (!memory || !memory->contains("MEMORY_ALIAHAN")) {
        if (why)
            *why = "memory table parse";
        return nullptr;
    }
    for (const auto& [id, row] : *memory)
        if (auto i = row.find("AreaNameId"); i != row.end())
            built->memory_area.emplace(id, i->second.s);
    const auto guides = ParseDataTable((*bytes)[12], (*bytes)[13]);
    if (!guides) {
        if (why)
            *why = "map guide table parse";
        return nullptr;
    }
    for (const auto& [id,row] : *guides) {
        MapData::Guide guide;
        if (const auto it=row.find("MessageTextID"); it!=row.end()) guide.message=it->second.s;
        for (int k=0;k<30;++k) {
            char col[48];
            std::snprintf(col,sizeof(col),"MarkerIDTownDungeon%02d",k);
            if (const auto it=row.find(col); it!=row.end() && it->second.s!="None" && !it->second.s.empty())
                guide.town.push_back(it->second.s);
            if (k>=20) continue;
            std::snprintf(col,sizeof(col),"MarkerIDWorldMap%02d",k);
            if (const auto it=row.find(col); it!=row.end() && it->second.s!="None" && !it->second.s.empty())
                guide.world.push_back(it->second.s);
        }
        if (!guide.message.empty() && (!guide.world.empty() || !guide.town.empty()))
            built->guides.push_back(std::move(guide));
    }
    auto shared = std::make_shared<const MapData>(std::move(*built));
    std::scoped_lock lock{map_mutex};
    if (!map_cache)
        map_cache = std::move(shared);
    return map_cache;
}

std::shared_ptr<const MapData> CachedMapData() {
    std::scoped_lock lock{map_mutex};
    return map_cache;
}

std::optional<Image> DecodeMapTexture(std::span<const u8> uexp, const MapTexturePin& pin,
                                      const AstcFn& astc,
                                      std::string* why) {
    const auto fail = [why](const char* r) -> std::optional<Image> {
        if (why)
            *why = r;
        return std::nullopt;
    };
    static constexpr std::string_view Formats[3] = {std::string_view{"PF_BC7", 7},
                                                    std::string_view{"PF_ASTC_8x8", 12},
                                                    std::string_view{"PF_B8G8R8A8", 12}};
    if (pin.format > 2 || pin.w == 0 || pin.h == 0 || pin.w > 2048 || pin.h > 2048)
        return fail("texture pin");
    const auto fmt = Formats[pin.format];
    const u64 want = pin.format == 0   ? u64{(pin.w + 3) / 4} * ((pin.h + 3) / 4) * 16
                     : pin.format == 1 ? u64{(pin.w + 7) / 8} * ((pin.h + 7) / 8) * 16
                                       : u64{pin.w} * pin.h * 4;
    if (pin.data_bytes != want || pin.pf_at < 20 || pin.data_at != pin.pf_at + fmt.size() + 32 ||
        uexp.size() < std::size_t{pin.data_at} + pin.data_bytes + 12)
        return fail("texture layout");
    const u8* p = uexp.data();
    if (Le32(p + pin.pf_at - 4) != fmt.size() || std::memcmp(p + pin.pf_at, fmt.data(), fmt.size()) != 0)
        return fail("texture format");
    if (Le32(p + pin.pf_at - 16) != pin.w || Le32(p + pin.pf_at - 12) != pin.h ||
        Le32(p + pin.pf_at - 8) != 1)
        return fail("texture dimensions");
    const u8* mip = p + pin.pf_at + fmt.size();
    if (Le32(mip) != 0 || Le32(mip + 4) != pin.mip_count || Le32(mip + 8) != 1 || Le32(mip + 12) != 72 ||
        Le32(mip + 16) != pin.data_bytes || Le32(mip + 20) != pin.data_bytes)
        return fail("texture mip header");
    const u8* data = p + pin.data_at;
    const u8* after = data + pin.data_bytes;
    if (Le32(after) != pin.w || Le32(after + 4) != pin.h || Le32(after + 8) != 1)
        return fail("texture trailer");
    Image img{pin.w, pin.h, std::vector<u8>(std::size_t{pin.w} * pin.h * 4)};
    if (pin.format == 2) {
        for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
            img.rgba[i] = data[i + 2];
            img.rgba[i + 1] = data[i + 1];
            img.rgba[i + 2] = data[i];
            img.rgba[i + 3] = data[i + 3];
        }
        return img;
    }
    if (pin.format == 1) {
        if (!astc || !astc(pin.w, pin.h, data, pin.data_bytes, img.rgba.data(), img.rgba.size()))
            return fail("ASTC decoder unavailable");
        return img;
    }
    const u32 bx = (pin.w + 3) / 4, by = (pin.h + 3) / 4;
    for (u32 y = 0; y < by; ++y)
        for (u32 x = 0; x < bx; ++x)
            DecodeBc7Block(data + (std::size_t{y} * bx + x) * 16,
                           img.rgba.data() + (std::size_t{y} * 4 * pin.w + x * 4) * 4, x * 4, y * 4,
                           pin.w, pin.h);
    return img;
}

// ------------------------------------------------------------------------------ live reads
std::string ResolveMap(const GuestRead& read, u64 main_base, u64 main_size, MapRoots& roots) {
    roots = {};
    if (auto bad = CheckPins(read, main_base, main_size, MapPins(), "map pin"); !bad.empty())
        return bad;
    // facility table: ADRP (0xce4140) + ADD (0xce4144), both pinned words of GetFacilityIconIndex
    const u32 adrp = 0xd001e129, add = 0x91042129;
    const u64 table = AdrpPage(main_base + 0xce4140, adrp) + ((add >> 10) & 0xfff);
    float f[14];
    if (table + sizeof(f) > main_base + main_size || !read(table, f, sizeof(f)))
        return "facility table read";
    for (int i = 0; i < 14; ++i) {
        if (f[i] != static_cast<float>(FacilityTable[static_cast<std::size_t>(i)]))
            return "facility table differs";
        roots.facility_cell[static_cast<std::size_t>(i)] = static_cast<int>(f[i]);
    }
    roots.ok = true;
    return {};
}

namespace {

std::optional<std::string> ClassName(const GuestRead& read, const Roots& roots, u64 obj,
                                     std::map<u64, std::string>& cache) {
    u64 cls = 0;
    if (!Get(read, obj + ObjClass, cls) || cls == 0)
        return std::nullopt;
    if (const auto it = cache.find(cls); it != cache.end())
        return it->second;
    u32 n[2]{};
    if (!read(cls + ObjName, n, sizeof(n)))
        return std::nullopt;
    auto s = ReadFName(read, roots.name_pool, n[0], n[1]);
    if (s && cache.size() < 512)
        cache.emplace(cls, *s);
    return s;
}

u64 Subsystem(const GuestRead& read, const Roots& roots, u64 world, std::string_view name,
              std::map<u64, std::string>& cache) {
    u64 subs = 0;
    s32 n = 0;
    if (!Get(read, world + WorldSubsystems, subs) || !Get(read, world + WorldSubsystems + 8, n) || n <= 0 ||
        n > 64)
        return 0;
    std::vector<u64> e(static_cast<std::size_t>(n) * 3);
    if (!read(subs, e.data(), e.size() * 8))
        return 0;
    for (s32 k = 0; k < n; ++k) {
        const u64 cls = e[static_cast<std::size_t>(k) * 3], obj = e[static_cast<std::size_t>(k) * 3 + 1];
        if (!cls || !obj)
            continue;
        auto it = cache.find(cls);
        if (it == cache.end()) {
            u32 nm[2]{};
            if (!read(cls + ObjName, nm, sizeof(nm)))
                continue;
            const auto s = ReadFName(read, roots.name_pool, nm[0], nm[1]);
            if (!s)
                continue;
            it = cache.emplace(cls, *s).first;
        }
        if (it->second == name)
            return obj;
    }
    return 0;
}

/// Levels of the current map's LoadingInfoMap entry (LevelStreamingDynamic pointers).
std::vector<u64> MapLevels(const GuestRead& read, u64 llm, const u32 key[2]) {
    std::vector<u64> out;
    u64 data = 0;
    s32 n = 0;
    if (!Get(read, llm + LlmLoadingInfo, data) || !Get(read, llm + LlmLoadingInfo + 8, n) || n <= 0 || n > 64)
        return out;
    std::vector<u8> e(static_cast<std::size_t>(n) * 0x30);
    if (!read(data, e.data(), e.size()))
        return out;
    for (s32 k = 0; k < n; ++k) {
        const u8* p = e.data() + static_cast<std::size_t>(k) * 0x30;
        if (Le32(p) != key[0] || Le32(p + 4) != key[1])
            continue;
        u64 la = 0;
        s32 ln = 0;
        std::memcpy(&la, p + 0x18, 8);
        std::memcpy(&ln, p + 0x20, 4);
        if (ln <= 0 || ln > 64 || !la)
            return out;
        std::vector<u64> l(static_cast<std::size_t>(ln) * 2);
        if (!read(la, l.data(), l.size() * 8))
            return out;
        for (s32 j = 0; j < ln; ++j)
            if (l[static_cast<std::size_t>(j) * 2])
                out.push_back(l[static_cast<std::size_t>(j) * 2]);
        return out;
    }
    return out;
}

bool ReadExtent(const GuestRead& read, u64 common, u64 origin, u64 count, u64 tile, WorldExtent& e) {
    float o[2]{}, t[2]{};
    s32 n[2]{};
    if (!read(common + origin, o, 8) || !read(common + count, n, 8) || !read(common + tile, t, 8))
        return false;
    if (n[0] <= 0 || n[1] <= 0 || n[0] > 256 || n[1] > 256 || !(t[0] > 0) || !(t[1] > 0))
        return false;
    e = {true, o[0], o[1], n[0] * t[0], n[1] * t[1]};
    return true;
}

} // namespace

bool ReadLocation(const GuestRead& read, const Roots& roots, const SceneState& scene, u64 pawn,
                  LocationCache& cache, LocationSnapshot& out, std::string* why) {
    const auto fail = [why](const char* r) {
        if (why)
            *why = r;
        return false;
    };
    out = {};
    if (!scene.llm || !pawn)
        return fail("no level manager / pawn");
    if (cache.world != scene.world_ptr) {
        cache = {};
        cache.world = scene.world_ptr;
    }
    u32 id[2]{};
    if (!read(scene.llm + LlmCurrent, id, sizeof(id)))
        return fail("map id");
    const auto map = ReadFName(read, roots.name_pool, id[0], id[1]);
    if (!map || !map->starts_with("MAPLIST_"))
        return fail("map id name");
    out.map_id = *map;
    const auto pc = ClassName(read, roots, pawn, cache.class_names);
    if (!pc)
        return fail("pawn class");
    out.pawn_class = *pc;
    u64 g = 0, gd = 0, p = 0, root = 0;
    if (!ReadGdP(read, roots, gd, p, &g))
        return fail("progress record");
    if (!Get(read, p + PGold, out.gold) || !Get(read, p + 0x34, out.medals) ||
        !read(p + PProgress, out.progress.data(), sizeof(out.progress)))
        return fail("progress fields");
    // time of day (MapTimeManager world subsystem); a cached pointer must still be one (a new world
    // can reuse the old world's address after title -> load)
    if (cache.time_mgr && ClassName(read, roots, cache.time_mgr, cache.class_names) != std::string{"MapTimeManager"})
        cache.time_mgr = 0;
    if (!cache.time_mgr)
        cache.time_mgr = Subsystem(read, roots, scene.world_ptr, "MapTimeManager", cache.class_names);
    float t = 0;
    if (cache.time_mgr && Get(read, cache.time_mgr + TimeSeconds, t) && t >= 0 && t < 1e9f)
        out.hour = (static_cast<int>(t) % 86400) / 3600;
    // world extents (NicolaDataAssetCommon)
    if (!cache.field[0].ok) {
        u64 common = 0;
        if (Get(read, g + GDataCommon, common) && common &&
            ClassName(read, roots, common, cache.class_names) == std::string{"NicolaDataAssetCommon"}) {
            ReadExtent(read, common, 0x168, 0x170, 0x178, cache.field[0]);
            ReadExtent(read, common, 0x180, 0x188, 0x190, cache.field[1]);
        }
    }
    // position: root RelativeLocation minus the current map's sublevel origin
    float loc[3]{}, rot[3]{}, fac[2]{};
    if (!Get(read, pawn + PawnRoot, root) || !root || !read(root + RootLoc, loc, sizeof(loc)) ||
        !read(root + RootRot, rot, sizeof(rot)))
        return fail("pawn root");
    const auto levels = MapLevels(read, scene.llm, id);
    float origin[3]{};
    if (levels.empty() || !read(levels.front() + LsdTranslation, origin, sizeof(origin)))
        return fail("map origin");
    out.x = loc[0] - origin[0];
    out.y = loc[1] - origin[1];
    out.have_pos = std::isfinite(out.x) && std::isfinite(out.y);
    // the Map screen turns the player icon by the root component's world yaw (0xbfa44c..0xbfa524:
    // ComponentToWorld rotator, Yaw -> SetRenderTransformAngle); the root has no parent here
    (void)fac;
    if (std::isfinite(rot[1])) {
        out.facing_deg = rot[1];
        out.facing_ok = true;
    }
    return true;
}

bool WorldVariantVisible(std::string_view id, const LocationSnapshot& loc) {
    const auto flag = [&](u32 i) { return ((loc.progress[i >> 6] >> (i & 63)) & 1) != 0; };
    for (const auto& v : WorldVariants) {
        if (v.id != id) continue;
        int chosen = 0;
        if (v.group == 0) {
            constexpr u32 stages[] = {0x47, 0x48, 0x49, 0x4b, 0x4d};
            for (int k = 0; k < 5; ++k) if (flag(stages[k])) chosen = k + 1;
        } else {
            constexpr u32 changes[] = {0x5b, 0x21, 0x61};
            chosen = flag(changes[v.group - 1]) ? 1 : 0;
        }
        return v.index == chosen;
    }
    return true;
}

std::optional<std::vector<WorldIcon>> ReadWorldIcons(const GuestRead& read, const Roots& roots,
    const MapData& data, const LocationSnapshot& loc, LocationCache& cache) {
    u64 g = 0, gd = 0, p = 0;
    if (!ReadGdP(read, roots, gd, p, &g))
        return std::nullopt;
    // Native GetObtainedRuraList(1): union of P+0x230 and P+0x240. Empty is valid.
    std::vector<std::string> acquired;
    for (u64 off : {0x230u, 0x240u}) {
        u64 a = 0; s32 n = 0;
        if (!Get(read, p + off, a) || !Get(read, p + off + 8, n) || n < 0 || n > 4096 || (n && !a))
            return std::nullopt;
        std::vector<u32> ids(static_cast<std::size_t>(n) * 2);
        if (n && !read(a, ids.data(), ids.size() * 4)) return std::nullopt;
        for (s32 k = 0; k < n; ++k) {
            const auto id = ReadFName(read, roots.name_pool, ids[k * 2], ids[k * 2 + 1]);
            if (!id) return std::nullopt;
            acquired.push_back(*id);
        }
    }
    // Static world anchors: native FindPlayerStartByTag traverses actor category 0x13.
    // Bound the linked list and reject partial/cyclic reads; no guessed coordinates.
    if (cache.world_anchors.empty()) {
        u64 manager = 0, heads = 0, list = 0, node = 0;
        if (!Get(read, g + 0x448, manager) || !manager ||
            ClassName(read, roots, manager, cache.class_names) != std::string{"ActorListMan"} ||
            !Get(read, manager + 0x28, heads) || !heads) return std::nullopt;
        std::map<std::string, std::pair<float, float>, std::less<>> anchors;
        for (u64 category : {0x13u, 0x1au}) {
            if (!Get(read, heads + category * 16, list) || !list || !Get(read, list, node)) return std::nullopt;
            std::map<u64, bool> seen;
            while (node) {
                if (seen.size() >= 4096 || !seen.emplace(node, true).second) return std::nullopt;
                u64 actor = 0, root = 0, next = 0; u32 tag[2]{}; float pos[3]{};
                if (!Get(read, node, next) || !Get(read, node + 0x10, actor) || !actor ||
                    !Get(read, actor + PawnRoot, root) || !root ||
                    !read(actor + (category == 0x13 ? 0x244 : 0x29c), tag, sizeof(tag)) || !read(root + 0x1d0, pos, sizeof(pos)))
                    return std::nullopt;
                const auto name = ReadFName(read, roots.name_pool, tag[0], tag[1]);
                if (!name || !std::isfinite(pos[0]) || !std::isfinite(pos[1])) return std::nullopt;
                anchors.try_emplace(*name, pos[0], pos[1]);
                node = next;
            }
        }
        cache.world_anchors = std::move(anchors);
    }
    std::vector<WorldIcon> out;
    for (const auto& id : acquired) {
        const auto d = data.destinations.find(id);
        if (d == data.destinations.end() || d->second.type >= 12 || d->second.type < 0 ||
            !WorldVariantVisible(id, loc)) continue;
        const auto* floor = data.Row(d->second.floor);
        if (!floor) continue;
        const auto anchor = cache.world_anchors.find(d->second.tag);
        if (anchor == cache.world_anchors.end()) continue;
        WorldIcon icon{d->second.type, anchor->second.first, anchor->second.second, floor->underground};
        if (std::find(out.begin(), out.end(), icon) == out.end()) out.push_back(icon);
    }
    return out;
}

std::optional<FieldMenuState> ReadFieldMenu(const GuestRead& read, const Roots& roots,
                                            std::map<std::uint64_t, std::string>* class_names) {
    std::map<u64, std::string> local;
    std::map<u64, std::string>& names = class_names ? *class_names : local;
    u64 g=0, ui=0, manager=0, arr=0;
    s32 n=0;
    if (!Get(read,roots.g_slot,g) || !g || !Get(read,g+0x418,ui) || !ui ||
        ClassName(read,roots,ui,names) != "BP_NicolaUIManager_C" ||
        !Get(read,ui+0x7c0,manager) || !manager ||
        ClassName(read,roots,manager,names) != "BP_UIWidgetManager_C" ||
        !Get(read,manager+0x298,arr) || !Get(read,manager+0x2a0,n) || n<0 || n>128 || (n && !arr))
        return std::nullopt;
    std::vector<u64> controls(n);
    if (n && !read(arr,controls.data(),controls.size()*8)) return std::nullopt;
    static constexpr std::string_view allowed[] = {"UICommonMenuCheck", "UIMiniMapMenu", "UIFieldPopup",
        "UIEventMovieControl", "UIFieldEfxMenu", "UIFieldTopMenu", "UIFieldTopMenuListTop",
        "UIFieldTacticsMenu", "UIFieldTacticsMenuListTop", "UIFieldTacticsFormationMenuListTop",
        "UIFieldEfxMenuWindowGuide"};
    std::array<u64,11> objects{};
    for (const auto obj : controls) {
        if (!obj) return std::nullopt;
        const auto name=ClassName(read,roots,obj,names);
        if (!name) return std::nullopt;
        const auto it=std::find(std::begin(allowed),std::end(allowed),*name);
        if (it==std::end(allowed)) return FieldMenuState{};
        const auto k=static_cast<std::size_t>(it-std::begin(allowed));
        if (objects[k]) return std::nullopt;
        objects[k]=obj;
    }
    for (int k=0;k<4;++k) if (!objects[k]) return FieldMenuState{};
    // Town interaction guides are resident prompts (e.g. Speak), not an open menu.
    // Require their parent effect controller and continue to reject unrelated controllers.
    if (objects[10] && !objects[4]) return std::nullopt;
    const int residents = 4+(objects[4] ? 1 : 0)+(objects[10] ? 1 : 0);
    if (n==residents) return FieldMenuState{FieldMenuMode::Field};
    if (!objects[5] || !objects[6]) return FieldMenuState{};
    if (n==residents+2) {
        s32 index=-1;
        if (!Get(read,objects[6]+0x94,index) || index<0 || index>5) return std::nullopt;
        return FieldMenuState{FieldMenuMode::Top,index};
    }
    if (n==residents+5 && objects[7] && objects[8] && objects[9]) {
        s32 cursor=-1, count=0, selected=-1, misc=-1;
        if (!Get(read,objects[8]+0x94,misc) || misc!=3 ||
            !Get(read,objects[9]+0x94,cursor) || !Get(read,objects[9]+0x98,count) ||
            !Get(read,objects[9]+0xb0,selected) || count<1 || count>4 || cursor<0 || cursor>=count ||
            selected < -1 || selected>=count) return std::nullopt;
        return FieldMenuState{FieldMenuMode::LineUp,cursor,4,selected,count};
    }
    if (n==residents+4 && objects[7] && objects[8]) {
        s32 index=-1,count=0;u64 values=0;u8 command=0;
        if (!Get(read,objects[8]+0x94,index) || !Get(read,objects[8]+0xc0,values) || !values ||
            !Get(read,objects[8]+0xc8,count) || count<1 || count>12 || index<0 || index>=count ||
            !Get(read,values+static_cast<u64>(index),command)) return std::nullopt;
        return FieldMenuState{FieldMenuMode::Misc,index,command};
    }
    return FieldMenuState{};
}

std::optional<std::vector<UiController>> ReadUiControllers(const GuestRead& read, const Roots& roots,
                                                           std::map<std::uint64_t, std::string>* class_names) {
    std::map<u64, std::string> local;
    std::map<u64, std::string>& names = class_names ? *class_names : local;
    u64 g=0, ui=0, manager=0, arr=0;
    s32 n=0;
    if (!Get(read,roots.g_slot,g) || !g || !Get(read,g+0x418,ui) || !ui ||
        ClassName(read,roots,ui,names) != "BP_NicolaUIManager_C" ||
        !Get(read,ui+0x7c0,manager) || !manager ||
        ClassName(read,roots,manager,names) != "BP_UIWidgetManager_C" ||
        !Get(read,manager+0x298,arr) || !Get(read,manager+0x2a0,n) || n<0 || n>128 || (n && !arr))
        return std::nullopt;
    std::vector<u64> controls(static_cast<std::size_t>(n));
    if (n && !read(arr,controls.data(),controls.size()*8)) return std::nullopt;
    std::vector<UiController> out;
    for (const auto obj : controls) {
        if (!obj) return std::nullopt;
        const auto name=ClassName(read,roots,obj,names);
        s32 state=0;
        if (!name || !Get(read,obj+0x88,state)) return std::nullopt;
        out.push_back({*name,state});
    }
    return out;
}

std::optional<std::vector<std::string>> ReadVisited(const GuestRead& read, const Roots& roots) {
    u64 gd = 0, p = 0, data = 0;
    s32 n = 0;
    if (!ReadGdP(read, roots, gd, p) || !Get(read, p + PVisited, data) || !Get(read, p + PVisited + 8, n) || n < 0 ||
        n > 4096)
        return std::nullopt;
    std::vector<u32> raw(static_cast<std::size_t>(n) * 2);
    if (n && !read(data, raw.data(), raw.size() * 4))
        return std::nullopt;
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(n));
    for (s32 k = 0; k < n; ++k) {
        auto s = ReadFName(read, roots.name_pool, raw[static_cast<std::size_t>(k) * 2],
                           raw[static_cast<std::size_t>(k) * 2 + 1]);
        if (!s)
            return std::nullopt;
        out.push_back(std::move(*s));
    }
    return out;
}

std::optional<std::pair<int, int>> ReadChests(const GuestRead& read, const Roots& roots, u64 llm,
                                              std::string_view map_id, ChestScan& scan) {
    u32 id[2]{};
    if (!llm || !read(llm + LlmCurrent, id, sizeof(id)))
        return std::nullopt;
    if (ReadFName(read, roots.name_pool, id[0], id[1]) != std::string{map_id})
        return std::nullopt; // the map changed meanwhile
    u64 gd = 0, p = 0;
    if (!ReadGdP(read, roots, gd, p))
        return std::nullopt;
    std::array<u64, 22> bits{};
    if (!read(p + PSearchBits, bits.data(), sizeof(bits)))
        return std::nullopt;
    std::vector<ChestScan::Level> levels;
    int total = 0, opened = 0;
    for (const u64 lsd : MapLevels(read, llm, id)) {
        u32 pk[2]{};
        u64 level = 0;
        if (!read(lsd + LsdPackage, pk, sizeof(pk)))
            return std::nullopt;
        const auto pkg = ReadFName(read, roots.name_pool, pk[0], pk[1]);
        if (!pkg || !Ends(*pkg, "_SearchObj"))
            continue;
        if (!Get(read, lsd + LsdLoaded, level) || !level)
            return std::nullopt; // not loaded yet
        u64 actors = 0;
        s32 n = 0;
        if (!Get(read, level + LevelActors, actors) || !Get(read, level + LevelActors + 8, n) || n < 0 ||
            n > 8192)
            return std::nullopt;
        ChestScan::Level cur{lsd, level, actors, std::vector<u64>(static_cast<std::size_t>(n)), {}};
        if (n && !read(actors, cur.actors.data(), cur.actors.size() * 8))
            return std::nullopt;
        const auto same = std::find_if(scan.levels.begin(), scan.levels.end(), [&](const ChestScan::Level& l) {
            return l.lsd == lsd && l.level == level && l.array == actors && l.actors == cur.actors;
        });
        bool definite = true; // every actor answered (a failed read is retried next time, not cached)
        if (same != scan.levels.end()) {
            cur.flags = same->flags;
        } else {
            for (const u64 actor : cur.actors) {
                u64 trig = 0;
                if (!actor)
                    continue;
                if (!Get(read, actor + ActorTrigger, trig)) {
                    definite = false;
                    continue;
                }
                if (!trig)
                    continue;
                const auto cls = ClassName(read, roots, trig, scan.classes);
                if (!cls) {
                    definite = false;
                    continue;
                }
                if (*cls != "SearchObjEventTrigger")
                    continue;
                std::uint16_t flag = 0;
                u8 type = 0;
                if (!Get(read, trig + TrigFlag, flag) || !Get(read, trig + TrigType, type))
                    return std::nullopt;
                if (static_cast<u8>(type - 11) >= 4) // IsTreasure (0x845a00)
                    continue;
                cur.flags.push_back(flag);
            }
        }
        total += static_cast<int>(cur.flags.size());
        for (const std::uint16_t flag : cur.flags)
            if (flag <= 0x55e && ((bits[flag >> 6] >> (flag & 63)) & 1))
                ++opened;
        if (definite)
            levels.push_back(std::move(cur));
    }
    scan.levels = std::move(levels);
    return std::pair{total, opened};
}

// ------------------------------------------------------------------------------ page values
namespace {

constexpr int MaxFloorsShown = 4;
constexpr int LegendRows = 7;     ///< 48 px pitch, 44 px when all seven are shown (render.py field_map_town)
constexpr int FloorPitch = 52;    ///< render.py field_dungeon floor rows
constexpr int FloorTextW = 316;   ///< a floor row's text room (info card inner width 332 - 16)
constexpr int HereHintW = 162;    ///< "you are here", Semibold 28

struct Floor {
    std::string id;
    const MapRow* row{};
};

/// Visible spot entries of a spot row (display / hide progress flags).
std::array<bool, 12> Visible(const std::array<SpotEntry, 12>& e, const LocationSnapshot& loc) {
    std::array<bool, 12> v{};
    const auto bit = [&](u32 i) {
        if (i > 0xac) // 0x877570 bound
            return false;
        return ((loc.progress[i >> 6] >> (i & 63)) & 1) != 0;
    };
    for (int k = 0; k < 12; ++k) {
        const auto& s = e[static_cast<std::size_t>(k)];
        v[static_cast<std::size_t>(k)] = s.type != 0 && (s.disp == 0 || bit(s.disp)) && !(s.hide && bit(s.hide));
    }
    return v;
}

} // namespace

namespace {
std::shared_ptr<const Image> NoMapTexture(const RangeReader& read, int index, std::string* why);

/// The no-map card (C): the composed page (dq3_system.cpp sys/card/) and the native objective in its window.
void PublishCard(MapView& v, const MapViewInput& in, const NoMapView& nm) {
    auto& I = v.ints;
    auto& T = v.texts;
    const bool mv = in.visible;
    I["dq3.mv"] = mv;
    I["dq3.mv.c"] = mv;
    if (!mv)
        return;
    int objw = 0;
    if (in.objective && !in.objective->empty()) {
        const std::string obj = Styled(*in.objective, FontStyle::Bold32);
        objw = (in.font ? in.font->Measure(obj) : 0) + CardObjPad;
        objw = std::min(objw, CardW - 40);
        T["dq3.m.obj"] = obj;
        I["dq3.m.obj.on"] = 1;
        I["dq3.m.obj.x"] = (CardW - objw) / 2 + CardObjText;
    }
    T["dq3.m.card"] = "module:dq3:sys/card/" + std::to_string(CardW) + "x" + std::to_string(CardH) + "/" +
                      std::to_string(nm.variant) + "/" + in.hero_sprite + "/" + std::to_string(objw);
}
} // namespace

NoMapView SelectNoMap(const MapData* d, const MapRow* row, std::string_view map_id,
                      const std::vector<std::string>* visited) {
    NoMapView v;
    // a place's town map: GOP_Memory AreaNameId -> the GOP_Rura_Riremito row of that name -> its FloorID
    const auto town_of = [d](std::string_view memory) -> std::string {
        const auto a = d->memory_area.find(memory);
        if (a == d->memory_area.end())
            return {};
        for (const auto& [id, dest] : d->destinations)
            if (dest.area == a->second && d->Row(dest.floor))
                return dest.floor;
        return {};
    };
    // the card's "journey begins" copy names Aliahan: shown while Aliahan's town map is not visited yet
    bool journey = false;
    if (d && visited) {
        const std::string town = town_of("MEMORY_ALIAHAN");
        journey = !town.empty() && std::find(visited->begin(), visited->end(), town) == visited->end();
    }
    if (!d || !row) {
        v.state = NoMapState::Card;
        v.variant = journey ? 0 : 2;
        return v;
    }
    if (row->image >= 0 || map_id.starts_with("MAPLIST_FIELD"))
        return v;
    const bool card_row = std::find(std::begin(CardRowsTable), std::end(CardRowsTable), map_id) != std::end(CardRowsTable);
    if (!card_row && !row->memory.empty() && row->memory != "None")
        for (std::size_t k = 0; k < std::size(PlaceArtTable); ++k)
            if (PlaceArtTable[k].memory == row->memory) {
                const auto a = d->memory_area.find(row->memory);
                if (a == d->memory_area.end())
                    break;
                v.state = NoMapState::Place;
                v.art = static_cast<int>(k);
                v.area = a->second;
                v.region = std::string{PlaceArtTable[k].region};
                v.town_row = town_of(row->memory);
                return v;
            }
    v.state = NoMapState::Card;
    v.variant = journey ? 0 : 1;
    return v;
}

#ifdef DQ3_TEST_API
std::vector<std::string_view> PlaceArtPlaces() {
    std::vector<std::string_view> out;
    for (const auto& p : PlaceArtTable)
        out.push_back(p.memory);
    return out;
}
#endif

std::shared_ptr<const Image> NoMapEmblemTexture(const RangeReader& read, std::string* why) {
    return NoMapTexture(read, NoMapEmblem, why);
}

MapView BuildMapView(const MapViewInput& in) {
    MapView v;
    auto& I = v.ints;
    auto& T = v.texts;
    I["dq3.m.on"] = 0;
    if (!in.data || !in.loc || !in.roots || !in.data->Row(in.loc->map_id)) {
        // location not read yet / row unknown: the no-map card instead of a blank page
        I["dq3.m.wait"] = 1;
        PublishCard(v, in, SelectNoMap(in.data, nullptr, {}, in.visited));
        return v;
    }
    I["dq3.m.wait"] = 0;
    const MapData& d = *in.data;
    const LocationSnapshot& loc = *in.loc;
    const MapRow* cur = d.Row(loc.map_id);
    const NoMapView nm = SelectNoMap(&d, cur, loc.map_id, in.visited);
    if (nm.state == NoMapState::Card) {
        PublishCard(v, in, nm);
        return v;
    }
    const bool place_card = nm.state == NoMapState::Place;
    I["dq3.m.on"] = 1;
    const int frame = loc.hour >= 0 && loc.hour < 24 ? d.hour_frame[static_cast<std::size_t>(loc.hour)] : 0;
    const bool day = frame != 3; // 0xbe5db8
    // time and gold
    I["dq3.m.time.on"] = frame != 0;
    T["dq3.m.time_src"] = std::string{"module:dq3:tsym/"} + (frame == 3 ? "1" : "0") + "/38";
    T["dq3.m.time"] = Styled(frame == 1 ? "Daytime" : frame == 2 ? "Evening" : "Night", FontStyle::Bold32);
    T["dq3.m.gold"] = Styled(Grouped(s64{loc.gold}) + " G", FontStyle::Bold32);

    // floors: visited rows of the same prefix and floor group, one per floor (sort number)
    std::vector<Floor> floors;
    if (cur->floor_group != 0 && in.visited) {
        std::map<int, Floor, std::greater<>> by_sort;
        for (const auto& id : *in.visited) {
            const MapRow* r = d.Row(id);
            if (!r || r->prefix != cur->prefix || r->floor_group != cur->floor_group)
                continue;
            auto& f = by_sort[r->floor_sort];
            if (!f.row || id == loc.map_id)
                f = {id, r};
        }
        if (auto it = by_sort.find(cur->floor_sort); it != by_sort.end())
            it->second = {loc.map_id, cur};
        else
            by_sort[cur->floor_sort] = {loc.map_id, cur}; // the current map is visited by definition
        for (auto& [s, f] : by_sort)
            floors.push_back(f);
    }
    int here = 0;
    for (std::size_t k = 0; k < floors.size(); ++k)
        if (floors[k].row == cur)
            here = static_cast<int>(k);
    int shown = here;
    if (floors.size() > 1) {
        shown = std::clamp(here + in.browse, 0, static_cast<int>(floors.size()) - 1);
        v.browse = shown - here;
    }
    const MapRow* view_row = floors.empty() ? cur : floors[static_cast<std::size_t>(shown)].row;
    const bool browsing = view_row != cur;

    // title: the native Map title (NameTextID); a floor group shows "<place>" with "<floor>" rows. A row
    // without a name: the world map title only on a field map, else its place (GOP_Memory AreaNameId)
    const auto noun = [&](const MapRow* r) -> std::u32string {
        if (r->name_text == "None" || r->name_text.empty()) {
            if (r == cur && !loc.map_id.starts_with("MAPLIST_FIELD")) {
                const auto a = d.memory_area.find(r->memory);
                const auto* n = a == d.memory_area.end() ? nullptr : d.Noun(a->second);
                return n ? *n : std::u32string{};
            }
            const auto it = d.menu.find(r->underground ? "Txt_Map_Menu_Alefgard_Map" : "Txt_Map_Menu_World_Map");
            return it != d.menu.end() ? it->second : std::u32string{};
        }
        const auto* n = d.Noun(r->name_text);
        return n ? *n : std::u32string{};
    };
    const std::u32string sep = U" - ";
    std::u32string title = noun(view_row);
    std::u32string place;
    bool split = floors.size() > 0;
    for (const auto& f : floors) {
        const auto n = noun(f.row);
        const auto at = n.find(sep);
        const auto p = at == std::u32string::npos ? std::u32string{} : n.substr(0, at);
        if (p.empty() || (!place.empty() && p != place))
            split = false;
        place = p;
    }

    // legend (towns / interiors) or floors (floor groups)
    const std::string& spot_id = day ? view_row->spot_day : view_row->spot_night;
    const auto sp = d.spots.find(spot_id);
    std::array<bool, 12> vis{};
    if (sp != d.spots.end())
        vis = Visible(sp->second, loc);
    // legend entries: the visible facility types of the spot row, first appearance order
    std::vector<u8> types;
    if (sp != d.spots.end())
        for (int k = 0; k < 12 && types.size() < 7; ++k) {
            const auto& e = sp->second[static_cast<std::size_t>(k)];
            if (vis[static_cast<std::size_t>(k)] && FacilityCell(*in.roots, e.type, false) >= 0 &&
                std::find(types.begin(), types.end(), e.type) == types.end())
                types.push_back(e.type);
        }
    // a floor list when there is a choice of floors (the native L / R selector) or nothing to list
    const bool floor_mode = floors.size() > 1 || (floors.size() == 1 && types.empty());
    I["dq3.m.flo"] = floor_mode;
    split = split && floor_mode;
    if (split)
        title = place;
    // render.py banner(): the largest of Bold 44 / 40 / 36 / 32 / 30 / 28 that fits between the TitleBG end
    // ornaments, baseline round(px * 0.35) below the banner's middle
    {
        static constexpr std::pair<FontStyle, int> sizes[] = {{FontStyle::Bold44, 44}, {FontStyle::Bold40, 40},
                                                              {FontStyle::Bold36, 36}, {FontStyle::Bold32, 32},
                                                              {FontStyle::Bold30, 30}, {FontStyle::Bold28, 28}};
        std::string label;
        int px = 28;
        for (const auto& [st, size] : sizes) {
            label = Styled(title, st);
            px = size;
            if (!in.font || in.font->Measure(label) <= MapTitleRoom)
                break;
        }
        T["dq3.m.title"] = std::move(label);
        I["dq3.m.title.dy"] = std::lround(px * 0.35);
    }
    int legend = 0;
    if (!floor_mode) {
        const int pitch = types.size() >= LegendRows ? 44 : 48;
        for (const u8 type : types) {
            const SpotEntry e{type};
            const std::string p = "dq3.m.l" + std::to_string(legend);
            I[p + ".on"] = 1;
            I[p + ".y"] = legend * pitch;
            T[p + ".src"] = "module:dq3:fac/" + std::to_string(FacilityCell(*in.roots, e.type, true)) + "/38";
            T[p + ".name"] = Styled(SpotLabel(e.type), FontStyle::Semi32);
            ++legend;
        }
    }
    for (int k = legend; k < 7; ++k)
        I["dq3.m.l" + std::to_string(k) + ".on"] = 0;
    // floors list: up to MaxFloorsShown rows around the shown floor
    int first = 0;
    if (static_cast<int>(floors.size()) > MaxFloorsShown)
        first = std::clamp(shown - MaxFloorsShown / 2, 0, static_cast<int>(floors.size()) - MaxFloorsShown);
    int rows = 0;
    for (int k = 0; k < MaxFloorsShown; ++k) {
        const int f = first + k;
        const std::string p = "dq3.m.f" + std::to_string(k);
        const bool on = floor_mode && f < static_cast<int>(floors.size());
        I[p + ".on"] = on;
        if (!on)
            continue;
        ++rows;
        auto n = noun(floors[static_cast<std::size_t>(f)].row);
        if (split)
            n = n.substr(place.size() + sep.size());
        const bool is_here = f == here, is_shown = f == shown;
        I[p + ".here"] = is_here;
        I[p + ".sel"] = is_shown;
        std::string label;
        for (const FontStyle st : {FontStyle::Bold32, FontStyle::Bold28, FontStyle::Bold24}) {
            label = Styled(n, st);
            if (!in.font || in.font->Measure(label) <= FloorTextW)
                break;
        }
        // The selection highlight still marks the current floor when its name needs
        // the whole row. Keep the optional hint clear of long names.
        I[p + ".hint"] = is_here && (in.font ? in.font->Measure(label) <= FloorTextW - HereHintW - 16 : n.size() <= 6);
        T[p + ".name"] = std::move(label);
    }
    // chests opened on the current map (no treasure positions: the native map shows none), under the floors
    I["dq3.m.chest.on"] = floor_mode && !browsing && in.chests.has_value() && in.chests->first > 0;
    I["dq3.m.chest.y"] = rows * FloorPitch;
    if (in.chests)
        T["dq3.m.chest"] = Styled("Chests opened " + std::to_string(in.chests->second), FontStyle::Semi30);

    // map image and marker
    const bool full = in.full;
    I["dq3.m.full"] = full;
    const int bx = full ? MapX0 : MapRightX, bw = full ? (MapX1 - MapX0) : MapRightW;
    const int by = full ? FullMapY : MapPageY, bh = full ? FullMapH : MapPageH;
    const bool field = loc.map_id.starts_with("MAPLIST_FIELD");
    const bool world_position = field ? loc.have_pos : in.area_anchor.has_value();
    const double wx = field ? loc.x : in.area_anchor ? in.area_anchor->first : 0;
    const double wy = field ? loc.y : in.area_anchor ? in.area_anchor->second : 0;
    const int zoom = place_card ? 0 : std::clamp(in.zoom, 0, 2);
    // the place card's full map is its town map (Town Map button), without a marker
    const MapRow* town = place_card && full ? d.Row(nm.town_row) : nullptr;
    if (place_card)
        view_row = town ? town : cur;
    // FIELD_POS_X3/Y3 are the centre (bd7bd0); other worlds return origin + half size.
    WorldExtent island{true, -62800 - 23750, 33000 - 23750, 47500, 47500};
    bool island_view = false;
    const int fin = MapFin;
    int img_index = view_row->image;
    bool world = false;
    const WorldExtent* ext = nullptr;
    if (field || zoom > 0) {
        // the world map material's textures (0xbfd8b8 / 0xbfd9b8, UIMap2Menu constructor 0xbe5580)
        const auto flag = [&](u32 i) { return ((loc.progress[i >> 6] >> (i & 63)) & 1) != 0; };
        img_index = FindMapImage(view_row->underground
                                     ? (flag(0x88) ? "T_UI_Map_WorldMapImage_FL_2_2" : "T_UI_Map_WorldMapImage_FL_2")
                                     : (flag(0x5b) ? "T_UI_Map_WorldMapImage_FL_1_2" : "T_UI_Map_WorldMapImage_FL_1"));
        world = true;
        ext = in.world && in.world->ok ? in.world : nullptr;
        if (zoom == 1 && world_position && !view_row->underground &&
            wx >= island.ox && wx <= island.ox + island.w && wy >= island.oy && wy <= island.oy + island.h) {
            img_index = FindMapImage("T_UI_Map_WorldMap_AllArea_Aliahan");
            ext = &island;
            island_view = true;
        }
    }
    const bool regional = in.world && in.world->ok && world_position;
    I["dq3.m.region"] = regional;
    I["dq3.m.zoom.in"] = in.visible && zoom > 0;
    I["dq3.m.zoom.out"] = in.visible && zoom < 2 && in.world && in.world->ok;
    // the zoom label on a window sized to it (gen_manifest ZOOM_BOX: 1 Area map, 2 Minimap, 3 Island map,
    // 4 Region map, 5 World map)
    const int zoom_box = !world ? 1 : zoom == 0 ? 2 : zoom == 1 && regional ? (island_view ? 3 : 4) : 5;
    static constexpr const char* zoom_labels[5] = {"Area map", "Minimap", "Island map", "Region map", "World map"};
    T["dq3.m.zoom.label"] = Styled(zoom_labels[zoom_box - 1], FontStyle::Value);
    I["dq3.m.zoom.box"] = zoom_box;
    T["dq3.m.live.label"] = Styled(field ? "Live location" : "Area location", FontStyle::Semi32);
    bool mk_on = false; // the location marker drawn into the map image ("~" suffix of dq3.m.img)
    if (img_index >= 0) {
        const auto& pin = MapImagePins()[static_cast<std::size_t>(img_index)];
        const double s = std::min(static_cast<double>(bw - 2 * fin) / pin.w,
                                  static_cast<double>(bh - 2 * fin) / pin.h);
        const int tw = static_cast<int>(pin.w * s), th = static_cast<int>(pin.h * s);
        const int ix = bx + (bw - tw) / 2, iy = by + (bh - th) / 2;
        const int box_x = bx + fin, box_y = by + fin, box_w = bw - 2 * fin, box_h = bh - 2 * fin;
        u32 mask = 0;
        if (!world && sp != d.spots.end())
            for (int k = 0; k < 12; ++k)
                if (vis[static_cast<std::size_t>(k)] && FacilityCell(*in.roots, sp->second[static_cast<std::size_t>(k)].type, false) >= 0)
                    mask |= 1u << k;
        // Detail and regional viewports are centred on the native field coordinate.
        // Clamp the camera, not the marker, so border tiles remain geometrically aligned.
        const bool crop = world && ext && world_position && !island_view && zoom < 2;
        const int source = zoom == 0 ? (view_row->underground ? -2 : -1) : img_index;
        const int atlas = zoom == 0 ? 4096 : static_cast<int>(pin.w);
        int cx = 0, cy = 0, cw = 0, ch = 0;
        if (crop) {
            const double scale = zoom == 0 ? 10.0 : 2.0;
            const double unit = static_cast<double>(atlas) / scale / std::min(box_w, box_h);
            cw = std::clamp(static_cast<int>(std::lround(box_w * unit)), 1, atlas);
            ch = std::clamp(static_cast<int>(std::lround(box_h * unit)), 1, atlas);
            cx = std::clamp(static_cast<int>(std::lround((wy-ext->oy)/ext->h*atlas-cw/2.0)),0,atlas-cw);
            cy = std::clamp(static_cast<int>(std::lround((1-(wx-ext->ox)/ext->w)*atlas-ch/2.0)),0,atlas-ch);
        }
        // the fitted image centred on the fixed map box (the widget rect is the box)
        T["dq3.m.img"] = "module:dq3:mapimg/" + std::to_string(img_index) + "/" + std::to_string(box_w) + "x" +
                         std::to_string(box_h) + "/" + std::to_string(tw) + "x" + std::to_string(th) + "/" +
                         std::to_string(ix - box_x) + "," + std::to_string(iy - box_y) + "/" +
                         (mask ? spot_id : std::string{"-"}) + "/" + Hex4(mask);
        if (crop) {
            const int flag = view_row->underground ? 0x88 : 0x5b;
            T["dq3.m.img"] = "module:dq3:fieldimg/" + std::to_string(source) + "/" +
                std::to_string((loc.progress[flag >> 6] >> (flag & 63)) & 1) + "/" +
                std::to_string(box_w) + "x" + std::to_string(box_h) + "/" +
                std::to_string(cx) + "," + std::to_string(cy) + "," + std::to_string(cw) + "," + std::to_string(ch);
        }
        if (world && ext && in.world_icons) {
            std::string icons;
            for (const auto& icon : *in.world_icons) {
                if (icon.underground != view_row->underground) continue;
                const double u = (icon.y - ext->oy) / ext->h;
                const double v = 1.0 - (icon.x - ext->ox) / ext->w;
                if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) continue;
                const double px = crop ? (u*atlas-cx)*box_w/cw : u*tw;
                const double py = crop ? (v*atlas-cy)*box_h/ch : v*th;
                if (crop && (px < 0 || py < 0 || px > box_w || py > box_h)) continue;
                // GetMapIconIndex(type, unselected, world-map): type*4 + 2.
                icons += std::to_string(icon.type * 4 + 2) + "," +
                    std::to_string(std::lround(px)) + "," + std::to_string(std::lround(py)) + ";";
            }
            if (!icons.empty()) T["dq3.m.img"] += "/" + icons;
        }
        // marker (current floor only)
        double u = 0, vv = 0;
        bool have = false;
        if (!place_card && (!browsing || world) && (world ? world_position : loc.have_pos)) {
            if (world && ext) {
                u = (wy - ext->oy) / ext->h * pin.w;
                vv = (1.0 - (wx - ext->ox) / ext->w) * pin.h;
                have = u >= 0 && u <= pin.w && vv >= 0 && vv <= pin.h;
            } else if (!world && cur->spot_index != 0 && sp != d.spots.end()) {
                // interiors: the spot entry (0xbfa204), 35 px lower unless a HOUSE
                const int k = cur->spot_index;
                if (k >= 1 && k <= 12) {
                    const auto& e = sp->second[static_cast<std::size_t>(k - 1)];
                    u = e.x;
                    vv = e.y + (e.type == SpotHouse ? 0.0 : 35.0); // UI_CONSTPARAM_MAP_PLAYER_SPOT_POS_Y_OFFSET
                    have = true;
                }
            } else if (!world && cur->sx > 0 && cur->sy > 0) {
                u = std::clamp(static_cast<double>(loc.y - cur->oy) * pin.w / cur->sy, 0.0, static_cast<double>(pin.w));
                vv = std::clamp(pin.h - static_cast<double>(loc.x - cur->ox) * pin.h / cur->sx, 0.0, static_cast<double>(pin.h));
                have = true;
            }
        }
        if (have) {
            const double px = crop ? box_x + (u/pin.w*atlas-cx)*box_w/cw : ix + u * s;
            const double py = crop ? box_y + (vv/pin.h*atlas-cy)*box_h/ch : iy + vv * s;
            mk_on = true;
            const bool mk_world = world && zoom > 0;
            s64 mk_x = 0, mk_y = 0, mk_rot = 0;
            if (mk_world) {
                // the quill's tip (cell pixel (12, 156) of 176) is the location
                const double k = static_cast<double>(WorldMarkerPx) / 176.0;
                mk_x = std::lround(px - 12 * k);
                mk_y = std::lround(py - 156 * k);
            } else {
                mk_x = std::lround(px - MarkerPx / 2.0);
                mk_y = std::lround(py - MarkerPx / 2.0);
                mk_rot = loc.facing_ok && cur->spot_index == 0 ? std::lround(loc.facing_deg) : 0;
            }
            // One pannable image keeps every marker in the map's coordinate system. The host
            // clips and transforms the complete image; gestures never select another style.
            // "~<world quill 0|1>,<x>,<y>,<rotation>" with x / y relative to the map box.
            T["dq3.m.img"] += "~" + std::to_string(mk_world ? 1 : 0) + "," + std::to_string(mk_x - box_x) + "," +
                std::to_string(mk_y - box_y) + "," + std::to_string(mk_rot);
        }
    }
    // floor stepper (visited floors only)
    I["dq3.m.step.on"] = floors.size() > 1 && !world;
    if (floors.size() > 1) {
        auto n = noun(view_row);
        if (split)
            n = n.substr(place.size() + sep.size());
        T["dq3.m.step"] = Styled(n, FontStyle::Bold36);
        T["dq3.m.step.src"] = "module:dq3:stepper/" + std::to_string(2 * MapButton + 70) + "x" +
                              std::to_string(MapButton) + "/" + std::to_string(shown > 0 ? 1 : 0) +
                              std::to_string(shown + 1 < static_cast<int>(floors.size()) ? 1 : 0);
    }
    if (world) {
        I["dq3.m.flo"] = 0;
        I["dq3.m.chest.on"] = 0;
        for (int k = 0; k < 7; ++k) I["dq3.m.l" + std::to_string(k) + ".on"] = 0;
        for (int k = 0; k < MaxFloorsShown; ++k) {
            const auto p = "dq3.m.f" + std::to_string(k);
            I[p + ".on"] = I[p + ".here"] = I[p + ".sel"] = 0;
        }
    }
    // the place card (B): the still for the time frame, the place and region names, the Town Map button and
    // the native objective in the left card
    if (place_card) {
        T["dq3.m.art"] = "module:dq3:areaart/" + std::to_string(nm.art) + "/" + std::to_string(std::max(0, frame - 1)) +
                         "/" + std::to_string(MapRightW - 2 * MapFin) + "x" + std::to_string(MapPageH - 2 * MapFin);
        if (const auto* n = d.Noun(nm.area))
            T["dq3.m.place"] = Styled(*n, FontStyle::Bold54);
        if (const auto it = d.menu.find(nm.region); it != d.menu.end())
            T["dq3.m.region.t"] = Styled(it->second, FontStyle::Semi32);
        if (const auto it = d.menu.find("Txt_Map_Menu_Town_Map"); it != d.menu.end())
            T["dq3.m.town"] = Styled(it->second, FontStyle::Bold32);
        I["dq3.m.town.on"] = !nm.town_row.empty();
    }
    if (place_card && in.objective && !in.objective->empty()) {
        T["dq3.m.lobj"] = StyledWrap(*in.objective, FontStyle::Semi32);
        I["dq3.m.lobj.on"] = 1;
    }
    // gates: everything only while the Map tab is open; the left column only without the full map; the map box
    // (frame, label, zoom, Reset, full map) only with a map image
    const bool mv = in.visible;
    const bool has_img = T.contains("dq3.m.img");
    I["dq3.mv"] = mv;
    I["dq3.mv.l"] = mv && !full;
    I["dq3.mv.n"] = mv && !full && has_img;
    I["dq3.mv.f"] = mv && full && has_img;
    I["dq3.mv.p"] = mv && !full && place_card;
    for (auto& [k, val] : I) {
        const bool left = k.starts_with("dq3.m.l") || k.starts_with("dq3.m.f") || k == "dq3.m.chest.on" ||
                          k == "dq3.m.time.on" || k == "dq3.m.flo" || k == "dq3.m.town.on";
        const bool gate = k.ends_with(".on") || k == "dq3.m.flo" || k.ends_with(".here") || k.ends_with(".sel");
        if (k == "dq3.m.full" || k.starts_with("dq3.mv") || k == "dq3.m.wait")
            continue;
        if (gate && (!mv || (left && full)))
            val = 0;
    }
    I["dq3.m.step.n"] = I["dq3.m.step.on"] && !full;
    I["dq3.m.step.f"] = I["dq3.m.step.on"] && full;
    for (int k = 0; k < MaxFloorsShown; ++k) {
        const std::string p = "dq3.m.f" + std::to_string(k);
        I[p + ".oth"] = I[p + ".on"] && !I[p + ".here"];
        I[p + ".hint"] = I[p + ".hint"] && I[p + ".here"];
    }
    I["dq3.m.live.on"] = world && mk_on && mv && !full;
    return v;
}

// ------------------------------------------------------------------------------ page art
namespace {

std::mutex tex_mutex;
std::shared_ptr<const Image> ui_tex[static_cast<std::size_t>(MapUi::Count)];
/// Decoded map images, least recently used first. Nine: a fieldimg patch spans up to 3 x 3 floor tiles
/// (1024 px each), which a smaller cache would evict and decode again on every compose.
constexpr std::size_t MapImageCacheSize = 9;
std::vector<std::pair<int, std::shared_ptr<const Image>>> img_cache;
/// The cached image `index` moved to the most recently used end; nullptr when not cached. Hold tex_mutex.
std::shared_ptr<const Image> TouchMapImage(int index) {
    const auto it =
        std::find_if(img_cache.begin(), img_cache.end(), [index](const auto& e) { return e.first == index; });
    if (it == img_cache.end())
        return nullptr;
    std::rotate(it, it + 1, img_cache.end());
    return img_cache.back().second;
}

std::shared_ptr<const Image> LoadPinned(const RangeReader& read, const MapTexturePin& pin,
                                        const AstcFn& astc,
                                        std::string* why) {
    std::string r;
    const auto bytes = ReadMember(read, pin.member, &r);
    if (!bytes) {
        if (why)
            *why = std::string{pin.member.path} + ": " + r;
        return nullptr;
    }
    auto img = DecodeMapTexture(*bytes, pin, astc, &r);
    if (!img) {
        if (why)
            *why = std::string{pin.name} + ": " + r;
        return nullptr;
    }
    return std::make_shared<const Image>(std::move(*img));
}

std::shared_ptr<const Image> Ui(const RangeReader& read, MapUi id,
                                const AstcFn& astc,
                                std::string* why) {
    const auto i = static_cast<std::size_t>(id);
    {
        std::scoped_lock lock{tex_mutex};
        if (ui_tex[i])
            return ui_tex[i];
    }
    auto t = LoadPinned(read, MapUiPin(id), astc, why);
    if (!t)
        return nullptr;
    std::scoped_lock lock{tex_mutex};
    ui_tex[i] = t;
    return t;
}

std::shared_ptr<const Image> MapImage(const RangeReader& read, int index,
                                      const AstcFn& astc,
                                      std::string* why) {
    {
        std::scoped_lock lock{tex_mutex};
        if (auto cached = TouchMapImage(index))
            return cached;
    }
    auto t = LoadPinned(read, MapImagePins()[static_cast<std::size_t>(index)], astc, why);
    if (!t)
        return nullptr;
    std::scoped_lock lock{tex_mutex};
    if (auto cached = TouchMapImage(index)) // decoded by another thread meanwhile
        return cached;
    img_cache.emplace_back(index, t);
    if (img_cache.size() > MapImageCacheSize)
        img_cache.erase(img_cache.begin());
    return t;
}

/// A no-map texture (dq3_nomap_pins.inc): the place stills (the last one kept) and the emblem.
std::shared_ptr<const Image> NoMapTexture(const RangeReader& read, int index, std::string* why) {
    static std::pair<int, std::shared_ptr<const Image>> still{-1, nullptr}, emblem{-1, nullptr};
    auto& slot = index == NoMapEmblem ? emblem : still;
    {
        std::scoped_lock lock{tex_mutex};
        if (slot.first == index && slot.second)
            return slot.second;
    }
    if (index < 0 || index >= static_cast<int>(std::size(NoMapTexturePinsTable))) {
        if (why)
            *why = "no-map texture index";
        return nullptr;
    }
    auto t = LoadPinned(read, NoMapTexturePinsTable[static_cast<std::size_t>(index)], {}, why);
    if (!t)
        return nullptr;
    std::scoped_lock lock{tex_mutex};
    slot = {index, t};
    return t;
}

using art_util::Blank, art_util::Split;
bool ParseNum(std::string_view s, int& v) {
    return art_util::ParseInt(s, v);
}

Image Fit(const Image& src, u32 x, u32 y, u32 w, u32 h, u32 ow, u32 oh) {
    return Resize(src, x, y, w, h, std::max<u32>(1, ow), std::max<u32>(1, oh));
}
/// Pillow Image.rotate(deg, expand=True) for multiples of 90 (counter-clockwise).
Image Rot90(const Image& s) {
    Image o{s.height, s.width, std::vector<u8>(s.rgba.size())};
    for (u32 y = 0; y < s.height; ++y)
        for (u32 x = 0; x < s.width; ++x)
            std::memcpy(&o.rgba[((std::size_t{s.width} - 1 - x) * o.width + y) * 4],
                        &s.rgba[(std::size_t{y} * s.width + x) * 4], 4);
    return o;
}
void Fade(Image& img, double f) {
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        img.rgba[i] = static_cast<u8>(img.rgba[i] * f);
}
/// The design's d.window: the card window nine-sliced 14 -> 16 with dark pixels at alpha x a.
void Window(const Image& base, Image& dst, int x, int y, u32 w, u32 h, double a) {
    DrawWindow(base, dst, x, y, w, h, a); // render.py window(): margin trimmed, rim on the box edge
}

// ------------------------------------------------------------------ one composer per key kind
using Parts = std::span<const std::string_view>;
struct ArtCtx {
    const RangeReader& read;
    const AstcFn& astc;
    std::string* why;
};

/// Cell c of a cols x rows UI atlas, fitted to ow x oh.
std::optional<Image> Cell(const ArtCtx& x, MapUi id, int cols, int rows, int c, u32 ow, u32 oh) {
    const auto t = Ui(x.read, id, x.astc, x.why);
    if (!t || c < 0 || c >= cols * rows)
        return std::nullopt;
    const u32 cw = t->width / static_cast<u32>(cols), ch = t->height / static_cast<u32>(rows);
    return Fit(*t, (static_cast<u32>(c) % cols) * cw, (static_cast<u32>(c) / cols) * ch, cw, ch, ow, oh);
}
/// The card window (WindowBase) at w x h with the given dark-pixel alpha.
std::optional<Image> WindowImage(const ArtCtx& x, u32 w, u32 h, double alpha) {
    const auto t = Ui(x.read, MapUi::WindowBase, x.astc, x.why);
    if (!t || w < 32 || h < 32)
        return std::nullopt;
    Image out = Blank(w, h);
    Window(*t, out, 0, 0, w, h, alpha);
    return out;
}

std::optional<Image> ComposeFieldImg(const ArtCtx& x, Parts p) {
    // fieldimg/<pin|-1 surface|-2 underground>/<story variant>/<box>/<x,y,w,h>[/icons]
    if (p.size() != 5 && p.size() != 6)
        return std::nullopt;
    int a = 0, b = 0;
    const auto box = ParseSize(p[3]);
    int r[4]{};
    auto rect = p[4];
    for (int k = 0; k < 4; ++k) {
        const auto comma = rect.find(',');
        if ((k < 3 && comma == std::string_view::npos) ||
            (k == 3 && comma != std::string_view::npos)) return std::nullopt;
        const auto part = rect.substr(0, comma);
        const auto n = std::from_chars(part.data(), part.data()+part.size(), r[k]);
        if (part.empty() || n.ec != std::errc{} || n.ptr != part.data()+part.size()) return std::nullopt;
        if (k < 3) rect.remove_prefix(comma+1);
    }
    if (!ParseNum(p[1],a) || !ParseNum(p[2],b) || !box || box->first > 1240 || box->second > 1080 ||
        a < -2 || a >= static_cast<int>(MapImagePins().size()) || b < 0 || b > 1 ||
        r[0] < 0 || r[1] < 0 || r[2] < 1 || r[3] < 1 || r[2] > 2048 || r[3] > 2048)
        return std::nullopt;
    Image out;
    if (a >= 0) {
        const auto src = MapImage(x.read,a,x.astc,x.why);
        if (!src || r[0] > static_cast<int>(src->width)-r[2] || r[1] > static_cast<int>(src->height)-r[3])
            return std::nullopt;
        out = Fit(*src,r[0],r[1],r[2],r[3],box->first,box->second);
    } else {
        if (r[0] > 4096-r[2] || r[1] > 4096-r[3]) return std::nullopt;
        Image patch = Blank(r[2],r[3]);
        for (int row=r[1]/1024; row<=(r[1]+r[3]-1)/1024; ++row)
            for (int col=r[0]/1024; col<=(r[0]+r[2]-1)/1024; ++col) {
                const int tx=3-row, ty=3-col;
                const bool changed = b && ((a == -1 && tx == 1 && ty == 2) ||
                    (a == -2 && tx >= 1 && tx <= 2 && ty >= 1 && ty <= 2));
                const auto name = "T_UI_Map_FL_" + std::to_string(-a) + "_x" + std::to_string(tx) +
                    "_y" + std::to_string(ty) + (changed ? "_2" : "");
                const int pin = FindMapImage(name);
                if (pin < 0) return std::nullopt;
                const auto tile = MapImage(x.read,pin,x.astc,x.why);
                if (!tile || tile->width != 1024 || tile->height != 1024) return std::nullopt;
                const int x0=std::max(r[0],col*1024), x1=std::min(r[0]+r[2],(col+1)*1024);
                const int y0=std::max(r[1],row*1024), y1=std::min(r[1]+r[3],(row+1)*1024);
                for (int y=y0; y<y1; ++y)
                    std::memcpy(&patch.rgba[(static_cast<std::size_t>(y-r[1])*patch.width+x0-r[0])*4],
                        &tile->rgba[(static_cast<std::size_t>(y-row*1024)*1024+x0-col*1024)*4],
                        static_cast<std::size_t>(x1-x0)*4);
            }
        out = Fit(patch,0,0,patch.width,patch.height,box->first,box->second);
    }
    if (p.size() == 6) {
        const auto atlas = Ui(x.read,MapUi::WorldFacility,x.astc,x.why);
        if (!atlas || atlas->width%4 || atlas->height%12) return std::nullopt;
        auto remaining = p[5]; int count=0;
        while (!remaining.empty()) {
            if (++count > 196) return std::nullopt;
            const auto end=remaining.find(';');
            if (end == std::string_view::npos) return std::nullopt;
            const auto entry=remaining.substr(0,end);
            const auto c1=entry.find(','), c2=entry.find(',',c1+1);
            int cell=0,ex=0,ey=0;
            if (c1 == std::string_view::npos || c2 == std::string_view::npos ||
                !ParseNum(entry.substr(0,c1),cell) || !ParseNum(entry.substr(c1+1,c2-c1-1),ex) ||
                !ParseNum(entry.substr(c2+1),ey) || cell < 0 || cell >= 48 || ex < 0 || ey < 0 ||
                ex > static_cast<int>(out.width) || ey > static_cast<int>(out.height)) return std::nullopt;
            const u32 cw=atlas->width/4,ch=atlas->height/12;
            const Image ic=Fit(*atlas,(cell%4)*cw,(cell/4)*ch,cw,ch,96,96);
            Over(out,ic,ex-48,ey-48);
            remaining.remove_prefix(end+1);
        }
    }
    return out;
}

std::optional<Image> ComposeMapImg(const ArtCtx& x, Parts p) {
    // mapimg/<pin>/<box w>x<h>/<w>x<h>/<x>,<y>/<spot row|->/<visible mask hex>: the map image fitted
    // (LANCZOS) to w x h at (x, y) of a transparent box, with the visible facility icons (map-sized
    // cells, GetFacilityIconIndex) at DispPosition * scale
    if (p.size() != 7 && p.size() != 8)
        return std::nullopt;
    int a = 0;
    const auto box = ParseSize(p[2]);
    const auto size = ParseSize(p[3]);
    const auto comma = p[4].find(',');
    int ox = 0, oy = 0;
    if (!ParseNum(p[1], a) || !box || !size || a < 0 || a >= static_cast<int>(MapImagePins().size()) ||
        comma == std::string_view::npos || !ParseNum(p[4].substr(0, comma), ox) || !ParseNum(p[4].substr(comma + 1), oy))
        return std::nullopt;
    u32 mask = 0;
    if (!art_util::ParseU32(p[6], mask, 16))
        return std::nullopt;
    const auto src = MapImage(x.read, a, x.astc, x.why);
    if (!src)
        return std::nullopt;
    Image img = Fit(*src, 0, 0, src->width, src->height, size->first, size->second);
    const std::string_view spot = p[5];
    Image& out = img;
    if (mask && spot != "-") {
        const auto data = CachedMapData();
        const auto fac = Ui(x.read, MapUi::Facility, x.astc, x.why);
        if (!data || !fac)
            return std::nullopt;
        const auto sp = data->spots.find(spot);
        if (sp == data->spots.end())
            return std::nullopt;
        const double s = static_cast<double>(size->first) / src->width;
        // icon box 70 image px (native Map screen: ~42 px at MapScale 1.13 around a 46 px glyph)
        const u32 ip = std::max<u32>(8, static_cast<u32>(std::lround(70 * s)));
        for (int k = 0; k < 12; ++k) {
            if (!((mask >> k) & 1))
                continue;
            const auto& e = sp->second[static_cast<std::size_t>(k)];
            if (e.type < 1 || e.type > 14)
                continue;
            const int c = FacilityTable[static_cast<std::size_t>(e.type - 1)] + 15;
            const Image ic = Fit(*fac, (static_cast<u32>(c) % 5) * 88, (static_cast<u32>(c) / 5) * 88, 88, 88, ip, ip);
            Over(out, ic, static_cast<int>(std::lround(e.x * s - ip / 2.0)),
                 static_cast<int>(std::lround(e.y * s - ip / 2.0)));
        }
    }
    if (p.size() == 8) {
        const auto atlas = Ui(x.read, MapUi::WorldFacility, x.astc, x.why);
        if (!atlas || atlas->width % 4 || atlas->height % 12) return std::nullopt;
        const u32 cw = atlas->width / 4, ch = atlas->height / 12;
        // The 120px atlas cells contain roughly 40px glyphs. Compensate for
        // that padding to match the native full-world map's icon/map ratio.
        const u32 ip = std::max<u32>(24, static_cast<u32>(std::lround(210.0 * size->first / src->width)));
        std::string_view remaining = p[7]; int count = 0;
        while (!remaining.empty()) {
            if (++count > 196) return std::nullopt;
            const auto end = remaining.find(';');
            if (end == std::string_view::npos) return std::nullopt;
            const auto entry = remaining.substr(0, end);
            const auto c1 = entry.find(','), c2 = entry.find(',', c1 + 1);
            int cell = 0, ex = 0, ey = 0;
            if (c1 == std::string_view::npos || c2 == std::string_view::npos ||
                !ParseNum(entry.substr(0,c1),cell) || !ParseNum(entry.substr(c1+1,c2-c1-1),ex) ||
                !ParseNum(entry.substr(c2+1),ey) || cell < 0 || cell >= 48 || ex < 0 || ey < 0 ||
                ex > static_cast<int>(size->first) || ey > static_cast<int>(size->second)) return std::nullopt;
            const Image ic = Fit(*atlas, (cell % 4)*cw, (cell / 4)*ch, cw,ch,ip,ip);
            Over(img,ic,ex-static_cast<int>(ip)/2,ey-static_cast<int>(ip)/2);
            remaining.remove_prefix(end+1);
        }
    }
    Image boxed = Blank(box->first, box->second);
    Over(boxed, img, ox, oy);
    return boxed;
}

std::optional<Image> ComposeFrame(const ArtCtx& x, Parts p) {
    if (p.size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    const auto t = Ui(x.read, MapUi::Frame, x.astc, x.why);
    if (!size || !t)
        return std::nullopt;
    return Fit(*t, 0, 0, t->width, t->height, size->first, size->second);
}

std::optional<Image> ComposeIcon(const ArtCtx& x, Parts p, MapUi id) {
    // nowloc/<px>, treasure/<px>: the whole texture fitted to px x px
    int a = 0;
    if (p.size() != 2 || !ParseNum(p[1], a) || a <= 0 || a > 256)
        return std::nullopt;
    const auto t = Ui(x.read, id, x.astc, x.why);
    if (!t)
        return std::nullopt;
    return Fit(*t, 0, 0, t->width, t->height, static_cast<u32>(a), static_cast<u32>(a));
}
std::optional<Image> ComposeNowLoc(const ArtCtx& x, Parts p) {
    return ComposeIcon(x, p, MapUi::NowLocation);
}
std::optional<Image> ComposeTreasure(const ArtCtx& x, Parts p) {
    return ComposeIcon(x, p, MapUi::Treasure);
}
std::optional<Image> ComposeQuill(const ArtCtx& x, Parts p) {
    int a = 0;
    if (p.size() != 2 || !ParseNum(p[1], a) || a <= 0 || a > 256)
        return std::nullopt;
    return Cell(x, MapUi::WorldNowLocation, 2, 1, 0, static_cast<u32>(a), static_cast<u32>(a));
}
/// <kind>/<cell>/<px>: one cell of a UI atlas at px x px.
std::optional<Image> ComposeCellKey(const ArtCtx& x, Parts p, MapUi id, int cols, int rows) {
    int a = 0, b = 0;
    if (p.size() != 3 || !ParseNum(p[1], a) || !ParseNum(p[2], b) || b <= 0 || b > 256)
        return std::nullopt;
    return Cell(x, id, cols, rows, a, static_cast<u32>(b), static_cast<u32>(b));
}
std::optional<Image> ComposeFac(const ArtCtx& x, Parts p) {
    return ComposeCellKey(x, p, MapUi::Facility, 5, 6);
}
std::optional<Image> ComposeTsym(const ArtCtx& x, Parts p) {
    return ComposeCellKey(x, p, MapUi::TimeSymbol, 2, 1);
}
std::optional<Image> ComposeCoin(const ArtCtx& x, Parts p) {
    return ComposeCellKey(x, p, MapUi::Coins, 4, 1);
}

std::optional<Image> ComposeTitleBg(const ArtCtx& x, Parts p) {
    if (p.size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    const auto t = Ui(x.read, MapUi::TitleBg, x.astc, x.why);
    if (!size || !t || size->first <= 100 || size->second <= 24)
        return std::nullopt;
    return TitleStrip(*t, size->first, size->second);
}

std::optional<Image> ComposeMLight(const ArtCtx& x, Parts p) {
    if (p.size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    const auto t = Ui(x.read, MapUi::TravelBg, x.astc, x.why);
    if (!size || !t || size->first <= 60 || size->second <= 60)
        return std::nullopt;
    return LightScrap(*t, size->first, size->second);
}

std::optional<Image> ComposeMapBtn(const ArtCtx& x, Parts p) {
    // icon-only Full map button (render.py map_box): opaque window (alpha 1.2) + T_UI_Map_MapMenu_Icon_00
    // (0, 0, 120, 92) at 52 x 40, (18, 24) in an 88 px button
    int a = 0;
    if (p.size() != 2 || !ParseNum(p[1], a) || a < 40 || a > 256)
        return std::nullopt;
    auto out = WindowImage(x, static_cast<u32>(a), static_cast<u32>(a), 1.2);
    const auto t = Ui(x.read, MapUi::MapMenuIcon, x.astc, x.why);
    if (!out || !t)
        return std::nullopt;
    Over(*out, Fit(*t, 0, 0, 120, 92, 52, 40), (a - 52) / 2, (a - 40) / 2);
    return out;
}

std::optional<Image> ComposeStepper(const ArtCtx& x, Parts p) {
    if (p.size() != 3 || p[2].size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    if (!size)
        return std::nullopt;
    // render.py map_box stepper: opaque window, 40 px chevrons centred in the two h x h halves
    auto out = WindowImage(x, size->first, size->second, 1.2);
    const auto t = Ui(x.read, MapUi::Select, x.astc, x.why);
    if (!out || !t)
        return std::nullopt;
    Image up = Rot90(Fit(*t, 0, 0, t->width, t->height, 40, 40)); // the right chevron turned up
    Image dn = Rot90(Rot90(up));
    if (p[2][0] != '1')
        Fade(up, 0.3);
    if (p[2][1] != '1')
        Fade(dn, 0.3);
    const int half = static_cast<int>(size->second), off = (half - 40) / 2;
    Over(*out, up, off, off);
    Over(*out, dn, static_cast<int>(size->first) - half + off, off);
    return out;
}

std::optional<Image> ComposeHeal(const ArtCtx& x, Parts p) {
    if (p.size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    if (!size)
        return std::nullopt;
    // render.py heal_buttons: window 0.88, T_UI_Common_HealEffect_00 at 26 x 34, 20 px in, centred
    auto out = WindowImage(x, size->first, size->second, 0.88);
    const auto t = Ui(x.read, MapUi::HealEffect, x.astc, x.why);
    if (!out || !t)
        return std::nullopt;
    Over(*out, Fit(*t, 0, 0, t->width, t->height, 26, 34), 20, (static_cast<int>(size->second) - 34) / 2);
    return out;
}

std::optional<Image> ComposeSelRow(const ArtCtx&, Parts p) {
    if (p.size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    if (!size)
        return std::nullopt;
    Image out = Blank(size->first, size->second);
    constexpr u8 brown[4] = {60, 45, 28, 255};
    RoundedBox(out, 0, 0, static_cast<int>(size->first) - 1, static_cast<int>(size->second) - 1, 8, brown, brown);
    return out;
}

std::optional<Image> ComposeTabIcon(const ArtCtx& x, Parts p) {
    int b = 0;
    if (p.size() != 3 || !ParseNum(p[2], b) || b <= 0 || b > 128)
        return std::nullopt;
    const u32 s = static_cast<u32>(b);
    if (p[1] == "map") {
        const auto t = Ui(x.read, MapUi::MapMenuIcon, x.astc, x.why);
        return t ? std::optional{Fit(*t, 0, 0, 120, 92, s, static_cast<u32>(s * 0.77))} : std::nullopt;
    }
    // render.py tab_icon(t, s): fitted (not aspect-kept) like the design's fit()
    if (p[1] == "party")
        return Cell(x, MapUi::CharaJob, 5, 3, 0, static_cast<u32>(std::lround(s * 1.2)), s);
    if (p[1] == "journal") {
        const auto t = Ui(x.read, MapUi::Saving, x.astc, x.why);
        return t ? std::optional{Fit(*t, 0, 0, t->width, t->height, static_cast<u32>(std::lround(s * 1.15)), s)}
                 : std::nullopt;
    }
    if (p[1] == "bag") // T_UI_Common_BagIcon_00 stretched to (1.2 s) x s (dq3_info.h ui/bag)
        return ComposeInfoArt(x.read, "ui/bagfit/" + std::to_string(std::lround(s * 1.2)) + "x" + std::to_string(s), x.why);
    return std::nullopt;
}

std::optional<Image> ComposeFTabs(const ArtCtx& x, Parts p) {
    // ftabs/<w>x<h>/<sel>/<icon x0>,..,<x3>/<chevron x>: the revision 7 command window (four tabs,
    // Map . Party . Bag . Journal) with the gold border on <sel> reaching the separators, separators, tab
    // icons and the chevron
    constexpr int N = 4;
    int a = 0;
    if (p.size() != 5)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    if (!size || !ParseNum(p[2], a) || a < 0 || a >= N)
        return std::nullopt;
    const u32 w = size->first, h = size->second;
    auto out = WindowImage(x, w, h, 0.88);
    if (!out)
        return std::nullopt;
    int xs[N]{}, cx = 0;
    {
        auto parts = p[3];
        for (int k = 0; k < N; ++k) {
            const auto c = parts.find(',');
            if (!ParseNum(parts.substr(0, c), xs[k]) || (k + 1 < N) == (c == std::string_view::npos))
                return std::nullopt;
            parts = c == std::string_view::npos ? std::string_view{} : parts.substr(c + 1);
        }
        if (!ParseNum(p[4], cx))
            return std::nullopt;
    }
    // render.py command_tabs(): separators (14 .. h - 14), the selected slot's gold ring
    // (sx + 6, 8, sx + slot - 6, h - 8, radius 8, 3 px), 38 px icons, the 34 px chevron (sizes for h = 88)
    const double slot = w / static_cast<double>(N);
    constexpr u8 gold[4] = {236, 210, 146, 255}, sepc[4] = {110, 106, 92, 255};
    for (int k = 1; k < N; ++k) {
        const int sx = static_cast<int>(k * slot);
        for (int y = 14; y <= static_cast<int>(h) - 14; ++y)
            std::memcpy(&out->rgba[(static_cast<std::size_t>(y) * w + sx) * 4], sepc, 4);
    }
    {
        const double sx = a * slot;
        art_util::RoundShape(*out, static_cast<int>(sx + 6), 8, static_cast<int>(sx + slot - 6), static_cast<int>(h) - 8, 8,
                             3, gold);
    }
    const u32 is = static_cast<u32>(std::lround(h * 38.0 / 88.0));
    static constexpr std::string_view names[N] = {"map", "party", "bag", "journal"};
    for (int k = 0; k < N; ++k) {
        std::string ik = "tabicon/" + std::string{names[k]} + "/" + std::to_string(is);
        const auto ic = ComposeMapArt(x.read, ik, x.astc, x.why);
        if (!ic)
            return std::nullopt;
        Over(*out, *ic, xs[k], static_cast<int>((h - ic->height) / 2));
    }
    const auto sel = Ui(x.read, MapUi::Select, x.astc, x.why);
    if (!sel)
        return std::nullopt;
    const u32 cs = static_cast<u32>(std::lround(h * 34.0 / 88.0));
    Over(*out, Fit(*sel, 0, 0, sel->width, sel->height, cs, cs), cx, static_cast<int>((h - cs) / 2));
    return out;
}

std::optional<Image> ComposeAreaArt(const ArtCtx& x, Parts p) {
    // areaart/<place>/<time frame 0..2>/<w>x<h> (render no-map option_b): the place's TitleDemo still for the
    // time frame, scaled to cover the box (centred, top kept), with the bottom scrim under the caption
    int place = 0, frame = 0;
    if (p.size() != 4 || !ParseNum(p[1], place) || !ParseNum(p[2], frame) || place < 0 ||
        place >= static_cast<int>(std::size(PlaceArtTable)) || frame < 0 || frame > 2)
        return std::nullopt;
    const auto size = ParseSize(p[3]);
    if (!size)
        return std::nullopt;
    const auto t = NoMapTexture(x.read, PlaceArtTable[static_cast<std::size_t>(place)].first + frame, x.why);
    if (!t)
        return std::nullopt;
    const double s = std::max(static_cast<double>(size->first) / t->width, static_cast<double>(size->second) / t->height);
    const u32 bw = static_cast<u32>(std::lround(t->width * s)), bh = static_cast<u32>(std::lround(t->height * s));
    const Image big = Resize(*t, 0, 0, t->width, t->height, std::max(bw, size->first), std::max(bh, size->second));
    const u32 ox = (big.width - size->first) / 2;
    Image out = Resize(big, ox, 0, size->first, size->second, size->first, size->second);
    const double h = size->second;
    for (u32 y = 0; y < out.height; ++y) {
        const double tt = std::max(0.0, (y - h * 0.52) / (h * 0.48));
        const double a = std::floor(210.0 * std::pow(tt, 1.3)) / 255.0;
        if (a <= 0)
            continue;
        for (u32 xx = 0; xx < out.width; ++xx) {
            u8* q = &out.rgba[(std::size_t{y} * out.width + xx) * 4];
            const u8 dark[3] = {10, 8, 6};
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<u8>(std::lround(q[c] * (1.0 - a) + dark[c] * a));
        }
    }
    return out;
}

std::optional<Image> ComposeTownBtn(const ArtCtx& x, Parts p) {
    // townbtn/<w>x<h>: the place card's Town Map button, an opaque window with the Map menu icon (44 x 34)
    if (p.size() != 2)
        return std::nullopt;
    const auto size = ParseSize(p[1]);
    if (!size)
        return std::nullopt;
    auto out = WindowImage(x, size->first, size->second, 1.2);
    const auto t = Ui(x.read, MapUi::MapMenuIcon, x.astc, x.why);
    if (!out || !t)
        return std::nullopt;
    Over(*out, Fit(*t, 0, 0, 120, 92, 44, 34), 24, (static_cast<int>(size->second) - 34) / 2);
    return out;
}

/// Every Map page key kind: IsMapArtKey and ComposeMapArt both use this one table.
struct MapArtKind {
    std::string_view prefix;
    std::optional<Image> (*fn)(const ArtCtx&, Parts);
};
constexpr MapArtKind MapArtKinds[] = {
    {"mapimg/", ComposeMapImg},   {"fieldimg/", ComposeFieldImg}, {"mframe/", ComposeFrame},
    {"nowloc/", ComposeNowLoc},   {"quill/", ComposeQuill},       {"fac/", ComposeFac},
    {"tsym/", ComposeTsym},       {"coin/", ComposeCoin},         {"treasure/", ComposeTreasure},
    {"titlebg/", ComposeTitleBg}, {"mapbtn/", ComposeMapBtn},     {"stepper/", ComposeStepper},
    {"heal/", ComposeHeal},       {"selrow/", ComposeSelRow},     {"ftabs/", ComposeFTabs},
    {"mlight/", ComposeMLight},   {"tabicon/", ComposeTabIcon},  {"areaart/", ComposeAreaArt},
    {"townbtn/", ComposeTownBtn},
};
const MapArtKind* FindKind(std::string_view key) {
    for (const auto& k : MapArtKinds)
        if (key.starts_with(k.prefix))
            return &k;
    return nullptr;
}

// ------------------------------------------------------------------ location overlay caches
// A marker move only changes the "~" suffix: the composed base image (the text before '~') and the
// two unrotated markers are reused (pixels identical to composing them again).
std::mutex overlay_mutex;
std::vector<std::pair<std::string, std::shared_ptr<const Image>>> base_cache; // LRU, most recent last
constexpr std::size_t BaseCacheMax = 4;
std::shared_ptr<const Image> marker_cache[2]; // nowloc/52, quill/64

std::shared_ptr<const Image> CachedBase(const RangeReader& read, std::string_view base, const AstcFn& astc,
                                        std::string* why) {
    {
        std::scoped_lock lock{overlay_mutex};
        for (auto it = base_cache.begin(); it != base_cache.end(); ++it)
            if (it->first == base) {
                auto hit = std::move(*it);
                base_cache.erase(it);
                base_cache.push_back(hit);
                return hit.second;
            }
    }
    auto img = ComposeMapArt(read, base, astc, why);
    if (!img)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{overlay_mutex};
    base_cache.emplace_back(std::string{base}, shared);
    if (base_cache.size() > BaseCacheMax)
        base_cache.erase(base_cache.begin());
    return shared;
}

std::shared_ptr<const Image> CachedMarker(const RangeReader& read, bool quill, const AstcFn& astc, std::string* why) {
    {
        std::scoped_lock lock{overlay_mutex};
        if (marker_cache[quill])
            return marker_cache[quill];
    }
    auto img = ComposeMapArt(read, quill ? "quill/64" : "nowloc/52", astc, why);
    if (!img)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{overlay_mutex};
    marker_cache[quill] = shared;
    return shared;
}

} // namespace

bool IsMapArtKey(std::string_view key) {
    return FindKind(key) != nullptr;
}

std::optional<Image> ComposeMapArt(const RangeReader& read, std::string_view key,
                                   const AstcFn& astc,
                                   std::string* why) {
    // Optional location overlay on a map image. Only one bounded suffix is accepted.
    if (const auto mark = key.find('~'); mark != std::string_view::npos) {
        const auto base = key.substr(0, mark);
        if (!(base.starts_with("mapimg/") || base.starts_with("fieldimg/")) ||
            key.find('~', mark + 1) != std::string_view::npos) return std::nullopt;
        int v[4]{};
        auto args = key.substr(mark + 1);
        for (int k = 0; k < 4; ++k) {
            const auto comma = args.find(',');
            if ((k < 3) != (comma != std::string_view::npos)) return std::nullopt;
            const auto part = args.substr(0, comma);
            const auto n = std::from_chars(part.data(), part.data() + part.size(), v[k]);
            if (part.empty() || n.ec != std::errc{} || n.ptr != part.data() + part.size()) return std::nullopt;
            if (k < 3) args.remove_prefix(comma + 1);
        }
        if (v[0] < 0 || v[0] > 1 || v[1] < -256 || v[1] > 1496 ||
            v[2] < -256 || v[2] > 1336 || v[3] < -360 || v[3] > 360) return std::nullopt;
        const auto cached_base = CachedBase(read, base, astc, why);
        const auto cached_marker = CachedMarker(read, v[0] != 0, astc, why);
        if (!cached_base || !cached_marker || cached_base->width > 1240 || cached_base->height > 1080)
            return std::nullopt;
        Image out = *cached_base; // Over draws into it
        const Image* marker = cached_marker.get();
        Image rotated;
        if (!v[0] && v[3]) {
            // Rotate about the centre, using the same clockwise screen coordinates as the host.
            rotated = Blank(marker->width, marker->height);
            const double rad = v[3] * 3.14159265358979323846 / 180.0;
            const double cs = std::cos(rad), sn = std::sin(rad);
            const double c = (marker->width - 1) / 2.0;
            for (u32 y = 0; y < rotated.height; ++y) for (u32 x = 0; x < rotated.width; ++x) {
                const int sx = static_cast<int>(std::lround(cs * (x-c) + sn * (y-c) + c));
                const int sy = static_cast<int>(std::lround(-sn * (x-c) + cs * (y-c) + c));
                if (sx >= 0 && sy >= 0 && sx < static_cast<int>(marker->width) && sy < static_cast<int>(marker->height))
                    std::memcpy(&rotated.rgba[(static_cast<std::size_t>(y)*rotated.width+x)*4],
                        &marker->rgba[(static_cast<std::size_t>(sy)*marker->width+sx)*4], 4);
            }
            marker = &rotated;
        }
        Over(out, *marker, v[1], v[2]);
        return out;
    }
    const auto* kind = FindKind(key);
    if (!kind)
        return std::nullopt;
    const auto parts = Split(key);
    return kind->fn(ArtCtx{read, astc, why}, parts);
}

} // namespace dq3
