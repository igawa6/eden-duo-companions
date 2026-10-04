// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Animal Crossing: New Horizons 3.0.3 (main build ff1d1c05670db6021c85b624a710b963) dual-screen
// companion: the ABI glue. Everything the bottom screen draws is either published by the live
// lane (acnh_live / acnh_publish through the Live* seam in acnh_types.h) or served here as
// module:acnh/<key> images / bytes decoded from the player's own romfs (acnh_ui_art.h lists the
// keys; nothing from the game ships in the package).
//
// Extensions: base (configure = the host's ASTC decoder, on_action -> live lane, load_image ->
// art dispatcher, map/ keys -> the map lane's MapLoadImage), font (decode_font: the package's
// font spec -> a Seurat-B + ParkExt atlas), data (load_data: font specs and small text blobs),
// write (configure = the host's write_batch, kept for the Bag page's guarded direct writes:
// acnh_bag.h; one batch per action, expect-guarded, applied with the guest threads suspended).
// Every callback catches everything: a module failure must never take the emulator down.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "acnh_catalog.h"
#include "acnh_config.h"
#include "acnh_flavor.h"
#include "acnh_font.h"
#include "acnh_lang.h"
#include "acnh_live.h"
#include "acnh_map.h"
#include "acnh_msbt.h"
#include "acnh_romfs.h"
#include "acnh_tex.h"
#include "acnh_types.h"
#include "acnh_ui_art.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x01006F8002326000);
// 3.0.3 (update v2228224) main: the first 16 bytes of the build id, the rest zero.
constexpr std::string_view BuildId303 = "ff1d1c05670db6021c85b624a710b963";
constexpr std::string_view KeyPrefix = "module:acnh/";

bool MatchesBuild(std::string_view hex) {
    if (hex.size() < BuildId303.size())
        return false;
    for (size_t i = 0; i < hex.size(); ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(hex[i])));
        if (i < BuildId303.size() ? c != BuildId303[i] : c != '0')
            return false;
    }
    return true;
}

std::string BuildHex(const EdenDsmodHostApi& h) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const u8 b : h.build_id) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

class Module {
public:
    explicit Module(const EdenDsmodHostApi& api)
        : host{api}, romfs{&api}, catalog{romfs}, fonts{romfs},
          font_epoch_host{(api.capabilities & EDEN_DSMOD_CAP_FONT_EPOCH) != 0},
          art{romfs, catalog, fonts,
              acnh::Art::Sources{[this](std::vector<u8>& jpeg, u64& rev) {
                                     return live && acnh::LivePassportJpeg(live, jpeg, rev);
                                 },
                                 [this] {
                                     const int l = live ? acnh::LiveLanguage(live) : -1;
                                     return l >= 0 && l < static_cast<int>(acnh::Lang::Count)
                                                ? static_cast<acnh::Lang>(l)
                                                : acnh::Lang::USen;
                                 }}},
          build_ok{MatchesBuild(BuildHex(api))}, ver_idx{acnh::flavor::BuildIndex(BuildHex(api))} {
        services.romfs = &romfs;
        services.catalog = &catalog;
        services.fonts = &fonts; // one copy of each face for the art, the widths and the map names
        services.image_activity_ms = &image_activity_ms; // r9 pop: the picture prewarm
        services.image_inflight = &image_inflight;
        acnh::MapSetFonts(&fonts);
        // Text-only mode on every other known build (acnh_flavor.h): no live reader, so not one
        // read of game memory; only the font (romfs) and the version index for the version page.
        if (build_ok)
            live = acnh::LiveCreate(api, services);
        // a language glyph set = every code point of the game's String + LayoutMsg texts + our
        // flavour lines (waiting / version / blocking card); "flavor" = every language's lines
        fonts.SetLanguageCodepoints([this](std::string_view folder) {
            std::vector<u32> out;
            if (folder == "flavor")
                return acnh::flavor::CodepointsAll();
            const acnh::Lang l = acnh::LangFromFolder(folder, acnh::Lang::Count);
            if (l == acnh::Lang::Count)
                return out;
            if (!build_ok)
                return acnh::flavor::Codepoints(l);
            std::vector<bool> seen(0x10000, false);
            for (const char* group : {"String", "LayoutMsg"})
                for (const auto& path : catalog.MessagePaths(l, group)) {
                    const auto m = catalog.Message(l, path);
                    if (!m)
                        continue;
                    for (const auto& [label, raw] : m->All()) {
                        const std::string text = acnh::Msbt::ToUtf8(raw);
                        for (size_t i = 0; i < text.size();) {
                            const char32_t c = acnh::NextCodepoint(text, i);
                            if (c >= 0x20 && c < 0x10000)
                                seen[c] = true;
                        }
                    }
                }
            for (const u32 c : acnh::flavor::Codepoints(l))
                if (c < 0x10000)
                    seen[c] = true;
            for (u32 c = 0x20; c < 0x10000; ++c)
                if (seen[c])
                    out.push_back(c);
            return out;
        });
    }
    ~Module() {
        if (font_prewarm.joinable())
            font_prewarm.join();
        acnh::LiveDestroy(live);
        acnh::MapSetFonts(nullptr);
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void Sample(const EdenDsmodHostApi& h) {
        host = h;
        Publish("acnh.build_ok", build_ok ? 1 : 0);
        // the version page: index of the running build in the flavour table (have.<i>), the font
        // state (labels in the game font only once it is decoded), the waiting line (every 4 s)
        Publish("acnh.ver.idx", ver_idx);
        Publish("acnh.font", font_ok.load() ? 1 : 0);
        if (font_epoch_host)
            WatchFontLanguage();
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            const int n = std::max(1, acnh::flavor::WaitCount());
            Publish("ui.wait.i", static_cast<int64_t>((ms / 4000) % n));
        }
#if EDEN_ACNH_DIAGNOSTICS
        Publish("core.img.n", static_cast<int64_t>(img_count.load()));
        Publish("core.img.fail", static_cast<int64_t>(img_fail.load()));
        Publish("core.img.us_last", static_cast<int64_t>(img_us_last.load()));
        Publish("core.img.us_max", static_cast<int64_t>(img_us_max.load()));
        Publish("core.img.us_total", static_cast<int64_t>(img_us_total.load()));
        Publish("core.img.cache_kb", static_cast<int64_t>(art.CachedBytes() / 1024));
        const auto t0 = std::chrono::steady_clock::now();
#endif
        if (live)
            acnh::LiveSample(live, h);
#if EDEN_ACNH_DIAGNOSTICS
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        Publish("core.sample_us", us);
#endif
    }
    void Tick(const EdenDsmodHostApi& h) {
        if (live)
            acnh::LiveTick(live, h);
    }
    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext))
            acnh::SetAstcDecoder(ext->userdata, ext->decode_astc);
    }
    void ConfigureWrite(const EdenDsmodHostWriteApi* w) {
        if (w && w->version == EDEN_DSMOD_WRITE_EXT_VERSION &&
            w->struct_size == sizeof(EdenDsmodHostWriteApi) &&
            w->abi_hash == EDEN_DSMOD_WRITE_EXT_HASH && w->write_batch && live)
            acnh::LiveSetWriteApi(live, *w);
    }
    bool Action(const char* action, s64 argument) {
        return live && action && acnh::LiveAction(live, action, argument);
    }

    // Asset worker thread.
    bool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                   EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink)
            return false;
        const std::string_view k{key};
        if (!k.starts_with(KeyPrefix))
            return false;
        auto logical = k.substr(KeyPrefix.size());
#if EDEN_ACNH_DIAGNOSTICS
        const auto t0 = std::chrono::steady_clock::now();
#endif
        image_activity_ms.store(acnh::live::NowMs()); // r9 pop: the prewarm waits for this
        image_inflight.fetch_add(1);
        struct Done {
            Module& m;
            ~Done() {
                m.image_activity_ms.store(acnh::live::NowMs());
                m.image_inflight.fetch_sub(1);
            }
        } done{*this};
        acnh::Image img;
        bool ok;
        // runtime 17 paged font (the package's font_atlas "module:acnh/font/<spec>?p={p}"): page
        // <n> of the same atlas the key without "?p=" names
        int font_page = -1;
        if (logical.starts_with("font/")) {
            if (const auto q = logical.find("?p="); q != std::string_view::npos) {
                const auto digits = logical.substr(q + 3);
                if (digits.empty() || digits.size() > 4 ||
                    digits.find_first_not_of("0123456789") != std::string_view::npos)
                    return false;
                font_page = std::stoi(std::string{digits});
                logical = logical.substr(0, q);
            }
        }
        if (!build_ok && !logical.starts_with("font/"))
            ok = false; // text-only mode: the font only (another build's romfs is not mapped)
        else if (!build_ok)
            ok = logical.find('?') == std::string_view::npos &&
                 art.Load("font/" + FlavorFontSpec(std::string{logical.substr(5)}), img);
        else if (logical.starts_with("map/"))
            ok = acnh::MapLoadImage(romfs, logical, img);
        else if (logical.starts_with("font/") && font_lang_index.load() >= 0 &&
                 logical.find('?') == std::string_view::npos)
            ok = art.Load(
                "font/" + EffectiveFontSpec(std::string{logical.substr(5)}, font_lang_index.load()),
                img);
        else
            ok = art.Load(logical, img);
        if (ok && font_page >= 0) {
            const int y0 = font_page * static_cast<int>(acnh::AtlasPageRows);
            const int rows = std::min(img.h - y0, static_cast<int>(acnh::AtlasPageRows));
            if (rows <= 0) {
                ok = false;
            } else {
                acnh::Image page{img.w, rows};
                std::memcpy(page.rgba.data(), img.At(0, y0), page.rgba.size());
                img = std::move(page);
            }
        }
        ok = ok && !img.Empty();
#if EDEN_ACNH_DIAGNOSTICS
        const auto us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count());
        img_count.fetch_add(1);
        img_us_last.store(us);
        img_us_total.fetch_add(us);
        for (u64 m = img_us_max.load(); us > m && !img_us_max.compare_exchange_weak(m, us);) {
        }
        if (image_host->log) {
            char line[384];
            std::snprintf(line, sizeof line, "ACNH load_image %s: %s %dx%d %llu us",
                          std::string{logical}.c_str(), ok ? "ok" : "FAILED", img.w, img.h,
                          static_cast<unsigned long long>(us));
            image_host->log(image_host->userdata,
                            ok ? EDEN_DSMOD_LOG_DEBUG : EDEN_DSMOD_LOG_WARNING, line);
        }
#endif
        if (!ok) {
#if EDEN_ACNH_DIAGNOSTICS
            img_fail.fetch_add(1);
#endif
            return false;
        }
        sink(receiver, static_cast<u32>(img.w), static_cast<u32>(img.h), img.rgba.data(),
             img.rgba.size());
        return true;
    }

    // Text-only mode: the package's base spec ("ui:40") with the "flavor" set (every language's
    // flavour lines; the language is not known without reading the game).
    static std::string FlavorFontSpec(const std::string& spec) {
        const auto canon = acnh::Fonts::CanonicalSpec(spec);
        if (!canon)
            return spec;
        return canon->substr(0, canon->rfind(':')) + ":flavor";
    }

    // The package's base spec ("ui:40") with the set of the game language: a CJK language folder
    // (its texts' code points; the cap steps down until the atlas fits the host's 16 MiB image
    // limit), else the spec as written. Remembered for the atlas image.
    std::string EffectiveFontSpec(const std::string& spec, int lang_index) {
        if (lang_index < 0 || lang_index >= static_cast<int>(acnh::Lang::Count) ||
            !acnh::LangIsCjk(static_cast<acnh::Lang>(lang_index)))
            return spec;
        const auto canon = acnh::Fonts::CanonicalSpec(spec);
        if (!canon || !canon->ends_with(":latin"))
            return spec;
        const std::string folder{acnh::LangFolder(static_cast<acnh::Lang>(lang_index))};
        const std::string cache_key = *canon + "#" + folder; // per language: it may change
        {
            std::scoped_lock lock{font_mutex};
            if (const auto it = font_effective.find(cache_key); it != font_effective.end())
                return it->second;
        }
        const std::string face = canon->substr(0, canon->find(':'));
        int cap = std::stoi(canon->substr(face.size() + 1));
        std::string eff = spec;
        for (; cap >= 16; cap -= 4) {
            const std::string s = face + ":" + std::to_string(cap) + ":" + folder;
            if (const auto a = fonts.Atlas(s); a && !a->atlas.Empty()) {
                eff = s;
                break;
            }
        }
        std::scoped_lock lock{font_mutex};
        font_effective[cache_key] = eff;
        return eff;
    }

    // decode_font: the bytes are the package's font_metrics_src = a spec ("ui:30", acnh_font.h),
    // served by load_data for module:acnh/font/<spec> or shipped as a tiny text file.
    bool DecodeFont(const u8* bytes, size_t size, void* receiver, EdenDsmodFontSink sink) {
        if (!bytes || !sink || size == 0 || size > 256)
            return false;
        std::string spec{reinterpret_cast<const char*>(bytes), size};
        while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.back())))
            spec.pop_back();
        // The glyph set follows the GAME language (the texts are the game's): wait for the live
        // reader to see it (the host retries a declined decode about once a second, 30 times,
        // then never asks again -- so decline through attempt 29 and decide on the last one);
        // CJK languages get their texts' code points, the others stay Latin. LIMIT: a boot where
        // the language is still unreadable after ~30 s (the game's "language changed" prompt, a
        // debugger-held start) keeps the Latin set until the package is reloaded on runtime 15-17
        // hosts (research/acnh/impl/fix/rt18-proposal.md).
        // Text-only mode (another build): at once, every language's flavour lines.
        // A runtime 18 host (EDEN_DSMOD_CAP_FONT_EPOCH) asks again when "__font_epoch" changes:
        // decode at once with what is known (the Latin set before the language is read) and count
        // the epoch up when the language turns out to need another set (WatchFontLanguage).
        int decoded_set = -1;
        if (!build_ok) {
            spec = FlavorFontSpec(spec);
        } else {
            const int l = live ? acnh::LiveLanguage(live) : -1;
            if (l < 0 && !font_epoch_host && ++font_attempts < 29)
                return false;
            {
                std::scoped_lock lock{font_mutex};
                font_base_spec = spec;
            }
            decoded_set = FontSetOf(l);
            spec = EffectiveFontSpec(spec, l);
            if (decoded_set >= 0 && !spec.ends_with(acnh::LangFolder(static_cast<acnh::Lang>(l))))
                return false;
        }
        const auto atlas = fonts.Atlas(spec);
        if (!atlas || atlas->glyphs.empty())
            return false;
        font_lang_index.store(decoded_set);
        font_set.store(decoded_set);
        font_ok.store(true);
        sink(receiver, atlas->line_height, atlas->first_codepoint, atlas->glyphs.data(),
             static_cast<u32>(atlas->glyphs.size()));
        return true;
    }

    // The glyph set a language needs: its CJK index, or -1 = the spec as written (Latin).
    static int FontSetOf(int lang) {
        return lang >= 0 && lang < static_cast<int>(acnh::Lang::Count) &&
                       acnh::LangIsCjk(static_cast<acnh::Lang>(lang))
                   ? lang
                   : -1;
    }

    // Tick thread (Sample), runtime 18 hosts only: once the game language is read (or changed)
    // and needs another glyph set than the decoded one, build that atlas off the tick thread
    // (a CJK set is ~65000 glyphs), then count "__font_epoch" up: the host calls decode_font
    // again (now a cache hit) and reloads the atlas image.
    void WatchFontLanguage() {
        Publish("__font_epoch", font_epoch.load());
        if (!build_ok || !font_ok.load() || font_prewarm_busy.load())
            return;
        const int l = live ? acnh::LiveLanguage(live) : -1;
        if (l < 0)
            return;
        const int want = FontSetOf(l);
        if (want == font_set.load() || want == font_requested.load())
            return;
        std::string base;
        {
            std::scoped_lock lock{font_mutex};
            base = font_base_spec;
        }
        if (base.empty())
            return;
        if (font_prewarm.joinable())
            font_prewarm.join(); // finished (busy is false)
        if (acnh::live::NowMs() < font_retry_ms.load())
            return;
        const auto log = host.log;
        void* const log_userdata = host.userdata;
        font_requested.store(want);
        font_prewarm_busy.store(true);
        try {
            font_prewarm = std::thread([this, base, l, log, log_userdata] {
                bool ready = false;
                try {
                    const std::string eff = EffectiveFontSpec(base, l);
                    const auto atlas = fonts.Atlas(eff);
                    ready = atlas && !atlas->atlas.Empty() && !atlas->glyphs.empty() &&
                            (FontSetOf(l) < 0 ||
                             eff.ends_with(acnh::LangFolder(static_cast<acnh::Lang>(l))));
                } catch (...) {
                    // Decoder/allocation failures must not escape a std::thread and terminate the
                    // host.
                }
                if (ready) {
                    font_epoch.fetch_add(1);
                } else {
                    font_requested.store(-2);
                    font_retry_ms.store(acnh::live::NowMs() + 1000);
                    if (log) {
                        try {
                            log(log_userdata, EDEN_DSMOD_LOG_WARNING,
                                "ACNH font refresh failed; will retry");
                        } catch (...) {
                        }
                    }
                }
                font_prewarm_busy.store(false);
            });
        } catch (...) {
            font_requested.store(-2);
            font_retry_ms.store(acnh::live::NowMs() + 1000);
            font_prewarm_busy.store(false);
            throw;
        }
    }

    // load_data: module:acnh/font/<spec> -> the canonical spec text (the matching atlas image is
    // load_image of the same key).
    bool LoadData(const char* key, void* receiver, EdenDsmodDataSink sink) {
        if (!key || !sink)
            return false;
        const std::string_view k{key};
        if (!k.starts_with(KeyPrefix))
            return false;
        const auto logical = k.substr(KeyPrefix.size());
        if (logical.starts_with("font/")) {
            const auto spec = acnh::Fonts::CanonicalSpec(logical.substr(5));
            if (!spec)
                return false;
            sink(receiver, reinterpret_cast<const u8*>(spec->data()), spec->size());
            return true;
        }
        return false;
    }

private:
    void Publish(const char* name, int64_t v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, v);
    }

    EdenDsmodHostApi host;
    acnh::Romfs romfs;
    acnh::Catalog catalog;
    acnh::Fonts fonts;
    int font_attempts = 0;
    const bool font_epoch_host;          ///< the host re-requests the font on "__font_epoch"
    std::atomic<s64> font_epoch{0};      ///< published "__font_epoch"
    std::atomic<int> font_set{-2};       ///< the glyph set decoded (FontSetOf), -2 = none yet
    std::atomic<int> font_requested{-2}; ///< the set the last epoch step asked for
    std::atomic<bool> font_prewarm_busy{false};
    std::atomic<uint64_t> font_retry_ms{0};
    std::thread font_prewarm;   ///< builds the new set's atlas off the tick thread
    std::string font_base_spec; ///< the package's spec (font_mutex)
    std::atomic<int> font_lang_index{
        -1}; ///< the CJK language the font was decoded with, -1 = as written
    std::mutex font_mutex;
    std::map<std::string, std::string> font_effective; ///< canonical base spec -> effective spec
    acnh::Art art;
    acnh::Services services;
    acnh::Live* live = nullptr;
    bool build_ok = false;
    int ver_idx = -1;                 ///< the running build in acnh_flavor's table, -1 = unknown
    std::atomic<bool> font_ok{false}; ///< decode_font answered (labels may use the game font)
#if EDEN_ACNH_DIAGNOSTICS
    std::atomic<u64> img_count{0}, img_fail{0}, img_us_last{0}, img_us_max{0}, img_us_total{0};
#endif
    std::atomic<uint64_t> image_activity_ms{0}; ///< r9 pop: last load_image start/end (live::NowMs)
    std::atomic<int> image_inflight{0};         ///< r9 pop: load_image calls running
};

EdenDsmodBool SupportsBuild(const char* build_id) {
    // 3.0.3 = the companion; every other known build = text-only mode (the version page)
    return build_id && (MatchesBuild(build_id) || acnh::flavor::BuildIndex(build_id) >= 0)
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->read_memory)
            return nullptr;
        return new Module{*host};
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
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "ACNH DSMod sample callback failed");
    }
}

void TickCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (p && host)
            static_cast<Module*>(p)->Tick(*host);
    } catch (...) {
    }
}

void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}

void ConfigureWriteCallback(void* p, const EdenDsmodHostWriteApi* w) {
    try {
        if (p)
            static_cast<Module*>(p)->ConfigureWrite(w);
    } catch (...) {
    }
}

EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        if (p && static_cast<Module*>(p)->Action(action, argument))
            return EDEN_DSMOD_TRUE;
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p && static_cast<Module*>(p)->LoadImage(host, key, receiver, sink))
            return EDEN_DSMOD_TRUE;
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, size_t size, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (p && static_cast<Module*>(p)->DecodeFont(bytes, size, receiver, sink))
            return EDEN_DSMOD_TRUE;
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

EdenDsmodBool LoadDataCallback(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                               EdenDsmodDataSink sink) {
    try {
        if (p && static_cast<Module*>(p)->LoadData(key, receiver, sink))
            return EDEN_DSMOD_TRUE;
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Animal Crossing: New Horizons DSMod",
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
const EdenDsmodModuleDataExtensions DataExtensions{EDEN_DSMOD_DATA_EXT_VERSION,
                                                   sizeof(EdenDsmodModuleDataExtensions),
                                                   EDEN_DSMOD_DATA_EXT_HASH, &LoadDataCallback};
const EdenDsmodModuleWriteExtensions WriteExtensions{
    EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(EdenDsmodModuleWriteExtensions), EDEN_DSMOD_WRITE_EXT_HASH,
    &ConfigureWriteCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
DSMOD_SDK_EXPORT_DATA_EXTENSIONS(DataExtensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
