// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Mario Kart 8 Deluxe module: ABI glue and the page-readiness outputs.
//   mk8d_reader.{h,cpp}  game-memory reader (race context, racers, local player, items, minimap
//                        projection, horn action), build-pinned code checks, name tables read
//                        from the game
//   mk8d_assets.{h,cpp}  asset-free art, map cameras, message text and font advances from the
//                        player's romfs
//   mk8d_ids.{h,cpp}     id -> name rules
//   mk8d_anim.{h,cpp}    the rank table's card-swap animation (presentation only)
// Supported builds: 4.0.0 (2C336A9BCF79C304...) and 3.0.3 (6A85262F21B90364..., also with the
// CTGP-DX plugin), each with its own verified pins (mk8d_reader.cpp profiles).

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "mk8d_anim.h"
#include "mk8d_assets.h"
#include "mk8d_ids.h"
#include "mk8d_reader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100152000022000);

/// Exactly the builds with a reader profile (mk8d_reader.cpp): 4.0.0 (2C336A9BCF79C304...,
/// AArch64) and 3.0.3 (6A85262F21B90364..., AArch32, with or without CTGP-DX).
EdenDsmodBool SupportsBuild(const char* build_id) {
    return Mk8dReader::SupportsBuildHex(build_id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

/// One module instance: the asset library and the reader that uses it.
struct Module {
    Module(const EdenDsmodHostApi& api, const char* config) : reader{api, assets, config} {
        for (int i = 0; i < Mk8dAnim::MaxCards; ++i) {
            const std::string p = "r" + std::to_string(i) + ".";
            auto& n = anim_names[static_cast<std::size_t>(i)];
            n = {p + "valid", p + "rank", p + "row_y", p + "row_dx", p + "row_glow"};
        }
        const char* log = std::getenv("EDEN_DSMOD_MK8D_ANIM_LOG");
        anim_log = log && *log && *log != '0';
    }
    void Sample(const EdenDsmodHostApi& api) {
        const auto t0 = std::chrono::steady_clock::now();
        reader.Sample(api);
        if (!late_publish) { // a wrapper module may publish these itself, after its own values
            PublishNameScales(api);
            PublishReadiness(api);
            PublishRankAnim(api);
        }
        if (anim_log)
            LogCost(api, t0);
    }
    bool late_publish{false};

    /// The rank table's card-swap animation (mk8d_anim.h): r{i}.row_y (row units, 1.0 = the
    /// rank-1 row; the page places each card with y_bind r{i}.row_y, y_scale = the row pitch),
    /// r{i}.row_dx (row units, + = right; x_bind, x_scale = the row pitch), r{i}.row_glow
    /// (0..3, the overtaking card's glow), rk.moving (cards in motion), rk.pile (a pile-up slide
    /// in progress) and rk.flip (toggles whenever a slide starts or restarts: the page's 60 Hz
    /// redraw trigger). Runs on every call (60 Hz; the reader re-reads race state at 30 Hz), from
    /// the reader's published values, which it never changes.
    void PublishRankAnim(const EdenDsmodHostApi& api) {
        if (!api.get_i64 || !api.publish_i64 || !api.publish_f64)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        const auto get = [&api](const char* name, std::int64_t fallback) {
            return api.get_i64(api.userdata, name, fallback);
        };
        // Race context: a new race (phase back to the start or out of the race scene), another
        // course, race state (not) resolved, another local slot -> the cards snap.
        const std::int64_t ready = get("mk.ready", 0), course = get("mk.course", -1),
                           phase = get("mk.phase", -1), me = get("me.slot", -1);
        if (ready != ctx_ready || course != ctx_course || me != ctx_me || phase < ctx_phase)
            ++ctx_epoch;
        ctx_ready = ready;
        ctx_course = course;
        ctx_me = me;
        ctx_phase = phase;
        Mk8dAnim::Input in;
        in.count = static_cast<int>(
            std::clamp<std::int64_t>(get("racers.count", 0), 0, Mk8dAnim::MaxCards));
        in.context = ctx_epoch;
        for (int i = 0; i < in.count; ++i) {
            const auto k = static_cast<std::size_t>(i);
            const std::int64_t rank = get(anim_names[k][1].c_str(), 0);
            in.valid[k] =
                get(anim_names[k][0].c_str(), 0) != 0 && rank >= 1 && rank <= Mk8dAnim::MaxCards;
            in.rank[k] = in.valid[k] ? static_cast<int>(rank) : 0;
        }
        const double now = std::chrono::duration<double, std::milli>(t0.time_since_epoch()).count();
        const auto snaps = rank_anim.Snaps();
        rank_anim.Update(now, in);
        for (int i = 0; i < Mk8dAnim::MaxCards; ++i) {
            const auto& n = anim_names[static_cast<std::size_t>(i)];
            const Mk8dAnim::Card& c = rank_anim.At(i);
            api.publish_f64(api.userdata, n[2].c_str(), c.y);
            api.publish_f64(api.userdata, n[3].c_str(), c.dx);
            api.publish_i64(api.userdata, n[4].c_str(), c.glow);
        }
        PublishLocalCard(api, in, me);
        // rk.flip toggles whenever a slide (re)starts; the page's 60 Hz trigger group runs one
        // slide duration from each toggle (so it logs one line per slide start, not per tick)
        if (rank_anim.Slides() != flip_slides) {
            flip_slides = rank_anim.Slides();
            anim_flip = !anim_flip;
        }
        api.publish_i64(api.userdata, "rk.moving", rank_anim.Moving());
        api.publish_i64(api.userdata, "rk.pile", rank_anim.Pileup() ? 1 : 0);
        api.publish_i64(api.userdata, "rk.flip", anim_flip ? 1 : 0);
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                .count();
        anim_sum_us += us;
        anim_max_us = std::max(anim_max_us, us);
        // EDEN_DSMOD_MK8D_ANIM_LOG=1 (dev): one line per call while a card moves or snapped,
        // so the positions can be plotted per tick.
        if (anim_log && api.log && (rank_anim.Moving() > 0 || rank_anim.Snaps() != snaps)) {
            std::string line = "MK8D anim t=" + std::to_string(static_cast<long long>(now)) +
                               " mv=" + std::to_string(rank_anim.Moving()) +
                               " pile=" + (rank_anim.Pileup() ? "1" : "0") +
                               (rank_anim.Snaps() != snaps ? " SNAP" : "") + " y/dx/glow=";
            char buf[48];
            for (int i = 0; i < in.count; ++i) {
                const Mk8dAnim::Card& c = rank_anim.At(i);
                std::snprintf(buf, sizeof(buf), "%s%d:%d:%.3f/%.3f/%d", i ? " " : "", i,
                              in.rank[static_cast<std::size_t>(i)], c.y, c.dx, c.glow);
                line += buf;
            }
            api.log(api.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
        }
    }
    /// The local player's card is drawn once more on top of all cards. So that the page needs one
    /// copy of it (not twelve gated ones), mc.* = the local racer's own published values, copied
    /// unchanged: mc.row_y / mc.row_dx / mc.row_glow (its card position), mc.icon, mc.name,
    /// mc.item0_key, mc.item1_key, mc.item_state, mc.item1_state, mc.finished, mc.name_scale.
    /// Published only while me.slot names a valid racer (the page gates the card on ui.me).
    void PublishLocalCard(const EdenDsmodHostApi& api, const Mk8dAnim::Input& in, std::int64_t me) {
        if (me < 0 || me >= in.count || !in.valid[static_cast<std::size_t>(me)])
            return;
        const Mk8dAnim::Card& c = rank_anim.At(static_cast<int>(me));
        api.publish_f64(api.userdata, "mc.row_y", c.y);
        api.publish_f64(api.userdata, "mc.row_dx", c.dx);
        api.publish_i64(api.userdata, "mc.row_glow", c.glow);
        const std::string p = "r" + std::to_string(me) + ".";
        static constexpr const char* Texts[] = {"icon", "name", "item0_key", "item1_key"};
        static constexpr const char* Ints[] = {"item_state", "item1_state", "finished",
                                               "name_scale"};
        if (api.get_text && api.publish_text)
            for (const char* f : Texts)
                if (const char* v = api.get_text(api.userdata, (p + f).c_str()))
                    api.publish_text(api.userdata, (std::string{"mc."} + f).c_str(), v);
        for (const char* f : Ints) {
            const std::int64_t missing = INT64_MIN;
            const std::int64_t v = api.get_i64(api.userdata, (p + f).c_str(), missing);
            if (v != missing)
                api.publish_i64(api.userdata, (std::string{"mc."} + f).c_str(), v);
        }
    }
    /// EDEN_DSMOD_MK8D_ANIM_LOG=1: whole-sample (reader + glue) and animation cost per 600 calls.
    void LogCost(const EdenDsmodHostApi& api, std::chrono::steady_clock::time_point t0) {
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                .count();
        cost_sum_us += us;
        cost_max_us = std::max(cost_max_us, us);
        if (++cost_n < 600)
            return;
        if (api.log) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "MK8D module: sample avg %.1f us max %.1f us; rank anim avg %.2f us max "
                          "%.1f us over %d calls",
                          cost_sum_us / cost_n, cost_max_us, anim_sum_us / cost_n, anim_max_us,
                          cost_n);
            api.log(api.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        cost_sum_us = cost_max_us = anim_sum_us = anim_max_us = 0;
        cost_n = 0;
    }
    Mk8dAnim::RankTable rank_anim;
    std::array<std::array<std::string, 5>, Mk8dAnim::MaxCards> anim_names;
    std::int64_t ctx_ready{-2}, ctx_course{-2}, ctx_me{-2}, ctx_phase{-2};
    std::uint64_t ctx_epoch{};
    bool anim_flip{};
    std::uint64_t flip_slides{};
    bool anim_log{};
    double cost_sum_us{}, cost_max_us{}, anim_sum_us{}, anim_max_us{};
    int cost_n{};

    /// The waiting / loading cards appear only with their core data, and each image slot only
    /// with a decodable image. mk.pict_ok / mk.cup_ok = the published key is non-empty AND this
    /// module decodes it (checked off the tick thread; the image then sits in the library cache
    /// for the page's own request); mk.name_ok = mk.course_name is non-empty.
    /// One check runs at a time per output: a key that changes while a check is running is
    /// checked when that one ends (dropping a running std::async future would block the tick
    /// thread until its decode finishes). The last finished result is kept for its key.
    struct KeyCheck {
        std::string key;     ///< the published key
        std::string job_key; ///< the key of the running / last finished check
        std::future<bool> job;
        bool job_done{};
        bool job_ok{};
    };
    void CheckKey(const EdenDsmodHostApi& api, KeyCheck& c, const char* source, const char* out) {
        const char* key = api.get_text(api.userdata, source);
        c.key = key ? key : "";
        if (c.job.valid() && c.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            c.job_ok = c.job.get();
            c.job_done = true;
            if (api.log)
                api.log(api.userdata, EDEN_DSMOD_LOG_INFO,
                        ("MK8D DSMod: " + std::string(out) + " = " + (c.job_ok ? "1" : "0") + " (" +
                         c.job_key + ")")
                            .c_str());
        }
        if (!c.job.valid() && !c.key.empty() && c.job_key != c.key) {
            Mk8dAssets::Library* lib = &assets;
            const EdenDsmodHostApi host = api;
            c.job_key = c.key;
            c.job_done = false;
            c.job = std::async(std::launch::async, [lib, host, k = c.key] {
                const auto image = lib->LoadImage(host, k);
                return image && !image->rgba.empty();
            });
        }
        const bool ok = !c.key.empty() && c.job_done && c.job_key == c.key && c.job_ok;
        api.publish_i64(api.userdata, out, ok ? 1 : 0);
    }
    void PublishReadiness(const EdenDsmodHostApi& api) {
        if (!api.get_text || !api.publish_i64)
            return;
        CheckKey(api, pict_check, "mk.pict_key", "mk.pict_ok");
        CheckKey(api, cup_check, "mk.cup_key", "mk.cup_ok");
        // the map tiles likewise (the page falls back from the fit crop to the whole map, and
        // draws no tile when neither decodes)
        CheckKey(api, fit_check, "mk.mapfit_key", "mk.fit_img");
        CheckKey(api, map_check, "mk.map_key", "mk.map_img");
        const char* name = api.get_text(api.userdata, "mk.course_name");
        api.publish_i64(api.userdata, "mk.name_ok", name && *name ? 1 : 0);
        // The course name's width in font units (sum of the page font's CWDH advances,
        // the same measure as r{i}.name_scale). The page derives the name's line count from it
        // (the runtime wraps on spaces only when the whole name is wider than the column), so the
        // cup / class lines sit right under a 1-line name. Published once the font is read.
        if (font)
            api.publish_i64(api.userdata, "mk.course_name_w",
                            name && *name ? static_cast<std::int64_t>(font->Measure(name)) : 0);
    }

    /// r{i}.name_scale = the largest text scale (5, 4, 3, 2) at which the
    /// racer's published name is no wider than "Rosalina" at scale 5, measured with the page
    /// font's own advance widths (UI/USen/font.sarc#turbo_MARIOFont.bffnt CWDH): the LONG row's
    /// name column is exactly that wide, so no name is ever cropped. The font's advances are
    /// read once, off the tick thread; until then nothing is published (the page hides names).
    void PublishNameScales(const EdenDsmodHostApi& api) {
        if (!api.get_text || !api.publish_i64)
            return;
        if (!font) {
            if (font_failed)
                return; // the page keeps names hidden
            if (!font_job.valid()) {
                Mk8dAssets::Library* lib = &assets;
                const EdenDsmodHostApi host = api;
                font_job = std::async(std::launch::async, [lib, host] {
                    return lib->LoadFontAdvances(host, "USen", "turbo_MARIOFont.bffnt");
                });
            }
            if (font_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            font = font_job.get();
            if (!font) {
                font_failed = true; // not retried (the library caches the failure too)
                return;
            }
            reference = font->Measure("Rosalina") * 5u;
        }
        for (int i = 0; i < 12; ++i) {
            const std::string prefix = "r" + std::to_string(i) + ".";
            const char* name = api.get_text(api.userdata, (prefix + "name").c_str());
            if (!name || !*name)
                continue;
            auto it = scale_of.find(name);
            if (it == scale_of.end()) {
                const std::uint32_t w = font->Measure(name);
                int scale = 1;
                for (const int s : {5, 4, 3, 2}) {
                    if (w * static_cast<std::uint32_t>(s) <= reference) {
                        scale = s;
                        break;
                    }
                }
                if (scale_of.size() > 256)
                    scale_of.clear();
                it = scale_of.emplace(name, scale).first;
            }
            api.publish_i64(api.userdata, (prefix + "name_scale").c_str(), it->second);
        }
    }
    ~Module() {
        reader.Shutdown();
    }
    void Configure(const EdenDsmodHostExtensions* ext) {
        const bool usable = ext && ext->version == EDEN_DSMOD_EXT_VERSION &&
                            ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
                            ext->struct_size >= sizeof(*ext);
        assets.SetAstcDecoder(usable ? Mk8dAssets::AstcDecoder{ext->userdata, ext->decode_astc}
                                     : Mk8dAssets::AstcDecoder{});
    }
    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const std::string_view public_key{key};
        if (!public_key.starts_with("module:mk8d:"))
            return EDEN_DSMOD_FALSE;
        const auto image = assets.LoadImage(*image_host, public_key);
        if (!image || image->rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    Mk8dAssets::Library assets; ///< declared before the reader that references it
    Mk8dReader::Reader reader;
    std::future<std::shared_ptr<const Mk8dAssets::FontAdvances>> font_job;
    std::shared_ptr<const Mk8dAssets::FontAdvances> font;
    bool font_failed{};
    std::uint32_t reference{};
    std::unordered_map<std::string, int> scale_of;
    KeyCheck pict_check, cup_check, fit_check, map_check;
};

void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        // create() re-checks all 32 build-id bytes (supports_build saw the hex form).
        if (!Mk8dReader::SupportsBuildId(host->build_id))
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
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "MK8D DSMod sample callback failed");
    }
}
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}
/// Module actions: "mk.horn_write" (the horn, Reader::HornStep). USE ITEM stays a manifest press
/// of the game's own item button.
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        return p && static_cast<Module*>(p)->reader.OnAction(action, argument) ? EDEN_DSMOD_TRUE
                                                                               : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
/// Write extension: the horn pulse is a guarded write_batch (expect-bytes checked).
void ConfigureWriteCallback(void* p, const EdenDsmodHostWriteApi* h) {
    try {
        if (!p || !h || h->version != EDEN_DSMOD_WRITE_EXT_VERSION ||
            h->struct_size != sizeof(EdenDsmodHostWriteApi) ||
            h->abi_hash != EDEN_DSMOD_WRITE_EXT_HASH || !h->write_batch)
            return;
        static_cast<Module*>(p)->reader.SetWriteApi(*h);
    } catch (...) {
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

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Mario Kart 8 Deluxe DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
    &ActionCallback,        &LoadImageCallback};
const EdenDsmodModuleWriteExtensions WriteExtensions{
    EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(EdenDsmodModuleWriteExtensions), EDEN_DSMOD_WRITE_EXT_HASH,
    &ConfigureWriteCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
