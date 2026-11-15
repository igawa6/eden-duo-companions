// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstring>
#include <map>
#include <string>

#include "core/mods/dsmod_module_abi.h"

namespace {
// Native A-button placements, hero in-memory scratch, 2026-11-08/09.
struct Placement {
    const char* stage;
    uint32_t base, mask;
    float xyz[3];
};
constexpr Placement kPlacements[] = {
    {"F020", 0x34, 0x537C, {141017.171875f, -20000.0f, -105066.4140625f}},
    {"F301", 0x430, 0x538D, {13791.9619140625f, 5500.0f, 573.4752807617188f}},
    {"F302", 0x250, 0x5385, {3477.343505859375f, -9582.046875f, 4404.73828125f}},
    {"F303", 0x2C8, 0x5387, {4047.75f, -9661.7333984375f, -27.55694580078125f}},
};
struct State {
    const Placement* placement = nullptr;
    uint8_t mask = 0;
    bool readable = true;
    std::string stage;
    std::map<std::string, int64_t> ints;
    std::map<std::string, double> floats;
};
}

int main() {
    State s;
    EdenDsmodHostApi h{};
    h.userdata = &s;
    h.read_memory = [](void* p, uint64_t addr, void* out, size_t n) -> EdenDsmodBool {
        const auto& s = *static_cast<State*>(p);
        constexpr uint64_t slot = 0x5AEAD54;
        if (!s.readable) return EDEN_DSMOD_FALSE;
        if (addr == slot + 0x531C && n == 8) {
            std::memset(out, 0, n);
            std::memcpy(out, s.stage.data(), s.stage.size());
        } else if (addr == slot + s.placement->mask && n == 1) {
            std::memcpy(out, &s.mask, n);
        } else if (addr == slot + s.placement->base && n == 60) {
            // Each slot has the same recorded position, so every active bit can be checked.
            for (int i = 0; i < 5; ++i)
                std::memcpy(static_cast<char*>(out) + 12 * i, s.placement->xyz, 12);
        } else return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    };
    h.publish_i64 = [](void* p, const char* k, int64_t v) { static_cast<State*>(p)->ints[k] = v; };
    h.publish_f64 = [](void* p, const char* k, double v) { static_cast<State*>(p)->floats[k] = v; };
    h.publish_text = [](void*, const char*, const char*) {};
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api);
    for (const auto& placement : kPlacements) {
        s.placement = &placement;
        s.stage = placement.stage;
        s.readable = true;
        for (uint8_t mask : {0, 1, 0x15, 0x1F}) {
            s.mask = mask;
            api->sample(nullptr, &h);
            int count = 0;
            for (int i = 0; i < 5; ++i) {
                const std::string key = "sshd.bc" + std::to_string(i);
                const bool on = (mask >> i & 1) != 0;
                assert(s.ints[key + ".on"] == on);
                assert(s.floats[key + ".u"] == placement.xyz[0]);
                assert(s.floats[key + ".v"] == -placement.xyz[2]);
                count += on;
            }
            assert(s.ints["sshd.bc.n"] == count);
        }
        s.readable = false;
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bc.n"] == 0);
        for (int i = 0; i < 5; ++i) assert(s.ints["sshd.bc" + std::to_string(i) + ".on"] == 0);
        s.readable = true;
        s.mask = 1;
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bc.n"] == 1);
        s.stage = "F999";
        api->sample(nullptr, &h);
        assert(s.ints["sshd.bc.n"] == 0);
        for (int i = 0; i < 5; ++i) assert(s.ints["sshd.bc" + std::to_string(i) + ".on"] == 0);
    }
}
