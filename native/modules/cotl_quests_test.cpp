// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include "cotl_fake_publisher.h"
#include "cotl_i2.h"
#include "cotl_quests.h"
#include "cotl_test_util.h"

using cotl::BuildQuests;
using cotl::GameSnapshot;
using cotl::Objective;

namespace {

int32_t EnumValue(std::string_view enum_name, std::string_view ident) {
    for (int32_t v = -1; v < 2000; ++v) {
        if (cotl::Terms::EnumName(enum_name, v) == ident) return v;
    }
    return -999;
}

const cotl::I2Table& Table() {
    static const auto t = cotl::I2Table::FromMap({
        {"Objectives/GroupTitles/Quest", "Sample title A"},
        {"Objectives/GroupTitles/SickFollower", "Sample title B"},
        {"Objectives/CollectItem", "Collect {0} ({1}/{2})"},
        {"Objectives/BuildStructure", "Build <color=#FFD201>{0}</color>"},
        {"Objectives/BuildStructure/Plural", "Build <color=#FFD201>{0}</color> ({1}/{2})"},
        {"Objectives/SendFollowerToBedRest", "Send <color=#FFD201>{0}</color> to bed rest"},
        {"Objectives/RecruitFollower/Plural", "Hire {0} helpers ({1}/{2})"},
        {"Objectives/Custom/GiveSermon", "Sample sermon text"},
        {"Inventory/BERRY", "Berry"},
        {"Objectives/KillEnemies", "Kill {0} ({1}/{2})"},
        {"Enemies/Scamp", "Scamp"},
        {"Objectives/NoDodge", "No dodging {0}/{1} rooms"},
        {"Objectives/NoDamage", "No damage {0}/{1} rooms"},
        {"Objectives/NoCurses", "No curses {0}/{1} rooms"},
        {"Objectives/Custom/ShootCurse", "Shoot {0} Enemies {1}/{2}"},
        {"Objectives/RecruitCursedFollower/None", "Recruit <color=#FFD201>{0}</color>"},
        {"Objectives/RecruitCursedFollower/Ill", "Hire sick helpers"},
        {"Objectives/FindFollower", "Find <color=#FFD201>{0}</color> in {1}"},
        {"NAMES/Places/Dungeon1_1", "Darkwood"},
        {"Objectives/AssignClothing", "Assign <color=#FFD201>{0}</color> the <color=#FFD201>{1}</color> at the Tailor"},
        {"Objectives/CraftClothing", "Craft <color=#FFD201>{0}</color> at the Tailor"},
        {"Clothing/Robe/Name", "Fine Robe"},
        {"Objectives/Custom/BlizzardOffering", "Place {2} on the <color=#FFD201>Table of Offerings</color> ({0}/{1})"},
        {"Objectives/GetAnimal/ANIMAL_GOAT", "Bring a Level {0} <color=#FFD201>Yakish</color> to the Board"},
        {"Objectives/FeedAnimal", "Feed <color=#FFD201>{0}</color> some {1}"},
        {"Objectives/WalkAnimal", "Take <color=#FFD201>{0}</color> for a walk."},
        {"Inventory/ANIMAL_GOAT", "Yakish"},
        {"Objectives/ShowFleece", "Wear the <color=#FFD201>{0}</color>"},
        {"TarotCards/Fleece1/Name", "Golden Fleece"},
        {"Objectives/LegendaryDungeonRun", "Complete a <color=#FFD201>dungeon run</color> with <color=#FFD201>{0}</color>"},
        {"Objectives/GroupTitles/LegendaryAxe", "Legendary Axe"},
        {"UpgradeSystem/Axe_Legendary/Name", "Vorpal Axe"},
        {"Objectives/FindChildren/Temple", "Find <color=#FFD201>Aza</color> in <color=#FFD201>the Cult</color>"},
        {"Objectives/Flockade/WinBetFinal", "Bet <color=#FFD201>{0}</color> Wool against <color=#FFD201>{1}</color> ({2}/{3})"},
        {"Objectives/LegendarySwordReturn", "Go back to <color=#FFD201>Midas's Cave</color> in <color=#FFD201>{0}</color> days"},
        {"Objectives/PlaceDecorations", "Deco set {0}/{1}"},
        {"Objectives/PlaceWoolhavenDecos", "Town deco set ({0}/{1})"},
        {"Objectives/PlaceRotDecos", "Rot deco set ({0}/{1})"},
        {"Objectives/PlaceEwefallDecos", "Ewe deco set ({0}/{1})"},
        {"Objectives/PlaceSteampunkDecos", "Yew deco set ({0}/{1})"},
        {"Objectives/Custom/GiveItem", "Give <color=#FFD201>{0}</color> {1} ({2}/{3})"},
        {"Sozo", "Sozo"},
        {"Objectives/Custom/Knucklebones", "Beat {0} at dice"},
        {"Ratau", "Ratau"},
        {"Objectives/TalkToFollower", "Talk to <color=#FFD201>{0}</color>"},
        {"Objectives/CollectReward", "Collect reward from <color=#FFD201>{0}</color>"},
        {"Objectives/EatMeal", "Make <color=#FFD201>{0}</color> eat <color=#FFD201>{1}</color>"},
        {"CookingData/MEAL/Name", "Grass Meal"},
        {"Objectives/Drink", "Make <color=#FFD201>{0}</color> drink a <color=#FFD201>{1}</color>"},
        {"Objectives/Mating", "Send <color=#FFD201>{0}</color> and <color=#FFD201>{1}</color> to the Mating tent"},
        {"Objectives/Custom/UseRelic", "Use Relic"},
        {"Objectives/FinishRace", "Finish a race on a ranch animal in under <color=#FFD201>{0}</color> seconds"},
        {"Objectives/FlowerBaskets", "Fill pots {0}/{1}"},
        {"Objectives/BuildStructure/WinterDecorations", "Build <color=#FFD201>Winter decor</color> ({0}/{1})"},
    });
    return t;
}

const cotl::Terms kTerms(&Table());

cotl::Follower Named(int32_t id, const char* name) {
    cotl::Follower f;
    f.id = id;
    f.name = name;
    return f;
}

} // namespace

TEST(quests_strip_markup) {
    CHECK_EQ(cotl::StripMarkup("Build <color=#FFD201>Shrine</color> <sprite name=\"x\">now"),
             std::string("Build Shrine now"));
}

TEST(quests_format_term_drops_unknown_progress) {
    CHECK_EQ(cotl::FormatTerm("Collect {0} ({1}/{2})", {"Berry", "3", "5"}),
             std::string("Collect Berry (3/5)"));
    CHECK_EQ(cotl::FormatTerm("Hire {0} helpers ({1}/{2})", {"2", "", "2"}),
             std::string("Hire 2 helpers"));
}

TEST(quests_collect_item_with_progress_and_giver) {
    GameSnapshot s;
    s.day = 1;
    s.followers = {Named(7, "Okra")};
    Objective q;
    q.type = 1; // COLLECT_ITEM
    q.follower_id = 7;
    q.group_id = "Objectives/GroupTitles/Quest";
    q.item_type = EnumValue("ItemType", "BERRY");
    q.ints = {q.item_type, 0, 5, 0, 3}; // ItemType @0x78, Target @0x80, Count @0x88
    q.has_expiry = true;
    q.expire = 1200 + 2 * 1200;
    s.objectives = {q};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 1);
    CHECK_EQ(qs[0].title, std::string("Sample title A"));
    CHECK_EQ(qs[0].text, std::string("Collect Berry"));
    CHECK_EQ(qs[0].giver, std::string("Okra"));
    CHECK_EQ(qs[0].progress, 3);
    CHECK_EQ(qs[0].target, 5);
    CHECK_EQ(qs[0].due, std::string("2 days left"));
}

TEST(quests_story_quest_without_follower_is_listed) {
    GameSnapshot s;
    Objective q;
    q.type = 5; // CUSTOM
    q.follower_id = -1;
    q.group_id = "Objectives/GroupTitles/SickFollower";
    q.ints = {EnumValue("CustomQuest", "GiveSermon"), -1, 0, 0, 0};
    s.objectives = {q};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 1);
    CHECK_EQ(qs[0].title, std::string("Sample title B"));
    CHECK_EQ(qs[0].text, std::string("Sample sermon text"));
    CHECK_EQ(qs[0].giver, std::string(""));
    CHECK_EQ(qs[0].due, std::string(""));
}

TEST(quests_build_structure_singular_and_plural) {
    GameSnapshot s;
    Objective one;
    one.type = 2;
    one.ints = {EnumValue("StructureType", "SHRINE"), 1, 0, 0, 0};
    Objective many = one;
    many.ints = {EnumValue("StructureType", "SHRINE"), 3, 1, 0, 0}; // Target 3, Count 1
    s.objectives = {one, many};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 2);
    CHECK_EQ(qs[0].text, std::string("Build SHRINE")); // without an I2 term: identifier
    CHECK_EQ(qs[1].text, std::string("Build SHRINE"));
    CHECK_EQ(qs[1].progress, 1);
    CHECK_EQ(qs[1].target, 3);
}

TEST(quests_bed_rest_uses_name_field) {
    GameSnapshot s;
    Objective q;
    q.type = 15; // SEND_FOLLOWER_BED_REST
    q.str78 = "Gremer";
    s.objectives = {q};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 1);
    CHECK_EQ(qs[0].text, std::string("Send Gremer to bed rest"));
}

TEST(quests_unknown_type_falls_back_to_type_name) {
    GameSnapshot s;
    Objective q;
    q.type = 19; // PERFORM_RITUAL, not replicated
    s.objectives = {q};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 1);
    CHECK_EQ(qs[0].text, std::string("Perform ritual"));
    CHECK_EQ(qs[0].title, std::string("Quest"));
}

TEST(quests_offered_first_then_by_time_done_hidden) {
    GameSnapshot s;
    s.day = 1;
    s.followers = {Named(1, "Asker")};
    s.offered_quest_follower_ids = {1};
    Objective later;
    later.type = 5;
    later.has_expiry = true;
    later.expire = 1200 + 3 * 1200;
    Objective sooner = later;
    sooner.expire = 1200 + 600;
    Objective done = later;
    done.complete = true;
    s.objectives = {later, sooner, done};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 3);
    CHECK(qs[0].offered);
    CHECK_EQ(qs[0].giver, std::string("Asker"));
    CHECK_EQ(qs[1].due, std::string("due today"));
    CHECK_EQ(qs[2].due, std::string("3 days left"));
}

TEST(quests_published_with_selection) {
    std::vector<cotl::QuestView> qs(2);
    qs[0].title = "A";
    qs[0].text = "Do A";
    qs[1].title = "B";
    qs[1].text = "Do B";
    qs[1].progress = 2;
    qs[1].target = 4;
    FakePublisher p;
    cotl::PublishQuests(p, qs, 1);
    CHECK_EQ(p.i64["quest.count"], int64_t{2});
    CHECK_EQ(p.text["quest.title0"], std::string("A"));
    CHECK_EQ(p.text["quest.sel_title"], std::string("B"));
    CHECK_EQ(p.text["quest.sel_due"], std::string(""));
}

TEST(quests_empty) {
    GameSnapshot s;
    CHECK(BuildQuests(s, kTerms).empty());
    FakePublisher p;
    cotl::PublishQuests(p, {}, -1);
    CHECK_EQ(p.i64["quest.count"], int64_t{0});
    CHECK_EQ(p.CountPrefix("quest.sel"), size_t{0});
}

TEST(quests_unlock_uses_upgrade_term) {
    const auto t = cotl::I2Table::FromMap({{"Objectives/Unlock/Building_Graves", "Unlock Graves"}});
    const cotl::Terms terms(&t);
    GameSnapshot s;
    Objective q;
    q.type = 20; // UNLOCK: UnlockType (UpgradeSystem.Type) @0x78
    q.ints = {EnumValue("UpgradeType", "Building_Graves"), 0, 0, 0, 0};
    s.objectives = {q};
    const auto qs = BuildQuests(s, terms);
    REQUIRE(qs.size() == 1);
    CHECK_EQ(qs[0].text, std::string("Unlock Graves"));
}

// One quest = all objectives with the same UniqueGroupID (QuestsMenu.OnShowStarted), also the
// completed ones from DataManager.CompletedObjectives
TEST(quests_group_steps_by_unique_group) {
    GameSnapshot s;
    Objective done;
    done.type = 5;
    done.unique_group = "g1";
    done.group_id = "Objectives/GroupTitles/SickFollower";
    done.complete = true;
    done.ints = {EnumValue("CustomQuest", "GiveSermon"), -1, 0, 0, 0};
    Objective open;
    open.type = 1;
    open.unique_group = "g1";
    open.group_id = "Objectives/GroupTitles/SickFollower";
    open.ints = {EnumValue("ItemType", "BERRY"), 0, 5, 0, 2};
    Objective other;
    other.type = 5;
    other.unique_group = "g2";
    s.objectives = {open, other, done};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 2);
    CHECK_EQ(qs[0].title, std::string("Sample title B"));
    CHECK_EQ(qs[0].text, std::string("Collect Berry")); // first open step
    REQUIRE(qs[0].steps.size() == 2);
    CHECK_EQ(qs[0].steps[0].text, std::string("Collect Berry"));
    CHECK(!qs[0].steps[0].done);
    CHECK_EQ(qs[0].steps[0].progress, 2);
    CHECK_EQ(qs[0].steps[1].text, std::string("Sample sermon text"));
    CHECK(qs[0].steps[1].done);
}

TEST(quests_group_without_open_step_is_not_listed) {
    GameSnapshot s;
    Objective done;
    done.type = 5;
    done.unique_group = "g1";
    done.complete = true;
    s.objectives = {done};
    CHECK(BuildQuests(s, kTerms).empty());
}

TEST(quests_perform_ritual_names_ritual_and_follower) {
    const auto t = cotl::I2Table::FromMap({
        {"Objectives/PerformRitual/Ritual_Sacrifice", "Sacrifice <color=#FFD201>{0}</color> now"},
        {"Objectives/PerformRitual/Ritual_Halloween", "Sample ritual text"},
    });
    const cotl::Terms terms(&t);
    GameSnapshot s;
    s.followers = {Named(4, "Gremer")};
    Objective sacrifice;
    sacrifice.type = 19; // PERFORM_RITUAL: Ritual @0x78, TargetFollowerID_1 @0x7C, _2 @0x80
    sacrifice.ints = {EnumValue("UpgradeType", "Ritual_Sacrifice"), 4, -1, 0, 0};
    Objective blood = sacrifice;
    blood.ints = {EnumValue("UpgradeType", "Ritual_Halloween"), -1, -1, 0, 0};
    s.objectives = {sacrifice, blood};
    const auto qs = BuildQuests(s, terms);
    REQUIRE(qs.size() == 2);
    CHECK_EQ(qs[0].text, std::string("Sacrifice Gremer now"));
    CHECK_EQ(qs[1].text, std::string("Sample ritual text"));
}

TEST(quests_published_steps_for_selection) {
    std::vector<cotl::QuestView> qs(1);
    qs[0].title = "T";
    qs[0].text = "Collect Berry";
    qs[0].steps = {{"Collect Berry", false, 2, 5}, {"Sample step", true}};
    FakePublisher p;
    cotl::PublishQuests(p, qs, 0);
    CHECK_EQ(p.text["quest.more0"], std::string("+1 more step"));
    CHECK_EQ(p.i64["quest.step_count"], int64_t{2});
    CHECK_EQ(p.text["quest.step0"], std::string("Collect Berry  2/5"));
    CHECK_EQ(p.i64["quest.step_done0"], int64_t{0});
    CHECK_EQ(p.text["quest.step1"], std::string("Sample step"));
    CHECK_EQ(p.i64["quest.step_done1"], int64_t{1});
}

namespace {

// Text of the first open step of a single objective
std::string TextOf(const Objective& q, const GameSnapshot& base = {}) {
    GameSnapshot s = base;
    s.objectives = {q};
    const auto qs = BuildQuests(s, kTerms);
    return qs.empty() ? std::string("<none>") : qs[0].text;
}

Objective Of(int32_t type, std::array<int32_t, 5> ints) {
    Objective q;
    q.type = type;
    q.follower_id = -1;
    q.ints = ints;
    return q;
}

} // namespace

// Objectives_PlaceStructure: DecoType @0x7C, Count @0x80, Target @0x84
TEST(quests_place_structures_shows_decorations_and_progress) {
    CHECK_EQ(TextOf(Of(18, {0, 0, 0, 3, 0})), std::string("Deco set 0/3"));
    CHECK_EQ(TextOf(Of(18, {0, 1, 1, 2, 0})), std::string("Town deco set (1/2)"));
    CHECK_EQ(TextOf(Of(18, {0, 2, 0, 4, 0})), std::string("Rot deco set (0/4)"));
    CHECK_EQ(TextOf(Of(18, {0, 3, 0, 4, 0})), std::string("Ewe deco set (0/4)"));
    CHECK_EQ(TextOf(Of(18, {0, 4, 0, 4, 0})), std::string("Yew deco set (0/4)"));
}

// Objectives_GiveItem: NPCTerm @0x78, Target @0x84, TargetType @0x88; Fortschritt = Bestand
TEST(quests_give_item_names_npc_item_and_owned) {
    const int32_t berry = EnumValue("ItemType", "BERRY");
    GameSnapshot s;
    s.items = {{berry, 2}};
    auto q = Of(33, {0, 0, 0, 5, berry});
    q.str78 = "Sozo";
    CHECK_EQ(TextOf(q, s), std::string("Give Sozo Berry (2/5)"));
}

TEST(quests_knucklebones_names_opponent) {
    auto q = Of(24, {});
    q.str78 = "Ratau";
    CHECK_EQ(TextOf(q), std::string("Beat Ratau at dice"));
}

TEST(quests_follower_targets) {
    GameSnapshot s;
    s.followers = {Named(7, "Okra"), Named(8, "Bean")};
    CHECK_EQ(TextOf(Of(16, {0, 0, 0, 0, 7}), s), std::string("Talk to Okra")); // TargetFollower @0x88
    CHECK_EQ(TextOf(Of(29, {7, 8, 0, 0, 0}), s), std::string("Send Okra and Bean to the Mating tent"));
    CHECK_EQ(TextOf(Of(21, {EnumValue("StructureType", "MEAL"), 7, 0, 0, 0}), s),
             std::string("Make Okra eat Grass Meal"));
    auto reward = Of(16, {0, 0, 0, 0, -1}); // without a target: collect the reward from the quest giver
    reward.follower_id = 8;
    s.objectives = {reward};
    CHECK_EQ(BuildQuests(s, kTerms)[0].text, std::string("Collect reward from Bean"));
}

TEST(quests_simple_terms_and_counts) {
    CHECK_EQ(TextOf(Of(26, {})), std::string("Use Relic"));
    CHECK_EQ(TextOf(Of(39, {1, 4, 0, 0, 0})), std::string("Fill pots 1/4"));
    CHECK_EQ(TextOf(Of(42, {3, 2, 0, 0, 0})), std::string("Build Winter decor (2/3)")); // Target, Count
    const float seconds = 45.0f;
    int32_t bits = 0;
    std::memcpy(&bits, &seconds, 4);
    CHECK_EQ(TextOf(Of(32, {bits, 0, 0, 0, 0})),
             std::string("Finish a race on a ranch animal in under 45 seconds"));
}

TEST(quests_kill_enemies_counts_kills_since_start) {
    const int32_t scamp = EnumValue("Enemy", "Scamp");
    GameSnapshot s;
    s.enemies_killed = {{scamp, 12}};
    CHECK_EQ(TextOf(Of(8, {scamp, 10, 5, 0, 0}), s), std::string("Kill Scamp (2/5)"));
}

TEST(quests_room_challenges_and_shooting) {
    CHECK_EQ(TextOf(Of(9, {3, 1, 0, 0, 0})), std::string("No dodging 1/3 rooms"));
    CHECK_EQ(TextOf(Of(10, {3, 2, 0, 0, 0})), std::string("No damage 2/3 rooms"));
    CHECK_EQ(TextOf(Of(11, {4, 0, 0, 0, 0})), std::string("No curses 0/4 rooms"));
    CHECK_EQ(TextOf(Of(13, {3, 0, 0, 0, 0})), std::string("Shoot 3 Enemies"));
}

TEST(quests_recruit_cursed_named_or_by_state) {
    auto named = Of(22, {});
    named.str2 = "Okra";
    CHECK_EQ(TextOf(named), std::string("Recruit Okra"));
    CHECK_EQ(TextOf(Of(22, {EnumValue("Thought", "Ill"), 3, 1, 0, 0})), std::string("Hire sick helpers (1/3)"));
}

TEST(quests_find_follower_names_place) {
    auto q = Of(23, {EnumValue("FollowerLocation", "Dungeon1_1"), 0, 0, 0, 0});
    q.str2 = "Leshy";
    CHECK_EQ(TextOf(q), std::string("Find Leshy in Darkwood"));
}

TEST(quests_clothing) {
    const int32_t robe = EnumValue("ClothingType", "Robe");
    GameSnapshot s;
    s.followers = {Named(7, "Okra")};
    CHECK_EQ(TextOf(Of(27, {robe, 7, 0, 0, 0}), s), std::string("Assign Okra the Fine Robe at the Tailor"));
    CHECK_EQ(TextOf(Of(30, {robe, 0, 0, 0, 0})), std::string("Craft Fine Robe at the Tailor"));
}

TEST(quests_blizzard_offering_uses_offered_amount) {
    const int32_t berry = EnumValue("ItemType", "BERRY");
    GameSnapshot s;
    s.offerings = {{berry, 2}};
    CHECK_EQ(TextOf(Of(31, {5, berry, 0, 0, 0}), s),
             std::string("Place Berry on the Table of Offerings (2/5)"));
}

TEST(quests_animals) {
    const int32_t goat = EnumValue("ItemType", "ANIMAL_GOAT");
    const int32_t berry = EnumValue("ItemType", "BERRY");
    GameSnapshot s;
    s.animals = {{goat, 9, "Daisy"}, {goat, 10, ""}};
    CHECK_EQ(TextOf(Of(34, {goat, 2, 0, 0, 0})), std::string("Bring a Level II Yakish to the Board"));
    CHECK_EQ(TextOf(Of(41, {9, berry, 0, 0, 0}), s), std::string("Feed Daisy some Berry"));
    CHECK_EQ(TextOf(Of(43, {9, 0, 0, 0, 0}), s), std::string("Take Daisy for a walk."));
    CHECK_EQ(TextOf(Of(43, {10, 0, 0, 0, 0}), s), std::string("Take Yakish for a walk.")); // without a name
}

TEST(quests_fleece_children_flockade_sword) {
    CHECK_EQ(TextOf(Of(35, {1, 0, 0, 0, 0})), std::string("Wear the Golden Fleece"));
    CHECK_EQ(TextOf(Of(37, {1, 0, 0, 0, 0})), std::string("Find Aza in the Cult"));
    auto bet = Of(38, {0, 0, 4, 9, 0}); // term @0x78, bet @0x80, needed @0x84
    bet.str78 = "Ratau";
    CHECK_EQ(TextOf(bet), std::string("Bet 4 Wool against Ratau (9/4)"));
    GameSnapshot s;
    s.day = 42;
    s.sword_return_day = 41; // one day ago: still 2 of 3 days
    CHECK_EQ(TextOf(Of(40, {}), s), std::string("Go back to Midas's Cave in 2 days"));
}

// As long as the weapon is unknown, the game only names the weapon kind
TEST(quests_legendary_run_hides_unknown_weapon_name) {
    const int32_t axe = EnumValue("EquipmentType", "Axe_Legendary");
    GameSnapshot s;
    CHECK_EQ(TextOf(Of(36, {axe, 0, 0, 0, 0}), s), std::string("Complete a dungeon run with Legendary Axe"));
    s.legendary_known = {axe};
    CHECK_EQ(TextOf(Of(36, {axe, 0, 0, 0, 0}), s), std::string("Complete a dungeon run with Vorpal Axe"));
}

// Key for the selection: stays when new quests are sorted in before it
TEST(quests_have_stable_keys) {
    GameSnapshot s;
    s.followers = {Named(7, "Okra")};
    s.offered_quest_follower_ids = {7};
    auto a = Of(26, {});
    a.unique_group = "groupA";
    s.objectives = {a};
    const auto qs = BuildQuests(s, kTerms);
    REQUIRE(qs.size() == 2);
    CHECK_EQ(qs[0].key, std::string("offer:7"));
    CHECK_EQ(qs[1].key, std::string("groupA"));
}
