// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct NamesFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Array=0xA001000,Records=0xA010000,Sources=0xA100004,Names=0xA200000;
 NamesFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put<int32_t>(Map+0x458,1103);put<int32_t>(Map+0x45C,1103);put(Map+0x460,Array);put<uint32_t>(Map+0x120,0x7FFF);
  for(unsigned i=0;i<botw::map_name_info.size();++i){const auto& e=botw::map_name_info[i];auto p=Records+i*0x100,s=Sources+i*0x80;put(Array+e.index*8,p);put(p,M+0x2B87200);put<int32_t>(p+0x1C,23);put(p+0x28,e.xyz);put<uint32_t>(p+0x44,i%15);put(p+(e.discovered?0x58:0x50),s);put<uint64_t>(p+(e.discovered?0x50:0x58),0);put(s,M+(e.discovered?0x2BB1E00:0x2BB1D70));put<uint32_t>(s+8,e.show);put(s+0x10,Names+i*64);put<int32_t>(s+0x34,1);std::array<char,64> text{};assert(e.key.size()<text.size());std::memcpy(text.data(),e.key.data(),e.key.size());put(Names+i*64,text);}
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=NamesFixture;unsigned cases{};auto check=[&](auto change,bool valid){F f;change(f);auto v=botw::map_names(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);
 auto slot=F::Array+botw::map_name_info[0].index*8;
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x460,slot,F::Records+0x50,F::Sources+0x10})for(uint64_t p:{0ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Map,F::Records,F::Sources})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(int n:{-1,0,1102,1104,INT32_MAX})check([&](F&f){f.put(F::Map+0x458,n);},false);
 for(int n:{1102,2049})check([&](F&f){f.put(F::Map+0x45C,n);},false);
 for(uint32_t n:{15u,254u,UINT32_MAX})check([&](F&f){f.put(F::Records+0x44,n);},false);
 check([](F&f){f.put<int32_t>(F::Records+0x1C,22);},false);check([](F&f){f.put<uint32_t>(F::Sources+8,0);},false);check([](F&f){f.put<uint64_t>(F::Records+0x58,1);},false);check([](F&f){f.put<char>(F::Names,'X');},false);check([](F&f){f.put<uint32_t>(F::Map+0x120,0x8000);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Records+0x28,n);},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x458,F::Map+0x45C,F::Map+0x460,F::Map+0x120,slot,F::Records,F::Records+0x1C,F::Records+0x28,F::Records+0x44,F::Records+0x50,F::Records+0x58,F::Sources,F::Sources+8,F::Sources+0x10,F::Names,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 unsigned dynamic{};while(!botw::map_name_info[dynamic].discovered)++dynamic;
 check([&](F&f){f.put<int32_t>(F::Sources+dynamic*0x80+0x34,-1);},false);
 {F f;f.put<int32_t>(F::Sources+dynamic*0x80+0x34,0);auto v=botw::map_names(f.h);assert(v.valid&&!v.visible[dynamic]&&v.visible[0]);botw::publish_map_names(f.h,v);assert(f.values["botw.map.names.0"]);
  // Perf round 4: a not-shown name is no longer published at all each
  // tick; clear between calls (same pattern round 3 used for koroks/places/landmarks) so the
  // second check reflects this publish, not a stale value left over from the first.
  f.values.clear();botw::publish_map_names(f.h,{});assert(!f.values["botw.map.names.valid"]&&!f.values.count("botw.map.names.0"));++cases;}
 {F f;f.put<uint32_t>(F::Map+0x120,0);auto v=botw::map_names(f.h);assert(v.valid);for(bool b:v.visible)assert(!b);f.put<uint32_t>(F::Records+0x44,255);v=botw::map_names(f.h);assert(v.valid&&v.visible[0]);++cases;}
 {F f;State s;sample(&s,&f.h);assert(f.values["botw.map.names.valid"]);f.values.clear();f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};s.next={};sample(&s,&f.h);assert(!f.values["botw.map.names.valid"]&&!f.values.count("botw.map.names.0"));++cases;}
 // DLC install (owner bug 2026-10-23): the location array grows 1103 -> 1105 and records after
 // the inserted entries move up; names must still resolve (bounded running shift), and a shift
 // larger than the inserted count must still fail closed.
 auto shifted=[](F& f,unsigned by,int32_t count){f.put<int32_t>(F::Map+0x458,count);f.put<int32_t>(F::Map+0x45C,count);
  for(unsigned i=botw::map_name_info.size();i-->0;){const auto& e=botw::map_name_info[i];if(e.index<500)continue;
   uint64_t p{};for(unsigned b=0;b<8;++b)p|=uint64_t(f.mem.at(F::Array+e.index*8+b))<<(8*b);f.put<uint64_t>(F::Array+e.index*8,0);f.put(F::Array+(e.index+by)*8,p);}};
 {F f;f.put<int32_t>(F::Map+0x458,1105);f.put<int32_t>(F::Map+0x45C,1105);assert(botw::map_names(f.h).valid);++cases;}
 {F f;shifted(f,2,1105);auto v=botw::map_names(f.h);assert(v.valid&&v.visible[0]);++cases;}
 {F f;shifted(f,3,1105);assert(!botw::map_names(f.h).valid);++cases;}
 {F f;shifted(f,2,1103);assert(!botw::map_names(f.h).valid);++cases;}
 std::cout<<cases<<" native map name bounds/identity/discovery/race/publication cases passed\n";
}

#endif
