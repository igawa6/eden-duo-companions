// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_map_assets.h"
namespace echoes {
struct NativeMapLabel {
  std::string part_name, label_ref, name;
  std::vector<uint16_t> traversal_indices;
  double u{}, v{}; // authored text-pane centre in native terrain UV, y down
  bool operator==(const NativeMapLabel&) const = default;
};
// Worker-only original UiMapLocationInfo + Field.bflyt T_text_00 overrides
// + L_MapLocationName transforms + exact localized MSBT references. No nearest
// name lookup or bundled label/position table. Geometry must come from Inspect.
// All output clears on failure. At most26 labels from supported original layout.
bool LoadNativeMapLabels(const ReadAssetFn&, const FieldMapGeometry&,
                         std::string_view message_root,
                         std::vector<NativeMapLabel>&);
// Conservative ALL-listed traversal policy. Native ANY-vs-ALL behavior has not
// been qualified. Caller additionally gates current scope and terrain coverage;
// metadata alone never establishes visibility. Malformed indices return false.
bool NativeMapLabelDiscovered(const NativeMapLabel&,
                              std::span<const uint8_t,64> discovered);
} // namespace echoes
