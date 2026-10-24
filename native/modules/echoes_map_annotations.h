// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include "echoes_map.h"
#include <array>
#include <span>
#include <string>
#include <vector>
namespace echoes {
struct LocationCondition {
  uint32_t category{};
  std::string parameter;
};
struct NativeLocation {
  std::string key, visible_flag, name_label, additional_label, name, additional;
  bool show_hud{}, show_rupees{};
  uint32_t field_type{}, condition_strategy{};
  std::vector<LocationCondition> conditions;
};
struct NativeRiftRegion {
  uint32_t unique_id{};
  std::string activate_flag, deactivate_flag;
  std::vector<std::vector<std::array<float, 2>>> rings;
};
// Worker-only parsing of original GSHT v2 resources. These describe metadata;
// neither loaded regions nor location rows grant current
// visibility/progression.
bool ParseNativeLocations(std::span<const uint8_t>,
                          std::vector<NativeLocation> &);
bool ParseNativeRiftRegions(std::span<const uint8_t>,
                            std::vector<NativeRiftRegion> &);
// Exact original label references only. Missing localization never substitutes
// internal keys for a localized name. locale_root is e.g. regionUS/USen.
bool LocalizeNativeLocations(const ReadAssetFn &, std::string_view locale_root,
                             std::vector<NativeLocation> &);
struct RiftRegionState {
  uint32_t unique_id{};
  bool active{};
  bool operator==(const RiftRegionState &) const = default;
};
struct RiftVisibilitySnapshot {
  bool compatible{}, known{};
  MapEpochSource source{};
  std::vector<RiftRegionState> regions;
};
// Optional native flag observation, bound to an already validated map epoch.
// Caller brackets normal gameplay/map scope around this result. Unknown clears
// all region states; it does not authorize drawing or removing a rift overlay.
RiftVisibilitySnapshot ReadRiftVisibility(const ReadFn &, uint64_t main_base,
                                          bool build_matches,
                                          const MapEpochSource &,
                                          std::span<const NativeRiftRegion>);
struct CurrentLocationSnapshot {
  bool compatible{}, known{};
  MapEpochSource source{};
  MapLevelSource level_source{};
  std::string key;
};
// Persistent current resource's native UiLocationInfo key, not the transient
// announcement string. Requires original metadata membership and three equal
// observations with final source revalidation. Caller still gates gameplay.
CurrentLocationSnapshot ReadCurrentLocation(const ReadFn &, uint64_t main_base,
                                            bool build_matches,
                                            const MapSnapshot &,
                                            std::span<const NativeLocation>);
} // namespace echoes
