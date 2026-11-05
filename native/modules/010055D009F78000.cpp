// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Fire Emblem: Three Houses module: ABI glue.
//   fe3h_reader.{h,cpp}  game-memory reader (mode, header, battle, academy), build-pinned code
//                        checks (fe3h_pins.inc), names from the game's in-memory text manager
//   fe3h_assets.{h,cpp}  asset-free art from the player's romfs (portraits, faces, battle
//                        overview maps, abbey map, UI sprites), with fe3h_romfs / fe3h_g1t /
//                        fe3h_ui2d; fe3h_font: the game's font (font extension)
// Supported build: 1.2.0 only (89048449BA238C8CF565518B83BF02D3 + zeros).
// Module actions: see Reader::OnAction (ui_open, bt_select, bt_goto[_tap], ac_select,
// qs_select / qs_view, li_select, cal_day).

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "fe3h_assets.h"
#include "fe3h_language.h"
#include "fe3h_reader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <string>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x010055D009F78000);

EdenDsmodBool SupportsBuild(const char* build_id) {
    return Fe3hReader::SupportsBuildHex(build_id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

/// The GPS player arrow: the pause-map star sprite (0x7BD) rotated clockwise by `deg` into a
/// square canvas (side = the sprite's diagonal) whose centre is the sprite's centre. Bilinear on
/// premultiplied alpha. deg is a multiple of 5 in 0..355.
bool RotateSprite(const std::vector<std::uint8_t>& src, std::uint32_t w, std::uint32_t h, int deg,
                  std::vector<std::uint8_t>& out, std::uint32_t& side) {
    side = static_cast<std::uint32_t>(std::ceil(std::sqrt(double(w) * w + double(h) * h)));
    side += side & 1;
    out.assign(static_cast<std::size_t>(side) * side * 4, 0);
    const double a = deg * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    const double cx = side / 2.0, cy = side / 2.0, hx = w / 2.0, hy = h / 2.0;
    const auto px = [&](int x, int y, int ch) -> double {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h))
            return 0.0;
        const std::uint8_t* p = src.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        return ch == 3 ? p[3] : p[ch] * (p[3] / 255.0); // premultiplied
    };
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            // output -> source: the inverse of a clockwise rotation (y down)
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double sx = c * dx + s * dy + hx - 0.5, sy = -s * dx + c * dy + hy - 0.5;
            const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
            const double fx = sx - x0, fy = sy - y0;
            std::array<double, 4> v{};
            for (int ch = 0; ch < 4; ++ch)
                v[ch] = (px(x0, y0, ch) * (1 - fx) + px(x0 + 1, y0, ch) * fx) * (1 - fy) +
                        (px(x0, y0 + 1, ch) * (1 - fx) + px(x0 + 1, y0 + 1, ch) * fx) * fy;
            std::uint8_t* o = out.data() + (static_cast<std::size_t>(y) * side + x) * 4;
            const double al = v[3];
            o[3] = static_cast<std::uint8_t>(std::lround(std::clamp(al, 0.0, 255.0)));
            for (int ch = 0; ch < 3; ++ch)
                o[ch] = al > 0 ? static_cast<std::uint8_t>(std::lround(
                                     std::clamp(v[ch] * 255.0 / al, 0.0, 255.0)))
                               : 0;
        }
    }
    return true;
}

/// One module instance: the asset library (asset worker thread) and the reader (tick thread).
/// They share no mutable state; the library is thread-safe on its own.
struct Module {
    Module(const EdenDsmodHostApi& api, const char* config)
        : host{api}, assets{fe3h_assets::CreateLibrary()}, reader{api, config} {
        reader.SetAssets(assets.get());
    }

    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink || !assets)
            return EDEN_DSMOD_FALSE;
        std::string_view k{key};
        if (k.starts_with("module:"))
            k.remove_prefix(7);
        if (!k.starts_with("fe3h:"))
            return EDEN_DSMOD_FALSE;
        std::vector<std::uint8_t> rgba;
        std::uint32_t w = 0, h = 0;
        const std::string plain{k};
        bool ok = false;
        if (k.starts_with("fe3h:danger/"))
            ok = reader.Danger().Render(k, rgba, w, h);
        else if (k.starts_with("fe3h:gpsarrow/"))
            ok = GpsArrow(image_host, k.substr(14), rgba, w, h);
        else
            ok = fe3h_assets::LoadImage(*assets, image_host, plain.c_str(), rgba, w, h);
        if (!ok || !w || !h ||
            rgba.size() != static_cast<std::size_t>(w) * h * 4)
            return EDEN_DSMOD_FALSE;
        sink(receiver, w, h, rgba.data(), rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    /// "fe3h:gpsarrow/<deg>", deg a multiple of 5 in 0..355; cached per angle.
    bool GpsArrow(const EdenDsmodHostApi* image_host, std::string_view arg,
                  std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h) {
        int deg = 0;
        if (arg.empty() || arg.size() > 3)
            return false;
        for (const char ch : arg) {
            if (ch < '0' || ch > '9')
                return false;
            deg = deg * 10 + (ch - '0');
        }
        if (deg % 5 != 0 || deg >= 360)
            return false;
        std::scoped_lock lk{arrow_mutex};
        auto& slot = arrows[static_cast<std::size_t>(deg / 5)];
        if (slot.empty()) {
            if (arrow_src.empty()) {
                const std::string key = "fe3h:sprite/" + std::to_string(fe3h_assets::SpritePlayerStar);
                if (!fe3h_assets::LoadImage(*assets, image_host, key.c_str(), arrow_src, arrow_w,
                                            arrow_h) ||
                    !arrow_w || !arrow_h ||
                    arrow_src.size() != static_cast<std::size_t>(arrow_w) * arrow_h * 4) {
                    arrow_src.clear();
                    return false;
                }
            }
            // the star's long tip points down (180 degrees) at rest: turn it to point at `deg`
            RotateSprite(arrow_src, arrow_w, arrow_h, (deg + 180) % 360, slot, arrow_side);
        }
        rgba = slot;
        w = h = arrow_side;
        return true;
    }

    /// Tick thread: find the language the game runs in and follow it. The language value the game
    /// keeps (Reader::GameLanguage) is only a hint: the language is the one whose words in the
    /// romfs message data equal a few words read from the game's memory (fe3h_language.h), which
    /// cannot be wrong about what the game shows. The CJK glyphs of the font depend on it (Han
    /// shapes differ per language), so a language found for the first time or changed is handed to
    /// the asset library and "__font_epoch" counts up: a runtime 18 host then calls decode_font
    /// again and reloads the atlas pages.
    /// Console checks: "fe.lang" (the game's value, -1 = unreadable), "fe.lang_det" (the language
    /// found, -1 = none yet) and "fe.lang_state" (0 nothing yet, 1 working, 2 done, 3 the romfs
    /// could not be read, tried again later).
    void WatchLanguage(const EdenDsmodHostApi* h) {
        using namespace std::chrono_literals;
        ++language_samples;
        const int reader_lang = reader.GameLanguage();
        if (probe.empty() || language_samples % 30 == 0) // a few RAM strings; none before the texts load
            probe = reader.LanguageProbe();
        if (language_job.valid() && language_job.wait_for(0s) == std::future_status::ready) {
            const LanguageResult r = language_job.get();
            language_state = r.complete ? 2 : 3;
            if (r.language >= 0) {
                language_found = r.language;
                if (r.language != font_language) {
                    font_language = r.language;
                    fe3h_assets::SetFontLanguage(*assets, r.language);
                    ++font_epoch;
                }
            }
            if (!r.complete) { // look again later (the romfs was not readable, or no language yet)
                language_retry_at = language_samples + 120;
                job_key.clear();
            }
        }
        const std::string key = probe + '#' + std::to_string(reader_lang);
        if (!language_job.valid() && (!probe.empty() || reader_lang >= 0) && key != job_key &&
            language_samples >= language_retry_at) {
            job_key = key;
            language_state = 1;
            language_job = std::async(std::launch::async, [this, reader_lang, p = probe] {
                return ResolveLanguage(reader_lang, p);
            });
        }
        if (h->publish_i64) {
            h->publish_i64(h->userdata, "__font_epoch", font_epoch);
            h->publish_i64(h->userdata, "fe.lang", reader_lang);
            h->publish_i64(h->userdata, "fe.lang_det", language_found);
            h->publish_i64(h->userdata, "fe.lang_state", language_state);
        }
    }

    struct LanguageResult {
        int language{-1}; ///< the language found, -1 = none
        bool complete{};  ///< the language is settled (else tried again later)
    };

    /// The language of the game: `probe_text` = its words from the game's memory, `hint` = the
    /// game's own language value or -1. Runs off the tick thread (it reads the romfs).
    LanguageResult ResolveLanguage(int hint, const std::string& probe_text) const {
        int language = hint;
        bool readable = true;
        if (!probe_text.empty()) {
            int reads = 0;
            const int found = fe3h_language::MatchLanguage(
                probe_text, hint, [&](int lang, std::uint32_t index, std::string& out) {
                    const bool ok = fe3h_assets::GameText(*assets, &host,
                                                          static_cast<std::uint32_t>(lang), 1, index, out);
                    reads += ok ? 1 : 0;
                    return ok;
                });
            if (found >= 0)
                language = found;
            else if (reads == 0)
                readable = false; // the romfs was not readable: try again later
        }
        return {language, readable && language >= 0};
    }

    int font_language{-1};
    std::int64_t font_epoch{0};
    std::string probe, job_key; ///< the game's words from its memory; what the last job was for
    int language_found{-1};
    int language_state{0};
    std::uint64_t language_samples{0}, language_retry_at{0};
    std::mutex arrow_mutex;
    std::vector<std::uint8_t> arrow_src;
    std::uint32_t arrow_w{}, arrow_h{}, arrow_side{};
    std::array<std::vector<std::uint8_t>, 72> arrows;
    EdenDsmodHostApi host; // create()'s host table: decode_font gets no host of its own
    std::unique_ptr<fe3h_assets::Library> assets;
    Fe3hReader::Reader reader;
    // last: destroyed (and joined) before the library and the host table the job reads
    std::future<LanguageResult> language_job;
};

void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        // create() re-checks all 32 build-id bytes (supports_build saw the hex form).
        if (!Fe3hReader::SupportsBuildId(host->build_id))
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
        if (p && host) {
            auto* m = static_cast<Module*>(p);
            m->reader.Sample(*host);
            m->WatchLanguage(host);
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "FE3H DSMod sample callback failed");
    }
}
void TickCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (p && host)
            static_cast<Module*>(p)->reader.Tick(*host);
    } catch (...) {
    }
}
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

// Font extension: the manifest `font` is a package text file "FE3HFONT serif" (or "sans"); the
// metrics come from the game (fe3h_font.h), the atlas is module:fe3h:font/latin. The runtime calls
// with bounded retries while the game assets become available. A false return lets the host
// retry loading this descriptor; the core font parsers do not understand FE3HFONT.
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, std::size_t size,
                                 void* receiver, EdenDsmodFontSink sink) {
    try {
        auto* m = static_cast<Module*>(p);
        fe3h_assets::FontOut font;
        if (!m || !sink || !m->assets ||
            !fe3h_assets::DecodeFont(*m->assets, &m->host, bytes, size, font))
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
                                   "Fire Emblem: Three Houses DSMod",
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
