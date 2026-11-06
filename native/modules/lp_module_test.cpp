// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The whole Pokémon module (0100000011D90000.cpp, or the Pearl build) through its exported C ABI.
//   1. With a host that has no game memory and no romfs: what it publishes before a game is
//      readable, and that every action needing the field or a battle is refused instead of
//      acting on a default snapshot. Needs no game data.
//   2. With a host backed by a small fake memory image (the FakeMemory pattern from
//      0100000011D90000_test.cpp): Tick()'s stale-sample menu restore (0100000011D90000.cpp
//      ~211-230) -- once a battle has parked the native menus and samples then stop arriving (the
//      companion window detached, owner issue 2026-10-23 #3 / companions #10's interim
//      workaround), Tick() alone -- never Sample() again -- must put them back within a bounded
//      number of ticks. A live aux-detach/reattach check in the harness was attempted for this
//      pass and not reached (see games/pokemon-luminescent-platinum/STATUS.md); this is the
//      fallback the task brief asked for in that case.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "lp_live.h"
#include "lp_profile.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(std::uint32_t version, std::uint64_t hash);
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(std::uint32_t version, std::uint64_t hash);

namespace {

struct FakeHost {
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, std::string> texts;
    std::uint64_t tick = 0;
    int writes = 0;
};

EdenDsmodHostApi MakeHost(FakeHost& h, bool matching_build) {
    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(EdenDsmodHostApi);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &h;
    api.title_id = lp_profile::Active.title_id;
    const auto hex = lp_profile::Active.build_id;
    for (std::size_t i = 0; i < sizeof(api.build_id); ++i)
        api.build_id[i] = static_cast<std::uint8_t>(std::stoi(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
    if (!matching_build)
        api.build_id[0] ^= 0xFF;
    api.main_base = 0x80000000;
    api.main_size = 0x5000000;
    api.get_tick = [](void* p) { return static_cast<FakeHost*>(p)->tick; };
    api.read_memory = [](void*, std::uint64_t, void*, size_t) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
    api.write_memory = [](void* p, std::uint64_t, const void*, size_t) -> EdenDsmodBool {
        ++static_cast<FakeHost*>(p)->writes;
        return EDEN_DSMOD_FALSE;
    };
    api.publish_i64 = [](void* p, const char* name, std::int64_t v) { static_cast<FakeHost*>(p)->ints[name] = v; };
    api.publish_text = [](void* p, const char* name, const char* v) { static_cast<FakeHost*>(p)->texts[name] = v; };
    api.read_romfs = [](void*, const char*, std::uint64_t, void*, size_t) -> size_t { return 0; };
    api.get_i64 = [](void*, const char*, std::int64_t fallback) { return fallback; };
    api.get_f64 = [](void*, const char*, double fallback) { return fallback; };
    return api;
}

void Run(bool matching_build) {
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext && api->title_id == lp_profile::Active.title_id);
    assert(std::strstr(api->name, lp_profile::Active.title_id == UINT64_C(0x010018E011D92000) ? "Shining Pearl"
                                                                                              : "Brilliant Diamond"));
    FakeHost h;
    const EdenDsmodHostApi host = MakeHost(h, matching_build);
    void* m = api->create(&host, nullptr);
    assert(m);
    for (int k = 0; k < 3; ++k) {
        ++h.tick;
        h.ints.clear();
        h.texts.clear();
        api->sample(m, &host);
        api->tick(m, &host);
    }
    assert(h.ints.at("lp.supported") == (matching_build ? 1 : 0));
    if (matching_build) {
        assert(h.ints.at("lp.lumi") == 0 && h.ints.at("lp.catalog") == 0);
        assert(h.texts.count("lp.t.toast_cant_now")); // the string table is published every sample
#if !defined(LP_DEBUG_KEYS) || !LP_DEBUG_KEYS
        // harness / console values stay out of a release build
        for (const char* k : {"lp.ready", "lp.delta", "lp.guest", "lp.lang", "lp.drive.busy"})
            assert(!h.ints.count(k));
#endif
    } else {
        assert(h.ints.size() == 1); // nothing else before the build matches
    }
    // No readable game: every battle and field action is refused, and nothing is written.
    for (const char* a : {"move", "command", "battleview", "battletab", "battlefocus", "battlecontinue", "ballkey",
                          "balluse", "targetconfirm", "fieldview", "select", "swap1", "moveswap1", "party_move",
                          "bag_use", "bag_open", "bag_target", "bag_move", "field_waza", "fly", "map_fly",
                          "dex_form", "dex_evo", "dex_area", "pkt_touch", "pkt_full", "apppick"})
        for (std::int64_t arg : {0, 1, 3})
            assert(ext->on_action(m, a, arg) == EDEN_DSMOD_FALSE);
    assert(h.writes == 0);
    api->destroy(m);
}

// A flat fake address space: page-granular byte map. Same shape as 0100000011D90000_test.cpp's
// FakeMemory (a local copy: the two test binaries are separate translation units/targets).
struct FakeMemory {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void W64(std::uint64_t a, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void W32(std::uint64_t a, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void W32F(std::uint64_t a, float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        W32(a, bits);
    }
    void W8(std::uint64_t a, std::uint8_t v) { bytes[a] = v; }
    bool Read(std::uint64_t a, void* out, std::size_t n) const {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            auto it = bytes.find(a + i);
            if (it == bytes.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) {
        auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) bytes[a + i] = p[i];
        return true;
    }
};

// userdata for MakeMemoryHost: the ABI's function pointers cannot capture, so tick travels with
// the memory, not as a separate reference (the same shape as lp_module_test.cpp's own FakeHost).
struct FakeMemoryHost {
    FakeMemory mem;
    std::uint64_t tick = 0;
    std::map<std::string, std::int64_t> ints; // Safari tests read these back; the park/restore
                                               // tests above only inspect raw memory bytes
    std::map<std::string, std::string> texts;
};

EdenDsmodHostApi MakeMemoryHost(FakeMemoryHost& h) {
    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(EdenDsmodHostApi);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &h;
    api.title_id = lp_profile::Active.title_id;
    const auto hex = lp_profile::Active.build_id;
    for (std::size_t i = 0; i < sizeof(api.build_id); ++i)
        api.build_id[i] = static_cast<std::uint8_t>(std::stoi(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
    api.main_base = 0x80000000;
    api.main_size = 0x5000000;
    api.get_tick = [](void* p) { return static_cast<FakeMemoryHost*>(p)->tick; };
    api.read_memory = [](void* p, std::uint64_t a, void* out, std::size_t n) -> EdenDsmodBool {
        return static_cast<FakeMemoryHost*>(p)->mem.Read(a, out, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    };
    api.write_memory = [](void* p, std::uint64_t a, const void* in, std::size_t n) -> EdenDsmodBool {
        return static_cast<FakeMemoryHost*>(p)->mem.Write(a, in, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    };
    api.publish_i64 = [](void* p, const char* name, std::int64_t v) { static_cast<FakeMemoryHost*>(p)->ints[name] = v; };
    api.publish_text = [](void* p, const char* name, const char* v) { static_cast<FakeMemoryHost*>(p)->texts[name] = v; };
    api.read_romfs = [](void*, const char*, std::uint64_t, void*, size_t) -> size_t { return 0; };
    api.get_i64 = [](void*, const char*, std::int64_t fallback) { return fallback; };
    api.get_f64 = [](void*, const char*, double fallback) { return fallback; };
    return api;
}

// Writes a minimal BattleViewCore -> UISystem -> {action, waza, ball, safari} canvas layout a real
// battle would have (same addresses/offsets as 0100000011D90000_test.cpp TestDeltaAndBattle), all
// four lists unparked (TransitionType 0, distinct sentinel anchors) so HideBattleMenus(true) has
// real originals to save and HideBattleMenus(false) has something to tell apart from "never
// parked". `safari`/`min`/`max` select a normal (false, 0, 3) or Safari (true, 5, 8) battle --
// companions #10 fix (b).
void WriteBattleLayout(FakeMemory& mem, bool safari = false, int min = 0, int max = 3) {
    using namespace lp_live;
    constexpr std::uint64_t Main = 0x80000000;
    for (const auto& f : off::Fingerprints) mem.W64(Main + f.slot, Main + f.target);
    const std::uint64_t method = 0x1000000, klass = 0x1001000, statics = 0x1002000, bvc = 0x1003000;
    const std::uint64_t ui = 0x1004000, action = 0x1005000, waza = 0x1006000;
    const std::uint64_t ball = 0x1007000, balls = 0x1008000, safariw = 0x1009000;
    mem.W64(Main + off::BattleViewCoreGetInstance, method);
    mem.W64(method + off::MethodKlass, klass);
    mem.W64(klass + off::KlassStatics, statics);
    mem.W64(statics, bvc);
    mem.W64(bvc + off::BvcUiSystem, ui);
    mem.W64(bvc + off::BvcViewSystem, 0);
    mem.W64(ui + off::UiActionList, action);
    mem.W64(ui + off::UiWazaList, waza);
    mem.W64(ui + off::UiPokeBallList, ball);
    mem.W64(ui + off::UiSafariBall, safariw);
    mem.W64(ball + off::BallListBalls, balls);
    mem.W32(balls + off::ListSize, 0);
    int n = 0;
    for (std::uint64_t list : {action, waza, ball, safariw}) {
        for (std::uint64_t o = 0; o < 0x70; ++o) mem.W8(list + o, 0); // the canvas object is mapped as a whole
        mem.W32(list + off::CanvasMaxIndex, 0);
        mem.W32(list + off::CanvasCurrentIndex, list == action ? min : 0);
        mem.W8(list + off::CanvasIsFocus, list == action ? 1 : 0);
        mem.W8(list + off::CanvasIsShow, list == action ? 1 : 0);
        mem.W8(list + off::CanvasTransitionType, 0); // unparked (not the 1/2400,2400 parked shape)
        mem.W32F(list + off::CanvasHideAnchor, 1000.0f + static_cast<float>(n)); // distinct originals
        mem.W32F(list + off::CanvasShowAnchor, 2000.0f + static_cast<float>(n));
        mem.W64(list + off::CanvasGroup, 0);
        ++n;
    }
    mem.W32(action + off::ActionMinIndex, min);
    mem.W32(action + off::ActionMaxIndex, max);
    mem.W8(action + off::ActionIsSafari, safari ? 1 : 0);
    mem.W64(Main + off::PlayerWorkTypeInfo, 0); // no PlayerWork: field/player stay unresolved, fine
}

bool AllListsParked(FakeMemory& mem) {
    using namespace lp_live;
    for (std::uint64_t list : {0x1005000ull, 0x1006000ull, 0x1007000ull, 0x1009000ull}) {
        std::uint32_t type = 0, hide = 0, show = 0;
        if (!mem.Read(list + off::CanvasTransitionType, &type, 1)) return false;
        if (!mem.Read(list + off::CanvasHideAnchor, &hide, 4)) return false;
        if (!mem.Read(list + off::CanvasShowAnchor, &show, 4)) return false;
        float hidef, showf;
        std::memcpy(&hidef, &hide, 4);
        std::memcpy(&showf, &show, 4);
        if (type != 1 || hidef != 2400.0f || showf != 2400.0f) return false;
    }
    return true;
}

bool AllListsRestored(FakeMemory& mem) {
    int n = 0;
    for (std::uint64_t list : {0x1005000ull, 0x1006000ull, 0x1007000ull, 0x1009000ull}) {
        std::uint32_t hide = 0, show = 0;
        if (!mem.Read(list + lp_live::off::CanvasHideAnchor, &hide, 4)) return false;
        if (!mem.Read(list + lp_live::off::CanvasShowAnchor, &show, 4)) return false;
        float hidef, showf;
        std::memcpy(&hidef, &hide, 4);
        std::memcpy(&showf, &show, 4);
        if (hidef != 1000.0f + n || showf != 2000.0f + n) return false;
        ++n;
    }
    return true;
}

// Tick() must restore the parked battle menus on its own, without a further Sample(), once
// samples have been stale for a while -- the companion window can be hidden mid-battle (aux
// detached: the host keeps calling tick() because the module has on_action, but stops calling
// sample()) and the top-screen command/move/ball lists must come back for native play.
void TestStaleSampleRestoresParkedMenus() {
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext);
    (void)ext;
    FakeMemoryHost h;
    WriteBattleLayout(h.mem);
    const EdenDsmodHostApi host = MakeMemoryHost(h);
    void* m = api->create(&host, nullptr);
    assert(m);

    // A few sampled ticks in battle: the module parks the native menus every battle sample
    // (0100000011D90000.cpp: "the menus are always parked").
    assert(!AllListsParked(h.mem));
    for (int k = 0; k < 3; ++k) {
        ++h.tick;
        api->sample(m, &host);
        api->tick(m, &host);
    }
    assert(AllListsParked(h.mem));
    assert(!AllListsRestored(h.mem));

    // Samples stop (aux detached): only tick() keeps running, with the host tick counter still
    // advancing. Nothing changes for the first 10 stale ticks (last_sample_tick + 10 guard).
    for (int k = 0; k < 10; ++k) {
        ++h.tick;
        api->tick(m, &host);
    }
    assert(AllListsParked(h.mem));
    assert(!AllListsRestored(h.mem));

    // Past the guard, Tick() restores the saved originals on its own -- no Sample() since the
    // battle was entered.
    bool restored = false;
    for (int k = 0; k < 40 && !restored; ++k) {
        ++h.tick;
        api->tick(m, &host);
        restored = AllListsRestored(h.mem);
    }
    assert(restored);
    assert(!AllListsParked(h.mem));
    api->destroy(m);
}

// companions #10 fix (b): the Safari command set (Ball/Bait/Mud/Run, absolute indices 5..8) drives
// through the same generalized "command" action and cmd{k}.available/.picked slots as the normal
// 4-command set, and UiSafariBall parks/restores alongside the other three menus (already covered
// by WriteBattleLayout/AllListsParked/AllListsRestored above, which this reuses with safari=true).
void TestSafariCommands() {
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext);
    {
        FakeMemoryHost h;
        WriteBattleLayout(h.mem, /*safari=*/true, /*min=*/5, /*max=*/8);
        const EdenDsmodHostApi host = MakeMemoryHost(h);
        void* m = api->create(&host, nullptr);
        assert(m);
        for (int k = 0; k < 3; ++k) {
            ++h.tick;
            api->sample(m, &host);
            api->tick(m, &host);
        }
        assert(AllListsParked(h.mem)); // UiSafariBall included: parked the same as action/waza/ball
        assert(h.ints.at("lp.bt.safari") == 1);
        for (int k = 0; k < 4; ++k)
            assert(h.ints.at("lp.bt.cmd" + std::to_string(k) + ".available") == 1); // 5..8 is 4 wide too
        assert(h.ints.at("lp.bt.cmd.index") == 0); // relative: native CurrentIndex(5) - action_min(5)
        // no PlayerWork class statics yet: the count line stays empty (the window's need_bind)
        assert(h.ints.at("lp.bt.safari.balls") == -1);
        assert(h.texts.at("lp.bt.safari.text").empty());
        // PlayerWork.SafariBallNum (a class static): the game's count line with the number in
        // the digit tag's place (no romfs here, so the English row: "Remaining: {n}")
        constexpr std::uint64_t Main = 0x80000000, pw_klass = 0x100A000, pw_statics = 0x100B000;
        h.mem.W64(Main + lp_live::off::PlayerWorkClassTypeInfo, pw_klass);
        h.mem.W64(pw_klass + lp_live::off::KlassStatics, pw_statics);
        h.mem.W32(pw_statics + lp_live::off::PwSafariBallNum, 30);
        ++h.tick;
        api->sample(m, &host);
        api->tick(m, &host);
        assert(h.ints.at("lp.bt.safari.balls") == 30);
        assert(h.texts.at("lp.bt.safari.text") == "Remaining: 30");
        // Out of range for this battle's live bounds: refused, including adjacent normal-mode
        // values. (Checked first, in the same instance: refusals never consume the one-drive-at-
        // a-time slot, so they do not disturb the in-range checks below.)
        for (std::int64_t arg : {0, 1, 3, 4, 9, 99})
            assert(ext->on_action(m, "command", arg) == EDEN_DSMOD_FALSE);
        api->destroy(m);
    }
    // In range (5..8): accepted, including Safari's own slot 0 (Ball) -- unlike normal mode's
    // Fight, none of the Safari commands opens a sub-list, so there is nothing to exclude. A fresh
    // module per value: a successful "command" occupies the module's one-drive-at-a-time slot, so
    // reusing one instance would make every value after the first look refused for the wrong reason.
    for (std::int64_t arg : {5, 6, 7, 8}) {
        FakeMemoryHost h;
        WriteBattleLayout(h.mem, /*safari=*/true, /*min=*/5, /*max=*/8);
        const EdenDsmodHostApi host = MakeMemoryHost(h);
        void* m = api->create(&host, nullptr);
        assert(m);
        for (int k = 0; k < 3; ++k) {
            ++h.tick;
            api->sample(m, &host);
            api->tick(m, &host);
        }
        assert(ext->on_action(m, "command", arg) == EDEN_DSMOD_TRUE);
        api->destroy(m);
    }
}

// Regression: an ordinary (non-Safari) battle keeps today's behaviour exactly -- Fight (0) is
// still refused through "command" (it drives via "battleview" instead), 1..3 still accepted, nothing
// outside 0..3 accepted, lp.bt.safari is 0, and cmd.index is unaffected by being made relative
// (action_min is 0 in a normal battle, so current - action_min == current, no behaviour change).
void TestNormalCommandsUnaffected() {
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(api && ext);
    {
        FakeMemoryHost h;
        WriteBattleLayout(h.mem, /*safari=*/false, /*min=*/0, /*max=*/3);
        const EdenDsmodHostApi host = MakeMemoryHost(h);
        void* m = api->create(&host, nullptr);
        assert(m);
        for (int k = 0; k < 3; ++k) {
            ++h.tick;
            api->sample(m, &host);
            api->tick(m, &host);
        }
        assert(h.ints.at("lp.bt.safari") == 0);
        for (int k = 0; k < 4; ++k)
            assert(h.ints.at("lp.bt.cmd" + std::to_string(k) + ".available") == 1);
        assert(h.ints.at("lp.bt.cmd.index") == 0); // native CurrentIndex(0) - action_min(0)
        for (std::int64_t arg : {0, 4, 5, 8, 99}) // 0 = Fight, still excluded
            assert(ext->on_action(m, "command", arg) == EDEN_DSMOD_FALSE);
        api->destroy(m);
    }
    // One fresh module per accepted value, same reason as TestSafariCommands above.
    for (std::int64_t arg : {1, 2, 3}) {
        FakeMemoryHost h;
        WriteBattleLayout(h.mem, /*safari=*/false, /*min=*/0, /*max=*/3);
        const EdenDsmodHostApi host = MakeMemoryHost(h);
        void* m = api->create(&host, nullptr);
        assert(m);
        for (int k = 0; k < 3; ++k) {
            ++h.tick;
            api->sample(m, &host);
            api->tick(m, &host);
        }
        assert(ext->on_action(m, "command", arg) == EDEN_DSMOD_TRUE);
        api->destroy(m);
    }
}

} // namespace

int main() {
    Run(false);
    Run(true);
    TestStaleSampleRestoresParkedMenus();
    TestSafariCommands();
    TestNormalCommandsUnaffected();
    std::puts("lp module tests passed");
    return 0;
}
