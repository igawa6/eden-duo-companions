// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_map.h"
namespace botw {
struct TemperatureDial {bool valid{};float frame{};};
// Read the native HUD's applied Pos animation; this retains native easing and
// effective-temperature handling rather than estimating a position from Celsius.
inline TemperatureDial temperature_dial(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};
  if(!ready_supported(h) || !ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h))return {};
  struct Reading {
    uint64_t hud{},hud_vt{},pane{},pane_vt{},animation{},animation_vt{};float frame{};
    bool operator==(const Reading&)const=default;
  } a{},z{};
  auto get=[&](Reading& r) {
    return ui.array<=UINT64_MAX-37*8 && read(h,ui.array+37*8,r.hud) && r.hud && r.hud%8==0 && r.hud<=UINT64_MAX-(0x36E0+ready_offsets(h)->pane_shift) &&
      read(h,r.hud,r.hud_vt) && r.hud_vt==h.main_base+ready_offsets(h)->hud_vtable &&
      read(h,r.hud+(0x36D8+ready_offsets(h)->pane_shift),r.pane) && r.pane && r.pane%8==0 && r.pane<=UINT64_MAX-0x138 &&
      read(h,r.pane,r.pane_vt) && r.pane_vt==h.main_base+ready_offsets(h)->temperature_pane_vtable &&
      read(h,r.pane+0x130,r.animation) && r.animation && r.animation%8==0 && r.animation<=UINT64_MAX-0x24 &&
      read(h,r.animation,r.animation_vt) && r.animation_vt==h.main_base+ready_offsets(h)->animator_vtable &&
      read(h,r.animation+0x20,r.frame) && std::isfinite(r.frame) && r.frame>=0 && r.frame<=120;
  };
  if(!get(a) || !get(z) || a!=z || !main_field_stage(h) || !ui_sample(h,after) || after!=ui)return {};
  return {true,a.frame};
}
inline int temperature_rotation(const TemperatureDial& s) {
  // Native PaTempMeter_00_Pos FLPA target5 Hermite:22,-114,slope3;
  //98,114,slope3. Runtime screen rotation has the opposite sign to BFLYT.
  return s.valid ? int(std::lround(-std::clamp((s.frame-22.f)*3.f-114.f,-114.f,114.f))) : 0;
}
inline void publish_temperature_dial(const EdenDsmodHostApi& h,const TemperatureDial& s) {
  h.publish_i64(h.userdata,"botw.ready.temperature.dial.valid",s.valid);
  h.publish_i64(h.userdata,"botw.ready.temperature.dial.rotation",temperature_rotation(s));
  if(h.publish_f64)h.publish_f64(h.userdata,"botw.ready.temperature.dial.frame",s.valid?s.frame:-1);
}
}
