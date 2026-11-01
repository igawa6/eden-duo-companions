// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_companion_helper.h"
#include "ot0_battle_actors.h"
#include "ot0_battle_signature.h"
#include <cstring>
namespace ot0 {
namespace {
constexpr uint32_t Off(size_t p) { return uint32_t(p); }
#define O(field) Off(offsetof(Ot0CompanionMailbox, field))
bool Fresh(uint32_t now, uint32_t then, uint32_t age) {
  return uint32_t(now - then) <= age;
}
uint32_t Next(uint32_t &seq) {
  seq = (seq + 2) & 0x7ffffffe;
  if (!seq)
    seq = 2;
  return seq;
}
uint32_t SerialValue(const battle_detail::SerialSample &s) {
  uint32_t v{};
  std::memcpy(&v, s.item.data() + 16, 4);
  return v;
}
bool Same(const Ot0NativeStatRow &a, const Ot0NativeStatRow &b) {
  return a.actor == b.actor && a.id == b.id && a.team == b.team &&
         a.slot == b.slot && a.serial == b.serial &&
         a.ailment_fingerprint == b.ailment_fingerprint &&
         a.weapon_selector == b.weapon_selector;
}
} // namespace
bool NativeCompanionHelper::Load32(uint32_t o, uint32_t &v) const {
  return configured_ && ext_.load_u32(ext_.userdata, o, &v) == EDEN_DSMOD_TRUE;
}
bool NativeCompanionHelper::Load64(uint32_t o, uint64_t &v) const {
  return configured_ && ext_.load_u64(ext_.userdata, o, &v) == EDEN_DSMOD_TRUE;
}
bool NativeCompanionHelper::Store32(uint32_t o, uint32_t v) const {
  return configured_ && ext_.store_u32(ext_.userdata, o, v) == EDEN_DSMOD_TRUE;
}
bool NativeCompanionHelper::Store64(uint32_t o, uint64_t v) const {
  return configured_ && ext_.store_u64(ext_.userdata, o, v) == EDEN_DSMOD_TRUE;
}
NativeCompanionHelper::~NativeCompanionHelper() { Stop(); }
void NativeCompanionHelper::Configure(const EdenDsmodHostExtensions &e) {
  std::lock_guard lock(mutex_);
  if (configured_ && ext_.userdata == e.userdata &&
      ext_.mailbox_address == e.mailbox_address &&
      ext_.mailbox_size == e.mailbox_size && ext_.load_u32 == e.load_u32 &&
      ext_.load_u64 == e.load_u64 && ext_.store_u32 == e.store_u32 &&
      ext_.store_u64 == e.store_u64 && e.version == EDEN_DSMOD_EXT_VERSION &&
      e.abi_hash == EDEN_DSMOD_EXT_HASH)
    return;
  StopLocked();
  ext_ = e;
  configured_ = e.version == EDEN_DSMOD_EXT_VERSION &&
                e.abi_hash == EDEN_DSMOD_EXT_HASH &&
                e.struct_size >= sizeof(EdenDsmodHostExtensions) &&
                e.mailbox_address &&
                e.mailbox_size >= sizeof(Ot0CompanionMailbox) && e.load_u32 &&
                e.load_u64 && e.store_u32 && e.store_u64;
  state_ = {};
  status_ = {};
  pending_ = {};
  cache_ = {};
  advanced_ = false;
  have_sample_ = false;
  last_epoch_ = last_heartbeat_ = 0;
  lease_seq_ = query_seq_ = 0;
  if (configured_) {
    Load32(O(lease_seq), lease_seq_);
    Load32(O(query_seq), query_seq_);
    lease_seq_ &= 0x7ffffffe;
    query_seq_ &= 0x7ffffffe;
  }
}
NativeCompanionHelper::State NativeCompanionHelper::PollLocked() {
  State s{};
  if (!configured_)
    return state_ = s;
  s.mailbox = ext_.mailbox_address;
  s.mailbox_size = ext_.mailbox_size;
  uint32_t epoch2{};
  if (!Load32(O(epoch), s.epoch) || !Load32(O(heartbeat), s.heartbeat) ||
      !Load32(O(helper_version), s.version) || !Load32(O(epoch), epoch2) ||
      epoch2 != s.epoch) {
    advanced_ = false;
    return state_ = s;
  }
  auto now = std::chrono::steady_clock::now();
  if (!have_sample_ || s.epoch != last_epoch_ ||
      s.version != OT0_COMPANION_ABI) {
    advanced_ = false;
    status_ = {};
    pending_ = {};
    cache_ = {};
    last_epoch_ = s.epoch;
    last_heartbeat_ = s.heartbeat;
    last_advance_ = now;
  } else if (s.heartbeat != last_heartbeat_) {
    advanced_ = true;
    last_heartbeat_ = s.heartbeat;
    last_advance_ = now;
  }
  have_sample_ = true;
  s.live = s.version == OT0_COMPANION_ABI && advanced_ &&
           now - last_advance_ < std::chrono::milliseconds(500);
  Load32(O(app_state), s.app_state);
  Load32(O(query_result), s.query_result);
  Load32(O(native_visibility), s.native_visibility);
  uint32_t a{}, b{};
  Ot0CompanionMailbox t{};
  bool ok = false;
  // An in-flight guest publication is unknown, not a confirmed ownership loss.
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    t = {};
    ok = Load32(O(status_seq), a) && !(a & 1);
#define L32(f) ok = ok && Load32(O(f), t.f)
#define L64(f) ok = ok && Load64(O(f), t.f)
    L32(hidden);
    L32(owned_index);
    L32(owned_serial);
    L32(owned_root_index);
    L32(owned_root_serial);
    L64(owned_ui);
    L64(owned_slate);
    L64(owned_controller);
    L64(owned_root);
    L64(owned_root_slate);
    L64(owned_root_controller);
    L64(owned_slot);
#undef L32
#undef L64
    ok = ok && Load32(O(status_seq), b) && a == b && Load32(O(epoch), epoch2) &&
         epoch2 == s.epoch;
    if (ok)
      break;
  }
  s.status_known = ok;
  if (ok)
    status_ = t;
  // A coherent epoch change above discards the previous ownership record.
  s.hidden = s.live && status_.hidden == 1 && status_.owned_ui &&
             status_.owned_slate && status_.owned_controller &&
             status_.owned_root && status_.owned_root_serial &&
             status_.owned_serial && status_.owned_slot;
  return state_ = s;
}
NativeCompanionHelper::State NativeCompanionHelper::Poll() {
  std::lock_guard lock(mutex_);
  return PollLocked();
}
bool NativeCompanionHelper::CommitLease(bool hide, uint64_t resource,
                                        uint64_t section, uint32_t map,
                                        uint64_t root, uint32_t ri, uint32_t rs,
                                        uint64_t canvas, uint32_t ci,
                                        uint32_t cs) {
  uint32_t even = Next(lease_seq_);
  return Store32(O(lease_seq), even - 1) && Store32(O(desired_hide), 0) &&
         Store32(O(lease_epoch), state_.epoch) &&
         Store32(O(lease_deadline), state_.heartbeat + 120) &&
         Store64(O(relocation_delta), 0) &&
         Store64(O(expected_resource), resource) &&
         Store64(O(expected_section), section) &&
         Store32(O(expected_map_id), map) && Store64(O(expected_root), root) &&
         Store32(O(expected_root_index), ri) &&
         Store32(O(expected_root_serial), rs) &&
         Store64(O(expected_canvas), canvas) &&
         Store32(O(expected_canvas_index), ci) &&
         Store32(O(expected_canvas_serial), cs) &&
         Store32(O(desired_hide), hide ? 1 : 0) && Store32(O(lease_seq), even);
}
void NativeCompanionHelper::StopLocked() {
  if (configured_)
    CommitLease(false);
  pending_ = {};
  cache_ = {};
}
void NativeCompanionHelper::Stop() {
  std::lock_guard lock(mutex_);
  StopLocked();
}
void NativeCompanionHelper::SetMapLease(const ReadMemory &read, uint64_t main,
                                        int64_t delta,
                                        const MinimapIcons *icons, bool hide) {
  std::lock_guard lock(mutex_);
  auto s = PollLocked();
  if (!hide || !s.live || delta || !icons || !icons->valid ||
      !icons->complete || !icons->native_resource || !icons->native_section) {
    CommitLease(false);
    return;
  }
  // Leave the committed deadline untouched: the guest still expires a lease
  // if coherent publication never recovers. Explicit exits above show at once.
  if (!s.status_known)
    return;
  const auto root = FindMinimapHud(read, main);
  auto canvas = FindMinimapCanvas(read, main);
  if (!canvas && s.hidden)
    canvas = FindMinimapCanvas(read, main, 0, true);
  battle_detail::Reader r{read, main, 0};
  battle_detail::SerialSample rs{}, cs{}, rs2{}, cs2{};
  bool ok = root && canvas && battle_detail::Serial(r, root->row, rs) &&
            battle_detail::Serial(r, canvas->row, cs);
  if (ok && s.hidden)
    ok = root->row == status_.owned_root &&
         root->widget == status_.owned_root_slate &&
         root->controller == status_.owned_root_controller &&
         canvas->row == status_.owned_ui &&
         canvas->widget == status_.owned_slate &&
         canvas->controller == status_.owned_controller &&
         uint32_t(rs.index) == status_.owned_root_index &&
         SerialValue(rs) == status_.owned_root_serial &&
         uint32_t(cs.index) == status_.owned_index &&
         SerialValue(cs) == status_.owned_serial;
  if (ok)
    ok = battle_detail::Serial(r, root->row, rs2) &&
         battle_detail::Serial(r, canvas->row, cs2) && rs == rs2 && cs == cs2;
  if (!ok) {
    CommitLease(false);
    return;
  }
  CommitLease(true, icons->native_resource, icons->native_section,
              icons->map_id, root->row, uint32_t(rs.index), SerialValue(rs),
              canvas->row, uint32_t(cs.index), SerialValue(cs));
}
BattleStatsSnapshot
NativeCompanionHelper::Query(const ReadMemory &read, uint64_t main,
                             int64_t delta, const BattleStatsSnapshot &input,
                             uint32_t desiredPartyId, int desiredEnemyIndex) {
  std::lock_guard lock(mutex_);
  auto s = PollLocked();
  auto out = input;
  for (auto &v : out.party)
    v.parameters = {};
  for (auto &v : out.enemies)
    v.parameters = {};
  if (!s.live || delta || !input.valid) {
    pending_ = {};
    cache_ = {};
    return out;
  }
  battle_detail::Reader r{read, main, 0};
  BattleSnapshot shape{};
  for (const auto &v : input.enemies)
    if (v.bound)
      ++shape.enemy_count;
  battle_detail::Context context{}, after{};
  if (!battle_detail::Sample(r, shape, context)) {
    pending_ = {};
    cache_ = {};
    return out;
  }
  Request desired{};
  desired.bm = context.bm;
  desired.epoch = s.epoch;
  desired.heartbeat = s.heartbeat;
  std::array<ActorCombatStats *, OT0_COMPANION_ROWS> targets{};
  const auto add = [&](const ActorCombatStats &v, ActorCombatStats &target,
                       uint8_t team, int slot) {
    if (slot < 0 || slot >= OT0_COMPANION_ROWS || !v.bound ||
        (team == 0 && !v.weapon_context_valid) ||
        !battle_detail::Identity(r, v.native_actor, team, v.id))
      return;
    const bool warmup = !v.signature_valid;
    battle_detail::ActorSignature signature{};
    if (warmup) {
      // A native weak handle has not necessarily been assigned yet. This
      // query requests initialization only; it cannot authorize any values.
      battle_detail::SerialSample before{}, after{};
      uint8_t selector{};
      if (!battle_detail::Serial(r, v.native_actor, before, true) ||
          SerialValue(before) != 0 ||
          !r.get(v.native_actor + 0x2a8, selector) || selector > 1 ||
          !battle_detail::Serial(r, v.native_actor, after, true) ||
          before != after)
        return;
      signature.weapon_selector = selector;
    } else {
      signature = battle_detail::ReadActorSignature(r, v.native_actor);
      if (!signature.valid || signature.native_serial != v.native_serial ||
          signature.raw_ailment_fingerprint != v.raw_ailment_fingerprint ||
          signature.weapon_selector != v.weapon_selector)
        return;
    }
    uint32_t raw_id{};
    if (!r.get(v.native_actor + 0x298, raw_id))
      return;
    auto &row = desired.rows[size_t(slot)];
    row.actor = v.native_actor;
    row.id = raw_id;
    row.team = team;
    row.slot = uint32_t(slot);
    row.serial = signature.native_serial;
    row.ailment_fingerprint = signature.raw_ailment_fingerprint;
    row.weapon_selector = signature.weapon_selector;
    desired.mask |= 1u << slot;
    if (!warmup)
      targets[size_t(slot)] = &target;
  };
  for (size_t i = 0; i < input.party.size(); ++i) {
    const auto &v = input.party[i];
    if (battle_detail::Alias(v.id) != battle_detail::Alias(desiredPartyId))
      continue;
    int slot = -1;
    for (int j = 0; j < context.front.count; ++j)
      if (context.fronts[size_t(j)] == v.native_actor)
        slot = j;
    for (int j = 0; j < context.back.count; ++j)
      if (context.backs[size_t(j)] == v.native_actor)
        slot = j + 4;
    add(v, out.party[i], 0, slot);
    break;
  }
  if (desiredEnemyIndex >= 0 && desiredEnemyIndex < context.enemy.count &&
      desiredEnemyIndex < int(input.enemies.size())) {
    const auto &v = input.enemies[size_t(desiredEnemyIndex)];
    if (v.native_actor == context.enemies[size_t(desiredEnemyIndex)])
      add(v, out.enemies[size_t(desiredEnemyIndex)], 1, 8 + desiredEnemyIndex);
  }
  if (!battle_detail::Sample(r, shape, after) || after != context) {
    pending_ = {};
    cache_ = {};
    return out;
  }
  // query_ack is the guest's release publication. Read rows only inside its
  // matching stable request/epoch envelope, and age them from request time.
  uint32_t ack{}, seq{}, epoch{}, result{}, query_epoch{};
  if (pending_.seq && Load32(O(query_ack), ack) && ack == pending_.seq &&
      Load32(O(query_seq), seq) && seq == ack && Load32(O(epoch), epoch) &&
      epoch == pending_.epoch && Load32(O(query_epoch), query_epoch) &&
      query_epoch == pending_.epoch && Load32(O(query_result), result)) {
    Request reply = pending_;
    bool ok = result == OT0_NATIVE_OK &&
              Fresh(s.heartbeat, pending_.heartbeat, 12) &&
              pending_.epoch == s.epoch;
    uint64_t bm{};
    ok = ok && Load64(O(query_bm), bm) && bm == pending_.bm;
    for (size_t i = 0; i < cache_.size(); ++i)
      if (pending_.mask & (1u << i)) {
        cache_[i] = {};
        auto &row = reply.rows[i];
        auto *bytes = reinterpret_cast<uint8_t *>(&row);
        for (size_t p = 0; ok && p < sizeof(row);) {
          const uint32_t offset = O(rows) + uint32_t(i * sizeof(row) + p);
          if (p == offsetof(Ot0NativeStatRow, actor) ||
              p == offsetof(Ot0NativeStatRow, ailment_fingerprint)) {
            uint64_t word{};
            ok = Load64(offset, word);
            if (ok)
              std::memcpy(bytes + p, &word, 8);
            p += 8;
          } else {
            uint32_t word{};
            ok = Load32(offset, word);
            if (ok)
              std::memcpy(bytes + p, &word, 4);
            p += 4;
          }
        }
      }
    uint32_t ack2{}, seq2{}, epoch2{};
    ok = ok && Load32(O(query_ack), ack2) && ack2 == ack &&
         Load32(O(query_seq), seq2) && seq2 == seq &&
         Load32(O(epoch), epoch2) && epoch2 == epoch;
    if (ok)
      for (size_t i = 0; i < cache_.size(); ++i)
        if (pending_.mask & (1u << i)) {
          const auto &row = reply.rows[i];
          uint8_t subtype{};
          const bool weapon_ok =
              row.team != 0 ||
              (r.get(row.actor + 0x294, subtype) &&
               (subtype != 0 || row.native_weapon == row.weapon_selector));
          if (row.serial > 0 && Same(row, pending_.rows[i]) &&
              row.valid_mask == 255 && row.error == 0 &&
              row.native_weapon <= 1 && weapon_ok)
            cache_[i] = {row, pending_.epoch, pending_.heartbeat, pending_.bm,
                         true};
        }
    pending_ = {};
  }
  // Existing ACKs are useful during the next query only while their exact
  // current actor signature, native array slot, epoch and BM still match.
  for (size_t i = 0; i < cache_.size(); ++i) {
    auto &c = cache_[i];
    if (!c.valid)
      continue;
    if (c.epoch != s.epoch || c.bm != desired.bm ||
        !Fresh(s.heartbeat, c.heartbeat, 12)) {
      c = {};
      continue;
    }
    if (targets[i] && Same(c.row, desired.rows[i]))
      for (size_t p = 0; p < 8; ++p)
        targets[i]->parameters[p] = {c.row.values[p], true};
  }
  bool same = pending_.seq && pending_.epoch == desired.epoch &&
              pending_.bm == desired.bm && pending_.mask == desired.mask &&
              Fresh(s.heartbeat, pending_.heartbeat, 12);
  if (same)
    for (size_t i = 0; i < cache_.size(); ++i)
      if ((desired.mask & (1u << i)) &&
          !Same(pending_.rows[i], desired.rows[i]))
        same = false;
  if (desired.mask && !same) {
    uint32_t even = Next(query_seq_);
    desired.seq = even;
    bool ok = Store32(O(query_seq), even - 1) &&
              Store32(O(query_mask), desired.mask) &&
              Store32(O(query_epoch), s.epoch) &&
              Store32(O(query_deadline), s.heartbeat + 12) &&
              Store64(O(relocation_delta), 0);
    // Guest obtains row identities from the current native arrays itself; host
    // never writes result rows or query_bm while that query is executing.
    ok = ok && Store32(O(query_seq), even);
    pending_ = ok ? desired : Request{};
  } else if (!desired.mask) {
    pending_ = {};
  }
  return out;
}
#undef O
} // namespace ot0
