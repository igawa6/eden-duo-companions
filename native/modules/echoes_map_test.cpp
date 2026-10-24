// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_map.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_map>

namespace {
constexpr uint64_t Base = 0x8070a000, Access = 0x2107000000;
constexpr uint64_t Save = 0x2107100000, List = 0x2107200000;
constexpr uint64_t Records = 0x2107300000, Registry = 0x2107400000;
constexpr uint64_t Header = 0x2107500000, Entries = 0x2107600000;
constexpr uint64_t Strings = 0x2107700000;
constexpr uint64_t Controller = 0x2107800000, Handle = 0x2107900000;
constexpr uint64_t Resource = 0x2107a00000, LevelName = 0x2107b00001;
size_t cases{};
void Check(bool value, const char* message) {
  if (!value) { std::cerr << "FAIL after " << cases << ": " << message << '\n'; std::exit(1); }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  size_t reads{}, largest{};
  void Put(uint64_t at, uint64_t value, size_t n = 8) {
    for (size_t i = 0; i < n; ++i) bytes[at + i] = uint8_t(value >> (i * 8));
  }
  void Zero(uint64_t at, size_t n) {
    for (size_t i = 0; i < n; ++i) bytes[at + i] = 0;
  }
  void Key(size_t i, const std::string& key) {
    Put(Entries + i * 16, Strings + i * 64);
    Put(Entries + i * 16 + 8, key.size(), 4);
    for (size_t j = 0; j < key.size(); ++j) bytes[Strings + i * 64 + j] = uint8_t(key[j]);
  }
  Memory() {
    for (const auto& g : echoes::IdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i) bytes[Base + g.offset + i] = g.bytes[i];
    const std::array<std::pair<uint64_t, std::array<uint8_t, 16>>, 3> guards{{
      {0x1688dcc, {0x36,0x89,0x40,0xb9,0x48,0x03,0x00,0x54,0xdf,0xfe,0x07,0x71,0x08,0x03,0x00,0x54}},
      {0x2386de0, {0xfd,0x7b,0xba,0xa9,0xfb,0x0b,0x00,0xf9,0xfa,0x67,0x02,0xa9,0xf8,0x5f,0x03,0xa9}},
      {0x261c890, {0x7f,0x7f,0x00,0x71,0x88,0xff,0xff,0x54,0x48,0x03,0x40,0xf9,0xa9,0x7e,0x43,0xd3}},
    }};
    for (const auto& [offset, value] : guards)
      for (size_t i = 0; i < value.size(); ++i) bytes[Base + offset + i] = value[i];
    Put(Base + 0x34bc848, Access); Put(Access + 0x10, Save);
    Put(Save + 0x16cf8, List); Zero(List, 0x20); Put(List + 0x18, 7, 4);
    Put(Save + 0x16b80, Records); Zero(Records, 32 * 0xd0);
    Put(Base + 0x34b80b8, Registry); Put(Registry + 0x78, Header);
    Zero(Header, 0x30); Put(Header + 0x20, Entries); Put(Header + 0x28, 28, 1);
    Zero(Entries, 28 * 16);
    for (size_t i = 0; i < 28; ++i)
      Key(i, i == 0 ? "Field" : i == 12 ? "" : "Map_" + std::to_string(i));
    // Deliberately differ across maps and bitplanes, including native edge511.
    Put(Records + 0x30, 0x81, 1); Put(Records + 0x70, 1, 1);
    Put(Records + 0xd0 + 0x30 + 63, 0x80, 1);
  }
  void Level(std::string key = "DekuTreeArea") {
    const std::array<std::pair<uint64_t, std::array<uint8_t, 16>>, 3> guards{{
      {0x360870, {0xfd,0x7b,0xbd,0xa9,0xf5,0x0b,0x00,0xf9,0xf4,0x4f,0x02,0xa9,0xfd,0x03,0x00,0x91}},
      {0x165b4c4, {0x68,0xf2,0x00,0xd0,0x08,0xe1,0x04,0x91,0x3f,0x00,0x08,0xeb,0x40,0x01,0x00,0x54}},
      {0x16659e8, {0x08,0xe0,0x00,0xf0,0x08,0xe1,0x1c,0x91,0x00,0x27,0x80,0x52,0xc8,0x02,0x00,0xf9}},
    }};
    for (const auto& [offset, value] : guards)
      for (size_t i = 0; i < value.size(); ++i) bytes[Base + offset + i] = value[i];
    Put(Base + 0x34bd718, Controller); Put(Controller, Base + 0x3269630);
    Put(Controller + 0x80, Handle); Put(Handle, Base + 0x3268098);
    Put(Handle + 0x90, Resource); Put(Resource, Base + 0x3268738);
    Put(Resource + 0x140, LevelName); Put(Resource + 0x148, key.size());
    for (size_t i = 0; i < key.size(); ++i) bytes[LevelName + i] = uint8_t(key[i]);
  }
  bool Read(uint64_t at, void* out, size_t n) {
    ++reads; largest = std::max(largest, n);
    const auto visit = ++visits[at];
    if (before) before(at, visit);
    if (!at || n > std::numeric_limits<uint64_t>::max() - at) return false;
    auto* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
      const auto it = bytes.find(at + i);
      if (it == bytes.end()) return false;
      p[i] = it->second;
    }
    return true;
  }
  echoes::MapSnapshot Sample(bool build = true, uint64_t base = Base) {
    return echoes::ReadMapSnapshot([&](auto a, auto p, auto n) { return Read(a, p, n); }, base, build);
  }
};
void Empty(const echoes::MapSnapshot& s) {
  Check(!s.discovery_valid && !s.current_level_valid && !s.marker_valid &&
        s.current_level_source == echoes::MapLevelSource{} && s.current_level_key.empty() &&
        s.source == echoes::MapEpochSource{} && s.map_count == 0 &&
        std::all_of(s.maps.begin(), s.maps.end(), [](const auto& m) {
          return m.key.empty() && std::all_of(m.discovered.begin(), m.discovered.end(),
                 [](auto b) { return b == 0; }) &&
                 std::all_of(m.presented.begin(), m.presented.end(), [](auto b) { return b == 0; });
        }), "invalid read must clear all discovery and source ownership");
}
void NoLevel(const echoes::MapSnapshot& s) {
  Check(s.discovery_valid && !s.current_level_valid && !s.marker_valid &&
        s.current_level_key.empty() && s.current_level_source == echoes::MapLevelSource{},
        "optional bad level must clear level ownership without erasing independent discovery");
}
} // namespace
int main() {
  {
    Memory m; auto s = m.Sample();
    Check(s.compatible && s.discovery_valid && s.map_count == 28 &&
          s.maps[0].key == "Field" && s.maps[12].key.empty() &&
          s.maps[0].Discovered(0) && s.maps[0].Discovered(7) &&
          !s.maps[0].Discovered(1) && s.maps[0].Presented(0) &&
          !s.maps[0].Presented(7) && s.maps[1].Discovered(511) &&
          !s.maps[1].Discovered(512) && !s.maps[1].Discovered(SIZE_MAX) &&
          !s.maps[1].Presented(SIZE_MAX), "native index/bit/plane boundaries");
    Check(s.source.save_epoch.save_data == Save && s.source.traversal == Records &&
          s.source.registry_entries == Entries && !s.current_level_valid && !s.marker_valid,
          "independent discovery never invents gameplay or player projection");
    Check(m.largest == 6656 && m.reads < 400, "bounded reader call and byte budgets");
    m.bytes.erase(Records); Empty(m.Sample()); // A fresh failure must not reuse s.
    ++cases;
  }
  {
    Memory m;
    Check(!m.Sample(false).compatible && m.reads == 0, "wrong build performs no reads");
    Check(!m.Sample(true, 0).compatible && m.reads == 0, "null base performs no reads");
    Check(!m.Sample(true, UINT64_MAX - 7).compatible && m.reads == 0,
          "overflow base performs no reads");
    Check(!echoes::ReadMapSnapshot({}, Base, true).compatible, "null callback refused"); ++cases;
  }
  for (auto offset : {uint64_t(0x1688dcc), uint64_t(0x2386de0), uint64_t(0x261c890)}) {
    Memory m; m.bytes[Base + offset] ^= 1;
    Check(!m.Sample().compatible, "original map instruction mismatch rejected"); ++cases;
  }
  for (auto at : {Base + 0x34bc848, Access + 0x10, Save + 0x16b80,
                 Base + 0x34b80b8, Registry + 0x78, Header + 0x20}) {
    for (auto value : {uint64_t(0), uint64_t(1), UINT64_MAX - 7}) {
      Memory m; m.Put(at, value); Empty(m.Sample()); ++cases;
    }
  }
  for (auto count : {0, 27, 29, 32, 255}) {
    Memory m; m.Put(Header + 0x28, count, 1); Empty(m.Sample()); ++cases;
  }
  for (auto length : {uint64_t(0), uint64_t(64), uint64_t(UINT32_MAX)}) {
    Memory m; m.Put(Entries + 16 + 8, length, 4); Empty(m.Sample()); ++cases;
  }
  {
    Memory m; m.Key(12, "Unexpected"); Empty(m.Sample()); ++cases;
    Memory duplicate; duplicate.Key(1, "Field"); Empty(duplicate.Sample()); ++cases;
    Memory wrong; wrong.Key(0, "Other"); Empty(wrong.Sample()); ++cases;
    Memory malformed; malformed.Key(1, "Map\n"); Empty(malformed.Sample()); ++cases;
  }
  for (auto visit : {size_t(2), size_t(3)}) {
    for (auto target : {Records + 0x30, Records + 0x70}) {
      Memory m; m.before = [&](uint64_t at, size_t v) {
        if (at == Records && v == visit) m.bytes[target] ^= 2;
      };
      Empty(m.Sample()); ++cases;
    }
    Memory epoch; epoch.before = [&](uint64_t at, size_t v) {
      if (at == Records && v == visit) epoch.Put(List + 0x18, 8, 4);
    };
    Empty(epoch.Sample()); ++cases;
    Memory owner; owner.before = [&](uint64_t at, size_t v) {
      if (at == Records && v == visit) owner.Put(Access + 0x10, Save + 8);
    };
    Empty(owner.Sample()); ++cases;
    Memory registry; registry.before = [&](uint64_t at, size_t v) {
      if (at == Records && v == visit) registry.Put(Base + 0x34b80b8, Registry + 8);
    };
    Empty(registry.Sample()); ++cases;
  }
  {
    Memory m; m.before = [&](uint64_t at, size_t v) {
      if (at == Entries && v == 2) m.Key(1, "Changed");
    };
    Empty(m.Sample()); ++cases;
  }
  for (auto target : {Header + 0x28, Entries + 16 + 8, Strings + 64}) {
    Memory m; m.before = [&](uint64_t at, size_t v) {
      if (at == Records && v == 3) m.bytes[target] ^= 1;
    };
    Empty(m.Sample()); ++cases;
  }
  for (auto at : {Records + 6655, Entries + 447, Header + 47, Strings + 64}) {
    Memory m; m.bytes.erase(at); Empty(m.Sample()); ++cases;
  }
  {
    Memory m; m.Level(); const auto s = m.Sample();
    Check(s.discovery_valid && s.current_level_valid && !s.marker_valid &&
          s.current_level_key == "DekuTreeArea" &&
          s.current_level_source == echoes::MapLevelSource{Controller, Handle, Resource, LevelName},
          "raw typed native level is optional resource metadata, never a Field alias or marker");
    Check(m.reads < 500 && m.largest == 6656, "level read remains bounded");
    m.bytes.erase(LevelName); NoLevel(m.Sample()); ++cases;
  }
  for (auto at : {Base + 0x34bd718, Controller + 0x80, Handle + 0x90}) {
    for (auto value : {uint64_t(0), uint64_t(1), UINT64_MAX - 7}) {
      Memory m; m.Level(); m.Put(at, value); NoLevel(m.Sample()); ++cases;
    }
  }
  for (auto at : {Controller, Handle, Resource}) {
    Memory m; m.Level(); m.Put(at, Base + 0x3268000); NoLevel(m.Sample()); ++cases;
  }
  for (auto offset : {uint64_t(0x360870), uint64_t(0x165b4c4), uint64_t(0x16659e8)}) {
    Memory m; m.Level(); m.bytes[Base + offset + 15] ^= 1;
    NoLevel(m.Sample()); ++cases;
  }
  for (auto length : {uint64_t(0), uint64_t(64), UINT64_MAX}) {
    Memory m; m.Level(); m.Put(Resource + 0x148, length); NoLevel(m.Sample()); ++cases;
  }
  {
    Memory m; m.Level("Area\n"); NoLevel(m.Sample()); ++cases;
    Memory unbounded; unbounded.Level(); unbounded.Put(Resource + 0x140, UINT64_MAX - 7);
    NoLevel(unbounded.Sample()); ++cases;
  }
  for (auto visit : {size_t(2), size_t(3)}) {
    for (auto at : {Base + 0x34bd718, Controller + 0x80, Handle + 0x90,
                   Controller, Handle, Resource, Resource + 0x140, Resource + 0x148}) {
      Memory m; m.Level(); m.before = [&](uint64_t p, size_t v) {
        if (p == LevelName && v == visit) m.Put(at, 0);
      };
      NoLevel(m.Sample()); ++cases;
    }
    Memory text; text.Level(); text.before = [&](uint64_t p, size_t v) {
      if (p == LevelName && v == visit) text.bytes[LevelName] = 'X';
    };
    NoLevel(text.Sample()); ++cases;
    Memory epoch; epoch.Level(); epoch.before = [&](uint64_t p, size_t v) {
      if (p == LevelName && v == visit) epoch.Put(List + 0x18, 8, 4);
    };
    Empty(epoch.Sample()); ++cases;
  }
  std::cout << "echoes_map: " << cases << " cases passed\n";
}
