// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_equipment.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace {
constexpr uint64_t Main = 0x80000000, App = 0x1400000000,
                   Manager = 0x1400010000, Entries = 0x1400020000,
                   Shop = 0x1400030000, Window = 0x1400040000,
                   Controller = 0x1400050000, Items = 0x1400060000,
                   Characters = 0x1400070000, Rows = 0x1400080000,
                   Row = 0x1400090000, Panel = 0x14000a0000,
                   Parameters = 0x14000b0000, Parts = 0x14000c0000,
                   Part = 0x14000d0000;
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  template <class T> void put(uint64_t p, const T &v) {
    auto b = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof(v); ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *out, size_t n) const {
    auto b = static_cast<uint8_t *>(out);
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
    put(p + 0xdc, uint8_t{});
  }
  void visible(uint64_t p, int64_t delta) {
    const auto slate = p + 0x1000, controller = p + 0x2000;
    put(p + 8, uint32_t{8});
    put(p + 0xdc, uint8_t{});
    put(p + 0x108, slate);
    put(p + 0x110, controller);
    put(slate, Main + 0x5d011e8 + static_cast<uint64_t>(delta));
    put(slate + 8, slate);
    put(controller + 8, int32_t{1});
    put(slate + 0x184, std::array<uint8_t, 2>{0x11, 1});
  }
  void array(uint64_t p, uint64_t data, int32_t n) {
    put(p, data);
    put(p + 8, n);
    put(p + 12, n);
  }
  void string(uint64_t p, uint64_t data, const std::u16string &s) {
    array(p, data, int32_t(s.size() + 1));
    for (size_t i = 0; i <= s.size(); ++i)
      put(data + i * 2, char16_t(i < s.size() ? s[i] : 0));
  }
  void text(uint64_t p, const std::u16string &s, int64_t delta) {
    object(p, 0x6367cc0, delta);
    put(p + 0x1a0, p + 0x400);
    put(p + 0x400, Main + 0x5b08170 + static_cast<uint64_t>(delta));
    put(p + 0x408, int32_t{1});
    string(p + 0x428, p + 0x800, s);
  }
};
Memory Fixture(int64_t delta = 0) {
  Memory m;
  m.put(Main + 0x7cc8cc0, App);
  m.object(App, 0x6207610, delta);
  m.put(App + 0x2f0, uint8_t{25});
  m.put(Main + 0x7cc9a18, Manager);
  m.object(Manager, 0x63b6f18, delta);
  m.put(Manager + 0x38a, uint8_t{7});
  m.put(Manager + 0x2f0, Entries);
  m.put(Manager + 0x2f8, int32_t{1});
  m.put(Manager + 0x324, int32_t{});
  m.put(Manager + 0x328, int32_t{});
  m.put(Manager + 0x330, uint64_t{});
  m.put(Manager + 0x338, int32_t{1});
  m.put(Entries, uint8_t{7});
  m.put(Entries + 8, Shop);
  m.object(Shop, 0x64680c8, delta);
  m.put(Shop + 0x350, uint8_t{3});
  m.visible(Shop, delta);
  m.put(Shop + 0x398, Window);
  m.object(Window, 0x6339278, delta);
  m.visible(Window, delta);
  m.put(Window + 0x3b0, Controller);
  m.object(Controller, 0x646e168, delta);
  m.put(Controller + 0x4b8, uint8_t{});
  m.put(Controller + 0x378, int32_t{});
  m.put(Controller + 0x444, int32_t{});
  m.put(Controller + 0x9e0, int32_t{1});
  m.array(Window + 0x468, Items, 1);
  m.put(Items + 8, uint32_t{1234});
  m.string(Items + 0x18, Items + 0x100, u"Steel Sword");
  m.put(Window + 0x3d8, Characters);
  m.object(Characters, 0x63a49e8, delta);
  m.array(Characters + 0x638, Rows, 1);
  m.put(Characters + 0x378, int32_t{});
  m.put(Rows, Row);
  m.object(Row, 0x63a6e10, delta);
  m.put(Row + 0x3e5, uint8_t{1});
  m.put(Row + 0x3f8, uint32_t{904});
  m.put(Row + 0x450, uint32_t{1234});
  m.put(Row + 0x454, int32_t{});
  m.put(Row + 0x4e8, Panel + 0x3000);
  m.put(Panel + 0x3000 + 8, uint32_t{8});
  m.put(Panel + 0x3000 + 0xdc, uint8_t{2});
  m.put(Row + 0x4f0, Panel);
  m.visible(Panel, delta);
  m.array(Row + 0x468, Parameters, 4);
  m.array(Row + 0x4f8, Parts, 4);
  m.put(Parameters, std::array<uint32_t, 4>{9, 10, 0, 0});
  for (int i = 0; i < 2; ++i) {
    const auto p = Part + uint64_t(i) * 0x10000;
    m.put(Parts + i * 8, p);
    m.object(p, 0x63a77a8, delta);
    m.visible(p, delta);
    m.put(p + 0x348, p + 0x3000);
    m.visible(p + 0x3000, delta);
    m.put(p + 0x328, p + 0x6000);
    m.put(p + 0x330, p + 0x7000);
    m.put(p + 0x340, p + 0x8000);
    m.text(p + 0x6000, i ? u"Elem. Atk." : u"Phys. Atk.", delta);
    m.text(p + 0x7000, i ? u"10" : u"25", delta);
    m.text(p + 0x8000, i ? u"8" : u"35", delta);
  }
  return m;
}
void LiveReplay() {
  const auto *path = std::getenv("OT0_EQUIPMENT_MEMORY");
  if (!path)
    return;
  std::ifstream input(path, std::ios::binary);
  assert(input);
  const auto get = [&](auto &out) {
    input.read(reinterpret_cast<char *>(&out), sizeof(out));
    assert(input);
  };
  const auto string = [&] {
    uint32_t n{};
    get(n);
    assert(n > 0 && n < 1024);
    std::string out(n, '\0');
    input.read(out.data(), n);
    assert(input);
    return out;
  };
  std::array<char, 8> magic{};
  get(magic);
  assert(std::memcmp(magic.data(), "OT0SHP1\0", 8) == 0);
  uint64_t main{}, item_record{};
  uint32_t item_id{}, selected_id{}, count{};
  get(main);
  get(item_record);
  get(item_id);
  get(selected_id);
  const auto name = string();
  get(count);
  assert(count >= 1 && count <= 8);
  std::vector<ot0::EquipmentMemberComparison> expected;
  for (unsigned i = 0; i < count; ++i) {
    ot0::EquipmentMemberComparison member;
    uint32_t stat_count{};
    get(member.id);
    get(stat_count);
    assert(stat_count >= 1 && stat_count <= 4);
    member.stat_count = uint8_t(stat_count);
    member.valid = true;
    for (unsigned j = 0; j < stat_count; ++j) {
      auto &stat = member.stats[j];
      get(stat.parameter_id);
      get(stat.current);
      get(stat.candidate);
      stat.label = string();
    }
    expected.push_back(std::move(member));
  }
  get(count);
  assert(count < 16384);
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
  const auto party = ot0::ReadSnapshot(read, main);
  assert(party.valid);
  const auto shop = ot0::ReadShopComparison(read, main, party);
  assert(shop.valid && shop.candidate_item_id == item_id &&
         shop.candidate_name == name && shop.selected_member_id == selected_id);
  if (const auto *cannot = std::getenv("OT0_EQUIPMENT_EXPECT_CANNOT")) {
    unsigned count_cannot{};
    for (const auto &member : shop.members)
      count_cannot += member.cannot_equip;
    assert(count_cannot == std::strtoul(cannot, nullptr, 10));
  }
  bool independent = false;
  const auto identity = [](uint32_t id) {
    return id >= 900 && id <= 909 ? 900 : id;
  };
  for (const auto &member : expected) {
    int index = -1;
    for (int i = 0; i < 8; ++i)
      if (party.members[i].available &&
          identity(party.members[i].id) == identity(member.id)) {
        assert(index == -1);
        index = i;
      }
    assert(index >= 0 && shop.members[index] == member);
    independent |= member.id != selected_id;
  }
  assert(independent);
  // Offline mutation only: a new item selection must not reuse old native
  // member caches. The guest and capture bytes remain unchanged.
  memory.put(item_record + 8, item_id + 1);
  assert(!ot0::ReadShopComparison(read, main, party).valid);
  std::cout << "Native shop candidate and independent party comparisons "
               "replayed; stale candidate rejected\n";
}
} // namespace
int main() {
  LiveReplay();
  ot0::Snapshot party;
  party.valid = true;
  party.state = 25;
  party.members[3] = {901, 1, 1, 1, true};
  auto m = Fixture();
  const auto sample = [&](int64_t delta = 0) {
    return ot0::ReadShopComparison(
        [&](uint64_t p, void *out, size_t n) { return m.read(p, out, n); },
        Main, party, delta);
  };
  const auto expected = sample();
  assert(expected.valid && expected.candidate_item_id == 1234 &&
         expected.candidate_name == "Steel Sword" &&
         expected.selected_member_id == 904);
  assert(expected.members[3].valid && expected.members[3].stat_count == 2);
  assert(expected.members[3].stats[0].parameter_id == 9 &&
         expected.members[3].stats[0].label == "Phys. Atk." &&
         expected.members[3].stats[0].current == 25 &&
         expected.members[3].stats[0].candidate == 35);
  m = Fixture();
  m.array(Window + 0x468, Items, 2);
  m.put(Items + 0x40 + 8, uint32_t{5678});
  m.string(Items + 0x40 + 0x18, Items + 0x200, u"Steel Bow");
  m.put(Controller + 0x444, int32_t{1});
  m.put(Row + 0x450, uint32_t{5678});
  assert(sample().valid && sample().candidate_item_id == 5678 &&
         sample().candidate_name == "Steel Bow");
  m.put(Controller + 0x4b8, uint8_t{5});
  m.put(Controller + 0x43c, int32_t{});
  m.put(Row + 0x450, uint32_t{1234});
  assert(sample() == expected);
  m = Fixture();
  m.put(Manager + 0x330, Entries + 0x1000);
  m.put(Manager + 0x338, int32_t{4});
  m.put(Entries + 0x1000 + 3 * 4, int32_t{});
  assert(sample() == expected);
  for (int64_t delta : {int64_t{0x1ec0000000}, -int64_t{0x20000000}}) {
    m = Fixture(delta);
    assert(sample(delta) == expected);
    assert(!sample().valid);
  }
  // Native updates comparisons for active characters even when their
  // top-screen ancestor is hidden and child Slate wrappers were never built.
  m = Fixture();
  m.put(Row + 0xdc, uint8_t{2});
  m.put(Part + 0x108, uint64_t{});
  assert(sample() == expected);
  m.put(Part + 0xdc, uint8_t{2});
  assert(sample().members[3].stat_count == 1);
  m = Fixture();
  m.put(Controller + 0x9e0, int32_t{});
  m.put(Controller + 0x444, int32_t{1});
  assert(sample() == expected); // Native vertical list legitimately has width0.
  m = Fixture();
  m.put(Characters + 0x378, int32_t{-1});
  assert(sample().valid && sample().members[3].valid &&
         !sample().selected_member_id);
  m = Fixture();
  m.put(Row + 0x450, uint32_t{2222});
  assert(!sample().valid && !sample().members[3].valid);
  m = Fixture();
  m.put(Panel + 0xdc, uint8_t{2});
  assert(!sample().members[3].valid);
  assert(!sample().members[3].cannot_equip);
  m.put(Panel + 0x3000 + 0xdc, uint8_t{0});
  assert(sample().members[3].cannot_equip && !sample().members[3].valid &&
         sample().members[3].id == 904);
  m.put(Row + 0x450, uint32_t{2222});
  assert(!sample().members[3].cannot_equip);
  m = Fixture();
  m.put(Part + 0x3000 + 0xdc, uint8_t{2});
  assert(sample().members[3].stat_count == 1);
  m = Fixture();
  m.put(Manager + 0x328, int32_t{1});
  assert(!sample().valid);
  m = Fixture();
  m.put(Manager + 0x338, int32_t{3});
  assert(!sample().valid);
  m = Fixture();
  m.put(Entries, uint8_t{8});
  m.put(Entries + 0x10, int32_t{});
  assert(!sample().valid);
  m = Fixture();
  m.put(Shop + 0x350, uint8_t{});
  assert(!sample().valid);
  m = Fixture();
  m.put(Controller + 0x444, int32_t{1});
  assert(!sample().valid);
  m = Fixture();
  m.put(Part + 0x8000 + 0x800, char16_t{u'?'});
  assert(sample().members[3].stat_count == 1);
  m = Fixture();
  unsigned count{};
  const auto torn = ot0::ReadShopComparison(
      [&](uint64_t p, void *out, size_t n) {
        if (p == Items + 8 && ++count == 2)
          m.put(Items + 8, uint32_t{2222});
        return m.read(p, out, n);
      },
      Main, party);
  assert(!torn.valid);
  for (uint8_t state : {uint8_t{23}, uint8_t{24}, uint8_t{25}}) {
    m = Fixture();
    party.state = state;
    m.put(App + 0x2f0, state);
    assert(sample() == expected);
  }
  m = Fixture();
  party.state = 23;
  assert(!sample().valid);
  party.state = 26;
  m.put(App + 0x2f0, uint8_t{26});
  assert(!sample().valid);
  std::cout << "Native shop selection, comparison identity, visibility and "
               "relocation checks passed\n";
}
