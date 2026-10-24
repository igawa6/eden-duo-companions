// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_markers.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
namespace echoes {
namespace {
struct Blob {
  std::span<const uint8_t> b;
  bool Has(uint64_t p, uint64_t n) const {
    return p <= b.size() && n <= b.size() - p;
  }
  uint16_t U16(size_t p) const {
    return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
  }
  uint32_t U32(size_t p) const { return U16(p) | uint32_t(U16(p + 2)) << 16; }
  float F32(size_t p) const { return std::bit_cast<float>(U32(p)); }
  bool String(uint64_t p, std::string &out) const {
    if (!Has(p, 1))
      return false;
    size_t n = 0;
    while (n < 512 && Has(p + n, 1) && b[p + n])
      ++n;
    if (!n || n == 512 || !Has(p + n, 1))
      return false;
    out.assign(reinterpret_cast<const char *>(b.data() + p), n);
    return std::all_of(out.begin(), out.end(),
                       [](unsigned char c) { return c >= 32 && c < 127; });
  }
};
uint32_t Crc(std::string_view s) {
  uint32_t c = ~0u;
  for (unsigned char b : s) {
    c ^= b;
    for (int j = 0; j < 8; ++j)
      c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
  }
  return ~c;
}
bool Parse(const Blob &b, std::string_view key,
           std::vector<NativeCheckpointGroup> &out) {
  constexpr std::array<uint8_t, 8> magic{0x65, 0x63, 0x6c, 0x6f, 3, 0, 1, 1};
  if (key.empty() || key.size() > 127 || !b.Has(0, 0x8c) ||
      !std::equal(magic.begin(), magic.end(), b.b.begin()) ||
      b.U32(8) != b.b.size() || b.U32(0x84) != 0x65636c64)
    return false;
  const auto count = b.U32(0xc), dir = b.U32(0x10), rows = b.U32(0x18),
             gc = b.U32(0x44), groups = b.U32(0x48), mc = b.U32(0x4c),
             markers = b.U32(0x50), ec = b.U32(0x54), expr = b.U32(0x58),
             sc = b.U32(0x74), si = b.U32(0x78), ss = b.U32(0x80);
  if (!count || count > 4096 || count != b.U32(0x14) || count != b.U32(0x88) ||
      gc > 4096 || mc > 16384 || ec > 65536 || sc > 16384 ||
      sc != b.U32(0x7c) || !b.Has(dir, uint64_t(count) * 8) ||
      !b.Has(rows, uint64_t(count) * 0x120) ||
      !b.Has(groups, uint64_t(gc) * 12) || !b.Has(markers, uint64_t(mc) * 28) ||
      !b.Has(expr, uint64_t(ec) * 8) || !b.Has(si, uint64_t(sc) * 4) ||
      !b.Has(ss, 1))
    return false;
  auto text = [&](uint32_t i, std::string &s) {
    return i < sc && b.String(uint64_t(ss) + b.U32(si + size_t(i) * 4), s);
  };
  size_t selected = 0;
  bool found = false;
  uint32_t prev = 0;
  std::set<uint32_t> indices;
  for (size_t i = 0; i < count; ++i) {
    const auto crc = b.U32(dir + i * 8), idx = b.U32(dir + i * 8 + 4);
    if ((i && crc <= prev) || idx >= count || !indices.insert(idx).second)
      return false;
    prev = crc;
    const size_t row = rows + size_t(idx) * 0x120;
    std::string name;
    if (!text(b.U32(row + 0x100), name) || Crc(name) != crc)
      return false;
    if (name == key) {
      if (found)
        return false;
      found = true;
      selected = row;
    }
  }
  if (!found)
    return false;
  const auto first = b.U32(selected + 0x110);
  const auto n = b.U16(selected + 0x11a);
  if (first > gc || n > gc - first)
    return false;
  std::set<uint32_t> quests;
  for (size_t i = 0; i < n; ++i) {
    const size_t g = groups + (first + i) * 12;
    NativeCheckpointGroup group;
    group.quest_crc = b.U32(g);
    const auto num = b.U32(g + 4), start = b.U32(g + 8);
    if (!group.quest_crc || !quests.insert(group.quest_crc).second ||
        start > mc || num > mc - start)
      return false;
    for (size_t j = 0; j < num; ++j) {
      const size_t p = markers + (start + j) * 28;
      NativeCheckpointMarker marker;
      for (size_t k = 0; k < 3; ++k)
        marker.world[k] = b.F32(p + k * 4);
      for (size_t k = 0; k < 2; ++k)
        marker.auxiliary[k] = b.F32(p + 12 + k * 4);
      if (!std::all_of(marker.world.begin(), marker.world.end(),
                       [](float v) { return std::isfinite(v); }) ||
          !std::all_of(marker.auxiliary.begin(), marker.auxiliary.end(),
                       [](float v) { return std::isfinite(v); }) ||
          b.b[p + 27] > 1)
        return false;
      marker.condition_strategy = b.b[p + 20];
      marker.auxiliary_byte = b.b[p + 26];
      marker.side_view = b.b[p + 27] != 0;
      const auto start_expr = b.U16(p + 22), num_expr = b.U16(p + 24);
      if (start_expr > ec || num_expr > ec - start_expr)
        return false;
      for (size_t k = 0; k < num_expr; ++k) {
        const size_t e = expr + (start_expr + k) * 8;
        NativeCheckpointCondition condition;
        condition.category = b.U32(e);
        if (!text(b.U32(e + 4), condition.parameter))
          return false;
        marker.conditions.push_back(std::move(condition));
      }
      group.markers.push_back(std::move(marker));
    }
    out.push_back(std::move(group));
  }
  return true;
}
} // namespace
bool ParseNativeCheckpoints(std::span<const uint8_t> b, std::string_view key,
                            std::vector<NativeCheckpointGroup> &out) {
  out.clear();
  std::vector<NativeCheckpointGroup> parsed;
  if (!Parse(Blob{b}, key, parsed))
    return false;
  out = std::move(parsed);
  return true;
}
bool LoadNativeCheckpoints(const ReadAssetFn &read, std::string_view key,
                           std::vector<NativeCheckpointGroup> &out) {
  out.clear();
  if (!read)
    return false;
  constexpr auto path = "region_common/level/Common.eclo";
  const auto n = read(path, 0, nullptr, 0);
  if (!n || n > 1024 * 1024)
    return false;
  std::vector<uint8_t> b(n);
  return read(path, 0, b.data(), n) == n && ParseNativeCheckpoints(b, key, out);
}
} // namespace echoes
