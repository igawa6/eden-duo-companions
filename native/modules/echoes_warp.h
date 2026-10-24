// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_map_annotations.h"
namespace echoes {
struct NativeWarpDestination {
  std::string level, location, unlock_flag;
  uint32_t report_id{};
  bool operator==(const NativeWarpDestination &) const = default;
};
struct NativeWarpSet {
  std::string key;
  std::vector<NativeWarpDestination> destinations;
};
struct NativeWarpCatalog {
  std::vector<NativeWarpSet> sets;
  std::vector<std::pair<std::string, std::string>> level_sets;
  std::vector<std::pair<std::string, std::string>> level_aliases;
};
// Worker-only original metadata; no inferred destination coordinates or names.
// Authored order and distinct records sharing a location are preserved.
bool ParseNativeWarpCatalog(std::span<const uint8_t> set_sheet,
                            std::span<const uint8_t> correspondence_sheet,
                            NativeWarpCatalog &out);
bool LoadNativeWarpCatalog(const ReadAssetFn &, NativeWarpCatalog &out);
bool ParseNativeWarpLevelAliases(
    std::span<const uint8_t>,
    std::vector<std::pair<std::string, std::string>> &out);
const NativeWarpSet *NativeWarpSetForLevel(const NativeWarpCatalog &,
                                         std::string_view level);
// One authored UiMapLevelDefine lookup, then exact correspondence membership.
// No alias chains, guessed prefixes or location-name matching.
const NativeWarpSet *NativeWarpSetForCurrentLevel(const NativeWarpCatalog &,
                                                std::string_view raw_level);
struct WarpUnlockSnapshot {
  bool compatible{}, known{};
  MapEpochSource source{};
  std::vector<uint8_t> unlocked;
};
// Exact GlobalFlags observations through the existing authenticated bounded
// flag reader. This grants neither travel permission nor map visibility.
// Caller must bracket it with independently valid current gameplay/context.
WarpUnlockSnapshot ReadWarpUnlocks(const ReadFn &, uint64_t main_base,
                                  bool build_matches, const MapEpochSource &,
                                  const NativeWarpSet &);
} // namespace echoes
