// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_drive.h"
#include "echoes_direct_select.h"
#include "echoes_direct_sort.h"
#include <map>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using namespace echoes;
size_t checks{};
void Check(bool ok, std::string_view what) {
  ++checks;
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    std::exit(1);
  }
}
LiveSnapshot Held(size_t count = 5) {
  LiveSnapshot s;
  s.compatible = s.collection_valid = true;
  s.collection_source = {0x1000, 0x2000, 0x3000, 7};
  auto& p = s.selector;
  p.compatible = p.selector_valid = p.held = p.equipped_valid = true;
  p.equipped_native_id = 10;
  const uint64_t end = 0x5000 + static_cast<uint64_t>(count) * 0x38;
  p.source = {0x4000, 0x5000, end, end, 1, 2};
  for (size_t i = 0; i < count; ++i) {
    p.ordered_native_ids.push_back(static_cast<uint16_t>(10 + i));
    s.learned_native_ids.push_back(static_cast<uint16_t>(10 + i));
  }
  p.highlighted_index = p.target_index = 0;
  p.highlighted_native_id = 10;
  return s;
}
LiveSnapshot Closed() {
  auto s = Held();
  s.selector.held = false;
  s.selector.source.native_state = 0;
  s.selector.source.records = s.selector.source.end =
      s.selector.source.capacity = 0;
  s.selector.ordered_native_ids.clear();
  s.selector.highlighted_index = s.selector.target_index = -1;
  s.selector.highlighted_native_id = 0xffff;
  return s;
}
void Cursor(LiveSnapshot& s, int32_t actual, int32_t target) {
  s.selector.highlighted_index = actual;
  s.selector.target_index = target;
  s.selector.highlighted_native_id =
      s.selector.ordered_native_ids[static_cast<size_t>(actual)];
}
void Start(SelectorDrive& d, const LiveSnapshot& s, uint16_t id) {
  Check(d.Advance(Closed(), 0) == Direction::None, "closed emits nothing");
  Check(d.Request(id, s, 1), "intention accepted after observed closed");
  Check(d.status() == DriveStatus::Pending, "request is not completion");
}
void FirstRight(SelectorDrive& d, const LiveSnapshot& s) {
  Start(d, s, 11);
  Check(d.Advance(s, 2) == Direction::None, "initial stable window");
  Check(d.Advance(s, 3) == Direction::Right, "one right pulse");
}
constexpr uint64_t StepSpan = SelectorDrive::PulseTicks + SelectorDrive::NeutralTicks + SelectorDrive::SettleTicks;
void CompletionAndTiming() {
  SelectorDrive d;auto s=Held();FirstRight(d,s);
  Check(d.Advance(s,3)==Direction::None,"same tick cannot retrigger pulse");
  Check(!d.Request(12,s,3),"pending request cannot replace active pulse");
  Cursor(s,0,1);Check(d.Advance(s,4)==Direction::None,"target alone is not success");
  Cursor(s,1,1);
  for(uint64_t t=5;t<3+StepSpan;++t){
    Check(d.Advance(s,t)==Direction::None,"pulse then neutral then settle");
    Check(d.status()==DriveStatus::Pending,"early arrival remains pending");
  }
  Check(d.Advance(s,3+StepSpan)==Direction::None,"completion emits no confirm");
  Check(d.status()==DriveStatus::Complete,"stable actual target completes");
  Check(s.selector.equipped_native_id==10,"native equipment untouched");
  Check(d.Advance(s,4+StepSpan)==Direction::None,"complete stays silent");
}
void MultipleStepsAndWrap() {
  SelectorDrive d;auto s=Held();Start(d,s,12);
  Check(d.Advance(s,2)==Direction::None,"initial settle");
  Check(d.Advance(s,3)==Direction::Right,"shorter right route");Cursor(s,1,1);
  for(uint64_t t=4;t<3+StepSpan;++t)Check(d.Advance(s,t)==Direction::None,"full neutral and settle before next pulse");
  Check(d.Advance(s,3+StepSpan)==Direction::Right,"second pulse after observed step");Cursor(s,2,2);
  for(uint64_t t=4+StepSpan;t<=3+2*StepSpan;++t)Check(d.Advance(s,t)==Direction::None,"second step never extra pulse");
  Check(d.status()==DriveStatus::Complete,"multistep complete");
  for(const bool reverse:{false,true}){
    SelectorDrive wrap;auto v=Held(127);if(reverse)Cursor(v,126,126);
    Start(wrap,v,reverse?10:136);Check(wrap.Advance(v,2)==Direction::None,"wrap settle");
    Check(wrap.Advance(v,3)==(reverse?Direction::Right:Direction::Left),"shortest one-step wrap");
    Cursor(v,reverse?0:126,reverse?0:126);
    for(uint64_t t=4;t<=3+StepSpan;++t)wrap.Advance(v,t);
    Check(wrap.status()==DriveStatus::Complete,"wrap observed complete");
  }
  SelectorDrive tie;auto even=Held(4);Start(tie,even,12);tie.Advance(even,2);
  Check(tie.Advance(even,3)==Direction::Right,"equal distance deterministic right");
}
// Simulates a slow guest: input is not sampled until after the old two-tick
// pulse ended, and native cursor animation can acknowledge after30host ticks.
void DelayedGuestAcknowledgement() {
  for(const uint64_t arrival:{uint64_t(8),uint64_t(35),uint64_t(70)}){
    SelectorDrive d;auto s=Held();FirstRight(d,s);
    for(uint64_t t=4;t<3+arrival+SelectorDrive::SettleTicks;++t){
      if(t==8)Cursor(s,0,1);
      if(t==3+arrival)Cursor(s,1,1);
      Check(d.Advance(s,t)==Direction::None,"delayed guest never duplicate pulse");
      Check(d.status()==DriveStatus::Pending,"delayed ACK remains pending");
    }
    const uint64_t end=std::max(3+StepSpan,3+arrival+SelectorDrive::SettleTicks);
    // For the early-arrival case finish the pulse+neutral window as well.
    for(uint64_t t=3+arrival+SelectorDrive::SettleTicks;t<=end;++t)d.Advance(s,t);
    Check(d.status()==DriveStatus::Complete,"late native ACK completes before bounded step timeout");
  }
}
void LifecycleAndClock() {
  auto s = Held();
  SelectorDrive unseen;
  Check(!unseen.Request(11, s, 0), "first hold cannot act without stable bootstrap");
  unseen.Advance(s, 1);
  Check(!unseen.Request(11, s, 1), "one interval cannot bootstrap first hold");
  Check(unseen.Advance(s, 2) == Direction::None &&
        unseen.status() != DriveStatus::Pending,
        "initial bootstrap retains no rejected intention");
  Check(unseen.Request(11, s, 2), "stable first hold accepts a fresh intention");
  SelectorDrive d;
  FirstRight(d, s);
  d.Advance(Closed(), 4);
  Check(d.status() == DriveStatus::Aborted && d.abort_reason() == DriveAbort::Released,
        "early physical release aborts intention");
  Check(d.Advance(s, 5) == Direction::None, "new hold reuses no pending target");
  Check(d.status() != DriveStatus::Pending, "same owner address is a new generation");
  Check(d.Request(12, s, 5), "new hold needs new explicit intention");
  d.Cancel();
  Check(d.status() == DriveStatus::Aborted, "controller cancel aborts");
  Check(!d.Request(11, s, 5), "cancel disarms current hold");
  d.Advance(Closed(), 6);
  Check(d.Request(11, s, 7), "closed lifecycle rearms after cancellation");
  for (const int mode : {0, 1, 2}) {
    SelectorDrive clock;
    FirstRight(clock, s);
    const uint64_t tick = mode == 0 ? 2 : mode == 1 ? 5 :
        std::numeric_limits<uint64_t>::max();
    Check(clock.Advance(s, tick) == Direction::None, "bad clock emits nothing");
    Check(clock.status() == DriveStatus::Aborted, "rollback/gap/overflow aborts");
    Check(!clock.Request(11, s, tick), "bad clock cannot restart same hold");
  }
  SelectorDrive near_end;
  constexpr auto max = std::numeric_limits<uint64_t>::max();
  near_end.Advance(Closed(), max - 2);
  Check(!near_end.Request(11, s, max - 1), "insufficient tick range refuses intention");
  near_end.Advance(s, 0);
  Check(near_end.status() == DriveStatus::Aborted, "wrapped tick aborts");
}
void Invalidations() {
  using Mutation = void (*)(LiveSnapshot&);
  const Mutation mutations[] = {
      [](LiveSnapshot& s) { s.compatible = false; },
      [](LiveSnapshot& s) { s.collection_valid = false; },
      [](LiveSnapshot& s) { ++s.collection_source.revision; },
      [](LiveSnapshot& s) { s.collection_source.save_data += 8; },
      [](LiveSnapshot& s) { s.collection_source.access_object += 8; },
      [](LiveSnapshot& s) { s.collection_source.list += 8; },
      [](LiveSnapshot& s) { s.selector.source.owner += 8; },
      [](LiveSnapshot& s) { s.selector.source.records += 8; s.selector.source.end += 8;
                              s.selector.source.capacity += 8; },
      [](LiveSnapshot& s) { s.selector.source.capacity += 0x38; },
      [](LiveSnapshot& s) { std::swap(s.selector.ordered_native_ids[1],
                                     s.selector.ordered_native_ids[2]); },
      [](LiveSnapshot& s) { s.selector.ordered_native_ids.pop_back();
                              s.selector.source.end -= 0x38; },
      [](LiveSnapshot& s) { s.learned_native_ids.push_back(20); },
      [](LiveSnapshot& s) { s.selector.source.mode = 0; },
      [](LiveSnapshot& s) { s.selector.source.native_state = 1; },
      [](LiveSnapshot& s) { s.selector.source.native_state = 3; },
      [](LiveSnapshot& s) { s.selector.selector_valid = false; },
      [](LiveSnapshot& s) { s.selector.equipped_valid = false; },
      [](LiveSnapshot& s) { s.selector.equipped_native_id = 11; },
      [](LiveSnapshot& s) { s.selector.target_index = 5; },
      [](LiveSnapshot& s) { s.selector.highlighted_index = -1; },
      [](LiveSnapshot& s) { s.selector.highlighted_native_id = 99; },
      [](LiveSnapshot& s) { s.selector.ordered_native_ids[1] = 10; },
      [](LiveSnapshot& s) { s.learned_native_ids[1] = 99; },
      [](LiveSnapshot& s) { s.selector.source.end = 0; },
      [](LiveSnapshot& s) { s.selector.source.owner = 0; },
  };
  for (const auto mutate : mutations) {
    SelectorDrive d;
    auto s = Held();
    FirstRight(d, s);
    mutate(s);
    Check(d.Advance(s, 4) == Direction::None, "context/identity corruption never pulses");
    Check(d.status() == DriveStatus::Aborted, "context/identity corruption aborts");
    Check(!d.Request(11, s, 4), "invalidation cannot retain hold authorization");
  }
}
void RefusalAndInterruption() {
  auto s = Held();
  SelectorDrive d;
  d.Advance(Closed(), 0);
  Check(!d.Request(1200, s, 1), "unlearned/unlisted target refused");
  Cursor(s, 0, 1);
  Check(!d.Request(11, s, 1), "request during native animation refused");
  Cursor(s, 0, 0);
  Check(d.Request(11, s, 1), "valid request after refusal remains possible");
  d.Advance(s, 2);
  d.Advance(s, 3);
  Cursor(s, 2, 2);
  Check(d.Advance(s, 4) == Direction::None, "unexpected controller jump no pulse");
  Check(d.abort_reason() == DriveAbort::UnexpectedCursor, "unexpected cursor aborts");
  SelectorDrive target_only;
  s = Held();
  FirstRight(target_only, s);
  Cursor(s, 0, 2);
  target_only.Advance(s, 4);
  Check(target_only.abort_reason() == DriveAbort::UnexpectedCursor,
        "unexpected animation destination aborts before actual moves");
  SelectorDrive before;
  s = Held();
  Start(before, s, 11);
  Cursor(s, 1, 1);
  before.Advance(s, 2);
  Check(before.abort_reason() == DriveAbort::UnexpectedCursor,
        "controller movement during initial settle aborts");
}
void FirstHoldBootstrap() {
  auto s = Held();
  SelectorDrive d;
  d.Advance({}, 0); // Initial title/loading observations need not expose mode1.
  Check(!d.Request(11, s, 1), "initial unknown context does not bypass bootstrap");
  d.Advance(s, 2);
  Cursor(s, 1, 1);
  d.Advance(s, 3);
  Check(!d.Request(12, s, 4), "changed cursor restarts stable bootstrap window");
  Check(d.Advance(s, 5) == Direction::None, "bootstrap never carries rejected intention");
  Check(d.Request(12, s, 5), "two stable intervals admit first verified held context");
  d.Advance({}, 6);
  for (uint64_t tick = 7; tick <= 10; ++tick)
    Check(d.Advance(s, tick) == Direction::None, "bootstrap cannot repeat after accepted hold");
  Check(!d.Request(12, s, 10), "invalidated first hold needs closed lifecycle");
  d.Advance(Closed(), 11);
  Check(d.Request(12, s, 12), "known close rearms after bootstrap invalidation");
  SelectorDrive gap;
  gap.Advance(s, 0);
  gap.Advance(s, 2);
  Check(!gap.Request(11, s, 3), "missing bootstrap tick refuses uncertain generation");
}

// Mirrors ReadSelector's unavailable opening shape: compatibility and equipped
// ownership survive, but no native selector state/source is exposed.
LiveSnapshot OpeningInvalid() {
  auto s = Held();
  const auto equipped = s.selector.equipped_native_id;
  s.selector = {};
  s.selector.compatible = s.selector.equipped_valid = true;
  s.selector.equipped_native_id = equipped;
  return s;
}
void RepeatedOpeningBootstrap() {
  const auto held = Held();
  SelectorDrive d;
  Start(d, held, 10);
  d.Advance(held, 2);
  d.Advance(held, 3);
  Check(d.status() == DriveStatus::Complete, "initial hold completes before repeat");
  uint64_t tick = 4;
  for (int cycle = 0; cycle < 3; ++cycle) {
    if (cycle == 1) d.Cancel();
    d.Advance(Closed(), tick++);
    for (int n = 0; n < 3; ++n) {
      Check(d.Advance(OpeningInvalid(), tick++) == Direction::None,
            "consecutive opening-invalid frames emit nothing");
      Check(d.status() == DriveStatus::Aborted &&
            d.abort_reason() == DriveAbort::InvalidSnapshot,
            "opening invalid still aborts all intentions");
    }
    Check(!d.Request(12, held, tick), "reopened selector needs stable bootstrap");
    Check(!d.Request(12, held, tick), "same-tick reads cannot bootstrap reopening");
    ++tick;
    Check(d.Advance(held, tick) == Direction::None,
          "one stable reopened interval emits nothing");
    Check(!d.Request(12, held, tick++), "one stable reopened interval cannot accept tap");
    Check(d.Advance(held, tick) == Direction::None,
          "two stable reopened intervals carry no rejected target");
    Check(d.status() != DriveStatus::Pending, "recovery never resurrects an intention");
    Check(d.Request(10, held, tick++), "fresh tap accepted after stable reopening");
    Check(d.Advance(held, tick++) == Direction::None, "fresh current target settles");
    Check(d.Advance(held, tick++) == Direction::None && d.status() == DriveStatus::Complete,
          "only new current-target intention completes without old movement");
  }
  {
    SelectorDrive intermittent;
    intermittent.Advance(Closed(), 0);
    intermittent.Advance(OpeningInvalid(), 1);
    intermittent.Advance(held, 2);
    intermittent.Advance(OpeningInvalid(), 3);
    intermittent.Advance(OpeningInvalid(), 4);
    Check(!intermittent.Request(12, held, 5), "interrupted opening restarts stable window");
    intermittent.Advance(held, 6);
    Check(!intermittent.Request(12, held, 6), "interrupted opening still needs two intervals");
    intermittent.Advance(held, 7);
    Check(intermittent.Request(10, held, 7), "unchanged opening anchor survives repeated invalids");
  }
  // A pending step or a hold already admitted before any tap must still disarm.
  for (const bool pending : {false, true}) {
    SelectorDrive active;
    active.Advance(Closed(), 0);
    active.Advance(held, 1);
    if (pending) Check(active.Request(11, held, 1), "pending invalidation setup");
    active.Advance(OpeningInvalid(), 2);
    for (uint64_t t = 3; t <= 7; ++t)
      Check(active.Advance(held, t) == Direction::None, "active-hold invalidation stays silent");
    Check(!active.Request(11, held, 7), "active hold invalidation cannot bootstrap again");
  }
  using Mutation = void (*)(LiveSnapshot&);
  const Mutation changes[] = {
      [](LiveSnapshot& s) { s.compatible = false; },
      [](LiveSnapshot& s) { s.collection_valid = false; },
      [](LiveSnapshot& s) { ++s.collection_source.revision; },
      [](LiveSnapshot& s) { s.collection_source.save_data += 8; },
      [](LiveSnapshot& s) { s.collection_source.list += 8; },
      [](LiveSnapshot& s) { s.collection_source.access_object += 8; },
      [](LiveSnapshot& s) { s.learned_native_ids.push_back(20); },
      [](LiveSnapshot& s) { s.learned_crc_keys.push_back(1); },
      [](LiveSnapshot& s) { s.selector.compatible = false; },
      [](LiveSnapshot& s) { s.selector.equipped_valid = false; },
      [](LiveSnapshot& s) { ++s.selector.equipped_native_id; },
  };
  for (const auto change : changes) {
    SelectorDrive lost;
    lost.Advance(Closed(), 0);
    lost.Advance(OpeningInvalid(), 1);
    auto bad = OpeningInvalid(); change(bad);
    lost.Advance(bad, 2);
    lost.Advance(OpeningInvalid(), 3);
    for (uint64_t t = 4; t <= 8; ++t) lost.Advance(held, t);
    Check(!lost.Request(11, held, 8), "lost opening source cannot regain candidate");
    lost.Advance(Closed(), 9);
    Check(lost.Request(11, held, 10), "fresh close rearms after lost opening source");
  }
  for (const bool cancel : {false, true}) {
    SelectorDrive stopped;
    stopped.Advance(Closed(), 0);
    stopped.Advance(OpeningInvalid(), 1);
    if (cancel) stopped.Cancel();
    const uint64_t start = cancel ? 2 : 3;
    for (uint64_t t = start; t < start + 5; ++t) stopped.Advance(held, t);
    Check(!stopped.Request(11, held, start + 4),
          "cancel or clock gap disarms reopening candidate");
  }
  // A real selector context change during recovery must not restart bootstrap.
  for (const bool during_bootstrap : {false, true}) {
    SelectorDrive changed;
    changed.Advance(Closed(), 0);
    changed.Advance(OpeningInvalid(), 1);
    auto bad = held;
    if (during_bootstrap) {
      changed.Advance(held, 2);
      bad.selector.source.capacity += 0x38;
      changed.Advance(bad, 3);
    } else {
      bad.selector.source.owner += 8;
      changed.Advance(bad, 2);
      changed.Advance(held, 3);
    }
    for (uint64_t t = 4; t <= 8; ++t) changed.Advance(held, t);
    Check(!changed.Request(11, held, 8), "changed selector recovery stays disarmed");
  }
}


void DirectSortTargets() {
  constexpr uint64_t owner=0x4000,list=owner+0x150,index=owner+0x2dec;
  struct Memory {
    std::map<uint64_t,uint8_t> bytes;
    size_t writes{};uint8_t written{};bool fail_write{};size_t index_reads{};bool tear{};
    void Put(uint64_t at,uint64_t value,size_t n=8) {
      for(size_t i=0;i<n;++i)bytes[at+i]=uint8_t(value>>(i*8));
    }
    Memory() {
      Put(list+0x98,1,1);Put(list+0x99,2,1);Put(list+0x9a,0xfe,1);
      Put(list+0x2c78,0);Put(index,0,1);
      // Two native blocks, including a populated range crossing the boundary.
      Put(owner+0x2df8,0x8000);Put(owner+0x2e00,0x8010);
      Put(owner+0x2e08,0);Put(owner+0x2e10,4094);Put(owner+0x2e18,5);
      Put(0x8000,0x9000);Put(0x8008,0xa000);
      const uint8_t values[]{1,2,4,8,16};
      for(size_t i=0;i<5;++i)Put(i<2?0x9000+4094+i:0xa000+i-2,values[i],1);
    }
    bool Read(uint64_t at,void* p,size_t n) {
      if(at==index && ++index_reads==2 && tear)bytes[0x6dec]=1;
      auto* b=static_cast<uint8_t*>(p);
      for(size_t i=0;i<n;++i){auto it=bytes.find(at+i);if(it==bytes.end())return false;b[i]=it->second;}
      return true;
    }
    bool Write(uint64_t at,const void* p,size_t n) {
      ++writes;Check(at==index && n==1,"sort only writes native index byte");
      written=*static_cast<const uint8_t*>(p);return !fail_write;
    }
  };
  auto held=Held();held.selector.sort_valid=held.selector.source.sort_valid=true;
  held.selector.sort_enum=held.selector.source.sort_enum=1;
  for(uint8_t current=0;current<5;++current)for(uint8_t target=0;target<5;++target) {
    auto value=held;value.selector.sort_enum=value.selector.source.sort_enum=uint8_t(1<<current);
    Memory mem;mem.Put(index,current,1);
    const auto result=PrepareDirectSort(value,uint8_t(1<<target),
      [&](auto at,auto p,auto n){return mem.Read(at,p,n);},
      [&](auto at,auto p,auto n){return mem.Write(at,p,n);});
    Check(result==(current==target?DirectSortResult::Current:DirectSortResult::Prepared),
          "all five direct target pairs accepted");
    const uint8_t predecessor=uint8_t((target+4)%5);
    Check(mem.writes==size_t(current!=target && current!=predecessor),
          "current and already adjacent targets require no index write");
    const uint8_t prepared=mem.writes?mem.written:current;
    Check(current==target || (prepared+1)%5==target,"exactly one native increment reaches target");
  }
  for(int fault=0;fault<22;++fault) {
    auto value=held;Memory mem;
    if(fault==0)value.compatible=false;
    if(fault==1)value.collection_valid=false;
    if(fault==2)value.selector.held=false;
    if(fault==3)value.selector.source.owner=UINT64_MAX-7;
    if(fault==4)value.selector.target_index=1;
    if(fault==5)value.selector.source.sort_valid=false;
    if(fault==6)mem.Put(list+0x98,2,1);
    if(fault==7)mem.Put(list+0x9a,2,1);
    if(fault==8)mem.Put(list+0x2c78,1);
    if(fault==9)mem.Put(index,5,1);
    if(fault==10)mem.Put(owner+0x2e18,6);
    if(fault==11)mem.Put(owner+0x2e18,0);
    if(fault==12)mem.Put(owner+0x2e10,UINT64_MAX);
    if(fault==13)mem.Put(owner+0x2df8,0x8001);
    if(fault==14)mem.Put(owner+0x2e00,0x8048);
    if(fault==15)mem.Put(0x8008,0);
    if(fault==16)mem.Put(0xa000,32,1);
    if(fault==17)mem.Put(0xa000,1,1);
    if(fault==18)mem.tear=true;
    if(fault==19)mem.bytes.erase(0xa001);
    if(fault==20)value.selector.sort_enum=value.selector.source.sort_enum=2;
    if(fault==21)value.selector.source.end+=8;
    const auto result=PrepareDirectSort(value,16,
      [&](auto at,auto p,auto n){return mem.Read(at,p,n);},
      [&](auto at,auto p,auto n){return mem.Write(at,p,n);});
    Check(result==DirectSortResult::Refused && mem.writes==0,"invalid/torn direct sort never writes");
  }
  for(uint8_t target:{uint8_t(0),uint8_t(3),uint8_t(32)}) {
    Memory mem;
    Check(PrepareDirectSort(held,target,[&](auto a,auto p,auto n){return mem.Read(a,p,n);},
      [&](auto a,auto p,auto n){return mem.Write(a,p,n);})==DirectSortResult::Refused && !mem.writes,
      "missing/unknown native sort target refused");
  }
  Memory failed;failed.fail_write=true;
  Check(PrepareDirectSort(held,16,[&](auto a,auto p,auto n){return failed.Read(a,p,n);},
    [&](auto a,auto p,auto n){return failed.Write(a,p,n);})==DirectSortResult::Refused,
    "failed predecessor write never authorizes native Y");
}

void RegressionAndTotalTimeout() {
  for (const bool arrived : {false, true}) {
    SelectorDrive d;
    auto s = Held();
    FirstRight(d, s);
    Cursor(s, arrived ? 1 : 0, 1);
    d.Advance(s, 4);
    Cursor(s, 0, arrived ? 1 : 0);
    Check(d.Advance(s, 5) == Direction::None, "controller reversal never adds a pulse");
    Check(d.abort_reason() == DriveAbort::UnexpectedCursor,
          "observed actual/destination progress cannot regress");
  }
  SelectorDrive d;
  auto s = Held(512);
  Start(d, s, 266);
  uint64_t sent{};
  int32_t next{};
  size_t pulses{};
  for (uint64_t tick = 2; tick <= 2049; ++tick) {
    if (sent && tick - sent >= 20) Cursor(s, next, next);
    const auto direction = d.Advance(s, tick);
    if (direction != Direction::None) {
      Check(direction == Direction::Right, "long route remains shortest direction");
      ++pulses;
      sent = tick;
      next = (s.selector.highlighted_index + 1) % 512;
      Cursor(s, s.selector.highlighted_index, next);
    }
  }
  Check(pulses > 1, "total timeout scenario includes genuine observed progress");
  Check(d.status() == DriveStatus::Aborted && d.abort_reason() == DriveAbort::Timeout,
        "slow continuous progress still has bounded total request lifetime");
  SelectorDrive unavailable;
  s = Held();
  FirstRight(unavailable, s);
  Check(unavailable.Advance({}, 4) == Direction::None, "unavailable frame cancels pulse plan");
  unavailable.Advance(s, 5);
  Check(!unavailable.Request(11, s, 5), "unavailable gap cannot hide another same-address hold");
}

void NoProgressAndCurrentTarget() {
  SelectorDrive dropped;
  auto s = Held();
  FirstRight(dropped, s);
  for (uint64_t t = 4; t <= 3 + SelectorDrive::StepTimeoutTicks; ++t)
    Check(dropped.Advance(s, t) == Direction::None, "dropped pulse never retried blindly");
  Check(dropped.abort_reason() == DriveAbort::Timeout, "no progress times out");
  SelectorDrive animation;
  FirstRight(animation, s);
  Cursor(s, 0, 1);
  for (uint64_t t = 4; t <= 3 + SelectorDrive::StepTimeoutTicks; ++t)
    Check(animation.Advance(s, t) == Direction::None, "target-only animation never completes");
  Check(animation.abort_reason() == DriveAbort::Timeout, "stalled actual cursor times out");
  SelectorDrive current;
  s = Held(1);
  Start(current, s, 10);
  Check(current.Advance(s, 2) == Direction::None &&
        current.status() == DriveStatus::Pending, "current target waits stable observation");
  Check(current.Advance(s, 3) == Direction::None &&
        current.status() == DriveStatus::Complete, "current target completes without pulse");
}
} // namespace
int main() {
  DirectSortTargets();
  {
    size_t writes=0;
    ReadFn read=[](uint64_t at,void* p,size_t n) {
      auto* b=static_cast<uint8_t*>(p);
      if(at==0x4000+0x150+0x98 && n==3){b[0]=1;b[1]=2;b[2]=0xfe;return true;}
      if(at==0x4000+0x150+0x2c98 && n==4){std::fill_n(b,4,0);return true;}return false;
    };
    auto write=[&](uint64_t at,const void* p,size_t n) {
      ++writes;const auto* b=static_cast<const uint8_t*>(p);
      if(writes%3==1)Check(at==0x4000+0x2dc8 && n==8 && b[0]==126 && b[4]==126 && b[1]==0 && b[5]==0,"aligned far cursor pair");
      if(writes%3==2)Check(at==0x4000+0x2de8 && n==4 && b[0]==2,"native same-index repaint");
      if(writes%3==0)Check(at==0x4000+0x1ea && n==1 && b[0]==2,"queue native Change state");
      return true;
    };
    auto s=Held(127);Check(DirectSelect(s,136,read,write) && writes==3,"far direct selection and native refresh once");
    Check(DirectSelect(s,10,read,write) && writes==3,"current is no-op");
    for(int fault=0;fault<11;++fault) {
      auto bad=s;
      if(fault==0)bad.selector.held=false;
      if(fault==1)bad.selector.source.mode=0;
      if(fault==2)bad.selector.source.native_state=3;
      if(fault==3)bad.selector.target_index=1;
      if(fault==4)bad.selector.source.owner=UINT64_MAX-7;
      if(fault==5)bad.selector.source.end+=8;
      if(fault==6)bad.collection_valid=false;
      if(fault==7)bad.learned_native_ids.pop_back();
      if(fault==8)bad.selector.selector_valid=false;
      if(fault==9)bad.selector.highlighted_index=-1;
      if(fault==10)bad.selector.highlighted_native_id=11;
      Check(!DirectSelect(bad,136,read,write) && writes==3,"invalid direct request never writes");
    }
    Check(!DirectSelect(s,999,read,write) && writes==3,"missing target never writes");
    Check(!DirectSelect(s,136,{},write) && writes==3,"missing native lifecycle never writes");
    Check(!DirectSelect(s,136,read,[](auto,auto,auto){return false;}),"write failure reported");
  }
  CompletionAndTiming();
  DelayedGuestAcknowledgement();
  MultipleStepsAndWrap();
  LifecycleAndClock();
  Invalidations();
  RefusalAndInterruption();
  NoProgressAndCurrentTarget();
  RegressionAndTotalTimeout();
  FirstHoldBootstrap();
  RepeatedOpeningBootstrap();
  std::cout << "echoes drive: " << checks << " checks passed\n";
}
