// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_resources.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
namespace {
constexpr uint64_t Main = 0x80000000, App = 0x1400000000,
                   Manager = 0x1400010000, Layout = 0x1400020000,
                   Control = 0x1400030000, Rows = 0x1400040000,
                   Row = 0x1400050000, Front = 0x1400060000,
                   Back = 0x1400070000, Actor = 0x1400080000,
                   BackActor = 0x1400090000;
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  template <class T> void put(uint64_t p, const T &v) {
    const auto *b = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof(T); ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *out, size_t n) const {
    auto *b = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      const auto it = bytes.find(p + i);
      if (it == bytes.end())
        return false;
      b[i] = it->second;
    }
    return true;
  }
  void object(uint64_t p, uint64_t vt, int64_t delta) {
    put(p, Main + vt + static_cast<uint64_t>(delta));
    put(p + 8, uint32_t{8});
  }
};
Memory Fixture(int64_t delta = 0) {
  Memory m;
  m.put(Main + 0x7cc8cc0, App);
  m.object(App, 0x6207610, delta);
  m.put(App + 0x2f0, uint8_t{26});
  m.put(Main + 0x7cc5b78, Manager);
  m.put(Manager + 8, uint32_t{8});
  m.put(Manager + 0x1678, Layout);
  m.object(Layout, 0x6221a30, delta);
  m.put(Layout + 0x2f8, Control);
  m.object(Control, 0x62281d0, delta);
  m.put(Control + 0x328, Rows);
  m.put(Control + 0x330, int32_t{1});
  m.put(Control + 0x334, int32_t{1});
  m.put(Rows, Row);
  m.object(Row, 0x6232f28, delta);
  m.put(Row + 0x370, Front);
  m.put(Row + 0x378, Back);
  m.object(Front, 0x63222a8, delta);
  m.object(Back, 0x6232f28, delta);
  m.put(Front + 0xdc, uint8_t{4});
  m.put(Back + 0xdc, uint8_t{4});
  m.put(Front + 0x368, Row);
  m.put(Back + 0x368, Row);
  m.put(Front + 0x2f8, Actor);
  m.put(Back + 0x2f8, BackActor);
  m.object(Actor, 0x63f39b8, delta);
  m.object(BackActor, 0x63f39b8, delta);
  m.put(Actor + 0x293, uint8_t{0});
  m.put(BackActor + 0x293, uint8_t{0});
  m.put(Actor + 0x298, uint32_t{904});
  m.put(BackActor + 0x298, uint32_t{10503});
  m.put(Front + 0x400, std::array<int32_t, 4>{727, 103, 765, 103});
  m.put(Back + 0x400, std::array<int32_t, 4>{558, 68, 747, 68});
  return m;
}
ot0::BattleSnapshot Battle() {
  ot0::BattleSnapshot b;
  b.valid = true;
  b.member_count = 2;
  b.members[0] = {901, 727, 103, 1};
  b.members[1] = {10503, 558, 68, 1};
  return b;
}
void FormationChecks() {
  auto m = Fixture();
  auto battle = Battle();
  const auto sample = [&](int64_t delta = 0) {
    return ot0::ReadBattleFormation(
        [&](uint64_t p, void *out, size_t n) { return m.read(p, out, n); },
        Main, battle, delta);
  };
  const ot0::BattleFormation original{true, {901, 0, 0, 0, 10503, 0, 0, 0}};
  assert(sample() == original);
  // KO auto-promotion follows actor binding, independently of HP-text caches
  // and saved/front-first BattleSnapshot member order.
  battle.members[0].hp = 0;
  m.put(Front + 0x2f8, BackActor);
  m.put(Back + 0x2f8, Actor);
  const ot0::BattleFormation promoted{true, {10503, 0, 0, 0, 901, 0, 0, 0}};
  assert(sample() == promoted);
  m.put(Front + 0x400, std::array<int32_t, 4>{-1, -1, -1, -1});
  m.put(Back + 0x400, std::array<int32_t, 4>{99999, 99999, 1, 1});
  assert(sample() == promoted);
  battle = Battle();
  for (int64_t delta : {int64_t{0x1ec0000000}, -int64_t{0x20000000}}) {
    m = Fixture(delta);
    assert(sample(delta) == original);
    assert(!sample().valid);
  }
  m = Fixture();
  battle.member_count = 1;
  m.put(Back + 0x2f8, uint64_t{});
  m.put(Back + 0xdc, uint8_t{1});
  const ot0::BattleFormation solo{true, {901, 0, 0, 0, 0, 0, 0, 0}};
  assert(sample() == solo);
  m.put(Control + 0x330, int32_t{3});
  m.put(Control + 0x334, int32_t{3});
  m.put(Rows + 8, uint64_t{});
  m.put(Rows + 16, uint64_t{});
  assert(sample() == solo);
  m.put(Row + 0x378, uint64_t{});
  assert(sample() == solo);
  m.put(Rows, uint64_t{});
  assert(!sample().valid);
  battle = Battle();
  for (uint8_t visibility : {uint8_t{1}, uint8_t{2}, uint8_t{5}}) {
    m = Fixture();
    m.put(Front + 0xdc, visibility);
    assert(!sample().valid);
  }
  m = Fixture();
  m.put(Back + 0x2f8, Actor);
  assert(!sample().valid);
  m = Fixture();
  m.put(Actor + 0x298, uint32_t{99999});
  assert(!sample().valid);
  m = Fixture();
  m.put(Actor + 0x293, uint8_t{1});
  assert(!sample().valid);
  m = Fixture();
  m.put(Actor + 8, uint32_t{0x40000000});
  assert(!sample().valid);
  m = Fixture();
  m.put(Actor, Main + 0x63f39b0);
  assert(!sample().valid);
  m = Fixture();
  m.put(Front + 0x368, Back);
  assert(!sample().valid);
  m = Fixture();
  m.put(Front + 0x2f8, uint64_t{});
  assert(!sample().valid);
  m = Fixture();
  battle.members[1].id = 904;
  assert(!sample().valid);
  battle = Battle();
  m = Fixture();
  unsigned count{};
  const auto torn = ot0::ReadBattleFormation(
      [&](uint64_t p, void *out, size_t n) {
        if (p == Front + 0x2f8 && ++count == 2)
          m.put(Front + 0x2f8, uint64_t{});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(!torn.valid);
  m = Fixture();
  const auto transition = ot0::ReadBattleFormation(
      [&](uint64_t p, void *out, size_t n) {
        if (p == BackActor + 0x298)
          m.put(Main + 0x7cc5b78, uint64_t{});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(!transition.valid);
  m = Fixture();
  battle.valid = false;
  assert(!sample().valid);
}
constexpr uint64_t Menu = 0x14000a0000, Panel = 0x14000b0000,
                   List = 0x14000c0000, MenuRow = 0x14000d0000;
void Visible(Memory &m, uint64_t p, int64_t delta) {
  const uint64_t slate = p + 0x1000, controller = p + 0x2000;
  m.put(p + 0x108, slate);
  m.put(p + 0x110, controller);
  m.put(slate, Main + 0x5d011e8 + static_cast<uint64_t>(delta));
  m.put(slate + 8, slate);
  m.put(controller + 8, int32_t{1});
  m.put(slate + 0x184, std::array<uint8_t, 2>{0x11, 1});
}
constexpr uint64_t Command = 0x1400100000, MainCommand = 0x1400110000,
                   SubCommand = 0x1400120000;
Memory CommandFixture(int64_t delta = 0) {
  auto m = Fixture(delta);
  m.put(Manager + 0x163c, uint8_t{2});
  m.put(Control + 0x300, Command);
  m.object(Command, 0x621cf90, delta);
  Visible(m, Command, delta);
  m.put(Command + 0x2f8, MainCommand);
  m.put(Command + 0x300, SubCommand);
  m.put(Command + 0x3a0, MainCommand);
  for (const auto list : {MainCommand, SubCommand}) {
    m.object(list, 0x621b8e0, delta);
    Visible(m, list, delta);
    m.put(list + 0x328, uint8_t{1});
    m.put(list + 0x329, uint8_t{1});
    m.put(list + 0x320, Actor);
  }
  return m;
}
void CommandChecks() {
  auto m = CommandFixture();
  auto battle = Battle();
  const auto sample = [&](int64_t delta = 0) {
    return ot0::ReadCommandActor(
        [&](uint64_t p, void *out, size_t n) { return m.read(p, out, n); },
        Main, battle, delta);
  };
  assert(sample() == 901);
  // A native ZR rebind can select the partner while queue and HP stay fixed.
  m.put(MainCommand + 0x320, BackActor);
  assert(sample() == 10503);
  battle.members[1].hp = 0;
  assert(sample() == 10503);
  battle = Battle();
  m = CommandFixture();
  m.put(Command + 0x3a0, SubCommand);
  m.put(SubCommand + 0x320, BackActor);
  m.put(SubCommand + 0x328, uint8_t{2});
  m.put(MainCommand + 0x1184, std::array<uint8_t, 2>{2, 1});
  assert(sample() == 10503);
  for (int64_t delta : {int64_t{0x1ec0000000}, -int64_t{0x20000000}}) {
    m = CommandFixture(delta);
    assert(sample(delta) == 901);
    assert(!sample());
  }
  for (uint8_t step : {uint8_t{0}, uint8_t{1}, uint8_t{4}, uint8_t{5},
                       uint8_t{6}, uint8_t{7}}) {
    m = CommandFixture();
    m.put(Manager + 0x163c, step);
    assert(!sample());
  }
  m = CommandFixture();
  m.put(Manager + 0x163c, uint8_t{3});
  assert(sample() == 901);
  for (uint64_t widget : {Command, MainCommand}) {
    m = CommandFixture();
    m.put(widget + 0x1184, std::array<uint8_t, 2>{2, 1});
    assert(!sample());
  }
  m = CommandFixture();
  m.put(Command + 0x3a0, Menu);
  assert(!sample());
  m = CommandFixture();
  m.put(MainCommand + 0x328, uint8_t{});
  assert(!sample());
  m = CommandFixture();
  m.put(MainCommand + 0x329, uint8_t{});
  assert(!sample());
  m = CommandFixture();
  m.put(MainCommand + 8, uint32_t{0x40000000});
  assert(!sample());
  m = CommandFixture();
  m.put(Actor + 0x293, uint8_t{1});
  assert(!sample());
  m = CommandFixture();
  m.put(Actor + 0x298, uint32_t{99999});
  assert(!sample());
  m = CommandFixture();
  battle.members[1].id = 904;
  assert(!sample());
  battle = Battle();
  m = CommandFixture();
  unsigned count{};
  const auto torn = ot0::ReadCommandActor(
      [&](uint64_t p, void *out, size_t n) {
        if (p == MainCommand + 0x320 && ++count == 2)
          m.put(MainCommand + 0x320, BackActor);
        return m.read(p, out, n);
      },
      Main, battle);
  assert(!torn);
  m = CommandFixture();
  battle.valid = false;
  assert(!sample());
}
Memory MenuFixture(int64_t delta = 0) {
  auto m = Fixture(delta);
  m.put(App + 0x2f0, uint8_t{25});
  m.put(Main + 0x7cc9a18, Manager);
  m.object(Manager, 0x63b6f18, delta);
  m.put(Manager + 0x38a, uint8_t{0x14});
  m.put(Manager + 0x408, Menu);
  m.object(Menu, 0x6393350, delta);
  m.put(Menu + 0x4a8, Panel);
  m.object(Panel, 0x63905b8, delta);
  m.put(Panel + 0x338, List);
  m.object(List, 0x646d038, delta);
  Visible(m, Menu, delta);
  Visible(m, Panel, delta);
  Visible(m, List, delta);
  m.put(List + 0x638, Rows);
  m.put(List + 0x640, int32_t{8});
  m.put(List + 0x644, int32_t{8});
  m.put(Rows, MenuRow);
  m.object(MenuRow, 0x638faa0, delta);
  m.put(MenuRow + 0x4f0, uint32_t{904});
  for (int i = 1; i < 8; ++i)
    m.put(Rows + i * 8, uint64_t{});
  const std::array<uint32_t, 4> nums{727, 765, 103, 103};
  for (int i = 0; i < 4; ++i) {
    const uint64_t text = MenuRow + 0x1000 + i * 0x1000;
    const uint64_t data = text + 0x400, chars = text + 0x800;
    m.put(MenuRow + 0x460 + i * 8, text);
    m.object(text, 0x6367cc0, delta);
    m.put(text + 0x1a0, data);
    m.put(data, Main + 0x5b08170 + static_cast<uint64_t>(delta));
    m.put(data + 8, int32_t{1});
    m.put(data + 0x28, chars);
    const auto n = std::to_string(nums[i]);
    m.put(data + 0x30, int32_t(n.size() + 1));
    m.put(data + 0x34, int32_t{8});
    for (size_t j = 0; j <= n.size(); ++j)
      m.put(chars + j * 2, char16_t(j == n.size() ? 0 : n[j]));
  }
  return m;
}
void MenuChecks() {
  ot0::Snapshot party;
  party.valid = true;
  party.state = 25;
  party.members[3] = {901, 10, 727, 103, true};
  auto m = MenuFixture();
  const auto sample = [&](int64_t delta = 0) {
    return ot0::ReadMenuResourceLimits(
        [&](uint64_t p, void *v, size_t n) { return m.read(p, v, n); }, Main,
        party, delta);
  };
  auto expected = std::array<ot0::ResourceLimits, 8>{};
  expected[3] = {765, 103, true};
  assert(sample() == expected);
  for (int64_t delta : {int64_t{0x1ec0000000}, -int64_t{0x20000000}}) {
    m = MenuFixture(delta);
    assert(sample(delta) == expected);
    assert(sample() == decltype(expected){});
  }
  m = MenuFixture();
  party.members[3].hp = 726;
  assert(sample() == decltype(expected){});
  party.members[3].hp = 727;
  for (uint64_t p : {Menu, Panel, List}) {
    m = MenuFixture();
    m.put(p + 0x1000 + 0x184, std::array<uint8_t, 2>{2, 1});
    assert(sample() == decltype(expected){});
  }
  for (uint8_t current : {uint8_t{0}, uint8_t{19}, uint8_t{21}}) {
    m = MenuFixture();
    m.put(Manager + 0x38a, current);
    assert(sample() == decltype(expected){});
  }
  m = MenuFixture();
  m.put(MenuRow + 0x1800, char16_t{u'-'});
  assert(sample() == decltype(expected){});
  m = MenuFixture();
  m.put(MenuRow + 0x1400, uint64_t{0});
  assert(sample() == decltype(expected){});
  m = MenuFixture();
  m.put(MenuRow + 0x1430, int32_t{1000});
  assert(sample() == decltype(expected){});
  m = MenuFixture();
  m.put(MenuRow + 8, uint32_t{0x40000000});
  assert(sample() == decltype(expected){});
  m = MenuFixture();
  unsigned count{};
  const auto torn = ot0::ReadMenuResourceLimits(
      [&](uint64_t p, void *v, size_t n) {
        if (p == MenuRow + 0x4f0 && ++count == 2)
          m.put(Manager + 0x408, uint64_t{});
        return m.read(p, v, n);
      },
      Main, party);
  assert(torn == decltype(expected){});
  party.state = 23;
  m = MenuFixture();
  assert(sample() == decltype(expected){});
}
void LiveReplay(bool menu = false, bool formation = false) {
  const char *path = std::getenv(formation ? "OT0_BATTLE_FORMATION_MEMORY"
                                 : menu    ? "OT0_MENU_RESOURCE_MEMORY"
                                           : "OT0_RESOURCE_MEMORY");
  if (!path)
    return;
  std::ifstream input(path, std::ios::binary);
  assert(input);
  const auto read = [&](auto &value) {
    input.read(reinterpret_cast<char *>(&value), sizeof(value));
    assert(input);
  };
  std::array<char, 8> magic{};
  read(magic);
  assert(std::memcmp(magic.data(), "OT0MAX1\0", 8) == 0);
  uint64_t main{};
  uint32_t member_count{}, count{};
  read(main);
  read(member_count);
  assert(member_count == 7);
  ot0::BattleSnapshot battle;
  battle.valid = true;
  battle.member_count = int(member_count);
  for (unsigned i = 0; i < member_count; ++i) {
    read(battle.members[i].id);
    read(battle.members[i].hp);
    read(battle.members[i].sp);
    read(battle.members[i].bp);
  }
  read(count);
  assert(count < 4096);
  Memory memory;
  for (unsigned i = 0; i < count; ++i) {
    uint64_t address{};
    uint32_t n{};
    read(address);
    read(n);
    assert(n && n < 65536);
    for (unsigned j = 0; j < n; ++j) {
      uint8_t value{};
      read(value);
      memory.put(address + j, value);
    }
  }
  assert(input.peek() == std::char_traits<char>::eof());
  if (formation) {
    const auto memory_read = [&](uint64_t p, void *out, size_t n) {
      return memory.read(p, out, n);
    };
    const ot0::BattleFormation expected{
        true, {904, 40204, 10905, 10101, 10804, 10503, 11407, 0}};
    assert(ot0::ReadBattleFormation(memory_read, main, battle) == expected);
    const auto q = [&](uint64_t p) {
      uint64_t value{};
      assert(memory.read(p, &value, sizeof value));
      return value;
    };
    const auto rows = q(q(q(q(main + 0x7cc5b78) + 0x1678) + 0x2f8) + 0x328);
    for (unsigned i = 0; i < 4; ++i) {
      const auto row = q(rows + i * 8);
      for (uint64_t off : {uint64_t{0x370}, uint64_t{0x378}})
        memory.put(q(row + off) + 0x400, int32_t{-9999});
    }
    assert(ot0::ReadBattleFormation(memory_read, main, battle) == expected);
    std::cout << "Native KO promotion replayed: Stia front / Celsus back, "
                 "independent of HP cache\n";
    return;
  }
  ot0::Snapshot party;
  party.valid = true;
  party.state = 25;
  for (unsigned i = 0; i < 7; ++i)
    party.members[i] = {battle.members[i].id, 1, uint32_t(battle.members[i].hp),
                        uint32_t(battle.members[i].sp), true};
  const auto memory_read = [&](uint64_t p, void *out, size_t n) {
    return memory.read(p, out, n);
  };
  const auto limits =
      menu ? ot0::ReadMenuResourceLimits(memory_read, main, party)
           : ot0::ReadBattleResourceLimits(memory_read, main, battle);
  const std::array<uint32_t, 7> hp{765, 747, 857, 908, 908, 850, 764};
  const std::array<uint32_t, 7> sp{103, 68, 60, 60, 68, 119, 80};
  for (unsigned i = 0; i < member_count; ++i)
    assert(limits[i].valid && limits[i].max_hp == hp[i] &&
           limits[i].max_sp == sp[i]);
  assert(!limits[7].valid);
  std::cout
      << "Seven native battle maxima replayed by current actor identity\n";
}
void CommandReplay() {
  const auto *path = std::getenv("OT0_COMMAND_ACTOR_MEMORY");
  if (!path)
    return;
  std::ifstream input(path, std::ios::binary);
  assert(input);
  const auto get = [&](auto &value) {
    input.read(reinterpret_cast<char *>(&value), sizeof value);
    assert(input);
  };
  std::array<char, 8> magic{};
  get(magic);
  assert(std::memcmp(magic.data(), "OT0CMD1\0", 8) == 0);
  uint64_t main{};
  uint32_t expected{}, count{};
  get(main);
  get(expected);
  get(count);
  assert(count > 0 && count <= 8);
  ot0::BattleSnapshot battle;
  battle.valid = true;
  battle.member_count = int(count);
  for (unsigned i = 0; i < count; ++i) {
    get(battle.members[i].id);
    get(battle.members[i].hp);
    get(battle.members[i].sp);
    get(battle.members[i].bp);
  }
  get(count);
  assert(count < 4096);
  Memory memory;
  for (unsigned i = 0; i < count; ++i) {
    uint64_t address{};
    uint32_t n{};
    get(address);
    get(n);
    assert(n && n <= 65536);
    for (unsigned j = 0; j < n; ++j) {
      uint8_t value{};
      get(value);
      memory.put(address + j, value);
    }
  }
  assert(input.peek() == std::char_traits<char>::eof());
  const auto read = [&](uint64_t p, void *out, size_t n) {
    return memory.read(p, out, n);
  };
  assert(ot0::ReadCommandActor(read, main, battle) == expected);
  // A retained command widget must never override native action/results phase.
  uint64_t bm{};
  assert(memory.read(main + 0x7cc5b78, &bm, sizeof bm));
  memory.put(bm + 0x163c, uint8_t{4});
  assert(!ot0::ReadCommandActor(read, main, battle));
  std::cout
      << "Native visible command actor replayed independently of turn queue\n";
}
} // namespace
int main() {
  LiveReplay();
  LiveReplay(true);
  LiveReplay(false, true);
  FormationChecks();
  CommandChecks();
  CommandReplay();
  MenuChecks();
  auto battle = Battle();
  auto m = Fixture();
  const auto read = [&](uint64_t p, void *out, size_t n) {
    return m.read(p, out, n);
  };
  const auto sample = [&](int64_t delta = 0) {
    return ot0::ReadBattleResourceLimits(read, Main, battle, delta);
  };
  const auto original = m.bytes;
  const auto expected = sample();
  assert(expected[0].valid && expected[0].max_hp == 765 &&
         expected[0].max_sp == 103);
  assert(expected[1].valid && expected[1].max_hp == 747 && !expected[2].valid &&
         m.bytes == original);
  // Native actor identities join by member rather than UI row order.
  std::swap(battle.members[0], battle.members[1]);
  assert(sample()[0] == expected[1] && sample()[1] == expected[0]);
  battle = Battle();
  for (const int64_t delta : {int64_t{0x4000000000}, int64_t{-0x200000}}) {
    m = Fixture(delta);
    assert(sample(delta) == expected);
    assert(!sample()[0].valid);
    m = Fixture();
    assert(sample(delta) == expected);
  }
  m = Fixture();
  m.put(Front + 0x400, int32_t{726}); // old widget number, new game state
  assert(!sample()[0].valid && sample()[1].valid);
  m = Fixture();
  m.put(Front + 0x408, int32_t{0});
  assert(!sample()[0].valid);
  m = Fixture();
  m.put(Front + 0x40c, int32_t{100}); // current SP exceeds a stale maximum
  assert(!sample()[0].valid);
  m = Fixture();
  m.put(Actor + 0x293, uint8_t{1}); // cannot borrow an enemy's resource data
  assert(!sample()[0].valid);
  m = Fixture();
  m.put(Actor + 0x298, uint32_t{10503});
  assert(!sample()[0].valid && sample()[1].valid);
  m = Fixture();
  m.put(Front + 0x368, Back);
  assert(!sample()[0].valid);
  m = Fixture();
  m.put(Actor + 8, uint32_t{0x40000000});
  assert(!sample()[0].valid);
  m = Fixture();
  m.put(Actor, Main + 0x63f39c0);
  assert(!sample()[0].valid);
  for (const int count : {-1, 33}) {
    m = Fixture();
    m.put(Control + 0x330, int32_t{count});
    assert(sample() == decltype(expected){});
  }
  for (const uint8_t state : {uint8_t{10}, uint8_t{23}, uint8_t{25}}) {
    m = Fixture();
    m.put(App + 0x2f0, state);
    assert(sample() == decltype(expected){});
  }
  m = Fixture();
  unsigned count = 0;
  const auto torn = ot0::ReadBattleResourceLimits(
      [&](uint64_t p, void *out, size_t n) {
        if (p == Front + 0x400 && ++count == 2)
          m.put(Front + 0x400, int32_t{726});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(!torn[0].valid && torn[1].valid);
  m = Fixture();
  const auto transition = ot0::ReadBattleResourceLimits(
      [&](uint64_t p, void *out, size_t n) {
        if (p == BackActor + 0x298)
          m.put(Main + 0x7cc5b78, uint64_t{0});
        return m.read(p, out, n);
      },
      Main, battle);
  assert(transition == decltype(expected){});
  battle.valid = false;
  assert(sample() == decltype(expected){});
  std::cout << "Native resource identity, freshness, relocation and lifecycle "
               "checks passed\n";
}
