// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Binding of Isaac: Afterbirth+ / Repentance module: ABI glue.
//   isaac_reader.{h,cpp}  edition-specific game-memory reader; validates the native image
//                         and publishes the companion output contract
//   isaac_assets.{h,cpp}  asset-free art (PCX + anm2 from the player's romfs), map image,
//                         the game's BMFont (font extension)
//   isaac_text.{h,cpp}    the game's text (items/pocket items/stages via stringtable.sta)
//   isaac_romfs.{h,cpp}   the game's mount order over romfs:/aoc:/base: sources
// Supported update: launcher B6E5BDB9DC12E1D1A25CBFDA17F4BE24B4754EF5 (v524288),
// Repentance.nro 91C73FDD575061318D68886316AFEAC72388B2AB or AfterbirthPlus.nro
// 0F65948274CE60ECA59C1E55B09C54D0607F3C57 when the DLC is absent.
// Module actions: see research/isaac/CONTRACT.md.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "isaac_assets.h"
#include "isaac_mapreg.h"
#include "isaac_reader.h"
#include "isaac_text.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#endif

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x010021C000B6A000);

EdenDsmodBool SupportsBuild(const char* build_id) {
    return isaac_reader::SupportsBuildHex(build_id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

/// One module instance. The asset library runs on the asset worker, the reader on the tick
/// thread; the game text is loaded once on its own low-priority thread and handed to the reader
/// only after Load() finished (the GameText object is immutable afterwards).
struct Module {
    Module(const EdenDsmodHostApi& api, const char* config)
        : host{api}, assets{isaac_assets::CreateLibrary()}, text{isaac_text::CreateGameText()},
          reader{api, config} {
        text_thread = std::thread([this]() {
#if defined(__linux__) || defined(__ANDROID__)
            setpriority(PRIO_PROCESS, 0, 19);
#endif
            // Sources can become available after module creation. Each retry is transactional;
            // the reader receives the immutable result only after successful completion.
            while (!text_stop.load(std::memory_order_relaxed)) {
                try {
                    if (text->Load(&host, isaac_romfs::Lang::En, &text_stop)) {
                        text_ready.store(true, std::memory_order_release);
                        return;
                    }
                } catch (...) {
                    // Host IO/allocation exceptions must not escape a worker thread.
                }
                std::unique_lock lock{text_mutex};
                text_wake.wait_for(lock, std::chrono::milliseconds{750},
                                   [this] { return text_stop.load(std::memory_order_relaxed); });
            }
        });
    }
    ~Module() {
        {
            std::lock_guard lock{text_mutex};
            text_stop.store(true, std::memory_order_relaxed);
        }
        text_wake.notify_all();
        if (text_thread.joinable())
            text_thread.join();
    }

    void Sample(const EdenDsmodHostApi& h) {
        if (!text_handed && text_ready.load(std::memory_order_acquire)) {
            reader.SetText(text.get());
            text_handed = true;
        }
        reader.Sample(h);
    }

    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink || !assets)
            return EDEN_DSMOD_FALSE;
        std::string_view k{key};
        if (k.starts_with("module:"))
            k.remove_prefix(7);
        if (!k.starts_with("isaac:"))
            return EDEN_DSMOD_FALSE;
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        std::string plain{k};
        // short map key (isaac_mapreg.h): expand to the full encoding the asset side renders
        if (k.starts_with("isaac:map/h")) {
            std::string hex;
            if (!isaac_mapreg::Lookup(k.substr(10), hex))
                return EDEN_DSMOD_FALSE;
            plain = "isaac:map/" + hex;
        }
        if (!isaac_assets::LoadImage(*assets, image_host, plain.c_str(), rgba, w, h) || !w || !h ||
            rgba.size() != static_cast<std::size_t>(w) * h * 4)
            return EDEN_DSMOD_FALSE;
        sink(receiver, w, h, rgba.data(), rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    EdenDsmodHostApi host; // create()'s host table: decode_font and the text thread use it
    std::unique_ptr<isaac_assets::Library> assets;
    std::unique_ptr<isaac_text::GameText> text;
    isaac_reader::Reader reader;
    std::atomic<bool> text_ready{false};
    bool text_handed{false};
    std::mutex text_mutex;
    std::condition_variable text_wake;
    std::atomic<bool> text_stop{false};
    std::thread text_thread;
};

void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        // create() re-checks all 32 build-id bytes (supports_build saw the hex form).
        if (!isaac_reader::SupportsBuildId(host->build_id))
            return nullptr;
        return new Module{*host, config};
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
        if (p && host)
            static_cast<Module*>(p)->Sample(*host);
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "Isaac DSMod sample callback failed");
    }
}
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void*, const EdenDsmodHostExtensions*) {}
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        return p && static_cast<Module*>(p)->reader.OnAction(action, argument) ? EDEN_DSMOD_TRUE
                                                                               : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->LoadImage(host, key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

// Font extension: the manifest `font` is a package text file naming the game font (CONTRACT.md);
// the metrics come from the game's BMFont, the atlas is the manifest's font_atlas module key.
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, std::size_t size,
                                 void* receiver, EdenDsmodFontSink sink) {
    try {
        auto* m = static_cast<Module*>(p);
        isaac_assets::FontOut font;
        if (!m || !sink || !m->assets ||
            !isaac_assets::DecodeFont(*m->assets, &m->host, bytes, size, font))
            return EDEN_DSMOD_FALSE;
        sink(receiver, font.line_height, font.first_codepoint, font.glyphs.data(),
             static_cast<std::uint32_t>(font.glyphs.size()));
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
                                   "The Binding of Isaac DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
    &ActionCallback,        &LoadImageCallback};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
