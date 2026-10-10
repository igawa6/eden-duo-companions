// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Read-only battle reader for Dragon Quest III HD-2D Remake, Switch 1.1.0.0 (build
// 4F41309B39EEBE5E). Research contracts (research/dragon-quest-iii-hd-2d-remake/contracts):
// BATTLE-ROSTER.md, BATTLE-UNITS.md, BATTLE-UNITS-BOOT2.md, FIELD-STATUS-CODE.md.
//
// Routes (main-relative code, checked word by word at resolve time):
//   0x6e5834  GetUnitById: bl GetG; ldr x8,[x0,#0x38]; ldr x8,[x8,#0x150]; ldr x8,[x8,#0x28];
//             add x8,x8,w19,sxtw #4   -> S = battle subsystem, BL = [S+0x38], U = [BL+0x150],
//             unit table TArray at U+0x28 (16-byte {object, controller} elements, id-indexed)
//   0x712a28  GetG: adrp/add G slot (the same slot as dq3_reader's GD accessor), then the
//             subsystem lookup in the GameInstance's subsystem map at G+0xE0 by the class
//             0xd23a1c  adrp x8; ldr x0,[x8,#0xbe8]  -> battle subsystem UClass* slot (decoded)
// The subsystem map's set elements are {UClass* key, USubsystem* value, hash} (0x18 bytes) at
// [G+0xF0], count G+0xF8 (live-verified, BATTLE-ROSTER.md section 1; matched by key, never by index).
//
// Unit objects (offsets relative to the element's controller pointer, as every live contract
// reads them; = code `this` + 0x10): id +0x18C, group +0x190, index in group +0x198 (the letter),
// MaxHP +0x1A0, HP +0x1A4, MaxMP +0x1A8, MP +0x1AC, level +0x1CC; enemy FNames +0x30 GOP_Monster row
// (MONSTER_MN_*), +0x110 GOP_Unit_Master row, +0x120 display-name text id; party field record
// [+0x78] (= code [this+0x68], the battle-end write-back's F). Slots 0..19 enemies (defeated ones
// stay with HP 0 until the battle ends), 20..23 party.
// Groups: TArray at U+0x18, 0x30-byte entries {FName monster, FName unit master, i32 alive +0x10,
// i32 initial +0x14, ...}.
// State: X = [BL+0x30]; X+1 = current EBattleState, X+2 = previous (live-verified over five
// battles on two boots; layout read, not code-pinned). BL null before the first battle.
// Effects: C = [unit+0x160]; TArray at C+0x28 of {interface, object}; E+0x00 class vtable,
// E+0x28 remaining turns, E+0x48 FName source action, E+0x54 ENicolaUnitStatusEffectFlag bit
// for flag-type classes. NULL entries are skipped (a woken unit leaves {0, 0}).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dq3_reader.h"

namespace dq3 {

enum class BattleState : std::uint8_t {
    None = 0, OpenLevel = 1, Encount = 2, Start = 3, Init = 4, WaitMenu = 5, TurnStart = 6,
    TurnEnd = 7, ActionStart = 8, ActionEnd = 9, ActionEffect = 10, ActionMessage = 11,
    ActionScript = 12, Attack = 13, Script = 14, Win = 15, Lose = 16, Run = 17, End = 18,
};
const char* BattleStateName(std::uint8_t state);

struct BattleRoots {
    std::uint64_t class_slot{}; ///< &UClass* of the battle subsystem (main+0x5647be8 on 1.1.0)
    std::uint64_t main_base{};
    std::uint64_t g_slot{}; ///< G slot as GetG decodes it (must equal Roots::g_slot)
};
/// Checks the battle pins and decodes the subsystem class slot. Empty string on success.
std::string ResolveBattle(const GuestRead& read, std::uint64_t main_base, std::uint64_t main_size,
                          BattleRoots& roots);

struct BattleEffect {
    std::uint64_t vtable{}; ///< main-relative
    s32 turns{};
    std::uint32_t action[2]{}; ///< FName of the source action
    std::uint32_t flag{};      ///< +0x54
    bool operator==(const BattleEffect&) const = default;
};

struct BattleUnit {
    std::uint32_t slot{};
    s32 id{}, group{}, index{};
    s32 hp{}, hp_max{}, mp{}, mp_max{}, level{};
    std::uint32_t monster[2]{}, master[2]{}, name_text[2]{}; ///< enemies only
    std::uint64_t field{};                                   ///< party only: F
    std::uint8_t active{};                                   ///< +0x90 (icon gate, code vf +0xe8), all units
    /// Party: the battle record (vf +0x288 = 0x720490: code this+0x2D8 when its flag byte is set,
    /// else +0x3F8, else +0x1B8; live +0x10), built from F at battle start by 0x71de30:
    /// rec+0x1C Attack (GetAttack(F, 0)), +0x20 Defence, +0x24 Agility, +0x34 Luck, +0x38 Wisdom.
    s32 attack{}, defense{}, agility{}, luck{}, wisdom{};
    /// Action slot 0 (code obj+0x88 TArray<{FName, ActionData*}>, live +0x98; written by the
    /// command commit 0x6f0410 for the party and by the AI at TURN_START / ACTION_START) and its
    /// target (code obj+0x98 TArray<i32>, live +0xA8; EBattleTargetMaskType encoding).
    std::uint32_t action[2]{};
    s32 target{-1};
    s32 action_slots{};
    std::uint32_t record_at{}; ///< live offset of the record used (diagnostic)
    std::uint64_t ctrl{};      ///< the unit table element's controller (live object identity)
    std::vector<BattleEffect> effects; ///< party and enemies (when read with `effects`)
    bool Defeated() const {
        return hp <= 0;
    }
    bool operator==(const BattleUnit&) const = default;
};

struct BattleGroup {
    std::uint32_t monster[2]{}, master[2]{};
    s32 alive{}, initial{};
    bool operator==(const BattleGroup&) const = default;
};

/// One member's command record of the command menu: M = [BL+0x170] (0x28-byte object, ctor
/// 0x708e60), phase u8 M+0x10, TArray M+0x18 of 0x24-byte records (record i = unit 20 + i), written
/// only by SetMemberCommand 0x6f437c (mov w9,#0x24; ldr x8,[x8,#0x170]; ldr x8,[x8,#0x18]).
struct BattleCommand {
    std::uint8_t valid{};      ///< +0x00
    std::uint8_t command{};    ///< +0x01 EBattleUnitCommand: 1 ATTACK, 2 ITEM, 3 SPELL, 4 SKILL, 5 GUARD
    std::uint32_t action[2]{}; ///< +0x04 effective action FName (GOP_Magic row or item row)
    std::uint32_t item[2]{};   ///< +0x14 item FName (ITEM only)
    s32 target{};              ///< +0x20 EBattleTargetMaskType
    bool operator==(const BattleCommand&) const = default;
};

struct BattleSnapshot {
    bool in_battle{};
    std::uint8_t phase{};                   ///< M+0x10 (1 = all Fight commands confirmed)
    std::vector<BattleCommand> commands;    ///< M's records (party order)
    s32 actor{-1};                          ///< [[BL+0x150]+0x44]: the acting unit id
    std::uint8_t state{}, prev{};
    std::uint64_t logic{};          ///< BattleLogic object (diagnostic)
    std::uint32_t encounter[2]{};   ///< BL+0x50 FName
    std::vector<BattleUnit> enemies; ///< slots 0..19 in slot order (group, then index)
    std::vector<BattleUnit> party;   ///< slots 20..23
    std::vector<BattleGroup> groups;
    /// Command input is open: WAIT_MENU after INIT (WAIT_MENU after WIN/RUN/LOSE is the result
    /// message wait).
    bool CommandInput() const {
        return state == 5 && prev != 15 && prev != 16 && prev != 17;
    }
    bool operator==(const BattleSnapshot&) const = default;
};

// ------------------------------------------------------------------------- battle menu (Follow)
// The native battle command UI, read from the same widget-manager controller list the field menu
// reader uses (G+0x418 BP_NicolaUIManager_C, +0x7C0 BP_UIWidgetManager_C, controllers TArray +0x298 /
// +0x2A0, class names checked). Each battle list controller has the UI manager C at +0x30 and its
// state at +0x88: AddController 0xaa54d8 stores 1 (active, the one taking input), 0xae9010 stores 2
// when it opens a child list (suspended), 0xaa5520 stores 3 (closing), 0xaa4f80 resets 0.
// C (= the NicolaUIManager) holds the command-input state the lists share:
//   C+0x78  TArray {unit, controller} of the members to command (0xae8f10 ldr x9,[x23,#0x78]);
//   C+0xA0  i32 index of the member in control (0xae8f20 ldrsw x8,[x23,#0xa0]; 0xae6208 steps it);
//   C+0xE0  the 0x24-byte command record being built (0xae8dcc add x21,x23,#0xe0; command byte
//           +0xE1 at 0xae8ddc), the layout SetMemberCommand 0x6f437c copies into BL+0x170;
//   C+0x120 TArray {unit, controller} of the ally targets (0xaec628: ldr x9,[x21,#0x120] indexed by
//           the TargetPlayer cursor +0x98);
//   C+0x150 TArray of 0x28-byte enemy target entries {+0x00 i32 group, ...} (0xaeb2d4 cursor move and
//           0xaeb338 decide: ldrsw [x19,#0x98] * 0x28, target = group | 0x20).
// Target lists keep their cursor at +0x98 and the count at +0x9C.
enum class MenuFocus : std::uint8_t {
    None,        ///< no battle list is active (between members, a native dialog): keep the last state
    Root,        ///< UIBattleTopMenuListTop or a Tactics list (Fight / Tactics / Flee)
    Member,      ///< a member's command lists (ListUnit / Magic / Item / Equip / EquipSlot)
    TargetEnemy, ///< UIBattleUnitMenuListTargetEnemy
    TargetAlly,  ///< UIBattleUnitMenuListTargetPlayer
};
struct BattleMenu {
    MenuFocus focus{MenuFocus::None};
    s32 member{-1};       ///< unit id (20..23) of the member in control (Member / Target*)
    s32 target_group{-1}; ///< enemy group under the cursor (TargetEnemy)
    s32 target_unit{-1};  ///< unit id under the cursor (TargetAlly)
    BattleCommand pending; ///< C+0xE0 while a target list is active (valid byte 0 until confirmed)
    bool operator==(const BattleMenu&) const = default;
};
/// Checks the battle menu pins (empty string on success).
std::string ResolveBattleMenu(const GuestRead& read, std::uint64_t main_base, std::uint64_t main_size);
/// The active native battle list, matched to the snapshot's units by controller identity. nullopt
/// when the UI chain is unreadable or a value is out of range (the caller keeps its last state).
/// `class_names` caches UClass* -> name (cleared by the caller when the world changes).
std::optional<BattleMenu> ReadBattleMenu(const GuestRead& read, const Roots& roots, const BattleSnapshot& snap,
                                         std::map<std::uint64_t, std::string>* class_names = nullptr);

/// Per-boot cache of the subsystem lookup (re-validated on every read).
struct BattleCache {
    std::uint64_t g{}, subsystem{};
    s32 element{-1};
};

enum class BattleRead { Ok, NotInBattle, Invalid };

/// One pass. `effects` reads every unit's effect list too (party and enemies).
BattleRead ReadBattleOnce(const GuestRead& read, const Roots& roots, const BattleRoots& broots,
                          BattleSnapshot& out, BattleCache* cache, bool effects,
                          std::string* why = nullptr);
/// Two passes that must agree.
BattleRead ReadBattle(const GuestRead& read, const Roots& roots, const BattleRoots& broots,
                      BattleSnapshot& out, BattleCache* cache, bool effects,
                      std::string* why = nullptr);

// Monster record (the Defeated Monster List): P = [[[G]+0x528]+0x18]; TMap<FName, int32> at
// P+0x280 {elements, Num +0x288, Max, allocation bits inline +0x290 / heap +0x2A0, NumBits
// +0x2A8, FirstFree +0x2B0, NumFree +0x2B4}, 0x14-byte elements {FName key, i32 kills +0x08, ...}.
// AddMonsterKill (0x877a40, pinned at 0x877c8c: add x21,x24,#0x280; ldr w8,[x24,#0x288];
// ldr w9,[x24,#0x2b4]; stride mov w9,#0x14 at 0x877ce4) creates an entry with count 1 on the
// first win against a species, keyed by the GOP_Unit_Master row after folding aliases (static
// table built at 0x878034). The record UI shows a monster ("known") only when its key is in the
// map; others are "???" with the MonsterShadow silhouette (bosses are not listed at all).
/// The canonical record key of a unit master row (the alias table of 0x878034).
std::string RecordKey(std::string_view unit_master);
/// Pins of the record code (empty string = ok).
std::string ResolveRecord(const GuestRead& read, std::uint64_t main_base, std::uint64_t main_size);
/// The record's keys (FName text) and kill counts; nullopt when unreadable or no game.
std::optional<std::vector<std::pair<std::string, s32>>> ReadMonsterRecord(const GuestRead& read,
                                                                          const Roots& roots);

} // namespace dq3
