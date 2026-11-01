// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_field_stats.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
namespace {
struct Mock {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(4096);
  uint32_t Get(uint32_t o) { uint32_t v; std::memcpy(&v, &bytes[o], 4); return v; }
  void Put(uint32_t o, uint32_t v) { std::memcpy(&bytes[o], &v, 4); }
  EdenDsmodHostExtensions Ext() {
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION; e.struct_size = sizeof e; e.abi_hash = EDEN_DSMOD_EXT_HASH;
    e.userdata = this; e.mailbox_address = 0x100000; e.mailbox_size = 4096;
    e.load_u32 = [](void *p, uint32_t o, uint32_t *v) -> EdenDsmodBool { *v = static_cast<Mock *>(p)->Get(o); return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void *p, uint32_t o, uint32_t v) -> EdenDsmodBool { static_cast<Mock *>(p)->Put(o, v); return EDEN_DSMOD_TRUE; };
    return e;
  }
  void Answer(uint32_t id, uint32_t stamp) { // what the helper writes (native order 9..16)
    Put(OT0_STATS_OFFSET, OT0_STATS_VERSION);
    for (int i = 0; i < 8; ++i) Put(OT0_STATS_OFFSET + 16 + 4 * i, 100 + i); // id 9+i -> 100+i
    Put(OT0_STATS_OFFSET + 12, stamp);
    Put(OT0_STATS_OFFSET + 8, id);
  }
};
} // namespace
int main() {
  Mock m;
  ot0::NativeStats s;
  s.Configure(m.Ext());
  m.Put(0, 1000);
  assert(!s.Poll(904).valid && m.Get(OT0_STATS_OFFSET + 4) == 904); // request written
  m.Answer(904, 995);
  auto r = s.Poll(904);
  // Status order: 9,11,10,12,15,13,14,16 -> 100,102,101,103,106,104,105,107
  assert(r.valid && r.id == 904 && r.values[0] == 100 && r.values[1] == 102 && r.values[2] == 101 &&
         r.values[4] == 106 && r.values[5] == 104 && r.values[7] == 107);
  m.Put(0, 2000); // an earlier valid result must not bypass expiry
  assert(!s.Poll(904).valid);
  m.Put(0, 1000);
  m.Put(OT0_STATS_OFFSET, 2);
  assert(!s.Poll(904).valid); // nor survive helper ABI refusal
  m.Put(OT0_STATS_OFFSET, OT0_STATS_VERSION);
  assert(!s.Poll(10503).valid && m.Get(OT0_STATS_OFFSET + 4) == 10503); // other member: not yet
  m.Answer(10503, 990);
  m.Put(0, 2000); // stale answer (helper stopped)
  assert(!s.Poll(10503).valid);
  m.Put(OT0_STATS_OFFSET, 2); // wrong version
  m.Put(0, 995);
  assert(!s.Poll(10503).valid);
  std::cout << "ot0 field stats ok\n";
}
