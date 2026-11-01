// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_data.h"
#include <array>
namespace ot0 {
struct CombatParameterInfo {
  uint32_t id{}, text_id{}, icon_id{}, frame_icon_id{};
  std::string name;
};
struct BattleAilmentInfo {
  uint32_t id{}, text_id{}, explanation_id{}, icon_id{}, support_icon_id{};
  std::string name, explanation;
  bool positive{}, negative{}, no_count{}, count_down{}, multiply{};
};
struct BattleMetadata {
  bool valid{};
  // ECHARA_STATUS 9..16, in native order; names and artwork IDs from owner's
  // DB.
  std::array<CombatParameterInfo, 8> parameters{};
  std::unordered_map<uint32_t, BattleAilmentInfo> ailments;
  MapNames ui_texts, skill_texts;
  // Unlocalized/unused entries are unknown; no label synthesized from enum
  // names.
  const BattleAilmentInfo *FindAilment(uint32_t id) const;
};
BattleMetadata DecodeBattleMetadata(std::span<const uint8_t> chara_status,
                                    std::span<const uint8_t> ailment_types,
                                    std::span<const uint8_t> english_ui,
                                    std::span<const uint8_t> english_skills);
} // namespace ot0
