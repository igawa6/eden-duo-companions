// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_selector.h"
#include "echoes_automatons.h"

namespace echoes {
struct LiveSnapshot {
  bool compatible{}, collection_valid{};
  EpochSource collection_source{};
  std::vector<uint16_t> learned_native_ids;
  // Parallel to learned_native_ids, for joining the player's native datasheet
  // metadata. Neither vector is the native selector's display order.
  std::vector<uint32_t> learned_crc_keys;
  // Optional persisted favorite set, stable across this complete joined read.
  bool favorite_valid{};
  std::vector<uint32_t> favorite_crc_keys;
  SelectorSnapshot selector;
  AutomatonSnapshot automatons;
};
// Joins native key/ID mapping and selector only across stable collection reads.
// No cached list is reused on unavailable reads or another save's ownership.
LiveSnapshot ReadLive(const ReadFn& read, uint64_t base, bool build_matches);
} // namespace echoes
