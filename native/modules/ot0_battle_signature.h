// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_battle_cache.h"
namespace ot0::battle_detail {
inline constexpr uint64_t AilmentHashOffset = 14695981039346656037ULL;
inline constexpr uint64_t AilmentHashPrime = 1099511628211ULL;
inline uint64_t HashByte(uint64_t hash, uint8_t byte) {
  return (hash ^ byte) * AilmentHashPrime;
}
struct ActorSignature {
  uint64_t native_actor{}, raw_ailment_fingerprint{};
  uint32_t native_serial{};
  uint8_t weapon_selector{};
  bool valid{};
  bool operator==(const ActorSignature &) const = default;
};
struct SerialSample {
  int32_t index{}, count{};
  uint64_t chunks{}, chunk{};
  std::array<uint8_t, 20> item{};
  bool operator==(const SerialSample &) const = default;
};
// Unallocated serial zero is valid only for requesting native weak-serial
// initialization. Read/display signatures retain the strict positive default.
inline bool Serial(const Reader &r, uint64_t actor, SerialSample &out,
                   bool allow_unallocated = false) {
  const uint64_t gu = r.main + 0x6ce97e8;
  if (!r.get(actor + 0xc, out.index) || out.index < 0 ||
      !r.get(gu + 0x24, out.count) || out.count < 1 || out.count > 1000000 ||
      out.index >= out.count || !r.ptr(gu + 0x10, out.chunks) ||
      !r.ptr(out.chunks + 8 * uint64_t(uint32_t(out.index) >> 16), out.chunk) ||
      !r.get(out.chunk + 20 * uint64_t(out.index & 65535), out.item))
    return false;
  uint64_t object{};
  uint32_t flags{};
  int32_t serial{};
  std::memcpy(&object, out.item.data(), 8);
  std::memcpy(&flags, out.item.data() + 8, 4);
  std::memcpy(&serial, out.item.data() + 16, 4);
  return object == actor &&
         (serial > 0 || (allow_unallocated && serial == 0)) &&
         !(flags & 0x30200000);
}
inline ActorSignature ReadActorSignature(const Reader &r, uint64_t actor) {
  ActorSignature out{};
  out.native_actor = actor;
  if (!r.object(actor, 0x63f39b8))
    return out;
  uint8_t again_selector{};
  if (!r.get(actor + 0x2a8, out.weapon_selector) || out.weapon_selector > 1)
    return out;
  Array ailments{}, checked{};
  if (!r.get(actor + 0x800, ailments) || ailments.count < 0 ||
      ailments.count > 128 || ailments.capacity < ailments.count ||
      ailments.capacity > 256 || (ailments.count && !Pointer(ailments.data)))
    return out;
  std::vector<std::array<uint8_t, 0xe8>> rows(size_t(ailments.count));
  uint64_t hash = AilmentHashOffset;
  for (unsigned i = 0; i < 4; ++i)
    hash = HashByte(hash, uint8_t(uint32_t(ailments.count) >> (8 * i)));
  for (int i = 0; i < ailments.count; ++i) {
    if (!r.get(ailments.data + uint64_t(i) * 0xe8, rows[size_t(i)]))
      return out;
    for (uint8_t byte : rows[size_t(i)])
      hash = HashByte(hash, byte);
  }
  if (!r.get(actor + 0x800, checked) || checked != ailments)
    return out;
  for (int i = 0; i < ailments.count; ++i) {
    std::array<uint8_t, 0xe8> row{};
    if (!r.get(ailments.data + uint64_t(i) * 0xe8, row) ||
        row != rows[size_t(i)])
      return out;
  }
  if (!r.get(actor + 0x2a8, again_selector) ||
      again_selector != out.weapon_selector)
    return out;
  out.raw_ailment_fingerprint = hash;
  SerialSample a{}, b{};
  if (!Serial(r, actor, a) || !Serial(r, actor, b) || a != b ||
      !r.object(actor, 0x63f39b8))
    return out;
  std::memcpy(&out.native_serial, a.item.data() + 16, 4);
  out.valid = true;
  return out;
}
} // namespace ot0::battle_detail
