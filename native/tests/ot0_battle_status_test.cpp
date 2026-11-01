// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_battle_status.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
namespace {
constexpr uint64_t Main = 0x80000000, App = 0x1400000000, Bm = 0x1400010000,
                   Actors = 0x1400020000, Actor = 0x1400030000,
                   Entries = 0x1400040000, Variants = 0x1400050000;
struct Memory {
  std::map<uint64_t, uint8_t> bytes;
  template <class T> void put(uint64_t p, const T &v) {
    auto b = reinterpret_cast<const uint8_t *>(&v);
    for (size_t i = 0; i < sizeof(T); ++i)
      bytes[p + i] = b[i];
  }
  bool read(uint64_t p, void *out, size_t n) const {
    auto b = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < n; ++i) {
      auto it = bytes.find(p + i);
      if (it == bytes.end())
        return false;
      b[i] = it->second;
    }
    return true;
  }
  void object(uint64_t p, uint64_t vt, int64_t delta) {
    put(p, Main + vt + uint64_t(delta));
    put(p + 8, uint32_t{8});
  }
};
Memory EnemyFixture(int64_t delta = 0) {
  Memory m;
  m.put(Main + 0x7cc8cc0, App);
  m.object(App, 0x6207610, delta);
  m.put(App + 0x2f0, uint8_t{26});
  m.put(Main + 0x7cc5b78, Bm);
  m.put(Bm + 8, uint32_t{8});
  m.put(Bm + 0xa98, Actors);
  m.put(Bm + 0xaa0, int32_t{1});
  m.put(Bm + 0xaa4, int32_t{1});
  m.put(Actors, Actor);
  m.object(Actor, 0x63f39b8, delta);
  m.put(Actor + 0x293, uint8_t{1});
  m.put(Actor + 0x298, uint32_t{32312});
  m.put(Actor + 0x540, int32_t{0});
  m.put(Actor + 0x548, int32_t{0});
  m.put(Actor + 0x5b8, std::array<uint8_t, 0x50>{});
  m.put(Actor + 0x5b8, Entries);
  m.put(Actor + 0x5c0, int32_t{1});
  m.put(Actor + 0x5c4, int32_t{1});
  m.put(Actor + 0x5f0, int32_t{0});
  m.put(Actor + 0x600, int32_t{1});
  m.put(Entries, std::array<uint8_t, 32>{});
  m.put(Entries, uint8_t{1});
  m.put(Entries + 8, Variants);
  m.put(Entries + 0x10, int32_t{1});
  m.put(Entries + 0x14, int32_t{1});
  m.put(Entries + 0x18, int32_t{-1});
  m.put(Variants, std::array<uint8_t, 12>{});
  m.put(Variants, uint8_t{1});
  m.put(Variants + 1, uint8_t{1});
  m.put(Variants + 8, int32_t{247});
  return m;
}
constexpr uint64_t Fronts = 0x1400060000, Player = 0x1400070000,
                   PlayerEntries = 0x1400080000, PlayerVariants = 0x1400090000;
Memory Fixture(int64_t delta = 0) {
  auto m = EnemyFixture(delta);
  m.put(Bm + 0x9b8, Fronts);
  m.put(Bm + 0x9c0, int32_t{1});
  m.put(Bm + 0x9c4, int32_t{1});
  m.put(Bm + 0x9c8, uint64_t{0});
  m.put(Bm + 0x9d0, int32_t{0});
  m.put(Bm + 0x9d4, int32_t{0});
  m.put(Fronts, Player);
  m.object(Player, 0x63f39b8, delta);
  m.put(Player + 0x293, uint8_t{0});
  m.put(Player + 0x2a8, uint8_t{1});
  m.put(Player + 0x298, uint32_t{904});
  m.put(Player + 0x5b8, std::array<uint8_t, 0x50>{});
  m.put(Player + 0x5b8, PlayerEntries);
  m.put(Player + 0x5c0, int32_t{8});
  m.put(Player + 0x5c4, int32_t{8});
  m.put(Player + 0x5f0, int32_t{7});
  m.put(Player + 0x600, int32_t{1});
  for (int i = 0; i < 8; ++i) {
    auto entry = PlayerEntries + uint64_t(i) * 32;
    auto variants = PlayerVariants + uint64_t(i) * 16;
    m.put(entry, std::array<uint8_t, 32>{});
    m.put(entry, uint8_t(9 + i));
    m.put(entry + 8, variants);
    m.put(entry + 0x10, int32_t{1});
    m.put(entry + 0x14, int32_t{1});
    m.put(entry + 0x18, int32_t{i - 1});
    m.put(variants, std::array<uint8_t, 12>{});
    m.put(variants, uint8_t{1});
    m.put(variants + 1, uint8_t{1});
    m.put(variants + 8, int32_t(100 + i));
  }
  m.put(Entries, uint8_t{13});
  m.put(Variants + 8, int32_t{73});
  return m;
}
constexpr uint64_t Raw = 0x14000a0000, Views = 0x14000b0000,
                   Texture = 0x14000c0000, Pool = 0x14000d0000,
                   TextureClass = 0x14000e0000, Package = 0x14000f0000;
Memory StatusFixture(int64_t delta = 0) {
  auto m = Fixture(delta);
  m.put(Player + 0x800, std::array<uint8_t, 16>{});
  m.put(Player + 0x858, std::array<uint8_t, 16>{});
  m.put(Actor + 0x800, Raw);
  m.put(Actor + 0x808, int32_t{2});
  m.put(Actor + 0x80c, int32_t{2});
  m.put(Actor + 0x858, Views);
  m.put(Actor + 0x860, int32_t{2});
  m.put(Actor + 0x864, int32_t{2});
  for (int i = 0; i < 2; ++i) {
    auto row = Raw + uint64_t(i) * 0xe8;
    auto view = Views + uint64_t(i) * 24;
    m.put(row, std::array<uint8_t, 0xe8>{});
    m.put(row, uint32_t(i ? 129 : 21));
    m.put(row + 6, int16_t{3});
    m.put(view, std::array<uint8_t, 24>{});
    m.put(view, uint32_t(i ? 94004 : 62347));
    m.put(view + 8, Texture + uint64_t(i) * 0x1000);
    m.put(view + 16, int32_t{3});
  }
  return m;
}
ot0::BattleMetadata Metadata() {
  ot0::BattleMetadata m{};
  m.valid = true;
  m.ui_texts.valid = true;
  m.ui_texts.names[62347] = "Provoked";
  m.ui_texts.names[94004] = "Accuracy Down";
  for (auto [id, text] : std::array<std::pair<uint32_t, uint32_t>, 2>{
           {{21, 62347}, {129, 94004}}}) {
    ot0::BattleAilmentInfo a{};
    a.id = id;
    a.text_id = text;
    a.name = m.ui_texts.names[text];
    a.negative = true;
    m.ailments.emplace(id, a);
  }
  return m;
}
void Name(Memory &m, uint32_t index, const std::string &s) {
  m.put(Pool + 2 * index, uint16_t(s.size() << 6));
  for (size_t i = 0; i < s.size(); ++i)
    m.put(Pool + 2 * index + 2 + i, uint8_t(s[i]));
}
void TextureAsset(Memory &m) {
  m.put(Main + 0x6c91440 + 0x38, Pool);
  m.put(Texture, std::array<uint8_t, 0x28>{});
  m.put(TextureClass, std::array<uint8_t, 0x28>{});
  m.put(Package, std::array<uint8_t, 0x28>{});
  m.put(Texture + 0x10, TextureClass);
  m.put(Texture + 0x18, uint32_t{64});
  m.put(Texture + 0x20, Package);
  m.put(TextureClass + 0x18, uint32_t{32});
  m.put(Package + 0x18, uint32_t{128});
  Name(m, 32, "Texture2D");
  Name(m, 64, "T_Icon_Provoked");
  Name(m, 128, "/Game/UI/Battle/T_Icon_Provoked");
}
std::vector<uint8_t> ReadFile(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  assert(f);
  return {std::istreambuf_iterator<char>(f), {}};
}
} // namespace
int main() {
  ot0::BattleSnapshot b{};
  b.valid = true;
  b.member_count = 1;
  b.members[0].id = 904;
  b.enemy_count = 1;
  b.enemies[0].enemy_id = 32312;
  auto metadata = Metadata();
  const auto run = [&](Memory &m, int64_t delta = 0) {
    return ot0::ReadBattleStatuses(
        [&](auto p, auto out, auto n) { return m.read(p, out, n); }, Main, b,
        metadata, delta);
  };
  auto m = StatusFixture();
  auto good = run(m);
  assert(good.valid && good.party[0].valid && good.party[0].entries.empty());
  assert(good.enemies[0].valid && good.enemies[0].entries.size() == 2 &&
         good.enemies[0].unknown_count == 0);
  auto first = good.enemies[0].entries[0];
  assert(first.type_id == 21 && first.name == "Provoked" &&
         first.counter == 3 && first.counter_valid && first.negative &&
         first.classification_valid && first.texture_path.empty());
  m.put(Raw + 6, int16_t{2});
  auto lag = run(m);
  assert(lag.valid && lag.enemies[0].entries.size() == 2 &&
         !lag.enemies[0]
              .entries[0]
              .counter_valid); // Native UI cache from a preceding turn cannot
                               // publish its old count.
  m.put(Views + 16, int32_t{2});
  assert(run(m).enemies[0].entries[0].counter_valid);
  m = StatusFixture();
  m.put(Raw + 0xe0, uint8_t{1});
  auto inactive = run(m);
  assert(inactive.enemies[0].entries.size() == 1 &&
         inactive.enemies[0].unknown_count == 1);
  m = StatusFixture();
  m.put(Views, uint32_t{0});
  m.put(Views + 21, uint8_t{1});
  auto support = run(m);
  assert(support.enemies[0].entries.size() == 1 &&
         support.enemies[0].unknown_count ==
             1); // No fabricated passive caption.
  m = StatusFixture();
  m.put(Views + 20, uint8_t{1});
  auto countmode = run(m);
  assert(countmode.enemies[0].entries[0].counter_mode == 1 &&
         !countmode.enemies[0]
              .entries[0]
              .counter_valid); // Stack/charge semantics remain unguessed.
  m = StatusFixture();
  m.put(Actor + 0x860, int32_t{0});
  auto neverbuilt = run(m);
  assert(neverbuilt.valid && !neverbuilt.enemies[0].valid &&
         neverbuilt.enemies[0].unknown_count == 2);
  m = StatusFixture();
  m.put(Actor + 0x808, int32_t{129});
  assert(!run(m).enemies[0].valid);
  m = StatusFixture();
  int changes = 0;
  auto torn = ot0::ReadBattleStatuses(
      [&](auto p, auto out, auto n) {
        if (p == Raw && ++changes == 2)
          m.put(Raw + 6, int16_t{2});
        return m.read(p, out, n);
      },
      Main, b, metadata);
  assert(torn.valid && !torn.enemies[0].valid);
  m = StatusFixture();
  m.put(Actor + 0x298, uint32_t{999});
  assert(!run(m).valid);
  m = StatusFixture();
  TextureAsset(m);
  assert(run(m).enemies[0].entries[0].texture_path ==
         "/Game/UI/Battle/T_Icon_Provoked.T_Icon_Provoked");
  Name(m, 32, "PaperSprite");
  assert(run(m).enemies[0].entries[0].asset_class == "PaperSprite" &&
         !run(m).enemies[0].entries[0].texture_path.empty());
  m.put(Texture + 8, uint32_t{0x40000000});
  assert(run(m).enemies[0].entries[0].texture_path.empty());
  constexpr int64_t delta = 0x1234000;
  m = StatusFixture(delta);
  assert(run(m, delta).valid && !run(m).valid);
  if (const char *path = std::getenv("OT0_BATTLE_STATUS_MEMORY")) {
    const char *dir = std::getenv("OT0_BATTLE_METADATA_DIR");
    assert(dir);
    std::string base = dir;
    auto db = ReadFile(base + "/Character/CharaStatusID.uexp"),
         ail = ReadFile(base + "/Skill/SkillAilmentType.uexp"),
         ui = ReadFile(base +
                       "/GameText/Localize/EN-US/SystemText/GameTextUI.uexp"),
         skill = ReadFile(
             base + "/GameText/Localize/EN-US/SystemText/GameTextSkill.uexp");
    auto meta = ot0::DecodeBattleMetadata(db, ail, ui, skill);
    assert(meta.valid);
    std::ifstream f(path, std::ios::binary);
    assert(f);
    auto get = [&]<class T>(T &v) {
      f.read(reinterpret_cast<char *>(&v), sizeof v);
      assert(f);
    };
    char magic[8];
    f.read(magic, 8);
    assert(!std::memcmp(magic, "OT0BST01", 8));
    uint64_t main{};
    uint32_t members{}, enemies{}, segments{};
    get(main);
    get(members);
    get(enemies);
    assert(members > 0 && members <= 8 && enemies > 0 && enemies <= 6);
    ot0::BattleSnapshot live{};
    live.valid = true;
    live.member_count = int(members);
    live.enemy_count = int(enemies);
    for (unsigned i = 0; i < members; ++i)
      get(live.members[i].id);
    for (unsigned i = 0; i < enemies; ++i)
      get(live.enemies[i].enemy_id);
    get(segments);
    assert(segments < 16384);
    Memory captured;
    for (unsigned i = 0; i < segments; ++i) {
      uint64_t p{};
      uint32_t n{};
      get(p);
      get(n);
      assert(n <= 65536);
      for (unsigned j = 0; j < n; ++j) {
        uint8_t v{};
        get(v);
        captured.put(p + j, v);
      }
    }
    auto result = ot0::ReadBattleStatuses(
        [&](auto p, auto out, auto n) { return captured.read(p, out, n); },
        main, live, meta);
    assert(result.valid);
    int expected = 3;
    if (const char *value = std::getenv("OT0_BATTLE_STATUS_COUNTER"))
      expected = std::atoi(value);
    if (expected == 2) {
      bool found = false;
      for (unsigned i = 0; i < members; ++i)
        if (live.members[i].id == 10905) {
          const auto &statuses = result.party[i];
          assert(statuses.valid && statuses.entries.size() == 1);
          const auto &buff = statuses.entries[0];
          assert(buff.type_id == 107 && buff.name == "Phys. Def. Up" &&
                 buff.counter == 2 && buff.counter_valid && buff.positive &&
                 buff.asset_class == "PaperSprite" &&
                 buff.texture_path.find(
                     "UiTX_Battle_IconStatus_Up_PDefense_png") !=
                     std::string::npos);
          found = true;
        }
      assert(found);
    }
    for (unsigned i = 0; i < enemies; ++i) {
      assert(result.enemies[i].valid && result.enemies[i].entries.size() == 2);
      const auto &a = result.enemies[i].entries[0];
      const auto &z = result.enemies[i].entries[1];
      if (expected == 2)
        assert(a.asset_class == "PaperSprite" &&
               a.texture_path.find("UiTX_Battle_IconStatus_Down_Provoke_png") !=
                   std::string::npos &&
               z.texture_path.find("UiTX_Battle_IconStatus_Down_FireUp_png") !=
                   std::string::npos);
      assert(a.type_id == 21 && a.name == "Provoked" && a.counter_valid &&
             a.counter == expected && z.type_id == 129 &&
             z.name == "Accuracy Down" && z.counter_valid &&
             z.counter == expected);
    }
  }
}
