// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_catalog.h"
#include "echoes_map_annotations.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>
namespace echoes {
namespace {
struct Guard {
  uint64_t offset;
  std::array<uint8_t, 32> bytes;
};
constexpr std::array<Guard, 4> Guards{{
    {0x163b1e0,
     {0xff, 0x83, 0x0,  0xd1, 0xfd, 0x7b, 0x1,  0xa9, 0xfd, 0x43, 0x0,
      0x91, 0x68, 0xf3, 0x0,  0xd0, 0x8,  0x21, 0x3,  0x91, 0x9,  0xfd,
      0xdf, 0x8,  0xe9, 0x9,  0x0,  0x36, 0x69, 0xf3, 0x0,  0xd0}},
    {0x177b1d4,
     {0xab, 0x12, 0x40, 0xf9, 0x8,  0x59, 0x40, 0x91, 0x14, 0x21, 0x30,
      0x91, 0xeb, 0x0,  0x0,  0xb4, 0xa8, 0xe,  0x40, 0xf9, 0x7f, 0x5,
      0x0,  0xf1, 0x1,  0x1,  0x0,  0x54, 0xea, 0x3,  0x1f, 0xaa}},
    {0x177b3a8,
     {0x8,  0x1,  0x9,  0x8b, 0x9,  0x8,  0x0, 0x12, 0x8,  0x21, 0x40,
      0x39, 0x8,  0x25, 0xc9, 0x1a, 0x1f, 0x1, 0x0,  0x72, 0xf4, 0x17,
      0x9f, 0x1a, 0x96, 0x0,  0x0,  0xb5, 0xc, 0x0,  0x0,  0x14}},
    {0x177b42c,
     {0xe0, 0x2,  0x14, 0xa,  0xf4, 0x4f, 0x44, 0xa9, 0xf6, 0x57, 0x43,
      0xa9, 0xf8, 0x5f, 0x42, 0xa9, 0xfd, 0x7b, 0x41, 0xa9, 0xff, 0x43,
      0x1,  0x91, 0xc0, 0x3,  0x5f, 0xd6, 0x9e, 0xe2, 0x42, 0x94}},
}};
constexpr std::array<Guard, 3> LocationGuards{{
    {0x16833b0,
     {0xe9, 0x23, 0xb9, 0x6d, 0xfd, 0x7b, 0x1,  0xa9, 0xfd, 0x43, 0x0,
      0x91, 0xfc, 0x6f, 0x2,  0xa9, 0xfa, 0x67, 0x3,  0xa9, 0xf8, 0x5f,
      0x4,  0xa9, 0xf6, 0x57, 0x5,  0xa9, 0xf4, 0x4f, 0x6,  0xa9}},
    {0x1683610,
     {0xf3, 0x3,  0x15, 0xaa, 0x69, 0xe,  0x4f, 0xf8, 0x76, 0xc2, 0x1,
      0xd1, 0x29, 0x71, 0x44, 0x39, 0x3f, 0x1,  0x0,  0x71, 0xe9, 0x7,
      0x9f, 0x1a, 0x9,  0x25, 0x7,  0x39, 0xe8, 0x22, 0x0,  0x91}},
    {0x1683710,
     {0xa1, 0x62, 0x40, 0xf9, 0xe0, 0x3,  0x16, 0xaa, 0x8,  0x83, 0x38,
      0x39, 0x3d, 0x1,  0x3f, 0x94, 0xc9, 0xf1, 0x0,  0x90, 0xa8, 0x62,
      0x40, 0xf9, 0x2,  0xd,  0x80, 0x52, 0x29, 0xd,  0x45, 0xf9}},
}};
constexpr std::array<Guard, 4> FieldLocationGuards{{
    {0x1699e10, {0x08, 0xf1, 0x00, 0xd0, 0xb7, 0x0a, 0x40, 0xf9, 0xe9, 0x03, 0x00, 0x91, 0x08, 0x0d, 0x45, 0xf9, 0x02, 0x0d, 0x80, 0x52, 0xa1, 0x43, 0x00, 0xd1, 0x15, 0x01, 0x40, 0xf9, 0xe8, 0x23, 0x40, 0xf9}},
    {0x1699e9c, {0x09, 0x6e, 0x8d, 0x52, 0x29, 0x00, 0xa0, 0x72, 0xc1, 0x02, 0x40, 0xf9, 0xe9, 0x6a, 0x69, 0xf8, 0x3f, 0x00, 0x00, 0xf1, 0xc8, 0x0a, 0x40, 0xb9, 0x20, 0x41, 0x02, 0x91, 0xe2, 0x03, 0x88, 0x9a}},
    {0x1699ec0, {0xe0, 0x00, 0x00, 0x54, 0x1f, 0x7c, 0x03, 0xa9, 0x1f, 0x7c, 0x02, 0xa9, 0x1f, 0x7c, 0x01, 0xa9, 0x1f, 0x7c, 0x00, 0xa9, 0x42, 0x00, 0x00, 0xb4, 0x26, 0x6e, 0x46, 0x94, 0xb5, 0x01, 0x00, 0xb4}},
    {0x268bd18, {0x28, 0x0b, 0x40, 0xf9, 0x09, 0x6e, 0x8d, 0x52, 0x29, 0x00, 0xa0, 0x72, 0xe0, 0x03, 0x00, 0x91, 0x08, 0x69, 0x69, 0xf8, 0x01, 0x41, 0x02, 0x91, 0x18, 0x9c, 0xbc, 0x97, 0x16, 0x1f, 0x45, 0xf9}}
}};
uint32_t U32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t U64(const uint8_t *p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }
uint64_t Add(uint64_t p, uint64_t n) {
  if (!p || n > UINT64_MAX - p)
    throw std::runtime_error("address");
  return p + n;
}
struct Trace {
  const ReadFn &read;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
  size_t bytes{};
  std::vector<uint8_t> Get(uint64_t p, size_t n) {
    if (!p || n > UINT64_MAX - p || n > 262144 - bytes ||
        records.size() >= 4096)
      throw std::runtime_error("budget");
    std::vector<uint8_t> b(n);
    if (!read(p, b.data(), n))
      throw std::runtime_error("read");
    bytes += n;
    records.emplace_back(p, b);
    return b;
  }
  uint64_t Q(uint64_t p) {
    if (p % 8)
      throw std::runtime_error("alignment");
    const auto b = Get(p, 8);
    return U64(b.data());
  }
  uint64_t Ptr(uint64_t p) {
    const auto v = Q(p);
    if (!v || v % 8)
      throw std::runtime_error("pointer");
    return v;
  }
  bool Same() const {
    for (const auto &[p, b] : records) {
      std::vector<uint8_t> now(b.size());
      if (!read(p, now.data(), now.size()) || now != b)
        return false;
    }
    return true;
  }
};
void Epoch(Trace &t, uint64_t base, const MapEpochSource &s) {
  const auto owner = t.Ptr(Add(base, 0x34bc848));
  const auto save = t.Ptr(Add(owner, 0x10));
  if (owner != s.save_epoch.access_object || save != s.save_epoch.save_data ||
      t.Ptr(Add(save, 0x16b80)) != s.traversal ||
      t.Ptr(Add(save, 0x16cf8)) != s.save_epoch.list)
    throw std::runtime_error("epoch");
  const auto learned = t.Get(s.save_epoch.list, 32);
  if (U32(learned.data() + 16) > 512 ||
      U32(learned.data() + 24) != s.save_epoch.revision ||
      t.Ptr(Add(base, 0x34b80b8)) != s.registry_owner ||
      t.Ptr(Add(s.registry_owner, 0x78)) != s.registry_header ||
      t.Ptr(Add(s.registry_header, 0x20)) != s.registry_entries)
    throw std::runtime_error("map");
}
uint32_t Lookup(Trace &t, uint64_t header, uint32_t key, uint32_t count) {
  const auto h = t.Get(header, 40);
  const auto map = U64(h.data()), buckets = U64(h.data() + 8);
  const auto load = std::bit_cast<float>(U32(h.data() + 32));
  if (!map || map % 8 || !buckets || buckets > 8192 ||
      U64(h.data() + 24) != count || !std::isfinite(load) || load <= 0)
    throw std::runtime_error("hash");
  const auto bucket = key % buckets;
  const auto prior = t.Ptr(Add(map, bucket * 8));
  uint64_t node = t.Ptr(prior);
  std::set<uint64_t> seen;
  bool found{};
  uint32_t value{};
  while (node) {
    if (node % 8 || seen.size() >= count || !seen.insert(node).second)
      throw std::runtime_error("cycle");
    const auto n = t.Get(node, 24);
    const auto hash = U64(n.data() + 8);
    if (hash % buckets != bucket)
      break;
    if (hash == key && U32(n.data() + 16) == key) {
      if (found || U32(n.data() + 20) >= count)
        throw std::runtime_error("index");
      found = true;
      value = U32(n.data() + 20);
    }
    node = U64(n.data());
  }
  if (!found)
    throw std::runtime_error("missing flag");
  return value;
}
struct Frame {
  std::vector<RiftRegionState> states;
  Trace trace;
};
Frame Sample(const ReadFn &read, uint64_t base, const MapEpochSource &source,
             std::span<const NativeRiftRegion> regions) {
  Frame f{{}, Trace{read, {}, 0}};
  auto &t = f.trace;
  Epoch(t, base, source);
  const auto c = t.Get(Add(source.save_epoch.save_data, 0x16c08), 32);
  if (U64(c.data()) != Add(base, 0x33ff850) ||
      t.Q(Add(base, 0x33ff870)) != Add(base, 0x163b1e0))
    throw std::runtime_error("flag class");
  const auto p = U64(c.data() + 16);
  const auto capacity = U32(c.data() + 24);
  if (!p || p % 8 || !capacity || capacity > 8192)
    throw std::runtime_error("capacity");
  const auto bits = t.Get(p, 8 + (capacity + 7) / 8);
  const auto count = U32(bits.data());
  if (!count || count > capacity || U32(bits.data() + 4) != U32(c.data() + 8) ||
      !(t.Get(Add(base, 0x34a90c8), 1)[0] & 1))
    throw std::runtime_error("flags");
  (void)t.Ptr(Add(base, 0x34a9090));
  std::set<uint32_t> ids;
  auto flag = [&](const std::string &name) {
    if (name.empty() || name.size() > 128 ||
        !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
          return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '_';
        }))
      throw std::runtime_error("flag symbol");
    const auto index = Lookup(t, Add(base, 0x34a90a0), ActorKey(name), count);
    return ((bits[8 + index / 8] >> (index % 8)) & 1) != 0;
  };
  for (const auto &r : regions) {
    if (!ids.insert(r.unique_id).second)
      throw std::runtime_error("duplicate region");
    const bool activate = r.activate_flag.empty() || flag(r.activate_flag);
    const bool deactivate =
        !r.deactivate_flag.empty() && flag(r.deactivate_flag);
    f.states.push_back({r.unique_id, activate && !deactivate});
  }
  return f;
}
void Level(Trace &t, uint64_t base, const MapSnapshot &expected) {
  const auto &s = expected.current_level_source;
  if (t.Ptr(Add(base, 0x34bd718)) != s.controller ||
      t.Q(s.controller) != Add(base, 0x3269630) ||
      t.Ptr(Add(s.controller, 0x80)) != s.handle ||
      t.Q(s.handle) != Add(base, 0x3268098) ||
      t.Ptr(Add(s.handle, 0x90)) != s.resource ||
      t.Q(s.resource) != Add(base, 0x3268738) ||
      t.Q(Add(base, 0x32687d0)) != Add(base, 0x16833b0))
    throw std::runtime_error("level class");
  const auto d = t.Get(Add(s.resource, 0x140), 16);
  if (U64(d.data()) != s.key_pointer ||
      U64(d.data() + 8) != expected.current_level_key.size() ||
      expected.current_level_key.empty() ||
      expected.current_level_key.size() > 128)
    throw std::runtime_error("level key");
  const auto name = t.Get(s.key_pointer, expected.current_level_key.size());
  if (!std::equal(name.begin(), name.end(), expected.current_level_key.begin()))
    throw std::runtime_error("level key");
}
struct LocationFrame {
  std::string key;
  Trace trace;
};
LocationFrame LocationSample(const ReadFn &read, uint64_t base,
                             const MapSnapshot &expected,
                             std::span<const NativeLocation> metadata) {
  LocationFrame f{{}, Trace{read, {}, 0}};
  auto &t = f.trace;
  Epoch(t, base, expected.source);
  Level(t, base, expected);
  const auto b = t.Get(Add(expected.current_level_source.resource, 0x80), 72);
  const auto length = U64(b.data() + 64);
  if (!length || length > 63 || b[size_t(length)])
    throw std::runtime_error("location size");
  f.key.assign(reinterpret_cast<const char *>(b.data()), size_t(length));
  // Original Field location callback persists its authored UiLocationInfo key
  // in the current save's location group. Resource aliases retain precedence.
  // Use this only for the exact generic Field resource/level pair.
  if (f.key == "Field" && expected.current_level_key == "Field") {
    for (const auto &g : FieldLocationGuards) {
      const auto bytes = t.Get(Add(base, g.offset), g.bytes.size());
      if (!MatchSupportedGuard(bytes, g.bytes, g.offset))
        throw std::runtime_error("field location writer");
    }
    const auto location = t.Ptr(Add(expected.source.save_epoch.save_data, 0x16b70));
    auto symbol = [&](uint64_t address) {
      const auto bytes = t.Get(address, 64);
      const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{});
      if (end == bytes.begin() || end == bytes.end())
        throw std::runtime_error("field location symbol");
      return std::string(bytes.begin(), end);
    };
    if (symbol(Add(location, 0x10)) != expected.current_level_key)
      throw std::runtime_error("field location level");
    f.key = symbol(Add(location, 0x90));
  }
  if (!std::all_of(f.key.begin(), f.key.end(),
                   [](unsigned char c) {
                     return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_';
                   }) ||
      std::count_if(metadata.begin(), metadata.end(),
                    [&](const auto &e) { return e.key == f.key; }) != 1)
    throw std::runtime_error("location membership");
  return f;
}

} // namespace
RiftVisibilitySnapshot
ReadRiftVisibility(const ReadFn &read, uint64_t base, bool build,
                   const MapEpochSource &source,
                   std::span<const NativeRiftRegion> regions) {
  RiftVisibilitySnapshot out;
  if (!read || !build || !base || base % 4096 || regions.empty() ||
      regions.size() > 64)
    return out;
  try {
    for (const auto &g : Guards) {
      std::array<uint8_t, 32> bytes{};
      if (!read(Add(base, g.offset), bytes.data(), bytes.size()) ||
          !MatchSupportedGuard(bytes, g.bytes, g.offset))
        return out;
    }
    out.compatible = true;
    auto first = Sample(read, base, source, regions),
         second = Sample(read, base, source, regions);
    if (first.states != second.states ||
        first.trace.records != second.trace.records || !first.trace.Same() ||
        !second.trace.Same())
      return out;
    out.source = source;
    out.regions = std::move(first.states);
    out.known = true;
    return out;
  } catch (...) {
    return out;
  }
}
CurrentLocationSnapshot
ReadCurrentLocation(const ReadFn &read, uint64_t base, bool build,
                    const MapSnapshot &expected,
                    std::span<const NativeLocation> metadata) {
  CurrentLocationSnapshot out;
  if (!read || !build || !base || base % 4096 || !expected.compatible ||
      !expected.discovery_valid || !expected.current_level_valid ||
      metadata.empty() || metadata.size() > 1024)
    return out;
  try {
    for (const auto &g : LocationGuards) {
      std::array<uint8_t, 32> bytes{};
      if (!read(Add(base, g.offset), bytes.data(), bytes.size()) ||
          !MatchSupportedGuard(bytes, g.bytes, g.offset))
        return out;
    }
    out.compatible = true;
    auto a = LocationSample(read, base, expected, metadata),
         b = LocationSample(read, base, expected, metadata),
         c = LocationSample(read, base, expected, metadata);
    if (a.key != b.key || a.key != c.key ||
        a.trace.records != b.trace.records ||
        a.trace.records != c.trace.records || !a.trace.Same() ||
        !b.trace.Same() || !c.trace.Same())
      return out;
    out.source = expected.source;
    out.level_source = expected.current_level_source;
    out.key = std::move(a.key);
    out.known = true;
    return out;
  } catch (...) {
    return out;
  }
}

} // namespace echoes
