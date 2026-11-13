// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
namespace botw {
struct MapSpecial {
  struct Entry {bool visible{};int32_t kind{-1};std::array<float,3> xyz{};};
  bool valid{};std::array<Entry,154> entries{};
};
inline const char* special_art(int kind) {
  switch(kind){case 19:return "Nt_MapYunboIconL_00^t";
    case 20:return "Nt_MapWolfLinkIconL_00^t";
    case 44:return "Sd_MapBikeIconL_00^t";
    case 42:return "Sd_MapWarpDLC_02^t";
    case 26:return "Nt_MapIconGuardianL_00^t";default:return nullptr;}
}
inline MapSpecial map_special(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
  uint64_t obj{},vt{},again{},array{};int32_t count{},capacity{};
  if(!read(h,h.main_base+map_offsets(h)->root,obj)||!obj||obj%8||obj>UINT64_MAX-0x5A8||
     !read(h,obj,vt)||vt!=h.main_base+map_offsets(h)->vtable||
     !read(h,obj+0x598,count)||count<0||count>150||
     !read(h,obj+0x59C,capacity)||capacity!=150||count>capacity||
     !read(h,obj+0x5A0,array)||(count&&(!array||array%8||array>UINT64_MAX-150*8)))return {};
  struct Record {uint64_t pointer{},vtable{},source{};int32_t kind{};uint32_t flags{};
    uint8_t enabled{};std::array<float,3> xyz{};bool operator==(const Record&)const=default;};
  constexpr uint64_t offsets[]={0x378,0x380,0x388,0x480};
  const uint64_t vtables[]={map_offsets(h)->yunobo_vtable,map_offsets(h)->wolf_vtable,map_offsets(h)->bike_vtable,map_offsets(h)->medallion_vtable};
  constexpr int kinds[]={19,20,44,42};
  auto record=[&](unsigned i,Record& r){
    if(!read(h,i<4?obj+offsets[i]:array+(i-4)*8,r.pointer)||!r.pointer||r.pointer%8||r.pointer>UINT64_MAX-0x80||
       !read(h,r.pointer,r.vtable)||r.vtable!=h.main_base+(i<4?vtables[i]:map_offsets(h)->guardian_vtable)||
       !read(h,r.pointer+0x1C,r.kind)||r.kind!=(i<4?kinds[i]:26)||
       !read(h,r.pointer+0x3C,r.flags)||!read(h,r.pointer+0x28,r.xyz))return false;
    r.flags&=0x10;
    if(i==0){if(!read(h,r.pointer+0x50,r.enabled)||r.enabled>1||bool(r.flags)!=bool(r.enabled))return false;}
    if(i>=4){if(!read(h,r.pointer+0x50,r.source)||(r.source&&(r.source%4||r.source>UINT64_MAX-0x68))||(r.flags&&!r.source))return false;}
    if(r.flags){for(float v:r.xyz)if(!std::isfinite(v))return false;
      if(std::abs(r.xyz[0])>6000||std::abs(r.xyz[1])>10000||std::abs(r.xyz[2])>5000)return false;}
    return true;
  };
  std::array<Record,154> records{};MapSpecial out;
  for(unsigned i=0;i<unsigned(count)+4;++i){auto& r=records[i];if(!record(i,r))return {};
    for(unsigned j=0;j<i;++j)if(records[j].pointer==r.pointer)return {};
    out.entries[i]={bool(r.flags),r.kind,r.xyz};}
  for(unsigned i=0;i<unsigned(count)+4;++i){Record r{};if(!record(i,r)||r!=records[i])return {};}
  int32_t nc{},cap{};uint64_t ar{};
  if(!read(h,h.main_base+map_offsets(h)->root,again)||again!=obj||!read(h,obj,again)||again!=vt||
     !read(h,obj+0x598,nc)||nc!=count||!read(h,obj+0x59C,cap)||cap!=capacity||!read(h,obj+0x5A0,ar)||ar!=array||
     !main_field_stage(h)||!ui_sample(h,after)||after!=ui)return {};
  out.valid=true;return out;
}
inline void publish_map_special(const EdenDsmodHostApi& h,const MapSpecial& v,bool full=true) {
  h.publish_i64(h.userdata,"botw.map.special.valid",v.valid);unsigned count{},guardians{};
  // Perf: full=false skips the per-entry loop off the Map page.
  // Perf round 4: skip the whole slot (no kind/icon/tint/x/y writes) when
  // inactive -- same discipline as this round's pins/moving fixes. Note for the coordinator: the
  // "guardian" half of this loop (i>=4, up to 150 slots) feeds the manifest's native_guardians
  // dynamic_markers group, which is currently disabled at the GROUP level (opacity 0, show_bind
  // "botw.rl.never" -- a key this module never publishes, so it is always hidden; grep-confirmed,
  // likely a prior dead-widget-prune pass) -- the runtime never even enters this group's per-slot
  // loop right now, so every guardian write (active or not) is presently unread. Left publishing
  // guardians' real active/inactive state here regardless (this fix only removes INACTIVE-slot
  // writes, not the feature), since re-enabling that group is a content decision outside this
  // item's scope, not a reason to change this publisher's contract.
  // Iteration 12 (Travel Medallion): the 4 singleton slots (Yunobo, Wolf Link, Motorcycle,
  // Travel Medallion = MapData+480 kind 42) are published on EVERY page, not only when `full`.
  // ReadyHyrule's native_special group reads the same botw.map.special.{0..3}.* keys, and since
  // perf2 the cpp passes full=false on Ready (map_entries_full), so with the runtime's per-tick
  // snapshot clear the Ready circle never drew a placed medallion. 4 slots, no measurable cost;
  // the 150-slot guardian slice (unread on every page) stays gated by `full`.
  for(unsigned i=0;i<(full?v.entries.size():4u);++i){const auto& e=v.entries[i];const char* art=special_art(e.kind);
    const bool active=v.valid&&e.visible&&art;
    if(!active)continue;
    Key key(i<4?"botw.map.special.":"botw.map.special.guardian.",i<4?i:i-4);
    h.publish_i64(h.userdata,key(".kind"),e.kind);
    const std::string src=std::string("romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#")+art;
    h.publish_text(h.userdata,key(".icon"),src.c_str());
    h.publish_i64(h.userdata,key(".tint"),e.kind==26?0xFFFF0000:e.kind==42?0xFF0048FF:0xFFF2CA66);
    ++count;if(i>=4)++guardians;if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),e.xyz[0]);h.publish_f64(h.userdata,key(".y"),-e.xyz[2]);}
  }
  if(full){h.publish_i64(h.userdata,"botw.map.special.count",count);
  h.publish_i64(h.userdata,"botw.map.special.guardian.count",guardians);}
}
}
