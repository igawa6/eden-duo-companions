// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Battle page tables for Dragon Quest III HD-2D Remake 1.1.0.0. Sources: dq3_tables.h.

#include "dq3_tables.h"

#include <mutex>
#include <set>
#include <vector>

#include "dq3_battle_art.h"
#include "dq3_cooked.h"
#include "dq3_sprites.h"

namespace dq3 {
namespace {

using namespace cooked;

std::mutex data_mutex;
std::shared_ptr<const BattleData> data_cache;

bool KeepNoun(std::string_view row) {
    return row.starts_with("TEXT_NOUN_ENGLISH_Monster_Name_") ||
           row.starts_with("TEXT_NOUN_ENGLISH_Magic_Name_") ||
           row.starts_with("TEXT_NOUN_ENGLISH_Item_Name_");
}

s32 Int(const TableRow& row, std::string_view col, bool& ok) {
    const auto it = row.find(col);
    if (it == row.end() || it->second.kind != TableValue::Kind::Int) {
        ok = false;
        return 0;
    }
    return static_cast<s32>(it->second.i);
}
float Float(const TableRow& row, std::string_view col, bool& ok) {
    const auto it = row.find(col);
    if (it == row.end() || it->second.kind != TableValue::Kind::Float) {
        ok = false;
        return 0.0f;
    }
    return static_cast<float>(it->second.f);
}
const std::string* Text(const TableRow& row, std::string_view col) {
    const auto it = row.find(col);
    if (it == row.end() || (it->second.kind != TableValue::Kind::Name &&
                            it->second.kind != TableValue::Kind::Str))
        return nullptr;
    return &it->second.s;
}

std::string ResistColumn(std::string_view stem) {
    return std::string{stem} + "_Damage_Multiplier";
}

} // namespace

std::optional<Table> ParseDataTable(std::span<const u8> uasset, std::span<const u8> uexp,
                                    bool (*keep_row)(std::string_view),
                                    std::span<const std::string_view> columns,
                                    std::vector<std::string>* order) {
    const auto pkg = ParseSummary(uasset);
    if (!pkg || pkg->export_offset + pkg->export_size > uexp.size())
        return std::nullopt;
    Cursor c{uexp.first(static_cast<std::size_t>(pkg->export_offset + pkg->export_size)),
             static_cast<std::size_t>(pkg->export_offset)};
    while (const auto t = NextTag(c, *pkg))
        c.Skip(static_cast<std::size_t>(t->size));
    if (!c.ok)
        return std::nullopt;
    c.Skip(4);
    const s32 rows = c.I32();
    if (!c.ok || rows <= 0 || rows > 100000)
        return std::nullopt;
    const auto name_at = [&](std::size_t at, std::string& out) {
        if (at + 8 > c.b.size())
            return false;
        const s32 i = static_cast<s32>(Le32(c.b.data() + at));
        const s32 n = static_cast<s32>(Le32(c.b.data() + at + 4));
        if (i < 0 || static_cast<std::size_t>(i) >= pkg->names.size() || n < 0)
            return false;
        out = pkg->names[static_cast<std::size_t>(i)];
        if (n)
            out += "_" + std::to_string(n - 1);
        return true;
    };
    Table out;
    for (s32 k = 0; k < rows; ++k) {
        std::string row;
        if (!name_at(c.o, row))
            return std::nullopt;
        c.Skip(8);
        const bool wanted = !keep_row || keep_row(row);
        TableRow values;
        while (auto t = NextTag(c, *pkg)) {
            const std::size_t end = c.o + static_cast<std::size_t>(t->size);
            if (t->number > 0) // UE FName suffix ("Name_<number - 1>")
                t->name += "_" + std::to_string(t->number - 1);
            bool want_col = wanted;
            if (want_col && !columns.empty()) {
                want_col = false;
                for (const auto col : columns)
                    want_col |= col == t->name;
            }
            if (want_col) {
                TableValue v;
                if (t->type == "IntProperty" && t->size == 4) {
                    v.kind = TableValue::Kind::Int;
                    v.i = static_cast<s32>(Le32(c.b.data() + c.o));
                } else if (t->type == "FloatProperty" && t->size == 4) {
                    v.kind = TableValue::Kind::Float;
                    float f;
                    const u32 bits = Le32(c.b.data() + c.o);
                    std::memcpy(&f, &bits, 4);
                    v.f = f;
                } else if ((t->type == "NameProperty" || t->type == "EnumProperty" ||
                            (t->type == "ByteProperty" && t->inner != "None")) &&
                           t->size == 8) {
                    v.kind = TableValue::Kind::Name;
                    if (!name_at(c.o, v.s))
                        return std::nullopt;
                } else if (t->type == "StrProperty") {
                    const auto s = c.FString();
                    if (!s || c.o != end)
                        return std::nullopt;
                    v.kind = TableValue::Kind::Str;
                    for (const char32_t ch : *s)
                        v.s += static_cast<char>(ch < 0x80 ? ch : '?');
                    v.u = *s;
                } else if (t->type == "StructProperty" && t->inner == "Vector2D" && t->size == 8) {
                    v.kind = TableValue::Kind::Vec2;
                    float a, b;
                    const u32 ba = Le32(c.b.data() + c.o), bb = Le32(c.b.data() + c.o + 4);
                    std::memcpy(&a, &ba, 4);
                    std::memcpy(&b, &bb, 4);
                    v.f = a;
                    v.f2 = b;
                } else if (t->type == "SoftObjectProperty" && t->size >= 12) {
                    // FSoftObjectPath: FName AssetPathName + FString SubPathString
                    v.kind = TableValue::Kind::Name;
                    if (!name_at(c.o, v.s))
                        return std::nullopt;
                    Cursor sub{c.b.first(end), c.o + 8};
                    const auto rest = sub.FString();
                    if (!rest || sub.o != end)
                        return std::nullopt;
                    if (!rest->empty())
                        v.s += ":" + Ascii(*rest);
                } else if (t->type == "BoolProperty") {
                    v.kind = TableValue::Kind::Bool;
                    v.i = t->bool_value;
                }
                if (v.kind != TableValue::Kind::None)
                    values.emplace(t->name, std::move(v));
            }
            c.o = end;
        }
        if (!c.ok)
            return std::nullopt;
        if (wanted) {
            if (order)
                order->push_back(row);
            out.emplace(std::move(row), std::move(values));
        }
    }
    return out;
}

std::optional<BattleData> BuildBattleData(std::span<const std::span<const u8>, 14> m, std::string* why) {
    const auto fail = [why](std::string reason) -> std::optional<BattleData> {
        if (why) *why = std::move(reason);
        return std::nullopt;
    };
    static constexpr std::string_view MonsterCols[] = {"Level",  "Max_HP",  "Max_MP", "Attack",
                                                       "Defense", "Speed", "Battle_Resist", "Monster_ActionList"};
    static constexpr std::string_view MagicCols[] = {"ListNo", "ConsumeMP", "SkillType", "UIIconNo",
                                                     "NameTextId"};
    std::vector<std::string> resist_cols_s;
    for (const auto stem : ResistStems)
        resist_cols_s.push_back(ResistColumn(stem));
    std::vector<std::string_view> resist_cols(resist_cols_s.begin(), resist_cols_s.end());
    const auto monsters = ParseDataTable(m[0], m[1], nullptr, MonsterCols);
    const auto resists = ParseDataTable(m[2], m[3], nullptr, resist_cols);
    const auto magic = ParseDataTable(m[4], m[5], nullptr, MagicCols);
    if (!monsters || !resists || !magic)
        return std::nullopt;
    BattleData d;
    for (const auto& [id, row] : *monsters) {
        bool ok = true;
        MonsterInfo mi{Int(row, "Level", ok),   Int(row, "Max_HP", ok),  Int(row, "Max_MP", ok),
                       Int(row, "Attack", ok),  Int(row, "Defense", ok), Int(row, "Speed", ok), {}, {}};
        const auto* r = Text(row, "Battle_Resist");
        const auto* group = Text(row, "Monster_ActionList");
        if (!ok || !r || !group)
            return fail("monster columns: " + id);
        mi.resist = *r;
        mi.action_group = *group;
        d.monsters.emplace(id, std::move(mi));
    }
    for (const auto& [id, row] : *resists) {
        bool ok = true;
        ResistInfo ri;
        for (int k = 0; k < ResistElements; ++k)
            ri.mult[static_cast<std::size_t>(k)] = Float(row, resist_cols[static_cast<std::size_t>(k)], ok);
        if (!ok)
            return std::nullopt;
        d.resists.emplace(id, ri);
    }
    for (const auto& [id, row] : *magic) {
        bool ok = true;
        MagicInfo mi{Int(row, "ListNo", ok), Int(row, "ConsumeMP", ok), Int(row, "UIIconNo", ok), false, false, {}};
        const auto* type = Text(row, "SkillType");
        const auto* name = Text(row, "NameTextId");
        if (!ok || !type || !name)
            return std::nullopt;
        mi.magic = *type == "ESkillType::MAGIC";
        mi.attack = *type == "ESkillType::ATTACK";
        mi.name_text = *name;
        d.magic.emplace(id, std::move(mi));
    }
    // The AI table contains several conditional rows for each action group. Union
    // their configured spells, rather than implying a particular next action.
    static constexpr std::string_view ActionCols[] = {"GOP_Magic"};
    static constexpr std::string_view ListCols[] = {"Monster_ActionList_ID", "Action1_ID", "Action2_ID",
        "Action3_ID", "Action4_ID", "Action5_ID", "Action6_ID", "Action7_ID", "Action8_ID"};
    const auto actions = ParseDataTable(m[10], m[11], nullptr, ActionCols);
    const auto lists = ParseDataTable(m[12], m[13], nullptr, ListCols);
    if (!actions || actions->empty()) return fail("monster action table parse");
    for (const auto& [id, row] : *actions)
        if (const auto* magic_id = Text(row, "GOP_Magic"); magic_id && d.magic.contains(*magic_id))
            d.monster_actions.emplace(id, *magic_id);
    if (!lists || lists->empty()) return fail("monster action-list table parse");
    std::map<std::string, std::set<std::string>, std::less<>> spells;
    for (const auto& [id, row] : *lists) {
        const auto* group = Text(row, ListCols[0]);
        if (!group) return fail("action group: " + id);
        auto& names = spells[*group];
        for (std::size_t i = 1; i < std::size(ListCols); ++i) {
            const auto* action = Text(row, ListCols[i]);
            if (!action) return fail("action column: " + id + "/" + std::string{ListCols[i]});
            if (*action == "None") continue;
            const std::string* magic_id = action;
            if (const auto it = actions->find(*action); it != actions->end())
                magic_id = Text(it->second, "GOP_Magic");
            // Debug groups use direct GOP_Magic rows; normal groups use Action rows.
            if (!magic_id) return fail("action magic: " + *action);
            const auto it = d.magic.find(*magic_id);
            if (it == d.magic.end()) return fail("unknown action/magic: " + *magic_id);
            if (it->second.magic) names.insert(it->second.name_text);
        }
    }
    for (const auto& [group, names] : spells) {
        if (names.size() > MaxEnemySpells) return fail("spell count exceeds layout: " + group);
        d.enemy_spells.emplace(group, std::vector<std::string>(names.begin(), names.end()));
    }
    static constexpr std::string_view ItemCols[] = {"NameTextId"};
    const auto items = ParseDataTable(m[8], m[9], nullptr, ItemCols);
    if (!items)
        return std::nullopt;
    for (const auto& [id, row] : *items)
        if (const auto* name = Text(row, "NameTextId"))
            d.items.emplace(id, *name);
    // Nouns: ListNoun of the monster, spell and item names.
    static constexpr std::string_view NounCols[] = {"ListNoun"};
    const auto nouns = ParseDataTable(m[6], m[7], &KeepNoun, NounCols);
    if (!nouns)
        return std::nullopt;
    constexpr std::string_view Prefix = "TEXT_NOUN_ENGLISH_";
    for (const auto& [id, row] : *nouns) {
        const auto it = row.find("ListNoun");
        if (it == row.end() || it->second.kind != TableValue::Kind::Str)
            continue;
        d.nouns.emplace("Txt_" + id.substr(Prefix.size()), it->second.u);
    }
    // Sanity: the first spell of each family has a noun, and the table rows exist.
    for (const auto t : ResistSpellText)
        if (!d.Noun(t))
            return std::nullopt;
    for (std::size_t k = 0; k < ResistElements; ++k) {
        const auto it = d.magic.find(ResistSpellRow[k]);
        if (it == d.magic.end() || it->second.name_text != ResistSpellText[k] || it->second.icon < 0 || it->second.icon > 7)
            return fail("affinity spell row: " + std::string{ResistSpellRow[k]});
    }
    if (d.monsters.size() < 100 || d.resists.size() < 100 || d.magic.size() < 100 || d.nouns.size() < 100)
        return std::nullopt;
    return d;
}

std::shared_ptr<const BattleData> LoadBattleData(const RangeReader& read, std::string* why) {
    {
        std::scoped_lock lock{data_mutex};
        if (data_cache)
            return data_cache;
    }
    const PakMember* pins[14] = {&BattleTableMember(BattleTable::MonsterAsset),
                                &BattleTableMember(BattleTable::MonsterExp),
                                &BattleTableMember(BattleTable::ResistAsset),
                                &BattleTableMember(BattleTable::ResistExp),
                                &BattleTableMember(BattleTable::MagicAsset),
                                &BattleTableMember(BattleTable::MagicExp),
                                &NounAssetMember(),
                                &NounExpMember(),
                                &BattleTableMember(BattleTable::ItemAsset),
                                &BattleTableMember(BattleTable::ItemExp),
                                &BattleTableMember(BattleTable::ActionAsset),
                                &BattleTableMember(BattleTable::ActionExp),
                                &BattleTableMember(BattleTable::ActionListAsset),
                                &BattleTableMember(BattleTable::ActionListExp)};
    const auto bytes = ReadMembers(read, pins, why);
    if (!bytes)
        return nullptr;
    std::array<std::span<const u8>, 14> spans;
    for (std::size_t i = 0; i < spans.size(); ++i)
        spans[i] = (*bytes)[i];
    std::string reason;
    auto built = BuildBattleData(spans, &reason);
    if (!built) {
        if (why)
            *why = reason.empty() ? "battle table parse" : reason;
        return nullptr;
    }
    auto shared = std::make_shared<const BattleData>(std::move(*built));
    std::scoped_lock lock{data_mutex};
    if (!data_cache)
        data_cache = std::move(shared);
    return data_cache;
}

std::shared_ptr<const BattleData> CachedBattleData() {
    std::scoped_lock lock{data_mutex};
    return data_cache;
}

} // namespace dq3
