// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon_icons.h"
#include "echoes_code_guard.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>
namespace echoes { namespace {
struct Guard { uint64_t at; std::array<uint8_t,32> bytes; };
constexpr Guard Guards[]={
 {0x2601c98,{0xf3,0x03,0x00,0xaa,0x20,0x00,0x80,0x52,0x68,0x1a,0x44,0xb9,0x60,0x22,0x04,0xb9,0x28,0x08,0x00,0x34,0x08,0x10,0x3c,0x1e,0xf4,0x03,0x01,0xaa,0xf6,0x03,0x1f,0xaa}},
 {0x26460dc,{0x3f,0x69,0x28,0x38,0x98,0x01,0x00,0x94,0x09,0xa0,0x45,0xa9,0x69,0x22,0x00,0xa9,0xc8,0x00,0x00,0xb4,0x08,0x21,0x00,0x91,0x09,0x7d,0x5f,0xc8,0x29,0x05,0x00,0x91}},
 {0x2646870,{0x8a,0x06,0x40,0xf9,0x5f,0x01,0x17,0xeb,0x41,0x02,0x00,0x54,0x8a,0x2a,0x40,0xf9,0x5f,0x01,0x08,0xeb,0x21,0xff,0xff,0x54,0xea,0x03,0x1f,0xaa,0x1f,0x20,0x03,0xd5}},
 {0x2646740,{0xfd,0x7b,0xbb,0xa9,0xf9,0x0b,0x00,0xf9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9,0xf4,0x4f,0x04,0xa9,0xfd,0x03,0x00,0x91,0x28,0x20,0x40,0xf9,0xb7,0x64,0x84,0xd2}},
 {0x26462ac,{0xa9,0xc6,0x47,0xf9,0x29,0x79,0x79,0xf8,0x49,0x04,0x00,0xb4,0x33,0x01,0x40,0xf9,0x13,0x04,0x00,0xb4,0x1f,0x09,0x00,0xf1,0x02,0x01,0x00,0x54,0x08,0x07,0x00,0xd1}},
 {0x2646220,{0xfd,0x7b,0xbb,0xa9,0xfa,0x67,0x01,0xa9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9,0xf4,0x4f,0x04,0xa9,0xfd,0x03,0x00,0x91,0x17,0x74,0x00,0xd0,0xf4,0x03,0x01,0xaa}},
 {0x25f3e6c,{0x68,0xf6,0x40,0xf9,0x68,0x0a,0x00,0xb4,0x60,0xf2,0x40,0xf9,0x03,0xd1,0x69,0x97,0x68,0xf6,0x40,0xf9,0x09,0x0c,0x40,0xf9,0x08,0x05,0x40,0xf9,0x28,0x05,0x00,0xf9}},
 {0x25f3dd4,{0xe8,0x33,0x40,0xf9,0x21,0x02,0x80,0x52,0xa0,0xa3,0x00,0xd1,0xa2,0xe3,0x00,0xd1,0xb5,0xa3,0x3c,0xa9,0x6a,0x48,0x01,0x94,0xa8,0xa7,0x7d,0xa9,0x75,0xfa,0x40,0xf9}},
 {0x260042c,{0x69,0x1e,0x44,0xb9,0x29,0x7d,0x89,0x0a,0x68,0x1a,0x44,0xb9,0x09,0x01,0x09,0x0b,0x08,0x01,0x0a,0x0b,0xe9,0x03,0x29,0x2a,0x08,0x01,0x09,0x0b,0xe8,0xab,0x01,0xb9}},
 {0x26902fc,{0xf9,0x3a,0x04,0xb9,0xff,0x42,0x04,0xb9,0xff,0x3a,0x02,0xf9,0xff,0x3e,0x02,0xf9,0xff,0x42,0x02,0xf9,0xff,0x4a,0x02,0xf9,0xff,0x4e,0x02,0xf9,0xff,0x52,0x02,0xf9}},
 {0x2690298,{0xe9,0x3e,0x04,0xb9,0xe9,0x10,0x00,0xb0,0x20,0x99,0xc0,0x3d,0xe9,0x10,0x00,0x90,0xff,0xc2,0x01,0xf9,0xff,0xc6,0x01,0xf9,0xff,0xca,0x01,0xf9,0xff,0xce,0x01,0xf9}},
 {0x2604a60,{0xe9,0x23,0xb9,0x6d,0xfd,0x7b,0x01,0xa9,0xfd,0x43,0x00,0x91,0xfc,0x6f,0x02,0xa9,0xfa,0x67,0x03,0xa9,0xf8,0x5f,0x04,0xa9,0xf6,0x57,0x05,0xa9,0xf4,0x4f,0x06,0xa9}},
 {0x2601c54,{0xc0,0x05,0x80,0x52,0xc0,0x03,0x5f,0xd6,0xe0,0x03,0x1f,0x2a,0xc0,0x03,0x5f,0xd6,0xc0,0x03,0x5f,0xd6,0x20,0x00,0x80,0x52,0xc0,0x03,0x5f,0xd6,0x08,0x28,0x42,0xf9}},
 {0x25ff0e0,{0xe9,0x23,0xb9,0x6d,0xfd,0x7b,0x01,0xa9,0xfd,0x43,0x00,0x91,0xfc,0x6f,0x02,0xa9,0xfa,0x67,0x03,0xa9,0xf8,0x5f,0x04,0xa9,0xf6,0x57,0x05,0xa9,0xf4,0x4f,0x06,0xa9}},
 {0x2610070,{0x00,0x86,0x80,0x52,0x8f,0x8f,0x08,0x94,0x88,0x70,0x00,0xb0,0x08,0x21,0x27,0x91,0x08,0x00,0x00,0xf9,0x16,0x80,0x00,0x91,0xfa,0x03,0x00,0xaa,0xf3,0x03,0x00,0xaa}},
 {0x25c5c28,{0x00,0x05,0x80,0x52,0xc0,0x03,0x5f,0xd6,0xfd,0x7b,0xbd,0xa9,0xf5,0x0b,0x00,0xf9,0xf4,0x4f,0x02,0xa9,0xfd,0x03,0x00,0x91,0xf4,0x03,0x00,0xaa,0x07,0xd6,0xfc,0x97}},
 {0x25c7494,{0xfd,0x7b,0xbe,0xa9,0xf3,0x0b,0x00,0xf9,0xfd,0x03,0x00,0x91,0xf3,0x03,0x08,0xaa,0x08,0x38,0x42,0xf9,0x0a,0x3c,0x42,0xf9,0x1f,0x01,0x0a,0xeb,0x40,0x03,0x00,0x54}},
 {0x2608070,{0x80,0x05,0x80,0x52,0xc0,0x03,0x5f,0xd6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xe8,0x0f,0x1b,0xfc,0xfd,0x7b,0x01,0xa9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9}},
 {0x260b358,{0x82,0x14,0x00,0xf0,0x42,0x7c,0x35,0x91,0xe0,0x83,0x00,0x91,0xe1,0x03,0x13,0xaa,0xe3,0x01,0x80,0x52,0x3d,0x21,0xfb,0x97,0xe8,0x27,0x42,0xa9,0x68,0x5a,0x01,0xf9}},
 {0x25f43ec,{0xfd,0x7b,0xbe,0xa9,0xf3,0x0b,0x00,0xf9,0xfd,0x03,0x00,0x91,0xe8,0x03,0x01,0xaa,0xf3,0x03,0x00,0xaa,0x01,0x80,0x04,0x91,0x02,0x00,0x08,0x91,0xe0,0x03,0x08,0xaa}},
 {0x25f44a0,{0xff,0xc3,0x02,0xd1,0xfd,0x7b,0x07,0xa9,0xf7,0x43,0x00,0xf9,0xf6,0x57,0x09,0xa9,0xf4,0x4f,0x0a,0xa9,0xfd,0xc3,0x01,0x91,0x28,0x04,0x40,0xf9,0xf5,0x03,0x00,0xaa}},
 {0x25f11f0,{0xfd,0x7b,0xbf,0xa9,0xfd,0x03,0x00,0x91,0xe8,0x03,0x00,0xaa,0x09,0xa0,0x43,0x39,0x00,0x44,0x40,0xf9,0x09,0x01,0x00,0x34,0xe1,0x00,0x00,0x36,0x80,0x01,0x00,0xb4}},
 {0x24cb26c,{0x00,0x0c,0x40,0xf9,0xc0,0x03,0x5f,0xd6,0x08,0x0c,0x40,0xf9,0x68,0x00,0x00,0xb4,0x00,0x0d,0x40,0xf9,0xc0,0x03,0x5f,0xd6,0xe0,0x03,0x1f,0xaa,0xc0,0x03,0x5f,0xd6}},
 {0x2605380,{0xd7,0x09,0x00,0xb4,0x95,0x02,0x40,0xf9,0xe8,0xfa,0x40,0xb9,0x09,0x2a,0x42,0xf9,0x33,0x11,0x08,0x8b,0xa0,0x66,0x40,0xfd,0xa8,0xa6,0x43,0x39,0xe0,0x07,0x80,0x3d}},
};
uint64_t U(const uint8_t*p,size_t n=8){uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;}
struct Trace {
 const ReadFn&r; size_t total{};
 std::vector<std::pair<uint64_t,std::vector<uint8_t>>> records;
 std::vector<uint8_t>B(uint64_t a,size_t n){
  if(!a||n>4096||a>UINT64_MAX-n||records.size()>=2048||total+n>131072)throw std::runtime_error("budget");
  std::vector<uint8_t>b(n);if(!r(a,b.data(),n))throw std::runtime_error("read");total+=n;records.emplace_back(a,b);return b;
 }
 uint64_t Q(uint64_t a){if(a%8)throw std::runtime_error("alignment");return U(B(a,8).data());}
 bool Replay(){for(auto&[a,b]:records){std::vector<uint8_t>v(b.size());if(!r(a,v.data(),v.size())||v!=b)return false;}return true;}
};
void Epoch(Trace&t,uint64_t b,const MapEpochSource&s){
 auto a=t.Q(b+0x34bc848);if(a!=s.save_epoch.access_object||t.Q(a+0x10)!=s.save_epoch.save_data)throw std::runtime_error("epoch");
 auto save=s.save_epoch.save_data;
 if(t.Q(save+0x16cf8)!=s.save_epoch.list||t.Q(save+0x16b80)!=s.traversal)throw std::runtime_error("epoch");
 auto h=t.B(s.save_epoch.list,32);
 if(U(h.data()+24,4)!=s.save_epoch.revision||U(h.data()+16,4)>MaxLearnedKeys||t.Q(b+0x34b80b8)!=s.registry_owner||t.Q(s.registry_owner+0x78)!=s.registry_header||t.Q(s.registry_header+0x20)!=s.registry_entries)throw std::runtime_error("epoch");
}
uint64_t Pane(Trace&t,uint64_t b,uint64_t c){
 if(!c||c%8||c>UINT64_MAX-0x20)throw std::runtime_error("pane component");
 auto vt=t.Q(c);if(vt<b||vt>b+0x3500000||t.Q(vt+0x50)!=b+0x24cb26c)throw std::runtime_error("pane accessor");
 auto w=t.Q(c+0x18);if(!w||w%8||w>UINT64_MAX-0x20)throw std::runtime_error("pane wrapper");
 auto p=t.Q(w+0x18);if(!p||p%8||p>UINT64_MAX-0x60)throw std::runtime_error("pane address");return p;
}
std::array<float,2> Point(Trace&t,uint64_t p){auto q=t.B(p+0x30,8);std::array<float,2>v{std::bit_cast<float>(uint32_t(U(q.data(),4))),std::bit_cast<float>(uint32_t(U(q.data()+4,4)))};for(auto f:v)if(!std::isfinite(f)||std::abs(f)>1e7f)throw std::runtime_error("point");return v;}
void Relocate(Trace&t,uint64_t b,uint64_t view,const DungeonMapDefinition&d,const std::vector<uint8_t>&raw,NativeDungeonIcon&i){
 // Original2604A94..2604B70 constructs 0_<level>_<EC>_<FC>, then
 // resolves the exact authored room instance through view+3D0's native cache.
 auto ec=U(raw.data()+0xec,4),fc=U(raw.data()+0xfc,4),number=U(raw.data()+0xf0,4);
 if(ec>65535||fc>65535||number>65535)return;
 const auto key="0_"+d.key+"_"+std::to_string(ec)+"_"+std::to_string(fc);
 uint64_t hash=0xcbf29ce484222325;for(unsigned char c:key)hash=(hash^c)*UINT64_C(0x100000001b3);
 auto h=t.B(view+0x3d0,16);auto buckets=U(h.data()),count=U(h.data()+8);if(!buckets||buckets%8||!count||count>8192||buckets>UINT64_MAX-count*8)return;
 auto bucket=hash%count,pred=t.Q(buckets+bucket*8),node=pred?t.Q(pred):0;std::set<uint64_t>seen;uint64_t room{};
 while(node){if(node%8||seen.size()>=512||!seen.insert(node).second)throw std::runtime_error("room cache");auto e=t.B(node,64);auto code=U(e.data()+8);if(code%count!=bucket)break;
  if(code==hash&&U(e.data()+0x28)==key.size()){auto k=t.B(U(e.data()+0x20),key.size());if(std::equal(k.begin(),k.end(),key.begin())){if(room)throw std::runtime_error("duplicate room");room=U(e.data()+0x30);if(room<32||room%8||room>UINT64_MAX-0x100||U(e.data()+0x38)!=room-32||t.Q(room)!=b+0x341fa10||t.Q(room-32)!=b+0x3422918)throw std::runtime_error("room owner");}}
  node=U(e.data());
 }
 if(!room)return;
 auto r=t.B(room+0x98,4);if(U(r.data(),4)!=number)return;
 auto flooridx=t.B(room+0xf8,4),v=t.B(view+0x450,24);auto first=U(v.data()),last=U(v.data()+8),cap=U(v.data()+16),idx=U(flooridx.data(),4);
 if(!first||first%8||first>last||last>cap||(last-first)%16||(cap-first)%16||(cap-first)/16>16||idx>=(last-first)/16)return;
 auto floor=t.Q(first+idx*16);auto floorpoint=Point(t,Pane(t,b,floor)),roompoint=Point(t,Pane(t,b,room));
 // Original2690298/26902FC: view41C=floorMin,418=floorNum;
 //260042C..448: floor-root index = authoredFloor-min(floorMin,0)-1.
 auto meta=t.B(view+0x418,8);const auto count_native=std::bit_cast<int32_t>(uint32_t(U(meta.data(),4))),min_native=std::bit_cast<int32_t>(uint32_t(U(meta.data()+4,4)));
 if(count_native!=d.floor_count||min_native!=d.floor_min)return;
 const int64_t actual_floor=int64_t(idx)+std::min(min_native,0)+1;
 if(actual_floor<d.floor_min||actual_floor>=int64_t(d.floor_min)+d.floor_count)return;
 auto authored=std::find_if(d.rooms.begin(),d.rooms.end(),[&](auto&r){return r.number==number&&std::find(r.floors.begin(),r.floors.end(),actual_floor)!=r.floors.end();});
 if(authored==d.rooms.end())return;
 i.room=uint16_t(number);i.floor=int32_t(actual_floor);i.floor_known=true;
 i.native_x=i.pane_x-floorpoint[0];i.native_y=i.pane_y-floorpoint[1];i.room_x=roompoint[0];i.room_y=roompoint[1];i.projection_known=true;
}
DungeonIconSprite Sprite(Trace&t,uint64_t b,uint64_t icon){
 // Original25F3DDC/25F3DF8 stores the exact type17 resource pair here;
 //25F3E80..E88 binds its texture to the actual native actor picture.
 auto bound=t.B(icon+0x1e8,16);const auto target=U(bound.data()),owner=U(bound.data()+8);
 if(!target||!owner)return DungeonIconSprite::Unknown;
 auto h=t.B(b+0x34c8f88,16);auto buckets=U(h.data()),count=U(h.data()+8);
 if(!buckets||buckets%8||!count||count>256||buckets>UINT64_MAX-count*8)return DungeonIconSprite::Unknown;
 auto bucket=17%count,pred=t.Q(buckets+bucket*8),node=pred?t.Q(pred):0;std::set<uint64_t>seen;uint64_t cache{};
 while(node){if(node%8||seen.size()>=256||!seen.insert(node).second)throw std::runtime_error("resource types");auto e=t.B(node,24);const auto hash=U(e.data()+8);if(hash%count!=bucket)break;if(hash==17&&e[0x10]==17){if(cache)throw std::runtime_error("duplicate type");cache=node+0x18;}node=U(e.data());}
 if(!cache)return DungeonIconSprite::Unknown;
 auto head=t.B(cache,16);buckets=U(head.data());count=U(head.data()+8);
 if(!buckets||buckets%8||!count||count>8192||buckets>UINT64_MAX-count*8)return DungeonIconSprite::Unknown;
 DungeonIconSprite found=DungeonIconSprite::Unknown;
 for(auto [name,sprite]:{std::pair<std::string_view,DungeonIconSprite>{"TreasureBox_Base",DungeonIconSprite::TreasureBox},{"TreasureBoxOpen_Base",DungeonIconSprite::TreasureBoxOpen}}){
  uint64_t hash=0xcbf29ce484222325;for(unsigned char c:name)hash=(hash^c)*UINT64_C(0x100000001b3);
  bucket=hash%count;pred=t.Q(buckets+bucket*8);node=pred?t.Q(pred):0;seen.clear();
  while(node){if(node%8||seen.size()>=512||!seen.insert(node).second)throw std::runtime_error("resource names");auto e=t.B(node,0x68);auto code=U(e.data()+8);if(code%count!=bucket)break;
   if(code==hash&&U(e.data()+0x50)==name.size()&&std::equal(name.begin(),name.end(),e.begin()+0x10)&&!e[0x10+name.size()]&&U(e.data()+0x58)==target&&U(e.data()+0x60)==owner){if(found!=DungeonIconSprite::Unknown)throw std::runtime_error("ambiguous sprite");found=sprite;}
   node=U(e.data());
  }
 }
 return found;
}
DungeonIconsSnapshot Frame(Trace&t,uint64_t b,const MapSnapshot&m,const DungeonMapDefinition&d){
 DungeonIconsSnapshot o;o.source=m.source;o.key=d.key;Epoch(t,b,m.source);
 auto sentinel=b+0x34bf070;auto h=t.B(sentinel,24);auto node=U(h.data()+8),prev=sentinel;std::set<uint64_t>seen;
 while(node!=sentinel){
  if(!node||node%8||seen.size()>=128||!seen.insert(node).second)throw std::runtime_error("list");
  auto l=t.B(node,24);if(U(l.data())!=prev)throw std::runtime_error("links");auto c=U(l.data()+16);
  if(!c||c%8||c>UINT64_MAX-0x488)throw std::runtime_error("owner");
  if(t.Q(c)==b+0x341ebf0){if(o.menu||c<32||t.Q(c-32)!=b+0x34268d0||t.Q(b+0x341ebf0+0x70)!=b+0x25c5c28||t.Q(b+0x341ebf0+0x158)!=b+0x25c7494)throw std::runtime_error("menu type");o.menu=c;}
  prev=node;node=U(l.data()+8);
 }
 if(U(h.data())!=prev||!o.menu)throw std::runtime_error("no menu");
 auto outer=t.B(o.menu+0x88,3),inner=t.B(o.menu+0x208,3);
 if(outer[0]!=2||outer[2]!=254||inner[0]<2||inner[0]>5||inner[2]!=254)throw std::runtime_error("closed");
 auto idx=t.B(o.menu+0x468,4),vec=t.B(o.menu+0x470,24);auto first=U(vec.data()),last=U(vec.data()+8),cap=U(vec.data()+16),index=U(idx.data(),4);
 if(!first||first%8||first>last||last>cap||(last-first)%72||(cap-first)%72||(cap-first)/72>64||index>=(last-first)/72)throw std::runtime_error("maps");
 auto desc=t.B(first+index*72,72);if(U(desc.data()+64)!=d.key.size()||desc[d.key.size()]||!std::equal(d.key.begin(),d.key.end(),desc.begin()))throw std::runtime_error("wrong map");
 auto cache=t.B(b+0x34bcb18,16);auto buckets=U(cache.data()),count=U(cache.data()+8);if(!buckets||buckets%8||!count||count>8192||buckets>UINT64_MAX-count*8)throw std::runtime_error("cache");
 uint64_t hash=0xcbf29ce484222325;for(unsigned char c:d.key)hash=(hash^c)*UINT64_C(0x100000001b3);
 auto bucket=hash%count;auto pred=t.Q(buckets+bucket*8);node=pred?t.Q(pred):0;seen.clear();
 while(node){if(node%8||seen.size()>=512||!seen.insert(node).second)throw std::runtime_error("cache loop");auto e=t.B(node,48);auto code=U(e.data()+8);if(code%count!=bucket)break;
  if(code==hash&&U(e.data()+24)==d.key.size()){auto k=t.B(U(e.data()+16),d.key.size());if(std::equal(k.begin(),k.end(),d.key.begin())){if(o.view)throw std::runtime_error("duplicate view");o.view=U(e.data()+32);if(o.view<32||o.view%8||o.view>UINT64_MAX-0x480||U(e.data()+40)!=o.view-32||t.Q(o.view-32)!=b+0x34228a8||t.Q(o.view)!=b+0x3422760||t.Q(b+0x3422760+0x70)!=b+0x2601c54||t.Q(b+0x3422760+0x20)!=b+0x25ff0e0)throw std::runtime_error("view type");}}
  node=U(e.data());
 }
 if(!o.view)throw std::runtime_error("missing view");
 // Original2601CA4/2601D40/2601D94 caches the current native map floor.
 auto current=t.B(o.view+0x420,4);o.native_floor=std::bit_cast<int32_t>(uint32_t(U(current.data(),4)));
 o.native_floor_known=o.native_floor>=d.floor_min&&o.native_floor<int64_t(d.floor_min)+d.floor_count;
 auto v=t.B(o.view+0x210,24);first=U(v.data());last=U(v.data()+8);cap=U(v.data()+16);
 if(first%8||first>last||last>cap||(last-first)%16||(cap-first)%16||(cap-first)/16>1024||(!first&&last))throw std::runtime_error("icons");
 seen.clear();for(auto at=first;at<last;at+=16){auto pair=t.B(at,16);auto icon=U(pair.data());if(!icon||icon%8||icon>UINT64_MAX-0x220||!seen.insert(icon).second)throw std::runtime_error("icon");
  if(t.Q(icon)!=b+0x3421818)continue;
  // Original2610078/2610084/26101DC constructs this exact owner pair.
  if(icon<32||U(pair.data()+8)!=icon-32||t.Q(icon-32)!=b+0x34219c8)throw std::runtime_error("actor owner");
  if(t.Q(b+0x3421818+0x50)!=b+0x24cb26c||t.Q(b+0x3421818+0x58)!=b+0x24cb274||t.Q(b+0x3421818+0x78)!=b+0x25f43ec)throw std::runtime_error("actor type");
  auto raw=t.B(icon,0x100);auto role=U(raw.data()+0xf4,4);if((role!=0&&role!=8)||raw[0xe8]>1)throw std::runtime_error("role");
  auto wrapper=U(raw.data()+0x18);if(!wrapper||wrapper%8||wrapper>UINT64_MAX-0x20)throw std::runtime_error("wrapper");auto pane=t.Q(wrapper+0x18);if(!pane||pane%8||pane>UINT64_MAX-0x60)throw std::runtime_error("pane");auto pr=t.B(pane+0x30,0x30);
  auto x=std::bit_cast<float>(uint32_t(U(pr.data(),4))),y=std::bit_cast<float>(uint32_t(U(pr.data()+4,4)));
  if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1e7f||std::abs(y)>1e7f)throw std::runtime_error("position");
  o.icons.push_back({icon,pane,uint32_t(role),x,y,raw[0xe8]==1,(pr[0x28]&1)!=0,U(raw.data()+0x88)!=0,false});
  Relocate(t,b,o.view,d,raw,o.icons.back());
  o.icons.back().sprite=Sprite(t,b,icon);
 }
 Epoch(t,b,m.source);o.valid=true;return o;
}
}
bool DungeonIconVisible(const NativeDungeonIcon&i,const DungeonIconsSnapshot&s,const DungeonProgressSnapshot&p,const DungeonMapDefinition&d,const DungeonMapArt&a,int32_t floor){
 if(!s.compatible||!s.valid||!s.native_floor_known||s.native_floor!=floor||!p.compatible||!p.valid||s.source!=p.source||s.key!=d.key||p.key!=d.key||a.key!=d.key||!i.projection_known||!i.floor_known||i.floor!=floor||i.room>=64||!i.predicate_visible||!i.pane_visible||(i.role!=8||i.sprite==DungeonIconSprite::Unknown)||(!p.map_item&&!(p.visited_rooms&(UINT64_C(1)<<i.room))))return false;
 if(std::find(s.icons.begin(),s.icons.end(),i)==s.icons.end())return false;
 auto room=std::find_if(d.rooms.begin(),d.rooms.end(),[&](auto&r){return r.number==i.room&&std::find(r.floors.begin(),r.floors.end(),floor)!=r.floors.end();});
 auto art=std::find_if(a.rooms.begin(),a.rooms.end(),[&](auto&r){return r.room==i.room;});
 if(room==d.rooms.end()||art==a.rooms.end()||!std::isfinite(d.size_ratio)||d.size_ratio<=0)return false;
 // Relocation and original room-raster coordinates must agree in this exact
 // native observation; otherwise do not silently apply a guessed origin.
 const float x=art->position_x*d.size_ratio,y=-art->position_y*d.size_ratio;
 return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(i.room_x)&&std::isfinite(i.room_y)&&std::abs(x-i.room_x)<=0.01f&&std::abs(y-i.room_y)<=0.01f;
}
std::optional<std::array<float,2>> ProjectDungeonIcon(const NativeDungeonIcon&i,const MapBounds&b,uint32_t cw,uint32_t ch){
 if(!i.projection_known||!i.floor_known||!cw||!ch||cw>8192||ch>8192||!std::isfinite(b.left)||!std::isfinite(b.top)||!std::isfinite(b.width)||!std::isfinite(b.height)||b.width<=0||b.height<=0||b.width>30000||b.height>30000||!std::isfinite(i.native_x)||!std::isfinite(i.native_y))return {};
 const double scale=std::min(1.,2048./std::max(b.width,b.height));
 const int w=std::max(1,int(std::ceil(b.width*scale))),h=std::max(1,int(std::ceil(b.height*scale)));
 const double fit=std::min(double(cw)/w,double(ch)/h);
 const int fw=std::clamp(int(std::lround(w*fit)),1,int(cw)),fh=std::clamp(int(std::lround(h*fit)),1,int(ch));
 const double x=(i.native_x-b.left)*scale,y=(b.top-i.native_y)*scale;
 if(x<0||y<0||x>w||y>h)return {};
 return std::array<float,2>{float(((int(cw)-fw)/2+x*fw/w)/cw),float(((int(ch)-fh)/2+y*fh/h)/ch)};
}
DungeonIconsSnapshot ReadDungeonIcons(const ReadFn&r,uint64_t b,bool matches,const MapSnapshot&m,const DungeonMapDefinition&d){
 DungeonIconsSnapshot out;
 if(!r||!matches||!b||b%4096||b>UINT64_MAX-0x34bf088||!m.compatible||!m.discovery_valid||!m.current_level_valid||m.current_level_key!=d.key||d.key.empty()||d.key.size()>=64||d.rooms.empty())return out;
 try{
  for(auto&g:Guards){std::array<uint8_t,32>v{};if(!r(b+g.at,v.data(),v.size())||!MatchSupportedGuard(v,g.bytes,g.at))return out;}
  out.compatible=true;Trace a{r,0,{}},c{r,0,{}};auto x=Frame(a,b,m,d),y=Frame(c,b,m,d);if(x!=y||a.records!=c.records||!c.Replay())return out;
  y.compatible=true;return y;
 }catch(...){return out;}
}
}
