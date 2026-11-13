// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_tracker_flags.h"
#include "botw_map_offsets.h"
#include "botw_warp_offsets.h"
#include <cstdio>
#include <string_view>
namespace botw {
// Game-state readers (RE: internal notes). Read-only; every value is a
// direct game field: UIManager per-frame flags (bit 0x800 = main+0x1243DB0 "event owns
// player/camera"), ScreenMgr screen show-state (+0xFE) by native layout name (+0xD0), the
// current map type/name FixedSafeStrings and GameData bools. Unknown is published as -1/"".
namespace states {
constexpr uint64_t MapType=0x2BE1708,MapName=0x2BE1740,StringVt=0x2A701C8;
constexpr uint64_t PauseInventory=0x2BA6E70,PauseSystem=0x2BA7110,PauseLog=0x2BA7170;
constexpr uint64_t FlagEvent=0x800,FlagLoading=0x800000,FlagHud=0x1000000000ULL;
constexpr uint32_t SlateFlag=0xBC13CC9Bu; // crc32("IsGet_Obj_DRStone_Get")
// Native layout names of the screens the readers need (exact match).
enum Screen:unsigned {PauseMenu,AppHome,AppMap,AppTool,AppAlbum,Message,MessageSp,MessageGet,DemoMessage,ScreenCount};
constexpr const char* ScreenNames[ScreenCount]={"PauseMenu_00","AppHome_00","AppMap_00","AppTool_00",
  "AppAlbum_00","Message_00","MessageSp_00","MessageGet_00","DemoMessage"};
inline uint32_t crc32(std::string_view s){uint32_t c=~0u;for(unsigned char ch:s){c^=ch;for(unsigned b=0;b<8;++b)c=(c>>1)^(0xEDB88320u&uint32_t(-int32_t(c&1)));}return ~c;}
inline int64_t fnv(std::string_view s){uint32_t k=2166136261u;for(const auto c:s)k=(k^uint8_t(c))*16777619u;return k;}
}
struct StateOffsets {uint64_t map_type,map_name,string_vtable,pause_inventory,pause_system,pause_log;uint32_t pause_state;};
inline constexpr StateOffsets States160{states::MapType,states::MapName,states::StringVt,states::PauseInventory,states::PauseSystem,states::PauseLog,0x150};
inline constexpr StateOffsets States190{0x1DAE3A0,0x1DAE3D8,0x1BC4E70,0x1E2F148,0x1E2F3E8,0x1E2F448,0x168};
inline const StateOffsets* state_offsets(const EdenDsmodHostApi& h) {
  return matches_build(h,InventoryBuild190) ? &States190 : matches_build(h,Build) ? &States160 : nullptr;
}
struct GameStates {
  bool valid{};
  bool title{},loading{},event{},dialogue{},cutscene{};
  std::string event_name,map_type,map_name,menu_screen;
  bool shrine{};int shrine_number{-1};int chests_complete{-1},cleared{-1};
  int slate{-1};
  std::array<int,states::ScreenCount> screen{}; // show state 0..3, -1 absent
  bool operator==(const GameStates&)const=default;
};
// sead::FixedSafeString<32> in main .data: vtable, char* to the inline buffer at +0x14, cap 32.
inline bool fixed_string32(const EdenDsmodHostApi& h,uint64_t off,std::string& out) {
  std::array<uint8_t,56> b{},again{};uint64_t vt{},ptr{};int32_t cap{};
  if(!read(h,h.main_base+off,b))return false;
  std::memcpy(&vt,b.data(),8);std::memcpy(&ptr,b.data()+8,8);std::memcpy(&cap,b.data()+16,4);
  if(vt!=h.main_base+state_offsets(h)->string_vtable||ptr!=h.main_base+off+20||cap!=32)return false;
  const auto* s=reinterpret_cast<const char*>(b.data()+20);const auto n=strnlen(s,32);
  if(n==32)return false;
  for(size_t i=0;i<n;++i)if(s[i]<0x20||s[i]>0x7e)return false;
  if(!read(h,h.main_base+off,again)||again!=b)return false;
  out.assign(s,n);return true;
}
// Show state of the named screens: scans the 99 ScreenMgr slots, matches the native layout name.
inline bool screen_states(const EdenDsmodHostApi& h,const UiSample& ui,std::array<int,states::ScreenCount>& out,
                          std::array<uint64_t,states::ScreenCount>* where=nullptr) {
  out.fill(-1);if(where)where->fill(0);
  std::array<uint64_t,99> slots{};
  if(ui.array>UINT64_MAX-sizeof(slots)||!read(h,ui.array,slots))return false;
  for(const auto p:slots) {
    if(!p)continue;
    if(p%8||p>UINT64_MAX-0x160)return false;
    uint64_t name{};std::array<char,16> text{};uint8_t state{};
    if(!read(h,p+0xD0,name)||name<h.main_base||name>h.main_base+h.main_size-16||!read(h,name,text)||!read(h,p+0xFE,state))return false;
    for(unsigned i=0;i<states::ScreenCount;++i) {
      const auto len=std::strlen(states::ScreenNames[i]);
      if(std::memcmp(text.data(),states::ScreenNames[i],len)||text[len])continue;
      if(state>3||out[i]>=0)return false; // unknown show state or duplicate name: fail closed
      out[i]=state;if(where)(*where)[i]=p;
    }
  }
  return true;
}
// Name of the current event in the event list (empty when none). Optional detail.
inline bool event_name(const EdenDsmodHostApi& h,std::string& out) {
  out.clear();
  if(!warp_build_supported(h))return false;
  const auto& offsets=*warp_offsets(h);
  uint64_t root{},vt{},list{},lvt{},array{},entry{},text{};uint32_t count{};int32_t sel{};int8_t index{};
  if(!read(h,h.main_base+offsets.event_root,root)||root%8||root>UINT64_MAX-0x1D2C0||!read(h,root,vt)||vt!=h.main_base+offsets.event_vtable||
     !read(h,root+offsets.event_current,list))return false;
  if(!list)return true;
  if(list%8||list>UINT64_MAX-0x200||!read(h,list,lvt)||lvt!=h.main_base+offsets.event_list_vtable||!read(h,list+8,count)||count>16||
     !read(h,list+0x10,array)||!read(h,list+0x1F0,sel))return false;
  if(!read(h,list+0x1E8+(sel>=0&&sel<8?sel:0),index)||index<0||uint32_t(index)>=count||array%8||array>UINT64_MAX-0x80||
     !read(h,array+8*uint64_t(index),entry)||!entry||entry%8||entry>UINT64_MAX-0x40||!read(h,entry+0x18,text))return false;
  std::array<char,40> b{};if(!read(h,text,b))return false;
  const auto n=strnlen(b.data(),b.size());if(n==b.size())return false;
  for(size_t i=0;i<n;++i)if(b[i]<0x20||b[i]>0x7e)return false;
  out.assign(b.data(),n);return true;
}
inline int shrine_number(std::string_view type,std::string_view name) {
  if(type!="CDungeon"||name.size()!=10||!name.starts_with("Dungeon"))return -1;
  int n=0;for(const char c:name.substr(7)){if(c<'0'||c>'9')return -1;n=n*10+(c-'0');}
  return n<=135?n:-1;
}
inline const char* menu_screen(uint64_t flags,const std::array<int,states::ScreenCount>& s,uint64_t pause_state,uint64_t main,const StateOffsets& offsets=States160) {
  using namespace states;
  if(flags!=0x1E09000004ULL)return "";
  if(s[PauseMenu]==2){
    if(pause_state==main+offsets.pause_inventory)return "inventory";
    if(pause_state==main+offsets.pause_system)return "system";
    if(pause_state==main+offsets.pause_log)return "adventure_log";
    return "menu";
  }
  if(s[AppHome]==2){
    if(s[AppTool]==2)return "runes";
    if(s[AppAlbum]==2)return "album";
    if(s[AppMap]==2)return "map";
  }
  return "menu";
}
// One stable sample (whole read twice; any difference -> invalid). flags may be a cache for slate.
inline GameStates game_states(const EdenDsmodHostApi& h,TrackerFlagReader* flags=nullptr) {
  using namespace states;
  auto once=[&](GameStates& g,uint64_t& pause_ptr)->bool{
    UiSample ui{};std::array<uint64_t,ScreenCount> where{};
    if(!map_build_supported(h)||h.main_size<inventory_offsets(h)->screen_root+8||h.main_base>UINT64_MAX-(inventory_offsets(h)->screen_root+8)||!ui_sample(h,ui))return false;
    const auto f=ui.flags;
    g.title=f==0x1849000000ULL&&ui.slots[1]&&!ui.slots[2];
    g.loading=!g.title&&(f&FlagLoading);
    const bool world_like=!g.title&&!g.loading&&!ui.slots[1]&&ui.slots[2]&&(f>>24)==0x1E09; // 0x1E09xxxxxx
    if(!screen_states(h,ui,g.screen,&where)||!fixed_string32(h,state_offsets(h)->map_type,g.map_type)||!fixed_string32(h,state_offsets(h)->map_name,g.map_name))return false;
    g.event=!g.title&&!g.loading&&(f&FlagEvent);
    g.dialogue=!g.title&&!g.loading&&(f&FlagHud)&&
      (g.screen[Message]>=1||g.screen[MessageSp]>=1||g.screen[MessageGet]>=1);
    g.cutscene=g.event&&!g.dialogue;
    if(g.event&&!event_name(h,g.event_name))g.event_name.clear();
    pause_ptr=0;if(where[PauseMenu]&&!read(h,where[PauseMenu]+state_offsets(h)->pause_state,pause_ptr))return false;
    g.menu_screen=menu_screen(f,g.screen,pause_ptr,h.main_base,*state_offsets(h));
    if(world_like){g.shrine_number=shrine_number(g.map_type,g.map_name);g.shrine=g.shrine_number>=0;}
    else{g.shrine_number=-1;g.shrine=false;}
    if(!world_like){g.map_type.clear();g.map_name.clear();}
    return true;
  };
  GameStates a{},b{};uint64_t pa{},pb{};
  if(!once(a,pa)||!once(b,pb)||!(a==b)||pa!=pb)return {};
  // GameData: only in the plain World/Menu signature (the reader's own guard); else unknown.
  TrackerFlagReader local;auto& fr=flags?*flags:local;
  if(fr.begin(h)) {
    const auto slate=fr.observe_boolean(SlateFlag);
    std::optional<bool> chests,clear;
    if(a.shrine){char id[16]{};std::snprintf(id,sizeof(id),"Dungeon%03d",a.shrine_number);
      chests=fr.boolean(crc32(std::string("CompleteTreasure_")+id));clear=fr.boolean(crc32(std::string("Clear_")+id));}
    if(fr.finish()){a.slate=slate?int(*slate):-1;a.chests_complete=chests?int(*chests):-1;a.cleared=clear?int(*clear):-1;}
  }
  a.valid=true;return a;
}
inline void publish_states(const EdenDsmodHostApi& h,const GameStates& g) {
  auto i=[&](const char* k,int64_t v){h.publish_i64(h.userdata,k,v);};
  auto t=[&](const char* k,const std::string& v){h.publish_text(h.userdata,k,v.c_str());};
  const bool v=g.valid;
  i("botw.state.valid",v);i("botw.state.title",v?g.title:-1);i("botw.state.loading",v?g.loading:-1);
  i("botw.state.event",v?g.event:-1);i("botw.state.cutscene",v?g.cutscene:-1);i("botw.state.dialogue",v?g.dialogue:-1);
  t("botw.state.event_name",v?g.event_name:"");t("botw.state.map_type",v?g.map_type:"");t("botw.state.map_name",v?g.map_name:"");
  t("botw.state.menu_screen",v?g.menu_screen:"");i("botw.state.slate_obtained",v?g.slate:-1);
  const bool s=v&&g.shrine;char id[16]{};if(s)std::snprintf(id,sizeof(id),"Dungeon%03d",g.shrine_number);
  i("botw.state.shrine.active",v?g.shrine:-1);t("botw.state.shrine.id",s?id:"");i("botw.state.shrine.number",s?g.shrine_number:-1);
  i("botw.state.shrine.name_key",s?states::fnv(id):-1);i("botw.state.shrine.sub_key",s?states::fnv(std::string(id)+"_sub"):-1);
  i("botw.state.shrine.chests_complete",s?g.chests_complete:-1);i("botw.state.shrine.cleared",s?g.cleared:-1);
  // Numeric companion of menu_screen: widget gates only read numbers (integration 2026-10-18).
  // A torn sample (v=false) must read as "no menu": the runtime shows need_bind widgets on any nonzero
  // value, so -1 put the full-page input lock up for a tick (bug audit 2026-10-19).
  i("botw.state.menu_open",v&&!g.menu_screen.empty());
}
// Integration 2026-10-18: what the companion pages gate on.
// - dim: the event/cutscene lock. Follows `event` (bit 0x800, continuous through a whole event,
//   clears ~150 ms after it; message windows close between lines, so `dialogue` alone would
//   flicker) with hysteresis: on after the event held >= 200 ms, off after it was clear >= 400 ms.
//   An invalid sample counts as clear (needs 400 ms of them to drop the lock).
// - dim_kind: 1 dialogue / 2 cutscene, latched when the lock turns on (caption never flips
//   mid-event even when a voice window opens inside a cutscene).
// - page: 1 main menu (title/save select, or World/Menu before the Sheikah Slate), 2 loading,
//   0 gameplay; any other sample (events, memory playback, unknown) keeps the last value, so the
//   edge-triggered page_binds see one clean edge per real change.
struct StateLatch {
  bool dim{};int kind{};int64_t page{-1};
  int64_t on_since{-1},off_since{-1};
  static constexpr int64_t EnterMs=200,LeaveMs=400;
  void update(const GameStates& g,int64_t now_ms) {
    const bool raw=g.valid&&g.event;
    if(raw){off_since=-1;if(on_since<0)on_since=now_ms;
      if(!dim&&now_ms-on_since>=EnterMs){dim=true;kind=g.dialogue?1:2;}}
    else{on_since=-1;if(off_since<0)off_since=now_ms;
      if(dim&&now_ms-off_since>=LeaveMs){dim=false;kind=0;}}
    if(g.valid){
      if(g.title||(g.slate==0&&!g.loading))page=1;
      else if(g.loading)page=2;
      else if(g.slate==1)page=0;
    }
  }
};
inline void publish_state_latch(const EdenDsmodHostApi& h,const StateLatch& l) {
  h.publish_i64(h.userdata,"botw.state.dim",l.dim);
  h.publish_i64(h.userdata,"botw.state.dim_kind",l.dim?l.kind:0);
  h.publish_i64(h.userdata,"botw.state.page",l.page);
}
}
