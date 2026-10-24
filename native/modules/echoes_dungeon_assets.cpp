// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_dungeon_assets.h"
#include "acnh_bytes.h"
#include "acnh_tex.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
namespace echoes {
namespace {
using namespace acnh::bytes;
constexpr size_t MaxSheet = 1u << 20, MaxSource = 32u << 20,
                 MaxDecoded = 32u << 20;
bool Component(std::string_view s) {
  return !s.empty() && s.size() < 96 &&
         std::all_of(
             s.begin(), s.end(),
             [](unsigned char c) {
               return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_';
             });
}
bool Load(const ReadAssetFn &read, std::string_view name, size_t maximum,
          std::vector<uint8_t> &out) {
  out.clear();
  if (!read)
    return false;
  const size_t n = read(name, 0, nullptr, 0);
  if (!n || n > maximum)
    return false;
  out.resize(n);
  if (read(name, 0, out.data(), n) != n) {
    out.clear();
    return false;
  }
  return true;
}
struct Sheet {
  std::span<const uint8_t> b;
  size_t pool{}, rows{};
  uint32_t count{}, stride{};
  bool String(size_t at, size_t length, std::string &out) const {
    if (length > 128 || at < pool || !Fits(b, at, length))
      return false;
    out.assign(reinterpret_cast<const char *>(b.data() + at), length);
    return std::all_of(out.begin(), out.end(),
                       [](unsigned char c) { return c >= 32 && c < 127; });
  }
  bool Ref(size_t at, std::string &out) const {
    return Fits(b, at, 16) && String(Le64(b, at), Le64(b, at + 8), out);
  }
  float Float(size_t at) const {
    float f = F32(b, at);
    if (!Fits(b, at, 4) || !std::isfinite(f) || std::abs(f) > 100000)
      throw std::runtime_error("float");
    return f;
  }
  bool Init(std::span<const uint8_t> bytes, uint32_t top, uint32_t all,
            uint32_t size, std::span<const std::string_view> names) {
    b = bytes;
    if (b.size() > MaxSheet || !Fits(b, 0, 48) ||
        std::memcmp(b.data(), "gsht\2\0\0\0", 8) || Le32(b, 24) != top ||
        Le32(b, 28) != all || all != names.size() || Le32(b, 44) != size ||
        !Fits(b, 48, size_t(all) * 48))
      return false;
    pool = Le64(b, 16);
    rows = Le64(b, 32);
    count = Le32(b, 40);
    stride = size;
    if (!count || count > 1024 || rows < 48 + size_t(all) * 48 ||
        !Fits(b, rows, uint64_t(count) * stride) ||
        pool < rows + uint64_t(count) * stride || !Fits(b, pool, 1))
      return false;
    for (size_t i = 0; i < all; ++i) {
      const size_t p = Le64(b, 48 + i * 48);
      if (p < pool || !Fits(b, p, 1))
        return false;
      size_t n = 0;
      while (n < 128 && Fits(b, p + n, 1) && b[p + n])
        ++n;
      std::string name;
      if (!String(p, n, name) || name != names[i])
        return false;
    }
    return true;
  }
};
constexpr std::array<std::string_view, 12> AreaFields = {
    "mAreaName",          "mPriority",   "mPos",  "mTexScale", "mFloorInfo",
    "mChangeTexutreInfo", "mPosX",       "mPosY", "mFloor",    "mHeight",
    "mFlagName",          "mTextureName"};
constexpr std::array<std::string_view, 7> LineFields = {"mLevelName",
                                                        "mLineInfos",
                                                        "mStartChangeFloorName",
                                                        "mEndChangeFloorName",
                                                        "mCorners",
                                                        "mX",
                                                        "mY"};
bool Layout(std::span<const uint8_t> b,
            std::span<const std::string_view> needed) {
  if (b.size() > 128u << 10 || !Fits(b, 0, 20) ||
      std::memcmp(b.data(), "FLYT", 4) || Le16(b, 4) != 0xfeff ||
      Le32(b, 8) != 0x09000000 || Le32(b, 12) != b.size())
    return false;
  size_t p = Le16(b, 6), count = 0;
  std::set<std::string> found;
  if (p < 20)
    return false;
  while (p < b.size()) {
    if (!Fits(b, p, 8))
      return false;
    const size_t n = Le32(b, p + 4);
    if (n < 8 || !Fits(b, p, n))
      return false;
    ++count;
    if ((!std::memcmp(b.data() + p, "pan1", 4) ||
         !std::memcmp(b.data() + p, "bnd1", 4)) &&
        n >= 0x54) {
      size_t len = 0;
      while (len < 24 && b[p + 12 + len])
        ++len;
      if (len == 24)
        return false;
      found.emplace(reinterpret_cast<const char *>(b.data() + p + 12), len);
    }
    p += n;
  }
  return count == Le16(b, 16) &&
         std::all_of(needed.begin(), needed.end(), [&](auto name) {
           return found.contains(std::string(name));
         });
}
std::string AreaKey(const DungeonMapDefinition &def, uint16_t number) {
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), "_%03u", unsigned(number));
  return def.key + suffix;
}
void Over(uint8_t *d, const uint8_t *s) {
  const unsigned a = unsigned(s[3]) * 255 + unsigned(d[3]) * (255 - s[3]);
  if (!a)
    return;
  for (int c = 0; c < 3; ++c)
    d[c] = uint8_t((unsigned(s[c]) * s[3] * 255 +
                    unsigned(d[c]) * d[3] * (255 - s[3]) + a / 2) /
                   a);
  d[3] = uint8_t((a + 127) / 255);
}
void Put32(std::vector<uint8_t> &b, size_t p, uint32_t value) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = uint8_t(value >> (8 * i));
}
void Put64(std::vector<uint8_t> &b, size_t p, uint64_t value) {
  for (int i = 0; i < 8; ++i)
    b[p + i] = uint8_t(value >> (8 * i));
}
uint32_t Deposit(uint32_t value, uint32_t mask) {
  uint32_t result = 0;
  for (uint32_t bit = 1; mask; bit += bit) {
    if (value & bit)
      result |= mask & (~mask + 1);
    mask &= mask - 1;
  }
  return result;
}
// Two original room textures exceed the host's2048px ASTC limit. Preserve their
// authored ASTC6x5 blocks, selecting <=1020px linear tiles for the existing
// native decoder. No full oversized decoder call or invented room bitmap is
// needed.
bool DecodeLargeRoom(const std::vector<uint8_t> &source, std::string_view name,
                     Image &out) {
  out = {};
  const auto b = std::span<const uint8_t>(source);
  if (!Fits(b, 0, 48) || std::memcmp(b.data(), "BNTX", 4))
    return false;
  const uint32_t count = Le32(b, 36);
  const uint64_t array = Le64(b, 40);
  if (!count || count > 512 || !Fits(b, array, uint64_t(count) * 8))
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t t = Le64(b, array + uint64_t(i) * 8);
    if (!Fits(b, t, 0xa0) || std::memcmp(b.data() + t, "BRTI", 4))
      return false;
    const uint64_t np = Le64(b, t + 0x60);
    if (!Fits(b, np, 2))
      return false;
    const uint16_t nl = Le16(b, np);
    if (!Fits(b, np + 2, nl) || nl > 128)
      return false;
    if (std::string_view(reinterpret_cast<const char *>(b.data() + np + 2),
                         nl) != name)
      continue;
    const uint32_t w = Le32(b, t + 0x24), h = Le32(b, t + 0x28),
                   size = Le32(b, t + 0x50), layout = Le32(b, t + 0x34) & 7;
    const uint64_t mip = Le64(b, t + 0x70);
    if (!w || !h || w > 4096 || h > 4096 || uint64_t(w) * h * 4 > MaxDecoded ||
        Le32(b, t + 0x1c) != 0x3006 || Le32(b, t + 0x30) != 1 ||
        Le16(b, t + 0x12) != 0 || layout > 5 || !Fits(b, mip, 8))
      return false;
    const uint64_t data = Le64(b, mip);
    if (!Fits(b, data, size))
      return false;
    constexpr uint32_t bw = 6, bh = 5;
    const uint32_t cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh;
    const uint64_t stride = (uint64_t(cols) * 16 + 63) & ~uint64_t(63),
                   aligned_rows =
                       (uint64_t(rows) + (uint64_t(8) << layout) - 1) &
                       ~((uint64_t(8) << layout) - 1);
    if (stride * aligned_rows > size)
      return false;
    std::vector<uint8_t> linear(size_t(cols) * rows * 16);
    const uint32_t block_size = uint32_t(stride / 64) << (9 + layout),
                   height_mask = (1u << layout) - 1;
    for (uint32_t y = 0; y < rows; ++y)
      for (uint32_t x = 0; x < cols; ++x) {
        const uint32_t by = y >> 3, byte_x = x * 16;
        const uint64_t at =
            uint64_t(by >> layout) * block_size + ((by & height_mask) << 9) +
            (uint64_t(byte_x >> 6) << (9 + layout)) +
            (Deposit(byte_x, 0b100101111) | Deposit(y, 0b011010000));
        if (at > size || 16 > size - at)
          return false;
        std::memcpy(linear.data() + (size_t(y) * cols + x) * 16,
                    b.data() + data + at, 16);
      }
    Image result{int(w), int(h)};
    auto view = source;
    // Original metadata only: direct a temporary one-layer view at a linear
    // tile appended to its owned local source. Other texture records stay
    // intact.
    view[size_t(t) + 0x12] = 1;
    view[size_t(t) + 0x13] = 0;
    for (uint32_t y = 0; y < h; y += 1020)
      for (uint32_t x = 0; x < w; x += 1020) {
        const uint32_t tw = std::min(1020u, w - x), th = std::min(1020u, h - y),
                       tc = (tw + 5) / 6, tr = (th + 4) / 5;
        const uint32_t pitch = (tc * 16 + 31) & ~31u, tile_bytes = pitch * tr;
        if (source.size() + tile_bytes > 64u << 20)
          return false;
        view.resize(source.size() + tile_bytes);
        std::fill(view.begin() + source.size(), view.end(), 0);
        for (uint32_t row = 0; row < tr; ++row)
          std::memcpy(view.data() + source.size() + uint64_t(row) * pitch,
                      linear.data() + (size_t(y / 5 + row) * cols + x / 6) * 16,
                      size_t(tc) * 16);
        Put32(view, 0x1c, uint32_t(view.size()));
        Put32(view, size_t(t) + 0x24, tw);
        Put32(view, size_t(t) + 0x28, th);
        Put32(view, size_t(t) + 0x50, tile_bytes);
        Put64(view, size_t(mip), source.size());
        Image tile;
        if (!acnh::BntxDecode(view, name, tile) || tile.w != int(tw) ||
            tile.h != int(th))
          return false;
        for (uint32_t row = 0; row < th; ++row)
          std::memcpy(result.At(int(x), int(y + row)), tile.At(0, int(row)),
                      size_t(tw) * 4);
      }
    out = std::move(result);
    return true;
  }
  return false;
}
struct Quad {
  const DungeonRoomArt *art;
  const DungeonRoom *room;
  double x, y, w, h;
};
Quad RoomQuad(const DungeonMapDefinition &def, const DungeonRoomArt &art,
              const DungeonRoom &room) {
  // Native MapArea25dea0c: (+posX,-posY)*size ratio.25dea5c: room
  // extents X/(side-view Y or top-down Z)*size ratio*authored texture scale.
  if (!std::isfinite(art.texture_scale) || art.texture_scale <= 0 ||
      art.texture_scale > 8 || !std::isfinite(art.position_x) ||
      !std::isfinite(art.position_y) || std::abs(art.position_x) > 100000 ||
      std::abs(art.position_y) > 100000)
    throw std::runtime_error("room art");
  for (size_t i = 0; i < 3; ++i)
    if (!std::isfinite(room.lo[i]) || !std::isfinite(room.hi[i]) ||
        std::abs(room.lo[i]) > 100000 || std::abs(room.hi[i]) > 100000 ||
        room.lo[i] > room.hi[i])
      throw std::runtime_error("room bounds");
  const double scale = double(def.size_ratio) * art.texture_scale;
  Quad q{&art,
         &room,
         double(art.position_x) * def.size_ratio,
         -double(art.position_y) * def.size_ratio,
         (double(room.hi[0]) - room.lo[0]) * scale,
         (double(room.hi[room.side_view ? 1 : 2]) -
          room.lo[room.side_view ? 1 : 2]) *
             scale};
  if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.w) ||
      !std::isfinite(q.h) || q.w <= 0 || q.h <= 0 || q.w > 20000 || q.h > 20000)
    throw std::runtime_error("quad");
  return q;
}
struct RoomFrame {
  std::vector<Quad> quads;
  double left{}, top{}, width{}, height{}, scale{};
  int w{}, h{};
  MapBounds Bounds() const {
    return {float(left), float(top), float(width), float(height)};
  }
};
RoomFrame Frame(const DungeonMapDefinition &def, const DungeonMapArt &art) {
  if (!Component(def.key) || !def.key.starts_with("L_Dungeon") ||
      art.key != def.key || def.rooms.empty() || def.rooms.size() > 64 ||
      art.rooms.empty() || art.rooms.size() > 64 || def.floor_count < 1 ||
      def.floor_count > 10 || !std::isfinite(def.size_ratio) ||
      def.size_ratio <= 0 || def.size_ratio > 64)
    throw std::runtime_error("room frame");
  RoomFrame result;
  std::set<uint16_t> drawable, used;
  for (const auto &room : def.rooms)
    if (!room.floors.empty() &&
        (room.number >= 64 || !drawable.insert(room.number).second))
      throw std::runtime_error("drawable room");
  // Native definitions can contain floor-bearing rooms with no UiMapArea row
  // (e.g. L_Dungeon002). Only authored area art contributes drawable bounds;
  // absent art must neither invalidate the floor nor invent a room focus.
  for (const auto &entry : art.rooms) {
    if (!used.insert(entry.room).second)
      throw std::runtime_error("duplicate room art");
    const auto room =
        std::find_if(def.rooms.begin(), def.rooms.end(), [&](const auto &r) {
          return r.number == entry.room && !r.floors.empty();
        });
    if (room == def.rooms.end())
      throw std::runtime_error("room art membership");
    result.quads.push_back(RoomQuad(def, entry, *room));
  }
  double left = 1e30, top = -1e30, right = -1e30, bottom = 1e30;
  for (const auto &q : result.quads) {
    left = std::min(left, q.x - q.w / 2);
    right = std::max(right, q.x + q.w / 2);
    top = std::max(top, q.y + q.h / 2);
    bottom = std::min(bottom, q.y - q.h / 2);
  }
  if (right <= left || top <= bottom || right - left > 30000 ||
      top - bottom > 30000)
    throw std::runtime_error("room frame bounds");
  result.left = left;
  result.top = top;
  result.width = right - left;
  result.height = top - bottom;
  result.scale = std::min(1., 2048. / std::max(result.width, result.height));
  result.w = std::max(1, int(std::ceil(result.width * result.scale)));
  result.h = std::max(1, int(std::ceil(result.height * result.scale)));
  return result;
}
} // namespace

DungeonRoomFocus ProjectDungeonRoomFocus(const DungeonMapDefinition &def,
                                         const DungeonMapArt &art,
                                         uint16_t room, int32_t floor,
                                         uint32_t canvas_width,
                                         uint32_t canvas_height) {
  if (!canvas_width || !canvas_height || canvas_width > 8192 ||
      canvas_height > 8192 || floor < def.floor_min ||
      floor >= int64_t(def.floor_min) + def.floor_count)
    return {};
  try {
    const auto frame = Frame(def, art);
    const auto q = std::find_if(
        frame.quads.begin(), frame.quads.end(),
        [room](const auto &quad) { return quad.room->number == room; });
    if (q == frame.quads.end() ||
        std::find(q->room->floors.begin(), q->room->floors.end(), floor) ==
            q->room->floors.end())
      return {};
    const double fit = std::min(double(canvas_width) / frame.w,
                                double(canvas_height) / frame.h);
    const int w = std::clamp(int(std::lround(frame.w * fit)), 1,
                             int(canvas_width)),
              h = std::clamp(int(std::lround(frame.h * fit)), 1,
                             int(canvas_height)),
              dx = (int(canvas_width) - w) / 2,
              dy = (int(canvas_height) - h) / 2;
    const double x = (q->x - frame.left) * frame.scale / frame.w,
                 y = (frame.top - q->y) * frame.scale / frame.h;
    DungeonRoomFocus result;
    result.center_uv = {float((dx + x * w) / canvas_width),
                        float((dy + y * h) / canvas_height)};
    result.extent_uv = {
        float(q->w * frame.scale / frame.w * w / canvas_width),
        float(q->h * frame.scale / frame.h * h / canvas_height)};
    for (size_t i = 0; i < 2; ++i)
      if (!std::isfinite(result.center_uv[i]) || result.center_uv[i] < 0 ||
          result.center_uv[i] > 1 || !std::isfinite(result.extent_uv[i]) ||
          result.extent_uv[i] <= 0 || result.extent_uv[i] > 1)
        return {};
    result.valid = true;
    return result;
  } catch (...) {
    return {};
  }
}

struct DungeonMapAssets::Impl {
  explicit Impl(ReadAssetFn r) : read(std::move(r)) {}
  ReadAssetFn read;
  std::string key;
  DungeonMapArt art;
  std::vector<uint8_t> source;
  std::map<std::string, Image, std::less<>> decoded;
  size_t decoded_bytes{};
  bool Inspect(const DungeonMapDefinition &def, DungeonMapArt &out) {
    out = {};
    if (!Component(def.key) || !def.key.starts_with("L_Dungeon") ||
        def.rooms.empty() || def.rooms.size() > 64 || def.floor_count < 1 ||
        def.floor_count > 10 || !std::isfinite(def.size_ratio) ||
        def.size_ratio <= 0 || def.size_ratio > 64)
      return false;
    if (key == def.key) {
      out = art;
      return true;
    }
    DungeonMapArt result;
    result.key = def.key;
    std::vector<uint8_t> area_data, line_data, layout;
    if (!Load(read, "region_common/datasheets/ui/UiMapArea.gsheet", MaxSheet,
              area_data) ||
        !Load(read, "region_common/datasheets/ui/UiMapLine.gsheet", MaxSheet,
              line_data))
      return false;
    constexpr std::array<std::string_view, 2> top_names = {"N_DgnMap_00",
                                                           "B_MapAreaTop_00"};
    const std::string root = "region_common/ui/" + def.key + ".arc#blyt/";
    if (!Load(read, root + def.key + ".bflyt", 128u << 10, layout) ||
        !Layout(layout, top_names))
      return false;
    constexpr std::array<std::string_view, 3> floor_names = {
        "B_FloorRange_00", "N_MapArea_Floor_00", "N_MapArea_Floor_09"};
    if (!Load(read, root + "L_MapArea.bflyt", 128u << 10, layout) ||
        !Layout(layout, floor_names))
      return false;
    Sheet a;
    if (!a.Init(area_data, 6, 12, 72, AreaFields))
      return false;
    std::set<uint16_t> numbers;
    for (const auto &room : def.rooms) {
      if (room.floors.empty())
        continue;
      if (room.number >= 64 || !numbers.insert(room.number).second)
        return false;
      const auto area_key = AreaKey(def, room.number);
      bool found = false;
      for (uint32_t row = 0; row < a.count; ++row) {
        const size_t p = a.rows + size_t(row) * a.stride;
        std::string name;
        if (!a.Ref(p, name))
          return false;
        if (name != area_key)
          continue;
        if (found)
          return false;
        found = true;
        DungeonRoomArt item;
        item.room = room.number;
        item.area_name = std::move(name);
        item.priority = int32_t(Le32(a.b, p + 16));
        item.position_x = a.Float(p + 20);
        item.position_y = a.Float(p + 24);
        const uint64_t texture_scale = Le64(a.b, p + 32);
        item.texture_scale =
            texture_scale ? a.Float(size_t(texture_scale)) : 1.f;
        if (item.texture_scale <= 0 || item.texture_scale > 8)
          return false;
        const uint64_t floors = Le64(a.b, p + 40), nf = Le64(a.b, p + 48),
                       changes = Le64(a.b, p + 56), nc = Le64(a.b, p + 64);
        if (nf > 10 || nc > 16 || !Fits(a.b, floors, nf * 8) ||
            !Fits(a.b, changes, nc * 32))
          return false;
        for (size_t i = 0; i < nf; ++i) {
          const float floor = a.Float(size_t(floors) + i * 8);
          if (std::trunc(floor) != floor || floor < 0 || floor > 10)
            return false;
          item.texture_floors.push_back(int32_t(floor));
        }
        for (size_t i = 0; i < nc; ++i) {
          std::string flag, texture;
          if (!a.Ref(size_t(changes) + i * 32, flag) ||
              !a.Ref(size_t(changes) + i * 32 + 16, texture) ||
              !Component(flag) || !Component(texture) ||
              !texture.starts_with(item.area_name + "_c_"))
            return false;
          item.conditional_textures.emplace_back(std::move(flag),
                                                 std::move(texture));
        }
        result.rooms.push_back(std::move(item));
      }
    }
    if (result.rooms.empty())
      return false;
    Sheet l;
    if (!l.Init(line_data, 2, 7, 32, LineFields))
      return false;
    for (uint32_t row = 0; row < l.count; ++row) {
      const size_t p = l.rows + size_t(row) * l.stride;
      std::string name;
      if (!l.Ref(p, name))
        return false;
      if (name != def.key)
        continue;
      const uint64_t list = Le64(l.b, p + 16), n = Le64(l.b, p + 24);
      if (n > 128 || !Fits(l.b, list, n * 48))
        return false;
      for (size_t i = 0; i < n; ++i) {
        const size_t q = size_t(list) + i * 48;
        DungeonConnectionArt line;
        if (!l.Ref(q, line.start_change_floor) ||
            !l.Ref(q + 16, line.end_change_floor))
          return false;
        const uint64_t corners = Le64(l.b, q + 32), nc = Le64(l.b, q + 40);
        if (nc > 64 || !Fits(l.b, corners, nc * 8))
          return false;
        for (size_t j = 0; j < nc; ++j)
          line.corners.push_back({l.Float(size_t(corners) + j * 8),
                                  l.Float(size_t(corners) + j * 8 + 4)});
        result.connections.push_back(std::move(line));
      }
    }
    key = def.key;
    art = std::move(result);
    source.clear();
    decoded.clear();
    decoded_bytes = 0;
    out = art;
    return true;
  }
  const Image *Texture(std::string_view name, bool &missing) {
    missing = false;
    if (const auto at = decoded.find(name); at != decoded.end())
      return &at->second;
    if (source.empty() && !Load(read,
                                "region_common/ui/MapRoomTex_" + key +
                                    ".arc#timg/__Combined.bntx",
                                MaxSource, source))
      return nullptr;
    Image image;
    const auto infos = acnh::BntxList(source);
    if (infos.empty())
      return nullptr;
    const auto info =
        std::find_if(infos.begin(), infos.end(),
                     [&](const auto &i) { return i.name == name; });
    if (info == infos.end()) {
      missing = true;
      return nullptr;
    }
    const bool large = info->w > 2048 || info->h > 2048;
    if (!(large ? DecodeLargeRoom(source, name, image)
                : acnh::BntxDecode(source, name, image)) ||
        image.Empty() || image.rgba.size() > MaxDecoded)
      return nullptr;
    if (image.rgba.size() > MaxDecoded - decoded_bytes) {
      decoded.clear();
      decoded_bytes = 0;
    }
    decoded_bytes += image.rgba.size();
    return &decoded.emplace(std::string(name), std::move(image)).first->second;
  }
};
DungeonMapAssets::DungeonMapAssets(ReadAssetFn r)
    : impl_(std::make_unique<Impl>(std::move(r))) {}
DungeonMapAssets::~DungeonMapAssets() = default;
bool DungeonMapAssets::Inspect(const DungeonMapDefinition &def,
                               DungeonMapArt &out) {
  out = {};
  try {
    return impl_->Inspect(def, out);
  } catch (...) {
    out = {};
    return false;
  }
}
bool DungeonMapAssets::Compose(const DungeonMapDefinition &def, int32_t floor,
                               uint64_t visited,
                               std::span<const DungeonRoomVariant> variants,
                               Image &out, MapBounds &bounds,
                               DungeonComposeStatus *status, bool map_item) {
  out = {};
  bounds = {};
  if (status)
    *status = DungeonComposeStatus::InvalidSource;
  try {
    DungeonMapArt ignored;
    if (!impl_->Inspect(def, ignored))
      return false;
    if (floor < def.floor_min ||
        floor >= int64_t(def.floor_min) + def.floor_count) {
      if (status)
        *status = DungeonComposeStatus::UnsupportedFloor;
      return false;
    }
    if (variants.size() > 64)
      return false;
    std::set<uint16_t> variant_numbers;
    for (const auto &v : variants)
      if (!variant_numbers.insert(v.room).second)
        return false;
    // Shared with the room-focus helper: all floors/visitation variants retain
    // identical native room bounds and raster rounding.
    auto frame = Frame(def, impl_->art);
    auto &quads = frame.quads;
    const auto result_bounds = frame.Bounds();
    const double left = frame.left, top = frame.top, scale = frame.scale;
    const int w = frame.w, h = frame.h;
    Image result(w, h);
    bool conservative = false, visible = false;
    std::stable_sort(quads.begin(), quads.end(),
                     [](const auto &a, const auto &b) {
                       return a.art->priority < b.art->priority;
                     });
    for (const auto &q : quads) {
      if (std::find(q.room->floors.begin(), q.room->floors.end(), floor) ==
          q.room->floors.end())
        continue;
      const bool was_visited = (visited & (uint64_t(1) << q.room->number)) != 0;
      if (!was_visited && !map_item)
        continue;
      const auto &a = *q.art;
      // Native25de544 reads the visited byte at+94. Unvisited follows
      // 25deab0's "{0}_{1:03}_t" (rodata287a85f), with multi-floor
      // 25de5b4 "{0}_{1:03}_t_f_{2:02}" (rodata28a63a3).
      // Only positively visited rooms evaluate conditional full-color art.
      std::string texture = a.area_name + (was_visited ? "_n" : "_t");
      if (!a.texture_floors.empty()) {
        const auto f =
            std::find(a.texture_floors.begin(), a.texture_floors.end(), floor);
        if (f == a.texture_floors.end())
          return false;
        char suffix[12];
        std::snprintf(suffix, sizeof(suffix), "_f_%02u",
                      unsigned(floor - def.floor_min));
        texture += suffix;
      }
      const auto variant =
          std::find_if(variants.begin(), variants.end(),
                       [&](const auto &v) { return v.room == a.room; });
      if (was_visited && !a.conditional_textures.empty()) {
        if (variant == variants.end() || !variant->known) {
          conservative = true;
          continue;
        }
        if (!variant->texture.empty()) {
          const auto native = std::find_if(
              a.conditional_textures.begin(), a.conditional_textures.end(),
              [&](const auto &v) { return v.second == variant->texture; });
          if (native == a.conditional_textures.end())
            return false;
          texture = native->second;
        }
      } else if (was_visited && variant != variants.end() &&
                 (!variant->texture.empty() || !variant->known))
        return false;
      bool missing = false;
      const auto *image = impl_->Texture(texture, missing);
      if (!image) {
        if (missing) {
          // Native sheets include areas with no drawable room sprite.
          // Missing art is never substituted with another reveal variant.
          conservative = true;
          continue;
        }
        return false;
      }
      const int x0 = std::max(0,
                              int(std::floor((q.x - q.w / 2 - left) * scale))),
                x1 =
                    std::min(w, int(std::ceil((q.x + q.w / 2 - left) * scale)));
      const int y0 =
                    std::max(0, int(std::floor((top - q.y - q.h / 2) * scale))),
                y1 = std::min(h, int(std::ceil((top - q.y + q.h / 2) * scale)));

      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          const double u = ((x + .5) / scale + left - (q.x - q.w / 2)) / q.w,
                       v = (q.y + q.h / 2 - (top - (y + .5) / scale)) / q.h;
          if (u < 0 || u >= 1 || v < 0 || v >= 1)
            continue;
          const int sx = std::clamp(int(u * image->w), 0, image->w - 1),
                    sy = std::clamp(int(v * image->h), 0, image->h - 1);
          const auto *pixel = image->At(sx, sy);
          if (pixel[3])
            visible = true;
          Over(result.At(x, y), pixel);
        }
    }
    out = std::move(result);
    bounds = result_bounds;
    if (status)
      *status = conservative ? DungeonComposeStatus::Conservative
                : visible    ? DungeonComposeStatus::Ready
                             : DungeonComposeStatus::Empty;
    return true;
  } catch (...) {
    out = {};
    bounds = {};
    return false;
  }
}
} // namespace echoes
