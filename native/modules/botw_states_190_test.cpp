// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_190_INVENTORY_EVIDENCE
#define BOTW_190_READY_EVIDENCE
#define BOTW_190_QUEST_EVIDENCE
#define BOTW_190_RUNES_EVIDENCE
#define BOTW_190_MAP_EVIDENCE
#define BOTW_STATES_FIXTURE_ONLY
#include "botw_states_test.cpp"
#include "botw_inventory_update_fixture.h"
namespace {
struct State190Fixture:LiveFixture {
  explicit State190Fixture(const char* id):LiveFixture(scen(id)) {
    update_fixture(*this);const auto original=mem;
    for(auto [old,next]:{std::pair{botw::States160.map_type,botw::States190.map_type},std::pair{botw::States160.map_name,botw::States190.map_name}}){
      for(unsigned j=0;j<56;++j)if(auto it=original.find(M+old+j);it!=original.end())put<uint8_t>(M+next+j,it->second);
      put(M+next,M+botw::States190.string_vtable);put(M+next+8,M+next+20);
      for(unsigned j=0;j<56;++j)mem.erase(M+old+j);
    }
    const auto& scenario=scen(id);
    for(unsigned k=0;k<scenario.slot_count;++k){const auto& sl=scenario.slots[k];uint64_t v=sl.state_ptr;
      if(v==botw::states::PauseInventory)v=botw::States190.pause_inventory;
      if(v==botw::states::PauseSystem)v=botw::States190.pause_system;
      if(v==botw::states::PauseLog)v=botw::States190.pause_log;
      put<uint64_t>(sl.ptr+0x168,v?M+v:0);
      for(unsigned j=0;j<8;++j)mem.erase(sl.ptr+0x150+j);
    }
  }
};
}
int main() {
  {State190Fixture f("a-world-1");auto g=botw::game_states(f.h);assert(g.valid&&!g.shrine&&g.map_type=="MainField");botw::publish_states(f.h,g);assert(f.values["botw.state.shrine.active"]==0);}
  for(auto [id,name]:{std::pair{"a-menu-plus","inventory"},std::pair{"a-menu-system","system"},std::pair{"a-menu-advlog","adventure_log"},std::pair{"a-menu-map","map"}}){State190Fixture f(id);auto g=botw::game_states(f.h);assert(g.valid&&g.menu_screen==name);}
  {State190Fixture f("a-shrine-in");auto g=botw::game_states(f.h);assert(g.valid&&g.shrine&&g.shrine_number==53);}
  {State190Fixture f("a-world-1");f.put<uint64_t>(f.M+botw::States190.map_name,f.M+botw::States160.string_vtable);assert(!botw::game_states(f.h).valid);}
  {State190Fixture f("a-world-1");f.h.build_id[0]^=1;assert(!botw::game_states(f.h).valid);}
  std::cout<<"1.9 scene strings, screen states, native pause tabs and shrine overlay contract passed\n";
}
