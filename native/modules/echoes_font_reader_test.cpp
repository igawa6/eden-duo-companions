// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_font_reader.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
namespace {
constexpr uint64_t Base = 0x10000000, Locale = 0x20000000, Engine = 0x30000000,
                   Node = 0x40000000, Resource = 0x50000000, Blob = 0x60000004;
void B16(std::vector<uint8_t> &a, size_t o, uint16_t n) {
  a[o] = uint8_t(n >> 8);
  a[o + 1] = uint8_t(n);
}
void B32(std::vector<uint8_t> &a, size_t o, uint32_t n) {
  for (size_t i = 0; i < 4; ++i)
    a[o + i] = uint8_t(n >> (24 - i * 8));
}
uint32_t Sum(const std::vector<uint8_t> &a, size_t start, size_t length) {
  uint32_t sum = 0;
  for (size_t i = 0; i < length; i += 4) {
    uint32_t v = 0;
    for (size_t j = 0; j < 4; ++j) {
      v <<= 8;
      if (i + j < length)
        v |= a[start + i + j];
    }
    sum += v;
  }
  return sum;
}
std::vector<uint8_t> Font(std::string family = "nintendo_udsg-r_std_003") {
  // Three independent tables; no glyph coverage is asserted by this fixture.
  const size_t name_length = 18 + family.size() * 2;
  std::vector<uint8_t> a(120 + name_length);
  B32(a, 0, 0x10000);
  B16(a, 4, 3);
  B16(a, 6, 32);
  B16(a, 8, 1);
  B16(a, 10, 16);
  B32(a, 12, 0x68656164);
  B32(a, 20, 60);
  B32(a, 24, 54);
  B32(a, 28, 0x636d6170);
  B32(a, 36, 116);
  B32(a, 40, 4);
  B32(a, 44, 0x6e616d65);
  B32(a, 52, 120);
  B32(a, 56, uint32_t(name_length));
  B32(a, 72, 0x5f0f3cf5);
  B16(a, 122, 1);
  B16(a, 124, 18);
  B16(a, 126, 3);
  B16(a, 128, 1);
  B16(a, 130, 0x409);
  B16(a, 132, 1);
  B16(a, 134, uint16_t(family.size() * 2));
  for (size_t i = 0; i < family.size(); ++i)
    a[139 + i * 2] = uint8_t(family[i]);
  B32(a, 16, Sum(a, 60, 54));
  B32(a, 32, Sum(a, 116, 4));
  B32(a, 48, Sum(a, 120, name_length));
  B32(a, 68, 0xb1b0afba - Sum(a, 0, a.size()));
  return a;
}
struct Memory {
  std::map<uint64_t, std::vector<uint8_t>> ranges;
  std::function<void(uint64_t, size_t)> before;
  void Put(uint64_t a, const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    ranges[a] = std::vector<uint8_t>(b, b + n);
  }
  void U64(uint64_t a, uint64_t v) {
    std::array<uint8_t, 8> x{};
    for (size_t i = 0; i < 8; ++i)
      x[i] = uint8_t(v >> (8 * i));
    Put(a, x.data(), 8);
  }
  void U32(uint64_t a, uint32_t v) {
    std::array<uint8_t, 4> x{};
    for (size_t i = 0; i < 4; ++i)
      x[i] = uint8_t(v >> (8 * i));
    Put(a, x.data(), 4);
  }
  bool Read(uint64_t a, void *p, size_t n) {
    if (before)
      before(a, n);
    auto it = ranges.upper_bound(a);
    if (it == ranges.begin())
      return false;
    --it;
    if (a - it->first > it->second.size() ||
        n > it->second.size() - size_t(a - it->first))
      return false;
    std::memcpy(p, it->second.data() + size_t(a - it->first), n);
    return true;
  }
  echoes::NativeFonts Run(bool matches = true, uint64_t base = Base) {
    return echoes::ReadNativeFonts(
        [&](uint64_t a, void *p, size_t n) { return Read(a, p, n); }, base,
        matches);
  }
  Memory() {
    for (const auto &g : echoes::IdentityGuards)
      Put(Base + g.offset, g.bytes.data(), g.bytes.size());
    for (const auto &g : echoes::FontIdentityGuards)
      Put(Base + g.offset, g.bytes.data(), g.bytes.size());
    U64(Base + 0x34aa570, Locale);
    U64(Locale + 8, Base + 0x32ae560);
    U32(Locale + 0x20, 2);
    U64(Base + 0x34bf1b8, Engine);
    U64(Engine + 8, Node);
    std::array<uint8_t, 32> node{};
    for (size_t i = 0; i < 8; ++i)
      node[8 + i] = uint8_t(Resource >> (8 * i));
    Put(Node, node.data(), node.size());
    U64(Resource + 0x10, Blob);
    auto f = Font();
    Put(Blob, f.data(), f.size());
  }
  void Next(uint64_t n) {
    auto &a = ranges.at(Node);
    for (size_t i = 0; i < 8; ++i)
      a[24 + i] = uint8_t(n >> (8 * i));
  }
  // Inserts a face at the list head, as the native loader does.
  void Push(const std::string &family) {
    const uint64_t at = Node + 64 * ++pushed, resource = Resource + 64 * pushed,
                   blob = Blob + 4096 * pushed;
    std::array<uint8_t, 32> node{};
    uint64_t head{};
    Read(Engine + 8, &head, 8);
    for (size_t i = 0; i < 8; ++i) {
      node[8 + i] = uint8_t(resource >> (8 * i));
      node[24 + i] = uint8_t(head >> (8 * i));
    }
    Put(at, node.data(), node.size());
    U64(resource + 0x10, blob);
    auto f = Font(family);
    Put(blob, f.data(), f.size());
    U64(Engine + 8, at);
  }
  size_t pushed = 0;
};
size_t cases = 0;
void Check(bool value) {
  ++cases;
  if (!value) {
    std::cerr << "failed case " << cases << '\n';
    std::abort();
  }
}
void BadFont(const std::function<void(Memory &)> &mutate) {
  Memory m;
  mutate(m);
  auto s = m.Run();
  Check(s.compatible && s.locale_valid && !s.standard_font_valid &&
        s.standard_font.empty());
}
} // namespace
int main() {
  {
    Memory m;
    auto s = m.Run();
    Check(s.compatible && s.locale_valid && s.body_font_supported &&
          s.standard_font_valid && s.standard_font == Font() &&
          s.standard_font_address == Blob && !s.glyph_coverage_verified);
  }
  {
    Memory m;
    Check(!m.Run(false).compatible);
    Check(!m.Run(true, 0).compatible);
    Check(!m.Run(true, UINT64_MAX - 7).compatible);
  }
  {
    Memory m;
    Check(!echoes::ReadNativeFonts({}, Base, true).compatible);
  }
  for (size_t i = 0; i < 5; ++i) {
    Memory m;
    uint64_t a = i < 3 ? echoes::IdentityGuards[i].offset
                       : echoes::FontIdentityGuards[i - 3].offset;
    m.ranges.at(Base + a)[0] ^= 1;
    Check(!m.Run().compatible);
  }
  constexpr std::array<std::string_view, 15> roots{
      "regionJP/JPja", "regionUS/USen", "regionUS/USfr", "regionUS/USes",
      "regionUS/USpt", "regionEU/EUen", "regionEU/EUfr", "regionEU/EUes",
      "regionEU/EUde", "regionEU/EUit", "regionEU/EUnl", "regionEU/EUru",
      "regionCN/CNzh", "regionTW/TWzh", "regionKR/KRko"};
  for (size_t i = 0; i < 15; ++i) {
    Memory m;
    m.U32(Locale + 0x20, uint32_t(1) << i);
    auto s = m.Run();
    // Chinese and Korean also need their own faces, absent here.
    bool supported = i <= 11;
    Check(s.locale_valid && s.message_root == roots[i] &&
          s.body_font_supported && s.standard_font_valid == supported &&
          (supported || s.standard_font.empty()) && s.locale_fonts.empty());
  }
  // The native list of all six shared faces, Standard loaded first.
  const auto shared = [](Memory &m) {
    for (auto family : {"nintendo_udsg-r_org_zh-cn_003",
                        "nintendo_udsg-r_ext_zh-cn_003",
                        "nintendo_udjxh-db_zh-tw_003", "nintendo_udsg-r_ko_003",
                        "NintendoExt003"})
      m.Push(family);
  };
  for (auto [flag, count] : {std::pair{0x1000u, 2u}, std::pair{0x2000u, 1u},
                             std::pair{0x4000u, 1u}, std::pair{0x2u, 0u},
                             std::pair{0x1u, 0u}}) {
    Memory m;
    shared(m);
    m.U32(Locale + 0x20, flag);
    auto s = m.Run();
    const auto families = echoes::LocaleFontFamilies(flag);
    Check(s.standard_font_valid && s.faces.size() == 6 &&
          families.size() == count && s.locale_fonts.size() == count);
    for (size_t i = 0; i < count; ++i)
      Check(s.locale_fonts[i].family == families[i] &&
            echoes::ValidateNativeFontBytes(s.locale_fonts[i].bytes,
                                            families[i]) &&
            !echoes::ValidateStandardFontBytes(s.locale_fonts[i].bytes));
  }
  Check(echoes::LocaleFontFamilies(0x1000)[0] == "nintendo_udsg-r_org_zh-cn_003" &&
        echoes::LocaleFontFamilies(0x1000)[1] == "nintendo_udsg-r_ext_zh-cn_003" &&
        echoes::LocaleFontFamilies(0x2000)[0] == "nintendo_udjxh-db_zh-tw_003" &&
        echoes::LocaleFontFamilies(0x4000)[0] == "nintendo_udsg-r_ko_003");
  // Without firmware every slot holds a replacement face; the Standard slot is
  // the list tail (loaded first). Native Standard always wins.
  const auto synthesized = [](Memory &m, bool broken_middle) {
    auto f = Font("Open Sans");
    m.Put(Blob, f.data(), f.size());
    for (auto family : {"Open Sans", "Source Sans Pro", "Open Sans",
                        "Open Sans", "YuzuOSSExtension"})
      m.Push(family);
    if (broken_middle)
      B16(m.ranges.at(Blob + 4096 * 3), 134, 0xffff);
  };
  for (auto [flag, broken] : {std::pair{0x2u, false}, std::pair{0x40u, true},
                              std::pair{0x800u, false}}) {
    Memory m;
    synthesized(m, broken);
    m.U32(Locale + 0x20, flag);
    auto s = m.Run();
    Check(s.standard_font_valid && s.fallback && s.standard_family == "Open Sans" &&
          s.standard_font_address == Blob && s.locale_fonts.empty() &&
          s.problem.empty() && echoes::ValidateNativeFontBytes(s.standard_font, "Open Sans") &&
          !echoes::ValidateStandardFontBytes(s.standard_font));
  }
  for (auto flag : {0x1u, 0x1000u, 0x2000u, 0x4000u}) {
    Memory m;
    synthesized(m, false);
    m.U32(Locale + 0x20, flag);
    auto s = m.Run();
    Check(s.locale_valid && !s.standard_font_valid && !s.fallback &&
          s.standard_font.empty() &&
          s.problem.find("no Japanese, Chinese or Korean glyphs") != std::string::npos);
  }
  {
    Memory m;
    shared(m);
    auto s = m.Run();
    Check(s.standard_font_valid && !s.fallback &&
          s.standard_family == "nintendo_udsg-r_std_003");
  }
  {
    // A corrupt replacement face, or a list that is not the six shared
    // fonts, publishes nothing.
    Memory m;
    synthesized(m, false);
    m.ranges.at(Blob)[100] ^= 1;
    auto s = m.Run();
    Check(!s.standard_font_valid && !s.fallback && !s.problem.empty());
    Memory five;
    auto f = Font("Open Sans");
    five.Put(Blob, f.data(), f.size());
    for (int i = 0; i < 4; ++i)
      five.Push("Open Sans");
    Check(!five.Run().standard_font_valid);
    Memory junk;
    synthesized(junk, false);
    std::vector<uint8_t> garbage(256, 0x5a);
    junk.Put(Blob, garbage.data(), garbage.size());
    Check(!junk.Run().standard_font_valid);
  }
  {
    // A missing Simplified Chinese extension face publishes no font.
    Memory m;
    m.Push("nintendo_udsg-r_org_zh-cn_003");
    m.U32(Locale + 0x20, 0x1000);
    auto s = m.Run();
    Check(s.locale_valid && !s.standard_font_valid && s.standard_font.empty() &&
          s.locale_fonts.empty());
  }
  {
    // Two faces with the Korean family are ambiguous.
    Memory m;
    m.Push("nintendo_udsg-r_ko_003");
    m.Push("nintendo_udsg-r_ko_003");
    m.U32(Locale + 0x20, 0x4000);
    Check(!m.Run().standard_font_valid);
  }
  {
    // A corrupt Korean face publishes no font.
    Memory m;
    m.Push("nintendo_udsg-r_ko_003");
    m.ranges.at(Blob + 4096)[100] ^= 1;
    m.U32(Locale + 0x20, 0x4000);
    Check(!m.Run().standard_font_valid);
  }
  for (auto flag : {0u, 3u, 0x8000u}) {
    Memory m;
    m.U32(Locale + 0x20, flag);
    auto s = m.Run();
    Check(!s.locale_valid && s.message_root.empty() && s.standard_font.empty());
  }
  {
    Memory m;
    m.U64(Locale + 8, Base + 0x32ae568);
    Check(!m.Run().locale_valid);
  }
  {
    Memory m;
    m.U64(Base + 0x34aa570, 0);
    Check(!m.Run().locale_valid);
  }
  {
    Memory m;
    int reads = 0;
    m.before = [&](uint64_t a, size_t) {
      if (a == Locale + 0x20 && ++reads == 2)
        m.U32(Locale + 0x20, 0x20);
    };
    auto s = m.Run();
    Check(!s.locale_valid && s.standard_font.empty());
  }
  BadFont([](Memory &m) { m.U64(Base + 0x34bf1b8, 0); });
  BadFont([](Memory &m) { m.U64(Engine + 8, Node + 4); });
  BadFont([](Memory &m) { m.U64(Resource + 0x10, 0); });
  BadFont([](Memory &m) { m.Next(Node); });
  BadFont([](Memory &m) { m.ranges.erase(Blob); });
  BadFont([](Memory &m) { m.ranges.at(Blob)[4] = 1; });
  BadFont([](Memory &m) { B32(m.ranges.at(Blob), 24, 0xffffffff); });
  BadFont([](Memory &m) { B32(m.ranges.at(Blob), 36, 64); });
  BadFont([](Memory &m) { m.ranges.at(Blob)[100] ^= 1; });
  BadFont([](Memory &m) { m.ranges.at(Blob)[68] ^= 1; });
  BadFont([](Memory &m) { m.ranges.at(Blob)[16] ^= 1; });
  BadFont([](Memory &m) {
    auto f = Font("nintendo_udsg-r_std_003_extra");
    m.Put(Blob, f.data(), f.size());
  });
  BadFont([](Memory &m) { B16(m.ranges.at(Blob), 134, 0xffff); });
  BadFont([](Memory &m) {
    auto &bytes = m.ranges.at(Node);
    auto second = bytes;
    for (size_t i = 0; i < 8; ++i)
      bytes[24 + i] = uint8_t((Node + 64) >> (8 * i));
    m.Put(Node + 64, second.data(), 32);
  });
  BadFont([](Memory &m) {
    auto count = std::make_shared<int>(0);
    m.before = [&m, count](uint64_t a, size_t) {
      if (a == Node && ++*count == 2)
        m.Next(Node);
    };
  });
  BadFont([](Memory &m) {
    auto count = std::make_shared<int>(0);
    m.before = [&m, count](uint64_t a, size_t) {
      if (a == Resource + 0x10 && ++*count == 2)
        m.U64(a, Blob + 4);
    };
  });
  BadFont([](Memory &m) {
    auto count = std::make_shared<int>(0);
    m.before = [&m, count](uint64_t a, size_t n) {
      if (a == Blob && n == 12 && ++*count == 2)
        m.ranges.at(Blob)[10] ^= 1;
    };
  });
  {
    Memory m;
    auto f = Font();
    m.ranges.erase(Blob);
    m.Put(Blob + 1, f.data(), f.size());
    m.U64(Resource + 0x10, Blob + 1);
    Check(m.Run().standard_font_valid);
  }
  // A different family at the list head must not hide Standard later in the
  // list.
  {
    Memory m;
    auto old = m.ranges.at(Node);
    auto head = old;
    for (size_t i = 0; i < 8; ++i) {
      head[8 + i] = uint8_t((Resource + 64) >> (8 * i));
      head[24 + i] = uint8_t(Node >> (8 * i));
    }
    m.Put(Node + 64, head.data(), 32);
    m.U64(Engine + 8, Node + 64);
    m.U64(Resource + 64 + 0x10, Blob + 4096);
    auto f = Font("NintendoExt003");
    m.Put(Blob + 4096, f.data(), f.size());
    auto s = m.Run();
    Check(s.standard_font_valid && s.faces.size() == 2 &&
          s.standard_font_address == Blob);
  }
  BadFont([](Memory &m) {
    auto count = std::make_shared<int>(0);
    m.before = [&m, count](uint64_t a, size_t) {
      if (a == Engine + 8 && ++*count == 2)
        m.U64(a, 0);
    };
  });
  BadFont([](Memory &m) {
    auto count = std::make_shared<int>(0);
    m.before = [&m, count](uint64_t a, size_t) {
      if (a == Base + 0x34bf1b8 && ++*count == 2)
        m.U64(a, Engine + 64);
    };
  });
  BadFont([](Memory &m) {
    for (size_t i = 0; i < 17; ++i) {
      auto node = m.ranges.at(Node);
      uint64_t next = i == 16 ? 0 : Node + (i + 1) * 64;
      for (size_t j = 0; j < 8; ++j)
        node[24 + j] = uint8_t(next >> (8 * j));
      m.Put(Node + i * 64, node.data(), 32);
    }
  });
  {
    Memory m;
    for (size_t i = 0; i < 16; ++i) {
      auto node = m.ranges.at(Node);
      uint64_t next = i == 15 ? 0 : Node + (i + 1) * 64;
      uint64_t resource = i == 15 ? Resource : Resource + 64;
      for (size_t j = 0; j < 8; ++j) {
        node[24 + j] = uint8_t(next >> (8 * j));
        node[8 + j] = uint8_t(resource >> (8 * j));
      }
      m.Put(Node + i * 64, node.data(), 32);
    }
    m.U64(Resource + 64 + 0x10, Blob + 4096);
    auto f = Font("NintendoExt003");
    m.Put(Blob + 4096, f.data(), f.size());
    Check(m.Run().standard_font_valid);
  }
  {
    Memory m;
    auto first = m.Run();
    m.U64(Engine + 8, 0);
    auto second = m.Run();
    Check(first.standard_font_valid && !second.standard_font_valid &&
          second.standard_font.empty());
  }
  if (const char *path = std::getenv("ECHOES_TEST_FONT")) {
    std::ifstream in(path, std::ios::binary);
    Check(bool(in));
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});
    Memory m;
    m.Put(Blob, f.data(), f.size());
    auto s = m.Run();
    Check(s.standard_font_valid && s.standard_font.size() <= f.size());
    std::cout << "Actual font validated: " << s.standard_font.size()
              << " bytes\n";
  }
  std::cout << cases << " native font reader checks passed\n";
}
