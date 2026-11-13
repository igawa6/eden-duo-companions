// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_context.h"
#include "botw_inventory.h"
#include "botw_hold_metadata.h"
namespace botw {
inline bool holdable_material(const Item& item) {
  return item.type==7&&!item.equipped&&item.value>0&&item.value<=999999&&hold_stackable_actor(item.actor);
}
// Native pouch reservations, including exhausted items omitted by the display
// reader. This does not establish that spawned carry actors reached Link's hands.
struct HeldReservation {
  uint64_t address{};std::string actor;int32_t remaining{};
  uint8_t flag8{},flag9{},in_inventory{};
  bool operator==(const HeldReservation&)const=default;
};
struct HeldTray {
  bool valid{};uint64_t manager{};unsigned count{};
  std::array<HeldReservation,5> slots{};
  bool operator==(const HeldTray&)const=default;
};
struct HeldCarry {
  bool valid{};uint64_t root{},link{},request{},actor{},actor_vtable{};
  uint32_t generation{UINT32_MAX},flags{},actor_state{};uint8_t acquired{};
  bool operator==(const HeldCarry&)const=default;
  bool active()const{return valid&&generation!=UINT32_MAX&&actor!=0;}
  // Native B7C83C creates a new CarryBox directly for the invalid generation;
  // a retained creation record is not an active actor (B7C878 -> B7C940).
  // The raw retained pointer stays frozen. Its storage may already have been
  // recycled, so only settled inactive state bypasses its dereference/type test.
  bool empty()const{return valid&&generation==UINT32_MAX&&!actor&&!acquired&&!flags;}
};
inline HeldCarry held_carry(const EdenDsmodHostApi& h){
  UiSample before{},after{};
  if(!inventory_supported(h)||!ui_sample(h,before)||before.flags!=0x1E09000000ULL)return {};
  auto pointer=[](uint64_t p,uint64_t size){return p>=0x10000&&p%8==0&&p<=UINT64_MAX-size;};
  auto capture=[&](HeldCarry& v){uint64_t vt{};
    if(!read(h,h.main_base+inventory_offsets(h).carry_root,v.root)||!pointer(v.root,0xA7C)||!read(h,v.root,vt)||vt!=h.main_base+inventory_offsets(h).carry_vtable||
       !read(h,v.root+0x300,v.link)||!read(h,v.root+0x308,v.generation)||!read(h,v.root+0x30C,v.acquired)||v.acquired>1||
       !read(h,v.root+0x310,v.request)||!read(h,v.root+0xA78,v.flags)||(v.flags&~0x40u))return false;
    // B7C840 skips +310 when generation is invalid; B7C940 creates afresh.
    // Reclaimed storage at that retained address is irrelevant in a settled idle
    // state. Preserve exact type checks for active/busy states and raw-pointer
    // stability in every capture; never dereference an inactive dangling record.
    const bool idle=v.generation==UINT32_MAX&&!v.acquired&&!v.flags;
    if(v.request&&(!pointer(v.request,8)||(!idle&&(!read(h,v.request,vt)||vt!=h.main_base+inventory_offsets(h).carry_record_vtable))))return false;
    if(v.generation!=UINT32_MAX){uint32_t id{},actor_id{};uint64_t backlink{},name{};int32_t capacity{};std::array<char,9> actor_name{};
      if(!pointer(v.link,0x58)||!read(h,v.link+0x40,id)||id!=v.generation||!read(h,v.link+0x48,v.actor)||!pointer(v.actor,0x70)||
         !read(h,v.actor,v.actor_vtable)||v.actor_vtable!=h.main_base+inventory_offsets(h).carry_actor_vtable||
         !read(h,v.actor+0x60,actor_id)||actor_id!=v.generation||!read(h,v.actor+0x64,v.actor_state)||(v.actor_state&0xffu)!=1||
         !read(h,v.actor+0x68,backlink)||backlink!=v.link||!read(h,v.actor+0x10,name)||name!=v.actor+0x1C||
         !read(h,v.actor+0x18,capacity)||capacity!=64||!read(h,name,actor_name)||std::memcmp(actor_name.data(),"CarryBox",9))return false;
    }
    v.valid=true;return true;
  };
  HeldCarry first,last;
  if(!capture(first)||!capture(last)||first!=last||!ui_sample(h,after)||before!=after)return {};
  return first;
}
inline HeldTray held_tray(const EdenDsmodHostApi& h) {
  UiSample before{},after{};
  if(!inventory_supported(h) || !ui_sample(h,before) || !inventory_context(context(h)))return {};
  const auto first=pouch(h);if(!first.valid)return {};
  uint64_t manager{},vt{};
  if(!read(h,h.main_base+inventory_offsets(h).pouch_root,manager) || !manager || manager%8 ||
     manager>UINT64_MAX-0x444F0 || !read(h,manager,vt) || vt!=h.main_base+inventory_offsets(h).pouch_vtable)return {};
  // Capture all raw list links: reservations may retain a valid stock-zero node.
  struct Links {uint64_t item{},next{},prev{};bool operator==(const Links&)const=default;};
  auto members=[&](std::vector<Links>& out){
    int32_t count{},offset{};uint64_t next{},last{};
    if(!read(h,manager+0x78,count)||count<0||count>420||!read(h,manager+0x7C,offset)||offset!=8||
       !read(h,manager+0x68,next)||!read(h,manager+0x70,last))return false;
    uint64_t previous=manager+0x68;
    for(int32_t i=0;i<count;++i){
      if(next<0x10008||next%8||next==manager+0x68)return false;
      for(const auto& v:out)if(v.item==next-8)return false;
      Links v{next-8};if(!read(h,next,v.next)||!read(h,next+8,v.prev)||v.prev!=previous)return false;
      out.push_back(v);previous=next;next=v.next;
    }
    return next==manager+0x68 && previous==last;
  };
  std::vector<Links> links,again;if(!members(links))return {};
  auto capture=[&](HeldTray& out){
    out.manager=manager;
    for(unsigned i=0;i<5;++i){
      auto& v=out.slots[i];const auto p=manager+0x444A0+i*16;
      if(!read(h,p,v.address)||!read(h,p+8,v.flag8)||!read(h,p+9,v.flag9)||v.flag8>1||v.flag9>1)return false;
      if(!v.address)continue;
      bool member=false;for(const auto& link:links)if(link.item==v.address)member=true;
      if(!member || v.address%8 || v.address>UINT64_MAX-0x80)return false;
      int32_t type{},capacity{};uint8_t equipped{};uint64_t name{},name_vt{};
      if(!read(h,v.address+0x18,type)||type!=7||!read(h,v.address+0x20,v.remaining)||v.remaining<0||v.remaining>999999||
         !read(h,v.address+0x24,equipped)||equipped||!read(h,v.address+0x25,v.in_inventory)||v.in_inventory>1||
         !read(h,v.address+0x28,name_vt)||name_vt!=h.main_base+inventory_offsets(h).pouch_string_vtable||
         !read(h,v.address+0x30,name)||name!=v.address+0x3C||!read(h,v.address+0x38,capacity)||capacity!=64)return false;
      for(unsigned k=0;k<64;++k){uint8_t c{};if(!read(h,name+k,c))return false;if(!c)break;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;
        v.actor+=char(c);
      }
      if(v.actor.empty()||v.actor.size()==64||!hold_stackable_actor(v.actor))return false;
      ++out.count;
    }
    out.valid=true;return true;
  };
  HeldTray out,last;if(!capture(out)||!capture(last)||out!=last||!members(again)||links!=again)return {};
  const auto stable=pouch(h);uint64_t root{},end_vt{};
  if(!stable.valid||stable.items.size()!=first.items.size()||!read(h,h.main_base+inventory_offsets(h).pouch_root,root)||root!=manager||
     !read(h,manager,end_vt)||end_vt!=vt||!ui_sample(h,after)||before!=after||!inventory_context(context(h)))return {};
  for(size_t i=0;i<first.items.size();++i){const auto&a=first.items[i];const auto&b=stable.items[i];
    if(a.address!=b.address||a.actor!=b.actor||a.type!=b.type||a.value!=b.value||a.equipped!=b.equipped)return {};
  }
  return out;
}
// Owner item 12 (iteration 5, 2026-10-18): the Inventory hold tray -- and the detail-pane
// space it reclaims from the description -- is shown ONLY while something is actually held
// or a hold request is in flight. A previous revision also forced this true at rest while
// simply browsing the Materials category (an always-on empty-tray hint with five ghost
// slots), which ate the description's space for no reason; that category term is gone.
// A pure, named predicate (rather than an inline expression at each call site) so both
// publish sites agree and a regression is a one-line host test, not a live-capture diff.
inline bool hold_tray_shown(bool hold_valid,bool hold_supported,bool holding,bool hold_pending) {
  return hold_valid&&hold_supported&&(holding||hold_pending);
}
// Iteration 12 item 14: "Long press to hold item" shares the Inventory footer band with the status
// line ("Sorted", ...). Like the native guide bar it carries one message at a time: Materials only,
// never with the tray, and a fresh status result replaces it instead of drawing on top of it.
inline bool hold_hint_visible(bool hold_supported,int category,bool tray_shown,bool status_shown) {
  return hold_supported&&category==4&&!tray_shown&&!status_shown;
}
} // namespace botw
