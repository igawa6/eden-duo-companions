// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_battle_selection.h"
#include "ot0_memory.h"
#include "ot0_resources.h"
#include <algorithm>
namespace ot0 {
namespace {
using Array = mem::Array;
// This file requires every guest address (including a TArray's data
// pointer) to be >= 0x1000000, not just non-null. Constructing the Reader
// below with minimum = 0x1000000 makes mem::Reader::get/ptr/object/array
// enforce that floor instead of the generic "non-null" one everyone else
// uses.
using Reader = mem::Reader;
struct Context {
  uint64_t app{}, bm{}, layout{}, control{}, command{}, list{}, cursor{},
      actor{};
  uint8_t state{}, step{}, list_type{}, ready{}, single{}, target_rule{},
      cursor_mode{};
  int32_t column{}, row_a{}, row_b{}, columns{}, items{}, selected{};
  Array targets{}, enemies{};
  bool operator==(const Context &) const = default;
};
struct Target {
  uint64_t actor{};
  uint32_t id{};
  int32_t order{};
  uint8_t team{};
  bool operator==(const Target &) const = default;
};
} // namespace
BattleSelection ReadBattleSelection(const ReadMemory &read, uint64_t main,
                                    const BattleSnapshot &battle,
                                    int64_t delta) {
  BattleSelection out;
  if (!read || !battle.valid || battle.member_count < 1 ||
      battle.member_count > 8 || battle.enemy_count < 0 ||
      battle.enemy_count > 6)
    return out;
  const auto actor_id = ReadCommandActor(read, main, battle, delta);
  if (!actor_id)
    return out;
  Reader r{read, main, delta, /*minimum=*/uint64_t{0x1000000}};
  const auto context = [&](Context &c) {
    if (!r.get(main + 0x7cc8cc0, c.app) || !r.object(c.app, 0x6207610) ||
        !r.get(c.app + 0x2f0, c.state) || c.state != 26 ||
        !r.get(main + 0x7cc5b78, c.bm) || !r.get(c.bm + 0x163c, c.step) ||
        (c.step != 2 && c.step != 3) || !r.get(c.bm + 0x1678, c.layout) ||
        !r.object(c.layout, 0x6221a30) || !r.get(c.layout + 0x2f8, c.control) ||
        !r.object(c.control, 0x62281d0) ||
        !r.get(c.control + 0x300, c.command) ||
        !r.object(c.command, 0x621cf90) || !r.get(c.command + 0x3a0, c.list) ||
        !r.object(c.list, 0x621b8e0) || !r.get(c.list + 0x320, c.actor) ||
        !r.object(c.actor, 0x63f39b8) || !r.get(c.list + 0x328, c.list_type) ||
        c.list_type < 1 || c.list_type > 7 || !r.get(c.list + 0x329, c.ready) ||
        !c.ready || !r.get(c.list + 0x2f8, c.cursor) ||
        !r.object(c.cursor, 0x646e168) || !r.get(c.cursor + 0x378, c.column) ||
        !r.get(c.cursor + 0x43c, c.row_a) ||
        !r.get(c.cursor + 0x444, c.row_b) ||
        !r.get(c.cursor + 0x4b8, c.cursor_mode) ||
        !r.get(c.cursor + 0x9dc, c.items) || c.items < 1 || c.items > 256 ||
        !r.get(c.cursor + 0x9e0, c.columns) || c.columns < 0 || c.columns > 256)
      return false;
    // Exact native cursor getter3bccf70: index378 + row43c/444 *9e0.
    //9e0 may be0 at the first selected item; never normalize it to1.
    const auto row = c.cursor_mode == 5 ? c.row_a : c.row_b;
    const int64_t index = int64_t(c.column) + int64_t(row) * c.columns;
    if (c.column < 0 || row < 0 || index < 0 || index >= c.items)
      return false;
    if (c.step == 3 && (!r.get(c.bm + 0x1610, c.single) || c.single > 1 ||
                        !r.get(c.bm + 0x1611, c.target_rule) ||
                        !r.get(c.control + 0x3b0, c.selected) ||
                        !r.array(c.bm + 0x1600, c.targets, 8, 32, 8) ||
                        !r.array(c.bm + 0xa98, c.enemies, 6, 32, 8) ||
                        c.enemies.count != battle.enemy_count))
      return false;
    return true;
  };
  Context c{}, checked{};
  if (!context(c))
    return out;
  out.actor_id = actor_id;
  out.list_type = c.list_type;
  out.command_index =
      c.column + (c.cursor_mode == 5 ? c.row_a : c.row_b) * c.columns;
  out.phase = BattleSelectionPhase::Command;
  std::array<Target, 8> targets{}, again{};
  std::array<uint64_t, 6> enemies{}, enemies2{};
  const auto sample_targets = [&](std::array<Target, 8> &values,
                                  std::array<uint64_t, 6> &enemy_values) {
    for (int i = 0; i < c.enemies.count; ++i) {
      uint32_t id{};
      if (!r.get(c.enemies.data + uint64_t(i) * 8, enemy_values[i]) ||
          !r.object(enemy_values[i], 0x63f39b8) ||
          !r.get(enemy_values[i] + 0x298, id) ||
          id != battle.enemies[i].enemy_id)
        return false;
      for (int j = 0; j < i; ++j)
        if (enemy_values[i] == enemy_values[j])
          return false;
    }
    for (int i = 0; i < c.targets.count; ++i) {
      auto &t = values[i];
      if (!r.get(c.targets.data + uint64_t(i) * 8, t.actor) ||
          !r.object(t.actor, 0x63f39b8) || !r.get(t.actor + 0x298, t.id) ||
          !t.id || !r.get(t.actor + 0x293, t.team) || t.team > 1)
        return false;
      if (t.team &&
          (!r.get(t.actor + 0xa40, t.order) || t.order < 0 || t.order > 255))
        return false;
      for (int j = 0; j < i; ++j)
        if (t.actor == values[j].actor)
          return false;
    }
    return true;
  };
  if (c.step == 3) {
    if (!c.targets.count || !sample_targets(targets, enemies))
      return {};
    out.target_count = c.targets.count;
    // Special grouped/front-back targeting is deliberately kept as overview.
    if (!c.single || (c.target_rule | 2) == 0xe2)
      out.phase = BattleSelectionPhase::GroupTargets;
    else {
      if (c.selected < 0 || c.selected >= c.targets.count)
        return {};
      const auto team = targets[0].team;
      for (int i = 0; i < c.targets.count; ++i)
        if (targets[i].team != team)
          return {};
      auto sorted = targets;
      if (team) {
        for (int i = 0; i < c.targets.count; ++i)
          for (int j = 0; j < i; ++j)
            if (targets[i].order == targets[j].order)
              return {};
        std::sort(
            sorted.begin(), sorted.begin() + c.targets.count,
            [](const Target &a, const Target &b) { return a.order < b.order; });
      }
      const auto &target = sorted[c.selected];
      if (team) {
        for (int i = 0; i < c.enemies.count; ++i)
          if (enemies[i] == target.actor)
            out.enemy_index = i;
        if (out.enemy_index < 0)
          return {};
        out.phase = BattleSelectionPhase::EnemyTarget;
      } else {
        for (int i = 0; i < battle.member_count; ++i)
          if (mem::Alias(battle.members[i].id) == mem::Alias(target.id)) {
            if (out.ally_id)
              return {};
            out.ally_id = battle.members[i].id;
          }
        if (!out.ally_id)
          return {};
        out.phase = BattleSelectionPhase::AllyTarget;
      }
    }
  }
  if (!context(checked) || checked != c ||
      ReadCommandActor(read, main, battle, delta) != actor_id ||
      (c.step == 3 && (!sample_targets(again, enemies2) || again != targets ||
                       enemies2 != enemies)))
    return {};
  out.valid = true;
  return out;
}
} // namespace ot0
