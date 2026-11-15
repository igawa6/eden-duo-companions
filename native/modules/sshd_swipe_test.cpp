// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// dev.45 Ready swipe pad outcomes (Thor dev.44, owner save at Hylia's Realm: 2 of 11 swipes with the L
// lock on ended with "aux gesture on 'stickpad_0' ended" and then neither "stick sequence N staged" nor
// "refused swipe"). Every deliberate stroke must end in exactly one of: "swipe N fed (...)" (it
// reached the stick long enough), "swipe N flick replays / waits" followed by "stick sequence M
// staged" (or that flick's own "refused swipe"), or one "refused swipe (<why>: stroke N ...)".
// The runtime side is simulated per tick: the pad's pan (glides home a fifth of the way after the
// lift), the guest helper's L hold (mailbox +0x20: the stick hook ORs L into the game's word and
// the +0x14 copy while it counts; the loop hook counts it down once per game frame) and its stick
// sequences (6 frames each), with a game frame every tick (desktop) or every other tick (a handheld
// at 30 fps).
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sshd_menu_fixture.h"
#include "sshd_sword_fixture.h"

namespace {
constexpr uint64_t kSlot = 0x5AEAD54, kScene = 0x182A7F4, kEvents = 0x182DED8, kEventObj = 0x7000000,
                   kPlayerPtr = 0x2BF9940;
struct State {
    std::string stage = "F402"; // the Sealed Grounds statue (desktop hero label); the Thor: F403
    uint32_t event_state = 0;
    uint8_t menu = 0;
    uint8_t items[0x20]{};
    uint64_t player = 0x7100000;
    uint16_t held = 0x11; // the sword drawn (both Thor refusal lines: held 0x11)
    double pan_x = 0, pan_y = 0;
    uint32_t mailbox[1024]{};
    std::map<std::string, int64_t> ints;
    std::vector<std::string> logs;
    bool physical_l = false;
};
template <typename T> bool Put(void* out, size_t n, const T& v) {
    if (n != sizeof(T)) return false;
    std::memcpy(out, &v, n);
    return true;
}
EdenDsmodBool Read(void* p, uint64_t a, void* out, size_t n) {
    auto& s = *static_cast<State*>(p);
    if (sshd_test::SwordFixtureRead(0, s.items, a, out, n)) return EDEN_DSMOD_TRUE;
    if (a == 0x1826AD0) return Put(out, n, uint64_t{0});
    if (a == kSlot + 0x531C && n == 8) {
        std::memset(out, 0, 8);
        std::memcpy(out, s.stage.data(), s.stage.size());
        return EDEN_DSMOD_TRUE;
    }
    if (a == kSlot + 0x531C && n == 1) return Put(out, n, static_cast<uint8_t>(s.stage.empty() ? 0 : s.stage[0]));
    if (a == kScene) return Put(out, n, uint32_t{0});
    if (sshd_test::MenuFixtureRead(0, s.menu, a, out, n)) return EDEN_DSMOD_TRUE;
    if (a == kEvents) return Put(out, n, kEventObj);
    if (a == kEventObj + 0x248) return Put(out, n, s.event_state);
    if (a == kPlayerPtr) return Put(out, n, s.player);
    if (s.player != 0 && a == s.player + 0x6404) return Put(out, n, s.held);
    if (s.player != 0 && a == s.player + 0x6408) return Put(out, n, static_cast<uint16_t>(1));
    return EDEN_DSMOD_FALSE;
}

struct Run {
    State s;
    EdenDsmodHostApi h{};
    EdenDsmodHostExtensions e{};
    const EdenDsmodModuleApi* api = nullptr;
    const EdenDsmodModuleExtensions* ext = nullptr;
    void* inst = nullptr;
    int frame_every = 1; // a game frame every n ticks
    long ticks = 0;
    int seq_frames = 0;  // frames the helper has played of the current sequence
    bool seq_stall = false; // the helper does not take a published sequence yet
    std::vector<int> l_frames; // per game frame: L in the game's button word (1) or not
    std::vector<std::array<int32_t, 2>> stick; // per game frame: the stick the game read

    Run() {
        api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
        ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
        assert(api && ext);
        h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION; h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        h.struct_size = sizeof(h); h.title_id = 0x01002DA013484000ull;
        const uint8_t build[] = {0x54,0x41,0xAF,0x3C,0x89,0xF9,0x7E,0x1D,0x25,0xA8,
                                 0xCC,0xF1,0x4C,0x74,0x2B,0xF8,0x23,0x36,0xD8,0xA8};
        std::memcpy(h.build_id, build, sizeof(build));
        h.userdata = &s;
        h.read_memory = Read;
        h.publish_i64 = [](void* p, const char* k, int64_t v) { static_cast<State*>(p)->ints[k] = v; };
        h.publish_f64 = [](void*, const char*, double) {};
        h.publish_text = [](void*, const char*, const char*) {};
        h.log = [](void* p, uint32_t, const char* m) { static_cast<State*>(p)->logs.emplace_back(m); };
        h.get_f64 = [](void* p, const char* k, double fb) -> double {
            auto& st = *static_cast<State*>(p);
            if (std::strcmp(k, "@view_pan_x:stickpad_0") == 0) return st.pan_x;
            if (std::strcmp(k, "@view_pan_y:stickpad_0") == 0) return st.pan_y;
            return fb;
        };
        inst = api->create(&h, "");
        assert(inst);
        e.version = EDEN_DSMOD_EXT_VERSION; e.abi_hash = EDEN_DSMOD_EXT_HASH; e.struct_size = sizeof(e);
        e.userdata = &s; e.mailbox_address = 0x9000000; e.mailbox_size = 4096;
        e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<State*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
        e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool { static_cast<State*>(p)->mailbox[o / 4] = v; return EDEN_DSMOD_TRUE; };
        s.items[0x9E5 - 0x9E0] = 1; // Goddess Sword
        ext->configure(inst, &e);
    }
    ~Run() { api->destroy(inst); }
    uint32_t& mb(uint32_t off) { return s.mailbox[off / 4]; }

    // One game frame of the helper (sshd_stick_helper.S): the stick hook (heartbeat, the npad copy with
    // the L hold ORed in, the stick words or a sequence step) and the loop hook (heartbeat, the menu
    // sample, the L-hold countdown, the sequence player).
    void Frame() {
        ++mb(0x04);
        const bool l_hold = mb(0x20) != 0;
        const bool game_l = s.physical_l || l_hold;
        mb(0x14) = game_l ? 0x40u : 0u;
        l_frames.push_back(game_l ? 1 : 0);
        int32_t x = 0, y = 0;
        if (mb(0x08) != 0) {
            --mb(0x08);
            x = static_cast<int32_t>(mb(0x0C)), y = static_cast<int32_t>(mb(0x10));
        }
        stick.push_back({x, y});
        ++mb(0x68);
        sshd_test::MenuHelperSample(s.mailbox, s.menu != 0 ? 1 : 0, s.menu != 0 ? 0x148 : 0);
        if (mb(0x20) != 0) --mb(0x20);
        // a sequence: picked up the frame after it is published, 6 frames, then answered
        if (seq_stall) {
        } else if (mb(0x90) == 1) {
            if (++seq_frames >= 6) {
                mb(0x90) = 0; mb(0x08) = 0;
                mb(0xA4) = 0; mb(0xA0) = mb(0x9C);
            } else {
                const uint32_t step = seq_frames < 4 ? 0 : 1;
                mb(0x0C) = static_cast<uint32_t>(static_cast<int16_t>(mb(0xC0 + 8 * step) & 0xFFFF));
                mb(0x10) = static_cast<uint32_t>(static_cast<int16_t>(mb(0xC0 + 8 * step) >> 16));
                mb(0x08) = 8;
            }
        } else if (mb(0x80) != mb(0xA0)) {
            mb(0x9C) = mb(0x80); mb(0x90) = 1; seq_frames = 0;
            mb(0x0C) = static_cast<uint32_t>(static_cast<int16_t>(mb(0xC0) & 0xFFFF));
            mb(0x10) = static_cast<uint32_t>(static_cast<int16_t>(mb(0xC0) >> 16));
            mb(0x08) = 8;
        }
    }
    void Tick() {
        if (ticks % frame_every == 0) Frame();
        ++ticks;
        api->sample(inst, &h);
        api->tick(inst, &h);
    }
    void Ticks(int n) { for (int i = 0; i < n; ++i) Tick(); }
    bool Act(const char* a) { return ext->on_action(inst, a, 0) == EDEN_DSMOD_TRUE; }
    // a finger stroke of `ticks` ticks covering (dx, dy) canvas px (right / down positive), then the lift
    void Stroke(double dx, double dy, int t) {
        for (int i = 1; i <= t; ++i) {
            s.pan_x = -dx * i / t; s.pan_y = -dy * i / t;
            Tick();
        }
        while (std::fabs(s.pan_x) + std::fabs(s.pan_y) > 0.5) { s.pan_x *= 0.8; s.pan_y *= 0.8; Tick(); }
        s.pan_x = s.pan_y = 0;
        Tick();
    }
    size_t Count(const std::string& what, size_t from = 0) const {
        size_t n = 0;
        for (size_t i = from; i < s.logs.size(); ++i) n += s.logs[i].find(what) != std::string::npos;
        return n;
    }
};

// The stroke's outcome from the lines it logged (from `from` on): 'F' fed, 'S' its flick staged,
// 'R' refused, '?' nothing (the dev.44 silent drop), '2' more than one.
char Outcome(const Run& r, size_t from, unsigned id, unsigned seq_before) {
    const std::string sid = std::to_string(id);
    const size_t fed = r.Count("sshd: swipe " + sid + " fed (", from);
    const size_t replay = r.Count("sshd: swipe " + sid + " flick replays", from) +
                          r.Count("sshd: swipe " + sid + " fed only ", from) +
                          r.Count("sshd: swipe " + sid + " flick waits", from);
    size_t refused = 0;
    for (size_t i = from; i < r.s.logs.size(); ++i) {
        const auto& l = r.s.logs[i];
        refused += l.find("sshd: refused swipe (") == 0 &&
                   (l.find(": stroke " + sid + ",") != std::string::npos || l.find(": stroke " + sid + " ") != std::string::npos);
    }
    const size_t staged = r.Count("sshd: stick sequence " + std::to_string(seq_before + 1) + " staged", from);
    if (fed + replay + refused == 0) return '?';
    if (fed == 1 && replay == 0 && refused == 0) return 'F';
    if (replay == 1 && fed == 0 && refused == 0 && staged == 1) return 'S';
    if (refused == 1 && fed == 0) return 'R'; // (a replayed flick refused at staging logs both)
    return '2';
}

unsigned StrokeId(const Run& r) {
    for (auto it = r.s.logs.rbegin(); it != r.s.logs.rend(); ++it) {
        unsigned id = 0;
        if (std::sscanf(it->c_str(), "sshd: swipe %u", &id) == 1) return id;
        const auto p = it->find(": stroke ");
        if (it->find("sshd: refused swipe") == 0 && p != std::string::npos && std::sscanf(it->c_str() + p + 9, "%u", &id) == 1)
            return id;
    }
    return 0;
}

} // namespace

int main() {
    for (const int frame_every : {1, 2}) { // desktop 60 fps / a handheld at 30 fps
        // --- 1. the statue, the L lock on, horizontal and vertical strokes 6..40 ticks long (the Thor's
        //        were 13..17 ticks from the lock letting go to the lift; the drops were the 16 / 17 tick
        //        ones): every stroke reaches the stick or logs one refusal, and its deflection is long
        //        enough for a slash (a flick of 4 frames, or a fed stroke of kLateFed ticks or more)
        Run r;
        r.frame_every = frame_every;
        r.Ticks(5);
        assert(r.Act("l_toggle"));
        r.Ticks(30);
        assert(r.s.ints.at("sshd.l.hold") == 1 && r.mb(0x20) > 0 && r.mb(0x14) == 0x40);
        unsigned strokes = 0;
        for (int t = 6; t <= 40; ++t) {
            for (const bool vertical : {false, true}) {
                assert(r.s.ints.at("sshd.l.hold") == 1);
                const size_t from = r.s.logs.size();
                const unsigned seq_before = r.mb(0x80);
                const size_t f0 = r.stick.size();
                if (vertical) r.Stroke(0, -320, t); else r.Stroke(268, 0, t);
                r.Ticks(40); // the flick plays, the lock re-engages
                const unsigned id = StrokeId(r);
                assert(id == ++strokes);
                const char o = Outcome(r, from, id, seq_before);
                if (o != 'F' && o != 'S') {
                    std::fprintf(stderr, "frame_every %d stroke %d ticks %s: outcome %c\n", frame_every, t,
                                 vertical ? "v" : "h", o);
                    for (size_t i = from; i < r.s.logs.size(); ++i) std::fprintf(stderr, "  %s\n", r.s.logs[i].c_str());
                    assert(false);
                }
                // what the game read: a full deflection for at least 3 frames in a row, with L up
                int run = 0, best = 0;
                for (size_t i = f0; i < r.stick.size(); ++i) {
                    const double m = std::hypot(r.stick[i][0], r.stick[i][1]) / 32767.0;
                    run = m > 0.95 && r.l_frames[i] == 0 ? run + 1 : 0;
                    best = std::max(best, run);
                }
                assert(best >= 3);
            }
        }
        assert(r.Count("sshd: refused") == 0);
        assert(r.Count("pressed virtual button") == 0); // (the host's line; the module never presses L)

        // --- 2. strokes within ~1 s after the pause menu / the map closes (the close debounce, then the
        //        lock's re-engage), started every 3 ticks from the close on: fed, replayed or refused,
        //        never silent; while the menu is open: one refusal each
        for (int start = 0; start <= 66; start += 3) {
            r.s.menu = 1;
            r.Ticks(10);
            assert(r.s.ints.at("sshd.menu.open") == 1 && r.s.ints.at("sshd.l.hold") == 0 && r.mb(0x20) == 0);
            size_t from = r.s.logs.size();
            unsigned seq_before = r.mb(0x80);
            r.Stroke(268, 0, 15);
            unsigned id = StrokeId(r);
            assert(id == ++strokes && Outcome(r, from, id, seq_before) == 'R');
            assert(r.Count("refused swipe (menu: stroke " + std::to_string(id), from) == 1);
            r.s.menu = 0; // closed
            r.Ticks(start);
            from = r.s.logs.size();
            seq_before = r.mb(0x80);
            r.Stroke(0, -333, 16);
            r.Ticks(40);
            id = StrokeId(r);
            assert(id == ++strokes);
            const char o = Outcome(r, from, id, seq_before);
            if (o != 'F' && o != 'S' && o != 'R') {
                std::fprintf(stderr, "after close +%d ticks: outcome %c\n", start, o);
                for (size_t i = from; i < r.s.logs.size(); ++i) std::fprintf(stderr, "  %s\n", r.s.logs[i].c_str());
                assert(false);
            }
            r.Ticks(30);
            assert(r.s.ints.at("sshd.l.hold") == 1); // re-engaged
        }
    }

    {
        // --- 3. a stroke that runs while another sequence owns the stick (a spin, or the last
        //        stroke's flick still waiting for L): its flick waits and plays after it (dev.38..44
        //        dropped it: no peak was kept, and the replay needed a free stick at the lift)
        Run r;
        r.Ticks(40);
        assert(r.Act("spin_h"));
        r.Tick();
        const unsigned spin = r.mb(0x80);
        assert(spin != 0 && r.mb(0x84) == 6);
        r.seq_stall = true; // the helper is slow to take it (the sequence stays in flight)
        const size_t from = r.s.logs.size();
        r.Stroke(268, 0, 8);
        assert(r.Count("flick waits for the stick", from) == 1);
        r.seq_stall = false;
        r.Ticks(60);
        assert(r.mb(0x80) == spin + 1 && r.Count("stick sequence " + std::to_string(spin + 1) + " staged", from) == 1);
        assert(static_cast<int16_t>(r.mb(0xC0) & 0xFFFF) == 32767);
        assert(r.Count("sshd: refused", from) == 0);

        // --- 4. the flick of a stroke that never reached the stick, refused when it would play (an
        //        item in Link's hands by then): one "refused swipe" line (dev.24..44: silent)
        assert(r.Act("l_toggle"));
        r.Ticks(30);
        size_t f2 = r.s.logs.size();
        r.s.held = 0x4; // the slingshot (the gate words are re-read every sample)
        r.Stroke(268, 0, 6);
        r.Ticks(20);
        assert(r.Count("flick replays", f2) == 1);
        assert(r.Count("refused swipe (item in hand: stroke ", f2) == 1 && r.Count("late flick", f2) == 1);
        assert(r.s.ints.at("sshd.swipe.refused") == 1);
        r.s.held = 0x11;
        r.Ticks(80);

        // --- 5. the helper refuses a late flick on the game thread (an event began): "refused swipe"
        f2 = r.s.logs.size();
        r.seq_stall = true; // hold the next request in flight
        r.Stroke(268, 0, 6);
        r.Ticks(20);
        const unsigned flick = r.mb(0x80);
        assert(r.Count("stick sequence " + std::to_string(flick) + " staged", f2) == 1);
        r.mb(0xA4) = 5; r.mb(0xA0) = flick; r.seq_stall = false; // answered: 5 = event
        r.Ticks(2);
        assert(r.Count("refused swipe (event: stroke ", f2) == 1);
        r.Ticks(80);

        // --- 6. a too-short stroke with the lock on (never fed, peak under kFlickMin): one line, no
        //        "Not now" (nothing the game refused)
        f2 = r.s.logs.size();
        r.Stroke(20, 0, 6); // 20 px: no flick boost (28 px in 3 ticks), peak 0.11
        r.Ticks(10);
        assert(r.Count("refused swipe (too short: stroke ", f2) == 1);

        // --- 7. physical L while the lock is off reads as the game's L (GameL), and the helper's hold
        //        word stays 0: nothing virtual is held
        assert(r.Act("l_toggle"));
        r.Ticks(5);
        assert(r.s.ints.at("sshd.l.on") == 0 && r.mb(0x20) == 0);
        r.s.physical_l = true;
        r.Ticks(3);
        assert(r.s.ints.at("sshd.l.game") == 1 && r.s.ints.at("sshd.l.eye") == 1 && r.mb(0x20) == 0);
        r.s.physical_l = false;
        r.Ticks(3);
        assert(r.s.ints.at("sshd.l.game") == 0);

        // --- 8. the lock on, then the module stops (no more ticks): the helper's hold lapses by
        //        itself within kLHoldFrames (8) frames
        assert(r.Act("l_toggle"));
        r.Ticks(30);
        assert(r.mb(0x20) > 0);
        int frames = 0;
        while (r.mb(0x20) != 0 && frames < 100) { r.Frame(); ++frames; }
        assert(frames <= 8);
        r.Frame();
        assert(r.mb(0x14) == 0); // the game sees L up
    }
    std::printf("sshd swipe outcomes ok\n");
    return 0;
}
