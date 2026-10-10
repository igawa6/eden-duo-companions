// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes companion: the bottom-screen pictures. Battle B1 (accepted 2026-10-09,
// research design/redesign-20261009/render/battle-early.png, renderer tools/redesign2/
// render-battle.py with STRIP = 'T1') and the secondary states (render-states.py), composed with
// ce_draw from the player's own romfs. The model holds only values a verified reader produced
// (research contracts/battle-readers.md); an element whose reader is not verified is absent from
// the model and is not drawn (no placeholder, no default 0).
#pragma once

#include "ce_draw.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ce_pages {

struct TurnEntry {
    bool party{};
    std::string name;   ///< party member name / enemy name + A/B suffix
    std::string sprite; ///< party: systemgfx portrait sprite name
    int enemy_id{-1};   ///< enemy: ctb_<id>
};

struct EnemyView {
    int enemy_id{-1};
    std::string name;
    std::string suffix; ///< "A", "B", ... only when the same enemy appears more than once
    int type{-1};       ///< EnemyInfo.ReturnEnemyType index (0 Beast .. 8 Machine)
    double hp{};        ///< internal HP ratio, drawn only as a bar
    bool alive{};
    std::array<int, 6> res{}; ///< fire, water, earth, wind, dark, light (live CurrentStats)
    std::vector<std::pair<int, int>> states; ///< (icon pic, turns)
    bool unlocked{};                         ///< bestiary entry present and kills >= needed
};

struct SkillRow {
    int type{};
    std::string name;
    std::string desc; ///< the battle Skill copy's description, values substituted
    std::optional<int> cost; ///< TP in the current zone (Overdrive halves, half to even)
    int base_cost{-1};       ///< TP before the zone multiplier
    bool match{};            ///< type == the required Overdrive category (gold name)
};

struct Member {
    bool present{};
    std::string name, sprite;
    int hp{}, hp_max{}, tp{}, tp_max{};
    bool alive{true};
    std::vector<std::pair<int, int>> states; ///< (icon pic, turns)
    int hp_ghost{-1}; ///< HP before the last hit, drawn as a draining segment for ~300 ms (-1 none)
};

struct OdZone {
    double a{}, b{}; ///< fractions of the gauge
    char kind{'o'};  ///< 'o' Overdrive (green), 'h' Overheat (red)
};

struct BattleView {
    bool od_visible{}; ///< the game's own gauge is open (Overdrive system unlocked)
    double od_pos{};   ///< ODMultiplicator / 100
    bool od_mode{};    ///< BattleFunctions.overDriveMode (gauge in the green zone)
    int od_cat{};      ///< required category = skill type id (0: locked, outside Overdrive)
    int od_left{};     ///< ODTurnsLeft (with od_cat)
    std::vector<OdZone> zones;     ///< from ODvalue[100]
    std::optional<double> od_proj; ///< the highlighted command's projected gauge / 100
    std::vector<TurnEntry> turn; ///< Act + next, at most 7
    std::vector<EnemyView> enemies;
    int selected{-1}; ///< index into enemies (an alive one)
    bool sheet{};     ///< full bestiary sheet open (only when the selected one is unlocked)
    std::string actor, actor_sprite;
    int actor_tp{-1};
    bool actor_dim{};   ///< an enemy acts: the next party actor, dimmed
    bool skills_known{}; ///< the skill list reader produced rows
    std::vector<SkillRow> skills;
    std::array<Member, 4> party{};
    std::array<Member, 4> partners{}; ///< linked reserve of each column (positional link)
    std::array<int, 4> link{-1, -1, -1, -1}; ///< -1 no partner, 0 linked, 1 switched this battle
    bool can_switch{}; ///< the acting member's partner can still switch (native Switch command)
    bool ultra_visible{}; ///< the game's Ultra Move bar is active (system unlocked)
    int ultra{};          ///< Battle.ultimate 0..100
    bool ultra_usable{};  ///< ZR would open the Ultra Move menu now (the button is enabled)
    int acting{-1}; ///< party column of the acting member (-1: an enemy acts)
    bool live_overlays{}; ///< the Overdrive marker / tick and the chip cursor are widgets (live module)
    bool native_targeting{}; ///< the game's target cursor is on enemies (selection follows it)
    bool native_all{};       ///< ... on all of them (all-target skill): every chip is highlighted
    bool Linked() const {
        for (const auto& m : partners)
            if (m.present)
                return true;
        return false;
    }
};

/// Canonical text of a model (stable across runs): equal views give equal signatures.
std::string Signature(const BattleView& v);

/// Native Formation screen mirror (UX-SPEC 2.2 "Formation -> battle-style party columns"): the four
/// active members over their linked reserves, read-only.
struct FormationMember {
    Member m;          ///< present, name, sprite, HP/TP, alive, states
    std::string emblem; ///< equipped class emblem name ("" none)
    int atk{}, mag{}, def{}, mnd{}, agi{};
};
struct FormationView {
    std::array<FormationMember, 4> party{};
    std::array<FormationMember, 4> partners{};
    bool retry{}; ///< opened by the Game Over "Retry Battle" choice
};
std::string Signature(const FormationView& v);

/// Enemy art / text needed only by the open sheet (static game tables 112 / 115 / 120).
struct SheetData {
    int hp{}, atk{}, def{}, mag{}, mnd{}, agi{};
    std::array<int, 6> res{};
    std::vector<std::string> drops;
    std::string steal, desc;
    bool ok{};
};
SheetData LoadSheet(ce_draw::Art& art, int enemy_id);

ce_draw::Image ComposeBattle(ce_draw::Art& art, const BattleView& v);
/// Tap geometry of a battle picture, computed from the same model the picture was drawn from:
/// the target card / open sheet box and the chip cells (left x, top y, ChipW x ChipH each) with the
/// enemy index each chip selects.
struct BattleHit {
    ce_draw::Canvas::Box card{};
    bool sheet{};
    int chip_y{};
    std::vector<int> chip_x, chip_enemy;
};
BattleHit HitGeometry(const BattleView& v);
int LinkedEnemyW(const BattleView& v);
/// x of the Overdrive marker / tick for a gauge position (B1 band geometry, for the live widgets).
int OdMarkerX(double pos);
/// The T1 turn strip (rail, Act segment + gold divider, names with A/B letters) at the B1 position
/// shifted by dy; shared with the Sky Armor page.
void DrawTurnStrip(ce_draw::Canvas& c, const std::vector<TurnEntry>& turn, int dy);
/// The strip alone (backdrop included), cropped to geo::Strip* at dy: the sliding overlay.
ce_draw::Image ComposeStrip(ce_draw::Art& art, const std::vector<TurnEntry>& turn, int dy);
ce_draw::Image ComposeFormation(ce_draw::Art& art, const FormationView& v);
ce_draw::Image ComposeTitle(ce_draw::Art& art);
/// In game, not in a normal battle (Field Home is a later slice): backdrop, dimmed logo and the
/// HUD-settings button.
ce_draw::Image ComposeHolding(ce_draw::Art& art);
/// "Unsupported version": detected text only from a verified build identity.
ce_draw::Image ComposeUnsupported(ce_draw::Art& art, std::string_view detected);
/// The settings page: hide_* = switch values (1 = HIDE).
ce_draw::Image ComposeSettings(ce_draw::Art& art, bool hide_ctb, bool hide_party, bool hide_od,
                               bool hide_ultra, bool hide_gear);

/// Geometry shared with the manifest generator (tools/chained-echoes/gen_manifest.py parses
/// these lines: keep the "Name = value" form).
namespace geo {
inline constexpr int ChipX0 = 135, ChipPitch = 102, ChipTop = 372, ChipW = 96, ChipH = 108;
inline constexpr int MaxChips = 6;
inline constexpr int CardX = 24, CardY = 366, CardW = 768, CardH = 426;
inline constexpr int LinkedCardW = 648, LinkedCardH = 276, LinkedCardWideW = 474;
inline constexpr int SheetW = 1192;
inline constexpr int SetRowY0 = 200, SetRowDy = 136, SetShowX = 600, SetHideX = 894,
                     SetBtnW = 222, SetBtnH = 96;
inline constexpr int BackX = 24, BackY = 960, BackW = 240, BackH = 96;
/// Ultra Move button (in the band, between the category text and the zone name)
inline constexpr int UltraX = 510, UltraY = 12, UltraW = 426, UltraH = 99;
/// Strip lift when the Overdrive band collapses (Ultra row only / nothing)
inline constexpr int BandUltraLift = 72, BandFullLift = 180;
/// The turn strip's rect at dy 0 (diamond tops to the rail's bottom; slide overlay)
inline constexpr int StripX = 24, StripY = 195, StripW = 1192, StripH = 141;
inline constexpr int SettingsBtnX = 1000, SettingsBtnY = 24, SettingsBtnW = 216,
                     SettingsBtnH = 96;
} // namespace geo

} // namespace ce_pages
