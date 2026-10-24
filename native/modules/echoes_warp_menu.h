// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_context.h"
#include "echoes_warp.h"
namespace echoes {
enum class WarpMapState : uint8_t { Normal = 2, SelectedIcon = 3, ConfirmWarp = 4, WarpExecuting = 5 };
struct NativeWarpIcon {
  uint64_t component{};
  uint32_t ordinal{};
  float x{}, y{};
  bool selectable{}, native_allowed{};
  // Exact native focus getter coordinates from the rendered icon pane;
  // cached x/y above is independently used for companion/world projection.
  float cursor_x{}, cursor_y{};
  bool cursor_known{};
  bool operator==(const NativeWarpIcon &) const = default;
};
struct WarpMenuSnapshot {
  bool compatible{}, valid{}, focus_warp{}, native_allowed{}, selectable{};
  // Exact native observations, not a complete gameplay/input permission gate.
  uint64_t component{}, focused{}, focused_owner{};
  bool targets_known{}, cursor_known{};
  float cursor_x{}, cursor_y{};
  uint64_t native_view{};
  bool zoom_valid{};
  uint8_t zoom_index{},zoom_previous{},zoom_state{},zoom_pending{};
  float zoom_scale{};
  std::vector<float> zoom_scales;
  std::vector<NativeWarpIcon> targets;
  WarpMapState state{WarpMapState::Normal};
  uint8_t secondary_state{};
  uint32_t map_index{}, destination_index{};
  std::string map_level, warp_set, focused_level, unlock_flag;
  bool operator==(const WarpMenuSnapshot &) const = default;
};
// Read-only authenticated MapMenu28/MapIconWarp observations. Original byte
// guards + exact paired vtables + two complete matching reads + final source
// revalidation. No final event/dialog choice or cursor movement is inferred.
// The caller independently joins a current same-save unlock snapshot and
// observes input interference; valid/native_allowed alone never grant travel.
WarpMenuSnapshot ReadWarpMenu(const ReadFn &, uint64_t main_base,
                              bool build_matches, const NativeWarpCatalog &,
                              bool include_targets = false);
} // namespace echoes
