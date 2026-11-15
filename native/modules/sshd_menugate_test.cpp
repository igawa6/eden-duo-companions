// SPDX-License-Identifier: GPL-3.0-or-later
// dev.44 menu gate (Thor RC 2026-11-14: "L lock paused (2)" for ~5.5 s at a time in plain play): the
// pause / map state comes from the layout controller's state (dLytControlGame_c +0x120), sampled by the
// helper's loop hook on the game thread (mailbox +0x6C code, +0x70 stamp = the frame's loop heartbeat,
// +0x74 the state's distance from StateID_Normal). The module uses that sample while it is fresh, falls
// back to reading the two words itself, keeps dev.40's debounce (3 samples open, 2 close) on either,
// and tells an unreadable state ("unreadable", LBlocked 7, after a 30-sample grace) from a menu.
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sshd_menu_fixture.h"

namespace {
constexpr uint64_t kSlot = 0x5AEAD54, kScene = 0x182A7F4, kEvents = 0x182DED8, kEventObj = 0x7000000,
                   kPlayerPtr = 0x2BF9940;
struct State {
    uint8_t menu = 0;     // the controller state the direct read sees (non-zero = Pause)
    bool ctrl_ok = true;  // the direct read of the controller succeeds
    uint8_t stack = 0;    // the dev.40..43 stack slot main+0x192D0E4 (must no longer matter)
    uint32_t mailbox[1024]{};
    std::map<std::string, int64_t> ints;
    std::vector<std::string> logs;
};
template <typename T> bool Put(void* out, size_t n, const T& v) {
    if (n != sizeof(T)) return false;
    std::memcpy(out, &v, n);
    return true;
}
EdenDsmodBool Read(void* p, uint64_t a, void* out, size_t n) {
    auto& s = *static_cast<State*>(p);
    if (sshd_test::MenuFixtureRead(0, s.menu, a, out, n, s.ctrl_ok)) return EDEN_DSMOD_TRUE;
    if (a == kSlot + 0x531C && n == 8) {
        std::memset(out, 0, 8);
        std::memcpy(out, "F000", 4);
        return EDEN_DSMOD_TRUE;
    }
    if (a == kSlot + 0x531C && n == 1) return Put(out, n, uint8_t{'F'});
    if (a == kScene) return Put(out, n, uint32_t{0});
    if (a == 0x192D0E4) return Put(out, n, s.stack);
    if (a == kEvents) return Put(out, n, kEventObj);
    if (a == kEventObj + 0x248) return Put(out, n, uint32_t{0});
    if (a == kPlayerPtr) return Put(out, n, uint64_t{0x7100000});
    if (a == 0x7100000 + 0x6404) return Put(out, n, uint16_t{0xB});
    if (a == 0x7100000 + 0x6408) return Put(out, n, uint16_t{1});
    if (a == 0x7100000 + 0x468) return Put(out, n, uint32_t{0});
    if (a == 0x7100000 + 0x63B5) return Put(out, n, uint8_t{0});
    if (a == 0x7100000 + 0x63E2) return Put(out, n, int8_t{-1});
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
    h.get_f64 = [](void*, const char*, double fb) -> double { return fb; };
    void* inst = api->create(&h, "");
    assert(inst);
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION; e.abi_hash = EDEN_DSMOD_EXT_HASH; e.struct_size = sizeof(e);
    e.userdata = &s; e.mailbox_address = 0x9000000; e.mailbox_size = 4096;
    e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<State*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool { static_cast<State*>(p)->mailbox[o / 4] = v; return EDEN_DSMOD_TRUE; };
    ext->configure(inst, &e);
    auto& mb = s.mailbox;
    auto& ints = s.ints;
    enum { kPlay = 0, kOpen = 1, kNone = 2, kUnread = -1 };     // sshd.menu.state
    enum { kSrcNone = 0, kSrcHelper = 1, kSrcDirect = 2 };    // sshd.menu.src
    constexpr int kWhyOk = 0, kWhyMenu = 4, kWhyUnread = 17;  // sshd.why.pouch
    // one runtime tick; `helper` = the loop hook ran this frame and wrote its sample (code, delta)
    int helper_code = -1;
    uint32_t helper_delta = 0;
    auto tick = [&] {
        ++mb[0x04 / 4];
        ++mb[0x68 / 4];
        if (helper_code >= 0) sshd_test::MenuHelperSample(mb, static_cast<uint32_t>(helper_code), helper_delta);
        api->sample(inst, &h);
        api->tick(inst, &h);
    };
    auto ticks = [&](int n) { for (int i = 0; i < n; ++i) tick(); };
    auto logged = [&](const char* m) {
        for (const auto& l : s.logs)
            if (l.find(m) != std::string::npos) return true;
        return false;
    };

    // --- no helper sample yet (stamp 0): the module reads the controller itself; play
    ticks(3);
    assert(ints.at("sshd.menu.src") == kSrcDirect && ints.at("sshd.menu.state") == kPlay);
    assert(ints.at("sshd.menu.open") == 0 && ints.at("sshd.why.pouch") == kWhyOk);
    // the old stack slot no longer matters: 0x10 there for seconds (the Thor RC) is still play
    s.stack = 0x10;
    ticks(40);
    assert(ints.at("sshd.menu.open") == 0 && ints.at("sshd.why.pouch") == kWhyOk && ints.at("sshd.l.blk") == 0);
    s.stack = 0;

    // --- a fresh helper sample wins over the direct read, both ways
    helper_code = kPlay;
    s.menu = 0x10; // the direct read would say Pause
    ticks(5);
    assert(ints.at("sshd.menu.src") == kSrcHelper && ints.at("sshd.menu.state") == kPlay && ints.at("sshd.menu.open") == 0);
    s.menu = 0;
    helper_code = kOpen, helper_delta = 0x148; // StateID_Pause
    tick(); tick();
    assert(ints.at("sshd.menu.open") == 0);    // dev.40 debounce: not yet (2 samples)
    tick();
    assert(ints.at("sshd.menu.open") == 1 && ints.at("sshd.menu.src") == kSrcHelper);
    assert(ints.at("sshd.why.pouch") == kWhyMenu && ints.at("sshd.l.blk") == 2);
    assert(logged("sshd: menu gate open: state Pause (game thread)"));
    helper_code = kPlay, helper_delta = 0;
    tick();
    assert(ints.at("sshd.menu.open") == 1);    // closes after 2 samples of play
    tick();
    assert(ints.at("sshd.menu.open") == 0 && ints.at("sshd.why.pouch") == kWhyOk);
    assert(logged("sshd: menu gate closed: state Normal (game thread)"));
    // the map's state is named in the line too
    helper_code = kOpen, helper_delta = 0x268; // StateID_Map
    ticks(3);
    assert(ints.at("sshd.menu.open") == 1 && logged("sshd: menu gate open: state Map (game thread)"));
    helper_code = kPlay, helper_delta = 0;
    ticks(2);
    assert(ints.at("sshd.menu.open") == 0);
    // no controller (title, a load): not a menu
    helper_code = kNone;
    ticks(3);
    assert(ints.at("sshd.menu.state") == kNone && ints.at("sshd.menu.open") == 0);
    helper_code = kPlay;
    ticks(2);

    // --- the stamp may trail the heartbeat by one frame (the helper between its heartbeat and its
    //     stamp) or lead it by one (it ran between the module's two loads): still fresh
    mb[0x6C / 4] = kOpen, mb[0x74 / 4] = 0x148;
    helper_code = -1;
    for (int i = 0; i < 3; ++i) {
        ++mb[0x04 / 4];
        ++mb[0x68 / 4];
        mb[0x70 / 4] = mb[0x68 / 4] - (i == 1 ? 0 : 1) + (i == 2 ? 2 : 0); // lag 1, 0, -1
        api->sample(inst, &h);
        api->tick(inst, &h);
        assert(ints.at("sshd.menu.src") == kSrcHelper);
    }
    assert(ints.at("sshd.menu.open") == 1);
    helper_code = kPlay;
    ticks(2);
    assert(ints.at("sshd.menu.open") == 0);

    // --- a stale stamp (the hook skipped frames: two or more behind) or an unknown code: the direct read
    helper_code = -1;
    mb[0x6C / 4] = kOpen;
    s.menu = 0; // the controller says play
    for (int i = 0; i < 4; ++i) {
        ++mb[0x04 / 4];
        mb[0x68 / 4] += 1;
        mb[0x70 / 4] = mb[0x68 / 4] - 3;
        api->sample(inst, &h);
        api->tick(inst, &h);
    }
    assert(ints.at("sshd.menu.src") == kSrcDirect && ints.at("sshd.menu.open") == 0);
    mb[0x6C / 4] = 7, mb[0x70 / 4] = mb[0x68 / 4]; // a code the helper never writes
    s.menu = 0x10;
    for (int i = 0; i < 3; ++i) {
        ++mb[0x04 / 4];
        api->sample(inst, &h);
        api->tick(inst, &h);
        mb[0x68 / 4] += 1, mb[0x70 / 4] = mb[0x68 / 4];
    }
    assert(ints.at("sshd.menu.src") == kSrcDirect && ints.at("sshd.menu.open") == 1);
    assert(logged("sshd: menu gate open: state Pause (direct)"));
    s.menu = 0;
    helper_code = kPlay;
    ticks(2);
    assert(ints.at("sshd.menu.open") == 0 && ints.at("sshd.menu.src") == kSrcHelper);

    // --- the helper's loop stops (its heartbeat stands still): after 8 samples its last sample is
    //     stale even though its stamp still matches the standing heartbeat; the direct read takes over
    helper_code = -1;
    mb[0x6C / 4] = kPlay, mb[0x70 / 4] = mb[0x68 / 4];
    s.menu = 0x10;
    for (int i = 0; i < 8; ++i) {
        ++mb[0x04 / 4];
        api->sample(inst, &h);
        api->tick(inst, &h);
    }
    assert(ints.at("sshd.menu.open") == 0); // the frozen helper sample still answered "play"
    for (int i = 0; i < 4; ++i) {
        ++mb[0x04 / 4];
        api->sample(inst, &h);
        api->tick(inst, &h);
    }
    assert(ints.at("sshd.menu.src") == kSrcDirect && ints.at("sshd.menu.open") == 1);
    s.menu = 0;
    helper_code = kPlay;
    ticks(3);
    assert(ints.at("sshd.menu.open") == 0 && ints.at("sshd.menu.src") == kSrcHelper);

    // --- neither source answers (no fresh sample, controller unreadable): a menu that was open stays
    //     open for the 30-sample grace, then the gate is "unreadable", not a menu (why 17, LBlocked 7)
    helper_code = kOpen, helper_delta = 0x148;
    ticks(3);
    assert(ints.at("sshd.menu.open") == 1);
    helper_code = -1;
    s.ctrl_ok = false;
    mb[0x70 / 4] = 0; // no stamp
    ticks(30);
    assert(ints.at("sshd.menu.state") == kUnread && ints.at("sshd.menu.src") == kSrcNone);
    assert(ints.at("sshd.menu.open") == 1 && ints.at("sshd.why.pouch") == kWhyMenu); // the grace
    tick();
    assert(ints.at("sshd.menu.open") == 0 && ints.at("sshd.why.pouch") == kWhyUnread && ints.at("sshd.l.blk") == 7);
    assert(logged("sshd: menu gate unreadable: state unread (none) stage F000 (the gate was open"));
    ticks(20);
    assert(ints.at("sshd.why.pouch") == kWhyUnread);
    // ... and recovers on the first answer
    s.ctrl_ok = true;
    tick();
    assert(ints.at("sshd.menu.src") == kSrcDirect && ints.at("sshd.why.pouch") == kWhyOk && ints.at("sshd.l.blk") == 0);
    assert(logged("sshd: menu gate closed: state Normal (direct) stage F000 (the gate was unreadable"));
    // a short failure in play is not even a refusal: the grace keeps "no menu"
    s.ctrl_ok = false;
    ticks(10);
    assert(ints.at("sshd.why.pouch") == kWhyOk && ints.at("sshd.menu.open") == 0);
    s.ctrl_ok = true;
    tick();

    api->destroy(inst);
    return 0;
}
