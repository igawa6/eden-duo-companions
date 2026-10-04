// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Game tables and text for the ACNH companion, read lazily from the player's romfs (BCSV + MSBT).
// Joins (research/acnh/catalog/DATA.md, all from data):
//   item pocket icon   ItemParam.ItemMenu (enum cell) == CRC32(ItemMenuIcon.Label)
//                      -> Model/Layout_MenuIcon_<ItemMenuIcon.ResName>
//   item full art      Layout_{FtrIcon,ClosetIcon,DIYRecipeIcon,CookingIcon,GardeningIcon,
//                      DoorOrnamentIcon}_<ItemParam.ResName>, first that exists (which one the
//                      game picks is a LEAD)
//   stack size         ItemKind.MultiHoldMaxNum where CRC32(ItemKind.Label) == ItemParam.ItemKind
//   item name          String/Item/STR_ItemName_* label "<ItemKind>_<id:05d>" ("_pl" = plural);
//                      clothing: Outfit/GroupName/STR_OutfitGroupName_<Kind> "<group:03d>" via
//                      Outfit/GroupColor/STR_OutfitGroupColor_<Kind> "<group>_<kind>_<id:05d>"
//   critters           Insect/Fish/SeafoodStatusParam (UniqueID, ItemID, ResName[Field]);
//                      book art Layout_Book{Insect,Fish,DiveFish}Icon_<res>
//   villagers          NmlNpcParam (Label "ant00", birthday) -> Layout_NpcIcon_<Label>,
//                      names String/Npc/STR_NNpcName
// Thread-safe; tables are loaded once and never freed, so references stay valid (the table
// accessors hand out an empty list until the load succeeds, never a list still being filled).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "acnh_bcsv.h"
#include "acnh_lang.h"
#include "acnh_msbt.h"

namespace acnh {

class Romfs;

class Catalog {
public:
    explicit Catalog(Romfs& romfs);
    ~Catalog();

    struct Item {
        uint16_t id = 0;
        uint32_t kind = 0;           ///< ItemParam.ItemKind (CRC32 of the kind label)
        uint32_t menu = 0;           ///< ItemParam.ItemMenu (CRC32 of an ItemMenuIcon label)
        uint32_t ui = 0;             ///< ItemParam.ItemUICategory (CRC32)
        int width2 = 0, height2 = 0; ///< ItemSize footprint, in half-grid units
        int stack_max = 0;           ///< ItemKind.MultiHoldMaxNum (0 = unknown kind)
        int16_t remake = -1;         ///< RemakeID (-1 = none)
        std::string label, res_name;
        std::string menu_icon; ///< "Layout_MenuIcon_<res>" or empty
    };
    struct Critter {
        uint16_t uid = 0, item = 0;
        std::string label, res; ///< StatusParam label and book-art ResName(Field)
        std::string book_icon;  ///< "Layout_Book<Kind>Icon_<res>" (may not exist for extras)
    };
    struct Villager {
        uint16_t uid = 0;
        std::string label;
        uint8_t birth_month = 0, birth_day = 0;
    };
    struct Recipe {
        uint16_t uid = 0, item = 0;
        int16_t serial = 0;
        uint32_t board = 0, data_type = 0; ///< BoardColorID; RecipeDataType (CRC32 "DIY"/"Cooking")
        bool remake_art =
            false; ///< column 0xad0c787a != 0: card art "Layout_DIYRecipeIcon_<label>_Remake_b_f"
        uint32_t season = 0; ///< CraftRecipeSeason (enum CRC32, raw column 0x7f5b6179)
        uint32_t season_type =
            0;             ///< CraftRecipeSelectSeasonType (enum CRC32, raw column 0x478b74e4)
        std::string event; ///< SelectCalendarEventSeason ("EventNone", "Halloween", ...)
        std::vector<std::pair<uint16_t, uint16_t>> materials; ///< (item, amount)
    };

    bool Load(); ///< all tables (cheap after the first call); false = romfs not readable
    const Item* FindItem(uint16_t id);
    const std::vector<Item>& Items();
    /// Archive names for an item's art, best first: full art (if `big`) then the pocket icon.
    std::vector<std::string> ItemIcons(uint16_t id, bool big);
    /// kind 0 insects, 1 fish, 2 sea creatures (StatusParam row order).
    const std::vector<Critter>& Critters(int kind);
    const Critter* FindCritter(int kind, uint16_t uid);
    const std::vector<Villager>& Villagers();
    const Villager* FindVillager(std::string_view label);
    const std::vector<Recipe>& Recipes();
    /// Any other table, parsed once (Bcsv/<name>.bcsv).
    const Bcsv* Table(std::string_view name);
    /// The hourly weather of a weather pattern (WeatherPatternParam.UniqueID = Land.Weather
    /// pattern): romfs Pack/StaticParam.pack Param/Weather/WeatherPattern.byml, the object whose
    /// label is the pattern's WeatherPatternParam.Label; per clock hour 0..23 the mWeatherType enum
    /// index (code 0x984150 / string table 0x482fec8: 0 clear, 1 fine, 2 cloudy, 3 rain clouds,
    /// 4 rain, 5 heavy rain, 6 snow, 7 heavy snow). Empty when unknown.
    std::vector<int> WeatherHours(int pattern);

    /// Message/<Group>_<Lang>.sarc.zs member "<file>.msbt"; `path` = "<Group>/<file>", e.g.
    /// "LayoutMsg/MenuDevice", "String/STR_Month", "String/Item/STR_ItemName_00_Ftr".
    std::shared_ptr<const Msbt> Message(Lang lang, std::string_view path);
    std::string Text(Lang lang, std::string_view path, std::string_view label);
    std::string ItemName(Lang lang, uint16_t id, bool plural = false);
    /// Pockets names include contents-dependent recipe cards (Item.FreeParam low u16).
    std::string PocketItemName(Lang lang, uint16_t id, uint32_t contents, bool plural = false);
    /// The same name with its control tags (UTF-16, STR_ItemName_* only; "" when unknown).
    std::u16string ItemNameRaw(Lang lang, uint16_t id, bool plural = false);
    std::string VillagerName(Lang lang, std::string_view label);
    /// Every "<Group>/<file>" of a language (tests, tools).
    std::vector<std::string> MessagePaths(Lang lang, std::string_view group);

private:
    struct Names;
    bool LoadLocked();
    std::shared_ptr<const Names> NamesFor(Lang lang);
    struct Group;
    std::shared_ptr<Group> GroupFor(Lang lang, std::string_view group);

    Romfs& romfs;
    std::recursive_mutex mutex;
    bool loaded = false;
    std::vector<Item> items;
    std::unordered_map<uint16_t, size_t> item_index;
    std::array<std::vector<Critter>, 3> critters;
    std::vector<Villager> villagers;
    std::vector<Recipe> recipes;
    std::map<std::string, std::unique_ptr<Bcsv>, std::less<>> tables;
    std::map<std::string, std::shared_ptr<Group>, std::less<>> groups;
    std::map<int, std::shared_ptr<const Names>> names;
    bool weather_tried = false;
    std::map<std::string, std::vector<int>, std::less<>> weather; ///< label -> 24 types
};

} // namespace acnh
