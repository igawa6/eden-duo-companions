// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_results.h"
#include <algorithm>
namespace ot0 {
void BattleResults::Observe(const Snapshot &s) {
  if (s.state == 10) { // returning to title retires the previous save's battle
    baseline_ = {};
    in_battle_ = false;
    last_ = {};
    return;
  }
  if (!s.valid)
    return;
  if (s.state == 26) {
    if (!in_battle_) { // battle start: the previous summary is replaced by this battle's
      baseline_ = s;
      in_battle_ = true;
      last_ = {};
    }
    return;
  }
  if (s.state != 23 || !in_battle_)
    return;
  in_battle_ = false;
  LastBattle next;
  for (const auto &m : s.members) {
    if (!m.available)
      continue;
    const auto before = std::find_if(baseline_.members.begin(), baseline_.members.end(),
                                     [&](const PartyMember &b) { return b.available && b.id == m.id; });
    if (before == baseline_.members.end())
      continue; // joined mid-battle: no baseline of its own
    if (m.exp > before->exp)
      next.exp = std::max(next.exp, m.exp - before->exp); // every member gains the same EXP
    if (m.level > before->level && next.levelup_count < 8)
      next.levelups[static_cast<size_t>(next.levelup_count++)] = m.id;
  }
  if (s.money > baseline_.money)
    next.money = s.money - baseline_.money;
  next.valid = next.exp || next.money || next.levelup_count;
  last_ = next;
}
} // namespace ot0
