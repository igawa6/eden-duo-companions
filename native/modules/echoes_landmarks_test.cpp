// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_catalog.h"
#include "echoes_landmarks.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool b, const char *s) {
  ++checks;
  if (!b) {
    std::cerr << "FAIL " << s << '\n';
    std::exit(1);
  }
}
std::vector<uint8_t> File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint64_t U(const std::vector<uint8_t> &b, size_t p, size_t n = 4) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i)
    v |= uint64_t(b.at(p + i)) << (8 * i);
  return v;
}
void Put(std::vector<uint8_t> &b, size_t p, uint64_t v, size_t n = 4) {
  for (size_t i = 0; i < n; ++i)
    b.at(p + i) = uint8_t(v >> (8 * i));
}
constexpr uint64_t Base = 0x80000000, Owner = 0x90000000, Save = 0x90010000,
                   List = 0x90030000, Traverse = 0x90031000,
                   Registry = 0x90032000, Header = 0x90033000,
                   Entries = 0x90034000, Bits = 0x90039000,
                   Buckets = 0x90040000, Prior = 0x90041000, Nodes = 0x90042000;
struct Memory {
  std::unordered_map<uint64_t, uint8_t> b;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  const std::vector<uint8_t> &text;
  MapEpochSource source{
      {Owner, Save, List, 7}, Traverse, Registry, Header, Entries};
  void P(uint64_t p, uint64_t v, size_t n = 8) {
    for (size_t i = 0; i < n; ++i)
      b[p + i] = uint8_t(v >> (8 * i));
  }
  void Zero(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      b[p + i] = 0;
  }
  Memory(const std::vector<uint8_t> &t, std::span<const NativeLandmark> m)
      : text(t) {
    P(Base + 0x34bc848, Owner);
    P(Owner + 0x10, Save);
    P(Save + 0x16b80, Traverse);
    P(Save + 0x16cf8, List);
    Zero(List, 32);
    P(List + 16, 127, 4);
    P(List + 24, 7, 4);
    P(Base + 0x34b80b8, Registry);
    P(Registry + 0x78, Header);
    P(Header + 0x20, Entries);
    Zero(Save + 0x16c08, 32);
    P(Save + 0x16c08, Base + 0x33ff850);
    P(Base + 0x33ff870, Base + 0x163b1e0);
    P(Save + 0x16c10, 123, 4);
    P(Save + 0x16c18, Bits);
    P(Save + 0x16c20, 8, 4);
    Zero(Bits, 9);
    P(Bits, m.size(), 4);
    P(Bits + 4, 123, 4);
    P(Bits + 8, 0x15, 1);
    P(Base + 0x34a90c8, 1, 1);
    P(Base + 0x34a9090, 0x90044000);
    Zero(Base + 0x34a90a0, 40);
    P(Base + 0x34a90a0, Buckets);
    P(Base + 0x34a90a8, 1);
    P(Base + 0x34a90b0, Nodes);
    P(Base + 0x34a90b8, m.size());
    P(Base + 0x34a90c0, 0x3f800000, 4);
    P(Buckets, Prior);
    P(Prior, Nodes);
    for (size_t i = 0; i < m.size(); ++i) {
      const auto node = Nodes + i * 32,
                 key = uint64_t(ActorKey(m[i].conditions[0].parameter));
      Zero(node, 24);
      P(node, i + 1 < m.size() ? node + 32 : 0);
      P(node + 8, key);
      P(node + 16, key, 4);
      P(node + 20, i, 4);
    }
  }
  bool Read(uint64_t p, void *out, size_t n) {
    const auto v = ++visits[p];
    if (before)
      before(p, v);
    if (n > UINT64_MAX - p)
      return false;
    for (size_t i = 0; i < n; ++i) {
      auto it = b.find(p + i);
      if (it != b.end())
        static_cast<uint8_t *>(out)[i] = it->second;
      else if (p + i >= Base && p + i - Base < text.size())
        static_cast<uint8_t *>(out)[i] = text[size_t(p + i - Base)];
      else
        return false;
    }
    return true;
  }
  auto Snapshot(std::span<const NativeLandmark> m, bool build = true) {
    return ReadLandmarkVisibility(
        [&](auto p, auto o, auto n) { return Read(p, o, n); }, Base, build,
        source, m);
  }
};
} // namespace
int main() {
  std::vector<NativeLandmark> m(1);
  Check(!ParseNativeLandmarks({}, m) && m.empty(), "empty source clears");
  Check(!LoadNativeLandmarks({}, m) && m.empty(),
        "missing asset reader clears");
  const auto romfs = std::getenv("ECHOES_ROMFS"),
             segments = std::getenv("ECHOES_SEGMENTS");
  if (!romfs || !segments) {
    std::cout << checks << " checks; fixtures unavailable\n";
    return 77;
  }
  const auto original =
                 File(std::filesystem::path(romfs) /
                      "region_common/datasheets/ui/UiMapIconLandMark.gsheet"),
             text = File(std::filesystem::path(segments) / "text.bin");
  Check(ParseNativeLandmarks(original, m) && m.size() == 6,
        "six native landmark rows");
  for (const auto &a : m)
    Check(a.level_key == "Field" && a.texture_name == "Smoothie" &&
              a.label_ref == "glossary/Location:DekuHouse001" &&
              a.conditions.size() == 1 && a.conditions[0].category == 1,
          "original supported flag metadata");
  const auto native = m;
  auto Reject = [&](size_t p, uint64_t v, size_t n = 4) {
    auto b = original;
    Put(b, p, v, n);
    m = native;
    Check(!ParseNativeLandmarks(b, m) && m.empty(),
          "invalid original schema rejected");
  };
  Reject(8, 0);
  Reject(24, 1);
  Reject(28, 10);
  Reject(44, 31);
  Reject(32, UINT64_MAX, 8);
  Reject(40, 257);
  Reject(48 + 20, 1, 2);
  Reject(96 + 32, 0, 8);
  const auto rows = U(original, 32, 8), array = U(original, rows + 16, 8),
             expr = U(original, array + 24, 8);
  Reject(rows + 24, 257, 8);
  Reject(array + 56, 0x7fc00000);
  Reject(array + 32, 33, 8);
  Reject(expr + 8, UINT64_MAX, 8);
  for (size_t n : {size_t(0), size_t(47), size_t(575), size_t(607)}) {
    auto b = original;
    b.resize(n);
    Check(!ParseNativeLandmarks(b, m) && m.empty(),
          "truncated original refused");
  }
  m = native;
  {
    Memory mem(text, m);
    const auto s = mem.Snapshot(m);
    Check(s.compatible && s.known && s.source == mem.source &&
              s.visible == std::vector<uint8_t>({1, 0, 1, 0, 1, 0}),
          "actual guarded flag reader returns native per-save visibility");
  }
  {
    Memory mem(text, m);
    mem.P(Bits + 8, 0, 1);
    const auto s = mem.Snapshot(m);
    Check(s.known && s.visible == std::vector<uint8_t>(6, 0),
          "undiscovered landmarks all hidden");
  }
  {
    Memory mem(text, m);
    mem.P(Bits + 8, 0x3f, 1);
    const auto s = mem.Snapshot(m);
    Check(s.known && s.visible == std::vector<uint8_t>(6, 1),
          "all native flags visible");
  }
  for (int fault = 0; fault < 8; ++fault) {
    auto altered = m;
    Memory mem(text, m);
    if (fault == 0)
      altered[0].conditions[0].category = 16;
    if (fault == 1)
      altered[0].conditions.clear();
    if (fault == 2)
      altered[0].condition_strategy = 1;
    if (fault == 3)
      mem.P(Save + 0x16c08, Base, 8);
    if (fault == 4)
      mem.P(Nodes + 20, 6, 4);
    if (fault == 5)
      mem.P(List + 24, 8, 4);
    if (fault == 6)
      mem.before = [&](uint64_t p, size_t n) {
        if (p == Bits && n == 2)
          mem.P(Bits + 8, 0, 1);
      };
    if (fault == 7)
      mem.before = [&](uint64_t p, size_t n) {
        if (p == Base + 0x34bc848 && n == 2)
          mem.P(Base + 0x34bc848, Owner + 8);
      };
    const auto s = mem.Snapshot(altered);
    Check(!s.known && s.visible.empty(),
          "unsupported gate or changing source hides all");
  }
  {
    Memory mem(text, m);
    const auto s = mem.Snapshot(m, false);
    Check(!s.compatible && !s.known && s.visible.empty() && mem.visits.empty(),
          "unsupported build makes no reads");
  }
  std::cout << checks << " checks passed\n";
}
