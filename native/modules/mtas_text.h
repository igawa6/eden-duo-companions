// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// My Time at Sandrock: game text decoded at runtime from the player's own romfs
// (StreamingAssets/AssetBundles/localization/<lang>, one AssetItem<Language> MonoBehaviour:
// header + "Text" tag + int32 count + {id:int32, text:UnityStr}[]). Nothing is compiled into the
// module; until the table is loaded every lookup returns "" and the UI shows nothing for it.
#pragma once

#include "core/mods/dsmod_module_abi.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtas {

/// NpcProtoData.birthday decode (self+0x18, a raw GameDateTime tick count -- same encoding and
/// per-day constant as TimeModule.CurrentTime.dateData, confirmed 2026-11-07 by an exact-integer
/// division against the one native-UI-documented example this game ships: Fennec Fox (npc id
/// 8071) birthday_raw=72576000000000 -> day_of_world=84 (exact, zero remainder) ->
/// CalendarFromDayOfWorld(84) = Month 4 (Winter) / Day 1, matching design/CONTENT-SPEC.md's
/// captured native text "Birthday: Day 1 of Winter" exactly. See mtas_text.cpp / DATA-CONTRACT.md.
struct NpcBirthday {
    bool ok{false};
    std::int32_t month{0}; ///< 1..4 (season slot, same convention as mtas::Calendar::month)
    std::int32_t day{0};   ///< 1..28
};

/// SocialLevelConfig-derived progress toward the next relationship level (designer_config's
/// "AssetSocialLevelConfigSocialLevel" table, 26 rows, byte-exact decoded 2026-11-07: see
/// mtas_text.cpp). `level`/`social_type` are FavorData's own mLevel/mType (SocialLevel/SocialType
/// enum ints, re/il2cpp-dump/dump.cs). Reasoned from the real per-(type,level) favor range
/// (`points_needed = range.max + 1 - favor`, `maxed` = no row exists for `level+1` of the same
/// `social_type`) -- the identity/favor/level chain itself is live-verified (LIVE-READS.md); this
/// specific "points to next level" arithmetic was not independently cross-checked against a
/// native heart-pip readout this pass (LIVE-READS.md already flags that pip mapping as an open,
/// pre-existing gap), so callers should treat `points_needed` as reasoned-not-pixel-verified.
struct SocialLevelInfo {
    bool ok{false};        ///< a config row for (social_type, level) was found
    bool maxed{false};     ///< no higher level exists for this social_type; points_needed is 0
    std::int32_t points_needed{0};
};

// ---- Pure table decoders (mtas_text.cpp) -----------------------------------------------------
// Namespace-scope (not nested in Text) so they're directly unit-testable against a small
// synthetic blob, the same way mtas_reader.h's CalendarFromDateData/MissionTimeLeft are -- each
// decodes one MonoBehaviour's already-extracted raw bytes (no romfs/bundle access of its own);
// `Text::Load` is what finds and extracts those bytes at runtime. Every row is byte-exact or the
// whole table is refused (`rows.size() == declared count`, 0 leftover expected in real data);
// bounded by a generous per-table cap, never allocates unboundedly on a corrupt/hostile blob.

struct LocEntry {
    std::int32_t id;
    std::string text;
};
/// AssetItem<Language> (ASSET-PATHS.md §1): header + "Text" tag + int32 count +
/// {id:int32, text:UnityStr}[]. `buf` is that MonoBehaviour's raw serialized bytes (including
/// the fixed 28-byte header -- see mtas_text.cpp's kMonoBehaviourHeader).
std::vector<LocEntry> ParseLocTable(const std::vector<std::uint8_t>& buf);

struct ItemPrototypeRow {
    std::int32_t id;
    std::int32_t name_id;
    std::string male_icon, female_icon;
    std::int32_t info_id{};
};
/// AssetItemPrototypeItem (ASSET-PATHS.md §2, Pathea.ItemNs.ItemPrototype, 31 fields/row).
std::vector<ItemPrototypeRow> ParseItemPrototypeTable(const std::vector<std::uint8_t>& buf);

struct NpcProtoRow {
    std::int32_t id;
    std::int32_t name_id;
    std::uint64_t birthday_raw;
};
/// AssetNpcProtoDataNpc (ASSET-PATHS.md §3, Pathea.NpcNs.NpcProtoData, 41 fields/row).
std::vector<NpcProtoRow> ParseNpcProtoTable(const std::vector<std::uint8_t>& buf);

struct SocialLevelRow {
    std::int32_t social_type, level, range_min, range_max;
    std::int32_t name_id{};
};
/// AssetSocialLevelConfigSocialLevel (Pathea.SocialNs.SocialLevelConfig, 11 fields/row; only the
/// 4 kept here are decoded into the row, the rest are walked and discarded).
std::vector<SocialLevelRow> ParseSocialLevelTable(const std::vector<std::uint8_t>& buf);

// ---- DATA LANE round 3 (2026-11-07) additions: Ingredients (cooking) + Handbook (assembly)
// static recipe tables. Same no-TypeTree, MonoBehaviour-by-name pattern as every table above;
// decode recipe + byte-exact proof (0 leftover against the real designer_config bundle) in
// research/.../tools/parse_cooking.py (pre-existing) and parse_blueprint_tables.py (new this
// round). Materials/counts are never shown without the matching live "known/unlocked" root
// (CookingModule.unlockedCookingFormulaIds / BluePrintModule.unlockedBluePrintDatas,
// mtas_reader.h) -- these tables alone are static game data, not spoiler-free by themselves for
// a not-yet-unlocked recipe, so Reader only looks up a row once the live unlocked-set says so.

struct CookingRow {
    std::int32_t id;         ///< Cooking.id (joins Reader-side only via formulaId, not used to key)
    bool is_active;
    std::int32_t out_item_id; ///< dish item id this formula produces
    std::int32_t formula_id;  ///< joins CookingFormulaRow.id
};
/// AssetCookingConfigCooking (Pathea.CookingNs.CookingConfig, dump.cs TypeDefIndex 11118, 6
/// scalar fields/row, `[DesignerConfigTable("Cooking","Cooking","")]`).
std::vector<CookingRow> ParseCookingTable(const std::vector<std::uint8_t>& buf);

struct CookingFormulaRow {
    std::int32_t id;
    bool is_active;
    std::int32_t cooking_type;
    std::vector<std::int32_t> materials; ///< ingredient item ids, qty 1 each (no per-material
                                         ///< count field exists on this row -- dump.cs's own
                                         ///< declared schema, see ASSET-PATHS.md §6)
};
/// AssetCookingFormulaConfigCookingFormula (CookingFormulaConfig, TypeDefIndex 11119,
/// `[DesignerConfigTable("CookingFormula","CookingFormula","")]`).
std::vector<CookingFormulaRow> ParseCookingFormulaTable(const std::vector<std::uint8_t>& buf);

struct RecipeMaterial {
    std::int32_t item_id;
    std::int32_t count; ///< always 1 this build (CookingFormulaConfig has no per-material count)
};
/// One cooking recipe, joined (Cooking.outItemId/formulaId <-> CookingFormula.id): the Reader's
/// own `GetRecipe(out_item_id)` returns this, "" names/empty materials if either half is missing.
struct RecipeConfigRow {
    std::int32_t out_item_id;
    std::int32_t formula_id;
    std::vector<RecipeMaterial> materials;
};

/// A single (itemId, count, grade) Vector3Int-shaped material entry, shared by MachineAtlas and
/// SyntheticConfig's own "Materials" field (both `[DesignerConfigField("Materials", {';',','})]`
/// Vector3Int[]; grade is always 1 (GradeType.Normal) in every row sampled this round -- kept, not
/// discarded, since a future build could use a higher grade).
struct AssemblyMaterial {
    std::int32_t item_id;
    std::int32_t count;
    std::int32_t grade;
};

struct MachineAtlasRow {
    std::int32_t id;
    std::int32_t item_id; ///< the machine/furniture item this row builds
    std::vector<AssemblyMaterial> materials;
};
/// AssetMachineAtlasMachineAtlas (Pathea.SyntheticNs.MachineAtlas, TypeDefIndex 10893,
/// `[DesignerConfigTable("MachineAtlas","MachineAtlas","")]`) -- machine
/// production recipes, distinct from the Handbook assembly creations.
std::vector<MachineAtlasRow> ParseMachineAtlasTable(const std::vector<std::uint8_t>& buf);

struct SyntheticRow {
    std::int32_t id;
    std::int32_t item_id;
    std::int32_t item_count;
    std::vector<AssemblyMaterial> materials;
};
/// AssetSyntheticConfigSynthetics (Pathea.SyntheticNs.SyntheticConfig, TypeDefIndex 10892,
/// `[DesignerConfigTable("Synthetics","Synthetics","")]`) -- the broader Assembly/Worktable
/// production recipe set, distinct from the Handbook assembly creations.
std::vector<SyntheticRow> ParseSyntheticTable(const std::vector<std::uint8_t>& buf);

/// Native Handbook membership, category and order come from CreationConfig.
struct CreationRow {
    std::int32_t id;
    std::int32_t item_id;
    std::vector<std::int32_t> part_ids;
    std::int32_t priority;
    std::int32_t category; ///< CreationType: 0 Machine, 1 Other
    bool hide_in_locking{false};
    bool full_page{false}; ///< native two-page assembly drawing
};
/// Native visible slots after packing single/spread drawings into Handbook pages. PERF: `unlocked`
/// is indexed into a local set once, up front, instead of `std::find`-scanning it again for every
/// one of `creations`' rows (O(creations.size() x unlocked.size()) before this).
std::vector<CreationRow> HandbookCatalogue(const std::vector<CreationRow>& creations,
                                          const std::vector<std::int32_t>& unlocked);
struct CreationPartRow {
    std::int32_t id;
    AssemblyMaterial material;
};
std::vector<CreationRow> ParseCreationTable(const std::vector<std::uint8_t>& buf);
std::vector<CreationPartRow> ParseCreationPartTable(const std::vector<std::uint8_t>& buf);
struct AssemblyRecipe {
    bool ok{false};
    std::vector<AssemblyMaterial> materials;
    std::int32_t priority{0};
    std::int32_t category{0};
};
/// Join every assembly part; refuse missing parts and combine repeated materials. `creation_by_item`/
/// `part_by_id` are id -> row* indices over the SAME tables `creations`/`parts` were built from
/// (Text::Load builds them once per boot, right after parsing; Text::AssemblyRecipeFor passes its
/// own cached indices) -- PERF: avoids this function's own `std::find_if` linear scan of either
/// table on every call (previously up to ~95 calls/tick, one per unlocked Handbook blueprint, each
/// re-scanning both static tables from scratch).
AssemblyRecipe JoinCreationRecipe(
    std::int32_t item_id,
    const std::unordered_map<std::int32_t, const CreationRow*>& creation_by_item,
    const std::unordered_map<std::int32_t, const CreationPartRow*>& part_by_id);

/// Pure: NpcProtoData.birthday raw tick count -> month/day (see NpcBirthday's own comment for
/// the Fennec Fox proof). Text::NpcBirthdayOf is a thin wrapper of this over the loaded table.
NpcBirthday DecodeNpcBirthday(std::uint64_t birthday_raw);
/// Pure: finds the (social_type, level) row in `levels` and computes points-to-next-level (see
/// SocialLevelInfo's own comment). Text::NextLevelInfo is a thin wrapper of this.
SocialLevelInfo ComputeNextLevelInfo(const std::vector<SocialLevelRow>& levels,
                                     std::int32_t social_type, std::int32_t level,
                                     std::int32_t favor);
/// Pure (Round 8): linear scan for the npc id whose own name (npcs[i].id -> name_id -> loc
/// lookup, same join NpcName itself does) equals `name` exactly; -1 if no row matches. Both
/// tables passed in rather than looked up globally so this is directly unit-testable against a
/// small synthetic roster. Text::NpcIdByName is a thin wrapper of this over the loaded tables.
std::int32_t FindNpcIdByName(const std::vector<NpcProtoRow>& npcs,
                             const std::vector<LocEntry>& loc, const std::string& name);

/// Language enum (dump.cs 3827) to romfs bundle; unknown/unshipped languages use English.
const char* LanguageBundle(std::int32_t language);

class Text {
public:
    /// Load the selected game language, falling back to English if unavailable. Only the
    /// localization table changes on a language switch; designer tables are loaded once.
    bool Load(const EdenDsmodHostApi& host, std::int32_t language = 1);
    bool Loaded() const;
    /// Localized string for a text id, "" if unknown or not loaded.
    std::string Get(std::int32_t id) const;
    std::int32_t CurrentLanguage() const;
    std::string FontText() const;
    /// Item display name: ItemPrototype(id).nameId -> Get(); "" if unknown.
    std::string ItemName(std::int32_t item_id) const;
    std::string ItemDescription(std::int32_t item_id) const;
    /// Item icon sprite name (ItemPrototype male/female icon path), "" if unknown.
    std::string ItemIcon(std::int32_t item_id) const;
    /// NPC display name: NpcProtoData(id).nameID -> Get(); "" if unknown.
    std::string NpcName(std::int32_t npc_id) const;
    /// Locale-invariant Social/calendar sprite key from NpcProtoRow.id.
    std::string NpcPortrait(std::int32_t npc_id) const;
    /// Round 8: reverse of NpcName -- the npc id whose own decoded name equals `name` exactly,
    /// -1 if none. Used to map the live dialogue box's own on-screen speaker name (read directly
    /// off DialogUI.dialogName, mtas_reader.h) back to an npc id, over the SAME already-loaded
    /// roster NpcName itself uses -- never reveals an unmet NPC's name, only resolves one the
    /// player is already looking at on screen.
    std::int32_t NpcIdByName(const std::string& name) const;
    /// NPC birthday (NpcProtoData(id).birthday, decoded); {} (ok=false) if unknown.
    NpcBirthday NpcBirthdayOf(std::int32_t npc_id) const;
    /// Points still needed to reach the next relationship level, or `maxed` if there is none.
    std::string SocialLevelTitle(std::int32_t social_type, std::int32_t level) const;
    SocialLevelInfo NextLevelInfo(std::int32_t social_type, std::int32_t level,
                                  std::int32_t favor) const;
    /// DATA LANE round 3: cooking recipe by CookingModule.unlockedCookingFormulaIds' own id (a
    /// CookingFormulaConfig.id -- that is the live id the reader has; Cooking.formulaId == that
    /// id gives the dish's own `outItemId`, CookingFormula.materials gives the ingredients).
    /// {} (dish_item_id 0, materials empty) if no Cooking row joins to this formula id at all.
    RecipeConfigRow RecipeByFormulaId(std::int32_t formula_id) const;
    /// Native Handbook assembly recipe, decoded from Creation/CreationPart at runtime.
    AssemblyRecipe AssemblyRecipeFor(std::int32_t item_id) const;
    std::vector<CreationRow> HandbookCatalogueFor(const std::vector<std::int32_t>& unlocked) const;

private:
    mutable std::mutex mutex_;
    bool loaded_{false};
    std::string language_;
    std::int32_t language_id_{1};
    std::vector<LocEntry> loc_;              ///< id-sorted ascending
    std::vector<ItemPrototypeRow> items_;    ///< id-sorted ascending
    std::vector<NpcProtoRow> npcs_;          ///< id-sorted ascending
    std::vector<SocialLevelRow> levels_;     ///< small (<=32 rows this build), unsorted
    std::vector<CookingRow> cooking_;              ///< 118 rows this build, unsorted (small)
    std::vector<CookingFormulaRow> cooking_formula_; ///< 118 rows, unsorted
    std::vector<CreationRow> creations_;
    std::vector<CreationPartRow> creation_parts_;
    /// PERF: id -> row* indices over creations_/creation_parts_, built once in Load() right after
    /// those two tables are parsed (never rebuilt afterward -- creations_/creation_parts_ are
    /// themselves only ever set once, on first load; a language switch only replaces loc_). Lets
    /// JoinCreationRecipe/AssemblyRecipeFor skip the linear `std::find_if` scan over either table
    /// that used to re-run on every one of the ~95 unlocked-blueprint lookups, every tick.
    std::unordered_map<std::int32_t, const CreationRow*> creation_by_item_;
    std::unordered_map<std::int32_t, const CreationPartRow*> creation_part_by_id_;
};

/// Process-wide instance used by the reader and the page code.
Text& GameText();

} // namespace mtas
