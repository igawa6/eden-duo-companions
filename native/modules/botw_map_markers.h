// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_quests.h"
#include "botw_map_offsets.h"
#include <cmath>
namespace botw {
struct QuestMapMarkers {
  bool valid{};
  unsigned count{};
  std::array<std::array<float,3>,5> positions{};
};
inline QuestMapMarkers quest_map_markers(const EdenDsmodHostApi& h,const QuestSnapshot& journal) {
  UiSample ui{},after{};
  if(!map_build_supported(h) || !journal.valid || !ui_sample(h,ui) || !hud_context(ui) ||
     h.main_size<(quest_offsets(h).root+8) || h.main_base>UINT64_MAX-(quest_offsets(h).root+8))return {};
  const auto destination=quest_destination(h);if(!destination.valid)return {};
  struct Roots {
    uint64_t map{},vtable{},quest{},array{};int32_t count{},capacity{};
    bool operator==(const Roots&) const = default;
  } a{},z{};
  auto roots=[&](Roots& r) {
    return read(h,h.main_base+map_offsets(h).root,r.map) && r.map && r.map%8==0 && r.map<=UINT64_MAX-0x3C0 &&
      read(h,r.map,r.vtable) && r.vtable==h.main_base+map_offsets(h).vtable && read(h,r.map+0x3A0,r.quest) &&
      read(h,r.map+0x3B0,r.count) && read(h,r.map+0x3B4,r.capacity) &&
      r.count>=0 && r.capacity>=1 && r.count<=r.capacity && r.capacity<=5 &&
      read(h,r.map+0x3B8,r.array) && r.array && r.array%8==0 &&
      r.array<=UINT64_MAX-uint64_t(r.capacity)*8;
  };
  if(!roots(a))return {};
  // Established before QuestGuard so the guard can relax its step floor for Main quests
  // (revealed, and thus eligible to become "current", from step 0 -- see botw_quests.h).
  const Quest* current=nullptr;
  if(a.quest)
    for(const auto& entry:journal.entries)
      if(entry.address==a.quest && entry.index==destination.value && entry.active && entry.state==1)current=&entry;
  struct QuestGuard {
    uint64_t vtable{},manager{},manager_vtable{},array{},registered{};int32_t state{},step{},steps{},count{},capacity{};uint32_t dlc{};uint8_t journal{};
    bool operator==(const QuestGuard&) const = default;
  } q{},q_after{};
  const int32_t min_step=(current && current->category==0)?0:1;
  auto quest_guard=[&](QuestGuard& v) {
    return destination.value>=0 && read(h,h.main_base+quest_offsets(h).root,v.manager) && v.manager &&
      v.manager%8==0 && v.manager<=UINT64_MAX-0xC8 && read(h,v.manager,v.manager_vtable) &&
      v.manager_vtable==h.main_base+quest_offsets(h).manager_vtable && read(h,v.manager+0xB8,v.count) &&
      read(h,v.manager+0xBC,v.capacity) && v.count>0 && v.count<=v.capacity && v.capacity<=512 &&
      destination.value<v.count && read(h,v.manager+0xC0,v.array) && v.array%8==0 &&
      v.array<=UINT64_MAX-uint64_t(v.count)*8 && read(h,v.array+uint64_t(destination.value)*8,v.registered) &&
      v.registered==a.quest && a.quest%8==0 && a.quest<=UINT64_MAX-0x144 && read(h,a.quest,v.vtable) &&
      v.vtable==h.main_base+quest_offsets(h).quest_vtable && read(h,a.quest+12,v.state) && v.state==1 &&
      read(h,a.quest+0x30,v.journal) && v.journal==1 && read(h,a.quest+0x140,v.step) &&
      read(h,a.quest+0xA0,v.steps) && v.steps>=2 && v.steps<=64 && v.step>=min_step && v.step<v.steps &&
      read(h,a.quest+0xD8,v.dlc) && !(v.dlc&~7u);
  };
  if(!a.quest) {
    if(destination.value!=-1 || a.count)return {};
  } else {
    if(!current || !quest_guard(q) || q.dlc!=current->dlc)return {};
  }
  QuestMapMarkers out;out.count=unsigned(a.count);
  std::array<uint64_t,5> pointers{};
  for(unsigned i=0;i<out.count;++i) {
    auto& p=pointers[i];auto& xyz=out.positions[i];
    if(!read(h,a.array+i*8,p) || !p || p%4 || p>UINT64_MAX-12 ||
       std::find(pointers.begin(),pointers.begin()+i,p)!=pointers.begin()+i || !read(h,p,xyz))return {};
    for(float f:xyz)if(!std::isfinite(f))return {};
    if(std::abs(xyz[0])>6000 || std::abs(xyz[1])>10000 || std::abs(xyz[2])>5000)return {};
  }
  for(unsigned i=0;i<out.count;++i) {
    uint64_t p{};std::array<float,3> xyz{};
    if(!read(h,a.array+i*8,p) || p!=pointers[i] || !read(h,p,xyz) || xyz!=out.positions[i])return {};
  }
  const auto last=quest_destination(h);
  if(!roots(z) || a!=z || (a.quest && (!quest_guard(q_after) || q!=q_after)) ||
     !last.valid || destination.value!=last.value || !ui_sample(h,after) || ui!=after)return {};
  out.valid=true;return out;
}
} // namespace botw
