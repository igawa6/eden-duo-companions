// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
namespace botw {
struct MapMoving {
  struct Entry {bool visible{};int32_t kind{-1};std::array<float,3> xyz{};};
  bool valid{};std::array<Entry,9> entries{};
};
inline const char* moving_art(int kind) {
  switch(kind){case 7:return "Nt_IconSinJuNewMapL_03^s";
    case 8:return "Nt_IconSinJuNewMapL_00^s";
    case 9:return "Nt_IconSinJuNewMapL_01^s";
    case 10:return "Nt_IconSinJuNewMapL_02^s";
    case 18:return "Nt_MapHorseIcon_00^t";default:return nullptr;}
}
inline MapMoving map_moving(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
  uint64_t obj{},vt{},again{};
  if(!read(h,h.main_base+map_offsets(h).root,obj)||!obj||obj%8||obj>UINT64_MAX-0x378||
     !read(h,obj,vt)||vt!=h.main_base+map_offsets(h).vtable)return {};
  constexpr int kinds[]={8,9,7,10,8,9,7,10,18};
  struct Record {
    uint64_t pointer{},vtable{},source{};int32_t kind{},seen{},cached{};
    uint32_t flags{};uint8_t variant{},position_mode{},mounted{};
    std::array<float,3> xyz{};bool operator==(const Record&)const=default;
  };
  auto record=[&](unsigned i,Record& r){
    const auto off=i<8?0x320+i*8:0x370;
    if(!read(h,obj+off,r.pointer)||!r.pointer||r.pointer%8||r.pointer>UINT64_MAX-0x92||
       !read(h,r.pointer,r.vtable)||r.vtable!=h.main_base+(i<8?map_offsets(h).beast_vtable:map_offsets(h).horse_vtable)||
       !read(h,r.pointer+0x1C,r.kind)||r.kind!=kinds[i]||
       !read(h,r.pointer+0x3C,r.flags)||!read(h,r.pointer+0x28,r.xyz))return false;
    r.flags&=i<8?0x88090:0x10;
    if(i<8){
      if(!read(h,r.pointer+0x58,r.source)||!r.source||r.source%4||r.source>UINT64_MAX-0x20||
         !read(h,r.source+0x1C,r.seen)||r.seen<0||!read(h,r.pointer+0x60,r.cached)||r.cached!=r.seen||
         !read(h,r.pointer+0x90,r.variant)||r.variant!=(i>=4)||
         !read(h,r.pointer+0x91,r.position_mode)||r.position_mode>1)return false;
      if(bool(r.flags&0x8000)!=(r.seen>0))return false;
      if((r.flags&0x90)==0x90&&(r.seen==0||bool(r.flags&0x80000)!=bool(r.variant)))return false;
    }else if(!read(h,r.pointer+0x50,r.mounted)||r.mounted>1)return false;
    const bool active=(r.flags&0x10)&&(i<8?bool(r.flags&0x80):!r.mounted);
    if(active){for(float v:r.xyz)if(!std::isfinite(v))return false;
      if(std::abs(r.xyz[0])>6000||std::abs(r.xyz[1])>10000||std::abs(r.xyz[2])>5000)return false;}
    return true;
  };
  std::array<Record,9> records{};MapMoving out;
  for(unsigned i=0;i<records.size();++i){auto& r=records[i];if(!record(i,r))return {};
    const bool visible=(r.flags&0x10)&&(i<8?bool(r.flags&0x80):!r.mounted);
    for(unsigned j=0;j<i;++j)if(records[j].pointer==r.pointer||(i<8&&records[j].source==r.source)||
       (visible&&out.entries[j].visible&&out.entries[j].kind==r.kind))return {};
    out.entries[i]={visible,r.kind,r.xyz};}
  for(unsigned i=0;i<records.size();++i){Record r{};if(!record(i,r)||r!=records[i])return {};}
  if(!read(h,h.main_base+map_offsets(h).root,again)||again!=obj||!read(h,obj,again)||again!=vt||
     !main_field_stage(h)||!ui_sample(h,after)||after!=ui)return {};
  out.valid=true;return out;
}
// Iteration 12 lane M: `cull` (Ready only) publishes the entries inside the Ready follow window,
// same rule as the Ready compact lists (WithinReadyWindow). Since perf round 2 the cpp passed
// full=false on Ready, so Divine Beasts and the horse never drew on the Ready circle; the cpp now
// passes full=true plus the Ready cull there. Off Map/Ready (full=false) nothing changes.
inline void publish_map_moving(const EdenDsmodHostApi& h,const MapMoving& v,bool full=true,const MapPosition* cull=nullptr) {
  h.publish_i64(h.userdata,"botw.map.moving.valid",v.valid);unsigned count{};
  // Perf: full=false skips the per-entry loop off the Map page.
  // Perf round 4: skip the whole slot (no kind/icon/tint/x/y writes) when
  // inactive -- same discipline as round 3's koroks/places/landmarks fix and this round's pins
  // fix; native_moving is a plain dynamic_markers group (hide_when_kind=-1), so a missing kind
  // (and the x/y this publisher never wrote for an inactive slot anyway) already `continue`s the
  // runtime's own marker loop.
  if(full)for(unsigned i=0;i<v.entries.size();++i){const auto& e=v.entries[i];const char* art=moving_art(e.kind);
    const bool active=v.valid&&e.visible&&art&&(!cull||WithinReadyWindow(e.xyz[0],e.xyz[2],*cull));
    if(!active)continue;
    Key key("botw.map.moving.",i);
    h.publish_i64(h.userdata,key(".kind"),e.kind);
    const std::string src=std::string("romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#")+art;
    h.publish_text(h.userdata,key(".icon"),src.c_str());
    h.publish_i64(h.userdata,key(".tint"),i<8?0xFFFF9600:0xFFF2CA66);
    ++count;if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),e.xyz[0]);h.publish_f64(h.userdata,key(".y"),-e.xyz[2]);}
  }
  if(full)h.publish_i64(h.userdata,"botw.map.moving.count",count);
}
}
