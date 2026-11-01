// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Public-ABI integration checks. Optional native art uses the owner's OT0_PAK
// only.
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
namespace {
int failures{};
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);             \
      ++failures;                                                              \
    }                                                                          \
  } while (false)
constexpr uint64_t Main = 0x80000000, Root = 0x1400000000,
                   Manager = 0x1400001000, Save = 0x1400002000,
                   Parties = 0x1400003000, Records = 0x1400004000,
                   Ids = 0x1400005000;
constexpr char Build[] =
    "9FE60A212CFF9B9A3656AB63D4F882E500000000000000000000000000000000";
struct Host {
  std::map<uint64_t, uint8_t> memory;
  std::map<std::string, int64_t> integers;
  std::map<std::string, std::string> texts;
  std::ifstream pak;
  uint64_t pak_size{};
  unsigned romfs_calls{}, warnings{};
  bool missing_pak{};
  std::map<std::string, int64_t> host_values;
  unsigned writes{};
  std::string page{"party"};
  template <class T> void Put(uint64_t p, const T &v) {
    auto b = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof v; ++i)
      memory[p + i] = b[i];
  }
  void Zero(uint64_t p, size_t n) {
    for (size_t i = 0; i < n; ++i)
      memory[p + i] = 0;
  }
  void Array(uint64_t p, uint64_t ptr, int count, int cap) {
    Put(p, ptr);
    Put(p + 8, int32_t(count));
    Put(p + 12, int32_t(cap));
  }
  void Fixture() {
    Put(Main + 0x7cc8cc0, Root);
    Put(Root, Main + 0x6207610);
    Put(Root + 0x2f0, std::array<uint8_t, 3>{23, 25, 0});
    Put(Main + 0x7cc8540, Manager);
    Put(Manager + 0x2c8, Save);
    Put(Save + 0x44, uint32_t(0));
    Array(Save + 0x128, Parties, 1, 1);
    Array(Save + 0x138, Records, 2, 2);
    Zero(Parties, 0x20);
    Array(Parties + 8, Ids, 36, 36);
    Zero(Records, 2 * 0x6c);
    Put(Records, uint32_t(900));
    Put(Records + 0xc, uint32_t(15));
    Put(Records + 0x10, uint32_t(727));
    Put(Records + 0x14, uint32_t(103));
    Put(Records + 0x6c, uint32_t(10503));
    Put(Records + 0x6c + 0xc, uint32_t(14));
    Put(Records + 0x6c + 0x10, uint32_t(558));
    Put(Records + 0x6c + 0x14, uint32_t(68));
    Put(Ids, std::array<uint32_t, 8>{901, 10503, 0, 0, 0, 0, 0, 0});
  }
  uint32_t next_name{0x100};
  uint32_t MapName(const std::string &text) {
    constexpr uint64_t Pool = 0x1400020000;
    const auto index = next_name;
    next_name += uint32_t((text.size() + 3) / 2 + 1);
    Put(Pool + 2 * index, uint16_t(text.size() << 6));
    for (size_t i = 0; i < text.size(); ++i)
      Put(Pool + 2 * index + 2 + i, uint8_t(text[i]));
    return index;
  }
  // Synthetic native-layout roots. Only the asset filename references the
  // owner's pinned pak; no recorded guest-memory bytes are embedded here.
  void MapFixture() {
    constexpr uint64_t Mini = 0x1400010000, Resource = 0x1400011000,
                       Volume = 0x1400012000, Texture = 0x1400013000,
                       Package = 0x1400014000, Nodes = 0x1400015000,
                       Cells = 0x1400016000, Controller = 0x1400017000,
                       Character = 0x1400018000, Pool = 0x1400020000;
    Put(Main + 0x7cc7588, Mini);
    Zero(Mini, 0x9d0);
    Put(Mini, Main + 0x63be0f0);
    Put(Main + 0x6c91440 + 0x38, Pool);
    Put(Mini + 0x348, uint32_t(81));
    Put(Mini + 0x34c, MapName("Map_DP_Plain_N0_1001_A1"));
    Put(Mini + 0x358, std::array<double, 3>{3100, 1500, 0});
    Put(Mini + 0x448, Resource);
    Zero(Resource, 0x110);
    auto section = MapName("Room");
    Put(Resource, uint64_t(section));
    Put(Main + 0x7cc7640, std::array<double, 2>{1, -1});
    Put(Resource + 0x98, std::array<double, 2>{0, 4096});
    Put(Resource + 0xb8, std::array<double, 2>{.25, .25});
    Put(Resource + 0xc8, std::array<double, 3>{0, 0, -100});
    Put(Resource + 0xe0, std::array<double, 3>{4096, 4096, 100});
    Put(Resource + 0xf8, uint8_t(1));
    Put(Resource + 0x100, Volume);
    Zero(Volume, 0x370);
    Put(Main + 0x7cbbcd0, uint64_t(0x1400030000));
    Put(Volume + 0x10, uint64_t(0x1400030000));
    Put(Volume + 0x18, uint64_t(section));
    Put(Volume + 0x2c9, uint8_t(1));
    Put(Volume + 0x2f8, int32_t(6144));
    Put(Volume + 0x310, int32_t(1050));
    Put(Volume + 0x314, float(3));
    constexpr uint64_t Mask = 0x1400031000, MaskPackage = Mask + 0x100;
    Zero(Mask, 0x28);
    Zero(MaskPackage, 0x28);
    Put(Mask + 0x18, MapName("T_mask"));
    Put(Mask + 0x20, MaskPackage);
    Put(MaskPackage + 0x18, MapName("/Game/UI/MiniMap/BP/T_mask"));
    Put(Mini + 0x7e0, Mask);
    Put(Volume + 0x2d8, Texture);
    Zero(Texture, 0x28);
    Zero(Package, 0x28);
    const std::string asset =
        "Map_DP_Plain_N0_1001_A1_Room_MinimapBoundsVolume3";
    Put(Texture + 0x18, MapName(asset));
    Put(Texture + 0x20, Package);
    Put(Package + 0x18,
        MapName("/Game/UI/MiniMap/Texture/MAP/Plain/Map_DP_Plain_N0_1001_A1/" +
                asset));
    Zero(Save + 0xd18, 0x58);
    Put(Save + 0xd18, Nodes);
    Put(Save + 0xd20, int32_t(1));
    Put(Save + 0xd24, int32_t(1));
    Put(Save + 0xd28, uint32_t(1));
    Put(Save + 0xd40, int32_t(1));
    Put(Save + 0xd44, int32_t(128));
    Put(Save + 0xd48, int32_t(-1));
    Put(Save + 0xd60, int32_t(1));
    Put(Save + 0xd68, int32_t(200));
    Zero(Nodes, 0x28);
    Put(Nodes, uint32_t(81));
    Put(Nodes + 4, uint64_t(section));
    Array(Nodes + 0x10, Cells, 2, 2);
    Put(Nodes + 0x20, int32_t(-1));
    Put(Cells, std::array<int32_t, 4>{15, 7, 14, 7});
    Put(Main + 0x7cc6288, Controller);
    Put(Controller + 0x290, Character);
    Zero(Character, 0x28);
    Put(Character, Main + 0x6416cf0);
    Put(Character + 0x920, uint8_t(3));
  }
  static constexpr uint64_t Battle = 0x1400100000, BattleData = 0x1400110000,
                            Enemy = 0x1400120000, HudLayout = 0x1400130000,
                            HudControl = 0x1400140000, HudRow = 0x1400150000,
                            HudWidget = 0x1400160000,
                            HudController = 0x1400170000, Cmd = 0x1400180000,
                            CmdWidget = 0x1400190000,
                            CmdController = 0x14001a0000,
                            ControlWidget = 0x1400230000, ControlController = 0x1400240000,
                            MenuManager = 0x14001b0000, Dialog = 0x14001c0000,
                            DialogWidget = 0x14001d0000, DialogController = 0x14001e0000,
                            FrontActor = 0x14001f0000, BackActor = 0x1400200000,
                            FrontPart = 0x1400210000, BackPart = 0x1400220000;
  void BattleFixture(int64_t delta) {
    host_values["__aux_present"] = 1;
    host_values["@flag:ot0.hide_party_hud"] = 1;
    host_values["__relocation_delta"] = delta;
    const auto vt = [&](uint64_t offset) {
      return Main + offset + uint64_t(delta);
    };
    Put(Main+0x7cc9a18,MenuManager);
    Put(MenuManager,vt(0x63b6f18));Put(MenuManager+8,uint32_t(8));
    Put(MenuManager+0x2a8,Dialog);Put(Dialog,vt(0x63aae78));Put(Dialog+8,uint32_t(8));
    Put(Dialog+0x108,DialogWidget);Put(Dialog+0x110,DialogController);
    Put(DialogController+8,int32_t(1));Put(DialogWidget,vt(0x5d011e8));
    Put(DialogWidget+8,DialogWidget);Put(DialogWidget+0x184,std::array<uint8_t,2>{2,1});
    Put(Root, vt(0x6207610));
    Put(Root+8,uint32_t(8));
    Put(Root + 0x2f0, uint8_t(26));
    Put(Main + 0x7cc5b78, Battle);
    Put(Battle + 8, uint32_t(8));
    Array(Battle + 0xa98, BattleData, 1, 1);
    Put(BattleData, Enemy);
    Put(Enemy + 0x298, uint32_t(32312));
    Put(Enemy + 0x290, uint8_t(1));
    Put(Enemy + 0xa48, uint32_t(0x8c));
    Put(Enemy + 0xa4c, uint32_t(2));
    const auto ints = [&](uint64_t header, uint64_t at,
                          std::array<int32_t, 8> values) {
      Array(header, at, 8, 8);
      Put(at, values);
    };
    const auto record = Save + 0x4f0;
    ints(record + 0x228, BattleData + 0x100, {1, 0, 0, 0, 0, 0, 0, 0});
    ints(record + 0x1c8, BattleData + 0x200, {901, 10503, 0, 0, 0, 0, 0, 0});
    ints(record + 0x1d8, BattleData + 0x300, {727, 558, 0, 0, 0, 0, 0, 0});
    ints(record + 0x1e8, BattleData + 0x400, {103, 68, 0, 0, 0, 0, 0, 0});
    ints(record + 0x1f8, BattleData + 0x500, {1, 1, 0, 0, 0, 0, 0, 0});
    Array(Save + 0x158, 0, 0, 0);
    Array(Battle + 0x8b8, BattleData + 0x600, 1, 1);
    Put(BattleData + 0x600, Enemy);
    Put(Battle + 0x8e0, int32_t(0));
    Array(Battle + 0x8f8, 0, 0, 0);
    Put(Battle + 0x1678, HudLayout);
    Put(HudLayout, vt(0x6221a30));
    Put(HudLayout+8,uint32_t(8));Put(HudLayout+0xdc,uint8_t(4));
    Put(HudLayout + 0x2f8, HudControl);
    Put(HudControl, vt(0x62281d0));
    Put(HudControl + 0x108, ControlWidget);
    Put(HudControl + 0x110, ControlController);
    Put(ControlController + 8, uint32_t(1));
    Put(ControlWidget, vt(0x5d011e8));
    Put(ControlWidget + 8, ControlWidget);
    Zero(ControlWidget + 0x198, 28);

    Put(HudControl+8,uint32_t(8));Put(HudControl+0xdc,uint8_t(4));
    Array(HudControl + 0x328, BattleData + 0x700, 1, 1);
    Put(BattleData + 0x700, HudRow);
    Put(HudRow, vt(0x6232f28));
    Put(HudRow+8,uint32_t(8));Put(HudRow+0xdc,uint8_t(4));
    for(const auto actor:{FrontActor,BackActor}) {
      Zero(actor,0x300);Put(actor,vt(0x63f39b8));Put(actor+8,uint32_t(8));
    }
    Put(FrontActor+0x298,uint32_t(901));Put(BackActor+0x298,uint32_t(10503));
    for(const auto part:{FrontPart,BackPart}) {
      Zero(part,0x420);Put(part+8,uint32_t(8));Put(part+0xdc,uint8_t(4));
      Put(part+0x368,HudRow);
    }
    Put(FrontPart,vt(0x63222a8));Put(BackPart,vt(0x6232f28));
    Put(FrontPart+0x2f8,FrontActor);Put(BackPart+0x2f8,BackActor);
    Put(FrontPart+0x400,std::array<int32_t,4>{727,103,765,103});
    Put(BackPart+0x400,std::array<int32_t,4>{558,68,747,68});
    Put(HudRow+0x370,FrontPart);Put(HudRow+0x378,BackPart);
    Put(HudRow + 0x108, HudWidget);
    Put(HudRow + 0x110, HudController);
    Put(HudController + 8, uint32_t(1));
    Put(HudWidget, vt(0x5d011e8));
    Put(HudWidget + 8, HudWidget);
    Put(HudWidget + 0x1cc, float(1));
    Put(HudControl + 0x300, Cmd);
    Put(Cmd, vt(0x621cf90));
    Put(Cmd + 0x108, CmdWidget);
    Put(Cmd + 0x110, CmdController);
    Put(CmdController + 8, uint32_t(1));
    Put(CmdWidget, vt(0x5d011e8));
    Put(CmdWidget + 8, CmdWidget);
    Put(CmdWidget + 0x198, std::array<float, 6>{1, 0, 0, 1, -32, 0});
    Put(CmdWidget + 0x1b0, uint8_t(1));
  }
  float Float(uint64_t address) const {
    float value{};
    auto *b = reinterpret_cast<uint8_t *>(&value);
    for (size_t i = 0; i < 4; ++i)
      b[i] = memory.at(address + i);
    return value;
  }
  EdenDsmodHostWriteApi WriteApi() {
    return {EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(EdenDsmodHostWriteApi),
            EDEN_DSMOD_WRITE_EXT_HASH, this,
            [](void *p, const EdenDsmodWriteOp *ops,
               uint32_t count) -> EdenDsmodBool {
              auto &self = *static_cast<Host *>(p);
              for (uint32_t i = 0; i < count; ++i) {
                if (!ops[i].size || ops[i].size > EDEN_DSMOD_WRITE_BATCH_MAX_BYTES ||
                    !ops[i].expect || !ops[i].value) return EDEN_DSMOD_FALSE;
                const auto* expect = static_cast<const uint8_t*>(ops[i].expect);
                for (uint32_t j=0; j<ops[i].size; ++j) {
                  const auto it=self.memory.find(ops[i].address+j);
                  if (it==self.memory.end() || it->second!=expect[j]) return EDEN_DSMOD_FALSE;
                }
              }
              for (uint32_t i=0; i<count; ++i) {
                const auto* value=static_cast<const uint8_t*>(ops[i].value);
                for (uint32_t j=0; j<ops[i].size; ++j) self.memory[ops[i].address+j]=value[j];
                ++self.writes;
              }
              return EDEN_DSMOD_TRUE;
            }};
  }
  Host() {
    Fixture();
    if (auto path = std::getenv("OT0_PAK")) {
      pak.open(path, std::ios::binary);
      if (pak) {
        pak.seekg(0, std::ios::end);
        pak_size = uint64_t(pak.tellg());
        pak.seekg(0);
      }
    }
  }
  EdenDsmodHostApi Api() {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.struct_size = sizeof h;
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.title_id = 0x01005270232F2000ULL;
    h.main_base = Main;
    h.userdata = this;
    for (size_t i = 0; i < 32; ++i) {
      unsigned n{};
      std::sscanf(Build + 2 * i, "%2x", &n);
      h.build_id[i] = uint8_t(n);
    }
    h.read_memory = [](void *p, uint64_t address, void *out,
                       size_t n) -> EdenDsmodBool {
      auto &self = *static_cast<Host *>(p);
      auto b = static_cast<uint8_t *>(out);
      for (size_t i = 0; i < n; ++i) {
        auto it = self.memory.find(address + i);
        if (it == self.memory.end())
          return EDEN_DSMOD_FALSE;
        b[i] = it->second;
      }
      return EDEN_DSMOD_TRUE;
    };
    h.get_i64 = [](void *p, const char *k, int64_t fallback) {
      const auto &values = static_cast<Host *>(p)->host_values;
      const auto it = values.find(k);
      return it == values.end() ? fallback : it->second;
    };
    h.get_text = [](void *p, const char *k) -> const char * {
      const auto &page = static_cast<Host *>(p)->page;
      return std::strcmp(k, "__page") == 0 && !page.empty() ? page.c_str()
                                                            : nullptr;
    };
    h.publish_i64 = [](void *p, const char *k, int64_t v) {
      static_cast<Host *>(p)->integers[k] = v;
    };
    h.publish_text = [](void *p, const char *k, const char *v) {
      static_cast<Host *>(p)->texts[k] = v;
    };
    h.log = [](void *p, uint32_t level, const char *) {
      if (level == EDEN_DSMOD_LOG_WARNING)
        ++static_cast<Host *>(p)->warnings;
    };
    h.read_romfs = [](void *p, const char *path, uint64_t offset, void *out,
                      size_t n) -> size_t {
      auto &self = *static_cast<Host *>(p);
      ++self.romfs_calls;
      if (self.missing_pak || !self.pak ||
          std::strcmp(
              path,
              "romfs:Octopath_Traveler0/Content/Paks/pakchunk0-Switch.pak"))
        return 0;
      if (!out)
        return self.pak_size;
      if (offset > self.pak_size || n > self.pak_size - offset)
        return 0;
      self.pak.clear();
      self.pak.seekg(offset);
      self.pak.read(static_cast<char *>(out), n);
      return size_t(self.pak.gcount());
    };
    return h;
  }
};
struct Image {
  uint32_t width{}, height{};
  std::vector<uint8_t> rgba;
  unsigned calls{};
};
void Sink(void *p, uint32_t w, uint32_t h, const uint8_t *bytes, size_t n) {
  auto &image = *static_cast<Image *>(p);
  ++image.calls;
  image.width = w;
  image.height = h;
  if (bytes)
    image.rgba.assign(bytes, bytes + n);
}
void Sample(const EdenDsmodModuleApi *module, void *instance,
            EdenDsmodHostApi &host, unsigned n = 6) {
  while (n--)
    module->sample(instance, &host);
}
void Shape(const Image &image) {
  CHECK(image.calls == 1);
  CHECK(image.width == 1240 && image.height == 1080);
  CHECK(image.rgba.size() == size_t(1240) * 1080 * 4);
  if (image.rgba.size() == size_t(1240) * 1080 * 4) {
    bool opaque = true;
    for (size_t i = 3; i < image.rgba.size(); i += 4)
      opaque &= image.rgba[i] == 255;
    CHECK(opaque);
  }
}
// The selected Celsus portrait contains warm skin/gold detail. Selection chrome
// alone also changes pixels, so require actual illustration content inside the
// art region, excluding labels and the blue frame highlight.
void CelsusIllustration(const Image &image) {
  if (image.width != 1240 || image.height != 1080 ||
      image.rgba.size() != size_t(1240) * 1080 * 4) {
    CHECK(false);
    return;
  }
  size_t warm_pixels = 0;
  for (size_t y = 110; y < 620; ++y)
    for (size_t x = 30; x < 550; ++x) {
      const size_t at = (y * image.width + x) * 4;
      const int r = image.rgba[at], g = image.rgba[at + 1],
                b = image.rgba[at + 2];
      if (r > 50 && g > 35 && r > b + 16 && g > b + 8)
        ++warm_pixels;
    }
  CHECK(warm_pixels > 1000);
}
void Dump(const Image &image) {
  if (auto path = std::getenv("OT0_TEST_SCREEN")) {
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4)
      out.write(reinterpret_cast<const char *>(image.rgba.data() + i), 3);
    CHECK(bool(out));
  }
}
void MapNavigation(const EdenDsmodModuleApi *api,
                   const EdenDsmodModuleExtensions *ext) {
  Host host;
  host.MapFixture();
  auto h = host.Api();
  void *instance = api->create(&h, "{}");
  CHECK(instance);
  if (!instance)
    return;
  Sample(api, instance, h);
  CHECK(host.integers["ot0.ready"] == 1);
  CHECK(host.integers["ot0.map_available"] == 1 &&
        host.integers["ot0.map_on"] == 1);
  for (int i = 0; i < 8; ++i)
    CHECK(host.integers["ot0.p" + std::to_string(i) + ".on"] == 0);
  CHECK(!ext->on_action(instance, "select", 1));
  CHECK(!ext->on_action(instance, "page", -1));
  CHECK(!ext->on_action(instance, "page", 4));
  CHECK(!ext->on_action(instance, "page", INT64_MAX));
  CHECK(!ext->on_action(instance, "map", 0));
  const auto memory_before = host.memory;
  const bool real = std::getenv("OT0_PAK") != nullptr;
  Image map_image, party_image, empty_map;
  const auto map_key = host.texts["ot0.screen"];
  if (real) {
    CHECK(ext->load_image(instance, &h, map_key.c_str(), &map_image, Sink));
    Shape(map_image);
  }
  CHECK(ext->on_action(instance, "page", 1));
  Sample(api, instance, h);
  CHECK(host.integers["ot0.map_on"] == 0 &&
        host.integers["ot0.map_available"] == 1);
  // Party page selects through the pair rows; the old Formation card taps stay off.
  CHECK(host.integers["ot0.p0.on"] == 0 && host.integers["ot0.mp0.on"] == 1 &&
        host.integers["ot0.mp1.on"] == 1 && host.integers["ot0.mp2.on"] == 0);
  CHECK(host.integers["ot0.p0.x"] >= 0 && host.integers["ot0.p0.y"] >= 0);
  CHECK(ext->on_action(instance, "select", 1));
  Sample(api, instance, h);
  CHECK(host.integers["ot0.selected"] == 1);
  if (real) {
    CHECK(ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                          &party_image, Sink));
    Shape(party_image);
    CHECK(party_image.rgba != map_image.rgba);
  }
  CHECK(ext->on_action(instance, "page", 0));
  Sample(api, instance, h);
  CHECK(host.integers["ot0.map_on"] == 1 && host.integers["ot0.selected"] == 1);
  CHECK(!ext->on_action(instance, "select", 0));
  CHECK(ext->on_action(instance, "page", 1));
  Sample(api, instance, h);
  CHECK(host.integers["ot0.selected"] == 1);
  CHECK(host.memory == memory_before);
  // A world party reorder retains the selected identity while browsing either
  // page.
  host.Put(Ids, std::array<uint32_t, 8>{10503, 901, 0, 0, 0, 0, 0, 0});
  Sample(api, instance, h);
  CHECK(host.integers["ot0.selected"] == 0);
  CHECK(ext->on_action(instance, "page", 0));
  Sample(api, instance, h);
  // An empty current-section reveal set remains an honest empty map, with no
  // synthetic discovery or party hit regions. Source mask raster checks live
  // in the dedicated map tests.
  constexpr uint64_t Nodes = 0x1400015000;
  host.Put(Nodes + 0x18, int32_t(0));
  Sample(api, instance, h);
  CHECK(host.integers["ot0.map_available"] == 1 &&
        host.integers["ot0.map_on"] == 1);
  for (int i = 0; i < 8; ++i)
    CHECK(host.integers["ot0.p" + std::to_string(i) + ".on"] == 0);
  if (real) {
    CHECK(ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                          &empty_map, Sink));
    Shape(empty_map);
    CHECK(empty_map.rgba != map_image.rgba);
  }
  // An absent native current section clears the map instead of guessing one (after the short
  // hold-off that rides out a single failed read).
  host.Put(0x1400010000 + 0x448, uint64_t(0));
  for (int i = 0; i < 41; ++i)
    Sample(api, instance, h);
  CHECK(host.integers["ot0.map_available"] == 0 &&
        host.integers["ot0.map_on"] == 0);
  CHECK(!ext->on_action(instance, "page", 0));
  CHECK(ext->on_action(instance, "page", 1));
  for (uint8_t state : {uint8_t(10), uint8_t(12), uint8_t(14), uint8_t(27)}) {
    host.Put(Root + 0x2f0, state);
    Sample(api, instance, h);
    CHECK(host.integers["ot0.ready"] == 0 &&
          host.integers["ot0.map_available"] == 0 &&
          host.integers["ot0.map_on"] == 0);
    CHECK(!ext->on_action(instance, "page", 0));
    CHECK(!ext->on_action(instance, "page", 1));
    CHECK(!ext->on_action(instance, "select", 0));
  }
  api->destroy(instance);
}
void BattleReadiness(const EdenDsmodModuleApi *api,
                     const EdenDsmodModuleExtensions *ext) {
  const auto *writes = eden_dsmod_get_write_extensions(
      EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
  CHECK(writes);
  if (!writes)
    return;
  for (int64_t delta : {int64_t(0), int64_t(0x10000000000), int64_t(-0x1000)}) {
    Host host;
    host.BattleFixture(delta);
    auto h = host.Api();
    auto w = host.WriteApi();
    auto *instance = api->create(&h, "{}");
    CHECK(instance);
    if (!instance)
      continue;
    writes->configure(instance, &w);
    Sample(api, instance, h);
    CHECK(host.integers["ot0.ready"] == 1 && host.integers["ot0.battle"] == 1);
    // Valid readers alone cannot hide the top HUD before the bottom was drawn.
    for (int i = 0; i < 32; ++i)
      api->tick(instance, &h);
    CHECK(host.writes == 0 && host.Float(Host::HudWidget + 0x1cc) == 1);
    host.missing_pak = true;
    Image missing;
    CHECK(!ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                           &missing, Sink));
    api->tick(instance, &h);
    CHECK(host.writes == 0);
    if (std::getenv("OT0_PAK")) {
      host.missing_pak = false;
      const auto initial_key = host.texts["ot0.screen"];
      Image rendered;
      CHECK(
          ext->load_image(instance, &h, initial_key.c_str(), &rendered, Sink));
      Shape(rendered);
      // First ready frame is between maintenance ticks: hide the native party
      // before raising commands into its space, rather than waiting 0.5 seconds.
      api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 112);
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 112);
      const auto repaint=[&] {
        Sample(api,instance,h);
        Image frame;
        CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&frame,Sink));
      };
      // Missing page support cannot prove that the battle image is visible.
      host.page.clear();
      api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
      host.page = "party";
      api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      repaint();
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      // Settings replaces the battle page, so the native party information
      // must return even though the auxiliary display remains present.
      host.page = "@settings";
      api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
      host.page = "party";
      api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      repaint();
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      // A hidden companion restores immediately, without the 30-tick cadence.
      host.host_values["__aux_present"] = 0;
      api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
      host.host_values["__aux_present"] = 1;
      api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      repaint();
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      // A refused root cannot leave a valid HUD writer hiding the native UI.
      host.Put(Root, Main + 0x6207618 + uint64_t(delta));
      Sample(api, instance, h);
      api->tick(instance, &h);
      CHECK(host.integers["ot0.ready"] == 0);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
      // Restoring the root does not authorize hiding until fresh art exists.
      host.Put(Root, Main + 0x6207610 + uint64_t(delta));
      Sample(api, instance, h);
      api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
      // An old image request cannot re-authorize a new battle generation.
      Image stale;
      CHECK(ext->load_image(instance, &h, initial_key.c_str(), &stale, Sink));
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      Image fresh;
      CHECK(ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                            &fresh, Sink));
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      // A real host setting wins over the legacy action fallback.
      host.host_values["@flag:ot0.hide_party_hud"] = 0;
      api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
      host.host_values["@flag:ot0.hide_party_hud"] = 1;
      for (int i = 0; i < 32; ++i)
        api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
      // All foes retired: results/wave pauses have no combat controls and
      // cannot hide the top HUD even after their native idle image loads.
      host.Put(Host::Enemy+0x290,uint8_t(0));
      Sample(api,instance,h);api->tick(instance,&h);
      CHECK(host.integers["ot0.combat"]==0 && host.integers["ot0.ready"]==0);
      CHECK(!ext->on_action(instance,"foe",0));
      CHECK(!ext->on_action(instance,"select",0));
      CHECK(!ext->on_action(instance,"page",1));
      repaint();api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget+0x1cc)==1);
      host.Put(Host::Enemy+0x290,uint8_t(1));
      repaint();for(int i=0;i<32;++i)api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget+0x1cc)==0);
      // A native confirmation blocks local controls and restores immediately.
      host.Put(Host::DialogWidget+0x184,uint8_t(0x11));
      api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget+0x1cc)==1);
      Sample(api,instance,h);
      CHECK(host.integers["ot0.ready"]==0);
      CHECK(!ext->on_action(instance,"foe",0));
      repaint();api->tick(instance,&h);
      CHECK(host.Float(Host::HudWidget+0x1cc)==1);
      host.Put(Host::DialogWidget+0x184,uint8_t(2));
      api->tick(instance,&h);repaint();
      host.Put(Root + 0x2f0, uint8_t(23));
      Sample(api, instance, h);
      api->tick(instance, &h);
      CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
      CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
    }
    api->destroy(instance);
  }
}
// Teardown runs through the ABI, with the host callbacks still live. Merely
// observing an already-restored module would not exercise destruction cleanup.
void BattleTeardown(const EdenDsmodModuleApi *api,
                    const EdenDsmodModuleExtensions *ext) {
  if (!std::getenv("OT0_PAK"))
    return;
  const auto *writes = eden_dsmod_get_write_extensions(
      EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
  CHECK(writes);
  if (!writes)
    return;
  for (int64_t delta : {int64_t(0), int64_t(0x10000000000), int64_t(-4096)}) {
    Host host;
    host.BattleFixture(delta);
    auto h = host.Api();
    auto w = host.WriteApi();
    auto *instance = api->create(&h, "{}");
    CHECK(instance);
    if (!instance)
      continue;
    writes->configure(instance, &w);
    Sample(api, instance, h);
    api->tick(instance, &h);
    Image frame;
    CHECK(ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                          &frame, Sink));
    for (int i = 0; i < 32; ++i)
      api->tick(instance, &h);
    CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
    CHECK(host.Float(Host::ControlWidget + 0x1a8) == 112);
    CHECK(host.Float(Host::ControlWidget + 0x1ac) == -368);
    const auto writes_before = host.writes;
    api->destroy(instance);
    CHECK(host.writes > writes_before);
    CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
    CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
    CHECK(host.Float(Host::ControlWidget + 0x1ac) == 0);
  }
}
void LegacySettingsSafety(const EdenDsmodModuleApi *api,
                          const EdenDsmodModuleExtensions *ext) {
  if (!std::getenv("OT0_PAK"))
    return;
  Host host;
  host.BattleFixture(0);
  // Keep valid readers, art, modal checks, page reporting and aux presence.
  // Only the live @flag protocol is absent, as on the older rt22 hosts.
  host.host_values.erase("@flag:ot0.hide_party_hud");
  auto h = host.Api();
  auto w = host.WriteApi();
  auto *instance = api->create(&h, "{}");
  CHECK(instance);
  if (!instance)
    return;
  const auto *writes = eden_dsmod_get_write_extensions(
      EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
  CHECK(writes);
  if (!writes) {
    api->destroy(instance);
    return;
  }
  writes->configure(instance, &w);
  Sample(api, instance, h);
  api->tick(instance, &h);
  Image frame;
  CHECK(ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                        &frame, Sink));
  CHECK(ext->on_action(instance, "hud_hide", 1));
  for (int i = 0; i < 32; ++i)
    api->tick(instance, &h);
  Sample(api, instance, h);
  CHECK(host.integers["ot0.ready"] == 1 && host.integers["ot0.combat"] == 1);
  CHECK(host.writes == 0);
  CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
  CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
  CHECK(host.Float(Host::ControlWidget + 0x1ac) == 0);
  // Counterfactual: the same rendered fixture can hide once a working re-show
  // protocol exists. Thus the preceding refusal was not missing art or data.
  host.host_values["@flag:ot0.hide_party_hud"] = 1;
  for (int i = 0; i < 32; ++i)
    api->tick(instance, &h);
  CHECK(host.Float(Host::HudWidget + 0x1cc) == 0);
  CHECK(host.Float(Host::ControlWidget + 0x1a8) == 112);
  host.host_values.erase("@flag:ot0.hide_party_hud");
  api->tick(instance, &h);
  CHECK(host.Float(Host::HudWidget + 0x1cc) == 1);
  CHECK(host.Float(Host::ControlWidget + 0x1a8) == 0);
  CHECK(host.Float(Host::ControlWidget + 0x1ac) == 0);
  api->destroy(instance);
}
void BattleRoleChanges(const EdenDsmodModuleApi* api,const EdenDsmodModuleExtensions* ext) {
  if(!std::getenv("OT0_PAK"))return;
  Host host;host.BattleFixture(0);
  auto h=host.Api();auto w=host.WriteApi();
  auto* instance=api->create(&h,"{}");CHECK(instance);if(!instance)return;
  const auto* writes=eden_dsmod_get_write_extensions(EDEN_DSMOD_WRITE_EXT_VERSION,EDEN_DSMOD_WRITE_EXT_HASH);
  CHECK(writes);if(!writes){api->destroy(instance);return;}
  writes->configure(instance,&w);Sample(api,instance,h);
  CHECK(host.integers["ot0.battle_layout_known"]==1);
  Image original;CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&original,Sink));
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==0);
  const auto old_key=host.texts["ot0.screen"];
  // Native ZR / KO replacement exchanges the widget actors while the saved
  // Formation and per-identity battle values remain unchanged.
  host.Put(Host::FrontPart+0x2f8,Host::BackActor);
  host.Put(Host::BackPart+0x2f8,Host::FrontActor);
  host.Put(Host::FrontPart+0x400,std::array<int32_t,4>{558,68,747,68});
  host.Put(Host::BackPart+0x400,std::array<int32_t,4>{727,103,765,103});
  Sample(api,instance,h);
  CHECK(host.integers["ot0.battle_layout_known"]==1 && host.texts["ot0.screen"]!=old_key);
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==1);
  Image stale;CHECK(ext->load_image(instance,&h,old_key.c_str(),&stale,Sink));
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==1);
  Image swapped;CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&swapped,Sink));
  CHECK(swapped.rgba!=original.rgba);
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==0);
  // An incomplete native row rebuild cannot authorize moved HUD data.
  host.Put(Host::FrontPart+0x2f8,uint64_t{});
  Sample(api,instance,h);api->tick(instance,&h);
  CHECK(host.integers["ot0.battle_layout_known"]==0 && host.integers["ot0.ready"]==0 && host.integers["ot0.combat"]==0);
  CHECK(host.Float(Host::HudWidget+0x1cc)==1 && host.Float(Host::ControlWidget+0x1a8)==0);
  Image unknown;CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&unknown,Sink));
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==1);
  host.Put(Host::FrontPart+0x2f8,Host::BackActor);
  Sample(api,instance,h);api->tick(instance,&h);
  CHECK(host.integers["ot0.battle_layout_known"]==1 && host.Float(Host::HudWidget+0x1cc)==1);
  Image recovered;CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&recovered,Sink));
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==0);
  // A swap while aux is hidden must be sampled on the first returning frame,
  // before a cached repaint can hide native information again.
  api->sample(instance,&h); // Move between the normal six-tick reader polls.
  host.host_values["__aux_present"]=0;api->tick(instance,&h);
  host.Put(Host::FrontPart+0x2f8,Host::FrontActor);
  host.Put(Host::BackPart+0x2f8,Host::BackActor);
  host.Put(Host::FrontPart+0x400,std::array<int32_t,4>{727,103,765,103});
  host.Put(Host::BackPart+0x400,std::array<int32_t,4>{558,68,747,68});
  host.host_values["__aux_present"]=1;api->tick(instance,&h);
  api->sample(instance,&h);
  Image shown;CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&shown,Sink));
  CHECK(shown.rgba==original.rgba);
  api->tick(instance,&h);CHECK(host.Float(Host::HudWidget+0x1cc)==0);
  // Real GameOver keeps app26 while the battle record has all actors at0 HP;
  // saved Formation resources may still have their pre-battle values.
  host.Put(Host::BattleData+0x300,std::array<int32_t,8>{});
  Sample(api,instance,h);api->tick(instance,&h);
  CHECK(host.integers["ot0.ready"]==0 && host.integers["ot0.combat"]==0);
  CHECK(host.Float(Host::HudWidget+0x1cc)==1);
  Image gameover;CHECK(ext->load_image(instance,&h,host.texts["ot0.screen"].c_str(),&gameover,Sink));
  CHECK(gameover.rgba==unknown.rgba);
  api->destroy(instance);
}
} // namespace
int main() {
  CHECK(!eden_dsmod_get_module(0, EDEN_DSMOD_MODULE_ABI_HASH));
  CHECK(!eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, 0));
  CHECK(!eden_dsmod_get_extensions(0, EDEN_DSMOD_EXT_HASH));
  CHECK(!eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, 0));
  const auto *api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION,
                                          EDEN_DSMOD_MODULE_ABI_HASH);
  const auto *ext =
      eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
  CHECK(api && ext);
  if (!api || !ext)
    return 1;
  CHECK(api->struct_size == sizeof(EdenDsmodModuleApi));
  CHECK(ext->struct_size == sizeof(EdenDsmodModuleExtensions));
  CHECK(!(api->capabilities &
          (EDEN_DSMOD_CAP_WRITE_MEMORY | EDEN_DSMOD_CAP_GUEST_CALL)));
  CHECK(api->supports_build(Build));
  CHECK(!api->supports_build(nullptr));
  CHECK(!api->supports_build("wrong"));
  CHECK(!api->create(nullptr, "{}"));
  MapNavigation(api, ext);
  BattleReadiness(api, ext);
  BattleTeardown(api, ext);
  LegacySettingsSafety(api, ext);
  BattleRoleChanges(api,ext);
  Host host;
  auto h = host.Api();
  auto wrong = h;
  wrong.abi_version = 0;
  CHECK(!api->create(&wrong, "{}"));
  wrong = h;
  wrong.abi_hash = 0;
  CHECK(!api->create(&wrong, "{}"));
  wrong = h;
  wrong.struct_size = sizeof h - 1;
  CHECK(!api->create(&wrong, "{}"));
  wrong = h;
  wrong.title_id++;
  CHECK(!api->create(&wrong, "{}"));
  wrong = h;
  wrong.build_id[0] ^= 1;
  CHECK(!api->create(&wrong, "{}"));
  wrong = h;
  wrong.read_romfs = nullptr;
  CHECK(!api->create(&wrong, "{}"));
  wrong = h;
  wrong.read_memory = nullptr;
  CHECK(!api->create(&wrong, "{}"));
  void *instance = api->create(&h, "{}");
  CHECK(instance);
  if (!instance)
    return 1;
  Sample(api, instance, h);
  CHECK(host.integers["ot0.ready"] == 1);
  CHECK(host.integers["ot0.selected"] == 0);
  CHECK(host.integers["ot0.p0.on"] == 0 && host.integers["ot0.mp0.on"] == 1 && host.integers["ot0.mp2.on"] == 0);
  const auto first_key = host.texts["ot0.screen"];
  CHECK(!ext->on_action(instance, "attack", 0));
  CHECK(!ext->on_action(instance, "select", -1));
  CHECK(!ext->on_action(instance, "select", 8));
  CHECK(!ext->on_action(instance, "select", 2));
  CHECK(!ext->on_action(instance, nullptr, 0));
  const auto memory_before = host.memory;
  CHECK(ext->on_action(instance, "select", 1));
  Sample(api, instance, h);
  CHECK(host.integers["ot0.selected"] == 1);
  CHECK(host.memory == memory_before);
  const auto selected_key = host.texts["ot0.screen"];
  CHECK(first_key != selected_key);
  Image first, selected;
  const bool real = std::getenv("OT0_PAK") != nullptr;
  if (real) {
    CHECK(host.pak.is_open());
    CHECK(ext->load_image(instance, &h, first_key.c_str(), &first, Sink));
    Shape(first);
    CHECK(ext->load_image(instance, &h, selected_key.c_str(), &selected, Sink));
    Shape(selected);
    CelsusIllustration(selected);
    CHECK(first.rgba != selected.rgba);
    Dump(selected);
  }
  const auto calls_before = host.romfs_calls;
  Image bad;
  for (const auto &key :
       {std::string("module:ot0:screen/"), std::string("module:ot0:screen/-1"),
        std::string("module:ot0:screen/1junk"),
        std::string("module:ot0:screen/18446744073709551616"),
        std::string("module:other:screen/1"),
        std::string("module:ot0:screen/99999")})
    CHECK(!ext->load_image(instance, &h, key.c_str(), &bad, Sink));
  CHECK(host.romfs_calls == calls_before);
  CHECK(bad.calls == 0);
  host.Put(Ids, std::array<uint32_t, 8>{10503, 901, 0, 0, 0, 0, 0, 0});
  Sample(api, instance, h);
  CHECK(host.integers["ot0.selected"] ==
        0); // Celsus retained by identity after reorder.
  for (uint8_t state : {uint8_t(10), uint8_t(12), uint8_t(14), uint8_t(27)}) {
    host.Put(Root + 0x2f0, state);
    Sample(api, instance, h);
    CHECK(host.integers["ot0.ready"] == 0);
    CHECK(host.integers["ot0.state"] == state);
    CHECK(host.integers["ot0.selected"] == -1);
    for (int i = 0; i < 8; ++i)
      CHECK(host.integers["ot0.p" + std::to_string(i) + ".on"] == 0);
    CHECK(!ext->on_action(instance, "select", 0));
  }
  if (real) {
    Image old, title;
    CHECK(ext->load_image(instance, &h, first_key.c_str(), &old, Sink));
    CHECK(old.rgba == first.rgba);
    host.Put(Root + 0x2f0, uint8_t(10));
    Sample(api, instance, h);
    CHECK(ext->load_image(instance, &h, host.texts["ot0.screen"].c_str(),
                          &title, Sink));
    Shape(title);
    CHECK(title.rgba != first.rgba);
  }
  host.Put(Root + 0x2f0, uint8_t(23));
  Sample(api, instance, h);
  for (int i = 0; i < 40; ++i) {
    CHECK(ext->on_action(instance, "select", i % 2));
    Sample(api, instance, h);
  }
  const auto before_evicted = host.romfs_calls;
  CHECK(!ext->load_image(instance, &h, first_key.c_str(), &bad, Sink));
  CHECK(host.romfs_calls == before_evicted);
  // Missing assets must refuse, without invoking a pixel sink or affecting
  // memory.
  Host missing;
  missing.missing_pak = true;
  auto mh = missing.Api();
  void *mi = api->create(&mh, "{}");
  CHECK(mi);
  if (mi) {
    Sample(api, mi, mh);
    Image absent;
    CHECK(!ext->load_image(mi, &mh, missing.texts["ot0.screen"].c_str(),
                           &absent, Sink));
    CHECK(absent.calls == 0);
    Sample(api, mi, mh);
    CHECK(missing.integers["ot0.asset_error"] == 1);
    CHECK(missing.warnings == 1);
    if (real) {
      // A transient missing archive can recover using the same immutable key.
      missing.missing_pak = false;
      Image recovered;
      CHECK(ext->load_image(mi, &mh, missing.texts["ot0.screen"].c_str(),
                            &recovered, Sink));
      Shape(recovered);
      Sample(api, mi, mh);
      CHECK(missing.integers["ot0.asset_error"] == 0);
    }
    api->destroy(mi);
  }
  for (const auto &[key, value] : host.integers) {
    (void)value;
    CHECK(key.find(".hp") == std::string::npos &&
          key.find(".sp") == std::string::npos);
  }
  api->destroy(instance);
  std::fprintf(stderr, "Octopath ABI integration: %d failures%s\n", failures,
               real ? " (native art checked)"
                    : " (OT0_PAK unset; native art skipped)");
  return failures ? 1 : 0;
}
