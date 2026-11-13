// SPDX-License-Identifier: GPL-3.0-or-later
#include "botw_states.h"
#include "botw_states_fixtures.h"
#include "botw_test_fixture.h"
#include "botw_inventory.h"
#include <cassert>
#include <iostream>
namespace bf=botw_states_fixture;
static uint8_t hexbyte(const char* s){auto v=[](char c){return c<='9'?c-'0':(c|32)-'a'+10;};return uint8_t(v(s[0])<<4|v(s[1]));}
// Rebuilds one live capture: real UI flags, every live screen slot (heap pointer, native name,
// show-state bytes, PauseMenu state pointer) and the two map-name strings, relocated to the
// fixture main base. GameData / event list are absent (published unknown) unless added.
struct LiveFixture:Fixture {
  explicit LiveFixture(const bf::Scenario& s):Fixture(s.flags,s.flags==0x1849000000ULL) {
    for(unsigned i=0;i<99;++i)put<uint64_t>(A+8*i,0);
    for(unsigned k=0;k<s.slot_count;++k){const auto& sl=s.slots[k];
      put<uint64_t>(A+8*sl.index,sl.ptr);put<uint64_t>(sl.ptr+0xD0,M+sl.name_off);
      for(size_t c=0;c<24;++c)put<uint8_t>(M+sl.name_off+c,c<std::strlen(sl.name)?uint8_t(sl.name[c]):0);
      for(unsigned b=0;b<16;++b)put<uint8_t>(sl.ptr+0xF8+b,hexbyte(sl.state16+2*b));
      put<uint64_t>(sl.ptr+0x150,sl.state_ptr?M+sl.state_ptr:0);}
    string(botw::states::MapType,s.map_type_raw);string(botw::states::MapName,s.map_name_raw);
  }
  void string(uint64_t off,const char* raw){
    if(!*raw)return;
    for(unsigned b=0;b<0x38;++b)put<uint8_t>(M+off+b,hexbyte(raw+2*b));
    put<uint64_t>(M+off,M+botw::states::StringVt);put<uint64_t>(M+off+8,M+off+20);
  }
};
static const bf::Scenario& scen(const char* id){for(const auto& s:bf::scenarios)if(!std::strcmp(s.id,id))return s;std::abort();}
#ifndef BOTW_STATES_FIXTURE_ONLY
int main() {
  int cases=0;auto check=[&](bool ok){++cases;if(!ok){std::cerr<<"failure "<<cases<<"\n";std::abort();}};
  using botw::game_states;
  // World (save A, Tabantha): plain world, field section C-3, no menu/event/shrine.
  {LiveFixture f(scen("a-world-1"));auto g=game_states(f.h);check(g.valid);check(!g.title&&!g.loading&&!g.event&&!g.dialogue&&!g.cutscene);
   check(g.map_type=="MainField"&&g.map_name=="C-3");check(!g.shrine&&g.shrine_number==-1);check(g.menu_screen.empty());check(g.slate==-1);
   check(g.screen[botw::states::PauseMenu]==0);check(g.screen[botw::states::AppMap]==2);}
  // Native menu tabs.
  check(game_states(LiveFixture(scen("a-menu-plus")).h).menu_screen=="inventory");
  check(game_states(LiveFixture(scen("a-menu-system")).h).menu_screen=="system");
  check(game_states(LiveFixture(scen("a-menu-advlog")).h).menu_screen=="adventure_log");
  check(game_states(LiveFixture(scen("a-menu-map")).h).menu_screen=="map");
  check(game_states(LiveFixture(scen("a-slate-runes")).h).menu_screen=="runes");
  check(game_states(LiveFixture(scen("a-slate-album")).h).menu_screen=="album");
  // Memory playback launched from the Adventure Log: event/cutscene, PauseMenu stays shown below
  // but the flags are not the menu signature -> no menu screen; flags clear bit 36 -> no dialogue.
  {LiveFixture f(scen("a-memory-play"));auto g=game_states(f.h);check(g.valid&&g.event&&g.cutscene&&!g.dialogue&&!g.loading);
   check(g.menu_screen.empty());check(g.screen[botw::states::PauseMenu]==2);check(g.screen[botw::states::DemoMessage]>=0);}
  // Inside Sha Warvo Shrine.
  {LiveFixture f(scen("a-shrine-in"));auto g=game_states(f.h);check(g.valid&&g.shrine&&g.shrine_number==53);
   check(g.map_type=="CDungeon"&&g.map_name=="Dungeon053");check(!g.event);
   botw::publish_states(f.h,g);check(f.texts["botw.state.shrine.id"]=="Dungeon053");check(f.values["botw.state.shrine.number"]==53);
   check(f.values["botw.state.shrine.name_key"]==botw::actor_key("Dungeon053"));check(f.values["botw.state.shrine.sub_key"]==botw::actor_key("Dungeon053_sub"));
   check(f.values["botw.state.shrine.chests_complete"]==-1);check(f.values["botw.state.shrine.active"]==1);}
  // Title: title flag, map type withheld, slate unknown.
  {LiveFixture f(scen("n-title"));auto g=game_states(f.h);check(g.valid&&g.title&&!g.event&&!g.shrine&&g.map_type.empty());
   botw::publish_states(f.h,g);check(f.values["botw.state.title"]==1&&f.values["botw.state.slate_obtained"]==-1);}
  // Pre-Slate Shrine of Resurrection (field section D-6, not a shrine) and after the pickup.
  {auto g=game_states(LiveFixture(scen("n3-sor-loaded")).h);check(g.valid&&!g.shrine&&g.map_name=="D-6"&&!g.event);}
  {auto g=game_states(LiveFixture(scen("n3-slate-done")).h);check(g.valid&&!g.event&&!g.dialogue);}
  // Voice message window during the pedestal event: dialogue (not cutscene).
  {LiveFixture f(scen("n3-msg"));auto g=game_states(f.h);check(g.valid&&g.event&&g.dialogue&&!g.cutscene);check(g.screen[botw::states::MessageSp]==2);}
  // Every capture reproduces and is stable across two reads.
  for(const auto& s:bf::scenarios){LiveFixture f(s);auto a=game_states(f.h),b=game_states(f.h);check(a.valid&&a==b);}
  // Guards: wrong vtable / pointer / capacity / unterminated name -> invalid; unknown show state, duplicates.
  for(int kind=0;kind<8;++kind){LiveFixture f(scen("a-shrine-in"));using namespace botw::states;
    switch(kind){
      case 0:f.put<uint64_t>(Fixture::M+MapType,0x1234);break;
      case 1:f.put<uint64_t>(Fixture::M+MapName+8,Fixture::M+MapName+24);break;
      case 2:f.put<int32_t>(Fixture::M+MapName+16,64);break;
      case 3:for(unsigned b=0;b<32;++b)f.put<uint8_t>(Fixture::M+MapName+20+b,'A');break;
      case 4:f.put<uint8_t>(Fixture::M+MapName+21,1);break;
      case 5:{const auto& s=scen("a-shrine-in");for(unsigned k=0;k<s.slot_count;++k)if(!std::strcmp(s.slots[k].name,"PauseMenu_00"))f.put<uint8_t>(s.slots[k].ptr+0xFE,7);break;}
      case 6:{const auto& s=scen("a-shrine-in");f.put<uint64_t>(s.slots[1].ptr+0xD0,Fixture::M+s.slots[0].name_off);
              // duplicate only matters for named screens: alias PauseMenu twice
              uint64_t pm{};for(unsigned k=0;k<s.slot_count;++k)if(!std::strcmp(s.slots[k].name,"PauseMenu_00"))pm=Fixture::M+s.slots[k].name_off;
              f.put<uint64_t>(s.slots[0].ptr+0xD0,pm);f.put<uint64_t>(s.slots[1].ptr+0xD0,pm);break;}
      case 7:f.mem.erase(Fixture::M+MapType+30);break;
    }
    {if(game_states(f.h).valid)std::cerr<<"kind "<<kind<<"\n";}check(!game_states(f.h).valid);}
  // A read that changes between the two samples is rejected (race on the map name buffer).
  {LiveFixture f(scen("a-shrine-in"));f.race=Fixture::M+botw::states::MapName;check(!game_states(f.h).valid);}
  // Shrine id parsing.
  check(botw::shrine_number("CDungeon","Dungeon000")==0);check(botw::shrine_number("CDungeon","Dungeon135")==135);
  check(botw::shrine_number("CDungeon","Dungeon136")==-1);check(botw::shrine_number("MainField","Dungeon053")==-1);
  check(botw::shrine_number("CDungeon","Dungeon05")==-1);check(botw::shrine_number("CDungeon","DungeonX53")==-1);
  // Event name chain (layout observed live: root vt, list vt, count, selected index, SafeString).
  {LiveFixture f(scen("n3-msg"));using namespace botw::states;const uint64_t R=0x8000000,L=0x8100000,AR=0x8200000,E=0x8300000,T=0x8400000;
   f.put<uint64_t>(Fixture::M+botw::Warp160.event_root,R);f.put<uint64_t>(R,Fixture::M+botw::Warp160.event_vtable);f.put<uint64_t>(R+0x1D2B8,L);
   f.put<uint64_t>(L,Fixture::M+botw::Warp160.event_list_vtable);f.put<uint32_t>(L+8,2);f.put<uint64_t>(L+0x10,AR);f.put<int32_t>(L+0x1F0,0);
   for(unsigned b=0;b<8;++b)f.put<uint8_t>(L+0x1E8+b,b==0?1:0);
   f.put<uint64_t>(AR,0);f.put<uint64_t>(AR+8,E);f.put<uint64_t>(E+0x18,T);
   const char n[40]="Demo701_0";for(unsigned b=0;b<40;++b)f.put<uint8_t>(T+b,uint8_t(n[b]));
   std::string out;check(botw::event_name(f.h,out)&&out=="Demo701_0");check(game_states(f.h).event_name=="Demo701_0");
   f.put<uint64_t>(R+0x1D2B8,0);check(botw::event_name(f.h,out)&&out.empty());f.put<uint64_t>(R+0x1D2B8,L);
   f.put<uint32_t>(L+8,1);check(!botw::event_name(f.h,out));f.put<uint32_t>(L+8,2);
   f.put<uint64_t>(L,0);check(!botw::event_name(f.h,out));f.put<uint64_t>(L,Fixture::M+botw::Warp160.event_list_vtable);
   f.put<uint8_t>(T+9,1);check(!botw::event_name(f.h,out));}
  // Slate flag through the GameData reader (World signature required; title -> unknown).
  for(int value=0;value<2;++value){LiveFixture f(scen("n3-sor-loaded"));
    const uint64_t G=0x9000000,H=0x9001000,B=0x9002000,BA=0x9003000,IA=0x9004000,BF=0x9005000,IF=0x9006000;
    f.put(Fixture::M+0x2CC5FE8,G);f.put(G,Fixture::M+0x2BD07E0);f.put(G+0xBE0,H);f.put(H,B);
    f.put<uint8_t>(G+0xBE8,1);f.put<uint32_t>(G+0xC18,0x8000);f.put<uint32_t>(G+0xC28,1);
    f.put<int32_t>(B+8,1);f.put<int32_t>(B+12,1);f.put(B+16,BA);f.put<int32_t>(B+24,1);f.put<int32_t>(B+28,1);f.put(B+32,IA);
    f.put(BA,BF);f.put(BF,Fixture::M+0x2BD1078);f.put<uint8_t>(BF+8,0);f.put<uint8_t>(BF+9,15);f.put<uint8_t>(BF+10,0);f.put<uint8_t>(BF+11,uint8_t(value));
    f.put<uint32_t>(BF+12,botw::states::SlateFlag);
    f.put(IA,IF);f.put(IF,Fixture::M+0x2BD1140);f.put<uint8_t>(IF+8,1);f.put<uint8_t>(IF+9,1);f.put<int32_t>(IF+16,0);f.put<int32_t>(IF+20,1);f.put<uint32_t>(IF+28,1);
    auto g=game_states(f.h);check(g.valid&&g.slate==value);
    f.put<uint64_t>(Fixture::U+0x64C30,0x1E09003800ULL);check(game_states(f.h).slate==-1);}
  check(botw::states::crc32("IsGet_Obj_DRStone_Get")==0xBC13CC9Bu);check(botw::states::crc32("CompleteTreasure_Dungeon053")==0xB7EC10DDu);
  // Invalid sample publishes unknowns.
  {Fixture f;botw::publish_states(f.h,{});check(f.values["botw.state.valid"]==0&&f.values["botw.state.event"]==-1&&f.texts["botw.state.menu_screen"].empty()&&f.values["botw.state.shrine.name_key"]==-1);}
  // Integration 2026-10-18: StateLatch hysteresis, latched caption kind, page enum.
  {botw::StateLatch l;botw::GameStates ev{};ev.valid=true;ev.event=true;ev.slate=-1;botw::GameStates clear{};clear.valid=true;clear.slate=1;
   l.update(ev,1000);check(!l.dim);l.update(ev,1150);check(!l.dim);l.update(ev,1200);check(l.dim&&l.kind==2);
   botw::GameStates talk=ev;talk.dialogue=true;l.update(talk,1300);check(l.dim&&l.kind==2);   // kind latched
   l.update(clear,1400);check(l.dim);l.update(ev,1500);check(l.dim);                          // short gap: no flicker
   l.update(clear,1600);l.update(clear,1900);check(l.dim);l.update(clear,2000);check(!l.dim&&l.kind==0);
   l.update(botw::GameStates{},2100);check(!l.dim);
   l.update(talk,3000);l.update(talk,3200);check(l.dim&&l.kind==1);
   l.update(botw::GameStates{},3300);l.update(botw::GameStates{},3699);check(l.dim);l.update(botw::GameStates{},3700);check(!l.dim);}
  {botw::StateLatch l;check(l.page==-1);botw::GameStates g{};g.valid=true;g.title=true;l.update(g,0);check(l.page==1);
   g={};g.valid=true;g.loading=true;l.update(g,1);check(l.page==2);
   g={};g.valid=true;g.slate=0;l.update(g,2);check(l.page==1);                 // pre-Slate World
   g={};g.valid=true;g.event=true;g.slate=-1;l.update(g,3);check(l.page==1);   // pedestal event keeps it
   g={};g.valid=true;g.slate=1;l.update(g,4);check(l.page==0);
   l.update(botw::GameStates{},5);check(l.page==0);
   Fixture f;botw::publish_state_latch(f.h,l);check(f.values["botw.state.page"]==0&&f.values["botw.state.dim"]==0&&f.values["botw.state.dim_kind"]==0);}
  {Fixture f;botw::GameStates g{};g.valid=true;g.menu_screen="inventory";botw::publish_states(f.h,g);check(f.values["botw.state.menu_open"]==1);
   g.menu_screen.clear();botw::publish_states(f.h,g);check(f.values["botw.state.menu_open"]==0);
   botw::publish_states(f.h,{});check(f.values["botw.state.menu_open"]==0);}  // torn sample = no menu (2026-10-19)
  std::cout<<cases<<" states cases passed\n";
}

#endif
