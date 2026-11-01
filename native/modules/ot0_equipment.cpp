// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_equipment.h"
#include "ot0_memory.h"
#include <algorithm>
namespace ot0 {
namespace {
using mem::Limit;
using mem::Pointer;
using Array = mem::Array;
struct Reader : mem::Reader {
  // This file's array() requirements differ from the generic default: an
  // empty array is never acceptable here (min_count = 1), and the capacity
  // cap is the same `maximum` as the count cap, not the generic 4096.
  // data_alignment stays 8, same as the generic default's non-array
  // pointer checks.
  bool array(uint64_t p, Array &out, int maximum) const {
    return mem::Reader::array(p, out, maximum, maximum, 8, 1);
  }
  bool visible(uint64_t p) const {
    uint64_t slate{}, controller{}, vt{}, self{};
    int32_t refs{};
    uint32_t flags{};
    std::array<uint8_t, 2> visibility{};
    return get(p + 8, flags) && !(flags & 0x40000030) &&
           ptr(p + 0x108, slate) && ptr(p + 0x110, controller) &&
           get(controller + 8, refs) && refs > 0 && refs <= 1000000 &&
           get(slate, vt) && MatchesVtable(vt, main + 0x5d011e8, delta) &&
           get(slate + 8, self) && self == slate &&
           get(slate + 0x184, visibility) && visibility[1] == 1 &&
           (visibility[0] == 1 || visibility[0] == 0x11 ||
            visibility[0] == 0x19);
  }
  bool panel_visible(uint64_t p) const {
    // Native UWidget visibility setter1f135f0 stores ESlateVisibility atdc.
    // Comparison containers are UPanelWidgets, not SObjectWidget wrappers;
    // their native setters explicitly choose Visible0 or Hidden2.
    uint32_t flags{};
    uint8_t visibility{};
    return Pointer(p) && get(p + 8, flags) && !(flags & 0x40000030) &&
           get(p + 0xdc, visibility) &&
           (visibility == 0 || visibility == 3 || visibility == 4);
  }
  bool string(uint64_t p, std::string &out) const {
    Array chars{}, checked{};
    std::array<char16_t, 128> text{}, again{};
    if (!array(p, chars, 128) || chars.count < 2 ||
        chars.data > Limit - uint64_t(chars.count) * 2 ||
        !read(chars.data, text.data(), size_t(chars.count) * 2) ||
        text[chars.count - 1] != 0)
      return false;
    std::string value;
    for (int i = 0; i < chars.count - 1; ++i) {
      uint32_t c = text[i];
      if (c < 0x20 || (c >= 0xdc00 && c <= 0xdfff))
        return false;
      if (c >= 0xd800 && c <= 0xdbff) {
        if (++i >= chars.count - 1 || text[i] < 0xdc00 || text[i] > 0xdfff)
          return false;
        c = 0x10000 + ((c - 0xd800) << 10) + (text[i] - 0xdc00);
      }
      if (c < 0x80)
        value += char(c);
      else if (c < 0x800) {
        value += char(0xc0 | (c >> 6));
        value += char(0x80 | (c & 63));
      } else if (c < 0x10000) {
        value += char(0xe0 | (c >> 12));
        value += char(0x80 | ((c >> 6) & 63));
        value += char(0x80 | (c & 63));
      } else {
        value += char(0xf0 | (c >> 18));
        value += char(0x80 | ((c >> 12) & 63));
        value += char(0x80 | ((c >> 6) & 63));
        value += char(0x80 | (c & 63));
      }
    }
    if (!get(p, checked) || checked != chars ||
        !read(chars.data, again.data(), size_t(chars.count) * 2) ||
        again != text)
      return false;
    out = std::move(value);
    return true;
  }
  bool text(uint64_t widget, std::string &out) const {
    uint64_t data{}, checked{}, vt{};
    int32_t refs{};
    // Native e1e60 copies FString into literal FTextData+28, same as dcf30.
    return object(widget, 0x6367cc0) && ptr(widget + 0x1a0, data) &&
           get(data, vt) && MatchesVtable(vt, main + 0x5b08170, delta) &&
           get(data + 8, refs) && refs > 0 && refs <= 1000000 &&
           string(data + 0x28, out) && get(widget + 0x1a0, checked) &&
           checked == data && object(widget, 0x6367cc0);
  }
  bool number(uint64_t widget, int32_t &out) const {
    std::string s;
    if (!text(widget, s) || s.empty() || s.size() > 6)
      return false;
    const bool negative = s.front() == '-';
    int32_t value{};
    if (negative && s.size() == 1)
      return false;
    for (size_t i = negative ? 1 : 0; i < s.size(); ++i) {
      if (s[i] < '0' || s[i] > '9')
        return false;
      value = value * 10 + s[i] - '0';
    }
    if (value > 99999)
      return false;
    out = negative ? -value : value;
    return true;
  }
  bool active_shop(uint64_t manager, uint64_t &shop) const {
    uint64_t entries{}, buckets{};
    int32_t maximum{}, free{}, hash_count{}, index{};
    uint8_t key{};
    // Native MenuUIManager::GetList3c1d7f0: bounded hash lookup, no scan.
    if (!object(manager, 0x63b6f18) || !get(manager + 0x38a, key) ||
        !ptr(manager + 0x2f0, entries) || !get(manager + 0x2f8, maximum) ||
        maximum < 1 || maximum > 256 || !get(manager + 0x324, free) ||
        free < 0 || free >= maximum || !get(manager + 0x330, buckets) ||
        !get(manager + 0x338, hash_count) || hash_count < 1 ||
        hash_count > 256 || (hash_count & (hash_count - 1)) ||
        (buckets ? !Pointer(buckets) : hash_count != 1))
      return false;
    if (!buckets)
      buckets = manager + 0x328;
    if (!get(buckets + uint64_t(key & (hash_count - 1)) * 4, index))
      return false;
    for (int count = 0; count < maximum && index != -1; ++count) {
      if (index < 0 || index >= maximum)
        return false;
      const uint64_t entry = entries + uint64_t(index) * 0x18;
      uint8_t entry_key{};
      if (!get(entry, entry_key))
        return false;
      if (entry_key == key) {
        uint8_t running{};
        return ptr(entry + 8, shop) && object(shop, 0x64680c8) &&
               get(shop + 0x350, running) && running != 0 && visible(shop);
      }
      if (!get(entry + 0x10, index))
        return false;
    }
    return false;
  }
};
struct Selection {
  uint64_t app{}, manager{}, shop{}, window{}, controller{}, characters{};
  Array items{}, rows{};
  int32_t selected{}, selected_character{};
  uint32_t item_id{};
  uint8_t state{};
  std::string name;
  bool operator==(const Selection &) const = default;
};
bool ReadSelection(const Reader &r, Selection &s) {
  uint8_t mode{};
  int32_t local{}, page{}, width{};
  if (!r.ptr(r.main + 0x7cc8cc0, s.app) || !r.object(s.app, 0x6207610) ||
      !r.get(s.app + 0x2f0, s.state) || s.state < 23 || s.state > 25 ||
      !r.ptr(r.main + 0x7cc9a18, s.manager) ||
      !r.active_shop(s.manager, s.shop) || !r.ptr(s.shop + 0x398, s.window) ||
      !r.object(s.window, 0x6339278) || !r.visible(s.window) ||
      !r.ptr(s.window + 0x3b0, s.controller) ||
      !r.object(s.controller, 0x646e168) ||
      !r.get(s.controller + 0x4b8, mode) ||
      !r.get(s.controller + 0x378, local) ||
      !r.get(s.controller + (mode == 5 ? 0x43c : 0x444), page) ||
      !r.get(s.controller + 0x9e0, width) || local < 0 || local > 256 ||
      page < 0 || page > 256 || width < 0 || width > 256 ||
      !r.array(s.window + 0x468, s.items, 256))
    return false;
  s.selected = local + page * width;
  if (s.selected >= s.items.count)
    return false;
  const uint64_t item = s.items.data + uint64_t(s.selected) * 0x40;
  return r.get(item + 8, s.item_id) && s.item_id > 0 &&
         r.string(item + 0x18, s.name) &&
         r.ptr(s.window + 0x3d8, s.characters) &&
         r.object(s.characters, 0x63a49e8) &&
         r.array(s.characters + 0x638, s.rows, 64) &&
         r.get(s.characters + 0x378, s.selected_character) &&
         s.selected_character >= -1 && s.selected_character < s.rows.count;
}
} // namespace
ShopComparisonSnapshot ReadShopComparison(const ReadMemory &read, uint64_t main,
                                          const Snapshot &party,
                                          int64_t delta) {
  ShopComparisonSnapshot out;
  if (!read || !Pointer(main) || main > Limit - 0x7cc9a20 || !party.valid ||
      (party.state < 23 || party.state > 25))
    return out;
  Reader r{read, main, delta};
  Selection s{};
  if (!ReadSelection(r, s) || s.state != party.state)
    return out;
  out.candidate_item_id = s.item_id;
  out.candidate_name = s.name;
  std::array<bool, 8> seen{};
  for (int row_index = 0; row_index < s.rows.count; ++row_index) {
    uint64_t row{}, panel{}, checked{};
    uint32_t id{}, candidate{};
    uint8_t active{};
    int32_t page{};
    Array parts{}, parameters{};
    if (!r.ptr(s.rows.data + uint64_t(row_index) * 8, row) ||
        !r.object(row, 0x63a6e10) || !r.get(row + 0x3e5, active) || !active ||
        !r.get(row + 0x3f8, id) || !id)
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
    if (row_index == s.selected_character)
      out.selected_member_id = id;
    if (!r.get(row + 0x450, candidate) || candidate != s.item_id ||
        !r.ptr(row + 0x4f0, panel))
      continue;
    uint8_t panel_visibility{};
    uint64_t cannot{}, checked_marker{}, checked_panel{}, checked_row{};
    uint32_t checked_id{}, checked_candidate{};
    // Native3c8f4e0's cannot-equip branch explicitly shows4e8 and hides4f0.
    // A hidden status panel alone does not establish equipment eligibility.
    if (r.get(panel + 0xdc, panel_visibility) && panel_visibility == 2 &&
        r.ptr(row + 0x4e8, cannot) && r.panel_visible(cannot)) {
      uint8_t checked_visibility{};
      if (!r.get(row + 0x3f8, checked_id) || checked_id != id ||
          !r.get(row + 0x450, checked_candidate) ||
          checked_candidate != candidate ||
          !r.get(row + 0x4e8, checked_marker) || checked_marker != cannot ||
          !r.get(row + 0x4f0, checked_panel) || checked_panel != panel ||
          !r.get(panel + 0xdc, checked_visibility) || checked_visibility != 2 ||
          !r.panel_visible(cannot) || !r.object(row, 0x63a6e10) ||
          !r.get(s.rows.data + uint64_t(row_index) * 8, checked_row) ||
          checked_row != row)
        continue;
      EquipmentMemberComparison member;
      member.id = id;
      member.cannot_equip = true;
      if (seen[index])
        out.members[index] = {};
      else {
        seen[index] = true;
        out.members[index] = std::move(member);
      }
      continue;
    }
    if (!r.panel_visible(panel) || !r.get(row + 0x454, page) || page < 0 ||
        page > 4 || !r.array(row + 0x468, parameters, 32) ||
        !r.array(row + 0x4f8, parts, 8) || parts.count < 4 ||
        page * 4 >= parameters.count)
      continue;
    EquipmentMemberComparison member;
    member.id = id;
    for (int i = 0; i < 4 && page * 4 + i < parameters.count; ++i) {
      uint32_t parameter{};
      uint64_t part{}, title{}, current{}, proposed{}, comparison_panel{};
      EquipmentStatComparison stat;
      if (!r.get(parameters.data + uint64_t(page * 4 + i) * 4, parameter) ||
          !parameter || parameter > 16 ||
          !r.ptr(parts.data + uint64_t(i) * 8, part) ||
          !r.object(part, 0x63a77a8) || !r.panel_visible(part) ||
          !r.ptr(part + 0x348, comparison_panel) ||
          !r.panel_visible(comparison_panel) || !r.ptr(part + 0x328, title) ||
          !r.text(title, stat.label) || !r.ptr(part + 0x330, current) ||
          !r.number(current, stat.current) || !r.ptr(part + 0x340, proposed) ||
          !r.number(proposed, stat.candidate))
        continue;
      EquipmentStatComparison again;
      uint32_t checked_parameter{};
      uint64_t checked_part{};
      if (!r.text(title, again.label) || again.label != stat.label ||
          !r.number(current, again.current) || again.current != stat.current ||
          !r.number(proposed, again.candidate) ||
          again.candidate != stat.candidate ||
          !r.get(parameters.data + uint64_t(page * 4 + i) * 4,
                 checked_parameter) ||
          checked_parameter != parameter ||
          !r.get(parts.data + uint64_t(i) * 8, checked_part) ||
          checked_part != part || !r.panel_visible(part) ||
          !r.panel_visible(comparison_panel))
        continue;
      stat.parameter_id = parameter;
      member.stats[member.stat_count++] = std::move(stat);
    }
    int32_t checked_page{};
    Array checked_parts{}, checked_parameters{};
    if (!member.stat_count || !r.get(row + 0x3f8, checked_id) ||
        checked_id != id || !r.get(row + 0x450, checked_candidate) ||
        checked_candidate != candidate || !r.get(row + 0x454, checked_page) ||
        checked_page != page || !r.get(row + 0x4f8, checked_parts) ||
        checked_parts != parts || !r.get(row + 0x468, checked_parameters) ||
        checked_parameters != parameters ||
        !r.get(s.rows.data + uint64_t(row_index) * 8, checked) ||
        checked != row || !r.object(row, 0x63a6e10) || !r.panel_visible(panel))
      continue;
    member.valid = true;
    if (seen[index])
      out.members[index] = {};
    else {
      seen[index] = true;
      out.members[index] = std::move(member);
    }
  }
  Selection again{};
  if (!ReadSelection(r, again) || again != s)
    return {};
  out.valid = std::any_of(out.members.begin(), out.members.end(),
                          [](const auto &member) { return member.valid; });
  return out;
}
} // namespace ot0
