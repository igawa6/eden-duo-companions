// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_menu.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
namespace echoes {
namespace {
constexpr size_t MaxNodes = 128, MaxEntries = 128;
uint32_t U32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t U64(const uint8_t *p) {
  return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32;
}
int32_t I32(const uint8_t *p) {
  int32_t v;
  const auto n = U32(p);
  std::memcpy(&v, &n, 4);
  return v;
}
bool At(uint64_t a, uint64_t off, uint64_t &p) {
  if (a > UINT64_MAX - off)
    return false;
  p = a + off;
  return true;
}
bool Bytes(const ReadFn &r, uint64_t p, void *out, size_t n) {
  return p && n <= UINT64_MAX - p && (!n || r(p, out, n));
}
bool Field(const ReadFn &r, uint64_t p, uint64_t off, void *out, size_t n) {
  uint64_t at{};
  return At(p, off, at) && Bytes(r, at, out, n);
}
bool Q(const ReadFn &r, uint64_t p, uint64_t off, uint64_t &value) {
  std::array<uint8_t, 8> b{};
  if (!Field(r, p, off, b.data(), b.size()))
    return false;
  value = U64(b.data());
  return true;
}
struct Node {
  uint64_t address{}, owner{}, vt{}, wrapper{};
  std::array<uint8_t, 24> links{};
  std::array<uint8_t, 3> state{};
  bool operator==(const Node &) const = default;
};
struct Frame {
  std::array<uint8_t, 24> header{};
  std::vector<Node> nodes;
  uint64_t component{};
  MenuKind kind{MenuKind::None};
  std::array<uint8_t, 3> outer{}, nested{};
  std::array<uint8_t, 24> vector{}, row_vector{};
  std::array<uint8_t, 16> cursors{};
  std::vector<uint8_t> records, rows;
  bool operator==(const Frame &) const = default;
};
bool Vector(const std::array<uint8_t, 24> &h, size_t stride, size_t max,
            uint64_t &begin, size_t &count) {
  begin = U64(h.data());
  const auto end = U64(h.data() + 8), cap = U64(h.data() + 16);
  if (!begin || begin % 8 || end < begin || cap < end ||
      (end - begin) % stride || (cap - begin) % stride ||
      (cap - begin) / stride > max)
    return false;
  count = size_t((end - begin) / stride);
  return count && count <= max;
}
bool Collect(const ReadFn &r, uint64_t base, Frame &f) {
  uint64_t sentinel{};
  if (!At(base, 0x34bf070, sentinel) ||
      !Bytes(r, sentinel, f.header.data(), f.header.size()))
    return false;
  uint64_t node = U64(f.header.data() + 8), previous = sentinel;
  while (node != sentinel) {
    if (!node || node % 8 || f.nodes.size() >= MaxNodes ||
        std::any_of(f.nodes.begin(), f.nodes.end(),
                    [&](const auto &n) { return n.address == node; }))
      return false;
    Node n;
    n.address = node;
    if (!Bytes(r, node, n.links.data(), n.links.size()) ||
        U64(n.links.data()) != previous)
      return false;
    n.owner = U64(n.links.data() + 16);
    if (!n.owner || n.owner % 8 || !Q(r, n.owner, 0, n.vt))
      return false;
    MenuKind kind = MenuKind::None;
    uint64_t wrapper{};
    if (n.vt == base + 0x3413d58) {
      kind = MenuKind::Equipment;
      wrapper = base + 0x3428190;
    }
    if (n.vt == base + 0x3415588) {
      kind = MenuKind::Items;
      wrapper = base + 0x3427310;
    }
    if (kind != MenuKind::None) {
      if (n.owner < 0x20 || !Q(r, n.owner - 0x20, 0, n.wrapper) ||
          n.wrapper != wrapper ||
          !Field(r, n.owner, 0x88, n.state.data(), n.state.size()))
        return false;
      if (n.state[0] != 0 && n.state[0] != 2)
        return false;
      if (n.state[0] == 2) {
        if (f.component || n.state[2] != 0xfe)
          return false;
        f.component = n.owner;
        f.kind = kind;
        f.outer = n.state;
      }
    }
    f.nodes.push_back(n);
    previous = node;
    node = U64(n.links.data() + 8);
  }
  if (U64(f.header.data()) != previous || !f.component ||
      !Field(r, f.component, 0x208, f.nested.data(), f.nested.size()))
    return false;
  if (f.kind == MenuKind::Equipment) {
    if (!Field(r, f.component, 0xa578, f.vector.data(), f.vector.size()) ||
        !Field(r, f.component, 0xa5a8, f.row_vector.data(),
               f.row_vector.size()) ||
        !Field(r, f.component, 0xa5c8, f.cursors.data(), 12) ||
        !Field(r, f.component, 0x2f0, f.cursors.data() + 12, 4))
      return false;
    uint64_t entries{}, rows{};
    size_t count{}, row_count{};
    if (!Vector(f.vector, 0x28, MaxEntries, entries, count) ||
        !Vector(f.row_vector, 4, MaxEntries / 4, rows, row_count))
      return false;
    const auto row = I32(f.cursors.data() + 4), col = I32(f.cursors.data() + 8);
    if (row < 0 || size_t(row) >= row_count || col < 0 || col >= 4)
      return false;
    f.records.resize(count * 0x28);
    f.rows.resize(row_count * 4);
    if (!Bytes(r, entries, f.records.data(), f.records.size()) ||
        !Bytes(r, rows, f.rows.data(), f.rows.size()))
      return false;
    for (size_t i = 0; i < row_count; ++i)
      if (U32(f.rows.data() + i * 4) >= ((count + 3) / 4))
        return false;
    const uint64_t selected =
        uint64_t(U32(f.rows.data() + size_t(row) * 4)) * 4 + uint32_t(col);
    return selected < count;
  }
  if (!Field(r, f.component, 0x53a0, f.vector.data(), f.vector.size()) ||
      !Field(r, f.component, 0x5390, f.cursors.data(), 4))
    return false;
  uint64_t entries{};
  size_t count{};
  if (!Vector(f.vector, 0x78, MaxEntries, entries, count))
    return false;
  const auto selected = I32(f.cursors.data());
  if (selected < 0 || size_t(selected) >= count)
    return false;
  f.records.resize(count * 0x78);
  return Bytes(r, entries, f.records.data(), f.records.size());
}
bool Decode(const Frame &f, MenuSnapshot &out) {
  out.kind = f.kind;
  out.component = f.component;
  out.outer_state = f.outer[0];
  out.nested_state = f.nested[0];
  if (f.kind == MenuKind::Equipment) {
    out.row = I32(f.cursors.data() + 4);
    out.column = I32(f.cursors.data() + 8);
    out.visible_cursor = I32(f.cursors.data() + 12);
    for (size_t i = 0; i < f.rows.size(); i += 4)
      out.row_starts.push_back(U32(f.rows.data() + i));
    out.selected_index =
        int32_t(out.row_starts[size_t(out.row)] * 4 + uint32_t(out.column));
    for (size_t i = 0; i < f.records.size(); i += 0x28) {
      const auto *p = f.records.data() + i;
      MenuEntry e;
      if (p[0x1c] > 1 || p[0x1e] > 1 || p[0x1f] > 1 || p[0x24] > 1 ||
          p[0x25] > 1)
        return false;
      e.owned = p[0x1e];
      e.equipped = p[0x1f];
      e.placeholder = p[0x24] || p[0x25];
      if (e.placeholder) {
        // Native row/control slots reuse the same default outfit-mask bytes.
        // They are never owned gear or a target, even when byte1e is set.
        e.owned = e.equipped = false;
        out.entries.push_back(e);
        continue;
      }
      if (e.owned) {
        e.kind = p[0x1c] ? MenuEntryKind::Outfit : MenuEntryKind::Accessory;
        e.mask = p[0x1c] ? (uint32_t(p[0]) | uint32_t(p[1]) << 8) : U32(p + 4);
        if (!std::has_single_bit(e.mask) ||
            (p[0x1c] ? (e.mask & ~0x7ffu) : (e.mask & ~0xfffffffu)))
          return false;
      } else if (e.equipped)
        return false;
      out.entries.push_back(e);
    }
  } else {
    out.selected_index = I32(f.cursors.data());
    out.visible_cursor = out.selected_index;
    for (size_t i = 0; i < f.records.size(); i += 0x78) {
      const auto *p = f.records.data() + i;
      MenuEntry e;
      e.kind = MenuEntryKind::Item;
      e.item_id = U32(p + 0x20);
      e.quantity = U32(p + 0x24);
      e.raw_action28 = p[0x28];
      e.raw_action29 = p[0x29];
      e.raw_action2a = p[0x2a];
      // Original descriptor construction at251f89c..251f8b8 initializes
      // quantity to-1; uncounted quest entries retain it. Keep that sentinel
      // outside the ingredient grid without exposing it as consumable stock.
      const bool uncounted = i / 0x78 >= 16 && e.quantity == UINT32_MAX &&
                             e.raw_action28 == 1 && e.raw_action29 == 0 &&
                             e.raw_action2a == 0;
      if (e.item_id > 65535 || (e.quantity > 9999 && !uncounted))
        return false;
      e.owned = !uncounted && e.quantity > 0;
      out.entries.push_back(e);
    }
  }
  return true;
}
} // namespace
MenuSnapshot ReadMenu(const ReadFn &r, uint64_t base, bool match) {
  MenuSnapshot out;
  if (!r || !match || !base || base % 0x1000)
    return out;
  try {
    for (const auto &g : MenuIdentityGuards) {
      uint64_t at{};
      std::array<uint8_t, 256> b{};
      if (!At(base, g.offset, at) || !Bytes(r, at, b.data(), b.size()) ||
          !MatchSupportedGuard(b, g.bytes, g.offset))
        return out;
    }
    const auto before = ReadContext(r, base, match);
    out.compatible = before.compatible;
    if (!before.valid || before.context != NativeContext::NativeMenu)
      return out;
    Frame first, second, final;
    if (!Collect(r, base, first) || !Collect(r, base, second) ||
        first != second || !Collect(r, base, final) || first != final)
      return out;
    const auto after = ReadContext(r, base, match);
    if (!after.valid || after.context != NativeContext::NativeMenu ||
        before.registry_count != after.registry_count)
      return out;
    MenuSnapshot decoded;
    decoded.compatible = true;
    if (!Decode(first, decoded))
      return out;
    decoded.valid = true;
    return decoded;
  } catch (...) {
    return out;
  }
}
} // namespace echoes
