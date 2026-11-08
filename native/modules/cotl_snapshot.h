// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Game state as a snapshot, read from guest memory.
#pragma once

#include <array>
#include <map>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cotl_actions.h"
#include "cotl_dungeon_map.h"
#include "cotl_follower_portrait.h"

#include "cotl_guest.h"

namespace cotl {

enum class FollowerGroup : uint8_t { Normal = 0, Missionary = 1, Prisoner = 2 };

// FollowerInfo.Thoughts: thought (Thought), faith modifier, stacked count
struct ThoughtEntry {
    int32_t type = 0;
    float modifier = 0;
    int32_t quantity = 1;
};

struct Follower {
    int32_t id = 0;
    std::string name;
    int32_t level = 0;
    int32_t age = 0;
    int32_t life_expectancy = 0;
    int32_t role = 0;
    int32_t cursed_state = 0;
    int32_t dwelling_id = 0;
    bool old_age = false;
    std::vector<int32_t> traits;
    std::vector<ThoughtEntry> thoughts;
    // Needs 0-100 (FollowerInfo setters clamp to [0, 100])
    float satiation = 0, starvation = 0, faith = 0, happiness = 0, illness = 0, injured = 0,
          adoration = 0, // loyalty, 0-100 (FollowerBrainStats.get_MAX_ADORATION)
          exhaustion = 0, dissent = 0;
    FollowerGroup group = FollowerGroup::Normal;
    std::string skin;            // FollowerInfo.SkinName, e.g. "Cat2"
    int32_t skin_colour = 0;     // FollowerInfo.SkinColour: colour set of the kind
    std::vector<SlotTint> tints; // slot colours from WorshipperData (empty if not loaded)
};

// WorshipperData does not change in the game: Skin → SkinAndData and (skin, colour set) → slot colours
// are read once per instance.
struct SkinColourCache {
    uint64_t instance = 0;
    std::map<std::string, uint64_t, std::less<>> skin_data;
    std::map<std::pair<std::string, int32_t>, std::vector<SlotTint>> tints;
};

struct Objective {
    int32_t type = 0;
    int32_t follower_id = -1;
    int32_t item_type = -1; // -1 = no collect quest
    float expire = 0;        // ExpireTimestamp, game minutes since the start of the game
    bool has_expiry = false; // QuestExpireDuration > 0
    std::string group_id;    // GroupId (+0x18), I2 term of the title
    std::string unique_group; // UniqueGroupID (+0x40): sub-items of a quest
    // Subclass fields +0x78, +0x7C, +0x80, +0x84, +0x88 as int; meaning per Type, see cotl_quests.cpp
    std::array<int32_t, 5> ints{};
    std::string str2; // second string: @0x88 RECRUIT_CURSED FollowerName, @0x90 FIND_FOLLOWER name
    std::string str78; // string @0x78 (BED_REST FollowerName, GIVE_ITEM NPCTerm, KNUCKLEBONES CharacterNameTerm)
    bool complete = false;
    bool failed = false;
};

// Values for the equipment cards; negative: game data not found
struct GearData {
    float weapon_avg = -1, weapon_speed = -1, curse_damage = -1, relic_charge = -1;
    float dm_bonus = 0, fleece_damage = 0, relic_mult = 0, relic_scale = 1, difficulty = 1;
    int32_t fleece = 0, phase = 0;
    bool relic_double = false;
};

// Dungeon run of player 1 (PlayerFarming.Instance). Hearts in half hearts as in the game.
struct Run {
    float hp_red = 0, hp_red_max = 0, hp_blue = 0, hp_black = 0, hp_spirit = 0, hp_spirit_max = 0,
          hp_fire = 0, hp_ice = 0;
    int32_t weapon = 0, weapon_lvl = 0, curse = 0, curse_lvl = 0, relic = 0;
    std::vector<std::pair<int32_t, int32_t>> tarot;
    std::vector<Room> rooms; // BiomeGenerator.Rooms of the current floor
    int32_t cur_x = 0, cur_y = 0;
    std::optional<GearData> gear; // only read while an equipment window is open
};

struct AnimalInfo {
    int32_t type = 0; // ITEM_TYPE
    int32_t id = 0;
    std::string name; // empty: unnamed
};

struct GameSnapshot {
    bool save_loaded = false; // SaveAndLoad.Loaded: false at first title screen, stays set after quitting
    bool at_title = false;    // UIMainMenuController.Instance set
    int32_t day = 0;  // DataManager.CurrentDayIndex
    float time = 0;   // DataManager.CurrentGameTime, game minutes in the day (0-1200)
    std::vector<Follower> followers;
    std::vector<Objective> objectives;              // Task 6
    std::vector<int32_t> offered_quest_follower_ids; // Task 6
    std::vector<std::pair<int32_t, int32_t>> items; // Task 7: (ITEM_TYPE, quantity)
    // Only read when an open quest needs them (quest texts)
    std::vector<std::pair<int32_t, int32_t>> enemies_killed; // (Enemy, count)
    std::vector<std::pair<int32_t, int32_t>> offerings;      // (ITEM_TYPE, quantity) BlizzardOfferings
    std::vector<AnimalInfo> animals;
    std::vector<int32_t> legendary_known; // EquipmentType
    int32_t sword_return_day = 0;
    std::string cult_name;  // DataManager.CultName
    int32_t cult_level = 0; // DataManager.CurrentCultLevel (One = 0)
    // Silos in the cult: sum of contents and capacities (Structures_Silo*.get_Capacity)
    int32_t seed_stored = 0, seed_capacity = 0;
    int32_t fertiliser_stored = 0, fertiliser_capacity = 0;
    // Dirt in the base (BaseStructures): poop piles, vomit, corpses
    int32_t poop = 0, vomit = 0, corpses = 0;
    std::optional<Run> run; // only during a dungeon run
    // Only for the selected follower (thoughts_for from ReadSnapshot), so only with the window open
    std::optional<FollowerCommandState> command_state;
};

// GameManager.IsDungeon without None (-1): locations where a run is in progress.
bool IsDungeonLocation(int32_t location);

// nullopt while DataManager.instance is missing or the follower list changes while
// being read (torn-read protection).
// Read thoughts only for this follower (many reads, only the Thoughts window shows them)
inline constexpr int32_t kAllThoughts = -2;
inline constexpr int32_t kNoThoughts = -1;
std::optional<GameSnapshot> ReadSnapshot(const Guest& g, uint64_t main_base,
                                         int32_t thoughts_for = kAllThoughts, bool read_gear = false,
                                         SkinColourCache* colours = nullptr);

} // namespace cotl
