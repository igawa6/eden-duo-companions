// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include <array>
#include <span>
#include <string>
#include <vector>
namespace echoes {
struct NativeCheckpointCondition {
  uint32_t category{};
  std::string parameter;
  bool operator==(const NativeCheckpointCondition &) const = default;
};
struct NativeCheckpointMarker {
  std::array<float, 3> world{};
  // Retained original fields. Their roles are not qualified for rendering.
  std::array<float, 2> auxiliary{};
  uint8_t condition_strategy{}, auxiliary_byte{};
  bool side_view{};
  std::vector<NativeCheckpointCondition> conditions;
  bool operator==(const NativeCheckpointMarker &) const = default;
};
struct NativeCheckpointGroup {
  uint32_t quest_crc{};
  std::vector<NativeCheckpointMarker> markers;
  bool operator==(const NativeCheckpointGroup &) const = default;
};
// Worker-only original Common.eclo v3 metadata. Output is cleared on any error.
// These authored records grant NO visibility. Native creation additionally
// checks tracked quest key/kind, its display toggle, and every original marker
// condition. In particular, condition category3 delegates to native quest
// category16; it is not a generic boolean flag. Unknown live gates must
// suppress the icon. MapIcon_CheckPoint animation and world->map projection are
// separate contracts.
bool ParseNativeCheckpoints(std::span<const uint8_t>,
                            std::string_view level_key,
                            std::vector<NativeCheckpointGroup> &);
bool LoadNativeCheckpoints(const ReadAssetFn &, std::string_view level_key,
                           std::vector<NativeCheckpointGroup> &);
} // namespace echoes
