// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Travel (fast travel) to a discovered shrine/tower: exact-build readers for the native travel
// point (MapData record + its LocationMarker entry) and the game's own allow-check inputs.
// RE chain: internal notes All reads are double-sampled and fail closed.
#include "botw_context.h"
#include "botw_runes.h"
#include "botw_map_landmarks.h"
#include "botw_warp_logic.h"
#include "botw_warp_offsets.h"
#include <string>
namespace botw::warp {
// Travel Medallion (DLC WarpDLC, iteration 12 item 13a):
// the MapData+480 singleton record (class 2B875E8, kind 42, placed = +3C bit4, position +28) and
// the native Travel producer's own destination strings (0x10F0074 DLC branch).
inline constexpr uint64_t MedallionSlot=0x480;
inline constexpr int MedallionKind=42;
// Pseudo landmarks index for the medallion (point()/request_warp()/TravelUi keep one index type);
// far above the 151 shrine+tower slots.
inline constexpr unsigned MedallionIndex=0x7FFF;
inline uint32_t crc(std::string_view s){uint32_t c=~0u;for(unsigned char ch:s){c^=ch;for(unsigned b=0;b<8;++b)c=(c>>1)^(0xEDB88320u&uint32_t(-int32_t(c&1)));}return ~c;}
inline uint32_t fnv(std::string_view s){uint32_t k=2166136261u;for(unsigned char c:s)k=(k^c)*16777619u;return k;}
// Bounded NUL-terminated printable string (<64 bytes), as the guest helper hashes it.
inline bool cstr(const EdenDsmodHostApi& h,uint64_t p,std::string& out) {
  std::array<char,64> b{};if(!p||p>UINT64_MAX-64||!read(h,p,b))return false;
  const auto* end=static_cast<const char*>(std::memchr(b.data(),0,b.size()));if(!end||end==b.data())return false;
  out.assign(b.data(),size_t(end-b.data()));for(unsigned char c:out)if(c<0x20||c>0x7E)return false;return true;
}
struct Point {
  bool valid{};
  int kind{-1};unsigned index{};uint64_t data{},record{},entry{},map_ptr{},pos_ptr{};
  uint32_t state{};uint8_t tower_open{};std::string message,map,pos;std::array<float,3> xyz{};
  // The native "travel point enabled" input (109D960/109FAF0 -> 109CC10): shrine +3C bit17
  // (Enter_), tower +64 open byte. (+3C bit9, the cached result, is refreshed lazily.)
  // Medallion: placed (+3C bit4, the bit the native map draws it by).
  bool enabled() const {return valid&&(kind==MedallionKind?(state&0x10u)!=0:kind==2?(state&0x20000u)!=0:tower_open==1);}
  bool operator==(const Point&) const = default;
};
// The placed Travel Medallion as a travel point: no LocationMarker entry; map/pos are the native
// rodata strings ("MainField/WarpIcon"/"WarpPoint01"), checked by content as well as address.
inline Point medallion_point(const EdenDsmodHostApi& h) {
  auto once=[&](Point& p) {
    uint64_t data{},vt{};int32_t kind{};
    if(!warp_build_supported(h)||h.main_size<map_offsets(h)->root+8||h.main_size<warp_offsets(h)->medallion_map+64||h.main_size<warp_offsets(h)->medallion_pos+64||
       !read(h,h.main_base+map_offsets(h)->root,data)||!data||data%8||data>UINT64_MAX-0x500||
       !read(h,data,vt)||vt!=h.main_base+map_offsets(h)->vtable||!read(h,data+MedallionSlot,p.record)||!p.record||p.record%8||p.record>UINT64_MAX-0x70||
       !read(h,p.record,vt)||vt!=h.main_base+map_offsets(h)->medallion_vtable||!read(h,p.record+0x1C,kind)||kind!=MedallionKind||
       !read(h,p.record+0x3C,p.state)||!read(h,p.record+0x28,p.xyz))return false;
    p.kind=MedallionKind;p.index=0;p.data=data;p.entry=0;
    p.map_ptr=h.main_base+warp_offsets(h)->medallion_map;p.pos_ptr=h.main_base+warp_offsets(h)->medallion_pos;
    if(!cstr(h,p.map_ptr,p.map)||p.map!="MainField/WarpIcon"||!cstr(h,p.pos_ptr,p.pos)||p.pos!="WarpPoint01")return false;
    p.message="Obj_WarpDLC";
    for(float v:p.xyz)if(!std::isfinite(v))return false;
    if((p.state&0x10u)&&(std::abs(p.xyz[0])>6000||std::abs(p.xyz[1])>10000||std::abs(p.xyz[2])>5000))return false;
    return p.valid=true;
  };
  Point a{},b{};if(!once(a)||!once(b)||a!=b)return {};
  return a;
}
// `n` = index into the landmarks order (shrine array first, then towers), as botw_map_ui.h uses;
// MedallionIndex = the placed Travel Medallion.
inline Point point(const EdenDsmodHostApi& h,unsigned n) {
  if(n==MedallionIndex)return medallion_point(h);
  auto once=[&](Point& p) {
    uint64_t data{},vt{},arr{};int32_t sc{},scap{},tc{},tcap{};
    if(!warp_build_supported(h)||h.main_size<map_offsets(h)->root+8||!read(h,h.main_base+map_offsets(h)->root,data)||!data||data%8||data>UINT64_MAX-0x320||
       !read(h,data,vt)||vt!=h.main_base+map_offsets(h)->vtable||!read(h,data+0x2F0,sc)||!read(h,data+0x2F4,scap)||
       !read(h,data+0x310,tc)||!read(h,data+0x314,tcap)||sc<0||sc>scap||scap>136||tc<0||tc>tcap||tcap>15)return false;
    const bool tower=n>=unsigned(sc);const unsigned i=tower?n-unsigned(sc):n;
    if(i>=unsigned(tower?tc:sc)||!read(h,data+(tower?0x318:0x2F8),arr)||!arr||arr%8)return false;
    p.kind=tower?3:2;p.index=i;p.data=data;
    int32_t kind{};
    if(!read(h,arr+uint64_t(i)*8,p.record)||!p.record||p.record%8||p.record>UINT64_MAX-0x70||
       !read(h,p.record,vt)||vt!=h.main_base+(tower?map_offsets(h)->tower_vtable:map_offsets(h)->shrine_vtable)||!read(h,p.record+0x1C,kind)||kind!=p.kind||
       !read(h,p.record+0x3C,p.state)||(tower&&(!read(h,p.record+0x64,p.tower_open)||p.tower_open>1))||!read(h,p.record+0x28,p.xyz)||!read(h,p.record+0x58,p.entry)||!p.entry||p.entry%4||
       p.entry>UINT64_MAX-0x40)return false;
    uint64_t msg{};
    if(!read(h,p.entry+0x08,msg)||!read(h,p.entry+0x28,p.map_ptr)||!read(h,p.entry+0x30,p.pos_ptr)||
       !cstr(h,msg,p.message)||!cstr(h,p.map_ptr,p.map)||!cstr(h,p.pos_ptr,p.pos))return false;
    for(float v:p.xyz)if(!std::isfinite(v))return false;
    return p.valid=true;
  };
  Point a{},b{};if(!once(a)||!once(b)||a!=b)return {};
  return a;
}
// The game's own Travel allow-check inputs at the current location (module side; the guest
// helper re-checks each one natively in the same frame as the call).
inline Env env(const EdenDsmodHostApi& h) {
  auto once=[&](Env& e) {
    UiSample ui{};if(!ui_sample(h,ui))return false;
    e.world=ui.flags==0x1E09000000ULL&&!ui.slots[1]&&ui.slots[2];
    e.menu=ui.flags==0x1E09000004ULL;
    e.loading=(ui.flags&0x800000ULL)!=0;
    uint64_t evt{},vt{},cur{};
    if(!read(h,h.main_base+warp_offsets(h)->event_root,evt)||!evt||evt%8||!read(h,evt,vt)||vt!=h.main_base+warp_offsets(h)->event_vtable||
       !read(h,evt+warp_offsets(h)->event_current,cur))return false;
    e.event=cur!=0||(ui.flags&0x800ULL)!=0;
    uint64_t l{};uint8_t latch{};
    if(read(h,h.main_base+warp_offsets(h)->latch_root,l)&&l&&read(h,l+0x28,l)&&l&&read(h,l+0x98,l)&&l){if(!read(h,l+0x28,latch))return false;}
    e.latch=latch!=0;
    // Native loading screens (ScreenMgr slots 69/78): state +FE or a positive request +FD.
    for(unsigned slot:{69u,78u}){uint64_t s{};uint8_t st{};int8_t rq{};
      if(!read(h,ui.array+slot*8,s))return false;
      if(s&&(!read(h,s+0xFE,st)||!read(h,s+0xFD,rq)))return false;
      if(s&&(st||rq>0))e.loading=true;}
    bool prohibited=false;
    for(const char* name:{"WarpProhibition","WarpProhibitionArea","WarpProhibitionArea_Fire_Relic"}){
      const auto f=rune_flag(h,crc(name));if(!f.valid)return false;prohibited|=f.value;}
    e.prohibited=prohibited;
    return e.valid=true;
  };
  Env a{},b{};if(!once(a)||!once(b)||a!=b)return {};
  return a;
}
// Resolve a shrine directly by its native MessageID against the live MapData array -- unlike
// find() below (which cross-checks the overworld landmarks table), this has NO main-field-stage
// gate: it is the only lookup that still works while physically standing inside the shrine
// itself, where botw_map_landmarks.h's main_field_stage() keeps the landmarks table invalid for
// the whole visit (found live 2026-10-19: botw.map.poi.valid stays 0 inside a shrine, which left
// the Ready-panel Travel button (item 9) permanently unresolvable even with warp.supported
// healthy and the correct shrine/number). Shrines only (towers are never "entered"); 136 is the
// documented max shrine array capacity.
inline int find_shrine_by_id(const EdenDsmodHostApi& h,std::string_view id) {
  for(unsigned n=0;n<136;++n){const auto p=point(h,n);if(p.valid&&p.kind==2&&p.message==id)return int(n);}
  return -1;
}
// Map a Trackers row id ("DungeonNNN" / "TowerNN") to its landmarks index: shrines by the native
// MessageID, towers by MessageID or (as tracker_data does) by exact native position.
inline int find(const EdenDsmodHostApi& h,const MapLandmarks& marks,std::string_view id,float x,float z) {
  if(!marks.valid||id.empty())return -1;
  int found=-1;
  for(unsigned i=0;i<marks.count;++i){
    const auto& m=marks.entries[i];const bool tower=m.kind==3;
    if(tower!=id.starts_with("Tower")||!(std::abs(m.xyz[0]-x)<=1&&std::abs(m.xyz[2]-z)<=1))continue;
    if(found>=0)return -1;
    found=int(i);
  }
  if(found<0)return -1;
  const auto p=point(h,unsigned(found));
  if(!p.valid)return -1;
  // Identity is the native MessageID itself (live: "Dungeon118", "Tower02"), plus the position.
  if(p.message!=id)return -1;
  return found;
}
} // namespace botw::warp
