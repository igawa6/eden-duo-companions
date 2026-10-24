// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_quest_markers.h"
#include "echoes_catalog.h"
#include "echoes_code_guard.h"
#include "echoes_story.h"
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
bool Identifier(std::string_view s) {
  return !s.empty() && s.size() <= 128 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}
struct ReadTrace {
  const ReadFn &r;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
  size_t total_bytes{};
  std::vector<uint8_t> Bytes(uint64_t at, size_t n) {
    if (records.size() >= 8192 || n > 524288 - total_bytes)
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
constexpr std::array<Guard, 8> Guards{{
    {0x16a5b2c,
     {0x97, 0x02, 0x08, 0x8b, 0xe0, 0x03, 0x17, 0xaa, 0x13, 0x1c, 0x07,
      0x94, 0x88, 0x5a, 0x40, 0x91, 0xe1, 0x03, 0x17, 0xaa, 0x16, 0xc1,
      0x29, 0x91, 0xe0, 0x03, 0x16, 0xaa, 0x36, 0x16, 0x33, 0x94}},
    {0x236b6dc,
     {0x08, 0x56, 0x88, 0x52, 0x7f, 0xd2, 0x02, 0xb9, 0x88, 0x02, 0x08,
      0x8b, 0x76, 0xe2, 0x02, 0xb9, 0x7f, 0x7e, 0x01, 0xf9, 0x7f, 0x82,
      0x01, 0xf9, 0x68, 0x62, 0x01, 0xf9, 0x08, 0x58, 0x88, 0x52}},
    {0x232c4d8,
     {0xe8, 0x0a, 0x40, 0xf9, 0xbf, 0x06, 0x00, 0x71, 0x08, 0x59, 0x40,
      0x91, 0x08, 0xc1, 0x34, 0x91, 0xc0, 0x01, 0x00, 0x54, 0x35, 0x02,
      0x00, 0x35, 0x08, 0x01, 0x40, 0xf9, 0x08, 0x71, 0x10, 0x91}},
    {0x232d1c8,
     {0xc8, 0x0a, 0x40, 0xf9, 0x9f, 0x06, 0x00, 0x71, 0x08, 0x59, 0x40,
      0x91, 0x08, 0xc1, 0x34, 0x91, 0xa0, 0x00, 0x00, 0x54, 0x54, 0x01,
      0x00, 0x35, 0x08, 0x01, 0x40, 0xf9, 0x08, 0x61, 0x10, 0x91}},
    {0x25f1ed8,
     {0x08, 0x1c, 0x00, 0x12, 0x1f, 0x0d, 0x00, 0x71, 0xe8, 0x02, 0x00,
      0x54, 0x08, 0x1c, 0x40, 0x92, 0x49, 0x17, 0x00, 0xb0, 0x29, 0x79,
      0x0b, 0x91, 0xaa, 0x00, 0x00, 0x10, 0x2b, 0x69, 0x68, 0x38}},
    {0x15816cc,
     {0xdd, 0x0e, 0x00, 0x94, 0xa8, 0x03, 0x5e, 0x38, 0x08, 0xba, 0x00,
      0x35, 0x9f, 0x02, 0x15, 0x6b, 0x82, 0x22, 0x40, 0x7a, 0xe8, 0x87,
      0x9f, 0x1a, 0xb3, 0x08, 0x00, 0x14, 0xe0, 0x43, 0x00, 0x91}},
    {0x1583504,
     {0xc8, 0x06, 0x40, 0x39, 0x1f, 0xf5, 0x00, 0x71, 0x81, 0x00, 0x00,
      0x54, 0x9f, 0x02, 0x13, 0x6b, 0xe8, 0x17, 0x9f, 0x1a, 0x26, 0x01,
      0x00, 0x14, 0x22, 0x98, 0x00, 0xd0, 0x42, 0x84, 0x2e, 0x91}},
    {0x545a18,
     {0x01, 0x01, 0x40, 0xb9, 0xe0, 0x03, 0x14, 0xaa, 0xdc, 0x95, 0x77,
      0x94, 0x80, 0x01, 0x00, 0x34, 0xf5, 0x03, 0x00, 0x2a, 0xe0, 0x03,
      0x14, 0xaa, 0xe2, 0x94, 0x77, 0x94, 0x09, 0x20, 0x40, 0xa9}},
}};
struct Frame {
  CheckpointVisibilitySnapshot result;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> trace;
};
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
struct Evaluation {
  bool known{}, value{};
};
Frame Sample(const ReadFn &r, uint64_t base, const MapEpochSource &source,
             std::span<const NativeQuestMarkerQuest> catalog,
             std::span<const NativeCheckpointGroup> groups) {
  ReadTrace t{r, {}};
  Epoch(t, base, source);
  Frame f;
  auto &out = f.result;
  const auto save = source.save_epoch.save_data;
  // Original save constructor16a5b2c..48 and backing adapter236b420:
  // adapter atSave+16a70, raw backing atSave+bcf0, quest record raw+42b0.
  if (t.Pointer(Add(save, 0x16a70)) != Add(save, 0xbcf0) ||
      t.Pointer(Add(save, 0x16d30)) != Add(save, 0xffa0))
    throw std::runtime_error("backing ownership");
  const auto obj = t.Bytes(Add(save, 0x16d38), 32);
  const auto payload = U64(obj.data() + 16);
  if (U64(obj.data()) != Add(base, 0x33ff808) ||
      t.Q(Add(base, 0x33ff828)) != Add(base, 0x232a9b0) ||
      payload != Add(save, 0xffb0) || U32(obj.data() + 24) != 256)
    throw std::runtime_error("quest class");
  const auto values = t.Bytes(payload, 8 + catalog.size() * 4);
  if (U32(values.data()) != catalog.size() ||
      U32(values.data() + 4) != U32(obj.data() + 8) ||
      !(t.Bytes(Add(base, 0x34bc3f8), 1)[0] & 1))
    throw std::runtime_error("quest header");
  (void)t.Pointer(Add(base, 0x34bc390));
  const auto vector = t.Bytes(Add(base, 0x34bc3c8), 24);
  const auto begin = U64(vector.data()), end = U64(vector.data() + 8),
             cap = U64(vector.data() + 16);
  if (!begin || begin % 8 || end % 8 || cap % 8 || end < begin || cap < end ||
      (end - begin) % 40 || (cap - begin) % 40 ||
      (end - begin) / 40 != catalog.size() || (cap - begin) / 40 > 128)
    throw std::runtime_error("stage vector");
  const auto track = t.Bytes(Add(save, 0x103b8), 8),
             side = t.Bytes(Add(save, 0x1043c), 4);
  out.tracked_quests = {U32(track.data() + 4), U32(side.data())};
  out.display = {track[0] != 0, track[1] != 0};
  std::map<uint32_t, size_t> bycrc;
  std::map<std::string, size_t, std::less<>> byname;
  for (size_t i = 0; i < catalog.size(); ++i) {
    const auto &q = catalog[i];
    if (!Identifier(q.key) || q.kind > 2 || q.progress_crcs.empty() ||
        q.progress_crcs.size() > 256 ||
        !bycrc.emplace(ActorKey(q.key), i).second ||
        !byname.emplace(q.key, i).second ||
        std::set<uint32_t>(q.progress_crcs.begin(), q.progress_crcs.end())
                .size() != q.progress_crcs.size())
      throw std::runtime_error("catalog");
  }
  std::map<size_t, uint32_t> observed;
  auto ordinal = [&](size_t i) -> uint32_t {
    const auto it = observed.find(i);
    if (it != observed.end())
      return it->second;
    const auto &q = catalog[i];
    const auto qi = Lookup(t, Add(base, 0x34bc3a0), ActorKey(q.key),
                           uint32_t(catalog.size()), uint32_t(catalog.size()));
    if (qi != i)
      throw std::runtime_error("quest metadata");
    const auto current = U32(values.data() + 8 + i * 4);
    const auto at =
        std::find(q.progress_crcs.begin(), q.progress_crcs.end(), current);
    if (at == q.progress_crcs.end())
      throw std::runtime_error("stage sentinel");
    const auto oi =
        Lookup(t, Add(begin, i * 40), current, uint32_t(q.progress_crcs.size()),
               uint32_t(q.progress_crcs.size()));
    if (oi != size_t(at - q.progress_crcs.begin()))
      throw std::runtime_error("stage metadata");
    observed[i] = oi;
    return oi;
  };
  auto condition = [&](const NativeCheckpointCondition &c) -> Evaluation {
    if (c.category == 1) {
      auto it = byname.find(c.parameter);
      if (it == byname.end())
        return {};
      const auto oi = ordinal(it->second);
      return {true,
              oi > 0 && oi < catalog[it->second].progress_crcs.size() - 1};
    }
    if (c.category != 3)
      return {};
    const auto first = c.parameter.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
    if (first == std::string::npos || !first)
      return {};
    const auto it = byname.find(c.parameter.substr(0, first));
    if (it == byname.end())
      return {};
    const auto &q = catalog[it->second];
    const auto oi = ordinal(it->second);
    auto stage = [&](std::string_view name, uint32_t &value) {
      if (!Identifier(name))
        return false;
      const auto crc = ActorKey(name);
      const auto at =
          std::find(q.progress_crcs.begin(), q.progress_crcs.end(), crc);
      if (at == q.progress_crcs.end())
        return false;
      value = Lookup(t, Add(begin, it->second * 40), crc,
                     uint32_t(q.progress_crcs.size()),
                     uint32_t(q.progress_crcs.size()));
      return value == size_t(at - q.progress_crcs.begin());
    };
    if (c.parameter[first] == ':') {
      const auto tilde = c.parameter.find('~', first + 1);
      uint32_t lo{}, hi{};
      if (tilde == std::string::npos ||
          !stage(std::string_view(c.parameter)
                     .substr(first + 1, tilde - first - 1),
                 lo) ||
          !stage(std::string_view(c.parameter).substr(tilde + 1), hi))
        return {};
      return {true, oi >= lo && oi <= hi};
    }
    const auto last = c.parameter.find_first_not_of("=!<>", first);
    if (last == std::string::npos)
      return {};
    const auto op = std::string_view(c.parameter).substr(first, last - first);
    uint32_t target{};
    if (!stage(std::string_view(c.parameter).substr(last), target))
      return {};
    if (op == "=" || op == "==")
      return {true, oi == target};
    if (op == "!=")
      return {true, oi != target};
    if (op == "<")
      return {true, oi < target};
    if (op == ">")
      return {true, oi > target};
    if (op == "<=")
      return {true, oi <= target};
    if (op == ">=")
      return {true, oi >= target};
    return {};
  };
  size_t marker_count = 0;
  std::set<uint32_t> group_keys;
  for (const auto &g : groups)
    if (g.markers.size() > 16384 - marker_count ||
        (marker_count += g.markers.size()) > 16384 ||
        !group_keys.insert(g.quest_crc).second)
      throw std::runtime_error("duplicate group");
  for (size_t kind = 0; kind < 2; ++kind) {
    const auto crc = out.tracked_quests[kind];
    if (!crc)
      continue;
    const auto qi = bycrc.find(crc);
    if (qi == bycrc.end() || catalog[qi->second].kind != kind)
      throw std::runtime_error("tracked metadata");
    (void)ordinal(qi->second);
    if (!out.display[kind])
      continue;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      const auto &g = groups[gi];
      if (g.quest_crc != crc)
        continue;
      for (size_t mi = 0; mi < g.markers.size(); ++mi) {
        const auto &m = g.markers[mi];
        if (m.conditions.size() > 256)
          throw std::runtime_error("condition budget");
        bool supported = m.condition_strategy <= 1;
        bool pass = m.condition_strategy == 0 && !m.conditions.empty();
        for (const auto &c : m.conditions) {
          const auto v = condition(c);
          supported &= v.known;
          if (m.condition_strategy == 0)
            pass &= v.value;
          else
            pass |= v.value;
        }
        if (!supported) {
          ++out.unsupported_markers;
          continue;
        }
        if (pass)
          out.visible.push_back({uint32_t(gi), uint32_t(mi), uint32_t(kind)});
      }
    }
  }
  Epoch(t, base, source);
  out.source = source;
  out.known = true;
  f.trace = std::move(t.records);
  return f;
}
bool Same(const Frame &a, const Frame &b) {
  return a.trace == b.trace &&
         a.result.tracked_quests == b.result.tracked_quests &&
         a.result.display == b.result.display &&
         a.result.visible == b.result.visible &&
         a.result.unsupported_markers == b.result.unsupported_markers;
}
bool Replay(const ReadFn &r, const Frame &f) {
  for (const auto &[p, b] : f.trace) {
    std::vector<uint8_t> now(b.size());
    if (!r(p, now.data(), now.size()) || now != b)
      return false;
  }
  return true;
}
} // namespace
bool LoadMarkerQuestCatalog(const ReadAssetFn &r,
                            std::vector<NativeQuestMarkerQuest> &out) {
  out.clear();
  if (!r)
    return false;
  try {
    Sheet s(r, "region_common/datasheets/Quest.gsheet");
    if (s.top != 7 || s.stride != 104 || s.fields != 14 || !s.count ||
        s.count > 128)
      return false;
    s.Field(48, 7, "mKey", 4, 4, 0, 16, 16);
    s.Field(48, 7, "mKind", 2, 16, 16, 4, 4);
    const auto progress = s.Field(48, 7, "mProgress", 0, 2, 56, 16, 24),
               child = s.Children(progress, 3);
    s.Field(child, 3, "mLabel", 2, 0, 0, 4, 4);
    std::vector<NativeQuestMarkerQuest> q;
    std::set<uint32_t> keys;
    for (size_t i = 0; i < s.count; ++i) {
      const auto row = s.rows + i * s.stride;
      NativeQuestMarkerQuest v;
      v.key = s.Text(row);
      v.kind = s.D(row + 16);
      if (!Identifier(v.key) || v.kind > 2 ||
          !keys.insert(ActorKey(v.key)).second)
        return false;
      const auto [p, n] = s.Array(row + 56, 24, 256);
      if (!n)
        return false;
      std::set<uint32_t> stages;
      for (size_t j = 0; j < n; ++j) {
        const auto crc = s.D(p + j * 24);
        if (!stages.insert(crc).second)
          return false;
        v.progress_crcs.push_back(crc);
      }
      q.push_back(std::move(v));
    }
    out = std::move(q);
    return true;
  } catch (...) {
    return false;
  }
}
CheckpointVisibilitySnapshot
ReadCheckpointVisibility(const ReadFn &r, uint64_t base, bool build,
                         const MapEpochSource &source,
                         std::span<const NativeQuestMarkerQuest> catalog,
                         std::span<const NativeCheckpointGroup> groups) {
  CheckpointVisibilitySnapshot out;
  if (!r || !build || !base || base % 4096 || catalog.empty() ||
      catalog.size() > 128 || groups.size() > 4096)
    return out;
  try {
    auto guard = [&](uint64_t off, auto expected) {
      std::vector<uint8_t> b(expected.size());
      return r(Add(base, off), b.data(), b.size()) &&
             MatchSupportedGuard(b, expected, off);
    };
    for (const auto &g : Guards)
      if (!guard(g.offset, g.bytes))
        return out;
    for (const auto &g : FieldStoryIdentityGuards)
      if (!guard(g.offset, g.bytes))
        return out;
    out.compatible = true;
    const auto a = Sample(r, base, source, catalog, groups),
               b = Sample(r, base, source, catalog, groups),
               c = Sample(r, base, source, catalog, groups);
    if (!Same(a, b) || !Same(a, c) || !Replay(r, a) || !Replay(r, b) ||
        !Replay(r, c))
      return out;
    out = a.result;
    out.compatible = true;
    return out;
  } catch (...) {
    return out;
  }
}
} // namespace echoes
