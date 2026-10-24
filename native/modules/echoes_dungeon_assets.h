// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_dungeon.h"
namespace echoes {
struct DungeonRoomArt {
  uint16_t room{};
  std::string area_name;
  int32_t priority{};
  float position_x{}, position_y{}, texture_scale{};
  std::vector<std::pair<std::string, std::string>> conditional_textures;
  std::vector<int32_t> texture_floors;
};
struct DungeonConnectionArt {
  std::string start_change_floor, end_change_floor;
  std::vector<std::array<float, 2>> corners;
};
struct DungeonMapArt {
  std::string key;
  std::vector<DungeonRoomArt> rooms;
  std::vector<DungeonConnectionArt> connections;
};
struct DungeonRoomFocus {
  bool valid{};
  std::array<float, 2> center_uv{}, extent_uv{};
};
// Companion camera focus at the authenticated current room's authored center,
// not a precise Zelda marker or a claim about the native initial camera/zoom.
// Uses Compose's shared all-floor room frame, raster rounding/2048 cap, then
// the same aspect-preserving fit and centered integer padding as its resource.
// Coordinates/extents are normalized to the complete padded canvas. The caller
// must authenticate current room/floor membership and visitation before use;
// this geometry helper grants no reveal permission and performs no IO.
DungeonRoomFocus ProjectDungeonRoomFocus(const DungeonMapDefinition &,
                                         const DungeonMapArt &, uint16_t room,
                                         int32_t selected_floor,
                                         uint32_t canvas_width,
                                         uint32_t canvas_height);
struct DungeonRoomVariant {
  uint16_t room{};
  // Exact known result of native condition evaluation. An empty texture selects
  // the ordinary visited variant. Missing state conservatively omits rooms with
  // conditional art; it never assumes their flags are false.
  bool known{};
  std::string texture;
};
enum class DungeonComposeStatus {
  InvalidSource,
  UnsupportedFloor,
  Empty,
  Conservative,
  Ready
};
// Worker only. Original UiMapArea/Line + native dynamic-floor layouts and room
// textures. The authenticated caller provides room visitation and selected
// floor. Unvisited rooms remain hidden unless authenticated map-item ownership
// is supplied; then only original gray _t room art is used.
class DungeonMapAssets {
public:
  explicit DungeonMapAssets(ReadAssetFn);
  ~DungeonMapAssets();
  bool Inspect(const DungeonMapDefinition &, DungeonMapArt &);
  bool Compose(const DungeonMapDefinition &, int32_t floor,
               uint64_t visited_rooms, std::span<const DungeonRoomVariant>,
               Image &, MapBounds &, DungeonComposeStatus *status = nullptr,
               bool map_item = false);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace echoes
