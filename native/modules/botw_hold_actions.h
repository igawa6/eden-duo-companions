// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_hold_controller.h"
#include "botw_inventory_actions.h"
namespace botw {
struct InventoryHold {
  bool valid{},put_away{};Item item;HeldTray tray;HeldCarry carry;
  PlayerEquipmentSnapshot equipment;UiSample ui;
  uint64_t manager{},name{},info_data{},heap_root{},heap{};
  uint32_t count{},column{},index{},metadata80{},metadata88{};
};
// Fix round A: `why` receives the first failing check (diagnostic only; published by the module as
// botw.inventory.hold.refusal so a live decline can be attributed without a debugger).
inline InventoryHold inventory_hold(const EdenDsmodHostApi& h,uint64_t address,const Snapshot& display,bool put_away=false,uint32_t* why=nullptr){
  uint32_t step=0;auto at=[&](uint32_t v){step=v;if(why)*why=v;return true;};at(1);
  if(!inventory_supported(h)||!display.valid||context(h)!=UiContext::World)return {};
  at(2);InventoryHold out;out.put_away=put_away;out.tray=held_tray(h);out.carry=held_carry(h);
  at(out.tray.valid?(out.carry.valid?3:0x103):0x203);
  if(!out.tray.valid||!out.carry.valid||(put_away?!out.tray.count:out.tray.count>=5))return {};
  if(!put_away){
    if(out.tray.count){at(4);if(!out.carry.active()||out.carry.flags!=0x40)return {};at(5);
      for(const auto& slot:out.tray.slots)if(slot.address&&slot.flag8!=1)return {};
    }else if(at(6)&&!out.carry.empty())return {};
  }
  at(7);const auto fresh=pouch(h);if(!fresh.valid)return {};
  if(!put_away){const Item* shown{},*chosen{};
    for(const auto& i:display.items)if(i.address==address)shown=&i;
    for(const auto& i:fresh.items)if(i.address==address)chosen=&i;
    at(!shown?8:!chosen?0x108:!HoldTargets::same(*shown,*chosen)?0x208:0x308);
    if(!shown||!chosen||!HoldTargets::same(*shown,*chosen)||!holdable_material(*chosen))return {};
    out.item=*chosen;
  }
  at(9);out.equipment=player_equipment(h);out.manager=out.tray.manager;
  uint64_t vt{};int32_t count{},offset{};uint8_t recursive{};
  auto pointer=[](uint64_t p,uint64_t size){return p>=0x10000&&p%8==0&&p<=UINT64_MAX-size;};
  if(!out.equipment.valid||!ui_sample(h,out.ui)||out.ui.flags!=0x1E09000000ULL||
     !read(h,out.manager+0x78,count)||count<1||count>420||!read(h,out.manager+0x7C,offset)||offset!=8||
     !read(h,out.manager+0x49,recursive)||recursive!=1||
     !read(h,h.main_base+inventory_offsets(h)->info_data_root,out.info_data)||!pointer(out.info_data,8)||
     !read(h,out.info_data,vt)||vt!=h.main_base+inventory_offsets(h)->info_data_vtable)return {};
  at(10);for(unsigned i=0;i<6;++i){int32_t state{};if(!read(h,out.equipment.creator+0xE8+i*0x68,state)||state)return {};}
  out.count=uint32_t(count);
  if(!put_away){
    at(11);if(!read(h,address+0x30,out.name)||out.name!=address+0x3C||
       !read(h,address+0x80,out.metadata80)||!read(h,address+0x88,out.metadata88)||
       !read(h,h.main_base+inventory_offsets(h)->actor_heap_root,out.heap_root)||!pointer(out.heap_root,0x30)||
       !read(h,out.heap_root+0x28,out.heap)||!pointer(out.heap,8))return {};
    // Cached native rows include exhausted nodes omitted from the companion grid.
    at(12);std::array<uint64_t,420> members{};uint64_t node{},previous=out.manager+0x68;
    if(!read(h,out.manager+0x68,node))return {};
    for(int32_t i=0;i<count;++i){uint64_t prev{},next{};
      if(!pointer(node,16)||node==out.manager+0x68||!read(h,node+8,prev)||prev!=previous||!read(h,node,next))return {};
      members[size_t(i)]=node-8;previous=node;node=next;
    }
    if(node!=out.manager+0x68)return {};
    at(13);bool found=false;
    for(unsigned column=0;column<50&&!found;++column){
      if(!read(h,out.manager+0x44230+column*8,node))return {};
      for(unsigned index=0;node&&index<20;++index){
        bool member=false;for(int32_t i=0;i<count;++i)if(members[size_t(i)]==node)member=true;
        int32_t type{};if(!member||!read(h,node+0x18,type)||type!=7)break;
        if(node==address){out.column=column;out.index=index;found=true;break;}
        uint64_t prev{};if(!read(h,node+16,prev))return {};
        node=prev==out.manager+0x68?0:prev>=8?prev-8:0;
      }
    }
    at(14);if(!found)return {};
  }
  at(15);const auto stable=pouch(h);UiSample after{};uint64_t info{},heap_root{},heap{};
  if(!stable.valid||stable.items.size()!=fresh.items.size()||held_tray(h)!=out.tray||held_carry(h)!=out.carry||
     player_equipment(h)!=out.equipment||!ui_sample(h,after)||after!=out.ui||
     !read(h,h.main_base+inventory_offsets(h)->info_data_root,info)||info!=out.info_data)return {};
  at(16);if(!put_away&&(!read(h,h.main_base+inventory_offsets(h)->actor_heap_root,heap_root)||heap_root!=out.heap_root||!read(h,heap_root+0x28,heap)||heap!=out.heap))return {};
  for(size_t i=0;i<fresh.items.size();++i)if(!HoldTargets::same(fresh.items[i],stable.items[i]))return {};
  at(0);(void)step;out.valid=true;return out;
}
} // namespace botw
