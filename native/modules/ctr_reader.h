// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash Team Racing Nitro-Fueled 1.0.15 (main 1C68951840693051...): the race reader.
// Every location below comes from the executable's exported symbols and Alchemy reflection
// (field offsets registered by each class's arkRegisterInitialize), checked live on the desktop.
//   [main+0x4D64F10]  CRacerManager*   +0x10 _racers (igObjectList: count u32 +0x0C, items +0x20)
//   CRacer            +0x20 _kart, +0x30 _driverData, +0x38 _outfitData (handles: object at +0x28),
//                     outfit +0x28 _materialPackages (list: items +0x20, item +0x10 name), +0x40
//                     _isAiControlled u8, +0x4C _currentLap, +0x64 _officialPosition, +0x8C
//                     _finished u8
//   kart entity       +0x20 world position x, y, z (f32)
//   driver data       +0x58 _name ("DriverCrash"), +0x60 _displayName (the game language)
//   [main+0x4D44710]  CClient singleton  +0xC0 _levelName ("octane/t111_crash_cove/...")
//   [main+0x4D65250]  CRaceSettingsManager  +0x0C _lapsCount
//   vptr main+0x49093A0  CGuiBehaviorRacingMinimap  +0x30 _topLeft, +0x38 _bottomRight (vec2),
//                     +0x40 _minimapDirection. Found by a sliced heap scan once per track; its
//                     bounds replace the level data's (Crash Cove's differ).
//   vptr main+0x4955920  CRaceClockComponent  +0x28 _raceTimer (CTimer: +0x0C start, +0x10 stop,
//                     +0x14 running, +0x15 paused, +0x18 type; type 0 = game clock
//                     [main+0x4D44798]+0x20, 1/8192 s). Found by the same heap pass.
// Top-screen HUD hiding (owner settings): Gui::igGuiPlaceable (vptr main+0x47EFF68) bitfield
// +0x0C, bit 10 hidden + bit 12 dirty, exactly as the game's hidePlaceable. Nodes: minimap =
// widget [+0x18] sprite [+0x40] parent; leaders = CGuiBehaviorRaceLeaders (vptr main+0x4909B78)
// [+0x10] list item[0] [+0x40]; timer/lap/position = player's CRacerHudComponent (vptr
// main+0x494D5A0) [+0x38] project -> [+0x10] root -> [+0x38] composition -> [+0x20]
// Root_placeable children [0]/[3]/[4] (layout y 138/93/961 checked).
// Data-segment addresses are tried at main+off and at main+off+__relocation_delta (NCE).

#pragma once

#include "ctr_alchemy.h"
#include "ctr_art.h"

#include "core/mods/dsmod_module_abi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CtrReader {

inline constexpr int MaxRacers = 8;
/// Top-screen HUD elements the companion can hide (owner settings, default hidden).
enum HudElement { HudLeaders, HudPosition, HudMap, HudLap, HudTime, HudCount };
inline constexpr int TimeSlots = 8; ///< "MM:SS:CC", right aligned

/// A published picture key, rebuilt only when its inputs change (Publish runs every sample).
struct ArtKey {
    std::string_view prefix; ///< always a string literal
    std::string a, b, value;
    bool built{};
    /// true (remembering the inputs) when `value` must be rebuilt for them
    bool Stale(std::string_view p, const std::string& x, const std::string& y) {
        if (built && p == prefix && x == a && y == b)
            return false;
        prefix = p, a = x, b = y, built = true;
        return true;
    }
};

class Reader {
public:
    explicit Reader(CtrArt::Library& art) : art{art} {}
    /// Clears the HUD hidden bits this reader set (the companion stops).
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    void Sample(const EdenDsmodHostApi& host);
    /// A build this reader does not support: publish the wrong-patch page only (no reads).
    void PublishUnsupported(const EdenDsmodHostApi& host, const char* code);
    /// Owner setting: hide (true) or show one top-screen HUD element; applied while racing.
    void SetHudHidden(int element, bool hide);

private:
    friend struct ReaderTestAccess;
    struct Racer {
        std::uint64_t address{}; ///< the CRacer object
        bool ai{};
        bool finished{};
        int position{};
        int lap{};
        float x{}, y{};
        bool has_pos{};
        std::string driver;  ///< "DriverCrash"
        std::string display; ///< "Crash"
        std::string outfit;  ///< "DriverPolar_Outfit_Alt_01_materials" (the equipped skin)
    };
    struct DriverNames {
        std::string name, display;
    };

    bool Read(std::uint64_t at, void* out, std::size_t size) const;
    template <class T>
    T Get(std::uint64_t at, T fallback = {}) const {
        T v{};
        return Read(at, &v, sizeof v) ? v : fallback;
    }
    std::string String(std::uint64_t at, std::size_t max = 128) const;
    std::uint64_t Static(std::uint64_t off) const; ///< main+off (or +delta) holding a heap pointer
    bool LearnStaticVariant();
    bool ReadRacers(std::array<Racer, MaxRacers>& out, int& found, std::uint64_t& identity);
    void ScanForWidget();
    bool RaceTime(double& seconds) const;
    bool IsPlaceable(std::uint64_t node) const;
    std::uint64_t HandleObject(std::uint64_t handle) const;
    struct WidgetCandidate {
        std::uint64_t address{};
        float v[4]{};
        int dir{-1};
    };
    std::uint64_t PlayerHudComponent() const;
    std::uint64_t PlayerComponent(std::uint64_t vtable_off) const;
    void UseWidget(const WidgetCandidate& w);
    void ResolveHudNodes();
    bool ShowNode(std::uint64_t node) const; ///< gives back a hidden bit we set
    void ApplyHud();
    void RestoreHud();
    std::uint64_t MinimapOptionAddress() const;
    void SteerMinimapOption();
    void RestoreMinimapOption();
    void TrackMinimapSuppression();
    /// false while the current race's HUD was built with the minimap option off by us
    bool MinimapExpected() const;
    void Publish();

    CtrArt::Library& art;
    const EdenDsmodHostApi* host{};
    std::int64_t delta{};
    int static_variant{-1}; ///< 0 = main+off, 1 = main+off+delta, -1 = unknown
    std::array<Racer, MaxRacers> racers{};
    int count{};
    int laps{};
    int misses{};             ///< consecutive failed racer reads
    std::uint64_t race_id{};  ///< set hash of the racer objects: changes on a new or restarted race
    std::string track; ///< "t111_crash_cove", empty outside a race track
    std::string level_track; ///< the loaded/loading level's track id, whether or not racers exist
    std::unordered_map<std::uint64_t, DriverNames> names;
    std::unordered_map<std::uint64_t, std::string> outfits;

    // the live minimap widget (per track)
    std::string scan_track;
    std::uint64_t scan_race{};
    std::uint64_t scan_cursor{}, scan_end{};
    int scan_idle{};
    bool widget_found{};
    float widget_v[4]{};
    int widget_dir{-1};
    std::vector<std::uint64_t> pass_clocks;
    std::uint64_t clock_component{}; ///< the race's CRaceClockComponent
    std::vector<WidgetCandidate> pass_widgets;
    std::uint64_t widget_addr{};
    std::uint64_t player_address{};
    /// a racer's card in the ranking column, sliding between rows
    struct Card {
        std::uint64_t address{}; ///< the racer (0: free)
        float y{}, from{}, to{}, x{};
        int t{-1}; ///< samples into the slide, -1 at rest
        bool rising{};
    };
    std::array<Card, MaxRacers> cards{};
    std::array<ArtKey, MaxRacers> portrait_keys, name_keys, head_keys; ///< per card / marker slot
    ArtKey player_head_key;
    bool race_timed{};  ///< the race clock read this sample (race shown)
    double race_secs{}; ///< its time
    std::uint64_t cards_race{};
    bool race_shown{};
#ifdef CTR_DSMOD_DIAGNOSTICS
    /// how many candidates the last ResolveHudNodes saw, and how many were the live ones
    struct Diag {
        int huds{}, huds_linked{}, maps{}, maps_linked{}, leaders{}, leaders_linked{};
    } diag;
    std::int64_t hud_hidden{}; ///< parts hidden as of the last ApplyHud
#endif
    std::vector<std::uint64_t> pass_leaders;
    std::uint64_t hud_project{};       ///< the player's racer HUD GUI project (scan focus)
    bool scan_focus{};
    int focus_passes{};
    bool loading{}; ///< the game's loading screen is up
    bool minimap_option_off{}; ///< we turned the game's minimap option off (restore it)
    std::uint64_t minimap_option_address{}; ///< exact option changed; never restore another context
    int minimap_option_wait{};
    bool map_suppressed{};             ///< the race loaded last was built with the option off by us
    std::uint64_t map_suppressed_race{}; ///< its race_id once shown (0: not yet)
    struct HudNodes {
        std::uint64_t node[HudCount]{};
    } hud_nodes;
    bool hud_found{};
    int extra_passes{};
    int hud_tick{};
    std::atomic<bool> hud_want[HudCount]{true, true, true, true, true}; ///< set from actions
    std::uint64_t hud_ours[HudCount]{};
    std::uint64_t hud_release[HudCount]{}; ///< replaced nodes still hidden by us (write failed)
};

} // namespace CtrReader
