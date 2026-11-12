// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_quick_slots.h"
#include "echoes_save_slot_bridge.h"
#include <array>
#include <cstdint>
#include <optional>

namespace echoes {
// In-memory, bounded (SaveSlotBridge::Buckets), LRU-by-write-age table
// mapping a stable save identity to that save's own circle assignments.
// Fully decoupled from how the identity is computed (Module's concern) and
// from guest memory; Encode()/Decode() round-trip through
// SaveSlotBridge::Table so the runtime-22 persisted flags can carry the
// whole table across an app restart. Identity 0 is never a valid save
// identity and is reserved to mean "this bucket is unused".
class SaveSlotTable {
 public:
  static constexpr size_t Buckets = SaveSlotBridge::Buckets;
  using Slots = std::array<uint16_t, QuickSlots::Count>;

  void Decode(const SaveSlotBridge::Table& raw) {
    for (size_t b = 0; b < Buckets; ++b) {
      auto& bucket = buckets_[b];
      const auto base = b * SaveSlotBridge::FieldsPerBucket;
      bucket.identity = (uint32_t(raw[base + 1]) << 16) | raw[base + 0];
      bucket.age = raw[base + 2];
      for (size_t i = 0; i < QuickSlots::Count; ++i)
        bucket.slots[i] = raw[base + 3 + i] ? static_cast<uint16_t>(raw[base + 3 + i] - 1) : QuickSlots::Empty;
      bucket.used = bucket.identity != 0;
    }
    generation_ = raw[Buckets * SaveSlotBridge::FieldsPerBucket];
    if (!generation_) generation_ = 1;
  }
  SaveSlotBridge::Table Encode() const {
    SaveSlotBridge::Table out{};
    for (size_t b = 0; b < Buckets; ++b) {
      const auto& bucket = buckets_[b];
      const auto base = b * SaveSlotBridge::FieldsPerBucket;
      out[base + 0] = static_cast<uint16_t>(bucket.identity & 0xffffu);
      out[base + 1] = static_cast<uint16_t>(bucket.identity >> 16);
      out[base + 2] = bucket.age;
      for (size_t i = 0; i < QuickSlots::Count; ++i)
        out[base + 3 + i] = bucket.slots[i] == QuickSlots::Empty ? uint16_t(0) : static_cast<uint16_t>(bucket.slots[i] + 1);
    }
    out[Buckets * SaveSlotBridge::FieldsPerBucket] = generation_;
    return out;
  }
  // The stored shortcuts for `identity`, or nullopt if not tracked.
  std::optional<Slots> Find(uint32_t identity) const {
    if (!identity) return {};
    for (const auto& bucket : buckets_)
      if (bucket.used && bucket.identity == identity) return bucket.slots;
    return {};
  }
  // Records `slots` for `identity`: refreshes it in place if already
  // tracked, else claims a free bucket, else evicts the least-recently
  // WRITTEN bucket (lowest age). A no-op for an invalid (zero) identity.
  void Store(uint32_t identity, const Slots& slots) {
    if (!identity) return;
    for (auto& bucket : buckets_)
      if (bucket.used && bucket.identity == identity) { bucket.slots = slots; bucket.age = NextGeneration(); return; }
    for (auto& bucket : buckets_)
      if (!bucket.used) { bucket.used = true; bucket.identity = identity; bucket.slots = slots; bucket.age = NextGeneration(); return; }
    auto* victim = &buckets_[0];
    for (auto& bucket : buckets_) if (bucket.age < victim->age) victim = &bucket;
    victim->identity = identity; victim->slots = slots; victim->age = NextGeneration();
  }
  size_t TrackedCount() const {
    size_t n = 0; for (const auto& bucket : buckets_) if (bucket.used) ++n; return n;
  }
  // Follows an identity DRIFT within the same save session (the save's own
  // identity components -- learned-Echo count, Automaton mask -- changed
  // without a save switch, e.g. the player just learned a new Echo): moves
  // whatever is tracked under `from` to key `to` instead of orphaning it,
  // so a hand assignment made earlier this same save keeps matching after
  // the drift. Never called across a real save switch (Module's own
  // concern: it only calls this when its one-shot ownership-change signal
  // did NOT fire between two different identity readings).
  // - No-op when `from`/`to` are equal, or either is the reserved 0, or
  //   nothing is tracked under `from` (nothing to move).
  // - If `to` already names a DIFFERENT tracked bucket (an identity
  //   collision, the same risk every lookup already accepts), that
  //   bucket's contents are overwritten with `from`'s -- the save that
  //   just drifted into this identity is, by definition, the one actually
  //   playing right now, so its own assignment wins.
  // - Otherwise `from`'s bucket is relabelled to `to` in place, keeping
  //   its slots untouched, and its age is bumped (a drift is activity).
  void Migrate(uint32_t from, uint32_t to) {
    if (!from || !to || from == to) return;
    Bucket* source = nullptr;
    for (auto& bucket : buckets_) if (bucket.used && bucket.identity == from) { source = &bucket; break; }
    if (!source) return;
    for (auto& bucket : buckets_) {
      if (&bucket == source) continue;
      if (bucket.used && bucket.identity == to) {
        bucket.slots = source->slots;
        bucket.age = NextGeneration();
        source->used = false; source->identity = 0; source->age = 0;
        source->slots = {QuickSlots::Empty, QuickSlots::Empty, QuickSlots::Empty};
        return;
      }
    }
    source->identity = to;
    source->age = NextGeneration();
  }

 private:
  uint16_t NextGeneration() {
    const auto g = generation_;
    generation_ = static_cast<uint16_t>(generation_ + 1);
    if (!generation_) generation_ = 1; // 0 is reserved; skip over a wrap
    return g;
  }
  struct Bucket {
    uint32_t identity{};
    uint16_t age{};
    Slots slots{QuickSlots::Empty, QuickSlots::Empty, QuickSlots::Empty};
    bool used{};
  };
  std::array<Bucket, Buckets> buckets_{};
  uint16_t generation_{1};
};
} // namespace echoes
