// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
#include "botw_map_korok_metadata.h"
#include <algorithm>
namespace botw {
struct MapKoroks {
  struct Entry {bool visible{};std::array<float,3> xyz{};bool found{},readable{};};
  bool valid{};std::array<Entry,900> entries{};
};
inline MapKoroks map_koroks(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};
  if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
  auto ptr=[](uint64_t p,uint64_t span,unsigned align=8){return p&&p%align==0&&p<=UINT64_MAX-span;};
  struct Roots {
    uint64_t map{},map_vt{},array{},manager{},manager_vt{},holder{},bank{},flags{};
    int32_t count{},cap{},flag_count{},flag_cap{};
    uint32_t mode{},generation{};uint8_t permission{};
    bool operator==(const Roots&)const=default;
  } root{},last{};
  auto roots=[&](Roots& r){
    return read(h,h.main_base+map_offsets(h).root,r.map)&&ptr(r.map,0x378)&&
      read(h,r.map,r.map_vt)&&r.map_vt==h.main_base+map_offsets(h).vtable&&
      read(h,r.map+0x360,r.count)&&read(h,r.map+0x364,r.cap)&&r.count==900&&r.cap==900&&
      read(h,r.map+0x368,r.array)&&ptr(r.array,900*8)&&
      read(h,h.main_base+inventory_offsets(h).gamedata_root,r.manager)&&ptr(r.manager,0xC2C)&&
      read(h,r.manager,r.manager_vt)&&r.manager_vt==h.main_base+inventory_offsets(h).gamedata_vtable&&
      read(h,r.manager+0xBE0,r.holder)&&ptr(r.holder,8)&&read(h,r.holder,r.bank)&&ptr(r.bank,24)&&
      read(h,r.bank+8,r.flag_count)&&read(h,r.bank+12,r.flag_cap)&&
      r.flag_count>0&&r.flag_count<=r.flag_cap&&r.flag_cap<=65536&&
      read(h,r.bank+16,r.flags)&&ptr(r.flags,uint64_t(r.flag_count)*8)&&
      read(h,r.manager+0xBE8,r.permission)&&r.permission<=1&&
      read(h,r.manager+0xC18,r.mode)&&read(h,r.manager+0xC28,r.generation)&&r.generation<=255;
  };
  if(!roots(root))return {};
  std::array<uint64_t,900> pointers{},again{};
  if(!read(h,root.array,pointers))return {};
  auto unique=[&](auto v){std::sort(v.begin(),v.end());return std::adjacent_find(v.begin(),v.end())==v.end();};
  if(!unique(pointers))return {};
  struct Flag {
    uint64_t vt{};uint8_t type{},properties{},config{},value{};uint32_t hash{};
    bool operator==(const Flag&)const=default;
  };
  static_assert(sizeof(Flag)==16);
  struct Record {
    uint64_t source{},name{},flag{};uint32_t handle{};std::array<float,3> xyz{};Flag data{};
    bool operator==(const Record&)const=default;
  };
  std::array<Record,900> records{};
  auto record=[&](unsigned i,Record& r){
    const auto p=pointers[i];uint64_t vt{};int32_t kind{};
    if(!ptr(p,0x60)||!read(h,p,vt)||vt!=h.main_base+map_offsets(h).korok_vtable||
       !read(h,p+0x1C,kind)||kind!=17||!read(h,p+0x28,r.xyz)||
       !read(h,p+0x50,r.source)||!ptr(r.source,12,4)||
       !read(h,r.source,r.name)||!ptr(r.name,1,1)||!read(h,r.source+8,r.handle)||r.handle==UINT32_MAX)return false;
    uint32_t index{};
    // Mirror native 104BD00: ordinary signed index, or packed generation/index mode.
    if(!(root.mode&0x8000)){if(r.handle>INT32_MAX)return false;index=r.handle;}
    else {if((r.handle>>24)!=root.generation)return false;index=r.handle&0xFFFFFF;}
    if(index>=uint32_t(root.flag_count)||!read(h,root.flags+uint64_t(index)*8,r.flag)||
       !ptr(r.flag,16)||!read(h,r.flag,r.data)||r.data.vt!=h.main_base+inventory_offsets(h).bool_flag_vtable||
       r.data.type||r.data.hash!=korok_flag_hashes[i])return false;
    for(float v:r.xyz)if(!std::isfinite(v))return false;
    return std::abs(r.xyz[0])<=6000&&std::abs(r.xyz[1])<=10000&&std::abs(r.xyz[2])<=5000;
  };
  MapKoroks out;std::array<uint64_t,900> sources{},flags{};
  for(unsigned i=0;i<900;++i){auto& r=records[i];if(!record(i,r))return {};
    sources[i]=r.source;flags[i]=r.flag;
    out.entries[i]={bool((r.data.value&1)&&(!root.permission||(r.data.properties&1))),r.xyz,bool(r.data.value&1),bool(!root.permission||(r.data.properties&1))};}
  if(!unique(sources)||!unique(flags))return {};
  for(unsigned i=0;i<900;++i){Record r{};if(!record(i,r)||r!=records[i])return {};}
  if(!read(h,root.array,again)||again!=pointers||!roots(last)||last!=root||
     !main_field_stage(h)||!ui_sample(h,after)||after!=ui)return {};
  out.valid=true;return out;
}
enum class KorokFilter { Found, Unfound, All };
inline bool korok_shown(const MapKoroks::Entry& entry,KorokFilter filter) {
  return filter==KorokFilter::Found?entry.visible:entry.readable && (filter==KorokFilter::All || !entry.found);
}
inline void publish_map_koroks(const EdenDsmodHostApi& h,const MapKoroks& v,KorokFilter filter=KorokFilter::Found,bool full=true,const MapPosition* cull=nullptr,const MapViewCull* vcull=nullptr) {
  h.publish_i64(h.userdata,"botw.map.koroks.valid",v.valid);unsigned count{};
  h.publish_text(h.userdata,"botw.map.koroks.icon",v.valid?
    "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_MapIconKolog^l":"");
  // Perf: full=false (any page but Map) skips the 900-entry loop -- none of
  // its per-entry keys are bound outside the Map page (manifest-checked); .valid/.icon/.count
  // (below) stay unconditional so every other reader keeps seeing a correct summary. `cull`
  // (Ready only -- Map's FullHyrule has no follow_window, so Map always passes nullptr) additionally
  // drops a slot outside the Ready circle's follow window: same per-slot "kind=-1, x/y omitted"
  // shape the runtime already treats as "not shown" for an out-of-filter/invalid entry, so the
  // circle's marker rendering is unaffected for anything that WOULD be visible inside it.
  // Perf round 3b (coordinator follow-up): the runtime's dynamic_marker
  // slot loop (mod_ui_map_widget.cpp) treats a MISSING per-slot kind/x/y exactly like an explicit
  // kind=-1 with x/y omitted -- confirmed from source: `position()` returns std::nullopt when
  // neither snapshot.floats nor .ints has the key (SnapshotNumber), and the slot loop `continue`s
  // on `!vx||!vy`; the kind lookup does `snapshot.ints.find(...)` and `continue`s on a miss, same
  // as an explicit hide_when_kind match. So an inactive slot can skip its Key-construction and
  // publish_i64/f64 calls ENTIRELY instead of still spending them on an explicit -1/0 -- same
  // drawn result, far fewer host map-inserts once most slots are culled out of view.
  if(full)for(unsigned i=0;i<900;++i){const auto& e=v.entries[i];bool visible=v.valid&&korok_shown(e,filter);
    if(visible&&cull&&!WithinReadyWindow(e.xyz[0],e.xyz[2],*cull))visible=false;
    if(visible&&vcull&&!WithinMapView(e.xyz[0],e.xyz[2],*vcull))visible=false;
    if(!visible)continue;
    ++count;
    Key key("botw.map.koroks.",i);
    h.publish_i64(h.userdata,key(".kind"),17);
    h.publish_i64(h.userdata,key(".tint"),e.found?0xFFF6DC9D:0xFF9DAEAB);
    if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),e.xyz[0]);h.publish_f64(h.userdata,key(".y"),-e.xyz[2]);}
  }
  h.publish_i64(h.userdata,"botw.map.koroks.count",count);
}
}
