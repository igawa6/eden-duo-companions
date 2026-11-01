// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle.h"
namespace ot0 {
enum class BattleSelectionPhase {
  None,
  Command,
  EnemyTarget,
  AllyTarget,
  GroupTargets
};
struct BattleSelection {
  bool valid{};
  BattleSelectionPhase phase{};
  uint32_t actor_id{}, ally_id{};
  int enemy_index{-1}, command_index{-1}, list_type{}, target_count{};
  bool operator==(const BattleSelection &) const = default;
};
// Visible command binding and the native target vector/index used by
// 3b8dc60. Enemy IDs are not unique: match actual pointers after the native
// a40 sort, then join the caller's enemy-array slot. No cursor is guessed.
BattleSelection ReadBattleSelection(const ReadMemory &, uint64_t main,
                                    const BattleSnapshot &, int64_t delta = 0);
} // namespace ot0
