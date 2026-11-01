// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_reader.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ot0 {
// One member's equipped items (save S+0x158 records, stride 0x98, key = the party id; the
// protagonist uses its current job's record 900 + job): weapons +0x08 (2), armour +0x18 (3: shield, head, body), accessories
// +0x28 (2); each a TArray of item ids, 0 = empty (research analysis/20261031-items-quests).
struct Gear {
  bool valid{};
  uint32_t member{};
  std::array<uint32_t, 2> weapons{};
  std::array<uint32_t, 3> armor{};
  std::array<uint32_t, 2> accessories{};
  bool operator==(const Gear &) const = default;
};
Gear ReadGear(const ReadMemory &read, uint64_t main, int64_t delta, uint32_t member);
// Owned items (save S+0x198, 12-byte {item id, count, flag} entries in acquisition order; the
// native "Obtained" tab lists them newest first).
struct InventoryEntry {
  uint32_t item{};
  int32_t count{};
  bool operator==(const InventoryEntry &) const = default;
};
struct Inventory {
  bool valid{};
  std::vector<InventoryEntry> entries;
  std::string favorite_dish; // the player's favorite-dish name (save S+0xa00 FString), UTF-8
  bool operator==(const Inventory &) const = default;
};
Inventory ReadInventory(const ReadMemory &read, uint64_t main, int64_t delta);
// Quest progress (save S+0x1d8, 0x38-byte records: +0 QuestList id, +4 EQUEST_STATE 0 none,
// 1 ready/available, 2 active, 3 cleared).
struct QuestState {
  uint32_t quest{}, state{};
  // The native list's own gate: the quest's detail (QPM map [main+0x7cc8380], TSet elements
  // +0x320 stride 0x18 {id, -, detail*}) has no pending list condition at detail+0x3c.
  bool listed{};
  // "Next destination": GameTextQuest id of the current step's task row (detail+0x280 rows,
  // stride 0x138, step index detail+0x2b0, text id at row+0x24); 0 = none.
  uint32_t destination{};
  bool operator==(const QuestState &) const = default;
};
struct QuestStates {
  bool valid{};
  std::vector<QuestState> entries;
  bool operator==(const QuestStates &) const = default;
};
QuestStates ReadQuestStates(const ReadMemory &read, uint64_t main, int64_t delta);
} // namespace ot0
