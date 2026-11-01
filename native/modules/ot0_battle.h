// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only battle state, Switch 1.0.5 (EAPP_STATE 26). Live-confirmed routes
// (research analysis/20261025-battle-route): battle manager [main+0x7cc5b78]
// enemy actors at +0xa98, turn order at +0x8b8 (this turn) / +0x8f8 (next
// turn); per actor EnemyID +0x298, alive +0x290, revealed weakness masks +0xa48
// (weapon) / +0xa4c (magic); save battle record S+0x4f0 arrays indexed by enemy
// / party battle order. No guest calls or writes. Unrevealed weaknesses are
// never read from the game tables here.
#pragma once
#include "ot0_reader.h"
#include <array>
#include <cstdint>
namespace ot0 {
struct BattleEnemy {
  uint32_t enemy_id{};    // EnemyID m_id
  uint32_t weapon_mask{}; // bit i = EWEAPON_TYPE i revealed (1 Sword .. 8 Fan)
  uint32_t magic_mask{};  // bit i = EMAGIC_TYPE i revealed (1 Fire .. 6 Dark)
  int32_t shields{};
  bool alive{};
  int32_t hp{-1}, max_hp{}, speed{-1}; // Live-only; -1/0 means unavailable, never a table estimate.
  bool operator==(const BattleEnemy &) const = default;
};
struct BattleMember {
  uint32_t id{};
  int32_t hp{}, sp{}, bp{};
  std::array<uint32_t, 2>
      weapons{}; // equipped weapon item ids (S+0x158 records, 0 = empty)
  // Optional, live-validated limits. Zero means unavailable; never infer a
  // maximum from current resources or a static character table.
  int32_t max_hp{}, max_sp{};
  // Boost level chosen for the pending command (actor +0x510, 0..3): BP the command will spend.
  int32_t boost{};
  // Current command weapon (actor +0x2a8: 1 = weapons[0], 0 = weapons[1]); -1 unknown.
  int32_t weapon_slot{-1};
  bool operator==(const BattleMember &) const = default;
};
// Turn-order entry: enemy index (0..5) or a party id.
struct TurnEntry {
  bool enemy{};
  uint32_t ref{};
  bool operator==(const TurnEntry &) const = default;
};
struct BattleSnapshot {
  bool valid{};
  int enemy_count{}, member_count{}, now_count{}, next_count{};
  int now_index{}; // BM+0x8e0: entries [0, now_index) of `now` have already
                   // acted
  std::array<BattleEnemy, 6> enemies{};
  std::array<BattleMember, 8> members{};
  std::array<TurnEntry, 16> now{}, next{};
  // Visible native command binding may select a back partner while the turn
  // queue still names the front member. Zero leaves normal action/turn flow.
  uint32_t command_actor_id{};
  bool operator==(const BattleSnapshot &) const = default;
};
// Presentation facts, not guessed native command-step values. A native action
// animation can use FINISH_COMMAND_DECIDE just like the results screen does.
enum class BattleFlow { Unavailable, Combat, NoLivingFoes, NoStandingParty };
BattleFlow GetBattleFlow(const BattleSnapshot &battle);
// The game's shared confirmation dialog, used by return-to-title and other
// native menus. No dialog text or selection is inferred from this visibility.
struct ModalState {
  bool valid{}, active{};
  bool operator==(const ModalState &) const = default;
};
ModalState ReadModalState(const ReadMemory &read, uint64_t main,
                          int64_t relocation_delta = 0);
BattleSnapshot ReadBattle(const ReadMemory &read, uint64_t main,
                          int64_t relocation_delta = 0);
} // namespace ot0
