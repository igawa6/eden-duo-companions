// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// BYML v3 parsing + File1.bin save extraction. Conventions in smo_save.h. Section layout and the
// two distinct node-value encodings (node-header "(payload<<8)|type" vs. hash-entry
// "(type<<24)|nameIndex") are ported from a verified reference BYML decoder's own documentation,
// which derived them empirically and validated them by set-equality against the romfs tables
// (research/SAVE-FORMAT.md).

#include "smo_save.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

#include "core/mods/modules/dsmod_module_sdk.h"

#include <set>
#include <charconv>
#include <tuple>
namespace SmoSave {
namespace {
using namespace dsmod_sdk::int_types;
using Span = std::span<const u8>;

// BYML node type tags (the LOW byte of a node header, or the HIGH byte of a hash entry).
constexpr u8 kString = 0xA0;
constexpr u8 kArray = 0xC0;
constexpr u8 kHash = 0xC1;
constexpr u8 kBool = 0xFF;
constexpr u8 kFloat = 0xD2;
// Anything else (0xD1 "int" and any other low byte observed in practice) is read as a plain
// integer payload, matching byml.py's parse_node() fallback ("return payload").

// Bounds, generous for real saves (775 ShineData rows, 1000 GetCoinCollectData rows) but still
// finite, so a corrupt save fails closed instead of hanging or exhausting memory.
constexpr u64 kMaxTableEntries = 8192;
constexpr u32 kMaxArrayCount = 65536;
constexpr u32 kMaxHashCount = 4096;
constexpr int kMaxDepth = 48;
constexpr u64 kMaxNodeBudget = 2'000'000;

constexpr bool Fits(u64 size, u64 o, u64 n) {
    return o <= size && n <= size - o;
}

class Parser {
public:
    explicit Parser(Span data) : d_(data) {}

    std::optional<Node> ParseRoot() {
        if (!Fits(d_.size(), 0, 0x10) || d_[0] != 'Y' || d_[1] != 'B') {
            return std::nullopt;
        }
        const u32 hash_key_off = U32(4);
        const u32 str_off = U32(8);
        const u32 root_off = U32(0xC);
        if (!ParseTable(hash_key_off, hash_keys_) || !ParseTable(str_off, strings_)) {
            return std::nullopt;
        }
        Node out;
        if (!ParseNode(root_off, 0, out)) {
            return std::nullopt;
        }
        return out;
    }

private:
    Span d_;
    std::vector<std::string> hash_keys_;
    std::vector<std::string> strings_;
    u64 node_budget_ = 0;

    u32 U32(u64 o) const {
        return Fits(d_.size(), o, 4) ? dsmod_sdk::Le32(d_.data() + o) : 0;
    }

    bool ParseTable(u32 off, std::vector<std::string>& out) {
        if (off == 0) {
            return true; // an absent table (no hash keys, or no strings) is not an error
        }
        if (!Fits(d_.size(), off, 4)) {
            return false;
        }
        const u32 v = U32(off);
        const u32 count = v >> 8;
        if (count > kMaxTableEntries || !Fits(d_.size(), u64{off} + 4, u64{count} * 4)) {
            return false;
        }
        out.clear();
        out.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            const u32 rel = U32(u64{off} + 4 + 4ull * i);
            const u64 start = u64{off} + rel;
            if (!Fits(d_.size(), start, 1)) {
                return false;
            }
            u64 end = start;
            while (end < d_.size() && d_[end] != 0) {
                ++end;
            }
            if (end >= d_.size()) {
                return false; // unterminated string
            }
            out.emplace_back(reinterpret_cast<const char*>(d_.data() + start), end - start);
        }
        return true;
    }

    bool ParseNode(u64 off, int depth, Node& out) {
        if (depth > kMaxDepth || ++node_budget_ > kMaxNodeBudget || !Fits(d_.size(), off, 4)) {
            return false;
        }
        const u32 v = U32(off);
        const u8 t = static_cast<u8>(v & 0xFF);
        const u32 payload = v >> 8;
        switch (t) {
        case kString:
            if (payload >= strings_.size()) {
                return false;
            }
            out.type = Node::Type::String;
            out.s = strings_[payload];
            return true;
        case kArray:
            return ParseArray(off + 4, payload, depth, out);
        case kHash:
            return ParseHash(off + 4, payload, depth, out);
        case kBool:
            out.type = Node::Type::Bool;
            out.b = payload != 0;
            return true;
        case kFloat: {
            // The node header packs the type into the low byte, so a node-level float loses its
            // low 8 mantissa bits (byml.py: "v & 0xFFFFFF00"). None of this reader's fields are
            // floats at the node level (only as array/hash scalars, a different encoding -- see
            // ParseScalar), but this path is implemented for completeness/robustness.
            const u32 bits = v & 0xFFFFFF00u;
            float fv;
            std::memcpy(&fv, &bits, sizeof fv);
            out.type = Node::Type::Float;
            out.f = fv;
            return true;
        }
        default:
            out.type = Node::Type::Int;
            out.i = payload;
            return true;
        }
    }

    bool ParseArray(u64 data_off, u32 count, int depth, Node& out) {
        if (count > kMaxArrayCount || !Fits(d_.size(), data_off, count)) {
            return false;
        }
        const u64 p = (data_off + count + 3) & ~u64{3};
        if (!Fits(d_.size(), p, u64{count} * 4)) {
            return false;
        }
        out.type = Node::Type::Array;
        out.arr.resize(count);
        for (u32 i = 0; i < count; ++i) {
            const u8 ty = d_[data_off + i];
            const u64 slot = p + 4ull * i;
            if (ty == kArray || ty == kHash) {
                if (!Fits(d_.size(), slot, 4) || !ParseNode(U32(slot), depth + 1, out.arr[i])) {
                    return false;
                }
            } else if (!ParseScalar(ty, slot, out.arr[i])) {
                return false;
            }
        }
        return true;
    }

    bool ParseHash(u64 entries_off, u32 count, int depth, Node& out) {
        if (count > kMaxHashCount || !Fits(d_.size(), entries_off, u64{count} * 8)) {
            return false;
        }
        out.type = Node::Type::Hash;
        out.hash.clear();
        out.hash.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            const u64 e_off = entries_off + 8ull * i;
            const u32 e = U32(e_off);
            const u32 nidx = e & 0xFFFFFFu;
            const u8 etype = static_cast<u8>(e >> 24);
            const u64 slot = e_off + 4;
            std::string key = nidx < hash_keys_.size() ? hash_keys_[nidx]
                                                       : ("#" + std::to_string(nidx));
            Node val;
            if (etype == kArray || etype == kHash) {
                if (!Fits(d_.size(), slot, 4) || !ParseNode(U32(slot), depth + 1, val)) {
                    return false;
                }
            } else if (!ParseScalar(etype, slot, val)) {
                return false;
            }
            out.hash.emplace_back(std::move(key), std::move(val));
        }
        return true;
    }

    bool ParseScalar(u8 t, u64 slot, Node& out) {
        if (!Fits(d_.size(), slot, 4)) {
            return false;
        }
        const u32 v = U32(slot);
        switch (t) {
        case kString:
            // A scalar slot carries the full raw value (unlike a node header, where the type tag
            // shares the low byte); byml.py falls back to a placeholder string rather than
            // failing the whole parse on an out-of-range index, so this does too.
            out.type = Node::Type::String;
            out.s = v < strings_.size() ? strings_[v] : std::string{};
            return true;
        case kBool:
            out.type = Node::Type::Bool;
            out.b = v != 0;
            return true;
        case kFloat: {
            float fv;
            std::memcpy(&fv, &v, sizeof fv);
            out.type = Node::Type::Float;
            out.f = fv;
            return true;
        }
        default:
            out.type = Node::Type::Int;
            out.i = v;
            return true;
        }
    }
};

} // namespace

const Node* Node::Find(std::string_view key) const {
    if (type != Type::Hash) {
        return nullptr;
    }
    for (const auto& [k, v] : hash) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

std::int64_t Node::AsInt(std::int64_t fallback) const {
    switch (type) {
    case Type::Int:
        return i;
    case Type::Bool:
        return b ? 1 : 0;
    case Type::Float:
        return static_cast<std::int64_t>(f);
    default:
        return fallback;
    }
}

bool Node::AsBool(bool fallback) const {
    switch (type) {
    case Type::Bool:
        return b;
    case Type::Int:
        return i != 0;
    default:
        return fallback;
    }
}

std::string_view Node::AsStringView() const {
    return type == Type::String ? std::string_view{s} : std::string_view{};
}

std::optional<Node> ParseByml(Span data) {
    try {
        return Parser(data).ParseRoot();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Node> ParseSaveFile(Span file1_bin) {
    if (!Fits(file1_bin.size(), 0, 0x10)) {
        return std::nullopt;
    }
    const u32 version = dsmod_sdk::Le32(file1_bin.data() + 4);
    if (version != 1) {
        return std::nullopt;
    }
    const u32 byml_size = dsmod_sdk::Le32(file1_bin.data() + 0xC);
    if (byml_size < 0x10 || !Fits(file1_bin.size(), 0x10, byml_size)) {
        return std::nullopt;
    }
    return ParseByml(file1_bin.subspan(0x10, byml_size));
}

bool ProgressCache::Update(Span bytes, SaveProgress& progress, const TaskRules* rules) {
    if (bytes.size() == last_bytes_.size() &&
        std::equal(bytes.begin(), bytes.end(), last_bytes_.begin())) return false;
    const auto root = ParseSaveFile(bytes);
    if (!root) return false;
    auto next = ExtractProgress(*root);
    if (!next.valid) return false;
    if (rules) ApplyTaskRules(*root, next, *rules);
    last_bytes_.assign(bytes.begin(), bytes.end());
    progress = std::move(next);
    return true;
}

SaveProgress ExtractProgress(const Node& root) {
    SaveProgress out;
    if (!root.IsHash()) {
        return out;
    }
    out.valid = true;
    if (const auto* v = root.Find("IsUnlockAchievement"); v &&
        (v->type == Node::Type::Int || v->type == Node::Type::Bool))
        if (const auto state = v->AsInt(-1); state == 0 || state == 1)
            out.achievements_available = state == 1;
    for (const auto* key : {"TotalCoinNum", "PlayerJumpCount", "PlayerThrowCapCount"})
        if (const auto* v = root.Find(key); v && v->type == Node::Type::Int && v->i >= 0)
            out.achievement_counters[key] = v->i;
    for (const auto* key : {"ItemCap", "ItemCloth"})
        if (const auto* v = root.Find(key); v && v->IsArray()) {
            std::int64_t count = 0;
            for (const auto& item : v->arr)
                if (item.type == Node::Type::String && !item.s.empty()) ++count;
            out.achievement_counters[key] = count;
        }
    // Raw owned item_name strings (Stats-tab extension: costume-set / souvenir ownership). Same
    // three array fields as above plus ItemGift, but keeping the actual names, not just a count,
    // so the caller can join them against the package's static costumes-cap.tsv/costumes-
    // cloth.tsv/items-gift.tsv name tables.
    for (const auto& [key, dest] : {std::pair{"ItemCap", &out.owned_caps}, std::pair{"ItemCloth", &out.owned_cloth},
                                    std::pair{"ItemGift", &out.owned_gifts}})
        if (const auto* v = root.Find(key); v && v->IsArray())
            for (const auto& item : v->arr)
                if (item.type == Node::Type::String && !item.s.empty()) dest->push_back(item.s);
    // CollectBgmList[] = {BgmResourceName, IsCollected} (research/SAVE-FORMAT.md). Unlike
    // ShineData/GetCoinCollectData, this array is APPEND-ONLY (grows as tracks are discovered,
    // not a fixed 82-slot roster with placeholders) -- confirmed this session against two
    // fixtures (8 entries / 8 collected on sand-16-moons-en; 82 / 82 on 100-percent-en, matching
    // RE batch 9's live finding exactly). So the array's own size is the OWNED count, never the
    // TOTAL (kMusicTrackTotal, a verified native constant) -- reading the array's size as the
    // total would under-report early-game progress as already-complete.
    if (const auto* bgm = root.Find("CollectBgmList"); bgm && bgm->IsArray()) {
        std::int64_t owned = 0;
        bool saw_entry = false;
        for (const auto& e : bgm->arr) {
            const auto* name = e.Find("BgmResourceName");
            if (!name || name->AsStringView().empty()) continue; // placeholder slot
            saw_entry = true;
            const auto* got = e.Find("IsCollected");
            if (got && got->AsBool()) ++owned;
        }
        // An empty-but-present array (a genuine fresh save, zero tracks found yet) is still a
        // valid read -- bgm->arr.empty() is not the same as the key being absent entirely.
        out.music_valid = saw_entry || bgm->arr.empty();
        out.music_owned = owned;
        out.music_total = kMusicTrackTotal;
    }
    if (const auto* v = root.Find("StageName")) {
        out.stage_name = std::string{v->AsStringView()};
    }
    if (const auto* v = root.Find("WorldId")) {
        out.world_id = v->AsInt(-1);
    }
    if (const auto* v = root.Find("PlayTimeTotal")) {
        out.play_time_s = v->AsInt(0);
    }
    if (const auto* v = root.Find("CoinNum")) {
        out.coins_held = v->AsInt(0);
    }
    if (const auto* v = root.Find("TotalCoinNum")) {
        out.coins_lifetime = v->AsInt(0);
    }
    const auto* game_progress = root.Find("GameProgressData");
    const auto* first_time = game_progress ? game_progress->Find("IsFirstTimeWorld") : nullptr;
    const auto* world_info = root.Find("WorldInfo");
    if (first_time && first_time->IsArray() && world_info && world_info->IsArray()) {
        const auto count = std::min<std::size_t>({17, first_time->arr.size(), world_info->arr.size()});
        for (std::size_t i = 0; i < count; ++i) {
            const auto* unlocked = world_info->arr[i].Find("UnlockedWorldId");
            if (first_time->arr[i].AsInt(1) == 0 && unlocked &&
                unlocked->AsInt(-1) >= 0 && unlocked->AsInt(-1) < 17)
                out.visited_world_ids.push_back(static_cast<int>(i));
        }
    }
    if (const auto* shine = root.Find("ShineData"); shine && shine->IsArray()) {
        for (const auto& e : shine->arr) {
            const auto* get = e.Find("IsGet");
            const auto* uid = e.Find("UniqueId");
            if (get && get->AsBool() && uid) {
                out.moon_ids.push_back(static_cast<std::uint32_t>(uid->AsInt()));
            }
        }
    }
    if (const auto* coins = root.Find("GetCoinCollectData"); coins && coins->IsArray()) {
        for (const auto& e : coins->arr) {
            const auto* get = e.Find("IsGet");
            const auto* uid = e.Find("UniqueId");
            if (get && get->AsBool() && uid) {
                out.coin_ids.push_back(static_cast<std::uint32_t>(uid->AsInt()));
            }
        }
    }
    if (const auto* cps = root.Find("Checkpoint"); cps && cps->IsArray()) {
        for (const auto& e : cps->arr) {
            const auto* stage = e.Find("StageName");
            const auto* obj = e.Find("ObjId");
            // Empty-string array slots are placeholder entries (research/SAVE-FORMAT.md): both
            // fields must be non-empty, matching save_read.py's "if e.get(...) and e.get(...)".
            if (stage && !stage->AsStringView().empty() && obj && !obj->AsStringView().empty()) {
                ++out.checkpoint_count;
                out.checkpoints.push_back({std::string{stage->AsStringView()},
                                           std::string{obj->AsStringView()}});
            }
        }
    }
    if (const auto* cps = root.Find("Checkpoint"); cps && cps->IsArray())
        out.achievement_counters["Checkpoint"] = out.checkpoint_count;
    if (const auto* ach = root.Find("AchievementSaveData"); ach && ach->IsArray()) {
        for (const auto& e : ach->arr) {
            const auto* get = e.Find("IsGet");
            const auto* name = e.Find("Name");
            if (get && get->AsBool() && name && !name->AsStringView().empty()) {
                out.achievements_claimed.emplace_back(name->AsStringView());
            }
        }
    }
    if (const auto* hacks = root.Find("HackDictionary"); hacks && hacks->IsArray()) {
        for (const auto& e : hacks->arr) {
            if (e.type == Node::Type::String && !e.s.empty()) {
                out.captures_seen.push_back(e.s);
            }
        }
    }
    if (const auto* hacks = root.Find("HackDictionary"); hacks && hacks->IsArray())
        out.achievement_counters["HackDictionary"] = out.captures_seen.size();
    return out;
}

namespace {
/// Splits `tsv` into non-empty lines (accepting both "\n" and "\r\n"), each split into tab
/// fields. The header row is returned as element 0 along with every data row -- callers index by
/// column position after checking the header, matching how these files are generated (fixed
/// column order, never reordered).
std::vector<std::vector<std::string_view>> SplitTsv(std::string_view tsv) {
    std::vector<std::vector<std::string_view>> rows;
    std::size_t pos = 0;
    while (pos <= tsv.size()) {
        const auto nl = tsv.find('\n', pos);
        std::string_view line = tsv.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                                            : nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            std::vector<std::string_view> fields;
            std::size_t fp = 0;
            while (true) {
                const auto tab = line.find('\t', fp);
                fields.push_back(line.substr(fp, tab == std::string_view::npos ? std::string_view::npos
                                                                               : tab - fp));
                if (tab == std::string_view::npos) {
                    break;
                }
                fp = tab + 1;
            }
            rows.push_back(std::move(fields));
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return rows;
}

/// Index of `name` in the header row, or -1.
int ColumnIndex(const std::vector<std::string_view>& header, std::string_view name) {
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
} // namespace

std::vector<CheckpointLocation> ParseCheckpointTable(std::string_view tsv) {
    const auto rows = SplitTsv(tsv);
    if (rows.empty() || rows.front() != std::vector<std::string_view>{"stage", "id", "x", "y", "z", "name"})
        return {};
    std::vector<CheckpointLocation> out;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.size() != 6 || r[0].empty() || r[1].empty() || r[5].empty()) continue;
        CheckpointLocation location{{std::string{r[0]}, std::string{r[1]}}, {}, std::string{r[5]}};
        bool valid = true;
        for (int axis = 0; axis < 3; ++axis) {
            const std::string field{r[axis + 2]};
            char* end = nullptr;
            location.position[axis] = std::strtof(field.c_str(), &end);
            valid &= end == field.c_str() + field.size() && end != field.c_str() &&
                     std::isfinite(location.position[axis]) && std::abs(location.position[axis]) < 1e7f;
        }
        if (valid && std::none_of(out.begin(), out.end(), [&](const auto& old) {
                return old.checkpoint == location.checkpoint;
            })) out.push_back(std::move(location));
    }
    return out;
}

std::vector<Task> ParseTaskTable(std::string_view tsv) {
    const auto rows = SplitTsv(tsv);
    if (rows.empty() || rows[0] != std::vector<std::string_view>{"label", "name", "hint", "target"}) return {};
    std::vector<Task> tasks;
    for (std::size_t i = 1; i < rows.size() && tasks.size() < 128; ++i) {
        const auto& r = rows[i];
        if (r.size() != 4 || r[0].empty() || r[1].empty() || r[2].empty()) continue;
        Task task{std::string{r[0]}, std::string{r[1]}, std::string{r[2]}, {}};
        if (!r[3].empty()) {
            std::int64_t n = 0;
            if (r[3].size() >= 10) continue;
            bool valid = true;
            for (const char c : r[3]) {
                if (c < '0' || c > '9') { valid = false; break; }
                n = n*10 + c-'0';
            }
            if (!valid || n <= 0) continue;
            task.target = n;
        }
        for (std::size_t p = 0; (p = task.hint.find("\\n", p)) != std::string::npos; ++p)
            task.hint.replace(p, 2, "\n");
        if (std::none_of(tasks.begin(), tasks.end(), [&](const auto& old) { return old.label == task.label; }))
            tasks.push_back(std::move(task));
    }
    return tasks;
}

std::string RowHint(const Task& task) {
    std::string out;
    out.reserve(task.hint.size());
    bool prev_space = false;
    for (const char c : task.hint) {
        const bool space = c == '\n' || c == ' ' || c == '\t';
        if (space) {
            if (!prev_space && !out.empty()) out.push_back(' ');
        } else {
            out.push_back(c);
        }
        prev_space = space;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

namespace {
bool TaskNumber(std::string_view text, int& out) {
    if(text.empty())return false;
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),out);
    return error==std::errc{} && end==text.data()+text.size() && out>=0;
}
std::optional<bool> TaskBool(const Node* n) {
    if(!n || (n->type!=Node::Type::Int && n->type!=Node::Type::Bool))return {};
    const auto v=n->AsInt(-1);
    return v==0 || v==1 ? std::optional<bool>(v!=0) : std::nullopt;
}
std::optional<std::set<std::string>> TaskItems(const Node& root, const std::string& name) {
    const auto* array=root.Find(name);
    if(!array || !array->IsArray())return {};
    std::set<std::string> items;
    for(const auto& item:array->arr){
        if(item.type!=Node::Type::String)return {};
        if(!item.s.empty())items.insert(item.s);
    }
    return items;
}
}
std::optional<TaskRules> ParseTaskRules(std::string_view shines, std::string_view worlds,
                                      std::string_view items, std::string_view shops, std::string_view moon_registry) {
    TaskRules out;
    const auto sr=SplitTsv(shines),wr=SplitTsv(worlds),ir=SplitTsv(items),pr=SplitTsv(shops);
    // `shines` alone may be entirely empty (not even a header row), as a defensive fallback for a
    // romfs read that failed for just that one table: ApplyTaskRules already degrades every
    // Shine_*/MiniGame_* achievement counter individually when `shines` has no matching entries,
    // the same "fail closed per domain" rule it applies to a malformed/absent save field. In
    // normal operation smo_romfs_tables.cpp builds this table from the game's own AchievementInfo
    // dispatch predicates, so it is populated. The world/item/shop tables are still required with
    // their exact header.
    if((!sr.empty() && sr[0]!=std::vector<std::string_view>{"label","unique_id","weight","group"}) ||
       wr.empty() || wr[0]!=std::vector<std::string_view>{"world_id","world","clear_main","after_ending","moon_rock","sphinx_field"} ||
       ir.empty() || ir[0]!=std::vector<std::string_view>{"save_array","item_name"} ||
       pr.empty() || pr[0]!=std::vector<std::string_view>{"world","save_array","item_name"})return {};
    std::set<std::pair<std::string,int>> seen_shines;
    for(size_t i=1;i<sr.size();++i){
        const auto& row=sr[i];int id=0,weight=0;
        if(row.size()!=4 || row[0].empty() || !TaskNumber(row[1],id) || !id ||
           !TaskNumber(row[2],weight) || weight<1 || weight>8 ||
           !seen_shines.emplace(std::string(row[0]),id).second)return {};
        out.shines.push_back({std::string(row[0]),std::uint32_t(id),weight,std::string(row[3])});
    }
    for(size_t i=1;i<wr.size();++i){
        const auto& row=wr[i];TaskWorldRule w{};
        if(row.size()!=6 || !TaskNumber(row[0],w.index) || w.index!=int(i-1) || row[1].empty() ||
           !TaskNumber(row[2],w.clear_main) || !TaskNumber(row[3],w.after_ending) || !TaskNumber(row[4],w.moon_rock) ||
           (row[5]!="IsAnswerCorrectSphinxQuiz" && row[5]!="IsAnswerCorrectSphinxQuizAll"))return {};
        w.world=row[1];w.sphinx_field=row[5];out.worlds.push_back(std::move(w));
    }
    const auto array_ok=[](std::string_view s){return s=="ItemGift" || s=="ItemSticker" || s=="ItemCap" || s=="ItemCloth";};
    std::set<std::pair<std::string,std::string>> seen_items;
    for(size_t i=1;i<ir.size();++i){
        const auto& row=ir[i];
        if(row.size()!=2 || !array_ok(row[0]) || row[1].empty() ||
           !seen_items.emplace(std::string(row[0]),std::string(row[1])).second)return {};
        out.items.push_back({std::string(row[0]),std::string(row[1]),{}});
    }
    std::set<std::tuple<std::string,std::string,std::string>> seen_shops;
    for(size_t i=1;i<pr.size();++i){
        const auto& row=pr[i];
        if(row.size()!=3 || row[0].empty() || !array_ok(row[1]) || row[2].empty() ||
           !seen_shops.emplace(std::string(row[0]),std::string(row[1]),std::string(row[2])).second)return {};
        out.shops.push_back({std::string(row[1]),std::string(row[2]),std::string(row[0])});
    }
    if(!moon_registry.empty()){
        const auto rows=SplitTsv(moon_registry);
        if(rows.empty())return {};
        const auto column=ColumnIndex(rows[0],"unique_id");
        if(column<0)return {};
        for(size_t i=1;i<rows.size();++i){
            int id=0;
            if(rows[i].size()<=size_t(column) || !TaskNumber(rows[i][column],id) || !id || !out.registered_shines.insert(std::uint32_t(id)).second)return {};
        }
    }
    // `shines` may legitimately be empty (the per-achievement moon/coin weighting and grouping
    // is not derivable from the player's own romfs with the tools this port has -- see
    // smo_romfs_tables.h -- so the package no longer ships it); ApplyTaskRules already degrades
    // every Shine_* achievement counter individually when `shines` has no matching entries, the
    // same "fail closed per domain" rule it already applies to a malformed/absent save field.
    // The world/item/shop rules are still required: this file always builds them from romfs.
    if(out.worlds.size()!=17 || out.items.empty() || out.shops.empty())return {};
    return out;
}
void ApplyTaskRules(const Node& root, SaveProgress& p, const TaskRules& rules) {
    if(!p.valid || rules.worlds.size()!=17)return;
    for(const auto& r:rules.shines)p.achievement_counters.erase(r.label);
    for(const char* label:{"Scenario_Ending","Scenario_WorldAll","Other_MoonStoneAll","Other_WorldWarpHoleAll","MiniGame_SphinxQuiz","Souvenir_Count_1","Souvenir_Count_2","Souvenir_Count_3","Shine_CollectCoinShop"})p.achievement_counters.erase(label);
    if(p.achievements_available.has_value())p.achievement_counters["Scenario_Ending"]=*p.achievements_available?1:0;
    const auto* shine=root.Find("ShineData");
    if(shine && shine->IsArray()){
        bool valid=true;std::set<std::uint32_t> collected;
        for(const char* name:{"Shine","GrandShine"}){
            const auto* legacy=root.Find(name);
            if(legacy && (!legacy->IsArray() || !legacy->arr.empty()))valid=false;
        }
        for(const auto& row:shine->arr){
            const auto got=TaskBool(row.Find("IsGet"));
            if(!got){valid=false;break;}
            if(!*got)continue;
            const auto* id=row.Find("UniqueId");
            if(!id || id->type!=Node::Type::Int || id->i<=0 || id->i>UINT32_MAX){valid=false;break;}
            if(!rules.registered_shines.empty() && !rules.registered_shines.contains(std::uint32_t(id->i))){valid=false;break;}
            collected.insert(std::uint32_t(id->i));
        }
        if(valid){
            std::map<std::string,std::int64_t> counts;
            std::map<std::string,std::set<std::string>> groups;
            for(const auto& r:rules.shines){
                counts.try_emplace(r.label,0);
                if(!collected.contains(r.id))continue;
                if(r.group.empty())counts[r.label]+=r.weight;
                else groups[r.label].insert(r.group);
            }
            for(const auto& [label,count]:counts)p.achievement_counters[label]=groups.contains(label)?groups[label].size():count;
        }
    }
    const auto* info=root.Find("WorldInfo");
    // Native acquired sum rebuild: non-shop placement units + all20 ShopShine slots
    // + claimed Toadette rewards. Legacy record arrays are resolved by the game and
    // are intentionally unavailable here until modern autosave or a live override.
    bool gather_valid=info && info->IsArray() && info->arr.size()>=20;
    std::int64_t extra_units=0;
    if(gather_valid)for(size_t i=0;i<20;++i){
        const auto* n=info->arr[i].Find("ShopShine");
        if(!n || n->type!=Node::Type::Int || n->i<0 || n->i>1000000){gather_valid=false;break;}
        extra_units+=n->i;
    }
    const auto* awards=root.Find("AchievementSaveData");
    if(!awards || !awards->IsArray())gather_valid=false;
    else{
        std::set<std::string> claimed;
        for(const auto& row:awards->arr){
            const auto got=TaskBool(row.Find("IsGet"));
            const auto* name=row.Find("Name");
            if(!got || !name || name->type!=Node::Type::String || (*got && name->s.empty())){gather_valid=false;break;}
            if(*got)claimed.insert(name->s);
        }
        extra_units+=claimed.size();
    }
    for(const char* name:{"Shine","GrandShine"}){
        const auto* legacy=root.Find(name);
        if(legacy && (!legacy->IsArray() || !legacy->arr.empty()))gather_valid=false;
    }
    for(int i=1;i<=3;++i){
        const auto key="Shine_Gather_"+std::to_string(i);
        if(!gather_valid)p.achievement_counters.erase(key);
        else if(auto found=p.achievement_counters.find(key);found!=p.achievement_counters.end())found->second+=extra_units;
    }
    if(info && info->IsArray()){
        bool world_valid=info->arr.size()>=14,rock_valid=info->arr.size()>=17;
        int peaceful=0,rocks=0;
        for(size_t i=0;i<17 && i<info->arr.size();++i){
            const auto* n=info->arr[i].Find("ScenarioNo");
            const bool valid=n && n->type==Node::Type::Int && n->i>=-1 && n->i<=UINT32_MAX;
            if(!valid){if(i<14)world_valid=false;rock_valid=false;continue;}
            const auto value=n->i;
            const auto& rule=rules.worlds[i];
            // Native load clamps zero upward to1; unused zero registry boundaries must
            // never classify unloaded/placeholder worlds as completed or moon-rock opened.
            if(value>0 && value<256){
                if(i<14 && (value==rule.clear_main || value==rule.after_ending || value==rule.moon_rock))++peaceful;
                if(rule.moon_rock>0 && value==rule.moon_rock)++rocks;
            }
        }
        if(world_valid)p.achievement_counters["Scenario_WorldAll"]=peaceful;
        if(rock_valid)p.achievement_counters["Other_MoonStoneAll"]=rocks;
        if(info->arr.size()>=20){
            bool valid=true;int count=0;
            for(size_t i=0;i<20;++i){const auto value=TaskBool(info->arr[i].Find("WorldWarpHoleThrough"));if(!value){valid=false;break;}count+=*value;}
            if(valid)p.achievement_counters["Other_WorldWarpHoleAll"]=count;
        }
    }
    const auto* quizzes=root.Find("SphinxQuizData");
    if(quizzes && quizzes->IsArray() && quizzes->arr.size()>=17){
        bool valid=true;int count=0;
        for(size_t i=0;i<17;++i){const auto value=TaskBool(quizzes->arr[i].Find(rules.worlds[i].sphinx_field));if(!value){valid=false;break;}count+=*value;}
        if(valid)p.achievement_counters["MiniGame_SphinxQuiz"]=count;
    }
    std::map<std::string,std::optional<std::set<std::string>>> owned;
    for(const auto& r:rules.items)if(!owned.contains(r.array))owned.emplace(r.array,TaskItems(root,r.array));
    for(const auto& r:rules.shops)if(!owned.contains(r.array))owned.emplace(r.array,TaskItems(root,r.array));
    bool souvenir_valid=true;std::int64_t souvenirs=0;
    for(const auto& r:rules.items){const auto& a=owned.at(r.array);if(!a){souvenir_valid=false;break;}souvenirs+=a->contains(r.item);}
    if(souvenir_valid)for(int i=1;i<=3;++i)p.achievement_counters["Souvenir_Count_"+std::to_string(i)]=souvenirs;
    bool shop_valid=true;std::set<std::string> purchased_worlds;
    for(const auto& r:rules.shops){const auto& a=owned.at(r.array);if(!a){shop_valid=false;break;}if(a->contains(r.item))purchased_worlds.insert(r.world);}
    if(shop_valid)p.achievement_counters["Shine_CollectCoinShop"]=purchased_worlds.size();
}

TaskProgress ForTask(const SaveProgress& save, const Task& task) {
    TaskProgress out;
    if (!save.valid) return out;
    out.claimed = std::find(save.achievements_claimed.begin(), save.achievements_claimed.end(), task.label)
                  != save.achievements_claimed.end();
    std::string_view counter;
    if (task.label.starts_with("Other_Coin_")) counter = "TotalCoinNum";
    else if (task.label == "Other_Jump") counter = "PlayerJumpCount";
    else if (task.label == "Other_CapThrow") counter = "PlayerThrowCapCount";
    else if (task.label.starts_with("Costume_Cap_")) counter = "ItemCap";
    else if (task.label.starts_with("Costume_Clothes_")) counter = "ItemCloth";
    else if (task.label.starts_with("Capture_Count_")) counter = "HackDictionary";
    else if (task.label.starts_with("Other_CheckPoint_")) counter = "Checkpoint";
    if (save.achievement_counters.contains(task.label)) counter = task.label;
    if (!counter.empty()) {
        const auto it = save.achievement_counters.find(std::string{counter});
        if (it != save.achievement_counters.end()) out.current = it->second;
    }
    const auto threshold=task.target ? task.target : task.label=="Scenario_Ending" ? std::optional<std::int64_t>(1) : std::nullopt;
    out.ready = !out.claimed && out.current && threshold && *out.current >= *threshold;
    return out;
}

std::optional<KingdomTables> ParseKingdomTables(std::string_view moons_tsv,
                                                std::string_view coins_tsv,
                                                std::string_view kingdoms_tsv) {
    KingdomTables out;

    const auto moons = SplitTsv(moons_tsv);
    if (moons.empty()) {
        return std::nullopt;
    }
    const int m_id = ColumnIndex(moons[0], "unique_id"), m_world = ColumnIndex(moons[0], "world");
    if (m_id < 0 || m_world < 0) {
        return std::nullopt;
    }
    for (std::size_t r = 1; r < moons.size(); ++r) {
        const auto& row = moons[r];
        if (static_cast<int>(row.size()) <= std::max(m_id, m_world)) {
            continue;
        }
        std::uint32_t id = 0;
        for (const char c : row[static_cast<std::size_t>(m_id)]) {
            if (c < '0' || c > '9') {
                id = 0;
                break;
            }
            id = id * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (id != 0) {
            out.moon_world[id] = std::string{row[static_cast<std::size_t>(m_world)]};
        }
    }

    const auto coins = SplitTsv(coins_tsv);
    if (coins.empty()) {
        return std::nullopt;
    }
    const int c_id = ColumnIndex(coins[0], "unique_id"), c_world = ColumnIndex(coins[0], "world");
    if (c_id < 0 || c_world < 0) {
        return std::nullopt;
    }
    for (std::size_t r = 1; r < coins.size(); ++r) {
        const auto& row = coins[r];
        if (static_cast<int>(row.size()) <= std::max(c_id, c_world)) {
            continue;
        }
        std::uint32_t id = 0;
        for (const char c : row[static_cast<std::size_t>(c_id)]) {
            if (c < '0' || c > '9') {
                id = 0;
                break;
            }
            id = id * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (id != 0) {
            out.coin_world[id] = std::string{row[static_cast<std::size_t>(c_world)]};
        }
    }

    const auto kings = SplitTsv(kingdoms_tsv);
    if (kings.empty()) {
        return std::nullopt;
    }
    const int k_world = ColumnIndex(kings[0], "world"), k_moons = ColumnIndex(kings[0], "moon_total"),
             k_coins = ColumnIndex(kings[0], "coin_capacity");
    if (k_world < 0 || k_moons < 0 || k_coins < 0) {
        return std::nullopt;
    }
    for (std::size_t r = 1; r < kings.size(); ++r) {
        const auto& row = kings[r];
        if (static_cast<int>(row.size()) <= std::max({k_world, k_moons, k_coins})) {
            continue;
        }
        const std::string world{row[static_cast<std::size_t>(k_world)]};
        int moon_total = 0, coin_cap = 0;
        for (const char c : row[static_cast<std::size_t>(k_moons)]) {
            if (c < '0' || c > '9') {
                moon_total = 0;
                break;
            }
            moon_total = moon_total * 10 + (c - '0');
        }
        for (const char c : row[static_cast<std::size_t>(k_coins)]) {
            if (c < '0' || c > '9') {
                coin_cap = 0;
                break;
            }
            coin_cap = coin_cap * 10 + (c - '0');
        }
        const int id_col = ColumnIndex(kings[0], "world_id");
        const int name_col = ColumnIndex(kings[0], "world_name");
        const int shine_col = ColumnIndex(kings[0], "shine_type");
        if (id_col >= 0 && name_col >= 0 && shine_col >= 0 &&
            static_cast<int>(row.size()) > std::max({id_col, name_col, shine_col})) {
            const auto number = [](std::string_view text) {
                int n = 0;
                if (text.empty() || text.size() > 2) return -1;
                for (char c : text) { if (c < '0' || c > '9') return -1; n = n*10+c-'0'; }
                return n;
            };
            const auto id = number(row[id_col]), shine = number(row[shine_col]);
            if (id >= 0 && id < 17 && shine >= 0 && shine <= 10)
                out.kingdoms.push_back({world, std::string{row[name_col]}, id, shine});
        }
        out.moon_total[world] = moon_total;
        out.coin_capacity[world] = coin_cap;
    }
    return out;
}

KingdomProgress ForKingdom(const SaveProgress& progress, const KingdomTables& tables,
                          std::string_view world) {
    KingdomProgress out;
    const std::string key{world};
    const auto mt = tables.moon_total.find(key);
    const auto cc = tables.coin_capacity.find(key);
    if (mt == tables.moon_total.end() && cc == tables.coin_capacity.end()) {
        return out; // this kingdom name is not in the shipped tables at all
    }
    out.known = true;
    out.moon_total = mt != tables.moon_total.end() ? mt->second : 0;
    out.coin_capacity = cc != tables.coin_capacity.end() ? cc->second : 0;
    for (const std::uint32_t id : progress.moon_ids) {
        const auto it = tables.moon_world.find(id);
        if (it != tables.moon_world.end() && it->second == world) {
            ++out.moons;
        }
    }
    if (world == "Peach") out.moons += static_cast<int>(progress.achievements_claimed.size());
    for (const std::uint32_t id : progress.coin_ids) {
        const auto it = tables.coin_world.find(id);
        if (it != tables.coin_world.end() && it->second == world) {
            ++out.coins;
        }
    }
    return out;
}

VisitedMoonTotals SumVisitedKingdomMoons(const SaveProgress& progress, const KingdomTables& tables) {
    VisitedMoonTotals out;
    for (const auto& info : tables.kingdoms) {
        if (std::find(progress.visited_world_ids.begin(), progress.visited_world_ids.end(),
                      info.world_id) == progress.visited_world_ids.end()) {
            continue; // unvisited: excluded from both the numerator and the denominator
        }
        const auto kingdom = ForKingdom(progress, tables, info.world);
        out.moons += kingdom.moons;
        out.moon_total += kingdom.moon_total;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> ParseNamePairsTsv(std::string_view tsv) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto rows = SplitTsv(tsv);
    if (rows.size() < 2) {
        return out;
    }
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() < 2) {
            continue;
        }
        out.emplace_back(std::string{row[0]}, std::string{row[1]});
    }
    return out;
}

std::optional<std::set<std::string>> ParseItemNameColumn(std::string_view tsv) {
    const auto rows = SplitTsv(tsv);
    if (rows.empty()) return std::nullopt;
    const auto column = ColumnIndex(rows[0], "item_name");
    if (column < 0) return std::nullopt;
    std::set<std::string> out;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (static_cast<std::size_t>(column) >= row.size() || row[column].empty()) continue;
        out.emplace(row[column]);
    }
    return out;
}

} // namespace SmoSave
