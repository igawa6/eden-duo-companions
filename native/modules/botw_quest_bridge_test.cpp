// SPDX-License-Identifier: GPL-3.0-or-later
#include "01007EF00011E000.cpp"
#include "botw_quest_test_fixture.h"
#include <cassert>
#include <iostream>
namespace {
struct QuestBridgeFixture:QuestFixture {
  static constexpr uint64_t Map=0xA000000;
  State state;
  std::array<uint8_t,4096> mailbox{};
  std::vector<uint32_t> stores;
  uint32_t fail=UINT32_MAX,change_epoch_at=UINT32_MAX;
  template<class T>T get(uint32_t at){T v{};std::memcpy(&v,mailbox.data()+at,sizeof(v));return v;}
  template<class T>void mb(uint32_t at,T v){std::memcpy(mailbox.data()+at,&v,sizeof(v));}
  QuestBridgeFixture() {
    texts["page"]="quest";values["aux"]=1;values["epoch"]=1;
    h.get_text=[](void* p,const char*)->const char*{return static_cast<QuestBridgeFixture*>(p)->texts["page"].c_str();};
    h.get_i64=[](void* p,const char* k,int64_t)->int64_t{auto& f=*static_cast<QuestBridgeFixture*>(p);return f.values[std::strcmp(k,"__mailbox_epoch")==0?"epoch":"aux"];};
    put<int32_t>(Destination+24,-1);put(M+0x2CA63B0,Map);put(Map,M+0x2B86848);put<uint64_t>(Map+0x3A0,0);put<int32_t>(M+0x2B7E8D0,33);
    mb<uint32_t>(4092,1);mb<uint32_t>(0,0x344F5544);mb<uint32_t>(4,100);
    EdenDsmodHostExtensions ext{};ext.version=EDEN_DSMOD_EXT_VERSION;ext.struct_size=sizeof(ext);ext.abi_hash=EDEN_DSMOD_EXT_HASH;ext.userdata=this;ext.mailbox_size=4096;
    ext.load_u32=[](void* p,uint32_t at,uint32_t* v)->EdenDsmodBool{auto& f=*static_cast<QuestBridgeFixture*>(p);assert(at%4==0&&at<=4092);*v=f.get<uint32_t>(at);return true;};
    ext.store_u32=[](void* p,uint32_t at,uint32_t v)->EdenDsmodBool{auto& f=*static_cast<QuestBridgeFixture*>(p);assert(at%4==0&&at<=4092);if(f.fail==at)return false;f.mb(at,v);f.stores.push_back(at);if(at==f.change_epoch_at){f.values["epoch"]++;f.mb(4092,uint32_t(f.values["epoch"]));}return true;};
    ext.store_u64=[](void* p,uint32_t at,uint64_t v)->EdenDsmodBool{auto& f=*static_cast<QuestBridgeFixture*>(p);assert(at%8==0&&at<=4088);if(f.fail==at)return false;f.mb(at,v);f.stores.push_back(at);if(at==f.change_epoch_at){f.values["epoch"]++;f.mb(4092,uint32_t(f.values["epoch"]));}return true;};
    state.bridge.configure(&ext);poll();state.quests=botw::quests(h);assert(state.quests.valid);state.quest_selected=Entry;
  }
  void poll(){state.bridge.poll(h,{});}
  bool choose(){return on_action(&state,"quest_activate",0);}
  void ack(uint32_t result=1){mb<uint32_t>(12,get<uint32_t>(8));mb(16,result);}
  void readback(){put<int32_t>(Destination+24,0);put(Map+0x3A0,Entry);}
};
} // namespace
#ifndef BOTW_QUEST_FIXTURE_ONLY
int main(){unsigned passed{};
  {QuestBridgeFixture f;assert(f.choose());assert(f.get<uint32_t>(8)==1&&f.get<uint32_t>(72)==1&&f.stores.back()==8);
   assert(f.get<uint32_t>(20)==5&&f.get<uint32_t>(24)==0&&f.get<uint64_t>(40)==f.Entry);
   assert(f.get<uint64_t>(96)==f.Q&&f.get<uint64_t>(104)==f.Map&&f.get<uint64_t>(112)==f.G&&f.get<uint32_t>(120)==33);
   assert(!f.choose());assert(std::string(f.state.bridge.status)=="Quest selection queued");++passed;}
  auto refuse=[&](auto change){QuestBridgeFixture f;change(f);const bool accepted=f.choose();if(accepted)std::cerr<<"Unexpected accepted case "<<passed+1<<"\n";assert(!accepted);assert(f.get<uint32_t>(8)==0);++passed;};
  refuse([](QuestBridgeFixture& f){f.texts["page"]="ready";});refuse([](QuestBridgeFixture& f){f.texts["page"]="inventory";});
  refuse([](QuestBridgeFixture& f){f.values["aux"]=0;});refuse([](QuestBridgeFixture& f){f.values["epoch"]=0;});
  refuse([](QuestBridgeFixture& f){f.values["epoch"]=2;});refuse([](QuestBridgeFixture& f){f.mb(4092,uint32_t(2));});
  refuse([](QuestBridgeFixture& f){f.mb(0,uint32_t(0x334F5544));f.poll();assert(f.state.bridge.healthy&&!f.state.bridge.quest_supported);});
  refuse([](QuestBridgeFixture& f){f.state.bridge.healthy=false;});
  refuse([](QuestBridgeFixture& f){f.state.bridge.sampled-=std::chrono::seconds(1);});
  refuse([](QuestBridgeFixture& f){f.state.bridge.heartbeat_at-=std::chrono::seconds(1);});
  for(auto flags:{0x1E09000004ULL,0x0E09E00000ULL,0x1849000000ULL,0ULL})refuse([&](QuestBridgeFixture& f){f.put(f.U+0x64C30,flags);});
  refuse([](QuestBridgeFixture& f){f.state.quests.valid=false;});refuse([](QuestBridgeFixture& f){f.state.quest_selected=0;});
  refuse([](QuestBridgeFixture& f){f.state.quests.entries[0].index=1;});refuse([](QuestBridgeFixture& f){f.state.quests.entries[0].name="Wrong";});
  refuse([](QuestBridgeFixture& f){f.state.quests.entries[0].message="Wrong";});refuse([](QuestBridgeFixture& f){f.state.quests.entries[0].state=2;});
  for(int n:{0,2})refuse([&](QuestBridgeFixture& f){f.put<int32_t>(f.Entry+12,n);});
  // Default fixture Type is "Main" (revealed at step 0 since item 16's fix); use a
  // Sub quest here to keep testing the side-quest "needs step>0" refusal path.
  refuse([](QuestBridgeFixture& f){f.string(f.Type,"Sub");f.put<int32_t>(f.Entry+0x140,0);});refuse([](QuestBridgeFixture& f){f.put<uint8_t>(f.Entry+0x30,0);});
  refuse([](QuestBridgeFixture& f){f.put<int32_t>(f.Destination+24,0);});
  for(uint32_t n:{2,4,6})refuse([&](QuestBridgeFixture& f){f.put(f.Entry+0xD8,n);f.put<uint8_t>(f.Dlc2+11,1);f.put<uint8_t>(f.Dlc3+11,1);});
  refuse([](QuestBridgeFixture& f){f.put<uint64_t>(f.Map,0);});refuse([](QuestBridgeFixture& f){f.put<uint64_t>(f.M+0x2CA63B0,0);});
  refuse([](QuestBridgeFixture& f){f.put<int32_t>(f.M+0x2B7E8D0,-1);});
  refuse([](QuestBridgeFixture& f){f.mb(12,uint32_t(1));});refuse([](QuestBridgeFixture& f){f.state.bridge.pending=8;});
  for(uint32_t at:{16,20,24,40,48,56,64,88,96,104,112,120,72,8})refuse([&](QuestBridgeFixture& f){f.fail=at;});
  for(uint32_t at:{88,96,104,112,120})refuse([&](QuestBridgeFixture& f){f.change_epoch_at=at;});
  {QuestBridgeFixture f;assert(f.choose());f.ack();f.poll();assert(f.state.bridge.pending&&f.get<uint32_t>(72)==0);assert(std::string(f.state.bridge.status)=="Awaiting native quest update");f.readback();f.poll();assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)=="Quest selected");++passed;}
  {QuestBridgeFixture f;assert(f.choose());f.ack(2);f.readback();f.poll();assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)=="Quest selection refused");++passed;}
  {QuestBridgeFixture f;assert(f.choose());f.ack();f.state.bridge.requested-=std::chrono::seconds(1);f.poll();assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)=="Quest selection not confirmed");++passed;}
  for(bool map_only:{false,true}){QuestBridgeFixture f;assert(f.choose());f.ack();if(map_only)f.put(f.Map+0x3A0,f.Entry);else f.put<int32_t>(f.Destination+24,0);f.poll();assert(f.state.bridge.pending);++passed;}
  for(int scope:{0,1,2,3}){QuestBridgeFixture f;assert(f.choose());if(scope==0)f.texts["page"]="ready";if(scope==1)f.values["aux"]=0;if(scope==2)f.put<uint64_t>(f.U+0x64C30,0x0E09E00000ULL);if(scope==3){f.values["epoch"]=2;f.mb(4092,uint32_t(2));}f.poll();assert(f.get<uint32_t>(72)==0);f.ack();f.readback();f.poll();assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)!="Quest selected");++passed;}
  {QuestBridgeFixture f;assert(f.choose());f.ack();f.readback();f.values["epoch"]=2;f.mb(4092,uint32_t(2));f.poll();assert(std::string(f.state.bridge.status)!="Quest selected");++passed;}
  {QuestBridgeFixture f;f.stores.clear();f.state.bridge.heartbeat_at-=std::chrono::milliseconds(150);assert(f.choose());assert(f.state.quest_deferred&&f.stores.empty());assert(!f.choose());sample(&f.state,&f.h);assert(f.state.quest_deferred&&f.get<uint32_t>(8)==0);f.mb(4,uint32_t(101));sample(&f.state,&f.h);assert(!f.state.quest_deferred&&f.get<uint32_t>(8)==1);++passed;}
  for(int mutation=0;mutation<12;++mutation){QuestBridgeFixture f;f.state.bridge.sampled-=std::chrono::milliseconds(120);assert(f.choose()&&f.state.quest_deferred);assert(f.get<uint32_t>(8)==0);
   if(mutation==0)f.texts["page"]="ready";
   if(mutation==1)f.values["aux"]=0;
   if(mutation==2){f.values["epoch"]=2;f.mb(4092,uint32_t(2));}
   if(mutation==3)f.put<uint64_t>(f.U+0x64C30,0x1E09000004ULL);
   if(mutation==4)f.state.quest_action_at-=std::chrono::milliseconds(251);
   if(mutation==5)f.state.last_sample=std::chrono::steady_clock::now()-std::chrono::milliseconds(251);
   if(mutation==6)f.put<int32_t>(f.Entry+12,2);
   if(mutation==7)f.put<int32_t>(f.Destination+24,0);
   if(mutation==8)f.state.quest_display.entries[0].name="Wrong";
   if(mutation==9)f.state.quest_display.entries[0].message="Wrong";
   if(mutation==10)assert(on_action(&f.state,"quest_category",1));
   if(mutation==11)assert(on_action(&f.state,"quest_choose",0));
   sample(&f.state,&f.h);assert(!f.state.quest_deferred&&f.get<uint32_t>(8)==0);++passed;}
  {QuestBridgeFixture f;f.state.bridge.sampled-=std::chrono::milliseconds(120);assert(f.choose());tick(&f.state,&f.h);assert(f.state.quest_deferred);f.values["aux"]=0;tick(&f.state,&f.h);assert(!f.state.quest_deferred&&f.get<uint32_t>(8)==0);++passed;}
  {QuestBridgeFixture f;f.state.bridge.sampled-=std::chrono::milliseconds(120);assert(f.choose());f.h.read_memory=[](void*,uint64_t,void*,size_t)->EdenDsmodBool{throw 1;};sample(&f.state,&f.h);assert(!f.state.quest_deferred&&f.get<uint32_t>(8)==0);++passed;}
  // Journal filters are local presentation state; indices follow filtered rows.
  auto journal=[](QuestBridgeFixture& f){
   const auto first=f.state.quests.entries.front();auto done=first;done.address+=0x1000;done.state=2;done.name="Completed";
   auto second=first;second.address+=0x2000;second.name="SecondActive";
   auto shrine=done;shrine.address+=0x3000;shrine.category=1;shrine.name="ShrineDone";
   f.state.quests.entries={first,done,second,shrine};f.state.quest_next=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  };
  // Iteration 12 item 10: native Adventure Log order -- one list per category, active quests first
  // then completed ones, each in native quest-data order; no completion filter action exists.
  {QuestBridgeFixture f;journal(f);sample(&f.state,&f.h);assert(f.values["botw.quest.count"]==3);
   assert(on_action(&f.state,"quest_choose",0));assert(f.state.quest_selected==f.Entry);
   assert(on_action(&f.state,"quest_choose",1));assert(f.state.quest_selected==f.Entry+0x2000);
   assert(on_action(&f.state,"quest_choose",2));assert(f.state.quest_selected==f.Entry+0x1000);
   assert(!on_action(&f.state,"quest_choose",3));sample(&f.state,&f.h);
   assert(f.values["botw.quest.item.2.complete"]==1&&f.values["botw.quest.item.0.complete"]==0&&f.values["botw.quest.item.2.open"]==0&&f.values["botw.quest.item.2.lit"]==0&&f.values["botw.quest.item.0.lit"]==1&&f.values["botw.quest.item.3.lit"]==0);
   assert(!f.texts.contains("botw.quest.filter.title")&&!f.values.contains("botw.quest.completed"));assert(!f.get<uint32_t>(8));++passed;}
  {QuestBridgeFixture f;journal(f);assert(!on_action(&f.state,"quest_completed",0));
   assert(on_action(&f.state,"quest_category",1));assert(!f.state.quest_selected);assert(on_action(&f.state,"quest_choose",0));assert(f.state.quest_selected==f.Entry+0x4000);
   assert(!on_action(&f.state,"quest_choose",1));sample(&f.state,&f.h);assert(f.values["botw.quest.count"]==1&&!f.get<uint32_t>(8));++passed;}
  std::cout<<passed<<" quest action freshness/scope/queue/native-readback cases passed\n";
}

#endif
