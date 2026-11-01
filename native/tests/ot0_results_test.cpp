// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Values from the live 2026-10-30 fixture battles (+17 EXP, +205 leaves, two level-ups).
#include "../modules/ot0_results.h"
#include <cassert>
#include <iostream>
namespace {
ot0::Snapshot Party(uint8_t state, uint32_t money, uint32_t exp, uint32_t phenn_level) {
  ot0::Snapshot s;
  s.valid = true;
  s.state = state;
  s.money = money;
  s.members[0] = {901, 15, 727, 103, true};
  s.members[0].exp = exp;
  s.members[1] = {10905, phenn_level, 857, 60, true};
  s.members[1].exp = exp - 100;
  return s;
}
} // namespace
int main() {
  ot0::BattleResults r;
  r.Observe(Party(23, 7340, 1405, 14));
  assert(!r.Last().valid);
  r.Observe(Party(26, 7340, 1405, 14)); // battle start
  r.Observe(Party(26, 7340, 1405, 14)); // results showing: nothing applied yet
  assert(!r.Last().valid);
  r.Observe(Party(23, 7545, 1422, 15)); // dismissed: native totals updated
  const auto &l = r.Last();
  assert(l.valid && l.exp == 17 && l.money == 205 && l.levelup_count == 1 && l.levelups[0] == 10905);
  r.Observe(Party(23, 7545, 1422, 15)); // stays until the next battle
  assert(r.Last().valid);
  r.Observe(Party(26, 7545, 1422, 15)); // next battle clears it
  assert(!r.Last().valid);
  r.Observe(Party(23, 7545, 1422, 15)); // fled: no gains, nothing shown
  assert(!r.Last().valid);
  ot0::Snapshot bad;
  r.Observe(bad); // invalid snapshots are ignored
  r.Observe(Party(26, 7340, 1405, 14));
  ot0::Snapshot title;
  title.state = 10;
  r.Observe(title);
  r.Observe(Party(23, 9000, 2000, 16)); // a different save is not a battle reward
  assert(!r.Last().valid);
  r.Observe(Party(26, 7340, 1405, 14));
  r.Observe(Party(23, 7545, 1422, 15));
  assert(r.Last().valid);
  r.Observe(title);
  assert(!r.Last().valid);
  std::cout << "ot0 results ok\n";
}
