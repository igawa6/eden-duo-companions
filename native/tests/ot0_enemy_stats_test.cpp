// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_enemy_stats.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
namespace {
constexpr uint64_t Main = 0x80000000, App = 0x1400000000, Bm = 0x1400010000,
                   Actors = 0x1400020000, Actor = 0x1400030000,
                   Entries = 0x1400040000, Variants = 0x1400050000;
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  template <class T> void put(uint64_t p, const T &v) {
    auto b = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof(T); ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *out, size_t n) const {
    auto b = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it == bytes.end())
        return false;
      b[i] = it->second;
    }
    return true;
  }
  void object(uint64_t p, uint64_t vt, int64_t delta) {
    put(p, Main + vt + uint64_t(delta));
    put(p + 8, uint32_t{8});
  }
};
Memory Fixture(int64_t delta = 0) {
  Memory m;
  m.put(Main + 0x7cc8cc0, App);
  m.object(App, 0x6207610, delta);
  m.put(App + 0x2f0, uint8_t{26});
  m.put(Main + 0x7cc5b78, Bm);
  m.put(Bm + 8, uint32_t{8});
  m.put(Bm + 0xa98, Actors);
  m.put(Bm + 0xaa0, int32_t{1});
  m.put(Bm + 0xaa4, int32_t{1});
  m.put(Actors, Actor);
  m.object(Actor, 0x63f39b8, delta);
  m.put(Actor + 0x293, uint8_t{1});
  m.put(Actor + 0x298, uint32_t{32312});
  m.put(Actor + 0x540, int32_t{0});
  m.put(Actor + 0x548, int32_t{0});
  m.put(Actor + 0x5b8, std::array<uint8_t, 0x50>{});
  m.put(Actor + 0x5b8, Entries);
  m.put(Actor + 0x5c0, int32_t{1});
  m.put(Actor + 0x5c4, int32_t{1});
  m.put(Actor + 0x5f0, int32_t{0});
  m.put(Actor + 0x600, int32_t{1});
  m.put(Entries, std::array<uint8_t, 32>{});
  m.put(Entries, uint8_t{1});
  m.put(Entries + 8, Variants);
  m.put(Entries + 0x10, int32_t{1});
  m.put(Entries + 0x14, int32_t{1});
  m.put(Entries + 0x18, int32_t{-1});
  m.put(Variants, std::array<uint8_t, 12>{});
  m.put(Variants, uint8_t{1});
  m.put(Variants + 1, uint8_t{1});
  m.put(Variants + 8, int32_t{247});
  return m;
}
} // namespace
int main() {
  ot0::BattleSnapshot b{};
  b.valid = true;
  b.enemy_count = 1;
  b.enemies[0].enemy_id = 32312;
  auto run = [&](Memory &m, int64_t delta = 0) {
    return ot0::ReadEnemyStats(
        [&](auto p, auto out, auto n) { return m.read(p, out, n); }, Main, b,
        delta);
  };
  auto m = Fixture();
  assert(run(m)[0].max_hp_valid && run(m)[0].max_hp == 247);
  m.put(Bm + 0xaa4, int32_t{8});
  assert(run(m)[0].max_hp_valid); // Native allocated slack may exceed six.
  m = Fixture();
  m.put(Entries, uint8_t{13});
  m.put(Variants + 8, int32_t{73});
  auto speed_only = run(m)[0];
  assert(!speed_only.max_hp_valid && speed_only.speed_valid &&
         speed_only.speed == 73);
  m = Fixture();
  m.put(Actor + 0x548, int32_t{300});
  assert(run(m)[0].max_hp == 300);
  m.put(Actor + 0x293, uint8_t{0});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  m.put(Variants + 2, uint8_t{1});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  m.put(Actor + 0x5ec, int32_t{1});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  m.put(Entries, uint8_t{2});
  m.put(Entries + 0x18, int32_t{0});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  m.put(Actor + 0x5c0, int32_t{2});
  m.put(Actor + 0x5c4, int32_t{2});
  m.put(Entries, uint8_t{9});
  m.put(Entries + 0x18, int32_t{1});
  m.put(Entries + 32, std::array<uint8_t, 32>{});
  m.put(Entries + 32, uint8_t{1});
  m.put(Entries + 32 + 8, Variants);
  m.put(Entries + 32 + 0x10, int32_t{1});
  m.put(Entries + 32 + 0x14, int32_t{1});
  m.put(Entries + 32 + 0x18, int32_t{-1});
  assert(run(m)[0].max_hp == 247); // Exact native hash collision chain.
  m = Fixture();
  m.put(Actor + 0x600, int32_t{3});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  m.put(Actor + 8, uint32_t{0x40000000});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  m.put(Actor + 0x298, uint32_t{99});
  assert(!run(m)[0].max_hp_valid);
  m = Fixture();
  int reads = 0;
  auto changed = ot0::ReadEnemyStats(
      [&](auto p, auto out, auto n) {
        if (p == Variants && ++reads == 2)
          m.put(Variants + 8, int32_t{123});
        return m.read(p, out, n);
      },
      Main, b);
  assert(!changed[0].max_hp_valid);
  constexpr int64_t delta = 0x1234000;
  m = Fixture(delta);
  assert(run(m, delta)[0].max_hp_valid);
  assert(!run(m)[0].max_hp_valid);
  if (const char *path = std::getenv("OT0_ENEMY_STATS_MEMORY")) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    auto get = [&]<class T>(T &v) {
      f.read(reinterpret_cast<char *>(&v), sizeof(v));
      assert(f);
    };
    char magic[8]{};
    f.read(magic, 8);
    assert(std::memcmp(magic, "OT0ENM01", 8) == 0);
    uint64_t main{};
    uint32_t count{}, segments{};
    get(main);
    get(count);
    assert(count > 0 && count <= 6);
    ot0::BattleSnapshot live{};
    live.valid = true;
    live.enemy_count = int(count);
    for (unsigned i = 0; i < count; ++i)
      get(live.enemies[i].enemy_id);
    get(segments);
    assert(segments < 4096);
    Memory captured;
    for (unsigned i = 0; i < segments; ++i) {
      uint64_t address{};
      uint32_t size{};
      get(address);
      get(size);
      assert(size < 65536);
      for (unsigned j = 0; j < size; ++j) {
        uint8_t byte{};
        get(byte);
        captured.put(address + j, byte);
      }
    }
    auto result = ot0::ReadEnemyStats(
        [&](auto p, auto out, auto n) { return captured.read(p, out, n); },
        main, live);
    for (unsigned i = 0; i < count; ++i)
      assert(result[i].max_hp_valid && result[i].max_hp == 226 &&
             result[i].speed_valid && result[i].speed == 73);
  }
}
