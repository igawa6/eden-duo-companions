// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_WARP_FIXTURE_ONLY
#include "botw_warp_test.cpp"
#include "botw_inventory_update_fixture.h"
namespace {
template<class F> void travel_update(F& f) {
  f.medallion();update_fixture(f);
  const std::pair<uint64_t,uint64_t> ids[]={
    {0x2CA63B0,0x1E17EF8},{0x2B86848,0x1CDD6C8},{0x2B86010,0x1CDC1E0},{0x2B86A00,0x1CDC410},
    {0x2B875E8,0x1CDD530},{0x2CC56D8,0x1E3A638},{0x2BCF2F8,0x1D1FCE8},{0x2CD1590,0x1E465B0}};
  auto original=f.mem;
  for(auto [at,byte]:original){(void)byte;if(at%8)continue;uint64_t v{};bool full=true;
    for(unsigned i=0;i<8;++i){auto it=original.find(at+i);if(it==original.end()){full=false;break;}v|=uint64_t(it->second)<<(i*8);}
    if(full)for(auto [old,next]:ids)if(v==F::M+old){f.put(at,F::M+next);break;}}
  for(auto [old,next]:ids)if(original.contains(F::M+old)){
    uint64_t v{};for(unsigned i=0;i<8;++i)v|=uint64_t(f.mem.at(F::M+old+i))<<(i*8);
    f.put(F::M+next,v);for(unsigned i=0;i<8;++i)f.mem.erase(F::M+old+i);}
  f.str(F::M+0x15EEDEF,"MainField/WarpIcon");f.str(F::M+0x161794D,"WarpPoint01");
  for(unsigned i=0;i<64;++i){f.mem.erase(F::M+0x2651911+i);f.mem.erase(F::M+0x269652D+i);}
}
}
int main(){
  {WarpFixture f;travel_update(f);assert(warp_build_supported(f.h));assert(supported(f.h));
    assert(warp::point(f.h,0).enabled());assert(!warp::point(f.h,1).enabled());assert(warp::point(f.h,2).enabled());
    assert(warp::point(f.h,warp::MedallionIndex).enabled());auto e=warp::env(f.h);assert(e.valid&&e.world&&!e.event&&!e.prohibited);
    f.put<uint64_t>(WarpFixture::EV+0x1D2C8,0x1234);assert(!warp::env(f.h).event);
    f.put<uint64_t>(WarpFixture::EV+0x1D2B8,0x1234);assert(warp::env(f.h).event);
    f.h.build_id[0]^=1;assert(!warp::point(f.h,0).valid&&!warp_build_supported(f.h));}
  for(unsigned n=0;n<3;++n){WarpFixture f;f.prohibit(n,true);travel_update(f);assert(warp::env(f.h).prohibited);}
  {WarpFixture f;travel_update(f);f.put<uint64_t>(WarpFixture::EV,Fixture::M+0x2BCF2F8);assert(!warp::env(f.h).valid);}
  {WarpFixture f;travel_update(f);f.put<uint8_t>(WarpFixture::L3+0x28,1);assert(warp::env(f.h).latch);}
  {WarpBridgeFixture f;travel_update(f);f.beat();assert(f.bridge.warp_supported);auto p=warp::point(f.h,0);
    assert(f.bridge.request_warp(0,p));assert(f.get<uint64_t>(1080)==WarpFixture::EV);
    assert(!f.bridge.request_warp(0,p));f.guest_ack(1);f.beat();assert(f.bridge.warp_result==1&&!f.bridge.pending);}
  {WarpBridgeFixture f;travel_update(f);f.beat();auto p=warp::point(f.h,0);f.put<uint32_t>(WarpFixture::R0+0x3C,0);
    assert(!f.bridge.request_warp(0,p)&&f.bridge.warp_refusal==512&&!f.get<uint32_t>(8));}
  {WarpFixture f;travel_update(f);std::string name;assert(event_name(f.h,name)&&name.empty());
    constexpr uint64_t list=0xD000000,array=list+0x1000,entry=list+0x2000,text=list+0x3000;
    f.put(WarpFixture::EV+0x1D2B8,list);f.put(list,Fixture::M+0x1D201A0);f.put<uint32_t>(list+8,1);f.put(list+0x10,array);
    f.put<int32_t>(list+0x1F0,0);f.put<int8_t>(list+0x1E8,0);f.put(array,entry);f.put(entry+0x18,text);f.str(text,"Demo008_1");
    assert(event_name(f.h,name)&&name=="Demo008_1");f.put<int8_t>(list+0x1E8,1);assert(!event_name(f.h,name));}
  std::cout<<"1.9.0 Travel: points, prohibitions, event identity, latch, stale/pending guards passed\n";
}
