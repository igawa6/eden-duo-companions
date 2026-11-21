// SPDX-License-Identifier: GPL-3.0-or-later
// Uses read-only encrypted records captured from Eden. No game is needed.
#include "za_live.cpp"
#include "za_map.h"
#include "za_travel.h"
#include "za_box_swap.h"
#include "za_native_box.h"
#include "za_move_menu.h"
#include <cassert>
#include <fstream>
#include <map>
#include <iterator>
struct Memory {
 std::map<uint64_t,std::vector<uint8_t>> ranges;
 uint64_t tick=0;
 uint64_t flip_after_read=0;
 void Put(uint64_t address,uint64_t value){auto& b=ranges[address];b.resize(8);std::memcpy(b.data(),&value,8);}
 bool Read(uint64_t at,void* out,size_t size){auto it=ranges.upper_bound(at);if(it==ranges.begin())return false;--it;auto offset=at-it->first;if(offset>it->second.size()||size>it->second.size()-offset)return false;if(out){std::memcpy(out,it->second.data()+offset,size);if(at==flip_after_read&&size>8){it->second[offset+8]^=1;flip_after_read=0;}}return true;}
};
int main(int argc,char** argv){
 if(argc<2)return 77;
 float u=0,v=0;
 // Independent landmark measurements in map-calibration-v7.json (texture pixels).
 assert(za::MapPoint("t1",-344.725525f,-648.544922f,u,v)&&std::abs(u-221.55f)<3&&std::abs(v-410.73f)<3);
 assert(za::MapPoint("t1",-584.487366f,-717.530029f,u,v)&&std::abs(u-372.07f)<3&&std::abs(v-455.24f)<3);
 assert(za::MapPoint("t1",-779.930359f,-900.439331f,u,v)&&std::hypot(u-491.71f,v-566.08f)<4);
 assert(!za::MapPoint("t4_06",-344,-648,u,v));
 assert(!za::MapPoint("t1",10000,-648,u,v));
 assert(!za::MapPoint("t1",NAN,-648,u,v));
 Memory m;auto load=[&](const char* name){std::ifstream f(std::string(argv[1])+"/"+name,std::ios::binary);return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),{});};
 m.ranges[0x50000]=load("live-party.enc");m.ranges[0x60000]=load("live-coordinates.enc");assert(m.ranges[0x50000].size()==0xB45&&m.ranges[0x60000].size()==0x75);
 m.Put(0x100000+0x610A670,0x20000);m.Put(0x20030,0x30000);m.Put(0x30008,0x40000);m.Put(0x30010,0x40040);
 std::vector<uint8_t> descriptors(64);auto put=[&](size_t off,uint64_t v){std::memcpy(descriptors.data()+off,&v,8);};
 put(0,(0xaabbccddull<<32)|za::KParty);put(8,0x50000);put(16,0xB45);put(32,(0x12345678ull<<32)|za::KCoordinates);put(40,0x60000);put(48,0x75);m.ranges[0x40000]=descriptors;
 EdenDsmodHostApi host{};host.main_base=0x100000;host.userdata=&m;uint8_t build[]={0xB1,0xF1,0x2F,0xD9,0x19,0xEA,0xE8,0x6A};std::memcpy(host.build_id,build,8);
 host.is_mapped=[](void* p,uint64_t at,size_t size)->EdenDsmodBool{return static_cast<Memory*>(p)->Read(at,nullptr,size);};
 host.read_memory=[](void* p,uint64_t at,void* out,size_t size)->EdenDsmodBool{return static_cast<Memory*>(p)->Read(at,out,size);};
 // Production pointer reads must never enter the host GPU-synchronizing ReadBlock path.
 auto production=host;
 production.get_read_pointer=[](void* p,uint64_t at,size_t size)->const uint8_t*{auto& ranges=static_cast<Memory*>(p)->ranges;auto it=ranges.upper_bound(at);if(it==ranges.begin())return nullptr;--it;const auto off=at-it->first;if(size>4096-(at&4095)||off>it->second.size()||size>it->second.size()-off)return nullptr;return it->second.data()+off;};
 production.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{assert(false&&"GPU read must not be invoked");return false;};
 auto safe=za::LiveReader{}.Poll(production);assert(safe.party_valid&&safe.party_count==6);
 za::LiveReader reader;auto first=reader.Poll(host);assert(first.party_valid&&first.coordinates_valid&&first.party_count==6);assert(first.party[0].species==716&&first.party[0].hp==318);
 // Native research must update without a save, and reject a recycled service object.
 {
  using za::u64;using za::u32;
  Memory state;auto sh=host;sh.userdata=&state;za::LiveReader reader;
  state.Put(sh.main_base+0x3EC3760,0xA0000);auto& rank=state.ranges[0xA0000];rank.resize(0x48);
  const auto put=[&](auto& b,std::size_t at,const auto& v){std::memcpy(b.data()+at,&v,sizeof(v));};
  put(rank,0,u64(sh.main_base+0x3DA5E80));put(rank,0x40,u32(1));put(rank,0x18,u32(1));put(rank,0x1C,u32(1000));
  auto s=reader.Poll(sh);assert(s.rank_live&&s.trainer_rank==1&&s.ticket_points==1000); // ticket ready is NOT promotion
  put(rank,0x18,u32(20));put(rank,0x1C,u32(19000));s=reader.Poll(sh);assert(s.trainer_rank==20&&s.ticket_points==19000);
  put(rank,0x18,u32(27));assert(reader.Poll(sh).trainer_rank==27);
  put(rank,0x40,u32(0));assert(!reader.Poll(sh).rank_live);put(rank,0x40,u32(1));
  put(rank,0x18,u32(28));assert(!reader.Poll(sh).rank_live);

  state.Put(sh.main_base+0x610A710,0xB0000);state.Put(sh.main_base+0x610A718,0xB0008);state.Put(0xB0000,0xC0000);
  auto& dim=state.ranges[0xC0000];dim.resize(0x40+0x28B8);
  put(dim,0,u64(sh.main_base+0x3DA3308));put(dim,0x10,u64(0xC0000));put(dim,0x40+10000,u32(3));
  for(int i=0;i<3;++i){const auto at=0x40+i*200;std::strcpy(reinterpret_cast<char*>(dim.data()+at+0x40),"t4_01_dim_wild_01");
   std::strcpy(reinterpret_cast<char*>(dim.data()+at+0xA8),i==0?"gate01":i==1?"gate02":"gate03");
   dim[at+0x60]=i==1?1:0;dim[at+0x62]=i==2?0:1;dim[at+0x63]=8;
   put(dim,at+0x8C,-400.f);put(dim,at+0x90,2.f);put(dim,at+0x94,-600.f);}
  s=reader.Poll(sh);assert(s.dimensions_live&&s.dimensions.size()==2&&s.dimensions[0].kind==0&&s.dimensions[1].kind==1);
  dim[0x40+2*200+0x60]=2;
  s=reader.Poll(sh);assert(s.dimensions_live&&s.dimensions.size()==3&&s.dimensions[2].kind==2&&s.dimensions[2].rank==-1);
  dim[0x40+2*200+0x60]=0;
  dim[0x40+0x61]=2;dim[0x40+0x62]=3;dim[0x40+0x64]=2;dim[0x40+0x65]=10;
  dim[0x40+0x66]=7;dim[0x40+0x67]=8;dim[0x40+0x68]=9;
  s=reader.Poll(sh);assert(s.dimensions.size()==2&&s.dimensions[0].rank==2&&s.dimensions[0].types[2]==10&&s.dimensions[0].patterns[1]==8);
  dim[0x40+0x61]=5;assert(!reader.Poll(sh).dimensions_live);dim[0x40+0x61]=2;
  dim[0x40+0x65]=18;assert(!reader.Poll(sh).dimensions_live);dim[0x40+0x65]=10;
  put(dim,0x40+10000,u32(51));assert(!reader.Poll(sh).dimensions_live);put(dim,0x40+10000,u32(3));
  put(dim,0x40+0x8C,float(NAN));assert(!reader.Poll(sh).dimensions_live);put(dim,0x40+0x8C,-400.f);
  state.flip_after_read=0xC0040;assert(!reader.Poll(sh).dimensions_live);

  // World -> typed activation list. A new night changes these names, not all 12 geometries.
  state.Put(sh.main_base+0x3EC71C8,0xD0000);state.Put(0xD0000,0x100001);state.Put(sh.main_base+0x3EC21C8,0xE0000);
  auto& entry=state.ranges[0xE0030];entry.resize(32);put(entry,0,u64(0x100001));put(entry,0x10,u64(0xF0028));
  put(entry,0x18,u32(0x10001));put(entry,0x1C,u32(1));
  auto& world=state.ranges[0xF0000];world.resize(0x100);put(world,0,u64(sh.main_base+0x3D71CC0));put(world,0x10,u64(0xF0000));put(world,0xF8,u64(0x110000));
  auto& zones=state.ranges[0x110000];zones.resize(0x120);put(zones,0,u64(sh.main_base+0x3D83680));put(zones,0x10,u64(0x110000));
  put(zones,0x100,u64(0x120000));put(zones,0x110,u64(1));
  auto& node=state.ranges[0x120000];node.resize(0x48);put(node,8,u64(0x128D7F07BDB7E0B6ull));put(node,0x28,u64(0x130000));put(node,0x30,u64(0x130020));
  auto& names=state.ranges[0x130000];names.resize(32);put(names,0,u64(0x140000));put(names,16,u64(0x150000));
  state.Put(0x140000,0x3648D977A82F0A3Aull);state.Put(0x150000,0xCBF29CE484222645ull);
  s=reader.Poll(sh);assert(s.battle_zones_live&&s.active_battle_zones.size()==1&&s.active_battle_zones[0]==0x3648D977A82F0A3Aull);
  state.Put(0x140000,0xCBF29CE484222645ull);s=reader.Poll(sh);assert(s.battle_zones_live&&s.active_battle_zones.empty());
  put(node,0x30,u64(0x130021));assert(!reader.Poll(sh).battle_zones_live);put(node,0x30,u64(0x130020));
  put(entry,0,u64(0x200001));assert(!reader.Poll(sh).battle_zones_live); // recycled owner
 }
 {
  Memory progress;auto ph=host;ph.userdata=&progress;
  progress.Put(ph.main_base+0x610A710,0x80000);progress.Put(ph.main_base+0x610A718,0x80008);progress.Put(0x80000,0x81000);
  auto& service=progress.ranges[0x81000];service.resize(0x50);
  const auto put=[&](std::size_t at,const auto& v){std::memcpy(service.data()+at,&v,sizeof(v));};
  put(0,std::uint64_t(ph.main_base+0x3DA4D10));put(0x10,std::uint64_t(0x81000));put(0x40,std::uint32_t(41));put(0x44,std::uint32_t(41));put(0x48,std::uint32_t(70300));
  za::LiveReader pr;auto p=pr.Poll(ph);assert(p.research_live&&p.research_level==41&&p.research_points==70300);
  put(0x48,std::uint32_t(70400));assert(pr.Poll(ph).research_points==70400);
  service[0]^=1;assert(!pr.Poll(ph).research_live);
  progress.Put(ph.main_base+0x610A670,0x90000);progress.Put(0x90030,0x91000);
  progress.Put(0x91008,0x92000);progress.Put(0x91010,0x92020);
  auto& descriptor=progress.ranges[0x92000];descriptor.resize(32);
  const std::array<std::uint64_t,4> scalar_desc{0x1D7EE369,0x93000,5,0};
  std::memcpy(descriptor.data(),scalar_desc.data(),32);
  const auto points=[&](std::uint32_t count){auto& bytes=progress.ranges[0x93000];bytes.resize(5);bytes[0]=10;
   std::memcpy(bytes.data()+1,&count,4);za::Cipher cipher(0x1D7EE369);for(auto& byte:bytes)byte^=cipher.Next();};
  points(12500);assert(pr.Poll(ph).infinite_points==12500);
  points(15000);assert(pr.Poll(ph).infinite_points==15000);
 }
 // Modify encrypted HP in each shuffled word until the verified logical HP field is reached.
 // XOR streams preserve the delta; checksum is adjusted for the changed plaintext word.
 const auto original=m.ranges[0x50000];bool found=false;
 for(size_t at=8;at<0x148;at+=2){
  auto& encrypted=m.ranges[0x50000];encrypted=original;
  const uint16_t hp_delta=318^100, new_checksum=uint16_t(first.party[0].checksum-318+100), checksum_delta=first.party[0].checksum^new_checksum;
  encrypted[5+at]^=uint8_t(hp_delta);encrypted[5+at+1]^=uint8_t(hp_delta>>8);
  encrypted[5+6]^=uint8_t(checksum_delta);encrypted[5+7]^=uint8_t(checksum_delta>>8);
  auto changed=reader.Poll(host);if(changed.party_valid&&changed.party[0].hp==100){found=true;break;}
 }
 assert(found);
 // A separate active PA9 overrides the unchanged save snapshot, then updates through cache.
 m.ranges[0x50000]=original;
 auto plain=original;za::Cipher cipher(za::KParty);for(auto& b:plain)b^=cipher.Next();
 m.Put(host.main_base+0x610A710,0x70000);m.Put(0x700B0,0x71000);
 auto& pool=m.ranges[0x1000000]; // use a distinct pool, outside fixture pointer metadata
 m.Put(0x71008,0x1000100);pool.resize((256u<<10)+za::kStoredSize);
 std::copy_n(plain.data()+5,za::kStoredSize,pool.data()+0x100);
 auto active=reader.Poll(host);assert(active.hp_valid[0]&&active.party[0].hp==318);
 const auto original_record=pool;found=false;
 for(size_t at=8;at<za::kStoredSize;at+=2){
  pool=original_record;const uint16_t delta=318^182, checksum=uint16_t(first.party[0].checksum-318+182), cd=first.party[0].checksum^checksum;
  pool[0x100+at]^=uint8_t(delta);pool[0x101+at]^=uint8_t(delta>>8);
  pool[0x106]^=uint8_t(cd);pool[0x107]^=uint8_t(cd>>8);
  reader=za::LiveReader{};auto updated=reader.Poll(host);if(updated.hp_valid[0]&&updated.party[0].hp==182){found=true;break;}
 }
 assert(found);const auto damaged_record=pool;
 pool=original_record;assert(reader.Poll(host).party[0].hp==318);
 pool=damaged_record;assert(reader.Poll(host).party[0].hp==182);
 pool[0x108]^=1;auto torn=reader.Poll(host);assert(!torn.hp_valid[0]&&torn.party[0].hp==182);
 uint16_t confirmed=0;assert(reader.LastHP(first.party[0],confirmed)&&confirmed==182);
 auto other=first.party[0];++other.pid;assert(!reader.LastHP(other,confirmed));
 // A readable PA9 page must not be lost because the rest of its scan chunk is unmapped.
 pool=damaged_record;pool.resize(4096);reader=za::LiveReader{};auto sparse=reader.Poll(host);assert(sparse.hp_valid[0]&&sparse.party[0].hp==182);
 // Android allocation order can place a party record before the allocator anchor.
 pool.assign(4096,0);m.ranges[0xFC0000]=damaged_record;m.ranges[0xFC0000].resize(4096);
 reader=za::LiveReader{};assert(!reader.Poll(host).hp_valid[0]);auto preceding=reader.Poll(host);assert(preceding.hp_valid[0]&&preceding.party[0].hp==182);
 pool=damaged_record;pool.resize(4096);m.ranges.erase(0xFC0000);
 // Runtime registry slots differ across boots; discover by structure and validate every read.
 auto& registry=m.ranges[host.main_base+0x40F0000];registry.resize(0x20000);
 uint64_t object=0x1100000000;std::memcpy(registry.data()+0x12340,&object,8);
 auto& pos=m.ranges[object];pos.resize(80);uint64_t vt=host.main_base+0x3DA4348;std::memcpy(pos.data(),&vt,8);pos[0x18]='t';pos[0x19]='1';
 float x=-345,z=-649;std::memcpy(pos.data()+0x38,&x,4);std::memcpy(pos.data()+0x40,&z,4);
 reader=za::LiveReader{};auto located=reader.Poll(host);assert(located.position_live&&located.x==x&&located.z==z);
 x=-330;std::memcpy(pos.data()+0x38,&x,4);auto moved=reader.Poll(host);assert(moved.position_live&&moved.x==x);
 pos[0]^=1;assert(!reader.Poll(host).position_live);
 auto& huds=m.ranges[host.main_base+0x4800000];huds.resize(0x100000);uint64_t hud=0x1101000000;std::memcpy(huds.data()+0x95000,&hud,8);
 m.Put(hud,host.main_base+0x3DE1A20);m.Put(hud+0xB8,0x1102000000);m.Put(0x1102000368,0x1103000000);
 auto& sel=m.ranges[0x1103000000];sel.resize(16);vt=host.main_base+0x3BD3B30;std::memcpy(sel.data(),&vt,8);sel[8]=4;sel[12]=6;
 reader=za::LiveReader{};assert(reader.Poll(host).selected_index==4);sel[8]=5;assert(reader.Poll(host).selected_index==5);
 sel[12]=1;assert(reader.Poll(host).selected_index==-1);
 // NCE heap addresses lie outside the former Windows-only 0x10..0x18 window.
 m.ranges.erase(host.main_base+0x40F0000);m.ranges.erase(host.main_base+0x4800000);
 const uint64_t nce_root=0x2180010000ull,nce_hud=0x2180020000ull;
 host.get_heap_begin=[](void*)->uint64_t{return 0x2180000000ull;};
 host.get_heap_end=[](void*)->uint64_t{return 0x2190000000ull;};
 auto& bss=m.ranges[host.main_base+0x610B000];bss.resize(0x1000);std::memcpy(bss.data()+0x700,&nce_root,8);std::memcpy(bss.data()+0x708,&nce_hud,8);
 auto& embedded=m.ranges[nce_root];embedded.resize(512);std::copy(pos.begin(),pos.end(),embedded.begin()+0xA8);vt=host.main_base+0x3DA4348;std::memcpy(embedded.data()+0xA8,&vt,8);
 auto& embedded_hud=m.ranges[nce_hud];embedded_hud.resize(512);vt=host.main_base+0x3DE1A20;std::memcpy(embedded_hud.data()+0xE8,&vt,8);uint64_t view=0x1102000000;std::memcpy(embedded_hud.data()+0xE8+0xB8,&view,8);sel[12]=6;sel[8]=3;
 reader=za::LiveReader{};auto nce=reader.Poll(host);assert(nce.position_live&&nce.x==x&&nce.selected_index==3);
 // Script objects may live in a mapped alias outside the process's regular heap.
 host.get_heap_begin=[](void*)->uint64_t{return 0x1080000000ull;};host.get_heap_end=[](void*)->uint64_t{return 0x1100000000ull;};
 reader=za::LiveReader{};auto alias=reader.Poll(host);assert(alias.position_live&&alias.selected_index==3);
 // The map presenter has explicit closed/map/destinations states and a real destination ID.
 const uint64_t ui=0x2181000000ull,map=ui+0x180;
 m.Put(host.main_base+0x610A000,0x1104000000);m.Put(0x11040005F8,0x1105000000);m.Put(0x11050006A8,0x1106000000);
 m.Put(0x1106000000,host.main_base+0x3BCE178);m.Put(0x1106000038,ui);
 auto& ui_bytes=m.ranges[ui];ui_bytes.resize(0x1000);vt=host.main_base+0x3DF1D48;std::memcpy(ui_bytes.data()+0x180,&vt,8);std::memcpy(ui_bytes.data()+0x190,&map,8);ui_bytes[0x180+0x208]=2;ui_bytes[0x180+0x2A8]=105;ui_bytes[0x180+0x585]=1;ui_bytes[0x180+0x448]=9;
 auto put_map = [&](std::size_t offset, auto value) {
     std::memcpy(ui_bytes.data() + offset, &value, sizeof(value));
 };
 put_map(0x1d0, ui + 0x740); put_map(0x1d8, ui + 0x760);
 put_map(0x740, ui + 0x800); put_map(0x748, ui + 0x800 + 4 * 0x90);
 put_map(0x800, std::uint8_t{1}); put_map(0x804, 4u); put_map(0x828, 53u);
 put_map(0x808, 562.3772f); put_map(0x80c, 1327.8998f);
 put_map(0x890, std::uint8_t{1}); put_map(0x894, 9u); put_map(0x8b8, 61u);
 put_map(0x920, std::uint8_t{0}); put_map(0x924, 4u); put_map(0x948, 35u);
 put_map(0x9b0, std::uint8_t{1}); put_map(0x9b4, 2u); put_map(0x9d8, 42u);
 put_map(0x9b8, -650.f); put_map(0x9bc, 1494.f);
 reader=za::LiveReader{};auto mapped=reader.Poll(host);assert(mapped.map_mode==2&&mapped.map_destination==105);
 assert(mapped.quest_pins.size() == 2);
 assert(mapped.quest_pins[0].category == 2 && mapped.quest_pins[0].id == 53);
 assert(mapped.quest_pins[0].x == 562.3772f && mapped.quest_pins[0].y == 1327.8998f);
 assert(mapped.quest_pins[1].category == 0 && mapped.quest_pins[1].id == 42);
 const uint64_t travel_catalog=ui+0x3000, travel_entries=ui+0x4000;
 put_map(0x628,travel_catalog);
 m.Put(travel_catalog+0x120,travel_entries);
 m.Put(travel_catalog+0x160,62);
 m.Put(travel_entries+61*32,0x4321);
 m.Put(travel_entries+61*32+0x10,ui+0x9000);
 assert(mapped.unlocked_travel.empty());
 reader=za::LiveReader{};assert(reader.Poll(host).unlocked_travel.empty());
 put_map(0x890,std::uint8_t{3});
 reader=za::LiveReader{};const auto unlocked=reader.Poll(host).unlocked_travel;
 assert(unlocked.size()==1&&unlocked[0].kind==9&&unlocked[0].row==61&&unlocked[0].key==0x4321);
 m.Put(travel_entries+61*32+0x10,0);
 reader=za::LiveReader{};assert(reader.Poll(host).unlocked_travel.empty());
 put_map(0x804,5u);
 put_map(0x1d8,ui+0x780);
 put_map(0x760,ui+0xc00);put_map(0x768,ui+0xc90);
 put_map(0xc00,std::uint8_t{2});put_map(0xc04,1u);
 put_map(0xc08,250.f);put_map(0xc0c,-125.f);
 reader=za::LiveReader{};auto native_pin=reader.Poll(host);
 assert(native_pin.quest_pins[0].category==2&&native_pin.quest_pins[0].id==53);
 assert(native_pin.map_marker.known&&native_pin.map_marker.visible&&native_pin.map_marker.x==250.f&&native_pin.map_marker.y==-125.f);
 assert(native_pin.quest_map_loaded);
 ui_bytes[0x180+0x585]=0;
 put_map(0x800,uint8_t{0});put_map(0x9b0,uint8_t{0});
 const auto closed=reader.Poll(host);
 assert(closed.quest_map_loaded && closed.quest_pins.size()==2);
 ui_bytes[0x180+0x585]=1;
 put_map(0x800,uint8_t{1});put_map(0x9b0,uint8_t{1});
 put_map(0xc00,std::uint8_t{0});reader=za::LiveReader{};
 assert(!reader.Poll(host).map_marker.visible);
 put_map(0x1d8,ui+0x760);
 ui_bytes[0x180+0x208]=3;assert(reader.Poll(host).map_mode==3);
 ui_bytes[0x180+0x208]=0;ui_bytes[0x180+0x585]=0;ui_bytes[0x180+0x448]=32;assert(reader.Poll(host).map_mode==3);ui_bytes[0x180+0x208]=2;assert(reader.Poll(host).map_mode==4);ui_bytes[0x180+0x448]=9;assert(reader.Poll(host).map_mode==4);ui_bytes[0x180+0x585]=1;ui_bytes[0x180+0x448]=9;ui_bytes[0x180+0x208]=0;assert(reader.Poll(host).map_mode==0);
 ui_bytes[0x180+0x208]=255;assert(reader.Poll(host).map_mode==-1);
 // A presenter created after the first sweep must be retried; idle ticks cannot
 // keep postponing its retry deadline forever.
 host.get_tick=[](void* p)->uint64_t{return static_cast<Memory*>(p)->tick;};
 reader=za::LiveReader{};ui_bytes[0x180]^=1;
 for(int i=0;i<32;++i)reader.Poll(host);
 m.tick=100;reader.Poll(host);ui_bytes[0x180]^=1;ui_bytes[0x180+0x208]=2;
 m.tick=301;assert(reader.Poll(host).map_mode==2);
 using C=za::TravelController::Command;za::TravelController travel;
 travel.Start(105,0);assert(travel.Poll(0,3,0)==C::OpenMap);assert(travel.Poll(90,0,0)==C::Destinations);
 assert(travel.Poll(150,2,104)==C::Down);assert(travel.Poll(174,2,105)==C::None);assert(travel.Poll(186,2,105)==C::Confirm&&travel.Active());assert(travel.Poll(246,4,105)==C::Confirm&&!travel.Active());
 travel.Start(105,0);assert(travel.Poll(0,2,105)==C::None);assert(travel.Poll(12,2,105)==C::Confirm);assert(travel.Poll(72,3,105)==C::None&&!travel.Active());
 travel.Start(105,0);assert(travel.Poll(0,2,105)==C::None);assert(travel.Poll(12,2,105)==C::Confirm);assert(travel.Poll(72,4,104)==C::None&&!travel.Active());
 travel.Start(105,0);assert(travel.Poll(0,4,105)==C::OpenMap);assert(travel.Poll(90,4,105)==C::None&&!travel.Active());
 travel.Start(105,0);assert(travel.Poll(0,0,0)==C::OpenMap);assert(travel.Poll(90,3,0)==C::OpenMap);assert(travel.Poll(180,0,0)==C::Destinations);assert(travel.Poll(240,2,0)==C::Down);assert(travel.Poll(264,2,102)==C::Down);
 travel.Start(205,0);assert(travel.Poll(0,2,204)==C::Down);assert(travel.Poll(24,2,206)==C::None&&!travel.Active());
 travel.Start(105,0);travel.Cancel();assert(travel.Poll(100,2,105)==C::None&&!travel.Active());
 travel.Start(105,0);assert(travel.Poll(0,1,105)==C::None);assert(travel.Poll(2401,2,105)==C::None&&!travel.Active());
 // Box exchange is a readback-driven state machine: no A before a proven pickup.
 {
  using B=za::BoxSwapController::Command;za::BoxSwapController swap;za::LiveSnapshot state;
  za::Entity source,victim;source.encryption_constant=11;source.pid=12;victim.encryption_constant=21;victim.pid=22;
  state.party_valid=state.box_valid=true;state.box_view_index=1;state.box_view[13]=source;state.party[0]=victim;state.boxes.valid=true;state.boxes.cursor=13;
  swap.Start(1,13,0,source,victim,0);assert(swap.Poll(0,state)==B::Right);
  // A closed/inactive box keeps its cached cursor: focus proof must refuse it.
  assert(swap.Poll(22,state)==B::None&&!swap.Active());
  swap.Start(1,13,0,source,victim,0);assert(swap.Poll(0,state)==B::Right);state.boxes.cursor=14;
  assert(swap.Poll(22,state)==B::None);assert(swap.Poll(23,state)==B::None);assert(swap.Poll(24,state)==B::Left);
  state.boxes.cursor=13;assert(swap.Poll(46,state)==B::Pick);assert(swap.Poll(68,state)==B::Right);
  state.boxes={true,14,0,1,13,1,14};assert(swap.Poll(90,state)==B::None);
  assert(swap.Poll(91,state)==B::Up);state.boxes.cursor=8;state.boxes.target_slot=8;assert(swap.Poll(113,state)==B::Up);
  state.boxes.cursor=2;state.boxes.target_slot=2;assert(swap.Poll(135,state)==B::Up);
  state.boxes.target_box=998;state.boxes.target_slot=state.boxes.party_cursor=2;assert(swap.Poll(157,state)==B::Left);
  state.boxes.target_slot=state.boxes.party_cursor=1;assert(swap.Poll(179,state)==B::Left);
  state.boxes.target_slot=state.boxes.party_cursor=0;assert(swap.Poll(201,state)==B::None);assert(swap.Poll(213,state)==B::Confirm);
  state.party[0]=source;state.box_view[13]={};state.boxes.source_box=998;state.boxes.source_slot=0;
  assert(swap.Poll(253,state)==B::None);assert(swap.Poll(254,state)==B::Down);
  state.boxes.target_box=1;state.boxes.cursor=state.boxes.target_slot=0;assert(swap.Poll(276,state)==B::Down);
  state.boxes.cursor=state.boxes.target_slot=6;assert(swap.Poll(298,state)==B::Down);
  state.boxes.cursor=state.boxes.target_slot=12;assert(swap.Poll(320,state)==B::Right);
  state.boxes.cursor=state.boxes.target_slot=13;assert(swap.Poll(342,state)==B::None);assert(swap.Poll(354,state)==B::Confirm);
  state.boxes={true,13,0,0,0,0,0};state.box_view[13]=victim;assert(swap.Poll(394,state)==B::None&&swap.Completed()&&!swap.Active());
  state.party[0]=victim;state.box_view[13]=source;
  swap.Start(1,13,0,source,victim,0);assert(swap.Poll(0,state)==B::Right);state.boxes.cursor=14;swap.Poll(22,state);swap.Poll(23,state);swap.Poll(24,state);
  state.boxes.cursor=13;swap.Poll(46,state);swap.Poll(68,state);state.boxes.cursor=14;
  assert(swap.Poll(90,state)==B::Back&&!swap.Active()); // an uncertain Y is cancelled, never repeated
  swap.Cancel();assert(swap.Poll(200,state)==B::None&&!swap.Active());
 }
 // The current box is a checksum-validated live SCBlock, not a save-file rewrite.
 {
  Memory bm=m;auto bh=host;bh.userdata=&bm;bm.tick=500;bm.ranges[0x50000]=original;
  std::vector<uint8_t> bd(96);auto set=[&](size_t off,uint64_t value){std::memcpy(bd.data()+off,&value,8);};
  const uint32_t box_size=za::kBoxCount*za::kBoxSlotCount*za::kBoxSlotSize;
  std::vector<uint8_t> enc(box_size+5);enc[0]=4;std::memcpy(enc.data()+1,&box_size,4);
  std::copy_n(plain.data()+5,za::kPartySize,enc.data()+5+(2*30+13)*za::kBoxSlotSize);
  za::Cipher bc(za::KBox);for(auto& v:enc)v^=bc.Next();bm.ranges[0x1800000]=enc;
  set(0,za::KBox);set(8,0x1800000);set(16,enc.size());set(32,za::KParty);set(40,0x50000);set(48,0xB45);set(64,za::KCoordinates);set(72,0x60000);set(80,0x75);
  bm.ranges[0x40000]=bd;bm.Put(0x30010,0x40060);
  auto& ub=bm.ranges[ui];ub.assign(0x5000,0);auto put=[&](size_t off,uint64_t value){std::memcpy(ub.data()+off,&value,8);};
  put(0x180,bh.main_base+0x3DB4880);put(0x190,ui+0x180);put(0x180+0xD0,ui+0x1000);
  put(0x1048,ui+0x1200);put(0x1378,ui+0x1400);put(0x15A0,ui+0x1600);
  put(0x1600,bh.main_base+0x3DB4D18);put(0x1610,ui+0x1600);put(0x16C8,(13ull<<32)|2);put(0x16D0,(1ull<<32)|998);
  put(0x180+0xE0,ui+0x2000);put(0x180+0xE8,ui+0x2800);
  put(0x2000,bh.main_base+0x3BD0E60);put(0x2368,ui+0x2400);put(0x2400,bh.main_base+0x3BD3B30);put(0x2408,(6ull<<32)|1);
  put(0x2800,bh.main_base+0x3BD0E60);put(0x2B68,ui+0x2C00);put(0x2C00,bh.main_base+0x3BD3B30);put(0x2C08,(6ull<<32)|1);
  za::LiveReader boxes;auto result=boxes.Poll(bh,true,2);
  assert(result.box_valid&&result.box_view_index==2&&result.box_view[13].species==716);
  assert(result.boxes.valid&&result.boxes.source_box==2&&result.boxes.source_slot==13&&result.boxes.target_box==998&&result.boxes.party_cursor==1);
  // A new session can place BoxPane 720 MiB beyond the UI allocator's start.
  const uint64_t late=ui+(720ull<<20)+0x180;bm.ranges[late-0x180]=std::vector<uint8_t>(4096,0);
  std::copy_n(ub.data()+0x180,0x100,bm.ranges[late-0x180].data()+0x180);
  std::memcpy(bm.ranges[late-0x180].data()+0x190,&late,8);put(0x180,0);za::LiveReader relocated;
  for(int n=0;n<190;++n){++bm.tick;result=relocated.Poll(bh,true,2);if(result.boxes.valid)break;}
  assert(result.boxes.valid&&result.boxes.source_box==2);

 }
 // Active boxes use the native service rather than the stale save SCBlock.
 {
  Memory bm=m;auto bh=host;bh.userdata=&bm;
  const uint64_t service=0x2500000,data=0x2600000,vector=0x2700000;
  bm.Put(bh.main_base+0x610A710,vector);bm.Put(bh.main_base+0x610A718,vector+8);bm.Put(vector,service);
  bm.Put(service,bh.main_base+0x3D59CF0);bm.Put(service+0x10,service);bm.Put(service+0x978,data);bm.Put(service+0x980,32);
  auto& mask=bm.ranges[service+0x40];mask.resize(120);mask[60/8]|=1<<(60%8);
  auto& bytes=bm.ranges[data+2*30*za::kBoxSlotSize];bytes.resize(30*za::kBoxSlotSize);
  std::copy_n(plain.data()+5,za::kPartySize,bytes.data());
  std::array<za::Entity,30> rows{};
  assert(za::BoxService(bh)==service&&za::WorkingBoxAt(bh,service,2,rows)&&rows[0].species==716);
  // Raw bytes remain in removed cells: occupancy, not EC, decides presence.
  mask[60/8]=0;assert(za::WorkingBoxAt(bh,service,2,rows)&&!rows[0].present);
  mask[60/8]|=1<<(60%8);
  std::copy_n(plain.data()+5+za::kPartySlotSize,za::kPartySize,bytes.data());
  assert(za::WorkingBoxAt(bh,service,2,rows)&&rows[0].species==398);
  // An explicitly flagged, checksum-consistent Bad Egg is read-only; it
  // cannot hide the rest of the box or become a native exchange source.
  bytes[4]=4;
  mask[61/8]|=1<<(61%8);
  std::copy_n(plain.data()+5,za::kPartySize,bytes.data()+za::kBoxSlotSize);
  assert(za::WorkingBoxAt(bh,service,2,rows)&&rows[0].present&&rows[0].egg&&!rows[0].checksum_ok&&
         rows[1].present&&rows[1].checksum_ok&&rows[1].species==716);
  za::NativeBoxExchange flagged;za::LiveSnapshot flagged_state;
  flagged_state.box_live=true;flagged_state.box_view_index=2;flagged_state.box_view=rows;
  assert(!flagged.Prepare(flagged_state,2,0,rows[0]));
  assert(flagged.Prepare(flagged_state,2,1,rows[1]));
  mask[61/8]&=~(1<<(61%8));
  bytes[8]^=1;assert(!za::WorkingBoxAt(bh,service,2,rows));bytes[8]^=1;
  bytes[4]=8;assert(!za::WorkingBoxAt(bh,service,2,rows));bytes[4]=0;
  bytes[8]^=1;assert(!za::WorkingBoxAt(bh,service,2,rows));bytes[8]^=1;
  bm.flip_after_read=data+2*30*za::kBoxSlotSize;
  assert(!za::WorkingBoxAt(bh,service,2,rows));bytes[8]^=1;
  bm.Put(service+0x980,33);assert(!za::WorkingBoxAt(bh,service,2,rows));bm.Put(service+0x980,32);
  assert(!za::WorkingBoxAt(bh,service,-1,rows)&&!za::WorkingBoxAt(bh,service,32,rows));
  bh.get_text=[](void*,const char* key)->const char*{return std::strcmp(key,"__page")==0?"box":"";};
  za::LiveReader boxes;auto active=boxes.Poll(bh,false,2);
  assert(active.box_valid&&active.box_live&&active.box_view[0].species==398);
  // Preflight closes on stale identity, deployed partner, or a transition. A
  // successful readback closes the gate, preventing a repeated inverse swap.
  auto state=active;state.party[0]=first.party[0];state.party_current=state.scene_idle=state.position_live=true;
  state.area="t1";state.deployed_index=-1;za::NativeBoxExchange swap;
  assert(!swap.Prepare(state,2,30,state.box_view[0]));
  assert(swap.Prepare(state,2,0,state.box_view[0])&&swap.Pick(state,0)&&swap.Ready(state));
  state.deployed_index=0;assert(!swap.Ready(state));state.deployed_index=-1;
  state.scene_idle=false;assert(!swap.Ready(state));state.scene_idle=true;
  state.party_current=false;assert(!swap.Ready(state));state.party_current=true;
  ++state.box_view[0].pid;assert(!swap.Ready(state));--state.box_view[0].pid;
  std::swap(state.party[0],state.box_view[0]);swap.Observe(state);
  assert(swap.completed&&!swap.Ready(state));
  // Selecting a victim again cannot re-arm an already completed operation.
  assert(!swap.Pick(state,0)&&!swap.Ready(state));
 }
 // Active party membership/order changes without any change to the SCBlock save.
 {
  Memory bm=m;auto bh=production;bh.userdata=&bm;bm.tick=1000;bm.ranges[0x50000]=original;
  bm.Put(bh.main_base+0x610A710,0x200000);bm.Put(0x200018,0x210000);bm.Put(0x2101B0,0x220000);
  std::array<uint64_t,6> nodes{};
  for(int n=0;n<6;++n){const uint64_t node=0x300000+n*0x1000,wrapper=0x400000+n*0x1000,data=0x500000+n*0x1000;
   nodes[n]=node;bm.Put(node,bh.main_base+0x3e2a438);bm.Put(node+0x10,node);bm.Put(node+0x50,wrapper);bm.Put(wrapper+0x10,wrapper);
   std::vector<uint8_t> d(64);auto put=[&](int off,uint64_t value){std::memcpy(d.data()+off,&value,8);};put(0,data);put(16,za::kStoredSize);put(32,data+0x200);put(48,16);bm.ranges[wrapper+0x30]=d;
   const auto start=plain.data()+5+n*za::kPartySlotSize;
   bm.ranges[data]=std::vector<uint8_t>(start,start+za::kStoredSize);bm.ranges[data+0x200]=std::vector<uint8_t>(start+za::kStoredSize,start+za::kPartySize);
  }
  auto publish=[&]{auto& out=bm.ranges[0x2200F0];out.resize(sizeof(nodes));std::memcpy(out.data(),nodes.data(),sizeof(nodes));};publish();
  za::LiveReader realtime;auto before=realtime.Poll(bh);assert(before.party_live&&before.party_count==6&&before.hp_valid[0]);
  auto set_cooldown=[&](float left){
   const std::array<float,8> timers{left,0,0,0,8,0,0,0};
   auto& data=bm.ranges[nodes[0]+0x64];data.resize(sizeof(timers));
   std::memcpy(data.data(),timers.data(),sizeof(timers));
  };
  set_cooldown(5.5f);
  auto in_combat=realtime.Poll(bh);assert(in_combat.recharge[0][0].valid&&in_combat.recharge[0][0].remaining==5.5f);
  set_cooldown(2.f);
  auto cooling=realtime.Poll(bh);assert(cooling.recharge[0][0].remaining==2.f&&cooling.recharge[0][0].initial==8.f);
  set_cooldown(12.f);
  assert(!realtime.Poll(bh).recharge[0][0].valid);
  std::swap(nodes[0],nodes[1]);publish();++bm.tick;auto after=realtime.Poll(bh);
  assert(after.party_live&&after.party[0].pid==before.party[1].pid&&after.party[1].pid==before.party[0].pid&&bm.ranges[0x50000]==original);
  // A different startup order invalidates the old allocation-dependent chain.
  Memory relocated=bm;
  auto moved_host=bh;moved_host.userdata=&relocated;
  for(auto it=relocated.ranges.lower_bound(0x200000);it!=relocated.ranges.end()&&it->first<0x300000;)
      it=relocated.ranges.erase(it);
  auto& allocation=relocated.ranges[0x200000];allocation.resize(1<<20);
  const auto write=[&](std::size_t offset,const auto& value){
      std::memcpy(allocation.data()+offset,&value,sizeof(value));
  };
  write(0x18,uint64_t{0x210000});
  write(0x101b0,uint64_t{0x101});
  write(0x80000,uint64_t{bh.main_base+0x3eb9a30});
  write(0x800f0,nodes);
  za::LiveReader rediscovered;
  auto moved_party=rediscovered.Poll(moved_host);
  assert(moved_party.party_live&&moved_party.party_count==6&&
         moved_party.party[0].pid==after.party[0].pid);
  auto cached_party=rediscovered.Poll(moved_host);
  assert(cached_party.party_live&&cached_party.party[0].pid==moved_party.party[0].pid);

  // A generation-checked native context outranks other valid old party buffers.
  const uint64_t manager=0x30000000,context=0x30001000,actor=0x30002000,
      components=0x30005000,party=0x30006000,storage=0x30007000,registry=0x31000000;
  auto typed=[&](uint64_t a,uint64_t type){bm.Put(a,bh.main_base+type);bm.Put(a+0x10,a);};
  typed(manager,0x3D2B8C8);typed(context,0x3D2C490);typed(components,0x3D2A770);typed(storage,0x3E2D1D0);
  bm.Put(bh.main_base+0x610B810,manager);bm.Put(manager+0x48,context);bm.Put(bh.main_base+0x3EC21C8,registry);
  auto ref=[&](uint64_t at,uint64_t key,uint64_t object,uint64_t type){
      typed(object,type);bm.Put(at,key);auto& entry=bm.ranges[registry+(key&0xFFFFF)*32+0x10];entry.assign(32,0);
      std::memcpy(entry.data(),&key,8);const auto face=object+0x28;std::memcpy(entry.data()+0x10,&face,8);
      entry[0x18]=entry[0x1A]=1;entry[0x1C]=0xFE;entry[0x1D]=entry[0x1E]=entry[0x1F]=0xFF;
  };
  ref(context+0x40,0x100123,actor,0x3D2A8E8);bm.Put(actor+0x2038,components);
  ref(components+0x100,0x200234,party,0x3D81450);bm.Put(party+0x190,storage);
  // The native component has several interface vtables, not a handle at +30.
  bm.Put(party+0x30,bh.main_base+0x3D81748);
  // Independent vectors must work even when far apart and ordered differently
  // from a checksum-valid old buffer found by the legacy heap scan.
  bm.ranges[0x33000000]=bm.ranges[0x2200F0];
  std::array<uint64_t,6> actors{};
  for(int i=0;i<6;++i){
      actors[i]=0x35000000+i*0x1000;typed(actors[i],0x3BE0D28);
      bm.ranges[actors[i]+0x3AB]={0};
  }
  auto& actor_bytes=bm.ranges[0x34000000];actor_bytes.resize(sizeof(actors));
  std::memcpy(actor_bytes.data(),actors.data(),sizeof(actors));
  for(auto address:{0x33000000ull,0x34000000ull}){
      auto& bytes=bm.ranges[address];assert(bytes.size()==48);
      for(int i=0;i<8;++i)std::swap(bytes[i],bytes[i+8]);
  }
  std::array<uint64_t,6> battle_handles{};
  const auto reordered_nodes=bm.ranges[0x33000000];
  for(int i=0;i<6;++i){
      uint64_t node=0;std::memcpy(&node,reordered_nodes.data()+i*8,8);
      battle_handles[i]=0x400300+i;ref(0x33000000+i*8,battle_handles[i],node,0x3E2A438);
      bm.ranges.erase(0x33000000+i*8);
  }
  auto& handle_bytes=bm.ranges[0x33000000];handle_bytes.resize(sizeof(battle_handles));
  std::memcpy(handle_bytes.data(),battle_handles.data(),sizeof(battle_handles));
  bm.Put(storage+0x70,0x34000000);bm.Put(storage+0x78,6);
  bm.Put(storage+0x98,0x33000000);bm.Put(storage+0xA0,6);
  assert(za::NativePartyStorage(bh)==storage);
  auto authoritative=za::LiveReader{}.Poll(bh);assert(authoritative.party_current&&authoritative.party_count==6&&authoritative.party[0].pid!=after.party[0].pid);
  assert(authoritative.party_nodes[0]&&authoritative.party_nodes[1]);
  auto move_state=authoritative;move_state.position_live=move_state.scene_idle=true;move_state.area="t1";move_state.deployed_index=-1;
  za::MoveMenu menu;menu.opened=1;menu.entity=move_state.party[0];menu.party=0;menu.slot=0;
  menu.desired=2047;menu.choices={{2047,1,0}};
  assert(menu.Ready(move_state));move_state.party_nodes[0]=0;assert(!menu.Ready(move_state));move_state=authoritative;
  move_state.position_live=move_state.scene_idle=true;move_state.area="t1";move_state.deployed_index=-1;
  menu.from_box=true;assert(!menu.Ready(move_state));menu.from_box=false;
  move_state.deployed_index=0;assert(!menu.Ready(move_state));move_state.deployed_index=-1;
  ++move_state.party[0].pid;assert(!menu.Ready(move_state));--move_state.party[0].pid;
  move_state.party[0].moves[0]=menu.desired;assert(!menu.Ready(move_state));
  move_state.party[0].moves[0]=0;move_state.party[0].moves[1]=menu.desired;assert(!menu.Ready(move_state));
  bm.Put(storage+0x78,3);bm.Put(storage+0xA0,3);
  auto partial=za::LiveReader{}.Poll(bh);assert(partial.party_current&&partial.party_count==3);
  bm.Put(storage+0x78,6);bm.Put(storage+0xA0,6);
  handle_bytes[2]^=0x10;assert(!za::LiveReader{}.Poll(bh).party_current);handle_bytes[2]^=0x10;
  // A recycled registry index is not the same generation; cached UI data may
  // remain visible but must not authorize a mutating call.
  bm.Put(components+0x100,0x300234);assert(!za::NativePartyStorage(bh));
  auto stale_party=za::LiveReader{}.Poll(bh);assert((!stale_party.party_current&&stale_party.party_nodes==std::array<uint64_t,6>{}));bm.Put(components+0x100,0x200234);
  bm.Put(party,bh.main_base+0x1234);assert(!za::NativePartyStorage(bh));typed(party,0x3D81450);
  bm.Put(storage+0x78,7);assert(!za::NativePartyStorage(bh));bm.Put(storage+0x78,6);

  bm.ranges[0x501000][8]^=1;++bm.tick;auto transient=realtime.Poll(bh);
  assert(transient.party_live&&transient.party[0].pid==after.party[0].pid); // never revert to saved order on a torn read
 }
 m.ranges[0x50000][5+8]^=1;assert(!reader.Poll(host).party_valid);
 {auto& signature=m.ranges[host.main_base+0x3da4348];signature.resize(24);uint64_t values[]={host.main_base+0x2add89c,host.main_base+0x269fe70,host.main_base+0x269fe7c};std::memcpy(signature.data(),values,24);}
 {auto& signature=m.ranges[host.main_base+0x3de1a20];signature.resize(24);uint64_t values[]={host.main_base+0x2c05980,host.main_base+0x269fe70,host.main_base+0x269fe7c};std::memcpy(signature.data(),values,24);}
 {auto& signature=m.ranges[host.main_base+0x3bd3b30];signature.resize(24);uint64_t values[]={host.main_base+0xfe6214,host.main_base+0xfe6290,host.main_base+0xfe56f4};std::memcpy(signature.data(),values,24);}
 // Validate the program itself when the host reports an old build ID.
 host.build_id[0]=0;assert(reader.Poll(host).selected_index==3);
 m.Put(host.main_base+0x3BD3B30,0x12345678);assert(reader.Poll(host).selected_index==-1);

 host.build_id[0]=build[0];m.ranges.erase(0x40000);assert(!reader.Poll(host).party_valid);
 std::puts("live reader: valid data, fresh HP, corruption, unsupported build, unmapped memory OK");
}
