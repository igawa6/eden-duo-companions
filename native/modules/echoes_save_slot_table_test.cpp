// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_save_slot_table.h"
#include <cstdlib>
#include <iostream>
using namespace echoes;
int main() {
  int checks=0;
  const auto check=[&](bool ok) { ++checks; if(!ok) {std::cerr<<"check "<<checks<<" failed\n";std::exit(1);} };
  constexpr uint16_t E=QuickSlots::Empty;

  // A: unknown identity is simply not found; identity 0 is never looked up
  // or stored (reserved for "unused").
  SaveSlotTable t;
  check(!t.Find(1234).has_value());
  t.Store(0, {10,20,30});
  check(t.TrackedCount()==0);

  // B: store + find round-trips for one save, in-memory.
  t.Store(0xAAAA1111u, {10,20,30});
  check(t.TrackedCount()==1);
  auto found = t.Find(0xAAAA1111u);
  check(found.has_value()); check((*found)[0]==10 && (*found)[1]==20 && (*found)[2]==30);

  // C: per-save isolation -- a second, different identity gets its own
  // independent bucket; neither Find() call ever returns the other's slots.
  t.Store(0xBBBB2222u, {E,40,E});
  check(t.TrackedCount()==2);
  check(*t.Find(0xAAAA1111u) == SaveSlotTable::Slots{10,20,30});
  check(*t.Find(0xBBBB2222u) == SaveSlotTable::Slots{E,40,E});

  // D: re-storing an already-tracked identity refreshes it in place --
  // tracked count does not grow, and age moves forward.
  t.Store(0xAAAA1111u, {E,E,99});
  check(t.TrackedCount()==2);
  check(*t.Find(0xAAAA1111u) == SaveSlotTable::Slots{E,E,99});

  // E: encode/decode round-trips the whole table through the exact
  // SaveSlotBridge::Table shape the persisted flags carry -- this is what
  // survives an app restart. A fresh table decoded from it must reproduce
  // every tracked save's shortcuts exactly, with no cross-save mixing.
  const auto raw = t.Encode();
  SaveSlotTable restored;
  restored.Decode(raw);
  check(restored.TrackedCount()==2);
  check(*restored.Find(0xAAAA1111u) == SaveSlotTable::Slots{E,E,99});
  check(*restored.Find(0xBBBB2222u) == SaveSlotTable::Slots{E,40,E});
  check(!restored.Find(0xCCCC3333u).has_value());

  // F: filling every bucket, then one more save evicts the least-recently
  // WRITTEN one (lowest age), never a more recently touched save.
  SaveSlotTable full;
  for (uint32_t i=0;i<SaveSlotTable::Buckets;++i)
    full.Store(0x1000u+i, {static_cast<uint16_t>(i),E,E});
  check(full.TrackedCount()==SaveSlotTable::Buckets);
  // Touch every save except the very first again, so it is now the oldest.
  for (uint32_t i=1;i<SaveSlotTable::Buckets;++i)
    full.Store(0x1000u+i, {static_cast<uint16_t>(i),1,E});
  full.Store(0x9999u, {7,7,7}); // one more than fits
  check(full.TrackedCount()==SaveSlotTable::Buckets); // still bounded, not 9
  check(!full.Find(0x1000u).has_value()); // the untouched (oldest) one was evicted
  check(full.Find(0x9999u).has_value());
  for (uint32_t i=1;i<SaveSlotTable::Buckets;++i)
    check(full.Find(0x1000u+i).has_value()); // every touched save survived

  // G: Migrate follows a same-save identity drift (e.g. the player just
  // learned a new Echo, changing the identity without a save switch): the
  // pins stay reachable under the NEW identity, and the OLD identity is
  // freed rather than left behind as an orphaned bucket -- this is the
  // "learn an Echo mid-session -> pins kept" scenario.
  SaveSlotTable g;
  g.Store(0x5000u, {1,2,3});
  check(g.TrackedCount()==1);
  g.Migrate(0x5000u, 0x5001u); // drift: same save, identity moved on
  check(g.TrackedCount()==1); // still one save tracked, not two
  check(!g.Find(0x5000u).has_value()); // the old key is gone
  check(*g.Find(0x5001u) == SaveSlotTable::Slots{1,2,3}); // pins intact under the new key

  // H: Migrate is a deliberate, explicit call -- it never fires on its own,
  // so a real save switch (which simply never calls it) shows nothing of
  // the previous save, exactly like loading any other never-visited save.
  // This is the "switch save -> no migration" contrast: Find() on the new
  // save's own (different) identity returns nothing, never 0x5001's pins.
  check(!g.Find(0x6000u).has_value());

  // I: Migrate no-ops when there is nothing to move, or the two identities
  // are the same, or either is the reserved 0 -- never fabricates a bucket.
  SaveSlotTable h;
  h.Migrate(0, 0x7000u);
  h.Migrate(0x7000u, 0);
  h.Migrate(0x7000u, 0x7000u);
  h.Migrate(0x7000u, 0x7001u); // 0x7000 was never stored -- nothing to move
  check(h.TrackedCount()==0);
  check(!h.Find(0x7000u).has_value() && !h.Find(0x7001u).has_value());

  // J: if the identity drifts onto one that ALREADY has a different
  // save's entry (an identity collision -- the same risk every lookup
  // already accepts), the drifting save's own pins win; the old bucket is
  // freed, so the table never grows past what it tracked before.
  SaveSlotTable i;
  i.Store(0x8000u, {5,6,7});   // some other save, already at this key
  i.Store(0x8001u, {8,9,10});  // the save that is about to drift
  check(i.TrackedCount()==2);
  i.Migrate(0x8001u, 0x8000u);
  check(i.TrackedCount()==1); // the two buckets collapsed into one
  check(*i.Find(0x8000u) == SaveSlotTable::Slots{8,9,10}); // the drifting save's own pins
  check(!i.Find(0x8001u).has_value());

  // K: "restart after drift -> pins restored" -- Migrate then an
  // Encode/Decode round-trip (what an app restart does) must still find
  // the pins under the save's now-current identity.
  SaveSlotTable j;
  j.Store(0x9000u, {E,11,E});
  j.Migrate(0x9000u, 0x9002u); // the save drifted once more before restart
  const auto raw2 = j.Encode();
  SaveSlotTable restarted;
  restarted.Decode(raw2);
  check(*restarted.Find(0x9002u) == SaveSlotTable::Slots{E,11,E});
  check(!restarted.Find(0x9000u).has_value());

  std::cout<<checks<<" save-slot-table checks passed\n";
}
