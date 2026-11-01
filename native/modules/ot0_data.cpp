// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ot0_data.h"
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
namespace ot0 {
namespace {
struct Node {
  enum Kind { Nil, Number, String, Array, Map, Other } kind = Nil;
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
      r.kind = Node::Other; // booleans: value kept in number (1 = true)
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
uint32_t Alias(uint32_t id) { return id >= 900 && id <= 909 ? 900 : id; }
} // namespace
MapNames DecodeMapNames(std::span<const uint8_t> english) {
  try {
    const auto root = Decode(english);
    const auto &rows = Field(root, "m_DataList", Node::Array).list;
    if (rows.empty() || rows.size() > 20000)
      Bad();
    MapNames out;
    for (const auto &row : rows) {
      const auto id = Id(row, "m_id");
      const auto &values = Field(row, "m_gametext", Node::Array).list;
      if (!id || values.empty() || values.size() > 32)
        Bad();
      for (const auto &value : values)
        if (value.kind != Node::String)
          Bad();
      if (!out.names.emplace(id, values.front().str).second)
        Bad();
    }
    out.valid = true;
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
std::string MapNames::LookupName(uint32_t id) const {
  if (!valid || !id)
    return {};
  const auto found = names.find(id);
  if (found == names.end() || found->second.find('<') != std::string::npos)
    return {};
  return found->second;
}
Names DecodeNames(std::span<const uint8_t> players,
                  std::span<const uint8_t> english) {
  try {
    auto p = Decode(players), e = Decode(english);
    const auto &ps = Field(p, "m_DataList", Node::Array).list;
    const auto &es = Field(e, "m_DataList", Node::Array).list;
    if (ps.empty() || ps.size() > 20000 || es.empty() || es.size() > 20000)
      Bad();
    std::unordered_map<uint32_t, std::string> text;
    for (const auto &row : es) {
      auto id = Id(row, "m_id");
      const auto &values = Field(row, "m_gametext", Node::Array).list;
      if (values.empty() || values.size() > 32 ||
          values.front().kind != Node::String)
        Bad();
      for (const auto &v : values)
        if (v.kind != Node::String)
          Bad();
      if (!text.emplace(id, values.front().str).second)
        Bad();
    }
    Names out;
    for (const auto &row : ps) {
      auto id = Id(row, "m_id"), name = Id(row, "m_Name"),
           sprite = Id(row, "m_CharaID");
      if (!out.sprite_ids.emplace(id, sprite).second)
        Bad();
      auto t = text.find(name);
      if (t != text.end() && !t->second.empty() &&
          t->second.find('<') == std::string::npos && id != 900)
        out.names.emplace(id, t->second);
    }
    out.valid = true;
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
namespace {
uint32_t WeakBits(const Node &list, unsigned slots) {
  if (list.kind != Node::Array || list.list.size() < slots || list.list.size() > 16)
    Bad();
  uint32_t m = 0;
  for (unsigned i = 1; i < slots; ++i)
    if (list.list[i].kind == Node::Number && list.list[i].number == 1)
      m |= 1u << i;
  return m;
}
std::unordered_map<uint32_t, const Node *> Rows(const Node &root) {
  const auto &rows = Field(root, "m_DataList", Node::Array).list;
  if (rows.empty() || rows.size() > 20000)
    Bad();
  std::unordered_map<uint32_t, const Node *> out;
  for (const auto &row : rows)
    if (!out.emplace(Id(row, "m_id"), &row).second)
      Bad();
  return out;
}
} // namespace
Enemies DecodeEnemies(std::span<const uint8_t> enemy_id, std::span<const uint8_t> type_id,
                      std::span<const uint8_t> anim_set, std::span<const uint8_t> tex_id,
                      std::span<const uint8_t> weak_id, std::span<const uint8_t> english) {
  try {
    const auto E = Decode(enemy_id), T = Decode(type_id), A = Decode(anim_set), X = Decode(tex_id),
               K = Decode(weak_id), G = Decode(english);
    const auto types = Rows(T), anims = Rows(A), texs = Rows(X), weaks = Rows(K), texts = Rows(G);
    Enemies out;
    for (const auto &row : Field(E, "m_DataList", Node::Array).list) {
      EnemyInfo info;
      const auto text = texts.find(Id(row, "m_DisplayName"));
      if (text != texts.end()) {
        const auto &v = Field(*text->second, "m_gametext", Node::Array).list;
        if (!v.empty() && v.front().kind == Node::String && v.front().str.find('<') == std::string::npos)
          info.name = v.front().str;
      }
      if (auto it = row.map.find("m_DisplayLevel"); it != row.map.end() && it->second.kind == Node::String &&
                                                     it->second.str.size() <= 4)
        info.level = it->second.str;
      info.max_shields = Id(row, "m_MaxSLD");
      if (info.max_shields > 99)
        Bad();
      if (const auto w = weaks.find(Id(row, "m_WeakID")); w != weaks.end()) {
        info.weapon_weak = WeakBits(Field(*w->second, "m_ResistWeapon", Node::Array), 9) & 0x1fe;
        info.magic_weak = WeakBits(Field(*w->second, "m_ResistMagic", Node::Array), 7) & 0x7e;
      }
      if (const auto ty = types.find(Id(row, "m_TypeID")); ty != types.end()) {
        const auto &sets = Field(*ty->second, "m_AnimationSetIDs", Node::Array).list;
        if (!sets.empty() && sets.front().kind == Node::Number)
          if (const auto an = anims.find(uint32_t(sets.front().number)); an != anims.end()) {
            const auto &ids = Field(*an->second, "m_EnemyTextureIDs", Node::Array).list;
            if (!ids.empty() && ids.front().kind == Node::Number)
              if (const auto tx = texs.find(uint32_t(ids.front().number)); tx != texs.end()) {
                // /Game/.../Enm/Emd_X.Emd_X -> /Game/.../Enm/Textures/Emd_X_cl.Emd_X_cl
                const auto &path = Field(*tx->second, "m_FilePath", Node::String).str;
                const auto slash = path.rfind('/'), dot = path.rfind('.');
                if (slash != std::string::npos && dot != std::string::npos && dot > slash) {
                  const auto stem = path.substr(slash + 1, dot - slash - 1) + "_cl";
                  info.texture = path.substr(0, slash) + "/Textures/" + stem + "." + stem;
                }
              }
          }
      }
      out.rows.emplace(Id(row, "m_id"), std::move(info));
    }
    out.valid = !out.rows.empty();
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
WeaponTypes DecodeWeaponTypes(std::span<const uint8_t> item_list) {
  try {
    const auto root = Decode(item_list);
    WeaponTypes out;
    for (const auto &row : Field(root, "m_DataList", Node::Array).list) {
      const auto type = Id(row, "m_WeaponTypeID");
      if (type > 8) Bad();
      if (type) out.types.emplace(Id(row, "m_id"), type);
    }
    out.valid = !out.types.empty();
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
MapRegions DecodeMapRegions(std::span<const uint8_t> map_list, std::span<const uint8_t> texture_id) {
  try {
    const auto maps = Decode(map_list), textures = Decode(texture_id);
    std::unordered_map<uint32_t, std::string> stems;
    for (const auto &row : Field(textures, "m_DataList", Node::Array).list) {
      const auto it = row.map.find("m_FilePath");
      if (it == row.map.end() || it->second.kind != Node::String) continue;
      const auto &path = it->second.str;
      const auto slash = path.rfind('/'), dot = path.rfind('.');
      if (slash != std::string::npos && dot != std::string::npos && dot > slash)
        stems.emplace(Id(row, "m_id"), path.substr(slash + 1, dot - slash - 1));
    }
    MapRegions out;
    for (const auto &row : Field(maps, "m_DataList", Node::Array).list) {
      MapRegion region;
      region.region_text = Id(row, "m_RegionText");
      if (const auto s = stems.find(Id(row, "m_RegionIconTexID")); s != stems.end()) region.emblem = s->second;
      out.maps.emplace(Id(row, "m_id"), std::move(region));
    }
    out.valid = !out.maps.empty() && !stems.empty();
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
const MapRegion *MapRegions::Find(uint32_t map_id) const {
  if (!valid) return nullptr;
  const auto it = maps.find(map_id);
  return it == maps.end() ? nullptr : &it->second;
}
Quests DecodeQuests(std::span<const uint8_t> quest_list, std::span<const uint8_t> english_quest) {
  try {
    const auto root = Decode(quest_list), text = Decode(english_quest);
    std::unordered_map<uint32_t, std::string> names;
    for (const auto &row : Field(text, "m_DataList", Node::Array).list) {
      const auto &v = Field(row, "m_gametext", Node::Array).list;
      if (!v.empty() && v.front().kind == Node::String && v.front().str.find('<') == std::string::npos)
        names.emplace(Id(row, "m_id"), v.front().str);
    }
    const auto lookup = [&](uint32_t id) {
      const auto n = names.find(id);
      return n == names.end() ? std::string{} : n->second;
    };
    const auto flag = [](const Node &row, const char *key) {
      const auto it = row.map.find(key);
      return it != row.map.end() && it->second.kind == Node::Other && it->second.number == 1;
    };
    Quests out;
    for (const auto &row : Field(root, "m_DataList", Node::Array).list) {
      QuestInfo info;
      info.title = lookup(Id(row, "m_Title"));
      info.subtitle = lookup(Id(row, "m_SubTitle"));
      // Native rows read "Ch. N: <subtitle>" with N = m_Chapter - 1 (Master of Fame m_Chapter 2
      // = "Ch. 1"), unless the chapter is hidden.
      if (const auto chapter = Id(row, "m_Chapter"); chapter > 1 && !flag(row, "m_HideChapter") && !info.subtitle.empty())
        info.subtitle = "Ch. " + std::to_string(chapter - 1) + ": " + info.subtitle;
      info.category = Id(row, "m_QuestCategory");
      info.sort = Id(row, "m_SortPriority");
      const auto &levels = Field(row, "m_RecommLv", Node::Array).list;
      if (!levels.empty() && levels.front().kind == Node::Number) info.level = uint32_t(levels.front().number);
      info.hide_info = flag(row, "m_HideInfoFlag");
      info.hide_list = flag(row, "m_HideQuestList");
      out.rows.emplace(Id(row, "m_id"), std::move(info));
    }
    out.valid = !out.rows.empty() && !names.empty();
    out.texts = std::move(names);
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
const QuestInfo *Quests::Find(uint32_t quest) const {
  if (!valid) return nullptr;
  const auto it = rows.find(quest);
  return it == rows.end() ? nullptr : &it->second;
}
Items DecodeItems(std::span<const uint8_t> item_list, std::span<const uint8_t> item_type,
                  std::span<const uint8_t> texture_id, std::span<const uint8_t> english_item) {
  try {
    const auto root = Decode(item_list), text = Decode(english_item), types = Decode(item_type),
               textures = Decode(texture_id);
    std::unordered_map<uint32_t, std::string> stems;
    for (const auto &row : Field(textures, "m_DataList", Node::Array).list) {
      const auto it = row.map.find("m_FilePath");
      if (it == row.map.end() || it->second.kind != Node::String) continue;
      const auto &path = it->second.str;
      const auto slash = path.rfind('/'), dot = path.rfind('.');
      if (slash != std::string::npos && dot != std::string::npos && dot > slash)
        stems.emplace(Id(row, "m_id"), path.substr(slash + 1, dot - slash - 1));
    }
    std::unordered_map<uint32_t, uint32_t> type_icon;
    struct TypeInfo { uint32_t category, weapon, text; };
    std::unordered_map<uint32_t, TypeInfo> type_info;
    for (const auto &row : Field(types, "m_DataList", Node::Array).list) {
      type_icon.emplace(Id(row, "m_id"), Id(row, "m_MenuIconID"));
      type_info.emplace(Id(row, "m_id"), TypeInfo{Id(row, "m_ItemCategory"), Id(row, "m_WeaponType"), Id(row, "m_TextID")});
    }
    std::unordered_map<uint32_t, std::string> names, names_raw;
    for (const auto &row : Field(text, "m_DataList", Node::Array).list) {
      const auto &v = Field(row, "m_gametext", Node::Array).list;
      if (v.empty() || v.front().kind != Node::String) continue;
      names_raw.emplace(Id(row, "m_id"), v.front().str);
      if (v.front().str.find('<') == std::string::npos)
        names.emplace(Id(row, "m_id"), v.front().str);
    }
    Items out;
    for (const auto &row : Field(root, "m_DataList", Node::Array).list) {
      ItemInfo info;
      if (const auto n = names.find(Id(row, "m_NameID")); n != names.end()) info.name = n->second;
      info.type = Id(row, "m_TypeID");
      auto icon = Id(row, "m_IconID");
      if (!icon)
        if (const auto t = type_icon.find(info.type); t != type_icon.end()) icon = t->second;
      if (const auto s = stems.find(icon); s != stems.end()) info.icon = s->second;
      info.show_order = Id(row, "m_ShowOrder");
      if (const auto it = row.map.find("m_NotDisplay"); it != row.map.end())
        info.not_display = it->second.kind == Node::Other && it->second.number == 1;
      if (const auto n = names_raw.find(Id(row, "m_NameID")); n != names_raw.end() &&
          n->second.rfind("<favorite_dish_name>", 0) == 0) {
        info.favorite_dish = true;
        info.name = n->second.substr(std::string_view("<favorite_dish_name>").size()); // e.g. " Recipe"
      }
      if (const auto t = type_info.find(info.type); t != type_info.end()) {
        info.category = t->second.category;
        info.weapon_type = t->second.weapon;
        info.type_text = t->second.text;
      }
      out.rows.emplace(Id(row, "m_id"), std::move(info));
    }
    out.valid = !out.rows.empty() && !names.empty();
    return out;
  } catch (const std::exception &) {
    return {};
  }
}
const ItemInfo *Items::Find(uint32_t item) const {
  if (!valid || !item) return nullptr;
  const auto it = rows.find(item);
  return it == rows.end() ? nullptr : &it->second;
}
uint32_t WeaponTypes::Of(uint32_t item) const {
  if (!valid || !item) return 0;
  auto it = types.find(item);
  return it == types.end() ? 0 : it->second;
}
const EnemyInfo *Enemies::Find(uint32_t id) const {
  if (!valid) return nullptr;
  auto it = rows.find(id);
  return it == rows.end() ? nullptr : &it->second;
}
std::string Names::LookupName(uint32_t id) const {
  if (!valid || !id)
    return {};
  auto it = names.find(Alias(id));
  return it == names.end() ? std::string{} : it->second;
}
uint32_t Names::LookupSprite(uint32_t id) const {
  if (!valid || !id)
    return 0;
  auto it = sprite_ids.find(id);
  return it == sprite_ids.end() ? 0 : it->second;
}
} // namespace ot0
