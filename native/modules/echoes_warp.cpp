// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_warp.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace echoes {
namespace {
constexpr size_t Budget = 65536;
bool Symbol(std::string_view s) {
  return !s.empty() && s.size() <= 128 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_';
         });
}
struct Sheet {
  std::span<const uint8_t> b;
  uint64_t rows{}, pool{};
  uint32_t count{}, stride{};
  void Fit(uint64_t p, uint64_t n) const {
    if (p > b.size() || n > b.size() - p)
      throw std::runtime_error("bounds");
  }
  uint16_t H(uint64_t p) const {
    Fit(p, 2);
    return uint16_t(b[size_t(p)]) | uint16_t(b[size_t(p + 1)]) << 8;
  }
  uint32_t D(uint64_t p) const { return H(p) | uint32_t(H(p + 2)) << 16; }
  uint64_t Q(uint64_t p) const { return D(p) | uint64_t(D(p + 4)) << 32; }
  std::string Text(uint64_t p, uint64_t n) const {
    Fit(p, n + 1);
    if (p < pool || !n || n > 128 || b[size_t(p + n)])
      throw std::runtime_error("text");
    std::string s(reinterpret_cast<const char *>(b.data() + p), size_t(n));
    if (!Symbol(s))
      throw std::runtime_error("symbol");
    return s;
  }
  std::string Ref(uint64_t p) const { return Text(Q(p), Q(p + 8)); }
  Sheet(std::span<const uint8_t> data, uint32_t fields, uint32_t all,
        uint32_t row_stride, uint32_t max_rows) : b(data) {
    constexpr std::array<uint8_t, 8> magic{'g', 's', 'h', 't', 2, 0, 0, 0};
    Fit(0, 48);
    if (b.size() > Budget || !std::equal(magic.begin(), magic.end(), b.begin()) ||
        D(24) != fields || D(28) != all || D(44) != row_stride)
      throw std::runtime_error("schema");
    rows = Q(32); pool = Q(16); count = D(40); stride = D(44);
    Fit(48, uint64_t(all) * 48);
    if (!count || count > max_rows || rows < 48 + uint64_t(all) * 48 ||
        pool < rows + uint64_t(count) * stride)
      throw std::runtime_error("directory");
    Fit(rows, uint64_t(count) * stride); Fit(pool, 1);
  }
  void Field(size_t i, std::string_view name, uint16_t type, uint16_t flags,
             uint16_t offset, uint16_t size, uint16_t item_stride,
             uint16_t children = 0, uint64_t child_offset = 0) const {
    const uint64_t p = 48 + uint64_t(i) * 48;
    if (Text(Q(p), name.size()) != name || H(p + 16) != type ||
        H(p + 18) != flags || H(p + 20) != offset || H(p + 22) != size ||
        H(p + 24) != item_stride || H(p + 26) != children ||
        Q(p + 32) != child_offset)
      throw std::runtime_error("field");
  }
};
std::vector<uint8_t> Read(const ReadAssetFn &read, std::string_view path) {
  const auto n = read ? read(path, 0, nullptr, 0) : 0;
  if (!n || n > Budget) throw std::runtime_error("asset budget");
  std::vector<uint8_t> b(n);
  if (read(path, 0, b.data(), n) != n || read(path, 0, nullptr, 0) != n)
    throw std::runtime_error("asset changed");
  return b;
}
bool Valid(const NativeWarpSet &set) {
  if (!Symbol(set.key) || set.destinations.empty() || set.destinations.size() > 64)
    return false;
  std::set<std::pair<std::string, std::string>> identities;
  std::set<uint32_t> reports;
  for (const auto &d : set.destinations) {
    if (!Symbol(d.level) || !Symbol(d.location) || !Symbol(d.unlock_flag) ||
        d.report_id > 100000 ||
        !identities.emplace(d.level, d.unlock_flag).second ||
        (d.report_id && !reports.insert(d.report_id).second)) return false;
  }
  return true;
}
} // namespace
bool ParseNativeWarpCatalog(std::span<const uint8_t> sets,
                            std::span<const uint8_t> correspondence,
                            NativeWarpCatalog &out) {
  out = {};
  try {
    Sheet s(sets, 2, 6, 32, 8), c(correspondence, 2, 2, 32, 128);
    s.Field(0, "mKey", 4, 4, 0, 16, 16);
    s.Field(1, "mWarpDestInfo", 0, 2, 16, 16, 56, 4, 144);
    s.Field(2, "mLevelName", 260, 0, 0, 16, 16);
    s.Field(3, "mLocation", 260, 0, 16, 16, 16);
    s.Field(4, "mGameFlag", 260, 9, 32, 16, 16);
    s.Field(5, "mReportId", 258, 0, 48, 4, 4);
    c.Field(0, "mKey", 4, 4, 0, 16, 16);
    c.Field(1, "mWarpSet", 4, 8, 16, 16, 16);
    NativeWarpCatalog parsed;
    std::set<std::string> names;
    for (size_t i = 0; i < s.count; ++i) {
      const auto row = s.rows + i * 32;
      NativeWarpSet set; set.key = s.Ref(row);
      if (!names.insert(set.key).second) return false;
      const auto p = s.Q(row + 16), n = s.Q(row + 24);
      if (p < s.pool || !n || n > 64) return false;
      s.Fit(p, n * 56);
      for (size_t j = 0; j < n; ++j) {
        const auto a = p + j * 56;
        set.destinations.push_back({s.Ref(a), s.Ref(a + 16), s.Ref(a + 32),
                                    s.D(a + 48)});
      }
      if (!Valid(set)) return false;
      parsed.sets.push_back(std::move(set));
    }
    std::set<std::string> levels;
    for (size_t i = 0; i < c.count; ++i) {
      const auto row = c.rows + i * 32;
      auto level = c.Ref(row), set = c.Ref(row + 16);
      if (!levels.insert(level).second || !names.contains(set)) return false;
      parsed.level_sets.emplace_back(std::move(level), std::move(set));
    }
    out = std::move(parsed);
    return true;
  } catch (...) { return false; }
}
bool LoadNativeWarpCatalog(const ReadAssetFn &read, NativeWarpCatalog &out) {
  out = {};
  try {
    const auto sets = Read(read, "region_common/datasheets/ui/UiWarpSet.gsheet");
    const auto correspondence = Read(read, "region_common/datasheets/ui/UiWarpCorrespondenceTable.gsheet");
    const auto aliases = Read(read, "region_common/datasheets/ui/UiMapLevelDefine.gsheet");
    NativeWarpCatalog parsed;
    if (!ParseNativeWarpCatalog(sets, correspondence, parsed) ||
        !ParseNativeWarpLevelAliases(aliases, parsed.level_aliases)) return false;
    out = std::move(parsed);
    return true;
  } catch (...) { return false; }
}
bool ParseNativeWarpLevelAliases(
    std::span<const uint8_t> bytes,
    std::vector<std::pair<std::string, std::string>> &out) {
  out.clear();
  try {
    Sheet s(bytes, 2, 2, 32, 128);
    s.Field(0, "mKey", 4, 4, 0, 16, 16);
    s.Field(1, "mLevelName", 4, 0, 16, 16, 16);
    std::vector<std::pair<std::string, std::string>> parsed;
    std::set<std::string> keys;
    for (size_t i = 0; i < s.count; ++i) {
      const auto row = s.rows + i * 32;
      auto key = s.Ref(row), value = s.Ref(row + 16);
      if (!keys.insert(key).second || key == value) return false;
      parsed.emplace_back(std::move(key), std::move(value));
    }
    // Refuse a malformed alias chain rather than recursively inventing scope.
    for (const auto &[key, value] : parsed) {
      (void)key;
      if (keys.contains(value)) return false;
    }
    out = std::move(parsed);
    return true;
  } catch (...) { return false; }
}
const NativeWarpSet *NativeWarpSetForLevel(const NativeWarpCatalog &catalog,
                                         std::string_view level) {
  const NativeWarpSet *result = nullptr;
  size_t matches{};
  for (const auto &[key, set] : catalog.level_sets)
    if (key == level) {
      ++matches;
      for (const auto &s : catalog.sets)
        if (s.key == set) {
          if (result) return nullptr;
          result = &s;
        }
    }
  return matches == 1 ? result : nullptr;
}
const NativeWarpSet *NativeWarpSetForCurrentLevel(const NativeWarpCatalog &catalog,
                                                std::string_view raw) {
  if (!Symbol(raw)) return nullptr;
  std::string_view resolved = raw;
  size_t matches{};
  for (const auto &[key, value] : catalog.level_aliases)
    if (key == raw) { ++matches; resolved = value; }
  if (matches > 1 || !Symbol(resolved)) return nullptr;
  if (matches) {
    for (const auto &[key, value] : catalog.level_aliases) {
      (void)value;
      if (key == resolved) return nullptr;
    }
  }
  return NativeWarpSetForLevel(catalog, resolved);
}
WarpUnlockSnapshot ReadWarpUnlocks(const ReadFn &read, uint64_t base, bool build,
                                  const MapEpochSource &source,
                                  const NativeWarpSet &set) {
  WarpUnlockSnapshot out;
  if (!Valid(set)) return out;
  std::vector<NativeRiftRegion> queries;
  for (size_t i = 0; i < set.destinations.size(); ++i)
    queries.push_back({uint32_t(i + 1), set.destinations[i].unlock_flag, "", {}});
  const auto result = ReadRiftVisibility(read, base, build, source, queries);
  out.compatible = result.compatible;
  if (!result.known || result.source != source || result.regions.size() != queries.size())
    return out;
  for (size_t i = 0; i < queries.size(); ++i) {
    if (result.regions[i].unique_id != queries[i].unique_id) return {};
    out.unlocked.push_back(result.regions[i].active ? 1 : 0);
  }
  out.known = true; out.source = source;
  return out;
}
} // namespace echoes
