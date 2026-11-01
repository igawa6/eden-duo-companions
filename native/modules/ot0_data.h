// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
namespace ot0 {
struct Names {
  bool valid{};
  std::unordered_map<uint32_t, std::string> names;
  std::unordered_map<uint32_t, uint32_t> sprite_ids;
  std::string LookupName(uint32_t player_id) const;
  uint32_t LookupSprite(uint32_t player_id) const;
};
struct MapNames {
  bool valid{};
  std::unordered_map<uint32_t, std::string> names;
  std::string LookupName(uint32_t text_id) const;
};
// Static enemy table (EnemyID joined with GameTextEnemy EN, EnemyWeakID, and the
// sprite chain EnemyTypeID -> EnemyBattleAnimSet -> EnemyTexID). The weakness
// masks are the full list of weak types (slot count/order only); which ones are
// shown comes from the live revealed masks, never from here.
struct EnemyInfo {
  std::string name, level;
  uint32_t max_shields{};
  uint32_t weapon_weak{}; // bit i = EWEAPON i (1 Sword .. 8 Fan)
  uint32_t magic_weak{};  // bit i = EMAGIC i (1 Fire .. 6 Dark)
  std::string texture;    // native object path of the battle sprite sheet's *_cl texture
};
struct Enemies {
  bool valid{};
  std::unordered_map<uint32_t, EnemyInfo> rows;
  const EnemyInfo *Find(uint32_t enemy_id) const;
};
Enemies DecodeEnemies(std::span<const uint8_t> enemy_id, std::span<const uint8_t> type_id,
                      std::span<const uint8_t> anim_set, std::span<const uint8_t> tex_id,
                      std::span<const uint8_t> weak_id, std::span<const uint8_t> english);
// ItemList m_id -> m_WeaponTypeID (EWEAPON 1 Sword .. 8 Fan; 0 = not a weapon).
struct WeaponTypes {
  bool valid{};
  std::unordered_map<uint32_t, uint32_t> types;
  uint32_t Of(uint32_t item) const;
};
WeaponTypes DecodeWeaponTypes(std::span<const uint8_t> item_list);
// ItemList m_id -> English name (GameTextItem by m_NameID), item type and menu icon ids.
struct ItemInfo {
  std::string name;
  uint32_t type{};          // ItemType m_id (15 shield, 9 head, 10 body, 11 accessory, 1..8 weapons)
  std::string icon;         // TextureID path stem: m_IconID, else the type's m_MenuIconID
  uint32_t show_order{};    // ItemList m_ShowOrder (0 = never listed by the native Items menu)
  uint32_t category{};      // ItemType m_ItemCategory
  uint32_t weapon_type{};   // ItemType m_WeaponType (1 Sword .. 8 Fan)
  uint32_t type_text{};     // ItemType m_TextID (94444 "Dishes")
  bool not_display{};       // ItemList m_NotDisplay (internal counters; never listed natively)
  bool favorite_dish{};     // name is the player's own favorite-dish name (save string)
};
struct Items {
  bool valid{};
  std::unordered_map<uint32_t, ItemInfo> rows;
  const ItemInfo *Find(uint32_t item) const;
};
// QuestList m_id -> English title/subtitle (GameTextQuest), list category, hide-info flag
// ("???" rows), sort priority and recommended level.
struct QuestInfo {
  std::string title, subtitle;
  uint32_t category{}, sort{}, level{};
  bool hide_info{}, hide_list{};
};
struct Quests {
  bool valid{};
  std::unordered_map<uint32_t, QuestInfo> rows;
  std::unordered_map<uint32_t, std::string> texts; // GameTextQuest (task destinations)
  const QuestInfo *Find(uint32_t quest) const;
};
Quests DecodeQuests(std::span<const uint8_t> quest_list, std::span<const uint8_t> english_quest);
Items DecodeItems(std::span<const uint8_t> item_list, std::span<const uint8_t> item_type,
                  std::span<const uint8_t> texture_id, std::span<const uint8_t> english_item);
MapNames DecodeMapNames(std::span<const uint8_t> english_map);
// MapListTable m_id (the exploration map id) -> region text id (GameTextMap) and the region
// emblem's texture name (TextureID path stem, e.g. "UiTX_Common_AreaEmblem_PlainMtex").
struct MapRegion {
  uint32_t region_text{};
  std::string emblem;
};
struct MapRegions {
  bool valid{};
  std::unordered_map<uint32_t, MapRegion> maps;
  const MapRegion *Find(uint32_t map_id) const;
};
MapRegions DecodeMapRegions(std::span<const uint8_t> map_list, std::span<const uint8_t> texture_id);
Names DecodeNames(std::span<const uint8_t> chara_player,
                  std::span<const uint8_t> english_character);
} // namespace ot0
