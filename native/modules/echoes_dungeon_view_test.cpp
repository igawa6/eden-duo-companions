// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon_view.h"
#include <iostream>
#include <stdexcept>
namespace {
size_t checks{};
void Check(bool ok, const char *why) {
  ++checks;
  if (!ok)
    throw std::runtime_error(why);
}
} // namespace
int main() {
  try {
    echoes::DungeonViewCache cache;
    echoes::DungeonViewState state;
    state.key = "L_Dungeon001";
    state.floor = 1;
    state.source.save_epoch.revision = 1;
    state.visited = 2;
    state.variants = {{1, true, {}}};
    auto first = cache.Accept(state);
    Check(first && *first == 0, "first authenticated floor identity");
    state.floor = 2;
    auto second = cache.Accept(state);
    Check(second && *second != *first, "floors have independent identities");
    const auto generation = cache.generation();
    for (size_t i = 0; i < 2048; ++i) {
      state.floor = (i & 1) ? 2 : 1;
      auto slot = cache.Accept(state);
      Check(slot && slot == ((i & 1) ? second : first),
            "floor loop reuses exact per-floor widget");
    }
    Check(cache.used() == 2 && cache.generation() == generation,
          "more than64 switches never exhaust floor identities");
    state.visited = 6;
    Check(cache.Accept(state) == second,
          "newly visited rooms reuse a less-revealing widget");
    state.floor = 1;
    Check(cache.Accept(state) == first && cache.slots()[*first].visited == 6,
          "returning floor receives new discovery without allocating");
    state.map_item = true;
    Check(cache.Accept(state) == first,
          "acquiring map reuses less-revealing widget");
    state.floor = 2;
    Check(cache.Accept(state) == second && cache.slots()[*second].map_item,
          "map acquisition refreshes other floor lazily");
    const auto old_second = second;
    state.visited = 2;
    second = cache.Accept(state);
    Check(second && second != old_second && cache.generation() > generation,
          "discovery rollback creates a fresh generation");
    state.floor = 1;
    auto fresh_first = cache.Accept(state);
    Check(fresh_first && fresh_first != first,
          "rollback retires other floor identities too");
    state.visited = 6;
    Check(cache.Accept(state) == fresh_first,
          "regrowth does not reactivate old generation");
    state.floor = 2;
    Check(cache.Accept(state) == second && cache.Accept(state) != old_second,
          "returning exact prior discovery cannot resurrect retired widget");
    const auto before_loss = cache.current();
    state.map_item = false;
    Check(cache.Accept(state) != before_loss,
          "map-item loss creates fresh widget");
    const auto after_loss = cache.current();
    state.map_item = true;
    Check(cache.Accept(state) == after_loss && cache.current() != before_loss,
          "map-item regain cannot resurrect an earlier generation");
    const auto before_variant = cache.current();
    state.variants[0].texture = "L_Dungeon001_001_c_00";
    Check(cache.Accept(state) != before_variant,
          "conditional terrain change creates fresh widget");
    const auto before_unknown = cache.current();
    state.variants.clear();
    Check(cache.Accept(state) != before_unknown,
          "unknown conditional state retires visible conditional art");
    const auto before_gap = cache.current();
    cache.Withdraw();
    Check(!cache.current(),
          "observation gap immediately withdraws active identity");
    Check(cache.Accept(state) != before_gap,
          "same state after gap cannot reuse old widget");
    const auto before_source = cache.current();
    state.source.save_epoch.revision++;
    Check(cache.Accept(state) != before_source,
          "native save source change creates fresh widget");
    const auto before_key = cache.current();
    state.key = "L_Dungeon002";
    Check(cache.Accept(state) != before_key,
          "native map change creates fresh widget");
    const auto before_bad = cache.current();
    auto bad = state;
    bad.key.clear();
    Check(!cache.Accept(bad) && !cache.current(),
          "invalid scope withdraws current identity");
    Check(cache.Accept(state) != before_bad,
          "invalid observation cannot preserve continuity");
    // Capacity remains bounded across real epoch gaps. Old identities are never
    // recycled to conceal exhaustion, including when the exact state returns.
    while (cache.used() < echoes::DungeonViewCache::Capacity) {
      cache.Withdraw();
      Check(bool(cache.Accept(state)), "bounded new epoch allocation");
    }
    const auto final = cache.current();
    Check(cache.Accept(state) == final,
          "capacity permits current continuous floor reuse");
    cache.Withdraw();
    Check(!cache.Accept(state) && !cache.current(),
          "full cache refuses fresh epoch without stale fallback");
    Check(cache.used() == echoes::DungeonViewCache::Capacity,
          "cache never grows beyond manifest widget capacity");
    std::cout << checks << " dungeon view identity checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
