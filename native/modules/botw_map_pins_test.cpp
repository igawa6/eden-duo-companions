// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct PinFixture:Fixture {
  static constexpr uint64_t Map=0xA000000,Array=0xA001000,Pin=0xA002000,Backing=0xA003000;
  PinFixture() {
    put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put<int32_t>(Map+0x658,5);put<int32_t>(Map+0x65C,5);put(Map+0x660,Array);
    for(unsigned i=0;i<5;++i){auto p=Pin+i*128;put(Array+i*8,p);put(p,M+0x2B866B0);put(p+8,Backing+i*8);put<uint8_t>(Backing+i*8,0);put<int32_t>(p+0x40,i);put<int32_t>(p+0x44,0);put<int32_t>(p+0x1C,37+i);put(p+0x28,std::array<float,3>{float(100*i),300,-float(100*i)});}
    for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}) {
      std::array<uint8_t,56> b{};uint64_t vt=M+0x2A701C8,ptr=M+off+20;int32_t cap=32;
      std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&ptr,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);
    }
  }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=PinFixture;unsigned cases{};
 auto check=[&](auto change,bool valid,int count=-1){F f;change(f);auto v=botw::map_pins(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);if(count>=0)assert(std::count(v.active.begin(),v.active.end(),true)==count);++cases;};
 check([](F&){},true,5);
 check([](F&f){for(unsigned i=0;i<5;++i)f.put<uint64_t>(F::Pin+i*128+8,0);},true,0);
 for(unsigned i=0;i<5;++i)check([&](F&f){f.put<uint64_t>(F::Pin+i*128+8,0);f.put<float>(F::Pin+i*128+0x28,std::numeric_limits<float>::quiet_NaN());f.put<int32_t>(F::Pin+i*128+0x1C,45);},true,4);
 check([](F&f){f.h.build_id[0]^=1;},false);
 check([](F&f){f.h.main_size=0x2CA63B0;},false);
 check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(int n:{-1,0,4,6})for(auto at:{F::Map+0x658,F::Map+0x65C})check([&](F&f){f.put<int32_t>(at,n);},false);
 for(auto at:{F::M+0x2CA63B0,F::Map+0x660,F::Array})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 check([](F&f){f.put<uint64_t>(F::Pin,0);},false);
 check([](F&f){f.put<uint64_t>(F::Map,0);},false);
 check([](F&f){f.put(F::Array+8,F::Pin);},false);
 check([](F&f){f.put(F::Pin+128+8,F::Backing);},false);
 check([](F&f){f.put<uint64_t>(F::Pin+8,0xB000000);},false);
 check([](F&f){f.put<uint64_t>(F::Pin+8,0x10001);},false);
 check([](F&f){f.put<int32_t>(F::Pin+0x40,1);},false);
 for(int n:{-1,36,38,45})check([&](F&f){f.put<int32_t>(F::Pin+0x1C,n);},false);
 check([](F&f){f.put<int32_t>(F::Pin+0x44,14);},true,5);
 for(float n:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),6001.f,-6001.f})check([&](F&f){f.put(F::Pin+0x28,n);},false);
 check([](F&f){f.put<float>(F::Pin+0x2C,10001);},false);
 check([](F&f){f.put<float>(F::Pin+0x30,5001);},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(auto at:{F::M+0x2CA63B0,F::Map,F::Map+0x658,F::Map+0x660,F::Array,F::Pin,F::Pin+8,F::Pin+0x40,F::Pin+0x1C,F::Pin+0x28,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 {F f;auto v=botw::map_pins(f.h);for(unsigned i=0;i<5;++i)assert(v.active[i]&&v.positions[i][0]==float(100*i)&&v.positions[i][2]==-float(100*i));++cases;}
 {F f;auto v=botw::map_pins(f.h);botw::publish_map_pins(f.h,v);assert(f.values["botw.map.pins.valid"]==1);for(unsigned i=0;i<5;++i){auto key="botw.map.pins."+std::to_string(i);assert(f.values[key+".kind"]==i);assert(f.texts[key+".core"].find("FL_00^t")!=std::string::npos);assert(f.texts[key+".inner"].find("Icon"+std::to_string(i+1))!=std::string::npos);}++cases;
  // Perf round 4: an inactive slot is no longer published at all; clear
  // between calls (same pattern round 3 used) so the invalid-pass check reflects this publish,
  // not stale per-slot state left over from the active publish above.
  f.values.clear();f.texts.clear();v.valid=false;botw::publish_map_pins(f.h,v);assert(!f.values["botw.map.pins.valid"]);for(unsigned i=0;i<5;++i){auto key="botw.map.pins."+std::to_string(i);assert(!f.values.count(key+".kind")&&!f.texts.count(key+".core")&&!f.texts.count(key+".inner")&&!f.texts.count(key+".halo"));}++cases;
 }
 {F f;State state;sample(&state,&f.h);assert(f.values["botw.map.pins.valid"]==1);f.values.clear();f.texts.clear();f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);state.next={};sample(&state,&f.h);assert(f.values["botw.map.pins.valid"]==0&&!f.values.count("botw.map.pins.4.kind")&&!f.texts.count("botw.map.pins.4.core"));++cases;}
 {F f;State state;sample(&state,&f.h);f.values.clear();f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};state.next={};sample(&state,&f.h);assert(!f.values["botw.map.pins.valid"]&&!f.values.count("botw.map.pins.0.kind"));++cases;}
 std::cout<<cases<<" native colored pin bounds/presence/race/context cases passed\n";
}

#endif
