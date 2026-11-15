// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary: Model -> snapshot values. The snapshot is cleared every tick, so every
// value that should stay on the page is published on every sample; a value that is not valid
// for the current state is simply not published (the manifest gates on its presence), never
// published as a default 0.
//
// Names (all bare, no prefix beyond what is listed):
//   ms.state           ms::State as int; ms.loading / ms.combat / ms.field 1 while true
//   ms.gold, ms.player
//   ms.party.n         party count
//   p<i>.v p<i>.name p<i>.lv p<i>.shift p<i>.fol p<i>.spr p<i>.rowspr p<i>.ab  i = 0..11 (party
//                      list rows: Field's follower candidates and the Team list; p<i>.light /
//                      p<i>.dark are derived in the manifest from p<i>.shift)
//   t.*                the Team page's focused monster's Equipment-side detail panel: t.v t.name
//                      t.i (its party index) t.sp t.hptxt t.mptxt t.g<k> (gear name, k = 0..5)
//   f.*                the Exploration page's follower card: f.v f.name f.spr f.ab f.abdw
//   b.v b.turn b.tgt b.keeper b.unknown b.cname b.cmptxt b.t1 (always 0/1: the Keeper duel's
//                      first-turn layout gate)
//   a<i>.v a<i>.name a<i>.hp a<i>.hpmax a<i>.mp a<i>.mpmax a<i>.cur      i = 0..2 (allies)
//   s<i>.v s<i>.mp s<i>.ok s<i>.sel  i = 0..9
//   e<i>.v e<i>.show e<i>.name e<i>.lv e<i>.hp e<i>.hpmax e<i>.sel  i = 0..2
//   m.v m.scene m.pct10
//   m.tp.h (the scrolling list's content height); tq<i>.name .here .index .dy i = 0..23 (one row
//                      per native destination); a repeated area's row: tq<i>.mini (its place in
//                      the area) and .label (the native map name of a place in its room, if any)
//   r<i>.v r<i>.name r<i>.ico r<i>.sel r<i>.tap  the native command rail rows the game offers
//   b.railfocus b.rname b.rdesc  the cursor is on the rail: that command's name and description
//   s<i>.label         the skill name with the native inline icons ({i:...})
//   f.abdw             the follower's explore ability description, one native point per line
//   e<i>.wr            target card: "Weak [icon] Earth  Resist [icon] Wind" (native words/icons)
//   e<i>.hitw e<i>.hitr  the targeted skill hits that enemy's (always shown) weakness / resistance
//   b.exec b.dur b.heal b.diff b.buffs  live Execution / Combat Duration / Healthiness / Enemy
//                      Difficulty / Buffs and Debuffs (the Combat Result screen's factors)
//   owner round 3:
//   sk.w1 / sk.w2 s.ex ss.x  the skill window's widths (one column / rows + the other skill
//                      column as an icon strip), the rows' and the strip's canvas x shifts
//   s<i>.badge ss<i>.v ss<i>.ico ss<i>.badge ss<i>.ok  the level number on a skill icon; the
//                      strip's cells (the native window's other skill column)
//   b.sdesc            the hovered skill's / level's native SkillTooltip description
//   b.lvly             the focused skill has levels: its "[Y] Select Level" control
//   b.lvl lv<i>.v .label .ico .badge .mp .ok .dim .sel  the level popup (i = 0..4)
//   w<i>.sel           the Switch Monster list's native cursor
//   e<i>.x e<i>.xl e<i>.xr e<i>.wk e<i>.rs  a target card's battlefield slot (canvas x), its
//                      selector corners' x, its compact Weak / Resist lines
//   b.tgta a<i>.x a<i>.xl a<i>.xr a<i>.lv a<i>.wk a<i>.rs  a skill on the own side: the ally cards
#pragma once

#include "ms_model.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ms {

/// Numeric part of a native ComboView label, with or without a colon.
std::string CombatFigure(std::string_view text);

struct Sink {
    std::function<void(std::string_view, std::int64_t)> i64;
    std::function<void(std::string_view, std::string_view)> text;
    /// The frame of a battle sprite set to show now ("Thanatos_light/0" -> ".../3"), with a
    /// per-card phase salt; empty = the still frame (no animation, e.g. reduced motion).
    std::function<std::string(const std::string&, int)> sprite{};
};

/// Readable element name ("" for elements the game shows no icon for).
std::string_view ElementName(int element);
/// The game's element icon sprite in pictures.idx ("ui/ElementFire"; "" when there is none).
std::string ElementSprite(int element);
/// A label followed by inline icons: "<text>{i:<sprite>}..." (drawn by the module's text
/// pictures right after the rendered text).
std::string WithIcons(std::string text, const std::vector<std::string>& sprites);
/// Command-rail rows the game offers now (active, selectable, not disabled), as indices into
/// Battle::defaults, at most `limit`.
std::vector<int> VisibleRail(const Battle& battle, int limit = 5);
/// "Fire, Earth" for a list of elements.
std::string ElementList(const std::vector<int>& elements);
/// "Keepers Stronghold3" -> "Keepers Stronghold" (scene names carry a room number suffix).
std::string AreaFromScene(std::string_view scene);

/// Direct travel is disabled during dialogue and other native menus; the teleport menu is allowed.
bool CanTeleport(const Model& model);
/// The native player-turn flag alone can outlive the command-taking combat state.
bool CanBattleInput(const Model& model);

/// Teleporter list rows (canvas px): a dense-list touch target of at least 88 px (LEGIBILITY),
/// 120 px for a repeated area's two-line row, 12 px apart; the list scrolls (drag / fling) in its
/// 680 px window, one row per native destination (MapMenu.teleports, in its order).
inline constexpr int TeleportRowPx = 88, TeleportRoomRowPx = 120, TeleportGapPx = 12, TeleportListPx = 680;
/// Rows the manifest draws (the game lists fewer destinations than this).
inline constexpr int TeleportRows = 24;
/// The picture key of a repeated area's crystal: where it sits in its area (DrawTeleporterArea).
std::string TeleportMiniKey(const MapInfo& map, const Teleporter& t);
/// Rows (= teleports[row]) with their y in the scrolling list and the content height (m.tp.h).
void PublishTeleports(const MapInfo& map, const Sink& out, bool enabled = false);

/// The Combat Result screen's Execution as it would credit it now, in percent (CombatResult
/// .CalculateResult: (Pre+Exec)/k + WeaknessExploitCount * 0.03 + ComboCounter * c, k / c by
/// the average level of the acting monsters); -1 without a read score.
int ExecutionPercent(const Battle::Score& score);
/// The average player level CalculateResult weighs everything by: totalLevelCount /
/// totalActionsCount + 1 in float (0 before the first player action).
float ScoreLevel(const Battle::Score& score);
/// The result screen's "Enemy Difficulty" points as they would be credited now (CalculateResult:
/// per enemy (DifficultyRating + shifted) * 225 + 50 + DifficultyRatingBonus; x1.5 / x0.5 for one
/// or two enemies; x Lerp(1.2, 0.8, avg enemy level / 40); x the player-vs-enemy level factor),
/// -1 without a read score.
int EnemyDifficultyPoints(const Battle::Score& score);
/// The result screen's "Buffs / Debuffs" points (CalculateResult: DotDamageCount and the allies'
/// buff / enemies' debuff strength, weighted by the player level, capped at 4000), -1 without a
/// read score.
int BuffPoints(const Battle::Score& score);
/// The native healthiness percent the result screen prints: RoundToInt(HP%^2 * 100) (the
/// running mean is squared by CalculateResult before the screen reads it back).
int HealthinessPercent(const Battle::Score& score);
/// "2", "2.5", "2.33", "2.66": Combat Duration as the result screen writes the turn count.
std::string TurnsText(const Battle::Score& score, int monsters);

/// Publishes the model. `team_focus` is the Team page's focused party index (clamped).
/// The native item window's columns as the bottom draws them: the cursor's column expanded,
/// the others as icon strips in column order (those before it on the left, after it on the right).
struct ItemLayout {
    int expanded{-1};          ///< the expanded column (-1 = no items)
    std::vector<int> strips;   ///< collapsed columns, left to right
    int left{};                ///< how many strips sit left of the expanded column
};
ItemLayout LayoutItems(const Battle& battle, int max_columns = 3);
/// The item in a slot of that layout: slot 0 = the expanded column, 1.. = strips[slot - 1].
const MenuRow* ItemAt(const Battle& battle, int slot, int row);
inline constexpr int ItemRows = 6, ItemStripPx = 80, ItemRowsPx = 784; // canvas px (4x mockup)

/// The native command window's skill columns as the bottom draws them (owner round 3): the
/// window has three columns of five (CombatMenu.MENU_COLUMNS); the commands are the rail, the
/// skills fill column 1 and overflow into column 2. The column the game expands (its cursor's)
/// is drawn as rows, the other skill column as an icon strip on its native side.
struct SkillLayout {
    int expanded{-1};     ///< the expanded skill column (1 or 2; -1 = no skills)
    int strip{-1};        ///< the other skill column, drawn as icons (-1 = none)
    bool strip_left{};    ///< the strip sits left of the rows (the cursor is in column 2)
};
SkillLayout LayoutSkills(const Battle& battle);
/// Index into Battle::skills of a slot's row: slot 0 the expanded column, 1 the strip; -1 = none.
int SkillIndex(const Battle& battle, int slot, int row);
const Skill* SkillAt(const Battle& battle, int slot, int row);
/// SkillStripPx: the strip's cell (gen_manifest SKILL_STRIP 22 x 4), the 88 px touch minimum
inline constexpr int SkillRows = 5, SkillStripPx = 88, SkillRowsPx = 784; // canvas px
inline constexpr int LevelRows = 5; ///< level popup rows the page draws
/// Horizontal target cards (owner round 3): the slot of each monster, left to right by its
/// battlefield x (Monster.CombatPos); ties keep the list order.
std::vector<int> FieldSlots(const std::vector<float>& xs);
/// FieldSlots of the battle's enemies (ally = false) or allies on the field (at most three).
std::vector<int> CardSlots(const Battle& battle, bool ally);
inline constexpr int TargetCardPx = 264; ///< one card's pitch, canvas px (66 mockup px)
/// The native selector points at the own side (ETargetType Self / SingleAlly / AllAllies /
/// DeadAlly) for a skill: the target cards are the allies.
bool TargetsAllies(const Battle& battle);
/// The native selector takes this monster now (a skill's target, not the Monster Info browse):
/// it is in TargetSelector.targets, or on the side of a group action (AllEnemies 1 / AllAllies 4),
/// the same test as the helper's TARGET_PICK. Its card tap is live only then (<side><i>.tok).
bool TargetTakes(const Battle& battle, std::uint64_t monster, bool ally);

/// The four Spectral Familiars of the game's starter pick (battle sprite sets), in its order:
/// the title / loading scene.
inline constexpr const char* Familiars[4] = {"SpectralWolf", "SpectralEagle", "SpectralToad", "SpectralLion"};

void Publish(const Model& m, int team_focus, const Sink& out);

} // namespace ms
