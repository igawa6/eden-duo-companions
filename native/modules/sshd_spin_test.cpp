// SPDX-License-Identifier: GPL-3.0-or-later
// Spin attacks (Ready spin buttons): the stick-sequence request the module stages for the helper's
// loop hook, its refusals, the result handling and the retirement of a helper that never answers.
// The tables were live-tuned on the desktop emulator (g0, 2026-11-11): 16 / 16 spins, each draining
// 312,500 stamina; the game took 1-6 frame flicks with 1-3 frame gaps, but no spin from 8-frame
// flicks or 6-frame gaps (the next flick has to start within 7 frames of the previous one).
#include <cassert>
#include <cstring>
#include <map>
#include <string>
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
    uint32_t water = 0;
    uint8_t items[0x20]{}; // slot+0x9E0.. item flags
    uint64_t player = 0x7100000;
    uint16_t held = 0xB;
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
    if (a == 0x1826AD0) return Put(out, n, uint64_t{0x7200000});
    if (a == 0x7200460) return Put(out, n, s.water);
    if (a == 0x7200468) return Put(out, n, uint32_t{0});
    if (a == kSlot + 0x531C && n == 8) {
        std::memset(out, 0, 8);
        std::memcpy(out, s.stage.data(), s.stage.size());
        return EDEN_DSMOD_TRUE;
    }
    if (a == kSlot + 0x531C && n == 1) return Put(out, n, static_cast<uint8_t>(s.stage.empty() ? 0 : s.stage[0]));
    if (a == kSlot + 0x9E0 && n == sizeof(s.items)) return std::memcpy(out, s.items, n), EDEN_DSMOD_TRUE;
    if (a == kScene) return Put(out, n, s.scene);
    if (sshd_test::MenuFixtureRead(0, s.menu, a, out, n)) return EDEN_DSMOD_TRUE; // dev.44: the controller state
    if (a == kEvents) return Put(out, n, kEventObj);
    if (a == kEventObj + 0x248) return Put(out, n, s.event_state);
    if (a == kPlayerPtr) return Put(out, n, s.player);
    if (s.player != 0 && a == s.player + 0x6404) return Put(out, n, s.held);
    if (s.player != 0 && a == s.player + 0x6408) return Put(out, n, static_cast<uint16_t>(1));
    return EDEN_DSMOD_FALSE;
}
}

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
    void* inst = api->create(&h, "");
    assert(inst);
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION; e.abi_hash = EDEN_DSMOD_EXT_HASH; e.struct_size = sizeof(e);
    e.userdata = &s; e.mailbox_address = 0x9000000; e.mailbox_size = 4096;
    e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<State*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool { static_cast<State*>(p)->mailbox[o / 4] = v; return EDEN_DSMOD_TRUE; };
    auto& mb = s.mailbox;
    mb[0xA0 / 4] = 41; // the helper's last answered sequence survives a reconfigure
    ext->configure(inst, &e);
    assert(mb[0x80 / 4] == 41);
    auto tick = [&] { ++mb[0x68 / 4]; api->sample(inst, &h); };
    auto act = [&](const char* a) { return ext->on_action(inst, a, 0) == EDEN_DSMOD_TRUE; };
    auto answer = [&](uint32_t code, uint32_t frames) { mb[0xA4 / 4] = code; mb[0xA8 / 4] = frames; mb[0xA0 / 4] = mb[0x80 / 4]; };
    auto step = [&](int i, int16_t& x, int16_t& y, uint32_t& f) {
        x = static_cast<int16_t>(mb[(0xC0 + 8 * i) / 4] & 0xFFFF);
        y = static_cast<int16_t>(mb[(0xC0 + 8 * i) / 4] >> 16);
        f = mb[(0xC4 + 8 * i) / 4];
    };

    // no sword yet (prologue save): a tap is refused
    tick();
    assert(s.ints.at("sshd.spin.ok") == 0 && !act("spin_h") && s.ints.at("sshd.spin.refused") == 0);
    tick();
    assert(s.ints.at("sshd.spin.refused") == 1);
    s.items[0x9E5 - 0x9E0] = 1; // Goddess Sword
    for (int i = 0; i < 100; ++i) tick();
    assert(s.ints.at("sshd.spin.ok") == 1 && s.ints.at("sshd.spin.refused") == 0);

    // dev.47: a map tap stages only a bounded R3 click, with the existing sword gates.
    constexpr int stab = 0x24 / 4;
    const auto logged = [&](const char* m) {
        for (const auto& l : s.logs)
            if (l.find(m) != std::string::npos) return true;
        return false;
    };
    assert(!act("sword_stab") && mb[stab] == 0); // sword sheathed
    assert(logged("refused sword_stab (sword not drawn)")); // dev.48: its own reason (was "item in hand")
    s.held = 0x11;
    tick();
    assert(act("sword_stab") && mb[stab] == 4);
    assert(!act("sword_stab") && mb[stab] == 4); // repeated tap cannot extend a click
    mb[stab] = 0; // helper expires it
    auto refused_stab = [&] { tick(); tick(); tick(); assert(!act("sword_stab") && mb[stab] == 0); };
    s.menu = 1; refused_stab(); s.menu = 0;
    s.event_state = 1; refused_stab(); s.event_state = 0;
    s.stage = "F020"; refused_stab();
    s.stage = "S000"; refused_stab(); s.stage = "F000";
    s.water = 0x40000; refused_stab(); s.water = 0;
    assert(logged("refused sword_stab (swimming)")); // dev.48: its own reason (was "unreadable")
    s.scene = 0x53656C74; refused_stab(); s.scene = 0;
    s.stage = ""; refused_stab(); s.stage = "F000";
    s.player = 0; refused_stab(); s.player = 0x7100000;
    s.held = 4; refused_stab(); s.held = 0x11;
    s.items[5] = 0; refused_stab(); s.items[5] = 1;
    for (int i = 0; i < 100; ++i) tick();
    assert(act("sword_stab") && mb[stab] == 4);
    mb[stab] = 0;
    s.held = 0xB;
    tick();

    // horizontal: left, right, left flicks with one neutral frame between, then two neutral frames
    assert(act("spin_h"));
    assert(mb[0x80 / 4] == 42 && mb[0x84 / 4] == 6 && mb[0x88 / 4] == mb[0xFFC / 4] && mb[0x88 / 4] != 0);
    {
        const int16_t want_x[6] = {-32767, 0, 32767, 0, -32767, 0};
        uint32_t total = 0, since_flick = 0;
        for (int i = 0; i < 6; ++i) {
            int16_t x, y;
            uint32_t f;
            step(i, x, y, f);
            assert(x == want_x[i] && y == 0 && f >= 1 && f <= 120);
            if (x != 0) {
                assert(i == 0 || since_flick <= 7 - 3); // each flick starts within the game's 7 frames
                since_flick = 0;
            } else {
                since_flick += f;
            }
            total += f;
        }
        assert(total == 13); // what the helper reported live ("after 13 frames")
    }
    tick();
    // dev.40 (Thor: four taps 130 ms apart during a spin, all "Not now"): a tap while one plays
    // queues ONE more spin; further taps while it waits are that spin
    assert(s.ints.at("sshd.spin.busy") == 1 && s.ints.at("sshd.spin.ok") == 0 && act("spin_v"));
    assert(act("spin_h") && act("spin_v"));
    tick();
    assert(s.ints.at("sshd.spin.queued") == 1 && mb[0x80 / 4] == 42 && s.ints.at("sshd.spin.refused") == 0);
    answer(0, 13);
    tick(); // the first ends, the queued vertical spin starts at once
    assert(s.ints.at("sshd.spin.code") == 0 && mb[0x80 / 4] == 43 && s.ints.at("sshd.spin.queued") == 0);
    assert(s.ints.at("sshd.spin.busy") == 1 && s.ints.at("sshd.spin.refused") == 0);

    // vertical: up, down, up
    {
        const int16_t want_y[6] = {32767, 0, -32767, 0, 32767, 0};
        for (int i = 0; i < 6; ++i) {
            int16_t x, y;
            uint32_t f;
            step(i, x, y, f);
            assert(x == 0 && y == want_y[i] && f >= 1 && f <= 120);
        }
    }
    answer(1, 1); // the physical stick took over: not a refusal
    tick();
    assert(s.ints.at("sshd.spin.code") == 1 && s.ints.at("sshd.spin.refused") == 0);

    // the sword drawn (0x11) is fine, any other item in hand is not
    s.held = 0x11;
    tick();
    assert(s.ints.at("sshd.spin.ok") == 1);
    s.held = 0x4;
    tick();
    assert(s.ints.at("sshd.spin.ok") == 0 && !act("spin_h"));
    assert(!act("sword_stab") && mb[stab] == 0);
    tick();
    assert(s.ints.at("sshd.spin.refused") == 1);
    // dev.40: the reason under the buttons is "Put away", not "Not now"
    assert(s.ints.at("sshd.spin.putaway") == 1 && s.ints.at("sshd.spin.notnow") == 0);
    assert(s.ints.at("sshd.why.spin") == 8 && s.ints.at("sshd.refuse.why") == 8);
    s.held = 0xB;
    for (int i = 0; i < 100; ++i) tick();
    // dev.40: one sample of the pause-menu byte is a blip (gate probe: 0x10 / 0xBB for a single
    // sample in plain play), not a menu
    s.menu = 0x10; tick(); assert(s.ints.at("sshd.menu.open") == 0 && act("spin_h")); s.menu = 0;
    answer(0, 13); tick();
    for (int i = 0; i < 100; ++i) tick();
    // menus, events, the title, Loftwing, no Link: refused, nothing staged
    const uint32_t before = mb[0x80 / 4];
    s.menu = 0x10; tick(); tick(); tick();
    assert(s.ints.at("sshd.menu.open") == 1 && !act("spin_h")); s.menu = 0; tick(); tick();
    tick(); assert(s.ints.at("sshd.spin.notnow") == 1 && s.ints.at("sshd.refuse.why") == 4);
    // an event waits up to half a second (a blip passes), then says "Not now"
    s.event_state = 2; tick(); assert(act("spin_v")); tick(); assert(s.ints.at("sshd.spin.queued") == 1);
    for (int i = 0; i < 31; ++i) tick();
    assert(s.ints.at("sshd.spin.queued") == 0 && s.ints.at("sshd.spin.refused") == 1 && s.ints.at("sshd.refuse.why") == 5);
    s.event_state = 0;
    s.scene = 0x53656C74; tick(); assert(!act("spin_h")); s.scene = 0;
    s.stage = "F020"; tick(); assert(!act("spin_h")); s.stage = "F000";
    s.player = 0; tick(); assert(!act("spin_h")); s.player = 0x7100000;
    assert(mb[0x80 / 4] == before);
    assert(!act("spin_x") && !act("spin"));
    tick();
    assert(s.ints.at("sshd.spin.ok") == 1);
    // the helper stopped it for the game state (event / item): shown as refused
    assert(act("spin_h"));
    answer(5, 4);
    tick();
    assert(s.ints.at("sshd.spin.code") == 5 && s.ints.at("sshd.spin.refused") == 1);
    for (int i = 0; i < 100; ++i) tick();
    { // dev.43: the game thread's refusal logs one line (not one per sample of its "Not now")
        int n = 0;
        for (const auto& l : s.logs) n += l.find("sshd: refused spin (event: sequence ") != std::string::npos;
        assert(n == 1);
    }

    // a stopped loop hook (no frame heartbeat) is not offered
    for (int i = 0; i < 40; ++i) api->sample(inst, &h);
    assert(s.ints.at("sshd.spin.ok") == 0 && !act("spin_h"));
    assert(!act("sword_stab") && mb[stab] == 0);
    tick();
    assert(s.ints.at("sshd.spin.ok") == 1);
    // a sequence the helper never answers retires the spins for the session
    assert(act("spin_v"));
    for (int i = 0; i < 260; ++i) tick();
    assert(s.ints.at("sshd.spin.ok") == 0 && s.ints.at("sshd.spin.busy") == 0 && !act("spin_h"));
    // a reconfigure (host reload) brings them back, in step with the helper's sequence
    ext->configure(inst, &e);
    tick();
    assert(s.ints.at("sshd.spin.ok") == 1 && mb[0x80 / 4] == mb[0xA0 / 4]);
    assert(s.ints.at("sshd.stick.ready") == 1 && s.ints.at("sshd.swipe.fallback") == 0);
    api->destroy(inst);

    // dev.42: without a ready helper the Ready swipes fall back to fixed right-stick
    // flicks (manifest slash_*, enabled_bind sshd.swipe.fallback). The same refusals apply:
    // menus, events, the title, a load, the Loftwing, a Silent Realm, no Link, an item in hand,
    // no sword
    void* nh = api->create(&h, ""); // no configure: no mailbox, no helper
    assert(nh);
    const auto fb = [&] {
        api->sample(nh, &h);
        assert(s.ints.at("sshd.stick.ready") == 0);
        return s.ints.at("sshd.swipe.fallback");
    };
    assert(fb() == 1);
    s.menu = 0x10; fb(); fb(); assert(fb() == 0); s.menu = 0;
    for (int i = 0; i < 10; ++i) fb();
    assert(fb() == 1);
    s.event_state = 2; assert(fb() == 0); s.event_state = 0; assert(fb() == 1);
    s.scene = 0x53656C74; assert(fb() == 0); s.scene = 0; assert(fb() == 1);
    s.stage = ""; assert(fb() == 0);
    s.stage = "F020"; assert(fb() == 0);
    for (const char* realm : {"S000", "S100", "S200", "S300"}) {
        s.stage = realm;
        assert(fb() == 0);
    }
    s.stage = "S400"; assert(fb() == 1); // not a Silent Realm stage name
    s.stage = "F000"; assert(fb() == 1);
    s.player = 0; assert(fb() == 0); s.player = 0x7100000;
    s.held = 0x4; assert(fb() == 0);
    s.held = 0x11; assert(fb() == 1); // the sword drawn is fine
    s.held = 0xB;
    s.items[0x9E5 - 0x9E0] = 0; assert(fb() == 0); s.items[0x9E5 - 0x9E0] = 1;
    assert(fb() == 1);
    // a helper that comes up takes over: the fallback is off while it is ready
    ext->configure(nh, &e);
    ++mb[0x68 / 4];
    api->sample(nh, &h);
    assert(s.ints.at("sshd.stick.ready") == 1 && s.ints.at("sshd.swipe.fallback") == 0);
    api->destroy(nh);
}
