// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_equipment.h"
#include "cotl_fake_publisher.h"
#include "cotl_i2.h"
#include "cotl_publish.h"
#include "cotl_terms.h"
#include "cotl_test_util.h"

using cotl::Follower;
using cotl::FollowerGroup;
using cotl::GameSnapshot;

namespace {

Follower F(int32_t id, const char* name, FollowerGroup group = FollowerGroup::Normal) {
    Follower f;
    f.id = id;
    f.name = name;
    f.level = id;
    f.satiation = 80.4f;
    f.faith = 65;
    f.illness = 72.6f;
    f.happiness = 20;
    f.adoration = 37.6f;
    f.age = 23;
    f.life_expectancy = 41;
    f.role = 1;          // FollowerRole.Worker
    f.traits = {1, 2};   // NaturallySkeptical, NaturallyObedient
    f.group = group;
    return f;
}

const cotl::I2Table& Table() {
    static const auto t = cotl::I2Table::FromMap({
        {"Traits/Worker", "Worker"},
        {"Traits/NaturallySkeptical", "Skeptical"},
        {"Traits/NaturallyObedient", "Obedient"},
        {"FollowerThoughts/AfterHoliday", "Sample thought"},
    });
    return t;
}

const cotl::Terms kTerms(&Table());

} // namespace

TEST(publish_followers_basic_values) {
    GameSnapshot s;
    s.followers = {F(1, "Bramble"), F(2, "Okra")};
    FakePublisher p;
    cotl::PublishFollowers(p, s, kTerms);
    CHECK_EQ(p.i64["fol.count"], int64_t{2});
    CHECK_EQ(p.text["fol.name0"], std::string("Bramble"));
    CHECK_EQ(p.i64["fol.level1"], int64_t{2});
    CHECK_EQ(p.i64["fol.sat0"], int64_t{80});
    CHECK_EQ(p.i64["fol.loyal0"], int64_t{38});
    CHECK_EQ(p.CountPrefix("fol.faith"), size_t{0}); // replaced by loyalty
    CHECK_EQ(p.i64["fol.ill0"], int64_t{73});
    CHECK_EQ(p.i64["fol.happy0"], int64_t{20});
    CHECK_EQ(p.text["fol.role0"], std::string("Worker"));
}

TEST(publish_followers_detail_has_role_age_traits) {
    GameSnapshot s;
    s.followers = {F(1, "Bramble")};
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 0);
    CHECK_EQ(p.text["fol.sel_detail"], std::string("Level 1 - Worker - Age 23\nTraits: Skeptical, Obedient"));
    CHECK(p.text["fol.sel_detail"].find("41") == std::string::npos); // no life expectancy
}

TEST(publish_followers_detail_without_traits) {
    GameSnapshot s;
    s.followers = {F(1, "Bramble")};
    s.followers[0].traits.clear();
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 0);
    CHECK_EQ(p.text["fol.sel_detail"], std::string("Level 1 - Worker - Age 23"));
}

TEST(publish_followers_groups_after_normal) {
    GameSnapshot s;
    s.followers = {F(1, "Prisoner", FollowerGroup::Prisoner), F(2, "Away", FollowerGroup::Missionary),
                   F(3, "Home")};
    FakePublisher p;
    cotl::PublishFollowers(p, s, kTerms);
    CHECK_EQ(p.text["fol.name0"], std::string("Home"));
    CHECK_EQ(p.text["fol.name1"], std::string("Away"));
    CHECK_EQ(p.text["fol.name2"], std::string("Prisoner"));
    CHECK_EQ(p.i64["fol.group0"], int64_t{0});
    CHECK_EQ(p.i64["fol.group1"], int64_t{1});
    CHECK_EQ(p.i64["fol.group2"], int64_t{2});
}

TEST(publish_followers_each_follower_once) {
    GameSnapshot s;
    s.followers = {F(1, "A"), F(2, "B", FollowerGroup::Prisoner)};
    FakePublisher p;
    cotl::PublishFollowers(p, s, kTerms);
    CHECK_EQ(p.CountPrefix("fol.name"), size_t{2});
}

TEST(publish_selected_follower_uses_list_order) {
    GameSnapshot s;
    s.followers = {F(1, "Prisoner", FollowerGroup::Prisoner), F(3, "Home")};
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 1); // row 1 of the list = "Prisoner"
    CHECK_EQ(p.text["fol.sel_name"], std::string("Prisoner"));
    CHECK_EQ(p.text["fol.sel_detail"], std::string("Level 1 - Worker - Age 23\nTraits: Skeptical, Obedient"));
}

// Large card in the follower window (tarot style): same values as the list row
TEST(publish_selected_follower_card_values) {
    GameSnapshot s;
    s.followers = {F(1, "Prisoner", FollowerGroup::Prisoner), F(3, "Home")};
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 1);
    CHECK_EQ(p.i64["fol.sel_level"], int64_t{1});
    CHECK_EQ(p.text["fol.sel_role"], std::string("Worker"));
    CHECK_EQ(p.i64["fol.sel_sat"], int64_t{80});
    CHECK_EQ(p.i64["fol.sel_loyal"], int64_t{38});
    CHECK_EQ(p.i64["fol.sel_ill"], int64_t{73});
    CHECK_EQ(p.i64["fol.sel_happy"], int64_t{20});
    CHECK_EQ(p.i64["fol.sel_group"], int64_t{2});
    CHECK_EQ(p.i64["fol.sel_age"], int64_t{23});
    CHECK_EQ(p.text["fol.sel_traits"], std::string("Skeptical\nObedient"));
}

TEST(publish_selected_follower_out_of_range_publishes_nothing) {
    GameSnapshot s;
    s.followers = {F(1, "A")};
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 5);
    cotl::PublishSelectedFollower(p, s, kTerms, -1);
    CHECK_EQ(p.CountPrefix("fol.sel"), size_t{0});
}

// Groups and order as in the game menu (InventoryMenu..ctor, inventory_menu_1_5_27.inc):
// Currency starts with GOLD_NUGGET (83), BLACK_GOLD (20), GOLD_REFINED (86), LOG (1), ...;
// Food with 35, 8, 21; Items with 72, 55, 56.
TEST(publish_inventory_groups_in_menu_order) {
    GameSnapshot s;
    // (ITEM_TYPE, quantity): LOG before COIN in the inventory, but COIN before LOG in the menu; quantity 0 is dropped;
    // 99999 is in no menu list and, as in the game, does not appear
    s.items = {{1, 12}, {20, 5}, {2, 0}, {8, 3}, {72, 1}, {99999, 7}};
    FakePublisher p;
    cotl::PublishInventory(p, s, kTerms, -1);
    CHECK_EQ(p.i64["inv.count"], int64_t{4});
    CHECK_EQ(p.i64["inv.n0"], int64_t{2});
    CHECK_EQ(p.i64["inv.n1"], int64_t{1});
    CHECK_EQ(p.i64["inv.n2"], int64_t{1});
    CHECK_EQ(p.text["inv.g0.icon0"], std::string("module:cotl:item/20"));
    CHECK_EQ(p.i64["inv.g0.qty0"], int64_t{5});
    CHECK_EQ(p.text["inv.g0.name1"], std::string("LOG")); // without an I2 term: identifier
    CHECK_EQ(p.i64["inv.g1.qty0"], int64_t{3});
    CHECK_EQ(p.text["inv.g2.icon0"], std::string("module:cotl:item/72"));
}

TEST(publish_inventory_quest_marker) {
    GameSnapshot s;
    s.items = {{1, 12}, {20, 5}};
    cotl::Objective collect; // open collect quest for LOG, without a follower
    collect.type = 1;
    collect.follower_id = -1;
    collect.item_type = 1;
    cotl::Objective done = collect; // a completed quest does not mark
    done.item_type = 20;
    done.complete = true;
    s.objectives = {collect, done};
    FakePublisher p;
    cotl::PublishInventory(p, s, kTerms, -1);
    CHECK_EQ(p.i64["inv.g0.quest0"], int64_t{0}); // COIN
    CHECK_EQ(p.i64["inv.g0.quest1"], int64_t{1}); // LOG
}

TEST(publish_inventory_quest_marker_for_give_item) {
    GameSnapshot s;
    s.items = {{1, 12}};
    cotl::Objective give; // GIVE_ITEM (story quest for an NPC)
    give.type = 33;
    give.follower_id = -1;
    give.item_type = 1;
    s.objectives = {give};
    FakePublisher p;
    cotl::PublishInventory(p, s, kTerms, -1);
    CHECK_EQ(p.i64["inv.g0.quest0"], int64_t{1});
}

TEST(publish_inventory_selection_with_description) {
    const auto t = cotl::I2Table::FromMap({
        {"Inventory/BERRY", "Berry"},
        {"Inventory/BERRY/Description", "A tasty <color=#FFD201>ingredient</color>."},
    });
    const cotl::Terms terms(&t);
    GameSnapshot s;
    s.items = {{21, 3}}; // BERRY (Food)
    FakePublisher p;
    cotl::PublishInventory(p, s, terms, 1000); // group 1, entry 0
    CHECK_EQ(p.text["inv.sel_name"], std::string("Berry"));
    CHECK_EQ(p.text["inv.sel_desc"], std::string("A tasty {c:#FFFFD201}ingredient{/c}."));
    CHECK_EQ(p.i64["inv.sel_qty"], int64_t{3});
    CHECK_EQ(p.text["inv.sel_icon"], std::string("module:cotl:item/21"));
}

TEST(publish_inventory_empty_and_bad_selection) {
    GameSnapshot s;
    FakePublisher p;
    cotl::PublishInventory(p, s, kTerms, 2005);
    CHECK_EQ(p.i64["inv.count"], int64_t{0});
    CHECK_EQ(p.i64["inv.n0"], int64_t{0});
    CHECK_EQ(p.CountPrefix("inv.sel"), size_t{0});
}

// Empty list
TEST(publish_followers_empty) {
    GameSnapshot s;
    FakePublisher p;
    cotl::PublishFollowers(p, s, kTerms);
    CHECK_EQ(p.i64["fol.count"], int64_t{0});
    CHECK_EQ(p.CountPrefix("fol.name"), size_t{0});
}

TEST(publish_selected_follower_thoughts) {
    GameSnapshot s;
    s.followers = {F(1, "Bramble")};
    const int32_t holiday = [] {
        for (int32_t v = 0; v < 2000; ++v)
            if (cotl::Terms::EnumName("Thought", v) == "AfterHoliday") return v;
        return -1;
    }();
    REQUIRE(holiday >= 0);
    s.followers[0].thoughts = {{holiday, 5.0f, 1}, {holiday, -2.4f, 2}};
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 0);
    CHECK_EQ(p.i64["fol.th_count"], int64_t{2});
    CHECK_EQ(p.text["fol.th_text0"], std::string("Sample thought"));
    CHECK_EQ(p.text["fol.th_mod0"], std::string("+5"));
    CHECK_EQ(p.text["fol.th_text1"], std::string("Sample thought x2"));
    CHECK_EQ(p.text["fol.th_mod1"], std::string("-2"));
}

TEST(publish_selected_follower_without_thoughts) {
    GameSnapshot s;
    s.followers = {F(1, "Bramble")};
    FakePublisher p;
    cotl::PublishSelectedFollower(p, s, kTerms, 0);
    CHECK_EQ(p.i64["fol.th_count"], int64_t{0});
}

TEST(publish_run_values_and_tarot) {
    const auto t = cotl::I2Table::FromMap({
        {"UpgradeSystem/Sword_Critical/Name", "Critical Sword"},
        {"TarotCards/Hearts1/Name", "The Hearts"},
        {"TarotCards/Hearts1/Description", "Gain <color=red>half</color> a heart."},
    });
    const cotl::Terms terms(&t);
    const int32_t hearts = [] {
        for (int32_t v = 0; v < 500; ++v)
            if (cotl::Terms::EnumName("TarotCard", v) == "Hearts1") return v;
        return -1;
    }();
    REQUIRE(hearts >= 0);
    GameSnapshot s;
    cotl::Run run;
    run.hp_red = 5; // half hearts
    run.hp_red_max = 6;
    run.weapon = 2; // Sword_Critical
    run.weapon_lvl = 4;
    run.curse = 9999; // None
    run.tarot = {{hearts, 0}, {hearts, 2}, {hearts, 0}, {hearts, 1}};
    s.run = run;
    FakePublisher p;
    cotl::PublishRun(p, s, terms, -1);
    CHECK_EQ(p.i64["g.in_run"], int64_t{1});
    CHECK_EQ(p.text["run.weapon"], std::string("Critical Sword"));
    CHECK_EQ(p.i64["run.weapon_lvl"], int64_t{4});
    CHECK_EQ(p.i64["run.has_curse"], int64_t{0});
    CHECK_EQ(p.i64["run.has_weapon"], int64_t{1});
    CHECK_EQ(p.i64["run.tarot_count"], int64_t{4});
    CHECK_EQ(p.text["run.tarot_name0"], std::string("The Hearts"));
    CHECK_EQ(p.text["run.tarot_name1"], std::string("The Hearts++"));
    CHECK_EQ(p.text["run.tarot_icon0"], "module:cotl:tarot/" + std::to_string(hearts));
}

TEST(publish_run_selected_tarot_text) {
    const auto t = cotl::I2Table::FromMap({
        {"TarotCards/Hearts1/Name", "The Hearts"},
        {"TarotCards/Hearts1/Description", "Gain <color=red>half</color> a heart."},
    });
    const cotl::Terms terms(&t);
    int32_t hearts = -1;
    for (int32_t v = 0; v < 500 && hearts < 0; ++v)
        if (cotl::Terms::EnumName("TarotCard", v) == "Hearts1") hearts = v;
    GameSnapshot s;
    s.run = cotl::Run{};
    s.run->tarot = {{hearts, 1}};
    FakePublisher p;
    cotl::PublishRun(p, s, terms, 0);
    CHECK_EQ(p.text["run.sel_name"], std::string("The Hearts+"));
    CHECK_EQ(p.text["run.sel_text"], std::string("Gain half a heart."));
    // Large card image in the window
    CHECK_EQ(p.text["run.sel_pic"], "module:cotl:tarot/" + std::to_string(hearts));
    CHECK_EQ(p.i64["run.sel_tarot"], int64_t{1});
}

namespace {
cotl::GearData PlainGear() {
    cotl::GearData d;
    d.weapon_avg = 2.0f;
    d.weapon_speed = 1.5f;
    d.curse_damage = 3.0f;
    d.relic_charge = 60.0f;
    return d;
}
cotl::Run GearRun() {
    cotl::Run r;
    r.weapon = 0; // Sword
    r.weapon_lvl = 3;
    r.curse = 500;
    r.curse_lvl = 2;
    r.relic = 1; // LightningStrike
    r.gear = PlainGear();
    return r;
}
} // namespace

TEST(equipment_weapon_damage_like_game) {
    cotl::Run r = GearRun();
    // 2 * (1 + 0.07 * 3 + 0.13) = 2.68, rounded to one decimal
    CHECK_EQ(cotl::FormatStat(*cotl::WeaponDamage(r)), std::string("2.7"));
    r.tarot = {{33, 1}}; // IncreasedDamage+: 0.5
    CHECK_EQ(cotl::FormatStat(*cotl::WeaponDamage(r)), std::string("3.7"));
    r.gear->relic_double = true;
    CHECK_EQ(cotl::FormatStat(*cotl::WeaponDamage(r)), std::string("5.7"));
    r.gear->difficulty = 0.5f;
    CHECK_EQ(cotl::FormatStat(*cotl::WeaponDamage(r)), std::string("2.8"));
}

TEST(equipment_curse_damage_like_game) {
    cotl::Run r = GearRun();
    // 3 * (Vlies 1 + 2 / 6) = 4
    CHECK_EQ(cotl::FormatStat(*cotl::CurseDamage(r)), std::string("4"));
    r.tarot = {{18, 2}}; // Potion++: 1
    CHECK_EQ(cotl::FormatStat(*cotl::CurseDamage(r)), std::string("7"));
    r.tarot = {{89, 0}}; // CursedIce only counts for ice curses
    CHECK_EQ(cotl::FormatStat(*cotl::CurseDamage(r)), std::string("4"));
    r.curse = 504;
    CHECK_EQ(cotl::FormatStat(*cotl::CurseDamage(r)), std::string("13"));
}

TEST(equipment_without_data_has_no_damage) {
    cotl::Run r = GearRun();
    r.gear->weapon_avg = -1;
    r.gear->curse_damage = -1;
    CHECK(!cotl::WeaponDamage(r).has_value());
    CHECK(!cotl::CurseDamage(r).has_value());
    r.gear.reset();
    CHECK(!cotl::WeaponDamage(r).has_value());
}

TEST(equipment_charge_categories_and_numerals) {
    CHECK_EQ(cotl::ChargeCategory(49.0f), std::string("Fast"));
    CHECK_EQ(cotl::ChargeCategory(50.0f), std::string("Average"));
    CHECK_EQ(cotl::ChargeCategory(80.0f), std::string("Slow"));
    CHECK_EQ(cotl::ToNumeral(1), std::string("I"));
    CHECK_EQ(cotl::ToNumeral(4), std::string("IV"));
    CHECK_EQ(cotl::ToNumeral(9), std::string("IX"));
    CHECK_EQ(cotl::ToNumeral(0), std::string());
}

TEST(publish_run_selected_gear) {
    const auto t = cotl::I2Table::FromMap({
        {"UpgradeSystem/Sword/Name", "Sample Blade"},
        {"UpgradeSystem/Sword/Description", "Sample weapon text."},
        {"UpgradeSystem/Sword/Lore", "Sample weapon lore."},
        {"Relics/LightningStrike", "Sample Relic"},
        {"Relics/LightningStrike/Description", "Sample relic text."},
        {"Relics/LightningStrike/Lore", "Sample relic lore."},
        {"UI/Charge", "Charge Speed:"},
    });
    const cotl::Terms terms(&t);
    GameSnapshot s;
    s.run = GearRun();
    FakePublisher w;
    cotl::PublishRun(w, s, terms, cotl::kRunSelWeapon);
    CHECK_EQ(w.i64["run.sel_open"], int64_t{1});
    CHECK_EQ(w.text["run.sel_name"], std::string("Sample Blade III"));
    CHECK_EQ(w.text["run.sel_stats"], std::string("Damage: 2.7    Speed: 1.5"));
    CHECK_EQ(w.text["run.sel_text"], std::string("Sample weapon text."));
    CHECK_EQ(w.text["run.sel_lore"], std::string("Sample weapon lore."));
    CHECK_EQ(w.text["run.sel_pic"], "module:cotl:equip/" + std::to_string(s.run->weapon));
    CHECK_EQ(w.i64["run.sel_tarot"], int64_t{0});

    FakePublisher r;
    cotl::PublishRun(r, s, terms, cotl::kRunSelRelic);
    CHECK_EQ(r.text["run.sel_name"], std::string("Sample Relic"));
    CHECK_EQ(r.text["run.sel_stats"], std::string("Charge Speed: Average"));
    CHECK_EQ(r.text["run.sel_text"], std::string("Sample relic text."));
    CHECK_EQ(r.text["run.sel_lore"], std::string("Sample relic lore."));
    CHECK_EQ(r.text["run.sel_pic"], "module:cotl:relic/" + std::to_string(s.run->relic));
}

TEST(publish_run_gear_window_only_when_equipped) {
    const cotl::Terms terms(nullptr);
    GameSnapshot s;
    s.run = GearRun();
    s.run->curse = cotl::kNoEquipment;
    s.run->relic = 0;
    FakePublisher c;
    cotl::PublishRun(c, s, terms, cotl::kRunSelCurse);
    CHECK_EQ(c.i64.count("run.sel_open"), size_t{0});
    FakePublisher r;
    cotl::PublishRun(r, s, terms, cotl::kRunSelRelic);
    CHECK_EQ(r.i64.count("run.sel_open"), size_t{0});
}

TEST(publish_title_screen_is_not_loaded) {
    GameSnapshot s;
    s.save_loaded = true;
    s.day = 9;
    s.followers.resize(3);
    s.at_title = true;
    FakePublisher p;
    cotl::PublishCult(p, s);
    CHECK_EQ(p.i64["g.loaded"], int64_t{0});
}

TEST(publish_run_outside_dungeon) {
    GameSnapshot s;
    FakePublisher p;
    cotl::PublishRun(p, s, kTerms, 0);
    CHECK_EQ(p.i64["g.in_run"], int64_t{0});
    CHECK_EQ(p.i64["run.tarot_count"], int64_t{0});
    CHECK_EQ(p.CountPrefix("run.sel"), size_t{0});
}

TEST(publish_run_without_weapon) {
    GameSnapshot s;
    s.run = cotl::Run{};
    s.run->weapon = 9999; // EquipmentType.None
    FakePublisher p;
    cotl::PublishRun(p, s, kTerms, -1);
    CHECK_EQ(p.i64["run.has_weapon"], int64_t{0});
}

TEST(publish_map_rooms_and_doors) {
    GameSnapshot s;
    s.run = cotl::Run{};
    cotl::Room a, b, c;
    a.x = 0; a.y = 0; a.discovered = a.visited = true; a.doors = {false, true, false, false};
    b.x = 1; b.y = 0; b.discovered = true; b.kind = cotl::RoomKind::Boss; b.doors = {false, false, false, true};
    c.x = 0; c.y = 1; // undiscovered
    s.run->rooms = {a, b, c};
    FakePublisher p;
    cotl::PublishMap(p, s);
    CHECK_EQ(p.i64["map.count"], int64_t{2});
    CHECK_EQ(p.i64["map.sz"], int64_t{0});
    CHECK_EQ(p.i64["map.cur0"], int64_t{1});
    CHECK_EQ(p.i64["map.dim1"], int64_t{1});
    CHECK_EQ(p.i64["map.cur1"], int64_t{0});
    CHECK_EQ(p.i64["map.vis1"], int64_t{0});
    CHECK_EQ(p.i64["map.hcount"], int64_t{2}); // one stub each: a to the east, b to the west
    CHECK_EQ(p.i64["map.hdim0"] + p.i64["map.hdim1"], int64_t{1});
    CHECK_EQ(p.i64["map.vcount"], int64_t{0});
}

TEST(publish_map_icons_question_marks_player) {
    GameSnapshot s;
    s.run = cotl::Run{};
    cotl::Room a, b;
    a.x = 0; a.y = 0; a.discovered = a.visited = true;
    a.path = "Assets/_Rooms/Boss Room Dungeon 1_1.prefab";
    b.x = 1; b.y = 0; b.question = true;
    s.run->rooms = {a, b};
    FakePublisher p;
    cotl::PublishMap(p, s);
    REQUIRE(p.i64["map.count"] == 2);
    CHECK_EQ(p.i64["map.has_icon0"], int64_t{1});
    CHECK_EQ(p.text["map.icon0"], std::string("module:cotl:mapicon/Icon_Boss"));
    CHECK_EQ(p.i64["map.has_icon1"], int64_t{0});
    CHECK_EQ(p.i64["map.q0"], int64_t{0});
    CHECK_EQ(p.i64["map.q1"], int64_t{1});
    CHECK_EQ(p.i64["map.vis1"] + p.i64["map.dim1"] + p.i64["map.cur1"], int64_t{0});
}

TEST(publish_map_outside_run_is_empty) {
    GameSnapshot s;
    FakePublisher p;
    cotl::PublishMap(p, s);
    CHECK_EQ(p.i64["map.count"], int64_t{0});
    CHECK_EQ(p.i64["map.hcount"], int64_t{0});
    CHECK_EQ(p.i64["map.vcount"], int64_t{0});
}

TEST(publish_cult_name_level_storage) {
    GameSnapshot s;
    s.cult_name = "Lambs";
    s.cult_level = 2; // CultLevel.Three
    s.seed_stored = 17;
    s.seed_capacity = 265;
    FakePublisher p;
    cotl::PublishCult(p, s);
    CHECK_EQ(p.text["cult.name"], std::string("Lambs"));
    CHECK_EQ(p.i64["cult.level"], int64_t{3});
    CHECK_EQ(p.i64["cult.seed"], int64_t{17});
    CHECK_EQ(p.i64["cult.seed_cap"], int64_t{265});
    CHECK_EQ(p.i64["cult.has_seed"], int64_t{1});
    CHECK_EQ(p.i64["cult.has_fert"], int64_t{0});
}

TEST(publish_cult_without_name) {
    GameSnapshot s; // the name is only given through a follower quest in the game
    FakePublisher p;
    cotl::PublishCult(p, s);
    CHECK_EQ(p.text["cult.name"], std::string("Unnamed cult"));
}

TEST(publish_run_heart_slots_like_hud) {
    GameSnapshot s;
    cotl::Run run;
    run.hp_red = 3; // half hearts: 1 full, 1 half, 1 empty container at 6
    run.hp_red_max = 6;
    run.hp_spirit = 2;
    run.hp_spirit_max = 4; // 1 full, 1 empty
    run.hp_blue = 3;       // 1 full, 1 half
    run.hp_black = 2;
    run.hp_fire = 1;
    s.run = run;
    FakePublisher p;
    cotl::PublishRun(p, s, kTerms, -1);
    const std::vector<std::string> want = {"red_full", "red_half", "red_empty", "spirit_full",
                                           "spirit_empty", "blue_full", "blue_half", "black_full",
                                           "fire_half"};
    REQUIRE(p.i64["run.heart_count"] == static_cast<int64_t>(want.size()));
    for (size_t i = 0; i < want.size(); ++i) {
        CHECK_EQ(p.text["run.heart" + std::to_string(i)], "module:cotl:heart/" + want[i]);
    }
}

TEST(publish_run_equipment_icons) {
    GameSnapshot s;
    cotl::Run run;
    run.weapon = 2;
    run.curse = 500;
    run.relic = 29;
    s.run = run;
    FakePublisher p;
    cotl::PublishRun(p, s, kTerms, -1);
    CHECK_EQ(p.text["run.weapon_icon"], std::string("module:cotl:equip/2"));
    CHECK_EQ(p.text["run.curse_icon"], std::string("module:cotl:equip/500"));
    CHECK_EQ(p.text["run.relic_icon"], std::string("module:cotl:relic/29"));
}

TEST(publish_cult_dirt) {
    GameSnapshot s;
    s.poop = 3;
    s.vomit = 2;
    s.corpses = 1;
    FakePublisher p;
    cotl::PublishCult(p, s);
    CHECK_EQ(p.i64["cult.dirt"], int64_t{5}); // poop and vomit
    CHECK_EQ(p.i64["cult.corpses"], int64_t{1});
}

TEST(publish_run_hearts_outside_dungeon) {
    GameSnapshot s;
    FakePublisher p;
    cotl::PublishRun(p, s, kTerms, -1);
    CHECK_EQ(p.i64["run.heart_count"], int64_t{0});
}

// Holds the values and passes on only those of the visible page
TEST(recording_publisher_replays_by_page) {
    cotl::RecordingPublisher rec;
    rec.I64("fol.count", 3);
    rec.Text("fol.name0", "Okra");
    rec.I64("prob.n1", 2);
    rec.I64("prob.name_not_a_counter", 9);
    rec.F64("run.x", 1.5);
    rec.I64("map.count", 4);
    rec.I64("g.in_run", 1);
    rec.I64("cult.level", 2);
    FakePublisher followers;
    rec.Replay(followers, cotl::PageGroup::Followers);
    CHECK_EQ(followers.text["fol.name0"], std::string("Okra"));
    CHECK_EQ(followers.i64["fol.count"], int64_t{3});
    CHECK_EQ(followers.i64["g.in_run"], int64_t{1});
    CHECK_EQ(followers.i64["prob.n1"], int64_t{2}); // counters for the tab bar and Cult tab
    CHECK_EQ(followers.CountPrefix("map."), size_t{0});
    CHECK_EQ(followers.CountPrefix("prob.name"), size_t{0});
    FakePublisher all;
    rec.Replay(all, std::nullopt);
    CHECK_EQ(all.i64["map.count"], int64_t{4});
    CHECK_EQ(all.CountPrefix("prob.name"), size_t{1});
    rec.Clear();
    FakePublisher none;
    rec.Replay(none, std::nullopt);
    CHECK_EQ(none.i64.size(), size_t{0});
}

TEST(page_groups_cover_all_tabs) {
    CHECK(cotl::GroupOfPage("followers") == cotl::PageGroup::Followers);
    CHECK(cotl::GroupOfPage("inventory") == cotl::PageGroup::Items);
    CHECK(cotl::GroupOfPage("dungeon") == cotl::PageGroup::Dungeon);
    CHECK(cotl::GroupOfPage("cult") == cotl::PageGroup::Always);
    CHECK(!cotl::GroupOfPage("@settings").has_value());
}

TEST(keep_selection_holds_key_until_flag_changes) {
    cotl::KeepSelection<int32_t> sel;
    const auto key = [](int64_t row) { return static_cast<int32_t>(row * 10); };
    CHECK(!sel.Update(-1, key).has_value());
    CHECK_EQ(*sel.Update(2, key), 20);
    CHECK_EQ(*sel.Update(2, [](int64_t) { return 99; }), 20); // rows re-sorted: the key stays
    CHECK(!sel.Update(-1, key).has_value());
    CHECK_EQ(*sel.Update(2, [](int64_t) { return 99; }), 99); // newly opened
}

TEST(follower_row_and_id_are_inverse_in_list_order) {
    GameSnapshot s;
    s.followers = {F(1, "Prisoner", FollowerGroup::Prisoner), F(3, "Home")};
    CHECK_EQ(*cotl::FollowerIdAtRow(s, 0), 3);
    CHECK_EQ(*cotl::FollowerIdAtRow(s, 1), 1);
    CHECK(!cotl::FollowerIdAtRow(s, 2).has_value());
    CHECK_EQ(cotl::FollowerRowOf(s, 1), int64_t{1});
    CHECK_EQ(cotl::FollowerRowOf(s, 9), int64_t{-1});
}

TEST(inventory_selection_follows_item_when_list_changes) {
    GameSnapshot s;
    s.items = {{1, 12}, {20, 5}};
    const auto type = cotl::InventoryTypeAt(s, 1); // Currency, second row: LOG
    REQUIRE(type.has_value());
    const int64_t before = cotl::InventoryRowOf(s, *type);
    CHECK_EQ(before, int64_t{1});
    s.items = {{20, 5}, {1, 12}};
    CHECK_EQ(cotl::InventoryRowOf(s, *type), before);
    s.items = {{20, 5}}; // used up
    CHECK_EQ(cotl::InventoryRowOf(s, *type), int64_t{-1});
    CHECK(!cotl::InventoryTypeAt(s, 500).has_value());
}

TEST(publish_run_window_only_for_existing_card) {
    GameSnapshot s;
    s.run = cotl::Run{};
    s.run->tarot = {{0, 0}};
    FakePublisher open;
    cotl::PublishRun(open, s, kTerms, 0);
    CHECK_EQ(open.i64["run.sel_open"], int64_t{1});
    FakePublisher gone;
    cotl::PublishRun(gone, s, kTerms, 3);
    CHECK_EQ(gone.CountPrefix("run.sel"), size_t{0});
}

TEST(publish_map_counts_per_size) {
    GameSnapshot s;
    s.run = cotl::Run{};
    cotl::Room a;
    a.discovered = a.visited = true;
    a.doors = {false, true, false, false};
    s.run->rooms = {a};
    FakePublisher p;
    cotl::PublishMap(p, s);
    CHECK_EQ(p.i64["map.count0"], int64_t{1});
    CHECK_EQ(p.i64["map.hcount0"], int64_t{1});
    CHECK_EQ(p.CountPrefix("map.count1"), size_t{0});
}

// At the title screen there are default values, but no loaded state
TEST(publish_loaded_only_with_save_or_progress) {
    GameSnapshot title;
    title.day = 1;
    FakePublisher a;
    cotl::PublishCult(a, title);
    CHECK_EQ(a.i64["g.loaded"], int64_t{0});
    GameSnapshot loaded = title;
    loaded.save_loaded = true;
    FakePublisher b;
    cotl::PublishCult(b, loaded);
    CHECK_EQ(b.i64["g.loaded"], int64_t{1});
    GameSnapshot old = title;
    old.followers = {F(1, "A")};
    FakePublisher c;
    cotl::PublishCult(c, old);
    CHECK_EQ(c.i64["g.loaded"], int64_t{1});
}

// Image per row and in the window: image key of the head, empty without a known skin
TEST(publish_followers_portraits) {
    GameSnapshot s;
    s.followers = {F(1, "Cat"), F(2, "Nobody")};
    s.followers[0].skin = "Cat";
    s.followers[0].tints = {{"HEAD_SKIN_TOP", 0xFF8000FF}};
    FakePublisher p;
    cotl::PublishFollowers(p, s, kTerms);
    CHECK_EQ(p.text["fol.pic0"], std::string("module:cotl:follower/Cat;HEAD_SKIN_TOP=FF8000FF"));
    CHECK_EQ(p.text["fol.pic1"], std::string());
    FakePublisher sel;
    cotl::PublishSelectedFollower(sel, s, kTerms, 0);
    CHECK_EQ(sel.text["fol.sel_pic"], std::string("module:cotl:follower/Cat;HEAD_SKIN_TOP=FF8000FF"));
}
