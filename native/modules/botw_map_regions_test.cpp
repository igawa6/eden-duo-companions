// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
// Literal bytes captured live from runs/native-Location.json (exact-1.6.0 dump of the real
// LocationMarker/region-source strings the game itself stores), NOT derived from
// botw::region_names -- this is an independent cross-check that the constant in
// botw_map_regions.h actually matches what the native game writes, including the real trailing
// space on "MapRegion_HyrulePrairie " (index 5; confirmed byte-for-byte against the capture,
// iteration 7 task 3). Same order as botw::region_names (alphabetical in both the header and the
// capture): Eldin, Firone, Gerudo, Hateru, Hebura, HyrulePrairie, Lanayru, Tamul.
inline constexpr std::array<const char*,8> kCapturedRegionNames{
 "MapRegion_Eldin","MapRegion_Firone","MapRegion_Gerudo","MapRegion_Hateru","MapRegion_Hebura",
 "MapRegion_HyrulePrairie ","MapRegion_Lanayru","MapRegion_Tamul"};
struct RegionsFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Array=0xA001000,Records=0xA010000,Sources=0xA100004,Names=0xA200000;
 RegionsFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put<int32_t>(Map+0x458,1103);put<int32_t>(Map+0x45C,1103);put(Map+0x460,Array);put<uint32_t>(Map+0x120,0x7FFF);
  for(unsigned i=0;i<8;++i){auto p=Records+i*0x100,s=Sources+i*0x40;put(Array+(477+i)*8,p);put(p,M+0x2B87200);put<int32_t>(p+0x1C,23);put(p+0x28,std::array<float,3>{float(i),0,-float(i)});put<uint32_t>(p+0x44,i);put(p+0x50,s);put<uint64_t>(p+0x58,0);put(s,M+0x2BB1D70);put<uint32_t>(s+8,1);put(s+0x10,Names+i*32);std::array<char,32> name{};std::memcpy(name.data(),kCapturedRegionNames[i],std::strlen(kCapturedRegionNames[i]));put(Names+i*32,name);}
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=RegionsFixture;unsigned cases{};auto check=[&](auto change,bool valid){F f;change(f);auto v=botw::map_regions(f.h);assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x460,F::Array+477*8,F::Records+0x50,F::Sources+0x10})for(uint64_t p:{0ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Map,F::Records,F::Sources})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(int n:{-1,0,1102,1104,INT32_MAX})check([&](F&f){f.put(F::Map+0x458,n);},false);
 for(int n:{1102,2049})check([&](F&f){f.put(F::Map+0x45C,n);},false);
 for(uint32_t n:{15u,255u,UINT32_MAX})check([&](F&f){f.put(F::Records+0x44,n);},false);
 check([](F&f){f.put<int32_t>(F::Records+0x1C,22);},false);check([](F&f){f.put<uint32_t>(F::Sources+8,2);},false);check([](F&f){f.put<uint64_t>(F::Records+0x58,1);},false);check([](F&f){f.put<char>(F::Names,'X');},false);check([](F&f){f.put<char>(F::Names+std::strlen(botw::region_names[0]),'X');},false);check([](F&f){f.put<uint32_t>(F::Map+0x120,0x8000);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Records+0x28,n);},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x458,F::Map+0x45C,F::Map+0x460,F::Map+0x120,F::Array+477*8,F::Records,F::Records+0x1C,F::Records+0x28,F::Records+0x44,F::Records+0x50,F::Records+0x58,F::Sources,F::Sources+8,F::Sources+0x10,F::Names,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 {F f;f.put<uint32_t>(F::Map+0x120,1u<<3);auto v=botw::map_regions(f.h);assert(v.valid);for(unsigned i=0;i<8;++i)assert(v.visible[i]==(i==3));botw::publish_map_regions(f.h,v);assert(f.values["botw.map.regions.3"]&&!f.values["botw.map.regions.0"]);botw::publish_map_regions(f.h,{});assert(!f.values["botw.map.regions.valid"]&&!f.values["botw.map.regions.3"]);++cases;}
 {F f;State s;sample(&s,&f.h);assert(f.values["botw.map.regions.valid"]);f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);s.next={};sample(&s,&f.h);assert(!f.values["botw.map.regions.valid"]&&!f.values["botw.map.regions.0"]);++cases;}
 // DLC install (owner bug 2026-10-23): 1105 location entries is valid; region slots 477..484 are unchanged.
 {F f;f.put<int32_t>(F::Map+0x458,1105);f.put<int32_t>(F::Map+0x45C,1105);assert(botw::map_regions(f.h).valid);++cases;}
 check([](F&f){f.put<int32_t>(F::Map+0x458,1104);f.put<int32_t>(F::Map+0x45C,1104);},false);
 std::cout<<cases<<" native map region identity/bounds/discovery/race/publication cases passed\n";
}

#endif
