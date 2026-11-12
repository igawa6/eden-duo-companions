// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_quick_slots.h"
#include <cstdlib>
#include <iostream>
using namespace echoes;
int main() {
  int checks=0;
  const auto check=[&](bool ok) { ++checks; if(!ok) {std::cerr<<"check "<<checks<<" failed\n";std::exit(1);} };

  // A: never ready before the first Sync, and an incompatible/unreadable
  // sample reports no candidates at all.
  QuickSlots q; LiveSnapshot v;
  check(!q.Target(0)); q.Sync(v); check(!q.ready()); check(!q.active());

  // B: favourites fill first, from the independently-sampled save CRC set
  // (mapped through learned_crc_keys), even though the held selector has
  // never been opened this session -- so no Last Used order is cached yet
  // and the other two slots stay empty rather than falling back to
  // native-ID order.
  v.compatible=v.collection_valid=true; v.collection_source={1,2,3,0};
  v.learned_native_ids={10,20,30,40,50}; v.learned_crc_keys={100,200,300,400,500};
  v.favorite_valid=true; v.favorite_crc_keys={400};
  q.Sync(v);
  check(q.ready()); check(q.Target(0)==40); check(!q.Target(1)); check(!q.Target(2)); check(!q.active());
  check(q.ConsumeOwnershipChange()); // first save seen this boot

  // C: holding the selector sorted Last Used (sort_enum bit 0) supplies the
  // remaining order; the favourite (40) still comes first.
  v.selector.compatible=v.selector.selector_valid=v.selector.held=true;
  v.selector.sort_valid=true; v.selector.sort_enum=1;
  v.selector.ordered_native_ids={20,10,40,30,50};
  q.Sync(v);
  check(q.Target(0)==40); check(q.Target(1)==20); check(q.Target(2)==10);
  check(!q.ConsumeOwnershipChange()); // same save, no repeat signal

  // D: closing the selector on the same save keeps showing the cached Last
  // Used order -- the rail does not go blank just because the picker isn't
  // currently held.
  v.selector.held=false; v.selector.ordered_native_ids.clear();
  q.Sync(v);
  check(q.Target(0)==40); check(q.Target(1)==20); check(q.Target(2)==10);

  // E: reopening the selector with a DIFFERENT active sort (not Last Used)
  // must not clobber the cached Last Used order with that sort's order.
  v.selector.held=true; v.selector.sort_enum=8; // e.g. Cost
  v.selector.ordered_native_ids={50,30,20,10,40};
  q.Sync(v);
  check(q.Target(0)==40); check(q.Target(1)==20); check(q.Target(2)==10);

  // F: the selector's own favourites-page order (exact game order) takes
  // priority over the CRC-derived fallback order once it is readable, and
  // the currently-equipped Echo is reported as active() by its slot.
  v.selector.sort_enum=1; v.selector.ordered_native_ids={30,50,20,10,40};
  v.selector.favorite_valid=true; v.selector.favorite_native_ids={20,40};
  v.selector.equipped_valid=true; v.selector.equipped_native_id=40;
  q.Sync(v);
  check(q.Target(0)==20); check(q.Target(1)==40); check(q.Target(2)==30);
  check(q.active()==1);

  // G: a transient unreadable sample (a loading screen, most commonly) hides
  // the circles for that one sample, but -- unlike a confirmed different
  // save -- it must NOT clear the cached favourites/Last Used order: the
  // moment the very same save is readable again, the circles come back
  // exactly as they were, with no re-hold of the picker needed.
  LiveSnapshot loading; // a bare, unreadable sample: compatible defaults false
  q.Sync(loading);
  check(!q.ready()); check(!q.Target(0)); check(!q.Target(1)); check(!q.Target(2)); check(!q.active());
  check(!q.ConsumeOwnershipChange()); // an unreadable sample is not a save change
  q.Sync(v); // the same save's next real sample, selector closed again
  v.selector.held=false;
  check(q.ready()); check(q.Target(0)==20); check(q.Target(1)==40); check(q.Target(2)==30);
  check(q.active()==1);
  check(!q.ConsumeOwnershipChange());

  // H: no cross-save leakage -- a different save (different save_data) on
  // the very next sample, with no favourites/Last Used known for it yet,
  // shows nothing from the old save even though the collection itself
  // reads as valid again immediately. This IS a confirmed save change, so
  // caches (and any hand assignment) are dropped, and the caller is told.
  QuickSlots qh; LiveSnapshot h1;
  h1.compatible=h1.collection_valid=true; h1.collection_source={1,2,3,0};
  h1.learned_native_ids={5,6,7}; h1.learned_crc_keys={50,60,70};
  h1.selector.compatible=h1.selector.selector_valid=h1.selector.held=true;
  h1.selector.sort_valid=true; h1.selector.sort_enum=1;
  h1.selector.ordered_native_ids={6,5,7};
  qh.Sync(h1);
  check(qh.ready()); check(qh.Target(0)==6); check(qh.Target(1)==5); check(qh.Target(2)==7);
  check(qh.ConsumeOwnershipChange());
  check(qh.Assign(0,6)); // hand-pin slot 0 on this save
  check(qh.dirty()); qh.ClearDirty();
  LiveSnapshot h2=h1;
  h2.collection_source.save_data=99; // a different save loaded
  h2.learned_native_ids={1,2}; h2.learned_crc_keys={10,20};
  h2.selector={}; // the picker is closed right after a fresh load, as usual
  qh.Sync(h2);
  check(qh.ready()); // the new save's own collection is readable
  check(!qh.Target(0)); check(!qh.Target(1)); check(!qh.Target(2)); // never 6/5/7, never the hand pin
  check(!qh.active());
  check(qh.ConsumeOwnershipChange()); // the caller must now resolve h2's own save identity
  check(qh.manual()==std::array<uint16_t,3>{QuickSlots::Empty,QuickSlots::Empty,QuickSlots::Empty});

  // I: a favourite whose CRC no longer resolves within the currently
  // learned set (e.g. after a de-sync) is dropped rather than shown stale.
  QuickSlots qi; LiveSnapshot i1;
  i1.compatible=i1.collection_valid=true; i1.collection_source={1,2,3,0};
  i1.learned_native_ids={1,2,3}; i1.learned_crc_keys={10,20,30};
  i1.favorite_valid=true; i1.favorite_crc_keys={20};
  qi.Sync(i1);
  check(qi.ready()); check(qi.Target(0)==2); check(!qi.Target(1));
  LiveSnapshot i2=i1;
  i2.learned_native_ids={1,3}; i2.learned_crc_keys={10,30}; // id 2 (crc 20) no longer learned
  qi.Sync(i2);
  check(qi.ready()); check(!qi.Target(0)); check(!qi.Target(1)); check(!qi.Target(2));

  // J: the same gate also applies directly to a cached Last Used id (no
  // CRC mapping involved at all) once it drops out of the learned set.
  QuickSlots qj; LiveSnapshot j1;
  j1.compatible=j1.collection_valid=true; j1.collection_source={1,2,3,0};
  j1.learned_native_ids={1,2,3}; j1.learned_crc_keys={10,20,30};
  j1.selector.compatible=j1.selector.selector_valid=j1.selector.held=true;
  j1.selector.sort_valid=true; j1.selector.sort_enum=1;
  j1.selector.ordered_native_ids={2,1,3};
  qj.Sync(j1);
  check(qj.ready()); check(qj.Target(0)==2); check(qj.Target(1)==1); check(qj.Target(2)==3);
  LiveSnapshot j2=j1;
  j2.learned_native_ids={1,3}; j2.learned_crc_keys={10,30}; // id 2 no longer learned
  j2.selector.held=false; j2.selector.ordered_native_ids.clear(); // picker closed; cache still holds {2,1,3}
  qj.Sync(j2);
  check(qj.ready()); check(qj.Target(0)==1); check(qj.Target(1)==3); check(!qj.Target(2));

  // K: manual assignment (tap-grid-then-slot / drag) takes its own fixed
  // circle ahead of any auto-fill, duplicate assignment swaps the two
  // hand-assigned circles, and an unassigned circle keeps auto-filling.
  QuickSlots qk; LiveSnapshot k1;
  k1.compatible=k1.collection_valid=true; k1.collection_source={1,2,3,0};
  k1.learned_native_ids={1,2,3,4,5}; k1.learned_crc_keys={10,20,30,40,50};
  k1.selector.compatible=k1.selector.selector_valid=k1.selector.held=true;
  k1.selector.sort_valid=true; k1.selector.sort_enum=1;
  k1.selector.ordered_native_ids={3,2,1,4,5};
  qk.Sync(k1);
  check(qk.Target(0)==3); check(qk.Target(1)==2); check(qk.Target(2)==1); // pure auto-fill so far
  check(!qk.Assign(5,1)); // out-of-range slot refused
  check(!qk.Assign(0,99)); // not learned, refused
  check(qk.Assign(2,5)); // hand-pin slot 2 to Echo 5 (not currently shown anywhere)
  qk.Sync(k1);
  check(qk.Target(0)==3); check(qk.Target(1)==2); check(qk.Target(2)==5); // slot 2 now fixed, auto-fill yields it the circle
  check(qk.Assign(0,5)); // 5 is already hand-pinned to slot 2 -> duplicate assignment swaps
  check(qk.manual()==std::array<uint16_t,3>{5,QuickSlots::Empty,QuickSlots::Empty}); // the pin moved to slot 0; slot 2's pin is gone, free for auto-fill
  qk.Sync(k1);
  check(qk.Target(0)==5); check(qk.Target(1)==3); check(qk.Target(2)==2); // slot 2 now auto-fills again

  // L: "save as reference" -- a hand assignment is only shown while that
  // Echo is still learned in this exact save; losing it (never happens in
  // play, but defends against an imperfect persisted-identity match) falls
  // the circle back to auto-fill without discarding the stored pin, and it
  // reappears the moment the Echo is learned/learnable again.
  LiveSnapshot k2=k1;
  k2.learned_native_ids={1,2,3,4}; k2.learned_crc_keys={10,20,30,40}; // Echo 5 no longer learned
  qk.Sync(k2);
  check(qk.Target(0)!=5); // the stale hand pin for slot 0 does not show
  check(qk.manual()[0]==5); // but it is still stored, not forgotten
  qk.Sync(k1); // Echo 5 learned again (e.g. transient read glitch undone)
  check(qk.Target(0)==5);

  // M: tapping a circle (Target()) is unaffected by any of the above --
  // it always just names whatever Echo that circle currently shows.
  check(qk.Target(1)==3);

  std::cout<<checks<<" quick-slot checks passed\n";
}
