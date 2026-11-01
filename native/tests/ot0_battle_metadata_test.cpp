// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../modules/ot0_battle_metadata.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
using Bytes = std::vector<uint8_t>;
namespace {
Bytes Number(uint32_t n) {
  return {0xce, uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8),
          uint8_t(n)};
}
Bytes String(const std::string &s) {
  assert(s.size() < 256);
  Bytes b{0xd9, uint8_t(s.size())};
  b.insert(b.end(), s.begin(), s.end());
  return b;
}
Bytes Flag(bool v) { return {uint8_t(v ? 0xc3 : 0xc2)}; }
Bytes Array(const std::vector<Bytes> &a) {
  assert(a.size() < 65536);
  Bytes b{0xdc, uint8_t(a.size() >> 8), uint8_t(a.size())};
  for (auto &v : a)
    b.insert(b.end(), v.begin(), v.end());
  return b;
}
Bytes Map(const std::vector<std::pair<std::string, Bytes>> &m) {
  assert(m.size() < 16);
  Bytes b{uint8_t(0x80 | m.size())};
  for (auto &[k, v] : m) {
    auto s = String(k);
    b.insert(b.end(), s.begin(), s.end());
    b.insert(b.end(), v.begin(), v.end());
  }
  return b;
}
Bytes Table(const std::vector<Bytes> &rows) {
  auto body = Map({{"m_DataList", Array(rows)}});
  Bytes b{0, 0, 0, 0, 0, 3};
  uint32_t n = uint32_t(body.size());
  for (unsigned i = 0; i < 4; ++i)
    b.push_back(uint8_t(n >> (i * 8)));
  b.insert(b.end(), body.begin(), body.end());
  b.insert(b.end(), {0, 0, 0, 0, 0xc1, 0x83, 0x2a, 0x9e});
  return b;
}
Bytes Text(uint32_t id, std::string text) {
  return Map({{"m_id", Number(id)}, {"m_gametext", Array({String(text)})}});
}
Bytes Ailment(uint32_t id, bool badbool = false) {
  return Map({{"m_id", Number(id)},
              {"m_TextID", Number(2000)},
              {"m_ExplanationTextID", Number(2001)},
              {"m_IconTexID", Number(123)},
              {"m_IconSptTexID", Number(124)},
              {"m_Plus", badbool ? Number(1) : Flag(true)},
              {"m_Minus", Flag(false)},
              {"m_NoCount", Flag(false)},
              {"m_CountDown", Flag(false)},
              {"m_Multiply", Flag(true)}});
}
struct Fixture {
  std::vector<Bytes> stats, texts;
  Bytes skills = Table({Text(0, "None"), Text(1, "Ability")});
  Fixture() {
    texts.push_back(Text(0, "No data"));
    for (uint32_t i = 9; i <= 16; ++i) {
      stats.push_back(Map({{"m_id", Number(i)},
                           {"m_NameID", Number(1000 + i)},
                           {"m_IconID", Number(100 + i)},
                           {"m_FrameIconID", Number(200 + i)}}));
      texts.push_back(Text(1000 + i, "Stat " + std::to_string(i)));
    }
    texts.push_back(Text(2000, "Buff"));
    texts.push_back(Text(2001, "Improves a stat"));
  }
  ot0::BattleMetadata Decode(std::vector<Bytes> ailments = {Ailment(106)}) {
    auto a = Table(stats), b = Table(ailments), c = Table(texts);
    return ot0::DecodeBattleMetadata(a, b, c, skills);
  }
};
Bytes Read(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  assert(f);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}
} // namespace
int main() {
  Fixture f;
  auto good = f.Decode();
  assert(good.valid && good.parameters[0].id == 9 &&
         good.parameters[7].name == "Stat 16");
  auto buff = good.FindAilment(106);
  assert(buff && buff->positive && !buff->negative && buff->multiply &&
         buff->icon_id == 123 && buff->support_icon_id == 124 &&
         buff->explanation == "Improves a stat");
  assert(!good.FindAilment(999));
  assert(!f.Decode({Ailment(106), Ailment(106)}).valid);
  assert(!f.Decode({Ailment(106, true)}).valid);
  f.stats.pop_back();
  assert(!f.Decode().valid);
  f = Fixture();
  f.stats.push_back(f.stats.front());
  assert(!f.Decode().valid);
  f = Fixture();
  f.texts.erase(f.texts.begin() + 1);
  assert(!f.Decode().valid);
  f = Fixture();
  f.texts.pop_back();
  f.texts.pop_back();
  auto unnamed = f.Decode();
  assert(
      unnamed.valid &&
      !unnamed.FindAilment(
          106)); // Missing localization is unknown, not an invented enum label.
  f = Fixture();
  auto a = Table(f.stats), b = Table({Ailment(106)}), c = Table(f.texts);
  a.back() ^= 1;
  assert(!ot0::DecodeBattleMetadata(a, b, c, f.skills).valid);
  if (const char *dir = std::getenv("OT0_BATTLE_METADATA_DIR")) {
    std::string base = dir;
    auto s = Read(base + "/Character/CharaStatusID.uexp"),
         a = Read(base + "/Skill/SkillAilmentType.uexp"),
         u = Read(base + "/GameText/Localize/EN-US/SystemText/GameTextUI.uexp"),
         k = Read(base +
                  "/GameText/Localize/EN-US/SystemText/GameTextSkill.uexp");
    auto d = ot0::DecodeBattleMetadata(s, a, u, k);
    assert(d.valid && d.parameters[0].name == "Phys. Atk." &&
           d.parameters[1].name == "Phys. Def." &&
           d.parameters[2].name == "Elem. Atk." &&
           d.parameters[3].name == "Elem. Def." &&
           d.parameters[6].name == "Accuracy" &&
           d.parameters[7].name == "Evasion");
    assert(d.ailments.size() == 490);
    auto buff = d.FindAilment(106);
    assert(buff && buff->name == "Phys. Atk. Up" && buff->icon_id == 4085 &&
           buff->positive);
    assert(!d.FindAilment(28));
  }
}
