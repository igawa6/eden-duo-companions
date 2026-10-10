// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Battle page game data for Dragon Quest III HD-2D Remake 1.1.0.0, parsed at runtime from the
// player's own Nicola-Switch.pak (pinned DataTable members, dq3_battle_art.h). Nothing is shipped.
//
//   GOP_Monster        row = the battle unit's FName +0x30 (MONSTER_MN_*): Level, Max_HP, Max_MP,
//                      Attack, Defense, Speed, Battle_Resist (a GOP_Battle_Resist row)
//   GOP_Battle_Resist  per-element damage multipliers of the six spell families
//                      Mela / Gila / Io / Hyado / Bagi / Dein (Frizz, Sizz, Bang, Crack, Woosh, Zap)
//   GOP_Magic          row = a spell-list entry's FName (contracts/INVENTORY.md): ListNo, ConsumeMP,
//                      SkillType, UIIconNo, NameTextId
//   GOP_Text_Noun_ENGLISH  ListNoun of TEXT_NOUN_ENGLISH_<id without "Txt_"> for every
//                      Monster_Name_* and Magic_Name_* text id (the menus' list labels)
#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dq3_pak.h"

namespace dq3 {

struct TableValue {
    enum class Kind { None, Int, Float, Name, Str, Bool, Vec2 } kind{Kind::None};
    std::int64_t i{};
    double f{}, f2{}; ///< Float; Vec2 = (f, f2)
    std::string s;     ///< names, soft object paths; strings as ASCII with '?' for other characters
    std::u32string u;  ///< StrProperty text
};
using TableRow = std::map<std::string, TableValue, std::less<>>;
using Table = std::map<std::string, TableRow, std::less<>>;

/// Rows of a cooked DataTable (uasset name map + uexp). `keep_row` filters rows (null = all);
/// only the listed columns are kept (empty = all scalar columns); `order` receives the kept row names in
/// cooked (GetAllRows) order. nullopt on a format surprise.
std::optional<Table> ParseDataTable(std::span<const u8> uasset, std::span<const u8> uexp,
                                    bool (*keep_row)(std::string_view) = nullptr,
                                    std::span<const std::string_view> columns = {},
                                    std::vector<std::string>* order = nullptr);

inline constexpr int ResistElements = 6;
inline constexpr int MaxEnemySpells = 8; ///< largest distinct spell set in the supported tables
/// The six families in the order the companion shows them, as GOP_Battle_Resist column stems and
/// the GOP_Magic rows of their first spell (whose noun is the column label).
inline constexpr std::array<std::string_view, ResistElements> ResistStems = {
    "Mela", "Gila", "Io", "Hyado", "Bagi", "Dein"};
inline constexpr std::array<std::string_view, ResistElements> ResistSpellText = {
    "Txt_Magic_Name_Mera", "Txt_Magic_Name_Gira", "Txt_Magic_Name_Io",
    "Txt_Magic_Name_Hyado", "Txt_Magic_Name_Bagi", "Txt_Magic_Name_Dein"};

/// The GOP_Magic rows of the same first spells (ListNo 1 / 4 / 7 / 10 / 14 / 20, Attribute1 MELA / GILA /
/// IO / HYADO / BAGI / DEIN, SkillUser PLAYER): their UIIconNo is the column icon. In 1.1.0.0 all six
/// are 0 (the attack-spell cell; the native battle Spells list shows the same icon for every one).
inline constexpr std::array<std::string_view, ResistElements> ResistSpellRow = {
    "MAGIC_ATTACK_MAGIC_MERA", "MAGIC_ATTACK_MAGIC_GIRA", "MAGIC_ATTACK_MAGIC_IO",
    "MAGIC_ATTACK_MAGIC_HYADO", "MAGIC_ATTACK_MAGIC_BAGI", "MAGIC_ATTACK_MAGIC_DEIN"};

/// Companion words for a GOP_Battle_Resist damage multiplier. The game has no player-facing words
/// for these levels: its tip (Txt_Information_TEXT_WEAK_RESIST, "Advantageous Attacks") only says
/// damage is red when "especially effective" and blue when it "isn't up to par", and its internal
/// names are EBattleActionRegistlType DEFAULT / REGIST / WEAK and the damage-number textures
/// T_UI_Battle_DamageNum_01_Weak / _02_Resist. The 1.1.0.0 table holds 25 distinct values (0, 0.1, 0.2,
/// 0.25, 0.3, 0.4, 0.5, 0.6, 0.7, 0.75, 0.8, 0.9, 1, 1.05, 1.1, 1.15, 1.2, 1.25, 1.3, 1.4, 1.5, 1.6, 1.7,
/// 1.8, 2), so the mapping is exact on the value: 0 Immune, below 1 Resist, 1 Normal, above 1 Weak.
enum class Affinity { Immune, Resist, Normal, Weak };
inline Affinity AffinityOf(float m) {
    if (m == 0.0f)
        return Affinity::Immune;
    if (m < 1.0f)
        return Affinity::Resist;
    if (m == 1.0f)
        return Affinity::Normal;
    return Affinity::Weak;
}
inline std::string_view AffinityWord(Affinity a) {
    switch (a) {
    case Affinity::Immune: return "Immune";
    case Affinity::Resist: return "Resist";
    case Affinity::Normal: return "Normal";
    case Affinity::Weak: return "Weak";
    }
    return "";
}

struct MonsterInfo {
    s32 level{}, max_hp{}, max_mp{}, attack{}, defense{}, speed{};
    std::string resist; ///< GOP_Battle_Resist row
    std::string action_group; ///< GOP_Battle_Monster_ActionList.Monster_ActionList_ID
};
struct ResistInfo {
    std::array<float, ResistElements> mult{};
};
struct MagicInfo {
    s32 list_no{}, consume_mp{}, icon{};
    bool magic{}; ///< ESkillType::MAGIC (else an ability)
    bool attack{}; ///< ESkillType::ATTACK (normal / status attacks: the battle menu's "Attack")
    std::string name_text;
};

struct BattleData {
    std::map<std::string, MonsterInfo, std::less<>> monsters;
    std::map<std::string, ResistInfo, std::less<>> resists;
    std::map<std::string, MagicInfo, std::less<>> magic;
    std::map<std::string, std::string, std::less<>> items; ///< GOP_Item row -> NameTextId
    /// GOP_Battle_Monster_Action row (an enemy's action slot FName) -> its GOP_Magic row.
    std::map<std::string, std::string, std::less<>> monster_actions;
    /// Action group -> distinct MAGIC NameTextIds across its conditional action rows.
    /// Empty vector means no configured spells; absent group means unavailable.
    std::map<std::string, std::vector<std::string>, std::less<>> enemy_spells;
    std::map<std::string, std::u32string, std::less<>> nouns; ///< "Txt_..." -> ListNoun
    const std::u32string* Noun(std::string_view text_id) const {
        const auto it = nouns.find(text_id);
        return it == nouns.end() ? nullptr : &it->second;
    }
};

/// Parses the pinned tables (once; thread-safe). nullptr until built.
std::shared_ptr<const BattleData> LoadBattleData(const RangeReader& read, std::string* why = nullptr);
std::shared_ptr<const BattleData> CachedBattleData();

/// Builds from caller-supplied members (tests): uasset/uexp pairs in the order Monster, Resist,
/// Magic, Noun, Item, MonsterAction, MonsterActionList.
std::optional<BattleData> BuildBattleData(std::span<const std::span<const u8>, 14> members,
                                        std::string* why = nullptr);

} // namespace dq3
