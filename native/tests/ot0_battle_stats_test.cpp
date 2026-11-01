// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_battle_stats.h"
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
Memory EnemyFixture(int64_t delta = 0) {
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
constexpr uint64_t Fronts = 0x1400060000, Player = 0x1400070000,
                   PlayerEntries = 0x1400080000, PlayerVariants = 0x1400090000;
Memory Fixture(int64_t delta = 0) {
  auto m = EnemyFixture(delta);
  m.put(Bm + 0x9b8, Fronts);
  m.put(Bm + 0x9c0, int32_t{1});
  m.put(Bm + 0x9c4, int32_t{1});
  m.put(Bm + 0x9c8, uint64_t{0});
  m.put(Bm + 0x9d0, int32_t{0});
  m.put(Bm + 0x9d4, int32_t{0});
  m.put(Fronts, Player);
  m.object(Player, 0x63f39b8, delta);
  m.put(Player + 0x293, uint8_t{0});
  m.put(Player + 0x2a8, uint8_t{1});
  m.put(Player + 0x298, uint32_t{904});
  m.put(Player + 0x5b8, std::array<uint8_t, 0x50>{});
  m.put(Player + 0x5b8, PlayerEntries);
  m.put(Player + 0x5c0, int32_t{8});
  m.put(Player + 0x5c4, int32_t{8});
  m.put(Player + 0x5f0, int32_t{7});
  m.put(Player + 0x600, int32_t{1});
  for (int i = 0; i < 8; ++i) {
    auto entry = PlayerEntries + uint64_t(i) * 32;
    auto variants = PlayerVariants + uint64_t(i) * 16;
    m.put(entry, std::array<uint8_t, 32>{});
    m.put(entry, uint8_t(9 + i));
    m.put(entry + 8, variants);
    m.put(entry + 0x10, int32_t{1});
    m.put(entry + 0x14, int32_t{1});
    m.put(entry + 0x18, int32_t{i - 1});
    m.put(variants, std::array<uint8_t, 12>{});
    m.put(variants, uint8_t{1});
    m.put(variants + 1, uint8_t{1});
    m.put(variants + 8, int32_t(100 + i));
  }
  m.put(Entries, uint8_t{13});
  m.put(Variants + 8, int32_t{73});
  return m;
}
constexpr uint64_t GuChunks = 0x1400100000, GuItems = 0x1400110000,
                   Ailments = 0x1400120000;
void SignatureFixture(Memory &m) {
  m.put(Main + 0x6ce97e8 + 0x10, GuChunks);
  m.put(Main + 0x6ce97e8 + 0x24, int32_t{3});
  m.put(GuChunks, GuItems);
  for (auto [actor, index] :
       std::array<std::pair<uint64_t, int32_t>, 2>{{{Player, 1}, {Actor, 2}}}) {
    m.put(actor + 0xc, index);
    m.put(actor + 0x2a8, uint8_t{1});
    m.put(actor + 0x800, std::array<uint8_t, 16>{});
    auto at = GuItems + uint64_t(index) * 20;
    m.put(at, std::array<uint8_t, 20>{});
    m.put(at, actor);
    m.put(at + 16, int32_t(1000 + index));
  }
}
} // namespace
int main() {
  ot0::BattleSnapshot battle{};
  battle.valid = true;
  battle.member_count = 1;
  battle.members[0].id = 904;
  battle.enemy_count = 1;
  battle.enemies[0].enemy_id = 32312;
  const auto run = [&](Memory &m, int64_t delta = 0) {
    return ot0::ReadBattleStats(
        [&](auto p, auto out, auto n) { return m.read(p, out, n); }, Main,
        battle, delta);
  };
  auto m = Fixture();
  auto good = run(m);
  assert(good.valid && good.party[0].bound && good.party[0].id == 904 &&
         good.enemies[0].bound);
  for (int i = 0; i < 8; ++i)
    assert(good.party[0].parameters[i].valid &&
           good.party[0].parameters[i].value == 100 + i);
  assert(!good.enemies[0].parameters[0].valid &&
         good.enemies[0].parameters[4].valid &&
         good.enemies[0].parameters[4].value == 73);
  assert(good.party[0].weapon_context_valid &&
         good.party[0].equipment_weapon_slot == 0);
  m.put(Player + 0x2a8, uint8_t{0});
  assert(run(m).party[0].equipment_weapon_slot == 1);
  m.put(Player + 0x2a8, uint8_t{2});
  auto badweapon = run(m);
  assert(badweapon.valid && !badweapon.party[0].weapon_context_valid &&
         !badweapon.party[0].parameters[0].valid);
  m = Fixture();
  SignatureFixture(m);
  auto signed_snapshot = run(m);
  assert(signed_snapshot.valid && signed_snapshot.party[0].signature_valid &&
         signed_snapshot.party[0].native_actor == Player &&
         signed_snapshot.party[0].native_serial == 1001 &&
         signed_snapshot.party[0].weapon_selector == 1 &&
         signed_snapshot.party[0].raw_ailment_fingerprint ==
             0x4d25767f9dce13f5ULL);
  assert(signed_snapshot.enemies[0].signature_valid &&
         signed_snapshot.enemies[0].native_serial == 1002);
  m.put(Player + 0x800, Ailments);
  m.put(Player + 0x808, int32_t{1});
  m.put(Player + 0x80c, int32_t{1});
  m.put(Ailments, std::array<uint8_t, 0xe8>{});
  m.put(Ailments, uint32_t{107});
  m.put(Ailments + 6, int16_t{3});
  auto one = run(m);
  assert(one.party[0].signature_valid &&
         one.party[0].raw_ailment_fingerprint == 0x10f663eaf65cfb74ULL);
  int mutation = 0;
  auto changed_signature = ot0::ReadBattleStats(
      [&](auto p, auto out, auto n) {
        if (p == PlayerEntries && ++mutation == 1)
          m.put(Ailments + 6, int16_t{2});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(changed_signature.valid &&
         !changed_signature.party[0].signature_valid &&
         changed_signature.party[0].parameters[0].valid);
  m.put(GuItems + 20, Actor);
  assert(!run(m).party[0].signature_valid);
  m.put(GuItems + 20, Player);
  m.put(GuItems + 20 + 8, uint32_t{0x200000});
  assert(!run(m).party[0].signature_valid);
  m.put(GuItems + 20 + 8, uint32_t{0});
  m.put(GuItems + 20 + 16, int32_t{0});
  assert(!run(m).party[0].signature_valid);
  m.put(GuItems + 20 + 16, int32_t{1001});
  m.put(Player + 0x808, int32_t{129});
  assert(!run(m).party[0].signature_valid);
  m = Fixture();
  assert(!run(m).party[0].signature_valid); // Old captures omit GU serial;
                                            // never fabricate it.
  m.put(PlayerVariants + 2, uint8_t{1});
  auto wrongflag = run(m);
  assert(wrongflag.valid && !wrongflag.party[0].parameters[0].valid &&
         wrongflag.party[0].parameters[1].valid);
  m = Fixture();
  m.put(Player + 0x293, uint8_t{1});
  assert(!run(m).valid);
  m = Fixture();
  m.put(Player + 8, uint32_t{0x40000000});
  assert(!run(m).valid);
  m = Fixture();
  m.put(Player + 0x298, uint32_t{10804});
  assert(!run(m).valid);
  m = Fixture();
  m.put(Player + 0x298, uint32_t{900});
  assert(run(m).valid); // Native protagonist aliases.
  m = Fixture();
  m.put(Actor + 0x298, uint32_t{904});
  battle.enemies[0].enemy_id = 900;
  assert(!run(m).valid);
  battle.enemies[0].enemy_id = 32312;
  m = Fixture();
  m.put(App + 0x2f0, uint8_t{23});
  assert(!run(m).valid);
  m = Fixture();
  int cache_reads = 0;
  auto partial = ot0::ReadBattleStats(
      [&](auto p, auto out, auto n) {
        if (p == PlayerVariants && ++cache_reads == 2)
          m.put(PlayerVariants + 8, int32_t{999});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(partial.valid && !partial.party[0].parameters[0].valid);
  m = Fixture();
  int root_reads = 0;
  auto stale = ot0::ReadBattleStats(
      [&](auto p, auto out, auto n) {
        if (p == Bm + 0x9b8 && ++root_reads == 2)
          m.put(Bm + 0x9c0, int32_t{0});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(!stale.valid);
  m = Fixture();
  m.put(Bm + 0x9c8, Fronts);
  m.put(Bm + 0x9d0, int32_t{1});
  m.put(Bm + 0x9d4, int32_t{1});
  assert(!run(m).valid); // Duplicated actor in roster.
  constexpr int64_t delta = 0x1234000;
  m = Fixture(delta);
  assert(run(m, delta).valid);
  assert(!run(m).valid);
  if (const char *path = std::getenv("OT0_BATTLE_STATS_MEMORY")) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    auto get = [&]<class T>(T &v) {
      f.read(reinterpret_cast<char *>(&v), sizeof(v));
      assert(f);
    };
    char magic[8];
    f.read(magic, 8);
    assert(!std::memcmp(magic, "OT0BST01", 8));
    uint64_t main{};
    uint32_t members{}, enemies{}, segments{};
    get(main);
    get(members);
    get(enemies);
    assert(members > 0 && members <= 8 && enemies > 0 && enemies <= 6);
    ot0::BattleSnapshot live{};
    live.valid = true;
    live.member_count = int(members);
    live.enemy_count = int(enemies);
    for (unsigned i = 0; i < members; ++i)
      get(live.members[i].id);
    for (unsigned i = 0; i < enemies; ++i)
      get(live.enemies[i].enemy_id);
    get(segments);
    assert(segments < 16384);
    Memory captured;
    for (unsigned i = 0; i < segments; ++i) {
      uint64_t p{};
      uint32_t n{};
      get(p);
      get(n);
      assert(n <= 65536);
      for (unsigned j = 0; j < n; ++j) {
        uint8_t v{};
        get(v);
        captured.put(p + j, v);
      }
    }
    auto actual = ot0::ReadBattleStats(
        [&](auto p, auto out, auto n) { return captured.read(p, out, n); },
        main, live);
    assert(actual.valid);
    const char *signature_env = std::getenv("OT0_BATTLE_STATS_SIGNATURES");
    const bool expect_signatures = signature_env && std::string(signature_env) == "1";
    for (unsigned i = 0; i < members; ++i)
      assert(actual.party[i].signature_valid == expect_signatures);
    for (unsigned i = 0; i < members; ++i)
      assert(actual.party[i].bound && actual.party[i].id == live.members[i].id);
    for (unsigned i = 0; i < enemies; ++i)
      assert(actual.enemies[i].signature_valid == expect_signatures);
    for (unsigned i = 0; i < enemies; ++i)
      assert(actual.enemies[i].bound && actual.enemies[i].parameters[4].valid &&
             actual.enemies[i].parameters[4].value == 73);
  }
}
