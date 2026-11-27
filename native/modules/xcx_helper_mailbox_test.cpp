// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit test of the XCX helper's module side against a fake mailbox, a fake guest (a C++ mirror of
// xcx_guest_helper.S's protocol) and a fake clock. Offline: no emulator, no game memory.
#include "xcx_helper_mailbox.h"

#include <cassert>
#include <cstdint>
#include <array>
#include <cstdio>
#include <map>
#include <vector>

using namespace xcx_helper;

namespace {

// ---- layout: the offsets the assembly encodes as literals ----------------------------------------
static_assert(MailboxSize == 4096 && EpochOffset == 4092);
static_assert(OffsetReqSeq == 0x20 && OffsetAckSeq == 0x30 && OffsetCancelSeq == 0x2C);
static_assert(OffsetState == 0x0C && OffsetHeartbeat == 0x08 && OffsetPollBeat == 0x10);
static_assert(ModuleMagic == 0x314D4358 && GuestMagic == 0x31474358);
static_assert(static_cast<std::uint32_t>(Result::NotReady) == 8);

struct Mailbox {
    std::map<std::uint32_t, std::uint32_t> words;
    int stores = 0;
    bool fail_stores = false;
    bool Load(std::uint32_t off, std::uint32_t* v) {
        *v = words.count(off) ? words[off] : 0;
        return true;
    }
    bool Store(std::uint32_t off, std::uint32_t v) {
        if (fail_stores) return false;
        words[off] = v;
        ++stores;
        return true;
    }
    std::uint32_t Get(std::uint32_t off) { return words.count(off) ? words[off] : 0; }
};

struct Load32 {
    Mailbox* m;
    bool operator()(std::uint32_t o, std::uint32_t* v) const { return m->Load(o, v); }
};
struct Store32 {
    Mailbox* m;
    bool operator()(std::uint32_t o, std::uint32_t v) const { return m->Store(o, v); }
};

// What the game's field state looks like to the fake guest.
struct Game {
    bool hud_ok = true, gate = true, piloting = false, valid = true, transitioning = false, input_ready = true, field_ok = true, can_return = false;
    int return_stores = 0;
    bool can_board = true, can_exit = true, singleton_blocked = false;
    std::uint32_t game_returns = 1; // tryRideOn / tryGetOff result
    int slot = 0;
    int ride_calls = 0, exit_calls = 0;
    // switch world: member ids by slot (0 = avatar), request block idle, cooldown, staged rows
    std::array<int, 4> party{0, 11, 12, 14};
    bool idle = true, cooldown = false;
    struct Row { std::uint32_t slot; std::uint16_t kind, id, aux1, aux2; bool operator==(const Row&) const = default; };
    std::vector<Row> staged;
    int commits = 0;
    static std::uint32_t Kind(int id) { return id == 0 ? 1u : 2u; }
    static void Aux(int id, std::uint16_t& a1, std::uint16_t& a2) {   // fake 0x56bed0 / live table
        a1 = a2 = 0;
        if (id == 0) { a1 = 6; a2 = 1; }
        else if (id == 11) { a1 = 6; a2 = 2; }
        else if (id == 12) { a1 = 6; a2 = 3; }
    }
};

// Mirror of xcx_guest_helper.S: one call per game frame (hook call).
struct Guest {
    Mailbox* m;
    Game* g;
    bool running = true;
    std::uint32_t Call() {
        // returns the state word published this call (0 when none)
        if (!running) return 0;
        m->words[OffsetGuestMagic] = GuestMagic;
        m->words[OffsetHeartbeat] = m->Get(OffsetHeartbeat) + 1;
        if (m->Get(OffsetModuleMagic) != ModuleMagic) return 0;
        const std::uint32_t seq = m->Get(OffsetReqSeq);
        if (seq != m->Get(OffsetAckSeq)) {
            std::uint32_t code = 0, value = 0;
            const std::uint32_t epoch = m->Get(EpochOffset);
            const std::uint32_t op = m->Get(OffsetReqOp);
            if (m->Get(OffsetCancelSeq) == seq) code = 4;
            else if (epoch == 0 || m->Get(OffsetReqEpoch) != epoch) code = 3;
            else if (op < 1 || op > 4) code = 2;
            else if (!g->hud_ok || !g->input_ready || !g->field_ok) code = 8;
            else if (g->transitioning) code = 9;
            else if (!g->valid) code = 6;
            else if (op == 4) {
                if (g->piloting) code = 5;
                else if (!g->can_return) code = 12;
                else { ++g->return_stores; value = 1; code = 0; }
                m->words[OffsetResultCode] = code;
                m->words[OffsetResultValue] = value;
                m->words[OffsetAckBeat] = m->Get(OffsetHeartbeat);
                m->words[OffsetAnswered] = m->Get(OffsetAnswered) + 1;
                m->words[OffsetPollCountdown] = 0;
                m->words[OffsetAckSeq] = seq;
                return 0;
            }
            else if (op == 3) {
                const std::uint32_t k = m->Get(OffsetReqArg);
                if (g->piloting) code = 5;
                else if (k < 1 || k > 3) code = 2;
                else if (!g->idle) code = 8;
                else if (g->cooldown) code = 10;
                else if (MemberToken(g->party[k]) != m->Get(OffsetReqTarget)) code = 11;
                else {
                    auto next = g->party;
                    std::swap(next[0], next[k]);
                    g->staged.clear();
                    for (std::uint32_t s2 = 0; s2 < 4; ++s2) {
                        Game::Row row{s2, static_cast<std::uint16_t>(Game::Kind(next[s2])), static_cast<std::uint16_t>(next[s2]), 0, 0};
                        Game::Aux(next[s2], row.aux1, row.aux2);
                        g->staged.push_back(row);
                    }
                    ++g->commits;
                    g->idle = false; // the block stays busy for a few frames
                    value = MemberToken(g->party[k]);
                    m->words[OffsetSubmitToken] = value;
                    m->words[OffsetSubmitCount] = m->Get(OffsetSubmitCount) + 1;
                    g->party = next;
                    code = 0;
                }
                if (code != 0) value = 0;
                m->words[OffsetResultCode] = code;
                m->words[OffsetResultValue] = value;
                m->words[OffsetAckBeat] = m->Get(OffsetHeartbeat);
                m->words[OffsetAnswered] = m->Get(OffsetAnswered) + 1;
                m->words[OffsetPollCountdown] = 0;
                m->words[OffsetAckSeq] = seq;
                return 0;
            }
            else if (op == 1 && g->piloting) code = 5;
            else if (op == 2 && !g->piloting) code = 5;
            else {
                if (op == 1) ++g->ride_calls;
                else ++g->exit_calls;
                value = g->game_returns & 1;
                code = value ? 0 : 7;
            }
            m->words[OffsetResultCode] = code;
            m->words[OffsetResultValue] = value;
            m->words[OffsetAckBeat] = m->Get(OffsetHeartbeat);
            m->words[OffsetAnswered] = m->Get(OffsetAnswered) + 1;
            m->words[OffsetPollCountdown] = 0;
            m->words[OffsetAckSeq] = seq;
            return 0;
        }
        const std::uint32_t countdown = m->Get(OffsetPollCountdown);
        if (countdown != 0) {
            m->words[OffsetPollCountdown] = countdown - 1;
            return 0;
        }
        m->words[OffsetPollCountdown] = PollEveryCalls - 1;
        std::uint32_t w = 0;
        if (g->hud_ok) w |= StateHudOk;
        if (g->input_ready) w |= StateInputReady;
        if (g->field_ok) w |= StateFieldOk;
        if (g->gate) w |= StateGate;
        if (g->transitioning) w |= StateTransition;
        if (g->valid) {
            w |= StateValid | (g->piloting ? StatePiloting : StateOnFoot);
            w |= static_cast<std::uint32_t>(g->slot) << StateSlotShift;
            // the prompt gate is diagnostic only: it never blocks eligibility
            if (!g->input_ready || !g->field_ok) w &= ~(StateCanBoard | StateCanExit | StateCanReturn);
            if (g->can_return && !g->piloting) w |= StateCanReturn;
            if (!g->piloting && g->can_board) w |= StateCanBoard;
            if (g->piloting && !g->singleton_blocked && g->can_exit) w |= StateCanExit;
        }
        if (g->valid) {
            if (g->idle) w |= StateIdle;
            if (!g->cooldown) w |= StateCooldownClear;
            if (!g->piloting && g->idle && !g->cooldown && g->input_ready && g->field_ok) w |= StateSwitchReady;
        }
        m->words[OffsetSlot0Token] = g->valid ? MemberToken(g->party[0]) : UnknownToken;
        m->words[OffsetState] = w;
        m->words[OffsetPollBeat] = m->Get(OffsetPollBeat) + 1;
        return w;
    }
};

struct Rig {
    Mailbox mb;
    Game game;
    Guest guest{&mb, &game};
    Bridge<Load32, Store32> bridge{Load32{&mb}, Store32{&mb}};
    Controller ctl;
    SwitchController sw;
    SetButton setb;
    bool sel_present = true, sel_controlled = false;
    std::int64_t now = 1000;
    Button button;
    Outcome last;
    bool got_outcome = false;
    static constexpr std::int64_t Frame = 33;
    Rig() {
        mb.words[EpochOffset] = 7;
        assert(bridge.Initialize());
    }
    // One game frame + one module sample.
    void Frame1() {
        guest.Call();
        now += Frame;
        Sample();
    }
    void Sample() {
        bridge.Update(now);
        Outcome o;
        if (bridge.TakeOutcome(o)) {
            ctl.OnOutcome(o, now);
            sw.OnOutcome(o, bridge.status(), now);
            last = o;
            got_outcome = true;
        }
        button = ctl.Update(bridge.status(), true, now);
        setb = sw.Update(bridge.status(), sel_present, sel_controlled, now);
    }
    void Frames(int n) {
        for (int i = 0; i < n; ++i) Frame1();
    }
    // Sample only (the game is not running).
    void Idle(std::int64_t ms) {
        const std::int64_t end = now + ms;
        while (now < end) {
            now += Frame;
            Sample();
        }
    }
};

void TestInitialize() {
    Rig r;
    assert(r.mb.Get(OffsetModuleMagic) == ModuleMagic);
    assert(r.mb.Get(OffsetReqSeq) == r.mb.Get(OffsetAckSeq));
    assert(r.mb.Get(OffsetCancelSeq) == 0);
    // never reuse an answered sequence number
    Mailbox mb;
    mb.words[OffsetAckSeq] = 41;
    Bridge<Load32, Store32> b{Load32{&mb}, Store32{&mb}};
    assert(b.Initialize());
    assert(mb.Get(OffsetReqSeq) == 41);
    // a failing store is reported
    Mailbox bad;
    bad.fail_stores = true;
    Bridge<Load32, Store32> bb{Load32{&bad}, Store32{&bad}};
    assert(!bb.Initialize());
}

void TestHelperAbsentLooksLikePrototype() {
    Rig r;
    r.guest.running = false;
    r.Idle(3000);
    assert(!r.bridge.status().helper_present);
    assert(r.button == Button{}); // Return face, disabled, NoHelper
    assert(r.button.face == Face::Return && !r.button.enabled && r.button.reason == Reason::NoHelper);
    // a tap is refused and stages nothing
    const auto press = r.ctl.OnPress(r.bridge, r.button, r.now);
    assert(!press.accepted);
    assert(r.mb.Get(OffsetReqSeq) == r.mb.Get(OffsetAckSeq));
    assert(!r.bridge.Stage(Op::Board, r.now));
}

void TestPresentAndFaces() {
    Rig r;
    r.Frames(40);
    assert(r.bridge.status().helper_present && r.bridge.status().state_fresh);
    assert(r.bridge.FrameMs() >= 20 && r.bridge.FrameMs() <= 50);
    assert(r.button.face == Face::Board && r.button.enabled && r.button.reason == Reason::None);
    // not eligible on foot: the prototype's own face, disabled
    r.game.can_board = false;
    r.Frames(30);
    assert(r.button.face == Face::Return && !r.button.enabled && r.button.reason == Reason::NotNow);
    // piloting, canGetOff yes -> Exit (after the flip lockout)
    r.game.piloting = true;
    r.Frames(30);
    assert(r.button.face == Face::Exit);
    r.Idle(700);
    r.Frames(20);
    assert(r.button.face == Face::Exit && r.button.enabled);
    // piloting, canGetOff no -> Exit shown disabled with the reason
    r.game.can_exit = false;
    r.Frames(30);
    assert(r.button.face == Face::Exit && !r.button.enabled && r.button.reason == Reason::NotNow);
    // exit singleton blocked: same
    r.game.can_exit = true;
    r.game.singleton_blocked = true;
    r.Frames(30);
    assert(!r.button.enabled);
    // prompt gate closed: diagnostic only, the button stays eligible
    r.game.singleton_blocked = false;
    r.game.gate = false;
    r.Frames(30);
    assert(r.button.enabled && r.button.face == Face::Exit);
    // transition (neither actor carries the driven mark): prototype face, disabled, no request
    r.game.transitioning = true;
    r.game.valid = false;
    r.Frames(30);
    assert(!r.button.enabled && r.button.face == Face::Return);
    assert(!r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    r.game.transitioning = false;
    r.game.valid = true;
    r.Frames(30);
    // HUD object not verified: unknown -> prototype face
    r.game.gate = true;
    r.game.hud_ok = false;
    r.Frames(30);
    assert(r.button.face == Face::Return && !r.button.enabled);
    // controlled object not the record's: invalid -> prototype face
    r.game.hud_ok = true;
    r.game.valid = false;
    r.Frames(30);
    assert(r.button.face == Face::Return && !r.button.enabled);
}

void TestContradictoryStateIgnored() {
    assert(!DecodeState(StateValid | StatePiloting | StateOnFoot | StateInputReady | StateFieldOk).valid);
    assert(!DecodeState(StatePiloting).valid); // not valid
    // the game does not accept input (menu/event/pause): nothing is eligible, state is not valid
    const auto ni = DecodeState(StateValid | StateOnFoot | StateGate | StateHudOk | StateCanBoard | StateSwitchReady | StateIdle | StateCooldownClear);
    assert(!ni.valid && !ni.can_board && !ni.switch_ready && !ni.input_ready && ni.hud_ok);
    const auto s = DecodeState(StateValid | StateOnFoot | StateGate | StateHudOk | StateInputReady | StateFieldOk | StateCanBoard | StateCanExit);
    assert(s.valid && s.can_board && !s.can_exit);
    const auto n = DecodeState(StateValid | StateOnFoot | StateHudOk | StateInputReady | StateFieldOk | StateCanBoard); // gate closed
    assert(n.valid && n.can_board && !n.gate);
    const auto t = DecodeState(StateTransition | StateHudOk);
    assert(!t.valid && t.transitioning && !t.can_board);
    assert(!DecodeState(StateTransition).transitioning); // without a verified HUD object
    assert(DecodeState(StateValid | StatePiloting | StateInputReady | StateFieldOk | (3u << StateSlotShift)).slot == 3);
}

void TestStaleHeartbeat() {
    Rig r;
    r.Frames(40);
    assert(r.bridge.status().helper_present && r.button.enabled);
    r.guest.running = false; // the HUD update stopped (menu / event / loading)
    r.Idle(r.bridge.BeatWindowMs() + 100);
    assert(!r.bridge.status().helper_present && !r.bridge.status().state_fresh);
    assert(!r.button.enabled && r.button.face == Face::Return && r.button.reason == Reason::NoHelper);
    // it resumes: fresh again only after a new heartbeat and a new state publication
    // (the game state changed while the helper was silent: the old state word must never be used)
    r.game.can_board = false;
    r.guest.running = true;
    for (int i = 0; i < 25; ++i) {
        r.Frame1();
        assert(!r.button.enabled);
    }
    assert(r.bridge.status().helper_present && r.bridge.status().state_fresh);
}

void TestStaleStateWindow() {
    Rig r;
    r.Frames(40);
    // heartbeat keeps moving but the state word does not (a guest that no longer publishes)
    for (int i = 0; i < 200; ++i) {
        r.mb.words[OffsetHeartbeat] = r.mb.Get(OffsetHeartbeat) + 1;
        r.now += Rig::Frame;
        r.Sample();
    }
    assert(r.bridge.status().helper_present && !r.bridge.status().state_fresh);
    assert(!r.button.enabled);
}

void TestTransitionAndGateRequests() {
    // transitioning: the guest answers 9 and never calls the game
    Rig r;
    r.Frames(40);
    assert(r.button.enabled);
    r.game.gate = false; // a closed gate must not stop a request
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    r.guest.Call();
    assert(r.game.ride_calls == 1 && r.mb.Get(OffsetResultCode) == 0);
    Rig t;
    t.Frames(40);
    assert(t.button.enabled);
    t.game.transitioning = true;
    assert(t.bridge.Stage(Op::Board, t.now)); // staged before the state is seen
    t.guest.Call();
    assert(t.game.ride_calls == 0 && t.mb.Get(OffsetResultCode) == 9);
    t.now += Rig::Frame;
    t.Sample();
    assert(t.got_outcome && t.last.answered && t.last.code == Result::Transitioning);
    // stale handles after a native switch: the guest answers BadHandle, then recovers
    Rig s;
    s.Frames(40);
    s.game.valid = false;
    assert(s.bridge.Stage(Op::Board, s.now));
    s.guest.Call();
    assert(s.mb.Get(OffsetResultCode) == 6 && s.game.ride_calls == 0);
    s.game.valid = true;
    s.Frames(60);
    assert(s.button.enabled);
}

void TestInputNotReady() {
    // gap 2: the `+` menu. Board, Exit and Switch all refuse with NotReady and the face is the prototype's.
    Rig r;
    r.Frames(40);
    assert(r.button.enabled && r.setb.enabled);
    r.game.input_ready = false;
    r.Frames(40);
    assert(r.bridge.status().helper_present && !r.bridge.status().state.valid);
    assert(!r.button.enabled && r.button.face == Face::Return && !r.setb.enabled);
    assert(!r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    for (Op op : {Op::Board, Op::Exit, Op::Switch}) {
        assert(r.bridge.Stage(op, r.now, 1, MemberToken(11)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 8);
        r.now += Rig::Frame;
        r.Sample();
        assert(r.last.code == Result::NotReady && r.last.answered);
    }
    assert(r.game.ride_calls == 0 && r.game.exit_calls == 0 && r.game.commits == 0);
    // back in the field: eligible again after a fresh poll
    r.game.input_ready = true;
    r.Frames(60);
    assert(r.button.enabled && r.setb.enabled);
    // piloting variant
    Rig p;
    p.game.piloting = true;
    p.Frames(80);
    assert(p.button.enabled && p.button.face == Face::Exit);
    p.game.input_ready = false;
    p.Frames(40);
    assert(!p.button.enabled);
}

void TestReturnToSkell() {
    // far from the Skell on foot, can_return: the Return face, a hold acts
    Rig r;
    r.game.can_board = false;
    r.game.can_return = true;
    r.Frames(60);
    assert(r.button.face == Face::Return && r.button.return_ready && !r.button.enabled);
    assert(r.bridge.status().state.can_return && r.bridge.status().state.field_ok);
    // a short tap is a refusal (haptic) and shows the Hold hint; nothing is staged
    assert(!r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    assert(r.mb.Get(OffsetReqSeq) == r.mb.Get(OffsetAckSeq));
    r.Sample();
    assert(r.button.hint);
    r.Idle(RefusedShownMs + 100);
    r.Frames(2);
    assert(!r.button.hint);
    // the hold stages op 4
    assert(r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
    assert(r.mb.Get(OffsetReqOp) == 4);
    assert(!r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted); // one at a time
    r.guest.Call();
    assert(r.game.return_stores == 1 && r.mb.Get(OffsetResultCode) == 0);
    r.now += Rig::Frame;
    r.Sample();
    assert(r.last.op == Op::Return && r.last.code == Result::Submitted);
    // lockout, then a long settle: neither tap nor hold acts while the loading runs
    r.Frames(10);
    assert(!r.button.return_ready && r.button.ring);
    r.Frames(40);
    assert(!r.button.return_ready && r.button.reason == Reason::Settling && r.button.ring);
    assert(!r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
    // the loading screen: the helper is silent for a while, the face is the prototype's
    r.guest.running = false;
    r.Idle(5000);
    assert(!r.button.return_ready && r.button.face == Face::Return && r.button.reason == Reason::NoHelper);
    r.guest.running = true;
    r.game.piloting = true; // the warp finished: the Skell is driven
    r.Frames(80);
    assert(r.button.face == Face::Exit);
    // back on foot later: Return is offered again (the settle ended with the piloting state)
    r.game.piloting = false;
    r.game.can_return = true;
    r.Frames(80);
    assert(r.button.return_ready || r.button.face == Face::Board);
}

void TestInFlight() {
    // Return in flight: a re-arm is blocked until (a) piloting, (b) field_ok 0 then 1, or (c) 90 s
    auto submit = [](Rig& r) {
        r.game.can_board = false;
        r.game.can_return = true;
        r.Frames(60);
        assert(r.button.return_ready);
        assert(r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
        r.guest.Call();
        r.now += Rig::Frame;
        r.Sample();
        assert(r.last.op == Op::Return && r.last.code == Result::Submitted);
    };
    {   // blocked while the helper still reports an eligible field for several seconds
        Rig r;
        submit(r);
        r.Frames(int(4000 / Rig::Frame)); // the ~4 s sample of the live run
        assert(!r.button.return_ready && !r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
        assert(r.mb.Get(OffsetReqSeq) == r.mb.Get(OffsetAckSeq));
    }
    {   // (a) piloting observed
        Rig r;
        submit(r);
        r.game.piloting = true;
        r.Frames(80);
        r.game.piloting = false;
        r.Frames(80);
        assert(r.button.return_ready || r.button.face == Face::Board);
    }
    {   // (b) field_ok goes 0 and comes back
        Rig r;
        submit(r);
        r.Frames(int(2000 / Rig::Frame));
        assert(!r.button.return_ready);
        r.game.field_ok = false;
        r.Frames(60);
        assert(!r.button.return_ready);
        r.game.field_ok = true;
        r.Frames(60);
        assert(r.button.return_ready);
    }
    {   // (b') a silent helper (the loading screen) counts as the zero
        Rig r;
        submit(r);
        r.guest.running = false;
        r.Idle(3000);
        r.guest.running = true;
        r.Frames(80);
        assert(r.button.return_ready);
    }
    {   // (c) 90 s timeout
        Rig r;
        submit(r);
        r.Frames(int(80000 / Rig::Frame));
        assert(!r.button.return_ready);
        r.Frames(int(12000 / Rig::Frame));
        assert(r.button.return_ready);
    }
    {   // Board / Exit: disabled until the state flips or 10 s
        Rig r;
        r.Frames(60);
        assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
        r.guest.Call();
        r.Frames(int(9000 / Rig::Frame));
        assert(!r.button.enabled && r.button.reason == Reason::Settling);
        r.Frames(int(3000 / Rig::Frame));
        assert(r.button.enabled); // never flipped: released by the 10 s timeout
    }
    {   // Switch: disabled until slot 0 holds the target or 10 s
        Rig r;
        r.Frames(60);
        assert(r.sw.OnPress(r.bridge, r.setb, 1, 11, r.now).accepted);
        r.guest.Call();
        r.game.idle = true;
        // the fake guest already moved slot 0 to the target: released after the two polls
        r.Frames(80);
        assert(r.setb.enabled);
        Rig q;
        q.Frames(60);
        assert(q.sw.OnPress(q.bridge, q.setb, 1, 11, q.now).accepted);
        q.guest.Call();
        q.game.idle = true;
        q.game.party = {0, 11, 12, 14}; // the game did not reorder yet: slot 0 is not the target
        q.Frames(int(9000 / Rig::Frame));
        assert(!q.setb.enabled && q.setb.reason == Reason::Settling);
        q.Frames(int(3000 / Rig::Frame));
        assert(q.setb.enabled);
    }
}

void TestReturnGates() {
    {   // Board has priority over Return
        Rig r;
        r.game.can_board = true;
        r.game.can_return = true;
        r.Frames(60);
        assert(r.button.face == Face::Board && r.button.enabled && !r.button.return_ready);
    }
    {   // can_return 0 (indoors / piloting): the prototype face, no hold
        Rig r;
        r.game.can_board = false;
        r.game.can_return = false;
        r.Frames(60);
        assert(r.button.face == Face::Return && !r.button.return_ready && r.button.reason == Reason::NotNow);
        assert(!r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
    }
    {   // the field does not accept input: nothing
        Rig r;
        r.game.can_board = false;
        r.game.can_return = true;
        r.Frames(60);
        assert(r.button.return_ready);
        r.game.input_ready = false;
        r.Frames(40);
        assert(!r.button.return_ready);
    }
    {   // the guest refuses: CannotReturn (12) stores nothing and the face recovers, no settle
        Rig r;
        r.game.can_board = false;
        r.game.can_return = true;
        r.Frames(60);
        r.game.can_return = false; // changed between the poll and the hook
        assert(r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 12 && r.game.return_stores == 0);
        r.now += Rig::Frame;
        r.Sample();
        assert(r.last.code == Result::CannotReturn);
        r.game.can_return = true;
        r.Frames(60);
        assert(r.button.return_ready);
    }
    {   // helper absent: the prototype face, no hold, no tap target semantics
        Rig r;
        r.guest.running = false;
        r.Idle(3000);
        assert(r.button == Button{});
        assert(!r.ctl.OnReturnHold(r.bridge, r.button, r.now).accepted);
    }
    // a state word without the input mark never carries can_return
    assert(!DecodeState(StateValid | StateOnFoot | StateHudOk | StateCanReturn).can_return);
    assert(DecodeState(StateValid | StateOnFoot | StateHudOk | StateInputReady | StateCanReturn | StateFieldOk).can_return);
    assert(DecodeState(StateValid | StateOnFoot | StateHudOk | StateInputReady | StateFieldOk).field_ok);
    assert(!DecodeState(StateValid | StatePiloting | StateHudOk | StateInputReady | StateFieldOk | StateCanReturn).can_return);
}

void TestFieldNotOk() {
    // loading screen: 0xf0b44 is 0 although the HUD update runs and actors exist
    Rig r;
    r.game.can_return = true;
    r.Frames(60);
    assert(r.button.enabled && r.setb.enabled);
    r.game.field_ok = false;
    r.Frames(40);
    assert(r.bridge.status().helper_present && !r.bridge.status().state.valid && !r.bridge.status().state.field_ok);
    assert(!r.button.enabled && !r.button.return_ready && r.button.face == Face::Return && !r.setb.enabled);
    for (Op op : {Op::Board, Op::Exit, Op::Switch, Op::Return}) {
        assert(r.bridge.Stage(op, r.now, 1, MemberToken(11)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 8);
        r.now += Rig::Frame;
        r.Sample();
        assert(r.last.code == Result::NotReady && r.last.answered);
    }
    assert(r.game.ride_calls == 0 && r.game.exit_calls == 0 && r.game.commits == 0 && r.game.return_stores == 0);
    r.game.field_ok = true;
    r.Frames(80);
    assert(r.button.enabled && r.setb.enabled);
    // the return face too
    Rig q;
    q.game.can_board = false;
    q.game.can_return = true;
    q.Frames(60);
    assert(q.button.return_ready);
    q.game.field_ok = false;
    q.Frames(40);
    assert(!q.button.return_ready);
    assert(!DecodeState(StateValid | StateOnFoot | StateHudOk | StateInputReady | StateCanBoard | StateSwitchReady).valid);
}

void TestBoardRequestAck() {
    Rig r;
    r.Frames(40);
    assert(r.button.enabled && r.button.face == Face::Board);
    const auto press = r.ctl.OnPress(r.bridge, r.button, r.now);
    assert(press.accepted);
    assert(r.mb.Get(OffsetReqOp) == 1 && r.mb.Get(OffsetReqEpoch) == 7);
    assert(r.mb.Get(OffsetReqSeq) != r.mb.Get(OffsetAckSeq));
    assert(r.bridge.status().busy);
    // one request at a time
    assert(!r.bridge.Stage(Op::Board, r.now));
    r.Sample();
    assert(r.button.ring && !r.button.enabled && r.button.reason == Reason::Busy);
    assert(!r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    r.guest.Call(); // the game thread runs the hook: request acted on exactly once
    assert(r.game.ride_calls == 1 && r.game.exit_calls == 0);
    r.guest.Call();
    assert(r.game.ride_calls == 1);
    r.now += Rig::Frame;
    r.Sample();
    assert(r.got_outcome && r.last.answered && r.last.code == Result::Submitted && r.last.op == Op::Board);
    assert(!r.bridge.status().busy);
    // after Submitted the button waits for the state to flip (the animation takes seconds)
    r.Frames(30);
    assert(!r.button.enabled && r.button.reason == Reason::Settling && r.button.face == Face::Board);
    // the game flips to piloting: lockout 600 ms, then Exit
    r.game.piloting = true;
    r.Frames(15);
    assert(r.button.face == Face::Exit && !r.button.enabled && r.button.reason == Reason::Lockout);
    r.Idle(700);
    r.Frames(15);
    assert(r.button.face == Face::Exit && r.button.enabled);
}

void TestExitRequestAndRefusals() {
    Rig r;
    r.game.piloting = true;
    r.Frames(80); // settle the first mode flip lockout (first known mode is not a flip)
    assert(r.button.face == Face::Exit && r.button.enabled);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    r.game.game_returns = 0; // the game's own check refuses
    r.guest.Call();
    r.now += Rig::Frame;
    r.Sample();
    assert(r.got_outcome && r.last.answered && r.last.code == Result::RefusedByGame && r.last.op == Op::Exit);
    assert(r.game.exit_calls == 1);
    // refusal reason shows for 1.5 s, the button is usable again (no flip awaited)
    r.Frames(30);
    assert(r.button.face == Face::Exit && r.button.enabled);
    // every answer code reaches the outcome unchanged
    const std::pair<Result, int> codes[] = {{Result::NotApplicable, 5}, {Result::BadHandle, 6},
                                            {Result::NotReady, 8}, {Result::BadOp, 2}};
    for (auto [res, num] : codes) {
        assert(r.bridge.Stage(Op::Exit, r.now));
        r.mb.words[OffsetResultCode] = static_cast<std::uint32_t>(num);
        r.mb.words[OffsetResultValue] = 0;
        r.mb.words[OffsetAckSeq] = r.mb.Get(OffsetReqSeq);
        r.now += Rig::Frame;
        r.Sample();
        assert(r.got_outcome && r.last.answered && r.last.code == res);
        r.got_outcome = false;
    }
    // an out-of-range code is not trusted
    assert(r.bridge.Stage(Op::Board, r.now));
    r.mb.words[OffsetResultCode] = 99;
    r.mb.words[OffsetAckSeq] = r.mb.Get(OffsetReqSeq);
    r.now += Rig::Frame;
    r.Sample();
    assert(r.last.code == Result::NotConfigured);
}

void TestRefusedReasonWindow() {
    Rig r;
    r.game.piloting = true;
    r.Frames(80);
    r.game.can_exit = false;
    r.Frames(30);
    assert(!r.button.enabled && r.button.reason == Reason::NotNow);
    Outcome o{true, true, Op::Exit, Result::RefusedByGame, 0};
    r.ctl.OnOutcome(o, r.now);
    r.Sample();
    assert(r.button.reason == Reason::Refused);
    r.Idle(RefusedShownMs + 100);
    r.guest.Call();
    r.Sample();
    assert(r.button.reason == Reason::NotNow);
}

void TestTimeoutCancel() {
    Rig r;
    r.Frames(40);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    const std::uint32_t seq = r.mb.Get(OffsetReqSeq);
    r.guest.running = false; // the hook never runs (menu / event)
    const std::int64_t window = r.bridge.AckWindowMs();
    r.Idle(window + 2 * Rig::Frame);
    assert(r.mb.Get(OffsetCancelSeq) == seq); // withdrawn
    assert(r.bridge.status().busy);
    r.Idle(window + 2 * Rig::Frame);
    assert(r.got_outcome && !r.last.answered && r.last.code == Result::Cancelled);
    assert(!r.bridge.status().busy);
    // the hook comes back much later: it must refuse as Cancelled and NOT call the game
    r.guest.running = true;
    r.guest.Call();
    assert(r.game.ride_calls == 0);
    assert(r.mb.Get(OffsetResultCode) == 4 && r.mb.Get(OffsetAckSeq) == seq);
    // an unanswered request does not leave the button waiting for a flip
    r.Frames(40);
    assert(r.button.enabled);
}

void TestLateAnswerAfterCancelIsStillTruthful() {
    Rig r;
    r.Frames(40);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    const std::uint32_t seq = r.mb.Get(OffsetReqSeq);
    r.guest.running = false;
    r.Idle(r.bridge.AckWindowMs() + 2 * Rig::Frame);
    assert(r.mb.Get(OffsetCancelSeq) == seq);
    // the guest had already executed it just before it saw the cancel word
    r.mb.words[OffsetResultCode] = 0;
    r.mb.words[OffsetResultValue] = 1;
    r.mb.words[OffsetAckSeq] = seq;
    r.now += Rig::Frame;
    r.Sample();
    assert(r.got_outcome && r.last.answered && r.last.code == Result::Submitted);
}

void TestEpochChange() {
    Rig r;
    r.Frames(40);
    assert(r.button.enabled);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    const std::uint32_t seq = r.mb.Get(OffsetReqSeq);
    // the host pauses/invalidates: the epoch moves before the guest answered
    r.mb.words[EpochOffset] = 8;
    r.now += Rig::Frame;
    r.Sample();
    assert(r.got_outcome && !r.last.answered && r.last.code == Result::StaleEpoch);
    assert(r.mb.Get(OffsetCancelSeq) == seq);
    assert(!r.bridge.status().helper_present && !r.bridge.status().state_fresh && !r.button.enabled);
    // the guest sees the stale request: it never calls the game
    r.guest.Call();
    assert(r.game.ride_calls == 0 && r.mb.Get(OffsetAckSeq) == seq);
    // everything must be observed again under the new epoch
    for (int i = 0; i < 25; ++i) {
        r.Frame1();
        assert(!r.bridge.status().state_fresh || r.bridge.status().helper_present);
    }
    assert(r.bridge.status().state_fresh && r.button.enabled);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    assert(r.mb.Get(OffsetReqEpoch) == 8);
    // a zero epoch (not seeded by the host) never lets a request go
    r.guest.Call();
    r.Sample();
    r.mb.words[EpochOffset] = 0;
    r.Frames(3);
    assert(!r.bridge.Stage(Op::Exit, r.now));
}

void TestGuestStaleEpochRefusal() {
    // a request staged under epoch 7 that the host bumped just before the guest ran
    Rig r;
    r.Frames(40);
    assert(r.bridge.Stage(Op::Board, r.now));
    r.mb.words[EpochOffset] = 9;
    r.guest.Call();
    assert(r.game.ride_calls == 0 && r.mb.Get(OffsetResultCode) == 3);
}

void TestLockoutOnFlipOnly() {
    Rig r;
    r.Frames(60);
    assert(r.button.enabled); // the first observed mode is not a flip
    r.game.piloting = true;
    r.Frames(12);
    assert(r.button.face == Face::Exit && r.button.reason == Reason::Lockout && r.button.ring);
    r.game.piloting = false; // flips back inside the lockout: restarts it
    r.Frames(12);
    assert(r.button.reason == Reason::Lockout);
    r.Idle(LockoutMs + 100);
    r.Frames(15);
    assert(r.button.enabled && r.button.face == Face::Board);
    // press during lockout is refused with the haptic
    r.game.piloting = true;
    r.Frames(12);
    const auto press = r.ctl.OnPress(r.bridge, r.button, r.now);
    assert(!press.accepted && press.reason == Reason::Lockout);
    assert(r.mb.Get(OffsetReqSeq) == r.mb.Get(OffsetAckSeq));
}

void TestSettleTimesOut() {
    Rig r;
    r.Frames(40);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    r.guest.Call();
    r.Frames(2);
    assert(r.last.code == Result::Submitted);
    r.Frames(int((SettleMs - 500) / Rig::Frame));
    assert(!r.button.enabled && r.button.reason == Reason::Settling);
    r.Frames(int(1000 / Rig::Frame));
    assert(r.button.enabled); // the game never flipped (it refused asynchronously): usable again
}

void TestNoSkell() {
    Rig r;
    r.Frames(40);
    const auto b = r.ctl.Update(r.bridge.status(), false, r.now);
    assert(b.face == Face::Return && !b.enabled);
}

void TestSeqWrap() {
    Mailbox mb;
    mb.words[EpochOffset] = 1;
    mb.words[OffsetAckSeq] = 0xFFFFFFFFu;
    Bridge<Load32, Store32> b{Load32{&mb}, Store32{&mb}};
    assert(b.Initialize());
    Game g;
    Guest guest{&mb, &g};
    std::int64_t now = 100;
    for (int i = 0; i < 40; ++i) {
        guest.Call();
        now += 33;
        b.Update(now);
    }
    assert(b.status().helper_present);
    assert(b.Stage(Op::Board, now));
    assert(mb.Get(OffsetReqSeq) == 1); // 0 is never used
}

void TestShutdown() {
    Rig r;
    r.Frames(40);
    assert(r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
    const std::uint32_t seq = r.mb.Get(OffsetReqSeq);
    r.bridge.Shutdown();
    assert(r.mb.Get(OffsetModuleMagic) == 0 && r.mb.Get(OffsetCancelSeq) == seq);
    // without the module magic the guest does not touch the game at all
    r.guest.Call();
    assert(r.game.ride_calls == 0);
}

void TestStoreFailure() {
    Rig r;
    r.Frames(40);
    r.mb.fail_stores = true;
    assert(!r.bridge.Stage(Op::Board, r.now));
    assert(!r.bridge.status().busy);
    assert(!r.ctl.OnPress(r.bridge, r.button, r.now).accepted);
}

// ---- OP_SWITCH -----------------------------------------------------------------------------
using Row = Game::Row;
void ExpectRows(const std::vector<Row>& got, const std::vector<Row>& want) {
    assert(got.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) assert(got[i] == want[i]);
}

void TestSwitchLiveRecords() {
    // LIVE-CHECK-4: Zanemato -> Elma, then back; aux values from the fake 0x56bed0 table
    Rig r;
    r.Frames(40);
    assert(r.setb.enabled && r.setb.reason == Reason::None);
    assert(r.sw.OnPress(r.bridge, r.setb, 1, 11, r.now).accepted);
    assert(r.mb.Get(OffsetReqArg) == 1 && r.mb.Get(OffsetReqTarget) == MemberToken(11));
    r.guest.Call();
    ExpectRows(r.game.staged, {{0, 2, 11, 6, 2}, {1, 1, 0, 6, 1}, {2, 2, 12, 6, 3}, {3, 2, 14, 0, 0}});
    assert(r.game.commits == 1 && r.mb.Get(OffsetResultCode) == 0 && r.mb.Get(OffsetResultValue) == MemberToken(11));
    assert(r.mb.Get(OffsetSubmitToken) == MemberToken(11) && r.mb.Get(OffsetSubmitCount) == 1);
    r.now += Rig::Frame;
    r.Sample();
    assert(r.got_outcome && r.last.op == Op::Switch && r.last.code == Result::Submitted);
    // the button stays off: busy block, lockout, then two fresh polls
    assert(!r.setb.enabled);
    r.Frames(3);
    assert(!r.setb.enabled);
    r.game.idle = true; // the consumer finished (about 4 frames)
    r.Frames(60);
    assert(r.setb.enabled);                       // party is now Elma, Zanemato, Lin, Irina
    assert(SwitchController::Completed(r.bridge.status()));
    assert(r.bridge.status().slot0_token == MemberToken(11));
    assert(r.sw.OnPress(r.bridge, r.setb, 1, 0, r.now).accepted);   // back to Zanemato (slot 1)
    r.guest.Call();
    ExpectRows(r.game.staged, {{0, 1, 0, 6, 1}, {1, 2, 11, 6, 2}, {2, 2, 12, 6, 3}, {3, 2, 14, 0, 0}});
    // other slots swap the same way
    Rig q;
    q.Frames(40);
    assert(q.sw.OnPress(q.bridge, q.setb, 3, 14, q.now).accepted);
    q.guest.Call();
    ExpectRows(q.game.staged, {{0, 2, 14, 0, 0}, {1, 2, 11, 6, 2}, {2, 2, 12, 6, 3}, {3, 1, 0, 6, 1}});
}

void TestSwitchGates() {
    {   // the controlled member's row does nothing; no tap target for a missing row
        Rig r;
        r.Frames(40);
        r.sel_controlled = true;
        r.Frames(2);
        assert(!r.setb.enabled && r.setb.reason == Reason::NotNow);
        assert(!r.sw.OnPress(r.bridge, r.setb, 0, 0, r.now).accepted);
        r.sel_controlled = false;
        r.sel_present = false;
        r.Frames(2);
        assert(!r.setb.enabled);
    }
    {   // request block busy: not ready, nothing staged
        Rig r;
        r.game.idle = false;
        r.Frames(40);
        assert(!r.setb.enabled);
        assert(r.bridge.Stage(Op::Switch, r.now, 1, MemberToken(11)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 8 && r.game.commits == 0);
    }
    {   // cooldown (0xf0388 != 0): the poll says not ready; a request is refused with code 10
        Rig r;
        r.game.cooldown = true;
        r.Frames(40);
        assert(!r.setb.enabled && r.setb.reason == Reason::NotNow);
        assert(r.bridge.Stage(Op::Switch, r.now, 1, MemberToken(11)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 10 && r.game.commits == 0);
        r.now += Rig::Frame;
        r.Sample();
        assert(r.last.code == Result::Cooldown && r.last.answered);
        r.game.cooldown = false;
        r.Frames(40);
        assert(r.setb.enabled);
    }
    {   // piloting: refused (NotApplicable), button disabled
        Rig r;
        r.game.piloting = true;
        r.Frames(40);
        assert(!r.setb.enabled);
        assert(r.bridge.Stage(Op::Switch, r.now, 1, MemberToken(11)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 5 && r.game.commits == 0);
    }
    {   // transition / stale handles: no call
        Rig r;
        r.Frames(40);
        r.game.transitioning = true;
        r.game.valid = false;
        assert(r.bridge.Stage(Op::Switch, r.now, 1, MemberToken(11)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 9 && r.game.commits == 0);
    }
    {   // stale target: slot k holds another member now (the party reordered after the tap)
        Rig r;
        r.Frames(40);
        assert(r.sw.OnPress(r.bridge, r.setb, 1, 11, r.now).accepted);
        r.game.party = {11, 0, 12, 14};
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 11 && r.game.commits == 0);
        r.now += Rig::Frame;
        r.Sample();
        assert(r.last.code == Result::StaleTarget);
    }
    {   // bad slot index is never staged by the controller, and the guest refuses it as BadOp
        Rig r;
        r.Frames(40);
        assert(!r.sw.OnPress(r.bridge, r.setb, 0, 0, r.now).accepted);
        assert(!r.sw.OnPress(r.bridge, r.setb, 4, 14, r.now).accepted);
        assert(r.bridge.Stage(Op::Switch, r.now, 7, MemberToken(14)));
        r.guest.Call();
        assert(r.mb.Get(OffsetResultCode) == 2);
    }
    {   // helper absent: no tap, prototype look
        Rig r;
        r.guest.running = false;
        r.Idle(3000);
        assert(r.setb == SetButton{});
    }
    assert(MemberToken(0) == 0x100 && MemberToken(11) == 0x20B);
    const auto s = DecodeState(StateValid | StateOnFoot | StateHudOk | StateInputReady | StateFieldOk | StateSwitchReady | StateIdle | StateCooldownClear);
    assert(s.switch_ready && s.idle && s.cooldown_clear);
    assert(!DecodeState(StateValid | StateOnFoot | StateHudOk | StateInputReady | StateFieldOk | StateSwitchReady).switch_ready); // not idle
    assert(!DecodeState(StateValid | StatePiloting | StateHudOk | StateInputReady | StateFieldOk | StateSwitchReady | StateIdle | StateCooldownClear).switch_ready);
}

} // namespace

int main() {
    TestInitialize();
    TestHelperAbsentLooksLikePrototype();
    TestPresentAndFaces();
    TestContradictoryStateIgnored();
    TestStaleHeartbeat();
    TestStaleStateWindow();
    TestTransitionAndGateRequests();
    TestInputNotReady();
    TestFieldNotOk();
    TestReturnToSkell();
    TestReturnGates();
    TestInFlight();
    TestBoardRequestAck();
    TestExitRequestAndRefusals();
    TestRefusedReasonWindow();
    TestTimeoutCancel();
    TestLateAnswerAfterCancelIsStillTruthful();
    TestEpochChange();
    TestGuestStaleEpochRefusal();
    TestLockoutOnFlipOnly();
    TestSettleTimesOut();
    TestNoSkell();
    TestSeqWrap();
    TestShutdown();
    TestSwitchLiveRecords();
    TestSwitchGates();
    TestStoreFailure();
    std::puts("xcx_helper_mailbox_test: all passed");
    return 0;
}
