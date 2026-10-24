// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Super Mario Odyssey save reader. Ports a verified reference BYML/save decoder prototype to
// C++: File1.bin is a 16-byte header followed by a BYML v3 document at offset 0x10
// (see the save format notes). Every progress field
// is named in the save's own hash-key table (ShineData, GetCoinCollectData, Checkpoint,
// AchievementSaveData, HackDictionary, WorldInfo, StageName/WorldId, ...).
//
// This file is read-only, host-free: it has no dependency on the module ABI or the save
// extension. Production wiring (polling the active File1–File5 slot through the host's read-only save extension,
// `eden_dsmod_get_save_extensions`) lives in 0100000000010000.cpp.
#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SmoSave {

// Native CollectBgmList.msbt track-name count (SystemMessage.szs, counted via the project's MSBT
// label parser and re-affirmed by a live "82/82" observation on a 100%-complete save fixture).
// The save's own
// CollectBgmList[] array cannot supply this total itself (see SaveProgress::music_total below).
inline constexpr std::int64_t kMusicTrackTotal = 82;

// ---------------------------------------------------------------------------------------------
// Generic BYML ("YB" v2/v3) node tree. See byml.py's module docstring for the empirically
// derived node encoding (node value = (payload << 24 bits) | type in the low byte for most
// nodes; hash entries instead pack (type in the HIGH byte) | (name index in the low 24 bits) --
// the two encodings are genuinely different and are not interchangeable).
// ---------------------------------------------------------------------------------------------

class Node {
public:
    enum class Type : std::uint8_t { Null, Bool, Int, Float, String, Array, Hash };

    Type type = Type::Null;
    bool b = false;
    std::int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::vector<Node> arr;
    std::vector<std::pair<std::string, Node>> hash; ///< insertion order; small (<=~30 keys/record)

    bool IsHash() const {
        return type == Type::Hash;
    }
    bool IsArray() const {
        return type == Type::Array;
    }
    /// Hash lookup by key, or nullptr (not a hash, or the key is absent).
    const Node* Find(std::string_view key) const;
    std::int64_t AsInt(std::int64_t fallback = 0) const;
    bool AsBool(bool fallback = false) const;
    std::string_view AsStringView() const;
};

/// Parses a whole "YB" BYML document (offset 0 = the 'Y' of "YB"). nullopt on any malformed or
/// out-of-range structure -- every offset is bounds-checked against `data.size()`, and recursion
/// depth / total node count are capped (a save file is not adversarial, but a corrupt one must
/// fail closed, not hang or crash).
std::optional<Node> ParseByml(std::span<const std::uint8_t> data);

/// Parses a whole File1.bin: validates the 16-byte header (version word at +0x04 must be 1, the
/// BYML magic must sit at +0x10) and hands the body (length from the +0x0C word) to ParseByml.
std::optional<Node> ParseSaveFile(std::span<const std::uint8_t> file1_bin);

// ---------------------------------------------------------------------------------------------
// High-level progress extraction (save_read.py's summarize(), the counts side).
// ---------------------------------------------------------------------------------------------

struct Checkpoint {
    std::string stage, id;
    bool operator==(const Checkpoint&) const = default;
};
struct CheckpointLocation {
    Checkpoint checkpoint;
    std::array<float, 3> position{};
    std::string name;
};
std::vector<CheckpointLocation> ParseCheckpointTable(std::string_view tsv);

struct SaveProgress {
    bool valid = false;
    std::optional<bool> achievements_available;
    std::map<std::string, std::int64_t> achievement_counters;
    std::string stage_name;
    std::int64_t world_id = -1;
    std::int64_t play_time_s = 0;
    std::int64_t coins_held = 0;    ///< CoinNum: yellow coins currently held
    std::int64_t coins_lifetime = 0; ///< TotalCoinNum: yellow coins collected, lifetime
    std::vector<std::uint32_t> moon_ids;  ///< ShineData[].UniqueId where IsGet == 1
    std::vector<std::uint32_t> coin_ids;  ///< GetCoinCollectData[].UniqueId where IsGet == 1
    std::size_t checkpoint_count = 0;     ///< Checkpoint[] entries with both StageName and ObjId
    std::vector<Checkpoint> checkpoints;
    std::vector<std::string> achievements_claimed; ///< AchievementSaveData[].Name where IsGet==1
    std::vector<int> visited_world_ids; ///< native record index; only seen/unlocked worlds
    std::vector<std::string> captures_seen;        ///< HackDictionary[] non-empty entries

    // ---- Stats-tab extension --------------------------------------------------------------
    // Raw owned item_name strings (not just counts): ItemCap[]/ItemCloth[]/ItemGift[] non-empty
    // entries, in the same item_name namespace as costumes-cap.tsv/costumes-cloth.tsv/
    // items-gift.tsv's own `item_name` column (already joined this way by ApplyTaskRules'
    // TaskItems()/task-item-rules.tsv match, which is the same save fields under a different,
    // task-scoped join). The caller intersects these with the package's static name tables to
    // get "costume sets owned" / "souvenirs owned" against their static totals.
    std::vector<std::string> owned_caps, owned_cloth, owned_gifts;
    // CollectBgmList[] = {BgmResourceName, IsCollected}. Empirically
    // confirmed this session (not just assumed): the array is APPEND-ONLY, growing as tracks are
    // discovered -- every entry ever observed in it is already IsCollected=1 (the sand-16-moons-en
    // fixture has only 8 entries, all collected; the 100-percent-en fixture has exactly 82, all
    // collected). So music_owned is this array's own (collected) entry count, but music_total
    // must NOT be read from the array's size (that would under-report early-game, e.g. "8/8"
    // instead of "8/82") -- it is kMusicTrackTotal below, the native CollectBgmList.msbt track
    // count already verified independently against the game's own romfs table.
    bool music_valid = false;
    std::int64_t music_owned = 0, music_total = 0;
};

/// `root` must be ParseSaveFile's result (a Hash node). Every field degrades to its default
/// (empty/zero) if the save lacks that key, rather than failing the whole extraction -- a save
/// from a different game version may be missing a field this reader does not yet use.
SaveProgress ExtractProgress(const Node& root);

struct Task {
    std::string label, name, hint;
    std::optional<std::int64_t> target;
};
struct TaskProgress {
    bool claimed = false;
    std::optional<std::int64_t> current;
    bool ready = false;
};
std::vector<Task> ParseTaskTable(std::string_view tsv);
TaskProgress ForTask(const SaveProgress&, const Task&);

/// The task list's hint text, as authored, often carries the native game's own line break
/// (e.g. "Bring peace to all kingdoms in\nthe world.") -- that break point is picked for the
/// native UI's own two-line hint box, not for this companion's one-line row preview, and taking
/// only the text before it (the row's old behaviour) can end mid-sentence with nothing to say the
/// rest was cut. RowHint joins every embedded line break back into the single continuous
/// sentence the hint actually is, collapsing any run of whitespace left behind to one space, so
/// the row's own `wrap_width`/`max_lines: 1` label (PACKAGE_FORMAT.md section 3.14) does the
/// fitting -- word-wrapping and, if it still does not fit, ending in a runtime-drawn ellipsis --
/// against real sentence text instead of an arbitrary break point. The gold detail band keeps
/// publishing the untouched `hint` (its own wrap already fits every real hint across up to 3
/// lines; see smo_romfs_tables_test.cpp's SMO_DUMP_HINTS survey).
std::string RowHint(const Task&);

// Native achievement registry joins. IDs and item/world membership come from shipped tables;
// malformed or absent source data leaves only the affected counter unknown.
struct TaskShineRule { std::string label; std::uint32_t id; int weight; std::string group; };
struct TaskWorldRule { int index, clear_main, after_ending, moon_rock; std::string world, sphinx_field; };
struct TaskItemRule { std::string array, item, world; };
struct TaskRules {
    std::vector<TaskShineRule> shines;
    std::set<std::uint32_t> registered_shines;
    std::vector<TaskWorldRule> worlds;
    std::vector<TaskItemRule> items, shops;
};
std::optional<TaskRules> ParseTaskRules(std::string_view shines, std::string_view worlds,
                                        std::string_view items, std::string_view shops,
                                        std::string_view moon_registry = {});
void ApplyTaskRules(const Node& root, SaveProgress& progress, const TaskRules&);

/// Content-based poll cache: same-length saves can contain different progress. Invalid or
/// unchanged reads leave the caller's last good snapshot untouched.
class ProgressCache {
public:
    bool Update(std::span<const std::uint8_t> bytes, SaveProgress& progress, const TaskRules* rules = nullptr);
private:
    std::vector<std::uint8_t> last_bytes_;
};

// ---------------------------------------------------------------------------------------------
// Native-table joins: small TSVs derived from the project's own native-data research and shipped
// as the package's own `file:data/*.tsv` assets (IDs/names/counts only -- no extracted game art;
// see packages/SuperMarioOdyssey/dualscreen/data/README.md for provenance).
// ---------------------------------------------------------------------------------------------

struct KingdomInfo {
    std::string world, name;
    int world_id = -1, shine_type = 0;
};
struct KingdomTables {
    std::vector<KingdomInfo> kingdoms; ///< native WorldList order
    std::map<std::uint32_t, std::string> moon_world; ///< moons.tsv: unique_id -> world
    std::map<std::uint32_t, std::string> coin_world; ///< purple-coins.tsv: unique_id -> world
    std::map<std::string, int> moon_total;           ///< kingdoms.tsv: world -> total moons
    std::map<std::string, int> coin_capacity;        ///< kingdoms.tsv: world -> coin capacity
};

/// Parses the three TSVs (header row required, tab-separated, no quoting -- matching how they
/// were generated). Returns nullopt only if a required column is missing; an empty/absent file
/// (romfs not ready, or this build predates the data files) is reported the same way, so the
/// caller just treats "no tables yet" like "no save yet".
std::optional<KingdomTables> ParseKingdomTables(std::string_view moons_tsv,
                                                std::string_view coins_tsv,
                                                std::string_view kingdoms_tsv);

struct KingdomProgress {
    bool known = false; ///< false if `world` is not in the kingdom table at all
    int moons = 0, moon_total = 0;
    int coins = 0, coin_capacity = 0;
};

/// Native per-kingdom list completion: placement IDs plus Toadette claims in Peach.
/// This counts list entries, not weighted global moon units or shop purchases.
KingdomProgress ForKingdom(const SaveProgress& progress, const KingdomTables& tables,
                          std::string_view world);

struct VisitedMoonTotals {
    int moons = 0, moon_total = 0;
};

/// Sum of collected moons / moon totals across every VISITED kingdom only -- the same
/// per-kingdom rows the globe headline/list and Stats page already read (built by filtering
/// `tables.kingdoms` against `progress.visited_world_ids`, exactly like 0100000000010000.cpp's
/// Sample() builds `stats_rows`/`globe_rows`). Spoiler policy ("Spoiler handling"): an unvisited
/// kingdom is excluded from both
/// the numerator (moons) and the denominator (moon_total) -- its moon_total is never added, even
/// though ForKingdom() could compute one for it regardless of visited state.
VisitedMoonTotals SumVisitedKingdomMoons(const SaveProgress& progress, const KingdomTables& tables);

/// Parses one small two-column TSV (header row required) into name -> name2 pairs, in file order.
/// Used for achievements.tsv (label -> display name) and captures.tsv (hack_name -> capture
/// name); exposed for the unit tests and for a future Tasks/Stats tab join.
std::vector<std::pair<std::string, std::string>> ParseNamePairsTsv(std::string_view tsv);

/// Reads the `item_name` column of one native item table (header row required; same shape as
/// costumes-cap.tsv / costumes-cloth.tsv / items-gift.tsv), as a set. nullopt on a missing
/// column, matching ParseKingdomTables' "no required column -> nullopt" convention.
std::optional<std::set<std::string>> ParseItemNameColumn(std::string_view tsv);

} // namespace SmoSave
