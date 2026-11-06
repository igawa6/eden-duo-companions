// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Enum value to game text. Term formats from the game code, see tools/cotl/README.md (I2 terms).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cotl {

class I2Table;

// Removes rich-text markup (<color=...>, <sprite ...>, </...>) the companion font cannot do.
std::string StripMarkup(std::string_view s);

// Icons in text: extra characters of the companion font (native/modules/cotl_font.h) from the HUD hearts.
// Unused range below the highest font characters (U+2026), so that the dense run reaches them.
inline constexpr uint32_t kIconHeart = 0x1E00;
inline constexpr uint32_t kIconHalfHeart = 0x1E01;
inline constexpr uint32_t kIconIceHeart = 0x1E02;
inline constexpr uint32_t kIconFireHeart = 0x1E03;
inline constexpr const char* kIconColorRed = "#FFE0352B";
inline constexpr const char* kIconColorBlue = "#FF19B8A6";
inline constexpr const char* kIconColorBlack = "#FF8C8C9C";
inline constexpr const char* kIconColorIce = "#FF7FD0F0";
inline constexpr const char* kIconColorFire = "#FFF0A030";

// Game text for labels with color_markup: <sprite name="icon_...Heart"> as a coloured icon,
// <color=#RRGGBB> as a colour span, <br> as a line break; other tags and sprites are dropped.
std::string TextWithIcons(std::string_view s);

class Terms {
public:
    explicit Terms(const I2Table* table) : table_(table) {}

    std::string Trait(int32_t v) const;      // Traits/{TraitType}
    std::string Role(int32_t v) const;       // Traits/{FollowerRole}
    std::string Item(int32_t v) const;       // Inventory/{ITEM_TYPE}
    // Inventory/{ITEM_TYPE}/Description without markup; empty without a table or term
    std::string ItemDescription(int32_t v) const;
    std::string TarotName(int32_t v) const;  // TarotCards/{Card}/Name
    std::string TarotText(int32_t v) const;  // TarotCards/{Card}/Description
    std::string Weapon(int32_t v) const;     // UpgradeSystem/{EquipmentType}/Name
    std::string Curse(int32_t v) const;      // UpgradeSystem/{EquipmentType}/Name
    std::string Relic(int32_t v) const;      // Relics/{RelicType}
    std::string EquipmentText(int32_t v) const; // UpgradeSystem/{EquipmentType}/Description
    std::string EquipmentLore(int32_t v) const; // UpgradeSystem/{EquipmentType}/Lore
    std::string RelicText(int32_t v) const;     // Relics/{RelicType}/Description
    std::string RelicLore(int32_t v) const;     // Relics/{RelicType}/Lore
    std::string Structure(int32_t v) const;  // Structures/{StructureBrain.TYPES}
    std::string Meal(int32_t v) const;       // CookingData/{ITEM_TYPE}/Name
    std::string Thought(int32_t v) const;    // FollowerThoughts/{Thought}

    // English text of any term; nullopt without a table or term
    std::optional<std::string> Raw(std::string_view term) const;

    // Code identifier of an enum value ("TraitType", 3 → "DesensitisedToDeath");
    // unknown value: "#<value>".
    static std::string EnumName(std::string_view enum_name, int32_t v);

private:
    // Description or lore; variants without their own text take the base form's, otherwise empty
    std::string GearText(std::string_view enum_name, int32_t v, std::string_view prefix,
                         std::string_view suffix) const;
    // Text for `prefix + identifier + suffix`; without a table or term, the identifier itself.
    std::string Lookup(std::string_view enum_name, int32_t v, std::string_view prefix,
                       std::string_view suffix) const;

    const I2Table* table_;
};

} // namespace cotl
