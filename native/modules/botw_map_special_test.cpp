// SPDX-License-Identifier: GPL-3.0-or-later
#include "botw_map_special.h"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct SpecialFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Array=0xA010000,Records=0xA100000;
 SpecialFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);
  put<int32_t>(Map+0x598,150);put<int32_t>(Map+0x59C,150);put(Map+0x5A0,Array);
  constexpr uint64_t offsets[]={0x378,0x380,0x388,0x480},vt[]={0x2B86D18,0x2B86E10,0x2B86F08,0x2B875E8};constexpr int kinds[]={19,20,44,42};
  for(unsigned i=0;i<154;++i){auto p=Records+i*0x100;put(i<4?Map+offsets[i]:Array+(i-4)*8,p);put(p,M+(i<4?vt[i]:0x2B865B8));put<int32_t>(p+0x1C,i<4?kinds[i]:26);put(p+0x28,std::array<float,3>{float(i),300,-float(i)});put<uint32_t>(p+0x3C,i<5?0x10:0);if(!i)put<uint8_t>(p+0x50,1);if(i>=4)put<uint64_t>(p+0x50,i==4?0xB000000:0);}
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=SpecialFixture;unsigned cases{};auto check=[&](auto change,bool valid){F f;change(f);auto v=botw::map_special(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x378,F::Map+0x5A0,F::Array})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Map,F::Records,F::Records+0x400})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(int32_t n:{-1,151,INT32_MAX})check([&](F&f){f.put(F::Map+0x598,n);},false);
 for(int32_t n:{0,149,151})check([&](F&f){f.put(F::Map+0x59C,n);},false);
 check([](F&f){f.put<int32_t>(F::Map+0x598,0);f.put<uint64_t>(F::Map+0x5A0,0);},true);
 check([](F&f){f.put<int32_t>(F::Records+0x1C,20);},false);check([](F&f){f.put<int32_t>(F::Records+0x41C,25);},false);
 check([](F&f){f.put<uint8_t>(F::Records+0x50,0);},false);check([](F&f){f.put<uint8_t>(F::Records+0x50,2);},false);
 for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-3ULL})check([&](F&f){f.put(F::Records+0x450,p);},false);
 check([](F&f){f.put(F::Array+8,F::Records+0x400);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Records+0x428,n);},false);
 check([](F&f){f.put(F::Records+0x528,std::array<float,3>{-100000,0,0});},true);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x598,F::Map+0x59C,F::Map+0x5A0,F::Map+0x378,F::Records,F::Records+0x1C,F::Records+0x28,F::Records+0x50,F::Records+0x450,F::Array,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 {F f;auto v=botw::map_special(f.h);assert(v.valid&&v.entries[0].visible&&v.entries[4].visible&&!v.entries[5].visible);botw::publish_map_special(f.h,v);assert(f.values["botw.map.special.count"]==5&&f.values["botw.map.special.guardian.count"]==1&&f.values["botw.map.special.guardian.0.kind"]==26&&f.values["botw.map.special.3.kind"]==42);++cases;assert(f.values["botw.map.special.guardian.0.tint"]==0xFFFF0000);++cases;assert(f.values["botw.map.special.3.tint"]==0xFF0048FF);++cases;assert(f.values["botw.map.special.0.tint"]==0xFFF2CA66);++cases;f.values.clear();f.texts.clear();botw::publish_map_special(f.h,{});assert(!f.values["botw.map.special.valid"]&&!f.values["botw.map.special.count"]&&!f.values.count("botw.map.special.guardian.0.kind")&&!f.texts.count("botw.map.special.guardian.0.icon"));++cases;}
 {F f;for(unsigned i=0;i<154;++i)f.put<uint32_t>(F::Records+i*0x100+0x3C,0);f.put<uint8_t>(F::Records+0x50,0);auto v=botw::map_special(f.h);assert(v.valid);botw::publish_map_special(f.h,v);assert(!f.values["botw.map.special.count"]);++cases;}
 // Iteration 12: Ready (full=false) still publishes the singleton slots (placed Travel Medallion
 // slot 3 included) but never the guardian slice.
 {F f;auto v=botw::map_special(f.h);botw::publish_map_special(f.h,v,false);assert(f.values["botw.map.special.valid"]==1&&f.values["botw.map.special.0.kind"]==19&&!f.values.count("botw.map.special.guardian.0.kind")&&!f.values.count("botw.map.special.count"));++cases;}
 {F f;for(unsigned i=0;i<3;++i)f.put<uint32_t>(F::Records+i*0x100+0x3C,0);f.put<uint8_t>(F::Records+0x50,0);f.put<uint32_t>(F::Records+0x33C,0x10);f.put(F::Records+0x328,std::array<float,3>{-2692.56f,363.f,-918.41f});
  auto v=botw::map_special(f.h);assert(v.valid&&v.entries[3].visible&&v.entries[3].kind==42);botw::publish_map_special(f.h,v,false);
  assert(f.values["botw.map.special.3.kind"]==42&&f.values["botw.map.special.3.tint"]==0xFF0048FF&&!f.values.count("botw.map.special.0.kind"));
  assert(f.texts["botw.map.special.3.icon"].ends_with("#Sd_MapWarpDLC_02^t"));
  assert(v.entries[3].xyz[0]==-2692.56f&&v.entries[3].xyz[2]==-918.41f);++cases;}
 {F f;f.put<uint32_t>(F::Records+0x33C,0);auto v=botw::map_special(f.h);botw::publish_map_special(f.h,v,false);assert(!f.values.count("botw.map.special.3.kind"));++cases;}
 for(int kind:{19,20,44,42,26}){assert(botw::special_art(kind));++cases;}assert(!botw::special_art(25));++cases;
 std::cout<<cases<<" special marker identity/activation/position/race/publication cases passed\n";
}

#endif
