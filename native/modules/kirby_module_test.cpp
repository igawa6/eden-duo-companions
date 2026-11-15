// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// The Kirby module's ABI table, its build gate and the actions it accepts, with a host that has no game: no
// memory, an empty romfs. Built twice: the release module (KIRBY_DEV 0) refuses the research actions, the
// development one (KIRBY_DEV 1) accepts them.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#ifndef KIRBY_DEV
#define KIRBY_DEV 0
#endif

namespace {

int failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                 \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr std::uint64_t Title = 0x01006B601380E000ull;
// the game's 1.0.0 build (FA24EA3C3A512C07.json, the manifest's build_ids)
constexpr const char* Build = "FA24EA3C3A512C072A2062556C9C9692B3E6ACE1000000000000000000000000";

EdenDsmodBool NotMapped(void*, std::uint64_t, std::uint64_t) { return EDEN_DSMOD_FALSE; }
EdenDsmodBool NoRead(void*, std::uint64_t, void*, std::size_t) { return EDEN_DSMOD_FALSE; }
std::size_t NoRomfs(void*, const char*, std::uint64_t, void*, std::size_t) { return 0; }
const std::uint8_t* NoPointer(void*, std::uint64_t, std::size_t) { return nullptr; }
EdenDsmodBool NoWrite(void*, std::uint64_t, const void*, std::size_t) { return EDEN_DSMOD_FALSE; }
std::uint64_t Zero(void*) { return 0; }
void Log(void*, std::uint32_t, const char*) {}
void PubI64(void*, const char*, std::int64_t) {}
void PubF64(void*, const char*, double) {}
void PubText(void*, const char*, const char*) {}
void PubAddress(void*, const char*, std::uint64_t) {}
std::int64_t GetI64(void*, const char*, std::int64_t fallback) { return fallback; }
double GetF64(void*, const char*, double fallback) { return fallback; }
const char* GetText(void*, const char*) { return ""; }

EdenDsmodHostApi MakeHost() {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.struct_size = sizeof(EdenDsmodHostApi);
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.title_id = Title;
    h.main_base = 0x80000000;
    h.main_size = 0x8000000;
    h.get_tick = Zero;
    h.get_heap_begin = Zero;
    h.get_heap_end = Zero;
    h.is_mapped = NotMapped;
    h.read_memory = NoRead;
    h.get_read_pointer = NoPointer;
    h.write_memory = NoWrite;
    h.read_romfs = NoRomfs;
    h.log = Log;
    h.publish_i64 = PubI64;
    h.publish_f64 = PubF64;
    h.publish_text = PubText;
    h.publish_address = PubAddress;
    h.get_i64 = GetI64;
    h.get_f64 = GetF64;
    h.get_text = GetText;
    return h;
}

} // namespace

int main() {
    const auto* m = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    if (!m) {
        std::fprintf(stderr, "no module table\n");
        return 1;
    }
    CHECK(m->title_id == Title);
    CHECK(m->struct_size == sizeof(EdenDsmodModuleApi));
    CHECK(m->supports_build && m->create && m->destroy && m->sample && m->tick);
    CHECK(eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION + 1, EDEN_DSMOD_MODULE_ABI_HASH) == nullptr);

    // the build gate: the one 1.0.0 build id, whole and upper case
    CHECK(m->supports_build(Build) != 0);
    CHECK(m->supports_build(nullptr) == 0);
    CHECK(m->supports_build("FA24EA3C3A512C07") == 0);
    CHECK(m->supports_build("fa24ea3c3a512c072a2062556c9c9692b3e6ace1000000000000000000000000") == 0);
    CHECK(m->supports_build("0000000000000000000000000000000000000000000000000000000000000000") == 0);

    // create: another title, another ABI or no romfs reader is refused
    auto host = MakeHost();
    host.title_id ^= 1;
    CHECK(m->create(&host, "{}") == nullptr);
    host = MakeHost();
    host.abi_hash ^= 1;
    CHECK(m->create(&host, "{}") == nullptr);
    host = MakeHost();
    host.read_romfs = nullptr;
    CHECK(m->create(&host, "{}") == nullptr);
    CHECK(m->create(nullptr, "{}") == nullptr);

    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    CHECK(ext && ext->on_action && ext->load_image);
    host = MakeHost();
    void* inst = m->create(&host, "{}");
    CHECK(inst != nullptr);
    if (inst && ext && ext->on_action) {
        // no game: a sample reads nothing and writes nothing
        m->sample(inst, &host);
        m->tick(inst, &host);
        // the companion's own actions
        for (const char* a : {"use_reserve", "play_tap", "col_tap", "mago_mask", "wmap_tap", "title_start"})
            CHECK(ext->on_action(inst, a, 0) != 0);
        CHECK(ext->on_action(inst, "file_tap", 3) == 0);
        CHECK(ext->on_action(inst, "nothing", 0) == 0);
        CHECK(ext->on_action(nullptr, "use_reserve", 0) == 0);
        CHECK(ext->on_action(inst, nullptr, 0) == 0);
        // the research actions (gen_manifest.py --dev): only in the development module
        for (const char* a : {"scan_player", "ptrscan", "statescan", "bytefind", "findseq", "basefind", "vscan", "vtscan",
                              "hscan", "layoutscan", "trace", "render", "fxwatch", "objfind", "panewatch", "bgmwatch",
                              "abilitystar", "starhunt", "newhunt", "newforce", "soul_scream"}) {
            const bool accepted = ext->on_action(inst, a, 0) != 0;
            if (accepted != (KIRBY_DEV != 0)) {
                std::fprintf(stderr, "research action %s %s\n", a, accepted ? "accepted" : "refused");
                ++failures;
            }
        }
        // an unknown picture key is refused
        CHECK(ext->load_image(inst, &host, "module:kirby:nothing", nullptr, nullptr) == 0);
    }
    if (inst)
        m->destroy(inst);

    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    else
        std::printf("kirby module (KIRBY_DEV %d): ok\n", KIRBY_DEV);
    return failures ? 1 : 0;
}
