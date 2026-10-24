// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_dungeon_variants.h"
#include "echoes_map_annotations.h"
#include <algorithm>
#include <limits>
#include <set>
namespace echoes {
namespace {
bool Symbol(std::string_view s) {
 return !s.empty()&&s.size()<96&&std::all_of(s.begin(),s.end(),[](unsigned char c){
  return(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});
}
struct Guard {uint64_t at;std::array<uint8_t,32> bytes;};
constexpr std::array<Guard,2> Guards={{{0x25de2d8,{0x15,0x81,0x8d,0x52,0x35,0x00,0xa0,0x72,0x17,0x00,0x80,0x12,0x0b,0x00,0x00,0x14,0xe0,0xa3,0x01,0x91,0xe1,0x03,0x19,0xaa,0x40,0x5d,0x09,0x94,0xe8,0x57,0x40,0xf9}},{0x25de440,{0x08,0x01,0x09,0x8b,0x09,0x08,0x00,0x12,0x08,0x21,0x40,0x39,0x08,0x25,0xc9,0x1a,0x88,0xf5,0x07,0x36,0x3f,0x03,0x00,0xf1,0xe2,0x03,0x96,0x9a,0x5f,0x00,0x01,0xf1}}}};
}
DungeonVariantsSnapshot ReadDungeonVariants(const ReadFn& read,uint64_t base,
 bool build,const MapEpochSource& source,const DungeonMapArt& art) {
 DungeonVariantsSnapshot out;
 if(!read||!build||!base||base%4096||!Symbol(art.key)||art.rooms.empty()||art.rooms.size()>64)return out;
 try {
  std::vector<NativeRiftRegion> queries;
  std::set<uint16_t> rooms;
  for(const auto& room:art.rooms){
   if(room.room>=64||!rooms.insert(room.room).second||!Symbol(room.area_name)||room.conditional_textures.size()>16)return out;
   for(const auto& [flag,texture]:room.conditional_textures){
    if(!Symbol(flag)||!Symbol(texture)||!texture.starts_with(room.area_name+"_c_")||queries.size()>=64)return out;
    queries.push_back({uint32_t(queries.size()+1),flag,"",{}});
   }
  }
  for(const auto& g:Guards){
   std::array<uint8_t,32> bytes{};
   if(base>std::numeric_limits<uint64_t>::max()-g.at||!read(base+g.at,bytes.data(),bytes.size())||!MatchSupportedGuard(bytes,g.bytes,g.at))return out;
  }
  out.compatible=true;
  std::vector<RiftRegionState> states;
  if(!queries.empty()){
   auto flags=ReadRiftVisibility(read,base,build,source,queries);
   if(!flags.compatible||!flags.known||flags.source!=source||flags.regions.size()!=queries.size())return out;
   states=std::move(flags.regions);
   for(size_t i=0;i<states.size();++i)if(states[i].unique_id!=queries[i].unique_id)return out;
  }
  size_t i{};
  for(const auto& room:art.rooms){
   DungeonRoomVariant variant{room.room,true,{}};
   // Native25de300..25de470 scans every authored condition. Each true flag
   // replaces the selected texture; a later true condition takes precedence.
   for(const auto& condition:room.conditional_textures){
    if(states[i++].active)variant.texture=condition.second;
   }
   out.variants.push_back(std::move(variant));
  }
  out.source=source;out.known=true;
  return out;
 }catch(...){out.known=false;out.variants.clear();out.source={};return out;}
}
} // namespace echoes
