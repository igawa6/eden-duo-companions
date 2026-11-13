// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
namespace botw {
struct MapRegions {bool valid{};std::array<bool,8> visible{};};
inline constexpr std::array<const char*,8> region_names{"MapRegion_Eldin","MapRegion_Firone","MapRegion_Gerudo","MapRegion_Hateru","MapRegion_Hebura","MapRegion_HyrulePrairie ","MapRegion_Lanayru","MapRegion_Tamul"};
inline MapRegions map_regions(const EdenDsmodHostApi& h) {
  if(!map_build_supported(h))return {};
 UiSample ui{},after{};if(!ui_sample(h,ui)||!hud_context(ui)||!main_field_stage(h))return {};
 struct Root {uint64_t p{},vt{},array{};int32_t count{},cap{};uint32_t bits{};bool operator==(const Root&)const=default;} root{},last{};
 auto roots=[&](Root& r){return read(h,h.main_base+map_offsets(h).root,r.p)&&r.p&&r.p%8==0&&r.p<=UINT64_MAX-0x468&&read(h,r.p,r.vt)&&r.vt==h.main_base+map_offsets(h).vtable&&read(h,r.p+0x458,r.count)&&read(h,r.p+0x45C,r.cap)&&(r.count==1103||r.count==1105)&&r.cap>=r.count&&r.cap<=2048&&read(h,r.p+0x460,r.array)&&r.array&&r.array%8==0&&r.array<=UINT64_MAX-2048*8&&read(h,r.p+0x120,r.bits)&&!(r.bits&~0x7FFFu);};
 if(!roots(root))return {};
 struct Record {uint64_t p{},vt{},source{},dynamic{},source_vt{},name{};int32_t kind{};uint32_t region{},show{};std::array<char,32> text{};std::array<float,3> xyz{};bool operator==(const Record&)const=default;};
 std::array<Record,8> records{};MapRegions out;
 auto record=[&](unsigned i,Record& r){return read(h,root.array+(477+i)*8,r.p)&&r.p&&r.p%8==0&&r.p<=UINT64_MAX-0x60&&read(h,r.p,r.vt)&&r.vt==h.main_base+map_offsets(h).name_vtable&&read(h,r.p+0x1C,r.kind)&&r.kind==23&&read(h,r.p+0x28,r.xyz)&&std::isfinite(r.xyz[0])&&std::abs(r.xyz[0])<=6000&&std::isfinite(r.xyz[1])&&std::abs(r.xyz[1])<=10000&&std::isfinite(r.xyz[2])&&std::abs(r.xyz[2])<=5000&&read(h,r.p+0x44,r.region)&&r.region<15&&read(h,r.p+0x50,r.source)&&r.source&&r.source%4==0&&r.source<=UINT64_MAX-0x18&&read(h,r.p+0x58,r.dynamic)&&!r.dynamic&&read(h,r.source,r.source_vt)&&r.source_vt==h.main_base+map_offsets(h).label_source_vtable&&read(h,r.source+8,r.show)&&r.show==1&&read(h,r.source+0x10,r.name)&&r.name&&r.name<=UINT64_MAX-32&&read(h,r.name,r.text)&&std::string_view(r.text.data(),std::strlen(region_names[i]))==region_names[i]&&r.text[std::strlen(region_names[i])]=='\0';};
 for(unsigned i=0;i<8;++i){if(!record(i,records[i]))return {};for(unsigned j=0;j<i;++j)if(records[i].p==records[j].p||records[i].source==records[j].source)return {};out.visible[i]=(root.bits&(1u<<records[i].region))!=0;}
 for(unsigned i=0;i<8;++i){Record again{};if(!record(i,again)||again!=records[i])return {};}
 if(!roots(last)||last!=root||!main_field_stage(h)||!ui_sample(h,after)||after!=ui)return {};
 out.valid=true;return out;
}
inline void publish_map_regions(const EdenDsmodHostApi& h,const MapRegions& regions,bool full=true){h.publish_i64(h.userdata,"botw.map.regions.valid",regions.valid);if(full)for(unsigned i=0;i<8;++i){Key key("botw.map.regions.",i);h.publish_i64(h.userdata,key.c_str(),regions.valid&&regions.visible[i]);}}
}
