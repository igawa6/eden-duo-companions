// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_drive.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace echoes {
namespace {
bool UniqueIds(const std::vector<uint16_t>& ids) {
  if (ids.empty() || ids.size() > MaxLearnedKeys) return false;
  auto sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  return sorted.back() < 1225 &&
         std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}
bool Closed(const LiveSnapshot& s) {
  const auto& p = s.selector;
  return s.compatible && s.collection_valid && p.compatible &&
         p.selector_valid && !p.held && p.source.owner &&
         p.source.mode == 1 && p.source.native_state == 0 &&
         p.ordered_native_ids.empty() && p.highlighted_index == -1 &&
         p.target_index == -1;
}
bool Held(const LiveSnapshot& s) {
  const auto& p = s.selector;
  const auto& h = p.source;
  const auto& e = s.collection_source;
  if (!s.compatible || !s.collection_valid || !e.access_object || !e.save_data ||
      !e.list || !p.compatible || !p.selector_valid || !p.held ||
      !p.equipped_valid || p.equipped_native_id >= 1225 ||
      !h.owner || h.owner % 8 || !h.records || h.records % 8 ||
      h.end < h.records || h.capacity < h.end || h.mode != 1 ||
      h.native_state != 2 || !UniqueIds(s.learned_native_ids) ||
      !UniqueIds(p.ordered_native_ids) ||
      h.end - h.records != p.ordered_native_ids.size() * UINT64_C(0x38) ||
      (h.capacity - h.records) % 0x38 ||
      (h.capacity - h.records) / 0x38 > MaxLearnedKeys ||
      p.highlighted_index < 0 || p.target_index < 0 ||
      static_cast<size_t>(p.highlighted_index) >= p.ordered_native_ids.size() ||
      static_cast<size_t>(p.target_index) >= p.ordered_native_ids.size() ||
      p.highlighted_native_id !=
          p.ordered_native_ids[static_cast<size_t>(p.highlighted_index)])
    return false;
  return std::all_of(p.ordered_native_ids.begin(), p.ordered_native_ids.end(),
      [&s](uint16_t id) {
        return std::find(s.learned_native_ids.begin(),
                         s.learned_native_ids.end(), id) !=
               s.learned_native_ids.end();
      });
}
bool SameCollectionAndEquipped(const LiveSnapshot& a, const LiveSnapshot& b) {
  return a.compatible && b.compatible && a.collection_valid && b.collection_valid &&
         a.collection_source == b.collection_source &&
         a.learned_native_ids == b.learned_native_ids &&
         a.learned_crc_keys == b.learned_crc_keys &&
         a.selector.compatible && b.selector.compatible &&
         a.selector.equipped_valid && b.selector.equipped_valid &&
         a.selector.equipped_native_id < 1225 &&
         a.selector.equipped_native_id == b.selector.equipped_native_id;
}
bool SameContext(const LiveSnapshot& a, const LiveSnapshot& b) {
  return a.collection_source == b.collection_source &&
         a.learned_native_ids == b.learned_native_ids &&
         a.selector.source == b.selector.source &&
         a.selector.ordered_native_ids == b.selector.ordered_native_ids &&
         a.selector.equipped_native_id == b.selector.equipped_native_id;
}
} // namespace

void SelectorDrive::Abort(DriveAbort why) {
  status_ = DriveStatus::Aborted;
  abort_ = why;
  armed_ = in_hold_ = step_active_ = settling_ = bootstrap_seen_ = false;
  context_ = {};
}
void SelectorDrive::Cancel() {
  bootstrap_allowed_ = false;
  Abort(DriveAbort::Cancelled);
}

bool SelectorDrive::Observe(const LiveSnapshot& s, uint64_t tick) {
  const bool fresh = !clock_seen_ || tick != last_tick_;
  if (clock_seen_ && tick < last_tick_) {
    bootstrap_allowed_ = false;
    Abort(DriveAbort::Clock);
    last_tick_ = tick;
    return false;
  }
  if (clock_seen_ && tick - last_tick_ > 1) {
    bootstrap_allowed_ = false;
    Abort(DriveAbort::Clock); // Missed updates could conceal another hold.
  }
  last_tick_ = tick;
  clock_seen_ = true;
  if (Closed(s)) {
    if (status_ == DriveStatus::Pending) Abort(DriveAbort::Released);
    armed_ = true;
    bootstrap_allowed_ = false;
    in_hold_ = bootstrap_seen_ = false;
    context_ = s;
    return false;
  }
  if (!Held(s)) {
    // A known close can be followed by an unavailable opening selector. Keep
    // only that unused lifecycle authorization and its source anchor, never an
    // intention. The reader still refuses every unsupported native state.
    const bool opening = armed_ && !in_hold_ && status_ != DriveStatus::Pending &&
        SameCollectionAndEquipped(context_, s) && !s.selector.selector_valid &&
        !s.selector.held && s.selector.ordered_native_ids.empty() &&
        s.selector.highlighted_index == -1 && s.selector.target_index == -1;
    const bool closed_authorization = armed_;
    auto anchor = opening ? context_ : LiveSnapshot{};
    Abort(DriveAbort::InvalidSnapshot);
    if (opening) {
      context_ = std::move(anchor);
      armed_ = bootstrap_allowed_ = true; // requires stable Held bootstrap below
    } else if (closed_authorization) {
      bootstrap_allowed_ = false;
    }
    return false;
  }
  if (!in_hold_) {
    if (!armed_ || bootstrap_allowed_) {
      if (!bootstrap_allowed_) return false;
      // Recovery retains the closed/candidate anchor through any consecutive
      // unavailable frames. A changed source/equipped/owner cannot restore it.
      if (armed_ && (!SameCollectionAndEquipped(context_, s) ||
          context_.selector.source.owner != s.selector.source.owner ||
          (context_.selector.held && !SameContext(context_, s)))) {
        bootstrap_allowed_ = false;
        Abort(DriveAbort::ContextChanged);
        return false;
      }
      if (s.selector.highlighted_index != s.selector.target_index) {
        bootstrap_seen_ = false;
        return false;
      }
      if (!bootstrap_seen_ || !SameContext(context_, s) ||
          bootstrap_cursor_ != s.selector.highlighted_index) {
        context_ = s;
        bootstrap_at_ = tick;
        bootstrap_cursor_ = s.selector.highlighted_index;
        bootstrap_seen_ = true;
        return false;
      }
      if (!fresh || tick - bootstrap_at_ < SettleTicks) return false;
    }
    context_ = s;
    in_hold_ = true;
    armed_ = bootstrap_allowed_ = bootstrap_seen_ = false;
  } else if (!SameContext(context_, s)) {
    Abort(DriveAbort::ContextChanged);
    return false;
  }
  return fresh;
}

bool SelectorDrive::Request(uint16_t id, const LiveSnapshot& s, uint64_t tick) {
  Observe(s, tick);
  if (!in_hold_ || status_ == DriveStatus::Pending ||
      s.selector.highlighted_index != s.selector.target_index ||
      tick > std::numeric_limits<uint64_t>::max() - RequestTimeoutTicks)
    return false;
  const auto& ids = s.selector.ordered_native_ids;
  const auto found = std::find(ids.begin(), ids.end(), id);
  if (found == ids.end()) return false;
  target_ = static_cast<int32_t>(found - ids.begin());
  origin_ = s.selector.highlighted_index;
  requested_at_ = settled_at_ = tick;
  step_active_ = false;
  settling_ = true;
  status_ = DriveStatus::Pending;
  abort_ = DriveAbort::None;
  return true;
}

Direction SelectorDrive::Advance(const LiveSnapshot& s, uint64_t tick) {
  const bool fresh = Observe(s, tick);
  if (!in_hold_ || status_ != DriveStatus::Pending) return Direction::None;
  const auto actual = s.selector.highlighted_index;
  const auto destination = s.selector.target_index;
  if (tick - requested_at_ >= RequestTimeoutTicks ||
      (step_active_ && tick - pulse_at_ >= StepTimeoutTicks)) {
    Abort(DriveAbort::Timeout);
    return Direction::None;
  }
  if ((!step_active_ && (actual != origin_ || destination != origin_)) ||
      (step_active_ && ((actual != origin_ && actual != expected_) ||
       (destination != origin_ && destination != expected_) ||
       (actual == expected_ && destination != expected_) ||
       (arrival_seen_ && actual != expected_) ||
       (destination_seen_ && destination != expected_)))) {
    Abort(DriveAbort::UnexpectedCursor);
    return Direction::None;
  }
  if (step_active_) {
    destination_seen_ = destination_seen_ || destination == expected_;
    arrival_seen_ = arrival_seen_ || actual == expected_;
  }
  if (!fresh) return Direction::None;
  if (step_active_) {
    // Reserve the complete pulse and neutral period before counting settled
    // observations. Dropped input times out; it is never retried blindly.
    if (tick - pulse_at_ < PulseTicks + NeutralTicks ||
        actual != expected_ || destination != expected_) {
      settling_ = false;
      return Direction::None;
    }
  }
  if (!settling_) {
    settled_at_ = tick;
    settling_ = true;
  }
  if (tick - settled_at_ < SettleTicks) return Direction::None;
  if (actual == target_) {
    status_ = DriveStatus::Complete;
    step_active_ = false;
    return Direction::None;
  }
  const auto count = static_cast<int32_t>(s.selector.ordered_native_ids.size());
  const int32_t right = (target_ - actual + count) % count;
  const int32_t left = (actual - target_ + count) % count;
  const bool go_right = right <= left; // Deterministic tie.
  origin_ = actual;
  expected_ = (actual + (go_right ? 1 : count - 1)) % count;
  pulse_at_ = tick;
  destination_seen_ = arrival_seen_ = false;
  step_active_ = true;
  settling_ = false;
  return go_right ? Direction::Right : Direction::Left;
}
} // namespace echoes
