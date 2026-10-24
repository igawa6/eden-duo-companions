// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_map_annotations.h"
namespace echoes {
struct NativeLandmark {
  std::string level_key, texture_name, label_ref;
  std::array<float, 2> position{}; // original mPosX/mPosY, not normalized UV
  uint32_t condition_strategy{};
  std::vector<LocationCondition> conditions;
};
// Original UiMapIconLandMark schema only. Metadata never grants visibility.
bool ParseNativeLandmarks(std::span<const uint8_t>,
                          std::vector<NativeLandmark> &);
bool LoadNativeLandmarks(const ReadAssetFn &, std::vector<NativeLandmark> &);
struct LandmarkVisibilitySnapshot {
  bool compatible{}, known{};
  MapEpochSource source{};
  std::vector<uint8_t>
      visible; // exact input order, no rendered fallback on unknown
};
// Supports only native strategy0 + one category1 global-flag expression.
// Reuses the original guarded global-flag reader and complete source
// revalidation. Unknown conditions/source changes clear the entire result.
// Caller still checks current map scope, exact level, authored projection and
// conservative fog coverage.
LandmarkVisibilitySnapshot
ReadLandmarkVisibility(const ReadFn &, uint64_t main_base, bool build_matches,
                       const MapEpochSource &, std::span<const NativeLandmark>);
} // namespace echoes
