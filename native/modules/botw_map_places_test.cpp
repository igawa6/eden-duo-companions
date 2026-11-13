// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct PlacesFixture:Fixture {
 static constexpr uint64_t Map=0xA000000,Array=0xA001000,Records=0xA010000,Sources=0xA100000;
 PlacesFixture(){put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put<int32_t>(Map+0x2E0,214);put<int32_t>(Map+0x2E4,214);put(Map+0x2E8,Array);
  for(unsigned i=0;i<214;++i){int kind=i%17;auto p=Records+i*0x100,source=Sources+i*0x40;put(Array+i*8,p);put(p,M+botw::place_vtable(h,kind));put<int32_t>(p+0x1C,kind);put(p+0x28,std::array<float,3>{float(i),300,-float(i)});put(p+0x58,source);put<int32_t>(source+0x1C,1);put<int32_t>(p+0x60,1);put<uint32_t>(p+0x3C,0x20000);}
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=PlacesFixture;unsigned cases{};auto check=[&](auto change,bool valid){F f;change(f);auto v=botw::map_places(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<'\n';assert(v.valid==valid);++cases;};
 check([](F&){},true);check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map+0x2E8,F::Array,F::Records+0x58})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(int32_t n:{-1,0,213,257,INT32_MAX})check([&](F&f){f.put(F::Map+0x2E0,n);},false);
 check([](F&f){f.put<int32_t>(F::Map+0x2E4,257);},false);check([](F&f){f.put<int32_t>(F::Map+0x2E4,213);},false);
 for(uint64_t at:{F::Map,F::Records})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(int32_t kind:{-1,17,INT32_MAX})check([&](F&f){f.put(F::Records+0x1C,kind);},false);
 check([](F&f){f.put(F::Array+8,F::Records);},false);check([](F&f){f.put(F::Records+0x100+0x58,F::Sources);},false);
 check([](F&f){f.put<int32_t>(F::Sources+0x1C,-1);},false);check([](F&f){f.put<int32_t>(F::Records+0x60,2);},false);
 for(float n:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),6001.f,-6001.f})check([&](F&f){f.put(F::Records+0x28,n);},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 for(uint64_t at:{F::M+0x2CA63B0,F::Map,F::Map+0x2E0,F::Map+0x2E8,F::Array,F::Records,F::Records+0x1C,F::Records+0x28,F::Records+0x58,F::Sources+0x1C,F::Records+0x60,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 {F f;auto v=botw::map_places(f.h);assert(v.valid&&v.count==214&&v.entries[0].visible&&!v.entries[2].visible&&!v.entries[7].visible);botw::publish_map_places(f.h,v);auto count=f.values["botw.map.places.count"];assert(count>100&&count<214);
  // Critique item C (fix round 2): kind 1 (Nt_MapHatago = stable) gets its own .kind_stable
  // layer, separate from every other place kind (.kind_place) -- index 0 is kind 0 (city),
  // index 1 is kind 1 (stable, i%17==1).
  assert(f.values["botw.map.places.0.kind_stable"]==-1&&f.values["botw.map.places.0.kind_place"]==0);
  assert(f.values["botw.map.places.1.kind_stable"]==1&&f.values["botw.map.places.1.kind_place"]==-1);
  // Item 17 round b: kind 11 (Nt_YadoIcon = inn, a shop kind 11-15) splits out of .kind_place
  // into its own .kind_shop (index 11 is kind 11, i%17==11), and does not count as a stable.
  assert(f.values["botw.map.places.11.kind_shop"]==11&&f.values["botw.map.places.11.kind_place"]==-1&&f.values["botw.map.places.11.kind_stable"]==-1);
  assert(f.values["botw.map.places.0.kind_shop"]==-1&&f.values["botw.map.places.1.kind_shop"]==-1);
  assert(f.texts["botw.map.places.backing_icon"]==std::string("file:assets/map-place-backing.png"));
  ++cases;
  // Iteration 12 item 6: every place names its per-kind cropped composite (uniform marker size).
  assert(f.texts["botw.map.places.0.icon"]=="composite:botw-place-0"&&f.texts["botw.map.places.1.icon"]=="composite:botw-place-1"&&
         f.texts["botw.map.places.11.icon"]=="composite:botw-place-11");
  for(int k:{0,1,4,5,6,11,12,13,14,15,16})assert(botw::place_art(k)&&botw::place_icon_src(k)=="composite:botw-place-"+std::to_string(k));
  ++cases;
  // Perf round 3b: publish_map_places no longer self-clears a slot's keys once it goes
  // inactive (relies on the runtime's own per-tick snapshot clear, see botw_map_places.h);
  // simulate that clear explicitly, same pattern botw_tracker_view_test.cpp already uses.
  f.values.clear();f.texts.clear();
  botw::publish_map_places(f.h,{});assert(!f.values["botw.map.places.valid"]&&!f.values["botw.map.places.count"]&&!f.values.count("botw.map.places.0.kind")&&!f.values.count("botw.map.places.0.kind_stable")&&!f.values.count("botw.map.places.0.kind_place")&&!f.values.count("botw.map.places.0.kind_shop"));++cases;}
 {F f;for(unsigned i=0;i<214;++i){f.put<int32_t>(F::Sources+i*0x40+0x1C,0);f.put<int32_t>(F::Records+i*0x100+0x60,0);}auto v=botw::map_places(f.h);assert(v.valid);botw::publish_map_places(f.h,v);assert(!f.values["botw.map.places.count"]);++cases;}
 // Item 15 (owner 2026-10-20): the field->shrine load passes through this exact Transition
 // signature. places/landmarks/names now survive it (cache preserved) instead of being wiped,
 // so a shrine interior keeps showing the overworld map icons it had just before entering.
 {F f;State s;sample(&s,&f.h);assert(f.values["botw.map.places.valid"]);auto kind0=f.values["botw.map.places.0.kind"];f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);s.next={};sample(&s,&f.h);assert(f.values["botw.map.places.valid"]&&f.values["botw.map.places.0.kind"]==kind0);++cases;}
 {F f;State s;sample(&s,&f.h);f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};s.next={};f.values.clear();f.texts.clear();sample(&s,&f.h);assert(!f.values["botw.map.places.valid"]&&!f.values.count("botw.map.places.0.kind"));++cases;}
 // DLC install (owner bug 2026-10-23): kind-43 entries (EX treasure locations) are a known record
 // type -- the list stays valid and they are never drawn; an unknown kind still fails closed.
 {F f;f.put(F::Records,F::M+botw::place_vtable(f.h,43));f.put<int32_t>(F::Records+0x1C,43);auto v=botw::map_places(f.h);assert(v.valid&&!v.entries[0].visible);++cases;}
 check([](F&f){f.put<int32_t>(F::Records+0x1C,44);},false);
 std::cout<<cases<<" native place bounds/race/discovery/context/publication cases passed\n";
}

#endif
