// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstring>
#include <map>
#include <string>

#include "core/mods/dsmod_module_abi.h"

int main() {
    struct State {
        uint16_t packed = 0;
        uint32_t scene = 0;
        uint8_t stage = 'F';
        uint64_t fail = 0;
        std::map<std::string, int64_t> values;
    } s;
    EdenDsmodHostApi host{};
    host.userdata = &s;
    host.main_base = 0x80004000;
    constexpr uint64_t word = 0x80004000 + 0x5AEAD54 + 0xA50;
    constexpr uint64_t stage = 0x80004000 + 0x5AEAD54 + 0x531C;
    constexpr uint64_t scene = 0x80004000 + 0x182A7F4;
    host.read_memory = [](void* p, uint64_t a, void* out, size_t n) -> EdenDsmodBool {
        const auto& s = *static_cast<State*>(p);
        if (a == s.fail) return EDEN_DSMOD_FALSE;
        if (a == word && n == 2) std::memcpy(out, &s.packed, n);
        else if (a == stage && n == 1) std::memcpy(out, &s.stage, n);
        else if (a == scene && n == 4) std::memcpy(out, &s.scene, n);
        else return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    };
    host.publish_i64 = [](void* p, const char* key, int64_t v) {
        static_cast<State*>(p)->values[key] = v;
    };
    const auto* api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(api);
    auto check = [&](int value, bool valid) {
        api->sample(nullptr, &host);
        assert(s.values.at("sshd.crystals.valid") == valid);
        assert(s.values.at("sshd.crystals") == (valid ? value : 0));
    };
    // Recorded fixture words plus zero, two-digit boundary and every unrelated bit set.
    for (auto [packed, count] : {std::pair{0x1280, 80}, {0x1080, 16}, {0x1088, 17}, {0x0428, 5},
                                {0x0000, 0}, {0x0050, 10}, {0xFC07, 0}, {0xFE87, 80}}) {
        s.packed = packed;
        check(count, true);
    }
    // A failed read must clear a previously published value immediately.
    for (uint64_t address : {word, stage, scene}) {
        s.fail = 0; check(80, true);
        s.fail = address; check(0, false);
    }
    s.fail = 0;
    s.packed = 81 << 3; check(0, false);
    s.packed = 127 << 3; check(0, false);
    s.packed = 80 << 3;
    s.scene = 0x53656C74; check(0, false); // title retains the old live save
    s.scene = 0; s.stage = 0; check(0, false);
    s.stage = 'F'; check(80, true);
    host.read_memory = nullptr; check(0, false);
}
