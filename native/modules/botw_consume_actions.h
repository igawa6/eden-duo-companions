// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_inventory_actions.h"
#include "botw_consumables.h"
namespace botw {
struct InventoryConsume {
  bool valid{};
  Item item;
  Consumable food;
  PlayerEquipmentSnapshot equipment;
  UiSample ui;
  uint64_t manager{},screen{},column{},eat_root{},preview{};
  uint32_t count{},cache_column{},row{};
};
inline InventoryConsume inventory_consume(const EdenDsmodHostApi& h,uint64_t address,const Snapshot& display) {
  if(!inventory_supported(h) || !display.valid || context(h)!=UiContext::World)return {};
  InventoryConsume out;const auto fresh=pouch(h);if(!fresh.valid)return {};
  const Item *shown=nullptr,*target=nullptr;
  for(const auto& i:display.items)if(i.address==address)shown=&i;
  for(const auto& i:fresh.items)if(i.address==address)target=&i;
  if(!shown || !target || shown->actor!=target->actor || shown->type!=target->type ||
     shown->value!=target->value || shown->equipped!=target->equipped)return {};
  out.food=consumable(h,*target);if(!out.food.valid)return {};
  out.equipment=player_equipment(h);
  uint64_t vt{},manager{},array{},root{},ptr{},selected{};
  int32_t count{},offset{},first{},cells{},cell{},life{};uint8_t recursive{};
  auto pointer=[&](uint64_t p,uint64_t size){return p && p%8==0 && p<=UINT64_MAX-size;};
  if(!out.equipment.valid || !ui_sample(h,out.ui) || out.ui.flags!=0x1E09000000ULL ||
     !read(h,h.main_base+inventory_offsets(h).pouch_root,out.manager) || !pointer(out.manager,0x443C0) ||
     !read(h,out.manager,vt) || vt!=h.main_base+inventory_offsets(h).pouch_vtable ||
     !read(h,out.manager+0x78,count) || count<1 || count>420 ||
     !read(h,out.manager+0x7C,offset) || offset!=8 ||
     !read(h,out.manager+0x49,recursive) || recursive!=1 ||
     !read(h,h.main_base+inventory_offsets(h).eat_player_root,root) || !pointer(root,0x50) ||
     !read(h,root,vt) || vt!=h.main_base+inventory_offsets(h).eat_root_vtable ||
     !read(h,root+0x48,out.preview) || !pointer(out.preview,8) ||
     !read(h,out.preview,vt) || vt!=h.main_base+inventory_offsets(h).pause_player_vtable ||
     !read(h,out.equipment.player+0x848,life) || life<1 || life>120 ||
     !read(h,h.main_base+inventory_offsets(h).screen_root,manager) || !pointer(manager,0x38) ||
     !read(h,manager+0x28,cells) || cells!=99 ||
     !read(h,manager+0x30,array) || !pointer(array,99*8) ||
     !read(h,array+46*8,out.screen) || !pointer(out.screen,(0x3A10+inventory_offsets(h).pause_shift)) ||
     !read(h,out.screen,vt) || vt!=h.main_base+inventory_offsets(h).inventory_screen_vtable ||
     !read(h,out.screen+(0x3610+inventory_offsets(h).pause_shift),ptr) || ptr!=out.manager ||
     !read(h,out.screen+(0x3630+inventory_offsets(h).pause_shift),selected) || selected!=0 ||
     !read(h,out.screen+(0x3638+inventory_offsets(h).pause_shift),cell) || cell!=0 ||
     !read(h,out.screen+(0x3A08+inventory_offsets(h).pause_shift),out.column) || !pointer(out.column,0xCE0) ||
     !read(h,out.column,vt) || vt!=h.main_base+inventory_offsets(h).inventory_column_vtable ||
     !read(h,out.column+0x134,first) || first!=29 ||
     !read(h,out.column+0xCD0,cells) || cells!=20 ||
     !read(h,out.column+0xCD8,ptr) || !pointer(ptr,8) ||
     !read(h,out.column+0x140,ptr) || !pointer(ptr,0x28) ||
     !read(h,ptr+0x20,ptr) || !pointer(ptr,8))return {};
  for(unsigned i=0;i<6;++i) {int32_t state{};if(!read(h,out.equipment.creator+0xE8+i*0x68,state) || state!=0)return {};}
  // Native caches retain an exhausted item until the next native Inventory open.
  // Keep bounded raw list membership, including stock0 nodes, for cache traversal.
  std::array<uint64_t,420> members{};uint64_t node{},previous=out.manager+0x68;
  if(!read(h,out.manager+0x68,node))return {};
  for(int32_t i=0;i<count;++i) {
    if(!pointer(node,16) || node==out.manager+0x68)return {};
    uint64_t prev{},next{};
    if(!read(h,node+8,prev) || prev!=previous || !read(h,node,next))return {};
    members[size_t(i)]=node-8;previous=node;node=next;
  }
  if(node!=out.manager+0x68)return {};
  bool found=false;
  for(unsigned col=0;col<50 && !found;++col) {
    uint64_t node{};if(!read(h,out.manager+0x44230+col*8,node))return {};
    for(unsigned row=0;node && row<20;++row) {
      bool member=false;for(int32_t i=0;i<count;++i)if(members[size_t(i)]==node)member=true;
      int32_t type{};if(!member || !read(h,node+0x18,type) || type!=target->type)break;
      if(node==address){out.cache_column=col;out.row=row;found=true;break;}
      uint64_t prev{};if(!read(h,node+16,prev))return {};
      node=prev==out.manager+0x68 ? 0 : prev>=8 ? prev-8 : 0;
    }
  }
  if(!found)return {};
  const auto stable=pouch(h);const auto food=consumable(h,*target);UiSample after{};
  if(!stable.valid || stable.items.size()!=fresh.items.size() || !food.valid || food.cook!=out.food.cook ||
     food.use!=out.food.use || player_equipment(h)!=out.equipment || !ui_sample(h,after) || after!=out.ui)return {};
  for(size_t i=0;i<fresh.items.size();++i) {
    const auto &a=fresh.items[i],&b=stable.items[i];
    if(a.address!=b.address || a.actor!=b.actor || a.type!=b.type || a.value!=b.value || a.equipped!=b.equipped)return {};
  }
  out.eat_root=root;out.item=*target;out.count=uint32_t(count);out.valid=true;return out;
}
} // namespace botw
