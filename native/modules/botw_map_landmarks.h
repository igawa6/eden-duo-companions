// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
#include "botw_quest_offsets.h"
namespace botw {
struct MapLandmarks {
  struct Entry {
    bool visible{},opened{},cleared{};
    int32_t kind{};
    std::array<float,3> xyz{};
  };
  bool valid{};
  unsigned count{};
  std::array<Entry,151> entries{};
};
inline MapLandmarks map_landmarks(const EdenDsmodHostApi& h) {
  UiSample ui{},after{};
  if(!quests_supported(h) || h.main_size<ready_offsets(h).map_root+8 || h.main_base>UINT64_MAX-(ready_offsets(h).map_root+8) ||
     !ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h))return {};
  struct Root {
    uint64_t object{},vtable{};
    std::array<uint64_t,2> arrays{};
    std::array<int32_t,2> counts{},capacities{};
    bool operator==(const Root&) const = default;
  } root{},last{};
  auto read_root=[&](Root& r) {
    if(!read(h,h.main_base+ready_offsets(h).map_root,r.object) || !r.object || r.object%8 ||
       r.object>UINT64_MAX-0x320 || !read(h,r.object,r.vtable) || r.vtable!=h.main_base+ready_offsets(h).map_vtable)return false;
    for(unsigned a=0;a<2;++a) {
      const uint64_t off=a?0x310:0x2F0;const int limit=a?15:136;
      if(!read(h,r.object+off,r.counts[a]) || !read(h,r.object+off+4,r.capacities[a]) ||
         r.counts[a]<0 || r.counts[a]>r.capacities[a] || r.capacities[a]>limit ||
         !read(h,r.object+off+8,r.arrays[a]) || !r.arrays[a] || r.arrays[a]%8 ||
         r.arrays[a]>UINT64_MAX-uint64_t(limit)*8)return false;
    }
    return r.counts[0]>=120 && r.counts[1]==15;
  };
  if(!read_root(root))return {};
  struct Record {
    uint64_t pointer{},vtable{},source{};
    int32_t kind{},seen{},cached{};
    uint32_t progress{};uint8_t tower_open{};
    std::array<float,3> xyz{};
    bool operator==(const Record&) const = default;
  };
  std::array<Record,151> records{};MapLandmarks out;
  auto read_record=[&](unsigned a,unsigned i,Record& r) {
    if(!read(h,root.arrays[a]+i*8,r.pointer) || !r.pointer || r.pointer%8 ||
       r.pointer>UINT64_MAX-0x68 || !read(h,r.pointer,r.vtable) ||
       r.vtable!=h.main_base+(a?quest_offsets(h).tower_vtable:quest_offsets(h).shrine_vtable) ||
       !read(h,r.pointer+0x1C,r.kind) || r.kind!=int32_t(a+2) ||
       !read(h,r.pointer+0x28,r.xyz) || !read(h,r.pointer+0x58,r.source) ||
       !r.source || r.source%4 || r.source>UINT64_MAX-0x20 ||
       !read(h,r.source+0x1C,r.seen) || r.seen<0 ||
       !read(h,r.pointer+0x60,r.cached) || r.cached!=r.seen ||
       !read(h,r.pointer+0x3C,r.progress))return false;
    // Only persistent Open/Clear bits participate in consistency, not animated UI flags.
    r.progress&=0xA0000;
    if(a && (!read(h,r.pointer+0x64,r.tower_open) || r.tower_open>1))return false;
    for(float v:r.xyz)if(!std::isfinite(v))return false;
    return std::abs(r.xyz[0])<=6000 && std::abs(r.xyz[1])<=10000 && std::abs(r.xyz[2])<=5000;
  };
  unsigned n{};
  for(unsigned a=0;a<2;++a)for(unsigned i=0;i<unsigned(root.counts[a]);++i,++n) {
    auto& r=records[n];if(!read_record(a,i,r))return {};
    for(unsigned j=0;j<n;++j)if(records[j].pointer==r.pointer || records[j].source==r.source)return {};
    // Native refresh tests source+1C > 0. +8 is a lazily attached display record.
    out.entries[n]={r.seen>0,a?bool(r.tower_open):bool(r.progress&0x20000),
                    a?bool(r.tower_open):bool(r.progress&0x80000),r.kind,r.xyz};
  }
  n=0;
  for(unsigned a=0;a<2;++a)for(unsigned i=0;i<unsigned(root.counts[a]);++i,++n) {
    Record again{};if(!read_record(a,i,again) || again!=records[n])return {};
  }
  if(!read_root(last) || last!=root || !main_field_stage(h) || !ui_sample(h,after) || after!=ui)return {};
  out.count=n;out.valid=true;return out;
}
inline void publish_map_landmarks(const EdenDsmodHostApi& h,const MapLandmarks& map,bool full=true,const MapPosition* cull=nullptr,const MapViewCull* vcull=nullptr) {
  h.publish_i64(h.userdata,"botw.map.poi.valid",map.valid);
  unsigned visible{};
  constexpr const char* common="romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#";
  // Perf: full=false skips the per-entry loop off the Map page; `cull`
  // (Ready only) additionally treats an out-of-follow-window entry as inactive.
  // Perf round 3b: see botw_map_koroks.h -- a missing per-slot kind/x/y is treated identically
  // to an explicit kind=-1/x,y-omitted by the runtime's dynamic_marker slot loop, so an inactive
  // slot skips its Key construction and every publish_i64/text/f64 call entirely.
  if(full)for(unsigned i=0;i<map.entries.size();++i) {
    const auto& e=map.entries[i];bool active=map.valid && i<map.count && e.visible;
    if(active&&cull&&!WithinReadyWindow(e.xyz[0],e.xyz[2],*cull))active=false;
    if(active&&vcull&&!WithinMapView(e.xyz[0],e.xyz[2],*vcull))active=false;
    if(!active)continue;
    ++visible;
    Key key("botw.map.poi.",i);
    h.publish_i64(h.userdata,key(".kind"),e.kind);
    // Shrines and towers drawn at their own native sizes (two marker layers, one per class).
    const bool tower=e.kind==3;
    h.publish_i64(h.userdata,key(".kind_shrine"),!tower?e.kind:-1);
    h.publish_i64(h.userdata,key(".kind_tower"),tower?e.kind:-1);
    h.publish_i64(h.userdata,key(".border_tint"),e.opened?0xFF00BFFF:0xFFFF8A19);
    h.publish_i64(h.userdata,key(".core_tint"),e.cleared?0xFF00BFFF:0xFFFF8A19);
    for(const auto* part:{"core","border","halo"}) {
      const std::string art=std::string(common)+(tower?"Nt_MapTowerNew":"Nt_MapDungeonNew")+
        (std::strcmp(part,"core")==0?"_00^t":std::strcmp(part,"halo")==0?(tower?"_01^t":"_02^t"):(tower?"_02^t":"_01^t"));
      h.publish_text(h.userdata,key(".",part),art.c_str());
    }
    if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),e.xyz[0]);h.publish_f64(h.userdata,key(".y"),-e.xyz[2]);}
  }
  if(full)h.publish_i64(h.userdata,"botw.map.poi.count",visible);
}
} // namespace botw
