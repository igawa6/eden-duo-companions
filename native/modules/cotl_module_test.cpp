// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <map>
#include <string>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "cotl_test_util.h"

namespace {

struct Published {
    std::map<std::string, int64_t> i64;
    std::map<std::string, double> f64;
    std::map<std::string, std::string> text;
    size_t Count() const { return i64.size() + f64.size() + text.size(); }
};

Published g_pub;

void PubI64(void*, const char* n, int64_t v) { g_pub.i64[n] = v; }
void PubF64(void*, const char* n, double v) { g_pub.f64[n] = v; }
void PubText(void*, const char* n, const char* v) { g_pub.text[n] = v; }
void PubAddr(void*, const char* n, uint64_t v) { g_pub.i64[n] = static_cast<int64_t>(v); }
EdenDsmodBool NotMapped(void*, uint64_t, uint64_t) { return EDEN_DSMOD_FALSE; }
EdenDsmodBool NoRead(void*, uint64_t, void*, size_t) { return EDEN_DSMOD_FALSE; }
void Log(void*, uint32_t, const char*) {}

const uint8_t kBuild[16] = {0x31, 0x6A, 0x4D, 0x48, 0x81, 0xA9, 0xF1, 0xE0,
                            0x77, 0xA0, 0x72, 0x62, 0xE9, 0x17, 0x91, 0x7C};
const char* kBuildHex = "316A4D4881A9F1E077A07262E917917C"
                        "00000000000000000000000000000000";

EdenDsmodHostApi MakeHost() {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.struct_size = sizeof(EdenDsmodHostApi);
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.title_id = 0x01002E7016C46000ull;
    std::memcpy(h.build_id, kBuild, sizeof(kBuild));
    h.main_base = 0x80000000;
    h.main_size = 0x8000000;
    h.is_mapped = NotMapped;
    h.read_memory = NoRead;
    h.log = Log;
    h.publish_i64 = PubI64;
    h.publish_f64 = PubF64;
    h.publish_text = PubText;
    h.publish_address = PubAddr;
    return h;
}

const EdenDsmodModuleApi* Module() {
    return eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
}

} // namespace

TEST(module_table_matches_abi) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    CHECK_EQ(m->title_id, uint64_t{0x01002E7016C46000ull});
    CHECK_EQ(m->struct_size, uint32_t{sizeof(EdenDsmodModuleApi)});
    CHECK(m->supports_build && m->create && m->destroy && m->sample && m->tick);
    CHECK(eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION + 1, EDEN_DSMOD_MODULE_ABI_HASH) ==
          nullptr);
}

TEST(module_supports_only_exact_build) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    CHECK(m->supports_build(kBuildHex) != 0);
    CHECK(m->supports_build("316A4D4881A9F1E0") == 0);
    CHECK(m->supports_build("0000000000000000000000000000000000000000000000000000000000000000") == 0);
}

TEST(module_create_rejects_other_build) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    auto host = MakeHost();
    host.build_id[0] ^= 0xFF;
    CHECK(m->create(&host, "{}") == nullptr);
}

TEST(module_exports_image_extension) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    CHECK((m->capabilities & EDEN_DSMOD_CAP_EXTENSIONS) != 0);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(ext != nullptr);
    CHECK(ext->configure != nullptr);
    CHECK(ext->load_image != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    // Unknown keys are rejected, without a romfs there is no icon
    CHECK(ext->load_image(inst, &host, "module:cotl:nothing", nullptr, nullptr) == 0);
    CHECK(ext->load_image(inst, &host, "module:cotl:item/1", nullptr, nullptr) == 0);
    m->destroy(inst);
}

// Before the title screen no game memory is readable: only mod.ready and g.in_run 0
// (the tab selection in the manifest always needs g.in_run), nothing else.
TEST(module_sample_without_game_memory_publishes_only_ready) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    g_pub = {};
    m->sample(inst, &host);
    CHECK_EQ(g_pub.i64["mod.ready"], int64_t{1});
    CHECK_EQ(g_pub.i64["g.in_run"], int64_t{0});
    CHECK_EQ(g_pub.Count(), size_t{3}); // mod.ready, g.in_run, g.loaded
    m->destroy(inst);
}
