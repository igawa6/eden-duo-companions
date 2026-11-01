// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// The HP the TOP screen's HP plates show (owner dev.18: "bottom screen hp is depleted first than
// top screen"). The battle struct's CurHP (sw_battle.h +0x74) drops when the server applies the
// damage, seconds before the top screen's gauge starts to drain (the move animation plays first).
//
// Live-found 2026-10-31 (label battle, 3 wild battles, one boot): each HP plate has a small data
// object the battle UI fills when it starts the gauge animation:
//
//   +0x08  u32  0x101                (flags; a torn-down copy reads 0x100)
//   +0x18  u64  changes over time (not a tag)
//   +0x20  u32  max HP
//   +0x24  u32  the HP the plate drains TO (changes when the top gauge starts, ~2.5-3 s after
//               the battle struct's CurHP for a Thunderbolt KO; unchanged while the move plays)
//   +0x30  u32  488, +0x38 u32 248   (plate geometry, constant)
//
// The objects are plain heap allocations (no static pointer chain was found from the battle
// client within 3 hops), at heap+0x849Dxxxx on the boot measured. The module scans a window around
// that offset incrementally (kScanStep per tick) once per battle, and only accepts an object
// whose max HP equals the battler's own max HP and whose drain target equals the battler's HP at
// the moment it is found. Not cross-boot verified: on a miss the caller falls back to the battle
// struct's HP (the drain still animates at the native rate, just without the start delay).
//
// The drain itself is animated module-side over a fixed time (kDrainTicks), like the native gauge.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace sw_battle_gauge {

inline constexpr std::uint64_t kWindowBegin = 0x80000000ull; // heap-relative
inline constexpr std::uint64_t kWindowEnd = 0x89000000ull;
inline constexpr std::uint64_t kScanStep = 0x400000;          // bytes scanned per tick (4 MiB)
inline constexpr std::uint64_t kChunk = 0x10000;
// The native gauge takes about the same time for any drain (live 2026-10-31: a 20 HP hit was ~60 %
// drained ~1.2 s after the plate's target changed, a 154 HP KO ~35 % after ~0.7 s): ~2 s each.
inline constexpr int kDrainTicks = 120;                         // module ticks (60 Hz) per drain

struct Plate {
    std::uint32_t max = 0, target = 0;
};

inline bool ParsePlate(const std::uint8_t* p, Plate& out) {
    std::uint32_t flags, max, target, w, h, w_hi, h_hi;
    std::memcpy(&flags, p + 0x08, 4);
    std::memcpy(&w_hi, p + 0x34, 4);
    std::memcpy(&h_hi, p + 0x3C, 4);
    std::memcpy(&max, p + 0x20, 4);
    std::memcpy(&target, p + 0x24, 4);
    std::memcpy(&w, p + 0x30, 4);
    std::memcpy(&h, p + 0x38, 4);
    if (flags != 0x101 || w_hi != 0 || h_hi != 0 || w != 488 || h != 248 || max == 0 || max > 999 || target > max) return false;
    out = {max, target};
    return true;
}

// Steps `shown` toward `target` so that a drain of `span` HP (the distance when the target last
// changed) takes kDrainTicks: at least 1 HP per tick.
inline int Step(int shown, int target, int span) {
    if (shown < 0) return target;
    const int rate = std::max(1, (std::abs(span) + kDrainTicks - 1) / kDrainTicks);
    if (shown > target) return std::max(target, shown - rate);
    if (shown < target) return std::min(target, shown + rate);
    return shown;
}

// One battler's displayed HP: follows the plate object's drain target once found, else the
// battle struct's HP; animated by Step.
struct Follower {
    std::uint64_t plate = 0; // address of a matched plate object (0 = none)
    std::uint64_t rejected = 0; // a plate dropped as stale (never re-matched this battler)
    int shown = -1;
    int mismatch = 0;        // ticks the plate's target has disagreed with the battle struct
    int last_target = -1, span = 0; // the current drain's start distance (Step's rate)
    std::uint16_t species = 0;
    void Reset() { *this = {}; }
};

// Incremental scanner: call Advance() each tick with a read callback; it reports candidate plate
// addresses through `on_plate(address, Plate)`.
struct Scanner {
    std::uint64_t next = 0; // heap-relative cursor, 0 = not started
    bool done = false;
    void Reset() { next = 0; done = false; }
    template <typename Read, typename OnPlate>
    void Advance(std::uint64_t heap_begin, Read&& read, OnPlate&& on_plate) {
        if (done || !heap_begin) return;
        if (next == 0) next = kWindowBegin;
        static thread_local std::uint8_t buf[kChunk + 0x40]; // per sampling thread, as HolderLayout
        const std::uint64_t stop = std::min(kWindowEnd, next + kScanStep);
        for (; next < stop; next += kChunk) {
            if (!read(heap_begin + next, buf, sizeof buf)) continue;
            for (std::size_t off = 0; off + 0x40 <= sizeof buf && off < kChunk; off += 8) {
                std::uint32_t w;
                std::memcpy(&w, buf + off + 0x30, 4);
                if (w != 488) continue;
                Plate p;
                if (ParsePlate(buf + off, p)) on_plate(heap_begin + next + off, p);
            }
        }
        if (next >= kWindowEnd) done = true;
    }
};

} // namespace sw_battle_gauge
