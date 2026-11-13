// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
struct BridgeFixture : Fixture {
  static constexpr uint64_t Screen=0x7005000,Pouch=0x8000000,One=0x8100000,Two=0x8100100;
  std::array<uint8_t,4096> mailbox{};
  std::vector<uint32_t> stores;
  uint32_t fail=UINT32_MAX,change_epoch_at=UINT32_MAX;
  State state;
  template<class T> T get(uint32_t at) {T v{};std::memcpy(&v,mailbox.data()+at,sizeof(v));return v;}
  template<class T> void mb(uint32_t at,T v) {std::memcpy(mailbox.data()+at,&v,sizeof(v));}
  BridgeFixture() {
    texts["page"]="ready";values["aux"]=1;values["epoch"]=1;mb<uint32_t>(4092,1);
    h.get_text=[](void* p,const char*)->const char*{return static_cast<BridgeFixture*>(p)->texts["page"].c_str();};
    h.get_i64=[](void* p,const char* name,int64_t)->int64_t{auto& f=*static_cast<BridgeFixture*>(p);return f.values[std::strcmp(name,"__mailbox_epoch")==0 ? "epoch" : "aux"];};
    put(M+0x2CA6D50,Pouch);put<uint64_t>(Pouch+0x68,One+8);put<uint64_t>(Pouch+0x70,Two+8);
    put<int32_t>(Pouch+0x78,2);put<int32_t>(Pouch+0x7C,8);
    for (uint64_t address:{One,Two}) {
      put(address,std::array<uint8_t,0x38>{});put<int32_t>(address+0x20,400);
      put<uint8_t>(address+0x24,address==One);put<uint8_t>(address+0x25,1);
      put<uint64_t>(address+0x30,address+0x80);
      const char actor[]="Weapon_Test";for(size_t i=0;i<sizeof(actor);++i)put<char>(address+0x80+i,actor[i]);
    }
    put<uint64_t>(One+8,Two+8);put<uint64_t>(One+16,Pouch+0x68);
    put<uint64_t>(Two+8,Pouch+0x68);put<uint64_t>(Two+16,One+8);
    put(A+360,Screen);put(Screen,M+0x2BA1D40);put(Screen+0x150,M+0x2BA1C10);
    put<int32_t>(Screen+0x3720,0);put<int32_t>(Screen+0x3724,0);
    put<uint32_t>(U+0x64C24,3);put<uint32_t>(U+0x64C58,0x80000);put<uint64_t>(U+0x64C30,0x1E09000004ULL);
    put(Screen+0x37B0,std::array<uint64_t,20>{Two,One});put<int32_t>(Screen+0x3860,1);
    put<int32_t>(Screen+0x3864,1);put<int32_t>(Screen+0x3868,2);put<uint8_t>(Screen+0x38CC,3);
    EdenDsmodHostExtensions ext{};ext.version=EDEN_DSMOD_EXT_VERSION;ext.struct_size=sizeof(ext);
    ext.abi_hash=EDEN_DSMOD_EXT_HASH;ext.userdata=this;ext.mailbox_size=4096;
    ext.load_u32=[](void* p,uint32_t at,uint32_t* v)->EdenDsmodBool{auto& f=*static_cast<BridgeFixture*>(p);assert(at<=4092 && at%4==0);*v=f.get<uint32_t>(at);return true;};
    ext.store_u32=[](void* p,uint32_t at,uint32_t v)->EdenDsmodBool{auto& f=*static_cast<BridgeFixture*>(p);assert(at<=4092 && at%4==0);if(at==f.fail)return false;f.mb(at,v);f.stores.push_back(at);if(at==f.change_epoch_at){f.values["epoch"]++;f.mb(4092,uint32_t(f.values["epoch"]));}return true;};
    ext.store_u64=[](void* p,uint32_t at,uint64_t v)->EdenDsmodBool{auto& f=*static_cast<BridgeFixture*>(p);assert(at<=4088 && at%8==0);if(at==f.fail)return false;f.mb(at,v);f.stores.push_back(at);return true;};
    mb<uint32_t>(0,0x464F5544);mb<uint32_t>(4,100);state.bridge.configure(&ext);
    state.bridge.poll(h,botw::native_quick(h));state.quick.armed=true;state.quick.shown=botw::QuickKind::Weapon;
    state.gear=botw::gear(h,botw::pouch(h));assert(state.gear.valid);
  }
  bool choose(int64_t index=0) {return on_action(&state,"select",index);}
};
// Switch the guest category after sample()'s stable observed selector and before
// gear() takes its own stable snapshot. Both readers can be individually valid.
struct CategoryRaceFixture : BridgeFixture {
  decltype(EdenDsmodHostApi{}.read_memory) original_read{};
  unsigned selector_reads{};
  CategoryRaceFixture() {
    for(auto item:{One,Two})put<int32_t>(item+0x18,2);
    state.data=botw::pouch(h);assert(state.data.valid);
    state.context=botw::UiContext::Menu;
    state.next=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    original_read=h.read_memory;
    h.read_memory=[](void* p,uint64_t at,void* buffer,size_t size)->EdenDsmodBool {
      auto& f=*static_cast<CategoryRaceFixture*>(p);
      if(at==Screen+0x3720 && ++f.selector_reads==3) {
        f.put<int32_t>(Screen+0x3720,3);f.put<int32_t>(Screen+0x3724,3);
        f.put<uint32_t>(U+0x64C58,0x40000);
      }
      return f.original_read(p,at,buffer,size);
    };
  }
};
int main() {
  unsigned passed=0;
  {BridgeFixture f;assert(f.choose());assert(f.get<uint32_t>(8)==1 && f.get<uint32_t>(72)==1);
   assert(f.get<uint64_t>(40)==f.Two && f.stores.back()==8 && f.state.bridge.pending==1);
   assert(f.state.quick.armed && f.state.quick.shown==botw::QuickKind::Weapon && f.state.gear.valid && !f.choose());++passed;}
  // The exclusive food-job claim reserves UINT32_MAX as its busy value.
  for(uint32_t seq:{UINT32_MAX-1,UINT32_MAX}) {
    BridgeFixture f;f.mb(8,seq);f.mb(12,seq);assert(f.choose());
    assert(f.get<uint32_t>(8)==1);++passed;
  }
  auto refuse=[&](auto change,int64_t index=0){BridgeFixture f;change(f);assert(!f.choose(index));assert(f.get<uint32_t>(8)==0);++passed;};
  refuse([](auto& f){f.state.quick.armed=false;});
  refuse([](auto& f){f.texts["page"]="inventory";});
  refuse([](auto& f){f.values["aux"]=0;});
  refuse([](auto& f){f.state.bridge.healthy=false;});
  refuse([](auto& f){f.state.bridge.sampled-=std::chrono::seconds(1);});
  refuse([](auto& f){f.state.bridge.heartbeat_at-=std::chrono::seconds(1);});
  refuse([](auto& f){f.put(f.U+0x64C58,uint32_t(0));});
  refuse([](auto& f){f.state.gear.items[0].address=f.One;});
  refuse([](auto& f){f.state.gear.items[0].actor="Wrong";});
  refuse([](auto& f){f.state.gear.items[0].value=401;});
  refuse([](auto& f){f.put(f.Screen+0x3864,int32_t(0));});
  refuse([](auto& f){f.put(f.Screen+0x3860,int32_t(0));});
  refuse([](auto&){},-1);refuse([](auto&){},2);refuse([](auto&){},20);
  refuse([](auto& f){f.mb(12,uint32_t(1));});
  refuse([](auto& f){f.fail=64;});
  {BridgeFixture f;assert(!on_action(&f.state,"wrong",0));++passed;}
  for (unsigned result:{1u,2u}) {
    BridgeFixture f;assert(f.choose());f.put<int32_t>(f.Screen+0x3864,0);f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,result);
    f.state.bridge.poll(f.h,botw::native_quick(f.h));assert(!f.state.bridge.pending && f.get<uint32_t>(72)==0);
    assert(std::string(f.state.bridge.status)==(result==1 ? "Highlighted" : "Selection refused"));++passed;
  }
  {BridgeFixture f;assert(f.choose());f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.state.bridge.poll(f.h,{});
   assert(std::string(f.state.bridge.status)=="Selection not confirmed");++passed;}
  {BridgeFixture f;assert(f.choose());f.texts["page"]="inventory";f.state.bridge.poll(f.h,botw::native_quick(f.h));
   assert(f.get<uint32_t>(72)==0 && f.state.bridge.pending==1);++passed;}
  {BridgeFixture f;assert(f.choose());f.values["aux"]=0;tick(&f.state,&f.h);assert(f.get<uint32_t>(72)==0);++passed;}
  {BridgeFixture f;f.mb<uint32_t>(0,0x314F5544);f.state.bridge.poll(f.h,botw::native_quick(f.h));assert(!f.state.bridge.healthy);++passed;}
  refuse([](auto& f){f.values["epoch"]=2;});
  refuse([](auto& f){f.mb(4092,uint32_t(2));});
  {BridgeFixture f;f.values["epoch"]=0;f.state.bridge.poll(f.h,{});assert(!f.state.bridge.healthy);++passed;}
  {BridgeFixture f;assert(f.choose());f.values["epoch"]=2;f.mb<uint32_t>(4092,2);f.state.bridge.poll(f.h,botw::native_quick(f.h));
   assert(f.get<uint32_t>(72)==0 && f.state.bridge.epoch_changed);++passed;}
  {BridgeFixture f;f.mb<uint32_t>(0,0x324F5544);f.state.bridge.poll(f.h,{});assert(!f.state.bridge.healthy);++passed;}
  refuse([](auto& f){f.values["epoch"]=0;});
  refuse([](auto& f){f.change_epoch_at=88;});
  {BridgeFixture f;f.values["epoch"]=2;f.mb<uint32_t>(4092,2);sample(&f.state,&f.h);
   assert(!f.state.quick.armed && f.state.quick.shown==botw::QuickKind::None && !f.state.gear.valid);++passed;}
  for(uint32_t magic:{0x334F5544u,0x454F5544u}) {
    BridgeFixture f;f.mb(0,magic);f.state.bridge.poll(f.h,botw::native_quick(f.h));
    assert(f.state.bridge.healthy && !f.state.bridge.cursor_supported && !f.choose());++passed;
  }
  {BridgeFixture f;assert(f.choose());f.put<int32_t>(f.Screen+0x3864,0);
   f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.state.bridge.poll(f.h,botw::native_quick(f.h));
   assert(std::string(f.state.bridge.status)=="Highlighted" && f.state.quick.armed);
   uint8_t equipped{};assert(botw::read(f.h,f.Two+0x24,equipped) && !equipped);
   assert(botw::read(f.h,f.One+0x24,equipped) && equipped);++passed;}
  {BridgeFixture f;assert(f.choose());f.put<int32_t>(f.Screen+0x3864,0);
   f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.values["epoch"]=2;f.mb<uint32_t>(4092,2);
   f.state.bridge.poll(f.h,botw::native_quick(f.h));assert(std::string(f.state.bridge.status)=="Selection not confirmed");++passed;}
  {BridgeFixture f;assert(f.choose());f.put<uint32_t>(f.U+0x64C58,0);
   f.state.bridge.poll(f.h,botw::native_quick(f.h));assert(f.get<uint32_t>(72)==0);
   f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.state.bridge.poll(f.h,botw::native_quick(f.h));
   assert(!f.state.bridge.pending && std::string(f.state.bridge.status)=="Selection not confirmed" && f.get<uint32_t>(8)==1);++passed;}
  {BridgeFixture f;
   f.put<int32_t>(f.Screen+0x3720,4);f.put<int32_t>(f.Screen+0x3724,4);f.put<uint32_t>(f.U+0x64C58,0x10000);
   constexpr uint64_t B=0x9000000,G=0xA000000,H=0xA001000,F=0xA002000,P=0xA003000,X=0xA004000,Y=0xA005000;
   f.put<int32_t>(f.Screen+0x3850,8);f.put(f.Screen+0x3858,B);f.put<int32_t>(f.Screen+0x3868,3);
   f.put(B,std::array<int32_t,8>{0,2,4,-1,-1,-1,-1,-1});
   f.put(f.M+0x2CC5FE8,G);f.put(G,f.M+0x2BD07E0);f.put(G+0xBE0,H);f.put(H,F);
   f.put<int32_t>(F+8,2);f.put<int32_t>(F+12,2);f.put(F+16,P);f.put(P,X);f.put(P+8,Y);
   for(auto p:{X,Y}){f.put(p,f.M+0x2BD1078);f.put<uint32_t>(p+8,0x01000F00);}
   f.put<uint32_t>(X+12,0x604C3BAE);f.put<uint32_t>(Y+12,0x9CA3D1C1);
   f.state.quick.shown=botw::QuickKind::Rune;f.state.runes=botw::runes(f.h);assert(f.state.runes.valid);
   assert(f.choose(2) && f.get<uint64_t>(40)==4);f.put<int32_t>(f.Screen+0x3864,2);
   f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.state.bridge.poll(f.h,botw::native_quick(f.h));
   assert(std::string(f.state.bridge.status)=="Highlighted" && f.state.quick.armed);
   assert(f.state.quick.shown==botw::QuickKind::Rune && f.state.runes.valid);
   const auto fresh=botw::runes(f.h);assert(fresh.valid && fresh.selected==1 && fresh.highlighted==2);++passed;}
  {BridgeFixture f;
   for(auto item:{f.One,f.Two})f.put<int32_t>(item+0x18,2);
   f.put<int32_t>(f.Screen+0x3720,3);f.put<int32_t>(f.Screen+0x3724,3);f.put<uint32_t>(f.U+0x64C58,0x40000);
   f.state.quick.shown=botw::QuickKind::Arrow;f.state.gear=botw::gear(f.h,botw::pouch(f.h));assert(f.state.gear.valid);
   assert(f.choose(0));assert(f.get<uint32_t>(20)==3);
   assert(f.state.quick.armed && f.state.quick.shown==botw::QuickKind::Arrow && f.state.gear.valid);
   f.put<int32_t>(f.Screen+0x3864,0);f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.state.bridge.poll(f.h,botw::native_quick(f.h));
   assert(std::string(f.state.bridge.status)=="Highlighted");uint8_t eq{};assert(botw::read(f.h,f.Two+0x24,eq) && !eq);
   sample(&f.state,&f.h);
   assert(f.values["botw.quick.held_gear"]==1 && f.values["botw.quick.highlighted"]==0);
   assert(f.texts["botw.quick.title"]=="Arrows" && f.values["botw.quick.selected.quantity"]==400);
   assert(f.values["botw.quick.selected.stat"]==-1 && f.values["botw.quick.selected.durability_pct"]==-1);
   assert(f.values["botw.quick.selected.modifier"]==0 && f.values["botw.quick.selected.shots"]==-1);
   // The next category must clear quantity and follow the new native highlight.
   for(auto item:{f.One,f.Two})f.put<int32_t>(item+0x18,0);
   f.put<int32_t>(f.Screen+0x3720,0);f.put<int32_t>(f.Screen+0x3724,0);f.put<uint32_t>(f.U+0x64C58,0x80000);
   f.state.next={};sample(&f.state,&f.h);
   assert(f.texts["botw.quick.title"]=="Weapons" && f.values["botw.quick.selected.quantity"]==-1);
   // Iteration 8 owner decision 2026-10-19: the held D-pad wheel now also gets a synthetic None
   // entry at items[0] on Weapon/Bow/Shield (never Arrow, see the Arrow case above, unaffected) --
   // native's own highlighted index (0 here) shifts by the one inserted slot.
   assert(f.values["botw.quick.highlighted"]==1 && f.values["botw.quick.held_gear"]==1);++passed;}
  {CategoryRaceFixture f;sample(&f.state,&f.h);
   assert(f.selector_reads>=6 && f.state.quick.shown==botw::QuickKind::Weapon);
   assert(!f.state.gear.valid && f.values["botw.quick.gear"]==0 && f.values["botw.quick.held_gear"]==0);
   assert(f.values["botw.quick.highlighted"]==-1 && f.values["botw.quick.selected.valid"]==0);
   assert(f.values["botw.quick.selected.quantity"]==-1);++passed;}
  std::cout<<passed<<" host selection scope/identity/publication/cancellation/readback cases passed\n";
}
