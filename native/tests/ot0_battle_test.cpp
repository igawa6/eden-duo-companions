// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic fixture mirroring the live 2026-10-25 Aristocrat fight (research
// analysis/20261025-battle-route): 2 foes, foe 0 at shield 1 after a dagger
// hit.
#include "../modules/ot0_battle.h"
#include <cassert>
#include <iostream>
#include <map>
namespace {
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  template <class T> void put(uint64_t p, const T &t) {
    const auto *b = reinterpret_cast<const uint8_t *>(&t);
    for (size_t i = 0; i < sizeof(T); ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *dest, size_t n) {
    auto *b = static_cast<uint8_t *>(dest);
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it == bytes.end())
        return false;
      b[i] = it->second;
    }
    return true;
  }
};
constexpr uint64_t M = 0x80000000, root = 0x1400000000, bm = 0x1400010000,
                   manager = 0x1400020000, save = 0x1400030000,
                   data = 0x1400040000, e0 = 0x1400050000, e1 = 0x1400051000,
                   p0 = 0x1400052000, p1 = 0x1400053000, p2 = 0x1400054000,
                   p3 = 0x1400055000;
void array(Memory &m, uint64_t at, uint64_t ptr, int n) {
  m.put(at, ptr);
  m.put(at + 8, int32_t(n));
  m.put(at + 12, int32_t(n));
}
template <size_t N>
void ints(Memory &m, uint64_t at, uint64_t buf,
          const std::array<int32_t, N> &v) {
  array(m, at, buf, int(N));
  m.put(buf, v);
}
Memory Fixture() {
  Memory m;
  m.put(M + 0x7cc8cc0, root);
  m.put(root, M + 0x6207610);
  m.put(root + 0x2f0, uint8_t(26));
  m.put(M + 0x7cc5b78, bm);
  m.put(bm + 8, uint32_t(0x8));
  m.put(M + 0x7cc8540, manager);
  m.put(manager + 0x2c8, save);
  array(m, bm + 0xa98, data, 2);
  m.put(data, std::array<uint64_t, 2>{e0, e1});
  for (auto [a, alive] : {std::pair{e0, 1}, std::pair{e1, 1}}) {
    m.put(a + 0x298, uint32_t(32312));
    m.put(a + 0x290, uint8_t(alive));
    m.put(a + 0xa48, uint32_t(0x8c));
    m.put(a + 0xa4c, uint32_t(0x2));
  }
  const uint64_t rec = save + 0x4f0;
  ints<6>(m, rec + 0x228, data + 0x100, {1, 2, 0, 0, 0, 0});
  ints<8>(m, rec + 0x1c8, data + 0x200,
          {904, 10503, 10905, 10101, 10804, 40204, 11407, 0});
  ints<8>(m, rec + 0x1d8, data + 0x300, {727, 558, 857, 791, 908, 850, 764, 0});
  ints<8>(m, rec + 0x1e8, data + 0x400, {103, 68, 60, 60, 68, 119, 80, 0});
  ints<8>(m, rec + 0x1f8, data + 0x500, {1, 1, 1, 1, 1, 1, 1, 0});
  for (auto [a, id] : {std::pair{p0, 10503}, std::pair{p1, 10101},
                       std::pair{p2, 904}, std::pair{p3, 10905}})
    m.put(a + 0x298, uint32_t(id));
  array(m, bm + 0x8b8, data + 0x600, 6);
  m.put(data + 0x600, std::array<uint64_t, 6>{p0, p1, p2, p3, e0, e1});
  m.put(bm + 0x8e0, int32_t(1));
  // Equipment: Celsus 10503 = Dagger 103001 + Sword 101001; Leader record keyed
  // 904.
  array(m, save + 0x158, data + 0x800, 2);
  m.put(data + 0x800, uint32_t(904));
  array(m, data + 0x808, data + 0x900, 2);
  m.put(data + 0x900, std::array<uint32_t, 2>{108001, 106006});
  m.put(data + 0x898, uint32_t(10503));
  array(m, data + 0x8a0, data + 0x910, 2);
  m.put(data + 0x910, std::array<uint32_t, 2>{103001, 101001});
  array(m, bm + 0x8f8, data + 0x700, 6);
  m.put(data + 0x700, std::array<uint64_t, 6>{p1, e0, p0, p2, p3, e1});
  return m;
}
ot0::BattleSnapshot Read(Memory &m, int64_t delta = 0) {
  return ot0::ReadBattle(
      [&](uint64_t p, void *o, size_t n) { return m.read(p, o, n); }, M, delta);
}
void ModalChecks() {
  constexpr uint64_t ui = 0x1400100000, dialog = ui + 0x1000,
                     slate = ui + 0x2000, ctrl = ui + 0x3000;
  const auto fixture = [&](int64_t delta) {
    Memory m;
    m.put(M + 0x7cc9a18, ui);
    m.put(ui, M + 0x63b6f18 + uint64_t(delta));
    m.put(ui + 8, uint32_t(8));
    m.put(ui + 0x2a8, dialog);
    m.put(dialog, M + 0x63aae78 + uint64_t(delta));
    m.put(dialog + 8, uint32_t(8));
    m.put(dialog + 0x108, slate);
    m.put(dialog + 0x110, ctrl);
    m.put(ctrl + 8, int32_t(1));
    m.put(slate, M + 0x5d011e8 + uint64_t(delta));
    m.put(slate + 8, slate);
    m.put(slate + 0x184, std::array<uint8_t, 2>{2, 1});
    return m;
  };
  for (int64_t delta : {int64_t(0), int64_t(0x10000000000), int64_t(-4096)}) {
    auto m = fixture(delta);
    const auto read = [&](uint64_t p, void *o, size_t n) {
      return m.read(p, o, n);
    };
    auto modal = ot0::ReadModalState(read, M, delta);
    assert(modal.valid && !modal.active);
    for (uint8_t visible : {uint8_t(1), uint8_t(0x11), uint8_t(0x19)}) {
      m.put(slate + 0x184, visible);
      modal = ot0::ReadModalState(read, M, delta);
      assert(modal.valid && modal.active);
    }
    if (delta)
      assert(!ot0::ReadModalState(read, M).valid);
    m.put(slate + 0x184, uint8_t(4));
    modal = ot0::ReadModalState(read, M, delta);
    assert(modal.valid && !modal.active);
    m.put(slate + 0x184, uint8_t(0xff));
    assert(!ot0::ReadModalState(read, M, delta).valid);
    m = fixture(delta);
    m.put(ctrl + 8, int32_t(0));
    assert(!ot0::ReadModalState(read, M, delta).valid);
    m = fixture(delta);
    m.put(dialog, M + 0x63aae80 + uint64_t(delta));
    assert(!ot0::ReadModalState(read, M, delta).valid);
    m = fixture(delta);
    int headers = 0;
    const auto replace = [&](uint64_t p, void *o, size_t n) {
      if (p == ui + 0x2a8 && ++headers == 2)
        m.put(p, dialog + 8);
      return m.read(p, o, n);
    };
    assert(!ot0::ReadModalState(replace, M, delta).valid);
  }
}
} // namespace
int main() {
  ModalChecks();
  auto m = Fixture();
  auto s = Read(m);
  assert(s.valid && s.enemy_count == 2 && s.member_count == 7);
  // Optional live HP must never invalidate the established battle snapshot.
  assert(s.enemies[0].hp == -1);
  ints<2>(m, save + 0x4f0 + 0x208, data + 0xa00, {226, 117});
  auto live_hp = Read(m);
  assert(live_hp.valid && live_hp.enemies[0].hp == 226 && live_hp.enemies[1].hp == 117);
  m.put(data + 0xa00, int32_t(-1));
  live_hp = Read(m);
  assert(live_hp.valid && live_hp.enemies[0].hp == -1);
  m.put(data + 0xa00, int32_t(226));
  int hp_reads = 0;
  live_hp = ot0::ReadBattle([&](uint64_t p, void* out, size_t n) {
    if(p == data + 0xa00 && ++hp_reads == 2) m.put(p, int32_t(225));
    return m.read(p, out, n);
  }, M);
  assert(live_hp.valid && live_hp.enemies[0].hp == -1);

  assert(s.enemies[0].shields == 1 && s.enemies[1].shields == 2 &&
         s.enemies[0].alive);
  assert(s.enemies[0].enemy_id == 32312 && s.enemies[0].weapon_mask == 0x8c &&
         s.enemies[0].magic_mask == 0x2);
  assert(s.members[0].id == 904 && s.members[2].hp == 857 &&
         s.members[5].sp == 119 && s.members[3].bp == 1);
  assert(s.now_count == 6 && !s.now[0].enemy && s.now[0].ref == 10503 &&
         s.now[4].enemy && s.now[4].ref == 0);
  assert(s.next_count == 6 && s.next[1].enemy && s.next[1].ref == 0 &&
         s.next[5].ref == 1);
  assert(ot0::GetBattleFlow(s) == ot0::BattleFlow::Combat);
  {
    auto f = s;
    f.enemies[0].alive = false;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::Combat);
    f.enemies[1].alive = false;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::NoLivingFoes);
    for (auto &member : f.members)
      member.hp = 0;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::NoStandingParty);
    f.enemies[1].alive = true;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::NoStandingParty);
    f.valid = false;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::Unavailable);
    f = s;
    f.enemy_count = 7;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::Unavailable);
    f = s;
    f.member_count = 0;
    assert(ot0::GetBattleFlow(f) == ot0::BattleFlow::Unavailable);
  }
  assert(s.now_index == 1);
  assert(s.members[0].weapons[0] == 108001 &&
         s.members[0].weapons[1] == 106006);
  assert(s.members[1].weapons[0] == 103001 && s.members[2].weapons[0] == 0);
  for (int64_t delta : {int64_t(0x10000000000), int64_t(-0x1000)}) {
    auto f = Fixture();
    f.put(root, M + 0x6207610 + static_cast<uint64_t>(delta));
    assert(!Read(f).valid);
    assert(Read(f, delta) == s);
    assert(!Read(f, delta + 8).valid);
    f.put(root, M + 0x6207610);
    assert(Read(f, delta) == s);
  }
  {
    auto f = Fixture();
    f.put(bm + 0x8e0, int32_t(7));
    assert(!Read(f).valid);
  }
  // Not in battle -> nothing.
  {
    auto f = Fixture();
    f.put(root + 0x2f0, uint8_t(23));
    assert(!Read(f).valid);
  }
  // Battle manager flag guard.
  {
    auto f = Fixture();
    f.put(bm + 8, uint32_t(0x40000000));
    assert(!Read(f).valid);
  }
  // Out-of-range BP / shields reject the whole snapshot.
  {
    auto f = Fixture();
    f.put(data + 0x500, int32_t(9));
    assert(!Read(f).valid);
  }
  {
    auto f = Fixture();
    f.put(data + 0x100, int32_t(-1));
    assert(!Read(f).valid);
  }
  // Unknown bits (Poison, sentinels) are masked off.
  {
    auto f = Fixture();
    f.put(e0 + 0xa48, uint32_t(0x28c));
    f.put(e0 + 0xa4c, uint32_t(0x182));
    auto t = Read(f);
    assert(t.valid && t.enemies[0].weapon_mask == 0x8c &&
           t.enemies[0].magic_mask == 0x2);
  }
  // Defeated foe.
  {
    auto f = Fixture();
    f.put(e1 + 0x290, uint8_t(0));
    auto t = Read(f);
    assert(t.valid && !t.enemies[1].alive);
  }
  // Missing turn actor -> reject.
  {
    auto f = Fixture();
    f.put(data + 0x600, uint64_t(0));
    assert(!Read(f).valid);
  }
  // A party-id array shorter than 8 is read only as far as it goes (regression: the reader
  // used to read 8 ids past a 4-entry array and drop the whole snapshot).
  {
    auto f = Fixture();
    const uint64_t rec = save + 0x4f0, short_ids = 0x1400060000;
    array(f, rec + 0x1c8, short_ids, 4);
    f.put(short_ids, std::array<int32_t, 4>{904, 10503, 10905, 10101});
    auto t = Read(f);
    assert(t.valid && t.member_count == 4 && t.members[0].id == 904 && t.members[3].id == 10101);
  }
  // Equipment keyed by another protagonist job alias (900..909) still attaches to the Leader.
  {
    auto f = Fixture();
    f.put(data + 0x800, uint32_t(901));
    auto t = Read(f);
    assert(t.valid && t.members[0].id == 904 && t.members[0].weapons[0] == 108001);
  }
  std::cout << "ot0 battle reader: all checks passed\n";
}
