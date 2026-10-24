// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_code_guard.h"
#include "echoes_player.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace echoes {
namespace {
struct Guard {
  uint64_t offset;
  std::array<uint8_t, 16> bytes;
};
constexpr std::array<Guard, 18> Guards{{
    {0x344298,
     {0x89, 0x84, 0x01, 0x90, 0x29, 0xa1, 0x13, 0x91, 0xb6, 0x8b, 0x01, 0xf0,
      0x09, 0x50, 0x00, 0xa9}},
    {0x2144828,
     {0xa8, 0x9b, 0x00, 0xf0, 0x14, 0xe1, 0x44, 0xf9, 0x14, 0x1a, 0x00, 0xb4,
      0x98, 0x42, 0x00, 0x91}},
    {0x21d37b4,
     {0x88, 0xfe, 0x5f, 0xc8, 0x08, 0x41, 0x40, 0x91, 0x88, 0xfe, 0x09, 0xc8,
      0xa9, 0xff, 0xff, 0x35}},
    {0x21d37d0,
     {0x08, 0x01, 0x15, 0x8a, 0x09, 0xc1, 0x7f, 0x91, 0x29, 0x05, 0x47, 0xd2,
      0x1f, 0x20, 0x03, 0xd5}},
    {0x21d3850,
     {0x68, 0x62, 0x00, 0x91, 0x00, 0xfd, 0x9f, 0xc8, 0x1f, 0x20, 0x03, 0xd5,
      0x1f, 0x20, 0x03, 0xd5}},
    {0x21448e4,
     {0x88, 0x06, 0x40, 0xf9, 0xa9, 0x06, 0x40, 0xf9, 0x09, 0x05, 0x00, 0xf9,
      0xea, 0x5b, 0x40, 0xf9}},
    {0x2144a40,
     {0x09, 0xff, 0xdf, 0xc8, 0xea, 0x9f, 0x68, 0xb2, 0x0c, 0x20, 0xa0, 0x52,
      0x28, 0x1d, 0x68, 0x92}},
    {0x2144b1c,
     {0xe9, 0x7f, 0x60, 0xb2, 0x08, 0xff, 0x5f, 0xc8, 0x0a, 0x01, 0x09, 0x8b,
      0x0a, 0xff, 0x0b, 0xc8}},
    {0x26198e8,
     {0x94, 0xe2, 0x44, 0xf9, 0xe1, 0x03, 0x1f, 0x2a, 0x60, 0xf2, 0x40, 0xf9,
      0x88, 0x06, 0x40, 0xf9}},
    {0x268e8bc,
     {0x2a, 0x81, 0x5f, 0xb8, 0x5f, 0x01, 0x14, 0x6b, 0x61, 0xff, 0xff, 0x54,
      0x20, 0xc1, 0x5f, 0xbc}},
    {0x261a1f8,
     {0x62, 0x4e, 0x41, 0xb9, 0xe1, 0xc3, 0x00, 0x91, 0xe0, 0x03, 0x14, 0xaa,
      0x77, 0xd1, 0x01, 0x94}},
    {0x261a3f8,
     {0xe0, 0x03, 0x13, 0xaa, 0xf1, 0xe7, 0xff, 0x97, 0x68, 0xad, 0x8f, 0x52,
      0x28, 0x52, 0xa7, 0x72}},
    {0x260b358,
     {0x82, 0x14, 0x00, 0xf0, 0x42, 0x7c, 0x35, 0x91, 0xe0, 0x83, 0x00, 0x91,
      0xe1, 0x03, 0x13, 0xaa}},
    {0x260ba8c,
     {0x68, 0x02, 0x40, 0xf9, 0xe0, 0x03, 0x13, 0xaa, 0x08, 0x75, 0x40, 0xf9,
      0x00, 0x01, 0x3f, 0xd6}},
    {0x260e394,
     {0x08, 0xbc, 0x40, 0xf9, 0x09, 0x00, 0x40, 0xf9, 0x29, 0x79, 0x40, 0xf9,
      0x08, 0x71, 0x40, 0xbd}},
    {0x260e3d0,
     {0x08, 0x60, 0x41, 0xf9, 0xa8, 0x00, 0x00, 0xb4, 0x00, 0x51, 0x40, 0xbd,
      0x08, 0x51, 0x01, 0x91}},
    {0x2619e58,
     {0x01, 0x09, 0x43, 0xad, 0x21, 0xd4, 0x22, 0x4e, 0xe2, 0x67, 0x05, 0x4f,
      0x08, 0xa4, 0x43, 0x39}},
    {0x2619e80,
     {0x22, 0x04, 0x14, 0x5e, 0x42, 0x40, 0x21, 0x1e, 0x23, 0x04, 0x0c, 0x5e,
      0x42, 0x0c, 0x23, 0x1e}},
}};
uint32_t U32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}
uint64_t U64(const uint8_t *p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }
float F32(const uint8_t *p) { return std::bit_cast<float>(U32(p)); }
bool Range(uint64_t a, uint64_t n) { return a && n <= UINT64_MAX - a; }
bool Bytes(const ReadFn &read, uint64_t a, void *p, size_t n) {
  return Range(a, n) && read(a, p, n);
}
bool Ptr(const ReadFn &read, uint64_t a, uint64_t &out) {
  std::array<uint8_t, 8> b{};
  if (a % 8 || !Bytes(read, a, b.data(), b.size()))
    return false;
  out = U64(b.data());
  return out && out % 8 == 0;
}
bool Vt(const ReadFn &r, uint64_t object, uint64_t base, uint64_t offset) {
  uint64_t v{};
  return Ptr(r, object, v) && v == base + offset;
}
bool Level(const ReadFn &r, uint64_t base, const MapSnapshot &expected) {
  const auto &s = expected.current_level_source;
  uint64_t controller{}, handle{}, resource{};
  std::array<uint8_t, 16> desc{};
  if (!s.controller || !s.handle || !s.resource || !s.key_pointer ||
      !Ptr(r, base + 0x34bd718, controller) || controller != s.controller ||
      !Vt(r, controller, base, 0x3269630) || !Range(controller, 0x88) ||
      !Ptr(r, controller + 0x80, handle) || handle != s.handle ||
      !Vt(r, handle, base, 0x3268098) || !Range(handle, 0x98) ||
      !Ptr(r, handle + 0x90, resource) || resource != s.resource ||
      !Vt(r, resource, base, 0x3268738) || !Range(resource, 0x150) ||
      !Bytes(r, resource + 0x140, desc.data(), desc.size()) ||
      U64(desc.data()) != s.key_pointer ||
      U64(desc.data() + 8) != expected.current_level_key.size())
    return false;
  std::string key(expected.current_level_key.size(), '\0');
  return Bytes(r, s.key_pointer, key.data(), key.size()) &&
         key == expected.current_level_key;
}
bool Epoch(const ReadFn &r, uint64_t base, const MapSnapshot &e) {
  const auto current = ReadSnapshot(r, base, true);
  if (!current.collection_valid || current.epoch_source != e.source.save_epoch)
    return false;
  uint64_t traversal{}, owner{}, header{}, entries{};
  std::array<uint8_t, 0x30> h{};
  return Range(current.epoch_source.save_data, 0x16b88) &&
         Ptr(r, current.epoch_source.save_data + 0x16b80, traversal) &&
         traversal == e.source.traversal && Ptr(r, base + 0x34b80b8, owner) &&
         owner == e.source.registry_owner && Range(owner, 0x80) &&
         Ptr(r, owner + 0x78, header) && header == e.source.registry_header &&
         Bytes(r, header, h.data(), h.size()) && h[0x28] == 28 &&
         (entries = U64(h.data() + 0x20)) &&
         entries == e.source.registry_entries;
}
struct Copy {
  uint64_t holder{};
  std::array<uint8_t, 40> header{};
  std::array<uint8_t, 64> payload{};
  bool operator==(const Copy &) const = default;
};
bool Header(const ReadFn &r, uint64_t base, Copy &c) {
  if (!Ptr(r, base + 0x34bb9c0, c.holder) ||
      !Bytes(r, c.holder, c.header.data(), c.header.size()) ||
      U64(c.header.data()) != base + 0x33d44e8)
    return false;
  const auto p = U64(c.header.data() + 8), w = U64(c.header.data() + 16);
  return p && p % 8 == 0 && Range(p, 64) && p == U64(c.header.data() + 32) &&
         (w & UINT64_C(0xff000000ffffffff)) == UINT64_C(0x0400000000000000) &&
         (w & UINT64_C(0x00000fff00000000)) != 0;
}
bool Sample(const ReadFn &r, uint64_t base, Copy &c) {
  Copy tail;
  return Header(r, base, c) &&
         Bytes(r, U64(c.header.data() + 8), c.payload.data(),
               c.payload.size()) &&
         Header(r, base, tail) && c.holder == tail.holder &&
         c.header == tail.header;
}
bool Finite3(const std::array<float, 3> &v) {
  return std::all_of(v.begin(), v.end(),
                     [](float f) { return std::isfinite(f); });
}
struct Blob {
  std::vector<uint8_t> b;
  bool Span(uint64_t a, uint64_t n) const {
    return a <= b.size() && n <= b.size() - a;
  }
  uint32_t At32(size_t a) const { return U32(b.data() + a); }
  uint64_t At64(size_t a) const { return U64(b.data() + a); }
  float AtF(size_t a) const { return F32(b.data() + a); }
  bool String(uint64_t p, size_t limit, std::string &out) const {
    if (!Span(p, 1))
      return false;
    size_t n = 0;
    while (n < limit && Span(p + n, 1) && b[p + n])
      ++n;
    if (n == limit || !Span(p + n, 1))
      return false;
    out.assign(reinterpret_cast<const char *>(b.data() + p), n);
    return true;
  }
};
bool Load(const ReadAssetFn &read, std::string_view path, size_t cap,
          Blob &out) {
  const auto n = read(path, 0, nullptr, 0);
  if (!n || n > cap)
    return false;
  out.b.resize(n);
  return read(path, 0, out.b.data(), n) == n;
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
struct Room {
  std::array<float, 3> lo{}, hi{};
  uint16_t number{};
  bool side_view{};
};
struct NativeLevel {
  std::string key;
  std::vector<Room> rooms;
  std::array<float, 3> lo{}, hi{};
};
bool Common(const Blob &b, std::map<std::string, NativeLevel> &out) {
  constexpr std::array<uint8_t, 8> magic{0x65, 0x63, 0x6c, 0x6f, 3, 0, 1, 1};
  if (!b.Span(0, 0x8c) ||
      !std::equal(magic.begin(), magic.end(), b.b.begin()) ||
      b.At32(8) != b.b.size() || b.At32(0x84) != 0x65636c64)
    return false;
  const auto count = b.At32(0xc), dir = b.At32(0x10), rows = b.At32(0x18),
             rc = b.At32(0x14);
  const auto rooms = b.At32(0x20), roomcount = b.At32(0x1c), si = b.At32(0x78),
             sc = b.At32(0x74), ss = b.At32(0x80);
  if (!count || count > 4096 || count != rc || count != b.At32(0x88) ||
      roomcount > 8192 || sc > 16384 || !b.Span(dir, uint64_t(count) * 8) ||
      !b.Span(rows, uint64_t(rc) * 0x120) ||
      !b.Span(rooms, uint64_t(roomcount) * 0x60) ||
      !b.Span(si, uint64_t(sc) * 4) || !b.Span(ss, 1))
    return false;
  std::set<uint32_t> indices;
  uint32_t prev = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto key = b.At32(dir + i * 8), index = b.At32(dir + i * 8 + 4);
    if ((i && key <= prev) || index >= rc || !indices.insert(index).second)
      return false;
    prev = key;
    const size_t a = rows + size_t(index) * 0x120;
    const auto ni = b.At32(a + 0x100);
    NativeLevel level;
    if (ni >= sc ||
        !b.String(uint64_t(ss) + b.At32(si + ni * 4), 128, level.key) ||
        level.key.empty() || Crc(level.key) != key)
      return false;
    for (size_t j = 0; j < 3; ++j) {
      level.lo[j] = b.AtF(a + 0x60 + j * 4);
      level.hi[j] = b.AtF(a + 0x70 + j * 4);
    }
    if (!Finite3(level.lo) || !Finite3(level.hi))
      return false;
    const auto first = b.At32(a + 0x108);
    const auto n = uint16_t(b.At32(a + 0x114) >> 16);
    if (n > 64 || first > roomcount || n > roomcount - first)
      return false;
    for (size_t j = 0; j < n; ++j) {
      const size_t q = rooms + (first + j) * 0x60;
      Room room;
      for (size_t k = 0; k < 3; ++k) {
        room.lo[k] = b.AtF(q + 0x30 + k * 4);
        room.hi[k] = b.AtF(q + 0x40 + k * 4);
      }
      room.number = uint16_t(b.At32(q + 0x54) >> 16);
      room.side_view = b.b[q + 0x5e] != 0;
      if (!Finite3(room.lo) || !Finite3(room.hi))
        return false;
      level.rooms.push_back(room);
    }
    if (!out.emplace(level.key, std::move(level)).second)
      return false;
  }
  return true;
}
struct Descriptor {
  std::string name, type_name;
  uint8_t type{}, depth{};
  uint16_t flags{}, offset{}, size{}, stride{}, children{};
  uint64_t child{};
};
bool Sheet(const Blob &b, size_t top, size_t fields, size_t stride,
           size_t maxrows, std::vector<Descriptor> &desc, size_t &rows,
           size_t &count) {
  constexpr std::array<uint8_t, 8> magic{0x67, 0x73, 0x68, 0x74, 2, 0, 0, 0};
  if (!b.Span(0, 48) || !std::equal(magic.begin(), magic.end(), b.b.begin()) ||
      b.At32(24) != top || b.At32(28) != fields || b.At32(44) != stride)
    return false;
  rows = b.At64(32);
  count = b.At32(40);
  if (count > maxrows || !b.Span(48, fields * 48) ||
      !b.Span(rows, count * stride))
    return false;
  for (size_t i = 0; i < fields; ++i) {
    size_t a = 48 + i * 48;
    Descriptor d;
    if (!b.String(b.At64(a), 128, d.name) ||
        !b.String(b.At64(a + 8), 128, d.type_name))
      return false;
    d.type = b.b[a + 16];
    d.depth = b.b[a + 17];
    const auto w = b.At32(a + 18), v = b.At32(a + 22), x = b.At32(a + 26);
    d.flags = uint16_t(w);
    d.offset = uint16_t(w >> 16);
    d.size = uint16_t(v);
    d.stride = uint16_t(v >> 16);
    d.children = uint16_t(x);
    d.child = b.At64(a + 32);
    desc.push_back(d);
  }
  return true;
}
bool Text(const Blob &b, size_t a, std::string &out) {
  if (!b.Span(a, 16))
    return false;
  const auto p = b.At64(a), n = b.At64(a + 8);
  if (n > 127 || !b.Span(p, n))
    return false;
  out.assign(reinterpret_cast<const char *>(b.b.data() + p), n);
  return std::all_of(out.begin(), out.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}
struct Alias {
  std::map<int, std::array<float, 3>> positions;
};
bool Aliases(const Blob &b, std::map<std::string, Alias> &out) {
  std::vector<Descriptor> d;
  size_t rows{}, count{};
  if (!b.Span(8, 4) || b.At32(8) != 0x08166e53 ||
      !Sheet(b, 2, 6, 32, 512, d, rows, count))
    return false;
  const std::array<std::string_view, 6> names{
      "mKey", "mPosInfo", "mUiMapLevelType", "mPosX", "mPosY", "mPosZ"};
  const std::array<uint8_t, 6> types{4, 0, 2, 3, 3, 3};
  const std::array<std::string_view, 6> type_names{
      "string",         "__inline_struct_UiMapPlayerPos_mPosInfo",
      "UiMapLevelType", "float",
      "float",          "float"};
  const std::array<uint16_t, 6> flags{4, 2, 16, 0, 0, 0},
      offsets{0, 16, 0, 4, 8, 12}, sizes{16, 16, 4, 4, 4, 4};
  for (size_t i = 0; i < 6; ++i)
    if (d[i].name != names[i] || d[i].type_name != type_names[i] ||
        d[i].type != types[i] || d[i].flags != flags[i] ||
        d[i].offset != offsets[i] || d[i].size != sizes[i] ||
        d[i].stride != sizes[i] || d[i].depth != (i >= 2 ? 1 : 0) ||
        d[i].children != (i == 1 ? 4 : 0) || d[i].child != (i == 1 ? 144 : 0))
      return false;
  for (size_t i = 0; i < count; ++i) {
    size_t a = rows + i * 32;
    std::string key;
    Alias alias;
    if (!Text(b, a, key) || key.empty())
      return false;
    const auto p = b.At64(a + 16), n = b.At64(a + 24);
    if (n > 16 || !b.Span(p, n * 16))
      return false;
    for (size_t j = 0; j < n; ++j) {
      size_t q = p + j * 16;
      int type = std::bit_cast<int32_t>(b.At32(q));
      std::array<float, 3> xyz{};
      for (size_t k = 0; k < 3; ++k)
        xyz[k] = b.AtF(q + 4 + k * 4);
      if (type < 0 || type > 3 || !Finite3(xyz) ||
          !alias.positions.emplace(type, xyz).second)
        return false;
    }
    if (!out.emplace(key, std::move(alias)).second)
      return false;
  }
  return true;
}
// Native268c040 reads UiMapLevelDefine key -> mLevelName;268d46c..d4e8
// binds this exact named GSHT/hash b99a1b35. Only explicit mappings to Field
// are admitted for raw aliases, avoiding guessed mapped-key precedence.
bool Mappings(const Blob &b, std::map<std::string, std::string> &out) {
  std::vector<Descriptor> d;
  size_t rows{}, count{};
  if (!b.Span(8, 4) || b.At32(8) != 0xb99a1b35 ||
      !Sheet(b, 2, 2, 32, 512, d, rows, count))
    return false;
  for (size_t i = 0; i < 2; ++i) {
    if (d[i].name != (i ? "mLevelName" : "mKey") ||
        d[i].type_name != "string" || d[i].type != 4 || d[i].depth != 0 ||
        d[i].flags != (i ? 0 : 4) || d[i].offset != i * 16 || d[i].size != 16 ||
        d[i].stride != 16 || d[i].children || d[i].child)
      return false;
  }
  for (size_t i = 0; i < count; ++i) {
    std::string key, value;
    if (!Text(b, rows + i * 32, key) || key.empty() ||
        !Text(b, rows + i * 32 + 16, value) || value.empty() ||
        !out.emplace(key, value).second)
      return false;
  }
  return true;
}
const MapPane *Pane(const std::vector<MapPane> &p, std::string_view name) {
  const MapPane *out = nullptr;
  for (const auto &x : p)
    if (x.name == name) {
      if (out)
        return nullptr;
      out = &x;
    }
  return out;
}
bool Plain(const MapPane &p) {
  return p.origin == 0 && p.rotation_x == 0 && p.rotation_y == 0 &&
         p.rotation_z == 0 && p.scale_x == 1 && p.scale_y == 1 && p.z == 0 &&
         std::isfinite(p.x) && std::isfinite(p.y);
}
bool AtParent(const std::vector<MapPane> &p, const MapPane &child,
              std::string_view parent) {
  return child.parent >= 0 && size_t(child.parent) < p.size() &&
         p[size_t(child.parent)].name == parent;
}
bool Geometry(const FieldMapGeometry &g, std::array<float, 2> &anchor,
              MapBounds &bounds, float &width) {
  const auto *area = Pane(g.field_panes, "B_MapAreaTop_00");
  const auto *part = Pane(g.field_panes, "L_Attach_00");
  const auto *player = Pane(g.attach_panes, "N_Player_00");
  const auto *priority = Pane(g.attach_panes, "N_Priority_00");
  const auto *root = Pane(g.attach_panes, "RootPane");
  if (!area || !part || !player || !priority || !root || !Plain(*area) ||
      !Plain(*part) || !Plain(*player) || !Plain(*priority) || !Plain(*root) ||
      !AtParent(g.field_panes, *part, "B_MapAreaTop_00") ||
      !AtParent(g.attach_panes, *player, "N_Priority_00") ||
      !AtParent(g.attach_panes, *priority, "RootPane") || root->parent != -1 ||
      part->part_layout != "L_MapAttachPanes" || part->x != 0 || part->y != 0 ||
      player->x != 0 || player->y != 0 || priority->x != 0 ||
      priority->y != 0 || root->x != 0 || root->y != 0 || !(area->width > 0))
    return false;
  anchor = {area->x, area->y};
  width = area->width;
  const auto *terrain = Pane(g.field_panes, "P_Terrain_00");
  if (!terrain || !Plain(*terrain) || !(terrain->width > 0) ||
      !(terrain->height > 0))
    return false;
  float x = 0, y = 0;
  const MapPane *at = terrain;
  size_t steps = 0;
  while (at->name != "N_MapRoot_00") {
    if (++steps > g.field_panes.size() || !Plain(*at) || at->parent < 0 ||
        size_t(at->parent) >= g.field_panes.size())
      return false;
    x += at->x;
    y += at->y;
    at = &g.field_panes[size_t(at->parent)];
  }
  if (!AtParent(g.field_panes, *area, "N_MapRoot_00"))
    return false;
  bounds = {x - terrain->width / 2, y + terrain->height / 2, terrain->width,
            terrain->height};
  return std::isfinite(width) && std::isfinite(bounds.left) &&
         std::isfinite(bounds.top) && std::isfinite(bounds.width) &&
         std::isfinite(bounds.height);
}
} // namespace
PlayerWorldSnapshot ReadPlayerWorld(const ReadFn &read, uint64_t base,
                                    bool build, const MapSnapshot &expected) {
  PlayerWorldSnapshot out;
  if (!read || !build || !base || base % 8 || base > UINT64_MAX - 0x34bf100 ||
      !expected.compatible || !expected.discovery_valid ||
      !expected.current_level_valid || expected.current_level_key.empty() ||
      expected.current_level_key.size() > 63)
    return out;
  for (const auto &g : Guards) {
    std::array<uint8_t, 16> b{};
    if (!Bytes(read, base + g.offset, b.data(), b.size()) || !MatchSupportedGuard(b, g.bytes, g.offset))
      return out;
  }
  out.compatible = true;
  if (!Epoch(read, base, expected) || !Level(read, base, expected))
    return out;
  Copy a, b, c, tail;
  if (!Sample(read, base, a) || !Sample(read, base, b) ||
      !Sample(read, base, c) || a != b || a != c)
    return out;
  std::array<float, 3> xyz{};
  for (size_t i = 0; i < 3; ++i)
    xyz[i] = F32(a.payload.data() + i * 4);
  if (!Finite3(xyz) || !Level(read, base, expected) ||
      !Epoch(read, base, expected) || !Header(read, base, tail) ||
      tail.holder != a.holder || tail.header != a.header ||
      !Level(read, base, expected))
    return out;
  // Recheck epoch after final level read; context brackets remain the caller's
  // job.
  if (!Epoch(read, base, expected) || !Sample(read, base, tail) || tail != a ||
      !Level(read, base, expected) || !Epoch(read, base, expected))
    return out;
  if (!Header(read, base, tail) || tail.holder != a.holder ||
      tail.header != a.header)
    return out;
  out.valid = true;
  out.xyz = xyz;
  out.level_key = expected.current_level_key;
  out.source = {expected.source, expected.current_level_source, a.holder,
                U64(a.header.data() + 8), U64(a.header.data() + 16)};
  return out;
}
struct FieldProjection::Impl {
  std::map<std::string, NativeLevel> levels;
  std::map<std::string, Alias> aliases;
  std::map<std::string, std::string> mappings;
  std::array<float, 3> center{};
  std::array<float, 2> anchor{};
  MapBounds terrain{};
  float scale{};
  bool authored_marker_attachments{};
};
struct FieldProjectionAccess {
  static const auto &Get(const FieldProjection &p) { return p.impl_; }
};
bool LoadFieldProjection(const ReadAssetFn &read,
                         const FieldMapGeometry &geometry,
                         FieldProjection &out) {
  out = FieldProjection{};
  if (!read)
    return false;
  Blob common, aliases, mappings;
  auto data = std::make_shared<FieldProjection::Impl>();
  float width{};
  if (!Load(read, "region_common/level/Common.eclo", 1024 * 1024, common) ||
      !Common(common, data->levels) ||
      !Load(read, "region_common/datasheets/ui/UiMapPlayerPos.gsheet", 65536,
            aliases) ||
      !Aliases(aliases, data->aliases) ||
      !Load(read, "region_common/datasheets/ui/UiMapLevelDefine.gsheet", 65536,
            mappings) ||
      !Mappings(mappings, data->mappings) ||
      !Geometry(geometry, data->anchor, data->terrain, width))
    return false;
  data->authored_marker_attachments=true;
  for(std::string_view name:{"N_Land_00","N_CheckPoint_00"}) {
    const auto* pane=Pane(geometry.attach_panes,name);
    if(!pane || !Plain(*pane) || pane->x!=0 || pane->y!=0 ||
        !AtParent(geometry.attach_panes,*pane,"N_Priority_00"))
      data->authored_marker_attachments=false;
  }
  const auto field = data->levels.find("Field");
  if (field == data->levels.end())
    return false;
  for (size_t i = 0; i < 3; ++i) {
    if (!(field->second.hi[i] > field->second.lo[i]))
      return false;
    data->center[i] = (field->second.lo[i] + field->second.hi[i]) * 0.5f;
  }
  data->scale = width / (field->second.hi[0] - field->second.lo[0]);
  if (!std::isfinite(data->scale) || !(data->scale > 0))
    return false;
  out.impl_ = std::move(data);
  return true;
}
FieldNativeProjection ProjectFieldNativePoint(const FieldProjection &data,
                                               std::array<float,2> point) {
  FieldNativeProjection out;
  const auto& p=FieldProjectionAccess::Get(data);
  if(!p || !std::isfinite(point[0]) || !std::isfinite(point[1]))return out;
  const float px=point[0]+p->anchor[0]-p->terrain.left,
              py=p->terrain.top-(point[1]+p->anchor[1]);
  const float u=px/p->terrain.width,v=py/p->terrain.height;
  if(!std::isfinite(u)||!std::isfinite(v)||u<0||u>1||v<0||v>1)return out;
  out.valid=true;out.uv={u,v};return out;
}
FieldNativeProjection ProjectFieldAuthoredWorld(const FieldProjection &data,
                                                std::array<float,3> world, std::array<float,2> offset) {
  const auto &p=FieldProjectionAccess::Get(data);
  if(!p || !p->authored_marker_attachments || !Finite3(world) || !std::isfinite(offset[0]) || !std::isfinite(offset[1]) || std::abs(offset[0])>10000 || std::abs(offset[1])>10000)return {};
  const auto level=p->levels.find("Field");
  if(level==p->levels.end() || level->second.rooms.size()!=1)return {};
  const auto &room=level->second.rooms[0];
  if(room.side_view || room.lo!=level->second.lo || room.hi!=level->second.hi ||
      p->aliases.contains("Field"))return {};
  for(const auto &[key,value]:p->aliases) {
    (void)value;
    if(key.starts_with("Field_"))return {};
  }
  for(size_t i=0;i<3;++i)
    if(world[i]<room.lo[i] || world[i]>room.hi[i])return {};
  // Native landmark2616790..6898 uses the original Field room/level centre
  // and separate float multiplies, matching this exact Field raw-world path.
  const volatile float x=(world[0]-p->center[0])*p->scale;
  const volatile float y=-(world[2]-p->center[2])*p->scale;
  // Native261129c..1320 adds catalog offsets in layout XY after projection.
  return ProjectFieldNativePoint(data,{x+offset[0],y+offset[1]});
}
FieldPlayerProjection ProjectFieldPlayer(const FieldProjection &data,
                                         const PlayerWorldSnapshot &world) {
  FieldPlayerProjection out;
  const auto &p = FieldProjectionAccess::Get(data);
  if (!p || !world.valid || !world.compatible || !Finite3(world.xyz))
    return out;
  const auto level = p->levels.find(world.level_key);
  if (level == p->levels.end())
    return out;
  // Native2619a54..2619b38 requires a containing authored room before alias
  // lookup. First slice admits one-room levels only. Multiple rooms/alternate
  // keys remain hidden.
  if (level->second.rooms.size() != 1)
    return out;
  const auto &room = level->second.rooms[0];
  if (room.side_view)
    return out;
  for (size_t i = 0; i < 3; ++i)
    if (world.xyz[i] < room.lo[i] || world.xyz[i] > room.hi[i])
      return out;
  std::array<float, 3> resolved = world.xyz;
  bool alias = false;
  float native_offset = 0;
  if (world.level_key != "Field") {
    const auto mapping = p->mappings.find(world.level_key);
    if (mapping == p->mappings.end() || mapping->second != "Field")
      return out;
    const auto raw = p->aliases.find(world.level_key);
    if (raw == p->aliases.end())
      return out;
    const auto position = raw->second.positions.find(0);
    if (position == raw->second.positions.end())
      return out;
    // Native room-key lookup precedes raw key; full-map mode also tries raw
    // and mapped cleared variants. Explicit map=Field plus absence of ALL
    // authored suffix variants makes those branches unsuccessful regardless
    // of their mutable condition. No literal level allowlist is used.
    for (const auto &[key, value] : p->aliases) {
      (void)value;
      if (key.starts_with(world.level_key + "_") ||
          key.starts_with(mapping->second + "_"))
        return out;
    }
    resolved = position->second;
    alias = true;
    // Original guarded MOVZ/MOVK at261a400/404 provide native alias-success
    // offset. This is an algorithm instruction immediate, not authored data.
    const auto &code = Guards[11].bytes;
    const uint32_t low = (U32(code.data() + 8) >> 5) & 0xffff,
                   high = (U32(code.data() + 12) >> 5) & 0xffff;
    native_offset = std::bit_cast<float>(low | (high << 16));
  } else {
    // Exact Field source: require sole room bounds equal world bounds, and
    // no authored Field alias/suffix. No local-area substitution is inferred.
    if (p->aliases.contains("Field"))
      return out;
    for (const auto &[key, value] : p->aliases) {
      (void)value;
      if (key.starts_with("Field_"))
        return out;
    }
    if (room.lo != level->second.lo || room.hi != level->second.hi)
      return out;
  }
  // Original uses separate FMUL/FADD; volatile boundaries prevent an ARM
  // compiler from fusing the authored alias offset into an FMA.
  const volatile float native_x = (resolved[0] - p->center[0]) * p->scale;
  const volatile float scaled_y = -(resolved[2] - p->center[2]) * p->scale;
  const float x = native_x;
  const float y = scaled_y + native_offset;
  const auto native=ProjectFieldNativePoint(data,{x,y});
  if(!native.valid)return out;
  out.valid = true;
  out.authored_alias = alias;
  out.native_layout = {x, y};
  out.uv = native.uv;
  out.world = world;
  return out;
}
} // namespace echoes
