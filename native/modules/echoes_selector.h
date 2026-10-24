// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_reader.h"

namespace echoes {
inline constexpr size_t MaxSelectorNodes = 128;
struct SelectorSource {
  uint64_t owner{}, records{}, end{}, capacity{};
  uint8_t mode{}, native_state{};
  uint8_t sort_enum{};
  bool sort_valid{};
  bool operator==(const SelectorSource &) const = default;
};
struct SelectorSnapshot {
  bool compatible{}, selector_valid{}, held{}, equipped_valid{};
  uint16_t equipped_native_id{0xffff};
  SelectorSource source{};
  // Only a stable, supported state2/mode1 picker supplies these fields. State0
  // is known closed; unavailable/closed snapshots retain no previous list.
  std::vector<uint16_t> ordered_native_ids;
  int32_t highlighted_index{-1};
  // Native animation/input destination; may differ from the current cursor.
  int32_t target_index{-1};
  uint16_t highlighted_native_id{0xffff};
  // Native sort enum (1/2/4/8/16); independent optional metadata. Unknown or
  // unreadable sort never invents a label or invalidates an otherwise held
  // list.
  uint8_t sort_enum{};
  bool sort_valid{};
  // Optional favorite subset of the current held list. Closed/unknown clears
  // it; ordinary-page favorites use the independently sampled save CRC set.
  bool favorite_valid{};
  std::vector<uint16_t> favorite_native_ids;
};
// Read-only observation of the native DRight picker on the supported build.
// No writes, guest calls, actions or state retained between invocations. The
// caller must intersect IDs with its learned set before presenting ownership.
// Equipped identity has an independent stable chain and can remain available
// when the selector is absent/unsupported. Alternate mode0 is unverified.
SelectorSnapshot ReadSelector(const ReadFn &read, uint64_t main_base,
                              bool build_matches);
} // namespace echoes
