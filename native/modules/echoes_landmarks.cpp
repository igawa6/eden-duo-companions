// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_landmarks.h"
#include "echoes_code_guard.h"
#include <algorithm>
#include <bit>
#include <cmath>
namespace echoes {
namespace {
struct Blob {
  std::span<const uint8_t> b;
  bool Has(uint64_t p, uint64_t n) const {
    return p <= b.size() && n <= b.size() - p;
  }
  uint16_t H(size_t p) const {
    return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
  }
  uint32_t D(size_t p) const { return H(p) | uint32_t(H(p + 2)) << 16; }
  uint64_t Q(size_t p) const { return D(p) | uint64_t(D(p + 4)) << 32; }
  bool String(uint64_t p, std::string &out) const {
    if (!Has(p, 1))
      return false;
    size_t n = 0;
    while (n < 256 && Has(p + n, 1) && b[p + n])
      ++n;
    if (n == 256 || !Has(p + n, 1))
      return false;
    out.assign(reinterpret_cast<const char *>(b.data() + p), n);
    return std::all_of(out.begin(), out.end(),
                       [](unsigned char c) { return c >= 32 && c < 127; });
  }
  bool Text(size_t p, std::string &out) const {
    if (!Has(p, 16))
      return false;
    const auto n = Q(p + 8);
    return n < 256 && Has(Q(p), n + 1) && String(Q(p), out) && out.size() == n;
  }
};
bool Parse(const Blob &b, std::vector<NativeLandmark> &out) {
  constexpr std::array<uint8_t, 8> magic{'g', 's', 'h', 't', 2, 0, 0, 0};
  if (!b.Has(0, 48 + 11 * 48) ||
      !std::equal(magic.begin(), magic.end(), b.b.begin()) ||
      b.D(8) != 0x5f1f3d5d || b.D(24) != 2 || b.D(28) != 11 || b.D(44) != 32)
    return false;
  constexpr std::array<std::string_view, 11> names{
      "mLevelName",   "mIconInfos", "mTextureName", "mVisibleCondition",
      "mLabelName",   "mPosX",      "mPosY",        "mStrategy",
      "mExpressions", "mCategory",  "mParameter"};
  constexpr std::array<std::string_view, 11> types{
      "string",
      "__inline_struct_UiMapIconLandMark_mIconInfos",
      "string",
      "__inline_struct_UiMapIconLandMark_mIconInfos.mVisibleCondition",
      "string",
      "float",
      "float",
      "__inline_enum_UiMapIconLandMark_mIconInfos.mVisibleCondition.mStrategy",
      "__inline_struct_UiMapIconLandMark_mIconInfos.mVisibleCondition."
      "mExpressions",
      "ConditionCategory",
      "string"};
  constexpr std::array<uint8_t, 11> kind{4, 0, 4, 0, 4, 3, 3, 2, 0, 2, 4},
      depth{0, 0, 1, 1, 1, 1, 1, 2, 2, 3, 3};
  constexpr std::array<uint16_t, 11> flags{4, 2, 0, 0, 0, 0, 0, 48, 2, 16, 0},
      offset{0, 16, 0, 16, 40, 56, 60, 0, 8, 0, 8},
      size{16, 16, 16, 24, 16, 4, 4, 4, 16, 4, 16},
      stride{16, 64, 16, 24, 16, 4, 4, 4, 24, 4, 16},
      children{0, 5, 0, 2, 0, 0, 0, 0, 2, 0, 0},
      child{0, 144, 0, 384, 0, 0, 0, 0, 480, 0, 0};
  for (size_t i = 0; i < 11; ++i) {
    const auto p = 48 + i * 48;
    std::string name, type;
    if (!b.String(b.Q(p), name) || name != names[i] ||
        !b.String(b.Q(p + 8), type) || type != types[i] ||
        b.b[p + 16] != kind[i] || b.b[p + 17] != depth[i] ||
        b.H(p + 18) != flags[i] || b.H(p + 20) != offset[i] ||
        b.H(p + 22) != size[i] || b.H(p + 24) != stride[i] ||
        b.H(p + 26) != children[i] || b.Q(p + 32) != child[i])
      return false;
  }
  const auto rows = b.Q(32);
  const auto count = b.D(40);
  if (count > 256 || !b.Has(rows, uint64_t(count) * 32))
    return false;
  for (size_t i = 0; i < count; ++i) {
    const auto p = rows + i * 32;
    std::string level;
    if (!b.Text(p, level) || level.empty())
      return false;
    const auto array = b.Q(p + 16), n = b.Q(p + 24);
    if (n > 256 || n > 256 - out.size() || !b.Has(array, n * 64))
      return false;
    for (size_t j = 0; j < n; ++j) {
      const auto q = array + j * 64;
      NativeLandmark m;
      m.level_key = level;
      if (!b.Text(q, m.texture_name) || m.texture_name.empty() ||
          !b.Text(q + 40, m.label_ref) || m.label_ref.empty())
        return false;
      m.position = {std::bit_cast<float>(b.D(q + 56)),
                    std::bit_cast<float>(b.D(q + 60))};
      if (!std::isfinite(m.position[0]) || !std::isfinite(m.position[1]))
        return false;
      m.condition_strategy = b.D(q + 16);
      const auto ex = b.Q(q + 24), en = b.Q(q + 32);
      if (en > 32 || !b.Has(ex, en * 24))
        return false;
      for (size_t k = 0; k < en; ++k) {
        const auto a = ex + k * 24;
        LocationCondition c;
        c.category = b.D(a);
        if (!b.Text(a + 8, c.parameter))
          return false;
        m.conditions.push_back(std::move(c));
      }
      out.push_back(std::move(m));
    }
  }
  return true;
}
} // namespace
bool ParseNativeLandmarks(std::span<const uint8_t> b,
                          std::vector<NativeLandmark> &out) {
  out.clear();
  std::vector<NativeLandmark> parsed;
  if (!Parse(Blob{b}, parsed))
    return false;
  out = std::move(parsed);
  return true;
}
bool LoadNativeLandmarks(const ReadAssetFn &read,
                         std::vector<NativeLandmark> &out) {
  out.clear();
  if (!read)
    return false;
  constexpr auto path = "region_common/datasheets/ui/UiMapIconLandMark.gsheet";
  const auto n = read(path, 0, nullptr, 0);
  if (!n || n > 1024 * 1024)
    return false;
  std::vector<uint8_t> b(n);
  return read(path, 0, b.data(), n) == n && ParseNativeLandmarks(b, out);
}
LandmarkVisibilitySnapshot
ReadLandmarkVisibility(const ReadFn &read, uint64_t base, bool build,
                       const MapEpochSource &source,
                       std::span<const NativeLandmark> metadata) {
  LandmarkVisibilitySnapshot out;
  if (!read || !build || !base || base % 4096 || metadata.empty() ||
      metadata.size() > 256)
    return out;
  struct NativeGuard {
    uint64_t offset;
    std::array<uint8_t, 32> bytes;
  };
  constexpr std::array<NativeGuard, 2> native_guards{{
      {0x25f8d84,
       {0x09, 0x05, 0x40, 0xf9, 0x00, 0x01, 0x40, 0xb9, 0xe2, 0x03, 0x1f,
        0xaa, 0x3f, 0x01, 0x00, 0xf1, 0x08, 0x11, 0x40, 0xb9, 0xe8, 0x03,
        0x88, 0x9a, 0xe9, 0x23, 0x00, 0xa9, 0x58, 0x1d, 0xbe, 0x97}},
      {0x2616790,
       {0xe8, 0x0a, 0x40, 0xf9, 0xba, 0xbe, 0x40, 0xf9, 0x03, 0x39, 0x40,
        0xbd, 0x08, 0xf1, 0x00, 0x91, 0x49, 0x2f, 0x42, 0x79, 0x03, 0x81,
        0x40, 0x4d, 0xa3, 0x03, 0x9f, 0x3c, 0x89, 0x0c, 0x00, 0xb4}},
  }};
  for (const auto &g : native_guards) {
    std::array<uint8_t, 32> bytes{};
    if (g.offset > UINT64_MAX - base ||
        !read(base + g.offset, bytes.data(), bytes.size()) ||
        !MatchSupportedGuard(bytes, g.bytes, g.offset))
      return out;
  }

  std::vector<NativeRiftRegion> flags;
  for (size_t i = 0; i < metadata.size(); ++i) {
    const auto &m = metadata[i];
    if (m.level_key.empty() || m.texture_name.empty() || m.label_ref.empty() ||
        !std::isfinite(m.position[0]) || !std::isfinite(m.position[1]) ||
        m.condition_strategy != 0 || m.conditions.size() != 1 ||
        m.conditions[0].category != 1 || m.conditions[0].parameter.empty())
      return out;
    flags.push_back({uint32_t(i + 1), m.conditions[0].parameter, "", {}});
  }
  const auto state = ReadRiftVisibility(read, base, build, source, flags);
  out.compatible = state.compatible;
  if (!state.known || state.source != source ||
      state.regions.size() != metadata.size())
    return out;
  for (size_t i = 0; i < metadata.size(); ++i) {
    if (state.regions[i].unique_id != i + 1)
      return LandmarkVisibilitySnapshot{};
    out.visible.push_back(state.regions[i].active ? 1 : 0);
  }
  out.source = source;
  out.known = true;
  return out;
}
} // namespace echoes
