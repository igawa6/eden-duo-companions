// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_map.h"
#include <cstring>
namespace botw {
// The native HUD minimap's own applied transforms (BOTW 1.6.0, AppMap screen = ScreenMgr slot 42).
// Options > "Fix minimap to north" OFF: the game turns the whole minimap so the CAMERA faces up
// (N_MapSR_00 rotate.z = camera yaw, ui2d degrees counter-clockwise; the N mark pane N_Rotate_00
// carries the same value) and the view cone P_Light_00 stays up. ON: N_MapSR_00 stays 0 and only
// P_Light_00 turns with the camera. Reading both panes therefore mirrors either setting exactly.
// nn::ui2d::Pane: +0 vtable, +8/+0x10 sibling link, +0x18 parent, +0x20/+0x28 child list head
// (prev/next), +0x3C rotate x/y/z, +0xB8 name[24].
struct MapView {
  bool valid{};      ///< N_MapSR_00 read: the map's own turn
  float map_rz{};    ///< native degrees, counter-clockwise
  bool cone_valid{}; ///< P_Light_00 read: the camera cone
  float cone_rz{};   ///< native degrees, counter-clockwise; 0 = pointing up
  uint64_t cone_pane{};
  bool operator==(const MapView&) const = default;
};
namespace map_view_detail {
// P_Light_00 up to the AppMap RootPane (live 1.6.0 pane tree, exact names).
inline constexpr std::array<const char*,9> ConePath = {"P_Light_00","N_All_00","Pa_MapIconPlayer_00",
  "N_MapIconPlayer_00","Pa_MapIcon_00","N_MapIcon_00","N_Cut_01","N_InOut_00","RootPane"};
struct Pane { uint64_t vtable{},parent{},first{};std::array<float,3> rotate{};std::array<char,24> name{}; };
inline bool pane(const EdenDsmodHostApi& h,uint64_t p,Pane& out) {
  if(!p || p%8 || p>UINT64_MAX-0xD0)return false;
  std::array<uint8_t,0xD0> b{};
  if(!read(h,p,b))return false;
  std::memcpy(&out.vtable,b.data(),8);std::memcpy(&out.parent,b.data()+0x18,8);
  std::memcpy(&out.first,b.data()+0x28,8);std::memcpy(out.rotate.data(),b.data()+0x3C,12);
  std::memcpy(out.name.data(),b.data()+0xB8,24);
  return out.vtable>h.main_base && out.vtable-h.main_base<h.main_size;
}
inline bool named(const Pane& p,const char* want) {
  const size_t n=std::strlen(want);
  return n<p.name.size() && std::memcmp(p.name.data(),want,n)==0 && p.name[n]==0;
}
inline bool finite_turn(const Pane& p) {
  return p.rotate[0]==0.f && p.rotate[1]==0.f && std::isfinite(p.rotate[2]) && std::abs(p.rotate[2])<=1e5f;
}
// The cached cone pane is accepted only when its exact parent chain ends at this screen's root.
inline bool cone_chain(const EdenDsmodHostApi& h,uint64_t cone,uint64_t root,Pane& leaf) {
  uint64_t p=cone;
  for(size_t i=0;i<ConePath.size();++i) {
    Pane q{};
    if(!pane(h,p,q) || !named(q,ConePath[i]))return false;
    if(i==0){if(q.vtable!=h.main_base+map_offsets(h).picture_pane_vtable)return false;leaf=q;}
    if(i+1==ConePath.size())return p==root && q.parent==0;
    p=q.parent;
  }
  return false;
}
// Bounded child walk by exact names from the root down (used only when the cache misses).
inline uint64_t find_cone(const EdenDsmodHostApi& h,uint64_t root) {
  uint64_t p=root;
  for(size_t level=ConePath.size()-1;level-->0;) {
    Pane parent{};
    if(!pane(h,p,parent) || p>UINT64_MAX-0x20)return 0;
    const uint64_t head=p+0x20;uint64_t node=parent.first,found=0;
    for(unsigned i=0;i<64 && node!=head;++i) {
      Pane child{};
      if(node<8 || node>UINT64_MAX-0x10 || !pane(h,node-8,child) || child.parent!=p)return 0;
      if(named(child,ConePath[level])){found=node-8;break;}
      if(!read(h,node+8,node))return 0;
    }
    if(!found)return 0;
    p=found;
  }
  return p;
}
} // namespace map_view_detail
inline MapView map_view(const EdenDsmodHostApi& h,uint64_t cached_cone=0) {
  using namespace map_view_detail;
  UiSample ui{},after{};
  if(!map_build_supported(h) || h.main_size<map_offsets(h).null_pane_vtable+8 || h.main_base>UINT64_MAX-(map_offsets(h).null_pane_vtable+8))return {};
  if(!ui_sample(h,ui) || !hud_context(ui) || !main_field_stage(h) || ui.array>UINT64_MAX-43*8)return {};
  uint64_t screen{},screen_vt{},sr{},root{};
  if(!read(h,ui.array+42*8,screen) || !screen || screen%8 || screen>UINT64_MAX-0x5418 ||
     !read(h,screen,screen_vt) || screen_vt!=h.main_base+map_offsets(h).screen_vtable ||
     !read(h,screen+map_offsets(h).minimap_sr,sr) || !read(h,screen+map_offsets(h).minimap_root,root))return {};
  Pane map{},again{},rootp{};
  if(!pane(h,sr,map) || map.vtable!=h.main_base+map_offsets(h).null_pane_vtable || !named(map,"N_MapSR_00") ||
     !finite_turn(map) || !pane(h,root,rootp) || rootp.vtable!=h.main_base+map_offsets(h).root_pane_vtable ||
     !named(rootp,"RootPane"))return {};
  MapView out{true,map.rotate[2]};
  Pane leaf{};
  uint64_t cone=cached_cone;
  if(!cone || !cone_chain(h,cone,root,leaf)) {
    cone=find_cone(h,root);
    if(cone && !cone_chain(h,cone,root,leaf))cone=0;
  }
  if(cone && finite_turn(leaf)){out.cone_valid=true;out.cone_rz=leaf.rotate[2];out.cone_pane=cone;}
  // Stable second read of every value and the context.
  uint64_t screen2{},sr2{};
  if(!read(h,ui.array+42*8,screen2) || screen2!=screen || !read(h,screen+map_offsets(h).minimap_sr,sr2) || sr2!=sr ||
     !pane(h,sr,again) || again.vtable!=map.vtable || again.name!=map.name || again.rotate!=map.rotate ||
     !main_field_stage(h) ||
     !ui_sample(h,after) || after!=ui)return {};
  if(out.cone_valid){Pane leaf2{};if(!pane(h,cone,leaf2) || leaf2.vtable!=leaf.vtable || leaf2.name!=leaf.name || leaf2.rotate!=leaf.rotate){out.cone_valid=false;out.cone_rz=0;}}
  return out;
}
// Companion degrees, clockwise on the y-down canvas, in [0, 360) on a half-degree grid: a slow
// camera drift moves the rim by at most ~3.5 px per step (whole degrees moved it ~7 px).
inline constexpr double TurnStep=0.5;
inline double turn_degrees(double cw) {
  if(!std::isfinite(cw) || std::abs(cw)>1e9)return 0.;
  double d=std::fmod(cw,360.);if(d<0.)d+=360.;
  d=std::round(d/TurnStep)*TurnStep+0.; // +0. turns a -0 into 0
  return d>=360.||d==0.?(d>=360.?d-360.:0.):d;
}
// Kept for callers that want whole degrees.
inline int64_t whole_degrees(double cw) {
  if(!std::isfinite(cw))return 0;
  auto d=int64_t(std::llround(std::fmod(cw,360.)));
  d%=360;if(d<0)d+=360;return d;
}
inline double view_rotation(const MapView& v){return v.valid?turn_degrees(-v.map_rz):0.;}
// The player arrow: the map-frame heading turned with the map (native N_PlayerRotate_00 =
// N_MapSR_00 - heading, verified live: 239.23 = 169.62 + 69.62).
inline double view_heading(const MapView& v,float heading){return turn_degrees(double(heading)+(v.valid?-v.map_rz:0.));}
// The camera cone: P_Light_00 is 0 while the map turns (cone straight up) and minus the camera yaw
// when the map is fixed north; the companion cone art points up at rotation 0.
inline double view_cone(const MapView& v){return v.cone_valid?turn_degrees(-v.cone_rz):0.;}
// Fractional degrees go out as f64 (the map's view_rotate_bind and the images' rotate_bind read
// either); a host without publish_f64 gets whole degrees.
inline void publish_degrees(const EdenDsmodHostApi& h,const char* key,double degrees) {
  if(h.publish_f64)h.publish_f64(h.userdata,key,degrees);
  else h.publish_i64(h.userdata,key,whole_degrees(degrees));
}
// A failed turn read degrades to the classic north-up view: rotation 0 and the arrow at its plain
// map heading; the cone needs the native camera pane and is hidden without it.
inline void publish_map_view(const EdenDsmodHostApi& h,const MapView& v,bool minimap,const MapPosition& p) {
  const bool ok=minimap && v.valid;
  const MapView turned=ok?v:MapView{};
  h.publish_i64(h.userdata,"botw.map.view.valid",ok);
  publish_degrees(h,"botw.map.view.rotation",view_rotation(turned));
  h.publish_i64(h.userdata,"botw.map.view.heading.valid",minimap && p.heading_valid);
  publish_degrees(h,"botw.map.view.heading",minimap && p.heading_valid?view_heading(turned,p.heading):0.);
  h.publish_i64(h.userdata,"botw.map.view.cone.valid",ok && v.cone_valid);
  publish_degrees(h,"botw.map.view.cone",ok?view_cone(v):0.);
}
} // namespace botw
