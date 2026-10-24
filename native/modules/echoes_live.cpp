// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_live.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace echoes {
namespace {
constexpr uint64_t TableOffset = 0x28daad4;
constexpr size_t NativeCount = 1225;
using NativeTable = std::array<uint8_t, NativeCount * 8>;
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
bool Map(const NativeTable& table, const std::vector<uint32_t>& keys,
         std::vector<uint16_t>& ids) {
  std::vector<uint32_t> native_keys;
  native_keys.reserve(NativeCount);
  for (size_t i = 0; i < NativeCount; ++i) {
    const auto* p = table.data() + i * 8;
    // The native table is dense: key, native ID, zero padding.
    if (Le32(p + 4) != i) return false;
    native_keys.push_back(Le32(p));
  }
  auto sorted = native_keys;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return false;
  for (const auto key : keys) {
    const auto at = std::find(native_keys.begin(), native_keys.end(), key);
    if (at == native_keys.end()) return false;
    ids.push_back(static_cast<uint16_t>(at - native_keys.begin()));
  }
  return true;
}
} // namespace
LiveSnapshot ReadLive(const ReadFn& read, uint64_t base, bool build_matches) {
  LiveSnapshot out;
  const auto first = ReadSnapshot(read, base, build_matches);
  out.compatible = first.compatible;
  if (!first.collection_valid || base > std::numeric_limits<uint64_t>::max() -
      TableOffset - sizeof(NativeTable)) return out;
  NativeTable table{}, again{};
  if (!read(base + TableOffset, table.data(), table.size())) return out;
  std::vector<uint16_t> ids;
  if (!Map(table, first.keys, ids)) return out;
  auto selector = ReadSelector(read, base, build_matches);
  const auto final = ReadSnapshot(read, base, build_matches);
  if (!final.collection_valid || first.epoch_source != final.epoch_source ||
      first.keys != final.keys ||
      !read(base + TableOffset, again.data(), again.size()) || table != again)
    return out;
  out.collection_valid = true;
  out.collection_source = first.epoch_source;
  out.learned_native_ids = std::move(ids);
  out.learned_crc_keys = first.keys;
  if (first.favorite_valid && final.favorite_valid &&
      first.favorite_crc_keys == final.favorite_crc_keys) {
    out.favorite_valid = true;
    out.favorite_crc_keys = first.favorite_crc_keys;
  }
  if (selector.held) {
    const bool owned = std::all_of(selector.ordered_native_ids.begin(),
        selector.ordered_native_ids.end(), [&](uint16_t id) {
          return std::find(out.learned_native_ids.begin(),
              out.learned_native_ids.end(), id) != out.learned_native_ids.end();
        });
    if (!owned) selector = {};
  }
  out.selector = std::move(selector);
  return out;
}
} // namespace echoes
