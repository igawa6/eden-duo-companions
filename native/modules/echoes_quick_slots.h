// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_live.h"
#include <algorithm>
#include <array>
#include <optional>

namespace echoes {
// Companion-local shortcuts, never additional native equipment. Sync must use
// an authenticated current snapshot; readiness is revoked on unavailable reads.
class QuickSlots {
 public:
  static constexpr uint16_t Empty = 0xffff;
  static constexpr size_t Count = 3;
  void Sync(const LiveSnapshot& value) {
    ready_ = false;
    active_.reset();
    if (!value.compatible || !value.collection_valid) return;
    const auto& source = value.collection_source;
    const bool ownership_change = !initialized_ || owner_ != source.access_object || save_ != source.save_data;
    if (ownership_change) {
      slots_.fill(Empty);
      owner_ = source.access_object; save_ = source.save_data;
      initialized_ = true;
      seeded_ = false;
    }
    learned_ = value.learned_native_ids;
    learned_.erase(std::remove(learned_.begin(), learned_.end(), Empty), learned_.end());
    std::sort(learned_.begin(), learned_.end());
    learned_.erase(std::unique(learned_.begin(), learned_.end()), learned_.end());
    for (auto& id : slots_) if (!Learned(id)) id = Empty;
    // First initialization prioritizes verified equipment; all other defaults
    // use stable native-ID order, independent of localized or native sort mode.
    const bool equipped = value.selector.equipped_valid && Learned(value.selector.equipped_native_id);
    // Native collection can become readable before native equipment at boot.
    // Keep the initial shortcuts unavailable until that first verified ID, so
    // seeded low IDs cannot crowd out the player's actually equipped Echo.
    if (!seeded_ && !equipped) return;
    if (!seeded_) {
      auto empty = std::find(slots_.begin(), slots_.end(), Empty);
      *empty = value.selector.equipped_native_id;
      seeded_ = true;
    }
    for (auto& slot : slots_) if (slot == Empty) {
      const auto next = std::find_if(learned_.begin(), learned_.end(), [&](uint16_t id) {
        return std::find(slots_.begin(), slots_.end(), id) == slots_.end();
      });
      if (next != learned_.end()) slot = *next;
    }
    if (equipped) {
      const auto at = std::find(slots_.begin(), slots_.end(), value.selector.equipped_native_id);
      if (at != slots_.end()) active_ = static_cast<size_t>(at - slots_.begin());
    }
    ready_ = true;
  }
  // Tap-grid-then-slot and drag/drop share this exact operation. A duplicate
  // assignment swaps slots, preserving three distinct shortcuts. It does not
  // equip and never optimistically changes the active green/Y indication.
  bool Assign(size_t slot, uint16_t id) {
    if (!ready_ || slot >= Count || !Learned(id)) return false;
    const auto other = std::find(slots_.begin(), slots_.end(), id);
    if (other != slots_.end()) {
      const auto index = static_cast<size_t>(other - slots_.begin());
      std::swap(slots_[slot], slots_[index]);
      if (active_) {
        if (*active_ == slot) active_ = index;
        else if (*active_ == index) active_ = slot;
      }
    } else {
      if (active_ == slot) active_.reset();
      slots_[slot] = id;
    }
    return true;
  }
  // Merely returns a candidate. Caller must freshly revalidate ownership,
  // gameplay context and action arbitration before StartEchoIntent(Equip).
  std::optional<uint16_t> Target(size_t slot) const {
    if (!ready_ || slot >= Count || slots_[slot] == Empty) return {};
    return slots_[slot];
  }
  bool ready() const { return ready_; }
  const std::array<uint16_t, Count>& slots() const { return slots_; }
  std::optional<size_t> active() const { return active_; }
 private:
  bool Learned(uint16_t id) const { return std::binary_search(learned_.begin(), learned_.end(), id); }
  std::array<uint16_t, Count> slots_{Empty, Empty, Empty};
  std::vector<uint16_t> learned_;
  uint64_t owner_{}, save_{};
  bool initialized_{}, seeded_{}, ready_{};
  std::optional<size_t> active_;
};
} // namespace echoes
