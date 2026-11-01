// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_reader.h"
#include <array>
#include <string>
namespace ot0 {
struct EquipmentStatComparison {
  uint32_t parameter_id{};
  std::string label;
  int32_t current{}, candidate{};
  bool operator==(const EquipmentStatComparison &) const = default;
};
struct EquipmentMemberComparison {
  uint32_t id{};
  std::array<EquipmentStatComparison, 4> stats{};
  uint8_t stat_count{};
  bool valid{}, cannot_equip{};
  bool operator==(const EquipmentMemberComparison &) const = default;
};
struct ShopComparisonSnapshot {
  uint32_t candidate_item_id{}, selected_member_id{};
  std::string candidate_name;
  std::array<EquipmentMemberComparison, 8> members{};
  bool valid{};
  bool operator==(const ShopComparisonSnapshot &) const = default;
};
// Current native shop selection and displayed native computed comparisons.
// Member indexes match the caller's normal formation. No guest calls/writes.
ShopComparisonSnapshot ReadShopComparison(const ReadMemory &read, uint64_t main,
                                          const Snapshot &party,
                                          int64_t relocation_delta = 0);
} // namespace ot0
