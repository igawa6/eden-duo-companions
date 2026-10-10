// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The whole Pokémon module (0100000011D90000.cpp, or the Pearl build) through its exported C ABI,
// with a host that has no game memory and no romfs: what it publishes before a game is readable,
// and that every action needing the field or a battle is refused instead of acting on a default
// snapshot. Needs no game data.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
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

} // namespace

int main() {
    Run(false);
    Run(true);
    std::puts("lp module tests passed");
    return 0;
}
