// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "acnh_bytes.h"
#include "acnh_tex.h"
#include "echoes_dungeon_assets.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#ifdef ECHOES_TEST_NATIVE_ASTC
#include "video_core/textures/astc.h"
#endif
namespace {
size_t checks{}, decoder_calls{}, tiled_calls{};
void Check(bool b, const char *why) {
  ++checks;
  if (!b)
    throw std::runtime_error(why);
}
struct Range {
  std::filesystem::path path;
  uint64_t start{}, size{};
  size_t Read(uint64_t at, void *out, size_t n) const {
    if (!out && !n)
      return size;
    if (at > size || n > size - at)
      return 0;
    std::ifstream f(path, std::ios::binary);
    f.seekg(std::streamoff(start + at));
    f.read(static_cast<char *>(out), std::streamsize(n));
    return size_t(f.gcount());
  }
};
struct Reader {
  std::filesystem::path root;
  std::map<std::string, Range, std::less<>> cache;
  size_t texture_reads{};
  Range Resolve(std::string_view key) {
    const auto split = key.find('#');
    Range file{root / std::string(key.substr(0, split)), 0, 0};
    if (!std::filesystem::exists(file.path))
      return {};
    file.size = std::filesystem::file_size(file.path);
    if (split == key.npos)
      return file;
    const auto wanted = key.substr(split + 1);
    std::array<uint8_t, 20> head{};
    Check(file.Read(0, head.data(), 20) == 20 &&
              !std::memcmp(head.data(), "SARC", 4),
          "native archive header");
    const auto data = acnh::bytes::Le32(head, 12);
    const auto offset = acnh::bytes::Le16(head, 4);
    std::array<uint8_t, 12> fat{};
    Check(file.Read(offset, fat.data(), 12) == 12, "native archive table");
    const auto count = acnh::bytes::Le16(fat, 6);
    Check(count <= 30000, "archive count bounded");
    const uint64_t names = offset + 12 + uint64_t(count) * 16 + 8;
    for (uint16_t i = 0; i < count; ++i) {
      std::array<uint8_t, 16> node{};
      Check(file.Read(offset + 12 + uint64_t(i) * 16, node.data(), 16) == 16,
            "archive entry");
      const uint32_t attr = acnh::bytes::Le32(node, 4),
                     begin = acnh::bytes::Le32(node, 8),
                     end = acnh::bytes::Le32(node, 12);
      Check(begin <= end && data + uint64_t(end) <= file.size,
            "archive member range");
      const uint64_t p = names + (attr & 0xffffff) * 4;
      if (p >= data)
        continue;
      std::array<char, 256> text{};
      const size_t n = std::min<uint64_t>(255, data - p);
      Check(file.Read(p, text.data(), n) == n, "archive member name");
      if (std::string_view(text.data()) == wanted)
        return {file.path, data + begin, end - begin};
    }
    return {};
  }
  size_t operator()(std::string_view key, uint64_t at, void *out, size_t n) {
    auto p = cache.find(key);
    if (p == cache.end())
      p = cache.emplace(std::string(key), Resolve(key)).first;
    if (out && key.find("MapRoomTex_") != key.npos)
      ++texture_reads;
    return p->second.Read(at, out, n);
  }
};
#ifdef ECHOES_TEST_NATIVE_ASTC
EdenDsmodBool Decode(void *, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                     const uint8_t *in, size_t ni, uint8_t *out, size_t no) {
  ++decoder_calls;
  tiled_calls += w == 1020 || h == 1020;
  if (!w || !h || w > 2048 || h > 2048 || no != uint64_t(w) * h * 4)
    return EDEN_DSMOD_FALSE;
  Tegra::Texture::ASTC::Decompress({in, ni}, w, h, 1, bw, bh, {out, no});
  return EDEN_DSMOD_TRUE;
}
#endif
size_t Ink(const echoes::Image &im) {
  size_t n = 0;
  for (size_t i = 3; i < im.rgba.size(); i += 4)
    n += im.rgba[i] != 0;
  return n;
}
void Save(const echoes::Image &im, const char *path) {
  std::ofstream f(path, std::ios::binary);
  f << "P6\n" << im.w << ' ' << im.h << "\n255\n";
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      const auto *p = im.At(x, y);
      for (int c = 0; c < 3; ++c)
        f.put(char((unsigned(p[c]) * p[3] + 30u * (255 - p[3])) / 255));
    }
}
bool Near(double a, double b) { return std::abs(a - b) < 0.00001; }
void FocusTests() {
  echoes::DungeonMapDefinition def;
  def.key = "L_DungeonTest";
  def.floor_min = 1;
  def.floor_count = 2;
  def.size_ratio = 2;
  echoes::DungeonRoom a, b;
  a.number = 1;
  a.lo = {0, 0, 0};
  a.hi = {20, 8, 10};
  a.floors = {1, 2};
  b.number = 2;
  b.lo = {500, 20, 500};
  b.hi = {510, 28, 510};
  b.floors = {2};
  def.rooms = {a, b};
  echoes::DungeonRoomArt aa, ab;
  aa.room = 1;
  aa.position_x = 5;
  aa.position_y = -7;
  aa.texture_scale = 1;
  ab.room = 2;
  ab.position_x = 35;
  ab.position_y = -7;
  ab.texture_scale = 1;
  echoes::DungeonMapArt art;
  art.key = def.key;
  art.rooms = {aa, ab};
  const auto project = [&](uint16_t room, int32_t floor = 1) {
    return echoes::ProjectDungeonRoomFocus(def, art, room, floor, 100, 100);
  };
  const auto first = project(1);
  Check(first.valid && Near(first.center_uv[0], 2. / 9) &&
            Near(first.center_uv[1], .5) && Near(first.extent_uv[0], 4. / 9) &&
            Near(first.extent_uv[1], .22),
        "room focus uses all-floor bounds and integer aspect-fit padding");
  auto no_area = def.rooms.front();
  no_area.number = 3;
  no_area.lo = {500, 500, 500};
  no_area.hi = {600, 600, 600};
  def.rooms.push_back(no_area);
  Check(
      project(1).center_uv == first.center_uv &&
          project(1).extent_uv == first.extent_uv,
      "native definition rooms without area art cannot shift drawable bounds");
  Check(!project(3).valid,
        "room without native area art cannot invent a room focus");
  def.rooms.pop_back();
  const auto second_floor = project(1, 2);
  Check(second_floor.valid && first.center_uv == second_floor.center_uv &&
            first.extent_uv == second_floor.extent_uv,
        "selected floor cannot refit a multi-floor room focus");
  const auto other = project(2, 2);
  Check(
      other.valid && Near(other.center_uv[0], 8. / 9) &&
          Near(other.center_uv[0] + other.extent_uv[0] / 2, 1),
      "room edge aligns with right raster bound despite separated world rooms");
  Check(!project(2).valid, "room must belong to the selected floor");
  auto floorless = a;
  floorless.floors.clear();
  def.rooms.insert(def.rooms.begin(), floorless);
  Check(project(1).center_uv == first.center_uv,
        "native duplicate floorless rows do not contribute geometry");
  def.rooms.erase(def.rooms.begin());
  def.rooms[0].side_view = true;
  const auto side = project(1);
  Check(side.valid && Near(side.extent_uv[1], .176) &&
            Near(side.extent_uv[0], first.extent_uv[0]),
        "side-view room extent uses world Y rather than top-down Z");
  def.rooms[0].side_view = false;
  art.rooms[0].texture_scale = .5f;
  const auto scaled = project(1);
  Check(
      scaled.valid && Near(scaled.center_uv[0], .125) &&
          Near(scaled.center_uv[1], .495) && Near(scaled.extent_uv[0], .25) &&
          Near(scaled.extent_uv[1], .125),
      "texture scale and odd integer padding retain the resource proportions");
  art.rooms[0].texture_scale = 1;
  def.rooms = {a};
  art.rooms = {aa};
  def.size_ratio = 1;
  def.rooms[0].hi = {10.25f, 8, 5.25f};
  art.rooms[0].position_x = art.rooms[0].position_y = 0;
  const auto fractional = project(1);
  Check(fractional.valid && Near(fractional.center_uv[0], 5.125 / 11) &&
            Near(fractional.center_uv[1], (22 + 2.625 / 6 * 55) / 100) &&
            Near(fractional.extent_uv[0], 10.25 / 11) &&
            Near(fractional.extent_uv[1], 5.25 / 6 * .55),
        "ceil raster dimensions participate in projection rather than ideal "
        "bounds");
  def.rooms[0].hi = {10000, 8, 5000};
  const auto capped = echoes::ProjectDungeonRoomFocus(def, art, 1, 1, 930, 842);
  Check(capped.valid && Near(capped.center_uv[0], .5) &&
            Near(capped.center_uv[1], (188 + 232.5) / 842) &&
            Near(capped.extent_uv[0], 1) &&
            Near(capped.extent_uv[1], 465. / 842),
        "2048 raster cap preserves aspect before canvas padding");
  const auto invalid = [&](const echoes::DungeonMapDefinition &d,
                           const echoes::DungeonMapArt &ar, uint16_t room = 1,
                           int32_t floor = 1, uint32_t w = 100,
                           uint32_t h = 100) {
    const auto focus =
        echoes::ProjectDungeonRoomFocus(d, ar, room, floor, w, h);
    Check(!focus.valid && focus.center_uv == std::array<float, 2>{} &&
              focus.extent_uv == std::array<float, 2>{},
          "invalid focus clears coordinates and extent");
  };
  invalid(def, art, 63);
  invalid(def, art, 1, 0);
  invalid(def, art, 1, INT32_MAX);
  invalid(def, art, 1, 1, 0);
  invalid(def, art, 1, 1, 100, 8193);
  auto bad_art = art;
  bad_art.key += "Other";
  invalid(def, bad_art);
  bad_art = art;
  bad_art.rooms.clear();
  invalid(def, bad_art);
  bad_art = art;
  bad_art.rooms.push_back(bad_art.rooms[0]);
  invalid(def, bad_art);
  bad_art = art;
  bad_art.rooms[0].room = 2;
  invalid(def, bad_art);
  bad_art = art;
  bad_art.rooms[0].texture_scale = 0;
  invalid(def, bad_art);
  bad_art = art;
  bad_art.rooms[0].position_x = std::numeric_limits<float>::quiet_NaN();
  invalid(def, bad_art);
  auto bad_def = def;
  bad_def.size_ratio = std::numeric_limits<float>::infinity();
  invalid(bad_def, art);
  bad_def = def;
  bad_def.rooms[0].hi[0] = bad_def.rooms[0].lo[0];
  invalid(bad_def, art);
  bad_def = def;
  bad_def.rooms[0].lo[1] = std::numeric_limits<float>::quiet_NaN();
  invalid(bad_def, art);
  bad_def = def;
  bad_def.rooms[0].side_view = true;
  bad_def.rooms[0].hi[1] = bad_def.rooms[0].lo[1];
  invalid(bad_def, art);
}
} // namespace
int main() {
  try {
    FocusTests();
    echoes::DungeonMapAssets absent({});
    echoes::DungeonMapArt art;
    echoes::Image image;
    echoes::MapBounds bounds;
    echoes::DungeonComposeStatus status;
    Check(!absent.Inspect({}, art), "missing definition fails closed");
    Check(!absent.Compose({}, 1, ~uint64_t(0), {}, image, bounds, &status) &&
              image.Empty(),
          "missing definition cannot reveal raw art");
    const char *root = std::getenv("ECHOES_ROMFS");
    if (!root) {
      std::cout << checks
                << " checks passed; original dungeon fixtures skipped "
                   "(ECHOES_ROMFS)\n";
      return 0;
    }
#ifdef ECHOES_TEST_NATIVE_ASTC
    acnh::SetAstcDecoder(nullptr, Decode);
#endif
    Reader reader{root, {}};
    const auto read = [&](std::string_view p, uint64_t a, void *o, size_t n) {
      return reader(p, a, o, n);
    };
    std::vector<echoes::DungeonMapDefinition> catalog;
    Check(echoes::LoadDungeonCatalog(read, catalog) && catalog.size() == 8,
          "native major dungeon catalog");
    echoes::DungeonMapAssets assets(read);
    size_t floors = 0;
    for (const auto &map : catalog) {
      Check(assets.Inspect(map, art) && !art.rooms.empty(),
            "native dynamic floor layout and area metadata");
      Check(art.key == map.key, "native map identity retained");
      std::vector<echoes::DungeonRoomVariant> known;
      for (const auto &room : art.rooms)
        if (!room.conditional_textures.empty())
          known.push_back({room.room, true, {}});
      echoes::MapBounds shared_bounds{};
      int shared_width = 0, shared_height = 0;
      for (int32_t floor = map.floor_min;
           floor < map.floor_min + map.floor_count; ++floor) {
        ++floors;

        const auto old_reads = reader.texture_reads;
        Check(assets.Compose(map, floor, 0, {}, image, bounds, &status),
              "unvisited floor safely composes");
        Check(Ink(image) == 0 && status == echoes::DungeonComposeStatus::Empty,
              "unvisited floor remains fully transparent");
        if (!shared_width) {
          shared_bounds = bounds;
          shared_width = image.w;
          shared_height = image.h;
        }
        const auto stable_frame = [&] {
          return image.w == shared_width && image.h == shared_height &&
                 bounds.left == shared_bounds.left &&
                 bounds.top == shared_bounds.top &&
                 bounds.width == shared_bounds.width &&
                 bounds.height == shared_bounds.height;
        };
        Check(stable_frame(),
              "all floors retain the dungeon's shared native coordinate frame");
        Check(reader.texture_reads == old_reads,
              "unvisited floors never load room texture source");
        // Derive expected focus from Compose's actual returned bounds and
        // rounded dimensions, independently of the shared private helper.
        for (const auto &area : art.rooms) {
          const auto room = std::find_if(
              map.rooms.begin(), map.rooms.end(), [&](const auto &r) {
                return r.number == area.room && !r.floors.empty();
              });
          Check(room != map.rooms.end(), "native room focus membership");
          if (std::find(room->floors.begin(), room->floors.end(), floor) ==
              room->floors.end())
            continue;
          const auto focus = echoes::ProjectDungeonRoomFocus(
              map, art, room->number, floor, 930, 842);
          const double raster_scale = std::min(
                           1., 2048. / std::max(bounds.width, bounds.height)),
                       fit = std::min(930. / image.w, 842. / image.h);
          const int fw = std::clamp(int(std::lround(image.w * fit)), 1, 930),
                    fh = std::clamp(int(std::lround(image.h * fit)), 1, 842),
                    dx = (930 - fw) / 2, dy = (842 - fh) / 2;
          const double cx = area.position_x * double(map.size_ratio),
                       cy = -area.position_y * double(map.size_ratio),
                       ex = (double(room->hi[0]) - room->lo[0]) *
                            map.size_ratio * area.texture_scale,
                       ey = (double(room->hi[room->side_view ? 1 : 2]) -
                             room->lo[room->side_view ? 1 : 2]) *
                            map.size_ratio * area.texture_scale;
          Check(
              focus.valid &&
                  Near(focus.center_uv[0],
                       (dx + (cx - bounds.left) * raster_scale / image.w * fw) /
                           930) &&
                  Near(focus.center_uv[1],
                       (dy + (bounds.top - cy) * raster_scale / image.h * fh) /
                           842) &&
                  Near(focus.extent_uv[0],
                       ex * raster_scale / image.w * fw / 930) &&
                  Near(focus.extent_uv[1],
                       ey * raster_scale / image.h * fh / 842),
              "native room focus aligns with Compose bounds/dimensions/padded "
              "resource");
          Check(focus.center_uv[0] - focus.extent_uv[0] / 2 >= -.00001f &&
                    focus.center_uv[0] + focus.extent_uv[0] / 2 <= 1.00001f &&
                    focus.center_uv[1] - focus.extent_uv[1] / 2 >= -.00001f &&
                    focus.center_uv[1] + focus.extent_uv[1] / 2 <= 1.00001f,
                "native room bounds remain inside the normalized padded "
                "resource");
        }
        Check(assets.Compose(map, floor, ~uint64_t(0), known, image, bounds,
                             &status),
              "visited floor composes native room sprites");
        Check(stable_frame(),
              "discovery cannot change room scale or map origin");
        const auto visited_raster = image.rgba;
        Check(assets.Compose(map, floor, 0, {}, image, bounds, &status, true),
              "map item composes original unvisited gray room art");
        Check(stable_frame(), "map ownership cannot refit the selected floor");
        Check((Ink(image) > 0 ||
               status == echoes::DungeonComposeStatus::Conservative) &&
                  (status == echoes::DungeonComposeStatus::Ready ||
                   status == echoes::DungeonComposeStatus::Conservative) &&
                  image.rgba != visited_raster,
              "unvisited map-item art differs from full-color visited rooms");
        Check(assets.Compose(map, floor, ~uint64_t(0), known, image, bounds,
                             &status, true) &&
                  image.rgba == visited_raster,
              "map ownership does not alter positively visited room art");
        Check(!image.Empty() && image.w <= 2048 && image.h <= 2048 &&
                  Ink(image) > 0,
              "visited floor native raster bounded and visible");
        if (map.key == "L_Dungeon001" && floor == map.floor_min)
          if (const char *output = std::getenv("ECHOES_DUNGEON_ASSET_OUTPUT"))
            Save(image, output);
      }
    }
    Check(floors == 24, "all original major dungeon floors covered");
#ifdef ECHOES_TEST_NATIVE_ASTC
    Check(decoder_calls > 0 && tiled_calls > 0,
          "oversized original rooms use bounded tiled ASTC decode calls");
#endif
    const auto &map = catalog.front();
    Check(assets.Inspect(map, art), "restore first map metadata");
    const auto no_gray =
        std::find_if(map.rooms.begin(), map.rooms.end(),
                     [](const auto &r) { return r.number == 1; });
    Check(no_gray != map.rooms.end() && !no_gray->floors.empty(),
          "original absent-gray room fixture exists");
    const uint64_t no_gray_bit = uint64_t(1) << no_gray->number;
    Check(assets.Compose(map, no_gray->floors.front(), no_gray_bit, {}, image,
                         bounds, &status) &&
              Ink(image) > 0,
          "absent-gray fixture has original full-color visited room art");
    Check(assets.Compose(map, no_gray->floors.front(), 0, {}, image, bounds,
                         &status, true) &&
              Ink(image) > 0,
          "map ownership exposes available original gray rooms only");
    // Restrict geometry to this native room to demonstrate there is no _n
    // fallback when its original _t counterpart does not exist.
    auto single = map;
    single.key = map.key;
    single.rooms = {*no_gray};
    echoes::DungeonMapAssets one(read);
    Check(one.Compose(single, no_gray->floors.front(), 0, {}, image, bounds,
                      &status, true) &&
              Ink(image) == 0 &&
              status == echoes::DungeonComposeStatus::Conservative,
          "missing native gray texture never falls back to visited room art");
    Check(!assets.Compose(map, map.floor_min - 1, ~uint64_t(0), {}, image,
                          bounds, &status) &&
              image.Empty() && bounds.width == 0 &&
              status == echoes::DungeonComposeStatus::UnsupportedFloor,
          "out-of-range floor fails closed");
    const auto conditional =
        std::find_if(art.rooms.begin(), art.rooms.end(), [](const auto &r) {
          return !r.conditional_textures.empty();
        });
    Check(conditional != art.rooms.end(),
          "original conditional room fixture exists");
    const auto room =
        std::find_if(map.rooms.begin(), map.rooms.end(), [&](const auto &r) {
          return r.number == conditional->room;
        });
    Check(room != map.rooms.end() && !room->floors.empty(),
          "conditional room native floor known");
    const auto floor = room->floors[0];
    const auto bit = uint64_t(1) << room->number;
    Check(assets.Compose(map, floor, bit, {}, image, bounds, &status) &&
              Ink(image) == 0 &&
              status == echoes::DungeonComposeStatus::Conservative,
          "unknown conditional room stays hidden");
    const std::array<echoes::DungeonRoomVariant, 1> base = {
        {{room->number, true, {}}}};
    Check(assets.Compose(map, floor, bit, base, image, bounds, &status) &&
              Ink(image) > 0,
          "known base state reveals only positively visited room");
    const auto baseline = image.rgba;
    const std::array<echoes::DungeonRoomVariant, 1> changed = {
        {{room->number, true, conditional->conditional_textures[0].second}}};
    Check(assets.Compose(map, floor, bit, changed, image, bounds, &status) &&
              Ink(image) > 0 && image.rgba != baseline,
          "native conditional texture changes actual room terrain");
    const std::array<echoes::DungeonRoomVariant, 1> invalid = {
        {{room->number, true, "FieldTerrain_00^_A"}}};
    Check(!assets.Compose(map, floor, bit, invalid, image, bounds, &status) &&
              image.Empty() && bounds.width == 0,
          "non-native texture override cannot leak unrelated art");
    Check(
        assets.Compose(map, floor, 0, invalid, image, bounds, &status, true) &&
            (status == echoes::DungeonComposeStatus::Ready ||
             status == echoes::DungeonComposeStatus::Conservative),
        "unvisited map-item gray art ignores irrelevant conditional override");
    const auto gray_raster = image.rgba;
    Check(
        assets.Compose(map, floor, 0, changed, image, bounds, &status, true) &&
            image.rgba == gray_raster,
        "conditional full-color replacement never leaks into unvisited gray "
        "art");
    Check(assets.Compose(map, floor, 0, base, image, bounds, &status) &&
              Ink(image) == 0,
          "discovery rollback withdraws rendered room");
    echoes::DungeonMapAssets corrupted(
        [&](std::string_view p, uint64_t a, void *o, size_t n) {
          const auto got = read(p, a, o, n);
          if (got && o && a == 0 && p.find("MapRoomTex_") != p.npos)
            static_cast<uint8_t *>(o)[0] = 0;
          return got;
        });
    Check(!corrupted.Compose(map, floor, bit, base, image, bounds, &status) &&
              image.Empty() && bounds.width == 0,
          "corrupt original room archive fails closed without stale raster");
    std::cout << checks << " checks passed with 8 original dungeon maps / "
              << floors << " floors\n";
  } catch (const std::exception &e) {
    std::cerr << "FAILED: " << e.what() << '\n';
    return 1;
  }
}
