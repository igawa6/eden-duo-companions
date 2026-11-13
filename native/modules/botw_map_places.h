// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
namespace botw {
struct MapPlaces {
  struct Entry {bool visible{},opened{};int32_t kind{};std::array<float,3> xyz{};};
  bool valid{};unsigned count{};std::array<Entry,256> entries{};
};
inline uint64_t place_vtable(const EdenDsmodHostApi& h,int kind) {
  switch(kind){case 0:case 1:case 5:case 6:case 11:case 12:case 13:case 14:case 15:return map_offsets(h).place_vtable;
    case 2:return map_offsets(h).shrine_vtable;case 3:return map_offsets(h).tower_vtable;case 4:return map_offsets(h).start_vtable;
    case 7:case 8:case 9:case 10:return map_offsets(h).beast_vtable;case 16:return map_offsets(h).lab_vtable;
    // DLC (Master Trials) adds 16 kind-43 entries (EX treasure locations) to this list; known,
    // validated like every other record, never drawn (no place_art) -- before this the whole
    // list failed closed on any DLC install, hiding every stable/town/shop icon.
    case 43:return map_offsets(h).ex_vtable;default:return 0;}
}
inline const char* place_art(int kind) {
  switch(kind){case 0:return "Nt_MapCityIcon_00^t";case 1:return "Nt_MapHatago_00^t";
    case 4:return "Nt_MapStartPointNew_00^t";case 5:return "Nt_MapEtcIcon_00^t";
    case 6:return "Nt_MapCastle_00^t";case 11:return "Nt_YadoIcon_00^t";
    case 12:return "Nt_BouguIcon_00^t";case 13:return "Nt_YorozuIcon_00^t";
    case 14:return "Nt_ColorlIcon_00^t";case 15:return "Nt_JewelIcon_00^t";
    case 16:return "Nt_MapReserchCenter_00^t";default:return nullptr;}
}
// Iteration 12 item 6 (owner: every map icon the size of the player arrow): the place arts sit at
// very different fractions of their native textures (city 26/32, travel gate 30/48, research lab
// 37/64, ...), so one marker-group size could not draw them alike. The manifest (tools/i12-markers.py)
// defines one composite per kind that crops the native art to its visible bounds; both the Map
// and the Ready publishers name that composite, so every place draws at the group's pixel size.
inline std::string place_icon_src(int kind) {return "composite:botw-place-"+std::to_string(kind);}
inline MapPlaces map_places(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
  UiSample ui{},after{};if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
  struct Root {uint64_t object{},vt{},array{};int32_t count{},cap{};bool operator==(const Root&)const=default;} root{},last{};
  auto roots=[&](Root& r){return read(h,h.main_base+map_offsets(h).root,r.object)&&r.object&&r.object%8==0&&r.object<=UINT64_MAX-0x2F0&&
    read(h,r.object,r.vt)&&r.vt==h.main_base+map_offsets(h).vtable&&read(h,r.object+0x2E0,r.count)&&read(h,r.object+0x2E4,r.cap)&&
    r.count>=214&&r.count<=r.cap&&r.cap<=256&&read(h,r.object+0x2E8,r.array)&&r.array&&r.array%8==0&&r.array<=UINT64_MAX-256*8;};
  if(!roots(root))return {};
  struct Record {uint64_t p{},vt{},source{};int32_t kind{},seen{},cached{};uint32_t open{};std::array<float,3> xyz{};bool operator==(const Record&)const=default;};
  std::array<Record,256> records{};MapPlaces out;
  auto record=[&](unsigned i,Record& r){
    if(!read(h,root.array+i*8,r.p)||!r.p||r.p%8||r.p>UINT64_MAX-0x64||!read(h,r.p+0x1C,r.kind)||!place_vtable(h,r.kind)||
       !read(h,r.p,r.vt)||r.vt!=h.main_base+place_vtable(h,r.kind)||!read(h,r.p+0x28,r.xyz)||
       !read(h,r.p+0x58,r.source)||!r.source||r.source%4||r.source>UINT64_MAX-0x20||
       !read(h,r.source+0x1C,r.seen)||r.seen<0||!read(h,r.p+0x60,r.cached)||r.cached!=r.seen||!read(h,r.p+0x3C,r.open))return false;
    r.open&=0x20000;for(float v:r.xyz)if(!std::isfinite(v))return false;
    return std::abs(r.xyz[0])<=6000&&std::abs(r.xyz[1])<=10000&&std::abs(r.xyz[2])<=5000;
  };
  for(unsigned i=0;i<unsigned(root.count);++i){auto& r=records[i];if(!record(i,r))return {};
    for(unsigned j=0;j<i;++j)if(records[j].p==r.p||records[j].source==r.source)return {};
    out.entries[i]={r.seen>0&&place_art(r.kind),bool(r.open),r.kind,r.xyz};}
  for(unsigned i=0;i<unsigned(root.count);++i){Record again{};if(!record(i,again)||again!=records[i])return {};}
  if(!roots(last)||last!=root||!main_field_stage(h)||!ui_sample(h,after)||after!=ui)return {};
  out.count=root.count;out.valid=true;return out;
}
inline void publish_map_places(const EdenDsmodHostApi& h,const MapPlaces& places,bool full=true,const MapPosition* cull=nullptr,const MapViewCull* vcull=nullptr) {
  h.publish_i64(h.userdata,"botw.map.places.valid",places.valid);unsigned count{};
  // Item 17: one static, non-indexed backing-plate image (Pause Glass dark disc, file:
  // asset -- see prototype/dualscreen/assets/map-place-backing.png) that the manifest draws
  // under every town/stable icon for contrast against gold-brown terrain.
  h.publish_text(h.userdata,"botw.map.places.backing_icon","file:assets/map-place-backing.png");
  // Perf: full=false skips the per-entry loop off the Map page; `cull`
  // (Ready only) additionally treats an out-of-follow-window entry as inactive -- same shape as
  // any other already-hidden entry.
  // Perf round 3b: see botw_map_koroks.h -- a missing per-slot kind/x/y is treated identically
  // to an explicit kind=-1/x,y-omitted by the runtime's dynamic_marker slot loop, so an inactive
  // slot skips its Key construction and every publish_i64/text/f64 call entirely.
  if(full)for(unsigned i=0;i<places.entries.size();++i){const auto& e=places.entries[i];const char* art=place_art(e.kind);bool active=places.valid&&i<places.count&&e.visible&&art;
    if(active&&cull&&!WithinReadyWindow(e.xyz[0],e.xyz[2],*cull))active=false;
    if(active&&vcull&&!WithinMapView(e.xyz[0],e.xyz[2],*vcull))active=false;
    if(!active)continue;
    ++count;
    Key key("botw.map.places.",i);
    h.publish_i64(h.userdata,key(".kind"),e.kind);
    // Critique item C (fix round 2): kind 1 (Nt_MapHatago = stable) was drawn by the single
    // native_places marker group at the same size as every other place (city, inn, shop...),
    // ~1.7x the game's ratio next to a shrine. Split it into its own layer (same pattern as
    // kind_shrine/kind_tower in botw_map_landmarks.h) so the manifest can size it separately.
    // Item 17 (owner, 2026-10-20): the diagnostic found the markers DO draw; the visibility
    // problem is stable size (18px) and shop icons (kind 11-15) overlapping towns. Publish a
    // third split, .kind_shop, so the manifest can gate shops by zoom separately from
    // towns/castle/labs/travel points (.kind_place).
    const bool is_shop=e.kind>=11&&e.kind<=15;
    h.publish_i64(h.userdata,key(".kind_stable"),e.kind==1?e.kind:-1);
    h.publish_i64(h.userdata,key(".kind_shop"),is_shop?e.kind:-1);
    h.publish_i64(h.userdata,key(".kind_place"),e.kind!=1&&!is_shop?e.kind:-1);
    h.publish_text(h.userdata,key(".icon"),place_icon_src(e.kind).c_str());
    const bool warp=e.kind==4||e.kind==16;
    h.publish_i64(h.userdata,key(".tint"),warp?(e.opened?0xFF00BFFF:0xFFFF8A19):0xFF967828);
    if(h.publish_f64){h.publish_f64(h.userdata,key(".x"),e.xyz[0]);h.publish_f64(h.userdata,key(".y"),-e.xyz[2]);}
  }
  if(full)h.publish_i64(h.userdata,"botw.map.places.count",count);
}
}
