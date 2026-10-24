// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone target: compile this test only; it includes the isolated observer.
#include "echoes_dungeon_icons.cpp"
#include <cassert>
#include <cstring>
#include <map>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
using namespace echoes;
namespace {
struct Fixture {
 const uint64_t base=0x10000000;
 std::map<uint64_t,uint8_t> mem;
 MapSnapshot map; DungeonMapDefinition dungeon;
 uint64_t menu=0x70001000,view=0x70003000,icon=0x70005000,pane=0x70007000;
 void zero(uint64_t a,size_t n){for(size_t i=0;i<n;++i)mem[a+i]=0;}
 void word(uint64_t a,uint64_t v,size_t n=8){for(size_t i=0;i<n;++i)mem[a+i]=uint8_t(v>>(i*8));}
 void str(uint64_t a,std::string_view s){for(size_t i=0;i<s.size();++i)mem[a+i]=s[i];}
 ReadFn reader(){return [this](uint64_t a,void*p,size_t n){auto*q=static_cast<uint8_t*>(p);for(size_t i=0;i<n;++i){auto it=mem.find(a+i);if(it==mem.end())return false;q[i]=it->second;}return true;};}
 Fixture(){
  for(auto&g:Guards)for(size_t i=0;i<g.bytes.size();++i)mem[base+g.at+i]=g.bytes[i];
  dungeon.key="L_Dungeon001";dungeon.major_index=0;dungeon.rooms.push_back({});dungeon.rooms[0].number=3;dungeon.rooms[0].floors={1};dungeon.size_ratio=2;dungeon.floor_min=1;dungeon.floor_count=2;
  map.compatible=map.discovery_valid=map.current_level_valid=true;map.current_level_key=dungeon.key;
  auto&s=map.source;s.save_epoch.access_object=0x71000000;s.save_epoch.save_data=0x71001000;s.save_epoch.list=0x71020000;s.save_epoch.revision=7;s.traversal=0x71030000;s.registry_owner=0x71040000;s.registry_header=0x71050000;s.registry_entries=0x71060000;
  word(base+0x34bc848,s.save_epoch.access_object);word(s.save_epoch.access_object+16,s.save_epoch.save_data);word(s.save_epoch.save_data+0x16cf8,s.save_epoch.list);word(s.save_epoch.save_data+0x16b80,s.traversal);zero(s.save_epoch.list,32);word(s.save_epoch.list+24,7,4);word(base+0x34b80b8,s.registry_owner);word(s.registry_owner+0x78,s.registry_header);word(s.registry_header+0x20,s.registry_entries);
  uint64_t sentinel=base+0x34bf070,node=0x72000000;zero(sentinel,24);word(sentinel,node);word(sentinel+8,node);word(node,sentinel);word(node+8,sentinel);word(node+16,menu);
  word(menu,base+0x341ebf0);word(menu-32,base+0x34268d0);word(base+0x341ebf0+0x70,base+0x25c5c28);word(base+0x341ebf0+0x158,base+0x25c7494);
  zero(menu+0x88,3);word(menu+0x88,2,1);word(menu+0x8a,254,1);zero(menu+0x208,3);word(menu+0x208,2,1);word(menu+0x20a,254,1);
  word(menu+0x468,0,4);auto descriptor=0x72001000;word(menu+0x470,descriptor);word(menu+0x478,descriptor+72);word(menu+0x480,descriptor+72);zero(descriptor,72);str(descriptor,dungeon.key);word(descriptor+64,dungeon.key.size());
  auto buckets=0x72002000,pre=0x72003000,entry=0x72004000,key=0x72005000;word(base+0x34bcb18,buckets);word(base+0x34bcb20,1);word(buckets,pre);word(pre,entry);zero(entry,48);uint64_t hash=0xcbf29ce484222325;for(unsigned char c:dungeon.key)hash=(hash^c)*UINT64_C(0x100000001b3);word(entry+8,hash);word(entry+16,key);word(entry+24,dungeon.key.size());str(key,dungeon.key);word(entry+32,view);word(entry+40,view-32);word(view-32,base+0x34228a8);word(view,base+0x3422760);word(base+0x3422760+0x70,base+0x2601c54);word(base+0x3422760+0x20,base+0x25ff0e0);
  auto pairs=0x72006000;word(view+0x210,pairs);word(view+0x218,pairs+16);word(view+0x220,pairs+16);word(pairs,icon);word(pairs+8,icon-32);zero(icon,0x100);word(icon,base+0x3421818);word(icon-32,base+0x34219c8);word(base+0x3421818+0x50,base+0x24cb26c);word(base+0x3421818+0x58,base+0x24cb274);word(base+0x3421818+0x78,base+0x25f43ec);word(icon+0xf4,8,4);word(icon+0xe8,1,1);word(icon+0x18,0x72007000);word(0x72007018,pane);zero(pane+0x30,0x30);word(pane+0x30,std::bit_cast<uint32_t>(32.f),4);word(pane+0x34,std::bit_cast<uint32_t>(-16.f),4);word(pane+0x58,1,1);
  word(icon+0xec,12,4);word(icon+0xf0,3,4);word(icon+0xfc,0,4);
  uint64_t rb=0x73000000,rp=0x73001000,re=0x73002000,rk=0x73003000,room=0x73004000,rw=0x73005000,rpane=0x73006000,fpairs=0x73007000,floor=0x73008000,fw=0x73009000,fpane=0x7300a000;
  std::string roomkey="0_"+dungeon.key+"_12_0";uint64_t rh=0xcbf29ce484222325;for(unsigned char c:roomkey)rh=(rh^c)*UINT64_C(0x100000001b3);
  word(view+0x3d0,rb);word(view+0x3d8,1);word(rb,rp);word(rp,re);zero(re,64);word(re+8,rh);word(re+0x20,rk);word(re+0x28,roomkey.size());str(rk,roomkey);word(re+0x30,room);word(re+0x38,room-32);
  word(room,base+0x341fa10);word(room-32,base+0x3422918);word(base+0x341fa10+0x50,base+0x24cb26c);word(room+0x98,3,4);word(room+0xf8,0,4);word(room+0x18,rw);word(rw+0x18,rpane);zero(rpane+0x30,8);
  word(view+0x420,1,4);word(view+0x418,2,4);word(view+0x41c,1,4);word(view+0x450,fpairs);word(view+0x458,fpairs+16);word(view+0x460,fpairs+16);word(fpairs,floor);word(fpairs+8,floor-32);word(floor,base+0x341fa10);word(floor+0x18,fw);word(fw+0x18,fpane);zero(fpane+0x30,8);
  word(icon+0x1e8,0x74010000);word(icon+0x1f0,0x74020000);
  uint64_t tb=0x74000000,tp=0x74001000,tn=0x74002000,nb=0x74003000,np=0x74004000,ne=0x74005000;
  word(base+0x34c8f88,tb);word(base+0x34c8f90,1);word(tb,tp);word(tp,tn);zero(tn,0x28);word(tn+8,17);word(tn+0x10,17,1);word(tn+0x18,nb);word(tn+0x20,1);word(nb,np);word(np,ne);zero(ne,0x68);
  std::string tex="TreasureBox_Base";uint64_t th=0xcbf29ce484222325;for(unsigned char c:tex)th=(th^c)*UINT64_C(0x100000001b3);word(ne+8,th);str(ne+0x10,tex);word(ne+0x50,tex.size());word(ne+0x58,0x74010000);word(ne+0x60,0x74020000);

 }
 DungeonIconsSnapshot read(){return ReadDungeonIcons(reader(),base,true,map,dungeon);}
};
}
int main(){
 if(const char* segments=std::getenv("ECHOES_SEGMENTS")){
  std::ifstream file(std::filesystem::path(segments)/"text.bin",std::ios::binary);assert(file);
  for(const auto& guard:Guards){std::array<uint8_t,32> original{};file.seekg(guard.at);file.read(reinterpret_cast<char*>(original.data()),original.size());assert(file && original==guard.bytes);}
 }

 Fixture f;auto s=f.read();assert(s.valid&&s.compatible&&s.icons.size()==1&&s.icons[0].sprite==DungeonIconSprite::TreasureBox);assert(s.icons[0].predicate_visible&&s.icons[0].pane_visible&&s.icons[0].projection_known&&s.icons[0].pane_x==32.f);
 f.word(f.icon+0xe8,0,1);s=f.read();assert(s.valid&&!s.icons[0].predicate_visible);f.word(f.icon+0xe8,1,1);
 f.word(f.pane+0x58,0,1);s=f.read();assert(s.valid&&!s.icons[0].pane_visible);f.word(f.pane+0x58,1,1);
 f.word(f.menu+0x88,0,1);assert(!f.read().valid);f.word(f.menu+0x88,2,1);assert(f.read().valid);
 f.map.current_level_key="Field";assert(!f.read().valid);f.map.current_level_key=f.dungeon.key;
 f.word(f.icon+0xf4,4,4);assert(!f.read().valid);f.word(f.icon+0xf4,8,4);
 f.word(f.pane+0x30,0x7fc00000,4);assert(!f.read().valid);f.word(f.pane+0x30,0,4);
 f.word(f.map.source.save_epoch.list+24,8,4);assert(!f.read().valid);f.word(f.map.source.save_epoch.list+24,7,4);
 f.mem[f.base+0x25f43ec]^=1;assert(!f.read().compatible);f.mem[f.base+0x25f43ec]^=1;
 auto r=f.reader();unsigned n=0;ReadFn changing=[&](uint64_t a,void*p,size_t z){bool ok=r(a,p,z);if(a==f.icon&&z==0x100&&++n==2)static_cast<uint8_t*>(p)[0xe8]^=1;return ok;};assert(!ReadDungeonIcons(changing,f.base,true,f.map,f.dungeon).valid);
 s=f.read();assert(s.valid&&s.icons[0].floor_known&&s.icons[0].room==3&&s.icons[0].floor==1);
 DungeonProgressSnapshot progress;progress.compatible=progress.valid=true;progress.key=f.dungeon.key;progress.source=f.map.source;progress.visited_rooms=8;
 DungeonMapArt art;art.key=f.dungeon.key;DungeonRoomArt ra;ra.room=3;art.rooms.push_back(ra);
 assert(DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));assert(!DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,2));progress.visited_rooms=0;assert(!DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));progress.map_item=true;assert(DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));art.rooms[0].position_x=1;assert(!DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));art.rooms[0].position_x=0;
 auto uv=ProjectDungeonIcon(s.icons[0],{-128,128,256,256},930,842);assert(uv&&(*uv)[0]>=0.5f&&(*uv)[1]>0.5f);assert(!ProjectDungeonIcon(s.icons[0],{-128,128,0,256},930,842));
 auto copy=s.icons[0];copy.native_x=10000;assert(!ProjectDungeonIcon(copy,{-128,128,256,256},930,842));copy=s.icons[0];copy.projection_known=false;assert(!ProjectDungeonIcon(copy,{-128,128,256,256},930,842));
 f.dungeon.rooms[0].floors.push_back(2);s=f.read();assert(s.valid&&s.icons[0].projection_known);
 f.word(0x730040f8,9,4);s=f.read();assert(s.valid&&!s.icons[0].projection_known);f.word(0x730040f8,0,4);
 s=f.read();assert(s.icons[0].projection_known);progress.source.save_epoch.revision++;assert(!DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));progress.source=f.map.source;
 f.word(f.icon-32,0,8);assert(!f.read().valid);f.word(f.icon-32,f.base+0x34219c8);
 f.word(f.icon+0x1e8,0x74010008);s=f.read();assert(s.valid&&s.icons[0].sprite==DungeonIconSprite::Unknown&&!DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));f.word(f.icon+0x1e8,0x74010000);
 std::string opened="TreasureBoxOpen_Base";uint64_t oh=0xcbf29ce484222325;for(unsigned char c:opened)oh=(oh^c)*UINT64_C(0x100000001b3);f.zero(0x74005010,64);f.str(0x74005010,opened);f.word(0x74005008,oh);f.word(0x74005050,opened.size());s=f.read();assert(s.valid&&s.icons[0].sprite==DungeonIconSprite::TreasureBoxOpen&&DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));
 f.word(f.view+0x420,2,4);s=f.read();assert(s.valid&&s.native_floor_known&&s.native_floor==2&&!DungeonIconVisible(s.icons[0],s,progress,f.dungeon,art,1));f.word(f.view+0x420,0,4);s=f.read();assert(s.valid&&!s.native_floor_known);f.word(f.view+0x420,1,4);
 assert(f.read().valid);std::cout<<"dungeon icon observer: native visibility, closure, role, epoch, code, finite position and mutation gates pass\n";
}
