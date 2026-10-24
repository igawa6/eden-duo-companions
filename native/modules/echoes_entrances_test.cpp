// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_entrances.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool yes, const char *why) {
  ++checks;
  if (!yes) {
    std::cerr << "FAIL " << why << '\n';
    std::exit(1);
  }
}
std::vector<uint8_t> File(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint32_t Crc(std::string_view s) {
  uint32_t c = ~0u;
  for (unsigned char b : s) {
    c ^= b;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
  }
  return ~c;
}
constexpr uint64_t Base = 0x80000000, Owner = 0x90000000, Save = 0x90010000,
                   List = 0x90030000, Traverse = 0x90031000,
                   Registry = 0x90032000, Header = 0x90033000,
                   Entries = 0x90034000, Buckets = 0x90040000,
                   Prior = 0x90041000, Nodes = 0x90042000;
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  const std::vector<uint8_t> &text;
  MapEpochSource source{
      {Owner, Save, List, 7}, Traverse, Registry, Header, Entries};
  void Put(uint64_t p, uint64_t v, size_t n = 8) {
    for (size_t i = 0; i < n; ++i)
      bytes[p + i] = uint8_t(v >> (8 * i));
  }
  void Zero(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[p + i] = 0;
  }
  Memory(const std::vector<uint8_t> &t, const NativeEntranceCatalog &c)
      : text(t) {
    Put(Base + 0x34bc848, Owner);
    Put(Owner + 16, Save);
    Put(Save + 0x16b80, Traverse);
    Put(Save + 0x16cf8, List);
    Zero(List, 32);
    Put(List + 16, 127, 4);
    Put(List + 24, 7, 4);
    Put(Base + 0x34b80b8, Registry);
    Put(Registry + 0x78, Header);
    Put(Header + 0x20, Entries);
    Put(Save + 0x16a70, Save + 0xbcf0);
    Zero(Save + 0x16ca8, 32);
    Put(Save + 0x16ca8, Base + 0x33ff9a8);
    Put(Save + 0x16cb0, 123, 4);
    Put(Save + 0x16cb8, Save + 0xe160);
    Put(Save + 0x16cc0, 256, 4);
    Put(Base + 0x33ff9c8, Base + 0x3f0a30);
    Zero(Save + 0xe160, 40);
    Put(Save + 0xe160, c.icons.size(), 4);
    Put(Save + 0xe164, 123, 4);
    Zero(Base + 0x34bc108, 72);
    Put(Base + 0x34bc150, 1, 1);
    Put(Base + 0x34bc108, Registry);
    Put(Base + 0x34bc118, Entries);
    Zero(Base + 0x34bc128, 40);
    Put(Base + 0x34bc128, Buckets);
    Put(Base + 0x34bc130, 1);
    Put(Base + 0x34bc138, Nodes);
    Put(Base + 0x34bc140, c.icons.size());
    Put(Base + 0x34bc148, 0x3f800000, 4);
    Put(Buckets, Prior);
    Put(Prior, Nodes);
    for (size_t i = 0; i < c.icons.size(); ++i) {
      const auto node = Nodes + i * 32, key = uint64_t(Crc(c.icons[i].key));
      Zero(node, 24);
      Put(node, i + 1 < c.icons.size() ? node + 32 : 0);
      Put(node + 8, key);
      Put(node + 16, key, 4);
      Put(node + 20, i, 4);
    }
  }
  bool Read(uint64_t p, void *out, size_t n) {
    const auto visit = ++visits[p];
    if (before)
      before(p, visit);
    if (n > UINT64_MAX - p)
      return false;
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it != bytes.end())
        static_cast<uint8_t *>(out)[i] = it->second;
      else if (p + i >= Base && p + i - Base < text.size())
        static_cast<uint8_t *>(out)[i] = text[size_t(p + i - Base)];
      else
        return false;
    }
    return true;
  }
  auto Snapshot(const NativeEntranceCatalog &c, bool build = true) {
    return ReadEntranceVisibility(
        [&](auto p, auto o, auto n) { return Read(p, o, n); }, Base, build,
        source, c);
  }
};
} // namespace
int main() {
  NativeEntranceCatalog c;
  c.icons.resize(1);
  Check(!LoadNativeEntrances({}, c) && c.icons.empty() && c.field.empty(),
        "missing reader clears");
  const auto root = std::getenv("ECHOES_ROMFS"),
             segments = std::getenv("ECHOES_SEGMENTS");
  if (!root || !segments)
    return 77;
  std::map<std::string, std::vector<uint8_t>, std::less<>> files;
  for (auto path :
       {"region_common/level/Common.eclo",
        "region_common/datasheets/ui/UiMapIconSensorLevelOpen.gsheet",
        "region_common/datasheets/ui/UiMapIconSensorLevelOpenPair.gsheet"})
    files[path] = File(std::filesystem::path(root) / path);
  auto read = [&](std::string_view path, uint64_t offset, void *out,
                  size_t n) -> size_t {
    const auto it = files.find(path);
    if (it == files.end())
      return 0;
    const auto &b = it->second;
    if (!out && !n)
      return b.size();
    if (offset > b.size() || n > b.size() - offset)
      return 0;
    std::memcpy(out, b.data() + offset, n);
    return n;
  };
  Check(LoadNativeEntrances(read, c), "original catalog loads");
  std::cout << "catalog " << c.icons.size() << " icons, " << c.field.size()
            << " direct Field entrances\n";
  Check(c.icons.size() == 106 && c.field.size() == 91,
        "original bounded direct slice");
  size_t cities = 0, shops = 0;
  for (const auto &e : c.field) {
    Check(e.icon_index < c.icons.size(), "native index");
    cities += c.icons[e.icon_index].texture_name == "City";
    shops += c.icons[e.icon_index].texture_name == "Shop";
  }
  Check(cities == 15 && shops == 6, "original town and shop coverage");
  const auto native = c;
  for (const auto &[path, b] : files) {
    const auto original = b;
    for (size_t length : {size_t(0), size_t(7), size_t(47)}) {
      files[path].resize(length);
      c = native;
      Check(!LoadNativeEntrances(read, c) && c.icons.empty() && c.field.empty(),
            "truncation clears");
    }
    files[path] = original;
  }
  for (int fault = 0; fault < 8; ++fault) {
    const std::string
        sheet = "region_common/datasheets/ui/UiMapIconSensorLevelOpen.gsheet",
        common = "region_common/level/Common.eclo";
    const auto original = files;
    auto put = [&](std::string_view path, size_t p, uint64_t value, size_t n) {
      auto &b = files.find(path)->second;
      for (size_t i = 0; i < n; ++i)
        b.at(p + i) = uint8_t(value >> (8 * i));
    };
    if (fault == 0)
      put(sheet, 48 + 20, 1, 2);
    if (fault == 1)
      put(sheet, 40, 257, 4);
    if (fault == 2)
      put(sheet, 32, UINT64_MAX, 8);
    if (fault == 3)
      put(sheet, 336 + 56, 0x7fc00000, 4);
    if (fault == 4)
      put(common, 0x88, 0, 4);
    if (fault == 5)
      put(common, 0x30, UINT32_MAX, 4);
    if (fault == 6)
      put(common, 0x1a2a0 + 192 * 64 + 32, 65535, 2);
    if (fault == 7)
      put(common, 0x1a2a0 + 192 * 64, 0x7fc00000, 4);
    c = native;
    Check(!LoadNativeEntrances(read, c) && c.icons.empty() && c.field.empty(),
          "malformed schema, typed source or geometry clears");
    files = original;
  }
  c = native;
  const auto text = File(std::filesystem::path(segments) / "text.bin");
  Check(!text.empty(), "original instruction fixture");
  {
    Memory m(text, c);
    auto s = m.Snapshot(c);
    Check(s.compatible && s.known && s.native_dictionary_validated &&
              s.source == m.source && s.visible == std::vector<uint8_t>(91, 0),
          "undiscovered stays hidden");
  }
  {
    Memory m(text, c);
    for (size_t i = 0; i < 32; ++i)
      m.Put(Save + 0xe168 + i, 255, 1);
    auto s = m.Snapshot(c);
    Check(s.known && s.visible == std::vector<uint8_t>(91, 1),
          "positive original discovery visible");
  }
  for (const auto off : {0x16a5370u, 0x16a55e4u, 0x16515f4u}) {
    Memory m(text, c);
    m.Zero(Base + 0x34bc108, 72);
    m.Put(Base + 0x34bc150, 0, 1);
    m.Put(Base + off, 0, 4);
    const auto state = m.Snapshot(c);
    Check(!state.compatible && !state.known,
          "original index builder code mandatory before map");
  }
  for (int phase = 0; phase < 2; ++phase) {
    Memory m(text, c);
    m.Zero(Base + 0x34bc108, 72);
    m.Put(Base + 0x34bc150, phase, 1);
    if (phase)
      m.Put(Base + 0x34bc148, 0x3f800000, 4);
    m.Put(Save + 0xe168 + c.field[0].icon_index / 8,
          1u << (c.field[0].icon_index % 8), 1);
    auto state = m.Snapshot(c);
    Check(state.known && !state.native_dictionary_validated && state.visible[0],
          "passive pre-map discovery uses guarded original ordinal");
    auto altered = c;
    std::swap(altered.icons[0], altered.icons[1]);
    state = m.Snapshot(altered);
    Check(!state.known && state.visible.empty(),
          "pre-map catalog order mutation rejected");
    m.Put(Base + 0x34bc128, Buckets);
    state = m.Snapshot(c);
    Check(!state.known && state.visible.empty(),
          "partial native dictionary never bypassed");
  }
  {
    Memory m(text, c);
    m.Zero(Base + 0x34bc108, 72);
    m.Put(Base + 0x34bc150, 0, 1);
    m.before = [&](uint64_t p, size_t n) {
      if (p == Base + 0x34bc108 && n == 4)
        m.Put(Base + 0x34bc108, Registry);
    };
    const auto state = m.Snapshot(c);
    Check(!state.known && state.visible.empty(),
          "pre-map metadata changes during final replay rejected");
  }
  for (int fault = 0; fault < 5; ++fault) {
    Memory m(text, c);
    m.Zero(Base + 0x34bc108, 72);
    m.Put(Base + 0x34bc150, 0, 1);
    if (fault == 0)
      m.Put(Base + 0x34bc110, 1);
    if (fault == 1)
      m.Put(Base + 0x34bc120, 1);
    if (fault == 2)
      m.Put(Base + 0x34bc148, 0x3f800000, 4);
    if (fault == 3)
      m.Put(Base + 0x34bc14c, 1, 4);
    if (fault == 4)
      m.Put(Save + 0xe164, 124, 4);
    const auto state = m.Snapshot(c);
    Check(!state.known && state.visible.empty(),
          "unknown empty metadata or invalid saved revision rejected");
  }
  for (const auto &candidate : c.field) {
    Memory m(text, c);
    m.Put(Save + 0xe168 + candidate.icon_index / 8,
          1u << (candidate.icon_index % 8), 1);
    auto s = m.Snapshot(c);
    Check(s.known, "each discovery known");
    for (size_t i = 0; i < c.field.size(); ++i)
      Check(s.visible[i] == (c.field[i].icon_index == candidate.icon_index),
            "discovery isolates exact destination");
  }
  for (int fault = 0; fault < 15; ++fault) {
    Memory m(text, c);
    auto altered = c;
    if (fault == 0)
      m.Put(Save + 0x16a70, Save);
    if (fault == 1)
      m.Put(Save + 0x16ca8, Base);
    if (fault == 2)
      m.Put(Save + 0x16cb8, Save + 0xe168);
    if (fault == 3)
      m.Put(Save + 0x16cc0, 257, 4);
    if (fault == 4)
      m.Put(Save + 0xe160, 105, 4);
    if (fault == 5)
      m.Put(Save + 0xe164, 124, 4);
    if (fault == 6)
      m.Put(Base + 0x34bc150, 0, 1);
    if (fault == 7)
      m.Put(Nodes + 20, 106, 4);
    if (fault == 8)
      m.Put(Nodes, Nodes);
    if (fault == 9)
      m.Put(List + 24, 8, 4);
    if (fault == 10)
      m.Put(Base + 0x236b698, 0, 4);
    if (fault == 11)
      altered.icons[0].key = "unknown";
    if (fault == 12)
      m.before = [&](uint64_t p, size_t n) {
        if (p == Save + 0xe160 && n == 2)
          m.Put(Save + 0xe168, 1, 1);
      };
    if (fault == 13)
      m.before = [&](uint64_t p, size_t n) {
        if (p == Base + 0x34bc848 && n == 4)
          m.Put(Base + 0x34bc848, Owner + 8);
      };
    if (fault == 14)
      m.before = [&](uint64_t p, size_t n) {
        if (p == Save + 0xe160 && n == 4)
          m.Put(Save + 0xe168, 1, 1);
      };
    auto s = m.Snapshot(altered);
    Check(!s.known && s.visible.empty(),
          "invalid, mixed or replay-changing source hidden");
  }
  {
    Memory m(text, c);
    auto s = m.Snapshot(c, false);
    Check(!s.compatible && !s.known && m.visits.empty(),
          "unsupported build no reads");
  }
  std::cout << checks << " checks passed\n";
}
