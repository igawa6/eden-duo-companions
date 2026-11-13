// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_saved_map_icons.h"
namespace botw {
struct MapEditMarker {
  uint64_t address{};int32_t kind{};uint32_t active{};
  std::array<float,3> xyz{};
  bool operator==(const MapEditMarker&) const = default;
};
struct MapEditSnapshot {
  bool valid{};UiSample ui{};
  uint64_t gamedata{},data{},controller{},area{},stamps{},pins{},free_stamp{};
  std::array<float,4> projection{};std::array<float,2> scale{};
  std::array<MapEditMarker,105> markers{};
  bool operator==(const MapEditSnapshot&) const = default;
};
inline MapEditSnapshot map_edit_snapshot(const EdenDsmodHostApi& h) {
  auto ptr=[](uint64_t p,uint64_t n){return p && !(p%8) && p<=UINT64_MAX-n;};
  auto capture=[&](MapEditSnapshot& s) {
    uint64_t vt{},p{};int32_t count{},cap{};
    if(!ui_sample(h,s.ui) || s.ui.flags!=0x1E09000000 || !main_field_stage(h) ||
       !read(h,h.main_base+inventory_offsets(h).gamedata_root,s.gamedata) || !ptr(s.gamedata,0xC2C) ||
       !read(h,s.gamedata,vt) || vt!=h.main_base+inventory_offsets(h).gamedata_vtable ||
       !read(h,h.main_base+map_offsets(h).root,s.data) || !ptr(s.data,0x8B8) ||
       !read(h,s.data,vt) || vt!=h.main_base+map_offsets(h).vtable ||
       !read(h,h.main_base+map_offsets(h).controller_root,s.controller) || !ptr(s.controller,0x8E8) ||
       !read(h,s.controller,vt) || vt!=h.main_base+map_offsets(h).controller_vtable ||
       !read(h,s.controller+0x8D8,p) || p!=s.data ||
       !read(h,s.controller+0x8E0,s.area) || s.area!=s.data+0x850 ||
       !read(h,s.area,vt) || vt!=h.main_base+map_offsets(h).area_vtable ||
       !read(h,s.data+0x610,count) || count!=100 || !read(h,s.data+0x614,cap) || cap!=100 ||
       !read(h,s.data+0x618,s.stamps) || !ptr(s.stamps,800) ||
       !read(h,s.data+0x658,count) || count!=5 || !read(h,s.data+0x65C,cap) || cap!=5 ||
       !read(h,s.data+0x660,s.pins) || !ptr(s.pins,40) ||
       !read(h,s.data+0x650,s.free_stamp) || !read(h,s.data+0x878,s.projection) ||
       !read(h,s.data+0x8B0,s.scale))return false;
    for(float v:s.projection)if(!std::isfinite(v))return false;
    if(s.projection[0]<=0 || s.projection[0]>16 || std::abs(s.projection[1])>3600 ||
       std::abs(s.projection[2])>24000 || std::abs(s.projection[3])>24000 ||
       s.scale[0]!=2 || s.scale[1]!=2)return false;
    std::set<uint64_t> seen;uint64_t first_free{};
    for(unsigned i=0;i<105;++i) {
      auto& v=s.markers[i];uint32_t flags{};int32_t slot{};
      if(!read(h,(i<5?s.pins:s.stamps)+(i<5?i:i-5)*8,v.address) || !ptr(v.address,0x48) ||
         !seen.insert(v.address).second || !read(h,v.address,vt) ||
         vt!=h.main_base+(i<5?map_offsets(h).pin_vtable:map_offsets(h).stamp_vtable) ||
         !read(h,v.address+0x1C,v.kind) || !read(h,v.address+0x3C,flags) ||
         !read(h,v.address+0x28,v.xyz))return false;
      v.active=flags&16;
      if(i<5 && (!read(h,v.address+0x40,slot) || slot!=int32_t(i)))return false;
      if(v.active && (i<5?v.kind!=37+int32_t(i):v.kind<27 || v.kind>35))return false;
      for(float f:v.xyz)if(!std::isfinite(f))return false;
      if(v.active && (std::abs(v.xyz[0])>6000 || std::abs(v.xyz[1])>10000 || std::abs(v.xyz[2])>5000))return false;
      if(i>=5 && !v.active && !first_free)first_free=v.address;
    }
    return s.free_stamp==first_free;
  };
  MapEditSnapshot a{},b{};
  if(!map_build_supported(h) || !capture(a) || !capture(b) || a!=b || !main_field_stage(h))return {};
  a.valid=true;return a;
}
inline int map_edit_target(const MapEditSnapshot& s,unsigned palette) {
  if(!s.valid || palette>13)return -1;
  if(palette>=6 && palette<=10)return int(palette-6);
  for(unsigned i=5;i<105;++i)if(s.markers[i].address==s.free_stamp)return int(i);
  return -1;
}
inline int map_edit_kind(unsigned palette) {
  return palette<=5?int(palette)+27:palette<=10?int(palette)+31:palette<=13?int(palette)+22:-1;
}
} // namespace botw
