// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Original immutable text guards plus synthetic mutable UI payload adversaries.
#include "echoes_menu.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
namespace {
constexpr uint64_t Base = 0x80000000, Head = Base + 0x34bf070;
constexpr uint64_t Node1 = 0x90000000, Node2 = Node1 + 0x100, Main = 0x90010020,
                   Menu = 0x90020020;
constexpr uint64_t Records = 0x90040000, Rows = 0x90050000;
std::vector<uint8_t> text, rodata;
size_t cases{};
void Check(bool ok, const char *what) {
  ++cases;
  if (!ok) {
    std::cerr << "FAIL " << cases << ": " << what << '\n';
    std::abort();
  }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t, size_t)> before;
  size_t reads{}, largest{};
  void Put(uint64_t at, uint64_t value, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[at + i] = uint8_t(value >> (i * 8));
  }
  void Zero(uint64_t at, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[at + i] = 0;
  }
  void State(uint64_t at, uint8_t state) {
    Put(at, state, 1);
    Put(at + 1, 0, 1);
    Put(at + 2, 0xfe, 1);
  }
  Memory(bool items = false) {
    Put(Head, Node2, 8);
    Put(Head + 8, Node1, 8);
    Put(Head + 16, 2, 8);
    Put(Node1, Head, 8);
    Put(Node1 + 8, Node2, 8);
    Put(Node1 + 16, Main, 8);
    Put(Node2, Node1, 8);
    Put(Node2 + 8, Head, 8);
    Put(Node2 + 16, Menu, 8);
    Put(Base + 0x3412b80 + 0x70, Base + 0x24eda50, 8);
    Put(Main, Base + 0x3412b80, 8);
    Put(Main - 0x20, Base + 0x3426450, 8);
    State(Main + 0x88, 2);
    Put(Menu, Base + (items ? 0x3415588 : 0x3413d58), 8);
    Put(Menu - 0x20, Base + (items ? 0x3427310 : 0x3428190), 8);
    State(Menu + 0x88, 2);
    State(Menu + 0x208, 0);
    if (items) {
      Put(Menu + 0x53a0, Records, 8);
      Put(Menu + 0x53a8, Records + 3 * 0x78, 8);
      Put(Menu + 0x53b0, Records + 3 * 0x78, 8);
      Put(Menu + 0x5390, 1, 4);
      Zero(Records, 3 * 0x78);
      for (size_t i = 0; i < 3; ++i) {
        Put(Records + i * 0x78 + 0x20, 212 + i, 4);
        Put(Records + i * 0x78 + 0x24, i + 1, 4);
      }
      Put(Records + 0x78 + 0x28, 1, 1);
      Put(Records + 0x78 + 0x2a, 1, 1);
    } else {
      Put(Menu + 0xa578, Records, 8);
      Put(Menu + 0xa580, Records + 8 * 0x28, 8);
      Put(Menu + 0xa588, Records + 8 * 0x28, 8);
      Put(Menu + 0xa5a8, Rows, 8);
      Put(Menu + 0xa5b0, Rows + 8, 8);
      Put(Menu + 0xa5b8, Rows + 8, 8);
      Put(Rows, 0, 4);
      Put(Rows + 4, 1, 4);
      Put(Menu + 0xa5c8, 0, 4);
      Put(Menu + 0xa5cc, 1, 4);
      Put(Menu + 0xa5d0, 1, 4);
      Put(Menu + 0x2f0, 5, 4);
      Zero(Records, 8 * 0x28);
      Put(Records + 0x1e, 1, 1);
      Put(Records + 4, 8, 4);
      Put(Records + 0x1f, 1, 1);
      Put(Records + 5 * 0x28, 32, 2);
      Put(Records + 5 * 0x28 + 0x1c, 1, 1);
      Put(Records + 5 * 0x28 + 0x1e, 1, 1);
    }
  }
  bool Read(uint64_t at, void *out, size_t n) {
    ++reads;
    largest = std::max(largest, n);
    const auto visit = ++visits[at];
    if (before)
      before(at, n, visit);
    if (n > UINT64_MAX - at)
      return false;
    auto *p = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      auto found = bytes.find(at + i);
      if (found != bytes.end())
        p[i] = found->second;
      else if (at + i >= Base && at + i - Base < text.size())
        p[i] = text[size_t(at + i - Base)];
      else if (at + i >= Base + 0x2837000 &&
               at + i - Base - 0x2837000 < rodata.size())
        p[i] = rodata[size_t(at + i - Base - 0x2837000)];
      else
        return false;
    }
    return true;
  }
  echoes::MenuSnapshot Sample(bool match = true) {
    return echoes::ReadMenu(
        [&](auto a, auto p, auto n) { return Read(a, p, n); }, Base, match);
  }
};
} // namespace
int main() {
  const char *root = std::getenv("ECHOES_SEGMENTS");
  if (!root) {
    std::cout
        << "Native immutable text fixture unavailable (ECHOES_SEGMENTS)\n";
    return 77;
  }
  std::ifstream file(std::filesystem::path(root) / "text.bin",
                     std::ios::binary);
  text = {std::istreambuf_iterator<char>(file), {}};
  std::ifstream datafile(std::filesystem::path(root) / "rodata.bin",
                         std::ios::binary);
  rodata = {std::istreambuf_iterator<char>(datafile), {}};
  Check(text.size() > 0x2669000, "original text fixture mapped");
  {
    Memory m;
    const auto s = m.Sample();
    Check(s.valid && s.kind == echoes::MenuKind::Equipment &&
              s.component == Menu && s.row == 1 && s.column == 1 &&
              s.selected_index == 5 && s.visible_cursor == 5,
          "DressUp row mapping preserves native selected index");
    Check(s.entries.size() == 8 &&
              s.entries[0].kind == echoes::MenuEntryKind::Accessory &&
              s.entries[0].mask == 8 && s.entries[0].equipped &&
              s.entries[5].kind == echoes::MenuEntryKind::Outfit &&
              s.entries[5].mask == 32 && !s.entries[5].equipped,
          "native gear union distinguishes accessory and outfit masks");
  }
  {
    Memory m(true);
    const auto s = m.Sample();
    Check(
        s.valid && s.kind == echoes::MenuKind::Items && s.selected_index == 1 &&
            s.entries[1].item_id == 213 && s.entries[1].quantity == 2 &&
            s.entries[1].owned && s.entries[1].raw_action2a == 1,
        "ItemMenu descriptor exposes identity/quantity but no consume action");
  }
  {
    Memory m(true);
    constexpr uint32_t quest_ids[] = {157, 186, 187, 189, 190,
                                      208, 209, 210, 229};
    m.Put(Menu + 0x53a8, Records + 26 * 0x78, 8);
    m.Put(Menu + 0x53b0, Records + 26 * 0x78, 8);
    m.Zero(Records + 3 * 0x78, 23 * 0x78);
    for (size_t i = 0; i < 9; ++i) {
      const auto at = Records + (17 + i) * 0x78;
      m.Put(at + 0x20, quest_ids[i], 4);
      m.Put(at + 0x24, UINT32_MAX, 4);
      m.Put(at + 0x28, 1, 1);
    }
    const auto s = m.Sample();
    Check(s.valid && s.entries.size() == 26 &&
              s.entries[1].quantity == 2 && s.entries[1].owned &&
              s.entries[17].quantity == UINT32_MAX &&
              !s.entries[17].owned && !s.entries[17].raw_action2a,
          "native quest sentinel does not withdraw counted ingredient menu");
    m.Put(Records + 17 * 0x78 + 0x2a, 1, 1);
    Check(!m.Sample().valid,
          "uncounted sentinel with consume flag refuses complete menu");
    m.Put(Records + 17 * 0x78 + 0x2a, 0, 1);
    m.Put(Records + 0x24, UINT32_MAX, 4);
    m.Put(Records + 0x28, 1, 1);
    Check(!m.Sample().valid,
          "ingredient cells cannot acquire the quest sentinel exemption");
  }
  {
    Memory m;
    m.Put(Records, 2, 2);
    m.Put(Records + 0x1c, 1, 1);
    m.Put(Records + 0x1e, 1, 1);
    m.Put(Records + 0x24, 0x101, 2);
    const auto s = m.Sample();
    Check(s.valid && s.entries[0].placeholder && !s.entries[0].owned &&
              s.entries[0].kind == echoes::MenuEntryKind::None &&
              s.entries[0].mask == 0 && s.selected_index == 5,
          "structural rows never become duplicate default-mask outfit targets");
  }
  {
    Memory m;
    Check(!m.Sample(false).compatible && m.reads == 0,
          "wrong build performs no guest reads");
  }
  for (const auto &g : echoes::MenuIdentityGuards) {
    Memory m;
    m.Put(Base + g.offset + 255, text[size_t(g.offset + 255)] ^ 1, 1);
    Check(!m.Sample().compatible,
          "each native guard mismatch refuses menu metadata");
  }
  for (int fault = 0; fault < 10; ++fault) {
    Memory m;
    if (fault == 0)
      m.Put(Menu - 0x20, Base + 0x3427310, 8);
    if (fault == 1)
      m.Put(Menu + 0xa580, Records + 8 * 0x28 + 1, 8);
    if (fault == 2)
      m.Put(Menu + 0xa588, Records + 129 * 0x28, 8);
    if (fault == 3)
      m.Put(Menu + 0xa5cc, 2, 4);
    if (fault == 4)
      m.Put(Menu + 0xa5d0, 4, 4);
    if (fault == 5)
      m.Put(Rows + 4, 3, 4);
    if (fault == 6)
      m.Put(Records + 5 * 0x28, 3, 2);
    if (fault == 7)
      m.Put(Records + 5 * 0x28 + 0x1e, 2, 1);
    if (fault == 8)
      m.Put(Menu + 0x88, 1, 1);
    if (fault == 9)
      m.Put(Node2 + 8, Node2, 8);
    const auto s = m.Sample();
    Check(!s.valid && s.component == 0 && s.entries.empty(),
          "malformed vector/cursor/gear/owner/lifecycle/cycle clears complete "
          "menu snapshot");
  }
  for (int fault = 0; fault < 4; ++fault) {
    Memory m(true);
    if (fault == 0)
      m.Put(Menu + 0x5390, uint32_t(-1), 4);
    if (fault == 1)
      m.Put(Menu + 0x5390, 3, 4);
    if (fault == 2)
      m.Put(Menu + 0x53a8, Records + 3 * 0x78 + 4, 8);
    if (fault == 3)
      m.Put(Records + 0x78 + 0x24, 10000, 4);
    Check(!m.Sample().valid,
          "invalid ItemMenu current index/stride/quantity refuses payload");
  }
  for (int fault = 0; fault < 4; ++fault) {
    Memory m;
    m.before = [&](auto at, auto, auto visit) {
      if (at == Records && visit == 2) {
        if (fault == 0)
          m.Put(Records + 4, 16, 4);
        if (fault == 1)
          m.Put(Menu + 0xa5d0, 0, 4);
        if (fault == 2)
          m.Put(Menu + 0x208, 2, 1);
        if (fault == 3)
          m.Put(Main + 0x88, 0, 1);
      }
    };
    Check(!m.Sample().valid, "changed payload/cursor/nested/context cannot "
                             "publish detached menu state");
  }
  {
    Memory m;
    m.State(Main + 0x88, 0);
    Check(!m.Sample().valid,
          "a readable active child never overrides absent native-menu context");
  }
  std::cout << cases << " native menu reader checks passed\n";
}
