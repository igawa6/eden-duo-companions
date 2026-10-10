// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Party and Journal tabs: game data, native rules and live readers (sources: dq3_info.h).

#include "dq3_info.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

#include "dq3_art.h"
#include "dq3_battle_art.h"
#include "dq3_cooked.h"
#include "dq3_heal.h"
#include "dq3_layout.h"
#include "dq3_page_layout.h"
#include "dq3_sprites.h"
#include "dq3_tables.h"

namespace dq3 {
namespace {

#include "dq3_info_pins.inc"

static_assert(std::size(InfoUiPinsTable) == static_cast<std::size_t>(InfoUi::Count));
static_assert(std::size(InfoTablePinsTable) == 28);
static_assert(std::size(ProgressEnumNames) == 174);

using s64 = std::int64_t;
using detail::Get;

enum Table : std::size_t {
    TPersonality, TLevelUpExp, TMedal, TLibrary, TInformation, TMapGuide, TMapGuideData, TUnitMaster, TUnitCommon,
    TTextItem, TTextMagic, TTextMonster, TTextInformation, TTextFieldMenu, TableCount
};

std::mutex data_mutex;
std::shared_ptr<const InfoData> data_cache;

const std::string* TextOf(const TableRow& row, std::string_view col) {
    const auto it = row.find(col);
    if (it == row.end() || (it->second.kind != TableValue::Kind::Name && it->second.kind != TableValue::Kind::Str))
        return nullptr;
    return &it->second.s;
}
std::optional<s64> IntOf(const TableRow& row, std::string_view col) {
    const auto it = row.find(col);
    if (it == row.end() || (it->second.kind != TableValue::Kind::Int && it->second.kind != TableValue::Kind::Bool))
        return std::nullopt;
    return it->second.i;
}

std::optional<u8> EnumIndex(std::string_view value, std::string_view prefix,
                            std::span<const std::string_view> names) {
    if (!value.starts_with(prefix))
        return std::nullopt;
    value.remove_prefix(prefix.size());
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == value)
            return static_cast<u8>(i);
    return std::nullopt;
}
// EEquipParamType / EParamUpType enumerator order (static FEnumeratorParams at main+0x4f82180 and
// main+0x4f82100, values = index).
constexpr std::string_view ParamTypes[] = {"NONE", "OFFENSE", "DEFENSE", "PHYSICAL", "INTELLIGENCE",
                                           "AGILITY", "LUC", "MAXHP", "MAXMP"};
constexpr std::string_view ParamUpTypes[] = {"NONE", "ADD", "RATIO", "SPECIAL"};
constexpr std::string_view JobColumns[10] = {"Hero", "Warrior", "Fighter", "Mage", "Priest",
                                              "Merchant", "Gadabout", "Thief", "Sage", "Breeder"};

} // namespace

const MapTexturePin& InfoUiPin(InfoUi id) {
    return InfoUiPinsTable[static_cast<std::size_t>(id)];
}

std::optional<u16> ProgressValue(std::string_view token) {
    for (std::size_t i = 0; i < std::size(ProgressEnumNames); ++i) {
        std::string_view n = ProgressEnumNames[i];
        constexpr std::string_view Prefix = "EGOPEnumProgressType::";
        if (n.substr(Prefix.size()) == token)
            return static_cast<u16>(i);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------- data
bool ParseLevelRowId(std::string_view id, int& level) {
    constexpr std::string_view Prefix = "LEVELUPEXP_";
    return id.starts_with(Prefix) && art_util::ParseInt(id.substr(Prefix.size()), level);
}

namespace {

std::optional<InfoData> BuildInfoData(std::span<const std::vector<u8>> m, std::span<const std::vector<u8>> battle,
                                      std::string* why) {
    const auto fail = [why](std::string r) -> std::optional<InfoData> {
        if (why)
            *why = std::move(r);
        return std::nullopt;
    };
    std::vector<std::string> medal_order, lib_order, info_order;
    const auto table = [&](std::size_t t, std::span<const std::string_view> cols = {},
                           bool (*keep)(std::string_view) = nullptr, std::vector<std::string>* order = nullptr) {
        return ParseDataTable(m[2 * t], m[2 * t + 1], keep, cols, order);
    };
    InfoData d;
    static constexpr std::string_view PersCols[] = {"DisplayNameId"};
    const auto pers = table(TPersonality, PersCols);
    if (!pers)
        return fail("GOP_Personality");
    for (const auto& [id, row] : *pers)
        if (const auto* t = TextOf(row, "DisplayNameId"))
            d.personality.emplace(id, *t);
    const auto exp = table(TLevelUpExp);
    if (!exp)
        return fail("GOP_LevelUpExp");
    for (const auto& [id, row] : *exp) {
        int level = 0;
        if (!ParseLevelRowId(id, level))
            return fail("level row " + id);
        std::array<s32, 10> v{};
        for (std::size_t k = 0; k < 10; ++k) {
            const auto x = IntOf(row, JobColumns[k]);
            if (!x)
                return fail("level column " + id);
            v[k] = static_cast<s32>(*x);
        }
        d.level_exp.emplace(level, v);
    }
    static constexpr std::string_view MedalCols[] = {"RequireMedal", "Product_0"};
    // row order matters (first minimum wins): the parser's map is sorted, `order` keeps the cooked order
    const auto medal = table(TMedal, MedalCols, nullptr, &medal_order);
    if (!medal)
        return fail("GOP_Medal");
    static constexpr std::string_view LibCols[] = {"GopIdUnitMaster", "GopIdMonster", "TypeSortNo", "GopIdHabitat1",
                                                   "GopIdHabitat2", "IsMoreHabitat", "GopIdTrivia"};
    const auto lib = table(TLibrary, LibCols, nullptr, &lib_order);
    if (!lib)
        return fail("GOP_Monster_Library");
    static constexpr std::string_view InfoCols[] = {"SortOrder", "InformationTitleId", "DispFlagName"};
    const auto info = table(TInformation, InfoCols, nullptr, &info_order);
    if (!info)
        return fail("GOP_Information");
    static constexpr std::string_view MasterCols[] = {"GopIdUnitCommon"};
    static constexpr std::string_view CommonCols[] = {"CharacterNameGopIdText"};
    const auto masters = table(TUnitMaster, MasterCols);
    const auto commons = table(TUnitCommon, CommonCols);
    if (!masters || !commons)
        return fail("GOP_Unit_Master / Common");
    const auto guides = table(TMapGuide);
    const auto guide_data = table(TMapGuideData);
    if (!guides || !guide_data)
        return fail("GOP_MapGuide");
    static constexpr std::string_view TextCols[] = {"Text"};
    for (const std::size_t t : {TTextItem, TTextMagic, TTextMonster, TTextInformation, TTextFieldMenu}) {
        // flavour texts, tip titles and the native field-menu labels the Party / Bag tabs reuse
        // (Equipment slot names, stat names, bag names, "Number Held", "Empty", "No Spells Learnt"),
        // and the Misc menu's Heal All / Handy Heal All result lines (dq3_heal.h HealMessageId)
        const auto texts = table(t, TextCols, [](std::string_view r) {
            return r.find("_Flavor_") != std::string_view::npos || r.starts_with("Txt_Information_TITLE_") ||
                   r.starts_with("Txt_FieldMenu_Equip_BLANK_") || r.starts_with("Txt_FieldMenu_STATUS_PARAMNAME_") ||
                   r.starts_with("Txt_FieldMenu_Item_") || r.starts_with("Txt_FieldMenu_Top_") ||
                   r == "Txt_FieldMenu_Magic_NONE" || IsHealMessageId(r);
        });
        if (!texts)
            return fail("text table " + std::to_string(t));
        for (const auto& [id, row] : *texts) {
            const auto it = row.find("Text");
            if (it != row.end() && it->second.kind == TableValue::Kind::Str)
                d.texts.emplace(id, it->second.u);
        }
    }
    for (const auto& id : medal_order) {
        const auto& row = medal->at(id);
        const auto req = IntOf(row, "RequireMedal");
        const auto* p = TextOf(row, "Product_0");
        if (!req || !p)
            return fail("medal " + id);
        d.medals.push_back({id, *p, static_cast<s32>(*req)});
    }
    for (const auto& id : lib_order) {
        const auto& row = lib->at(id);
        LibraryRow r;
        r.id = id;
        const auto* um = TextOf(row, "GopIdUnitMaster");
        const auto* mo = TextOf(row, "GopIdMonster");
        const auto* h1 = TextOf(row, "GopIdHabitat1");
        const auto* h2 = TextOf(row, "GopIdHabitat2");
        const auto* tr = TextOf(row, "GopIdTrivia");
        const auto so = IntOf(row, "TypeSortNo");
        if (!um || !mo || !h1 || !h2 || !tr || !so)
            return fail("library " + id);
        r.unit_master = *um;
        r.monster = *mo;
        r.habitat[0] = *h1;
        r.habitat[1] = *h2;
        r.trivia = *tr;
        r.sort = static_cast<int>(*so);
        r.more = IntOf(row, "IsMoreHabitat").value_or(0) != 0;
        // name: GOP_Unit_Master.GopIdUnitCommon -> GOP_Unit_Common.CharacterNameGopIdText
        const auto um_row = masters->find(r.unit_master);
        const auto* common = um_row == masters->end() ? nullptr : TextOf(um_row->second, "GopIdUnitCommon");
        const auto cm_row = common ? commons->find(*common) : commons->end();
        const auto* name = cm_row == commons->end() ? nullptr : TextOf(cm_row->second, "CharacterNameGopIdText");
        if (!name)
            return fail("library name " + id);
        r.name_text = *name;
        d.library.push_back(std::move(r));
    }
    // Numerical Order: TypeSortNo ascending (stable for equal numbers)
    std::stable_sort(d.library.begin(), d.library.end(),
                     [](const LibraryRow& a, const LibraryRow& b) { return a.sort < b.sort; });
    for (const auto& id : info_order) {
        const auto& row = info->at(id);
        const auto so = IntOf(row, "SortOrder");
        const auto* title = TextOf(row, "InformationTitleId");
        const auto* flag = TextOf(row, "DispFlagName");
        if (!so || !title || !flag)
            return fail("information " + id);
        constexpr std::string_view Prefix = "EGOPEnumProgressType::";
        if (!std::string_view{*flag}.starts_with(Prefix))
            return fail("information flag " + id);
        const auto v = ProgressValue(std::string_view{*flag}.substr(Prefix.size()));
        if (!v)
            return fail("information flag value " + id);
        d.info.push_back({id, *title, static_cast<int>(*so), *v});
    }
    for (const auto& [id, row] : *guides) {
        GuideRow g;
        const auto* main = TextOf(row, "TargetMain");
        if (!main)
            return fail("guide " + id);
        g.main = *main;
        for (int k = 0; k < 20; ++k) {
            char col[16];
            std::snprintf(col, sizeof(col), "TargetSub%02d", k);
            const auto* s = TextOf(row, col);
            if (!s)
                return fail("guide sub " + id);
            g.subs.push_back(*s); // None entries are kept (the game skips them by message)
        }
        d.guides.emplace(id, std::move(g));
    }
    for (const auto& [id, row] : *guide_data) {
        const auto* disp = TextOf(row, "DispTriggerEventID");
        const auto* done = TextOf(row, "CompleteTriggerEventID");
        const auto* msg = TextOf(row, "MessageTextID");
        if (!disp || !done || !msg)
            return fail("guide data " + id);
        d.guide_data.emplace(id, GuideDataRow{*disp, *done, *msg});
    }
    // battle pins: GOP_Item, GOP_Magic, GOP_Monster and the noun table
    static constexpr std::string_view ItemCols[] = {"NameTextId", "FlavorTextId", "UIIconNo", "ParamType",
                                                    "ParamUpType", "ParamValue", "ParamFighterDown"};
    const auto items = ParseDataTable(battle[0], battle[1], nullptr, ItemCols);
    static constexpr std::string_view MagicCols[] = {"FlavorTextId"};
    const auto magic = ParseDataTable(battle[2], battle[3], nullptr, MagicCols);
    static constexpr std::string_view MonCols[] = {"EXP", "Gold", "Drop_Item", "Is_Boss"};
    const auto monsters = ParseDataTable(battle[4], battle[5], nullptr, MonCols);
    if (!items || !magic || !monsters)
        return fail("item / magic / monster tables");
    for (const auto& [id, row] : *items) {
        ItemRow r;
        const auto* n = TextOf(row, "NameTextId");
        const auto* f = TextOf(row, "FlavorTextId");
        const auto icon = IntOf(row, "UIIconNo");
        const auto* pt = TextOf(row, "ParamType");
        const auto* pu = TextOf(row, "ParamUpType");
        const auto pv = IntOf(row, "ParamValue");
        const auto fd = IntOf(row, "ParamFighterDown");
        if (!n || !f || !icon || !pt || !pu || !pv || !fd)
            return fail("item " + id);
        const auto t = EnumIndex(*pt, "EEquipParamType::", ParamTypes);
        const auto u = EnumIndex(*pu, "EParamUpType::", ParamUpTypes);
        if (!t || !u)
            return fail("item param " + id);
        r.name_text = *n;
        r.flavor_text = *f;
        r.icon = static_cast<int>(*icon);
        r.param_type = *t;
        r.param_up = *u;
        r.param_value = static_cast<s32>(*pv);
        r.fighter_down = static_cast<s32>(*fd);
        d.items.emplace(id, std::move(r));
    }
    for (const auto& [id, row] : *magic)
        if (const auto* f = TextOf(row, "FlavorTextId"))
            d.magic_flavor.emplace(id, *f);
    for (const auto& [id, row] : *monsters) {
        const auto e = IntOf(row, "EXP");
        const auto g = IntOf(row, "Gold");
        const auto* drop = TextOf(row, "Drop_Item");
        if (!e || !g || !drop)
            return fail("monster " + id);
        d.monsters.emplace(id, MonsterRecordRow{static_cast<s32>(*e), static_cast<s32>(*g), *drop,
                                                 IntOf(row, "Is_Boss").value_or(0) != 0});
    }
    static constexpr std::string_view NounCols[] = {"ListNoun"};
    const auto nouns = ParseDataTable(battle[6], battle[7], [](std::string_view r) {
        return r.starts_with("TEXT_NOUN_ENGLISH_Status_Personality_") || r.starts_with("TEXT_NOUN_ENGLISH_Item_Name_") ||
               r.starts_with("TEXT_NOUN_ENGLISH_Magic_Name_") || r.starts_with("TEXT_NOUN_ENGLISH_Monster_Name_") ||
               r.starts_with("TEXT_NOUN_ENGLISH_Map_");
    }, NounCols);
    if (!nouns)
        return fail("noun table");
    constexpr std::string_view NounPrefix = "TEXT_NOUN_ENGLISH_";
    for (const auto& [id, row] : *nouns) {
        const auto it = row.find("ListNoun");
        if (it != row.end() && it->second.kind == TableValue::Kind::Str)
            d.nouns.emplace("Txt_" + id.substr(NounPrefix.size()), it->second.u);
    }
    if (d.personality.size() < 40 || d.level_exp.size() < 90 || d.medals.size() < 20 || d.library.size() < 150 ||
        d.info.size() < 60 || d.guides.size() < 20 || d.guide_data.size() < 50 || d.items.size() < 300 ||
        d.monsters.size() < 400 || !d.Noun("Txt_Status_Personality_PARAGON"))
        return fail("sanity");
    return d;
}

} // namespace

std::shared_ptr<const InfoData> LoadInfoData(const RangeReader& read, std::string* why) {
    {
        std::scoped_lock lock{data_mutex};
        if (data_cache)
            return data_cache;
    }
    std::vector<const PakMember*> info_pins;
    for (const PakMember& p : InfoTablePinsTable)
        info_pins.push_back(&p);
    const PakMember* pins[] = {&BattleTableMember(BattleTable::ItemAsset),    &BattleTableMember(BattleTable::ItemExp),
                               &BattleTableMember(BattleTable::MagicAsset),   &BattleTableMember(BattleTable::MagicExp),
                               &BattleTableMember(BattleTable::MonsterAsset), &BattleTableMember(BattleTable::MonsterExp),
                               &NounAssetMember(),                            &NounExpMember()};
    const auto bytes = ReadMembers(read, info_pins, why);
    const auto battle = bytes ? ReadMembers(read, pins, why) : std::nullopt;
    if (!battle)
        return nullptr;
    std::string reason;
    auto built = BuildInfoData(*bytes, *battle, &reason);
    if (!built) {
        if (why)
            *why = "info tables: " + reason;
        return nullptr;
    }
    auto shared = std::make_shared<const InfoData>(std::move(*built));
    std::scoped_lock lock{data_mutex};
    if (!data_cache)
        data_cache = std::move(shared);
    return data_cache;
}

std::shared_ptr<const InfoData> CachedInfoData() {
    std::scoped_lock lock{data_mutex};
    return data_cache;
}

// ---------------------------------------------------------------------------------- rules
bool EvalCondition(std::string_view condition, const Progress& p) {
    std::string s;
    for (const char c : condition)
        if (c != ' ')
            s += c;
    const auto token_value = [&](std::string_view t) -> std::optional<u16> {
        const auto v = ProgressValue(t);
        if (!v || *v == 0)
            return std::nullopt;
        return v;
    };
    const auto split = [](std::string_view v, char sep) {
        std::vector<std::string_view> out;
        while (true) {
            const auto at = v.find(sep);
            out.push_back(v.substr(0, at));
            if (at == std::string_view::npos)
                return out;
            v.remove_prefix(at + 1);
        }
    };
    if (s.find('&') != std::string::npos) {
        for (const auto t : split(s, '&')) {
            const auto v = token_value(t);
            if (!v || !p.Test(*v))
                return false;
        }
        return true;
    }
    if (s.find('|') != std::string::npos) {
        for (const auto t : split(s, '|')) {
            const auto v = token_value(t);
            if (!v)
                return false;
            if (p.Test(*v))
                return true;
        }
        return false;
    }
    const auto v = token_value(s);
    return v && p.Test(*v);
}

std::optional<s32> LevelExp(const InfoData& d, int level, int vocation) {
    if (vocation < 1 || vocation > 10)
        return std::nullopt;
    const auto it = d.level_exp.find(level);
    if (it == d.level_exp.end())
        return std::nullopt;
    return it->second[static_cast<std::size_t>(vocation - 1)];
}

std::optional<s32> ExpToNext(const InfoData& d, int level, int vocation, s32 exp) {
    if (level > 98)
        return 0;
    const auto next = LevelExp(d, level + 1, vocation);
    const s64 total = next ? *next : std::numeric_limits<s32>::max();
    return static_cast<s32>(total - exp);
}

namespace {

const ItemRow* Row(const InfoData& d, const std::string& id) {
    const auto it = d.items.find(id);
    return it == d.items.end() ? nullptr : &it->second;
}
/// The ADD / RATIO helpers (0x87ef90 shape): the row value (RATIO: minus 100) plus the first matching
/// item-instance extra entry.
s32 ParamHelper(const InfoData& d, const OwnedItem& it, u8 type, u8 up) {
    s32 v = 0;
    if (const auto* r = Row(d, it.id); r && r->param_type == type && r->param_up == up)
        v = up == 2 ? r->param_value - 100 : r->param_value;
    for (const auto& e : it.extras)
        if (e[1] == type && e[2] == up) {
            s32 x;
            std::memcpy(&x, e.data() + 4, 4);
            return v + x;
        }
    return v;
}
s32 Clamp999(s64 v) {
    return static_cast<s32>(std::clamp<s64>(v, 0, 999));
}
s32 Floor(double base) {
    return static_cast<s32>(std::floor(static_cast<float>(base)));
}
/// Agility / Stamina / Wisdom / Luck (0x89ee40 shape).
s32 RatioStat(const InfoData& d, const StatInputs& in, std::size_t k, u8 type) {
    s64 add = 0, ratio = 0;
    for (const auto& it : in.items)
        if (it.equip) {
            add += ParamHelper(d, it, type, 1);
            ratio += ParamHelper(d, it, type, 2);
        }
    const s32 base = Floor(in.base[k]);
    // 32-bit multiply (mul w8, w8, w24), then the signed division by 100 (smull / asr / sign fix)
    const s32 mul = static_cast<s32>(static_cast<u32>(static_cast<s64>(base + in.bonus[k]) * ratio));
    const s32 part = mul / 100;
    return Clamp999(s64{base} + add + part + in.bonus[k]);
}

} // namespace

s32 EquipMaxParam(const InfoData& d, const std::vector<OwnedItem>& items, u8 type) {
    s64 sum = 0;
    for (const auto& it : items) {
        if (!it.equip)
            continue;
        if (const auto* r = Row(d, it.id); r && r->param_type == type && r->param_up == 1)
            sum += r->param_value;
    }
    return static_cast<s32>(std::clamp<s64>(sum, -100000, 100000));
}

bool HasMaxHpMpItems(const InfoData& d) {
    return std::any_of(d.items.begin(), d.items.end(), [](const auto& kv) {
        return (kv.second.param_type == 7 || kv.second.param_type == 8) && kv.second.param_up == 1;
    });
}

s32 ItemParam(const InfoData& d, const OwnedItem& it, u8 type, u8 up) {
    return ParamHelper(d, it, type, up);
}

Stats ComputeStats(const InfoData& d, const StatInputs& in) {
    Stats s;
    // GetStr / GetRes full path: floor(base) + 0 (no STR / RES params) + bonus
    s.strength = Clamp999(s64{Floor(in.base[0])} + in.bonus[0]);
    s.resilience = Clamp999(s64{Floor(in.base[1])} + in.bonus[1]);
    s.agility = RatioStat(d, in, 2, 5);
    s.stamina = RatioStat(d, in, 3, 3);
    s.wisdom = RatioStat(d, in, 4, 4);
    s.luck = RatioStat(d, in, 5, 6);
    s.luck_ok = !in.luck_override;
    s64 atk = 0, def = 0;
    for (const auto& it : in.items) {
        if (!it.equip)
            continue;
        const auto* r = Row(d, it.id);
        if (!r)
            continue;
        if (in.vocation == 3 && r->fighter_down != 0)
            atk += r->fighter_down;
        else if (r->param_type == 1 && r->param_up == 1)
            atk += r->param_value;
        if (r->param_type == 2 && r->param_up == 1)
            def += r->param_value;
    }
    s.attack = Clamp999(s64{s.strength} + atk);
    s.defence = Clamp999(s64{s.resilience} + def);
    s.max_hp = DisplayedMax(AddClamped(in.hp_base, EquipMaxParam(d, in.items, 7)), in.hp_bonus);
    s.max_mp = DisplayedMax(AddClamped(in.mp_base, EquipMaxParam(d, in.items, 8)), in.mp_bonus);
    return s;
}

std::vector<TipEntry> VisibleTips(const InfoData& d, const Progress& p, std::span<const u8> read) {
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < d.info.size(); ++i)
        if (d.info[i].flag == 0 || p.Test(d.info[i].flag))
            rows.push_back(i);
    // selection by minimum SortOrder, the first minimum winning ties
    std::vector<TipEntry> out;
    while (!rows.empty()) {
        std::size_t best = 0;
        for (std::size_t k = 1; k < rows.size(); ++k)
            if (d.info[rows[k]].sort < d.info[rows[best]].sort)
                best = k;
        const std::size_t r = rows[best];
        out.push_back({r, !(r < read.size() && read[r] != 0)});
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(best));
    }
    return out;
}

const MedalRow* NextMedal(const InfoData& d, std::span<const std::string> received) {
    const MedalRow* best = nullptr;
    for (const auto& m : d.medals) {
        if (std::find(received.begin(), received.end(), m.id) != received.end())
            continue;
        if (!best || m.require < best->require)
            best = &m;
    }
    return best;
}

std::vector<std::size_t> VisibleMonsters(const InfoData& d, const std::map<std::string, s32>& record) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < d.library.size(); ++i) {
        const auto& r = d.library[i];
        if (record.contains(r.unit_master)) {
            out.push_back(i);
            continue;
        }
        const auto m = d.monsters.find(r.monster);
        if (m != d.monsters.end() && !m->second.boss)
            out.push_back(i);
    }
    return out;
}

std::u32string CleanGameText(std::u32string_view s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == U'<') {
            const auto end = s.find(U'>', i);
            if (end == std::u32string_view::npos)
                break;
            const auto tag = s.substr(i, end - i + 1);
            if (tag == U"<--->")
                out += U'—';
            i = end;
            continue;
        }
        if (s[i] == U'\r')
            continue;
        out += s[i] == U'\n' ? U' ' : s[i];
    }
    return out;
}

std::optional<Objective> ObjectiveFor(const InfoData& d, const MapData& map, std::string_view guide,
                                      const Progress& p) {
    const auto g = d.guides.find(guide);
    if (g == d.guides.end())
        return std::nullopt;
    const auto text = [&](const std::string& id) -> const std::u32string* {
        const auto it = map.menu.find(id);
        return it == map.menu.end() || it->second.empty() ? nullptr : &it->second;
    };
    const auto main = d.guide_data.find(g->second.main);
    if (main == d.guide_data.end())
        return std::nullopt;
    Objective o;
    if (const auto* t = text(main->second.message))
        o.main = CleanGameText(*t);
    for (const auto& sub : g->second.subs) {
        const auto it = d.guide_data.find(sub);
        if (it == d.guide_data.end() || it->second.message.empty() || it->second.message == "None")
            continue;
        if (!EvalCondition(it->second.disp, p) || EvalCondition(it->second.complete, p))
            continue;
        if (const auto* t = text(it->second.message))
            o.subs.push_back(CleanGameText(*t));
    }
    if (o.main.empty() && o.subs.empty())
        return std::nullopt;
    return o;
}

// ---------------------------------------------------------------------------------- live state
namespace {

// Words copied from the 1.1.0.0 main image (research runs/party-journal-tabs/re listings).
const std::vector<CodePin>& InfoPins() {
    static const std::vector<CodePin> pins{
        {0x8d0294, {0xf84ac000, 0xd65f03c0}}, // personality: ldur x0, [x0, #0xac]; ret
        // exp to next: level > 98 -> 0; threshold(F, C+0xAA) - F+0x20
        {0x8d02d0, {0xb9401e88, 0x7101891f, 0x5400018c, 0x3942a801, 0xaa1403e0, 0x97ff5082, 0xb9402288, 0x4b080014}},
        {0x8a44fc, {0xb9401c08, 0x7101891f, 0x5400056c}}, // threshold: level > 98 -> INT_MAX
        {0x8a4590, {0xb9401013}},                         // vocation 1 -> row +0x10 (Hero)
        // Strength: floor((float)F+0x38), bonus F+0x70, clamp 999
        {0x89e374, {0xfd401e60, 0x1e624000, 0x1e300015}},
        {0x89e798, {0xb9407268, 0x52807ce9, 0x0b0802c8, 0x710f9d1f, 0x1a89b109, 0x7100013f, 0x1a9fc129, 0x7200029f}},
        {0x89e924, {0xfd402260}}, // Resilience base F+0x40
        {0x89ed48, {0xb9407668, 0x52807ce9}}, // Resilience bonus F+0x74
        {0x87ef78, {0x2a1f03e0, 0xd65f03c0, 0x2a1f03e0, 0xd65f03c0}}, // STR / RES item helpers return 0
        // per-stat final sums: bonus + floor(base) + add + ratio part
        {0x89f5c8, {0xb9407a68, 0x0b1602e9, 0x0b180129, 0x0b080128}},
        {0x89fda8, {0xb9407e68, 0x0b1602e9, 0x0b180129, 0x0b080128}},
        {0x8a0588, {0xb9408268, 0x0b1602e9, 0x0b180129, 0x0b080128}},
        {0x8a0eb8, {0xb9408668, 0x0b1602e9, 0x0b180129, 0x0b080128}},
        // ratio part (base + bonus) * sum / 100
        {0x8a0d50, {0x0b190348, 0x5290a3e9, 0x72aa3d69, 0x1b187d08, 0x1e300116, 0x9b297d08, 0xd37ffd09, 0x9365fd08}},
        // item helpers: ParamType / ParamUpType compares (AGILITY 5 ADD / RATIO, value - 100)
        {0x87efb4, {0x39432408, 0x7100151f, 0x54000101, 0x39432808, 0x7100051f, 0x540000a1}},
        {0x87f414, {0x39432408, 0x7100151f, 0x54000121, 0x39432808, 0x7100091f, 0x540000c1, 0xb940cc08, 0x51019108}},
        {0x87f2a8, {0x39432408, 0x7100051f, 0x54000121, 0x39432808, 0x7100051f, 0x540000c1}}, // OFFENSE ADD
        {0x87f278, {0x12001e88, 0x71000d1f, 0x54000141, 0xb940d408}},                         // Fighter
        {0x87f308, {0x39432408, 0x7100091f, 0x54000101, 0x39432808, 0x7100051f, 0x540000a1}}, // DEFENSE ADD
        {0x8a1070, {0x39406008, 0x7101791f}},                                                 // LUCK_ZORO 0x5e
        // GetMaxHP / GetMaxMP full path: equipment helper sum + base + bonus (F+0x68 / +0x6C), clamp 999
        {0x89d494, {0x97ff868b, 0x0b170017}},             // bl 0x87eec0 (MAXHP helper); add w23, w0, w23
        {0x89d53c, {0xb9406a89, 0x0b1802e8, 0x0b090108}}, // HP: + base F+0x28, + bonus F+0x68
        {0x89d734, {0x97ff85fa}},                         // bl 0x87ef1c (MAXMP helper)
        {0x89d7dc, {0xb9406e89}},                         // MP bonus F+0x6C
        // helpers: row ParamType 7 / 8, ParamUpType ADD (1), ParamValue +0xCC; no instance extras
        {0x87eee4, {0x39432408, 0x71001d1f, 0x54000101, 0x39432808, 0x7100051f, 0x540000a1, 0xb940cc00}},
        {0x87ef40, {0x39432408, 0x7100211f, 0x54000101, 0x39432808, 0x7100051f, 0x540000a1, 0xb940cc00}},
        // tips: DispFlagName u16 at row +0x30 -> progress; unread byte [R+0x18][i]
        {0xb9c518, {0x79406308, 0x340000e8, 0x97f35f97, 0xf9400008, 0x9104a100, 0x79406301, 0x97f36c10}},
        {0x8737c4, {0xaa0003e8, 0x2a1f03e0, 0x37f80101, 0xb9402109, 0x6b01013f, 0x540000ad, 0xf9400d08, 0x38614908,
                    0x7100011f, 0x1a9f07e0}},
        {0x873530, {0xf0026d89, 0x910ba129, 0xf9400129, 0xf9429529, 0xb4000149, 0xa944a52a}}, // R = [GD+0x48]
        // medals: received rewards P+0x40 / +0x48, RequireMedal row +0x10
        {0xc09dd4, {0xf9400008, 0xb9804909, 0x34000229, 0xf9402108}},
        {0xc09e20, {0xb9401328}},
        // current guide: progress 0x38 / 0x3b, manager +0x28, R+0xC
        {0xc02db8, {0x97f1c571, 0xf9400008, 0x52800701, 0x9104a114, 0xaa1403e0, 0x97f1d1e9, 0x360000a0, 0x52800761,
                    0xaa1403e0, 0x97f1d1e5}},
        {0xc02de8, {0x97f1c1d2, 0xa940cfe8, 0xf840c108, 0xf81f83a8}},
        {0xc02e5c, {0xf9401668, 0xf81f83a8}},
        // banner main = TargetMain (+0x10) message (+0x38); subs Disp (+0x10) / Complete (+0x20)
        {0xc03c78, {0x91004016, 0x97f43275, 0xaa1603e0, 0x2a1f03e1, 0x97fffe40, 0xb40000a0, 0xf9401c09}},
        {0xc02f14, {0xb9403800, 0x2a1f03e1, 0x94149139, 0x36000060, 0xb9403ea8, 0x34fffe48, 0x910042a0, 0x97f11d00,
                    0x3607fde0, 0x910082a0}},
        {0x84a380, {0x90020281, 0x91351021, 0x52800082}}, // condition separators
        {0xb8ca18, {0x39426288, 0x35000ce8}}, // monster list: an unrecorded Is_Boss (+0x98) row is skipped
        {0xb7abd8, {0x4b090118}},             // Types of Monster Defeated = record num - free (- Pandora pair)
        // bag full (0x8aa528): limit = B+0x21 (bLimitStockNum) ? 20 : INT_MAX; full = num B+0x30 >= limit
        {0x8aa528, {0x39408409, 0x12b0000a, 0x7100013f, 0xb9403008, 0x52800289, 0x1a890149, 0x6b09011f, 0x1a9fb7e0}},
    };
    return pins;
}

} // namespace

std::string ResolveInfo(const GuestRead& read, u64 main_base, u64 main_size, InfoRoots& roots) {
    roots = {};
    if (auto bad = CheckPins(read, main_base, main_size, InfoPins(), "info pin"); !bad.empty())
        return bad;
    // the enumerator table: {const char* name (relocated), int64 value} x 174, names equal the pins
    const u64 at = main_base + ProgressEnumParamsOffset;
    if (ProgressEnumParamsOffset + 174 * 16 > main_size)
        return "progress enum outside main";
    std::vector<u64> e(174 * 2);
    if (!read(at, e.data(), e.size() * 8))
        return "progress enum read";
    for (std::size_t k = 0; k < 174; ++k) {
        // NameUTF8 is relocated by the loader (main + rodata offset); an early resolve can still see the
        // unrelocated image-relative value, which names the same string
        const u64 value = e[2 * k + 1];
        const u64 name = e[2 * k] >= main_base ? e[2 * k] : main_base + e[2 * k];
        const auto want = ProgressEnumNames[k];
        if (value != k || name < main_base || name + want.size() + 1 > main_base + main_size) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "progress enum entry %zu (name 0x%llx value %llu)", k,
                          static_cast<unsigned long long>(e[2 * k]), static_cast<unsigned long long>(value));
            return buf;
        }
        std::string got(want.size() + 1, '\0');
        if (!read(name, got.data(), got.size()) || got.compare(0, want.size(), want) != 0 || got.back() != '\0')
            return "progress enum name " + std::to_string(k);
    }
    roots.ok = true;
    return {};
}

namespace {

constexpr u64 ReadState = 0x48, GuideManager = 0x480;
constexpr u64 PMedals = 0x34, PMedalRewards = 0x40;
using layout::PPartyManager, layout::PProgress;

std::optional<std::string> Name8(const GuestRead& read, const Roots& roots, u64 at) {
    u32 n[2]{};
    if (!read(at, n, sizeof(n)))
        return std::nullopt;
    if (n[0] == 0 && n[1] == 0)
        return std::string{};
    return ReadFName(read, roots.name_pool, n[0], n[1]);
}

} // namespace

std::optional<std::vector<OwnedItem>> ReadBagItems(const GuestRead& read, const Roots& roots, u64 bag) {
    u64 arr = 0;
    s32 n = 0;
    if (!Get(read, bag + 0x28, arr) || !Get(read, bag + 0x30, n) || n < 0 || n > 512 || (n && !arr))
        return std::nullopt;
    std::vector<u64> ptrs(static_cast<std::size_t>(n) * 2);
    if (n && !read(arr, ptrs.data(), ptrs.size() * 8))
        return std::nullopt;
    std::vector<OwnedItem> out;
    out.reserve(static_cast<std::size_t>(n));
    for (s32 k = 0; k < n; ++k) {
        const u64 slot = ptrs[static_cast<std::size_t>(k) * 2];
        u8 head[16];
        if (!slot || !read(slot, head, sizeof(head)))
            return std::nullopt;
        OwnedItem it;
        std::memcpy(&it.count, head, 4);
        it.equip = head[4];
        u64 def = 0;
        std::memcpy(&def, head + 8, 8);
        if (!def || it.count < 0 || it.equip > 6)
            return std::nullopt;
        const auto id = Name8(read, roots, def + 8);
        if (!id || id->empty())
            return std::nullopt;
        it.id = *id;
        u64 xa = 0;
        s32 xn = 0;
        if (!Get(read, def + 0x10, xa) || !Get(read, def + 0x18, xn) || xn < 0 || xn > 16 || (xn && !xa))
            return std::nullopt;
        it.extras.resize(static_cast<std::size_t>(xn));
        if (xn && !read(xa, it.extras.data(), it.extras.size() * 8))
            return std::nullopt;
        out.push_back(std::move(it));
    }
    return out;
}

namespace {

std::optional<MemberDetail> ReadMemberOnce(const GuestRead& read, const Roots& roots, u64 field) {
    MemberDetail m;
    m.field = field;
    u64 c = 0;
    if (!field || !Get(read, field + 8, c) || !c)
        return std::nullopt;
    u64 back = 0;
    if (!Get(read, c + 0x58, back) || back != field)
        return std::nullopt;
    const auto pers = Name8(read, roots, c + 0xac);
    if (!pers)
        return std::nullopt;
    m.personality = *pers;
    if (!Get(read, c + 0xaa, m.stats.vocation) || !Get(read, field + 0x1c, m.level) || !Get(read, field + 0x20, m.exp))
        return std::nullopt;
    // the stat record: F, or [F+0x1A8] while its controller holds a shared reference
    const auto stat = StatRecord(read, field);
    if (!stat)
        return std::nullopt;
    const u64 src = *stat;
    double base[6];
    s32 bonus[6];
    if (!read(src + 0x38, base, sizeof(base)) || !read(src + 0x70, bonus, sizeof(bonus)))
        return std::nullopt;
    for (int k = 0; k < 6; ++k) {
        if (!std::isfinite(base[k]) || base[k] < 0 || base[k] > 100000 || bonus[k] < -100000 || bonus[k] > 100000)
            return std::nullopt;
        m.stats.base[static_cast<std::size_t>(k)] = base[k];
        m.stats.bonus[static_cast<std::size_t>(k)] = bonus[k];
    }
    // GetMaxHP / GetMaxMP inputs from F (0x89d30c ldr w24,[x0,#0x28] / 0x89d53c ldr w9,[x20,#0x68]; MP +0x30 / +0x6C)
    if (!Get(read, field + 0x28, m.stats.hp_base) || !Get(read, field + 0x68, m.stats.hp_bonus) ||
        !Get(read, field + 0x30, m.stats.mp_base) || !Get(read, field + 0x6c, m.stats.mp_bonus) ||
        m.stats.hp_base < 0 || m.stats.hp_base > 99999 || m.stats.mp_base < 0 || m.stats.mp_base > 99999 ||
        m.stats.hp_bonus < -99999 || m.stats.hp_bonus > 99999 || m.stats.mp_bonus < -99999 || m.stats.mp_bonus > 99999)
        return std::nullopt;
    u64 bag = 0;
    if (!Get(read, c + 0x78, bag) || !bag)
        return std::nullopt;
    auto items = ReadBagItems(read, roots, bag);
    u8 limited = 0;
    if (!items || !Get(read, bag + 0x21, limited) || limited > 1)
        return std::nullopt;
    m.stats.items = std::move(*items);
    m.limited = limited != 0;
    // bag effects B+0x88 / +0x90 (16-byte shared pointers; object +0x18 = ItemEffectType)
    u64 fx = 0;
    s32 nfx = 0;
    if (!Get(read, bag + 0x88, fx) || !Get(read, bag + 0x90, nfx) || nfx < 0 || nfx > 64 || (nfx && !fx))
        return std::nullopt;
    for (s32 k = 0; k < nfx; ++k) {
        u64 obj = 0;
        u8 type = 0;
        if (!Get(read, fx + static_cast<u64>(k) * 16, obj) || !obj || !Get(read, obj + 0x18, type))
            return std::nullopt;
        if (type == 0x5e)
            m.stats.luck_override = true;
    }
    const auto entries = ReadSkillEntries(read, field);
    if (!entries)
        return std::nullopt;
    for (const RawSkill& e : *entries) {
        const auto id = ReadFName(read, roots.name_pool, e.name[0], e.name[1]);
        if (!id)
            return std::nullopt;
        m.skills.push_back({*id, e.learn, e.timing});
    }
    return m;
}

} // namespace

std::optional<u64> StatRecord(const GuestRead& read, u64 field) {
    u64 ov = 0, ctrl = 0;
    s32 refs = 0;
    if (!Get(read, field + 0x1a8, ov) || !Get(read, field + 0x1b0, ctrl))
        return std::nullopt;
    if (ov && ctrl && Get(read, ctrl + 8, refs) && refs >= 1)
        return ov;
    return field;
}

std::optional<s32> ReadStrength(const GuestRead& read, u64 field) {
    const auto rec = field ? StatRecord(read, field) : std::nullopt;
    double base = 0;
    s32 bonus = 0;
    if (!rec || !Get(read, *rec + 0x38, base) || !Get(read, *rec + 0x70, bonus) || !std::isfinite(base) || base < 0 ||
        base > 100000 || bonus < -100000 || bonus > 100000)
        return std::nullopt;
    return Clamp999(s64{Floor(base)} + bonus);
}

std::optional<std::vector<RawSkill>> ReadSkillEntries(const GuestRead& read, u64 field) {
    // skill list F+0xA8 (0x24-byte entries: +0x4 FName GOP_Magic row, +0xC learn, +0xD timing)
    u64 list = 0;
    s32 num = 0;
    if (!Get(read, field + 0xa8, list) || !Get(read, field + 0xb0, num) || num < 0 || num > 256 || (num && !list))
        return std::nullopt;
    std::vector<u8> raw(static_cast<std::size_t>(num) * 0x24);
    if (num && !read(list, raw.data(), raw.size()))
        return std::nullopt;
    std::vector<RawSkill> out(static_cast<std::size_t>(num));
    for (s32 i = 0; i < num; ++i) {
        const u8* e = raw.data() + 0x24 * i;
        RawSkill& r = out[static_cast<std::size_t>(i)];
        std::memcpy(r.name, e + 4, 8);
        r.learn = e[0xc];
        r.timing = e[0xd];
    }
    return out;
}

std::optional<MemberDetail> ReadMemberDetail(const GuestRead& read, const Roots& roots, u64 field) {
    const auto a = ReadMemberOnce(read, roots, field);
    if (!a)
        return std::nullopt;
    const auto b = ReadMemberOnce(read, roots, field);
    if (!b || !(*a == *b))
        return std::nullopt;
    return a;
}

std::optional<std::array<u64, 3>> ReadBagOwners(const GuestRead& read, const Roots& roots) {
    u64 gd = 0, p = 0, pm = 0, arr = 0;
    s32 n = 0;
    if (!ReadGdP(read, roots, gd, p) || !Get(read, p + PPartyManager, pm) || !pm || !Get(read, pm + 0x10, arr) ||
        !Get(read, pm + 0x18, n) || n <= 0 || n > 64 || !arr)
        return std::nullopt;
    std::array<u64, 3> out{};
    for (s32 k = 0; k < n; ++k) {
        u64 c = 0;
        u8 voc = 0;
        if (!Get(read, arr + static_cast<u64>(k) * 16, c) || !c || !Get(read, c + 0xaa, voc))
            return std::nullopt;
        if (voc < 11 || voc > 13)
            continue;
        const std::size_t b = voc - 11u;
        u64 bag = 0;
        if (out[b] || !Get(read, c + 0x78, bag) || !bag)
            return std::nullopt;
        out[b] = bag;
    }
    if (!out[0] || !out[1] || !out[2])
        return std::nullopt;
    return out;
}

#ifdef DQ3_TEST_API
std::optional<BagList> ReadBags(const GuestRead& read, const Roots& roots) {
    const auto owners = ReadBagOwners(read, roots);
    if (!owners)
        return std::nullopt;
    BagList out;
    for (std::size_t b = 0; b < 3; ++b) {
        auto items = ReadBagItems(read, roots, (*owners)[b]);
        if (!items)
            return std::nullopt;
        out.bags[b] = std::move(*items);
    }
    return out;
}
#endif

std::optional<JournalState> ReadJournalState(const GuestRead& read, const Roots& roots) {
    u64 g = 0, gd = 0, p = 0, r = 0;
    if (!ReadGdP(read, roots, gd, p, &g) || !g || !Get(read, gd + ReadState, r) || !r)
        return std::nullopt;
    JournalState s;
    if (!read(p + PProgress, s.progress.bits.data(), sizeof(s.progress.bits)) || !Get(read, p + PMedals, s.medals))
        return std::nullopt;
    // current MAPGUIDE (0xc02da0)
    if (s.progress.Test(0x38) && !s.progress.Test(0x3b)) {
        u64 mgr = 0, cls = 0;
        u32 cn[2]{};
        if (!Get(read, g + GuideManager, mgr) || !mgr || !Get(read, mgr + 0x10, cls) || !cls ||
            !read(cls + 0x18, cn, sizeof(cn)) || ReadFName(read, roots.name_pool, cn[0], cn[1]) != "NicolaMapGuideManager")
            return std::nullopt;
        const auto id = Name8(read, roots, mgr + 0x28);
        if (!id)
            return std::nullopt;
        s.guide = *id;
    } else {
        const auto id = Name8(read, roots, r + 0xc);
        if (!id)
            return std::nullopt;
        s.guide = *id;
    }
    u64 ra = 0;
    s32 rn = 0;
    if (!Get(read, r + 0x18, ra) || !Get(read, r + 0x20, rn) || rn < 0 || rn > 512 || (rn && !ra))
        return std::nullopt;
    s.tips_read.resize(static_cast<std::size_t>(rn));
    if (rn && !read(ra, s.tips_read.data(), s.tips_read.size()))
        return std::nullopt;
    u64 ma = 0;
    s32 mn = 0;
    if (!Get(read, p + PMedalRewards, ma) || !Get(read, p + PMedalRewards + 8, mn) || mn < 0 || mn > 64 || (mn && !ma))
        return std::nullopt;
    for (s32 k = 0; k < mn; ++k) {
        const auto id = Name8(read, roots, ma + static_cast<u64>(k) * 8);
        if (!id)
            return std::nullopt;
        s.medals_got.push_back(*id);
    }
    return s;
}

// ---------------------------------------------------------------------------------- art
namespace {

using art_util::Blank, art_util::ParseInt, art_util::RoundShape, art_util::Split;

constexpr u8 GoldRing[4] = {236, 210, 146, 255}; // revision 2 GOLD
constexpr u8 RowLine[4] = {170, 120, 40, 255};   // the selected slot's brown-gold outline
constexpr u8 SlotFill[4] = {236, 224, 196, 255}; // render.py party_page slot
constexpr u8 SlotLine[4] = {170, 145, 100, 255};
constexpr u8 SubFill[4] = {40, 36, 28, 255};     // render.py subtabs() selected box

std::mutex art_mutex;
std::shared_ptr<const Image> ui_cache[static_cast<std::size_t>(InfoUi::Count)];

std::shared_ptr<const Image> UiTex(const RangeReader& read, InfoUi id, std::string* why) {
    const auto i = static_cast<std::size_t>(id);
    {
        std::scoped_lock lock{art_mutex};
        if (ui_cache[i])
            return ui_cache[i];
    }
    const auto& pin = InfoUiPin(id);
    const auto bytes = ReadMember(read, pin.member, why);
    if (!bytes)
        return nullptr;
    auto img = DecodeMapTexture(*bytes, pin, {}, why);
    if (!img)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::scoped_lock lock{art_mutex};
    ui_cache[i] = shared;
    return shared;
}

/// 1x idle frame of class sprite n at 2x nearest, cropped to `h` rows, on a w x h canvas (centred,
/// bottom-aligned).
std::optional<Image> Sprite2x(const RangeReader& read, int n, u32 w, u32 h, bool bottom, std::string* why) {
    const auto frame = SpriteFrame(read, static_cast<u32>(n), why);
    if (!frame)
        return std::nullopt;
    Image out = Blank(w, h);
    const int sw = static_cast<int>(frame->width) * 2, sh = std::min(static_cast<int>(frame->height) * 2, static_cast<int>(h));
    const int dx = static_cast<int>(std::floor((static_cast<int>(w) - sw) / 2.0));
    const int dy = bottom ? static_cast<int>(h) - sh : static_cast<int>(std::floor((static_cast<int>(h) - sh) / 2.0));
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const int tx = dx + x, ty = dy + y;
            if (tx < 0 || tx >= static_cast<int>(w) || ty < 0 || ty >= static_cast<int>(h))
                continue;
            std::memcpy(&out.rgba[(static_cast<std::size_t>(ty) * w + static_cast<u32>(tx)) * 4],
                        &frame->rgba[(std::size_t(y / 2) * frame->width + std::size_t(x / 2)) * 4], 4);
        }
    return out;
}

} // namespace

bool IsInfoArtKey(std::string_view key) {
    static constexpr std::string_view kinds[] = {"fcard/", "portrait/", "itemicon/", "itemtype/", "ui/", "box/", "head/"};
    for (const auto k : kinds)
        if (key.starts_with(k))
            return true;
    return false;
}

std::optional<Image> ComposeInfoArt(const RangeReader& read, std::string_view key, std::string* why) {
    const auto parts = Split(key);
    const std::string_view kind = parts[0];
    if (kind == "fcard" && (parts.size() == 3 || parts.size() == 4)) {
        // render.py field_card: window 286 x 188, sprite (2x, rows <= SpriteMaxH) centred in the 80 px column
        // at x = 10 + (80 - w) / 2, centred vertically; gold outline (inset 2, radius 10, 3 px) if selected.
        // fcard/<sprite>/<sel>/<w>: the dynamic strip's width (one of StripCardW(1..4); the window is
        // nine-sliced, the sprite keeps its column, nothing is stretched)
        u32 W = FieldCardW;
        constexpr u32 H = FieldCardH;
        static_assert(H == SpriteBoxH);
        if (parts.size() == 4) {
            int w = 0;
            if (!ParseInt(parts[3], w) || !(w == StripCardW(1) || w == StripCardW(2) || w == StripCardW(3) ||
                                            w == StripCardW(4)))
                return std::nullopt;
            W = static_cast<u32>(w);
        }
        ArtLibrary lib;
        const auto win = lib.Load(read, "win/" + std::to_string(W) + "x" + std::to_string(H));
        if (!win)
            return std::nullopt;
        Image out = *win;
        int n = -1;
        if (parts[1] != "-" && !ParseInt(parts[1], n))
            return std::nullopt;
        if (n >= 0) {
            const auto canvas = ComposeSprite(read, static_cast<u32>(n), why);
            if (!canvas)
                return std::nullopt;
            Over(out, *canvas, 10 - SpriteBoxDx, 0);
        }
        if (parts[2] == "1")
            RoundShape(out, 2, 2, static_cast<int>(W) - 2, static_cast<int>(H) - 2, 10, 3, GoldRing);
        else if (parts[2] != "0")
            return std::nullopt;
        return out;
    }
    if (kind == "portrait" && parts.size() == 2) {
        // render.py party_page status: T_UI_FieldMemory_ImageFrame00 fit 112 x 112; sprite 2x cropped to
        // 104 rows, bottom at y = 4 + 104, centred at x = 56
        const auto frame = UiTex(read, InfoUi::ImageFrame, why);
        if (!frame)
            return std::nullopt;
        Image out = Resize(*frame, 0, 0, frame->width, frame->height, 112, 112);
        int n = -1;
        if (parts[1] != "-" && !ParseInt(parts[1], n))
            return std::nullopt;
        if (n >= 0) {
            const auto sp = Sprite2x(read, n, 112, 104, true, why);
            if (!sp)
                return std::nullopt;
            Over(out, *sp, 0, 4);
        }
        return out;
    }
    if (kind == "head" && parts.size() == 2) {
        // Items tab icon: the member's sprite at 1x, top-left 40 x 32 (revision 7 tab icon)
        int n = 0;
        if (!ParseInt(parts[1], n) || n < 0)
            return std::nullopt;
        const auto frame = SpriteFrame(read, static_cast<u32>(n), why);
        if (!frame)
            return std::nullopt;
        Image out = Blank(40, 32);
        for (u32 y = 0; y < 32 && y < frame->height; ++y)
            for (u32 x = 0; x < 40 && x < frame->width; ++x)
                std::memcpy(&out.rgba[(std::size_t{y} * 40 + x) * 4], &frame->rgba[(std::size_t{y} * frame->width + x) * 4], 4);
        return out;
    }
    if (kind == "itemicon" && parts.size() == 3) {
        // T_UI_Map_ItemBG_01 fit 38 x 38, the UIIconNo cell (4 x 3 of 50 px) resized to 32 at (3, 3)
        int cell = 0, px = 0;
        if (!ParseInt(parts[1], cell) || !ParseInt(parts[2], px) || cell < 0 || cell >= 12 || px < 16 || px > 128)
            return std::nullopt;
        const auto bg = UiTex(read, InfoUi::ItemBg, why);
        const auto icons = UiTex(read, InfoUi::ItemType, why);
        if (!bg || !icons)
            return std::nullopt;
        const u32 box = static_cast<u32>(px), ip = static_cast<u32>(std::lround(px * 32.0 / 38.0));
        Image out = Resize(*bg, 0, 0, bg->width, bg->height, box, box);
        const u32 cw = icons->width / 4, ch = icons->height / 3;
        const Image ic = Resize(*icons, static_cast<u32>(cell % 4) * cw, static_cast<u32>(cell / 4) * ch, cw, ch, ip, ip);
        Over(out, ic, static_cast<int>((box - ip) / 2), static_cast<int>((box - ip) / 2));
        return out;
    }
    if (kind == "itemtype" && parts.size() == 3) {
        // the UIIconNo cell alone, resized to px (the dark grids and the detail box)
        int cell = 0, px = 0;
        if (!ParseInt(parts[1], cell) || !ParseInt(parts[2], px) || cell < 0 || cell >= 12 || px < 16 || px > 128)
            return std::nullopt;
        const auto icons = UiTex(read, InfoUi::ItemType, why);
        if (!icons)
            return std::nullopt;
        const u32 cw = icons->width / 4, ch = icons->height / 3;
        return Resize(*icons, static_cast<u32>(cell % 4) * cw, static_cast<u32>(cell / 4) * ch, cw, ch,
                      static_cast<u32>(px), static_cast<u32>(px));
    }
    if (kind == "ui" && parts.size() == 3) {
        const auto size = ParseSize(parts[2]);
        if (!size)
            return std::nullopt;
        InfoUi id;
        if (parts[1] == "bagfit") { // the field tab icon: stretched to the box like render.py fit()
            const auto tex = UiTex(read, InfoUi::BagIcon, why);
            if (!tex)
                return std::nullopt;
            return Resize(*tex, 0, 0, tex->width, tex->height, size->first, size->second);
        }
        if (parts[1] == "bag")
            id = InfoUi::BagIcon;
        else if (parts[1] == "nextguide")
            id = InfoUi::NextGuide;
        else if (parts[1] == "medal")
            id = InfoUi::Medal;
        else
            return std::nullopt;
        const auto tex = UiTex(read, id, why);
        if (!tex)
            return std::nullopt;
        // d.fit: keep the aspect ratio inside the box, centred
        const double f = std::min(static_cast<double>(size->first) / tex->width, static_cast<double>(size->second) / tex->height);
        const u32 w = std::max<u32>(1, static_cast<u32>(tex->width * f)), h = std::max<u32>(1, static_cast<u32>(tex->height * f));
        Image out = Blank(size->first, size->second);
        Over(out, Resize(*tex, 0, 0, tex->width, tex->height, w, h), static_cast<int>((size->first - w) / 2),
             static_cast<int>((size->second - h) / 2));
        return out;
    }
    if (kind == "box" && parts.size() == 3) {
        const auto size = ParseSize(parts[1]);
        if (!size)
            return std::nullopt;
        Image out = Blank(size->first, size->second);
        const int w = static_cast<int>(size->first) - 1, h = static_cast<int>(size->second) - 1;
        if (parts[2] == "slot" || parts[2] == "slotsel") { // option B + C equipped slots (render.py party_page)
            RoundShape(out, 0, 0, w, h, 8, 0, SlotFill);
            if (parts[2] == "slot")
                RoundShape(out, 0, 0, w, h, 8, 2, SlotLine);
            else
                RoundShape(out, 0, 0, w, h, 8, 3, RowLine);
        } else if (parts[2] == "sub") { // render.py subtabs(): dark fill, 2 px gold outline
            RoundShape(out, 0, 0, w, h, 8, 0, SubFill);
            RoundShape(out, 0, 0, w, h, 8, 2, GoldRing);
        }
        else
            return std::nullopt;
        return out;
    }
    return std::nullopt;
}

} // namespace dq3
