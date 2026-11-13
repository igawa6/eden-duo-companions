// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_190_INVENTORY_EVIDENCE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define BOTW_SORT_FIXTURE_ONLY
#include "botw_sort_test.cpp"
#undef BOTW_SORT_FIXTURE_ONLY
#pragma GCC diagnostic pop
#include "botw_inventory_update_fixture.h"
int main() {
  using F=SortFixture;
  for(int category=0;category<7;++category){F f;update_fixture(f);
    botw::SelectionBridge bridge;bridge.configure(&f.extensions);bridge.poll(f.h,{});
    assert(bridge.sort_mode(category)==0);assert(bridge.request_sort(category));
    f.put<int32_t>(F::Manager+0x44800,category);
    if(category<4){f.mode(true);for(unsigned i=0;i<4;++i)f.put(F::F0+i*16,F::M+0x1D21A20);}
    f.mail<uint32_t>(16,1);f.mail<uint32_t>(12,bridge.pending);bridge.poll(f.h,{});
    assert(!bridge.pending&&std::string(bridge.status)=="Sorted");
  }
  {F f;update_fixture(f);botw::TrackerFlagReader flags;
    assert(!flags.begin(f.h)); // general trackers remain refused
    assert(flags.begin(f.h,true));
    f.put<uint64_t>(F::F0,0);assert(!flags.observe_boolean(botw::warp::crc("SortTypeWeaponPouch")));
  }
  std::cout<<"1.9.0 seven-category Sort confirmation and general-tracker refusal passed\n";
}
