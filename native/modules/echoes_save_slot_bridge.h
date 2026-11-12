// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace echoes {
enum class SaveSlotRestoreStatus { Collecting, Ready, Refused };
// Runtime22 flag transport for a small, bounded table of per-save circle
// assignments (8 saves, LRU), independent of native guest memory. Shaped
// exactly like the stamp/quick-slot bridges this replaces: each persisted
// value is transported one bit at a time through gated actions, because a
// module cannot read a persisted flag directly -- only receive it pushed
// back through on_action. Every one of the Count values is an OPAQUE raw
// unsigned16 (no packing convention here); Module alone decides what each
// index means (bucket identity halves, an age counter, or a circle content
// with its own "+1, 0 = unset" convention) and whether 0 means "unused".
class SaveSlotBridge {
 public:
  static constexpr size_t Buckets = 8;
  static constexpr size_t FieldsPerBucket = 6; // id_lo, id_hi, age, c0, c1, c2
  static constexpr size_t Count = Buckets * FieldsPerBucket + 1; // +1 shared generation counter
  using Table = std::array<uint16_t, Count>;
  SaveSlotRestoreStatus status() const { return status_; }
  bool restore_pending() const { return status_ == SaveSlotRestoreStatus::Collecting; }
  size_t restore_slot() const { return slot_; }
  void RefuseRestore() {
    status_ = SaveSlotRestoreStatus::Refused; received_ = value_ = 0; restored_ = {}; pending_.reset();
  }
  // bit*2+value, 0..31; -1 refuses invalid runtime flags. Equal duplicates are
  // idempotent, conflicting packets reject the complete restore transaction.
  bool RestorePacket(int64_t packet) {
    if (status_ == SaveSlotRestoreStatus::Refused) return false;
    if (packet < 0 || packet >= 32) { RefuseRestore(); return false; }
    if (status_ == SaveSlotRestoreStatus::Ready) return true;
    const auto mask = static_cast<uint16_t>(1u << unsigned(packet / 2));
    const bool set = (packet % 2) != 0;
    if ((received_ & mask) && bool(value_ & mask) != set) { RefuseRestore(); return false; }
    received_ |= mask; if (set) value_ |= mask;
    return true;
  }
  // Call only at Sample start. Writes the whole table out atomically once
  // every value has arrived. On later calls returns ready without replacing
  // the caller's copy.
  bool AdvanceRestore(Table& out) {
    if (status_ != SaveSlotRestoreStatus::Collecting) return status_ == SaveSlotRestoreStatus::Ready;
    if (received_ != UINT16_MAX) return false;
    if (slot_ >= Count) { RefuseRestore(); return false; }
    restored_[slot_] = value_; received_ = value_ = 0;
    if (++slot_ < Count) return false;
    out = restored_; acknowledged_ = targets_ = restored_; status_ = SaveSlotRestoreStatus::Ready;
    return true;
  }
  // Call in Sample ONLY, after Module has (re)computed the table it wants
  // persisted. Finds the first value that differs from what was last
  // acknowledged and queues only that one; call again next sample (same
  // `values`) after each ack to advance to the next difference.
  bool QueueWrites(const Table& values) {
    if (status_ != SaveSlotRestoreStatus::Ready) return false;
    targets_ = values;
    for (size_t i = 0; i < Count; ++i) {
      if (targets_[i] != acknowledged_[i]) { pending_ = i; return true; }
    }
    pending_.reset();
    return true;
  }
  bool writes_pending() const { return pending_.has_value(); }
  bool write_pending(size_t slot) const { return pending_ && *pending_ == slot; }
  int64_t write_value(size_t slot) const { return slot < Count ? static_cast<int64_t>(targets_[slot]) : 0; }
  // Module action must be protected by the exact published flag==target gate.
  bool AcknowledgeWrite(size_t slot) {
    if (status_ != SaveSlotRestoreStatus::Ready || slot >= Count || !pending_ || *pending_ != slot) return false;
    acknowledged_[slot] = targets_[slot]; pending_.reset();
    return true;
  }
 private:
  SaveSlotRestoreStatus status_{SaveSlotRestoreStatus::Collecting};
  size_t slot_{};
  uint16_t received_{}, value_{};
  Table restored_{}, acknowledged_{}, targets_{};
  std::optional<size_t> pending_;
};
} // namespace echoes
