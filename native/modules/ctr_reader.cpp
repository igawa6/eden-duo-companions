// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ctr_reader.h"

#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

// CTR_DSMOD_DIAGNOSTICS=1 (developer builds only) publishes console-check values that no
// manifest widget reads: ctr.page/hud/hudw/hudo/hudh/dhud/dmap/dlead/count/widget/widget_used/track
#ifdef CTR_DSMOD_DIAGNOSTICS
#define CTR_DIAG(...) __VA_ARGS__
#else
#define CTR_DIAG(...)
#endif

namespace CtrReader {
namespace {

using namespace dsmod_sdk::int_types;

constexpr u64 OffRacerManager = 0x4D64F10;
constexpr u64 OffClient = 0x4D44710;
constexpr u64 OffRaceSettings = 0x4D65250;
constexpr u64 OffMinimapVtable = 0x4909390 + 0x10; // _ZTV25CGuiBehaviorRacingMinimap + 16
constexpr u64 OffRaceClockVtable = 0x4955910 + 0x10; // _ZTV19CRaceClockComponent + 16
constexpr u64 OffTimerVtable = 0x4962788 + 0x10;     // _ZTV6CTimer + 16
constexpr u64 OffClock = 0x4D44798;                  // CClock::_Instance; +0x20 game time (1/8192 s)
constexpr u64 OffLeadersVtable = 0x4909B68 + 0x10;   // _ZTV23CGuiBehaviorRaceLeaders + 16
constexpr u64 OffRacerHudVtable = 0x494D590 + 0x10;  // _ZTV18CRacerHudComponent + 16
constexpr u64 OffPlaceableVtable = 0x47EFF58 + 0x10; // _ZTVN3Gui14igGuiPlaceableE + 16
// COctaneGlobalOptionsData._showMinimap (u8 +0x21): [main+0x4D5D7F8] +0xDB0 -> +0x78. The game
// reads it when it builds a race HUD (writing it mid-race changes nothing)
constexpr u64 OffOptionsRoot = 0x4D5D7F8;
constexpr u64 ScanPagesPerSample = 2048;           // 8 MiB of heap per sample
constexpr u64 ScanFocusHalf = 32ull << 20;          // the first pass's window around the HUD project
// the scanned vtables all lie in [minimap, race clock]: one range test rejects almost every word
constexpr u64 ScanVtablesLo = OffMinimapVtable, ScanVtablesSpan = OffRaceClockVtable - OffMinimapVtable;
constexpr int MissesBeforeRaceGone = 20;           // samples a failed racer read is bridged
constexpr int HudApplyEvery = 10;                  // samples between HUD hide checks (~6/s)
constexpr int MinimapOptionHold = 60 * 60;          // samples after loading without a race clock
constexpr int CardSlideSamples = 18;               // a rank card's slide (~0.3 s at 60 samples/s)
constexpr float CardBulgePx = 34;                  // how far an overtaking card swings out

/// Fixed published names (built once; Publish runs every sample).
struct Keys {
    struct Row { ///< a position slot: its number
        std::string v, me;
    };
    struct Card { ///< a racer's sliding card (frame tab, portrait, name, finish flag)
        std::string v, x, y, me, fin, p, n;
    };
    struct Marker {
        std::string v, x, y, h;
    };
    std::array<Row, MaxRacers> rows;
    std::array<Card, MaxRacers> cards;
    std::array<Marker, MaxRacers> markers;
    std::array<std::string, TimeSlots> slot, slot_v;
    Keys() {
        for (int k = 0; k < MaxRacers; ++k) {
            const std::string r = "rw" + std::to_string(k) + ".", c = "rc" + std::to_string(k) + ".",
                              m = "mk" + std::to_string(k) + ".";
            rows[k] = {r + "v", r + "me"};
            cards[k] = {c + "v", c + "x", c + "y", c + "me", c + "fin", c + "p", c + "n"};
            markers[k] = {m + "v", m + "x", m + "y", m + "h"};
        }
        for (int k = 0; k < TimeSlots; ++k) {
            slot[k] = "tm.c" + std::to_string(k);
            slot_v[k] = slot[k] + "v";
        }
    }
};
const Keys& PublishKeys() {
    static const Keys keys;
    return keys;
}

/// "octane/t111_crash_cove/t111_crash_cove" -> "t111_crash_cove" for race tracks (t###) and
/// battle arenas (a###); empty otherwise (hub, menus).
std::string TrackOf(const std::string& level) {
    constexpr std::string_view prefix = "octane/";
    if (level.compare(0, prefix.size(), prefix) != 0)
        return {};
    const std::size_t start = prefix.size();
    const std::size_t end = level.find('/', start);
    std::string t = level.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (t.size() < 6 || (t[0] != 't' && t[0] != 'a') || !std::isdigit(static_cast<unsigned char>(t[1])) ||
        !std::isdigit(static_cast<unsigned char>(t[2])) || !std::isdigit(static_cast<unsigned char>(t[3])) ||
        t[4] != '_')
        return {};
    for (const char c : t)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return {};
    return t;
}

std::string Upper(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x80)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// "module:ctr:tdigit:<0-9>" / ":colon", built once.
const std::string& TimeDigitKey(char c) {
    static const std::array<std::string, 11> keys = [] {
        std::array<std::string, 11> k;
        for (int i = 0; i < 10; ++i)
            k[i] = "module:ctr:tdigit:" + std::string(1, static_cast<char>('0' + i));
        k[10] = "module:ctr:tdigit:colon";
        return k;
    }();
    return c >= '0' && c <= '9' ? keys[c - '0'] : keys[10];
}

bool KeySafe(const std::string& s) {
    if (s.empty() || s.size() > 96)
        return false;
    for (const char c : s)
        if (c == ':' || c == '\0' || static_cast<unsigned char>(c) < 0x20)
            return false;
    return true;
}

/// "<prefix><driver>[:<outfit>]" (portrait / head pictures), rebuilt only when the slot's racer
/// strings change.
const std::string& RacerArtKey(ArtKey& k, std::string_view prefix, const std::string& driver,
                               const std::string& outfit) {
    if (k.Stale(prefix, driver, outfit))
        k.value = std::string(prefix) + driver + (KeySafe(outfit) ? ":" + outfit : "");
    return k.value;
}

} // namespace

bool Reader::Read(u64 at, void* out, std::size_t size) const {
    return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(*host, at, out, size);
}

std::string Reader::String(u64 at, std::size_t max) const {
    std::string s;
    char buf[32];
    while (s.size() < max) {
        // chunks never cross a guest page: a string ending just before an unmapped page reads
        const u64 at_now = at + s.size();
        const std::size_t n = std::min({sizeof buf, std::size_t{0x1000 - (at_now & 0xFFF)}, max - s.size()});
        if (!Read(at_now, buf, n))
            break;
        if (const void* nul = std::memchr(buf, 0, n)) {
            s.append(buf, static_cast<const char*>(nul) - buf);
            return s;
        }
        s.append(buf, n);
    }
    return {};
}

u64 Reader::Static(u64 off) const {
    const u64 a = host->main_base + off;
    if (static_variant == 1)
        return Get<u64>(a + static_cast<u64>(delta));
    return Get<u64>(a);
}

bool Reader::LearnStaticVariant() {
    // which static address form holds the game's singletons (Dynarmic: main+off; NCE: + delta):
    // the racer manager's list, or the client's level name ("octane/..."), which exists from the
    // title screen on (the loading-screen state and the minimap option are read before any race)
    for (int v = 0; v < 2; ++v) {
        if (v == 1 && delta == 0)
            break;
        const u64 shift = v ? static_cast<u64>(delta) : 0;
        const u64 mgr = Get<u64>(host->main_base + OffRacerManager + shift);
        const u64 list = mgr ? Get<u64>(mgr + 0x10) : 0;
        const bool racers = list && Get<u32>(list + 0x0C, 99) <= MaxRacers;
        const u64 client = Get<u64>(host->main_base + OffClient + shift);
        const bool level = client && String(Get<u64>(client + 0xC0)).starts_with("octane/");
        if (racers || level) {
            static_variant = v;
            if (host->log)
                host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                          (std::string("CTR DSMod: statics at main+off") + (v ? "+delta" : "") + " (" +
                           (racers ? "racer manager" : "client level") + ")").c_str());
            return true;
        }
    }
    return false;
}

bool Reader::ReadRacers(std::array<Racer, MaxRacers>& out, int& found, u64& identity) {
    found = 0;
    identity = 0;
    if (static_variant < 0 && !LearnStaticVariant())
        return false;
    const u64 mgr = Static(OffRacerManager);
    const u64 list = mgr ? Get<u64>(mgr + 0x10) : 0;
    const u32 n = list ? Get<u32>(list + 0x0C, 99) : 99;
    const u64 items = list ? Get<u64>(list + 0x20) : 0;
    if (n == 0 || n > MaxRacers || !items)
        return false;
    for (u32 i = 0; i < n; ++i) {
        Racer& r = out[found];
        r = {};
        const u64 address = Get<u64>(items + 8ull * i);
        r.address = address;
        if (!address)
            continue;
        // the race's racer objects, order-independent (the list is re-sorted as positions change)
        identity += (address ^ (address >> 29)) * 0xBF58476D1CE4E5B9ull;
        u8 block[0x80];
        if (!Read(address + 0x10, block, sizeof block))
            continue;
        u64 kart_handle, driver_handle;
        std::memcpy(&kart_handle, block + 0x20 - 0x10, 8);
        std::memcpy(&driver_handle, block + 0x30 - 0x10, 8);
        r.ai = block[0x40 - 0x10] != 0;
        s32 lap, pos, race_pos;
        std::memcpy(&lap, block + 0x4C - 0x10, 4);
        std::memcpy(&pos, block + 0x64 - 0x10, 4);
        // _officialPosition is 0 until the green light; the race position (+0x58, the grid order
        // during the intro and countdown, then equal to the official one) covers the start
        std::memcpy(&race_pos, block + 0x58 - 0x10, 4);
        if (pos < 1 || pos > static_cast<s32>(n))
            pos = race_pos;
        r.finished = block[0x8C - 0x10] != 0;
        r.lap = std::clamp<s32>(lap, -1, 99);
        r.position = pos;
        if (pos < 1 || pos > static_cast<s32>(n))
            continue;
        if (const u64 ent = kart_handle ? Get<u64>(kart_handle + 0x28) : 0) {
            float xy[2];
            if (Read(ent + 0x20, xy, sizeof xy) && std::isfinite(xy[0]) && std::isfinite(xy[1]) &&
                std::abs(xy[0]) < 1e6f && std::abs(xy[1]) < 1e6f) {
                r.x = xy[0];
                r.y = xy[1];
                r.has_pos = true;
            }
        }
        // the equipped outfit's first material package (its skin portrait is named there)
        u64 outfit_handle;
        std::memcpy(&outfit_handle, block + 0x38 - 0x10, 8);
        if (const u64 outfit = outfit_handle ? Get<u64>(outfit_handle + 0x28) : 0) {
            auto it = outfits.find(outfit);
            if (it == outfits.end()) {
                std::string pkg;
                if (const u64 list = Get<u64>(outfit + 0x28); list && Get<u32>(list + 0x0C) > 0)
                    if (const u64 items = Get<u64>(list + 0x20))
                        if (const u64 item = Get<u64>(items))
                            pkg = String(Get<u64>(item + 0x10));
                if (pkg.rfind("Driver", 0) != 0 || pkg.size() > 96)
                    pkg.clear();
                if (outfits.size() > 64)
                    outfits.clear();
                it = outfits.emplace(outfit, std::move(pkg)).first;
            }
            r.outfit = it->second;
        }
        if (const u64 data = driver_handle ? Get<u64>(driver_handle + 0x28) : 0) {
            auto it = names.find(data);
            if (it == names.end()) {
                DriverNames dn{String(Get<u64>(data + 0x58)), String(Get<u64>(data + 0x60))};
                if (dn.name.rfind("Driver", 0) != 0)
                    dn = {};
                if (names.size() > 64)
                    names.clear();
                it = names.emplace(data, std::move(dn)).first;
            }
            r.driver = it->second.name;
            r.display = it->second.display;
        }
        ++found;
    }
    // positions are unique within 1..n (a racer that could not be read leaves a hole); a
    // duplicate is a torn read mid-overtake: the caller keeps the previous sample
    std::array<bool, MaxRacers + 1> seen{};
    for (int i = 0; i < found; ++i) {
        const int p = out[i].position;
        if (seen[p])
            return found = 0, false;
        seen[p] = true;
    }
    return found > 0;
}

void Reader::ScanForWidget() {
    const auto restart = [this] {
        scan_cursor = (host->get_heap_begin ? host->get_heap_begin(host->userdata) : 0) & ~u64{0xFFF};
        scan_end = host->get_heap_end ? host->get_heap_end(host->userdata) : 0;
        pass_widgets.clear();
        pass_clocks.clear();
        pass_leaders.clear();
        // the minimap and leaders behaviours are allocated with the HUD's GUI projects: once the
        // player's HUD project is known, look around it first (a 64 MiB window), then everywhere
        scan_focus = hud_project && ++focus_passes % 4 != 0;
        if (scan_focus) {
            scan_cursor = std::max(scan_cursor, (hud_project - ScanFocusHalf) & ~u64{0xFFF});
            scan_end = std::min(scan_end, hud_project + ScanFocusHalf);
        }
    };
    if (scan_race != race_id || scan_track != track) {
        // a new race (another track, or the same one restarted: new racer objects): its HUD
        // objects are new too, and freed driver data may be reused at the old addresses
        scan_race = race_id;
        scan_track = track;
        widget_found = false;
        widget_addr = 0;
        clock_component = 0;
        hud_nodes = {};
        std::fill(std::begin(hud_release), std::end(hud_release), u64{0});
        hud_found = false;
        hud_project = 0;
        scan_focus = false;
        focus_passes = 0;
        extra_passes = 0;
        names.clear();
        outfits.clear();
        restart();
    }
    if (track.empty())
        return;
    // keep a found race clock only while it still is one
    if (clock_component) {
        const u64 v = Get<u64>(clock_component), want = host->main_base + OffRaceClockVtable;
        if (v != want && v != want + static_cast<u64>(delta))
            clock_component = 0;
    }
    // a race built while we held the minimap option off has no minimap widget to wait for
    const bool map_done = widget_found || !MinimapExpected();
    if (map_done && clock_component && (hud_found || extra_passes >= 8))
        return;
    if (hud_project && !scan_focus && focus_passes == 0)
        restart(); // the HUD just appeared: search its neighbourhood now
    if (scan_cursor >= scan_end) {
        // the HUD creates its widgets and race clock when the race starts (after the intro
        // flyby): an incomplete pass is repeated after a short pause, at once while only the
        // HUD parts to hide are missing (their settings apply from the race start)
        const bool only_hud_missing = map_done && clock_component && !hud_found;
        // the minimap and leaders appear some time after the HUD: keep looking around it (every
        // fourth pass covers the whole heap) until they are found
        const bool after_focus = std::exchange(scan_focus, false) || (hud_project && !hud_found);
        if (!only_hud_missing && !after_focus && ++scan_idle < 120)
            return;
        scan_idle = 0;
        restart();
    }
    const u64 d = static_cast<u64>(delta);
    const u64 vw = host->main_base + OffMinimapVtable, vc = host->main_base + OffRaceClockVtable;
    const u64 vl = host->main_base + OffLeadersVtable;
    const u64 lo = host->main_base + ScanVtablesLo;
    u64 page_buf[512];
    for (u64 k = 0; k < ScanPagesPerSample && scan_cursor < scan_end; ++k, scan_cursor += 0x1000) {
        const u64* words = nullptr;
        if (host->get_read_pointer)
            words = reinterpret_cast<const u64*>(host->get_read_pointer(host->userdata, scan_cursor, 0x1000));
        if (!words) {
            if (!host->is_mapped || !host->is_mapped(host->userdata, scan_cursor, 0x1000) ||
                !host->read_memory(host->userdata, scan_cursor, page_buf, sizeof page_buf))
                continue;
            words = page_buf;
        }
        for (u64 w = 0; w < 512; ++w) {
            const u64 v = words[w];
            if (v - lo > ScanVtablesSpan && v - (lo + d) > ScanVtablesSpan)
                continue;
            if ((v == vc || v == vc + d) && pass_clocks.size() < 16)
                pass_clocks.push_back(scan_cursor + w * 8);
            if ((v == vl || v == vl + d) && pass_leaders.size() < 16)
                pass_leaders.push_back(scan_cursor + w * 8);
            if (v != vw && v != vw + d)
                continue;
            u8 obj[0x48];
            if (!Read(scan_cursor + w * 8, obj, sizeof obj))
                continue;
            float tl[2], br[2];
            s32 dir;
            std::memcpy(tl, obj + 0x30, 8);
            std::memcpy(br, obj + 0x38, 8);
            std::memcpy(&dir, obj + 0x40, 4);
            if (!std::isfinite(tl[0]) || !std::isfinite(tl[1]) || !std::isfinite(br[0]) ||
                !std::isfinite(br[1]) || std::abs(tl[0] - br[0]) < 1000 || std::abs(tl[1] - br[1]) < 1000 ||
                dir < 0 || dir > 3)
                continue;
            if (pass_widgets.size() < 8)
                pass_widgets.push_back({scan_cursor + w * 8, {br[0], br[1], tl[0], tl[1]}, dir});
        }
    }
    if (scan_cursor < scan_end)
        return;
    // pass complete
    if (map_done && clock_component)
        ++extra_passes;
    if (!pass_widgets.empty() && !widget_found)
        UseWidget(pass_widgets.back()); // provisional: ResolveHudNodes prefers the player's own
    if (!hud_found)
        ResolveHudNodes();
    if (!clock_component) {
        // several race clocks may exist (earlier races): take the one whose timer runs, else
        // the one started last
        u64 best = 0;
        s64 best_score = -1;
        for (const u64 c : pass_clocks) {
            const u64 timer = Get<u64>(c + 0x28);
            const u64 tv = timer ? Get<u64>(timer) : 0;
            if (tv != host->main_base + OffTimerVtable && tv != host->main_base + OffTimerVtable + d)
                continue;
            const s64 score = (Get<u8>(timer + 0x14) ? (s64{1} << 40) : 0) + Get<u32>(timer + 0x0C);
            if (score > best_score)
                best_score = score, best = c;
        }
        clock_component = best;
    }
}

bool Reader::IsPlaceable(u64 node) const {
    if (!node)
        return false;
    const u64 v = Get<u64>(node), want = host->main_base + OffPlaceableVtable;
    return v == want || v == want + static_cast<u64>(delta);
}

u64 Reader::HandleObject(u64 handle) const {
    // igHandle record: the object at +0x28 unless the record is aliased/redirected
    if (!handle || (Get<u32>(handle) & 0x30000000u))
        return 0;
    return Get<u64>(handle + 0x28);
}

u64 Reader::PlayerHudComponent() const {
    return PlayerComponent(OffRacerHudVtable);
}

u64 Reader::PlayerComponent(u64 vtable_off) const {
    // the player's kart entity owns its race components (CRacerHudComponent, ...): CRacer +0x20
    // kart handle -> entity, entity +0x18 component list (count +0x0C, items +0x20); no heap
    // search needed
    if (!player_address)
        return 0;
    const u64 entity = HandleObject(Get<u64>(player_address + 0x20));
    const u64 list = entity ? Get<u64>(entity + 0x18) : 0;
    const u32 n = list ? Get<u32>(list + 0x0C) : 0;
    const u64 items = list ? Get<u64>(list + 0x20) : 0;
    if (!items || n == 0 || n > 128)
        return 0;
    u64 components[128];
    if (!Read(items, components, 8ull * n))
        return 0;
    const u64 want = host->main_base + vtable_off;
    for (u32 i = 0; i < n; ++i)
        if (const u64 v = components[i] ? Get<u64>(components[i]) : 0; v == want || v == want + static_cast<u64>(delta))
            return components[i];
    return 0;
}

void Reader::UseWidget(const WidgetCandidate& w) {
    std::copy(w.v, w.v + 4, widget_v);
    widget_dir = w.dir;
    widget_addr = w.address;
    widget_found = true;
}

void Reader::ResolveHudNodes() {
    // Several copies of each HUD object sit in the heap (one racer HUD component per racer,
    // leaders columns of earlier races and menus; under NCE the first in heap order is often
    // not the live one). Live picks, checked on desktop 1.0.15:
    //   CRacerHudComponent._racer (+0x28, a handle) -> the player's CRacer
    //   minimap widget: its map sprite's parent sits in a loaded project (+0x20 != 0) and its
    //     _racerHud icon list (+0x10) has one icon per racer
    //   leaders column: _hasRaceStarted (+0x30), its items in a loaded project, one per racer
    // Unlinked copies are only a fallback (and keep the search going).
    HudNodes n{};
    CTR_DIAG(diag = {};)
    const auto links = [this](u64 field, u64 target) {
        return target && field && (field == target || HandleObject(field) == target);
    };
    // the player's racer HUD: project -> root -> composition -> Root_placeable children
    // [0] timer, [3] lap, [4] position (checked by their layout heights)
    u64 hud = 0, hud_children = 0;
    bool hud_linked = false;
    for (const u64 c : {PlayerHudComponent()}) {
        if (!c)
            continue;
        const u64 project = HandleObject(Get<u64>(c + 0x38));
        const u64 root = project ? Get<u64>(project + 0x10) : 0;
        if (!IsPlaceable(root))
            continue;
        const u64 comp = HandleObject(Get<u64>(root + 0x38));
        const u64 rp = comp ? Get<u64>(comp + 0x20) : 0;
        if (!IsPlaceable(rp))
            continue;
        const u64 table = Get<u64>(rp + 0x48);
        const u32 kids = table ? Get<u32>(table + 0x0C) : 0;
        const u64 data = table ? Get<u64>(table + 0x20) : 0;
        if (kids < 5 || kids > 64 || !data)
            continue;
        CTR_DIAG(++diag.huds;)
        const bool linked = links(Get<u64>(c + 0x28), player_address);
        CTR_DIAG(diag.huds_linked += linked ? 1 : 0;)
        if ((linked && !hud_linked) || !hud) {
            hud = c, hud_children = data;
            hud_linked = linked;
            hud_project = project;
        }
    }
    if (hud) {
        const auto child = [&](int i, float y) -> u64 {
            const u64 p = Get<u64>(hud_children + 8ull * i);
            const float py = IsPlaceable(p) ? Get<float>(p + 0x64, -1e9f) : -1e9f;
            return std::abs(py - y) < 60 ? p : 0;
        };
        n.node[HudTime] = child(0, 138);
        n.node[HudLap] = child(3, 93);
        n.node[HudPosition] = child(4, 961);
    }
    // minimap: the live widget (its bounds also place the bottom map's markers)
    const auto in_project = [this](u64 placeable) {
        return IsPlaceable(placeable) && Get<u64>(placeable + 0x20) != 0;
    };
    bool map_live = false;
    for (const WidgetCandidate& w : pass_widgets) {
        CTR_DIAG(++diag.maps;)
        const u64 sprite = Get<u64>(w.address + 0x18);
        const u64 icons = Get<u64>(w.address + 0x10);
        const bool live = in_project(sprite) && in_project(Get<u64>(sprite + 0x40)) && icons &&
                          static_cast<int>(Get<u32>(icons + 0x0C)) == count;
        if (!live)
            continue;
        CTR_DIAG(++diag.maps_linked;)
        if (!map_live && w.address != widget_addr)
            UseWidget(w);
        map_live = true;
    }
    // the widget's map sprite's parent (Null_Asset_Minimap)
    if (widget_addr)
        if (const u64 sprite = Get<u64>(widget_addr + 0x18); IsPlaceable(sprite))
            if (const u64 owner = Get<u64>(sprite + 0x40); IsPlaceable(owner))
                n.node[HudMap] = owner;
    // leaders column: the started race's behaviour, items in a loaded project -> their parent
    bool leaders_live = false;
    for (const u64 b : pass_leaders) {
        const u64 list = Get<u64>(b + 0x10);
        const u32 items = list ? Get<u32>(list + 0x0C) : 0;
        if (items == 0 || items > 16)
            continue;
        const u64 item = Get<u64>(Get<u64>(list + 0x20));
        const u64 master = in_project(item) ? Get<u64>(item + 0x40) : 0;
        if (!IsPlaceable(master))
            continue;
        CTR_DIAG(++diag.leaders;)
        const bool live = Get<u8>(b + 0x30) != 0 && static_cast<int>(items) == count;
        CTR_DIAG(diag.leaders_linked += live ? 1 : 0;)
        if (live || !n.node[HudLeaders])
            n.node[HudLeaders] = master;
        if (live) {
            leaders_live = true;
            break;
        }
    }
    // keep nodes found earlier that are still placeables (a new pass starts with empty
    // candidate lists; dropping a node we hid would strand it hidden) -- unless a linked pick
    // replaced them
    for (int e = 0; e < HudCount; ++e)
        if (!n.node[e] && IsPlaceable(hud_nodes.node[e]))
            n.node[e] = hud_nodes.node[e];
    // a node we hid that is no longer ours to manage (a better pick replaced it): give it back;
    // a failed write keeps it pending, retried by ApplyHud like a failed SHOW
    for (int e = 0; e < HudCount; ++e)
        if (hud_ours[e] && hud_ours[e] != n.node[e]) {
            const u64 old = std::exchange(hud_ours[e], 0);
            if (hud_release[e] && hud_release[e] != old)
                ShowNode(hud_release[e]); // an older pending one: one more best-effort try
            hud_release[e] = ShowNode(old) ? 0 : old;
        }
    hud_nodes = n;
    // complete only when the picks are the player's own (an unlinked fallback keeps looking)
    // (a race built without the minimap needs no map node)
    const bool map_ok = (n.node[HudMap] && map_live) || !MinimapExpected();
    hud_found = map_ok && n.node[HudLeaders] && n.node[HudTime] && n.node[HudLap] && n.node[HudPosition] &&
                hud_linked && leaders_live;
}

void Reader::SetHudHidden(int element, bool hide) {
    if (element >= 0 && element < HudCount)
        hud_want[element] = hide;
}

bool Reader::ShowNode(u64 node) const {
    // clears the hidden bit this module set (bit 10 off, bit 12 dirty); true when nothing is
    // left to give back (shown, not hidden, or no longer a placeable)
    if (!IsPlaceable(node))
        return true;
    u8 b = Get<u8>(node + 0x0D);
    if (!(b & 0x04))
        return true;
    b = static_cast<u8>((b & ~0x04) | 0x10);
    return host->write_memory && host->write_memory(host->userdata, node + 0x0D, &b, 1);
}

void Reader::ApplyHud() {
    // the game's own hidePlaceable: bitfield +0x0C bit 10 (hidden) + bit 12 (dirty); hiding a
    // parent hides its subtree. Only bits this module set are ever cleared again.
    CTR_DIAG(hud_hidden = 0;)
    if (!host->write_memory)
        return;
    for (int e = 0; e < HudCount; ++e) {
        const u64 node = hud_nodes.node[e];
        // a replaced node whose give-back write failed: retry, or own it again if it is the
        // pick again
        if (hud_release[e] && hud_release[e] == node)
            hud_ours[e] = std::exchange(hud_release[e], 0);
        else if (hud_release[e] && ShowNode(hud_release[e]))
            hud_release[e] = 0;
        if (!IsPlaceable(node)) {
            hud_ours[e] = 0;
            continue;
        }
        u8 b = Get<u8>(node + 0x0D);
        if (hud_want[e]) {
            // hidden by the game already (intro, results): ours too, so SHOW can undo it
            if (!(b & 0x04)) {
                b |= 0x14;
                host->write_memory(host->userdata, node + 0x0D, &b, 1);
            }
            hud_ours[e] = node;
        } else if (hud_ours[e] == node && (b & 0x04)) {
            b = static_cast<u8>((b & ~0x04) | 0x10);
            if (host->write_memory(host->userdata, node + 0x0D, &b, 1))
                hud_ours[e] = 0;
        }
        CTR_DIAG(hud_hidden |= (b & 0x04) ? std::int64_t{1} << e : 0;)
    }
}

Reader::~Reader() {
    RestoreHud();
    RestoreMinimapOption();
}

u64 Reader::MinimapOptionAddress() const {
    if (static_variant < 0)
        return 0;
    const u64 root = Static(OffOptionsRoot);
    const u64 holder = root ? Get<u64>(root + 0xDB0) : 0;
    const u64 options = holder ? Get<u64>(holder + 0x78) : 0;
    return options ? options + 0x21 : 0;
}

void Reader::SteerMinimapOption() {
    // With the minimap switch on HIDE, the next race is built without the native minimap: the
    // game's own minimap option is turned off while a loading screen is up and turned back on
    // once the race clock runs (the HUD is built by then), so the player's saved option stays
    // theirs. A race without a loading screen (Restart) is covered by hiding the node instead.
    // An option the player turned off themselves is never touched.
    const u64 at = MinimapOptionAddress();
    if (!at || !host->write_memory)
        return;
    const u8 now = Get<u8>(at, 0xFF);
    if (now > 1)
        return; // not the option we know
    if (minimap_option_off && at != minimap_option_address) {
        // A verified replacement context owns its own setting. Forget the old address without
        // writing through it, then let this loading sample acquire the new option if needed.
        minimap_option_off = false;
        minimap_option_address = 0;
    }
    if (minimap_option_off) {
        const bool running = race_timed && race_secs > 0;
        if (!hud_want[HudMap] || running || (!loading && ++minimap_option_wait > MinimapOptionHold))
            RestoreMinimapOption();
        else if (loading)
            minimap_option_wait = 0;
        return;
    }
    // only while loading a race track (t###): the adventure hub's map uses the same option
    if (loading && level_track.starts_with('t') && hud_want[HudMap] && now == 1) {
        const u8 off = 0;
        if (host->write_memory(host->userdata, at, &off, 1)) {
            minimap_option_off = true;
            minimap_option_address = at;
            minimap_option_wait = 0;
        }
    }
}

bool Reader::MinimapExpected() const {
    // the race whose loading screen we held the option off for has no native minimap; another
    // race (Restart, a later track) builds one again
    return !map_suppressed || (map_suppressed_race && map_suppressed_race != race_id);
}

void Reader::TrackMinimapSuppression() {
    // while loading, follow whether the race being built sees the option off by us; once the
    // race is shown, bind that to its racers (a Restart has new ones and a minimap again)
    if (loading) {
        map_suppressed = minimap_option_off;
        map_suppressed_race = 0;
    } else if (map_suppressed && !map_suppressed_race && race_shown) {
        map_suppressed_race = race_id;
    }
}

void Reader::RestoreMinimapOption() {
    if (!minimap_option_off || !host || !host->write_memory)
        return;
    const u64 at = MinimapOptionAddress();
    // Retain ownership across transient read/write failures. A verified replacement context
    // must keep its own setting and must not block suppression during the next race load.
    if (!at)
        return;
    const u8 now = Get<u8>(at, 0xFF);
    if (now > 1)
        return;
    if (at != minimap_option_address) {
        minimap_option_off = false;
        minimap_option_address = 0;
        return;
    }
    if (now == 0) {
        const u8 on = 1;
        if (!host->write_memory(host->userdata, at, &on, 1))
            return;
    }
    minimap_option_off = false;
    minimap_option_address = 0;
}

void Reader::RestoreHud() {
    // the companion stops (package unloaded, emulation ending): give the game its HUD back
    if (!host || !host->write_memory)
        return;
    int restored = 0;
    for (int e = 0; e < HudCount; ++e) {
        if (const u64 pending = std::exchange(hud_release[e], 0))
            ShowNode(pending); // best effort, like the rest of this cleanup
        const u64 node = std::exchange(hud_ours[e], 0);
        if (!node || node != hud_nodes.node[e] || !IsPlaceable(node))
            continue;
        if (u8 b = Get<u8>(node + 0x0D); b & 0x04) {
            b = static_cast<u8>((b & ~0x04) | 0x10);
            restored += host->write_memory(host->userdata, node + 0x0D, &b, 1) ? 1 : 0;
        }
    }
    if (restored && host->log)
        host->log(host->userdata, EDEN_DSMOD_LOG_INFO,
                  ("CTR DSMod: restored " + std::to_string(restored) + " top-screen HUD part(s)").c_str());
}

bool Reader::RaceTime(double& seconds) const {
    if (!clock_component)
        return false;
    const u64 timer = Get<u64>(clock_component + 0x28);
    if (!timer)
        return false;
    u8 t[0x1C];
    if (!Read(timer, t, sizeof t))
        return false;
    u32 start, stop, type;
    std::memcpy(&start, t + 0x0C, 4);
    std::memcpy(&stop, t + 0x10, 4);
    std::memcpy(&type, t + 0x18, 4);
    const bool running = t[0x14] != 0, paused = t[0x15] != 0;
    if (!running && stop == start)
        return seconds = 0, true; // countdown: not started
    u32 now = stop;
    if (running && !paused) {
        if (type != 0) // the race clock uses the game clock; other clock types are not read
            return false;
        const u64 clock = Static(OffClock);
        if (!clock)
            return false;
        now = Get<u32>(clock + 0x20);
    }
    seconds = static_cast<double>(static_cast<s32>(now - start)) / 8192.0;
    return seconds >= 0 && seconds < 60 * 100;
}

void Reader::Sample(const EdenDsmodHostApi& h) {
    host = &h;
    delta = h.get_i64 ? h.get_i64(h.userdata, "__relocation_delta", 0) : 0;
    std::array<Racer, MaxRacers> fresh;
    int found = 0;
    u64 identity = 0;
    if (ReadRacers(fresh, found, identity)) {
        racers = std::move(fresh);
        count = found;
        race_id = identity;
        misses = 0;
    } else if (++misses > MissesBeforeRaceGone) {
        count = 0; // a torn read keeps the last good sample; a lasting failure ends the race
    }
    if (static_variant < 0)
        LearnStaticVariant();
    track.clear();
    level_track.clear();
    laps = 0;
    loading = false;
    if (static_variant >= 0)
        if (const u64 client = Static(OffClient)) {
            if (const u64 screen = Get<u64>(client + 0x88)) // CClient._loadingScreen
                loading = Get<s32>(screen + 0x0C) != 0;     // CLoadingScreen._state: 0 = none
            level_track = TrackOf(String(Get<u64>(client + 0xC0)));
        }
    if (static_variant >= 0 && count > 0) {
        track = level_track;
        if (const u64 settings = Static(OffRaceSettings)) {
            const s32 l = Get<s32>(settings + 0x0C);
            laps = l >= 1 && l <= 9 ? l : 0;
        }
    }
    // the player's racer (its HUD and widgets link to it)
    player_address = 0;
    int best = MaxRacers + 1;
    for (int i = 0; i < count; ++i)
        if (!racers[i].ai && racers[i].position < best)
            best = racers[i].position, player_address = racers[i].address;
    ScanForWidget();
    // the race page shows once the race is really set up (while loading, the list briefly
    // holds only the player's racer); then it stays for this race
    // (a cup keeps its racers and the next track's name through the loading screen between races)
    if (track.empty() || count == 0 || loading)
        race_shown = false;
    else if (count >= 2 || clock_component)
        race_shown = true;
    // the race clock, read once per sample (the minimap option and the stopwatch use it)
    race_timed = race_shown && RaceTime(race_secs);
    SteerMinimapOption();
    TrackMinimapSuppression();
    if (!track.empty() && ++hud_tick >= HudApplyEvery) {
        hud_tick = 0;
        if (!hud_found)
            ResolveHudNodes();
        ApplyHud();
    }
    Publish();
}

void Reader::PublishUnsupported(const EdenDsmodHostApi& h, const char* code) {
    host = &h;
    if (!h.publish_i64 || !h.publish_text)
        return;
    CTR_DIAG(h.publish_i64(h.userdata, "ctr.page", 3);)
    h.publish_i64(h.userdata, "ctr.wrongv", 1);
    h.publish_text(h.userdata, "ctr.wrong", (std::string("module:ctr:wrongpatch:") + code).c_str());
}

void Reader::Publish() {
    const auto pi = [this](const std::string& n, s64 v) { host->publish_i64(host->userdata, n.c_str(), v); };
    const auto pf = [this](const std::string& n, double v) { host->publish_f64(host->userdata, n.c_str(), v); };
    const auto pt = [this](const std::string& n, const std::string& v) {
        host->publish_text(host->userdata, n.c_str(), v.c_str());
    };
    if (!host->publish_i64 || !host->publish_f64 || !host->publish_text)
        return;
    const bool race = race_shown && count > 0 && !track.empty();
    pi("ctr.race", race ? 1 : 0);
#ifdef CTR_DSMOD_DIAGNOSTICS
    pi("ctr.page", race ? 2 : 1);
    pi("ctr.hud", (hud_nodes.node[HudMap] ? 1 : 0) | (hud_nodes.node[HudLeaders] ? 2 : 0) |
                      (hud_nodes.node[HudTime] ? 4 : 0) | (hud_nodes.node[HudLap] ? 8 : 0) |
                      (hud_nodes.node[HudPosition] ? 16 : 0));
    {
        // diagnostics for console checks (bits: leaders 1, position 2, map 4, lap 8, timer 16)
        s64 want = 0, ours = 0;
        for (int e = 0; e < HudCount; ++e) {
            want |= hud_want[e] ? s64{1} << e : 0;
            ours |= hud_ours[e] ? s64{1} << e : 0;
        }
        pi("ctr.hudw", want);
        pi("ctr.hudo", ours);
        pi("ctr.hudh", hud_hidden); // as of the last ApplyHud
        // candidates seen / linked to the player (huds, minimap widgets, leaders columns)
        pi("ctr.dhud", diag.huds * 10 + diag.huds_linked);
        pi("ctr.dmap", diag.maps * 10 + diag.maps_linked);
        pi("ctr.dlead", diag.leaders * 10 + diag.leaders_linked);
    }
    pi("ctr.count", count);
    pi("ctr.widget", widget_found ? 1 : 0);
    pt("ctr.track", track);
#endif
    const Racer* me = nullptr;
    for (int i = 0; i < count && race; ++i)
        if (!racers[i].ai && (!me || racers[i].position < me->position))
            me = &racers[i];

    // ranking column: the position numbers stay in their rows; each racer's card slides to
    // its row when positions change (MK8 style: the overtaker passes over the card it passes,
    // bulging outwards), drawn last so it is on top
    const Keys& keys = PublishKeys();
    for (int k = 0; k < MaxRacers; ++k) {
        bool shown = false;
        for (int i = 0; i < count && race; ++i)
            if (racers[i].position == k + 1 && KeySafe(racers[i].driver))
                shown = true;
        pi(keys.rows[k].v, shown ? 1 : 0);
        pi(keys.rows[k].me, shown && me && me->position == k + 1 ? 1 : 0);
    }
    if (cards_race != race_id || !race) {
        cards = {};
        cards_race = race_id;
    }
    struct Shown {
        const Racer* r;
        Card* c;
    };
    std::array<Shown, MaxRacers> shown{};
    int n_shown = 0;
    for (int i = 0; i < count && race; ++i) {
        const Racer& r = racers[i];
        if (!KeySafe(r.driver))
            continue;
        Card* c = nullptr;
        for (Card& x : cards)
            if (x.address == r.address)
                c = &x;
        if (!c)
            for (Card& x : cards)
                if (!x.address && !c)
                    c = &x;
        if (!c)
            continue;
        const float target = static_cast<float>((r.position - 1) * CtrArt::RankRowDy);
        if (!c->address) { // first sight: in place
            *c = Card{};
            c->address = r.address;
            c->y = c->to = target;
        } else if (target != c->to) {
            c->from = c->y;
            c->to = target;
            c->t = 0;
            c->rising = target < c->from;
        }
        if (c->t >= 0) {
            const float p = std::min(1.0f, static_cast<float>(++c->t) / CardSlideSamples);
            const float e = p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.0f) / 2; // ease in-out
            c->y = c->from + (c->to - c->from) * e;
            c->x = c->rising ? std::sin(p * 3.14159265f) * CardBulgePx : 0;
            if (p >= 1)
                c->t = -1, c->x = 0, c->y = c->to, c->rising = false;
        }
        shown[n_shown++] = {&r, c};
    }
    // cards moving up are drawn last (on top)
    std::stable_sort(shown.begin(), shown.begin() + n_shown,
                     [](const Shown& a, const Shown& b) { return !a.c->rising && b.c->rising; });
    for (int k = 0; k < MaxRacers; ++k) {
        const Keys::Card& p = keys.cards[k];
        pi(p.v, k < n_shown ? 1 : 0);
        if (k >= n_shown)
            continue;
        const Racer& r = *shown[k].r;
        const Card& c = *shown[k].c;
        pf(p.x, c.x);
        pf(p.y, c.y);
        pi(p.me, &r == me ? 1 : 0);
        pi(p.fin, r.finished ? 1 : 0);
        pt(p.p, RacerArtKey(portrait_keys[k], "module:ctr:portrait:", r.driver, r.outfit));
        ArtKey& name = name_keys[k];
        if (name.Stale(&r == me ? "module:ctr:name:n:" : "module:ctr:name:w:", r.display, r.driver))
            name.value = std::string(name.prefix) + (KeySafe(r.display) ? Upper(r.display) : Upper(r.driver.substr(6)));
        pt(p.n, name.value);
    }

    // big rank and lap
    pi("ctr.mev", me ? 1 : 0);
    if (me) {
        pt("ctr.rank", "module:ctr:rank:" + std::to_string(me->position));
        // the HUD shows _currentLap (0 on the grid before the line is first crossed)
        const int shown_lap = std::clamp(me->lap, 1, std::max(1, laps));
        pi("ctr.lapv", laps > 0 && track[0] == 't' ? 1 : 0);
        pt("ctr.lap", "module:ctr:lap:" + std::to_string(shown_lap) + ":" + std::to_string(laps));
    } else {
        pi("ctr.lapv", 0);
    }

    // race time (m:ss:cc, the HUD's format), one picture per character slot (right aligned)
    const bool timed = race && race_timed;
    pi("tm.v", timed ? 1 : 0);
    if (timed) {
        const int cs = static_cast<int>(race_secs * 100);
        char text[16];
        std::snprintf(text, sizeof text, "%d:%02d:%02d", cs / 6000, cs / 100 % 60, cs % 100);
        const std::string s = text;
        for (int k = 0; k < TimeSlots; ++k) {
            const int at = static_cast<int>(s.size()) - TimeSlots + k;
            if (at < 0) {
                pi(keys.slot_v[k], 0);
                continue;
            }
            pi(keys.slot_v[k], 1);
            pt(keys.slot[k], TimeDigitKey(s[at]));
        }
    }

    // map and markers
    pt("ctr.bg", race ? "module:ctr:bg:" + track : std::string{});
    pt("ctr.map", race ? "module:ctr:map:" + track : std::string{});
    std::optional<CtrArt::MapView> view = race ? art.View(track) : std::nullopt;
    // the live widget's bounds replace the level data's when they describe the same area (a widget
    // left over from an earlier track must not be used)
    const auto similar = [](const float* a, const float* b) {
        const float aw = std::abs(a[0] - a[2]), ah = std::abs(a[3] - a[1]);
        const float bw = std::abs(b[0] - b[2]), bh = std::abs(b[3] - b[1]);
        if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0)
            return false;
        const float rw = aw / bw, rh = ah / bh;
        const float dcx = std::abs((a[0] + a[2]) - (b[0] + b[2])) / 2, dcy = std::abs((a[1] + a[3]) - (b[1] + b[3])) / 2;
        return rw > 0.8f && rw < 1.25f && rh > 0.8f && rh < 1.25f && dcx < 0.15f * bw && dcy < 0.15f * bh;
    };
    const bool use_widget = view && widget_found && widget_dir == view->params.direction &&
                            similar(widget_v, view->params.v);
    CTR_DIAG(pi("ctr.widget_used", use_widget ? 1 : 0);)
    if (use_widget)
        std::copy(widget_v, widget_v + 4, view->params.v);
    const auto place = [&](const Racer& r, float size, float& x, float& y) {
        float px, py;
        CtrAlchemy::ProjectToTexture(view->params, r.x, r.y, view->tex, px, py);
        x = view->off_x + (px - view->crop_x) * view->scale - size / 2;
        y = view->off_y + (py - view->crop_y) * view->scale - size / 2;
        // a projection far outside the map box is not shown
        return x > CtrArt::MapBoxX - 60 && x < CtrArt::MapBoxX + CtrArt::MapBoxW + 60 &&
               y > CtrArt::MapBoxY - 60 && y < CtrArt::MapBoxY + CtrArt::MapBoxH + 60;
    };
    int slot = 0;
    for (int i = 0; i < count && view; ++i) {
        const Racer& r = racers[i];
        if (&r == me || !r.has_pos || !KeySafe(r.driver))
            continue;
        float x, y;
        if (!place(r, CtrArt::HeadPx, x, y))
            continue;
        const int m = slot++;
        const Keys::Marker& p = keys.markers[m];
        pi(p.v, 1);
        pf(p.x, x);
        pf(p.y, y);
        pt(p.h, RacerArtKey(head_keys[m], "module:ctr:head:", r.driver, r.outfit));
    }
    for (; slot < MaxRacers; ++slot)
        pi(keys.markers[slot].v, 0);
    float x = 0, y = 0;
    const bool me_on_map = view && me && me->has_pos && KeySafe(me->driver) && place(*me, CtrArt::PlayerMarkerPx, x, y);
    pi("pm.v", me_on_map ? 1 : 0);
    if (me_on_map) {
        pf("pm.x", x);
        pf("pm.y", y);
        pt("pm.h", RacerArtKey(player_head_key, "module:ctr:headp:", me->driver, me->outfit));
    }
}

} // namespace CtrReader
