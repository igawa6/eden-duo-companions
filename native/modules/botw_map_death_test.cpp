// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_map_death.h"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct DeathFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Array=0xA001000,Record=0xA002000,Manager=0xA003000;
 DeathFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put<int32_t>(Map+0x468,1);put<int32_t>(Map+0x46C,1);put(Map+0x470,Array);put(Array,Record);put(Record,M+0x2B873F8);put<int32_t>(Record+0x1C,24);put<int32_t>(Record+0x40,0);put<uint32_t>(Record+0x3C,0x10);put(Record+0x28,std::array<float,3>{-1250,300,1760});put(M+0x2CA3E18,Manager);put(Manager,M+0x2B7D758);put<int32_t>(Manager+0x34,0);put<int32_t>(Manager+0x38,1);put(Manager+0x28,std::array<float,3>{-1250,300,1760});
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
 void absent(){put<int32_t>(Manager+0x34,-1);put<int32_t>(Manager+0x38,0);put<uint32_t>(Record+0x3C,0);}
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=DeathFixture;unsigned cases{};auto check=[&](auto change,bool expected){F f;change(f);auto v=botw::map_death(f.h);if(v.valid!=expected)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==expected);++cases;};
 check([](F&){},true);check([](F&f){f.absent();},true);
 check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x470,F::Array,F::M+0x2CA3E18})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Map,F::Record,F::Manager})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(uint64_t at:{F::Map+0x468,F::Map+0x46C})for(int32_t n:{-1,0,2,INT32_MAX})check([&](F&f){f.put(at,n);},false);
 for(int32_t n:{-1,23,25,INT32_MAX})check([&](F&f){f.put(F::Record+0x1C,n);},false);
 for(int32_t n:{-1,1,INT32_MAX})check([&](F&f){f.put(F::Record+0x40,n);},false);
 for(int32_t n:{-1,2,INT32_MAX})check([&](F&f){f.put(F::Manager+0x38,n);},false);
 for(int32_t n:{-1,1,INT32_MAX})check([&](F&f){f.put(F::Manager+0x34,n);},false);
 check([](F&f){f.absent();f.put<int32_t>(F::Manager+0x34,0);},false);
 check([](F&f){f.put<uint32_t>(F::Record+0x3C,0);},false);check([](F&f){f.absent();f.put<uint32_t>(F::Record+0x3C,0x10);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Manager+0x28,n);f.put(F::Record+0x28,n);},false);
 check([](F&f){f.put(F::Manager+0x2C,10001.f);f.put(F::Record+0x2C,10001.f);},false);check([](F&f){f.put(F::Manager+0x30,5001.f);f.put(F::Record+0x30,5001.f);},false);
 check([](F&f){f.put(F::Record+0x28,2.f);},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x468,F::Map+0x46C,F::Map+0x470,F::Array,F::Record,F::Record+0x1C,F::Record+0x40,F::Record+0x28,F::M+0x2CA3E18,F::Manager,F::Manager+0x34,F::Manager+0x38,F::Manager+0x28,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 check([](F&f){f.put<uint32_t>(F::Record+0x3C,0xFFFFFFFF);},true);
 check([](F&f){f.absent();f.put(F::Manager+0x28,std::numeric_limits<float>::quiet_NaN());},true);
 {F f;f.h.read_memory=[](void* p,uint64_t a,void* b,size_t n)->EdenDsmodBool{auto& f=*static_cast<F*>(p);for(size_t i=0;i<n;++i)static_cast<uint8_t*>(b)[i]=f.mem.at(a+i);if(a==F::Record+0x3C&&++f.reads==2)static_cast<uint8_t*>(b)[0]^=0x10;return true;};assert(!botw::map_death(f.h).valid);++cases;}
 {F f;auto v=botw::map_death(f.h);assert(v.valid&&v.visible&&v.xyz[0]==-1250);botw::publish_map_death(f.h,v);assert(f.values["botw.map.death.count"]==1&&f.values["botw.map.death.0.kind"]==24&&f.values["botw.map.death.tint"]==0xFFC80000);++cases;botw::publish_map_death(f.h,{});assert(!f.values["botw.map.death.count"]&&f.values["botw.map.death.0.kind"]==-1&&f.texts["botw.map.death.icon"].empty());++cases;}
 {F f;f.absent();auto v=botw::map_death(f.h);assert(v.valid&&!v.visible);++cases;}
 std::cout<<cases<<" native death identity/source/visibility/race/publication cases passed\n";
}

#endif
