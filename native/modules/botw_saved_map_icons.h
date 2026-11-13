// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
#include <set>
namespace botw {
struct SavedMapIcons {
  struct Icon { int32_t kind{-1}; std::array<float,3> xyz{}; };
  bool valid{};
  std::array<Icon,200> icons{};
};
// Native MapData restores icons from these two GameData arrays at 10AFE40..10B025C.
// Display attachments in MapData are not the persistent save records.
inline SavedMapIcons saved_map_icons(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};
  if(h.main_size<(inventory_offsets(h).gamedata_root+8) || h.main_base>UINT64_MAX-(inventory_offsets(h).gamedata_root+8) ||
     !ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h))return {};
  auto ptr=[](uint64_t p,uint64_t size){return p && !(p%8) && p<=UINT64_MAX-size;};
  struct Group {
    uint32_t handle{}; int32_t groups{},capacity{},count{},cap{};
    uint64_t array{},group{},entries{};
    bool operator==(const Group&) const = default;
  };
  struct Root {
    uint64_t manager{},vtable{},holder{},bank{};uint32_t flags{},epoch{};
    std::array<Group,2> groups{};
    bool operator==(const Root&) const = default;
  } root{},last{};
  auto roots=[&](Root& r) {
    if(!read(h,h.main_base+inventory_offsets(h).gamedata_root,r.manager) || !ptr(r.manager,0xC2C) ||
       !read(h,r.manager,r.vtable) || r.vtable!=h.main_base+inventory_offsets(h).gamedata_vtable ||
       !read(h,r.manager+0xBE0,r.holder) || !ptr(r.holder,8) ||
       !read(h,r.holder,r.bank) || !ptr(r.bank,0x118) ||
       !read(h,r.manager+0xC18,r.flags) || !read(h,r.manager+0xC28,r.epoch))return false;
    for(unsigned i=0;i<2;++i) {
      auto& g=r.groups[i];const uint64_t off=i?0x108:0xA8;
      if(!read(h,h.main_base+map_offsets(h).save_handles+i*4,g.handle) || g.handle==UINT32_MAX ||
         ((r.flags&0x8000) && (g.handle>>24)!=r.epoch))return false;
      const uint32_t index=(r.flags&0x8000)?g.handle&0xFFFFFF:g.handle;
      if(!read(h,r.bank+off,g.groups) || !read(h,r.bank+off+4,g.capacity) ||
         g.groups<=0 || g.groups>65536 || g.capacity<g.groups || g.capacity>65536 || index>=uint32_t(g.groups) ||
         !read(h,r.bank+off+8,g.array) || !ptr(g.array,uint64_t(g.groups)*8) ||
         !read(h,g.array+index*8,g.group) || !ptr(g.group,16) ||
         !read(h,g.group,g.count) || !read(h,g.group+4,g.cap) || g.count!=200 || g.cap!=200 ||
         !read(h,g.group+8,g.entries) || !ptr(g.entries,1600))return false;
    }
    return true;
  };
  if(!roots(root))return {};
  struct Record {
    uint64_t no{},pos{},vtable{};uint8_t type{};int32_t kind{};uint32_t hash{};
    uint64_t pos_vtable{};uint8_t pos_type{};std::array<float,3> xyz{};uint32_t pos_hash{};
    bool operator==(const Record&) const = default;
  };
  std::array<Record,200> records{};std::set<uint64_t> pointers;SavedMapIcons out;
  auto record=[&](unsigned i,Record& r) {
    if(!read(h,root.groups[0].entries+i*8,r.no) || !ptr(r.no,0x20) ||
       !read(h,r.no,r.vtable) || r.vtable!=h.main_base+inventory_offsets(h).int_flag_vtable ||
       !read(h,r.no+8,r.type) || r.type!=10 || !read(h,r.no+0x18,r.kind) ||
       !read(h,r.no+0x1C,r.hash) || r.hash!=0x9383490E || r.kind < -1 || r.kind>44)return false;
    if(r.kind==-1)return true; // Deleted entries may retain stale position bytes.
    if(!read(h,root.groups[1].entries+i*8,r.pos) || !ptr(r.pos,0x40) ||
       !read(h,r.pos,r.pos_vtable) || r.pos_vtable!=h.main_base+map_offsets(h).position_flag_vtable ||
       !read(h,r.pos+8,r.pos_type) || r.pos_type!=16 ||
       !read(h,r.pos+0x30,r.xyz) || !read(h,r.pos+0x3C,r.pos_hash) || r.pos_hash!=0xEA9DEF3F)return false;
    for(float v:r.xyz)if(!std::isfinite(v))return false;
    return std::abs(r.xyz[0])<=6000 && std::abs(r.xyz[1])<=10000 && std::abs(r.xyz[2])<=5000;
  };
  for(unsigned i=0;i<200;++i) {
    auto& r=records[i];if(!record(i,r) || !pointers.insert(r.no).second ||
       (r.pos && !pointers.insert(r.pos).second))return {};
    out.icons[i]={r.kind,r.xyz};
  }
  for(unsigned i=0;i<200;++i){Record r{};if(!record(i,r) || r!=records[i])return {};}
  if(!roots(last) || root!=last || !main_field_stage(h) || !ui_sample(h,after) || ui!=after)return {};
  out.valid=true;return out;
}
inline void publish_map_stamps(const EdenDsmodHostApi& h,const SavedMapIcons& data,bool full=true,const MapViewCull* vcull=nullptr) {
  unsigned count{};
  for(const auto& icon:data.icons)if(icon.kind>=27 && icon.kind<=35)++count;
  const bool valid=data.valid && count<=100;
  h.publish_i64(h.userdata,"botw.map.stamps.valid",valid);
  h.publish_i64(h.userdata,"botw.map.stamps.count",valid?count:0);
  h.publish_i64(h.userdata,"botw.map.stamps.tint",0xFFECEC1D);
  // Perf: full=false (Ready -- it now gets these via the compact
  // botw.map.ready.marks.* list instead -- and any page with no native_stamps map widget) skips
  // the per-entry loop (up to 100 slots, each with a std::string build even to blank it);
  // .valid/.count/.tint stay unconditional, same shape every other per-entry publisher here uses.
  if(!full)return;
  unsigned slot{};
  // Perf round 4: the tail (inactive) slots used to write an explicit
  // kind=-1 + icon="" sentinel every tick. Verified from the runtime source
  // (mod_ui_map_widget.cpp's dynamic_markers loop): a slot's x/y keys are never written for an
  // inactive compacted slot (only active slots get `.x`/`.y` above), and that loop already
  // `continue`s on a missing x or y before it ever looks at kind/icon -- so the tail sentinel
  // writes were dead: no consumer ever reached them. Dropped entirely (no behaviour change --
  // the runtime's own per-tick snapshot clear already makes a slot's keys absent whenever this
  // publisher doesn't write them, same "no cross-tick persistence to lean on" rule round 3's
  // koroks/places/landmarks fix established).
  if(valid)for(const auto& icon:data.icons)if(icon.kind>=27 && icon.kind<=35 &&
      (!vcull||WithinMapView(icon.xyz[0],icon.xyz[2],*vcull))) {
    Key key("botw.map.stamps.",slot++);
    h.publish_i64(h.userdata,key(".kind"),icon.kind-27);
    if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),icon.xyz[0]);h.publish_f64(h.userdata,key(".y"),-icon.xyz[2]);}
    const std::string art="composite:native-map-stamp-"+std::to_string(icon.kind-27);
    h.publish_text(h.userdata,key(".icon"),art.c_str());
  }
}
} // namespace botw
