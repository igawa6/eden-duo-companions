// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_battle.h"
#include "ot0_battle_actors.h"
#include "ot0_memory.h"
#include <cstring>
#include <algorithm>
namespace ot0 {
namespace {
using mem::Array;
using mem::Limit;
using mem::Pointer;
using Reader = mem::Reader;
} // namespace
BattleFlow GetBattleFlow(const BattleSnapshot &battle) {
  if (!battle.valid || battle.enemy_count < 1 || battle.enemy_count > 6 ||
      battle.member_count < 1 || battle.member_count > 8)
    return BattleFlow::Unavailable;
  bool party_standing = false, foes_alive = false;
  for (int i = 0; i < battle.member_count; ++i)
    party_standing |= battle.members[i].hp > 0;
  for (int i = 0; i < battle.enemy_count; ++i)
    foes_alive |= battle.enemies[i].alive;
  if (!party_standing)
    return BattleFlow::NoStandingParty;
  if (!foes_alive)
    return BattleFlow::NoLivingFoes;
  return BattleFlow::Combat;
}
ModalState ReadModalState(const ReadMemory &read, uint64_t main,
                          int64_t relocation_delta) {
  ModalState out;
  if (!read || !Pointer(main) || main > Limit - 0x7cc9a20)
    return out;
  Reader r{read};
  uint64_t manager{}, dialog{}, slate{}, controller{}, self{};
  uint64_t manager_vt{}, dialog_vt{}, slate_vt{};
  uint32_t manager_flags{}, dialog_flags{};
  int32_t refs{};
  std::array<uint8_t, 2> visibility{};
  // ShowGoToTitleDialog native3c7a9f0 uses MenuUIManager singleton7cc9a18
  // and its CommonDialog at2a8. The same widget is live-correlated with native
  // journal replay confirmation; cached outer Slate visibility changes02->11.
  if (!r.ptr(main + 0x7cc9a18, manager) || !r.get(manager, manager_vt) ||
      !MatchesVtable(manager_vt, main + 0x63b6f18, relocation_delta) ||
      !r.get(manager + 8, manager_flags) || (manager_flags & 0x40000030) ||
      !r.ptr(manager + 0x2a8, dialog) || !r.get(dialog, dialog_vt) ||
      !MatchesVtable(dialog_vt, main + 0x63aae78, relocation_delta) ||
      !r.get(dialog + 8, dialog_flags) || (dialog_flags & 0x40000030) ||
      !r.ptr(dialog + 0x108, slate) ||
      !r.ptr(dialog + 0x110, controller) || !r.get(controller + 8, refs) ||
      refs < 1 || refs > 1000000 || !r.get(slate, slate_vt) ||
      !MatchesVtable(slate_vt, main + 0x5d011e8, relocation_delta) ||
      !r.get(slate + 8, self) || self != slate ||
      !r.get(slate + 0x184, visibility) || visibility[1] != 1)
    return out;
  switch (visibility[0]) {
  case 1:    // HitTestInvisible
  case 0x11: // SelfHitTestInvisible
  case 0x19: // Visible
    out.active = true;
    break;
  case 2: // Collapsed
  case 4: // Hidden
    break;
  default:
    return ModalState{};
  }
  uint64_t p{}, vt{};
  uint32_t flags{};
  std::array<uint8_t, 2> again{};
  int32_t refs2{};
  if (!r.ptr(main + 0x7cc9a18, p) || p != manager || !r.get(manager, vt) ||
      vt != manager_vt || !r.get(manager + 8, flags) ||
      flags != manager_flags || !r.ptr(manager + 0x2a8, p) || p != dialog ||
      !r.get(dialog, vt) || vt != dialog_vt || !r.get(dialog + 8, flags) ||
      flags != dialog_flags || !r.ptr(dialog + 0x108, p) || p != slate ||
      !r.ptr(dialog + 0x110, p) || p != controller ||
      !r.get(controller + 8, refs2) || refs2 < 1 || refs2 > 1000000 ||
      !r.get(slate, vt) || vt != slate_vt || !r.get(slate + 8, p) ||
      p != slate || !r.get(slate + 0x184, again) || again != visibility)
    return ModalState{};
  out.valid = true;
  return out;
}
BattleSnapshot ReadBattle(const ReadMemory &read, uint64_t main,
                          int64_t relocation_delta) {
  BattleSnapshot out;
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8)
    return out;
  Reader r{read};
  uint64_t root{}, vtable{}, bm{}, manager{}, save{};
  uint8_t state{};
  uint32_t flags{};
  if (!r.ptr(main + 0x7cc8cc0, root) || !r.get(root, vtable) ||
      !MatchesVtable(vtable, main + 0x6207610, relocation_delta) ||
      !r.get(root + 0x2f0, state) || state != 26)
    return out;
  if (!r.ptr(main + 0x7cc5b78, bm) || !r.get(bm + 8, flags) ||
      (flags & 0x40000000) || !r.ptr(main + 0x7cc8540, manager) ||
      !r.ptr(manager + 0x2c8, save))
    return out;
  Array enemies{};
  if (!r.array(bm + 0xa98, enemies, 6) || enemies.count < 1)
    return out;
  std::array<uint64_t, 6> actors{};
  if (!r.raw(enemies.data, actors.data(), size_t(enemies.count) * 8))
    return out;
  const uint64_t rec = save + 0x4f0;
  std::array<int32_t, 6> shields{};
  if (!r.ints(rec + 0x228, enemies.count, shields.data()))
    return out;
  for (int i = 0; i < enemies.count; ++i) {
    uint64_t a = actors[i];
    BattleEnemy e;
    uint8_t alive{};
    if (!Pointer(a) || !r.get(a + 0x298, e.enemy_id) ||
        !r.get(a + 0x290, alive) || !r.get(a + 0xa48, e.weapon_mask) ||
        !r.get(a + 0xa4c, e.magic_mask) || alive > 1 || shields[i] < 0 ||
        shields[i] > 99)
      return BattleSnapshot{};
    e.alive = alive == 1;
    e.shields = shields[i];
    e.weapon_mask &= 0x1fe; // EWEAPON 1..8
    e.magic_mask &= 0x7e;   // EMAGIC 1..6 (Poison bit 8 not shown)
    out.enemies[i] = e;
  }
  out.enemy_count = enemies.count;
  // Party battle order: ids / HP / SP / BP arrays share one index.
  Array ids{};
  if (!r.array(rec + 0x1c8, ids, 64) || ids.count < 1)
    return BattleSnapshot{};
  std::array<int32_t, 8> pid{}, hp{}, sp{}, bp{};
  // Read only the entries the array holds (at most 8); the rest stay 0 and end the member list.
  if (!r.raw(ids.data, pid.data(), size_t(std::min(ids.count, 8)) * 4) || !r.ints(rec + 0x1d8, 8, hp.data()) ||
      !r.ints(rec + 0x1e8, 8, sp.data()) || !r.ints(rec + 0x1f8, 8, bp.data()))
    return BattleSnapshot{};
  for (int i = 0; i < 8 && pid[i] > 0; ++i) {
    if (hp[i] < 0 || hp[i] > 99999 || sp[i] < 0 || sp[i] > 9999 || bp[i] < 0 ||
        bp[i] > 5)
      return BattleSnapshot{};
    out.members[i] = {uint32_t(pid[i]), hp[i], sp[i], bp[i]};
    out.member_count = i + 1;
  }
  // Equipment records: S+0x158 TArray, stride 0x98, key = player id (job alias
  // for the protagonist), weapons TArray at +0x08 (cap 2). Missing record =
  // nothing equipped.
  Array equip{};
  if (!r.array(save + 0x158, equip, 64))
    return BattleSnapshot{};
  for (int e = 0; e < equip.count; ++e) {
    uint32_t key{};
    Array weapons{};
    const uint64_t rec_at = equip.data + uint64_t(e) * 0x98;
    if (!r.get(rec_at, key) || !r.get(rec_at + 8, weapons) ||
        weapons.count < 0 || weapons.count > 2 ||
        weapons.capacity < weapons.count || weapons.capacity > 8 ||
        (weapons.count && !Pointer(weapons.data, 4)))
      return BattleSnapshot{};
    for (int i = 0; i < out.member_count; ++i)
      if (battle_detail::Alias(out.members[i].id) == battle_detail::Alias(key) && weapons.count &&
          !r.raw(weapons.data, out.members[i].weapons.data(),
                 size_t(weapons.count) * 4))
        return BattleSnapshot{};
  }
  auto turns = [&](uint64_t at, std::array<TurnEntry, 16> &dst, int &count) {
    Array a{};
    std::array<uint64_t, 16> ptrs{};
    if (!r.array(at, a, 16) ||
        (a.count && !r.raw(a.data, ptrs.data(), size_t(a.count) * 8)))
      return false;
    for (int i = 0; i < a.count; ++i) {
      int e = -1;
      for (int k = 0; k < enemies.count; ++k)
        if (ptrs[i] == actors[k])
          e = k;
      if (e >= 0)
        dst[i] = {true, uint32_t(e)};
      else {
        uint32_t id{};
        if (!Pointer(ptrs[i]) || !r.get(ptrs[i] + 0x298, id))
          return false;
        dst[i] = {false, id};
      }
    }
    count = a.count;
    return true;
  };
  if (!turns(bm + 0x8b8, out.now, out.now_count) ||
      !turns(bm + 0x8f8, out.next, out.next_count) ||
      !r.get(bm + 0x8e0, out.now_index) || out.now_index < 0 ||
      out.now_index > out.now_count)
    return BattleSnapshot{};
  // Enemy HP uses the same battle-record order as the enemy actor array.
  // Optional and independently double-checked: unreadable/torn data must not
  // invalidate otherwise usable combat/weakness information.
  Array eh{}, eh2{};
  std::array<int32_t,6> enemy_hp{}, enemy_hp2{};
  std::array<uint64_t,6> actors2{};
  uint64_t manager2{}, save2{};
  if(r.array(rec+0x208,eh,6) && eh.count>=enemies.count && eh.capacity<=64 &&
     r.raw(eh.data,enemy_hp.data(),size_t(enemies.count)*4) &&
     std::all_of(enemy_hp.begin(),enemy_hp.begin()+enemies.count,[](int32_t v){return v>=0;}) &&
     r.get(rec+0x208,eh2) && eh2==eh &&
     r.raw(eh.data,enemy_hp2.data(),size_t(enemies.count)*4) && enemy_hp==enemy_hp2 &&
     r.raw(enemies.data,actors2.data(),size_t(enemies.count)*8) && actors2==actors &&
     r.ptr(main+0x7cc8540,manager2) && manager2==manager &&
     r.ptr(manager+0x2c8,save2) && save2==save) {
    for(int i=0;i<enemies.count;++i)out.enemies[i].hp=enemy_hp[static_cast<size_t>(i)];
  }
  // Re-check roots: never publish a snapshot torn by a battle transition.
  uint64_t check{}, vtable2{};
  Array e2{};
  uint8_t s2{};
  if (!r.ptr(main + 0x7cc8cc0, check) || check != root ||
      !r.get(root, vtable2) || vtable2 != vtable ||
      !r.ptr(main + 0x7cc5b78, check) || check != bm ||
      !r.get(bm + 0xa98, e2) || e2 != enemies || !r.get(root + 0x2f0, s2) ||
      s2 != 26)
    return BattleSnapshot{};
  out.valid = true;
  return out;
}
} // namespace ot0
