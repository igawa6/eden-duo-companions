// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <iostream>
#include <cassert>
struct MapFixture:Fixture {
 std::map<std::string,double> floats;
 static constexpr uint64_t Data=0x8000000,Nodes=0x9000000,Player=0xA000000,Actor=0xA010000,G=0xB000000,Holder=0xB010000,Bank=0xB020000,Array=0xB030000,Flags=0xB040000;
 MapFixture() {
  h.publish_f64=[](void* p,const char* k,double v){static_cast<MapFixture*>(p)->floats[k]=v;};
  put(M+0x2CA63B0,Data);put(Data,M+0x2B86848);put<uint32_t>(Data+0x120,0x7FFF);
  for(unsigned k=0;k<2;++k){const auto head=U+(k?0x63E78:0x627E8),base=Nodes+k*0x10000;
   put(head,base+119*24+8);put(head+8,base+8);put<uint64_t>(head+16,120);
   for(unsigned i=0;i<120;++i){const auto p=base+i*24;for(unsigned b=0;b<24;++b)put<uint8_t>(p+b,0);put<uint16_t>(p,1);put<uint16_t>(p+2,k?3:0);put<uint64_t>(p+8,i?p-16:head);put<uint64_t>(p+16,i==119?head:p+32);}
  }
  for(const uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){for(unsigned i=0;i<56;++i)put<uint8_t>(M+off+i,0);put(M+off,M+0x2A701C8);put(M+off+8,M+off+20);put<int32_t>(M+off+16,32);for(unsigned i=0;i<10;++i)put<char>(M+off+20+i,"MainField\0"[i]);}
  put(M+0x2CA1140,Player);put(Player,M+0x2B7BE80);put(Player+0x60,Actor);put(Actor,M+0x2B7BEA8);put(Player+0x88,std::array<float,3>{-2988,374,-1941});
  put(M+0x2CC5FE8,G);put(G,M+0x2BD07E0);put(G+0xBE0,Holder);put(Holder,Bank);put<int32_t>(Bank+8,6);put<int32_t>(Bank+12,6);put(Bank+16,Array);
  std::array<uint32_t,6> hashes={0x25718F36,0xBBDE4235,0x22D7138F,0x55D02319,0xCBB4B6BA,0xF44A4DAA};std::sort(hashes.begin(),hashes.end());
  for(unsigned i=0;i<6;++i){auto p=Flags+i*16;put(Array+i*8,p);put(p,M+0x2BD1078);put<uint32_t>(p+8,0);put<uint32_t>(p+12,hashes[i]);}
 }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=MapFixture;unsigned cases{};
 auto check=[&](auto mutate,bool valid){F f;mutate(f);const auto s=botw::map_snapshot(f.h);if(s.valid!=valid)std::cerr<<"Map case "<<cases+1<<"\n";assert(s.valid==valid);++cases;};
 {F f;const auto s=botw::map_snapshot(f.h);assert(s.valid&&s.main_field&&s.revealed==32767&&s.submasks[0]==3&&s.subfilters[0]==1);const auto p=botw::map_position(f.h);assert(p.valid&&p.x==-2988&&p.y==1941);++cases;}
 for(const auto value:{0u,1u,0x7FFFu})check([&](F&f){f.put<uint32_t>(F::Data+0x120,value);},true);
 check([](F&f){f.put<uint32_t>(F::Data+0x120,0x8000);},false);
 for(auto off:{0x627E8u,0x63E78u}){
  check([&](F&f){f.put<uint64_t>(F::U+off+16,119);},false);
  check([&](F&f){f.put<uint64_t>(F::U+off+8,F::U+off);},false);
  check([&](F&f){f.put<uint64_t>(F::U+off,0);},false);
 }
 for(auto node:{F::Nodes,F::Nodes+0x10000}){
  check([&](F&f){f.put<uint64_t>(node+16,node+8);},false);
  check([&](F&f){f.put<uint64_t>(node+8,0);},false);
  check([&](F&f){f.mem.erase(node);},false);
  check([&](F&f){f.race=node;},false);
 }
 check([](F&f){f.put<uint16_t>(F::Nodes,0x8000);},false);
 check([](F&f){f.put<uint16_t>(F::Nodes+0x10000+2,64);},false);
 for(auto at:{F::M+0x2CA63B0,F::Data,F::Data+0x120,F::U+0x627E8,F::Flags,F::U+0x64C30})check([&](F&f){f.race=at;},false);
 check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=0;},false);
 for(auto flag:{0x1849000000ULL,0x0E09E00000ULL,0x1E09000001ULL})check([&](F&f){f.put<uint64_t>(F::U+0x64C30,flag);},false);
 {F f;f.put<char>(F::M+0x2C9E87C,'C');assert(botw::map_snapshot(f.h).valid&&!botw::map_snapshot(f.h).main_field&&!botw::map_position(f.h).valid);++cases;}
 for(auto pos:{std::array<float,3>{NAN,0,0},std::array<float,3>{0,0,INFINITY},std::array<float,3>{50001,0,0}}){F f;f.put(F::Player+0x88,pos);assert(!botw::map_position(f.h).valid);++cases;}
 for(auto at:{F::M+0x2CA1140,F::Player+0x88}){F f;f.race=at;assert(!botw::map_position(f.h).valid);++cases;}
 {F f;auto s=botw::map_snapshot(f.h);s.masks[0]=3;s.subfilters[0]=2;s.submasks[0]=3;s.changes[0]=s.changes[1]=true;
  s.revealed=0;assert(botw::map_tile(s,0,3)=="romfs:/UI/MapTex/MainField/MapTex3_Z-0_0.sbmaptex");
  s.revealed=1;assert(botw::map_tile(s,0,3).ends_with("_1.sbmaptex"));s.revealed=2;assert(botw::map_tile(s,0,3).ends_with("_2-3.sbmaptex"));s.revealed=3;assert(botw::map_tile(s,0,3).ends_with("Z-0-3.sbmaptex"));assert(botw::map_tile(s,120,3).empty()&&botw::map_tile(s,0,4).empty());s.valid=false;assert(botw::map_tile(s,0,3).empty());++cases;}
 const std::array<float,12> south={1,0,0,-2988,0,1,0,374,0,0,1,-1941};
 for(unsigned direction=0;direction<4;++direction){F f;auto m=south;const float angles[]={0,90,180,-90};const float a=angles[direction]*3.14159265358979323846f/180.f;m[2]=std::sin(a);m[10]=-std::cos(a);f.put(F::Actor+0x398,m);const auto p=botw::map_position(f.h);assert(p.valid&&p.heading_valid&&std::abs(std::remainder(p.heading-angles[direction],360.f))<.001f);++cases;}
 for(unsigned bad=0;bad<6;++bad){F f;auto m=south;if(bad==0)m[2]=NAN;if(bad==1)m[10]=INFINITY;if(bad==2)m[10]=0;if(bad==3)m[10]=2;if(bad==4)m[3]+=3;f.put(F::Actor+0x398,m);if(bad==5)f.race=F::Actor+0x398;const auto p=botw::map_position(f.h);assert(p.valid&&!p.heading_valid);++cases;}
 {F f;f.h.publish_f64=[](void* p,const char* k,double v){static_cast<F*>(p)->floats[k]=v;};f.put(F::Actor+0x398,south);State state;sample(&state,&f.h);assert(f.values["botw.map.valid"]==1&&f.values["botw.map.heading.valid"]==1&&f.floats.contains("botw.map.heading")&&f.floats["botw.map.x"]==-2988.75 /*Perf round 2: quantized to the 3.75-unit (0.25 Ready-px) grid, botw::quantize_display*/);f.floats.clear();f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);state.next={};state.map_next={};sample(&state,&f.h);assert(!f.values["botw.map.valid"]&&!f.values["botw.map.heading.valid"]&&!f.values["botw.map.position.valid"]&&!f.floats.contains("botw.map.x")&&!f.floats.contains("botw.map.heading"));++cases;}
 // A popup/unknown UI frame must not leave recovery behind the old 500 ms deadline.
 {F f;State state;sample(&state,&f.h);assert(f.values["botw.map.minimap.valid"]==1);
  state.map_next=std::chrono::steady_clock::now()+std::chrono::hours(1);
  f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);state.next={};sample(&state,&f.h);
  assert(!f.values["botw.map.valid"] && !f.values["botw.map.position.valid"]);
  f.put<uint64_t>(F::U+0x64C30,0x1E09000000ULL);state.next={};sample(&state,&f.h);
  assert(f.values["botw.map.minimap.valid"]==1);++cases;}
 // Native menu release refreshes discovery data immediately, even before periodic expiry.
 {F f;State state;sample(&state,&f.h);f.put<uint64_t>(F::U+0x64C30,0x1E09000004ULL);
  state.next={};sample(&state,&f.h);assert(state.context==botw::UiContext::Menu);
  state.map_next=std::chrono::steady_clock::now()+std::chrono::hours(1);
  f.put<uint32_t>(F::Data+0x120,1);f.put<uint64_t>(F::U+0x64C30,0x1E09000000ULL);
  state.next={};sample(&state,&f.h);assert(f.values["botw.map.revealed"]==1 && f.values["botw.map.minimap.valid"]==1);++cases;}
 // A failed read retries on the 100 ms cadence, not the successful-cache 500 ms cadence.
 {F f;State state;sample(&state,&f.h);f.put<uint32_t>(F::Data+0x120,0x8000);state.map_next={};sample(&state,&f.h);
  assert(!f.values["botw.map.valid"]);f.put<uint32_t>(F::Data+0x120,0x7FFF);
  const auto overlay_deadline=state.map_next;
  state.map_retry_next-=std::chrono::milliseconds(101);sample(&state,&f.h);
  assert(f.values["botw.map.minimap.valid"]==1 && state.map_next==overlay_deadline);++cases;}
 // Retrying does not mask persistent invalid memory or turn a real unavailable map into a cache hit.
 {F f;State state;sample(&state,&f.h);f.put<uint32_t>(F::Data+0x120,0x8000);state.map_next={};sample(&state,&f.h);
  state.map_retry_next-=std::chrono::milliseconds(101);sample(&state,&f.h);
  assert(!f.values["botw.map.valid"] && !f.values["botw.map.minimap.valid"] && f.texts["botw.map.tile.1.0"].empty());++cases;}
 // Integration 2026-10-18: the current map type string withholds field data inside shrines.
 {auto map_type=[](F& f,const char* name){const uint64_t off=0x2BE1708;for(unsigned i=0;i<56;++i)f.put<uint8_t>(F::M+off+i,0);
    f.put(F::M+off,F::M+0x2A701C8);f.put(F::M+off+8,F::M+off+20);f.put<int32_t>(F::M+off+16,32);
    for(unsigned i=0;name[i];++i)f.put<char>(F::M+off+20+i,name[i]);};
  {F f;assert(botw::main_field_stage(f.h));++cases;}                     // string absent: unchanged
  {F f;map_type(f,"MainField");assert(botw::main_field_stage(f.h));assert(botw::map_position(f.h).valid);++cases;}
  {F f;map_type(f,"CDungeon");assert(!botw::main_field_stage(f.h));assert(!botw::map_snapshot(f.h).main_field);assert(!botw::map_position(f.h).valid);++cases;}
  {F f;map_type(f,"TitleMenu");assert(!botw::main_field_stage(f.h));++cases;}}
 std::cout<<cases<<" map cases passed\n";
}

#endif
