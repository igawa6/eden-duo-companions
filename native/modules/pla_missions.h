// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only quest definitions from the player's 1.1.1 ROMFS. Conditions/rewards
// are intentionally not interpreted without a validated schema.
#pragma once
#include "pla_metadata.h"
#include <algorithm>

namespace pla {
struct MissionStep {
  uint8_t id = 0;
  uint64_t outline = 0, purpose = 0;
};
struct MissionDefinition {
  uint32_t id = 0;
  uint64_t state_hash = 0, title_hash = 0;
  bool request = false;
  std::vector<MissionStep> steps;
};
struct MissionDefinitions {
  bool valid = false;
  std::vector<MissionDefinition> quests;
};
inline MissionDefinitions
ParseMissionDefinitions(std::span<const uint8_t> bytes, bool requests) {
  FlatMetadata f(bytes);
  const auto root = f.Read(0, 4);
  const auto tables = f.Tables(root, 0, 128);
  const unsigned expected = requests ? 122 : 27;
  if (!f.Valid() || tables.size() != expected)
    return {};
  MissionDefinitions result;
  for (const auto table : tables) {
    MissionDefinition q;
    q.id = uint32_t(f.Scalar(table, 2, 4));
    q.state_hash = f.Scalar(table, 1, 8);
    q.title_hash = f.Scalar(table, 4, 8);
    q.request = requests;
    const auto ids = f.U32Values(table, 8, 64);
    const auto outlines = f.Tables(table, 9, 64),
               purposes = f.Tables(table, 11, 64);
    if (!f.Valid() || q.id < 1 || q.id > expected || !q.state_hash ||
        !q.title_hash || ids.empty() || ids.size() != outlines.size() ||
        ids.size() != purposes.size())
      return {};
    for (const auto &previous : result.quests)
      if (previous.id == q.id || previous.state_hash == q.state_hash)
        return {};
    for (size_t j = 0; j < ids.size(); ++j) {
      // Zero entries are real placeholders in these tables; preserve
      // their positions but never expose them as an active objective.
      if (ids[j] > 255 || (ids[j] && std::find(ids.begin(), ids.begin() + j,
                                               ids[j]) != ids.begin() + j))
        return {};
      q.steps.push_back({uint8_t(ids[j]), f.Scalar(outlines[j], 0, 8),
                         f.Scalar(purposes[j], 0, 8)});
    }
    if (!f.Valid())
      return {};
    result.quests.push_back(std::move(q));
  }
  result.valid = true;
  return result;
}
// Native received-list membership only. Raw0/1 are not received; neither
// establishes global unlock/offer eligibility. Unknown states remain unavailable.
enum class MissionStatus { Unavailable, NotReceived, Active, Completed };
struct MissionState {
  MissionStatus status = MissionStatus::Unavailable;
  unsigned ordinal = 0; // Original vector position, not player progress.
  uint64_t outline = 0, purpose = 0;
};
inline MissionState ClassifyMission(const MissionDefinition &q, bool readable,
                                    uint8_t value) {
  if (!readable)
    return {};
  if (value <= 1)
    return {MissionStatus::NotReceived};
  const auto step = std::find_if(q.steps.begin(), q.steps.end(),
                                 [&](const auto &s) { return s.id == value; });
  // Unknown states cannot reveal names or an invented objective.
  if (step == q.steps.end())
    return {};
  MissionState state;
  state.status = value == 255 ? MissionStatus::Completed : MissionStatus::Active;
  state.ordinal = unsigned(step - q.steps.begin());
  state.outline = step->outline;
  if (state.status == MissionStatus::Active)
    state.purpose = step->purpose;
  return state;
}
} // namespace pla
