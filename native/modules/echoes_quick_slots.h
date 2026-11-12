// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_live.h"
#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace echoes {
// Companion-local shortcut rail. Three circles: the player may assign any of
// them by hand (tap-grid-then-slot, drag onto a circle); an unassigned circle
// auto-fills from this exact save's own favourites, then Last Used order.
// A hand assignment is a per-save REFERENCE, not an extra inventory: it is
// shown only while that Echo is still learned in the save it was made in,
// and native-ID order is never used anywhere in this class.
class QuickSlots {
 public:
  static constexpr uint16_t Empty = 0xffff;
  static constexpr size_t Count = 3;
  void Sync(const LiveSnapshot& value) {
    ready_ = false;
    active_.reset();
    slots_.fill(Empty);
    if (!value.compatible || !value.collection_valid) {
      // Transient (most commonly a loading screen): every cache -- auto-fill
      // order AND the hand-assigned slots -- survives untouched. Only a
      // confirmed different save (below) ever clears them.
      return;
    }
    const auto& source = value.collection_source;
    const bool ownership_change =
        !initialized_ || owner_ != source.access_object || save_ != source.save_data;
    if (ownership_change) {
      owner_ = source.access_object;
      save_ = source.save_data;
      initialized_ = true;
      ownership_changed_pending_ = true;
      // A different save's cached order and hand assignments must never
      // leak into this one. The caller resolves this save's own persisted
      // assignments (if any) via ConsumeOwnershipChange()+SetManual().
      last_used_.clear();
      favorites_.clear();
      manual_.fill(Empty);
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

    // Hand assignments take their own fixed circle, but only while that
    // Echo is still learned in THIS save ("save as reference": a stored
    // shortcut is a pointer into the save's own collection, never content
    // of its own). An unlearned or never-assigned circle stays open for
    // auto-fill below.
    for (size_t i = 0; i < Count; ++i)
      if (manual_[i] != Empty && Learned(manual_[i])) slots_[i] = manual_[i];

    const auto placed = [&](uint16_t id) { return std::find(slots_.begin(), slots_.end(), id) != slots_.end(); };
    const auto fill = [&](uint16_t id) {
      if (id == Empty || !Learned(id) || placed(id)) return;
      for (auto& slot : slots_) if (slot == Empty) { slot = id; return; }
    };
    for (const auto id : favorites_) fill(id);
    for (const auto id : last_used_) fill(id);

    if (value.selector.equipped_valid) {
      const auto at = std::find(slots_.begin(), slots_.end(), value.selector.equipped_native_id);
      if (at != slots_.end()) active_ = static_cast<size_t>(at - slots_.begin());
    }
    ready_ = true;
  }
  // Tap-grid-then-slot and drag/drop share this exact operation. A duplicate
  // assignment swaps the two hand-assigned circles, preserving three distinct
  // shortcuts; it never touches an auto-filled circle's content directly
  // (Sync's de-duplication keeps auto-fill out of a circle's way once a hand
  // assignment claims it). It does not equip and never optimistically
  // changes the active green/Y indication -- the next Sync recomputes that.
  bool Assign(size_t slot, uint16_t id) {
    if (!ready_ || slot >= Count || !Learned(id)) return false;
    const auto other = std::find(manual_.begin(), manual_.end(), id);
    if (other != manual_.end()) {
      std::swap(manual_[slot], *other);
    } else {
      manual_[slot] = id;
    }
    dirty_ = true;
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

  // -- Per-save persistence hooks (Module owns the actual store/identity) --
  // True exactly once per detected save change (including the first save
  // seen this boot); the caller resolves the new save's own persisted
  // assignments (if any) and calls SetManual, or leaves everything auto.
  bool ConsumeOwnershipChange() { const bool v = ownership_changed_pending_; ownership_changed_pending_ = false; return v; }
  const std::array<uint16_t, Count>& manual() const { return manual_; }
  void SetManual(const std::array<uint16_t, Count>& restored) { manual_ = restored; }
  // True after Assign() changed a hand assignment; the caller persists it
  // (keyed by this save's stable identity) and then clears it.
  bool dirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }

 private:
  bool Learned(uint16_t id) const { return std::binary_search(learned_.begin(), learned_.end(), id); }
  size_t LastUsedRank(uint16_t id) const {
    const auto at = std::find(last_used_.begin(), last_used_.end(), id);
    return at == last_used_.end() ? last_used_.size() : static_cast<size_t>(at - last_used_.begin());
  }
  std::array<uint16_t, Count> slots_{Empty, Empty, Empty};
  std::array<uint16_t, Count> manual_{Empty, Empty, Empty};
  std::vector<uint16_t> learned_, last_used_, favorites_;
  uint64_t owner_{}, save_{};
  bool initialized_{}, ready_{}, ownership_changed_pending_{}, dirty_{};
  std::optional<size_t> active_;
};
} // namespace echoes
