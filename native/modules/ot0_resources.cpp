// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_memory.h"
#include "ot0_resources.h"
#include <algorithm>
namespace ot0 {
namespace {
using mem::Limit;
using mem::Pointer;
using Array = mem::Array;
struct Values {
  int32_t hp{}, sp{}, max_hp{}, max_sp{};
  bool operator==(const Values &) const = default;
};
struct Reader : mem::Reader {
  // Native3c3e66c stores KSTextBlock FText at1a0; dcf30 moves its numeric
  // FString into literal FTextData+28. Other/localized text types are refused.
  bool number(uint64_t widget, uint32_t &out) const {
    uint64_t data{}, data2{}, vt{};
    int32_t refs{};
    Array chars{}, again{};
    std::array<char16_t, 6> digits{}, checked{};
    if (!object(widget, 0x6367cc0) || !ptr(widget + 0x1a0, data) ||
        !get(data, vt) || !MatchesVtable(vt, main + 0x5b08170, delta) ||
        !get(data + 8, refs) || refs < 1 || refs > 1000000 ||
        !get(data + 0x28, chars) || !Pointer(chars.data) || chars.count < 2 ||
        chars.count > 6 || chars.capacity < chars.count ||
        chars.capacity > 32 ||
        !read(chars.data, digits.data(), size_t(chars.count) * 2) ||
        digits[chars.count - 1] != 0)
      return false;
    uint32_t value{};
    for (int i = 0; i < chars.count - 1; ++i) {
      if (digits[i] < u'0' || digits[i] > u'9')
        return false;
      value = value * 10 + uint32_t(digits[i] - u'0');
    }
    if (!get(widget + 0x1a0, data2) || data2 != data ||
        !get(data + 0x28, again) || again != chars ||
        !read(chars.data, checked.data(), size_t(chars.count) * 2) ||
        checked != digits || !object(widget, 0x6367cc0))
      return false;
    out = value;
    return true;
  }
  bool visible(uint64_t widget) const {
    uint64_t slate{}, controller{}, vt{}, self{};
    int32_t refs{};
    std::array<uint8_t, 2> visibility{};
    return ptr(widget + 0x108, slate) && ptr(widget + 0x110, controller) &&
           get(controller + 8, refs) && refs > 0 && refs <= 1000000 &&
           get(slate, vt) && MatchesVtable(vt, main + 0x5d011e8, delta) &&
           get(slate + 8, self) && self == slate &&
           get(slate + 0x184, visibility) && visibility[1] == 1 &&
           (visibility[0] == 1 || visibility[0] == 0x11 ||
            visibility[0] == 0x19);
  }
  // get/ptr/object are inherited unchanged from mem::Reader.
};
} // namespace

uint32_t ReadCommandActor(const ReadMemory &read, uint64_t main,
                          const BattleSnapshot &battle, int64_t delta) {
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8 || !battle.valid ||
      battle.member_count < 1 || battle.member_count > 8)
    return 0;
  Reader r{read, main, delta};
  struct Context {
    uint64_t app{}, bm{}, layout{}, control{}, command{}, main_list{},
        sub_list{}, active_list{}, actor{};
    uint32_t id{};
    uint8_t step{}, list_type{}, ready{};
    bool operator==(const Context &) const = default;
  };
  const auto sample = [&](Context &c) {
    uint8_t state{}, team{};
    uint32_t flags{};
    // m_CurrentActiveCommandList is a BattleCommandItemList; native3b870a0
    // binds its m_CommandCharacter+320 after resolving the swap partner.
    return r.ptr(main + 0x7cc8cc0, c.app) && r.object(c.app, 0x6207610) &&
           r.get(c.app + 0x2f0, state) && state == 26 &&
           r.ptr(main + 0x7cc5b78, c.bm) && r.get(c.bm + 8, flags) &&
           !(flags & 0x40000030) && r.get(c.bm + 0x163c, c.step) &&
           (c.step == 2 || c.step == 3) && r.ptr(c.bm + 0x1678, c.layout) &&
           r.object(c.layout, 0x6221a30) &&
           r.ptr(c.layout + 0x2f8, c.control) &&
           r.object(c.control, 0x62281d0) &&
           r.ptr(c.control + 0x300, c.command) &&
           r.object(c.command, 0x621cf90) && r.visible(c.command) &&
           r.get(c.command + 0x2f8, c.main_list) &&
           r.get(c.command + 0x300, c.sub_list) &&
           r.ptr(c.command + 0x3a0, c.active_list) &&
           (c.active_list == c.main_list || c.active_list == c.sub_list) &&
           r.object(c.active_list, 0x621b8e0) && r.visible(c.active_list) &&
           r.get(c.active_list + 0x328, c.list_type) && c.list_type >= 1 &&
           c.list_type <= 7 && r.get(c.active_list + 0x329, c.ready) &&
           c.ready && r.ptr(c.active_list + 0x320, c.actor) &&
           r.object(c.actor, 0x63f39b8) && r.get(c.actor + 0x293, team) &&
           team == 0 && r.get(c.actor + 0x298, c.id) && c.id;
  };
  Context first{}, again{};
  if (!sample(first))
    return 0;
  uint32_t id{};
  for (int i = 0; i < battle.member_count; ++i)
    if (mem::Alias(battle.members[i].id) == mem::Alias(first.id)) {
      if (id)
        return 0;
      id = battle.members[i].id;
    }
  if (!id || !sample(again) || again != first)
    return 0;
  return id;
}

BattleFormation ReadBattleFormation(const ReadMemory &read, uint64_t main,
                                    const BattleSnapshot &battle,
                                    int64_t delta) {
  BattleFormation out;
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8 || !battle.valid ||
      battle.member_count < 1 || battle.member_count > 8)
    return out;
  for (int i = 0; i < battle.member_count; ++i) {
    if (!battle.members[i].id)
      return out;
    for (int j = 0; j < i; ++j)
      if (mem::Alias(battle.members[i].id) == mem::Alias(battle.members[j].id))
        return out;
  }
  Reader r{read, main, delta};
  uint64_t app{}, bm{}, layout{}, control{};
  uint8_t state{};
  uint32_t flags{};
  Array rows{};
  if (!r.ptr(main + 0x7cc8cc0, app) || !r.object(app, 0x6207610) ||
      !r.get(app + 0x2f0, state) || state != 26 ||
      !r.ptr(main + 0x7cc5b78, bm) || !r.get(bm + 8, flags) ||
      (flags & 0x40000030) || !r.ptr(bm + 0x1678, layout) ||
      !r.object(layout, 0x6221a30) || !r.ptr(layout + 0x2f8, control) ||
      !r.object(control, 0x62281d0) || !r.get(control + 0x328, rows) ||
      rows.count < 1 || rows.count > 32 || rows.capacity < rows.count ||
      rows.capacity > 32 || !Pointer(rows.data))
    return out;
  struct Binding {
    uint64_t row{}, part{}, actor{};
    uint32_t id{};
    uint8_t visibility{};
  };
  std::array<Binding, 8> bindings{};
  std::array<bool, 8> seen{};
  // Same native first-four pair rows as resource widgets. These actor links
  // are rebound by auto-promotion after a KO; saved formation is not a role.
  for (int row_index = 0; row_index < std::min(rows.count, 4); ++row_index) {
    uint64_t row{};
    if (!r.get(rows.data + uint64_t(row_index) * 8, row) ||
        (row && !r.object(row, 0x6232f28)))
      return {};
    for (int side = 0; side < 2; ++side) {
      const int slot = row_index + side * 4;
      auto &b = bindings[slot];
      b.row = row;
      if (!row)
        continue;
      const uint64_t offset = side == 0 ? 0x370 : 0x378;
      const uint64_t vt = side == 0 ? 0x63222a8 : 0x6232f28;
      uint64_t parent{};
      uint8_t team{};
      if (!r.get(row + offset, b.part))
        return {};
      if (!b.part)
        continue;
      if (!r.object(b.part, vt) || !r.get(b.part + 0x368, parent) ||
          parent != row || !r.get(b.part + 0x2f8, b.actor) ||
          !r.get(b.part + 0xdc, b.visibility))
        return {};
      if (!b.actor)
        continue;
      // A collapsed/hidden part may retain a previous actor. It does not
      // prove a current role even when that actor still belongs to the party.
      if ((b.visibility != 0 && b.visibility != 3 && b.visibility != 4) ||
          !r.object(b.actor, 0x63f39b8) || !r.get(b.actor + 0x293, team) ||
          team != 0 || !r.get(b.actor + 0x298, b.id) || !b.id)
        return {};
      int index = -1;
      for (int i = 0; i < battle.member_count; ++i)
        if (mem::Alias(battle.members[i].id) == mem::Alias(b.id)) {
          index = i;
          break;
        }
      if (index < 0 || seen[index])
        return {};
      seen[index] = true;
      out.ids[slot] = battle.members[index].id;
    }
  }
  for (int i = 0; i < battle.member_count; ++i)
    if (!seen[i])
      return {};
  for (int row_index = 0; row_index < std::min(rows.count, 4); ++row_index) {
    uint64_t row{};
    if (!r.get(rows.data + uint64_t(row_index) * 8, row) ||
        row != bindings[row_index].row || (row && !r.object(row, 0x6232f28)))
      return {};
    for (int side = 0; side < 2; ++side) {
      const auto &b = bindings[row_index + side * 4];
      if (!row)
        continue;
      uint64_t part{}, actor{}, parent{};
      uint32_t id{};
      uint8_t visibility{}, team{};
      const uint64_t offset = side == 0 ? 0x370 : 0x378;
      const uint64_t vt = side == 0 ? 0x63222a8 : 0x6232f28;
      if (!r.get(row + offset, part) || part != b.part)
        return {};
      if (!part)
        continue;
      if (!r.object(part, vt) || !r.get(part + 0x368, parent) ||
          parent != row || !r.get(part + 0x2f8, actor) || actor != b.actor ||
          !r.get(part + 0xdc, visibility) || visibility != b.visibility)
        return {};
      if (actor &&
          (!r.object(actor, 0x63f39b8) || !r.get(actor + 0x293, team) ||
           team != 0 || !r.get(actor + 0x298, id) || id != b.id))
        return {};
    }
  }
  uint64_t checked{};
  Array checked_rows{};
  if (!r.get(main + 0x7cc8cc0, checked) || checked != app ||
      !r.object(app, 0x6207610) || !r.get(app + 0x2f0, state) || state != 26 ||
      !r.get(main + 0x7cc5b78, checked) || checked != bm ||
      !r.get(bm + 8, flags) || (flags & 0x40000030) ||
      !r.get(bm + 0x1678, checked) || checked != layout ||
      !r.get(layout + 0x2f8, checked) || checked != control ||
      !r.object(layout, 0x6221a30) || !r.object(control, 0x62281d0) ||
      !r.get(control + 0x328, checked_rows) || checked_rows != rows)
    return {};
  out.valid = true;
  return out;
}

std::array<ResourceLimits, 8>
ReadBattleResourceLimits(const ReadMemory &read, uint64_t main,
                         const BattleSnapshot &battle, int64_t delta) {
  std::array<ResourceLimits, 8> out{};
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8 || !battle.valid ||
      battle.member_count < 1 || battle.member_count > 8)
    return out;
  Reader r{read, main, delta};
  uint64_t app{}, bm{}, layout{}, control{};
  uint8_t state{};
  uint32_t flags{};
  Array rows{};
  if (!r.ptr(main + 0x7cc8cc0, app) || !r.object(app, 0x6207610) ||
      !r.get(app + 0x2f0, state) || state != 26 ||
      !r.ptr(main + 0x7cc5b78, bm) || !r.get(bm + 8, flags) ||
      (flags & 0x40000030) || !r.ptr(bm + 0x1678, layout) ||
      !r.object(layout, 0x6221a30) || !r.ptr(layout + 0x2f8, control) ||
      !r.object(control, 0x62281d0) || !r.get(control + 0x328, rows) ||
      rows.count < 1 || rows.count > 32 || rows.capacity < rows.count ||
      rows.capacity > 32 || !Pointer(rows.data))
    return out;
  std::array<bool, 8> seen{};
  // The native control's first four rows are the four front/back party pairs.
  // Additional rows are collapsed native templates/variants, not party slots.
  for (int row_index = 0; row_index < std::min(rows.count, 4); ++row_index) {
    uint64_t row{};
    if (!r.ptr(rows.data + uint64_t(row_index) * 8, row) ||
        !r.object(row, 0x6232f28))
      continue;
    for (uint64_t offset : {uint64_t{0x370}, uint64_t{0x378}}) {
      uint64_t part{}, actor{}, parent{}, actor2{}, parent2{}, row2{}, part2{};
      uint32_t id{}, id2{};
      uint8_t team{};
      Values values{}, checked{};
      const uint64_t part_vtable = offset == 0x370 ? 0x63222a8 : 0x6232f28;
      if (!r.ptr(row + offset, part) || !r.object(part, part_vtable) ||
          !r.ptr(part + 0x368, parent) || parent != row ||
          !r.ptr(part + 0x2f8, actor) || !r.object(actor, 0x63f39b8) ||
          !r.get(actor + 0x293, team) || team != 0 ||
          !r.get(actor + 0x298, id) || !id || !r.get(part + 0x400, values))
        continue;
      int index = -1;
      for (int i = 0; i < battle.member_count; ++i)
        if (mem::Alias(battle.members[i].id) == mem::Alias(id)) {
          if (index != -1) {
            index = -1;
            break;
          }
          index = i;
        }
      if (index < 0)
        continue;
      const auto &member = battle.members[index];
      if (values.hp != member.hp || values.sp != member.sp ||
          values.max_hp < 1 || values.max_hp > 99999 || values.max_sp < 1 ||
          values.max_sp > 9999 || values.hp < 0 || values.hp > values.max_hp ||
          values.sp < 0 || values.sp > values.max_sp)
        continue;
      if (!r.get(part + 0x400, checked) || checked != values ||
          !r.get(part + 0x2f8, actor2) || actor2 != actor ||
          !r.get(part + 0x368, parent2) || parent2 != parent ||
          !r.get(actor + 0x298, id2) || id2 != id ||
          !r.get(row + offset, part2) || part2 != part ||
          !r.get(rows.data + uint64_t(row_index) * 8, row2) || row2 != row ||
          !r.object(row, 0x6232f28) || !r.object(part, part_vtable) ||
          !r.object(actor, 0x63f39b8))
        continue;
      // Duplicate live bindings are ambiguous during a row rebuild.
      if (seen[index])
        out[index] = {};
      else {
        seen[index] = true;
        out[index] = {uint32_t(values.max_hp), uint32_t(values.max_sp), true};
      }
    }
  }
  uint64_t check{};
  Array checked_rows{};
  if (!r.get(main + 0x7cc8cc0, check) || check != app ||
      !r.object(app, 0x6207610) || !r.get(app + 0x2f0, state) || state != 26 ||
      !r.get(main + 0x7cc5b78, check) || check != bm ||
      !r.get(bm + 0x1678, check) || check != layout ||
      !r.get(layout + 0x2f8, check) || check != control ||
      !r.object(layout, 0x6221a30) || !r.object(control, 0x62281d0) ||
      !r.get(control + 0x328, checked_rows) || checked_rows != rows)
    return {};
  return out;
}

std::array<ResourceLimits, 8> ReadMenuResourceLimits(const ReadMemory &read,
                                                     uint64_t main,
                                                     const Snapshot &party,
                                                     int64_t delta) {
  std::array<ResourceLimits, 8> out{};
  if (!read || !Pointer(main) || main > Limit - 0x7cc9a20 || !party.valid ||
      party.state != 25)
    return out;
  Reader r{read, main, delta};
  uint64_t app{}, manager{}, menu{}, panel{}, list{};
  uint8_t state{}, current{};
  Array rows{};
  // Native3c0ae80 binds ID4f0 and calls3cc30a0 with the four resource
  // text widgets. Only the currently visible normal MainMenu is eligible.
  const auto roots = [&] {
    return r.ptr(main + 0x7cc8cc0, app) && r.object(app, 0x6207610) &&
           r.get(app + 0x2f0, state) && state == 25 &&
           r.ptr(main + 0x7cc9a18, manager) && r.object(manager, 0x63b6f18) &&
           r.get(manager + 0x38a, current) && current == 0x14 &&
           r.ptr(manager + 0x408, menu) && r.object(menu, 0x6393350) &&
           r.visible(menu) && r.ptr(menu + 0x4a8, panel) &&
           r.object(panel, 0x63905b8) && r.visible(panel) &&
           r.ptr(panel + 0x338, list) && r.object(list, 0x646d038) &&
           r.visible(list) && r.get(list + 0x638, rows) && Pointer(rows.data) &&
           rows.count == 8 && rows.capacity >= 8 && rows.capacity <= 32;
  };
  if (!roots())
    return out;
  const auto bindings =
      std::array<uint64_t, 5>{app, manager, menu, panel, list};
  const auto original_rows = rows;
  std::array<bool, 8> seen{};
  for (int row_index = 0; row_index < rows.count; ++row_index) {
    uint64_t row{}, row2{};
    uint32_t id{}, id2{};
    std::array<uint64_t, 4> texts{}, checked_texts{};
    std::array<uint32_t, 4> numbers{}, checked_numbers{};
    if (!r.ptr(rows.data + uint64_t(row_index) * 8, row) ||
        !r.object(row, 0x638faa0) || !r.get(row + 0x4f0, id) || !id)
      continue;
    int index = -1;
    for (int i = 0; i < 8; ++i)
      if (party.members[i].available &&
          mem::Alias(party.members[i].id) == mem::Alias(id)) {
        if (index != -1) {
          index = -1;
          break;
        }
        index = i;
      }
    if (index < 0)
      continue;
    bool valid = true;
    for (int i = 0; i < 4; ++i)
      valid = valid && r.ptr(row + 0x460 + uint64_t(i) * 8, texts[i]) &&
              r.number(texts[i], numbers[i]);
    const auto &member = party.members[index];
    if (!valid || numbers[0] != member.hp || numbers[2] != member.sp ||
        numbers[1] < 1 || numbers[1] > 99999 || numbers[3] < 1 ||
        numbers[3] > 9999 || numbers[0] > numbers[1] || numbers[2] > numbers[3])
      continue;
    for (int i = 0; i < 4; ++i)
      valid = valid && r.get(row + 0x460 + uint64_t(i) * 8, checked_texts[i]) &&
              checked_texts[i] == texts[i] &&
              r.number(texts[i], checked_numbers[i]);
    if (!valid || checked_numbers != numbers || !r.get(row + 0x4f0, id2) ||
        id2 != id || !r.object(row, 0x638faa0) ||
        !r.get(rows.data + uint64_t(row_index) * 8, row2) || row2 != row)
      continue;
    if (seen[index])
      out[index] = {};
    else {
      seen[index] = true;
      out[index] = {numbers[1], numbers[3], true};
    }
  }
  if (!roots() || rows != original_rows ||
      std::array<uint64_t, 5>{app, manager, menu, panel, list} != bindings)
    return {};
  return out;
}
} // namespace ot0
