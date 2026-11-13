// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_190_INVENTORY_EVIDENCE
#define BOTW_190_READY_EVIDENCE
#define BOTW_190_QUEST_EVIDENCE
#define BOTW_190_RUNES_EVIDENCE
#define BOTW_WORLD_RUNE_FIXTURE_ONLY
#include "botw_world_rune_bridge_test.cpp"
#include "botw_inventory_update_fixture.h"
namespace {
struct Shortcut190Fixture:WorldRuneBridgeFixture {
  static constexpr uint64_t Buffer=0xB000000;
  Shortcut190Fixture() {
    update_fixture(*this);
    const std::pair<uint64_t,uint64_t> ids[]={
      {0x2BA1D40,0x1CF51D8},{0x2BA1B50,0x1E2D538},{0x2BA1C10,0x1E2D598},
      {0x2BA1C70,0x1E2D5F8},{0x2B858B0,0x1CDB958},
      {0x2C9A668,0x1E0CD40},{0x2B4DD40,0x1CA92D8},
      {0x2CC4A40,0x1E39A00},{0x2BCD5C0,0x1D1E300}};
    auto original=mem;
    for(auto [at,byte]:original){(void)byte;if(at%8)continue;uint64_t v{};bool full=true;
      for(unsigned i=0;i<8;++i){auto f=original.find(at+i);if(f==original.end()){full=false;break;}v|=uint64_t(f->second)<<(i*8);}
      if(full)for(auto [old,next]:ids)if(v==M+old){put(at,M+next);break;}}
    for(auto [old,next]:ids){bool full=true;uint64_t v{};
      for(unsigned i=0;i<8;++i){auto f=mem.find(M+old+i);if(f==mem.end()){full=false;break;}v|=uint64_t(f->second)<<(i*8);}
      if(full){put(M+next,v);for(unsigned i=0;i<8;++i)mem.erase(M+old+i);}}
    original=mem;for(auto [at,byte]:original)if(at>=C+0x150&&at<C+0x38CD)mem.erase(at);
    for(auto [at,byte]:original)if(at>=C+0x150&&at<C+0x38CD)put<uint8_t>(at+24,byte);
    put<uint32_t>(D+0xD0,0xDEADBEEF);put<uint32_t>(D+0xD8,0x300);
    mb<uint32_t>(0,0x464F5544);state.bridge.poll(h,botw::native_quick(h));
  }
  void held_runes() {
    put<uint64_t>(U+0x64C30,0x1E09000004ULL);put<uint32_t>(U+0x64C24,3);put<uint32_t>(U+0x64C58,0x10000);
    put(C+0x168,M+botw::Shortcut190.menu_state);put<int32_t>(C+0x3738,4);put<int32_t>(C+0x373C,4);
    put<int32_t>(C+0x3868,8);put(C+0x3870,Buffer);put(Buffer,std::array<int32_t,8>{0,1,2,3,4,5,6,7});
    put<int32_t>(C+0x3878,3);put<int32_t>(C+0x387C,3);put<int32_t>(C+0x3880,8);
    state.bridge.poll(h,botw::native_quick(h));
  }
};
}
int main() {
  using F=Shortcut190Fixture;
  {F f;assert(!botw::supported(f.h)&&botw::shortcut_supported(f.h));auto r=botw::world_runes(f.h);
    assert(r.valid&&r.list.count==8&&r.list.ids[6]==6&&r.list.ids[7]==7&&r.dlc_version==0x300);
    assert(f.request(2));assert(f.get<uint64_t>(640)==f.C&&f.get<uint64_t>(664)==f.D);
    f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.put<int32_t>(f.R+0x240,2);f.state.bridge.poll(f.h,{});assert(f.state.bridge.pending);
    f.put<int32_t>(f.C+0x3700,2);f.state.bridge.poll(f.h,{});assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)=="Rune selected");}
  {F f;f.put<uint32_t>(f.D+0xD8,0x200);auto r=botw::world_runes(f.h);assert(r.valid&&r.list.count==7&&!f.request(6));}
  {F f;f.flag(0x511872C2,false);auto r=botw::world_runes(f.h);assert(r.valid&&r.list.count==7&&!f.request(7));}
  {F f;f.held_runes();auto r=botw::runes(f.h);assert(r.valid&&r.count==8&&r.highlighted==3);
    assert(f.state.bridge.request(4,botw::QuickKind::Rune,{},r));assert(f.get<uint32_t>(20)==4&&f.get<uint32_t>(24)==4);
    f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.put<int32_t>(f.C+0x387C,4);f.state.bridge.poll(f.h,botw::native_quick(f.h));
    assert(!f.state.bridge.pending);int32_t current{};assert(botw::read(f.h,f.R+0x240,current)&&current==3);}
  {F f;f.held_runes();f.put<uint64_t>(f.C+0x168,0);assert(!botw::native_quick(f.h).valid&&!botw::runes(f.h).valid);}
  {F f;f.held_runes();f.put(f.C+0x168,f.M+botw::Shortcut160.menu_state);assert(!botw::native_quick(f.h).valid);}
  {F f;f.race=f.C+0x3700;assert(!botw::world_runes(f.h).valid);}
  {F f;f.held_runes();f.race=f.C+0x387C;assert(!botw::runes(f.h).valid);}
  for(int type:{0,1,2,3}) {F f;f.held_runes();const int selector=type==3?1:type==1?2:type==2?3:0;
    f.put<int32_t>(f.C+0x3738,selector);f.put<int32_t>(f.C+0x373C,selector);f.put<uint32_t>(f.U+0x64C58,(type==3||type==2)?0x40000:0x80000);
    f.put(f.C+0x37C8,std::array<uint64_t,20>{F::Buffer});f.put<int32_t>(f.C+0x3878,0);f.put<int32_t>(f.C+0x387C,0);f.put<int32_t>(f.C+0x3880,1);f.put<uint8_t>(f.C+0x38E4,3);
    f.put(F::Buffer,std::array<uint8_t,0x38>{});f.put<int32_t>(F::Buffer+0x18,type);f.put<int32_t>(F::Buffer+0x20,400);f.put<uint8_t>(F::Buffer+0x25,1);f.put(F::Buffer+0x30,F::Buffer+0x100);
    constexpr char actor[]="Weapon_Test";for(unsigned i=0;i<sizeof(actor);++i)f.put<char>(F::Buffer+0x100+i,actor[i]);
    botw::Snapshot pouch{true,{{actor,type,400,false,F::Buffer}}};auto gear=botw::gear(f.h,pouch);assert(gear.valid&&gear.items.size()==1&&gear.initial==0);
    f.put<uint8_t>(f.C+0x38E4,2);assert(!botw::gear(f.h,pouch).valid);
  }
  {F f;f.h.build_id[0]^=1;assert(!botw::shortcut_supported(f.h)&&!botw::world_runes(f.h).valid);}
  std::cout<<"1.9 shortcut states/shift, held gear/runes, DLC/options and native completion passed\n";
}
