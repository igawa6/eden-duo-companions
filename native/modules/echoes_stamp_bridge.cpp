// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_stamp_bridge.h"
namespace echoes {
namespace { constexpr uint64_t Complete = (UINT64_C(1) << 40) - 1; }
void StampFlagBridge::RefuseRestore() {
  status_ = StampRestoreStatus::Refused;
  received_ = value_ = 0; restored_ = {}; pending_ = 0;
}
bool StampFlagBridge::RestorePacket(int64_t packet) {
  if (status_ == StampRestoreStatus::Refused) return false;
  if (packet < 0 || packet >= 80) { RefuseRestore(); return false; }
  if (status_ == StampRestoreStatus::Ready) return true;
  const auto bit = static_cast<unsigned>(packet / 2);
  const auto mask = UINT64_C(1) << bit;
  const bool set = (packet % 2) != 0;
  if ((received_ & mask) && ((value_ & mask) != 0) != set) {
    RefuseRestore(); return false;
  }
  received_ |= mask;
  if (set) value_ |= mask;
  return true;
}
bool StampFlagBridge::AdvanceRestore(StampStore& out) {
  if (status_ != StampRestoreStatus::Collecting)
    return status_ == StampRestoreStatus::Ready;
  if (received_ != Complete) return false;
  StampRecord validated;
  if (slot_ >= StampCapacity || !UnpackStamp(static_cast<int64_t>(value_), validated)) {
    RefuseRestore(); return false;
  }
  restored_[slot_] = static_cast<int64_t>(value_);
  received_ = value_ = 0;
  if (++slot_ < StampCapacity) return false;
  if (!out.Restore(restored_)) { RefuseRestore(); return false; }
  acknowledged_ = targets_ = restored_;
  status_ = StampRestoreStatus::Ready;
  return true;
}
bool StampFlagBridge::QueueWrites(const StampStore& store) {
  if (status_ != StampRestoreStatus::Ready) return false;
  targets_ = store.Packed(); pending_ = 0;
  for (size_t i = 0; i < StampCapacity; ++i)
    if (targets_[i] != acknowledged_[i]) pending_ |= uint32_t(1) << i;
  return true;
}
bool StampFlagBridge::write_pending(size_t slot) const {
  return slot < StampCapacity && (pending_ & (uint32_t(1) << slot));
}
int64_t StampFlagBridge::write_value(size_t slot) const {
  return slot < StampCapacity ? targets_[slot] : 0;
}
bool StampFlagBridge::AcknowledgeWrite(size_t slot) {
  if (status_ != StampRestoreStatus::Ready || slot >= StampCapacity) return false;
  acknowledged_[slot] = targets_[slot];
  pending_ &= ~(uint32_t(1) << slot);
  return true;
}
} // namespace echoes
