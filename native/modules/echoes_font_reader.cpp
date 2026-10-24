// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_font_reader.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
namespace echoes {
namespace {
constexpr std::string_view Standard = "nintendo_udsg-r_std_003";
uint16_t BE16(const uint8_t *p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
uint32_t BE32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
uint32_t LE32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t LE64(const uint8_t *p) {
  return LE32(p) | uint64_t(LE32(p + 4)) << 32;
}
bool Read(const ReadFn &r, uint64_t base, uint64_t offset, void *out,
          size_t n) {
  if (!base || offset > UINT64_MAX - base || n > UINT64_MAX - (base + offset))
    return false;
  return !n || r(base + offset, out, n);
}
bool Ptr(const ReadFn &r, uint64_t b, uint64_t o, uint64_t &v,
         bool nullable = false, bool aligned = true) {
  std::array<uint8_t, 8> a{};
  if (b % 8 || o % 8 || !Read(r, b, o, a.data(), a.size()))
    return false;
  v = LE64(a.data());
  return (nullable || v) && (!aligned || v % 8 == 0);
}
struct Locale {
  uint64_t object{};
  uint32_t flag{};
  std::string root;
  bool supported{};
  bool operator==(const Locale &) const = default;
};
bool GetLocale(const ReadFn &r, uint64_t b, Locale &l) {
  uint64_t vt{};
  std::array<uint8_t, 4> a{};
  if (b > UINT64_MAX - 0x34bf1b0 || !Ptr(r, b, 0x34aa570, l.object) ||
      !Ptr(r, l.object, 8, vt) || vt != b + 0x32ae560 ||
      !Read(r, l.object, 0x20, a.data(), 4))
    return false;
  l.flag = LE32(a.data());
  constexpr std::array<std::string_view, 15> roots{
      "regionJP/JPja", "regionUS/USen", "regionUS/USfr", "regionUS/USes",
      "regionUS/USpt", "regionEU/EUen", "regionEU/EUfr", "regionEU/EUes",
      "regionEU/EUde", "regionEU/EUit", "regionEU/EUnl", "regionEU/EUru",
      "regionCN/CNzh", "regionTW/TWzh", "regionKR/KRko"};
  for (size_t i = 0; i < roots.size(); ++i)
    if (l.flag == (uint32_t(1) << i)) {
      l.root = roots[i];
      l.supported = i >= 1 && i <= 11;
      return true;
    }
  return false;
}
struct Table {
  uint32_t tag{}, checksum{}, offset{}, length{};
  bool operator==(const Table &) const = default;
};
struct Font {
  NativeFontFace face;
  std::vector<uint8_t> directory, name;
  std::vector<Table> tables;
  size_t size{};
};
bool Names(Font &f) {
  const auto &a = f.name;
  if (a.size() < 6 || BE16(a.data()) != 0)
    return false;
  size_t count = BE16(a.data() + 2), strings = BE16(a.data() + 4);
  if (count > 512 || 6 + count * 12 > a.size() || strings < 6 + count * 12 ||
      strings > a.size())
    return false;
  std::string family;
  for (size_t i = 0; i < count; ++i) {
    const auto *p = a.data() + 6 + i * 12;
    const size_t length = BE16(p + 8), offset = BE16(p + 10);
    if (offset > a.size() - strings || length > a.size() - strings - offset)
      return false;
    if (BE16(p + 6) != 1)
      continue;
    std::string s;
    const auto *q = a.data() + strings + offset;
    if (BE16(p) == 0 || BE16(p) == 3) {
      if (length % 2)
        return false;
      // Native Standard family is ASCII encoded as UTF-16BE. Other face names
      // are metadata only; non-ASCII records cannot authorize Standard.
      for (size_t j = 0; j < length; j += 2) {
        auto c = BE16(q + j);
        if (c > 127 || !c) {
          s.clear();
          break;
        }
        s.push_back(char(c));
      }
    } else if (BE16(p) == 1) {
      for (size_t j = 0; j < length; ++j) {
        if (!q[j] || q[j] > 127) {
          s.clear();
          break;
        }
        s.push_back(char(q[j]));
      }
    }
    if (!s.empty()) {
      if (!family.empty() && family != s)
        return false;
      family = std::move(s);
    }
  }
  f.face.family = std::move(family);
  return true;
}
bool Describe(const ReadFn &r, Font &f) {
  std::array<uint8_t, 12> h{};
  if (!Read(r, f.face.bytes, 0, h.data(), h.size()) ||
      (BE32(h.data()) != 0x10000 && BE32(h.data()) != 0x4f54544f))
    return false;
  size_t count = BE16(h.data() + 4);
  if (!count || count > 64)
    return false;
  f.directory.resize(12 + count * 16);
  if (!Read(r, f.face.bytes, 0, f.directory.data(), f.directory.size()) ||
      !std::equal(h.begin(), h.end(), f.directory.begin()))
    return false;
  f.size = f.directory.size();
  std::set<uint32_t> tags;
  for (size_t i = 0; i < count; ++i) {
    const auto *p = f.directory.data() + 12 + i * 16;
    Table t{BE32(p), BE32(p + 4), BE32(p + 8), BE32(p + 12)};
    if (!tags.insert(t.tag).second || t.offset > MaxNativeFontBytes ||
        t.length > MaxNativeFontBytes - t.offset ||
        (t.length && (t.offset < f.directory.size() || t.offset % 4)))
      return false;
    for (const auto &old : f.tables)
      if (t.length && old.length &&
          uint64_t(t.offset) < uint64_t(old.offset) + old.length &&
          uint64_t(old.offset) < uint64_t(t.offset) + t.length)
        return false;
    f.tables.push_back(t);
    f.size = std::max(f.size, size_t(t.offset) + t.length);
    if (t.tag == 0x6e616d65) {
      if (t.length > 65536)
        return false;
      f.name.resize(t.length);
      if (!Read(r, f.face.bytes, t.offset, f.name.data(), f.name.size()))
        return false;
    }
  }
  return !f.name.empty() && Names(f);
}
struct Node {
  uint64_t at{}, resource{}, raw{};
  std::array<uint8_t, 32> bytes{};
  bool operator==(const Node &) const = default;
};
struct Chain {
  uint64_t engine{}, head{};
  std::vector<Node> nodes;
  bool operator==(const Chain &) const = default;
};
bool GetChain(const ReadFn &r, uint64_t b, Chain &c) {
  if (b > UINT64_MAX - 0x34bf1b0 || !Ptr(r, b + 0x34bf1b0, 8, c.engine) ||
      !Ptr(r, c.engine, 8, c.head, true))
    return false;
  uint64_t n = c.head;
  std::set<uint64_t> seen;
  while (n) {
    if (n % 8 || c.nodes.size() == 16 || !seen.insert(n).second)
      return false;
    Node v;
    v.at = n;
    if (!Read(r, n, 0, v.bytes.data(), 32))
      return false;
    v.resource = LE64(v.bytes.data() + 8);
    if (!v.resource || v.resource % 8 ||
        !Ptr(r, v.resource, 0x10, v.raw, false, false))
      return false;
    n = LE64(v.bytes.data() + 24);
    c.nodes.push_back(v);
  }
  return true;
}
uint32_t Checksum(const uint8_t *p, size_t n, bool head = false) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i += 4) {
    uint32_t v = 0;
    for (size_t j = 0; j < 4; ++j) {
      v <<= 8;
      if (i + j < n && !(head && i + j >= 8 && i + j < 12))
        v |= p[i + j];
    }
    sum += v;
  }
  return sum;
}
bool Verify(const Font &f, const std::vector<uint8_t> &bytes) {
  if (BE32(bytes.data()) != 0x10000 || bytes.size() != f.size ||
      !std::equal(f.directory.begin(), f.directory.end(), bytes.begin()))
    return false;
  bool head = false, cmap = false;
  for (const auto &t : f.tables) {
    if (t.tag == 0x68656164) {
      if (t.length < 54 || BE32(bytes.data() + t.offset + 12) != 0x5f0f3cf5)
        return false;
      head = true;
    }
    if (t.tag == 0x636d6170)
      cmap = t.length >= 4;
    if (t.tag == 0x6e616d65 &&
        !std::equal(f.name.begin(), f.name.end(), bytes.begin() + t.offset))
      return false;
    if (Checksum(bytes.data() + t.offset, t.length, t.tag == 0x68656164) !=
        t.checksum)
      return false;
  }
  return head && cmap && Checksum(bytes.data(), bytes.size()) == 0xb1b0afba;
}
bool SameDescription(const Font &a, const Font &b) {
  return a.directory == b.directory && a.name == b.name &&
         a.face.family == b.face.family;
}
} // namespace
bool ValidateStandardFontBytes(const std::vector<uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > MaxNativeFontBytes) return false;
  constexpr uint64_t base = 0x1000;
  const ReadFn read = [&](uint64_t at, void* output, size_t n) {
    if (at < base || at - base > bytes.size() || n > bytes.size() - (at - base))
      return false;
    std::memcpy(output, bytes.data() + (at - base), n);
    return true;
  };
  Font font;
  font.face.bytes = base;
  return Describe(read, font) && font.face.family == Standard &&
         font.size == bytes.size() && Verify(font, bytes);
}

NativeFonts ReadNativeFonts(const ReadFn &r, uint64_t b, bool build_matches) {
  NativeFonts out;
  if (!build_matches || !r || !b || b % 8)
    return out;
  auto guards = [&](const auto &list) {
    for (const auto &g : list) {
      std::array<uint8_t, 256> v{};
      if (!Read(r, b, g.offset, v.data(), v.size()) || !MatchSupportedGuard(v, g.bytes, g.offset))
        return false;
    }
    return true;
  };
  if (!guards(IdentityGuards) || !guards(FontIdentityGuards))
    return out;
  out.compatible = true;
  Locale l, last;
  if (!GetLocale(r, b, l))
    return out;
  auto publish_locale = [&]() {
    out.locale_valid = true;
    out.locale_flag = l.flag;
    out.message_root = l.root;
    out.body_font_supported = l.supported;
    out.locale_object = l.object;
  };
  if (!l.supported) {
    if (GetLocale(r, b, last) && l == last)
      publish_locale();
    return out;
  }
  Chain chain, final;
  if (!GetChain(r, b, chain)) {
    if (GetLocale(r, b, last) && l == last)
      publish_locale();
    return out;
  }
  std::vector<Font> fonts;
  size_t chosen = 16;
  bool valid = true;
  for (const auto &n : chain.nodes) {
    Font f;
    f.face.node = n.at;
    f.face.resource = n.resource;
    f.face.bytes = n.raw;
    if (!Describe(r, f)) {
      valid = false;
      break;
    }
    if (f.face.family == Standard) {
      if (chosen != 16) {
        valid = false;
        break;
      }
      chosen = fonts.size();
    }
    fonts.push_back(std::move(f));
  }
  std::vector<uint8_t> copy;
  if (valid && chosen != 16) {
    const auto &f = fonts[chosen];
    copy.resize(f.size);
    valid =
        Read(r, f.face.bytes, 0, copy.data(), copy.size()) && Verify(f, copy);
  } else
    valid = false;
  if (valid)
    for (const auto &f : fonts) {
      Font check;
      check.face = f.face;
      if (!Describe(r, check) || !SameDescription(f, check)) {
        valid = false;
        break;
      }
    }
  if (!GetLocale(r, b, last) || l != last)
    return out;
  publish_locale();
  if (!valid || !GetChain(r, b, final) || chain != final)
    return out;
  out.engine = chain.engine;
  out.list_head = chain.head;
  for (const auto &f : fonts)
    out.faces.push_back(f.face);
  out.standard_font_address = fonts[chosen].face.bytes;
  out.standard_font = std::move(copy);
  out.standard_font_valid = true;
  return out;
}
} // namespace echoes
