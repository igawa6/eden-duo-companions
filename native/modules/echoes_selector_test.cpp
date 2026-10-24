// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_selector.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_map>

namespace {
constexpr uint64_t Base = 0x80488000, Head = Base + 0x34bf070;
constexpr uint64_t Node1 = 0x2107000000, Node2 = Node1 + 0x100;
constexpr uint64_t Other = Node1 + 0x1000, Owner = 0x2107100020;
constexpr uint64_t Records = 0x2107200000, Player = 0x2107300000, State = 0x2107400000;
constexpr uint64_t List = Owner + 0x150;
size_t cases{};
void Check(bool ok, const char* why) {
  if (!ok) { std::cerr << "FAIL after " << cases << " cases: " << why << '\n'; std::exit(1); }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t, size_t)> before;
  size_t reads{}, largest{};
  void Put(uint64_t at, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) bytes[at + i] = uint8_t(v >> (i * 8));
  }
  explicit Memory(size_t count = 3) {
    for (const auto& g : echoes::IdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i) bytes[Base + g.offset + i] = g.bytes[i];
    for (const auto& g : echoes::FavoriteIdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i) bytes[Base + g.offset + i] = g.bytes[i];
    Put(Base + 0x34bc840, Player, 8); Put(Player + 8, State, 8);
    Put(State + 0x1970, 40, 2);
    Put(Head, Node2, 8); Put(Head + 8, Node1, 8); Put(Head + 16, 2, 8);
    Put(Node1, Head, 8); Put(Node1 + 8, Node2, 8); Put(Node1 + 16, Other, 8);
    Put(Other, Base + 0x3410000, 8);
    Put(Node2, Node1, 8); Put(Node2 + 8, Head, 8); Put(Node2 + 16, Owner, 8);
    Put(Owner - 32, Base + 0x34266d0, 8); Put(Owner, Base + 0x34119c8, 8);
    Put(Owner + 0x88, 2, 3); Put(Owner + 0x7f50, 0x1000, 8);
    Put(Owner + 0x7faa, 1, 1); Put(List, Base + 0x342c260, 8);
    Put(List + 0x2c48, Records, 8);
    Put(List + 0x2c50, Records + count * 56, 8);
    Put(List + 0x2c58, Records + count * 56, 8);
    Put(List + 0x2c78, 0, 8);
    for (size_t i = 0; i < count * 56; ++i) bytes[Records + i] = 0;
    for (size_t i = 0; i < count; ++i) Put(Records + i * 56, i == 0 ? 40 : 873 + i, 2);
  }
  void Sort(uint8_t value = 1) {
    Put(Owner + 0x2dec, 0, 1);
    Put(Owner + 0x2df8, 0x2107500000, 8);
    Put(Owner + 0x2e00, 0x2107500008, 8);
    Put(Owner + 0x2e08, 0x2107500008, 8);
    Put(Owner + 0x2e10, 0, 8);
    Put(Owner + 0x2e18, 5, 8);
    Put(0x2107500000, 0x2107600000, 8);
    Put(0x2107600000, value, 1);
  }
  bool Read(uint64_t at, void* out, size_t n) {
    ++reads; largest = std::max(largest, n);
    const size_t visit = ++visits[at];
    if (before) before(at, n, visit);
    if (!at || n > std::numeric_limits<uint64_t>::max() - at) return false;
    auto* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
      const auto it = bytes.find(at + i);
      if (it == bytes.end()) return false;
      p[i] = it->second;
    }
    return true;
  }
  echoes::SelectorSnapshot Sample(bool build = true, uint64_t base = Base) {
    return echoes::ReadSelector([&](auto a, auto p, auto n) { return Read(a, p, n); }, base, build);
  }
};
void EmptySelector(const echoes::SelectorSnapshot& s) {
  Check(s.compatible && !s.selector_valid && !s.held &&
        s.ordered_native_ids.empty() && s.highlighted_index == -1 && s.target_index == -1 &&
        s.highlighted_native_id == 0xffff && s.source == echoes::SelectorSource{},
        "invalid selector must clear ownership, list and highlight");
}
} // namespace

int main() {
  {
    Memory m;
    m.Put(Records + 56 + 0x30, 1, 1);
    auto s = m.Sample();
    Check(s.held && s.favorite_valid &&
              s.favorite_native_ids == std::vector<uint16_t>{874},
          "held favorites use the native record bit, independent of equipped ID");
    m.Put(Records + 56 + 0x30, 0, 1);
    s = m.Sample();
    Check(s.held && s.favorite_valid && s.favorite_native_ids.empty(),
          "native unfavorite observation updates to a known empty set");
    m.Put(Owner + 0x88, 0, 3);
    s = m.Sample();
    Check(!s.held && !s.favorite_valid && s.favorite_native_ids.empty(),
          "closed selector never exposes retained favorite records");
    ++cases;
  }
  for (bool guard_fault : {false, true}) {
    Memory m;
    if (guard_fault)
      m.bytes[Base + echoes::FavoriteIdentityGuards[1].offset] ^= 1;
    else
      m.Put(Records + 0x30, 2, 1);
    const auto s = m.Sample();
    Check(s.held && s.selector_valid && !s.favorite_valid &&
              s.favorite_native_ids.empty() && s.ordered_native_ids.size() == 3,
          "unsupported favorite metadata preserves the independently verified held selector");
    ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) m.Put(Records + 0x30, 1, 1);
    };
    const auto s = m.Sample();
    Check(!s.selector_valid && !s.favorite_valid && s.favorite_native_ids.empty(),
          "favorite toggle during complete record comparison cannot publish torn state");
    ++cases;
  }
  {
    Memory m;
    auto s = m.Sample();
    Check(s.compatible && s.selector_valid && s.held && s.equipped_valid &&
          s.equipped_native_id == 40 && s.highlighted_index == 0 && s.target_index == 0 &&
          s.highlighted_native_id == 40 &&
          s.ordered_native_ids == std::vector<uint16_t>{40, 874, 875} &&
          s.source.owner == Owner && s.source.records == Records,
          "stable native order and highlighted ID are independent of ownership storage");
    // A simulated native lifecycle: closed -> held -> move while held -> release.
    m.Put(Owner + 0x88, 0, 3);
    s = m.Sample();
    Check(s.selector_valid && !s.held && s.ordered_native_ids.empty() &&
          s.highlighted_index == -1 && s.equipped_native_id == 40,
          "closed state clears cached list while preserving independent equipped read");
    m.Put(Owner + 0x88, 2, 3); m.Put(List + 0x2c78, 1, 4); m.Put(List + 0x2c7c, 1, 4);
    s = m.Sample();
    Check(s.held && s.highlighted_native_id == 874 && s.equipped_native_id == 40,
          "held cursor navigation must not invent an equip commit");
    m.Put(Owner + 0x88, 0, 3); m.Put(State + 0x1970, 874, 2);
    s = m.Sample();
    Check(s.selector_valid && !s.held && s.ordered_native_ids.empty() &&
          s.highlighted_native_id == 0xffff && s.equipped_native_id == 874,
          "release snapshot reflects native equip without retaining an actionable grid");
    ++cases;
  }
  {
    Memory m; m.Put(List + 0x2c7c, 1, 4);
    const auto s = m.Sample();
    Check(s.held && s.highlighted_index == 0 && s.target_index == 1 &&
          s.highlighted_native_id == 40,
          "native target cursor must remain distinct during selection animation");
    ++cases;
  }
  {
    Memory m;
    Check(!m.Sample(false).compatible && m.reads == 0, "wrong build performs no reads");
    Check(!m.Sample(true, 0).compatible && m.reads == 0, "null base performs no reads");
    Check(!m.Sample(true, UINT64_MAX - 7).compatible && m.reads == 0,
          "overflow base performs no reads");
    Check(!echoes::ReadSelector({}, Base, true).compatible, "empty callback refused");
    ++cases;
  }
  for (const auto& g : echoes::IdentityGuards) {
    Memory m; m.bytes[Base + g.offset + 255] ^= 1;
    Check(!m.Sample().compatible, "corrupt immutable build window fails closed"); ++cases;
  }
  for (const auto [field, value] : std::vector<std::pair<uint64_t, uint64_t>>{
      {Owner, Base + 0x34119c0}, {Owner - 32, Base + 0x34266d8},
      {List, Base + 0x342e498}, {Owner + 0x7f50, 0x1000000000}}) {
    Memory m; m.Put(field, value, 8); EmptySelector(m.Sample()); ++cases;
  }
  for (uint64_t mode : {uint64_t(0), uint64_t(2), uint64_t(255)}) {
    Memory m; m.Put(Owner + 0x7faa, mode, 1); auto s = m.Sample(); EmptySelector(s);
    Check(s.equipped_valid, "unverified mode does not erase separately stable equipped ID"); ++cases;
  }
  for (uint64_t state : {uint64_t(1), uint64_t(3), uint64_t(255)}) {
    Memory m; m.Put(Owner + 0x88, state, 1); EmptySelector(m.Sample()); ++cases;
  }
  for (uint64_t cursor : {uint64_t(3), uint64_t(UINT32_MAX), uint64_t(INT32_MAX)}) {
    Memory m; m.Put(List + 0x2c78, cursor, 4); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(List + 0x2c7c, UINT32_MAX, 4); EmptySelector(m.Sample()); ++cases;
  }
  for (uint64_t pointer : {uint64_t(0), Records + 1, UINT64_MAX - 7}) {
    Memory m; m.Put(List + 0x2c48, pointer, 8); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(List + 0x2c50, Records + 55, 8); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(List + 0x2c58, Records + 56, 8); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(List + 0x2c50, Records + 513 * 56, 8);
    m.Put(List + 0x2c58, Records + 513 * 56, 8); EmptySelector(m.Sample());
    Check(m.visits[Records] == 0 && m.largest <= 256, "oversize vector rejected before payload read"); ++cases;
  }
  {
    Memory m(512);
    // Give each row a distinct catalogue ID within the supported range.
    for (size_t i = 0; i < 512; ++i) m.Put(Records + i * 56, i, 2);
    Check(m.Sample().ordered_native_ids.size() == 512 && m.largest == 512 * 56,
          "maximum payload is bounded and handles little-endian IDs"); ++cases;
  }
  for (bool duplicate : {false, true}) {
    Memory m; m.Put(Records + 56, duplicate ? 40 : 0xffff, 2);
    EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.bytes.erase(Records + 57); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(Node2 + 8, Node1, 8); EmptySelector(m.Sample());
    Check(m.reads < 50, "cyclic registry stops promptly"); ++cases;
  }
  {
    Memory m; m.Put(Node2, Head, 8); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(Other, Base + 0x34119c8, 8); EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m; m.Put(Head, Head, 8); m.Put(Head + 8, Head, 8); m.Put(Head + 16, 0, 8);
    auto s = m.Sample(); EmptySelector(s);
    Check(s.equipped_valid, "absent title selector does not invent held context"); ++cases;
  }
  {
    Memory m; m.Put(Base + 0x34bc840, 0, 8);
    const auto s = m.Sample(); Check(s.held && !s.equipped_valid && s.equipped_native_id == 0xffff,
                                  "equipped pointer chain independently fails closed"); ++cases;
  }
  // Reuse still-readable old allocations while changing the live registry,
  // owner header, displayed order or player identity during the copy.
  for (uint64_t field : {Head + 8, Node2 + 16, Owner + 0x7f50,
                         List + 0x2c48, List + 0x2c78}) {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) {
        const size_t width = field == List + 0x2c78 ? 4 : 8;
        m.Put(field, field == List + 0x2c78 ? 1 : 0, width);
      }
    };
    EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) m.Put(Records + 2 * 56, 876, 2);
    };
    EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) m.Put(Owner + 0x88, 0, 1);
    };
    EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) m.Put(Player + 8, 0, 8);
    };
    const auto s = m.Sample(); Check(s.held && !s.equipped_valid && s.equipped_native_id == 0xffff,
                                  "detached readable player is not published as current"); ++cases;
  }
  for (size_t count : {size_t(128), size_t(129)}) {
    Memory m;
    const uint64_t first = 0x2107500000;
    m.Put(Head, first + (count - 1) * 32, 8);
    m.Put(Head + 8, first, 8); m.Put(Head + 16, count, 8);
    for (size_t i = 0; i < count; ++i) {
      const uint64_t node = first + i * 32;
      m.Put(node, i ? node - 32 : Head, 8);
      m.Put(node + 8, i + 1 == count ? Head : node + 32, 8);
      m.Put(node + 16, i + 1 == count ? Owner : Other, 8);
    }
    const auto result = m.Sample();
    if (count == 128) Check(result.held, "exact registry cap remains supported");
    else {
      EmptySelector(result);
      Check(m.reads < 600, "oversized registry is rejected without an unbounded walk");
    }
    ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) m.Put(Head + 16, 3, 8);
    };
    EmptySelector(m.Sample()); ++cases;
  }
  {
    Memory m;
    m.Put(Owner + 0x88, 0, 3);
    m.bytes.erase(List); // A closed picker must never touch its stale vector.
    m.Put(List + 0x2c48, UINT64_MAX, 8);
    const auto result = m.Sample();
    Check(result.selector_valid && !result.held && m.visits[List] == 0 &&
          result.ordered_native_ids.empty(), "closed state ignores inaccessible cached list");
    ++cases;
  }
  for (uint64_t id : {uint64_t(1225), uint64_t(0xffff)}) {
    Memory m; m.Put(State + 0x1970, id, 2);
    const auto result = m.Sample();
    Check(result.held && !result.equipped_valid && result.equipped_native_id == 0xffff,
          "invalid equipped ID cannot become a display default"); ++cases;
  }
  {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == State + 0x1970 && visit == 4) m.Put(Player + 8, 0, 8);
    };
    const auto result = m.Sample();
    Check(result.held && !result.equipped_valid,
          "player detach during final ID read is caught by post-read owner checks");
    ++cases;
  }
  for (uint8_t value : {1, 2, 4, 8, 16}) {
    Memory m;
    m.Sort(value);
    const auto s = m.Sample();
    Check(s.held && s.sort_valid && s.sort_enum == value &&
              s.source.sort_enum == value && s.source.sort_valid,
          "known native sort observed without guessing from order");
    ++cases;
  }
  for (uint8_t value : {0, 3, 32, 255}) {
    Memory m;
    m.Sort(value);
    const auto s = m.Sample();
    Check(s.held && !s.sort_valid && s.sort_enum == value,
          "unknown enum retains raw metadata but supplies no label");
    ++cases;
  }
  {
    Memory m;
    const auto s = m.Sample();
    Check(s.held && !s.sort_valid,
          "missing optional sort preserves held selector");
    ++cases;
  }
  for (auto pair : {std::pair<uint64_t, uint64_t>{0x2e18, 0},
                    {0x2e18, 7},
                    {0x2e10, UINT64_MAX},
                    {0x2df8, 0},
                    {0x2e00, 0x2107500001},
                    {0x2e00, 0x2107500048},
                    {0x2dec, 5}}) {
    Memory m;
    m.Sort();
    m.Put(Owner + pair.first, pair.second, pair.first == 0x2dec ? 1 : 8);
    const auto s = m.Sample();
    Check(s.held && !s.sort_valid,
          "malformed bounded deque fails optional metadata only");
    ++cases;
  }
  {
    Memory m;
    m.Sort();
    m.Put(Owner + 0x2e10, 4095, 8);
    m.Put(Owner + 0x2e00, 0x2107500010, 8);
    m.Put(Owner + 0x2dec, 1, 1);
    m.Put(0x2107500008, 0x2107700000, 8);
    m.Put(0x2107700000, 8, 1);
    const auto s = m.Sample();
    Check(s.sort_valid && s.sort_enum == 8,
          "byte deque selection crosses block boundary");
    ++cases;
  }
  for (uint64_t changed :
       {Owner + 0x2df8, Owner + 0x2dec, uint64_t(0x2107600000)}) {
    Memory m;
    m.Sort();
    m.before = [&](auto at, auto, auto visit) {
      if (at == changed && visit == 2)
        m.Put(0x2107600000, 2, 1);
    };
    const auto s = m.Sample();
    Check(s.held && !s.sort_valid && s.sort_enum == 0,
          "torn optional sort clears metadata without changing held readiness");
    ++cases;
  }
  {
    Memory m;
    m.Sort();
    auto a = m.Sample();
    m.Sort(2);
    auto b = m.Sample();
    Check(a.ordered_native_ids == b.ordered_native_ids && a.source != b.source,
          "sort change invalidates source even when row order stays identical");
    ++cases;
  }
  {
    Memory m;
    m.Sort();
    m.Put(Owner + 0x88, 0, 3);
    const auto s = m.Sample();
    Check(!s.held && !s.sort_valid && s.sort_enum == 0 &&
              m.visits[Owner + 0x2df8] == 0,
          "closed native picker never reads cached sort");
    ++cases;
  }
  std::cout << cases << " Echoes selector adversarial cases passed\n";
}
