// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace echoes {
enum class QuickSlotRestoreStatus { Collecting, Ready, Refused };
// Runtime22 flag transport, independent of native guest memory. Each persisted
// unsigned16 value encodes native ID+1; zero means unset. The current restore
// slot stays fixed throughout enforce; only Sample may advance it.
class QuickSlotFlagBridge {
 public:
  static constexpr size_t Count=3;
  using Restored=std::array<std::optional<uint16_t>,Count>;
  QuickSlotRestoreStatus status() const { return status_; }
  bool restore_pending() const { return status_==QuickSlotRestoreStatus::Collecting; }
  size_t restore_slot() const { return slot_; }
  uint16_t received_mask() const { return received_; }
  void RefuseRestore() {
    status_=QuickSlotRestoreStatus::Refused;received_=value_=0;restored_={};pending_=0;
  }
  // bit*2+value, 0..31; -1 refuses invalid runtime flags. Equal duplicates are
  // idempotent, conflicting packets reject the complete restore transaction.
  bool RestorePacket(int64_t packet) {
    if(status_==QuickSlotRestoreStatus::Refused)return false;
    if(packet<0 || packet>=32){RefuseRestore();return false;}
    if(status_==QuickSlotRestoreStatus::Ready)return true;
    const auto mask=static_cast<uint16_t>(1u<<unsigned(packet/2));
    const bool set=(packet%2)!=0;
    if((received_&mask) && bool(value_&mask)!=set){RefuseRestore();return false;}
    received_|=mask;if(set)value_|=mask;return true;
  }
  // Call only at Sample start. Writes output atomically once all three flags
  // arrived. On later calls returns ready without replacing the caller's data.
  // Root must apply the output once, validating IDs against current ownership.
  bool AdvanceRestore(Restored& out) {
    if(status_!=QuickSlotRestoreStatus::Collecting)return status_==QuickSlotRestoreStatus::Ready;
    if(received_!=UINT16_MAX)return false;
    if(slot_>=Count){RefuseRestore();return false;}
    restored_[slot_]=value_;received_=value_=0;
    if(++slot_<Count)return false;
    Restored validated{};
    for(size_t i=0;i<Count;++i)if(restored_[i])validated[i]=static_cast<uint16_t>(restored_[i]-1);
    out=validated;acknowledged_=targets_=restored_;status_=QuickSlotRestoreStatus::Ready;return true;
  }
  // Call in Sample ONLY, after restore has been applied/rejected against the
  // learned collection. Action callbacks must defer QueueWrites until Sample:
  // otherwise old derived equality gates could ACK an unpublished new target.
  bool QueueWrites(const std::array<uint16_t,Count>& ids) {
    if(status_!=QuickSlotRestoreStatus::Ready)return false;
    pending_=0;
    for(size_t i=0;i<Count;++i){
      targets_[i]=ids[i]==UINT16_MAX?0:static_cast<uint16_t>(ids[i]+1u);
      if(targets_[i]!=acknowledged_[i])pending_|=uint8_t(1u<<i);
    }
    return true;
  }
  bool writes_pending() const { return pending_!=0; }
  bool write_pending(size_t slot) const { return slot<Count && (pending_&(1u<<slot)); }
  int64_t write_value(size_t slot) const { return slot<Count?targets_[slot]:0; }
  // Module action must be protected by the exact published flag==target gate.
  bool AcknowledgeWrite(size_t slot) {
    if(status_!=QuickSlotRestoreStatus::Ready || slot>=Count)return false;
    acknowledged_[slot]=targets_[slot];pending_&=uint8_t(~(1u<<slot));return true;
  }
 private:
  QuickSlotRestoreStatus status_{QuickSlotRestoreStatus::Collecting};
  size_t slot_{};
  uint16_t received_{},value_{};
  std::array<uint16_t,Count> restored_{},acknowledged_{},targets_{};
  uint8_t pending_{};
};
} // namespace echoes
