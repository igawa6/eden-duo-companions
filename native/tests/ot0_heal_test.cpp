// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_heal.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
using namespace ot0;
namespace {
constexpr uint32_t Base = 2048;
#define H(field) (Base + uint32_t(offsetof(Ot0HealMailbox, field)))
struct Write {
  uint32_t offset;
  uint64_t value;
  unsigned size;
};
struct Mock {
  std::array<uint8_t, 4096> bytes{};
  std::vector<Write> writes;
  bool fail_read{}, fail_write{}, tear_epoch{};
  template <class T> T Get(uint32_t at) const {
    T v{};
    assert(at + sizeof v <= bytes.size());
    std::memcpy(&v, bytes.data() + at, sizeof v);
    return v;
  }
  template <class T> void Put(uint32_t at, T v) {
    assert(at + sizeof v <= bytes.size());
    std::memcpy(bytes.data() + at, &v, sizeof v);
  }
  Ot0HealMailbox Box() const {
    Ot0HealMailbox b{};
    std::memcpy(&b, bytes.data() + Base, sizeof b);
    return b;
  }
  void Box(const Ot0HealMailbox &b) {
    std::memcpy(bytes.data() + Base, &b, sizeof b);
  }
  Mock() {
    Put(0, 100u);
    Put(4, 3u);
    Put(H(version), 2u);
    Put(H(epoch), 3u);
  }
  void Tick(uint32_t count = 1) {
    Put(0, Get<uint32_t>(0) + count);
    Put(H(heartbeat), Get<uint32_t>(0));
  }
  void AckOffer(uint32_t target = 904) {
    auto b = Box();
    b.heartbeat = Get<uint32_t>(0);
    b.epoch = Get<uint32_t>(4);
    b.ack = b.seq;
    b.result = OT0_HEAL_QUERY_OK;
    b.caster = 10804;
    b.skill = 900099;
    b.target = target;
    b.cost = 6;
    b.target_hp = 727;
    b.caster_sp = 68;
    b.target_max_hp = 765;
    b.native_eligible = 1;
    b.app_state = 23;
    b.version = 2;
    b.expected_hp = 727;
    b.expected_sp = 68;
    b.expected_cost = 6;
    b.expected_max_hp = 765;
    b.expected_save = 0x100000000;
    b.expected_avatar = 0x200000000;
    b.expected_party_hash = 0x123456789abcdef0ULL;
    b.result_heartbeat = Get<uint32_t>(0);
    b.skill_text_id = 12345;
    Box(b);
  }
  EdenDsmodHostExtensions Ext() {
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION;
    e.struct_size = sizeof e;
    e.abi_hash = EDEN_DSMOD_EXT_HASH;
    e.userdata = this;
    e.mailbox_address = 0x100000;
    e.mailbox_size = bytes.size();
    e.load_u32 = [](void *p, uint32_t o, uint32_t *v) -> EdenDsmodBool {
      auto &m = *static_cast<Mock *>(p);
      if (m.fail_read)
        return EDEN_DSMOD_FALSE;
      *v = m.Get<uint32_t>(o);
      if (m.tear_epoch && o == 4)
        m.Put(4, *v + 1);
      return EDEN_DSMOD_TRUE;
    };
    e.load_u64 = [](void *p, uint32_t o, uint64_t *v) -> EdenDsmodBool {
      auto &m = *static_cast<Mock *>(p);
      if (m.fail_read)
        return EDEN_DSMOD_FALSE;
      *v = m.Get<uint64_t>(o);
      return EDEN_DSMOD_TRUE;
    };
    e.store_u32 = [](void *p, uint32_t o, uint32_t v) -> EdenDsmodBool {
      auto &m = *static_cast<Mock *>(p);
      if (m.fail_write)
        return EDEN_DSMOD_FALSE;
      m.writes.push_back({o, v, 4});
      m.Put(o, v);
      return EDEN_DSMOD_TRUE;
    };
    e.store_u64 = [](void *p, uint32_t o, uint64_t v) -> EdenDsmodBool {
      auto &m = *static_cast<Mock *>(p);
      if (m.fail_write)
        return EDEN_DSMOD_FALSE;
      m.writes.push_back({o, v, 8});
      m.Put(o, v);
      return EDEN_DSMOD_TRUE;
    };
    return e;
  }
};
void Live(Mock &m, NativeHeal &h) {
  h.Configure(m.Ext());
  assert(!h.Poll().live);
  m.Tick();
  assert(h.Poll().live);
}
void Ready(Mock &m, NativeHeal &h) {
  Live(m, h);
  assert(h.Find(901));
  m.Tick();
  m.AckOffer();
  auto s = h.Poll();
  assert(s.phase == HealPhase::Ready && s.ready && s.live);
  assert(s.target == 904 && s.caster == 10804 && s.skill == 900099 &&
         s.text_id == 12345);
  assert(s.cost == 6 && s.hp == 727 && s.max_hp == 765 && s.sp == 68);
}
void TestLifecycle() {
  Mock m;
  NativeHeal h;
  Ready(m, h);
  const auto offer = m.Box();
  assert(m.writes.front().offset == H(seq) && (m.writes.front().value & 1));
  assert(m.writes.back().offset == H(seq) && !(m.writes.back().value & 1));
  m.writes.clear();
  assert(h.Apply());
  auto req = m.Box();
  assert(req.op == OT0_HEAL_APPLY && req.seq != offer.seq &&
         req.ack == offer.ack);
  assert(req.caster == offer.caster && req.skill == offer.skill &&
         req.target == offer.target);
  assert(req.expected_hp == offer.expected_hp &&
         req.expected_sp == offer.expected_sp);
  assert(req.expected_cost == offer.expected_cost &&
         req.expected_max_hp == offer.expected_max_hp);
  assert(req.expected_save == offer.expected_save &&
         req.expected_avatar == offer.expected_avatar);
  assert(req.expected_party_hash == offer.expected_party_hash);
  assert(m.writes.front().offset == H(seq) && (m.writes.front().value & 1));
  assert(m.writes.back().offset == H(seq) && !(m.writes.back().value & 1));
  const auto count = m.writes.size();
  assert(!h.Apply());
  assert(!h.Find(10503));
  assert(m.writes.size() == count && h.Poll().phase == HealPhase::Applying);
  m.Tick();
  req.ack = req.seq;
  req.result = OT0_HEAL_APPLIED;
  req.executed_seq = req.seq;
  req.heartbeat = m.Get<uint32_t>(0);
  req.result_heartbeat = req.heartbeat;
  req.result_hp = 765;
  req.result_sp = 62;
  m.Box(req);
  auto s = h.Poll();
  assert(s.phase == HealPhase::Done && !s.ready);
  assert(s.after_hp == 765 && s.after_sp == 62 && s.result == OT0_HEAL_APPLIED);
  assert(!h.Apply());
  assert(m.writes.size() == count);
  for (const auto &w : m.writes)
    assert(w.offset >= Base &&
           w.offset + w.size <= Base + sizeof(Ot0HealMailbox));
  h.Stop();
  assert(m.Get<uint32_t>(H(seq)) & 1);
  assert(m.Get<uint32_t>(H(op)) == 0);
  assert(h.Poll().phase == HealPhase::Idle && !h.Poll().ready);
  assert(!h.Find(10503));
  m.Tick();
  // Stop can be reused after the previous native invocation has drained.
  assert(h.Find(10503));
}
void TestPendingQueryDrain() {
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    const auto request = m.Box();
    const auto writes = m.writes.size();
    assert(!h.Find(10503));
    assert(m.writes.size() == writes && m.Box().seq == request.seq &&
           m.Box().target == 904);
    h.Cancel();
    const auto withdrawn = m.Box().seq;
    const auto cancelled_writes = m.writes.size();
    assert(withdrawn & 1);
    // An already-running FIND may still finish output/ACK writes after the
    // host invalidates it. It must not race with a newly published request.
    m.Put(H(target), 904u);
    m.Put(H(expected_hp), 727u);
    m.Put(H(ack), request.seq);
    assert(!h.Find(10503));
    assert(m.writes.size() == cancelled_writes && m.Box().seq == withdrawn);
    m.Tick();
    assert(h.Find(10503));
    assert(m.Box().target == 10503 && m.Box().seq != request.seq &&
           !(m.Box().seq & 1) && m.Box().expected_hp == 0);
  }
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    m.Tick(121);
    assert(h.Poll().result == OT0_HEAL_EXPIRED);
    const auto writes = m.writes.size();
    assert(!h.Find(10503) && m.writes.size() == writes);
    m.Tick();
    assert(h.Find(10503));
  }
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    m.Put(4, 4u);
    m.Put(H(epoch), 4u);
    assert(!h.Poll().live && !h.Find(10503));
    m.Tick();
    assert(h.Find(10503));
  }
}
void TestExpiryAndEpoch() {
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    m.Tick(121);
    auto s = h.Poll();
    assert(!s.ready && s.phase == HealPhase::Unavailable &&
           s.result == OT0_HEAL_EXPIRED);
    assert(!h.Apply());
  }
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    m.Tick(121);
    assert(h.Poll().result == OT0_HEAL_EXPIRED && (m.Box().seq & 1));
  }
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    m.Put(4, 4u);
    m.Put(H(epoch), 4u);
    auto s = h.Poll();
    assert(!s.live && !s.ready && s.result == OT0_HEAL_EXPIRED);
    m.Tick();
    assert(h.Poll().live);
    assert(!h.Apply());
    assert(h.Find(904));
  }
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    assert(h.Apply());
    const auto seq = m.Box().seq;
    m.Tick(121);
    auto s = h.Poll();
    assert(s.result == OT0_HEAL_EXPIRED);
    assert(!h.Apply() && m.Box().seq != seq && (m.Box().seq & 1));
  }
}
void TestUntrustedResults() {
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    m.AckOffer(10503);
    assert(h.Poll().result == OT0_HEAL_STALE && !h.Apply());
  }
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    m.AckOffer();
    auto b = m.Box();
    --b.ack;
    m.Box(b);
    assert(h.Poll().phase == HealPhase::Checking);
    b.ack = b.seq;
    b.expected_sp = 67;
    m.Box(b);
    assert(h.Poll().phase == HealPhase::Unavailable && !h.Apply());
  }
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(904));
    m.AckOffer();
    auto b = m.Box();
    b.result = OT0_HEAL_UNAFFORDABLE;
    m.Box(b);
    auto s = h.Poll();
    assert(s.phase == HealPhase::Unavailable &&
           s.result == OT0_HEAL_UNAFFORDABLE);
  }
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    m.tear_epoch = true;
    assert(!h.Poll().live && !h.Apply());
  }
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    m.fail_read = true;
    assert(!h.Poll().live && !h.Apply());
  }
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    m.fail_write = true;
    assert(!h.Apply() && h.Poll().phase == HealPhase::Unavailable);
  }
}
} // namespace
int main() {
  TestLifecycle();
  TestPendingQueryDrain();
  TestExpiryAndEpoch();
  TestUntrustedResults();
  // A KO'd member (HP 0) offered a native revival skill is a valid offer.
  {
    Mock m;
    NativeHeal h;
    Live(m, h);
    assert(h.Find(901));
    m.Tick();
    m.AckOffer();
    auto b = m.Box();
    b.target_hp = 0;
    b.expected_hp = 0;
    m.Box(b);
    auto s = h.Poll();
    assert(s.phase == HealPhase::Ready && s.ready && s.hp == 0 && s.max_hp == 765);
  }
  // Unsupported helpers must never advertise readiness or accept requests.
  {
    Mock m;
    NativeHeal h;
    m.Put(H(version), 1u);
    h.Configure(m.Ext());
    m.Tick();
    assert(!h.Poll().live && !h.Find(904));
  }
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    assert(!h.Poll().live && !h.Apply());
  }
  // Idempotent Configure cannot erase an in-flight one-shot action.
  {
    Mock m;
    NativeHeal h;
    Ready(m, h);
    assert(h.Apply());
    const auto seq = m.Box().seq;
    const auto writes = m.writes.size();
    h.Configure(m.Ext());
    assert(h.Poll().phase == HealPhase::Applying && m.Box().seq == seq);
    assert(m.writes.size() == writes);
  }
  {
    Mock m;
    NativeHeal h;
    auto e = m.Ext();
    e.mailbox_size = 2048;
    h.Configure(e);
    assert(!h.Find(904) && m.writes.empty());
  }
  {
    Mock m;
    NativeHeal h;
    auto e = m.Ext();
    e.abi_hash = 0;
    h.Configure(e);
    assert(!h.Find(904) && m.writes.empty());
  }
}
