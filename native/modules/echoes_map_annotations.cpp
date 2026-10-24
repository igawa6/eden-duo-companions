// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map_annotations.h"
#include "echoes_catalog.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <unordered_map>
namespace echoes {
namespace {
constexpr size_t Budget = 1024 * 1024;
struct Blob {
  std::span<const uint8_t> b;
  bool Fits(uint64_t p, uint64_t n) const {
    return p <= b.size() && n <= b.size() - p;
  }
  uint16_t U16(size_t p) const {
    return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
  }
  uint32_t U32(size_t p) const { return U16(p) | uint32_t(U16(p + 2)) << 16; }
  uint64_t U64(size_t p) const { return U32(p) | uint64_t(U32(p + 4)) << 32; }
  bool String(uint64_t p, uint64_t n, uint64_t pool, std::string &out) const {
    if (!p && !n) {
      out.clear();
      return true;
    }
    if (p < pool || n > 256 || !Fits(p, n + 1) || b[size_t(p + n)])
      return false;
    out.assign(reinterpret_cast<const char *>(b.data() + p), size_t(n));
    return std::all_of(out.begin(), out.end(),
                       [](unsigned char c) { return c >= 32 && c < 127; });
  }
  bool Ref(size_t p, uint64_t pool, std::string &out) const {
    return Fits(p, 16) && String(U64(p), U64(p + 8), pool, out);
  }
};
struct Field {
  const char *name;
  uint16_t type, offset;
};
struct Sheet {
  Blob blob;
  uint64_t rows{}, pool{};
  uint32_t count{}, stride{};
  bool Init(std::span<const uint8_t> b, std::span<const Field> fields,
            uint32_t expected_top, uint32_t expected_stride) {
    blob = {b};
    constexpr std::array<uint8_t, 8> magic{'g', 's', 'h', 't', 2, 0, 0, 0};
    if (b.size() > Budget || !blob.Fits(0, 48) ||
        !std::equal(magic.begin(), magic.end(), b.begin()) ||
        blob.U32(24) != expected_top || blob.U32(28) != fields.size() ||
        blob.U32(44) != expected_stride || !blob.Fits(48, fields.size() * 48))
      return false;
    rows = blob.U64(32);
    pool = blob.U64(16);
    count = blob.U32(40);
    stride = expected_stride;
    if (!count || count > 1024 || rows < 48 + fields.size() * 48 ||
        !blob.Fits(rows, uint64_t(count) * stride) ||
        pool < rows + uint64_t(count) * stride || !blob.Fits(pool, 1))
      return false;
    for (size_t i = 0; i < fields.size(); ++i) {
      const auto p = 48 + i * 48;
      const auto at = blob.U64(p);
      if (at < pool || !blob.Fits(at, 1))
        return false;
      size_t n = 0;
      while (n < 128 && blob.Fits(at + n, 1) && b[size_t(at + n)])
        ++n;
      std::string name;
      if (!blob.String(at, n, pool, name) || name != fields[i].name ||
          blob.U16(p + 16) != fields[i].type ||
          blob.U16(p + 20) != fields[i].offset)
        return false;
    }
    return true;
  }
};
constexpr std::array<Field, 12> LocationFields{{{"mKey", 4, 0},
                                                {"mVisibleFlag", 4, 16},
                                                {"mIsShowHud", 1, 32},
                                                {"mFieldType", 2, 36},
                                                {"mIsShowRupee", 1, 40},
                                                {"mLabel", 4, 48},
                                                {"mAddtionalLabel", 4, 64},
                                                {"mVisibleCondition", 0, 80},
                                                {"mStrategy", 258, 0},
                                                {"mExpressions", 256, 8},
                                                {"mCategory", 514, 0},
                                                {"mParameter", 516, 8}}};
constexpr std::array<Field, 4> RiftFields{{{"mUniqueId", 2, 0},
                                           {"mVertices", 5, 8},
                                           {"mActivateFlag", 4, 24},
                                           {"mDeactivateFlag", 4, 40}}};
bool Component(std::string_view s) {
  return !s.empty() && s.size() <= 128 &&
         std::all_of(s.begin(), s.end(),
                     [](unsigned char c) {
                       return (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '/';
                     }) &&
         s.front() != '/' && s.back() != '/';
}
} // namespace
bool ParseNativeLocations(std::span<const uint8_t> bytes,
                          std::vector<NativeLocation> &out) {
  out.clear();
  Sheet s;
  if (!s.Init(bytes, LocationFields, 8, 104))
    return false;
  std::vector<NativeLocation> parsed;
  std::set<std::string> keys;
  const auto &b = s.blob;
  for (size_t i = 0; i < s.count; ++i) {
    const size_t a = size_t(s.rows) + i * s.stride;
    NativeLocation v;
    if (!b.Ref(a, s.pool, v.key) || !Component(v.key) ||
        !keys.insert(v.key).second || !b.Ref(a + 16, s.pool, v.visible_flag) ||
        !b.Ref(a + 48, s.pool, v.name_label) ||
        !b.Ref(a + 64, s.pool, v.additional_label) || bytes[a + 32] > 1 ||
        bytes[a + 40] > 1)
      return false;
    v.show_hud = bytes[a + 32];
    v.show_rupees = bytes[a + 40];
    v.field_type = b.U32(a + 36);
    v.condition_strategy = b.U32(a + 80);
    const uint64_t p = b.U64(a + 88), n = b.U64(a + 96);
    if (n > 64 || (p && (p < s.pool || !b.Fits(p, n * 24))) || (n && !p))
      return false;
    for (size_t j = 0; j < n; ++j) {
      const auto at = size_t(p) + j * 24;
      LocationCondition c;
      c.category = b.U32(at);
      if (!b.Ref(at + 8, s.pool, c.parameter))
        return false;
      v.conditions.push_back(std::move(c));
    }
    parsed.push_back(std::move(v));
  }
  out = std::move(parsed);
  return true;
}
bool ParseNativeRiftRegions(std::span<const uint8_t> bytes,
                            std::vector<NativeRiftRegion> &out) {
  out.clear();
  Sheet s;
  if (!s.Init(bytes, RiftFields, 4, 56))
    return false;
  std::vector<NativeRiftRegion> parsed;
  std::set<uint32_t> ids;
  const auto &b = s.blob;
  size_t total_points{};
  for (size_t i = 0; i < s.count; ++i) {
    const size_t a = size_t(s.rows) + i * s.stride;
    NativeRiftRegion v;
    v.unique_id = b.U32(a);
    if (!ids.insert(v.unique_id).second ||
        !b.Ref(a + 24, s.pool, v.activate_flag) ||
        !b.Ref(a + 40, s.pool, v.deactivate_flag))
      return false;
    const auto p = b.U64(a + 8), n = b.U64(a + 16);
    if (p < s.pool || n < 4 || !b.Fits(p, n))
      return false;
    const auto rings = b.U32(size_t(p));
    if (!rings || rings > 32)
      return false;
    uint64_t cursor = p + 4;
    for (size_t j = 0; j < rings; ++j) {
      if (cursor > p + n || p + n - cursor < 4)
        return false;
      const auto count = b.U32(size_t(cursor));
      cursor += 4;
      if (count < 3 || count > 4096 || total_points + count > 32768 ||
          uint64_t(count) * 8 > p + n - cursor)
        return false;
      total_points += count;
      std::vector<std::array<float, 2>> ring;
      for (size_t k = 0; k < count; ++k) {
        std::array<float, 2> point{};
        for (size_t axis = 0; axis < 2; ++axis) {
          point[axis] = std::bit_cast<float>(b.U32(size_t(cursor) + axis * 4));
          if (!std::isfinite(point[axis]) || std::abs(point[axis]) > 1000000)
            return false;
        }
        ring.push_back(point);
        cursor += 8;
      }
      v.rings.push_back(std::move(ring));
    }
    if (cursor != p + n)
      return false;
    parsed.push_back(std::move(v));
  }
  out = std::move(parsed);
  return true;
}
bool LocalizeNativeLocations(const ReadAssetFn &read, std::string_view root,
                             std::vector<NativeLocation> &entries) {
  for (auto &e : entries) {
    e.name.clear();
    e.additional.clear();
  }
  if (!read || !Component(root) || entries.size() > 1024)
    return false;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      banks;
  auto label = [&](const std::string &ref, std::string &out) {
    if (ref.empty())
      return true;
    const auto colon = ref.find(':');
    if (colon == std::string::npos ||
        ref.find(':', colon + 1) != std::string::npos)
      return false;
    const auto file = ref.substr(0, colon), key = ref.substr(colon + 1);
    if (!Component(file) || !Component(key))
      return false;
    if (!banks.contains(file)) {
      if (banks.size() >= 8)
        return false;
      const std::string path = std::string(root) + "/message/" + file + ".msbt";
      const auto n = read(path, 0, nullptr, 0);
      if (!n || n > Budget)
        return false;
      std::vector<uint8_t> data(n);
      std::unordered_map<std::string, std::string> messages;
      if (read(path, 0, data.data(), n) != n ||
          read(path, 0, nullptr, 0) != n ||
          !DecodeNativeMessages(data, messages))
        return false;
      banks.emplace(file, std::move(messages));
    }
    const auto it = banks.at(file).find(key);
    if (it == banks.at(file).end() || it->second.empty())
      return false;
    out = it->second;
    return true;
  };
  auto localized = entries;
  for (auto &e : localized)
    if (!label(e.name_label, e.name) ||
        !label(e.additional_label, e.additional))
      return false;
  entries = std::move(localized);
  return true;
}
} // namespace echoes
