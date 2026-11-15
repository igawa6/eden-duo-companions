// SPDX-License-Identifier: GPL-3.0-or-later
// Skyward Sword HD Ready swipe pad, dev.39: the gold trail under the finger on a runtime that tells
// where it is (25: @view_touch_x/y:stickpad_<i>, absent while no finger is down), dev.38's trail out
// of the minimap centre on one that does not (runtime 22..24: get_f64 answers the caller's fallback
// for a key it does not know), chosen per stroke, and the same stick input either way.
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
constexpr uint64_t kSlot = 0x5AEAD54, kScene = 0x182A7F4, kEvents = 0x182DED8,
                   kEventObj = 0x7000000, kPlayerPtr = 0x2BF9940;

// The runtime's side of one pad: a pan_zoom view at zoom 1 whose pan moves against the finger, glides
// home a fifth of the way per tick after the lift (GlideViews), and (runtime 25) the live finger.
struct Pad {
    double pan_x = 0, pan_y = 0;
    bool down = false;
    double fx = 0, fy = 0;         // the finger, canvas px
    double lx = 0, ly = 0;         // where it landed
    double bx = 0, by = 0;         // the pan when it landed (a finger stops a glide where it is)
};

struct Host {
    bool rt25 = false;       // answers @view_touch_x/y / @view_touch_down
    bool blind = false;      // rt25 that does not report this stroke's finger (selection falls back)
    std::array<Pad, 2> pads;
    uint32_t mailbox[1024]{};
    uint8_t items[0x20]{};
    std::map<std::string, int64_t> ints; // this tick's publications (the snapshot starts empty)
    std::vector<std::array<int64_t, 3>> writes; // (x, y, ttl) every time the module publishes a vector
    int releases = 0;            // dev.48: ttl 0 stores (the stick handed back to the player at once)
    std::vector<std::string> logs;
    std::string stage = "F000";
    uint64_t player = 0x7100000;
    uint8_t menu = 0;            // a pause menu (dev.44: the controller state, sshd_menu_fixture.h)
    uint32_t event_state = 0;    // EventManager state (+0x248)
};

template <typename T> bool Put(void* out, size_t n, const T& v) {
    if (n != sizeof(T)) return false;
    std::memcpy(out, &v, n);
    return true;
}
EdenDsmodBool Read(void* p, uint64_t a, void* out, size_t n) {
    auto& s = *static_cast<Host*>(p);
    if (sshd_test::SwordFixtureRead(0, s.items, a, out, n)) return EDEN_DSMOD_TRUE;
    if (a == 0x1826AD0) return Put(out, n, uint64_t{0});
    if (a == kSlot + 0x531C && n == 8) {
        std::memset(out, 0, 8);
        std::memcpy(out, s.stage.data(), s.stage.size());
        return EDEN_DSMOD_TRUE;
    }
    if (a == kSlot + 0x531C && n == 1) return Put(out, n, static_cast<uint8_t>(s.stage[0]));
    if (a == kSlot + 0x9E0 && n == sizeof(s.items)) return std::memcpy(out, s.items, n), EDEN_DSMOD_TRUE;
    if (a == kScene) return Put(out, n, uint32_t{0});
    if (sshd_test::MenuFixtureRead(0, s.menu, a, out, n)) return EDEN_DSMOD_TRUE; // dev.44: the controller state
    if (a == kEvents) return Put(out, n, kEventObj);
    if (a == kEventObj + 0x248) return Put(out, n, s.event_state);
    if (a == kPlayerPtr) return Put(out, n, s.player);
    if (a == s.player + 0x6404) return Put(out, n, uint16_t{0xB});
    if (a == s.player + 0x6408) return Put(out, n, uint16_t{1});
    return EDEN_DSMOD_FALSE;
}

double Answer(Host& s, const char* k, double fb) {
    for (int i = 0; i < 2; ++i) {
        const Pad& p = s.pads[i];
        const std::string id = "stickpad_" + std::to_string(i);
        if (k == "@view_pan_x:" + id) return p.pan_x;
        if (k == "@view_pan_y:" + id) return p.pan_y;
        if (!s.rt25)
            continue; // runtime 22..24: not a key it knows, the caller's fallback
        if (k == "@view_touch_down:" + id) return p.down ? 1 : 0;
        if (p.down && !s.blind && k == "@view_touch_x:" + id) return p.fx;
        if (p.down && !s.blind && k == "@view_touch_y:" + id) return p.fy;
    }
    return fb;
}

struct Run {
    Host s;
    EdenDsmodHostApi h{};
    EdenDsmodHostExtensions e{};
    const EdenDsmodModuleApi* api = nullptr;
    const EdenDsmodModuleExtensions* ext = nullptr;
    void* inst = nullptr;
    int active = -1; // the pad the finger is on

    explicit Run(bool rt25) {
        s.rt25 = rt25;
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
        h.publish_i64 = [](void* p, const char* k, int64_t v) { static_cast<Host*>(p)->ints[k] = v; };
        h.publish_f64 = [](void*, const char*, double) {};
        h.publish_text = [](void*, const char*, const char*) {};
        h.get_f64 = [](void* p, const char* k, double fb) { return Answer(*static_cast<Host*>(p), k, fb); };
        h.log = [](void* p, uint32_t, const char* m) { static_cast<Host*>(p)->logs.emplace_back(m); };
        inst = api->create(&h, "");
        assert(inst);
        e.version = EDEN_DSMOD_EXT_VERSION; e.abi_hash = EDEN_DSMOD_EXT_HASH; e.struct_size = sizeof(e);
        e.userdata = &s; e.mailbox_address = 0x9000000; e.mailbox_size = 4096;
        e.load_u32 = [](void* p, uint32_t o, uint32_t* v) -> EdenDsmodBool { *v = static_cast<Host*>(p)->mailbox[o / 4]; return EDEN_DSMOD_TRUE; };
        e.store_u32 = [](void* p, uint32_t o, uint32_t v) -> EdenDsmodBool {
            auto& st = *static_cast<Host*>(p);
            st.mailbox[o / 4] = v;
            if (o == 0x08 && v != 0) // the ttl goes last: it publishes the vector
                st.writes.push_back({static_cast<int32_t>(st.mailbox[0x0C / 4]), static_cast<int32_t>(st.mailbox[0x10 / 4]), v});
            else if (o == 0x08)
                ++st.releases; // dev.48: Configure / Destroy and a stroke refused mid-drag
            return EDEN_DSMOD_TRUE;
        };
        s.items[0x9E5 - 0x9E0] = 1; // Goddess Sword
        ext->configure(inst, &e);
    }
    ~Run() { api->destroy(inst); }

    // one runtime tick: the module (sample, tick: each into a fresh snapshot, RunGameModule) sees the
    // pads as the last tick left them, then the host glides a released view home. `ints` = the tick's.
    void Tick() {
        ++s.mailbox[0x04 / 4];
        ++s.mailbox[0x68 / 4];
        s.ints.clear();
        api->sample(inst, &h);
        s.ints.clear();
        api->tick(inst, &h);
        for (Pad& p : s.pads) {
            if (p.down)
                continue;
            p.pan_x *= 0.8, p.pan_y *= 0.8;
            if (std::fabs(p.pan_x) + std::fabs(p.pan_y) <= 0.5)
                p.pan_x = p.pan_y = 0;
        }
    }
    void Down(int pad, double x, double y) {
        Pad& p = s.pads[pad];
        p.down = true;
        p.fx = p.lx = x, p.fy = p.ly = y;
        p.bx = p.pan_x, p.by = p.pan_y;
        active = pad;
    }
    void Move(double x, double y) {
        Pad& p = s.pads[active];
        p.fx = x, p.fy = y;
        p.pan_x = p.bx - (x - p.lx); // the pan moves against the finger
        p.pan_y = p.by - (y - p.ly);
    }
    void Up() { s.pads[active].down = false; }
    // a straight stroke: land at a, move to b over `ticks` ticks, lift
    void Stroke(int pad, double ax, double ay, double bx, double by, int ticks, int rest_after = 20) {
        Down(pad, ax, ay);
        Tick();
        for (int t = 1; t <= ticks; ++t) {
            Move(ax + (bx - ax) * t / ticks, ay + (by - ay) * t / ticks);
            Tick();
        }
        Up();
        for (int t = 0; t < rest_after; ++t)
            Tick();
    }
    int64_t I(const std::string& k) const {
        const auto it = s.ints.find(k);
        return it == s.ints.end() ? -999999 : it->second;
    }
    bool On(int k) const { return I("sshd.tr.on") == 1 && s.ints.count("sshd.tr" + std::to_string(k) + ".x") != 0; }
    bool Logged(const char* part) const { return Count(part) > 0; }
    int Count(const char* part) const {
        int n = 0;
        for (const auto& l : s.logs)
            n += l.find(part) != std::string::npos ? 1 : 0;
        return n;
    }
};

// The same finger script on both runtimes (strokes on both pads, a flick, a stroke that lands while
// the last one still glides home, one that runs over the right column)
void Script(Run& r) {
    r.Stroke(0, 500, 500, 650, 500, 10);               // slow, right
    r.Stroke(0, 700, 300, 560, 420, 3, 3);             // a fast flick down-left, then...
    r.Stroke(0, 560, 420, 700, 300, 3);                // ...back while the flick still glides home
    r.Stroke(1, 100, 400, 150, 600, 6);                // the left pane
    r.Stroke(0, 900, 500, 1150, 520, 8);               // over the spin column (fed as before)
}
} // namespace

int main() {
    // --- the stick input does not depend on the runtime: every vector written is the same
    {
        Run old(false), now(true);
        Script(old);
        Script(now);
        assert(!old.s.writes.empty());
        assert(old.s.writes == now.s.writes);
        assert(old.Logged("swipe trail from the minimap centre") && !old.Logged("under the finger"));
        assert(now.Logged("swipe trail under the finger") && !now.Logged("from the minimap centre"));
    }

    // --- runtime 22..24: dev.38's trail, its head at the minimap centre (620, 430) + the stroke
    {
        Run r(false);
        r.Down(0, 500, 500);
        r.Tick();
        r.Move(530, 480);
        r.Tick();
        assert(r.On(0) && r.I("sshd.tr0.x") == 650 && r.I("sshd.tr0.y") == 410);
        assert(r.I("sshd.tr0.a") == 15 && r.I("sshd.tr0.s") == 1000);
    }

    // --- runtime 25: under the finger, from where it landed
    {
        Run r(true);
        r.Down(0, 500, 500);
        r.Tick(); // a finger at rest: no stroke yet (no pan), no trail
        assert(r.I("sshd.tr.on") == 0);
        r.Move(530, 500);
        r.Tick();
        assert(r.On(0) && r.I("sshd.tr0.x") == 530 && r.I("sshd.tr0.y") == 500); // the head: the finger
        assert(r.I("sshd.tr0.a") == 15 && r.I("sshd.tr0.s") == 1000);
        assert(r.On(1) && r.I("sshd.tr1.x") == 519 && r.I("sshd.tr2.x") == 508); // back toward the landing
        assert(!r.On(3)); // ...and no further than it (30 px of path)
        for (int t = 1; t <= 6; ++t) {
            r.Move(530 + 40 * t, 500 - 20 * t);
            r.Tick();
            assert(r.I("sshd.tr0.x") == 530 + 40 * t && r.I("sshd.tr0.y") == 500 - 20 * t);
        }
        assert(r.On(23)); // the whole trail out now (the path is longer than 23 x 11 px)
        // the lift: the trail stays where the finger was and fades out in 14 ticks (as dev.38's)
        r.Up();
        int last = -1;
        for (int t = 0; t < 20; ++t) {
            r.Tick();
            if (r.I("sshd.tr.on") == 1) {
                assert(r.I("sshd.tr0.x") == 770 && r.I("sshd.tr0.y") == 380);
                last = t;
            }
        }
        // the host's first glide step shows the lift (t 1); then it fades, gone at kTrailFade (14)
        // ticks, like dev.38's trail
        assert(last == 13);
    }

    // --- runtime 25: the left pane, and a finger that lands while the last stroke still glides
    {
        Run r(true);
        r.Stroke(1, 100, 400, 100, 600, 5, 2); // lifted 2 ticks ago: pad 1 still glides home
        assert(std::fabs(r.s.pads[1].pan_y) > 50);
        r.Down(1, 120, 300);
        r.Tick();
        r.Move(160, 320);
        r.Tick();
        assert(r.I("sshd.tr0.x") == 160 && r.I("sshd.tr0.y") == 320); // the new finger, not the glide
        const double back = std::hypot(r.I("sshd.tr1.x") - 120.0, r.I("sshd.tr1.y") - 300.0);
        assert(back < 40); // toward its own landing point
    }

    // --- runtime 25: kept inside the pads. Near the right column a dot is shrunk to fit; over it, dropped;
    //     the canvas top is no limit
    {
        Run r(true);
        r.Down(0, 900, 500);
        r.Tick();
        r.Move(990, 500); // 30 px from stickpad_0's right edge (1020)
        r.Tick();
        assert(r.On(0) && r.I("sshd.tr0.x") == 990);
        const int64_t sc = r.I("sshd.tr0.s");
        assert(sc < 1000 && 990 + 44 * sc / 1000.0 <= 1020.5 && sc >= 600);
        r.Move(1100, 500); // over the spin column: the head dot is not drawn, the ones behind are
        r.Tick();
        assert(!r.On(0) && r.On(12));
        for (int k = 0; k < 24; ++k) {
            if (!r.On(k))
                continue;
            const double x = r.I("sshd.tr" + std::to_string(k) + ".x");
            const double half = 44 * r.I("sshd.tr" + std::to_string(k) + ".s") / 1000.0;
            assert(x + half <= 1020.5);
        }
        r.Up();
        for (int t = 0; t < 20; ++t) r.Tick();
        r.Down(0, 600, 60);
        r.Tick();
        r.Move(600, 20); // 20 px from the canvas top (stickpad_0 starts at y 0): full size
        r.Tick();
        assert(r.On(0) && r.I("sshd.tr0.s") == 1000 && r.I("sshd.tr0.y") == 20);
    }

    // --- per stroke: a stroke whose finger the runtime does not report falls back to dev.38's trail,
    //     and the next one that is reported is under the finger again
    {
        Run r(true);
        r.s.blind = true;
        r.Down(0, 500, 500);
        r.Tick();
        r.Move(530, 480);
        r.Tick();
        assert(r.I("sshd.tr0.x") == 650 && r.I("sshd.tr0.y") == 410); // from the minimap centre
        r.Up();
        for (int t = 0; t < 30; ++t) r.Tick();
        r.s.blind = false;
        r.Down(0, 500, 500);
        r.Tick();
        r.Move(530, 480);
        r.Tick();
        assert(r.I("sshd.tr0.x") == 530 && r.I("sshd.tr0.y") == 480);
        assert(r.Logged("from the minimap centre") && r.Logged("under the finger"));
    }
    // --- dev.43 (Thor RC: "Not now" over the pad with no log line): a refused stroke logs one
    //     "sshd: refused swipe" line with its reason and the LBlocked code, not one per sample; the
    //     stick is not fed; the next refused stroke logs its own line
    {
        Run r(true);
        r.s.menu = 0x10; // a pause menu: debounced (3 reads) and held kLBlockTicks ticks
        for (int t = 0; t < 6; ++t) r.Tick();
        const size_t fed = r.s.writes.size();
        r.Down(0, 500, 500);
        r.Tick();
        for (int t = 1; t <= 12; ++t) {
            r.Move(500 + 15 * t, 500);
            r.Tick();
        }
        assert(r.I("sshd.swipe.refused") == 1);
        r.Up();
        for (int t = 0; t < 20; ++t) r.Tick();
        assert(r.s.writes.size() == fed);
        assert(r.Count("sshd: refused swipe") == 1);
        assert(r.Count("sshd: refused swipe (menu: stroke 1, lblocked 2, l_block ") == 1);
        // dev.44: the line names the controller state behind the menu gate and where it was read
        assert(r.Logged("no_sword 0): held 0xb") && r.Logged("menu Pause direct (run "));
        r.Stroke(0, 500, 500, 650, 500, 10);
        assert(r.Count("sshd: refused swipe") == 2 && r.Logged("(menu: stroke 2, lblocked 2"));
        assert(r.s.writes.size() == fed);
        // free play again: a stroke is fed and logs no refusal
        r.s.menu = 0;
        for (int t = 0; t < 6; ++t) r.Tick();
        r.Stroke(0, 500, 500, 650, 500, 10);
        assert(r.s.writes.size() > fed && r.Count("sshd: refused swipe") == 2);
    }
    // --- dev.43: an interaction prompt is no event. At a bird statue with the native "Examine"
    //     prompt up the gate words read as plain play (live, Skyloft statue: menu 0x00, EventManager
    //     state 0): the stroke is fed and nothing is refused. The statue's own dialogue (A: Examine)
    //     is an event (live: EventManager state 2): refused, one line naming the event
    {
        Run r(true);
        r.s.menu = 0, r.s.event_state = 0; // the Examine prompt
        for (int t = 0; t < 6; ++t) r.Tick();
        const size_t fed = r.s.writes.size();
        r.Stroke(0, 620, 250, 620, 650, 15);
        assert(r.s.writes.size() > fed && r.Count("sshd: refused") == 0);
        r.s.event_state = 2; // Examine pressed: the Bird Statue dialogue
        for (int t = 0; t < 6; ++t) r.Tick();
        const size_t before = r.s.writes.size();
        r.Stroke(0, 620, 250, 620, 650, 15);
        assert(r.s.writes.size() == before);
        assert(r.Count("sshd: refused swipe (event: stroke 2, lblocked 3,") == 1 && r.Logged(" ev 2 stage F000"));
        r.s.event_state = 0; // cancelled: play again, the next stroke is fed
        for (int t = 0; t < 6; ++t) r.Tick();
        r.Stroke(0, 620, 250, 620, 650, 15);
        assert(r.s.writes.size() > before && r.Count("sshd: refused") == 1);
    }
    // --- dev.48: a stroke fed in play and refused mid-drag (a pause menu opens under the finger) hands
    //     the stick back at once: ttl 0, once, instead of the last vector for up to kTtl passes
    {
        Run r(true);
        for (int t = 0; t < 6; ++t) r.Tick();
        r.Down(0, 500, 500);
        r.Tick();
        for (int t = 1; t <= 4; ++t) {
            r.Move(500 + 15 * t, 500);
            r.Tick();
        }
        const size_t fed = r.s.writes.size();
        assert(fed > 0 && r.s.mailbox[0x08 / 4] != 0); // the finger reached the stick
        const int released = r.s.releases;
        r.s.menu = 0x10;
        for (int t = 5; t <= 14; ++t) {
            r.Move(500 + 15 * t, 500);
            r.Tick();
        }
        assert(r.I("sshd.swipe.refused") == 1 && r.Count("sshd: refused swipe (menu: stroke 1") == 1);
        assert(r.s.releases == released + 1 && r.s.mailbox[0x08 / 4] == 0);
        r.Up();
        for (int t = 0; t < 20; ++t) r.Tick();
        assert(r.s.mailbox[0x08 / 4] == 0 && r.s.releases == released + 1);
        r.s.menu = 0;
    }
    std::puts("sshd trail: ok");
    return 0;
}
