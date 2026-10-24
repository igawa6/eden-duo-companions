// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_prologue.h"
#include "echoes_code_guard.h"
#include "echoes_inventory.h"
#include <algorithm>
#include <array>
#include <limits>
#include <set>
namespace echoes {
namespace {
uint16_t U16(std::span<const uint8_t> b, size_t p) {
  return uint16_t(b[p]) | uint16_t(b[p + 1]) << 8;
}
uint32_t U32(std::span<const uint8_t> b, size_t p) {
  return U16(b, p) | uint32_t(U16(b, p + 2)) << 16;
}
uint64_t U64(std::span<const uint8_t> b, size_t p) {
  return U32(b, p) | uint64_t(U32(b, p + 4)) << 32;
}
bool Fits(std::span<const uint8_t> b, uint64_t p, uint64_t n) {
  return p <= b.size() && n <= b.size() - p;
}
bool Ref(std::span<const uint8_t> b, size_t p, uint64_t pool,
         std::string &out) {
  const auto at = U64(b, p), n = U64(b, p + 8);
  if (!n || n > 256 || at < pool || !Fits(b, at, n + 1) || b[size_t(at + n)])
    return false;
  out.assign(reinterpret_cast<const char *>(b.data() + at), size_t(n));
  return std::all_of(out.begin(), out.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '/';
  });
}
} // namespace
bool LoadPrologueItems(const ReadAssetFn &read, std::string_view locale,
                       std::vector<PrologueItem> &out) {
  out.clear();
  if (!read)
    return false;
  constexpr std::string_view path =
      "region_common/datasheets/ui/UiPrologueMenu.gsheet";
  const auto n = read(path, 0, nullptr, 0);
  if (n < 48 || n > 65536)
    return false;
  std::vector<uint8_t> b(n);
  if (read(path, 0, b.data(), n) != n || read(path, 0, nullptr, 0) != n)
    return false;
  constexpr std::array<uint8_t, 8> magic{'g', 's', 'h', 't', 2, 0, 0, 0};
  if (!std::equal(magic.begin(), magic.end(), b.begin()) || U32(b, 24) != 3 ||
      U32(b, 28) != 3 || U32(b, 40) != 5 || U32(b, 44) != 48)
    return false;
  const auto rows = U64(b, 32), pool = U64(b, 16);
  if (rows < 192 || !Fits(b, rows, 5 * 48) || pool < rows + 5 * 48 ||
      !Fits(b, pool, 1))
    return false;
  constexpr std::array<std::string_view, 3> fields{
      "mTextureName", "mNameTextLabel", "mDescriptionTextLabel"};
  for (size_t i = 0; i < 3; ++i) {
    const size_t a = 48 + i * 48;
    const auto p = U64(b, a);
    if (p < pool || !Fits(b, p, fields[i].size() + 1) ||
        b[size_t(p) + fields[i].size()] ||
        !std::equal(fields[i].begin(), fields[i].end(),
                    b.begin() + static_cast<std::ptrdiff_t>(p)) ||
        U16(b, a + 16) != 4 || U16(b, a + 20) != i * 16)
      return false;
  }
  std::vector<PrologueItem> parsed;
  std::vector<NativeLocation> refs;
  std::set<std::string> textures;
  for (size_t i = 0; i < 5; ++i) {
    const size_t a = size_t(rows) + i * 48;
    PrologueItem item;
    if (!Ref(b, a, pool, item.texture) ||
        !Ref(b, a + 16, pool, item.name_label) ||
        !Ref(b, a + 32, pool, item.description_label) ||
        !textures.insert(item.texture).second ||
        item.texture.find_first_of("/:") != std::string::npos)
      return false;
    NativeLocation ref;
    ref.name_label = item.name_label;
    ref.additional_label = item.description_label;
    refs.push_back(std::move(ref));
    parsed.push_back(std::move(item));
  }
  if (!LocalizeNativeLocations(read, locale, refs))
    return false;
  for (size_t i = 0; i < 5; ++i) {
    parsed[i].name = std::move(refs[i].name);
    parsed[i].description = std::move(refs[i].additional);
  }
  out = std::move(parsed);
  return true;
}
PrologueActorSnapshot ReadPrologueActor(const ReadFn &read, uint64_t base,
                                        bool build) {
  PrologueActorSnapshot out;
  if (!build || !read || !base || base % 4096 ||
      base > std::numeric_limits<uint64_t>::max() - 0x34bc848)
    return out;
  const auto bytes = [&](uint64_t p, void *dst, size_t n) {
    return p && n <= std::numeric_limits<uint64_t>::max() - p &&
           read(p, dst, n);
  };
  const auto guard = [&](const IdentityGuard &g) {
    std::array<uint8_t, 256> observed{};
    return bytes(base + g.offset, observed.data(), observed.size()) &&
           MatchSupportedGuard(observed, g.bytes, g.offset);
  };
  if (!guard(InventoryIdentityGuards[0]))
    return out;
  for (const auto &g : PrologueActorGuards)
    if (!guard(g))
      return out;
  constexpr std::array<uint64_t, 12> methods{
      {0x2127cbc, 0x2127d10, 0x2127d60, 0x2127da0, 0x2127ed0, 0x2128020,
       0x2128100, 0x21281f0, 0x2128290, 0x21283d0, 0x2128520, 0x2128610}};
  std::array<uint8_t, 96> table{};
  if (!bytes(base + 0x33cdda0, table.data(), table.size()))
    return out;
  for (size_t i = 0; i < methods.size(); ++i)
    if (U64(table, i * 8) != base + methods[i])
      return out;
  out.compatible = true;
  struct Sample {
    std::array<uint8_t, 8> root{};
    std::array<uint8_t, 32> owner{}, state{};
    std::array<uint8_t, 2> kind{};
    bool operator==(const Sample &) const = default;
  };
  const auto collect = [&](Sample &v) {
    if (!bytes(base + 0x34bc840, v.root.data(), v.root.size()))
      return false;
    const uint64_t p = U64(v.root, 0);
    if (!p || p % 8 || !bytes(p, v.owner.data(), v.owner.size()) ||
        U64(v.owner, 0) != base + 0x33cdda0)
      return false;
    const uint64_t q = U64(v.owner, 8), c = U64(v.owner, 16);
    // Strict sufficient normal acquisition path; no atomic writes or calls.
    if (!q || q % 8 || q > std::numeric_limits<uint64_t>::max() - 0xb2 ||
        (c & UINT64_C(0xff)) || (c & UINT64_C(0x03000000ff000000)) ||
        !(c & UINT64_C(0x00000fff0000ff00)) ||
        !bytes(q, v.state.data(), v.state.size()) || (v.state[0x17] & 0x40) ||
        !bytes(q + 0xb0, v.kind.data(), v.kind.size()))
      return false;
    return true;
  };
  Sample a, b;
  if (!collect(a) || !collect(b) || a != b)
    return out;
  // Revalidate the headers and root after the final model-kind read.
  std::array<uint8_t, 32> owner{}, state{};
  std::array<uint8_t, 8> root{};
  const uint64_t p = U64(a.root, 0), q = U64(a.owner, 8);
  if (!bytes(q, state.data(), state.size()) || state != a.state ||
      !bytes(p, owner.data(), owner.size()) || owner != a.owner ||
      !bytes(base + 0x34bc840, root.data(), root.size()) || root != a.root)
    return out;
  out.owner = p;
  out.state = q;
  out.kind = U16(a.kind, 0);
  out.known = true;
  out.link = out.kind == 0x394;
  return out;
}
SwordfighterSnapshot ReadSwordfighter(const ReadFn& read,uint64_t base,bool build) {
  SwordfighterSnapshot out;
  const auto a=ReadPrologueActor(read,base,build);
  out.compatible=a.compatible;
  if(!a.known || a.state>std::numeric_limits<uint64_t>::max()-0x184c)return out;
  std::array<uint8_t,4> first{},last{};
  if(!read(a.state+0x1848,first.data(),first.size()))return out;
  const auto b=ReadPrologueActor(read,base,build);
  if(!b.known || a.owner!=b.owner || a.state!=b.state || a.kind!=b.kind ||
     !read(b.state+0x1848,last.data(),last.size()) || first!=last)return out;
  const uint32_t mode=uint32_t(first[0])|uint32_t(first[1])<<8|uint32_t(first[2])<<16|uint32_t(first[3])<<24;
  if(mode>1)return out;
  out.known=true;out.mode=mode;out.active=mode==1 && !a.link;out.actor=b;
  return out;
}
PrologueScopeSnapshot
ReadPrologueScope(const ReadFn &read, uint64_t base, bool build,
                  const MapSnapshot &map,
                  std::span<const NativeLocation> metadata) {
  PrologueScopeSnapshot out;
  out.location = ReadCurrentLocation(read, base, build, map, metadata);
  out.compatible = out.location.compatible;
  out.known = out.location.known;
  if (out.known && out.location.key == "OP_Dungeon") {
    out.actor = ReadPrologueActor(read, base, build);
    out.compatible = out.compatible && out.actor.compatible;
    out.known = out.actor.known;
    out.opening = out.actor.known && out.actor.link;
  }
  return out;
}
} // namespace echoes
