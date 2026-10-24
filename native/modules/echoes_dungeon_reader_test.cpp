// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
namespace {
size_t checks{};void Check(bool x){++checks;if(!x)throw std::runtime_error("dungeon reader check "+std::to_string(checks));}
std::vector<uint8_t> File(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
uint32_t U32(const uint8_t*p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint64_t U64(const uint8_t*p){return U32(p)|uint64_t(U32(p+4))<<32;}
constexpr uint64_t Base=0x80400000,Access=0x100000,Save=0x101000,List=0x120000,Traversal=0x121000,MapOwner=0x130000,MapHeader=0x131000,MapNames=0x132000,Owner=0x140000,Header=0x141000,Names=0x142000,Record=0x160000;
struct Memory{
 std::map<uint64_t,uint8_t> bytes;std::vector<uint8_t>text,rodata;size_t reads{},record_reads{};bool fail{},throws{},tear{},replay_tear{};
 void Put(uint64_t a,const void*p,size_t n){const auto*b=static_cast<const uint8_t*>(p);for(size_t i=0;i<n;++i)bytes[a+i]=b[i];}
 void Q(uint64_t a,uint64_t x){for(size_t i=0;i<8;++i)bytes[a+i]=uint8_t(x>>(i*8));}
 void D(uint64_t a,uint32_t x){for(size_t i=0;i<4;++i)bytes[a+i]=uint8_t(x>>(i*8));}
 bool Read(uint64_t a,void*p,size_t n){++reads;if(throws)throw std::runtime_error("guest");if(fail)return false;
  if(a==Record&&++record_reads==(replay_tear?4u:2u)&& (tear||replay_tear))bytes[Record+0x18]^=2;
  if(a>=Base&&a-Base<text.size()&&n<=text.size()-(a-Base)){std::memcpy(p,text.data()+a-Base,n);return true;}
  if(a>=Base+0x2837000&&a-Base-0x2837000<rodata.size()&&n<=rodata.size()-(a-Base-0x2837000)){std::memcpy(p,rodata.data()+a-Base-0x2837000,n);return true;}
  auto*b=static_cast<uint8_t*>(p);for(size_t i=0;i<n;++i){auto at=bytes.find(a+i);if(at==bytes.end())return false;b[i]=at->second;}return true;
 }
 echoes::ReadFn Fn(){return [this](uint64_t a,void*p,size_t n){return Read(a,p,n);};}
};
}
int main(){try{
 Memory m;echoes::MapSnapshot expected;echoes::DungeonMapDefinition map;map.key="L_Dungeon001";map.floor_count=2;
 Check(!echoes::ReadDungeonProgress(m.Fn(),Base,false,expected,map).compatible&&m.reads==0);
 const char*segments=std::getenv("ECHOES_SEGMENTS"),*romfs=std::getenv("ECHOES_ROMFS");if(!segments||!romfs){std::cout<<checks<<" checks; native fixtures skipped\n";return 77;}
 m.text=File(std::filesystem::path(segments)/"text.bin");m.rodata=File(std::filesystem::path(segments)/"rodata.bin");Check(!m.text.empty()&&!m.rodata.empty());
 expected.compatible=expected.discovery_valid=true;expected.map_count=28;expected.source={{Access,Save,List,7},Traversal,MapOwner,MapHeader,MapNames};
 m.Q(Base+0x34bc848,Access);m.Q(Access+0x10,Save);m.Q(Save+0x16cf8,List);m.Q(Save+0x16b80,Traversal);std::array<uint8_t,32>empty{};m.Put(List,empty.data(),32);m.D(List+24,7);
 m.Q(Base+0x34b80b8,MapOwner);m.Q(MapOwner+0x78,MapHeader);m.Q(MapHeader+0x20,MapNames);
 auto major=File(std::filesystem::path(romfs)/"region_common/datasheets/MajorDungeonDefinition.gsheet");Check(major.size()>48);const auto rows=U64(major.data()+32);const auto count=U32(major.data()+40);Check(count==27);
 m.Q(Base+0x34b8050,Owner);m.Q(Owner,Base+0x3433078);m.Q(Owner+0x78,Header);m.Put(Header,empty.data(),32);m.Put(Header+32,empty.data(),16);m.D(Header,0x54485347);m.D(Header+4,2);m.D(Header+8,0x8e97cc21);m.D(Header+24,1);m.D(Header+28,1);m.Q(Header+0x20,Names);m.bytes[Header+0x28]=uint8_t(count);
 for(size_t i=0;i<count;++i){auto q=major.data()+rows+i*16;auto p=U64(q),n=U64(q+8);Check(p<=major.size()&&n<=major.size()-p&&n<=63);uint64_t str=0x150000+i*128;m.Q(Names+i*16,str);m.Q(Names+i*16+8,n);m.Put(str,major.data()+p,size_t(n));}
 m.Q(Save+0x16a80,Record);m.Put(Record,empty.data(),empty.size());m.D(Record,0x16744);m.D(Record+8,32);m.bytes[Record+0x12]=1;m.Q(Record+0x18,(uint64_t(1)<<1)|(uint64_t(1)<<7)|(uint64_t(1)<<36));
 const auto baseline=m.bytes;
 auto s=echoes::ReadDungeonProgress(m.Fn(),Base,true,expected,map);Check(s.compatible&&s.valid&&s.key==map.key&&s.native_index==0&&s.map_item&&s.visited_rooms==((uint64_t(1)<<1)|(uint64_t(1)<<7)|(uint64_t(1)<<36))&&s.record==Record&&s.source==expected.source);
 m.Q(Record+0x18,0);m.bytes[Record+0x12]=0;auto unseen=echoes::ReadDungeonProgress(m.Fn(),Base,true,expected,map);Check(unseen.valid&&!unseen.map_item&&unseen.visited_rooms==0);
 for(int fault=0;fault<20;++fault){m.bytes=baseline;m.record_reads=0;m.fail=m.throws=m.tear=m.replay_tear=false;auto e=expected;auto d=map;
  if(fault==0)m.fail=true;
  if(fault==1)m.throws=true;
  if(fault==2)m.tear=true;
  if(fault==3)m.replay_tear=true;
  if(fault==4)m.D(List+24,8);
  if(fault==5)m.Q(Access+0x10,Save+8);
  if(fault==6)m.bytes[Header+0x28]=31;
  if(fault==7)m.bytes[Header+0x28]=0;
  if(fault==8)m.Q(Names+8,64);
  if(fault==9)m.Q(Save+0x16a80,Record+1);
  if(fault==10)m.Q(Save+0x16a80,0);
  if(fault==11)d.major_index=1;
  if(fault==12)d.key="OP_Dungeon";
  if(fault==13)e.source.registry_entries+=8;
  if(fault==14)m.Q(Names+16,0x150000);
  if(fault==15)m.Q(Header+0x20,UINT64_MAX-7);
  if(fault==16)m.D(Record,0x16745);
  if(fault==17)m.D(Record+8,16);
  if(fault==18)m.Q(Owner,Base+0x3433080);
  if(fault==19)m.D(Header+8,0);
  const auto invalid=echoes::ReadDungeonProgress(m.Fn(),Base,true,e,d);Check(!invalid.valid&&invalid.visited_rooms==0&&!invalid.map_item&&invalid.key.empty());
 }
 m.bytes=baseline;m.fail=m.throws=m.tear=m.replay_tear=false;m.text[0x25de1c0]^=1;Check(!echoes::ReadDungeonProgress(m.Fn(),Base,true,expected,map).compatible);
 std::cout<<checks<<" native dungeon progress guard/race checks passed\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
