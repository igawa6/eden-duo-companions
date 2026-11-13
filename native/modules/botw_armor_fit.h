// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_armor_metadata.h"
#include "botw_player_equipment.h"
namespace botw {
struct ArmorFitPlan {
  bool valid{},needs_b{},refresh_head{};
  Item head,upper;
  std::string head_actor,target_actor;
  uint64_t info_data{};
};
// Native fitting applies to the head/upper pair. Lower armor never changes it.
// This plan is checked again against native InfoData on the game UI thread.
inline ArmorFitPlan armor_fit(const EdenDsmodHostApi& h,const Snapshot& pouch,
                             const PlayerEquipmentSnapshot& equipment,const Item& target) {
  if(!inventory_supported(h) || !pouch.valid || !equipment.valid || (target.type!=4 && target.type!=5) ||
     target.equipped || target.value < -1 || target.value > 15 ||
     !armor_info(target.actor))return {};
  ArmorFitPlan out;unsigned heads{},uppers{};
  for(const auto& i:pouch.items)if(i.equipped) {
    if(i.type==4){out.head=i;++heads;}
    if(i.type==5){out.upper=i;++uppers;}
  }
  if(heads>1 || uppers>1)return {};
  for(const auto* i:{&out.head,&out.upper})if(i->address &&
      (i->value < -1 || i->value > 15 || !armor_info(i->actor)))return {};
  const auto old_head=heads ? out.head.actor : "Armor_Default_Head";
  const auto old_upper=uppers ? out.upper.actor : "Armor_Default_Upper";
  if((equipment.slots[3].name!=old_head && equipment.slots[3].name!=old_head+"_B") ||
     equipment.slots[4].name!=old_upper)return {};
  const auto head_name=target.type==4 ? target.actor : old_head;
  const auto upper_name=target.type==5 ? target.actor : old_upper;
  const auto *head=armor_info(head_name),*upper=armor_info(upper_name);
  if(!head || !upper)return {};
  out.needs_b=head->head_mantle==2 && upper->upper_mantle!=0;
  out.head_actor=head_name+(out.needs_b ? "_B" : "");
  if(out.head_actor.size()>=64 || !armor_info(out.head_actor))return {};
  out.refresh_head=target.type==5 && out.head_actor!=equipment.slots[3].name;
  out.target_actor=target.type==4 ? out.head_actor : target.actor;
  uint64_t vt{},again{};
  if(!read(h,h.main_base+inventory_offsets(h).info_data_root,out.info_data) || out.info_data%8 ||
     out.info_data<0x10000 || out.info_data>UINT64_MAX-8 ||
     !read(h,out.info_data,vt) || vt!=h.main_base+inventory_offsets(h).info_data_vtable ||
     !read(h,h.main_base+inventory_offsets(h).info_data_root,again) || again!=out.info_data)return {};
  out.valid=true;return out;
}
} // namespace botw
