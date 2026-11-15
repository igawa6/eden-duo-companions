// SPDX-License-Identifier: GPL-3.0-or-later
// Ready L corner (owner 2026-11-11; dev.37 owner Thor feedback 2026-11-12): the icon follows the
// game's L while unlocked (sshd.l.eye), a tap locks (dev.45: the module refreshes the helper's L-hold
// word, mailbox +0x20, and publishes sshd.l.hold; dev.37..44 that was a runtime enforce gate) and another unlocks, the lock persists across doors / room loads / menus / events /
// flight / Silent Realms (paused, then re-engaged) and ends only on a tap, the title or a host
// reload; the stroke that lets the virtual L go and waits for the game to see it up, the late-stroke
// flick, the spin queued behind the release, the shield bash request (virtual L3 via the
// helper), and the HUD ammo count (base container +
// the pouch's ammo containers; g0 file 1 = 15 + Small Seed Satchel 9 = 24, hero file 1 = 20).
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sshd_menu_fixture.h"
#include "sshd_sword_fixture.h"

namespace {
constexpr uint64_t kSlot = 0x5AEAD54, kScene = 0x182A7F4, kEvents = 0x182DED8,
                   kEventObj = 0x7000000, kPlayerPtr = 0x2BF9940;
struct State {
    std::string stage = "F000";
    uint32_t scene = 0, event_state = 0;
    uint8_t menu = 0;
    uint8_t items[0x20]{};   // slot+0x9E0.. item flags
    uint16_t pouch[16]{};    // slot+0x7C0: (id, count) x8
    uint32_t ammo = 0;       // slot+0xA60
    uint8_t equipped = 4;    // slot+0x53AE: slingshot
    uint8_t shield_slot = 8; // slot+0x53B1: the equipped shield's pouch slot (8 none)
    uint64_t player = 0x7100000;
    uint16_t held = 0xB;
    double pan_x = 0, pan_y = 0; // stick pad 0
    uint32_t mailbox[1024]{};
    std::map<std::string, int64_t> ints;
    std::vector<std::string> logs; // dev.43: the module's log lines
};
template <typename T> bool Put(void* out, size_t n, const T& v) {
    if (n != sizeof(T)) return false;
    std::memcpy(out, &v, n);
    return true;
}
EdenDsmodBool Read(void* p, uint64_t a, void* out, size_t n) {
    auto& s = *static_cast<State*>(p);
    // dev.33: the sword reader's item-flag path over s.items; Link's water state (no player object)
    if (sshd_test::SwordFixtureRead(0, s.items, a, out, n)) return EDEN_DSMOD_TRUE;
    if (a == 0x1826AD0) return Put(out, n, uint64_t{0});
    if (a == kSlot + 0x531C && n == 8) {
        std::memset(out, 0, 8);
        std::memcpy(out, s.stage.data(), s.stage.size());
        return EDEN_DSMOD_TRUE;
    }
    if (a == kSlot + 0x531C && n == 1) return Put(out, n, static_cast<uint8_t>(s.stage.empty() ? 0 : s.stage[0]));
    if (a == kSlot + 0x9E0 && n == sizeof(s.items)) return std::memcpy(out, s.items, n), EDEN_DSMOD_TRUE;
    if (a == kSlot + 0x7C0 && n == sizeof(s.pouch)) return std::memcpy(out, s.pouch, n), EDEN_DSMOD_TRUE;
    if (a == kSlot + 0xA60) return Put(out, n, s.ammo);
    if (a == kSlot + 0x53AE) return Put(out, n, s.equipped);
    if (a == kSlot + 0x53B1) return Put(out, n, s.shield_slot);
    if (a >= kSlot + 0x7C0 && a < kSlot + 0x7E0 && n == 4) return std::memcpy(out, &s.pouch[(a - kSlot - 0x7C0) / 2], 4), EDEN_DSMOD_TRUE;
    if (a == kScene) return Put(out, n, s.scene);
    if (sshd_test::MenuFixtureRead(0, s.menu, a, out, n)) return EDEN_DSMOD_TRUE; // dev.44: the controller state
    if (a == kEvents) return Put(out, n, kEventObj);
    if (a == kEventObj + 0x248) return Put(out, n, s.event_state);
    if (a == kPlayerPtr) return Put(out, n, s.player);
    if (s.player != 0 && a == s.player + 0x6404) return Put(out, n, s.held);
    if (s.player != 0 && a == s.player + 0x6408) return Put(out, n, static_cast<uint16_t>(1));
    return EDEN_DSMOD_FALSE;
}
} // namespace

int main() {
    State s;
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext);
    EdenDsmodHostApi h{};
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
    void* inst = api->create(&h, "");
    assert(inst);
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION; e.abi_hash = EDEN_DSMOD_EXT_HASH; e.struct_size = sizeof(e);
    e.userdata = &s; e.mailbox_address = 0x9000000; e.mailbox_size = 4096;
    e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<State*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool { static_cast<State*>(p)->mailbox[o / 4] = v; return EDEN_DSMOD_TRUE; };
    auto& mb = s.mailbox;
    s.items[0x9E5 - 0x9E0] = 1; // Goddess Sword
    ext->configure(inst, &e);
    auto& ints = s.ints;
    // one runtime tick: the helper's heartbeats move, sample, then tick (StickMirror + LTick)
    auto tick = [&] { ++mb[0x04 / 4]; ++mb[0x68 / 4]; api->sample(inst, &h); api->tick(inst, &h); };
    auto act = [&](const char* a) { return ext->on_action(inst, a, 0) == EDEN_DSMOD_TRUE; };
    auto game_l = [&](bool on) { mb[0x14 / 4] = on ? 0x40u : 0u; };
    auto answer = [&](uint32_t code) { mb[0xA4 / 4] = code; mb[0xA0 / 4] = mb[0x80 / 4]; };
    auto lift = [&] { while (std::fabs(s.pan_x) + std::fabs(s.pan_y) > 0.5) { s.pan_x *= 0.8; s.pan_y *= 0.8; tick(); } s.pan_x = s.pan_y = 0; tick(); };

    // --- HUD ammo: base container + pouch containers (Item Check stock never counts)
    s.ammo = 0x80000780; // g0 file 1: base seeds 15
    s.pouch[0] = 119; s.pouch[1] = 24; s.pouch[2] = 78; s.pouch[3] = 1; s.pouch[6] = 128; s.pouch[7] = 9;
    tick();
    assert(ints.at("sshd.ammo.seeds") == 24);
    assert(ints.at("sshd.sc0.on") == 1 && ints.at("sshd.sc0.nd0") == 2 && ints.at("sshd.sc0.nd1") == 4);
    s.ammo = 0xc2140a00; std::memset(s.pouch, 0, sizeof(s.pouch)); // hero file 1: 20, satchel in stock
    tick();
    assert(ints.at("sshd.ammo.seeds") == 20 && ints.at("sshd.sc0.nd0") == 2 && ints.at("sshd.sc0.nd1") == 0);
    s.pouch[4] = 129; s.pouch[5] = 1000; // an implausible container count is ignored
    tick();
    assert(ints.at("sshd.ammo.seeds") == 20);
    std::memset(s.pouch, 0, sizeof(s.pouch));

    // --- unlocked (default): the icon follows the game's L (eye while held, else the sword); nothing
    //     is pressed
    tick();
    assert(ints.at("sshd.l.on") == 0 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.eye") == 0);
    game_l(true); tick();
    assert(ints.at("sshd.l.eye") == 1 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.on") == 0);
    game_l(false); tick();
    assert(ints.at("sshd.l.eye") == 0);

    // --- a tap locks anywhere but the title and a stopped helper; a lock tapped in a menu / event /
    //     flight / Silent Realm waits (paused) and engages kLReengage (20) ticks after free play
    s.scene = 0x53656C74; tick(); assert(!act("l_toggle") && ints.at("sshd.l.on") == 0); s.scene = 0;
    { // dev.43: the refused tap logs one line
        int n = 0;
        for (const auto& l : s.logs) n += l.find("sshd: refused l_toggle (title: lblocked 1): held") != std::string::npos;
        assert(n == 1);
    }
    tick();
    assert(ints.at("sshd.l.reason") == 1);
    struct Pause { int reason; void (*set)(State&); void (*reset)(State&); };
    const Pause pauses[] = {
        {2, [](State& st) { st.menu = 0x10; }, [](State& st) { st.menu = 0; }},
        {3, [](State& st) { st.event_state = 2; }, [](State& st) { st.event_state = 0; }},
        {4, [](State& st) { st.stage = "F020"; }, [](State& st) { st.stage = "F000"; }},
        {5, [](State& st) { st.stage = "S100"; }, [](State& st) { st.stage = "F000"; }},
        {9, [](State& st) { st.stage = ""; }, [](State& st) { st.stage = "F000"; }},
    };
    for (const auto& c : pauses) {
        c.set(s);
        for (int i = 0; i < 4; ++i) tick();
        assert(act("l_toggle"));
        tick();
        assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.eye") == 1 && ints.at("sshd.l.hold") == 0);
        assert(ints.at("sshd.l.reason") == c.reason);
        c.reset(s);
        // dev.40: a menu counts as closed after two zero samples of the byte (MenuGate), one more
        const int closing = c.reason == 2 ? 1 : 0;
        for (int i = 0; i < 19 + closing; ++i) { tick(); assert(ints.at("sshd.l.hold") == 0); }
        tick();
        assert(ints.at("sshd.l.hold") == 1 && ints.at("sshd.l.on") == 1);
        assert(act("l_toggle")); // unlock
        tick();
        assert(ints.at("sshd.l.on") == 0 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.reason") == 0);
    }

    // --- locked: the gate goes up at once; the game sees L within the confirmation window
    assert(act("l_toggle"));
    tick();
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 1 && ints.at("sshd.l.game") == 0);
    assert(ints.at("sshd.l.eye") == 1);
    // dev.45: the helper holds L while the module refreshes the L-hold word (kLHoldFrames = 8)
    assert(mb[0x20 / 4] == 8);
    mb[0x20 / 4] = 3; tick(); assert(mb[0x20 / 4] == 8); // the loop hook counted it down: refreshed
    for (int i = 0; i < 4; ++i) tick();
    game_l(true);
    for (int i = 0; i < 200; ++i) tick(); // persists (the module keeps refreshing the helper's L-hold word)
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 1 && ints.at("sshd.l.game") == 1);
    // tap again: unlocked (dev.45: the L-hold word is zeroed, the helper lets go at its next stick
    // pass); the icon follows L again
    assert(act("l_toggle"));
    tick();
    assert(ints.at("sshd.l.on") == 0 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.reason") == 0);
    assert(mb[0x20 / 4] == 0);
    assert(ints.at("sshd.l.eye") == 1); // the game still sees the last virtual press
    game_l(false);
    tick();
    assert(ints.at("sshd.l.eye") == 0);

    // --- dev.37: a lock the game has not seen yet stays locked (only a tap unlocks)
    assert(act("l_toggle"));
    for (int i = 0; i < 200; ++i) tick();
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 1);

    // --- a slow stroke: the gate drops on the first drag tick, the stick waits until L has been up
    //     12 ticks (kLSettle), then feeds
    tick(); game_l(true); tick();
    mb[0x08 / 4] = 0;
    s.pan_x = -60; s.pan_y = 0; // finger 60 px right
    tick();
    assert(ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.on") == 1 && ints.at("sshd.l.eye") == 1);
    assert(mb[0x08 / 4] == 0); // not fed: the game still sees L (the stick would pan the camera)
    // dev.38: the gold trail follows the stroke from the minimap centre (620, 430), head first
    assert(ints.at("sshd.tr.on") == 1 && ints.at("sshd.tr0.x") == 680 && ints.at("sshd.tr0.y") == 430);
    assert(ints.at("sshd.tr0.a") == 15 && ints.at("sshd.tr0.s") == 1000);
    s.pan_x = -120; tick(); assert(mb[0x08 / 4] == 0);
    game_l(false); // the runtime's press ran out
    s.pan_x = -180;
    int settle = 0; // the game keeps the stick on the camera for a few frames after L comes up
    while (mb[0x08 / 4] == 0 && settle < 60) { tick(); ++settle; }
    assert(settle == 13); // fed on the first tick after 12 ticks of L up
    assert(mb[0x08 / 4] != 0 && static_cast<int32_t>(mb[0x0C / 4]) == 32767 && mb[0x10 / 4] == 0); // full right
    const uint32_t seq_before = mb[0x80 / 4];
    // dev.45: fed kLateFed (4) ticks or more counts as fed (the finger rests: the pan stays put)
    for (int i = 0; i < 3; ++i) tick();
    // dev.38: the lift is seen on the pad's first glide step (sshd_stroke.h PanStroke), so the hold
    // returns kLResume (6) ticks after the finger let go, while the pad still glides home
    s.pan_x *= 0.8; tick();
    assert(mb[0x80 / 4] == seq_before); // the stroke reached the stick: no replay
    assert(s.logs.back().find("sshd: swipe ") == 0 && s.logs.back().find(" fed (4 ticks, after the L release)") != std::string::npos);
    for (int i = 0; i < 5; ++i) { assert(ints.at("sshd.l.hold") == 0); s.pan_x *= 0.8; tick(); }
    assert(ints.at("sshd.l.hold") == 1 && ints.at("sshd.l.on") == 1); // re-held after the stroke
    lift();
    assert(mb[0x80 / 4] == seq_before);
    game_l(true); tick();

    // --- a fast stroke that ends before the game sees L up: replayed as a flick once it does
    mb[0x08 / 4] = 0;
    s.pan_x = 0; s.pan_y = 150; tick(); // finger up
    s.pan_x = 0; s.pan_y = 160; tick();
    assert(mb[0x08 / 4] == 0);
    s.pan_y *= 0.8; tick();               // lifted: the pad glides home
    assert(mb[0x80 / 4] == seq_before);   // queued, not staged: L still held in the game
    assert(ints.at("sshd.l.hold") == 0);
    s.pan_y *= 0.8; tick();
    assert(mb[0x80 / 4] == seq_before && ints.at("sshd.spin.busy") == 1);
    game_l(false);
    for (int i = 0; i < 11; ++i) { s.pan_y *= 0.8; tick(); assert(mb[0x80 / 4] == seq_before); }
    s.pan_y *= 0.8; tick(); // 12 ticks of L up: staged
    assert(mb[0x80 / 4] == seq_before + 1 && mb[0x84 / 4] == 2);
    lift();
    assert((mb[0xC0 / 4] & 0xFFFF) == 0 && static_cast<int16_t>(mb[0xC0 / 4] >> 16) == 32767 && mb[0xC4 / 4] == 4); // up
    assert(mb[0xC8 / 4] == 0 && mb[0xCC / 4] == 2);
    answer(6); tick(); // refused by the helper (item in hand): a flick's refusal is not "Not now"
    assert(ints.at("sshd.spin.refused") == 0);
    for (int i = 0; i < 6; ++i) tick();
    assert(ints.at("sshd.l.hold") == 1);
    game_l(true); tick();

    // --- dev.45 (Thor dev.44: two swipes dropped without a line): a stroke that waited for L and got
    //     through only 1..3 ticks before the lift is replayed as a flick, staged on the lift tick
    //     (the game has seen L up long enough by then), continuing the deflection already written
    for (int fed = 1; fed <= 3; ++fed) {
        assert(ints.at("sshd.l.hold") == 1);
        const uint32_t before = mb[0x80 / 4];
        mb[0x08 / 4] = 0;
        s.pan_x = -40; tick();                       // the lock lets go
        assert(ints.at("sshd.l.hold") == 0 && mb[0x20 / 4] == 0);
        game_l(false);
        int waited = 0;
        while (mb[0x08 / 4] == 0 && waited < 60) { s.pan_x -= 10; tick(); ++waited; }
        assert(waited == 13 && mb[0x80 / 4] == before);
        for (int i = 1; i < fed; ++i) { s.pan_x -= 10; tick(); }
        const size_t n_logs = s.logs.size();
        s.pan_x *= 0.8; tick();                      // the lift
        assert(mb[0x80 / 4] == before + 1 && mb[0x84 / 4] == 2); // staged at once
        assert(static_cast<int16_t>(mb[0xC0 / 4] & 0xFFFF) == 32767 && (mb[0xC0 / 4] >> 16) == 0 && mb[0xC4 / 4] == 4);
        bool replay_line = false, staged_line = false, refused = false;
        for (size_t i = n_logs; i < s.logs.size(); ++i) {
            replay_line |= s.logs[i].find("fed only " + std::to_string(fed) + " ticks after the L release: its flick replays") != std::string::npos;
            staged_line |= s.logs[i].find("stick sequence " + std::to_string(before + 1) + " staged") != std::string::npos;
            refused |= s.logs[i].find("refused") != std::string::npos;
        }
        assert(replay_line && staged_line && !refused);
        answer(0);
        lift();
        for (int i = 0; i < 8; ++i) tick();
        assert(ints.at("sshd.l.hold") == 1 && mb[0x20 / 4] == 8);
        game_l(true); tick();
    }

    // --- a spin while locked: queued until the game sees L up, then staged
    for (int i = 0; i < 100; ++i) tick();
    assert(ints.at("sshd.spin.ok") == 1);
    const uint32_t spin_before = mb[0x80 / 4];
    assert(act("spin_h"));
    tick();
    assert(mb[0x80 / 4] == spin_before && ints.at("sshd.l.hold") == 0 && ints.at("sshd.spin.busy") == 1);
    game_l(false);
    for (int i = 0; i < 11; ++i) { tick(); assert(mb[0x80 / 4] == spin_before); }
    tick();
    assert(mb[0x80 / 4] == spin_before + 1 && mb[0x84 / 4] == 6);
    answer(0); tick(); // ended: the hold returns kLResume (6) ticks later
    for (int i = 0; i < 4; ++i) tick();
    assert(ints.at("sshd.l.hold") == 0);
    tick();
    assert(ints.at("sshd.l.hold") == 1);
    game_l(true); tick();

    // --- dev.37: the player's own L (pressed during a stroke, or L dropped by another source)
    //     no longer unlocks
    s.pan_x = -100; tick();
    game_l(false); tick(); // released for the stroke
    game_l(true); tick();  // L again although the module lets go: the player
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 0);
    lift(); // the stroke never reached the stick: its flick waits for L up, then plays
    game_l(false);
    for (int i = 0; i < 60; ++i) {
        tick();
        if (mb[0x80 / 4] != mb[0xA0 / 4]) answer(0);
    }
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 1);
    game_l(true); tick();
    game_l(false); for (int i = 0; i < 10; ++i) tick(); // L dropped by another source
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 1);
    game_l(true); tick();

    // --- dev.37: a door / room / area load, menus, events, flight and the Silent Realms only pause the
    //     lock; it is held again once free play has run kLReengage (20) ticks. A two-tick blip (an
    //     off-thread read) does not pause it at all.
    struct Case { int reason; void (*set)(State&); void (*reset)(State&); };
    const Case cases[] = {
        {3, [](State& st) { st.event_state = 2; }, [](State& st) { st.event_state = 0; }}, // the door opens
        {9, [](State& st) { st.stage = ""; }, [](State& st) { st.stage = "F001r"; }},     // the room loads
        {3, [](State& st) { st.event_state = 1; }, [](State& st) { st.event_state = 0; }}, // entrance event
        {2, [](State& st) { st.menu = 0x10; }, [](State& st) { st.menu = 0; }},
        {4, [](State& st) { st.stage = "F020"; }, [](State& st) { st.stage = "F000"; }},
        {5, [](State& st) { st.stage = "S000"; }, [](State& st) { st.stage = "F000"; }},
    };
    for (const auto& c : cases) {
        // dev.40: the menu byte itself counts after three non-zero samples and closes after two zero
        // samples (MenuGate), on top of the lock's own three-tick debounce
        const int opening = c.reason == 2 ? 2 : 0, closing = c.reason == 2 ? 1 : 0;
        assert(ints.at("sshd.l.hold") == 1);
        c.set(s); tick(); tick();
        for (int i = 0; i < opening; ++i) tick();
        assert(ints.at("sshd.l.hold") == 1); // a blip is not a state
        tick();
        assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.reason") == c.reason);
        assert(ints.at("sshd.l.eye") == 1);
        game_l(false);
        for (int i = 0; i < 30; ++i) tick(); // the state lasts
        assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 0);
        assert(mb[0x20 / 4] == 0); // dev.45: the helper's L lets go while paused
        c.reset(s);
        for (int i = 0; i < 19 + closing; ++i) { tick(); assert(ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.on") == 1); }
        tick();
        assert(ints.at("sshd.l.hold") == 1 && ints.at("sshd.l.on") == 1); // re-engaged
        assert(mb[0x20 / 4] == 8);
        game_l(true); tick();
    }
    s.stage = "F000";
    // the stick hook stalls (6): paused as well, re-engaged once it beats again
    for (int i = 0; i < 31; ++i) { api->sample(inst, &h); api->tick(inst, &h); }
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.reason") == 6);
    for (int i = 0; i < 20; ++i) tick();
    assert(ints.at("sshd.l.on") == 1 && ints.at("sshd.l.hold") == 1);
    // the title / Quest Logs (save and quit) ends the lock; free play does not bring it back
    s.scene = 0x53656C74; tick(); tick(); tick();
    assert(ints.at("sshd.l.on") == 0 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.reason") == 1);
    s.scene = 0;
    game_l(false);
    for (int i = 0; i < 40; ++i) tick();
    assert(ints.at("sshd.l.on") == 0 && ints.at("sshd.l.hold") == 0 && ints.at("sshd.l.eye") == 0);
    // a host reload drops the lock
    assert(act("l_toggle")); tick();
    assert(ints.at("sshd.l.hold") == 1);
    mb[0x20 / 4] = 8;
    ext->configure(inst, &e);
    assert(mb[0x20 / 4] == 0); // the reload lets the helper's L go at once
    tick();
    assert(ints.at("sshd.l.on") == 0 && ints.at("sshd.l.hold") == 0);
    // unlocked, a physical L shows the eye, changes nothing else, and a stroke feeds at once
    game_l(true);
    mb[0x08 / 4] = 0;
    s.pan_x = -90; tick();
    assert(mb[0x08 / 4] != 0 && ints.at("sshd.l.on") == 0 && ints.at("sshd.l.eye") == 1 && ints.at("sshd.l.hold") == 0);
    lift();

    // --- shield bash (Ready top-left tap): a virtual L3 for kBashFrames game frames via the helper
    game_l(false); tick();
    std::memset(s.pouch, 0, sizeof(s.pouch));
    assert(!act("shield_bash") && mb[0x18 / 4] == 0); // no shield
    tick();
    assert(ints.at("sshd.bash.refused") == 1);
    s.pouch[4] = 119; s.pouch[5] = 24; // an Iron Shield in the pouch but none equipped (g0's file)
    assert(!act("shield_bash") && mb[0x18 / 4] == 0);
    s.shield_slot = 2; s.pouch[4] = 116; s.pouch[5] = 0; // equipped, but a broken Wooden Shield
    assert(!act("shield_bash") && mb[0x18 / 4] == 0);
    s.pouch[4] = 125; // the Hylian Shield (never breaks)
    assert(act("shield_bash") && mb[0x18 / 4] == 4);
    assert(!act("shield_bash")); // one at a time: the helper still counts the last one down
    mb[0x18 / 4] = 0;
    s.pouch[4] = 119; s.pouch[5] = 24; // Iron Shield 24
    s.held = 0x11; assert(act("shield_bash")); mb[0x18 / 4] = 0; // the sword drawn is fine
    s.held = 0x4; assert(!act("shield_bash")); s.held = 0xB;     // the slingshot is not
    // dev.40: one sample of the menu byte is a blip (MenuGate), three in a row are a menu
    s.menu = 0x10; tick(); assert(act("shield_bash")); mb[0x18 / 4] = 0;
    tick(); tick(); assert(!act("shield_bash")); s.menu = 0; tick(); tick();
    s.held = 0x4; assert(!act("shield_bash")); tick();
    assert(ints.at("sshd.bash.putaway") == 1 && ints.at("sshd.bash.notnow") == 0); // "Put away"
    s.held = 0xB;
    s.event_state = 1; tick(); assert(!act("shield_bash")); s.event_state = 0;
    tick(); assert(ints.at("sshd.bash.putaway") == 0 && ints.at("sshd.bash.notnow") == 1); // "Not now"
    s.stage = "F020"; tick(); assert(!act("shield_bash")); s.stage = "F000";
    s.stage = "S200"; tick(); assert(!act("shield_bash")); s.stage = "F000";
    assert(mb[0x18 / 4] == 0);
    for (int i = 0; i < 100; ++i) tick();
    assert(ints.at("sshd.bash.refused") == 0);

    const auto logged = [&](const char* m) {
        for (const auto& l : s.logs)
            if (l.find(m) != std::string::npos) return true;
        return false;
    };
    // --- dev.48 A6: the map rim lights (sshd.stab.lit) only for a stab that was staged
    s.held = 0xB;
    assert(!act("sword_stab")); // empty hands: refused
    tick();
    assert(ints.at("sshd.stab.lit") == 0 && mb[0x24 / 4] == 0);
    // --- dev.48: the sword stab and the shield bash exclude each other (never L3 and R3 together)
    s.held = 0x11;
    tick();
    assert(act("sword_stab") && mb[0x24 / 4] == 4);
    tick();
    assert(ints.at("sshd.stab.lit") == 1); // staged: lit for the look's ~150 ms
    assert(!act("shield_bash") && mb[0x18 / 4] == 0 && logged("refused shield_bash (busy)"));
    mb[0x24 / 4] = 0; // the helper counted R3 down
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tick();
    assert(ints.at("sshd.stab.lit") == 0); // the look ended
    assert(act("shield_bash") && mb[0x18 / 4] == 4);
    assert(!act("sword_stab") && mb[0x24 / 4] == 0 && logged("refused sword_stab (busy)"));
    tick();
    assert(ints.at("sshd.stab.lit") == 0); // a refused tap never lights the rim
    mb[0x18 / 4] = 0;
    s.held = 0xB;
    tick();

    // --- dev.48 A2: with the L lock on, a queued spin stages only in free play (LBlocked 0); the
    //     LBlocked grace (kLBlockTicks) keeps the lock but makes the sequence wait, then refuse
    const auto queue_spin = [&] {
        game_l(true);
        for (int i = 0; i < 30; ++i) tick();
        assert(ints.at("sshd.l.on") == 1);
        assert(act("spin_h")); // queued behind the L release (SeqQueue)
        game_l(false);         // the hold let go: the game sees L up
        for (int i = 0; i < 11; ++i) tick(); // one tick short of kLSettle
    };
    assert(act("l_toggle"));
    const uint32_t req0 = mb[0x80 / 4];
    queue_spin();
    assert(mb[0x80 / 4] == req0);
    s.event_state = 1; // an event blip just as the game has seen L up long enough (inside the grace)
    tick();
    assert(mb[0x80 / 4] == req0); // dev.47 staged it here, into the event
    s.event_state = 0;
    tick();
    assert(mb[0x80 / 4] == req0 + 1); // the blip passed: staged in free play
    answer(0);
    tick();
    queue_spin();
    const uint32_t req1 = mb[0x80 / 4];
    s.event_state = 1; // an event that stays: the grace runs out and the sequence is refused
    tick();
    tick();
    assert(mb[0x80 / 4] == req1);
    tick();
    assert(mb[0x80 / 4] == req1 && logged("refused queued sequence (event: lblocked 3"));
    s.event_state = 0;
    for (int i = 0; i < 30; ++i) tick();
    assert(act("l_toggle")); // unlocked again
    tick();
    assert(ints.at("sshd.l.on") == 0 && mb[0x20 / 4] == 0);

    // --- dev.48: a host reload (Configure) and unloading (Destroy) release every virtual button and
    //     the stick vector at once (dev.45..47: only the L lock's L)
    const auto press_all = [&] { mb[0x08 / 4] = 40; mb[0x18 / 4] = 4; mb[0x1C / 4] = 4; mb[0x20 / 4] = 8; mb[0x24 / 4] = 4; };
    const auto released = [&] { return mb[0x08 / 4] == 0 && mb[0x18 / 4] == 0 && mb[0x1C / 4] == 0 && mb[0x20 / 4] == 0 && mb[0x24 / 4] == 0; };
    press_all();
    ext->configure(inst, &e);
    assert(released());
    press_all();
    api->destroy(inst);
    assert(released());
}
