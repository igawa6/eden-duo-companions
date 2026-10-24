// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_player.h"
#include <vector>
namespace echoes {
struct DungeonFloorHeight { float floor{}, height{}; };
struct DungeonRoom {
  uint16_t number{};
  std::array<float,3> lo{}, hi{};
  std::vector<int32_t> floors;
  std::vector<DungeonFloorHeight> floor_heights;
  bool side_view{};
};
struct DungeonMapDefinition {
  std::string key;
  uint8_t major_index{};
  int32_t floor_min{}, floor_count{}, floor_text_first_index{};
  float size_ratio{}, base_x{}, base_y{};
  std::array<float,3> lo{}, hi{};
  std::vector<DungeonRoom> rooms;
};
// Worker only: original MajorDungeonDefinition, UiMap, UiMapArea and Common.eclo.
// Only exact major map keys with native room/floor membership are returned.
bool LoadDungeonCatalog(const ReadAssetFn&, std::vector<DungeonMapDefinition>&);
struct DungeonPlayerRoom {
  bool valid{};
  uint16_t room{};
  int32_t floor{};
  bool side_view{};
};
// Replays original authored room containment and floor-height selection. This
// returns no projected map position and grants no gameplay/reveal permission.
DungeonPlayerRoom ResolveDungeonPlayerRoom(const DungeonMapDefinition&,
                                           const PlayerWorldSnapshot&);
// Original ui/Map message key for an authored floor. Empty when the floor or
// native floor label table is unsupported; callers must not substitute raw indices.
std::string_view DungeonFloorMessageKey(const DungeonMapDefinition&, int32_t floor);
struct DungeonProgressSnapshot {
  bool compatible{}, valid{};
  std::string key;
  uint8_t native_index{};
  uint64_t visited_rooms{};
  bool map_item{};
  MapEpochSource source{};
  uint64_t registry_owner{}, registry_header{}, registry_entries{}, record{};
};
// Read-only optional per-dungeon record, independent of Field traversal bits.
// The expected authenticated save epoch and exact original registry are replayed.
DungeonProgressSnapshot ReadDungeonProgress(const ReadFn&, uint64_t main_base,
    bool build_matches, const MapSnapshot&, const DungeonMapDefinition&);
} // namespace echoes
