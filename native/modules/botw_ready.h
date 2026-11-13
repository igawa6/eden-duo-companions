// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_pouch.h"
#include "botw_weapon_metadata.h"
#include <algorithm>
#include <array>
namespace botw {
// Only native equipped flags determine Ready icons. Durability is shown as condition, never attack,
// and pouch order does not imply a previous/secondary item.
inline std::array<std::string,4> ready_equipment(const Snapshot& pouch) {
  std::array<std::string,4> out{};
  if (!pouch.valid) return out;
  std::array<unsigned,4> counts{};
  for (const auto& item:pouch.items) {
    if (!item.equipped || item.type<0 || item.type>3) continue;
    const auto index=static_cast<unsigned>(item.type);
    if (++counts[index]==1) out[index]=item.actor;
    else out[index].clear(); // Reject ambiguous equipped instances, even same actor.
  }
  return out;
}
// Condition of a weapon (0), bow (1) or shield (3) as 0-100 % of its ActorInfo generalLife.
// The pouch value holds life x100 at full condition (live: Hylian Shield 800 -> 79900). A native
// "Durability Up" modifier (flag 0x2; live flag layout confirmed by 0x80000001 = yellow Attack Up)
// raises the maximum by its value. The Master Sword recharges instead of breaking, so it has no bar.
// Edited saves can exceed the maximum; the result is clamped.
inline constexpr uint32_t ModifierDurabilityUp=0x2;
inline int durability_percent(const Item& item,uint32_t modifier_flags=0,uint32_t modifier_value=0) {
  if(item.type<0 || item.type>3 || item.type==2 || item.value<0)return -1;
  if(item.actor.rfind("Weapon_Sword_07",0)==0 && item.actor.size()==16 && item.actor[15]>='0' && item.actor[15]<='2')return -1;
  const auto* info=weapon_info(item.actor);
  if(!info || info->life<=0)return -1;
  const int64_t life=int64_t(info->life)+((modifier_flags&ModifierDurabilityUp)?int64_t(std::min<uint32_t>(modifier_value,100000)):0);
  return int(std::clamp<int64_t>(int64_t(item.value)/life,0,100));
}
// Native weapon modifier of a pouch instance (item+0x80 value, item+0x88 flags), read only after the
// same PouchItem vtable/name-layout checks the equip adapter uses.
struct GearModifier {bool valid{};uint32_t flags{},value{};};
inline GearModifier gear_modifier(const EdenDsmodHostApi& h,uint64_t address) {
  GearModifier out;uint64_t vt{},name{};
  if(!inventory_supported(h) || !address || address>UINT64_MAX-0x90 || !read(h,address+0x28,vt) || vt!=h.main_base+inventory_offsets(h).pouch_string_vtable ||
     !read(h,address+0x30,name) || name!=address+0x3C ||
     !read(h,address+0x88,out.flags) || !read(h,address+0x80,out.value))return {};
  out.valid=true;return out;
}
// Owned arrow stacks (type 2, quantity > 0) in native pouch order; returns the stack dir (+1/-1)
// away from the single equipped one, wrapping. No result with fewer than two kinds or an ambiguous equip.
inline const Item* arrow_step(const Snapshot& pouch,int dir,unsigned* kinds=nullptr) {
  if(kinds)*kinds=0;
  if(!pouch.valid || (dir!=1 && dir!=-1))return nullptr;
  std::array<const Item*,32> owned{};unsigned n{};int current=-1;
  for(const auto& i:pouch.items) {
    if(i.type!=2 || i.value<=0 || !i.address)continue;
    if(n==owned.size())return nullptr;
    if(i.equipped){if(current>=0)return nullptr;current=int(n);}
    owned[n++]=&i;
  }
  if(kinds)*kinds=n;
  if(n<2 || current<0)return nullptr;
  return owned[size_t((current+dir+int(n))%int(n))];
}
// Same unique-equipped rule as ready_equipment, returning the instances.
inline std::array<const Item*,4> ready_items(const Snapshot& pouch) {
  std::array<const Item*,4> out{};
  if (!pouch.valid) return out;
  std::array<unsigned,4> counts{};
  for (const auto& item:pouch.items) {
    if (!item.equipped || item.type<0 || item.type>3) continue;
    const auto index=static_cast<unsigned>(item.type);
    out[index]=++counts[index]==1?&item:nullptr;
  }
  return out;
}
} // namespace botw
