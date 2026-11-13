// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_quick.h"
#include "botw_ready_offsets.h"
namespace botw {
struct RuneSnapshot {
  bool valid{};
  std::array<int32_t,8> ids{};
  int32_t count{}, selected{}, highlighted{-1};
  bool bombs_plus{}, stasis_plus{};
  bool operator==(const RuneSnapshot&) const = default;
};
// Read the same sorted boolean flag bank used by the native Rune variant resolver.
// No guest calls; only the exact 1.6.0 FlagBool layout is accepted.
struct RuneFlag {bool valid{},value{};uint64_t address{};uint32_t hash{};int32_t index{};uint8_t properties{};bool operator==(const RuneFlag&) const = default;};
inline RuneFlag rune_flag(const EdenDsmodHostApi& h,uint32_t hash) {
  struct Roots {
    uint64_t manager{}, vtable{}, holder{}, bank{}, array{};
    int32_t count{}, capacity{};
    bool operator==(const Roots&) const = default;
  } a{},z{};
  auto roots=[&](Roots& r) {
    return read(h,h.main_base+inventory_offsets(h).gamedata_root,r.manager) && r.manager%8==0 &&
      r.manager<=UINT64_MAX-0xBE8 && read(h,r.manager,r.vtable) &&
      r.vtable==h.main_base+inventory_offsets(h).gamedata_vtable && read(h,r.manager+0xBE0,r.holder) && r.holder%8==0 &&
      read(h,r.holder,r.bank) && r.bank%8==0 && r.bank<=UINT64_MAX-24 &&
      read(h,r.bank+8,r.count) && read(h,r.bank+12,r.capacity) &&
      r.count>0 && r.count<=65536 && r.capacity>=r.count && r.capacity<=65536 &&
      read(h,r.bank+16,r.array) && r.array%8==0 && r.array<=UINT64_MAX-uint64_t(r.count)*8;
  };
  if (!ready_supported(h) || h.main_size<(inventory_offsets(h).gamedata_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h).gamedata_root+8) || !roots(a)) return {};
  struct Flag {
    uint64_t vtable{};
    uint8_t type{},properties{},config{},value{};
    uint32_t hash{};
    bool operator==(const Flag&) const = default;
  } f{},again{};
  static_assert(sizeof(Flag)==16);
  auto flag=[&](int32_t index,uint64_t& p,Flag& b) {
    return index>=0 && index<a.count && read(h,a.array+uint64_t(index)*8,p) &&
      p%8==0 && read(h,p,b) && b.vtable==h.main_base+inventory_offsets(h).bool_flag_vtable && b.type==0;
  };
  int32_t lo=0,hi=a.count; uint64_t p{};
  for (unsigned step=0;lo<hi && step<17;++step) {
    const auto mid=lo+(hi-lo)/2;
    if (!flag(mid,p,f)) return {};
    if (f.hash<hash) lo=mid+1; else hi=mid;
  }
  if (lo!=hi || !flag(lo,p,f) || f.hash!=hash) return {};
  uint64_t p2{};
  if (!flag(lo,p2,again) || p2!=p || again!=f || !roots(z) || a!=z) return {};
  return {true,(f.value&1)!=0,p,hash,lo,f.properties};
}
inline bool rune_upgrade(const EdenDsmodHostApi& h,uint32_t hash,bool& out) {
  const auto flag=rune_flag(h,hash);if(!flag.valid)return false;out=flag.value;return true;
}
inline RuneSnapshot runes(const EdenDsmodHostApi& h) {
  RuneSnapshot out;
  const auto q=native_quick(h);
  if (!q.valid || q.kind!=QuickKind::Rune) return out;
  UiSample u{},after{};
  if (!ui_sample(h,u)) return out;
  struct List {
    uint64_t screen{}, buffer{};
    int32_t capacity{},count{},selected{},highlighted{};
    bool operator==(const List&) const = default;
  } a{},z{};
  auto list=[&](List& l) {
    return read(h,u.array+45*8,l.screen) && l.screen<=UINT64_MAX-0x386C &&
      read(h,l.screen+0x3850,l.capacity) && l.capacity==8 &&
      read(h,l.screen+0x3858,l.buffer) && l.buffer%4==0 &&
      read(h,l.screen+0x3868,l.count) && l.count>0 && l.count<=8 &&
      read(h,l.screen+0x3860,l.selected) && l.selected>=0 && l.selected<l.count &&
      read(h,l.screen+0x3864,l.highlighted) && l.highlighted>=0 && l.highlighted<l.count;
  };
  std::array<int32_t,8> ids{},again{};
  if (!list(a) || !read(h,a.buffer,ids)) return out;
  uint32_t seen=0;
  for (int32_t i=0;i<a.count;++i) {
    if (ids[i]<0 || ids[i]>7 || (seen&(1u<<ids[i]))) return out;
    seen|=1u<<ids[i];
  }
  if (!rune_upgrade(h,0x604C3BAE,out.bombs_plus) ||
      !rune_upgrade(h,0x9CA3D1C1,out.stasis_plus) ||
      !read(h,a.buffer,again) || again!=ids || !list(z) || a!=z ||
      !ui_sample(h,after) || after!=u) return {};
  bool bombs_again{},stasis_again{};
  if (!rune_upgrade(h,0x604C3BAE,bombs_again) || !rune_upgrade(h,0x9CA3D1C1,stasis_again) ||
      bombs_again!=out.bombs_plus || stasis_again!=out.stasis_plus) return {};
  const auto last=native_quick(h);
  if (!last.valid || last.kind!=QuickKind::Rune) return {};
  out.ids=ids;out.count=a.count;out.selected=a.selected;out.highlighted=a.highlighted;out.valid=true;
  return out;
}
inline const char* rune_actor(int32_t id,bool bombs_plus,bool stasis_plus) {
  switch (id) {
    case 0:return bombs_plus ? "Obj_RemoteBombBall2" : "Obj_RemoteBombBall";
    case 1:return bombs_plus ? "Obj_RemoteBombCube2" : "Obj_RemoteBombCube";
    case 2:return "Obj_Magnetglove";
    case 3:return stasis_plus ? "Obj_StopTimerLv2" : "Obj_StopTimer";
    case 4:return "Obj_IceMaker";
    case 5:return "Obj_Camera";
    case 6:return "Obj_Motorcycle";
    case 7:return "Obj_AmiiboItem";
    default:return "";
  }
}
} // namespace botw
