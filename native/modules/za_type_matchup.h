// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace za {
// Type IDs match the personal and move tables. Multipliers use quarters.
constexpr int TypeMatchup(int attack, int first, int second = -1) {
    enum Type { Normal, Fighting, Flying, Poison, Ground, Rock, Bug, Ghost,
                Steel, Fire, Water, Grass, Electric, Psychic, Ice, Dragon, Dark, Fairy };
    constexpr auto mask = []<class... T>(T... types) { return ((1u << types) | ... | 0u); };
    struct Row { unsigned strong, weak, immune; };
    constexpr Row chart[] = {
        {0,mask(Rock,Steel),mask(Ghost)},
        {mask(Normal,Rock,Steel,Ice,Dark),mask(Flying,Poison,Bug,Psychic,Fairy),mask(Ghost)},
        {mask(Fighting,Bug,Grass),mask(Rock,Steel,Electric),0},
        {mask(Grass,Fairy),mask(Poison,Ground,Rock,Ghost),mask(Steel)},
        {mask(Poison,Rock,Steel,Fire,Electric),mask(Bug,Grass),mask(Flying)},
        {mask(Flying,Bug,Fire,Ice),mask(Fighting,Ground,Steel),0},
        {mask(Grass,Psychic,Dark),mask(Fighting,Flying,Poison,Ghost,Steel,Fire,Fairy),0},
        {mask(Ghost,Psychic),mask(Dark),mask(Normal)},
        {mask(Rock,Ice,Fairy),mask(Steel,Fire,Water,Electric),0},
        {mask(Bug,Steel,Grass,Ice),mask(Rock,Fire,Water,Dragon),0},
        {mask(Ground,Rock,Fire),mask(Water,Grass,Dragon),0},
        {mask(Ground,Rock,Water),mask(Flying,Poison,Bug,Steel,Fire,Grass,Dragon),0},
        {mask(Flying,Water),mask(Grass,Electric,Dragon),mask(Ground)},
        {mask(Fighting,Poison),mask(Steel,Psychic),mask(Dark)},
        {mask(Flying,Ground,Grass,Dragon),mask(Steel,Fire,Water,Ice),0},
        {mask(Dragon),mask(Steel),mask(Fairy)},
        {mask(Ghost,Psychic),mask(Fighting,Dark,Fairy),0},
        {mask(Fighting,Dragon,Dark),mask(Poison,Steel,Fire),0},
    };
    if (attack<0 || attack>Fairy || first<0 || first>Fairy) return -1;
    const auto& row=chart[attack];
    auto factor=[&](int type) {
        if(type<0 || type>Fairy) return 2;
        const auto bit=1u<<type;
        return row.immune&bit ? 0 : row.strong&bit ? 4 : row.weak&bit ? 1 : 2;
    };
    return factor(first)*factor(second==first ? -1 : second);
}
static_assert(TypeMatchup(14,15,2)==16 && TypeMatchup(4,12,2)==0 &&
              TypeMatchup(11,9,2)==1 && TypeMatchup(9,11,11)==8);
}
