// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
// FINAL FANTASY RESONANCE companion: read-only game state (UE 5.6 object array + reflection-derived offsets).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace ffr {

struct UnitState {
    int id = 0, xp = 0, hp = 0, mp = 0, vision = -1, weapon = -1, armor = -1, join = 0;
    bool usable = false;
    std::vector<int> accessories;
    std::vector<int> skills;  // equipped skills (SaveData_UnitParameter.equipSkillList), -1 = empty slot
    // From the live unit parameter object when one is found (-1 = unknown).
    int level = -1, max_hp = -1, max_mp = -1;
    int stats[8] = {-1, -1, -1, -1, -1, -1, -1, -1};  // atk def int mnd div agi (+2 spare)
};

struct EquipInfo {
    int type = 0, detail = 0;  // eEquipmentType, eEquipmentDetailType
    int params[21] = {};       // eUnitParameterType -> bonus
    std::vector<int> passives; // passive skills the piece grants (EQUIPMENT_ITEM_DATA.passiveList)
};

struct ItemStack {
    int id = 0, num = 0;
};

// A consumable as the game's item menus treat it: CONSUMABLES_ITEM_DATA and its skill's SKILL_DATA
struct ConsumableInfo {
    enum Kind { kOther, kHp, kMp, kRevive };
    bool field = false;  // usableScene MapOnly or Any: the menu can use it outside a battle
    int skill = -1;
    int kind = kOther;   // the skill's parameterType (HitPoint, MagicPoint), or a revive (its defaultTargetState Dead)
    bool party = false;  // TargetType Group: the whole party at once
};

// A quest as the game's journal shows it (CPP_QuestSubsystem.QuestDataMap: QuestData)
struct Quest {
    int id = 0, cat = 0, prog = 0, priority = 0;  // EQuestCategory (1 main, 2 sub, 3 mini), EQuestProgression (1 start, 2 progress, 3 complete)
    bool can_track = false, tracked = false;      // QuestProperty.bCanPreferred; the game's tracked quest (PreferredQuestId)
    std::string name, subtitle, place, desc;      // Name, Subtitle, DestinationTitle, Description
    std::vector<std::string> log;                 // SummaryList: what has happened so far, oldest first
    // A Name the game has not looked up yet (a string table entry without its display string): the table's namespace and the
    // entry's source text (Japanese), for the module to look up in the game's own localization files
    std::string name_ns, name_src;
};

// A point of interest the game's minimap shows (a shop, the inn, a save point), in land coordinates.
struct MapIcon {
    double x = 0, y = 0;
    std::string icon;  // UI texture under Icon/MAP/, e.g. "ICON_MAP_L_Shop"
};

// One island of the world map: its capture (MapCapture/Wld/Wld_<n>) and the square of land it covers.
struct Island {
    int area = 0;
    std::string texture;
    double wx = 0, wy = 0, bw = 0, bh = 0;
};

// A place the world map shows (CPP_UI_WldMapSubsystem.FldObjDataMap: the known ones), in land coordinates.
struct Landmark {
    int key = 0, map_id = 0;  // the field object's id, the map it leads to (RelatedMapId)
    double x = 0, y = 0;
    bool visited = false;     // SaveData_PerMap.bIsVisited
    int type = -1;            // MAP_DATA.MapType (eMapType: 0 town, 1 dungeon, 2 field, 3 underworld, 4 shrine)
    int landing = -1;         // StaticFieldObjectData.LandingPointId (known once the world map's own table is filled)
    std::string name;
    std::vector<int> ids;     // every map id that is this place (its landmark row's variants and their group): the game's list may
                              // name it by another (Kol's neighbour: 2010 here, 2011 in the list)
    // The game's own rules (DA_WldMapParameter): no fast travel to a place not yet visited. Its map-type rule is about where the
    // player is (no fast travel out of a dungeon), and it may refuse for a story event or a locked map: its own dialog says so.
    bool CanTravel() const { return visited; }
};

// The game's world map menu (VM_WldMap, its destination list, the dialog layer), for driving a fast travel.
struct WorldMenu {
    bool found = false;        // the menu's objects exist (they are made the first time the map opens)
    bool list_open = false;    // the destination list is up (SceneTag UI.WldMap.Scene.Location)
    bool category = false;     // a list category is set: the map is open at some point since the last close (None when closed)
    bool dialog = false;       // a dialog is up (CPP_DialogManager.DialogWidget)
    int focus = -1;            // the focused place's map id (FocusObjectData.RelatedMapId)
    std::vector<int> list;     // the list's places in this category, in order (DisplayLandmarkIdList: map ids)
    bool can_travel = false;   // the focused place: the game would travel (FastTravelBehavior.bCanTravel)
    std::string travel_dialog; // the dialog it shows for it (FastTravelBehavior.DialogTag: ...Map.FastTravel, or ...CanNot.<why>)
    int main_menu = 0;         // the game's main menu scenes open (CPP_VM_MainMenu.SceneDataStack: Root, Party, ...), 0 closed
};

// Who has the game's controls (WBP_OverAllLayout, a CPP_EU_WBP_GameLayout: CurrentInputTargetLayer and its layer stacks). In the
// field no layer takes input and the Event layer is empty; a menu, the world map, a shop or the save screen takes input on
// UI.Layer.System (.Front1 ...), a conversation or a story scene on UI.Layer.Event.
struct UiState {
    bool valid = false;
    std::string input;      // the layer with input ("" none: the field), e.g. "UI.Layer.System"
    bool event = false;     // the Event layer holds something (a conversation, a story scene)
    bool save = false;      // the Save layer holds something
    bool Free() const { return valid && input.empty() && !event && !save; }
    bool Quiet() const { return valid && !event && !save; }  // a menu may be open, but no event or save runs
};

struct MapState {
    bool valid = false;
    int map_id = -1, area_id = -1;
    double px = 0, py = 0, pz = 0, yaw = 0;
    std::string name;     // the map's English name, from the game's map table
    std::string texture;  // container path of the area's capture, "" when unknown
    double wx = 0, wy = 0, bw = 0, bh = 0;  // capture world location and box size
    bool world = false;           // the world map (its islands), not one area's capture
    std::vector<Island> islands;  // world: every island with a capture
    std::vector<Island> world_islands;  // the world map's islands, read in any map (the world view from a town)
    std::vector<Landmark> landmarks;    // the places the world map shows
    int map_type = -1;                  // this map's MAP_DATA.MapType (1 dungeon: the game allows no fast travel from it)
    int here = -1;                      // the landmark of the town or dungeon the player is in (index), -1 none or on the world map
    std::vector<MapIcon> icons;
    // The objectives in this map: minimap entries whose quests (RelatedQuestIdList) are in progress, and the one the game marks as
    // the quest target. main: a main-story quest's (or the marked one when its quests are unknown); side: a side quest's. One
    // place can be both (a town the story and a side quest both lead to).
    struct Goal {
        double x = 0, y = 0;
        bool main = true, side = false;
    };
    std::vector<Goal> goals;
};

// The battle command menu as the game shows it (CPP_VM_BtlCmd): the open list and its cursor, or the target choice.
struct ActionRef {
    int cat = 0, id = 0;  // eActionCategory (0 none: a command list, 1 skill, 2 item) and its id
    bool operator==(const ActionRef& o) const { return cat == o.cat && id == o.id; }
};
struct BattleMenu {
    bool valid = false;  // the reads check out: the page shows it and taps may drive it
    bool open = false;   // the game is showing the command window
    int state = 0;       // 0 a list, 1 choosing a target (the path ends in an action: a skill, Attack, an item), 2 the resonance wheel
    std::vector<ActionRef> entries;
    int cursor = -1;
    std::vector<ActionRef> path;  // the choices that led to this list (Skills > Fire)
    std::vector<uint64_t> targets;  // the CPP_BtlUnits under the game's target pointer (CPP_VM_BtlUnitParam.TargetSelectState Main)
    std::vector<uint64_t> sub;      // the others the action reaches (TargetSelectState Sub: an area attack)
    int actor = 0;                  // the unit whose command it is (CPP_BtlUnitManager.m_pCmdSelectUnit; unit id, 0 unknown)
    ActionRef focus;                // the game's focused entry (FocusCmdData): the open list holds it at the cursor
    // state 2: the resonance wheel (the game's Finish command, CPP_VM_FinishCmd.EntryDataList): one choice per d-pad direction
    struct Finish {
        bool enable = false;
        int skill = 0, vision = 0, uid = 0;
        int dir = -1;       // EUINavigation: 0 left, 1 right, 2 up, 3 down
        uint64_t unit = 0;  // the CPP_BtlUnit of UnitUniqueId (0 unknown)
        bool operator==(const Finish& o) const { return enable == o.enable && skill == o.skill && vision == o.vision && uid == o.uid && dir == o.dir && unit == o.unit; }
    };
    std::vector<Finish> finish;
    uint64_t vm = 0, list = 0, units = 0;  // CPP_VM_BtlCmd, the open CPP_BtlCmdList*, CPP_BtlUnitManager (RefreshMenu)
};

struct BattleUnit {
    int id = 0;
    bool enemy = false;
    int hp = -1, max_hp = -1, mp = -1, max_mp = -1;
    std::string name;
    uint64_t param = 0;  // the live CPP_BtlUnitParameter (Reader::RefreshBattle re-reads HP/MP from it)
    uint64_t unit = 0;   // the CPP_BtlUnit (the command menu's targets point at it)
    int bp = -1, max_bp = -1;  // the break gauge; an enemy whose gauge is empty is staggered
    bool staggered = false;
    uint64_t owner = 0;  // a vision: the CPP_BtlUnit it is equipped to (it fights beside that character)
};

struct GameState {
    bool in_game = false;  // a field (or card game) game mode is running: not the title or boot screens
    bool save_ok = false;
    int money = 0, difficulty = 0;
    float play_time = 0;
    std::vector<int> party;
    int guest = 0;  // a temporary ally who fights beside the party (SaveData_Party.guestUnitId), 0 none
    std::vector<UnitState> units;
    std::vector<ItemStack> inventory;
    MapState map;
    bool in_battle = false;
    std::vector<BattleUnit> battle;
    BattleMenu menu;
    std::string status;  // diagnostic
    std::unordered_map<int, EquipInfo> equipment;  // from the game's equipment table (static)
    std::unordered_map<int, ConsumableInfo> consumables;  // from the game's consumables and skill tables (static)
    std::vector<Quest> quests;  // the journal: the game's quests (read about once a second)
    int tracked_quest = -1;     // CPP_QuestSubsystem.PreferredQuestId
    uint64_t Hash() const;
};

class Guest {
public:
    explicit Guest(const EdenDsmodHostApi* h = nullptr) : h_(h) {}
    void Set(const EdenDsmodHostApi* h) { h_ = h; }
    bool Read(uint64_t a, void* out, size_t n) const {
        return h_ && a && h_->read_memory(h_->userdata, a, out, n) == EDEN_DSMOD_TRUE;
    }
    template <class T> T Get(uint64_t a, T def = T{}) const {
        T v;
        return Read(a, &v, sizeof v) ? v : def;
    }
    const EdenDsmodHostApi* host() const { return h_; }

private:
    const EdenDsmodHostApi* h_;
};

class Reader {
public:
    // Per-build roots; false when this build is unknown or the roots do not validate.
    bool Init(const EdenDsmodHostApi* host, const char* build_hex, std::string& why);
    void Update(const EdenDsmodHostApi* host, GameState& out);
    // Between full updates in a battle: re-read only HP/MP of the units Update found (a few small reads).
    // False when a unit no longer checks out (the next full update rebuilds the list).
    bool RefreshBattle(const EdenDsmodHostApi* host, GameState& s);
    // The command menu's cursor and state between full updates (a few reads); false when it no longer checks out.
    bool RefreshMenu(const EdenDsmodHostApi* host, GameState& s);
    // Every tick: only the player's position and heading (a handful of reads), for the live map marker.
    bool ReadPlayer(const EdenDsmodHostApi* host, MapState& m);
    // The world map menu (a few reads). scan: when its objects are not known yet, look for them (at most every 700 ms; they are
    // made the first time the map opens). False when they are not found.
    bool ReadWorldMenu(const EdenDsmodHostApi* host, WorldMenu& w, bool scan);
    int PlayerMapId(const EdenDsmodHostApi* host);  // the map the player is in (-1 unknown)
    // DA_WldMapParameter and its class, for the guest helper's fast travel (the game's rules read it); false while not found
    bool TravelAsset(const EdenDsmodHostApi* host, uint64_t& da, uint64_t& cls);
    // DA_UI_SkillAction (required) and DA_UI_SkillTargeting (0 when not found), for the guest helper's item use
    bool ItemAssets(const EdenDsmodHostApi* host, uint64_t& action, uint64_t& action_cls, uint64_t& target, uint64_t& target_cls);
    // CPP_QuestSubsystem and its class, for the guest helper's quest tracking
    bool QuestSystem(const EdenDsmodHostApi* host, uint64_t& sub, uint64_t& cls);
    bool ReadUi(const EdenDsmodHostApi* host, UiState& u);  // a dozen reads; false when the layout is not found
    std::string Name(uint32_t id);
    // The FNames of these strings (0: not in the pool), looked up a block of the name pool per call (128 KiB); true when done
    // (all found, or the pool searched). want must stay the same between calls.
    bool FindNames(const EdenDsmodHostApi* host, const std::vector<std::string>& want, std::vector<uint32_t>& ids);
    std::string Debug();  // the battle command objects found (development logging)

private:
    struct Obj {
        uint64_t addr = 0, cls = 0;
        uint32_t index = 0;
    };
    void Scan();
    bool Alive(const Obj& o) const;
    std::string ClassName(uint64_t cls);
    void ReadSave(GameState& s);
    void ReadMap(GameState& s);
    void ReadBattle(GameState& s);
    void ReadMenu(GameState& s);
    void ReadMenuHead(GameState& s);  // open, focus, cursor, path, actor, targets (a few reads)
    void FindList(BattleMenu& mn);    // the open list: a pass over the object array for this menu's lists
    bool ListMatches(const BattleMenu& mn);
    uint64_t last_find_ms_ = 0, menu_list_ = 0;
    std::unordered_map<uint64_t, bool> list_class_;
    std::unordered_map<uint64_t, uint64_t> unit_vms_;  // CPP_BtlUnitParameter -> its CPP_VM_BtlUnitParam
    std::unordered_map<uint64_t, bool> unit_vm_class_;
    uint64_t last_vm_find_ms_ = 0;
    void FindUnitVms(const GameState& s);
    Obj finish_vm_;  // CPP_VM_FinishCmd (made when the resonance wheel first opens)
    std::unordered_map<uint64_t, bool> finish_class_;
    uint64_t last_finish_find_ms_ = 0;
    void ReadFinish(GameState& s);
    void ReadIcons(MapState& m);
    void ReadLandmarks(MapState& m);
    struct MapRow {
        std::string name;
        int type = -1, group = -1, same = -1;
        bool found = false;
        uint64_t name_ms = 0;  // when the name was last looked up (the game resolves some texts late)
        uint64_t row = 0;
    };
    void LoadMapRows();  // every map's group, same-map and type, once the tables are loaded
    bool rows_loaded_ = false;
    std::vector<int> Mates(int id);  // the maps that are the same place: its group or same-map id
    int last_here_ = -1;  // the last place found for the player (map id): kept inside a building or a dungeon floor without one
    Obj lm_list_;          // DA_LandmarkList (always loaded): the places when the world map's own table is empty
    const MapRow& MapInfo(int id);  // the game's map table row (MAP_DATA), cached once found
    std::unordered_map<int, MapRow> map_rows_;
    Obj wld_sub_, wld_vm_, wld_list_, dialogs_, main_vm_;  // (main_vm_: CPP_VM_MainMenu)
    Obj widgets_;                                          // CPP_GameWidgetManagerSubsystem (its PrimaryGameLayout +0x30)
    Obj wld_param_;                                        // DA_WldMapParameter (always loaded)
    Obj skill_action_, skill_targeting_;                   // DA_UI_SkillAction, DA_UI_SkillTargeting (always loaded)
    void LoadConsumables();
    bool consumables_loaded_ = false;
    std::unordered_map<int, ConsumableInfo> consumables_;
    uint64_t layout_ = 0;                                  // the layout the layer table was read from
    std::vector<std::pair<std::string, uint64_t>> layers_; // its Layers: tag, widget stack  // CPP_UI_WldMapSubsystem, VM_WldMap, VM_WldMapLandmarkList, CPP_DialogManager
    uint64_t last_wm_scan_ms_ = 0;
    // The quests the game holds (CPP_QuestSubsystem.QuestDataMap): id -> category (EQuestCategory) and progression, ~once a second
    struct QuestInfo {
        uint8_t cat = 0, prog = 0;
    };
    void ReadQuests();
    std::unordered_map<int, QuestInfo> quests_;
    uint64_t last_quest_ms_ = 0;
    void Heading(MapState& m, double yaw_fallback);
    uint64_t Resolve(int32_t index, int32_t serial);  // FWeakObjectPtr -> UObject*
    std::string SoftPath(uint64_t soft);

    Guest g_;
    uint64_t main_ = 0, blocks_ = 0, objarr_ = 0;
    std::unordered_map<uint32_t, std::string> names_;
    uint32_t find_block_ = 0;
    std::vector<uint8_t> find_buf_;
    std::unordered_map<uint64_t, std::string> class_names_;
    void ReadUnitParams(GameState& s);
    void LoadEquipment();
    // Visions add to the stats through their mastery tables: passive skills and flat parameters unlocked by the vision's
    // awakening points, and by the synchro points of the character wearing it. Passives raise a parameter by a percentage.
    struct MasteryRow {
        int pts = 0;
        std::vector<int> passives;
        std::vector<std::pair<int, int>> flats;  // eUnitParameterType, value
    };
    struct VisionPoints {
        int awakening = 0;
        std::unordered_map<int, int> synchro;  // unit id -> points
    };
    void LoadSkillTables();
    bool skills_loaded_ = false;
    std::unordered_map<int, std::vector<MasteryRow>> awakening_, synchro_;    // vision id -> rows
    std::unordered_map<int, std::vector<std::pair<int, int>>> passive_pct_;  // passive id -> (eUnitParameterType, percent)
    std::unordered_map<int, VisionPoints> vision_points_;                     // from the save, every update
    bool IsGameplayMode(uint64_t cls);
    bool IsA(uint64_t cls, const char* base);  // the class or one of its ancestors is named base
    std::map<std::pair<uint64_t, std::string>, bool> isa_;
    std::unordered_map<uint64_t, bool> mode_classes_;
    Obj save_mgr_, quest_sub_, minimap_, battle_, controller_, assets_, game_mode_, cmd_vm_, cmd_mgr_;
    std::vector<Obj> unit_params_, map_tables_, cmd_lists_;
    std::unordered_map<int, std::string> map_names_;
    std::string MapName(int id);
    std::string FTextString(uint64_t ftext, int max_chars = 256);
    bool StringTableSource(uint64_t ftext, std::string& ns, std::string& src);
    std::string FStringAt(uint64_t fstring, int max_chars);
    void ReadQuestList(GameState& s);
    std::vector<Quest> quest_list_;
    uint64_t last_quest_list_ms_ = 0;
    struct QuestTexts { int prog = -1, summaries = -1; uint64_t read_ms = 0; Quest q; };
    std::unordered_map<int, QuestTexts> quest_texts_;
    std::unordered_map<std::string, Obj> string_tables_;  // the loaded UStringTable objects by name (ST_EventQuestTitle)
    std::unordered_map<int, EquipInfo> equipment_;
    bool equipment_loaded_ = false;
    uint64_t last_scan_ms_ = 0;
    double last_px_ = 0, last_py_ = 0, heading_ = 0;  // the movement heading (ReadMap)
    bool have_pos_ = false, have_heading_ = false, was_battle_ = false;
    int last_map_ = -1;
    uint32_t scans_ = 0;
};

} // namespace ffr
