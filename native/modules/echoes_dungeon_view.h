// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_dungeon_assets.h"
#include <algorithm>
#include <optional>
namespace echoes {
struct DungeonViewState {
  std::string key;
  int32_t floor{};
  MapEpochSource source{};
  uint64_t visited{};
  bool map_item{};
  std::vector<DungeonRoomVariant> variants;
};
struct DungeonViewSlot : DungeonViewState {
  bool used{};
  size_t generation{};
};
// A host widget may retain its last-good image while another URI decodes.
// Reuse one widget per floor only while every intervening observation remains
// continuous and at least as revealing. A gap or restriction creates a fresh
// generation that can never find an earlier widget, even if its state returns.
// This cache grants no gameplay/discovery permission; the caller authenticates
// every input and withdraws whenever any required observation is unavailable.
class DungeonViewCache {
public:
  static constexpr size_t Capacity = 64;
  static bool SameVariants(std::span<const DungeonRoomVariant> a,
                           std::span<const DungeonRoomVariant> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](const auto &x, const auto &y) {
                                                return x.room == y.room &&
                                                       x.known == y.known &&
                                                       x.texture == y.texture;
                                              });
  }
  void Withdraw() {
    continuous_ = false;
    current_.reset();
  }
  std::optional<size_t> Accept(const DungeonViewState &next) {
    if (next.key.empty() || next.key.size() > 63 || next.variants.size() > 64) {
      Withdraw();
      return {};
    }
    const bool same = continuous_ && last_.key == next.key &&
                      last_.source == next.source &&
                      (last_.visited & next.visited) == last_.visited &&
                      (!last_.map_item || next.map_item) &&
                      SameVariants(last_.variants, next.variants);
    if (!same) {
      if (used_ == Capacity) {
        Withdraw();
        return {};
      }
      ++generation_;
    }
    size_t selected = used_;
    if (same) {
      for (size_t i = 0; i < used_; ++i)
        if (slots_[i].generation == generation_ && slots_[i].key == next.key &&
            slots_[i].source == next.source && slots_[i].floor == next.floor) {
          selected = i;
          break;
        }
    }
    if (selected == used_) {
      if (used_ == Capacity) {
        Withdraw();
        return {};
      }
      ++used_;
    }
    static_cast<DungeonViewState &>(slots_[selected]) = next;
    slots_[selected].used = true;
    slots_[selected].generation = generation_;
    last_ = next;
    current_ = selected;
    continuous_ = true;
    return current_;
  }
  const std::array<DungeonViewSlot, Capacity> &slots() const { return slots_; }
  size_t used() const { return used_; }
  size_t generation() const { return generation_; }
  std::optional<size_t> current() const { return current_; }

private:
  std::array<DungeonViewSlot, Capacity> slots_{};
  DungeonViewState last_;
  size_t used_{}, generation_{};
  bool continuous_{};
  std::optional<size_t> current_;
};
} // namespace echoes
