// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Super Mario Odyssey module: ABI glue. The reader (0100000000010000.h / smo_reader.cpp)
// resolves StageScene -> GameDataHolder -> gameData once per boot (heap-scan-by-vtable,
// re-validated every sample) and publishes the slice's live fields.
//
// Merged font atlas: the actual BFFNT/BNTX decode and atlas packing live in smo_font.{h,cpp};
// this file only fetches the six raw .bffnt byte blobs via host->read_romfs (the host already
// does Yaz0+SARC decode for a `#member` romfs path) and wires decode_font/load_image to
// SmoFont::BuildFontAtlasFrom/AtlasPage. See the package manifest's own "_font"/"_font_atlas"
// comments for the manifest-side half of this (the font_metrics_src marker file and the paged
// font_atlas key).
//
// Capabilities: EXTENSIONS (load_image/decode_font) and ROMFS_READ (host->read_romfs for the six
// font members). Heap reads for the coin reader need no capability bit (MODULE_GUIDE.md section
// 1.3 table).

#include "0100000000010000.h"
#include "smo_font.h"
#include "smo_save.h"
#include "smo_map.h"
#include "smo_capture.h"
#include "smo_progress.h"
#include "smo_world.h"
#include "smo_romfs_tables.h"
#include "smo_ui.h"
#include "smo_hud.h"
#include "smo_warp.h"
#include <cstddef>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace dsmod_sdk::int_types;

EdenDsmodBool SupportsBuild(const char* build_id) {
    return SmoReader::SupportsBuildHex(build_id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

// ---- merged font: six raw .bffnt blobs, fetched opportunistically, then packed once -----------

/// Index order matches SmoFont::BuildFontAtlasFrom's parameter order.
enum class FontSlot { Message, Title, Head, Number, Picture, Icon, Count };
constexpr std::array<std::string_view, 5> kFontMembers = {
    // Message is read first, by name, below (kept out of this array only so its index lines up
    // with FontSlot::Title.. without an off-by-one; see FontMemberName()).
    "TitleFont60.bffnt", "HeadFont72.bffnt", "NumberFont26.bffnt", "PictureFont80.bffnt",
    "IconFont80.bffnt",
};
constexpr std::string_view FontMemberName(FontSlot slot) {
    return slot == FontSlot::Message ? std::string_view{"MessageFont38.bffnt"}
                                     : kFontMembers[static_cast<std::size_t>(slot) - 1];
}
// Verified host quirk (packages/SuperMarioOdyssey's own manifest comment): the 1.4.1 update's
// FontData.szs is a truncated Yaz0 stream
// for every language; base 1.0's copy decodes cleanly. Always read from base:, never romfs:.
constexpr std::string_view kFontArchive = "base:/LocalizedData/USen/LayoutData/FontData.szs#";
// Rows per atlas page (must match the manifest's font_page_h) and the module: key prefix
// load_image receives verbatim (MODULE_GUIDE.md section 1.4: "Keys include the module: prefix").
constexpr std::string_view kImageKeyPrefix = "module:smo:font_page_";
// The manifest's font_metrics_src is a tiny marker file, not game data (see RequestNxFont's
// ".bffnt"/IsNxAssetSource heuristic in mod_nx_runtime.cpp: a `file:` source never matches it, so
// the host always falls through to this module's decode_font instead of trying its own built-in
// BFFNT parser on a one-line text file).
constexpr std::string_view kFontMarker = "smo-font 1";

/// One of the six font members: its raw bytes once fetched, and a simple once-per-second retry
/// throttle (StartScan/kScanIdleTicks' sibling in smo_reader.cpp) so a romfs that is not mounted
/// yet does not get re-read 60 times a second.
struct FontSource {
    std::vector<u8> bytes;
    bool ready = false;
    int idle_ticks = 1 << 30; // "start immediately" (see smo_reader.cpp's identical trick)
};

constexpr int kFontRetryIdleTicks = 90; // 1.5s at 60Hz, matching smo_reader.cpp's kScanIdleTicks

/// Tries to fill in any not-yet-ready font source via host->read_romfs. Cheap once every font is
/// ready (every call after that is a no-op); before that, throttled to roughly once every 1.5s
/// per missing font so a game romfs that is not mounted yet is not hammered every tick.
void TryFetchFonts(const EdenDsmodHostApi& host, std::array<FontSource, 6>& sources) {
    if (!host.read_romfs) {
        return;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        FontSource& src = sources[i];
        if (src.ready) {
            continue;
        }
        if (src.idle_ticks < kFontRetryIdleTicks) {
            ++src.idle_ticks;
            continue;
        }
        src.idle_ticks = 0;
        const auto name = FontMemberName(static_cast<FontSlot>(i));
        std::string path;
        path.reserve(kFontArchive.size() + name.size());
        path += kFontArchive;
        path += name;
        const std::size_t size =
            host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
        if (size == 0 || size > (16u << 20)) {
            continue; // not ready yet (romfs not mounted) or implausibly large: retry later
        }
        std::vector<u8> bytes(size);
        if (host.read_romfs(host.userdata, path.c_str(), 0, bytes.data(), size) != size) {
            continue;
        }
        src.bytes = std::move(bytes);
        src.ready = true;
    }
}

bool AllFontsReady(const std::array<FontSource, 6>& sources) {
    for (const auto& s : sources) {
        if (!s.ready) {
            return false;
        }
    }
    return true;
}

// ---- native table joins and active-save progress polling -------------------------------

// Checked on every sample, but the save file itself is only re-read (and re-parsed) at most this
// often, and only if its contents actually changed -- "poll at a modest interval... never per
// frame". 180 ticks = 3s at 60Hz.
constexpr int kSaveRetryIdleTicks = 180;

/// The native-table joins, built off the tick thread (SmoRomfsTables::Build() measured ~1.5-2.3s
/// on desktop: twenty-odd archive decodes plus 22 SmoMap::DecodeArchive texture decodes for
/// world-map-variants.tsv -- a single frame stall at that cost is not acceptable on the tick
/// thread, which the game and every other sample()/tick() call share). Follows the same pattern
/// as the Metroid Dread module's MapJob: a worker thread the module itself starts, polled by
/// Done() rather than waited on, with the result installed on the tick thread once ready.
struct TablesJob {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    SmoRomfsTables::Tables result; // written by the worker before `done` is set; read-only after

    bool Done() {
        std::scoped_lock lock{mutex};
        return done;
    }
};

void RunTablesJob(TablesJob& job, EdenDsmodHostApi host) {
    auto built = SmoRomfsTables::Build(host);
    std::scoped_lock lock{job.mutex};
    job.result = std::move(built);
    job.done = true;
    job.cv.notify_all();
}

/// Starts the job on its own thread; on a platform where std::thread throws (no thread support),
/// falls back to running it inline on the calling (tick) thread -- correct, just not async.
std::shared_ptr<TablesJob> StartTablesJob(const EdenDsmodHostApi& host) {
    auto job = std::make_shared<TablesJob>();
    try {
        job->thread = std::thread([job, api = host]() mutable { RunTablesJob(*job, api); });
    } catch (...) {
        RunTablesJob(*job, host);
    }
    return job;
}

struct SaveState {
    EdenDsmodHostSaveApi save_api{};
    bool have_save_api = false;
    std::string poll_status;
    unsigned active_file = 0;
    int poll_idle_ticks = 1 << 30; // "start immediately" (see FontSource's identical trick)
    SmoSave::ProgressCache cache;
    SmoSave::SaveProgress progress;

    std::shared_ptr<TablesJob> tables_job; // set once; retried (a fresh job) only if it failed
    bool tables_installed = false;
    std::optional<SmoSave::KingdomTables> tables;
    std::vector<SmoSave::CheckpointLocation> checkpoint_locations;
    std::shared_ptr<const SmoWorld::Catalog> catalog;
    std::vector<SmoSave::Task> tasks;
    std::optional<SmoSave::TaskRules> task_rules;
    std::map<std::string, std::string> capture_names;
    // Stats-tab extension: static item_name catalogues, parsed once.
    std::optional<std::set<std::string>> costume_cap_names, costume_cloth_names, souvenir_names;
};

/// Reads one whole `file:` package asset (the package's own bundled data, always available once
/// the module is loaded -- unlike the game's romfs, there is no "not mounted yet" to retry for).
std::string ReadPackageFile(const EdenDsmodHostApi& host, const char* path) {
    if (!host.read_romfs) {
        return {};
    }
    const std::size_t size = host.read_romfs(host.userdata, path, 0, nullptr, 0);
    if (size == 0 || size > (4u << 20)) {
        return {};
    }
    std::string buf(size, '\0');
    if (host.read_romfs(host.userdata, path, 0, buf.data(), size) != size) {
        return {};
    }
    return buf;
}

/// Static tables load once. Active-slot changes clear cached progress before polling;
/// a transient read failure may retain progress only for the same validated save slot.
void PollSave(SaveState& save, const EdenDsmodHostApi& host, unsigned active_file) {
    if (save.active_file != active_file) {
        save.active_file = active_file;
        save.cache = {};
        save.progress = {};
        save.poll_idle_ticks = kSaveRetryIdleTicks;
    }
    // Native-table joins are built from the player's own romfs, not shipped as package TSVs
    // (steward ruling 2026-10-24: "no game asset inside" -- see smo_romfs_tables.h), on the
    // TablesJob worker thread (never the tick thread: see its own comment on SaveState). Starts
    // once; if the build fails outright (romfs not mounted yet at boot), a fresh job is started
    // on a later sample so it keeps retrying, same as every other romfs read in this file.
    if (!save.tables_job) {
        save.tables_job = StartTablesJob(host);
    } else if (!save.tables_installed && save.tables_job->Done()) {
        // The worker has already returned (Done() only turns true after it does): joining it here
        // cannot block, and must happen before the job can be replaced or dropped (a joinable
        // std::thread's destructor would otherwise call std::terminate).
        if (save.tables_job->thread.joinable()) save.tables_job->thread.join();
        const SmoRomfsTables::Tables& built = save.tables_job->result;
        if (built.system_data_ok) {
            save.tables_installed = true;
            const SmoWorld::Tables tables{built.world_maps, built.world_stages, built.world_map_invalid,
                                          built.world_map_anchors, built.world_checkpoints, built.world_map_icons,
                                          built.world_objectives, built.world_map_variants, built.world_map_moons};
            if (auto c = SmoWorld::ParseTables(tables))
                save.catalog = std::make_shared<SmoWorld::Catalog>(std::move(*c));
            for (const auto& pair : SmoSave::ParseNamePairsTsv(built.captures))
                save.capture_names.emplace(pair);
            save.task_rules=SmoSave::ParseTaskRules(
                built.task_shine_rules, built.task_world_rules, built.task_item_rules,
                built.task_shop_rules, built.moons);
            save.tasks = SmoSave::ParseTaskTable(built.task_details);
            save.checkpoint_locations = SmoSave::ParseCheckpointTable(built.checkpoints);
            if (!built.moons.empty() && !built.purple_coins.empty() && !built.kingdoms.empty()) {
                save.tables = SmoSave::ParseKingdomTables(built.moons, built.purple_coins, built.kingdoms);
            }
            save.costume_cap_names = SmoSave::ParseItemNameColumn(built.costumes_cap);
            save.costume_cloth_names = SmoSave::ParseItemNameColumn(built.costumes_cloth);
            save.souvenir_names = SmoSave::ParseItemNameColumn(built.items_gift);
        } else {
            save.tables_job = nullptr; // the whole build failed (no archive was readable yet): retry
        }
    }

    const auto report = [&](std::string status) {
        if (save.poll_status == status) return;
        save.poll_status = std::move(status);
        if (host.log) host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
            ("SMO save: " + save.poll_status).c_str());
    };
    if (!active_file) { report("waiting for active save slot"); return; }
    if (!save.have_save_api || !save.save_api.read_save_file) {
        report("save API unavailable"); return;
    }
    if (save.poll_idle_ticks < kSaveRetryIdleTicks) {
        ++save.poll_idle_ticks;
        return;
    }
    save.poll_idle_ticks = 0;
    const auto filename = "File" + std::to_string(active_file) + ".bin";
    const std::size_t size =
        save.save_api.read_save_file(save.save_api.userdata, filename.c_str(), 0, nullptr, 0);
    if (size == 0 || size > (64u << 20)) {
        report(filename + " unavailable (size " + std::to_string(size) + ")");
        return; // no save yet (a fresh profile), or an implausible size
    }
    std::vector<u8> bytes(size);
    if (save.save_api.read_save_file(save.save_api.userdata, filename.c_str(), 0, bytes.data(), size) !=
        size) {
        report(filename + " read incomplete"); return;
    }
    save.cache.Update(bytes, save.progress, save.task_rules ? &*save.task_rules : nullptr);
    report(filename + (save.progress.valid ? " progress ready" : " parse unavailable"));
}

/// One module instance: the coin/HUD reader, the merged-font state (six fetched byte blobs plus
/// the atlas built from them, cached once) and the save-poll state. `font_mutex` guards the font
/// fields: TryFetchFonts and DecodeFontFn run on whichever thread calls sample()/LoadFont (see
/// MODULE_GUIDE.md section 1.2; not documented as a fixed thread for decode_font, so this module
/// does not assume it is the tick thread), while LoadImageFn runs on the asset worker and only
/// ever reads `atlas`. `save_mutex` guards the save fields the same way (configure() and
/// Sample()'s poll are the only writers; nothing reads save state off the tick thread yet).
struct Instance {
    EdenDsmodHostApi host{};
    SmoHud::Controller hud;
    EdenDsmodHostWriteApi write_api{};
    std::atomic<bool> warp_requested{false};
    bool warp_waiting=false;
    unsigned warp_wait_ticks=0;
    s64 warp_slot=-1;
    s32 warp_world=-1;
    std::atomic<bool> hide_hud{true};
    SmoReader::Reader reader;
    std::atomic<s64> selected_flag{-1};
    std::atomic<s64> selected_task{0};

    std::mutex font_mutex;
    std::array<FontSource, 6> font_sources;
    std::shared_ptr<const SmoFont::FontAtlas> atlas; // built once every source is ready

    std::mutex save_mutex;
    SaveState save;
    // Decode on the asset worker, outside this lock; sample() only copies the immutable pointer.
    std::mutex capture_mutex;
    EdenDsmodAstcDecoder decode_astc = nullptr;
    void* astc_userdata = nullptr;
    std::shared_ptr<const std::vector<u8>> capture_archive;
    std::mutex map_mutex;
    std::map<std::string, std::shared_ptr<const SmoMap::Resources>> maps;
    std::shared_ptr<const std::vector<u8>> map_archive;
    unsigned projection_retry_ticks = 0;
    unsigned objective_ticks = 0;
};

void* Create(const EdenDsmodHostApi* host, const char* /*config_json*/) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != SmoReader::TitleId ||
            !host->is_mapped || !host->read_memory) {
            return nullptr;
        }
        // supports_build() already checked the hex form; create() re-checks all 32 raw bytes
        // again, per the P5R precedent documented in MODULE_GUIDE.md section 3 ("P5R accepts
        // only the exact 64-character ID, and create compares all 32 bytes of host->build_id
        // again"). A module must never imply support for an unlisted build.
        if (!SmoReader::SupportsBuildBytes(host->build_id)) {
            return nullptr;
        }
        auto inst = std::make_unique<Instance>();
        inst->host=*host;
        // Best-effort immediate fetch (MODULE_GUIDE.md section 1.2: "Since runtime 16 the host is
        // fully usable inside [create()]: read_romfs works for every source"). If the game romfs
        // is not mounted yet this simply fails silently and Sample() keeps retrying.
        TryFetchFonts(*host, inst->font_sources);
        return inst.release();
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* p) {
    try {
        if(p){
            auto* inst=static_cast<Instance*>(p);
            inst->hud.Restore(inst->host);
            // A joinable std::thread's destructor calls std::terminate(); SmoRomfsTables::Build()
            // has no early-abort, so a shutdown shortly after load blocks here for up to its own
            // runtime (measured ~1.5-2.3s) rather than crashing. One-time cost, not a frame stall.
            if (inst->save.tables_job && inst->save.tables_job->thread.joinable())
                inst->save.tables_job->thread.join();
            delete inst;
        }
    } catch (...) {
    }
}

void ConfigureSave(void* p, const EdenDsmodHostSaveApi* api) {
    try {
        if (!p || !api) {
            return;
        }
        auto& inst = *static_cast<Instance*>(p);
        std::scoped_lock lock{inst.save_mutex};
        inst.save.save_api = *api; // borrowed for this call only (MODULE_GUIDE.md section 2.3): copy it
        inst.save.have_save_api = true;
    } catch (...) {
    }
}

void Sample(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!p || !host) {
            return;
        }
        auto& inst = *static_cast<Instance*>(p);
        inst.host=*host;
        SmoReader::Snapshot snap;
        inst.reader.Sample(*host, snap);

        {
            std::scoped_lock lock{inst.font_mutex};
            TryFetchFonts(*host, inst.font_sources);
        }

        const auto progress = snap.chain_ready ? SmoProgress::Read(*host,snap.game_data,snap.world_id) : SmoProgress::Snapshot{};
        const int moon_slots=progress.moon_row_valid ? progress.moon_row+
            (progress.moon_requirement_valid?progress.moon_missing:0) : 0;
        const auto map_footer=SmoMap::LayoutForMoonSlots(moon_slots);
        std::shared_ptr<const SmoWorld::Catalog> catalog;
        SmoSave::SaveProgress saved;
        bool kingdom_known = false;
        SmoSave::KingdomProgress kingdom{};
        struct StatsRow { SmoSave::KingdomInfo info; SmoSave::KingdomProgress progress; };
        std::vector<StatsRow> stats_rows;
        SmoSave::VisitedMoonTotals globe_total; // "Collected, all visited kingdoms" pill
        std::vector<std::pair<std::string, std::string>> portraits;
        std::size_t captures_seen = 0;
        bool stats_valid = false;
        bool tasks_available = false, tasks_locked = false;
        std::size_t tasks_claimed = 0;
        std::vector<SmoSave::Task> tasks;
        std::vector<SmoSave::TaskProgress> task_progress;
        std::vector<SmoSave::CheckpointLocation> touched;
        std::vector<SmoSave::CheckpointLocation> locations;
        bool tables_ready = false;
        {
            std::scoped_lock lock{inst.save_mutex};
            PollSave(inst.save, *host, snap.chain_ready ? SmoProgress::ActiveSaveFile(*host, snap.game_data_holder, snap.game_data) : 0);
            tables_ready = inst.save.tables_installed;
            catalog = inst.save.catalog;
            saved = inst.save.progress;
            tasks_available = snap.chain_ready && inst.save.progress.achievements_available.value_or(false);
            tasks_locked = snap.chain_ready && inst.save.progress.achievements_available == false;
            tasks = inst.save.tasks;
            tasks_claimed = inst.save.progress.achievements_claimed.size();
            stats_valid = snap.chain_ready && inst.save.progress.valid && inst.save.tables.has_value();
            if (stats_valid) {
                for (const auto& info : inst.save.tables->kingdoms) {
                    if (std::find(inst.save.progress.visited_world_ids.begin(), inst.save.progress.visited_world_ids.end(), info.world_id)
                        != inst.save.progress.visited_world_ids.end())
                        stats_rows.push_back({info, SmoSave::ForKingdom(inst.save.progress, *inst.save.tables, info.world)});
                }
                // Same visited-kingdoms rows as stats_rows/globe_rows above, summed once for the
                // globe page's top-right "Collected, all visited kingdoms" pill ("Spoiler
                // handling": unvisited kingdoms are excluded from both the numerator and the
                // denominator).
                globe_total = SmoSave::SumVisitedKingdomMoons(inst.save.progress, *inst.save.tables);
                for (const auto& capture : inst.save.progress.captures_seen) {
                    const auto name = inst.save.capture_names.find(capture);
                    if (name == inst.save.capture_names.end()) continue;
                    ++captures_seen;
                    if (portraits.size() < 52) portraits.emplace_back(capture, name->second);
                }
            }
            for (const auto& task : tasks) {
                auto value=SmoSave::ForTask(inst.save.progress,task);
                if(task.label.starts_with("Shine_Gather_") && progress.unbanked_valid){
                    value.current=progress.acquired_units;
                    value.ready=!value.claimed && task.target && *value.current>=*task.target;
                }
                task_progress.push_back(value);
            }
            if (inst.save.progress.valid && inst.save.tables && catalog) {
                if (const auto* world = SmoWorld::WorldForId(*catalog,snap.world_id)) {
                    kingdom = SmoSave::ForKingdom(saved, *inst.save.tables, world->key);
                    kingdom_known = snap.chain_ready && kingdom.known;
                }
            }
        }

        if (host->publish_i64) {
            // The native-table joins (map catalogue, task rules, kingdom/stats joins) build on a
            // background worker (TablesJob) and take on the order of a second or two; every page
            // that reads them already degrades gracefully while they're unavailable (catalog is
            // null, tasks is empty, stats_valid is false), but a manifest page can bind this to
            // show an explicit "loading" state for that window instead of a bare empty page.
            host->publish_i64(host->userdata, "smo.tables_ready", tables_ready);
            host->publish_i64(host->userdata, "smo.stats_valid", stats_valid);
            host->publish_i64(host->userdata, "smo.captures_seen", captures_seen);
            host->publish_i64(host->userdata, "smo.portrait_count", portraits.size());
            host->publish_i64(host->userdata, "smo.captures_total", 52);
            host->publish_i64(host->userdata, "smo.captures_unknown", 52 - std::min<std::size_t>(52, captures_seen));
            host->publish_i64(host->userdata, "smo.captures_have_unknown", stats_valid && captures_seen < 52);
        }
        for (std::size_t i = 0; i < 52; ++i) {
            const auto key = "smo.portrait" + std::to_string(i);
            const bool present = i < portraits.size();
            if (host->publish_i64) host->publish_i64(host->userdata, (key + ".visible").c_str(), present);
            if (present && host->publish_text) {
                const auto src = "module:smo:capture/" + portraits[i].first;
                host->publish_text(host->userdata, (key + ".src").c_str(), src.c_str());
                host->publish_text(host->userdata, (key + ".name").c_str(), portraits[i].second.c_str());
            }
        }
        for (std::size_t i = 0; i < 17; ++i) {
            const auto key = "smo.stats" + std::to_string(i);
            const bool present = i < stats_rows.size();
            if (host->publish_i64) host->publish_i64(host->userdata, (key + ".visible").c_str(), present);
            if (!present) continue;
            const auto& row = stats_rows[i];
            const bool moon_complete = row.progress.moons >= row.progress.moon_total;
            const bool coin_present = row.progress.coin_capacity > 0;
            const bool coin_complete = row.progress.coins >= row.progress.coin_capacity;
            const bool verified_coin = row.info.world == "Cap" || row.info.world == "Waterfall" || row.info.world == "Sand" || row.info.world == "Peach";
            const auto coin_src = verified_coin ? "regional_" + row.info.world : "regional_generic";
            const auto moon_src = "moon_" + std::to_string(row.info.shine_type);
            const auto moons = SmoFont::Styled(std::to_string(row.progress.moons) + "/" + std::to_string(row.progress.moon_total), SmoFont::Role::Number);
            const auto coins = SmoFont::Styled(std::to_string(row.progress.coins) + "/" + std::to_string(row.progress.coin_capacity), SmoFont::Role::Number);
            if (host->publish_i64) {
                host->publish_i64(host->userdata, (key + ".moon_complete").c_str(), moon_complete);
                host->publish_i64(host->userdata, (key + ".moon_partial").c_str(), !moon_complete);
                host->publish_i64(host->userdata, (key + ".coin_complete").c_str(), coin_present && coin_complete);
                host->publish_i64(host->userdata, (key + ".coin_partial").c_str(), coin_present && !coin_complete);
                host->publish_i64(host->userdata, (key + ".coin_visible").c_str(), coin_present);
                host->publish_i64(host->userdata, (key + ".coin_tint").c_str(), verified_coin ? 0 : 1);
            }
            if (host->publish_text) {
                auto name = row.info.name;
                constexpr std::string_view suffix = " Kingdom";
                if (name.ends_with(suffix)) name.resize(name.size() - suffix.size());
                host->publish_text(host->userdata, (key + ".name").c_str(), name.c_str());
                host->publish_text(host->userdata, (key + ".moons").c_str(), moons.c_str());
                host->publish_text(host->userdata, (key + ".coins").c_str(), coins.c_str());
                host->publish_text(host->userdata, (key + ".moon_src").c_str(), moon_src.c_str());
                host->publish_text(host->userdata, (key + ".coin_src").c_str(), coin_src.c_str());
            }
        }

        // ---- D: Stats-tab Collections strip (souvenir/music/costume-set ownership) -----------
        // Souvenir and costume-set ownership are confirmed live save counters (ItemGift[] /
        // ItemCap[]+ItemCloth[] joined by item_name against the package's own static catalogues).
        // Music ownership is CollectBgmList[]'s IsCollected field (save format notes,
        // RE batch 9 "82/82" live finding) -- a real save/data source, not left unproven; if the
        // array itself is absent or empty on a save this build cannot read, music_valid stays
        // false and the page shows the native unavailable treatment, never a fabricated number.
        const bool collections_valid = stats_valid && inst.save.costume_cap_names &&
            inst.save.costume_cloth_names && inst.save.souvenir_names;
        int souvenir_owned = 0, souvenir_total = 0, costume_owned = 0, costume_total = 0;
        if (collections_valid) {
            const std::set<std::string> owned_caps(saved.owned_caps.begin(), saved.owned_caps.end());
            const std::set<std::string> owned_cloth(saved.owned_cloth.begin(), saved.owned_cloth.end());
            const std::set<std::string> owned_gifts(saved.owned_gifts.begin(), saved.owned_gifts.end());
            for (const auto& name : *inst.save.costume_cap_names) {
                if (!inst.save.costume_cloth_names->contains(name)) continue; // only pairable sets
                ++costume_total;
                if (owned_caps.contains(name) && owned_cloth.contains(name)) ++costume_owned;
            }
            souvenir_total = static_cast<int>(inst.save.souvenir_names->size());
            for (const auto& name : *inst.save.souvenir_names)
                if (owned_gifts.contains(name)) ++souvenir_owned;
        }
        const bool music_chip_valid = collections_valid && saved.music_valid;
        if (host->publish_i64) {
            host->publish_i64(host->userdata, "smo.collections_valid", collections_valid);
            host->publish_i64(host->userdata, "smo.souvenirs_owned", souvenir_owned);
            host->publish_i64(host->userdata, "smo.souvenirs_total", souvenir_total);
            host->publish_i64(host->userdata, "smo.costumes_owned", costume_owned);
            host->publish_i64(host->userdata, "smo.costumes_total", costume_total);
            host->publish_i64(host->userdata, "smo.music_valid", music_chip_valid);
            host->publish_i64(host->userdata, "smo.music_owned", music_chip_valid ? static_cast<s64>(saved.music_owned) : 0);
            host->publish_i64(host->userdata, "smo.music_total", music_chip_valid ? static_cast<s64>(saved.music_total) : 0);
        }
        if (host->publish_text) {
            host->publish_text(host->userdata, "smo.souvenirs_text", SmoFont::Styled(std::to_string(souvenir_owned) + "/" + std::to_string(souvenir_total), SmoFont::Role::Number).c_str());
            host->publish_text(host->userdata, "smo.costumes_text", SmoFont::Styled(std::to_string(costume_owned) + "/" + std::to_string(costume_total), SmoFont::Role::Number).c_str());
            if (music_chip_valid)
                host->publish_text(host->userdata, "smo.music_text", SmoFont::Styled(std::to_string(saved.music_owned) + "/" + std::to_string(saved.music_total), SmoFont::Role::Number).c_str());
        }

        // ---- B: Odyssey globe (kingdom-select) headline + list -------------------------------
        // Detection: SmoReader::Snapshot's globe_open / globe_cursor_world_id (a fixed-order
        // state pool + cursor lookup table). Display-only: never travels, never confirms a
        // selection.
        struct GlobeRow {
            std::string world, name;
            int shine_type = 0, moons = 0, moon_total = 0, coins = 0, coin_cap = 0;
            unsigned cp_touched = 0, cp_total = 0;
        };
        std::vector<GlobeRow> globe_rows; // one per visited kingdom (mirrors stats_rows exactly)
        if (stats_valid && catalog) {
            for (const auto& row : stats_rows) {
                GlobeRow g;
                g.world = row.info.world; g.name = row.info.name; g.shine_type = row.info.shine_type;
                g.moons = row.progress.moons; g.moon_total = row.progress.moon_total;
                g.coins = row.progress.coins; g.coin_cap = row.progress.coin_capacity;
                const auto cps = SmoWorld::CheckpointsForWorld(*catalog, g.world);
                g.cp_total = static_cast<unsigned>(cps.size());
                for (const auto& cp : cps)
                    if (std::find(saved.checkpoints.begin(), saved.checkpoints.end(),
                                  SmoSave::Checkpoint{cp.stage, cp.id}) != saved.checkpoints.end())
                        ++g.cp_touched;
                globe_rows.push_back(std::move(g));
            }
        }
        const bool globe_open = snap.globe_open;
        bool headline_visible = false, headline_visited = false, headline_complete = false;
        std::string headline_world_key, headline_name, headline_region;
        GlobeRow headline_row;
        if (globe_open && catalog) {
            if (const auto* hw = snap.globe_cursor_valid ? SmoWorld::WorldForId(*catalog, snap.globe_cursor_world_id) : nullptr) {
                headline_visible = true;
                headline_world_key = hw->key; headline_name = hw->name; headline_region = hw->region;
                const auto found = std::find_if(globe_rows.begin(), globe_rows.end(),
                    [&](const GlobeRow& g) { return g.world == hw->key; });
                if (found != globe_rows.end()) {
                    headline_visited = true;
                    headline_row = *found;
                    const bool coins_done = headline_row.coin_cap == 0 || headline_row.coins >= headline_row.coin_cap;
                    headline_complete = headline_row.moons >= headline_row.moon_total && coins_done &&
                        headline_row.cp_touched >= headline_row.cp_total;
                }
            }
        }
        // Page-level "Collected, all visited kingdoms" pill: independent of which kingdom is
        // the headline, hidden until there's at least one visited kingdom to total (a fresh save
        // would otherwise show a meaningless 0/0).
        const bool globe_total_visible = stats_valid && !stats_rows.empty();
        if (host->publish_i64) {
            host->publish_i64(host->userdata, "smo.globe_total.visible", globe_total_visible);
            host->publish_i64(host->userdata, "smo.globe_total.moons", globe_total.moons);
            host->publish_i64(host->userdata, "smo.globe_total.moon_total", globe_total.moon_total);
        }
        if (host->publish_text && globe_total_visible) {
            host->publish_text(host->userdata, "smo.globe_total.ratio",
                SmoFont::Styled(std::to_string(globe_total.moons) + "/" + std::to_string(globe_total.moon_total),
                                SmoFont::Role::Number).c_str());
        }
        if (host->publish_i64) {
            host->publish_i64(host->userdata, "smo.globe_open", globe_open);
            host->publish_i64(host->userdata, "smo.globe_headline.visible", headline_visible);
            host->publish_i64(host->userdata, "smo.globe_headline.visited", headline_visited);
            host->publish_i64(host->userdata, "smo.globe_headline.complete", headline_complete);
            host->publish_i64(host->userdata, "smo.globe_headline.coins_visible", headline_visited && headline_row.coin_cap > 0);
            host->publish_i64(host->userdata, "smo.globe_headline.moons", headline_row.moons);
            host->publish_i64(host->userdata, "smo.globe_headline.moon_total", headline_row.moon_total);
            // Owner request 2026-10-23: per-kingdom bar colour on the globe page (same
            // shine_type -> tint_colors palette as the list-wash page, smo_ui's BarFill +
            // PACKAGE_FORMAT.md's "tint_bind applies to ... bar `image`"). Falls back to the
            // bar's flat `color` (still gold) whenever a module build predates this bind.
            host->publish_i64(host->userdata, "smo.globe_headline.tint_index", std::clamp(headline_row.shine_type, 0, 9));
        }
        if (host->publish_text) {
            host->publish_text(host->userdata, "smo.globe_headline.name", SmoFont::Styled(headline_name, SmoFont::Role::Title).c_str());
            host->publish_text(host->userdata, "smo.globe_headline.region", headline_region.c_str());
            host->publish_text(host->userdata, "smo.globe_headline.moons_left", SmoFont::Styled(std::to_string(headline_row.moon_total - headline_row.moons), SmoFont::Role::Head).c_str());
            host->publish_text(host->userdata, "smo.globe_headline.coins_left", SmoFont::Styled(std::to_string(std::max(0, headline_row.coin_cap - headline_row.coins)) + "/" + std::to_string(headline_row.coin_cap), SmoFont::Role::Number).c_str());
            host->publish_text(host->userdata, "smo.globe_headline.flags_left", SmoFont::Styled(std::to_string(headline_row.cp_total - headline_row.cp_touched) + "/" + std::to_string(headline_row.cp_total), SmoFont::Role::Number).c_str());
            host->publish_text(host->userdata, "smo.globe_headline.moons_ratio", SmoFont::Styled(std::to_string(headline_row.moons) + "/" + std::to_string(headline_row.moon_total), SmoFont::Role::Number).c_str());
            host->publish_text(host->userdata, "smo.globe_headline.moon_src", ("moon_" + std::to_string(headline_row.shine_type)).c_str());
            const bool verified_coin = headline_row.world == "Cap" || headline_row.world == "Waterfall" || headline_row.world == "Sand" || headline_row.world == "Peach";
            host->publish_text(host->userdata, "smo.globe_headline.coin_src", (verified_coin ? "regional_" + headline_row.world : "regional_generic").c_str());
            if (host->publish_i64) host->publish_i64(host->userdata, "smo.globe_headline.coin_tint", verified_coin ? 0 : 1);
        }
        // List: other visited kingdoms, headline excluded, sorted by most unfinished (moons +
        // coins + checkpoints remaining, descending).
        std::vector<GlobeRow> globe_list;
        for (const auto& g : globe_rows) if (g.world != headline_world_key) globe_list.push_back(g);
        std::sort(globe_list.begin(), globe_list.end(), [](const GlobeRow& a, const GlobeRow& b) {
            const int left_a = (a.moon_total - a.moons) + std::max(0, a.coin_cap - a.coins) + int(a.cp_total - a.cp_touched);
            const int left_b = (b.moon_total - b.moons) + std::max(0, b.coin_cap - b.coins) + int(b.cp_total - b.cp_touched);
            return left_a > left_b;
        });
        for (std::size_t i = 0; i < 16; ++i) {
            const bool present = i < globe_list.size();
            const auto key = "smo.globe_list" + std::to_string(i);
            if (host->publish_i64) host->publish_i64(host->userdata, (key + ".visible").c_str(), globe_open && present);
            if (!present) continue;
            const auto& g = globe_list[i];
            if (host->publish_text) {
                host->publish_text(host->userdata, (key + ".name").c_str(), SmoFont::Styled(g.name, SmoFont::Role::Title).c_str());
                host->publish_text(host->userdata, (key + ".moons_left").c_str(), SmoFont::Styled(std::to_string(g.moon_total - g.moons), SmoFont::Role::Head).c_str());
                host->publish_text(host->userdata, (key + ".coins_left").c_str(), SmoFont::Styled(std::to_string(std::max(0, g.coin_cap - g.coins)), SmoFont::Role::Number).c_str());
                host->publish_text(host->userdata, (key + ".flags_left").c_str(), SmoFont::Styled(std::to_string(g.cp_total - g.cp_touched), SmoFont::Role::Number).c_str());
                host->publish_text(host->userdata, (key + ".moons_ratio").c_str(), SmoFont::Styled(std::to_string(g.moons) + "/" + std::to_string(g.moon_total), SmoFont::Role::Number).c_str());
                host->publish_text(host->userdata, (key + ".moon_src").c_str(), ("moon_" + std::to_string(g.shine_type)).c_str());
                const bool verified_coin = g.world == "Cap" || g.world == "Waterfall" || g.world == "Sand" || g.world == "Peach";
                host->publish_text(host->userdata, (key + ".coin_src").c_str(), (verified_coin ? "regional_" + g.world : "regional_generic").c_str());
            }
            if (host->publish_i64) {
                const bool verified_coin = g.world == "Cap" || g.world == "Waterfall" || g.world == "Sand" || g.world == "Peach";
                host->publish_i64(host->userdata, (key + ".coins_visible").c_str(), g.coin_cap > 0);
                host->publish_i64(host->userdata, (key + ".coin_tint").c_str(), verified_coin ? 0 : 1);
                host->publish_i64(host->userdata, (key + ".moons").c_str(), g.moons);
                host->publish_i64(host->userdata, (key + ".moon_total").c_str(), g.moon_total);
                host->publish_i64(host->userdata, (key + ".tint_index").c_str(), std::clamp(g.shine_type, 0, 9));
            }
        }

        const auto selected_task = inst.selected_task.load();
        if (host->publish_i64) {
            host->publish_i64(host->userdata, "smo.tasks_available", tasks_available);
            host->publish_i64(host->userdata, "smo.tasks_locked", tasks_locked);
            host->publish_i64(host->userdata, "smo.tasks_pending", snap.chain_ready && !tasks_available && !tasks_locked);
            host->publish_i64(host->userdata, "smo.stats_pending", snap.chain_ready && !stats_valid);
            host->publish_i64(host->userdata, "smo.tasks_claimed", tasks_claimed);
            host->publish_i64(host->userdata, "smo.tasks_ready", tasks_available ?
                std::count_if(task_progress.begin(),task_progress.end(),[](const auto& v){return v.ready;}) : 0);
            host->publish_i64(host->userdata, "smo.tasks_total", tasks.size());
        }
        if(host->publish_i64)host->publish_i64(host->userdata,"smo.tasks_progress_known",
            std::count_if(task_progress.begin(),task_progress.end(),[](const auto& v){return v.current.has_value();}));
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const auto& task = tasks[i];
            const auto& progress = task_progress[i];
            const std::string prefix = "smo.task" + std::to_string(i);
            const bool selected = static_cast<s64>(i) == selected_task;
            const bool has_count = !progress.claimed && progress.current && task.target;
            const std::string count = has_count ? std::to_string(std::min(*progress.current, *task.target)) + "/" + std::to_string(*task.target) : "";
            if (host->publish_i64) {
                host->publish_i64(host->userdata, (prefix + ".selected").c_str(), selected && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".has_count").c_str(), has_count && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".count_waiting").c_str(), has_count && !progress.ready && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".no_count").c_str(), !has_count && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".ready").c_str(), progress.ready && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".ready_no_count").c_str(), progress.ready && !has_count && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".claimed").c_str(), progress.claimed && tasks_available);
                host->publish_i64(host->userdata, (prefix + ".missing").c_str(), !progress.claimed && tasks_available);
            }
            if (host->publish_text) {
                const auto name = SmoFont::Styled(task.name, SmoFont::Role::Body);
                // The row's hint label is a single wrapping, ellipsis-capable line
                // (manifest wrap_width/max_lines, PACKAGE_FORMAT.md section 3.14): publish the
                // whole sentence, newline breaks joined back to spaces, and let that label do
                // the fitting instead of hard-cutting at the native hint's own line break (which
                // can end mid-sentence with no ellipsis, since the cut text then fits the row's
                // wrap_width on its own and the runtime's wrap-triggered ellipsis never fires).
                const auto short_hint = SmoSave::RowHint(task);
                const auto index = SmoFont::Styled((i < 9 ? "0" : "") + std::to_string(i + 1), SmoFont::Role::Number);
                const auto number = SmoFont::Styled(count, SmoFont::Role::Number);
                host->publish_text(host->userdata, (prefix + ".name").c_str(), name.c_str());
                host->publish_text(host->userdata, (prefix + ".hint").c_str(), short_hint.c_str());
                host->publish_text(host->userdata, (prefix + ".index").c_str(), index.c_str());
                host->publish_text(host->userdata, (prefix + ".count").c_str(), number.c_str());
                if (selected) {
                    const auto title = SmoFont::Styled(task.name, SmoFont::Role::Title);
                    host->publish_text(host->userdata, "smo.task_detail_name", title.c_str());
                    host->publish_text(host->userdata, "smo.task_detail_hint", task.hint.c_str());
                    host->publish_text(host->userdata, "smo.task_detail_count", number.c_str());
                }
            }
        }

        const auto* world = catalog && snap.world_id >= 0 ? SmoWorld::WorldForId(*catalog,snap.world_id) : nullptr;
        // Capacity is static native metadata, independent of whether an autosave exists yet.
        int regional_capacity = -1;
        if(world && inst.save.tables){
            const auto found=inst.save.tables->coin_capacity.find(world->key);
            if(found!=inst.save.tables->coin_capacity.end())regional_capacity=found->second;
        }
        const auto* variant = world && progress.scenario_valid ? SmoWorld::MapVariant(*catalog,world->key,progress.scenario) : nullptr;
        std::shared_ptr<const SmoMap::Resources> map;
        if (variant) {
            std::scoped_lock lock{inst.map_mutex};
            if (const auto found=inst.maps.find(variant->texture);found!=inst.maps.end()) map=found->second;
        }
        if (!map && variant && ++inst.projection_retry_ticks >= 60) {
            inst.projection_retry_ticks=0;
            const auto path="base:/ObjectData/Texture2dMap.szs#"+variant->projection_member;
            const auto bytes=ReadPackageFile(*host,path.c_str());
            auto decoded=SmoMap::DecodeProjection(std::span<const u8>{reinterpret_cast<const u8*>(bytes.data()),bytes.size()},variant->width,variant->height);
            if(decoded){
                map=std::make_shared<SmoMap::Resources>(std::move(*decoded));
                std::scoped_lock lock{inst.map_mutex};inst.maps[variant->texture]=map;
            }
        }
        const bool loading=snap.scene_mode==SmoReader::SceneMode::Loading;
        const bool map_allowed=world && progress.scenario_valid && !loading &&
            SmoWorld::MapAllowed(*catalog,snap.stage_name,progress.scenario);
        std::optional<SmoMap::Point> map_position;
        std::optional<double> map_heading, camera_heading;
        if(map && map_allowed){
            if(snap.player_valid && SmoWorld::DirectPlayerCoordinates(*catalog,snap.stage_name)){
                map_position=SmoMap::Project(*map,snap.player_position);
                map_heading=SmoMap::Heading(*map,snap.player_position,snap.player_front);
                if(snap.camera_valid)camera_heading=SmoMap::Heading(*map,snap.player_position,snap.camera_front);
            }else if(auto anchor=SmoWorld::StageAnchor(*catalog,snap.stage_name,progress.scenario))
                map_position=SmoMap::Project(*map,*anchor);
        }
        const bool map_valid=map && map_allowed;
        if(!map_valid)inst.selected_flag.store(-1);
        const auto selected_flag=inst.selected_flag.load();
        std::string selected_name;
        std::optional<SmoMap::Point> selected_position;
        unsigned touched_count=0;
        bool flags_live_known=true;
        const auto flags=world && progress.scenario_valid ? SmoWorld::Checkpoints(*catalog,world->key,progress.scenario) : std::vector<SmoWorld::Checkpoint>{};
        for(size_t i=0;i<32;++i){
            const auto* flag=i<flags.size()?&flags[i]:nullptr;
            const auto live_touched=flag ? SmoProgress::CheckpointTouched(*host,snap.game_data,snap.world_id,flag->stage,flag->id) : std::nullopt;
            if(flag && !live_touched.has_value())flags_live_known=false;
            const bool unlocked=flag && live_touched.value_or(saved.valid && std::find(saved.checkpoints.begin(),saved.checkpoints.end(),SmoSave::Checkpoint{flag->stage,flag->id})!=saved.checkpoints.end());
            if(unlocked)++touched_count;
            const auto point=flag && map?SmoMap::Project(*map,flag->position):std::nullopt;
            const auto slot=std::to_string(i);
            if(host->publish_text)host->publish_text(host->userdata,("smo.flag_src"+slot).c_str(),unlocked?
                "base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#CmIconFlag00^w":
                "base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#CmIconFlag05^t");
            if(host->publish_i64)host->publish_i64(host->userdata,("smo.flag_kind"+slot).c_str(),map_valid && point && (unlocked || live_touched.has_value() || saved.valid)?(unlocked?1:2):0);
            if(host->publish_f64){
                host->publish_f64(host->userdata,("smo.flag_x"+slot).c_str(),point?point->x:0);
                host->publish_f64(host->userdata,("smo.flag_y"+slot).c_str(),point?point->y:0);
            }
            if(map_valid && unlocked && static_cast<s64>(i)==selected_flag){selected_name=flag->name;selected_position=point;}
        }
        unsigned warp_reason=10;
        std::optional<SmoWarp::Plan> warp;
        const bool flag_chosen=selected_flag>=0 && selected_flag<static_cast<s64>(flags.size()) && !selected_name.empty();
        if((snap.scene_mode==SmoReader::SceneMode::Playing || snap.scene_mode==SmoReader::SceneMode::StageMap) && flag_chosen){
            const auto& flag=flags[selected_flag];
            warp=SmoWarp::Prepare(*host,snap.stage_scene,snap.game_data,snap.world_id,flag.stage,flag.id,true,&warp_reason,
                snap.scene_mode==SmoReader::SceneMode::Playing);
        }
        const bool aux=host->get_i64 && host->get_i64(host->userdata,"__aux_present",0)==1;
        const bool warp_available=aux && !inst.warp_waiting && inst.write_api.write_batch && warp.has_value();
        if(inst.warp_requested.exchange(false) && warp_available){
            inst.warp_waiting=true;inst.warp_wait_ticks=0;
            inst.warp_slot=selected_flag;inst.warp_world=snap.world_id;
        }
        if(inst.warp_waiting){
            if(!aux || !snap.chain_ready || selected_flag!=inst.warp_slot || snap.world_id!=inst.warp_world ||
               snap.scene_mode==SmoReader::SceneMode::Paused || ++inst.warp_wait_ticks>30){
                inst.warp_waiting=false;
            }else if(warp && inst.write_api.write_batch){
                std::array<EdenDsmodWriteOp,3> ops{};
                for(size_t i=0;i<ops.size();++i){auto& w=warp->writes[i];ops[i]={w.address,w.size,0,&w.before,&w.after};}
                const bool ok=inst.write_api.write_batch(inst.write_api.userdata,ops.data(),ops.size());
                if(ok){
                    if(host->log)host->log(host->userdata,EDEN_DSMOD_LOG_INFO,"Native checkpoint warp requested");
                    inst.warp_waiting=false;inst.selected_flag.store(-1);
                }
            }
        }
        if(host->publish_i64){
            host->publish_i64(host->userdata,"smo.diag.warp_reason",warp_reason);
            host->publish_i64(host->userdata,"smo.warp_available",warp_available && !inst.warp_waiting);
            host->publish_i64(host->userdata,"smo.warp_busy",inst.warp_waiting);
            host->publish_i64(host->userdata,"smo.warp_locked",!selected_name.empty() && (!warp_available || inst.warp_waiting));
        }
        if(host->publish_text)host->publish_text(host->userdata,"smo.warp_hint",inst.warp_waiting?"Warping...":"Warp unavailable");
        const auto icons=world && progress.scenario_valid ? SmoWorld::Icons(*catalog,world->key,progress.scenario) : std::vector<SmoWorld::Icon>{};
        std::vector<SmoMap::Point> poi_points; // kept for the moon-marker overlap rule below
        for(size_t i=0;i<32;++i){
            const auto* icon=i<icons.size()?&icons[i]:nullptr;
            const auto point=icon && map?SmoMap::Project(*map,icon->position):std::nullopt;
            if(map_valid && point)poi_points.push_back(*point);
            const auto key="smo.poi"+std::to_string(i);
            if(host->publish_i64)host->publish_i64(host->userdata,(key+".kind").c_str(),map_valid && point?1:0);
            if(host->publish_f64){host->publish_f64(host->userdata,(key+".x").c_str(),point?point->x:0);host->publish_f64(host->userdata,(key+".y").c_str(),point?point->y:0);}
            if(host->publish_text && icon)host->publish_text(host->userdata,(key+".src").c_str(),icon->kind=="MapIconShop"?"base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#CmIconShop^t":icon->kind=="MapIconPoetter"?"base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#CmIconPoet^t":"base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#CmIconMiniGame^t");
        }
        // Power Moon markers: owner-requested 2026-10-23. Collected moons always show (kingdom-
        // coloured native pictogram, PictureFont80 U+0070+shine_type). Moon Rock / post-game
        // moons stay gated behind their own world thresholds. A moon whose projection lands
        // outside its own kingdom's map image (two Lava Kingdom ShineList entries, unique_id
        // 257/259, verifiably do) is never drawn: "exclude moons not on this map" is enforced by
        // bounds, not a denylist.
        //
        // "Show Uncollected Moons" IS live, via a generated `derived` expr (manifest.json, 64
        // "smo.mapmoonN.kind" entries), NOT a module-side flag read: this Sample() callback runs
        // before PublishFlags every frame (same bug yoru_module.cpp's log_content_rev comment
        // documents for its own Option page), so a module get_i64("@flag:...") read here would
        // always see the fallback, never a live toggle. Derived values are evaluated after
        // PublishFlags, so they see it fresh. (An earlier session wrongly concluded the derived
        // route also failed live -- that test rebooted without resetting a flag left ON from a
        // previous toggle, so "all moons kept showing" was actually correct given the real flag
        // state; a single-slot pilot with the flag state explicitly controlled, console-verified
        // both ways on 2026-10-23, confirmed the expr gates correctly.) This Sample() callback
        // only publishes the raw material each `derived` entry reads: "smo.mapmoonN.kind_raw" (0
        // none, 1 collected -- always shown regardless of the flag, 2 uncollected-and-gate-
        // eligible -- shown only when the flag is on), x/y and the icon src text.
        int moon_shine_type=0;
        if(world && inst.save.tables){
            const auto it=std::find_if(inst.save.tables->kingdoms.begin(),inst.save.tables->kingdoms.end(),[&](const auto& k){return k.world==world->key;});
            if(it!=inst.save.tables->kingdoms.end())moon_shine_type=it->shine_type;
        }
        char moon_hex[5]={};
        std::snprintf(moon_hex,sizeof moon_hex,"%04X",0x70+std::clamp(moon_shine_type,0,9));
        const std::string moon_collected_src="module:smo:picture/"+std::string(moon_hex);
        // ---- C: animated kingdom-colour list-wash --------------------------------------------
        // Auto-shown while a Collection List CONTENT page (Power Moon / Capture / Souvenir /
        // Music) is open on top -- explicitly NOT the Map page of CollectionList (the Minus map),
        // which keeps the already-accepted Map page behaviour.
        // Kingdom = the player's current WorldId (the Collection List is reached from gameplay in
        // one kingdom, not a cross-kingdom destination picker like the globe). Reuses the exact
        // moon_collected_src / moon_shine_type already computed above for the map markers, so the
        // wash's glyph and tint always agree with this kingdom's own moon pictogram.
        const bool list_wash_open = world && (
            snap.collection_page==SmoReader::CollectionPage::PowerMoon ||
            snap.collection_page==SmoReader::CollectionPage::Capture ||
            snap.collection_page==SmoReader::CollectionPage::Souvenir ||
            snap.collection_page==SmoReader::CollectionPage::Music);
        if (host->publish_i64) {
            host->publish_i64(host->userdata, "smo.list_wash_open", list_wash_open);
            host->publish_i64(host->userdata, "smo.list_wash.tint_index", std::clamp(moon_shine_type, 0, 9));
        }
        if (host->publish_text) {
            host->publish_text(host->userdata, "smo.list_wash.world_name", world ? SmoFont::Styled(world->name, SmoFont::Role::Title).c_str() : "");
            host->publish_text(host->userdata, "smo.list_wash.region", world ? world->region.c_str() : "");
            host->publish_text(host->userdata, "smo.list_wash.moon_src", moon_collected_src.c_str());
        }
        // Owner-reviewed capture of Sand with moons on: U+007A is the
        // rainbow/unknown-kingdom moon pictogram (sprite_map's reserved "moon_10", a collected-
        // moon glyph, not an empty slot) -- using it for an UNcollected marker reads as a
        // wrongly-coloured collected moon. U+007B is the native dotted empty-slot glyph, already
        // the accepted convention for "nothing here yet" on the Tasks page (manifest.json tasks
        // widgets, "need_bind": "smo.task{i}.missing" / "smo.tasks_available").
        constexpr const char* kMoonEmptySrc="module:smo:picture/007B";
        // Marker-size radii (manifest.json dynamic_markers: mapmoons size 36, poi size 48) sum to
        // a 42px half-span; 38 gives moons a little breathing room before they are considered
        // "overlapping" a POI icon, and 30 is a visible but modest nudge (just over one mapmoon
        // marker radius) rather than relocating the moon to a materially different spot.
        constexpr double kPoiSeparation=38, kPoiNudge=30, kMoonMergeDistance=24;
        const auto all_moons=world && map_valid ? SmoWorld::Moons(*catalog,world->key) : std::vector<SmoWorld::Moon>{};
        std::vector<SmoMap::Point> moon_points; std::vector<bool> moon_collected_flags;
        for(const auto& moon:all_moons){
            if(moon_points.size()>=64)break;
            // Missing save access is UNKNOWN, never "every moon uncollected".
            // This is especially important on Android while the host UID bug is open.
            if(!saved.valid)continue;
            const bool collected=std::find(saved.moon_ids.begin(),saved.moon_ids.end(),moon.unique_id)!=saved.moon_ids.end();
            if(!collected){
                if(moon.is_moon_rock){if(progress.scenario<world->moon_rock)continue;}
                else if(moon.main_scenario_no!=UINT32_MAX && progress.scenario<=moon.main_scenario_no)continue;
            }
            auto point=SmoMap::Project(*map,moon.position);
            if(!point||point->x<0||point->x>map->image.width||point->y>0||point->y<-double(map->image.height))continue;
            // Reduce overlap with POI (shop/minigame/poetter) icons: nudge away, then re-check
            // the map bounds (a nudge must never push a marker off the kingdom's own map image).
            const auto nudged=SmoMap::NudgeAwayFromConflicts(*point,poi_points,kPoiSeparation,kPoiNudge);
            if(nudged.x>=0 && nudged.x<=map->image.width && nudged.y<=0 && nudged.y>=-double(map->image.height))
                point=nudged;
            bool merged=false;
            for(size_t k=0;k<moon_points.size() && !merged;++k)
                merged=moon_collected_flags[k]==collected && std::hypot(moon_points[k].x-point->x,moon_points[k].y-point->y)<kMoonMergeDistance;
            if(merged)continue;
            // Keep marker membership independent of the last-drawn viewport. The map
            // renderer clips to its real rectangle, and visible banner widgets cover
            // their own pixels; absent overlays must never leave invisible dead zones.
            moon_points.push_back(*point);moon_collected_flags.push_back(collected);
        }
        unsigned moons_collected_shown=0;
        for(size_t i=0;i<64;++i){
            const bool has=i<moon_points.size();
            const auto key="smo.mapmoon"+std::to_string(i);
            const int kind_raw=!map_valid||!has?0:(moon_collected_flags[i]?1:2);
            if(has && moon_collected_flags[i])++moons_collected_shown;
            if(host->publish_i64)host->publish_i64(host->userdata,(key+".kind_raw").c_str(),kind_raw);
            // "smo.mapmoonN.kind" itself -- what dynamic_markers actually binds -- is owned
            // entirely by the generated `derived` expr (manifest.json); this module never
            // publishes that exact name, so there is no two-writer race.
            if(host->publish_f64){host->publish_f64(host->userdata,(key+".x").c_str(),has?moon_points[i].x:0);host->publish_f64(host->userdata,(key+".y").c_str(),has?moon_points[i].y:0);}
            if(host->publish_text)host->publish_text(host->userdata,(key+".src").c_str(),has?(moon_collected_flags[i]?moon_collected_src.c_str():kMoonEmptySrc):"");
        }
        if(host->publish_i64)host->publish_i64(host->userdata,"smo.map_moons_shown",static_cast<s64>(moon_points.size()));
        if(host->publish_i64)host->publish_i64(host->userdata,"smo.map_moons_collected_shown",static_cast<s64>(moons_collected_shown));
        auto goals=world && progress.main_shine_valid ? SmoWorld::Objectives(*catalog,world->key,progress.main_shine) : std::vector<SmoWorld::Objective>{};
        std::erase_if(goals,[&](const auto& g){return std::find(saved.moon_ids.begin(),saved.moon_ids.end(),g.unique_id)!=saved.moon_ids.end();});
        std::string objective;
        const size_t objective_index=goals.empty()?0:(inst.objective_ticks++/300)%goals.size();
        if(!goals.empty())objective=goals[objective_index].name;
        // Last-drawn map transform is provided by rt22, including touch pan/zoom.
        // Draw the native brochure connector toward the actual objective, clipped to map bounds.
        double line_dx=0,line_dy=0,line_length=0;
        if(map_valid && !goals.empty() && host->get_f64){
            const auto goal=SmoMap::Project(*map,goals[objective_index].position);
            const auto query=[&](const char* field,double fallback){return host->get_f64(host->userdata,("@map_view_"+std::string(field)+":"+variant->texture).c_str(),fallback);};
            const double scale=query("ppw",0),cx=query("cx",0),cy=query("cy",0);
            if(goal && scale>0 && std::isfinite(scale) && query("rot",0)==0){
                const double gx=query("x",0)+query("w",930)/2+(goal->x-cx)*scale;
                const double gy=query("y",112)+query("h",map_footer.map_height)/2-(goal->y-cy)*scale;
                line_dx=gx-733;line_dy=gy-220;line_length=std::hypot(line_dx,line_dy);
            }
        }
        const auto pointer_angle=line_length>1?unsigned(std::lround((std::atan2(line_dy,line_dx)*180/3.141592653589793+360)/5))*5%360:0;
        if(host->publish_text)host->publish_text(host->userdata,"smo.objective_pointer",("module:smo:objective_pointer/"+std::to_string(pointer_angle)).c_str());
        for(size_t i=0;i<192;++i){
            const double distance=double(i)*8,t=line_length>1?distance/line_length:0;
            const double x=733+line_dx*t,y=220+line_dy*t;
            const bool visible=map_valid && line_length>1 && distance<=line_length && x>=3 && x<=927 && y>=115 && y<=map_footer.footer_y-3;
            const auto key="smo.objective_dot"+std::to_string(i);
            if(host->publish_i64)host->publish_i64(host->userdata,(key+".visible").c_str(),visible);
            if(host->publish_f64){host->publish_f64(host->userdata,(key+".x").c_str(),visible?x:0);host->publish_f64(host->userdata,(key+".y").c_str(),visible?y:0);}
        }
        for(size_t i=0;i<4;++i){
            const auto point=i<goals.size() && map?SmoMap::Project(*map,goals[i].position):std::nullopt;
            const auto key="smo.goal"+std::to_string(i);
            if(host->publish_i64)host->publish_i64(host->userdata,(key+".kind").c_str(),map_valid && point?1:0);
            if(host->publish_f64){host->publish_f64(host->userdata,(key+".x").c_str(),point?point->x:0);host->publish_f64(host->userdata,(key+".y").c_str(),point?point->y:0);}
        }
        if(host->publish_i64){
            host->publish_i64(host->userdata,"smo.state_title",snap.scene_mode==SmoReader::SceneMode::Title);
            host->publish_i64(host->userdata,"smo.state_loading",loading || (!snap.chain_ready && snap.scene_mode!=SmoReader::SceneMode::Title));
            host->publish_i64(host->userdata,"smo.state_pause",snap.scene_mode==SmoReader::SceneMode::Paused);
            host->publish_i64(host->userdata,"smo.map_unavailable",snap.chain_ready && (!variant || !map_allowed));
            host->publish_i64(host->userdata,"smo.objective_valid",map_valid && !objective.empty());
            host->publish_i64(host->userdata,"smo.health_valid",snap.health_valid && !loading);
            host->publish_i64(host->userdata,"smo.health",snap.health);
            host->publish_i64(host->userdata,"smo.health_max",snap.health_max);
            host->publish_i64(host->userdata,"smo.global_moons_valid",progress.global_valid && !loading);
            host->publish_i64(host->userdata,"smo.global_moons",progress.banked_total);
            host->publish_i64(host->userdata,"smo.moon_row_valid",progress.moon_row_valid && !loading);
            host->publish_i64(host->userdata,"smo.moon_row",progress.moon_row);
            host->publish_i64(host->userdata,"smo.moon_missing",progress.moon_requirement_valid?progress.moon_missing:0);
            for(size_t i=0;i<40;++i){
                const auto key="smo.moon"+std::to_string(i);
                const bool visible=progress.moon_row_valid && !loading && progress.moon_row>static_cast<int>(i);
                const bool empty=progress.moon_row_valid && progress.moon_requirement_valid && !loading &&
                    i>=static_cast<size_t>(progress.moon_row) && i<static_cast<size_t>(moon_slots);
                host->publish_i64(host->userdata,(key+".large").c_str(),visible && map_footer.large);
                host->publish_i64(host->userdata,(key+".small").c_str(),visible && !map_footer.large);
                host->publish_i64(host->userdata,(key+".empty_large").c_str(),empty && map_footer.large);
                host->publish_i64(host->userdata,(key+".empty_small").c_str(),empty && !map_footer.large);
            }
            host->publish_i64(host->userdata,"smo.moon_overflow",progress.moon_row_valid && progress.moon_row>40);
            host->publish_i64(host->userdata,"smo.map_ready",map?1:0);
            host->publish_i64(host->userdata,"smo.map_present",map && snap.chain_ready && !loading);
            host->publish_i64(host->userdata,"smo.map_rows",map_footer.rows);
            for(int variant_index=0;variant_index<=4;++variant_index)
                host->publish_i64(host->userdata,("smo.map_layout"+std::to_string(variant_index)).c_str(),map && snap.chain_ready && !loading && map_footer.variant==variant_index);
            host->publish_i64(host->userdata,"smo.map_valid",map_valid);
            host->publish_i64(host->userdata,"smo.map_player_valid",map_valid && map_position.has_value());
            host->publish_i64(host->userdata,"smo.map_facing_valid",map_valid && map_heading.has_value());
            host->publish_i64(host->userdata,"smo.camera_cone_kind",map_valid && camera_heading?1+static_cast<int>(std::lround(*camera_heading/5))%72:0);
            host->publish_i64(host->userdata,"smo.flag_selected",!selected_name.empty());
            host->publish_i64(host->userdata,"smo.map_legend_visible",map_valid && selected_name.empty());
            host->publish_i64(host->userdata,"smo.checkpoints_touched",touched_count);
            host->publish_i64(host->userdata,"smo.sand_flags_total",flags.size());
            host->publish_i64(host->userdata,"smo.flags_known",map_valid && !flags.empty());
            host->publish_i64(host->userdata,"smo.flags_progress_valid",map_valid && !flags.empty() && (flags_live_known || saved.valid));
            host->publish_i64(host->userdata,"smo.flags_pending",map_valid && !flags.empty() && !flags_live_known && !saved.valid);
            host->publish_i64(host->userdata,"smo.diag.game_data",snap.game_data);
            host->publish_i64(host->userdata,"smo.diag.stage_scene",snap.stage_scene);
            host->publish_i64(host->userdata,"smo.diag.scene_mode",static_cast<int>(snap.scene_mode));
            host->publish_i64(host->userdata,"smo.diag.hit_point_data",snap.hit_point_data);
            host->publish_i64(host->userdata,"smo.diag.player",snap.player);
        }
        if(host->publish_f64){
            host->publish_f64(host->userdata,"smo.map_bottom",map_footer.footer_y);
            host->publish_f64(host->userdata,"smo.selected_flag_x",selected_position?selected_position->x:0);
            host->publish_f64(host->userdata,"smo.selected_flag_y",selected_position?selected_position->y:0);
            host->publish_f64(host->userdata,"smo.map_x",map_position?map_position->x:0);
            host->publish_f64(host->userdata,"smo.map_y",map_position?map_position->y:0);
            host->publish_f64(host->userdata,"smo.map_heading",map_heading.value_or(0));
            host->publish_f64(host->userdata,"smo.map_camera_heading",camera_heading.value_or(0));
            // One stable Mario arrow rotates in the map renderer. Missing position/
            // heading omits its coordinates, so the renderer suppresses the marker.
            if(map_valid && map_position && map_heading){
                host->publish_f64(host->userdata,"smo.cone_x",map_position->x);
                host->publish_f64(host->userdata,"smo.cone_y",map_position->y);
            }
            // Fill the width up to the right information pane, preserving aspect.
            // Center the cropped default view on the live player; manual pan/zoom
            // remains independent until the owner taps Reset.
            const double width=map?double(map->image.width):2048;
            const double height=map?double(map->image.height):2048;
            const double view_height=std::min(height,width*map_footer.map_height/930.0);
            const double center_y=map_position?std::clamp(map_position->y,-height+view_height/2,-view_height/2):-height/2;
            host->publish_f64(host->userdata,"smo.view_x0",0);
            host->publish_f64(host->userdata,"smo.view_y0",center_y-view_height/2);
            host->publish_f64(host->userdata,"smo.view_x1",width);
            host->publish_f64(host->userdata,"smo.view_y1",center_y+view_height/2);
        }
        if(host->publish_text){
            // Dynamic map markers accept icon_src_bind (not a literal icon_src).
            host->publish_text(host->userdata,"smo.goal_src","base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#MyIconHintMain^t");
            host->publish_text(host->userdata,"smo.progress_pending_title",SmoFont::Styled("Progress unavailable",SmoFont::Role::Title).c_str());
            host->publish_text(host->userdata,"smo.flags_pending_text",SmoFont::Styled("--/"+std::to_string(flags.size()),SmoFont::Role::Number).c_str());
            host->publish_text(host->userdata,"smo.map_area",variant?variant->texture.c_str():"SandWorldHomeStage");
            host->publish_text(host->userdata,"smo.world_name",world?SmoFont::Styled(world->name,SmoFont::Role::Title).c_str():"");
            host->publish_text(host->userdata,"smo.region_name",world?world->region.c_str():"");
            host->publish_text(host->userdata,"smo.objective",SmoFont::Styled(objective,SmoFont::Role::Title).c_str());
            const auto health="module:smo:life/"+std::to_string(snap.health)+"/"+std::to_string(snap.health_max);
            host->publish_text(host->userdata,"smo.health_src",health.c_str());
            host->publish_text(host->userdata,"smo.health_text",SmoFont::Styled(std::to_string(snap.health),SmoFont::Role::Head).c_str());
            host->publish_text(host->userdata,"smo.global_moons_text",(SmoFont::Styled(std::to_string(progress.banked_total),SmoFont::Role::Head)+SmoFont::Styled("/999",SmoFont::Role::Number)).c_str());
            int shine_type=0;
            if(world && inst.save.tables){
                const auto it=std::find_if(inst.save.tables->kingdoms.begin(),inst.save.tables->kingdoms.end(),[&](const auto& k){return k.world==world->key;});
                if(it!=inst.save.tables->kingdoms.end())shine_type=it->shine_type;
            }
            const auto glyph="moon_"+std::to_string(shine_type);
            host->publish_text(host->userdata,"smo.moon_src",glyph.c_str());
            const bool verified=world && (world->key=="Cap" || world->key=="Waterfall" || world->key=="Sand" || world->key=="Peach");
            const auto regional=verified?"regional_"+world->key:"regional_generic";
            host->publish_text(host->userdata,"smo.regional_src",regional.c_str());
            host->publish_text(host->userdata,"smo.stage_name",snap.stage_name.c_str());
            host->publish_text(host->userdata,"smo.player_pin_src","module:smo:player_pin");
            host->publish_text(host->userdata,"smo.flag_src","base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx#CmIconFlag00^w");
            host->publish_text(host->userdata,"smo.flag_name",SmoFont::Styled(selected_name,SmoFont::Role::Body).c_str());
        }

        // "Hide Top-Screen HUD" setting (owner-confirmed 2026-10-23, keep alongside "Show
        // Uncollected Moons"). Two possible live read paths, tried in order:
        //  1. A generated `derived` point, "smo.hide_top_hud_live" (manifest.json, expr
        //     "@flag:smo.hide_top_hud != 0"), read here by its PLAIN name (no "@flag:" prefix).
        //     Derived values are evaluated after PublishFlags each tick, so a plain get_i64 read
        //     from Sample() sees last tick's result -- the same one-tick-behind pattern
        //     @map_view_*/@view_zoom use (PACKAGE_FORMAT.md runtime 20/22, MODULE_GUIDE.md
        //     §1.3).
        //  2. A direct get_i64("@flag:smo.hide_top_hud") read, a fallback for a runtime build
        //     that predates the derived point shipping live. Kept as the fallback anyway -- its
        //     default (ON) exactly matches the always-on pre-existing behaviour, so it's harmless
        //     on an old runtime build and becomes correct for free once that build is updated.
        // Diagnostic published either way: smo.diag.hide_top_hud_live_read (-2 no get_i64, -1
        // path 1 returned nothing/not 0 or 1, else the raw path-1 value actually used).
        {
            const s64 derived_read = host->get_i64 ? host->get_i64(host->userdata,"smo.hide_top_hud_live",-1) : -2;
            if(derived_read==0 || derived_read==1) inst.hide_hud.store(derived_read!=0);
            else inst.hide_hud.store(!host->get_i64 || host->get_i64(host->userdata,"@flag:smo.hide_top_hud",1)!=0);
            if(host->publish_i64)host->publish_i64(host->userdata,"smo.diag.hide_top_hud_live_read",derived_read);
        }
        inst.hud.Update(*host,snap,inst.hide_hud.load() && snap.health_valid && snap.coins_valid && progress.moon_row_valid && progress.moon_requirement_valid && progress.regional_valid);
        if(host->publish_i64)host->publish_i64(host->userdata,"smo.hud_hidden",inst.hud.HiddenCount());
        if(host->publish_i64){
            host->publish_i64(host->userdata,"smo.diag.hide_requested",inst.hide_hud.load());
            host->publish_i64(host->userdata,"smo.diag.hud_stage",inst.hud.GetterStage());
            host->publish_i64(host->userdata,"smo.diag.hud_candidates",inst.hud.CandidateCount());
            host->publish_i64(host->userdata,"smo.diag.hud_panes",inst.hud.PaneCount());
            host->publish_i64(host->userdata,"smo.diag.hud_failures",inst.hud.FailedWrites());
            host->publish_i64(host->userdata,"smo.diag.hud_probe",inst.hud.ProbePane());
        }
        if(host->publish_i64){host->publish_i64(host->userdata,"smo.diag.aux_present",host->get_i64?host->get_i64(host->userdata,"__aux_present",-1):-2);host->publish_i64(host->userdata,"smo.diag.write_api",inst.write_api.write_batch!=nullptr);}
        // Published every sample regardless of validity (MODULE_GUIDE.md section 1.2 "the
        // snapshot is cleared every tick: publish every value on every sample, or the value
        // disappears"). `smo.coins_valid` drives the manifest's need_bind so the page shows a
        // native "unavailable" treatment instead of a stale or zeroed number while the chain is
        // not yet resolved.
        if (host->publish_i64) {
            host->publish_i64(host->userdata, "smo.coins_valid", snap.coins_valid ? 1 : 0);
            host->publish_i64(host->userdata, "smo.coins", snap.coins_valid ? snap.coins : 0);
            host->publish_i64(host->userdata, "smo.chain_ready", snap.chain_ready ? 1 : 0);
            host->publish_i64(host->userdata, "smo.world_id", snap.world_id);
            host->publish_i64(host->userdata, "smo.player_valid", snap.player_valid ? 1 : 0);
            // Diagnostics only (not referenced by the slice's manifest; harmless if a future
            // page binds them, dropped by the publish filter otherwise once one exists).
            host->publish_i64(host->userdata, "smo.diag.stage_scene_known",
                              snap.stage_scene != 0 ? 1 : 0);
            host->publish_i64(host->userdata, "smo.diag.scan_pages",
                              static_cast<std::int64_t>(snap.scan_pages_this_sample));

            // Native current-world wallet updates on pickups and purchases.
            host->publish_i64(host->userdata, "smo.regional_coins_valid", progress.regional_valid && regional_capacity > 0);
            host->publish_i64(host->userdata, "smo.regional_present", regional_capacity > 0);
            host->publish_i64(host->userdata, "smo.regional_unknown", regional_capacity > 0 && !progress.regional_valid);
            host->publish_i64(host->userdata, "smo.regional_coins", progress.regional_valid ? progress.regional_wallet : 0);
            host->publish_i64(host->userdata, "smo.diag.active_save_file", inst.save.active_file);
            host->publish_i64(host->userdata, "smo.regional_coins_max",
                              std::max(0,regional_capacity));
            host->publish_i64(host->userdata, "smo.kingdom_moons_valid", kingdom_known ? 1 : 0);
            host->publish_i64(host->userdata, "smo.kingdom_moons", kingdom_known ? kingdom.moons : 0);
            host->publish_i64(host->userdata, "smo.kingdom_moons_total",
                              kingdom_known ? kingdom.moon_total : 0);
        }
        if (host->publish_f64) {
            constexpr const char* axes[] = {"smo.player_x", "smo.player_y", "smo.player_z"};
            constexpr const char* front[] = {"smo.player_front_x", "smo.player_front_y",
                                             "smo.player_front_z"};
            for (size_t i = 0; i < 3; ++i) {
                host->publish_f64(host->userdata, axes[i],
                                  snap.player_valid ? snap.player_position[i] : 0);
                host->publish_f64(host->userdata, front[i],
                                  snap.player_valid ? snap.player_front[i] : 0);
            }
            host->publish_f64(host->userdata, "smo.player_heading_deg",
                              snap.player_valid ? snap.player_heading_deg : 0);
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log) {
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "SMO DSMod sample callback failed");
        }
    }
}

void Tick(void* p, const EdenDsmodHostApi* host) {
    if(p && host && (!host->get_i64 || !host->get_i64(host->userdata,"__aux_present",0)))
        {auto& inst=*static_cast<Instance*>(p);inst.hud.Restore(*host);inst.warp_requested.store(false);inst.warp_waiting=false;}
}
void ConfigureWrite(void* p,const EdenDsmodHostWriteApi* api){
    if(p && api && api->version==EDEN_DSMOD_WRITE_EXT_VERSION && api->struct_size==sizeof(*api) && api->abi_hash==EDEN_DSMOD_WRITE_EXT_HASH){
        auto& inst=*static_cast<Instance*>(p);inst.hud.SetWriteApi(*api);inst.write_api=*api;
    }
}

// ---- base extensions: no mailbox/actions needed yet, just load_image for font pages -----------

void Configure(void* p, const EdenDsmodHostExtensions* ext) {
    if (!p || !ext || ext->version != EDEN_DSMOD_EXT_VERSION || ext->abi_hash != EDEN_DSMOD_EXT_HASH ||
        ext->struct_size < offsetof(EdenDsmodHostExtensions, decode_astc) + sizeof(ext->decode_astc)) return;
    auto& inst = *static_cast<Instance*>(p);
    std::scoped_lock lock{inst.capture_mutex};
    inst.decode_astc = ext->decode_astc;
    inst.astc_userdata = ext->userdata;
}

EdenDsmodBool OnAction(void* inst, const char* action, std::int64_t argument) {
    // Keep the detail selection in the module: input state is published after sample().
    // No write, warp or guest call is issued.
    if (!inst || !action) return EDEN_DSMOD_FALSE;
    auto& state = *static_cast<Instance*>(inst);
    if(std::string_view{action}=="flag_warp"){state.warp_requested.store(true);return EDEN_DSMOD_TRUE;}
    if(std::string_view{action}=="hud_hide"){
        state.hide_hud.store(argument!=0);return EDEN_DSMOD_TRUE;
    }
    if (std::string_view{action} == "task_select") {
        if (argument < 0 || argument >= 61) return EDEN_DSMOD_FALSE;
        state.selected_task.store(argument);
        return EDEN_DSMOD_TRUE;
    }
    if (std::string_view{action} == "flag_clear") {
        state.selected_flag.store(-1);
        return EDEN_DSMOD_TRUE;
    }
    if (std::string_view{action} != "flag_select" || argument < 0 || argument >= 32)
        return EDEN_DSMOD_FALSE;
    state.selected_flag.store(argument);
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool LoadImageFn(void* p, const EdenDsmodHostApi* host, const char* key,
                          void* receiver, EdenDsmodImageSink sink) {
    try {
        if (!p || !key || !sink) {
            return EDEN_DSMOD_FALSE;
        }
        const std::string_view k{key};
        if(k.starts_with("module:smo:picture/") || k.starts_with("module:smo:icon/")) {
            const bool picture=k.starts_with("module:smo:picture/");
            const auto digits=k.substr(picture?19:16);
            if(digits.size()!=4 || digits.find_first_not_of("0123456789ABCDEFabcdef")!=digits.npos)
                return EDEN_DSMOD_FALSE;
            const auto cp=static_cast<u32>(std::strtoul(std::string(digits).c_str(),nullptr,16));
            auto& inst=*static_cast<Instance*>(p);
            std::shared_ptr<const SmoFont::FontAtlas> atlas;
            {std::scoped_lock lock{inst.font_mutex};atlas=inst.atlas;}
            if(!atlas)return EDEN_DSMOD_FALSE;
            auto image=SmoFont::GlyphImage(*atlas,picture?SmoFont::Role::Picture:SmoFont::Role::Icon,cp);
            if(image.Empty())return EDEN_DSMOD_FALSE;
            if(!picture)for(std::size_t i=0;i<image.rgba.size();i+=4){image.rgba[i]=191;image.rgba[i+1]=100;image.rgba[i+2]=255;}
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());return EDEN_DSMOD_TRUE;
        }
        if(k=="module:smo:objective_dot"){
            if(!host || !host->read_romfs)return EDEN_DSMOD_FALSE;
            const auto bytes=ReadPackageFile(*host,"base:/LayoutData/MapLine.szs#layout.lyarc#timg/__Combined.bntx");
            const auto image=SmoUi::ObjectiveDot({reinterpret_cast<const u8*>(bytes.data()),bytes.size()});
            if(!image)return EDEN_DSMOD_FALSE;
            sink(receiver,image->width,image->height,image->rgba.data(),image->rgba.size());return EDEN_DSMOD_TRUE;
        }
        if(k.starts_with("module:smo:objective_pointer/")){
            if(!host || !host->read_romfs)return EDEN_DSMOD_FALSE;
            const auto digits=k.substr(std::string_view{"module:smo:objective_pointer/"}.size());
            if(digits.empty() || digits.size()>3 || digits.find_first_not_of("0123456789")!=digits.npos)return EDEN_DSMOD_FALSE;
            const unsigned degrees=unsigned(std::strtoul(std::string(digits).c_str(),nullptr,10));
            if(degrees>=360 || degrees%5)return EDEN_DSMOD_FALSE;
            const auto bytes=ReadPackageFile(*host,"base:/LayoutData/MapLine.szs#layout.lyarc#timg/__Combined.bntx");
            const auto image=SmoUi::ObjectivePointer({reinterpret_cast<const u8*>(bytes.data()),bytes.size()},degrees);
            if(!image)return EDEN_DSMOD_FALSE;
            sink(receiver,image->width,image->height,image->rgba.data(),image->rgba.size());return EDEN_DSMOD_TRUE;
        }
        if(k=="module:smo:bar_fill"){
            const auto image=SmoUi::BarFill();
            sink(receiver,image.width,image.height,image.rgba.data(),image.rgba.size());return EDEN_DSMOD_TRUE;
        }
        if(k=="module:smo:world_bg" || k=="module:smo:world_bg_neutral" || k=="module:smo:slab" || k=="module:smo:slab_tall" || k=="module:smo:flag_ring" || k=="module:smo:title_logo_small" || k=="module:smo:cappy_cursor" || k.starts_with("module:smo:life/")){
            if(!host || !host->read_romfs)return EDEN_DSMOD_FALSE;
            std::optional<fe3h_ui2d::Texture> image;
            if(k=="module:smo:slab" || k=="module:smo:slab_tall")image=SmoUi::Slab(k=="module:smo:slab_tall");
            else{
                const auto path=k=="module:smo:flag_ring"?"base:/LayoutData/MapIcon.szs#layout.lyarc#timg/__Combined.bntx"
                    :(k=="module:smo:world_bg"||k=="module:smo:world_bg_neutral")?"base:/LayoutData/CommonBgParts.szs#layout.lyarc#timg/__Combined.bntx"
                    :k=="module:smo:title_logo_small"?"base:/LayoutData/TitleLogoSmallParts.szs#layout.lyarc#timg/__Combined.bntx"
                    :k=="module:smo:cappy_cursor"?"base:/LayoutData/CursorParts.szs#layout.lyarc#timg/__Combined.bntx"
                    :"base:/LayoutData/CounterLife.szs#layout.lyarc#timg/__Combined.bntx";
                const auto bytes=ReadPackageFile(*host,path);
                auto span=std::span<const u8>{reinterpret_cast<const u8*>(bytes.data()),bytes.size()};
                if(k=="module:smo:world_bg")image=SmoUi::WorldBackground(span);
                else if(k=="module:smo:world_bg_neutral")image=SmoUi::WorldBackgroundNeutral(span);
                else if(k=="module:smo:flag_ring")image=SmoUi::FlagRing(span);
                else if(k=="module:smo:title_logo_small" || k=="module:smo:cappy_cursor"){
                    auto& inst=*static_cast<Instance*>(p);
                    EdenDsmodAstcDecoder decode;void* userdata;
                    {std::scoped_lock lock{inst.capture_mutex};decode=inst.decode_astc;userdata=inst.astc_userdata;}
                    image=k=="module:smo:title_logo_small"?SmoUi::TitleLogoSmall(span,userdata,decode):SmoUi::CappyCursor(span,userdata,decode);
                }
                else{
                    const auto v=k.substr(16);
                    if(v.size()!=3 || v[1]!='/' || v[0]<'0' || v[0]>'9' || v[2]<'1' || v[2]>'9')return EDEN_DSMOD_FALSE;
                    image=SmoUi::Life(span,v[0]-'0',v[2]-'0');
                }
            }
            if(!image)return EDEN_DSMOD_FALSE;
            sink(receiver,image->width,image->height,image->rgba.data(),image->rgba.size());return EDEN_DSMOD_TRUE;
        }
        if(k.starts_with("module:smo:map/")){
            auto& inst=*static_cast<Instance*>(p);
            const auto name=k.substr(15);
            if(name.empty() || name.size()>128 || name.find_first_of("/\\#")!=name.npos)return EDEN_DSMOD_FALSE;
            std::shared_ptr<const std::vector<u8>> archive;
            {std::scoped_lock lock{inst.map_mutex};archive=inst.map_archive;}
            if(!archive){
                const char* path="base:/ObjectData/Texture2dMap.szs";
                const auto size=host->read_romfs(host->userdata,path,0,nullptr,0);
                if(!size || size>(64u<<20))return EDEN_DSMOD_FALSE;
                auto bytes=std::make_shared<std::vector<u8>>(size);
                if(host->read_romfs(host->userdata,path,0,bytes->data(),size)!=size)return EDEN_DSMOD_FALSE;
                archive=bytes;std::scoped_lock lock{inst.map_mutex};inst.map_archive=archive;
            }
            auto decoded=SmoMap::DecodeArchive(*archive,name);
            if(!decoded)return EDEN_DSMOD_FALSE;
            sink(receiver,decoded->image.width,decoded->image.height,decoded->image.rgba.data(),decoded->image.rgba.size());
            decoded->image.rgba.clear();decoded->image.rgba.shrink_to_fit();
            auto metadata=std::make_shared<SmoMap::Resources>(std::move(*decoded));
            std::scoped_lock lock{inst.map_mutex};inst.maps[std::string{name}]=metadata;return EDEN_DSMOD_TRUE;
        }
        constexpr std::string_view capture_prefix = "module:smo:capture/";
        if (k.starts_with(capture_prefix)) {
            if (!host || !host->read_romfs) return EDEN_DSMOD_FALSE;
            auto& inst = *static_cast<Instance*>(p);
            std::shared_ptr<const std::vector<u8>> archive;
            EdenDsmodAstcDecoder decode;
            void* userdata;
            {
                std::scoped_lock lock{inst.capture_mutex};
                archive = inst.capture_archive; decode = inst.decode_astc; userdata = inst.astc_userdata;
            }
            if (!decode) return EDEN_DSMOD_FALSE;
            if (!archive) {
                constexpr const char* source = "base:/ObjectData/HackListTexture.szs";
                const auto size = host->read_romfs(host->userdata, source, 0, nullptr, 0);
                if (!size || size > (64u << 20)) return EDEN_DSMOD_FALSE;
                auto bytes = std::make_shared<std::vector<u8>>(size);
                if (host->read_romfs(host->userdata, source, 0, bytes->data(), size) != size)
                    return EDEN_DSMOD_FALSE;
                archive = bytes;
                std::scoped_lock lock{inst.capture_mutex};
                inst.capture_archive = archive;
            }
            const auto portrait = SmoCapture::DecodeArchive(*archive, k.substr(capture_prefix.size()), userdata, decode);
            if (!portrait) return EDEN_DSMOD_FALSE;
            sink(receiver, portrait->width, portrait->height, portrait->rgba.data(), portrait->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (k == "module:smo:player_pin" || k == "module:smo:player_arrow" || k == "module:smo:camera_cones" || k.starts_with("module:smo:arc_")) {
            if (!host || !host->read_romfs) return EDEN_DSMOD_FALSE;
            auto& inst = *static_cast<Instance*>(p);
            const auto read_asset = [&](const char* source) {
                const auto size = host->read_romfs(host->userdata, source, 0, nullptr, 0);
                if (!size || size > (64u << 20)) return std::vector<u8>{};
                std::vector<u8> bytes(size);
                if (host->read_romfs(host->userdata, source, 0, bytes.data(), size) != size) bytes.clear();
                return bytes;
            };
            if (k == "module:smo:player_pin" || k == "module:smo:player_arrow" || k == "module:smo:camera_cones" || k.starts_with("module:smo:arc_")) {
                unsigned angle = 0;
                if (k.starts_with("module:smo:arc_")) {
                    const auto digits = k.substr(std::string_view{"module:smo:arc_"}.size());
                    if (digits.empty() || digits.size() > 3 || digits.find_first_not_of("0123456789") != std::string_view::npos)
                        return EDEN_DSMOD_FALSE;
                    angle = static_cast<unsigned>(std::strtoul(std::string{digits}.c_str(), nullptr, 10));
                    if (angle >= 360 || angle % 5) return EDEN_DSMOD_FALSE;
                }
                const auto bytes = read_asset("base:/LayoutData/MapPlayer.szs#layout.lyarc#timg/__Combined.bntx");
                const auto pin = k == "module:smo:player_pin" ? SmoMap::DecodePlayerPin(bytes) :
                    k == "module:smo:player_arrow" ? SmoMap::DecodePlayerArrow(bytes) :
                    k == "module:smo:camera_cones" ? SmoMap::DecodeCameraConeAtlas(bytes) : SmoMap::DecodeFacingArc(bytes, angle);
                if (!pin) return EDEN_DSMOD_FALSE;
                sink(receiver, pin->width, pin->height, pin->rgba.data(), pin->rgba.size());
                return EDEN_DSMOD_TRUE;
            }
            return EDEN_DSMOD_FALSE;
        }

        if (!k.starts_with(kImageKeyPrefix)) {
            return EDEN_DSMOD_FALSE;
        }
        const auto digits = k.substr(kImageKeyPrefix.size());
        if (digits.empty() || digits.size() > 2 ||
            digits.find_first_not_of("0123456789") != std::string_view::npos) {
            return EDEN_DSMOD_FALSE;
        }
        const unsigned page = static_cast<unsigned>(std::strtoul(std::string{digits}.c_str(), nullptr, 10));

        auto& inst = *static_cast<Instance*>(p);
        std::shared_ptr<const SmoFont::FontAtlas> atlas;
        {
            std::scoped_lock lock{inst.font_mutex};
            atlas = inst.atlas;
        }
        if (!atlas) {
            return EDEN_DSMOD_FALSE; // not built yet: the host retries a failed page later
        }
        const auto page_image = SmoFont::AtlasPage(atlas->atlas, page);
        if (page_image.Empty()) {
            return EDEN_DSMOD_FALSE;
        }
        sink(receiver, static_cast<std::uint32_t>(page_image.w),
            static_cast<std::uint32_t>(page_image.h), page_image.rgba.data(),
            page_image.rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

// ---- font extension: build the merged atlas once, then just re-report the cached metrics ------

EdenDsmodBool DecodeFontFn(void* p, const std::uint8_t* bytes, std::size_t size, void* receiver,
                           EdenDsmodFontSink sink) {
    try {
        if (!p || !bytes || size == 0 || size > 64) {
            return EDEN_DSMOD_FALSE;
        }
        // Loose marker check: the host calls this with font_metrics_src's exact file contents.
        // A mismatch here (a different package file by accident) should fail closed, not guess.
        if (std::string_view{reinterpret_cast<const char*>(bytes), size}.substr(
                0, kFontMarker.size()) != kFontMarker) {
            return EDEN_DSMOD_FALSE;
        }
        auto& inst = *static_cast<Instance*>(p);
        std::scoped_lock lock{inst.font_mutex};
        if (!inst.atlas) {
            if (!AllFontsReady(inst.font_sources)) {
                return EDEN_DSMOD_FALSE; // the host retries ~once a second, up to 30 times
            }
            inst.atlas = SmoFont::BuildFontAtlasFrom(
                inst.font_sources[static_cast<std::size_t>(FontSlot::Message)].bytes,
                inst.font_sources[static_cast<std::size_t>(FontSlot::Title)].bytes,
                inst.font_sources[static_cast<std::size_t>(FontSlot::Head)].bytes,
                inst.font_sources[static_cast<std::size_t>(FontSlot::Number)].bytes,
                inst.font_sources[static_cast<std::size_t>(FontSlot::Picture)].bytes,
                inst.font_sources[static_cast<std::size_t>(FontSlot::Icon)].bytes);
            if (!inst.atlas) {
                return EDEN_DSMOD_FALSE; // malformed input: fail closed, do not retry forever
            }
        }
        sink(receiver, inst.atlas->line_height, inst.atlas->first_codepoint,
            inst.atlas->glyphs.data(), static_cast<std::uint32_t>(inst.atlas->glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleApi ModuleApi{
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    SmoReader::TitleId,
    "Super Mario Odyssey DSMod",
    EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
    &SupportsBuild,
    &Create,
    &Destroy,
    &Sample,
    &Tick,
};

const EdenDsmodModuleExtensions Extensions{
    EDEN_DSMOD_EXT_VERSION,
    sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,
    &Configure,
    &OnAction,
    &LoadImageFn,
};

const EdenDsmodFontExtensions FontExtensions{
    EDEN_DSMOD_FONT_EXT_VERSION,
    sizeof(EdenDsmodFontExtensions),
    EDEN_DSMOD_FONT_EXT_HASH,
    &DecodeFontFn,
};

const EdenDsmodModuleWriteExtensions WriteExtensions{
    EDEN_DSMOD_WRITE_EXT_VERSION,sizeof(EdenDsmodModuleWriteExtensions),EDEN_DSMOD_WRITE_EXT_HASH,&ConfigureWrite,
};
const EdenDsmodModuleSaveExtensions SaveExtensions{
    EDEN_DSMOD_SAVE_EXT_VERSION,
    sizeof(EdenDsmodModuleSaveExtensions),
    EDEN_DSMOD_SAVE_EXT_HASH,
    &ConfigureSave,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
// No DSMOD_SDK_EXPORT_SAVE_EXTENSIONS macro exists (MODULE_GUIDE.md section 1.5: "a module that
// wants it defines eden_dsmod_get_save_extensions itself"); DSMOD_SDK_DEFINE_GETTER is the same
// generic getter the three macros above expand to.
DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_save_extensions, EdenDsmodModuleSaveExtensions,
                       EDEN_DSMOD_SAVE_EXT_VERSION, EDEN_DSMOD_SAVE_EXT_HASH, SaveExtensions)

DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_write_extensions, EdenDsmodModuleWriteExtensions,
                       EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH, WriteExtensions)
