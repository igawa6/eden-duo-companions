// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../guest/ot0_stats_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "ot0_screen.h"
#include <mutex>
namespace ot0 {
// Mailbox client for the helper's field Status values (ABI3). Request() names the member to
// read; Poll() returns values only when the helper answered for exactly that member within the
// last 120 game frames. Display order maps native ids to the Status page labels.
class NativeStats {
public:
  void Configure(const EdenDsmodHostExtensions &ext);
  MemberStats Poll(uint32_t member_id);

private:
  bool Load(uint32_t offset, uint32_t &value) const;
  bool Store(uint32_t offset, uint32_t value) const;
  mutable std::mutex mutex_;
  EdenDsmodHostExtensions ext_{};
  bool configured_{};
  uint32_t requested_{};
  MemberStats Fresh(uint32_t member_id);
};
// Native ids 9..16 (helper order) -> Status page order Phys. Atk, Elem. Atk, Phys. Def,
// Elem. Def, Accuracy, Speed, Critical, Evasion. Live-confirmed 2026-10-31 against the native
// Status page: 9 Phys. Atk, 10 Phys. Def, 11 Elem. Atk, 12 Elem. Def, 13 Speed, 14 Critical,
// 15 Accuracy, 16 Evasion.
inline constexpr std::array<int, 8> StatusOrder{9, 11, 10, 12, 15, 13, 14, 16};
// Selector arguments after (member, stat): w2..w7 and the stacked byte, as the Status page
// passes them (current job, no level override, equipment and bonuses included).
inline constexpr std::array<int32_t, 7> SelectorArgs{0, 0, 1, 1, 1, 1, 0};
} // namespace ot0
