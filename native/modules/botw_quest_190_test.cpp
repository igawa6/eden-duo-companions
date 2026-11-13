// SPDX-License-Identifier: GPL-3.0-or-later
#define BOTW_190_INVENTORY_EVIDENCE
#define BOTW_190_READY_EVIDENCE
#define BOTW_190_QUEST_EVIDENCE
#define BOTW_QUEST_FIXTURE_ONLY
#include "botw_quest_bridge_test.cpp"
#include "botw_inventory_update_fixture.h"
namespace {
struct Quest190Fixture:QuestBridgeFixture {
  static constexpr uint64_t Dlc=0xB000000;
  Quest190Fixture() {
    update_fixture(*this);
    const auto& q=botw::Quests190;const auto& r=botw::Ready190;
    for(auto off:{0x2CD1700,0x2CA63B0})for(unsigned i=0;i<8;++i)mem.erase(M+off+i);
    for(unsigned i=0;i<4;++i)mem.erase(M+0x2B7E8D0+i);
    put(M+q.root,Q);put(Q,M+q.manager_vtable);put(Entry,M+q.quest_vtable);put(Step,M+q.step_vtable);
    put(M+r.map_root,Map);put(Map,M+r.map_vtable);put<int32_t>(M+q.destination_handle,33);
    put<uint8_t>(G+0xBE8,0);put<uint32_t>(G+0xC18,0);put<uint32_t>(G+0xC28,0);
    put(M+q.dlc_root,Dlc);put(Dlc,M+q.dlc_vtable);put<uint32_t>(Dlc+0xD0,0xDEADBEEF);put<uint32_t>(Dlc+q.dlc_version,0x300);
    state.quests=botw::quests(h);assert(state.quests.valid);poll();
  }
};
} // namespace
int main() {
  using F=Quest190Fixture;
  {F f;assert(!botw::supported(f.h)&&botw::quests_supported(f.h));
    auto q=botw::quests(f.h);assert(q.valid&&q.entries.size()==1&&q.entries[0].name=="GanonQuest"&&q.entries[0].message=="Desc");
    const auto dlc=botw::tracker_dlc_sample(f.h);assert(dlc.valid&&dlc.version==0x300);
    botw::TrackerFlagReader flags;assert(flags.begin(f.h));assert(flags.integer(0x5ED42D75)==-1);assert(flags.boolean(0x7609DE2C)==false);assert(flags.finish());
    assert(f.choose());assert(f.get<uint64_t>(96)==f.Q&&f.get<uint64_t>(104)==f.Map&&f.get<uint32_t>(120)==33);
    f.ack();f.poll();assert(f.state.bridge.pending);f.readback();f.poll();assert(!f.state.bridge.pending&&std::string(f.state.bridge.status)=="Quest selected");
  }
  for(int mismatch=0;mismatch<3;++mismatch){F f;assert(f.choose());f.ack();f.readback();
    if(mismatch==0)f.put<int32_t>(f.Destination+24,-1);
    if(mismatch==1)f.put<uint64_t>(f.Map+0x3A0,0);
    if(mismatch==2){f.values["epoch"]=2;f.mb<uint32_t>(4092,2);}
    f.poll();assert(std::string(f.state.bridge.status)!="Quest selected");}
  {F f;f.put<uint64_t>(f.Step,0);assert(!botw::quests(f.h).valid);assert(!f.choose());}
  {F f;botw::TrackerFlagReader flags;assert(flags.begin(f.h));assert(flags.integer(0x5ED42D75)==-1);f.put<int32_t>(f.Destination+24,2);assert(!flags.finish());}
  {F f;f.put(F::Dlc,f.M+botw::Quests160.dlc_vtable);assert(!botw::tracker_dlc_sample(f.h).valid);}
  {F f;f.race=f.Q;assert(!botw::quests(f.h).valid);}
  {F f;f.h.build_id[0]^=1;assert(!botw::quests_supported(f.h));assert(!botw::quests(f.h).valid);}
  std::cout<<"1.9 quest identity, DLC shifted version, stable flags and two-part native completion passed\n";
}
