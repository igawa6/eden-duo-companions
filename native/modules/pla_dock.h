// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pla_blocks.h"
#include <cstdio>

namespace pla {
inline std::string DockCardKey(const Pokemon &p,bool selected,const char *kind) {
    char key[256]{};
    std::snprintf(key,sizeof key,"module:pla:%s:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%s",kind,
        p.species,p.form,p.gender,p.shiny,p.level,p.hp,p.max_hp,p.status,selected,p.nicknamed,p.nickname.c_str());
    return key;
}
inline void PublishDock(const EdenDsmodHostApi &h, const Party &party, const Selector &s) {
    auto number = [&](const std::string &k, int64_t n) { h.publish_i64(h.userdata, k.c_str(), n); };
    auto text = [&](const std::string &k, const std::string &v) {
        if (h.publish_text)
            h.publish_text(h.userdata, k.c_str(), v.c_str());
    };
    number("pla.selector_valid", s.valid);
    number("pla.party_valid", s.valid && party.valid);
    number("pla.party_missing", s.valid && !party.valid);
    number("pla.items_armed", s.valid && s.mode == 0);
    number("pla.party_armed", s.valid && s.mode == 1);
    number("pla.item_count", s.valid ? s.item_count : 0);
    number("pla.item_width", s.valid && s.item_count ? s.item_count * 108 + 14 : 0);
    const int64_t scroll = h.get_i64 ? h.get_i64(h.userdata, "@scroll_x:pla_items", 0) : 0;
    number("pla.items_more", s.valid && int64_t(s.item_count) * 108 + 14 - 770 > scroll);
    number("pla.items_before", s.valid && scroll > 0);
    const unsigned selectedSlot = s.party_count ? s.party[s.party_index] : 6;
    const bool cursor=s.valid && s.mode==1 && party.valid && selectedSlot<party.count &&
        selectedSlot<6 && party.slots[selectedSlot].valid;
    number("pla.party_cursor_valid",cursor);
    number("pla.party_cursor_x",cursor ? int(selectedSlot)*182-18 : 0);
    number("pla.party_cursor_y",cursor ? 910 : 0);
    for (unsigned j = 0; j < 64; ++j) {
        const auto prefix = "pla.item." + std::to_string(j);
        const bool valid = s.valid && j < s.item_count;
        const auto it = valid ? s.items[j] : Item{};
        number(prefix + ".valid", valid);
        char key[160]{};
        if (valid)
            std::snprintf(key, sizeof key, "module:pla:item:%u:%u:%u", it.id, it.count,
                          s.item_index == j ? (s.mode == 0 ? 1 : 2) : 0);
        text(prefix + ".image", key);
    }
    for (unsigned j = 0; j < 6; ++j) {
        const auto prefix = "pla.party." + std::to_string(j);
        const auto &p = party.slots[j];
        const bool valid = s.valid && party.valid && j < party.count && p.valid;
        const bool selected = valid && s.mode == 1 && j == selectedSlot;
        number(prefix + ".valid", valid);
        number(prefix + ".missing", s.valid && party.valid && j < party.count && !p.valid);
        number(prefix + ".species", valid ? p.species : 0);
        number(prefix + ".form", valid ? p.form : 0);
        number(prefix + ".level_value", valid ? p.level : 0);
        number(prefix + ".status_value", valid ? p.status : 0);
        number(prefix + ".hp", valid ? p.hp : 0);
        number(prefix + ".max_hp", valid ? p.max_hp : 0);
        number(prefix + ".fainted", valid && p.fainted);
        number(prefix + ".alive", valid && !p.fainted);
        number(prefix + ".selected", selected);
        text(prefix + ".card",valid ? DockCardKey(p,selected,"party_cell") : "");
        text(prefix + ".plate", valid ? (selected    ? "module:pla:plate:selected"
                                         : p.fainted ? "module:pla:plate:fainted"
                                                     : "module:pla:plate:normal")
                                      : "");
        char key[200]{};
        if (valid)
            std::snprintf(key, sizeof key, "module:pla:name:%u:%u:%s", p.species, p.nicknamed,
                          p.nickname.c_str());
        text(prefix + ".name", key);
        if (valid && p.gender <= 2)
            std::snprintf(key, sizeof key, "module:pla:pokemon:%u:%u:%u:%u",
                          p.species, p.form, p.gender, p.shiny);
        else
            key[0] = 0;
        text(prefix + ".icon", key);
        text(prefix + ".level", valid ? "Lv. " + std::to_string(p.level) : "");
        text(prefix + ".hp_text",
             valid ? std::to_string(p.hp) + "/" + std::to_string(p.max_hp) : "");
        // Companion-authored status labels; bit layout is the Gen-8 PA8 mask.
        text(prefix + ".status", !valid || p.fainted ? ""
                                 : (p.status & 0x88) ? "PSN"
                                 : (p.status & 0x10) ? "BRN"
                                 : (p.status & 0x20) ? "FST"
                                 : (p.status & 0x40) ? "PAR"
                                 : (p.status & 7)    ? "DRW"
                                 : p.status          ? "STATUS"
                                                     : "");
    }
    char hero[300]{};
    if (s.valid && s.mode == 0 && s.item_count) {
        const auto &it = s.items[s.item_index];
        std::snprintf(hero, sizeof hero, "module:pla:hero_item_card:%u:%u", it.id, it.count);
    } else if (s.valid && s.mode == 1 && party.valid && selectedSlot < party.count &&
               party.slots[selectedSlot].valid) {
        const auto &p = party.slots[selectedSlot];
        const auto card=DockCardKey(p,true,"party_hero");
        std::snprintf(hero,sizeof hero,"%s",card.c_str());
    } else if (s.valid && s.mode == 1 && s.party_count) {
        // Keep the game's physical-slot selection visible even if PA8 details
        // cannot be read. Never substitute another Pokemon's name or HP.
        std::snprintf(hero, sizeof hero, "module:pla:hero_slot:%u", selectedSlot + 1);
    }
    text("pla.hero", hero);
}
} // namespace pla
