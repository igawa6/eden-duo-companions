// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_dungeon.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <set>
namespace echoes {
namespace {
struct Guard {uint64_t offset;std::array<uint8_t,32> bytes;};
constexpr std::array<Guard,5> Guards{{
 {0x236c3e4,{0x8b,0xe8,0x8c,0x52,0x2b,0x00,0xa0,0x72,0x0c,0x04,0x80,0x52,0x6d,0x0e,0x09,0x8b,0x29,0x09,0x00,0x91,0x5f,0x01,0x09,0xeb,0xae,0x09,0x40,0xf9,0xcb,0x01,0x00,0xb9}},
 {0x2368320,{0xfd,0x7b,0xba,0xa9,0xfb,0x0b,0x00,0xf9,0xfa,0x67,0x02,0xa9,0xf8,0x5f,0x03,0xa9,0xf6,0x57,0x04,0xa9,0xf4,0x4f,0x05,0xa9,0xfd,0x03,0x00,0x91,0x96,0x8a,0x00,0x90}},
 {0x23683ec,{0xc8,0x2a,0x40,0xf9,0x08,0x3d,0x40,0xf9,0x17,0xa1,0x40,0x39,0xd7,0x08,0x00,0x34,0xf4,0x03,0x1f,0xaa,0x05,0x00,0x00,0x14,0x9b,0x08,0x00,0x37,0x94,0x06,0x00,0x91}},
 {0x169a2e0,{0xc8,0x0a,0x40,0xf9,0x09,0xce,0x8d,0x52,0x29,0x00,0xa0,0x72,0x09,0x69,0x69,0x38,0x3f,0x79,0x00,0xf1,0x22,0x04,0x00,0x54,0x08,0x0d,0x09,0x8b,0x0a,0x50,0x8d,0x52}},
 {0x25de1c0,{0x61,0x01,0x00,0xb4,0x29,0x00,0x40,0xf9,0x68,0x9a,0x40,0xb9,0x29,0x0d,0x40,0xf9,0x28,0x25,0xc8,0x9a,0x08,0x01,0x00,0x12,0x68,0x52,0x02,0x39,0x28,0x00,0x40,0xf9}}
}};
uint32_t U32(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint64_t U64(const uint8_t*p){return U32(p)|uint64_t(U32(p+4))<<32;}
uint64_t Add(uint64_t a,uint64_t n){if(!a||n>UINT64_MAX-a)throw std::runtime_error("range");return a+n;}
struct Trace {
 const ReadFn& read;
 std::vector<std::pair<uint64_t,std::vector<uint8_t>>> records;
 size_t budget{};
 std::vector<uint8_t> Get(uint64_t at,size_t n){
  if(!at||!n||n>8192||n>65536-budget||records.size()>=512||n>UINT64_MAX-at)throw std::runtime_error("bounded");
  std::vector<uint8_t>b(n);if(!read(at,b.data(),n))throw std::runtime_error("read");budget+=n;records.emplace_back(at,b);return b;
 }
 uint64_t Ptr(uint64_t at){if(at%8)throw std::runtime_error("alignment");const auto b=Get(at,8);auto p=U64(b.data());if(!p||p%8)throw std::runtime_error("pointer");return p;}
 bool Replay()const {for(const auto&[at,b]:records){std::vector<uint8_t>now(b.size());if(!read(at,now.data(),now.size())||now!=b)return false;}return true;}
};
void Epoch(Trace&t,uint64_t base,const MapEpochSource&s){
 auto access=t.Ptr(Add(base,0x34bc848));auto save=t.Ptr(Add(access,0x10));
 if(access!=s.save_epoch.access_object||save!=s.save_epoch.save_data||t.Ptr(Add(save,0x16cf8))!=s.save_epoch.list||t.Ptr(Add(save,0x16b80))!=s.traversal)throw std::runtime_error("save epoch");
 const auto header=t.Get(s.save_epoch.list,32);
 if(U32(header.data()+16)>MaxLearnedKeys||U32(header.data()+24)!=s.save_epoch.revision||t.Ptr(Add(base,0x34b80b8))!=s.registry_owner||t.Ptr(Add(s.registry_owner,0x78))!=s.registry_header||t.Ptr(Add(s.registry_header,0x20))!=s.registry_entries)throw std::runtime_error("map epoch");
}
struct Copy {
 uint64_t owner{},header{},entries{},record{};uint8_t index{};std::array<uint8_t,32> raw{};
 bool operator==(const Copy&)const=default;
};
Copy Sample(Trace&t,uint64_t base,const MapSnapshot&expected,const DungeonMapDefinition&map){
 Epoch(t,base,expected.source);Copy out;
 out.owner=t.Ptr(Add(base,0x34b8050));
 if(t.Ptr(out.owner)!=Add(base,0x3433078))throw std::runtime_error("registry type");
 out.header=t.Ptr(Add(out.owner,0x78));
 const auto h=t.Get(out.header,48);
 if(U32(h.data())!=0x54485347||U32(h.data()+4)!=2||U32(h.data()+8)!=0x8e97cc21||U32(h.data()+24)!=1||U32(h.data()+28)!=1)throw std::runtime_error("native sheet");
 out.entries=t.Ptr(Add(out.header,0x20));
 const auto count=t.Get(Add(out.header,0x28),1)[0];if(!count||count>30||map.major_index>=count)throw std::runtime_error("registry count");
 const auto names=t.Get(out.entries,size_t(count)*16);std::set<std::string> unique;bool found=false;
 for(size_t i=0;i<count;++i){const auto*p=names.data()+i*16;auto pointer=U64(p);auto length=U64(p+8);if(!pointer||!length||length>63)throw std::runtime_error("name range");
  const auto raw=t.Get(pointer,size_t(length));std::string key(raw.begin(),raw.end());
  if(!std::all_of(key.begin(),key.end(),[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';})||!unique.insert(key).second)throw std::runtime_error("name");
  if(key==map.key){if(i!=map.major_index)throw std::runtime_error("ordinal");found=true;out.index=uint8_t(i);}
 }
 if(!found)throw std::runtime_error("unregistered");
 out.record=t.Ptr(Add(expected.source.save_epoch.save_data,0x16a80+uint64_t(out.index)*8));
 const auto bytes=t.Get(out.record,out.raw.size());
 // Original constructor236c3e4 and loader237ca70 retain this serialized
 // record tag/version and32-byte size. There is no vtable at record+0.
 if(U32(bytes.data())!=0x16744||U32(bytes.data()+8)!=32)throw std::runtime_error("record type");
 std::copy(bytes.begin(),bytes.end(),out.raw.begin());
 Epoch(t,base,expected.source);return out;
}
} // namespace
DungeonProgressSnapshot ReadDungeonProgress(const ReadFn&read,uint64_t base,bool build,const MapSnapshot&expected,const DungeonMapDefinition&map){
 DungeonProgressSnapshot out;
 if(!read||!build||!base||base%0x1000||base>UINT64_MAX-0x34bf100||!expected.compatible||!expected.discovery_valid||!expected.map_count||expected.map_count>MaxTraversalMaps||map.key.empty()||map.key.size()>63||map.major_index>=30||map.floor_count<1||map.floor_count>10)return out;
 try {
  for(const auto&g:IdentityGuards){std::array<uint8_t,256>b{};if(!read(Add(base,g.offset),b.data(),b.size())||!MatchSupportedGuard(b,g.bytes,g.offset))return out;}
  for(const auto&g:Guards){std::array<uint8_t,32>b{};if(!read(Add(base,g.offset),b.data(),b.size())||!MatchSupportedGuard(b,g.bytes,g.offset))return out;}
  out.compatible=true;
  Trace a{read,{},0},b{read,{},0},c{read,{},0};auto first=Sample(a,base,expected,map);auto second=Sample(b,base,expected,map);auto third=Sample(c,base,expected,map);
  if(first!=second||first!=third||!a.Replay()||!b.Replay()||!c.Replay())return out;
  out.valid=true;out.key=map.key;out.native_index=first.index;out.visited_rooms=U64(first.raw.data()+0x18);out.map_item=(first.raw[0x12]&1)!=0;out.source=expected.source;out.registry_owner=first.owner;out.registry_header=first.header;out.registry_entries=first.entries;out.record=first.record;
 }catch(...){out.valid=false;out.key.clear();out.visited_rooms=0;out.map_item=false;}
 return out;
}
} // namespace echoes
