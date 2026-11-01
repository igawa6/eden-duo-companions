// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_hud.h"
#include <array>
#include <string>
namespace ot0 {
struct ExplorationSnapshot;
struct MinimapIcon {
  double world_x{}, world_y{};
  // Native atlas-frame asset paths, NOT standalone texture paths. The
  // package compositor must resolve their proven source rectangles.
  std::string frame_path, background_frame_path;
  std::array<float, 4> color{}, background_color{};
  float opacity{};
  bool quest{}; // Native quest-marker class/layer; suitable for subtle pulse.
  bool reveal_checked{}; // passed the native fog/reveal policy (DirectRevealed)
  bool operator==(const MinimapIcon &) const = default;
};
struct MinimapIcons {
  bool valid{}, complete{};
  uint32_t map_id{};
  // Current-resource lease identity for the game-thread UI helper.
  uint64_t native_resource{}, native_section{};
  std::array<float, 4> player_color{};
  std::vector<MinimapIcon> icons;
  bool operator==(const MinimapIcons &) const = default;
};
// Bounded current-resource native visible widget and FULL-map eligible direct
// icon reads. Native fog/instance-hidden rules remain authoritative. Unknown
// classes or unresolved native layers leave complete=false. Even valid
// partial results NEVER authorize hiding the native minimap. The final bool
// is reserved for a native helper that has proved it owns this collapsed root;
// ordinary hidden/native-menu roots must keep the default false.
MinimapIcons ReadMinimapIcons(const ReadMemory &, uint64_t main,
                              int64_t delta = 0,
                              const ExplorationSnapshot *exploration = nullptr,
                              bool allow_owned_hidden_root = false);
// MenuUIManager::GetList(key=10), native MiniMapUI and its cached
// SObjectWidget. This proves root identity for marker reads only. Its opacity
// does not hide the full rendered minimap; it is not a supported hide target.
std::optional<HudRow> FindMinimapHud(const ReadMemory &, uint64_t main,
                                     int64_t delta = 0,
                                     bool allow_owned_hidden_root = false);
// Exact source-owned small-map canvas (all retainer/frame/exit layers).
// The bool accepts an already-owned collapsed canvas; the outer UI stays
// visible so its Blueprint Tick can continue. This function never writes.
std::optional<HudRow> FindMinimapCanvas(const ReadMemory &, uint64_t main,
                                      int64_t delta = 0,
                                      bool allow_owned_hidden_canvas = false);
class MinimapHud {
public:
  // Compatibility interface: native hiding is disabled after GPU verification
  // disproved the old opacity target. Updates/restores never write memory,
  // including when the requested hide and icon-parity gates are both true.
  void Update(const ReadMemory &, const WriteF32 &, uint64_t main, bool hide,
              bool icon_parity_ready, int64_t delta = 0);
  void Restore(const ReadMemory &, const WriteF32 &, uint64_t main,
               int64_t delta = 0);
  bool Hidden() const { return false; }
  int Failures() const { return 0; }
};
} // namespace ot0
