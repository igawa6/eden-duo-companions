// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_reader.h"
#include <array>
#include <string>
#include <vector>
namespace ot0 {
struct MapPoint {
  double x{}, y{};
  bool operator==(const MapPoint &) const = default;
};
struct MapCell {
  int32_t x{}, y{};
  bool operator==(const MapCell &) const = default;
};
struct ExplorationSnapshot {
  bool valid{};
  uint8_t state{};
  uint32_t map_id{}, map_name_id{};
  bool filter_on{true};
  double reveal_stamp_world_size{};
  uint32_t reveal_texture_world_size{};
  MapPoint reveal_world_offset{};
  std::string reveal_mask_path;
  bool heading_valid{};
  uint8_t native_direction{};
  std::string map_key, section_key, texture_path;
  std::array<double, 3> world_xyz{}, bounds_min{}, bounds_max{};
  MapPoint axis{}, offset{}, translation{}, scale{}, texture_position{};
  uint32_t block_width{};
  std::vector<MapCell> revealed_cells;
  bool operator==(const ExplorationSnapshot &) const = default;
};
// Current native section only. No registry/heap scans, guest calls or writes.
ExplorationSnapshot ReadExploration(const ReadMemory &read, uint64_t main,
                                    int64_t relocation_delta = 0);
// Native resource/UI canvas transform. These coordinates are not source
// texture pixels; use NativeTexturePoint when drawing a decoded map asset.
MapPoint WorldToTexture(const ExplorationSnapshot &map, double world_x,
                        double world_y);
// Inverse resource/UI canvas transform; returns world X/Y.
MapPoint TextureToWorld(const ExplorationSnapshot &map, double pixel_x,
                        double pixel_y);
// Native FCVTZS truncation, which equals floor inside the section bounds.
MapCell WorldCell(const ExplorationSnapshot &map, double world_x,
                  double world_y);
// Native filter canvas coordinates before zoom, using the source texture size.
MapPoint NativeTexturePoint(const ExplorationSnapshot &map, double world_x,
                            double world_y, uint32_t texture_width,
                            uint32_t texture_height);
// Strict visited-cell bounds in the resource/UI canvas coordinate system.
std::array<MapPoint, 2> CellTextureBounds(const ExplorationSnapshot &map,
                                          MapCell cell);
} // namespace ot0
