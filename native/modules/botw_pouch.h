// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_inventory_offsets.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace botw {
struct Item {
  std::string actor;
  int32_t type{}, value{};
  bool equipped{};
  // Native instance identity: duplicate actors are distinct pouch entries.
  uint64_t address{};
};
struct Snapshot {
  bool valid{};
  std::vector<Item> items;
};
template <class T> bool read(const EdenDsmodHostApi &h, uint64_t p, T &out) {
  return p >= 0x10000 && p <= UINT64_MAX - sizeof(T) &&
         h.is_mapped(h.userdata, p, sizeof(T)) &&
         h.read_memory(h.userdata, p, &out, sizeof(T));
}
inline Snapshot pouch(const EdenDsmodHostApi &h) {
  Snapshot out;
  if (h.title_id != 0x01007EF00011E000ULL || h.main_size < (inventory_offsets(h).pouch_root+8) ||
      h.main_base > UINT64_MAX - (inventory_offsets(h).pouch_root+8))
    return out;
  if (!inventory_supported(h))
    return out;
  uint64_t mgr{};
  if (!read(h, h.main_base + inventory_offsets(h).pouch_root, mgr) || mgr % 8 ||
      mgr > UINT64_MAX - 0x80)
    return out;
  const uint64_t head = mgr + 0x68;
  struct List {
    uint64_t next, prev;
    int32_t count, offset;
  } a{}, z{};
  if (!read(h, head, a) || a.count < 0 || a.count > 420 || a.offset != 8)
    return out;
  std::array<uint64_t, 420> seen{};
  uint64_t node = a.next, previous = head;
  for (int32_t i = 0; i < a.count; i++) {
    if (node == head || node < 0x10008 || node % 8)
      return {};
    for (int j = 0; j < i; j++)
      if (seen[j] == node)
        return {};
    seen[i] = node;
    std::array<uint8_t, 0x38> bytes{};
    if (!read(h, node - 8, bytes))
      return {};
    auto u64 = [&](size_t o) {
      uint64_t v;
      std::memcpy(&v, bytes.data() + o, 8);
      return v;
    };
    auto i32 = [&](size_t o) {
      int32_t v;
      std::memcpy(&v, bytes.data() + o, 4);
      return v;
    };
    if (u64(0x10) != previous || i32(0x18) < 0 || i32(0x18) > 9 ||
        bytes[0x24] > 1 || bytes[0x25] > 1)
      return {};
    uint64_t str = u64(0x30);
    std::string name;
    for (unsigned k = 0; k < 64; k++) {
      uint8_t c{};
      if (str > UINT64_MAX - k || !read(h, str + k, c))
        return {};
      if (!c)
        break;
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_'))
        return {};
      name += char(c);
    }
    if (name.empty() || name.size() == 64)
      return {};
    if (bytes[0x25]) out.items.push_back(
        {std::move(name), i32(0x18), i32(0x20), bool(bytes[0x24]), node - 8});
    previous = node;
    node = u64(8);
  }
  if (node != head || previous != a.prev || !read(h, head, z) ||
      std::memcmp(&a, &z, sizeof(a)))
    return {};
  uint64_t again{};
  if (!read(h, h.main_base + inventory_offsets(h).pouch_root, again) || again != mgr)
    return {};
  out.valid = true;
  return out;
}
} // namespace botw
