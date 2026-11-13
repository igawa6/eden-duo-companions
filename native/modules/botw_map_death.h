// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_map.h"
namespace botw {
struct MapDeath { bool valid{},visible{}; std::array<float,3> xyz{}; };
inline MapDeath map_death(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};
  if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
  struct Snapshot {
    uint64_t map{},map_vt{},array{},record{},record_vt{},manager{},manager_vt{};
    int32_t size{},capacity{},kind{},index{},cursor{},count{};
    uint32_t flags{};std::array<float,3> xyz{},cached{};
    bool operator==(const Snapshot&)const=default;
  };
  auto snapshot=[&](Snapshot& s){
    if(!read(h,h.main_base+map_offsets(h)->root,s.map)||!s.map||s.map%8||s.map>UINT64_MAX-0x478||
       !read(h,s.map,s.map_vt)||s.map_vt!=h.main_base+map_offsets(h)->vtable||
       !read(h,s.map+0x468,s.size)||s.size!=1||!read(h,s.map+0x46C,s.capacity)||s.capacity!=1||
       !read(h,s.map+0x470,s.array)||!s.array||s.array%8||s.array>UINT64_MAX-8||
       !read(h,s.array,s.record)||!s.record||s.record%8||s.record>UINT64_MAX-0x44||
       !read(h,s.record,s.record_vt)||s.record_vt!=h.main_base+map_offsets(h)->death_vtable||
       !read(h,s.record+0x1C,s.kind)||s.kind!=24||!read(h,s.record+0x40,s.index)||s.index!=0||
       !read(h,s.record+0x3C,s.flags)||!read(h,s.record+0x28,s.cached)||
       !read(h,h.main_base+map_offsets(h)->death_root,s.manager)||!s.manager||s.manager%8||s.manager>UINT64_MAX-0x3C||
       !read(h,s.manager,s.manager_vt)||s.manager_vt!=h.main_base+map_offsets(h)->death_manager_vtable||
       !read(h,s.manager+0x34,s.cursor)||!read(h,s.manager+0x38,s.count)||
       s.count<0||s.count>1||s.cursor!=(s.count?0:-1)||!read(h,s.manager+0x28,s.xyz))return false;
    s.flags&=0x10;
    if(bool(s.flags)!=bool(s.count))return false;
    if(s.count){
      for(float v:s.xyz)if(!std::isfinite(v))return false;
      if(std::abs(s.xyz[0])>6000||std::abs(s.xyz[1])>10000||std::abs(s.xyz[2])>5000||s.xyz!=s.cached)return false;
    }else{s.xyz={};s.cached={};}
    return true;
  };
  Snapshot first{},last{};
  if(!snapshot(first)||!snapshot(last)||first!=last||!main_field_stage(h)||
     !ui_sample(h,after)||after!=ui)return {};
  return {true,first.count==1,first.count?first.xyz:std::array<float,3>{}};
}
inline void publish_map_death(const EdenDsmodHostApi& h,const MapDeath& v) {
  const bool active=v.valid&&v.visible;
  h.publish_text(h.userdata,"botw.map.death.icon",v.valid?"romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_MapDeadIcon_00^t":"");
  h.publish_i64(h.userdata,"botw.map.death.tint",0xFFC80000);
  h.publish_i64(h.userdata,"botw.map.death.valid",v.valid);
  h.publish_i64(h.userdata,"botw.map.death.count",active?1:0);
  h.publish_i64(h.userdata,"botw.map.death.0.kind",active?24:-1);
  if(h.publish_f64){h.publish_f64(h.userdata,"botw.map.death.0.x",active?v.xyz[0]:0);
    h.publish_f64(h.userdata,"botw.map.death.0.y",active?-v.xyz[2]:0);}
}
}
