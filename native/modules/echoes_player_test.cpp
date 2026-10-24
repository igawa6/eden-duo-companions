// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_player.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <unordered_map>

namespace {
constexpr uint64_t Base = 0x8070a000, Access = 0x2107000000;
constexpr uint64_t Save = 0x2107100000, List = 0x2107200000;
constexpr uint64_t Records = 0x2107300000, Registry = 0x2107400000;
constexpr uint64_t Header = 0x2107500000, Entries = 0x2107600000;
constexpr uint64_t Strings = 0x2107700000;
constexpr uint64_t Controller = 0x2107800000, Handle = 0x2107900000;
constexpr uint64_t Resource = 0x2107a00000, LevelName = 0x2107b00001;
size_t cases{};
void Check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL after " << cases << ": " << message << '\n';
    std::exit(1);
  }
}
struct Memory {
  std::unordered_map<uint64_t, uint8_t> bytes;
  std::map<uint64_t, size_t> visits;
  std::function<void(uint64_t, size_t)> before;
  size_t reads{}, largest{};
  void Put(uint64_t at, uint64_t value, size_t n = 8) {
    for (size_t i = 0; i < n; ++i)
      bytes[at + i] = uint8_t(value >> (i * 8));
  }
  void Zero(uint64_t at, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[at + i] = 0;
  }
  void Key(size_t i, const std::string &key) {
    Put(Entries + i * 16, Strings + i * 64);
    Put(Entries + i * 16 + 8, key.size(), 4);
    for (size_t j = 0; j < key.size(); ++j)
      bytes[Strings + i * 64 + j] = uint8_t(key[j]);
  }
  Memory() {
    for (const auto &g : echoes::IdentityGuards)
      for (size_t i = 0; i < g.bytes.size(); ++i)
        bytes[Base + g.offset + i] = g.bytes[i];
    const std::array<std::pair<uint64_t, std::array<uint8_t, 16>>, 3> guards{{
        {0x1688dcc,
         {0x36, 0x89, 0x40, 0xb9, 0x48, 0x03, 0x00, 0x54, 0xdf, 0xfe, 0x07,
          0x71, 0x08, 0x03, 0x00, 0x54}},
        {0x2386de0,
         {0xfd, 0x7b, 0xba, 0xa9, 0xfb, 0x0b, 0x00, 0xf9, 0xfa, 0x67, 0x02,
          0xa9, 0xf8, 0x5f, 0x03, 0xa9}},
        {0x261c890,
         {0x7f, 0x7f, 0x00, 0x71, 0x88, 0xff, 0xff, 0x54, 0x48, 0x03, 0x40,
          0xf9, 0xa9, 0x7e, 0x43, 0xd3}},
    }};
    for (const auto &[offset, value] : guards)
      for (size_t i = 0; i < value.size(); ++i)
        bytes[Base + offset + i] = value[i];
    Put(Base + 0x34bc848, Access);
    Put(Access + 0x10, Save);
    Put(Save + 0x16cf8, List);
    Zero(List, 0x20);
    Put(List + 0x18, 7, 4);
    Put(Save + 0x16b80, Records);
    Zero(Records, 32 * 0xd0);
    Put(Base + 0x34b80b8, Registry);
    Put(Registry + 0x78, Header);
    Zero(Header, 0x30);
    Put(Header + 0x20, Entries);
    Put(Header + 0x28, 28, 1);
    Zero(Entries, 28 * 16);
    for (size_t i = 0; i < 28; ++i)
      Key(i, i == 0 ? "Field" : i == 12 ? "" : "Map_" + std::to_string(i));
    // Deliberately differ across maps and bitplanes, including native edge511.
    Put(Records + 0x30, 0x81, 1);
    Put(Records + 0x70, 1, 1);
    Put(Records + 0xd0 + 0x30 + 63, 0x80, 1);
  }
  void Level(std::string key = "DekuTreeArea") {
    const std::array<std::pair<uint64_t, std::array<uint8_t, 16>>, 3> guards{{
        {0x360870,
         {0xfd, 0x7b, 0xbd, 0xa9, 0xf5, 0x0b, 0x00, 0xf9, 0xf4, 0x4f, 0x02,
          0xa9, 0xfd, 0x03, 0x00, 0x91}},
        {0x165b4c4,
         {0x68, 0xf2, 0x00, 0xd0, 0x08, 0xe1, 0x04, 0x91, 0x3f, 0x00, 0x08,
          0xeb, 0x40, 0x01, 0x00, 0x54}},
        {0x16659e8,
         {0x08, 0xe0, 0x00, 0xf0, 0x08, 0xe1, 0x1c, 0x91, 0x00, 0x27, 0x80,
          0x52, 0xc8, 0x02, 0x00, 0xf9}},
    }};
    for (const auto &[offset, value] : guards)
      for (size_t i = 0; i < value.size(); ++i)
        bytes[Base + offset + i] = value[i];
    Put(Base + 0x34bd718, Controller);
    Put(Controller, Base + 0x3269630);
    Put(Controller + 0x80, Handle);
    Put(Handle, Base + 0x3268098);
    Put(Handle + 0x90, Resource);
    Put(Resource, Base + 0x3268738);
    Put(Resource + 0x140, LevelName);
    Put(Resource + 0x148, key.size());
    for (size_t i = 0; i < key.size(); ++i)
      bytes[LevelName + i] = uint8_t(key[i]);
  }
  bool Read(uint64_t at, void *out, size_t n) {
    ++reads;
    largest = std::max(largest, n);
    const auto visit = ++visits[at];
    if (before)
      before(at, visit);
    if (!at || n > std::numeric_limits<uint64_t>::max() - at)
      return false;
    auto *p = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      const auto it = bytes.find(at + i);
      if (it == bytes.end())
        return false;
      p[i] = it->second;
    }
    return true;
  }
  echoes::MapSnapshot Sample(bool build = true, uint64_t base = Base) {
    return echoes::ReadMapSnapshot(
        [&](auto a, auto p, auto n) { return Read(a, p, n); }, base, build);
  }
};
constexpr uint64_t Holder = 0x2107c00000, Payload = 0x2107d00000;
constexpr uint64_t Lock = 0x0400000100000000;
void World(Memory &m) {
  m.Level("Field");
  {
    const std::array<uint8_t, 16> b{0x89, 0x84, 0x01, 0x90, 0x29, 0xa1,
                                    0x13, 0x91, 0xb6, 0x8b, 0x01, 0xf0,
                                    0x09, 0x50, 0x00, 0xa9};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x344298 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0xa8, 0x9b, 0x00, 0xf0, 0x14, 0xe1,
                                    0x44, 0xf9, 0x14, 0x1a, 0x00, 0xb4,
                                    0x98, 0x42, 0x00, 0x91};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x2144828 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x88, 0xfe, 0x5f, 0xc8, 0x08, 0x41,
                                    0x40, 0x91, 0x88, 0xfe, 0x09, 0xc8,
                                    0xa9, 0xff, 0xff, 0x35};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x21d37b4 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x08, 0x01, 0x15, 0x8a, 0x09, 0xc1,
                                    0x7f, 0x91, 0x29, 0x05, 0x47, 0xd2,
                                    0x1f, 0x20, 0x03, 0xd5};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x21d37d0 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x68, 0x62, 0x00, 0x91, 0x00, 0xfd,
                                    0x9f, 0xc8, 0x1f, 0x20, 0x03, 0xd5,
                                    0x1f, 0x20, 0x03, 0xd5};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x21d3850 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x88, 0x06, 0x40, 0xf9, 0xa9, 0x06,
                                    0x40, 0xf9, 0x09, 0x05, 0x00, 0xf9,
                                    0xea, 0x5b, 0x40, 0xf9};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x21448e4 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x09, 0xff, 0xdf, 0xc8, 0xea, 0x9f,
                                    0x68, 0xb2, 0x0c, 0x20, 0xa0, 0x52,
                                    0x28, 0x1d, 0x68, 0x92};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x2144a40 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0xe9, 0x7f, 0x60, 0xb2, 0x08, 0xff,
                                    0x5f, 0xc8, 0x0a, 0x01, 0x09, 0x8b,
                                    0x0a, 0xff, 0x0b, 0xc8};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x2144b1c + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x94, 0xe2, 0x44, 0xf9, 0xe1, 0x03,
                                    0x1f, 0x2a, 0x60, 0xf2, 0x40, 0xf9,
                                    0x88, 0x06, 0x40, 0xf9};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x26198e8 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x2a, 0x81, 0x5f, 0xb8, 0x5f, 0x01,
                                    0x14, 0x6b, 0x61, 0xff, 0xff, 0x54,
                                    0x20, 0xc1, 0x5f, 0xbc};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x268e8bc + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x62, 0x4e, 0x41, 0xb9, 0xe1, 0xc3,
                                    0x00, 0x91, 0xe0, 0x03, 0x14, 0xaa,
                                    0x77, 0xd1, 0x01, 0x94};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x261a1f8 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0xe0, 0x03, 0x13, 0xaa, 0xf1, 0xe7,
                                    0xff, 0x97, 0x68, 0xad, 0x8f, 0x52,
                                    0x28, 0x52, 0xa7, 0x72};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x261a3f8 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x82, 0x14, 0x00, 0xf0, 0x42, 0x7c,
                                    0x35, 0x91, 0xe0, 0x83, 0x00, 0x91,
                                    0xe1, 0x03, 0x13, 0xaa};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x260b358 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x68, 0x02, 0x40, 0xf9, 0xe0, 0x03,
                                    0x13, 0xaa, 0x08, 0x75, 0x40, 0xf9,
                                    0x00, 0x01, 0x3f, 0xd6};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x260ba8c + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x08, 0xbc, 0x40, 0xf9, 0x09, 0x00,
                                    0x40, 0xf9, 0x29, 0x79, 0x40, 0xf9,
                                    0x08, 0x71, 0x40, 0xbd};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x260e394 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x08, 0x60, 0x41, 0xf9, 0xa8, 0x00,
                                    0x00, 0xb4, 0x00, 0x51, 0x40, 0xbd,
                                    0x08, 0x51, 0x01, 0x91};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x260e3d0 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x01, 0x09, 0x43, 0xad, 0x21, 0xd4,
                                    0x22, 0x4e, 0xe2, 0x67, 0x05, 0x4f,
                                    0x08, 0xa4, 0x43, 0x39};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x2619e58 + i] = b[i];
  }
  {
    const std::array<uint8_t, 16> b{0x22, 0x04, 0x14, 0x5e, 0x42, 0x40,
                                    0x21, 0x1e, 0x23, 0x04, 0x0c, 0x5e,
                                    0x42, 0x0c, 0x23, 0x1e};
    for (size_t i = 0; i < b.size(); ++i)
      m.bytes[Base + 0x2619e80 + i] = b[i];
  }
  m.Put(Base + 0x34bb9c0, Holder);
  m.Zero(Holder, 40);
  m.Put(Holder, Base + 0x33d44e8);
  m.Put(Holder + 8, Payload);
  m.Put(Holder + 16, Lock);
  m.Put(Holder + 32, Payload);
  m.Zero(Payload, 64);
  for (size_t i = 0; i < 3; ++i)
    m.Put(Payload + i * 4,
          std::bit_cast<uint32_t>(std::array<float, 3>{198, 0, 72.5f}[i]), 4);
}
echoes::PlayerWorldSnapshot ReadWorld(Memory &m,
                                      const echoes::MapSnapshot &expected,
                                      bool build = true, uint64_t base = Base) {
  return echoes::ReadPlayerWorld(
      [&](auto a, auto p, auto n) { return m.Read(a, p, n); }, base, build,
      expected);
}
void Empty(const echoes::PlayerWorldSnapshot &s) {
  Check(!s.valid && s.level_key.empty() && s.xyz == std::array<float, 3>{} &&
            s.source == echoes::PlayerWorldSource{},
        "failed world clears all values/source");
  ++cases;
}
std::vector<uint8_t> File(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return {};
  auto n = f.tellg();
  if (n < 0 || n > 1024 * 1024)
    return {};
  std::vector<uint8_t> b(static_cast<size_t>(n));
  f.seekg(0);
  f.read(reinterpret_cast<char *>(b.data()), n);
  if (!f)
    return {};
  return b;
}
uint32_t U32(const uint8_t *p) {
  return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint16_t U16(const uint8_t *p) { return uint16_t(p[0] | uint16_t(p[1]) << 8); }
struct NativeArchive {
  std::filesystem::path path;
  uint64_t size{};
  size_t bytes_read{}, calls{};
  std::map<std::string, std::pair<uint64_t, uint64_t>, std::less<>> members;
  size_t Read(uint64_t at, void *output, size_t n) {
    if (at > size || n > size - at)
      return 0;
    std::ifstream stream(path, std::ios::binary);
    stream.seekg(static_cast<std::streamoff>(at));
    stream.read(static_cast<char *>(output), static_cast<std::streamsize>(n));
    bytes_read += static_cast<size_t>(stream.gcount());
    ++calls;
    return static_cast<size_t>(stream.gcount());
  }
  explicit NativeArchive(const char *root) {
    path = std::filesystem::path(root) / "region_common/ui/Field.arc";
    size = std::filesystem::file_size(path);
    std::array<uint8_t, 20> h{};
    Check(Read(0, h.data(), 20) == 20 && !std::memcmp(h.data(), "SARC", 4),
          "native SARC");
    Check(U16(h.data() + 6) == 0xfeff, "SARC byte order");
    const uint64_t data = U32(h.data() + 12), declared = U32(h.data() + 8),
                   off = U16(h.data() + 4);
    Check(declared <= size && data <= declared, "SARC range");
    std::array<uint8_t, 12> fat{};
    Check(Read(off, fat.data(), 12) == 12 &&
              !std::memcmp(fat.data(), "SFAT", 4),
          "SFAT");
    const size_t n = U16(fat.data() + 6);
    Check(n < 30000, "SARC count");
    std::vector<uint8_t> nodes(n * 16);
    Check(Read(off + 12, nodes.data(), nodes.size()) == nodes.size(),
          "SARC nodes");
    const uint64_t names = off + 12 + n * 16 + 8;
    for (size_t i = 0; i < n; ++i) {
      const auto *p = nodes.data() + i * 16;
      const uint32_t attr = U32(p + 4), first = U32(p + 8), last = U32(p + 12);
      Check(first <= last && data + last <= declared, "member bounds");
      if (!(attr >> 24))
        continue;
      const uint64_t name = names + uint64_t(attr & 0xffffff) * 4;
      Check(name < data, "name bounds");
      std::array<char, 256> text{};
      const size_t length = std::min<uint64_t>(255, data - name);
      Check(Read(name, text.data(), length) == length, "member name");
      members.emplace(std::string(text.data()),
                      std::pair{data + first, uint64_t(last - first)});
    }
  }
  size_t Asset(std::string_view path, uint64_t offset, void *out, size_t n) {
    constexpr std::string_view prefix = "region_common/ui/Field.arc#";
    if (!path.starts_with(prefix))
      return 0;
    path.remove_prefix(prefix.size());
    const auto it = members.find(path);
    if (it == members.end())
      return 0;
    if (!out && !n)
      return it->second.second;
    if (offset > it->second.second || n > it->second.second - offset)
      return 0;
    return Read(it->second.first + offset, out, n);
  }
  std::vector<uint8_t> Blob(std::string_view name) {
    std::string path = "region_common/ui/Field.arc#";
    path += name;
    const size_t n = Asset(path, 0, nullptr, 0);
    Check(n && n <= 128u * 1024, "fixture layout size");
    std::vector<uint8_t> b(n);
    Check(Asset(path, 0, b.data(), n) == n, "fixture layout read");
    return b;
  }
};
std::vector<echoes::MapPane> OriginalPanes(const std::vector<uint8_t> &b) {
  Check(b.size() >= 20 && !std::memcmp(b.data(), "FLYT", 4) &&
            U32(b.data() + 8) == 0x09000000,
        "original FLYT v9");
  std::vector<echoes::MapPane> out;
  std::vector<int> parents;
  int last = -1;
  const auto f = [&](size_t a) {
    Check(a + 4 <= b.size(), "FLYT float span");
    return std::bit_cast<float>(U32(b.data() + a));
  };
  const auto text = [&](size_t a, size_t cap) {
    Check(a + cap <= b.size(), "FLYT string span");
    size_t n = 0;
    while (n < cap && b[a + n])
      ++n;
    Check(n < cap, "terminated FLYT string");
    return std::string(reinterpret_cast<const char *>(b.data() + a), n);
  };
  for (size_t a = U16(b.data() + 6); a < b.size();) {
    Check(a + 8 <= b.size(), "section header");
    const auto n = U32(b.data() + a + 4);
    Check(n >= 8 && n <= b.size() - a, "section span");
    const auto *p = b.data() + a;
    if (!std::memcmp(p, "pas1", 4)) {
      Check(last >= 0, "parent push");
      parents.push_back(last);
    } else if (!std::memcmp(p, "pae1", 4)) {
      Check(!parents.empty(), "parent pop");
      parents.pop_back();
    } else if (!std::memcmp(p, "pan1", 4) || !std::memcmp(p, "bnd1", 4) ||
               !std::memcmp(p, "prt1", 4) || !std::memcmp(p, "pic1", 4) ||
               !std::memcmp(p, "txt1", 4)) {
      Check(n >= 0x54, "pane bytes");
      echoes::MapPane q;
      q.name = text(a + 12, 24);
      q.parent = parents.empty() ? -1 : parents.back();
      q.flags = p[8];
      q.origin = p[9];
      q.alpha = p[10];
      q.x = f(a + 0x2c);
      q.y = f(a + 0x30);
      q.z = f(a + 0x34);
      q.rotation_x = f(a + 0x38);
      q.rotation_y = f(a + 0x3c);
      q.rotation_z = f(a + 0x40);
      q.scale_x = f(a + 0x44);
      q.scale_y = f(a + 0x48);
      q.width = f(a + 0x4c);
      q.height = f(a + 0x50);
      if (!std::memcmp(p, "prt1", 4)) {
        Check(n >= 0x60, "part bytes");
        const auto props = U32(p + 0x54);
        const size_t off = 0x60 + size_t(props) * 40;
        Check(off < n, "part layout name span");
        q.part_layout = text(a + off, n - off);
      }
      out.push_back(q);
      last = int(out.size() - 1);
    }
    a += n;
  }
  Check(parents.empty(), "balanced native hierarchy");
  return out;
}
echoes::FieldMapGeometry Geometry(const char *root) {
  NativeArchive archive(root);
  echoes::FieldMapGeometry g;
  g.field_panes = OriginalPanes(archive.Blob("blyt/Field.bflyt"));
  g.attach_panes = OriginalPanes(archive.Blob("blyt/L_MapAttachPanes.bflyt"));
  return g;
}
} // namespace
int main() {
  {
    Memory m;
    World(m);
    const auto e = m.Sample();
    const auto s = ReadWorld(m, e);
    Check(s.valid && s.compatible &&
              s.xyz == std::array<float, 3>{198, 0, 72.5f} &&
              s.level_key == "Field" && s.source.holder == Holder &&
              s.source.payload == Payload && s.source.lock_word == Lock,
          "typed world observation");
    ++cases;
    const auto before = m.reads;
    Empty(ReadWorld(m, e, false));
    Check(m.reads == before, "wrong build performs no reads");
    Empty(ReadWorld(m, e, true, UINT64_MAX - 7));
    Empty(echoes::ReadPlayerWorld({}, Base, true, e));
  }
  const std::array<uint64_t, 18> offsets{
      0x344298,  0x2144828, 0x21d37b4, 0x21d37d0, 0x21d3850, 0x21448e4,
      0x2144a40, 0x2144b1c, 0x26198e8, 0x268e8bc, 0x261a1f8, 0x261a3f8,
      0x260b358, 0x260ba8c, 0x260e394, 0x260e3d0, 0x2619e58, 0x2619e80};
  for (auto offset : offsets) {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.bytes[Base + offset] ^= 1;
    Empty(ReadWorld(m, e));
  }
  for (auto word :
       {uint64_t(0), Lock | 1, Lock | 0x100, Lock | 0x10000, Lock | 0x1000000,
        Lock | (uint64_t(1) << 56), Lock | (uint64_t(1) << 57),
        uint64_t(0x0400000000000000), UINT64_MAX}) {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.Put(Holder + 16, word);
    Empty(ReadWorld(m, e));
  }
  for (auto at : {Base + 0x34bb9c0, Holder, Holder + 8, Holder + 32}) {
    for (auto value : {uint64_t(0), uint64_t(1), UINT64_MAX - 7}) {
      Memory m;
      World(m);
      const auto e = m.Sample();
      m.Put(at, value);
      Empty(ReadWorld(m, e));
    }
  }
  {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.Put(Holder + 32, Payload + 8);
    Empty(ReadWorld(m, e));
  }
  for (auto bits :
       {uint32_t(0x7fc00000), uint32_t(0x7f800000), uint32_t(0xff800000)}) {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.Put(Payload, bits, 4);
    Empty(ReadWorld(m, e));
  }
  for (auto at :
       {Base + 0x34bd718, Controller, Controller + 0x80, Handle, Handle + 0x90,
        Resource, Resource + 0x140, Resource + 0x148, LevelName, Save + 0x16b80,
        Base + 0x34b80b8, Registry + 0x78, Header + 0x20, List + 0x18}) {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.bytes[at] ^= 1;
    Empty(ReadWorld(m, e));
  }
  for (size_t visit : {size_t(2), size_t(3), size_t(4)}) {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.before = [&](uint64_t at, size_t n) {
      if (at == Payload && n == visit)
        m.bytes[Payload + 3] ^= 1;
    };
    Empty(ReadWorld(m, e));
  }
  {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.before = [&](uint64_t at, size_t n) {
      if (at == Payload && n == 3)
        m.Put(Holder + 16, Lock | 0x10000);
    };
    Empty(ReadWorld(m, e));
  }
  {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.before = [&](uint64_t at, size_t n) {
      if (at == Payload && n == 3)
        m.Put(Resource + 0x148, 4);
    };
    Empty(ReadWorld(m, e));
  }
  {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.before = [&](uint64_t at, size_t n) {
      if (at == Payload && n == 3)
        m.Put(List + 0x18, 8, 4);
    };
    Empty(ReadWorld(m, e));
  }
  {
    Memory m;
    World(m);
    auto e = m.Sample();
    e.current_level_valid = false;
    Empty(ReadWorld(m, e));
  }
  for (auto at : {Resource + 0x148, List + 0x18, Base + 0x34bb9c0}) {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.before = [&](uint64_t a, size_t n) {
      if (a == Payload && n == 4)
        m.bytes[at] ^= 1;
    };
    Empty(ReadWorld(m, e));
  }
  {
    Memory m;
    World(m);
    const auto e = m.Sample();
    m.reads = m.largest = 0;
    Check(ReadWorld(m, e).valid && m.reads < 512 && m.largest <= 256,
          "bounded world observation");
    ++cases;
  }
  const char *root = std::getenv("ECHOES_ROMFS");
  if (!root) {
    std::cout << cases
              << " reader checks passed; original asset fixture skipped (set "
                 "ECHOES_ROMFS)\n";
    return 77;
  }
  std::map<std::string, std::vector<uint8_t>, std::less<>> assets;
  for (const std::string path :
       {"region_common/level/Common.eclo",
        "region_common/datasheets/ui/UiMapPlayerPos.gsheet",
        "region_common/datasheets/ui/UiMapLevelDefine.gsheet"}) {
    assets[path] = File(std::filesystem::path(root) / path);
    Check(!assets[path].empty(), "original asset fixture present");
  }
  const echoes::ReadAssetFn read = [&](std::string_view key, uint64_t at,
                                       void *output, size_t n) {
    auto i = assets.find(key);
    if (i == assets.end())
      return size_t(0);
    const auto &b = i->second;
    if (!output && !n)
      return b.size();
    if (at > b.size() || n > b.size() - at)
      return size_t(0);
    std::memcpy(output, b.data() + at, n);
    return n;
  };
  echoes::FieldProjection projection;
  auto geometry = Geometry(root);
  Check(echoes::LoadFieldProjection(read, geometry, projection),
        "original Common.eclo and named GSHT schema load");
  ++cases;
  echoes::PlayerWorldSnapshot w;
  w.compatible = w.valid = true;
  w.level_key = "Field";
  w.xyz = {198, 0, 72.5f};
  const auto p = echoes::ProjectFieldPlayer(projection, w);
  Check(p.valid && !p.authored_alias &&
            std::abs(p.uv[0] - 0.32413663f) < 0.000001f &&
            std::abs(p.uv[1] - 0.19664585f) < 0.000001f,
        "raw Field native bounds/scale/anchor UV");
  ++cases;
  const auto point=echoes::ProjectFieldNativePoint(projection,p.native_layout);
  Check(point.valid && point.uv==p.uv,"native Field points share original terrain registration");
  Check(!echoes::ProjectFieldNativePoint({},p.native_layout).valid,"missing native Field geometry refuses");
  Check(!echoes::ProjectFieldNativePoint(projection,{1e7f,1e7f}).valid,"off-map native point refuses");
  Check(!echoes::ProjectFieldNativePoint(projection,{std::numeric_limits<float>::quiet_NaN(),0}).valid,
        "nonfinite native point refuses");
  const auto authored=echoes::ProjectFieldAuthoredWorld(projection,w.xyz);
  Check(authored.valid && authored.uv==p.uv,
        "authored Field world point shares native projection without player provenance");
  const auto offset=echoes::ProjectFieldAuthoredWorld(projection,w.xyz,{2.5f,.3f});
  const auto expected_offset=echoes::ProjectFieldNativePoint(projection,{p.native_layout[0]+2.5f,p.native_layout[1]+.3f});
  Check(offset.valid && offset.uv==expected_offset.uv && offset.uv!=authored.uv,
        "native entrance offsets add in layout XY before terrain registration");
  Check(!echoes::ProjectFieldAuthoredWorld(projection,w.xyz,{std::numeric_limits<float>::quiet_NaN(),0}).valid,
        "nonfinite entrance offset refuses");
  Check(!echoes::ProjectFieldAuthoredWorld({},w.xyz).valid,
        "authored point requires original Field metadata");
  Check(!echoes::ProjectFieldAuthoredWorld(projection,{1e7f,0,0}).valid,
        "authored point outside native Field room refuses");
  Check(!echoes::ProjectFieldAuthoredWorld(projection,{0,1e7f,0}).valid,
        "authored point respects all three native room dimensions");
  Check(!echoes::ProjectFieldAuthoredWorld(projection,{0,0,std::numeric_limits<float>::quiet_NaN()}).valid,
        "authored nonfinite world point refuses");
  {
    auto changed=geometry;
    for(auto& pane:changed.attach_panes)
      if(pane.name=="N_Land_00")pane.x=1;
    echoes::FieldProjection optional;
    Check(echoes::LoadFieldProjection(read,changed,optional),
          "unsupported optional landmark attachment preserves player geometry");
    Check(!echoes::ProjectFieldAuthoredWorld(optional,w.xyz).valid &&
            echoes::ProjectFieldPlayer(optional,w).valid,
          "authored marker attachment cannot borrow player registration");
  }
  ++cases;
  w.level_key = "DekuTreeArea";
  w.xyz = {0, 0, 23.3f};
  const auto alias = echoes::ProjectFieldPlayer(projection, w);
  Check(alias.valid && alias.authored_alias &&
            std::bit_cast<uint32_t>(alias.native_layout[0]) == 0xc35f5924u &&
            std::bit_cast<uint32_t>(alias.native_layout[1]) == 0x436bdf9fu,
        "native authored alias and special offset bit exact");
  w.level_key = "S_Dungeon018";
  Check(!echoes::ProjectFieldPlayer(projection, w).valid,
        "authored room variant hides ambiguous level");
  w.level_key = "L_Dungeon001";
  Check(!echoes::ProjectFieldPlayer(projection, w).valid,
        "unmapped/cleared branch unsupported");
  ++cases;
  w.level_key = "Field";
  w.xyz[0] = std::numeric_limits<float>::infinity();
  Check(!echoes::ProjectFieldPlayer(projection, w).valid,
        "nonfinite projection rejected");
  ++cases;
  w.xyz = {1000000, 0, 0};
  Check(!echoes::ProjectFieldPlayer(projection, w).valid,
        "outside native containing room rejected");
  ++cases;
  auto malformed = geometry;
  for (auto &pane : malformed.attach_panes)
    if (pane.name == "N_Player_00")
      pane.x = 1;
  Check(!echoes::LoadFieldProjection(read, malformed, projection),
        "changed attach geometry fails closed");
  Check(!echoes::ProjectFieldPlayer(projection, w).valid,
        "failed load clears prior projection");
  ++cases;
  malformed = geometry;
  for (auto &pane : malformed.field_panes)
    if (pane.name == "B_MapAreaTop_00")
      pane.width = 0;
  Check(!echoes::LoadFieldProjection(read, malformed, projection),
        "zero native pane width rejected");
  ++cases;
  malformed = geometry;
  for (size_t i = 0; i < malformed.field_panes.size(); ++i)
    if (malformed.field_panes[i].name == "L_Attach_00")
      malformed.field_panes[i].parent = int(i);
  Check(!echoes::LoadFieldProjection(read, malformed, projection),
        "attach parent cycle rejected");
  ++cases;
  for (const auto &key :
       {std::string("region_common/level/Common.eclo"),
        std::string("region_common/datasheets/ui/UiMapPlayerPos.gsheet")}) {
    auto saved = assets[key];
    for (size_t off : {size_t(0), size_t(8), size_t(24)}) {
      assets[key] = saved;
      assets[key][off] ^= 0xff;
      Check(!echoes::LoadFieldProjection(read, geometry, projection),
            "malformed original header fails closed");
      ++cases;
    }
    assets[key] = saved;
  }
  {
    auto &b = assets["region_common/level/Common.eclo"];
    const auto saved = b;
    for (size_t off : {size_t(0x10), size_t(0x18), size_t(0x20), size_t(0x78),
                       size_t(0x80)}) {
      b = saved;
      std::fill_n(b.begin() + off, 4, 0xff);
      Check(!echoes::LoadFieldProjection(read, geometry, projection),
            "original directory extent rejected");
      ++cases;
    }
    b = saved;
  }
  {
    auto &b = assets["region_common/datasheets/ui/UiMapPlayerPos.gsheet"];
    const auto saved = b;
    for (size_t off : {size_t(48 + 16), size_t(96 + 18), size_t(144 + 20),
                       size_t(192 + 24)}) {
      b = saved;
      b[off] ^= 1;
      Check(!echoes::LoadFieldProjection(read, geometry, projection),
            "named GSHT descriptor mismatch rejected");
      ++cases;
    }
    b = saved;
  }
  {
    auto &b = assets["region_common/datasheets/ui/UiMapPlayerPos.gsheet"];
    const auto saved = b;
    const size_t row = U32(b.data() + 32), position = U32(b.data() + row + 16);
    for (size_t off : {row + 16, row + 24}) {
      b = saved;
      std::fill_n(b.begin() + off, 8, 0xff);
      Check(!echoes::LoadFieldProjection(read, geometry, projection),
            "alias array range/count overflow rejected");
      ++cases;
    }
    b = saved;
    b[position + 6] = 0xc0;
    b[position + 7] = 0x7f;
    Check(!echoes::LoadFieldProjection(read, geometry, projection),
          "alias coordinate NaN rejected");
    ++cases;
    b = saved;
    std::copy_n(b.begin() + row, 16, b.begin() + row + 32);
    Check(!echoes::LoadFieldProjection(read, geometry, projection),
          "duplicate alias keys rejected");
    ++cases;
    b = saved;
    // Existing room alias outranks a raw alias. Reuse original string storage.
    for (size_t n = 0; n < U32(b.data() + 40); ++n) {
      const size_t a = row + n * 32, p = U32(b.data() + a),
                   len = U32(b.data() + a + 8);
      const std::string key(reinterpret_cast<const char *>(b.data() + p), len);
      if (key == "L_DungeonCastle_023") {
        const std::string replacement = "DekuTreeArea_001";
        std::copy(replacement.begin(), replacement.end(), b.begin() + p);
        std::fill_n(b.begin() + a + 8, 8, 0);
        b[a + 8] = uint8_t(replacement.size());
        break;
      }
    }
    Check(echoes::LoadFieldProjection(read, geometry, projection),
          "authored room-alias variant remains parseable");
    w.level_key = "DekuTreeArea";
    w.xyz = {0, 0, 23.3f};
    Check(!echoes::ProjectFieldPlayer(projection, w).valid,
          "earlier authored alias prevents raw alias projection");
    ++cases;
    b = saved;
  }
  {
    auto &b = assets["region_common/datasheets/ui/UiMapLevelDefine.gsheet"];
    const auto saved = b;
    for (size_t off : {size_t(8), size_t(48 + 16), size_t(96 + 20)}) {
      b = saved;
      b[off] ^= 1;
      Check(!echoes::LoadFieldProjection(read, geometry, projection),
            "mapped level schema mismatch rejected");
      ++cases;
    }
    b = saved;
  }
  std::cout << cases
            << " guarded reader and original asset projection checks passed\n";
}
