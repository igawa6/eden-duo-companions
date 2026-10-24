// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include "echoes_map.h"
#include <span>
#include <vector>
namespace echoes {
struct NativeEntranceIcon {
  std::string key, texture_name, label_ref;
  uint32_t zoom_level{};
  std::array<float, 2> offset{};
  bool operator==(const NativeEntranceIcon &) const = default;
};
struct NativeFieldEntrance {
  uint32_t actor_index{}, icon_index{};
  std::array<float, 3> world{};
  bool operator==(const NativeFieldEntrance &) const = default;
};
struct NativeEntranceCatalog {
  // Native initializer maps each original row key directly to its row ordinal.
  // This order decodes saved bits even before native map metadata is loaded.
  std::vector<NativeEntranceIcon> icons;
  // Only direct Field destinations absent from pair-table keys. No guessed
  // remapping.
  std::vector<NativeFieldEntrance> field;
  // True only for the worker-loaded original key order; edits invalidate it.
  bool NativeIndexOrderValid() const;
  bool operator==(const NativeEntranceCatalog &) const = default;

private:
  std::vector<std::string> original_key_order_;
  friend bool LoadNativeEntrances(const ReadAssetFn &, NativeEntranceCatalog &);
};
// Asset worker only: Common.eclo + both original entrance sheets. Clears on
// failure.
bool LoadNativeEntrances(const ReadAssetFn &, NativeEntranceCatalog &);
struct EntranceVisibilitySnapshot {
  bool compatible{}, known{}, native_dictionary_validated{};
  MapEpochSource source{};
  // Parallel to catalog.field; positive saved discovery only, never map-item
  // inference.
  std::vector<uint8_t> visible;
};
// Caller independently brackets exact Field/gameplay/map epoch and terrain
// coverage. Original worker-loaded key order + guarded initializer authorize
// passive saved-bit decoding before the native map singleton is populated. An
// available runtime dictionary must independently match. Triple identical
// bounded samples + complete source replay; no guest calls/writes.
EntranceVisibilitySnapshot
ReadEntranceVisibility(const ReadFn &, uint64_t main_base, bool build_matches,
                       const MapEpochSource &, const NativeEntranceCatalog &);
} // namespace echoes
