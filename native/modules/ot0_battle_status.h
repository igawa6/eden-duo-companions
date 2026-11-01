// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle.h"
#include "ot0_battle_metadata.h"
#include <vector>
namespace ot0 {
struct VisibleBattleStatus {
  uint32_t type_id{}, text_id{};
  std::string name, texture_path, asset_class;
  // texture_path is the exact native object asset path; PaperSprite frames
  // require their native atlas rectangle, not a guessed standalone texture.
  int32_t counter{};
  // Native ViewIconsInfo flags, not guessed stack/duration enums.
  uint8_t counter_mode{}, category{};
  bool counter_valid{};
  bool positive{}, negative{}, classification_valid{};
  bool operator==(const VisibleBattleStatus &) const = default;
};
struct ActorBattleStatuses {
  uint32_t id{};
  bool valid{};
  uint32_t unknown_count{};
  std::vector<VisibleBattleStatus> entries;
  bool operator==(const ActorBattleStatuses &) const = default;
};
struct BattleStatusSnapshot {
  bool valid{};
  std::array<ActorBattleStatuses, 8> party{};
  std::array<ActorBattleStatuses, 6> enemies{};
  bool operator==(const BattleStatusSnapshot &) const = default;
};
// Native visible aggregate858/24bytes, joined with current raw800/e8 types;
// native grouping/order/counters retained. Unproved support captions stay
// unknown.
BattleStatusSnapshot ReadBattleStatuses(const ReadMemory &, uint64_t main,
                                        const BattleSnapshot &,
                                        const BattleMetadata &,
                                        int64_t relocation_delta = 0);
} // namespace ot0
