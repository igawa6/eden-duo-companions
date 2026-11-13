// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
namespace botw {
struct Sensor {
  bool valid{},enabled{};
  int32_t mode{};
  float frame{};
  // The game's own sensor state from the map record alone (obtained flag + mode). The HUD-pane
  // chain above can legitimately fail to validate while the companion re-poses the native HUD,
  // so availability (shrine compass gate) uses only these (owner Thor report 2026-10-23).
  bool map_valid{},map_enabled{};
  int32_t map_mode{2};
};
// Map-record-only read of the game's sensor (double-sampled, fail closed).
inline Sensor sensor_map_state(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return {};
  struct M {uint64_t map{},vt{};int32_t mode{};uint8_t enabled{};bool operator==(const M&) const=default;} a{},z{};
  auto get=[&](M& r){
    return read(h,h.main_base+ready_offsets(h).map_root,r.map) && r.map && r.map%8==0 && r.map<=UINT64_MAX-0x38AD &&
           read(h,r.map,r.vt) && r.vt==h.main_base+ready_offsets(h).map_vtable &&
           read(h,r.map+0x38A8,r.mode) && r.mode>=0 && r.mode<=2 &&
           read(h,r.map+0x38AC,r.enabled) && r.enabled<=1;
  };
  Sensor out{};
  if(!main_field_stage(h) || !get(a) || !get(z) || !(a==z))return out;
  out.map_valid=true;out.map_enabled=a.enabled;out.map_mode=a.mode;return out;
}
inline Sensor sensor(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};
  if(!ready_supported(h) || !ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h))return sensor_map_state(h);
  struct Reading {
    uint64_t map{},map_vt{},sensor{},sensor_vt{},hud{},hud_vt{},pane{},pane_vt{},animation{},animation_vt{};
    int32_t mode{};uint8_t enabled{};float frame{},applied{};
    bool operator==(const Reading&) const = default;
  } a{},z{};
  auto get=[&](Reading& r) {
    if(ui.array>UINT64_MAX-38*8 ||
       !read(h,h.main_base+ready_offsets(h).map_root,r.map) || !r.map || r.map%8 || r.map>UINT64_MAX-0x38AD ||
       !read(h,r.map,r.map_vt) || r.map_vt!=h.main_base+ready_offsets(h).map_vtable ||
       !read(h,r.map+0x38A8,r.mode) || r.mode<0 || r.mode>2 ||
       !read(h,r.map+0x38AC,r.enabled) || r.enabled>1 ||
       !read(h,h.main_base+ready_offsets(h).sensor_root,r.sensor) || !r.sensor || r.sensor%8 ||
       !read(h,r.sensor,r.sensor_vt) || r.sensor_vt!=h.main_base+ready_offsets(h).sensor_vtable ||
       !read(h,ui.array+37*8,r.hud) || !r.hud || r.hud%8 || r.hud>UINT64_MAX-(0x3D70+ready_offsets(h).pane_shift) ||
       !read(h,r.hud,r.hud_vt) || r.hud_vt!=h.main_base+ready_offsets(h).hud_vtable ||
       !read(h,r.hud+(0x3D68+ready_offsets(h).pane_shift),r.pane) || !r.pane || r.pane%8 || r.pane>UINT64_MAX-0x160 ||
       !read(h,r.pane,r.pane_vt) || r.pane_vt!=h.main_base+ready_offsets(h).sensor_pane_vtable ||
       !read(h,r.pane+0x144,r.frame) || !std::isfinite(r.frame) || r.frame<0 || r.frame>100 ||
       !read(h,r.pane+0x158,r.animation) || !r.animation || r.animation%8 || r.animation>UINT64_MAX-0x24 ||
       !read(h,r.animation,r.animation_vt) || r.animation_vt!=h.main_base+ready_offsets(h).animator_vtable ||
       !read(h,r.animation+0x20,r.applied) || !std::isfinite(r.applied) ||
       (r.enabled && r.mode!=2 && std::abs(r.applied-r.frame)>.01f))return false;
    return true;
  };
  if(!get(a) || !get(z) || a!=z || !main_field_stage(h) || !ui_sample(h,after) || after!=ui)return sensor_map_state(h);
  // Full reading validated: its own (double-sampled) map fields are the map state too.
  Sensor full{true,bool(a.enabled),a.mode,a.frame};
  full.map_valid=true;full.map_enabled=a.enabled;full.map_mode=a.mode;
  return full;
}
// The game's Sheikah Sensor is usable: obtained and switched on (mode 2 = off).
inline bool sensor_available(const Sensor& s){return s.map_valid&&s.map_enabled&&s.map_mode!=2;}
inline void publish_sensor(const EdenDsmodHostApi& h,const Sensor& s) {
  h.publish_i64(h.userdata,"botw.ready.sensor.valid",s.valid);
  h.publish_i64(h.userdata,"botw.ready.sensor.enabled",s.valid&&s.enabled&&s.mode!=2);
  h.publish_i64(h.userdata,"botw.ready.sensor.mode",s.valid?s.mode:-1);
  if(h.publish_f64)h.publish_f64(h.userdata,"botw.ready.sensor.frame",s.valid?s.frame:-1);
  h.publish_text(h.userdata,"botw.ready.sensor.text",!s.valid||!s.enabled?"Sensor":s.mode==0?"Shrine":s.mode==1?"Sensor +":"Off");
  // Native PaAppAllRadarGauge_00_TexPattern material white colors.
  const std::string art="romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_SeekSensorIcon"+std::string(s.valid&&s.mode==1?"P":"");
  for(unsigned i=0;i<4;++i)h.publish_text(h.userdata,Key("botw.ready.sensor.art.",i).c_str(),(art+"_0"+std::to_string(i)+"^s").c_str());
  const uint32_t rgb=s.mode==1?0x18C457:0x0096FF;
  h.publish_i64(h.userdata,"botw.ready.sensor.tint",s.valid&&s.enabled&&s.mode!=2?0xFF000000|rgb:0x809DAEAB);
  h.publish_i64(h.userdata,"botw.ready.sensor.palette",s.valid&&s.enabled&&s.mode!=2?s.mode:2);
  for(unsigned i=0;i<3;++i) {
    // Native Gauge2 ring ramps:50..70,65..85,80..100 (Hermite slopes12.75: linear).
    const float t=std::clamp((s.frame-(50.f+15.f*i))/20.f,0.f,1.f);
    const uint32_t alpha=s.valid&&s.enabled&&s.mode!=2?uint32_t(std::lround(t*255.f)):0;
    h.publish_i64(h.userdata,Key("botw.ready.sensor.ring_palette.",i).c_str(),alpha+(s.mode==1?256:0));
    h.publish_i64(h.userdata,Key("botw.ready.sensor.ring.",i).c_str(),(alpha<<24)|rgb);
  }
}
} // namespace botw
