// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle.h"
#include <array>
namespace ot0 {
struct BattleFormation {
  bool valid{};
  std::array<uint32_t, 8> ids{};
  bool operator==(const BattleFormation &) const = default;
};
// Live HUD actor bindings, slots0..3 front /4..7 back. IDs preserve the
// caller's BattleSnapshot aliases. All current participants must bind once.
// Numeric HP/SP caches do not establish or invalidate native front/back roles.
BattleFormation ReadBattleFormation(const ReadMemory &read, uint64_t main,
                                    const BattleSnapshot &battle,
                                    int64_t relocation_delta = 0);
// Currently bound party actor from the visible active command list in native
// command/target selection. Zero means unavailable; caller may keep its normal
// action/turn-order presentation. No identity is inferred from queue or HP.
uint32_t ReadCommandActor(const ReadMemory &read, uint64_t main,
                          const BattleSnapshot &battle,
                          int64_t relocation_delta = 0);
struct ResourceLimits {
  uint32_t max_hp{}, max_sp{};
  bool valid{};
  bool operator==(const ResourceLimits &) const = default;
};
// The native battle status widgets cache their own computed maxima. Values
// are accepted only for a current party actor whose cached HP/SP exactly match
// the caller's battle snapshot. Unknown/transitional values stay unknown.
std::array<ResourceLimits, 8>
ReadBattleResourceLimits(const ReadMemory &read, uint64_t main,
                         const BattleSnapshot &battle,
                         int64_t relocation_delta = 0);
// Fresh numeric text from the visible normal MainMenu party panel only.
// Hidden/cached menus and other state25 pages do not provide field maxima.
std::array<ResourceLimits, 8>
ReadMenuResourceLimits(const ReadMemory &read, uint64_t main,
                       const Snapshot &party, int64_t relocation_delta = 0);
} // namespace ot0
