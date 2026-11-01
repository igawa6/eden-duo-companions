// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_exploration.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>
namespace {
int failures{};
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #x);              \
      ++failures;                                                              \
    }                                                                          \
  } while (false)
constexpr uint64_t Main = 0x80000000, App = 0x1400000000, Mini = 0x1400001000,
                   Manager = 0x1400002000, Save = 0x1400003000,
                   Resource = 0x1400005000, Volume = 0x1400006000,
                   Tex = 0x1400007000, Package = 0x1400008000,
                   Pool = 0x1400010000, Nodes = 0x1400009000,
                   Pairs = 0x140000a000, Controller = 0x140000b000,
                   Character = 0x140000c000;
struct Memory {
  std::map<uint64_t, uint8_t> data;
  unsigned reads{};
  uint32_t next_name = 0x100;
  template <class T> void Put(uint64_t at, const T &v) {
    auto b = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof v; ++i)
      data[at + i] = b[i];
  }
  void Zero(uint64_t at, size_t n) {
    for (size_t i = 0; i < n; ++i)
      data[at + i] = 0;
  }
  bool Read(uint64_t at, void *output, size_t n) {
    ++reads;
    auto *out = static_cast<uint8_t *>(output);
    for (size_t i = 0; i < n; ++i) {
      auto p = data.find(at + i);
      if (p == data.end())
        return false;
      out[i] = p->second;
    }
    return true;
  }
  uint32_t Name(const std::string &name) {
    auto id = next_name;
    next_name += uint32_t((name.size() + 3) / 2 + 1);
    Put(Pool + 2 * id, uint16_t(name.size() << 6));
    for (size_t i = 0; i < name.size(); ++i)
      Put(Pool + 2 * id + 2 + i, uint8_t(name[i]));
    return id;
  }
  ot0::ExplorationSnapshot Snapshot(uint64_t main = Main, int64_t delta = 0) {
    return ot0::ReadExploration(
        [&](auto at, void *out, size_t n) { return Read(at, out, n); }, main,
        delta);
  }
  Memory() {
    Put(Main + 0x7cc8cc0, App);
    Put(App, Main + 0x6207610);
    Put(App + 0x2f0, std::array<uint8_t, 3>{23, 25, 1});
    Put(Main + 0x7cc7588, Mini);
    Zero(Mini, 0x9d0);
    Put(Mini, Main + 0x63be0f0);
    Put(Main + 0x7cc8540, Manager);
    Put(Manager + 0x2c8, Save);
    Put(Main + 0x6c91440 + 0x38, Pool);
    Put(Main + 0x7cc7640, ot0::MapPoint{1, -1});
    Put(Main + 0x7cbbcd0, uint64_t(0x1400020000));
    Put(Mini + 0x348, uint32_t(81));
    Put(Mini + 0x34c, Name("Map_Test"));
    Put(Mini + 0x358, std::array<double, 3>{1000, -20000, 595});
    Put(Mini + 0x448, Resource);
    Zero(Resource, 0x110);
    auto section = Name("Room");
    Put(Resource, uint64_t(section));
    Put(Resource + 0x98, ot0::MapPoint{21000, 1900});
    Put(Resource + 0xa8, ot0::MapPoint{0, 0});
    Put(Resource + 0xb8, ot0::MapPoint{.25, .25});
    Put(Resource + 0xc8, std::array<double, 3>{-2000, -21000, -600});
    Put(Resource + 0xe0, std::array<double, 3>{2000, -17000, 1400});
    Put(Resource + 0xf8, uint8_t(1));
    Put(Resource + 0x100, Volume);
    Zero(Volume, 0x370);
    Put(Volume + 0x10, uint64_t(0x1400020000));
    Put(Volume + 0x18, uint64_t(section));
    Put(Volume + 0x2c9, uint8_t(1));
    constexpr uint64_t Mask = 0x1400030000, MaskPackage = Mask + 0x100;
    Zero(Mask, 0x28);
    Zero(MaskPackage, 0x28);
    Put(Mask + 0x18, Name("T_mask"));
    Put(Mask + 0x20, MaskPackage);
    Put(MaskPackage + 0x18, Name("/Game/UI/MiniMap/BP/T_mask"));
    Put(Mini + 0x7e0, Mask);
    Put(Volume + 0x2f8, int32_t(6144));
    Put(Volume + 0x310, int32_t(1050));
    Put(Volume + 0x314, float(3));
    Put(Volume + 0x2d8, Tex);
    Zero(Tex, 0x28);
    Zero(Package, 0x28);
    Put(Tex + 0x18, Name("Map_Test_Path"));
    Put(Tex + 0x20, Package);
    Put(Package + 0x18,
        Name("/Game/UI/MiniMap/Texture/MAP/Test/Map_Test/Map_Test_Path"));
    Zero(Save + 0xd18, 0x58);
    Put(Save + 0xd18, Nodes);
    Put(Save + 0xd20, int32_t(1));
    Put(Save + 0xd24, int32_t(1));
    Put(Save + 0xd28, uint32_t(1));
    Put(Save + 0xd40, int32_t(1));
    Put(Save + 0xd44, int32_t(128));
    Put(Save + 0xd48, int32_t(-1));
    Put(Save + 0xd60, int32_t(1));
    Put(Save + 0xd68, int32_t(200));
    Zero(Nodes, 0x28);
    Put(Nodes, uint32_t(81));
    Put(Nodes + 4, uint64_t(section));
    Put(Nodes + 0x10, Pairs);
    Put(Nodes + 0x18, int32_t(1));
    Put(Nodes + 0x1c, int32_t(1));
    Put(Nodes + 0x20, int32_t(-1));
    Put(Pairs, ot0::MapCell{4, 4});
    Put(Main + 0x7cc6288, Controller);
    Put(Controller + 0x290, Character);
    Zero(Character, 0x28);
    Put(Character, Main + 0x6416cf0);
    Put(Character + 0x920, uint8_t(3));
  }
};
void Synthetic() {
  Memory m;
  auto s = m.Snapshot();
  CHECK(s.valid);
  CHECK(s.map_key == "Map_Test" && s.section_key == "Room");
  CHECK(
      s.texture_path ==
      "/Game/UI/MiniMap/Texture/MAP/Test/Map_Test/Map_Test_Path.Map_Test_Path");
  CHECK(s.texture_position == ot0::MapPoint({250, 225}));
  CHECK(s.heading_valid && s.native_direction == 3);
  CHECK(ot0::TextureToWorld(s, 250, 225) == ot0::MapPoint({1000, -20000}));
  CHECK(ot0::WorldCell(s, 1000, -20000) == ot0::MapCell({15, 5}));
  CHECK(s.revealed_cells.size() == 1);
  for (int64_t delta : {int64_t(0x10000000000), int64_t(-0x1000)}) {
    Memory shifted;
    shifted.Put(App, Main + 0x6207610 + static_cast<uint64_t>(delta));
    shifted.Put(Mini, Main + 0x63be0f0 + static_cast<uint64_t>(delta));
    shifted.Put(Character, Main + 0x6416cf0 + static_cast<uint64_t>(delta));
    CHECK(!shifted.Snapshot().valid);
    CHECK(shifted.Snapshot(Main, delta) == s);
    shifted.Put(Character, Main + 0x6416cf8 + static_cast<uint64_t>(delta));
    CHECK(!shifted.Snapshot(Main, delta).valid);
    Memory original;
    CHECK(original.Snapshot(Main, delta) == s);
  }
  {
    Memory renamed;
    constexpr uint64_t Override = 0x1400040000;
    renamed.Put(Volume + 0x1c, uint32_t(2));
    CHECK(!renamed.Snapshot()
               .valid); // Without override, exact FName still matters.
    renamed.Put(Volume + 0x338, Override);
    renamed.Put(Volume + 0x340, int32_t(5));
    renamed.Put(Volume + 0x344, int32_t(8));
    renamed.Put(Override, std::array<uint16_t, 5>{'R', 'o', 'o', 'm', 0});
    CHECK(renamed.Snapshot() == s);
    // A custom logical name is independent of the actor's generated name,
    // not merely an instruction to ignore its FName instance number.
    renamed.Put(Volume + 0x18, renamed.Name("SpawnedBounds"));
    CHECK(renamed.Snapshot() == s);
    renamed.Put(Override, std::array<uint16_t, 5>{'R', 'O', 'O', 'M', 0});
    CHECK(renamed.Snapshot() == s);
    renamed.Put(Override, std::array<uint16_t, 5>{'N', 'o', 'p', 'e', 0});
    CHECK(!renamed.Snapshot().valid);
    renamed.Put(Override, std::array<uint16_t, 5>{'R', 'o', 'o', 'm', 'x'});
    CHECK(!renamed.Snapshot().valid); // Missing terminator.
    renamed.Put(Override, std::array<uint16_t, 5>{'R', 0xd800, 'o', 'm', 0});
    CHECK(!renamed.Snapshot().valid); // Broken UTF-16.
    renamed.Put(Override, std::array<uint16_t, 5>{'R', 'o', 'o', 'm', 0});
    renamed.Put(Volume + 0x344, int32_t(4));
    CHECK(!renamed.Snapshot().valid);
    renamed.Put(Volume + 0x344, int32_t(8));
    int strings = 0;
    const auto changing = [&](uint64_t at, void *out, size_t n) {
      if (at == Override && ++strings == 2)
        renamed.Put(Override, std::array<uint16_t, 5>{'N', 'o', 'p', 'e', 0});
      return renamed.Read(at, out, n);
    };
    CHECK(!ot0::ReadExploration(changing, Main).valid);
  }
  CHECK(s.filter_on);
  m.Put(Volume + 0x2c9, uint8_t(0));
  CHECK(m.Snapshot().valid && !m.Snapshot().filter_on);
  m.Put(Volume + 0x2c9, uint8_t(2));
  CHECK(!m.Snapshot().valid);
  m.Put(Volume + 0x2c9, uint8_t(1));
  constexpr uint64_t Mask = 0x1400030000, MaskPackage = Mask + 0x100;
  m.Zero(Mask, 0x28);
  m.Zero(MaskPackage, 0x28);
  m.Put(Mask + 0x18, m.Name("T_mask"));
  m.Put(Mask + 0x20, MaskPackage);
  m.Put(MaskPackage + 0x18, m.Name("/Game/UI/MiniMap/BP/T_mask"));
  m.Put(Mini + 0x7e0, Mask);
  m.Put(Volume + 0x2f8, int32_t(6144));
  m.Put(Volume + 0x310, int32_t(1050));
  m.Put(Volume + 0x314, float(3));
  auto stamped = m.Snapshot();
  CHECK(ot0::NativeTexturePoint(stamped, 0, -19000, 1024, 512) ==
        ot0::MapPoint({512, 256}));
  const auto shifted =
      ot0::NativeTexturePoint(stamped, 1536, -17464, 1024, 512);
  CHECK(shifted == ot0::MapPoint({768, 0}));
  CHECK(stamped.valid && stamped.reveal_stamp_world_size == 3150 &&
        stamped.reveal_texture_world_size == 6144 &&
        stamped.reveal_mask_path == "/Game/UI/MiniMap/BP/T_mask.T_mask");
  m.Put(Volume + 0x314, float(NAN));
  CHECK(!m.Snapshot().valid);
  m.Put(Volume + 0x314, float(3));
  m.Put(Mini + 0x7e0, uint64_t(0));
  CHECK(!m.Snapshot().valid);
  m.Put(Mini + 0x7e0, Mask);
  m.Put(Mask + 0x18, m.Name("Unexpected_mask"));
  CHECK(!m.Snapshot().valid);
  m.Put(Mask + 0x18, m.Name("T_mask"));
  auto bounds = ot0::CellTextureBounds(s, {4, 4});
  CHECK(bounds[0] == ot0::MapPoint({200, 725}));
  CHECK(bounds[1] == ot0::MapPoint({250, 775}));
  for (uint8_t state : {10, 12, 14, 25, 26, 27}) {
    m.Put(App + 0x2f0, state);
    auto x = m.Snapshot();
    CHECK(!x.valid && x.state == state && x.texture_path.empty() &&
          x.revealed_cells.empty() && !x.heading_valid);
  }
  m = Memory();
  m.Put(App + 0x2f0, uint8_t(24));
  CHECK(m.Snapshot().valid);
  m = Memory();
  constexpr uint64_t Level = 0x1400030000, Data = 0x1400031000,
                     Rows = 0x1400032000;
  uint64_t map_key{};
  m.Read(Mini + 0x34c, &map_key, 8);
  m.Put(Main + 0x7cc6e80, Level);
  m.Put(Main + 0x7cc7448, Data);
  m.Put(Level + 0x35c, map_key);
  m.Put(Data + 0x358, Rows);
  m.Put(Data + 0x360, int32_t(1));
  m.Put(Data + 0x364, int32_t(1));
  m.Zero(Rows, 0x108);
  m.Put(Rows + 8, uint32_t(81));
  m.Put(Rows + 0x50, map_key);
  m.Put(Rows + 0x68, uint32_t(12345));
  CHECK(m.Snapshot().valid && m.Snapshot().map_name_id == 12345);
  m.Put(Rows + 0x50, uint64_t(0));
  CHECK(m.Snapshot().valid && m.Snapshot().map_name_id == 0);
  m.Put(Data + 0x364, int32_t(0));
  CHECK(m.Snapshot().valid && m.Snapshot().map_name_id == 0);
  m = Memory();
  m.Put(Mini, uint64_t(0));
  CHECK(!m.Snapshot().valid);
  m = Memory();
  m.Put(Volume + 0x10, uint64_t(0x1400020008));
  CHECK(!m.Snapshot().valid);
  m = Memory();
  m.Put(Mini + 0x34c, uint64_t(m.Name("Map_Other")));
  auto stale_map = m.Snapshot();
  CHECK(!stale_map.valid && stale_map.texture_path.empty());
  m = Memory();
  m.Put(Resource + 0xf8, uint8_t(0));
  CHECK(!m.Snapshot().valid);
  m = Memory();
  m.Put(Save + 0xd24, int32_t(0));
  CHECK(!m.Snapshot().valid);
  m = Memory();
  m.Put(Save + 0xd28, uint32_t(0));
  CHECK(!m.Snapshot().valid);
  m = Memory();
  m.Put(Nodes + 4, uint32_t(0));
  m.Put(Nodes + 0x20, int32_t(0));
  CHECK(!m.Snapshot().valid && m.reads < 100);
  m = Memory();
  m.Put(Pairs, ot0::MapCell{-1, 4});
  CHECK(!m.Snapshot().valid);
  m = Memory();
  m.Put(Mini + 0x358, std::array<double, 3>{NAN, 0, 0});
  CHECK(!m.Snapshot().valid);
  m = Memory();
  unsigned roots = 0;
  auto x = ot0::ReadExploration(
      [&](uint64_t at, void *out, size_t n) {
        if (at == Main + 0x7cc7588 && ++roots == 2)
          m.Put(at, Mini + 8);
        return m.Read(at, out, n);
      },
      Main);
  CHECK(!x.valid && x.texture_path.empty());
  CHECK(!ot0::ReadExploration({}, Main).valid);
  CHECK(!m.Snapshot(UINT64_MAX).valid);
}
void Native() {
  const char *path = std::getenv("OT0_MAP_MEMORY");
  if (!path)
    return;
  std::ifstream in(path, std::ios::binary);
  CHECK(bool(in));
  if (!in)
    return;
  char magic[8];
  in.read(magic, 8);
  CHECK(std::string(magic, 8) == "OT0MAP01");
  uint64_t main{};
  uint32_t count{};
  in.read(reinterpret_cast<char *>(&main), 8);
  in.read(reinterpret_cast<char *>(&count), 4);
  CHECK(count <= 4096);
  if (count > 4096)
    return;
  Memory m;
  m.data.clear();
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t at{};
    uint32_t n{};
    in.read(reinterpret_cast<char *>(&at), 8);
    in.read(reinterpret_cast<char *>(&n), 4);
    CHECK(n <= 65536);
    if (n > 65536)
      return;
    std::vector<uint8_t> b(n);
    in.read(reinterpret_cast<char *>(b.data()), n);
    for (size_t j = 0; j < n; ++j)
      m.data[at + j] = b[j];
  }
  CHECK(bool(in));
  auto s = m.Snapshot(main);
  CHECK(s.valid);
  if (s.map_id == 46) {
    // Private native Valore replay. Its explicit logical name omits the
    // spawned bounds actor's _1 suffix; the old full-FName guard refused it.
    CHECK(s.map_key == "Map_TP_Forest_N0_1001_A1");
    CHECK(s.section_key == "MinimapBoundsVolume");
    CHECK(s.revealed_cells.size() == 165);
    CHECK(s.native_direction == 2 && s.map_name_id == 3247);
    CHECK(s.filter_on && s.reveal_texture_world_size == 8192 &&
          s.reveal_stamp_world_size == 3150);
  } else {
    CHECK(s.map_id == 81 && s.section_key == "Room_MinimapBoundsVolume3_3");
    CHECK(s.revealed_cells.size() == 22);
    CHECK(s.native_direction == 0 || s.native_direction == 3);
    CHECK(std::abs(s.texture_position.x -
                   (s.native_direction == 0 ? 370.83943176 : 394.519)) < .001);
  }
  std::fprintf(stderr,
               "Native map %s, section %s, point %.3f,%.3f, direction %u, "
               "revealed %zu; reads %u\n",
               s.map_key.c_str(), s.section_key.c_str(), s.texture_position.x,
               s.texture_position.y, s.native_direction,
               s.revealed_cells.size(), m.reads);
}
} // namespace
int main() {
  Synthetic();
  Native();
  std::fprintf(stderr, "Octopath exploration: %d failures\n", failures);
  return failures ? 1 : 0;
}
