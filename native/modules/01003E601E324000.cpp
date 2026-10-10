// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Dragon Quest III HD-2D Remake (01003E601E324000) companion module, Switch 1.1.0.0 only
// (build 4F41309B39EEBE5E1B8F2729B8105C35): the field top cards, the field tabs (Map, Party, Bag,
// Journal), the battle page, the native Heal All / Handy Heal All (the game's own field-menu routine,
// run on the game thread by the guest load plan: no input, no menu, dq3_heal.h), and one native drive
// that presses ordinary buttons (the top-card party reorder through the native Line-Up).
//
// No guest memory writes. One guest call: the load plan's helper (tools/dq3/gen_load_plan.py) runs
// the Misc menu's Heal All case when the module asks through the mailbox; the game does the healing,
// the MP costs and the checks. Game state comes from dq3_reader / dq3_map /
// dq3_info / dq3_battle (code-pinned routes); art, tables and the font are decoded at runtime from
// the player's own Nicola-Switch.pak (dq3_pak, dq3_art, dq3_font). The package ships no game data.
//
// Outputs (the host clears the snapshot every tick, so values are published every sample; gate ints
// marked * only while non-zero, since a missing gate reads as closed):
//   dq3.ready        1 while a party is shown; dq3.empty = !ready
//   dq3.p<i>.on      member i (Line-Up order) present
//   dq3.p<i>.name    name, Name style (dq3_font.h), HP-state colour markup (dq3_sprites.h)
//   dq3.p<i>.lv      "<job> · Lv N", Small style, HP-state colour markup
//   dq3.p<i>.hp_t/.mp_t               HP / MP numbers (Value style), HP-state colour markup
//   dq3.p<i>.hp/.hp_max/.mp/.mp_max   field HP/MP and displayed max (GetMaxHP/MP simple path)
//   dq3.p<i>.fcard   the top card as one image (window + looks sprite + gold outline on the member the
//                    Party tab shows; dq3_info.h fcard/)
//   dq3.p<i>.st<k>.on/.src/.x         up to two status icons (poison, paralysis) after the name
//   dq3.party.reorder, dq3.party.drag<i>*, dq3.p<i>.nodrag*   top-card drag reorder gates
//   dq3.party.s<k>p<p>*  party reorder press of button k with counter target p (dq3_party_order.h)
//   dq3.heal.ready   the Heal All / Handy Heal All buttons are usable (field free, the load plan's
//                    helper alive, no request in flight)
//   dq3.heal.msg / dq3.heal.msg.on*   the Misc menu's own result line for the last request, ~4 s
//   dq3.input.pending*   a party drive press is pending (the manifest's enforce_gate)
//   dq3.battle       1 while a battle runs (BattleLogic state != NONE): the page bind shows the
//                    battle page; WAIT_MENU after WIN / RUN is the result-message wait, still battle
//   dq3.b.* / dq3.e<i>.* / dq3.bp<k>.* / dq3.ed.* / dq3.pd.*   battle page values (dq3_battle_view.h)
//   dq3.tab / dq3.tab.party / dq3.tab.bag / dq3.tab.journal   field tab (0 Map, 1 Party, 2 Bag,
//                    3 Journal); dq3.ftabs + dq3.ftab<k>.x/.t the command window
//   dq3.m.* / dq3.mv*      field Map page values (dq3_map.h BuildMapView); dq3.m.img / dq3.m.img.next the shown
//                    map picture and its hidden prefetch (dq3_map_swap.h: a new key shows once composed)
//   dq3.pp.* / pr.* / qg* / ps.* / qs* / dt.*    Party tab (dq3_info_view.h BuildPartyView)
//   dq3.bg.* / qg* / dt.*           Bag tab (dq3_info_view.h BuildBagView)
//   dq3.jn / jo / jt / jm / jr      Journal tab (dq3_info_view.h BuildJournalView)
// Diagnostics (no manifest binding; published only with EDEN_DSMOD_DQ3_ALL_OUTPUTS=1):
//   dq3.state (0 no game, 1 party read, 2 rejected), dq3.held, dq3.count, dq3.why, dq3.world,
//   dq3.sample_us, dq3.b.view_us
// Actions: "bsel" <arg> = tap on enemy cell arg (0..9) or party card 100 + k; a second tap on the
// same target clears the selection. "ftab" <0..3> selects a field tab, "floor" <+1 / -1> browses
// the visited floors on the Map page, "fullmap" toggles the full-size map; "partypick" <k> shows top
// card k on the Party tab (ignored on the Bag tab), "partytab" (Carried / Spells), "partyitem",
// "partyslot" (equipped slot), "bagpage", "bagitem" and "journalmon" select inside the tabs
// (companion-side only, no guest writes). "partydrop<k>" <source> swaps two top cards through the
// native Line-Up menu with ordinary button input (dq3_party_order.h); "heal" <0 Heal All | 1 Handy>
// asks the load plan's helper (dq3_heal.h). The party drive takes each press's target from the host's
// button-action counter (@dq3_party_button<k>), so a cleared drive can never leave the module and the
// host out of step.
// Module images: module:dq3:<art key> (dq3_art.h, dq3_info.h, dq3_battle_art.h, dq3_map.h) and
// module:dq3:font/1.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "dq3_art.h"
#include "dq3_battle.h"
#include "dq3_battle_art.h"
#include "dq3_battle_view.h"
#include "dq3_font.h"
#include "dq3_heal.h"
#include "dq3_info.h"
#include "dq3_info_view.h"
#include "dq3_keys.h"
#include "dq3_map.h"
#include "dq3_map_swap.h"
#include "dq3_page_layout.h"
#include "dq3_party_order.h"
#include "dq3_pak.h"
#include "dq3_reader.h"
#include "dq3_sprites.h"
#include "dq3_tables.h"

namespace {

using namespace dsmod_sdk::int_types;
namespace keys = dq3::keys;

constexpr u64 TitleId = 0x01003E601E324000ULL;
constexpr std::string_view BuildId =
    "4F41309B39EEBE5E1B8F2729B8105C3500000000000000000000000000000000";
constexpr std::string_view KeyPrefix = "module:dq3:";
constexpr int ReadEvery = 6;       // samples between party reads (10 Hz at 60 Hz)
constexpr int HoldInvalid = 30;    // samples a rejected read may reuse the last good snapshot
constexpr int ResolveRetry = 120;  // samples between resolve attempts after a failure
constexpr u32 IconPoison = 10, IconParalysis = 11; // StatusIcon_00 cells (revision 7 design)
constexpr int IconPx = 28, IconGap = 8, IconStep = 32;
constexpr auto TableRetry = std::chrono::seconds(10); // a table load that failed for want of the romfs
constexpr auto HealMsgFor = std::chrono::seconds(4);  // the companion shows a heal result line this long

std::string BuildHex(const EdenDsmodHostApi& h) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const u8 b : h.build_id) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// PakPath views a string literal, so its data() is NUL-terminated (no per-read std::string).
static_assert(dq3::PakPath.data()[dq3::PakPath.size()] == '\0');

/// `size_ok` latches the pinned archive size (checked until it matches; the romfs may open late). It is the
/// Module's, shared by its readers: a new game session (a new Module) checks again.
dq3::RangeReader MakePakReader(const EdenDsmodHostApi& h, std::shared_ptr<std::atomic<bool>> size_ok) {
    return [read_romfs = h.read_romfs, userdata = h.userdata, size_ok = std::move(size_ok)](
               u64 offset, void* out, std::size_t size) {
        if (!read_romfs || size == 0 || size > 64 * 1024 * 1024 || offset > dq3::PakSize ||
            size > dq3::PakSize - offset)
            return false;
        if (!size_ok->load(std::memory_order_acquire)) {
            if (read_romfs(userdata, dq3::PakPath.data(), 0, nullptr, 0) != dq3::PakSize)
                return false;
            size_ok->store(true, std::memory_order_release);
        }
        return read_romfs(userdata, dq3::PakPath.data(), offset, out, size) == size;
    };
}

void LogTo(const EdenDsmodHostApi* h, u32 level, const std::string& s) {
    if (h && h->log)
        h->log(h->userdata, level, s.c_str());
}

/// Diagnostic outputs (dq3.state, why, ...) are published only when this environment variable is set.
bool AllOutputsRequested() {
    const char* v = std::getenv("EDEN_DSMOD_DQ3_ALL_OUTPUTS");
    return v && *v && std::string_view{v} != "0";
}

/// EDEN_DSMOD_DQ3_TEST_REFUSE (controlled tests of the refusal screens, never set by a package): "build" makes
/// supports_build refuse (the host's own refusal, the manifest's host-font fallback page); "pins" treats the code
/// pins as mismatched (the module's wrong-patch screen with game art); "pins-flat" does that and also takes the
/// game art as undecodable (the module publishes the flat fallback instead).
/// EDEN_DSMOD_DQ3_MAP_TRACE (diagnostics, never set by a package): log every Map image swap step (the key the page
/// wants, the prefetch request and the shown key, with the sample index), research runs/map-stable-0.10.2.
bool MapTraceRequested() {
    const char* v = std::getenv("EDEN_DSMOD_DQ3_MAP_TRACE");
    return v && *v && std::string_view{v} != "0";
}

std::string_view TestRefuse() {
    const char* v = std::getenv("EDEN_DSMOD_DQ3_TEST_REFUSE");
    return v ? std::string_view{v} : std::string_view{};
}

/// A resolve failure that is the game's code itself (a pinned word differs or a decoded address falls outside
/// main), not a read that may succeed later.
bool CodeMismatch(std::string_view why) {
    return why.starts_with("code pin main+") || why.find("outside main") != std::string_view::npos;
}

/// The game tables the tick thread needs, loaded on the asset worker (LoadImage).
struct TableLoader {
    const char* label;
    bool (*cached)();
    bool (*load)(const dq3::RangeReader&, std::string*);
};
const TableLoader TableLoaders[] = {
    {"battle tables", [] { return dq3::CachedBattleData() != nullptr; },
     [](const dq3::RangeReader& r, std::string* w) { return dq3::LoadBattleData(r, w) != nullptr; }},
    {"game text", [] { return dq3::CachedGameText() != nullptr; },
     [](const dq3::RangeReader& r, std::string* w) { return dq3::LoadGameText(r, w) != nullptr; }},
    {"map tables", [] { return dq3::CachedMapData() != nullptr; },
     [](const dq3::RangeReader& r, std::string* w) { return dq3::LoadMapData(r, w) != nullptr; }},
    {"party / journal tables", [] { return dq3::CachedInfoData() != nullptr; },
     [](const dq3::RangeReader& r, std::string* w) { return dq3::LoadInfoData(r, w) != nullptr; }},
};
constexpr std::size_t TableCount = std::size(TableLoaders);

/// A load that failed because the archive could not be read (romfs not open yet) is retried; a
/// hash, decode, parse or sanity failure is deterministic for this archive and is not.
bool TransientFailure(std::string_view why) {
    return why.find("header read") != std::string_view::npos ||
           why.find("payload read") != std::string_view::npos || why.find("no reader") != std::string_view::npos;
}

// One member's published strings, rebuilt only when the snapshot or the game text changes.
struct Card {
    std::string name, lv, hp_t, mp_t;
    std::u32string job;
    std::optional<dq3::u32> looks_index;   ///< the member's looks row (coffin when dead): fcard / portrait / head
    std::optional<dq3::u32> sprite_index;  ///< the alive looks row (battle cards)
    std::optional<dq3::u32> coffin_index;  ///< the class coffin (battle cards)
    std::string fcard[2];                  ///< module:dq3:fcard/<looks|->/<0|1> (unselected, selected)
    std::string st_src[2];
    int st_x[2]{};
    int st_n{};
    int lv_x{};                            ///< wide cards (N <= 2): the job line's offset after the name and icons
    int hps{};
};

/// The looks sprite as an art key token ("-" = none).
std::string SpriteToken(const Card& c) {
    return c.looks_index ? std::to_string(*c.looks_index) : std::string{"-"};
}

/// Sum and maximum of a timing over the 600-sample stats window.
struct Stat {
    double sum{}, max{};
    void Add(double v) {
        sum += v;
        max = std::max(max, v);
    }
    void Reset() {
        sum = max = 0.0;
    }
    double Avg(int n) const {
        return n ? sum / n : 0.0;
    }
};

class Module {
public:
    explicit Module(const EdenDsmodHostApi& api)
        : host{api}, pak_size_ok{std::make_shared<std::atomic<bool>>(false)}, pak{MakePakReader(api, pak_size_ok)},
          all_outputs{AllOutputsRequested()}, map_trace{MapTraceRequested()} {}

    void UpdateHost(const EdenDsmodHostApi& api) {
        host = api;
    }

    void Sample() {
        const auto t0 = Clock::now();
        log_us = 0.0;
        SampleInner();
        if (!non_ascii_logged && dq3::NonAsciiTextCount()) {
            // release builds: a non-ASCII byte string reached the byte-wise text path and was drawn with '?'
            non_ascii_logged = true;
            Log(EDEN_DSMOD_LOG_WARNING, ("DQ3 DSMod: non-ASCII text in the byte-wise text path replaced with '?' (first: \"" +
                                         dq3::FirstNonAsciiText() + "\")").c_str());
        }
        const double us = Us(t0, Clock::now());
        cost.Add(us);
        log_max = std::max(log_max, log_us);
        if (++cost_n == 600) {
            cost_avg = cost.sum / cost_n;
            if (all_outputs) { // diagnostics only: the line is not built otherwise
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                              "DQ3 DSMod: sample avg %.1f us, max %.1f us over 600 samples (scene avg "
                              "%.1f max %.1f, party avg %.1f max %.1f per read, publish avg %.1f max "
                              "%.1f, log max %.1f, reads %d, battle avg %.1f max %.1f, map avg %.1f max %.1f, map "
                              "view %.1f, party view %.1f, bag view %.1f, journal view %.1f); state %d, party %zu%s%s",
                              cost_avg, cost.max, scene_stat.Avg(reads_window), scene_stat.max,
                              party_stat.Avg(reads_window), party_stat.max, publish_stat.Avg(cost_n), publish_stat.max,
                              log_max, reads_window, battle_stat.Avg(reads_window), battle_stat.max,
                              map_stat.Avg(reads_window), map_stat.max, map_view_us, party_view_us, bag_view_us, journal_view_us,
                              StateCode(), shown.members.size(), held ? ", held" : "", battle_on ? ", battle" : "");
                Log(EDEN_DSMOD_LOG_INFO, buf);
            }
            cost.Reset();
            scene_stat.Reset();
            party_stat.Reset();
            publish_stat.Reset();
            battle_stat.Reset();
            map_stat.Reset();
            log_max = log_us; // this stats line's own cost lands in the next window
            cost_n = reads_window = 0;
        }
        if (all_outputs)
            F64("dq3.sample_us", cost_avg);
    }

    EdenDsmodBool DecodeFont(const u8* bytes, std::size_t size, void* receiver,
                             EdenDsmodFontSink sink) {
        if (!bytes || !sink || !dq3::IsFontSpec({bytes, size}))
            return EDEN_DSMOD_FALSE;
        std::string why;
        const auto font = dq3::BuildFontAtlas(pak, &why);
        if (!font) {
            Log(EDEN_DSMOD_LOG_WARNING, ("DQ3 DSMod: font not ready: " + why).c_str());
            return EDEN_DSMOD_FALSE;
        }
        sink(receiver, font->line_height, font->first_codepoint, font->glyphs.data(),
             static_cast<u32>(font->glyphs.size()));
        return EDEN_DSMOD_TRUE;
    }

    // Asset worker: touches only the internally locked art library, font and table caches.
    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const std::string_view k{key};
        if (!k.starts_with(KeyPrefix))
            return EDEN_DSMOD_FALSE;
        const auto logical = k.substr(KeyPrefix.size());
        const auto reader = MakePakReader(*image_host, pak_size_ok);
        if (logical == "font/1") {
            const auto font = dq3::BuildFontAtlas(reader);
            if (!font)
                return EDEN_DSMOD_FALSE;
            sink(receiver, font->atlas.width, font->atlas.height, font->atlas.rgba.data(),
                 font->atlas.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        // Job names, HP-state colours and the page tables come from the archive too; built once, here.
        LoadTables(image_host, reader);
        std::string why;
        std::shared_ptr<const dq3::Image> shared;
        std::optional<dq3::Image> composed;
        const dq3::Image* img = nullptr;
        if (dq3::IsMapArtKey(logical)) {
            // map art bypasses the library's LRU: map images are large and change with the marker
            composed = dq3::ComposeMapArt(reader, logical, Astc(), &why);
            img = composed ? &*composed : nullptr;
            // the Map page image swap (dq3_map_swap.h) switches the shown key only to a composed one
            if (logical.starts_with("mapimg/") || logical.starts_with("fieldimg/"))
                map_ready.Note(k, img != nullptr, img ? img->rgba.size() : 0);
        } else {
            shared = art.Load(reader, logical, &why);
            img = shared.get();
        }
        if (!img) {
            LogTo(image_host, EDEN_DSMOD_LOG_WARNING,
                  "DQ3 DSMod: image " + std::string{logical} + " not ready: " + why);
            return EDEN_DSMOD_FALSE;
        }
        sink(receiver, img->width, img->height, img->rgba.data(), img->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    void Configure(const EdenDsmodHostExtensions* ext) {
        if (!ext || ext->struct_size < sizeof(EdenDsmodHostExtensions))
            return;
        astc_fn.store(ext->decode_astc, std::memory_order_release);
        astc_user.store(ext->userdata, std::memory_order_release);
        // the guest load plan's mailbox (dq3_heal.h); absent without the plan (buttons stay disabled)
        if (ext->mailbox_address && ext->mailbox_size >= dq3::heal_mb::Size && ext->load_u32 && ext->store_u32 &&
            ext->load_u64) {
            void* const user = ext->userdata;
            const auto load32 = ext->load_u32;
            const auto store32 = ext->store_u32;
            const auto load64 = ext->load_u64;
            heal_box.Configure({[=](u32 off, u32* v) { return load32(user, off, v) == EDEN_DSMOD_TRUE; },
                                [=](u32 off, u32 v) { return store32(user, off, v) == EDEN_DSMOD_TRUE; },
                                [=](u32 off, u64* v) { return load64(user, off, v) == EDEN_DSMOD_TRUE; }});
        }
    }
    dq3::AstcFn Astc() const {
        const auto fn = astc_fn.load(std::memory_order_acquire);
        void* user = astc_user.load(std::memory_order_acquire);
        if (!fn)
            return {};
        return [fn, user](dq3::u32 w, dq3::u32 h, const dq3::u8* in, std::size_t n, dq3::u8* out, std::size_t m) {
            return fn(user, w, h, 8, 8, in, n, out, m) == EDEN_DSMOD_TRUE;
        };
    }

private:
    using Clock = std::chrono::steady_clock;
    static double Us(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::micro>(b - a).count();
    }

    /// Loads the missing tables. A failed loader is skipped while its failure is recent (a romfs
    /// that is not readable yet) or for good (hash / parse / sanity), and each reason is logged once.
    void LoadTables(const EdenDsmodHostApi* image_host, const dq3::RangeReader& reader) {
        for (std::size_t i = 0; i < TableCount; ++i) {
            const TableLoader& l = TableLoaders[i];
            if (l.cached())
                continue;
            const auto now = Clock::now();
            {
                std::scoped_lock lock{loader_mutex};
                const LoaderFailure& f = loader_failures[i];
                if (f.failed && (f.permanent || now - f.at < TableRetry))
                    continue;
            }
            std::string why;
            if (l.load(reader, &why))
                continue;
            std::scoped_lock lock{loader_mutex};
            LoaderFailure& f = loader_failures[i];
            f.failed = true;
            f.at = now;
            f.permanent = !TransientFailure(why);
            if (why != f.logged) {
                f.logged = why;
                LogTo(image_host, EDEN_DSMOD_LOG_WARNING,
                      std::string{"DQ3 DSMod: "} + l.label + " not ready: " + why +
                          (f.permanent ? " (not retried)" : ""));
            }
        }
    }

    dq3::GuestRead Reader() {
        return [this](u64 at, void* out, std::size_t n) {
            return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, out, n);
        };
    }

    int StateCode() const {
        return published_state == dq3::PartyState::Ok       ? 1
               : published_state == dq3::PartyState::NoGame ? 0
                                                            : 2;
    }

    /// Shared gate of the field actions (healing, the party drive): a free field in an adventure with a valid party.
    bool FieldInputAllowed() const {
        return resolved && mroots.ok && !battle_on && !held && !cur_scene.loading &&
               published_state == dq3::PartyState::Ok && cur_scene.InAdventure() && !shown.members.empty();
    }

    /// The native field menu, with the UI widget classes cached for the current world.
    std::optional<dq3::FieldMenuState> ReadMenu() {
        if (menu_classes_world != cur_scene.world_ptr) {
            menu_classes.clear();
            menu_classes_world = cur_scene.world_ptr;
        }
        return dq3::ReadFieldMenu(Reader(), roots, &menu_classes);
    }

    /// The target of a new press: the host's button-action counter + 1 (mod 2), read when the press
    /// is planned and kept while it is pending. A host without get_i64 keeps a private parity.
    bool NextTarget(const char* counter, bool& fallback) {
        if (host.get_i64)
            fallback = ((host.get_i64(host.userdata, counter, 0) + 1) & 1) != 0;
        else
            fallback = !fallback;
        return fallback;
    }

    /// A Map page int of the current view (0 when absent).
    s64 MapInt(const char* k) const {
        const auto it = mview.ints.find(k);
        return it == mview.ints.end() ? 0 : it->second;
    }

    void SampleInner() {
        if (!resolved && sample_index >= next_resolve) {
            const std::string_view test = TestRefuse();
            const bool test_pins = test == "pins" || test == "pins-flat";
            const std::string bad = test_pins ? std::string{"code pin main+0x0 word 0 = 00000000 (test flag "
                                                            "EDEN_DSMOD_DQ3_TEST_REFUSE)"}
                                              : dq3::Resolve(Reader(), host.main_base, host.main_size, roots);
            resolved = bad.empty();
            refused = !resolved && CodeMismatch(bad);
            refused_flat_test = test == "pins-flat";
            if (!resolved) {
                next_resolve = sample_index + ResolveRetry;
                resolve_error = "resolve: " + bad;
                if (resolve_error != logged_resolve_error) {
                    Log(EDEN_DSMOD_LOG_ERROR, ("DQ3 DSMod: " + resolve_error).c_str());
                    logged_resolve_error = resolve_error;
                }
            } else {
                dq3::ClearFNameCache(); // a fresh pool: nothing read before this resolve is reused
                chest_scan.Clear();     // and no UClass* / actor array of the previous one
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "DQ3 DSMod: code pins ok, G slot main+0x%llx, name pool main+0x%llx, "
                              "class table main+0x%llx, job ids %s",
                              static_cast<unsigned long long>(roots.g_slot - host.main_base),
                              static_cast<unsigned long long>(roots.name_pool - host.main_base),
                              static_cast<unsigned long long>(roots.class_table - host.main_base),
                              roots.job_ids_ok ? "ok" : "MISMATCH (job names off)");
                Log(EDEN_DSMOD_LOG_INFO, buf);
                const std::string bad_battle =
                    dq3::ResolveBattle(Reader(), host.main_base, host.main_size, broots);
                battle_resolved = bad_battle.empty() && broots.g_slot == roots.g_slot;
                const std::string bad_record = dq3::ResolveRecord(Reader(), host.main_base, host.main_size);
                record_ok = bad_record.empty();
                const std::string bad_menu = dq3::ResolveBattleMenu(Reader(), host.main_base, host.main_size);
                battle_menu_ok = bad_menu.empty();
                battle_menu_classes.clear();
                Log(battle_menu_ok ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_WARNING,
                    ("DQ3 DSMod: battle menu pins " +
                     (battle_menu_ok ? std::string{"ok"} : bad_menu + " (Follow shows the acting unit only)"))
                        .c_str());
                const std::string bad_info = dq3::ResolveInfo(Reader(), host.main_base, host.main_size, iroots);
                Log(bad_info.empty() ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_ERROR,
                    ("DQ3 DSMod: party / journal pins " + (bad_info.empty() ? std::string{"ok"} : bad_info)).c_str());
                const std::string bad_map = dq3::ResolveMap(Reader(), host.main_base, host.main_size, mroots);
                Log(bad_map.empty() ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_ERROR,
                    ("DQ3 DSMod: map pins " + (bad_map.empty() ? std::string{"ok"} : bad_map)).c_str());
                char slot[32];
                std::snprintf(slot, sizeof(slot), "%llx",
                              static_cast<unsigned long long>(broots.class_slot - host.main_base));
                const std::string line =
                    "DQ3 DSMod: battle pins " +
                    (battle_resolved ? std::string{"ok"} : bad_battle.empty() ? std::string{"G slot mismatch"} : bad_battle) +
                    " (class slot main+0x" + slot + "), record pins " +
                    (record_ok ? std::string{"ok"} : bad_record + " (affinities hidden)");
                Log(battle_resolved && record_ok ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_WARNING, line.c_str());
            }
        }
        ++sample_index;
        if (resolved && (sample_index % ReadEvery) == 0) {
            ReadNow();
            const auto tb = Clock::now();
            ReadBattleNow();
            battle_stat.Add(Us(tb, Clock::now()));
            const auto tm = Clock::now();
            ReadMapNow();
            ReadInfoNow();
            map_stat.Add(Us(tm, Clock::now()));
        }
        else if (!resolved) {
            published_state = dq3::PartyState::Invalid;
            why = resolve_error;
            shown = {};
            held = false;
        }
        const auto t0 = Clock::now();
        // The field menu, read at most once per sample for the heal gate and the party drive.
        const bool allowed = FieldInputAllowed();
        MenuRead menu;
        menu.cadence = sample_index % ReadEvery == 0;
        if (allowed && (party.active || menu.cadence))
            menu.menu = ReadMenu();
        UpdateHeal(allowed, menu);
        UpdatePartyDrive(allowed, menu);
        Publish();
        publish_stat.Add(Us(t0, Clock::now()));
    }

    void ReadNow() {
        const dq3::GuestRead read = Reader();
        ++reads_window;
        const auto t0 = Clock::now();
        dq3::PartySnapshot snap;
        std::string reason;
        // Scene gate first: the title screen already holds the autosave's roster.
        dq3::SceneState scene;
        const bool scene_ok = dq3::ReadScene(read, roots, scene, &scene_cache);
        const auto t1 = Clock::now();
        scene_stat.Add(Us(t0, t1));
        world = scene_ok ? scene.world : "?";
        // a world swap: the engine objects are unreadable between two worlds; after the title or the field
        // world this is a load (title -> adventure, adventure -> title), at boot it is not
        if (scene_ok)
            last_world = scene.world;
        world_swap = !scene_ok && (last_world == "title" || last_world == "FieldTop");
        ReadTitleLoad(read, scene_ok, scene);
        // the current scene for every reader and gate, refreshed on every read whatever the party
        // read below decides (an unreadable scene leaves it empty: the gates fail closed)
        cur_scene = scene_ok ? scene : dq3::SceneState{};
        LogScene(scene_ok, scene);
        // A map change inside the same FieldTop world: keep the last party, read nothing.
        if (scene_ok && scene.Transition() && published_state == dq3::PartyState::Ok &&
            !shown.members.empty() && scene.world_ptr == party_world) {
            if (!held)
                Log(EDEN_DSMOD_LOG_INFO, "DQ3 DSMod: holding the party through a map transition");
            held = true;
            return;
        }
        if (held)
            Log(EDEN_DSMOD_LOG_INFO, "DQ3 DSMod: transition over, reading again");
        held = false;
        dq3::PartyState st = dq3::PartyState::NoGame;
        if (!scene_ok)
            reason = "no adventure: engine objects unreadable";
        else if (!scene.InAdventure() || scene.loading != 0)
            reason = "no adventure: world " + scene.world + (scene.pawn ? ", pawn" : ", no pawn") +
                     (scene.loading ? ", loading" : "");
        else
            st = dq3::ReadParty(read, roots.g_slot, snap, &reason);
        party_stat.Add(Us(t1, Clock::now()));
        if (st == dq3::PartyState::Ok)
            AddEquipMax(read, snap);
        if (st == dq3::PartyState::Ok) {
            if (all_outputs && (!(snap == shown) || published_state != dq3::PartyState::Ok))
                LogParty(snap);
            if (!(snap == shown))
                cards_dirty = true;
            shown = std::move(snap);
            published_state = st;
            party_world = scene.world_ptr;
            invalid_run = 0;
            why.clear();
            if (shown.flag9c_entries && !logged_9c) {
                Log(EDEN_DSMOD_LOG_WARNING,
                    "DQ3 DSMod: roster entry with C+0x9C != 0 seen (unverified list path)");
                logged_9c = true;
            }
            return;
        }
        if (st == dq3::PartyState::NoGame) {
            // title / boot / a save loading: the next save must not show this one's map state
            ResetMapState();
            DropParty(st, reason, EDEN_DSMOD_LOG_INFO, "no party: ");
            return;
        }
        // Invalid / torn: keep the last good snapshot briefly, then fail closed.
        if (published_state == dq3::PartyState::Ok && ++invalid_run <= HoldInvalid)
            return;
        DropParty(st, reason, EDEN_DSMOD_LOG_WARNING, "party rejected: ");
    }

    /// The title's save load (research runs/system-screens, logs/title-ui-load.txt): on the title world the save
    /// is confirmed by the Yes / No (UIMessageWindowListYesNo); once a reading has no Yes / No any more and no list
    /// taking input (no *List* controller in state 1), twice in a row, the game is loading that save while the
    /// title shows its loading book. Latched: the menus closing one by one (ListTop state 0, then none) keep it,
    /// until a list takes input again (a "No" returns to the save list) or the world changes (world_swap / the
    /// field take over).
    void ReadTitleLoad(const dq3::GuestRead& read, bool scene_ok, const dq3::SceneState& scene) {
        if (!scene_ok || scene.world != "title") {
            if (scene_ok) {
                title_yesno = title_loading = false;
                title_idle = 0;
            }
            return;
        }
        if (title_classes_world != scene.world_ptr) {
            title_classes.clear();
            title_classes_world = scene.world_ptr;
        }
        const auto list = dq3::ReadUiControllers(read, roots, &title_classes);
        if (!list) {
            if (!title_unreadable_logged) {
                Log(EDEN_DSMOD_LOG_INFO, "DQ3 DSMod: title UI controllers unreadable");
                title_unreadable_logged = true;
            }
            return; // unreadable: keep
        }
        {
            std::string line;
            for (const auto& c : *list)
                line += " " + c.name + (c.name.find("List") != std::string::npos ? ":" + std::to_string(c.state) : "");
            if (line != logged_title_ui) {
                Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: title UI" + (line.empty() ? std::string{" (none)"} : line)).c_str());
                logged_title_ui = line;
            }
        }
        const bool yesno = std::any_of(list->begin(), list->end(),
                                       [](const dq3::UiController& c) { return c.name == "UIMessageWindowListYesNo"; });
        const bool active = std::any_of(list->begin(), list->end(), [](const dq3::UiController& c) {
            return c.state == 1 && c.name.find("List") != std::string::npos;
        });
        if (yesno && active) {
            title_yesno = true; // the question is up
            title_idle = 0;
            return;
        }
        if (active) { // a list takes input again: no load
            title_yesno = title_loading = false;
            title_idle = 0;
            return;
        }
        if (title_loading || !title_yesno || yesno)
            return;
        if (++title_idle >= 2) {
            title_loading = true;
            Log(EDEN_DSMOD_LOG_INFO, "DQ3 DSMod: title Yes / No decided, no list active (save loading)");
        }
    }

    void DropParty(dq3::PartyState st, const std::string& reason, u32 level, const char* prefix) {
        if (published_state != st || why != reason)
            Log(level, (std::string{"DQ3 DSMod: "} + prefix + reason).c_str());
        shown = {};
        cards_dirty = true;
        published_state = st;
        party_world = 0;
        why = reason;
    }

    /// Forgets the location, visited floors, chests and world icons (and the cached world objects).
    void ResetMapState() {
        if (lcache.world == 0 && !loc_ok && !visited_ok && world_icons.empty() && !chests)
            return;
        lcache = {};
        loc_ok = visited_ok = false;
        visited.clear();
        world_icons.clear();
        world_icons_at = visited_at = chests_at = 0;
        chests.reset();
        chest_scan.Clear();
        map_dirty = true;
        map_ready.Clear();
    }

    // Class name (guest FString table) and armour FName text, cached: both are static strings.
    std::optional<std::string> ClassName(u8 vocation) {
        if (vocation >= class_names.size())
            return std::nullopt;
        auto& slot = class_names[vocation];
        if (!slot)
            slot = dq3::ReadClassName(Reader(), roots, vocation);
        return slot;
    }
    /// FName text ({0, 0} = ""); dq3::ReadFName caches the pool's entries.
    std::optional<std::string> FNameText(const u32 name[2]) {
        if (name[0] == 0 && name[1] == 0)
            return std::string{};
        return dq3::ReadFName(Reader(), roots.name_pool, name[0], name[1]);
    }

    void BuildCards() {
        bool retry = false;
        const auto text = dq3::CachedGameText();
        const auto font = dq3::CachedFontAtlas();
        cards_text = text.get();
        cards_font = font.get();
        cards.clear();
        for (const auto& m : shown.members) {
            Card c;
            c.hps = dq3::HpState(m);
            const u32 colour = !text || c.hps == 0 ? 0
                               : c.hps == 2        ? text->colours.dead
                               : c.hps == 3        ? text->colours.low
                                                   : text->colours.half;
            const auto paint = [colour](const std::string& styled) {
                return colour ? dq3::Colour(colour, styled) : styled;
            };
            const std::string name = dq3::Styled(m.name, dq3::FontStyle::Bold32);
            c.name = paint(name);
            std::u32string lv;
            if (text && roots.job_ids_ok)
                if (const auto id = dq3::JobTextId(m.vocation, shown.roto))
                    if (const auto it = text->jobs.find(*id); it != text->jobs.end()) {
                        lv = it->second + U" · ";
                        c.job = it->second;
                    }
            lv += dq3::AsciiToU32("Lv " + std::to_string(m.level));
            c.lv = paint(dq3::Styled(lv, dq3::FontStyle::Semi24));
            c.hp_t = paint(dq3::Styled(std::to_string(m.hp), dq3::FontStyle::Semi30));
            c.mp_t = paint(dq3::Styled(std::to_string(m.mp), dq3::FontStyle::Semi30));
            // Sprite: the game's looks row (coffin when KO, costume armour rows included).
            const auto cls = ClassName(m.vocation);
            const auto armour = m.armour_read ? FNameText(m.armour) : std::nullopt;
            if (!cls || !armour)
                retry = true; // guest strings unreadable right now: try again later
            if (cls && armour) {
                const std::string id = dq3::LooksId(m, *cls, *armour);
                c.looks_index = dq3::FindLooks(id);
                dq3::PartyMember alive = m;
                alive.status &= ~1u;
                c.sprite_index = dq3::FindLooks(dq3::LooksId(alive, *cls, *armour));
                c.coffin_index = dq3::FindLooks("UNIT_LOOKS_PC_COFFIN_" + *cls);
                if (id != logged_looks[m.slot & 3]) {
                    Log(EDEN_DSMOD_LOG_INFO,
                        ("DQ3 DSMod: slot " + std::to_string(m.slot) + " looks " + id + " (armour " +
                         (armour->empty() ? "none" : *armour) + ") -> " +
                         (c.looks_index ? "sprite " + SpriteToken(c) : std::string{"no sprite"}))
                            .c_str());
                    logged_looks[m.slot & 3] = id;
                }
            }
            // the whole card window + sprite as one image (the drag ghost of the top-card reorder);
            // the gold outline marks the member the Party tab shows
            for (int sel = 0; sel < 2; ++sel)
                c.fcard[sel] = std::string{KeyPrefix} + "fcard/" + SpriteToken(c) + (sel ? "/1" : "/0");
            const int name_w = font ? font->Measure(name) : 0;
            u32 icons[2]{};
            if (m.Poisoned())
                icons[c.st_n++] = IconPoison;
            if (m.Paralysed())
                icons[c.st_n++] = IconParalysis;
            if (!font)
                c.st_n = 0;
            c.lv_x = c.st_n ? name_w + IconGap + (c.st_n - 1) * IconStep + IconPx + 12 : name_w + 14;
            for (int k = 0; k < c.st_n; ++k) {
                c.st_x[k] = name_w + IconGap + k * IconStep;
                c.st_src[k] = "module:dq3:status/" + std::to_string(icons[k]) + "/" +
                              std::to_string(IconPx);
            }
            cards.push_back(std::move(c));
        }
        cards_dirty = false;
        map_dirty = true; // the no-map card shows the hero's sprite
        ++cards_gen;
        cards_retry_at = retry ? sample_index + 60 : 0;
    }

    void Publish() {
        I64("dq3.battle", battle_on ? 1 : 0);
        if (battle_on) {
            PublishBattle();
            return;
        }
        const bool ready = published_state == dq3::PartyState::Ok && !shown.members.empty();
        if (all_outputs) {
            I64("dq3.state", StateCode());
            I64("dq3.held", ready && held ? 1 : 0);
            I64("dq3.count", ready ? static_cast<s64>(shown.members.size()) : 0);
            Text("dq3.why", why);
            Text("dq3.world", world);
        }
        I64("dq3.ready", ready ? 1 : 0);
        I64("dq3.empty", ready ? 0 : 1);
        PublishSystem(ready);
        if (cards_dirty || cards_text != dq3::CachedGameText().get() ||
            cards_font != dq3::CachedFontAtlas().get() ||
            (cards_retry_at && sample_index >= cards_retry_at))
            BuildCards();
        const int selected = ready && tab == 1 ? PartySel() : -1;
        // the dynamic strip (owner 2026-10-10): N live members share the width; fcard/ is keyed at that width
        // (N = 4 keeps the 286 px key without the width part)
        const int strip_n = StripCount(ready);
        const std::string fcard_w = strip_n >= 1 && strip_n <= 3 ? "/" + std::to_string(dq3::StripCardW(strip_n)) : "";
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& key = keys::Member[i];
            const bool on = ready && i < shown.members.size() && i < cards.size();
            I64(key[keys::MOn], on ? 1 : 0);
            if (!on)
                continue;
            const dq3::PartyMember& m = shown.members[i];
            const Card& c = cards[i];
            Text(key[keys::MName], c.name);
            Text(key[keys::MLv], c.lv);
            Text(key[keys::MHpT], c.hp_t);
            Text(key[keys::MMpT], c.mp_t);
            Text(key[keys::MFcard], c.fcard[static_cast<int>(i) == selected ? 1 : 0] + fcard_w);
            I64(keys::LvX[i], c.lv_x);
            I64(key[keys::MHp], m.hp);
            I64(key[keys::MHpMax], m.hp_max);
            I64(key[keys::MMp], m.mp);
            I64(key[keys::MMpMax], m.mp_max);
            static constexpr int st_keys[2][3] = {{keys::MSt0On, keys::MSt0X, keys::MSt0Src},
                                                  {keys::MSt1On, keys::MSt1X, keys::MSt1Src}};
            for (int k = 0; k < 2; ++k) {
                I64(key[st_keys[k][0]], k < c.st_n ? 1 : 0);
                if (k >= c.st_n)
                    continue;
                I64(key[st_keys[k][1]], c.st_x[k]);
                Text(key[st_keys[k][2]], c.st_src[k]);
            }
        }
        PublishMap(ready);
        // the strip's layout gate (gen_manifest card(): hide_bind dq3.strip kept at N): 0 while the full map
        // covers the strip
        if (strip_n > 0 && !MapInt("dq3.m.full"))
            I64("dq3.strip", strip_n);
    }

    /// Cards on the top strip: the live active party (Line-Up order, C+0xC0 == 1 by C+0xC4), 1..4, else 0.
    int StripCount(bool ready) const {
        if (!ready)
            return 0;
        return static_cast<int>(std::min<std::size_t>({shown.members.size(), cards.size(), std::size_t{4}}));
    }

    /// The system screens (dq3_system.h), gate ints only while on: wrong patch when the code pins refuse
    /// this game (dq3.sys.wrong with the game art, dq3.sys.flat when the art cannot be decoded: the manifest's
    /// host-font page); otherwise, with no party page to show, loading while the game is between worlds or
    /// building the field world (FieldTop without a party) or the title has closed its menus for a confirmed
    /// save (ReadTitleLoad), else start (title, boot). A map transition
    /// with a party keeps the party page (held), so neither shows there.
    int SystemScreen(bool ready) const {
        if (refused)
            return 3;
        if (ready)
            return 0;
        return world_swap || title_loading || cur_scene.world == "FieldTop" ? 2 : 1;
    }
    void PublishSystem(bool ready) {
        const int s = SystemScreen(ready);
        if (s != logged_sys) {
            static constexpr const char* Names[] = {"none", "start", "loading", "wrong patch"};
            Log(EDEN_DSMOD_LOG_INFO, (std::string{"DQ3 DSMod: system screen "} + Names[s] +
                                      (s == 3 ? std::string{" ("} + resolve_error + ")" : std::string{}))
                                         .c_str());
            logged_sys = s;
        }
        if (s == 1)
            I64("dq3.sys.start", 1);
        else if (s == 2)
            I64("dq3.sys.loading", 1);
        else if (s == 3)
            I64(!refused_flat_test && dq3::CachedFontAtlas() ? "dq3.sys.wrong" : "dq3.sys.flat", 1);
    }

    /// Field tabs (command window), the drive gates and the Map page.
    void PublishMap(bool ready) {
        // gate ints only while non-zero: a missing gate is closed (gen_manifest.py check_zero_gates)
        I64("dq3.heal.ready", ready && heal_ready && !heal_box.Busy() && !party.active);
        if (!heal_msg.empty() && Clock::now() < heal_msg_until) {
            I64("dq3.heal.msg.on", 1);
            Text("dq3.heal.msg", heal_msg);
        }
        // top-card drag reorder: any field tab while the cards are shown and the game is free
        const bool reorder = ready && !held && !cur_scene.loading && !heal_box.Busy() && !party.active &&
                             party_menu.mode == dq3::FieldMenuMode::Field && shown.members.size() > 1 && !full_map;
        I64("dq3.party.reorder", reorder);
        if (party.pending >= 0)
            I64("dq3.input.pending", 1);
        for (int k = 0; k < 4; ++k) {
            const bool on = ready && k < static_cast<int>(shown.members.size());
            if (on)
                I64(reorder ? keys::PartyDrag[k] : keys::NoDrag[k], 1);
        }
        for (int k = 0; k < 6; ++k)
            for (int p = 0; p < 2; ++p)
                if (party.pending == k && party.target[k] == bool(p))
                    I64(keys::PartyPress[k][p], 1);
        const auto font = dq3::CachedFontAtlas();
        const auto data = dq3::CachedMapData();
        if (ready && tab == 1)
            PublishPartyTab();
        if (ready && tab == 2)
            PublishBagTab();
        if (ready && !held && !cur_scene.loading && tab == 3 && data)
            PublishJournalTab(*data);
        I64("dq3.m.view.zoom", 1);
        I64("dq3.m.view.center", 0);
        I64("dq3.m.view.reset", map_reset);
        I64("dq3.tab", tab);
        I64("dq3.tab.party", ready && tab == 1 ? 1 : 0);
        I64("dq3.tab.bag", ready && tab == 2 ? 1 : 0);
        I64("dq3.tab.journal", ready && !held && !cur_scene.loading && tab == 3 ? 1 : 0);
        if (tabs_dirty || tabs_font != font.get()) {
            BuildTabs(font.get());
            tabs_font = font.get();
            tabs_dirty = false;
        }
        Text("dq3.ftabs", tabs_key);
        for (int k = 0; k < dq3::FieldTabs; ++k) {
            I64(keys::FtabX[k], tab_label_x[k]);
            Text(keys::FtabT[k], tab_label[k]);
        }
        const bool visible = ready && tab == 0;
        if (map_dirty || map_data != data.get() || map_font != font.get() || visible != map_visible) {
            const auto tv = Clock::now();
            dq3::MapViewInput in;
            in.font = font.get();
            in.data = data.get();
            // the no-map card / place card: the native objective (0xc03c78 route, the Journal's reader) and the
            // hero's sprite (the Hero by vocation, else the first member)
            std::u32string objective;
            if (const auto info = dq3::CachedInfoData(); info && data && jstate)
                if (const auto o = dq3::ObjectiveFor(*info, *data, jstate->guide, jstate->progress))
                    objective = o->main;
            in.objective = objective.empty() ? nullptr : &objective;
            for (std::size_t k = 0; k < shown.members.size() && k < cards.size(); ++k)
                if (cards[k].looks_index && (shown.members[k].vocation == 1 || in.hero_sprite == "-")) {
                    in.hero_sprite = SpriteToken(cards[k]);
                    if (shown.members[k].vocation == 1)
                        break;
                }
            in.roots = mroots.ok ? &mroots : nullptr;
            in.loc = loc_ok ? &loc : nullptr;
            in.visited = visited_ok ? &visited : nullptr;
            const bool under = data && loc_ok && data->Row(loc.map_id) && data->Row(loc.map_id)->underground;
            in.world = &lcache.field[under ? 1 : 0];
            in.chests = chests;
            in.world_icons = &world_icons;
            in.browse = browse;
            in.zoom = map_zoom;
            if (data && loc_ok) {
                const auto* row = data->Row(loc.map_id);
                if (row) {
                    const auto a = lcache.world_anchors.find(row->field_symbol);
                    if (a != lcache.world_anchors.end()) in.area_anchor = a->second;
                    if (!in.area_anchor && !row->prefix.empty()) {
                        for (const auto& [id, dest] : data->destinations) {
                            const auto* area = data->Row(dest.floor);
                            if (!area || area->prefix != row->prefix || area->underground != row->underground ||
                                !dq3::WorldVariantVisible(id, loc)) continue;
                            const auto anchor = lcache.world_anchors.find(dest.tag);
                            if (anchor != lcache.world_anchors.end()) { in.area_anchor = anchor->second; break; }
                        }
                    }
                }
            }
            in.full = full_map && visible;
            in.visible = visible;
            mview = dq3::BuildMapView(in);
            browse = mview.browse;
            map_dirty = false;
            map_data = data.get();
            map_font = font.get();
            map_visible = visible;
            map_view_us = Us(tv, Clock::now());
            // no map to show (unknown row, no data): the full map closes rather than staying armed
            if (full_map && !MapInt("dq3.m.on")) {
                full_map = false;
                ++map_reset;
                map_dirty = true;
            }
        }
        // the snapshot is rebuilt every sample: a missing gate is closed and a missing offset is 0,
        // so only the non-zero values are published (keeps the field sample cheap)
        for (const auto& [k, v] : mview.ints)
            if (v)
                I64(k.c_str(), v);
        if (visible)
            for (const auto& [k, v] : mview.texts)
                if (k != "dq3.m.img")
                    Text(k.c_str(), v);
        PublishMapImage(visible);
    }

    /// dq3.m.img (shown) and dq3.m.img.next (the hidden prefetch widget): a new map key shows only once the host
    /// holds its picture (dq3_map_swap.h), so a marker / camera move never blanks the map box.
    void PublishMapImage(bool visible) {
        std::string want;
        if (visible)
            if (const auto it = mview.texts.find("dq3.m.img"); it != mview.texts.end())
                want = it->second;
        const std::string before_shown = map_swap.shown(), before_req = map_swap.request();
        const auto out = map_swap.Step(want, sample_index, [this](const std::string& key) { return map_ready.Get(key); });
        if (!out.shown.empty())
            Text("dq3.m.img", out.shown);
        if (!out.request.empty())
            Text("dq3.m.img.next", out.request);
        if (map_trace && (out.shown != before_shown || out.request != before_req || want != map_trace_want)) {
            const auto shortk = [](const std::string& k) {
                return k.empty() ? std::string{"-"} : k.substr(std::min<std::size_t>(k.size(), KeyPrefix.size()));
            };
            Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: map img s" + std::to_string(sample_index) + " want " + shortk(want) +
                                      " | next " + shortk(out.request) + " | shown " + shortk(out.shown))
                                         .c_str());
            map_trace_want = want;
        }
    }

    void BuildTabs(const dq3::FontAtlas* font) {
        // render.py command_tabs(labels, sel, (X0, FTAB_Y, CW, TAB_H), icons, px=34) with four tabs (option B:
        // Map . Party . Bag . Journal): [chevron 34 + 8 on the selected tab] icon + 10, Bold 34 label, the group
        // centred in its slot
        constexpr int N = dq3::FieldTabs;
        static constexpr const char* labels[N] = {"Map", "Party", "Bag", "Journal"};
        static constexpr int icon_w[N] = {38, 46, 46, 44}; // tab_icon(t, 38) widths (fit 1, 1.2, 1.2, 1.15)
        constexpr int W = dq3::FTabW, H = dq3::FTabH, X0 = dq3::FTabX0;
        constexpr int cs = 34;
        const double slot = W / static_cast<double>(N);
        int icon_x[N]{}, chev = 0;
        for (int k = 0; k < N; ++k) {
            const std::string st = dq3::Styled(std::string_view{labels[k]}, dq3::FontStyle::Bold34);
            const int lw = font ? font->Measure(st) : 0;
            const bool on = k == tab;
            const double tot = lw + icon_w[k] + 10 + (on ? cs + 8 : 0);
            double gx = k * slot + (slot - tot) / 2;
            if (on) {
                chev = static_cast<int>(gx);
                gx += cs + 8;
            }
            icon_x[k] = static_cast<int>(gx);
            tab_label_x[k] = X0 + static_cast<int>(gx + icon_w[k] + 10);
            tab_label[k] = "{c:" + std::string{on ? "#FFECD292" : "#FFD6CEB8"} + "}" + st + "{/c}";
        }
        tabs_key = "module:dq3:ftabs/" + std::to_string(W) + "x" + std::to_string(H) + "/" + std::to_string(tab) + "/";
        for (int k = 0; k < N; ++k)
            tabs_key += (k ? "," : "") + std::to_string(icon_x[k]);
        tabs_key += "/" + std::to_string(chev);
    }

    void ReadMapNow() {
        if (!mroots.ok || battle_on || held || published_state != dq3::PartyState::Ok || shown.members.empty() ||
            !cur_scene.InAdventure() || cur_scene.loading || !cur_scene.llm || !cur_scene.pawn_ptr)
            return;
        dq3::LocationSnapshot l;
        std::string why_loc;
        const dq3::GuestRead read = Reader();
        if (!dq3::ReadLocation(read, roots, cur_scene, cur_scene.pawn_ptr, lcache, l, &why_loc)) {
            if (why_loc != logged_loc_why) {
                Log(EDEN_DSMOD_LOG_WARNING, ("DQ3 DSMod: location not read: " + why_loc).c_str());
                logged_loc_why = why_loc;
            }
            return;
        }
        logged_loc_why.clear();
        if (l.pawn_class == "BP_BattlePawn_C")
            return; // battle: the battle page takes over, keep the last map
        const bool changed_map = !loc_ok || l.map_id != loc.map_id;
        if (changed_map) {
            browse = 0;
            ++map_reset;
            world_icons_at = 0;
            world_icons.clear();
            chests.reset();
            chest_scan.Clear();
            chests_at = 0;
            visited_at = 0;
            char buf[200];
            std::snprintf(buf, sizeof(buf), "DQ3 DSMod: map %s (%s) at %.0f, %.0f, hour %d, gold %u",
                          l.map_id.c_str(), l.pawn_class.c_str(), l.x, l.y, l.hour, l.gold);
            Log(EDEN_DSMOD_LOG_INFO, buf);
        }
        if (!loc_ok || !(l == loc))
            map_dirty = true;
        loc = std::move(l);
        loc_ok = true;
        if (tab != 0)
            return; // the Party / Bag / Journal tabs read their own state (ReadInfoNow)
        if (!visited_at || sample_index >= visited_at + 300) {
            visited_at = sample_index;
            if (auto v = dq3::ReadVisited(read, roots)) {
                if (!visited_ok || *v != visited) {
                    visited = std::move(*v);
                    map_dirty = true;
                    Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: visited maps " + std::to_string(visited.size())).c_str());
                }
                visited_ok = true;
            }
        }
        const auto data = dq3::CachedMapData();
        const auto* row = data ? data->Row(loc.map_id) : nullptr;
        if (data &&
            (!world_icons_at || sample_index >= world_icons_at + 300)) {
            world_icons_at = sample_index;
            if (const auto icons = dq3::ReadWorldIcons(read, roots, *data, loc, lcache)) {
                if (*icons != world_icons) {
                    world_icons = *icons;
                    map_dirty = true;
                    Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: acquired world icons " +
                        std::to_string(world_icons.size())).c_str());
                }
            } else {
                // An unreadable registry must not retain icons from a different map/load.
                if (!world_icons.empty()) { world_icons.clear(); map_dirty = true; }
            }
        }
        if (row && row->floor_group != 0 && (!chests_at || sample_index >= chests_at + 120)) {
            chests_at = sample_index;
            const auto c = dq3::ReadChests(read, roots, cur_scene.llm, loc.map_id, chest_scan);
            if (c && c != chests) {
                chests = c;
                map_dirty = true;
                char buf[120];
                std::snprintf(buf, sizeof(buf), "DQ3 DSMod: %s chests %d, opened %d", loc.map_id.c_str(), c->first,
                              c->second);
                Log(EDEN_DSMOD_LOG_INFO, buf);
            }
        }
    }

public:
    /// Companion-side selection (taps). arg 0..9 = enemy cell, 100 + k = party card k.
    bool Action(std::string_view action, std::int64_t arg) {
        if (action.size()==10 && action.starts_with("partydrop") && action.back()>='0' && action.back()<='3') {
            if (arg < 0 || arg > 3 || !FieldInputAllowed() || full_map || heal_box.Busy() || party.active ||
                shown.members.size() < 2)
                return false;
            const auto menu = ReadMenu();
            const auto fields=ReadPartyFields();
            if (!menu || menu->mode!=dq3::FieldMenuMode::Field || !fields || fields->size()!=shown.members.size()) return false;
            for (std::size_t k=0;k<fields->size();++k) if ((*fields)[k]!=shown.members[k].field) return false;
            party.drag=dq3::PartyDragOrder::Start(static_cast<int>(arg),action.back()-'0',*fields);
            if (!party.drag) return false;
            party.goal=party.drag->Goal();
            party.active=true;
            party.waiting=party.terminal=false;
            party.pending=-1;
            party.started=party.pressed=Clock::now();
            party.expected=*menu;
            Log(EDEN_DSMOD_LOG_INFO,"DQ3 DSMod: drag reorder started (native buttons, no guest writes)");
            return true;
        }
        if (action == "heal") {
            // the same gate as the button: a free field with the native menu closed, the helper alive
            const auto now = Clock::now();
            if (arg < 0 || arg > 1 || !HealGateNow(true, now).Open())
                return false;
            const auto menu = ReadMenu();
            if (!HealGateNow(menu && menu->mode == dq3::FieldMenuMode::Field, now).Open() ||
                !heal_box.Request(static_cast<int>(arg), now))
                return false;
            heal_name = shown.members.size() == 1 ? shown.members[0].name : std::u32string{};
            heal_msg.clear();
            Log(EDEN_DSMOD_LOG_INFO, arg ? "DQ3 DSMod: Handy Heal All requested (game routine, no input)" :
                                           "DQ3 DSMod: Heal All requested (game routine, no input)");
            return true;
        }
        if (action == "bfollow") {
            if (!battle_on || (arg != 0 && arg != 1)) return false;
            battle_follow = arg != 0;
            follow_menu.reset();
            if (battle_follow)
                ReadFollowMenu();
            sel = dq3::BattleSel::None;
            sel_index = -1;
            detail_at = 0;
            view_dirty = true;
            return true;
        }
        if (action == "ftab") {
            if (arg < 0 || arg >= dq3::FieldTabs || battle_on)
                return false;
            tab = static_cast<int>(arg);
            journal_at=record_at=member_at=bags_at=0;
            party_dirty=bag_dirty=journal_dirty=true;
            if (full_map) { // switching tabs leaves the full map
                full_map = false;
                ++map_reset;
            }
            map_dirty = true;
            tabs_dirty = true;
            return true;
        }
        if (action == "partypick") {
            // tapping a top card shows that member on the Party tab; the Bag tab is shared, so a tap there
            // changes nothing (the cards stay draggable)
            if (battle_on || tab==2 || arg<0 || arg>=static_cast<s64>(shown.members.size()) ||
                published_state!=dq3::PartyState::Ok)
                return false;
            const u64 f=shown.members[static_cast<std::size_t>(arg)].field;
            if (f!=party_field || tab!=1) { party_field=f; party_sel=0; party_slot=-1; member.reset(); member_at=0; }
            if (tab!=1) { tab=1; map_dirty=tabs_dirty=true; }
            ++party_key;
            party_dirty=true;
            return true;
        }
        if (action == "partytab") { // Carried / Spells
            if (battle_on || tab!=1 || arg<0 || arg>1) return false;
            if (party_sub!=arg) { party_sub=static_cast<int>(arg); party_sel=0; }
            party_slot=-1;
            ++party_key;
            party_dirty=true;
            return true;
        }
        if (action == "partyitem") {
            if (battle_on || tab!=1 || arg<0 || arg>=dq3::MaxRows) return false;
            party_sel=static_cast<int>(arg);
            party_slot=-1;
            party_dirty=true;
            return true;
        }
        if (action == "partyslot") { // an equipped slot: its item in the detail area (Carried)
            if (battle_on || tab!=1 || arg<0 || arg>5) return false;
            party_slot=static_cast<int>(arg);
            if (party_sub!=0) { party_sub=0; party_sel=0; ++party_key; }
            party_dirty=true;
            return true;
        }
        if (action == "bagpage") {
            if (battle_on || tab!=2 || arg<0 || arg>2) return false;
            if (bag_page!=arg) { bag_page=static_cast<int>(arg); bag_sel=0; ++bag_key; }
            bags_at=0;
            bag_dirty=true;
            return true;
        }
        if (action == "bagitem") {
            if (battle_on || tab!=2 || arg<0 || arg>=dq3::MaxRows) return false;
            bag_sel=static_cast<int>(arg);
            bag_dirty=true;
            return true;
        }
        if (action == "journalmon") {
            if (battle_on || tab!=3 || arg<0 || arg>=1024) return false;
            mon_sel=static_cast<int>(arg);
            journal_dirty=true;
            return true;
        }
        if (action == "floor") {
            if ((arg != 1 && arg != -1) || battle_on || tab != 0)
                return false;
            browse += static_cast<int>(arg);
            ++map_reset;
            map_dirty = true;
            return true;
        }
        if (action == "fullmap") {
            if (full_map) { // closing is always allowed: the full map never stays stuck
                full_map = false;
                ++map_reset;
                map_dirty = true;
                return true;
            }
            if (battle_on || held || cur_scene.loading || tab != 0 || !map_visible || !MapInt("dq3.m.on"))
                return false;
            full_map = true;
            ++map_reset;
            map_dirty = true;
            return true;
        }
        if (action == "mapzoom") {
            if ((arg != -1 && arg != 1) || battle_on || held || cur_scene.loading || tab != 0 || !map_visible ||
                !MapInt(arg < 0 ? "dq3.m.zoom.in" : "dq3.m.zoom.out"))
                return false;
            ++map_reset;
            map_zoom = std::clamp(map_zoom + static_cast<int>(arg), 0, 2);
            if (map_zoom == 1 && !MapInt("dq3.m.region")) map_zoom = arg > 0 ? 2 : 0;
            map_dirty = true;
            return true;
        }
        if (action != "bsel" || !battle_on)
            return false;
        // tapping a card takes manual control: Tactical with that card selected (a tap during Follow
        // never clears, even on the card Follow was showing)
        const bool was_follow = battle_follow;
        battle_follow = false;
        dq3::BattleSel s = dq3::BattleSel::None;
        int index = -1;
        if (arg >= 0 && arg < 10) {
            s = dq3::BattleSel::Enemy;
            index = static_cast<int>(arg);
        } else if (arg >= 100 && arg < 104) {
            s = dq3::BattleSel::Party;
            index = static_cast<int>(arg - 100);
        } else {
            return false;
        }
        if (!was_follow && s == sel && index == sel_index) {
            sel = dq3::BattleSel::None; // second tap: close the detail
            sel_index = -1;
        } else {
            sel = s;
            sel_index = index;
        }
        detail_at = 0; // read the selected member's spells now
        view_dirty = true;
        return true;
    }

private:
    // ------------------------------------------------------------------ Party and Journal tabs
    /// The member the Party tab shows (by field record, so a reorder keeps it).
    /// Displayed max HP / MP = GetMaxHP / GetMaxMP full path (dq3_info.h EquipMaxParam): the card values
    /// get the equipped MAXHP / MAXMP item term. Only when the loaded GOP_Item has such rows (none in
    /// 1.1.0.0, where the term is provably 0 and nothing extra is read); then each member's bag is read
    /// at most every 2 s and the last sums are kept per F between reads.
    void AddEquipMax(const dq3::GuestRead& read, dq3::PartySnapshot& snap) {
        const auto info = dq3::CachedInfoData();
        if (info.get() != equip_max_info) {
            equip_max_info = info.get();
            equip_max_on = info && dq3::HasMaxHpMpItems(*info);
            equip_max.clear();
            equip_max_at = 0;
        }
        if (!equip_max_on || !iroots.ok)
            return;
        if (!equip_max_at || sample_index >= equip_max_at + 120) {
            equip_max_at = sample_index;
            std::map<u64, std::pair<s32, s32>> sums;
            for (const auto& m : snap.members)
                if (const auto d = dq3::ReadMemberDetail(read, roots, m.field))
                    sums[m.field] = {dq3::EquipMaxParam(*info, d->stats.items, 7),
                                     dq3::EquipMaxParam(*info, d->stats.items, 8)};
                else if (const auto old = equip_max.find(m.field); old != equip_max.end())
                    sums[m.field] = old->second;
            equip_max = std::move(sums);
        }
        for (auto& m : snap.members)
            if (const auto it = equip_max.find(m.field); it != equip_max.end()) {
                // hp_base / mp_base are unchecked guest words (dq3_reader.cpp ReadPartyOnce)
                m.hp_max = dq3::DisplayedMax(dq3::AddClamped(m.hp_base, it->second.first), m.hp_bonus);
                m.mp_max = dq3::DisplayedMax(dq3::AddClamped(m.mp_base, it->second.second), m.mp_bonus);
            }
    }

    int PartySel() {
        for (std::size_t k = 0; k < shown.members.size(); ++k)
            if (shown.members[k].field == party_field)
                return static_cast<int>(k);
        if (shown.members.empty())
            return -1;
        party_field = shown.members[0].field;
        return 0;
    }

    void ReadInfoNow() {
        if (!iroots.ok || battle_on || held || published_state != dq3::PartyState::Ok || shown.members.empty() ||
            !cur_scene.InAdventure() || cur_scene.loading || !dq3::CachedInfoData())
            return;
        const dq3::GuestRead read = Reader();
        if (tab == 1) {
            const int sel = PartySel();
            const u64 f = sel >= 0 ? shown.members[static_cast<std::size_t>(sel)].field : 0;
            if (f && (!member || member->field != f || !member_at || sample_index >= member_at + 30)) {
                member_at = sample_index;
                auto d = dq3::ReadMemberDetail(read, roots, f);
                if (d) {
                    if (!member || !(*d == *member)) {
                        member = std::move(d);
                        party_dirty = true;
                    }
                    member_fail = 0;
                } else if (++member_fail > 5 && member) {
                    member.reset();
                    party_dirty = true;
                    Log(EDEN_DSMOD_LOG_WARNING, "DQ3 DSMod: party member detail unreadable");
                }
            }
        } else if (tab == 2) {
            if (!bags_at || sample_index >= bags_at + 60) {
                // only the shown bag's rows are read; the owners are validated every time
                bags_at = sample_index;
                const auto owners = dq3::ReadBagOwners(read, roots);
                auto items = owners ? dq3::ReadBagItems(read, roots, (*owners)[static_cast<std::size_t>(bag_page)])
                                    : std::nullopt;
                auto& rows = bag_rows[static_cast<std::size_t>(bag_page)];
                if (!items) {
                    if (std::any_of(bag_rows.begin(), bag_rows.end(), [](const auto& b) { return b.has_value(); })) {
                        bag_rows = {};
                        bag_dirty = true;
                    }
                } else if (!rows || !(*items == *rows)) {
                    rows = std::move(items);
                    bag_dirty = true;
                }
            }
        } else if (tab == 3 || (tab == 0 && (MapInt("dq3.mv.c") || MapInt("dq3.mv.p")))) {
            // the Journal, and the Map tab's no-map card / place card (their native objective line)
            if (!journal_at || sample_index >= journal_at + 60) {
                journal_at = sample_index;
                auto s = dq3::ReadJournalState(read, roots);
                if (s && (!jstate || !(*s == *jstate))) {
                    if (!jstate || jstate->guide != s->guide)
                        Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: map guide " + s->guide).c_str());
                    jstate = std::move(s);
                    journal_dirty = map_dirty = true;
                } else if (!s && jstate) {
                    jstate.reset();
                    journal_dirty = map_dirty = true;
                }
            }
            if (record_ok && (!record_at || sample_index >= record_at + 120)) {
                record_at = sample_index;
                if (const auto rec = dq3::ReadMonsterRecord(read, roots)) {
                    std::map<std::string, s32> kills(rec->begin(), rec->end());
                    if (!record || kills != *record) {
                        record = std::move(kills);
                        journal_dirty = true;
                    }
                }
            }
        }
    }

    void PublishPartyTab() {
        const auto info = dq3::CachedInfoData();
        const auto font = dq3::CachedFontAtlas();
        const std::optional<u32> gold = loc_ok ? std::optional<u32>{loc.gold} : std::nullopt;
        // rebuilt on a detail / bag / selection change and when the top cards (job, looks, max HP / MP)
        // or the gold change
        if (party_dirty || pv_info != info.get() || pv_font != font.get() || cards_gen != pv_cards_gen ||
            gold != pv_gold) {
            const auto t0 = Clock::now();
            const auto bdata = dq3::CachedBattleData();
            dq3::PartyViewInput in;
            const int sel = PartySel();
            if (sel >= 0 && static_cast<std::size_t>(sel) < cards.size() && member &&
                member->field == shown.members[static_cast<std::size_t>(sel)].field) {
                const Card& c = cards[static_cast<std::size_t>(sel)];
                in.member = &shown.members[static_cast<std::size_t>(sel)];
                in.job = c.job;
                in.sprite = SpriteToken(c);
                in.detail = &*member;
            }
            in.gold = gold;
            in.sub = party_sub;
            in.sel = party_sel;
            in.slot = party_slot;
            in.key = party_key;
            in.info = info.get();
            in.battle = bdata.get();
            in.font = font.get();
            pv = dq3::BuildPartyView(in);
            party_sel = pv.sel;
            party_slot = pv.slot;
            pv_info = info.get();
            pv_font = font.get();
            pv_cards_gen = cards_gen;
            pv_gold = gold;
            party_view_us = Us(t0, Clock::now());
            party_dirty = false;
        }
        for (const auto& [k, v] : pv.ints)
            I64(k.c_str(), v);
        for (const auto& [k, v] : pv.texts)
            Text(k.c_str(), v);
    }

    void PublishBagTab() {
        const auto info = dq3::CachedInfoData();
        const auto font = dq3::CachedFontAtlas();
        if (bag_dirty || bv_info != info.get() || bv_font != font.get()) {
            const auto t0 = Clock::now();
            dq3::BagViewInput in;
            const auto& rows = bag_rows[static_cast<std::size_t>(bag_page)];
            in.rows = rows ? &*rows : nullptr;
            in.page = bag_page;
            in.sel = bag_sel;
            in.key = bag_key;
            in.info = info.get();
            in.font = font.get();
            bv = dq3::BuildBagView(in);
            bag_sel = bv.sel;
            bv_info = info.get();
            bv_font = font.get();
            bag_view_us = Us(t0, Clock::now());
            bag_dirty = false;
        }
        for (const auto& [k, v] : bv.ints)
            I64(k.c_str(), v);
        for (const auto& [k, v] : bv.texts)
            Text(k.c_str(), v);
    }

    void PublishJournalTab(const dq3::MapData& map) {
        const auto info = dq3::CachedInfoData();
        if (journal_dirty || jv_info != info.get() || jv_map != &map) {
            const auto t0 = Clock::now();
            dq3::JournalViewInput in;
            in.state = jstate ? &*jstate : nullptr;
            in.record = record ? &*record : nullptr;
            in.mon_sel = mon_sel;
            in.info = info.get();
            in.map = &map;
            const auto font = dq3::CachedFontAtlas();
            in.font = font.get();
            jv = dq3::BuildJournalView(in);
            mon_sel = jv.mon_sel;
            jv_info = info.get();
            jv_map = &map;
            journal_view_us = Us(t0, Clock::now());
            journal_dirty = false;
        }
        for (const auto& [k, v] : jv.ints)
            I64(k.c_str(), v);
        for (const auto& [k, v] : jv.texts)
            Text(k.c_str(), v);
    }

    // ------------------------------------------------------------------ native button drives
    std::optional<std::vector<dq3::u64>> ReadPartyFields() {
        dq3::PartySnapshot current;
        if (dq3::ReadParty(Reader(),roots.g_slot,current)!=dq3::PartyState::Ok) return std::nullopt;
        std::vector<dq3::u64> fields;
        for (const auto& member:current.members) fields.push_back(member.field);
        return fields;
    }

    /// The field menu of this sample (read once for both drives).
    struct MenuRead {
        bool cadence{}; ///< a ReadEvery sample: idle drives refresh their state
        std::optional<dq3::FieldMenuState> menu;
    };

    void UpdatePartyDrive(bool allowed, const MenuRead& m) {
        using Mode=dq3::FieldMenuMode;
        if (!allowed) {
            party.Clear(); party_menu={}; return;
        }
        if (!party.active && !m.cadence) return;
        const auto& menu=m.menu;
        party_menu=menu.value_or(dq3::FieldMenuState{});
        if (!party.active) return;
        const auto now=Clock::now();
        const auto since=std::chrono::duration_cast<std::chrono::milliseconds>(now-party.pressed).count();
        const auto total=std::chrono::duration_cast<std::chrono::milliseconds>(now-party.started).count();
        const auto match=[](const auto& a,const auto& b) {
            return a.mode==b.mode && a.cursor==b.cursor && a.selected==b.selected;
        };
        const auto abort=[&] { party.Clear(); Log(EDEN_DSMOD_LOG_WARNING,"DQ3 DSMod: party input stopped (unexpected native menu/cursor or timeout)"); };
        // Let native menu animations settle after acknowledgement before sending another button.
        // Native menus remove their controllers over successive frames when closing. Hold the
        // existing token briefly; no next input is sent until the expected complete menu arrives.
        if (party.waiting && (!menu || menu->mode==Mode::Other) && since<2500 && total<25000) return;
        if (!menu || menu->mode==Mode::Other || total>25000 || since>2500) { abort();return; }
        if (party.waiting) {
            if (!match(*menu,party.expected)) {
                if (!match(*menu,party.previous)) abort();
                return;
            }
            if (since<500) return;
            party.pending=-1;party.waiting=false;
            if (party.terminal) {
                const auto fields=ReadPartyFields();
                if (!party.drag || !fields || !party.drag->FinishStage(*menu,*fields)) {abort();return;}
                if (party.drag->stage==4) {
                    party.Clear();Log(EDEN_DSMOD_LOG_INFO,"DQ3 DSMod: drag reorder completed (native roster verified, Field restored)");return;
                }
                party.goal=party.drag->Goal();party.terminal=false;party.pressed=now;
                return;
            }
        } else if (since<500) return;
        const auto fields=ReadPartyFields();
        if (!party.drag || !fields || !party.drag->MatchesRoster(*fields)) {abort();return;}
        const auto step=dq3::PlanPartyOrder(*menu,party.goal,static_cast<int>(fields->size()));
        if (!step) {abort();return;}
        party.previous=*menu;party.expected=step->expected;party.terminal=step->terminal;
        party.pending=step->button;
        NextTarget(keys::PartyCounter[step->button], party.target[static_cast<std::size_t>(step->button)]);
        party.waiting=true;party.pressed=now;
    }

    /// Heal All / Handy Heal All (dq3_heal.h): the helper's heartbeat and result, then the button gate.
    void UpdateHeal(bool allowed, const MenuRead& m) {
        const auto now = Clock::now();
        heal_box.Beat(now);
        if (const auto done = heal_box.Poll(now))
            FinishHeal(*done, now);
        if (!allowed)
            heal_ready = false;
        else if (m.cadence)
            heal_ready = HealGateNow(m.menu && m.menu->mode == dq3::FieldMenuMode::Field, now).Open();
    }

    dq3::HealGate HealGateNow(bool menu_closed, Clock::time_point now) const {
        // FieldInputAllowed holds !battle_on too; the gate names it on its own for the tests
        return {battle_on, FieldInputAllowed(), menu_closed, party.active, heal_box.Alive(now), heal_box.Busy()};
    }

    /// The Misc menu's own result line for a finished request (shown on the companion for HealMsgFor).
    void FinishHeal(const dq3::HealMailbox::Finished& f, Clock::time_point now) {
        const auto outcome = f.timed_out ? std::nullopt : dq3::DecodeHealResult(f.raw);
        char line[160];
        std::snprintf(line, sizeof(line), "DQ3 DSMod: %s %s (helper result 0x%llx)",
                      f.mode ? "Handy Heal All" : "Heal All",
                      f.timed_out ? "not served (the game loop stopped), cancelled"
                      : !outcome  ? "returned an unknown result"
                                  : "done",
                      static_cast<unsigned long long>(f.raw));
        Log(outcome ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_WARNING, line);
        heal_msg.clear();
        if (!outcome)
            return;
        const char* id = dq3::HealMessageId(*outcome, f.mode, !heal_name.empty());
        const auto info = dq3::CachedInfoData();
        const std::u32string* raw = id && info ? info->Text(id) : nullptr;
        if (!raw)
            return;
        heal_msg = dq3::StyledWrap(dq3::CleanGameText(dq3::NameHealMessage(*raw, heal_name)), dq3::FontStyle::Semi28);
        heal_msg_until = now + HealMsgFor;
    }

    // ------------------------------------------------------------------ battle page
    void ReadBattleNow() {
        if (!battle_resolved) {
            battle_on = false;
            return;
        }
        dq3::BattleSnapshot snap;
        std::string reason;
        const dq3::BattleRead r =
            dq3::ReadBattle(Reader(), roots, broots, snap, &bcache, true, &reason);
        if (r == dq3::BattleRead::Ok) {
            if (!battle_on) {
                sel = dq3::BattleSel::None;
                sel_index = -1;
                follow_menu.reset();
                battle_menu_classes.clear();
                // battle art can push old map pictures out of the host cache: forget which are composed
                map_ready.Clear();
                map_swap.Reset();
                turn_actor = snap.actor;
                for (auto& o : icon_order)
                    o.clear();
                ReadKnown();
                std::string enc = FNameText(snap.encounter).value_or("?");
                Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: battle start " + enc + ", state " +
                                          dq3::BattleStateName(snap.state))
                                             .c_str());
            }
            for (std::size_t k = 0; k < icon_order.size(); ++k) {
                const dq3::BattleUnit* pu = nullptr;
                for (const auto& u : snap.party)
                    if (u.slot == 20 + k)
                        pu = &u;
                if (!pu) {
                    icon_order[k].clear();
                    continue;
                }
                const auto before = icon_order[k];
                dq3::MergeIconOrder(icon_order[k], dq3::BattleIconCells(*pu));
                if (before != icon_order[k])
                    view_dirty = true;
            }
            if (!(snap == battle) || !battle_on) {
                if (all_outputs)
                    LogBattle(snap);
                view_dirty = true;
            }
            battle = std::move(snap);
            battle_on = true;
            battle_invalid = 0;
            logged_battle_why.clear();
            if (battle.state < 8 || battle.state > 14)
                turn_actor = battle.actor;
            if (const bool av = dq3::ActorCurrent(battle, turn_actor); av != actor_valid) {
                actor_valid = av;
                view_dirty = true;
            }
            if (battle_follow)
                ReadFollowMenu();
        } else if (r == dq3::BattleRead::NotInBattle) {
            if (battle_on)
                Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: battle over (" + reason + ")").c_str());
            battle_on = false;
            battle = {};
            follow_menu.reset();
        } else {
            if (reason != logged_battle_why) {
                Log(EDEN_DSMOD_LOG_WARNING, ("DQ3 DSMod: battle read rejected: " + reason).c_str());
                logged_battle_why = reason;
            }
            if (battle_on && ++battle_invalid > HoldInvalid / ReadEvery * 2) {
                battle_on = false;
                battle = {};
            }
        }
        if (battle_on && sel == dq3::BattleSel::Party)
            ReadSelectedMember();
    }

    /// Follow: the active native battle list, read twice (a changed second reading is torn and keeps
    /// the last state); MenuFocus::None (no list active) keeps the last settled state too.
    void ReadFollowMenu() {
        if (!battle_menu_ok || !battle.CommandInput())
            return;
        const auto a = dq3::ReadBattleMenu(Reader(), roots, battle, &battle_menu_classes);
        const auto b = a ? dq3::ReadBattleMenu(Reader(), roots, battle, &battle_menu_classes) : std::nullopt;
        if (!a || !b || !(*a == *b) || a->focus == dq3::MenuFocus::None)
            return;
        if (!follow_menu || !(*follow_menu == *a)) {
            follow_menu = *a;
            view_dirty = true;
            static constexpr const char* Names[] = {"none", "root", "member", "enemy target", "ally target"};
            char buf[160];
            std::snprintf(buf, sizeof(buf), "DQ3 DSMod: follow menu %s member %d group %d unit %d pending %u/%u",
                          Names[static_cast<int>(a->focus)], a->member, a->target_group, a->target_unit,
                          a->pending.valid, a->pending.command);
            Log(EDEN_DSMOD_LOG_INFO, buf);
        }
    }

    /// The monster record keys with at least one kill (read once per battle).
    void ReadKnown() {
        known.clear();
        known_ok = false;
        if (!record_ok)
            return;
        const auto rec = dq3::ReadMonsterRecord(Reader(), roots);
        if (!rec) {
            Log(EDEN_DSMOD_LOG_WARNING, "DQ3 DSMod: monster record unreadable (affinities hidden)");
            return;
        }
        for (const auto& [key, kills] : *rec)
            if (kills > 0)
                known.insert(key);
        known_ok = true;
        Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: monster record: " + std::to_string(known.size()) +
                                  " species defeated")
                                     .c_str());
    }

    /// Strength and the battle Spells list of the selected member, at most every 2 s (the list
    /// changes only on level-up). Strength is GetStr 0x89e2e0: both its paths read the base +0x38 and
    /// the bonus +0x70 from the stat record s = F, or [F+0x1A8] while its controller F+0x1B0 holds a
    /// reference (dq3::StatRecord, the same record the Party tab uses); the override is set only by
    /// battle-unit code (0x721e18 / 0x721ea8 / 0x721ecc -> 0x8a1880). STR has no equipment term.
    void ReadSelectedMember() {
        if (sel_index < 0 || static_cast<std::size_t>(sel_index) >= shown.members.size())
            return;
        const u64 f = shown.members[static_cast<std::size_t>(sel_index)].field;
        if (f == detail_field && detail_at && sample_index < detail_at + 120)
            return;
        detail_at = sample_index;
        Detail d;
        d.field = f;
        const dq3::GuestRead read = Reader();
        if (const auto str = dq3::ReadStrength(read, f)) {
            d.strength = *str;
            d.strength_ok = true;
        }
        // the skill list (dq3_info.h ReadSkillEntries); unreadable leaves the list empty
        if (f)
            if (const auto raw = dq3::ReadSkillEntries(read, f))
                for (const dq3::RawSkill& e : *raw) {
                    Detail::Entry en;
                    std::memcpy(en.name, e.name, sizeof(en.name));
                    en.learn = e.learn;
                    en.timing = e.timing;
                    d.entries.push_back(en);
                }
        // resolve the entry FNames (cached)
        for (auto& en : d.entries)
            if (const auto t = FNameText(en.name))
                en.id = *t;
        if (!(d == detail)) {
            detail = std::move(d);
            detail_field = f;
            view_dirty = true;
        }
    }

    std::vector<dq3::BattleMember> BattleMembers() {
        std::vector<dq3::BattleMember> out;
        const auto data = dq3::CachedBattleData();
        for (std::size_t k = 0; k < shown.members.size() && k < cards.size(); ++k) {
            const auto& m = shown.members[k];
            dq3::BattleMember bm;
            bm.name = m.name;
            bm.job = cards[k].job;
            bm.level = m.level;
            bm.field = m.field;
            bm.sprite = cards[k].sprite_index;
            bm.coffin = cards[k].coffin_index;
            for (const auto& u : battle.party)
                if (u.field == m.field && u.slot >= 20 && u.slot < 24)
                    bm.icons = icon_order[u.slot - 20];
            if (sel == dq3::BattleSel::Party && static_cast<int>(k) == sel_index && detail.field == m.field) {
                bm.strength = detail.strength;
                bm.strength_ok = detail.strength_ok;
                // battle mp for the state rule (0xae4844)
                s32 mp = 0;
                for (const auto& u : battle.party)
                    if (u.field == m.field)
                        mp = u.mp;
                for (const auto& en : detail.entries) {
                    if (!data || en.timing == 3)
                        continue;
                    const auto it = data->magic.find(en.id);
                    if (it == data->magic.end() || !it->second.magic)
                        continue;
                    dq3::BattleMember::Spell sp;
                    if (en.learn == 1) {
                        sp.name = U"???";
                        sp.state = 0;
                    } else if (en.learn == 2 || en.learn == 3) {
                        const auto* noun = data->Noun(it->second.name_text);
                        if (!noun)
                            continue;
                        sp.name = *noun;
                        sp.mp = it->second.consume_mp;
                        sp.state = sp.mp == 0 ? 2 : (mp < 1 || mp < sp.mp) ? 1 : 2;
                    } else {
                        continue;
                    }
                    bm.spells.push_back(std::move(sp));
                }
            }
            out.push_back(std::move(bm));
        }
        return out;
    }

    void PublishBattle() {
        const auto data = dq3::CachedBattleData();
        const auto text = dq3::CachedGameText();
        const auto font = dq3::CachedFontAtlas();
        const int page = static_cast<int>(sample_index / 120); // 2.0 s icon paging
        if (cards_dirty || cards_text != text.get() || cards_font != font.get())
            BuildCards();
        if (view_dirty || page != view_page || view_data != data.get() || view_font != font.get() ||
            (view_retry_at && sample_index >= view_retry_at)) {
            const auto tv = Clock::now();
            dq3::BattleViewInput in;
            in.battle = &battle;
            in.members = BattleMembers();
            in.data = data.get();
            in.font = font.get();
            in.colours = text ? &text->colours : nullptr;
            in.fname = [this](const std::uint32_t n[2]) { return FNameText(n); };
            in.known = known_ok ? &known : nullptr;
            in.sel = sel;
            in.sel_index = sel_index;
            in.icon_page = page;
            in.follow = battle_follow;
            if (battle_follow)
                in.menu = follow_menu;
            in.actor_valid = actor_valid;
            view = dq3::BuildBattleView(in);
            sel = view.sel;
            sel_index = view.sel_index;
            view_dirty = false;
            view_page = page;
            view_data = data.get();
            view_font = font.get();
            view_retry_at = view.retry ? sample_index + 60 : 0;
            view_us = Us(tv, Clock::now());
        }
        for (const auto& [k, v] : view.ints)
            I64(k.c_str(), v);
        for (const auto& [k, v] : view.texts)
            Text(k.c_str(), v);
        if (all_outputs)
            F64("dq3.b.view_us", view_us);
    }

    void LogBattle(const dq3::BattleSnapshot& s) {
        std::string line = std::string{"DQ3 DSMod: battle "} + dq3::BattleStateName(s.state) + "/" +
                           dq3::BattleStateName(s.prev) + ":";
        for (const auto& e : s.enemies) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), " [%u] %s g%d i%d HP %d/%d Lv%d;", e.slot,
                          FNameText(e.master).value_or("?").c_str(), e.group, e.index, e.hp,
                          e.hp_max, e.level);
            line += buf;
        }
        for (const auto& p : s.party) {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          " [%u] HP %d/%d MP %d/%d Lv%d atk %d def %d agi %d wis %d luck %d rec+0x%x "
                          "act %u fx",
                          p.slot, p.hp, p.hp_max, p.mp, p.mp_max, p.level, p.attack, p.defense,
                          p.agility, p.wisdom, p.luck, p.record_at, p.active);
            line += buf;
            for (const auto& e : p.effects) {
                std::snprintf(buf, sizeof(buf), " %llx/%x/%d", static_cast<unsigned long long>(e.vtable),
                              e.flag, e.turns);
                line += buf;
            }
            std::snprintf(buf, sizeof(buf), " act[%d] %s -> 0x%x", p.action_slots,
                          FNameText(p.action).value_or("?").c_str(), static_cast<unsigned>(p.target));
            line += buf;
            line += ";";
        }
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf), " phase %u actor %d cmds", s.phase, s.actor);
            line += buf;
            for (const auto& c : s.commands) {
                std::snprintf(buf, sizeof(buf), " {%u %u %s %s 0x%x}", c.valid, c.command,
                              FNameText(c.action).value_or("?").c_str(),
                              FNameText(c.item).value_or("?").c_str(), static_cast<unsigned>(c.target));
                line += buf;
            }
            for (const auto& e : s.enemies) {
                std::snprintf(buf, sizeof(buf), " e%u:%s->0x%x", e.slot, FNameText(e.action).value_or("?").c_str(),
                              static_cast<unsigned>(e.target));
                line += buf;
            }
        }
        if (line == logged_battle)
            return;
        Log(EDEN_DSMOD_LOG_INFO, line.c_str());
        logged_battle = line;
    }

    void LogScene(bool ok, const dq3::SceneState& s) {
        const std::string line = ok ? s.world + (s.pawn ? " pawn" : " no-pawn") +
                                          (s.llm_found ? " llm " + std::to_string(s.loading)
                                                       : std::string{" llm ?"})
                                    : std::string{"unreadable"};
        if (line == logged_scene)
            return;
        Log(EDEN_DSMOD_LOG_INFO, ("DQ3 DSMod: scene " + line).c_str());
        logged_scene = line;
    }

    void LogParty(const dq3::PartySnapshot& s) {
        std::string line = "DQ3 DSMod: party (" + std::to_string(s.roster_count) + " roster" +
                           (s.roto ? ", Erdrick title" : "") + "):";
        for (const auto& m : s.members) {
            std::string ascii;
            for (const char32_t c : m.name)
                ascii += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          " [%u] %s Lv%d HP %d/%d MP %d/%d st 0x%x voc %u looks %u hps %d;", m.slot,
                          ascii.c_str(), m.level, m.hp, m.hp_max, m.mp, m.mp_max, m.status,
                          m.vocation, m.looks, dq3::HpState(m));
            line += buf;
        }
        Log(EDEN_DSMOD_LOG_INFO, line.c_str());
    }

    void I64(const char* n, s64 v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, n, v);
    }
    void F64(const char* n, double v) {
        if (host.publish_f64)
            host.publish_f64(host.userdata, n, v);
    }
    void Text(const char* n, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, n, v.c_str());
    }
    void Log(u32 level, const char* msg) {
        const auto t0 = Clock::now();
        if (host.log)
            host.log(host.userdata, level, msg);
        log_us += Us(t0, Clock::now());
    }

    EdenDsmodHostApi host{};
    std::shared_ptr<std::atomic<bool>> pak_size_ok; ///< MakePakReader's archive size latch (before pak)
    dq3::RangeReader pak;
    /// EDEN_DSMOD_DQ3_ALL_OUTPUTS: also publish the diagnostics and log the per-change party / battle lines and
    /// the 600-sample cost line (none of those strings is built otherwise)
    const bool all_outputs;
    const bool map_trace;   ///< EDEN_DSMOD_DQ3_MAP_TRACE: log the Map image swap steps
    dq3::ArtLibrary art;
    // asset worker: table loads that failed (LoadTables)
    struct LoaderFailure {
        bool failed{}, permanent{};
        Clock::time_point at{};
        std::string logged;
    };
    std::mutex loader_mutex;
    std::array<LoaderFailure, TableCount> loader_failures{};
    bool resolved{};
    bool refused{};           ///< the code pins refuse this game (dq3.sys.wrong / flat)
    bool refused_flat_test{}; ///< EDEN_DSMOD_DQ3_TEST_REFUSE=pins-flat
    std::string last_world;   ///< the last readable viewport world name
    bool world_swap{};        ///< engine objects unreadable after the title / field world (a load)
    bool title_loading{};     ///< a save was confirmed on the title (its controllers all closed)
    bool title_yesno{};       ///< the title's Yes / No was up (a save chosen)
    int title_idle{};         ///< consecutive title readings after it without a Yes / No or an active list
    bool title_unreadable_logged{};
    std::string logged_title_ui; ///< the last logged title controller list
    std::map<u64, std::string> title_classes;
    u64 title_classes_world{};
    int logged_sys{-1};
    dq3::Roots roots;
    dq3::SceneCache scene_cache;
    std::string world, logged_scene;
    u64 sample_index{}, next_resolve{};
    std::string resolve_error, logged_resolve_error;
    dq3::PartyState published_state{dq3::PartyState::NoGame};
    dq3::PartySnapshot shown;
    u64 party_world{};
    bool held{};
    std::string why{"starting"};
    int invalid_run{};
    bool logged_9c{};
    std::vector<Card> cards;
    bool cards_dirty{true};
    u64 cards_gen{}; ///< bumped by every BuildCards (the Party tab rebuilds on it)
    u64 cards_retry_at{};
    const dq3::GameText* cards_text{};
    const dq3::FontAtlas* cards_font{};
    // battle page
    struct Detail {
        u64 field{};
        s32 strength{};
        bool strength_ok{};
        struct Entry {
            std::uint32_t name[2]{};
            u8 learn{}, timing{};
            std::string id;
            bool operator==(const Entry&) const = default;
        };
        std::vector<Entry> entries;
        bool operator==(const Detail&) const = default;
    };
    bool battle_resolved{}, record_ok{};
    /// Follow: the native battle list (dq3_battle.h ReadBattleMenu), read only while Follow is on; a
    /// MenuFocus::None or unreadable reading keeps the last settled state (between members, dialogs).
    bool battle_menu_ok{};
    std::map<u64, std::string> battle_menu_classes;
    std::optional<dq3::BattleMenu> follow_menu;
    /// The acting-unit id outside the action states (it keeps that value until the first action of a
    /// turn writes it, see dq3::ActorCurrent).
    s32 turn_actor{-1};
    bool actor_valid{};
    dq3::BattleRoots broots;
    dq3::BattleCache bcache;
    dq3::BattleSnapshot battle;
    bool battle_on{};
    bool battle_follow{};
    int battle_invalid{};
    std::string logged_battle, logged_battle_why;
    std::set<std::string> known;
    bool known_ok{};
    dq3::BattleSel sel{dq3::BattleSel::None};
    int sel_index{-1};
    Detail detail;
    u64 detail_field{}, detail_at{};
    std::array<std::vector<dq3::u32>, 4> icon_order; ///< per party slot (unit 20 + k)
    dq3::BattleView view;
    bool view_dirty{true};
    int view_page{-1};
    const dq3::BattleData* view_data{};
    const dq3::FontAtlas* view_font{};
    u64 view_retry_at{};
    double view_us{};
    std::array<std::optional<std::string>, 11> class_names;
    std::string logged_looks[4];
    // field tabs and the Map page
    int tab{};
    bool tabs_dirty{true};
    const dq3::FontAtlas* tabs_font{};
    std::string tabs_key, tab_label[dq3::FieldTabs];
    int tab_label_x[dq3::FieldTabs]{};
    dq3::MapRoots mroots;
    dq3::SceneState cur_scene; ///< the scene of the last read (empty when unreadable)
    dq3::LocationCache lcache;
    dq3::ChestScan chest_scan; ///< ReadChests' class names and per-level treasure flags
    dq3::LocationSnapshot loc;
    bool loc_ok{};
    std::string logged_loc_why;
    std::vector<std::string> visited;
    bool visited_ok{};
    u64 visited_at{}, chests_at{};
    std::optional<std::pair<int, int>> chests;
    int browse{};
    bool full_map{}, map_dirty{true}, map_visible{};
    int map_zoom{};
    std::int64_t map_reset{1};
    const dq3::MapData* map_data{};
    const dq3::FontAtlas* map_font{};
    dq3::MapView mview;
    dq3::MapImageRegistry map_ready; ///< map keys load_image composed (asset worker -> tick thread)
    dq3::MapImageSwap map_swap;
    std::string map_trace_want;
    std::vector<dq3::WorldIcon> world_icons;
    std::uint64_t world_icons_at{};
    // native button drives (field menu read once per sample, UI classes cached per world)
    std::map<u64, std::string> menu_classes;
    u64 menu_classes_world{};
    struct PartyDrive {
        bool active{}, waiting{}, terminal{};
        std::optional<dq3::PartyDragOrder> drag;
        int goal{-2}, pending{-1};
        std::array<bool, 6> target{}; ///< counter target of the pending press per button
        dq3::FieldMenuState expected, previous;
        Clock::time_point started{}, pressed{};
        void Clear() {
            active = waiting = terminal = false;
            pending = -1;
            drag.reset();
        }
    } party;
    dq3::FieldMenuState party_menu;
    dq3::HealMailbox heal_box;     ///< the load plan's mailbox (configured once, before the first sample)
    bool heal_ready{};             ///< the native field menu is closed (refreshed every ReadEvery samples)
    std::u32string heal_name;      ///< the lone member's name for "<name>'s wounds are healed!"
    std::string heal_msg;          ///< the last result line (styled), shown until heal_msg_until
    Clock::time_point heal_msg_until{};
    // Party / Bag / Journal tabs
    dq3::InfoRoots iroots;
    u64 party_field{};
    int party_sub{}, party_sel{}, party_slot{-1}, bag_page{}, bag_sel{}, mon_sel{-1};
    s64 party_key{}, bag_key{};
    std::optional<dq3::MemberDetail> member;
    std::array<std::optional<std::vector<dq3::OwnedItem>>, 3> bag_rows; ///< per bag, read while shown
    std::optional<dq3::JournalState> jstate;
    std::optional<std::map<std::string, s32>> record;
    u64 member_at{}, bags_at{}, journal_at{}, record_at{};
    int member_fail{};
    bool party_dirty{true}, bag_dirty{true}, journal_dirty{true};
    dq3::InfoView pv, bv, jv;
    const dq3::InfoData* bv_info{};
    const dq3::FontAtlas* bv_font{};
    const dq3::InfoData* pv_info{};
    const dq3::InfoData* jv_info{};
    const dq3::FontAtlas* pv_font{};
    u64 pv_cards_gen{};
    std::optional<u32> pv_gold;
    const dq3::MapData* jv_map{};
    std::atomic<EdenDsmodAstcDecoder> astc_fn{};
    std::atomic<void*> astc_user{};
    // 600-sample timing window
    Stat cost, scene_stat, party_stat, publish_stat, battle_stat, map_stat;
    double cost_avg{}, log_us{}, log_max{}, map_view_us{}, party_view_us{}, bag_view_us{}, journal_view_us{};
    int cost_n{}, reads_window{};
    bool non_ascii_logged{}; ///< the one warning line for dq3::NonAsciiTextCount() was written
    const dq3::InfoData* equip_max_info{}; ///< AddEquipMax: the data the flag below was computed for
    bool equip_max_on{};                   ///< that data has MAXHP / MAXMP items
    std::map<u64, std::pair<s32, s32>> equip_max; ///< F -> equipped MAXHP / MAXMP sums
    u64 equip_max_at{};
};

EdenDsmodBool SupportsBuild(const char* build_id) {
    if (TestRefuse() == "build")
        return EDEN_DSMOD_FALSE; // controlled test of the host's refusal (TestRefuse)
    return build_id && std::string_view{build_id} == BuildId ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory || !host->read_romfs || BuildHex(*host) != BuildId)
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
        if (p && host) {
            auto& m = *static_cast<Module*>(p);
            m.UpdateHost(*host);
            m.Sample();
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "DQ3 DSMod sample callback failed");
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
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t arg) {
    try {
        if (p && action)
            return static_cast<Module*>(p)->Action(action, arg) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
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
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, size_t size, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->DecodeFont(bytes, size, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Dragon Quest III HD-2D Remake DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN,
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
