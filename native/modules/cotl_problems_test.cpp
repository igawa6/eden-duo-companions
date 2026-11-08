// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_fake_publisher.h"
#include "cotl_problems.h"
#include "cotl_test_util.h"

using cotl::DeriveProblems;
using cotl::Follower;
using cotl::GameSnapshot;
using cotl::Objective;
using cotl::ProblemKind;

namespace {

const cotl::Terms kTerms(nullptr);

Follower Healthy(int32_t id, const char* name) {
    Follower f;
    f.id = id;
    f.name = name;
    f.level = 1;
    f.satiation = 80;
    return f;
}

GameSnapshot At(int32_t day, float time) {
    GameSnapshot s;
    s.day = day;
    s.time = time;
    return s;
}

} // namespace

TEST(problems_sorted_by_urgency) {
    auto s = At(5, 0);
    auto dissent = Healthy(1, "Dissent");
    dissent.cursed_state = cotl::kThoughtDissenter;
    auto ill = Healthy(2, "Ill");
    ill.cursed_state = cotl::kThoughtIll;
    auto starving = Healthy(3, "Starving");
    starving.cursed_state = cotl::kThoughtStarving;
    s.followers = {dissent, ill, starving};
    const auto rows = DeriveProblems(s, kTerms);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].kind == ProblemKind::Starving);
    CHECK(rows[1].kind == ProblemKind::Ill);
    CHECK(rows[2].kind == ProblemKind::Dissent);
    CHECK_EQ(rows[0].name, std::string("Starving"));
}

TEST(problems_hungry_below_game_threshold) {
    auto s = At(5, 0);
    auto hungry = Healthy(1, "Hungry");
    hungry.satiation = 29;
    auto fed = Healthy(2, "Fed");
    fed.satiation = 30; // HUNGER_THRESHOLD: hungry only below this
    s.followers = {hungry, fed};
    const auto rows = DeriveProblems(s, kTerms);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ProblemKind::Hungry);
}

// Quests belong in the Quests tab, not with the problems
TEST(problems_ignore_quests) {
    auto s = At(1, 0);
    s.followers = {Healthy(7, "Okra")};
    Objective q;
    q.type = 1;
    q.follower_id = 7;
    s.objectives = {q};
    s.offered_quest_follower_ids = {7};
    CHECK(DeriveProblems(s, kTerms).empty());
}

TEST(problems_filter_lists_every_follower_with_that_kind) {
    auto s = At(1, 0);
    auto ill_and_tired = Healthy(1, "IllTired");
    ill_and_tired.cursed_state = cotl::kThoughtIll;
    ill_and_tired.exhaustion = 3;
    auto tired = Healthy(2, "Tired");
    tired.exhaustion = 1;
    auto hungry = Healthy(3, "Hungry");
    hungry.satiation = 5;
    s.followers = {ill_and_tired, tired, hungry};
    const auto rows = DeriveProblems(s, kTerms, static_cast<int32_t>(ProblemKind::Exhausted));
    REQUIRE(rows.size() == 2);
    CHECK_EQ(rows[0].name, std::string("IllTired"));
    CHECK(rows[0].kind == ProblemKind::Exhausted);
    CHECK_EQ(rows[0].reason, std::string("Exhausted, +1 more"));
    CHECK_EQ(rows[1].name, std::string("Tired"));
}

TEST(problems_one_row_per_follower_with_more_count) {
    auto s = At(1, 0);
    auto f = Healthy(1, "Busy");
    f.cursed_state = cotl::kThoughtIll;
    f.satiation = 10;  // hungry
    f.exhaustion = 5;  // exhausted
    s.followers = {f};
    const auto rows = DeriveProblems(s, kTerms);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ProblemKind::Ill);
    CHECK_EQ(rows[0].reason, std::string("Ill, +2 more")); // no parentheses
}

TEST(problems_old_age_and_exhaustion) {
    auto s = At(1, 0);
    auto old = Healthy(1, "Old");
    old.cursed_state = cotl::kThoughtOldAge;
    auto tired = Healthy(2, "Tired");
    tired.exhaustion = 0.5f; // > 0 = exhausted (FollowerBrainStats.set_Exhaustion)
    s.followers = {old, tired};
    const auto rows = DeriveProblems(s, kTerms);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].kind == ProblemKind::Exhausted);
    CHECK(rows[1].kind == ProblemKind::Old);
    CHECK_EQ(static_cast<int>(ProblemKind::Old), 7); // manifest order (PROBLEM_KINDS)
}

TEST(problems_cursed_state_uses_thought_name) {
    auto s = At(1, 0);
    auto f = Healthy(1, "Cold");
    f.cursed_state = cotl::kThoughtFreezing;
    s.followers = {f};
    const auto rows = DeriveProblems(s, kTerms);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].kind == ProblemKind::Cursed);
    CHECK_EQ(rows[0].reason, std::string("Freezing"));
}

TEST(problems_healthy_and_empty) {
    auto s = At(1, 0);
    CHECK(DeriveProblems(s, kTerms).empty());
    s.followers = {Healthy(1, "Fine")};
    CHECK(DeriveProblems(s, kTerms).empty());
}

TEST(problems_published_with_counts) {
    auto s = At(1, 0);
    auto a = Healthy(1, "A");
    a.cursed_state = cotl::kThoughtIll;
    a.satiation = 10;
    auto b = Healthy(2, "B");
    b.satiation = 10;
    s.followers = {a, b};
    const auto rows = DeriveProblems(s, kTerms);
    FakePublisher p;
    cotl::PublishProblems(p, s, rows);
    CHECK_EQ(p.i64["prob.count"], int64_t{2});
    CHECK_EQ(p.i64["prob.n1"], int64_t{1}); // Ill
    CHECK_EQ(p.i64["prob.n3"], int64_t{2}); // Hungry: both followers
    CHECK_EQ(p.text["prob.name0"], std::string("A"));
    CHECK_EQ(p.i64["prob.kind1"], int64_t{3});
}

// The number in the tab bar counts all affected followers, independent of the filter
TEST(problems_total_ignores_filter) {
    auto s = At(1, 0);
    auto ill = Healthy(1, "Ill");
    ill.cursed_state = cotl::kThoughtIll;
    auto tired = Healthy(2, "Tired");
    tired.exhaustion = 1;
    s.followers = {ill, tired, Healthy(3, "Fine")};
    FakePublisher p;
    cotl::PublishProblems(p, s, DeriveProblems(s, kTerms, static_cast<int32_t>(ProblemKind::Ill)));
    CHECK_EQ(p.i64["prob.count"], int64_t{1});
    CHECK_EQ(p.i64["prob.total"], int64_t{2});
}

TEST(problems_selected_follower_lists_all_problems_by_urgency) {
    auto s = At(1, 0);
    auto f = Healthy(7, "Busy");
    f.cursed_state = cotl::kThoughtIll;
    f.satiation = 10; // hungry
    f.exhaustion = 5; // exhausted
    f.level = 3;
    s.followers = {Healthy(1, "Fine"), f};
    FakePublisher p;
    cotl::PublishSelectedProblems(p, s, 7);
    CHECK_EQ(p.text["prob.sel_name"], std::string("Busy"));
    CHECK_EQ(p.i64["prob.sel_level"], int64_t{3});
    REQUIRE(p.i64["prob.sel_count"] == 3);
    CHECK_EQ(p.i64["prob.sel_kind0"], static_cast<int64_t>(ProblemKind::Ill));
    CHECK_EQ(p.text["prob.sel_reason0"], std::string("Ill"));
    CHECK_EQ(p.text["prob.sel_reason1"], std::string("Hungry"));
    CHECK_EQ(p.text["prob.sel_reason2"], std::string("Exhausted"));
}

TEST(problems_selected_unknown_follower_is_empty) {
    auto s = At(1, 0);
    s.followers = {Healthy(1, "Fine")};
    FakePublisher p;
    cotl::PublishSelectedProblems(p, s, 99);
    CHECK_EQ(p.i64["prob.sel_count"], int64_t{0});
    FakePublisher closed;
    cotl::PublishSelectedProblems(closed, s, -1);
    CHECK_EQ(closed.i64["prob.sel_count"], int64_t{0});
    CHECK_EQ(closed.CountPrefix("prob.sel_name"), size_t{0});
}

// The window stays on the tapped follower, even if the rows re-sort
TEST(problems_selection_keeps_follower_when_rows_reorder) {
    cotl::ProblemRow a, b;
    a.follower_id = 1;
    b.follower_id = 2;
    cotl::ProblemSelection sel;
    CHECK_EQ(sel.Update(-1, {a, b}), -1);
    CHECK_EQ(sel.Update(1, {a, b}), 2);
    CHECK_EQ(sel.Update(1, {b, a}), 2); // re-sorted: still follower 2
    CHECK_EQ(sel.Update(-1, {b, a}), -1);
    CHECK_EQ(sel.Update(1, {b, a}), 1); // newly opened: current row
    CHECK_EQ(sel.Update(5, {b, a}), -1); // that row does not exist
}

TEST(problems_published_with_portraits) {
    auto s = At(1, 0);
    auto a = Healthy(1, "A");
    a.satiation = 10;
    a.skin = "Dog";
    s.followers = {a};
    FakePublisher p;
    cotl::PublishProblems(p, s, DeriveProblems(s, kTerms));
    CHECK_EQ(p.text["prob.pic0"], std::string("module:cotl:follower/Dog"));
    FakePublisher sel;
    cotl::PublishSelectedProblems(sel, s, 1);
    CHECK_EQ(sel.text["prob.sel_pic"], std::string("module:cotl:follower/Dog"));
}
