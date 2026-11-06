// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include "cotl_fake_game.h"
#include "cotl_snapshot.h"

using cotl::FollowerGroup;
using cotl::ReadSnapshot;

TEST(snapshot_reads_followers) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble", 4, 80, 65, 72, 20, 37, {3, 9}}, {8, "Okra", 6}});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->followers.size() == 2);
    const auto& f = s->followers[0];
    CHECK_EQ(f.id, 7);
    CHECK_EQ(f.name, std::string("Bramble"));
    CHECK_EQ(f.level, 4);
    CHECK_EQ(f.satiation, 80.0f);
    CHECK_EQ(f.faith, 65.0f);
    CHECK_EQ(f.illness, 72.0f);
    CHECK_EQ(f.happiness, 20.0f);
    CHECK_EQ(f.adoration, 37.0f);
    CHECK_EQ(f.traits.size(), size_t{2});
    CHECK_EQ(s->followers[1].name, std::string("Okra"));
}

TEST(snapshot_marks_missionaries_and_prisoners) {
    fake::Game game;
    game.SetFollowers({{1, "A"}, {2, "B"}, {3, "C"}});
    game.SetIdList(0xDD8, {2}); // Missionary
    game.SetIdList(0xDC8, {3}); // Prisoner
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->followers.size() == 3);
    CHECK(s->followers[0].group == FollowerGroup::Normal);
    CHECK(s->followers[1].group == FollowerGroup::Missionary);
    CHECK(s->followers[2].group == FollowerGroup::Prisoner);
}

TEST(snapshot_without_data_manager_is_empty) {
    fake::Game game;
    game.g.PutU64(fake::kDataManagerStatics + 0xE0, 0);
    CHECK(!ReadSnapshot(game.g, fake::kMain).has_value());
}

TEST(snapshot_rejects_list_changed_while_reading) {
    fake::Game game;
    game.SetFollowers({{1, "A"}, {2, "B"}});
    bool mutated = false;
    game.g.on_read = [&](uint64_t addr) {
        // As soon as the first follower object is read, a follower is added
        if (!mutated && addr >= fake::Game::FollowerObj(0) && addr < fake::Game::FollowerObj(0) + 0x1000) {
            mutated = true;
            game.g.PutS32(0x180000 + 0x18, 1);
        }
    };
    CHECK(!ReadSnapshot(game.g, fake::kMain).has_value());
}

// Many followers
TEST(snapshot_reads_sixty_followers) {
    fake::Game game;
    std::vector<fake::FollowerSpec> fs;
    for (int i = 0; i < 60; ++i) {
        fs.push_back({i + 1, "F" + std::to_string(i)});
    }
    game.SetFollowers(fs);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK_EQ(s->followers.size(), size_t{60});
    CHECK_EQ(s->followers[59].name, std::string("F59"));
}

TEST(snapshot_reads_day_and_time) {
    fake::Game game;
    game.SetDayTime(10, 600.5f);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK_EQ(s->day, 10);
    CHECK_EQ(s->time, 600.5f);
}

TEST(snapshot_reads_objectives) {
    fake::Game game;
    game.SetObjectives({
        {1, 7, 2400, 15000, false, false, 42}, // collect quest for follower 7
        {5, -1},                               // story quest without a follower
        {3, 8, 0, 0, true},                    // completed
    });
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->objectives.size() == 3);
    const auto& q = s->objectives[0];
    CHECK_EQ(q.type, 1);
    CHECK_EQ(q.follower_id, 7);
    CHECK(q.has_expiry);
    CHECK_EQ(q.expire, 15000.0f);
    CHECK_EQ(q.item_type, 42);
    CHECK_EQ(s->objectives[1].follower_id, -1);
    CHECK(!s->objectives[1].has_expiry);
    CHECK_EQ(s->objectives[1].item_type, -1); // no COLLECT_ITEM
    CHECK(s->objectives[2].complete);
}

// Objectives_GiveItem (e.g. "Seeking Sozo"): TargetType @0x88
TEST(snapshot_reads_give_item_target_type) {
    fake::Game game;
    fake::Game::ObjectiveSpec give{33, -1};
    give.ints = {0, 0, 3, 0, 29};
    game.SetObjectives({give});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->objectives.size() == 1);
    CHECK_EQ(s->objectives[0].item_type, 29);
}

// The NPC term of GIVE_ITEM and the opponent of DEFEAT_KNUCKLEBONES are strings @0x78
TEST(snapshot_reads_npc_term_of_give_item_and_knucklebones) {
    fake::Game game;
    fake::Game::ObjectiveSpec give{33, -1};
    give.str78 = "Sozo";
    fake::Game::ObjectiveSpec knuckle{24, -1};
    knuckle.str78 = "Ratau";
    game.SetObjectives({give, knuckle});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->objectives.size() == 2);
    CHECK_EQ(s->objectives[0].str78, std::string("Sozo"));
    CHECK_EQ(s->objectives[1].str78, std::string("Ratau"));
}

TEST(snapshot_reads_offered_quests) {
    fake::Game game;
    game.SetFollowers({{1, "Asks"}, {2, "Hungry"}, {3, "Working"}});
    game.SetBrains({{0, true, 8}, {1, true, 0}, {2, false}});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->offered_quest_follower_ids.size() == 1);
    CHECK_EQ(s->offered_quest_follower_ids[0], 1);
}

// FollowerBrain is initialised late: until then the TypeInfo slot only holds a token
TEST(snapshot_without_brains_has_no_offers) {
    fake::Game game;
    game.SetFollowers({{1, "A"}});
    game.g.PutU64(fake::kMain + 0x713C068, 0x2000C489);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK(s->offered_quest_follower_ids.empty());
}

TEST(snapshot_reads_quest_text_fields) {
    fake::Game game;
    fake::Game::ObjectiveSpec collect{1, 7};
    collect.group_id = "Objectives/GroupTitles/Quest";
    collect.ints = {12, 0, 5, 0, 3};
    fake::Game::ObjectiveSpec bed{15, -1};
    bed.str78 = "Gremer";
    game.SetObjectives({collect, bed});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->objectives.size() == 2);
    CHECK_EQ(s->objectives[0].group_id, std::string("Objectives/GroupTitles/Quest"));
    CHECK_EQ(s->objectives[0].ints[0], 12);
    CHECK_EQ(s->objectives[0].ints[2], 5);
    CHECK_EQ(s->objectives[0].ints[4], 3);
    CHECK_EQ(s->objectives[1].str78, std::string("Gremer"));
}

TEST(snapshot_reads_thoughts) {
    fake::Game game;
    game.SetFollowers({{1, "A"}});
    const uint64_t f = fake::Game::FollowerObj(0);
    // FollowerInfo.Thoughts @0x120 → List<ThoughtData>; ThoughtData: Type 0x10, Modifier 0x18,
    // Quantity 0x24
    game.g.PutU64(f + 0x120, f + 0xB00);
    game.g.PutRefList(f + 0xB00, f + 0xB40, {f + 0xC00});
    game.g.PutS32(f + 0xC00 + 0x10, 77);
    game.g.PutF32(f + 0xC00 + 0x18, -3.0f);
    game.g.PutS32(f + 0xC00 + 0x24, 2);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->followers[0].thoughts.size() == 1);
    CHECK_EQ(s->followers[0].thoughts[0].type, 77);
    CHECK_EQ(s->followers[0].thoughts[0].modifier, -3.0f);
    CHECK_EQ(s->followers[0].thoughts[0].quantity, 2);
}

TEST(snapshot_reads_inventory) {
    fake::Game game;
    // DataManager.items @0xE80 → List<InventoryItem>; InventoryItem: type 0x10, quantity 0x14
    game.g.PutU64(fake::kDataManager + 0xE80, 0x5C0000);
    game.g.PutRefList(0x5C0000, 0x5C1000, {0x5C2000, 0x5C2100});
    game.g.PutS32(0x5C2000 + 0x10, 1);
    game.g.PutS32(0x5C2000 + 0x14, 12);
    game.g.PutS32(0x5C2100 + 0x10, 2);
    game.g.PutS32(0x5C2100 + 0x14, 0);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->items.size() == 2);
    CHECK_EQ(s->items[0].first, 1);
    CHECK_EQ(s->items[0].second, 12);
    CHECK_EQ(s->items[1].second, 0);
}

TEST(snapshot_reads_completed_objectives_and_unique_group) {
    fake::Game game;
    fake::Game::ObjectiveSpec open{1, -1};
    open.unique_group = "grp";
    fake::Game::ObjectiveSpec done{5, -1};
    done.unique_group = "grp";
    game.SetObjectives({open});
    game.SetCompletedObjectives({done});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->objectives.size() == 2);
    CHECK_EQ(s->objectives[0].unique_group, std::string("grp"));
    CHECK(!s->objectives[0].complete);
    CHECK_EQ(s->objectives[1].type, 5);
    CHECK(s->objectives[1].complete); // comes from CompletedObjectives
}

TEST(snapshot_without_player_has_no_run) {
    fake::Game game;
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK(!s->run.has_value());
}

TEST(snapshot_in_base_has_no_run) {
    fake::Game game;
    fake::Game::RunSpec r;
    r.location = 1; // Base
    game.SetPlayer(r);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK(!s->run.has_value());
}

TEST(snapshot_reads_run_in_dungeon) {
    fake::Game game;
    fake::Game::RunSpec r;
    r.location = 86; // Dungeon1_6 (Woolhaven)
    r.weapon = 2;
    r.curse = 500;
    r.curse_lvl = 2;
    r.relic = 1;
    r.tarot = {{4, 0}, {12, 1}};
    game.SetPlayer(r);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->run.has_value());
    const auto& run = *s->run;
    CHECK_EQ(run.hp_red, 4.0f);
    CHECK_EQ(run.hp_red_max, 6.0f);
    CHECK_EQ(run.hp_blue, 2.0f);
    CHECK_EQ(run.hp_black, 1.0f);
    CHECK_EQ(run.hp_spirit, 1.0f);
    CHECK_EQ(run.hp_spirit_max, 2.0f);
    CHECK_EQ(run.weapon, 2);
    CHECK_EQ(run.weapon_lvl, 3);
    CHECK_EQ(run.curse, 500);
    CHECK_EQ(run.curse_lvl, 2);
    CHECK_EQ(run.relic, 1);
    REQUIRE(run.tarot.size() == 2);
    CHECK_EQ(run.tarot[1].first, 12);
    CHECK_EQ(run.tarot[1].second, 1);
}

TEST(dungeon_locations_match_game_manager) {
    // GameManager.IsDungeon without None (-1): Dungeon1_1-1_4, Boss_5, Dungeon1_5, IntroDungeon,
    // Dungeon1_6, Boss_Yngya, Boss_Wolf
    for (int32_t v : {7, 8, 9, 10, 51, 53, 81, 86, 93, 96}) {
        CHECK(cotl::IsDungeonLocation(v));
    }
    for (int32_t v : {-1, 0, 1, 6, 11, 52, 87, 95}) {
        CHECK(!cotl::IsDungeonLocation(v));
    }
}

TEST(snapshot_reads_dungeon_rooms) {
    fake::Game game;
    game.SetPlayer({});
    game.SetRooms({{0, 0, true, true, 1}, {1, 0, true, false, -1, true}, {0, 1, false, false}}, 0, 0);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value() && s->run.has_value());
    const auto& rooms = s->run->rooms;
    REQUIRE(rooms.size() == 3);
    CHECK(rooms[0].visited && rooms[0].discovered);
    CHECK(!rooms[1].visited && rooms[1].discovered);
    CHECK(rooms[1].kind == cotl::RoomKind::Boss);
    CHECK(rooms[0].doors[cotl::kDoorE]);
    CHECK(!rooms[0].doors[cotl::kDoorN]);
    CHECK_EQ(s->run->cur_x, 0);
}

// Doors like MiniMapIcon.SetIcons: only "connected" kinds (True, Boss, from NextLayer), target room not
// hidden, no NextLayer door to the north
TEST(snapshot_door_rule_follows_minimap) {
    fake::Game game;
    game.SetPlayer({});
    fake::Game::RoomSpec a{0, 0, true, true};
    a.conn_type = {6, 3, 4, 5}; // N NextLayer, E Exit, S Boss, W DoorRoom
    fake::Game::RoomSpec b{1, 1, true, true};
    b.conn_type = {1, 6, 1, 2}; // N True to a hidden room, E NextLayer, S True without Room, W Entrance
    b.conn_room = {2, -1, -1, -1};
    fake::Game::RoomSpec hidden{1, 2, false, false};
    hidden.hidden = true;
    game.SetRooms({a, b, hidden}, 0, 0);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value() && s->run.has_value());
    const auto& r = s->run->rooms;
    REQUIRE(r.size() == 3);
    CHECK(!r[0].doors[cotl::kDoorN]); // NextLayer to the north
    CHECK(!r[0].doors[cotl::kDoorE]); // Exit
    CHECK(r[0].doors[cotl::kDoorS]);  // Boss
    CHECK(!r[0].doors[cotl::kDoorW]); // DoorRoom
    CHECK(!r[1].doors[cotl::kDoorN]); // target room hidden
    CHECK(r[1].doors[cotl::kDoorE]);  // NextLayer to the east
    CHECK(r[1].doors[cotl::kDoorS]);  // True without a target room
    CHECK(!r[1].doors[cotl::kDoorW]); // Entrance
}

// Question mark like MiniMapIcon.SetSelfToQuestionMark: undiscovered, not hidden room (not
// the start room), connected by a door to a visited room that points back
TEST(snapshot_question_mark_rooms) {
    fake::Game game;
    game.SetPlayer({});
    fake::Game::RoomSpec a{0, 0, true, true};
    a.conn_type = {1, 1, 0, 0}; // N to c (no way back), E to b
    a.conn_room = {2, 1, -1, -1};
    a.path = "Assets/_Rooms/Boss Room Dungeon 1_1.prefab";
    fake::Game::RoomSpec b{1, 0, false, false};
    b.conn_type = {0, 0, 0, 1};
    b.conn_room = {-1, -1, -1, 0};
    fake::Game::RoomSpec c{0, 1, false, false};
    c.conn_type = {0, 0, 1, 0}; // S to a, but a.N points to c: connected back
    c.conn_room = {-1, -1, 0, -1};
    fake::Game::RoomSpec d{-1, 0, false, false}; // hidden, otherwise like b
    d.hidden = true;
    d.conn_type = {0, 1, 0, 0};
    d.conn_room = {-1, 0, -1, -1};
    fake::Game::RoomSpec e{0, -1, false, false}; // points to a, a does not point back
    e.conn_type = {1, 0, 0, 0};
    e.conn_room = {0, -1, -1, -1};
    game.SetRooms({a, b, c, d, e}, 0, 0);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value() && s->run.has_value());
    const auto& r = s->run->rooms;
    REQUIRE(r.size() == 5);
    CHECK(!r[0].question); // visited
    CHECK(r[1].question);
    CHECK(r[2].question);
    CHECK(!r[3].question); // hidden
    CHECK(!r[4].question); // no way back
    CHECK_EQ(r[0].path, std::string("Assets/_Rooms/Boss Room Dungeon 1_1.prefab"));
}

TEST(snapshot_reads_cult_name_and_level) {
    fake::Game game;
    game.SetCult("Lambs", 2);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK_EQ(s->cult_name, std::string("Lambs"));
    CHECK_EQ(s->cult_level, 2);
}

TEST(snapshot_sums_silo_storage) {
    fake::Game game;
    // SILO_SEED 169 (15), SEED_BUCKET 334 (250), SILO_FERTILISER 170 (15), other building
    game.SetBaseStructures({{169, {3, 4}}, {334, {10}}, {170, {2}}, {1, {99}}});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK_EQ(s->seed_stored, 17);
    CHECK_EQ(s->seed_capacity, 265);
    CHECK_EQ(s->fertiliser_stored, 2);
    CHECK_EQ(s->fertiliser_capacity, 15);
}

// Dirt in the base: POOP (97) and variants 337-342, 419; VOMIT 48; DEAD_WORSHIPPER 47.
// POOP_BUCKET (333) is a silo, not dirt.
TEST(snapshot_counts_dirt) {
    fake::Game game;
    game.SetBaseStructures({{97, {}}, {97, {}}, {337, {}}, {419, {}}, {48, {}}, {47, {}}, {47, {}},
                            {333, {}}, {1, {}}});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK_EQ(s->poop, 4);
    CHECK_EQ(s->vomit, 1);
    CHECK_EQ(s->corpses, 2);
}

// Read extra data only when a quest needs it
TEST(snapshot_reads_quest_data_when_needed) {
    fake::Game game;
    fake::Game::ObjectiveSpec kill{8, -1};
    fake::Game::ObjectiveSpec find{23, -1};
    find.str2 = "Leshy";
    fake::Game::ObjectiveSpec cursed{22, -1};
    cursed.str2 = "Okra";
    game.SetObjectives({kill, find, cursed});
    game.SetQuestData({{3, 12}}, {{5, 2}}, {{70, 9, "Daisy"}}, {7}, 41);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->enemies_killed.size() == 1);
    CHECK_EQ(s->enemies_killed[0].second, 12);
    CHECK_EQ(s->objectives[1].str2, std::string("Leshy"));
    CHECK_EQ(s->objectives[2].str2, std::string("Okra"));
    CHECK_EQ(s->offerings.size(), size_t{0}); // no BLIZZARD_OFFERING open
    CHECK_EQ(s->animals.size(), size_t{0});
}

TEST(snapshot_reads_animals_offerings_weapons_and_sword_day) {
    fake::Game game;
    game.SetObjectives({{41, -1}, {31, -1}, {36, -1}, {40, -1}});
    game.SetQuestData({}, {{5, 2}}, {{70, 9, "Daisy"}}, {7}, 41);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->animals.size() == 1);
    CHECK_EQ(s->animals[0].id, 9);
    CHECK_EQ(s->animals[0].type, 70);
    CHECK_EQ(s->animals[0].name, std::string("Daisy"));
    REQUIRE(s->offerings.size() == 1);
    CHECK_EQ(s->offerings[0].second, 2);
    REQUIRE(s->legendary_known.size() == 1);
    CHECK_EQ(s->legendary_known[0], 7);
    CHECK_EQ(s->sword_return_day, 41);
}

// Invalid read values must not create huge lists and texts (publishing runs in the tick)
TEST(snapshot_clamps_garbage_run_values) {
    fake::Game game;
    fake::Game::RunSpec r;
    r.hp = 1e30f;
    r.hp_total = std::nanf("");
    r.blue = -5;
    r.black = 1e9f;
    r.tarot = {{4, 2147483647}, {5, -3}, {6, 1}};
    game.SetPlayer(r);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value() && s->run.has_value());
    CHECK_EQ(s->run->hp_red, 88.0f);
    CHECK_EQ(s->run->hp_red_max, 0.0f);
    CHECK_EQ(s->run->hp_blue, 0.0f);
    CHECK_EQ(s->run->hp_black, 88.0f);
    CHECK_EQ(s->run->tarot[0].second, 9);
    CHECK_EQ(s->run->tarot[1].second, 0);
    CHECK_EQ(s->run->tarot[2].second, 1);
}

TEST(snapshot_caps_tarot_list) {
    fake::Game game;
    fake::Game::RunSpec r;
    r.tarot.assign(100, {4, 0});
    game.SetPlayer(r);
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value() && s->run.has_value());
    CHECK_EQ(s->run->tarot.size(), size_t{60});
}

TEST(snapshot_reads_thoughts_only_for_selected_follower) {
    fake::Game game;
    game.SetFollowers({{1, "A"}, {2, "B"}});
    const auto all = ReadSnapshot(game.g, fake::kMain);
    const auto none = ReadSnapshot(game.g, fake::kMain, cotl::kNoThoughts);
    REQUIRE(all.has_value() && none.has_value());
    CHECK_EQ(none->followers.size(), all->followers.size());
    for (const auto& f : none->followers) {
        CHECK(f.thoughts.empty());
    }
}

TEST(snapshot_detects_title_screen) {
    fake::Game game;
    const auto game_view = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(game_view.has_value());
    CHECK(!game_view->at_title);
    game.g.PutU64(fake::kMain + 0x71428F8, 0x5A00);
    game.g.PutU64(0x5A00 + 0xB8, 0x6A00);
    game.g.PutU64(0x6A00, 0x9A0000); // UIMainMenuController.Instance
    const auto title = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(title.has_value());
    CHECK(title->at_title);
}

TEST(snapshot_reads_gear_stats_only_on_request) {
    fake::Game game;
    fake::Game::RunSpec r;
    r.weapon = 2;
    r.curse = 500;
    r.relic = 1;
    game.SetPlayer(r);
    auto& g = game.g;
    const auto array = [&](uint64_t at, uint64_t element) {
        g.PutU64(at + 0x18, 1);
        g.PutU64(at + 0x20, element);
    };
    g.PutU64(fake::kMain + 0x713B9A0, 0x5B00);
    g.PutU64(0x5B00 + 0xB8, 0x6B00);
    g.PutU64(0x6B00 + 0x8, 0xA00000);
    g.PutU64(0x6B00 + 0x10, 0xA10000);
    g.PutU64(0x6B00 + 0x18, 0xA20000);
    array(0xA00000, 0xA01000);
    array(0xA10000, 0xA11000);
    array(0xA20000, 0xA21000);
    g.PutS32(0xA01000 + 0x18, 2);        // WeaponData
    g.PutF32(0xA01000 + 0x4C, 1.5f);     // Speed
    g.PutU64(0xA01000 + 0x50, 0xA02000); // Combos
    g.PutU64(0xA02000 + 0x10, 0xA03000);
    g.PutS32(0xA02000 + 0x18, 2);
    g.PutS32(0xA03000 + 0x18, 2);
    g.PutU64(0xA03000 + 0x20, 0xA04000);
    g.PutU64(0xA03000 + 0x28, 0xA05000);
    g.PutF32(0xA04000 + 0x14, 1.0f);
    g.PutF32(0xA05000 + 0x14, 3.0f);
    g.PutS32(0xA11000 + 0x18, 500);      // CurseData
    g.PutF32(0xA11000 + 0x6C, 4.0f);
    g.PutS32(0xA21000 + 0x18, 1);        // RelicData
    g.PutF32(0xA21000 + 0x28, 60.0f);

    const auto without = ReadSnapshot(g, fake::kMain);
    REQUIRE(without.has_value() && without->run.has_value());
    CHECK(!without->run->gear.has_value());

    const auto with = ReadSnapshot(g, fake::kMain, cotl::kAllThoughts, true);
    REQUIRE(with.has_value() && with->run.has_value() && with->run->gear.has_value());
    const auto& gear = *with->run->gear;
    CHECK_EQ(gear.weapon_avg, 2.0f);
    CHECK_EQ(gear.weapon_speed, 1.5f);
    CHECK_EQ(gear.curse_damage, 4.0f);
    CHECK_EQ(gear.relic_charge, 60.0f);
}

TEST(snapshot_save_not_loaded_without_class) {
    fake::Game game;
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK(!s->save_loaded);
}

namespace {
// Follower.Followers with one follower whose task `task_class` is in `state`; 0: no task
void PutSceneFollower(fake::Game& game, size_t index, uint64_t task_class, int32_t state,
                      uint64_t change_location = 0) {
    constexpr uint64_t kClass = 0x4000000, kStatics = 0x4001000, kList = 0x4002000,
                       kItems = 0x4003000, kScene = 0x4004000, kBrain = 0x4005000, kTask = 0x4006000;
    game.g.PutU64(fake::kMain + 0x713C060, kClass);
    game.g.PutU64(kClass + 0xB8, kStatics);
    game.g.PutU64(kStatics + 0x0, kList);
    game.g.PutRefList(kList, kItems, {kScene});
    game.g.PutU64(kScene + 0x130, kBrain);
    game.g.PutU64(kBrain + 0x20, fake::Game::FollowerObj(index));
    game.g.PutU64(kBrain + 0xC8, task_class ? kTask : 0);
    game.g.PutU64(kTask, task_class);
    game.g.PutS32(kTask + 0x20, state);
    game.g.PutU64(kTask + 0x28, change_location);
}
constexpr uint64_t kSleepClass = 0x4010000, kBedRestClass = 0x4011000, kOtherClass = 0x4012000;
void PutTaskClasses(fake::Game& game) {
    game.g.PutU64(fake::kMain + 0x713C5B0, kSleepClass);
    game.g.PutU64(fake::kMain + 0x713C5B8, kBedRestClass);
}
} // namespace

// The command state is read only for the selected follower
TEST(snapshot_reads_command_state_of_selected_follower) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble", 1, 50, 50, 0, 50, 0, {3, 9}}, {8, "Okra"}});
    const uint64_t okra = fake::Game::FollowerObj(1);
    game.g.PutS32(okra + 0x20, 0x44);
    game.g.PutU8(okra + 0x210, 1);
    game.g.PutF32(okra + 0x260, 2.5f);
    game.g.PutU8(okra + 0x2D1, 1);
    game.g.PutS32(okra + 0x8C, 0x31);
    game.g.PutU8(fake::kDataManager + 0x75A, 1);
    game.g.PutS32(fake::kDataManager + 0x16C, 1);
    game.g.PutS32(fake::kDataManager + 0x174, 1);
    game.g.PutS32(fake::kDataManager + 0x614, 4);
    game.SetIdList(0x5B0, {6, 7});
    game.SetIdList(0x5C0, {0x50});

    CHECK(!ReadSnapshot(game.g, fake::kMain)->command_state.has_value());
    CHECK(!ReadSnapshot(game.g, fake::kMain, cotl::kNoThoughts)->command_state.has_value());
    CHECK(!ReadSnapshot(game.g, fake::kMain, 99)->command_state.has_value());

    const auto s = ReadSnapshot(game.g, fake::kMain, 8);
    REQUIRE(s.has_value());
    REQUIRE(s->command_state.has_value());
    const auto& c = *s->command_state;
    CHECK_EQ(c.follower_id, 8);
    CHECK_EQ(c.cursed_state, 0x44);
    CHECK(c.is_snowman);
    CHECK(c.drunk);
    CHECK(c.received_blessing);
    CHECK_EQ(c.necklace, 0x31);
    CHECK(c.show_loyalty_bars);
    CHECK(c.inspire_unlocked);
    CHECK(c.intimidate_unlocked);
    CHECK_EQ(c.season, 1);
    CHECK_EQ(c.weather, 1);
    CHECK_EQ(c.phase, 4);
    CHECK(c.cult_traits == std::vector<int32_t>{0x50});
    CHECK(c.task == cotl::FollowerTaskKind::None); // not in the scene

    const auto first = ReadSnapshot(game.g, fake::kMain, 7);
    REQUIRE(first->command_state.has_value());
    CHECK(first->command_state->own_traits == (std::vector<int32_t>{3, 9}));
    CHECK(!first->command_state->received_blessing);
}

TEST(snapshot_reads_sleep_task_like_the_wheel) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}});
    PutTaskClasses(game);
    auto task = [&] { return ReadSnapshot(game.g, fake::kMain, 7)->command_state->task; };
    using K = cotl::FollowerTaskKind;
    CHECK(task() == K::None); // no scene
    PutSceneFollower(game, 0, 0, 0);
    CHECK(task() == K::None); // no task
    PutSceneFollower(game, 0, kSleepClass, 4);
    CHECK(task() == K::Sleep);
    PutSceneFollower(game, 0, kBedRestClass, 4);
    CHECK(task() == K::BedRest);
    PutSceneFollower(game, 0, kOtherClass, 4);
    CHECK(task() == K::None);
    PutSceneFollower(game, 0, kSleepClass, 3); // not Doing yet
    CHECK(task() == K::None);
    PutSceneFollower(game, 0, kSleepClass, 4, 0x4013000); // subtask: on the way to bed
    CHECK(task() == K::None);
}

TEST(snapshot_reads_the_timed_action_of_the_follower) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}});
    PutTaskClasses(game);
    auto timed = [&] { return ReadSnapshot(game.g, fake::kMain, 7)->command_state->timed_action; };
    CHECK(!timed()); // no scene
    PutSceneFollower(game, 0, 0, 0);
    constexpr uint64_t kScene = 0x4004000, kMachine = 0x4007000; // Follower+0x98, StateMachine+0x30
    CHECK(!timed());                                             // no StateMachine: unreadable, no lock
    game.g.PutU64(kScene + 0x98, kMachine);
    game.g.PutS32(kMachine + 0x30, 0);
    CHECK(!timed());
    game.g.PutS32(kMachine + 0x30, 16); // even without a task, it does not count here
    CHECK(timed());
    PutSceneFollower(game, 0, kSleepClass, 4);
    CHECK(timed());
}

// Unreadable stays set: the helper decides on the tap, Bless must not disappear
TEST(snapshot_reads_tutorial_flag_and_keeps_it_when_unreadable) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}});
    game.g.PutU8(fake::kDataManager + 0x850, 0);
    CHECK(!ReadSnapshot(game.g, fake::kMain, 7)->command_state->in_tutorial);
    game.g.PutU8(fake::kDataManager + 0x850, 1);
    CHECK(ReadSnapshot(game.g, fake::kMain, 7)->command_state->in_tutorial);
    game.g.PutU8(fake::kDataManager + 0x850, 0);
    game.g.unreadable = fake::kDataManager + 0x850;
    CHECK(ReadSnapshot(game.g, fake::kMain, 7)->command_state->in_tutorial);
}

// The conditions of the simple commands (tools/cotl/README.md, Einfache Befehle)
TEST(snapshot_reads_the_gates_of_the_simple_commands) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}, {8, "Okra"}});
    const uint64_t okra = fake::Game::FollowerObj(1);
    game.g.PutS32(fake::kDataManager + 0x610, 3);
    const auto read = [&](int32_t id) { return *ReadSnapshot(game.g, fake::kMain, id)->command_state; };

    auto c = read(8);
    CHECK(!c.inspired && !c.intimidated && !c.bribed && !c.scared_interacted && !c.kissed && !c.petted);
    CHECK(!c.married && !c.skin_dog && !c.bribe_unlocked && !c.shrine_built && !c.sozo_freed);
    CHECK(!c.work_through_night);
    CHECK(!c.paid_tithes && !c.extort_unlocked);
    CHECK_EQ(c.gold, 0);
    CHECK_EQ(c.onboarding_phase, 0);

    game.g.PutU8(okra + 0x2D0, 1);
    game.g.PutU8(okra + 0x2D3, 1);
    game.g.PutU8(okra + 0x2D4, 1);
    game.g.PutU8(okra + 0x2D5, 1);
    game.g.PutU8(okra + 0x2D7, 1);
    game.g.PutU8(okra + 0x2D8, 1);
    game.g.PutU8(okra + 0x2D9, 1);
    game.g.PutU8(okra + 0x1F5, 1);
    game.g.PutU64(okra + 0x58, okra + 0xC00);
    game.g.PutString(okra + 0xC00, "Dog_2");
    game.g.PutS32(okra + 0x2E4, 12);
    game.g.PutS32(fake::kDataManager + 0x610, 12);
    game.SetIdList(0x5B0, {0xF, 0xE});
    game.g.PutU8(fake::kDataManager + 0x14C, 1);
    game.g.PutS32(fake::kDataManager + 0x110, 3);
    game.g.PutU8(fake::kDataManager + 0x6C3, 1);
    game.g.PutU64(fake::kDataManager + 0xE80, 0x5C0000);
    game.g.PutRefList(0x5C0000, 0x5C1000, {0x5C2000, 0x5C3000, 0x5C4000});
    game.g.PutS32(0x5C2000 + 0x10, 1);
    game.g.PutS32(0x5C2000 + 0x14, 99);
    game.g.PutS32(0x5C3000 + 0x10, 0x14);
    game.g.PutS32(0x5C3000 + 0x14, 7);
    game.g.PutS32(0x5C4000 + 0x10, 0x14); // GetItemQuantity counts only the first stack
    game.g.PutS32(0x5C4000 + 0x14, 50);

    c = read(8);
    CHECK(c.kissed && c.inspired && c.petted && c.intimidated && c.bribed && c.scared_interacted);
    CHECK(c.married && c.skin_dog && c.bribe_unlocked && c.shrine_built && c.sozo_freed);
    CHECK(c.work_through_night);
    CHECK(c.paid_tithes && c.extort_unlocked);
    CHECK_EQ(c.gold, 7);
    CHECK_EQ(c.onboarding_phase, 3);

    const auto other = read(7);
    CHECK(!other.kissed && !other.married && !other.skin_dog);
    CHECK(other.work_through_night == false);
    CHECK(!other.paid_tithes);
    CHECK(other.extort_unlocked); // doctrine applies to the whole cult

    game.g.PutString(okra + 0xC00, "Poppy");
    CHECK(read(8).skin_dog);
    game.g.PutString(okra + 0xC00, "Cat");
    CHECK(!read(8).skin_dog);
}

// MakeDemandCommands asks get_CurrentTaskType: a subtask counts with its type
TEST(snapshot_reads_the_sleep_task_type_in_any_state) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}});
    PutTaskClasses(game);
    auto in_sleep = [&] { return ReadSnapshot(game.g, fake::kMain, 7)->command_state->in_sleep_task; };
    CHECK(!in_sleep());
    PutSceneFollower(game, 0, kSleepClass, 3);
    CHECK(in_sleep());
    PutSceneFollower(game, 0, kBedRestClass, 3);
    CHECK(in_sleep());
    PutSceneFollower(game, 0, kOtherClass, 4);
    CHECK(!in_sleep());
    PutSceneFollower(game, 0, kSleepClass, 4, 0x4013000);
    CHECK(!in_sleep());
}

// Food and work (tools/cotl/README.md): base buildings, meals and drinks, fasting, holiday, nudity
TEST(snapshot_reads_what_eat_drink_and_work_offer) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}});
    const auto read = [&] { return *ReadSnapshot(game.g, fake::kMain, 7)->command_state; };
    auto c = read();
    CHECK(c.meals.empty() && c.drinks.empty() && c.structures.empty());
    CHECK(!c.fasting && !c.holiday && !c.nudism && !c.pub_drinking);

    // Meals: Structures_Meal is a building with its own type, plus the stores of the kitchens;
    // drinks from the stores of the pubs, only the unreserved ones
    game.SetBaseStructures({{52, {}},                                     // MEAL -> ITEM 0x20
                            {431, {}},                                    // -> 0xC7
                            {10, {}, {{0x39}, {0x91}}},                   // KITCHEN: meal and beer
                            {265, {}, {{0x73, 2}}},                       // KITCHEN_II
                            {343, {}, {{0x91}, {0x93, 1, 1}, {0x20}}},    // PUB: beer, cocktail reserved
                            {350, {}, {{0xEC, 1, 0}}},                    // PUB_2: milkshake
                            {31, {}},                                     // BUILD_SITE
                            {1, {}, {{0x45}}}});                          // other building, its store does not count
    c = read();
    CHECK(c.structures == (std::vector<int32_t>{52, 431, 10, 265, 343, 350, 31, 1}));
    CHECK(c.meals == (std::vector<int32_t>{0x20, 0xC7, 0x39, 0x73}));
    CHECK(c.drinks == (std::vector<int32_t>{0x91, 0xEC}));

    // Clock: TotalElapsedGameTime = day * 1200 + time; fasting 3600, holiday 1200, nudity 600
    game.SetDayTime(3, 100.f);
    const float now = 3 * 1200.f + 100.f;
    game.g.PutF32(fake::kDataManager + 0x374, now - 3599.f);
    game.g.PutF32(fake::kDataManager + 0x35C, now - 1199.f);
    game.g.PutF32(fake::kDataManager + 0x364, now - 599.f);
    c = read();
    CHECK(c.fasting && c.holiday && c.nudism);
    game.g.PutF32(fake::kDataManager + 0x374, now - 3600.f);
    game.g.PutF32(fake::kDataManager + 0x35C, now - 1200.f);
    game.g.PutF32(fake::kDataManager + 0x364, now - 600.f);
    c = read();
    CHECK(!c.fasting && !c.holiday && !c.nudism);

    // Structures_Pub.IsDrinking (static @0x0)
    constexpr uint64_t kPubClass = 0x7000, kPubStatics = 0x7400;
    game.g.PutU64(fake::kMain + 0x7141930, kPubClass);
    game.g.PutU64(kPubClass + 0xB8, kPubStatics);
    CHECK(!read().pub_drinking);
    game.g.PutU8(kPubStatics, 1);
    CHECK(read().pub_drinking);
}

// Gifts (tools/cotl/README.md): inventory by first stack, AllNecklaces, the necklace and the pet
TEST(snapshot_reads_what_gifts_offer) {
    fake::Game game;
    game.SetFollowers({{7, "Bramble"}});
    const uint64_t info = fake::Game::FollowerObj(0);
    const auto read = [&] { return *ReadSnapshot(game.g, fake::kMain, 7)->command_state; };
    auto c = read();
    CHECK(c.items.empty() && c.all_necklaces.empty());
    CHECK(!c.showing_necklace && !c.has_dlc_pet && !c.onboarded_wool);

    // First stack counts (GetItemQuantity 0x1BBD440): 0x2D with 0 hides the second stack
    game.g.PutU64(fake::kDataManager + 0xE80, 0x5C0000);
    game.g.PutRefList(0x5C0000, 0x5C1000, {0x5C2000, 0x5C3000, 0x5C4000, 0x5C5000});
    game.g.PutS32(0x5C2000 + 0x10, 0x2B);
    game.g.PutS32(0x5C2000 + 0x14, 2);
    game.g.PutS32(0x5C3000 + 0x10, 0x2D);
    game.g.PutS32(0x5C3000 + 0x14, 0);
    game.g.PutS32(0x5C4000 + 0x10, 0x2D);
    game.g.PutS32(0x5C4000 + 0x14, 5);
    game.g.PutS32(0x5C5000 + 0x10, 0xAA);
    game.g.PutS32(0x5C5000 + 0x14, 1);
    // DataManager.AllNecklaces (static @0x50)
    game.g.PutU64(fake::kDataManagerStatics + 0x50, 0x5D0000);
    game.g.PutIntList(0x5D0000, 0x5D1000, {0x2D, 0x2E});
    game.g.PutU8(info + 0x60, 1);
    game.g.PutU8(fake::kDataManager + 0x95C, 1);
    c = read();
    CHECK(c.items == (std::vector<int32_t>{0x2B, 0xAA}));
    CHECK(c.all_necklaces == (std::vector<int32_t>{0x2D, 0x2E}));
    CHECK(c.showing_necklace);
    CHECK(c.onboarded_wool);
    CHECK(!c.has_dlc_pet);

    // FollowerInfo.DLCPets @0xA0: one entry means a pet
    game.g.PutU64(info + 0xA0, 0x5E0000);
    game.g.PutIntList(0x5E0000, 0x5E1000, {});
    CHECK(!read().has_dlc_pet);
    game.g.PutIntList(0x5E0000, 0x5E1000, {3});
    CHECK(read().has_dlc_pet);
}

namespace {
fake::FollowerSpec Skinned(int32_t id, std::string skin, int32_t colour) {
    fake::FollowerSpec f{id, "F" + std::to_string(id)};
    f.skin = std::move(skin);
    f.skin_colour = colour;
    return f;
}
} // namespace

// Colours as in SetFollowerCostume: first entry whose skin list has the name; set = SkinColour,
// clamped to [0, count − 1].
TEST(snapshot_reads_skin_colours) {
    fake::Game game;
    game.SetWorshipperData({
        {{"Dog"}, {{{"HEAD_SKIN_TOP", {0, 0, 1, 1}}}}},
        {{"Cat", "Cat2"},
         {{{"HEAD_SKIN_TOP", {1, 0.5f, 0, 1}}, {"MARKINGS", {0, 1, 0, 0.5f}}}, {{"HEAD_SKIN_TOP", {1, 1, 1, 1}}}}},
    });
    game.SetFollowers({Skinned(1, "Cat2", 0), Skinned(2, "Cat", 7), Skinned(3, "Dog", -1),
                       Skinned(4, "Unknown", 0), {5, "NoSkin"}});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    REQUIRE(s->followers.size() == 5);
    const auto& a = s->followers[0];
    CHECK_EQ(a.skin, std::string("Cat2"));
    REQUIRE(a.tints.size() == 2);
    CHECK_EQ(a.tints[0].slot, std::string("HEAD_SKIN_TOP"));
    CHECK_EQ(a.tints[0].rgba, 0xFF8000FFu);
    CHECK_EQ(a.tints[1].rgba, 0x00FF0080u);
    REQUIRE(s->followers[1].tints.size() == 1); // set 7 → last
    CHECK_EQ(s->followers[1].tints[0].rgba, 0xFFFFFFFFu);
    REQUIRE(s->followers[2].tints.size() == 1); // −1 → first
    CHECK_EQ(s->followers[2].tints[0].rgba, 0x0000FFFFu);
    CHECK(s->followers[3].tints.empty());
    CHECK_EQ(s->followers[3].skin, std::string("Unknown"));
    CHECK(s->followers[4].skin.empty());
}

TEST(snapshot_skin_without_worshipper_data) {
    fake::Game game;
    game.SetFollowers({Skinned(1, "Cat", 0)});
    const auto s = ReadSnapshot(game.g, fake::kMain);
    REQUIRE(s.has_value());
    CHECK_EQ(s->followers[0].skin, std::string("Cat"));
    CHECK(s->followers[0].tints.empty());
}

// The cache reads the colour table only once (it does not change in the game)
TEST(snapshot_skin_colour_cache) {
    fake::Game game;
    game.SetWorshipperData({{{"Cat"}, {{{"HEAD_SKIN_TOP", {1, 0, 0, 1}}}}}});
    game.SetFollowers({Skinned(1, "Cat", 0)});
    cotl::SkinColourCache cache;
    REQUIRE(ReadSnapshot(game.g, fake::kMain, cotl::kAllThoughts, false, &cache).has_value());
    game.SetWorshipperData({{{"Cat"}, {{{"HEAD_SKIN_TOP", {0, 0, 1, 1}}}}}});
    const auto s = ReadSnapshot(game.g, fake::kMain, cotl::kAllThoughts, false, &cache);
    REQUIRE(s.has_value());
    REQUIRE(s->followers[0].tints.size() == 1);
    CHECK_EQ(s->followers[0].tints[0].rgba, 0xFF0000FFu);
}

// Shortly after loading the colour table is still empty: that must not stick
TEST(snapshot_skin_colour_cache_retries_empty) {
    fake::Game game;
    game.SetWorshipperData({});
    game.SetFollowers({Skinned(1, "Cat", 0)});
    cotl::SkinColourCache cache;
    const auto early = ReadSnapshot(game.g, fake::kMain, cotl::kAllThoughts, false, &cache);
    REQUIRE(early.has_value());
    CHECK(early->followers[0].tints.empty());
    game.SetWorshipperData({{{"Cat"}, {{{"HEAD_SKIN_TOP", {1, 0, 0, 1}}}}}});
    const auto s = ReadSnapshot(game.g, fake::kMain, cotl::kAllThoughts, false, &cache);
    REQUIRE(s.has_value());
    REQUIRE(s->followers[0].tints.size() == 1);
    CHECK_EQ(s->followers[0].tints[0].rgba, 0xFF0000FFu);
}
