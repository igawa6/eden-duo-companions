// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct AreaFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Player=0xA001000,Name=0xA002001;
 AreaFixture() {
  put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put(Map+0x478,Player);put(Player,M+0x2B87300);put<int32_t>(Player+0x1C,25);put(Player+0x28,std::array<float,3>{100,300,-100});put(Player+0x48,Name);std::array<char,64>name{};std::memcpy(name.data(),"TabantaFrontier",16);put(Name,name);
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=AreaFixture;unsigned cases{};auto check=[&](auto change,bool valid){F f;change(f);auto v=botw::map_area(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(auto at:{F::M+0x2CA63B0,F::Map+0x478})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(auto at:{F::Map,F::Player})check([&](F&f){f.put<uint64_t>(at,0);},false);
 check([](F&f){f.put<int32_t>(F::Player+0x1C,21);},false);check([](F&f){f.put<uint64_t>(F::Player+0x48,0);},false);check([](F&f){f.put<uint64_t>(F::Player+0x48,UINT64_MAX-7);},false);
 check([](F&f){f.put<char>(F::Name,0);},false);check([](F&f){std::array<char,64>b;b.fill('a');f.put(F::Name,b);},false);
 for(char c:{'/','.',char(0xFF),' ','-'})check([&](F&f){f.put<char>(F::Name,c);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Player+0x28,n);},false);
 check([](F&f){f.put<float>(F::Player+0x2C,10001);},false);check([](F&f){f.put<float>(F::Player+0x30,5001);},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(auto at:{F::M+0x2CA63B0,F::Map,F::Map+0x478,F::Player,F::Player+0x1C,F::Player+0x28,F::Player+0x48,F::Name,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 {F f;auto v=botw::map_area(f.h);assert(v.key=="TabantaFrontier");botw::publish_map_area(f.h,v);assert(f.values["botw.ready.area.valid"]==1&&f.values["botw.ready.area"]==botw::actor_key(v.key)&&f.texts["botw.ready.area.key"]==v.key);++cases;botw::publish_map_area(f.h,{});assert(!f.values["botw.ready.area.valid"]&&f.values["botw.ready.area"]==-1&&f.texts["botw.ready.area.key"].empty());++cases;}
 {F f;State s;sample(&s,&f.h);assert(f.values["botw.ready.area.valid"]==1);f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);s.next={};sample(&s,&f.h);assert(!f.values["botw.ready.area.valid"]&&f.texts["botw.ready.area.key"].empty());++cases;}
 {F f;State s;sample(&s,&f.h);f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};s.next={};sample(&s,&f.h);assert(!f.values["botw.ready.area.valid"]&&f.texts["botw.ready.area.key"].empty());++cases;}
 std::cout<<cases<<" native player area bounds/race/context/publication cases passed\n";
}

#endif
