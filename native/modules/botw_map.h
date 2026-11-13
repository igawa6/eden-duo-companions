// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_hud.h"
#include <algorithm>
#include <cstdio>
namespace botw {
struct MapSnapshot {
  bool valid{},main_field{};
  uint16_t revealed{};
  std::array<uint16_t,120> masks{},submasks{},subfilters{};
  std::array<bool,6> changes{};
};
inline bool map_masks(const EdenDsmodHostApi& h,uint64_t head,std::array<uint16_t,120>& out,unsigned field=0) {
  if(field>2)return false;
  std::array<uint64_t,3> first{},last{};
  if(head>UINT64_MAX-24||!read(h,head,first)||first[2]!=120)return false;
  auto node=first[1];std::array<uint64_t,120> seen{};
  for(unsigned i=0;i<120;++i) {
    if(node<8||node%4||node>UINT64_MAX-16||node==head||
       std::find(seen.begin(),seen.begin()+i,node)!=seen.begin()+i)return false;
    seen[i]=node;std::array<uint8_t,24> raw{},again{};
    if(!read(h,node-8,raw))return false;
    uint64_t previous{},next{};std::memcpy(&out[i],raw.data()+field,2);
    std::memcpy(&previous,raw.data()+8,8);std::memcpy(&next,raw.data()+16,8);
    if(previous!=(i?seen[i-1]:head)||!read(h,node-8,again)||again!=raw)return false;
    node=next;
  }
  return node==head&&first[0]==seen.back()&&read(h,head,last)&&first==last;
}
inline bool main_field_stage(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return false;
  // Exact-build scene/request type strings. Both must agree; other stages are withheld.
  for(const uint64_t off:{ready_offsets(h).stage_current,ready_offsets(h).stage_requested}) {
    std::array<uint8_t,56> b{};uint64_t vt{},ptr{};int32_t cap{};
    if(!read(h,h.main_base+off,b))return false;
    std::memcpy(&vt,b.data(),8);std::memcpy(&ptr,b.data()+8,8);std::memcpy(&cap,b.data()+16,4);
    if(vt!=h.main_base+ready_offsets(h).stage_vtable||ptr!=h.main_base+off+20||cap!=32||
       std::memcmp(b.data()+20,"MainField\0",10)!=0)return false;
  }
  // Integration 2026-10-18 (states RE-FINDINGS section 3): the two strings above stay
  // "MainField" inside shrines, so every map/position reader kept publishing stale field data
  // there. The current map type FixedSafeString (main+0x2BE1708: "MainField" / "CDungeon" /
  // "TitleMenu") withholds them when it is a valid string other than MainField.
  {
    const uint64_t off=ready_offsets(h).map_type;std::array<uint8_t,56> b{};uint64_t vt{},ptr{};int32_t cap{};
    if(read(h,h.main_base+off,b)){
      std::memcpy(&vt,b.data(),8);std::memcpy(&ptr,b.data()+8,8);std::memcpy(&cap,b.data()+16,4);
      if(vt==h.main_base+ready_offsets(h).stage_vtable&&ptr==h.main_base+off+20&&cap==32&&
         std::memchr(b.data()+20,0,32)!=nullptr&&std::memcmp(b.data()+20,"MainField\0",10)!=0)return false;
    }
  }
  return true;
}
inline MapSnapshot map_snapshot(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};MapSnapshot out;
  if(h.main_size<0x2CBDB88||h.main_base>UINT64_MAX-0x2CBDB88||!ui_sample(h,ui)||!hud_context(ui))return {};
  uint64_t p{},p2{},vt{},vt2{};uint32_t mask{},mask2{};
  if(!read(h,h.main_base+0x2CA63B0,p)||p%8||p>UINT64_MAX-0x124||
     !read(h,p,vt)||vt!=h.main_base+0x2B86848||!read(h,p+0x120,mask)||(mask&~0x7FFFu)||
     ui.manager>UINT64_MAX-0x63E90||!map_masks(h,ui.manager+0x627E8,out.masks)||
     !map_masks(h,ui.manager+0x63E78,out.subfilters)||!map_masks(h,ui.manager+0x63E78,out.submasks,2))return {};
  constexpr std::array<uint32_t,6> hashes={0x25718F36,0xBBDE4235,0x22D7138F,0x55D02319,0xCBB4B6BA,0xF44A4DAA};
  for(unsigned i=0;i<6;++i)if(!rune_upgrade(h,hashes[i],out.changes[i]))return {};
  for(auto v:out.masks)if(v&~0x7FFFu)return {};
  for(auto v:out.submasks)if(v&~0x3Fu)return {};
  out.main_field=main_field_stage(h);
  if(!read(h,h.main_base+0x2CA63B0,p2)||p2!=p||!read(h,p,vt2)||vt2!=vt||
     !read(h,p+0x120,mask2)||mask2!=mask||main_field_stage(h)!=out.main_field||
     !ui_sample(h,after)||after!=ui)return {};
  for(unsigned i=0;i<6;++i){bool v{};if(!rune_upgrade(h,hashes[i],v)||v!=out.changes[i])return {};}
  out.revealed=uint16_t(mask);out.valid=true;return out;
}
struct MapPosition {bool valid{};float x{},y{};bool heading_valid{};float heading{};};
// Perf round 2: quantize the player's published display position to
// a visible-resolution grid so sub-pixel float jitter (the native actor transform's raw float
// noise) doesn't trip the runtime's redraw dirty-check every tick for a change nothing can see.
// Ready's minimap widget draws ReadyHyrule (min [-6000,-5000], max [6000,5000]) into an 800x800
// px rect (prototype/dualscreen/manifest.json "ready" page's "map" widget) -- the limiting axis
// is ~15 world units/px (12000/800), so 0.25 px of visible motion is ~3.75 world units; rounding
// the *published* x/y to that grid (the underlying read stays exact -- this only affects what
// goes out over the publish boundary) is below what the Ready circle can show at its own pixel
// density. Only applied where the value's own precision doesn't matter to a test or a game-state
// decision -- the live player marker/follow position (botw.map.x/.y) and the new compact Ready
// marks (botw_map_ready_marks.h) -- not the full per-entry Map lists (koroks/places/landmarks/...),
// whose host tests assert exact native coordinates and whose Map-page display is zoomed out
// enough that this wouldn't be visible anyway.
inline constexpr float kDisplayQuantum = 3.75f;
inline float quantize_display(float v) {
  if (!std::isfinite(v)) return v;
  return std::round(v / kDisplayQuantum) * kDisplayQuantum;
}
// Perf: Ready-page spatial cull. ReadyHyrule's map widget has
// follow_window=900 (prototype/dualscreen/manifest.json) -- a 900x900 world-unit window
// centred on the player that rotates with the camera, so a world point can be as far as
// (900/2)*sqrt(2) from the player and still land inside the (rotated) square window; +100 margin
// for position jitter/the moment-to-moment camera cone. Map's FullHyrule has no follow_window
// (it's the static full-world map), so this only ever applies on Ready. A world marker's xyz[0]/
// -xyz[2] is directly comparable to MapPosition.x/y -- both are "y grows north" display space
// (map_position() below negates xyz[2] the same way every *_xyz-publishing reader already does).
inline constexpr float kReadyCullRadius = 450.0f * 1.41421356f + 100.0f; // ~736.4
inline bool WithinReadyWindow(float world_x, float world_z, const MapPosition& pos) {
  if (!pos.valid) return true; // no reference point: fail open, never hide for a missing position
  const float dx = world_x - pos.x, dy = (-world_z) - pos.y;
  return dx * dx + dy * dy <= kReadyCullRadius * kReadyCullRadius;
}
// Perf round 3 (PERF2-BRIEF.md item 6 "Map-page viewport culling"):
// FullHyrule (the Map page's map widget) has no follow_window -- it's the static full-world
// map -- but the runtime DOES publish the last-drawn view per area as host floats
// "@map_view_{cx,cy,ppw,x,y,w,h}:<area>" (see botw_map_page_overlay.h's MapViewRect/map_view_rect,
// read at the call site for area "FullHyrule"; that header is not included here to avoid a
// circular dependency, so the call site converts the host floats into this plain world-space
// AABB before passing it down). Margin: 50% of the view's world half-extent on each side, so a
// marker just outside the drawn view is still published and ready to scroll in on the very next
// pan tick (the manifest's own marker/label widgets already no-op an out-of-rect draw, same as
// an out-of-follow-window Ready entry) -- this does not change what is drawn, only what is
// candidate+published for an area already far outside the margin.
struct MapViewCull {
  bool valid{};
  float minx{},maxx{},miny{},maxy{}; // world-space AABB, already expanded by the pan margin
};
inline bool WithinMapView(float world_x, float world_z, const MapViewCull& c) {
  if (!c.valid) return true; // invalid/unknown view: fail open, same discipline as WithinReadyWindow
  const float wy = -world_z;
  return world_x >= c.minx && world_x <= c.maxx && wy >= c.miny && wy <= c.maxy;
}
inline MapPosition map_position(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};
  if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
  uint64_t p{},p2{},vt{},actor{},avt{};std::array<float,3> a{},z{};
  if(!read(h,h.main_base+0x2CA1140,p)||p%8||p>UINT64_MAX-0x94||!read(h,p,vt)||
     vt!=h.main_base+0x2B7BE80||!read(h,p+0x60,actor)||actor%8||!read(h,actor,avt)||
     avt!=h.main_base+0x2B7BEA8||!read(h,p+0x88,a))return {};
  for(auto v:a)if(!std::isfinite(v)||std::abs(v)>50000.f)return {};
  if(!read(h,p+0x88,z)||a!=z||!read(h,h.main_base+0x2CA1140,p2)||p2!=p||
     !main_field_stage(h)||!ui_sample(h,after)||ui!=after)return {};
  MapPosition out{true,a[0],-a[2]}; // map world y grows north; game z grows south
  // Native Actor transform: third column is forward, fourth is translation.
  // Captured on 1.6.0 across three normal movement directions; absent/unstable
  // rotation withholds heading independently of a valid position.
  std::array<float,12> matrix{},again{};uint64_t actor2{},avt2{};
  if(actor>UINT64_MAX-0x3C8 || !read(h,actor+0x398,matrix) ||
     !read(h,actor+0x398,again) || matrix!=again ||
     !read(h,p+0x60,actor2) || actor2!=actor || !read(h,actor,avt2) || avt2!=avt)return out;
  for(auto v:matrix)if(!std::isfinite(v))return out;
  float error{};for(unsigned i=0;i<3;++i)error+=(matrix[i*4+3]-a[i])*(matrix[i*4+3]-a[i]);
  const float norm=matrix[2]*matrix[2]+matrix[6]*matrix[6]+matrix[10]*matrix[10];
  const float horizontal=matrix[2]*matrix[2]+matrix[10]*matrix[10];
  if(error>4.f || norm<.9f || norm>1.1f || horizontal<.01f)return out;
  if(!ui_sample(h,after)||after!=ui||!main_field_stage(h))return {};
  out.heading_valid=true;out.heading=std::atan2(matrix[2],-matrix[10])*180.f/3.14159265358979323846f;
  return out;
}
// Native revealed-tile texture name, written into a caller buffer (no allocation per tile).
// Returns "" for an invalid snapshot, tile or level. The longest name is 56 bytes
// (level<=3, row 0-9, revealed<=65535, sub<=63), well inside the 80-byte buffer.
inline const char* map_tile_name(const MapSnapshot& s,unsigned tile,unsigned level,std::array<char,80>& out) {
  out[0]=0;
  if(!s.valid||tile>=120||level<1||level>3)return out.data();
  const auto mask=s.masks[tile],revealed=uint16_t(s.revealed&mask);
  const char col="ZABCDEFGHIJK"[tile%12],row=char('0'+tile/12);
  int n=std::snprintf(out.data(),out.size(),"romfs:/UI/MapTex/MainField/MapTex%u_%c-%c",level,col,row);
  if(revealed!=mask)n+=std::snprintf(out.data()+n,out.size()-size_t(n),"_%u",unsigned(revealed));
  if(revealed && (s.revealed&s.subfilters[tile])) {
    uint16_t sub{};for(unsigned i=0;i<6;++i)if(s.changes[i])sub|=uint16_t(1u<<i);
    sub&=s.submasks[tile];
    if(sub)n+=std::snprintf(out.data()+n,out.size()-size_t(n),"-%u",unsigned(sub));
  }
  std::snprintf(out.data()+n,out.size()-size_t(n),".sbmaptex");
  return out.data();
}
inline std::string map_tile(const MapSnapshot& s,unsigned tile,unsigned level) {
  std::array<char,80> name{};
  return map_tile_name(s,tile,level,name);
}
} // namespace botw
