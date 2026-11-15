// SPDX-License-Identifier: GPL-3.0-or-later
// Goddess Cube count, opened Goddess Chest markers, the Adventure Pouch request path and the ZR / R
// equip requests, against bytes recorded live on 2026-11-09 (hero 100 % save, owner partial save).
// dev.40: the gates' reasons, the debounced pause-menu byte, the one-place queue (an op in flight, an
// event blip, a sword swing, an open native wheel), the sword drawn and the put-away of a held item.
#include <cassert>
#include <cstring>
#include <map>
#include <string>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "sshd_menu_fixture.h"

namespace {
constexpr uint64_t kSlot = 0x5AEAD54, kStory = 0x182E1F8 + 0x1B, kScene = 0x182A7F4,
                   kEvents = 0x182DED8, kTbox = 0x182E118, kEventObj = 0x7000000, kPlayerPtr = 0x2BF9940;
struct State {
    std::string stage = "F402";
    uint8_t story[5]{};
    uint32_t scene = 0, sky = 0, skyloft = 0, live_tbox = 0, event_state = 0;
    uint8_t menu = 0;
    uint16_t pouch_flags = 1 << 6, counter = 4 << 10; // Adventure Pouch + 4 expansions
    uint64_t fail = 0;
    uint64_t player = 0;                    // dAcPy* (0 = no Link)
    uint16_t held = 0xB, bsel = 1;          // player+0x6404 item in hand (0xB none), +0x6408 ZR item
    uint32_t act = 0;                       // player+0x468 action id (0x68 / 0x9A sword attacks)
    uint8_t wheel = 0;                      // player+0x63B5 native wheel bits (1 B-wheel, 4 pouch wheel)
    uint32_t mailbox[1024]{};
    std::map<std::string, int64_t> ints;
    std::map<std::string, double> floats;
};

template <typename T> bool Put(void* out, size_t n, const T& v) {
    if (n != sizeof(T)) return false;
    std::memcpy(out, &v, n);
    return true;
}
EdenDsmodBool Read(void* p, uint64_t a, void* out, size_t n) {
    auto& s = *static_cast<State*>(p);
    if (a == s.fail) return EDEN_DSMOD_FALSE;
    if (a == kSlot + 0x531C && n == 8) {
        std::memset(out, 0, 8);
        std::memcpy(out, s.stage.data(), s.stage.size());
        return EDEN_DSMOD_TRUE;
    }
    if (a == kSlot + 0x531C && n == 1) return Put(out, n, static_cast<uint8_t>(s.stage.empty() ? 0 : s.stage[0]));
    if (a == kStory && n == 5) return std::memcpy(out, s.story, 5), EDEN_DSMOD_TRUE;
    if (a == kScene) return Put(out, n, s.scene);
    if (sshd_test::MenuFixtureRead(0, s.menu, a, out, n)) return EDEN_DSMOD_TRUE; // dev.44: the controller state
    if (a == kEvents) return Put(out, n, kEventObj);
    if (a == kEventObj + 0x248) return Put(out, n, s.event_state);
    if (a == kTbox) return Put(out, n, s.live_tbox);
    if (a == kSlot + 0x2A64) return Put(out, n, s.skyloft);
    if (a == kSlot + 0x2A64 + 4 * 21) return Put(out, n, s.sky);
    if (a == kSlot + 0x9E4 + 2 * 5) return Put(out, n, s.pouch_flags);
    if (a == kSlot + 0x9E4 + 2 * 54) return Put(out, n, s.counter);
    if (a == kPlayerPtr) return Put(out, n, s.player);
    if (s.player != 0 && a == s.player + 0x6404) return Put(out, n, s.held);
    if (s.player != 0 && a == s.player + 0x6408) return Put(out, n, s.bsel);
    if (s.player != 0 && a == s.player + 0x468) return Put(out, n, s.act);
    if (s.player != 0 && a == s.player + 0x63B5) return Put(out, n, s.wheel);
    return EDEN_DSMOD_FALSE; // everything else the module samples is unreadable here
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
    h.publish_f64 = [](void* p, const char* k, double v) { static_cast<State*>(p)->floats[k] = v; };
    h.publish_text = [](void*, const char*, const char*) {};
    void* inst = api->create(&h, "");
    assert(inst);

    // --- Goddess Cubes: per-region counts of the live story-flag bytes
    const uint8_t hero[5] = {0xE0, 0xF3, 0xFF, 0x7F, 0x27}, owner[5] = {0xE0, 0xB1, 0x80, 0x40, 0x00};
    auto cubes = [&](const char* st, const uint8_t* f) {
        s.stage = st;
        std::memcpy(s.story, f, 5);
        api->sample(inst, &h);
        return s.ints.at("sshd.cubes.valid") ? s.ints.at("sshd.cubes") : -1;
    };
    for (const char* st : {"F100", "F101", "F102_1", "B100_1", "F200", "F201_3", "F300", "F301", "F302"})
        assert(cubes(st, hero) == 9);
    assert(cubes("F100", owner) == 2 && cubes("F200", owner) == 4 && cubes("F300", owner) == 3);
    assert(cubes("F402", hero) == -1 && cubes("F020", hero) == -1 && cubes("D100", hero) == -1);
    s.scene = 0x53656C74; assert(cubes("F200", hero) == -1); s.scene = 0; // title keeps the old file
    s.fail = kStory; assert(cubes("F200", hero) == -1); s.fail = 0;
    const uint8_t none[5] = {0x08, 0x0C, 0, 0x80, 0x38}; // first-cube flag and unused ids only
    assert(cubes("F100", none) == 0 && cubes("F200", none) == 0 && cubes("F300", none) == 0);

    // --- Sky Goddess tags (map-marker contract 2026-11-11): cube struck AND chest still closed.
    // Owner's real save: struck cubes 227-230, 234, 235, 237, 245, 252 (story bytes above), opened
    // chest flags 0, 1, 7, 10, 18 (sky word 0x40483) -> tags on chest flags 2, 3, 8, 25 = cubes
    // 229, 230, 235, 252, exactly the four native tags (native capture 07) = markers 2, 3, 6, 20.
    s.stage = "F020"; s.sky = 0x40483; s.skyloft = 0;
    std::memcpy(s.story, owner, 5);
    api->sample(inst, &h);
    assert(s.ints.at("sshd.gc.n") == 4);
    for (int i = 0; i < 22; ++i)
        assert(s.ints.at("sshd.gc" + std::to_string(i) + ".on") == (i == 2 || i == 3 || i == 6 || i == 20));
    assert(s.floats.at("sshd.gc2.u") == -137465 && s.floats.at("sshd.gc2.v") == -32051);
    s.live_tbox = 1 << 2; api->sample(inst, &h); // chest 229 opened this visit, before the commit
    assert(s.ints.at("sshd.gc.n") == 3 && !s.ints.at("sshd.gc2.on")); s.live_tbox = 0;
    s.fail = kSlot + 0x2A64 + 84; api->sample(inst, &h); assert(s.ints.at("sshd.gc.n") == 0); s.fail = 0;
    s.fail = kStory; api->sample(inst, &h); assert(s.ints.at("sshd.gc.n") == 0); s.fail = 0;
    std::memcpy(s.story, hero, 5); api->sample(inst, &h); // 100 %: every chest opened, no tags
    s.sky = 0xFFFFFFFF; api->sample(inst, &h); assert(s.ints.at("sshd.gc.n") == 0);
    s.stage = "F000"; s.skyloft = 0; api->sample(inst, &h); // Skyloft: no tag template, never tagged
    assert(s.ints.at("sshd.gc.n") == 0);
    s.stage = "F300"; api->sample(inst, &h); assert(s.ints.at("sshd.gc.n") == 0);

    // --- Adventure Pouch: requests through the mailbox, refused outside free gameplay
    EdenDsmodHostExtensions e{};
    e.version = EDEN_DSMOD_EXT_VERSION; e.struct_size = sizeof(e); e.abi_hash = EDEN_DSMOD_EXT_HASH;
    e.userdata = &s; e.mailbox_address = 0x9000000; e.mailbox_size = 4096;
    e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<State*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
    e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool { static_cast<State*>(p)->mailbox[o / 4] = v; return EDEN_DSMOD_TRUE; };
    ext->configure(inst, &e);
    assert(s.mailbox[0xFFC / 4] == 1); // epoch seeded
    auto tick = [&] { ++s.mailbox[0x68 / 4]; api->sample(inst, &h); }; // the helper's loop heartbeat
    auto act = [&](const char* a, int64_t v) { return ext->on_action(inst, a, v) == EDEN_DSMOD_TRUE; };
    auto answer = [&](uint32_t code) { s.mailbox[0x64 / 4] = code; s.mailbox[0x60 / 4] = s.mailbox[0x40 / 4]; };
    auto& mb = s.mailbox;
    auto seq = [&] { return mb[0x40 / 4]; };
    auto why = [&] { return s.ints.at("sshd.refuse.why"); };
    // dev.40 reason codes (GateWhy)
    enum { kOk = 0, kHelper = 1, kTitle = 2, kLoad = 3, kMenu = 4, kEvent = 5, kLoftwing = 6, kNoPlayer = 7,
           kItem = 8, kSwing = 9, kBusy = 13, kPayload = 14, kGame = 15 };
    s.stage = "F200";
    tick(); tick();
    assert(s.ints.at("sshd.pouch.ok") == 1 && s.ints.at("sshd.pouch.slots") == 8);
    assert(act("pouch_to_3", 0));
    assert(seq() == 1 && mb[0x44 / 4] == 1 && mb[0x48 / 4] == 0 && mb[0x4C / 4] == 3 && mb[0x50 / 4] == 1);
    // dev.40 (Thor: drops while an op was in flight said "Not now"): a drop then waits in the one
    // queue place and is staged as soon as the helper answers; another op finding it taken is busy
    assert(act("pouch_to_4", 1) && seq() == 1);
    tick();
    assert(s.ints.at("sshd.pouch.queued") == 1 && seq() == 1);
    assert(!act("stock_to_7", 2)); tick(); assert(why() == kBusy && s.ints.at("sshd.pouch.refused") == 1);
    answer(0); tick();
    assert(seq() == 2 && mb[0x44 / 4] == 1 && mb[0x48 / 4] == 1 && mb[0x4C / 4] == 4);
    assert(s.ints.at("sshd.pouch.queued") == 0);
    answer(0); tick();
    assert(act("stock_to_5", 2) && seq() == 3 && mb[0x44 / 4] == 2 && mb[0x48 / 4] == 2 && mb[0x4C / 4] == 5);
    answer(0); tick();
    assert(act("pouch_to_1", 105) && seq() == 4 && mb[0x44 / 4] == 2 && mb[0x48 / 4] == 1 && mb[0x4C / 4] == 5);
    answer(5); tick(); // the helper saw an event: shown as refused, the game's code logged
    assert(s.ints.at("sshd.pouch.refused") == 1 && why() == kGame);
    assert(act("pouch_to_2", 2) && seq() == 4); // drop on itself: accepted, nothing staged
    assert(!act("stock_to_1", 104) && !act("pouch_to_9", 0) && !act("stock_to_60", 0) && !act("bogus", 0));
    // the pause-menu byte: one sample is a blip (gate probe, plain play), three in a row a menu
    s.menu = 0x10; tick();
    assert(s.ints.at("sshd.pouch.ok") == 1 && act("pouch_to_3", 0) && seq() == 5);
    answer(0); s.menu = 0; tick(); tick();
    s.menu = 0x10; tick(); tick(); tick();
    assert(!s.ints.at("sshd.pouch.ok") && !act("pouch_to_3", 0) && seq() == 5);
    tick(); assert(why() == kMenu && s.ints.at("sshd.why.pouch") == kMenu);
    s.menu = 0; tick(); assert(!s.ints.at("sshd.pouch.ok")); tick(); assert(s.ints.at("sshd.pouch.ok") == 1);
    // an event: a drop waits up to kPendEventWait (30) samples, so a blip passes ...
    s.event_state = 2; tick();
    assert(s.ints.at("sshd.pouch.ok") == 1 && act("pouch_to_3", 0) && seq() == 5);
    tick(); tick(); s.event_state = 0; tick();
    assert(seq() == 6 && mb[0x48 / 4] == 0 && mb[0x4C / 4] == 3);
    answer(0); tick();
    // ... and a cutscene refuses it
    s.event_state = 2; tick(); assert(act("pouch_to_3", 0));
    for (int i = 0; i < 31; ++i) tick();
    assert(!s.ints.at("sshd.pouch.queued") && s.ints.at("sshd.pouch.refused") == 1 && why() == kEvent && seq() == 6);
    s.event_state = 0; tick();
    // the title and a load refuse at once
    s.scene = 0x53656C74; tick(); assert(!act("pouch_to_3", 0)); s.scene = 0; tick(); assert(why() == kTitle);
    s.stage = ""; tick(); assert(!act("pouch_to_3", 0)); s.stage = "F200"; tick(); assert(why() == kLoad);
    s.counter = 1 << 10; tick(); // 5 unlocked slots: slots 5-7 take no drops
    assert(s.ints.at("sshd.pouch.slots") == 5 && !act("pouch_to_6", 0) && !act("stock_to_0", 6) && act("pouch_to_4", 0));
    answer(0); tick();
    // --- equip: tap/drag onto the Gear hub = ZR item (op 3, Gear wheel position -> item id), onto
    // the Pouch hub = R item (op 4, a pouch slot)
    s.counter = 4 << 10; tick();
    assert(!s.ints.at("sshd.bitem.ok") && !act("bitem_equip", 0)); // no Link
    tick(); assert(why() == kNoPlayer);
    s.player = 0x7100000; s.held = 0xB; tick();
    assert(s.ints.at("sshd.bitem.ok") == 1 && s.ints.at("sshd.ply.held") == 0xB && s.ints.at("sshd.ply.bsel") == 1);
    const uint32_t wheel_ids[8] = {1, 0, 3, 8, 4, 2, 6, 5}; // bow, bombs, beetle, net, slingshot, claws, whip, gust
    for (int pos = 0; pos < 8; ++pos) {
        assert(act("bitem_equip", pos));
        assert(mb[0x44 / 4] == 3 && mb[0x48 / 4] == wheel_ids[pos]);
        answer(0); tick();
    }
    // a newer equip replaces a queued one: the player's last choice is the one staged
    uint32_t n0 = seq();
    assert(act("bitem_equip", 0) && seq() == n0 + 1);
    assert(act("bitem_equip", 1) && act("bitem_equip", 2) && seq() == n0 + 1);
    assert(!act("pouch_equip", 1)); // the place holds a ZR equip: an R equip is busy
    answer(0); tick();
    assert(seq() == n0 + 2 && mb[0x44 / 4] == 3 && mb[0x48 / 4] == wheel_ids[2]);
    answer(0); tick();
    assert(!act("bitem_equip", 8) && !act("bitem_equip", -1) && !act("bitem_equip", 107));
    tick(); assert(why() == kPayload);
    // dev.40: the sword drawn (0x11, kept after a spin / swipe) equips, like the native wheel
    s.held = 0x11; tick();
    assert(s.ints.at("sshd.bitem.ok") == 1 && s.ints.at("sshd.why.bitem") == kOk);
    n0 = seq();
    assert(act("bitem_equip", 1) && seq() == n0 + 1 && mb[0x48 / 4] == 0);
    answer(0); tick();
    // ... except during the two sword attack states the native wheel refuses: the drop waits
    s.act = 0x68; tick();
    assert(s.ints.at("sshd.why.bitem") == kSwing && s.ints.at("sshd.bitem.ok") == 1);
    assert(act("bitem_equip", 2) && seq() == n0 + 1);
    tick(); s.act = 0x9A; tick(); assert(seq() == n0 + 1);
    s.act = 2; tick();
    assert(seq() == n0 + 2 && mb[0x48 / 4] == 3);
    answer(0); tick();
    // a B-item in Link's hand (the bow): the helper presses B for 4 frames to put it away, then the
    // equip commits on empty hands
    s.held = 1; tick();
    assert(s.ints.at("sshd.why.bitem") == kItem && s.ints.at("sshd.bitem.ok") == 1);
    mb[0x1C / 4] = 0;
    assert(act("bitem_equip", 4) && mb[0x1C / 4] == 4 && seq() == n0 + 2);
    tick(); tick(); mb[0x1C / 4] = 0; tick();
    assert(mb[0x1C / 4] == 0 && seq() == n0 + 2); // pressed once
    s.held = 0xB; tick();
    assert(seq() == n0 + 3 && mb[0x48 / 4] == 4);
    answer(0); tick();
    // an item that never goes away: refused after kPendWait (120) samples, "item in hand"
    s.held = 4; tick();
    assert(act("bitem_equip", 0) && mb[0x1C / 4] == 4);
    for (int i = 0; i < 121; ++i) tick();
    assert(!s.ints.at("sshd.pouch.queued") && s.ints.at("sshd.pouch.refused") == 1 && why() == kItem);
    assert(seq() == n0 + 3);
    s.held = 0xB; tick();
    // the native B-wheel open: the drop waits for it to close
    s.wheel = 1; tick();
    assert(s.ints.at("sshd.why.bitem") == kBusy && act("bitem_equip", 7) && seq() == n0 + 3);
    tick(); s.wheel = 0; tick();
    assert(seq() == n0 + 4 && mb[0x48 / 4] == wheel_ids[7]);
    answer(0); tick();
    // the native pouch wheel open (bit 2): an R equip waits likewise
    s.wheel = 4; tick();
    assert(s.ints.at("sshd.why.pouch") == kBusy && act("pouch_equip", 2) && seq() == n0 + 4);
    s.wheel = 0; tick();
    assert(seq() == n0 + 5 && mb[0x44 / 4] == 4 && mb[0x48 / 4] == 2);
    answer(0); tick();
    s.stage = "F020"; tick(); assert(!s.ints.at("sshd.bitem.ok") && !act("bitem_equip", 1));
    tick(); assert(why() == kLoftwing); s.stage = "F200";
    s.menu = 0x10; tick(); tick(); tick();
    assert(!act("bitem_equip", 1) && !act("pouch_equip", 1)); s.menu = 0; tick(); tick();
    tick();
    n0 = seq();
    assert(act("bitem_equip", 1) && seq() == n0 + 1);
    answer(7); tick(); // the helper found the item not owned / withheld: shown as refused
    assert(s.ints.at("sshd.pouch.refused") == 1 && why() == kGame);
    assert(act("pouch_equip", 2) && mb[0x44 / 4] == 4 && mb[0x48 / 4] == 2);
    answer(0); tick();
    // R equips ignore Link's hands: an item or the sword in hand is fine
    s.held = 4; tick();
    assert(s.ints.at("sshd.why.pouch") == kOk && act("pouch_equip", 1) && mb[0x44 / 4] == 4 && mb[0x48 / 4] == 1);
    answer(0); s.held = 0xB; tick();
    // dev.41: an unreadable action word (player+0x468) or native wheel byte
    // (+0x63B5) is not "no attack" / "wheel closed": the gates fail closed (17, unreadable) and
    // nothing is staged; once readable again they open
    constexpr int kUnread = 17;
    n0 = seq();
    s.held = 0x11; s.fail = s.player + 0x468; tick();
    assert(s.ints.at("sshd.why.bitem") == kUnread && !s.ints.at("sshd.bitem.ok"));
    assert(!act("bitem_equip", 1) && seq() == n0);
    tick(); assert(why() == kUnread);
    s.held = 0xB; tick(); // empty hands: the action word is not needed
    assert(s.ints.at("sshd.why.bitem") == kOk);
    s.fail = s.player + 0x63B5; tick();
    assert(s.ints.at("sshd.why.bitem") == kUnread && s.ints.at("sshd.why.pouch") == kUnread);
    assert(!act("bitem_equip", 1) && !act("pouch_equip", 1) && seq() == n0);
    s.fail = 0; tick();
    assert(s.ints.at("sshd.why.bitem") == kOk && s.ints.at("sshd.why.pouch") == kOk && s.ints.at("sshd.bitem.ok") == 1);
    assert(!act("pouch_equip", 102) && !act("pouch_equip", 8) && !act("pouch_equip", -1));
    s.counter = 1 << 10; tick(); // 5 unlocked slots
    assert(act("pouch_equip", 4)); answer(0); tick();
    assert(!act("pouch_equip", 5));
    const int64_t refusals = s.ints.at("sshd.refuse.n");
    assert(refusals > 10);

    // a request the helper never answers retires the pouch ops for the session
    s.counter = 4 << 10; tick();
    assert(act("pouch_to_7", 0));
    for (int i = 0; i < 130; ++i) tick();
    assert(!s.ints.at("sshd.pouch.ok") && !s.ints.at("sshd.pouch.helper") && !act("pouch_to_6", 0));
    tick(); assert(why() == kHelper);
    // a stopped helper (no loop heartbeat) is not offered either
    ext->configure(inst, &e);
    for (int i = 0; i < 40; ++i) api->sample(inst, &h);
    assert(!s.ints.at("sshd.pouch.helper") && !act("pouch_to_6", 0));
    api->destroy(inst);
}
