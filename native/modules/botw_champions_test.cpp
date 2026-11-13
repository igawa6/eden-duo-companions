// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>

struct ChampionFixture:Fixture {
 static constexpr uint64_t Info=0x9800000,Player=0x9900000;
 ChampionFixture(){
  put(M+0x2CA1140,Info);put(Info,M+0x2B7BE80);put(Info+0x60,Player);put(Player,M+0x2B7BEA8);
  for(auto offset:{0x1DF4,0x1E00,0x1E0C,0x1E18}){put<float>(Player+offset,0.f);put<float>(Player+offset+8,-1.f);}
  for(auto offset:{0x1CC8,0x1CCC,0x1CD0})put<int32_t>(Player+offset,3);
 }
};
#ifndef BOTW_CHAMPION_FIXTURE_ONLY
int main(){
 using F=ChampionFixture;unsigned cases{};
 {F f;auto a=botw::champion_abilities(f.h);for(unsigned i=0;i<4;++i){assert(a[i].valid&&!a[i].recharging&&a[i].seconds==0&&a[i].charges==(i?3:1));++cases;}}
 for(const auto [i,offset]:std::array<std::pair<unsigned,unsigned>,4>{{{0,0x1E18},{1,0x1DF4},{2,0x1E00},{3,0x1E0C}}}){
  for(float rate:{-1.f,-3.f,-6.f}){
   F f;f.put<float>(F::Player+offset,1800.f);f.put<float>(F::Player+offset+8,rate);auto a=botw::champion_abilities(f.h);
   assert(a[i].valid&&a[i].recharging&&a[i].charges==0&&a[i].seconds==int32_t(60/-rate));++cases;
  }
  for(float frames:{0.01f,29.f,30.01f}){F f;f.put<float>(F::Player+offset,frames);auto a=botw::champion_abilities(f.h);assert(a[i].valid&&a[i].seconds==(frames>30?2:1));++cases;}
  for(float bad:{NAN,INFINITY,-INFINITY,1000001.f,-31.f}){F f;f.put<float>(F::Player+offset,bad);assert(!botw::champion_abilities(f.h)[i].valid);++cases;}
  for(float bad:{0.f,1.f,-101.f,NAN,INFINITY}){F f;f.put<float>(F::Player+offset,1800.f);f.put<float>(F::Player+offset+8,bad);assert(!botw::champion_abilities(f.h)[i].valid);++cases;}
  {F f;f.mem.erase(F::Player+offset);assert(!botw::champion_abilities(f.h)[i].valid);++cases;}
  for(uint64_t at:{F::Player+offset,F::Player+offset+8}){F f;f.race=at;assert(!botw::champion_abilities(f.h)[i].valid);++cases;}
 }
 for(unsigned offset:{0x1CC8,0x1CCC,0x1CD0})for(int bad:{-1,4,INT32_MAX}){F f;f.put<int32_t>(F::Player+offset,bad);const auto a=botw::champion_abilities(f.h);unsigned valid{};for(auto r:a)valid+=r.valid;assert(valid==3);++cases;}
 for(uint64_t where:{F::M+0x2CA1140,F::Info+0x60})for(uint64_t bad:{uint64_t(0),uint64_t(0x10001),UINT64_MAX-7}){F f;f.put(where,bad);for(auto a:botw::champion_abilities(f.h))assert(!a.valid);++cases;}
 for(uint64_t where:{F::Info,F::Player}){F f;f.put<uint64_t>(where,0);for(auto a:botw::champion_abilities(f.h))assert(!a.valid);++cases;}
 {F f;f.h.build_id[0]^=1;for(auto a:botw::champion_abilities(f.h))assert(!a.valid);++cases;}
 {F f;f.h.main_size=1;for(auto a:botw::champion_abilities(f.h))assert(!a.valid);++cases;}
 for(uint64_t flags:{0x1849000000ULL,0x0E09E00000ULL}){F f;f.put<uint64_t>(F::U+0x64C30,flags);for(auto a:botw::champion_abilities(f.h))assert(!a.valid);++cases;}
 for(uint64_t where:{F::M+0x2CA1140,F::Info+0x60,F::Player,F::U+0x64C30}){F f;f.race=where;for(auto a:botw::champion_abilities(f.h))assert(!a.valid);++cases;}
 // Compact slots must follow ownership (including upgraded actors), not pouch order.
 {F f;f.put<float>(F::Player+0x1DF4,7710.f);f.put<float>(F::Player+0x1DF4+8,-3.f);
  botw::Snapshot pouch{true,{{"Obj_DLC_HeroSoul_Rito",9,1,true,0},{"Obj_HeroSoul_Zora",9,1,false,0}}};
  botw::publish_champion_ownership(f.h,pouch,botw::champion_abilities(f.h));
  // Iteration 14 item 12: obtained-but-pouch-unequipped shows its live state (Zora ready, x1), not "Off".
  assert(f.texts["botw.ready.champion_slot.0.display"]=="x1" && f.values["botw.ready.champion_slot.0.display_kind"]==1);
  assert(f.texts["botw.ready.champion_slot.1.display"]=="1:26");
  assert(f.values["botw.ready.champion_slot.1.display_kind"]==2);
  assert(f.values["botw.ready.champion_slot.1.charges"]==0);
  // A failed current reader must erase an earlier valid timer rather than retain it.
  botw::publish_champion_ownership(f.h,pouch,{});
  assert(f.texts["botw.ready.champion_slot.1.display"]=="--");
  assert(f.values["botw.ready.champion_slot.1.seconds"]==-1);++cases;
 }
 for(const auto& [n,text]:std::array<std::pair<int,const char*>,6>{{{0,"0:00"},{1,"0:01"},{65,"1:05"},{1080,"18:00"},{5999,"99:59"},{6000,"1h40"}}}){assert(botw::champion_countdown(n)==text);++cases;}
 std::cout<<cases<<" Champion live reader/charges/cooldown/race/compaction cases passed\n";
}

#endif
