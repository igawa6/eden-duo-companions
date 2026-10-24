// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon.h"
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
namespace {
size_t checks{};
void Check(bool b){++checks;if(!b)throw std::runtime_error("dungeon check "+std::to_string(checks));}
std::vector<uint8_t> File(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
uint32_t U32(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void Put(std::vector<uint8_t>&b,size_t a,uint32_t x){for(size_t i=0;i<4;++i)b.at(a+i)=uint8_t(x>>(i*8));}
}
int main(){try{
 std::vector<echoes::DungeonMapDefinition> catalog;
 Check(!echoes::LoadDungeonCatalog({},catalog)&&catalog.empty());
 const char* root=std::getenv("ECHOES_ROMFS");if(!root){std::cout<<checks<<" checks; native dungeon fixtures skipped\n";return 77;}
 std::map<std::string,std::vector<uint8_t>> files;
 for(auto p:{"region_common/level/Common.eclo","region_common/datasheets/MajorDungeonDefinition.gsheet","region_common/datasheets/ui/UiMap.gsheet","region_common/datasheets/ui/UiMapArea.gsheet"}){files[p]=File(std::filesystem::path(root)/p);Check(!files[p].empty());}
 std::string missing,corrupt;
 auto read=[&](std::string_view path,uint64_t offset,void*out,size_t n){auto at=files.find(std::string(path));if(at==files.end()||path==missing)return size_t(0);const auto&b=at->second;if(!out&&!n)return b.size();if(offset>b.size()||n>b.size()-offset)return size_t(0);std::memcpy(out,b.data()+offset,n);return n;};
 Check(echoes::LoadDungeonCatalog(read,catalog)&&catalog.size()==8);
 const std::array<size_t,8> room_counts={22,21,27,19,27,30,30,19};
 for(size_t i=0;i<catalog.size();++i){const auto&m=catalog[i];Check(m.rooms.size()==room_counts[i]&&m.size_ratio==8);Check(m.floor_min==1&&m.floor_count>=1&&m.floor_count<=5);
  for(const auto&r:m.rooms){Check(r.number<64);for(auto f:r.floors)Check(f>=m.floor_min&&f<m.floor_min+m.floor_count);}
 }
 const std::array<std::vector<std::string_view>,8> labels={
  std::vector<std::string_view>{"Map_1F","Map_2F"},
  {"Map_B1","Map_1F","Map_2F","Map_3F"},
  {"Map_1F","Map_2F"},{"Map_1F","Map_2F","Map_3F"},
  {"Map_1F","Map_2F","Map_3F","Map_4F"},
  {"Map_B4","Map_B3","Map_B2","Map_B1","Map_1F"},
  {"Map_B2","Map_B1","Map_1F"},{"Map_1F"}};
 for(size_t i=0;i<catalog.size();++i){const auto& m=catalog[i];
  Check(labels[i].size()==size_t(m.floor_count));
  for(int32_t f=0;f<m.floor_count;++f)Check(echoes::DungeonFloorMessageKey(m,m.floor_min+f)==labels[i][size_t(f)]);
  Check(echoes::DungeonFloorMessageKey(m,m.floor_min-1).empty());
  Check(echoes::DungeonFloorMessageKey(m,m.floor_min+m.floor_count).empty());
 }
 auto invalid_label=catalog[0];invalid_label.floor_text_first_index=2147483647;
 Check(echoes::DungeonFloorMessageKey(invalid_label,invalid_label.floor_min).empty());
 invalid_label.floor_text_first_index=-2147483647-1;
 Check(echoes::DungeonFloorMessageKey(invalid_label,invalid_label.floor_min).empty());
 auto suthorn=catalog[0];Check(suthorn.key=="L_Dungeon001"&&suthorn.major_index==0&&suthorn.floor_count==2);
 const auto at=std::find_if(suthorn.rooms.begin(),suthorn.rooms.end(),[](const auto&r){return r.number==7;});Check(at!=suthorn.rooms.end()&&at->floors==std::vector<int32_t>{1,2}&&at->floor_heights.size()==2);
 const auto room=*at;suthorn.rooms={room};echoes::PlayerWorldSnapshot world;world.valid=true;world.level_key=suthorn.key;
 world.xyz={(room.lo[0]+room.hi[0])/2,21,(room.lo[2]+room.hi[2])/2};
 auto current=echoes::ResolveDungeonPlayerRoom(suthorn,world);Check(current.valid&&current.room==7&&current.floor==1);
 world.xyz[1]=std::nextafter(21.f,std::numeric_limits<float>::infinity());current=echoes::ResolveDungeonPlayerRoom(suthorn,world);Check(current.valid&&current.floor==2);
 world.xyz[0]=room.lo[0];Check(echoes::ResolveDungeonPlayerRoom(suthorn,world).valid);
 world.xyz[0]=std::nextafter(room.lo[0],-std::numeric_limits<float>::infinity());Check(!echoes::ResolveDungeonPlayerRoom(suthorn,world).valid);
 world.xyz[0]=std::numeric_limits<float>::quiet_NaN();Check(!echoes::ResolveDungeonPlayerRoom(suthorn,world).valid);
 world.xyz[0]=room.lo[0];world.level_key="OP_Dungeon";Check(!echoes::ResolveDungeonPlayerRoom(suthorn,world).valid);
 world.level_key=suthorn.key;world.valid=false;Check(!echoes::ResolveDungeonPlayerRoom(suthorn,world).valid);
 for(const auto&[path,bytes]:files){missing=path;catalog.clear();Check(!echoes::LoadDungeonCatalog(read,catalog)&&catalog.empty());}missing.clear();
 const std::string common="region_common/level/Common.eclo",maps="region_common/datasheets/ui/UiMap.gsheet",area="region_common/datasheets/ui/UiMapArea.gsheet";
 for(const auto& fault:std::vector<std::pair<std::string,size_t>>{{common,8},{common,0xc},{common,0x28},{maps,0x28},{maps,0x2c},{area,0x28}}){auto saved=files[fault.first];Put(files[fault.first],fault.second,0xffffffff);Check(!echoes::LoadDungeonCatalog(read,catalog)&&catalog.empty());files[fault.first]=std::move(saved);}
 auto saved=files[common];const auto dir=U32(saved.data()+0x10);Put(files[common],dir,0);Check(!echoes::LoadDungeonCatalog(read,catalog)&&catalog.empty());files[common]=std::move(saved);
 Check(echoes::LoadDungeonCatalog(read,catalog)&&catalog.size()==8);
 std::cout<<checks<<" native dungeon catalog/floor checks passed\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
