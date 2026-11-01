// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_battle_selection.h"
#include <cassert>
#include <map>
namespace {
constexpr uint64_t M = 0x80000000, A = 0x1400000000, B = A + 0x1000,
                   L = A + 0x3000, C = A + 0x5000, P = A + 0x7000,
                   ML = A + 0x9000, SL = A + 0xb000, Cursor = A + 0xd000,
                   Hero = A + 0x10000, Macy = A + 0x12000, Celsus = A + 0x14000,
                   Stia = A + 0x16000, E0 = A + 0x20000, E1 = A + 0x22000,
                   E2 = A + 0x24000, Targets = A + 0x30000,
                   Enemies = A + 0x31000;
struct Memory {
  std::map<uint64_t, uint8_t> b;
  template <class T> void put(uint64_t p, const T &v) {
    auto *x = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof v; ++i)
      b[p + i] = x[i];
  }
  bool read(uint64_t p, void *out, size_t n) {
    auto *x = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      auto it = b.find(p + i);
      if (it == b.end())
        return false;
      x[i] = it->second;
    }
    return true;
  }
};
void array(Memory &m, uint64_t at, uint64_t data, int32_t count) {
  m.put(at, data);
  m.put(at + 8, count);
  m.put(at + 12, count);
}
Memory fixture(int64_t delta = 0) {
  Memory m;
  auto object = [&](uint64_t at, uint64_t vt) {
    m.put(at, M + vt + uint64_t(delta));
    m.put(at + 8, uint32_t(8));
  };
  auto visible = [&](uint64_t at, uint64_t s, uint64_t ctrl) {
    m.put(at + 0x108, s);
    m.put(at + 0x110, ctrl);
    m.put(ctrl + 8, int32_t(1));
    m.put(s, M + 0x5d011e8 + uint64_t(delta));
    m.put(s + 8, s);
    m.put(s + 0x184, std::array<uint8_t, 2>{0x11, 1});
  };
  m.put(M + 0x7cc8cc0, A);
  object(A, 0x6207610);
  m.put(A + 0x2f0, uint8_t(26));
  m.put(M + 0x7cc5b78, B);
  m.put(B + 8, uint32_t(8));
  m.put(B + 0x163c, uint8_t(2));
  m.put(B + 0x1678, L);
  object(L, 0x6221a30);
  m.put(L + 0x2f8, C);
  object(C, 0x62281d0);
  m.put(C + 0x300, P);
  object(P, 0x621cf90);
  visible(P, A + 0x40000, A + 0x41000);
  m.put(P + 0x2f8, ML);
  m.put(P + 0x300, SL);
  m.put(P + 0x3a0, ML);
  object(ML, 0x621b8e0);
  object(SL, 0x621b8e0);
  visible(ML, A + 0x42000, A + 0x43000);
  visible(SL, A + 0x44000, A + 0x45000);
  for (auto at : {ML, SL}) {
    m.put(at + 0x2f8, Cursor);
    m.put(at + 0x320, Hero);
    m.put(at + 0x328, uint8_t(at == ML ? 1 : 2));
    m.put(at + 0x329, uint8_t(1));
  }
  object(Cursor, 0x646e168);
  m.put(Cursor + 0x378, int32_t(0));
  m.put(Cursor + 0x43c, int32_t(7));
  m.put(Cursor + 0x444, int32_t(1));
  m.put(Cursor + 0x4b8, uint8_t(3));
  m.put(Cursor + 0x9dc, int32_t(7));
  m.put(Cursor + 0x9e0, int32_t(0));
  for (auto [at, id] : {std::pair{Hero, 904u},
                        {Macy, 10804u},
                        {Celsus, 10503u},
                        {Stia, 40204u}}) {
    object(at, 0x63f39b8);
    m.put(at + 0x293, uint8_t(0));
    m.put(at + 0x298, id);
  }
  for (auto [at, id] : {std::pair{E0, 32316u}, {E1, 32316u}, {E2, 32313u}}) {
    object(at, 0x63f39b8);
    m.put(at + 0x293, uint8_t(1));
    m.put(at + 0x298, id);
  }
  m.put(E0 + 0xa40, int32_t(3));
  m.put(E1 + 0xa40, int32_t(2));
  m.put(E2 + 0xa40, int32_t(1));
  array(m, B + 0xa98, Enemies, 3);
  m.put(Enemies, std::array<uint64_t, 3>{E0, E1, E2});
  array(m, B + 0x1600, Targets, 3);
  m.put(Targets, std::array<uint64_t, 3>{E0, E1, E2});
  m.put(B + 0x1610, uint8_t(1));
  m.put(B + 0x1611, uint8_t(1));
  m.put(C + 0x3b0, int32_t(0));
  return m;
}
ot0::BattleSnapshot battle() {
  ot0::BattleSnapshot b;
  b.valid = true;
  b.member_count = 4;
  b.enemy_count = 3;
  for (int i = 0; i < 4; ++i)
    b.members[i].id = std::array<uint32_t, 4>{900, 10804, 10503, 40204}[i];
  for (int i = 0; i < 3; ++i)
    b.enemies[i].enemy_id = i == 2 ? 32313 : 32316;
  return b;
}
} // namespace
int main() {
  using Phase = ot0::BattleSelectionPhase;
  auto b = battle();
  auto m = fixture();
  auto sample = [&](int64_t d = 0) {
    return ot0::ReadBattleSelection(
        [&](auto p, void *out, size_t n) { return m.read(p, out, n); }, M, b,
        d);
  };
  auto s = sample();
  assert(s.valid && s.phase == Phase::Command && s.actor_id == 900 &&
         s.command_index == 0);
  // Captured actual native main list starts at a zero9e0 cursor factor.
  m.put(Cursor + 0x9e0, int32_t(1));assert(sample().command_index==1);
  m.put(Cursor + 0x9e0, int32_t(-1));assert(!sample().valid);
  m.put(Cursor + 0x9e0, int32_t(0));
  m.put(B + 0x163c, uint8_t(3));
  s = sample();
  assert(s.valid && s.phase == Phase::EnemyTarget && s.enemy_index == 2);
  m.put(C + 0x3b0, int32_t(1));
  assert(sample().enemy_index == 1);
  m.put(C + 0x3b0, int32_t(2));
  assert(sample().enemy_index == 0);
  // Duplicate enemy IDs are deliberately resolved by actor/slot, never by name.
  m.put(P + 0x3a0, SL);
  m.put(SL + 0x320, Macy);
  m.put(B + 0x1611, uint8_t(30));
  array(m, B + 0x1600, Targets, 2);
  m.put(Targets, std::array<uint64_t, 2>{Celsus, Stia});
  m.put(C + 0x3b0, int32_t(1));
  s = sample();
  assert(s.valid && s.phase == Phase::AllyTarget && s.actor_id == 10804 &&
         s.ally_id == 40204 && s.list_type == 2);
  m.put(B + 0x1610, uint8_t(0));
  assert(sample().phase == Phase::GroupTargets);
  m = fixture();
  m.put(B + 0x163c, uint8_t(4));
  assert(!sample().valid);
  m = fixture();
  m.put(Cursor + 0x9dc, int32_t(0));
  assert(!sample().valid);
  m = fixture();
  m.put(B + 0x163c, uint8_t(3));
  m.put(Targets + 8, E0);
  assert(!sample().valid);
  m = fixture();
  m.put(B + 0x163c, uint8_t(3));
  m.put(E1 + 0xa40, int32_t(3));
  assert(!sample().valid);
  m = fixture();
  m.put(B + 0x163c, uint8_t(3));
  m.put(C + 0x3b0, int32_t(8));
  assert(!sample().valid);
  m = fixture();
  m.put(B + 0x163c, uint8_t(3));
  int reads = 0;
  s = ot0::ReadBattleSelection(
      [&](auto p, void *out, size_t n) {
        if (p == C + 0x3b0 && ++reads == 2)
          m.put(p, int32_t(1));
        return m.read(p, out, n);
      },
      M, b);
  assert(!s.valid);
  for (auto delta : {int64_t(0x10000000000), int64_t(-4096)}) {
    m = fixture(delta);
    assert(sample(delta).valid);
    assert(!sample().valid);
  }
}
