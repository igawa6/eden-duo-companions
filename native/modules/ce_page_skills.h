// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes companion: the Skills page (Field Home sub-page, accepted 2026-10-09). Layouts:
// research design/redesign-20261009/render/skills.png and skills-assign.png (renderer
// tools/redesign2/render-manage.py). Only values a verified reader produced (research
// contracts/skill-readers.md) are in the model. Slots follow the native Set Skills screen:
// action slots 1-6 "Character ACTION Skills" + 7-8 "Class ACTION Skills", passive slots 1-3
// "Character PASSIVE Skills" + 7-8 "Class PASSIVE Skills".
#pragma once

#include "ce_draw.h"

#include <array>
#include <string>
#include <vector>

namespace ce_page_skills {

/// One equipped skill (action or passive) as the Set Skills screen lists it.
struct Skill {
    bool present{};
    int id{-1};
    int icon{};          ///< skill_type_N (123 Type / 117 icon)
    std::string name;
    int tp{-1};          ///< action: TP cost at the current level (-1 = passive)
    std::string type;    ///< action: 123 Type name ("Attack", ...)
    int lvl{1};          ///< 1..3
    int sp_left{};       ///< SP to the next level (ReturnSPLeft); 0 at Lv 3
    float frac{};        ///< progress inside the current level, 0..1 (1 at Lv 3)
    bool afford{};       ///< the member's SP pool covers sp_left
    bool cls{};          ///< class emblem skill (passives: equippable in the class slots only)
    bool flag{};         ///< spare passive with an empty slot it fits: an alarm row
};

struct Member {
    int id{};
    std::string name;
    std::string sprite; ///< systemgfx portrait sprite
    bool alarm{};       ///< empty slot + a learned skill of that kind not equipped
};

struct Booster {
    std::string label; ///< "HP+15"
    bool taken{};
    bool locked{};     ///< its row needs more learned skills ("Learn N more skills")
};

/// A plan entry: put a learned passive into an empty passive slot (no writes; the player equips
/// it in the native menu and the entry ticks itself).
struct Plan {
    int slot{};      ///< passive slot index 0..4 (native slots 1, 2, 3, 7, 8)
    int id{};        ///< passive id (table 117)
    int icon{};
    std::string name;
    bool done{};     ///< the passive is equipped now (shown with a tick, then dropped)
};

struct SkillsView {
    std::vector<Member> members; ///< picker, native order (PartyMember.posInParty)
    int sel{};                   ///< selected member index
    std::array<Skill, 6> actions;       ///< slots 1-6
    std::array<Skill, 2> class_actions; ///< slots 7-8
    std::array<Skill, 5> passives;      ///< slots 1-3, 7-8 (index 3, 4 = class)
    std::vector<Skill> spare;           ///< learned passives not equipped (native list order)
    bool flag_spare{};   ///< a spare passive fits an empty character slot (class slots: flag_class)
    bool flag_class{};   ///< a spare passive fits an empty class slot
    std::string emblem;  ///< equipped class emblem name ("" none)
    int gs{};            ///< the member's Grimoire Shards (PartyMember.gs, floored)
    int sp{};            ///< the member's SP pool
    std::vector<Booster> boosters; ///< the member's Stat Booster track (16 for characters)
    int boost_taken{};
    std::string boost_next; ///< next bonus label ("" none left)
    int learn_more{};       ///< > 0: the next booster row needs this many more learned skills
    std::vector<std::string> learn_next; ///< next learnable skills (unlocked rows, not learned)
    std::vector<Plan> plans;             ///< the selected member's plan entries
    bool mirror{};        ///< the native Skills menu is open: read-only
};

std::string Signature(const SkillsView& v);
ce_draw::Image ComposeSkills(ce_draw::Art& art, const SkillsView& v);
/// A spare passive row as the page draws it while it is draggable (flagged), over the window's
/// own pixels, so the drag widget can sit on the picture; the runtime's drag ghost is this row.
ce_draw::Image ComposeSpareRow(ce_draw::Art& art, const Skill& s, bool compact);
/// The shadow under the drag ghost (drag_under_src), row-sized.
ce_draw::Image ComposeRowShadow(bool compact);
/// The valid-drop glow drawn over an empty passive slot (kind 0 character slot, 1 class tile).
ce_draw::Image ComposeGlow(ce_draw::Art& art, int kind);
/// The selection mark over a spare passive row held for tap-then-tap (layout A row / B cell).
ce_draw::Image ComposeRowSelected(ce_draw::Art& art, bool compact);

/// Picker cell geometry for n members (n <= 4: the mockup's 288-wide cells).
struct Cell {
    int x{}, w{};
};
Cell PickerCell(int n, int i);

/// Spare passive rows: layout A (<= 2 spare: full rows) or B (compact 2 x 4 grid).
inline constexpr int SpareFullMax = 2, SpareGridMax = 8;

/// Canvas geometry shared with tools/chained-echoes/gen_manifest.py ("Name = value" lines).
namespace geo {
inline constexpr int SkPickY = 30, SkPickH = 90, SkMaxMembers = 8;
inline constexpr int SkSlotX = 658, SkSlotW = 528, SkSlotH = 90, SkSlotY0 = 183, SkSlotDy = 93;
inline constexpr int SkClassX0 = 664, SkClassDx = 261, SkClassY = 828, SkClassW = 255, SkClassH = 90;
inline constexpr int SkFullY0 = 507, SkFullDy = 93;
inline constexpr int SkGridX0 = 658, SkGridDx = 267, SkGridW = 261, SkGridY0 = 507, SkGridDy = 48,
                     SkGridH = 45;
inline constexpr int SkBackX = 24, SkBackY = 948, SkBackW = 264, SkBackH = 96;
inline constexpr int SkBodyY = 132, SkBodyH = 804;
inline constexpr int SkLiftW = 432, SkLiftH = 84, SkGlowPad = 9;
} // namespace geo

} // namespace ce_page_skills
