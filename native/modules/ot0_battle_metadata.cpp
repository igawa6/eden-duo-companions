// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ot0_battle_metadata.h"
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
namespace ot0 {
namespace {
struct Node {
  enum Kind { Nil, Number, String, Array, Map, Boolean, Other } kind = Nil;
  uint64_t number{};
  std::string str;
  std::vector<Node> list;
  std::unordered_map<std::string, Node> map;
};
[[noreturn]] void Bad() { throw std::runtime_error("Invalid native database"); }
bool Utf8(std::span<const uint8_t> s) {
  for (size_t i = 0; i < s.size();) {
    uint32_t c = s[i++];
    unsigned n = 0;
    uint32_t min = 0;
    if (c < 128) {
      if (!c)
        return false;
      continue;
    }
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      c &= 31;
      min = 128;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      c &= 15;
      min = 2048;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      c &= 7;
      min = 65536;
    } else
      return false;
    if (n > s.size() - i)
      return false;
    while (n--) {
      uint8_t b = s[i++];
      if ((b & 0xc0) != 0x80)
        return false;
      c = (c << 6) | (b & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return false;
  }
  return true;
}
class Parser {
  std::span<const uint8_t> bytes;
  size_t pos = 0, nodes = 0;
  std::span<const uint8_t> take(size_t n) {
    if (n > bytes.size() - pos)
      Bad();
    auto b = bytes.subspan(pos, n);
    pos += n;
    return b;
  }
  uint64_t integer(unsigned n) {
    uint64_t v = 0;
    for (auto b : take(n))
      v = (v << 8) | b;
    return v;
  }
  std::string string(size_t n) {
    if (n > 100000)
      Bad();
    auto b = take(n);
    if (!Utf8(b))
      Bad();
    return std::string(reinterpret_cast<const char *>(b.data()), b.size());
  }
  Node sequence(size_t n, unsigned depth, bool map) {
    if (n > 200000)
      Bad();
    Node r;
    r.kind = map ? Node::Map : Node::Array;
    if (map) {
      for (size_t i = 0; i < n; ++i) {
        Node key = parse(depth + 1);
        if (key.kind != Node::String)
          Bad();
        auto value = parse(depth + 1);
        if (!r.map.emplace(std::move(key.str), std::move(value)).second)
          Bad();
      }
    } else {
      r.list.reserve(n);
      for (size_t i = 0; i < n; ++i)
        r.list.push_back(parse(depth + 1));
    }
    return r;
  }
  Node parse(unsigned depth) {
    if (depth > 16 || ++nodes > 1000000)
      Bad();
    uint8_t t = uint8_t(integer(1));
    Node r;
    if (t <= 0x7f) {
      r.kind = Node::Number;
      r.number = t;
      return r;
    }
    if (t >= 0xe0) {
      r.kind = Node::Other;
      return r;
    }
    if ((t & 0xe0) == 0xa0) {
      r.kind = Node::String;
      r.str = string(t & 31);
      return r;
    }
    if ((t & 0xf0) == 0x90)
      return sequence(t & 15, depth, false);
    if ((t & 0xf0) == 0x80)
      return sequence(t & 15, depth, true);
    if (t == 0xc0)
      return r;
    if (t == 0xc2 || t == 0xc3) {
      r.kind = Node::Boolean;
      r.number = t == 0xc3;
      return r;
    }
    if (t >= 0xcc && t <= 0xcf) {
      r.kind = Node::Number;
      r.number = integer(1u << (t - 0xcc));
      return r;
    }
    if (t >= 0xd0 && t <= 0xd3) {
      unsigned n = 1u << (t - 0xd0);
      auto v = integer(n);
      r.kind = (v & (uint64_t(1) << (n * 8 - 1))) ? Node::Other : Node::Number;
      r.number = v;
      return r;
    }
    if (t == 0xca || t == 0xcb) {
      take(t == 0xca ? 4 : 8);
      r.kind = Node::Other;
      return r;
    }
    if (t >= 0xd9 && t <= 0xdb) {
      r.kind = Node::String;
      r.str = string(integer(1u << (t - 0xd9)));
      return r;
    }
    if (t == 0xdc || t == 0xdd)
      return sequence(integer(t == 0xdc ? 2 : 4), depth, false);
    if (t == 0xde || t == 0xdf)
      return sequence(integer(t == 0xde ? 2 : 4), depth, true);
    Bad();
  }

public:
  explicit Parser(std::span<const uint8_t> b) : bytes(b) {}
  Node root() {
    auto r = parse(0);
    if (pos != bytes.size())
      Bad();
    return r;
  }
};
Node Decode(std::span<const uint8_t> b) {
  constexpr std::array<uint8_t, 6> head{0, 0, 0, 0, 0, 3};
  constexpr std::array<uint8_t, 8> tail{0, 0, 0, 0, 0xc1, 0x83, 0x2a, 0x9e};
  if (b.size() < 18 || b.size() > 20000000 ||
      std::memcmp(b.data(), head.data(), 6))
    Bad();
  uint32_t n = uint32_t(b[6]) | uint32_t(b[7]) << 8 | uint32_t(b[8]) << 16 |
               uint32_t(b[9]) << 24;
  if (n != b.size() - 18 || std::memcmp(b.data() + 10 + n, tail.data(), 8))
    Bad();
  return Parser(b.subspan(10, n)).root();
}
const Node &Field(const Node &r, const char *key, Node::Kind kind) {
  if (r.kind != Node::Map)
    Bad();
  auto it = r.map.find(key);
  if (it == r.map.end() || it->second.kind != kind)
    Bad();
  return it->second;
}
uint32_t Id(const Node &r, const char *key) {
  auto n = Field(r, key, Node::Number).number;
  if (n > UINT32_MAX)
    Bad();
  return uint32_t(n);
}
bool Flag(const Node &row, const char *key) {
  return Field(row, key, Node::Boolean).number != 0;
}
MapNames Texts(std::span<const uint8_t> bytes) {
  const auto root = Decode(bytes);
  const auto &rows = Field(root, "m_DataList", Node::Array).list;
  if (rows.empty() || rows.size() > 20000)
    Bad();
  MapNames out;
  for (const auto &row : rows) {
    const auto id = Id(row, "m_id");
    const auto &values = Field(row, "m_gametext", Node::Array).list;
    if (values.empty() || values.size() > 32)
      Bad();
    for (const auto &value : values)
      if (value.kind != Node::String)
        Bad();
    if (!out.names.emplace(id, values.front().str).second)
      Bad();
  }
  out.valid = true;
  return out;
}
} // namespace
const BattleAilmentInfo *BattleMetadata::FindAilment(uint32_t id) const {
  auto it = ailments.find(id);
  return valid && it != ailments.end() && !it->second.name.empty() ? &it->second
                                                                   : nullptr;
}
BattleMetadata DecodeBattleMetadata(std::span<const uint8_t> chara_status,
                                    std::span<const uint8_t> ailment_types,
                                    std::span<const uint8_t> english_ui,
                                    std::span<const uint8_t> english_skills) {
  try {
    BattleMetadata out;
    out.ui_texts = Texts(english_ui);
    out.skill_texts = Texts(english_skills);
    const auto status_root = Decode(chara_status);
    const auto &status = Field(status_root, "m_DataList", Node::Array).list;
    if (status.size() < 8 || status.size() > 256)
      Bad();
    std::array<bool, 256> seen{};
    for (const auto &row : status) {
      const auto id = Id(row, "m_id");
      if (id >= 256 || seen[id])
        Bad();
      seen[id] = true;
      if (id < 9 || id > 16)
        continue;
      CombatParameterInfo p{id,
                            Id(row, "m_NameID"),
                            Id(row, "m_IconID"),
                            Id(row, "m_FrameIconID"),
                            {}};
      auto text = out.ui_texts.names.find(p.text_id);
      if (!p.text_id || !p.icon_id || text == out.ui_texts.names.end() ||
          text->second.empty())
        Bad();
      p.name = text->second;
      out.parameters[id - 9] = std::move(p);
    }
    for (const auto &p : out.parameters)
      if (!p.id)
        Bad();
    const auto ailment_root = Decode(ailment_types);
    const auto &ailments = Field(ailment_root, "m_DataList", Node::Array).list;
    if (ailments.empty() || ailments.size() > 4096)
      Bad();
    for (const auto &row : ailments) {
      BattleAilmentInfo a;
      a.id = Id(row, "m_id");
      a.text_id = Id(row, "m_TextID");
      a.explanation_id = Id(row, "m_ExplanationTextID");
      a.icon_id = Id(row, "m_IconTexID");
      a.support_icon_id = Id(row, "m_IconSptTexID");
      a.positive = Flag(row, "m_Plus");
      a.negative = Flag(row, "m_Minus");
      a.no_count = Flag(row, "m_NoCount");
      a.count_down = Flag(row, "m_CountDown");
      a.multiply = Flag(row, "m_Multiply");
      if (a.id > 65535)
        Bad();
      if (a.text_id) {
        auto name = out.ui_texts.names.find(a.text_id);
        if (name != out.ui_texts.names.end())
          a.name = name->second;
      }
      if (a.explanation_id) {
        auto text = out.ui_texts.names.find(a.explanation_id);
        if (text != out.ui_texts.names.end())
          a.explanation = text->second;
      }
      if (!out.ailments.emplace(a.id, std::move(a)).second)
        Bad();
    }
    out.valid = true;
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
} // namespace ot0
