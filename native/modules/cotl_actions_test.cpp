// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Module side of the follower actions: ActionClient over a recording fake mailbox.
#include <map>
#include <string>
#include <vector>

#include <algorithm>
#include "cotl_actions.h"
#include "cotl_fake_publisher.h"
#include "cotl_action_abi.h"
#include "cotl_publish.h"
#include "cotl_test_util.h"

namespace {

using cotl::ActionClient;
using cotl::ActionReason;
using cotl::ActionState;
using cotl::ActionStatus;

constexpr uint32_t kBless = 59;

std::vector<cotl::ActionRow> BlessRow() {
    return cotl::VisibleActions({{kBless, true}});
}

// Follower with no special traits on a save with a loyalty bar, no doctrine
cotl::FollowerCommandState PlainFollower() {
    cotl::FollowerCommandState s;
    s.follower_id = 7;
    s.show_loyalty_bars = true;
    return s;
}

cotl::CommandGroup GroupOf(const cotl::FollowerCommandState& s) {
    return cotl::SelectCommandGroup(s);
}

bool OffersBless(const cotl::FollowerCommandState& s) {
    for (const auto& c : cotl::OfferedCommands(s)) {
        if (c.command == kBless) return true;
    }
    return false;
}

// Mailbox recording all writes (offset, value) in order
struct FakeMailbox : cotl::MailboxIo {
    std::map<uint32_t, uint32_t> words;
    std::vector<std::pair<uint32_t, uint32_t>> stores;
    bool hooked = true;

    bool Load(uint32_t off, uint32_t* v) override {
        if (!hooked) return false;
        *v = words[off];
        return true;
    }
    bool Store(uint32_t off, uint32_t v) override {
        if (!hooked) return false;
        stores.emplace_back(off, v);
        words[off] = v;
        return true;
    }
    uint32_t& operator[](uint32_t off) { return words[off]; }

    // The helper is running: the heartbeat counts up
    void Beat() { ++words[COTL_ACT_HEARTBEAT]; }

    // Imitation of the helper's acceptance (action_tick.c): handle a new sequence number once
    void HelperStep(uint32_t state, uint32_t reason, bool busy = false) {
        Beat();
        const uint32_t seq = words[COTL_ACT_REQ_SEQ];
        if (!seq || seq == words[COTL_ACT_ST_SEQ] || seq == words[COTL_ACT_ST_BUSY_SEQ]) return;
        if (busy) {
            words[COTL_ACT_ST_BUSY_SEQ] = seq;
            return;
        }
        words[COTL_ACT_ST_REASON] = reason;
        words[COTL_ACT_ST_STATE] = state;
        words[COTL_ACT_ST_SEQ] = seq;
    }
};

// A client with a live helper: the heartbeat moves across several polls
struct Rig {
    FakeMailbox box;
    ActionClient client{box};
    Rig() {
        for (int i = 0; i < 3; ++i) {
            box.Beat();
            client.Poll();
        }
    }
};

} // namespace

TEST(Submit_writes_fields_before_seq) {
    Rig r;
    r.box[COTL_ACT_EPOCH] = 7;
    r.box.stores.clear();
    REQUIRE(r.client.Submit(1234, kBless));
    REQUIRE(r.box.stores.size() == 5);
    // REQ_SEQ last; before it follower, command, parent, epoch (any order among the four)
    CHECK_EQ(r.box.stores.back().first, uint32_t{COTL_ACT_REQ_SEQ});
    for (size_t i = 0; i + 1 < r.box.stores.size(); ++i) CHECK(r.box.stores[i].first != COTL_ACT_REQ_SEQ);
    CHECK_EQ(r.box[COTL_ACT_REQ_FOLLOWER], uint32_t{1234});
    CHECK_EQ(r.box[COTL_ACT_REQ_COMMAND], kBless);
    CHECK_EQ(r.box[COTL_ACT_REQ_EPOCH], uint32_t{7}); // current epoch of the host
    CHECK(r.box[COTL_ACT_REQ_SEQ] != 0);
    // The module never writes the helper's epoch and status
    for (const auto& [off, v] : r.box.stores) {
        CHECK(off != COTL_ACT_EPOCH && off != COTL_ACT_HEARTBEAT);
        CHECK(off < COTL_ACT_ST_SEQ || off == COTL_ACT_CANCEL_SEQ);
    }
}

// Tear protection: while the last sequence number is unacknowledged, every REQ_* field stays untouched
TEST(Submit_waits_for_ack_before_touching_fields) {
    Rig r;
    REQUIRE(r.client.Submit(1, kBless));
    const auto n = r.box.stores.size();
    const uint32_t follower = r.box[COTL_ACT_REQ_FOLLOWER];
    CHECK(!r.client.Submit(2, kBless)); // no reply from the helper yet
    CHECK_EQ(r.box.stores.size(), n);   // nothing written, not even follower/command
    CHECK_EQ(r.box[COTL_ACT_REQ_FOLLOWER], follower);
    r.box.HelperStep(COTL_ACT_STATE_REFUSED, COTL_ACT_REASON_NOT_AT_BASE);
    r.client.Poll();
    CHECK(r.client.Submit(2, kBless));
    CHECK_EQ(r.box[COTL_ACT_REQ_FOLLOWER], uint32_t{2});
}

TEST(Double_tap_submits_once) {
    Rig r;
    REQUIRE(r.client.Submit(5, kBless));
    const auto n = r.box.stores.size();
    const uint32_t seq = r.box[COTL_ACT_REQ_SEQ];
    CHECK(!r.client.Submit(5, kBless));
    CHECK(!r.client.Submit(5, kBless));
    CHECK_EQ(r.box.stores.size(), n);
    CHECK_EQ(r.box[COTL_ACT_REQ_SEQ], seq);
}

TEST(Submit_refused_while_busy) {
    Rig r;
    REQUIRE(r.client.Submit(5, kBless));
    r.box.HelperStep(COTL_ACT_STATE_WALKING, COTL_ACT_REASON_NONE);
    r.client.Poll();
    const uint32_t first = r.box[COTL_ACT_REQ_SEQ];
    // While the action runs, a second request goes to the helper, which rejects it as busy
    REQUIRE(r.client.Submit(6, kBless));
    CHECK(r.box[COTL_ACT_REQ_SEQ] != first);
    r.box.HelperStep(0, 0, true);
    const ActionStatus st = r.client.Poll();
    CHECK(st.state == ActionState::Refused);
    CHECK(st.reason == ActionReason::LambBusy);
    CHECK_EQ(r.box[COTL_ACT_ST_SEQ], first); // the running action stays untouched
    // After that (BUSY_SEQ == sequence number) sending is allowed again
    CHECK(r.client.Submit(6, kBless));
}

TEST(Submit_refused_when_helper_dead) {
    Rig r;
    CHECK(r.client.Poll().helper_alive);
    for (int i = 0; i < 58; ++i) CHECK(r.client.Poll().helper_alive); // heartbeat is stuck (polls 2..59)
    CHECK(!r.client.Poll().helper_alive); // 60th poll without change
    const auto n = r.box.stores.size();
    CHECK(!r.client.Submit(1, kBless));
    CHECK_EQ(r.box.stores.size(), n);
    // The helper is running again: it works
    r.box.Beat();
    CHECK(r.client.Poll().helper_alive);
    CHECK(r.client.Submit(1, kBless));
}

TEST(Submit_refused_without_hook) {
    FakeMailbox box;
    box.hooked = false;
    ActionClient client(box);
    CHECK(!client.Poll().helper_alive);
    CHECK(!client.Submit(1, kBless));
    CHECK(!client.Cancel());
    CHECK(!client.Available());
}

// A helper whose heartbeat never runs (title screen, unsupported build) does not count as present
TEST(Helper_never_beating_is_not_available) {
    FakeMailbox box;
    ActionClient client(box);
    for (int i = 0; i < 100; ++i) CHECK(!client.Poll().helper_alive);
    CHECK(!client.Available());
    CHECK(!client.Submit(1, kBless));
}

TEST(Publish_hides_actions_without_helper) {
    FakeMailbox box;
    box.hooked = false;
    ActionClient client(box);
    const ActionStatus st = client.Poll();
    FakePublisher p;
    cotl::PublishActions(p, client.Available(), st, BlessRow());
    CHECK(p.i64.count("act.available") == 0 || p.i64["act.available"] == 0);
    CHECK_EQ(p.CountPrefix("act.name"), size_t{0});
    CHECK_EQ(p.CountPrefix("act.enabled"), size_t{0});
    CHECK_EQ(p.CountPrefix("act.command"), size_t{0});
    CHECK(p.i64.count("act.count") == 0 || p.i64["act.count"] == 0);
}

TEST(Publish_shows_bless_with_helper) {
    Rig r;
    FakePublisher p;
    cotl::PublishActions(p, r.client.Available(), r.client.Poll(), BlessRow());
    CHECK_EQ(p.i64["act.available"], int64_t{1});
    CHECK_EQ(p.i64["act.count"], int64_t{1});
    CHECK_EQ(p.text["act.name0"], std::string("Bless"));
    CHECK_EQ(p.i64["act.enabled0"], int64_t{1});
    CHECK_EQ(p.i64["act.command0"], int64_t{59});
    CHECK_EQ(p.i64["act.state"], int64_t{0});
}

// A paused game (pause menu) leaves the buttons in place so nothing flickers; the tap
// is rejected and the row says why
TEST(Pause_keeps_buttons_but_says_paused) {
    Rig r;
    for (int i = 0; i < 70; ++i) r.client.Poll();
    const ActionStatus st = r.client.Poll();
    CHECK(!st.helper_alive);
    CHECK(r.client.Available());
    FakePublisher p;
    cotl::PublishActions(p, r.client.Available(), st, BlessRow());
    CHECK_EQ(p.i64["act.available"], int64_t{1});
    CHECK_EQ(p.i64["act.count"], int64_t{1});
    CHECK_EQ(p.text["act.message"], std::string(cotl::ActionMessage(st)));
    CHECK(!p.text["act.message"].empty());
}

TEST(Message_for_every_reason) {
    for (uint32_t v = 0; v <= COTL_ACT_REASON_CANT_WORK; ++v) {
        const char* t = cotl::ActionMessage(static_cast<ActionReason>(v));
        REQUIRE(t != nullptr);
        CHECK(t[0] != '\0');
        // short, no dashes
        CHECK(std::string(t).size() <= 40);
        CHECK(std::string(t).find("\xE2\x80\x94") == std::string::npos);
    }
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::LambBusy)), std::string("Lamb is busy"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::AlreadyBlessed)),
             std::string("Already blessed today"));
    // Unknown value from the helper: still some text
    CHECK(cotl::ActionMessage(static_cast<ActionReason>(99))[0] != '\0');
    // Status texts: Refused shows the reason, Walking is not empty
    ActionStatus s{ActionState::Refused, ActionReason::FollowerLocked, true};
    CHECK_EQ(std::string(cotl::ActionMessage(s)),
             std::string(cotl::ActionMessage(ActionReason::FollowerLocked)));
    ActionStatus w{ActionState::Walking, ActionReason::None, true};
    CHECK(cotl::ActionMessage(w)[0] != '\0');
    ActionStatus idle{ActionState::Idle, ActionReason::None, true};
    CHECK_EQ(std::string(cotl::ActionMessage(idle)), std::string());
}

TEST(Status_follows_helper_for_own_seq) {
    Rig r;
    CHECK(r.client.Poll().state == ActionState::Idle);
    REQUIRE(r.client.Submit(5, kBless));
    CHECK(r.client.Poll().state == ActionState::Idle); // not acknowledged yet
    r.box.HelperStep(COTL_ACT_STATE_WALKING, COTL_ACT_REASON_NONE);
    CHECK(r.client.Poll().state == ActionState::Walking);
    r.box[COTL_ACT_ST_STATE] = COTL_ACT_STATE_CANCELLED;
    r.box[COTL_ACT_ST_REASON] = COTL_ACT_REASON_STALE_SESSION;
    const auto st = r.client.Poll();
    CHECK(st.state == ActionState::Cancelled);
    CHECK(st.reason == ActionReason::StaleSession);
}

// Session change with an unacknowledged sequence number: as soon as the helper runs, it acknowledges it (stale
// epoch, rejected); the client does not get stuck
TEST(Epoch_change_with_unacked_seq_recovers) {
    Rig r;
    r.box[COTL_ACT_EPOCH] = 1;
    REQUIRE(r.client.Submit(5, kBless));
    r.box[COTL_ACT_EPOCH] = 2; // new session, helper has not seen the sequence number yet
    CHECK(!r.client.Submit(5, kBless));
    r.box.HelperStep(COTL_ACT_STATE_REFUSED, COTL_ACT_REASON_STALE_SESSION);
    r.client.Poll();
    REQUIRE(r.client.Submit(5, kBless));
    CHECK_EQ(r.box[COTL_ACT_REQ_EPOCH], uint32_t{2});
}

// The host resets the mailbox (all words 0, heartbeat restarts): the sequence number is no longer
// there, so nothing to wait for
TEST(Mailbox_reset_does_not_stick) {
    Rig r;
    REQUIRE(r.client.Submit(5, kBless));
    const uint32_t old = r.box[COTL_ACT_REQ_SEQ];
    for (auto& [off, v] : r.box.words) v = 0;
    r.box.Beat();
    r.client.Poll();
    REQUIRE(r.client.Submit(5, kBless));
    CHECK(r.box[COTL_ACT_REQ_SEQ] != 0);
    CHECK(r.box[COTL_ACT_REQ_SEQ] != old);
}

// New module object over an existing mailbox: the new sequence number must differ from ST_SEQ and
// ST_BUSY_SEQ, otherwise the helper misses it
TEST(New_client_picks_fresh_seq) {
    FakeMailbox box;
    box[COTL_ACT_REQ_SEQ] = 7;
    box[COTL_ACT_ST_SEQ] = 7;
    box[COTL_ACT_ST_BUSY_SEQ] = 8;
    ActionClient client(box);
    for (int i = 0; i < 3; ++i) {
        box.Beat();
        client.Poll();
    }
    REQUIRE(client.Submit(1, kBless));
    const uint32_t s = box[COTL_ACT_REQ_SEQ];
    CHECK(s != 0 && s != 7 && s != 8);
}

TEST(Submit_never_uses_seq_zero_on_wrap) {
    FakeMailbox box;
    box[COTL_ACT_REQ_SEQ] = 0xFFFFFFFFu;
    box[COTL_ACT_ST_SEQ] = 0xFFFFFFFFu;
    ActionClient client(box);
    for (int i = 0; i < 3; ++i) {
        box.Beat();
        client.Poll();
    }
    REQUIRE(client.Submit(1, kBless));
    CHECK(box[COTL_ACT_REQ_SEQ] != 0);
}

TEST(Cancel_writes_cancel_seq) {
    Rig r;
    CHECK(!r.client.Cancel()); // nothing sent
    REQUIRE(r.client.Submit(5, kBless));
    r.box.stores.clear();
    CHECK(r.client.Cancel());
    REQUIRE(r.box.stores.size() == 1);
    CHECK_EQ(r.box.stores[0].first, uint32_t{COTL_ACT_CANCEL_SEQ});
    CHECK_EQ(r.box.stores[0].second, r.box[COTL_ACT_REQ_SEQ]);
}

// The busy rejection shows in the row only briefly; afterwards the helper's state applies again
TEST(Busy_refusal_message_expires) {
    Rig r;
    REQUIRE(r.client.Submit(5, kBless));
    r.box.HelperStep(COTL_ACT_STATE_WALKING, COTL_ACT_REASON_NONE);
    REQUIRE(r.client.Submit(5, kBless));
    r.box.HelperStep(0, 0, true);
    for (int i = 0; i < cotl::kBusyShowPolls; ++i) {
        r.box.Beat();
        const auto st = r.client.Poll();
        CHECK(st.state == ActionState::Refused);
        CHECK(st.reason == ActionReason::LambBusy);
    }
    r.box.Beat();
    auto st = r.client.Poll(); // time is up: the running action
    CHECK(st.state == ActionState::Walking);
    CHECK(st.reason == ActionReason::None);
    r.box[COTL_ACT_ST_STATE] = COTL_ACT_STATE_DONE; // the first action ends
    r.box.Beat();
    CHECK(r.client.Poll().state == ActionState::Done);
    // A new rejection shows briefly again
    REQUIRE(r.client.Submit(5, kBless));
    r.box.HelperStep(0, 0, true);
    CHECK(r.client.Poll().state == ActionState::Refused);
}

// A message belongs to the follower the request went to
TEST(Status_hidden_for_other_follower) {
    Rig r;
    REQUIRE(r.client.Submit(5, kBless));
    r.box.HelperStep(COTL_ACT_STATE_REFUSED, COTL_ACT_REASON_NOT_AT_BASE);
    CHECK(r.client.Poll(5).state == ActionState::Refused);
    const auto other = r.client.Poll(6);
    CHECK(other.state == ActionState::Idle);
    CHECK(other.reason == ActionReason::None);
    CHECK(other.helper_alive);
    CHECK(r.client.Poll(5).state == ActionState::Refused); // back at the first one
    CHECK(r.client.Poll().state == ActionState::Refused);  // without filter
}

// Order as in FollowerCommandGroups.DefaultCommands 0x203FF80 (tools/cotl/README.md)
TEST(test_query_lists_commands) {
    using G = cotl::CommandGroup;
    auto with_cursed = [](int32_t cursed) {
        auto s = PlainFollower();
        s.cursed_state = cursed;
        return s;
    };
    auto with_trait = [](int32_t trait) {
        auto s = PlainFollower();
        s.own_traits = {trait};
        return s;
    };
    CHECK(GroupOf(PlainFollower()) == G::Normal);
    CHECK(GroupOf(with_cursed(0x188)) == G::Baby);
    CHECK(GroupOf(with_cursed(0x42)) == G::Dissenter);
    CHECK(GroupOf(with_cursed(0x44)) == G::MakeDemand);
    CHECK(GroupOf(with_cursed(0x92)) == G::OldAge);
    CHECK(GroupOf(with_trait(0x48)) == G::Zombie);
    CHECK(GroupOf(with_trait(0x4E)) == G::ExistentialDread);
    CHECK(GroupOf(with_trait(0x43)) == G::ExistentialDread);
    CHECK(GroupOf(with_trait(0x25)) == G::Scared);
    CHECK(GroupOf(with_trait(0x28)) == G::Scared);
    CHECK(GroupOf(with_trait(0x3D)) == G::Scared);
    CHECK(GroupOf(with_trait(0x3C)) == G::Poet);
    CHECK(GroupOf(with_trait(0x77)) == G::Poet);
    CHECK(GroupOf(with_trait(0x50)) == G::Spy);
    CHECK(GroupOf(with_trait(0x5A)) == G::Mutated);

    // Cult traits count like own ones; only 0x3D the game checks on the follower alone
    auto cult = PlainFollower();
    cult.cult_traits = {0x50};
    CHECK(GroupOf(cult) == G::Spy);
    cult.cult_traits = {0x3D};
    CHECK(GroupOf(cult) == G::Normal);

    auto snow = PlainFollower();
    snow.is_snowman = true;
    CHECK(GroupOf(snow) == G::Snowman);
    snow.cursed_state = 0x188;
    CHECK(GroupOf(snow) == G::SnowmanChild);

    auto drunk = with_cursed(0x92);
    drunk.drunk = true;
    CHECK(GroupOf(drunk) == G::Drunk); // before OldAge
    auto dissent_zombie = with_trait(0x48);
    dissent_zombie.cursed_state = 0x42;
    CHECK(GroupOf(dissent_zombie) == G::Zombie); // before Dissenter

    // Sleep: bed rest always wakes, sleep only if the follower is not awake
    auto bed = PlainFollower();
    bed.task = cotl::FollowerTaskKind::BedRest;
    CHECK(GroupOf(bed) == G::WakeUp);
    auto sleep = PlainFollower();
    sleep.task = cotl::FollowerTaskKind::Sleep;
    CHECK(GroupOf(sleep) == G::WakeUp);
    CHECK(!OffersBless(sleep));
    sleep.weather = 1; // Blizzard, without trait 0x3B and not in phase 4: awake
    CHECK(GroupOf(sleep) == G::Normal);
    sleep.phase = 4;
    CHECK(GroupOf(sleep) == G::WakeUp);
    sleep.necklace = 0x31;
    CHECK(GroupOf(sleep) == G::Normal);
    sleep.own_traits = {0x5E};
    CHECK(GroupOf(sleep) == G::WakeUp); // Hibernation sleeps even in the blizzard

    // Hibernation and summer rest come before Baby and Zombie
    auto hibernate = with_cursed(0x188);
    hibernate.own_traits = {0x5E};
    hibernate.task = cotl::FollowerTaskKind::Sleep;
    hibernate.season = 1;
    CHECK(GroupOf(hibernate) == G::WakeUp);
    hibernate.season = 0;
    CHECK(GroupOf(hibernate) == G::Baby);
    auto aestivate = with_trait(0x5F);
    aestivate.cursed_state = 0x188;
    aestivate.task = cotl::FollowerTaskKind::Sleep;
    CHECK(GroupOf(aestivate) == G::WakeUp);

    // The game offers Bless only in MakeDemandCommands and for OldAge
    CHECK(OffersBless(PlainFollower()));
    CHECK(OffersBless(with_cursed(0x44)));
    CHECK(OffersBless(with_cursed(0x92)));
    CHECK(OffersBless(with_trait(0x3C)));
    for (const int32_t cursed : {0x188, 0x42}) CHECK(!OffersBless(with_cursed(cursed)));
    for (const int32_t trait : {0x48, 0x4E, 0x25, 0x50, 0x5A}) CHECK(!OffersBless(with_trait(trait)));
    CHECK(OffersBless(drunk)); // Drunk 0x2041AB0: without Dance and Intimidate
    CHECK(!OffersBless(snow));

    // With the doctrines Inspire/Intimidate or without a loyalty bar there is no Bless
    for (const int32_t cursed : {0, 0x44, 0x92}) {
        auto s = with_cursed(cursed);
        s.inspire_unlocked = true;
        CHECK(!OffersBless(s));
        s = with_cursed(cursed);
        s.intimidate_unlocked = true;
        CHECK(!OffersBless(s));
        s = with_cursed(cursed);
        s.show_loyalty_bars = false;
        CHECK(!OffersBless(s));
    }

    // Bless is available exactly without ReceivedBlessing (BlessCommandItem.IsAvailable 0x203EF00)
    auto bless_available = [](const cotl::FollowerCommandState& s) {
        for (const auto& c : cotl::OfferedCommands(s)) {
            if (c.command == kBless) return c.available;
        }
        return false;
    };
    auto blessed = PlainFollower();
    CHECK(bless_available(blessed));
    blessed.received_blessing = true;
    CHECK(OffersBless(blessed));
    CHECK(!bless_available(blessed));
}

TEST(Only_supported_commands_are_shown) {
    // The list contains more than is shown: task 7 unlocks commands one by one
    CHECK(cotl::OfferedCommands(PlainFollower()).size() > 1);
    const auto rows = cotl::VisibleActions(cotl::OfferedCommands(PlainFollower()));
    REQUIRE(rows.size() == 4); // Bless first: its button keeps its position; then Work, Sleep, Eat
    CHECK_EQ(rows[1].command, uint32_t{1});
    CHECK_EQ(rows[2].command, uint32_t{26});
    CHECK_EQ(rows[3].command, uint32_t{25});
    CHECK_EQ(rows[0].command, kBless);
    CHECK_EQ(std::string(rows[0].name), std::string("Bless"));
    CHECK(rows[0].enabled());

    // Unknown and not yet unlocked commands stay hidden, Murder and the rest too
    std::vector<uint32_t> supported = {kBless, 8, 9, 22, 23, 26, 27, 43, 44, 75, 85, 86,
                                       // Eat, Drink, Work and their submenus (food and work)
                                       1, 25, 89, 14, 17, 28, 175, 47, 30, 58, 113, 117, 124, 126, 116, 64, 81, 88,
                                       31, 32, 33, 37, 35, 34, 36, 133, 134, 135, 38, 60, 87, 61, 65, 66, 71, 72, 70,
                                       67, 68, 69, 105, 112, 84, 90, 92, 96, 91, 94, 95, 93, 127, 128, 129, 130, 177,
                                       // Gifts: Gift, Give Pet, their submenus, the necklace
                                       11, 148, 62, 51, 52, 53, 54, 55, 56, 57, 77, 82, 79, 76, 80, 78, 119,
                                       120, 121, 122, 123, 141, 142, 143, 144, 145, 146, 147};
    for (uint32_t command = 0; command < 256; ++command) {
        const bool on = std::find(supported.begin(), supported.end(), command) != supported.end();
        CHECK(cotl::SupportOf(command) == (on ? cotl::ActionSupport::Supported : cotl::ActionSupport::Hidden));
    }
    CHECK(cotl::VisibleActions({{5, true}, {7, true}, {46, true}}).empty());

    auto zombie = PlainFollower();
    zombie.own_traits = {0x48};
    FakePublisher p;
    ActionStatus alive;
    alive.helper_alive = true;
    cotl::PublishActions(p, true, alive, cotl::VisibleActions(cotl::OfferedCommands(zombie)));
    CHECK_EQ(p.i64["act.available"], int64_t{1});
    CHECK_EQ(p.i64["act.count"], int64_t{0});
    CHECK_EQ(p.CountPrefix("act.name"), size_t{0});
}

TEST(Locked_commands_are_disabled) {
    auto blessed = PlainFollower();
    blessed.received_blessing = true;
    const auto both = cotl::VisibleActions(cotl::OfferedCommands(blessed));
    REQUIRE(both.size() == 4);
    const std::vector<cotl::ActionRow> rows = {both[0]};
    CHECK(!rows[0].enabled());
    CHECK(rows[0].locked == ActionReason::AlreadyBlessed);

    ActionStatus alive;
    alive.helper_alive = true;
    FakePublisher p;
    cotl::PublishActions(p, true, alive, rows);
    CHECK_EQ(p.i64["act.count"], int64_t{1});
    CHECK_EQ(p.text["act.name0"], std::string("Bless"));
    CHECK_EQ(p.i64["act.enabled0"], int64_t{0});
    CHECK_EQ(p.i64["act.locked0"], int64_t{1});
    // Same message as the helper's rejection; shown as long as there is nothing else to say
    CHECK_EQ(p.text["act.message"], std::string("Already blessed today"));

    // The helper's state takes precedence
    ActionStatus refused = alive;
    refused.state = ActionState::Refused;
    refused.reason = ActionReason::LambBusy;
    FakePublisher q;
    cotl::PublishActions(q, true, refused, rows);
    CHECK_EQ(q.text["act.message"], std::string("Lamb is busy"));
    ActionStatus paused;
    FakePublisher r;
    cotl::PublishActions(r, true, paused, rows);
    CHECK_EQ(r.text["act.message"], std::string("Game is paused"));

    // Free button: enabled, not locked
    FakePublisher free_row;
    cotl::PublishActions(free_row, true, alive, BlessRow());
    CHECK_EQ(free_row.i64["act.enabled0"], int64_t{1});
    CHECK_EQ(free_row.i64["act.locked0"], int64_t{0});
    CHECK_EQ(free_row.text["act.message"], std::string(""));
}

// NormalCommands 0x2041248 and PoetCommands 0x20436B0 skip Make Demand with its submenu when
// DM.InTutorial == 0; the direct path (Cursed 0x44) and OldAge do not ask for it
TEST(Normal_and_Poet_offer_demands_only_with_the_tutorial_flag) {
    constexpr uint32_t kMakeDemand = 5, kSurveillance = 46;
    auto offers = [](const cotl::FollowerCommandState& s, uint32_t command) {
        for (const auto& c : cotl::OfferedCommands(s)) {
            if (c.command == command) return true;
        }
        return false;
    };
    auto normal = PlainFollower();
    auto poet = PlainFollower();
    poet.own_traits = {0x3C};
    for (auto* s : {&normal, &poet}) {
        s->in_tutorial = true;
        CHECK(offers(*s, kMakeDemand));
        CHECK(offers(*s, kBless));
        s->in_tutorial = false;
        CHECK(!offers(*s, kMakeDemand));
        CHECK(!offers(*s, kBless));
        CHECK(offers(*s, kSurveillance)); // the rest of the group stays
        // Work is in NormalCommands outside Make Demand, for Poet it is missing
        const auto rows = cotl::VisibleActions(cotl::OfferedCommands(*s));
        REQUIRE(rows.size() == (s == &normal ? 1u : 0u));
        if (s == &normal) CHECK_EQ(rows[0].command, uint32_t{1});
    }
    CHECK(GroupOf(poet) == cotl::CommandGroup::Poet);

    // Sleeping but awake follower: also NormalCommands
    auto awake = PlainFollower();
    awake.task = cotl::FollowerTaskKind::Sleep;
    awake.weather = 1;
    awake.in_tutorial = false;
    CHECK(GroupOf(awake) == cotl::CommandGroup::Normal);
    CHECK(!offers(awake, kBless));

    for (const int32_t cursed : {0x44, 0x92}) {
        auto s = PlainFollower();
        s.cursed_state = cursed;
        s.in_tutorial = false;
        CHECK(offers(s, kBless));
        const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
        REQUIRE(!rows.empty());
        CHECK_EQ(rows[0].command, kBless);
    }

    CHECK(cotl::FollowerCommandState{}.in_tutorial); // default: Bless stays
}

// --- Task 7, simple commands (derivation in tools/cotl/README.md)
namespace {
constexpr uint32_t kExtort = 8, kDance = 9, kRomance = 22, kWakeUp = 23, kSleep = 26, kWorship = 27, kIntimidate = 43,
                   kBribe = 44, kPetDog = 75, kBully = 85, kReassure = 86;

// What is shown, as a set in wheel order, Bless first. The simple commands date from before the
// submenus: Work appears here as Worship if its submenu contains it, otherwise it is missing, likewise Eat and
// Drink. The full rows are checked by the "food and work" tests (Rows, SubRows).
std::vector<uint32_t> Shown(const cotl::FollowerCommandState& s) {
    std::vector<uint32_t> out;
    const auto offered = cotl::OfferedCommands(s);
    for (const auto& r : cotl::VisibleActions(offered)) {
        if (r.command == 25 || r.command == 89) continue;
        if (r.command == 1) {
            for (const auto& c : offered) {
                if (c.command != 1) continue;
                for (const auto& sub : c.sub) {
                    if (sub.command == 27) out.push_back(27);
                }
            }
            continue;
        }
        out.push_back(r.command);
    }
    return out;
}

bool Shows(const cotl::FollowerCommandState& s, uint32_t command) {
    const auto v = Shown(s);
    return std::find(v.begin(), v.end(), command) != v.end();
}

// Lock of a row, also in the submenus
ActionReason LockOf(const cotl::FollowerCommandState& s, uint32_t command) {
    const auto offered = cotl::OfferedCommands(s);
    for (const auto& r : cotl::VisibleActions(offered)) {
        if (r.command == command) return r.locked;
    }
    for (const auto& c : offered) {
        for (int page = 0; !c.sub.empty() && page < 3; ++page) {
            for (const auto& r : cotl::MenuActions(offered, {c.command, page})) {
                if (r.command == command) return r.locked;
            }
        }
    }
    CHECK(false); // not shown
    return ActionReason::None;
}

cotl::FollowerCommandState Everything() {
    auto s = PlainFollower();
    s.bribe_unlocked = s.inspire_unlocked = s.intimidate_unlocked = true;
    s.married = s.skin_dog = true;
    s.shrine_built = true;
    s.gold = 10;
    return s;
}
} // namespace

TEST(Simple_commands_are_supported_and_BeNice_is_not) {
    for (const uint32_t c : {kExtort, kDance, kRomance, kWakeUp, kSleep, kWorship, kIntimidate, kBribe, kPetDog,
                             kBully, kReassure, kBless}) {
        CHECK(cotl::SupportOf(c) == cotl::ActionSupport::Supported);
    }
    CHECK(cotl::SupportOf(21) == cotl::ActionSupport::Hidden); // BeNice: no entry in the wheel, handler default
}

TEST(Names_fit_the_panel) {
    const std::map<uint32_t, std::string> names = {
        {kDance, "Dance"},     {kIntimidate, "Intimidate"}, {kBribe, "Bribe"},     {kReassure, "Reassure"},
        {kBully, "Bully"},     {kRomance, "Romance"},       {kPetDog, "Pet Dog"},      {kWakeUp, "Wake Up"},
        {kSleep, "Sleep"},     {kWorship, "Worship"},       {kBless, "Bless"},         {kExtort, "Extort"}};
    for (const auto& [command, name] : names) {
        const auto rows = cotl::VisibleActions({{command, true}});
        REQUIRE(rows.size() == 1);
        CHECK_EQ(std::string(rows[0].name), name);
    }
}

TEST(Normal_group_lists_the_wheel_flat_with_bless_in_the_first_slot) {
    auto s = PlainFollower();
    s.shrine_built = true;
    s.married = true;
    s.skin_dog = true;
    s.gold = 5;
    // GiveWorker: Worship; MakeDemand: Sleep, Bless, Romance, Pet
    CHECK(Shown(s) == (std::vector<uint32_t>{kBless, kWorship, kSleep, kRomance, kPetDog}));

    s = Everything();
    // With doctrines no Bless; Bully/Reassure only with traits 0x24 or 0x3F
    CHECK(Shown(s) == (std::vector<uint32_t>{kWorship, kSleep, kBribe, kDance, kIntimidate, kRomance, kPetDog}));
    for (const int32_t trait : {0x24, 0x3F}) {
        auto t = Everything();
        t.own_traits = {trait};
        CHECK(Shown(t) == (std::vector<uint32_t>{kWorship, kSleep, kBribe, kDance, kIntimidate, kRomance, kPetDog,
                                                 kBully, kReassure}));
        t.own_traits = {};
        t.cult_traits = {trait};
        CHECK(Shows(t, kBully));
    }

    // Without DM.InTutorial the whole submenu is dropped, Worship and Bully stay
    s = Everything();
    s.own_traits = {0x24};
    s.in_tutorial = false;
    CHECK(Shown(s) == (std::vector<uint32_t>{kWorship, kBully, kReassure}));
}

TEST(Sleep_is_not_offered_to_a_follower_already_in_a_sleep_task) {
    auto s = PlainFollower();
    CHECK(Shows(s, kSleep));
    s.in_sleep_task = true; // MakeDemandCommands asks CurrentTaskType 7/0x31, also on the way to bed
    CHECK(!Shows(s, kSleep));
    CHECK(Shows(s, kBless));
}

TEST(Make_demand_group_has_no_sleep_and_poet_no_worship) {
    auto s = Everything();
    s.cursed_state = 0x44; // bed rest instead of Sleep (command 6, unsupported)
    CHECK(Shown(s) == (std::vector<uint32_t>{kBribe, kDance, kIntimidate, kRomance, kPetDog}));

    s = Everything();
    s.own_traits = {0x3C}; // Poet: submenu, no Worship, no Bully
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep, kBribe, kDance, kIntimidate, kRomance, kPetDog}));
    s.in_tutorial = false;
    CHECK(Shown(s).empty());
}

// Follower.TimedAnimation (0x2993D30): while it runs, the helper rejects every command
TEST(Timed_action_locks_what_is_still_free) {
    auto s = Everything();
    s.received_blessing = true;
    s.inspire_unlocked = s.intimidate_unlocked = false;
    const auto free_rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    s.timed_action = true;
    const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    REQUIRE(rows.size() == free_rows.size());
    REQUIRE(!rows.empty());
    for (size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].command == free_rows[i].command);
        // The daily reason stays: the helper names it first, too
        CHECK(rows[i].locked == (free_rows[i].enabled() ? cotl::ActionReason::FollowerLocked : free_rows[i].locked));
    }
}

TEST(Other_groups_list_what_the_wheel_has) {
    auto s = Everything();
    s.cursed_state = 0x92; // OldAge 0x2042580: doctrine 6 -> Dance, else 7 -> Intimidate, else Bless
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep, kDance, kRomance, kPetDog}));
    {
        auto t = s;
        t.inspire_unlocked = false;
        CHECK(Shown(t) == (std::vector<uint32_t>{kSleep, kIntimidate, kRomance, kPetDog}));
        t.intimidate_unlocked = false;
        t.show_loyalty_bars = false;
        CHECK(Shown(t) == (std::vector<uint32_t>{kSleep, kRomance, kPetDog}));
    }
    for (const int32_t trait : {0x25, 0x28, 0x3D, 0x24, 0x43}) {
        auto t = s;
        t.own_traits = {trait};
        if (cotl::SelectCommandGroup(t) == cotl::CommandGroup::OldAge) {
            CHECK(Shows(t, kBully));
        }
    }
    s.inspire_unlocked = s.intimidate_unlocked = false;
    CHECK(Shown(s) == (std::vector<uint32_t>{kBless, kSleep, kRomance, kPetDog}));

    // Drunk 0x2041AB0: Dance, else Intimidate, else Bless, only in the else branch of the trait check
    // (0x2041D70 after 0x20422A4), before Surveillance (0x2042334 after 0x2041EB0)
    s = Everything();
    s.drunk = true;
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep, kDance, kRomance, kPetDog}));
    s.inspire_unlocked = false;
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep, kIntimidate, kRomance, kPetDog}));
    s.intimidate_unlocked = false;
    CHECK(Shown(s) == (std::vector<uint32_t>{kBless, kSleep, kRomance, kPetDog}));
    s.own_traits = {0x24};
    CHECK(Shows(s, kBully) && Shows(s, kReassure));
    CHECK(!Shows(s, kDance) && !Shows(s, kIntimidate) && !Shows(s, kBless));
    {
        auto t = s; // the doctrine changes nothing here: a tap would otherwise cost the daily flag
        t.inspire_unlocked = true;
        CHECK(Shown(t) == (std::vector<uint32_t>{kSleep, kBully, kReassure, kRomance, kPetDog}));
    }
    s.own_traits = {};
    s.show_loyalty_bars = false;
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep, kRomance, kPetDog}));

    s = Everything();
    s.own_traits = {0x25}; // Scared: Bully and Reassure always
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep, kBully, kReassure}));
    s = Everything();
    s.own_traits = {0x50}; // Spy
    CHECK(Shown(s) == (std::vector<uint32_t>{kSleep}));
    s = Everything();
    s.own_traits = {0x48}; // Zombie: Romance, if married
    CHECK(Shown(s) == (std::vector<uint32_t>{kRomance}));
    s.married = false;
    CHECK(Shown(s).empty());
    s = Everything();
    s.cult_traits = {0x5A}; // Mutated, the trait applies to the whole cult: Romance, then Worship
    CHECK(Shown(s) == (std::vector<uint32_t>{kRomance, kWorship}));
    s.cult_traits = {};
    s.own_traits = {0x5A}; // own mutation: GiveWorkerCommands 0x203CDC0 leaves out Worship
    CHECK(Shown(s) == (std::vector<uint32_t>{kRomance}));

    // Baby, snowman child, dissenter, existential dread: none of them
    for (const int32_t cursed : {0x188, 0x42}) {
        s = Everything();
        s.cursed_state = cursed;
        CHECK(Shown(s).empty());
    }
    s = Everything();
    s.is_snowman = true;
    s.cursed_state = 0x188;
    CHECK(Shown(s).empty());
    s = Everything();
    s.own_traits = {0x4E};
    CHECK(Shown(s).empty());

    // Snowman: only Worship
    s = Everything();
    s.is_snowman = true;
    CHECK(Shown(s) == (std::vector<uint32_t>{kWorship}));

    // WakeUp group
    s = Everything();
    s.task = cotl::FollowerTaskKind::BedRest;
    s.in_sleep_task = true;
    CHECK(Shown(s) == (std::vector<uint32_t>{kWakeUp}));
}

TEST(Worship_follows_the_shrine_and_the_onboarding) {
    auto s = PlainFollower();
    CHECK(!Shows(s, kWorship)); // no shrine
    s.shrine_built = true;
    CHECK(Shows(s, kWorship));
    for (const int32_t phase : {1, 4}) {
        s.onboarding_phase = phase;
        CHECK(!Shows(s, kWorship));
    }
    s.shrine_built = false;
    s.onboarding_phase = 3;
    CHECK(Shows(s, kWorship));
    s.onboarding_phase = 0;

    // GiveWorkerCommand 0x203CC90: Thought 0 and not trait 0x67
    s = PlainFollower();
    s.shrine_built = true;
    s.own_traits = {0x67};
    CHECK(!Shows(s, kWorship));
    s = PlainFollower();
    s.shrine_built = true;
    s.cursed_state = 5;
    CHECK(!Shows(s, kWorship));

    // Sozo before the liberation
    s = PlainFollower();
    s.shrine_built = true;
    s.follower_id = 99996;
    CHECK(!Shows(s, kWorship));
    s.sozo_freed = true;
    CHECK(Shows(s, kWorship));
    // Only in NormalCommands: with another follower the flag has no effect
    s.follower_id = 7;
    s.sozo_freed = false;
    CHECK(Shows(s, kWorship));
}

TEST(Romance_and_pet_need_spouse_and_dog) {
    auto s = PlainFollower();
    CHECK(!Shows(s, kRomance));
    CHECK(!Shows(s, kPetDog));
    s.married = true;
    CHECK(Shows(s, kRomance));
    CHECK(!Shows(s, kPetDog));
    s.skin_dog = true;
    CHECK(Shows(s, kPetDog));
}

TEST(Bribe_needs_the_doctrine) {
    auto s = PlainFollower();
    s.gold = 5;
    CHECK(!Shows(s, kBribe));
    s.bribe_unlocked = true;
    CHECK(Shows(s, kBribe));
}

// MakeDemandCommands 0x203B6C0: Sleep/bed rest, Murder, Extort (doctrine 14, 0x203BEA4), Bribe
TEST(Extort_needs_the_doctrine_and_sits_between_sleep_and_bribe) {
    auto s = Everything();
    CHECK(!Shows(s, kExtort));
    s.extort_unlocked = true;
    CHECK(Shown(s) == (std::vector<uint32_t>{kWorship, kSleep, kExtort, kBribe, kDance, kIntimidate, kRomance,
                                             kPetDog}));

    s.in_sleep_task = true; // without Sleep it moves to the front
    CHECK(Shown(s) == (std::vector<uint32_t>{kWorship, kExtort, kBribe, kDance, kIntimidate, kRomance, kPetDog}));
}

TEST(Extort_is_offered_only_where_make_demand_is) {
    auto s = Everything();
    s.extort_unlocked = true;
    s.cursed_state = 0x44; // Make Demand group
    CHECK(Shows(s, kExtort));

    s = Everything();
    s.extort_unlocked = true;
    s.own_traits = {0x3C}; // Poet: the same submenu
    CHECK(Shows(s, kExtort));
    s.in_tutorial = false;
    CHECK(!Shows(s, kExtort));

    s = Everything();
    s.extort_unlocked = true;
    s.in_tutorial = false; // Normal without submenu
    CHECK(!Shows(s, kExtort));

    s = Everything();
    s.extort_unlocked = true;
    s.cursed_state = 0x92; // OldAge does not know it
    CHECK(!Shows(s, kExtort));
}

TEST(Extort_locks_on_the_daily_flag) {
    auto s = Everything();
    s.extort_unlocked = true;
    CHECK(LockOf(s, kExtort) == ActionReason::None);
    s.paid_tithes = true;
    CHECK(LockOf(s, kExtort) == ActionReason::AlreadyExtorted);
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::AlreadyExtorted)), std::string("Already extorted today"));
    s.paid_tithes = false;
    s.timed_action = true;
    CHECK(LockOf(s, kExtort) == ActionReason::FollowerLocked);
}

TEST(Longest_list_with_extort_fits_the_panel) {
    auto s = Everything();
    s.extort_unlocked = true;
    s.own_traits = {0x24};
    s.structures = {343}; // pub: Drink added
    const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    // Work (instead of Worship), Sleep, Extort, Bribe, Dance, Intimidate, Romance, Pet, Eat, Drink, Bully, Reassure
    CHECK_EQ(rows.size(), size_t{12});
    CHECK(rows.size() <= static_cast<size_t>(cotl::kActionSlots));
}

TEST(Locks_match_the_helper_refusals) {
    auto s = Everything();
    s.own_traits = {0x24};
    CHECK(LockOf(s, kDance) == ActionReason::None);
    CHECK(LockOf(s, kIntimidate) == ActionReason::None);
    CHECK(LockOf(s, kBribe) == ActionReason::None);
    CHECK(LockOf(s, kRomance) == ActionReason::None);
    CHECK(LockOf(s, kPetDog) == ActionReason::None);
    CHECK(LockOf(s, kBully) == ActionReason::None);
    CHECK(LockOf(s, kReassure) == ActionReason::None);
    CHECK(LockOf(s, kWorship) == ActionReason::None);
    CHECK(LockOf(s, kSleep) == ActionReason::None);

    s.inspired = true;
    CHECK(LockOf(s, kDance) == ActionReason::AlreadyInspired);
    s.intimidated = true;
    CHECK(LockOf(s, kIntimidate) == ActionReason::AlreadyIntimidated);
    s.kissed = true;
    CHECK(LockOf(s, kRomance) == ActionReason::AlreadyKissed);
    s.petted = true;
    CHECK(LockOf(s, kPetDog) == ActionReason::AlreadyPetted);
    s.scared_interacted = true;
    CHECK(LockOf(s, kBully) == ActionReason::AlreadyInteracted);
    CHECK(LockOf(s, kReassure) == ActionReason::AlreadyInteracted);
    // The game does not lock in the wheel after little gold, but rejects after the walk
    s.gold = 2;
    CHECK(LockOf(s, kBribe) == ActionReason::NotEnoughGold);
    s.gold = 3;
    CHECK(LockOf(s, kBribe) == ActionReason::None);
    s.gold = 0;
    s.bribed = true;
    CHECK(LockOf(s, kBribe) == ActionReason::AlreadyBribed);
    // Sleep knows no rejection
    CHECK(LockOf(s, kSleep) == ActionReason::None);
}

TEST(Worship_and_wake_up_lock_in_the_cold) {
    auto s = PlainFollower();
    s.shrine_built = true;
    s.weather = 1;
    CHECK(LockOf(s, kWorship) == ActionReason::TooCold);
    auto warm = s;
    warm.cult_traits = {0x79};
    CHECK(LockOf(warm, kWorship) == ActionReason::None);
    auto cult = s;
    cult.cult_traits = {0x57};
    CHECK(LockOf(cult, kWorship) == ActionReason::None);
    auto own = s;
    own.own_traits = {0x57}; // counts only as a cult trait
    CHECK(LockOf(own, kWorship) == ActionReason::TooCold);
    auto snow = s;
    snow.is_snowman = true;
    CHECK(LockOf(snow, kWorship) == ActionReason::None);
    auto mutated = s;
    mutated.cult_traits = {0x5A};
    CHECK(LockOf(mutated, kWorship) == ActionReason::None);

    // Awake in the blizzard would be Normal: bed rest stays in the WakeUp group
    auto wake = PlainFollower();
    wake.task = cotl::FollowerTaskKind::BedRest;
    CHECK(LockOf(wake, kWakeUp) == ActionReason::None);
    wake.weather = 1;
    CHECK(LockOf(wake, kWakeUp) == ActionReason::TooCold);
}

TEST(Wake_up_locks_while_hibernating) {
    auto s = PlainFollower();
    s.task = cotl::FollowerTaskKind::Sleep;
    s.own_traits = {0x5E};
    s.season = 1;
    CHECK(LockOf(s, kWakeUp) == ActionReason::SleepsUntilSeason);
    s.work_through_night = true;
    CHECK(LockOf(s, kWakeUp) == ActionReason::None);

    s = PlainFollower();
    s.task = cotl::FollowerTaskKind::Sleep;
    s.own_traits = {0x5F};
    s.season = 0;
    CHECK(LockOf(s, kWakeUp) == ActionReason::SleepsUntilSeason);
}

TEST(Publishes_a_slot_for_every_button) {
    ActionStatus alive;
    alive.helper_alive = true;
    FakePublisher p;
    cotl::PublishActions(p, true, alive, BlessRow());
    CHECK_EQ(p.i64["act.count"], int64_t{1});
    // The fixed slots in the window show only what applies now: the empty ones are 0 too
    CHECK_EQ(cotl::kActionSlots, 12);
    for (int i = 1; i < cotl::kActionSlots; ++i) {
        const auto n = std::to_string(i);
        REQUIRE(p.i64.count("act.enabled" + n) == 1);
        CHECK_EQ(p.i64["act.enabled" + n], int64_t{0});
        CHECK_EQ(p.i64["act.locked" + n], int64_t{0});
    }
    CHECK_EQ(p.CountPrefix("act.name"), size_t{1});
    // A row beyond the slots has no button and cannot be tapped
    std::vector<cotl::ActionRow> many(14, BlessRow()[0]);
    FakePublisher q;
    cotl::PublishActions(q, true, alive, many);
    CHECK_EQ(q.i64["act.count"], int64_t{12});
    CHECK(q.i64.count("act.enabled12") == 0);
}

TEST(Message_names_locked_commands_when_several_are_shown) {
    ActionStatus alive;
    alive.helper_alive = true;
    const auto publish = [&](const std::vector<cotl::ActionRow>& rows) {
        FakePublisher p;
        cotl::PublishActions(p, true, alive, rows);
        return p.text["act.message"];
    };
    const cotl::ActionRow dance{"Dance", kDance, ActionReason::AlreadyInspired};
    const cotl::ActionRow sleep{"Sleep", kSleep, ActionReason::None};
    const cotl::ActionRow bribe{"Bribe", kBribe, ActionReason::NotEnoughGold};
    CHECK_EQ(publish({dance}), std::string("Already inspired today"));
    CHECK_EQ(publish({sleep}), std::string(""));
    CHECK_EQ(publish({sleep, dance}), std::string("Dance: Already inspired today"));
    CHECK_EQ(publish({dance, sleep, bribe}),
             std::string("Dance: Already inspired today, Bribe: Not enough gold"));
    // No longer fits in the row: stop, never a truncated part
    const cotl::ActionRow bully{"Bully", kBully, ActionReason::AlreadyInteracted};
    const auto full = publish({dance, bribe, bully});
    CHECK_EQ(full, std::string("Dance: Already inspired today, Bribe: Not enough gold"));
    // The helper's state still takes precedence
    ActionStatus refused = alive;
    refused.state = ActionState::Refused;
    refused.reason = ActionReason::LambBusy;
    FakePublisher p;
    cotl::PublishActions(p, true, refused, {dance, bribe});
    CHECK_EQ(p.text["act.message"], std::string("Lamb is busy"));
}

// --- Task 7, food and work: submenus Eat 25, Drink 89, Work 1 (tools/cotl/README.md, food and work)
namespace {
constexpr uint32_t kWork = 1, kEat = 25, kDrink = 89, kChop = 14, kRubble = 17, kBuild = 28, kFarmer = 47,
                   kCook = 30, kJanitor = 58, kMedic = 113, kLogistics = 117, kHandyman = 124, kTraits = 126,
                   kRancher = 116, kRefiner = 64, kUndertaker = 81, kBrew = 88, kRotstone = 175;

std::vector<uint32_t> Rows(const cotl::FollowerCommandState& s) {
    std::vector<uint32_t> out;
    for (const auto& r : cotl::VisibleActions(cotl::OfferedCommands(s))) out.push_back(r.command);
    return out;
}

std::vector<cotl::ActionRow> SubRowsOf(const cotl::FollowerCommandState& s, uint32_t parent, int page = 0) {
    return cotl::MenuActions(cotl::OfferedCommands(s), {parent, page});
}

std::vector<uint32_t> SubRows(const cotl::FollowerCommandState& s, uint32_t parent, int page = 0) {
    std::vector<uint32_t> out;
    for (const auto& r : SubRowsOf(s, parent, page)) out.push_back(r.command);
    return out;
}

const cotl::ActionRow* RowOf(const std::vector<cotl::ActionRow>& rows, uint32_t command) {
    for (const auto& r : rows) {
        if (r.command == command) return &r;
    }
    return nullptr;
}

constexpr uint32_t B = cotl::kMenuBack, M = cotl::kMenuMore;
} // namespace

TEST(Eat_and_drink_stand_where_the_wheel_has_them) {
    auto s = Everything();
    s.cursed_state = 0x42; // Dissenter 0x20419F4: after Surveillance
    CHECK(Rows(s) == (std::vector<uint32_t>{kEat}));
    s = Everything();
    s.own_traits = {0x50}; // Spy 0x2043D48
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kEat}));

    // Drink only with a pub and without Structures_Pub.IsDrinking (0x203C3B4)
    s = Everything();
    s.drunk = true; // Drunk 0x2041F70: after Surveillance, before Romance
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kEat, kRomance, kPetDog}));
    s.structures = {343};
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kEat, kDrink, kRomance, kPetDog}));
    s.pub_drinking = true;
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kEat, kRomance, kPetDog}));
    s.pub_drinking = false;
    s.structures = {350};
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kEat, kDrink, kRomance, kPetDog}));

    s = Everything();
    s.cursed_state = 0x92; // OldAge 0x2042C1C: after Bully/Reassure
    s.structures = {343};
    s.own_traits = {0x24};
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kBully, kReassure, kEat, kDrink, kRomance, kPetDog}));

    s = Everything();
    s.cursed_state = 0x44; // MakeDemand 0x203C2CC: after Bless, before Romance
    s.structures = {343};
    s.inspire_unlocked = s.intimidate_unlocked = false;
    CHECK(Rows(s) == (std::vector<uint32_t>{kBless, kBribe, kEat, kDrink, kRomance, kPetDog}));
    // Normal: the Make Demand submenu sits flat after Work, with Eat and Drink inside
    s = Everything();
    s.structures = {343};
    CHECK(Rows(s) == (std::vector<uint32_t>{kWork, kSleep, kBribe, kDance, kIntimidate, kEat, kDrink, kRomance,
                                            kPetDog}));
}

TEST(Eat_lists_the_meals_in_the_wheel_order_behind_back) {
    auto s = PlainFollower();
    s.cursed_state = 0x42;
    // Without a meal: no submenu, Eat greyed (the handler would say "NoMeals", 0x2C5F394)
    auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    REQUIRE(rows.size() == 1);
    CHECK_EQ(std::string(rows[0].name), std::string("Eat"));
    CHECK(rows[0].locked == ActionReason::NoMeals);
    CHECK(!rows[0].opens);
    // MealCommands 0x20356E0: fixed order by ITEM_TYPE, regardless of how many and from where
    s.meals = {0x73, 0x20, 0x39, 0xC8, 0x6A, 0x20, 0x6E};
    rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    CHECK(rows[0].locked == ActionReason::None);
    CHECK(rows[0].opens);
    CHECK(SubRows(s, kEat) == (std::vector<uint32_t>{B, 31, 32, 135, 65, 69, 84}));
    const auto sub = SubRowsOf(s, kEat);
    CHECK_EQ(std::string(sub[0].name), std::string("Back"));
    CHECK_EQ(sub[0].parent, uint32_t{0});
    for (size_t i = 1; i < sub.size(); ++i) {
        CHECK_EQ(sub[i].parent, kEat);
        CHECK(sub[i].enabled()); // FoodCommandItem has no IsAvailable
    }
    // Spoiled food (MEAL_ROTTEN 0x40) is not in the wheel
    s.meals = {0x40};
    CHECK(cotl::VisibleActions(cotl::OfferedCommands(s))[0].locked == ActionReason::NoMeals);
    // Fasting: no submenu (EatSomething 0x2035100), the handler would say "CantEatFasting"
    s.meals = {0x20};
    s.fasting = true;
    rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    CHECK(rows[0].locked == ActionReason::Fasting);
    CHECK(!rows[0].opens);
    // A submenu that does not exist shows the top level
    CHECK(SubRows(s, kEat) == Rows(s));
}

TEST(Every_meal_and_drink_has_a_name_and_its_item) {
    const std::vector<std::pair<int32_t, uint32_t>> meals = {
        {0x20, 31},  {0x39, 32},  {0x45, 33},  {0x50, 37}, {0x41, 35}, {0x3C, 34}, {0x3B, 36},
        {0xC6, 133}, {0xC7, 134}, {0xC8, 135}, {0x3A, 38}, {0x63, 60}, {0xA1, 87}, {0x64, 61},
        {0x6A, 65},  {0x6B, 66},  {0x70, 71},  {0x71, 72}, {0x6F, 70}, {0x6C, 67}, {0x6D, 68},
        {0x6E, 69},  {0x86, 105}, {0xAB, 112}, {0x73, 84}};
    auto s = PlainFollower();
    s.cursed_state = 0x42;
    std::vector<uint32_t> want;
    for (const auto& [item, command] : meals) {
        s.meals.push_back(item);
        want.push_back(command);
        CHECK(cotl::SupportOf(command) == cotl::ActionSupport::Supported);
    }
    std::vector<uint32_t> all;
    for (int page = 0; page < 3; ++page) {
        for (const uint32_t c : SubRows(s, kEat, page)) {
            if (c != B && c != M) all.push_back(c);
        }
    }
    CHECK(all == want);

    const std::vector<std::pair<int32_t, uint32_t>> drinks = {
        {0x91, 90}, {0x93, 92}, {0x94, 96}, {0x9C, 91}, {0x9E, 94}, {0x95, 95},
        {0x92, 93}, {0xBE, 127}, {0xBF, 128}, {0xC0, 129}, {0xC1, 130}, {0xEC, 177}};
    s = Everything();
    s.drunk = true;
    s.structures = {343};
    std::vector<uint32_t> wd;
    for (const auto& [item, command] : drinks) {
        s.drinks.push_back(item);
        wd.push_back(command);
        CHECK(cotl::SupportOf(command) == cotl::ActionSupport::Supported);
    }
    std::vector<uint32_t> got;
    for (int page = 0; page < 2; ++page) {
        for (const uint32_t c : SubRows(s, kDrink, page)) {
            if (c != B && c != M) got.push_back(c);
        }
    }
    CHECK(got == wd);
    for (const uint32_t c : {kEat, kDrink, kWork}) CHECK(cotl::SupportOf(c) == cotl::ActionSupport::Supported);
}

TEST(Drink_locks_without_drinks) {
    auto s = Everything();
    s.drunk = true;
    s.structures = {343};
    const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    const auto* drink = RowOf(rows, kDrink);
    REQUIRE(drink != nullptr);
    CHECK(drink->locked == ActionReason::NoDrinks); // the handler would say "NoDrinks" (0x2C5E2B8)
    CHECK(!drink->opens);
    s.drinks = {0x91};
    CHECK(SubRows(s, kDrink) == (std::vector<uint32_t>{B, 90}));
}

TEST(Long_sub_menus_page_with_more) {
    auto s = PlainFollower();
    s.shrine_built = true;
    // All buildings: Worship, 14, 17, Build, then the roles per GiveWorkerCommands 0x203CDC0
    s.structures = {31, 498, 8, 265, 216, 396, 407, 416, 420, 397, 148, 304, 343};
    const std::vector<uint32_t> all = {kWorship, kChop,      kRubble,   kBuild,     kRotstone, kFarmer,
                                       kCook,    kJanitor,   kMedic,    kLogistics, kHandyman, kTraits,
                                       kRancher, kRefiner,   kUndertaker, kBrew};
    // 16 entries and Back do not fit in 12 slots: 10 per page, More pages on, finally back to the front
    const auto p0 = SubRows(s, kWork, 0);
    const auto p1 = SubRows(s, kWork, 1);
    CHECK_EQ(p0.size(), size_t{12});
    CHECK_EQ(p0.front(), B);
    CHECK_EQ(p0.back(), M);
    CHECK(std::vector<uint32_t>(p0.begin() + 1, p0.end() - 1) ==
          std::vector<uint32_t>(all.begin(), all.begin() + 10));
    CHECK(p1 == (std::vector<uint32_t>{B, kHandyman, kTraits, kRancher, kRefiner, kUndertaker, kBrew, M}));
    CHECK(SubRows(s, kWork, 2) == p0);
    CHECK_EQ(std::string(SubRowsOf(s, kWork)[11].name), std::string("More"));
    // Eleven entries still fit with Back without paging
    s.structures = {31, 498, 8, 265, 216, 396, 407};
    CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kWorship, kChop, kRubble, kBuild, kRotstone, kFarmer, kCook,
                                                      kJanitor, kMedic, kLogistics}));
}

TEST(Work_lists_roles_by_onboarding_and_buildings) {
    auto s = PlainFollower();
    // Without shrine and buildings: wood, rubble, Build (greyed without a building site, 0x2C5D398 "NoBuildingSites")
    CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kChop, kRubble, kBuild}));
    CHECK(RowOf(SubRowsOf(s, kWork), kBuild)->locked == ActionReason::NoBuildSites);
    s.structures = {118};
    CHECK(RowOf(SubRowsOf(s, kWork), kBuild)->locked == ActionReason::None);
    // Ranch only in the second tier (0x203DC48: type 0x18D)
    s.structures = {392};
    CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kChop, kRubble, kBuild}));
    s.structures = {397, 266, 335, 176, 305, 350, 422, 499};
    CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kChop, kRubble, kBuild, kRotstone, kFarmer, kJanitor,
                                                      kTraits, kRancher, kRefiner, kUndertaker, kBrew}));
    // Onboarding 1 and 4: wood, rubble, farmer only with a farm station; 3: only Worship
    for (const int32_t phase : {1, 4}) {
        s = PlainFollower();
        s.shrine_built = true;
        s.onboarding_phase = phase;
        CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kChop, kRubble}));
        s.structures = {8, 31};
        CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kChop, kRubble, kFarmer}));
    }
    s = PlainFollower();
    s.onboarding_phase = 3;
    s.structures = {8};
    CHECK(SubRows(s, kWork) == (std::vector<uint32_t>{B, kWorship}));
}

TEST(Work_locks_where_the_wheel_or_the_handler_would_refuse) {
    const auto work = [](const cotl::FollowerCommandState& s) -> cotl::ActionRow {
        const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
        const auto* r = RowOf(rows, kWork);
        CHECK(r != nullptr);
        if (!r) return {"", 0, ActionReason::NotAvailable};
        return *r;
    };
    auto s = PlainFollower();
    CHECK(work(s).enabled() && work(s).opens);
    // GiveWorkerCommand 0x203CC90: with Cursed or 0x67 no submenu, [1] alone the handler would reject
    s.own_traits = {0x67};
    CHECK(work(s).locked == ActionReason::CantWork);
    CHECK(!work(s).opens);
    // GiverWorkerCommandItem.IsAvailable 0x203FCC0: ShouldWork (holiday, nudity), then CanWork
    s = PlainFollower();
    s.holiday = true;
    CHECK(work(s).locked == ActionReason::CantWork);
    s = PlainFollower();
    s.nudism = true;
    CHECK(work(s).locked == ActionReason::CantWork);
    s = PlainFollower();
    s.weather = 1;
    CHECK(work(s).locked == ActionReason::TooCold);
    // The snowman is always available
    s.is_snowman = true;
    s.holiday = true;
    CHECK(work(s).enabled());

    // In the blizzard the handler locks every role (0x2C5C848), Build bypasses it
    s = PlainFollower();
    s.weather = 1;
    s.structures = {31};
    const auto sub = SubRowsOf(s, kWork);
    CHECK(RowOf(sub, kChop)->locked == ActionReason::TooCold);
    CHECK(RowOf(sub, kRubble)->locked == ActionReason::TooCold);
    CHECK(RowOf(sub, kBuild)->locked == ActionReason::None);
}

TEST(Timed_action_locks_the_sub_menus_too) {
    auto s = PlainFollower();
    s.timed_action = true;
    const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    CHECK(RowOf(rows, kWork)->locked == ActionReason::FollowerLocked);
    s.timed_action = false;
    s.cursed_state = 0x42;
    s.meals = {0x20};
    s.timed_action = true;
    CHECK(cotl::VisibleActions(cotl::OfferedCommands(s))[0].locked == ActionReason::FollowerLocked);
    CHECK(RowOf(SubRowsOf(s, kEat), 31)->locked == ActionReason::FollowerLocked);
}

TEST(Submit_sends_the_parent_of_a_sub_command) {
    Rig r;
    CHECK(r.client.Submit(7, 31, kEat));
    CHECK_EQ(r.box[COTL_ACT_REQ_COMMAND], uint32_t{31});
    CHECK_EQ(r.box[COTL_ACT_REQ_PARENT], kEat);
    // REQ_PARENT comes before REQ_SEQ
    size_t parent_at = 99, seq_at = 0;
    for (size_t i = 0; i < r.box.stores.size(); ++i) {
        if (r.box.stores[i].first == COTL_ACT_REQ_PARENT) parent_at = i;
        if (r.box.stores[i].first == COTL_ACT_REQ_SEQ) seq_at = i;
    }
    CHECK(parent_at < seq_at);
    r.box.HelperStep(COTL_ACT_STATE_DONE, COTL_ACT_REASON_NONE);
    CHECK(r.client.Submit(7, kBless));
    CHECK_EQ(r.box[COTL_ACT_REQ_PARENT], uint32_t{0}); // 0 again without a parent
}

TEST(New_reasons_have_messages) {
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::NoMeals)), std::string("No meals at the base"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::NoDrinks)), std::string("No drinks at the pub"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::Fasting)), std::string("The cult is fasting"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::NoBuildSites)), std::string("Nothing to build"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::CantWork)), std::string("Cannot work right now"));
}

TEST(Sub_names_fit_the_panel) {
    for (uint32_t c = 0; c < 200; ++c) {
        const auto rows = cotl::VisibleActions({{c, true}});
        if (!rows.empty()) CHECK(std::string(rows[0].name).size() <= 12);
    }
    auto s = PlainFollower();
    s.shrine_built = true;
    s.structures = {31, 498, 8, 265, 216, 396, 407, 416, 420, 397, 148, 304, 343};
    for (int page = 0; page < 2; ++page) {
        for (const auto& r : SubRowsOf(s, kWork, page)) CHECK(std::string(r.name).size() <= 12);
    }
}

// --- Task 7, gifts: Gift 11 with GiftCommands, Give Pet 148, necklaces 62/98/99, 120/122 at the top
// (tools/cotl/README.md, Geschenke)
namespace {
constexpr uint32_t kGift = 11, kGivePet = 148, kRemoveNecklace = 62, kHideNecklace = 98, kShowNecklace = 99,
                   kWinterNecklace = 120, kWeirdNecklace = 122;
// GiftCommands 0x2037CA0: command and ITEM_TYPE in wheel order
const std::vector<std::pair<uint32_t, int32_t>> kGiftItems = {
    {51, 0x2B},  {52, 0x2C},  {53, 0x2D},  {54, 0x2E},  {55, 0x2F},  {56, 0x30},
    {57, 0x31},  {77, 0x7C},  {82, 0x7F},  {79, 0x7B},  {76, 0x7D},  {80, 0x7A},
    {78, 0x7E},  {119, 0xB4}, {120, 0xB5}, {121, 0xB6}, {122, 0xB7}, {123, 0xB8}};
// GiveAnimalPetCommands 0x20395B0
const std::vector<std::pair<uint32_t, int32_t>> kPetItems = {{141, 0xAA}, {142, 0xBC}, {143, 0xB2}, {144, 0xBD},
                                                             {146, 0xB3}, {147, 0xB0}, {145, 0xB1}};
// DataManager.AllNecklaces: all necklaces, without the two gifts
std::vector<int32_t> AllNecklaces() {
    std::vector<int32_t> out;
    for (const auto& [c, item] : kGiftItems) {
        if (item != 0x2B && item != 0x2C) out.push_back(item);
    }
    return out;
}

cotl::FollowerCommandState Demanding() {
    auto s = Everything();
    s.cursed_state = 0x44; // MakeDemand
    s.inspire_unlocked = s.intimidate_unlocked = false;
    s.all_necklaces = AllNecklaces();
    return s;
}

std::vector<uint32_t> AllSubRows(const cotl::FollowerCommandState& s, uint32_t parent) {
    std::vector<uint32_t> out;
    for (int page = 0; page < 3; ++page) {
        for (const uint32_t c : SubRows(s, parent, page)) {
            if (c != B && c != M && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
        }
    }
    return out;
}
} // namespace

TEST(Gift_stands_after_drink_and_before_romance) {
    auto s = Demanding();
    CHECK(Rows(s) == (std::vector<uint32_t>{kBless, kBribe, kEat, kRomance, kPetDog}));
    s.items = {0x2B};
    s.structures = {343};
    CHECK(Rows(s) == (std::vector<uint32_t>{kBless, kBribe, kEat, kDrink, kGift, kRomance, kPetDog}));
    // Normal with DM.InTutorial: Make Demand flat, Gift inside
    s = Everything();
    s.items = {0x2D};
    CHECK(Rows(s) == (std::vector<uint32_t>{kWork, kSleep, kBribe, kDance, kIntimidate, kEat, kGift, kRomance,
                                            kPetDog}));
    // Without Make Demand no Gift
    s.in_tutorial = false;
    CHECK(Rows(s) == (std::vector<uint32_t>{kWork}));
    // Drunk has no Gift
    s = Everything();
    s.items = {0x2D};
    s.drunk = true;
    CHECK(RowOf(cotl::VisibleActions(cotl::OfferedCommands(s)), kGift) == nullptr);
}

TEST(Gift_lists_gifts_and_necklaces_in_the_wheel_order) {
    auto s = Demanding();
    std::vector<uint32_t> want;
    for (auto it = kGiftItems.rbegin(); it != kGiftItems.rend(); ++it) s.items.push_back(it->second);
    for (const auto& [c, item] : kGiftItems) want.push_back(c);
    CHECK(AllSubRows(s, kGift) == want);
    // 18 entries: two pages with Back at the front and More at the end
    const auto p0 = SubRows(s, kGift, 0);
    CHECK_EQ(p0.size(), size_t{12});
    CHECK_EQ(p0.front(), B);
    CHECK_EQ(p0.back(), M);
    for (const auto& r : SubRowsOf(s, kGift)) {
        if (r.command != B && r.command != M) {
            CHECK_EQ(r.parent, kGift);
            CHECK(r.enabled());
        }
    }
    // Only what is in the base inventory
    s.items = {0x7F, 0x2C};
    CHECK(SubRows(s, kGift) == (std::vector<uint32_t>{B, 52, 82}));
    for (const auto& [c, item] : kGiftItems) {
        CHECK(cotl::SupportOf(c) == cotl::ActionSupport::Supported);
        CHECK(std::string(cotl::VisibleActions({{c, true}})[0].name).size() <= 12);
    }
    CHECK(cotl::SupportOf(kGift) == cotl::ActionSupport::Supported);
}

TEST(Gift_locks_when_only_the_bell_is_in_stock) {
    // HasGift 0x1BBE710 counts the bell 0xA3, GiftCommands does not list it: empty submenu, the
    // handler would say "NoGifts"
    auto s = Demanding();
    s.items = {0xA3};
    const auto* gift = RowOf(cotl::VisibleActions(cotl::OfferedCommands(s)), kGift);
    REQUIRE(gift != nullptr);
    CHECK(gift->locked == ActionReason::NoGifts);
    CHECK(!gift->opens);
}

TEST(Necklaces_lock_while_one_is_worn) {
    // GiveItemRoutine 0x2C7E640: with a necklace it says "AlreadyHaveNecklace" if the gift is one
    auto s = Demanding();
    s.items = {0x2B, 0x2D, 0xB5};
    s.necklace = 0x31;
    const auto sub = SubRowsOf(s, kGift);
    CHECK(RowOf(sub, 51)->locked == ActionReason::None);
    CHECK(RowOf(sub, 53)->locked == ActionReason::HasNecklace);
    CHECK(RowOf(sub, 120)->locked == ActionReason::HasNecklace);
    // AllNecklaces unreadable: no lock, the helper checks on the tap
    s.all_necklaces.clear();
    CHECK(RowOf(SubRowsOf(s, kGift), 53)->locked == ActionReason::None);
}

TEST(Give_pet_needs_wool_onboarding_and_an_animal) {
    auto s = Demanding();
    s.items = {0xB1, 0xAA};
    CHECK(RowOf(cotl::VisibleActions(cotl::OfferedCommands(s)), kGivePet) == nullptr);
    s.onboarded_wool = true;
    s.items.push_back(0x2B);
    CHECK(Rows(s) == (std::vector<uint32_t>{kBless, kBribe, kEat, kGift, kGivePet, kRomance, kPetDog}));
    CHECK(SubRows(s, kGivePet) == (std::vector<uint32_t>{B, 141, 145}));
    s.items.clear();
    for (const auto& [c, item] : kPetItems) s.items.push_back(item);
    std::vector<uint32_t> want;
    for (const auto& [c, item] : kPetItems) {
        want.push_back(c);
        CHECK(cotl::SupportOf(c) == cotl::ActionSupport::Supported);
        CHECK(std::string(cotl::VisibleActions({{c, true}})[0].name).size() <= 12);
    }
    CHECK(AllSubRows(s, kGivePet) == want);
    for (const auto& r : SubRowsOf(s, kGivePet)) {
        if (r.command != B) CHECK_EQ(r.parent, kGivePet);
    }
    // GivePetRoutine 0x2C7FAA0: with a pet it says "AlreadyHavePet"
    s.has_dlc_pet = true;
    const auto* pet = RowOf(cotl::VisibleActions(cotl::OfferedCommands(s)), kGivePet);
    REQUIRE(pet != nullptr);
    CHECK(pet->locked == ActionReason::HasPet);
    CHECK(!pet->opens);
}

TEST(Necklace_commands_follow_the_worn_necklace) {
    auto s = Demanding();
    CHECK(RowOf(cotl::VisibleActions(cotl::OfferedCommands(s)), kRemoveNecklace) == nullptr);
    s.necklace = 0x2D;
    CHECK(Rows(s) == (std::vector<uint32_t>{kBless, kBribe, kEat, kRomance, kPetDog, kRemoveNecklace}));
    s.showing_necklace = true;
    CHECK(Rows(s) == (std::vector<uint32_t>{kBless, kBribe, kEat, kRomance, kPetDog, kRemoveNecklace}));
    // OldAge offers only Show/Hide: the module nothing of it (ruling Show/Hide, fix round 1)
    s = Everything();
    s.cursed_state = 0x92;
    s.necklace = 0x2D;
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kEat, kRomance, kPetDog}));
    s.showing_necklace = true;
    CHECK(Rows(s) == (std::vector<uint32_t>{kSleep, kDance, kEat, kRomance, kPetDog}));
    CHECK(cotl::SupportOf(kRemoveNecklace) == cotl::ActionSupport::Supported);
    CHECK(cotl::SupportOf(kHideNecklace) == cotl::ActionSupport::Hidden);
    CHECK(cotl::SupportOf(kShowNecklace) == cotl::ActionSupport::Hidden);
    // GiveItem 3 and GiveLeaderItem 83 stay hidden (rulings on gifts)
    CHECK(cotl::SupportOf(3) == cotl::ActionSupport::Hidden);
    CHECK(cotl::SupportOf(83) == cotl::ActionSupport::Hidden);
}

TEST(Snowman_and_mutated_offer_their_necklace_on_top) {
    auto s = Everything();
    s.is_snowman = true;
    s.items = {0xB5, 0xB7};
    CHECK(Rows(s) == (std::vector<uint32_t>{kWork, kWinterNecklace}));
    const auto* winter = RowOf(cotl::VisibleActions(cotl::OfferedCommands(s)), kWinterNecklace);
    REQUIRE(winter != nullptr);
    CHECK_EQ(winter->parent, uint32_t{0});
    CHECK(!winter->opens);
    s.necklace = 0x2D;
    CHECK(Rows(s) == (std::vector<uint32_t>{kWork}));
    s.necklace = 0;
    s.items = {};
    CHECK(Rows(s) == (std::vector<uint32_t>{kWork}));

    s = Everything();
    s.own_traits = {0x5A}; // Mutated
    s.items = {0xB7};
    CHECK(Rows(s) == (std::vector<uint32_t>{kRomance, kWeirdNecklace, kWork}));
    s.necklace = 0x2D;
    CHECK(Rows(s) == (std::vector<uint32_t>{kRomance, kWork}));
}

TEST(Top_level_pages_with_more_when_it_does_not_fit) {
    auto s = Everything();
    s.extort_unlocked = true;
    s.own_traits = {0x24};
    s.structures = {343};
    s.items = {0x2B, 0xAA};
    s.onboarded_wool = true;
    s.necklace = 0x2D;
    s.all_necklaces = AllNecklaces();
    const auto all = Rows(s);
    // Work, Sleep, Extort, Bribe, Dance, Intimidate, Eat, Drink, Gift, Pet, Romance, Pet Dog, 62, Bully, Reassure
    REQUIRE(all.size() == 15);
    const auto p0 = cotl::MenuActions(cotl::OfferedCommands(s), {0, 0});
    const auto p1 = cotl::MenuActions(cotl::OfferedCommands(s), {0, 1});
    REQUIRE(p0.size() == 12);
    CHECK_EQ(p0.back().command, M);
    for (size_t i = 0; i < 11; ++i) CHECK_EQ(p0[i].command, all[i]);
    REQUIRE(p1.size() == 5);
    CHECK_EQ(p1.back().command, M);
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(p1[i].command, all[11 + i]);
    CHECK_EQ(cotl::MenuActions(cotl::OfferedCommands(s), {0, 2}).front().command, all[0]);
    // Without overflow no More
    s.necklace = 0;
    s.items = {};
    CHECK_EQ(cotl::MenuActions(cotl::OfferedCommands(s), {0, 0}).size(), size_t{12});
    CHECK(RowOf(cotl::MenuActions(cotl::OfferedCommands(s), {0, 0}), M) == nullptr);
}

TEST(Timed_action_locks_gifts_too) {
    auto s = Demanding();
    s.items = {0x2B, 0xAA};
    s.onboarded_wool = true;
    s.timed_action = true;
    const auto rows = cotl::VisibleActions(cotl::OfferedCommands(s));
    CHECK(RowOf(rows, kGift)->locked == ActionReason::FollowerLocked);
    CHECK(RowOf(rows, kGivePet)->locked == ActionReason::FollowerLocked);
}

TEST(Gift_reasons_have_messages) {
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::NoGifts)), std::string("No gifts in stock"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::HasNecklace)), std::string("Already wears a necklace"));
    CHECK_EQ(std::string(cotl::ActionMessage(ActionReason::HasPet)), std::string("Already has a pet"));
}

TEST(walk_message_says_arrived_when_done) {
    cotl::ActionStatus st;
    st.helper_alive = true;
    st.state = cotl::ActionState::Done;
    CHECK_EQ(std::string(cotl::WalkMessage(st)), std::string("Arrived"));
    st.state = cotl::ActionState::Walking;
    CHECK_EQ(std::string(cotl::WalkMessage(st)), std::string("Walking over"));
    st.state = cotl::ActionState::Refused;
    st.reason = cotl::ActionReason::LambBusy;
    CHECK_EQ(std::string(cotl::WalkMessage(st)), std::string("Lamb is busy"));
}
