// SPDX-License-Identifier: GPL-3.0-or-later
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main inventory_bridge_fixture_main
#include "botw_inventory_bridge_test.cpp"
#undef main
#pragma GCC diagnostic pop
#include "botw_inventory_update_fixture.h"

static void update_food(InventoryBridgeFixture& f) {
  f.food();update_fixture(f);
  const auto original=f.mem;
  for(auto [offset,size]:std::array<std::pair<uint64_t,unsigned>,4>{{{0x3610,8},{0x3630,8},{0x3638,4},{0x3A08,8}}}) {
    const auto at=f.FoodScreen+offset;
    for(unsigned i=0;i<size;++i){f.mem[at+24+i]=original.at(at+i);f.mem.erase(at+i);}
  }
  f.state.data=botw::pouch(f.h);assert(f.state.data.valid);
  f.state.bridge.poll(f.h,{});
}
int main() {
  using F=InventoryBridgeFixture;
  {F f;update_food(f);assert(supports(botw::InventoryBuild190));
    assert(botw::supported(f.h)&&botw::inventory_supported(f.h));
    const auto request=botw::inventory_consume(f.h,F::Two,f.state.data);
    assert(request.valid&&request.screen==F::FoodScreen&&request.column==F::Column);
    assert(on_action(&f.state,"inventory_consume",0));
    assert(f.get<uint64_t>(368)==F::FoodScreen&&f.get<uint64_t>(376)==F::Column);
    // An ack/stock change alone does not claim a completed player-thread food job.
    f.put<int32_t>(F::Two+0x20,4);f.mb<uint32_t>(12,f.state.bridge.pending);f.mb<uint32_t>(16,1);
    f.mb<uint32_t>(416,1);f.state.bridge.poll(f.h,{});
    assert(f.state.bridge.pending&&std::string(f.state.bridge.status)=="Eating");
    f.mb<uint32_t>(416,0);f.mb<uint32_t>(420,1);f.state.bridge.poll(f.h,{});
    assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)=="Consumed");
    assert(f.state.bridge.quest_supported&&!f.state.bridge.map_supported&&!f.state.bridge.warp_supported); // DUOC predates Map/Travel
  }
  for(unsigned fault=0;fault<4;++fault){F f;update_food(f);
    if(fault==0)f.put(F::FoodScreen,F::M+0x2BA7240);
    if(fault==1)f.put<uint64_t>(F::FoodScreen+0x3648,F::Two);
    if(fault==2)f.put<uint32_t>(F::FoodScreen+0x3650,29);
    if(fault==3)f.put<uint64_t>(F::FoodScreen+0x3A20,0);
    assert(!botw::inventory_consume(f.h,F::Two,f.state.data).valid);
  }
  std::cout<<"1.9.0 pause fields, original type refusal and player-job completion checks passed\n";
}
