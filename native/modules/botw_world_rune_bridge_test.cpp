// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_world_runes_fixture.h"
#include <cassert>
#include <iostream>
namespace {
struct WorldRuneBridgeFixture:WorldRuneFixture {
 std::array<uint8_t,4096> mailbox{};State state;uint32_t fail=UINT32_MAX,change_epoch=UINT32_MAX;
 template<class T> void mb(unsigned at,T value){std::memcpy(mailbox.data()+at,&value,sizeof(value));}
 template<class T> T get(unsigned at){T value{};std::memcpy(&value,mailbox.data()+at,sizeof(value));return value;}
 WorldRuneBridgeFixture() {
  texts["__page"]="ready";values["__aux_present"]=1;values["__mailbox_epoch"]=1;
  h.get_text=[](void*p,const char*k)->const char*{return static_cast<WorldRuneBridgeFixture*>(p)->texts[k].c_str();};
  h.get_i64=[](void*p,const char*k,int64_t fallback){const auto&v=static_cast<WorldRuneBridgeFixture*>(p)->values;const auto i=v.find(k);return i==v.end()?fallback:i->second;};
  EdenDsmodHostExtensions e{};e.version=EDEN_DSMOD_EXT_VERSION;e.struct_size=sizeof(e);e.abi_hash=EDEN_DSMOD_EXT_HASH;e.userdata=this;e.mailbox_size=4096;
  e.load_u32=[](void*p,uint32_t at,uint32_t*v)->EdenDsmodBool{*v=static_cast<WorldRuneBridgeFixture*>(p)->get<uint32_t>(at);return true;};
  e.store_u32=[](void*p,uint32_t at,uint32_t v)->EdenDsmodBool{auto&f=*static_cast<WorldRuneBridgeFixture*>(p);if(at==f.fail)return false;f.mb(at,v);if(at==f.change_epoch){f.values["__mailbox_epoch"]++;f.mb<uint32_t>(4092,f.values["__mailbox_epoch"]);}return true;};
  e.store_u64=[](void*p,uint32_t at,uint64_t v)->EdenDsmodBool{auto&f=*static_cast<WorldRuneBridgeFixture*>(p);if(at==f.fail)return false;f.mb(at,v);return true;};
  mb<uint32_t>(0,0x454F5544);mb<uint32_t>(4,100);mb<uint32_t>(4092,1);state.bridge.configure(&e);state.bridge.poll(h,{});assert(state.bridge.world_rune_supported);
 }
 bool request(int id=2){return state.bridge.request_world_rune(id,botw::world_runes(h));}
};
} // namespace
#ifndef BOTW_WORLD_RUNE_FIXTURE_ONLY
// Touch Rune picker: "select" moves the focus only; "touch_confirm" queues the change.
int main(){using F=WorldRuneBridgeFixture;unsigned cases{};
 {F f;assert(f.request());assert(f.get<uint32_t>(20)==10 && f.get<uint32_t>(712)==2 && f.get<uint64_t>(696)==f.flags[0x795E7BBC]);++cases;}
 for(int id:{-1,6,8}){F f;assert(!f.request(id) && !f.get<uint32_t>(8));++cases;}
 for(const char* p:{"inventory","map","quest","unknown"}){F f;f.texts["__page"]=p;assert(!f.request());++cases;}
 for(uint32_t at:{16,20,48,64,88,640,648,656,664,672,680,688,696,704,708,712,716,720,724,728,732,72,8}){F f;f.fail=at;assert(!f.request() && !f.state.bridge.pending && !f.get<uint32_t>(72));++cases;}
 for(uint32_t at:{712,728,72}){F f;f.change_epoch=at;assert(!f.request() && !f.get<uint32_t>(8) && !f.get<uint32_t>(72));++cases;}
 {F f;auto display=botw::world_runes(f.h);f.flag(0x795E7BBC,false);assert(!f.state.bridge.request_world_rune(2,display));++cases;}
 {F f;f.mb<uint32_t>(0,0x444F5544);f.state.bridge.poll(f.h,{});assert(f.state.bridge.healthy && !f.state.bridge.world_rune_supported && !f.request());++cases;}
 for(int result:{1,2,3}){F f;assert(f.request());f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,result);f.state.bridge.requested-=std::chrono::seconds(4);f.state.bridge.poll(f.h,{});assert(!f.state.bridge.pending && std::string(f.state.bridge.status)==(result==2?"Rune change refused":"Rune change not confirmed"));++cases;}
 {F f;assert(f.request());f.put<int32_t>(F::R+0x240,2);f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.state.bridge.poll(f.h,{});assert(f.state.bridge.pending && std::string(f.state.bridge.status)=="Changing rune");f.put<int32_t>(F::C+0x36E8,2);f.state.bridge.poll(f.h,{});assert(!f.state.bridge.pending && std::string(f.state.bridge.status)=="Rune selected");++cases;}
 {F f;assert(on_action(&f.state,"quick_help",3));sample(&f.state,&f.h);assert(f.state.touch_kind==botw::QuickKind::Rune && f.state.runes.valid);assert(on_action(&f.state,"select",2)&&!f.get<uint32_t>(8)&&!f.state.touch_select_deferred);assert(on_action(&f.state,"touch_confirm",0));sample(&f.state,&f.h);assert(f.state.bridge.pending_world_rune && f.state.touch_kind==botw::QuickKind::None);++cases;}
 {F f;assert(on_action(&f.state,"quick_help",3));sample(&f.state,&f.h);assert(on_action(&f.state,"select",3)&&on_action(&f.state,"touch_confirm",0));assert(!f.get<uint32_t>(8) && f.state.touch_kind==botw::QuickKind::None);++cases;}
 {F f;assert(on_action(&f.state,"quick_help",3));sample(&f.state,&f.h);assert(on_action(&f.state,"select",2)&&!f.get<uint32_t>(8)&&!f.state.touch_select_deferred);assert(on_action(&f.state,"touch_confirm",0));f.state.bridge.heartbeat_at-=std::chrono::milliseconds(150);sample(&f.state,&f.h);assert(f.state.touch_select_deferred && !f.state.bridge.pending);f.mb<uint32_t>(4,101);sample(&f.state,&f.h);assert(!f.state.touch_select_deferred && f.state.bridge.pending_world_rune);++cases;}
 for(int reason=0;reason<4;++reason){F f;assert(on_action(&f.state,"quick_help",3));sample(&f.state,&f.h);assert(on_action(&f.state,"select",2)&&!f.get<uint32_t>(8)&&!f.state.touch_select_deferred);assert(on_action(&f.state,"touch_confirm",0));if(reason==0)f.texts["__page"]="map";if(reason==1)f.values["__aux_present"]=0;if(reason==2){f.values["__mailbox_epoch"]=2;f.mb<uint32_t>(4092,2);}if(reason==3)assert(on_action(&f.state,"quick_close",0));sample(&f.state,&f.h);assert(!f.get<uint32_t>(8) && !f.state.touch_select_deferred);++cases;}
 {F f;for(auto h:botw::RuneUnlockHashes)f.flag(h,false);assert(on_action(&f.state,"quick_help",3));sample(&f.state,&f.h);assert(f.values["botw.quick.empty"]==1 && !on_action(&f.state,"select",0));assert(on_action(&f.state,"quick_close",0) && !f.get<uint32_t>(8));++cases;}
 std::cout<<cases<<" World Rune bridge/scope/deferred/native-confirmation cases passed\n";
}

#endif
