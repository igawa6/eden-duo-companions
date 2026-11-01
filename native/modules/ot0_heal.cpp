// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_heal.h"
#include <cstddef>

namespace ot0 {
namespace {
constexpr uint32_t Base = 2048;
#define H(field) (Base + uint32_t(offsetof(Ot0HealMailbox, field)))
bool SameIdentity(uint32_t a, uint32_t b) {
  return a == b || (a >= 900 && a <= 909 && b >= 900 && b <= 909);
}
bool Fresh(uint32_t now, uint32_t then) { return uint32_t(now - then) <= 120; }
bool Pointer(uint64_t p) { return p && p < (uint64_t{1} << 39) && !(p & 7); }
bool ValidOffer(const Ot0HealMailbox &m) {
  return m.result == OT0_HEAL_QUERY_OK && m.native_eligible == 1 &&
         m.app_state == 23 && m.caster && m.skill && m.target && m.cost >= 0 &&
         m.caster_sp >= m.cost && m.target_hp >= 0 && // 0 = KO'd revival target
        
         m.target_max_hp > m.target_hp && m.expected_hp == m.target_hp &&
         m.expected_sp == m.caster_sp && m.expected_cost == m.cost &&
         m.expected_max_hp == m.target_max_hp && Pointer(m.expected_save) &&
         Pointer(m.expected_avatar) && m.expected_party_hash;
}
} // namespace
NativeHeal::~NativeHeal() { Stop(); }
bool NativeHeal::Load32(uint32_t o, uint32_t &v) const {
  return configured_ && ext_.load_u32(ext_.userdata, o, &v) == EDEN_DSMOD_TRUE;
}
bool NativeHeal::Load64(uint32_t o, uint64_t &v) const {
  return configured_ && ext_.load_u64(ext_.userdata, o, &v) == EDEN_DSMOD_TRUE;
}
bool NativeHeal::Store32(uint32_t o, uint32_t v) const {
  return configured_ && ext_.store_u32(ext_.userdata, o, v) == EDEN_DSMOD_TRUE;
}
bool NativeHeal::Store64(uint32_t o, uint64_t v) const {
  return configured_ && ext_.store_u64(ext_.userdata, o, v) == EDEN_DSMOD_TRUE;
}
uint32_t NativeHeal::NextSeq() {
  sequence_ = (sequence_ + 2) & ~uint32_t{1};
  if (!sequence_)
    sequence_ = 2;
  return sequence_;
}
void NativeHeal::Configure(const EdenDsmodHostExtensions &e) {
  std::lock_guard lock(mutex_);
  if (configured_ && e.version == ext_.version && e.abi_hash == ext_.abi_hash &&
      e.struct_size == ext_.struct_size && e.userdata == ext_.userdata &&
      e.mailbox_address == ext_.mailbox_address &&
      e.mailbox_size == ext_.mailbox_size && e.load_u32 == ext_.load_u32 &&
      e.load_u64 == ext_.load_u64 && e.store_u32 == ext_.store_u32 &&
      e.store_u64 == ext_.store_u64)
    return;
  CancelLocked();
  ext_ = e;
  configured_ = e.version == EDEN_DSMOD_EXT_VERSION &&
                e.abi_hash == EDEN_DSMOD_EXT_HASH &&
                e.struct_size >= sizeof(EdenDsmodHostExtensions) &&
                e.mailbox_address &&
                e.mailbox_size >= Base + sizeof(Ot0HealMailbox) && e.load_u32 &&
                e.load_u64 && e.store_u32 && e.store_u64;
  state_ = {};
  pending_ = {};
  offer_ = {};
  have_sample_ = advanced_ = false;
  draining_ = drain_sample_ = false;
  sequence_ = last_epoch_ = last_heartbeat_ = 0;
  if (configured_) {
    Load32(H(seq), sequence_);
    uint32_t executed{};
    if (Load32(H(executed_seq), executed) && int32_t(executed - sequence_) > 0)
      sequence_ = executed;
  }
}
bool NativeHeal::ReadBox(Ot0HealMailbox &m) const {
  uint32_t epoch{}, seq{}, ack{};
  bool ok = Load32(4, epoch) && Load32(H(seq), seq) && !(seq & 1) &&
            Load32(H(ack), ack) && ack == seq;
#define L32(f) ok = ok && Load32(H(f), m.f)
#define S32(f)                                                                 \
  do {                                                                         \
    uint32_t v{};                                                              \
    ok = ok && Load32(H(f), v);                                                \
    m.f = int32_t(v);                                                          \
  } while (false)
#define L64(f) ok = ok && Load64(H(f), m.f)
  L32(heartbeat);
  L32(epoch);
  L32(op);
  L32(seq);
  L32(ack);
  L32(result);
  L32(caster);
  L32(skill);
  L32(target);
  S32(cost);
  S32(target_hp);
  S32(caster_sp);
  S32(target_max_hp);
  L32(native_eligible);
  L32(app_state);
  L32(version);
  L32(expected_epoch);
  L32(deadline);
  S32(expected_hp);
  S32(expected_sp);
  S32(expected_cost);
  S32(expected_max_hp);
  L64(expected_save);
  L64(expected_avatar);
  L64(expected_party_hash);
  S32(result_hp);
  S32(result_sp);
  L32(executed_seq);
  L32(result_heartbeat);
  L32(skill_text_id);
#undef L32
#undef S32
#undef L64
  uint32_t epoch2{}, seq2{}, ack2{};
  return ok && Load32(H(seq), seq2) && Load32(H(ack), ack2) &&
         Load32(4, epoch2) && epoch == epoch2 && m.epoch == epoch &&
         m.version == 2 && seq == seq2 && ack == ack2 && m.seq == seq &&
         m.ack == ack;
}
void NativeHeal::BeginDrainLocked() {
  // Query results reuse request fields. Withdrawing the sequence cannot stop
  // a query already on the game thread from finishing its output writes.
  // A later wrapper heartbeat proves that previous invocation returned.
  draining_ = true;
  drain_sample_ = Load32(0, drain_heartbeat_);
}
void NativeHeal::RefuseLocked(uint32_t result) {
  // Retire a pending request before a stopped/paused helper can resume.
  if (pending_.seq || offer_.seq) {
    const uint32_t seq = NextSeq();
    Store32(H(seq), seq - 1);
    Store32(H(op), 0);
    BeginDrainLocked();
  }
  pending_ = {};
  offer_ = {};
  state_.phase = HealPhase::Unavailable;
  state_.ready = false;
  state_.result = result;
}
HealState NativeHeal::PollLocked() {
  uint32_t epoch{}, heartbeat{}, version{}, epoch2{};
  if (!Load32(4, epoch) || !Load32(0, heartbeat) ||
      !Load32(H(version), version) || !Load32(4, epoch2) || epoch != epoch2 ||
      version != 2) {
    state_.live = false;
    if (pending_.seq || state_.ready)
      RefuseLocked(OT0_HEAL_NATIVE_UNAVAILABLE);
    return state_;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!have_sample_ || last_epoch_ != epoch) {
    if (have_sample_ && (pending_.seq || state_.ready))
      RefuseLocked(OT0_HEAL_EXPIRED);
    have_sample_ = true;
    advanced_ = false;
    last_epoch_ = epoch;
    last_heartbeat_ = heartbeat;
    last_advance_ = now;
  } else if (last_heartbeat_ != heartbeat) {
    advanced_ = true;
    last_heartbeat_ = heartbeat;
    last_advance_ = now;
  }
  state_.live =
      advanced_ && now - last_advance_ < std::chrono::milliseconds(500);
  if (draining_) {
    if (!drain_sample_) {
      drain_heartbeat_ = heartbeat;
      drain_sample_ = true;
    } else if (heartbeat != drain_heartbeat_) {
      draining_ = false;
    }
  }
  if (!state_.live) {
    if (pending_.seq || state_.ready)
      RefuseLocked(OT0_HEAL_NATIVE_UNAVAILABLE);
    return state_;
  }
  if (state_.ready && (!Fresh(heartbeat, offer_.result_heartbeat) ||
                       offer_.expected_epoch != epoch)) {
    RefuseLocked(OT0_HEAL_EXPIRED);
    return state_;
  }
  if (!pending_.seq)
    return state_;
  if (pending_.epoch != epoch || !Fresh(heartbeat, pending_.heartbeat)) {
    RefuseLocked(OT0_HEAL_EXPIRED);
    return state_;
  }
  Ot0HealMailbox box{};
  if (!ReadBox(box) || box.seq != pending_.seq)
    return state_;
  if (box.expected_epoch != pending_.epoch ||
      !Fresh(heartbeat, box.result_heartbeat) ||
      !SameIdentity(box.target, pending_.target) || box.op != pending_.op ||
      (pending_.op == OT0_HEAL_APPLY &&
       (box.caster != pending_.caster || box.skill != pending_.skill))) {
    RefuseLocked(OT0_HEAL_STALE);
    return state_;
  }
  if ((pending_.op == OT0_HEAL_FIND && box.result == OT0_HEAL_QUERY_OK &&
       !ValidOffer(box)) ||
      (pending_.op == OT0_HEAL_APPLY && box.result == OT0_HEAL_APPLIED &&
       box.executed_seq != pending_.seq)) {
    RefuseLocked(OT0_HEAL_STALE);
    return state_;
  }
  state_.target = box.target;
  state_.caster = box.caster;
  state_.skill = box.skill;
  state_.text_id = box.skill_text_id;
  state_.cost = box.cost;
  state_.hp = box.target_hp;
  state_.max_hp = box.target_max_hp;
  state_.sp = box.caster_sp;
  state_.result = box.result;
  state_.after_hp = box.result_hp;
  state_.after_sp = box.result_sp;
  state_.ready = false;
  if (pending_.op == OT0_HEAL_FIND && ValidOffer(box)) {
    state_.phase = HealPhase::Ready;
    state_.ready = true;
    offer_ = box;
  } else if (pending_.op == OT0_HEAL_APPLY && box.result == OT0_HEAL_APPLIED &&
             box.executed_seq == pending_.seq) {
    state_.phase = HealPhase::Done;
    offer_ = {};
  } else {
    state_.phase = HealPhase::Unavailable;
    offer_ = {};
  }
  pending_ = {};
  return state_;
}
HealState NativeHeal::Poll() {
  std::lock_guard lock(mutex_);
  return PollLocked();
}
bool NativeHeal::Publish(uint32_t op, const Ot0HealMailbox &m, uint32_t epoch,
                         uint32_t heartbeat) {
  const uint32_t seq = NextSeq();
  bool ok = Store32(H(seq), seq - 1) && Store32(H(op), op) &&
            Store32(H(caster), m.caster) && Store32(H(skill), m.skill) &&
            Store32(H(target), m.target) && Store32(H(expected_epoch), epoch) &&
            Store32(H(deadline), heartbeat + 120) &&
            Store32(H(expected_hp), uint32_t(m.expected_hp)) &&
            Store32(H(expected_sp), uint32_t(m.expected_sp)) &&
            Store32(H(expected_cost), uint32_t(m.expected_cost)) &&
            Store32(H(expected_max_hp), uint32_t(m.expected_max_hp)) &&
            Store64(H(expected_save), m.expected_save) &&
            Store64(H(expected_avatar), m.expected_avatar) &&
            Store64(H(expected_party_hash), m.expected_party_hash) &&
            Store32(H(seq), seq);
  if (!ok) {
    Store32(H(seq), seq - 1);
    Store32(H(op), 0);
    BeginDrainLocked();
    return false;
  }
  pending_ = {seq, epoch, heartbeat, m.target, m.caster, m.skill, op};
  return true;
}
bool NativeHeal::Find(uint32_t target) {
  std::lock_guard lock(mutex_);
  PollLocked();
  if (!target || !state_.live || draining_ ||
      state_.phase == HealPhase::Checking ||
      state_.phase == HealPhase::Applying)
    return false;
  Ot0HealMailbox request{};
  request.target = target;
  offer_ = {};
  state_ = {};
  state_.live = true;
  state_.target = target;
  if (!Publish(OT0_HEAL_FIND, request, last_epoch_, last_heartbeat_)) {
    RefuseLocked(OT0_HEAL_NATIVE_UNAVAILABLE);
    return false;
  }
  state_.phase = HealPhase::Checking;
  return true;
}
bool NativeHeal::Apply() {
  std::lock_guard lock(mutex_);
  PollLocked();
  if (!state_.live || draining_ || !state_.ready ||
      state_.phase != HealPhase::Ready || !ValidOffer(offer_))
    return false;
  if (!Publish(OT0_HEAL_APPLY, offer_, last_epoch_, last_heartbeat_)) {
    RefuseLocked(OT0_HEAL_NATIVE_UNAVAILABLE);
    return false;
  }
  state_.phase = HealPhase::Applying;
  state_.ready = false;
  offer_ = {};
  return true;
}
void NativeHeal::CancelLocked() {
  if (configured_) {
    const uint32_t seq = NextSeq();
    Store32(H(seq), seq - 1);
    Store32(H(op), 0);
    BeginDrainLocked();
  }
  pending_ = {};
  offer_ = {};
  const bool live = state_.live;
  state_ = {};
  state_.live = live;
}
void NativeHeal::Cancel() {
  std::lock_guard lock(mutex_);
  CancelLocked();
}
void NativeHeal::Stop() { Cancel(); }
} // namespace ot0
