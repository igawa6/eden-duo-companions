// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_reader.h"
#include <string>

namespace echoes {
inline constexpr size_t MaxTraversalMaps = 32;
inline constexpr size_t TraversalBits = 512;
struct MapDiscovery {
  // Exact native TraverseLevelList index and key, never UiMap row order.
  std::string key;
  std::array<uint8_t, TraversalBits / 8> discovered{}, presented{};
  bool Discovered(size_t index) const;
  bool Presented(size_t index) const;
  bool operator==(const MapDiscovery&) const = default;
};
struct MapEpochSource {
  EpochSource save_epoch{};
  uint64_t traversal{}, registry_owner{}, registry_header{}, registry_entries{};
  bool operator==(const MapEpochSource&) const = default;
};
struct MapLevelSource {
  uint64_t controller{}, handle{}, resource{}, key_pointer{};
  bool operator==(const MapLevelSource&) const = default;
};
struct MapSnapshot {
  bool compatible{}, discovery_valid{};
  MapEpochSource source{};
  size_t map_count{};
  std::array<MapDiscovery, MaxTraversalMaps> maps{};
  // Optional raw native resource metadata. This is NOT a gameplay/title gate:
  // a level resource may remain allocated while gameplay is inactive. The key
  // is not an alias-corrected map name, a player position or a friendly label.
  MapLevelSource current_level_source{};
  std::string current_level_key;
  // Marker remains unavailable until projection lifecycle/geometry is proven.
  bool current_level_valid{}, marker_valid{};
};
// Stateless, bounded, read-only discovery observation on the supported build.
// Requires a stable save epoch, exact native registry shape, two identical full
// traversal records and final owner/header/payload revalidation. Discovery does
// not establish gameplay or defeat story fog; presented is animation state,
// not an independent reveal permission. The caller must reject unreadable ranges.
MapSnapshot ReadMapSnapshot(const ReadFn& read, uint64_t main_base,
                            bool build_matches);
} // namespace echoes
