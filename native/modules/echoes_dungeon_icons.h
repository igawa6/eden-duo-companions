// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_dungeon.h"
#include "echoes_dungeon_assets.h"
#include <optional>
namespace echoes {
enum class DungeonIconSprite : uint8_t { Unknown, TreasureBox, TreasureBoxOpen };
struct NativeDungeonIcon {
 uint64_t component{}, pane{};
 uint32_t role{}; // Original UiMapIcon key: 0 actor/door or 8 treasure.
 float pane_x{}, pane_y{};
 bool predicate_visible{}, pane_visible{}, animated{};
 // Native pane coordinates include native floor translation.
 // projection_known requires the exact original owning room/floor-root lookup.
 bool projection_known{};
 uint16_t room{};
 bool floor_known{};
 int32_t floor{};
 float native_x{},native_y{},room_x{},room_y{};
 DungeonIconSprite sprite{DungeonIconSprite::Unknown};
 bool operator==(const NativeDungeonIcon&) const = default;
};
struct DungeonIconsSnapshot {
 bool compatible{}, valid{};
 bool native_floor_known{};
 int32_t native_floor{};
 uint64_t menu{}, view{};
 std::string key;
 MapEpochSource source{};
 std::vector<NativeDungeonIcon> icons;
 bool operator==(const DungeonIconsSnapshot&) const = default;
};
// Fresh read-only native-open MapMenu observer. No caching across map closure.
// A valid empty list is supported; invalid/closed must immediately clear icons.
// This exposes actual native actor predicate and pane state, not a chest-open
// flag. The caller must use DungeonIconVisible with same-source progress and
// selected floor, then ProjectDungeonIcon with matching Compose bounds.
bool DungeonIconVisible(const NativeDungeonIcon&,const DungeonIconsSnapshot&,
 const DungeonProgressSnapshot&,const DungeonMapDefinition&,
 const DungeonMapArt&,int32_t selected_floor);
// Converts an authenticated native root point using Compose's all-floor frame,
// capped raster rounding and aspect-preserving centered canvas padding.
// The caller supplies the exact matching Compose bounds and canvas size.
std::optional<std::array<float,2>> ProjectDungeonIcon(const NativeDungeonIcon&,
 const MapBounds&,uint32_t canvas_width,uint32_t canvas_height);
DungeonIconsSnapshot ReadDungeonIcons(const ReadFn&,uint64_t main_base,
 bool build_matches,const MapSnapshot&,const DungeonMapDefinition&);
}
