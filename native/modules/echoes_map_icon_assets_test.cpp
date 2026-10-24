// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "acnh_tex.h"
#include "echoes_map_icon_assets.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#ifdef ECHOES_TEST_NATIVE_ASTC
#include "video_core/textures/astc.h"
#endif
namespace {
int checks{};
void Check(bool ok, const char *what) {
  ++checks;
  if (!ok)
    throw std::runtime_error(what);
}
uint16_t U16(const uint8_t *p) { return uint16_t(p[0] | uint16_t(p[1]) << 8); }
uint32_t U32(const uint8_t *p) {
  return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
struct Fixtures {
  std::filesystem::path root;
  std::map<std::string, std::vector<uint8_t>> archives;
  size_t Read(std::string_view path, uint64_t at, void *out, size_t n) {
    const auto split = path.find('#');
    if (split == path.npos)
      return 0;
    auto archive = std::string(path.substr(0, split));
    auto member = path.substr(split + 1);
    auto it = archives.find(archive);
    if (it == archives.end()) {
      auto file = root / archive;
      std::ifstream f(file, std::ios::binary | std::ios::ate);
      if (!f)
        return 0;
      auto size = f.tellg();
      if (size < 20 || size > std::streamoff(64u << 20))
        return 0;
      std::vector<uint8_t> bytes(static_cast<size_t>(size));
      f.seekg(0);
      f.read(reinterpret_cast<char *>(bytes.data()), size);
      if (!f)
        return 0;
      it = archives.emplace(archive, std::move(bytes)).first;
    }
    auto &b = it->second;
    if (std::memcmp(b.data(), "SARC", 4) || U16(b.data() + 6) != 0xfeff)
      return 0;
    size_t data = U32(b.data() + 12), fat = U16(b.data() + 4);
    if (fat + 12 > b.size())
      return 0;
    size_t count = U16(b.data() + fat + 6);
    if (count > 30000 || fat + 12 + count * 16 + 8 > data || data > b.size())
      return 0;
    size_t names = fat + 12 + count * 16 + 8;
    for (size_t i = 0; i < count; ++i) {
      auto p = b.data() + fat + 12 + i * 16;
      auto attr = U32(p + 4);
      size_t a = U32(p + 8), end = U32(p + 12),
             name = names + size_t(attr & 0xffffff) * 4;
      if (!(attr >> 24) || name >= data || end < a || end > b.size() - data)
        continue;
      size_t z = name;
      while (z < data && b[z])
        ++z;
      if (z == data)
        continue;
      if (std::string_view(reinterpret_cast<char *>(b.data() + name),
                           z - name) != member)
        continue;
      size_t size = end - a;
      if (!out && !n)
        return size;
      if (at > size || n > size - at)
        return 0;
      std::memcpy(out, b.data() + data + a + at, n);
      return n;
    }
    return 0;
  }
};
#ifdef ECHOES_TEST_NATIVE_ASTC
EdenDsmodBool Decode(void *, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                     const uint8_t *in, size_t ni, uint8_t *out, size_t no) {
  if (no != uint64_t(w) * h * 4)
    return EDEN_DSMOD_FALSE;
  Tegra::Texture::ASTC::Decompress({in, ni}, w, h, 1, bw, bh, {out, no});
  return EDEN_DSMOD_TRUE;
}
#endif
void Save(const char *name, const echoes::Image &im) {
  if (const char *root = std::getenv("ECHOES_MAP_ICON_OUTPUT")) {
    std::filesystem::create_directories(root);
    std::ofstream f(std::filesystem::path(root) / (std::string(name) + ".rgba"),
                    std::ios::binary);
    f.write(reinterpret_cast<const char *>(im.rgba.data()),
            static_cast<std::streamsize>(im.rgba.size()));
  }
}
} // namespace
int main() {
  try {
    double v = 9;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Check(!echoes::EvaluateMapIconCurve({}, 0, v) && v == 0,
          "empty curve refused");
    const std::array<echoes::MapIconCurveKey, 2> linear{
        {{0, 0, 1}, {10, 10, 1}}};
    Check(echoes::EvaluateMapIconCurve(linear, 5, v) && std::abs(v - 5) < 1e-9,
          "Hermite midpoint with native slopes");
    Check(echoes::EvaluateMapIconCurve(linear, -1, v) && v == 0,
          "pre-key clamp");
    Check(echoes::EvaluateMapIconCurve(linear, 11, v) && v == 10,
          "post-key clamp");
    const std::array<echoes::MapIconCurveKey, 2> ease{{{0, 0, 0}, {10, 10, 0}}};
    Check(echoes::EvaluateMapIconCurve(ease, 2.5, v) &&
              std::abs(v - 1.5625) < 1e-9,
          "nonlinear native zero-slope Hermite");
    const std::array<echoes::MapIconCurveKey, 3> discontinuity{
        {{0, 1, 0}, {3, 2, 0}, {3, 4, 0}}};
    Check(echoes::EvaluateMapIconCurve(discontinuity, 3, v) && v == 4,
          "duplicate-frame right-hand native discontinuity");
    const std::array<echoes::MapIconCurveKey, 2> unordered{
        {{10, 1, 0}, {0, 4, 0}}};
    Check(!echoes::EvaluateMapIconCurve(unordered, 3, v),
          "unsorted curve refused");
    Check(!echoes::EvaluateMapIconCurve(linear, nan, v),
          "nonfinite frame refused");
    echoes::MapIconAssets empty(
        [](std::string_view, uint64_t, void *, size_t) { return size_t(0); });
    echoes::Image out(1, 1);
    echoes::MapIconRenderInfo info;
    Check(!empty.Checkpoint(0, 64, out, &info) && out.Empty() &&
              !info.original_sources,
          "missing original sources clear output");
    Check(!empty.Smoothie(64, out) && out.Empty(),
          "missing smoothie source refused");
    Check(!empty.Checkpoint(nan, 64, out) && out.Empty(), "NaN render refused");
    Check(!empty.Checkpoint(-1, 64, out), "negative render refused");
    Check(!empty.Checkpoint(0, 39, out), "too small refused");
    Check(!empty.Checkpoint(0, 257, out), "oversized output refused");
    echoes::MapIconAssets oversized(
        [](std::string_view, uint64_t, void *, size_t) {
          return size_t(256u << 10) + 1;
        });
    Check(!oversized.Checkpoint(0, 64, out), "metadata size bound");
    echoes::MapIconAssets bad(
        [](std::string_view, uint64_t, void *p, size_t n) {
          if (p)
            std::memset(p, 0, n);
          return p ? n : size_t(64);
        });
    Check(!bad.Checkpoint(0, 64, out), "malformed layout refused");
    echoes::MapIconAssets shortread(
        [](std::string_view, uint64_t, void *p, size_t) {
          return p ? size_t(3) : size_t(64);
        });
    Check(!shortread.Checkpoint(0, 64, out), "short original read refused");
    Check(!empty.Chest(false, 64, out) && out.Empty(),
          "missing original chest refused");
    Check(!empty.Chest(true, 39, out), "small chest refused");
    Check(!empty.Chest(false, 257, out), "oversized chest refused");
    const char *root = std::getenv("ECHOES_ROMFS");
    if (!root) {
      std::cout
          << checks
          << " checks passed; original fixtures skipped (set ECHOES_ROMFS)\n";
      return 0;
    }
#ifdef ECHOES_TEST_NATIVE_ASTC
    acnh::SetAstcDecoder(nullptr, Decode);
#endif
    Fixtures fixtures{root, {}};
    auto read = [&](std::string_view p, uint64_t at, void *o, size_t n) {
      return fixtures.Read(p, at, o, n);
    };
    echoes::MapIconAssets assets(read);
    echoes::Image first, next, wrapped;
    Check(assets.Checkpoint(0, 128, first, &info), "native checkpoint0 render");
    Check(info.original_sources && info.native_loop_frames == 700 &&
              info.native_tracks == 53,
          "actual native Loop identity");
    Check(!info.shader_parity && info.omitted_complex_panes == 4,
          "subset shader limitations explicit");
    Check(assets.Checkpoint(150, 128, next), "native checkpoint150 render");
    Check(first.rgba != next.rgba,
          "original SRT tracks change actual rendered native glow");
    Check(assets.Checkpoint(700, 128, wrapped) && first.rgba == wrapped.rgba,
          "native700frame loop wraps exactly");
    Check(std::any_of(first.rgba.begin() + 3, first.rgba.end(),
                      [](uint8_t c) { return c != 0; }),
          "native circle visible");
    Save("checkpoint-0-128", first);
    Save("checkpoint-150-128", next);
    Check(assets.Smoothie(64, out),
          "native smoothie constructor-selected icon");
    Check(out.w == 64 && out.h == 64,
          "uniform aspect preserving smoothie output");
    Save("smoothie-64", out);
    echoes::Image closed_chest, open_chest;
    Check(assets.Chest(false, 64, closed_chest), "native TreasureBox icon");
    Check(assets.Chest(true, 64, open_chest), "native TreasureBoxOpen icon");
    Check(closed_chest.w == 64 && closed_chest.h == 64 && open_chest.w == 64 &&
              open_chest.h == 64,
          "original chest states preserve native square aspect");
    Check(closed_chest.rgba != open_chest.rgba,
          "distinct original opened/closed artwork");
    Save("chest-closed-64", closed_chest);
    Save("chest-open-64", open_chest);
    Check(!assets.Landmark("../City", 64, out) && out.Empty(),
          "landmark path-like symbol refused");
    Check(!assets.Landmark("MissingNativeIcon", 64, out) && out.Empty(),
          "missing original landmark has no fallback");
    for (auto name : {"City", "Shop", "SDungeon", "Luberi", "MachineEngineer",
                      "Smoothie", "GreatFairy"}) {
      Check(assets.Landmark(name, 64, out),
            "original discovery landmark texture");
      Check(out.w == 64 && out.h == 64, "original landmark square aspect");
      Save(name, out);
    }
    auto corrupt = [&](std::string_view p, uint64_t at, void *o, size_t n) {
      size_t result = fixtures.Read(p, at, o, n);
      if (o && n && p.find("Loop.bflan") != p.npos)
        static_cast<uint8_t *>(o)[0] = '?';
      return result;
    };
    echoes::MapIconAssets malformed_loop(corrupt);
    Check(!malformed_loop.Checkpoint(0, 64, out) && out.Empty(),
          "malformed native Loop refused");
    std::cout
        << checks
        << " checks passed; original layout/Loop/texture fixtures rendered\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "map icon test: " << e.what() << "\n";
    return 1;
  }
}
