// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pla_select.h"
#include <cassert>
#include <iostream>
#include <vector>

namespace {
struct Game {
    std::array<uint32_t, PLA_MAILBOX_SIZE / 4> box{};
    pla::Selector s;
    pla::Party party;
    pla::TapSelect select; // destroyed before mailbox
    unsigned calls = 0;
    bool busy = false, pad = false, responsive = true, fail_store = false;
    std::vector<unsigned> writes;
    Game() {
        box[PLA_EPOCH / 4] = 7;
        s.valid = true;
        s.enabled = 1;
        s.input_state = 0;
        s.view = 0x12340000;
        s.item_count = 7;
        s.party_count = 6;
        party.valid = true;
        party.count = 6;
        for (unsigned j = 0; j < 7; ++j)
            s.items[j] = {uint16_t(1710 + j), 5, 1};
        for (unsigned j = 0; j < 6; ++j) {
            s.party[j] = 5 - j; // deliberately nonidentity physical slots
            auto &mon = party.slots[j];
            mon.valid = true;
            mon.species = 100 + j;
            mon.hp = mon.max_hp = 50;
        }
        auto io = Io();
        select.Configure(&io);
    }
    EdenDsmodHostExtensions Io() {
        EdenDsmodHostExtensions io{};
        io.version = EDEN_DSMOD_EXT_VERSION;
        io.struct_size = sizeof io;
        io.abi_hash = EDEN_DSMOD_EXT_HASH;
        io.userdata = this;
        io.mailbox_address = 0x11000000;
        io.mailbox_size = PLA_MAILBOX_SIZE;
        io.load_u32 = [](void *u, uint32_t off, uint32_t *v) -> EdenDsmodBool {
            auto &g = *static_cast<Game *>(u);
            assert(off % 4 == 0 && off < PLA_MAILBOX_SIZE);
            *v = g.box[off / 4];
            return true;
        };
        io.store_u32 = [](void *u, uint32_t off, uint32_t v) -> EdenDsmodBool {
            auto &g = *static_cast<Game *>(u);
            assert(off % 4 == 0 && off < PLA_MAILBOX_SIZE);
            g.writes.push_back(off);
            if (g.fail_store)
                return false;
            g.box[off / 4] = v;
            return true;
        };
        return io;
    }
    uint32_t &B(unsigned off) { return box[off / 4]; }
    void Frame() {
        if (B(PLA_REQ) == B(PLA_ACK))
            return;
        const auto req_view = uint64_t(B(PLA_VIEW)) | uint64_t(B(PLA_VIEW + 4)) << 32;
        unsigned result = PLA_INVALID;
        if (req_view == s.view && req_view && !(req_view & 7) && !pad &&
            B(PLA_REQ_EPOCH) == B(PLA_EPOCH) && B(PLA_EPOCH) && B(PLA_CANCEL) != B(PLA_REQ) &&
            B(PLA_MODE) == s.mode && B(PLA_ITEM) == s.item_index && B(PLA_PARTY) == s.party_index &&
            B(PLA_ITEMS) == s.item_count && B(PLA_PARTIES) == s.party_count) {
            result = PLA_BUSY;
            if (!busy && !s.animation && s.input_state != 2 && s.enabled && B(PLA_OP) >= 1 &&
                B(PLA_OP) <= 3) {
                ++calls;
                result = PLA_INVALID; // called but no synchronous progress: never retry
                if (responsive) {
                    if (B(PLA_OP) == PLA_TOGGLE)
                        s.mode ^= 1;
                    else {
                        auto &index = s.mode == 0 ? s.item_index : s.party_index;
                        const auto count = s.mode == 0 ? s.item_count : s.party_count;
                        index = B(PLA_OP) == PLA_NEXT ? (index + 1) % count
                                                      : (index + count - 1) % count;
                    }
                    result = PLA_DONE;
                }
            }
        }
        B(PLA_RESULT) = result;
        B(PLA_POST_MODE) = s.mode;
        B(PLA_POST_ITEM) = s.item_index;
        B(PLA_POST_PARTY) = s.party_index;
        B(PLA_ACK) = B(PLA_REQ);
    }
    bool Tap(unsigned mode, unsigned index, int64_t now = 0, bool allowed = true) {
        return select.Start(s, party, mode, index, allowed, now);
    }
    void Tick(int64_t now, bool allowed = true) { select.Step(s, party, allowed, now); }
};
void Reach() {
    Game g;
    assert(g.Tap(0, 3));
    assert(g.select.Pending(0, 3));
    g.Tick(0);
    assert(g.writes.back() == PLA_REQ); // payload published last
    assert(g.B(PLA_OP) == PLA_NEXT);
    assert(!g.Tap(0, 4)); // never mutate an in-flight request
    for (int n = 1; n <= 3; ++n) {
        g.Frame();
        assert(g.s.item_index == unsigned(n));
        const auto calls = g.calls;
        g.Frame();
        assert(g.calls == calls); // ack prevents reentry
        g.Tick(n * 30);
    }
    assert(!g.select.Active() && g.calls == 3);
    assert(g.Tap(0, 6, 100)); // shortest route wraps backwards from 3: 3 next vs 4 prev
    g.Tick(100);
    assert(g.B(PLA_OP) == PLA_NEXT);
    for (int n = 0; n < 3; ++n) {
        g.Frame();
        g.Tick(130 + n * 30);
    }
    assert(g.s.item_index == 6 && !g.select.Active());
    assert(g.Tap(0, 0, 300));
    g.Tick(300);
    assert(g.B(PLA_OP) == PLA_NEXT);
    g.Frame();
    g.Tick(330);
    assert(g.s.item_index == 0 && !g.select.Active());
    assert(g.Tap(0, 6, 400));
    g.Tick(400);
    assert(g.B(PLA_OP) == PLA_PREVIOUS);
    g.Frame();
    g.Tick(430);
    assert(g.s.item_index == 6 && !g.select.Active());
}
void ToggleAndBusy() {
    Game g;
    assert(g.Tap(1, 4));
    g.Tick(0);
    assert(g.B(PLA_OP) == PLA_TOGGLE);
    g.busy = true;
    g.Frame();
    g.Tick(30);
    assert(g.calls == 0 && g.select.Active());
    g.busy = false;
    g.Frame();
    g.Tick(60);
    assert(g.s.mode == 1 && g.B(PLA_OP) == PLA_PREVIOUS);
    g.Frame();
    g.Tick(90);
    g.Frame();
    g.Tick(120);
    assert(g.s.party_index == 4 && g.s.party[g.s.party_index] == 1 && !g.select.Active());
    Game animation;
    animation.s.animation = 3;
    assert(animation.Tap(0, 2));
    animation.Tick(0);
    assert(animation.B(PLA_REQ) == animation.B(PLA_ACK));
    animation.s.animation = 0;
    animation.Tick(30);
    animation.Frame();
    animation.Tick(60);
    assert(animation.s.item_index == 1);
}
void TimeoutsAndInterference() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Game g;
        assert(g.Tap(0, 3));
        g.Tick(0);
        if (mode == 1) {
            g.busy = true;
            g.Frame();
            g.Tick(100);
        }
        if (mode == 2) {
            g.responsive = false;
            g.Frame();
            g.Tick(100);
        }
        g.Tick(2000);
        assert(!g.select.Active());
        g.busy = false;
        g.Frame();
        assert(g.calls == (mode == 2 ? 1u : 0u)); // cancelled late delivery
    }
    Game watchdog;
    assert(watchdog.Tap(0, 3));
    watchdog.Tick(0);
    watchdog.select.Watchdog(2000);
    watchdog.Frame();
    assert(watchdog.calls == 0);
    Game g;
    assert(g.Tap(0, 3));
    g.Tick(0);
    g.Frame();
    g.Tick(50);
    g.s.item_index = 5;
    g.Tick(60);
    assert(!g.select.Active());
    g.Frame();
    assert(g.calls == 1);
    Game pad;
    assert(pad.Tap(0, 3));
    pad.Tick(0);
    pad.pad = true;
    pad.Frame();
    pad.Tick(30);
    assert(!pad.select.Active() && pad.calls == 0);
    Game hidden;
    assert(hidden.Tap(0, 3));
    hidden.Tick(0);
    hidden.s.valid = false;
    hidden.Tick(30);
    hidden.Frame();
    assert(hidden.calls == 0);
}
void GuardsAndFailures() {
    Game g;
    assert(!g.Tap(0, 3, 0, false)); // wrong build / invalid position / disabled setting
    g.select.Configure(nullptr);
    assert(!g.Tap(0, 3));
    auto io = g.Io();
    io.mailbox_size = 0;
    g.select.Configure(&io);
    assert(!g.Tap(0, 3));
    io = g.Io();
    g.select.Configure(&io);
    g.s.items[3].count = 0;
    assert(!g.Tap(0, 3));
    g.s.items[3].count = 5;
    g.party.slots[g.s.party[3]].fainted = true;
    assert(!g.Tap(1, 3));
    g.party.slots[g.s.party[3]].fainted = false;
    g.party.slots[g.s.party[3]].hp = 0;
    assert(!g.Tap(1, 3));
    g.s.input_state = 2;
    assert(!g.Tap(0, 3));
    g.s.input_state = 0;
    assert(g.Tap(0, 3));
    g.Tick(0);
    g.s.items[3].count = 0;
    g.Tick(30);
    g.Frame();
    assert(g.calls == 0);
    Game removed;
    assert(removed.Tap(0, 3));
    removed.Tick(0);
    removed.s.items[3].id = 99;
    removed.Tick(30);
    removed.Frame();
    assert(removed.calls == 0);
    Game faint;
    assert(faint.Tap(1, 3));
    faint.Tick(0);
    faint.party.slots[2].fainted = true;
    faint.Tick(30);
    faint.Frame();
    assert(faint.calls == 0);
    Game setting;
    assert(setting.Tap(0, 3));
    setting.Tick(0);
    setting.Tick(30, false);
    setting.Frame();
    assert(setting.calls == 0);
    Game fail;
    assert(fail.Tap(0, 3));
    fail.fail_store = true;
    fail.Tick(0);
    assert(fail.B(PLA_REQ) == fail.B(PLA_ACK) && !fail.select.Active());
    Game epoch;
    assert(epoch.Tap(0, 3));
    epoch.Tick(0);
    ++epoch.B(PLA_EPOCH);
    epoch.Frame();
    epoch.Tick(30);
    assert(!epoch.select.Active() && epoch.calls == 0);
    Game stale;
    assert(stale.Tap(0, 3));
    stale.Tick(0);
    stale.B(PLA_VIEW) += 8;
    stale.Frame();
    stale.Tick(30);
    assert(!stale.select.Active() && stale.calls == 0);
    Game delayed;
    assert(delayed.Tap(0, 3));
    delayed.Tick(0);
    auto old = delayed.s;
    delayed.Frame();
    delayed.select.Step(old, delayed.party, true, 30);
    assert(delayed.B(PLA_REQ) == delayed.B(PLA_ACK)); // ack without sampled change cannot stage
    delayed.Tick(60);
    assert(delayed.B(PLA_REQ) != delayed.B(PLA_ACK));
    Game reset;
    assert(reset.Tap(0, 3));
    reset.Tick(0);
    auto ext = reset.Io();
    reset.select.Configure(&ext);
    assert(!reset.select.Active());
    reset.Frame();
    assert(reset.calls == 0);
}
} // namespace
int main() {
    Reach();
    ToggleAndBusy();
    TimeoutsAndInterference();
    GuardsAndFailures();
    std::cout << "PLA mailbox/state machine simulation passed\n";
}
