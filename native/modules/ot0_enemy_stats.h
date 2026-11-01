// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle.h"
namespace ot0 {
struct EnemyStats {
  int32_t max_hp{};
  bool max_hp_valid{};
  int32_t speed{};
  bool speed_valid{};
  bool operator==(const EnemyStats &) const = default;
};
// Native computed cache only; unavailable entries remain unknown. Same enemy
// order as BattleSnapshot. No database estimates or guest calls.
std::array<EnemyStats, 6> ReadEnemyStats(const ReadMemory &, uint64_t main,
                                         const BattleSnapshot &,
                                         int64_t relocation_delta = 0);
} // namespace ot0
