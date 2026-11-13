// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_map.h"
namespace botw {
struct MapArea {bool valid{};std::string key;};
inline MapArea map_area(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};
  if(h.main_size<(inventory_offsets(h).screen_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h).screen_root+8) ||
     !ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h))return {};
  struct Sample {
    uint64_t map{},map_vtable{},player{},vtable{},name{};int32_t kind{};
    std::array<float,3> xyz{};std::array<char,64> key{};
    bool operator==(const Sample&) const = default;
  } a{},z{};
  auto sample=[&](Sample& s) {
    if(!read(h,h.main_base+map_offsets(h).root,s.map) || !s.map || s.map%8 || s.map>UINT64_MAX-0x480 ||
       !read(h,s.map,s.map_vtable) || s.map_vtable!=h.main_base+map_offsets(h).vtable ||
       !read(h,s.map+0x478,s.player) || !s.player || s.player%8 || s.player>UINT64_MAX-0x50 ||
       !read(h,s.player,s.vtable) || s.vtable!=h.main_base+map_offsets(h).player_marker_vtable ||
       !read(h,s.player+0x1C,s.kind) || s.kind!=25 || !read(h,s.player+0x28,s.xyz) ||
       !read(h,s.player+0x48,s.name) || !s.name || s.name>UINT64_MAX-64 || !read(h,s.name,s.key))return false;
    for(float v:s.xyz)if(!std::isfinite(v))return false;
    if(std::abs(s.xyz[0])>6000 || std::abs(s.xyz[1])>10000 || std::abs(s.xyz[2])>5000)return false;
    const auto end=std::find(s.key.begin(),s.key.end(),'\0');
    if(end==s.key.begin() || end==s.key.end())return false;
    for(auto i=s.key.begin();i!=end;++i)if(!((*i>='A'&&*i<='Z')||(*i>='a'&&*i<='z')||(*i>='0'&&*i<='9')||*i=='_'))return false;
    return true;
  };
  if(!sample(a) || !sample(z) || a!=z || !main_field_stage(h) || !ui_sample(h,after) || ui!=after)return {};
  return {true,std::string(a.key.data())};
}
inline void publish_map_area(const EdenDsmodHostApi& h,const MapArea& area) {
  h.publish_i64(h.userdata,"botw.ready.area.valid",area.valid);
  uint32_t key=2166136261u;for(char c:area.key)key=(key^uint8_t(c))*16777619u;
  h.publish_i64(h.userdata,"botw.ready.area",area.valid?int64_t(key):-1);
  h.publish_text(h.userdata,"botw.ready.area.key",area.valid?area.key.c_str():"");
}
} // namespace botw
