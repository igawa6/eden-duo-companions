// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared guest-memory reading helpers for the Octopath Traveler 0 companion.
// ot0_reader.cpp, ot0_battle.cpp, ot0_battle_cache.h, ot0_resources.cpp,
// ot0_equipment.cpp and ot0_battle_selection.cpp each re-implemented the same
// {address bounds, TArray-header, vtable-matched object} checks over
// ot0::ReadMemory. This header is the one place those checks live now.
//
// Where a file's existing behaviour genuinely differs from the others (the
// address floor, an array's count/capacity caps, whether an empty array is
// accepted, the data pointer's required alignment) that difference is an
// explicit argument at the call site in the file that needs it -- nothing
// here silently unifies them. In particular, ot0_battle_selection.cpp reads
// require addresses >= 0x1000000 (not just non-null); it gets this by
// constructing its Reader with `minimum = 0x1000000`, which every check
// below (get/ptr/object/array/raw/ints) then enforces.
#pragma once
#include "ot0_reader.h"
#include <cstddef>
#include <cstdint>

namespace ot0::mem {

// Every copy picked this same 2^39 guest-address ceiling independently.
constexpr uint64_t Limit = uint64_t{1} << 39;

// A plausible guest pointer value: at least `minimum` (non-null by
// default), in range, and aligned. `minimum` is only ever raised by
// ot0_battle_selection.cpp (to 0x1000000), everyone else keeps the default.
inline bool Pointer(uint64_t p, size_t alignment = 8, uint64_t minimum = 1) {
  return p >= minimum && p < Limit && p % alignment == 0;
}

// A guest TArray<T>-like header: {data pointer, count, capacity}.
struct Array {
  uint64_t data{};
  int32_t count{}, capacity{};
  bool operator==(const Array &) const = default;
};
static_assert(sizeof(Array) == 16);

// The protagonist's travel-mode records (ids 900..909) share one roster
// entry (900). Any comparison between a party member id and a record/actor
// id must compare aliased ids, or it will miscount a protagonist as a
// stranger (or two different protagonist forms as two separate members).
inline uint32_t Alias(uint32_t id) { return id >= 900 && id <= 909 ? 900 : id; }

// Shared wrapper over ot0::ReadMemory. `minimum` is the address floor used
// by every check below; raising it (ot0_battle_selection.cpp constructs a
// Reader with minimum = 0x1000000) tightens every one of them at once,
// reproducing that file's historical stricter floor without duplicating the
// bounds logic.
struct Reader {
  const ReadMemory &read;
  uint64_t main{};
  int64_t delta{};
  uint64_t minimum{1};

  // Bounded raw read. 65536 is an arbitrary-but-generous cap carried over
  // from ot0_reader.cpp/ot0_battle.cpp; every actual read made through any
  // of these readers is far smaller than it.
  bool raw(uint64_t p, void *out, size_t n) const {
    return n && n <= 65536 && p >= minimum && p < Limit && n <= Limit - p &&
           read(p, out, n);
  }
  template <class T> bool get(uint64_t p, T &value) const {
    return raw(p, &value, sizeof value);
  }
  bool ptr(uint64_t p, uint64_t &value) const {
    return get(p, value) && Pointer(value, 8, minimum);
  }
  // A guest UObject-like header at `p`: a vtable matched against
  // main + vtable_offset (allowing for NCE relocation) plus flags that must
  // not be pending-kill/garbage/unreachable (0x40000030).
  bool object(uint64_t p, uint64_t vtable_offset) const {
    uint64_t actual{};
    uint32_t flags{};
    return Pointer(p, 8, minimum) && get(p, actual) &&
           MatchesVtable(actual, main + vtable_offset, delta) &&
           get(p + 8, flags) && !(flags & 0x40000030);
  }
  // A TArray-like header at `p`. `max_count` bounds a.count; `min_count`
  // is the smallest a.count accepted (0 by default, so an empty array is
  // fine; ot0_equipment.cpp passes 1 to keep requiring a non-empty array,
  // as it always has). `max_capacity` bounds a.capacity (default 4096,
  // ot0_reader.cpp/ot0_battle.cpp's historical cap; other callers pass
  // their own, smaller, historical cap). `data_alignment` bounds the data
  // pointer's alignment when a.count > 0 (default 4, matching
  // ot0_reader.cpp/ot0_battle.cpp's 4-byte-element arrays;
  // ot0_equipment.cpp/ot0_battle_selection.cpp pass 8).
  bool array(uint64_t p, Array &a, int max_count, int max_capacity = 4096,
             size_t data_alignment = 4, int min_count = 0) const {
    return get(p, a) && a.count >= min_count && a.count <= max_count &&
           a.capacity >= a.count && a.capacity <= max_capacity &&
           (!a.count || Pointer(a.data, data_alignment, minimum));
  }
  // i32 values [0, n) of a record TArray with at least n entries and at
  // most 64 (ot0_battle.cpp's original cap on this helper).
  bool ints(uint64_t p, int n, int32_t *out) const {
    Array a;
    return n > 0 && array(p, a, 64) && a.count >= n &&
           raw(a.data, out, size_t(n) * 4);
  }
};

} // namespace ot0::mem
