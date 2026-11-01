// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle.h"
namespace ot0 {
inline constexpr std::array<uint8_t, 8> CombatParameterIds{9,  10, 11, 12,
                                                           13, 14, 15, 16};
struct ComputedParameter {
  int32_t value{};
  bool valid{};
  bool operator==(const ComputedParameter &) const = default;
};
struct ActorCombatStats {
  uint32_t id{};
  bool bound{};
  // Native ECHARA_STATUS9..16; labels are supplied by BattleMetadata.
  std::array<ComputedParameter, 8> parameters{};
  // Standard native getter uses equipment weapons[slot], not the best of two.
  int32_t equipment_weapon_slot{-1};
  bool weapon_context_valid{};
  uint64_t native_actor{}, raw_ailment_fingerprint{};
  uint32_t native_serial{};
  uint8_t weapon_selector{};
  // Caller must additionally match a recent native helper ACK before presenting
  // these cache values as live. Missing old-replay serial data remains unknown.
  bool signature_valid{};
  bool operator==(const ActorCombatStats &) const = default;
};
struct BattleStatsSnapshot {
  bool valid{};
  // Indexes match BattleSnapshot members/enemies, independent of native cursor.
  std::array<ActorCombatStats, 8> party{};
  std::array<ActorCombatStats, 6> enemies{};
  bool operator==(const BattleStatsSnapshot &) const = default;
};
// Only native computed cache variants1/1/0; absent values stay unknown. An
// effect-free game-thread native getter may populate these caches separately.
BattleStatsSnapshot ReadBattleStats(const ReadMemory &, uint64_t main,
                                    const BattleSnapshot &,
                                    int64_t relocation_delta = 0);
} // namespace ot0
