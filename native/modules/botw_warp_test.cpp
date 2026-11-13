// SPDX-License-Identifier: GPL-3.0-or-later
// Travel: guard/transaction logic, native travel-point + allow-check readers, bridge request
// (mailbox op 13) and the confirm controller. Memory layouts mirror internal notes
#include "01007EF00011E000.cpp"
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
using namespace botw;
using Clock=std::chrono::steady_clock;
struct WarpFixture:Fixture {
  static constexpr uint64_t D=0xC000000,SA=0xC001000,TA=0xC002000,R0=0xC010000,E0=0xC020004,STR=0xC030000,
    EV=0xC100000,L1=0xC200000,L2=0xC201000,L3=0xC202000,LS=0xC300000,G=0xC400000,HO=0xC401000,BK=0xC402000,FA=0xC403000,FL=0xC404000;
  void str(uint64_t at,const char* s){std::array<char,64> b{};std::strncpy(b.data(),s,63);put(at,b);}
  void record(unsigned n,bool tower,unsigned slot,const char* msg,const char* map,const char* pos,std::array<float,3> xyz,uint32_t state) {
    const uint64_t r=R0+n*0x100,e=E0+n*0x100,s=STR+n*0x200;
    put((tower?TA:SA)+slot*8,r);put(r,M+(tower?0x2B86A00:0x2B86010));put<int32_t>(r+0x1C,tower?3:2);put(r+0x28,xyz);put<uint32_t>(r+0x3C,state);put(r+0x58,e);
    put(e+8,s);put(e+0x28,s+0x40);put(e+0x30,s+0x80);str(s,msg);str(s+0x40,map);str(s+0x80,pos);
  }
  void flag(unsigned i,uint32_t hash,bool value){const uint64_t p=FL+i*16;put(FA+i*8,p);put(p,M+0x2BD1078);put<uint8_t>(p+8,0);put<uint8_t>(p+9,0);put<uint8_t>(p+10,0);put<uint8_t>(p+11,value);put<uint32_t>(p+12,hash);}
  // Iteration 12 item 13a: the placed Travel Medallion (MapData+480, class 2B875E8, kind 42,
  // +3C 0x210 = live save A value after the DLC install) + the native DLC Travel strings.
  static constexpr uint64_t MED=0xC050000;static constexpr std::array<float,3> MXYZ{-2692.561279296875f,362.9970703125f,-918.4134521484375f};
  void medallion(uint32_t state=0x210){put(D+0x480,MED);put(MED,M+0x2B875E8);put<int32_t>(MED+0x1C,42);put<uint32_t>(MED+0x3C,state);put(MED+0x28,MXYZ);
    str(M+0x2651911,"MainField/WarpIcon");str(M+0x269652D,"WarpPoint01");}
  std::array<uint32_t,3> hashes{};
  void prohibit(unsigned which,bool v){std::array<std::pair<uint32_t,bool>,3> f;for(unsigned i=0;i<3;++i)f[i]={hashes[i],i==which&&v};
    std::sort(f.begin(),f.end());for(unsigned i=0;i<3;++i)flag(i,f[i].first,f[i].second);}
  WarpFixture(uint64_t flags=0x1e09000000ULL):Fixture(flags) {
    put(M+0x2CA63B0,D);put(D,M+0x2B86848);put<int32_t>(D+0x2F0,2);put<int32_t>(D+0x2F4,136);put(D+0x2F8,SA);put<int32_t>(D+0x310,1);put<int32_t>(D+0x314,15);put(D+0x318,TA);
    record(0,false,0,"Dungeon053","MainField/D-6","StartPos_CDungeon053",{-1234.5f,250,1500},0x2a8890);   // live save A value: entered, +3C bit9 not cached
    record(1,false,1,"Dungeon000","MainField/E-5","StartPos_CDungeon000",{100,200,300},0x88a90);   // seen, Enter_ bit17 clear, stale bit9 set
    record(2,true,0,"Tower05","MainField/F-3","StartPos_Tower05",{883.88427734375f,371.03424072265625f,-1605.710205078125f},0x28890);put<uint8_t>(R0+2*0x100+0x64,1);
    put(M+0x2CC56D8,EV);put(EV,M+0x2BCF2F8);put<uint64_t>(EV+0x1D2B8,0);
    put(M+0x2CD1590,L1);put(L1+0x28,L2);put(L2+0x98,L3);put<uint8_t>(L3+0x28,0);
    put(A+69*8,LS);put(A+78*8,LS+0x100);put<uint8_t>(LS+0xFE,0);put<int8_t>(LS+0xFD,0);put<uint8_t>(LS+0x1FE,0);put<int8_t>(LS+0x1FD,-1);
    put(M+0x2CC5FE8,G);put(G,M+0x2BD07E0);put(G+0xBE0,HO);put(HO,BK);put<int32_t>(BK+8,3);put<int32_t>(BK+12,3);put(BK+16,FA);
    hashes={warp::crc("WarpProhibition"),warp::crc("WarpProhibitionArea"),warp::crc("WarpProhibitionArea_Fire_Relic")};prohibit(0,false);
  }
};
struct WarpBridgeFixture:WarpFixture {
  std::array<uint8_t,4096> mailbox{};SelectionBridge bridge;uint32_t fail=UINT32_MAX;std::map<std::string,double> floats;
  template<class T> void mb(unsigned at,T value){std::memcpy(mailbox.data()+at,&value,sizeof(value));}
  template<class T> T get(unsigned at){T v{};std::memcpy(&v,mailbox.data()+at,sizeof(v));return v;}
  explicit WarpBridgeFixture(uint32_t magic=0x484F5544,const char* page="map") {
    texts["__page"]=page;values["__aux_present"]=1;values["__mailbox_epoch"]=1;
    h.get_i64=[](void*p,const char*k,int64_t fallback){auto& v=static_cast<WarpBridgeFixture*>(p)->values;const auto i=v.find(k);return i==v.end()?fallback:i->second;};
    h.get_text=[](void*p,const char*k)->const char*{return static_cast<WarpBridgeFixture*>(p)->texts[k].c_str();};
    h.publish_f64=[](void*p,const char*k,double v){static_cast<WarpBridgeFixture*>(p)->floats[k]=v;};
    EdenDsmodHostExtensions ext{};ext.version=EDEN_DSMOD_EXT_VERSION;ext.struct_size=sizeof(ext);ext.abi_hash=EDEN_DSMOD_EXT_HASH;ext.userdata=this;ext.mailbox_size=4096;
    ext.load_u32=[](void*p,uint32_t at,uint32_t*v)->EdenDsmodBool{*v=static_cast<WarpBridgeFixture*>(p)->get<uint32_t>(at);return true;};
    ext.store_u32=[](void*p,uint32_t at,uint32_t v)->EdenDsmodBool{auto&f=*static_cast<WarpBridgeFixture*>(p);if(at==f.fail)return false;f.mb(at,v);return true;};
    ext.store_u64=[](void*p,uint32_t at,uint64_t v)->EdenDsmodBool{auto&f=*static_cast<WarpBridgeFixture*>(p);if(at==f.fail)return false;f.mb(at,v);return true;};
    mb<uint32_t>(0,magic);mb<uint32_t>(4,100);mb<uint32_t>(4092,1);bridge.configure(&ext);bridge.poll(h,{});
  }
  void beat(){mb<uint32_t>(4,get<uint32_t>(4)+1);bridge.poll(h,{});}
  void guest_ack(uint32_t result){mb<uint32_t>(16,result);mb<uint32_t>(12,get<uint32_t>(8));}
};
#ifndef BOTW_WARP_FIXTURE_ONLY
int main() {
  unsigned cases{};
  // 1. Guard order and texts.
  {warp::Env e{};e.valid=e.world=true;
   assert(warp::block(e,false,false,warp::Phase::Idle,true)==warp::Block::Unsupported);
   assert(warp::block(e,true,false,warp::Phase::Loading,true)==warp::Block::InFlight);
   assert(warp::block(e,true,true,warp::Phase::Idle,true)==warp::Block::Busy);
   assert(warp::block(e,true,false,warp::Phase::Idle,true)==warp::Block::None);
   assert(warp::block(e,true,false,warp::Phase::Idle,false)==warp::Block::NotTravelPoint);
   auto t=e;t.event=true;assert(warp::block(t,true,false,warp::Phase::Idle,true)==warp::Block::Event);
   t=e;t.latch=true;assert(warp::block(t,true,false,warp::Phase::Idle,true)==warp::Block::Event);
   t=e;t.loading=true;t.event=true;assert(warp::block(t,true,false,warp::Phase::Idle,true)==warp::Block::Loading);
   t=e;t.world=false;t.menu=true;assert(warp::block(t,true,false,warp::Phase::Idle,true)==warp::Block::Menu);
   t=e;t.prohibited=true;assert(warp::block(t,true,false,warp::Phase::Idle,true)==warp::Block::Prohibited);
   assert(std::string(warp::block_text(warp::Block::Prohibited))=="You can't travel from here.");
   t=e;t.valid=false;assert(warp::block(t,true,false,warp::Phase::Idle,true)==warp::Block::NotWorld);
   for(unsigned b=1;b<=10;++b)assert(*warp::block_text(warp::Block(b)));
   cases+=13;}
  // 2. Transaction: one request, full lifecycle, never back to Idle without a quiet world.
  {const auto t0=Clock::now();warp::Env quiet{};quiet.valid=quiet.world=true;auto ev=quiet;ev.event=true;ev.world=false;auto ld=quiet;ld.loading=true;ld.world=false;ld.event=true;
   using std::chrono::milliseconds;
   {warp::Transaction x;x.queued(t0);assert(x.phase==warp::Phase::Queued&&*x.text());x.observe(quiet,t0+milliseconds(100));assert(x.phase==warp::Phase::Queued);
    x.ack(1,t0);assert(x.phase==warp::Phase::Issued);x.observe(quiet,t0+milliseconds(200));assert(x.phase==warp::Phase::Issued);
    x.observe(ev,t0+milliseconds(300));assert(x.phase==warp::Phase::Out);x.observe(ev,t0+std::chrono::seconds(10));assert(x.phase==warp::Phase::Out);
    x.observe(ld,t0+std::chrono::seconds(11));assert(x.phase==warp::Phase::Loading);x.observe(ev,t0+std::chrono::seconds(20));assert(x.phase==warp::Phase::Arrival);
    x.observe(quiet,t0+std::chrono::seconds(25));assert(x.phase==warp::Phase::Arrival);x.observe(quiet,t0+std::chrono::milliseconds(25900));assert(x.phase==warp::Phase::Idle&&x.arrived);++cases;}
   {warp::Transaction x;x.queued(t0);x.ack(2,t0);assert(x.phase==warp::Phase::Idle&&!x.arrived);x.ack(1,t0);assert(x.phase==warp::Phase::Idle);++cases;}
   {warp::Transaction x;x.queued(t0);x.ack(3,t0);assert(x.phase==warp::Phase::Uncertain);x.observe(quiet,t0+std::chrono::seconds(1));x.observe(quiet,t0+std::chrono::seconds(4));assert(x.phase==warp::Phase::Uncertain);
    x.observe(ev,t0+std::chrono::seconds(5));x.observe(quiet,t0+std::chrono::seconds(6));x.observe(quiet,t0+std::chrono::seconds(9));assert(x.phase==warp::Phase::Uncertain);x.observe(quiet,t0+std::chrono::seconds(10));assert(x.phase==warp::Phase::Idle);++cases;}
   {warp::Transaction x;x.queued(t0);x.observe(quiet,t0+std::chrono::seconds(4));assert(x.phase==warp::Phase::Uncertain);++cases;}            // no guest answer
   {warp::Transaction x;x.queued(t0);x.ack(1,t0);x.observe(quiet,t0+std::chrono::seconds(6));assert(x.phase==warp::Phase::Uncertain);++cases;}  // latch but no event
   {warp::Transaction x;x.queued(t0);x.ack(1,t0);x.observe(ld,t0+milliseconds(100));assert(x.phase==warp::Phase::Loading);++cases;}
   {warp::Transaction x;x.queued(t0);x.cancel_unsent();assert(x.phase==warp::Phase::Uncertain);warp::Transaction y;y.cancel_unsent();assert(y.phase==warp::Phase::Idle);++cases;}
   {warp::Transaction x;x.queued(t0);x.ack(1,t0);x.observe(ev,t0+milliseconds(100));x.observe(warp::Env{},t0+milliseconds(200));assert(x.phase==warp::Phase::Out);++cases;}
   {warp::Transaction x;x.queued(t0);x.ack(1,t0);x.observe(ev,t0+milliseconds(100));assert(x.phase==warp::Phase::Out); // never quiet, never loading -> would stick forever pre-fix (bug audit 2026-10-19, MEDIUM #2)
    for(unsigned s=1;s<=59;++s){x.observe(ev,t0+std::chrono::seconds(s));assert(x.phase==warp::Phase::Out);}
    x.observe(ev,t0+std::chrono::seconds(61));assert(x.phase==warp::Phase::Uncertain); // absolute fallback recovers it
    assert(warp::block(quiet,true,false,x.phase,true)!=warp::Block::None); // still "in flight" until the world settles
    x.observe(quiet,t0+std::chrono::seconds(62));x.observe(quiet,t0+std::chrono::seconds(66));assert(x.phase==warp::Phase::Idle); // quiet settle -> Idle (quiet_since=62, +4s settle)
    assert(warp::block(quiet,true,false,x.phase,true)==warp::Block::None);++cases;}} // further Travel tap now works again
  // 3. Travel point reader.
  {WarpFixture f;const auto p=warp::point(f.h,0);assert(p.valid&&p.enabled()&&p.kind==2&&p.index==0&&p.message=="Dungeon053"&&p.map=="MainField/D-6"&&p.pos=="StartPos_CDungeon053"&&p.record==WarpFixture::R0&&p.entry==WarpFixture::E0);
   const auto q=warp::point(f.h,1);assert(q.valid&&!q.enabled());const auto t=warp::point(f.h,2);assert(t.valid&&t.enabled()&&t.kind==3&&t.index==0&&t.message=="Tower05");
   assert(!warp::point(f.h,3).valid);
   f.put<uint8_t>(WarpFixture::R0+2*0x100+0x64,0);assert(warp::point(f.h,2).valid&&!warp::point(f.h,2).enabled());
   f.put<uint8_t>(WarpFixture::R0+2*0x100+0x64,2);assert(!warp::point(f.h,2).valid);cases+=6;}
  {auto bad=[&](auto change){WarpFixture f;change(f);assert(!warp::point(f.h,0).valid);++cases;};
   using F=WarpFixture;
   bad([](F& f){f.put<uint64_t>(F::M+0x2CA63B0,0);});bad([](F& f){f.put<uint64_t>(F::D,F::M);});bad([](F& f){f.put<int32_t>(F::D+0x2F0,137);});
   bad([](F& f){f.put<int32_t>(F::D+0x2F4,137);});bad([](F& f){f.put<int32_t>(F::D+0x2F0,-1);});bad([](F& f){f.put<uint64_t>(F::D+0x2F8,0);});
   bad([](F& f){f.put<uint64_t>(F::SA,0);});bad([](F& f){f.put<uint64_t>(F::SA,F::R0+4);});bad([](F& f){f.put<uint64_t>(F::R0,F::M+0x2B86A00);});
   bad([](F& f){f.put<int32_t>(F::R0+0x1C,3);});bad([](F& f){f.put<uint64_t>(F::R0+0x58,0);});bad([](F& f){f.put<uint64_t>(F::E0+0x28,0);});
   bad([](F& f){f.put<uint64_t>(F::E0+0x30,0);});bad([](F& f){f.put<uint64_t>(F::E0+8,0);});bad([](F& f){f.str(F::STR+0x40,"");});
   bad([](F& f){std::array<char,64> b;b.fill('A');f.put(F::STR+0x80,b);});bad([](F& f){f.str(F::STR+0x40,"Main\x01");});
   bad([](F& f){f.put(F::R0+0x28,std::array<float,3>{NAN,0,0});});bad([](F& f){f.race=F::R0+0x3C;});}
  // 4. Allow-check inputs.
  {WarpFixture f;auto e=warp::env(f.h);assert(e.valid&&e.world&&!e.event&&!e.menu&&!e.loading&&!e.prohibited&&!e.latch);++cases;
   for(unsigned i=0;i<3;++i){WarpFixture g;g.prohibit(i,true);assert(warp::env(g.h).prohibited);++cases;}
   {WarpFixture g(0x1e09000004ULL);auto x=warp::env(g.h);assert(x.valid&&x.menu&&!x.world);++cases;}
   {WarpFixture g(0x1e09003800ULL);auto x=warp::env(g.h);assert(x.valid&&x.event&&!x.world);++cases;}
   {WarpFixture g(0x0e09e00000ULL);auto x=warp::env(g.h);assert(x.valid&&x.loading);++cases;}
   {WarpFixture g;g.put<uint64_t>(WarpFixture::EV+0x1D2B8,0x1234);assert(warp::env(g.h).event);++cases;}
   {WarpFixture g;g.put<uint8_t>(WarpFixture::L3+0x28,1);assert(warp::env(g.h).latch);++cases;}
   {WarpFixture g;g.put<uint64_t>(WarpFixture::L1+0x28,0);auto x=warp::env(g.h);assert(x.valid&&!x.latch);++cases;}
   {WarpFixture g;g.put<uint8_t>(WarpFixture::LS+0xFE,2);assert(warp::env(g.h).loading);++cases;}
   {WarpFixture g;g.put<int8_t>(WarpFixture::LS+0x1FD,1);assert(warp::env(g.h).loading);++cases;}
   {WarpFixture g;g.put<uint64_t>(Fixture::A+69*8,0);assert(warp::env(g.h).valid&&!warp::env(g.h).loading);++cases;}
   {WarpFixture g;g.put<uint64_t>(WarpFixture::EV,0);assert(!warp::env(g.h).valid);++cases;}
   {WarpFixture g;g.put<int32_t>(WarpFixture::BK+8,2);g.put<int32_t>(WarpFixture::BK+12,2);
    auto x=warp::env(g.h);(void)x;++cases;}  // a missing prohibition flag may sort out of the table: must not crash
   {WarpFixture g;g.put<uint32_t>(WarpFixture::FL+12,1);assert(!warp::env(g.h).valid);++cases;}
   {WarpFixture g;g.race=WarpFixture::EV+0x1D2B8;assert(!warp::env(g.h).valid);++cases;}}
  // 5. Trackers row -> native travel point.
  {WarpFixture f;MapLandmarks m;m.valid=true;m.count=4;
   m.entries[0]={true,true,false,2,{-1234.5f,250,1500}};m.entries[1]={true,false,false,2,{100,200,300}};m.entries[2]={true,true,true,3,{883.88427734375f,371.03424072265625f,-1605.710205078125f}};
   assert(warp::find(f.h,m,"Dungeon053",-1234.5f,1500)==0);assert(warp::find(f.h,m,"Dungeon054",-1234.5f,1500)==-1);
   assert(warp::find(f.h,m,"Tower05",883.88427734375f,-1605.710205078125f)==2);assert(warp::find(f.h,m,"Tower05",0,0)==-1);
   assert(warp::find(f.h,m,"",0,0)==-1);m.entries[3]=m.entries[0];assert(warp::find(f.h,m,"Dungeon053",-1234.5f,1500)==-1);cases+=6;}
  // 6. Bridge request: payload, refusals, one request at a time.
  {WarpBridgeFixture f;assert(f.bridge.warp_supported&&f.bridge.healthy);const auto p=warp::point(f.h,0);
   assert(f.bridge.request_warp(0,p));assert(f.get<uint32_t>(20)==13&&f.get<uint64_t>(1024)==WarpFixture::D&&f.get<uint64_t>(1032)==WarpFixture::R0&&f.get<uint32_t>(1040)==0&&f.get<uint32_t>(1044)==2&&
     f.get<uint64_t>(1048)==WarpFixture::E0&&f.get<uint64_t>(1056)==p.map_ptr&&f.get<uint64_t>(1064)==p.pos_ptr&&f.get<uint32_t>(1072)==warp::fnv("MainField/D-6")&&
     f.get<uint32_t>(1076)==warp::fnv("StartPos_CDungeon053")&&f.get<uint64_t>(1080)==WarpFixture::EV&&f.get<uint32_t>(72)==f.get<uint32_t>(8)&&f.get<uint32_t>(8)==1&&f.get<uint64_t>(48)==Fixture::U);
   assert(!f.bridge.request_warp(0,p)&&(f.bridge.warp_refusal&4));  // pending
   f.guest_ack(1);f.beat();assert(!f.bridge.pending&&f.bridge.warp_result==1&&f.bridge.warp_acked==1);cases+=3;}
  {WarpBridgeFixture f(0x474F5544);assert(!f.bridge.warp_supported&&!f.bridge.request_warp(0,warp::point(f.h,0))&&(f.bridge.warp_refusal&2));++cases;}
  {WarpBridgeFixture f(0x484F5544,"inventory");assert(!f.bridge.request_warp(0,warp::point(f.h,0))&&(f.bridge.warp_refusal&32));++cases;}
  {WarpBridgeFixture f(0x484F5544,"trackers");assert(f.bridge.request_warp(2,warp::point(f.h,2))&&f.get<uint32_t>(1044)==3);++cases;}
  {WarpBridgeFixture f;auto p=warp::point(f.h,0);f.put<uint32_t>(WarpFixture::R0+0x3C,0x88a90);assert(!f.bridge.request_warp(0,p)&&f.bridge.warp_refusal==512&&!f.get<uint32_t>(8));++cases;}
  {WarpBridgeFixture f;const auto p=warp::point(f.h,1);assert(!f.bridge.request_warp(1,p)&&f.bridge.warp_refusal==512);++cases;}
  {WarpBridgeFixture f;f.put<uint64_t>(Fixture::U+0x64c30,0x1e09000004ULL);assert(!f.bridge.request_warp(0,warp::point(f.h,0)));++cases;}
  {WarpBridgeFixture f;f.fail=1056;assert(!f.bridge.request_warp(0,warp::point(f.h,0))&&!f.get<uint32_t>(8)&&!f.get<uint32_t>(72));++cases;}
  {WarpBridgeFixture f;f.values["__mailbox_epoch"]=2;assert(!f.bridge.request_warp(0,warp::point(f.h,0)));++cases;}
  // 7. Controller: confirm overlay, refusals with the native text, exactly one request.
  {WarpBridgeFixture f;TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=3;
   m.entries[0]={true,true,false,2,{-1234.5f,250,1500}};m.entries[1]={true,false,false,2,{100,200,300}};m.entries[2]={true,true,true,3,{883.88427734375f,371.03424072265625f,-1605.710205078125f}};
   map.observed=m;map.selected=0;const auto now=Clock::now();
   ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);ui.publish(f.h,true,false,false,true,now);
   assert(f.values["botw.warp.map.available"]==1&&f.values["botw.warp.confirm"]==0);
   assert(!ui.action("warp_confirm",f.h,"map",map,f.bridge,now));                       // nothing to confirm
   f.prohibit(1,true);assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now));ui.publish(f.h,true,false,false,true,now);
   assert(f.values["botw.warp.confirm"]==0&&f.texts["botw.warp.status"]=="You can't travel from here."&&!f.get<uint32_t>(8));
   f.prohibit(1,false);f.put<uint64_t>(WarpFixture::EV+0x1D2B8,0x55);assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now));assert(ui.message=="Can't travel during an event");
   f.put<uint64_t>(WarpFixture::EV+0x1D2B8,0);
   assert(ui.action("warp_open_map",f.h,"map",map,f.bridge,now));assert(f.get<uint32_t>(8)==1&&ui.tx.phase==warp::Phase::Queued&&!ui.confirm&&map.selected==-1);
   assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&!ui.action("warp_confirm",f.h,"map",map,f.bridge,now)&&f.get<uint32_t>(8)==1);
   f.guest_ack(1);f.beat();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},Clock::now());assert(ui.tx.phase==warp::Phase::Issued);
   ui.publish(f.h,true,false,false,true,Clock::now());assert(f.texts["botw.warp.status"]=="Already traveling"&&f.values["botw.warp.status.visible"]==1);  // the refused re-open explains itself
   ui.message.clear();ui.publish(f.h,true,false,false,true,Clock::now());assert(f.texts["botw.warp.status"]=="Traveling...");
   map.selected=0;map.observed=m;ui.map_at={};ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},Clock::now());ui.publish(f.h,true,false,false,true,Clock::now());
   assert(f.values["botw.warp.map.available"]==0);  // no Travel button while one is in flight
   cases+=8;}
  {WarpBridgeFixture f;TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=3;m.entries[0]={true,true,false,2,{-1234.5f,250,1500}};
   map.observed=m;map.selected=0;auto now=Clock::now();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);
   assert(ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&!ui.confirm);
   f.guest_ack(2);f.beat();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},Clock::now());assert(ui.tx.phase==warp::Phase::Idle&&ui.message=="Travel refused by the game");
   assert(ui.action("warp_cancel",f.h,"map",map,f.bridge,now)==false);++cases;}
  {WarpBridgeFixture f;TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=3;m.entries[1]={true,false,false,2,{100,200,300}};
   map.observed=m;map.selected=1;auto now=Clock::now();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);ui.publish(f.h,true,false,false,true,now);
   assert(f.values["botw.warp.map.available"]==0&&!ui.action("warp_open_map",f.h,"map",map,f.bridge,now));++cases;}  // seen, not entered: no Travel
  {WarpBridgeFixture f;TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=1;m.entries[0]={true,true,false,2,{-1234.5f,250,1500}};
   map.observed=m;map.selected=0;auto now=Clock::now();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);
   assert(ui.action("warp_open_map",f.h,"map",map,f.bridge,now));ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::Menu,{},now);assert(!ui.confirm);++cases;} // native menu closes the overlay
  {WarpBridgeFixture f;TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=1;m.entries[0]={true,true,false,2,{-1234.5f,250,1500}};
   map.observed=m;map.selected=0;auto now=Clock::now();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);
   f.put<uint64_t>(Fixture::U+0x64c30,0x1e09000004ULL);assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&ui.message=="Close the game menu to travel"&&!ui.confirm);
   f.put<uint64_t>(Fixture::U+0x64c30,0x1e09003800ULL);assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&ui.message=="Can't travel during an event");
   f.put<uint64_t>(Fixture::U+0x64c30,0x1e09000000ULL);f.prohibit(2,true);assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&ui.message=="You can't travel from here.");
   f.prohibit(2,false);assert(ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&!ui.confirm&&f.get<uint32_t>(8)==1);cases+=4;}
  // 9. Ready-page shrine panel Travel -- REMOVED, owner item 11 (iteration 10, 2026-10-20): the
  // player is already standing inside the shrine, Travel to it is redundant. Was added item 9,
  // iteration 9, 2026-10-19 (same test fixtures/shrine data kept below, assertions flipped to
  // confirm the action is now refused everywhere and never opens a confirm overlay, regardless
  // of whether resolve() still privately finds a valid shrine point -- rdy_id/rdy_index/
  // rdy_point bookkeeping in resolve() is deliberately left running (harmless unused state, see
  // botw_warp_ui.h's comment) rather than threading a resolve()/sample() signature change
  // through every call site for a now-dead branch.
  {WarpBridgeFixture f(0x484F5544,"ready");TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=1;
   m.entries[0]={true,true,false,2,{-3822.2373046875f,278.1203918457031f,2206.895751953125f}};
   auto now=Clock::now();
   ui.sample(f.h,"ready",map,trk,m,f.bridge,UiContext::World,{},now,/*shrine_active=*/true,/*shrine_number=*/53);
   ui.publish(f.h,false,false,true,true,now);
   assert(f.values["botw.warp.confirm"]==0);
   assert(!ui.action("warp_open_map",f.h,"ready",map,f.bridge,now));             // wrong-page opener refused
   assert(!ui.action("warp_open_ready",f.h,"map",map,f.bridge,now));             // action no longer exists at all
   assert(!ui.action("warp_open_ready",f.h,"ready",map,f.bridge,now));           // ...not even on the ready page
   ui.publish(f.h,false,false,true,true,now);
   assert(f.values["botw.warp.confirm"]==0&&!ui.confirm);                       // never opens a confirm overlay
   cases+=4;}
  // warp_scope() (botw_bridge.h) no longer includes "ready": module-level wiring (on_action,
  // bridge request gating) outside TravelUi itself is checked by #8 below, unchanged by this
  // TravelUi-level test; "ready" simply never reaches TravelUi::action() as an opening page now.
  // 10. Iteration 12 item 13a: Travel Medallion as a travel point (reader, bridge payload, Map inspect).
  {WarpFixture f;f.medallion();const auto p=warp::point(f.h,warp::MedallionIndex);
   assert(p.valid&&p.enabled()&&p.kind==42&&p.index==0&&p.entry==0&&p.record==WarpFixture::MED&&p.data==WarpFixture::D&&p.xyz==WarpFixture::MXYZ&&
     p.map=="MainField/WarpIcon"&&p.pos=="WarpPoint01"&&p.map_ptr==Fixture::M+0x2651911&&p.pos_ptr==Fixture::M+0x269652D&&p.message=="Obj_WarpDLC");
   f.put<uint32_t>(WarpFixture::MED+0x3C,0x200);const auto q=warp::point(f.h,warp::MedallionIndex);assert(q.valid&&!q.enabled());  // not placed: no Travel
   assert(warp::point(f.h,0).valid&&warp::point(f.h,0).kind==2);cases+=3;}
  {WarpFixture f;assert(!warp::point(f.h,warp::MedallionIndex).valid);++cases;}  // no DLC record at all
  {auto bad=[&](auto change){WarpFixture f;f.medallion();change(f);assert(!warp::point(f.h,warp::MedallionIndex).valid);++cases;};
   using F=WarpFixture;
   bad([](F& f){f.put<uint64_t>(F::D+0x480,0);});bad([](F& f){f.put<uint64_t>(F::D+0x480,F::MED+4);});bad([](F& f){f.put<uint64_t>(F::MED,F::M+0x2B86010);});
   bad([](F& f){f.put<int32_t>(F::MED+0x1C,2);});bad([](F& f){f.str(F::M+0x2651911,"MainField/WarpIco");});bad([](F& f){f.str(F::M+0x269652D,"WarpPoint02");});
   bad([](F& f){f.put(F::MED+0x28,std::array<float,3>{NAN,0,0});});bad([](F& f){f.put(F::MED+0x28,std::array<float,3>{9000,0,0});});
   bad([](F& f){f.put<uint64_t>(F::D,F::M);});bad([](F& f){f.race=F::MED+0x3C;});}
  {WarpBridgeFixture f;f.medallion();const auto p=warp::point(f.h,warp::MedallionIndex);
   assert(f.bridge.request_warp(warp::MedallionIndex,p));
   assert(f.get<uint32_t>(20)==13&&f.get<uint64_t>(1024)==WarpFixture::D&&f.get<uint64_t>(1032)==WarpFixture::MED&&f.get<uint32_t>(1040)==0&&f.get<uint32_t>(1044)==42&&
     f.get<uint64_t>(1048)==0&&f.get<uint64_t>(1056)==Fixture::M+0x2651911&&f.get<uint64_t>(1064)==Fixture::M+0x269652D&&
     f.get<uint32_t>(1072)==warp::fnv("MainField/WarpIcon")&&f.get<uint32_t>(1076)==warp::fnv("WarpPoint01")&&f.get<uint32_t>(8)==1);cases+=1;}
  {WarpBridgeFixture f;f.medallion();const auto p=warp::point(f.h,warp::MedallionIndex);f.put<uint32_t>(WarpFixture::MED+0x3C,0x200);
   assert(!f.bridge.request_warp(warp::MedallionIndex,p)&&f.bridge.warp_refusal==512&&!f.get<uint32_t>(8));++cases;}  // picked up after display
  {WarpBridgeFixture f;f.medallion();const auto p=warp::point(f.h,warp::MedallionIndex);f.put(WarpFixture::MED+0x28,std::array<float,3>{1,2,3});
   assert(!f.bridge.request_warp(warp::MedallionIndex,p)&&f.bridge.warp_refusal==512);++cases;}  // re-placed elsewhere: stale
  {WarpBridgeFixture f;f.medallion();TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=1;m.entries[0]={true,true,false,2,{-1234.5f,250,1500}};
   map.observed=m;map.medallion={true,42,WarpFixture::MXYZ};assert(map.action("map_marker",351,true)&&map.medallion_selected&&map.selected==-1);
   auto now=Clock::now();ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);ui.publish(f.h,true,false,false,true,now);
   assert(ui.map_index==int(warp::MedallionIndex)&&f.values["botw.warp.map.available"]==1);
   f.prohibit(0,true);assert(!ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&ui.message=="You can't travel from here."&&!f.get<uint32_t>(8));f.prohibit(0,false);
   assert(ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&!ui.confirm&&ui.tx.phase==warp::Phase::Queued&&f.get<uint32_t>(1044)==42&&
     ui.name_key==MapUi::medallion_name_key()&&ui.target_message=="Obj_WarpDLC"&&ui.target_xyz==WarpFixture::MXYZ&&!map.medallion_selected);
   cases+=3;}
  {WarpBridgeFixture f;f.medallion(0x200);TravelUi ui;MapUi map;TrackerView trk;MapLandmarks m;m.valid=true;m.count=0;
   map.medallion={true,42,WarpFixture::MXYZ};map.medallion_selected=true;auto now=Clock::now();
   ui.sample(f.h,"map",map,trk,m,f.bridge,UiContext::World,{},now);ui.publish(f.h,true,false,false,true,now);
   assert(f.values["botw.warp.map.available"]==0&&!ui.action("warp_open_map",f.h,"map",map,f.bridge,now)&&!f.get<uint32_t>(8));++cases;}  // record says not placed
  {assert(MapUi::medallion_name_key()==uint32_t(actor_key("Obj_WarpDLC"))&&MapUi::medallion_name_key()==4255962846u);++cases;}
  // 8. Module wiring: actions route through on_action; helper level 14.
  {assert(SelectionBridge::helper_level(0x484F5544)==14&&SelectionBridge::helper_level(0x474F5544)==13);State s;assert(!on_action(&s,"warp_confirm",0)&&!on_action(&s,"warp_open_map",0)&&!on_action(&s,"warp_cancel",0));cases+=2;}
  std::cout<<cases<<" travel guard/transaction/reader/bridge/controller cases passed\n";
}

#endif
