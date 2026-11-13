// SPDX-License-Identifier: GPL-3.0-or-later
// Native HUD minimap turn (N_MapSR_00) and camera cone (P_Light_00) reader: chain, names, vtables,
// bounds, races, context gating, cache re-validation, degree conversion and publication.
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct ViewFixture:Fixture {
  static constexpr uint64_t Screen=0xC000000,Sr=0xC010000,Root=0xC020000;
  // Root -> N_InOut_00 -> N_Cut_01 -> N_MapIcon_00 -> Pa_MapIcon_00 -> N_MapIconPlayer_00 ->
  // Pa_MapIconPlayer_00 -> N_All_00 -> {N_Btn_00, P_Light_00}
  static constexpr std::array<uint64_t,9> Tree={0xC020000,0xC021000,0xC022000,0xC023000,0xC024000,0xC025000,0xC026000,0xC027000,0xC028000};
  static constexpr uint64_t Sibling=0xC029000;
  static constexpr uint64_t Cone=0xC028000;
  void pane_at(uint64_t p,uint64_t vt,const char* name,uint64_t parent,float rz) {
    std::array<uint8_t,0xD0> b{};std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+0x18,&parent,8);
    const uint64_t head=p+0x20;std::memcpy(b.data()+0x20,&head,8);std::memcpy(b.data()+0x28,&head,8); // empty
    std::memcpy(b.data()+0x44,&rz,4);std::memcpy(b.data()+0xB8,name,std::strlen(name)+1);put(p,b);
  }
  void link(uint64_t parent,std::initializer_list<uint64_t> kids) {
    // head at parent+0x20: prev=last, next=first; child link at child+8: prev, next.
    std::vector<uint64_t> v(kids);const uint64_t head=parent+0x20;
    put(head,v.back()+8);put(head+8,v.front()+8);
    for(size_t i=0;i<v.size();++i){put(v[i]+8,i?v[i-1]+8:head);put(v[i]+16,i+1<v.size()?v[i+1]+8:head);}
  }
  ViewFixture(float map_rz=169.6f,float cone_rz=180.f) {
    put(A+42*8,Screen);put(Screen,M+0x2B93A50);put(Screen+0x5410,Sr);put(Screen+0x3E30,Root);
    pane_at(Sr,M+0x2C22488,"N_MapSR_00",0x1234500,map_rz);
    const std::array<const char*,9> names={"RootPane","N_InOut_00","N_Cut_01","N_MapIcon_00","Pa_MapIcon_00","N_MapIconPlayer_00","Pa_MapIconPlayer_00","N_All_00","P_Light_00"};
    for(size_t i=0;i<9;++i)pane_at(Tree[i],M+(i==0?0x2BBD968:i==8?0x2BBD870:0x2C22488),names[i],i?Tree[i-1]:0,i==8?cone_rz:0.f);
    pane_at(Sibling,M+0x2BBD870,"N_Btn_00",Tree[7],0);
    for(size_t i=0;i+2<9;++i)link(Tree[i],{Tree[i+1]});
    link(Tree[7],{Sibling,Cone});
    for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
  }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=ViewFixture;unsigned cases{};
 auto check=[&](auto change,bool valid,bool cone=true){F f;change(f);auto v=botw::map_view(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases<<"\n";assert(v.valid==valid);if(valid)assert(v.cone_valid==cone);++cases;};
 {F f;auto v=botw::map_view(f.h);assert(v.valid&&v.cone_valid&&v.cone_pane==F::Cone);assert(std::abs(v.map_rz-169.6f)<1e-4f&&v.cone_rz==180.f);++cases;}
 // Build, layout and context gates.
 check([](F&f){f.h.build_id[0]^=1;},false);check([](F&f){f.h.main_size=1;},false);check([](F&f){f.h.main_base=UINT64_MAX-10;},false);
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL})check([&](F&f){f.put<char>(F::M+off+20,'X');},false);
 // Bad pointers, vtables and names on the map chain.
 for(uint64_t at:{F::A+42*8,F::Screen+0x5410,F::Screen+0x3E30})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(uint64_t at:{F::Screen,F::Sr,F::Root})check([&](F&f){f.put<uint64_t>(at,f.M+0x2B00000);},false);
 check([](F&f){f.put<char>(F::Sr+0xB8+2,'X');},false);check([](F&f){f.put<char>(F::Root+0xB8,'X');},false);
 check([](F&f){f.put<char>(F::Sr+0xB8+10,'0');},false); // "N_MapSR_000": a longer name is not the pane
 // Nonfinite, huge or tilted turns.
 for(float rz:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),2e5f})check([&](F&f){f.put(F::Sr+0x44,rz);},false);
 check([](F&f){f.put(F::Sr+0x3C,1.f);},false);
 // Every read raced once: the double read refuses.
 for(uint64_t at:{F::A+42*8,F::Screen+0x5410,F::Sr,F::U+0x64C30,F::M+0x2C9E868})check([&](F&f){f.race=at;},false);
 // The cone is optional: a broken cone path keeps the map turn.
 check([](F&f){f.put<char>(F::Cone+0xB8,'X');},true,false);
 check([](F&f){f.put<uint64_t>(F::Cone,f.M+0x2C22488);},true,false);       // not a picture pane
 check([](F&f){f.put<char>(F::Tree[4]+0xB8+3,'Z');},true,false);           // wrong ancestor
 check([](F&f){f.put(F::Cone+0x44,std::numeric_limits<float>::quiet_NaN());},true,false);
 check([](F&f){f.put<uint64_t>(F::Tree[7]+0x28,F::Tree[7]+0x20);},true,false); // empty child list
 check([](F&f){f.put<uint64_t>(F::Sibling+0x10,F::Sibling+8);},true,false);   // a cycle stops at 64
 check([](F&f){f.race=F::Cone;},true,false);
 // Cache: a valid cached pane is reused; a stale one (other parent chain) is re-resolved.
 {F f;auto v=botw::map_view(f.h,F::Cone);assert(v.cone_valid&&v.cone_pane==F::Cone);v=botw::map_view(f.h,F::Sibling);assert(v.cone_valid&&v.cone_pane==F::Cone);v=botw::map_view(f.h,0x10);assert(v.cone_valid&&v.cone_pane==F::Cone);cases+=3;}
 // Degrees: clockwise companion values, whole degrees in [0, 360).
 for(auto [rz,deg]:{std::pair{0.f,0.},std::pair{169.6f,190.5},std::pair{45.73f,314.5},std::pair{-90.f,90.},std::pair{720.f,0.},std::pair{359.6f,0.5},std::pair{0.4f,359.5},std::pair{0.1f,0.},std::pair{0.6f,359.5}}){F f(rz);auto v=botw::map_view(f.h);assert(botw::view_rotation(v)==deg);++cases;}
 {F f(169.62f);auto v=botw::map_view(f.h);assert(botw::view_heading(v,-69.62f)==121.);++cases;} // native arrow 239.23 ccw = 120.77 cw
 for(auto [rz,deg]:{std::pair{180.f,180.},std::pair{158.35f,201.5},std::pair{141.26f,218.5},std::pair{0.f,0.},std::pair{-70.f,70.}}){F f(0.f,rz);auto v=botw::map_view(f.h);assert(botw::view_cone(v)==deg);++cases;}
 assert(botw::turn_degrees(std::numeric_limits<double>::quiet_NaN())==0.&&botw::turn_degrees(-1e12)==0.&&botw::turn_degrees(359.8)==0.&&botw::turn_degrees(-0.3)==359.5);assert(!std::signbit(botw::turn_degrees(-0.))&&!std::signbit(botw::turn_degrees(-0.1)));cases+=2;
 assert(botw::whole_degrees(std::numeric_limits<double>::quiet_NaN())==0&&botw::whole_degrees(-1e12)>=0);cases+=1;
 assert(botw::view_rotation(botw::MapView{})==0&&botw::view_cone(botw::MapView{})==0);++cases;
 // Publication: gated by the minimap, zeros when invalid.
 {F f;auto v=botw::map_view(f.h);botw::MapPosition p{true,0,0,true,-69.62f};botw::publish_map_view(f.h,v,true,p);
  assert(f.values["botw.map.view.valid"]==1&&f.values["botw.map.view.rotation"]==191&&f.values["botw.map.view.heading.valid"]==1&&f.values["botw.map.view.cone.valid"]==1&&f.values["botw.map.view.cone"]==180);
  botw::publish_map_view(f.h,v,false,p);assert(f.values["botw.map.view.valid"]==0&&f.values["botw.map.view.rotation"]==0&&f.values["botw.map.view.heading.valid"]==0&&f.values["botw.map.view.cone.valid"]==0);
  p.heading_valid=false;botw::publish_map_view(f.h,v,true,p);assert(f.values["botw.map.view.heading.valid"]==0&&f.values["botw.map.view.valid"]==1);
  p.heading_valid=true;botw::publish_map_view(f.h,botw::MapView{},true,p); // reader failed: north-up arrow, no cone
  assert(f.values["botw.map.view.valid"]==0&&f.values["botw.map.view.rotation"]==0&&f.values["botw.map.view.heading.valid"]==1&&f.values["botw.map.view.heading"]==291&&f.values["botw.map.view.cone.valid"]==0);cases+=4;}
 // Edge pins keep their true bearing on the turned map.
 {auto a=botw::ready_mark(0,0,1000,0,0),b=botw::ready_mark(0,0,1000,0,90),c=botw::ready_mark(0,0,0,-1000,90);
  assert(a.visible&&b.visible&&c.visible);assert(a.x>620&&a.y==464);assert(b.y>464&&b.x==620);assert(c.x>620&&c.y==464);
  for(double r:{0.,37.,190.,359.}){auto q=botw::ready_mark(10,20,700,-300,r);assert(q.visible&&q.rim&&std::abs(std::hypot(q.x-620.,q.y-464.)-398.)<1);}cases+=5;}
 std::cout<<cases<<" native minimap turn/cone chain, guard, race, cache, degree and publication cases passed\n";
}

#endif
