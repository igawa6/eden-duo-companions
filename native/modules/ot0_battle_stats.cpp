// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_battle_stats.h"
#include "ot0_battle_actors.h"
#include "ot0_battle_signature.h"
namespace ot0 {
namespace {
using namespace battle_detail;
void Parameters(const Reader &r, uint64_t actor, ActorCombatStats &out) {
  auto before = ReadActorSignature(r, actor);
  out.native_actor = actor;
  for (size_t k = 0; k < CombatParameterIds.size(); ++k) {
    Cached a{}, b{};
    if (Cache(r, actor, CombatParameterIds[k], a) &&
        Cache(r, actor, CombatParameterIds[k], b) && a == b)
      out.parameters[k] = {a.value, true};
  }
  auto after = ReadActorSignature(r, actor);
  out.native_serial = after.native_serial;
  out.raw_ailment_fingerprint = after.raw_ailment_fingerprint;
  out.weapon_selector = after.weapon_selector;
  out.signature_valid = before.valid && after.valid && before == after;
}
} // namespace
BattleStatsSnapshot ReadBattleStats(const ReadMemory &read, uint64_t main,
                                    const BattleSnapshot &b, int64_t delta) {
  BattleStatsSnapshot out{};
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8 || !b.valid ||
      b.member_count < 1 || b.member_count > 8 || b.enemy_count < 1 ||
      b.enemy_count > 6)
    return out;
  Reader r{read, main, delta};
  BoundActors actors{};
  if (!BindActors(r, b, actors))
    return out;
  for (int i = 0; i < b.member_count; ++i) {
    out.party[i].id = b.members[i].id;
    out.party[i].bound = true;
    out.party[i].native_actor = actors.party[i];
    uint8_t selector{}, again{};
    if (r.get(actors.party[i] + 0x2a8, selector) && selector <= 1) {
      Parameters(r, actors.party[i], out.party[i]);
      if (r.get(actors.party[i] + 0x2a8, again) && again == selector) {
        out.party[i].equipment_weapon_slot = selector ? 0 : 1;
        out.party[i].weapon_context_valid = true;
      } else {
        out.party[i].parameters = {};
        out.party[i].signature_valid = false;
      }
    }
  }
  for (int i = 0; i < b.enemy_count; ++i) {
    out.enemies[i].id = b.enemies[i].enemy_id;
    out.enemies[i].bound = true;
    Parameters(r, actors.context.enemies[i], out.enemies[i]);
  }
  if (!SameActors(r, b, actors))
    return {};
  out.valid = true;
  return out;
}
} // namespace ot0
