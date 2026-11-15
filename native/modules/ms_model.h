// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary (0100A0F00DA68000, Ver 2.1.0.32, build 8EA928B6AA10C84F) companion model:
// plain values read from the running game by ms_reader and published by ms_publish. Nothing
// here touches guest memory, so the publish rules can be tested without a game.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ms {

/// The last whole number in a rendered text ("120", "Mana: 120"), -1 when there is none.
inline int LastNumber(std::string_view t) {
    std::size_t end = t.size();
    while (end > 0 && (t[end - 1] < '0' || t[end - 1] > '9'))
        --end;
    if (end == 0)
        return -1;
    std::size_t begin = end;
    while (begin > 0 && t[begin - 1] >= '0' && t[begin - 1] <= '9' && end - begin < 9)
        --begin;
    int v = 0;
    for (std::size_t i = begin; i < end; ++i)
        v = v * 10 + (t[i] - '0');
    return v;
}

/// Companion screen state (published as ms.state).
enum class State : int {
    Unknown = 0,     ///< roots missing (boot, title not yet initialised)
    Title = 1,       ///< GameStates.MainMenu
    Exploring = 2,   ///< GameStates.Exploring, no dialogue open
    Combat = 3,      ///< GameStates.Combat
    Menu = 4,        ///< GameStates.IngameMenu (native pause menu)
    Loading = 5,     ///< scene change / teleport / transition states 4..10
    Dialogue = 6,    ///< DialogueView.IsOpen while exploring
    Cinematic = 7,   ///< GameStates.Cinematic / NavigateCamera
    // 8: not used (a wrong game build never creates the module; the host draws that card)
    LeadPick = 9,    ///< a battle's native start screen (VS): a Keeper Duel's intro with its lead-3
                     ///< pick, a wild battle's Start Combat / Choose Monsters / Retreat menu
};

/// GameStateManager.GameStates -> companion state (dialogue is decided separately).
State StateFromGame(int game_state);

struct Battle;
/// A battle's native start screen (the VS screen before the first turn), which the bottom dims
/// like the other native menus: a Keeper battle's whole intro (its lead pick: choose 3 of the
/// team) and a wild battle's Start Combat / Choose Monsters / Retreat menu with its Choose
/// Monsters pick. Not a mid-battle backup or switch select.
bool LeadPick(const Battle& battle);

/// EElement (Neutral 0 .. QuadElement 9).
enum class Element : int { Neutral = 0, Fire, Water, Wind, Earth, Physical, Magical, Debuff, None, Quad };

struct Gear {
    std::string name; ///< the item GameObject's name ("Katar+5"), else BaseItem.Name ("" = empty slot)
};

struct PartyMonster {
    std::string name;
    int level{};
    int hp{}, hp_max{};
    int mana{}, mana_max{};
    int shift{};       ///< EShift: 0 normal, 1 light, 2 dark
    int skill_points{};
    bool active{};     ///< in MonsterManager.Active (else Inactive / bench)
    std::string ability;      ///< ExploreAbility.Name ("" when none)
    std::string ability_desc; ///< ExploreAbility.Description
    std::vector<int> ability_types; ///< native AttackAbility/MountAbility.InteractableTypes
    std::array<Gear, 6> gear; ///< Weapon, Weapon2, Accessory 1-4
    std::string sprite;       ///< battle sprite name in pictures.idx ("Thanatos_light/0")
    std::uint64_t ptr{};      ///< the Monster object (guest address; only for helper requests)
    float combat_x{};         ///< Monster.CombatPos.x: where it stands on the battlefield (battle only)
    std::vector<int> weak, resist; ///< innate weaknesses / resistances (battle allies only, as the cards)
};

struct Skill {
    std::string name;
    std::string icon;
    int mana{};          ///< the cost the native row shows (effective, after Channel etc.)
    int target{};        ///< ETargetType
    int element{-1};     ///< EElement of the first ActionDamage, -1 = not read
    bool ultimate{};
    bool affordable{};   ///< mana <= the caster's current mana
    int column{-1}, row{-1};  ///< position in CombatMenu.MenuList.Lists
    std::uint64_t item{};     ///< the MenuListItem (helper requests)
    /// Sprites the native row draws right after its name (CombatMenuItem.TypeIcon, then
    /// .ElementIcon), only those the game shows
    std::vector<std::string> inline_icons;
    int base_mana{};       ///< BaseAction.Mana (base cost, before modifiers; diagnostic)
    std::string cost_text; ///< MenuListItem.infoText as rendered (diagnostic)
    /// MenuListItem.iconInfo as rendered: the small number on the row's icon (the castable
    /// action's level, BaseSkill.GetIconInfo), "" when the game shows none
    std::string badge;
    bool disabled{};       ///< MenuListItem.IsDisabled (the game greys the row: CanUseAction false)
};

/// The native SkillTooltip (the description strip under the command window): the hovered
/// skill / level as the game describes it.
struct SkillTip {
    bool visible{};
    std::string name;        ///< "Slash Lvl 5"
    std::string description; ///< as rendered, one line (Reader::TextMesh maps native line breaks to spaces)
    std::string subtitle;    ///< the tooltip's right-hand line as rendered ("Mana: 180")
};

struct Enemy {
    std::string name;
    int level{};
    int hp{}, hp_max{};
    std::string sprite;
    /// The native target card's Weak / Resist (MonsterInfo.UpdateMonsterInfo): the innate
    /// PassiveElementModifier passives, Modifier > 0 weak, anything else resist; always shown
    /// (the game has no reveal condition), buff-granted ones are not on the card
    std::vector<int> weak;
    std::vector<int> resist;
    std::uint64_t ptr{};     ///< the Monster object (helper requests)
    float combat_x{};        ///< Monster.CombatPos.x: where it stands on the battlefield
};

/// A row of a native combat menu the bottom can commit (helper MENU_PICK).
struct MenuRow {
    std::string name;
    std::string icon;
    std::string info;       ///< right-hand text as rendered (item quantity)
    bool disabled{};          ///< MenuListItem.IsDisabled
    bool selectable{true};    ///< MenuListItem.IsSelectable
    bool active{true};        ///< the row's GameObject is active (shown)
    int column{-1}, row{-1};
    std::uint64_t item{};
    std::uint64_t monster{};  ///< the row's Displayable when it is a Monster (switch list)
    std::string sprite;       ///< that monster's battle sprite set (switch list)
    std::vector<int> weak, resist; ///< its innate weaknesses / resistances (switch list cards)
};

/// The rows a native MenuList actually shows: a pooled MenuListItem the game parked (GameObject
/// inactive, e.g. a stale "Equipment" row left in the item menu's second column) is not a row.
std::vector<MenuRow> ShownRows(const std::vector<MenuRow>& rows);

/// The native MonsterInfo card (target select / the Monster Info command), as rendered.
struct InfoCard {
    bool open{};              ///< TargetSelector selecting in IsEnemyInfo mode (Monster Info)
    bool visible{};           ///< the card's GameObject is shown (also while targeting)
    std::string headline;     ///< "Level 5 Vaero"
    std::string health, mana; ///< "423/423", "110/110"
    std::string crit, crit_damage; ///< "10%", "+50%"
    std::string sprite;       ///< the monster's battle sprite set
    std::vector<int> weak, resist; ///< innate PassiveElementModifier elements (as the card)
};

/// The native ItemTooltip under the item menu (the highlighted item).
struct ItemTip {
    bool visible{};
    std::string name, description, type, icon;
};

struct Battle {
    bool valid{};
    int combat_state{-1};   ///< CombatController.CombatStates
    bool player_turn{};
    bool targeting{};       ///< TargetSelector.IsSelecting
    int cursor{-1};         ///< native command list index (CombatMenu.MenuList.CurrentIndex)
    int target{-1};         ///< enemy index under the native target cursor
    int caster{-1};         ///< index of CombatController.CurrentMonster in `allies`
    bool keeper{};          ///< Keeper battle (champion / duel encounter)
    int encounter_type{-1}; ///< MonsterEncounter.EncounterType
    int turn{};
    int backup_enemies{};   ///< enemies not yet on the field (not revealed)
    std::string combo;      ///< ComboView.Hits as rendered
    std::string damage;     ///< ComboView.Damage as rendered ("100%")
    std::vector<PartyMonster> allies; ///< CombatController.PlayerMonsters (on the field)
    /// the current monster's actions: the command window's skill columns (1, then 2: a monster
    /// with more than five skills overflows into the window's third column), in native order
    std::vector<Skill> skills;
    /// the skill column the native window draws expanded (MenuListItem.isExpanded: the cursor's
    /// column; the other one is an icon strip), -1 when not known
    int skill_column{-1};
    /// CombatMenu.LevelSelectButton is shown: the hovered skill has several levels (the native
    /// "[Y] Select Level" hint; CombatMenu.OnItemHovered -> ActionHasMultipleLevels)
    bool level_hint{};
    SkillTip skill_tip;               ///< the SkillTooltip while it is shown
    /// The native level popup (CombatUIController.LevelSelect, opened by Y on a skill with
    /// levels): its rows, highest level first, and its cursor row
    bool level_open{};
    std::vector<Skill> levels;
    int level_cursor{-1};
    std::vector<Enemy> enemies;       ///< CombatController.Enemies (on the field)
    bool menu_open{};                 ///< the command MenuList is taking input
    std::vector<MenuRow> defaults;    ///< the command rail, column 0 (Attack, Items, Switch, ...)
    int rail_cursor{-1};              ///< native cursor row while it is on the rail
    std::string rail_description;     ///< visible SkillTooltip.Description for that command
    bool items_open{};                ///< CombatItemMenu.MenuList taking input
    /// its shown rows, every column in the native order (column 0 top to bottom, then column 1
    /// ...); the native window expands the cursor's column and shows the others as icons
    std::vector<MenuRow> items;
    int item_column{-1}, item_row{-1}; ///< the native cursor (CurrentListIndex / CurrentIndex)
    ItemTip item_tip;
    InfoCard info;
    bool switch_open{};               ///< StartCombatMenu.MonsterMenu taking input
    /// StartCombatMenu (CombatUIController.StartMenu) IsOpen / IsBackupSelect / IsSwitchSelect:
    /// open without either select flag = the pre-battle start screen (wild: Start Combat / Retreat;
    /// Keeper battle: the lead pick), see LeadPick()
    bool start_open{}, start_backup{}, start_switch{};
    std::vector<MenuRow> switches;    ///< the MonsterMenu's rows (switch_open)
    int switch_column{-1}, switch_row{-1}; ///< its native cursor (CurrentListIndex / CurrentIndex)
    std::vector<std::uint64_t> targets; ///< TargetSelector.targets (helper requests)
    int target_kind{-1};                ///< TargetSelector.targetType (ETargetType: 0/1 enemies, 2..5 own side)
    std::uint64_t target_monster{};     ///< TargetSelector.CurrentTarget (ally or enemy)
    /// The shown cursor's SelectionView (command menu, or the target selector while targeting):
    /// dTimeAcc (s since the selection last changed, drives the arrow sway) and its lock
    /// The action being targeted (TargetSelector.Action): its ActionDamage element and damage
    /// type as an element (5 physical, 6 magical); -1 = no damage action
    int target_element{-1}, target_type{-1};
    /// CombatResult as it stands mid-battle (CombatController+0x98); the score the result screen
    /// will credit is computed from these by CalculateResult
    struct Score {
        bool valid{};
        int turns{}, acted{};       ///< TurnsPlayed, MonsterActed (this turn)
        float health{1};            ///< MonsterHealthinessPercent (running mean, 0..1)
        float pre{}, exec{};        ///< PreExecutionDamage, ExecutionDamage
        float weakness{};           ///< WeaknessExploitCount
        int combo{};                ///< ComboCounter (the score's, not the on-screen combo)
        int levels{}, actions{};    ///< totalLevelCount, totalActionsCount
        float dot{};                ///< DotDamageCount (enemy poison / burn / congeal ticks)
        bool champion{};            ///< MonsterEncounter.IsChampion (EncounterType 1 or 3)
        int active{-1};             ///< PlayerController.Monsters.Active count (-1 = not read)
        /// An enemy as CalculateResult weighs it (CombatController.Enemies): Level, DifficultyRating
        /// (+0x6C), shifted, currentStats.DifficultyRatingBonus, BuffManager total debuff strength
        struct Foe {
            int level{}, rating{};
            bool shifted{};
            float bonus{}, debuffs{};
        };
        std::vector<Foe> foes;
        std::vector<float> ally_buffs; ///< total buff strength per CombatController.PlayerMonsters
        /// CalculateResult has run (StarsGained 1..6): the result screen's own figures replace the
        /// live projection (EnemyDifficulty +0x10, BuffPoints +0x48, ExecutionPercent +0x3C, and
        /// MonsterHealthinessPercent +0x24 written back squared)
        bool final{};
        int final_difficulty{}, final_buffs{};
        float final_exec{};
    } score;
    float sel_time{-1};
    bool sel_locked{};
};

struct MapIcon {
    std::string sprite; ///< native UI collection name
    float x{}, y{};     ///< room-local cell
    std::string label;  ///< MapIconData.DisplayName: the native map's name for it ("Monster Farm")
};

struct Room {
    float x{}, y{}, w{}, h{}; ///< MapData.Position / Size (map cells)
    int explored{};          ///< explored cells
    int cells{};             ///< cells in Explored[]
    bool teleporter{};
    float teleporter_x{}, teleporter_y{};
    std::vector<MapIcon> icons; ///< MapData.MapIcons, authored order (last at a cell wins)
    std::vector<std::uint8_t> revealed; ///< MinimapEntry.Explored, x + y * width
    std::vector<int> markers; ///< MinimapCustomIcons, same cell order; >0 overrides authored icon
    bool current{};
    std::string scene;
    int area{-1};             ///< index into MapInfo::area_names (MapData.MapArea)
};

/// MapMenu.AreaExplorationProgress: the game's own per-area tile counts (filled when the
/// native map menu has been opened once this session).
struct AreaProgress {
    std::string name;
    float explored{}, unexplored{};
};

struct Teleporter {
    std::string name;
    std::string scene; ///< exact MapData.SceneName; distinguishes crystals in the same area
    std::string label; ///< the native map label of a named place in its room ("Monster Farm"), if any
    std::uint64_t ptr{};  ///< TeleportSelectable
    bool start{};         ///< MapMenu.StartTeleport (where the player stands)
};

struct MapInfo {
    bool valid{};
    bool teleport_menu{};               ///< native map open at a teleporter (openType Teleporting)
    std::vector<Teleporter> teleports;  ///< MapMenu.teleports while it is
    std::vector<int> obstacles; ///< uncleared unconditional destructibles, current visited room only
    std::string scene;        ///< current room's MapData.SceneName
    int explored_cells{};     ///< over all MinimapEntries
    int total_cells{};
    std::vector<Room> rooms;  ///< every entry (bounded)
    std::vector<std::string> area_names;  ///< MapArea.Name per area seen in rooms
    std::vector<std::uint32_t> area_colors; ///< MapArea.MinimapColor as 0xRRGGBB
    std::vector<AreaProgress> progress;   ///< the native map menu's areaInfos (may be empty)
    bool player{};                        ///< player_x/y valid (inside the current room)
    float player_x{}, player_y{};         ///< MinimapManager.PlayerPosition, map cells (offset applied)
    std::string completion_text;          ///< MapMenu.AreaPercentText as rendered
    int completion10{-1};                 ///< its percentage in tenths (-1 = not read)
};

/// An InventoryItem of InventoryManager.Weapons / .Accessories (equipment picker).
struct InvEquip {
    std::string name;
    int quantity{};
    std::uint64_t ptr{}; ///< the InventoryItem
    std::uint64_t item{}; ///< InventoryItem.Item: the shared item object (equipped slots point at it)
    /// Owned monsters (team, bench, farm) with this same item equipped
    struct Owner {
        std::uint64_t monster{};
        std::string name, sprite;
    };
    std::vector<Owner> equipped_on;
};

struct Model {
    State state{State::Unknown};
    int game_state{-1};
    bool menu_open{};
    bool dialogue{};
    int gold{-1};
    std::string player;
    std::vector<PartyMonster> party; ///< MonsterManager.Active (the team, at most 6 + spare)
    int follower{-1};                ///< index into party of PlayerFollower.Monster (-1 = not in party)
    PartyMonster follower_mon;       ///< PlayerFollower.Monster itself (name empty = none)
    int bench{-1};                   ///< MonsterManager.Inactive count (-1 = not read)
    Battle battle;
    MapInfo map;
    std::vector<InvEquip> weapons, accessories;
    /// Title screen: MainMenu.state (0 PressAnyButton .. 5 MainMenu, -1 = not read), the save
    /// slot menu open, and the version text the title shows (UIController.VersionString)
    int title_stage{-1};
    bool save_menu{};
    std::string version;
    /// The title's own "Press [A] to start" prompt object is shown (the helper finds the
    /// GameObject the game shows; GameObject.Find returns only active objects): 1 shown, 0 not,
    /// -1 not known (no helper)
    int title_prompt{-1};
    /// the prompt's FlashRenderer (shows its target while ((++counter & mask) == 0), counter
    /// +0x20, mask +0x24 = 32: 32 frames on, 32 off); mask 0 = not found
    int title_flash_counter{}, title_flash_mask{};
};

/// The command the battle page's rail highlights: the native cursor's row while it is on the
/// rail, else the command whose submenu is open (Items / Switch Monster / Monster Info), else
/// Attack (skills, targeting). An index into Battle::defaults, -1 = none.
int ActiveCommand(const Battle& battle);

} // namespace ms
