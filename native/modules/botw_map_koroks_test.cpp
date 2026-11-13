// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct KorokFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Array=0xA010000,Records=0xA100000,
 Sources=0xA200000,Manager=0xA300000,Holder=0xA310000,Bank=0xA320000,
 Flags=0xA330000,Data=0xA400000;
 KorokFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);
  put<int32_t>(Map+0x360,900);put<int32_t>(Map+0x364,900);put(Map+0x368,Array);
  put(M+0x2CC5FE8,Manager);put(Manager,M+0x2BD07E0);put(Manager+0xBE0,Holder);put(Holder,Bank);
  put<uint8_t>(Manager+0xBE8,1);put<uint32_t>(Manager+0xC18,0x1010);put<uint32_t>(Manager+0xC28,0);
  put<int32_t>(Bank+8,900);put<int32_t>(Bank+12,900);put(Bank+16,Flags);
  for(unsigned i=0;i<900;++i){auto p=Records+i*0x100,s=Sources+i*16,f=Data+i*16;
   put(Array+i*8,p);put(p,M+0x2B86C10);put<int32_t>(p+0x1C,17);put(p+0x28,std::array<float,3>{float(i),300,-float(i)});
   put(p+0x50,s);put<uint64_t>(s,0xB000000+i*64);put<uint32_t>(s+8,i);
   put(Flags+i*8,f);put(f,M+0x2BD1078);put<uint8_t>(f+8,0);put<uint8_t>(f+9,7);put<uint8_t>(f+10,0);put<uint8_t>(f+11,i%2?0x16:0x17);put(f+12,botw::korok_flag_hashes[i]);}
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
int main(){using F=KorokFixture;unsigned cases{};
 auto check=[&](auto change,bool valid){F f;change(f);const auto v=botw::map_koroks(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x368,F::Array,F::Records+0x50,F::M+0x2CC5FE8,F::Manager+0xBE0,F::Holder,F::Bank+16,F::Flags})
  for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Map,F::Records,F::Manager,F::Data})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(int32_t n:{-1,0,899,901,INT32_MAX})check([&](F&f){f.put(F::Map+0x360,n);},false);
 check([](F&f){f.put<int32_t>(F::Map+0x364,901);},false);
 for(int32_t n:{-1,0,65537,INT32_MAX})check([&](F&f){f.put(F::Bank+8,n);},false);
 check([](F&f){f.put<int32_t>(F::Bank+12,899);},false);
 check([](F&f){f.put<int32_t>(F::Bank+12,65537);},false);
 check([](F&f){f.put(F::Array+8,F::Records);},false);
 check([](F&f){f.put(F::Records+0x150,F::Sources);},false);
 check([](F&f){f.put(F::Flags+8,F::Data);},false);
 for(uint32_t n:{900u,0xFFFFFFFFu,0x80000000u})check([&](F&f){f.put(F::Sources+8,n);},false);
 check([](F&f){f.put<uint32_t>(F::Manager+0xC18,0x9010);},true);
 check([](F&f){f.put<uint32_t>(F::Manager+0xC18,0x9010);f.put<uint32_t>(F::Manager+0xC28,1);},false);
 check([](F&f){f.put<uint32_t>(F::Manager+0xC18,0x9010);f.put<uint32_t>(F::Manager+0xC28,256);},false);
 check([](F&f){f.put<uint32_t>(F::Manager+0xC18,0x9010);f.put<uint32_t>(F::Manager+0xC28,3);for(unsigned i=0;i<900;++i)f.put<uint32_t>(F::Sources+i*16+8,0x3000000+i);},true);
 check([](F&f){f.put<uint8_t>(F::Manager+0xBE8,2);},false);
 check([](F&f){f.put<uint8_t>(F::Data+8,1);},false);check([](F&f){f.put<uint32_t>(F::Data+12,0);},false);
 check([](F&f){f.put<int32_t>(F::Records+0x1C,18);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f})check([&](F&f){f.put(F::Records+0x28,n);},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x360,F::Map+0x368,F::Array,F::Records,F::Records+0x1C,F::Records+0x28,F::Records+0x50,F::Sources,F::Sources+8,F::Flags,F::Data,F::Manager+0xBE8,F::Manager+0xC18,F::Manager+0xC28,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 {F f;auto v=botw::map_koroks(f.h);assert(v.valid&&v.entries[0].visible&&!v.entries[1].visible);botw::publish_map_koroks(f.h,v);assert(f.values["botw.map.koroks.count"]==450);++cases;
  f.put<uint8_t>(F::Data+9,6);v=botw::map_koroks(f.h);assert(v.valid&&!v.entries[0].visible);++cases;
  f.put<uint8_t>(F::Manager+0xBE8,0);v=botw::map_koroks(f.h);assert(v.valid&&v.entries[0].visible);++cases;
  // Perf round 3b: publish_map_koroks no longer self-clears a stale slot's keys when that slot
  // goes inactive (it relies on the runtime's own per-tick snapshot clear, same contract round 1
  // established -- "no cross-tick persistence to lean on" -- see botw_map_koroks.h); simulate that
  // clear explicitly here, same pattern already used by botw_tracker_view_test.cpp.
  f.values.clear();f.texts.clear();
  botw::publish_map_koroks(f.h,{});assert(!f.values["botw.map.koroks.valid"]&&!f.values["botw.map.koroks.count"]&&!f.values.count("botw.map.koroks.0.kind")&&f.texts["botw.map.koroks.icon"].empty());++cases;}
 {F f;State s;sample(&s,&f.h);assert(f.values["botw.map.koroks.valid"]);f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);s.next={};f.values.clear();f.texts.clear();sample(&s,&f.h);assert(!f.values["botw.map.koroks.valid"]&&!f.values.count("botw.map.koroks.0.kind"));++cases;}
 {F f;State s;sample(&s,&f.h);f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};s.next={};sample(&s,&f.h);assert(!f.values["botw.map.koroks.valid"]);++cases;}
 {F f;auto v=botw::map_koroks(f.h);assert(v.entries[0].found&&v.entries[0].readable&&!v.entries[1].found&&v.entries[1].readable);
  botw::publish_map_koroks(f.h,v,botw::KorokFilter::Unfound);assert(f.values["botw.map.koroks.count"]==450&&!f.values.count("botw.map.koroks.0.kind")&&f.values["botw.map.koroks.1.kind"]==17);++cases;
  f.values.clear();f.texts.clear();
  botw::publish_map_koroks(f.h,v,botw::KorokFilter::All);assert(f.values["botw.map.koroks.count"]==900);assert(f.values["botw.map.koroks.0.tint"]==0xFFF6DC9D&&f.values["botw.map.koroks.1.tint"]==0xFF9DAEAB);++cases;
  f.values.clear();f.texts.clear();
  f.put<uint8_t>(F::Data+16+9,6);v=botw::map_koroks(f.h);assert(!v.entries[1].readable);botw::publish_map_koroks(f.h,v,botw::KorokFilter::Unfound);assert(f.values["botw.map.koroks.count"]==449&&!f.values.count("botw.map.koroks.1.kind"));++cases;
  f.values.clear();f.texts.clear();
  botw::publish_map_koroks(f.h,{},botw::KorokFilter::All);assert(f.values["botw.map.koroks.count"]==0&&!f.values.count("botw.map.koroks.1.kind")&&f.values["botw.map.koroks.1.tint"]==0);++cases;
 }
 std::cout<<cases<<" Korok discovery/permissions/handles/spoiler/race/publication cases passed\n";
}

#endif
