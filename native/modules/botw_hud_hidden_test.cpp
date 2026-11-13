// SPDX-License-Identifier: GPL-3.0-or-later
// Iteration 11 item E: hidden companion = native HUD back. tick() mirrors the host's
// __aux_present into mailbox+2048 (1 shown / 0 native) on edges only; the guest helper
// (prototype/companion-hud.inc, prototype/tests/hud-hook-test.py) acts on it.
#include "01007EF00011E000.cpp"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
struct HudHiddenFixture : Fixture {
  std::array<uint8_t,4096> mailbox{};
  std::vector<std::pair<uint32_t,uint32_t>> stores;
  unsigned loads{},publishes{},aux_queries{};
  int64_t aux=1;
  bool aux_key=true;
  State state;
  EdenDsmodHostExtensions ext{};
  uint32_t get(uint32_t at) const {uint32_t v{};std::memcpy(&v,mailbox.data()+at,4);return v;}
  void mb(uint32_t at,uint32_t v) {std::memcpy(mailbox.data()+at,&v,4);}
  HudHiddenFixture() {
    h.get_i64=[](void* p,const char* name,int64_t fallback)->int64_t{
      auto& f=*static_cast<HudHiddenFixture*>(p);
      if(std::strcmp(name,"__aux_present")==0){++f.aux_queries;return f.aux_key ? f.aux : fallback;}
      return fallback;
    };
    h.publish_i64=[](void* p,const char*,int64_t){++static_cast<HudHiddenFixture*>(p)->publishes;};
    h.publish_text=[](void* p,const char*,const char*){++static_cast<HudHiddenFixture*>(p)->publishes;};
    ext.version=EDEN_DSMOD_EXT_VERSION;ext.struct_size=sizeof(ext);ext.abi_hash=EDEN_DSMOD_EXT_HASH;
    ext.userdata=this;ext.mailbox_size=4096;
    ext.load_u32=[](void* p,uint32_t at,uint32_t* v)->EdenDsmodBool{auto& f=*static_cast<HudHiddenFixture*>(p);assert(at<=4092 && at%4==0);++f.loads;*v=f.get(at);return true;};
    ext.store_u32=[](void* p,uint32_t at,uint32_t v)->EdenDsmodBool{auto& f=*static_cast<HudHiddenFixture*>(p);assert(at<=4092 && at%4==0);f.mb(at,v);f.stores.emplace_back(at,v);return true;};
    ext.store_u64=[](void*,uint32_t,uint64_t)->EdenDsmodBool{return true;};
    state.bridge.configure(&ext);
    stores.clear();
  }
  unsigned hud_stores() const {unsigned n=0;for(auto& s:stores)if(s.first==2048)++n;return n;}
  void step() {tick(&state,&h);}
};
int main() {
  HudHiddenFixture f;
  assert(f.get(2048)==0); // Missing module / untouched mailbox keeps native HUD.
  f.step();assert(f.get(2048)==1&&f.hud_stores()==1);
  for(int i=0;i<100;++i){f.step();}
  assert(f.hud_stores()==1);
  f.mb(4,120);f.step();assert(f.get(2048)==121);
  f.stores.clear();
  f.aux=0;f.step();assert(f.get(2048)==0&&f.hud_stores()==1);
  for(int i=0;i<100;++i){f.step();}
  assert(f.hud_stores()==1);
  f.aux=1;f.step();assert(f.get(2048)==121&&f.hud_stores()==2);
  for(auto [at,value]:f.stores){(void)value;assert(at!=2052);}
  auto* state=new State;state->bridge.configure(&f.ext);
  destroy(state);assert(f.get(2048)==0);
  f.state.bridge.configure(nullptr);f.stores.clear();f.step();assert(f.stores.empty());
  std::cout<<"native HUD zero/default, shown/hidden edges, detach and missing mailbox passed\n";
}
