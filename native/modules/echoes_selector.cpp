// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_selector.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace echoes {
namespace {
constexpr size_t RecordStride = 0x38;
constexpr uint16_t NativeIdCount = 1225;
bool Add(uint64_t a, uint64_t b, uint64_t& out) {
  if (a > std::numeric_limits<uint64_t>::max() - b) return false;
  out = a + b;
  return true;
}
bool Bytes(const ReadFn& read, uint64_t at, void* out, size_t n) {
  return at && n <= std::numeric_limits<uint64_t>::max() - at &&
         (n == 0 || read(at, out, n));
}
bool Field(const ReadFn& read, uint64_t base, uint64_t off,
           void* out, size_t n) {
  uint64_t at{};
  return Add(base, off, at) && Bytes(read, at, out, n);
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32;
}
int32_t Signed32(const uint8_t* p) {
  const auto v = Le32(p);
  return v <= uint32_t(std::numeric_limits<int32_t>::max())
      ? static_cast<int32_t>(v)
      : static_cast<int32_t>(int64_t(v) - (INT64_C(1) << 32));
}
bool U64(const ReadFn& read, uint64_t base, uint64_t off, uint64_t& out) {
  uint64_t at{};
  std::array<uint8_t, 8> b{};
  if (!Add(base, off, at) || at % 8 || !Bytes(read, at, b.data(), b.size()))
    return false;
  out = Le64(b.data());
  return true;
}
bool Object(const ReadFn& read, uint64_t base, uint64_t off, uint64_t& out) {
  return U64(read, base, off, out) && out && out % 8 == 0;
}
struct Equipped {
  uint64_t access{}, state{};
  uint16_t id{};
  bool operator==(const Equipped&) const = default;
};
bool ReadEquipped(const ReadFn& read, uint64_t base, Equipped& out) {
  std::array<uint8_t, 2> b{};
  if (!Object(read, base, 0x34bc840, out.access) ||
      !Object(read, out.access, 8, out.state) ||
      !Field(read, out.state, 0x1970, b.data(), b.size())) return false;
  out.id = uint16_t(uint16_t(b[0]) | uint16_t(uint16_t(b[1]) << 8));
  uint64_t access{}, state{};
  return out.id < NativeIdCount &&
         Object(read, base, 0x34bc840, access) && access == out.access &&
         Object(read, access, 8, state) && state == out.state;
}
struct Sort {
  bool observed{};
  uint8_t index{}, value{};
  uint64_t slot{}, block{};
  std::array<uint8_t, 40> header{};
  bool operator==(const Sort &) const = default;
};
Sort ReadSort(const ReadFn &read, uint64_t owner) {
  Sort s;
  if (!owner || !Field(read, owner, 0x2dec, &s.index, 1) ||
      !Field(read, owner, 0x2df8, s.header.data(), s.header.size()))
    return {};
  const auto map = Le64(s.header.data());
  const auto end = Le64(s.header.data() + 8);
  const auto start = Le64(s.header.data() + 24);
  const auto count = Le64(s.header.data() + 32);
  // Native deque elements are bytes in 4096-byte blocks. Bound its map to
  // eight pointers and its populated sort choices to the six native cases.
  if (!map || map % 8 || end % 8 || end <= map || end - map > 64 || !count ||
      count > 6 || s.index >= count)
    return {};
  const auto blocks = (end - map) / 8;
  uint64_t stop{}, pos{}, byte_at{};
  if (!Add(start, count, stop) || stop > blocks * 4096 ||
      !Add(start, s.index, pos) || !Add(map, (pos / 4096) * 8, s.slot) ||
      !Object(read, s.slot, 0, s.block) || !Add(s.block, pos % 4096, byte_at) ||
      !Bytes(read, byte_at, &s.value, 1))
    return {};
  s.observed = true;
  return s;
}
bool KnownSort(uint8_t value) {
  return value == 1 || value == 2 || value == 4 || value == 8 || value == 16;
}
struct Node {
  uint64_t address{}, owner{}, vtable{};
  std::array<uint8_t, 24> links{};
  bool operator==(const Node&) const = default;
};
struct Frame {
  std::array<uint8_t, 24> registry_header{};
  std::vector<Node> nodes;
  uint64_t owner{}, wrapper_vtable{}, owner_vtable{}, action_mask{}, list_vtable{};
  std::array<uint8_t, 3> state{};
  uint8_t mode{};
  std::array<uint8_t, 24> vector_header{};
  std::array<uint8_t, 8> cursors{};
  std::vector<uint8_t> records;
  bool operator==(const Frame&) const = default;
};
bool ReadFrame(const ReadFn& read, uint64_t base, Frame& out, bool payload) {
  uint64_t sentinel{}, owner_vtable{}, wrapper_vtable{}, list_vtable{};
  if (!Add(base, 0x34bf070, sentinel) || sentinel % 8 ||
      !Add(base, 0x34119c8, owner_vtable) ||
      !Add(base, 0x34266d0, wrapper_vtable) ||
      !Add(base, 0x342c260, list_vtable) ||
      !Bytes(read, sentinel, out.registry_header.data(), out.registry_header.size()))
    return false;
  uint64_t node = Le64(out.registry_header.data() + 8), previous = sentinel;
  while (node != sentinel) {
    if (!node || node % 8 || out.nodes.size() >= MaxSelectorNodes ||
        std::any_of(out.nodes.begin(), out.nodes.end(),
                    [node](const Node& n) { return n.address == node; })) return false;
    Node n;
    n.address = node;
    if (!Bytes(read, node, n.links.data(), n.links.size()) ||
        Le64(n.links.data()) != previous) return false;
    n.owner = Le64(n.links.data() + 16);
    if (!n.owner || n.owner % 8 || !U64(read, n.owner, 0, n.vtable)) return false;
    if (n.vtable == owner_vtable) {
      if (out.owner) return false; // Ambiguous native instance ownership.
      out.owner = n.owner;
    }
    out.nodes.push_back(n);
    previous = node;
    node = Le64(n.links.data() + 8);
  }
  if (Le64(out.registry_header.data()) != previous || !out.owner ||
      out.owner < 0x20 ||
      !U64(read, out.owner - 0x20, 0, out.wrapper_vtable) ||
      out.wrapper_vtable != wrapper_vtable ||
      !U64(read, out.owner, 0, out.owner_vtable) ||
      out.owner_vtable != owner_vtable ||
      !Field(read, out.owner, 0x88, out.state.data(), out.state.size()) ||
      !U64(read, out.owner, 0x7f50, out.action_mask) || out.action_mask != 0x1000 ||
      !Field(read, out.owner, 0x7faa, &out.mode, 1) || out.mode != 1)
    return false;
  if (out.state[0] == 0) return true; // Closed: do not read/publish cached list.
  if (out.state[0] != 2) return false; // Other native states lack live coverage.
  uint64_t list{};
  if (!Add(out.owner, 0x150, list) || !U64(read, list, 0, out.list_vtable) ||
      out.list_vtable != list_vtable ||
      !Field(read, list, 0x2c48, out.vector_header.data(), out.vector_header.size()) ||
      !Field(read, list, 0x2c78, out.cursors.data(), out.cursors.size())) return false;
  const uint64_t begin = Le64(out.vector_header.data());
  const uint64_t end = Le64(out.vector_header.data() + 8);
  const uint64_t capacity = Le64(out.vector_header.data() + 16);
  if (!begin || begin % 8 || end % 8 || capacity % 8 || end < begin ||
      capacity < end || (end - begin) % RecordStride ||
      (capacity - begin) % RecordStride) return false;
  const uint64_t count = (end - begin) / RecordStride;
  const int32_t cursor = Signed32(out.cursors.data());
  const int32_t target = Signed32(out.cursors.data() + 4);
  if (!count || count > MaxLearnedKeys ||
      (capacity - begin) / RecordStride > MaxLearnedKeys ||
      cursor < 0 || uint64_t(cursor) >= count || target < 0 || uint64_t(target) >= count)
    return false;
  if (payload) {
    out.records.resize(static_cast<size_t>(count) * RecordStride);
    if (!Bytes(read, begin, out.records.data(), out.records.size())) return false;
    std::vector<uint16_t> ids;
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
      const auto* b = out.records.data() + i * RecordStride;
      const uint16_t id = uint16_t(uint16_t(b[0]) | uint16_t(uint16_t(b[1]) << 8));
      if (id >= NativeIdCount) return false;
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return false;
  }
  return true;
}
} // namespace

SelectorSnapshot ReadSelector(const ReadFn& read, uint64_t base, bool build_matches) {
  SelectorSnapshot out;
  if (!read || !build_matches || !base || base % 8) return out;
  for (const auto& guard : IdentityGuards) {
    uint64_t at{};
    std::array<uint8_t, 256> b{};
    if (!Add(base, guard.offset, at) || !Bytes(read, at, b.data(), b.size()) ||
        !MatchSupportedGuard(b, guard.bytes, guard.offset)) return out;
  }
  out.compatible = true;
  Equipped a, b, final_equipped;
  if (ReadEquipped(read, base, a) && ReadEquipped(read, base, b) && a == b &&
      ReadEquipped(read, base, final_equipped) && a == final_equipped) {
    out.equipped_valid = true;
    out.equipped_native_id = a.id;
  }
  Frame first, second, anchors;
  Sort sort_first, sort_second, sort_final;
  bool selector_ok = ReadFrame(read, base, first, true);
  if (selector_ok && first.state[0] == 2)
    sort_first = ReadSort(read, first.owner);
  selector_ok =
      selector_ok && ReadFrame(read, base, second, true) && first == second;
  if (selector_ok && second.state[0] == 2) {
    sort_second = ReadSort(read, second.owner);
    sort_final = ReadSort(read, second.owner);
  }
  selector_ok = selector_ok && ReadFrame(read, base, anchors, false);
  if (selector_ok) {
    auto metadata = first;
    metadata.records.clear();
    selector_ok = metadata == anchors;
  }
  // Revalidate equipped ownership after the selector reads as well. A player
  // switch can leave the old player memory readable while its picker is copied.
  if (out.equipped_valid && (!ReadEquipped(read, base, final_equipped) ||
                            a != final_equipped)) {
    out.equipped_valid = false;
    out.equipped_native_id = 0xffff;
  }
  if (!selector_ok) return out;
  out.selector_valid = true;
  out.source.owner = first.owner;
  out.source.mode = first.mode;
  out.source.native_state = first.state[0];
  if (first.state[0] == 0) return out;
  out.held = true;
  if (sort_first.observed && sort_first == sort_second &&
      sort_first == sort_final) {
    out.sort_enum = sort_first.value;
    out.sort_valid = KnownSort(out.sort_enum);
    out.source.sort_enum = out.sort_enum;
    out.source.sort_valid = out.sort_valid;
  }
  out.source.records = Le64(first.vector_header.data());
  out.source.end = Le64(first.vector_header.data() + 8);
  out.source.capacity = Le64(first.vector_header.data() + 16);
  for (size_t i = 0; i < first.records.size(); i += RecordStride)
    out.ordered_native_ids.push_back(uint16_t(uint16_t(first.records[i]) |
                              uint16_t(uint16_t(first.records[i + 1]) << 8)));
  // Original X-toggle input updates the favorite byte at record+0x30. The
  // already compared complete records authenticate it without another owner
  // guess. This remains optional until its independent instruction guards fit.
  bool favorites = true;
  for (const auto& guard : FavoriteIdentityGuards) {
    uint64_t at{};
    std::array<uint8_t, 256> observed{};
    if (!Add(base, guard.offset, at) ||
        !Bytes(read, at, observed.data(), observed.size()) || !MatchSupportedGuard(observed, guard.bytes, guard.offset)) {
      favorites = false;
      break;
    }
  }
  for (size_t i = 0; favorites && i < first.records.size(); i += RecordStride) {
    const auto flag = first.records[i + 0x30];
    if (flag > 1) {
      favorites = false;
      break;
    }
    if (flag)
      out.favorite_native_ids.push_back(out.ordered_native_ids[i / RecordStride]);
  }
  out.favorite_valid = favorites;
  if (!favorites)
    out.favorite_native_ids.clear();
  out.highlighted_index = Signed32(first.cursors.data());
  out.target_index = Signed32(first.cursors.data() + 4);
  out.highlighted_native_id = out.ordered_native_ids[static_cast<size_t>(out.highlighted_index)];
  return out;
}
} // namespace echoes
