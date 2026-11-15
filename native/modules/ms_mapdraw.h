// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary map picture: the minimap rooms around the current room, drawn the way the
// native map menu draws them (code-drawn, no tile sprites): every explored cell filled with its
// area's MinimapColor at half brightness, a 1-cell-unit outline in the full colour around each
// room, the current room in cream. Unexplored cells are not drawn (the native fog).
#pragma once

#include "ms_model.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace ms {

struct Picture {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};

/// Where map cells land in a DrawMap picture.
struct MapView {
    float cx{}, cy{};        ///< map cell at the picture centre
    std::uint32_t cell{};    ///< pixels per map cell (0 = nothing to draw)
    std::uint32_t width{}, height{};
    int X(float x) const;    ///< picture x of map x
    int Y(float y) const;    ///< picture y of map y (map y grows north)
};

/// Rooms centred on the current room. `cell` is the maximum pixel size of one map cell.
/// `fit_explored` centres the explored bounds and reduces the integer cell size to fit them.
/// Pan offsets move the view centre in map cells (positive y is north).
MapView ViewOf(const MapInfo& map, std::uint32_t width, std::uint32_t height, std::uint32_t cell,
               bool fit_explored = false, int pan_x = 0, int pan_y = 0);
/// The explored rooms in the game's own minimap colours (MapArea.MinimapColor frame, half of it
/// inside); the current room is marked by the separate player dot, as in the game.
Picture DrawMap(const MapInfo& map, std::uint32_t width, std::uint32_t height, std::uint32_t cell,
                bool fit_explored = false, int pan_x = 0, int pan_y = 0);

/// Native MapMenu symbols at explored cells only, each inside its own cell like the native 8 px
/// tile: the authored MapIcon / Place Marker sprite at native size centred in the tile (scaled
/// with the map: cell / 8 picture px per native px), the teleporter as a still frame of its cyan
/// sparkle (unless `teleporters` is false: the Map page animates it over the picture); custom
/// markers replace authored symbols. The loader returns unscaled game sprites.
void DrawMapIcons(Picture& picture, const MapInfo& map, const MapView& view,
                  const std::function<std::optional<Picture>(std::string_view)>& load, bool teleporters = true);

/// The cells (map cell coordinates, centre) of the explored teleporters the native map shows.
std::vector<std::pair<float, float>> TeleporterCells(const MapInfo& map);
/// The same into `out` (cleared first; its capacity is reused).
void TeleporterCells(const MapInfo& map, std::vector<std::pair<float, float>>& out);

/// Where one teleporter sits in its own area, for telling two crystals of one area apart: the
/// area's explored rooms fitted into the picture in the area's MinimapColor at half brightness,
/// the crystal's room in the full colour and the native cyan teleporter mark (map/teleporter's
/// tint, at least 2 cells) at its TeleporterPos, on the native map's ground. Only the ground when
/// the room is not known.
Picture DrawTeleporterArea(const MapInfo& map, std::string_view scene, std::uint32_t width, std::uint32_t height);

/// The native player dot ping (MinimapView / MapMenu PlayerDot): a 1 s cycle in which the 4x4
/// dot scales 4x -> 1x -> 4x while its alpha goes 0.25 -> 0.75 -> 0.25. `phase` 0..steps-1;
/// `cell_px` the map cell size it is drawn at. A square picture of `box` px.
Picture DrawPlayerDot(int phase, int steps, std::uint32_t cell_px, std::uint32_t box);

/// A short stable signature of what DrawMap would draw (for the picture key).
std::uint64_t MapSignature(const MapInfo& map);

/// Native "Total Completion" in tenths of a percent: the figure the native map menu rendered
/// (MapMenu.AreaPercentText); -1 when the menu has not been opened yet.
int TotalCompletion10(const MapInfo& map);

} // namespace ms
