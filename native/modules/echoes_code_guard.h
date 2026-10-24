// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
namespace echoes {
enum class GuardDomain { Data, A64Code };
// The guest helper's entry hook (tools/echoes-of-wisdom/hook.S, --hook-kind entry).
inline constexpr uint64_t HelperHookSite = 0x2595d60;
inline constexpr uint32_t HelperHookOriginal = 0xd10103ffu; // sub sp, sp, #0x40
// Matches Core::NCE::Exclusive::Verify in the supported runtime. Only inspect
// trusted original instructions; an observed opcode cannot authorize a rewrite.
constexpr bool OriginalExclusive(uint32_t word) {
  return ((word >> 23) & 0x7f) == 0x10 &&
         (((word >> 21) & 1) || ((word >> 10) & 31) == 31);
}
// Device-qualified on the supported original build: both main+21cdae0 and
// main+24db2d0 changed C85F7D09 -> C85FFD09 under NCE.
// Data is always exact. A64 windows must contain complete aligned LE words.
// The optional exception is exclusively original -> original|bit15, matching
// Exclusive::AsOrdered; no bit clearing, branch rewriting or other delta passes.
inline bool MatchOriginalGuard(std::span<const uint8_t> observed,
                               std::span<const uint8_t> original,
                               uint64_t offset, GuardDomain domain,
                               bool allow_ordered_exclusives = false) {
  if (original.empty() || original.size() > 4096 ||
      observed.size() != original.size()) return false;
  if (domain == GuardDomain::Data)
    return std::equal(observed.begin(), observed.end(), original.begin());
  if (domain != GuardDomain::A64Code || (offset & 3) || (original.size() & 3))
    return false;
  const auto word = [](std::span<const uint8_t> b, size_t i) {
    return uint32_t(b[i]) | uint32_t(b[i+1]) << 8 |
           uint32_t(b[i+2]) << 16 | uint32_t(b[i+3]) << 24;
  };
  for (size_t i = 0; i < original.size(); i += 4) {
    const uint32_t expected = word(original,i), actual = word(observed,i);
    if (actual == expected) continue;
    // Our own load-plan hook: the HeartGauge update entry becomes a plain B to
    // the guest helper (the plan verifies the original bytes before writing).
    if (offset + i == HelperHookSite && expected == HelperHookOriginal &&
        (actual & 0xFC000000u) == 0x14000000u) continue;
    if (!allow_ordered_exclusives || !OriginalExclusive(expected) ||
        actual != (expected | 0x8000u)) return false;
  }
  return true;
}
// Original supported main .text extent, corroborated by retained segments and
// runtime executable mapping. Offsets in rodata/data retain exact comparison.
inline bool MatchSupportedGuard(std::span<const uint8_t> observed,
                                std::span<const uint8_t> original,
                                uint64_t offset) {
  constexpr uint64_t TextSize = 0x2837000;
  const bool code = offset < TextSize && original.size() <= TextSize-offset;
  return MatchOriginalGuard(observed, original, offset,
                           code ? GuardDomain::A64Code : GuardDomain::Data,
                           code);
}
} // namespace echoes
