// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Include the reader to seed its private exact instruction fixtures without
// publishing a second guard API. Do not also link echoes_warp_menu.cpp here.
#include "echoes_warp_menu.cpp"
#include "echoes_warp_direct.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
namespace {
size_t checks{};
void Check(bool ok,const char *s) {++checks;if(!ok){std::cerr<<"FAIL "<<s<<'\n';std::abort();}}
constexpr uint64_t Base=0x80000000,Owner=0x90000020,Node=0x90010000,Descriptor=0x90020000,Focus=0x90030000;
struct Fixture {
 std::map<uint64_t,uint8_t> memory;
 echoes::NativeWarpCatalog catalog{{{"Field",{{"Field","Point","Warp_Field_Test",7}}}},{{"Field","Field"}}, {}};
 void Put(uint64_t p,uint64_t v,size_t n=8){for(size_t i=0;i<n;++i)memory[p+i]=uint8_t(v>>(8*i));}
 void Zero(uint64_t p,size_t n){for(size_t i=0;i<n;++i)memory[p+i]=0;}
 void Text(uint64_t p,std::string_view s){for(size_t i=0;i<s.size();++i)memory[p+i]=s[i];memory[p+s.size()]=0;}
 Fixture(){
  for(const auto &g:echoes::Guards)for(size_t i=0;i<g.bytes.size();++i)memory[Base+g.offset+i]=g.bytes[i];
  auto sentinel=Base+0x34bf070;Zero(sentinel,24);Put(sentinel,Node);Put(sentinel+8,Node);
  Put(Node,sentinel);Put(Node+8,sentinel);Put(Node+16,Owner);
  Put(Owner,Base+0x341ebf0);Put(Owner-0x20,Base+0x34268d0);
  Put(Base+0x341ebf0+0x70,Base+0x25c5c28);Put(Base+0x341ebf0+0x158,Base+0x25c7494);
  Zero(Owner+0x88,3);Put(Owner+0x88,2,1);Put(Owner+0x8a,0xfe,1);
  Zero(Owner+0x208,3);Put(Owner+0x208,2,1);Put(Owner+0x20a,0xfe,1);
  Put(Owner+0x468,0,4);Put(Owner+0x470,Descriptor);Put(Owner+0x478,Descriptor+0x48);Put(Owner+0x480,Descriptor+0x48);
  Zero(Descriptor,0x48);Text(Descriptor,"Field");Put(Descriptor+0x40,5);
  Put(Owner+0x3b0,Focus);Put(Owner+0x3b8,0x90040000);
  Zero(Focus,0xf8);Put(Focus,Base+0x34223d8);Put(Focus+0xf4,4,4);Put(Focus+0xe1,1,1);
  Put(Base+0x34223d8+0xc0,Base+0x25fda30);
  Zero(Focus+0x180,0x4e);Text(Focus+0x180,"Field");Put(Focus+0x1c0,5);Put(Focus+0x1cd,1,1);
 }
 void Targets(){
  for(const auto &g:echoes::CursorGuards)for(size_t i=0;i<g.bytes.size();++i)memory[Base+g.offset+i]=g.bytes[i];
  Zero(Owner+0x398,8);
  Put(Owner+0x4a0,0x90050020);Put(Base+0x341ebf0+0x168,Base+0x25c7560);Put(Base+0x34223d8+0x58,Base+0x24cb274);
  Put(Focus+0x18,0x90066000);Put(0x90066018,0x90067000);
  Put(0x90067030,std::bit_cast<uint32_t>(42.5f),4);Put(0x90067034,std::bit_cast<uint32_t>(-60.0f),4);
  Zero(Focus+0xf8,0x88);
  for(const auto &g:echoes::ZoomGuards)for(size_t i=0;i<g.bytes.size();++i)memory[Base+g.offset+i]=g.bytes[i];
  Put(Owner+0x260,Base+0x341db60);Put(Base+0x341db60+0x28,Base+0x25b3744);
  Put(Base+0x341e240+0x30,Base+0x25bc138);Put(Base+0x341e288+0x30,Base+0x25bbff4);
  Zero(Owner+0x268,3);Put(Owner+0x26a,0xfe,1);Put(Owner+0x418,0,2);
  Put(Owner+0x440,0x90068000);Put(Owner+0x448,0x9006800c);Put(Owner+0x450,0x9006800c);
  Put(0x90068000,std::bit_cast<uint32_t>(1.f),4);Put(0x90068004,std::bit_cast<uint32_t>(2.f),4);Put(0x90068008,std::bit_cast<uint32_t>(3.f),4);
  Put(Owner+0x3d0,std::bit_cast<uint32_t>(1.f),4);
  Put(Owner+0x2a0,0x90069000);Put(Owner+0x2a8,0x900691e0);Put(Owner+0x2b0,0x900691e0);
  Put(0x900690c0,0x9006a000);Put(0x900690f0,0x9006a010);
  Put(0x9006a000,Base+0x341e240);Put(0x9006a008,Owner);Put(0x9006a010,Base+0x341e288);Put(0x9006a018,Owner);
  for(const auto &g:echoes::TargetGuards)for(size_t i=0;i<g.bytes.size();++i)memory[Base+g.offset+i]=g.bytes[i];
  constexpr uint64_t view=0x90050020,buckets=0x90060000,predecessor=0x90061000,node=0x90062000,key=0x90063000,pairs=0x90064000;
  uint64_t hash=0xcbf29ce484222325;for(unsigned char c:std::string("Field"))hash=(hash^c)*UINT64_C(0x100000001b3);
  Put(Base+0x34bcb18,buckets);Put(Base+0x34bcb20,1);Put(buckets,predecessor);Put(predecessor,node);
  Put(node,0);Put(node+8,hash);Put(node+16,key);Put(node+24,5);Put(node+32,view);Put(node+40,view-0x20);Text(key,"Field");
  Put(view+0x2c0,0x90065000);Put(0x90065030,std::bit_cast<uint32_t>(-8.35f),4);Put(0x90065034,std::bit_cast<uint32_t>(3.0f),4);
  Put(view,Base+0x3422aa8);Put(view-0x20,Base+0x3429fc0);Put(Base+0x3422aa8+0x70,Base+0x2608070);Put(Base+0x3422aa8+0x130,Base+0x260ec60);
  Put(view+0x210,pairs);Put(view+0x218,pairs+16);Put(view+0x220,pairs+16);Put(pairs,Focus);Put(pairs+8,Focus-0x20);Put(Focus-0x20,Base+0x34224d0);
  Put(Focus+0xc8,std::bit_cast<uint32_t>(12.5f),4);Put(Focus+0xcc,std::bit_cast<uint32_t>(-20.0f),4);
 }
 echoes::WarpMenuSnapshot WithTargets(){return echoes::ReadWarpMenu([&](auto p,auto out,auto n){return Read(p,out,n);},Base,true,catalog,true);}
 bool Read(uint64_t p,void *out,size_t n)const{auto *b=static_cast<uint8_t*>(out);for(size_t i=0;i<n;++i){auto it=memory.find(p+i);if(it==memory.end())return false;b[i]=it->second;}return true;}
 echoes::WarpMenuSnapshot Run(){return echoes::ReadWarpMenu([&](auto p,auto out,auto n){return Read(p,out,n);},Base,true,catalog);}
};
}
int main(){
 Fixture f;auto good=f.Run();
 Check(good.compatible&&good.valid&&good.focus_warp&&good.selectable&&good.native_allowed,"stable original warp focus");
 Check(good.map_level=="Field"&&good.unlock_flag=="Warp_Field_Test"&&good.destination_index==0,"exact authored scope and ordinal");
 for(uint8_t state:{2,3,4,5}){auto t=f;t.Put(Owner+0x208,state,1);Check(t.Run().valid,"native states observed");}
 for(auto [at,value,n]:std::vector<std::tuple<uint64_t,uint64_t,size_t>>{
  {Owner-0x20,Base+0x34268d8,8},{Owner+0x88,0,1},{Owner+0x8a,0,1},
  {Owner+0x208,8,1},{Owner+0x20a,3,1},{Owner+0x468,1,4},
  {Owner+0x478,Descriptor+0x47,8},{Owner+0x480,Descriptor+0x4800,8},
  {Descriptor+0x40,64,8},{Focus+0xf4,3,4},{Focus+0x1c0,64,8},
  {Focus+0x1c8,1,4},{Focus+0x1cd,2,1},{Focus+0xe1,2,1},
  {Base+0x34223d8+0xc0,Base+0x25fda38,8},{Node+8,Node,8},{Node,0,8}}){
   auto t=f;t.Put(at,value,n);auto s=t.Run();Check(s.compatible&&!s.valid&&!s.focus_warp,"invalid native source clears observation");
 }
 auto t=f;t.Text(Focus+0x180,"Other");Check(!t.Run().valid,"native icon scope cannot be substituted by ordinal");
 t=f;t.Put(Focus+0x1cd,0,1);Check(t.Run().valid&&!t.Run().native_allowed,"native denial retained");
 t=f;t.Put(Focus+0xe1,0,1);Check(t.Run().valid&&!t.Run().selectable,"native unselectable retained");
 t=f;t.Put(Owner+0x3b0,0);Check(t.Run().valid&&!t.Run().focus_warp,"map without focus has no warp");
 t=f;t.Put(Focus,Base+0x3420000);Check(t.Run().valid&&!t.Run().focus_warp,"other focused type grants nothing");
 t=f;t.catalog.level_aliases.emplace_back("AlternateArea","Field");t.Text(Descriptor,"AlternateArea");t.Put(Descriptor+0x40,13);t.Text(Focus+0x180,"AlternateArea");t.Put(Focus+0x1c0,13);Check(t.Run().valid,"exact native alias");
 t=f;t.Text(Descriptor,"Unknown");t.Put(Descriptor+0x40,7);Check(!t.Run().valid,"unknown map refuses");
 for(const auto &g:echoes::Guards){t=f;t.memory[Base+g.offset]^=1;Check(!t.Run().compatible,"every original guard checked");}
 size_t calls{};auto changing=[&](uint64_t p,void*out,size_t n){++calls;if(calls==50)f.Put(Focus+0x1cd,0,1);return f.Read(p,out,n);};
 Check(!echoes::ReadWarpMenu(changing,Base,true,f.catalog).valid,"mid-read mutation refused");
 f=Fixture{};calls=0;auto throwing=[&](uint64_t p,void*out,size_t n){if(++calls==30)throw 1;return f.Read(p,out,n);};
 Check(!echoes::ReadWarpMenu(throwing,Base,true,f.catalog).valid,"callback exceptions clear state");
 Check(!echoes::ReadWarpMenu({},Base,true,f.catalog).valid,"missing reader refuses");
 t=Fixture{};t.Targets();auto targets=t.WithTargets();
 Check(targets.valid&&targets.targets_known&&targets.targets.size()==1,"authenticated original native icon vector");
 Check(targets.targets[0].cursor_known&&std::abs(targets.targets[0].cursor_x-34.15f)<0.00001f&&targets.targets[0].cursor_y==-57.0f,"live rendered pane plus authored map anchor supplies native cursor coordinates");
 Check(targets.targets[0].ordinal==0&&targets.targets[0].x==12.5f&&targets.targets[0].y==-20.0f,"original ordinal and native projected coordinates");
 for(auto [at,value,n]:std::vector<std::tuple<uint64_t,uint64_t,size_t>>{
  {0x90050000,Base+0x3429fc8,8},{Base+0x3422aa8+0x130,0,8},
  {0x90050020+0x218,0x90064011,8},{0x90050020+0x220,0x90064000+16*1025,8},
  {0x90062000,0x90062000,8},{0x90062000+24,6,8},
  {Focus-0x20,Base+0x34224d8,8},{Focus+0xc8,0x7fc00000,4}}){
  auto bad=t;bad.Put(at,value,n);Check(!bad.WithTargets().valid,"malformed native cache/vector/type/coordinate refuses");
 }
 {auto root=t;root.Put(Focus+0xe9,1,1);auto observed=root.WithTargets();
  Check(observed.valid&&observed.targets[0].cursor_x==42.5f&&observed.targets[0].cursor_y==-60,"root-space native icon omits area anchor");}
 for(auto [at,value,n]:std::vector<std::tuple<uint64_t,uint64_t,size_t>>{
  {Owner+0x4a0,0x90050028,8},{Base+0x341ebf0+0x168,0,8},{Base+0x34223d8+0x58,0,8},
  {Focus+0x18,0,8},{Focus+0x18,0x90066001,8},{0x90066018,0,8},{0x90066018,0x90067001,8},
  {0x90067030,0x7fc00000,4},{Focus+0xe9,2,1}}){auto bad=t;bad.Put(at,value,n);Check(!bad.WithTargets().valid,"invalid native rendered-pane coordinate source refuses");}
 for(const auto &g:echoes::CursorGuards){auto bad=t;bad.memory[Base+g.offset]^=1;Check(!bad.WithTargets().compatible,"every cursor original code guard checked");}
 {auto bad=t;bad.Put(Owner+0x398,0x7fc00000,4);Check(!bad.WithTargets().valid,"nonfinite native cursor refuses");}
 for(const auto &g:echoes::TargetGuards){auto bad=t;bad.memory[Base+g.offset]^=1;Check(!bad.WithTargets().compatible,"every target original code guard checked");}
 {
  Fixture direct;direct.Targets();auto expected=direct.WithTargets();size_t writes{};
  auto read=[&](uint64_t p,void *out,size_t n){return direct.Read(p,out,n);};
  auto write=[&](uint64_t p,const void *bytes,size_t n){
   ++writes;Check(p==Owner+0x398&&n==8,"direct request writes only aligned native cursor pair");
   auto raw=static_cast<const uint8_t*>(bytes);uint64_t value{};for(size_t i=0;i<n;++i)value|=uint64_t(raw[i])<<(i*8);
   Check(uint32_t(value)==std::bit_cast<uint32_t>(expected.targets[0].cursor_x)&&uint32_t(value>>32)==std::bit_cast<uint32_t>(expected.targets[0].cursor_y),"direct pair is exact authored target plus native anchor");return true;
  };
  Check(echoes::DirectWarpTarget(read,Base,true,direct.catalog,expected,0,write)&&writes==1,"fresh exact target authorizes one cursor write");
  for(int mode=0;mode<8;++mode){auto bad=expected;writes=0;
   if(mode==0)bad.state=echoes::WarpMapState::ConfirmWarp;
   if(mode==1)bad.targets[0].native_allowed=false;
   if(mode==2)bad.targets[0].selectable=false;
   if(mode==3)bad.targets[0].cursor_x=NAN;
   if(mode==4)bad.targets_known=false;
   if(mode==5)bad.component++;
   if(mode==6)bad.native_view++;
   if(mode==7)bad.map_index++;
   Check(!echoes::DirectWarpTarget(read,Base,true,direct.catalog,bad,0,write)&&writes==0,"invalid or stale direct target refuses all writes");
  }
  writes=0;direct.Put(Focus+0x1cd,0,1);
  Check(!echoes::DirectWarpTarget(read,Base,true,direct.catalog,expected,0,write)&&writes==0,"revoked native permission revalidation refuses stale request");
 }
 {
  Fixture zoom;zoom.Targets();zoom.Put(Owner+0x418,2,1);zoom.Put(Owner+0x419,2,1);zoom.Put(Owner+0x3d0,std::bit_cast<uint32_t>(3.f),4);
  auto expected=zoom.WithTargets();Check(expected.valid&&expected.zoom_valid&&expected.zoom_index==2&&expected.zoom_scale==3.f,"native zoom lifecycle and authored scale vector observed");
  auto read=[&](uint64_t p,void *out,size_t n){return zoom.Read(p,out,n);};
  std::vector<std::pair<uint64_t,uint8_t>> writes;
  auto write=[&](uint64_t p,const void *bytes,size_t n){Check(n==1,"zoom request writes one byte per native field");writes.emplace_back(p,*static_cast<const uint8_t*>(bytes));return true;};
  Check(echoes::RequestNativeFullZoom(read,Base,true,zoom.catalog,expected,write)&&writes==std::vector<std::pair<uint64_t,uint8_t>>{{Owner+0x418,0},{Owner+0x26a,1}},"native full zoom writes target index then pending Zoom1 only");
  for(int mode=0;mode<6;++mode){auto bad=expected;writes.clear();
   if(mode==0)bad.zoom_state=1;if(mode==1)bad.zoom_pending=1;if(mode==2)bad.zoom_previous=1;
   if(mode==3)bad.zoom_scale=2.f;if(mode==4)bad.zoom_scales[0]=0.5f;if(mode==5)bad.component+=8;
   Check(!echoes::RequestNativeFullZoom(read,Base,true,zoom.catalog,bad,write)&&writes.empty(),"stale or unsettled native zoom refuses writes");
  }
  writes.clear();zoom.Put(Owner+0x26a,1,1);
  Check(!echoes::RequestNativeFullZoom(read,Base,true,zoom.catalog,expected,write)&&writes.empty(),"fresh pending transition prevents repeated zoom request");
 }
 for(const auto &g:echoes::ZoomGuards){auto bad=t;bad.memory[Base+g.offset]^=1;Check(!bad.WithTargets().compatible,"native zoom transition guards checked");}
 for(auto [at,value,n]:std::vector<std::tuple<uint64_t,uint64_t,size_t>>{
  {Owner+0x260,0,8},{Base+0x341db60+0x28,0,8},{Owner+0x418,3,1},
  {Owner+0x268,3,1},{Owner+0x26a,3,1},{0x90068004,std::bit_cast<uint32_t>(0.f),4},
  {0x90068008,std::bit_cast<uint32_t>(1.f),4},{0x9006a008,Owner+8,8},{0x9006a018,Owner+8,8}}){
  auto bad=t;bad.Put(at,value,n);Check(!bad.WithTargets().valid,"malformed native zoom lifecycle/scales/callback owner refuses");}
 std::cout<<checks<<" warp menu observation/refusal checks passed; native travel not granted\n";
}
