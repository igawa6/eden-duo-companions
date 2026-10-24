// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include <span>
namespace echoes {
struct MapIconCurveKey {
  double frame{}, value{}, slope{};
};
// Original BFLAN cubic-Hermite interpolation, not an authored pulse.
bool EvaluateMapIconCurve(std::span<const MapIconCurveKey>, double frame,
                          double &);
struct MapIconRenderInfo {
  uint32_t native_loop_frames{}, native_tracks{}, omitted_complex_panes{};
  bool original_sources{}, shader_parity{};
};
// Asset-worker owned. Reads original layout/animation/texture bytes from the
// player's RomFS; no extracted game data is compiled or shipped.
class MapIconAssets {
public:
  explicit MapIconAssets(ReadAssetFn);
  ~MapIconAssets();
  MapIconAssets(const MapIconAssets &) = delete;
  MapIconAssets &operator=(const MapIconAssets &) = delete;
  // A deliberately limited native-source subset: original checkpoint circle
  // and glow, with original Loop SRT curves. Four detailed-combiner RGB wave
  // panes are omitted. Glow combines its original scrolling masks by coverage;
  // this is NOT a reproduction of the native detailed-combiner shader.
  // Frame is in original native animation frames; output side40..256. Output
  // is cleared on failure. The caller owns marker visibility/discovery gates.
  bool Checkpoint(double frame, uint32_t side, Image &,
                  MapIconRenderInfo * = nullptr);
  // Native UiMapIconLandMark key2's constructor-selected Smoothie texture,
  // fitted to the original MapIcon_Base picture preserving its aspect.
  bool Smoothie(uint32_t side, Image &);
  // Native dungeon TreasureBox_Base / TreasureBoxOpen_Base resource aliases
  // resolve to IconMap's TreasureBox / TreasureBoxOpen texture names.
  bool Chest(bool open, uint32_t side, Image &);
  // Constructor-selected original IconMap texture, e.g. authenticated City/Shop
  // or SDungeon metadata. This grants no discovery or coordinate permission.
  bool Landmark(std::string_view texture, uint32_t side, Image &);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace echoes
