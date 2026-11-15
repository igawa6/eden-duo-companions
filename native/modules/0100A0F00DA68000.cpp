// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Monster Sanctuary (0100A0F00DA68000) dual-screen module, update v524288 (Ver 2.1.0.32,
// merged main build 8EA928B6AA10C84F...). Reads the game state, party, follower and explore
// abilities, battle (the native command rail, skills, items, switch list, Monster Info, targets
// with the weaknesses / resistances the game itself shows) and the explored map with its
// teleporters. Through the format-2 guest helper (ms_guest_helper.S, MS-HELPER.md) the bottom
// screen drives native menus and picks (battle commands, targets, team swaps, equipment, the
// follower, fast travel) and hides the battle windows it mirrors on the top screen. The model is
// refreshed at 10 Hz and published on every sample (the snapshot is cleared each tick).
#include "ms_art.h"
#include "ms_config.h"
#include "ms_helper_mailbox.h"
#include "ms_mapdraw.h"
#include "ms_publish.h"
#include "ms_reader.h"

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100A0F00DA68000);
constexpr u8 BuildId[32]{0x8E, 0xA9, 0x28, 0xB6, 0xAA, 0x10, 0xC8, 0x4F,
                         0x9E, 0x69, 0x12, 0x9C, 0x45, 0xBC, 0xAA, 0xAF}; // v524288 (2.1.0.32)
constexpr int RefreshEvery = 6;                                          // samples (60 Hz -> 10 Hz)

// Host mailbox primitives, copied out of configure()'s extension table.
struct HelperIo {
    void* userdata{};
    EdenDsmodBool (*load_u32)(void*, std::uint32_t, std::uint32_t*){};
    EdenDsmodBool (*load_u64)(void*, std::uint32_t, std::uint64_t*){};
    EdenDsmodBool (*store_u32)(void*, std::uint32_t, std::uint32_t){};
    EdenDsmodBool (*store_u64)(void*, std::uint32_t, std::uint64_t){};
};
struct L32 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint32_t* v) const { return io.load_u32(io.userdata, o, v) == EDEN_DSMOD_TRUE; }
};
struct L64 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint64_t* v) const { return io.load_u64(io.userdata, o, v) == EDEN_DSMOD_TRUE; }
};
struct S32 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint32_t v) const { return io.store_u32(io.userdata, o, v) == EDEN_DSMOD_TRUE; }
};
struct S64 {
    HelperIo io;
    bool operator()(std::uint32_t o, std::uint64_t v) const { return io.store_u64(io.userdata, o, v) == EDEN_DSMOD_TRUE; }
};
using HelperClient = ms_helper_mailbox::Client<L32, L64, S32, S64>;
namespace hm = ms_helper_mailbox;

constexpr std::uint32_t HideLeaseFrames = 90; // ~1.5 s of game frames without a sample: menu back
constexpr std::uint64_t AnswerTimeoutSamples = 180;
constexpr std::uint64_t MessageSamples = 150;

bool ParseBuildId(const char* s, u8 (&out)[32]) {
    if (!s || std::strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; ++i) {
        const int c = std::toupper(static_cast<unsigned char>(s[i]));
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return false;
        out[i / 2] = static_cast<u8>((i % 2 ? out[i / 2] << 4 : 0) | v);
    }
    return true;
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    u8 id[32]{};
    return ParseBuildId(build_id, id) && std::memcmp(id, BuildId, sizeof(BuildId)) == 0
               ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

struct Module {
    EdenDsmodHostApi host{}; ///< copy: read_romfs + userdata for the asset worker
    std::unique_ptr<ms::Art> art;
    ms::Reader reader;
    ms::Model model;
    u64 samples{};
    std::string last_debug;
    std::atomic<int> team_focus{0};
    /// Image src_bind names "T:<font>[/<k>]:<w>:<a>:<value>" found in the manifest: each sample
    /// publishes the name as the text picture key of <value>'s current text (at 4x, or k times).
    struct TextBind {
        std::string name, prefix, value;
    };
    std::vector<TextBind> text_binds;
    std::vector<std::string> bind_values; ///< the text_binds' values, sorted (the sink keeps only these)
    std::string key_buf, text_buf;        ///< the sink's reused buffers (no per-publish allocation)
    std::mutex map_mutex;
    ms::MapInfo map; ///< copy for the asset worker

    // guest helper (format-2 load plan): present only when the host installed it for this build
    std::optional<HelperClient> helper;
    /// Options (persisted runtime flags ms.hide_menu, default on / ms.hide_combo, default off):
    /// hide the native battle-menu windows / the combo counter on top
    bool hide_menu{true};
    bool hide_combo{};
    bool swap_mode{};           ///< Team page: the next row tap swaps with the focused monster
    int equip_slot{-1};         ///< Team page: equipment picker open for this slot
    std::uint64_t staged_at{};  ///< sample count when the pending request was staged
    std::string staged_what;    ///< what it was (log)
    std::string message;        ///< last result for the page
    bool message_ok{};
    std::uint64_t message_at{};
    /// Result feedback without a toast (owner decision 2026-11-12, FEEDBACK-PROPOSAL Option 1):
    /// the row the last request came from ("s2", "tq5", "fp1" ...) pulses amber while the game
    /// has not answered (fb.<key>.b) and red when it refused or did not answer (fb.<key>.n, for
    /// the message window, with the reason in the page's own text slot); a success shows nothing
    /// but the game's own state change
    std::string fb_key, fb_next;
    bool fb_refused{};
    std::uint32_t last_heartbeat{};
    std::uint64_t heartbeat_seen{}; ///< sample count when the heartbeat last moved
    // EDEN_MS_DIAGNOSTICS (ms_config.h) only: the helper counters, the follower watch, reads
    std::uint64_t diag_at{};
    std::string follower_watch;
    bool detached{}; ///< after Destroy (or h_detach): no lease renewal, no requests
    /// guest reads per refresh since the last report (logged every ~5 s, by companion state)
    struct ReadStats {
        std::uint64_t refreshes{}, total{}, max{};
    };
    std::map<int, ReadStats> read_stats;
    std::uint64_t read_stats_at{};
    /// module pictures rendered (asset worker): particle frames ("@" keys), map sparkles, others
    std::atomic<std::uint64_t> fx_pictures{}, sparkle_pictures{}, other_pictures{};

    // motion: the game's own idle clips / selection / map blink, off with "Reduce motion"
    std::chrono::steady_clock::time_point epoch{std::chrono::steady_clock::now()};
    std::map<std::string, ms::PictureIndex::Clip, std::less<>> clips; ///< copy (lock-free picks)
    std::set<std::string, std::less<>> fx_sets; ///< sets with particle effects (P records)
    bool clips_loaded{};
    bool reduce_motion{};
    double model_at{}; ///< NowMs of the last refresh (the read SelectionView time is from then)
    // map motion geometry, per refresh (the model changes only then): the Map page's two views, the
    // Exploration minimap, the player's map position and the teleporter cells
    ms::MapView map_views[2], mini_view;
    float player_x{}, player_y{};
    std::vector<std::pair<float, float>> tp_cells;
    std::uint32_t model_hb{}; ///< the helper heartbeat (game frames) at the last refresh
    std::map<std::string, std::string, std::less<>> labels; ///< native strings, cached once read
    /// The battle's description strip (the hovered skill / level, item or command, as the native
    /// tooltip words it) is read whole: a text that needs more than its one line is shown a line
    /// at a time, turning every DescPageMs or on a tap on the strip (b_desc).
    struct DescPages {
        std::string full;
        std::vector<std::string> lines; ///< the pages (empty: the text fits the line)
        bool measured{}, narrow{};
        int page{}, taps{};
        double since{};
    };
    std::map<std::string, DescPages, std::less<>> desc_pages;
    std::atomic<int> desc_taps{}; ///< b_desc taps (action thread -> sample thread)
};

constexpr double Pi = 3.14159265358979;
constexpr int DotSteps = 12;                            // the 1 s player-dot ping, 12 pictures
constexpr std::uint32_t DotBoxL = 48, DotBoxS = 32;     // 2 map cells of each map picture
// the teleporter sparkle over the Map page views (the native MinimapTileView's Rect2x2 particles):
// one picture per FxSteps step and phase group, in a fixed box (canvas px); the tile's centre sits
// at (SparkleBoxW / 2, SparkleTileY) and one native tile px is cell / 8 widget px (cell <= 48)
constexpr std::uint32_t SparkleBoxW = 96, SparkleBoxH = 144, SparkleTileY = 104;
constexpr int SparkleSlots = 24, SparkleGroups = 3;
constexpr int GlowSteps = 6;                            // manifest GLOW: GlowingColor .. gray
// the description strip (gen_manifest page_battle): its line is 298 mockup px, 238 while the
// window Back (x 250..306, y 190..206) shows beside it; a paged text keeps 22 px at the right for
// the page count ("2/3")
constexpr int DescLineW = 298, DescPageW = 276, DescLineNarrowW = 238, DescPageNarrowW = 216;
constexpr double DescPageMs = 3500;

template <class V>
const typename V::value_type* At(const V& v, std::int64_t i) {
    return i >= 0 && static_cast<std::size_t>(i) < v.size() ? &v[static_cast<std::size_t>(i)] : nullptr;
}

double NowMs(const Module& m) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m.epoch).count();
}

bool IsDescription(std::string_view k) {
    return k == "b.sdesc" || k == "b.tipdesc" || k == "b.rdesc";
}

/// A description that fits its line in the regular or the small font is shown as before (the
/// "W:" box's own fit); a longer one is wrapped at DescPageW in the regular font and `text`
/// becomes the current line; returns its page count ("1/3", the caller publishes it as <k>.pg)
/// and publishes <k>.pgw / <k>.pgn (where the count goes) and b.desc.pg; "" for a text that is
/// not paged. Never cut: every line is
/// shown in turn.
std::string PageDescription(Module& m, const EdenDsmodHostApi& host, std::string_view k, std::string& text) {
    auto& p = m.desc_pages[std::string{k}];
    const double now = NowMs(m);
    // the item window and the level popup carry the window Back at the strip's right end
    const bool narrow = k == "b.tipdesc" || (k == "b.sdesc" && m.model.battle.level_open);
    if (p.full != text || p.narrow != narrow) {
        p = {};
        p.full = text;
        p.narrow = narrow;
        p.since = now;
        p.taps = m.desc_taps.load();
    }
    if (!p.measured) {
        // nullopt while a decode holds the art lock: measured on a later sample
        const int line_w = narrow ? DescLineNarrowW : DescLineW;
        const auto regular = m.art->WrapLines("regular", line_w, text);
        // (the narrow line has no small-font fallback: the W box would draw the regular font)
        const auto small = regular && regular->size() > 1 && !narrow ? m.art->WrapLines("small", line_w, text) : regular;
        if (!regular || !small)
            return {};
        if (regular->size() > 1 && small->size() > 1) {
            auto pages = m.art->WrapLines("regular", narrow ? DescPageNarrowW : DescPageW, text);
            if (!pages)
                return {};
            p.lines = std::move(*pages);
        }
        p.measured = true;
    }
    const int n = static_cast<int>(p.lines.size());
    if (n < 2)
        return {};
    if (const int taps = m.desc_taps.load(); taps != p.taps) {
        p.taps = taps;
        p.page = (p.page + 1) % n;
        p.since = now;
    } else if (now - p.since >= DescPageMs) {
        p.page = (p.page + 1) % n;
        p.since = now;
    }
    text = p.lines[static_cast<std::size_t>(p.page)];
    // the page count's place: the line's end, left of Back while it shows (<k>.pgn / .pgw)
    host.publish_i64(host.userdata, (std::string{k} + (narrow ? ".pgn" : ".pgw")).c_str(), 1);
    host.publish_i64(host.userdata, "b.desc.pg", 1);
    return std::to_string(p.page + 1) + "/" + std::to_string(n);
}

/// The idle-clip frame of a battle sprite set at this moment: "Vaero/0" -> "birdy/3~Vaero"
/// (frame sprite ~ set, so the picture lines the frames up by the clip). Reduce motion keeps
/// the clip's first frame. Each card has its own phase (salt) so a team does not breathe in
/// lockstep. A set with particle effects (the Spectral familiars' fire, Specter, Frosty ...)
/// also carries the effect's moment, "@<n>" in clip frames, so the picture changes only when the
/// clip frame does (Reduce motion: 0, a still of the running effect).
std::string AnimFrame(const Module& m, const std::string& sprite, int salt) {
    if (m.clips.empty())
        return sprite;
    const std::string set = sprite.substr(0, sprite.find('/'));
    std::string lower = set;
    for (auto& ch : lower)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto it = m.clips.find(lower);
    if (it == m.clips.end() || it->second.frames.empty())
        return sprite;
    // whole clip frames since the epoch: the frame shown and the effect's moment on one clock
    const double fps = it->second.fps;
    const long long n = static_cast<long long>((NowMs(m) + salt * 173.0) * fps / 1000.0);
    const std::size_t frame = m.reduce_motion ? 0 : ms::ClipFrame(it->second, (n + 0.5) * 1000.0 / fps);
    std::string key = it->second.frames[frame] + "~" + set;
    if (m.fx_sets.count(lower)) {
        const long long loop = std::max<long long>(1, std::llround(ms::FxPeriod * fps));
        key += "@" + std::to_string(m.reduce_motion ? 0 : n % loop);
    }
    return key;
}

// The Map page's two map views (the narrow one beside the Teleporter panel, the wide one while it
// is collapsed): widget size in canvas px; the picture is drawn at MapScale times that, so the
// host's pan / zoom (up to the manifest's max_zoom) keeps room frames and the native map symbols
// sharp (nearest sampling of whole 2 px features at zoom 1).
struct MapViewDef {
    const char* id;
    char key;
    std::uint32_t w, h;
};
constexpr MapViewDef MapViews[2] = {{"map-n", 'L', 800, 704}, {"map-w", 'W', 1240, 704}};
constexpr std::uint32_t MapScale = 2, MapCell = 24;
// a repeated area's teleporter row: the crystal's place in its area (gen_manifest.py TP_MINI)
constexpr std::uint32_t TeleportMiniW = 176, TeleportMiniH = 44;

void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->read_memory ||
            std::memcmp(host->build_id, BuildId, sizeof(BuildId)) != 0)
            return nullptr;
        auto m = std::make_unique<Module>();
        m->host = *host;
        // collect "T:" binds from the manifest text (config_json is the whole manifest.json)
        const std::string_view cfg = config ? std::string_view{config} : std::string_view{};
        // "W:<font>:<w>:<lines>:<value>": the value word-wrapped (module:ms:wrap:...)
        for (std::size_t at = cfg.find("\"W:"); at != std::string_view::npos; at = cfg.find("\"W:", at + 1)) {
            const auto end = cfg.find('"', at + 1);
            if (end == std::string_view::npos)
                break;
            const std::string name{cfg.substr(at + 1, end - at - 1)};
            std::size_t c = 0, colons = 0;
            for (std::size_t i = 0; i < name.size() && colons < 4; ++i)
                if (name[i] == ':' && ++colons == 4)
                    c = i;
            if (colons != 4 || std::any_of(m->text_binds.begin(), m->text_binds.end(),
                                           [&](const auto& t) { return t.name == name; }))
                continue;
            m->text_binds.push_back({name, "module:ms:wrap:" + name.substr(2, name.find(':', 2) - 2) + ":4:" +
                                               name.substr(name.find(':', 2) + 1, c - name.find(':', 2)),
                                     name.substr(c + 1)});
        }
        for (std::size_t at = cfg.find("\"T:"); at != std::string_view::npos; at = cfg.find("\"T:", at + 1)) {
            const auto end = cfg.find('"', at + 1);
            if (end == std::string_view::npos)
                break;
            const std::string name{cfg.substr(at + 1, end - at - 1)};
            // T:<font>[/<k>]:<w>:<a>:<value> (k: the picture's scale, 1..8, default the canvas' 4)
            std::size_t c = 1, colons = 0;
            for (std::size_t i = 0; i < name.size() && colons < 4; ++i)
                if (name[i] == ':' && ++colons == 4)
                    c = i;
            if (colons != 4 || std::any_of(m->text_binds.begin(), m->text_binds.end(),
                                           [&](const auto& t) { return t.name == name; }))
                continue;
            const auto first = name.find(':'), second = name.find(':', first + 1);
            std::string font = name.substr(2, second - 2), scale = "4";
            if (const auto slash = font.find('/'); slash != std::string::npos) {
                scale = font.substr(slash + 1);
                font.resize(slash);
            }
            m->text_binds.push_back({name, "module:ms:txt:" + font + ":" + scale + ":" + name.substr(second + 1, c - second),
                                     name.substr(c + 1)});
        }
        for (const auto& t : m->text_binds)
            m->bind_values.push_back(t.value);
        std::sort(m->bind_values.begin(), m->bind_values.end());
        m->bind_values.erase(std::unique(m->bind_values.begin(), m->bind_values.end()), m->bind_values.end());
        if (host->read_romfs) {
            m->art = std::make_unique<ms::Art>(
                [h = &m->host](const char* path, std::uint64_t off, void* out, std::size_t size) {
                    return h->read_romfs(h->userdata, path, off, out, size);
                });
        }
        return m.release();
    } catch (...) {
        return nullptr;
    }
}

/// What Destroy does to the guest: the helper puts the battle menu back next frame. Also run by
/// the diagnostics-only "h_detach" action (the runtime keeps a module with installed guest hooks
/// loaded until the game exits, so Destroy cannot be triggered in a running session).
void Detach(Module& m) {
    if (m.helper)
        m.helper->SetHide(false, false, 0);
    m.detached = true;
}

void Destroy(void* p) {
    auto* m = static_cast<Module*>(p);
    if (m)
        Detach(*m);
    delete m;
}

void Log(const Module& m, const std::string& text) {
    if (m.host.log)
        m.host.log(m.host.userdata, EDEN_DSMOD_LOG_INFO, ("Monster Sanctuary helper: " + text).c_str());
}

void SetMessage(Module& m, std::string text, bool ok) {
    m.fb_refused = !ok;
    if (ok)
        m.fb_key.clear(); // success: the state change is the feedback
    if (m.art) {
        const char* key = text == "Done" ? "done" : text == "Not available" ? "unavailable" : nullptr;
        if (key) {
            const auto native = m.art->String(key);
            if (!native.empty())
                text = native;
        }
    }
    m.message = std::move(text);
    m.message_ok = ok;
    m.message_at = m.samples;
}

/// Every sample: hide lease, heartbeat, answers, timeouts.
void ServiceHelper(Module& m, const EdenDsmodHostApi&) {
    if (!m.helper || m.detached)
        return;
    auto& h = *m.helper;
    const std::uint32_t hb = h.Heartbeat();
    if (hb != m.last_heartbeat) {
        m.last_heartbeat = hb;
        m.heartbeat_seen = m.samples;
    }
    h.SetHide(m.hide_menu, m.hide_combo, HideLeaseFrames);
    if (const auto out = h.Poll(); out.answered) {
        const bool ok = out.code == hm::Result::Ok;
        SetMessage(m, std::string{hm::ResultText(out.code)}, ok);
        Log(m, m.staged_what + " -> " + std::to_string(static_cast<unsigned>(out.code)) + " " +
                   std::string{hm::ResultText(out.code)} + " value=" + std::to_string(out.value));
    } else if (h.Busy() && m.samples - m.staged_at > AnswerTimeoutSamples) {
        h.Abandon();
        SetMessage(m, "No answer from the game", false);
        Log(m, m.staged_what + " -> no answer, abandoned");
    }
    if (ms::Diagnostics && m.samples - m.diag_at >= 300) { // every ~5 s: the helper's own view
        m.diag_at = m.samples;
        Log(m, "hb=" + std::to_string(hb) + " gs=" + std::to_string(static_cast<int>(h.Word(hm::OffDiagGs))) +
                   " cs=" + std::to_string(static_cast<int>(h.Word(hm::OffDiagCs))) +
                   " reqs=" + std::to_string(h.Word(hm::OffDiagReqs)) +
                   " precull=" + std::to_string(h.Word(hm::OffDiagPrecull)) +
                   " rehid=" + std::to_string(h.Word(hm::OffDiagSaves)) + "/" +
                   std::to_string(h.Word(hm::OffDiagRemarks)) +
                   " hide=" + std::to_string(h.Word(hm::OffHideOn)) + "/" + std::to_string(h.Word(hm::OffHideState)) +
                   " combo=" + std::to_string(h.Word(hm::OffComboHideState)));
    }
}

bool HelperAlive(const Module& m) {
    return m.helper && m.samples - m.heartbeat_seen < 60;
}

/// The names PublishMotion publishes, built once (no string building per sample).
struct DotKeys {
    std::string on, x, y, pic;
};
struct ViewKeys {
    std::string zoom, pan_x, pan_y; ///< the host's pan / zoom of the view
    DotKeys dot;
    std::vector<DotKeys> sparkles; ///< SparkleSlots
};
struct MotionKeys {
    ViewKeys views[2];
    DotKeys mini;
};

DotKeys DotKeyNames(const std::string& prefix) {
    return {prefix + ".on", prefix + ".x", prefix + ".y", prefix + ".pic"};
}

const MotionKeys& MotionKeyNames() {
    static const MotionKeys keys = [] {
        MotionKeys k;
        for (std::size_t i = 0; i < 2; ++i) {
            const std::string id = MapViews[i].id;
            auto& v = k.views[i];
            v.zoom = "@view_zoom:" + id;
            v.pan_x = "@view_pan_x:" + id;
            v.pan_y = "@view_pan_y:" + id;
            const bool narrow = MapViews[i].key == 'L';
            v.dot = DotKeyNames(narrow ? "m.dot" : "m.dotw");
            for (int s = 0; s < SparkleSlots; ++s)
                v.sparkles.push_back(DotKeyNames((narrow ? "m.tpl" : "m.tpw") + std::to_string(s)));
        }
        k.mini = DotKeyNames("m.mdot");
        return k;
    }();
    return keys;
}

/// The game's own motion, as published values (nothing while Reduce motion is on):
///   sel.d      the selection arrows' sway, canvas px outward (SelectionView: 1 px, 0.5 s cosine,
///              rounded to whole pixels, from the view's own dTimeAcc so it keeps the top's phase)
///   e<i>.glow / a<i>.glow  the targeted monster's colour pulse (GlowingColor -> gray, 0.5 s)
///   m.dot.* / m.mdot.*  the player dot ping on the Map / Exploration map (1 s)
///   m.tp<l|w><i>.*      the teleporter sparkles on the Map page views
/// The map parts only for the page on screen (`page`, the host's __page; null = unknown: all).
void PublishMotion(Module& m, const ms::Sink& o, const char* page) {
    const auto& md = m.model;
    const auto& b = md.battle;
    const double now = NowMs(m);
    if (ms::CanBattleInput(md) && b.sel_time >= 0) {
        const double t = b.sel_time + (now - m.model_at) / 1000.0;
        const bool still = m.reduce_motion || b.sel_locked;
        const auto d = still ? 0 : std::lround(-std::cos(4 * Pi * t)) * 4;
        o.i64("sel.d", d);
        // the target cards' selector corners sway around their card's slot (e<i>.xl / .xr), on
        // the side whose cards show (the allies only for a skill on the own side)
        if (b.targeting && d != 0)
            for (const bool ally : {false, true}) {
                if (ally != ms::TargetsAllies(b))
                    continue;
                const auto slots = ms::CardSlots(b, ally);
                for (std::size_t i = 0; i < slots.size(); ++i) {
                    const std::string k = (ally ? "a" : "e") + std::to_string(i);
                    o.i64(k + ".xl", slots[i] * ms::TargetCardPx - d);
                    o.i64(k + ".xr", slots[i] * ms::TargetCardPx + d);
                }
            }
        if (b.targeting && !still) {
            const double f = 0.5 - 0.5 * std::cos(2 * Pi * t / 0.5);
            const auto step = std::lround(f * (GlowSteps - 1));
            if (b.target >= 0)
                o.i64("e" + std::to_string(b.target) + ".glow", step);
            // a skill on the own side: the targeted ally's card pulses the same way
            for (std::size_t i = 0; i < b.allies.size() && i < 3; ++i)
                if (b.target_monster && b.allies[i].ptr == b.target_monster)
                    o.i64("a" + std::to_string(i) + ".glow", step);
        }
    }
    // the title prompt's own blink (FlashRenderer: its counter counts game frames; the heartbeat
    // does too, so the phase follows the top screen between reads); Reduce motion keeps it on.
    // Always published (0/1): ti.show reads !ti.off, and a derived expr reads an unpublished
    // name as missing (the prompt would never show)
    bool title_off = false;
    if (md.title_flash_mask > 0 && md.title_prompt > 0 && !m.reduce_motion && m.helper) {
        const auto frames = static_cast<int>(m.helper->Heartbeat() - m.model_hb);
        title_off = ((md.title_flash_counter + frames) & md.title_flash_mask) != 0;
    }
    o.i64("ti.off", title_off ? 1 : 0);
    if (!md.map.valid || md.state == ms::State::Loading || md.state == ms::State::Combat)
        return;
    // only for the page on screen: the Map page's views, the Exploration page's minimap (a host
    // that does not answer __page: both, as before)
    const bool any_page = page == nullptr;
    const std::string_view page_id = page ? page : "";
    const bool on_map = any_page || page_id == "map", on_field = any_page || page_id == "field";
    if (!on_map && !on_field)
        return;
    const int phase = m.reduce_motion ? DotSteps / 2 : static_cast<int>(std::fmod(now / 1000.0, 1.0) * DotSteps);
    const auto& keys = MotionKeyNames();
    char pic[48];
    const auto pic_view = [&pic](int n) {
        return std::string_view{pic, static_cast<std::size_t>(std::clamp(n, 0, static_cast<int>(sizeof pic) - 1))};
    };
    const auto publish_dot = [&](const DotKeys& k, int sx, int sy, int cell_px, std::uint32_t box, std::uint32_t w,
                                 std::uint32_t h) {
        const int half = static_cast<int>(box / 2);
        if (cell_px <= 0 || sx < 0 || sy < 0 || sx > static_cast<int>(w) || sy > static_cast<int>(h))
            return;
        o.i64(k.on, 1);
        o.i64(k.x, sx - half);
        o.i64(k.y, sy - half);
        o.text(k.pic, pic_view(std::snprintf(pic, sizeof pic, "module:ms:dot:%d:%d:%u", phase, cell_px, box)));
    };
    if (on_map) {
        // the Map page: the dot and the teleporter sparkles follow the host's pan / zoom of each
        // view (@view_zoom / @view_pan_x / @view_pan_y:<id>, zoom-1 widget px); a host that does
        // not answer = the home view. Teleporters fall into SparkleGroups phase groups (the native
        // ones each run their own random sparkle).
        const bool sparkle = m.fx_sets.count("maptp") != 0;
        const int base = m.reduce_motion ? 0
                                         : static_cast<int>(std::fmod(now, ms::FxPeriod * 1000.0) * ms::FxSteps / 1000.0);
        for (std::size_t vi = 0; vi < 2; ++vi) {
            const auto& def = MapViews[vi];
            const auto& v = m.map_views[vi];
            const auto& k = keys.views[vi];
            if (v.cell == 0)
                continue;
            double zoom = 1, pan_x = 0, pan_y = 0;
            if (m.host.get_f64) {
                zoom = m.host.get_f64(m.host.userdata, k.zoom.c_str(), 1.0);
                pan_x = m.host.get_f64(m.host.userdata, k.pan_x.c_str(), 0.0);
                pan_y = m.host.get_f64(m.host.userdata, k.pan_y.c_str(), 0.0);
                if (!std::isfinite(zoom) || zoom <= 0 || !std::isfinite(pan_x) || !std::isfinite(pan_y))
                    zoom = 1, pan_x = pan_y = 0;
            }
            // the host answers the view's stored pan, but draws the picture with it clamped to the
            // picture (mod_view_default.h ClampPanAxis, no pan_bounds: [0, size * (1 - 1 / zoom)]);
            // place over the picture as drawn (a drag at zoom 1 moves nothing)
            const auto drawn = [zoom](double pan, double size) {
                return std::clamp(pan, 0.0, std::max(0.0, size - size / zoom));
            };
            pan_x = drawn(pan_x, def.w);
            pan_y = drawn(pan_y, def.h);
            const double wx = static_cast<double>(v.X(m.player_x)) / MapScale;
            const double wy = static_cast<double>(v.Y(m.player_y)) / MapScale;
            const int sx = static_cast<int>(std::lround((wx - pan_x) * zoom));
            const int sy = static_cast<int>(std::lround((wy - pan_y) * zoom));
            const int dot_cell = std::max(1, static_cast<int>(std::lround(v.cell / static_cast<double>(MapScale) * zoom)));
            publish_dot(k.dot, sx, sy, std::min(dot_cell, 64), DotBoxL, def.w, def.h);
            if (!sparkle)
                continue;
            const int cell = std::clamp(static_cast<int>(std::lround(v.cell / static_cast<double>(MapScale) * zoom)), 1, 48);
            int slot = 0;
            for (std::size_t i = 0; i < m.tp_cells.size() && slot < SparkleSlots; ++i) {
                const double tx = v.X(m.tp_cells[i].first) / static_cast<double>(MapScale);
                const double ty = v.Y(m.tp_cells[i].second) / static_cast<double>(MapScale);
                const int px = static_cast<int>(std::lround((tx - pan_x) * zoom));
                const int py = static_cast<int>(std::lround((ty - pan_y) * zoom));
                if (px < 0 || py < 0 || px > static_cast<int>(def.w) || py > static_cast<int>(def.h))
                    continue;
                const int group = static_cast<int>(i % SparkleGroups);
                const int step = m.reduce_motion ? 0 : (base + group * (static_cast<int>(ms::FxPeriod) * ms::FxSteps / SparkleGroups)) %
                                                           (static_cast<int>(ms::FxPeriod) * ms::FxSteps);
                const auto& sk = k.sparkles[static_cast<std::size_t>(slot++)];
                o.i64(sk.on, 1);
                o.i64(sk.x, px - static_cast<int>(SparkleBoxW / 2));
                o.i64(sk.y, py - static_cast<int>(SparkleTileY));
                o.text(sk.pic, pic_view(std::snprintf(pic, sizeof pic, "module:ms:tpfx:%d:%d", step, cell)));
            }
        }
    }
    // the Exploration page's minimap (not zoomable)
    if (on_field && m.mini_view.cell != 0) {
        const int px = m.mini_view.X(m.player_x), py = m.mini_view.Y(m.player_y);
        const int half = static_cast<int>(DotBoxS / 2);
        if (px >= half && py >= half && px <= 584 - half && py <= 252 - half)
            publish_dot(keys.mini, px, py, static_cast<int>(m.mini_view.cell), DotBoxS, 584, 252);
    }
}

/// Per refresh: the geometry PublishMotion places from (ViewOf and the teleporter scan are
/// O(rooms); the model changes only at a refresh).
void UpdateMotionGeometry(Module& m) {
    const auto& map = m.model.map;
    for (std::size_t i = 0; i < 2; ++i)
        m.map_views[i] = map.valid ? ms::ViewOf(map, MapViews[i].w * MapScale, MapViews[i].h * MapScale,
                                                MapCell * MapScale, true)
                                   : ms::MapView{};
    m.mini_view = map.valid ? ms::ViewOf(map, 584, 252, 16) : ms::MapView{};
    // the player position (the current room's centre when not known)
    m.player_x = map.player_x;
    m.player_y = map.player_y;
    if (!map.player)
        for (const auto& r : map.rooms)
            if (r.current) {
                m.player_x = r.x + r.w / 2;
                m.player_y = r.y + r.h / 2;
            }
    ms::TeleporterCells(map, m.tp_cells);
}

/// Values the action widgets bind to (outside ms::Publish: they depend on companion state).
void PublishActions(Module& m, int focus, const ms::Sink& o) {
    const auto& md = m.model;
    if (md.map.valid && md.state != ms::State::Loading)
        o.text("m.help", "Pinch to zoom / Drag to pan");
    if (HelperAlive(m))
        o.i64("h.alive", 1);
    if (md.state == ms::State::Unknown || md.state == ms::State::Title || md.state == ms::State::Loading)
        return;
    // a refusal (or no answer): the reason, for the page's own text slot, and the tapped row's red
    // cue; a success publishes no text
    const bool refusal = !m.message.empty() && !m.message_ok && m.samples - m.message_at < MessageSamples;
    if (refusal) {
        o.text("h.msg", m.message);
        o.i64("h.no", 1);
    }
    const bool busy = m.helper && m.helper->Busy();
    if (busy)
        o.i64("h.busy", 1);
    if (!m.fb_key.empty()) {
        if (busy && !m.fb_refused)
            o.i64("fb." + m.fb_key + ".b", 1);
        else if (refusal)
            o.i64("fb." + m.fb_key + ".n", 1);
        else if (!busy)
            m.fb_key.clear();
    }
    const auto& b = md.battle;
    if (ms::CanBattleInput(md) && b.menu_open && !b.targeting)
        o.i64("b.menu", 1);
    // teleporter rows: the native teleport menu's own list while it is open at a teleporter
    // the game's own teleporter list (explored teleporter rooms, MapMenu.teleports): direct
    // fast travel from the bottom screen while exploring
    if (!md.map.teleports.empty()) {
        if (md.map.teleport_menu)
            o.i64("m.tpmenu", 1);
        // 0/1: the Map page's "not now" dim reads !m.tpok
        o.i64("m.tpok", ms::CanTeleport(md) ? 1 : 0);
        ms::PublishTeleports(md.map, o, ms::CanTeleport(md));
    }
    if (m.swap_mode)
        o.i64("t.swap", 1);
    // equipment picker for the focused monster's slot; q.open always published (0/1): the Team
    // page's Swap tap reads !q.open (a derived expr reads an unpublished name as missing)
    o.i64("q.open", m.equip_slot >= 0 ? 1 : 0);
    if (m.equip_slot >= 0) {
        o.i64("q.slot", m.equip_slot);
        const auto& list = m.equip_slot < 2 ? md.weapons : md.accessories;
        const int n = static_cast<int>(std::min<std::size_t>(list.size(), 8));
        for (int i = 0; i < n; ++i) {
            const auto& e = list[static_cast<std::size_t>(i)];
            const std::string k = "q" + std::to_string(i) + ".";
            o.i64(k + "v", 1);
            o.text(k + "name", e.name);
            o.i64(k + "n", e.quantity);
            // the owned monsters that have this same item equipped (team, bench, farm)
            // ... other than the monster being equipped
            std::vector<const ms::InvEquip::Owner*> others;
            const auto* focused = At(md.party, m.team_focus.load());
            for (const auto& ow : e.equipped_on)
                if (!focused || ow.monster != focused->ptr)
                    others.push_back(&ow);
            if (!others.empty()) {
                std::string name = others.front()->name;
                while (!name.empty() && name.back() == ' ')
                    name.pop_back();
                o.text(k + "eqtxt", "Equipped on " + name +
                                        (others.size() > 1 ? " +" + std::to_string(others.size() - 1) : ""));
                o.i64(k + "eq.on", 1);
                if (!others.front()->sprite.empty()) {
                    o.text(k + "eqspr", "module:ms:row2:" + AnimFrame(m, others.front()->sprite, 40 + i));
                    o.i64(k + "eqspr.on", 1);
                }
            }
        }
    }
    (void)focus;
}

/// Stage one helper request; false (refused haptic) when it cannot be sent now.
bool Stage(Module& m, hm::Request r, std::string what) {
    if (!m.helper) {
        m.fb_key = m.fb_next;
        SetMessage(m, "Needs the in-game helper", false);
        return false;
    }
    if (m.helper->Busy()) {
        // the earlier request still pulses amber; this tap is refused on its own row
        m.fb_key = m.fb_next;
        SetMessage(m, "Busy, try again", false);
        return false;
    }
    r.epoch = m.helper->Epoch();
    if (!r.epoch || !m.helper->TryStage(r))
        return false;
    m.fb_key = m.fb_next; // this row pulses until the game answers
    m.fb_refused = false;
    m.staged_at = m.samples;
    m.staged_what = std::move(what);
    Log(m, "staged " + m.staged_what);
    return true;
}

EdenDsmodBool B(bool v) {
    return v ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

bool MenuPick(Module& m, hm::MenuKind kind, const ms::MenuRow* r, const char* what) {
    if (!r || r->item == 0)
        return false;
    return Stage(m, {hm::Op::MenuPick, 0, r->item, 0, static_cast<std::uint32_t>(kind),
                     static_cast<std::uint32_t>(r->column), static_cast<std::uint32_t>(r->row)},
                 std::string{what} + " " + r->name);
}

/// The row a direct action came from, as the manifest's feedback cues name it (gen_manifest.py
/// fb_cue): "" for actions without one.
std::string FeedbackKey(std::string_view a, std::int64_t arg, bool swap_mode) {
    const auto n = std::to_string(arg);
    if (a == "follow")
        return "fp" + n;
    if (a == "team_focus")
        return swap_mode ? "tm" + n : "";
    if (a == "eq_pick")
        return "q" + n;
    if (a == "tp")
        return "tq" + n;
    if (a == "b_cmd")
        return "s" + n;
    if (a == "b_skst") // the skill window's icon strip (its other skill column)
        return "ss" + n;
    if (a == "b_level") // the focused skill's "[Y] Select Level"
        return "lvy";
    if (a == "b_lvl")
        return "lv" + n;
    if (a == "b_rail")
        return "r" + n;
    if (a == "b_item") // slot * 8 + row: slot 0 the expanded column, 1.. the icon strips
        return arg / 8 == 0 ? "ix" + std::to_string(arg % 8)
                            : "sk" + std::to_string(arg / 8 - 1) + "_" + std::to_string(arg % 8);
    if (a == "b_switch")
        return "w" + n;
    if (a == "b_tgt")
        return "e" + n;
    if (a == "b_ally")
        return "a" + n;
    if (a == "b_info")
        return arg < 8 ? "a" + n : "ic" + std::to_string(arg - 8);
    if (a == "b_back" || a == "b_subback")
        return "bk";
    return {};
}

/// The game accepts a team swap, equipping and a follower change only while exploring (helper
/// FIELD_GATE: GameStates.Exploring; dialogue dims the page). In battle (its start screen
/// included), the pause menu or a cinematic the pages show those controls disabled
/// (gen_manifest ms.explore), an open swap or equipment picker closes, and a stale tap is refused.
bool FieldEdit(const ms::Model& md) {
    return md.state == ms::State::Exploring;
}

EdenDsmodBool DoAction(Module& m, std::string_view a, std::int64_t arg) {
    const auto& md = m.model;
    const auto& b = md.battle;
    m.fb_next = FeedbackKey(a, arg, m.swap_mode);
    if (a == "team_focus") {
        if (arg < 0 || arg >= 12)
            return EDEN_DSMOD_FALSE;
        if (m.swap_mode) {
            m.swap_mode = false;
            if (!FieldEdit(md))
                return EDEN_DSMOD_FALSE;
            const auto* x = At(md.party, m.team_focus.load());
            const auto* y = At(md.party, arg);
            if (!x || !y || !x->ptr || !y->ptr)
                return EDEN_DSMOD_FALSE;
            m.team_focus.store(static_cast<int>(arg));
            return B(Stage(m, {hm::Op::TeamSwap, 0, x->ptr, y->ptr}, "swap " + x->name + " <> " + y->name));
        }
        m.equip_slot = -1;
        m.team_focus.store(static_cast<int>(arg));
        return EDEN_DSMOD_TRUE;
    }
    if (a == "swap_mode") {
        if (!FieldEdit(md)) {
            m.swap_mode = false;
            return EDEN_DSMOD_FALSE;
        }
        m.swap_mode = !m.swap_mode;
        m.equip_slot = -1;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "eq_slot") {
        if (arg < 0 || arg > 5 || !FieldEdit(md))
            return EDEN_DSMOD_FALSE;
        m.swap_mode = false;
        m.equip_slot = m.equip_slot == arg ? -1 : static_cast<int>(arg);
        return EDEN_DSMOD_TRUE;
    }
    if (a == "eq_close") {
        m.equip_slot = -1;
        return EDEN_DSMOD_TRUE;
    }
    if (a == "eq_pick") {
        const auto& list = m.equip_slot < 2 ? md.weapons : md.accessories;
        const auto* e = At(list, arg);
        const auto* mon = At(md.party, m.team_focus.load());
        if (!FieldEdit(md)) {
            m.equip_slot = -1;
            return EDEN_DSMOD_FALSE;
        }
        if (m.equip_slot < 0 || !e || !mon || !mon->ptr)
            return EDEN_DSMOD_FALSE;
        const int slot = m.equip_slot;
        m.equip_slot = -1;
        return B(Stage(m, {hm::Op::Equip, 0, mon->ptr, e->ptr, static_cast<std::uint32_t>(slot)},
                       "equip " + e->name + " on " + mon->name + " slot " + std::to_string(slot)));
    }
    if (a == "follow") {
        const auto* mon = At(md.party, arg);
        if (!mon || !mon->ptr || !FieldEdit(md))
            return EDEN_DSMOD_FALSE;
        return B(Stage(m, {hm::Op::Follower, 0, mon->ptr}, "follower " + mon->name));
    }
    if (a == "tp") {
        if (!ms::CanTeleport(md))
            return EDEN_DSMOD_FALSE;
        const auto* t = At(md.map.teleports, arg); // the scrolling list: row = native index
        if (!t || !t->ptr)
            return EDEN_DSMOD_FALSE;
        return B(Stage(m, {hm::Op::Teleport, 0, t->ptr}, "teleport " + t->name));
    }
    if (a == "b_desc") { // a tap on the description strip: its next line now
        m.desc_taps.fetch_add(1);
        return EDEN_DSMOD_TRUE;
    }
    if (a.starts_with("b_") && !ms::CanBattleInput(md))
        return EDEN_DSMOD_FALSE;
    if (a == "b_cmd" || a == "b_skst") { // a skill row (expanded column) / its icon strip
        if (b.level_open || b.targeting)
            return EDEN_DSMOD_FALSE;
        const auto* s = ms::SkillAt(b, a == "b_cmd" ? 0 : 1, static_cast<int>(arg));
        if (!s)
            return EDEN_DSMOD_FALSE;
        ms::MenuRow r;
        r.name = s->name;
        r.column = s->column;
        r.row = s->row;
        r.item = s->item;
        return B(MenuPick(m, hm::MenuKind::Command, &r, "skill"));
    }
    if (a == "b_level") { // the native Y on the focused skill: its level popup
        const auto* s = At(b.skills, b.cursor);
        if (!s || !b.level_hint || b.level_open || b.targeting || !b.menu_open)
            return EDEN_DSMOD_FALSE;
        return B(Stage(m, {hm::Op::LevelOpen, 0, s->item, 0, 0, static_cast<std::uint32_t>(s->column),
                           static_cast<std::uint32_t>(s->row)},
                       "levels " + s->name));
    }
    if (a == "b_lvl") { // a level of the popup: the native A there (SelectAction of that level)
        // LevelSelectMenu.IsOpen stays 1 through the picked level's target select (the native
        // list is locked there): no level pick while targeting
        const auto* s = b.level_open && !b.targeting ? At(b.levels, arg) : nullptr;
        if (!s)
            return EDEN_DSMOD_FALSE;
        ms::MenuRow r;
        r.name = s->name;
        r.column = s->column;
        r.row = s->row;
        r.item = s->item;
        return B(MenuPick(m, hm::MenuKind::Level, &r, "level"));
    }
    if (a == "b_rail") { // the i-th command the game offers now (the top tab row)
        if (b.targeting || b.items_open || b.switch_open || !b.menu_open)
            return EDEN_DSMOD_FALSE;
        const auto rail = ms::VisibleRail(b);
        const auto* index = At(rail, arg);
        return B(index && MenuPick(m, hm::MenuKind::Command, At(b.defaults, *index), "command"));
    }

    if (a == "b_item") // slot * 8 + row: slot 0 the expanded column, 1.. the icon strips
        return B(MenuPick(m, hm::MenuKind::Items, ms::ItemAt(b, static_cast<int>(arg / 8), static_cast<int>(arg % 8)),
                          "item"));
    if (a == "b_switch")
        return B(MenuPick(m, hm::MenuKind::Switch, At(b.switches, arg), "switch"));
    if (a == "b_tgt" || a == "b_ally") {
        std::uint64_t ptr = 0;
        std::string name;
        if (a == "b_tgt") {
            if (const auto* e = At(b.enemies, arg)) {
                ptr = e->ptr;
                name = e->name;
            }
        } else if (const auto* al = At(b.allies, arg)) {
            ptr = al->ptr;
            name = al->name;
        }
        if (!ptr)
            return EDEN_DSMOD_FALSE;
        return B(Stage(m, {hm::Op::TargetPick, 0, ptr}, "target " + name));
    }
    if (a == "b_back") { // the target / Monster Info selector's Back: only while it is open
        if (!b.targeting)
            return EDEN_DSMOD_FALSE;
        return B(Stage(m, {hm::Op::CancelTarget}, "cancel target"));
    }
    if (a == "b_subback") { // the item window / Switch Monster list / level popup's Back: the native B there
        if (b.targeting || (!b.items_open && !b.switch_open && !b.level_open))
            return EDEN_DSMOD_FALSE;
        const auto kind = b.level_open ? hm::MenuKind::Level : b.items_open ? hm::MenuKind::Items : hm::MenuKind::Switch;
        return B(Stage(m, {hm::Op::MenuCancel, 0, 0, 0, static_cast<std::uint32_t>(kind)},
                       b.level_open ? "back levels" : b.items_open ? "back items" : "back switch"));
    }
    if (a == "b_info") { // Monster Info: show this ally (0..2) / enemy (8..10) on the native card
        if (!b.targeting || !b.info.open)
            return EDEN_DSMOD_FALSE;
        const ms::PartyMonster* al = arg < 8 ? At(b.allies, arg) : nullptr;
        const ms::Enemy* en = arg >= 8 ? At(b.enemies, arg - 8) : nullptr;
        const std::uint64_t ptr = al ? al->ptr : en ? en->ptr : 0;
        if (!ptr)
            return EDEN_DSMOD_FALSE;
        return B(Stage(m, {hm::Op::InfoPick, 0, ptr}, "info " + (al ? al->name : en->name)));
    }
    if constexpr (ms::Diagnostics) { // not in the shipped manifest: add them for a local audit
        if (a == "h_cache") { // party / map-room caches off and on (read-count A/B)
            m.reader.use_cache = !m.reader.use_cache;
            m.reader.Invalidate();
            Log(m, std::string{"reader caches "} + (m.reader.use_cache ? "on" : "off"));
            return EDEN_DSMOD_TRUE;
        }
        if (a == "h_detach") { // the Destroy restore path in a running session
            Detach(m);
            Log(m, "detached (as on close): menu and combo hide off");
            return EDEN_DSMOD_TRUE;
        }
    }
    return EDEN_DSMOD_FALSE;
}

void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static bool reported = false;
    try {
        if (!p || !host)
            return;
        auto& m = *static_cast<Module*>(p);
        if (m.samples++ % RefreshEvery == 0) {
            m.reader.want_owner_gear = m.equip_slot >= 0;
            // the title's own prompt object, as the helper last found it (no helper installed:
            // unknown; installed but not ticking yet -- the game's Update does not run under
            // Unity's start-up splash -- the word is still 0, so no prompt)
            m.reader.title_prompt_known = m.helper && !m.detached;
            m.reader.title_prompt_go = m.reader.title_prompt_known ? m.helper->Word64(hm::OffTitlePrompt) : 0;
            m.model = m.reader.Refresh(*host);
            m.model_at = NowMs(m);
            UpdateMotionGeometry(m);
            m.model_hb = m.helper ? m.helper->Heartbeat() : 0;
            if constexpr (ms::Diagnostics) {
                auto& st = m.read_stats[static_cast<int>(m.model.state)];
                ++st.refreshes;
                st.total += m.reader.LastReads();
                st.max = std::max(st.max, m.reader.LastReads());
                if (const auto ev = m.reader.TakeCacheEvent(); !ev.empty())
                    Log(m, ev);
                if (m.samples - m.read_stats_at >= 300) { // every ~5 s: reads per refresh by state
                    m.read_stats_at = m.samples;
                    std::string line = std::string{"reads/refresh cache="} + (m.reader.use_cache ? "on" : "off");
                    for (const auto& [state, st] : m.read_stats)
                        line += " st" + std::to_string(state) + ":n=" + std::to_string(st.refreshes) + ",avg=" +
                                std::to_string(st.total / std::max<std::uint64_t>(st.refreshes, 1)) + ",max=" +
                                std::to_string(st.max);
                    // module pictures rendered in those ~5 s (P3: the particle picture churn)
                    line += " pictures fx=" + std::to_string(m.fx_pictures.exchange(0)) +
                            " sparkle=" + std::to_string(m.sparkle_pictures.exchange(0)) +
                            " other=" + std::to_string(m.other_pictures.exchange(0));
                    Log(m, line);
                    m.read_stats.clear();
                }
            }
            {
                std::lock_guard lock{m.map_mutex};
                m.map = m.model.map; // invalidate cached images as soon as the map is unavailable
            }
            if (!FieldEdit(m.model)) {
                m.equip_slot = -1;
                m.swap_mode = false;
            }
            if (m.art && !m.clips_loaded) {
                m.clips = m.art->Clips();
                m.fx_sets = m.art->FxSets();
                m.clips_loaded = !m.clips.empty();
            }
            if (m.art && host->log)
                for (const auto& miss : m.art->TakeMissing())
                    host->log(host->userdata, EDEN_DSMOD_LOG_WARNING,
                              ("Monster Sanctuary art: no sprite \"" + miss + "\" in pictures.idx").c_str());
            if (ms::Diagnostics && m.reader.Debug() != m.last_debug && host->log) {
                m.last_debug = m.reader.Debug();
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                          ("Monster Sanctuary reader: " + m.last_debug).c_str());
            }
        }
        // follower write watch (diagnostics): log every change of Monster / NextMonster / State
        if (ms::Diagnostics && m.model.state == ms::State::Exploring) {
            const auto w = m.reader.FollowerWatch(*host);
            if (w != m.follower_watch && host->log) {
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                          ("Monster Sanctuary follower: " + w + " @" + std::to_string(m.samples)).c_str());
                m.follower_watch = w;
            }
        }
        m.reduce_motion = host->get_i64 && host->get_i64(host->userdata, "@flag:ms.reduce_motion", 0) != 0;
        if (host->get_i64) {
            const bool menu = host->get_i64(host->userdata, "@flag:ms.hide_menu", 1) != 0;
            const bool combo = host->get_i64(host->userdata, "@flag:ms.hide_combo", 0) != 0;
            if (menu != m.hide_menu || combo != m.hide_combo || m.samples == 1)
                Log(m, std::string{"hide: battle menu "} + (menu ? "on" : "off") + ", combo counter " +
                           (combo ? "on" : "off"));
            m.hide_menu = menu;
            m.hide_combo = combo;
        }
        const int focus = m.team_focus.load();
        std::unordered_map<std::string, std::string> texts;
        // the text_binds read back their values' current text (only those are kept)
        const auto bound = [&m](std::string_view k) {
            return std::binary_search(m.bind_values.begin(), m.bind_values.end(), k, std::less<>{});
        };
        const ms::Sink sink{
            [host, &texts, &m, &bound](std::string_view k, std::int64_t v) {
                m.key_buf.assign(k);
                host->publish_i64(host->userdata, m.key_buf.c_str(), v);
                if (bound(k))
                    texts[std::string{k}] = std::to_string(v);
            },
            [host, &texts, &m, &bound](std::string_view k, std::string_view v) {
                auto& text = m.text_buf;
                text.assign(v);
                if (m.art && (k == "b.tweak" || k == "b.tres")) {
                    text.clear();
                    while (!v.empty()) {
                        const auto comma = v.find(", ");
                        std::string name{v.substr(0, comma)};
                        for (auto& c : name)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        auto it = m.labels.find(name);
                        if (it == m.labels.end())
                            it = m.labels.emplace(name, m.art->String(name)).first;
                        const auto& native = it->second;
                        if (!text.empty())
                            text += ", ";
                        text += native.empty() ? std::string{v.substr(0, comma)} : native;
                        if (comma == std::string_view::npos)
                            break;
                        v.remove_prefix(comma + 2);
                    }
                }
                if (m.art && IsDescription(k))
                    if (const auto pg = PageDescription(m, *host, k, text); !pg.empty()) {
                        const std::string pk = std::string{k} + ".pg";
                        host->publish_text(host->userdata, pk.c_str(), pg.c_str());
                        if (bound(pk))
                            texts[pk] = pg;
                    }
                m.key_buf.assign(k);
                host->publish_text(host->userdata, m.key_buf.c_str(), text.c_str());
                if (bound(k))
                    texts[std::string{k}] = text;
            },
            [&m](const std::string& sprite, int salt) { return AnimFrame(m, sprite, salt); }};
        if (m.art) {
            // native strings: read once (the art lock can be held by a long decode), then cached
            const auto label = [&m](const char* name) {
                if (const auto it = m.labels.find(name); it != m.labels.end())
                    return it->second;
                if (m.samples % RefreshEvery != 1)
                    return std::string{};
                auto text = m.art->String(name);
                if (!text.empty())
                    m.labels.emplace(name, text);
                return text;
            };
            for (const auto* name : {"health", "mana", "combo", "damage", "completion", "items", "resist",
                                     "switch", "weak", "options", "on", "off", "exploration", "combat", "map", "team",
                                     "equipment", "weapon", "accessories", "skillpoints", "ability",
                                     "back", "close", "loading", "lvl", "enemyturn"})
                if (const auto text = label(name); !text.empty())
                    sink.text(std::string{"l."} + name, text);
            // Keep the accepted two-line banner. Split at the last word boundary before its centre;
            // each line goes through the existing font -> regular -> small -> ellipsis fit.
            const auto rule = label("turn1");
            const auto split = rule.rfind(' ', rule.size() / 2);
            if (split != std::string::npos) {
                sink.text("l.turn1a", rule.substr(0, split));
                sink.text("l.turn1b", rule.substr(split + 1));
            }
        }
        ms::Publish(m.model, static_cast<int>(focus), sink);
        ServiceHelper(m, *host);
        PublishActions(m, focus, sink);
        PublishMotion(m, sink, host->get_text ? host->get_text(host->userdata, "__page") : nullptr);
        for (const auto& t : m.text_binds)
            if (const auto it = texts.find(t.value); it != texts.end() && !it->second.empty())
                host->publish_text(host->userdata, t.name.c_str(), (t.prefix + it->second).c_str());
    } catch (...) {
        if (!reported && host && host->log) {
            reported = true;
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "Monster Sanctuary sample failed");
        }
    }
}

void TickCallback(void*, const EdenDsmodHostApi*) {}

void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    if (!p)
        return;
    auto& m = *static_cast<Module*>(p);
    m.helper.reset();
    // The host provides a mailbox only when the package's load plan matched this build and its
    // helper code was installed.
    if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->struct_size == sizeof(*ext) &&
        ext->abi_hash == EDEN_DSMOD_EXT_HASH && ext->mailbox_address &&
        ext->mailbox_size >= hm::MailboxSize && ext->load_u32 && ext->load_u64 && ext->store_u32 &&
        ext->store_u64) {
        const HelperIo io{ext->userdata, ext->load_u32, ext->load_u64, ext->store_u32, ext->store_u64};
        m.helper.emplace(L32{io}, L64{io}, S32{io}, S64{io});
        const auto epoch = m.helper->Epoch();
        if (!m.helper->Initialize(epoch ? epoch : 1))
            m.helper.reset();
    }
    Log(m, m.helper ? "mailbox configured" : "no mailbox (helper not installed for this build)");
}

EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        if (!p || !action)
            return EDEN_DSMOD_FALSE;
        return DoAction(*static_cast<Module*>(p), action, argument);
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    try {
        if (!p || !key || !sink)
            return EDEN_DSMOD_FALSE;
        auto& m = *static_cast<Module*>(p);
        const std::string_view k{key};
        // module:ms:tpa:<signature>:<scene>  a repeated area's crystal: where it sits in its area
        if (k.starts_with("module:ms:tpa:")) {
            const auto rest = k.substr(14);
            const auto colon = rest.find(':');
            if (colon == std::string_view::npos)
                return EDEN_DSMOD_FALSE;
            ms::MapInfo map;
            {
                std::lock_guard lock{m.map_mutex};
                map = m.map;
            }
            if (!map.valid || std::to_string(ms::MapSignature(map)) != rest.substr(0, colon))
                return EDEN_DSMOD_FALSE; // stale key: the host asks again for the new one
            const auto pic = ms::DrawTeleporterArea(map, rest.substr(colon + 1), TeleportMiniW, TeleportMiniH);
            sink(receiver, pic.width, pic.height, pic.rgba.data(), pic.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        // module:ms:map:<L|S>:<signature>  L = Map page (800x704), S = Exploration page (584x252)
        if (k.starts_with("module:ms:tpfx:")) { // module:ms:tpfx:<step>:<cell px>
            if constexpr (ms::Diagnostics)
                ++m.sparkle_pictures;
            int step{}, cell{};
            if (!m.art || std::sscanf(key + 15, "%d:%d", &step, &cell) != 2 || cell < 1 || cell > 48 || step < 0)
                return EDEN_DSMOD_FALSE;
            // the sparkle at native pixels (a 2 x 3 tile window, the tile's centre at (8, 16)),
            // then nearest-scaled to cell / 8 per native px into the fixed box
            const auto native = m.art->FxPicture("maptp", static_cast<double>(step) / ms::FxSteps, -1, 16, 24, 8, 16, 1);
            if (!native)
                return EDEN_DSMOD_FALSE;
            ms::Picture pic;
            pic.width = SparkleBoxW;
            pic.height = SparkleBoxH;
            pic.rgba.assign(static_cast<std::size_t>(pic.width) * pic.height * 4, 0);
            const double f = cell / 8.0;
            for (std::uint32_t y = 0; y < pic.height; ++y)
                for (std::uint32_t x = 0; x < pic.width; ++x) {
                    const int nx = static_cast<int>(std::floor((static_cast<double>(x) - SparkleBoxW / 2.0) / f + 8));
                    const int ny = static_cast<int>(std::floor((static_cast<double>(y) - SparkleTileY) / f + 16));
                    if (nx < 0 || ny < 0 || nx >= 16 || ny >= 24)
                        continue;
                    std::copy_n(&native->rgba[(static_cast<std::size_t>(ny) * 16 + nx) * 4], 4,
                                &pic.rgba[(static_cast<std::size_t>(y) * pic.width + x) * 4]);
                }
            sink(receiver, pic.width, pic.height, pic.rgba.data(), pic.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (k.starts_with("module:ms:dot:")) { // module:ms:dot:<phase>:<cell px>:<box px>
            int phase{}, cell{}, box{};
            if (std::sscanf(key + 14, "%d:%d:%d", &phase, &cell, &box) != 3 || cell < 1 || cell > 64 ||
                box < 4 || box > 128)
                return EDEN_DSMOD_FALSE;
            const auto pic = ms::DrawPlayerDot(phase, DotSteps, static_cast<std::uint32_t>(cell),
                                               static_cast<std::uint32_t>(box));
            sink(receiver, pic.width, pic.height, pic.rgba.data(), pic.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (!k.starts_with("module:ms:map:") || k.size() < 16) {
            if constexpr (ms::Diagnostics)
                ++(k.find('@') != std::string_view::npos ? m.fx_pictures : m.other_pictures);
            const auto pic = m.art ? m.art->Load(k) : std::nullopt;
            if (!pic || pic->rgba.empty())
                return EDEN_DSMOD_FALSE;
            sink(receiver, pic->width, pic->height, pic->rgba.data(), pic->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        const char view = k[14];
        ms::MapInfo map;
        {
            std::lock_guard lock{m.map_mutex};
            map = m.map;
        }
        if (!map.valid || std::to_string(ms::MapSignature(map)) != k.substr(16))
            return EDEN_DSMOD_FALSE; // stale key: the host asks again for the new one
        ms::Picture pic;
        if (view == 'S') {
            pic = ms::DrawMap(map, 584, 252, 16);
        } else {
            const auto* def = view == 'W' ? &MapViews[1] : view == 'L' ? &MapViews[0] : nullptr;
            if (!def)
                return EDEN_DSMOD_FALSE;
            pic = ms::DrawMap(map, def->w * MapScale, def->h * MapScale, MapCell * MapScale, true);
            if (m.art)
                ms::DrawMapIcons(pic, map, ms::ViewOf(map, def->w * MapScale, def->h * MapScale, MapCell * MapScale, true),
                                 [&](std::string_view name) { return m.art->Load("module:ms:spr1:" + std::string{name}); },
                                 !m.art->FxSets().count("maptp")); // animated over the picture instead
        }
        sink(receiver, pic.width, pic.height, pic.rgba.data(), pic.rgba.size());
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
                                   "Monster Sanctuary DSMod",
                                   EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_EXTENSIONS |
                                       EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};

EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t*, size_t, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        auto* m = static_cast<Module*>(p);
        if (!m || !m->art || !sink)
            return EDEN_DSMOD_FALSE;
        std::uint32_t first{}, line{};
        std::vector<EdenDsmodFontGlyph> glyphs;
        if (!m->art->Font(first, glyphs, line) || glyphs.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, line, first, glyphs.data(), static_cast<std::uint32_t>(glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};

const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    &ConfigureCallback,     &ActionCallback,                   &LoadImageCallback};

} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
