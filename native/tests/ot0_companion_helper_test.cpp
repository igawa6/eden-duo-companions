// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_battle_signature.h"
#include "ot0_companion_helper.h"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>
using namespace ot0;
namespace ot0 {
std::optional<HudRow> FindMinimapHud(const ReadMemory &, uint64_t, int64_t,
                                     bool) {
  return {};
}
std::optional<HudRow> FindMinimapCanvas(const ReadMemory &, uint64_t, int64_t,
                                        bool) {
  return {};
}
} // namespace ot0
struct Mock {
  Ot0CompanionMailbox box{};
  std::vector<std::pair<uint32_t, uint64_t>> writes;
};
EdenDsmodBool l32(void *p, uint32_t o, uint32_t *v) {
  std::memcpy(v, (char *)&static_cast<Mock *>(p)->box + o, 4);
  return EDEN_DSMOD_TRUE;
}
EdenDsmodBool l64(void *p, uint32_t o, uint64_t *v) {
  std::memcpy(v, (char *)&static_cast<Mock *>(p)->box + o, 8);
  return EDEN_DSMOD_TRUE;
}
EdenDsmodBool s32(void *p, uint32_t o, uint32_t v) {
  auto &m = *static_cast<Mock *>(p);
  m.writes.emplace_back(o, v);
  std::memcpy((char *)&m.box + o, &v, 4);
  return EDEN_DSMOD_TRUE;
}
EdenDsmodBool s64(void *p, uint32_t o, uint64_t v) {
  auto &m = *static_cast<Mock *>(p);
  m.writes.emplace_back(o, v);
  std::memcpy((char *)&m.box + o, &v, 8);
  return EDEN_DSMOD_TRUE;
}
int main() {
  Mock m;
  m.box.lease_seq = 10;
  m.box.query_seq = 26;
  m.box.epoch = 3;
  m.box.helper_version = 1;
  m.box.heartbeat = 100;
  EdenDsmodHostExtensions ext{};
  ext.version = EDEN_DSMOD_EXT_VERSION;
  ext.struct_size = sizeof ext;
  ext.abi_hash = EDEN_DSMOD_EXT_HASH;
  ext.userdata = &m;
  ext.mailbox_address = 0x100000;
  ext.mailbox_size = sizeof m.box;
  ext.load_u32 = l32;
  ext.load_u64 = l64;
  ext.store_u32 = s32;
  ext.store_u64 = s64;
  NativeCompanionHelper helper;
  helper.Configure(ext);
  assert(!helper.Poll().live);
  ++m.box.heartbeat;
  assert(helper.Poll().live);
  helper.Stop();
  assert(m.box.lease_seq == 12 && m.box.desired_hide == 0);
  assert(m.writes.front() ==
         std::make_pair(uint32_t(offsetof(Ot0CompanionMailbox, lease_seq)),
                        uint64_t(11)));
  assert(m.writes.back() ==
         std::make_pair(uint32_t(offsetof(Ot0CompanionMailbox, lease_seq)),
                        uint64_t(12)));
  assert(m.box.lease_deadline == 221);
  for (const auto &[o, v] : m.writes)
    assert(o < offsetof(Ot0CompanionMailbox, reserved_tail));
  BattleStatsSnapshot in{};
  in.valid = true;
  in.party[0].parameters[0] = {99, true};
  in.enemies[0].parameters[2] = {88, true};
  auto out = helper.Query([](uint64_t, void *, size_t) { return false; },
                          0x80000000, 1, in, 900, 0);
  assert(!out.party[0].parameters[0].valid &&
         !out.enemies[0].parameters[2].valid);
  std::map<uint64_t, uint8_t> mem;
  auto put = [&](uint64_t a, auto v) {
    for (size_t i = 0; i < sizeof(v); ++i)
      mem[a + i] = reinterpret_cast<const uint8_t *>(&v)[i];
  };
  uint64_t main = 0x80000000, app = 0x100000000, bm = 0x100001000,
           actor = 0x100002000, arr = 0x100003000, chunks = 0x100004000,
           chunk = 0x100005000;
  put(main + 0x7cc8cc0, app);
  put(app, main + 0x6207610);
  put(app + 0x2f0, uint8_t(26));
  put(main + 0x7cc5b78, bm);
  put(bm + 8, uint32_t(0));
  battle_detail::Array empty{}, enemies{arr, 1, 1};
  put(bm + 0x9b8, empty);
  put(bm + 0x9c8, empty);
  put(bm + 0xa98, enemies);
  put(arr, actor);
  put(actor, main + 0x63f39b8);
  put(actor + 8, uint32_t(0));
  put(app + 8, uint32_t(0));
  put(actor + 0x293, uint8_t(1));
  put(actor + 0x298, uint32_t(123));
  put(actor + 0x2a8, uint8_t(0));
  put(actor + 0x800, empty);
  put(actor + 0xc, int32_t(0));
  put(main + 0x6ce97e8 + 0x24, int32_t(1));
  put(main + 0x6ce97e8 + 0x10, chunks);
  put(chunks, chunk);
  std::array<uint8_t, 20> item{};
  std::memcpy(item.data(), &actor, 8);
  uint32_t serial = 9;
  std::memcpy(item.data() + 16, &serial, 4);
  put(chunk, item);
  ReadMemory rd = [&](uint64_t a, void *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      auto it = mem.find(a + i);
      if (it == mem.end())
        return false;
      static_cast<uint8_t *>(p)[i] = it->second;
    }
    return true;
  };
  auto sig = battle_detail::ReadActorSignature({rd, main, 0}, actor);
  assert(sig.valid);
  BattleStatsSnapshot stats{};
  stats.valid = true;
  auto &enemy = stats.enemies[0];
  enemy.bound = enemy.signature_valid = true;
  enemy.id = 123;
  enemy.native_actor = actor;
  enemy.native_serial = serial;
  enemy.raw_ailment_fingerprint = sig.raw_ailment_fingerprint;
  assert(!enemy.weapon_context_valid);
  auto waiting = helper.Query(rd, main, 0, stats, 0, 0);
  assert(m.box.query_mask == 256 && m.box.query_seq == 28 &&
         !waiting.enemies[0].parameters[0].valid);
  auto &row = m.box.rows[8];
  row.actor = actor;
  row.id = 123;
  row.team = 1;
  row.slot = 8;
  row.valid_mask = 255;
  row.serial = serial;
  row.ailment_fingerprint = sig.raw_ailment_fingerprint;
  for (int i = 0; i < 8; ++i)
    row.values[i] = 100 + i;
  m.box.query_bm = bm;
  m.box.query_ack = m.box.query_seq;
  m.box.query_result = OT0_NATIVE_OK;
  ++m.box.heartbeat;
  auto fresh = helper.Query(rd, main, 0, stats, 0, 0);
  assert(fresh.enemies[0].parameters[0].valid &&
         fresh.enemies[0].parameters[0].value == 100 && m.box.query_seq == 30);
  ++m.box.heartbeat;
  auto cached = helper.Query(rd, main, 0, stats, 0, 0);
  assert(cached.enemies[0].parameters[7].valid &&
         cached.enemies[0].parameters[7].value == 107);
  m.box.heartbeat += 13;
  auto expired = helper.Query(rd, main, 0, stats, 0, 0);
  assert(!expired.enemies[0].parameters[0].valid);
  // Serial zero permits a native initialization request, never a displayed ACK.
  uint32_t zero = 0;
  std::memcpy(item.data() + 16, &zero, 4);
  put(chunk, item);
  battle_detail::SerialSample unallocated{};
  assert(!battle_detail::Serial({rd, main, 0}, actor, unallocated));
  assert(battle_detail::Serial({rd, main, 0}, actor, unallocated, true));
  enemy.signature_valid = false;
  enemy.native_serial = 0;
  auto warming = helper.Query(rd, main, 0, stats, 0, 0);
  assert(m.box.query_mask == 256 && !warming.enemies[0].parameters[0].valid);
  row.actor = actor;
  row.id = 123;
  row.team = 1;
  row.slot = 8;
  row.serial = 0;
  row.valid_mask = 255;
  row.error = 0;
  row.ailment_fingerprint = 0;
  row.weapon_selector = 0;
  m.box.query_ack = m.box.query_seq;
  m.box.query_result = OT0_NATIVE_OK;
  ++m.box.heartbeat;
  auto zero_ack = helper.Query(rd, main, 0, stats, 0, 0);
  assert(!zero_ack.enemies[0].parameters[0].valid);
  // Native serial allocation makes the first warmup ACK intentionally mismatch.
  std::memcpy(item.data() + 16, &serial, 4);
  put(chunk, item);
  row.serial = serial;
  row.ailment_fingerprint = sig.raw_ailment_fingerprint;
  m.box.query_ack = m.box.query_seq;
  ++m.box.heartbeat;
  auto first_positive = helper.Query(rd, main, 0, stats, 0, 0);
  assert(!first_positive.enemies[0].parameters[0].valid);
  enemy.signature_valid = true;
  enemy.native_serial = serial;
  auto normal_request = helper.Query(rd, main, 0, stats, 0, 0);
  assert(!normal_request.enemies[0].parameters[0].valid);
  m.box.query_ack = m.box.query_seq;
  ++m.box.heartbeat;
  auto after_warmup = helper.Query(rd, main, 0, stats, 0, 0);
  assert(after_warmup.enemies[0].parameters[0].valid);
  // Zero-serial opt-in still rejects invalid GU flags and recycled objects.
  std::memcpy(item.data() + 16, &zero, 4);
  uint32_t badflags = 0x20000000;
  std::memcpy(item.data() + 8, &badflags, 4);
  put(chunk, item);
  assert(!battle_detail::Serial({rd, main, 0}, actor, unallocated, true));
  std::memset(item.data() + 8, 0, 4);
  uint64_t other = actor + 0x100;
  std::memcpy(item.data(), &other, 8);
  put(chunk, item);
  assert(!battle_detail::Serial({rd, main, 0}, actor, unallocated, true));
  ++m.box.epoch;
  assert(!helper.Poll().live);
  ++m.box.heartbeat;
  assert(helper.Poll().live);
  m.box.helper_version = 2;
  assert(!helper.Poll().live);
  helper.SetMapLease({}, 0, 0, nullptr, false);
  assert(!m.box.desired_hide && !(m.box.lease_seq & 1));
}
