// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_memory.h"
#include "ot0_reader.h"
#include <array>
#include <cstring>
#include <vector>
namespace ot0::battle_detail {
// Limit/Array/Pointer/Reader used to be defined here directly; they now
// live in ot0::mem (shared with ot0_reader.cpp, ot0_battle.cpp,
// ot0_resources.cpp, ot0_equipment.cpp, ot0_battle_selection.cpp) and are
// pulled in by name so every existing unqualified use in this namespace
// (here, in ot0_battle_actors.h and in ot0_battle_signature.h) keeps
// resolving the same way. Reader keeps its historical default address
// floor (minimum = 1, i.e. non-null); nothing in this namespace needs the
// stricter floor ot0_battle_selection.cpp uses.
using ot0::mem::Array;
using ot0::mem::Limit;
using ot0::mem::Pointer;
using Reader = ot0::mem::Reader;
// Native382da50 TMap<byte,TArray<variant>>. The normal max-HP getter
// 382d3c8 requests key1 and flags1/1/0, then max(cache,actor540,actor548,1).
// Hash-chain traversal matches native382daf4..382dbe8; variants are12 bytes.
struct Cached {
  std::array<uint8_t, 0x50> map{};
  std::vector<std::array<uint8_t, 32>> chain;
  std::vector<std::array<uint8_t, 12>> variants;
  int32_t bucket{}, floor{}, minimum{}, value{};
  bool operator==(const Cached &) const = default;
};
inline bool Cache(const Reader &r, uint64_t actor, uint8_t key, Cached &out,
                  bool hp_bounds = false) {
  if (!r.get(actor + 0x5b8, out.map) ||
      (hp_bounds && (!r.get(actor + 0x540, out.floor) ||
                     !r.get(actor + 0x548, out.minimum))))
    return false;
  const auto field = [&]<class T>(size_t off, T &v) {
    std::memcpy(&v, out.map.data() + off, sizeof(v));
  };
  uint64_t data{}, hash{};
  int32_t count{}, capacity{}, free{}, hash_count{};
  field(0, data);
  field(8, count);
  field(12, capacity);
  field(0x34, free);
  field(0x40, hash);
  field(0x48, hash_count);
  if (!Pointer(data) || count < 1 || count > 256 || capacity < count ||
      capacity > 256 || free < 0 || free >= count || hash_count < 1 ||
      hash_count > 256 || (hash_count & (hash_count - 1)))
    return false;
  uint64_t buckets = hash ? hash : actor + 0x5f0;
  if (hash && !Pointer(hash))
    return false;
  if (!r.get(buckets + uint64_t(key & (hash_count - 1)) * 4, out.bucket))
    return false;
  int32_t index = out.bucket;
  std::array<bool, 256> seen{};
  for (int n = 0; n < count; ++n) {
    if (index < 0 || index >= count || seen[index])
      return false;
    seen[index] = true;
    std::array<uint8_t, 32> entry{};
    if (!r.get(data + uint64_t(index) * 32, entry))
      return false;
    out.chain.push_back(entry);
    if (entry[0] == key) {
      Array variants{};
      std::memcpy(&variants, entry.data() + 8, sizeof(variants));
      if (!Pointer(variants.data) || variants.count < 1 ||
          variants.count > 32 || variants.capacity < variants.count ||
          variants.capacity > 32)
        return false;
      bool found = false;
      for (int i = 0; i < variants.count; ++i) {
        std::array<uint8_t, 12> v{};
        if (!r.get(variants.data + uint64_t(i) * 12, v))
          return false;
        out.variants.push_back(v);
        if (v[0] == 1 && v[1] == 1 && v[2] == 0) {
          if (found)
            return false;
          std::memcpy(&out.value, v.data() + 8, 4);
          found = true;
        }
      }
      return found;
    }
    std::memcpy(&index, entry.data() + 0x18, 4);
  }
  return false;
}
} // namespace ot0::battle_detail
