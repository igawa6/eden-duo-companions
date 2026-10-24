// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_quick_slot_bridge.h"
#include <cstdlib>
#include <iostream>
using namespace echoes;
int main(){
 size_t n=0;const auto check=[&](bool b){if(!b){std::cerr<<"failed "<<n+1<<'\n';std::exit(1);}++n;};
 const auto record=[&](QuickSlotFlagBridge& b,uint16_t v){for(int bit=15;bit>=0;--bit){const int p=bit*2+((v>>bit)&1);check(b.RestorePacket(p));check(b.RestorePacket(p));}};
 QuickSlotFlagBridge b;QuickSlotFlagBridge::Restored out{9,8,7};const auto before=out;
 check(!b.QueueWrites({1,2,3}));check(!b.AcknowledgeWrite(0));
 const std::array<uint16_t,3> values{0,1,65535};
 for(size_t i=0;i<3;++i){check(b.restore_slot()==i);check(!b.AdvanceRestore(out));check(out==before);record(b,values[i]);check(b.restore_slot()==i);check(b.AdvanceRestore(out)==(i==2));if(i<2)check(out==before);}
 check(!out[0]&&out[1]==0&&out[2]==65534);check(!b.restore_pending());
 out={4,5,6};check(b.AdvanceRestore(out));check(out==QuickSlotFlagBridge::Restored{4,5,6});
 check(b.QueueWrites({65535,0,65534})&&!b.writes_pending());
 check(b.QueueWrites({2,3,4}));for(size_t i=0;i<3;++i){check(b.write_pending(i));check(b.write_value(i)==int64_t(i+3));}
 check(!b.AcknowledgeWrite(3));check(b.writes_pending());
 check(b.AcknowledgeWrite(0));check(!b.write_pending(0));check(b.AcknowledgeWrite(0));
 check(b.QueueWrites({2,0,65534}));check(!b.writes_pending());
 check(b.QueueWrites({65535,0,65534}));check(b.write_pending(0)&&b.write_value(0)==0);check(b.AcknowledgeWrite(0));
 for(const int p:{-1,32,999}){QuickSlotFlagBridge bad;auto old=out;check(!bad.RestorePacket(p));check(!bad.AdvanceRestore(out));check(old==out);check(!bad.QueueWrites({1,2,3}));}
 QuickSlotFlagBridge conflict;check(conflict.RestorePacket(0));check(!conflict.RestorePacket(1));check(conflict.status()==QuickSlotRestoreStatus::Refused);
 QuickSlotFlagBridge partial;record(partial,0);check(!partial.AdvanceRestore(out));check(!partial.QueueWrites({1,2,3}));check(!partial.AcknowledgeWrite(0));
 std::cout<<n<<" quick-slot bridge checks passed\n";
}
