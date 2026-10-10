// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared build-pinned guest layout of Dragon Quest III HD-2D Remake 1.1.0.0 (build 4F41309B39EEBE5E):
// the offsets more than one reader walks. Each value is pinned by the code named next to it (the
// pins themselves live with the reader that checks them: dq3_reader.cpp Pins(), dq3_map.cpp
// MapPins(), dq3_info.cpp InfoPins()). Internal header.
#pragma once

#include <cstdint>

namespace dq3::layout {

// G -> GD -> P: GD accessor main+0x87437c (ldr x8,[x8,#0x528]; add x0,x8,#0x18), dq3_reader.h.
inline constexpr std::uint64_t GGd = 0x528; ///< GD = [G + 0x528]
inline constexpr std::uint64_t GdP = 0x18;  ///< P = [GD + 0x18]
/// PM = [P + 0x220] (HealPartyMember main+0x8bc1c8: ldr x0,[x8,#0x220]).
inline constexpr std::uint64_t PPartyManager = 0x220;
/// The progress bitset P + 0x128 + 0xC8 (main+0x877570: qword [x + 0xc8 + 8 * (i >> 6)]).
inline constexpr std::uint64_t PProgress = 0x1f0;

// UE4 reflection layout (verified live by class / object names, contracts/LOCATION.md).
inline constexpr std::uint64_t ObjClass = 0x10, ObjName = 0x18, ObjOuter = 0x20;
/// UWorld subsystem TMap {data, num}, 0x18-byte entries {UClass*, object, hash}.
inline constexpr std::uint64_t WorldSubsystems = 0x6e8;

} // namespace dq3::layout
