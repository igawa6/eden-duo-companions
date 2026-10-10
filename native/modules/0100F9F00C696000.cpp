// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash Team Racing Nitro-Fueled module: ABI glue.
//   ctr_reader.{h,cpp}   race reader: racers, ranks, positions, track, laps, the minimap widget
//   ctr_art.{h,cpp}      the page's pictures composed from the player's romfs (Alchemy assets)
//   ctr_alchemy.{h,cpp}  igArchive / IGZ / igImage2 / igBitmapFont / minimap data readers
//   ctr_lzma.{h,cpp}     the archives' raw LZMA blocks
// Supported build: 1.0.15 (update v983040), main 1C689518406930512C13DDF4217E7676. Every other
// known build loads too, but only publishes the "update required" page (no reads, no writes).
// Actions hud_lead / hud_pos / hud_map / hud_lap / hud_time (argument 1 = hide, 0 = show) come
// from the settings page's enforce entries; the reader applies them on the top screen.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "ctr_art.h"
#include "ctr_reader.h"

#include <atomic>
#include <cctype>
#include <utility>
#include <cstring>
#include <string_view>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100F9F00C696000);
constexpr u8 BuildId[16]{0x1C, 0x68, 0x95, 0x18, 0x40, 0x69, 0x30, 0x51,
                         0x2C, 0x13, 0xDD, 0xF4, 0x21, 0x7E, 0x76, 0x76};

// Other released builds (first 8 bytes of the main build id): the wrong-patch page names them.
struct KnownBuild {
    u8 id[8];
    const char* code; ///< "base" = 1.0.0, "old" = an older update
};
constexpr KnownBuild OtherBuilds[]{
    {{0x04, 0xD1, 0xCF, 0xEB, 0x1E, 0x0B, 0x93, 0x49}, "base"}, // 1.0.0
    {{0x20, 0xE8, 0x62, 0xBD, 0x6C, 0x39, 0xD8, 0xAE}, "old"},
    {{0x22, 0xDF, 0x17, 0x9B, 0x89, 0x61, 0x18, 0x07}, "old"}, // 1.0.7
    {{0x47, 0xE6, 0x08, 0x71, 0x47, 0x1F, 0x1C, 0xE9}, "old"}, // 1.0.14
    {{0x64, 0x73, 0x64, 0x28, 0xD3, 0x44, 0xE7, 0x8E}, "old"},
    {{0x67, 0x81, 0x3B, 0x3F, 0x85, 0x78, 0x29, 0x91}, "old"},
    {{0x6D, 0x69, 0x33, 0x14, 0xDA, 0xC7, 0xB0, 0x40}, "old"},
    {{0x9A, 0x5C, 0xF7, 0x03, 0x01, 0xFB, 0xEB, 0x93}, "old"},
    {{0xD6, 0x14, 0xA2, 0x0D, 0xDE, 0x59, 0xE4, 0xD7}, "old"},
    {{0xDF, 0xCF, 0xAF, 0xF4, 0x46, 0x73, 0xEF, 0x2B}, "old"},
};

/// The wrong-patch code for a build id, "" for the supported build, null when unknown.
const char* BuildCode(const u8* id) {
    if (std::memcmp(id, BuildId, 8) == 0)
        return "";
    for (const auto& b : OtherBuilds)
        if (std::memcmp(id, b.id, 8) == 0)
            return b.code;
    return nullptr;
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    if (!build_id || std::strlen(build_id) < 16)
        return EDEN_DSMOD_FALSE;
    u8 id[8]{};
    for (int i = 0; i < 16; ++i) {
        const int c = std::toupper(static_cast<unsigned char>(build_id[i]));
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return EDEN_DSMOD_FALSE;
        id[i / 2] = static_cast<u8>(id[i / 2] << 4 | v);
    }
    return BuildCode(id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

struct Module {
    CtrArt::Library art; ///< before the reader that uses it
    CtrReader::Reader reader{art};
    const char* wrong{}; ///< non-null: an unsupported release, only the wrong-patch page
};

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory || !host->read_romfs)
            return nullptr;
        const char* code = BuildCode(host->build_id);
        if (!code || (!*code && std::memcmp(host->build_id, BuildId, sizeof BuildId) != 0))
            return nullptr;
        auto* m = new Module{};
        m->wrong = *code ? code : nullptr;
        return m;
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Module*>(p);
    } catch (...) {
    }
}
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host) {
            auto* m = static_cast<Module*>(p);
            if (m->wrong)
                m->reader.PublishUnsupported(*host, m->wrong);
            else
                m->reader.Sample(*host);
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "CTR DSMod sample callback failed");
    }
}
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void*, const EdenDsmodHostExtensions*) {}
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    // settings -> top-screen HUD (argument: the switch, 1 = hide)
    static constexpr std::pair<std::string_view, int> Hud[]{
        {"hud_lead", CtrReader::HudLeaders}, {"hud_pos", CtrReader::HudPosition},
        {"hud_map", CtrReader::HudMap},      {"hud_lap", CtrReader::HudLap},
        {"hud_time", CtrReader::HudTime}};
    if (!p || !action)
        return EDEN_DSMOD_FALSE;
    auto* m = static_cast<Module*>(p);
    for (const auto& [name, element] : Hud)
        if (name == action) {
            if (m->wrong)
                return EDEN_DSMOD_TRUE; // nothing to hide on an unsupported build
            m->reader.SetHudHidden(element, argument != 0);
            return EDEN_DSMOD_TRUE;
        }
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    try {
        if (!p || !host || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const auto img = static_cast<Module*>(p)->art.Load(*host, key);
        if (!img || img->rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, img->width, img->height, img->rgba.data(), img->rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Crash Team Racing Nitro-Fueled DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
