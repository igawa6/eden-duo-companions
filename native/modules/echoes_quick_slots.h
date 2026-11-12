// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_live.h"
#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace echoes {
// Companion-local shortcut RAIL, never additional native equipment or
// storage. Every sample recomputes the three circles from this exact save's
// own favourites and Last Used order; nothing here is ever assigned by hand
// or persisted, so a save switch (or any unreadable sample, which can be the
// same moment a save switch is in flight) can never leave a previous save's
// Echoes on screen.
class QuickSlots {
 public:
  static constexpr uint16_t Empty = 0xffff;
  static constexpr size_t Count = 3;
  void Sync(const LiveSnapshot& value) {
    ready_ = false;
    active_.reset();
    slots_.fill(Empty);
    if (!value.compatible || !value.collection_valid) {
      // Unreadable: drop every cache rather than risk it surviving into a
      // save this read never actually confirmed.
      initialized_ = false;
      last_used_.clear();
      favorites_.clear();
      return;
    }
    const auto& source = value.collection_source;
    const bool ownership_change =
        !initialized_ || owner_ != source.access_object || save_ != source.save_data;
    if (ownership_change) {
      owner_ = source.access_object;
      save_ = source.save_data;
      initialized_ = true;
      // A different save's cached order must never leak into this one.
      last_used_.clear();
      favorites_.clear();
    }
    learned_ = value.learned_native_ids;
    std::sort(learned_.begin(), learned_.end());
    learned_.erase(std::unique(learned_.begin(), learned_.end()), learned_.end());

    // Cache the held selector's own display order only once it is confirmed
    // to be the Last Used sort (bit 0 of sort_enum); any other active sort
    // is not "Last Used order" and must not overwrite a previously-cached
    // one from this same save.
    if (value.selector.held && value.selector.sort_valid && value.selector.sort_enum == 1)
      last_used_ = value.selector.ordered_native_ids;

    // Favourites: the held selector's own favourites page order is
    // authoritative when readable (exactly how the game itself shows
    // them); otherwise fall back to the independently-sampled save CRC
    // set, ordered by the cached Last Used position when one is known.
    if (value.selector.held && value.selector.favorite_valid) {
      favorites_ = value.selector.favorite_native_ids;
    } else if (value.favorite_valid) {
      std::vector<uint16_t> mapped;
      for (const auto crc : value.favorite_crc_keys) {
        const auto at = std::find(value.learned_crc_keys.begin(), value.learned_crc_keys.end(), crc);
        if (at == value.learned_crc_keys.end()) continue;
        const auto index = static_cast<size_t>(at - value.learned_crc_keys.begin());
        if (index < value.learned_native_ids.size()) mapped.push_back(value.learned_native_ids[index]);
      }
      std::stable_sort(mapped.begin(), mapped.end(), [&](uint16_t a, uint16_t b) {
        return LastUsedRank(a) < LastUsedRank(b);
      });
      favorites_ = std::move(mapped);
    }

    std::vector<uint16_t> order;
    order.reserve(Count);
    const auto add = [&](uint16_t id) {
      if (order.size() >= Count || id == Empty || !Learned(id)) return;
      if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    };
    for (const auto id : favorites_) add(id);
    for (const auto id : last_used_) add(id);
    std::copy(order.begin(), order.end(), slots_.begin());

    if (value.selector.equipped_valid) {
      const auto at = std::find(slots_.begin(), slots_.end(), value.selector.equipped_native_id);
      if (at != slots_.end()) active_ = static_cast<size_t>(at - slots_.begin());
    }
    ready_ = true;
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
  size_t LastUsedRank(uint16_t id) const {
    const auto at = std::find(last_used_.begin(), last_used_.end(), id);
    return at == last_used_.end() ? last_used_.size() : static_cast<size_t>(at - last_used_.begin());
  }
  std::array<uint16_t, Count> slots_{Empty, Empty, Empty};
  std::vector<uint16_t> learned_, last_used_, favorites_;
  uint64_t owner_{}, save_{};
  bool initialized_{}, ready_{};
  std::optional<size_t> active_;
};
} // namespace echoes
