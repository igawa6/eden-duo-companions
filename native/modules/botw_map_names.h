// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
#include "botw_map_name_metadata.h"
namespace botw {
struct MapNames {bool valid{};std::array<bool,map_name_info.size()> visible{};};
inline MapNames map_names(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
 UiSample ui{},after{};if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
 struct Root {uint64_t p{},vt{},array{};int32_t count{},cap{};uint32_t bits{};bool operator==(const Root&)const=default;} root{},last{};
 auto roots=[&](Root& r){return read(h,h.main_base+map_offsets(h).root,r.p)&&r.p&&r.p%8==0&&r.p<=UINT64_MAX-0x468&&read(h,r.p,r.vt)&&r.vt==h.main_base+map_offsets(h).vtable&&read(h,r.p+0x458,r.count)&&read(h,r.p+0x45C,r.cap)&&(r.count==1103||r.count==1105)&&r.cap>=r.count&&r.cap<=2048&&read(h,r.p+0x460,r.array)&&r.array&&r.array%8==0&&r.array<=UINT64_MAX-2048*8&&read(h,r.p+0x120,r.bits)&&!(r.bits&~0x7FFFu);};
 if(!roots(root))return {};
 struct Record {uint64_t p{},vt{},source{},other{},source_vt{},name{};int32_t kind{},seen{};uint32_t region{},show{};std::array<char,64> text{};std::array<float,3> xyz{};bool operator==(const Record&)const=default;};
 std::array<Record,map_name_info.size()> records{};MapNames out;
 auto record=[&](unsigned i,Record& r,unsigned shift){const auto& e=map_name_info[i];
  if(e.index+shift>=unsigned(root.count)||!read(h,root.array+(e.index+shift)*8,r.p)||!r.p||r.p%8||r.p>UINT64_MAX-0x60||!read(h,r.p,r.vt)||r.vt!=h.main_base+map_offsets(h).name_vtable||!read(h,r.p+0x1C,r.kind)||r.kind!=23||!read(h,r.p+0x28,r.xyz)||!read(h,r.p+0x44,r.region)||(r.region>=15&&r.region!=255)||
     !read(h,r.p+(e.discovered?0x58:0x50),r.source)||!r.source||r.source%4||r.source>UINT64_MAX-0x38||!read(h,r.p+(e.discovered?0x50:0x58),r.other)||r.other||!read(h,r.source,r.source_vt)||r.source_vt!=h.main_base+(e.discovered?map_offsets(h).discovered_source_vtable:map_offsets(h).label_source_vtable)||!read(h,r.source+8,r.show)||!(r.show&e.show)||r.show&~15u||!read(h,r.source+0x10,r.name)||!r.name||r.name>UINT64_MAX-64||!read(h,r.name,r.text)||e.key.size()>=r.text.size()||std::string_view(r.text.data(),e.key.size())!=e.key||r.text[e.key.size()]!='\0')return false;
  for(unsigned n=0;n<3;++n)if(!std::isfinite(r.xyz[n])||std::abs(r.xyz[n]-e.xyz[n])>.01f)return false;
  return !e.discovered||(read(h,r.source+0x34,r.seen)&&r.seen>=0);
 };
 // DLC (Master Trials/Champions' Ballad) inserts entries into the location array (1103 -> 1105);
 // later records move up by the number inserted before them. Catalog indices are base-game
 // order, so keep a running shift (never decreasing) bounded by the inserted count; every
 // candidate still has to pass the full name/position/source validation above.
 const unsigned max_shift=unsigned(root.count-1103);std::array<unsigned,map_name_info.size()> shifts{};unsigned cur=0;
 for(unsigned i=0;i<records.size();++i){auto& r=records[i];bool ok=false;for(unsigned d=cur;d<=max_shift&&!ok;++d)if(record(i,r,d)){ok=true;cur=d;}if(!ok)return {};shifts[i]=cur;for(unsigned j=0;j<i;++j)if(r.p==records[j].p)return {};out.visible[i]=(r.region==255||(root.bits&(1u<<r.region)))&&(!map_name_info[i].discovered||r.seen>0);}
 for(unsigned i=0;i<records.size();++i){Record again{};if(!record(i,again,shifts[i])||again!=records[i])return {};}
 if(!roots(last)||last!=root||!main_field_stage(h)||!ui_sample(h,after)||after!=ui)return {};
 out.valid=true;return out;
}
// Perf round 3: vcull (Map only) additionally hides a name whose
// static catalog position (map_name_info[i].xyz -- these are fixed area-name anchors, not
// moving entries, so the catalog position IS the live position) falls outside the current
// view+margin.
// Perf round 4: skip the publish_i64 entirely when a name is not shown,
// instead of writing an explicit 0 -- same GateOpen()-missing-key-is-false discipline labels got
// (botw_map_labels.h); this key IS the show_bind the manifest reads directly (no ".show" suffix,
// manifest-checked: "botw.map.names.N" appears only as its own entry's show_bind).
inline void publish_map_names(const EdenDsmodHostApi& h,const MapNames& names,bool full=true,const MapViewCull* vcull=nullptr){h.publish_i64(h.userdata,"botw.map.names.valid",names.valid);if(full)for(unsigned i=0;i<names.visible.size();++i){bool v=names.valid&&names.visible[i];if(v&&vcull&&!WithinMapView(map_name_info[i].xyz[0],map_name_info[i].xyz[2],*vcull))v=false;if(!v)continue;Key key("botw.map.names.",i);h.publish_i64(h.userdata,key.c_str(),v);}}
}
