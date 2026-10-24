// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
namespace echoes {
namespace {
struct Blob {
 std::vector<uint8_t> b;
 bool Span(uint64_t a,uint64_t n)const{return a<=b.size()&&n<=b.size()-a;}
 uint32_t U32(size_t a)const{return uint32_t(b[a])|uint32_t(b[a+1])<<8|uint32_t(b[a+2])<<16|uint32_t(b[a+3])<<24;}
 uint64_t U64(size_t a)const{return U32(a)|uint64_t(U32(a+4))<<32;}
 uint16_t U16(size_t a)const{return uint16_t(b[a])|uint16_t(b[a+1])<<8;}
 float F(size_t a)const{return std::bit_cast<float>(U32(a));}
 bool Str(size_t a,std::string& s)const {
  if(!Span(a,16))return false;
  auto p=U64(a),n=U64(a+8);if(!n||n>127||!Span(p,n))return false;
  s.assign(reinterpret_cast<const char*>(b.data()+p),size_t(n));
  return std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';});
 }
 bool CString(size_t a,std::string& s)const {
  if(!Span(a,1))return false;
  auto e=std::find(b.begin()+a,b.begin()+std::min(b.size(),a+128),0);
  if(e==b.begin()+std::min(b.size(),a+128))return false;
  s.assign(b.begin()+a,e);return true;
 }
};
bool Load(const ReadAssetFn& read,std::string_view path,Blob& out) {
 const auto n=read(path,0,nullptr,0);if(n<48||n>1024*1024)return false;
 out.b.resize(n);return read(path,0,out.b.data(),n)==n;
}
bool Sheet(const Blob& b,uint32_t fields,uint32_t all,uint32_t stride,uint32_t max,size_t& rows,size_t& count) {
 if(!b.Span(0,48+all*48)||b.U32(0)!=0x74687367||b.U32(4)!=2||b.U32(24)!=fields||b.U32(28)!=all||b.U32(44)!=stride)return false;
 rows=b.U64(32);count=b.U32(40);return count>0&&count<=max&&b.Span(rows,count*stride);
}
bool Field(const Blob& b,size_t descriptor,std::string_view expected,uint16_t offset,uint8_t type,uint16_t flags) {
 std::string name;if(!b.CString(size_t(b.U64(descriptor)),name))return false;
 return name==expected&&b.b[descriptor+16]==type&&b.U16(descriptor+18)==flags&&b.U16(descriptor+20)==offset;
}
uint32_t Crc(std::string_view s) {
 uint32_t c=~0u;for(unsigned char b:s){c^=b;for(int i=0;i<8;++i)c=(c>>1)^((0u-(c&1))&0xedb88320u);}return ~c;
}
bool Finite(float x){return std::isfinite(x)&&std::abs(x)<=100000;}
bool Bounds(const Blob& b,size_t at,std::array<float,3>& lo,std::array<float,3>& hi) {
 if(!b.Span(at,32))return false;
 for(size_t k=0;k<3;++k){lo[k]=b.F(at+k*4);hi[k]=b.F(at+16+k*4);if(!Finite(lo[k])||!Finite(hi[k])||lo[k]>hi[k])return false;}return true;
}
bool Common(const Blob& b,std::map<std::string,DungeonMapDefinition>& levels) {
 if(!b.Span(0,0x8c)||b.U32(0)!=0x6f6c6365||b.U32(4)!=0x01010003||b.U32(8)!=b.b.size()||b.U32(0x84)!=0x65636c64)return false;
 auto n=b.U32(0xc),dir=b.U32(0x10),rows=b.U32(0x18),rc=b.U32(0x14),room_count=b.U32(0x1c),room_rows=b.U32(0x20),fc=b.U32(0x24),fr=b.U32(0x28),sc=b.U32(0x74),si=b.U32(0x78),ss=b.U32(0x80);
 if(!n||n>4096||n!=rc||n!=b.U32(0x88)||room_count>8192||fc>8192||sc>16384||!b.Span(dir,n*8)||!b.Span(rows,rc*0x120)||!b.Span(room_rows,room_count*0x60)||!b.Span(fr,fc*4)||!b.Span(si,sc*4)||!b.Span(ss,1))return false;
 std::set<uint32_t> indices;uint32_t previous=0;
 for(uint32_t i=0;i<n;++i) {
  auto crc=b.U32(dir+i*8),index=b.U32(dir+i*8+4);if((i&&crc<=previous)||index>=rc||!indices.insert(index).second)return false;previous=crc;
  size_t a=rows+index*0x120;auto ni=b.U32(a+0x100);DungeonMapDefinition level;
  if(ni>=sc||!b.CString(size_t(ss)+b.U32(si+ni*4),level.key)||level.key.empty()||Crc(level.key)!=crc||!Bounds(b,a+0x60,level.lo,level.hi))return false;
  auto first=b.U32(a+0x108);auto count=b.U16(a+0x116);if(count>64||first>room_count||count>room_count-first)return false;
  for(size_t j=0;j<count;++j){size_t q=room_rows+(first+j)*0x60;DungeonRoom room;room.number=b.U16(q+0x56);room.side_view=b.b[q+0x5e]!=0;
   if(!Bounds(b,q+0x30,room.lo,room.hi))return false;
   auto fi=b.U16(q+0x5a);auto fn=b.b[q+0x5d];if(fn>10||fi>fc||fn>fc-fi)return false;
   for(size_t k=0;k<fn;++k){auto floor=std::bit_cast<int32_t>(b.U32(fr+(fi+k)*4));if(floor< -10||floor>10)return false;room.floors.push_back(floor);}
   level.rooms.push_back(std::move(room));
  }
  if(!levels.emplace(level.key,std::move(level)).second)return false;
 }
 return true;
}
bool Major(const Blob& b,std::map<std::string,uint8_t>& out) {
 size_t rows{},count{};if(!Sheet(b,1,1,16,30,rows,count)||!Field(b,48,"mLevelname",0,4,4))return false;
 for(size_t i=0;i<count;++i){std::string key;if(!b.Str(rows+i*16,key)||!out.emplace(key,uint8_t(i)).second)return false;}return true;
}
bool Maps(const Blob& b,std::map<std::string,DungeonMapDefinition>& out,const std::map<std::string,uint8_t>& majors) {
 size_t rows{},count{};if(!Sheet(b,17,17,120,32,rows,count))return false;
 const std::array<std::string_view,17> names={"mLevelName","mUiMapLevelType","mLayoutName","mOtherLevelKey","mSizeRatio","mSideAreaRatio","mIsMinimap","mIsHudTime","mFloorNum","mFloorMin","mFloorTextFirstIndex","mLastRoomNo","mZoomScales","mDefaultZoomLevel","mZoomScaleWarpRegister","mBasePosX","mBasePosY"};
 const std::array<uint16_t,17> offsets={0,16,24,40,56,60,64,65,68,72,76,80,88,104,108,112,116};
 const std::array<uint8_t,17> types={4,2,4,4,3,3,1,1,2,2,2,2,3,2,3,3,3};
 const std::array<uint16_t,17> flags={4,16,0,9,0,0,0,0,0,0,0,0,2,0,0,0,0};
 for(size_t i=0;i<17;++i)if(!Field(b,48+i*48,names[i],offsets[i],types[i],flags[i]))return false;
 std::set<std::string> seen;
 for(size_t i=0;i<count;++i){size_t a=rows+i*120;std::string key;if(!b.Str(a,key)||!seen.insert(key).second)return false;
  if(b.U32(a+16)!=2)continue;
  auto level=out.find(key);auto major=majors.find(key);std::string layout;
  if(level==out.end()||major==majors.end()||!b.Str(a+24,layout)||layout!=key||level->second.rooms.empty())return false;
  auto& x=level->second;x.major_index=major->second;x.floor_count=std::bit_cast<int32_t>(b.U32(a+68));x.floor_min=std::bit_cast<int32_t>(b.U32(a+72));x.floor_text_first_index=std::bit_cast<int32_t>(b.U32(a+76));x.size_ratio=b.F(a+56);x.base_x=b.F(a+112);x.base_y=b.F(a+116);
  if(x.floor_count<1||x.floor_count>10||x.floor_min< -10||x.floor_min>10||x.size_ratio<=0||!Finite(x.size_ratio)||!Finite(x.base_x)||!Finite(x.base_y))return false;
 }
 for(auto at=out.begin();at!=out.end();)if(!at->second.floor_count)at=out.erase(at);else ++at;
 return !out.empty();
}
bool FloorHeights(const Blob& b,std::map<std::string,DungeonMapDefinition>& maps) {
 size_t rows{},count{};if(!Sheet(b,6,12,72,512,rows,count)||!Field(b,48,"mAreaName",0,4,4)||!Field(b,48+4*48,"mFloorInfo",40,0,2))return false;
 std::set<std::string> seen;
 for(size_t i=0;i<count;++i){size_t a=rows+i*72;std::string key;if(!b.Str(a,key)||!seen.insert(key).second)return false;
  auto start=b.U64(a+40),n=b.U64(a+48);if(n>10||!b.Span(start,n*8))return false;
  for(auto& [mapkey,map]:maps)for(auto& room:map.rooms){std::string number=std::to_string(room.number);while(number.size()<3)number="0"+number;
   if(key!=mapkey+"_"+number)continue;
   for(size_t j=0;j<n;++j){DungeonFloorHeight f{b.F(start+j*8),b.F(start+j*8+4)};if(!Finite(f.floor)||!Finite(f.height)||f.floor!=std::trunc(f.floor))return false;room.floor_heights.push_back(f);}
  }
 }
 return true;
}
} // namespace
bool LoadDungeonCatalog(const ReadAssetFn& read,std::vector<DungeonMapDefinition>& out) {
 out.clear();if(!read)return false;
 try {
  Blob common,major,maps,floors;std::map<std::string,DungeonMapDefinition> levels;std::map<std::string,uint8_t> majors;
  if(!Load(read,"region_common/level/Common.eclo",common)||!Common(common,levels)||!Load(read,"region_common/datasheets/MajorDungeonDefinition.gsheet",major)||!Major(major,majors)||!Load(read,"region_common/datasheets/ui/UiMap.gsheet",maps)||!Maps(maps,levels,majors)||!Load(read,"region_common/datasheets/ui/UiMapArea.gsheet",floors)||!FloorHeights(floors,levels))return false;
  std::vector<DungeonMapDefinition> result;for(auto& [key,map]:levels){std::set<uint16_t> numbers;for(const auto& room:map.rooms){if(room.number>=64||(!room.floors.empty()&&!numbers.insert(room.number).second))return false;for(auto f:room.floors)if(f<map.floor_min||f>=map.floor_min+map.floor_count)return false;}result.push_back(std::move(map));}
  out=std::move(result);return true;
 }catch(...){out.clear();return false;}
}
std::string_view DungeonFloorMessageKey(const DungeonMapDefinition& map, int32_t floor) {
 // Original 25cf06c..25cf1dc builds the signed -4..3 message lookup from
 // mFloorTextFirstIndex and its reversed floor-list ordinal. Native authored
 // floor indices are bottom-up, starting at mFloorMin.
 static constexpr std::array<std::string_view,8> keys={
  "Map_B4","Map_B3","Map_B2","Map_B1","Map_1F","Map_2F","Map_3F","Map_4F"};
 const int64_t ordinal=int64_t(floor)-map.floor_min;
 if(map.floor_count<1||map.floor_count>10||ordinal<0||ordinal>=map.floor_count)return {};
 const int64_t key=int64_t(map.floor_text_first_index)+ordinal;
 return key>=-4&&key<=3?keys[size_t(key+4)]:std::string_view{};
}
DungeonPlayerRoom ResolveDungeonPlayerRoom(const DungeonMapDefinition& map,const PlayerWorldSnapshot& world) {
 DungeonPlayerRoom out;if(!world.valid||world.level_key!=map.key||map.rooms.size()>64)return out;
 for(float f:world.xyz)if(!Finite(f))return out;
 for(const auto& room:map.rooms){bool inside=true;for(size_t i=0;i<3;++i)inside=inside&&world.xyz[i]>=room.lo[i]&&world.xyz[i]<=room.hi[i];if(!inside)continue;
  int32_t floor=0;
  if(room.floors.size()==1)floor=room.floors[0];
  else if(room.floors.size()>1){floor=1;for(const auto& entry:room.floor_heights)if(world.xyz[1]>entry.height)floor=int32_t(entry.floor);}
  if(std::find(room.floors.begin(),room.floors.end(),floor)==room.floors.end())return out;
  out={true,room.number,floor,room.side_view};return out;
 }
 return out;
}
} // namespace echoes
