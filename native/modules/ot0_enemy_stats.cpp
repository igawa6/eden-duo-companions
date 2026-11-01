// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_enemy_stats.h"
#include "ot0_battle_cache.h"
#include <algorithm>
#include <cstring>
#include <vector>
namespace ot0 {
namespace {
using battle_detail::Array;
using battle_detail::Cache;
using battle_detail::Cached;
using battle_detail::Limit;
using battle_detail::Pointer;
using battle_detail::Reader;
struct Context {
  uint64_t app{}, bm{};
  Array actors{};
  std::array<uint64_t, 6> pointers{};
  bool operator==(const Context &) const = default;
};
bool ContextRead(const Reader &r, const BattleSnapshot &b, Context &c) {
  uint8_t state{};
  uint32_t flags{};
  const bool valid =
      r.ptr(r.main + 0x7cc8cc0, c.app) && r.object(c.app, 0x6207610) &&
      r.get(c.app + 0x2f0, state) && state == 26 &&
      r.ptr(r.main + 0x7cc5b78, c.bm) && r.get(c.bm + 8, flags) &&
      !(flags & 0x40000030) && r.get(c.bm + 0xa98, c.actors) &&
      Pointer(c.actors.data) && c.actors.count == b.enemy_count &&
      c.actors.capacity >= c.actors.count && c.actors.capacity <= 64 &&
      size_t(c.actors.count) * 8 <= Limit - c.actors.data &&
      r.read(c.actors.data, c.pointers.data(), size_t(c.actors.count) * 8);
  if (!valid)
    return false;
  for (int i = 0; i < c.actors.count; ++i) {
    if (!Pointer(c.pointers[i]))
      return false;
    for (int j = 0; j < i; ++j)
      if (c.pointers[i] == c.pointers[j])
        return false;
  }
  return true;
}
} // namespace
std::array<EnemyStats, 6> ReadEnemyStats(const ReadMemory &read, uint64_t main,
                                         const BattleSnapshot &battle,
                                         int64_t delta) {
  std::array<EnemyStats, 6> result{};
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8 || !battle.valid ||
      battle.enemy_count < 1 || battle.enemy_count > 6)
    return result;
  Reader r{read, main, delta};
  Context before{}, after{};
  if (!ContextRead(r, battle, before))
    return result;
  for (int i = 0; i < battle.enemy_count; ++i) {
    uint64_t actor = before.pointers[i];
    uint8_t team{};
    uint32_t id{};
    const auto identity = [&] {
      return r.object(actor, 0x63f39b8) && r.get(actor + 0x293, team) &&
             team == 1 && r.get(actor + 0x298, id) &&
             id == battle.enemies[i].enemy_id;
    };
    if (!identity())
      continue;
    Cached a{}, b{};
    if (Cache(r, actor, 1, a, true) && Cache(r, actor, 1, b, true) && a == b) {
      result[i].max_hp = std::max({a.value, a.floor, a.minimum, 1});
      result[i].max_hp_valid = true;
    }
    // Native380a5c8..5d8 calls382e264(actor,13,null,0), which forwards
    // flags1/1/0 to382da50. Parameter13 is source-named Speed in native UI.
    Cached speed_a{}, speed_b{};
    if (Cache(r, actor, 13, speed_a) && Cache(r, actor, 13, speed_b) &&
        speed_a == speed_b) {
      result[i].speed = speed_a.value;
      result[i].speed_valid = true;
    }
    if (!identity())
      result[i] = {};
  }
  if (!ContextRead(r, battle, after) || before != after)
    return {};
  return result;
}
} // namespace ot0
