// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BOTW_MAP_FIXTURE_ONLY
#include "01007EF00011E000.cpp"
#endif
#include "botw_test_fixture.h"
#include <cassert>
#include <iostream>
#include <limits>
struct SavedFixture:Fixture {
 static constexpr uint64_t G=0xA000000,H=0xA001000,B=0xA002000,Groups=0xA003000,NoGroup=0xA004000,PosGroup=0xA005000,NoEntries=0xA006000,PosEntries=0xA007000,No=0xA010000,Pos=0xA020000;
 SavedFixture() {
  put(M+0x2CC5FE8,G);put(G,M+0x2BD07E0);put(G+0xBE0,H);put(H,B);put<uint32_t>(G+0xC18,0);put<uint32_t>(G+0xC28,0);
  put<uint32_t>(M+0x2B7E7D8,0);put<uint32_t>(M+0x2B7E7DC,0);
  for(unsigned i=0;i<2;++i){auto off=i?0x108:0xA8;put<int32_t>(B+off,1);put<int32_t>(B+off+4,1);put(B+off+8,Groups+i*8);put(Groups+i*8,i?PosGroup:NoGroup);auto group=i?PosGroup:NoGroup;put<int32_t>(group,200);put<int32_t>(group+4,200);put(group+8,i?PosEntries:NoEntries);}
  for(unsigned i=0;i<200;++i){auto n=No+i*40,p=Pos+i*72;put(NoEntries+i*8,n);put(PosEntries+i*8,p);put(n,M+0x2BD1140);put<uint8_t>(n+8,10);put<int32_t>(n+0x18,i<9?27+i:(i==9?41:-1));put<uint32_t>(n+0x1C,0x9383490E);put(p,M+0x2BD15F0);put<uint8_t>(p+8,16);put(p+0x30,std::array<float,3>{float(i)*10,300,-float(i)*10});put<uint32_t>(p+0x3C,0xEA9DEF3F);}
  for(uint64_t off:{0x2C9E868ULL,0x2CA4DA0ULL}){std::array<uint8_t,56>b{};uint64_t vt=M+0x2A701C8,p=M+off+20;int32_t cap=32;std::memcpy(b.data(),&vt,8);std::memcpy(b.data()+8,&p,8);std::memcpy(b.data()+16,&cap,4);std::memcpy(b.data()+20,"MainField",10);put(M+off,b);}
 }
};
struct EditFixture:SavedFixture {
 static constexpr uint64_t D=0xB000000,C=0xB010000,AR=0xB020000,N=0xB030000;
 EditFixture() {
  put(M+0x2CA63B0,D);put(D,M+0x2B86848);put(M+0x2CA63B8,C);put(C,M+0x2B8AAD8);put(C+0x8D8,D);put(C+0x8E0,D+0x850);put(D+0x850,M+0x2B86868);
  put<int32_t>(D+0x610,100);put<int32_t>(D+0x614,100);put(D+0x618,AR);put<int32_t>(D+0x658,5);put<int32_t>(D+0x65C,5);put(D+0x660,AR+800);put(D+0x650,N+5*0x80);
  put(D+0x878,std::array<float,4>{.28f,0,5981.897f,-3886.656f});put(D+0x8B0,std::array<float,2>{2,2});
  for(unsigned i=0;i<105;++i){auto p=N+i*0x80;put(i<5?AR+800+i*8:AR+(i-5)*8,p);put(p,M+(i<5?0x2B866B0:0x2B874F0));put<int32_t>(p+0x1C,i<5?37+i:45);put<uint32_t>(p+0x3C,0);put<int32_t>(p+0x40,i);put(p+0x28,std::array<float,3>{-100000,0,0});}
 }
};
struct EditBridgeFixture:EditFixture {
 std::array<uint8_t,4096> mailbox{};botw::SelectionBridge bridge;uint32_t fail=UINT32_MAX;
 std::map<std::string,double> floats;
 template<class T> void mb(unsigned at,T value){std::memcpy(mailbox.data()+at,&value,sizeof(value));}
 template<class T> T get(unsigned at){T v{};std::memcpy(&v,mailbox.data()+at,sizeof(v));return v;}
 EditBridgeFixture() {
  texts["__page"]="map";values["__aux_present"]=1;values["__mailbox_epoch"]=1;
  h.get_i64=[](void*p,const char*k,int64_t fallback){auto& v=static_cast<EditBridgeFixture*>(p)->values;const auto i=v.find(k);return i==v.end()?fallback:i->second;};
  h.get_text=[](void*p,const char*k)->const char*{return static_cast<EditBridgeFixture*>(p)->texts[k].c_str();};
  h.get_f64=[](void*p,const char*k,double fallback){auto&v=static_cast<EditBridgeFixture*>(p)->floats;const auto i=v.find(k);return i==v.end()?fallback:i->second;};
  h.publish_f64=[](void*p,const char*k,double v){static_cast<EditBridgeFixture*>(p)->floats[k]=v;};
  EdenDsmodHostExtensions ext{};ext.version=EDEN_DSMOD_EXT_VERSION;ext.struct_size=sizeof(ext);ext.abi_hash=EDEN_DSMOD_EXT_HASH;ext.userdata=this;ext.mailbox_size=4096;
  ext.load_u32=[](void*p,uint32_t at,uint32_t*v)->EdenDsmodBool{*v=static_cast<EditBridgeFixture*>(p)->get<uint32_t>(at);return true;};
  ext.store_u32=[](void*p,uint32_t at,uint32_t v)->EdenDsmodBool{auto&f=*static_cast<EditBridgeFixture*>(p);if(at==f.fail)return false;f.mb(at,v);return true;};
  ext.store_u64=[](void*p,uint32_t at,uint64_t v)->EdenDsmodBool{auto&f=*static_cast<EditBridgeFixture*>(p);if(at==f.fail)return false;f.mb(at,v);return true;};
  mb<uint32_t>(0,0x444F5544);mb<uint32_t>(4,100);mb<uint32_t>(4092,1);bridge.configure(&ext);bridge.poll(h,{});assert(bridge.map_supported);
 }
 void native_place() {put<int32_t>(N+0x1C,37);put<uint32_t>(N+0x3C,16);put(N+0x28,std::array<float,3>{123,300,456});}
 void save_place(){put<int32_t>(No+10*40+0x18,37);put(Pos+10*72+0x30,std::array<float,3>{123,300,456});}
};
#ifndef BOTW_MAP_FIXTURE_ONLY
int main(){using F=EditFixture;unsigned cases{};
 auto check=[&](auto change,bool valid){F f;change(f);const auto v=botw::map_edit_snapshot(f.h);if(v.valid!=valid)std::cerr<<"case "<<cases+1<<"\n";assert(v.valid==valid);++cases;};
 check([](F&){},true);
 for(auto at:{F::M+0x2CA63B0,F::M+0x2CA63B8,F::C+0x8D8,F::C+0x8E0,F::D+0x618,F::D+0x660,F::AR,F::AR+800})for(uint64_t p:{0ULL,0x10001ULL,UINT64_MAX-7ULL})check([&](F&f){f.put(at,p);},false);
 for(auto at:{F::D,F::C,F::D+0x850,F::N,F::N+5*0x80})check([&](F&f){f.put<uint64_t>(at,0);},false);
 for(auto at:{F::D+0x610,F::D+0x614,F::D+0x658,F::D+0x65C})check([&](F&f){f.put<int32_t>(at,101);},false);
 check([](F&f){f.put<uint64_t>(F::D+0x650,0);},false);
 check([](F&f){f.put(F::AR+8,F::N+5*0x80);},false);
 check([](F&f){f.put<uint32_t>(F::N+0x40,4);},false);
 check([](F&f){f.put<uint32_t>(F::N+0x3C,16);},false);
 check([](F&f){f.put<uint32_t>(F::N+0x3C,16);f.put(F::N+0x28,std::array<float,3>{100,300,-200});},true);
 for(auto at:{F::D+0x878,F::D+0x87C,F::D+0x880,F::D+0x884,F::D+0x8B0,F::N+0x28})check([&](F&f){f.put<float>(at,std::numeric_limits<float>::quiet_NaN());},false);
 for(auto at:{F::M+0x2CA63B0,F::C+0x8D8,F::D+0x878,F::D+0x618,F::AR,F::N+0x1C,F::N+0x28,F::U+0x64C30})check([&](F&f){f.race=at;},false);
 for(uint64_t flags:{0x1E09000004ULL,0x1849000000ULL,0ULL})check([&](F&f){f.put(F::U+0x64C30,flags);},false);
 {F f;const auto v=botw::map_edit_snapshot(f.h);for(unsigned i=0;i<14;++i){assert(botw::map_edit_target(v,i)==(i>=6&&i<=10?int(i-6):5));assert(botw::map_edit_kind(i)==(i<6?int(i+27):i<=10?int(i+31):int(i+22)));}++cases;}
 {EditBridgeFixture f;assert(f.bridge.request_map(6,123,456));assert(f.get<uint32_t>(20)==9 && f.get<uint64_t>(680)==F::N && f.get<uint32_t>(8)==1);f.mb<uint32_t>(12,1);f.mb<uint32_t>(16,1);f.native_place();f.bridge.poll(f.h,{});assert(f.bridge.pending && std::strcmp(f.bridge.status,"Updating map")==0);f.save_place();f.bridge.poll(f.h,{});assert(!f.bridge.pending && std::strcmp(f.bridge.status,"Marker placed")==0);++cases;}
 for(uint32_t offset:{16,20,48,640,680,688,708,736,752,72,8}){EditBridgeFixture f;f.fail=offset;assert(!f.bridge.request_map(6,123,456));assert(!f.bridge.pending && f.get<uint32_t>(72)==0);++cases;}
 {EditBridgeFixture f;f.texts["__page"]="ready";assert(!f.bridge.request_map(6,123,456));++cases;}
 {EditBridgeFixture f;f.values["__mailbox_epoch"]=2;assert(!f.bridge.request_map(6,123,456));++cases;}
 {EditBridgeFixture f;f.bridge.sampled-=std::chrono::seconds(1);assert(!f.bridge.request_map(6,123,456));++cases;}
 {EditBridgeFixture f;assert(!f.bridge.request_map(14,123,456));assert(!f.bridge.request_map(6,NAN,456));++cases;}
 {EditBridgeFixture f;assert(f.bridge.request_map(6,123,456));f.values["__mailbox_epoch"]=2;f.mb<uint32_t>(4092,2);f.bridge.poll(f.h,{});assert(f.get<uint32_t>(72)==0);++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;state.map_palette=6;state.map_last_tap_seq=10;assert(on_action(&state,"map_tap",0));f.values["@map_tap_seq"]=11;f.floats["@map_tap_x"]=123;f.floats["@map_tap_y"]=-456;sample(&state,&f.h);assert(state.bridge.pending==1 && f.get<float>(712)==123 && f.get<float>(716)==456);++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;state.map_palette=6;state.map_last_tap_seq=10;assert(on_action(&state,"map_tap",0));f.values["@map_tap_seq"]=12;sample(&state,&f.h);assert(!state.bridge.pending);++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;state.map_palette=6;state.map_last_tap_seq=10;assert(on_action(&state,"map_tap",0));f.values["@map_tap_seq"]=11;f.floats["@map_tap_x"]=123;f.floats["@map_tap_y"]=-456;state.bridge.heartbeat_at-=std::chrono::milliseconds(150);sample(&state,&f.h);assert(!state.bridge.pending && state.map_tap_deferred && state.bridge.map_refusal==256);f.mb<uint32_t>(4,101);sample(&state,&f.h);assert(state.bridge.pending==1 && !state.map_tap_deferred);++cases;}
 {EditBridgeFixture f;f.native_place();f.save_place();State state;state.bridge=f.bridge;state.map_selected=0;state.map_selected_marker=botw::map_edit_snapshot(f.h).markers[0];assert(on_action(&state,"map_delete",0));assert(!state.bridge.pending && state.map_delete_deferred);sample(&state,&f.h);assert(state.bridge.pending && f.get<uint32_t>(708)==14);++cases;}
 {EditBridgeFixture f;f.native_place();f.save_place();State state;state.bridge=f.bridge;state.map_selected=0;state.map_selected_marker=botw::map_edit_snapshot(f.h).markers[0];assert(on_action(&state,"map_delete",0));f.values["__mailbox_epoch"]=2;f.mb<uint32_t>(4092,2);sample(&state,&f.h);assert(!state.bridge.pending && !state.map_delete_deferred && !f.get<uint32_t>(8));++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;state.map_selected=0;state.map_marker_deferred=true;assert(on_action(&state,"map_tap",0));assert(state.map_selected==-1 && !state.map_marker_deferred && !state.map_tap_deferred && !f.get<uint32_t>(8));++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;state.map_marker_deferred=true;assert(on_action(&state,"map_tool",6));assert(!state.map_marker_deferred && state.map_palette==6);++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;state.saved_icons=botw::saved_map_icons(f.h);assert(state.saved_icons.valid);state.map_palette=6;state.map_tap_deferred=true;assert(on_action(&state,"map_marker",0));assert(state.map_palette==-1 && !state.map_tap_deferred && state.map_marker_deferred && !f.get<uint32_t>(8));++cases;}
 {EditBridgeFixture f;State state;state.bridge=f.bridge;assert(on_action(&state,"map_tool",6));sample(&state,&f.h);assert(f.values["botw.map.edit.armed.6"]==1 && f.values["botw.map.edit.armed.7"]==0 && f.texts["botw.map.edit.hint"]=="Place Red pin · Tap map");assert(on_action(&state,"map_tool",-1));sample(&state,&f.h);assert(f.values["botw.map.edit.armed.6"]==0);++cases;}
 // Choosing the armed symbol again disarms it (replaces the Pan button); another symbol re-arms.
 {EditBridgeFixture f;State state;state.bridge=f.bridge;assert(on_action(&state,"map_tool",6));sample(&state,&f.h);assert(f.values["botw.map.edit.armed.6"]==1);
  assert(on_action(&state,"map_tool",6));sample(&state,&f.h);assert(f.values["botw.map.edit.armed.6"]==0&&f.values["botw.map.edit.tool"]==-1&&f.texts["botw.map.edit.hint"]=="Pinch to zoom · Drag to pan");
  assert(on_action(&state,"map_tool",6));assert(on_action(&state,"map_tool",2));sample(&state,&f.h);assert(f.values["botw.map.edit.armed.2"]==1&&f.values["botw.map.edit.armed.6"]==0);++cases;}
 std::cout<<cases<<" map snapshot identity/bounds/race/target cases passed\n";
}

#endif
