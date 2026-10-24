// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_entrances.h"
#include "echoes_code_guard.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
namespace echoes {
namespace {
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
struct Sheet {
  std::vector<uint8_t> b;
  uint64_t pool{}, rows{};
  uint32_t count{}, stride{}, fields{}, top{};
  void Fit(uint64_t p, uint64_t n) const {
    if (p > b.size() || n > b.size() - p)
      throw std::runtime_error("sheet range");
  }
  uint32_t D(uint64_t p) const {
    Fit(p, 4);
    return U32(b.data() + p);
  }
  uint64_t Q(uint64_t p) const {
    Fit(p, 8);
    return U64(b.data() + p);
  }
  std::string C(uint64_t p) const {
    if (p < pool)
      throw std::runtime_error("pool");
    std::string s;
    for (size_t n = 0; n < 256; ++n) {
      Fit(p + n, 1);
      auto c = b[p + n];
      if (!c)
        return s;
      s += char(c);
    }
    throw std::runtime_error("text");
  }
  std::string Text(uint64_t at) const {
    auto p = Q(at), n = Q(at + 8);
    if (!n || n > 255 || p < pool)
      throw std::runtime_error("string");
    Fit(p, n + 1);
    if (b[p + n])
      throw std::runtime_error("termination");
    std::string s(reinterpret_cast<const char *>(b.data() + p), size_t(n));
    if (s.find('\0') != s.npos)
      throw std::runtime_error("nul");
    return s;
  }
  uint64_t Field(uint64_t first, size_t n, std::string_view name, uint8_t type,
                 uint16_t flags, uint16_t off, uint16_t size,
                 uint16_t step) const {
    uint64_t found = 0;
    for (size_t i = 0; i < n; ++i) {
      auto p = Add(first, i * 48);
      Fit(p, 48);
      if (C(Q(p)) != name)
        continue;
      if (found || b[p + 16] != type || (D(p + 18) & 65535) != flags ||
          (D(p + 20) & 65535) != off || (D(p + 22) & 65535) != size ||
          (D(p + 24) & 65535) != step)
        throw std::runtime_error("schema");
      found = p;
    }
    if (!found)
      throw std::runtime_error("field");
    return found;
  }
  uint64_t Children(uint64_t f, size_t expected) const {
    if ((D(f + 26) & 65535) != expected)
      throw std::runtime_error("children");
    auto p = Q(f + 32);
    if (p < 48 || (p - 48) % 48 || expected > fields ||
        (p - 48) / 48 > fields - expected)
      throw std::runtime_error("descriptors");
    return p;
  }
  std::pair<uint64_t, uint64_t> Array(uint64_t at, uint64_t step,
                                      uint64_t max) const {
    auto p = Q(at), n = Q(at + 8);
    if (n > max)
      throw std::runtime_error("array");
    Fit(p, n * step);
    return {p, n};
  }
  Sheet(const ReadAssetFn &r, std::string_view path) {
    auto n = r(path, 0, nullptr, 0);
    if (n < 48 || n > 1024 * 1024)
      throw std::runtime_error("asset budget");
    b.resize(n);
    if (r(path, 0, b.data(), n) != n || r(path, 0, nullptr, 0) != n)
      throw std::runtime_error("asset changed");
    if (std::memcmp(b.data(), "gsht\2\0\0\0", 8))
      throw std::runtime_error("format");
    pool = Q(16);
    top = D(24);
    fields = D(28);
    rows = Q(32);
    count = D(40);
    stride = D(44);
    if (fields > 128 || top > fields || count > 2048 || stride > 512 ||
        rows < 48 + fields * 48 || pool < rows || !stride)
      throw std::runtime_error("header");
    Fit(48, fields * 48);
    Fit(rows, uint64_t(count) * stride);
    Fit(pool, 1);
  }
};
struct ReadTrace {
  const ReadFn &r;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
  size_t total_bytes{};
  std::vector<uint8_t> Bytes(uint64_t at, size_t n) {
    if (records.size() >= 16384 || n > 524288 - total_bytes)
      throw std::runtime_error("budget");
    total_bytes += n;
    if (!at || n > UINT64_MAX - at)
      throw std::runtime_error("address");
    std::vector<uint8_t> b(n);
    if (!r(at, b.data(), n))
      throw std::runtime_error("read");
    records.emplace_back(at, b);
    return b;
  }
  uint64_t Q(uint64_t at) {
    if (at % 8)
      throw std::runtime_error("alignment");
    auto b = Bytes(at, 8);
    return U64(b.data());
  }
  uint64_t Pointer(uint64_t at) {
    auto v = Q(at);
    if (!v || v % 8)
      throw std::runtime_error("pointer");
    return v;
  }
};
uint32_t Lookup(ReadTrace &t, uint64_t at, uint32_t key,
                uint32_t expected_count, uint32_t max_value) {
  auto h = t.Bytes(at, 40);
  auto map = U64(h.data()), buckets = U64(h.data() + 8),
       size = U64(h.data() + 24);
  float load = std::bit_cast<float>(U32(h.data() + 32));
  if (!map || map % 8 || !buckets || buckets > 4096 || size != expected_count ||
      !size || !std::isfinite(load) || load <= 0)
    throw std::runtime_error("hash header");
  auto bucket = key % buckets;
  auto predecessor = t.Pointer(Add(map, bucket * 8));
  auto node = t.Pointer(predecessor);
  std::set<uint64_t> seen;
  bool found = false;
  uint32_t value = 0;
  while (node) {
    if (node % 8 || seen.size() >= size || !seen.insert(node).second)
      throw std::runtime_error("hash cycle");
    auto n = t.Bytes(node, 24);
    auto hash = U64(n.data() + 8);
    if (hash % buckets != bucket)
      break;
    if (hash == key && U32(n.data() + 16) == key) {
      if (found)
        throw std::runtime_error("duplicate");
      value = U32(n.data() + 20);
      if (value >= max_value)
        throw std::runtime_error("index");
      found = true;
    }
    node = U64(n.data());
  }
  if (!found)
    throw std::runtime_error("absent");
  return value;
}
struct Guard {
  uint64_t offset;
  std::array<uint8_t, 32> bytes;
};
constexpr std::array<Guard, 9> Guards{{
    {0x236b698,
     {0x8,  0x8e, 0x84, 0x52, 0x28, 0x0,  0x8,  0x8b, 0x8,  0x24, 0x1,
      0xf9, 0xa8, 0x84, 0x0,  0x90, 0x8,  0xa1, 0x26, 0x91, 0x8,  0x1c,
      0x1,  0xf9, 0x8,  0x8c, 0x82, 0x52, 0x28, 0x0,  0x8,  0x8b}},
    {0x25fade0,
     {0x8,  0x9,  0x40, 0xf9, 0x8,  0x59, 0x40, 0x91, 0x13, 0xa1, 0x32,
      0x91, 0x28, 0x1,  0x40, 0xf9, 0xa8, 0x4,  0x0,  0xb4, 0x29, 0x9,
      0x40, 0xb9, 0x69, 0x4,  0x0,  0x34, 0x3f, 0x5,  0x0,  0x71}},
    {0x25faea8,
     {0x68, 0xa,  0x40, 0xf9, 0x9,  0x1,  0x40, 0xb9, 0x3f, 0x1,  0x0,
      0x6b, 0xe9, 0x1,  0x0,  0x54, 0x69, 0x1a, 0x40, 0xb9, 0x2a, 0x1d,
      0x0,  0x11, 0xe9, 0x3,  0x0,  0x2a, 0x29, 0xfd, 0x43, 0xd3}},
    {0x3f0ad8,
     {0x6b, 0x86, 0x1,  0x90, 0x6b, 0x95, 0x40, 0xf9, 0x6b, 0x79, 0x6a,
      0xf8, 0xb,  0x1,  0x0,  0xb4, 0x6b, 0x1,  0x40, 0xf9, 0xcb, 0x0,
      0x0,  0xb4, 0x9f, 0x9,  0x0,  0xf1, 0x82, 0x2,  0x0,  0x54}},
    {0x6035e8,
     {0x8,  0x4,  0x40, 0xf9, 0x9,  0x21, 0x40, 0xa9, 0x8,  0x65, 0x40,
      0xb9, 0x28, 0x1,  0x8,  0x8b, 0x0,  0x4d, 0x21, 0x8b, 0xc0, 0x3,
      0x5f, 0xd6, 0x8,  0x4,  0x40, 0xf9, 0x9,  0x21, 0x40, 0xa9}},
    {0x260fc78,
     {0xe8, 0xab, 0x50, 0xa9, 0x49, 0x3,  0x0,  0x12, 0xeb, 0x8f, 0x40,
      0xf9, 0xe9, 0x53, 0x8,  0x39, 0xf4, 0x13, 0x2,  0xb9, 0xed, 0x3b,
      0x40, 0xf9, 0xf7, 0x23, 0x2,  0xb9, 0xf6, 0x27, 0x2,  0xb9}},
    {0x16a5370,
     {0x48, 0x1,  0x0,  0xb4, 0x9,  0x3d, 0x40, 0xf9, 0xcb, 0xe2, 0x93,
      0x52, 0x2b, 0xe7, 0xa1, 0x72, 0x2a, 0x9,  0x40, 0xb9, 0x5f, 0x1,
      0xb,  0x6b, 0xc1, 0x15, 0x0,  0x54, 0x29, 0x29, 0x40, 0xb9}},
    {0x16a55e4,
     {0x8,  0x1b, 0x14, 0x8b, 0xe0, 0x3,  0x15, 0xaa, 0xe3, 0x3,  0x14,
      0x2a, 0x1,  0x1,  0x40, 0xf9, 0x8,  0x9,  0x40, 0xb9, 0x3f, 0x0,
      0x0,  0xf1, 0xe2, 0x3,  0x88, 0x9a, 0xd4, 0xaf, 0xfe, 0x97}},
    {0x16515f4,
     {0xca, 0x6a, 0x6a, 0x38, 0x4a, 0x1,  0x9,  0x4a, 0x4a, 0x1d, 0x40,
      0x92, 0x8,  0x79, 0x6a, 0xb8, 0x9,  0x21, 0x49, 0x4a, 0xe2, 0x3,
      0x29, 0x2a, 0x82, 0x56, 0x2,  0x29, 0xf5, 0x3,  0x2,  0x2a}},
}};
void Epoch(ReadTrace &t, uint64_t base, const MapEpochSource &s) {
  const auto owner = t.Pointer(Add(base, 0x34bc848)),
             save = t.Pointer(Add(owner, 0x10));
  if (owner != s.save_epoch.access_object || save != s.save_epoch.save_data ||
      t.Pointer(Add(save, 0x16b80)) != s.traversal ||
      t.Pointer(Add(save, 0x16cf8)) != s.save_epoch.list)
    throw std::runtime_error("epoch");
  const auto list = t.Bytes(s.save_epoch.list, 32);
  if (U32(list.data() + 16) > 512 ||
      U32(list.data() + 24) != s.save_epoch.revision ||
      t.Pointer(Add(base, 0x34b80b8)) != s.registry_owner ||
      t.Pointer(Add(s.registry_owner, 0x78)) != s.registry_header ||
      t.Pointer(Add(s.registry_header, 0x20)) != s.registry_entries)
    throw std::runtime_error("epoch");
}
uint32_t Crc(std::string_view s) {
  uint32_t c = ~0u;
  for (unsigned char ch : s) {
    c ^= ch;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
  }
  return ~c;
}
void Schema(const Sheet &s, std::span<const std::string_view> names,
            std::span<const uint8_t> types, std::span<const uint16_t> offsets,
            uint32_t stride) {
  if (s.top != names.size() || s.fields != names.size() || s.stride != stride)
    throw std::runtime_error("schema size");
  for (size_t i = 0; i < names.size(); ++i) {
    auto at = 48 + i * 48;
    const uint16_t flags = i == 0 ? 4 : types[i] == 2 ? 16 : 0;
    if (s.C(s.Q(at)) != names[i] || s.b[at + 17] || s.Q(at + 32) ||
        (s.D(at + 26) & 65535))
      throw std::runtime_error("schema order");
    s.Field(48, names.size(), names[i], types[i], flags, offsets[i],
            types[i] == 4 ? 16 : 4, types[i] == 4 ? 16 : 4);
  }
}
void Catalog(const ReadAssetFn &read, NativeEntranceCatalog &out) {
  const Sheet icons(
      read, "region_common/datasheets/ui/UiMapIconSensorLevelOpen.gsheet");
  constexpr std::array<std::string_view, 6> names{
      "mKey",       "mViewZoomLevel", "mTextureName",
      "mLabelName", "mOffsetX",       "mOffsetY"};
  constexpr std::array<uint8_t, 6> types{4, 2, 4, 4, 3, 3};
  constexpr std::array<uint16_t, 6> offsets{0, 16, 24, 40, 56, 60};
  Schema(icons, names, types, offsets, 64);
  if (icons.D(8) != 0x0f399f16)
    throw std::runtime_error("original icon schema identity");
  if (!icons.count || icons.count > 256)
    throw std::runtime_error("icon count");
  std::map<std::string, uint32_t, std::less<>> bykey;
  for (uint32_t i = 0; i < icons.count; ++i) {
    const auto p = icons.rows + i * 64;
    NativeEntranceIcon icon;
    icon.key = icons.Text(p);
    if (icons.Q(p + 32))
      icon.texture_name = icons.Text(p + 24);
    else if (icons.Q(p + 24) < icons.pool || icons.C(icons.Q(p + 24)) != "")
      throw std::runtime_error("empty texture");
    if (icons.Q(p + 48))
      icon.label_ref = icons.Text(p + 40);
    else if (icons.Q(p + 40) < icons.pool || icons.C(icons.Q(p + 40)) != "")
      throw std::runtime_error("empty label");
    icon.zoom_level = icons.D(p + 16);
    icon.offset = {std::bit_cast<float>(icons.D(p + 56)),
                   std::bit_cast<float>(icons.D(p + 60))};
    if (icon.key.size() > 128 || icon.texture_name.size() > 64 ||
        icon.zoom_level > 2 || !std::isfinite(icon.offset[0]) ||
        !std::isfinite(icon.offset[1]) || !bykey.emplace(icon.key, i).second)
      throw std::runtime_error("icon metadata");
    out.icons.push_back(std::move(icon));
  }
  const Sheet pairs(
      read, "region_common/datasheets/ui/UiMapIconSensorLevelOpenPair.gsheet");
  constexpr std::array<std::string_view, 2> pn{"mKey", "mPairKey"};
  constexpr std::array<uint8_t, 2> pt{4, 4};
  constexpr std::array<uint16_t, 2> po{0, 16};
  Schema(pairs, pn, pt, po, 32);
  if (pairs.count > 256)
    throw std::runtime_error("pair count");
  std::set<std::string> paired;
  for (uint32_t i = 0; i < pairs.count; ++i) {
    const auto p = pairs.rows + i * 32;
    if (!paired.insert(pairs.Text(p)).second || pairs.Text(p + 16).empty())
      throw std::runtime_error("pair metadata");
  }
  constexpr auto path = "region_common/level/Common.eclo";
  const auto n = read(path, 0, nullptr, 0);
  if (n < 0x8c || n > 1024 * 1024)
    throw std::runtime_error("Common size");
  std::vector<uint8_t> b(n);
  if (read(path, 0, b.data(), n) != n || read(path, 0, nullptr, 0) != n ||
      std::memcmp(b.data(), "eclo\3\0\1\1", 8))
    throw std::runtime_error("Common format");
  auto fit = [&](uint64_t p, uint64_t len) {
    if (p > b.size() || len > b.size() - p)
      throw std::runtime_error("Common range");
  };
  auto d = [&](size_t p) {
    fit(p, 4);
    return U32(b.data() + p);
  };
  auto h = [&](size_t p) {
    fit(p, 2);
    return uint16_t(b[p] | uint16_t(b[p + 1]) << 8);
  };
  if (d(8) != n || d(0x84) != 0x65636c64 || d(0xc) != d(0x14) ||
      d(0xc) != d(0x88) || d(0x74) != d(0x7c) || d(0xc) > 4096 ||
      d(0x2c) > 16384 || d(0x6c) > 65536 || d(0x74) > 16384)
    throw std::runtime_error("Common header");
  const auto sc = d(0x74), si = d(0x78), ss = d(0x80), rows = d(0x18),
             dir = d(0x10), ac = d(0x2c), actors = d(0x30), tc = d(0x6c),
             typed = d(0x70);
  fit(si, uint64_t(sc) * 4);
  fit(rows, uint64_t(d(0xc)) * 0x120);
  fit(dir, uint64_t(d(0xc)) * 8);
  fit(actors, uint64_t(ac) * 64);
  fit(typed, uint64_t(tc) * 8);
  auto str = [&](uint32_t index) {
    if (index >= sc)
      throw std::runtime_error("Common string index");
    const uint64_t p = uint64_t(ss) + d(si + index * 4);
    std::string s;
    for (size_t j = 0; j < 256; ++j) {
      fit(p + j, 1);
      if (!b[p + j])
        return s;
      if (b[p + j] < 32 || b[p + j] >= 127)
        throw std::runtime_error("Common string");
      s += char(b[p + j]);
    }
    throw std::runtime_error("Common unterminated");
  };
  size_t selected = 0;
  std::set<uint32_t> indices;
  uint32_t previous = 0;
  for (uint32_t i = 0; i < d(0xc); ++i) {
    const auto hash = d(dir + i * 8), ix = d(dir + i * 8 + 4);
    if (ix >= d(0xc) || (i && hash <= previous) || !indices.insert(ix).second)
      throw std::runtime_error("Common dictionary");
    previous = hash;
    const auto row = rows + ix * 0x120;
    const auto name = str(d(row + 0x100));
    if (Crc(name) != hash)
      throw std::runtime_error("Common key");
    if (name == "Field") {
      if (selected)
        throw std::runtime_error("duplicate Field");
      selected = row;
    }
  }
  if (!selected)
    throw std::runtime_error("Field absent");
  const auto first = d(selected + 0x10c);
  const auto count = h(selected + 0x114);
  if (first > ac || count > ac - first)
    throw std::runtime_error("Field actor range");
  auto parameter = [&](size_t actor, size_t slot) {
    const auto index = h(actor + 32 + slot * 2);
    if (index >= tc || h(typed + index * 8) != 4)
      throw std::runtime_error("typed string");
    return str(d(typed + index * 8 + 4));
  };
  for (uint32_t i = 0; i < count; ++i) {
    const auto at = actors + (first + i) * 64;
    if (d(at + 48) != Crc("SensorLevelOpen"))
      continue;
    const auto level = parameter(at, 0), locator = parameter(at, 1),
               key = level + ":" + locator;
    const auto found = bykey.find(key);
    if (found == bykey.end() || paired.contains(key) ||
        out.icons[found->second].texture_name.empty())
      continue;
    NativeFieldEntrance e;
    e.actor_index = first + i;
    e.icon_index = found->second;
    for (size_t j = 0; j < 3; ++j) {
      e.world[j] = std::bit_cast<float>(d(at + j * 4));
      if (!std::isfinite(e.world[j]))
        throw std::runtime_error("actor position");
    }
    out.field.push_back(e);
  }
}
struct Frame {
  std::vector<uint8_t> visible;
  bool native_dictionary_validated{};
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> trace;
};
Frame Sample(const ReadFn &r, uint64_t base, const MapEpochSource &source,
             const NativeEntranceCatalog &catalog) {
  ReadTrace t{r, {}};
  Epoch(t, base, source);
  const auto save = source.save_epoch.save_data;
  if (t.Pointer(Add(save, 0x16a70)) != Add(save, 0xbcf0))
    throw std::runtime_error("raw backing");
  const auto obj = t.Bytes(Add(save, 0x16ca8), 32);
  if (U64(obj.data()) != Add(base, 0x33ff9a8) ||
      t.Q(Add(base, 0x33ff9c8)) != Add(base, 0x3f0a30) ||
      U64(obj.data() + 16) != Add(save, 0xe160) || U32(obj.data() + 24) != 256)
    throw std::runtime_error("entrance container");
  const auto data = t.Bytes(Add(save, 0xe160), 40);
  if (U32(data.data()) != catalog.icons.size() ||
      U32(data.data() + 4) != U32(obj.data() + 8) ||
      !catalog.NativeIndexOrderValid())
    throw std::runtime_error("entrance header");
  // Native16a5554..5600 builds CRC->row ordinal directly from GSHT rows;
  // 1651608..160c stores that ordinal. The saved bit index is therefore
  // known even before the lazily loaded native map metadata exists.
  // Still authenticate an available runtime dictionary; a partial/corrupt
  // dictionary is never treated as an uninitialized optional source.
  const auto initialized = t.Bytes(Add(base, 0x34bc150), 1)[0];
  const auto runtime = t.Bytes(Add(base, 0x34bc108), 72);
  const bool resources = U64(runtime.data()) || U64(runtime.data() + 16);
  const bool dictionary = U64(runtime.data() + 32) ||
                          U64(runtime.data() + 40) ||
                          U64(runtime.data() + 48) || U64(runtime.data() + 56);
  Frame f;
  if (resources || dictionary) {
    if (!(initialized & 1))
      throw std::runtime_error("partial metadata initialization");
    (void)t.Pointer(Add(base, 0x34bc108));
    (void)t.Pointer(Add(base, 0x34bc118));
    std::set<uint32_t> hashes;
    for (size_t i = 0; i < catalog.icons.size(); ++i) {
      const auto hash = Crc(catalog.icons[i].key);
      if (!hashes.insert(hash).second ||
          Lookup(t, Add(base, 0x34bc128), hash, uint32_t(catalog.icons.size()),
                 uint32_t(catalog.icons.size())) != i)
        throw std::runtime_error("entrance dictionary");
    }
    f.native_dictionary_validated = true;
  } else {
    // Exact empty native singleton, either untouched BSS or its constructor.
    // Ignore no unknown/live discovery source: saved ownership/count/revision
    // and all original initializer code remain mandatory above.
    if (U64(runtime.data() + 8) || U64(runtime.data() + 24) ||
        U32(runtime.data() + 68) ||
        U32(runtime.data() + 64) != ((initialized & 1) ? 0x3f800000u : 0u))
      throw std::runtime_error("nonempty metadata initialization");
  }
  for (const auto &e : catalog.field) {
    if (e.icon_index >= catalog.icons.size())
      throw std::runtime_error("candidate index");
    f.visible.push_back((data[8 + e.icon_index / 8] >> (e.icon_index % 8)) & 1);
  }
  Epoch(t, base, source);
  f.trace = std::move(t.records);
  return f;
}
bool Replay(const ReadFn &read, const Frame &f) {
  for (const auto &[p, b] : f.trace) {
    std::vector<uint8_t> now(b.size());
    if (!read(p, now.data(), now.size()) || now != b)
      return false;
  }
  return true;
}
} // namespace
bool NativeEntranceCatalog::NativeIndexOrderValid() const {
  if (original_key_order_.empty() || original_key_order_.size() != icons.size())
    return false;
  for (size_t i = 0; i < icons.size(); ++i)
    if (icons[i].key != original_key_order_[i])
      return false;
  return true;
}
bool LoadNativeEntrances(const ReadAssetFn &read, NativeEntranceCatalog &out) {
  out = {};
  if (!read)
    return false;
  try {
    NativeEntranceCatalog parsed;
    Catalog(read, parsed);
    for (const auto &icon : parsed.icons)
      parsed.original_key_order_.push_back(icon.key);
    out = std::move(parsed);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}
EntranceVisibilitySnapshot
ReadEntranceVisibility(const ReadFn &read, uint64_t base, bool build,
                       const MapEpochSource &source,
                       const NativeEntranceCatalog &catalog) {
  EntranceVisibilitySnapshot out;
  if (!read || !build || !base || base % 4096 || catalog.icons.empty() ||
      catalog.icons.size() > 256 || catalog.field.size() > 16384)
    return out;
  try {
    for (const auto &g : Guards) {
      std::array<uint8_t, 32> b{};
      if (!read(Add(base, g.offset), b.data(), b.size()) ||
          !MatchSupportedGuard(b, g.bytes, g.offset))
        return out;
    }
    out.compatible = true;
    const auto a = Sample(read, base, source, catalog),
               b = Sample(read, base, source, catalog),
               c = Sample(read, base, source, catalog);
    if (a.trace != b.trace || b.trace != c.trace || a.visible != b.visible ||
        b.visible != c.visible || !Replay(read, c))
      return out;
    out.native_dictionary_validated = c.native_dictionary_validated;
    out.visible = c.visible;
    out.source = source;
    out.known = true;
  } catch (const std::exception &) {
  }
  return out;
}
} // namespace echoes
