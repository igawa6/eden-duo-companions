// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_map.h"
#include "echoes_map_markers.h"
namespace echoes {
struct NativeQuestMarkerQuest {
  std::string key;
  uint32_t kind{};
  std::vector<uint32_t> progress_crcs;
};
// Original Quest.gsheet key/kind/progress order. No bundled quest progression.
bool LoadMarkerQuestCatalog(const ReadAssetFn &,
                            std::vector<NativeQuestMarkerQuest> &);
struct VisibleCheckpointMarker {
  uint32_t group{}, marker{}, tracked_kind{};
  bool operator==(const VisibleCheckpointMarker &) const = default;
};
struct CheckpointVisibilitySnapshot {
  bool compatible{}, known{};
  MapEpochSource source{};
  std::array<uint32_t, 2> tracked_quests{};
  std::array<bool, 2> display{};
  std::vector<VisibleCheckpointMarker> visible;
  size_t unsupported_markers{};
};
// Read-only original save backing/container + original runtime quest/stage
// hashes. Three equal samples and complete final trace replay; exact map/save
// epoch. Output contains only positively proven marker references, never
// guessed positions. Supported native condition categories1(active
// quest),3(quest stage comparisons and inclusive range);
// unsupported/sentinel/malformed conditions hide that marker. Caller must gate
// gameplay/current map and independently prove native projection, icon
// attachment/animation and terrain/story coverage before drawing.
CheckpointVisibilitySnapshot
ReadCheckpointVisibility(const ReadFn &, uint64_t main_base, bool build_matches,
                         const MapEpochSource &,
                         std::span<const NativeQuestMarkerQuest>,
                         std::span<const NativeCheckpointGroup>);
} // namespace echoes
