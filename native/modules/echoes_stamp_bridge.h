// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_stamps.h"
namespace echoes {
enum class StampRestoreStatus { Collecting, Ready, Refused };
// Frozen-runtime22 transport: declarative gates read persisted flags and send
// constant (bit*2 + value) module actions. No module host flag-query assumption.
// All40 bits arrive for one slot before AdvanceRestore moves to the next slot;
// publish slot/remaining state only from Sample, never midway through enforce.
class StampFlagBridge {
public:
  StampRestoreStatus status() const { return status_; }
  bool restore_pending() const { return status_ == StampRestoreStatus::Collecting; }
  size_t restore_slot() const { return slot_; }
  uint64_t received_mask() const { return received_; }
  //0..79 encodes bit0..39 and value0/1. -1 explicitly refuses corrupt flag.
  // Duplicate identical packets are idempotent; conflicting packets refuse.
  bool RestorePacket(int64_t);
  // Call ONLY at beginning of Sample. The external store changes once, after
  // all24 records strictly validate. Returns true exactly when ready.
  bool AdvanceRestore(StampStore&);
  void RefuseRestore();
  // Call ONLY in Sample after restore is ready. Edits made by input actions
  // should change StampStore and defer QueueWrites to the next Sample, so old
  // derived ACK gates cannot acknowledge a new unpublished target value.
  bool QueueWrites(const StampStore&);
  bool write_pending(size_t) const;
  bool writes_pending() const { return pending_ != 0; }
  int64_t write_value(size_t) const;
  // Call only through per-slot declarative gate comparing persisted flag to
  // the exact published write_value. The bridge cannot itself read hostflags.
  bool AcknowledgeWrite(size_t);
private:
  StampRestoreStatus status_{StampRestoreStatus::Collecting};
  size_t slot_{};
  uint64_t received_{}, value_{};
  PackedStamps restored_{}, acknowledged_{}, targets_{};
  uint32_t pending_{};
};
} // namespace echoes
