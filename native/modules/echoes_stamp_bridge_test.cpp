// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_stamp_bridge.h"
#include <cstdlib>
#include <iostream>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool ok,const char*why){++checks;if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
void Record(StampFlagBridge&bridge,int64_t value){
  for(int bit=39;bit>=0;--bit){const auto packet=bit*2+int((uint64_t(value)>>bit)&1);Check(bridge.RestorePacket(packet),"valid bit packet accepted");Check(bridge.RestorePacket(packet),"duplicate equal bit idempotent");}
}
void Restore(){
 StampStore wanted;wanted.Add(StampKind::RedFlag,{.2,.3});wanted.Add(StampKind::Heart,{1,0});wanted.Add(StampKind::Diamond,{0,1});const auto records=wanted.Packed();
 StampStore out;out.Add(StampKind::Chest,{.5,.5});const auto previous=out.Packed();StampFlagBridge bridge;
 Check(!bridge.QueueWrites(out),"writes cannot overwrite flags before restoration");
 for(size_t slot=0;slot<StampCapacity;++slot){Check(bridge.restore_slot()==slot&&bridge.restore_pending(),"one stable slot published per restore tick");Check(!bridge.AdvanceRestore(out),"incomplete packet cannot advance");Check(out.Packed()==previous,"store unchanged before all records arrive");Record(bridge,records[slot]);Check(bridge.restore_slot()==slot,"packets never advance slot mid-enforce");const auto ready=bridge.AdvanceRestore(out);Check(ready==(slot+1==StampCapacity),"last complete record alone finalizes restore");if(slot+1<StampCapacity)Check(out.Packed()==previous,"partial batch never reaches live store");}
 Check(bridge.status()==StampRestoreStatus::Ready&&out.Packed()==records,"all24 records replace store atomically");
 Check(bridge.AdvanceRestore(out),"ready advance idempotent");Check(bridge.QueueWrites(out)&&!bridge.writes_pending(),"restored baseline produces no disk rewrites");
 out.Remove(0);out.Add(StampKind::BlueFlag,{.4,.6});Check(bridge.QueueWrites(out)&&bridge.write_pending(0),"changed existing slot queued");Check(bridge.write_value(0)==out.Packed()[0],"exact single positive int64 write value");Check(bridge.AcknowledgeWrite(0)&&!bridge.writes_pending(),"derived equality ACK retires write");Check(bridge.AcknowledgeWrite(0),"duplicate ACK idempotent");
 out.Remove(0);Check(bridge.QueueWrites(out)&&bridge.write_pending(0)&&bridge.write_value(0)==0,"deletion queues canonical tombstone");Check(!bridge.AcknowledgeWrite(24)&&bridge.write_pending(0),"out of range ACK cannot retire write");bridge.AcknowledgeWrite(0);
 out.Add(StampKind::Chest,{.7,.2});bridge.QueueWrites(out);out.Remove(0);bridge.QueueWrites(out);Check(!bridge.write_pending(0),"reverting to acknowledged value cancels unneeded write");
}
void Refusal(){
 for(const auto packet:{int64_t(-1),int64_t(80),int64_t(9000)}){StampFlagBridge bridge;StampStore out;out.Add(StampKind::Heart,{.1,.2});const auto before=out.Packed();Check(!bridge.RestorePacket(packet)&&bridge.status()==StampRestoreStatus::Refused,"out of range transport packet refuses");Check(!bridge.AdvanceRestore(out)&&out.Packed()==before,"failed transport preserves prior store");Check(!bridge.QueueWrites(out),"refused restore never writes defaults over old flags");}
 StampFlagBridge conflict;Check(conflict.RestorePacket(0),"first clear bit");Check(!conflict.RestorePacket(1)&&conflict.status()==StampRestoreStatus::Refused,"conflicting bit refuses whole restore");
 StampFlagBridge unknown;StampStore out;out.Add(StampKind::Heart,{.1,.2});const auto before=out.Packed();
 for(size_t slot=0;slot<23;++slot){Record(unknown,0);Check(!unknown.AdvanceRestore(out),"empty record stages without clearing live data");}
 Record(unknown,INT64_C(1)<<39);Check(!unknown.AdvanceRestore(out)&&unknown.status()==StampRestoreStatus::Refused&&out.Packed()==before,"malformed final native record rejects entire batch");
}
}
int main(){Restore();Refusal();std::cout<<checks<<" stamp flag bridge checks passed\n";}
