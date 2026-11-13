// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
namespace botw {
struct MapPins {
  bool valid{};
  std::array<bool,5> active{};
  std::array<std::array<float,3>,5> positions{};
};
inline MapPins map_pins(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};
  if(h.main_size<(inventory_offsets(h)->screen_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h)->screen_root+8) ||
     !ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h))return {};
  struct Root {
    uint64_t object{},vtable{},array{};int32_t count{},capacity{};
    bool operator==(const Root&) const = default;
  } root{},last{};
  auto read_root=[&](Root& r) {
    return read(h,h.main_base+map_offsets(h)->root,r.object) && r.object && r.object%8==0 &&
      r.object<=UINT64_MAX-0x668 && read(h,r.object,r.vtable) && r.vtable==h.main_base+map_offsets(h)->vtable &&
      read(h,r.object+0x658,r.count) && read(h,r.object+0x65C,r.capacity) && r.count==5 && r.capacity==5 &&
      read(h,r.object+0x660,r.array) && r.array && r.array%8==0 && r.array<=UINT64_MAX-40;
  };
  if(!read_root(root))return {};
  struct Pin {
    uint64_t pointer{},vtable{},backing{};int32_t kind{},slot{};
    std::array<float,3> xyz{};
    bool operator==(const Pin&) const = default;
  };
  std::array<Pin,5> pins{};MapPins out;
  auto read_pin=[&](unsigned i,Pin& p) {
    if(!read(h,root.array+i*8,p.pointer) || !p.pointer || p.pointer%8 || p.pointer>UINT64_MAX-0x48 ||
       !read(h,p.pointer,p.vtable) || p.vtable!=h.main_base+map_offsets(h)->pin_vtable ||
       !read(h,p.pointer+8,p.backing) || !read(h,p.pointer+0x40,p.slot) || p.slot!=int32_t(i))return false;
    // Native deletion keeps old kind/coordinates. The backing record is the presence signal.
    if(!p.backing)return true;
    if(p.backing%8 || !h.is_mapped || !h.is_mapped(h.userdata,p.backing,1) ||
       !read(h,p.pointer+0x1C,p.kind) || p.kind!=37+int32_t(i) ||
       !read(h,p.pointer+0x28,p.xyz))return false;
    for(float v:p.xyz)if(!std::isfinite(v))return false;
    return std::abs(p.xyz[0])<=6000 && std::abs(p.xyz[1])<=10000 && std::abs(p.xyz[2])<=5000;
  };
  for(unsigned i=0;i<5;++i) {
    if(!read_pin(i,pins[i]))return {};
    for(unsigned j=0;j<i;++j)if(pins[i].pointer==pins[j].pointer ||
       (pins[i].backing && pins[i].backing==pins[j].backing))return {};
    out.active[i]=pins[i].backing!=0;out.positions[i]=pins[i].xyz;
  }
  for(unsigned i=0;i<5;++i){Pin again{};if(!read_pin(i,again) || again!=pins[i])return {};}
  if(!read_root(last) || last!=root || !main_field_stage(h) || !ui_sample(h,after) || ui!=after)return {};
  out.valid=true;return out;
}
inline void publish_map_pins(const EdenDsmodHostApi& h,const MapPins& pins,const MapViewCull* vcull=nullptr) {
  h.publish_i64(h.userdata,"botw.map.pins.valid",pins.valid);
  constexpr std::array<uint32_t,5> colors={0xFFFF6E5A,0xFF6E7DFF,0xFFF7CC4B,0xFF6BFF77,0xFFFF8CDD};
  constexpr const char* common="romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#";
  // Perf round 3: native_pins is a FullHyrule(Map)-only dynamic_marker
  // group (manifest-checked -- ReadyHyrule has no native_pins group), so vcull (only ever passed
  // on the Map page) is safe to apply unconditionally here -- user pins off the drawn view+margin
  // are still tracked (pins.active[i] is only 5 entries, independent of this) but not published.
  // Perf round 4: skip the whole slot (no kind/tint/text/x/y writes at
  // all) when inactive, same "continue on a missing/culled slot" treatment round 3 gave
  // koroks/places/landmarks -- verified from mod_ui_map_widget.cpp: a missing x or y key (never
  // written here for an inactive slot either way) already makes the dynamic_markers loop
  // `continue` before kind/tint/icon are even looked up, so kind/tint/text were pure writes with
  // no reader for an inactive slot.
  for(unsigned i=0;i<5;++i) {
    bool active=pins.valid&&pins.active[i];
    if(active&&vcull&&!WithinMapView(pins.positions[i][0],pins.positions[i][2],*vcull))active=false;
    if(!active)continue;
    Key key("botw.map.pins.",i);
    h.publish_i64(h.userdata,key(".kind"),int64_t(i));
    h.publish_i64(h.userdata,key(".tint"),colors[i]);
    if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),pins.positions[i][0]);h.publish_f64(h.userdata,key(".y"),-pins.positions[i][2]);}
    const std::string stem=std::string(common)+"Nt_MapPinNew"+(i?std::to_string(i+1):"");
    h.publish_text(h.userdata,key(".core"),(stem+"FL_00^t").c_str());
    h.publish_text(h.userdata,key(".halo"),(stem+"F_01^t").c_str());
    h.publish_text(h.userdata,key(".inner"),(std::string(common)+"Nt_MapMapPinIcon"+std::to_string(i+1)+"_00^s").c_str());
  }
}
} // namespace botw
