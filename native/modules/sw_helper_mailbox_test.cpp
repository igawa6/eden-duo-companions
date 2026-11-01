// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-only tests for the Pokemon Sword guest helper (no emulator, no game dump):
//   1. Mailbox protocol (sw_helper::Bridge) against a fake in-memory mailbox + fake clock, with a
//      tiny model of the guest side (beat, ack, cancel) written from sw_guest_helper.S.
//   2. Tap-time preconditions (CheckFight/CheckRun/CheckOpenMenu).
//   3. HUD hide: the module's HudHide against a model of sw_hud_entry (one battle_top_00 frame)
//      with a fake pane tree -- hide, keep hidden, restore only what it hid, forget a gone layout,
//      drop the hide when the module's heartbeat stops.
//   4. Field channel (FieldBridge) against a model of sw_field_entry: field beat = "field live",
//      medicine / repel / fly round trips, the FLY_QUERY mask, cancel when the field stops (a menu
//      opened), stale epoch, independence from the battle channel.
//   5. Stub encoding: the shipped load-plan JSON -- hook patch, relocation targets, no system
//      instructions, the magic constants, and every mailbox offset the guest code touches through
//      its mailbox base register (x20) is one sw_helper_mailbox.h declares.

#include "sw_helper_mailbox.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace sw_helper;

struct Fake {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(MailboxSize, 0);
    std::uint32_t Get(std::uint32_t o) const {
        std::uint32_t v;
        std::memcpy(&v, bytes.data() + o, 4);
        return v;
    }
    void Set(std::uint32_t o, std::uint32_t v) { std::memcpy(bytes.data() + o, &v, 4); }

    // One frame of sw_guest_helper.S while the command menu waits. `answer` is the result the
    // native checks would give for a pending request (the commit itself is the game's).
    std::uint32_t kind = BattleKindAllowed; // wild single, offline
    void GuestFrame(Result answer = Result::Ok, std::uint32_t value = 0) {
        Set(OffsetGuestMagic, GuestMagic);
        Set(OffsetWaitBeat, Get(OffsetWaitBeat) + 1);
        Set(OffsetBattleKind, kind);
        if (Get(OffsetModuleMagic) != ModuleMagic) return;
        const std::uint32_t seq = Get(OffsetReqSeq);
        if (seq == Get(OffsetAckSeq)) return;
        Result code = answer;
        if (Get(OffsetCancelSeq) == seq) code = Result::Cancelled;
        else if (Get(EpochOffset) == 0 || Get(OffsetReqEpoch) != Get(EpochOffset)) code = Result::StaleEpoch;
        else if (!(kind & BattleKindAllowed)) code = Result::BattleType;
        else if (Get(OffsetReqOp) < 1 || Get(OffsetReqOp) > 7) code = Result::BadOp;
        Set(OffsetResultCode, static_cast<std::uint32_t>(code));
        Set(OffsetResultValue, code == Result::Ok ? value : 0);
        Set(OffsetAckBeat, Get(OffsetWaitBeat));
        Set(OffsetAckSeq, seq);
    }
};

// sw_alive_entry: one battle-client frame (any phase of the battle).
void AliveFrame(Fake& f, std::uint64_t client = 0x21'9532'EE60ull) {
    f.Set(OffsetGuestMagic, GuestMagic);
    f.Set(OffsetAliveBeat, f.Get(OffsetAliveBeat) + 1);
    f.Set(OffsetAliveClient, static_cast<std::uint32_t>(client));
    f.Set(OffsetAliveClient + 4, static_cast<std::uint32_t>(client >> 32));
}

struct Load {
    Fake* f;
    bool operator()(std::uint32_t o, std::uint32_t* v) const {
        if (o % 4 || o + 4 > MailboxSize) return false;
        *v = f->Get(o);
        return true;
    }
};
struct Store {
    Fake* f;
    bool operator()(std::uint32_t o, std::uint32_t v) const {
        if (o % 4 || o + 4 > MailboxSize) return false;
        f->Set(o, v);
        return true;
    }
};
using TestBridge = Bridge<Load, Store>;
using TestHud = HudHide<Load, Store>;

// The four plates' N_inout panes of one battle_top_00 layout: pane address (0 = not found) and the
// flags byte (bit 0 = visible).
struct FakeLayout {
    std::uint64_t layout = 0x21'8916'8798ull;
    std::array<std::uint64_t, HudPlates> pane{0x21'82C0'5710ull, 0x21'82BF'6950ull, 0x21'82C0'B860ull, 0x21'82BF'BA50ull};
    std::array<std::uint8_t, HudPlates> flags{0x83, 0x83, 0x13, 0x13};
};

std::uint64_t Get64(const Fake& f, std::uint32_t o) {
    return f.Get(o) | (static_cast<std::uint64_t>(f.Get(o + 4)) << 32);
}
void Set64(Fake& f, std::uint32_t o, std::uint64_t v) {
    f.Set(o, static_cast<std::uint32_t>(v));
    f.Set(o + 4, static_cast<std::uint32_t>(v >> 32));
}

// sw_hud_entry for the battle_top_00 layout, step by step as the assembly does it.
void HudFrame(Fake& f, FakeLayout& l) {
    if (f.Get(OffsetBattleSerial) != f.Get(OffsetHudSeenBattleSerial) ||
        Get64(f, OffsetAliveClient) != Get64(f, OffsetHudSeenClient)) {
        f.Set(OffsetHudSeenBattleSerial, f.Get(OffsetBattleSerial));
        Set64(f, OffsetHudSeenClient, Get64(f, OffsetAliveClient));
        f.Set(OffsetHudState, 0);
        Set64(f, OffsetHudLayout, 0);
        for (int i = 0; i < HudPlates; ++i) Set64(f, OffsetHudPane + 8 * i, 0);
    }
    f.Set(OffsetGuestMagic, GuestMagic);
    f.Set(OffsetHudFrame, f.Get(OffsetHudFrame) + 1);
    if (f.Get(OffsetHudReqBeat) != f.Get(OffsetHudSeenBeat)) {
        f.Set(OffsetHudSeenBeat, f.Get(OffsetHudReqBeat));
        f.Set(OffsetHudStale, 0);
    } else {
        f.Set(OffsetHudStale, f.Get(OffsetHudStale) + 1);
    }
    const bool want = f.Get(OffsetModuleMagic) == ModuleMagic && f.Get(OffsetHudReq) == 1 &&
                      f.Get(OffsetHudStale) <= HudStaleFrames;
    std::uint32_t state = f.Get(OffsetHudState);
    if (want) {
        if (Get64(f, OffsetHudLayout) != l.layout) {
            state = 0;
            Set64(f, OffsetHudLayout, l.layout);
            for (int i = 0; i < HudPlates; ++i) Set64(f, OffsetHudPane + 8 * i, 0);
        }
        state &= 0xF;
        for (int i = 0; i < HudPlates; ++i) {
            if (!l.pane[i]) continue;
            state |= 0x100u << i;
            if (!(l.flags[i] & 1)) continue;
            l.flags[i] &= 0xFE;
            Set64(f, OffsetHudPane + 8 * i, l.pane[i]);
            state |= 1u << i;
        }
        f.Set(OffsetHudState, state);
        return;
    }
    if ((state & 0xF) && Get64(f, OffsetHudLayout) == l.layout) {
        for (int i = 0; i < HudPlates; ++i)
            if ((state >> i & 1) && l.pane[i] && l.pane[i] == Get64(f, OffsetHudPane + 8 * i)) l.flags[i] |= 1;
    }
    f.Set(OffsetHudState, 0);
    Set64(f, OffsetHudLayout, 0);
    for (int i = 0; i < HudPlates; ++i) Set64(f, OffsetHudPane + 8 * i, 0);
}

using TestField = FieldBridge<Load, Store>;

// sw_field_entry: one field frame. `answer` stands for what the game's own checks give; a
// FLY_QUERY answers with `mask`/`state`.
void FieldFrame(Fake& f, Result answer = Result::Ok, std::uint64_t mask = 0, std::uint32_t state = 0) {
    f.Set(OffsetGuestMagic, GuestMagic);
    f.Set(OffsetFieldBeat, f.Get(OffsetFieldBeat) + 1);
    f.Set(OffsetFieldFree, answer == Result::NotFree ? 0 : 1);
    if (f.Get(OffsetModuleMagic) != ModuleMagic) return;
    const std::uint32_t seq = f.Get(OffsetFieldReqSeq);
    if (seq == f.Get(OffsetFieldAckSeq)) return;
    Result code = answer;
    std::uint32_t value = 0;
    const auto op = static_cast<FieldOp>(f.Get(OffsetFieldReqOp));
    if (f.Get(OffsetFieldCancelSeq) == seq) code = Result::Cancelled;
    else if (f.Get(EpochOffset) == 0 || f.Get(OffsetFieldReqEpoch) != f.Get(EpochOffset)) code = Result::StaleEpoch;
    else if (op == FieldOp::None || static_cast<std::uint32_t>(op) > 7) code = Result::BadOp;
    else if (op != FieldOp::FlyQuery && f.Get(OffsetFieldFree) != 1) code = Result::NotFree;
    else if (op == FieldOp::Medicine && !IsFieldMedicine(static_cast<int>(f.Get(OffsetFieldReqArg)))) code = Result::NotUsable;
    else if (op == FieldOp::Repel && !IsRepel(static_cast<int>(f.Get(OffsetFieldReqArg)))) code = Result::NotUsable;
    else if (op == FieldOp::FlyQuery) {
        code = Result::Ok;
        Set64(f, OffsetFlyQueryMask, mask);
        f.Set(OffsetFlyQueryState, state);
        value = f.Get(OffsetFlyQueryCount);
    }
    if (code == Result::Ok && (op == FieldOp::Medicine || op == FieldOp::Repel)) value = f.Get(OffsetFieldReqArg);
    f.Set(OffsetFieldResultCode, static_cast<std::uint32_t>(code));
    f.Set(OffsetFieldResultValue, code == Result::Ok ? value : 0);
    f.Set(OffsetFieldAckBeat, f.Get(OffsetFieldBeat));
    f.Set(OffsetFieldAckSeq, seq);
}

void TestFieldChannel() {
    Fake f;
    f.Set(EpochOffset, 3);
    TestBridge battle{Load{&f}, Store{&f}};
    TestField field{Load{&f}, Store{&f}};
    assert(battle.Initialize() && field.Initialize());
    std::int64_t t = 1000;
    field.Update(t);
    assert(!field.status().helper_present && !field.status().live);
    // A few field frames at ~30 fps: live.
    for (int i = 0; i < 4; ++i) {
        FieldFrame(f);
        t += 33;
        field.Update(t);
    }
    assert(field.status().helper_present && field.status().live && !field.status().busy);
    // Medicine (Potion on slot 0): staged with the slot's encryption constant, answered by the
    // guest's next frame.
    assert(field.Stage(FieldOp::Medicine, 17, 0, t, 0, 0, 0xEDB84209u, 2));
    assert(f.Get(OffsetFieldExpectEc) == 0xEDB84209u && f.Get(OffsetFieldMove) == 2);
    assert(!field.Stage(FieldOp::Repel, 77, 0, t)); // one at a time
    assert(f.Get(OffsetFieldReqOp) == 1 && f.Get(OffsetFieldReqArg) == 17 && f.Get(OffsetFieldReqExpect) == 0);
    assert(f.Get(OffsetFieldReqEpoch) == 3 && f.Get(OffsetFieldReqSeq) == f.Get(OffsetFieldAckSeq) + 1);
    assert(f.Get(OffsetReqSeq) == f.Get(OffsetAckSeq)); // the battle channel is untouched
    FieldFrame(f);
    t += 33;
    field.Update(t);
    FieldOutcome out;
    assert(field.TakeOutcome(out) && out.answered && out.op == FieldOp::Medicine && out.code == Result::Ok &&
           out.value == 17);
    assert(!field.TakeOutcome(out));
    // PP medicine carries the selected move rather than silently targeting slot0. Reject a
    // malformed index before publishing any new request sequence.
    const auto before_pp = f.Get(OffsetFieldReqSeq);
    assert(!field.Stage(FieldOp::Medicine, 38, 0, t, 0, 0, 0xEDB84209u, 4));
    assert(f.Get(OffsetFieldReqSeq) == before_pp);
    assert(field.Stage(FieldOp::Medicine, 38, 0, t, 0, 0, 0xEDB84209u, 3));
    assert(f.Get(OffsetFieldMove) == 3);
    FieldFrame(f);
    field.Update(t += 33);
    assert(field.TakeOutcome(out) && out.code == Result::Ok && out.value == 38);
    // Party reorder: both slots + both members' encryption constants, validated before any
    // sequence is published (distinct slots 0..5, non-zero constants).
    const auto before_swap = f.Get(OffsetFieldReqSeq);
    assert(!field.Stage(FieldOp::PartySwap, 1, 1, t, 0, 0, 0x11111111u, 0x22222222u));
    assert(!field.Stage(FieldOp::PartySwap, 6, 0, t, 0, 0, 0x11111111u, 0x22222222u));
    assert(!field.Stage(FieldOp::PartySwap, 0, 2, t, 0, 0, 0, 0x22222222u));
    assert(!field.Stage(FieldOp::PartySwap, 0, 2, t, 0, 0, 0x11111111u, 0));
    assert(f.Get(OffsetFieldReqSeq) == before_swap);
    assert(field.Stage(FieldOp::PartySwap, 0, 2, t, 0, 0, 0x11111111u, 0x22222222u));
    assert(f.Get(OffsetFieldReqOp) == 7 && f.Get(OffsetFieldReqArg) == 0 && f.Get(OffsetFieldReqExpect) == 2 &&
           f.Get(OffsetFieldExpectEc) == 0x11111111u && f.Get(OffsetFieldMove) == 0x22222222u);
    FieldFrame(f, Result::TargetChanged);
    field.Update(t += 33);
    assert(field.TakeOutcome(out) && out.op == FieldOp::PartySwap && out.code == Result::TargetChanged);
    // The game's own refusal comes back as the result code.
    assert(field.Stage(FieldOp::Repel, 77, 0, t));
    FieldFrame(f, Result::RepelActive);
    t += 33;
    field.Update(t);
    assert(field.TakeOutcome(out) && out.code == Result::RepelActive && out.value == 0);
    // Fly: hashes in the mailbox before the sequence number.
    assert(field.Stage(FieldOp::Fly, 0, 0, t, 0x776779717EA44D57ull, 0x5262127BB7E43408ull));
    assert(Get64(f, OffsetFieldZone) == 0x776779717EA44D57ull && Get64(f, OffsetFieldLanding) == 0x5262127BB7E43408ull);
    FieldFrame(f);
    t += 33;
    field.Update(t);
    assert(field.TakeOutcome(out) && out.op == FieldOp::Fly && out.code == Result::Ok);
    // FLY_QUERY: pairs written, mask + state read back.
    std::uint64_t pairs[3][2] = {{1, 2}, {3, 4}, {5, 6}};
    assert(field.StageQuery(pairs, 3, t));
    assert(f.Get(OffsetFlyQueryCount) == 3 && Get64(f, OffsetFlyQueryPairs + 16) == 3 && Get64(f, OffsetFlyQueryPairs + 40) == 6);
    f.Set(OffsetBikeUsable, 1); // native CanBike snapshot is returned with the read-only query
    FieldFrame(f, Result::Ok, 0x5, 0);
    t += 33;
    field.Update(t);
    assert(field.TakeOutcome(out) && out.op == FieldOp::FlyQuery && out.fly_mask == 0x5 && out.fly_state == 0 && out.bike_usable);
    std::uint64_t too_many[FlyQueryMax + 1][2]{};
    assert(!field.StageQuery(too_many, FlyQueryMax + 1, t));
    // The field stops (a native menu opened, a battle began): the request is cancelled and never
    // runs when the field comes back.
    assert(field.Stage(FieldOp::Medicine, 17, 1, t));
    for (int i = 0; i < 40; ++i) {
        t += 50;
        field.Update(t);
    }
    assert(!field.status().live);
    assert(f.Get(OffsetFieldCancelSeq) == f.Get(OffsetFieldReqSeq));
    assert(field.TakeOutcome(out) && !out.answered && out.code == Result::Cancelled);
    FieldFrame(f); // the field resumes: the guest refuses the cancelled sequence
    assert(f.Get(OffsetFieldResultCode) == static_cast<std::uint32_t>(Result::Cancelled));
    // A host pause bumps the epoch: the field must be seen running again, and a request staged
    // under the old epoch is refused as stale.
    for (int i = 0; i < 3; ++i) {
        FieldFrame(f);
        t += 33;
        field.Update(t);
    }
    assert(field.status().live);
    assert(field.Stage(FieldOp::Repel, 79, 0, t));
    f.Set(EpochOffset, 4);
    FieldFrame(f);
    t += 33;
    field.Update(t);
    assert(field.TakeOutcome(out) && out.code == Result::StaleEpoch);
    // Battle frames never answer the field channel and field frames never answer the battle one.
    assert(field.Stage(FieldOp::Repel, 79, 0, t));
    f.GuestFrame();
    assert(f.Get(OffsetFieldAckSeq) != f.Get(OffsetFieldReqSeq));
    FieldFrame(f);
    assert(f.Get(OffsetFieldAckSeq) == f.Get(OffsetFieldReqSeq));
    // Review H1: a dialogue/event (field_free 0) shows on the module side and the guest refuses a
    // request that was staged just before it began; the read-only query still answers.
    for (int i = 0; i < 3; ++i) {
        FieldFrame(f);
        t += 33;
        field.Update(t);
    }
    assert(field.status().player_free);
    assert(field.Stage(FieldOp::Medicine, 17, 0, t, 0, 0, 1));
    FieldFrame(f, Result::NotFree);
    t += 33;
    field.Update(t);
    assert(!field.status().player_free && field.status().live);
    assert(field.TakeOutcome(out) && out.answered && out.code == Result::NotFree);
    assert(field.StageQuery(pairs, 3, t));
    FieldFrame(f, Result::NotFree);
    t += 33;
    field.Update(t);
    assert(field.TakeOutcome(out) && out.op == FieldOp::FlyQuery && out.code == Result::Ok);
    // Sets the guest re-checks (review M3: the verified HP-restoring medicine only).
    for (int id : {17, 23, 24, 25, 26, 30, 31, 32, 33}) assert(IsFieldMedicine(id));
    for (int id : {0, 16, 34, 35, 36, 37, 42, 50, 52, 54, 63, 64, 76}) assert(!IsFieldMedicine(id));
    assert(kFieldMedicineMask == 0x2803C3FFFE0000ull); // the literal sw_guest_helper.S loads
    // Teardown (review L1): a staged request is cancelled and the battle bridge withdraws the
    // module magic, so no channel takes a request afterwards.
    for (int i = 0; i < 3; ++i) {
        FieldFrame(f);
        t += 33;
        field.Update(t);
        battle.Update(t);
    }
    assert(field.Stage(FieldOp::Repel, 79, 0, t));
    field.Shutdown();
    battle.Shutdown();
    assert(f.Get(OffsetFieldCancelSeq) == f.Get(OffsetFieldReqSeq) && f.Get(OffsetModuleMagic) == 0);
    FieldFrame(f);
    assert(f.Get(OffsetFieldAckSeq) != f.Get(OffsetFieldReqSeq)); // no magic: never even looked at
    assert(IsRepel(76) && IsRepel(77) && IsRepel(79) && !IsRepel(78));
    std::puts("field channel ok");
}

BattleView Battle() {
    BattleView v;
    v.in_battle = true;
    v.own_ok = true;
    v.move_id = {85, 98, 0, 0};
    v.pp = {15, 0, 0, 0};
    return v;
}

void TestPresenceAndMenuWaiting() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    assert(b.Initialize());
    assert(f.Get(OffsetModuleMagic) == ModuleMagic);
    assert(f.Get(EpochOffset) == 0); // the host owns the epoch word
    b.Update(0);
    assert(!b.status().helper_present && !b.status().menu_waiting);
    f.Set(EpochOffset, 7);
    f.GuestFrame();
    b.Update(10);
    assert(b.status().helper_present);
    assert(!b.status().menu_waiting); // the first beat value read is history, not a beat
    f.GuestFrame();
    b.Update(43);
    assert(b.status().menu_waiting);
    for (int t = 76; t <= 400; t += 33) {
        f.GuestFrame();
        b.Update(t);
    }
    assert(b.status().menu_waiting);
    assert(b.FrameMs() >= 30 && b.FrameMs() <= 40);
    assert(b.BeatWindowMs() == 100); // 1.5 frames, floored at 100 ms
    b.Update(560);                   // menu closed: no beats for > window
    assert(!b.status().menu_waiting);
    f.GuestFrame();
    b.Update(570);
    assert(b.status().menu_waiting);
    f.Set(EpochOffset, 8); // host pause/invalidate: must see the menu waiting again
    b.Update(580);
    assert(!b.status().menu_waiting);
    std::puts("presence/menu waiting ok");
}

void Ready(Fake& f, TestBridge& b, std::int64_t& t) {
    f.Set(EpochOffset, 3);
    for (int i = 0; i < 3; ++i) {
        f.GuestFrame();
        b.Update(t += 33);
    }
    assert(b.status().menu_waiting);
}

void TestFightRoundTrip() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    b.Initialize();
    Ready(f, b, t);
    assert(CheckFight(b.status(), Battle(), 0) == Refusal::None);
    assert(b.Stage(Op::Fight, 0, 85, t));
    assert(f.Get(OffsetReqOp) == 1 && f.Get(OffsetReqArg) == 0 && f.Get(OffsetReqExpect) == 85);
    assert(f.Get(OffsetReqEpoch) == 3 && f.Get(OffsetReqSeq) == 1);
    assert(b.status().busy);
    assert(CheckFight(b.status(), Battle(), 0) == Refusal::Busy);
    assert(!b.Stage(Op::Run, 0, 0, t)); // one request in flight at most
    f.GuestFrame(Result::Ok, 85);
    b.Update(t += 33);
    Outcome out;
    assert(b.TakeOutcome(out));
    assert(out.answered && out.op == Op::Fight && out.code == Result::Ok && out.value == 85);
    assert(!b.TakeOutcome(out));
    assert(!b.status().busy && b.acks() == 1);
    // The commit closed the native menu: no second command until the guest beats again.
    assert(!b.status().menu_waiting);
    assert(CheckRun(b.status(), Battle()) == Refusal::MenuNotWaiting);
    b.Update(t += 10); // same beat value: still not waiting
    assert(!b.status().menu_waiting);
    f.GuestFrame(); // next turn's menu
    b.Update(t += 33);
    assert(b.status().menu_waiting);
    // A second command next turn gets a fresh sequence number.
    assert(b.Stage(Op::Run, 0, 0, t));
    assert(f.Get(OffsetReqSeq) == 2 && f.Get(OffsetReqOp) == 2);
    f.GuestFrame(Result::NotFirst);
    b.Update(t += 33);
    assert(b.TakeOutcome(out) && out.answered && out.code == Result::NotFirst && out.op == Op::Run);
    std::puts("fight/run round trip ok");
}

// Review H2/M4: a double battle (rule 1) or a link battle keeps every command locked, and a
// request that slips through is refused by the guest (BattleType).
void TestBattleKindGate() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    b.Initialize();
    Ready(f, b, t);
    assert(b.status().offline_single && CheckFight(b.status(), Battle(), 0) == Refusal::None);
    f.kind = 0x00000101u; // trainer double: rule 1, competitor 1, no verdict bit
    f.GuestFrame();
    b.Update(t += 33);
    assert(!b.status().offline_single && b.status().battle_kind == 0x101u);
    assert(CheckFight(b.status(), Battle(), 0) == Refusal::BattleType);
    assert(CheckRun(b.status(), Battle()) == Refusal::BattleType);
    assert(b.Stage(Op::Fight, 0, 85, t)); // the mailbox itself does not judge; the guest does
    f.GuestFrame();
    b.Update(t += 33);
    Outcome out;
    assert(b.TakeOutcome(out) && out.answered && out.code == Result::BattleType);
    std::puts("battle kind gate ok");
}

void TestTimeoutCancelsAndNeverRunsLater() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    b.Initialize();
    Ready(f, b, t);
    assert(b.Stage(Op::Fight, 0, 85, t));
    // The menu closes before the guest runs (the player used the native menu).
    b.Update(t += 300);
    assert(f.Get(OffsetCancelSeq) == 1);
    Outcome out;
    assert(!b.TakeOutcome(out));
    b.Update(t += 300);
    assert(b.TakeOutcome(out) && !out.answered && out.code == Result::Cancelled);
    // Next turn's menu: the guest refuses the old request instead of running it.
    f.GuestFrame(Result::Ok, 85);
    assert(f.Get(OffsetAckSeq) == 1 && f.Get(OffsetResultCode) == static_cast<std::uint32_t>(Result::Cancelled));
    std::puts("timeout -> cancel ok");
}

void TestStaleEpochAndReinitialize() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    b.Initialize();
    Ready(f, b, t);
    assert(b.Stage(Op::Fight, 0, 85, t));
    f.Set(EpochOffset, 4); // paused between staging and the guest frame
    f.GuestFrame(Result::Ok, 85);
    b.Update(t += 33);
    Outcome out;
    assert(b.TakeOutcome(out) && out.code == Result::StaleEpoch);
    // A module reload continues the sequence instead of replaying an answered one.
    TestBridge b2{Load{&f}, Store{&f}};
    assert(b2.Initialize());
    assert(f.Get(OffsetReqSeq) == f.Get(OffsetAckSeq));
    std::int64_t t2 = 1000;
    Ready(f, b2, t2);
    assert(b2.Stage(Op::Run, 0, 0, t2));
    assert(f.Get(OffsetReqSeq) == 2);
    std::puts("stale epoch/reinitialize ok");
}

void TestPreconditions() {
    Status s;
    BattleView v = Battle();
    assert(CheckFight(s, v, 0) == Refusal::NoHelper);
    s.helper_present = true;
    assert(CheckFight(s, BattleView{}, 0) == Refusal::NotInBattle);
    assert(CheckFight(s, v, 0) == Refusal::MenuNotWaiting);
    s.menu_waiting = true;
    assert(CheckFight(s, v, 0) == Refusal::BattleType); // review H2/M4: not (yet) an offline single
    assert(CheckRun(s, v) == Refusal::BattleType);
    s.offline_single = true;
    assert(CheckFight(s, v, 0) == Refusal::None);
    assert(CheckFight(s, v, -1) == Refusal::BadSlot);
    assert(CheckFight(s, v, 4) == Refusal::BadSlot);
    assert(CheckFight(s, v, 1) == Refusal::NoPp);
    assert(CheckFight(s, v, 2) == Refusal::NoMove);
    s.busy = true;
    assert(CheckFight(s, v, 0) == Refusal::Busy);
    s.busy = false;
    s.pos_info = 0x0000; // position 0, first position 0
    assert(CheckRun(s, v) == Refusal::None);
    s.pos_info = 0x0001; // second position of a double battle
    assert(CheckRun(s, v) == Refusal::NotFirst);
    BattleView out_of_battle;
    assert(CheckRun(s, out_of_battle) == Refusal::NotInBattle);
    std::puts("preconditions ok");
}

// Opening a native picker is separate from committing its eventual item/switch choice.
// These are host guard/protocol checks; actual native no-turn behavior needs a live check.
void TestOpenMenuPreconditions() {
    Status s;
    BattleView v = Battle();
    assert(CheckOpenMenu(s, v) == Refusal::NoHelper);
    s.helper_present = true;
    assert(CheckOpenMenu(s, BattleView{}) == Refusal::NotInBattle);
    auto no_own = v;
    no_own.own_ok = false;
    assert(CheckOpenMenu(s, no_own) == Refusal::NotInBattle);
    assert(CheckOpenMenu(s, v) == Refusal::MenuNotWaiting);
    s.menu_waiting = true;
    assert(CheckOpenMenu(s, v) == Refusal::BattleType);
    s.offline_raid = true;
    assert(CheckOpenMenu(s, v) == Refusal::BattleType); // Raid promotion must not unlock these pickers.
    s.offline_single = true;
    s.busy = true;
    assert(CheckOpenMenu(s, v) == Refusal::Busy);
    s.busy = false;
    s.pos_info = 0x0001;
    assert(CheckOpenMenu(s, v) == Refusal::NotFirst);
    s.pos_info = 0x0101;
    assert(CheckOpenMenu(s, v) == Refusal::None); // compares positions; not a hard-coded slot0 test.
    // Unlike direct Fight/Switch/Item, opening a picker does not require a move, PP,
    // a selected member or a selected owned item. Native UI handles the later selection.
    v.move_id.fill(0);
    v.pp.fill(0);
    assert(CheckOpenMenu(s, v) == Refusal::None);
    std::puts("native picker guards: helper/battle/own/menu/offline/first/busy, independent from command choice");
}

void TestOpenMenuRoundTripAndCancellation() {
    assert(static_cast<std::uint32_t>(Op::OpenBag) == 6);
    assert(static_cast<std::uint32_t>(Op::OpenPokemon) == 7);
    assert(static_cast<std::uint32_t>(Op::Decide) == 8 && DecideFight == 1 && DecideBag == 2);
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    assert(b.Initialize());
    Ready(f, b, t);
    assert(CheckOpenMenu(b.status(), Battle()) == Refusal::None);
    assert(b.Stage(Op::OpenBag, 0, 0, t));
    assert(f.Get(OffsetReqOp) == 6 && f.Get(OffsetReqArg) == 0 && f.Get(OffsetReqExpect) == 0);
    assert(f.Get(OffsetReqEpoch) == 3 && f.Get(OffsetReqSeq) == 1);
    assert(CheckOpenMenu(b.status(), Battle()) == Refusal::Busy);
    assert(!b.Stage(Op::OpenPokemon, 0, 0, t));
    f.GuestFrame(Result::Ok, 6);
    b.Update(t += 33);
    Outcome out;
    assert(b.TakeOutcome(out) && out.answered && out.op == Op::OpenBag && out.code == Result::Ok && out.value == 6);
    assert(!b.TakeOutcome(out));
    assert(!b.status().busy && !b.status().menu_waiting);
    assert(CheckOpenMenu(b.status(), Battle()) == Refusal::MenuNotWaiting);
    // The native picker may stay open arbitrarily long. An old heartbeat never rearms a tap.
    b.Update(t += 1000);
    assert(!b.status().menu_waiting);
    f.GuestFrame(); // native B/cancel returns to the command wait hook
    b.Update(t += 33);
    assert(CheckOpenMenu(b.status(), Battle()) == Refusal::None);
    assert(b.Stage(Op::OpenPokemon, 0, 0, t));
    assert(f.Get(OffsetReqOp) == 7 && f.Get(OffsetReqSeq) == 2);
    f.GuestFrame(Result::Blocked);
    b.Update(t += 33);
    assert(b.TakeOutcome(out) && out.answered && out.op == Op::OpenPokemon && out.code == Result::Blocked);
    assert(b.status().menu_waiting); // native condition0x22 refused it before state transition
    assert(b.Stage(Op::OpenPokemon, 0, 0, t));
    f.GuestFrame(Result::Ok, 7);
    b.Update(t += 33);
    assert(b.TakeOutcome(out) && out.answered && out.op == Op::OpenPokemon && out.code == Result::Ok && out.value == 7);
    assert(!b.status().menu_waiting);

    for (const Op op : {Op::OpenBag, Op::OpenPokemon}) {
        Fake delayed;
        TestBridge bridge{Load{&delayed}, Store{&delayed}};
        std::int64_t at = 0;
        assert(bridge.Initialize()); Ready(delayed, bridge, at);
        assert(bridge.Stage(op, 0, 0, at));
        bridge.Update(at += bridge.AckWindowMs() + 1);
        const auto cancelled = delayed.Get(OffsetReqSeq);
        assert(delayed.Get(OffsetCancelSeq) == cancelled);
        bridge.Update(at += bridge.AckWindowMs() + 1);
        assert(bridge.TakeOutcome(out) && !out.answered && out.op == op && out.code == Result::Cancelled);
        delayed.GuestFrame(Result::Ok, static_cast<std::uint32_t>(op));
        assert(delayed.Get(OffsetAckSeq) == cancelled);
        assert(delayed.Get(OffsetResultCode) == static_cast<std::uint32_t>(Result::Cancelled));
        bridge.Update(at += 33);
        assert(!bridge.TakeOutcome(out)); // late ack cannot revive/duplicate a finished request
    }

    Fake stale;
    TestBridge bridge{Load{&stale}, Store{&stale}};
    std::int64_t at = 0;
    assert(bridge.Initialize()); Ready(stale, bridge, at);
    assert(bridge.Stage(Op::OpenPokemon, 0, 0, at));
    stale.Set(EpochOffset, 4);
    stale.GuestFrame(Result::Ok, 7);
    bridge.Update(at += 33);
    assert(bridge.TakeOutcome(out) && out.op == Op::OpenPokemon && out.code == Result::StaleEpoch);
    std::puts("native picker op6/7 protocol: distinct payloads, one request, ack/menu-rearm, refusal, timeout/cancel and stale epoch");
}

void TestBallGuardsAndNativeIdentityPayload() {
    Status s;
    BattleView v = Battle();
    v.foe_ok = true;
    assert(CheckBall(s, v, 4, 1) == Refusal::NoHelper);
    s.helper_present = true;
    assert(CheckBall(s, v, 4, 1) == Refusal::MenuNotWaiting);
    s.menu_waiting = true;
    assert(CheckBall(s, v, 4, 1) == Refusal::BattleType);
    s.offline_single = true;
    s.battle_kind = BattleKindAllowed;
    assert(CheckBall(s, BattleView{}, 4, 1) == Refusal::NotInBattle);
    auto missing_foe = v; missing_foe.foe_ok = false;
    assert(CheckBall(s, missing_foe, 4, 1) == Refusal::NotInBattle);
    auto fainted_foe = v; fainted_foe.foe_fainted = true;
    assert(CheckBall(s, fainted_foe, 4, 1) == Refusal::NoEffect);
    assert(CheckBall(s, v, 4, 0) == Refusal::NoItem);
    assert(CheckBall(s, v, 4, -1) == Refusal::NoItem);
    assert(CheckBall(s, v, 17, 1) == Refusal::NotBattleItem); // Potion is medicine, not a ball.
    s.busy = true;
    assert(CheckBall(s, v, 4, 1) == Refusal::Busy);
    s.busy = false;
    s.pos_info = 0x0001;
    assert(CheckBall(s, v, 4, 1) == Refusal::NotFirst);
    s.pos_info = 0;
    for (const std::uint32_t unsupported : {1u, 1u << 8, 1u << 16}) {
        s.battle_kind = BattleKindAllowed | unsupported; // trainer, non-single rule, communication
        assert(CheckBall(s, v, 4, 1) == Refusal::BattleType);
    }
    s.battle_kind = BattleKindAllowed;
    s.offline_single = false; s.offline_raid = true;
    assert(CheckBall(s, v, 4, 1) == Refusal::BattleType);
    s.offline_raid = false;
    assert(CheckBall(s, v, 4, 1) == Refusal::BattleType); // unknown/unsupported battle
    s.offline_single = true;
    // Independent named item catalog cases from SwSh's itemname table. Cover ordinary,
    // gift/event and Apricorn balls; Park Ball500 is not a usable SwSh ball.
    struct BallCase { int id; const char* name; };
    constexpr BallCase known[]{
        {1,"Master"},{2,"Ultra"},{3,"Great"},{4,"Poke"},{5,"Safari"},{6,"Net"},{7,"Dive"},{8,"Nest"},
        {9,"Repeat"},{10,"Timer"},{11,"Luxury"},{12,"Premier"},{13,"Dusk"},{14,"Heal"},{15,"Quick"},{16,"Cherish"},
        {492,"Fast"},{493,"Level"},{494,"Lure"},{495,"Heavy"},{496,"Love"},{497,"Friend"},{498,"Moon"},{499,"Sport"},
        {576,"Dream"},{851,"Beast"}
    };
    static_assert(std::size(known) == 26);
    std::set<int> ids;
    for (const auto& ball : known) {
        assert(ball.name[0] && ids.insert(ball.id).second);
        assert(IsBattleBall(ball.id));
        assert(CheckBall(s, v, ball.id, 1) == Refusal::None);
    }
    for (const int other : {-1, 0, 17, 491, 500, 575, 577, 850, 852}) {
        assert(!IsBattleBall(other));
        assert(CheckBall(s, v, other, 1) == Refusal::NotBattleItem);
    }
    // The bridge carries native item identity851, never a visible row/page index.
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    assert(b.Initialize()); Ready(f, b, t);
    assert(CheckBall(b.status(), v, 851, 1) == Refusal::None);
    assert(b.Stage(Op::Item, 851, 0, t));
    assert(f.Get(OffsetReqOp) == 4 && f.Get(OffsetReqArg) == 851 && f.Get(OffsetReqExpect) == 0);
    assert(CheckBall(b.status(), v, 4, 1) == Refusal::Busy);
    assert(!b.Stage(Op::Item, 4, 0, t));
    f.GuestFrame(Result::BallRefused);
    b.Update(t += 33);
    Outcome out;
    assert(b.TakeOutcome(out) && out.op == Op::Item && out.code == Result::BallRefused);
    assert(b.status().menu_waiting); // rejected native throw did not close its command wait
    b.Update(t += b.BeatWindowMs() + 1);
    assert(CheckBall(b.status(), v, 851, 1) == Refusal::MenuNotWaiting);
    std::puts("ball guards: all26 named SwSh balls; wild/known-live-foe/quantity/wait gates and native ID851 bridge payload");
}

// ---- stub encoding ----
std::vector<std::uint8_t> Hex(const std::string& h) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}

// Phase 2: the battle-alive beat keeps a battle "alive" through turn animations (no menu beat),
// ends it once silent, carries the client object, and a short host pause does not end it.
void TestBattleAliveBeat() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    b.Initialize();
    f.Set(EpochOffset, 5);
    b.Update(t);
    assert(!b.status().battle_alive);
    for (int i = 0; i < 6; ++i) {
        AliveFrame(f);
        b.Update(t += 33);
    }
    assert(b.status().helper_present && b.status().battle_alive);
    assert(!b.status().menu_waiting); // turn animation: alive, but no command menu
    assert(b.status().alive_client == 0x21'9532'EE60ull);
    assert(b.AliveWindowMs() == 300); // 6 frames of ~33 ms, floored at 300 ms
    b.Update(t += 250);
    assert(b.status().battle_alive); // inside the window
    f.Set(EpochOffset, 6);           // host pause/resume: refreshed once
    b.Update(t += 250);
    assert(b.status().battle_alive);
    b.Update(t += 400); // silent past the window: the battle is over
    assert(!b.status().battle_alive && b.status().alive_client == 0);
    AliveFrame(f);
    b.Update(t += 33);
    assert(b.status().battle_alive); // next battle
    std::puts("battle alive beat ok");
}

// Phase 2: SWITCH and ITEM requests round-trip like FIGHT, carry their arguments, and a game
// refusal comes back as its result code.
void TestSwitchItemRoundTrip() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    std::int64_t t = 0;
    b.Initialize();
    Ready(f, b, t);
    assert(b.Stage(Op::Switch, 2, 0, t));
    assert(f.Get(OffsetReqOp) == 3 && f.Get(OffsetReqArg) == 2);
    f.GuestFrame(Result::Ok, 2);
    b.Update(t += 33);
    Outcome out;
    assert(b.TakeOutcome(out) && out.op == Op::Switch && out.code == Result::Ok && out.value == 2);
    assert(!b.status().menu_waiting); // committed: menu closed until the next beat
    f.GuestFrame();
    b.Update(t += 33);
    assert(b.Stage(Op::Item, 17, 2, t)); // Potion on pokeID 2
    assert(f.Get(OffsetReqOp) == 4 && f.Get(OffsetReqArg) == 17 && f.Get(OffsetReqExpect) == 2);
    f.GuestFrame(Result::NoEffect);
    b.Update(t += 33);
    assert(b.TakeOutcome(out) && out.op == Op::Item && out.code == Result::NoEffect);
    assert(std::string(ResultText(out.code)) == sw_str::S(sw_str::Id::kResNoEffect)); // the game's own line
    assert(b.status().menu_waiting); // a refusal leaves the menu up
    assert(b.Stage(Op::Switch, 0, 0, t));
    f.GuestFrame(Result::Trapped);
    b.Update(t += 33);
    assert(b.TakeOutcome(out) && out.code == Result::Trapped);
    std::puts("switch/item round trip ok");
}

void TestSwitchItemPreconditions() {
    Status s;
    BattleView v = Battle();
    v.active_pid = 0;
    v.members[0] = {true, false, 100, 200};
    v.members[1] = {true, true, 0, 300};    // fainted
    v.members[2] = {true, false, 250, 297};
    assert(CheckSwitch(s, v, 2) == Refusal::NoHelper);
    s.helper_present = true;
    assert(CheckSwitch(s, v, 2) == Refusal::MenuNotWaiting);
    s.menu_waiting = true;
    assert(CheckSwitch(s, v, 2) == Refusal::BattleType);
    assert(CheckItem(s, v, 17, 5, 2) == Refusal::BattleType);
    s.offline_single = true;
    assert(CheckSwitch(s, v, 2) == Refusal::None);
    assert(CheckSwitch(s, v, -1) == Refusal::NoSelection);
    assert(CheckSwitch(s, v, 0) == Refusal::ActiveMember);
    assert(CheckSwitch(s, v, 1) == Refusal::FaintedMember);
    assert(CheckSwitch(s, v, 3) == Refusal::NotMember);
    assert(CheckSwitch(s, v, 7) == Refusal::NotMember);
    assert(CheckSwitch(s, BattleView{}, 2) == Refusal::NotInBattle);
    s.busy = true;
    assert(CheckSwitch(s, v, 2) == Refusal::Busy);
    s.busy = false;
    assert(CheckItem(s, v, 17, 995, 0) == Refusal::None);   // Potion on the active battler
    assert(CheckItem(s, v, 17, 995, 2) == Refusal::None);   // ... on a bench member
    assert(CheckItem(s, v, 17, 0, 0) == Refusal::NoItem);   // none left
    assert(CheckItem(s, v, 0, 5, 0) == Refusal::NoItem);    // nothing picked
    assert(CheckItem(s, v, 1, 5, 0) == Refusal::NotBattleItem); // Master Ball is not offered
    assert(CheckItem(s, v, 17, 5, 4) == Refusal::NotMember);
    assert(CheckItem(s, v, 28, 5, 2) == Refusal::TargetNotFainted); // Revive on a healthy member
    assert(CheckItem(s, v, 28, 5, 1) == Refusal::None);
    assert(CheckItem(s, v, 4, 5, -1) == Refusal::None);     // Poke Ball: no target needed
    v.members[3] = {true, false, 299, 299};                  // full HP
    assert(CheckItem(s, v, 17, 5, 3) == Refusal::NoEffect);  // Potion on full HP
    assert(CheckItem(s, v, 17, 5, 1) == Refusal::NoEffect);  // Potion on a fainted member
    assert(CheckItem(s, v, 23, 5, 3) == Refusal::NoEffect);  // Full Restore, full HP, no status
    v.members[3].has_status = true;
    assert(CheckItem(s, v, 23, 5, 3) == Refusal::None);      // ... but it cures the status
    s.pos_info = 0x0001;
    assert(CheckItem(s, v, 4, 5, -1) == Refusal::NotFirst);
    s.pos_info = 0;
    s.menu_waiting = false;
    assert(CheckItem(s, v, 17, 5, 0) == Refusal::MenuNotWaiting);
    assert(std::string(RefusalText(Refusal::FaintedMember)) == "No energy left to battle");
    std::puts("switch/item preconditions ok");
}

void TestHudHide() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    assert(b.Initialize()); // module magic
    TestHud h{Load{&f}, Store{&f}};
    f.Set(OffsetHudReq, 1);
    assert(h.Initialize() && f.Get(OffsetHudReq) == 0); // nothing hidden until a tick asks
    FakeLayout l;
    std::int64_t t = 1000;

    // Companion shown, "Battle HP on top" off: hidden from the next guest frame on.
    h.Tick(true, t);
    assert(f.Get(OffsetHudReq) == 1 && f.Get(OffsetHudReqBeat) == 1);
    HudFrame(f, l);
    assert(l.flags[0] == 0x82 && l.flags[1] == 0x82 && l.flags[2] == 0x12 && l.flags[3] == 0x12);
    t += 16;
    h.Tick(true, t);
    assert(h.status().layout_live && h.status().hidden == 0xF && h.status().found == 0xF);
    assert(HudCount(h.status().hidden) == 4);
    // The game shows a plate again (not seen live; the in/out animations only touch alpha):
    // re-hidden on the next frame.
    l.flags[0] |= 1;
    HudFrame(f, l);
    assert(l.flags[0] == 0x82);

    // Setting on (or the companion hidden): restored on the next frame, exactly the bit it cleared.
    l.flags[1] |= 0x10; // the game's own dirty bit in between survives
    t += 16;
    h.Tick(false, t);
    HudFrame(f, l);
    assert(l.flags[0] == 0x83 && l.flags[1] == 0x93 && l.flags[2] == 0x13 && l.flags[3] == 0x13);
    assert(f.Get(OffsetHudState) == 0 && Get64(f, OffsetHudLayout) == 0);

    // A plate the game itself keeps hidden is never shown by the restore.
    l.flags[2] = 0x12;
    h.Tick(true, t += 16);
    HudFrame(f, l);
    assert((f.Get(OffsetHudState) & 0xF) == 0xB && l.flags[2] == 0x12);
    h.Tick(false, t += 16);
    HudFrame(f, l);
    assert(l.flags[2] == 0x12 && (l.flags[0] & 1) && (l.flags[1] & 1) && (l.flags[3] & 1));

    // Battle over while hidden; the next battle's layout (new panes, maybe at reused addresses):
    // the old record is forgotten, never written through.
    l.flags = {0x83, 0x83, 0x13, 0x13};
    h.Tick(true, t += 16);
    HudFrame(f, l);
    FakeLayout next;
    next.layout = 0x21'8936'87C8ull;
    next.pane[2] = next.pane[3] = 0; // a layout without the doubles plates
    h.Tick(false, t += 16);
    HudFrame(f, next); // restore against another layout: nothing touched
    assert(next.flags[0] == 0x83 && f.Get(OffsetHudState) == 0);
    h.Tick(true, t += 16);
    HudFrame(f, next);
    assert(next.flags[0] == 0x82 && next.flags[1] == 0x82 && (f.Get(OffsetHudState) & 0xF) == 0x3);
    assert(((f.Get(OffsetHudState) >> 8) & 0xF) == 0x3);

    // The module stops (no more ticks, no Release): the guest gives the plates back by itself.
    for (std::uint32_t i = 0; i < HudStaleFrames; ++i) HudFrame(f, next);
    assert(next.flags[0] == 0x82); // still within the window
    HudFrame(f, next);
    assert(next.flags[0] == 0x83 && next.flags[1] == 0x83);
    // ... and hides again as soon as the module beats again.
    h.Tick(true, t += 16);
    HudFrame(f, next);
    assert(next.flags[0] == 0x82);

    // Teardown asks for them back.
    h.Release();
    assert(f.Get(OffsetHudReq) == 0);
    HudFrame(f, next);
    assert(next.flags[0] == 0x83);

    // The HUD layout stopped drawing (battle over): not live after a second, nothing reported hidden.
    h.Tick(true, t += 16);
    assert(h.status().layout_live);
    h.Tick(true, t += 2000);
    assert(!h.status().layout_live && h.status().hidden == 0 && h.status().requested);
    // hud_req is written on a change only; the heartbeat every tick.
    h.Tick(true, t += 16);
    f.Set(OffsetHudReq, 0);
    const std::uint32_t beat = f.Get(OffsetHudReqBeat);
    h.Tick(true, t += 16);
    assert(f.Get(OffsetHudReq) == 0 && f.Get(OffsetHudReqBeat) == beat + 1);
    h.Tick(false, t += 16);
    h.Tick(true, t += 16);
    assert(f.Get(OffsetHudReq) == 1);

    // Native HP hides only after an explicit setting0 and shown companion. Older hosts,
    // unknown flags and hidden companions always return the native plates.
    assert(HudWant(0, true) && !HudWant(-1, true) && !HudWant(1, true));
    assert(!HudWant(2, true) && !HudWant(-42, true));
    assert(!HudWant(0, false) && !HudWant(-1, false) && !HudWant(1, false));
    h.Tick(HudWant(0, true), t += 16); HudFrame(f, next);
    assert((next.flags[0] & 1) == 0);
    h.Tick(HudWant(-1, true), t += 16); HudFrame(f, next);
    assert((next.flags[0] & 1) != 0 && f.Get(OffsetHudReq) == 0);
    h.Tick(HudWant(0, true), t += 16); HudFrame(f, next);
    assert((next.flags[0] & 1) == 0);
    h.Tick(HudWant(1, true), t += 16); HudFrame(f, next);
    assert((next.flags[0] & 1) != 0 && f.Get(OffsetHudReq) == 0);
    std::puts("hud hide ok");
}

void TestHudBattleLifetime() {
    Fake f;
    TestBridge b{Load{&f}, Store{&f}};
    assert(b.Initialize());
    TestHud h{Load{&f}, Store{&f}};
    assert(h.Initialize());
    Set64(f, OffsetAliveClient, 0x21110000);
    f.Set(OffsetBattleSerial, 10);
    FakeLayout l;
    h.Tick(true, 1000);
    HudFrame(f, l);
    assert((f.Get(OffsetHudState) & 0xF) == 0xF);
    assert(f.Get(OffsetHudSeenBattleSerial) == 10);

    // A paused/slow battle can lose its alive beat. Silence is not a new lifetime: keep the
    // pane ownership so a settings change/companion hide can still restore it after resuming.
    f.Set(OffsetFieldBeat, 200); // field process can update during battle; this does not expire ownership
    h.Tick(false, 10000);
    HudFrame(f, l);
    assert(l.flags[0] == 0x83 && l.flags[2] == 0x13);
    h.Tick(true, 10016);
    HudFrame(f, l);

    // The old layout stops drawing before it sees the request to restore. The next battle uses
    // identical layout/pane/client addresses, but the game starts the doubles panes hidden.
    f.Set(OffsetBattleSerial, 20); // native initialization ran for the next battle
    l.flags = {0x83, 0x83, 0x12, 0x12};
    h.Tick(false, 10100);
    HudFrame(f, l);
    assert(l.flags[2] == 0x12 && l.flags[3] == 0x12); // no stale ownership restoration
    h.Tick(true, 10200);
    HudFrame(f, l);
    assert((f.Get(OffsetHudState) & 0xF) == 3);
    h.Tick(false, 10300);
    HudFrame(f, l);
    assert(l.flags[0] == 0x83 && l.flags[1] == 0x83);
    assert(l.flags[2] == 0x12 && l.flags[3] == 0x12);

    // Also invalidate for a replacement player client even without an intervening field phase.
    h.Tick(true, 10400);
    HudFrame(f, l);
    Set64(f, OffsetAliveClient, 0x21220000);
    l.flags = {0x82, 0x82, 0x12, 0x12}; // naturally hidden in the replacement battle
    h.Tick(false, 10500);
    HudFrame(f, l);
    assert(l.flags[0] == 0x82 && l.flags[1] == 0x82);
    assert(Get64(f, OffsetHudSeenClient) == 0x21220000);
    std::puts("hud battle lifetime ok");
}

void TestProgressionLifecycle() {
    Fake f;
    f.Set(EpochOffset, 7);
    TestField field{Load{&f}, Store{&f}};
    assert(field.Initialize());
    std::int64_t t = 1000;
    for (int i = 0; i < 4; ++i) { FieldFrame(f); field.Update(t += 33); }
    const auto seq0 = f.Get(OffsetFieldReqSeq);
    assert(!field.Stage(FieldOp::Progression, 17, 0, t)); // not a progression item
    assert(!field.Stage(FieldOp::Progression, 50, 6, t)); // invalid target
    assert(!field.Stage(FieldOp::Progression, 50, 0, t, 0, 0, 123, 1)); // no move arg
    assert(f.Get(OffsetFieldReqSeq) == seq0);
    assert(field.Stage(FieldOp::Progression, 50, 0, t, 0, 0, 123));
    const auto seq = f.Get(OffsetFieldReqSeq);
    assert(f.Get(OffsetFieldExpectEc) == 123 && f.Get(OffsetFieldMove) == 0);
    f.Set(OffsetFieldProgressionSeq, seq);
    // A genuine native effect can wait for input for minutes, exceeding dispatch windows.
    for (int i = 0; i < 100; ++i) {
        f.Set(OffsetFieldBeat, f.Get(OffsetFieldBeat) + 1);
        field.Update(t += 1000);
        assert(field.status().busy && !field.status().player_free);
    }
    assert(f.Get(OffsetFieldCancelSeq) != seq);
    f.Set(EpochOffset, 8);
    field.Update(t += 33);
    assert(f.Get(OffsetFieldCancelSeq) == seq); // notifies the guest; committed native flow still finishes
    assert(field.status().busy);
    // Cancellation is not native completion: the evolution task can still own the target,
    // then pop while its native screen-fade cleanup is pending. Active seq covers both.
    FieldOutcome out;
    for (const auto phase : {2u, 3u}) {
        f.Set(0x500, phase); // guest-private phase; the host protocol deliberately uses active seq
        f.Set(OffsetFieldBeat, f.Get(OffsetFieldBeat) + 1);
        field.Update(t += 1000);
        assert(field.status().busy && !field.TakeOutcome(out));
        assert(!field.Stage(FieldOp::Progression, 50, 0, t, 0, 0, 123));
        assert(f.Get(OffsetFieldReqSeq) == seq); // no resubmission or second consumption request
        assert(f.Get(OffsetFieldCancelSeq) == seq);
    }
    // A committed item completes its whole native flow despite the host epoch/cancel request.
    // Only task completion plus fade completion acknowledges success, never a partial cancel.
    f.Set(OffsetFieldAckSeq, seq);
    f.Set(OffsetFieldResultCode, static_cast<unsigned>(Result::Ok));
    f.Set(OffsetFieldProgressionSeq, 0);
    field.Update(t += 33);
    assert(field.TakeOutcome(out) && out.answered && out.code == Result::Ok);
    assert(!field.status().busy);
    // A surviving old native effect blocks a new bridge from reusing its buffers on reload.
    f.Set(OffsetFieldProgressionSeq, seq);
    field.Update(t += 33);
    assert(!field.Stage(FieldOp::Progression, 83, 0, t, 0, 0, 456));
    std::uint64_t pair[1][2]{{1, 2}};
    assert(!field.StageQuery(pair, 1, t));
    std::puts("progression staging/lifecycle/epoch/reload guards ok");
}

void TestFieldReloadSequences() {
    Fake f;
    f.Set(EpochOffset, 7);
    f.Set(OffsetFieldAckSeq, 40);
    f.Set(OffsetFieldReqSeq, 41);
    f.Set(OffsetFieldProgressionSeq, 41); // old module's committed native effect
    TestField reloaded{Load{&f}, Store{&f}};
    assert(reloaded.Initialize());
    assert(f.Get(OffsetFieldReqSeq) == 41); // do not withdraw borrowed native state by rewriting seq
    std::int64_t t = 1000;
    for (int i = 0; i < 3; ++i) {
        f.Set(OffsetGuestMagic, GuestMagic);
        f.Set(OffsetFieldBeat, f.Get(OffsetFieldBeat) + 1);
        reloaded.Update(t += 33);
    }
    assert(reloaded.status().busy);
    assert(!reloaded.Stage(FieldOp::Progression, 50, 0, t, 0, 0, 123));
    // The old task finishes while the new bridge owns no pending operation.
    f.Set(OffsetFieldProgressionSeq, 0);
    f.Set(OffsetFieldAckSeq, 41);
    f.Set(OffsetFieldResultCode, 0);
    f.Set(OffsetFieldFree, 1);
    f.Set(OffsetFieldBeat, f.Get(OffsetFieldBeat) + 1);
    reloaded.Update(t += 33);
    FieldOutcome out;
    assert(!reloaded.TakeOutcome(out));
    assert(reloaded.Stage(FieldOp::Progression, 50, 0, t, 0, 0, 123));
    assert(f.Get(OffsetFieldReqSeq) == 42); // cannot mistake ACK41 for this new operation
    reloaded.Update(t += 33);
    assert(reloaded.status().busy && !reloaded.TakeOutcome(out));

    // A withdrawn request that has not dispatched remains cancelled through reinitialization.
    Fake waiting;
    waiting.Set(OffsetFieldAckSeq, 70);
    waiting.Set(OffsetFieldReqSeq, 71);
    waiting.Set(OffsetFieldCancelSeq, 71);
    TestField reload_waiting{Load{&waiting}, Store{&waiting}};
    assert(reload_waiting.Initialize());
    assert(waiting.Get(OffsetFieldReqSeq) == 71 && waiting.Get(OffsetFieldCancelSeq) == 71);
    waiting.Set(OffsetFieldCancelSeq, 0);
    TestField withdraw_unanswered{Load{&waiting}, Store{&waiting}};
    assert(withdraw_unanswered.Initialize());
    assert(waiting.Get(OffsetFieldCancelSeq) == 71); // withdraw even without prior Shutdown
    waiting.Set(EpochOffset, 2);
    for (int i = 0; i < 3; ++i) { FieldFrame(waiting); reload_waiting.Update(t += 33); }
    assert(reload_waiting.Stage(FieldOp::Progression, 50, 0, t, 0, 0, 123));
    assert(waiting.Get(OffsetFieldReqSeq) == 72);
    std::puts("field reload preserves cancellation and never reuses active sequence ok");
}

void SeedPreview(Fake& f) {
    f.Set(EpochOffset, 7);
    f.Set(OffsetGuestMagic, GuestMagic);
    f.Set(OffsetBattleKind, 2);
    f.Set(OffsetBattleSerial, 42);
    f.Set(OffsetBattleKindSerial, 42);
    Set64(f, OffsetAliveClient, 0x2111223300ull);
    Set64(f, OffsetClient, 0x2111223300ull);
    Set64(f, OffsetDmaxClient, 0x2111223300ull);
    Set64(f, OffsetMon, 0x2144556600ull);
    Set64(f, OffsetDmaxMon, 0x2144556600ull);
    f.Set(OffsetDmaxOpportunity, 1 | (4u << 8) | (4u << 16));
    f.Set(OffsetRaidTargetMask, 0x33); // normal slots0/1 and max slots0/1
    const std::array<std::uint32_t, 4> ids{757, 760, 0, 826}, base{85, 98, 0, 826}, power{140, 100, 0, 250};
    for (std::uint32_t i = 0; i < 4; ++i) {
        f.Set(OffsetDmaxMoveIds + i * 4, ids[i]);
        f.Set(OffsetDmaxBaseIds + i * 4, base[i]);
        f.Set(OffsetDmaxPowers + i * 4, power[i]);
        f.Set(OffsetDmaxGmaxIndices + i * 4, i == 3 ? 32u : ~0u);
    }
}

template<class B> void PreviewFrames(Fake& f, B& b, std::int64_t& t) {
    for (int i = 0; i < 3; ++i) {
        f.Set(OffsetWaitBeat, f.Get(OffsetWaitBeat) + 1);
        f.Set(OffsetAliveBeat, f.Get(OffsetAliveBeat) + 1);
        f.Set(OffsetDmaxPreviewBeat, f.Get(OffsetWaitBeat));
        b.Update(t += 33);
    }
}

struct PreviewFaultLoad {
    Fake* f;
    std::uint32_t fail_at = ~0u, tear_at = ~0u;
    mutable unsigned reads = 0;
    bool operator()(std::uint32_t off, std::uint32_t* value) const {
        if (off == fail_at) return false;
        if (!Load{f}(off, value)) return false;
        if (off == tear_at && (++reads % 2) == 0) *value ^= 1;
        return true;
    }
};

void TestNativePreviewPipeline() {
    Fake f;
    SeedPreview(f);
    TestBridge b{Load{&f}, Store{&f}};
    assert(b.Initialize());
    std::int64_t t = 1000;
    PreviewFrames(f, b, t);
    assert(b.status().kind_current && b.status().preview.valid && b.status().offline_raid);
    assert(b.status().preview.eligible && !b.status().preview.active);
    assert(b.status().preview.remaining == 4 && b.status().preview.total == 4);
    assert(b.status().preview.ids[0] == 757 && b.status().preview.base_ids[0] == 85);
    assert(b.status().preview.powers[3] == 250 && b.status().preview.gmax[3] == 32);
    assert(b.status().preview.gmax[0] == -1);
    f.Set(OffsetDmaxPreviewBeat, f.Get(OffsetWaitBeat) - 1);
    b.Update(t += 1);
    assert(!b.status().preview.valid && !b.status().offline_raid);
    assert(b.status().preview.ids[0] == 0); // cannot retain stale display data on failure
    f.Set(OffsetDmaxPreviewBeat, f.Get(OffsetWaitBeat));
    Set64(f, OffsetDmaxClient, 0x2177770000ull);
    b.Update(t += 1);
    assert(!b.status().preview.valid);
    Set64(f, OffsetDmaxClient, 0x2111223300ull);
    Set64(f, OffsetMon, 0x2177770000ull);
    b.Update(t += 1);
    assert(!b.status().preview.valid); // same released beat, different controlled Pokemon
    Set64(f, OffsetMon, 0x2144556600ull);
    f.Set(OffsetBattleSerial, 43);
    b.Update(t += 1);
    assert(!b.status().kind_current && !b.status().preview.valid);
    f.Set(OffsetBattleSerial, 42);
    b.Update(t += 5000);
    assert(!b.status().preview.valid && !b.status().offline_raid); // silent menu is stale

    const std::array<std::pair<std::uint32_t, std::uint32_t>, 9> invalid{{
        {OffsetDmaxOpportunity, 2 | (4u << 8) | (4u << 16)},
        {OffsetDmaxOpportunity, 1 | (4u << 8) | (3u << 16)},
        {OffsetDmaxOpportunity, 1 | (5u << 16)},
        {OffsetDmaxOpportunity, 1 | (2u << 24)},
        {OffsetDmaxMoveIds, 827}, {OffsetDmaxBaseIds, 827},
        {OffsetDmaxPowers, 251}, {OffsetDmaxGmaxIndices, static_cast<std::uint32_t>(-2)},
        {OffsetDmaxGmaxIndices, 33},
    }};
    for (const auto& [offset, value] : invalid) {
        Fake bad;
        SeedPreview(bad);
        bad.Set(offset, value);
        TestBridge bridge{Load{&bad}, Store{&bad}};
        assert(bridge.Initialize());
        PreviewFrames(bad, bridge, t);
        assert(!bridge.status().preview.valid && !bridge.status().offline_raid);
    }
    // Partial reads, a concurrently republished beat, and a torn pointer all fail closed.
    for (int which = 0; which < 3; ++which) {
        Fake torn;
        SeedPreview(torn);
        PreviewFaultLoad load{&torn};
        if (which == 0) load.fail_at = OffsetDmaxPowers + 12;
        if (which == 1) load.tear_at = OffsetDmaxPreviewBeat;
        if (which == 2) load.tear_at = OffsetDmaxMon;
        Bridge<PreviewFaultLoad, Store> bridge{load, Store{&torn}};
        assert(bridge.Initialize());
        PreviewFrames(torn, bridge, t);
        assert(!bridge.status().preview.valid && !bridge.status().offline_raid);
    }
    std::puts("native preview freshness/identity/bounds/partial-read guards ok");
}

void TestHudBattleModes() {
    Fake f;
    TestHud hud{Load{&f}, Store{&f}};
    assert(hud.Initialize());
    hud.Tick(true, 1000);
    assert(f.Get(OffsetHudReq) == 1);
    hud.Tick(true, 1033, true);
    assert(f.Get(OffsetHudReq) == 2); // same want=true still changes layout mode
    hud.Tick(false, 1066, true);
    assert(f.Get(OffsetHudReq) == 0);
    assert(HudWantBattle(0, true, true, true, false));
    assert(HudWantBattle(0, true, true, false, true));
    assert(!HudWantBattle(0, true, true, false, false)); // unsupported rule1 must restore
    assert(!HudWantBattle(0, true, false, true, true)); // stale battle generation
    assert(!HudWantBattle(1, true, true, true, true));
    assert(!HudWantBattle(-1, true, true, true, true)); // unknown setting restores even a verified format
    assert(!HudWantBattle(0, false, true, true, true));
    hud.Tick(true, 1100, true);
    hud.Release();
    assert(f.Get(OffsetHudReq) == 0);
    std::puts("HUD single/raid/restore modes and unsupported-battle visibility ok");
}

void TestRaidTargetsAndMaxPayload() {
    Status::Preview preview;
    preview.valid = true;
    preview.base_ids = {85, 98, 0, 0};
    preview.target_mask = 0x11; // only slot0 has the supported boss target, normal and Max
    auto view = Battle();
    assert(RaidTargetMatches(preview, view, 0, false));
    assert(RaidTargetMatches(preview, view, 0, true));
    assert(!RaidTargetMatches(preview, view, 1, false));
    assert(!RaidTargetMatches(preview, view, 1, true));
    assert(!RaidTargetMatches(preview, view, -1, false));
    assert(!RaidTargetMatches(preview, view, 4, true));
    preview.target_mask = 0x01;
    assert(RaidTargetMatches(preview, view, 0, false));
    assert(!RaidTargetMatches(preview, view, 0, true)); // masks cannot leak between move modes
    preview.target_mask = 0x10;
    assert(!RaidTargetMatches(preview, view, 0, false));
    assert(RaidTargetMatches(preview, view, 0, true));
    view.move_id[0] = 86;
    assert(!RaidTargetMatches(preview, view, 0, true)); // new base move, old native preview
    view.move_id[0] = preview.base_ids[0] = 0;
    assert(!RaidTargetMatches(preview, view, 0, true));
    preview.valid = false;
    assert(!RaidTargetMatches(preview, Battle(), 0, false));

    Fake f;
    SeedPreview(f);
    TestBridge bridge{Load{&f}, Store{&f}};
    assert(bridge.Initialize());
    std::int64_t t = 1000;
    PreviewFrames(f, bridge, t);
    auto status = bridge.status();
    status.offline_single = true;
    status.offline_raid = false;
    assert(CheckFight(status, Battle(), 0) == Refusal::None);
    assert(CheckFight(status, Battle(), 0, true) == Refusal::BattleType); // Single Max unverified
    status = bridge.status();
    if (RaidDirectVerified) {
        assert(CheckFight(status, Battle(), 0) == Refusal::None);
        status.preview.target_mask = 0;
        assert(CheckFight(status, Battle(), 0) == Refusal::BattleType);
        status = bridge.status();
        status.preview.base_ids[0] = 86;
        assert(CheckFight(status, Battle(), 0) == Refusal::BattleType);
        assert(CheckRun(status, Battle()) == Refusal::BattleType);
        assert(CheckSwitch(status, Battle(), 1) == Refusal::BattleType);
        assert(CheckItem(status, Battle(), 17, 1, 0) == Refusal::BattleType);
    }
    if (DynamaxDirectVerified) {
        status = bridge.status();
        assert(CheckFight(status, Battle(), 0, true) == Refusal::None);
        status.preview.eligible = false;
        assert(CheckFight(status, Battle(), 0, true) == Refusal::BattleType);
        status.preview.active = true;
        assert(CheckFight(status, Battle(), 0, true) == Refusal::None);
    }
    // The command carries the base move as its stale-target guard, never the displayed Max ID.
    assert(bridge.Stage(Op::MaxFight, 0, 85, t));
    assert(f.Get(OffsetReqOp) == 5 && f.Get(OffsetReqArg) == 0);
    assert(f.Get(OffsetReqExpect) == 85 && f.Get(OffsetReqExpect) != bridge.status().preview.ids[0]);
    assert(!bridge.Stage(Op::MaxFight, 0, 85, t));
    f.Set(OffsetResultCode, 0);
    f.Set(OffsetResultValue, 85);
    f.Set(OffsetAckSeq, f.Get(OffsetReqSeq));
    bridge.Update(t += 33);
    Outcome result;
    assert(bridge.TakeOutcome(result) && result.op == Op::MaxFight && result.code == Result::Ok);
    assert(!bridge.status().menu_waiting && !bridge.status().preview.valid);
    std::puts("raid target masks/base-move identity and Max command payload ok");
}

// Package contract (dev.17): the move page's Back is the top's own B press only while the native
// move list waits (otherwise the companion-only page change), and every party drop is gated.
void TestManifestDirectContracts() {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
        "../../packages/PokemonSword/dualscreen/manifest.json";
    std::ifstream in(path);
    if (!in) {
        std::printf("manifest contracts: %s not found, skipped\n", path.string().c_str());
        return;
    }
    const auto m = nlohmann::json::parse(in);
    const auto& back = m.at("actions").at("battle_move_back_native");
    assert(back.at("kind") == "button" && back.at("button") == "B" && back.at("enabled_bind") == "d.mv.native_wait");
    bool derived = false;
    for (const auto& d : m.at("derived"))
        if (d.at("name") == "d.mv.native_wait")
            derived = d.at("expr") == "sw.state.battle && sw.battle.native.menu == 2";
    assert(derived);
    for (int i = 0; i < 6; ++i)
        assert(m.at("actions").at("party_swap" + std::to_string(i)).at("enabled_bind") == "sw.party.swap.ready");
    int native = 0, companion = 0;
    for (const auto& page : m.at("pages")) {
        const bool moves = page.at("id") == "battle_moves" || page.at("id") == "raid_moves";
        for (const auto& w : page.at("widgets")) {
            if (!w.contains("on_tap")) continue;
            if (w.at("on_tap") == "battle_move_back_native") {
                assert(moves && w.at("need_bind") == "d.mv.native_wait");
                ++native;
            } else if (moves && w.at("on_tap") == "battle_command_open") {
                assert(w.at("need_bind") == "!d.mv.native_wait");
                ++companion;
            }
        }
    }
    assert(native == 2 && companion == 2);
    std::puts("manifest direct contracts ok");
}

void TestStubEncoding() {
    const auto plan_path = std::filesystem::path(__FILE__).parent_path() /
        "../../packages/PokemonSword/dualscreen/guest/A3B75BCD3311385AEED67FBEEB79CBB7BF02F471000000000000000000000000/load-plan.json";
    const char* candidate = std::getenv("SW_HELPER_TEST_PLAN");
    std::ifstream in(candidate ? std::filesystem::path(candidate) : plan_path);
    if (!in) {
        std::printf("stub encoding: %s not found, skipped\n", plan_path.string().c_str());
        return;
    }
    const auto plan = nlohmann::json::parse(in);
    assert(plan.at("format") == 2 && plan.at("module") == "main" && plan.at("title_id") == "0100ABF008968000");
    assert(plan.at("mailbox_size") == MailboxSize && plan.at("mailbox_epoch_offset") == EpochOffset);
    const auto& patch = plan.at("patches").at(0);
    assert(patch.at("offset") == 0x802DF0 && patch.at("expected") == "f657bda9");
    assert(patch.at("relocations").at(0).at("target") == "code");
    // Phase 2: the battle-alive hook on the client's per-frame update.
    const auto& alive = plan.at("patches").at(1);
    assert(alive.at("offset") == 0x7FCE10 && alive.at("expected") == "fc0f1bf8");
    const auto alive_entry = alive.at("relocations").at(0).at("addend").get<std::int64_t>();
    assert(alive.at("relocations").at(0).at("target") == "code" && alive_entry > 0 && alive_entry % 4 == 0);
    // HUD hide: the layout work's per-frame draw step.
    const auto& hud = plan.at("patches").at(2);
    assert(hud.at("offset") == 0x685A00 && hud.at("expected") == "ff8302d1");
    const auto hud_entry = hud.at("relocations").at(0).at("addend").get<std::int64_t>();
    assert(hud.at("relocations").at(0).at("target") == "code" && hud_entry > alive_entry && hud_entry % 4 == 0);
    // Field actions: the field process's per-frame update.
    const auto& fieldp = plan.at("patches").at(3);
    assert(fieldp.at("offset") == 0xDBD850 && fieldp.at("expected") == "ffc303d1");
    const auto field_entry = fieldp.at("relocations").at(0).at("addend").get<std::int64_t>();
    assert(fieldp.at("relocations").at(0).at("target") == "code" && field_entry > hud_entry && field_entry % 4 == 0);
    const auto& begin = plan.at("patches").at(4);
    assert(begin.at("offset") == 0x7FC398 && begin.at("expected") == "607200f9");
    const auto begin_entry = begin.at("relocations").at(0).at("addend").get<std::int64_t>();
    assert(begin.at("relocations").at(0).at("target") == "code" && begin_entry > field_entry && begin_entry % 4 == 0);
    const bool portrait = plan.at("patches").size() == 8;
    assert(plan.at("patches").size() == 5 || portrait);
    if (portrait) {
        const auto& p = plan.at("patches").at(5);
        assert(p.at("offset") == 0xF1CD30 && p.at("expected") == "f85cdd97");
        assert(p.at("relocations").at(0).at("target") == "code");
        assert(p.at("relocations").at(0).at("addend").get<std::int64_t>() > begin_entry);
        assert(plan.at("patches").at(6).at("offset") == 0xADFF50);
        assert(plan.at("patches").at(6).at("expected") == "ff0301d1");
        assert(plan.at("patches").at(7).at("offset") == 0x10F28A0);
        assert(plan.at("patches").at(7).at("expected") == "fc6fbaa9");
    }

    const auto code = Hex(plan.at("guest_code").at("bytes").get<std::string>());
    assert(!code.empty() && code.size() % 4 == 0);
    std::vector<std::uint32_t> words(code.size() / 4);
    std::memcpy(words.data(), code.data(), code.size());
    // First instruction reproduces the overwritten one.
    assert(words[0] == 0xA9BD57F6u);
    assert(words[static_cast<std::size_t>(alive_entry) / 4] == 0xF81B0FFCu); // str x28, [sp, #-0x50]!
    assert(words[static_cast<std::size_t>(hud_entry) / 4] == 0xD10283FFu);   // sub sp, sp, #0xa0
    assert(words[static_cast<std::size_t>(field_entry) / 4] == 0xD103C3FFu); // sub sp, sp, #0xf0
    assert(words[static_cast<std::size_t>(begin_entry) / 4] == 0xF9007260u); // str x0, [x19, #0xE0]
    // The plate names the HUD hook looks up, 8 bytes each, then N_inout.
    const std::string names(reinterpret_cast<const char*>(code.data()), code.size());
    assert(names.find(std::string("L_near1\0L_far1\0\0L_near2\0L_far2\0\0N_inout\0", 40)) != std::string::npos);
    std::set<std::uint32_t> used;
    bool guest_magic = false, module_magic_lo = false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint32_t w = words[i];
        assert((w & 0xFF000000u) != 0xD4000000u);                          // no SVC/BRK/HLT
        assert((w & 0xFF000000u) != 0xD5000000u || w == 0xD503201Fu);      // no MRS/MSR/barriers
        const std::uint32_t rn = (w >> 5) & 31;
        if (rn == 20 || (rn == 16 && i * 4 >= static_cast<std::size_t>(alive_entry))) {
            if ((w & 0xFFC00000u) == 0xB9000000u || (w & 0xFFC00000u) == 0xB9400000u)
                used.insert(((w >> 10) & 0xFFF) * 4); // str/ldr w, [x20, #imm]
            if ((w & 0xFFC00000u) == 0xF9000000u || (w & 0xFFC00000u) == 0xF9400000u)
                used.insert(((w >> 10) & 0xFFF) * 8); // str/ldr x, [x20, #imm]
            if ((w & 0xFFC00000u) == 0x91000000u)
                used.insert((w >> 10) & 0xFFF);       // add xN, x20, #imm (ldar/stlr address)
        }
        if (w == 0x528AEA69u && i + 1 < words.size() && words[i + 1] == 0x72A628E9u) guest_magic = true;
        if ((w & 0xFFFFFFE0u) == 0x528AEA60u) module_magic_lo = true; // movz wN, #0x5753
    }
    const std::set<std::uint32_t> declared = {OffsetModuleMagic, OffsetGuestMagic, OffsetWaitBeat, OffsetPosInfo,
                                              OffsetClient, OffsetMon, OffsetReqSeq, OffsetReqOp, OffsetReqArg,
                                              OffsetReqExpect, OffsetReqEpoch, OffsetCancelSeq, OffsetBattleKind,
                                              OffsetAckSeq,
                                              OffsetResultCode, OffsetResultValue, OffsetAckBeat, EpochOffset,
                                              OffsetAliveBeat, OffsetAliveClient, OffsetHudReq, OffsetHudReqBeat,
                                              OffsetHudFrame, OffsetHudState, OffsetHudSeenBeat, OffsetHudStale,
                                              OffsetHudLayout, OffsetHudPane, OffsetHudSeenBattleSerial, OffsetBattleSerial, OffsetHudSeenClient, OffsetFieldBeat, OffsetFieldFree,
                                              OffsetFieldReqSeq, OffsetFieldReqOp, OffsetFieldReqArg,
                                              OffsetFieldReqExpect, OffsetFieldReqEpoch, OffsetFieldCancelSeq,
                                              OffsetFieldZone, OffsetFieldLanding, OffsetFieldExpectEc, OffsetFieldMove,
                                              OffsetFieldAckSeq,
                                              OffsetFieldResultCode, OffsetFieldResultValue, OffsetFieldAckBeat,
                                              OffsetFlyQueryCount, OffsetFlyQueryMask, OffsetFlyQueryState, OffsetBikeUsable,
                                              OffsetFlyQueryPairs, OffsetCursorReqSeq, OffsetCursorReqIndex,
                                              OffsetCursorReqEpoch, OffsetCursorReqList, OffsetCursorReqView,
                                              OffsetCursorReqClient, OffsetCursorAckSeq, OffsetCursorResult,
                                              OffsetCursorReqKind, OffsetMenuReq, OffsetMenuReqBeat,
                                              OffsetMenuSeenBeat, OffsetMenuStale, OffsetMenuCursors};
    for (const auto o : used) {
        const bool portrait_private = portrait && o >= OffsetPortraitReqSeq && o <= OffsetPortraitCompleteSeq && o % 4 == 0;
        const bool progression_private = o >= 0x500 && o < 0x6B0;
        const bool extension = o == OffsetFieldProgressionSeq || o == OffsetMapQueryZone || o == OffsetMapQueryParent;
        const bool preview = o >= OffsetDmaxOpportunity && o <= OffsetBattleKindSerial && o % 4 == 0;
        const bool preview_base = o >= OffsetDmaxBaseIds && o <= OffsetHudSeenMode && o % 4 == 0;
        // Menu-hide records: {pane, layout, serial, hidden} x MenuSlots, guest-owned.
        const bool menu_record = o >= OffsetMenuRecords && o < OffsetMenuRecords + MenuSlots * MenuRecordBytes;
        if (!declared.count(o) && !progression_private && !extension && !preview && !preview_base && !portrait_private &&
            !menu_record)
            std::printf("undeclared mailbox offset %#x\n", o);
        assert(declared.count(o) || progression_private || extension || preview || preview_base || portrait_private ||
               menu_record);
    }
    assert(used.count(OffsetReqSeq) && used.count(OffsetAckSeq) && used.count(OffsetCancelSeq) && used.count(EpochOffset));
    assert(used.count(OffsetAliveBeat) && used.count(OffsetAliveClient));
    assert(used.count(OffsetBattleKind) && used.count(OffsetFieldFree)); // review H2/M4, H1
    assert(used.count(OffsetHudReq) && used.count(OffsetHudReqBeat) && used.count(OffsetHudFrame) &&
           used.count(OffsetHudState) && used.count(OffsetHudSeenBeat) && used.count(OffsetHudStale) &&
           used.count(OffsetHudLayout) && used.count(OffsetHudPane) &&
           used.count(OffsetHudSeenBattleSerial) && used.count(OffsetBattleSerial) && used.count(OffsetHudSeenClient));
    assert(used.count(OffsetFieldBeat) && used.count(OffsetFieldReqSeq) && used.count(OffsetFieldAckSeq) &&
           used.count(OffsetFieldCancelSeq) && used.count(OffsetFieldZone) && used.count(OffsetFieldLanding) &&
           used.count(OffsetFieldExpectEc) &&
           used.count(OffsetFlyQueryMask) && used.count(OffsetFlyQueryState) && used.count(OffsetFlyQueryPairs));
    assert(guest_magic && module_magic_lo);
    // Battle-menu hide (2026-11-01): request + heartbeat + cursor list + records, and the cursor
    // channel's list kind.
    assert(used.count(OffsetMenuReq) && used.count(OffsetMenuReqBeat) && used.count(OffsetMenuSeenBeat) &&
           used.count(OffsetMenuStale) && used.count(OffsetMenuCursors) && used.count(OffsetMenuRecords));
    assert(names.find(std::string("battle_commandSelect_00\0", 24)) != std::string::npos);
    assert(names.find(std::string("battle_commandSelect_01\0", 24)) != std::string::npos);
    assert(names.find(std::string("battle_skillSelect_00\0", 22)) != std::string::npos);
    assert(names.find(std::string("battle_ballselect_00\0", 21)) != std::string::npos);
    assert(names.find(std::string("battle_target_select_00\0", 24)) != std::string::npos);
    assert(names.find(std::string("cursor\0", 7)) != std::string::npos);
    // The field medicine set the guest tests (review M3) is kFieldMedicineMask:
    // movz x9, #0 / movk x9, #0xC782, lsl #16 / movk x9, #0x3, lsl #32.
    bool med_mask = false;
    for (std::size_t i = 0; i + 3 < words.size(); ++i)
        med_mask = med_mask || (words[i] == 0xD2800009u && words[i + 1] == 0xF2BFFFC9u && words[i + 2] == 0xF2C07869u && words[i + 3] == 0xF2E00509u);
    assert(med_mask && ((0xFFFEull << 16) | (0x3C3ull << 32) | (0x28ull << 48)) == kFieldMedicineMask);
    // Relocations: only mailbox/main targets, branches only to the native functions declared by
    // the plan builder.
    std::set<std::int64_t> branch_targets, data_targets;
    for (const auto& r : plan.at("guest_code").at("relocations")) {
        const auto target = r.at("target").get<std::string>();
        assert(target == "mailbox" || target == "main");
        if (r.at("kind") == "aarch64_branch26") branch_targets.insert(r.at("addend").get<std::int64_t>());
        else if (target == "main") data_targets.insert(r.at("addend").get<std::int64_t>());
    }
    // Native menu state addresses are member-function pointers, never direct calls.
    assert(data_targets.count(0x8039C0) && data_targets.count(0x8032C0));
    assert(!branch_targets.count(0x8039C0) && !branch_targets.count(0x8032C0));
    std::set<std::int64_t> expected = {
        0x7E1810, // native condition0x22, shared Bag/Pokemon picker-entry guard
        0x7E2240, 0x7E2480, 0x8041B0, 0x818500, 0x802ED8, 0x802DF4,          // phase 1
        0x7FCE14, 0x7E1600, 0x818630, 0x818600, 0x814330, 0x7E2D30, 0x805070, // phase 2
        0x7F8110, 0x8141D0, 0x7F35C0, 0x7EC410, 0x8048A0, 0x7EC3F0, 0x7F98A0, 0x7F9940,
        0x685A04, 0x7FC39C,                                                    // HUD hide / battle lifetime
        0xDBD854, 0x13B6720, 0x13B7300, 0x1420B70, 0xDCED50, 0xDCECC0,        // field actions
        0xD7E260, 0xDA02C0, 0xE05550, 0x1410F00, 0x989AD0, 0xD7E430, 0xCB17D0, 0xF09C00, 0x1519990, 0xDF2CA0,
        0x7844F0,                                                              // party exchange (PARTY_SWAP)
        0x9A4F60};                                                             // Decide: sub-screen slot lookup
    // Decide checks the command view's decided-command getter (view vtable +0xC0) by address.
    assert(data_targets.count(0x99E6E0));
    if (used.count(OffsetFieldProgressionSeq)) {
        expected.insert({0x1421090,0x798730,0x77A5F0,0x7783B0,0x7783C0,0x77B350,
                         0x764B10,0x764BA0,0x13B4620,0x77DB00,0x77DA90,0xC8DA80,0xDCEC10,
                         0xC939E0,0xC93FB0}); // native post-evolution field fade/restore wait
    }
    if (used.count(OffsetMapQueryZone)) expected.insert({0xD7E2E0,0xF096D0});
    if (used.count(OffsetCursorReqSeq)) {
        expected.insert(0x15B7A20); // grid list "cursor moved" (bottom -> top move cursor)
        // The cursor channel checks the move view / grid list / grid cursor vtables.
        assert(data_targets.count(0x252DC10) && data_targets.count(0x25BC8F0) && data_targets.count(0x25BD1F8));
        assert(used.count(OffsetCursorAckSeq));
    }
    if (used.count(OffsetDmaxPreviewBeat)) {
        expected.insert({0x8040B0, 0x7E1290, 0x7E5AD0, 0x845420, 0x8454F0, 0x8C7390,
                         0x780F80, 0x7F9470, 0x7DF7F0, 0x7ED700, 0x7EE2F0, 0x804670});
        assert(used.count(OffsetBattleKindSerial) && used.count(OffsetRaidTargetMask));
        assert(used.count(OffsetDmaxBaseIds) && used.count(OffsetHudSeenMode));
    }
    if (portrait) {
        expected.insert({0xF1CD34,0x674110,0xADF8B0,0xADFE00,0xADFF50,0xADFF30,0x5FDD90,0x18FDC90,0xADFF54,0x10F28A4});
        assert(used.count(OffsetPortraitReqSeq) && used.count(OffsetPortraitPhase) &&
               used.count(OffsetPortraitCapturedSeq) && used.count(OffsetPortraitTarget) &&
               used.count(OffsetPortraitWorker) && used.count(OffsetPortraitLifetimeFault));
        assert(FieldPortraitAutoVerified); // first-load and cancellation qualification required
    }
    assert(branch_targets == expected);
    std::printf("stub encoding ok (%zu instructions, %zu mailbox offsets)\n", words.size(), used.size());
}

} // namespace

// Cursor channel (bottom -> top): request fields land before the seq, one request in flight, the
// guest's answer (or a timeout) closes it, row -> grid index row * 2 + 1, the epoch is the host's.
void TestCursorChannel() {
    Fake f;
    f.Set(EpochOffset, 7);
    f.Set(OffsetCursorAckSeq, 41);
    auto load = [&](std::uint32_t o, std::uint32_t* v) { *v = f.Get(o); return true; };
    auto store = [&](std::uint32_t o, std::uint32_t v) { f.Set(o, v); return true; };
    CursorChannel<decltype(load), decltype(store)> c(load, store);
    assert(c.Initialize());
    assert(f.Get(OffsetCursorReqSeq) == 41); // nothing pending
    assert(!c.Stage(4, 1, 2, 3, 0) && !c.Stage(1, 0, 2, 3, 0) && !c.Stage(-1, 1, 2, 3, 0));
    assert(c.Stage(2, 0x21'89C0'0000ull, 0x21'892A'5520ull, 0x21'9512'EE60ull, 100));
    assert(f.Get(OffsetCursorReqSeq) == 42 && f.Get(OffsetCursorReqIndex) == 5 && f.Get(OffsetCursorReqEpoch) == 7);
    assert(f.Get(OffsetCursorReqView) == 0x892A5520u && f.Get(OffsetCursorReqView + 4) == 0x21u);
    assert(f.Get(OffsetCursorReqClient) == 0x9512EE60u && f.Get(OffsetCursorReqList + 4) == 0x21u);
    assert(!c.Stage(1, 1, 2, 3, 110)); // one in flight
    CursorResult code{};
    assert(!c.Poll(120, &code));
    f.Set(OffsetCursorResult, static_cast<std::uint32_t>(CursorResult::NotWaiting));
    f.Set(OffsetCursorAckSeq, 42);
    assert(c.Poll(130, &code) && code == CursorResult::NotWaiting && !c.pending());
    assert(c.Stage(0, 1, 2, 3, 200) && f.Get(OffsetCursorReqIndex) == 1);
    assert(!c.Poll(1600, &code) && c.Poll(1701, &code) && code == CursorResult::Stale);
    f.Set(EpochOffset, 0);
    assert(!c.Stage(1, 1, 2, 3, 1800)); // no host epoch
}

// Cursor channel list kinds (2026-11-01): command menu cells and target-select grid indices.
void TestCursorKinds() {
    Fake f;
    f.Set(EpochOffset, 3);
    auto load = [&](std::uint32_t o, std::uint32_t* v) { *v = f.Get(o); return true; };
    auto store = [&](std::uint32_t o, std::uint32_t v) { f.Set(o, v); return true; };
    CursorChannel<decltype(load), decltype(store)> c(load, store);
    assert(c.Initialize());
    assert(c.StageIndex(CursorList::Target, 18, 0x21'8914'2AB0ull, 0x21'88AA'0B00ull, 0x21'9512'EE60ull, 10));
    assert(f.Get(OffsetCursorReqIndex) == 18 && f.Get(OffsetCursorReqKind) == 2 && f.Get(OffsetCursorReqSeq) == 1);
    CursorResult code{};
    f.Set(OffsetCursorAckSeq, 1);
    assert(c.Poll(20, &code) && code == CursorResult::Ok);
    assert(c.StageIndex(CursorList::Command, 5, 1, 2, 3, 30) && f.Get(OffsetCursorReqKind) == 1 &&
           f.Get(OffsetCursorReqIndex) == 5);
    f.Set(OffsetCursorAckSeq, 2);
    assert(c.Poll(40, &code));
    assert(c.Stage(3, 1, 2, 3, 50) && f.Get(OffsetCursorReqKind) == 0 && f.Get(OffsetCursorReqIndex) == 7);
    f.Set(OffsetCursorAckSeq, 3);
    assert(c.Poll(60, &code));
    assert(!c.StageIndex(CursorList::Target, 300, 1, 2, 3, 70)); // outside any grid
    assert(!c.StageIndex(CursorList::Target, 0, 0, 2, 3, 70));   // no list
}

// Menu hide (2026-11-01): the rule (default hidden only where the bottom is the choice surface,
// never over the raid catch prompt, target select only with the synced target page), the
// request/heartbeat/cursor-list protocol, and the guest's per-slot hidden flags.
void TestMenuHide() {
    // flag 0 (setting off = default), shown, current kind, drivable, no catch prompt.
    assert(MenuWant(0, true, true, true, false, true) == (MenuReqMenus | MenuReqTarget));
    assert(MenuWant(0, true, true, true, false, false) == MenuReqMenus);
    assert(MenuWant(1, true, true, true, false, true) == 0);   // "Battle menu on top" on
    assert(MenuWant(-1, true, true, true, false, true) == 0);  // unknown host answer
    assert(MenuWant(0, false, true, true, false, true) == 0);  // companion hidden / other page
    assert(MenuWant(0, true, false, true, false, true) == 0);  // stale battle kind
    assert(MenuWant(0, true, true, false, false, true) == 0);  // bottom cannot choose (link)
    assert(MenuWant(0, true, true, true, true, true) == 0);    // raid catch prompt up
    Fake f;
    auto load = [&](std::uint32_t o, std::uint32_t* v) { *v = f.Get(o); return true; };
    auto store = [&](std::uint32_t o, std::uint32_t v) { f.Set(o, v); return true; };
    MenuHide<decltype(load), decltype(store)> h(load, store);
    f.Set(OffsetMenuReq, 3);
    assert(h.Initialize() && f.Get(OffsetMenuReq) == 0);
    const std::uint64_t cursors[4] = {0x21'890A'FC78ull, 0x21'8946'CF80ull, 0, 0x21'8912'DF58ull};
    h.Tick(MenuReqMenus | MenuReqTarget, cursors);
    assert(f.Get(OffsetMenuReq) == 3 && f.Get(OffsetMenuReqBeat) == 1);
    assert(f.Get(OffsetMenuCursors) == 0x890AFC78u && f.Get(OffsetMenuCursors + 4) == 0x21u);
    assert(f.Get(OffsetMenuCursors + 16) == 0 && f.Get(OffsetMenuCursors + 24) == 0x8912DF58u);
    f.Set(OffsetMenuRecords + MenuRecordBytes * 0 + 20, 1);
    f.Set(OffsetMenuRecords + MenuRecordBytes * 6 + 20, 1);
    h.Tick(MenuReqMenus, cursors);
    assert(f.Get(OffsetMenuReq) == 1 && f.Get(OffsetMenuReqBeat) == 2 && h.status().hidden == 0b1000001);
    const std::uint64_t none[4] = {};
    h.Tick(0, none);
    assert(f.Get(OffsetMenuReq) == 0 && f.Get(OffsetMenuCursors) == 0 && f.Get(OffsetMenuCursors + 4) == 0);
    h.Tick(1, cursors);
    h.Release();
    assert(f.Get(OffsetMenuReq) == 0);
}

// Owner Thor 2026-11-01 (doubles: "some time top screen shown, sometime hidden"): a failed
// guest load (battle kind / serial / magic) on single ticks must not drop the request; the
// player's own switches and a catch prompt act at once; a real end drops it within the hold.
void TestMenuWantFilterHolds() {
    MenuWantFilter w;
    const std::uint32_t both = MenuReqMenus | MenuReqTarget;
    for (int tick = 0; tick < 600; ++tick) { // 10 s, every 3rd tick the kind unknown, every 5th undrivable
        const bool kind = tick % 3 != 1, drive = tick % 5 != 2;
        assert(w.Step(0, true, kind, drive, false, true) == both);
    }
    assert(w.Step(0, true, false, true, false, false) == MenuReqMenus); // target bit: the caller's, live
    assert(w.Step(1, true, true, true, false, true) == 0);              // "Battle menu on top" on: at once
    assert(w.Step(0, true, false, true, false, true) == 0);             // nothing to hold after a reset
    assert(w.Step(0, true, true, true, false, true) == both);
    assert(w.Step(0, false, true, true, false, true) == 0);             // companion hidden: at once
    assert(w.Step(0, true, true, true, false, true) == both);
    assert(w.Step(0, true, true, true, true, true) == 0);               // catch prompt shown: at once
    assert(w.Step(0, true, true, true, false, true) == both);
    int held = 0;
    while (w.Step(0, true, false, false, false, true) && held < 100) ++held; // the battle ended
    assert(held == MenuWantFilter::kHoldTicks);
    assert(w.Step(0, true, false, false, false, true) == 0);
    std::puts("sw_helper_mailbox_test: the menu-hide request holds through failed guest loads");
}

int main() {
    TestCursorKinds();
    TestMenuHide();
    TestMenuWantFilterHolds();
    TestPresenceAndMenuWaiting();
    TestFightRoundTrip();
    TestBattleKindGate();
    TestTimeoutCancelsAndNeverRunsLater();
    TestStaleEpochAndReinitialize();
    TestPreconditions();
    TestOpenMenuPreconditions();
    TestOpenMenuRoundTripAndCancellation();
    TestBallGuardsAndNativeIdentityPayload();
    TestBattleAliveBeat();
    TestSwitchItemRoundTrip();
    TestSwitchItemPreconditions();
    TestHudHide();
    TestHudBattleLifetime();
    TestFieldChannel();
    TestProgressionLifecycle();
    TestFieldReloadSequences();
    TestNativePreviewPipeline();
    TestHudBattleModes();
    TestRaidTargetsAndMaxPayload();
    TestStubEncoding();
    TestManifestDirectContracts();
    TestCursorChannel();
    std::puts("sw_helper_mailbox_test: all passed");
    return 0;
}
