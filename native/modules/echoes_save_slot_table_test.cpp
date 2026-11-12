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

  std::cout<<checks<<" save-slot-table checks passed\n";
}
