// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_minimap.h"
#include "ot0_exploration.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
namespace {
int failures{};
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                          \
      ++failures;                                                              \
    }                                                                          \
  } while (false)
constexpr uint64_t Main = 0x80000000, App = 0x11000000, Manager = 0x12000000,
                   Entries = 0x13000000, Ui = 0x14000000, Slate = 0x15000000,
                   Controller = 0x16000000;
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  int writes{};
  void block(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      bytes[p + i] = 0;
  }
  template <class T> void put(uint64_t p, T value) {
    const auto *b = reinterpret_cast<const uint8_t *>(&value);
    for (size_t i = 0; i < sizeof value; ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *out, size_t n) {
    auto *b = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it == bytes.end())
        return false;
      b[i] = it->second;
    }
    return true;
  }
  float opacity() {
    float out{};
    read(Slate + 0x1cc, &out, 4);
    return out;
  }
  ot0::ReadMemory reader() {
    return [this](uint64_t p, void *v, size_t n) { return read(p, v, n); };
  }
  ot0::WriteF32 writer() {
    return [this](uint64_t p, float expected, float value) {
      float actual{};
      if (!read(p, &actual, 4) || actual != expected)
        return false;
      put(p, value);
      ++writes;
      return true;
    };
  }
  explicit Memory(int64_t delta = 0) {
    block(App, 0x300);
    block(Manager, 0x340);
    block(Entries, 0x18);
    block(Ui, 0x120);
    block(Slate, 0x1d0);
    block(Controller, 0x10);
    put(Main + 0x7cc8cc0, App);
    put(App, Main + 0x6207610 + uint64_t(delta));
    put(App + 0x2f0, uint8_t{23});
    put(Main + 0x7cc9a18, Manager);
    put(Manager, Main + 0x63b6f18 + uint64_t(delta));
    put(Manager + 0x2f0, Entries);
    put(Manager + 0x2f8, int32_t{1});
    put(Manager + 0x2fc, int32_t{1});
    put(Manager + 0x300, uint32_t{1});
    put(Manager + 0x318, int32_t{1});
    put(Manager + 0x31c, int32_t{128});
    put(Manager + 0x320, int32_t{-1});
    put(Manager + 0x338, int32_t{1});
    put(Entries, uint8_t{10});
    put(Entries + 8, Ui);
    put(Entries + 0x10, int32_t{-1});
    put(Ui, Main + 0x63c0c48 + uint64_t(delta));
    put(Ui + 0x108, Slate);
    put(Ui + 0x110, Controller);
    put(Controller + 8, int32_t{2});
    put(Slate, Main + 0x5d011e8 + uint64_t(delta));
    put(Slate + 8, Slate);
    put(Slate + 0x184, uint8_t{0x11});
    put(Slate + 0x185, uint8_t{1});
    put(Slate + 0x1cc, 1.f);
  }
};
void Synthetic(int64_t delta) {
  Memory m(delta);
  ot0::MinimapHud hud;
  CHECK(ot0::FindMinimapHud(m.reader(), Main, delta).has_value());
  m.put(Slate + 0x184, uint8_t{2});
  CHECK(!ot0::FindMinimapHud(m.reader(), Main, delta));
  CHECK(ot0::FindMinimapHud(m.reader(), Main, delta, true).has_value());
  m.put(Slate + 0x184, uint8_t{0x11});
  // Complete marker parity does not prove full native hide support. Neither
  // preference state can resurrect the disproved root-opacity write path.
  for (bool hide : {false, true})
    for (bool parity : {false, true}) {
      hud.Update(m.reader(), m.writer(), Main, hide, parity, delta);
      hud.Restore(m.reader(), m.writer(), Main, delta);
      CHECK(m.opacity() == 1 && !hud.Hidden() && hud.Failures() == 0);
      CHECK(m.writes == 0);
    }
  m.put(Slate + 0x184, uint8_t{4});
  m.put(App + 0x2f0, uint8_t{25});
  hud.Update(m.reader(), m.writer(), Main, true, true, delta);
  CHECK(m.opacity() == 1 && !hud.Hidden() && m.writes == 0);
  m.put(Slate + 0x184, uint8_t{0x11});
  m.put(App + 0x2f0, uint8_t{23});
  m.put(Manager + 0x300, uint32_t{0});
  CHECK(!ot0::FindMinimapHud(m.reader(), Main, delta));
  m.put(Manager + 0x300, uint32_t{1});
  m.put(Ui, Main + 0x63c0c50);
  CHECK(!ot0::FindMinimapHud(m.reader(), Main, delta));
  m.put(Ui, Main + 0x63c0c48 + uint64_t(delta));
  m.put(Entries, uint8_t{9});
  m.put(Entries + 0x10, int32_t{0});
  CHECK(!ot0::FindMinimapHud(m.reader(), Main, delta));
  m.put(Entries, uint8_t{10});
  m.put(Entries + 0x10, int32_t{-1});
  hud.Update(m.reader(), m.writer(), Main, true, true, delta);
  CHECK(!hud.Hidden() && m.writes == 0);
  // Even externally zeroed or recycled roots do not authorize restoration.
  m.put(Slate + 0x1cc, 0.f);
  m.put(Ui + 0x110, Controller + 8);
  const auto before = m.writes;
  hud.Restore(m.reader(), m.writer(), Main, delta);
  CHECK(!hud.Hidden() && m.writes == before && m.opacity() == 0);
}
void Canvas(int64_t delta) {
  Memory m(delta);
  constexpr uint64_t root = 0x17000000, tree = 0x18000000,
      canvas = 0x19000000, slots = 0x1a000000, slot = 0x1b000000,
      slate = 0x1c000000, controller = 0x1d000000, names = 0x1e000000;
  for (auto p : {root, tree, canvas, slot}) m.block(p, 0x1a0);
  m.block(slate, 0x1d0); m.block(controller, 16);
  m.put(Ui + 0x278, tree); m.put(Ui + 0x488, root);
  m.put(root, Main + 0x5cc23b0 + uint64_t(delta));
  m.put(tree, Main + 0x5d0ee88 + uint64_t(delta));
  m.put(tree + 0x20, Ui); m.put(tree + 0x30, root);
  m.put(root + 0x20, tree); m.put(root + 0x178, slots);
  m.put(root + 0x180, int32_t{1}); m.put(root + 0x184, int32_t{1});
  m.put(slots, slot); m.put(slot, Main + 0x5cc35a8 + uint64_t(delta));
  m.put(slot + 0x28, root); m.put(slot + 0x30, canvas);
  m.put(canvas, Main + 0x5cc23b0 + uint64_t(delta));
  m.put(canvas + 0x18, uint64_t{1}); m.put(canvas + 0x20, tree);
  m.put(canvas + 0x30, slot); m.put(canvas + 0xdc, uint8_t{4});
  m.put(canvas + 0x190, slate); m.put(canvas + 0x198, controller);
  m.put(canvas + 0xf8, slate); m.put(canvas + 0x100, controller);
  m.put(slate, Main + 0x5b5af90 + uint64_t(delta));
  m.put(slate + 8, slate); m.put(controller + 8, int32_t{2});
  m.put(slate + 0x184, uint8_t{0x11}); m.put(slate + 0x185, uint8_t{1});
  constexpr char name[] = "CanvasPanel_MinimapSmall";
  m.put(Main + 0x6c91478, names);
  m.put(names + 2, uint16_t((sizeof(name) - 1) << 6));
  for (size_t i = 0; i < sizeof(name) - 1; ++i) m.put(names + 4 + i, name[i]);
  const auto find = [&](bool hidden = false) {
    return ot0::FindMinimapCanvas(m.reader(), Main, delta, hidden);
  };
  const auto found = find();
  CHECK(found && (*found == ot0::HudRow{canvas, slate, controller}));
  m.put(canvas + 0xdc, uint8_t{1}); m.put(slate + 0x184, uint8_t{2});
  CHECK(!find() && find(true) == found);
  m.put(canvas + 0xdc, uint8_t{4});
  CHECK(!find(true)); // UMG and actual Slate visibility must agree.
  m.put(slate + 0x184, uint8_t{0x11});
  m.put(canvas + 0x30, uint64_t{0}); CHECK(!find()); m.put(canvas + 0x30, slot);
  m.put(canvas + 0xf8, slate + 8); CHECK(!find()); m.put(canvas + 0xf8, slate);
  m.put(slate, Main + 0x5b5af98); CHECK(!find());
  m.put(slate, Main + 0x5b5af90 + uint64_t(delta));
  m.put(controller + 8, int32_t{0}); CHECK(!find());
  m.put(controller + 8, int32_t{2});
  int arrays{};
  const auto torn = [&](uint64_t p, void *v, size_t n) {
    if (!m.read(p, v, n)) return false;
    if (p == slots && n == 8 && ++arrays == 2) static_cast<uint8_t *>(v)[0] ^= 8;
    return true;
  };
  CHECK(!ot0::FindMinimapCanvas(torn, Main, delta));
  int visibility_reads{};
  const auto moving_visibility = [&](uint64_t p, void *v, size_t n) {
    if (!m.read(p, v, n)) return false;
    if (p == canvas + 0xdc && n == 1 && ++visibility_reads == 2)
      static_cast<uint8_t *>(v)[0] = 1;
    return true;
  };
  CHECK(!ot0::FindMinimapCanvas(moving_visibility, Main, delta));
  CHECK(find() == found && m.writes == 0);
}
void Native(const char *environment, bool ready, bool smithy = false, bool direct = false, bool full = false, bool canvas_capture = false) {
  const char *path = std::getenv(environment);
  if (!path)
    return;
  std::ifstream in(path, std::ios::binary);
  char magic[8]{};
  uint64_t main{};
  uint32_t count{};
  in.read(magic, 8);
  in.read(reinterpret_cast<char *>(&main), 8);
  in.read(reinterpret_cast<char *>(&count), 4);
  CHECK(std::string(magic, 8) == "OT0READ1" && count <= 10000);
  if (!in || count > 10000)
    return;
  Memory m;
  m.bytes.clear();
  for (uint32_t i = 0; i < count && in; ++i) {
    uint64_t address{};
    uint32_t size{};
    in.read(reinterpret_cast<char *>(&address), 8);
    in.read(reinterpret_cast<char *>(&size), 4);
    CHECK(size <= 65536);
    if (size > 65536)
      return;
    std::vector<char> b(size);
    in.read(b.data(), size);
    for (size_t k = 0; k < size; ++k)
      m.bytes[address + k] = uint8_t(b[k]);
  }
  CHECK(bool(in));
  const auto found = ot0::FindMinimapHud(m.reader(), main);
  CHECK(found.has_value());
  const auto map = full ? ot0::ReadExploration(m.reader(), main) : ot0::ExplorationSnapshot{};
  if (full) CHECK(map.valid);
  const auto icons = ot0::ReadMinimapIcons(m.reader(), main, 0, full ? &map : nullptr);
  std::fprintf(stderr, "Native icons valid=%d complete=%d map=%u count=%zu\n",
               icons.valid, icons.complete, icons.map_id, icons.icons.size());
  CHECK(icons.valid && icons.complete == ready && icons.map_id == 46 &&
        icons.icons.size() == (smithy ? (direct ? 2 : 1) : (full ? 11 : 9)));
  if (found) {
    uint8_t visibility{};
    CHECK(m.read(found->widget + 0x184, &visibility, 1));
    m.put(found->widget + 0x184, uint8_t{2});
    CHECK(!ot0::ReadMinimapIcons(m.reader(), main).valid);
    const auto owned_hidden = ot0::ReadMinimapIcons(m.reader(), main, 0, full ? &map : nullptr, true);
    CHECK(owned_hidden.valid && owned_hidden.complete == ready &&
          owned_hidden.icons == icons.icons);
    m.put(found->widget + 0x184, visibility);
  }
  if (canvas_capture) {
    const auto canvas = ot0::FindMinimapCanvas(m.reader(), main);
    CHECK(canvas.has_value() && icons.native_resource && icons.native_section);
    if (canvas) {
      m.put(canvas->row + 0xdc, uint8_t{1});
      m.put(canvas->widget + 0x184, uint8_t{2});
      CHECK(!ot0::FindMinimapCanvas(m.reader(), main));
      CHECK(ot0::FindMinimapCanvas(m.reader(), main, 0, true) == canvas);
      CHECK(ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(canvas->row + 0xdc, uint8_t{4});
      m.put(canvas->widget + 0x184, uint8_t{0x11});
    }
  }
  if (full) {
    CHECK(icons.native_resource && icons.native_section);
    CHECK(ot0::FindMinimapCanvas(m.reader(), main).has_value());
    int bags{}, known{}, hidden{};
    for (const auto &icon : icons.icons) {
      bags += icon.frame_path.ends_with("MiniMapIcon_11_png.UiTX_MainMenu_MiniMapIcon_11_png");
      known += icon.frame_path.ends_with("MiniMapIcon_23_png.UiTX_MainMenu_MiniMapIcon_23_png");
      hidden += icon.frame_path.ends_with("MiniMapIcon_15_png.UiTX_MainMenu_MiniMapIcon_15_png");
    }
    CHECK(bags == 1 && known == 1 && hidden == 0);
    auto q = [&](uint64_t p) { uint64_t v{}; CHECK(m.read(p, &v, 8)); return v; };
    const auto direct_data = q(icons.native_resource + 0x28);
    const auto hidden_marker = q(direct_data + 16);
    const auto hidden_weak = q(hidden_marker + 0x204);
    m.put(hidden_marker + 0x204, ~uint64_t{});
    const auto no_hidden_actor = ot0::ReadMinimapIcons(m.reader(), main, 0, &map);
    CHECK(no_hidden_actor == icons); // Invalid hidden actor must never be followed.
    m.put(hidden_marker + 0x204, hidden_weak);
    int section_reads{};
    const auto torn_section = [&](uint64_t p, void *v, size_t n) {
      if (!m.read(p, v, n)) return false;
      if (p == icons.native_resource && n == 8 && ++section_reads == 3)
        static_cast<uint8_t *>(v)[0] ^= 8;
      return true;
    };
    CHECK(!ot0::ReadMinimapIcons(torn_section, main, 0, &map).valid);
    CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
    auto stale = map;
    stale.section_key += "_previous";
    const auto wrong_section = ot0::ReadMinimapIcons(m.reader(), main, 0, &stale);
    CHECK(wrong_section.valid && !wrong_section.complete && wrong_section.icons.size() == 9);
    stale = map;
    stale.revealed_cells.clear();
    const auto undiscovered = ot0::ReadMinimapIcons(m.reader(), main, 0, &stale);
    CHECK(undiscovered.valid && undiscovered.complete && undiscovered.icons.size() == 10);
    for (const auto &icon : undiscovered.icons)
      CHECK(!icon.frame_path.ends_with("MiniMapIcon_11_png.UiTX_MainMenu_MiniMapIcon_11_png"));
    CHECK(m.writes == 0);
    return;
  }
  if (smithy) {
    // Native PaintIconHigh keeps the linked exit visible while the low
    // widget is hidden. Its position is the low marker's world coordinate,
    // not the player position or an inferred edge clamp.
    if (!icons.icons.empty()) {
      const auto &exit = icons.icons.front();
      CHECK(exit.quest && exit.frame_path.ends_with(
          "MiniMapIcon_13_png.UiTX_MainMenu_MiniMapIcon_13_png"));
      CHECK(exit.world_x == -550.0 && exit.world_y == -28275.0);
      CHECK(exit.background_color[3] == 0.0f);
      CHECK(exit.color[1] > .70f && exit.color[1] < .72f);
    }
    if (direct && found && icons.icons.size() == 2) {
      const auto &sword = icons.icons.back();
      CHECK(!sword.quest && sword.frame_path.ends_with(
          "MiniMapIcon_10_png.UiTX_MainMenu_MiniMapIcon_10_png"));
      CHECK(sword.world_x == 345.0 && sword.world_y == -28275.0);
      CHECK(sword.background_frame_path.empty() && sword.opacity == 1);
      CHECK((sword.color == std::array<float, 4>{1, 1, 1, 1}));
      auto q = [&](uint64_t p) { uint64_t v{}; CHECK(m.read(p, &v, 8)); return v; };
      const auto manager = q(main + 0x7cc7588);
      const auto resource = q(manager + 0x448);
      const auto marker = q(q(resource + 0x28));
      const auto vt = q(marker);
      m.put(marker, vt + 8);
      const auto unknown = ot0::ReadMinimapIcons(m.reader(), main);
      CHECK(unknown.valid && !unknown.complete && unknown.icons.size() == 1);
      m.put(marker, vt);
      const auto sprite = q(marker + 0xf0);
      m.put(marker + 0xf0, uint64_t{0});
      const auto unbound = ot0::ReadMinimapIcons(m.reader(), main);
      CHECK(unbound.valid && unbound.complete && unbound.icons.size() == 1);
      m.put(marker + 0xf0, sprite);
      const auto database = q(main + 0x7cc7448);
      const auto row = q(database + 0x5e58) + 10 * 0x30;
      m.put(row + 0xc, uint8_t{1});
      const auto hidden = ot0::ReadMinimapIcons(m.reader(), main);
      CHECK(hidden.valid && hidden.complete && hidden.icons.size() == 1);
      m.put(row + 0xc, uint8_t{0});
      m.put(row + 0xd, uint8_t{1});
      const auto unsupported = ot0::ReadMinimapIcons(m.reader(), main);
      CHECK(unsupported.valid && !unsupported.complete && unsupported.icons.size() == 1);
      // Same source reveal predicate on a synthetic fog-dependent direct
      // row: exact native section snapshot, hidden cell and stamp boundary.
      auto revealed_map = ot0::ReadExploration(m.reader(), main);
      CHECK(revealed_map.valid && revealed_map.filter_on);
      const auto eligible = ot0::ReadMinimapIcons(m.reader(), main, 0, &revealed_map);
      CHECK(eligible.valid && eligible.complete && eligible.icons.size() == 2);
      revealed_map.revealed_cells.clear();
      const auto fogged = ot0::ReadMinimapIcons(m.reader(), main, 0, &revealed_map);
      CHECK(fogged.valid && fogged.complete && fogged.icons.size() == 1);
      auto stale_map = revealed_map;
      stale_map.section_key += "_old";
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main, 0, &stale_map).complete);
      // A point in an unvisited neighboring cell is admitted at the native
      // f32 reveal radius, then rejected immediately beyond that radius.
      const auto weak = q(marker + 0x204);
      const auto index = uint32_t(weak);
      const auto chunk = q(q(main + 0x6ce97e8 + 0x10) + 8 * (index >> 16));
      const auto actor = q(chunk + 20 * uint64_t(index & 65535));
      const auto component = q(actor + 0x1a0);
      std::array<double, 2> original_world{};
      CHECK(m.read(component + 0x1f0, original_world.data(), sizeof original_world));
      revealed_map.revealed_cells = {{12, 3}};
      const double cx = revealed_map.bounds_min[0] + 12.5 * revealed_map.block_width;
      const double cy = revealed_map.bounds_min[1] + 3.5 * revealed_map.block_width;
      const double radius = float(float(revealed_map.reveal_stamp_world_size) * .3f);
      m.put(component + 0x1f0, std::array<double, 2>{cx - radius, cy});
      CHECK(ot0::ReadMinimapIcons(m.reader(), main, 0, &revealed_map).icons.size() == 2);
      m.put(component + 0x1f0, std::array<double, 2>{cx - radius - .001, cy});
      const auto beyond_stamp = ot0::ReadMinimapIcons(m.reader(), main, 0, &revealed_map);
      CHECK(beyond_stamp.valid && beyond_stamp.complete && beyond_stamp.icons.size() == 1);
      m.put(component + 0x1f0, original_world);
      m.put(row + 0xd, uint8_t{0});
      // The direct native painter ignores brush tint (SImage does not).
      std::array<float, 4> brush_tint{};
      CHECK(m.read(marker + 0x98, brush_tint.data(), sizeof brush_tint));
      m.put(marker + 0x98, std::array<float, 4>{0, 0, 0, 0});
      const auto direct_color = ot0::ReadMinimapIcons(m.reader(), main);
      CHECK(direct_color.complete && (direct_color.icons.back().color == std::array<float, 4>{1, 1, 1, 1}));
      m.put(marker + 0x98, brush_tint);
      m.put(marker + 0x1f0, uint8_t{1});
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(marker + 0x1f0, uint8_t{0});
      int arrays{};
      const auto torn = [&](uint64_t p, void *v, size_t n) {
        if (!m.read(p, v, n)) return false;
        if (p == resource + 0x28 && n == 16 && ++arrays == 2)
          static_cast<uint8_t *>(v)[0] ^= 8;
        return true;
      };
      const auto changing = ot0::ReadMinimapIcons(torn, main);
      CHECK(changing.valid && !changing.complete && changing.icons.size() == 1);
      int elements{};
      const auto replaced = [&](uint64_t p, void *v, size_t n) {
        if (!m.read(p, v, n)) return false;
        if (p == q(resource + 0x28) && n == 8 && ++elements == 2)
          static_cast<uint8_t *>(v)[0] ^= 8;
        return true;
      };
      const auto replacement = ot0::ReadMinimapIcons(replaced, main);
      CHECK(replacement.valid && !replacement.complete && replacement.icons.size() == 1);
      for (int64_t delta : {int64_t{-4096}, int64_t{0x10000000000ll}}) {
        m.put(marker, vt + uint64_t(delta));
        CHECK(ot0::ReadMinimapIcons(m.reader(), main, delta).complete);
        CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      }
      m.put(marker, vt);
    }
    CHECK(m.writes == 0);
    return;
  }
  if (icons.valid) {
    for (const auto &icon : icons.icons) {
      CHECK(icon.quest ||
            icon.frame_path.find("MiniMapIcon_37_png") == std::string::npos);
      CHECK(icon.background_frame_path.ends_with(
          "MiniMapIcon_Back_png.UiTX_MainMenu_MiniMapIcon_Back_png"));
    }
    int quests{};
    for (const auto &icon : icons.icons)
      quests += icon.quest;
    CHECK(quests == 2);
    CHECK(icons.player_color[0] > .75f && icons.player_color[0] < .77f);
    auto q = [&](uint64_t address) {
      uint64_t value{};
      CHECK(m.read(address, &value, 8));
      return value;
    };
    const uint64_t manager = q(main + 0x7cc7588);
    const uint64_t resource = q(manager + 0x448);
    const uint64_t markers = q(resource + 0x48);
    const uint64_t marker = q(markers);
    const auto resolve_weak = [&](uint64_t value) {
      const uint32_t index = uint32_t(value);
      const auto chunks = q(main + 0x6ce97e8 + 0x10);
      const auto chunk = q(chunks + 8 * uint64_t(index >> 16));
      return q(chunk + 20 * uint64_t(index & 65535));
    };
    std::vector<std::pair<uint64_t, uint8_t>> face_slates;
    const auto save_face = [&](uint64_t widget) {
      const auto slate = q(widget + 0x108);
      uint8_t visibility{};
      CHECK(m.read(slate + 0x184, &visibility, 1));
      if (visibility != 1 && visibility != 0x11 && visibility != 0x19)
        return;
      const auto face_slate = q(q(widget + 0x308) + 0x108);
      CHECK(m.read(face_slate + 0x184, &visibility, 1));
      face_slates.push_back({face_slate, visibility});
    };
    for (int i = 0; i < 8; ++i)
      save_face(resolve_weak(q(q(markers + 8 * uint64_t(i)) + 0xa8)));
    for (auto quest_marker : {manager + 0x578, q(q(resource + 0x38))})
      save_face(resolve_weak(q(quest_marker + 0xa4)));
    // Hidden native faces remain a supported no-draw case. Restore each
    // capture's actual visibility before testing its zero-area default branch.
    CHECK(face_slates.size() == 9);
    for (auto [slate, visibility] : face_slates) {
      (void)visibility;
      m.put(slate + 0x184, uint8_t{4});
    }
    CHECK(ot0::ReadMinimapIcons(m.reader(), main).complete == ready);
    for (auto [slate, visibility] : face_slates)
      m.put(slate + 0x184, visibility);
    CHECK(ot0::ReadMinimapIcons(m.reader(), main).complete == ready);
    if (ready) {
      const auto widget = resolve_weak(q(marker + 0xa8));
      const auto face = q(widget + 0x308);
      const auto face_slate = q(face + 0x108);
      const auto loader = q(face + 0x2f8);
      const auto image = q(loader + 0x2f8);
      const auto image_slate = q(image + 0x288);
      const auto material = q(image + 0x200);
      const auto sprite = resolve_weak(q(image + 0x2fc));
      const auto current = q(image + 0x2d0);
      const auto custom = q(image + 0x2c8);
      // Positive arranged face area is an unsupported rendered layer, even
      // with null textures and the same visible flag as the zero-area tree.
      m.put(face_slate + 0xd0, std::array<float, 2>{1, 1});
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(face_slate + 0xd0, std::array<float, 2>{0, 0});
      // A newly loaded current texture invalidates parity independently of
      // stale zero geometry while native top opacity is0.
      m.put(sprite + 0x68, uint64_t{0x18000000});
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(sprite + 0x68, uint64_t{0});
      // A later animation frame also invalidates before it becomes current.
      const auto frames = q(current + 0x30);
      const auto later = q(frames + 16);
      m.put(later + 0x68, uint64_t{0x18000000});
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(later + 0x68, uint64_t{0});
      // Detect a newly populated source flipbook before its current-copy or
      // Slate caches update. Never follow the unsupported texture pointer.
      std::array<uint8_t, 16> custom_header{};
      CHECK(m.read(custom + 0x30, custom_header.data(), custom_header.size()));
      m.block(0x17000000, 16);
      m.block(0x18000000, 0xd0);
      m.put(0x17000000, uint64_t{0x18000000});
      m.put(0x18000000, main + 0x635b7d8);
      m.put(0x18000068, uint64_t{0x19000000});
      m.put(custom + 0x30, uint64_t{0x17000000});
      m.put(custom + 0x38, int32_t{1});
      m.put(custom + 0x3c, int32_t{1});
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(custom + 0x30, custom_header);
      m.put(material + 0x1a0, int32_t{1});
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      m.put(material + 0x1a0, int32_t{0});
      // Missing or changing geometry cannot become evidence of zero area.
      int geometry_reads{};
      const auto moving = [&](uint64_t p, void *v, size_t n) {
        if (!m.read(p, v, n))
          return false;
        if (p == face_slate + 0xd0 && n == 0x38 && ++geometry_reads == 2) {
          const float positive = 1;
          std::memcpy(v, &positive, sizeof positive);
        }
        return true;
      };
      const auto changing = ot0::ReadMinimapIcons(moving, main);
      CHECK(changing.valid && !changing.complete);
      const auto missing = [&](uint64_t p, void *v, size_t n) {
        return p != face_slate + 0xd0 && m.read(p, v, n);
      };
      CHECK(!ot0::ReadMinimapIcons(missing, main).complete);
      for (const int64_t delta : {int64_t{-4096}, int64_t{0x10000000000ll}}) {
        m.put(face, main + 0x625f360 + uint64_t(delta));
        m.put(image_slate, main + 0x5b45718 + uint64_t(delta));
        m.put(current, main + 0x635b400 + uint64_t(delta));
        CHECK(ot0::ReadMinimapIcons(m.reader(), main, delta).complete);
        CHECK(!ot0::ReadMinimapIcons(m.reader(), main).complete);
      }
      m.put(face, main + 0x625f360);
      m.put(image_slate, main + 0x5b45718);
      m.put(current, main + 0x635b400);
      CHECK(ot0::ReadMinimapIcons(m.reader(), main).complete);
    }
    // Native branch becomes nonempty: known POIs remain usable, but the
    // companion must not hide the newly unsupported native marker layer.
    m.put(resource + 0x60, int32_t{1});
    m.put(resource + 0x64, int32_t{1});
    const auto extra = ot0::ReadMinimapIcons(m.reader(), main);
    CHECK(extra.valid && !extra.complete && extra.icons.size() == 9);
    m.put(resource + 0x60, int32_t{0});
    m.put(resource + 0x64, int32_t{0});
    const uint64_t marker_vt = q(marker);
    m.put(marker, marker_vt + 8);
    CHECK(!ot0::ReadMinimapIcons(m.reader(), main).valid);
    m.put(marker, marker_vt);
    // A changed resource array between its first read and publication cannot
    // publish a mixture of old markers and a new native map section.
    int array_reads{};
    const auto torn = [&](uint64_t p, void *v, size_t n) {
      if (!m.read(p, v, n))
        return false;
      if (p == resource + 0x48 && n == 16 && ++array_reads == 2)
        static_cast<uint8_t *>(v)[0] ^= 8;
      return true;
    };
    CHECK(!ot0::ReadMinimapIcons(torn, main).valid);
    // Relocated custom marker and native top wrapper with unchanged data
    // roots: both signed deltas work; omitting the delta must refuse.
    const auto ui = found->row;
    const auto ui_vt = q(ui);
    for (const int64_t delta : {int64_t{-4096}, int64_t{0x10000000000ll}}) {
      m.put(ui, ui_vt + uint64_t(delta));
      m.put(marker, marker_vt + uint64_t(delta));
      CHECK(ot0::ReadMinimapIcons(m.reader(), main, delta).icons.size() == 9);
      CHECK(!ot0::ReadMinimapIcons(m.reader(), main).valid);
    }
    m.put(ui, ui_vt);
    m.put(marker, marker_vt);
  }
  // Captured reads prove discovery only. This replay never invokes a writer.
  CHECK(m.writes == 0);
}
} // namespace
int main() {
  Synthetic(0);
  Synthetic(0x10000000000ll);
  Synthetic(-4096);
  Canvas(0);
  Canvas(0x10000000000ll);
  Canvas(-4096);
  Native("OT0_MINIMAP_MEMORY", false);
  Native("OT0_MINIMAP_TOWN_FULL_MEMORY", true, false, false, true);
  Native("OT0_MINIMAP_TOWN_DIRECT_MEMORY", false);
  Native("OT0_MINIMAP_READY_MEMORY", false);
  Native("OT0_MINIMAP_SMITHY_MEMORY", false, true);
  Native("OT0_MINIMAP_SMITHY_DIRECT_MEMORY", true, true, true);
  Native("OT0_MINIMAP_SMITHY_FULL_MEMORY", true, true, true, false, true);
  std::fprintf(stderr, "Minimap HUD: %d failures\n", failures);
  return failures ? 1 : 0;
}
