// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_minimap.h"
#include "ot0_exploration.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
namespace ot0 {
namespace {
constexpr uint64_t Limit = uint64_t{1} << 39;
// Nine fully proved native markers use about2,000 small reads, including
// geometry and every unbound animation frame. Leave bounded room for denser
// current sections without changing the marker/array traversal limits.
constexpr unsigned MaxReads = 8192;
bool Pointer(uint64_t p) { return p >= 0x1000000 && p < Limit && p % 8 == 0; }
struct Reader {
  const ReadMemory &read;
  uint64_t main;
  int64_t delta;
  mutable unsigned calls{};
  template <class T> bool get(uint64_t p, T &v) const {
    return ++calls <= MaxReads && p && p < Limit && sizeof(T) <= Limit - p &&
           read && read(p, &v, sizeof v);
  }
  bool object(uint64_t p, uint64_t offset) const {
    uint64_t vt{};
    uint32_t flags{};
    return Pointer(p) && get(p, vt) &&
           MatchesVtable(vt, main + offset, delta) && get(p + 8, flags) &&
           !(flags & 0x40000030);
  }
  bool widget(uint64_t p, HudRow &out, bool require_visible = true) const {
    uint64_t slate{}, controller{}, vt{}, self{};
    int32_t refs{};
    std::array<uint8_t, 2> visibility{};
    // MiniMapUI constructor370c4ac stores vtable63c0c48. Cached Slate is
    // the same UUserWidget/SObjectWidget ownership contract as party HUD.
    if (!object(p, 0x63c0c48) || !get(p + 0x108, slate) || !Pointer(slate) ||
        !get(p + 0x110, controller) || !Pointer(controller) ||
        !get(controller + 8, refs) || refs <= 0 || refs > 1000000 ||
        !get(slate, vt) || !MatchesVtable(vt, main + 0x5d011e8, delta) ||
        !get(slate + 8, self) || self != slate ||
        !get(slate + 0x184, visibility) || visibility[1] != 1 ||
        (visibility[0] != 1 && visibility[0] != 0x11 && visibility[0] != 0x19 &&
         (require_visible || (visibility[0] != 2 && visibility[0] != 4))))
      return false;
    out = {p, slate, controller};
    return true;
  }
  bool name(uint64_t value, std::string &out) const {
    const uint32_t index = uint32_t(value), number = uint32_t(value >> 32);
    uint64_t block{};
    uint16_t head{};
    if (number || index >> 16 > 8191 ||
        !get(main + 0x6c91478 + 8 * (index >> 16), block) || !Pointer(block) ||
        !get(block + 2 * (index & 65535), head) || (head & 1) || !(head >> 6) ||
        (head >> 6) > 512)
      return false;
    std::array<char, 512> text{};
    if (++calls > MaxReads ||
        !read(block + 2 * (index & 65535) + 2, text.data(), head >> 6))
      return false;
    out.clear();
    for (unsigned i = 0; i < unsigned(head >> 6); ++i) {
      if (text[i] < 32 || text[i] > 126)
        return false;
      out += text[i];
    }
    return true;
  }
  bool frame(uint64_t p, std::string &out) const {
    uint64_t fn{}, outer{}, packagefn{};
    uint32_t flags{};
    std::string n, package;
    if (!object(p, 0x60f4cd8) || !get(p + 0x18, fn) || !name(fn, n) ||
        !get(p + 0x20, outer) || !Pointer(outer) || outer == p ||
        !get(outer + 8, flags) || flags & 0x40000030 ||
        !get(outer + 0x18, packagefn) || !name(packagefn, package) ||
        !n.starts_with("UiTX_MainMenu_MiniMapIcon_") || !n.ends_with("_png") ||
        package != "/Game/UI/Menu/Texture/Frames/" + n)
      return false;
    out = package + "." + n;
    return true;
  }
  bool weak(uint64_t value, uint64_t &out) const {
    const int32_t index = int32_t(value), serial = int32_t(value >> 32);
    uint64_t chunks{}, chunk{};
    int32_t count{};
    std::array<uint8_t, 20> item{};
    if (index < 0 || serial <= 0 || !get(main + 0x6ce97e8 + 0x24, count) ||
        count < 1 || count > 1000000 || index >= count ||
        !get(main + 0x6ce97e8 + 0x10, chunks) || !Pointer(chunks) ||
        !get(chunks + 8 * uint64_t(uint32_t(index) >> 16), chunk) ||
        !Pointer(chunk) || !get(chunk + 20 * uint64_t(index & 65535), item))
      return false;
    int32_t actual{};
    uint32_t flags{};
    std::memcpy(&out, item.data(), 8);
    std::memcpy(&flags, item.data() + 8, 4);
    std::memcpy(&actual, item.data() + 16, 4);
    return Pointer(out) && actual == serial && !(flags & 0x30200000);
  }
};
} // namespace
namespace {
struct SlateGeometry {
  uint64_t object{}, offset{}, slate{}, controller{}, vtable{};
  bool geometry_read{};
  std::array<uint8_t, 0x38> geometry{};
  std::array<uint8_t, 2> visibility{};
  bool operator==(const SlateGeometry &) const = default;
};
bool ReadGeometry(const Reader &r, uint64_t object, bool image,
                  SlateGeometry &out, bool geometry = true) {
  SlateGeometry v;
  v.object = object;
  v.offset = image ? 0x288 : 0x108;
  v.vtable = image ? 0x5b45718 : 0x5d011e8;
  v.geometry_read = geometry;
  uint64_t vt{}, self{};
  int32_t refs{};
  if (!r.get(object + v.offset, v.slate) || !Pointer(v.slate) ||
      !r.get(object + v.offset + 8, v.controller) || !Pointer(v.controller) ||
      !r.get(v.controller + 8, refs) || refs < 1 || refs > 1000000 ||
      !r.get(v.slate, vt) || !MatchesVtable(vt, r.main + v.vtable, r.delta) ||
      !r.get(v.slate + 8, self) || self != v.slate ||
      (geometry && !r.get(v.slate + 0xd0, v.geometry)) ||
      !r.get(v.slate + 0x184, v.visibility) || v.visibility[1] != 1 ||
      (v.visibility[0] != 1 && v.visibility[0] != 0x11 &&
       v.visibility[0] != 0x19 && v.visibility[0] != 2 && v.visibility[0] != 4))
    return false;
  out = v;
  return true;
}
bool Area(const SlateGeometry &v, bool zero) {
  std::array<float, 2> size{};
  std::memcpy(size.data(), v.geometry.data(), sizeof(size));
  if (!std::isfinite(size[0]) || !std::isfinite(size[1]))
    return false;
  return zero ? size[0] == 0 && size[1] == 0
              : size[0] > 0 && size[1] > 0 && size[0] <= 65536 &&
                    size[1] <= 65536;
}
bool StableGeometry(const Reader &r, const SlateGeometry &v) {
  SlateGeometry again;
  return ReadGeometry(r, v.object, v.offset == 0x288, again, v.geometry_read) &&
         again == v;
}
bool UnboundFlipbook(const Reader &r, uint64_t book) {
  // KSPaperFlipbook constructor39b4d9c; native GetSpriteAtTime2fe7190:
  // keyframes+30, count+38, stride10, sprite pointer+0. Check every frame so
  // a future portrait load is detected even while top opacity0 skips paint.
  std::array<uint8_t, 16> header{}, checked{};
  uint64_t data{};
  int32_t count{}, capacity{};
  if (!r.object(book, 0x635b400) || !r.get(book + 0x30, header))
    return false;
  std::memcpy(&data, header.data(), 8);
  std::memcpy(&count, header.data() + 8, 4);
  std::memcpy(&capacity, header.data() + 12, 4);
  if (count < 0 || count > 64 || capacity < count || capacity > 128 ||
      (capacity && !Pointer(data)))
    return false;
  std::vector<std::array<uint8_t, 16>> frames;
  for (int i = 0; i < count; ++i) {
    std::array<uint8_t, 16> frame{};
    uint64_t sprite{}, texture{};
    if (!r.get(data + 16 * uint64_t(i), frame))
      return false;
    std::memcpy(&sprite, frame.data(), 8);
    if (!r.object(sprite, 0x635b7d8) || !r.get(sprite + 0x68, texture) ||
        texture)
      return false;
    frames.push_back(frame);
  }
  for (int i = 0; i < count; ++i) {
    std::array<uint8_t, 16> frame{};
    uint64_t sprite{}, texture{};
    std::memcpy(&sprite, frames[i].data(), 8);
    if (!r.get(data + 16 * uint64_t(i), frame) || frame != frames[i] ||
        !r.object(sprite, 0x635b7d8) || !r.get(sprite + 0x68, texture) ||
        texture)
      return false;
  }
  return r.get(book + 0x30, checked) && checked == header &&
         r.object(book, 0x635b400);
}
bool AbsentFace(const Reader &r, uint64_t widget) {
  uint64_t face{};
  SlateGeometry fg;
  if (!r.get(widget + 0x308, face) || !r.object(face, 0x625f360) ||
      !ReadGeometry(r, face, false, fg, false))
    return false;
  if (fg.visibility[0] == 2 || fg.visibility[0] == 4)
    return StableGeometry(r, fg);
  if (!ReadGeometry(r, face, false, fg))
    return false;

  uint64_t glyph{}, loader{}, image{}, material{}, spriteweak{}, sprite{},
      texture{}, custom{}, current{};
  SlateGeometry wg, gg, lg, ig;
  std::array<uint8_t, 16> parameters{};
  // Native GetCachedGeometry1f73620→5d0428 returns Slate+d0; native
  // GetLocalSize1fb2434 reads its first float2. SWidget Paint5d710c updates
  // that cache before dispatching OnPaint5d7294. SImage OnPaint5d5a94 uses
  // its allotted local size; MakeBox50ec70..94 rejects zero width/height.
  // Positive glyph controls distinguish the field's actual drawn marker
  // from its instantiated, visible-flag but unarranged default face tree.
  if (!Area(fg, true) || !ReadGeometry(r, widget, false, wg) ||
      !Area(wg, false) || !r.get(widget + 0x2f8, glyph) ||
      !r.object(glyph, 0x5cd32d0) || !ReadGeometry(r, glyph, true, gg) ||
      !Area(gg, false) || !r.get(face + 0x2f8, loader) ||
      !r.object(loader, 0x625e9e0) || !ReadGeometry(r, loader, false, lg) ||
      !Area(lg, true) || !r.get(loader + 0x2f8, image) ||
      !r.object(image, 0x630af90) || !ReadGeometry(r, image, true, ig) ||
      !Area(ig, true) || !r.get(image + 0x2fc, spriteweak) ||
      !r.weak(spriteweak, sprite) || !r.object(sprite, 0x635b7d8) ||
      !r.get(sprite + 0x68, texture) || texture ||
      !r.get(image + 0x200, material) || !r.object(material, 0x5fe9970) ||
      !r.get(material + 0x198, parameters) ||
      parameters != std::array<uint8_t, 16>{} ||
      !r.get(image + 0x2c8, custom) || !UnboundFlipbook(r, custom) ||
      !r.get(image + 0x2d0, current) || !UnboundFlipbook(r, current))
    return false;
  uint64_t checked{};
  std::array<uint8_t, 16> again{};
  return StableGeometry(r, wg) && StableGeometry(r, gg) &&
         StableGeometry(r, fg) && StableGeometry(r, lg) &&
         StableGeometry(r, ig) && r.get(widget + 0x308, checked) &&
         checked == face && r.get(face + 0x2f8, checked) && checked == loader &&
         r.get(loader + 0x2f8, checked) && checked == image &&
         r.get(image + 0x200, checked) && checked == material &&
         r.get(image + 0x2fc, checked) && checked == spriteweak &&
         r.get(image + 0x2c8, checked) && checked == custom &&
         r.get(image + 0x2d0, checked) && checked == current &&
         r.get(sprite + 0x68, checked) && checked == 0 &&
         r.get(material + 0x198, again) && again == parameters;
}
bool WidgetIcon(const Reader &r, uint64_t widget, MinimapIcon &icon,
                bool &visible, bool &complete) {
  if (!r.object(widget, 0x63ba6e8))
    return false;
  uint64_t slate{}, controller{}, self{}, vt{};
  int32_t refs{};
  std::array<uint8_t, 2> vis{};
  float opacity{};
  if (!r.get(widget + 0x108, slate) || !Pointer(slate) ||
      !r.get(widget + 0x110, controller) || !Pointer(controller) ||
      !r.get(controller + 8, refs) || refs <= 0 || refs > 1000000 ||
      !r.get(slate, vt) || !MatchesVtable(vt, r.main + 0x5d011e8, r.delta) ||
      !r.get(slate + 8, self) || self != slate || !r.get(slate + 0x184, vis) ||
      vis[1] != 1 || !r.get(slate + 0x1cc, opacity) ||
      !std::isfinite(opacity) || opacity < 0 || opacity > 1)
    return false;
  visible = vis[0] == 1 || vis[0] == 0x11 || vis[0] == 0x19;
  if (!visible && vis[0] != 2 && vis[0] != 4)
    return false;
  if (!visible || opacity == 0) {
    visible = false;
    return true;
  }
  icon.opacity = opacity;
  // Loaded, nonzero-area, unknown or torn face/material branches remain
  // unsupported. Visible flags alone neither prove nor disprove drawing.
  if (!AbsentFace(r, widget))
    complete = false;
  for (int layer = 0; layer < 2; ++layer) {
    uint64_t image{}, asset{}, image2{}, asset2{};
    std::array<float, 4> color{}, tint{}, parent{};
    uint8_t draw{}, imagevis{};
    if (!r.get(widget + 0x2f8 + 8 * uint64_t(layer), image) ||
        !r.object(image, 0x5cd32d0) || !r.get(image + 0xdc, imagevis) ||
        imagevis > 4 || !r.get(image + 0x189, draw) || draw != 3 ||
        !r.get(image + 0x200, asset) ||
        !r.frame(asset, layer ? icon.background_frame_path : icon.frame_path) ||
        !r.get(image + 0x250, color) || !r.get(image + 0x1a8, tint) ||
        !r.get(widget + 0x180, parent))
      return false;
    for (int c = 0; c < 4; ++c) {
      if (!std::isfinite(color[c]) || !std::isfinite(tint[c]) ||
          !std::isfinite(parent[c]) || color[c] < 0 || tint[c] < 0 ||
          parent[c] < 0 || color[c] > 1 || tint[c] > 1 || parent[c] > 1)
        return false;
      (layer ? icon.background_color : icon.color)[c] =
          color[c] * tint[c] * parent[c];
    }
    if (imagevis == 1 || imagevis == 2) {
      if (layer)
        icon.background_color[3] = 0;
      else
        visible = false;
    }
    if (!r.get(widget + 0x2f8 + 8 * uint64_t(layer), image2) ||
        image2 != image || !r.get(image + 0x200, asset2) || asset2 != asset)
      return false;
  }
  std::array<uint8_t, 2> vis2{};
  uint64_t slate2{}, controller2{};
  return r.get(widget + 0x108, slate2) && slate2 == slate &&
         r.get(widget + 0x110, controller2) && controller2 == controller &&
         r.get(slate + 0x184, vis2) && vis2 == vis;
}
bool PositionValid(const MinimapIcon &i) {
  return std::isfinite(i.world_x) && std::isfinite(i.world_y) &&
         std::abs(i.world_x) <= 1e9 && std::abs(i.world_y) <= 1e9;
}
// Section FNames carry an instance number (MinimapBoundsVolume_7 is stored
// as number 8); match the exploration reader's "_<number-1>" spelling.
bool SectionName(const Reader &r, uint64_t fname, std::string &out) {
  const uint32_t number = uint32_t(fname >> 32);
  if (number > 1000000000 || !r.name(uint32_t(fname), out)) return false;
  if (number) out += "_" + std::to_string(number - 1);
  return true;
}
// Exact native3a23c00 reveal policy. The current-section snapshot already
// proves the saved visited-cell key; do not substitute a player-distance rule.
std::optional<bool> DirectRevealed(const Reader &r, uint64_t manager,
                                   uint64_t resource, const MinimapIcon &icon,
                                   const ExplorationSnapshot *map) {
  uint64_t volume{}, volume2{}, cls{}, expected{}, fname{};
  uint32_t flags{}, map_id{};
  uint8_t filter{}, filter2{};
  if (!r.get(resource + 0x100, volume) || !Pointer(volume) ||
      !r.get(volume + 0x10, cls) ||
      !r.get(r.main + 0x7cbbcd0, expected) || !Pointer(expected) ||
      cls != expected || !r.get(volume + 8, flags) || flags & 0x40000030 ||
      !r.get(volume + 0x2c9, filter) || filter > 1)
    return std::nullopt;
  bool allowed = true;
  if (filter) {
    std::string section;
    std::array<double, 3> lo{}, hi{};
    int32_t stamp{}, texture_size{};
    float multiplier{};
    if (!map || !map->valid || !map->filter_on || !map->block_width ||
        map->block_width > 100000 || map->revealed_cells.size() > 65536 ||
        !r.get(manager + 0x348, map_id) || map_id != map->map_id ||
        !r.get(resource, fname) || !SectionName(r, fname, section) ||
        section != map->section_key || !r.get(resource + 0xc8, lo) ||
        !r.get(resource + 0xe0, hi) || lo != map->bounds_min ||
        hi != map->bounds_max || !r.get(volume + 0x310, stamp) ||
        stamp < 1 || stamp > 100000 ||
        !r.get(volume + 0x314, multiplier) || !std::isfinite(multiplier) ||
        multiplier <= 0 || multiplier > 100 ||
        map->reveal_stamp_world_size != double(stamp) * multiplier ||
        !r.get(volume + 0x2f8, texture_size) || texture_size < 1 ||
        uint32_t(texture_size) != map->reveal_texture_world_size)
      return std::nullopt;
    const auto cell = WorldCell(*map, icon.world_x, icon.world_y);
    allowed = std::find(map->revealed_cells.begin(), map->revealed_cells.end(),
                        cell) != map->revealed_cells.end();
    // MiniMapWidget.GetFilterMagnification export38 returns float0.3 in
    // supported1.0.5 cooked Blueprint (uexp SHA256 a01f3a2e46695ca9...).
    // Native3a23cc8..d4 rounds both products to f32 before comparison.
    constexpr float FilterMagnification = 0.3f;
    const double radius = float(float(float(stamp) * multiplier) *
                                 FilterMagnification);
    for (const auto &visited : map->revealed_cells) {
      if (allowed) break;
      const float width = float(map->block_width);
      const double x = lo[0] + double(float(visited.x) * width) +
                       double(width * 0.5f);
      const double y = lo[1] + double(float(visited.y) * width) +
                       double(width * 0.5f);
      const double dx = x - icon.world_x, dy = y - icon.world_y;
      allowed = std::sqrt(dx * dx + dy * dy) <= radius;
    }
  }
  if (!r.get(resource + 0x100, volume2) || volume2 != volume ||
      !r.get(volume + 0x2c9, filter2) || filter2 != filter)
    return std::nullopt;
  return allowed;
}
// Native FULL current-section policy: Low3a2ae24 bypasses the small-map
// layer/viewport split when7b0=1; kind6 uses AlmostTop3a2c6d0 instead. Both
// reach this typed class's direct painter3a31a14 and the same reveal policy.
// The bottom full map deliberately keeps full-map-eligible offscreen POIs.
bool DirectIcons(const Reader &r, uint64_t manager, uint64_t resource,
                 const ExplorationSnapshot *map, std::vector<MinimapIcon> &out) {
  std::array<uint8_t, 16> header{}, again{};
  uint64_t data{};
  int32_t count{}, capacity{};
  if (!r.get(resource + 0x28, header)) return false;
  std::memcpy(&data, header.data(), 8);
  std::memcpy(&count, header.data() + 8, 4);
  std::memcpy(&capacity, header.data() + 12, 4);
  if (count < 0 || count > 128 || capacity < count || capacity > 256 ||
      (count && !Pointer(data))) return false;
  std::vector<MinimapIcon> icons;
  for (int i = 0; i < count; ++i) {
    uint64_t marker{}, vt{};
    std::array<uint8_t, 0x210> body{}, body2{};
    if (!r.get(data + uint64_t(i) * 8, marker) || !Pointer(marker) ||
        !r.get(marker, body)) return false;
    std::memcpy(&vt, body.data(), 8);
    if (!MatchesVtable(vt, r.main + 0x64ff4c8, r.delta)) return false;
    uint64_t main_sprite{};
    std::memcpy(&main_sprite, body.data() + 0xf0, 8);
    if (!main_sprite) {
      // Native3a31a2c returns before both foreground/background painting.
      // No actor lookup or disclosure is needed for an unbound resource.
      uint64_t marker2{};
      if (!r.get(data + uint64_t(i) * 8, marker2) || marker2 != marker ||
          !r.get(marker, body2) || body2 != body) return false;
      continue;
    }
    uint32_t kind{};
    std::memcpy(&kind, body.data() + 0x5c, 4);
    if (kind > 255) return false;
    uint64_t database{}, rows{}, database2{};
    std::array<uint8_t, 16> table{}, table2{};
    std::array<uint8_t, 0x30> row{}, row2{};
    uint8_t mode{}, mode2{};
    int32_t rows_count{}, rows_capacity{};
    if (!r.get(r.main + 0x7cc7448, database) ||
        !r.object(database, 0x62a4e80) ||
        !r.get(database + 0x5e58, table) ||
        !r.get(manager + 0x7b0, mode) || mode > 1) return false;
    std::memcpy(&rows, table.data(), 8);
    std::memcpy(&rows_count, table.data() + 8, 4);
    std::memcpy(&rows_capacity, table.data() + 12, 4);
    if (!Pointer(rows) || rows_count <= int32_t(kind) || rows_count > 256 ||
        rows_capacity < rows_count || rows_capacity > 512 ||
        !r.get(rows + uint64_t(kind) * 0x30, row)) return false;
    uint32_t row_id{};
    std::memcpy(&row_id, row.data() + 8, 4);
    if (row_id != kind || row[0xc] > 1 || row[0xd] > 1 || row[0xe] > 1 ||
        body[0x58] > 1 || body[0x200] > 1 || body[0x1f0] > 1)
      return false;
    // Native painter returns before resolving an actor for these hidden
    // instances. In particular Town kind79 must never reveal its position.
    if (!row[0xc] && !body[0x58] && !body[0x200]) {
      uint64_t sprite{}, weak{}, actor{}, component{}, actor2{}, component2{};
      uint32_t flags{};
      std::array<double, 2> world{}, world2{};
      std::array<double, 3> lo{}, hi{};
      std::array<float, 4> color{};
      MinimapIcon icon;
      std::memcpy(&sprite, body.data() + 0xf0, 8);
      std::memcpy(&weak, body.data() + 0x204, 8);
      std::memcpy(color.data(), body.data() + 0x20, sizeof color);
      if (body[0x79] != 3 || !r.frame(sprite, icon.frame_path) ||
          !r.weak(weak, actor) || !r.object(actor, 0x63cd0d0) ||
          !r.get(actor + 0x1a0, component) || !Pointer(component) ||
          !r.get(component + 8, flags) || (flags & 0x40000030) ||
          !r.get(component + 0x1f0, world) ||
          !r.get(resource + 0xc8, lo) || !r.get(resource + 0xe0, hi)) return false;
      icon.world_x = world[0]; icon.world_y = world[1];
      icon.opacity = 1;
      if (!PositionValid(icon) || !(lo[0] < world[0] && world[0] < hi[0] &&
                                    lo[1] < world[1] && world[1] < hi[1]))
        return false; // Never borrow another section's position.
      for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(color[c]) || color[c] < 0 || color[c] > 1)
          return false;
        // Direct3a31b60 supplies recordColor to MakeBox. SetBox50e8f0
        // copies that tint verbatim; unlike SImage it does not read brushTint.
        icon.color[c] = color[c];
      }
      if (body[0x1f0]) {
        uint64_t background{};
        std::memcpy(&background, body.data() + 0x1b0, 8);
        if (body[0x139] != 3 ||
            !r.frame(background, icon.background_frame_path)) return false;
        icon.background_color = color;
      }
      if (!r.weak(weak, actor2) || actor2 != actor ||
          !r.get(actor + 0x1a0, component2) || component2 != component ||
          !r.get(component + 0x1f0, world2) || world2 != world)
        return false;
      const auto revealed = row[0xd] ? DirectRevealed(r, manager, resource, icon, map)
                                    : std::optional<bool>(true);
      if (!revealed) return false;
      icon.reveal_checked = row[0xd] != 0;
      if (*revealed) icons.push_back(std::move(icon));
    }
    uint64_t marker2{};
    if (!r.get(data + uint64_t(i) * 8, marker2) || marker2 != marker ||
        !r.get(marker, body2) || body2 != body ||
        !r.get(r.main + 0x7cc7448, database2) || database2 != database ||
        !r.get(database + 0x5e58, table2) || table2 != table ||
        !r.get(rows + uint64_t(kind) * 0x30, row2) || row2 != row ||
        !r.get(manager + 0x7b0, mode2) || mode2 != mode)
      return false;
  }
  if (!r.get(resource + 0x28, again) || again != header) return false;
  out.insert(out.end(), icons.begin(), icons.end());
  return true;
}
} // namespace
MinimapIcons ReadMinimapIcons(const ReadMemory &read, uint64_t main,
                              int64_t delta, const ExplorationSnapshot *exploration,
                              bool allow_owned_hidden_root) {
  MinimapIcons out;
  if (!FindMinimapHud(read, main, delta, allow_owned_hidden_root))
    return out;
  Reader r{read, main, delta};
  uint64_t manager{}, resource{}, data{}, manager2{}, resource2{};
  uint32_t map{}, map2{};
  int32_t count{}, capacity{}, village_count{}, village_free{};
  std::array<uint8_t, 16> array{}, checked{};
  if (!r.get(main + 0x7cc7588, manager) || !r.object(manager, 0x63be0f0) ||
      !r.get(manager + 0x348, map) || !map ||
      !r.get(manager + 0x448, resource) || !Pointer(resource) ||
      !r.get(resource + 0x48, array))
    return out;
  std::memcpy(&data, array.data(), 8);
  std::memcpy(&count, array.data() + 8, 4);
  std::memcpy(&capacity, array.data() + 12, 4);
  if (count < 0 || count > 128 || capacity < count || capacity > 256 ||
      (count && !Pointer(data)) || !r.get(manager + 0x878, village_count) ||
      !r.get(manager + 0x8a4, village_free) || village_count < 0 ||
      village_count > 256 || village_free < 0 || village_free > village_count)
    return out;
  uint64_t section{};
  const bool section_read = r.get(resource, section);
  out.complete = village_count == village_free;
  // PaintPlayerIcon3a2a430 uses manager+450; native draw3a291e8 multiplies
  // its RGBA at+20 by the supplied white tint before painting PaperSprite01.
  if (!r.get(manager + 0x470, out.player_color))
    return {};
  for (float c : out.player_color)
    if (!std::isfinite(c) || c < 0 || c > 1)
      return {};
  // These separate native NPC/other marker arrays are intentionally
  // unsupported. Their presence preserves the top map while permitting proved
  // POI glyphs.
  for (uint64_t offset : {0x58ull, 0x68ull}) {
    std::array<uint8_t, 16> h{};
    int32_t n{}, cap{};
    if (!r.get(resource + offset, h))
      return {};
    std::memcpy(&n, h.data() + 8, 4);
    std::memcpy(&cap, h.data() + 12, 4);
    if (n < 0 || n > 128 || cap < n || cap > 256)
      return {};
    if (n)
      out.complete = false;
  }
  struct High {
    uint64_t marker{};
    double x{}, y{};
  };
  std::vector<High> highs;
  for (int i = 0; i < count; ++i) {
    uint64_t marker{}, widget{}, weak{}, high{}, high2{}, vt{};
    std::array<uint8_t, 0xc8> body{}, body2{};
    if (!r.get(data + 8 * uint64_t(i), marker) || !Pointer(marker) ||
        !r.get(marker, body))
      return {};
    std::memcpy(&vt, body.data(), 8);
    if (!MatchesVtable(vt, main + 0x64ff3c8, delta))
      return {};
    std::memcpy(&weak, body.data() + 0xa8, 8);
    std::memcpy(&high, body.data() + 0x98, 8);
    std::memcpy(&high2, body.data() + 0xa0, 8);
    MinimapIcon icon;
    std::memcpy(&icon.world_x, body.data() + 0x48, 8);
    std::memcpy(&icon.world_y, body.data() + 0x50, 8);
    if (!PositionValid(icon))
      return {};
    if (high) {
      if (high != manager + 0x578)
        out.complete = false;
      else
        highs.push_back({high, icon.world_x, icon.world_y});
    }
    if (high2)
      out.complete = false;
    bool visible{};
    if (!r.weak(weak, widget) ||
        !WidgetIcon(r, widget, icon, visible, out.complete))
      return {};
    if (visible)
      out.icons.push_back(std::move(icon));
    uint64_t widget2{};
    if (!r.get(marker, body2) || body2 != body || !r.weak(weak, widget2) ||
        widget2 != widget)
      return {};
  }
  // Native PaintIconHigh uses the linked low marker's world position for this
  // embedded quest marker, even when that low widget itself is hidden.
  if (highs.size() > 1)
    out.complete = false;
  for (const auto &h : highs) {
    uint64_t vt{}, weak{}, widget{};
    MinimapIcon icon;
    icon.world_x = h.x;
    icon.world_y = h.y;
    icon.quest = true;
    bool visible{};
    if (!r.get(h.marker, vt) || !MatchesVtable(vt, main + 0x64ff648, delta) ||
        !r.get(h.marker + 0xa4, weak) || !r.weak(weak, widget) ||
        !WidgetIcon(r, widget, icon, visible, out.complete))
      return {};
    if (visible)
      out.icons.push_back(std::move(icon));
  }
  // Current-resource quest markers: native virtual+70 (3a4d050) resolves its
  // owned target actor+9c and reads that actor's RootComponent world position.
  std::array<uint8_t, 16> quests{}, quests2{};
  uint64_t qdata{};
  int32_t qcount{}, qcap{};
  if (!r.get(resource + 0x38, quests))
    return {};
  std::memcpy(&qdata, quests.data(), 8);
  std::memcpy(&qcount, quests.data() + 8, 4);
  std::memcpy(&qcap, quests.data() + 12, 4);
  if (qcount < 0 || qcount > 64 || qcap < qcount || qcap > 128 ||
      (qcount && !Pointer(qdata)))
    return {};
  for (int i = 0; i < qcount; ++i) {
    uint64_t marker{}, vt{}, weak{}, widget{}, actorweak{}, actor{},
        component{};
    std::array<uint8_t, 0xc8> body{}, body2{};
    if (!r.get(qdata + 8 * uint64_t(i), marker) || !Pointer(marker) ||
        !r.get(marker, body))
      return {};
    std::memcpy(&vt, body.data(), 8);
    std::memcpy(&weak, body.data() + 0xa4, 8);
    if (!MatchesVtable(vt, main + 0x64ff648, delta) || !r.weak(weak, widget))
      return {};
    MinimapIcon icon;
    icon.quest = true;
    bool visible{};
    if (!WidgetIcon(r, widget, icon, visible, out.complete))
      return {};
    if (visible) {
      std::memcpy(&actorweak, body.data() + 0x9c, 8);
      std::array<double, 2> position{}, position2{};
      uint32_t component_flags{};
      uint64_t actor2{}, component2{};
      // This is the observed native target class only; unknown target kinds
      // cannot establish parity and must never expose hidden actor positions.
      if (!r.weak(actorweak, actor) || !r.object(actor, 0x63cd0d0) ||
          !r.get(actor + 0x1a0, component) || !Pointer(component) ||
          !r.get(component + 8, component_flags) ||
          (component_flags & 0x40000030) || !r.get(component + 0x1f0, position))
        return {};
      icon.world_x = position[0];
      icon.world_y = position[1];
      if (!PositionValid(icon) || !r.weak(actorweak, actor2) ||
          actor2 != actor || !r.get(actor + 0x1a0, component2) ||
          component2 != component || !r.get(component + 0x1f0, position2) ||
          position2 != position)
        return {};
      out.icons.push_back(std::move(icon));
    }
    if (!r.get(marker, body2) || body2 != body)
      return {};
  }
  if (!DirectIcons(r, manager, resource, exploration, out.icons))
    out.complete = false;
  if (!r.get(resource + 0x38, quests2) || quests2 != quests ||
      !r.get(resource + 0x48, checked) || checked != array ||
      !r.get(main + 0x7cc7588, manager2) || manager2 != manager ||
      !r.get(manager + 0x448, resource2) || resource2 != resource ||
      !r.get(manager + 0x348, map2) || map2 != map ||
      !FindMinimapHud(read, main, delta, allow_owned_hidden_root))
    return {};
  uint64_t section2{};
  const bool section_checked = r.get(resource, section2);
  if (section_read && section_checked && section2 != section) return {};
  if (!section_read || !section_checked || !section)
    out.complete = false;
  else
    out.native_section = section;
  out.native_resource = resource;
  out.map_id = map;
  out.valid = true;
  return out;
}
std::optional<HudRow> FindMinimapHud(const ReadMemory &read, uint64_t main,
                                     int64_t delta,
                                     bool allow_owned_hidden_root) {
  Reader r{read, main, delta};
  uint64_t app{}, manager{};
  uint8_t state{};
  if (main >= Limit - 0x7cc9a20 || !r.get(main + 0x7cc8cc0, app) ||
      !r.object(app, 0x6207610) || !r.get(app + 0x2f0, state) ||
      (state != 23 && state != 24) || !r.get(main + 0x7cc9a18, manager) ||
      !r.object(manager, 0x63b6f18))
    return std::nullopt;
  // Native lookup3c1d7f0. A bounded bucket chain for key10, never a registry
  // or heap scan. Validate allocation flags as well as native hash links.
  std::array<uint8_t, 0x50> header{}, checked{};
  auto at = [&]<class T>(size_t n) {
    T v{};
    std::memcpy(&v, header.data() + n, sizeof v);
    return v;
  };
  if (!r.get(manager + 0x2f0, header))
    return std::nullopt;
  const auto entries = at.operator()<uint64_t>(0);
  const auto count = at.operator()<int32_t>(8);
  const auto capacity = at.operator()<int32_t>(12);
  const auto free = at.operator()<int32_t>(0x34);
  auto buckets = at.operator()<uint64_t>(0x40);
  const auto hashes = at.operator()<int32_t>(0x48);
  const auto bitcount = at.operator()<int32_t>(0x28);
  auto bits = at.operator()<uint64_t>(0x20);
  if (!Pointer(entries) || count <= 0 || count > 256 || capacity < count ||
      capacity > 256 || free < 0 || free >= count || hashes <= 0 ||
      hashes > 256 || (hashes & (hashes - 1)) || bitcount < count ||
      bitcount > 256 || (buckets ? !Pointer(buckets) : hashes != 1) ||
      (bits && !Pointer(bits)))
    return std::nullopt;
  if (!buckets)
    buckets = manager + 0x328;
  if (!bits)
    bits = manager + 0x300;
  int32_t index{};
  if (!r.get(buckets + uint64_t(10 & (hashes - 1)) * 4, index))
    return std::nullopt;
  std::array<bool, 256> visited{};
  for (int steps = 0; steps < 256 && index != -1; ++steps) {
    if (index < 0 || index >= count || visited[index])
      return std::nullopt;
    visited[index] = true;
    uint32_t allocated{};
    std::array<uint8_t, 0x18> node{}, again{};
    if (!r.get(bits + uint64_t(index / 32) * 4, allocated) ||
        !(allocated & (1u << (index % 32))) ||
        !r.get(entries + uint64_t(index) * 0x18, node))
      return std::nullopt;
    if (node[0] == 10) {
      uint64_t ui{};
      std::memcpy(&ui, node.data() + 8, 8);
      HudRow out;
      uint64_t app2{}, manager2{};
      uint8_t state2{};
      if (!r.widget(ui, out, !allow_owned_hidden_root) ||
          !r.get(entries + uint64_t(index) * 0x18, again) || node != again ||
          !r.get(manager + 0x2f0, checked) || header != checked ||
          !r.get(main + 0x7cc8cc0, app2) || app2 != app ||
          !r.get(app + 0x2f0, state2) || state2 != state ||
          !r.get(main + 0x7cc9a18, manager2) || manager2 != manager ||
          !r.object(manager, 0x63b6f18))
        return std::nullopt;
      return out;
    }
    std::memcpy(&index, node.data() + 0x10, 4);
  }
  return std::nullopt;
}
std::optional<HudRow> FindMinimapCanvas(const ReadMemory &read, uint64_t main,
                                        int64_t delta, bool allow_hidden) {
  const auto ui = FindMinimapHud(read, main, delta);
  if (!ui) return std::nullopt;
  Reader r{read, main, delta};
  uint64_t root{}, tree{}, tree_root{}, outer{}, data{};
  std::array<uint8_t, 16> header{}, checked{};
  int32_t count{}, capacity{};
  // Reflected MiniMapUI.m_pMiniMapRoot488 and UUserWidget.WidgetTree278;
  // native GetRootWidget1fd1fd4 reads that tree's RootWidget30.
  if (!r.get(ui->row + 0x488, root) || !r.object(root, 0x5cc23b0) ||
      !r.get(ui->row + 0x278, tree) || !r.object(tree, 0x5d0ee88) ||
      !r.get(tree + 0x30, tree_root) || tree_root != root ||
      !r.get(tree + 0x20, outer) || outer != ui->row ||
      !r.get(root + 0x20, outer) || outer != tree ||
      !r.get(root + 0x178, header)) return std::nullopt;
  std::memcpy(&data, header.data(), 8);
  std::memcpy(&count, header.data() + 8, 4);
  std::memcpy(&capacity, header.data() + 12, 4);
  if (!Pointer(data) || count < 1 || count > 32 || capacity < count ||
      capacity > 128) return std::nullopt;
  std::array<uint64_t, 32> slots{};
  std::optional<HudRow> found;
  uint64_t found_slot{}, found_name{};
  uint8_t found_visibility{};
  std::array<uint8_t, 2> found_slate_visibility{};
  for (int i = 0; i < count; ++i) {
    uint64_t slot{}, parent{}, child{}, fn{}, back{};
    std::string name;
    if (!r.get(data + uint64_t(i) * 8, slot) ||
        !r.object(slot, 0x5cc35a8) || !r.get(slot + 0x28, parent) ||
        parent != root || !r.get(slot + 0x30, child) || !Pointer(child) ||
        !r.get(child + 0x18, fn) || !r.name(fn, name))
      return std::nullopt;
    slots[i] = slot;
    if (name != "CanvasPanel_MinimapSmall") continue;
    if (found || !r.object(child, 0x5cc23b0) ||
        !r.get(child + 0x30, back) || back != slot ||
        !r.get(child + 0x20, outer) || outer != tree)
      return std::nullopt;
    uint64_t slate{}, controller{}, fallback{}, fallback_controller{}, vt{}, self{};
    uint64_t cached{}, weak{};
    int32_t refs{};
    uint8_t visibility{};
    std::array<uint8_t, 2> slate_visibility{};
    // Native UCanvasPanel.Rebuild1eb5190 stores190/198; base native
    // SetVisibility1f135f0 falls back throughf8/100 when108 is empty.
    if (!r.get(child + 0x190, slate) || !Pointer(slate) ||
        !r.get(child + 0x198, controller) || !Pointer(controller) ||
        !r.get(child + 0xf8, fallback) || fallback != slate ||
        !r.get(child + 0x100, fallback_controller) || fallback_controller != controller ||
        !r.get(child + 0x108, cached) || !r.get(child + 0x110, weak) ||
        (cached && (cached != slate || weak != controller)) ||
        !r.get(controller + 8, refs) || refs < 1 || refs > 1000000 ||
        !r.get(slate, vt) || !MatchesVtable(vt, main + 0x5b5af90, delta) ||
        !r.get(slate + 8, self) || self != slate ||
        !r.get(child + 0xdc, visibility) || visibility > 4 ||
        !r.get(slate + 0x184, slate_visibility) || slate_visibility[1] != 1)
      return std::nullopt;
    constexpr uint8_t NativeVisibility[] = {0x19, 2, 4, 1, 0x11};
    if (slate_visibility[0] != NativeVisibility[visibility] ||
        (!allow_hidden && (visibility == 1 || visibility == 2)))
      return std::nullopt;
    found = HudRow{child, slate, controller};
    found_slot = slot;
    found_name = fn;
    found_visibility = visibility;
    found_slate_visibility = slate_visibility;
  }
  if (!found) return std::nullopt;
  // Revalidate the exact owned attachment, not merely a matching class at a
  // cached address. Array replacement with an unchanged header also refuses.
  for (int i = 0; i < count; ++i) {
    uint64_t slot{};
    if (!r.get(data + uint64_t(i) * 8, slot) || slot != slots[i]) return std::nullopt;
  }
  uint64_t root2{}, tree2{}, parent2{}, child2{}, slot2{}, slate2{}, controller2{};
  uint64_t name2{}, fallback2{}, fallback_controller2{};
  uint8_t visibility2{};
  std::array<uint8_t, 2> slate_visibility2{};
  int32_t refs2{};
  if (!r.get(ui->row + 0x488, root2) || root2 != root ||
      !r.get(ui->row + 0x278, tree2) || tree2 != tree ||
      !r.get(tree + 0x30, child2) || child2 != root ||
      !r.get(tree + 0x20, parent2) || parent2 != ui->row ||
      !r.get(root + 0x178, checked) || checked != header ||
      !r.get(found_slot + 0x28, parent2) || parent2 != root ||
      !r.get(found_slot + 0x30, child2) || child2 != found->row ||
      !r.get(found->row + 0x30, slot2) || slot2 != found_slot ||
      !r.get(found->row + 0x190, slate2) || slate2 != found->widget ||
      !r.get(found->row + 0x198, controller2) || controller2 != found->controller ||
      !r.get(found->row + 0xf8, fallback2) || fallback2 != found->widget ||
      !r.get(found->row + 0x100, fallback_controller2) || fallback_controller2 != found->controller ||
      !r.get(found->row + 0x18, name2) || name2 != found_name ||
      !r.get(found->row + 0x20, parent2) || parent2 != tree ||
      !r.get(found->row + 0xdc, visibility2) || visibility2 != found_visibility ||
      !r.get(found->widget + 0x184, slate_visibility2) ||
      slate_visibility2 != found_slate_visibility ||
      !r.get(found->controller + 8, refs2) || refs2 < 1 || refs2 > 1000000 ||
      !r.object(found->row, 0x5cc23b0) ||
      FindMinimapHud(read, main, delta) != ui)
    return std::nullopt;
  return found;
}
// Native GPU tests disproved root/canvas opacity hiding. Three retainer
// opacities hide only cached contents, leaving the independent exit and frame.
// Keep the compatibility interface inert until a complete native contract is
// proved; no write path or claimed ownership remains reachable.
void MinimapHud::Update(const ReadMemory &, const WriteF32 &, uint64_t,
                        bool, bool, int64_t) {}
void MinimapHud::Restore(const ReadMemory &, const WriteF32 &, uint64_t,
                         int64_t) {}
} // namespace ot0
