// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_hud.h"
namespace botw {
struct Environment {
  bool valid{};
  float temperature{};
  std::array<int32_t,5> forecast{};
};
// Same native forecast slots consumed by 1.6.0's HUD at +111D9E4.
// The manual world override byte is deliberately not used as effective weather.
inline Environment environment(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};
  if (!ready_supported(h) || !ui_sample(h,ui) || !hud_context(ui) || h.main_size<(ready_offsets(h).world_root+8) ||
      h.main_base>UINT64_MAX-(ready_offsets(h).world_root+8)) return {};
  struct Sample {
    uint64_t world{},world_vtable{},array{},weather{},weather_vtable{},temp{},temp_vtable{};
    int32_t count{},capacity{},climate{};
    float temperature{},base{},instant{};
    std::array<uint8_t,5> forecast{};
    bool operator==(const Sample&) const = default;
  } a{},z{};
  auto sample=[&](Sample& s) {
    return read(h,h.main_base+ready_offsets(h).world_root,s.world) && s.world%8==0 && s.world<=UINT64_MAX-0x7E0 &&
      read(h,s.world,s.world_vtable) && s.world_vtable==h.main_base+ready_offsets(h).world_vtable &&
      read(h,s.world+0x5C0,s.count) && s.count==9 &&
      read(h,s.world+0x5C4,s.capacity) && s.capacity==9 &&
      read(h,s.world+0x5C8,s.array) && s.array%8==0 && s.array<=UINT64_MAX-72 &&
      read(h,s.world+0x780,s.climate) && s.climate>=0 && s.climate<20 &&
      read(h,s.array+24,s.weather) && s.weather%8==0 && s.weather<=UINT64_MAX-0x398 &&
      read(h,s.weather,s.weather_vtable) && s.weather_vtable==h.main_base+ready_offsets(h).weather_vtable &&
      read(h,s.weather+0x284,s.forecast) &&
      read(h,s.array+32,s.temp) && s.temp%8==0 && s.temp<=UINT64_MAX-0x90 &&
      read(h,s.temp,s.temp_vtable) && s.temp_vtable==h.main_base+ready_offsets(h).temperature_vtable &&
      read(h,s.temp+0x40,s.temperature) && read(h,s.temp+0x20,s.base) && read(h,s.temp+0x60,s.instant) &&
      std::isfinite(s.temperature) && s.temperature>=-100.f && s.temperature<=100.f &&
      std::isfinite(s.base) && std::isfinite(s.instant);
  };
  if (!sample(a)) return {};
  Environment out;
  constexpr int32_t frames[]={0,1,2,2,4,4,3,3,2};
  for (unsigned i=0;i<out.forecast.size();++i) {
    if(a.forecast[i]>8) return {};
    const auto frame=frames[a.forecast[i]];
    // Native HUD converts rain to snow when either native temperature is <= -2 C.
    out.forecast[i]=frame==2 && (a.base<=-2.f || a.instant<=-2.f) ? 4 : frame;
  }
  if (!sample(z) || a!=z || !ui_sample(h,after) || after!=ui) return {};
  out.valid=true;out.temperature=a.temperature;return out;
}
} // namespace botw
