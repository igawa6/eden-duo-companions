// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes companion: the Sky Armor battle picture (accepted mockup research
// design/redesign-20261009/render/skyarmor.png + skyarmor-armed.png, renderer tools/redesign2/
// render-states.py skyarmor()). The model holds only values the Sky Armor readers verified
// (research contracts/skyarmor-readers.md); absent values are not drawn.
#pragma once

#include "ce_draw.h"
#include "ce_pages.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ce_sky {

struct Pilot {
    ce_pages::Member m; ///< the armour's battle stats (CurrentStats of the mech battle object)
    int gear{-1};       ///< Battle.gear[slot] 0..2
};

struct SkyView {
    std::array<Pilot, 4> pilots{};
    int acting{-1};     ///< pilot column of the acting armour (-1: none)
    bool actor_dim{};   ///< an enemy acts: the next pilot, dimmed
    std::string actor;  ///< acting pilot name
    int frame_id{-1};         ///< equipment id (table 120, types 30-36) of the acting armour's frame
    std::vector<int> weapon_ids; ///< its two weapons (table 120 types 17-25), equip order
    int gear{-1};             ///< acting armour's gear
    int next_gear{-1};        ///< the gear one R press reaches (0->1->2->0; 2->1 with the talent)
    int shift{2};             ///< 0 shift available, 1 shifted this turn, 2 not now (no command)
    int armed{-1};            ///< row armed for the tap-twice confirm (-1 none)
    bool talent{};            ///< party talent 10: gear 2 -> 1 and gear 1 regenerates TP
    double od_pos{};          ///< gauge / 100
    std::vector<ce_pages::OdZone> zones;
    std::optional<double> od_proj; ///< the highlighted command's projected gauge / 100
    std::vector<ce_pages::TurnEntry> turn;
    std::vector<ce_pages::EnemyView> enemies;
    int selected{-1};
    std::vector<ce_pages::SkillRow> skills; ///< the acting armour's weapon skills (cost at its gear)
    bool skills_locked{}; ///< gear 0: the game shows "GEAR 0 - LOCKED" over the skills
};

std::string Signature(const SkyView& v);
ce_draw::Image ComposeSkyArmor(ce_draw::Art& art, const SkyView& v);

/// Gear-row geometry shared with tools/chained-echoes/gen_manifest.py (keep "Name = value").
namespace geo {
inline constexpr int GearRowX = 69, GearRowY0 = 408, GearRowDy = 117, GearRowW = 439, GearRowH = 111;
} // namespace geo

} // namespace ce_sky
