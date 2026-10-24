// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_inventory.h"
#include <algorithm>
#include <bit>
#include <limits>

namespace echoes {
namespace {
constexpr uint64_t OwnerGlobal = 0x34bc840, OwnerVtable = 0x33cdda0;
constexpr std::array<uint64_t, 12> OwnerMethods{{
    0x2127cbc, 0x2127d10, 0x2127d60, 0x2127da0, 0x2127ed0, 0x2128020,
    0x2128100, 0x21281f0, 0x2128290, 0x21283d0, 0x2128520, 0x2128610}};
constexpr uint64_t PayloadOffset = 0x185d;
constexpr size_t PayloadSize = 0x37;
bool Add(uint64_t a, uint64_t b, uint64_t& out) {
  if (a > std::numeric_limits<uint64_t>::max() - b) return false;
  out = a + b;
  return true;
}
bool Bytes(const ReadFn& read, uint64_t at, void* out, size_t n) {
  return at && n <= std::numeric_limits<uint64_t>::max() - at &&
         (n == 0 || read(at, out, n));
}
bool Field(const ReadFn& read, uint64_t at, uint64_t off, void* out, size_t n) {
  uint64_t address{};
  return Add(at, off, address) && Bytes(read, address, out, n);
}
uint16_t Le16(const uint8_t* p) {
  return uint16_t(uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8));
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(Le16(p)) | uint32_t(Le16(p + 2)) << 16;
}
uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32;
}
bool Pointer(const ReadFn& read, uint64_t base, uint64_t off, uint64_t& out) {
  uint64_t at{};
  std::array<uint8_t, 8> b{};
  if (!Add(base, off, at) || at % 8 || !Bytes(read, at, b.data(), b.size()))
    return false;
  out = Le64(b.data());
  return out && out % 8 == 0;
}
struct Sample {
  InventorySource source{};
  std::array<uint8_t, 32> owner{}, state{};
  std::array<uint8_t, PayloadSize> payload{};
  bool operator==(const Sample&) const = default;
};
bool TypeMatches(const ReadFn& read, uint64_t base, uint64_t observed) {
  uint64_t table{};
  std::array<uint8_t, OwnerMethods.size() * 8> bytes{};
  if (!Add(base, OwnerVtable, table) || observed != table ||
      !Bytes(read, table, bytes.data(), bytes.size())) return false;
  for (size_t i = 0; i < OwnerMethods.size(); ++i) {
    uint64_t method{};
    if (!Add(base, OwnerMethods[i], method) || Le64(bytes.data() + i * 8) != method)
      return false;
  }
  return true;
}
bool UncontendedReference(uint64_t counter) {
  // +2128610 normal branch adds1 to the low byte, tests availability mask,
  // then waits until bits in ~0xfcffffff00ffffff clear before acquiring.
  // This strict subset makes that normal branch's tests succeed without its
  // mutations, current-thread fast path, or a potentially blocking wait.
  // These are opaque native packed fields, not a gameplay/readiness enum.
  return (counter & UINT64_C(0xff)) == 0 &&
         (counter & UINT64_C(0x03000000ff000000)) == 0 &&
         (counter & UINT64_C(0x00000fff0000ff00)) != 0;
}
bool Collect(const ReadFn& read, uint64_t base, Sample& out, bool& type_supported,
             bool inventory_payload = true) {
  if (!Pointer(read, base, OwnerGlobal, out.source.owner) ||
      !Bytes(read, out.source.owner, out.owner.data(), out.owner.size())) return false;
  if (!TypeMatches(read, base, Le64(out.owner.data()))) return false;
  type_supported = true;
  out.source.state = Le64(out.owner.data() + 8);
  if (!out.source.state || out.source.state % 8 ||
      !UncontendedReference(Le64(out.owner.data() + 0x10)) ||
      !Bytes(read, out.source.state, out.state.data(), out.state.size()) ||
      (out.state[0x17] & 0x40) != 0)
    return false;
  return !inventory_payload || Field(read, out.source.state, PayloadOffset,
                                     out.payload.data(), out.payload.size());
}
bool StillCurrent(const ReadFn& read, uint64_t base, const Sample& sample) {
  uint64_t owner{};
  std::array<uint8_t, 32> owner_header{}, state_header{};
  // Check headers after the last payload read, then the root pointer last.
  return Bytes(read, sample.source.state, state_header.data(), state_header.size()) &&
         state_header == sample.state &&
         Bytes(read, sample.source.owner, owner_header.data(), owner_header.size()) &&
         owner_header == sample.owner &&
         Pointer(read, base, OwnerGlobal, owner) && owner == sample.source.owner;
}
bool Decode(const Sample& sample, InventorySnapshot& out) {
  const auto* p = sample.payload.data();
  const uint32_t flags = Le32(p + 0x1874 - PayloadOffset);
  out.acquired_outfits = Le16(p + 0x1878 - PayloadOffset);
  out.equipped_outfit = Le16(p + 0x187a - PayloadOffset);
  out.acquired_accessories = Le32(p + 0x187c - PayloadOffset);
  out.accessory_capacity = uint8_t(1 + std::popcount((flags >> 20) & 15));
  if ((out.acquired_outfits & ~KnownOutfitMask) != 0 ||
      (out.acquired_accessories & ~KnownAccessoryMask) != 0 ||
      !std::has_single_bit(out.equipped_outfit) ||
      (out.equipped_outfit & out.acquired_outfits) != out.equipped_outfit ||
      out.accessory_capacity > MaxAccessorySlots) return false;
  uint32_t seen{};
  for (size_t i = 0; i < out.accessory_capacity; ++i) {
    const uint32_t value = Le32(p + 0x1880 - PayloadOffset + i * 4);
    if (value && (!std::has_single_bit(value) ||
                  (value & out.acquired_accessories) != value || (seen & value)))
      return false;
    out.equipped_accessories[i] = value;
    seen |= value;
    if (value) ++out.equipped_accessory_count;
  }
  out.coupons = p[0];
  std::copy_n(p + 1, IngredientKinds, out.ingredients.begin());
  return out.coupons <= 99 &&
         std::all_of(out.ingredients.begin(), out.ingredients.end(),
                     [](uint8_t n) { return n <= 99; });
}
} // namespace

RupeeSnapshot ReadRupees(const ReadFn& read, uint64_t base, bool build_matches) {
  RupeeSnapshot out;
  if (!read || !build_matches || !base || base % 4) return out;
  for (const auto& guard : RupeeIdentityGuards) {
    uint64_t at{};
    std::array<uint8_t, 256> observed{};
    if (!Add(base, guard.offset, at) ||
        !Bytes(read, at, observed.data(), observed.size()) ||
        !MatchSupportedGuard(observed, guard.bytes, guard.offset)) return out;
  }
  out.compatible = true;
  Sample first, second;
  std::array<uint8_t, 4> first_value{}, second_value{};
  if (!Collect(read, base, first, out.native_predicate_supported, false) ||
      !Field(read, first.source.state, 0x1858, first_value.data(), first_value.size()) ||
      !Collect(read, base, second, out.native_predicate_supported, false) ||
      !Field(read, second.source.state, 0x1858, second_value.data(), second_value.size()) ||
      first != second || first_value != second_value ||
      !StillCurrent(read, base, first)) return out;
  const uint32_t rupees = Le32(first_value.data());
  if (rupees > MaxRupees) return out;
  out.rupees_valid = true;
  out.rupees = rupees;
  out.source = first.source;
  return out;
}

InventorySnapshot ReadInventory(const ReadFn& read, uint64_t base, bool build_matches) {
  InventorySnapshot out;
  if (!read || !build_matches || !base || base % 4) return out;
  for (const auto& guard : InventoryIdentityGuards) {
    uint64_t at{};
    std::array<uint8_t, 256> observed{};
    if (!Add(base, guard.offset, at) ||
        !Bytes(read, at, observed.data(), observed.size()) || !MatchSupportedGuard(observed, guard.bytes, guard.offset))
      return out;
  }
  out.compatible = true;
  Sample first, second;
  if (!Collect(read, base, first, out.native_predicate_supported) ||
      !Collect(read, base, second, out.native_predicate_supported) || first != second ||
      !StillCurrent(read, base, first)) return out;
  InventorySnapshot candidate;
  if (!Decode(first, candidate)) return out;
  candidate.compatible = candidate.native_predicate_supported = candidate.inventory_valid = true;
  candidate.source = first.source;
  return candidate;
}
} // namespace echoes
