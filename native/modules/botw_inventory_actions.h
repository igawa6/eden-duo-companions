// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_inventory.h"
#include "botw_player_equipment.h"
#include "botw_armor_fit.h"
namespace botw {
struct InventoryEquip {
  bool valid{},remove{};
  Item item;
  PlayerEquipmentSnapshot equipment;
  UiSample ui;
  ArmorFitPlan fit;
  uint64_t manager{},name{};
  uint32_t count{},slot{},column{},index{},modifier_flags{},modifier_value{},name_key{};
};
// Use the native autoEquip under its own pouch lock, then request the native
// equipment actor. Head/upper plans include native fitting and paired completion.
inline InventoryEquip inventory_equip(const EdenDsmodHostApi& h,uint64_t address,
                                     const Snapshot& display,bool lower_armor=false,bool all_armor=false,bool arrows=false,bool remove=false,bool remove_armor=false) {
  if (!inventory_supported(h) || !display.valid || context(h)!=UiContext::World) return {};
  const auto fresh=pouch(h);
  if (!fresh.valid) return {};
  const Item *shown=nullptr,*target=nullptr;
  for(const auto& i:display.items)if(i.address==address)shown=&i;
  for(const auto& i:fresh.items)if(i.address==address)target=&i;
  if(!shown || !target || shown->actor!=target->actor || shown->type!=target->type ||
     shown->value!=target->value || shown->equipped!=target->equipped ||
     (remove ? (!target->equipped || (target->type!=0 && target->type!=1 && target->type!=3 && !(remove_armor && target->type>=4 && target->type<=6))) : target->equipped) ||
     (((target->type==6 && lower_armor) || ((target->type==4 || target->type==5) && all_armor)) ? (target->value < -1 || target->value > 15) :
       (target->value<=0 || (target->type!=0 && target->type!=1 && target->type!=3 && !(arrows && target->type==2))))) return {};
  if(remove) {unsigned equipped=0;for(const auto& i:fresh.items)if(i.type==target->type && i.equipped)++equipped;if(equipped!=1)return {};}
  InventoryEquip out;
  out.equipment=player_equipment(h);
  if(!out.equipment.valid || !ui_sample(h,out.ui) || out.ui.flags!=0x1E09000000ULL) return {};
  uint64_t vt{};int32_t count{},offset{},capacity{};
  if(!read(h,h.main_base+inventory_offsets(h)->pouch_root,out.manager) || out.manager%8 ||
     out.manager>UINT64_MAX-0x80 || !read(h,out.manager,vt) || vt!=h.main_base+inventory_offsets(h)->pouch_vtable ||
     !read(h,out.manager+0x78,count) || count<1 || count>420 ||
     !read(h,out.manager+0x7C,offset) || offset!=8 ||
     address>UINT64_MAX-0x90 || !read(h,address+0x28,vt) || vt!=h.main_base+inventory_offsets(h)->pouch_string_vtable ||
     !read(h,address+0x30,out.name) || out.name!=address+0x3C ||
     !read(h,address+0x38,capacity) || capacity!=64 ||
     !read(h,address+0x88,out.modifier_flags) || !read(h,address+0x80,out.modifier_value)) return {};
  // Refuse competing native actor work, including an Inventory transition.
  for(unsigned i=0;i<6;++i) {
    int32_t state{};
    if(!read(h,out.equipment.creator+0xE8+i*0x68,state) || state!=0) return {};
  }
  if(target->type==4 || target->type==5) {
    Item candidate=*target;
    if(remove){candidate.actor=target->type==4 ? "Armor_Default_Head" : "Armor_Default_Upper";candidate.value=-1;candidate.equipped=false;candidate.address=0;}
    out.fit=armor_fit(h,fresh,out.equipment,candidate);if(!out.fit.valid)return {};
  }
  const auto equipment_after=player_equipment(h);
  UiSample after{};
  const auto stable=pouch(h);
  if(equipment_after!=out.equipment || !ui_sample(h,after) || after!=out.ui ||
     !stable.valid || stable.items.size()!=fresh.items.size()) return {};
  for(size_t i=0;i<fresh.items.size();++i) {
    const auto &a=fresh.items[i],&b=stable.items[i];
    if(a.address!=b.address || a.actor!=b.actor || a.type!=b.type ||
       a.value!=b.value || a.equipped!=b.equipped) return {};
  }
  out.item=*target;out.count=uint32_t(count);
  out.slot=target->type==2 ? 6 : target->type>=4 && target->type<=6 ? uint32_t(target->type-1) : target->type==0 ? 0 : target->type==3 ? 1 : 2;
  if(remove) {
    if(out.equipment.slots[out.slot].name!=target->actor && !(target->type==4 && out.fit.valid && out.equipment.slots[3].name==target->actor+"_B"))return {};
    uint8_t recursive{};
    if(!read(h,out.manager+0x49,recursive) || recursive!=1)return {};
    bool found=false;
    for(unsigned col=0;col<50 && !found;++col) {
      uint64_t node{};if(!read(h,out.manager+0x44230+col*8,node))return {};
      for(unsigned idx=0;node && idx<20;++idx) {
        const Item* item=nullptr;for(const auto& i:fresh.items)if(i.address==node)item=&i;
        if(!item || (target->type>=4 ? item->type<4 || item->type>6 : item->type!=target->type))break;
        if(node==address){out.column=col;out.index=idx;found=true;break;}
        uint64_t prev{};if(!read(h,node+16,prev))return {};
        node=prev==out.manager+0x68 ? 0 : prev>=8 ? prev-8 : 0;
      }
    }
    if(!found)return {};
    const auto& defaults=inventory_offsets(h)->defaults;
    const auto& names=inventory_offsets(h)->default_names;
    uint64_t vtable{},name{};
    if(!read(h,h.main_base+defaults[out.slot],vtable) || vtable!=h.main_base+inventory_offsets(h)->native_string_vtable ||
       !read(h,h.main_base+defaults[out.slot]+8,name) || name!=h.main_base+names[out.slot])return {};
    out.remove=true;
  }
  out.name_key=uint32_t(actor_key(target->actor));out.valid=true;return out;
}
} // namespace botw
