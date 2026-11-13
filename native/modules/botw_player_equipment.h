// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_context.h"
#include <algorithm>
namespace botw {
struct AttachedEquipment {
  uint64_t actor{}, link_data{};
  uint32_t id{UINT32_MAX};
  std::string name;
  bool operator==(const AttachedEquipment&) const = default;
};
struct PlayerEquipmentSnapshot {
  bool valid{};
  uint64_t info{}, player{}, creator{};
  // Native creation slots: Sword, Shield, Bow, Head, Upper, Lower.
  std::array<AttachedEquipment,6> slots{};
  bool operator==(const PlayerEquipmentSnapshot&) const = default;
};
// Read actual attached actors, independently of native pouch equipped bits.
// Exact 1.6.0 offsets were observed through native Inventory's creation/handoff.
inline PlayerEquipmentSnapshot player_equipment(const EdenDsmodHostApi& h) {
  if (!inventory_supported(h) || h.main_size<(inventory_offsets(h).player_info_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h).player_info_root+8))
    return {};
  UiSample before{},after{};
  if (!ui_sample(h,before) || before.flags!=0x1E09000000ULL) return {};
  auto pointer=[](uint64_t p,uint64_t end){return p>=0x10000 && p%8==0 && p<=UINT64_MAX-end;};
  auto sample=[&](PlayerEquipmentSnapshot& s) {
    uint64_t vt{};
    if (!read(h,h.main_base+inventory_offsets(h).player_info_root,s.info) || !pointer(s.info,0x68) ||
        !read(h,s.info,vt) || vt!=h.main_base+inventory_offsets(h).player_info_vtable ||
        !read(h,s.info+0x60,s.player) || !pointer(s.player,0x2450) ||
        !read(h,s.player,vt) || vt!=h.main_base+inventory_offsets(h).player_vtable ||
        !read(h,h.main_base+inventory_offsets(h).creator_root,s.creator) || !pointer(s.creator,0x310) ||
        !read(h,s.creator,vt) || vt!=h.main_base+inventory_offsets(h).creator_vtable ||
        !read(h,s.player+0xB90,vt) || vt!=h.main_base+inventory_offsets(h).player_gear_vtable ||
        !read(h,s.player+0x23E0,vt) || vt!=h.main_base+inventory_offsets(h).player_armor_vtable) return false;
    for (unsigned i=0;i<6;++i) {
      const uint64_t at=s.player+(i<3 ? 0xB98+i*24 : 0x23F0+(i-3)*16);
      auto& e=s.slots[i];uint8_t acquired{};
      if (!read(h,at,e.link_data) || !read(h,at+8,e.id) ||
          !read(h,at+12,acquired) || acquired>1) return false;
      if (e.id==UINT32_MAX) {e.link_data=0;continue;}
      uint32_t data_id{},actor_id{};uint8_t state{};uint64_t name{},actor_link{};int32_t capacity{};
      // Iteration 14 item 6 (live 2026-10-23, broken bow): after a held weapon/bow/shield breaks the
      // native slot keeps its id while the link record is released (data_id UINT32_MAX, actor 0)
      // until the next native equip. That hand is empty, not an inconsistent snapshot: report it
      // with an empty name and actor 0 but the native id/link kept, which is exactly what the
      // guest's old-slot check (inventory-ui-hook.S: id, link, link+0x48 actor) compares against.
      if (pointer(e.link_data,0x58) && read(h,e.link_data+0x40,data_id) && data_id==UINT32_MAX &&
          read(h,e.link_data+0x48,e.actor) && e.actor==0) {e.name.clear();continue;}
      if (!pointer(e.link_data,0x58) || !read(h,e.link_data+0x40,data_id) || data_id!=e.id ||
          !read(h,e.link_data+0x48,e.actor) || !pointer(e.actor,0x80) ||
          !read(h,e.actor,vt) || vt!=h.main_base+(i<3 ? inventory_offsets(h).weapon_actor_vtable : inventory_offsets(h).armor_actor_vtable) ||
          !read(h,e.actor+0x10,name) || name!=e.actor+0x1C ||
          !read(h,e.actor+0x18,capacity) || capacity!=64 ||
          !read(h,e.actor+0x60,actor_id) || actor_id!=e.id ||
          !read(h,e.actor+0x64,state) || state!=1 ||
          !read(h,e.actor+0x68,actor_link) || actor_link!=e.link_data) return false;
      std::array<char,64> chars{};
      if (!read(h,name,chars)) return false;
      auto end=std::find(chars.begin(),chars.end(),'\0');
      if (end==chars.begin() || end==chars.end()) return false;
      for (auto it=chars.begin();it!=end;++it)
        if (!((*it>='A' && *it<='Z') || (*it>='a' && *it<='z') ||
              (*it>='0' && *it<='9') || *it=='_')) return false;
      e.name.assign(chars.begin(),end);
      for (unsigned j=0;j<i;++j) if (s.slots[j].actor==e.actor) return false;
    }
    return true;
  };
  PlayerEquipmentSnapshot a{},b{};
  if (!sample(a) || !sample(b) || a!=b || !ui_sample(h,after) || before!=after) return {};
  a.valid=true;return a;
}
} // namespace botw
