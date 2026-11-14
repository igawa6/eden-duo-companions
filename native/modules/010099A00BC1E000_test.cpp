// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_reader.h"
#include "re4_slots.h"
#include "core/mods/dsmod_module_extensions.h"
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t, uint64_t);
#include "re4_map.h"
#include "re4_map_publish.h"
#include "re4_hud_host.h"
#include "re4_equip.h"
#include <set>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <iostream>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
namespace {
std::vector<re4::MapMesh> MapMeshes;
auto TestMapPose(std::string_view area,const std::array<float,3>& pos,const std::array<float,2>& facing){
 return re4::MapPose(area,pos,facing,nullptr,MapMeshes);
}
auto TestMapFloor(std::string_view area,const std::array<float,3>& pos){return re4::MapFloor(area,pos,MapMeshes);}
size_t RomRead(void*,const char* path,uint64_t offset,void* out,size_t n){
 // This suite keeps inventory-name refusal tests synthetic; names have their own golden.
 if(std::string_view(path)=="bio4/etc/core.udas")return 0;
 const char* root=std::getenv("RE4_TEST_ROMFS");if(!root)return 0;
 std::ifstream f(std::string(root)+"/"+path,std::ios::binary|std::ios::ate);if(!f)return 0;
 const auto size=f.tellg();if(!out)return size;if(offset>uint64_t(size))return 0;
 f.seekg(offset);f.read(static_cast<char*>(out),n);return f.gcount();
}

struct Fixture {
 std::map<uint64_t,uint8_t> mem;
 std::map<std::string,int64_t> ints;
 std::map<std::string,unsigned> publications;
 std::map<std::string,std::string> texts;
 std::map<std::string,double> floats;
 EdenDsmodHostApi host{};
 int reads=0; size_t read_bytes=0, max_read=0; int legend_publishes=0;
 template<class T> void Put(uint64_t a,T value){const auto* p=reinterpret_cast<const uint8_t*>(&value);for(size_t i=0;i<sizeof value;++i)mem[a+i]=p[i];}
 Fixture(){
  host.read_romfs=RomRead;
  host.abi_version=EDEN_DSMOD_MODULE_ABI_VERSION;host.abi_hash=EDEN_DSMOD_MODULE_ABI_HASH;
  host.struct_size=sizeof host;host.title_id=re4::Title;host.main_base=0x80000000;host.main_size=0xC00000;host.userdata=this;
  for(int i=0;i<16;++i){auto nib=[](char c){return c<='9'?c-'0':c-'A'+10;};host.build_id[i]=nib(re4::Build[i*2])*16+nib(re4::Build[i*2+1]);}
  host.is_mapped=[](void* u,uint64_t a,uint64_t n)->EdenDsmodBool {auto& f=*static_cast<Fixture*>(u);for(uint64_t i=0;i<n;++i)if(!f.mem.contains(a+i))return false;return true;};
  host.read_memory=[](void* u,uint64_t a,void* out,size_t n)->EdenDsmodBool {auto& f=*static_cast<Fixture*>(u);++f.reads;f.read_bytes+=n;f.max_read=std::max(f.max_read,n);if(!f.host.is_mapped(u,a,n))return false;for(size_t i=0;i<n;++i)static_cast<uint8_t*>(out)[i]=f.mem[a+i];return true;};
  host.publish_i64=[](void* u,const char* k,int64_t v){auto& f=*static_cast<Fixture*>(u);f.ints[k]=v;++f.publications[k];if(std::string_view(k).starts_with("re4.map.room.") || std::string_view(k).starts_with("re4.map.door.") || std::string_view(k).starts_with("re4.map.goal.") || std::string_view(k).starts_with("re4.map.save.") || std::string_view(k)=="re4.map.native_area")++f.legend_publishes;};
  host.publish_text=[](void* u,const char* k,const char* v){auto& f=*static_cast<Fixture*>(u);f.texts[k]=v;++f.publications[k];if(std::string_view(k).starts_with("re4.map.room.") || std::string_view(k).starts_with("re4.map.door."))++f.legend_publishes;};
  host.publish_f64=[](void* u,const char* k,double v){static_cast<Fixture*>(u)->floats[k]=v;};
  Put<uint64_t>(host.main_base+0xB25D78,0x80B00000);Put<uint64_t>(0x80B00000,0x82000000);
  Put<uint64_t>(host.main_base+0xB25D88,0x80B00008);Put<uint64_t>(0x80B00008,0x2100000000);
  Put<uint8_t>(0x8200503C,0);
  Put<uint8_t>(0x82000028,3); // GameTask: interactive main loop, not a room load
  Put<uint32_t>(0x8200501C,1751510);Put<uint32_t>(0x82005098,0);
  Put(0x2100000128,std::array<float,3>{-93581.2f,-62.6f,-3904.4f});
  Put<float>(0x2100000018,0);Put<float>(0x2100000020,1);
 }
 // Synthetic valid floor centres keep room/gate tests independent of the spawn pose.
 void MapRoom(uint16_t room){
  Put<uint16_t>(0x82005020,room);
  const auto area=re4::MapArea(room,false);
  for(const auto& mesh:MapMeshes)if(mesh.area==area) {
   const auto& t=mesh.triangles.front();
   const auto& a=mesh.vertices[t.a];const auto& b=mesh.vertices[t.b];const auto& c=mesh.vertices[t.c];
   Put(0x2100000128,std::array<float,3>{float((a.x+b.x+c.x)/3),float((a.y+b.y+c.y)/3),float((a.z+b.z+c.z)/3)});
   return;
  }
 }
 void Medallions(){
  Put<uint64_t>(host.main_base+0xB26080,0x84000000);
  Put<uint64_t>(0x84000030,0x85000000);
  constexpr int indexes[]={3,18,8,20};
  constexpr int rooms[]={0x103,0x113,0x108,0x118};
  for(int n=0;n<4;++n){
   Put(0x85000000+indexes[n]*0xD8,std::array<uint8_t,0xD8>{});
   Put<uint16_t>(0x85000000+indexes[n]*0xD8,rooms[n]);
  }
 }
 void Legend(){
  Put(0x82005340,std::array<uint32_t,13>{});
  Put<uint64_t>(host.main_base+0xB26080,0x84000000);
  Put<uint64_t>(0x84000030,0x85000000);
  Put(0x85000000,std::array<uint8_t,re4::MapSaveCount*0xD8>{});
  for(const auto& row:re4::MapSaveRooms)Put<uint16_t>(0x85000000+row.index*0xD8,row.room);
  Put<uint64_t>(host.main_base+0xB26C98,0x86000000);
  Put<uint8_t>(0x86000448,0);Put(0x8600002C,std::array<uint32_t,2>{1,0});
 }
 void Flag(unsigned flag,bool set=true,bool key=false){
  const uint64_t address=0x82005340+(key?32:0)+4*(flag/32);
  uint32_t word=0;for(int i=0;i<4;++i)word|=uint32_t(mem[address+i])<<(8*i);
  const uint32_t mask=0x80000000u>>(flag%32);
  Put<uint32_t>(address,set?word|mask:word&~mask);
 }
 void Passed(uint16_t room,bool set=true){
  for(const auto& row:re4::MapSaveRooms)if(row.room==room) {
   Put<uint8_t>(0x85000000+row.index*0xD8+2,set?0x80:0);return;
  }
  assert(false);
 }
 void Sample(const EdenDsmodModuleApi* api,void* module){
  const auto previous_reads=reads;const auto previous_bytes=read_bytes,previous_max=max_read;
  const auto previous_legend=legend_publishes;const auto previous_publications=publications;
  auto sample=[&]{
   std::erase_if(ints,[](const auto& entry){return !entry.first.starts_with("@");});
   texts.clear();floats.clear();api->sample(module,&host);
  };
  sample();
  if(!MapMeshes.empty()&&!ints["re4.geometry.ready"]){
   const auto key=texts["re4.geometry.art"];
   const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
   assert(ext->load_image(module,&host,key.c_str(),nullptr,[](void*,uint32_t,uint32_t,const uint8_t*,size_t){}));
   // Model a completed worker turn; count only the final publication sample below.
   reads=previous_reads;read_bytes=previous_bytes;max_read=previous_max;
   legend_publishes=previous_legend;publications=previous_publications;sample();
  }
 }
 re4::Snapshot Poll(){return re4::Resolver(host,re4::Build.data()).Poll();}
 void Items(){
  Put<uint64_t>(host.main_base+0xB25F60,0x83000000);
  Put<uint64_t>(0x83000020,0x2200000000);Put<int32_t>(0x83000030,3);
  Put<uint8_t>(0x8300001B,0);Put<uint16_t>(0x83000018,0x27);
  Put<uint64_t>(0x83000010,0x220000000E);
  for(int n=0;n<3;++n)Put(0x2200000000+n*14,std::array<uint8_t,14>{});
  Put(0x220000000E,std::array<uint8_t,14>{0x27,0,1,0,1,0,0x26,0x52,0x18,1,2,1,0,1});
 }
};
}
int main(){
 const char* rom=std::getenv("RE4_TEST_ROMFS");
 const bool have_geometry=rom&&std::filesystem::is_regular_file(std::string(rom)+"/bio4/ss/cmn/map_obj1.dat");
 if(have_geometry){
  re4::GeometryCache geometry;
  for(unsigned stage:{1,2,3,5}){assert(geometry.Warm(nullptr,stage,rom));
   const auto data=geometry.Get(stage);MapMeshes.insert(MapMeshes.end(),data->meshes.begin(),data->meshes.end());
  }
 }else std::cout<<"SKIP: fixture-dependent map groups (set RE4_TEST_ROMFS); other groups run\n";
 // Exercise the actual asynchronous worker boundary without the convenience Sample helper.
 if(have_geometry) {
  Fixture f;f.MapRoom(0x100);f.Legend();f.Flag(29);
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
  auto* m=api->create(&f.host,nullptr);assert(m);
  auto sample=[&]{f.ints.clear();f.texts.clear();api->sample(m,&f.host);};
  auto warm=[&](const std::string& key){return ext->load_image(m,&f.host,key.c_str(),nullptr,[](void*,uint32_t,uint32_t,const uint8_t*,size_t){});};
  sample();assert(!f.ints["re4.map.available"]&&f.ints["re4.map.native_area"]==-1);
  const auto failed_key=f.texts["re4.geometry.art"];
  f.host.read_romfs=nullptr;assert(!warm(failed_key));sample();
  assert(!f.ints["re4.map.available"]&&f.texts["re4.geometry.art"]!=failed_key);
  f.host.read_romfs=RomRead;
  const auto village=f.texts["re4.geometry.art"];assert(warm(village));sample();assert(f.ints["re4.map.available"]);
  f.MapRoom(0x201);sample();assert(!f.ints["re4.map.available"]&&f.texts["re4.map.area"].empty());
  const auto castle=f.texts["re4.geometry.art"];
  assert(warm(village));sample();assert(!f.ints["re4.geometry.ready"]);
  assert(warm(castle));sample();assert(f.ints["re4.geometry.ready"]&&f.ints["re4.map.available"]);
  f.MapRoom(0x100);sample();assert(!f.ints["re4.map.available"]);
  assert(warm(castle));sample();assert(!f.ints["re4.geometry.ready"]);
  assert(warm(f.texts["re4.geometry.art"]));sample();assert(f.ints["re4.map.available"]);
  api->destroy(m);
 }
 if(std::getenv("RE4_BENCHMARK")) {
  if(!have_geometry)return 77;
  Fixture f;f.Items();f.MapRoom(0x100);f.Legend();f.Flag(29);
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);
  for(int n=0;n<50;++n)f.Sample(api,m);
  constexpr int iterations=2000;
  f.legend_publishes=0;
  const auto start=std::chrono::steady_clock::now();
  for(int n=0;n<iterations;++n)f.Sample(api,m);
  const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/iterations;
  size_t room_rows=0,door_rows=0,mark_rows=0;
  for(const auto& [key,value]:f.ints) {
   if(key.starts_with("re4.map.room.") && key.ends_with(".state"))++room_rows;
   if(key.starts_with("re4.map.door.") && key.ends_with(".state"))++door_rows;
   if((key.starts_with("re4.map.save.") || key.starts_with("re4.map.goal.")) && value)++mark_rows;
  }
  std::cout<<"visible_rooms="<<room_rows<<" doors="<<door_rows<<" marks="<<mark_rows
           <<" snapshot_values="<<f.ints.size()+f.texts.size()+f.floats.size()<<" ";
  std::cout<<"sample_us="<<us<<" legend_publishes="<<f.legend_publishes/iterations<<" iterations="<<iterations<<"\n";
  api->destroy(m);return 0;
 }

 using namespace re4;
 int groups=0;
 {assert(MapFlag(std::array<uint32_t,2>{0x80000001,0x40000000},0)==true);
  assert(MapFlag(std::array<uint32_t,2>{0x80000001,0x40000000},31)==true);
  assert(MapFlag(std::array<uint32_t,2>{0x80000001,0x40000000},33)==true);
  assert(!MapFlag(std::array<uint32_t,1>{},32));++groups;}
 {assert(NativeRoomTint(true,false,{},{},{})==RoomTint::Current);
  assert(NativeRoomTint(true,{},{},{},{})==RoomTint::Hidden);
  assert(NativeRoomTint(false,true,true,false,true)==RoomTint::Hidden);
  assert(NativeRoomTint(false,false,false,false,false)==RoomTint::Hidden);
  assert(NativeRoomTint(false,false,true,false,false)==RoomTint::NotEntered);
  assert(NativeRoomTint(false,false,false,false,true)==RoomTint::Entered);
  assert(NativeRoomTint(false,false,true,true,true)==RoomTint::Sealed);
  assert(NativeRoomTint(false,false,true,false,{})==RoomTint::Hidden);++groups;}
 {assert(NativeDoorTint(0,{},{})==DoorTint::Open);
  assert(NativeDoorTint(3,true,true)==DoorTint::Hidden);
  for(unsigned type:{1,2}) {
   assert(NativeDoorTint(type,true,{})==DoorTint::Open);
   assert(NativeDoorTint(type,false,false)==DoorTint::Locked);
   assert(NativeDoorTint(type,false,true)==DoorTint::HaveKey);
   assert(NativeDoorTint(type,false,{})==DoorTint::Hidden);
   assert(NativeDoorTint(type,{},true)==DoorTint::Hidden);
  }++groups;}
 {const std::array<uint16_t,3> flags{0,1,33};
  assert(NativeGoalPart(std::array<uint32_t,2>{},flags)==0);
  assert(NativeGoalPart(std::array<uint32_t,2>{0x40000000,0},flags)==1);
  assert(NativeGoalPart(std::array<uint32_t,2>{0x40000000,0x40000000},flags)==2);
  assert(!NativeGoalPart(std::array<uint32_t,1>{},flags));++groups;}
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);f.Legend();
  auto room=[&](std::string_view key){for(const auto& row:MapStateRooms)if(row.key==key)return row;assert(false);return MapStateRooms[0];};
  auto s=f.Poll();assert(NativeMapArea(s,"village-100")==0);
  assert(MapRoomTint(s,room("village-100"),0)==RoomTint::Current);
  assert(MapRoomTint(s,room("village-101"),0)==RoomTint::Hidden);
  assert(!MapGoalPart(s,0));
  f.Flag(29);s=f.Poll();assert(NativeMapArea(s,"village-100")==1);
  assert(MapRoomTint(s,room("village-101"),1)==RoomTint::NotEntered);
  f.Passed(0x101);s=f.Poll();assert(MapRoomTint(s,room("village-101"),1)==RoomTint::Entered);
  f.Flag(28);s=f.Poll();assert(MapRoomTint(s,room("village-104"),1)==RoomTint::Sealed);
  assert(MapRoomTint(s,room("castle-200"),1)==RoomTint::Hidden);
  assert(MapRoomTint(s,room("village-104"),2)==RoomTint::Hidden);
  // A corrupt identity does not become "unentered"; a missing refresh hides dependants.
  for(const auto& r:MapSaveRooms)if(r.room==0x101)f.Put<uint16_t>(0x85000000+r.index*0xD8,0x102);
  s=f.Poll();assert(MapRoomTint(s,room("village-101"),1)==RoomTint::Hidden);
  f.mem.erase(0x85000003);s=f.Poll();assert(!s.map_passed);
  assert(MapRoomTint(s,room("village-101"),1)==RoomTint::Hidden);
  f.mem.erase(0x82005340);s=f.Poll();assert(!s.map_flags && !NativeMapArea(s,"village-100"));
  ++groups;}
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);f.Legend();f.Flag(29);
  const auto door=*std::find_if(std::begin(MapDoorDefs),std::end(MapDoorDefs),[](const auto& d){return d.area==1&&d.index==3;});
  assert(door.type==2&&door.flag==12&&door.item==0x8B);
  auto s=f.Poll();assert(MapDoorTint(s,door,1)==DoorTint::Locked);
  f.Put(0x220000001C,std::array<uint8_t,14>{0x8B,0,1,0,1});
  s=f.Poll();assert(MapDoorTint(s,door,1)==DoorTint::HaveKey);
  f.Flag(12,true,true);s=f.Poll();assert(MapDoorTint(s,door,1)==DoorTint::Open);
  assert(MapDoorTint(s,door,0)==DoorTint::Hidden);
  s.owned_items.reset();f.Flag(12,false,true);s=f.Poll();s.owned_items.reset();
  assert(MapDoorTint(s,door,1)==DoorTint::Hidden);
  // Expanded Switch unlock flags exceed the original GC two-word array.
  const auto sw=*std::find_if(std::begin(MapDoorDefs),std::end(MapDoorDefs),[](const auto& d){return d.type==2&&d.flag==152;});
  f.Flag(152,true,true);s=f.Poll();assert(MapDoorTint(s,sw,sw.area)==DoorTint::Open);
  for(const auto& d:MapDoorDefs) {
   assert(d.flag<(d.type==2?160:256));assert(d.area<26);
  }++groups;}
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);f.Legend();f.Flag(29);
  assert(MapGoalPart(f.Poll(),1)==0);
  f.Flag(17);assert(MapGoalPart(f.Poll(),1)==1);
  f.Flag(167);assert(MapGoalPart(f.Poll(),1)==7);
  f.Flag(168);assert(MapGoalPart(f.Poll(),1)==8);
  f.host.get_i64=[](void* u,const char* key,int64_t fallback){auto& v=static_cast<Fixture*>(u)->ints;
   auto it=v.find(key);return it==v.end()?fallback:it->second;};
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);auto* m=api->create(&f.host,nullptr);
  f.Sample(api,m);
  assert(f.ints["re4.map.native_area"]==1&&f.ints["re4.map.goal.1.8.show"]==1);
  assert(f.ints["re4.map.goal.1.0.show"]==0&&f.ints["re4.map.save.1.0.show"]==1);
  assert(f.ints["re4.map.save.2.0.show"]==0&&f.ints["re4.treasure.show"]==0);
  f.ints["@flag:opt.map_spoilers"]=1;f.Sample(api,m);
  assert(f.ints["re4.treasure.show"]==1&&f.ints["re4.map.room.village-101.state"]==2);
  assert(f.ints["re4.map.door.1.3.state"]==2&&f.ints["re4.map.goal.1.8.show"]==1);
  assert(f.ints["re4.map.save.1.0.show"]==1);
  f.ints["@flag:opt.map_spoilers"]=0;
  f.Put<uint8_t>(0x86000448,1);f.Sample(api,m);
  assert(f.ints["re4.map.save.1.0.show"]==0&&f.ints["re4.map.goal.1.8.show"]==1);
  f.Flag(29,false);f.Sample(api,m);
  assert(f.ints["re4.map.native_area"]==0&&f.ints["re4.map.goal.1.8.show"]==0);
  assert(f.ints["re4.map.village"]==0);
  f.ints["@flag:opt.map_spoilers"]=1;f.Sample(api,m);
  assert(f.ints["re4.treasure.show"]==0&&f.ints["re4.medallion.show"]==0);
  for(int n=0;n<15;++n)assert(f.ints["re4.medallion."+std::to_string(n)+".show"]==0);
  assert(f.ints["re4.map.save.1.0.show"]==0&&f.ints["re4.map.door.1.3.1.show"]==0);
  f.mem.erase(0x82005340);f.Sample(api,m);
  assert(f.ints["re4.map.native_area"]==-1&&f.ints["re4.map.room.village-100.show"]==0);
  assert(f.texts["re4.map.room.village-100.art"].empty());
  api->destroy(m);++groups;}
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x201);f.Legend();f.Flag(40);f.Flag(33);f.Passed(0x214);
  auto s=f.Poll();auto room=[&](std::string_view key){for(const auto& r:MapStateRooms)if(r.key==key)return r;assert(false);return MapStateRooms[0];};
  assert(MapRoomTint(s,room("castle-214"),2)==RoomTint::Hidden);
  assert(MapRoomTint(s,room("castle-814"),2)==RoomTint::Entered);
  s.room=0x214;assert(MapRoomTint(s,room("castle-814"),2)==RoomTint::Current);
  assert(MapRoomTint(s,room("castle-214"),2)==RoomTint::Hidden);
  assert(MapRoomTint(s,room("castle-217"),2)==RoomTint::Hidden);
  s.room=0x217;assert(MapRoomTint(s,room("castle-217"),2)==RoomTint::Current);
  s.room=0x228;assert(MapRoomTint(s,room("castle-227"),3)==RoomTint::Hidden);
  s.room=0x227;assert(MapRoomTint(s,room("castle-228"),3)==RoomTint::Hidden);
  s.room=0x507;assert(MapRoomTint(s,room("stage5-509"),19,1)==RoomTint::Current);
  assert(MapRoomTint(s,room("stage5-509"),19,{})==RoomTint::Hidden);
  assert(MapRoomTint(s,room("stage5-509"),19,0)==RoomTint::NotEntered);
  (*s.map_flags)[1]|=0x10000;
  s.room=0x306;s.position=std::array<float,3>{0,1999,0};assert(NativeMapArea(s,"island-306")==6);
  s.position=std::array<float,3>{0,2000,0};assert(NativeMapArea(s,"island-306")==7);
  s.room=0x304;s.map_open=std::array<uint32_t,2>{2,1};assert(NativeMapArea(s,"island-304")==7);
  s.map_open.reset();assert(!NativeMapArea(s,"island-304"));++groups;}
 if(have_geometry) {bool upper=false,lower=false;
  for(const auto& mesh:MapMeshes)if(mesh.area=="stage5-507")for(const auto& t:mesh.triangles) {
   const auto& a=mesh.vertices[t.a];const auto& b=mesh.vertices[t.b];const auto& c=mesh.vertices[t.c];
   const auto floor=TestMapFloor(mesh.area,{float((a.x+b.x+c.x)/3),float((a.y+b.y+c.y)/3),float((a.z+b.z+c.z)/3)});
   upper|=floor==1;lower|=floor==0;
  }
  assert(upper&&lower);assert(!TestMapFloor("stage5-507",{1e8f,0,1e8f}));++groups;}
 {assert(SupportsBuild(Build.data()));assert(SupportsBuild("82c2a04a21d3e0b850f273d4149c9003"));
 assert(SupportsBuild((std::string(Build)+std::string(32,'0')).c_str()));
 assert(!SupportsBuild(nullptr));assert(!SupportsBuild("82C2A04A21D3E0B8"));
 assert(!SupportsBuild((std::string(Build)+std::string(31,'0')+"1").c_str()));
 assert(!SupportsBuild("92C2A04A21D3E0B850F273D4149C9003"));++groups;}
 {for(auto [ammo,weapons]:std::vector<std::pair<uint16_t,std::vector<uint16_t>>>{
    {4,{3,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x40}},
    {0x18,{0x2C,0x2D,0x94,0x47}},{7,{0x2E,0x2F,0x51,0x6B,0x6C,0x99}},
    {0x20,{0x30,0x31,0x32,0x33,0x3E}},{0,{0x29,0x2A,0x2B}},
    {0x46,{0x36,0xAB}},{0x6A,{0x34,0x53}},{0x1A,{0x37}},{0x72,{0x52}}})
    for(auto weapon:weapons)assert(AmmoItem(weapon)==ammo);
  for(uint16_t id:{1,2,14,0x17,0x35,0x6D,0x38,0x0A,0x10,0x41,0x800,0xFFFF})assert(!AmmoItem(id));
  // Every StripWeapon()-eligible id (the complete, shipped, live-verified 30-id weapon set --
  // analysis/20261101-r5-inventory.md S2b) is enumerated here exactly once: this is the "every
  // ammo-using weapon id in the game" completeness check, not just a handful of samples. Each id
  // either has a real ammo item (handgun/shotgun/rifle/TMP/magnum/mine darts/0x34/
  // 0x37/bowgun) or is confirmed no-ammo (knife ids 1/2/14, the rocket launcher family
  // 0x35/0x17/0x6D/0x38, and the beam weapon 0x41) or is an explicitly-open gap (0x10, a distinct
  // crossbow render with no live-verified ammo item yet -- analysis/20261109-reserve-ammo.md).
  {const std::vector<uint16_t> strip_weapons{
    3,0x21,0x40,0x23,0x25,0x27,0x29,0x2A,0x2C,0x2D,0x2E,0x2F,0x30,0x34,0x35,0x36,0x37,0x38,
    0x94,0x17,0x6D,0x3E,0x52,1,2,14,0x41,0x10,0x47,0x53};
   assert(strip_weapons.size()==30);
   for(uint16_t id:strip_weapons)assert(StripWeapon(id));
   const std::vector<uint16_t> no_ammo{1,2,14,0x17,0x35,0x38,0x6D,0x41,0x10};
   for(uint16_t id:strip_weapons) {
    const bool expect_no_ammo=std::find(no_ammo.begin(),no_ammo.end(),id)!=no_ammo.end();
    assert(expect_no_ammo==!AmmoItem(id).has_value());
   }
   assert(strip_weapons.size()==no_ammo.size()+/*ammo-mapped*/21u);
  }
  // 0x0A is deliberately NOT a StripWeapon id (the native English table calls it Gold Chicken
  // Egg; its 1x1 footprint matches 0x08/0x09). Confirm the gate
  // agrees so a future StripWeapon edit can't silently start treating it as equippable.
  assert(!StripWeapon(0x0A));
  assert(ReserveAmmo(0x27,{})==0);
  assert(ReserveAmmo(0x27,{{0,4,50,0,0,0},{1,4,17,0,0,0},{2,7,100,0,0,0}})==67);
  assert(ReserveAmmo(0x29,{{0,0,12,0,0,0}})==12);
  assert(!ReserveAmmo(0x35,{}));
  assert(ReserveAmmo(0x27,std::vector<PlacedItem>(384,{0,4,65535,0,0,0}))==25165440);
  ++groups;}
 // Loaded-ammo mirror table (analysis/20261114-ammo-mirror.md). Every StripWeapon id is
 // enumerated once: proven ids publish u16(record+8)>>3 up to the capacity of the record's
 // level; every other id (knives/grenades/rockets, infinite or special weapons, unreached
 // variants) never publishes a magazine at any level or value.
 {const std::vector<std::pair<uint16_t,uint16_t>> proven{ // id, level-0 capacity
    {0x23,10},{0x25,8},{0x21,10},{0x27,15},{3,15},{0x29,6},{0x2A,7},{0x2C,6},{0x2D,12},{0x94,7},
    {0x2E,5},{0x2F,10},{0x30,30},{0x36,5}};
  const std::vector<uint16_t> unproven{0x40,0x34,0x35,0x37,0x38,0x17,0x6D,0x3E,0x52,
    1,2,14,0x41,0x10,0x47,0x53};
  assert(proven.size()+unproven.size()==30);
  for(auto [id,first]:proven) {
   assert(StripWeapon(id)&&MagazineProven(id)&&MagazineCapacity(id));
   assert(LoadedRounds(id,0,first)==first&&!LoadedRounds(id,0,first+1));
   assert(LoadedRounds(id,0,0)==0);assert(!LoadedRounds(id,7,0)&&!LoadedRounds(id,15,0));
   const auto capacity=*MagazineCapacity(id);
   for(uint8_t level=0;level<7;++level) {
    const uint16_t max=capacity[level];
    if(!max){assert(!LoadedRounds(id,level,0));continue;} // level does not exist
    assert(max!=0xFFFF);assert(LoadedRounds(id,level,max)==max);assert(!LoadedRounds(id,level,max+1));
   }
  }
  for(uint16_t id:unproven) {
   assert(StripWeapon(id)&&!MagazineProven(id));
   for(uint8_t level=0;level<16;++level)for(uint16_t mag:{0,1,5,16,35,100,8191})assert(!LoadedRounds(id,level,mag));
  }
  // Live boundary values: weapon 0x27 byte 0x52 -> level 5 = 35, 0x2D byte 0x62 -> level 6 = 100,
  // weapon 0x03 level 6 = 100; weapons 0x29/0x2A have no level above 3/2.
  assert(LoadedRounds(0x27,5,35)==35&&!LoadedRounds(0x27,5,36));
  assert(LoadedRounds(0x2D,6,100)==100&&LoadedRounds(3,6,100)==100&&LoadedRounds(0x2E,5,18)==18);
  assert(!LoadedRounds(0x29,4,6)&&!LoadedRounds(0x2A,3,7));
  // Infinite levels (native dashes): weapons 0x37/0x34 at level 6 publish nothing,
  // and the shipped strip no longer prints weapon 0x37's zero magazine.
  assert((*MagazineCapacity(0x37))[6]==0xFFFF&&(*MagazineCapacity(0x34))[6]==0xFFFF);
  assert(SlotAmmo({3,0x37,1,19,7,0,0,6}).empty()&&SlotAmmo({1,0x34,1,6,2,0,16,6}).empty());
  assert(SlotAmmo({9,0x2E,1,8,14,0,18,5})=="18"&&SlotAmmo({13,0x2D,1,25,10,1,100,6})=="100");
  assert(SlotAmmo({35,2,3,2,5,0,0})=="3"); // grenades keep their stack count
  // Attached-part variants: only the live scoped-rifle pair maps to its base record.
  assert(EquippedRecordId(0x6B)==0x2E);
  for(uint16_t id:{0x2E,0x99,0x6C,0x51,0x26,0x32,0x22,0x24,0x28,0xAB,0x27})assert(EquippedRecordId(id)==id);
  ++groups;}
 {Fixture f;f.Items(); // native equips the scoped rifle as 0x6B while the record keeps 0x2E
  f.Put<uint16_t>(0x83000018,0x6B);
  f.Put(0x220000000E,std::array<uint8_t,14>{0x2E,0,1,0,1,0,0x06,0x52,0x90,0,8,14,0,1});
  auto s=f.Poll();assert(s.weapon==0x2E&&s.ammo==18&&s.equipped_slot==1);
  f.Put<uint8_t>(0x2200000016,0x88);assert(f.Poll().ammo==17);
  f.Put<uint16_t>(0x83000018,0x99);s=f.Poll();assert(!s.weapon&&!s.ammo); // unmapped variant
  f.Put<uint16_t>(0x83000018,0x37); // weapon 0x37 at the infinite level: weapon yes, magazine no
  f.Put(0x220000000E,std::array<uint8_t,14>{0x37,0,1,0,1,0,0x06,0x62,0,0,19,7,0,1});
  s=f.Poll();assert(s.weapon==0x37&&!s.ammo);
  f.Put<uint16_t>(0x83000018,0x2D); // weapon 0x2D exclusive capacity 100
  f.Put(0x220000000E,std::array<uint8_t,14>{0x2D,0,1,0,1,0,0x05,0x62,0x20,3,25,10,1,1});
  s=f.Poll();assert(s.weapon==0x2D&&s.ammo==100);
  f.Put<uint8_t>(0x2200000015,0x52);assert(!f.Poll().ammo); // 100 exceeds level-5 capacity 28
  ++groups;}
 {Fixture f;f.Items();
  f.Put(0x2200000000,std::array<uint8_t,14>{4,0,50,0,1,0,0,0,0,0,6,2,0,1});
  f.Put(0x220000001C,std::array<uint8_t,14>{4,0,17,0,1,0,0,0,0,0,8,2,0,1});
  assert(f.Poll().reserve_ammo==67);
  f.Put<uint16_t>(0x2200000002,49);assert(f.Poll().reserve_ammo==66);
  f.Put<uint8_t>(0x2200000020,0);assert(f.Poll().reserve_ammo==49); // inactive
  f.Put<uint8_t>(0x2200000005,1);assert(f.Poll().reserve_ammo==0); // other character
  f.Put<uint8_t>(0x2200000005,0);f.Put<uint8_t>(0x220000000D,0);
  assert(f.Poll().reserve_ammo==0); // spare board is not in the case
  f.Put<uint8_t>(0x220000000D,1);
  for(uint16_t weapon:{0x2E,0x29,0x36}) {
   f.Put<uint16_t>(0x83000018,weapon);f.Put<uint16_t>(0x220000000E,weapon);
   f.Put<uint16_t>(0x2200000000,*AmmoItem(weapon));assert(f.Poll().reserve_ammo==49);
  }
  f.Put<uint16_t>(0x83000018,0x35);f.Put<uint16_t>(0x220000000E,0x35);
  assert(!f.Poll().reserve_ammo);
  f.Put<uint16_t>(0x83000018,0x27);f.Put<uint16_t>(0x220000000E,0x27);
  f.Put<uint8_t>(0x220000000A,127);assert(!f.Poll().reserve_ammo); // invalid case
  f.Put<uint8_t>(0x220000000A,6);f.mem.erase(0x2200000029);
  assert(!f.Poll().reserve_ammo); // partial acquisition, no stale sum
  ++groups;}
 {Fixture f;f.Items();
  f.Put(0x2200000000,std::array<uint8_t,14>{4,0,45,0,1,0,0,0,0,0,6,2,0,1});
  f.host.get_i64=[](void*,const char* key,int64_t fallback)->int64_t {
   if(std::strcmp(key,"__aux_present")==0)return 1;
   if(std::strcmp(key,"@flag:opt.hide_ammo")==0)return 0;
   return fallback;
  };
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
  assert(f.ints["re4.ammo.reserve.available"]==1&&f.ints["re4.ammo.reserve"]==45);
  assert(f.texts["re4.ammo.reserve.text"]=="45");
  assert(f.ints["re4.ammo.display_available"]==0); // reserve does not bypass magazine helper gate
  f.Put<uint16_t>(0x83000018,0x2E);f.Put<uint16_t>(0x220000000E,0x2E);
  f.Sample(api,m);assert(f.ints["re4.ammo.reserve"]==0);
  assert(f.texts["re4.ammo.reserve.text"]=="0"); // available zero is explicit
  f.mem.erase(0x2200000029);f.Sample(api,m);
  assert(f.ints["re4.ammo.reserve.available"]==0&&f.ints["re4.ammo.reserve"]==0);
  assert(f.texts["re4.ammo.reserve.text"].empty());
  api->destroy(m);++groups;}
 // Regression: a real Ch1-1 pose needs the authored room translation.
 if(have_geometry) {const auto pose=TestMapPose("village-100",{-93581.2f,-62.6f,-3904.4f},{0,1});assert(pose);
  assert(std::abs((*pose)[0]-(-9358.12-15195.9248046875))<.01);
  assert(std::abs((*pose)[1]-(-(-390.44+3362.08544921875)))<.01);
  assert(!TestMapPose("village-10b",{-93581.2f,-62.6f,-3904.4f},{0,1}));
  assert(!TestMapPose("village-110",{0,0,0},{0,1}));
  assert(!*MapArea(0x814)); // outline-only until a live verification bracket exists
  ++groups;}
 // Every decoded main-campaign room has at least one valid synthetic projection.
 if(have_geometry) {for(const auto& mesh:MapMeshes) {
   bool hit=false;
   for(const auto& t:mesh.triangles) {
    const auto& a=mesh.vertices[t.a];const auto& b=mesh.vertices[t.b];const auto& c=mesh.vertices[t.c];
    const std::array<float,3> world{float((a.x+b.x+c.x)/3),float((a.y+b.y+c.y)/3),float((a.z+b.z+c.z)/3)};
    if(auto pose=TestMapPose(mesh.area,world,{0,1})) {
     assert(std::isfinite((*pose)[0])&&std::isfinite((*pose)[1]));
     if(mesh.area=="village-10b")assert((*pose)[0]>35000);
     if(mesh.area=="village-10d")assert(std::abs((*pose)[2]-130)<.01);
     const auto bounds=MapBounds(mesh.area);assert(bounds);
     assert((*pose)[0]>=(*bounds)[0]&&(*pose)[0]<=(*bounds)[2]);
     assert((*pose)[1]>=(*bounds)[1]&&(*pose)[1]<=(*bounds)[3]);
     hit=true;break;
    }
   }
   assert(hit);
  }++groups;}
 {const auto bounds=MapCamera("village-100",0,0,false);assert(bounds);
  for(double x:{-24000.,0.,68000.})for(double y:{-33000.,0.,22000.}) {
   const auto solo=MapCamera("village-100",x,y,false);
   const auto escort=MapCamera("village-100",x,y,true);assert(solo&&escort&&solo==escort);
   assert(std::abs(((*solo)[0]+(*solo)[2])/2-x)<1e-8);
   assert(std::abs(((*solo)[1]+(*solo)[3])/2-y)<1e-8);
   assert(std::abs(((*solo)[2]-(*solo)[0])-((*bounds)[2]-(*bounds)[0]))<1e-8);
  }
  assert(!MapCamera("",0,0,true));++groups;}
 {Fixture f;Snapshot s;s.placed_items=std::vector<PlacedItem>{{8,0x27,1,2,1,0,35,5},{35,2,1,2,5,0,0}};
  PublishSlots(f.host,s,"@flag:slots.assignment.0");
  assert(f.texts["re4.slot.0.badge"]=="file:assets/r5-badge-2.png");
  assert(f.texts["re4.slot.0.digit.0"]=="module:re4/digit/3");
  assert(f.texts["re4.slot.0.digit.1"]=="module:re4/digit/5");
  assert(f.ints["re4.slot.0.digit.1.x"]-f.ints["re4.slot.0.digit.0.x"]==19);
  assert(1081+f.ints["re4.slot.0.digit.0.x"]>=1081&&1081+f.ints["re4.slot.0.digit.1.x"]+23<=1204);
  assert(f.texts["re4.slot.1.badge"]=="file:assets/r5-badge-1.png");
  s.placed_items.reset();PublishSlots(f.host,s,"@flag:slots.assignment.0");
  assert(f.texts["re4.slot.0.badge"].empty()&&f.texts["re4.slot.0.digit.0"].empty());++groups;}
 {Fixture f;auto s=f.Poll();assert(s.money==1751510);assert(s.paused==false);assert(s.position&&s.facing);assert(s.state==State::Unknown);
 f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::Pause);
 f.Put<uint32_t>(0x82005098,0x10);assert(f.Poll().state==State::Unknown);++groups;}
 {Fixture f;f.Put<uint64_t>(f.host.main_base+0xB25D88,0);auto s=f.Poll();assert(!s.position&&!s.facing&&!s.money);++groups;}
 {Fixture f;f.Put<uint64_t>(0x80B00000,UINT64_MAX-4);auto s=f.Poll();assert(!s.money&&!s.paused&&s.position);++groups;}
 {Fixture f;f.host.main_size=4;assert(!f.Poll().money);assert(f.reads==0);++groups;}
 {Fixture f;f.host.main_base=UINT64_MAX-4;assert(!f.Poll().position);assert(f.reads==0);++groups;}
 {Fixture f;auto s=Resolver(f.host,"bad").Poll();assert(s.state==State::Unsupported&&f.reads==0);++groups;}
 {Fixture f;f.Put<uint32_t>(0x8200501C,100000000);assert(!f.Poll().money);++groups;}
 {Fixture f;f.Put<float>(0x2100000128,std::numeric_limits<float>::quiet_NaN());assert(!f.Poll().position);
 f.Put<float>(0x2100000128,10000001);assert(!f.Poll().position);++groups;}
 {Fixture f;f.Put<float>(0x2100000018,1);assert(!f.Poll().facing);
 f.Put<float>(0x2100000020,std::numeric_limits<float>::infinity());assert(!f.Poll().facing);++groups;}
 {Fixture f;f.mem.erase(0x8200501C);assert(!f.Poll().money&&f.Poll().paused.has_value());++groups;}
 {Fixture f;f.Items();auto s=f.Poll();assert(s.weapon==0x27&&s.ammo==35);
  assert(s.placed_items&&s.placed_items->size()==1);
  const auto& item=s.placed_items->front();assert(item.slot==1&&item.centre_x2==2&&item.centre_y2==1&&item.orientation==0);
  f.Put<uint8_t>(0x2200000016,0x10);assert(f.Poll().ammo==34);
  f.Put<uint8_t>(0x2200000018,1);f.Put<uint8_t>(0x2200000019,4);f.Put<uint8_t>(0x220000001A,1);
  s=f.Poll();assert(s.placed_items->front().centre_x2==1&&s.placed_items->front().centre_y2==4&&s.placed_items->front().orientation==1);++groups;}
 {Fixture f;f.Items();f.Put(0x82005028,std::array<int16_t,2>{1830,2400});
  assert((f.Poll().health==std::array<int16_t,2>{1830,2400}));
  f.Put<int16_t>(0x82005028,0);assert(f.Poll().health&&(*f.Poll().health)[0]==0);
  for(auto pair:{std::array<int16_t,2>{-1,2400},{2401,2400},{100,0},{100,2401}}){
   f.Put(0x82005028,pair);assert(!f.Poll().health);}
  f.Put(0x82005028,std::array<int16_t,2>{2400,2400});f.mem.erase(0x8200502B);
  assert(!f.Poll().health);++groups;}
 {Fixture f;f.Items();f.Put(0x82005028,std::array<int16_t,2>{2400,2400});
  f.Put<uint8_t>(0x8300001B,1);assert(!f.Poll().health);
  f.Put<uint8_t>(0x8300001B,0);f.Put<uint64_t>(0x80B00008,0);assert(!f.Poll().health);
  f.Put<uint64_t>(0x80B00008,0x2100000000);
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);assert(m);f.Sample(api,m);
  assert(f.ints["re4.hp.available"]==1&&f.ints["re4.hp"]==2400&&f.ints["re4.hp.max"]==2400);
  assert(f.ints["re4.hp.level"]==14);
  for(auto pair:{std::array<int16_t,2>{1200,7},{1,1},{0,0}}){
   f.Put(0x82005028,std::array<int16_t,2>{pair[0],2400});f.Sample(api,m);
   assert(f.ints["re4.hp.level"]==pair[1]);}
  f.mem.erase(0x82005028);f.Sample(api,m);
  assert(f.ints["re4.hp.available"]==0&&f.ints["re4.hp"]==0&&f.ints["re4.hp.max"]==0);
  assert(f.texts["re4.hp.text"]=="");api->destroy(m);++groups;}
 {Fixture f;f.Items();f.Put<uint64_t>(0x83000010,0);f.Put<uint16_t>(0x83000018,0x800);
  auto s=f.Poll();assert(!s.ammo&&!s.weapon&&s.placed_items);++groups;}
 {Fixture f;f.Items();f.Put<uint64_t>(0x83000010,0x220000000F);assert(!f.Poll().ammo);
  f.Put<uint64_t>(0x83000010,UINT64_MAX);assert(!f.Poll().ammo);
  f.Put<int32_t>(0x83000030,385);assert(!f.Poll().placed_items);++groups;}
 {Fixture f;f.Items();f.Put<uint8_t>(0x2200000013,1);auto s=f.Poll();assert(!s.ammo&&s.placed_items->empty());
  f.Put<uint8_t>(0x8300001B,1);assert(!f.Poll().placed_items);++groups;}
 {Fixture f;f.Items();f.Put<uint8_t>(0x2200000016,0x20);assert(!f.Poll().ammo); // 36 exceeds tested weapon 0x27 maximum
  f.Put<uint8_t>(0x2200000016,0);f.Put<uint8_t>(0x2200000017,0);assert(f.Poll().ammo==0);
  f.mem.erase(0x2200000000);assert(!f.Poll().placed_items);++groups;}
 {Fixture f;f.Items();f.Put<uint8_t>(0x2200000018,0xff);assert(!f.Poll().placed_items);
  f.Put<uint16_t>(0x82005020,0x120);assert(f.Poll().room==0x120&&f.Poll().stage==1);
  f.Put<uint16_t>(0x82005020,0x100);assert(f.Poll().room==0x100&&f.Poll().stage==1&&f.Poll().state==State::Unknown);
  // re4.stage is room's own high byte (analysis/20261103-open-items.md "2. re4.stage"): it
  // tracks room exactly, including the same "cached outside gameplay" reset.
  f.Put<uint64_t>(0x80B00008,0);assert(!f.Poll().room&&!f.Poll().stage&&!f.Poll().ammo);++groups;}
 {Fixture f;f.Items();auto s=f.Poll();const auto small_reads=f.reads;
  for(int n=3;n<384;++n)f.Put(0x2200000000+n*14,std::array<uint8_t,14>{});
  f.Put<int32_t>(0x83000030,384);f.reads=0;s=f.Poll();
  assert(s.ammo==35&&s.placed_items&&s.placed_items->size()==1);
  assert(f.reads==small_reads); // Inventory size must not multiply host crossings.
  f.mem.erase(0x2200000000+383*14+13);s=f.Poll();
  assert(!s.placed_items&&!s.ammo);++groups;}
 {Fixture f;f.Items();const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);assert(m);f.Sample(api,m);
  assert(f.ints["re4.ammo.available"]==1&&f.ints["re4.ammo"]==35);
  assert(f.ints["re4.ammo.display_available"]==0);
  assert(f.ints["re4.inventory.positions.available"]==1);
  assert(f.texts["re4.inventory.positions"]=="1:39:1:2:1:0;");
  f.mem.clear();f.Sample(api,m);
  assert(f.ints["re4.ammo.available"]==0&&f.ints["re4.ammo"]==0);
  assert(f.ints["re4.inventory.positions.count"]==0&&f.texts["re4.inventory.positions"].empty());
  api->destroy(m);++groups;}
 {Fixture f;re4::Snapshot s;s.owned_items=std::vector<re4::OwnedItem>{{0,0xb9,2},{1,0xa2,1},{2,0x3b,1},{3,0xa9,1},{4,0x27,1}};
  assert(re4::KeyItems(s,1).size()==1&&re4::KeyItems(s,2).size()==2);
  std::array<size_t,2> pages{8,9};re4::PublishKeys(f.host,s,pages,0);
  assert(pages[0]==0&&pages[1]==0&&f.texts["re4.keys.name"].empty());
  assert(f.ints["re4.keys.2.0.available"]==1&&f.texts["re4.keys.2.0.count"]=="2");
  assert(f.texts["re4.keys.2.1.count"].empty());
  s.owned_items.reset();re4::PublishKeys(f.host,s,pages,0);
  assert(!f.ints["re4.keys.2.0.available"]&&f.texts["re4.keys.2.0.art"].empty()&&f.texts["re4.keys.name"].empty());++groups;}
 {struct Mail {
  std::array<uint32_t,18> words{};bool fail=false;
  EdenDsmodHostExtensions Ext(){EdenDsmodHostExtensions e{};e.version=EDEN_DSMOD_EXT_VERSION;e.abi_hash=EDEN_DSMOD_EXT_HASH;e.struct_size=sizeof e;e.userdata=this;e.mailbox_address=0x90000000;e.mailbox_size=72;
   e.load_u32=[](void* p,uint32_t o,uint32_t* out)->EdenDsmodBool {if(o%4||o>=72)return false;*out=static_cast<Mail*>(p)->words[o/4];return true;};
   e.store_u32=[](void* p,uint32_t o,uint32_t v)->EdenDsmodBool {auto& m=*static_cast<Mail*>(p);if(m.fail||o%4||o>=72)return false;m.words[o/4]=v;return true;};return e;}
  void Ack(uint32_t mask){++words[0];words[6]=words[2];words[7]=mask;words[8]=0;}
 } m;m.words[1]=1;auto e=m.Ext();HudLease lease;lease.Configure(&e);
 auto h=lease.Poll(true,true,true);assert(!h.available&&!h.display&&m.words[5]==0);
 m.Ack(0);h=lease.Poll(true,true,true);assert(h.available&&!h.display&&m.words[5]==1);
 m.Ack(1);h=lease.Poll(true,true,true);assert(h.available&&h.hidden&&h.display);
 const auto token=m.words[2];for(int n=0;n<20;++n){h=lease.Poll(true,true,true);assert(m.words[2]==token&&h.display);}
 m.Ack(1);h=lease.Poll(true,true,false);assert(h.display&&m.words[5]==0);
 m.Ack(0);h=lease.Poll(false,true,true);assert(!h.display&&m.words[5]==0);
 m.Ack(0);lease.Poll(true,false,true);assert(m.words[5]==0);
 m.words[1]=2;h=lease.Poll(true,true,true);assert(!h.available&&!h.display&&m.words[3]==2);
 m.Ack(0);lease.Poll(true,true,true);m.Ack(1);h=lease.Poll(true,true,true);assert(h.display);
 m.words[8]=3;h=lease.Poll(true,true,true);assert(!h.display);
 for(int n=0;n<121;++n)h=lease.Poll(true,true,true);
 assert(!h.available&&!h.display&&m.words[5]==0);
 m.Ack(1);m.fail=true;h=lease.Poll(true,true,true);assert(!h.available&&!h.display);
 m.fail=false;lease.Stop();assert(m.words[5]==0&&!(m.words[2]&1));++groups;}
 {assert(re4_hud_lease_live(100,100));assert(re4_hud_lease_live(100,190));
 assert(!re4_hud_lease_live(100,191));assert(!re4_hud_lease_live(100,99));
 assert(re4_hud_lease_live(0xfffffff0u,0x10));assert(!re4_hud_lease_live(0x10,0xfffffff0u));++groups;}
 {assert(!Resolver::Range(0,8));assert(!Resolver::Range(UINT64_MAX,8));assert(!Resolver::Range(0x10000,0));++groups;}
 {const char* expected[]={"unavailable","standby","standby","standby","standby","standby","dimmed",
 "field","field","field","field","field","field","field","dimmed","hud_only","case","dimmed",
 "case","key_items","field","field","standby","field","case","field","mercenaries",
 "mercenaries","mercenaries","standby","unsupported","dimmed","field","dimmed"};
 static_assert(std::size(expected)==static_cast<int>(State::ShootingRange)+1);
 static_assert(static_cast<int>(State::ShootingRange)==33);
 static_assert(static_cast<int>(State::GameOver)==22); // retired state 21 stays unassigned
 assert(Select(State::ShootingRange).dim);
 for(int n=0;n<=static_cast<int>(State::ShootingRange);++n)assert(std::string(Select(static_cast<State>(n)).page)==expected[n]);
 assert(std::string(Select(State::Merchant).page)=="case");assert(Select(State::Qte).dim);
 assert(std::string(Select(State::Loading).page)=="standby");assert(std::string(Select(State::Vehicle).page)=="hud_only");
 assert(std::string(Select(State::Unknown).page)=="unavailable");++groups;}
 // ---- C1 fix: screen-state detection beyond Pause/GameOver (audit 20261102) ----
 {Fixture f;
 // Title: no live pG at all -- the global slot's own pointer chain broken/absent.
 f.Put<uint64_t>(0x80B00000,0);assert(f.Poll().state==State::Title);
 f.mem.erase(0x80B00000);assert(f.Poll().state==State::Title);++groups;}
 {Fixture f;
 // Switch GameTask's byte, derived from its dispatch and writes, not the GC+0x84 guess.
 for(uint8_t routine:{1,2,4}) {
  f.Put<uint8_t>(0x82000028,routine);assert(f.Poll().state==State::Loading);
  // A menu request can linger while a room is being replaced: Loading wins.
  f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::Loading);
  f.Put<uint32_t>(0x82005098,0);
 }
 for(uint8_t routine:{0,3,5,6,255}) {
  f.Put<uint8_t>(0x82000028,routine);assert(f.Poll().state!=State::Loading);
 }
 f.mem.erase(0x82000028);assert(f.Poll().state!=State::Loading);
 f.Put<uint8_t>(0x82000028,3);
 f.Put<uint32_t>(0x8200509C,0x2000);assert(f.Poll().state==State::Unknown);
 for(uint32_t ui:{256u,65792u}) {
  f.Put<uint32_t>(f.host.main_base+0x1242E10,ui);assert(f.Poll().state==State::Unknown);
 }
 ++groups;}
 {Fixture f;
 // Cutscene: STA_EVENT_SYSYTEM (+ its unnamed companion bit), STA_SUSPEND, and the
 // scripted camera-freeze control word, each independently sufficient.
 f.Put<uint32_t>(0x82005098,0x00090000);assert(f.Poll().state==State::Cutscene);
 f.Put<uint32_t>(0x82005098,0);f.Put<uint32_t>(0x82005094,0x10000000);
 assert(f.Poll().state==State::Cutscene);
 f.Put<uint32_t>(0x82005094,0);f.Put<uint32_t>(0x820001A8,0x80000000);
 assert(f.Poll().state==State::Cutscene);
 f.Put<uint32_t>(0x820001A8,0);assert(f.Poll().state==State::Unknown);++groups;}
 {Fixture f;
 // Merchant requires its native open type as well as both status bits.
 f.Put<uint64_t>(f.host.main_base+0xB26C98,0x84000000);
 f.Put<uint32_t>(0x8400002C,0x10);
 f.Put<uint32_t>(0x82005098,0x04040000);assert(f.Poll().state==State::Merchant);
 f.Put<uint32_t>(0x82005098,0x00040000);assert(f.Poll().state==State::Unknown);
 // Priority: Loading beats a simultaneously-set stale shop bit.
 f.Put<uint8_t>(0x82000028,4);assert(f.Poll().state==State::Loading);
 f.Put<uint8_t>(0x82000028,3);
 // The real island merchant sets both STA_INTO_SHOP and STA_SSCRN_REQUEST.
 f.Put<uint32_t>(0x82005098,0x00040000|0x04000000);assert(f.Poll().state==State::Merchant);
 // Ordinary Case after a merchant visit retains the same status bits.
 f.Put<uint32_t>(0x8400002C,1);assert(f.Poll().state==State::Pause);
 // An unreadable open type cannot claim Merchant.
 f.mem.erase(0x8400002C);assert(f.Poll().state==State::Pause);
 f.Put<uint32_t>(0x8400002C,0x10);
 // Closing the shop leaves the generic menu detector available.
 f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::Pause);
 f.Put<uint32_t>(0x82005098,0x04040000);
 // GameOver (existing, unchanged) still overrides every new state, including Pause.
 f.Items();f.Put(0x82005028,std::array<int16_t,2>{0,2400});
 assert(f.Poll().state==State::GameOver);++groups;}
 {Fixture f;
 // Title: STA_TITLE, decomp ordinal 80 -- LIVE-VERIFIED this round at the genuine title
 // screen ("PRESS A", no save loaded), exact bit match (pG+0x5098 read back exactly
 // 0x8000, no other bits set). See analysis/20261102-fix-c.md.
 f.Put<uint32_t>(0x82005098,0x00008000);assert(f.Poll().state==State::Title);
 f.Put<uint32_t>(0x82005098,0);
 // Title fallback: no live pPL object yet (MainMenu/SaveSelect/Options -- no dedicated
 // live bit found this round; Select() already presents these identically to Title).
 // Distinct from the earlier "no live pG" test: here only the player chain is broken.
 f.Put<uint64_t>(f.host.main_base+0xB25D88,0);assert(f.Poll().state==State::Title);
 // Cleanup-pass bugfix (code-audit finding, not live-tested): the generic sub-screen-
 // owns-input bit (STA_SSCRN_REQUEST) must not reclassify the Title-fallback screens
 // (MainMenu/SaveSelect/Options) into the dimmed Pause page. Every other member of the
 // same native sub-screen family found so far (the shop, SW's chapter/Directives/Map
 // cards) has needed exactly this exemption; Title had not been given it yet.
 f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::Title);
 f.Put<uint32_t>(0x82005098,0);++groups;}
 // ---- Mercenaries round state (re4.state=MercRound), LIVE-VERIFIED 2026-11-03
 // (analysis/20261103-merc.md): zero new readers, reuses the already-shipped room field --
 // the campaign only ever uses stage 1-3, so stage==4 (unused by the base game) is a safe
 // positive signal, live-confirmed for stage 4 (village). Restricted to exactly stage==4
 // (bug fixed 2026-11-04, analysis/20261104-separate-ways-2.md): a prior `>=4` catch-all
 // also matched stage5's Separate Ways rooms -- see the dedicated SeparateWays group below. ----
 {Fixture f;
 f.Put<uint16_t>(0x82005020,0x400);
 assert(f.Poll().room==0x400&&f.Poll().stage==4&&f.Poll().state==State::MercRound);
 // Campaign stages (village/castle/island, 1-3) must never read as a Mercenaries round.
 f.Put<uint16_t>(0x82005020,0x300);assert(f.Poll().state==State::Unknown);
 // Stage5 (Separate Ways) must never read as a Mercenaries round -- this was the exact bug:
 // the old `>=4` bound silently also matched 0x500-0x51F. Unverified stages 6/7 (if the other
 // native Mercenaries maps are even addressed this way) now correctly fall through to
 // Unknown rather than being guessed as either mode -- narrower is safer than the prior
 // blanket bound once stage5 was found to collide with a real, different game mode.
 f.Put<uint16_t>(0x82005020,0x500);assert(f.Poll().state!=State::MercRound);
 f.Put<uint16_t>(0x82005020,0x700);assert(f.Poll().state==State::Unknown);
 // Priority: Loading/Cutscene/Merchant/Title all still beat MercRound (same cascade order
 // as every other branch above), and Pause/GameOver (below) still have the final say.
 f.Put<uint16_t>(0x82005020,0x400);
 f.Put<uint64_t>(f.host.main_base+0xB26C98,0x84000000);
 f.Put<uint32_t>(0x8400002C,0x10);
 f.Put<uint8_t>(0x82000028,4);assert(f.Poll().state==State::Loading);
 f.Put<uint8_t>(0x82000028,3);
 f.Put<uint32_t>(0x82005098,0x00090000);assert(f.Poll().state==State::Cutscene);
 f.Put<uint32_t>(0x82005098,0x04040000);assert(f.Poll().state==State::Merchant);
 f.Put<uint32_t>(0x82005098,0x00008000);assert(f.Poll().state==State::Title);
 f.Put<uint32_t>(0x82005098,0);assert(f.Poll().state==State::MercRound);
 f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::Pause);
 f.Put<uint32_t>(0x82005098,0);
 f.Items();f.Put(0x82005028,std::array<int16_t,2>{0,2400});
 assert(f.Poll().state==State::GameOver);++groups;}
 // ---- Separate Ways (Ada) state (re4.state=SeparateWays/SeparateWaysCase), LIVE-VERIFIED
 // 2026-11-04 (analysis/20261104-separate-ways-2.md: re4.room=1280/0x500, re4.stage=5,
 // reproduced on 4 independent live samples). Fixes the room>=4 catch-all misclassifying SW
 // gameplay as MercRound (coordinator bug report, same date). SeparateWaysCase mirrors the
 // Merchant/Case pattern: SW's Directives/Map/case sub-screens share the identical
 // STA_SSCRN_REQUEST bit this reader already reads as `paused`, so it is the same
 // best-effort label while that bit is set -- not an independently addressable raw flag. ----
 {Fixture f;
 f.Put<uint16_t>(0x82005020,0x500);
 assert(f.Poll().room==0x500&&f.Poll().stage==5&&f.Poll().state==State::SeparateWays);
 // The menu/case flag: same bit as Pause elsewhere in this reader (`s.paused`), scoped to
 // stage5 -- while set, SW's sub-screen family reads as SeparateWaysCase here, same as the
 // existing Merchant/Case branches do for their own bit.
 f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::SeparateWaysCase);
 f.Put<uint32_t>(0x82005098,0);assert(f.Poll().state==State::SeparateWays);
 // Priority unchanged: Loading/Cutscene/Merchant/Title still beat SeparateWays (same cascade
 // order as MercRound above).
 f.Put<uint8_t>(0x82000028,4);assert(f.Poll().state==State::Loading);
 f.Put<uint8_t>(0x82000028,3);
 f.Put<uint32_t>(0x82005098,0x00090000);assert(f.Poll().state==State::Cutscene);
 f.Put<uint64_t>(f.host.main_base+0xB26C98,0x84000000);
 f.Put<uint32_t>(0x8400002C,0x10);
 f.Put<uint32_t>(0x82005098,0x04040000);assert(f.Poll().state==State::Merchant);
 f.Put<uint32_t>(0x82005098,0x00008000);assert(f.Poll().state==State::Title);
 f.Put<uint32_t>(0x82005098,0);assert(f.Poll().state==State::SeparateWays);
 // Select() presentation: plain SW gameplay falls to the generic {"field",false} default
 // (same as Exploration/Combat -- never explicitly listed either); SeparateWaysCase presents
 // like Merchant/Case.
 assert(Select(State::SeparateWays).page==std::string("field"));
 assert(Select(State::SeparateWaysCase).page==std::string("case"));++groups;}
 // ---- Merchant's shooting range (room 0x22C): Room_flg[0] at pG+0x1AC, bit31 = range
 // session (inventory swapped for the range weapon), bit29 = round running
 // (analysis/20261114-gallery-sw.md A1). Standing in the room outside a session stays the
 // ordinary Field clone with no map. ----
 if(have_geometry) {Fixture f;f.Items();f.Put<uint16_t>(0x82005020,0x22C);f.Put<uint32_t>(0x820001AC,0);
 assert(f.Poll().state==State::Unknown);assert(!*MapArea(0x22C));
 f.Put<uint32_t>(0x820001AC,0x80000000);assert(f.Poll().state==State::ShootingRange);
 f.Put<uint32_t>(0x820001AC,0x20000000);assert(f.Poll().state==State::ShootingRange);
 f.Put<uint32_t>(0x820001AC,0x10000000);assert(f.Poll().state==State::Unknown);
 // An unreadable flag word in the range fails safe to the dimmed state.
 for(int i=0;i<4;++i)f.mem.erase(0x820001AC+i);assert(f.Poll().state==State::ShootingRange);
 f.Put<uint32_t>(0x820001AC,0xA0000000);assert(f.Poll().state==State::ShootingRange);
 // Priority: room loads, cutscenes, the result board (Stop_flg SPF_KEY), the weapon-choice
 // sub-screen and death all keep their own states.
 f.Put<uint8_t>(0x82000028,4);assert(f.Poll().state==State::Loading);f.Put<uint8_t>(0x82000028,3);
 f.Put<uint32_t>(0x82005098,0x00090000);assert(f.Poll().state==State::Cutscene);f.Put<uint32_t>(0x82005098,0);
 f.Put<uint32_t>(0x820001A8,0x80000000);assert(f.Poll().state==State::Cutscene);f.Put<uint32_t>(0x820001A8,0);
 f.Put<uint32_t>(0x82005098,0x04000000);assert(f.Poll().state==State::Pause);f.Put<uint32_t>(0x82005098,0);
 assert(f.Poll().state==State::ShootingRange);
 f.Put(0x82005028,std::array<int16_t,2>{0,2400});assert(f.Poll().state==State::GameOver);
 f.Put(0x82005028,std::array<int16_t,2>{2400,2400});
 // Room gate: the same bits in any other room (r11C reuses bit29) are never the range.
 for(uint16_t room:{0x204,0x11C,0x22B,0x32C}){f.Put<uint16_t>(0x82005020,room);assert(f.Poll().state==State::Unknown);}
 // Only room 0x22C pays the extra flag read.
 f.Put<uint16_t>(0x82005020,0x100);f.reads=0;f.Poll();const auto field_reads=f.reads;
 f.Put<uint16_t>(0x82005020,0x22C);f.Put<uint32_t>(0x820001AC,0);f.reads=0;f.Poll();assert(f.reads==field_reads+1);
 // Published page and tap-to-equip guard: a range session never allows an equip.
 f.Put<uint32_t>(0x82005090,0x02000000);f.Put<uint32_t>(0x8200509C,0);f.Put<uint8_t>(0x8200503C,0);
 f.Put<uint32_t>(0x82005094,0);f.Put<uint32_t>(0x820001A8,0);
 f.Put<uint64_t>(f.host.main_base+0xB26C98,0x84000000);f.Put<uint32_t>(f.host.main_base+0x1242E10,0);
 RoomStability stable;EquipGuard guard;
 auto settle=[&]{for(int n=0;n<40;++n)guard=PollEquipGuard(f.host,Build.data(),f.Poll(),stable);};
 settle();assert(f.Poll().state==State::Unknown&&guard.safe);
 assert(guard.bits&RE4_EQUIP_GUARD_NOT_GAMEOVER);
 f.Put<uint32_t>(0x820001AC,0x80000000);settle();
 assert(!guard.safe&&!(guard.bits&RE4_EQUIP_GUARD_NOT_GAMEOVER));
 f.Put<uint32_t>(0x820001AC,0x20000000);settle();assert(!guard.safe);
 f.Put<uint32_t>(0x820001AC,0);settle();assert(guard.safe);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);assert(m);
 f.Put<uint32_t>(0x820001AC,0xA0000000);f.Sample(api,m);
 assert(f.ints["re4.state"]==33&&f.texts["re4.page"]=="dimmed");
 assert(f.ints["re4.map.available"]==0&&f.ints["re4.equip.safe"]==0);
 assert(f.Poll().reserve_ammo.has_value()); // the raw inventory sum exists but is hidden
 assert(f.ints["re4.ammo.reserve.available"]==0);
 const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
 assert(!ext->on_action(m,"equip_unavailable",0));
 f.Put<uint32_t>(0x820001AC,0);f.Sample(api,m);
 assert(f.ints["re4.state"]==0&&f.texts["re4.page"]=="unavailable");
 assert(f.ints["re4.ammo.reserve.available"]==1);
 api->destroy(m);++groups;}
 // ---- Mercenaries score/combo/stage-timer/combo-window -- still not found live despite
 // 2026-11-04's dedicated kill session (analysis/20261104-merc-kills.md: Infinite HP cheat
 // active, 8+ live rounds, gunfire+melee, never a confirmed kill) -- always published
 // unavailable, never a fabricated value, even while a Mercenaries round is correctly
 // detected and the page is live. .available is now explicitly gated to a live round
 // (re4.state==27) rather than unconditionally false, per this round's task. ----
 {Fixture f;f.Put<uint16_t>(0x82005020,0x400);
 f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
  auto it=values.find(key);return it==values.end()?fallback:it->second;};
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);assert(m);f.Sample(api,m);
 assert(f.ints["re4.state"]==static_cast<int>(State::MercRound));
 assert(f.texts["re4.page"]=="mercenaries");
 // re4.stage published alongside, module level (analysis/20261103-open-items.md "2."):
 // room's own high byte, zero new readers, usable by the tools-side map-key lookup for the
 // 111/112 non-colliding area entries -- NOT a fix for the room-id 0x200 collision itself.
 assert(f.ints["re4.stage.available"]==1&&f.ints["re4.stage"]==4);
 // The four genuinely-never-found fields stay unavailable even inside a live round.
 for(const char* k:{"re4.merc.time","re4.merc.combo","re4.merc.combo_window","re4.merc.score"})
  assert(f.ints[std::string(k)+".available"]==0);
 assert(f.texts["re4.merc.time"].empty());
 assert(f.ints["re4.merc.combo"]==0&&f.ints["re4.merc.combo_window"]==0);
 assert(f.ints["re4.merc.score"]==0);
 // re4.merc.best is different: it IS available in a live round (reads the persisted flag,
 // see the dedicated persistence block below) -- no persisted value yet here (no Items(),
 // no @flag set), so it reads back the flag's own default of 0, not "unavailable".
 assert(f.ints["re4.merc.best.available"]==1&&f.ints["re4.merc.best"]==0);
 assert(f.ints["re4.merc.best_candidate"]==0);
 // Outside a Mercenaries round (campaign village, stage1), all five -- including best, which
 // the shipped code before this round published unconditionally -- must read unavailable.
 // This is the actual behaviour change this round's task asked for.
 f.Put<uint16_t>(0x82005020,0x100);f.ints["@flag:merc.best.1.0"]=500;f.Sample(api,m);
 assert(f.ints["re4.state"]!=static_cast<int>(State::MercRound));
 assert(f.ints["re4.merc.best.available"]==0&&f.ints["re4.merc.best"]==0);
 api->destroy(m);++groups;}
 // ---- re4.merc.best: companion-side persistence per (stage, character), same split as
 // re4.slots.assignment (PER-SAVE SLOT ISOLATION) -- this module only reads the flag; a
 // manifest enforce action (out of native/ scope) would write it on round end. ----
 {Fixture f;f.Put<uint16_t>(0x82005020,0x400);f.Items(); // character=0 (Leon) via Items()
 f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
  auto it=values.find(key);return it==values.end()?fallback:it->second;};
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);assert(m);
 // No persisted flag yet (fresh @flag default -1 would be wrong here; this flag's own
 // default is 0, unlike slots.assignment's sentinel -1) -- reads back exactly 0.
 f.Sample(api,m);
 assert(f.ints["re4.merc.best.available"]==1&&f.ints["re4.merc.best"]==0);
 assert(f.ints["re4.merc.best_candidate"]==0);
 // Keyed by BOTH stage and character: stage4+character0 (Leon, village) must not see a
 // value persisted under a different stage or a different character.
 f.ints["@flag:merc.best.5.0"]=12345;f.Sample(api,m);
 assert(f.ints["re4.merc.best"]==0); // wrong stage (5, Separate Ways) -- not this bucket
 f.ints["@flag:merc.best.4.1"]=777;f.Sample(api,m);
 assert(f.ints["re4.merc.best"]==0); // wrong character (1, not Leon) -- not this bucket
 f.ints["@flag:merc.best.4.0"]=4200;f.Sample(api,m);
 assert(f.ints["re4.merc.best"]==4200&&f.ints["re4.merc.best_candidate"]==4200);
 // A different character in the SAME stage gets its own independent best.
 f.mem[0x8300001B]=1;f.ints["@flag:merc.best.4.1"]=999;f.Sample(api,m);
 assert(f.ints["re4.merc.best"]==999);
 api->destroy(m);++groups;}
 {Fixture f;const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);assert(api);
 assert(!eden_dsmod_get_module(2,EDEN_DSMOD_MODULE_ABI_HASH));
 auto* m=api->create(&f.host,nullptr);assert(m);f.Sample(api,m);
 assert(f.ints["re4.money.available"]==1&&f.ints["re4.money"]==1751510);
 assert(f.ints["re4.hp.available"]==0&&f.ints["re4.room.available"]==0&&f.ints["re4.stage.available"]==0);
 f.mem.clear();f.Sample(api,m);assert(f.ints["re4.money.available"]==0&&f.ints["re4.money"]==0);
 assert(f.texts["re4.page"]=="unavailable");api->destroy(m);
 f.host.build_id[31]=1;assert(!api->create(&f.host,nullptr));
 f.host.build_id[31]=0;f.host.title_id=0;assert(!api->create(&f.host,nullptr));
 f.host.title_id=Title;f.host.read_memory=nullptr;assert(!api->create(&f.host,nullptr));
 assert(!api->create(nullptr,nullptr));++groups;}
 {assert(std::string(MapArea(0x100))=="village-100");
 assert(std::string(MapArea(0x200,false))=="village-200");
 for(auto room:{0,0x110,0x120,0x51f,0xffff})assert(!*MapArea(room)); // 0x331 is now island-331
 assert(std::abs(MapHeading({-1,0}))<0.001);
 assert(std::abs(MapHeading({0,1})-90)<0.001);
 assert(std::abs(MapHeading({1,0})-180)<0.001);
 assert(std::abs(MapHeading({0,-1})+90)<0.001);++groups;}
 if(have_geometry) {Fixture f;f.MapRoom(0x100);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.map.available"]==1&&f.texts["re4.map.area"]=="village-100");
 f.MapRoom(0x120);f.Sample(api,m); // genuinely unmapped room
 assert(f.ints["re4.map.available"]==0&&f.texts["re4.map.area"].empty());
 f.MapRoom(0x101);f.Sample(api,m);
 assert(f.ints["re4.map.available"]==1&&f.texts["re4.map.area"]=="village-101");
 f.mem.erase(0x2100000020);f.Sample(api,m);
 assert(f.ints["re4.map.available"]==0&&f.texts["re4.map.area"].empty());
 api->destroy(m);++groups;}
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);
 f.Put(0x82005028,std::array<int16_t,2>{0,2400});
 assert(f.Poll().state==State::GameOver);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.map.available"]==0&&f.texts["re4.page"]=="standby");
 f.Put(0x82005028,std::array<int16_t,2>{2400,2400});f.Sample(api,m);
 assert(f.ints["re4.map.available"]==1&&f.ints["re4.state"]==0);
 api->destroy(m);++groups;}
 if(have_geometry) {Fixture f;f.MapRoom(0x331);
 f.Put<uint64_t>(f.host.main_base+0xB25E08,0x80B00010);
 f.Put<uint64_t>(0x80B00010,0);assert(f.Poll().ashley_escort==false);
 f.Put<uint64_t>(0x80B00010,0x2300000000);f.Put<uint8_t>(0x2300000118,3);
 assert(f.Poll().ashley_escort==true);
 f.Put<uint8_t>(0x2300000118,2);assert(f.Poll().ashley_escort==false);
 f.Put<uint8_t>(0x2300000118,3);f.MapRoom(0x500);
 assert(f.Poll().ashley_escort==false);
 f.MapRoom(0x331);f.mem.erase(0x2300000118);
 assert(!f.Poll().ashley_escort.has_value());
 f.Put<uint64_t>(0x80B00010,0x2300000001);assert(!f.Poll().ashley_escort.has_value());
 f.Put<uint64_t>(0x80B00010,0x2300000000);f.Put<uint8_t>(0x2300000118,3);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.ashley.presence.available"]==1&&f.ints["re4.ashley.escort"]==1);
 f.mem.erase(0x2300000118);f.Sample(api,m);
 assert(f.ints["re4.ashley.presence.available"]==0&&f.ints["re4.ashley.escort"]==0);
 api->destroy(m);++groups;}
 {assert(AshleyCommandStatus(0,{0,0},0,0)==1);
 assert(AshleyCommandStatus(2,{0,0},0,0)==2);
 assert(AshleyCommandStatus(0,{0,0x10},0,0)==2);
 assert(AshleyCommandStatus(2,{0,0},0x80,0)==0x22);
 assert(AshleyCommandStatus(0,{0,0},0,1)==9);
 assert(AshleyCommandStatus(8,{0,0},0,0)==0x21);
 assert(AshleyCommandStatus(0,{5,0},0,0)==0x21);
 assert(AshleyCommandStatus(0,{0,0x13},0,0)==0x11);
 assert(AshleyCommandStatus(0,{0,0x14},0,0)==0x11);
 assert(AshleyCommandStatus(0,{0,0x12},0,0)==0x21);++groups;}
 if(have_geometry) {Fixture f;f.Items();f.Put(0x82005028,std::array<int16_t,2>{1200,2400});
 f.MapRoom(0x331);f.Put<uint64_t>(f.host.main_base+0xB25E08,0x80B00010);
 f.Put<uint64_t>(0x80B00010,0x2300000000);f.Put<uint8_t>(0x2300000118,3);
 f.Put(0x2300000114,std::array<uint8_t,2>{0,0});f.Put<uint16_t>(0x23000004F0,0x46);
 f.Put<uint8_t>(0x82005096,0);f.Put<uint8_t>(0x8200509A,0);
 f.host.get_i64=[](void*,const char*,int64_t)->int64_t{return 0;};
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.ashley_command.available"]==1&&f.ints["re4.ashley.waiting"]==1);
 f.host.get_i64=nullptr;f.Sample(api,m);
 assert(f.ints["re4.ashley_command.available"]==1&&f.ints["re4.ashley.command.display_available"]==0);
 assert(f.ints["re4.ashley.waiting"]==0);
 f.host.get_i64=[](void*,const char*,int64_t)->int64_t{return 0;};
 f.Put<uint16_t>(0x23000004F0,0x44);f.Sample(api,m);
 assert(f.ints["re4.ashley_command.available"]==1&&f.ints["re4.ashley.following"]==1);
 f.Put<uint32_t>(0x82005098,0x04000000);f.Sample(api,m);
 assert(f.ints["re4.ashley_command.available"]==0);
 f.Put<uint32_t>(0x82005098,0);f.Put<uint8_t>(0x82005096,1);f.Sample(api,m);
 assert(f.ints["re4.ashley_command.available"]==0);
 f.mem.erase(0x23000004F0);f.Sample(api,m);
 assert(f.ints["re4.ashley.command.state"]==0&&f.ints["re4.ashley.following"]==0);
 api->destroy(m);++groups;}
 // ---- Ashley HP (pG+502C/+502E), gated strictly on escort==true this same sample: the
 // address is known to hold a stale nonzero value while Ashley is absent (LIVE-READERS.md). ----
 if(have_geometry) {Fixture f;f.MapRoom(0x331);
 f.Put<uint64_t>(f.host.main_base+0xB25E08,0x80B00010);
 f.Put<uint64_t>(0x80B00010,0x2300000000);f.Put<uint8_t>(0x2300000118,3);
 f.Put(0x8200502C,std::array<int16_t,2>{1200,1200});
 assert((f.Poll().ashley_health==std::array<int16_t,2>{1200,1200}));
 // Not escorting (mode!=3): the stale value at this same address must never be published.
 f.Put<uint8_t>(0x2300000118,2);assert(!f.Poll().ashley_health);
 f.Put<uint8_t>(0x2300000118,3);
 // Implausible pairs are rejected the same way as Leon's current/max bounds check.
 for(auto pair:{std::array<int16_t,2>{-1,1200},{1201,1200},{100,0}}){
  f.Put(0x8200502C,pair);assert(!f.Poll().ashley_health);}
 f.Put(0x8200502C,std::array<int16_t,2>{1200,1200});
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.ashley.hp.available"]==1&&f.ints["re4.ashley.hp"]==1200&&f.ints["re4.ashley.hp.max"]==1200);
 f.Put<uint8_t>(0x2300000118,2);f.Sample(api,m);
 assert(f.ints["re4.ashley.hp.available"]==0&&f.ints["re4.ashley.hp"]==0);
 api->destroy(m);++groups;}
 {Fixture f;f.Items();assert(!f.Poll().case_capacity);
 constexpr std::array<std::array<uint8_t,2>,4> sizes{{{10,6},{11,7},{12,8},{15,8}}};
 for(int n=0;n<4;++n) {
  f.Put(0x220000001C,std::array<uint8_t,14>{static_cast<uint8_t>(0x7C+n),0,1,0,1,0,0,0,0,0,0,0,0,0});
  assert(f.Poll().case_capacity==sizes[n]);
 }
 f.Put(0x2200000000,std::array<uint8_t,14>{0x7C,0,1,0,1,0,0,0,0,0,0,0,0,0});
 assert(f.Poll().case_capacity==sizes[3]);
 // An unsupported placement before the case record must not stop ownership scanning.
 f.Put(0x2200000000,std::array<uint8_t,14>{1,0,1,0,1,0,0,0,0,0,0,0,2,1});
 assert(!f.Poll().placed_items && f.Poll().case_capacity==sizes[3]);
 f.Put(0x2200000000,std::array<uint8_t,14>{0x7C,0,1,0,1,0,0,0,0,0,0,0,0,0});
 f.Put<uint8_t>(0x8200503C,1);assert(!f.Poll().case_capacity);
 f.Put<uint8_t>(0x8200503C,0);
 f.Put<uint8_t>(0x2200000021,1);assert(f.Poll().case_capacity==sizes[0]);
 f.Put<uint8_t>(0x2200000004,0);assert(!f.Poll().case_capacity);
 f.mem.erase(0x2200000029);assert(!f.Poll().case_capacity);++groups;}
 if(have_geometry) {Fixture f;f.Items();f.Legend();f.Medallions();f.Flag(29);f.MapRoom(0x103);
  f.Put<uint32_t>(0x82005344,0);assert(f.Poll().medallions_unbroken==0x7FFF);
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
   auto it=values.find(key);return it==values.end()?fallback:it->second;};
  auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
  assert(f.ints["re4.medallion.gate"]==0&&f.ints["re4.medallion.0.show"]==0);
  f.ints["@flag:opt.map_spoilers"]=1;f.Sample(api,m);
  assert(f.ints["re4.treasure.show"]==1&&f.ints["re4.medallion.0.show"]==1);
  f.ints["@flag:opt.map_spoilers"]=0;
  f.Put(0x220000001C,std::array<uint8_t,14>{0xB0,0,1,0,1});f.Sample(api,m);
  assert(f.ints["re4.medallion.gate"]==1&&f.ints["re4.medallion.0.show"]==1);
  assert(f.ints["re4.treasure.show"]==0); // Note does not grant the Treasure Map.
  f.Put<uint16_t>(0x220000001E,0);f.Put<uint32_t>(0x82005344,0x00400000);
  f.Sample(api,m);assert(f.ints["re4.medallion.14.show"]==1);
  f.Put<uint32_t>(0x82005344,0x00800000);f.Sample(api,m);
  assert(f.ints["re4.medallion.gate"]==0); // Adjacent castle flag is not merchant contact.
  constexpr int targets[]={15,16,17,18,20,19,21,4,5,6,7,9,8,10,11};
  for(int n=0;n<15;++n)for(int night=0;night<2;++night){
   const int index=n<7?(night?18:3):(night?20:8);
   const uint64_t address=0x85000000+index*0xD8+0x28+2*targets[n];
   f.Put<uint16_t>(address,1);
   assert(f.Poll().medallions_unbroken==(0x7FFF^(1u<<n)));
   for(int spoilers=0;spoilers<2;++spoilers){
    f.ints["@flag:opt.map_spoilers"]=spoilers;f.Put<uint32_t>(0x82005344,0x00400000);
    f.Sample(api,m);assert(f.ints["re4.medallion."+std::to_string(n)+".shot"]==1);
    assert(f.ints["re4.medallion."+std::to_string(n)+".show"]==0);
   }
   f.Put<uint16_t>(address,2);assert(f.Poll().medallions_unbroken==0x7FFF); // Only bit 0.
   f.Put<uint16_t>(address,0);
  }
  f.Put<uint16_t>(0x85000000+3*0xD8,0x104);assert(!f.Poll().medallions_unbroken);
  f.Put<uint16_t>(0x85000000+3*0xD8,0x103);f.mem.erase(0x85000000+20*0xD8+0xD7);
  f.Sample(api,m);assert(f.ints["re4.medallion.state.available"]==0&&f.ints["re4.medallion.0.show"]==0);
  f.Put<uint64_t>(0x84000030,UINT64_MAX);assert(!f.Poll().medallions_unbroken);
  f.MapRoom(0x201);f.Sample(api,m);
  assert(f.ints["re4.submission.show"]==1&&f.ints["re4.medallion.0.show"]==0);
  // Spoilers are on here: SW area 18 draws its own native treasure marks, never sub-missions;
  // area 22 (0x513) draws none.
  f.MapRoom(0x500);f.Sample(api,m);
  assert(f.ints["re4.treasure.show"]==1&&f.ints["re4.submission.show"]==0);
  f.MapRoom(0x513);f.Sample(api,m);
  assert(f.ints["re4.treasure.show"]==0&&f.ints["re4.submission.show"]==0);
  api->destroy(m);++groups;}
 if(have_geometry) {Fixture f;f.Items();assert(f.Poll().village_treasure_map==false);
 f.Put(0x220000001C,std::array<uint8_t,14>{0xA9,0,1,0,1,0,0,0,0,0,0,0,0,0});
 assert(f.Poll().village_treasure_map==true);
 f.Put<uint8_t>(0x2200000021,1);assert(f.Poll().village_treasure_map==false);
 f.Put<uint8_t>(0x2200000021,0);f.Put<uint16_t>(0x220000001E,0);
 assert(f.Poll().village_treasure_map==false);
 f.Put<uint16_t>(0x220000001E,1);f.MapRoom(0x100);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.treasure.show"]==1);
 // Owning only the village's own Treasure Map must not leak into island's gate.
 f.MapRoom(0x300);f.Sample(api,m);
 assert(f.ints["re4.map.available"]==1&&f.ints["re4.map.island"]==1);
 assert(f.ints["re4.treasure.map.available"]==1&&f.ints["re4.treasure.map.owned"]==0&&f.ints["re4.treasure.show"]==0);
 f.MapRoom(0x120);f.Sample(api,m); // genuinely unmapped room
 // re4.treasure.map.available reflects raw inventory (unconditional on area, by design);
 // re4.treasure.available/.show are the area-gated binds and must drop with the map.
 assert(f.ints["re4.map.available"]==0&&f.ints["re4.treasure.available"]==0&&f.ints["re4.treasure.show"]==0);
 f.MapRoom(0x100);f.mem.erase(0x2200000029);f.Sample(api,m);
 assert(f.ints["re4.treasure.map.available"]==0&&f.ints["re4.treasure.show"]==0);
 api->destroy(m);++groups;}
 {Fixture f;f.Items();
 f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
  auto it=values.find(key);return it==values.end()?fallback:it->second;};
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);assert(m);f.Sample(api,m);
 assert(f.ints["re4.slot.0.present"]==1&&f.ints["re4.slot.0.equipped"]==1);
 assert(f.texts["re4.slot.0.ammo"]=="35"&&f.ints["re4.slot.1.not_present"]==1);
 f.ints["@flag:slots.assignment.0"]=0x270000;f.Sample(api,m);
 assert(f.ints["re4.slot.0.present"]==0&&f.ints["re4.slot.2.equipped"]==1);
 f.ints["@flag:slots.assignment.0"]=0x23;f.Sample(api,m);
 assert(f.ints["re4.slot.0.missing"]==1&&f.ints["re4.slot.0.ammo.available"]==0);
 f.mem.erase(0x2200000000);f.Sample(api,m);
 assert(f.ints["re4.slots.ready"]==0&&f.ints["re4.slot.0.missing"]==0);
 assert(f.texts["re4.slot.0.ammo"].empty());
 api->destroy(m);++groups;}
 {PlacedItem item{1,39,1,2,1,0,35};
 assert((CaseRect(item,{15,8})==std::array<int,4>{0,0,3,2}));
 item.orientation=1;item.centre_x2=1;item.centre_y2=2;
 assert((CaseRect(item,{15,8})==std::array<int,4>{0,0,2,3}));
 item.centre_x2=0;assert(!CaseRect(item,{15,8}));
 item.centre_x2=29;assert(!CaseRect(item,{15,8}));++groups;}
 {constexpr std::array<std::array<uint8_t,2>,4> capacities{{{10,6},{11,7},{12,8},{15,8}}};
 const int cells[]={120,108,96,80};
 const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 for(size_t c=0;c<capacities.size();++c) {
  const auto capacity=capacities[c];const auto grid=CaseGeometry(capacity);
  assert(grid.cell==cells[c] && !(grid.x%2) && !(grid.y%2));
  assert(grid.x>=20 && grid.x+capacity[0]*grid.cell<=1220);
  assert(grid.y>=104 && grid.y+capacity[1]*grid.cell<=872);
  assert(grid.x*2+capacity[0]*grid.cell==1240);
  // Round-trip every occupied cell through the actual module action and selection publisher.
  Fixture f;f.Items();f.Put<uint16_t>(0x220000001C,0x7C+c);f.Put<uint8_t>(0x2200000020,1);
  auto* m=api->create(&f.host,nullptr);assert(m);
  for(int y=0;y<capacity[1];++y)for(int x=0;x<capacity[0];++x) {
   // Item 0x08, a native 1x1 piece, also exercises an item outside the old name switch.
   f.Put(0x220000000E,std::array<uint8_t,14>{8,0,1,0,1,0,0,0,0,0,
         static_cast<uint8_t>(x*2),static_cast<uint8_t>(y*2),0,1});
   f.Sample(api,m);
   assert(f.floats["re4.case.scale"]==grid.cell*1000.0/120);
   for(auto point:std::array<std::array<int,2>,3>{{{0,0},{grid.cell/2,grid.cell/2},{grid.cell-1,grid.cell-1}}}) {
    const uint64_t packed=(uint64_t(grid.x+x*grid.cell+point[0]-20)<<32)|
                           uint32_t(grid.y+y*grid.cell+point[1]-104);
    assert(ext->on_action(m,"case_select",static_cast<int64_t>(packed)));
    f.Sample(api,m);
    assert(f.texts["re4.case.name"].empty() && f.texts["re4.case.category"].empty());
    assert(f.ints["re4.case.selection.x"]==grid.x+x*grid.cell);
    assert(f.ints["re4.case.selection.y"]==grid.y+y*grid.cell);
    assert(f.ints["re4.case.shape.1x1.0.x"]==grid.x+(x*2+1)*grid.cell/2-540);
    assert(f.ints["re4.case.shape.1x1.0.y"]==grid.y+(y*2+1)*grid.cell/2-360);
    assert(f.ints.at("re4.case.shape.1x1.count")==1);
   }
  }
  // Rotated weapon 0x23 at the bottom/right, then weapon 0x27 with its badge inside the footprint.
  for(unsigned id:{0x23u,0x27u})for(unsigned rotation:{0u,1u}) {
   auto size=CaseSize(id);if(rotation)std::swap(size[0],size[1]);
   const auto x=capacity[0]-size[0],y=capacity[1]-size[1];
   f.Put(0x220000000E,std::array<uint8_t,14>{static_cast<uint8_t>(id),0,1,0,1,0,0,0,0x60,0,
     static_cast<uint8_t>(x*2+size[0]-1),static_cast<uint8_t>(y*2+size[1]-1),static_cast<uint8_t>(rotation),1});
   f.Sample(api,m);
   const uint64_t packed=(uint64_t(grid.x+x*grid.cell-20)<<32)|uint32_t(grid.y+y*grid.cell-104);
   assert(ext->on_action(m,"case_select",static_cast<int64_t>(packed)));f.Sample(api,m);
   assert(f.texts["re4.case.name"]==WeaponName(id));
   assert(f.texts["re4.picker.0.name"]==WeaponName(id));
   assert(f.ints["re4.case.selection.shape"]==size[0]*16+size[1]);
   if(id==0x27){assert(f.texts["re4.case.ammo"]=="12");
    assert(f.ints["re4.case.badge.x"]==grid.x+capacity[0]*grid.cell-108);
    assert(f.ints["re4.case.badge.y"]==grid.y+capacity[1]*grid.cell-46);}
  }
  // The fixed tap area includes the metal lip; a tap above the grid must clear selection.
  if(grid.y>104) {
   const uint64_t lip=uint64_t(grid.x-20)<<32;
   assert(ext->on_action(m,"case_select",static_cast<int64_t>(lip)));f.Sample(api,m);
   assert(f.texts["re4.case.name"].empty() && !f.ints["re4.case.selection.shape"]);
   assert(!f.ints["re4.case.ammo.available"]);
  }
  // Half-open bounds: the first pixel outside the last cell must deselect.
  const uint64_t outside=(uint64_t(grid.x+capacity[0]*grid.cell-20)<<32)|
                         uint32_t(grid.y+(capacity[1]-1)*grid.cell-104);
  assert(ext->on_action(m,"case_select",static_cast<int64_t>(outside)));f.Sample(api,m);
  assert(f.texts["re4.case.name"].empty() && !f.ints["re4.case.ammo.available"]);
  api->destroy(m);
 }++groups;}
 {assert(std::string(WeaponName(UINT16_MAX)).empty());
 assert(std::string(WeaponName(0x23)).empty()); // no RomFS supplied: fail closed
 ++groups;}
 {uint32_t slots=0;
 for(size_t n=0;n<4;++n)slots=AssignSlot(slots,n,static_cast<uint8_t>(n+1));
 assert(slots==0x04030201);
 assert(AssignUnique(slots,0,3)==0x04000203);
 const auto first=CommitSlots(slots,slots),second=CommitSlots(slots,first);
 assert(first!=second && static_cast<uint32_t>(first)==slots && static_cast<uint32_t>(second)==slots);
 assert(CommitSlots(0,first,true)==-1);
 assert((CaseSize(39)==std::array<unsigned,2>{3,2}));
 assert((CaseSize(20)==std::array<unsigned,2>{1,2}));assert(SwapSlots(slots,0,3)==0x01030204);
 assert(SwapSlots(SwapSlots(slots,1,2),1,2)==slots);
 assert(AssignSlot(slots,2,0)==0x04000201);++groups;}
 // ---- MAPS: castle/island room->area and the documented village-200/castle-200 collision ----
 if(have_geometry) {assert(std::string(MapArea(0x201))=="castle-201");
 assert(std::string(MapArea(0x219))=="castle-219");
 assert(std::string(MapArea(0x22a))=="castle-22a");
 assert(std::string(MapArea(0x300))=="island-300");
 assert(std::string(MapArea(0x306))=="island-306");
 assert(std::string(MapArea(0x333))=="island-333");
 // The same room changes native map at the castle-entry story event.
 assert(!*MapArea(0x200));
 assert(std::string(MapArea(0x200,false))=="village-200");
 assert(std::string(MapArea(0x200,true))=="castle-200");
 {Fixture f;f.MapRoom(0x200);assert(f.Poll().stage==2);}
 for(auto room:{0x400,0x40a,0x51f,0x5a0})assert(!*MapArea(room)); // unverified rooms stay unavailable
 assert(std::string(MapArea(0x500))=="stage5-500");
 // Native mapDataInit areas 18..25 (18 rooms also live-verified by controlled placement).
 assert(std::string(MapArea(0x501))=="stage5-501");
 assert(std::string(MapArea(0x502))=="stage5-502");
 assert(std::string(MapArea(0x504))=="stage5-504");
 assert(std::string(MapArea(0x505))=="stage5-505");
 assert(std::string(MapArea(0x507))=="stage5-507");
 assert(std::string(MapArea(0x509))=="stage5-509");
 assert(std::string(MapArea(0x50a))=="stage5-50a");
 assert(std::string(MapArea(0x50b))=="stage5-50b");
 assert(std::string(MapArea(0x50c))=="stage5-50c");
 assert(std::string(MapArea(0x50d))=="stage5-50d");
 assert(std::string(MapArea(0x510))=="stage5-510");
 assert(std::string(MapArea(0x513))=="stage5-513");
 assert(std::string(MapArea(0x515))=="stage5-515");
 assert(std::string(MapArea(0x517))=="stage5-517");
 assert(std::string(MapArea(0x519))=="stage5-519");
 assert(std::string(MapArea(0x51b))=="stage5-51b");
 assert(std::string(MapArea(0x51e))=="stage5-51e");
 for(uint16_t room:{0x503,0x506,0x508,0x50e,0x50f,0x512,0x514,0x516,0x518,0x51a,0x51c,0x51d}) {
  char key[16];std::snprintf(key,sizeof key,"stage5-%03x",room);
  assert(std::string(MapArea(room))==key&&MapBounds(key)&&MapCameraBounds(key));
 }
 // No native row (0x511) or a native placeholder square only (0x51F, 0x520): never mapped.
 assert(!*MapArea(0x511));assert(!*MapArea(0x51f));assert(!*MapArea(0x520));
 // Every native SW room carries its native area in the generated legend tables.
 {const std::pair<uint16_t,unsigned> areas[]={{0x500,18},{0x501,18},{0x502,18},{0x503,18},
   {0x504,19},{0x505,19},{0x506,19},{0x507,19},{0x508,19},{0x509,19},{0x50a,19},{0x50b,19},
   {0x50c,20},{0x50d,20},{0x50e,20},{0x50f,20},{0x510,20},{0x512,21},{0x514,21},{0x513,22},
   {0x515,23},{0x516,23},{0x517,23},{0x518,24},{0x519,24},{0x51a,24},{0x51b,24},{0x51c,24},
   {0x51d,25},{0x51e,25}};
  for(const auto& [room,area]:areas) {
   char key[16];std::snprintf(key,sizeof key,"stage5-%03x",room);
   const auto row=std::find_if(std::begin(MapStateRooms),std::end(MapStateRooms),[&](const auto& r){return r.key==key;});
   assert(row!=std::end(MapStateRooms)&&row->room==room&&row->area==area);
  }
  assert(std::count_if(std::begin(MapStateRooms),std::end(MapStateRooms),[](const auto& r){return r.key.starts_with("stage5-");})==30);}
 assert(MapBounds("stage5-500").has_value());
 assert(MapBounds("castle-201").has_value());assert(MapBounds("island-300").has_value());
 assert(MapBounds("castle-200").has_value());
 ++groups;}
 // ---- Separate Ways (stage5) native pause-Map label, LIVE-VERIFIED 2026-11-09
 // (analysis/20261109-sw-label-and-zip.md): one verified room per map_obj5X.dat file in use
 // placed by the controlled-placement method, the real native Map opened and the plate read.
 // Locks in the fix for the owner-found bug where the Field plate showed VILLAGE for every
 // stage5 room regardless of which file governs it -- re4.map.{village,castle,island} must
 // now reuse those same native labels for stage5 too (same bind names/widgets the main
 // campaign already uses, no new art). ----
 if(have_geometry) {Fixture f;f.Items();
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);
 auto at=[&](uint16_t room){f.MapRoom(room);f.Sample(api,m);};
 // A mapped stage5 room must publish BOTH its area (the manifest key that selects the
 // room's own geometry PNG + drives the marker, "stage5-xxx") AND the correct native
 // label flag -- never just one. Orchestrator review 2026-11-09 found a live smoke capture
 // with the correct label but an empty map (no geometry/marker) and asked for this to be
 // locked in explicitly; root-caused to an incomplete smoke-test deployment (the
 // assets/stage5-*.png files were never copied, independently of manifest.json/module.so),
 // not a code defect -- re-verified live with a full deploy: 0x501 and 0x50c both rendered
 // geometry + marker + the correct label together. This assertion guards the real
 // invariant going forward.
 char buf[16];
 auto area_of=[&](uint16_t room){std::snprintf(buf,sizeof buf,"stage5-%03x",room);return std::string(buf);};
 // map_obj5a.dat / map_obj5b.dat -> VILLAGE.
 for(uint16_t room:{0x500,0x501,0x502,0x503,0x504,0x505,0x506,0x507,0x508,0x509,0x50a,0x50b}) {
  at(room);
  assert(f.ints["re4.map.available"]==1&&f.ints["re4.map.village"]==1);
  assert(f.ints["re4.map.castle"]==0&&f.ints["re4.map.island"]==0);
  assert(f.texts["re4.map.area"]==area_of(room));
 }
 // map_obj5c.dat -> CASTLE.
 for(uint16_t room:{0x50c,0x50d,0x50e,0x50f,0x510}) {
  at(room);
  assert(f.ints["re4.map.available"]==1&&f.ints["re4.map.castle"]==1);
  assert(f.ints["re4.map.village"]==0&&f.ints["re4.map.island"]==0);
  assert(f.texts["re4.map.area"]==area_of(room));
 }
 // map_obj5e.dat / map_obj5f.dat / map_obj5g.dat / map_obj5h.dat -> ISLAND.
 for(uint16_t room:{0x512,0x513,0x514,0x515,0x516,0x517,0x518,0x519,0x51a,0x51b,0x51c,0x51d,0x51e}) {
  at(room);
  assert(f.ints["re4.map.available"]==1&&f.ints["re4.map.island"]==1);
  assert(f.ints["re4.map.village"]==0&&f.ints["re4.map.castle"]==0);
  assert(f.texts["re4.map.area"]==area_of(room));
 }
 // The campaign's own village/castle/island labels (distinct room-id ranges) are unaffected.
 at(0x100);assert(f.ints["re4.map.village"]==1&&f.ints["re4.map.castle"]==0&&f.ints["re4.map.island"]==0);
 at(0x201);assert(f.ints["re4.map.castle"]==1&&f.ints["re4.map.village"]==0&&f.ints["re4.map.island"]==0);
 at(0x300);assert(f.ints["re4.map.island"]==1&&f.ints["re4.map.village"]==0&&f.ints["re4.map.castle"]==0);
 // 0x511 has no native map row (getAreaNo area 0): no map, no plate.
 at(0x511);assert(f.ints["re4.map.available"]==0);
 assert(Stage5Label(0x511)==0&&Stage5Label(0x51f)==0);
 api->destroy(m);++groups;}
 // ---- Separate Ways pose in the native placed frame (analysis/20261114-gallery-sw.md B3/B6):
 // the native barycentric world->map transfer, and world/10 + root only off-mesh in rooms whose
 // map mesh is exactly that affine map. Raw positions are retained live samples. ----
 if(have_geometry) {auto near=[](const std::optional<std::array<double,3>>& p,double x,double y,double tol=.05){
   return p && std::abs((*p)[0]-x)<tol && std::abs((*p)[1]-y)<tol;};
  assert(near(TestMapPose("stage5-501",{-19537.f,-0.000793f,-17983.7f},{0,1}),2130.225,-429.559));
  assert(near(TestMapPose("stage5-508",{4100.f,-16740.7f,-109100.f},{0,1}),24217.535,9702.485));
  assert(!TestMapPose("stage5-508",{1e6f,0,1e6f},{0,1})); // distorted piece: no off-mesh guess
  assert(near(TestMapPose("stage5-514",{-24283.8f,-3980.69f,-54126.f},{0,1}),-2428.38,6212.60));
  const auto r503=TestMapPose("stage5-503",{-1743.f,-1037.5f,1469.8f},{0,1});
  assert(near(r503,11151.1,-5825.0,.1)&&std::abs((*r503)[2]-160)<1e-4); // native heading -70
  assert(near(TestMapPose("stage5-507",{158743.f,-9580.85f,-32723.3f},{0,1}),24998.57,7449.51));
  // Affine rooms keep the off-mesh contract; the result includes the room's root offset.
  for(const auto& mesh:MapMeshes)if(mesh.area.starts_with("stage5-")) {
   const auto off=TestMapPose(mesh.area,{1e6f,0,1e6f},{0,1});
   assert(off.has_value()==mesh.affine);
   if(off)assert(std::abs((*off)[0]-(1e5+mesh.ox))<1e-6&&std::abs((*off)[1]+(1e5+mesh.oz))<1e-6);
   assert(!TestMapFloor(mesh.area,{1e6f,0,1e6f})); // no hit under Ada: no floor
  }
  // Live 0x51E: Ada 1142 units above her only hit (outside the narrow bracket). The affine pose
  // holds and the floor follows that hit (native drew it); non-affine rooms never do this.
  assert(near(TestMapPose("stage5-51e",{-66973.3f,20798.7f,69203.7f},{0,1}),-1349.46,5638.88));
  assert(TestMapFloor("stage5-51e",{-66973.3f,20798.7f,69203.7f})==2);
  assert(!TestMapFloor("stage5-51d",{9179.63f,-958.177f,-13915.2f})); // non-affine, native showed none
  assert(!TestMapPose("stage5-51d",{9179.63f,-958.177f,-13915.2f},{0,1}));
  for(const char* key:{"stage5-503","stage5-506","stage5-507","stage5-508","stage5-50f","stage5-510","stage5-51d"})
   for(const auto& mesh:MapMeshes)if(mesh.area==key)assert(!mesh.affine);
  for(const auto& mesh:MapMeshes)if(!mesh.area.starts_with("stage5-"))assert(!mesh.affine);
  ++groups;}
 // ---- Separate Ways treasure (scf_check_treasure): one map item per native area owned by the
 // current character (ItemMgr+0x1B), alive bit and id only; areas 22/25 never draw. ----
 if(have_geometry) {Fixture f;f.Items();f.Legend();
  f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
   auto it=values.find(key);return it==values.end()?fallback:it->second;};
  f.Put<uint8_t>(0x8300001B,2);f.Put<int32_t>(0x83000030,5);
  for(int n=0;n<5;++n)f.Put(0x2200000000+n*14,std::array<uint8_t,14>{});
  auto give=[&](uint16_t id,uint8_t owner=2,uint8_t alive=1,uint16_t count=1){
   f.Put(0x2200000000+4*14,std::array<uint8_t,14>{uint8_t(id),uint8_t(id>>8),uint8_t(count),uint8_t(count>>8),alive,owner});};
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);assert(m);
  auto show=[&](uint16_t room){f.MapRoom(room);f.Sample(api,m);assert(f.ints["re4.map.available"]==1);return f.ints["re4.treasure.show"];};
  give(0x10B);assert(show(0x500)==1&&f.ints["re4.treasure.map.owned"]==1&&f.ints["re4.map.native_area"]==18);
  assert(f.Poll().sw_treasure_maps==1);
  give(0x10C);assert(show(0x500)==0&&f.ints["re4.treasure.map.available"]==1&&f.ints["re4.treasure.map.owned"]==0);
  assert(show(0x507)==1);
  give(0x10B,0);assert(show(0x500)==0);   // another character's record
  give(0x10B,2,0);assert(show(0x500)==0); // not alive
  give(0x10B,2,1,0);assert(show(0x500)==1); // native ignores the count
  give(0x10D);assert(show(0x50e)==1&&show(0x504)==0);
  give(0x10E);assert(show(0x512)==1&&show(0x513)==0&&show(0x516)==1);
  give(0x10F);assert(show(0x518)==1&&show(0x51e)==0&&show(0x51d)==0);
  // No SW map item: nothing, unless the spoiler setting shows every drawn area's marks.
  f.Put(0x2200000000+4*14,std::array<uint8_t,14>{});
  for(uint16_t room:{0x500,0x504,0x50c,0x512,0x513,0x515,0x518,0x51e})assert(show(room)==0);
  f.ints["@flag:opt.map_spoilers"]=1;
  for(uint16_t room:{0x500,0x504,0x50c,0x512,0x515,0x518})assert(show(room)==1);
  for(uint16_t room:{0x513,0x51e})assert(show(room)==0);
  // SW never shows medallion or sub-mission marks.
  for(uint16_t room:{0x500,0x50c,0x518}){show(room);assert(f.ints["re4.medallion.show"]==0&&f.ints["re4.submission.show"]==0);}
  // Ada's records never feed the Leon-only case/slot outputs.
  assert(!f.Poll().placed_items&&!f.Poll().owned_items&&!f.Poll().weapon);
  api->destroy(m);++groups;}
 // The current multi-floor SW room keeps its art when Ada's pose is the affine fallback.
 if(have_geometry) {Fixture f;f.Items();f.Legend();f.Put<uint16_t>(0x82005020,0x51E);
  f.Put(0x2100000128,std::array<float,3>{-66973.3f,20798.7f,69203.7f});
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
  assert(f.ints["re4.map.available"]==1&&f.ints["re4.map.room.stage5-51e.state"]==4);
  assert(f.texts["re4.map.room.stage5-51e.art"]=="module:re4/room/stage5/51e/4/f2");
  api->destroy(m);++groups;}
 // Leon (character 0) in Separate Ways uses the same single acquisition.
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x500);
  f.Put(0x2200000000,std::array<uint8_t,14>{0x0B,1,1,0,1,0});
  auto s=f.Poll();assert(s.sw_treasure_maps==1&&s.placed_items&&s.weapon==0x27);
  f.MapRoom(0x100);assert(!f.Poll().sw_treasure_maps);++groups;}
 // ---- ROOM 0x200: native story flag, not chapter/position/room high byte ----
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x200);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);
 auto sample=[&]{f.Sample(api,m);};
 sample(); // A missing progress read must not guess either colliding map.
 assert(f.ints["re4.progress.castle_entered.available"]==0);
 assert(f.ints["re4.map.available"]==0&&f.texts["re4.map.area"].empty());
 f.Put<uint32_t>(0x82005344,0x0040001f);sample();
 assert(f.Poll().castle_entered==false);
 assert(f.ints["re4.progress.castle_entered.available"]==1);
 assert(f.ints["re4.progress.castle_entered"]==0);
 assert(f.ints["re4.stage"]==2&&f.texts["re4.map.area"]=="village-200");
 assert(f.ints["re4.map.village"]==1&&f.ints["re4.map.castle"]==0);
 f.Put<uint8_t>(0x8200500E,20);sample(); // NG+ header/chapter cannot select the map.
 assert(f.texts["re4.map.area"]=="village-200");
 f.Put<uint32_t>(0x82005344,0x00c0001f);sample();
 assert(f.Poll().castle_entered==true);
 assert(f.ints["re4.progress.castle_entered"]==1);
 assert(f.ints["re4.stage"]==2&&f.texts["re4.map.area"]=="castle-200");
 assert(f.ints["re4.map.castle"]==1&&f.ints["re4.map.village"]==0);
 // Only the village map is owned by this fixture; switching must change ownership gates.
 f.Put(0x220000001C,std::array<uint8_t,14>{0xa9,0,1,0,1,0,0,0,0,0,0,0,0,0});sample();
 assert(f.ints["re4.treasure.show"]==0);
 f.Put<uint32_t>(0x82005344,0x0040001f);sample();
 assert(f.ints["re4.treasure.show"]==1);
 f.mem.erase(0x82005345);sample();
 assert(f.ints["re4.progress.castle_entered.available"]==0&&f.ints["re4.map.available"]==0);
 f.Put<uint32_t>(0x82005344,0x00800000);
 f.Put<uint64_t>(0x80B00008,0);sample();
 assert(!f.Poll().castle_entered&&f.ints["re4.progress.castle_entered.available"]==0);
 api->destroy(m);++groups;}
 // ---- MAPS: per-stage Treasure Map ownership (village 0xA9, castle 0x54, island 0x55) ----
 {Fixture f;f.Items();
 f.Put(0x220000001C,std::array<uint8_t,14>{0x54,0,1,0,1,0,0,0,0,0,0,0,0,0});
 auto s=f.Poll();
 assert(s.castle_treasure_map==true&&s.village_treasure_map==false&&s.island_treasure_map==false);
 f.Put(0x220000001C,std::array<uint8_t,14>{0x55,0,1,0,1,0,0,0,0,0,0,0,0,0});
 s=f.Poll();
 assert(s.island_treasure_map==true&&s.castle_treasure_map==false&&s.village_treasure_map==false);
 ++groups;}
 if(have_geometry) {Fixture f;f.Items();
 f.Put(0x220000001C,std::array<uint8_t,14>{0x54,0,1,0,1,0,0,0,0,0,0,0,0,0});
 f.MapRoom(0x201);
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
 assert(f.ints["re4.map.available"]==1&&f.ints["re4.map.castle"]==1&&f.ints["re4.map.village"]==0);
 assert(f.texts["re4.map.area"]=="castle-201");
 assert(f.ints["re4.treasure.map.owned"]==1&&f.ints["re4.treasure.show"]==1);
 // Owning the castle map must not leak into the island gate.
 f.MapRoom(0x300);f.Sample(api,m);
 assert(f.ints["re4.map.island"]==1&&f.ints["re4.map.castle"]==0);
 assert(f.ints["re4.treasure.map.owned"]==0&&f.ints["re4.treasure.show"]==0);
 api->destroy(m);++groups;}
 // Cached publishers must replay every bind into the per-tick-cleared snapshot.
 {Fixture f,reference;InventoryPublisher cache;InventorySelection selected;
 f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
  auto it=values.find(key);return it==values.end()?fallback:it->second;};
 reference.host.get_i64=f.host.get_i64;
 Snapshot s;s.placed_items=std::vector<PlacedItem>{{5,0x27,1,2,1,0,35,5},{6,1,3,6,0,0}};
 s.owned_items=std::vector<OwnedItem>{{9,0xa2,1},{10,0xb9,2}};
 s.case_capacity=std::array<uint8_t,2>{10,6};s.health=std::array<int16_t,2>{800,1600};
 s.weapon=0x27;s.ammo=35;s.reserve_ammo=99;s.ashley_command=1;
 s.save_identity=std::array<uint32_t,3>{2,7,1};s.stage=1;s.character=0;
 selected.selected_slot=5;selected.key_selected=9;
 const auto verify=[&] {
  std::erase_if(f.ints,[](const auto& entry){return !entry.first.starts_with("@");});
  f.texts.clear();f.floats.clear();
  cache.Publish(f.host,s,"@flag:slots.assignment.0",selected);
  reference.ints=f.ints;
  std::erase_if(reference.ints,[](const auto& entry){return !entry.first.starts_with("@");});
  reference.texts.clear();reference.floats.clear();
  auto pages=selected.key_pages;
  PublishSlots(reference.host,s,"@flag:slots.assignment.0");
  PublishKeys(reference.host,s,pages,selected.key_selected);
  PublishCaseSelection(reference.host,s,selected.selected_slot);
  assert(f.ints==reference.ints);
  for(const auto& [k,v]:reference.texts)assert(f.texts.contains(k) && f.texts.at(k)==v);
  for(const auto& [k,v]:reference.floats)assert(f.floats.contains(k) && f.floats.at(k)==v);
 };
 verify();const auto cold=cache.StringBuilds();assert(cold>0);
 const auto ints=f.ints;const auto texts=f.texts;const auto floats=f.floats;
 for(int tick=0;tick<4;++tick) {
  verify();assert(cache.StringBuilds()==cold);
  assert(f.ints==ints && f.texts==texts && f.floats==floats);
 }
 const auto steady_builds=cache.StringBuilds()-cold;
 assert(f.texts["re4.inventory.positions"]=="5:39:1:2:1:0;6:1:3:6:0:0;");
 assert(f.texts["re4.save.fingerprint"]=="2:7:1" && f.texts["re4.hp.text"]=="800 / 1600");
 const auto change=[&] {const auto before=cache.StringBuilds();verify();assert(cache.StringBuilds()>before);
  const auto after=cache.StringBuilds();verify();assert(cache.StringBuilds()==after);};
 f.ints["@flag:slots.assignment.0"]=1;change();
 s.placed_items->front().magazine=12;change();
 s.placed_items->front().capacity_level=6;change();
 s.placed_items->front().count=2;change();
 s.placed_items->front().centre_x2=4;change();
 s.placed_items->front().orientation=1;change();
 s.case_capacity=std::array<uint8_t,2>{15,8};change();
 s.weapon=1;change();s.health=std::array<int16_t,2>{400,1600};change();
 s.ashley_command=2;change();selected.selected_slot=6;change();
 selected.key_selected=10;change();s.owned_items->back().count=3;change();
 for(int n=0;n<9;++n)s.owned_items->push_back({uint16_t(11+n),0xb9,uint16_t(4+n)});
 change();selected.key_pages[1]=1;change();
 assert(f.texts["re4.keys.2.0.count"]=="10");
 selected.ResetForSave();change();assert(f.ints["re4.case.selection.shape"]==0);
 s.ammo=12;change();s.reserve_ammo=98;change();
 s.save_identity=std::array<uint32_t,3>{5,41,3};change();
 s.stage=4;s.character=2;change();assert(cache.merc_flag_key=="@flag:merc.best.4.2");
 s.placed_items.reset();s.owned_items.reset();s.case_capacity.reset();s.health.reset();
 s.ammo.reset();s.reserve_ammo.reset();s.save_identity.reset();change();
 assert(!f.ints["re4.slot.0.present"] && !f.ints["re4.keys.2.0.available"]);
 assert(f.texts["re4.inventory.positions"].empty() && f.texts["re4.hp.text"].empty());
 std::cout<<"Inventory steady strings built: "<<steady_builds
          <<", cold cached strings: "<<cold<<", cleared-snapshot replay passed\n";
 ++groups;}
 {InventorySelection selection;
 selection.selected_slot=5;selection.key_selected=8;selection.key_pages={2,3};
 selection.ResetForSave();
 assert(selection.selected_slot==UINT16_MAX && selection.key_selected==UINT16_MAX);
 assert((selection.key_pages==std::array<size_t,2>{} && selection.selected));++groups;}
 // ---- PER-SAVE SLOT ISOLATION: bucketed persisted flag, keyed off {chapter,save_cnt,
 // terminal_no} (candidate fields, the r5-save-identity research notes), falls back to
 // bucket 0 with no identity yet. ----
 {Fixture f;f.Items();
 f.host.get_i64=[](void* p,const char* key,int64_t fallback){auto& values=static_cast<Fixture*>(p)->ints;
  auto it=values.find(key);return it==values.end()?fallback:it->second;};
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);assert(m);
 f.Sample(api,m);
 assert(f.ints["re4.save.bucket"]==0&&f.texts["re4.save.fingerprint"].empty());
 // Two concrete, independently-confirmed-distinct fingerprints (own bucket computation,
 // not asserted blind): {chapter=2,save_cnt=7,terminal_no=1} and {5,41,3}.
 f.Put<uint8_t>(0x8200500E,2);f.Put<uint16_t>(0x82005010,7);f.Put<uint8_t>(0x82005016,1);
 f.Sample(api,m);
 const auto bucket_a=f.ints["re4.save.bucket"];
 assert(bucket_a==3&&f.texts["re4.save.fingerprint"]=="2:7:1");
 // Select a real case item before switching saves.
 f.Put<uint16_t>(0x220000001C,0x7C);f.Put<uint8_t>(0x2200000020,1);
 f.Sample(api,m);
 const auto* ext=eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION,EDEN_DSMOD_EXT_HASH);
 const auto grid=CaseGeometry(*f.Poll().case_capacity);
 const uint64_t tap=(uint64_t(grid.x-20)<<32)|uint32_t(grid.y-104);
 assert(ext->on_action(m,"case_select",static_cast<int64_t>(tap)));
 f.Sample(api,m);assert(f.ints["re4.case.selection.shape"]!=0);
 f.ints["@flag:slots.assignment.3"]=0x2700;
 f.Sample(api,m);assert(f.ints["re4.slot.0.present"]==0); // reads bucket 3's own flag
 f.Put<uint8_t>(0x8200500E,5);f.Put<uint16_t>(0x82005010,41);f.Put<uint8_t>(0x82005016,3);
 f.Sample(api,m);
 const auto bucket_b=f.ints["re4.save.bucket"];
 assert(bucket_b==0&&bucket_a!=bucket_b&&f.texts["re4.save.fingerprint"]=="5:41:3");
 // Switching to bucket 0 (unset flag, default -1) must re-derive from owned items, not
 // retain bucket 3's assignment, and must not spuriously fire "slots.committed".
 assert(f.ints["re4.slot.0.present"]==1&&f.ints["re4.slots.committed"]==0);
 assert(f.ints["re4.case.selection.shape"]==0 && f.texts["re4.case.ammo"].empty());
 f.Sample(api,m);assert(f.ints["re4.case.selection.shape"]==0);
 // An out-of-range save_cnt/chapter/terminal_no must never be trusted as an identity.
 f.Put<uint16_t>(0x82005010,10000);f.Sample(api,m);
 assert(f.ints["re4.save.bucket"]==0&&f.texts["re4.save.fingerprint"].empty());
 api->destroy(m);++groups;}
 // ---- TAP-TO-EQUIP guard: FLAGS-ONLY safe (owner decision 2026-11-01) once every known
 // bit is satisfied, even though ROUTINE_CALM is never computed host-side (Leon's r_no_0/1/2
 // offset still unlocated, ANSWERS.md Q1) -- an accepted risk, not a bug. ----
 if(have_geometry) {Fixture f;f.Items();
 f.Put<uint32_t>(0x82005090,0x02000000); // STA_SSCRN_ENABLE
 f.Put<uint32_t>(0x82005094,0);f.Put<uint32_t>(0x82005098,0);f.Put<uint32_t>(0x8200509c,0);
 f.Put<uint32_t>(0x820001a8,0);
 f.Put<uint64_t>(f.host.main_base+0xB26C98,0x82010000); // sane non-null subscreen pointer
 f.Put<uint32_t>(f.host.main_base+0x1242E10,0); // NowLoading UI flag clear
 f.Put(0x82005028,std::array<int16_t,2>{2400,2400});
 f.MapRoom(0x101); // a held, unchanging room id for RoomStability to settle on
 const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
 auto* m=api->create(&f.host,nullptr);
 for(int n=0;n<40;++n)f.Sample(api,m); // let RoomStability settle
 assert(f.ints["re4.equip.safe"]==1);
 assert((f.ints["re4.equip.guard_bits"]&RE4_EQUIP_GUARD_ROUTINE_CALM)==0);
 assert((f.ints["re4.equip.guard_bits"]&RE4_EQUIP_GUARD_ALL_KNOWN)==RE4_EQUIP_GUARD_ALL_KNOWN);
 // Native room-init remains unsafe even while the room id is unchanged and all legacy
 // flags pass. Each phase must publish Loading and clear the fresh main-loop guard.
 for(uint8_t routine:{1,2,4}) {
  f.Put<uint8_t>(0x82000028,routine);f.Sample(api,m);
  assert(f.ints["re4.state"]==static_cast<int>(State::Loading));
  assert(f.ints["re4.equip.safe"]==0);
  assert((f.ints["re4.equip.guard_bits"]&RE4_EQUIP_GUARD_GAME_MAINLOOP)==0);
 }
 f.mem.erase(0x82000028);f.Sample(api,m);assert(f.ints["re4.equip.safe"]==0);
 f.Put<uint8_t>(0x82000028,3);f.Sample(api,m);assert(f.ints["re4.equip.safe"]==1);
 // Busy/reentrancy lock set -> that bit (and only that known bit) drops, unsafe again.
 f.Put<uint8_t>(0x8200503C,1);f.Sample(api,m);
 assert((f.ints["re4.equip.guard_bits"]&RE4_EQUIP_GUARD_BUSY_CLEAR)==0);
 assert(f.ints["re4.equip.safe"]==0);
 f.Put<uint8_t>(0x8200503C,0);f.Sample(api,m);assert(f.ints["re4.equip.safe"]==1);
 // A scripted-event freeze clears exactly its own bit, unsafe again.
 f.Put<uint32_t>(0x820001a8,0x80000000u);f.Sample(api,m);
 assert((f.ints["re4.equip.guard_bits"]&RE4_EQUIP_GUARD_NO_SCRIPT_FREEZE)==0);
 assert(f.ints["re4.equip.safe"]==0);
 f.Put<uint32_t>(0x820001a8,0);f.Sample(api,m);assert(f.ints["re4.equip.safe"]==1);
 // STA_SSCRN_REQUEST (case already mid-open) clears exactly its own bit, unsafe again.
 f.Put<uint32_t>(0x82005098,0x04000000u);f.Sample(api,m);
 assert((f.ints["re4.equip.guard_bits"]&RE4_EQUIP_GUARD_NOT_REQUESTED)==0);
 assert(f.ints["re4.equip.safe"]==0);
 f.Put<uint32_t>(0x82005098,0);f.Sample(api,m);assert(f.ints["re4.equip.safe"]==1);
 api->destroy(m);++groups;}
 // Case output follows the same cleared-snapshot contract: keep visible rows,
 // then omit the group counts and item fields when the owned case disappears.
 {Fixture f;f.Items();
  f.Put(0x220000001C,std::array<uint8_t,14>{0x7F,0,1,0,1});
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);
  for(int n=0;n<3;++n){f.Sample(api,m);assert(f.ints.at("re4.case.shape.3x2.count")==1);}
  f.Put(0x220000001C,std::array<uint8_t,14>{});f.Sample(api,m);
  for(const auto& [k,v]:f.ints)assert(!k.starts_with("re4.case.shape."));
  for(const auto& [k,v]:f.texts)assert(!k.starts_with("re4.case.shape."));
  api->destroy(m);++groups;}
 // The output cache must reuse predicates, yet invalidate for every predicate input.
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);f.Legend();f.Flag(29);
  auto s=f.Poll();MapLegendPublisher legend;
  auto publish=[&](std::optional<unsigned> area=1,std::optional<int> floor=0){
   legend.Publish(f.host,s,area,floor);
  };
  publish();assert(f.texts["re4.map.current.art"]=="module:re4/room/village/100/4/f0");
  const auto keys=LegendKeys().built;
  for(int n=0;n<100;++n)publish();
  assert(legend.rebuilds==1 && LegendKeys().built==keys);
  s.room=0x101;publish();assert(legend.rebuilds==2);
  publish(1,1);assert(legend.rebuilds==3);
  (*s.map_flags)[0]^=1;publish();assert(legend.rebuilds==4);
  (*s.map_passed)[0]^=1;publish();assert(legend.rebuilds==5);
  s.map_mode=1;publish();assert(legend.rebuilds==6);
  s.owned_items->push_back({3,0x8b,1});publish();assert(legend.rebuilds==7);
  s.owned_items.reset();publish();assert(legend.rebuilds==8);
  publish(0);assert(legend.rebuilds==9);
  publish({});assert(f.texts["re4.map.current.art"].empty());
  publish();assert(legend.rebuilds==10);
  ++groups;}
 // ---- Cleanup-pass optimization lock-in: ordinary gameplay (equipped weapon 0x27, no
 // Ashley, no case item) must resolve the player object exactly once per Poll(). Before
 // this round, the duplicate Object(layout->player) call (once inside the Title check,
 // once for position/facing) cost 2 extra host reads on every single sample in this
 // exact, common configuration: measured 19 host reads with the duplicate restored,
 // 17 with it memoized (same measurement method: a fresh Fixture's f.reads counter
 // after one Poll() call). A hard count, not a range, so any future read added to this
 // path is a deliberate, reviewed change to this assertion. ----
 {Fixture f;f.Items();f.Poll();const auto reads=f.reads;assert(reads==17);++groups;}
 // Same fully populated native village fixture used for the before/after read audit.
 // Cold reader: 25 baseline reads, plus seven map reads (including the separate
 // scenario-progress word required before page classification).
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);f.Legend();f.Flag(29);
  f.reads=0;f.Poll();assert(f.reads==32);++groups;}
 if(have_geometry) {Fixture f;f.Items();f.MapRoom(0x100);f.Legend();f.Flag(29);
  const auto* api=eden_dsmod_get_module(1,EDEN_DSMOD_MODULE_ABI_HASH);
  auto* m=api->create(&f.host,nullptr);f.Sample(api,m);
  // First native Field sample may still have the previous standby page.
  f.host.get_text=[](void*,const char* key)->const char* {
   return std::string_view(key)=="__page"?"standby":nullptr;
  };
  f.Sample(api,m);
  assert(f.ints["re4.map.available"]==1 && f.ints["re4.map.warm.ready"]==1);
  assert(f.texts["re4.map.warm.0.art"]==f.texts["re4.map.current.art"]);
  std::set<std::string> warm;
  for(int n=0;n<f.ints["re4.map.warm.count"];++n)
   assert(warm.insert(f.texts.at("re4.map.warm."+std::to_string(n)+".art")).second);
  // Loading can warm native state without displaying a Field map.
  f.Put<uint8_t>(0x82000028,4);f.Sample(api,m);
  assert(f.ints["re4.state"]==static_cast<int>(State::Loading));
  assert(f.ints["re4.map.available"]==0 && f.ints["re4.map.warm.ready"]==1);
  assert(f.ints["re4.map.warm.count"]==static_cast<int64_t>(warm.size()));
  f.Passed(0x101);
  f.Put<uint8_t>(0x82000028,3);f.host.get_text=nullptr;f.legend_publishes=0;f.max_read=0;f.Sample(api,m);
  assert(f.max_read==47952 && f.ints["re4.map.room.village-101.state"]==1);
  const auto cold_count=f.legend_publishes;
  const auto visible_ints=f.ints;
  const auto visible_texts=f.texts;
  const auto visible_floats=f.floats;
  const auto key_count=LegendKeys().built;
  const auto cold_bytes=f.read_bytes;
  for(int tick=0;tick<100;++tick) {
   f.ints["unrepublished.sentinel"]=7;f.texts["unrepublished.sentinel"]="stale";f.floats["unrepublished.sentinel"]=7;
   f.legend_publishes=0;f.max_read=0;f.read_bytes=0;f.Sample(api,m);
   assert(!f.ints.contains("unrepublished.sentinel") && !f.texts.contains("unrepublished.sentinel") && !f.floats.contains("unrepublished.sentinel"));
   for(const auto& [key,value]:visible_ints)if(key.starts_with("re4.map."))assert(f.ints.at(key)==value);
   for(const auto& [key,value]:visible_texts)if(key.starts_with("re4.map."))assert(f.texts.at(key)==value);
   for(const auto& [key,value]:visible_floats)if(key.starts_with("re4.map."))assert(f.floats.at(key)==value);
   assert(f.legend_publishes==cold_count && f.max_read<1000);
  }
  assert(LegendKeys().built==key_count);
  std::cout<<"Map cold/steady publishes: "<<cold_count<<"/"<<f.legend_publishes
           <<", steady keys built: 0, bytes cold/steady: "<<cold_bytes<<"/"<<f.read_bytes<<"\n";
  f.Put(0x220000001C,std::array<uint8_t,14>{0x8B,0,1,0,1});
  f.Sample(api,m);
  assert(f.ints.at("re4.map.door.1.3.state")==3);
  assert(f.texts.at("re4.map.door.1.3.art").ends_with("/3"));
  assert(!f.ints.contains("re4.map.door.1.3.1.show"));
  assert(!f.ints.contains("re4.map.door.1.3.2.show"));
  assert(!f.ints.contains("re4.map.door.1.3.3.show"));
  f.Put<uint32_t>(0x82005098,0x04000000);f.Sample(api,m);
  assert(f.ints["re4.map.native_area"]==-1 && f.ints["re4.map.room.village-100.show"]==0);
  f.legend_publishes=0;f.max_read=0;f.read_bytes=0;f.Sample(api,m);
  assert(f.legend_publishes==1 && f.max_read<1000);
  std::cout<<"Map paused steady publishes: "<<f.legend_publishes<<", bytes: "<<f.read_bytes<<"\n";
  f.Put<uint32_t>(0x82005098,0);f.Passed(0x101);f.Sample(api,m);
  assert(f.ints["re4.map.room.village-101.state"]==1);
  f.host.get_text=[](void*,const char* key)->const char* {
   return std::string_view(key)=="__page"?"case":nullptr;
  };
  f.read_bytes=0;f.max_read=0;f.Sample(api,m);
  assert(f.ints["re4.map.available"]==1 && f.ints["re4.map.native_area"]==1);
  assert(f.max_read<1000);
  f.legend_publishes=0;f.Sample(api,m);assert(f.legend_publishes==cold_count);
  f.host.get_text=nullptr;f.max_read=0;f.Sample(api,m);
  assert(f.max_read<1000 && f.ints["re4.map.room.village-101.state"]==1);
  f.mem.erase(0x85000003);f.Sample(api,m);
  assert(f.ints["re4.map.room.village-101.state"]==0);
  api->destroy(m);++groups;}
 std::cout<<groups<<" synthetic test groups passed\n";
}
