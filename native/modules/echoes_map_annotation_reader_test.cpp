// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_catalog.h"
#include "echoes_map_annotations.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>
namespace {
constexpr uint64_t Base = 0x80000000, Owner = 0x90000000, Save = 0x90010000,
                   List = 0x90030000, Traverse = 0x90031000,
                   Registry = 0x90032000, Header = 0x90033000,
                   Entries = 0x90034000, Controller = 0x90035000,
                   Handle = 0x90036000, Resource = 0x90037000,
                   LevelKey = 0x90038000, Bits = 0x90039000,
                   Buckets = 0x90040000, Prior = 0x90041000, Node1 = 0x90042000,
                   Node2 = 0x90043000;
std::vector<uint8_t> text;
size_t checks{};
void Check(bool value, const char *reason) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL " << checks << ": " << reason << '\n';
    std::abort();
  }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  echoes::MapSnapshot map;
  std::vector<echoes::NativeLocation> locations;
  std::vector<echoes::NativeRiftRegion> rifts;
  size_t reads{};
  void Put(uint64_t p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[p + i] = uint8_t(v >> (i * 8));
  }
  void Zero(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[p + i] = 0;
  }
  void Name(uint64_t p, std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[p + i] = uint8_t(s[i]);
    bytes[p + s.size()] = 0;
  }
  Memory() {
    map.compatible = map.discovery_valid = map.current_level_valid = true;
    map.source = {{Owner, Save, List, 7}, Traverse, Registry, Header, Entries};
    map.current_level_source = {Controller, Handle, Resource, LevelKey};
    map.current_level_key = "Field";
    Put(Base + 0x34bc848, Owner, 8);
    Put(Owner + 0x10, Save, 8);
    Put(Save + 0x16b80, Traverse, 8);
    Put(Save + 0x16cf8, List, 8);
    Zero(List, 32);
    Put(List + 16, 127, 4);
    Put(List + 24, 7, 4);
    Put(Base + 0x34b80b8, Registry, 8);
    Put(Registry + 0x78, Header, 8);
    Put(Header + 0x20, Entries, 8);
    Put(Base + 0x34bd718, Controller, 8);
    Put(Controller, Base + 0x3269630, 8);
    Put(Controller + 0x80, Handle, 8);
    Put(Handle, Base + 0x3268098, 8);
    Put(Handle + 0x90, Resource, 8);
    Put(Resource, Base + 0x3268738, 8);
    Put(Base + 0x32687d0, Base + 0x16833b0, 8);
    Zero(Resource + 0x80, 72);
    Name(Resource + 0x80, "DekuTreeArea");
    Put(Resource + 0xc0, 12, 8);
    Put(Resource + 0x140, LevelKey, 8);
    Put(Resource + 0x148, 5, 8);
    Name(LevelKey, "Field");
    echoes::NativeLocation location;
    location.key = "DekuTreeArea";
    locations.push_back(location);
    Zero(Save + 0x16c08, 32);
    Put(Save + 0x16c08, Base + 0x33ff850, 8);
    Put(Base + 0x33ff870, Base + 0x163b1e0, 8);
    Put(Save + 0x16c10, 123, 4);
    Put(Save + 0x16c18, Bits, 8);
    Put(Save + 0x16c20, 8, 4);
    Zero(Bits, 9);
    Put(Bits, 2, 4);
    Put(Bits + 4, 123, 4);
    Put(Bits + 8, 1, 1);
    Put(Base + 0x34a90c8, 1, 1);
    Put(Base + 0x34a9090, 0x90044000, 8);
    Zero(Base + 0x34a90a0, 40);
    Put(Base + 0x34a90a0, Buckets, 8);
    Put(Base + 0x34a90a8, 1, 8);
    Put(Base + 0x34a90b0, Node1, 8);
    Put(Base + 0x34a90b8, 2, 8);
    Put(Base + 0x34a90c0, 0x3f800000, 4);
    Put(Buckets, Prior, 8);
    Put(Prior, Node1, 8);
    Zero(Node1, 24);
    Zero(Node2, 24);
    Put(Node1, Node2, 8);
    for (const auto [node, symbol, index] :
         {std::tuple{Node1, "FlagActivate", 0u},
          std::tuple{Node2, "FlagDeactivate", 1u}}) {
      const auto key = echoes::ActorKey(symbol);
      Put(node + 8, key, 8);
      Put(node + 16, key, 4);
      Put(node + 20, index, 4);
    }
    rifts = {{1, "FlagActivate", "FlagDeactivate", {}},
             {2, "", "FlagDeactivate", {}},
             {3, "FlagActivate", "", {}},
             {4, "", "", {}}};
  }
  bool Read(uint64_t p, void *out, size_t n) {
    ++reads;
    const auto v = ++visits[p];
    if (before)
      before(p, v);
    if (n > UINT64_MAX - p)
      return false;
    auto *target = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      const auto it = bytes.find(p + i);
      if (it != bytes.end())
        target[i] = it->second;
      else if (p + i >= Base && p + i - Base < text.size())
        target[i] = text[size_t(p + i - Base)];
      else
        return false;
    }
    return true;
  }
  echoes::ReadFn Reader() {
    return [&](auto p, auto o, auto n) { return Read(p, o, n); };
  }
  auto Location(bool build = true) {
    return echoes::ReadCurrentLocation(Reader(), Base, build, map, locations);
  }
  auto Rifts(bool build = true) {
    return echoes::ReadRiftVisibility(Reader(), Base, build, map.source, rifts);
  }
};
} // namespace
int main() {
  const char *env = std::getenv("ECHOES_SEGMENTS");
  if (!env) {
    std::cout << "Original segments unavailable\n";
    return 77;
  }
  std::ifstream f(std::filesystem::path(env) / "text.bin", std::ios::binary);
  text = {std::istreambuf_iterator<char>(f), {}};
  Check(text.size() > 0x177b450, "original immutable guards available");
  {
    Memory m;
    const auto s = m.Location();
    Check(s.compatible && s.known && s.key == "DekuTreeArea" &&
              s.source == m.map.source &&
              s.level_source == m.map.current_level_source,
          "persistent resource area survives absent transient announcement");
  }
  for (int fault = 0; fault < 10; ++fault) {
    Memory m;
    if (fault == 0)
      m.Put(Resource + 0xc0, 64, 8);
    if (fault == 1)
      m.bytes[Resource + 0x80 + 12] = 'x';
    if (fault == 2)
      m.locations.clear();
    if (fault == 3)
      m.locations.push_back(m.locations[0]);
    if (fault == 4)
      m.Put(Handle + 0x90, Resource + 8, 8);
    if (fault == 5)
      m.Put(Resource, Base + 0x3268730, 8);
    if (fault == 6)
      m.Put(List + 24, 8, 4);
    if (fault == 7)
      m.before = [&](auto p, auto visit) {
        if (p == Resource + 0x80 && visit == 2)
          m.Name(Resource + 0x80, "OtherArea");
      };
    if (fault == 8)
      m.before = [&](auto p, auto visit) {
        if (p == Resource + 0x80 && visit == 4)
          m.Put(Owner + 0x10, Save + 8, 8);
      };
    if (fault == 9)
      m.bytes[Resource + 0x80] = '.';
    const auto s = m.Location();
    Check(!s.known && s.key.empty(),
          "malformed, absent, changed or detached location never publishes");
  }
  for (int fault = -1; fault < 11; ++fault) {
    Memory m;
    constexpr uint64_t Location = 0x90045000;
    m.Zero(Resource + 0x80, 72);
    m.Name(Resource + 0x80, "Field");
    m.Put(Resource + 0xc0, 5, 8);
    m.Put(Save + 0x16b70, Location, 8);
    m.Zero(Location + 0x10, 64);
    m.Name(Location + 0x10, "Field");
    m.Zero(Location + 0x90, 64);
    m.Name(Location + 0x90, "Field_Forest");
    m.locations[0].key = "Field_Forest";
    if (fault == 0) m.Put(Save + 0x16b70, 0, 8);
    if (fault == 1) m.Put(Save + 0x16b70, Location + 1, 8);
    if (fault == 2) m.Name(Location + 0x10, "OtherLevel");
    if (fault == 3) m.Name(Location + 0x90, "UnknownArea");
    if (fault == 4) m.Zero(Location + 0x90, 64);
    if (fault == 5) for (size_t i=0;i<64;++i) m.bytes[Location+0x90+i]='A';
    if (fault == 6) m.locations.push_back(m.locations[0]);
    if (fault == 7) m.before = [&](auto p, auto v) {
      if (p == Location+0x90 && v == 2) m.Name(Location+0x90,"OtherArea");
    };
    if (fault == 8) m.before = [&](auto p, auto v) {
      if (p == Location+0x90 && v == 4) m.Put(Save+0x16b70,Location+8,8);
    };
    if (fault == 9) m.Name(Location + 0x90,"Field.Forest");
    if (fault == 10) m.Put(Base+0x1699e9c,text[0x1699e9c]^1,1);
    const auto result = m.Location();
    Check(fault == -1 ? result.known && result.key == "Field_Forest" :
                       !result.known && result.key.empty(),
          "native persistent Field key requires exact level, metadata, guards and stable epoch");
  }
  for (uint8_t bits = 0; bits < 4; ++bits) {
    Memory m;
    m.Put(Bits + 8, bits, 1);
    const auto s = m.Rifts();
    Check(s.compatible && s.known && s.regions.size() == 4 &&
              s.regions[0].active == bool((bits & 1) && !(bits & 2)) &&
              s.regions[1].active == !(bits & 2) &&
              s.regions[2].active == bool(bits & 1) && s.regions[3].active,
          "native activation AND NOT deactivation with empty defaults");
  }
  for (int fault = 0; fault < 12; ++fault) {
    Memory m;
    if (fault == 0)
      m.Put(Base + 0x34a90c8, 0, 1);
    if (fault == 1)
      m.Put(Save + 0x16c08, Base + 0x33ff808, 8);
    if (fault == 2)
      m.Put(Save + 0x16c20, 8193, 4);
    if (fault == 3)
      m.Put(Bits + 4, 124, 4);
    if (fault == 4)
      m.Put(Node2, Node1, 8);
    if (fault == 5)
      m.Put(Node1 + 20, 2, 4);
    if (fault == 6)
      m.rifts[0].activate_flag = "MissingFlag";
    if (fault == 7)
      m.Put(Base + 0x34a90b8, 3, 8);
    if (fault == 8)
      m.Put(Node2 + 8, echoes::ActorKey("FlagActivate"), 8),
          m.Put(Node2 + 16, echoes::ActorKey("FlagActivate"), 4);
    if (fault == 9)
      m.before = [&](auto p, auto visit) {
        if (p == Bits && visit == 2)
          m.Put(Bits + 8, 3, 1);
      };
    if (fault == 10)
      m.before = [&](auto p, auto visit) {
        if (p == Bits && visit == 3)
          m.Put(Owner + 0x10, Save + 8, 8);
      };
    if (fault == 11)
      m.rifts[1].unique_id = 1;
    const auto s = m.Rifts();
    Check(!s.known && s.regions.empty(),
          "malformed/racing native flags never authorize partial overlays");
  }
  for (const auto offset : {0x163b1e0, 0x177b1d4, 0x177b3a8, 0x177b42c}) {
    Memory m;
    m.Put(Base + offset, text[size_t(offset)] ^ 1, 1);
    Check(!m.Rifts().compatible,
          "each native flag/semantics guard refuses changed binary");
  }
  for (const auto offset : {0x16833b0, 0x1683610, 0x1683710}) {
    Memory m;
    m.Put(Base + offset, text[size_t(offset)] ^ 1, 1);
    Check(!m.Location().compatible,
          "each native location writer guard refuses changed binary");
  }
  {
    Memory m;
    Check(!m.Location(false).known && !m.Rifts(false).known && !m.reads,
          "wrong build does not read guest memory");
  }
  std::cout << checks << " native annotation reader checks passed\n";
}
