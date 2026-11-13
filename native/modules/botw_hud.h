// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_runes.h"
#include <cmath>
namespace botw {
struct CurrencySnapshot { bool valid{}; int32_t value{}; };
struct CurrentRune { bool valid{}; int32_t id{-1}; bool upgraded{}; };
struct WorldClock { bool valid{}; int32_t minutes{}; };
struct Stamina { bool valid{}; bool depleted{}; int32_t percent{}; };
struct Noise { bool valid{}; float level{}; int32_t percent{-1}; };
// These are display readers, never action-readiness or native availability gates.
inline bool hud_context(const UiSample& u) {
  return !u.slots[1] && u.slots[2] &&
    (u.flags==0x1E09000000ULL || u.flags==0x1E09000004ULL);
}
// Native HUD11897B0 reads UIManager+70;11897C4 scales by1/2 for its animation.
// This is a display value only, never an input/action readiness signal.
inline Noise noise(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return {};
  UiSample ui{},after{};float level{},again{};
  if (!ui_sample(h,ui) || !hud_context(ui) ||
      !read(h,ui.manager+0x70,level) || !std::isfinite(level) || level<0.f || level>2.f ||
      !read(h,ui.manager+0x70,again) || again!=level ||
      !ui_sample(h,after) || after!=ui) return {};
  return {true,level,int32_t(std::lround(level*50.f))};
}
inline CurrencySnapshot currency(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return {};
  UiSample ui{},after{};
  if (!ui_sample(h,ui) || !hud_context(ui)) return {};
  struct Roots {
    uint64_t manager{},vtable{},holder{},bank{},array{};
    int32_t count{},capacity{};
    bool operator==(const Roots&) const = default;
  } a{},z{};
  auto roots=[&](Roots& r) {
    return read(h,h.main_base+inventory_offsets(h)->gamedata_root,r.manager) && r.manager%8==0 &&
      r.manager<=UINT64_MAX-0xBE8 && read(h,r.manager,r.vtable) &&
      r.vtable==h.main_base+inventory_offsets(h)->gamedata_vtable && read(h,r.manager+0xBE0,r.holder) && r.holder%8==0 &&
      read(h,r.holder,r.bank) && r.bank%8==0 && r.bank<=UINT64_MAX-0x28 &&
      read(h,r.bank+0x18,r.count) && read(h,r.bank+0x1C,r.capacity) &&
      r.count>0 && r.count<=65536 && r.capacity>=r.count && r.capacity<=65536 &&
      read(h,r.bank+0x20,r.array) && r.array%8==0 && r.array<=UINT64_MAX-uint64_t(r.count)*8;
  };
  if (h.main_size<(inventory_offsets(h)->gamedata_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h)->gamedata_root+8) || !roots(a)) return {};
  struct Flag {
    uint64_t vtable{};
    uint8_t type{},properties{};
    std::array<uint8_t,2> pad{};
    int32_t initial{},minimum{},maximum{},value{};
    uint32_t hash{};
    uint64_t debug{};
    bool operator==(const Flag&) const = default;
  } f{},again{};
  static_assert(sizeof(Flag)==40);
  auto flag=[&](int32_t i,uint64_t& p,Flag& v) {
    return i>=0 && i<a.count && read(h,a.array+uint64_t(i)*8,p) && p%8==0 &&
      read(h,p,v) && v.vtable==h.main_base+inventory_offsets(h)->int_flag_vtable && v.type==1;
  };
  int32_t lo=0,hi=a.count;uint64_t p{},p2{};
  for (unsigned step=0;lo<hi && step<17;++step) {
    const auto mid=lo+(hi-lo)/2;
    if (!flag(mid,p,f)) return {};
    if (f.hash<0x23149BF8u) lo=mid+1;else hi=mid;
  }
  if (lo!=hi || !flag(lo,p,f) || f.hash!=0x23149BF8u ||
      f.minimum!=0 || f.maximum!=999999 || f.value<0 || f.value>f.maximum ||
      !flag(lo,p2,again) || p2!=p || again!=f || !roots(z) || z!=a ||
      !ui_sample(h,after) || after!=ui) return {};
  return {true,f.value};
}
inline CurrentRune current_rune(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return {};
  UiSample ui{},after{};
  if (!ui_sample(h,ui) || !hud_context(ui)) return {};
  struct Player {
    uint64_t pointer{},vtable{};int32_t id{};
    bool operator==(const Player&) const = default;
  } a{},z{};
  auto player=[&](Player& p) {
    return read(h,h.main_base+ready_offsets(h)->rune_root,p.pointer) && p.pointer%8==0 &&
      p.pointer<=UINT64_MAX-0x244 && read(h,p.pointer,p.vtable) &&
      p.vtable==h.main_base+ready_offsets(h)->rune_vtable && read(h,p.pointer+0x240,p.id) && p.id>=0 && p.id<=7;
  };
  if (!player(a)) return {};
  bool upgraded{},again{};
  const auto hash=a.id<=1 ? 0x604C3BAEu : a.id==3 ? 0x9CA3D1C1u : 0u;
  if (hash && (!rune_upgrade(h,hash,upgraded) || !rune_upgrade(h,hash,again) || again!=upgraded)) return {};
  if (!player(z) || z!=a || !ui_sample(h,after) || after!=ui) return {};
  return {true,a.id,upgraded};
}
inline WorldClock world_clock(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return {};
  UiSample ui{},after{};
  if (!ui_sample(h,ui) || !hud_context(ui)) return {};
  struct Roots {
    uint64_t manager{},vtable{},holder{},bank{},array{};
    int32_t count{},capacity{};
    bool operator==(const Roots&) const = default;
  } a{},z{};
  auto roots=[&](Roots& r) {
    return read(h,h.main_base+inventory_offsets(h)->gamedata_root,r.manager) && r.manager%8==0 &&
      r.manager<=UINT64_MAX-0xBE8 && read(h,r.manager,r.vtable) &&
      r.vtable==h.main_base+inventory_offsets(h)->gamedata_vtable && read(h,r.manager+0xBE0,r.holder) && r.holder%8==0 &&
      read(h,r.holder,r.bank) && r.bank%8==0 && r.bank<=UINT64_MAX-0x38 &&
      read(h,r.bank+0x28,r.count) && read(h,r.bank+0x2C,r.capacity) &&
      r.count>0 && r.count<=65536 && r.capacity>=r.count && r.capacity<=65536 &&
      read(h,r.bank+0x30,r.array) && r.array%8==0 && r.array<=UINT64_MAX-uint64_t(r.count)*8;
  };
  if (h.main_size<(inventory_offsets(h)->gamedata_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h)->gamedata_root+8) || !roots(a)) return {};
  struct Flag {
    uint64_t vtable{};uint8_t type{},properties{};std::array<uint8_t,2> pad{};
    float initial{},minimum{},maximum{},value{};uint32_t hash{};uint64_t debug{};
    bool operator==(const Flag&) const = default;
  } f{},again{};
  static_assert(sizeof(Flag)==40);
  auto flag=[&](int32_t i,uint64_t& p,Flag& v) {
    return i>=0 && i<a.count && read(h,a.array+uint64_t(i)*8,p) && p%8==0 &&
      read(h,p,v) && v.vtable==h.main_base+ready_offsets(h)->float_flag_vtable && v.type==2;
  };
  int32_t lo=0,hi=a.count;uint64_t p{},p2{};
  for (unsigned step=0;lo<hi && step<17;++step) {
    const auto mid=lo+(hi-lo)/2;
    if (!flag(mid,p,f)) return {};
    if (f.hash<0x9C79A188u) lo=mid+1;else hi=mid;
  }
  if (lo!=hi || !flag(lo,p,f) || f.hash!=0x9C79A188u ||
      f.minimum!=0.f || f.maximum!=360.f || !std::isfinite(f.value) ||
      f.value<0.f || f.value>360.f || !flag(lo,p2,again) || p2!=p || again!=f ||
      !roots(z) || z!=a || !ui_sample(h,after) || after!=ui) return {};
  // Native world time is degrees: 15 per hour. HUD clock uses five-minute steps.
  return {true,(int32_t(f.value*4.f)/5*5)%1440};
}
inline Stamina stamina(const EdenDsmodHostApi& h) {
  if(!ready_supported(h))return {};
  UiSample ui{},after{};
  if (!ui_sample(h,ui) || !hud_context(ui) || ui.flags!=0x1E09000000ULL) return {};
  struct Player {
    uint64_t info{},info_vtable{},actor{},actor_vtable{};
    float current{},maximum{},actor_maximum{};
    bool operator==(const Player&) const = default;
  } a{},z{};
  auto player=[&](Player& p) {
    return read(h,h.main_base+inventory_offsets(h)->player_info_root,p.info) && p.info%8==0 && p.info<=UINT64_MAX-0xAC &&
      read(h,p.info,p.info_vtable) && p.info_vtable==h.main_base+inventory_offsets(h)->player_info_vtable &&
      read(h,p.info+0x60,p.actor) && p.actor%8==0 && p.actor<=UINT64_MAX-0x1870 &&
      read(h,p.actor,p.actor_vtable) && p.actor_vtable==h.main_base+inventory_offsets(h)->player_vtable &&
      read(h,p.info+0xA4,p.current) && read(h,p.info+0xA8,p.maximum) &&
      read(h,p.actor+0x186C,p.actor_maximum) &&
      std::isfinite(p.current) && std::isfinite(p.maximum) &&
      p.maximum>=1000.f && p.maximum<=3000.f && p.actor_maximum==p.maximum &&
      p.current>=0.f && p.current<=p.maximum;
  };
  if (!player(a) || !player(z) || a!=z || !ui_sample(h,after) || after!=ui) return {};
  return {true,a.current<a.maximum-0.001f,int32_t(a.current/a.maximum*100.f)};
}
} // namespace botw
