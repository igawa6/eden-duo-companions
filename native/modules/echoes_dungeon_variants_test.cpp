// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon_variants.h"
#include "echoes_map_annotations.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
namespace {
size_t checks{},calls{};
echoes::RiftVisibilitySnapshot flags;
std::vector<echoes::NativeRiftRegion> requested;
bool throw_flags{};
void Check(bool b){++checks;if(!b)throw std::runtime_error("variant check "+std::to_string(checks));}
}
namespace echoes {
// Unit boundary: the production transport is ReadRiftVisibility, whose complete
// native guard/epoch/hash/replay tests remain in its dedicated reader suite.
RiftVisibilitySnapshot ReadRiftVisibility(const ReadFn&,uint64_t,bool,
 const MapEpochSource&,std::span<const NativeRiftRegion> q){
 ++calls;requested.assign(q.begin(),q.end());
 if(throw_flags)throw std::runtime_error("read fault");
 return flags;
}
}
int main(){try{
 const char* dir=std::getenv("ECHOES_SEGMENTS");if(!dir)return 77;
 std::ifstream file(std::filesystem::path(dir)/"text.bin",std::ios::binary);
 std::vector<uint8_t> text{std::istreambuf_iterator<char>(file),{}};
 Check(text.size()>0x25de460);
 constexpr uint64_t Base=0x80000000;
 bool fail{},corrupt{},throw_read{};
 echoes::ReadFn read=[&](uint64_t at,void* out,size_t n){
  if(throw_read)throw std::runtime_error("fault");
  if(fail||at<Base||at-Base>text.size()||n>text.size()-(at-Base))return false;
  std::memcpy(out,text.data()+at-Base,n);
  if(corrupt&&n)static_cast<uint8_t*>(out)[0]^=1;
  return true;
 };
 echoes::MapEpochSource source{{0x90000000,0x90010000,0x90020000,7},0x90030000,0x90040000,0x90050000,0x90060000};
 echoes::DungeonMapArt art;art.key="L_Dungeon003";
 echoes::DungeonRoomArt room;room.room=10;room.area_name="L_Dungeon003_010";
 room.conditional_textures={{"L003R010SandDown","L_Dungeon003_010_c_00"},{"L003R023SandDown","L_Dungeon003_010_c_01"}};
 art.rooms.push_back(room);room.room=11;room.area_name="L_Dungeon003_011";room.conditional_textures.clear();art.rooms.push_back(room);
 flags.compatible=flags.known=true;flags.source=source;flags.regions={{1,false},{2,false}};
 auto run=[&](){return echoes::ReadDungeonVariants(read,Base,true,source,art);};
 auto good=[&](std::string_view expected){auto s=run();Check(s.compatible&&s.known&&s.source==source&&s.variants.size()==2);Check(s.variants[0].room==10&&s.variants[0].known&&s.variants[0].texture==expected);Check(s.variants[1].room==11&&s.variants[1].known&&s.variants[1].texture.empty());};
 auto bad=[&](){auto s=run();Check(!s.known&&s.variants.empty()&&s.source==echoes::MapEpochSource{});};
 good("");Check(requested.size()==2&&requested[0].activate_flag=="L003R010SandDown"&&requested[1].activate_flag=="L003R023SandDown"&&requested[0].deactivate_flag.empty());
 flags.regions[0].active=true;good("L_Dungeon003_010_c_00");
 flags.regions[1].active=true;good("L_Dungeon003_010_c_01");
 flags.regions[0].active=false;good("L_Dungeon003_010_c_01");
 flags.known=false;bad();flags.known=true;
 flags.compatible=false;bad();flags.compatible=true;
 flags.source.save_epoch.revision++;bad();flags.source=source;
 flags.regions.pop_back();bad();flags.regions={{2,false},{1,false}};bad();flags.regions={{1,false},{2,false}};
 throw_flags=true;bad();throw_flags=false;
 fail=true;bad();fail=false;corrupt=true;bad();corrupt=false;throw_read=true;bad();throw_read=false;
 auto saved=art;
 art.rooms[0].conditional_textures[0].first="bad/flag";bad();art=saved;
 art.rooms[0].conditional_textures[0].second="L_Dungeon003_011_c_00";bad();art=saved;
 art.rooms[1].room=10;bad();art=saved;art.rooms[0].room=64;bad();art=saved;
 art.key="";bad();art=saved;art.rooms.clear();bad();art=saved;
 art.rooms[0].conditional_textures.resize(17);bad();art=saved;
 for(size_t i=0;i<5;++i){auto r=room;r.room=uint16_t(20+i);r.conditional_textures.assign(16,{"NativeFlag","L_Dungeon003_011_c_00"});art.rooms.push_back(r);}bad();art=saved;
 Check(!echoes::ReadDungeonVariants(read,Base,false,source,art).known);
 Check(!echoes::ReadDungeonVariants(read,Base+1,true,source,art).known);
 Check(!echoes::ReadDungeonVariants({},Base,true,source,art).known);
 art.rooms.erase(art.rooms.begin());auto old_calls=calls;auto s=run();Check(s.compatible&&s.known&&s.variants.size()==1&&calls==old_calls);
 std::cout<<checks<<" dungeon conditional variant checks passed\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
