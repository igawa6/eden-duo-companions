// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Legend of Zelda: Skyward Sword HD (01002DA013484000) companion module, game version 1.0.1
// (main build 5441AF3C89F97E1D25A8CCF14C742BF82336D8A8) only.
//
// The game values (Link's position, rupees, stage id) are plain declarative points in the
// package's per-build data file. The module serves pictures and drives the right-stick helper: the game keeps its art in
// Wii-era formats the host does not decode (U8 archives, GX TPL textures, RFNT fonts, LZ11
// stage archives with KCL collision meshes), so `module:sshd:<key>` images are built here from
// the player's own romfs on request (sshd_art.cpp). Guest-side effects go only through the
// format-2 helper's mailbox (native/helper/sshd_stick_helper.S): the right-stick mirror and the
// Adventure Pouch / Item Check ops, which the helper runs on the game thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__GLIBC__) || defined(__ANDROID__)
#include <malloc.h>
#endif

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "sshd_art.h"
#include "sshd_cone.h"
#include "sshd_stroke.h"
#include "sshd_dpad.h"
#include "sshd_dungeon.h"
#include "sshd_hudhide.h"
#include "sshd_story.h"

namespace {

constexpr uint64_t kTitleId = 0x01002DA013484000ull;
// Main NSO build id of version 1.0.1 (update v65536), 20 bytes.
constexpr uint8_t kBuildId[20] = {0x54, 0x41, 0xAF, 0x3C, 0x89, 0xF9, 0x7E, 0x1D, 0x25, 0xA8,
                                  0xCC, 0xF1, 0x4C, 0x74, 0x2B, 0xF8, 0x23, 0x36, 0xD8, 0xA8};
constexpr std::string_view kBuildHex = "5441AF3C89F97E1D25A8CCF14C742BF82336D8A8";
constexpr std::string_view kKeyPrefix = "module:sshd:";

// Instance::floor_pick: kFloorAuto = follow Link's floor; kFloorSlotPick + i = tab i tapped (not
// yet resolved); else the native floor picked
constexpr int kFloorAuto = 99, kFloorSlotPick = 100;

struct StickStep { // one step of a helper stick sequence (spin, late-stroke flick)
    int16_t x, y;
    uint32_t frames;
};

// Values of the live-object walk (PublishLiveMarkers), republished on every sample: the host
// builds a new snapshot per sample, and the walk itself runs on every 8th.
struct LivePub {
    std::vector<std::pair<std::string, int64_t>> ints;
    std::vector<std::pair<std::string, double>> floats;
    std::vector<std::pair<std::string, std::string>> texts;
    void i(const char* k, int64_t v) { ints.emplace_back(k, v); }
    void f(const char* k, double v) { floats.emplace_back(k, v); }
    void t(const char* k, const char* v) { texts.emplace_back(k, v); }
    // the walk's neighbour plates (PublishPlates places them on screen every sample)
    struct Plate {
        int shown, deg, region;
        double u, v;
    };
    std::vector<Plate> plates;
    void Clear() {
        ints.clear();
        floats.clear();
        texts.clear();
        plates.clear();
    }
    void Publish(const EdenDsmodHostApi* host) const {
        for (const auto& [k, v] : ints)
            host->publish_i64(host->userdata, k.c_str(), v);
        for (const auto& [k, v] : floats)
            host->publish_f64(host->userdata, k.c_str(), v);
        for (const auto& [k, v] : texts)
            host->publish_text(host->userdata, k.c_str(), v.c_str());
    }
};

// dev.41: one publisher's values with the inputs they were computed from. The host
// clears its snapshot every tick (mod_module_host.cpp RunGameModule) and redraws only when a bound
// value changed (mod_redraw.cpp UiSignature / BuildRenderExtras), so every value is still published
// every sample; a sample whose inputs match the cached ones republishes the cached values instead
// of computing them again (label fitting: FitAnchor for every plate / island name).
struct PubCache {
    bool valid = false;
    std::vector<double> key;
    LivePub pub;
    bool Hit(const std::vector<double>& k) const { return valid && k == key; }
};

// Diagnostics (dev.37), read once from the environment at Create; all off in normal use.
//   SSHD_DIAG=1            log the hide list / cone / L-lock events and per-state frame counts (the
//                          helper's draw probe on the Cross base pane, mailbox +0x410..)
//   SSHD_DIAG_LEGACY=1     replay dev.36: the hide list cleared in menus / without Help / on a refused
//                          sample, count 0 written before every change, 30-frame ttl; the cone
//                          switched to the wanted picture at once (the "before" of a measurement)
//   SSHD_DIAG_THROTTLE=lo,hi,pct,ms  Thor-like sampling: the hide list and the cone are updated
//                          only every lo..hi ms (uniform), and pct % of the waits last ms instead
//   SSHD_DIAG_SLOW_CONE=ms a cone picture takes ms longer to build (a handheld's asset worker)
//   SSHD_DIAG_LEGACY_PAD=1 the Ready swipe pad's dev.17..37 logic (LegacyPadMirror), for the dev.38
//                          stroke log's "before"; SSHD_DIAG=1 logs one "sshd stroke" line per stroke
//   SSHD_DIAG_HOLD_TICKS=n the swipe trail and refusal stay on screen n ticks after the lift (a
//                          test driver still is taken seconds after its request, once the stroke is over);
//                          dev.40: the Gear / Bag / spin / bash refusals stay up as many samples
//   SSHD_DIAG_TRAIL_MS=lo,hi  dev.39 Thor-like finger sampling for the trail only: the finger trail
//                          takes a new finger position at most every lo..hi ms (uniform); the stick
//                          is fed as always
//   SSHD_DIAG_MENU_BLIP=pct dev.40: pct % of the gate reads see a pause state for that one read (dev.40:
//                          the stack slot's single-sample blips, made frequent); with SSHD_DIAG_THROTTLE
//                          the gate words are also read on the throttled samples only
//   SSHD_DIAG_MENU_RAW=1   dev.39's menu rule (one menu sample is a menu, one play sample closes it), the
//                          "before" of the menu debounce measurement
//   SSHD_DIAG_MENU_DIRECT=1 dev.44: ignore the helper's game-thread menu sample (the module's own read of
//                          the controller state, the fallback path, runs on every sample)
//   SSHD_DIAG_PRESS=id     dev.46: keep button id (gen_manifest PRESS) in its pressed look, for stills
//                          (a page-changing button's page is frozen by the runtime at the tap)
struct Diag {
    bool on = false, legacy = false, legacy_pad = false;
    int hold_ticks = 0; // SSHD_DIAG_HOLD_TICKS: the trail / swipe refusal stay up this long after a lift
    int trail_lo = 0, trail_hi = 0; // SSHD_DIAG_TRAIL_MS
    std::chrono::steady_clock::time_point trail_next{};
    int lo = 0, hi = 0, pct = 0, stall = 0, slow_cone = 0;
    int menu_blip = 0; // SSHD_DIAG_MENU_BLIP
    int press = 0;     // SSHD_DIAG_PRESS (dev.46)
    bool menu_raw = false; // SSHD_DIAG_MENU_RAW
    bool menu_direct = false; // SSHD_DIAG_MENU_DIRECT
    uint32_t rng = 0x2545F491u;
    std::chrono::steady_clock::time_point next{};
    bool due = true; // this sample updates the hide list and the cone
    uint64_t probe = 0;
    uint32_t last_drawn = 0, last_skipped = 0, last_frames = 0;
    uint64_t drawn[3]{}, skipped[3]{}, frames[3]{}; // play / menu / other (title, no stage)
    uint64_t writes = 0, clears = 0, holds = 0, samples = 0, due_samples = 0;
    uint64_t cone_on = 0, cone_blank = 0, cone_switch = 0, cone_held = 0;
    bool cone_ready_once = false; // blank samples count from the first loaded picture on (before
                                  // it there is nothing to hold: the first picture is still building)
    int last_shown = -1;
    void Init() {
        const auto num = [](const char* k) {
            const char* v = std::getenv(k);
            return v != nullptr ? std::atoi(v) : 0;
        };
        on = num("SSHD_DIAG") != 0;
        legacy = num("SSHD_DIAG_LEGACY") != 0;
        legacy_pad = num("SSHD_DIAG_LEGACY_PAD") != 0;
        hold_ticks = num("SSHD_DIAG_HOLD_TICKS");
        slow_cone = num("SSHD_DIAG_SLOW_CONE");
        menu_blip = num("SSHD_DIAG_MENU_BLIP");
        press = num("SSHD_DIAG_PRESS");
        menu_raw = num("SSHD_DIAG_MENU_RAW") != 0;
        menu_direct = num("SSHD_DIAG_MENU_DIRECT") != 0;
        if (const char* t = std::getenv("SSHD_DIAG_THROTTLE"); t != nullptr)
            std::sscanf(t, "%d,%d,%d,%d", &lo, &hi, &pct, &stall);
        if (const char* t = std::getenv("SSHD_DIAG_TRAIL_MS"); t != nullptr)
            std::sscanf(t, "%d,%d", &trail_lo, &trail_hi);
    }
    // SSHD_DIAG_TRAIL_MS: may the finger trail take a finger position now?
    bool TrailDue() {
        if (trail_hi <= 0)
            return true;
        const auto now = std::chrono::steady_clock::now();
        if (now < trail_next)
            return false;
        const int ms = trail_lo + static_cast<int>(Rand() % static_cast<uint32_t>(std::max(1, trail_hi - trail_lo + 1)));
        trail_next = now + std::chrono::milliseconds(ms);
        return true;
    }
    uint32_t Rand() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }
    // once per sample: is this a sample the throttled module "runs"?
    void Tick() {
        ++samples;
        if (hi <= 0) {
            due = true;
            ++due_samples;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        due = now >= next;
        if (due) {
            ++due_samples;
            int ms = lo + static_cast<int>(Rand() % static_cast<uint32_t>(std::max(1, hi - lo + 1)));
            if (pct > 0 && static_cast<int>(Rand() % 100u) < pct)
                ms = stall;
            next = now + std::chrono::milliseconds(ms);
        }
    }
};

// dev.40 (owner Thor report 2026-11-13: "sometimes ... (R/ZR) from the bottom screen, it's said 'not
// now'"): every bottom-screen game action (Gear / Bag equips and moves, spins, shield bash) reads the
// same gameplay words once per sample into a GateIn and refuses with one of these reasons, logged
// with the raw values and published (sshd.why.* the gate now, sshd.refuse.why the last refusal).
enum GateWhy : int {
    kWhyOk = 0,
    kWhyHelper = 1,   // no mailbox, the helper retired after a timeout, or its loop heartbeat stalled
    kWhyTitle = 2,    // the title / Quest Logs scene
    kWhyLoad = 3,     // no stage in the live slot (a load)
    kWhyMenu = 4,     // a pause / Collection / map menu (debounced, see MenuGate)
    kWhyEvent = 5,    // an event: cutscene, dialogue, door, item get (EventManager state != 0)
    kWhyLoftwing = 6, // riding the Loftwing (F020): no item wheel, no sword
    kWhyNoPlayer = 7, // no Link (dAcPy*) to act on
    kWhyItem = 8,     // an item other than the sword in Link's hands (a flick would aim it)
    kWhySwing = 9,    // a sword swing / spin is playing (the native ZR wheel refuses then too)
    kWhyNoSword = 10, // no sword yet
    kWhyNoShield = 11,// no usable shield in the pouch
    kWhyRealm = 12,   // a Silent Realm (no sword, no shield)
    kWhyBusy = 13,    // the one queue place is taken (an op / a sequence already waits)
    kWhyPayload = 14, // a bad payload or a locked pouch slot
    kWhyGame = 15,    // the helper refused on the game thread (its code is logged)
    kWhyWait = 16,    // a queued op waited too long for free play
    kWhyUnread = 17,  // the gameplay words were unreadable
    kWhyShort = 18,   // dev.45: a swipe too short to be a slash (its peak never reached kFlickMin)
    kWhyNotDrawn = 19,// dev.48: the sword stab with Link's hands empty (R3 stabs only with the sword drawn)
    kWhySwim = 20,    // dev.48: the sword stab while Link is in water
};
const char* WhyName(int why) {
    static const char* const kNames[] = {"ok",    "helper", "title", "load",    "menu",   "event",
                                         "loftwing", "no player", "item in hand", "swing", "no sword",
                                         "no shield", "silent realm", "busy", "payload", "game refused",
                                         "waited too long", "unreadable", "too short", "sword not drawn",
                                         "swimming"};
    return why >= 0 && why < static_cast<int>(sizeof(kNames) / sizeof(kNames[0])) ? kNames[why] : "?";
}

// The raw words behind the gates (read once per sample by ReadGate; an action re-reads them).
struct GateIn {
    bool read = false;      // stage, scene and event words readable
    char stage[9]{};        // live slot stage name
    uint32_t scene = 0;     // main+0x182A7F4 ('Selt' on the title / Quest Logs)
    // (dev.44: the pause / map state is no longer a GateIn word: MenuSample / MenuGate)
    uint64_t events = 0;    // EventManager*
    uint32_t ev = 0;        // its state, 0 = no event
    uint64_t player = 0;    // dAcPy*
    int held = -1, bsel = -1; // player+0x6404 item in hand (0xB none, 0x11 sword), +0x6408 ZR item
    int64_t act = -1;       // player+0x468 action id (0x68 / 0x9A: sword attack states)
    int wheel = -1;         // player+0x63B5 (+0x6054 + 0x361): the native B-wheel's open bits
    int wcur = -2;          // player+0x63E2 (+0x6054 + 0x38E): its cursor slot (-1 none)
};

// dev.39: the Ready swipe pads (gen_manifest stickpad_0 / stickpad_1 rects) and the canvas: the finger
// trail stays inside the pads (sshd::FingerTrail::SetArea); test_manifest checks them against the manifest
constexpr sshd::FingerTrail::Box kPadRects[2] = {{220, 0, 800, 916}, {0, 220, 220, 640}};
constexpr sshd::FingerTrail::Box kCanvas = {0, 0, 1240, 1080};

struct Instance {
    EdenDsmodHostApi host{}; // copy: callback pointers are only borrowed
    Diag diag;
    std::mutex art_mutex;    // load_image runs on the asset worker
    std::unique_ptr<sshd::Art> art;
    EdenDsmodHostExtensions ext{}; // copy; mailbox present only when the load plan matched
    bool mailbox = false;
    bool dragging = false;
    double drag_x0 = 0, drag_y0 = 0; // last pan seen
    bool releasing = false;
    uint32_t last_heartbeat = 0, heartbeat_still = 0;
    bool hud_applied = false; // the companion switched the native Screen Display to Simple
    uint8_t hud_orig = 0;     // the option byte before that, restored on Off / exit
    // Adventure Pouch and equip ops through the helper's main-loop hook (PouchStage / PouchPoll)
    uint32_t pouch_seq = 0;      // last staged request
    int pouch_wait = 0;          // samples since it was staged, 0 = none in flight
    bool pouch_dead = false;     // a request timed out: the helper is retired for this session
    uint32_t loop_beat = 0;      // last seen loop heartbeat
    int loop_still = 1000;       // samples without a heartbeat change
    bool pouch_ok = false;       // gameplay state allows an op now (published sshd.pouch.ok)
    int pouch_slots = 0;         // unlocked pouch slots (native getPouchSlotCount)
    int pouch_flash = 0;         // samples left to show the last refusal
    int pouch_code = 0;          // last helper result code
    bool bitem_ok = false;       // a B-item equip may be staged now (published sshd.bitem.ok)
    // spin attacks: stick sequences played by the helper's loop hook (SpinPoll / SpinAction)
    uint32_t seq_id = 0;         // last staged sequence
    int seq_wait = 0;            // samples since it was staged, 0 = none in flight
    bool seq_dead = false;       // a sequence never finished: spins off for this session
    bool spin_ok = false;        // a spin may be staged now (published sshd.spin.ok)
    int seq_flash = 0;           // samples left to show the last refusal ("Not now")
    int seq_code = -1;           // last helper result (-1 none yet)
    uint32_t seq_quiet = 0;      // id of a late-stroke flick: its refusal is not a spin's "Not now"
    uint32_t seq_quiet_stroke = 0; // dev.45: the stroke that flick came from (its refusal is logged as that swipe's)
    // a stick sequence waiting for the game to see L released (L toggle on; LTick stages it)
    StickStep pend[8]{};
    int pend_n = 0, pend_wait = 0;
    bool pend_quiet = false;
    // L-hold toggle (Ready bottom-right corner since dev.47, owner 2026-11-11): see LTick
    bool l_on = false;       // the player's L lock (eye + lock badge; dev.37 a persistent mode)
    bool l_suspend = false;  // a stroke / sequence owns the right stick: the virtual L is let go
    int l_resume = 0;        // ticks left before the hold is asserted again after a stroke
    int l_engage = 0;        // ticks left before a paused lock holds L again (kLReengage)
    int l_reason = 0;        // last pause / end of the lock (published sshd.l.reason)
    // stroke while the game still sees L: the finger's peak vector, replayed as a flick once L is up
    int stroke_wait = 0;     // ticks the stroke has waited for the release
    bool stroke_fed = false; // the stroke reached the stick words
    double peak_x = 0, peak_y = 0;
    // dev.45: a stroke's late flick that ended while another sequence still owned the stick waits here
    // (LTick moves it into pend once the stick is free, or refuses it after kSpinQueueWait ticks)
    bool flick_q = false;
    StickStep flick_steps[2]{};
    int flick_q_wait = 0;
    uint32_t flick_stroke = 0; // the stroke it came from (log lines)
    uint32_t pend_stroke = 0;  // likewise for the flick waiting in pend (0 = a spin)
    // dev.38 swipe pad (StickMirror, sshd_stroke.h)
    sshd::Stroke stroke;
    std::array<sshd::PanStroke, 2> pads; // stickpad_0 / stickpad_1: the finger from each pad's pan
    bool stroke_refused = false; // this stroke met a state where the game cannot slash
    int swipe_flash = 0;         // samples left to show the refusal ("Not now" over the pad)
    int legacy_skip = 0;         // diagnostics: legacy pad ticks a moving finger was not fed
    std::array<sshd::Stroke::Dot, sshd::Stroke::kTrailPoints> trail{}; // StickMirror's last trail
    // dev.39: the trail under the finger where the runtime tells where it is (25,
    // @view_touch_x/y:stickpad_<i>); else dev.38's trail out of the minimap centre (stroke.Trail)
    sshd::FingerTrail ftrail;
    struct Land {
        bool on = false;
        double x = 0, y = 0;
    };
    std::array<Land, 2> land{}; // where each pad's finger landed (its first finger sample)
    int trail_pad = -1;         // the pad of the current / last stroke
    int trail_mode = -1;        // the trail kind last logged: 0 from the minimap centre, 1 under the finger
    uint32_t stroke_id = 0;
    struct StrokeLog {           // diagnostics (SSHD_DIAG): the stroke log line
        bool open = false, legacy = false;
        uint32_t id = 0;
        sshd::Stroke::Stats s{};
        int post = 0, gate = 0, lwait = 0, spin = 0, replay = 0, skipped = 0;
        int trail = 0, fsamples = 0; // dev.39: 1 = the trail under the finger, its finger samples
        double lx = NAN, ly = NAN, hx = NAN, hy = NAN; // where the finger landed / the trail's head
        std::array<uint32_t, 8> acts{};
        int nacts = 0;
    } sdiag;
    int l_block = 0;         // consecutive ticks a game state blocked the hold (LTick debounce)
    int l_up = 0;            // consecutive ticks the game has seen L up
    int bash_flash = 0;      // samples left to show a refused shield bash ("Not now")
    // dev.46 pressed look (PressTick): the clock the manifest's press countdown reads (sshd.ms)
    std::chrono::steady_clock::time_point press_t0 = std::chrono::steady_clock::now();
    int64_t stab_at = -1;    // dev.48: ClockMs of the last staged sword stab (the map rim's lit look)
    // dev.40 gates (ReadGate / MenuGate / Refuse)
    GateIn gate;             // this sample's gameplay words
    int menu_run = 0;        // consecutive samples that saw a pause / map state
    int menu_quiet = 0;      // consecutive samples that saw play
    bool menu_open = false;  // the debounced pause-menu state the gates use
    int menu_span = 0;       // samples since the debounced state last changed (dev.43 MenuGate line)
    // dev.44 (MenuSample): this sample's menu state, where it came from, and the unread grace
    int menu_code = 0;       // kMenuPlay / kMenuOpen / kMenuNone / kMenuUnread
    int menu_src = 0;        // kSrcHelper (the loop hook's game-thread sample) / kSrcDirect / kSrcNone
    uint32_t menu_delta = 0; // the controller state's distance from StateID_Normal (log: its name)
    int menu_unread_n = 0;   // consecutive samples with no fresh helper sample and no direct read
    bool menu_unread = false;// ... for longer than kMenuUnreadGrace: the gates refuse as "unreadable"
    int diag_menu_code = 0;  // SSHD_DIAG gate watch: the last logged menu state
    int why_pouch = 0, why_bitem = 0, why_spin = 0, why_bash = 0; // each gate now (sshd.why.*)
    int refuse_why = 0;      // the last refusal's reason (sshd.refuse.why)
    int fallback_why = -1;   // the no-helper swipe fallback's gate as last logged (-1 helper ready)
    uint32_t refusals = 0;   // refusals this session (sshd.refuse.n)
    GateIn diag_gate;        // SSHD_DIAG gate watch: the last logged words
    uint64_t diag_gate_n = 0;
    // one queued Gear / Bag op: a drop that arrives while an op is in flight, an event blips, a
    // sword swing plays, the native wheel is open or (ZR) an item is being put away (PendPoll)
    struct PendOp {
        bool on = false;
        uint32_t op = 0, a = 0, b = 0;
        int wait = 0;          // samples it has waited
        int why = 0;           // the gate it last waited on
        bool put_away = false; // the B press that puts Link's item away was sent for it
        char act[16]{};
    } pend_op;
    int spin_q = 0;          // a queued spin (1 horizontal, 2 vertical) waiting for the one in flight
    int spin_q_wait = 0;
    int spin_why = 0, bash_why = 0; // the last spin / bash refusal's reason (the "Put away" hint)
    sshd::DpadReader dpad;   // Ready D-pad cluster (display only; PublishDpad)
    sshd::HideResolver hide_res; // top-screen HUD hiding (ApplyTopHide): panes to skip
    sshd::HudHide hide;          // ... and what the mailbox list currently holds
    uint32_t hide_bank = 0;      // ... in which bank (the helper reads +0x210 + 0x100 * bank)
    int hide_groups = 0;         // last published sshd.hud.hidden (republished on a throttled sample)
    sshd::ConeHold cone;         // Link's view cone: the last loaded picture (PublishCone)
    sshd::ConeHold::Out cone_out; // ... last published (republished on a throttled sample)
    char live_stage[9]{};    // PublishLiveMarkers: stage of the last object walk
    int live_tick = 0;       // samples since that walk
    LivePub live_pub;        // that walk's values
    PubCache sky_cache;      // dev.41: PublishSkyLayers' values and inputs
    PubCache plate_cache;    // dev.41: PublishPlates' values and inputs
    // on-screen label sizes (sshd_plate.h), measured once from the art: neighbour plates by label,
    // island names by index. state 0 = not yet, 1 = known, -1 = failed (retried after `retry` samples)
    struct LabelSize {
        int state = 0, retry = 0;
        sshd::PlaceSize place{};
        sshd::Box box{};
    };
    LabelSize place_size[13];
    LabelSize isle_size[7];
    // dungeon floor tabs (dev.30): the floor the player picked on the Map page (kFloorAuto = follow
    // Link). OnAction writes it; the pick is dropped on a new stage or once the Map page is closed.
    std::atomic<int> floor_pick{kFloorAuto};
    char floor_stage[9]{};  // stage of the pick
    // dev.32: the field picture's past variant, latched when the stage loads like the native
    // MapRelated (its constructor reads the selector once; a flag change shows after a reload)
    char fmap_stage[9]{};
    int fmap_past = -1;
    int map_closed = 0;     // samples the Map page's dungeon view has been absent
    int live_key = -1;      // PublishLiveMarkers: shown floor / Map / pick of the last walk
    // the native story-flag table (sshd_story.h), read once per table address
    uint64_t story_addr = 0;
    sshd::StoryTable story_table;
    int story_why = 1;      // dev.41: the last story read's reason (StoryLazy; sshd.story.why)
};

// Native Options > Screen Display: bit 7 of the byte at main+0x5AF7730 (0 Normal, 1 Simple; found by
// cycling the option in the Quest Logs Options menu and narrowing main .bss, 2026-11-07). Simple is
// the game's own reduced top-screen HUD (hearts stay). Settings flag hud.simple (default off).
constexpr uint64_t kScreenDisplay = 0x5AF7730;
void ApplyHud(Instance* inst, const EdenDsmodHostApi* host) {
    if (host->read_memory == nullptr || host->write_memory == nullptr || host->get_i64 == nullptr)
        return;
    const bool want = host->get_i64(host->userdata, "@flag:hud.simple", 0) != 0;
    const uint64_t a = host->main_base + kScreenDisplay;
    uint8_t v = 0;
    if (!host->read_memory(host->userdata, a, &v, 1))
        return;
    if (want && !inst->hud_applied) {
        inst->hud_orig = v;
        inst->hud_applied = true;
    }
    if (want && (v & 0x80) == 0) {
        v |= 0x80;
        host->write_memory(host->userdata, a, &v, 1);
    } else if (!want && inst->hud_applied) {
        v = static_cast<uint8_t>((v & 0x7F) | (inst->hud_orig & 0x80));
        host->write_memory(host->userdata, a, &v, 1);
        inst->hud_applied = false;
    }
}

// Right-stick mirror mailbox (native/helper/sshd_stick_helper.S).
constexpr uint32_t kMbMagic = 0x44485353u, kOffMagic = 0x00, kOffHeartbeat = 0x04, kOffTtl = 0x08,
                   kOffX = 0x0C, kOffY = 0x10; // +0x14 sampled npad buttons (GameL)
// The owner-approved virtual buttons: game frames the stick hook keeps adding each one to the
// controller's button word (the loop hook counts them down; dev.48: none while the helper's menu
// sample is not play, and the L lock's L not during an event).
constexpr uint32_t kOffBash = 0x18;    // L3, the shield bash
constexpr uint32_t kOffPutAway = 0x1C; // B, a put-away before a ZR equip (dev.40)
constexpr uint32_t kOffLHold = 0x20;   // L, the L lock (dev.45)
constexpr uint32_t kOffStab = 0x24;    // R3, the Ready map tap's sword stab (dev.47)
constexpr int kSwipeFlash = 60;      // samples a refused swipe's "Not now" stays over the pad
constexpr uint32_t kTtl = 40;          // helper passes (up to 4 per frame) a vector stays valid

// dev.48: a host reload (Configure) and unloading (Destroy) release every virtual button and the stick
// vector at once (dev.45..47 released only the L lock's L; the others lapsed after a few frames).
void ReleaseInputs(const EdenDsmodHostExtensions& e) {
    for (const uint32_t off : {kOffTtl, kOffBash, kOffPutAway, kOffLHold, kOffStab})
        e.store_u32(e.userdata, off, 0);
}
constexpr double kStickTravelPx = 180; // canvas px of finger travel for a full deflection
// The pad's pan_y grows as the finger moves up (+200 for a 200 px upward drag, live 2026-11-07).
constexpr double kPanSignX = -1.0; // pan_x falls as the finger moves right

EdenDsmodBool SupportsBuild(const char* build_hex) {
    if (build_hex == nullptr) {
        return EDEN_DSMOD_FALSE;
    }
    const std::string_view b(build_hex);
    if (b.size() < kBuildHex.size() || b.substr(0, kBuildHex.size()) != kBuildHex) {
        return EDEN_DSMOD_FALSE;
    }
    for (const char c : b.substr(kBuildHex.size())) {
        if (c != '0') {
            return EDEN_DSMOD_FALSE; // the 32-byte id is the 20-byte NSO id plus zero padding
        }
    }
    return EDEN_DSMOD_TRUE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != kTitleId) {
            return nullptr;
        }
        if (std::memcmp(host->build_id, kBuildId, sizeof(kBuildId)) != 0) {
            return nullptr;
        }
        for (size_t i = sizeof(kBuildId); i < sizeof(host->build_id); ++i) {
            if (host->build_id[i] != 0) {
                return nullptr;
            }
        }
        auto inst = std::unique_ptr<Instance>(new (std::nothrow) Instance{});
        if (inst == nullptr) {
            return nullptr;
        }
        inst->host = *host;
        inst->diag.Init();
        inst->stroke.SetTrailHold(inst->diag.hold_ticks);
        inst->ftrail.SetHold(inst->diag.hold_ticks);
        inst->ftrail.SetArea(kPadRects, 2, kCanvas);
        if (host->log != nullptr) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "sshd: module ready, main base %#llx",
                          static_cast<unsigned long long>(host->main_base));
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
        const EdenDsmodHostApi* h = &inst->host;
        inst->art = std::make_unique<sshd::Art>(
            [h](const std::string& path) -> std::optional<std::vector<uint8_t>> {
                if (h->read_romfs == nullptr) {
                    return std::nullopt;
                }
                const size_t size = h->read_romfs(h->userdata, path.c_str(), 0, nullptr, 0);
                if (size == 0 || size > (64u << 20)) {
                    return std::nullopt;
                }
                std::vector<uint8_t> bytes(size);
                if (h->read_romfs(h->userdata, path.c_str(), 0, bytes.data(), size) != size) {
                    return std::nullopt;
                }
                return bytes;
            });
        return inst.release();
    } catch (...) {
        return nullptr;
    }
}

void ClearTopHide(Instance* inst);
void DiagSummary(const Instance* inst);

void Destroy(void* instance) {
    auto* inst = static_cast<Instance*>(instance);
    if (inst != nullptr && inst->diag.on)
        DiagSummary(inst);
    if (inst != nullptr)
        ClearTopHide(inst); // the native top HUD draws again at once (the ttl would lapse anyway)
    if (inst != nullptr && inst->mailbox)
        ReleaseInputs(inst->ext); // the L lock's hold, L3 / R3 / B and the stick let go at once
    if (inst != nullptr && inst->hud_applied && inst->host.read_memory != nullptr && inst->host.write_memory != nullptr) {
        uint8_t v = 0;
        const uint64_t a = inst->host.main_base + kScreenDisplay;
        if (inst->host.read_memory(inst->host.userdata, a, &v, 1)) {
            v = static_cast<uint8_t>((v & 0x7F) | (inst->hud_orig & 0x80));
            inst->host.write_memory(inst->host.userdata, a, &v, 1);
        }
    }
    delete inst;
}

void StickMirror(void* instance, const EdenDsmodHostApi* host);
struct Instance;
void PublishTrail(Instance* inst, const EdenDsmodHostApi* host);
// dev.44: the pause / map state. The game's layout controller dLytControlGame_c (pointer
// main+0x183AC40) keeps its current state ID at +0x120: a pointer to one of its static state objects,
// StateID_Normal (main+0x164F988) in play, Pause_SetRes / Pause_SyncRes / Pause / Pause_DelRes for the
// pause menu and its Collection page, Map_SetRes .. Map_DelRes for the map (the objects' own names;
// the game's isStateNormal check main+0xb253fc reads the same two words; live hero 2026-11-14: play
// Normal, pause and its second page Pause, map Map, Normal again after each closes). dev.40..43 read
// main+0x192D0E4 instead: that is a slot of the game thread dMain's own stack (the thread is created
// at main+0x103DDC8 on the 0xF0000-byte stack main+0x183E000..main+0x192E000, 0xF1C below its top),
// holding whatever the deepest call last left there: 0x10 for the pause menu, but also blips in play
// and still 0x10 after the pause menu closed (live), and seconds-long runs of it on the Thor.
constexpr uint64_t kLytControlGame = 0x183AC40; // dLytControlGame_c*
constexpr uint64_t kCtrlState = 0x120;          // its current state ID (sFStateID_c*)
constexpr uint64_t kStateNormal = 0x164F988;    // dLytControlGame_c::StateID_Normal

// B-item shortcuts. Live save slot main+0x5AEAD54 (verified live):
//   +0x53AE u8 equipped B-item (0 bombs 1 bow 2 clawshots 3 beetle 4 slingshot 5 gust 6 whip 8 net)
//   +0xA60  u32 ammo, 7-bit fields: seeds bits 7..13, arrows 16..22, bombs 23..29 -- the base
//           container only. The HUD count adds what the ammo containers in the Adventure Pouch
//           hold (slot+0x7C0: u16 id, u16 count x8): Seed Satchels 128-130 to the seeds, Quivers
//           131-133 to the arrows, Bomb Bags 134-136 to the bombs. g0 file 1: base 15 + Small
//           Seed Satchel 9 = HUD 24; hero file 1 has its satchel in the Item Check, not the pouch,
//           so base 20 = HUD 20 (fixtures + live, 2026-11-11). Item Check stock does not count.
//   +0x9E5.. item flags found by bit pokes against the native item wheel (2026-11-07):
//     bow 9E5:4, iron bow 9EC:0, sacred bow 9EC:1, bombs 9EC:2, clawshots 9E5:5, gust 9E7:1,
//     whip 9F2:4, hook/quick/tough beetle 9EA:1/2/3, scattershot 9ED:7, big bug net 9F3:1.
//     base slingshot 9E7:4, base beetle 9E7:5, base bug net 9E9:0 (single-bit clears on g0's live
//     slot remove each from the native wheel, 2026-11-07 gaps pass).
//   Swords (2026-11-07 gaps pass, set/clear tests on the in-memory file image read back on the
//   native Quest Logs card): goddess 9E5:0, goddess longsword 9E5:1, master 9E5:2, goddess white
//   sword 9FA:4, true master 9F2:7; none set = practice sword. Highest stage wins.
//   (dev.33: superseded by the native item flags 9..14 through the item manager, ReadSwordState;
//   the Practice Sword is item flag 10 = +0x9F3 bit 0, reader analysis.)
//   (slot+0x9E0..0x9E3 are the tail of the story-flag block, re-copied from main+0x182E1C0 on pause.)
constexpr uint64_t kSlot = 0x5AEAD54;
void PublishCrystals(const EdenDsmodHostApi* host) {
    uint16_t packed = 0;
    uint32_t scene = 0;
    uint8_t stage = 0;
    // Gratitude Crystals occupy bits 3..9; the other bits in this word are unrelated.
    // Native Collection verified 2026-11-09: owner 17 -> 7 poke; hero's completed save = 80.
    const bool readable = host->read_memory != nullptr &&
        host->read_memory(host->userdata, host->main_base + kSlot + 0xA50, &packed, sizeof(packed)) &&
        host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, &stage, sizeof(stage)) &&
        host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, sizeof(scene));
    const int count = (packed >> 3) & 127;
    const bool valid = readable && stage != 0 && scene != 0x53656C74 && count <= 80;
    host->publish_i64(host->userdata, "sshd.crystals.valid", valid);
    host->publish_i64(host->userdata, "sshd.crystals", valid ? count : 0);
}

sshd::SwordState ReadSwordState(const EdenDsmodHostApi* host) {
    if (host->read_memory == nullptr)
        return {};
    const sshd::DpadRead rd = [host](uint64_t a, void* out, size_t n) {
        return host->read_memory(host->userdata, a, out, n) != EDEN_DSMOD_FALSE;
    };
    return sshd::ReadSword(rd, host->main_base);
}

void PublishSword(const EdenDsmodHostApi* host) {
    // current sword stage -> pause-menu art (only the current one is shown: spoiler rule)
    static const char* const kSword[6] = {"module:sshd:lyt/MenuPause/hm_swordA_00", "module:sshd:lyt/MenuPause/hm_swordB_00", "module:sshd:lyt/MenuPause/hm_swordC_00",
                                           "module:sshd:lyt/MenuPause/hm_swordD_00", "module:sshd:lyt/MenuPause/hm_swordE_00", "module:sshd:lyt/MenuPause/hm_swordMaster_00"};
    // dev.33 (reader analysis "Practice Sword", two boots): the native model
    // selector's item flags 14 / 13 / 9 / 12 / 11 / 10 over the active file -> stage 5..0, none = no
    // sword (replaces the old 0x9E4 & 0x3E heuristic, which the analysis rejected: +0x9E4 stayed 0x3E
    // with and without the Practice Sword). Stage 0 = the Practice Sword (hm_swordA_00).
    const sshd::SwordState sw = ReadSwordState(host);
    host->publish_i64(host->userdata, "sshd.sword.has", sw.Has() ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.sword.stage", sw.valid ? sw.stage : -1);
    host->publish_text(host->userdata, "sshd.sword.src", kSword[sw.Has() ? sw.stage : 0]);
}

void PublishShortcuts(const EdenDsmodHostApi* host) {
    uint8_t f[0x20]{};
    uint8_t eq = 0xFF;
    uint32_t ammo = 0;
    const uint64_t base = host->main_base + kSlot;
    if (host->read_memory == nullptr || !host->read_memory(host->userdata, base + 0x9E0, f, sizeof(f)) ||
        !host->read_memory(host->userdata, base + 0x53AE, &eq, 1) ||
        !host->read_memory(host->userdata, base + 0xA60, &ammo, 4)) {
        return;
    }
    auto bit = [&](int off, int b) { return (f[off - 0x9E0] >> b & 1) != 0; };
    struct Item {
        int id;
        bool owned;
        const char* icon;
        int count;
    };
    int seeds = ammo >> 7 & 127, arrows = ammo >> 16 & 127, bombs = ammo >> 23 & 127;
    uint16_t pouch[16]{};
    if (host->read_memory(host->userdata, base + 0x7C0, pouch, sizeof(pouch))) {
        for (int i = 0; i < 8; ++i) {
            const int id = pouch[2 * i], n = pouch[2 * i + 1] <= 99 ? pouch[2 * i + 1] : 0;
            if (id >= 128 && id <= 130) seeds += n;
            else if (id >= 131 && id <= 133) arrows += n;
            else if (id >= 134 && id <= 136) bombs += n;
        }
    }
    host->publish_i64(host->userdata, "sshd.ammo.seeds", seeds);
    const char* bow = bit(0x9EC, 1) ? "module:sshd:sq/DoButton/uc_bowC_00" : (bit(0x9EC, 0) ? "module:sshd:sq/DoButton/uc_bowB_00" : "module:sshd:sq/DoButton/uc_bowA_00");
    const char* beetle = bit(0x9EA, 3) ? "module:sshd:sq/DoButton/uc_beetleD_00"
                         : bit(0x9EA, 2) ? "module:sshd:sq/DoButton/uc_beetleC_00"
                         : bit(0x9EA, 1) ? "module:sshd:sq/DoButton/uc_beetleB_00" : "module:sshd:sq/DoButton/uc_beetleA_00";
    const Item wheel[8] = {  // native wheel, clockwise from the top
        {1, bit(0x9E5, 4) || bit(0x9EC, 0) || bit(0x9EC, 1) || eq == 1, bow, arrows},
        {0, bit(0x9EC, 2) || eq == 0, "module:sshd:sq/DoButton/uc_bomb_00", bombs},
        {3, bit(0x9E7, 5) || bit(0x9EA, 1) || bit(0x9EA, 2) || bit(0x9EA, 3) || eq == 3, beetle, -1},
        {8, bit(0x9E9, 0) || bit(0x9F3, 1) || eq == 8, bit(0x9F3, 1) ? "module:sshd:sq/DoButton/uc_netB_00" : "module:sshd:sq/DoButton/uc_netA_00", -1},
        {4, bit(0x9E7, 4) || bit(0x9ED, 7) || eq == 4, bit(0x9ED, 7) ? "module:sshd:sq/DoButton/uc_pachinkoB_00" : "module:sshd:sq/DoButton/uc_pachinkoA_00", seeds},
        {2, bit(0x9E5, 5) || eq == 2, "module:sshd:sq/DoButton/uc_hookShot_00", -1},
        {6, bit(0x9F2, 4) || eq == 6, "module:sshd:sq/DoButton/tr_whip_00", -1},
        {5, bit(0x9E7, 1) || eq == 5, "module:sshd:sq/DoButton/tr_vacuumIcon_00", -1},
    };
    static constexpr const char* kWheelKeys[][ 3 ] = {
        {"sshd.w0.on", "sshd.w0.eq", "sshd.w0.src"},
        {"sshd.w1.on", "sshd.w1.eq", "sshd.w1.src"},
        {"sshd.w2.on", "sshd.w2.eq", "sshd.w2.src"},
        {"sshd.w3.on", "sshd.w3.eq", "sshd.w3.src"},
        {"sshd.w4.on", "sshd.w4.eq", "sshd.w4.src"},
        {"sshd.w5.on", "sshd.w5.eq", "sshd.w5.src"},
        {"sshd.w6.on", "sshd.w6.eq", "sshd.w6.src"},
        {"sshd.w7.on", "sshd.w7.eq", "sshd.w7.src"},
    };
    static constexpr const char* kShortcutKeys[][ 5 ] = {
        {"sshd.sc0.src", "sshd.sc0.on", "sshd.sc0.nd0", "sshd.sc0.nd1", "sshd.sc0.hasn"},
        {"sshd.sc1.src", "sshd.sc1.on", "sshd.sc1.nd0", "sshd.sc1.nd1", "sshd.sc1.hasn"},
        {"sshd.sc2.src", "sshd.sc2.on", "sshd.sc2.nd0", "sshd.sc2.nd1", "sshd.sc2.hasn"},
        {"sshd.sc3.src", "sshd.sc3.on", "sshd.sc3.nd0", "sshd.sc3.nd1", "sshd.sc3.hasn"},
    };
    // full wheel for the Gear page (owned items with their tier icon; equipped one glows)
    bool cur = false;
    for (int i = 0; i < 8; ++i) {
        const auto& keys = kWheelKeys[i];
        host->publish_i64(host->userdata, keys[0], wheel[i].owned ? 1 : 0);
        host->publish_i64(host->userdata, keys[1], wheel[i].owned && wheel[i].id == eq ? 1 : 0);
        if (wheel[i].owned)
            host->publish_text(host->userdata, keys[2],
                               wheel[i].icon);
        if (wheel[i].owned && wheel[i].id == eq) {
            // Gear hub: the current ZR item, as the native HUD's ZR "Items" icon shows it
            host->publish_text(host->userdata, "sshd.bcur.src", wheel[i].icon);
            cur = true;
        }
    }
    host->publish_i64(host->userdata, "sshd.bcur.on", cur ? 1 : 0);
    int start = -1;
    for (int i = 0; i < 8; ++i)
        if (wheel[i].id == eq)
            start = i;
    int k = 0;
    if (start >= 0) {
        for (int step = 0; step < 8 && k < 4; ++step) {
            const Item& it = wheel[(start + step) % 8];
            if (!it.owned)
                continue;
            const auto& keys = kShortcutKeys[k];
            host->publish_text(host->userdata, keys[0],
                               it.icon);
            host->publish_i64(host->userdata, keys[1], 1);
            host->publish_i64(host->userdata, keys[2], it.count / 10 % 10);
            host->publish_i64(host->userdata, keys[3], it.count % 10);
            host->publish_i64(host->userdata, keys[4], it.count >= 0 ? 1 : 0);
            ++k;
        }
    }
    for (; k < 4; ++k)
        host->publish_i64(host->userdata, kShortcutKeys[k][1], 0);
}

// Player beacons (native map, up to 5 per area): slot+base + 12*i = f32 x, y, z; bitmask byte at
// slot+mask (live 2026-11-07: placing a beacon on the native map in Sealed Grounds wrote slot+0x70..
// and set slot+0x537D bit 0; in Faron Woods slot+0xAC.. and slot+0x537E; Lanayru Desert k=7, slot+0x214 /
// slot+0x5384; Eldin Volcano k=4, slot+0x160 / slot+0x5381, native placement 2026-11-08).
// Native placements 2026-11-08: Gorge k=8, Caves k=10, Sand Sea k=16; Skyloft uses the preceding
// entry (base 0x34, mask 0x537C, i.e. k=-1); native F020 sky placement confirms the same entry.
// Areas: k -> base 0x70+k*0x3C, mask slot+0x537D+k.
void PublishBeacons(const EdenDsmodHostApi* host) {
    static constexpr const char* kBeaconKeys[][ 3 ] = {
        {"sshd.bc0.on", "sshd.bc0.u", "sshd.bc0.v"},
        {"sshd.bc1.on", "sshd.bc1.u", "sshd.bc1.v"},
        {"sshd.bc2.on", "sshd.bc2.u", "sshd.bc2.v"},
        {"sshd.bc3.on", "sshd.bc3.u", "sshd.bc3.v"},
        {"sshd.bc4.on", "sshd.bc4.u", "sshd.bc4.v"},
    };
    struct AreaBeacons {
        const char* stage;
        uint32_t base, mask;
    };
    static constexpr AreaBeacons kB[] = {{"F401", 0x70, 0x537D}, {"F402", 0x70, 0x537D}, {"F100", 0xAC, 0x537E},
                                       {"F200", 0x160, 0x5381}, {"F300", 0x214, 0x5384}, {"F302", 0x250, 0x5385},
                                       {"F303", 0x2C8, 0x5387}, {"F301", 0x430, 0x538D}, {"F000", 0x34, 0x537C},
                                       {"F020", 0x34, 0x537C}};
    char st[8]{};
    const uint64_t slot = host->main_base + kSlot;
    int n = 0;
    if (host->publish_text != nullptr)
        host->publish_text(host->userdata, "sshd.bc.src", "module:sshd:beacon/84");
    if (host->read_memory(host->userdata, slot + 0x531C, st, sizeof(st))) {
        for (const auto& a : kB) {
            if (std::strncmp(st, a.stage, 8) != 0)
                continue;
            uint8_t m = 0;
            float xyz[15]{};
            if (!host->read_memory(host->userdata, slot + a.mask, &m, 1) ||
                !host->read_memory(host->userdata, slot + a.base, xyz, sizeof(xyz)))
                break;
            for (int i = 0; i < 5; ++i) {
                const bool on = (m >> i & 1) != 0;
                const auto& keys = kBeaconKeys[i];
                host->publish_i64(host->userdata, keys[0], on ? 1 : 0);
                // These native Lanayru maps and the sky have +x right and +z down, matching Link and the art.
                const bool flipped = std::strncmp(st, "F301", 8) == 0 ||
                                     std::strncmp(st, "F302", 8) == 0 ||
                                     std::strncmp(st, "F303", 8) == 0 ||
                                     std::strncmp(st, "F020", 8) == 0;
                host->publish_f64(host->userdata, keys[1], flipped ? xyz[i * 3] : -xyz[i * 3]);
                host->publish_f64(host->userdata, keys[2], flipped ? -xyz[i * 3 + 2] : xyz[i * 3 + 2]);
                n += on ? 1 : 0;
            }
            host->publish_i64(host->userdata, "sshd.bc.n", n);
            return;
        }
    }
    for (int i = 0; i < 5; ++i)
        host->publish_i64(host->userdata, kBeaconKeys[i][0], 0);
    host->publish_i64(host->userdata, "sshd.bc.n", 0);
}

// Goddess Cubes struck in the current region (Ready rim NE, on foot). A cube sets its story flag
// (the GodCube actor's params1 & 0x3FF; the Goddess Chest's params2 low half checks the same id),
// stored in the live story-flag block main+0x182E1F8 (static StoryflagManager copy, same layout as
// the save's story flags at slot+0x8E4). id -> (byte, bit) from the decomp's story-flag table, Wii
// big-endian bytes swapped to the Switch's little-endian u16 words. Region = the stage the GodCube
// actor sits in (stage archives F1xx/B100_1, F2xx, F3xx; 9 each). Verified on save fixtures:
// 100 % saves 9/9/9, owner 2/4/3, fresh 0/0/0 (pouch analysis).
constexpr uint64_t kStoryFlags = 0x182E1F8;
struct CubeFlag {
    uint8_t byte, bit;
};
constexpr CubeFlag Cube(int id) {
    // ids 227..256 live in Wii bytes 0x1A..0x1E (+0x805A9AD8); see analysis README for the table
    constexpr uint8_t kByte[] = {0x1B, 0x1C, 0x1D, 0x1E, 0x1F}; // HD byte of Wii 0x1A,0x1D,0x1C,0x1F,0x1E
    if (id <= 229) return {kByte[0], static_cast<uint8_t>(id - 227 + 5)};
    if (id <= 237) return {kByte[1], static_cast<uint8_t>(id - 230)};
    if (id <= 245) return {kByte[2], static_cast<uint8_t>(id - 238)};
    if (id <= 253) return {kByte[3], static_cast<uint8_t>(id - 246)};
    return {kByte[4], static_cast<uint8_t>(id - 254)};
}
constexpr int kCubes[3][9] = {
    {227, 228, 231, 236, 241, 244, 249, 250, 254}, // Faron Woods, Deep Woods, Lake Floria, Skyview Spring
    {229, 234, 237, 240, 242, 245, 248, 251, 255}, // Eldin Volcano, Volcano Summit
    {230, 235, 238, 239, 243, 246, 247, 252, 256}, // Lanayru Desert, Sand Sea, Gorge
};
int CubeRegion(const char* st) {
    if (std::strncmp(st, "B100_1", 8) == 0 || (st[0] == 'F' && st[1] == '1'))
        return 0;
    if (st[0] == 'F' && st[1] == '2')
        return 1;
    if (st[0] == 'F' && st[1] == '3')
        return 2;
    return -1;
}
void PublishCubes(const EdenDsmodHostApi* host) {
    char st[8]{};
    uint8_t f[5]{};
    uint32_t scene = 0;
    const uint64_t slot = host->main_base + kSlot;
    const bool read = host->read_memory != nullptr &&
                      host->read_memory(host->userdata, slot + 0x531C, st, sizeof(st)) &&
                      host->read_memory(host->userdata, host->main_base + kStoryFlags + 0x1B, f, sizeof(f)) &&
                      host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, sizeof(scene));
    const int region = read && scene != 0x53656C74 ? CubeRegion(st) : -1;
    int n = 0;
    if (region >= 0)
        for (const int id : kCubes[region]) {
            const auto c = Cube(id);
            n += (f[c.byte - 0x1B] >> c.bit) & 1;
        }
    host->publish_i64(host->userdata, "sshd.cubes.valid", region >= 0 ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.cubes", region >= 0 ? n : 0);
}

// The Sky map's Goddess tags, as the native draws them (map-marker contract 2026-11-11): a tag
// for each Goddess Chest whose cube is struck and which is still closed; never cube positions.
// A chest's "opened" is its tbox flag ((rot.z >> 9) & 0x3F of the TBox actor) in the scene's
// tbox word at slot+0x2A64 + 4*sceneIndex (Sky 21; 100 % saves 22/22); the current scene's
// word also lives in main+0x182E118, OR'ed in so a chest just opened drops its tag before the
// commit. Positions = the TBox actors in F020 (stage archive, room r00, layer 0).
constexpr uint64_t kTboxCurrent = 0x182E118;
struct Chest {
    uint8_t flag;
    float x, z;
};
constexpr Chest kSkyChests[] = {
    {0, 103755, 46044},    {1, 28608, 83346},     {2, -137465, 32051},  {3, -26031, 69685},
    {4, -26025, 69508},    {7, 103064, -121218},  {8, 158288, -80073},  {9, -99267, -122057},
    {10, 159241, -80065},  {11, -139107, 31373},  {12, -161837, -83076}, {13, -159979, -82701},
    {17, 129678, -108180}, {18, 39923, -112124},  {19, 102955, -121181}, {20, -137615, 31584},
    {21, -140500, -109134}, {22, -114369, 49941}, {23, 102303, 46534},  {24, -169763, -49270},
    {25, 129739, -107928}, {28, -138658, -110936},
};
constexpr int kChestMarkers = static_cast<int>(sizeof(kSkyChests) / sizeof(kSkyChests[0]));
// Each Goddess Chest's TBox params2 low half is its cube's story flag, 227 + its chest flag for
// all 22 F020 chests (map marker analysis). The native Sky map tags a chest
// whose cube is struck and which is still closed (owner: story 229 / 230 / 235 / 252 tagged, all
// four live matches); an opened chest loses its tag. Skyloft's five chests are not tagged: the
// tag template is MapSky's only and their params were not read, so they stay hidden.
constexpr int kGoddessStoryBase = 227;
void PublishGoddessChests(const EdenDsmodHostApi* host) {
    static const auto keys = [] {
        std::array<std::array<std::string, 3>, kChestMarkers> k;
        for (int i = 0; i < kChestMarkers; ++i)
            for (int j = 0; j < 3; ++j)
                k[i][j] = "sshd.gc" + std::to_string(i) + (j == 0 ? ".on" : j == 1 ? ".u" : ".v");
        return k;
    }();
    char st[8]{};
    const uint64_t slot = host->main_base + kSlot;
    // dev.35: also Inside the Thunderhead (F023: the Sky's scene, STIF flag index 21, same chests)
    bool sky = host->read_memory != nullptr && host->read_memory(host->userdata, slot + 0x531C, st, sizeof(st)) &&
               (std::strncmp(st, "F020", 8) == 0 || std::strncmp(st, "F023", 8) == 0);
    uint32_t saved = 0, live = 0;
    uint8_t story[5]{};
    // fail closed: any unreadable word hides every tag
    if (sky && (!host->read_memory(host->userdata, slot + 0x2A64 + 4 * 21, &saved, 4) ||
                !host->read_memory(host->userdata, host->main_base + kTboxCurrent, &live, 4) ||
                !host->read_memory(host->userdata, host->main_base + kStoryFlags + 0x1B, story, sizeof(story))))
        sky = false;
    const uint32_t opened = saved | live;
    int n = 0;
    for (int i = 0; i < kChestMarkers; ++i) {
        const auto cube = Cube(kGoddessStoryBase + kSkyChests[i].flag);
        const bool struck = ((story[cube.byte - 0x1B] >> cube.bit) & 1) != 0;
        const bool on = sky && struck && ((opened >> kSkyChests[i].flag) & 1) == 0;
        host->publish_i64(host->userdata, keys[i][0].c_str(), on ? 1 : 0);
        if (!on)
            continue;
        // sky map: +x right, +z down (u = x, v = -z)
        host->publish_f64(host->userdata, keys[i][1].c_str(), kSkyChests[i].x);
        host->publish_f64(host->userdata, keys[i][2].c_str(), -kSkyChests[i].z);
        ++n;
    }
    host->publish_i64(host->userdata, "sshd.gc.n", n);
    if (host->publish_text != nullptr)
        host->publish_text(host->userdata, "sshd.gc.src", "module:sshd:goddesstag");
}

// Adventure Pouch rearrange / Item Check store-retrieve, run by the helper's main-loop hook on the
// game thread (native/helper/sshd_stick_helper.S): op 1 = the Item Check's pouch-to-pouch swap,
// op 2 = the game's swapStockAndPouchItems, op 3 = the ZR-wheel commit (equip a B-item), op 4 = the
// pouch-wheel commit (last-used pouch slot). The module only stages a request in free gameplay
// (a file in play, no event, no pause / map menu; the helper re-checks the event) into an
// unlocked pouch slot (native getPouchSlotCount: 4 with the Adventure Pouch + the expansion
// counter, item flags u16[5] bit 6 and u16[54] bits 10-12, capped at 8).
constexpr uint32_t kOffReqSeq = 0x40, kOffOp = 0x44, kOffA = 0x48, kOffB = 0x4C, kOffReqEpoch = 0x50,
                   kOffResSeq = 0x60, kOffResCode = 0x64, kOffLoopBeat = 0x68, kOffEpoch = 0xFFC;
constexpr uint64_t kEventManager = 0x182DED8; // EventManager*; state u32 at +0x248, 0 = no event
constexpr int kPouchTimeout = 120;            // samples (~2 s) before a request is given up

int PouchSlotCount(const EdenDsmodHostApi* host) {
    uint16_t pouch = 0, counter = 0;
    const uint64_t flags = host->main_base + kSlot + 0x9E4;
    if (host->read_memory == nullptr || !host->read_memory(host->userdata, flags + 2 * 5, &pouch, 2) ||
        !host->read_memory(host->userdata, flags + 2 * 54, &counter, 2))
        return 0;
    const int n = ((counter >> 10) & 7) + ((pouch >> 6 & 1) ? 4 : 0);
    return n > 8 ? 8 : n;
}

// B-item equip (helper op 3, the player's own ZR-wheel commit: the
// equip analysis). player+0x6404 = the item in Link's hand (0xB none, 0x11 the sword
// drawn, else a B-item id: live 0x4 slingshot, 0x8 bug net); player+0x6408 = the ZR item.
constexpr uint64_t kPlayer = 0x2BF9940;
constexpr uint16_t kHeldNone = 0xB; // must match SSHD_HELD_NONE in the helper
constexpr uint16_t kHeldSword = 0x11; // the sword drawn (SSHD_HELD_SWORD)
// B-wheel positions clockwise from the top (Gear ring order) -> item id (0 bombs .. 8 bug net)
constexpr int kWheelIds[8] = {1, 0, 3, 8, 4, 2, 6, 5};
// player+0x468 action ids the native ZR wheel refuses with the sword drawn (main+0xa6253c..0xa62558,
// sword swing analysis: two sword attack states, checked with held == 0x11)
constexpr uint32_t kActAttackA = 0x68, kActAttackB = 0x9A;

GateIn ReadGate(const EdenDsmodHostApi* host) {
    GateIn g;
    if (host->read_memory == nullptr)
        return g;
    const auto rd = [host](uint64_t a, void* out, size_t n) {
        return host->read_memory(host->userdata, a, out, n) != EDEN_DSMOD_FALSE;
    };
    const uint64_t b = host->main_base;
    g.read = rd(b + kSlot + 0x531C, g.stage, 8) && rd(b + 0x182A7F4, &g.scene, 4) && rd(b + kEventManager, &g.events, 8);
    g.stage[8] = 0;
    if (g.events != 0 && !rd(g.events + 0x248, &g.ev, 4))
        g.read = false;
    uint16_t h = 0, s = 0;
    uint32_t act = 0;
    uint8_t wheel = 0;
    if (rd(b + kPlayer, &g.player, 8) && g.player != 0) {
        if (rd(g.player + 0x6404, &h, 2) && rd(g.player + 0x6408, &s, 2)) {
            g.held = h;
            g.bsel = s;
        }
        if (rd(g.player + 0x468, &act, 4))
            g.act = act;
        if (rd(g.player + 0x63B5, &wheel, 1))
            g.wheel = wheel;
        int8_t cur = 0;
        if (rd(g.player + 0x63E2, &cur, 1))
            g.wcur = cur;
    } else {
        g.player = 0;
    }
    return g;
}

// dev.44 menu sample (MenuSample): kMenuPlay / kMenuOpen / kMenuNone (no layout controller: the title,
// file select, a load) / kMenuUnread (neither source answered)
enum MenuCode : int { kMenuUnread = -1, kMenuPlay = 0, kMenuOpen = 1, kMenuNone = 2 };
enum MenuSrc : int { kSrcNone = 0, kSrcHelper = 1, kSrcDirect = 2 };
const char* MenuSrcName(int src) {
    return src == kSrcHelper ? "game thread" : src == kSrcDirect ? "direct" : "none";
}
// the controller's state objects by their distance from StateID_Normal: Pause_SetRes is 0xB8 after
// it, the other eight follow every 0x48 bytes (main+0x164FA40 .. main+0x164FC38)
const char* MenuStateName(int code, uint32_t delta) {
    static const char* const kNames[] = {"Pause_SetRes", "Pause_SyncRes", "Pause",       "Pause_DelRes",
                                         "Map_SetRes",   "Map_SyncRes",   "Map",         "Map_DelRes"};
    if (code == kMenuUnread)
        return "unread";
    if (code == kMenuNone)
        return "no controller";
    if (delta == 0)
        return "Normal";
    if (delta >= 0xB8 && (delta - 0xB8) % 0x48 == 0 && (delta - 0xB8) / 0x48 < 8)
        return kNames[(delta - 0xB8) / 0x48];
    return "other";
}

// SSHD_DIAG: one line whenever a gate word changes (menu state, event, hands, action, the B-wheel bits)
// with how many samples the previous combination lasted; the field evidence for the gate rules.
void GateDiag(Instance* inst) {
    if (!inst->diag.on || inst->host.log == nullptr)
        return;
    const GateIn& g = inst->gate;
    const GateIn& o = inst->diag_gate;
    ++inst->diag_gate_n;
    if (inst->diag_gate_n > 1 && inst->menu_code == inst->diag_menu_code && g.ev == o.ev && g.held == o.held &&
        g.bsel == o.bsel && g.act == o.act && g.wheel == o.wheel && g.wcur == o.wcur && std::strcmp(g.stage, o.stage) == 0 &&
        g.scene == o.scene)
        return;
    char msg[240];
    std::snprintf(msg, sizeof(msg),
                  "sshd gate: menu %s (%s) ev %u held 0x%x bsel %d act 0x%llx wheel 0x%x cur %d stage %.8s%s | menu_open %d "
                  "(prev %llu samples)",
                  MenuStateName(inst->menu_code, inst->menu_delta), MenuSrcName(inst->menu_src), g.ev, g.held, g.bsel,
                  static_cast<long long>(g.act), g.wheel, g.wcur, g.stage, g.scene == 0x53656C74 ? " title" : "",
                  inst->menu_open ? 1 : 0, static_cast<unsigned long long>(inst->diag_gate_n - 1));
    inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
    inst->diag_gate = g;
    inst->diag_menu_code = inst->menu_code;
    inst->diag_gate_n = 1;
}

// --- dev.40 gates -------------------------------------------------------------------------------
// dev.40 debounce, kept for dev.44's menu sample: a pause / map state counts as a menu once
// kMenuOpenRun samples in a row saw it, and as closed again after kMenuCloseRun samples of play.
// (dev.40 added it for the stack slot main+0x192D0E4, whose one-sample blips in play refused whatever
// was tapped in that 1/60 s; the controller state has no such blips, so it only costs 3 samples.)
constexpr int kMenuOpenRun = 3;
constexpr int kMenuCloseRun = 2;
// dev.44: the loop hook's menu sample (mailbox +0x6C, the state's distance from Normal +0x74, stamped
// +0x70 with the loop heartbeat of its frame) is used while the helper's loop runs (no retired helper,
// its heartbeat moved within kMenuStale samples) and the stamp is this frame's or the one before.
// Otherwise the module reads the same two words itself: the aligned 8-byte controller pointer, then
// the 8-byte state pointer in it (both change only on a state change, so an off-thread read sees one
// state or the next, never a torn or stale-stack value). A sample that neither source answers holds the
// debounced state for kMenuUnreadGrace samples (~0.5 s: one failed read does not drop a real menu),
// then the menu counts as closed and the gates refuse as "unreadable" instead (FreeWhy kWhyUnread,
// LBlocked 7): fail closed like every other unreadable gate word, but never reported as a menu.
constexpr uint32_t kOffMenuCode = 0x6C, kOffMenuStamp = 0x70, kOffMenuDelta = 0x74;
constexpr int kMenuStale = 8;
constexpr int kMenuUnreadGrace = 30;

struct MenuRead {
    int code = kMenuUnread;
    int src = kSrcNone;
    uint32_t delta = 0;
};

// The helper's sample is fresh while its loop runs and the stamp lies within one frame of the loop
// heartbeat (the module reads the heartbeat first: the helper may run between the two loads, or be
// between its heartbeat store and its stamp), with a code the helper writes.
bool MenuFresh(bool loop_alive, uint32_t beat, uint32_t stamp, uint32_t code) {
    const int32_t lag = static_cast<int32_t>(beat - stamp);
    return loop_alive && stamp != 0 && lag >= -1 && lag <= 1 && code <= static_cast<uint32_t>(kMenuNone);
}

MenuRead MenuSample(const Instance* inst, const EdenDsmodHostApi* host) {
    MenuRead m;
    const auto& e = inst->ext;
    if (inst->mailbox && e.load_u32 != nullptr && !inst->diag.menu_direct) {
        uint32_t beat = 0, stamp = 0, code = 0, delta = 0;
        if (e.load_u32(e.userdata, kOffLoopBeat, &beat) && e.load_u32(e.userdata, kOffMenuStamp, &stamp) &&
            e.load_u32(e.userdata, kOffMenuCode, &code) && e.load_u32(e.userdata, kOffMenuDelta, &delta) &&
            MenuFresh(!inst->pouch_dead && inst->loop_still < kMenuStale, beat, stamp, code)) {
            m.code = static_cast<int>(code);
            m.src = kSrcHelper;
            m.delta = code == kMenuOpen ? delta : 0;
            return m;
        }
    }
    uint64_t ctrl = 0, state = 0;
    if (host->read_memory == nullptr || !host->read_memory(host->userdata, host->main_base + kLytControlGame, &ctrl, 8))
        return m;
    if (ctrl != 0 && !host->read_memory(host->userdata, ctrl + kCtrlState, &state, 8))
        return m;
    m.src = kSrcDirect;
    if (ctrl == 0 || state == 0) {
        m.code = kMenuNone;
        return m;
    }
    m.delta = static_cast<uint32_t>(state - (host->main_base + kStateNormal));
    m.code = m.delta == 0 ? kMenuPlay : kMenuOpen;
    return m;
}

// Once per (throttled) sample: the debounced menu state from this sample's MenuRead.
void MenuGate(Instance* inst, const MenuRead& m) {
    const GateIn& g = inst->gate;
    inst->menu_code = m.code;
    inst->menu_src = m.src;
    inst->menu_delta = m.delta;
    const bool was = inst->menu_open, was_unread = inst->menu_unread;
    if (m.code == kMenuUnread) {
        // the grace holds the debounced state; after it: not a menu, but unreadable
        if (++inst->menu_unread_n > kMenuUnreadGrace) {
            inst->menu_unread = true;
            inst->menu_open = false;
            inst->menu_run = 0;
        }
    } else {
        inst->menu_unread_n = 0;
        inst->menu_unread = false;
        if (m.code == kMenuOpen) {
            ++inst->menu_run;
            inst->menu_quiet = 0;
        } else {
            ++inst->menu_quiet;
            inst->menu_run = 0;
        }
        const bool raw = inst->diag.menu_raw;
        if (inst->menu_run >= (raw ? 1 : kMenuOpenRun))
            inst->menu_open = true;
        else if (inst->menu_quiet >= (raw ? 1 : kMenuCloseRun))
            inst->menu_open = false;
    }
    ++inst->menu_span;
    // dev.43 (Thor RC: "Not now" while the top screen showed play): one line each time the
    // debounced menu state changes, with the state behind it and how long the previous one lasted;
    // dev.44: also where the sample came from, and the unreadable state
    if (inst->menu_open != was || inst->menu_unread != was_unread) {
        if (inst->host.log != nullptr) {
            char msg[176];
            std::snprintf(msg, sizeof(msg), "sshd: menu gate %s: state %s (%s) stage %.8s (the gate was %s %d samples)",
                          inst->menu_open ? "open" : inst->menu_unread ? "unreadable" : "closed",
                          MenuStateName(m.code, m.delta), MenuSrcName(m.src), g.stage,
                          was ? "open" : was_unread ? "unreadable" : "closed", inst->menu_span);
            inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
        inst->menu_span = 0;
    }
}

// Free gameplay: a file in play (not the title / Quest Logs, not a load), no pause or map menu, no
// event (the helper re-checks the event on the game thread).
int FreeWhy(const Instance* inst, const GateIn& g) {
    if (!g.read)
        return kWhyUnread;
    if (g.scene == 0x53656C74)
        return kWhyTitle;
    if (g.stage[0] == 0)
        return kWhyLoad;
    if (inst->menu_open)
        return kWhyMenu;
    if (inst->menu_unread)
        return kWhyUnread; // dev.44: no menu state for longer than the grace (MenuGate)
    if (g.events == 0 || g.ev != 0)
        return kWhyEvent;
    return kWhyOk;
}

bool OnLoftwing(const GateIn& g) {
    return std::strncmp(g.stage, "F020", 8) == 0;
}

// Silent Realms: stages S000 (Skyloft), S100 (Faron), S200 (Eldin), S300 (Lanayru)
bool SilentRealm(const char* st) {
    return st[0] == 'S' && st[1] >= '0' && st[1] <= '3' && st[2] == '0' && st[3] == '0' && st[4] == 0;
}

int PouchHelperWhy(const Instance* inst) {
    return inst->mailbox && !inst->pouch_dead && inst->loop_still < 30 ? kWhyOk : kWhyHelper;
}

// A ZR equip now (helper op 3). The native wheel commit (main+0xa64230..0xa64990) does not look at
// what Link holds: the wheel opens with the sword drawn (dev.40 probe) and commits on it; it refuses
// only during the two sword attack states. With a B-item in hand a ZR press puts it away first
// (probe: bug net 0x8 -> 0xB on the press, then the wheel opens), so an equip then puts the item
// away the same way (PendPoll) and commits once Link's hands are empty. While the native B-wheel
// itself is open the drop waits for it to close.
int BItemWhy(const Instance* inst, const GateIn& g) {
    if (const int w = PouchHelperWhy(inst); w != kWhyOk)
        return w;
    if (const int w = FreeWhy(inst, g); w != kWhyOk)
        return w;
    if (OnLoftwing(g))
        return kWhyLoftwing;
    if (g.player == 0 || g.held < 0)
        return kWhyNoPlayer;
    // dev.41: an unreadable wheel byte is not "wheel closed", nor an unreadable action "no attack"
    // with the sword drawn (ReadGate leaves them -1): fail closed
    if (g.wheel < 0 || (g.held == kHeldSword && g.act < 0))
        return kWhyUnread;
    if (g.held == kHeldSword && (g.act == kActAttackA || g.act == kActAttackB))
        return kWhySwing;
    if (g.wheel > 0)
        return kWhyBusy; // a native item / pouch wheel is open: its own commit decides first
    if (g.held != kHeldNone && g.held != kHeldSword)
        return kWhyItem;
    return kWhyOk;
}

// An R equip / pouch move now (helper ops 1, 2, 4): a plain store or the Item Check's own routine,
// nothing about Link's hands; the native pouch wheel's own commit goes first (wheel bit 2).
int PouchWhy(const Instance* inst, const GateIn& g) {
    if (const int w = PouchHelperWhy(inst); w != kWhyOk)
        return w;
    if (const int w = FreeWhy(inst, g); w != kWhyOk)
        return w;
    if (g.player != 0 && g.wheel < 0)
        return kWhyUnread; // dev.41: Link is there but his wheel byte is unreadable (fail closed)
    if (g.wheel > 0 && (g.wheel & 4) != 0)
        return kWhyBusy;
    return kWhyOk;
}

// Reasons a queued op may wait out (a drop must not be lost to them); the rest refuse at once.
bool Transient(int why) {
    return why == kWhyEvent || why == kWhyBusy || why == kWhySwing || why == kWhyItem;
}
constexpr int kPendEventWait = 30;  // samples (~0.5 s) an event may last before a drop is refused
constexpr int kPendWait = 120;      // samples (~2 s) for an op in flight, a swing, a wheel, a put-away
constexpr uint32_t kPutAwayFrames = 4; // mailbox kOffPutAway: frames the stick hook adds B (put away)

// Samples a refusal's "Not now" / "Put away" stays up (SSHD_DIAG_HOLD_TICKS keeps it longer for stills).
int Flash(const Instance* inst) {
    return std::max(90, inst->diag.hold_ticks);
}

// One line: what happened, its reason and the raw gate words (the Thor log's evidence).
void GateLine(const Instance* inst, const char* what, const char* act, int why, const char* note) {
    if (inst->host.log == nullptr)
        return;
    const GateIn& g = inst->gate;
    char msg[448];
    std::snprintf(msg, sizeof(msg),
                  "sshd: %s %s (%s%s): held 0x%x bsel %d act 0x%llx wheel %d menu %s %s (run %d, open %d, unread %d) ev %u "
                  "stage %.8s%s pouch_wait %d pend %d loop_still %d seq_wait %d seq_pend %d drag %d",
                  what, act, WhyName(why), note, g.held, g.bsel, static_cast<long long>(g.act), g.wheel,
                  MenuStateName(inst->menu_code, inst->menu_delta), MenuSrcName(inst->menu_src), inst->menu_run,
                  inst->menu_open ? 1 : 0, inst->menu_unread_n, g.ev, g.stage, g.scene == 0x53656C74 ? " title" : "",
                  inst->pouch_wait, inst->pend_op.on ? 1 : 0, inst->loop_still, inst->seq_wait, inst->pend_n,
                  inst->dragging ? 1 : 0);
    inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
}

// One line per refusal (a "Not now" / "Put away" / refused haptic the player saw).
void Refuse(Instance* inst, const char* act, int why, const char* note = "") {
    inst->refuse_why = why;
    ++inst->refusals;
    GateLine(inst, "refused", act, why, note);
}

void PendPoll(Instance* inst);

void PouchPoll(Instance* inst, const EdenDsmodHostApi* host) {
    const auto& e = inst->ext;
    uint32_t beat = 0;
    if (inst->mailbox && e.load_u32(e.userdata, kOffLoopBeat, &beat)) {
        inst->loop_still = beat == inst->loop_beat ? inst->loop_still + 1 : 0;
        inst->loop_beat = beat;
    }
    if (inst->pouch_wait > 0) {
        uint32_t seq = 0, code = 0;
        if (e.load_u32(e.userdata, kOffResSeq, &seq) && seq == inst->pouch_seq) {
            e.load_u32(e.userdata, kOffResCode, &code);
            inst->pouch_code = static_cast<int>(code);
            inst->pouch_wait = 0;
            if (code != 0) {
                inst->pouch_flash = Flash(inst);
                char note[48];
                std::snprintf(note, sizeof(note), ": request %u, helper code %u", seq, code);
                Refuse(inst, "pouch op", kWhyGame, note);
            }
        } else if (++inst->pouch_wait > kPouchTimeout) {
            // never retried: the op may still land, and a stuck helper must not be fed more
            inst->pouch_wait = 0;
            inst->pouch_dead = true;
            if (host->log != nullptr)
                host->log(host->userdata, EDEN_DSMOD_LOG_WARNING, "sshd: pouch request timed out; pouch ops off");
        }
    }
    if (inst->pouch_flash > 0)
        --inst->pouch_flash;
    inst->pouch_slots = PouchSlotCount(host);
    PendPoll(inst);
    const GateIn& g = inst->gate;
    const int helper = PouchHelperWhy(inst);
    inst->why_pouch = PouchWhy(inst, g);
    inst->why_bitem = BItemWhy(inst, g);
    // sshd.pouch.ok gates the Bag moves in the manifest (enabled_bind): free play and a live helper;
    // an op in flight, an event or the open pouch wheel only make a drop wait (PendPoll)
    const int free = helper != kWhyOk ? helper : FreeWhy(inst, g);
    inst->pouch_ok = free == kWhyOk || Transient(free);
    inst->bitem_ok = inst->why_bitem == kWhyOk || Transient(inst->why_bitem);
    host->publish_i64(host->userdata, "sshd.ply.held", g.held);
    host->publish_i64(host->userdata, "sshd.ply.bsel", g.bsel);
    host->publish_i64(host->userdata, "sshd.ply.act", g.act); // diagnostics only (test runs), unbound
    host->publish_i64(host->userdata, "sshd.bitem.ok", inst->bitem_ok ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.pouch.helper", helper == kWhyOk ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.pouch.ok", inst->pouch_ok ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.pouch.slots", inst->pouch_slots);
    host->publish_i64(host->userdata, "sshd.pouch.refused", inst->pouch_flash > 0 ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.pouch.queued", inst->pend_op.on ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.why.pouch", inst->why_pouch);
    host->publish_i64(host->userdata, "sshd.why.bitem", inst->why_bitem);
    host->publish_i64(host->userdata, "sshd.menu.open", inst->menu_open ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.menu.state", inst->menu_code); // diagnostics only (MenuCode), unbound
    host->publish_i64(host->userdata, "sshd.menu.src", inst->menu_src);    // diagnostics only (MenuSrc), unbound
    host->publish_i64(host->userdata, "sshd.refuse.why", inst->refuse_why);
    host->publish_i64(host->userdata, "sshd.refuse.n", inst->refusals);
}

// Writes the request for the helper's loop hook (callers have checked the gate).
bool PouchStage(Instance* inst, uint32_t op, uint32_t a, uint32_t b) {
    const auto& e = inst->ext;
    const EdenDsmodHostApi* host = &inst->host;
    uint32_t epoch = 0;
    if (!e.load_u32(e.userdata, kOffEpoch, &epoch) || epoch == 0)
        return false;
    const uint32_t seq = inst->pouch_seq + 1;
    e.store_u32(e.userdata, kOffOp, op);
    e.store_u32(e.userdata, kOffA, a);
    e.store_u32(e.userdata, kOffB, b);
    e.store_u32(e.userdata, kOffReqEpoch, epoch);
    e.store_u32(e.userdata, kOffReqSeq, seq); // last: publishes the request
    inst->pouch_seq = seq;
    inst->pouch_wait = 1;
    if (host->log != nullptr) {
        char msg[80];
        std::snprintf(msg, sizeof(msg), "sshd: pouch request %u op %u (%u, %u)", seq, op, a, b);
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
    }
    return true;
}

int OpWhy(const Instance* inst, uint32_t op, const GateIn& g) {
    return op == 3 ? BItemWhy(inst, g) : PouchWhy(inst, g);
}

// A drop: staged now when its gate is open and no op is in flight; queued (the one place) while it
// only has to wait (Transient, or an op in flight); refused at once otherwise. A newer equip
// replaces a queued equip of the same kind (the player's last choice wins); anything else finding
// the place taken is refused as busy.
bool PouchSubmit(Instance* inst, const char* act, uint32_t op, uint32_t a, uint32_t b) {
    inst->gate = ReadGate(&inst->host); // the words as of the drop (the menu stays debounced)
    const int why = OpWhy(inst, op, inst->gate);
    if (why != kWhyOk && !Transient(why)) {
        inst->pouch_flash = Flash(inst);
        Refuse(inst, act, why);
        return false;
    }
    if (why == kWhyOk && inst->pouch_wait == 0 && !inst->pend_op.on) {
        if (PouchStage(inst, op, a, b))
            return true;
        inst->pouch_flash = Flash(inst);
        Refuse(inst, act, kWhyHelper, ": no epoch");
        return false;
    }
    auto& q = inst->pend_op;
    const bool equip = op == 3 || op == 4;
    if (q.on && !(equip && q.op == op)) {
        inst->pouch_flash = Flash(inst);
        Refuse(inst, act, kWhyBusy, ": the queue place is taken");
        return false;
    }
    if (!q.on)
        q.put_away = false;
    q.on = true;
    q.op = op, q.a = a, q.b = b;
    q.wait = 0;
    q.why = why == kWhyOk ? kWhyBusy : why;
    std::snprintf(q.act, sizeof(q.act), "%s", act);
    if (inst->host.log != nullptr) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "sshd: %s queued (%s) op %u (%u, %u)", act, WhyName(q.why), op, a, b);
        inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
    }
    PendPoll(inst); // an item in hand: the put-away press starts now
    return true;
}

// Once per sample (and right after a drop): the queued op is staged as soon as its gate opens; a ZR
// equip with a B-item in Link's hand first has the helper press B for kPutAwayFrames frames (the
// game's own put-away, what a ZR press does before the native wheel opens); a reason that turns
// hard, or one that outlasts its wait, refuses it with that reason.
void PendPoll(Instance* inst) {
    auto& q = inst->pend_op;
    if (!q.on)
        return;
    const GateIn& g = inst->gate;
    const int why = OpWhy(inst, q.op, g);
    const auto drop = [&](int w, const char* note) {
        q.on = false;
        inst->pouch_flash = Flash(inst);
        Refuse(inst, q.act, w, note);
    };
    if (why == kWhyOk && inst->pouch_wait == 0) {
        q.on = false;
        if (!PouchStage(inst, q.op, q.a, q.b))
            drop(kWhyHelper, ": no epoch");
        return;
    }
    if (why != kWhyOk && !Transient(why)) {
        drop(why, ", while queued");
        return;
    }
    if (why == kWhyItem && !q.put_away && inst->mailbox) {
        inst->ext.store_u32(inst->ext.userdata, kOffPutAway, kPutAwayFrames);
        q.put_away = true;
        if (inst->host.log != nullptr) {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "sshd: %s: putting away item 0x%x first (B)", q.act, g.held);
            inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
    }
    q.why = why == kWhyOk ? kWhyBusy : why;
    const int limit = q.why == kWhyEvent ? kPendEventWait : kPendWait;
    if (++q.wait > limit)
        drop(q.why, ", waited too long");
}

// Equip drops on the Gear / Pouch hubs (tap a ring item then the hub, or drag it onto the hub):
// "bitem_equip" with a B-wheel position 0..7 makes that item the ZR item (helper op 3, BItemWhy);
// "pouch_equip" with a pouch slot makes it the R item, the slot the native pouch wheel opens on
// (op 4, PouchWhy). Item Check payloads (100+) are refused.
bool EquipAction(Instance* inst, std::string_view action, int64_t src) {
    if (action == "bitem_equip") {
        if (src < 0 || src > 7) {
            Refuse(inst, "bitem_equip", kWhyPayload);
            return false;
        }
        return PouchSubmit(inst, "bitem_equip", 3, static_cast<uint32_t>(kWheelIds[src]), 0);
    }
    if (action == "pouch_equip") {
        if (src < 0 || src >= inst->pouch_slots) {
            Refuse(inst, "pouch_equip", kWhyPayload);
            return false;
        }
        return PouchSubmit(inst, "pouch_equip", 4, static_cast<uint32_t>(src), 0);
    }
    return false;
}

// Drop actions: "pouch_to_<slot>" / "stock_to_<slot>", argument = the carried payload
// (pouch slot 0..7, or 100 + Item Check slot 0..59).
bool PouchAction(Instance* inst, std::string_view action, int64_t src) {
    if (action == "bitem_equip" || action == "pouch_equip")
        return EquipAction(inst, action, src);
    int to = -1;
    bool to_pouch = false;
    for (const auto& [prefix, pouch] : {std::pair{std::string_view("pouch_to_"), true},
                                        std::pair{std::string_view("stock_to_"), false}}) {
        if (action.substr(0, prefix.size()) != prefix)
            continue;
        to_pouch = pouch;
        to = 0;
        for (const char c : action.substr(prefix.size())) {
            if (c < '0' || c > '9' || to > 100)
                return false;
            to = to * 10 + (c - '0');
        }
    }
    if (to < 0 || (to_pouch ? to >= inst->pouch_slots : to >= 60))
        return false;
    const bool from_pouch = src >= 0 && src < inst->pouch_slots;
    const bool from_stock = src >= 100 && src < 160;
    const char* act = to_pouch ? "pouch_to" : "stock_to";
    if (to_pouch && from_pouch)
        return src == to || PouchSubmit(inst, act, 1, static_cast<uint32_t>(src), static_cast<uint32_t>(to));
    if (to_pouch && from_stock)
        return PouchSubmit(inst, act, 2, static_cast<uint32_t>(to), static_cast<uint32_t>(src - 100));
    if (!to_pouch && from_pouch)
        return PouchSubmit(inst, act, 2, static_cast<uint32_t>(src), static_cast<uint32_t>(to));
    return false; // Item Check slot to Item Check slot: not offered
}

// Spin attacks (Ready right column, owner 2026-11-11). The game's own button-only input: a quick
// left-right-left right-stick flick is a horizontal spin, up-down-up a vertical spin. The module
// stages the vectors as a table and the helper's loop hook plays them, one step per game frame,
// into the same stick words the drag mirror uses (native/helper/sshd_stick_helper.S "Stick
// sequence"): the game reads them through its own stick sampler, so it decides itself whether the
// flicks make a spin (stamina, recovery, the sword's state). Timing is in game frames (the loop
// hook runs once per frame), never wall time. The physical stick stops a sequence at once.
constexpr uint32_t kOffSeqReq = 0x80, kOffSeqSteps = 0x84, kOffSeqEpoch = 0x88, kOffSeqRes = 0xA0, kOffSeqCode = 0xA4, kOffSeqFrames = 0xA8, kOffSeqTable = 0xC0;
constexpr int kSeqTimeout = 240; // samples (~4 s): longer than any table below
constexpr int16_t kFull = 32767;
// live-tuned on the desktop emulator (g0 file, 2026-11-11): 16 / 16 spins at 312,500 stamina each; the
// game also took 1-6 frame flicks with 1-3 frame gaps, not 8-frame flicks or 6-frame gaps (sshd_spin_test)
constexpr StickStep kSpinH[] = {{-kFull, 0, 3}, {0, 0, 1}, {kFull, 0, 3}, {0, 0, 1}, {-kFull, 0, 3}, {0, 0, 2}};
constexpr StickStep kSpinV[] = {{0, kFull, 3}, {0, 0, 1}, {0, -kFull, 3}, {0, 0, 1}, {0, kFull, 3}, {0, 0, 2}};

// A sword in hand: the same rule as the Gear sword (PublishShortcuts), the native model selector's
// item flags (dev.33; unreadable = no sword, fail closed).
bool HasSword(const EdenDsmodHostApi* host) {
    return ReadSwordState(host).Has();
}

// Link's hands for a spin: empty or holding the sword (a flick draws it and it stays drawn). Any
// other item would be aimed by the flicks, so a spin is refused then ("Put away" under the button).
bool SpinHands(const GateIn& g) {
    return g.held == kHeldNone || g.held == kHeldSword;
}

// The game's L as its own pad sampler saw it this frame: the stick hook copies the sampled npad
// buttons of the live controller into mailbox+0x14 (bit 6 = L, which the game maps to its camera /
// look button 0x4000 at main+0xed6368..0xed6380). Virtual and physical presses read alike.
constexpr uint32_t kOffButtons = 0x14, kNpadL = 1u << 6;
bool GameL(const Instance* inst) {
    uint32_t b = 0;
    return inst->mailbox && inst->heartbeat_still < 30 &&
           inst->ext.load_u32(inst->ext.userdata, kOffButtons, &b) && (b & kNpadL) != 0;
}

void SpinStart(Instance* inst, int kind);

// dev.40: a spin needs a live helper, free play on foot, a sword and empty / sword hands (SpinWhy);
// one sequence plays at a time, so a tap while one plays (or a stroke / an L-lock wait owns the
// stick) queues one spin instead of "Not now" (Thor: four taps 130 ms apart during a spin were
// refused); further taps while one waits are the same spin. An event blip may also be waited out.
int SpinWhy(const Instance* inst, const EdenDsmodHostApi* host, const GateIn& g) {
    if (!inst->mailbox || inst->seq_dead || inst->loop_still >= 30 || inst->heartbeat_still >= 30)
        return kWhyHelper;
    if (const int w = FreeWhy(inst, g); w != kWhyOk)
        return w;
    if (OnLoftwing(g))
        return kWhyLoftwing;
    if (g.player == 0 || g.held < 0)
        return kWhyNoPlayer;
    if (!SpinHands(g))
        return kWhyItem;
    if (!HasSword(host))
        return kWhyNoSword;
    return kWhyOk;
}
// dev.42: the no-helper swipe fallback (the manifest's slash_* right-stick flicks,
// offered only while sshd.stick.ready is 0) takes the same refusals as the helper's strokes and
// spins: free play (not the title / a load / a menu / an event), on foot (not the Loftwing), not a
// Silent Realm, Link present with empty or sword hands, and a sword. The manifest gates the actions
// on sshd.swipe.fallback (enabled_bind: unpublished = refused).
int FallbackWhy(const Instance* inst, const EdenDsmodHostApi* host) {
    const GateIn& g = inst->gate;
    if (const int w = FreeWhy(inst, g); w != kWhyOk)
        return w;
    if (OnLoftwing(g))
        return kWhyLoftwing;
    if (SilentRealm(g.stage))
        return kWhyRealm;
    if (g.player == 0 || g.held < 0)
        return kWhyNoPlayer;
    if (!SpinHands(g))
        return kWhyItem;
    if (!HasSword(host))
        return kWhyNoSword;
    return kWhyOk;
}

bool SpinBusy(const Instance* inst) {
    return inst->seq_wait > 0 || inst->pend_n > 0 || inst->dragging || inst->flick_q;
}
constexpr int kSpinQueueWait = 90; // samples (~1.5 s) a queued spin may wait for the one in flight

void SpinRefuse(Instance* inst, const char* act, int why, const char* note = "") {
    inst->seq_flash = Flash(inst);
    inst->spin_why = why;
    Refuse(inst, act, why, note);
}

void SpinPoll(Instance* inst, const EdenDsmodHostApi* host) {
    const auto& e = inst->ext;
    if (inst->seq_wait > 0) {
        uint32_t res = 0, code = 0;
        if (e.load_u32(e.userdata, kOffSeqRes, &res) && res == inst->seq_id) {
            e.load_u32(e.userdata, kOffSeqCode, &code);
            inst->seq_code = static_cast<int>(code);
            inst->seq_wait = 0;
            // refused or stopped by the game state (the physical stick (1) is no refusal); a
            // late-stroke flick's refusal is not a spin's
            if (code >= 2 && res != inst->seq_quiet) {
                inst->seq_flash = Flash(inst);
                inst->spin_why = code == 5 ? kWhyEvent : code == 6 ? kWhyItem : kWhyGame;
                // dev.43: the game thread's refusal ("Not now" / "Put away") logs one line too
                char note[48];
                std::snprintf(note, sizeof(note), ": sequence %u, helper code %u", res, code);
                Refuse(inst, "spin", inst->spin_why, note);
            } else if (code >= 2) {
                // dev.45: a stroke's late flick the game thread refused is that swipe's refusal ("Not
                // now" over the pad), not a spin's; dev.38..44 logged only "ended (code)"
                inst->swipe_flash = std::max(kSwipeFlash, inst->diag.hold_ticks);
                char note[72];
                std::snprintf(note, sizeof(note), ": stroke %u, late flick, sequence %u, helper code %u",
                              inst->seq_quiet_stroke, res, code);
                Refuse(inst, "swipe", code == 5 ? kWhyEvent : code == 6 ? kWhyItem : kWhyGame, note);
            }
            if (host->log != nullptr) {
                uint32_t frames = 0;
                e.load_u32(e.userdata, kOffSeqFrames, &frames);
                char msg[80];
                std::snprintf(msg, sizeof(msg), "sshd: stick sequence %u ended (%u) after %u frames", res, code, frames);
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
            }
        } else if (++inst->seq_wait > kSeqTimeout) {
            inst->seq_wait = 0;
            inst->seq_dead = true;
            if (host->log != nullptr)
                host->log(host->userdata, EDEN_DSMOD_LOG_WARNING, "sshd: stick sequence timed out; spins off");
        }
    }
    if (inst->seq_flash > 0)
        --inst->seq_flash;
    const int why = SpinWhy(inst, host, inst->gate);
    if (inst->spin_q != 0) {
        const char* act = inst->spin_q == 1 ? "spin_h" : "spin_v";
        if (why != kWhyOk && why != kWhyEvent) {
            SpinRefuse(inst, act, why, ", while queued");
            inst->spin_q = 0;
        } else if (why == kWhyOk && !SpinBusy(inst)) {
            SpinStart(inst, inst->spin_q);
            inst->spin_q = 0;
        } else if (++inst->spin_q_wait > (why == kWhyEvent ? kPendEventWait : kSpinQueueWait)) {
            SpinRefuse(inst, act, why == kWhyOk ? kWhyBusy : why, ", waited too long");
            inst->spin_q = 0;
        }
    }
    inst->why_spin = why == kWhyOk && SpinBusy(inst) ? kWhyBusy : why;
    inst->spin_ok = inst->why_spin == kWhyOk;
    const bool refused = inst->seq_flash > 0;
    host->publish_i64(host->userdata, "sshd.spin.ok", inst->spin_ok ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.spin.busy", inst->seq_wait > 0 || inst->pend_n > 0 ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.spin.queued", inst->spin_q != 0 ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.spin.refused", refused ? 1 : 0);
    // the refusal's words under the buttons: "Put away" for an item in Link's hands, else "Not now"
    host->publish_i64(host->userdata, "sshd.spin.putaway", refused && inst->spin_why == kWhyItem ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.spin.notnow", refused && inst->spin_why != kWhyItem ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.spin.code", inst->seq_code);
    host->publish_i64(host->userdata, "sshd.why.spin", inst->why_spin);
}

// Writes a stick sequence table for the helper's loop hook and publishes the request (no gating).
bool SeqStage(Instance* inst, const StickStep* steps, size_t n, bool quiet) {
    const auto& e = inst->ext;
    const EdenDsmodHostApi* host = &inst->host;
    uint32_t epoch = 0;
    if (n < 1 || n > 8 || !e.load_u32(e.userdata, kOffEpoch, &epoch) || epoch == 0)
        return false;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t xy = static_cast<uint16_t>(steps[i].x) | static_cast<uint32_t>(static_cast<uint16_t>(steps[i].y)) << 16;
        e.store_u32(e.userdata, kOffSeqTable + 8 * static_cast<uint32_t>(i), xy);
        e.store_u32(e.userdata, kOffSeqTable + 8 * static_cast<uint32_t>(i) + 4, steps[i].frames);
    }
    const uint32_t id = inst->seq_id + 1;
    e.store_u32(e.userdata, kOffSeqSteps, static_cast<uint32_t>(n));
    e.store_u32(e.userdata, kOffSeqEpoch, epoch);
    e.store_u32(e.userdata, kOffSeqReq, id); // last: publishes the request
    inst->seq_id = id;
    inst->seq_quiet = quiet ? id : 0;
    inst->seq_wait = 1;
    inst->spin_ok = false;
    if (host->log != nullptr) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "sshd: stick sequence %u staged (%zu steps)", id, n);
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
    }
    return true;
}

// While the L toggle holds L the game routes the right stick to the camera, so a sequence first
// waits (LTick) until the game sees L released.
void SeqQueue(Instance* inst, const StickStep* steps, size_t n, bool quiet) {
    for (size_t i = 0; i < n && i < 8; ++i)
        inst->pend[i] = steps[i];
    inst->pend_n = static_cast<int>(n < 8 ? n : 8);
    inst->pend_wait = 0;
    inst->pend_quiet = quiet;
    inst->spin_ok = false;
}

// Plays a spin (1 horizontal, 2 vertical): with the L lock on it first waits for the game to see L
// up (SeqQueue / LTick).
void SpinStart(Instance* inst, int kind) {
    const StickStep* steps = kind == 1 ? kSpinH : kSpinV;
    const size_t n = kind == 1 ? std::size(kSpinH) : std::size(kSpinV);
    if (inst->l_on)
        SeqQueue(inst, steps, n, false);
    else
        SeqStage(inst, steps, n, false);
}

// "spin_h" / "spin_v" (the Ready spin buttons): refused ("Not now", or "Put away" with an item in
// Link's hands) outside free gameplay on foot with a sword; queued while another sequence, a stroke
// or an L-lock wait owns the stick, or an event blips (SpinPoll starts it).
bool SpinAction(Instance* inst, std::string_view action) {
    const int kind = action == "spin_h" ? 1 : action == "spin_v" ? 2 : 0;
    if (kind == 0)
        return false;
    const char* act = kind == 1 ? "spin_h" : "spin_v";
    inst->gate = ReadGate(&inst->host);
    const int why = SpinWhy(inst, &inst->host, inst->gate);
    if (why != kWhyOk && why != kWhyEvent) {
        SpinRefuse(inst, act, why);
        return false;
    }
    if (why == kWhyOk && !SpinBusy(inst) && inst->spin_q == 0) {
        SpinStart(inst, kind);
        return true;
    }
    if (inst->spin_q == 0) {
        inst->spin_q = kind;
        inst->spin_q_wait = 0;
        if (inst->host.log != nullptr) {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "sshd: %s queued (%s)", act, WhyName(why == kWhyOk ? kWhyBusy : why));
            inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
    }
    return true; // a tap while one spin already waits is that spin
}

// L corner (Ready bottom-right since dev.47). Owner decision 2026-11-11 explicitly approved a virtual L hold for
// this feature only; dev.37 (owner Thor feedback 2026-11-12) makes it a persistent LOCK:
//  - unlocked (default): the icon follows the game's L as its own pad sampler sees it (GameL; the
//    player physically holding L, no virtual press runs then): eye while held, else the current
//    sword (empty without one). Nothing is pressed.
//  - a tap locks (eye + a lock badge): the guest helper holds L on the game thread (dev.45: the
//    module refreshes the L-hold word, mailbox +0x20, every tick with kLHoldFrames game frames and
//    the stick hook ORs the game's L bit into the controller's button word while it counts, the
//    way it adds L3 for the shield bash), so the camera stays in L mode without the player holding
//    L. A module that stops refreshing the word lets L go by itself within kLHoldFrames frames.
//    dev.37..44 published sshd.l.hold as the manifest's enforce gate and the runtime re-pressed its
//    virtual L every 50 ms, one host log line per press (Thor: 9.2k lines in 10 min rotated the
//    boot lines out of logcat). sshd.l.hold is still published (diagnostics, tests). Another tap
//    unlocks.
//  - the lock is a mode, not a press: menus (2), events / cutscenes / doors (3), the Loftwing (4),
//    the Silent Realms (5), a stalled helper (6), an unreadable menu state (7, dev.44) and loads
//    (9: no stage in the live slot) only
//    PAUSE the hold; it is asserted again kLReengage ticks after free play returns (dev.24..36
//    dropped the toggle there and never re-engaged it after a door or a room load). Only the
//    player's tap unlocks; the title (1: the title / Quest Logs scene), a host reload (Configure)
//    and unloading the module clear it. The player's own L no longer turns it off.
//  - the swipe pad stays the sword: a stroke (or a spin / late flick sequence) suspends the hold,
//    the stroke waits until the game has seen L up for kLSettle ticks (LSettled), and the hold
//    returns kLResume ticks after the stroke.
// Swimming has no reader yet (not covered).
constexpr int kLReleaseWait = 40;  // ticks a stroke / sequence waits for the game to settle L up
constexpr int kLSettle = 12;       // ticks L must read up before the stick is the sword again
constexpr int kLResume = 6;        // ticks after a stroke before the hold is asserted again
constexpr int kLReengage = 20;     // ticks of free play after a pause before the hold returns
constexpr double kFlickMin = 0.35; // late-stroke peak (unit deflection) worth replaying as a flick
constexpr int kLBlockTicks = 3;    // ticks a blocking game state must persist before it pauses
constexpr uint32_t kLHoldFrames = 8;    // refreshed every tick while held; lapses ~8 frames after the module stops

int LBlocked(const Instance* inst, const EdenDsmodHostApi* host) {
    char st[8]{};
    uint32_t scene = 0, state = 0;
    uint64_t events = 0;
    if (!inst->mailbox || inst->heartbeat_still >= 30)
        return 6;
    if (host->read_memory == nullptr ||
        !host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, 4) || scene == 0x53656C74)
        return 1;
    if (!host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, st, sizeof(st)) || st[0] == 0)
        return 9;
    // the debounced pause / map state (MenuGate; dev.44 the layout controller's state, not the stack
    // slot dev.40..43 read here, which also returned 2 on a failed read); 7 = no menu state for longer
    // than the grace (a read failure is no longer reported as a menu)
    if (inst->menu_open)
        return 2;
    if (inst->menu_unread)
        return 7;
    if (!host->read_memory(host->userdata, host->main_base + kEventManager, &events, 8) || events == 0 ||
        !host->read_memory(host->userdata, events + 0x248, &state, 4) || state != 0)
        return 3;
    if (std::strncmp(st, "F020", 8) == 0)
        return 4;
    if (SilentRealm(st))
        return 5;
    return 0;
}

// An LBlocked code as a refusal reason (the swipe, the L corner, the shield bash and a queued
// sequence log it through Refuse).
int LBlockWhy(int blocked) {
    switch (blocked) {
    case 0: return kWhyOk;
    case 1: return kWhyTitle;
    case 2: return kWhyMenu;
    case 3: return kWhyEvent;
    case 4: return kWhyLoftwing;
    case 5: return kWhyRealm;
    case 6: return kWhyHelper;
    case 7: return kWhyUnread; // dev.44: no menu state (MenuGate)
    case 9: return kWhyLoad;
    default: return kWhyUnread;
    }
}

void LLog(const Instance* inst, const char* what, int why) {
    if (inst->host.log != nullptr) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "sshd: L lock %s (%d)", what, why);
        inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
    }
}

// "l_toggle" (the corner tap): unlock always; lock anywhere but the title or a stopped helper (a
// lock tapped while a state pauses it engages once free play returns).
bool LToggle(Instance* inst) {
    if (inst->l_on) {
        inst->l_on = false;
        inst->l_reason = 0;
        LLog(inst, "off (tap)", 0);
        return true;
    }
    const int why = LBlocked(inst, &inst->host);
    if (why == 1 || why == 6) {
        inst->l_reason = why;
        inst->gate = ReadGate(&inst->host);
        char note[40];
        std::snprintf(note, sizeof(note), ": lblocked %d", why);
        Refuse(inst, "l_toggle", LBlockWhy(why), note); // dev.43: one line per refused tap
        return false; // refused haptic, the icon keeps following L
    }
    inst->l_on = true;
    inst->l_suspend = false;
    inst->l_resume = 0;
    inst->l_engage = why == 0 ? 0 : kLReengage;
    inst->l_reason = why;
    LLog(inst, "on (tap)", why);
    return true;
}

// Once per tick after StickMirror (which reports the stroke through inst->dragging).
// The game keeps the right stick on the camera for a few frames after L comes up: live (g0,
// 2026-11-11) a spin staged 6 or 7 ticks after the release never spun, 8 sometimes, 9 / 12 / 15 / 30
// always (stamina 1,000,000 -> 687,500). kLSettle keeps a margin over that.
bool LSettled(const Instance* inst) {
    return inst->l_up >= kLSettle;
}

void LTick(Instance* inst, const EdenDsmodHostApi* host) {
    const bool game_l = GameL(inst);
    inst->l_up = game_l ? 0 : inst->l_up + 1;
    // a game state must hold kLBlockTicks ticks running: the module samples off the game thread,
    // and around an L release the menu word reads non-zero for a frame or two (live 2026-11-11)
    const int blocked = LBlocked(inst, host);
    inst->l_block = blocked != 0 ? inst->l_block + 1 : 0;
    const bool state_ok = blocked == 0 || (blocked != 6 && inst->l_block < kLBlockTicks);
    // dev.45: a stroke's late flick that ended while another sequence owned the stick moves into
    // pend once the stick is free (and then waits for L like any sequence), or is refused (logged)
    if (inst->flick_q) {
        if (inst->seq_wait == 0 && inst->pend_n == 0) {
            inst->flick_q = false;
            SeqQueue(inst, inst->flick_steps, 2, true);
            inst->pend_stroke = inst->flick_stroke;
        } else if (++inst->flick_q_wait > kSpinQueueWait) {
            inst->flick_q = false;
            inst->swipe_flash = std::max(kSwipeFlash, inst->diag.hold_ticks);
            inst->gate = ReadGate(host);
            char note[80];
            std::snprintf(note, sizeof(note), ": stroke %u, late flick, a sequence kept the stick %d ticks",
                          inst->flick_stroke, inst->flick_q_wait);
            Refuse(inst, "swipe", kWhyBusy, note);
        }
    }
    // a queued sequence (spin, late-stroke flick) starts once the game has seen L up long enough.
    // dev.48: it stages only in free play (LBlocked 0). A blocking state still inside its kLBlockTicks
    // grace keeps the lock holding but never stages a sequence: the sequence waits (at most the grace,
    // a few ticks) and plays if the state was a blip, else it is refused once the grace ends.
    const bool ready = inst->pend_n > 0 && (LSettled(inst) || ++inst->pend_wait > kLReleaseWait);
    if (ready && !(blocked != 0 && state_ok)) {
        const bool hands = SpinHands(inst->gate);
        const bool staged =
            blocked == 0 && hands && SeqStage(inst, inst->pend, static_cast<size_t>(inst->pend_n), inst->pend_quiet);
        if (staged) {
            inst->seq_quiet_stroke = inst->pend_quiet ? inst->pend_stroke : 0;
        } else {
            // dev.43: the dropped sequence's "Not now" logs one line; dev.45 a stroke's late flick too
            // (dev.24..44 dropped it without a word), and a sequence the mailbox would not take
            const int why = blocked != 0 ? LBlockWhy(blocked) : !hands ? kWhyItem : kWhyHelper;
            inst->gate = ReadGate(host);
            char note[96];
            if (inst->pend_quiet) {
                inst->swipe_flash = std::max(kSwipeFlash, inst->diag.hold_ticks);
                std::snprintf(note, sizeof(note), ": stroke %u, late flick, lblocked %d, l_block %d", inst->pend_stroke,
                              blocked, inst->l_block);
                Refuse(inst, "swipe", why, note);
            } else {
                inst->seq_flash = Flash(inst);
                std::snprintf(note, sizeof(note), ": lblocked %d, l_block %d", blocked, inst->l_block);
                Refuse(inst, "queued sequence", why, note);
            }
        }
        inst->pend_n = 0;
    }
    if (inst->l_on && !state_ok && blocked == 1) { // the title / Quest Logs: the lock ends
        inst->l_on = false;
        inst->l_reason = 1;
        LLog(inst, "off (title)", 1);
    }
    const bool paused = inst->l_on && !state_ok;
    if (paused) {
        if (inst->l_engage == 0 || inst->l_reason != blocked) {
            if (inst->l_engage == 0)
                LLog(inst, "paused", blocked);
            inst->l_reason = blocked;
        }
        inst->l_engage = kLReengage;
    } else if (inst->l_on && inst->l_engage > 0 && --inst->l_engage == 0) {
        LLog(inst, "re-engaged", inst->l_reason);
    }
    const bool stroke = inst->dragging || inst->seq_wait > 0 || inst->pend_n > 0 || inst->flick_q;
    if (!inst->l_on) {
        inst->l_suspend = false;
    } else if (stroke) {
        inst->l_suspend = true;
        inst->l_resume = kLResume;
    } else if (inst->l_suspend && --inst->l_resume <= 0) {
        inst->l_suspend = false;
    }
    const bool hold = inst->l_on && !paused && inst->l_engage == 0 && !inst->l_suspend;
    // dev.45: the helper holds L while this word counts; 0 lets go at the next stick pass
    if (inst->mailbox)
        inst->ext.store_u32(inst->ext.userdata, kOffLHold, hold ? kLHoldFrames : 0u);
    host->publish_i64(host->userdata, "sshd.l.on", inst->l_on ? 1 : 0); // the lock (badge)
    host->publish_i64(host->userdata, "sshd.l.eye", inst->l_on || game_l ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.l.hold", hold ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.l.game", game_l ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.l.reason", inst->l_reason);
    host->publish_i64(host->userdata, "sshd.l.blk", blocked); // diagnostics only (the LBlocked reason), unbound
}

// Shield bash (Ready top-left corner tap; owner 2026-11-11 approved a virtual L3 press for this
// feature only). The game's button-only bash is a left-stick click with a shield in the pouch; no
// callable bash routine was found (combat button analysis) and the runtime's button
// tokens have no stick click, so the helper adds the L3 bit to the controller's own button word for
// kBashFrames game frames (mailbox +0x18, sshd_stick_helper.S) and the game decides the rest.
// Refused ("Not now") with no usable shield equipped (ids 116-125, a breakable one with durability
// left), an item in Link's hands other than the sword, the menus, an event, the title,
// the Loftwing, a Silent Realm, or a stopped helper (LBlocked).
constexpr uint32_t kBashFrames = 4; // mailbox kOffBash (L3); the sword stab's R3 (kOffStab) as long

// The equipped shield: the pouch slot in file+0x53B1 (8 = none; the slot the native Item Check moves
// along with a swap, sshd_stick_helper.S op 1). hero = slot 2, Sacred Shield 20; g0's save-editor
// file carries an Iron Shield in the pouch with 0x53B1 = 8, and Link wears none (live 2026-11-11).
bool ShieldReady(const EdenDsmodHostApi* host) {
    uint16_t entry[2]{};
    uint8_t slot = 8;
    if (host->read_memory == nullptr ||
        !host->read_memory(host->userdata, host->main_base + kSlot + 0x53B1, &slot, 1) || slot > 7 ||
        !host->read_memory(host->userdata, host->main_base + kSlot + 0x7C0 + 4u * slot, entry, sizeof(entry)))
        return false;
    const int id = entry[0], dur = entry[1];
    return id == 125 || (id >= 116 && id <= 124 && dur > 0); // the Hylian Shield never breaks
}

bool ShieldBash(Instance* inst) {
    const EdenDsmodHostApi* host = &inst->host;
    inst->gate = ReadGate(host);
    const GateIn& g = inst->gate;
    uint32_t left = 1;
    const int blocked = LBlocked(inst, host);
    int why = kWhyOk;
    if (blocked != 0)
        why = LBlockWhy(blocked); // dev.44: one mapping (7 = an unreadable menu state is not a realm)
    else if (g.player == 0 || g.held < 0)
        why = kWhyNoPlayer;
    else if (!SpinHands(g))
        why = kWhyItem;
    else if (!ShieldReady(host))
        why = kWhyNoShield;
    else if (!inst->ext.load_u32(inst->ext.userdata, kOffBash, &left) || left != 0)
        why = kWhyBusy;
    else if (!inst->ext.load_u32(inst->ext.userdata, kOffStab, &left) || left != 0)
        why = kWhyBusy; // dev.48: a sword stab's R3 still counts (never L3 and R3 in one frame)
    inst->why_bash = why;
    if (why != kWhyOk) {
        inst->bash_flash = Flash(inst);
        inst->bash_why = why;
        Refuse(inst, "shield_bash", why);
        return false;
    }
    inst->ext.store_u32(inst->ext.userdata, kOffBash, kBashFrames);
    if (host->log != nullptr)
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, "sshd: shield bash (L3) staged");
    return true;
}

// Link in water (dev.33; reader analysis): player = *(main+0x1826AD0),
// in-water bit u32 +0x460 & 0x40000 (= +0x462 bit 2, the bit the HD gauge logic consumes), action
// u32 +0x468. 0 = not in water, 1 = the verified surface states (action 0x4F swim, 0x56 Look) outside
// Skyloft, 2 = any other water state (unverified), -1 = unreadable.
constexpr uint64_t kPlayerPtr = 0x1826AD0;
constexpr uint32_t kInWater = 0x40000, kActSwim = 0x4F, kActSwimLook = 0x56;
int SwimState(const EdenDsmodHostApi* host) {
    uint64_t player = 0;
    uint32_t flags = 0, action = 0;
    char st[8]{};
    if (host->read_memory == nullptr ||
        !host->read_memory(host->userdata, host->main_base + kPlayerPtr, &player, 8))
        return -1;
    if (player == 0)
        return 0; // no player object (title, loading): the pane mirror decides
    if (!host->read_memory(host->userdata, player + 0x460, &flags, 4) ||
        !host->read_memory(host->userdata, player + 0x468, &action, 4) ||
        !host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, st, sizeof(st)))
        return -1;
    if ((flags & kInWater) == 0)
        return 0;
    const bool skyloft = std::strncmp(st, "F000", 8) == 0;
    return !skyloft && (action == kActSwim || action == kActSwimLook) ? 1 : 2;
}

// dev.46 pressed look clock (PressTick publishes it as sshd.ms): ms since the module was created.
int64_t ClockMs(const Instance* inst) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - inst->press_t0)
        .count();
}

// dev.47: Ready map tap, owner-approved R3 through the same bounded mailbox path as L3.
// Refuse rather than queue: the finger must never leave a click waiting for a later game state.
// dev.48: empty hands refuse as "sword not drawn", water as "swimming" (dev.47 logged "item in hand"
// and "unreadable"); a shield bash's L3 still counting is busy; the map rim lights (sshd.stab.lit,
// PressTick) only for a stab that was staged, never for a refused tap.
bool SwordStab(Instance* inst) {
    const auto* host = &inst->host;
    inst->gate = ReadGate(host);
    int why = SpinWhy(inst, host, inst->gate);
    uint32_t left = 1, bash = 1;
    if (why == kWhyOk && SilentRealm(inst->gate.stage))
        why = kWhyRealm;
    if (why == kWhyOk && inst->gate.held == kHeldNone)
        why = kWhyNotDrawn;
    if (why == kWhyOk && inst->gate.held != kHeldSword)
        why = kWhyItem;
    if (why == kWhyOk) {
        const int swim = SwimState(host);
        if (swim < 0)
            why = kWhyUnread;
        else if (swim != 0)
            why = kWhySwim;
    }
    if (why == kWhyOk && (SpinBusy(inst) || inst->spin_q != 0 ||
        !inst->ext.load_u32(inst->ext.userdata, kOffStab, &left) || left != 0 ||
        !inst->ext.load_u32(inst->ext.userdata, kOffBash, &bash) || bash != 0))
        why = kWhyBusy;
    if (why != kWhyOk || !inst->ext.store_u32(inst->ext.userdata, kOffStab, kBashFrames)) {
        inst->swipe_flash = std::max(kSwipeFlash, inst->diag.hold_ticks);
        Refuse(inst, "sword_stab", why == kWhyOk ? kWhyHelper : why);
        return false;
    }
    inst->stab_at = ClockMs(inst);
    if (host->log != nullptr)
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, "sshd: sword stab (R3) staged");
    return true;
}

// The native stamina gauge's state (dev.33; reader analysis "Stamina
// exhausted", two boots): G = *(main+0x183C9A0) + 0x1BD0 with its vtable main+0x165F8F8; wheel state
// u64 G+0xD0 (the state descriptor: ToMin main+0x165F810, Recovery main+0x165F858 = exhausted), the
// colour selector f32 G+0x910 (2 = the red land-wheel palette, which lingers into the refill like the
// native wheel). The Ready ring takes the red segments while the palette is red; eligibility
// (exhausted) is published separately. Unreadable / another object: green, not exhausted.
constexpr uint64_t kGaugeVt = 0x165F8F8, kGaugeToMin = 0x165F810, kGaugeRecovery = 0x165F858;
void PublishStamina(const EdenDsmodHostApi* host) {
    uint64_t m = 0, vt = 0, state = 0;
    float colour = 0.0f;
    const uint64_t main = host->main_base;
    const bool ok = host->read_memory != nullptr && host->read_memory(host->userdata, main + 0x183C9A0, &m, 8) &&
                    m != 0 && host->read_memory(host->userdata, m + 0x1BD0, &vt, 8) && vt == main + kGaugeVt &&
                    host->read_memory(host->userdata, m + 0x1BD0 + 0xD0, &state, 8) &&
                    host->read_memory(host->userdata, m + 0x1BD0 + 0x910, &colour, 4);
    const bool red = ok && colour == 2.0f;
    const bool exhausted = ok && (state == main + kGaugeToMin || state == main + kGaugeRecovery);
    host->publish_i64(host->userdata, "sshd.stam.valid", ok ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.stam.red", red ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.stam.exh", exhausted ? 1 : 0);
}

// Ready D-pad cluster (owner 2026-11-11: "change stamina wheel to dpad info like in top screen",
// display only). sshd_dpad.cpp mirrors the rendered native panes; each layer is published as an
// opacity level 0..15 (0 = hidden, the manifest's need_bind; else tint_colors[level] carries the
// native effective alpha). Labels: Up Look / Dowse / Back and Right Help / Close, each its own
// level; the selected dowsing target's art key. Unsure reads publish all zeros (fail closed).
void PublishDpad(Instance* inst, const EdenDsmodHostApi* host) {
    static constexpr const char* kKeys[sshd::kDpLayerCount] = {
        "sshd.dp.base_c", "sshd.dp.base_d", "sshd.dp.base_h", "sshd.dp.up",   "sshd.dp.down",
        "sshd.dp.left",   "sshd.dp.right",  "sshd.dp.eye",    "sshd.dp.fi",   "sshd.dp.fis0",
        "sshd.dp.fis1",   "sshd.dp.fih0",   "sshd.dp.fih1",   "sshd.dp.bird", nullptr,
        nullptr,          "sshd.dp.target"}; // dev.41: the two text layers publish through look .. close below
    const sshd::DpadRead rd = [host](uint64_t a, void* out, size_t n) {
        return host->read_memory(host->userdata, a, out, n) != EDEN_DSMOD_FALSE;
    };
    sshd::DpadState s = host->read_memory != nullptr ? inst->dpad.Sample(rd, host->main_base) : sshd::DpadState{};
    // Swimming (dev.33, reader analysis "Surface swimming", two boots): the
    // native in-water bit (player +0x460 mask 0x40000) with player action 0x4F (surface swim) or
    // 0x56 (swimming Look) is the measured surface state, where the native cluster itself shows the
    // eye / Up and Help / Close and hides Fi, which the pane mirror above follows. Any other water
    // state (diving / underwater, the scale-on 0x4E) and Skyloft's lake are not verified: the whole
    // cluster stays hidden there (fail closed). sshd.dp.swim: 0 dry, 1 surface, 2 hidden in water.
    const int swim = SwimState(host);
    if (swim == 2 || swim < 0)
        s = sshd::DpadState{};
    host->publish_i64(host->userdata, "sshd.dp.swim", swim);
    auto level = [](uint8_t a) { return a == 0 ? 0 : std::max(1, (a * 15 + 127) / 255); };
    for (int l = 0; l < sshd::kDpLayerCount; ++l)
        if (kKeys[l] != nullptr)
            host->publish_i64(host->userdata, kKeys[l], level(s.alpha[l]));
    const int ut = level(s.alpha[sshd::kDpUpText]), ht = level(s.alpha[sshd::kDpHelpText]);
    host->publish_i64(host->userdata, "sshd.dp.look", s.up_text == 1 ? ut : 0);
    host->publish_i64(host->userdata, "sshd.dp.dowse", s.up_text == 2 ? ut : 0);
    host->publish_i64(host->userdata, "sshd.dp.back", s.up_text == 3 ? ut : 0);
    host->publish_i64(host->userdata, "sshd.dp.help", s.help_close ? 0 : ht);
    host->publish_i64(host->userdata, "sshd.dp.close", s.help_close ? ht : 0);
    host->publish_i64(host->userdata, "sshd.dp.valid", s.valid ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.dp.fiatt", s.fi_attention ? 1 : 0);
    if (host->publish_text != nullptr) {
        const std::string key = s.target_logical >= 0 ? "module:sshd:dpad/target/" + std::to_string(s.target_logical) : "";
        host->publish_text(host->userdata, "sshd.dp.tsrc", key.c_str());
    }
}

// Top-screen HUD hiding (owner 2026-11-11 "hide top screen dpad and rupee", default hidden; Bag >
// Options "Top D-pad" / "Top rupees" show them again, flags hud.dpad / hud.rupee = 1). The helper's
// draw hook skips the listed panes (sshd_hudhide.h); nothing in the game's panes is written, so
// PublishDpad above keeps mirroring the native state and the bottom rupee counter reads the file.
// dev.37 (owner Thor feedback: the top D-pad "blinking instead of hidden"): the list is written
// into the bank the helper is not reading and published with one select-word store (never count 0
// in between), its 120-frame ttl refreshed every sample; a refused sample or a stalled helper only
// pauses the refresh (sshd::HudHide). Cleared when a setting shows the copy again, on configure
// and on unload.
constexpr uint32_t kOffHideTtl = 0x200, kOffHideSel = 0x204, kOffHideHits = 0x208, kOffHideList = 0x210,
                   kHideBankBytes = 0x100;
// draw probe (diagnostics, SSHD_DIAG): the pane, this frame's bits, frames drawn / skipped / counted
constexpr uint32_t kOffProbe = 0x410, kOffProbeDrawn = 0x41C, kOffProbeSkipped = 0x420, kOffProbeFrames = 0x424;
void DiagLog(const Instance* inst, const char* msg) {
    if (inst->diag.on && inst->host.log != nullptr)
        inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
}
void ClearTopHide(Instance* inst) {
    if (inst->mailbox && inst->hide.Active()) {
        inst->ext.store_u32(inst->ext.userdata, kOffHideSel, inst->hide_bank << 8);
        inst->ext.store_u32(inst->ext.userdata, kOffHideTtl, 0);
    }
    inst->hide.Reset();
}
// the probe's frames since the last sample, by the game state now (play / menu / other)
void DiagProbe(Instance* inst, const EdenDsmodHostApi* host) {
    Diag& d = inst->diag;
    if (!d.on || !inst->mailbox)
        return;
    const auto& e = inst->ext;
    uint32_t drawn = 0, skipped = 0, frames = 0;
    e.load_u32(e.userdata, kOffProbeDrawn, &drawn);
    e.load_u32(e.userdata, kOffProbeSkipped, &skipped);
    e.load_u32(e.userdata, kOffProbeFrames, &frames);
    uint8_t stage = 0;
    uint32_t scene = 0;
    const bool rd = host->read_memory != nullptr &&
                    host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, &stage, 1) &&
                    host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, 4);
    const int st = !rd || stage == 0 || scene == 0x53656C74 ? 2 : inst->menu_open ? 1 : 0;
    d.drawn[st] += drawn - d.last_drawn;
    d.skipped[st] += skipped - d.last_skipped;
    d.frames[st] += frames - d.last_frames;
    if (drawn != d.last_drawn) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "sshd diag hide: top D-pad drawn %u frame(s), state %d", drawn - d.last_drawn, st);
        DiagLog(inst, msg);
    }
    d.last_drawn = drawn;
    d.last_skipped = skipped;
    d.last_frames = frames;
}
void DiagSummary(const Instance* inst) {
    const Diag& d = inst->diag;
    char msg[400];
    std::snprintf(msg, sizeof(msg),
                  "sshd diag summary: samples %llu due %llu | top D-pad frames play %llu drawn %llu skipped %llu | menu %llu "
                  "drawn %llu skipped %llu | other %llu drawn %llu skipped %llu | list writes %llu clears %llu holds %llu "
                  "resolves %d | cone on %llu blank %llu switches %llu held-on-miss %llu",
                  (unsigned long long)d.samples, (unsigned long long)d.due_samples, (unsigned long long)d.frames[0],
                  (unsigned long long)d.drawn[0], (unsigned long long)d.skipped[0], (unsigned long long)d.frames[1],
                  (unsigned long long)d.drawn[1], (unsigned long long)d.skipped[1], (unsigned long long)d.frames[2],
                  (unsigned long long)d.drawn[2], (unsigned long long)d.skipped[2], (unsigned long long)d.writes,
                  (unsigned long long)d.clears, (unsigned long long)d.holds, inst->hide_res.Resolves(),
                  (unsigned long long)d.cone_on, (unsigned long long)d.cone_blank, (unsigned long long)d.cone_switch,
                  (unsigned long long)d.cone_held);
    DiagLog(inst, msg);
}
void ApplyTopHide(Instance* inst, const EdenDsmodHostApi* host) {
    const auto flag = [host](const char* k) { return host->get_i64 != nullptr && host->get_i64(host->userdata, k, 0) != 0; };
    const bool show_dpad = flag("@flag:hud.dpad"), show_rupee = flag("@flag:hud.rupee");
    const bool legacy = inst->diag.legacy;
    DiagProbe(inst, host);
    if (!inst->diag.due) { // throttled (diagnostics): the module "did not sample" this time
        host->publish_i64(host->userdata, "sshd.hud.hidden", inst->hide_groups);
        return;
    }
    const bool helper_ok = inst->mailbox && inst->loop_still < 30;
    sshd::HideTargets t;
    const int resolves = inst->hide_res.Resolves();
    if (helper_ok && host->read_memory != nullptr && (!show_dpad || !show_rupee)) {
        const sshd::DpadRead rd = [host](uint64_t a, void* out, size_t n) {
            return host->read_memory(host->userdata, a, out, n) != EDEN_DSMOD_FALSE;
        };
        t = inst->hide_res.Sample(rd, host->main_base);
    }
    const sshd::HideStep s = inst->hide.Step(show_dpad, show_rupee, helper_ok, t, legacy, inst->menu_open);
    const auto& e = inst->ext;
    char msg[160];
    if (inst->diag.on && inst->hide_res.Resolves() != resolves && t.valid && !t.entries.empty()) {
        std::snprintf(msg, sizeof(msg), "sshd diag hide: panes looked up (#%d), %zu entries, first %#llx, help %d menu %d",
                      inst->hide_res.Resolves(), t.entries.size(), (unsigned long long)t.entries[0].pane, t.help ? 1 : 0,
                      inst->menu_open ? 1 : 0);
        DiagLog(inst, msg);
    }
    if (inst->diag.on && inst->mailbox && t.valid) // probe the Cross base pane (the list's first D-pad entry)
        for (const auto& h : t.entries)
            if (h.group == sshd::kHideDpad) {
                if (h.pane != inst->diag.probe) {
                    e.store_u32(e.userdata, kOffProbe, static_cast<uint32_t>(h.pane));
                    e.store_u32(e.userdata, kOffProbe + 4, static_cast<uint32_t>(h.pane >> 32));
                    inst->diag.probe = h.pane;
                }
                break;
            }
    const uint32_t ttl = legacy ? sshd::kHideTtlLegacy : sshd::kHideTtl;
    switch (s.kind) {
    case sshd::HideStep::kWrite: {
        // dev.37: the bank the hook is not reading, then bank + count in one store (legacy: count 0
        // first, the same bank, then the count: the hook drew the panes in between)
        const uint32_t bank = legacy ? inst->hide_bank : inst->hide_bank ^ 1u;
        if (legacy)
            e.store_u32(e.userdata, kOffHideSel, bank << 8);
        for (size_t i = 0; i < s.list.size(); ++i) {
            const uint32_t at = kOffHideList + kHideBankBytes * bank + 16 * static_cast<uint32_t>(i);
            e.store_u32(e.userdata, at, static_cast<uint32_t>(s.list[i].pane));
            e.store_u32(e.userdata, at + 4, static_cast<uint32_t>(s.list[i].pane >> 32));
            e.store_u32(e.userdata, at + 8, s.list[i].name_lo);
            e.store_u32(e.userdata, at + 12, s.list[i].name_hi);
        }
        e.store_u32(e.userdata, kOffHideSel, bank << 8 | static_cast<uint32_t>(s.list.size()));
        e.store_u32(e.userdata, kOffHideTtl, ttl);
        inst->hide_bank = bank;
        ++inst->diag.writes;
        std::snprintf(msg, sizeof(msg), "sshd diag hide: list written, %zu entries, bank %u", s.list.size(), bank);
        DiagLog(inst, msg);
        break;
    }
    case sshd::HideStep::kRefresh:
        e.store_u32(e.userdata, kOffHideTtl, ttl);
        break;
    case sshd::HideStep::kClear:
        e.store_u32(e.userdata, kOffHideSel, inst->hide_bank << 8);
        e.store_u32(e.userdata, kOffHideTtl, 0);
        ++inst->diag.clears;
        std::snprintf(msg, sizeof(msg), "sshd diag hide: list cleared (valid %d help %d menu %d helper %d)", t.valid ? 1 : 0,
                      t.help ? 1 : 0, inst->menu_open ? 1 : 0, helper_ok ? 1 : 0);
        DiagLog(inst, msg);
        break;
    case sshd::HideStep::kNone:
        if (inst->hide.Active())
            ++inst->diag.holds;
        break;
    }
    int groups = 0;
    uint32_t hits = 0;
    if (inst->hide.Active()) {
        for (const auto& h : t.entries)
            if ((h.group == sshd::kHideDpad && !show_dpad) || (h.group == sshd::kHideRupee && !show_rupee))
                groups |= h.group;
        if (!t.valid) // holding: what was last published
            groups = inst->hide_groups;
        if (inst->mailbox)
            e.load_u32(e.userdata, kOffHideHits, &hits);
    }
    inst->hide_groups = groups;
    host->publish_i64(host->userdata, "sshd.hud.hidden", groups); // 1 D-pad, 2 rupees (diagnostics)
    host->publish_i64(host->userdata, "sshd.hud.hits", hits);
    if (inst->diag.on && inst->diag.samples % 1800 == 0)
        DiagSummary(inst);
}

// Opened ordinary chests (map-marker contract 2026-11-11: "native displays an opened normal chest",
// LIVE high on hero F402). Only stages whose BZS was decoded are listed (F401 / F402
// share room geometry, F020), layer-0 TBox actors with a chest flag below 32 and no Goddess
// story id; a chest shows once its flag ((rot.z >> 9) & 63) is set in the stage's tbox word
// (slot+0x2A64 + 4 * STIF flagindex, OR'ed with the live current-scene word). Unopened chests
// (native Dungeon Map gate) and every other stage stay hidden: fail closed.
struct OpenChest {
    uint8_t flag;
    float x, z;
};
struct ChestStage {
    const char* stage;
    int scene; // STIF flagindex
    bool u_is_x, v_is_minus_z;
    const OpenChest* chests;
    int n;
};
constexpr OpenChest kChestsF401[] = {{0, -1235.0f, 12500.0f}};
constexpr OpenChest kChestsF020[] = {{5, 135833.0f, 76091.0f}, {6, -145706.0f, 44281.0f}};
// (dungeons: every chest comes from the live object walk instead, PublishDungeonMarkers, dev.30)
constexpr ChestStage kChestStages[] = {
    {"F401", 10, false, false, kChestsF401, 1}, {"F402", 10, false, false, kChestsF401, 1},
    {"F020", 21, true, true, kChestsF020, 2},
};
constexpr int kOpenChestSlots = 5;
void PublishOpenChests(const EdenDsmodHostApi* host) {
    static constexpr const char* kKeys[kOpenChestSlots][3] = {
        {"sshd.ch0.on", "sshd.ch0.u", "sshd.ch0.v"}, {"sshd.ch1.on", "sshd.ch1.u", "sshd.ch1.v"},
        {"sshd.ch2.on", "sshd.ch2.u", "sshd.ch2.v"}, {"sshd.ch3.on", "sshd.ch3.u", "sshd.ch3.v"},
        {"sshd.ch4.on", "sshd.ch4.u", "sshd.ch4.v"}};
    char st[9]{};
    const uint64_t slot = host->main_base + kSlot;
    const ChestStage* cs = nullptr;
    if (host->read_memory(host->userdata, slot + 0x531C, st, 8))
        for (const auto& s : kChestStages)
            if (std::strcmp(st, s.stage) == 0)
                cs = &s;
    uint32_t saved = 0, live = 0, scene = 0;
    if (cs != nullptr && (!host->read_memory(host->userdata, slot + 0x2A64 + 4 * cs->scene, &saved, 4) ||
                          !host->read_memory(host->userdata, host->main_base + kTboxCurrent, &live, 4) ||
                          !host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, 4) ||
                          scene == 0x53656C74))
        cs = nullptr;
    const uint32_t opened = saved | live;
    if (host->publish_text != nullptr)
        host->publish_text(host->userdata, "sshd.ch.src", "module:sshd:openchest");
    for (int i = 0; i < kOpenChestSlots; ++i) {
        const bool on = cs != nullptr && i < cs->n && ((opened >> cs->chests[i].flag) & 1) != 0;
        host->publish_i64(host->userdata, kKeys[i][0], on ? 1 : 0);
        if (!on)
            continue;
        host->publish_f64(host->userdata, kKeys[i][1], cs->u_is_x ? cs->chests[i].x : -cs->chests[i].x);
        host->publish_f64(host->userdata, kKeys[i][2], cs->v_is_minus_z ? -cs->chests[i].z : cs->chests[i].z);
    }
}

// Link's view cone on the maps (map-marker contract 2026-11-11, map marker analysis):
// the facing is the player's rotation Y, s16 main+0x623E7BE (live correlation with the native
// cone). A facing yaw16 points along game (sin t, cos t) in (x, z); the companion maps draw
// u = -x (or +x on the native-x maps: AreaMap flip_h, and the Sky) and v = z (or -z on the
// flip_v maps and the Sky), y up. Calibrated on the native Sealed Grounds map (captures 01 / 03:
// yaw 14807 -> cone 81 degrees left of up, yaw 1972 -> 11 degrees left of up). Published as the
// pre-turned picture cone/<deg>, 5-degree steps; off (fail closed) on any unknown stage or read.
int ConeDegrees(int16_t yaw, bool u_is_x, bool v_is_minus_z) {
    const double t = yaw * (2.0 * 3.14159265358979323846 / 65536.0);
    const double dx = (u_is_x ? 1.0 : -1.0) * std::sin(t);  // screen right
    const double up = (v_is_minus_z ? -1.0 : 1.0) * std::cos(t); // screen up
    double deg = std::atan2(dx, up) * 180.0 / 3.14159265358979323846;
    int q = static_cast<int>(std::lround(deg / 5.0)) * 5;
    q %= 360;
    return q < 0 ? q + 360 : q;
}
// dev.37 (owner Thor feedback "bottom view cone some time blinking when move"): the picture shown is
// the last one known to be loaded, the wanted one is requested through an off-screen prefetch
// marker until it is (sshd_cone.h); a failed read keeps the last answer for a few samples.
constexpr double kConeAway = 1.0e7; // map units: the prefetch marker sits far outside every view
void PublishConeOut(const EdenDsmodHostApi* host, const sshd::ConeHold::Out& o) {
    host->publish_i64(host->userdata, "sshd.cone.on", o.on && o.shown >= 0 ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.cone.pf", o.on && o.prefetch >= 0 ? 1 : 0);
    char key[48];
    if (o.on && o.shown >= 0) {
        std::snprintf(key, sizeof(key), "module:sshd:cone/%d", o.shown);
        host->publish_text(host->userdata, "sshd.cone.src", key);
    }
    if (o.on && o.prefetch >= 0) {
        std::snprintf(key, sizeof(key), "module:sshd:cone/%d", o.prefetch);
        host->publish_text(host->userdata, "sshd.cone.pfsrc", key);
        if (host->publish_f64 != nullptr) {
            host->publish_f64(host->userdata, "sshd.cone.pfu", kConeAway);
            host->publish_f64(host->userdata, "sshd.cone.pfv", kConeAway);
        }
    }
}
// diagnostics: one count per sample (60 Hz), throttled samples included: is the published cone a
// picture the host can draw?
void DiagCone(Instance* inst, int deg) {
    Diag& d = inst->diag;
    const sshd::ConeHold::Out& o = inst->cone_out;
    if (!d.on || !o.on || o.shown < 0) {
        d.last_shown = o.on ? o.shown : d.last_shown;
        return;
    }
    ++d.cone_on;
    const bool ready = inst->cone.ShownReady();
    d.cone_ready_once = d.cone_ready_once || ready;
    char msg[96];
    if (!ready && d.cone_ready_once) {
        ++d.cone_blank;
        std::snprintf(msg, sizeof(msg), "sshd diag cone: blank sample, shown %d not loaded (wanted %d)", o.shown, deg);
        DiagLog(inst, msg);
    }
    if (o.shown != d.last_shown) {
        ++d.cone_switch;
        std::snprintf(msg, sizeof(msg), "sshd diag cone: shown %d (wanted %d, ready %d)", o.shown, deg, ready ? 1 : 0);
        DiagLog(inst, msg);
    }
    d.last_shown = o.shown;
}
void PublishCone(Instance* inst, const EdenDsmodHostApi* host, bool away) {
    if (inst != nullptr && !inst->diag.due) { // throttled (diagnostics): the last answer again
        PublishConeOut(host, inst->cone_out);
        DiagCone(inst, -1);
        return;
    }
    char st[9]{};
    int16_t yaw = 0;
    uint32_t scene = 0;
    const bool read = host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, st, 8) &&
                      host->read_memory(host->userdata, host->main_base + 0x623E7BE, &yaw, sizeof(yaw)) &&
                      host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, sizeof(scene));
    // the stage drawn with another stage's picture uses that picture's axes (manifest ALIASES)
    std::string_view s(st);
    static constexpr std::pair<std::string_view, std::string_view> kAlias[] = {
        {"F402", "F401"}, {"D201_1", "D201"}, {"D300_1", "D300"}, {"D301_1", "D301"}};
    for (const auto& [from, to] : kAlias)
        if (s == from)
            s = to;
    // dev.35: Inside the Thunderhead (F023) is drawn on the Sky's picture, in its frame
    const bool sky = s == "F020" || s == "F023";
    const sshd::AreaMap* area = read && scene != 0x53656C74 && !sky ? sshd::FindArea(s) : nullptr;
    // off on the Map page while it shows another floor than Link's (dev.30)
    const bool on = !away && (sky ? read && scene != 0x53656C74 : area != nullptr);
    int deg = -1;
    if (on) {
        if (!sky && area->gen) // dev.34: a turned generic frame
            yaw = static_cast<int16_t>(yaw - area->rot * 0x4000);
        deg = sky ? ConeDegrees(yaw, true, true) : ConeDegrees(yaw, area->flip_h, area->flip_v);
    }
    if (inst == nullptr) {
        PublishConeOut(host, {on, deg, -1});
        return;
    }
    const sshd::ConeHold::Out o = inst->cone.Step(read, on, deg, inst->diag.legacy);
    inst->cone_out = o;
    PublishConeOut(host, o);
    if (inst->diag.on && !read && o.on)
        ++inst->diag.cone_held;
    DiagCone(inst, deg);
}

// ---------------------------------------------------------------- live map actors (dev.27)
// Bird statues, door bars and neighbouring-area labels exactly as the native field map enqueues
// them (main+0xC7AE78, map marker analysis): the native walks the game's
// connect tree of live objects (root *(main+0x1688510); node[0] parent, [1] first child, [3] next
// sibling, [4] the object) and draws per object profile (u16 object+0x10), skipping hidden
// objects (property bit 0x100 of u32 +0x15C). Positions are the object's +0x144 (x, y, z), the
// yaw its rotation +0x13E. Walking the same tree here needs no per-stage tables, follows the
// active story layer automatically and never shows an object the game has not spawned.
constexpr uint64_t kObjTree = 0x1688510;
constexpr uint16_t kProfDoorA = 0xCA, kProfDoorB = 0xCB, kProfDoor = 0xCD, kProfStatue = 0x216, kProfMapMark = 0x224,
                   kProfBeedle = 0x69;
// Kikwi (the Kikwi marker analysis): EA Machi, EB / 13C Oolo, ED Lopsa, EE Erla, EC the Elder
constexpr uint16_t kProfKikwi[] = {0xEA, 0xEB, 0x13C, 0xED, 0xEE, 0xEC};
bool IsKikwi(uint16_t p) {
    return std::find(std::begin(kProfKikwi), std::end(kProfKikwi), p) != std::end(kProfKikwi);
}
// dAcOSave_c::checkStatueFlag as an object member function pointer (+0x928): main+0x8BA1BC tests
// the scene flag (params & 0xFF) in the object's room; main+0x8BA1EC is "always activated".
constexpr uint64_t kStatueFlagFn = 0x8BA1BC, kStatueAlwaysFn = 0x8BA1EC;
constexpr int kTriforceSlots = 3; // dev.34: Sky Keep's Triforce pieces
constexpr int kStatueSlots = 8, kDoorSlots = 24, kPlaceSlots = 4, kKeySlots = 8, kChestSlots = 16, kMarkSlots = 6,
              kBalloonSlots = 4, kCaveSlots = 2, kKikwiSlots = 5, kTargetSlots = 16, kBugSlots = 16;
constexpr uint16_t kProfBugTag = 0x1C1; // insect spawn tag (Bug Medal locations, dev.33)
// dev.34 (item 10): profile 0x1A7 is the actor "Bee" (static map analysis of
// the actor names). The field class's group-2 walker emits its bug dot (N_mushi_00, category
// 3) at main+0xC7DFF4 only after `ldrb [map+0x27A8 + 2]` (main+0xC7DFD8: map+0x27AA, the Bug Medal
// flag the field gate sets, the field bug analysis), skips it when hidden (+0x15D bit 0) or off the
// shown floor: the same medal gate as the insect tags. The Sky class (main+0xC70B14) emits another
// kind (0x13) for it, not drawn here.
constexpr uint16_t kProfBee = 0x1A7;
struct LiveObj {
    uint16_t prof;
    uint32_t params;
    float x, y, z;
    int16_t yaw;
    int8_t room;
    uint8_t b7c0;   // door: the native draws one door of a double door (+0x7C0 != 0)
    bool pair;      // door: +0x748 set (one of a double door: the bar sits 70 units in)
    uint64_t pmf;   // statue: checkStatueFlag member pointer, main-relative
    uint8_t mm[0x18]; // map mark: +0x190..+0x1A7 (flags, condition, label, type)
    // dungeon doors (dev.30): the vtable's lock (+0xF8) / known-room (+0x100) methods, main-relative,
    // and the lock fields their methods read
    uint64_t m_lock, m_known;
    uint64_t c650, p540, c760; // 0xCA lock child, 0xCB lock pointer, 0xCD lock child
    uint8_t b802, b803;        // 0xCA unlock flag (signed), lock type
    uint8_t b7b9, b7ba;        // 0xCD lock type, flag
    uint8_t opened;            // chest (0x1E8): +0x1994, 1 = opened; bug tag (0x1C1): 1 = drawn
    uint16_t fid;              // Kikwi: the face callback's story id (+0x19FA); pmf holds the callback
};
// Bounded walk (sshd::NextTreeNode: kTreeWalkMax nodes, at most kTreeClimbMax ancestor steps per
// climb, dev.41); any unreadable node or a longer climb (a transient parent cycle) ends it (fail
// closed: what was read stays).
std::vector<LiveObj> WalkLiveObjects(const EdenDsmodHostApi* host) {
    std::vector<LiveObj> out;
    const auto rd = [host](uint64_t a, void* o, size_t n) {
        return a != 0 && host->read_memory(host->userdata, a, o, n) != EDEN_DSMOD_FALSE;
    };
    uint64_t node = 0;
    if (!rd(host->main_base + kObjTree, &node, 8))
        return out;
    for (int n = 0; node != 0 && n < sshd::kTreeWalkMax; ++n) {
        uint64_t nd[5]{};
        if (!rd(node, nd, sizeof(nd)))
            break;
        uint8_t hdr[0x18]{};
        if (nd[4] != 0 && rd(nd[4], hdr, sizeof(hdr))) {
            uint16_t prof = 0;
            std::memcpy(&prof, hdr + 0x10, 2);
            if (prof == kProfDoorA || prof == kProfDoorB || prof == kProfDoor || prof == kProfStatue ||
                prof == kProfMapMark || prof == sshd::kProfCC || prof == sshd::kProfTBox || prof == kProfBeedle ||
                prof == kProfBugTag || prof == kProfBee || IsKikwi(prof)) {
                uint8_t a[0x180]{};
                if (rd(nd[4], a, sizeof(a))) {
                    uint32_t prop = 0;
                    std::memcpy(&prop, a + 0x15C, 4);
                    LiveObj o{};
                    o.prof = prof;
                    std::memcpy(&o.params, a + 0xC, 4);
                    std::memcpy(&o.x, a + 0x144, 4);
                    std::memcpy(&o.y, a + 0x148, 4);
                    std::memcpy(&o.z, a + 0x14C, 4);
                    std::memcpy(&o.yaw, a + 0x13E, 2);
                    o.room = static_cast<int8_t>(a[0x17C]);
                    bool ok = (prop & 0x100) == 0 && std::isfinite(o.x) && std::isfinite(o.z);
                    if (prof == kProfDoorA || prof == kProfDoorB || prof == kProfDoor || prof == sshd::kProfCC) {
                        // +0x700..+0x80F: partner +0x748, child +0x760, CD type / flag +0x7B9 / +0x7BA,
                        // pair door +0x7C0, CA flag / type +0x802 / +0x803; the vtable's methods
                        uint8_t b[0x110]{};
                        uint64_t vt = 0, m[2]{};
                        ok = ok && rd(nd[4] + 0x700, b, sizeof(b)) && rd(nd[4], &vt, 8) && rd(vt + 0xF8, &m[0], 8) &&
                             rd(vt + 0x100, &m[1], 8) && rd(nd[4] + 0x540, &o.p540, 8) && rd(nd[4] + 0x650, &o.c650, 8);
                        uint64_t partner = 0;
                        std::memcpy(&partner, b + 0x48, 8);
                        std::memcpy(&o.c760, b + 0x60, 8);
                        o.pair = partner != 0;
                        o.b7b9 = b[0xB9];
                        o.b7ba = b[0xBA];
                        o.b7c0 = b[0xC0];
                        o.b802 = b[0x102];
                        o.b803 = b[0x103];
                        o.m_lock = m[0] - host->main_base;
                        o.m_known = m[1] - host->main_base;
                    } else if (prof == sshd::kProfTBox) {
                        ok = ok && rd(nd[4] + 0x1994, &o.opened, 1);
                    } else if (prof == kProfStatue) {
                        uint64_t pm[2]{};
                        ok = ok && rd(nd[4] + 0x928, pm, sizeof(pm)) && pm[1] == 0;
                        o.pmf = pm[0] - host->main_base;
                    } else if (prof == kProfMapMark) {
                        ok = ok && rd(nd[4] + 0x190, o.mm, sizeof(o.mm));
                    } else if (prof == kProfBugTag) {
                        // dev.33 (Skyloft analysis C2c): +0x3D6 must be 0, count s32
                        // +0x3D0 >= 1, and a live insect: a pointer at +0x210 + 0x18 i (i < count) or
                        // +0x3D7 set with a non-zero byte at +0x3D8 + i
                        uint8_t t[0x200]{};
                        ok = ok && rd(nd[4] + 0x200, t, sizeof(t));
                        int32_t count = 0;
                        std::memcpy(&count, t + 0x1D0, 4);
                        bool live = false;
                        for (int i = 0; ok && i < count && i < 16 && !live; ++i) {
                            uint64_t ptr = 0;
                            std::memcpy(&ptr, t + 0x10 + 0x18 * i, 8);
                            live = ptr != 0 || (t[0x1D7] != 0 && t[0x1D8 + i] != 0);
                        }
                        o.opened = ok && t[0x1D6] == 0 && count >= 1 && live ? 1 : 0; // 1 = a bug location
                    } else if (IsKikwi(prof)) {
                        // the face callback (vtable +0x1D0) and its story id (+0x19FA)
                        uint64_t vt = 0, cb = 0;
                        ok = ok && rd(nd[4], &vt, 8) && rd(vt + 0x1D0, &cb, 8) && rd(nd[4] + 0x19FA, &o.fid, 2);
                        o.pmf = cb - host->main_base;
                    }
                    if (ok)
                        out.push_back(o);
                }
            }
        }
        uint64_t next = 0;
        if (!sshd::NextTreeNode(rd, node, nd[1], &next))
            return out; // unreadable, or a parent chain past kTreeClimbMax (a cycle): keep what was read
        node = next;
    }
    return out;
}

// A scene / temp / zone flag as SceneflagManager::checkFlag (main+0xB7F19C) reads it from the
// current file: flags 0..127 = the scene's 128 bits (slot+0x1A64 + 16 * scene), 128..191 = temp
// flags (slot+0x30F4), 192..254 = zone flags per room (slot+0x30FC, 64 per room), 255 = none.
bool SceneFlag(const EdenDsmodHostApi* host, int scene, int room, int flag, bool* value) {
    uint64_t a = 0;
    int bit = 0;
    if (flag >= 255 || flag < 0) {
        *value = false;
        return true;
    }
    if (flag >= 0xC0) {
        if (room < 0)
            return false;
        const int i = flag - 0xC0 + room * 64;
        a = kSlot + 0x30FC + 2 * (i >> 4), bit = i & 15;
    } else if (flag >= 0x80) {
        a = kSlot + 0x30F4 + 2 * ((flag - 0x80) >> 4), bit = (flag - 0x80) & 15;
    } else {
        if (scene < 0 || scene >= 26)
            return false;
        const int i = flag + scene * 128;
        a = kSlot + 0x1A64 + 2 * (i >> 4), bit = i & 15;
    }
    uint16_t w = 0;
    if (!host->read_memory(host->userdata, host->main_base + a, &w, 2))
        return false;
    *value = ((w >> bit) & 1) != 0;
    return true;
}

// ---------------------------------------------------------------- story flags (dev.31)
// The native story-flag getter's view (sshd_story.h, NPC map mark
// analysis): the table *(*(*(main+0x182E1F0)+0x18)+8) checked against the decoded anchors (read
// again only when its address changes) and the words of the file in play, once per sample. Every
// map condition with a story term reads it; an unreadable state (no manager, the special-scene
// alternate buffer, a table that is not the decoded one) answers nothing and the mark fails closed.
struct StoryCtx {
    const sshd::StoryTable* table = nullptr;
    sshd::StoryWords words;
    int why = 1; // StoryWordsOf: 0 ok; 4 = no / unknown table
    std::optional<uint32_t> Value(int id) const {
        return table != nullptr ? sshd::StoryValue(*table, words, id) : std::nullopt;
    }
    std::optional<bool> Flag(int id) const {
        const auto v = Value(id);
        return v ? std::optional<bool>(*v != 0) : std::nullopt;
    }
    // the reader MarkCondition takes
    bool operator()(int id, uint32_t* v) const {
        const auto r = Value(id);
        if (r)
            *v = *r;
        return r.has_value();
    }
};

StoryCtx ReadStory(Instance* inst, const EdenDsmodHostApi* host) {
    StoryCtx c;
    const auto rd = [host](uint64_t a, void* o, size_t n) {
        return a != 0 && host->read_memory(host->userdata, a, o, n) != EDEN_DSMOD_FALSE;
    };
    static sshd::StoryTable no_inst; // tests run without an instance
    sshd::StoryTable& t = inst != nullptr ? inst->story_table : no_inst;
    uint64_t& cached = inst != nullptr ? inst->story_addr : *[] { static uint64_t a = 0; return &a; }();
    const uint64_t addr = sshd::StoryTableAddr(rd, host->main_base);
    if (addr == 0) {
        c.why = 4;
        return c;
    }
    if (addr != cached || !t.valid) {
        std::vector<uint8_t> raw(sshd::kStoryTableBytes);
        t = rd(addr, raw.data(), raw.size()) ? sshd::ParseStoryTable(raw.data()) : sshd::StoryTable{};
        cached = addr;
    }
    if (!t.valid) {
        c.why = 4;
        return c;
    }
    c.table = &t;
    c.words = sshd::StoryWordsOf(rd, host->main_base, &c.why);
    return c;
}

// dev.41: the story words are read only on the samples that use them, not every
// sample: the live-object walk (every 8th sample, or at once on a new stage / shown floor / Map
// page, PublishLiveMarkers) and the Sea Chart latch (once per stage visit, PublishFieldMap).
// ReadStory probes the object tree (StoryWordsOf) and reads 510 bytes of words; one read serves
// the whole sample. sshd.story.why republishes the last result (diagnostics only).
struct StoryLazy {
    Instance* inst;
    const EdenDsmodHostApi* host;
    std::optional<StoryCtx> ctx;
    const StoryCtx& Get() {
        if (!ctx) {
            ctx = ReadStory(inst, host);
            if (inst != nullptr)
                inst->story_why = ctx->why;
        }
        return *ctx;
    }
    int Why() const { return ctx ? ctx->why : inst != nullptr ? inst->story_why : 1; }
};

// The picture axes of a stage's map (as PublishCone): u = -x or +x, v = z or -z.
struct MapAxes {
    bool ok, u_is_x, v_is_minus_z;
    double du = 0, dv = 0; // Sky Keep: the shown room's tile offset in the tile map's frame
    double s = 1;          // dev.34 generic frames: world units -> frame units (u = s x + du)
    int rot = 0;           // dev.34 generic frames: the MapArea yaw turn (objects are turned first)
};
MapAxes AxesOf(std::string_view st) {
    static constexpr std::pair<std::string_view, std::string_view> kAlias[] = {
        {"F402", "F401"}, {"D201_1", "D201"}, {"D300_1", "D300"}, {"D301_1", "D301"}};
    for (const auto& [from, to] : kAlias)
        if (st == from)
            st = to;
    if (st == "F020" || st == "F023") // dev.35: F023 shares the Sky's frame
        return {true, true, true};
    const sshd::AreaMap* a = sshd::FindArea(st);
    if (a != nullptr && a->gen) {
        // dev.34 GEN frame: u = s (mx - cx), v = -s (mz - cz) in the turned map coordinates
        const sshd::GenStage* g = sshd::FindGen(st);
        const sshd::GenFrame f = g != nullptr ? sshd::GenFrameOf(*g) : sshd::GenFrame{};
        MapAxes ax{g != nullptr, true, true, -f.s * f.cx, f.s * f.cz, f.s, a->rot};
        return ax;
    }
    return a != nullptr ? MapAxes{true, a->flip_h, a->flip_v} : MapAxes{false, false, false};
}
// dev.34: a world position / yaw turned into a generic frame's map coordinates (MapArea yaw).
void TurnXZ(int rot, float& x, float& z) {
    const float c = rot == 1 ? 0.0f : rot == 2 ? -1.0f : rot == 3 ? 0.0f : 1.0f;
    const float sn = rot == 1 ? 1.0f : rot == 3 ? -1.0f : 0.0f;
    const float mx = x * c - z * sn, mz = x * sn + z * c;
    x = mx, z = mz;
}

// ---------------------------------------------------------------- on-screen label fit (dev.28)
// Screen zones the pages draw over their map viewports (gen_manifest.py; test_manifest.py checks
// that every picture drawn over each viewport lies inside one of them). Map page: the area title
// plate (32, 26, its width per field area x 120), the compass and the Reset plank (dev.38; the Zoom
// plank's spot). Fly page: the title plate, the compass, the altitude gauge (with its bird), the
// three wings and the Beacons plank. Canvas px as x0, y0, x1, y1. The zones are screen-fixed: the
// plates are fitted to the live view (pan / pinch, dev.38) every sample (ViewOf: the host's view
// as last drawn), so a plate stays clear of them at any zoom.
// dev.33: the flying page's D-pad cluster (gen_manifest FLY_DPAD_BOX) joins the Fly page's zones;
// dev.38: the Fly page's Reset plank under the compass (gen_manifest FLY_RESET); dev.41: both Reset
// planks 216 x 128 (a 112 px tall tap rect), the Map page's 22 px higher.
// plate-avoid (begin)
constexpr sshd::Box kMapCompass{1065, 21, 1215, 171};
constexpr sshd::Box kMapReset{1004, 778, 1220, 906};
constexpr std::pair<std::string_view, int> kMapTitleW[] = {
    {"F401", 645}, {"F100", 565}, {"F200", 608}, {"F300", 645}, {"F301", 723}, {"F302", 629}, {"F303", 629}};
constexpr sshd::Box kFlyAvoid[] = {
    {32, 26, 512, 146}, {1065, 21, 1215, 171}, {40, 180, 160, 620}, {300, 790, 576, 882}, {40, 744, 286, 910},
    {930, 450, 1215, 902}, {1004, 190, 1220, 318}};
// plate-avoid (end)
// How far a fitted label may lie from its anchor (px; the native box touches it): a plate stays by
// its mark, an island name by its island (names only move outwards, never over the island art).
constexpr float kPlateGap = 120.0f, kIsleGap = 90.0f;
constexpr float kFitMargin = 2.0f; // px round each box: the host rounds the drawn position

// The Map page's plate copy of a field area ("F401_m"), or empty when the area has no plates.
std::string PlaceArea(std::string_view st) {
    if (st == "F402")
        st = "F401";
    for (const auto& [s, w] : kMapTitleW)
        if (st == s)
            return std::string(st) + "_m";
    return {};
}

// A map widget's view as last drawn, from the host ("@map_view_*:<area>", runtime 22+): invalid
// when that area is not on the current page (or no frame was drawn yet).
sshd::MapView ViewOf(const EdenDsmodHostApi* host, const std::string& area) {
    sshd::MapView v;
    if (host->get_f64 == nullptr || area.empty())
        return v;
    const double nan = std::nan("");
    const auto g = [&](const char* f) {
        const std::string k = "@map_view_" + std::string(f) + ":" + area;
        return host->get_f64(host->userdata, k.c_str(), nan);
    };
    v.cx = g("cx"), v.cy = g("cy"), v.ppw = g("ppw"), v.x = g("x"), v.y = g("y"), v.w = g("w"), v.h = g("h");
    const double rot = g("rot");
    if (!std::isfinite(v.ppw) || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.w) ||
        !std::isfinite(v.h) || (std::isfinite(rot) && std::fabs(rot) > 0.01))
        v.ppw = 0; // a turned view is not handled: fail closed
    return v;
}

// ---------------------------------------------------------------- dungeons (dev.30)
// Reader contract: dungeon marker analysis. The dungeon's flags row
// (slot+0xA64 + 16 * scene index: u16[0] bit 0x02 = the Dungeon Map, u16[7] = visited rooms),
// the native floor resolver's live MapArea table (sshd_dungeon.h) and Link's floor / room.
struct DungeonStage {
    const char* stage;
    int index;
};
constexpr DungeonStage kDungeonStages[] = {
    {"D100", 11},   {"D101", 12},   {"D200", 14},   {"D201", 15},   {"D201_1", 15}, {"D300", 17},
    {"D300_1", 17}, {"D301", 18},   {"D301_1", 18}, {"D003_0", 20}, {"D003_1", 20}, {"D003_2", 20},
    {"D003_3", 20}, {"D003_4", 20}, {"D003_5", 20}, {"D003_6", 20}, {"D003_7", 20}, {"D003_8", 20}};
// Link's object (the player is the static object main+0x623E680: position +0x144, yaw +0x13E,
// room s8 +0x17C)
constexpr uint64_t kLinkY = 0x623E7C8, kLinkRoom = 0x623E7FC;
constexpr uint32_t kSkyKeepCells = 0x539C; // file +0x539C: Sky Keep's 3 x 3 arrangement (dev.31)
// Sky Keep tile offset of a cell in the tile map's frame (sshd_art.h kSkyKeep*: u = x + (col - 1)
// pitch, v = -z - (row - 1) pitch)
std::array<double, 2> SkyKeepOffset(int cell) {
    return {(cell % 3 - 1) * static_cast<double>(sshd::kSkyKeepPitch), -(cell / 3 - 1) * static_cast<double>(sshd::kSkyKeepPitch)};
}
constexpr int kFloorTabs = 5;

struct DungeonCtx {
    bool dungeon = false;     // a dungeon stage of kDungeonStages
    std::string stage, base;  // live stage name; the stage whose picture it draws (D201_1 -> D201)
    int index = -1;           // dungeon flags / scene index
    bool flags_ok = false;    // the flags row was read
    bool has_map = false;     // the Dungeon Map
    uint16_t visited = 0;     // visited rooms
    sshd::FloorTable ft;      // the native floor table (valid or not)
    std::vector<int> tabs;    // floor tabs top to bottom (empty: one floor)
    int link_floor = 0;       // Link's native floor
    int floor = 0;            // the floor shown (a pick on the Map page, else Link's)
    bool picked = false;      // the player picked `floor`
    bool d201 = false;
    // dev.35: a generic stage (sshd_stages.inc) with two or more native floors where Link stands
    // on an active one (the Knight Academy 1F / 2F, Inside the Great Tree B1 / 1F / 2F): the
    // native map's floor tabs, a pick shows that floor's band (Map page "GEN_m")
    bool gen = false;
    // Sky Keep (dev.31): the live tile arrangement (file +0x539C: 9 cells, row-major from the top
    // left, the D003_<n> room in each or 0xFF for the empty one), Link's room and its cell
    int sk_cells[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int sk_room = -1, sk_cell = -1;
    bool sk_ok = false; // the arrangement read and valid (8 distinct rooms 0..7 and one empty cell)
    // the object's native floor equals the shown floor (no tabs: every object)
    bool OnFloor(float y, int room) const {
        return tabs.empty() || (ft.valid && sshd::ResolveFloor(ft, y, room, d201) == floor);
    }
};

// The Map page's floor pick (dev.30 dungeons, dev.35 generic stages): the page draws the stage as
// `view_area`; while its view is absent (another page) for a few samples, the pick goes back to
// following Link; a new stage drops it too.
void ApplyFloorPick(Instance* inst, const EdenDsmodHostApi* host, DungeonCtx& c, const char* st, const std::string& view_area) {
    c.floor = c.link_floor;
    if (inst == nullptr)
        return;
    const bool open = host->get_f64 != nullptr && ViewOf(host, view_area).Valid();
    inst->map_closed = open ? 0 : inst->map_closed + 1;
    if (std::strcmp(inst->floor_stage, st) != 0 || inst->map_closed > 10) {
        std::snprintf(inst->floor_stage, sizeof(inst->floor_stage), "%s", st);
        inst->floor_pick.store(kFloorAuto);
    }
    int pick = inst->floor_pick.load();
    if (pick >= kFloorSlotPick) {
        // a tapped tab (OnAction stores its slot): from now on the floor that tab showed
        const int slot = pick - kFloorSlotPick;
        const int f = slot < static_cast<int>(c.tabs.size()) ? c.tabs[slot] : kFloorAuto;
        inst->floor_pick.compare_exchange_strong(pick, f);
        pick = inst->floor_pick.load();
    }
    if (pick != kFloorAuto && pick < kFloorSlotPick) {
        if (std::find(c.tabs.begin(), c.tabs.end(), pick) != c.tabs.end()) {
            c.floor = pick;
            c.picked = true;
        } else {
            inst->floor_pick.compare_exchange_strong(pick, kFloorAuto);
        }
    }
}

// dev.35: a generic stage's native floors (the live MapArea table): tabs only with two or more
// floors and Link on an active one (else one picture, as dev.34), on a stage in play.
void ReadGenFloors(Instance* inst, const EdenDsmodHostApi* host, DungeonCtx& c, const char* st) {
    uint32_t scene = 0;
    uint64_t mgr = 0;
    uint8_t raw[sshd::kFloorBytes]{};
    float y = 0;
    int8_t room = -1;
    if (sshd::FindGen(st) == nullptr ||
        !host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, 4) || scene == 0x53656C74 ||
        !host->read_memory(host->userdata, host->main_base + sshd::kFloorMgr, &mgr, 8) || mgr == 0 ||
        !host->read_memory(host->userdata, mgr + sshd::kFloorData, raw, sizeof(raw)) ||
        !host->read_memory(host->userdata, host->main_base + kLinkY, &y, 4) ||
        !host->read_memory(host->userdata, host->main_base + kLinkRoom, &room, 1) || !std::isfinite(y))
        return;
    c.ft = sshd::ParseFloorTable(raw);
    const int f = c.ft.valid ? sshd::ResolveFloor(c.ft, y, room, false) : 0;
    if (!c.ft.valid || c.ft.Count() < 2 || f < sshd::kFloorMin || f > sshd::kFloorMax || !c.ft.active[f - sshd::kFloorMin])
        return;
    c.gen = true;
    c.tabs = sshd::TabFloors(c.ft);
    c.link_floor = f;
    ApplyFloorPick(inst, host, c, st, "GEN_m");
}

// The height band [lo, hi] of native floor f (a generic stage's picture / markers of that floor).
std::optional<std::pair<int, int>> FloorBand(const sshd::FloorTable& t, int f) {
    const int i = f - sshd::kFloorMin;
    if (!t.valid || i < 0 || i >= 8 || !t.active[i])
        return std::nullopt;
    return std::pair<int, int>{static_cast<int>(std::floor(t.min[i])), static_cast<int>(std::ceil(t.min[i] + t.height[i]))};
}

DungeonCtx ReadDungeon(Instance* inst, const EdenDsmodHostApi* host) {
    DungeonCtx c;
    char st[9]{};
    const uint64_t slot = host->main_base + kSlot;
    if (!host->read_memory(host->userdata, slot + 0x531C, st, 8))
        return c;
    c.stage = st;
    for (const auto& d : kDungeonStages)
        if (c.stage == d.stage)
            c.index = d.index;
    if (c.index < 0) {
        ReadGenFloors(inst, host, c, st);
        return c;
    }
    c.dungeon = true;
    c.base = c.stage;
    if (c.base.size() > 4 && c.base[4] == '_')
        c.base.resize(4); // D201_1 etc. use the main stage's rooms; D003_<n>: Sky Keep's tile map
    if (c.base == "D003") {
        c.sk_room = c.stage.size() == 6 ? c.stage[5] - '0' : -1;
        uint8_t raw[9]{};
        if (c.sk_room >= 0 && c.sk_room <= 8 && host->read_memory(host->userdata, slot + kSkyKeepCells, raw, 9)) {
            int seen = 0, empty = 0;
            for (int i = 0; i < 9; ++i) {
                if (raw[i] == 0xFF) {
                    ++empty;
                } else if (raw[i] <= 7 && (seen & (1 << raw[i])) == 0) {
                    seen |= 1 << raw[i];
                    c.sk_cells[i] = raw[i];
                    if (raw[i] == c.sk_room)
                        c.sk_cell = i;
                }
            }
            c.sk_ok = seen == 0xFF && empty == 1;
        }
        if (!c.sk_ok) {
            // unreadable / not the decoded layout: Link's room alone in the centre tile
            std::fill(std::begin(c.sk_cells), std::end(c.sk_cells), -1);
            c.sk_cells[4] = c.sk_room;
            c.sk_cell = 4;
        } else if (c.sk_cell < 0) {
            c.sk_cell = -1; // a room off the grid (D003_8): no tile
        }
    }
    c.d201 = c.base == "D201";
    uint16_t fl[8]{};
    c.flags_ok = host->read_memory(host->userdata, slot + 0xA64 + 16u * c.index, fl, sizeof(fl)) != EDEN_DSMOD_FALSE;
    c.has_map = c.flags_ok && (fl[0] & 0x02) != 0;
    c.visited = c.flags_ok ? fl[7] : 0;
    uint64_t mgr = 0;
    uint8_t raw[sshd::kFloorBytes]{};
    if (host->read_memory(host->userdata, host->main_base + sshd::kFloorMgr, &mgr, 8) && mgr != 0 &&
        host->read_memory(host->userdata, mgr + sshd::kFloorData, raw, sizeof(raw)))
        c.ft = sshd::ParseFloorTable(raw);
    float y = 0;
    int8_t room = -1;
    const bool link = host->read_memory(host->userdata, host->main_base + kLinkY, &y, 4) &&
                      host->read_memory(host->userdata, host->main_base + kLinkRoom, &room, 1) && std::isfinite(y);
    if (c.ft.valid && link) {
        c.tabs = sshd::TabFloors(c.ft);
        c.link_floor = sshd::ResolveFloor(c.ft, y, room, c.d201);
    }
    ApplyFloorPick(inst, host, c, st, c.base + "_m");
    return c;
}

// Native Minus-map floor layouts of the companion's room pictures (art builder), by tab position
// (1 = the top tab): the rooms each tab draws (the art clips Sanctuary r06 / Sandship's shared
// rooms to the tab's height band itself).
struct FloorRooms {
    const char* stage;
    int count;
    uint32_t rooms[4];
};
constexpr FloorRooms kFloorRooms[] = {
    {"D101", 2, {~((1u << 4) | (1u << 7)), (1u << 4) | (1u << 7)}},
    {"D201", 2, {~(1u << 9), (1u << 6) | (1u << 9)}},
    {"D301", 4, {0xE003u, 0x01FFu, 0x0E11u, 0x1000u}},
};

uint32_t DungeonLitRooms(uint16_t visited) {
    return 0xFFFF0000u | visited;
}

// Field map picture state (dev.32, map colour analysis): MapRelated draws the
// past variant (the time-shift backgrounds kcl/room_obj<n>k, Lanayru's green past) in F300 /
// F300_4 while the scene / temp flag 3 is set (S300 is always past, drawn so by the art), and
// the Sealed Grounds by the temple (F402) has its own geometry in the F401 frame. Empty = the
// area's own picture.
std::string FieldMapKey(std::string_view stage, bool past, int layer = -1) {
    // dev.33 (static map analysis C3): in F402's layer 7 the native draws
    // the world-space F406Col background over the pit (Art::Map state kMapF406)
    if (stage == "F402")
        return layer == 7 ? "module:sshd:map/F402/4294967295/0/4294967295/2" : "module:sshd:map/F402";
    if (stage == "F300" && past)
        return "module:sshd:map/F300/4294967295/0/4294967295/1";
    // dev.33 (C5): the sailing Sand Sea's sea-chart state = its past background (room_obj0k)
    if (stage == "F301_1" && past)
        return "module:sshd:map/F301_1/4294967295/0/4294967295/1";
    return {};
}

void PublishFieldMap(Instance* inst, const EdenDsmodHostApi* host, StoryLazy& story) {
    char st[9]{};
    std::string key;
    if (host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, st, 8)) {
        bool past = false;
        // the loaded layer (dev.33): s8 main+0x2BF98FB, the current spawn info's layer
        int8_t layer = -1;
        if (!host->read_memory(host->userdata, host->main_base + 0x2BF98FB, &layer, 1))
            layer = -1;
        if (inst != nullptr && std::strcmp(inst->fmap_stage, st) == 0 && inst->fmap_past >= 0) {
            past = inst->fmap_past != 0; // latched for this stage visit
        } else if (std::string_view(st) == "F301_1") {
            // dev.33 (static map analysis C5, main+0xE05F2C): story 519 (the Sea Chart activated), read
            // once at stage load like MapRelated; unreadable = not latched, read again
            const auto chart = story.Get().Flag(519);
            if (chart)
                past = *chart;
            else
                st[0] = '\0';
        } else if (std::string_view(st) == "F300") {
            // the current scene (manager *(main+0x182DEF8) u16 +0x4A) and its flag 3; unreadable =
            // the present picture
            uint64_t mgr = 0;
            uint16_t idx = 0;
            bool v = false;
            if (host->read_memory(host->userdata, host->main_base + 0x182DEF8, &mgr, 8) && mgr != 0 &&
                host->read_memory(host->userdata, mgr + 0x4A, &idx, 2) && SceneFlag(host, idx & 0x1FFF, -1, 3, &v))
                past = v;
            else
                st[0] = '\0'; // unreadable: not latched, read again next sample
        }
        if (inst != nullptr && st[0] != '\0' && std::strcmp(inst->fmap_stage, st) != 0) {
            std::snprintf(inst->fmap_stage, sizeof(inst->fmap_stage), "%s", st);
            inst->fmap_past = past ? 1 : 0;
        }
        key = FieldMapKey(st, past, layer);
        host->publish_i64(host->userdata, "sshd.stage.layer", layer);
    }
    host->publish_text(host->userdata, "sshd.fmap.src", key.c_str());
}

// dev.34 generic sub-stage map (the stage survey): every stage without
// a dedicated area (sshd_stages.inc) draws its own map faces in the manifest's one square GEN frame,
// like the native map draws the current stage: sshd.gen.on, the picture (sshd.gen.src; F300_4 its
// past variant by scene flag 3 like F300, MapRelated), Link in the frame (sshd.gen.lu / lv), the
// native STIF map name as the Ready title (MAP_<id>_S, one line) and the Map page plate (MAP_<id>
// with its line breaks) in the STIF area's plate colour. A stage with no drawn faces (the cutscene
// stages Demo / F021 / F405, the sky arenas B301 / B400, F023) shows the paper with Link.
constexpr int kGenTitleW = 760, kGenPlateW = 680, kGenPlateTextW = 520; // gen_manifest.py GEN_TITLE_W / GEN_PLATE_W
void PublishGenMap(const EdenDsmodHostApi* host, const DungeonCtx& dc) {
    char st[9]{};
    float pos[3]{};
    uint32_t scene = 0;
    const sshd::GenStage* g = nullptr;
    if (host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, st, 8) &&
        host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene, 4) && scene != 0x53656C74 &&
        host->read_memory(host->userdata, host->main_base + 0x623E7C4, pos, sizeof(pos)) && std::isfinite(pos[0]) &&
        std::isfinite(pos[2]))
        g = sshd::FindGen(st);
    host->publish_i64(host->userdata, "sshd.gen.on", g != nullptr ? 1 : 0);
    if (g == nullptr)
        return;
    const sshd::GenFrame f = sshd::GenFrameOf(*g);
    float x = pos[0], z = pos[2];
    TurnXZ(g->rot, x, z);
    host->publish_f64(host->userdata, "sshd.gen.lu", f.s * (x - f.cx));
    host->publish_f64(host->userdata, "sshd.gen.lv", -f.s * (z - f.cz));
    bool past = false;
    if (std::string_view(st) == "F300_4") {
        uint64_t mgr = 0;
        uint16_t idx = 0;
        bool v = false;
        if (host->read_memory(host->userdata, host->main_base + 0x182DEF8, &mgr, 8) && mgr != 0 &&
            host->read_memory(host->userdata, mgr + 0x4A, &idx, 2) && SceneFlag(host, idx & 0x1FFF, -1, 3, &v))
            past = v;
    }
    // dev.35: a stage whose layer archive registers map water (sshd::LayerActors: Inside the Great
    // Tree's pool) draws the loaded layer's (s8 main+0x2BF98FB, the current spawn info's layer)
    int state = past ? sshd::kMapPast : 0;
    int8_t layer = -1;
    if (sshd::LayerActors(st) && host->read_memory(host->userdata, host->main_base + 0x2BF98FB, &layer, 1) &&
        layer >= 0 && layer < 30)
        state |= (layer + 1) << sshd::kMapLayerShift;
    char key[96];
    if (state != 0)
        std::snprintf(key, sizeof(key), "module:sshd:map/%s/4294967295/0/4294967295/%d", st, state);
    else
        std::snprintf(key, sizeof(key), "module:sshd:map/%s", st);
    // a stage with two or more native floors (the live MapArea table, sshd_dungeon.h; e.g. the
    // Knight Academy 1F / 2F): one floor at a time as the native map, Link's or the picked tab's
    if (const auto band = dc.gen ? FloorBand(dc.ft, dc.floor) : std::nullopt)
        std::snprintf(key, sizeof(key), "module:sshd:gmapf/%s/%d/%d/%d", st, band->first, band->second, state);
    host->publish_text(host->userdata, "sshd.gen.src", key);
    if (g->name_id >= 0)
        std::snprintf(key, sizeof(key), "module:sshd:ctitle/title/34/%d/67/MAP_%02d_S", kGenTitleW, g->name_id);
    else
        std::snprintf(key, sizeof(key), "module:sshd:ctitle/title/34/%d/67/-", kGenTitleW);
    host->publish_text(host->userdata, "sshd.gen.title", key);
    if (g->name_id >= 0)
        std::snprintf(key, sizeof(key), "module:sshd:ctitle/plate/34/%d/86/MAP_%02d", kGenPlateTextW, g->name_id);
    else
        std::snprintf(key, sizeof(key), "module:sshd:ctitle/plate/34/%d/86/-", kGenPlateTextW);
    host->publish_text(host->userdata, "sshd.gen.ptext", key);
    std::snprintf(key, sizeof(key), "module:sshd:areaplate/%d/120/%d", kGenPlateW, g->region);
    host->publish_text(host->userdata, "sshd.gen.plate", key);
    host->publish_i64(host->userdata, "sshd.gen.titled", g->name_id >= 0 ? 1 : 0);
}

// Dungeon map picture: the full layout once the Dungeon Map is owned, else only the visited rooms,
// like the native map (the Settings override "Dungeon layout before the Map" forces the full
// layout); the shown floor's rooms on a floor-tab dungeon. Floor tabs (Map page) as the native
// MapFloorBtn: one per active floor, top to bottom, the shown one lit, Link's marked.
void PublishDungeonMap(const EdenDsmodHostApi* host, const DungeonCtx& c) {
    static constexpr const char* kTab[kFloorTabs][3] = {
        {"sshd.ft0.on", "sshd.ft0.src", "sshd.ft0.here"}, {"sshd.ft1.on", "sshd.ft1.src", "sshd.ft1.here"},
        {"sshd.ft2.on", "sshd.ft2.src", "sshd.ft2.here"}, {"sshd.ft3.on", "sshd.ft3.src", "sshd.ft3.here"},
        {"sshd.ft4.on", "sshd.ft4.src", "sshd.ft4.here"}};
    const int ntabs = std::min<int>(static_cast<int>(c.tabs.size()), kFloorTabs);
    const auto pos = [&](int f) {
        for (int i = 0; i < ntabs; ++i)
            if (c.tabs[i] == f)
                return i + 1;
        return 0;
    };
    // dev.35: a generic multi-floor stage (dc.gen) has the same tabs, without a dungeon picture
    const bool ok = (c.dungeon && c.flags_ok) || c.gen;
    const int shown = ok ? pos(c.floor) : 0, here = ok ? pos(c.link_floor) : 0;
    host->publish_i64(host->userdata, "sshd.floor", shown);
    host->publish_i64(host->userdata, "sshd.floor.cur", here);
    host->publish_i64(host->userdata, "sshd.floor.count", ok ? ntabs : 0);
    // Link (and his view cone) on the Map page only on his own floor
    host->publish_i64(host->userdata, "sshd.lk.here", !ok || c.floor == c.link_floor ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.lk.away", ok && c.floor != c.link_floor ? 1 : 0);
    for (int i = 0; i < kFloorTabs; ++i) {
        const bool on = ok && i < ntabs;
        host->publish_i64(host->userdata, kTab[i][0], on ? 1 : 0);
        host->publish_i64(host->userdata, kTab[i][2], on && c.tabs[i] == c.link_floor ? 1 : 0);
        if (on) {
            char src[48];
            std::snprintf(src, sizeof(src), "module:sshd:floortab/%d/%d", c.tabs[i], c.tabs[i] == c.floor ? 1 : 0);
            host->publish_text(host->userdata, kTab[i][1], src);
        }
    }
    if (!ok || c.gen) {
        host->publish_text(host->userdata, "sshd.dmap.src", "");
        host->publish_i64(host->userdata, "sshd.dmap.full", 0);
        return;
    }
    const bool full = c.has_map ||
                      (host->get_i64 != nullptr && host->get_i64(host->userdata, "@flag:dmap.layout", 0) != 0);
    if (c.base == "D003") {
        // Sky Keep: the tile map (art skykeep/<cells>/<Link's room>/<all>), every room's tile:
        // the native map's dedicated Sky Keep routine draws all eight map-dummy tiles with or
        // without the Dungeon Map (live dev.32 hero D003_0, Map bit and visits cleared); Link and
        // his cone in the tile map's frame (sshd.sk.lu / lv), hidden off the grid
        char cells[10]{};
        for (int i = 0; i < 9; ++i)
            cells[i] = c.sk_cells[i] >= 0 ? static_cast<char>('0' + c.sk_cells[i]) : '-';
        char key[64];
        std::snprintf(key, sizeof(key), "module:sshd:skykeep/%s/%d/1", cells, c.sk_room);
        host->publish_text(host->userdata, "sshd.dmap.src", key);
        host->publish_i64(host->userdata, "sshd.dmap.full", full ? 1 : 0);
        float p[3]{};
        const bool link = c.sk_cell >= 0 &&
                          host->read_memory(host->userdata, host->main_base + kLinkY - 4, p, sizeof(p)) &&
                          std::isfinite(p[0]) && std::isfinite(p[2]);
        const auto off = SkyKeepOffset(std::max(0, c.sk_cell));
        host->publish_i64(host->userdata, "sshd.sk.ok", c.sk_ok ? 1 : 0);
        host->publish_i64(host->userdata, "sshd.sk.cell", c.sk_cell);
        host->publish_i64(host->userdata, "sshd.sk.on", link ? 1 : 0);
        host->publish_f64(host->userdata, "sshd.sk.lu", link ? p[0] + off[0] : 0.0);
        host->publish_f64(host->userdata, "sshd.sk.lv", link ? -p[2] + off[1] : 0.0);
        return;
    }
    uint32_t mask = full ? 0xFFFFFFFFu : c.visited;
    int layout = 0;
    for (const auto& f : kFloorRooms)
        if (c.base == f.stage && f.count == ntabs && shown >= 1) {
            mask &= f.rooms[shown - 1];
            layout = shown;
        }
    const int floor_arg = (c.base == "D201" || c.base == "D301") && layout > 0 ? layout : 0;
    // dev.32: with the Map (or the layout setting), rooms not yet visited take the native
    // unvisited shade (dRoom_c::drawOnMapIfVisible: drawn, not filled); only 16 visit bits exist,
    // so rooms 16+ count as visited
    const uint32_t lit = DungeonLitRooms(c.visited);
    std::string key = "module:sshd:map/" + c.base;
    if ((mask & ~lit) != 0)
        key += "/" + std::to_string(mask) + "/" + std::to_string(floor_arg) + "/" + std::to_string(lit);
    else
        key += (mask == 0xFFFFFFFFu ? std::string() : "/" + std::to_string(mask)) +
               (floor_arg > 0 ? "/" + std::to_string(floor_arg) : std::string());
    host->publish_text(host->userdata, "sshd.dmap.src", key.c_str());
    host->publish_i64(host->userdata, "sshd.dmap.full", full ? 1 : 0);
}

// Measures a label once from the art (the asset worker holds art_mutex while it builds: try only;
// a failed measure is retried after 600 samples, as a romfs may open late).
template <typename F>
bool LabelReady(Instance* inst, Instance::LabelSize& m, F&& measure) {
    if (m.state == 1)
        return true;
    if (inst == nullptr || inst->art == nullptr || (m.state == -1 && --m.retry > 0))
        return false;
    std::unique_lock lock(inst->art_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    if (measure(*inst->art, m)) {
        m.state = 1;
        return true;
    }
    m.state = -1, m.retry = 600;
    return false;
}

// Fits one label for a view: its box (w x h, centred `off` from its world anchor (u, v)) must lie
// fully inside the viewport and clear of `avoid` (FitLabel). Returns the anchor to publish so the
// unchanged art lands at the fitted spot, or nullopt to hide the label.
std::optional<std::array<double, 2>> FitAnchor(const sshd::MapView& view, double u, double v, std::array<float, 2> off,
                                               float w, float h, std::span<const sshd::Box> avoid, float max_gap,
                                               bool outward_only) {
    const auto a = view.ToScreen(u, v);
    const std::array<float, 2> native{a[0] + off[0], a[1] + off[1]};
    const float len = std::hypot(off[0], off[1]);
    const auto fit = sshd::FitLabel(a, native, w + 2 * kFitMargin, h + 2 * kFitMargin, view.Rect(), avoid, max_gap,
                                    outward_only ? len : 0.0f);
    if (!fit)
        return std::nullopt;
    return view.ToWorld(a[0] + (*fit)[0] - native[0], a[1] + (*fit)[1] - native[1]);
}

// The neighbour plates of the last walk, placed for the Map page's current view (every sample:
// the view follows Link). Hidden while the Map page's view is unknown or the plate cannot fit.
void PublishPlates(Instance* inst, const EdenDsmodHostApi* host, const std::vector<LivePub::Plate>& plates,
                   std::string_view st) {
    static constexpr const char* kPl[kPlaceSlots][4] = {
        {"sshd.pl0.on", "sshd.pl0.u", "sshd.pl0.v", "sshd.pl0.src"}, {"sshd.pl1.on", "sshd.pl1.u", "sshd.pl1.v", "sshd.pl1.src"},
        {"sshd.pl2.on", "sshd.pl2.u", "sshd.pl2.v", "sshd.pl2.src"}, {"sshd.pl3.on", "sshd.pl3.u", "sshd.pl3.v", "sshd.pl3.src"}};
    const std::string area = PlaceArea(st);
    const sshd::MapView view = ViewOf(host, area);
    // dev.41: the same plates, view and label sizes as the last sample: its values again
    const sshd::MapView kv = view.Valid() ? view : sshd::MapView{}; // NaN never compares equal
    std::vector<double> key{kv.cx, kv.cy, kv.ppw, kv.x, kv.y, kv.w, kv.h};
    for (const char c : area)
        key.push_back(c);
    bool sizes_known = true;
    for (const auto& p : plates) {
        key.insert(key.end(), {static_cast<double>(p.shown), static_cast<double>(p.deg), static_cast<double>(p.region), p.u, p.v});
        if (inst != nullptr && p.shown >= 0 && p.shown <= 12 && inst->place_size[p.shown].state != 1)
            sizes_known = false;
    }
    PubCache* cache = inst != nullptr ? &inst->plate_cache : nullptr;
    if (cache != nullptr && cache->Hit(key)) {
        cache->pub.Publish(host);
        return;
    }
    LivePub out;
    std::vector<sshd::Box> avoid{kMapCompass, kMapReset};
    for (const auto& [s, w] : kMapTitleW)
        if (area.size() > 2 && std::string_view(area).substr(0, area.size() - 2) == s)
            avoid.push_back({32, 26, static_cast<float>(32 + w), 146});
    for (int i = 0; i < kPlaceSlots; ++i) {
        if (i >= static_cast<int>(plates.size())) {
            out.i(kPl[i][0], 0);
            continue;
        }
        const auto& p = plates[i];
        char src[48];
        std::snprintf(src, sizeof(src), "module:sshd:place/%d/%d/%d", p.shown, p.deg, p.region);
        out.t(kPl[i][3], src);
        std::optional<std::array<double, 2>> at;
        if (view.Valid() && p.shown >= 0 && p.shown <= 12 && inst != nullptr &&
            LabelReady(inst, inst->place_size[p.shown], [&](sshd::Art& art, Instance::LabelSize& m) {
                const auto ps = art.PlaceMetrics(p.shown);
                if (ps)
                    m.place = *ps;
                return ps.has_value();
            })) {
            const sshd::PlaceSize& ps = inst->place_size[p.shown].place;
            const auto off = sshd::PlaceOffset(p.deg, static_cast<float>(ps.tw), static_cast<float>(ps.bw),
                                               static_cast<float>(ps.bh));
            at = FitAnchor(view, p.u, p.v, off, static_cast<float>(ps.bw), static_cast<float>(ps.bh), avoid,
                           kPlateGap, false);
        }
        out.i(kPl[i][0], at ? 1 : 0);
        out.f(kPl[i][1], at ? (*at)[0] : p.u);
        out.f(kPl[i][2], at ? (*at)[1] : p.v);
    }
    out.Publish(host);
    if (cache != nullptr) {
        // a label size still being measured (or retried) is computed again next sample
        cache->valid = sizes_known || !view.Valid();
        cache->key = std::move(key);
        cache->pub = std::move(out);
    }
}

// Numbered marker keys ("sshd.<group><i>.<field>").
struct SlotKey {
    char k[32];
    SlotKey(const char* group, int i, const char* field) { std::snprintf(k, sizeof(k), "sshd.%s%d.%s", group, i, field); }
    operator const char*() const { return k; }
};

// One marker in slot i of a group: on, u, v and (when given) its picture.
void PubMarker(LivePub& pub, const char* group, int i, double u, double v, const char* src = nullptr) {
    pub.i(SlotKey(group, i, "on"), 1);
    pub.f(SlotKey(group, i, "u"), u);
    pub.f(SlotKey(group, i, "v"), v);
    if (src != nullptr)
        pub.t(SlotKey(group, i, "src"), src);
}
void PubOff(LivePub& pub, const char* group, int from, int count) {
    for (int i = from; i < count; ++i)
        pub.i(SlotKey(group, i, "on"), 0);
}

// The dungeon's current scene index as the native scene flags use it (manager *(main+0x182DEF8),
// u16 +0x4A); -1 unless it is the dungeon's own index (fail closed for flag-gated markers).
int DungeonScene(const EdenDsmodHostApi* host, const DungeonCtx& c) {
    uint64_t mgr = 0;
    uint16_t idx = 0;
    if (!c.dungeon || !host->read_memory(host->userdata, host->main_base + 0x182DEF8, &mgr, 8) || mgr == 0 ||
        !host->read_memory(host->userdata, mgr + 0x4A, &idx, 2))
        return -1;
    return (idx & 0x1FFF) == c.index ? c.index : -1;
}

// A dungeon clear flag (sshd::ClearStory / BossStory ids) through the native story getter (dev.31;
// was the mirror main+0x182E1F8); nullopt when the id is unknown or the state unreadable.
std::optional<bool> StoryId(const StoryCtx& sc, int story) {
    return story >= 0 ? sc.Flag(story) : std::nullopt;
}

// Dungeon markers (dev.30, dungeon marker analysis): door bars per profile and
// known-room method, small-key locks, the boss door (first eligible 0xCC, or Sandship's MapMark
// boss-door substitute) with its key and objective, chests, the red X of type-0 map marks. Every
// object must be live, not hidden (property 0x100) and on the shown floor.
// Sky Keep's other tiles (dev.33, static map analysis C6): with the Dungeon
// Map the native board draws every other tile's icons from the map dummy's dat/d3icon.bil (each
// D003_<n> stage carries the same records; consumer main+0xC6E57C loop 0xC6E780), skipping the
// loaded tile's own (drawn live): ShtCS 0xCB / TstShtr 0xCA door bars (a key over a TstShtr with
// b1 0x42 while its scene flag is clear), statues (yellow), the red X of map marks, chests (opened
// art once their scene flag is set). Triforce pieces and InHole are not drawn here. Positions are the
// tile room's own coordinates moved into the tile's cell (u = x + (col - 1) 7000, v = -z - (row - 1)
// 7000), yaw in degrees.
struct SkyKeepIcon {
    int8_t tile;
    char kind; // 'd' door bar, 's' statue, 'x' red X, 'c' chest, 't' Triforce piece (dev.34)
    int16_t x, z, yaw;
    uint8_t b1, flag;
};
constexpr SkyKeepIcon kSkyKeepIcons[] = {
    {0, 'd', 3150, 0, -90, 64, 255}, {0, 'd', 0, 3150, 180, 64, 1},     {0, 'd', 1624, 899, 0, 66, 63},
    {0, 'd', -2090, -100, 180, 64, 11}, {0, 'd', -355, 1681, -90, 64, 12}, {0, 'd', 690, -2412, 90, 64, 10},
    {1, 'd', 0, -3150, 0, 64, 2},       {1, 'd', 3150, 0, -90, 64, 3},    {1, 's', 2639, 603, -90, 2, 72},
    {1, 'x', 2500, -2700, 0, 0, 255},   {2, 'd', -3150, 0, 90, 64, 5},    {2, 'd', 3150, 0, -90, 64, 4},
    {2, 's', 2480, -760, 0, 2, 73},     {3, 'd', -3150, 0, 90, 64, 6},    {3, 's', -2157, -552, 0, 2, 74},
    {4, 'd', 0, -3150, 0, 64, 7},       {4, 'd', 0, 3150, 180, 64, 8},    {4, 's', 475, 2249, 0, 2, 75},
    {4, 'x', -750, -2900, 0, 0, 255},   {5, 'd', -3150, 0, 90, 64, 10},   {5, 'd', 0, -3150, 0, 64, 9},
    {5, 's', -677, -2867, 0, 2, 76},    {6, 'd', 0, 3150, 180, 64, 11},   {6, 'd', -3150, 0, 90, 64, 12},
    {6, 'c', 811, -2481, -90, 1, 95},   {6, 'x', -900, -2650, 0, 0, 255}, {6, 'd', -600, -2230, 90, 64, 25},
    {7, 'd', 0, 3150, 180, 64, 14},     {7, 'd', 3150, 0, -90, 64, 13},   {7, 'c', 6, -1, 0, 50, 21},
    {7, 's', -621, 2150, 90, 2, 78},    {7, 'x', 2600, -300, 0, 0, 255},
    // dev.34 (item 9): the three Triforce pieces, d3icon "Item" records (b1 = item 96 Power / 95
    // Courage / 97 Wisdom); the native draws one (type 0x17 sub 4) while its scene flag is clear
    {0, 't', 255, 1681, 90, 96, 64},    {2, 't', -2596, -1209, 0, 95, 61}, {3, 't', 2502, -28, -90, 97, 62},
};
// the cell of D003_<tile> in the live arrangement, -1 when absent
int SkyKeepCell(const DungeonCtx& c, int tile) {
    for (int i = 0; i < 9; ++i)
        if (c.sk_cells[i] == tile)
            return i;
    return -1;
}
bool SkyKeepBoardOn(const DungeonCtx& c) {
    return c.base == "D003" && c.flags_ok && c.has_map && c.sk_ok && c.sk_room >= 0;
}
std::array<double, 2> SkyKeepIconUV(const SkyKeepIcon& k, int cell) {
    const auto off = SkyKeepOffset(cell);
    return {static_cast<double>(k.x) + off[0], -static_cast<double>(k.z) + off[1]};
}
int SkyKeepIconDoorDeg(const SkyKeepIcon& k) {
    const int16_t yaw = static_cast<int16_t>(std::lround(k.yaw * 65536.0 / 360.0));
    return ConeDegrees(yaw, true, true) % 180;
}

void PublishDungeonMarkers(LivePub& pub, const EdenDsmodHostApi* host, const DungeonCtx& c,
                           const std::vector<LiveObj>& objs, const MapAxes& ax, const StoryCtx& sc) {
    const auto u_of = [&](float x) { return (ax.u_is_x ? static_cast<double>(x) : -static_cast<double>(x)) * ax.s + ax.du; };
    const auto v_of = [&](float z) { return (ax.v_is_minus_z ? -static_cast<double>(z) : static_cast<double>(z)) * ax.s + ax.dv; };
    const int scene = DungeonScene(host, c);
    // a scene / temp / zone flag of the dungeon's scene; nullopt when unreadable (fail closed)
    const auto flag = [&](int f, int room) -> std::optional<bool> {
        bool v = false;
        if (scene < 0 || !SceneFlag(host, scene, room, f, &v))
            return std::nullopt;
        return v;
    };
    uint8_t time_byte = 0;
    const int time = host->read_memory(host->userdata, host->main_base + 0x2BF98FD, &time_byte, 1) ? time_byte : -1;
    // stage metadata (*(*(main+0x2BF9DA8)+0xE7D8)) byte +0xE 1 / 2 suppresses the 0xCD locks
    std::optional<bool> cd_suppress;
    {
        uint64_t a = 0, meta = 0;
        uint8_t kind = 0;
        if (host->read_memory(host->userdata, host->main_base + 0x2BF9DA8, &a, 8) && a != 0 &&
            host->read_memory(host->userdata, a + 0xE7D8, &meta, 8))
            cd_suppress = meta != 0 && host->read_memory(host->userdata, meta + 0xE, &kind, 1) && (kind == 1 || kind == 2);
    }
    const std::optional<bool> cleared = StoryId(sc, sshd::ClearStory(c.index));
    int nd = 0, nk = 0, nc = 0, nx = 0;
    bool boss_done = false;
    std::optional<std::array<double, 2>> objective;
    const auto key_at = [&](double u, double v) {
        if (nk < kKeySlots)
            PubMarker(pub, "dk", nk++, u, v);
    };
    for (const auto& o : objs) {
        if (!c.OnFloor(o.y, o.room))
            continue;
        const bool door = o.prof == kProfDoorA || o.prof == kProfDoorB || o.prof == kProfDoor || o.prof == sshd::kProfCC;
        if (door) {
            if (!sshd::DoorMethodsKnown(o.prof, o.m_lock, o.m_known))
                continue; // another door class: fail closed
            if (o.prof == kProfDoor && o.b7c0 == 0)
                continue; // the native draws one door of a pair
            if (o.prof == sshd::kProfCC) {
                if (boss_done)
                    continue; // the first eligible boss door only
                boss_done = true;
            }
            if (!sshd::DoorBarShown(o.prof, o.params, o.room, c.has_map, c.visited))
                continue;
            float x = o.x, z = o.z;
            if (o.prof == kProfDoor && o.pair) {
                const double t = o.yaw * (2.0 * 3.14159265358979323846 / 65536.0);
                x = static_cast<float>(x - 70.0 * std::cos(t));
                z = static_cast<float>(z + 70.0 * std::sin(t));
            }
            if (nd < kDoorSlots) {
                char src[48];
                std::snprintf(src, sizeof(src), "module:sshd:door/%d", ConeDegrees(o.yaw, ax.u_is_x, ax.v_is_minus_z) % 180);
                PubMarker(pub, "dr", nd++, u_of(x), v_of(z), src);
            }
            bool locked = false;
            if (o.prof == kProfDoorA) {
                const auto f = flag(static_cast<int8_t>(o.b802), o.room);
                locked = static_cast<int8_t>(o.b802) >= 0 && f && sshd::LockedCA(o.b803, static_cast<int8_t>(o.b802), o.c650, *f);
            } else if (o.prof == kProfDoorB) {
                locked = sshd::LockedCB(o.p540);
            } else if (o.prof == kProfDoor) {
                const auto f = o.b7ba != 255 ? flag(o.b7ba, o.room) : std::optional<bool>(false);
                locked = f && cd_suppress && sshd::LockedCD({o.params, o.c760, *cd_suppress, o.b7b9, o.b7ba, *f, time});
            } else {
                const int bf = static_cast<int>((o.params >> 6) & 255);
                const auto f = bf != 255 ? flag(bf, o.room) : std::optional<bool>(false);
                const auto story = StoryId(sc, sshd::BossStory(o.params));
                locked = f && story && sshd::LockedCC(o.params, *f, *story);
                // the objective: Dungeon Map owned and the dungeon not cleared
                if (c.has_map && cleared && !*cleared)
                    objective = std::array<double, 2>{u_of(x), v_of(z)};
            }
            if (locked)
                key_at(u_of(x), v_of(z));
        } else if (o.prof == sshd::kProfTBox) {
            // opened (always) or closed (with the Dungeon Map); other states fail closed
            if (o.opened > 1 || (o.opened == 0 && !c.has_map) || nc >= kChestSlots)
                continue;
            PubMarker(pub, "dc", nc++, u_of(o.x), v_of(o.z),
                      o.opened == 1 ? "module:sshd:openchest" : "module:sshd:closedchest");
        } else if (o.prof == kProfMapMark) {
            const sshd::MarkFields m = sshd::ParseMark(o.mm);
            const auto sf = [&](int f, bool* v) {
                const auto r = flag(f, o.room);
                if (r)
                    *v = *r;
                return r.has_value();
            };
            if (m.type == 0) {
                // the red X: a type-0 mark whose native condition holds (Skyview's three hit
                // switches, scene flag 11; dev.31: story terms and the second story too)
                if (nx < kMarkSlots && sshd::MarkCondition(m, sf, sc))
                    PubMarker(pub, "mx", nx++, u_of(o.x), v_of(o.z));
            } else if (m.type == 4 && m.icon == 25 && c.base == "D301" && !boss_done) {
                // Sandship's boss-door substitute (main+0xC8013C icon 25): a bar at a fixed -90
                // degrees once the Map is owned or room 9 visited; the key while story 15 is
                // clear; the flame while story 15 is clear and the Map is owned
                if (!sshd::MarkCondition(m, sf, sc))
                    continue;
                boss_done = true;
                if (!(c.has_map || sshd::Visited(c.visited, 9)))
                    continue;
                if (nd < kDoorSlots)
                    PubMarker(pub, "dr", nd++, u_of(o.x), v_of(o.z), "module:sshd:door/90");
                const auto s15 = StoryId(sc, 15);
                if (s15 && !*s15) {
                    key_at(u_of(o.x), v_of(o.z));
                    if (c.has_map)
                        objective = std::array<double, 2>{u_of(o.x), v_of(o.z)};
                }
            }
        }
    }
    int nt = 0;
    if (SkyKeepBoardOn(c)) {
        for (const auto& k : kSkyKeepIcons) {
            const int cell = SkyKeepCell(c, k.tile);
            // dev.34: a Triforce piece on every tile, the loaded one too (its record mirrors the
            // live item actor, which exists exactly while the piece's scene flag is clear)
            if (k.kind == 't' && cell >= 0 && nt < kTriforceSlots) {
                const auto f = flag(k.flag, -1);
                if (f && !*f) {
                    const auto uv = SkyKeepIconUV(k, cell);
                    PubMarker(pub, "tf", nt++, uv[0], uv[1]);
                }
                continue;
            }
            if (k.tile == c.sk_room || cell < 0)
                continue;
            const auto uv = SkyKeepIconUV(k, cell);
            if (k.kind == 'd') {
                if (nd < kDoorSlots) {
                    char src[48];
                    std::snprintf(src, sizeof(src), "module:sshd:door/%d", SkyKeepIconDoorDeg(k));
                    PubMarker(pub, "dr", nd++, uv[0], uv[1], src);
                }
                const auto f = flag(k.flag, -1);
                if (k.b1 == 0x42 && f && !*f)
                    key_at(uv[0], uv[1]);
            } else if (k.kind == 'x') {
                if (nx < kMarkSlots)
                    PubMarker(pub, "mx", nx++, uv[0], uv[1]);
            } else if (k.kind == 'c') {
                const auto f = flag(k.flag, -1);
                if (f && nc < kChestSlots)
                    PubMarker(pub, "dc", nc++, uv[0], uv[1], *f ? "module:sshd:openchest" : "module:sshd:closedchest");
            }
        }
    }
    // The entrance arrow is not drawn (dev.31, Sky Keep analysis): its data is known (the
    // dungeon start main+0x193979C and yaw u16 main+0x2D032058, the start actor's own +0x13E copied
    // on spawn by main+0xAF0C2C), but the native never showed it in the live states measured
    // (N_start_000 is authored at alpha 0) and the pane rotation's consumer was not found, so a drawn
    // arrow would be a marker the native map does not show.
    pub.t("sshd.dk.src", "module:sshd:doorkey");
    pub.t("sshd.mx.src", "module:sshd:mark");
    pub.t("sshd.tf.src", "module:sshd:triforce");
    PubOff(pub, "tf", nt, kTriforceSlots);
    PubOff(pub, "dr", nd, kDoorSlots);
    PubOff(pub, "dk", nk, kKeySlots);
    PubOff(pub, "dc", nc, kChestSlots);
    PubOff(pub, "mx", nx, kMarkSlots);
    const int art = sshd::ObjectiveArt(c.index);
    if (objective && art >= 0) {
        char src[40];
        std::snprintf(src, sizeof(src), "module:sshd:objective/%d", art);
        PubMarker(pub, "ob", 0, (*objective)[0], (*objective)[1], src);
    } else {
        pub.i("sshd.ob0.on", 0);
    }
}

// The stage's STIF record (dev.33, Skyloft analysis): *(*(main+0x2BF9DA8)
// + 0xE7D8); byte +0x11 = the map context the native map stores at open (0 field, 1 dungeon-like,
// 2 Skyloft, 3 interiors, 4 the Sky), byte +0xF = the region (1 = the Sealed Grounds).
struct StageInfo {
    bool ok = false;
    int ctx = -1, region = -1;
};
StageInfo ReadStageInfo(const EdenDsmodHostApi* host) {
    StageInfo si;
    uint64_t mgr = 0, stif = 0;
    uint8_t b[0x12]{};
    if (host->read_memory(host->userdata, host->main_base + 0x2BF9DA8, &mgr, 8) && mgr != 0 &&
        host->read_memory(host->userdata, mgr + 0xE7D8, &stif, 8) && stif != 0 &&
        host->read_memory(host->userdata, stif, b, sizeof(b))) {
        si.ok = true;
        si.ctx = b[0x11];
        si.region = b[0xF];
    }
    return si;
}

// A dungeon flag of the current scene as the native door methods read it (main+0xE089E8): manager
// *(main+0x182E120), scene u16 +2, record k of the mapping *(*(mgr+8)+8) (word, shift << 4 | width
// - 1) over the scene's row slot+0xA64 + 16 * scene. nullopt when unreadable.
std::optional<uint32_t> DungeonFlag(const EdenDsmodHostApi* host, int k) {
    uint64_t mgr = 0, m1 = 0, map = 0;
    uint16_t scene = 0, word = 0;
    uint8_t rec[2]{};
    if (!host->read_memory(host->userdata, host->main_base + 0x182E120, &mgr, 8) || mgr == 0 ||
        !host->read_memory(host->userdata, mgr + 2, &scene, 2) || !host->read_memory(host->userdata, mgr + 8, &m1, 8) ||
        m1 == 0 || !host->read_memory(host->userdata, m1 + 8, &map, 8) || map == 0 ||
        !host->read_memory(host->userdata, map + 2u * static_cast<uint64_t>(k), rec, 2) || scene >= 26 || rec[0] >= 8)
        return std::nullopt;
    if (!host->read_memory(host->userdata, host->main_base + kSlot + 0xA64 + 16u * scene + 2u * rec[0], &word, 2))
        return std::nullopt;
    return (word >> (rec[1] >> 4)) & ((2u << (rec[1] & 15)) - 1u);
}

// The native door record's known byte (Sky class main+0xC70188, field class main+0xC7BB2C).
// dev.34 (item 11): the context-3 branch is confirmed in a real interior. Live hero F001r (Knight
// Academy, STIF +0x11 = 3): the native map draws the door bars of the shown floor (1F: six), with
// no dungeon flag or visited room. The dev.33 test forced context 3 on Skyloft, a Sky-class stage
// whose map changed style instead, so it never tested this branch. The companion draws the bars of
// Link's floor band only (on_floor, generic multi-floor stages).
bool DoorKnown(const EdenDsmodHostApi* host, const StageInfo& si, int room) {
    if (si.ctx == 3)
        return true;
    if (si.ctx != 1 && si.region != 1)
        return false;
    const auto map = DungeonFlag(host, 2);
    if (map && *map != 0)
        return true;
    if (room < 0 || room > 63)
        return false;
    const auto v = DungeonFlag(host, 8 + room / 16);
    return v && ((*v >> (room % 16)) & 1u) != 0;
}

int PouchSlotCount(const EdenDsmodHostApi* host);
// The native map's Bug Medal gate (dev.33): set at map open from story 166 == 0, not the special mode
// and the medal (pouch item 0x46) in one of the unlocked pouch slots.
bool BugMedalOn(const EdenDsmodHostApi* host, const StoryCtx& sc) {
    const auto s166 = sc.Flag(166);
    uint64_t special = 0;
    uint32_t mode = 0;
    if (!s166 || *s166 || !host->read_memory(host->userdata, host->main_base + 0x192FB58, &special, 8) ||
        !host->read_memory(host->userdata, host->main_base + 0x166CC10, &mode, 4) || (special != 0 && mode == 9))
        return false;
    const int n = PouchSlotCount(host);
    for (int i = 0; i < n; ++i) {
        uint16_t id = 0;
        if (host->read_memory(host->userdata, host->main_base + kSlot + 0x7C0 + 4u * i, &id, 2) && id == 0x46)
            return true;
    }
    return false;
}

void PublishLiveMarkers(Instance* inst, const EdenDsmodHostApi* host, const DungeonCtx& dc, StoryLazy& story) {
    // throttle: the walk reads a few hundred objects; every 8th sample, or at once on a new stage
    // or when the shown floor / Dungeon Map changes
    char st[9]{};
    uint32_t scene_name = 0;
    int16_t scene = -1;
    const uint64_t slot = host->main_base + kSlot;
    const bool read = host->read_memory(host->userdata, slot + 0x531C, st, 8) &&
                      host->read_memory(host->userdata, slot + 0x5318, &scene, 2) &&
                      host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene_name, 4);
    const std::string_view sv(st);
    MapAxes ax = read && scene_name != 0x53656C74 ? AxesOf(sv) : MapAxes{false, false, false};
    if (dc.base == "D003") {
        // Sky Keep: the loaded room's markers in its tile; off the grid nothing is drawn
        if (dc.sk_cell < 0)
            ax.ok = false;
        const auto off = SkyKeepOffset(std::max(0, dc.sk_cell));
        ax.du = off[0], ax.dv = off[1];
    }
    // every sample publishes a fresh snapshot: a throttled sample republishes the last walk's values
    LivePub local;
    LivePub& pub = inst != nullptr ? inst->live_pub : local;
    if (inst != nullptr) {
        const int key = (dc.floor + 8) * 4 + (dc.has_map ? 2 : 0) + (dc.flags_ok ? 1 : 0);
        const bool same = sv == inst->live_stage && key == inst->live_key;
        inst->live_key = key;
        if (same && ++inst->live_tick < 8) {
            pub.Publish(host);
            PublishPlates(inst, host, pub.plates, sv);
            return;
        }
        inst->live_tick = 0;
        std::snprintf(inst->live_stage, sizeof(inst->live_stage), "%s", st);
    }
    pub.Clear();
    const StoryCtx& sc = story.Get(); // dev.41: read on walk samples only
    std::vector<LiveObj> objs = ax.ok ? WalkLiveObjects(host) : std::vector<LiveObj>{};
    if (ax.rot != 0) // dev.34: a turned generic frame: positions and yaws in its map coordinates
        for (auto& o : objs) {
            TurnXZ(ax.rot, o.x, o.z);
            o.yaw = static_cast<int16_t>(o.yaw - ax.rot * 0x4000);
        }
    const auto u_of = [&](float x) { return (ax.u_is_x ? static_cast<double>(x) : -static_cast<double>(x)) * ax.s + ax.du; };
    const auto v_of = [&](float z) { return (ax.v_is_minus_z ? -static_cast<double>(z) : static_cast<double>(z)) * ax.s + ax.dv; };
    const bool sky = sv == "F020" || sv == "F023"; // dev.35: F023 on the Sky picture (Sky class marks)
    const bool dungeon = dc.dungeon;
    const bool has_map = dc.has_map;
    // dev.34: a generic multi-floor stage (e.g. the Knight Academy) shows one floor band: Link's
    // (dev.35: or the picked floor tab's)
    std::optional<std::pair<int, int>> gen_band;
    if (ax.ok && !dungeon && dc.gen)
        gen_band = FloorBand(dc.ft, dc.floor);
    const auto on_floor = [&](float y, int room = -1) {
        if (gen_band)
            return y >= gen_band->first && y <= gen_band->second;
        return !dungeon || dc.OnFloor(y, room);
    };

    // Bird statues (profile 0x216): shown when activated (checkStatueFlag), in a dungeon also when
    // its Dungeon Map is owned (the native reveal); colour = statue type (params >> 8) & 0xFF:
    // 0 normal (green), 1 overworld (orange), 2 dungeon (yellow), as saveObjectColor frames 0/1/2.
    int ns = 0;
    for (const auto& o : objs) {
        if (o.prof != kProfStatue || ns >= kStatueSlots || !on_floor(o.y, o.room))
            continue;
        bool activated = false;
        if (o.pmf == kStatueAlwaysFn)
            activated = true;
        else if (o.pmf != kStatueFlagFn || !SceneFlag(host, scene, o.room, static_cast<int>(o.params & 0xFF), &activated))
            continue; // an unknown check function: fail closed
        if (!activated && !(dungeon && has_map))
            continue;
        const int type = static_cast<int>((o.params >> 8) & 0xFF);
        char src[48];
        std::snprintf(src, sizeof(src), "module:sshd:statue/%d/%d", type <= 2 ? type : 0, sky ? 1 : 0);
        PubMarker(pub, "st", ns++, u_of(o.x), v_of(o.z), src);
    }
    // Sky Keep's other tiles' statues (dev.33, kSkyKeepIcons: drawn once the Map is owned, yellow)
    if (SkyKeepBoardOn(dc))
        for (const auto& k : kSkyKeepIcons) {
            const int cell = SkyKeepCell(dc, k.tile);
            if (k.kind != 's' || k.tile == dc.sk_room || cell < 0 || ns >= kStatueSlots)
                continue;
            const auto uv = SkyKeepIconUV(k, cell);
            PubMarker(pub, "st", ns++, uv[0], uv[1], "module:sshd:statue/2/0");
        }
    PubOff(pub, "st", ns, kStatueSlots);

    if (dungeon && dc.flags_ok) {
        PublishDungeonMarkers(pub, host, dc, objs, ax, sc);
    } else {
        // Door bars (profiles 0xCA / 0xCB; 0xCD only the door of a double door with +0x7C0 set,
        // drawn 70 units in towards the pair's centre) on field and Skyloft maps, as dev.27, behind
        // the native door record's "known" gate (dev.33, Skyloft analysis:
        // the draw loop skips a door record whose known byte is 0): known = map context 3
        // (interiors) || ((context 1 || region 1, the Sealed Grounds) && (dungeon flag 2 || the
        // door's room visited)). Skyloft (context 2), the Sky (4) and the ordinary fields (0) never
        // draw a bar. An unreadable stage record or flag draws nothing (fail closed).
        int nd = 0;
        const StageInfo si = ReadStageInfo(host);
        if (!sky && !dungeon && si.ok) {
            for (const auto& o : objs) {
                if (nd >= kDoorSlots || !(o.prof == kProfDoorA || o.prof == kProfDoorB || (o.prof == kProfDoor && o.b7c0 != 0)))
                    continue;
                if (!DoorKnown(host, si, o.room))
                    continue;
                float x = o.x, z = o.z;
                if (o.prof == kProfDoor && o.pair) {
                    const double t = o.yaw * (2.0 * 3.14159265358979323846 / 65536.0);
                    x = static_cast<float>(x - 70.0 * std::cos(t));
                    z = static_cast<float>(z + 70.0 * std::sin(t));
                }
                // the bar lies across the door's facing: its picture turns with the projected facing
                char src[48];
                std::snprintf(src, sizeof(src), "module:sshd:door/%d", ConeDegrees(o.yaw, ax.u_is_x, ax.v_is_minus_z) % 180);
                PubMarker(pub, "dr", nd++, u_of(x), v_of(z), src);
            }
        }
        PubOff(pub, "dr", nd, kDoorSlots);
        PubOff(pub, "dk", 0, kKeySlots);
        PubOff(pub, "dc", 0, kChestSlots);
        PubOff(pub, "mx", 0, kMarkSlots);
        pub.i("sshd.ob0.on", 0);
    }

    // a map mark's native condition main+0xC732B0 (dev.31: scene / story / toggle / second story)
    const auto mark_ok = [&](const LiveObj& o, const sshd::MarkFields& m) {
        const auto sf = [&](int f, bool* v) { return SceneFlag(host, scene, o.room, f, v); };
        return sshd::MarkCondition(m, sf, sc);
    };
    // Neighbouring-area plates: map marks (profile 0x224) of type 3 (+0x19C) on field maps
    // (main+0xC7EA24). Label index +0x198 (0x28 = 11, MAP_40): its name when the area's bit is set
    // in the file's discovered-area mask (slot+0x7BC), else the native "?" plate; Faron Woods
    // (index 2) instead needs story flag 130 (Impa met, the map's +0x15E2 = flag 0x82 at map
    // build). dev.31: each mark's native condition decides (Faron's Deep Woods plate story 58, Lake
    // Floria scene 56, its dead duplicate never); an unreadable condition fails closed.
    int np = 0;
    uint32_t discovered = 0;
    const bool field = !sky && !dungeon && st[0] == 'F' && sv != "F000";
    if (field && host->read_memory(host->userdata, slot + 0x7BC, &discovered, 4)) {
        for (const auto& o : objs) {
            if (o.prof != kProfMapMark || np >= kPlaceSlots)
                continue;
            const sshd::MarkFields m = sshd::ParseMark(o.mm);
            if (m.type != 3 || !on_floor(o.y) || !mark_ok(o, m))
                continue;
            int label = m.label == 0x28 ? 11 : m.label;
            int shown = label <= 11 && ((discovered >> label) & 1) != 0 ? label : 12;
            if (label == 2) {
                const auto impa = sc.Flag(130); // story 130, Impa met
                if (!impa)
                    continue;
                shown = *impa ? 2 : 12;
            }
            // the plate sits outside the mark along its facing (screen direction, degrees cw from up)
            const int deg = ConeDegrees(o.yaw, ax.u_is_x, ax.v_is_minus_z);
            // plate colour = the current map's region (mapPlace_00_color; as the area plates:
            // Faron green, Eldin red, Lanayru yellow)
            const int region = st[1] == '2' ? 2 : st[1] == '3' ? 3 : 1;
            // placed on screen (and shown only where it fits) by PublishPlates, every sample
            pub.plates.push_back({shown, deg, region, u_of(o.x), v_of(o.z)});
            ++np;
        }
    }
    // NPC speech balloons (dev.31, NPC map mark analysis): type-4 map
    // marks whose icon kind (+0x199) is a portrait, at the mark's own position (not the NPC's):
    // Sky / Skyloft (MapSky) 17 Dodoh, 19 Peater, 20 Strich, 23 Fledge; field (MapField) 18 Thrill
    // Digger Mogma, 21 Thunder Dragon, 22 Gortram. Native rule: a live, not hidden mark on the shown
    // floor whose condition holds (Story 42 / 150 / 201 / 400; 180 / 509 / 274 as authored), and
    // Fledge only by day (time byte main+0x2BF98FD != 1; unreadable: not drawn).
    uint8_t time_byte = 1;
    const bool day = host->read_memory(host->userdata, host->main_base + 0x2BF98FD, &time_byte, 1) && time_byte != 1;
    const bool sky_layout = sky || sv == "F000";
    int nb = 0, nc = 0;
    if (!dungeon) {
        for (const auto& o : objs) {
            if (o.prof != kProfMapMark || !on_floor(o.y))
                continue;
            const sshd::MarkFields m = sshd::ParseMark(o.mm);
            if (m.type != 4)
                continue;
            const bool sky_kind = m.icon == 17 || m.icon == 19 || m.icon == 20 || m.icon == 23;
            const bool field_kind = m.icon == 18 || m.icon == 21 || m.icon == 22;
            if (sky_kind || field_kind) {
                if (nb >= kBalloonSlots || (sky_kind ? !sky_layout : (sky_layout || st[0] != 'F')))
                    continue;
                if ((m.icon == 23 && !day) || !mark_ok(o, m))
                    continue;
                char src[40];
                std::snprintf(src, sizeof(src), "module:sshd:balloon/%d", m.icon);
                PubMarker(pub, "nb", nb++, u_of(o.x), v_of(o.z), src);
            } else if (m.icon == 24 && field && nc < kCaveSlots && mark_ok(o, m)) {
                // a field landmark glyph (N_cave_00, main+0xC8013C pane 38): dungeon entrances
                // whose discovery story flag (89 / 155 / 183 / 211) the mark's condition reads
                PubMarker(pub, "cv", nc++, u_of(o.x), v_of(o.z));
            }
        }
    }
    PubOff(pub, "nb", nb, kBalloonSlots);
    PubOff(pub, "cv", nc, kCaveSlots);
    // Red common targets (dev.33, Skyloft analysis C2a): the Sky class
    // (Skyloft, the Sky) draws a type-4 mark whose icon kind (+0x199) is 0, 255 or >= 25
    // (N_commonIcon_00) or 2 / 3 (the Academy / Bazaar, N_commonIcon_02 / _03, a size larger) when
    // its native condition holds (the house discovery stories 712..720 are in it); the field class
    // rejects these icons. Not drawn: the hover popup labels (MAP_POP_*).
    int nt = 0;
    if (sky_layout && !dungeon) {
        for (const auto& o : objs) {
            if (o.prof != kProfMapMark || nt >= kTargetSlots || !on_floor(o.y))
                continue;
            const sshd::MarkFields m = sshd::ParseMark(o.mm);
            const bool common = m.icon == 0 || m.icon == 255 || m.icon >= 25;
            if (m.type != 4 || !(common || m.icon == 2 || m.icon == 3) || !mark_ok(o, m))
                continue;
            PubMarker(pub, "tg", nt++, u_of(o.x), v_of(o.z), common ? "module:sshd:target/0" : "module:sshd:target/1");
        }
    }
    PubOff(pub, "tg", nt, kTargetSlots);
    // Bug Medal bug locations (dev.33, Skyloft analysis C2c, Sky class main+0xC72524,
    // field class main+0xC7F594): with the medal (pouch item 0x46 in an unlocked pouch slot), story
    // 166 clear and not the special mode (*(main+0x192FB58) != 0 && u32(main+0x166CC10) == 9), each
    // live insect tag (0x1C1) with +0x3D6 == 0, a count and a live insect, at the tag. Unreadable:
    // none.
    int ng = 0;
    // the field map class (MapField): not the Sky class stages (the Sky, Skyloft, the Thunderhead)
    const bool field_class = !sky && sv != "F000" && sv != "F023";
    if (!dungeon && BugMedalOn(host, sc)) {
        for (const auto& o : objs)
            if (((o.prof == kProfBugTag && o.opened == 1) || (o.prof == kProfBee && field_class)) && ng < kBugSlots &&
                on_floor(o.y))
                PubMarker(pub, "bg", ng++, u_of(o.x), v_of(o.z));
    }
    PubOff(pub, "bg", ng, kBugSlots);
    pub.t("sshd.bg.src", "module:sshd:bug");
    pub.t("sshd.cv.src", "module:sshd:cave");
    // Beedle's airshop (live profile 0x69, the Sky layout's handler: by day, not hidden, at the
    // actor's position). Its portrait child (N_terryOnOff_00) was only seen with story 709 (shop
    // entered) set, so the shop is drawn with the face then and not at all otherwise (the 709-clear
    // composition is not proven).
    bool shop = false;
    if (sky_layout && day) {
        const auto entered = sc.Flag(709);
        for (const auto& o : objs)
            if (o.prof == kProfBeedle && entered && *entered && !shop) {
                PubMarker(pub, "bs", 0, u_of(o.x), v_of(o.z), "module:sshd:beedle/1");
                shop = true;
            }
    }
    if (!shop)
        pub.i("sshd.bs0.on", 0);
    // Kikwi faces (field; the Kikwi marker analysis): live EA / EB / ED / EE / EC actors with
    // the common face callback main+0x5BAB70 (its story id +0x19FA), 13C with main+0x61ACA8
    // (story 53), drawn at the actor while that story flag reads set in the manager's cached
    // buffer (vfunc +0x58: *(*(mgr+0x10)+8), the table's mapping); the Elder (EC) with his own face.
    // Another callback, an unreadable buffer: not drawn.
    int nk = 0;
    if (field && sc.table != nullptr) {
        const auto rd = [host](uint64_t a, void* o, size_t n) {
            return a != 0 && host->read_memory(host->userdata, a, o, n) != EDEN_DSMOD_FALSE;
        };
        const uint64_t cached = sshd::StoryCachedAddr(rd, host->main_base);
        sshd::StoryWords cw;
        cw.valid = cached != 0 && rd(cached, cw.w, sizeof(cw.w));
        for (const auto& o : objs) {
            if (!IsKikwi(o.prof) || nk >= kKikwiSlots || !on_floor(o.y))
                continue;
            int fid = -1;
            if (o.prof == 0x13C && o.pmf == 0x61ACA8)
                fid = 53;
            else if (o.prof != 0x13C && o.pmf == 0x5BAB70 && o.fid <= 0x7FF)
                fid = o.fid;
            const auto v = fid >= 0 ? sshd::StoryValue(*sc.table, cw, fid) : std::nullopt;
            if (!v || *v == 0)
                continue;
            PubMarker(pub, "kk", nk++, u_of(o.x), v_of(o.z), o.prof == 0xEC ? "module:sshd:kikwi/1" : "module:sshd:kikwi/0");
        }
    }
    PubOff(pub, "kk", nk, kKikwiSlots);
    pub.i("sshd.live.n", static_cast<int64_t>(objs.size())); // diagnostics only (objects walked), unbound
    pub.Publish(host);
    PublishPlates(inst, host, pub.plates, sv);
}

// The Sky map's fixed layers (MapSky mapIcon_01, map marker analysis):
// - the thunderhead at (-135210, -84790), always on the Sky map; its cloud cover is the
//   cloudAlpha frame 1 once story flag 379 (thunderhead entered) is set, else frame 0;
// - the three surface light columns (LightLi objects; their params name story flags 46 / 47 / 48)
//   in the region colour, each once that region's pillar is opened (the native reads the same
//   three flags: object +0xDA0 region 0 / 1 / 2 -> its Faron / Eldin / Lanayru pillar bools);
// - island names: Skyloft always, the other islands once discovered (story 281..286).
void PublishSkyLayers(Instance* inst, const EdenDsmodHostApi* host) {
    char st[9]{};
    uint32_t scene_name = 0;
    const bool sky = host->read_memory(host->userdata, host->main_base + kSlot + 0x531C, st, 8) &&
                     host->read_memory(host->userdata, host->main_base + 0x182A7F4, &scene_name, 4) &&
                     (std::strcmp(st, "F020") == 0 || std::strcmp(st, "F023") == 0) && scene_name != 0x53656C74;
    uint8_t s[0x30]{};
    const bool flags = sky && host->read_memory(host->userdata, host->main_base + kStoryFlags, s, sizeof(s));
    const auto bit = [&](int byte, int b) { return flags && ((s[byte] >> b) & 1) != 0; };
    const sshd::MapView view = sky ? ViewOf(host, "F020") : sshd::MapView{};
    // dev.41: the same flags, view and name sizes as the last sample: its values again
    const sshd::MapView kv = view.Valid() ? view : sshd::MapView{}; // NaN never compares equal
    std::vector<double> key{sky ? 1.0 : 0.0, flags ? 1.0 : 0.0, kv.cx, kv.cy, kv.ppw, kv.x, kv.y, kv.w, kv.h};
    for (const int b : {0x0E, 0x0F, 0x22, 0x23, 0x2C})
        key.push_back(flags ? s[b] : 0);
    PubCache* cache = inst != nullptr ? &inst->sky_cache : nullptr;
    if (cache != nullptr && cache->Hit(key)) {
        cache->pub.Publish(host);
        return;
    }
    LivePub out;
    bool sizes_known = true;
    // Sky map axes: u = x, v = -z
    out.i("sshd.th.on", flags ? 1 : 0);
    out.f("sshd.th.u", -135210.0);
    out.f("sshd.th.v", 84790.0);
    out.t("sshd.th.src", bit(0x2C, 1) ? "module:sshd:thunderhead/1" : "module:sshd:thunderhead/0");
    static constexpr const char* kCol[3][4] = {{"sshd.col0.on", "sshd.col0.u", "sshd.col0.v", "sshd.col0.src"},
                                               {"sshd.col1.on", "sshd.col1.u", "sshd.col1.v", "sshd.col1.src"},
                                               {"sshd.col2.on", "sshd.col2.u", "sshd.col2.v", "sshd.col2.src"}};
    static constexpr float kColXZ[3][2] = {{39582.6f, 39378.9f}, {26500.0f, -77700.0f}, {-65300.0f, 63800.0f}};
    const bool pillar[3] = {bit(0x0E, 7), bit(0x0F, 0), bit(0x0F, 1)}; // story 46, 47, 48
    for (int i = 0; i < 3; ++i) {
        out.i(kCol[i][0], pillar[i] ? 1 : 0);
        out.f(kCol[i][1], kColXZ[i][0]);
        out.f(kCol[i][2], -kColXZ[i][1]);
        char src[40];
        std::snprintf(src, sizeof(src), "module:sshd:column/%d", i);
        out.t(kCol[i][3], src);
    }
    // island names, at the companion's island art (sshd_art kSkyIslands), the name `gap` px right:
    // Skyloft, Fun Fun (281), Pumpkin (282), Beedle (283), Bamboo (284), Isle of Songs (285),
    // Bug Rock (286)
    static constexpr const char* kIsl[7][4] = {
        {"sshd.isl0.on", "sshd.isl0.u", "sshd.isl0.v", "sshd.isl0.src"}, {"sshd.isl1.on", "sshd.isl1.u", "sshd.isl1.v", "sshd.isl1.src"},
        {"sshd.isl2.on", "sshd.isl2.u", "sshd.isl2.v", "sshd.isl2.src"}, {"sshd.isl3.on", "sshd.isl3.u", "sshd.isl3.v", "sshd.isl3.src"},
        {"sshd.isl4.on", "sshd.isl4.u", "sshd.isl4.v", "sshd.isl4.src"}, {"sshd.isl5.on", "sshd.isl5.u", "sshd.isl5.v", "sshd.isl5.src"},
        {"sshd.isl6.on", "sshd.isl6.u", "sshd.isl6.v", "sshd.isl6.src"}};
    static constexpr float kIslXZ[7][2] = {{0.0f, 0.0f},           {-114296.0f, 49995.0f},  {102667.0f, 47640.0f},
                                           {130349.0f, -108755.0f}, {39922.0f, -110282.0f}, {-140495.0f, -109061.0f},
                                           {-170937.0f, -50851.0f}};
    const bool disc[7] = {flags, bit(0x22, 3), bit(0x22, 4), bit(0x22, 5), bit(0x22, 6), bit(0x22, 7), bit(0x23, 0)};
    // each name fully inside the Fly page's view and clear of its title / compass / gauge / wings /
    // Beacons (as the neighbour plates, sshd_plate.h): moved outwards from its island, else hidden
    int64_t known = 0; // discovered names, bit i (diagnostics: shown or not, the rule's input)
    for (int i = 0; i < 7; ++i)
        known |= disc[i] ? int64_t{1} << i : 0;
    out.i("sshd.isl.known", known);
    for (int i = 0; i < 7; ++i) {
        const int gap = i == 0 ? 66 : 46;
        std::optional<std::array<double, 2>> at;
        if (disc[i] && view.Valid() && inst != nullptr &&
            LabelReady(inst, inst->isle_size[i], [&](sshd::Art& art, Instance::LabelSize& m) {
                const auto b = art.IsleNameBox(i, gap);
                if (b)
                    m.box = *b;
                return b.has_value();
            })) {
            const sshd::Box& b = inst->isle_size[i].box;
            at = FitAnchor(view, kIslXZ[i][0], -kIslXZ[i][1], {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2}, b.W(), b.H(),
                           kFlyAvoid, kIsleGap, true);
        }
        if (disc[i] && view.Valid() && (inst == nullptr || inst->isle_size[i].state != 1))
            sizes_known = false;
        out.i(kIsl[i][0], at ? 1 : 0);
        out.f(kIsl[i][1], at ? (*at)[0] : kIslXZ[i][0]);
        out.f(kIsl[i][2], at ? (*at)[1] : -kIslXZ[i][1]);
        char src[40];
        std::snprintf(src, sizeof(src), "module:sshd:islename/%d/%d", i, gap);
        out.t(kIsl[i][3], src);
    }
    out.Publish(host);
    if (cache != nullptr) {
        // a name size still being measured (or retried) is computed again next sample
        cache->valid = sizes_known;
        cache->key = std::move(key);
        cache->pub = std::move(out);
    }
}

void Sample(void* instance, const EdenDsmodHostApi* host) {
    if (host == nullptr || host->publish_i64 == nullptr) {
        return;
    }
    if (instance != nullptr)
        static_cast<Instance*>(instance)->diag.Tick();
    PublishCrystals(host);
    PublishCubes(host);
    PublishStamina(host);
    const DungeonCtx dc = host->read_memory != nullptr ? ReadDungeon(static_cast<Instance*>(instance), host) : DungeonCtx{};
    if (host->read_memory != nullptr && host->publish_text != nullptr)
        PublishCone(static_cast<Instance*>(instance), host, (dc.dungeon || dc.gen) && dc.floor != dc.link_floor);
    StoryLazy story{static_cast<Instance*>(instance), host, std::nullopt};
    if (host->read_memory != nullptr && host->publish_text != nullptr && host->publish_f64 != nullptr)
        PublishLiveMarkers(static_cast<Instance*>(instance), host, dc, story);
    if (host->read_memory != nullptr && host->publish_text != nullptr && host->publish_f64 != nullptr)
        PublishSkyLayers(static_cast<Instance*>(instance), host);
    if (host->read_memory != nullptr && host->publish_f64 != nullptr)
        PublishOpenChests(host);
    if (host->publish_f64 != nullptr)
        PublishGoddessChests(host);
    if (host->publish_text != nullptr) {
        PublishSword(host);
        PublishShortcuts(host);
    }
    if (host->read_memory != nullptr && host->publish_f64 != nullptr)
        PublishBeacons(host);
    if (host->read_memory != nullptr && host->publish_text != nullptr)
        PublishDungeonMap(host, dc);
    if (host->read_memory != nullptr && host->publish_text != nullptr)
        PublishFieldMap(static_cast<Instance*>(instance), host, story);
    if (host->read_memory != nullptr)
        host->publish_i64(host->userdata, "sshd.story.why", story.Why()); // diagnostics only
    if (host->read_memory != nullptr && host->publish_text != nullptr && host->publish_f64 != nullptr)
        PublishGenMap(host, dc);
    auto* inst = static_cast<Instance*>(instance);
    if (inst != nullptr)
        ApplyHud(inst, host);
    if (inst != nullptr) {
        const bool helper_ready = inst->heartbeat_still < 30 && inst->mailbox;
        host->publish_i64(host->userdata, "sshd.stick.ready", helper_ready ? 1 : 0);
        // the gameplay words behind every action gate, read once per (Thor-like throttled) sample
        if (inst->diag.due) {
            inst->gate = ReadGate(host);
            MenuRead m = MenuSample(inst, host);
            // SSHD_DIAG_MENU_BLIP: this one sample sees a pause state (dev.40's injected blips)
            if (inst->diag.menu_blip > 0 && static_cast<int>(inst->diag.Rand() % 100u) < inst->diag.menu_blip)
                m.code = kMenuOpen;
            MenuGate(inst, m);
        }
        GateDiag(inst);
        // the fallback swipes exist only while the helper is not ready; 0 whenever it is
        const int fwhy = helper_ready ? kWhyOk : FallbackWhy(inst, host);
        host->publish_i64(host->userdata, "sshd.swipe.fallback", !helper_ready && fwhy == kWhyOk ? 1 : 0);
        // dev.43: the runtime refuses a fallback flick itself (enabled_bind); the reason behind it is
        // logged once each time it changes while the fallback is in use, never per sample
        if (!helper_ready && fwhy != inst->fallback_why) {
            if (fwhy != kWhyOk)
                GateLine(inst, "swipe fallback", "off", fwhy, "");
            else if (host->log != nullptr)
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO, "sshd: swipe fallback on (free play)");
        }
        inst->fallback_why = helper_ready ? -1 : fwhy;
        PouchPoll(inst, host);
        SpinPoll(inst, host);
        if (inst->bash_flash > 0)
            --inst->bash_flash;
        host->publish_i64(host->userdata, "sshd.bash.refused", inst->bash_flash > 0 ? 1 : 0);
        host->publish_i64(host->userdata, "sshd.bash.putaway", inst->bash_flash > 0 && inst->bash_why == kWhyItem ? 1 : 0);
        host->publish_i64(host->userdata, "sshd.bash.notnow", inst->bash_flash > 0 && inst->bash_why != kWhyItem ? 1 : 0);
        // dev.38: the swipe pad's trail and refusal as StickMirror last computed them
        PublishTrail(inst, host);
        PublishDpad(inst, host);
        ApplyTopHide(inst, host);
    }
}

// dev.38 Ready swipe pad (sshd_stroke.h, owner Thor feedback 2026-11-12): PanStroke tells each pad's
// finger from the host's glide home (dev.17..37 lost a stroke that began while the last one still
// glided: back-and-forth slashes), Stroke turns the finger's displacement into the stick vector (a
// fast flick = a full deflection, a short hold after the lift) and the gold trail, drawn from the
// minimap centre (runtime 22..24 give a module the pan only, not where the finger is). dev.39: on a
// runtime that tells where the finger is (25: @view_touch_x/y:stickpad_<i>) the trail is drawn under
// the finger instead (sshd::FingerTrail), chosen per stroke by whether finger positions came; the
// stick input is the same either way. The game
// decides the rest: a held stick poses the sword, a flick slashes. Refused (nothing fed, "Not now"
// over the pad for a moment) while the game cannot slash: a menu, an event, a load, the title, a
// Silent Realm (no sword), no sword yet, a stopped helper (LBlocked, debounced like the L lock). A
// spin sequence in flight owns the stick words; with the L lock on, the stroke waits for the game
// to see L up (the dev.24 late-flick replay keeps a stroke that never got through; dev.45 also one
// that got through only in its last ticks, and one that ran under a sequence: SwipeEnded).
// SSHD_DIAG_LEGACY_PAD=1 runs the dev.17..37 logic instead (LegacyPadMirror; the measurement's "before").
constexpr double kStrokeAnchor[2] = {620, 430}; // the minimap centre (gen_manifest MX, MY): the trail's origin
constexpr int kStrokePost = 30;      // ticks the diagnostics keep watching the player after a lift

// Link's action id for the stroke log: u32 +0x468 of the player behind main+0x1826AD0 (SwimState's
// pointer, low half) and behind main+0x2BF9940 (kPlayer, high half); the sword-swing actions are
// 0x68 / 0x9a (sword swing analysis, static; live check pending).
uint32_t PlayerAction(const EdenDsmodHostApi* host) {
    uint32_t out = 0;
    for (const uint64_t base : {kPlayerPtr, kPlayer}) {
        uint64_t player = 0;
        uint32_t action = 0xFFFF;
        if (host->read_memory == nullptr || !host->read_memory(host->userdata, host->main_base + base, &player, 8) ||
            player == 0 || !host->read_memory(host->userdata, player + 0x468, &action, 4))
            action = 0xFFFF;
        out = (out << 16) | (action & 0xFFFF);
    }
    return out;
}

// Diagnostics (SSHD_DIAG): one line per stroke, after kStrokePost ticks of watching Link's action.
void StrokeDiag(Instance* inst, const EdenDsmodHostApi* host, bool flush) {
    auto& d = inst->sdiag;
    if (!inst->diag.on || !d.open)
        return;
    if (!flush) {
        if (d.post == 0)
            return; // the stroke still runs
        --d.post;
        const uint32_t a = PlayerAction(host);
        if (std::find(d.acts.begin(), d.acts.begin() + d.nacts, a) == d.acts.begin() + d.nacts && d.nacts < 8)
            d.acts[d.nacts++] = a;
        if (d.post > 0)
            return;
    }
    char acts[96] = "";
    for (int i = 0, o = 0; i < d.nacts && o < 90; ++i)
        o += std::snprintf(acts + o, sizeof(acts) - o, "%s%x", i ? "," : "", d.acts[i]);
    char msg[448];
    std::snprintf(msg, sizeof(msg),
                  "sshd stroke %u: %s ticks %d start (%.0f,%.0f) len %.0f peak %.2f flick %d fed %d gate %d lwait %d "
                  "spin %d replay %d skipped %d acts %s trail %s fs %d land (%.0f,%.0f) head (%.0f,%.0f)",
                  d.id, d.legacy ? "pan" : "drag", d.s.ticks, d.s.x0, d.s.y0, d.s.len, d.s.peak, d.s.flick ? 1 : 0,
                  d.s.fed, d.gate, d.lwait, d.spin, d.replay, d.skipped, acts, d.trail ? "finger" : "anchor",
                  d.fsamples, d.lx, d.ly, d.hx, d.hy);
    DiagLog(inst, msg);
    d.open = false;
}

void StrokeDiagTick(Instance* inst, const EdenDsmodHostApi* host, bool stroking) {
    auto& d = inst->sdiag;
    if (!inst->diag.on || !stroking)
        return;
    const uint32_t a = PlayerAction(host);
    if (std::find(d.acts.begin(), d.acts.begin() + d.nacts, a) == d.acts.begin() + d.nacts && d.nacts < 8)
        d.acts[d.nacts++] = a;
}

// The trail and the swipe refusal as StickMirror last computed them (Instance::trail /
// swipe_flash), published by the sample and by the tick alike so both leave the same picture.
void PublishTrail(Instance* inst, const EdenDsmodHostApi* host) {
    static constexpr int N = sshd::Stroke::kTrailPoints;
    static const auto keys = [] {
        std::array<std::array<std::string, 4>, N> k;
        for (int i = 0; i < N; ++i) {
            const std::string p = "sshd.tr" + std::to_string(i) + ".";
            k[i] = {p + "x", p + "y", p + "a", p + "s"};
        }
        return k;
    }();
    const auto& trail = inst->trail;
    const bool refused = inst->swipe_flash > 0;
    bool any = false;
    for (int i = 0; i < N; ++i) {
        const auto& d = trail[i];
        if (!d.on)
            continue; // off: left unpublished (the snapshot starts empty every tick)
        any = true;
        host->publish_i64(host->userdata, keys[i][0].c_str(), static_cast<int64_t>(std::lround(d.x)));
        host->publish_i64(host->userdata, keys[i][1].c_str(), static_cast<int64_t>(std::lround(d.y)));
        host->publish_i64(host->userdata, keys[i][2].c_str(), d.alpha);
        host->publish_i64(host->userdata, keys[i][3].c_str(), d.scale);
    }
    host->publish_i64(host->userdata, "sshd.tr.on", any ? 1 : 0);
    host->publish_i64(host->userdata, "sshd.swipe.refused", refused ? 1 : 0);
}

// Before dev.38 (and the before/after measurement): the pad was a pan_zoom view; mirror its pan
// (the finger's offset from where the drag began) onto the right stick (full deflection at
// kStickTravelPx). A held drag poses the sword; a fast stroke is a quick deflection and release,
// which the game reads as a slash. The lift is recognised by the host's glide home; a stroke that
// starts while the last one still glides is not fed (the dev.38 root cause, sshd_stroke.h).
void LegacyPadMirror(Instance* inst, const EdenDsmodHostApi* host) {
    const auto& e = inst->ext;
    // pan of whichever stick pad the finger is on (one tick behind, host-clamped: unbounded)
    double px = 0, py = 0;
    static constexpr const char* kPanKeys[][2] = {
        {"@view_pan_x:stickpad_0", "@view_pan_y:stickpad_0"},
        {"@view_pan_x:stickpad_1", "@view_pan_y:stickpad_1"},
    };
    for (const auto& keys : kPanKeys) {
        const double x = host->get_f64(host->userdata, keys[0], 0.0);
        const double y = host->get_f64(host->userdata, keys[1], 0.0);
        if (std::fabs(x) + std::fabs(y) > std::fabs(px) + std::fabs(py)) {
            px = x;
            py = y;
        }
    }
    // A lifted finger glides the view home a fifth of the remaining way per tick (host
    // GlideViews): recognise that exact decay and release the stick at once instead of easing.
    const double prev = std::fabs(inst->drag_x0) + std::fabs(inst->drag_y0);
    const bool gliding = inst->dragging && prev > 2.0 &&
                         std::fabs(px) + std::fabs(py) < prev &&
                         std::fabs(px - inst->drag_x0 * 0.8) <= 0.02 * prev + 0.05 &&
                         std::fabs(py - inst->drag_y0 * 0.8) <= 0.02 * prev + 0.05;
    const bool held = std::fabs(px) + std::fabs(py) > 0.5 && !gliding && !inst->releasing;
    if (inst->diag.on) { // the stroke log (SSHD_DIAG): fed episodes, ticks a finger was not fed
        auto& d = inst->sdiag;
        if (std::fabs(px) + std::fabs(py) > 0.5 && !gliding && inst->releasing)
            ++inst->legacy_skip;
        if (held && !inst->dragging) {
            StrokeDiag(inst, host, true);
            d = {};
            d.open = true, d.legacy = true, d.id = ++inst->stroke_id;
            d.skipped = inst->legacy_skip;
            inst->legacy_skip = 0;
        }
        if (held) {
            ++d.s.ticks;
            d.s.peak = std::max(d.s.peak, std::min(1.0, std::hypot(px, py) / kStickTravelPx));
            d.s.len = std::max(d.s.len, std::hypot(px, py));
            StrokeDiagTick(inst, host, true);
        } else if (inst->dragging && d.open && d.post == 0) {
            d.post = kStrokePost;
        }
        StrokeDiag(inst, host, false);
    }
    if (gliding)
        inst->releasing = true;
    if (std::fabs(px) + std::fabs(py) <= 0.5)
        inst->releasing = false;
    inst->drag_x0 = px;
    inst->drag_y0 = py;
    inst->dragging = std::fabs(px) + std::fabs(py) > 0.5;
    if (!held) {
        // the stroke ended: with the L toggle on, one that never reached the stick (the game still
        // saw L) is replayed as a flick of its peak direction once L is up (LTick)
        const double pm = std::sqrt(inst->peak_x * inst->peak_x + inst->peak_y * inst->peak_y);
        if (inst->l_on && !inst->stroke_fed && pm >= kFlickMin && inst->seq_wait == 0 && inst->pend_n == 0) {
            const StickStep flick[] = {{static_cast<int16_t>(std::lround(inst->peak_x / pm * 32767)),
                                        static_cast<int16_t>(std::lround(inst->peak_y / pm * 32767)), 4},
                                       {0, 0, 2}};
            SeqQueue(inst, flick, 2, true);
        }
        inst->stroke_fed = false;
        inst->stroke_wait = 0;
        inst->peak_x = inst->peak_y = 0;
        return;
    }
    if (inst->seq_wait > 0 || inst->pend_n > 0) { // a spin sequence owns the stick words while it plays
        return;
    }
    // native pause / Collection / map open: do not feed the stick into the menus. dev.44: the
    // debounced controller state (MenuGate); dev.17..43 read the raw stack slot main+0x192D0E4 here,
    // so a stale 0x10 there dropped strokes in play too.
    if (inst->menu_open) {
        e.store_u32(e.userdata, kOffTtl, 0); // dev.48: and drop the vector fed before the menu opened
        return;
    }
    // finger right/up = stick right/up; the pan follows the finger in canvas px
    double vx = kPanSignX * px / kStickTravelPx, vy = py / kStickTravelPx;
    const double m = std::sqrt(vx * vx + vy * vy);
    if (m > 1.0) {
        vx /= m;
        vy /= m;
    }
    // L toggle on: the game routes the stick to the camera until L has been up kLSettle ticks
    if (inst->l_on && !LSettled(inst) && inst->stroke_wait < kLReleaseWait) {
        ++inst->stroke_wait;
        if (vx * vx + vy * vy > inst->peak_x * inst->peak_x + inst->peak_y * inst->peak_y) {
            inst->peak_x = vx;
            inst->peak_y = vy;
        }
        return;
    }
    inst->stroke_fed = true;
    ++inst->sdiag.s.fed;
    e.store_u32(e.userdata, kOffX, static_cast<uint32_t>(static_cast<int32_t>(std::lround(vx * 32767))));
    e.store_u32(e.userdata, kOffY, static_cast<uint32_t>(static_cast<int32_t>(std::lround(vy * 32767))));
    e.store_u32(e.userdata, kOffTtl, kTtl); // last: publishes the vector
}


// The stroke's peak vector so far (its late flick's direction).
void NotePeak(Instance* inst, double x, double y) {
    if (x * x + y * y > inst->peak_x * inst->peak_x + inst->peak_y * inst->peak_y) {
        inst->peak_x = x;
        inst->peak_y = y;
    }
}

// dev.45 (Thor dev.44: 2 of 11 swipes with the L lock on "silently dropped": the gesture ended, then
// neither "stick sequence staged" nor "refused swipe"). A stroke that waited for the game to see L
// up and got through only in its last tick or two before the lift (stroke 17..18 ticks long on the
// desktop, 16..17 on the Thor) wrote the stick 1..2 ticks plus the lift's short hold: too short for a
// slash, and with the stick fed it was no longer replayed as a flick. Every stroke now ends in one of:
//  - fed long enough: one "swipe N fed" line;
//  - its flick replayed (never fed, or fed fewer than kLateFed ticks after the L wait; the flick
//    continues the deflection already written, so it is one slash): "stick sequence M staged" follows
//    (or the late flick's own refusal), behind a sequence that still owns the stick if need be;
//  - one "refused swipe (<why>)" line: refused while it ran (logged then), too short for a flick,
//    or its flick could not play.
constexpr int kLateFed = 4; // ticks a stroke that waited for L must reach the stick to count as fed
void SwipeEnded(Instance* inst, const EdenDsmodHostApi* host, int fed, int& replay) {
    if (inst->stroke_refused)
        return; // logged when the stroke met the state
    const uint32_t id = inst->stroke_id;
    const bool waited = inst->stroke_wait > 0;
    const bool late = waited && fed > 0 && fed < kLateFed;
    const double pm = std::sqrt(inst->peak_x * inst->peak_x + inst->peak_y * inst->peak_y);
    char msg[112];
    if (fed > 0 && (!late || pm < kFlickMin)) {
        if (host->log != nullptr) {
            std::snprintf(msg, sizeof(msg), "sshd: swipe %u fed (%d ticks%s)", id, fed,
                          waited ? ", after the L release" : "");
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
        return;
    }
    if (pm < kFlickMin) {
        inst->gate = ReadGate(host);
        std::snprintf(msg, sizeof(msg), ": stroke %u, peak %.2f under %.2f, waited %d ticks", id, pm, kFlickMin,
                      inst->stroke_wait);
        Refuse(inst, "swipe", kWhyShort, msg); // no "Not now": nothing the game refused
        return;
    }
    const StickStep flick[] = {{static_cast<int16_t>(std::lround(inst->peak_x / pm * 32767)),
                                static_cast<int16_t>(std::lround(inst->peak_y / pm * 32767)), 4},
                               {0, 0, 2}};
    replay = 1;
    if (inst->seq_wait > 0 || inst->pend_n > 0) { // another sequence owns the stick: wait for it
        if (inst->flick_q) {
            inst->swipe_flash = std::max(kSwipeFlash, inst->diag.hold_ticks);
            inst->gate = ReadGate(host);
            std::snprintf(msg, sizeof(msg), ": stroke %u, late flick, stroke %u's flick already waits", id,
                          inst->flick_stroke);
            Refuse(inst, "swipe", kWhyBusy, msg);
            return;
        }
        inst->flick_q = true;
        inst->flick_steps[0] = flick[0];
        inst->flick_steps[1] = flick[1];
        inst->flick_q_wait = 0;
        inst->flick_stroke = id;
        if (host->log != nullptr) {
            std::snprintf(msg, sizeof(msg), "sshd: swipe %u flick waits for the stick (sequence %u in flight)", id,
                          inst->seq_id);
            host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
        return;
    }
    SeqQueue(inst, flick, 2, true); // LTick stages it once the game has seen L up
    inst->pend_stroke = id;
    if (host->log != nullptr) {
        if (late)
            std::snprintf(msg, sizeof(msg), "sshd: swipe %u fed only %d ticks after the L release: its flick replays",
                          id, fed);
        else
            std::snprintf(msg, sizeof(msg), "sshd: swipe %u flick replays (the stroke waited %d ticks for L)", id,
                          inst->stroke_wait);
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
    }
}

void StickMirror(void* instance, const EdenDsmodHostApi* host) {
    auto* inst = static_cast<Instance*>(instance);
    if (inst == nullptr || host == nullptr || !inst->mailbox || host->get_f64 == nullptr) {
        return;
    }
    const auto& e = inst->ext;
    e.store_u32(e.userdata, kOffMagic, kMbMagic);
    uint32_t hb = 0;
    if (e.load_u32(e.userdata, kOffHeartbeat, &hb)) {
        inst->heartbeat_still = hb == inst->last_heartbeat ? inst->heartbeat_still + 1 : 0;
        inst->last_heartbeat = hb;
    }
    if (inst->swipe_flash > 0)
        --inst->swipe_flash;
    if (inst->diag.legacy_pad) { // diagnostics: the dev.17..37 pad logic (the "before" of a measurement)
        LegacyPadMirror(inst, host);
        return;
    }
    // each pad's finger from its pan (PanStroke); a finger strokes one pad at a time
    static constexpr const char* kPads[][2] = {{"@view_pan_x:stickpad_0", "@view_pan_y:stickpad_0"},
                                               {"@view_pan_x:stickpad_1", "@view_pan_y:stickpad_1"}};
    sshd::PanStroke::Out f;
    int fi = -1;
    for (size_t i = 0; i < inst->pads.size(); ++i) {
        const auto o = inst->pads[i].Step(host->get_f64(host->userdata, kPads[i][0], 0.0),
                                          host->get_f64(host->userdata, kPads[i][1], 0.0));
        if (o.down && !f.down) {
            f = o;
            fi = static_cast<int>(i);
        }
    }
    // dev.39: where each pad's finger is (runtime 25; one tick behind, like the pan). An older
    // runtime answers neither key: the fallback NaN, so no finger, and the dev.38 trail.
    static constexpr const char* kTouch[][2] = {{"@view_touch_x:stickpad_0", "@view_touch_y:stickpad_0"},
                                                {"@view_touch_x:stickpad_1", "@view_touch_y:stickpad_1"}};
    std::array<std::array<double, 2>, 2> touch{};
    std::array<bool, 2> has{};
    if (inst->diag.TrailDue()) {
        for (size_t i = 0; i < touch.size(); ++i) {
            touch[i] = {host->get_f64(host->userdata, kTouch[i][0], NAN),
                        host->get_f64(host->userdata, kTouch[i][1], NAN)};
            has[i] = std::isfinite(touch[i][0]) && std::isfinite(touch[i][1]);
            auto& l = inst->land[i];
            if (has[i] && !l.on)
                l = {true, touch[i][0], touch[i][1]};
            else if (!has[i])
                l.on = false;
        }
    }
    const bool drag = f.down;
    const double fx = kStrokeAnchor[0] + f.dx, fy = kStrokeAnchor[1] + f.dy;
    const bool was = inst->stroke.Active();
    const sshd::StrokeVec v = inst->stroke.Step(drag, fx, fy, kStrokeAnchor[0], kStrokeAnchor[1]);
    auto& d = inst->sdiag;
    if (drag && !was) { // a new stroke
        StrokeDiag(inst, host, true);
        d = {};
        d.open = inst->diag.on;
        d.id = ++inst->stroke_id;
        inst->stroke_fed = false;
        inst->stroke_wait = 0;
        inst->peak_x = inst->peak_y = 0;
        inst->stroke_refused = false;
        // the finger trail starts where this finger landed (it may have rested a tick or two before
        // the pan moved the 1 px PanStroke needs)
        inst->trail_pad = fi;
        const auto& l = inst->land[static_cast<size_t>(fi)];
        inst->ftrail.Begin(l.on, l.x, l.y);
        if (l.on)
            d.lx = l.x, d.ly = l.y;
    }
    {
        const int tp = inst->trail_pad;
        const bool th = drag && tp >= 0 && has[static_cast<size_t>(tp)];
        inst->ftrail.Step(drag, th, th ? touch[static_cast<size_t>(tp)][0] : 0.0,
                          th ? touch[static_cast<size_t>(tp)][1] : 0.0);
    }
    inst->dragging = drag;
    StrokeDiagTick(inst, host, drag);
    if (inst->stroke.TakeEnded()) {
        d.s = inst->stroke.LastStats();
        d.post = kStrokePost;
        SwipeEnded(inst, host, d.s.fed, d.replay);
    }
    StrokeDiag(inst, host, false);
    // the trail: under the finger once this stroke has finger positions, else dev.38's
    const bool under = inst->ftrail.Used();
    inst->trail = under ? inst->ftrail.Trail() : inst->stroke.Trail();
    if (drag && !was && inst->trail_mode != (under ? 1 : 0) && host->log != nullptr) {
        inst->trail_mode = under ? 1 : 0;
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                  under ? "sshd: swipe trail under the finger (the runtime tells where it is: @view_touch_x/y)"
                        : "sshd: swipe trail from the minimap centre (no @view_touch_x/y from this runtime)");
    }
    d.trail = under ? 1 : 0;
    if (under) {
        d.fsamples = inst->ftrail.Samples();
        const auto hd = inst->ftrail.Head();
        d.hx = hd[0], d.hy = hd[1];
    }
    if (host->publish_i64 != nullptr)
        PublishTrail(inst, host);
    if (!v.feed)
        return;
    if (!drag && (!inst->stroke_fed || inst->stroke_refused))
        return; // the lift's short hold only follows a stroke that reached the stick
    if (inst->seq_wait > 0 || inst->pend_n > 0) { // a spin sequence owns the stick words while it plays
        if (drag) {
            d.spin = 1;
            NotePeak(inst, v.x, v.y); // dev.45: its flick follows the sequence (SwipeEnded)
        }
        return;
    }
    if (drag) {
        // the game cannot slash now: say so instead of silently doing nothing
        const int blocked = LBlocked(inst, host);
        const bool no_sword = host->get_i64 != nullptr && host->get_i64(host->userdata, "sshd.sword.has", 1) == 0;
        if ((blocked != 0 && blocked != 4 && (blocked == 6 || inst->l_block >= kLBlockTicks)) || no_sword) {
            inst->swipe_flash = std::max(kSwipeFlash, inst->diag.hold_ticks);
            if (!inst->stroke_refused) {
                // dev.43 (Thor RC: a "Not now" with no log line): one line per refused stroke, not
                // per sample, with the LBlocked code, its debounce count and the raw gate words
                inst->gate = ReadGate(host);
                char note[80];
                std::snprintf(note, sizeof(note), ": stroke %u, lblocked %d, l_block %d, no_sword %d", inst->stroke_id,
                              blocked, inst->l_block, no_sword ? 1 : 0);
                Refuse(inst, "swipe", no_sword ? kWhyNoSword : LBlockWhy(blocked), note);
                // dev.48: a stroke already fed leaves no vector behind (the helper would replay its
                // last one for up to kTtl passes): ttl 0 hands the stick back to the player at once
                e.store_u32(e.userdata, kOffTtl, 0);
            }
            inst->stroke_refused = true;
            d.gate = no_sword ? 10 : blocked;
            return;
        }
    }
    // L lock on: the game routes the stick to the camera until L has been up kLSettle ticks
    if (drag && inst->l_on && !LSettled(inst) && inst->stroke_wait < kLReleaseWait) {
        ++inst->stroke_wait;
        ++d.lwait;
        NotePeak(inst, v.x, v.y);
        return;
    }
    if (drag) {
        inst->stroke_fed = true;
        inst->stroke.Fed();
        NotePeak(inst, v.x, v.y);
    }
    e.store_u32(e.userdata, kOffX, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v.x * 32767))));
    e.store_u32(e.userdata, kOffY, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v.y * 32767))));
    // last: publishes the vector; the neutral after a lift lasts only a couple of passes, then the
    // player's own stick passes through again
    e.store_u32(e.userdata, kOffTtl, !drag && v.x == 0 && v.y == 0 ? 2u : kTtl);
}

// dev.46: the buttons' pressed look (the choice plank's lit rim + decide flash, ~150 ms, display only:
// no action waits for it). The runtime tells a module of no tap and clears the snapshot (the
// "@sel:" values included) before the module's call, so the countdown is the manifest's: the buttons
// whose tap stays on the page (Map / Fly Reset) select their id in the group "sshd.press" (a second tap
// clears it again), and derived values (gen_manifest press.*) hold this clock at the last change of
// "@sel:sshd.press" and light the id while the clock is less than sshd.press.hold ms past it. A
// page-changing button cannot show it: the runtime freezes the page being left at the tap;
// SSHD_DIAG_PRESS keeps any id lit for stills (sshd.press.diag), SSHD_DIAG_HOLD_TICKS lengthens the look.
// dev.48: the Ready map rim's lit look follows the module, not the tap: sshd.stab.lit is 1 for the
// look's length after SwordStab staged R3 (a refused tap shows the "Not now" pill only).
constexpr int kPressLookMs = 150;
void PressTick(Instance* inst, const EdenDsmodHostApi* host) {
    const int64_t ms = ClockMs(inst);
    const int hold = std::max(kPressLookMs, inst->diag.hold_ticks * 1000 / 60);
    host->publish_i64(host->userdata, "sshd.ms", 1 + ms);
    host->publish_i64(host->userdata, "sshd.press.hold", hold);
    host->publish_i64(host->userdata, "sshd.press.diag", inst->diag.press);
    host->publish_i64(host->userdata, "sshd.stab.lit", inst->stab_at >= 0 && ms - inst->stab_at < hold ? 1 : 0);
}

void Tick(void* instance, const EdenDsmodHostApi* host) {
    StickMirror(instance, host);
    auto* inst = static_cast<Instance*>(instance);
    if (inst != nullptr && host != nullptr && host->publish_i64 != nullptr) {
        LTick(inst, host);
        PressTick(inst, host);
    }
}

void Configure(void* instance, const EdenDsmodHostExtensions* ext) {
    auto* inst = static_cast<Instance*>(instance);
    if (inst == nullptr || ext == nullptr || ext->version != EDEN_DSMOD_EXT_VERSION ||
        ext->abi_hash != EDEN_DSMOD_EXT_HASH || ext->mailbox_address == 0 || ext->mailbox_size < 4096 ||
        ext->load_u32 == nullptr || ext->store_u32 == nullptr) {
        return;
    }
    if (inst->host.log != nullptr) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "sshd: stick mailbox %#llx size %llu",
                      static_cast<unsigned long long>(ext->mailbox_address),
                      static_cast<unsigned long long>(ext->mailbox_size));
        inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
    }
    inst->ext = *ext;
    inst->mailbox = true;
    // seed the pouch request epoch (the host bumps the word in place on pause / invalidate)
    uint32_t epoch = 0;
    if (ext->load_u32(ext->userdata, kOffEpoch, &epoch) && epoch == 0)
        ext->store_u32(ext->userdata, kOffEpoch, 1);
    uint32_t seq = 0;
    if (ext->load_u32(ext->userdata, kOffResSeq, &seq))
        inst->pouch_seq = seq; // a reconfigure keeps the helper's sequence
    ext->store_u32(ext->userdata, kOffReqSeq, inst->pouch_seq);
    inst->pouch_wait = 0;
    inst->pouch_dead = false;
    uint32_t res = 0;
    if (ext->load_u32(ext->userdata, kOffSeqRes, &res))
        inst->seq_id = res; // likewise for stick sequences
    ext->store_u32(ext->userdata, kOffSeqReq, inst->seq_id);
    inst->seq_wait = 0;
    inst->seq_dead = false;
    inst->pend_n = 0;
    inst->flick_q = false;
    inst->l_on = inst->l_suspend = false; // a host reload drops the L lock
    inst->l_engage = 0;
    ReleaseInputs(*ext);                  // and lets the helper's L, L3 / R3 / B and the stick go at once
    inst->dpad.Reset();                   // and every cached D-pad pane
    inst->hide_res.Reset();               // the top-HUD hide list starts empty
    inst->hide.Reset();
    inst->hide_bank = 0;
    ext->store_u32(ext->userdata, kOffHideSel, 0);
    ext->store_u32(ext->userdata, kOffHideTtl, 0);
    for (uint32_t o = kOffProbe; o <= kOffProbeFrames; o += 4) // the draw probe off, counters zero
        ext->store_u32(ext->userdata, o, 0);
    inst->diag.probe = 0;
    inst->diag.last_drawn = inst->diag.last_skipped = inst->diag.last_frames = 0;
}

EdenDsmodBool OnAction(void* instance, const char* action, int64_t argument) {
    auto* inst = static_cast<Instance*>(instance);
    if (inst == nullptr || action == nullptr)
        return EDEN_DSMOD_FALSE;
    // a Map page floor tab (display only, no game write): the next sample shows that floor
    if (std::string_view(action) == "floor_pick") {
        if (argument < 0 || argument >= kFloorTabs)
            return EDEN_DSMOD_FALSE;
        inst->floor_pick.store(kFloorSlotPick + static_cast<int>(argument));
        return EDEN_DSMOD_TRUE;
    }
    if (!inst->mailbox)
        return EDEN_DSMOD_FALSE;
    const std::string_view a(action);
    if (a == "spin_h" || a == "spin_v")
        return SpinAction(inst, a) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (a == "l_toggle")
        return LToggle(inst) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (a == "sword_stab")
        return SwordStab(inst) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    if (a == "shield_bash")
        return ShieldBash(inst) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    return PouchAction(inst, action, argument) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

// Returns the freed heap of a finished render to the OS: glibc keeps freed blocks of the worker's
// arena mapped (live desktop: about 30 MB after each map); bionic purges on request from API 26 (the
// android-24 build relies on its allocator unmapping the large render buffers when freed).
void ReleaseFreedMemory() {
#if defined(__GLIBC__)
    malloc_trim(0);
#elif defined(__ANDROID__) && defined(M_PURGE) && __ANDROID_API__ >= 26
    mallopt(M_PURGE, 0);
#endif
}

EdenDsmodBool LoadImage(void* instance, const EdenDsmodHostApi*, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    try {
        auto* inst = static_cast<Instance*>(instance);
        if (inst == nullptr || key == nullptr || sink == nullptr) {
            return EDEN_DSMOD_FALSE;
        }
        std::string_view k(key);
        if (k.substr(0, kKeyPrefix.size()) != kKeyPrefix) {
            return EDEN_DSMOD_FALSE;
        }
        k.remove_prefix(kKeyPrefix.size());
        // dev.33 (perf / memory budget): this runs on the host's module asset worker, never on
        // the sample / tick path (that only try-locks art_mutex, LabelReady), so a map render
        // delays only the images queued behind it. Builds of 20 ms or more are logged with the
        // module's cache size; after a large picture the freed render buffers go back to the OS.
        std::lock_guard lock(inst->art_mutex);
        const auto t0 = std::chrono::steady_clock::now();
        // dev.37: the view cone's pictures are tracked (sshd_cone.h: shown only once delivered)
        int cone_deg = -1;
        if (k.substr(0, 5) == "cone/") {
            cone_deg = std::atoi(std::string(k.substr(5)).c_str());
            inst->cone.Building(cone_deg);
            if (inst->diag.slow_cone > 0) // diagnostics: a handheld's slower asset worker
                std::this_thread::sleep_for(std::chrono::milliseconds(inst->diag.slow_cone));
        }
        auto img = inst->art->Build(k);
        if (!img || img->w <= 0 || img->h <= 0 || img->w > 4096 || img->h > 4096) {
            if (cone_deg >= 0)
                inst->cone.Delivered(cone_deg, false);
            if (inst->host.log != nullptr) {
                const std::string msg = "sshd image " + std::string(key) + ": " + inst->art->LastError();
                inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_WARNING, msg.c_str());
            }
            return EDEN_DSMOD_FALSE;
        }
        const int w = img->w, h = img->h;
        const auto rgba = img->Rgba8();
        img.reset(); // the float picture goes before the host copies the bytes
        sink(receiver, static_cast<uint32_t>(w), static_cast<uint32_t>(h), rgba.data(), rgba.size());
        inst->cone.Bytes(rgba.size());
        if (cone_deg >= 0)
            inst->cone.Delivered(cone_deg, true);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const bool large = static_cast<size_t>(w) * static_cast<size_t>(h) >= (1u << 20);
        if (large)
            ReleaseFreedMemory();
        if (ms >= 20 && inst->host.log != nullptr) {
            char msg[200];
            std::snprintf(msg, sizeof(msg), "sshd: image %.120s built in %lld ms (%dx%d), art cache %zu kB", key,
                          static_cast<long long>(ms), w, h, inst->art->CacheBytes() / 1024);
            inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_INFO, msg);
        }
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    Configure, OnAction, LoadImage,
};

const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    kTitleId,
    "Skyward Sword HD companion",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    Tick,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(kModule)
DSMOD_SDK_EXPORT_EXTENSIONS(kExtensions)
