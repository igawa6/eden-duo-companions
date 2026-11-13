// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct MovingFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Records=0xA010000,Sources=0xA100000;
 MovingFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);
  constexpr int kinds[]={8,9,7,10,8,9,7,10,18};
  for(unsigned i=0;i<9;++i){auto p=Records+i*0x100;put(Map+(i<8?0x320+i*8:0x370),p);put(p,M+(i<8?0x2B86388:0x2B86B18));put<int32_t>(p+0x1C,kinds[i]);put(p+0x28,std::array<float,3>{float(i),300,-float(i)});put<uint32_t>(p+0x3C,i<4?0x88000:i<8?0x88090:0x10);
   if(i<8){auto source=Sources+i*0x40;put(p+0x58,source);put<int32_t>(source+0x1C,1);put<int32_t>(p+0x60,1);put<uint8_t>(p+0x90,i>=4);put<uint8_t>(p+0x91,1);}else put<uint8_t>(p+0x50,0);}
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
  // Perf: sample()'s map-publish chain now gates its per-entry loop to
  // page=="map" (full=false elsewhere); this fixture's dispatcher-level sample() assertions below
  // need get_text/__page wired so those assertions keep exercising the full per-entry path, same
  // as the already-established EditBridgeFixture pattern (botw_map_actions_test.cpp).
  h.get_text = [](void* p, const char* k) -> const char* {
    auto& f = *static_cast<Fixture*>(p);
    auto it = f.texts.find(k);
    return it == f.texts.end() ? nullptr : it->second.c_str();
  };
  texts["__page"] = "map";
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=MovingFixture;unsigned cases{};auto check=[&](auto change,bool valid){F f;change(f);auto v=botw::map_moving(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x320,F::Map+0x370,F::Records+0x58})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Map,F::Records,F::Records+8*0x100})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(int32_t kind:{-1,7,INT32_MAX})check([&](F&f){f.put(F::Records+0x1C,kind);},false);
 check([](F&f){f.put(F::Map+0x340,F::Records);},false);check([](F&f){f.put(F::Records+0x100+0x58,F::Sources);},false);
 check([](F&f){f.put<int32_t>(F::Sources+0x1C,-1);},false);check([](F&f){f.put<int32_t>(F::Records+0x60,2);},false);
 check([](F&f){f.put<uint8_t>(F::Records+0x90,1);},false);check([](F&f){f.put<uint8_t>(F::Records+0x91,2);},false);check([](F&f){f.put<uint8_t>(F::Records+0x850,2);},false);
 check([](F&f){f.put<uint32_t>(F::Records+0x43C,0x90);},false);
 check([](F&f){f.put<uint32_t>(F::Records+0x3C,0x88090);},false);
 check([](F&f){f.put<uint32_t>(F::Records+0x3C,0x8090);f.put<uint32_t>(F::Records+0x43C,0x8090);},false);
 check([](F&f){f.put<uint32_t>(F::Records+0x3C,0x8090);f.put<uint32_t>(F::Records+0x43C,0x88000);},true);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Records+0x428,n);},false);
 check([](F&f){f.put(F::Records+0x828,std::array<float,3>{-100000,0,0});f.put<uint32_t>(F::Records+0x83C,0);},true);
 check([](F&f){f.put(F::Records+0x828,std::array<float,3>{-100000,0,0});},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x320,F::Map+0x370,F::Records,F::Records+0x1C,F::Records+0x28,F::Records+0x58,F::Sources+0x1C,F::Records+0x60,F::Records+0x90,F::Records+0x91,F::Records+0x850,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 {F f;f.h.read_memory=[](void* p,uint64_t a,void* b,size_t n)->EdenDsmodBool{auto& f=*static_cast<F*>(p);for(size_t i=0;i<n;++i)static_cast<uint8_t*>(b)[i]=f.mem.at(a+i);if(a==F::Records+0x3C&&++f.reads==2)static_cast<uint8_t*>(b)[0]^=0x10;return true;};assert(!botw::map_moving(f.h).valid);++cases;}
 {F f;auto v=botw::map_moving(f.h);assert(v.valid&&!v.entries[0].visible&&v.entries[4].visible&&v.entries[8].visible);botw::publish_map_moving(f.h,v);assert(f.values["botw.map.moving.count"]==5);++cases;
  // Perf round 4: an inactive slot is no longer published at all; clear
  // first (same pattern round 3 used) so the invalid-pass check reflects this publish, not stale
  // per-slot state left over from the active publish above.
  f.values.clear();botw::publish_map_moving(f.h,{});assert(!f.values["botw.map.moving.valid"]&&!f.values["botw.map.moving.count"]&&!f.values.count("botw.map.moving.8.kind"));++cases;}
 // Iteration 12 lane M: Ready publishes the moving markers inside its follow window only.
 {F f;auto v=botw::map_moving(f.h);assert(v.valid&&v.entries[4].visible);
  const botw::MapPosition at{true,v.entries[4].xyz[0],-v.entries[4].xyz[2]};
  botw::publish_map_moving(f.h,v,true,&at);assert(f.values["botw.map.moving.4.kind"]==v.entries[4].kind);
  unsigned expect=0;for(unsigned i=0;i<9;++i){const auto& e=v.entries[i];const bool in=e.visible&&botw::moving_art(e.kind)&&botw::WithinReadyWindow(e.xyz[0],e.xyz[2],at);
    expect+=in;assert(f.values.count(botw::Key("botw.map.moving.",i)(".kind"))==(in?1u:0u));}
  assert(f.values["botw.map.moving.count"]==expect);++cases;
  f.values.clear();f.texts.clear();const botw::MapPosition far{true,5999.f,4999.f};bool any_far=false;
  for(const auto& e:v.entries)any_far=any_far||(e.visible&&botw::WithinReadyWindow(e.xyz[0],e.xyz[2],far));
  botw::publish_map_moving(f.h,v,true,&far);assert(any_far||f.values["botw.map.moving.count"]==0);++cases;
  // Off Map/Ready (full=false, no cull) still publishes no entries.
  f.values.clear();botw::publish_map_moving(f.h,v,false);assert(!f.values.count("botw.map.moving.4.kind"));++cases;}
 {F f;f.put<uint8_t>(F::Records+0x850,1);auto v=botw::map_moving(f.h);assert(v.valid&&!v.entries[8].visible);++cases;}
 {F f;for(unsigned i=0;i<8;++i){f.put<uint32_t>(F::Records+i*0x100+0x3C,0);f.put<int32_t>(F::Sources+i*0x40+0x1C,0);f.put<int32_t>(F::Records+i*0x100+0x60,0);}f.put<uint32_t>(F::Records+0x83C,0);auto v=botw::map_moving(f.h);assert(v.valid);botw::publish_map_moving(f.h,v);assert(!f.values["botw.map.moving.count"]);++cases;}
 {F f;State s;sample(&s,&f.h);assert(f.values["botw.map.moving.valid"]);f.values.clear();f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);s.next={};sample(&s,&f.h);assert(!f.values["botw.map.moving.valid"]&&!f.values.count("botw.map.moving.8.kind"));++cases;}
 {F f;State s;sample(&s,&f.h);f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};s.next={};sample(&s,&f.h);assert(!f.values["botw.map.moving.valid"]);++cases;}
 std::cout<<cases<<" native beast/horse visibility/position/identity/race/publication cases passed\n";
}

#endif
