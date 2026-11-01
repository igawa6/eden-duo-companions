// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
namespace ot0 {
struct PartyMember {
  uint32_t id{}, level{}, hp{}, sp{};
  bool available{};
  uint32_t max_hp{}, max_sp{}; // Optional, fresh native menu values; zero is unknown.
  uint32_t exp{};              // Native total EXP (party record +0x8).
  bool operator==(const PartyMember &) const = default;
};
struct Snapshot {
  uint8_t state{};
  bool valid{};
  uint32_t active_party{};
  uint32_t money{}; // Native leaves (save +0x40); results are added on dismissing the results.
  std::array<PartyMember, 8> members{};
  bool operator==(const Snapshot &) const = default;
};
using ReadMemory = std::function<bool(uint64_t, void *, size_t)>;
// Vtables may retain the guest address or point into NCE's relocated code
// image. The host supplies the signed delta; this is not a memory-search
// heuristic.
inline bool MatchesVtable(uint64_t actual, uint64_t expected, int64_t delta) {
  return actual == expected ||
         (delta && actual == expected + static_cast<uint64_t>(delta));
}
// Normal formation only. No calls into the guest and no writes.
Snapshot ReadSnapshot(const ReadMemory &read, uint64_t main,
                      int64_t relocation_delta = 0);
} // namespace ot0
