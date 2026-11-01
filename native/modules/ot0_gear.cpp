// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_gear.h"
#include "ot0_memory.h"
#include <algorithm>
#include <cstring>

namespace ot0 {
namespace {
template <std::size_t N>
bool Items(const mem::Reader &r, uint64_t at, std::array<uint32_t, N> &out) {
  // The native getters grow these arrays lazily (an accessory array can hold 5 zero-padded
  // entries); the screen has N slots, the first N entries.
  mem::Array a{};
  if (!r.get(at, a) || a.count < 0 || a.count > 8 || a.capacity < a.count ||
      a.capacity > 16 || (a.count && !mem::Pointer(a.data, 4)))
    return false;
  out = {};
  const auto n = std::min<std::size_t>(std::size_t(a.count), N);
  return !n || r.raw(a.data, out.data(), n * 4);
}
} // namespace
Gear ReadGear(const ReadMemory &read, uint64_t main, int64_t delta, uint32_t member) {
  const mem::Reader r{read, main, delta};
  uint64_t manager{}, save{};
  mem::Array records{};
  if (!member || !r.ptr(main + 0x7cc8540, manager) || !r.ptr(manager + 0x2c8, save) ||
      !r.array(save + 0x158, records, 64))
    return {};
  // The protagonist (party ids 900..909) wears the current job's record: native
  // main+0x3a916e8 = 900 + job, job = save+0xa14 (1..9, else 1). Live 2026-10-31: party id 901,
  // job 4 -> record 904 = the native Equipment screen's Laurel Staff/Philosophy Tome/... set.
  uint32_t key_wanted = member;
  if (member >= 900 && member <= 909) {
    uint32_t job{};
    if (!r.get(save + 0xa14, job))
      return {};
    job &= 0xff;
    if (job < 1 || job > 9) job = 1;
    key_wanted = 900 + job;
  }
  Gear out;
  out.member = member;
  for (int e = 0; e < records.count; ++e) {
    const uint64_t rec = records.data + uint64_t(e) * 0x98;
    uint32_t key{};
    if (!r.get(rec, key))
      return {};
    // Exact record id (each protagonist job 900..909 keeps its own record).
    if (key != key_wanted)
      continue;
    if (!Items(r, rec + 0x08, out.weapons) || !Items(r, rec + 0x18, out.armor) ||
        !Items(r, rec + 0x28, out.accessories))
      return {};
    break; // a missing record means nothing equipped
  }
  out.valid = true;
  return out;
}
Inventory ReadInventory(const ReadMemory &read, uint64_t main, int64_t delta) {
  const mem::Reader r{read, main, delta};
  uint64_t manager{}, save{};
  mem::Array a{};
  if (!r.ptr(main + 0x7cc8540, manager) || !r.ptr(manager + 0x2c8, save) ||
      !r.array(save + 0x198, a, 4096) || (a.count && !mem::Pointer(a.data, 4)))
    return {};
  std::vector<uint32_t> raw(std::size_t(a.count) * 3);
  if (a.count && !r.raw(a.data, raw.data(), raw.size() * 4))
    return {};
  Inventory out;
  // FString {UTF-16 data, num incl. terminator, max}; optional (empty when unreadable).
  if (mem::Array s{}; r.get(save + 0xa00, s) && s.count > 1 && s.count <= 64 && s.capacity >= s.count &&
                      mem::Pointer(s.data, 2)) {
    std::vector<uint16_t> text(std::size_t(s.count));
    if (r.raw(s.data, text.data(), text.size() * 2) && text.back() == 0) {
      for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        uint32_t c = text[i];
        if (c >= 0xd800 && c <= 0xdfff) { out.favorite_dish.clear(); break; } // no surrogates
        if (c < 0x80) out.favorite_dish += char(c);
        else if (c < 0x800) { out.favorite_dish += char(0xc0 | c >> 6); out.favorite_dish += char(0x80 | (c & 63)); }
        else { out.favorite_dish += char(0xe0 | c >> 12); out.favorite_dish += char(0x80 | ((c >> 6) & 63)); out.favorite_dish += char(0x80 | (c & 63)); }
      }
    }
  }
  out.entries.reserve(std::size_t(a.count));
  for (int i = 0; i < a.count; ++i) {
    const auto count = static_cast<int32_t>(raw[std::size_t(i) * 3 + 1]);
    if (count < 0 || count > 9999)
      return {};
    out.entries.push_back({raw[std::size_t(i) * 3], count});
  }
  out.valid = true;
  return out;
}
QuestStates ReadQuestStates(const ReadMemory &read, uint64_t main, int64_t delta) {
  const mem::Reader r{read, main, delta};
  uint64_t manager{}, save{};
  mem::Array a{};
  if (!r.ptr(main + 0x7cc8540, manager) || !r.ptr(manager + 0x2c8, save) ||
      !r.array(save + 0x1d8, a, 1024) || (a.count && !mem::Pointer(a.data, 4)))
    return {};
  std::vector<uint32_t> raw(std::size_t(a.count) * 14);
  if (a.count && !r.raw(a.data, raw.data(), raw.size() * 4))
    return {};
  uint64_t map{}, elements{};
  int32_t count{};
  if (!r.ptr(main + 0x7cc8380, map) || !r.get(map + 0x320, elements) || !r.get(map + 0x328, count) ||
      count < 0 || count > 1024 || (count && !mem::Pointer(elements)))
    return {};
  std::vector<uint8_t> set(std::size_t(count) * 0x18);
  if (count && !r.raw(elements, set.data(), set.size()))
    return {};
  const auto pending = [&](uint32_t quest, bool &listed, uint32_t &destination) {
    for (int k = 0; k < count; ++k) {
      uint32_t id{};
      uint64_t detail{};
      std::memcpy(&id, set.data() + std::size_t(k) * 0x18, 4);
      std::memcpy(&detail, set.data() + std::size_t(k) * 0x18 + 8, 8);
      if (id != quest) continue;
      uint32_t condition{};
      if (!mem::Pointer(detail) || !r.get(detail + 0x3c, condition)) return false;
      listed = condition == 0;
      mem::Array tasks{};
      int32_t step{};
      destination = 0;
      if (listed && r.get(detail + 0x280, tasks) && r.get(detail + 0x2b0, step) && tasks.count > 0 &&
          tasks.count <= 64 && tasks.capacity >= tasks.count && mem::Pointer(tasks.data) && step >= 0 &&
          step < tasks.count && !r.get(tasks.data + uint64_t(step) * 0x138 + 0x24, destination))
        destination = 0;
      return true;
    }
    listed = false; // unknown to the native map: never listed
    return true;
  };
  QuestStates out;
  for (int i = 0; i < a.count; ++i) {
    const auto state = raw[std::size_t(i) * 14 + 1];
    if (state > 3)
      return {};
    QuestState q{raw[std::size_t(i) * 14], state, false, 0};
    if ((state == 1 || state == 2) && !pending(q.quest, q.listed, q.destination))
      return {};
    out.entries.push_back(q);
  }
  out.valid = true;
  return out;
}
} // namespace ot0
