// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_map_markers.h"
#include "botw_quest_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct MarkerFixture:QuestFixture {
  static constexpr uint64_t Map=0xA000000,Points=0xA001000,Point=0xA002000;
  botw::QuestSnapshot journal;
  std::map<std::string,double> floats;
  MarkerFixture() {
    h.publish_f64=[](void* p,const char* k,double v){static_cast<MarkerFixture*>(p)->floats[k]=v;};
    journal=botw::quests(h);assert(journal.valid);
    put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put(Map+0x3A0,Entry);
    put<int32_t>(Map+0x3B0,1);put<int32_t>(Map+0x3B4,5);put(Map+0x3B8,Points);
    put(Points,Point);put(Point,std::array<float,3>{-254,0,-1030});
  }
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=MarkerFixture;unsigned cases{};
 auto check=[&](auto change,bool valid,int count=-1){F f;change(f);const auto v=botw::quest_map_markers(f.h,f.journal);if(v.valid!=valid)std::cerr<<"Marker case "<<cases+1<<"\n";assert(v.valid==valid);if(count>=0)assert(v.count==unsigned(count));++cases;};
 {F f;const auto v=botw::quest_map_markers(f.h,f.journal);assert(v.valid&&v.count==1&&(v.positions[0]==std::array<float,3>{-254,0,-1030}));++cases;}
 check([](F&f){f.put<int32_t>(F::Map+0x3B0,0);},true,0);
 check([](F&f){f.put<uint64_t>(F::Map+0x3A0,0);f.put<int32_t>(F::Map+0x3B0,0);f.put<int32_t>(F::Destination+24,-1);},true,0);
 check([](F&f){f.put<int32_t>(F::Map+0x3B0,5);for(unsigned i=0;i<5;++i){f.put(F::Points+i*8,F::Point+i*16);f.put(F::Point+i*16,std::array<float,3>{float(i),0,0});}},true,5);
 check([](F&f){f.journal.valid=false;},false);
 check([](F&f){f.h.build_id[0]^=1;},false);
 check([](F&f){f.h.main_size=0x2CD1700;},false);
 for(int n:{-1,6})check([&](F&f){f.put<int32_t>(F::Map+0x3B0,n);},false);
 for(int n:{0,6})check([&](F&f){f.put<int32_t>(F::Map+0x3B4,n);},false);
 for(auto at:{F::M+0x2CA63B0,F::Map+0x3B8,F::Points,F::Map+0x3A0}){
  check([&](F&f){f.put<uint64_t>(at,0);},false);
  check([&](F&f){f.put<uint64_t>(at,0x10001);},false);
  check([&](F&f){f.put<uint64_t>(at,UINT64_MAX-7);},false);
 }
 check([](F&f){f.put<uint64_t>(F::Map,0);},false);
 check([](F&f){f.put(F::Array,F::Entry+8);},false);
 for(int n:{0,2})check([&](F&f){f.put<int32_t>(F::Entry+12,n);},false);
 for(int n:{-1,2})check([&](F&f){f.put<int32_t>(F::Entry+0x140,n);},false);
 // Item 16: a Main quest's marker now renders at native step 0 too (it is "revealed"
 // from the start -- see botw_quests.h); side/shrine quests still need step>0.
 check([&](F&f){f.put<int32_t>(F::Entry+0x140,0);},true,1);
 check([&](F&f){f.journal.entries[0].category=1;f.put<int32_t>(F::Entry+0x140,0);},false);
 check([](F&f){f.put<uint8_t>(F::Entry+0x30,0);},false);
 check([](F&f){f.put<int32_t>(F::Destination+24,-1);},false);
 check([](F&f){f.journal.entries[0].active=false;},false);
 check([](F&f){f.journal.entries[0].index=1;},false);
 check([](F&f){f.journal.entries[0].state=2;},false);
 check([](F&f){f.journal.entries[0].dlc=2;},false);
 for(float n:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),6001.0f,-6001.0f})check([&](F&f){f.put<float>(F::Point,n);},false);
 check([](F&f){f.put<float>(F::Point+4,10001);},false);
 check([](F&f){f.put<float>(F::Point+8,-5001);},false);
 check([](F&f){f.put<int32_t>(F::Map+0x3B0,2);f.put(F::Points+8,F::Point);},false);
 for(auto at:{F::M+0x2CA63B0,F::Map,F::Map+0x3A0,F::Map+0x3B0,F::Map+0x3B8,F::Points,F::Point,F::Entry+12,F::Entry+0x140,F::Destination,F::U+0x64C30,F::Q+0xC0,F::Array})check([&](F&f){f.race=at;},false);
 for(auto flags:{0x1849000000ULL,0x0E09E00000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 {F f;State state;sample(&state,&f.h);assert(f.values["botw.map.quest.valid"]==1&&f.values["botw.map.quest.count"]==1&&f.floats["botw.map.quest.0.x"]==-254&&f.floats["botw.map.quest.0.y"]==1030);
  // Perf round 4: an inactive quest-destination slot is no longer
  // published at all (same discipline as this round's other dynamic_markers fixes). Check
  // absence, not an explicit -1.
  assert(!f.values.count("botw.map.quest.1.kind"));
  assert(f.texts["botw.map.quest.0.core"].find("Nt_MapChallengeIcon_00^s")!=std::string::npos);
  f.floats.clear();f.values.clear();f.texts.clear();f.put<uint64_t>(F::U+0x64C30,0x0E09E00000ULL);state.next={};state.quest_next={};sample(&state,&f.h);assert(!f.values["botw.map.quest.valid"]&&!f.values["botw.map.quest.count"]&&!f.values.count("botw.map.quest.0.kind")&&!f.texts.count("botw.map.quest.0.core")&&!f.floats.contains("botw.map.quest.0.x"));++cases;}
 std::cout<<cases<<" native quest map marker cases passed\n";
}

#endif
