// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/dsmod_module_abi.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    constexpr char build[] = "8EA928B6AA10C84F9E69129C45BCAAAF00000000000000000000000000000000";
    const auto* module = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION,
                                              EDEN_DSMOD_MODULE_ABI_HASH);
    assert(module && module->supports_build(build));
    assert(!module->supports_build(nullptr));
    assert(!module->supports_build("8EA928B6AA10C84F"));
    assert(!module->supports_build((std::string(build) + "0").c_str()));
    auto nonhex = std::string(build);
    nonhex.back() = 'G';
    assert(!module->supports_build(nonhex.c_str()));
    EdenDsmodHostApi host{};
    host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    host.struct_size = sizeof(host);
    host.title_id = UINT64_C(0x0100A0F00DA68000);
    unsigned reads{};
    host.userdata = &reads;
    host.read_memory = [](void* p, uint64_t, void*, size_t) -> EdenDsmodBool {
        ++*static_cast<unsigned*>(p);
        return EDEN_DSMOD_FALSE;
    };
    host.write_memory = [](void* p, uint64_t, const void*, size_t) -> EdenDsmodBool {
        ++*static_cast<unsigned*>(p);
        return EDEN_DSMOD_FALSE;
    };
    host.read_romfs = [](void* p, const char*, uint64_t, void*, size_t) -> size_t {
        ++*static_cast<unsigned*>(p);
        return 0;
    };
    for (unsigned i = 0; i < 32; ++i) {
        unsigned byte{};
        std::sscanf(build + i * 2, "%2x", &byte);
        host.build_id[i] = byte;
    }
    // The accepted build creates without reading guest memory or romfs.
    void* instance = module->create(&host, "{}");
    assert(instance && reads == 0);
    module->destroy(instance);
    // Every byte matters, including the former unchecked suffix and zero padding.
    for (unsigned i = 0; i < 32; ++i) {
        std::string wrong{build};
        wrong[i * 2] = wrong[i * 2] == '0' ? '1' : '0';
        assert(!module->supports_build(wrong.c_str()));
        host.build_id[i] ^= 1;
        assert(!module->create(&host, "{}"));
        host.build_id[i] ^= 1;
        assert(reads == 0);
    }
    std::puts("MS module: exact build accepted; 32 byte mismatches refused before guest IO");
}
