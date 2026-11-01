// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// "Last battle" gains from native totals (live-verified 2026-10-30, research
// analysis/20261030-items-revival-results): the game adds EXP (party record +0x8), levels
// (+0xc) and leaves (save +0x40) when the results screen is dismissed. A baseline taken when
// battle starts and the totals back in the field give exactly what the results showed. A
// battle without gains (fled, lost) leaves nothing to show.
#pragma once
#include "ot0_reader.h"
#include <array>
#include <cstdint>
namespace ot0 {
struct LastBattle {
  bool valid{};
  uint32_t exp{}, money{};
  std::array<uint32_t, 8> levelups{}; // member ids that gained a level
  int levelup_count{};
  bool operator==(const LastBattle &) const = default;
};
class BattleResults {
public:
  // Feed every validated party snapshot (field 23, battle 26, others ignored).
  void Observe(const Snapshot &snapshot);
  const LastBattle &Last() const { return last_; }

private:
  Snapshot baseline_{};
  bool in_battle_{};
  LastBattle last_{};
};
} // namespace ot0
