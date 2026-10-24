// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic save switches and native-table corruption, without guest execution.
#include "echoes_live.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_map>

namespace {
constexpr uint64_t Base = 0x80488000, Table = Base + 0x28daad4;
constexpr uint64_t Access = 0x2107000000, Save = 0x2107100000, Collection = 0x2107200000;
constexpr uint64_t Head = Base + 0x34bf070, Node = 0x2107300000, Owner = 0x2107400020;
constexpr uint64_t List = Owner + 0x150, Records = 0x2107500000;
constexpr uint64_t Player = 0x2107600000, State = 0x2107700000;
constexpr uint64_t FavoriteData = 0x2107800000;
constexpr size_t TableSize = 1225 * 8;
size_t cases{};
void Check(bool ok, const char* why) {
  if (!ok) { std::cerr << "FAIL after " << cases << " cases: " << why << '\n'; std::exit(1); }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t, size_t)> before;
  size_t reads{};
  void Put(uint64_t at, uint64_t value, size_t n) {
    for (size_t i = 0; i < n; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
  }
  uint32_t Key(size_t id) const {
    uint32_t key{};
    for (size_t i = 0; i < 4; ++i) key |= uint32_t(bytes.at(Table + id * 8 + i)) << (8 * i);
    return key;
  }
  Memory() {
    for (size_t i = 0; i < 1225; ++i) {
      Put(Table + i * 8, 0x10000000 + i, 4); Put(Table + i * 8 + 4, i, 4);
    }
    // The first mapping rows also participate in an immutable identity guard.
    for (const auto& guard : echoes::IdentityGuards)
      for (size_t i = 0; i < guard.bytes.size(); ++i) bytes[Base + guard.offset + i] = guard.bytes[i];
    Put(Base + 0x34bc848, Access, 8); Put(Access + 0x10, Save, 8);
    Put(Save + 0x16cf8, Collection, 8);
    for (size_t i = 0; i < 32; ++i) bytes[Collection + i] = 0;
    Put(Collection + 0x10, 3, 4); Put(Collection + 0x18, 0x12345678, 4);
    // Ownership order deliberately differs from native picker order.
    Put(Collection + 32, Key(40), 4); Put(Collection + 36, Key(875), 4);
    Put(Collection + 40, Key(874), 4);
    Put(Base + 0x34bc840, Player, 8); Put(Player + 8, State, 8);
    Put(State + 0x1970, 40, 2);
    Put(Head, Node, 8); Put(Head + 8, Node, 8); Put(Head + 16, 1, 8);
    Put(Node, Head, 8); Put(Node + 8, Head, 8); Put(Node + 16, Owner, 8);
    Put(Owner - 32, Base + 0x34266d0, 8); Put(Owner, Base + 0x34119c8, 8);
    Put(Owner + 0x88, 2, 3); Put(Owner + 0x7f50, 0x1000, 8);
    Put(Owner + 0x7faa, 1, 1); Put(List, Base + 0x342c260, 8);
    Put(List + 0x2c48, Records, 8); Put(List + 0x2c50, Records + 112, 8);
    Put(List + 0x2c58, Records + 112, 8); Put(List + 0x2c78, 1, 4);
    Put(List + 0x2c7c, 1, 4);
    for (size_t i = 0; i < 112; ++i) bytes[Records + i] = 0;
    Put(Records, 40, 2); Put(Records + 56, 874, 2);
  }
  bool Read(uint64_t at, void* out, size_t n) {
    ++reads;
    const size_t visit = ++visits[at];
    if (before) before(at, n, visit);
    if (!at || n > std::numeric_limits<uint64_t>::max() - at) return false;
    auto* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
      const auto found = bytes.find(at + i);
      if (found == bytes.end()) return false;
      p[i] = found->second;
    }
    return true;
  }
  void Favorites() {
    for (const auto& g : echoes::FavoriteIdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i)
        bytes[Base + g.offset + i] = g.bytes[i];
    Put(Save + 0x16d60, FavoriteData, 8);
    for (size_t i = 0; i < 16; ++i) bytes[FavoriteData + i] = 0;
    Put(FavoriteData + 0x3058, 1, 4);
    Put(FavoriteData + 0x2058, Key(874), 4);
    Put(Records + 56 + 0x30, 1, 1);
  }
  echoes::LiveSnapshot Sample(bool build = true) {
    return echoes::ReadLive([&](auto a, auto p, auto n) { return Read(a, p, n); }, Base, build);
  }
};
void NoCollection(const echoes::LiveSnapshot& s) {
  Check(s.compatible && !s.collection_valid && s.learned_native_ids.empty() && s.learned_crc_keys.empty() &&
        s.collection_source == echoes::EpochSource{} && !s.selector.compatible &&
        !s.selector.held && s.selector.ordered_native_ids.empty(),
        "failed join cannot publish ownership or a detached native selector");
}
} // namespace

int main() {
  {
    Memory m;
    m.Favorites();
    const auto s = m.Sample();
    Check(s.collection_valid && s.favorite_valid &&
              s.favorite_crc_keys == std::vector<uint32_t>{m.Key(874)} &&
              s.selector.favorite_valid &&
              s.selector.favorite_native_ids == std::vector<uint16_t>{874},
          "joined persisted CRC and current held favorite metadata retain their distinct identities");
    ++cases;
  }
  {
    Memory m;
    m.Favorites();
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 1)
        m.Put(FavoriteData + 0x2058, m.Key(875), 4);
    };
    const auto s = m.Sample();
    Check(s.collection_valid && s.selector.held && !s.favorite_valid &&
              s.favorite_crc_keys.empty(),
          "favorite change between joined snapshots withdraws only persisted optional metadata");
    ++cases;
  }
  {
    Memory m;
    const auto s = m.Sample();
    Check(s.compatible && s.collection_valid && s.collection_source.access_object == Access &&
          s.collection_source.save_data == Save && s.collection_source.list == Collection &&
          s.learned_native_ids == std::vector<uint16_t>{40, 875, 874} &&
          s.learned_crc_keys == std::vector<uint32_t>{m.Key(40), m.Key(875), m.Key(874)} &&
          s.selector.held && s.selector.ordered_native_ids == std::vector<uint16_t>{40, 874} &&
          s.selector.highlighted_native_id == 874 && s.selector.equipped_native_id == 40,
          "stable epoch joins learned IDs to distinct native picker order and highlight");
    m.Put(Base + 0x34bc848, 0, 8);
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m; const auto s = m.Sample(false);
    Check(!s.compatible && !s.collection_valid && m.reads == 0,
          "unsupported build does not read mappings or ownership"); ++cases;
  }
  {
    Memory m; m.Put(Records + 56, 876, 2);
    const auto s = m.Sample();
    Check(s.collection_valid && s.learned_native_ids.size() == 3 &&
          !s.selector.held && !s.selector.selector_valid && s.selector.ordered_native_ids.empty(),
          "an unlearned displayed ID invalidates the whole held list, not its ownership set"); ++cases;
  }
  {
    Memory m; m.Put(Collection + 0x10, 0, 4);
    const auto s = m.Sample();
    Check(s.collection_valid && s.learned_native_ids.empty() && !s.selector.held &&
          s.selector.ordered_native_ids.empty(),
          "empty title ownership cannot authenticate a stale held picker"); ++cases;
  }
  {
    Memory m; m.Put(Owner + 0x88, 0, 1);
    const auto s = m.Sample();
    Check(s.collection_valid && s.selector.selector_valid && !s.selector.held &&
          s.selector.ordered_native_ids.empty() && s.selector.equipped_valid,
          "known closed native selector joins without exposing its cached order"); ++cases;
  }
  {
    Memory m; m.Put(Owner + 0x7faa, 0, 1);
    const auto s = m.Sample();
    Check(s.collection_valid && !s.selector.selector_valid && !s.selector.held &&
          s.selector.ordered_native_ids.empty(), "unverified alternate mode stays unavailable"); ++cases;
  }
  {
    Memory m; m.Put(Collection + 36, 0xdeadbeef, 4);
    NoCollection(m.Sample()); ++cases;
  }
  for (bool padding : {false, true}) {
    Memory m; m.Put(Table + 400 * 8 + (padding ? 6 : 4), padding ? 1 : 401, 2);
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(Table + 400 * 8, m.Key(401), 4);
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m; m.bytes.erase(Table + 400 * 8);
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m; size_t whole_reads{};
    m.before = [&](auto at, auto n, auto) {
      if (at == Table && n == TableSize && ++whole_reads == 2)
        m.Put(Table + 400 * 8, 0x778899aa, 4);
    };
    NoCollection(m.Sample());
    Check(whole_reads == 2, "second full table read detects otherwise valid mapping change"); ++cases;
  }
  {
    Memory m; size_t whole_reads{};
    m.before = [&](auto at, auto n, auto) {
      if (at == Table && n == TableSize && ++whole_reads == 2)
        m.bytes.erase(Table + 900 * 8);
    };
    NoCollection(m.Sample()); ++cases;
  }
  for (bool revision : {false, true}) {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 1) {
        if (revision) m.Put(Collection + 0x18, 0xabcdef01, 4);
        else m.Put(Collection + 36, m.Key(876), 4);
      }
    };
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m;
    const uint64_t next_access = Access + 0x100;
    m.Put(next_access + 0x10, Save, 8); // Same old list remains readable.
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) m.Put(Base + 0x34bc848, next_access, 8);
    };
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 1) m.Put(Node + 8, Node, 8);
    };
    const auto s = m.Sample();
    Check(s.collection_valid && s.learned_native_ids.size() == 3 &&
          !s.selector.selector_valid && !s.selector.held,
          "bad selector registry cannot erase valid independently joined collection"); ++cases;
  }
  {
    Memory m;
    m.Put(Collection + 36, m.Key(40), 4);
    NoCollection(m.Sample()); ++cases;
  }
  {
    Memory m; m.bytes[Table + 255] ^= 1;
    const auto s = m.Sample();
    Check(!s.compatible && !s.collection_valid && s.learned_native_ids.empty(),
          "native table identity corruption refuses compatibility at the guard"); ++cases;
  }
  std::cout << cases << " Echoes live join adversarial cases passed\n";
}
