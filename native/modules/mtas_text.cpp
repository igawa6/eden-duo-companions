// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mtas_text.h"

#include "mtas_reader.h" // mtas::CalendarFromDayOfWorld (birthday decode reuses the shared calendar math)

#include "chained_echoes_unity.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace mtas {

namespace {

constexpr std::string_view kAssetRoot = "Data/StreamingAssets/AssetBundles/";
constexpr std::int32_t kClassMonoBehaviour = 114;
// m_GameObject PPtr (12B) + m_Enabled (4B) + m_Script PPtr (12B) -- same convention
// SerializedFile::PeekName's own class_id==114 fallback uses (chained_echoes_unity.cpp), and the
// one tools/loc_lib.py / parse_item_prototype.py / parse_npc_proto.py derived empirically against
// this game's own bundles (byte-exact: 0 leftover bytes parsing every row of every table below).
constexpr std::size_t kMonoBehaviourHeader = 28;

// Hard per-table caps: bounded memory, refuse rather than allocate unboundedly on a corrupt or
// hostile blob. The real tables are 62,664 (loc) / 3,842 (item) / 139 (npc) / 26 (social level)
// rows -- these caps are generous multiples, not tight fits.
constexpr std::int32_t kMaxLocRows = 200'000;
constexpr std::int32_t kMaxItemRows = 20'000;
constexpr std::int32_t kMaxNpcRows = 2'000;
constexpr std::int32_t kMaxArrayLen = 4'096;
// Generous: some localization rows are long dialogue/lore paragraphs (one real row is 8,365
// bytes, found live while validating this parser against the real romfs -- the original 8,192
// cap was too tight and silently truncated the table 5,290 rows in).
constexpr std::int32_t kMaxStringLen = 65'536;

std::size_t Align4(std::size_t n) {
    return (n + 3) & ~std::size_t{3};
}

/// Bounds-checked little-endian cursor over one object's raw bytes. Never throws; any overrun
/// latches `ok()` false and every further read returns a default value -- callers check `ok()`
/// once at the end of a row, matching the project's "refuse rather than mis-decode" convention.
class Cursor {
public:
    explicit Cursor(const std::vector<std::uint8_t>& buf, std::size_t pos = 0)
        : buf_(buf), pos_(pos) {}

    bool ok() const {
        return ok_;
    }
    std::size_t pos() const {
        return pos_;
    }

    std::int32_t I32() {
        return Get<std::int32_t>();
    }
    std::uint64_t U64() {
        return Get<std::uint64_t>();
    }
    float F32() {
        return Get<float>();
    }
    bool Bool4() {
        return I32() != 0;
    }

    std::string Str() {
        const std::int32_t n = I32();
        if (!ok_)
            return {};
        if (n < 0 || n > kMaxStringLen || pos_ + static_cast<std::size_t>(n) > buf_.size()) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(buf_.data()) + pos_, static_cast<std::size_t>(n));
        pos_ += Align4(static_cast<std::size_t>(n));
        if (pos_ > buf_.size()) {
            ok_ = false;
            return {};
        }
        return s;
    }

    /// int32 count + count*int32 (CookingFormulaConfig.materials / BCL `int[]`).
    std::vector<std::int32_t> IntArray(std::int32_t max_len) {
        const std::int32_t n = I32();
        if (!ok_ || n < 0 || n > max_len) {
            ok_ = false;
            return {};
        }
        std::vector<std::int32_t> out(static_cast<std::size_t>(n));
        for (auto& v : out)
            v = I32();
        return out;
    }

    /// Unity Vector3Int[]: int32 count + count*{x:i32,y:i32,z:i32} (12B/elem, no padding --
    /// confirmed byte-exact offline, research/.../tools/parse_blueprint_tables.py). Used for
    /// MachineAtlas/SyntheticConfig's own "Materials" field ((itemId,count,grade) triples).
    std::vector<AssemblyMaterial> Vec3IntArray(std::int32_t max_len) {
        const std::int32_t n = I32();
        if (!ok_ || n < 0 || n > max_len) {
            ok_ = false;
            return {};
        }
        std::vector<AssemblyMaterial> out(static_cast<std::size_t>(n));
        for (auto& m : out) {
            m.item_id = I32();
            m.count = I32();
            m.grade = I32();
        }
        return out;
    }

    void SkipIntArray() {
        const std::int32_t n = I32();
        if (!ok_ || n < 0 || n > kMaxArrayLen) {
            ok_ = false;
            return;
        }
        Skip(static_cast<std::size_t>(n) * 4);
    }
    void SkipFloatArray() {
        SkipIntArray(); // same element size (4 bytes), same count-prefix shape
    }
    void SkipStrArray() {
        const std::int32_t n = I32();
        if (!ok_ || n < 0 || n > kMaxArrayLen) {
            ok_ = false;
            return;
        }
        for (std::int32_t i = 0; i < n && ok_; ++i)
            Str();
    }
    /// List<SocialLevelDes> / List<{id,count}>-shaped array of two int32 fields.
    void SkipPairArray() {
        const std::int32_t n = I32();
        if (!ok_ || n < 0 || n > kMaxArrayLen) {
            ok_ = false;
            return;
        }
        Skip(static_cast<std::size_t>(n) * 8);
    }
    void Skip(std::size_t n) {
        if (!ok_ || pos_ + n > buf_.size()) {
            ok_ = false;
            return;
        }
        pos_ += n;
    }

private:
    template <class T>
    T Get() {
        T v{};
        if (!ok_ || pos_ + sizeof(T) > buf_.size()) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, buf_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }

    const std::vector<std::uint8_t>& buf_;
    std::size_t pos_;
    bool ok_{true};
};

/// Finds the MonoBehaviour named `name` in `bundle` (e.g. "localization/english",
/// "designer_config") and returns its raw serialized bytes, or nullopt on any failure (missing
/// bundle/romfs file, no such object, read error). Generic: no byte-offset pin is used here --
/// this game's bundles carry no embedded TypeTree (see mtas_art.h's header comment), but
/// `SerializedFile::Objects()`/`PeekName()` already handle a nameless-TypeTree MonoBehaviour's
/// fixed 28-byte header generically (chained_echoes_unity.cpp), so the table's *location* inside
/// the bundle is found by name at runtime, not baked in -- only each row's own field *schema*
/// (declaration order from dump.cs) is compiled in, same as every other reader in this project.
std::optional<std::vector<std::uint8_t>> FindMonoBehaviourBytes(ce_unity::Reader& unity,
                                                                std::string_view bundle,
                                                                std::string_view name) {
    try {
        const auto sf = unity.OpenSerialized(bundle);
        if (!sf)
            return std::nullopt;
        for (const auto& obj : sf->Objects()) {
            if (obj.class_id != kClassMonoBehaviour)
                continue;
            const auto peeked = sf->PeekName(obj);
            if (peeked && (*peeked == name ||
                           (name == "AssetItem" && peeked->starts_with("AssetItem"))))
                return sf->ReadRaw(obj);
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

/// `tag` is read and discarded (every table's own constant "Text"/"Item"/"Npc"/"SocialLevel"
/// marker) -- kept only as a structural sanity divider between m_Name and the row count, matching
/// tools/loc_lib.py's documented recipe exactly.
struct TableHeader {
    bool ok{false};
    std::int32_t count{0};
};
TableHeader ReadTableHeader(Cursor& c) {
    c.Skip(kMonoBehaviourHeader);
    c.Str(); // m_Name
    c.Str(); // tag
    const std::int32_t count = c.I32();
    return {c.ok(), count};
}

} // namespace

// The four parsers below are declared in mtas_text.h (namespace-scope, not nested in Text) so
// they're directly unit-testable against a small synthetic blob; `ReadTableHeader`/`Cursor`
// above stay internal (anonymous-namespace) implementation detail.

std::vector<LocEntry> ParseLocTable(const std::vector<std::uint8_t>& buf) {
    std::vector<LocEntry> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > kMaxLocRows)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        std::string text = c.Str();
        if (!c.ok())
            break;
        rows.push_back({id, std::move(text)});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {}; // partial/corrupt table: refuse the whole thing rather than publish half of it
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return rows;
}

/// Pathea.ItemNs.ItemPrototype (TypeDefIndex 10942, dump.cs L398108), 31 DesignerConfigField
/// members in declaration order (research/.../analysis/ASSET-PATHS.md §2). Only id/nameId/
/// male+femaleIconPath are kept; every other field is walked (to reach the next row) and
/// discarded.
std::vector<ItemPrototypeRow> ParseItemPrototypeTable(const std::vector<std::uint8_t>& buf) {
    std::vector<ItemPrototypeRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > kMaxItemRows)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        std::string male_icon = c.Str();
        std::string female_icon = c.Str();
        const std::int32_t name_id = c.I32();
        const std::int32_t info_id = c.I32(); // localized item description
        c.Str();         // designer
        c.SkipIntArray(); // itemFromTypes
        c.Str();         // maleModelPath
        c.Str();         // femaleModelPath
        c.Str();         // dropModelPath
        c.Str();         // interactableModelPath
        c.SkipIntArray(); // attachModelID
        c.I32();         // stackNumber
        c.SkipIntArray(); // skillIds
        c.Bool4();        // isDefaultKnown
        c.Bool4();        // IsMissionItem
        c.Bool4();        // showroomMark
        c.SkipIntArray(); // dlcRequires
        c.SkipIntArray(); // gradeWeight
        c.I32();         // orderIndex
        c.SkipIntArray(); // tags
        c.I32();         // buyPrice
        c.I32();         // sellPrice.a
        c.I32();         // sellPrice.b
        c.SkipIntArray(); // itemTag
        c.I32();         // holdType
        c.Bool4();        // cantSold
        c.Bool4();        // cantDiscard
        c.Bool4();        // cantRefine
        c.I32();         // dropStackNumber
        c.Str();         // complexModelPath
        c.SkipIntArray(); // blueprintFromId
        if (!c.ok())
            break;
        rows.push_back({id, name_id, std::move(male_icon), std::move(female_icon), info_id});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return rows;
}

/// Pathea.NpcNs.NpcProtoData (TypeDefIndex 3947, dump.cs L122014), 41 fields in declaration order
/// (ASSET-PATHS.md §3). Only id/nameID/birthday are kept.
std::vector<NpcProtoRow> ParseNpcProtoTable(const std::vector<std::uint8_t>& buf) {
    std::vector<NpcProtoRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > kMaxNpcRows)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        c.I32();         // templetID
        const std::int32_t name_id = c.I32();
        c.I32();         // identityID
        c.I32();         // initActorLevel
        const std::uint64_t birthday = c.U64();
        c.F32(); c.F32(); c.F32(); // defaultPos (Vector3)
        c.I32();         // defaultScene
        c.F32(); c.F32(); c.F32(); // homePos (Vector3)
        c.I32();         // homeScene
        c.I32();         // appearSuits
        c.Bool4();        // canLove
        c.SkipFloatArray(); // characteristic
        c.I32();         // initFavor
        c.I32();         // initFavorType
        c.I32();         // initFavorLevel
        c.I32();         // breakup
        c.I32();         // breakupInitiative
        c.I32();         // divorce
        c.I32();         // divorceInitiative
        c.SkipStrArray(); // aiTags
        c.SkipStrArray(); // tags
        c.SkipIntArray(); // defaultBuffs
        c.I32();         // characteristics
        c.SkipIntArray(); // interactChoice
        c.F32(); c.F32(); // partyFoods (Vector2)
        c.F32(); c.F32(); // partyFactor (Vector2)
        c.Str();         // height
        c.Str();         // weight
        c.I32();         // sensorId
        c.I32();         // fovHearId
        c.I32();         // expFactor
        c.SkipPairArray(); // backgrounds (SocialLevelDes[])
        c.SkipPairArray(); // influences (SocialLevelDes[])
        c.I32();         // expPerDay
        c.I32();         // rescueCount
        c.SkipIntArray(); // rescueFavor
        c.SkipIntArray(); // rescueBuffId
        c.SkipIntArray(); // rescueBubbleId
        c.F32();         // exhaustionTime
        c.Bool4();        // preCombineModel
        if (!c.ok())
            break;
        rows.push_back({id, name_id, birthday});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return rows;
}

/// Pathea.SocialNs.SocialLevelConfig (dump.cs), 26 rows this build, byte-exact decoded
/// 2026-11-07 (consumed == total, 0 leftover, verified offline against the real designer_config
/// bundle). `level`/`type` are the raw SocialLevel/SocialType enum ints (the same ints
/// FavorData.mLevel/mType store); `range` is the favor [min,max] window for that level.
std::vector<SocialLevelRow> ParseSocialLevelTable(const std::vector<std::uint8_t>& buf) {
    std::vector<SocialLevelRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 256)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t level = c.I32();
        const std::int32_t type = c.I32();
        const auto name_id = c.I32();  // nameId
        c.I32();  // upgradeTipsId
        c.I32();  // downgradeTipsId
        const std::int32_t range_min = c.I32();
        const std::int32_t range_max = c.I32();
        c.SkipStrArray(); // actions
        c.F32();          // partyProbability
        c.F32(); c.F32(); // confessionProbability (Vector2)
        c.F32(); c.F32(); // proposeProbability (Vector2)
        c.F32(); c.F32(); // birthdayProbability (Vector2)
        if (!c.ok())
            break;
        rows.push_back({type, level, range_min, range_max, name_id});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

// ---- DATA LANE round 3: Cooking (Ingredients) + MachineAtlas/Synthetic (Handbook) tables.
// Row schemas cross-checked byte-exact (0 leftover) against the real designer_config bundle
// offline this round (research/.../tools/parse_cooking.py (pre-existing, Cooking/CookingFormula)
// and parse_blueprint_tables.py (new, MachineAtlas/Synthetic/BluePrint)) -- see mtas_text.h's own
// struct comments for the exact dump.cs field provenance.

std::vector<CookingRow> ParseCookingTable(const std::vector<std::uint8_t>& buf) {
    std::vector<CookingRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 2'000)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        const bool is_active = c.Bool4();
        const std::int32_t out_item_id = c.I32();
        c.F32();                      // costMinutes
        c.I32();                      // gainExp
        const std::int32_t formula_id = c.I32();
        if (!c.ok())
            break;
        rows.push_back({id, is_active, out_item_id, formula_id});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

std::vector<CookingFormulaRow> ParseCookingFormulaTable(const std::vector<std::uint8_t>& buf) {
    std::vector<CookingFormulaRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 2'000)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        const bool is_active = c.Bool4();
        c.Bool4();                     // disableTrying
        const std::int32_t cooking_type = c.I32();
        std::vector<std::int32_t> materials = c.IntArray(kMaxArrayLen);
        if (!c.ok())
            break;
        rows.push_back({id, is_active, cooking_type, std::move(materials)});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

std::vector<MachineAtlasRow> ParseMachineAtlasTable(const std::vector<std::uint8_t>& buf) {
    std::vector<MachineAtlasRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 2'000)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        const std::int32_t item_id = c.I32();
        std::vector<AssemblyMaterial> materials = c.Vec3IntArray(kMaxArrayLen);
        c.I32(); // fromMachineType (MachineTag enum)
        c.I32(); // fromMachineLevel
        c.F32(); // makeTime
        if (!c.ok())
            break;
        rows.push_back({id, item_id, std::move(materials)});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

std::vector<SyntheticRow> ParseSyntheticTable(const std::vector<std::uint8_t>& buf) {
    std::vector<SyntheticRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 2'000)
        return {};
    rows.reserve(static_cast<std::size_t>(hdr.count));
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        const std::int32_t id = c.I32();
        const std::int32_t item_id = c.I32();
        const std::int32_t item_count = c.I32();
        std::vector<AssemblyMaterial> materials = c.Vec3IntArray(kMaxArrayLen);
        c.I32();  // fromMachineType
        c.I32();  // fromMachineLevel
        c.F32();  // makeTime
        c.I32();  // orderId
        c.I32();  // exp
        c.I32();  // proficiencyExp
        if (!c.ok())
            break;
        rows.push_back({id, item_id, item_count, std::move(materials)});
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

std::vector<CreationRow> ParseCreationTable(const std::vector<std::uint8_t>& buf) {
    std::vector<CreationRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 2'000)
        return {};
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        CreationRow row{};
        row.id = c.I32();
        row.item_id = c.I32();
        row.hide_in_locking = c.Bool4();
        c.I32(); // nameId
        c.I32(); // descriptionId
        c.F32(); // makeTime
        c.I32(); // fromMachineLevel
        row.part_ids = c.IntArray(kMaxArrayLen);
        c.Str(); // framePrefabName
        const auto asset_count = c.I32(); // handBookAssetsName
        if (asset_count < 0 || asset_count > kMaxArrayLen)
            return {};
        row.full_page = asset_count == 2;
        for (std::int32_t j = 0; j < asset_count; ++j)
            c.Str();
        c.SkipIntArray(); // page
        row.priority = c.I32();
        c.I32(); // rewardExp
        c.I32(); // rewardProficiencyExp
        c.Str(); // creationIcon
        row.category = c.I32();
        if (!c.ok() || row.category < 0 || row.category > 1)
            return {};
        rows.push_back(std::move(row));
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

std::vector<CreationRow> HandbookCatalogue(const std::vector<CreationRow>& creations,
                                          const std::vector<std::int32_t>& unlocked) {
    // PERF: a hash-set membership test instead of re-scanning `unlocked` with std::find for
    // every row in `creations` (O(creations.size() x unlocked.size()) before this).
    const std::unordered_set<std::int32_t> unlocked_set(unlocked.begin(), unlocked.end());
    std::vector<CreationRow> out;
    for (const auto& row : creations) {
        if (!row.hide_in_locking || unlocked_set.contains(row.item_id))
            out.push_back(row);
    }
    std::stable_sort(out.begin(), out.end(),
        [](const CreationRow& a, const CreationRow& b) { return a.priority > b.priority; });
    std::vector<CreationRow> singles, spreads;
    for (auto& row : out)
        (row.full_page ? spreads : singles).push_back(std::move(row));
    out.clear();
    std::size_t single = 0, spread = 0, page = 0;
    // HandBookModule.CreateCreationPages fills a left/right pair before starting a spread.
    // The native grid deduplicates the spread's two adjacent references into one card.
    while (single < singles.size() || spread < spreads.size()) {
        if (single == singles.size() ||
            (spread < spreads.size() && page % 2 == 0 &&
             (singles[single].priority < spreads[spread].priority ||
              singles.size() - single < 2))) {
            out.push_back(std::move(spreads[spread++]));
            page += page % 2 + 2;
        } else {
            out.push_back(std::move(singles[single++]));
            ++page;
        }
    }
    return out;
}

std::vector<CreationPartRow> ParseCreationPartTable(const std::vector<std::uint8_t>& buf) {
    std::vector<CreationPartRow> rows;
    Cursor c(buf);
    const auto hdr = ReadTableHeader(c);
    if (!hdr.ok || hdr.count < 0 || hdr.count > 10'000)
        return {};
    for (std::int32_t i = 0; i < hdr.count && c.ok(); ++i) {
        CreationPartRow row{};
        row.id = c.I32();
        c.I32(); // descriptionId
        row.material = {c.I32(), c.I32(), c.I32()};
        c.I32(); // partSize
        if (!c.ok())
            return {};
        rows.push_back(row);
    }
    if (static_cast<std::int32_t>(rows.size()) != hdr.count)
        return {};
    return rows;
}

AssemblyRecipe JoinCreationRecipe(
    std::int32_t item_id,
    const std::unordered_map<std::int32_t, const CreationRow*>& creation_by_item,
    const std::unordered_map<std::int32_t, const CreationPartRow*>& part_by_id) {
    const auto creation_it = creation_by_item.find(item_id);
    if (creation_it == creation_by_item.end())
        return {};
    const CreationRow& creation = *creation_it->second;
    AssemblyRecipe out;
    out.priority = creation.priority;
    out.category = creation.category;
    for (const auto part_id : creation.part_ids) {
        const auto part_it = part_by_id.find(part_id);
        if (part_it == part_by_id.end())
            return {};
        const auto& material = part_it->second->material;
        auto existing = std::find_if(out.materials.begin(), out.materials.end(),
            [&material](const AssemblyMaterial& m) {
                return m.item_id == material.item_id && m.grade == material.grade;
            });
        if (existing == out.materials.end())
            out.materials.push_back(material);
        else
            existing->count += material.count;
    }
    out.ok = true;
    return out;
}

const char* LanguageBundle(std::int32_t language) {
    // The shipped Russian bundle is misspelled; Turkish (enum 9) has no bundle.
    switch (language) {
    case 0: return "chinese";
    case 2: return "german";
    case 3: return "french";
    case 4: return "traditional_chinese";
    case 5: return "italian";
    case 6: return "spanish";
    case 7: return "japanese";
    case 8: return "russain";
    case 10: return "korean";
    case 11: return "thailand";
    case 12: return "portuguese";
    default: return "english";
    }
}

bool Text::Load(const EdenDsmodHostApi& host, std::int32_t language) {
    std::lock_guard lock{mutex_};
    const std::string requested = LanguageBundle(language);
    if (loaded_ && language_ == requested)
        return true;
    try {
        ce_unity::RangeReader romfs = ce_unity::MakeHostRangeReader(&host);
        if (!romfs)
            return false;
        ce_unity::Reader unity(romfs, {}, std::string{kAssetRoot});
        const auto load_loc = [&](const std::string& bundle) {
            const auto raw = FindMonoBehaviourBytes(unity, "localization/" + bundle, "AssetItem");
            return raw ? ParseLocTable(*raw) : std::vector<LocEntry>{};
        };
        auto loc = load_loc(requested);
        auto resolved_language = requested == "english" ? 1 : language;
        if (loc.empty() && requested != "english") {
            loc = load_loc("english");
            resolved_language = 1;
        }
        if (loc.empty())
            return false;
        language_id_ = resolved_language;
        // Track the bundle actually loaded, not the one requested: on an English fallback
        // (resolved_language==1 but requested != "english"), `language_` must stay "english" so a
        // later Load() call for the SAME originally-requested language is recognised as not-yet-
        // loaded and retries the real bundle, instead of short-circuiting on the stale match below.
        const std::string loaded_bundle = resolved_language == 1 ? std::string{"english"} : requested;
        if (loaded_) {
            loc_ = std::move(loc);
            language_ = loaded_bundle;
            return true;
        }

        auto item_raw = FindMonoBehaviourBytes(unity, "designer_config", "AssetItemPrototypeItem");
        auto npc_raw = FindMonoBehaviourBytes(unity, "designer_config", "AssetNpcProtoDataNpc");
        auto level_raw =
            FindMonoBehaviourBytes(unity, "designer_config", "AssetSocialLevelConfigSocialLevel");
        auto cooking_raw = FindMonoBehaviourBytes(unity, "designer_config", "AssetCookingConfigCooking");
        auto cooking_formula_raw =
            FindMonoBehaviourBytes(unity, "designer_config", "AssetCookingFormulaConfigCookingFormula");
        auto creation_raw =
            FindMonoBehaviourBytes(unity, "designer_config", "AssetCreationConfigCreation");
        auto creation_part_raw =
            FindMonoBehaviourBytes(unity, "designer_config", "AssetCreationPartConfigCreationPart");

        loc_ = std::move(loc);
        items_ = item_raw ? ParseItemPrototypeTable(*item_raw) : std::vector<ItemPrototypeRow>{};
        npcs_ = npc_raw ? ParseNpcProtoTable(*npc_raw) : std::vector<NpcProtoRow>{};
        levels_ = level_raw ? ParseSocialLevelTable(*level_raw) : std::vector<SocialLevelRow>{};
        cooking_ = cooking_raw ? ParseCookingTable(*cooking_raw) : std::vector<CookingRow>{};
        cooking_formula_ = cooking_formula_raw ? ParseCookingFormulaTable(*cooking_formula_raw)
                                               : std::vector<CookingFormulaRow>{};
        creations_ = creation_raw ? ParseCreationTable(*creation_raw) : std::vector<CreationRow>{};
        creation_parts_ = creation_part_raw ? ParseCreationPartTable(*creation_part_raw)
                                           : std::vector<CreationPartRow>{};
        // PERF: index both static tables once here, not on every JoinCreationRecipe call --
        // creations_/creation_parts_ are never resized again after this point (a later Load()
        // call only ever replaces loc_, see the `if (loaded_)` branch above), so these pointers
        // stay valid for Text's whole lifetime.
        creation_by_item_.clear();
        creation_by_item_.reserve(creations_.size());
        for (const auto& row : creations_)
            creation_by_item_.emplace(row.item_id, &row);
        creation_part_by_id_.clear();
        creation_part_by_id_.reserve(creation_parts_.size());
        for (const auto& row : creation_parts_)
            creation_part_by_id_.emplace(row.id, &row);
        language_ = loaded_bundle;
        loaded_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

std::int32_t Text::CurrentLanguage() const {
    std::lock_guard lock{mutex_};
    return language_id_;
}

bool Text::Loaded() const {
    std::lock_guard lock{mutex_};
    return loaded_;
}

std::string Text::Get(std::int32_t id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(loc_.begin(), loc_.end(), id,
                                     [](const LocEntry& r, std::int32_t v) { return r.id < v; });
    if (it != loc_.end() && it->id == id)
        return it->text;
    return {};
}

std::string Text::ItemName(std::int32_t item_id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(items_.begin(), items_.end(), item_id,
                                     [](const ItemPrototypeRow& r, std::int32_t v) { return r.id < v; });
    if (it == items_.end() || it->id != item_id)
        return {};
    const auto name_it = std::lower_bound(
        loc_.begin(), loc_.end(), it->name_id,
        [](const LocEntry& r, std::int32_t v) { return r.id < v; });
    if (name_it != loc_.end() && name_it->id == it->name_id)
        return name_it->text;
    return {};
}

std::string Text::ItemDescription(std::int32_t item_id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(items_.begin(), items_.end(), item_id,
        [](const ItemPrototypeRow& r, std::int32_t v) { return r.id < v; });
    if (it == items_.end() || it->id != item_id || it->info_id <= 0)
        return {};
    const auto text = std::lower_bound(loc_.begin(), loc_.end(), it->info_id,
        [](const LocEntry& r, std::int32_t v) { return r.id < v; });
    return text != loc_.end() && text->id == it->info_id ? text->text : std::string{};
}

std::string Text::ItemIcon(std::int32_t item_id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(items_.begin(), items_.end(), item_id,
                                     [](const ItemPrototypeRow& r, std::int32_t v) { return r.id < v; });
    if (it == items_.end() || it->id != item_id)
        return {};
    return !it->male_icon.empty() ? it->male_icon : it->female_icon;
}

std::string Text::NpcName(std::int32_t npc_id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(npcs_.begin(), npcs_.end(), npc_id,
                                     [](const NpcProtoRow& r, std::int32_t v) { return r.id < v; });
    if (it == npcs_.end() || it->id != npc_id)
        return {};
    const auto name_it = std::lower_bound(
        loc_.begin(), loc_.end(), it->name_id,
        [](const LocEntry& r, std::int32_t v) { return r.id < v; });
    if (name_it != loc_.end() && name_it->id == it->name_id)
        return name_it->text;
    return {};
}

std::string Text::NpcPortrait(std::int32_t npc_id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(npcs_.begin(), npcs_.end(), npc_id,
        [](const NpcProtoRow& row, std::int32_t id) { return row.id < id; });
    return it == npcs_.end() || it->id != npc_id ? std::string{} : "npc_" + std::to_string(npc_id);
}

std::int32_t Text::NpcIdByName(const std::string& name) const {
    std::lock_guard lock{mutex_};
    return FindNpcIdByName(npcs_, loc_, name);
}

std::int32_t FindNpcIdByName(const std::vector<NpcProtoRow>& npcs,
                             const std::vector<LocEntry>& loc, const std::string& name) {
    if (name.empty())
        return -1;
    // loc is id-sorted (Text::Load's own invariant, same as npcs_); binary-search each
    // candidate's name_id rather than building a reverse map for a roster this small (139 rows
    // this build).
    for (const auto& row : npcs) {
        const auto it = std::lower_bound(loc.begin(), loc.end(), row.name_id,
                                         [](const LocEntry& r, std::int32_t v) { return r.id < v; });
        if (it != loc.end() && it->id == row.name_id && it->text == name)
            return row.id;
    }
    return -1;
}

NpcBirthday DecodeNpcBirthday(std::uint64_t birthday_raw) {
    // Same ticks-per-day constant and Kind-bit mask as TimeModule.CurrentTime.dateData (see this
    // header's own comment, the Fennec Fox proof); CalendarFromDayOfWorld gives month/day
    // directly.
    constexpr std::uint64_t TicksMask = 0x3FFFFFFFFFFFFFFFULL;
    constexpr std::uint64_t TicksPerDay = 24ULL * 60 * 60 * 1000 * 10000;
    const std::uint64_t ticks = birthday_raw & TicksMask;
    const Calendar cal = CalendarFromDayOfWorld(ticks / TicksPerDay);
    return {true, cal.month, cal.day_in_season};
}

SocialLevelInfo ComputeNextLevelInfo(const std::vector<SocialLevelRow>& levels,
                                     std::int32_t social_type, std::int32_t level,
                                     std::int32_t favor) {
    const SocialLevelRow* cur = nullptr;
    bool has_next = false;
    for (const auto& row : levels) {
        if (row.social_type != social_type)
            continue;
        if (row.level == level)
            cur = &row;
        else if (row.level == level + 1)
            has_next = true;
    }
    if (!cur)
        return {};
    if (!has_next)
        return {true, true, 0};
    const std::int32_t needed = cur->range_max + 1 - favor;
    return {true, false, needed > 0 ? needed : 0};
}

NpcBirthday Text::NpcBirthdayOf(std::int32_t npc_id) const {
    std::lock_guard lock{mutex_};
    const auto it = std::lower_bound(npcs_.begin(), npcs_.end(), npc_id,
                                     [](const NpcProtoRow& r, std::int32_t v) { return r.id < v; });
    if (it == npcs_.end() || it->id != npc_id)
        return {};
    return DecodeNpcBirthday(it->birthday_raw);
}

std::string Text::SocialLevelTitle(std::int32_t social_type, std::int32_t level) const {
    std::lock_guard lock{mutex_};
    for (const auto& row : levels_) {
        if (row.social_type == social_type && row.level == level) {
            const auto it = std::lower_bound(loc_.begin(), loc_.end(), row.name_id,
                [](const LocEntry& entry, std::int32_t id) { return entry.id < id; });
            return it != loc_.end() && it->id == row.name_id ? it->text : std::string{};
        }
    }
    return {};
}

SocialLevelInfo Text::NextLevelInfo(std::int32_t social_type, std::int32_t level,
                                    std::int32_t favor) const {
    std::lock_guard lock{mutex_};
    return ComputeNextLevelInfo(levels_, social_type, level, favor);
}

RecipeConfigRow Text::RecipeByFormulaId(std::int32_t formula_id) const {
    std::lock_guard lock{mutex_};
    RecipeConfigRow out;
    out.formula_id = formula_id;
    for (const auto& cooking : cooking_) {
        if (cooking.formula_id != formula_id)
            continue;
        out.out_item_id = cooking.out_item_id;
        for (const auto& formula : cooking_formula_) {
            if (formula.id != formula_id)
                continue;
            out.materials.reserve(formula.materials.size());
            for (const std::int32_t item_id : formula.materials)
                out.materials.push_back({item_id, 1}); // no per-material count field this build
            break;
        }
        break;
    }
    return out;
}

std::vector<CreationRow> Text::HandbookCatalogueFor(const std::vector<std::int32_t>& unlocked) const {
    std::lock_guard lock{mutex_};
    return HandbookCatalogue(creations_, unlocked);
}

AssemblyRecipe Text::AssemblyRecipeFor(std::int32_t item_id) const {
    std::lock_guard lock{mutex_};
    return JoinCreationRecipe(item_id, creation_by_item_, creation_part_by_id_);
}

std::string Text::FontText() const {
    std::lock_guard lock{mutex_};
    std::string out;
    for (const auto& row : loc_)
        out += row.text;
    return out;
}

Text& GameText() {
    static Text instance;
    return instance;
}

} // namespace mtas
