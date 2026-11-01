// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_exploration.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string_view>
namespace ot0 {
namespace {
constexpr uint64_t Limit = uint64_t{1} << 39;
[[noreturn]] void Bad() {
  throw std::runtime_error("Unavailable native minimap");
}
bool Pointer(uint64_t p) { return p && p < Limit && p % 8 == 0; }
template <class T> T At(const uint8_t *p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
// The bounds actor may provide an explicit logical section name. Unreal
// constructs the resource FName from this UTF-16 FString, not from the actor's
// generated level-instance suffix (native3a21124..3a21180).
std::string LogicalName(const std::vector<uint8_t> &bytes) {
  if (bytes.size() < 4 || bytes.size() % 2 ||
      At<uint16_t>(&bytes[bytes.size() - 2]) != 0)
    Bad();
  std::string out;
  const size_t chars = bytes.size() / 2 - 1;
  for (size_t i = 0; i < chars; ++i) {
    uint32_t cp = At<uint16_t>(&bytes[i * 2]);
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (++i >= chars)
        Bad();
      const auto low = At<uint16_t>(&bytes[i * 2]);
      if (low < 0xdc00 || low > 0xdfff)
        Bad();
      cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
    } else if (cp >= 0xdc00 && cp <= 0xdfff)
      Bad();
    if (cp < 32)
      Bad();
    if (cp < 128)
      out += char(cp);
    else if (cp < 2048) {
      out += char(0xc0 | (cp >> 6));
      out += char(0x80 | (cp & 63));
    } else if (cp < 65536) {
      out += char(0xe0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 63));
      out += char(0x80 | (cp & 63));
    } else {
      out += char(0xf0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 63));
      out += char(0x80 | ((cp >> 6) & 63));
      out += char(0x80 | (cp & 63));
    }
  }
  return out;
}
bool SameLogicalName(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  const auto fold = [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
  };
  for (size_t i = 0; i < a.size(); ++i)
    if (fold(static_cast<unsigned char>(a[i])) !=
        fold(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}
struct Reader {
  const ReadMemory &f;
  uint64_t main;
  unsigned calls{};
  void raw(uint64_t at, void *out, size_t n) {
    if (++calls > 512 || !at || at >= Limit || !n || n > 65536 ||
        n > Limit - at || !f(at, out, n))
      Bad();
  }
  template <class T> T get(uint64_t at) {
    T out;
    raw(at, &out, sizeof out);
    return out;
  }
  uint64_t ptr(uint64_t at) {
    auto p = get<uint64_t>(at);
    if (!Pointer(p))
      Bad();
    return p;
  }
  std::string name(uint32_t index, uint32_t number) {
    if (index >> 16 > 8191)
      Bad();
    auto block = ptr(main + 0x6c91440 + 0x38 + 8 * (index >> 16));
    auto ent = block + 2 * (index & 65535);
    auto head = get<uint16_t>(ent);
    size_t len = head >> 6;
    if (!len || len > 1023)
      Bad();
    std::vector<uint8_t> b(len * (head & 1 ? 2 : 1));
    raw(ent + 2, b.data(), b.size());
    std::string result;
    if (head & 1) {
      for (size_t i = 0; i < len; ++i) {
        uint32_t cp = At<uint16_t>(&b[i * 2]);
        if (cp >= 0xd800 && cp <= 0xdbff) {
          if (++i >= len)
            Bad();
          auto low = At<uint16_t>(&b[i * 2]);
          if (low < 0xdc00 || low > 0xdfff)
            Bad();
          cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if (cp >= 0xdc00 && cp <= 0xdfff)
          Bad();
        if (!cp)
          Bad();
        if (cp < 128)
          result += char(cp);
        else if (cp < 2048) {
          result += char(0xc0 | (cp >> 6));
          result += char(0x80 | (cp & 63));
        } else if (cp < 65536) {
          result += char(0xe0 | (cp >> 12));
          result += char(0x80 | ((cp >> 6) & 63));
          result += char(0x80 | (cp & 63));
        } else {
          result += char(0xf0 | (cp >> 18));
          result += char(0x80 | ((cp >> 12) & 63));
          result += char(0x80 | ((cp >> 6) & 63));
          result += char(0x80 | (cp & 63));
        }
      }
    } else {
      for (auto c : b) {
        if (c < 32 || c > 126)
          Bad();
        result += char(c);
      }
    }
    if (number) {
      if (number > 1000000000)
        Bad();
      result += "_" + std::to_string(number - 1);
    }
    return result;
  }
  std::string
  texture(uint64_t object,
          std::string_view prefix = "/Game/UI/MiniMap/Texture/MAP/") {
    auto header = get<std::array<uint8_t, 0x28>>(object);
    auto flags = At<uint32_t>(&header[8]);
    if (flags & 0x40000030)
      Bad();
    const auto n =
        name(At<uint32_t>(&header[0x18]), At<uint32_t>(&header[0x1c]));
    auto outer = At<uint64_t>(&header[0x20]);
    if (!Pointer(outer) || outer == object)
      Bad();
    auto ph = get<std::array<uint8_t, 0x28>>(outer);
    auto package = name(At<uint32_t>(&ph[0x18]), At<uint32_t>(&ph[0x1c]));
    // Native UTexture2D assets have the package as their direct outer. Reject
    // dynamic/generated textures and paths outside the current map asset
    // family.
    if (!package.starts_with(prefix) || !package.ends_with("/" + n) ||
        package.find("..") != std::string::npos)
      Bad();
    return package + "." + n;
  }
};
uint32_t MapNameId(Reader &r, uint32_t map_id, uint64_t map_name) {
  try {
    const auto level = r.ptr(r.main + 0x7cc6e80);
    const auto data = r.ptr(r.main + 0x7cc7448);
    if (r.get<uint64_t>(level + 0x35c) != map_name)
      return 0;
    const auto header = r.get<std::array<uint8_t, 16>>(data + 0x358);
    const auto rows = At<uint64_t>(header.data());
    const int count = At<int32_t>(&header[8]), cap = At<int32_t>(&header[12]);
    if (!Pointer(rows) || count < 1 || count > 4096 || cap < count ||
        cap > 4096)
      return 0;
    // The MapDB rows are static per load: reuse the last unique match for the same map and the
    // same table header instead of rescanning up to 4096 records on every read.
    // Only the matched row is re-read to confirm it.
    struct Cached { uint32_t map_id{}; uint64_t map_name{}, data{}; std::array<uint8_t, 16> header{}; uint32_t id{}; int row{-1}; };
    thread_local Cached cached;
    if (cached.id && cached.map_id == map_id && cached.map_name == map_name && cached.data == data &&
        cached.header == header && cached.row >= 0 && cached.row < count) {
      const auto row = r.get<std::array<uint8_t, 0x108>>(rows + uint64_t(cached.row) * 0x108);
      if (At<uint32_t>(row.data() + 8) == map_id && At<uint64_t>(row.data() + 0x50) == map_name &&
          At<uint32_t>(row.data() + 0x68) == cached.id && r.ptr(r.main + 0x7cc6e80) == level &&
          r.get<uint64_t>(level + 0x35c) == map_name)
        return cached.id;
    }
    int matched_row = -1;
    uint32_t id = 0;
    unsigned matches = 0;
    // This is the native MapDB record array, not an object/heap search.
    for (int start = 0; start < count; start += 248) {
      const auto take = std::min(248, count - start);
      std::vector<uint8_t> chunk(size_t(take) * 0x108);
      r.raw(rows + uint64_t(start) * 0x108, chunk.data(), chunk.size());
      for (int i = 0; i < take; ++i) {
        const auto *row = &chunk[size_t(i) * 0x108];
        if (At<uint32_t>(row + 8) == map_id &&
            At<uint64_t>(row + 0x50) == map_name) {
          ++matches;
          id = At<uint32_t>(row + 0x68);
          matched_row = start + i;
        }
      }
    }
    if (matches != 1 || id > 100000000 || r.ptr(r.main + 0x7cc6e80) != level ||
        r.ptr(r.main + 0x7cc7448) != data ||
        r.get<uint64_t>(level + 0x35c) != map_name ||
        r.get<std::array<uint8_t, 16>>(data + 0x358) != header)
      return 0;
    cached = {map_id, map_name, data, header, id, matched_row};
    return id;
  } catch (const std::exception &) {
    return 0;
  }
}
uint32_t Crc32(const uint8_t *p, size_t n) {
  uint32_t c = ~uint32_t{};
  while (n--) {
    c ^= *p++;
    for (unsigned i = 0; i < 8; ++i)
      c = (c >> 1) ^ (0xedb88320u & uint32_t(-int32_t(c & 1)));
  }
  return ~c;
}
bool Finite(double v) { return std::isfinite(v) && std::abs(v) <= 1000000000; }
} // namespace
MapPoint WorldToTexture(const ExplorationSnapshot &m, double x, double y) {
  return {(y * m.axis.x + m.offset.x) * m.scale.x + m.translation.x,
          (x * m.axis.y + m.offset.y) * m.scale.y + m.translation.y};
}
MapPoint TextureToWorld(const ExplorationSnapshot &m, double x, double y) {
  if (m.scale.x <= 0 || m.scale.y <= 0 || std::abs(m.axis.x) != 1 ||
      std::abs(m.axis.y) != 1)
    return {NAN, NAN};
  return {((y - m.translation.y) / m.scale.y - m.offset.y) / m.axis.y,
          ((x - m.translation.x) / m.scale.x - m.offset.x) / m.axis.x};
}
MapCell WorldCell(const ExplorationSnapshot &m, double x, double y) {
  if (!m.block_width)
    return {-1, -1};
  const double cx = std::trunc((x - m.bounds_min[0]) / m.block_width);
  const double cy = std::trunc((y - m.bounds_min[1]) / m.block_width);
  if (!std::isfinite(cx) || !std::isfinite(cy) || cx < INT32_MIN ||
      cx > INT32_MAX || cy < INT32_MIN || cy > INT32_MAX)
    return {-1, -1};
  return {int32_t(cx), int32_t(cy)};
}
MapPoint NativeTexturePoint(const ExplorationSnapshot &m, double x, double y,
                            uint32_t width, uint32_t height) {
  if (!m.reveal_texture_world_size || !width || !height)
    return {NAN, NAN};
  const double ratio = double(width) / m.reveal_texture_world_size;
  return {double(width) / 2 + (y - (m.bounds_min[1] + m.bounds_max[1]) / 2 +
                               m.reveal_world_offset.y) *
                                  ratio,
          double(height) / 2 - (x - (m.bounds_min[0] + m.bounds_max[0]) / 2 +
                                m.reveal_world_offset.x) *
                                   ratio};
}
std::array<MapPoint, 2> CellTextureBounds(const ExplorationSnapshot &m,
                                          MapCell cell) {
  double x = m.bounds_min[0] + double(cell.x) * m.block_width,
         y = m.bounds_min[1] + double(cell.y) * m.block_width;
  auto a = WorldToTexture(m, x, y),
       b = WorldToTexture(m, x + m.block_width, y + m.block_width);
  return {{{std::min(a.x, b.x), std::min(a.y, b.y)},
           {std::max(a.x, b.x), std::max(a.y, b.y)}}};
}
ExplorationSnapshot ReadExploration(const ReadMemory &read, uint64_t main,
                                    int64_t relocation_delta) {
  ExplorationSnapshot out;
  try {
    if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8)
      Bad();
    Reader r{read, main};
    auto app = r.ptr(main + 0x7cc8cc0);
    if (!MatchesVtable(r.get<uint64_t>(app), main + 0x6207610,
                       relocation_delta))
      Bad();
    const auto state = r.get<std::array<uint8_t, 3>>(app + 0x2f0);
    out.state = state[0];
    if (out.state != 23 && out.state != 24)
      Bad();
    const auto mm = r.ptr(main + 0x7cc7588);
    const auto mh = r.get<std::array<uint8_t, 0x28>>(mm);
    if (!MatchesVtable(At<uint64_t>(mh.data()), main + 0x63be0f0,
                       relocation_delta) ||
        (At<uint32_t>(&mh[8]) & 0x40000030))
      Bad();
    const auto m = r.get<std::array<uint8_t, 0x98>>(mm + 0x348);
    out.map_id = At<uint32_t>(m.data());
    if (out.map_id == 0 || out.map_id > 100000000)
      Bad();
    out.map_key = r.name(At<uint32_t>(&m[4]), At<uint32_t>(&m[8]));
    std::memcpy(out.world_xyz.data(), &m[0x10], 24);
    if (!std::all_of(out.world_xyz.begin(), out.world_xyz.end(), Finite))
      Bad();
    const auto resource = r.ptr(mm + 0x448);
    const auto rb = r.get<std::array<uint8_t, 0x110>>(resource);
    if (rb[0xf8] != 1)
      Bad();
    const auto fname = At<uint64_t>(rb.data());
    out.section_key = r.name(uint32_t(fname), uint32_t(fname >> 32));
    const auto volume = At<uint64_t>(&rb[0x100]);
    if (!Pointer(volume))
      Bad();
    const auto vh = r.get<std::array<uint8_t, 0x370>>(volume);
    const auto volume_class = r.ptr(main + 0x7cbbcd0);
    if (At<uint64_t>(&vh[0x10]) != volume_class ||
        (At<uint32_t>(&vh[8]) & 0x40000030))
      Bad();
    const auto logical_header = At<std::array<uint8_t, 16>>(&vh[0x338]);
    const auto logical_ptr = At<uint64_t>(logical_header.data());
    const int logical_count = At<int32_t>(&logical_header[8]);
    const int logical_capacity = At<int32_t>(&logical_header[12]);
    if (logical_count < 0 || logical_count > 1024 ||
        logical_capacity < logical_count || logical_capacity > 4096)
      Bad();
    std::vector<uint8_t> logical_bytes;
    if (logical_count > 1) {
      if (!Pointer(logical_ptr))
        Bad();
      logical_bytes.resize(size_t(logical_count) * 2);
      r.raw(logical_ptr, logical_bytes.data(), logical_bytes.size());
      if (!SameLogicalName(LogicalName(logical_bytes), out.section_key))
        Bad();
    } else if (At<uint64_t>(&vh[0x18]) != fname)
      Bad();
    if (vh[0x2c9] > 1)
      Bad();
    out.filter_on = vh[0x2c9] != 0;
    std::memcpy(&out.reveal_world_offset, &vh[0x2e0], 16);
    if (!Finite(out.reveal_world_offset.x) ||
        !Finite(out.reveal_world_offset.y))
      Bad();
    const auto mask_texture = r.ptr(mm + 0x7e0);
    {
      if (!Pointer(mask_texture))
        Bad();
      const auto world_size = At<int32_t>(&vh[0x2f8]);
      const auto stamp_size = At<int32_t>(&vh[0x310]);
      const auto multiplier = At<float>(&vh[0x314]);
      if (world_size < 1 || world_size > 10000000 || stamp_size < 1 ||
          stamp_size > 100000 || !std::isfinite(multiplier) ||
          multiplier <= 0 || multiplier > 100)
        Bad();
      out.reveal_texture_world_size = uint32_t(world_size);
      out.reveal_stamp_world_size = double(stamp_size) * multiplier;
      out.reveal_mask_path = r.texture(mask_texture, "/Game/UI/MiniMap/BP/");
      if (out.reveal_mask_path != "/Game/UI/MiniMap/BP/T_mask.T_mask")
        Bad();
    }
    const auto tex = At<uint64_t>(&vh[0x2d8]);
    if (!Pointer(tex))
      Bad();
    out.texture_path = r.texture(tex);
    // Map IDs/FNames can advance before a previous section resource retires.
    // Only accept an asset owned by the current native map's own folder.
    const auto asset_family = "/" + out.map_key + "/" + out.map_key + "_";
    if (out.texture_path.find(asset_family) == std::string::npos)
      Bad();
    out.axis = r.get<MapPoint>(main + 0x7cc7640);
    std::memcpy(&out.offset, &rb[0x98], 16);
    std::memcpy(&out.translation, &rb[0xa8], 16);
    std::memcpy(&out.scale, &rb[0xb8], 16);
    std::memcpy(out.bounds_min.data(), &rb[0xc8], 24);
    std::memcpy(out.bounds_max.data(), &rb[0xe0], 24);
    for (auto p : {out.axis, out.offset, out.translation, out.scale})
      if (!Finite(p.x) || !Finite(p.y))
        Bad();
    if (std::abs(out.axis.x) != 1 || std::abs(out.axis.y) != 1 ||
        out.scale.x <= 0 || out.scale.y <= 0 || out.scale.x > 100 ||
        out.scale.y > 100)
      Bad();
    for (unsigned i = 0; i < 3; ++i)
      if (!Finite(out.bounds_min[i]) || !Finite(out.bounds_max[i]) ||
          out.bounds_min[i] >= out.bounds_max[i])
        Bad();
    for (unsigned i = 0; i < 2; ++i)
      if (out.world_xyz[i] < out.bounds_min[i] ||
          out.world_xyz[i] > out.bounds_max[i])
        Bad();
    out.texture_position =
        WorldToTexture(out, out.world_xyz[0], out.world_xyz[1]);
    const auto controller = r.ptr(main + 0x7cc6288);
    const auto character = r.ptr(controller + 0x290);
    const auto character_header = r.get<std::array<uint8_t, 0x28>>(character);
    if (!MatchesVtable(At<uint64_t>(character_header.data()), main + 0x6416cf0,
                       relocation_delta) ||
        (At<uint32_t>(&character_header[8]) & 0x40000030))
      Bad();
    out.native_direction = r.get<uint8_t>(character + 0x920);
    // The four cardinals are source-named; diagonal subdivisions remain
    // uncorrelated and intentionally have no claimed heading angle.
    out.heading_valid = out.native_direction <= 3;

    auto manager = r.ptr(main + 0x7cc8540), save = r.ptr(manager + 0x2c8);
    const auto header = r.get<std::array<uint8_t, 0x58>>(save + 0xd18);
    int width = At<int32_t>(&header[0x50]);
    out.block_width = width < 1 ? 200 : uint32_t(width);
    if (out.block_width > 100000)
      Bad();
    const auto nodes = At<uint64_t>(header.data());
    const int count = At<int32_t>(&header[8]), cap = At<int32_t>(&header[0xc]),
              bits = At<int32_t>(&header[0x28]),
              maxbits = At<int32_t>(&header[0x2c]),
              free_count = At<int32_t>(&header[0x34]),
              hashsize = At<int32_t>(&header[0x48]);
    if (count < 0 || count > 4096 || cap < count || cap > 8192 ||
        bits != count || maxbits < bits || maxbits > 16384 || free_count < 0 ||
        free_count > count)
      Bad();
    std::array<uint8_t, 0x28> matched{};
    uint64_t matched_address{};
    if (count > free_count) {
      if (!Pointer(nodes) || hashsize < 1 || hashsize > 8192 ||
          (hashsize & (hashsize - 1)))
        Bad();
      std::array<uint8_t, 12> key{};
      std::memcpy(key.data(), &out.map_id, 4);
      std::memcpy(key.data() + 4, &fname, 8);
      auto hp = At<uint64_t>(&header[0x40]);
      if (!hp)
        hp = save + 0xd18 + 0x38;
      if (hp % 4 || hp >= Limit)
        Bad();
      const uint32_t bucket =
          Crc32(key.data(), key.size()) & uint32_t(hashsize - 1);
      auto index = r.get<int32_t>(hp + bucket * 4);
      std::array<int32_t, 64> seen{};
      unsigned n = 0;
      while (index != -1) {
        if (index < 0 || index >= count || n == seen.size() ||
            std::find(seen.begin(), seen.begin() + n, index) !=
                seen.begin() + n)
          Bad();
        seen[n++] = index;
        auto bp = At<uint64_t>(&header[0x20]);
        if (!bp)
          bp = save + 0xd18 + 0x10;
        if (bp % 4 || bp >= Limit)
          Bad();
        auto allocation = r.get<uint32_t>(bp + uint32_t(index / 32) * 4);
        if (!(allocation & (1u << (index % 32))))
          Bad();
        auto at = nodes + uint64_t(index) * 0x28;
        const auto node = r.get<std::array<uint8_t, 0x28>>(at);
        if (!std::memcmp(node.data(), key.data(), 12)) {
          matched = node;
          matched_address = at;
          break;
        }
        index = At<int32_t>(&node[0x20]);
      }
      if (matched_address) {
        auto p = At<uint64_t>(&matched[0x10]);
        int num = At<int32_t>(&matched[0x18]),
            capacity = At<int32_t>(&matched[0x1c]);
        if (num < 0 || num > 8192 || capacity < num || capacity > 16384 ||
            (num && !Pointer(p)))
          Bad();
        out.revealed_cells.resize(num);
        if (num)
          r.raw(p, out.revealed_cells.data(), num * sizeof(MapCell));
        for (auto cell : out.revealed_cells)
          if (cell.x < 0 || cell.y < 0 || cell.x > 65535 || cell.y > 65535)
            Bad();
      }
    }
    out.map_name_id = MapNameId(r, out.map_id, At<uint64_t>(&m[4]));
    if (r.ptr(main + 0x7cc8cc0) != app ||
        r.get<std::array<uint8_t, 3>>(app + 0x2f0) != state ||
        r.ptr(main + 0x7cc7588) != mm ||
        r.get<std::array<uint8_t, 0x28>>(mm) != mh ||
        r.get<uint32_t>(mm + 0x348) != out.map_id ||
        r.get<uint64_t>(mm + 0x34c) != At<uint64_t>(&m[4]) ||
        r.ptr(mm + 0x448) != resource ||
        r.get<std::array<uint8_t, 0x110>>(resource) != rb ||
        r.ptr(main + 0x7cc8540) != manager || r.ptr(manager + 0x2c8) != save ||
        r.get<std::array<uint8_t, 0x58>>(save + 0xd18) != header ||
        r.ptr(main + 0x7cbbcd0) != volume_class)
      Bad();
    if (matched_address &&
        r.get<std::array<uint8_t, 0x28>>(matched_address) != matched)
      Bad();
    if (r.get<std::array<uint8_t, 16>>(volume + 0x338) != logical_header)
      Bad();
    if (!logical_bytes.empty()) {
      std::vector<uint8_t> again(logical_bytes.size());
      r.raw(logical_ptr, again.data(), again.size());
      if (again != logical_bytes)
        Bad();
    }
    const auto volume_header = r.get<std::array<uint8_t, 0x28>>(volume);
    if (std::memcmp(volume_header.data(), vh.data(), volume_header.size()) ||
        r.get<uint64_t>(volume + 0x2d8) != tex ||
        r.get<uint8_t>(volume + 0x2c9) != vh[0x2c9] ||
        r.get<MapPoint>(volume + 0x2e0) != out.reveal_world_offset ||
        r.get<uint64_t>(mm + 0x7e0) != mask_texture ||
        r.get<uint32_t>(volume + 0x2f8) != At<uint32_t>(&vh[0x2f8]) ||
        r.get<uint64_t>(volume + 0x310) != At<uint64_t>(&vh[0x310]))
      Bad();
    if (r.ptr(main + 0x7cc6288) != controller ||
        r.ptr(controller + 0x290) != character ||
        r.get<std::array<uint8_t, 0x28>>(character) != character_header)
      Bad();
    out.valid = true;
    return out;
  } catch (const std::exception &) {
    ExplorationSnapshot cleared;
    cleared.state = out.state;
    return cleared;
  }
}
} // namespace ot0
