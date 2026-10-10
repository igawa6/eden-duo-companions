// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The battle page's published values (revision 7 "Tactical" layout, research
// tools/render-revision7.py battle()), built from the battle snapshot, the field party, the game
// tables and the companion-side selection. Pure: no guest access, unit-testable.
//
// Selection (taps change only this companion state): none, one enemy cell (display index) or one
// party card. Modes: none -> enemies with names + full party cards with sprites above the name;
// enemy -> enemy detail scrap + short cards; party -> enemies without names + the member's
// two-column detail + short cards with the selected card grown downward (sprite under MP).
//
// Status icons on the battle party cards follow the game's battle status window (research
// runs/slice2-battle/re/status-icons): panel 00 cells 1, 3, 5, 19, 22, 20, 23, 21 and panel 01
// cells 2, 4, 6, 16, 18, 8 of T_UI_Common_Status_StatusIcon_00, each shown while its effect's
// remaining turns are > 0 (UnitObject+0x90 != 0 gates the unit); flag classes match
// ENicolaUnitStatusEffectFlag E+0x54 against cell 1 BAIKIRUTO 0x1000, 16 MAHOTON 0x20,
// 18 STUN 0x10000 | LIQUOR 0x10000000, 19 BEAST 0x20000, 20 COUNTER_BREATH 0x40000, 21 BUILD_UP
// 0x80000, 22 NO_DAMAGE 0x100000, 23 AUTO_HEAL_MP 0x200000; buff/debuff classes match
// EBattleBuffDebuffFlag E+0x54 against 2 OFFENSE_DOWN 0x2, 3 DEFENSE_UP 0x4, 4 DEFENSE_DOWN 0x8,
// 5 SPEED_UP 0x10, 6 SPEED_DOWN 0x20, 8 SPELL_WEAKNESS 0x800. The game shows three per panel and
// pages every 2.0 s (UI_CONSTPARAM_BATTLE_ICON_VAR_STATUS_WAIT_SEC); the card shows three at a
// time from both panels in that order and pages the same way.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dq3_battle.h"
#include "dq3_font.h"
#include "dq3_sprites.h"
#include "dq3_tables.h"

namespace dq3 {

enum class BattleSel { None, Enemy, Party };

/// Field-side facts about one active member (Line-Up order), from the field reader and cards.
struct BattleMember {
    std::u32string name;
    std::u32string job;    ///< job label ("Warrior"), may be empty
    s32 level{};
    std::uint64_t field{}; ///< F (matches the battle unit's +0x78)
    std::optional<u32> sprite; ///< class sprite index (dq3_sprites.h)
    std::optional<u32> coffin; ///< coffin sprite index (for a KO in battle)
    /// Strength (GetStr 0x89e2e0: clamp(floor((float)s.double[+0x38]) + s+0x70, 0, 999), s = the stat
    /// record dq3::StatRecord(F); no equipment term). Attack / Defence / Agility / Wisdom / Luck come
    /// from the battle record.
    s32 strength{};
    bool strength_ok{};
    struct Spell {
        std::u32string name; ///< "???" for a known-but-unlearned entry (state 0)
        s32 mp{};            ///< GOP_Magic.ConsumeMP
        int state{};         ///< battle list state (0xae4844): 0 unknown, 1 not enough MP, 2 usable
    };
    std::vector<Spell> spells; ///< battle Spells list in F+0xA8 order (selected member only)
    /// Icon cells in the game's display order (MergeIconOrder); nullopt = candidate order.
    std::optional<std::vector<u32>> icons;
};

struct BattleViewInput {
    const BattleSnapshot* battle{};
    std::vector<BattleMember> members;
    const BattleData* data{};
    const FontAtlas* font{};
    const TextColours* colours{};
    /// FName text of a unit field ({index, number}); nullopt when unreadable.
    std::function<std::optional<std::string>(const std::uint32_t name[2])> fname;
    const std::set<std::string>* known{}; ///< monster record keys (nullptr = record unreadable)
    BattleSel sel{BattleSel::None};
    int sel_index{-1};
    int icon_page{}; ///< increments every 2.0 s
    /// Follow: the selection comes from the game's battle state (FollowSelection below) instead of
    /// `sel` / `sel_index`.
    bool follow{};
    /// The last settled native battle list (dq3_battle.h ReadBattleMenu; MenuFocus::None readings
    /// keep the previous one). nullopt = unreadable / not resolved: command input shows nothing.
    std::optional<BattleMenu> menu;
    /// The acting unit id is current (false between TURN_START and the first actor write, see
    /// ActorCurrent).
    bool actor_valid{true};
};

struct BattleView {
    std::vector<std::pair<std::string, std::int64_t>> ints;
    std::vector<std::pair<std::string, std::string>> texts;
    BattleSel sel{BattleSel::None}; ///< the selection after validation
    int sel_index{-1};
    bool swap{};                    ///< enemy-turn layout (party cards on top, see FollowSelection)
    bool retry{};                   ///< some text was unavailable (FName, table): rebuild later
};

BattleView BuildBattleView(const BattleViewInput& in);

/// Follow mode (owner spec, research runs/follow-states): what the companion shows, from the game's own
/// battle state only (no timers).
///  - action phase (ACTION_START .. SCRIPT, the acting unit [[BL+0x150]+0x44] current): an enemy actor
///    with HP > 0 selects that enemy (an actor at HP 0 is out of the battle: nothing) and swaps the layout (party cards with their moves on top, the acting enemy's
///    card, the Enemies box at the bottom); a party actor selects that member (party detail, grown card).
///  - command input (WAIT_MENU after INIT), by the active native list: Root -> nothing (overview);
///    Member -> the member in control; TargetEnemy -> the first living unit of the group under the
///    cursor (every living unit of that group glows); TargetAlly -> the member under the cursor.
///  - anything else (turn start / end, results, flee): nothing.
struct FollowSelection {
    BattleSel sel{BattleSel::None};
    int index{-1};           ///< enemy display index / party card index
    bool swap{};             ///< an enemy is acting
    std::vector<int> glow;   ///< enemy display indices with the selection glow
};
FollowSelection FollowSelect(const BattleSnapshot& b, const std::vector<BattleMember>& members,
                             const std::optional<BattleMenu>& menu, bool actor_valid);
/// The acting unit id is written only after the first ACTION_START of a turn begins (live, runs/follow-states
/// poll logs): until then it still holds the value it had at TURN_START. `turn_actor` is the actor read
/// at TURN_START (or at the battle's first read).
bool ActorCurrent(const BattleSnapshot& b, s32 turn_actor);

/// The status window's list update (0xae4ed0): entries whose effect ended are removed, the rest
/// keep their place, and newly active cells are appended in candidate order.
void MergeIconOrder(std::vector<u32>& order, const std::vector<u32>& active);

/// Battle party icon cells (candidate order), from the unit's effect list.
std::vector<u32> BattleIconCells(const BattleUnit& unit);
/// The status icon cells of `cells` that belong to one detail stat, in `cells` order: 0 Attack (1
/// BAIKIRUTO = Oomph, 2 OFFENSE_DOWN), 1 Defence (3 DEFENSE_UP = Kabuff/Buff, 4 DEFENSE_DOWN = Sap),
/// 2 Agility (5 SPEED_UP = Acceleratle, 6 SPEED_DOWN = Deceleratle). The game exposes no buffed stat
/// value: a buff is a phase (effect vfunc 0x757254 = +/- GOP_Battle_StatusEffect EffectValue) used only
/// by the damage / turn-order calculation, and the player sees only these icons. The detail shows
/// them beside the stat (party: the battle record value; enemy: the GOP_Monster value).
std::vector<u32> StatIconCells(const std::vector<u32>& cells, int stat);

// Layout constants shared with tools/dq3/gen_manifest.py (revision 2 "current-compact").
namespace bl {
inline constexpr int X0 = 36, Y0 = 32, X1 = 1204, Y1 = 1048, CW = X1 - X0, G = 8, PAD = 14;
inline constexpr int HeadH = 88, EnemyY = 128; // Y0 + HeadH + G
inline constexpr int BoxNames = 244, BoxBare = 172;       // Enemies box with / without names
inline constexpr int CellNames = 100, CellBare = 64;      // enemy cell heights
inline constexpr int Gi = 18, Gap = 6;                    // grid inset, cell gap
inline constexpr int EnemyCellW = 221;                    // (CW - 2 Gi - 4 Gap) / 5
inline constexpr int Mv = 92, PartyBottom = 956;          // Y1 - Mv
inline constexpr int ShortH = 150, FullH = 280;
inline constexpr int TabsW = 400, TabsX = X1 - TabsW;
inline constexpr int EnemySpellW = 139, EnemySpellCol = 151, EnemySpellRowH = 40; // two columns, 40 px rows
inline constexpr int EnemySpellRowW = 2 * EnemySpellCol - 12;                      // a name across both
inline constexpr int AffIconPx = 56; // affinity cell element icon (elem/<k>/56, gen_manifest AFF_ICON)
inline constexpr int NormalDetailY = 380, NormalDetailH = 418; // under the Enemies box, above the short cards
static_assert(EnemyY == Y0 + HeadH + G && PartyBottom == Y1 - Mv && NormalDetailY == EnemyY + BoxNames + G &&
              NormalDetailH == PartyBottom - ShortH - G - NormalDetailY && EnemyCellW == (CW - 2 * Gi - 4 * Gap) / 5);
// Follow enemy-turn layout: party short cards under the header, their moves, the acting enemy's card,
// the "Enemies" strip, then the Enemies box (with names) ending on the page bottom (Y1).
inline constexpr int SwapPartyBottom = 278, SwapDetailY = 374, SwapEnemyY = 804;
inline constexpr int SwapDetailH = 368; // ends 6 px above the heading's 48 px TitleBG strip
static_assert(SwapPartyBottom == EnemyY + ShortH && SwapDetailY == SwapPartyBottom + Mv + 4 &&
              SwapEnemyY == Y1 - BoxNames && SwapDetailY + SwapDetailH == SwapEnemyY - G - 48 - 6);
} // namespace bl

/// Fits a complete spell name in `width` (one enemy-card column, or a whole row) using native font metrics:
/// Semibold 28, else 24 / 22 / 20.
std::string EnemySpellLabel(std::u32string_view name, const FontAtlas* font, int width = bl::EnemySpellW);
/// Possible-spells placement: names in reading order, two per row; a name wider than a column at the
/// smallest size takes a whole row. Returns {column 0 / 1, row} per name.
std::vector<std::pair<int, int>> EnemySpellCells(const std::vector<std::u32string>& names, const FontAtlas* font);

} // namespace dq3
