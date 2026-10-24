// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_quick_slots.h"
#include <cstdlib>
#include <iostream>
using namespace echoes;
int main() {
  int checks=0;
  const auto check=[&](bool ok) { ++checks; if(!ok) {std::cerr<<"check "<<checks<<" failed\n";std::exit(1);} };
  QuickSlots q; LiveSnapshot v;
  check(!q.Target(0)); q.Sync(v); check(!q.ready());
  v.compatible=v.collection_valid=true; v.collection_source={1,2,3,0};
  v.learned_native_ids={9,4,2,4,0xffff};v.selector.equipped_valid=true;v.selector.equipped_native_id=9;
  q.Sync(v);check(q.slots()==std::array<uint16_t,3>{9,2,4});check(q.active()==0);
  check(!q.Assign(3,2));check(!q.Assign(0,8));check(q.Assign(2,9));
  check(q.slots()==std::array<uint16_t,3>{4,2,9});check(q.active()==2);
  v.selector.equipped_native_id=2;q.Sync(v);check(q.active()==1);
  v.learned_native_ids.push_back(8);q.Sync(v);check(q.Assign(1,8));check(!q.active());
  check(q.Target(1)==8);q.Sync(v);check(q.Target(0)==4);check(!q.active());
  v.collection_valid=false;q.Sync(v);check(!q.ready());check(!q.active());check(!q.Target(0));check(!q.Assign(0,9));
  v.collection_valid=true;v.collection_source.revision++;v.collection_source.list=88;q.Sync(v);check(q.Target(1)==8);
  v.learned_native_ids={2};q.Sync(v);check(q.slots()==std::array<uint16_t,3>{2,0xffff,0xffff});check(!q.Target(1));
  v.collection_source.save_data=7;v.learned_native_ids={7,3,5};v.selector.equipped_native_id=5;q.Sync(v);
  check(q.slots()==std::array<uint16_t,3>{5,3,7});check(q.active()==0);
  v.selector.equipped_valid=false;q.Sync(v);check(!q.active());check(q.Target(0)==5);
  v.learned_native_ids.clear();q.Sync(v);check(q.ready());check(!q.Target(0));
  v.collection_source.save_data=8;v.learned_native_ids={6,1,3};v.selector.equipped_valid=true;v.selector.equipped_native_id=1;q.Sync(v);
  check(q.slots()==std::array<uint16_t,3>{1,3,6});
  check(q.Assign(0,1));check(q.slots()==std::array<uint16_t,3>{1,3,6});
  v.learned_native_ids={6,1};q.Sync(v);check(q.slots()==std::array<uint16_t,3>{1,0xffff,6});
  v.learned_native_ids={6,1,8};q.Sync(v);check(q.slots()==std::array<uint16_t,3>{1,8,6});
  v.compatible=false;q.Sync(v);check(!q.Target(2));
  QuickSlots boot;LiveSnapshot initial;
  initial.compatible=initial.collection_valid=true;initial.collection_source={10,20,30,1};
  initial.learned_native_ids={0,1,2,1128};
  boot.Sync(initial);check(!boot.ready());check(!boot.Target(0));check(!boot.Assign(0,2));
  check(boot.slots()==std::array<uint16_t,3>{65535,65535,65535});
  initial.selector.equipped_valid=true;initial.selector.equipped_native_id=9999;
  boot.Sync(initial);check(!boot.ready());
  initial.selector.equipped_native_id=1128;boot.Sync(initial);
  check(boot.ready());check(boot.slots()==std::array<uint16_t,3>{1128,0,1});check(boot.active()==0);
  check(boot.Assign(0,2));boot.Sync(initial);check(boot.Target(0)==2);check(!boot.active());
  initial.selector.equipped_valid=false;boot.Sync(initial);check(boot.ready());check(boot.Target(0)==2);
  initial.collection_source.save_data=21;boot.Sync(initial);check(!boot.ready());
  check(boot.slots()==std::array<uint16_t,3>{65535,65535,65535});
  initial.selector.equipped_valid=true;initial.selector.equipped_native_id=1;boot.Sync(initial);
  check(boot.ready());check(boot.Target(0)==1);check(boot.active()==0);
  std::cout<<checks<<" quick-slot checks passed\n";
}
