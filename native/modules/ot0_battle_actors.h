// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle.h"
#include "ot0_battle_cache.h"
#include "ot0_memory.h"
namespace ot0::battle_detail {
// Forwards to the one definition of the protagonist alias, shared with
// ot0_resources.cpp/ot0_equipment.cpp/ot0_battle_selection.cpp.
inline uint32_t Alias(uint32_t id) { return ot0::mem::Alias(id); }
struct Context {
  uint64_t app{}, bm{};
  Array front{}, back{}, enemy{};
  std::array<uint64_t, 4> fronts{}, backs{};
  std::array<uint64_t, 6> enemies{};
  bool operator==(const Context &) const = default;
};
inline bool PointerArray(const Reader &r, uint64_t p, Array &a, uint64_t *out,
                         int max) {
  if (!r.get(p, a) || a.count < 0 || a.count > max || a.capacity < a.count ||
      a.capacity > 64)
    return false;
  return !a.count ||
         (Pointer(a.data) && size_t(a.count) * 8 <= Limit - a.data &&
          r.read(a.data, out, size_t(a.count) * 8));
}
inline bool Sample(const Reader &r, const BattleSnapshot &b, Context &c) {
  uint8_t state{};
  uint32_t flags{};
  return r.ptr(r.main + 0x7cc8cc0, c.app) && r.object(c.app, 0x6207610) &&
         r.get(c.app + 0x2f0, state) && state == 26 &&
         r.ptr(r.main + 0x7cc5b78, c.bm) && r.get(c.bm + 8, flags) &&
         !(flags & 0x40000030) &&
         PointerArray(r, c.bm + 0x9b8, c.front, c.fronts.data(), 4) &&
         PointerArray(r, c.bm + 0x9c8, c.back, c.backs.data(), 4) &&
         PointerArray(r, c.bm + 0xa98, c.enemy, c.enemies.data(), 6) &&
         c.enemy.count == b.enemy_count;
}
inline bool Identity(const Reader &r, uint64_t actor, uint8_t team,
                     uint32_t id) {
  uint8_t actual_team{};
  uint32_t actual_id{};
  return r.object(actor, 0x63f39b8) && r.get(actor + 0x293, actual_team) &&
         actual_team == team && r.get(actor + 0x298, actual_id) &&
         (team == 0 ? Alias(actual_id) == Alias(id) : actual_id == id);
}
struct BoundActors {
  Context context;
  std::array<uint64_t, 8> party{};
};
inline bool BindActors(const Reader &r, const BattleSnapshot &b,
                       BoundActors &out) {
  if (!Sample(r, b, out.context))
    return false;
  const auto join = [&](const auto &pointers, int count) {
    for (int i = 0; i < count; ++i) {
      uint64_t actor = pointers[i];
      if (!actor)
        continue;
      uint32_t id{};
      if (!r.get(actor + 0x298, id))
        return false;
      int slot = -1;
      for (int j = 0; j < b.member_count; ++j)
        if (Alias(id) == Alias(b.members[j].id)) {
          if (slot >= 0)
            return false;
          slot = j;
        }
      if (slot < 0 || out.party[slot] ||
          !Identity(r, actor, 0, b.members[slot].id))
        return false;
      for (auto p : out.party)
        if (p == actor)
          return false;
      out.party[slot] = actor;
    }
    return true;
  };
  if (!join(out.context.fronts, out.context.front.count) ||
      !join(out.context.backs, out.context.back.count))
    return false;
  for (int i = 0; i < b.member_count; ++i)
    if (!out.party[i])
      return false;
  for (int i = 0; i < b.enemy_count; ++i) {
    auto actor = out.context.enemies[i];
    if (!Pointer(actor) || !Identity(r, actor, 1, b.enemies[i].enemy_id))
      return false;
    for (int j = 0; j < i; ++j)
      if (out.context.enemies[j] == actor)
        return false;
  }
  return true;
}
inline bool SameActors(const Reader &r, const BattleSnapshot &b,
                       const BoundActors &before) {
  for (int i = 0; i < b.member_count; ++i)
    if (!Identity(r, before.party[i], 0, b.members[i].id))
      return false;
  for (int i = 0; i < b.enemy_count; ++i)
    if (!Identity(r, before.context.enemies[i], 1, b.enemies[i].enemy_id))
      return false;
  Context after{};
  return Sample(r, b, after) && before.context == after;
}
} // namespace ot0::battle_detail
