// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Legend of Zelda: Tears of the Kingdom 1.4.3 (v720896, main build id
// 277178b7dba1b6d4949a84778d4abc58): reader for the game's GameData manager and the player actor.
//
// Parts of this layout are ported from TotK Native's games/totk/runtime/companion_snapshot.c (MIT):
//   Copyright (c) 2026 MacNativeGames contributors, https://github.com/AurelioB/totk-android
//   (full license text in NOTICE-TotKNative.txt of the package)
// namely: the per-type table offsets and strides, the Vector3 value offset (+0x24), MurmurHash3 of the
// dotted flag names, string-array elements, the pouch partition selector, and the scene -> resident
// actor walk to the "Player" actor (actor +0x218 name, +0x2B4 position, +0x2C0 rotation).
// The singleton slot, the hash-index algorithm and the Int/Float/Bool value offsets were found
// independently (README.md) and agree with it; the hearts hashes read from Player.pack equal
// MurmurHash3("PlayerStatus.Life" ...), which cross-checks both analyses.
//
// All offsets are relative to the main NSO image. Every read is bounds checked.
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace totk {

constexpr uint64_t kMgrSlot = 0x3AD66B0;     // gmd::GameDataMgr *
constexpr uint64_t kGotSlot = 0x395DFF0;     // GOT entry holding that address (relocated: main + kMgrSlot)
constexpr uint64_t kPouchSlot = 0x3A81678;   // pouch manager *; +0x188 u32 current partition (0/1)
constexpr uint64_t kSceneSlot = 0x3ADD238;   // scene module *; +0x1E8 scene
constexpr uint32_t kActorName = 0x218, kActorPos = 0x2B4, kActorRot = 0x2C0;

// MurmurHash3 x86_32, seed 0: the game's GameData flag and enum value hash.
inline uint32_t hash(const char* s) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
  const size_t n = std::strlen(s);
  uint32_t h = 0;
  const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    uint32_t k = uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16 | uint32_t(p[i + 3]) << 24;
    k *= c1; k = k << 15 | k >> 17; k *= c2;
    h ^= k; h = h << 13 | h >> 19; h = h * 5 + 0xe6546b64u;
  }
  uint32_t k = 0;
  switch (n & 3) {
    case 3: k ^= uint32_t(p[i + 2]) << 16; [[fallthrough]];
    case 2: k ^= uint32_t(p[i + 1]) << 8; [[fallthrough]];
    case 1: k ^= p[i]; k *= c1; k = k << 15 | k >> 17; k *= c2; h ^= k;
  }
  h ^= uint32_t(n); h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
  return h;
}

enum Type { T_BOOL, T_BOOLARR, T_INT, T_INTARR, T_FLOAT, T_ENUM, T_ENUMARR, T_VEC2ARR, T_VEC3, T_VEC3ARR, T_STRARR, T_COUNT };
struct TypeInfo { uint16_t base, stride, value, count, data; };
// table block = mgr + base: {vtable; i32 count; entries; hash index; u32 used; u32 capacity}
constexpr TypeInfo kTypes[T_COUNT] = {
    {0x008, 0x18, 0x16, 0, 0},   {0x068, 0x40, 0, 0x20, 0x28}, {0x0c8, 0x28, 0x1c, 0, 0},
    {0x128, 0x40, 0, 0x20, 0x28}, {0x188, 0x28, 0x1c, 0, 0},   {0x248, 0x38, 0x1c, 0, 0},
    {0x2a8, 0x50, 0, 0x20, 0x28}, {0x368, 0x48, 0, 0x28, 0x30}, {0x3c8, 0x40, 0x24, 0, 0},
    {0x428, 0x48, 0, 0x28, 0x30}, {0x668, 0x90, 0, 0x70, 0x78}};

constexpr uint32_t kMaxTable = 1u << 20, kMaxArray = 4096, kStrElem = 0x50, kNameMax = 64;
constexpr uint32_t kIntCountMin = 5000, kIntCountMax = 8000;  // GameDataList has 5675 Int flags

struct Arr { uint64_t data{}; uint32_t count{}; };

struct Reader {
  const EdenDsmodHostApi* h{};
  uint64_t mgr{};

  template <class T> bool rd(uint64_t a, T& out) const { return rdn(a, &out, sizeof(T)); }
  bool rdn(uint64_t a, void* out, size_t n) const {
    return h && h->read_memory && a >= 0x1000 && a <= UINT64_MAX - n && h->read_memory(h->userdata, a, out, n) != 0;
  }
  uint64_t rd64(uint64_t a) const { uint64_t v = 0; return rd(a, v) ? v : 0; }

  // Resolves the manager from main+kMgrSlot. False when null or unreadable (boot, title).
  bool bind() {
    mgr = 0;
    if (!h || !h->main_base) return false;
    const uint64_t p = rd64(h->main_base + kMgrSlot);
    uint32_t probe = 0;
    if (p < 0x10000 || (p & 7) || !rd(p + kTypes[T_INT].base + 0x24, probe)) return false;
    mgr = p;
    return true;
  }
  bool count(Type t, uint32_t& n) const { return mgr && rd(mgr + kTypes[t].base + 8, n); }

  // The entry for a flag hash in a type's table, or 0.
  uint64_t lookup(Type t, uint32_t hs) const {
    if (!mgr || !hs) return 0;
    uint8_t blk[0x20];
    if (!rdn(mgr + kTypes[t].base + 8, blk, sizeof blk)) return 0;
    int32_t cnt; uint64_t entries, index; uint32_t cap;
    std::memcpy(&cnt, blk, 4); std::memcpy(&entries, blk + 8, 8); std::memcpy(&index, blk + 0x10, 8);
    std::memcpy(&cap, blk + 0x1c, 4);
    if (cnt <= 0 || cnt > int32_t(kMaxTable) || !cap || cap > 2 * kMaxTable || !entries || !index) return 0;
    uint32_t slot = hs % cap;
    for (uint32_t i = 0; i < cap; ++i) {
      uint32_t cell[2];
      if (!rd(index + 8ull * slot, cell) || !cell[0]) return 0;
      if (cell[0] == hs) return cell[1] < uint32_t(cnt) ? entries + uint64_t(cell[1]) * kTypes[t].stride : 0;
      if (++slot == cap) slot = 0;
    }
    return 0;
  }
  bool value(Type t, uint32_t hs, void* out, size_t n) const {
    const uint64_t e = lookup(t, hs);
    return e && rdn(e + kTypes[t].value, out, n);
  }
  bool get_int(const char* name, int32_t& v) const { return value(T_INT, hash(name), &v, 4); }
  bool get_int(uint32_t hs, int32_t& v) const { return value(T_INT, hs, &v, 4); }
  bool get_float(const char* name, float& v) const { return value(T_FLOAT, hash(name), &v, 4) && std::isfinite(v); }
  bool get_bool(const char* name, bool& v) const {
    uint8_t b;
    if (!value(T_BOOL, hash(name), &b, 1)) return false;
    v = b & 1;
    return true;
  }
  bool get_enum_hash(const char* name, uint32_t& v) const { return value(T_ENUM, hash(name), &v, 4); }
  bool get_vec3(const char* name, float (&v)[3]) const {
    return value(T_VEC3, hash(name), v, 12) && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
  }
  bool arr(Type t, const char* name, Arr& a) const {
    const uint64_t e = lookup(t, hash(name));
    if (!e || !rd(e + kTypes[t].count, a.count) || !rd(e + kTypes[t].data, a.data)) return false;
    return a.data && a.count <= kMaxArray;
  }
  bool words(const Arr& a, uint32_t first, uint32_t n, void* out) const {
    return first <= a.count && n <= a.count - first && rdn(a.data + 4ull * first, out, 4ull * n);
  }
  // String array elements {char* data; i32 capacity; char inline[]...}, 0x50 bytes. Non-identifiers come out empty.
  bool names(const Arr& a, uint32_t first, uint32_t n, char (*out)[kNameMax]) const {
    if (first > a.count || n > a.count - first) return false;
    std::string raw(size_t(n) * kStrElem, '\0');
    const bool ok = n == 0 || rdn(a.data + uint64_t(first) * kStrElem, raw.data(), raw.size());
    for (uint32_t i = 0; i < n; ++i) {
      out[i][0] = 0;
      if (!ok) continue;
      const uint8_t* e = reinterpret_cast<const uint8_t*>(raw.data()) + size_t(i) * kStrElem;
      uint64_t data; int32_t cap;
      std::memcpy(&data, e, 8); std::memcpy(&cap, e + 8, 4);
      const size_t len = cap > 0 && cap < int32_t(kNameMax) ? size_t(cap) : kNameMax;
      char s[kNameMax + 1] = {0};
      const uint64_t self = a.data + uint64_t(first + i) * kStrElem;
      if (data == self + 0xc) std::memcpy(s, e + 0xc, len);
      else if (!data || !rdn(data, s, len)) continue;
      s[len < kNameMax ? len : kNameMax - 1] = 0;
      if (!std::memchr(s, 0, len) || !s[0]) continue;
      bool id = true;
      for (const char* c = s; *c; ++c) id = id && (std::isalnum(uint8_t(*c)) || *c == '_');
      if (id) std::memcpy(out[i], s, std::strlen(s) + 1);
    }
    return ok;
  }

  // The scene's resident actor named "Player" (not the title screen's one at the origin). Returns the actor or 0.
  uint64_t player_actor() const {
    const uint64_t module = rd64(h->main_base + kSceneSlot), scene = module ? rd64(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !rd(scene + 0x50, count) || count < 14 || count > 256) return 0;
    const uint64_t comps = rd64(scene + 0x58), resident = comps ? rd64(comps + 13 * 8) : 0;
    if (!resident || !rd(resident + 0x20, count) || count < 1 || count > 64) return 0;
    const uint64_t links = rd64(resident + 0x28);
    if (!links) return 0;
    for (int i = 0; i < count; ++i) {
      const uint64_t data = rd64(links + uint64_t(i) * 0x70 + 8), actor = data ? rd64(data + 0x40) : 0;
      const uint64_t name = actor ? rd64(actor + kActorName) : 0;
      char text[7]; float p[3];
      if (name && rdn(name, text, sizeof text) && !std::memcmp(text, "Player", 7) && rd(actor + kActorPos, p) &&
          std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::fabs(p[0]) <= 8000 &&
          std::fabs(p[1]) <= 8000 && std::fabs(p[2]) <= 8000 && (p[0] != 0 || p[1] != 0 || p[2] != 0))
        return actor;
    }
    return 0;
  }
};

}  // namespace totk
