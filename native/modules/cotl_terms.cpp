// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_terms.h"

#include <map>

#include "cotl_i2.h"

namespace cotl {

namespace {

using EnumTable = std::map<std::string_view, std::map<int32_t, std::string_view>>;

const EnumTable& Enums() {
    static const EnumTable table = [] {
        EnumTable t;
#define COTL_ENUM(name, value, ident) t[#name][value] = ident;
#include "cotl_enums_1_5_27.inc"
#undef COTL_ENUM
        return t;
    }();
    return table;
}

} // namespace

std::string TextWithIcons(std::string_view s) {
    struct Icon {
        std::string_view name;
        uint32_t codepoint;
        const char* color;
    };
    static constexpr Icon kIcons[] = {
        {"icon_RedHeart", kIconHeart, kIconColorRed},
        {"icon_HalfRedHeart", kIconHalfHeart, kIconColorRed},
        {"icon_BlueHeart", kIconHeart, kIconColorBlue},
        {"icon_HalfBlueHeart", kIconHalfHeart, kIconColorBlue},
        {"icon_BlackHeart", kIconHeart, kIconColorBlack},
        {"icon_HalfBlackHeart", kIconHalfHeart, kIconColorBlack},
        {"icon_UIHeartIce", kIconIceHeart, kIconColorIce},
        {"icon_UIHeartFire", kIconFireHeart, kIconColorFire},
    };
    std::string out;
    bool in_span = false;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            // Collapse spaces, also after removed tags and around line breaks
            if (s[i] != ' ' || (!out.empty() && out.back() != ' ' && out.back() != '\n')) {
                out += s[i];
            }
            ++i;
            continue;
        }
        const size_t end = s.find('>', i);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view tag = s.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag == "br") {
            while (!out.empty() && out.back() == ' ') {
                out.pop_back();
            }
            out += '\n';
        } else if (tag.starts_with("color=#") && tag.size() == 13 && !in_span) {
            out += "{c:#FF";
            out += tag.substr(7);
            out += '}';
            in_span = true;
        } else if (tag == "/color" && in_span) {
            out += "{/c}";
            in_span = false;
        } else if (tag.starts_with("sprite name=")) {
            std::string_view name = tag.substr(12);
            if (name.size() >= 2 && name.front() == '"') {
                name = name.substr(1, name.size() - 2);
            }
            for (const Icon& icon : kIcons) {
                if (icon.name == name) {
                    if (in_span) {
                        out += "{/c}"; // spans cannot be nested
                    }
                    out += "{c:";
                    out += icon.color;
                    out += '}';
                    out += static_cast<char>(0xE0 | (icon.codepoint >> 12));
                    out += static_cast<char>(0x80 | ((icon.codepoint >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (icon.codepoint & 0x3F));
                    out += "{/c}";
                    in_span = false;
                }
            }
        }
    }
    if (in_span) {
        out += "{/c}";
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
        out.pop_back();
    }
    return out;
}

std::string StripMarkup(std::string_view s) {
    std::string out;
    bool in_tag = false;
    for (const char c : s) {
        if (c == '<') {
            in_tag = true;
        } else if (c == '>' && in_tag) {
            in_tag = false;
        } else if (!in_tag) {
            out += c;
        }
    }
    // Collapse double spaces after removed sprites
    std::string clean;
    for (const char c : out) {
        if (!(c == ' ' && !clean.empty() && clean.back() == ' ')) {
            clean += c;
        }
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    return clean;
}

std::string Terms::EnumName(std::string_view enum_name, int32_t v) {
    const auto& enums = Enums();
    if (const auto e = enums.find(enum_name); e != enums.end()) {
        if (const auto it = e->second.find(v); it != e->second.end()) {
            return std::string(it->second);
        }
    }
    return "#" + std::to_string(v);
}

std::string Terms::Lookup(std::string_view enum_name, int32_t v, std::string_view prefix,
                          std::string_view suffix) const {
    const std::string ident = EnumName(enum_name, v);
    if (table_ == nullptr || ident.starts_with('#')) {
        return ident;
    }
    std::string term(prefix);
    term += ident;
    term += suffix;
    const auto text = table_->English(term);
    return text ? StripMarkup(*text) : ident;
}

std::string Terms::Trait(int32_t v) const { return Lookup("TraitType", v, "Traits/", ""); }
std::string Terms::Role(int32_t v) const { return Lookup("FollowerRole", v, "Traits/", ""); }
std::string Terms::Item(int32_t v) const { return Lookup("ItemType", v, "Inventory/", ""); }
std::string Terms::TarotName(int32_t v) const { return Lookup("TarotCard", v, "TarotCards/", "/Name"); }
std::string Terms::TarotText(int32_t v) const {
    const std::string ident = EnumName("TarotCard", v);
    const auto text = table_ != nullptr && !ident.starts_with('#')
                          ? table_->English("TarotCards/" + ident + "/Description")
                          : std::nullopt;
    return text ? TextWithIcons(*text) : ident;
}
std::string Terms::Weapon(int32_t v) const {
    return Lookup("EquipmentType", v, "UpgradeSystem/", "/Name");
}
std::string Terms::Curse(int32_t v) const {
    return Lookup("EquipmentType", v, "UpgradeSystem/", "/Name");
}
std::string Terms::Relic(int32_t v) const { return Lookup("RelicType", v, "Relics/", ""); }

std::string Terms::GearText(std::string_view enum_name, int32_t v, std::string_view prefix,
                            std::string_view suffix) const {
    std::string ident = EnumName(enum_name, v);
    if (table_ == nullptr || ident.starts_with('#')) {
        return {};
    }
    auto text = table_->English(std::string(prefix) + ident + std::string(suffix));
    // Variants without their own text ("_Dammed") or with placeholders ("_Corrupted": "{0}, {1}"):
    // text of the base form
    if ((!text || text->find('{') != std::string::npos) && ident.rfind('_') != std::string::npos) {
        ident.resize(ident.rfind('_'));
        text = table_->English(std::string(prefix) + ident + std::string(suffix));
    }
    return text && text->find('{') == std::string::npos ? TextWithIcons(*text) : std::string();
}
std::string Terms::EquipmentText(int32_t v) const {
    return GearText("EquipmentType", v, "UpgradeSystem/", "/Description");
}
std::string Terms::EquipmentLore(int32_t v) const {
    return GearText("EquipmentType", v, "UpgradeSystem/", "/Lore");
}
std::string Terms::RelicText(int32_t v) const { return GearText("RelicType", v, "Relics/", "/Description"); }
std::string Terms::RelicLore(int32_t v) const { return GearText("RelicType", v, "Relics/", "/Lore"); }

} // namespace cotl

namespace cotl {

std::string Terms::Structure(int32_t v) const { return Lookup("StructureType", v, "Structures/", ""); }
std::string Terms::Meal(int32_t v) const { return Lookup("ItemType", v, "CookingData/", "/Name"); }
std::string Terms::Thought(int32_t v) const { return Lookup("Thought", v, "FollowerThoughts/", ""); }

std::string Terms::ItemDescription(int32_t v) const {
    const std::string ident = EnumName("ItemType", v);
    if (table_ == nullptr || ident.starts_with('#')) {
        return {};
    }
    const auto text = table_->English("Inventory/" + ident + "/Description");
    return text ? TextWithIcons(*text) : std::string();
}

std::optional<std::string> Terms::Raw(std::string_view term) const {
    return table_ ? table_->English(term) : std::nullopt;
}

} // namespace cotl
