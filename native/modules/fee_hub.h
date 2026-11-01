// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage (0100A6301214E000) hub/world-map/roster reader, Ver. 2.0.0 only
// (fee_pins.inc). Independent of fee_reader.{h,cpp} (owned by another agent): this reads its
// own chains for the same build, reusing the already-live-verified UnitPool/Person/Job/GodUnit
// offsets from fee_pins.inc (the same data fee_reader publishes as war.u*/war.s.*, here
// published under roster.* so a roster works outside a map battle, where fee_reader's War page
// has nothing to show).
//
// 2026-10-30 pass 2: resolved from script.json ScriptMetadata (TypeInfo RVAs) + dump.cs (field
// offsets), the same method contracts/STATE-FLAGS.md used, not live-verified beyond the
// synthetic test (see HANDOFF notes in the commit message / report for what still needs a
// device/desktop read):
//   - ChapterData (App.StructData<ChapterData>_TypeInfo 0x6153a70): s_list is a List<ChapterData*>
//     (same generic List<T> layout as fee_pins' SkillData use -- ListItems/ListSize). Fields:
//     Cid +0x20, Name +0x28, NextChapter +0x80, GmapSpotState +0x90 (GmapSpot.State enum, i32;
//     Active=3, CanSearch=8 -- the game's own spoiler gate: a row is never Active/CanSearch
//     before the player can reach it), Flag +0xBC (ChapterData.Flags; SideStory=1073741824 marks
//     a paralogue/skirmish, unset = a main chapter), Nation/NationName +0x110/+0x118,
//     RecommendedLevel +0x108 (byte). gmap.chapter = the newest non-SideStory Active/CanSearch
//     row; gmap.g{i}.* = the SideStory rows in the same state.
//   - GameTime (App.GameTime_TypeInfo 0x6157a20, plain static fields, not a Singleton<T>):
//     s_PlayTime (statics+0x18, i32 ticks, TimeInv = 1/512 so seconds = s_PlayTime/512). Published
//     as hub.time -- this is the run's total play time (GameTime.PlayTime), not an in-Somniel
//     clock; FE Engage's Somniel has no day/night cycle in this build's classes, so "Current Time
//     hero" is read as play time, stated explicitly rather than invented.
//   - Facility/people inventory (HubSequence +0x150 <AccessData> -> List<HubAccessManager> ->
//     AccessList +0x18 -> List<HubAccessData>): AID +0x10 (name key) and the Is*/kind bytes
//     (IsStory/IsReliance/IsGod/IsUnit/IsAnimal/IsPerson +0x20..+0x25). Gold / other currencies:
//     no wallet singleton found -- unresolved.
// 2026-10-30 Live verification 5 (LIVE-READERS): the Somniel map. HubMiniMap (HubSequence +0x130)
//   m_center/m_size/m_uv/m_offset/m_reverse place a world point on its mapimage art exactly like
//   HubMiniMap.TransformPosition; its m_iconList gives every native map marker (HubAccess
//   OrigPosition, kind -> hubmapicon.xml icon, IconData.m_active, talkL). People and the player
//   move, so their positions come from the game's own Transform.get_position, called through the
//   HUD call stub (NextCall/OnCall, module RouteCalls); heading = HubPlayerController.Dir.
// Roster also supports a filter (roster_filter 0 all / 1 has-Emblem / 2 no-Emblem), matching the
// D2 "Roster filter chips" requirement, applied before the capped row list is published.

#include <array>
#include <chrono>
#include <deque>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "lp_unity.h"

namespace fee_hub {

using namespace dsmod_sdk::int_types;

inline constexpr int MaxRosterRows = 48; // UnitPool army force, published cap (pool holds 250)

struct RosterRow {
    u64 at{};
    int lv{}, hp{}, bond{-1};
    std::string pid, name, job, god, fid, icon;
    // Extras from the module's unit reader (SetUnitExtra): fee_reader Brief().
    int mhp{-1}, eng{}, weapon{}; // weapon = ItemData.Kinds (1 Sword 2 Lance 3 Axe 4 Bow ...)
    std::string face, ring, engimg, dot, pid8;
    std::string rface; // 72 px faction ring (Somniel people rows)
};

struct ChapterRow {
    std::string cid, name, nation, nation_name, next, place;
    int state{-1};    // GmapSpot.State (3 Active, 8 CanSearch are the only ones ever published)
    int lv{-1};        // RecommendedLevel
    bool side_story{}; // Flags.SideStory
};

/// One read-only game call the hub wants run through the HUD call stub (module routes it).
inline constexpr u32 CallTagBase = 0x100;
struct HubCall {
    u64 fn{};                  // main RVA
    std::array<u64, 4> args{};
    u32 tag{};
};

/// One Somniel map marker (HubMiniMap.m_iconList entry, the native large map's icons).
struct HubIcon {
    u64 access{};        // HubAccess component
    std::string aid;     // HubAccess.AID
    std::string icon;    // minimap.bundle sprite (hubmapicon.xml IconName for its DisposName)
    float x{}, z{};      // world position (OrigPosition; people: the game's live get_position)
    bool person{};       // HubAccess.IsPerson: the native map follows them every frame
    bool active{};       // IconData.m_active (the native map shows it)
    bool talk{};         // has a talk marker (IconData.talkL)
    std::string dot;     // army unit's map dot sprite (fee_sprites dot/<name>), "" = plain dot
    int talked{-1};      // HubAccess.AccessCount > 0 (dimmed marker); -1 = unknown
    bool reliance{};     // HubAccessData.IsReliance (marker never dims)
    std::string pid;     // HubAccess.AID as UTF-8 ("PID_..." for army members)
    std::u16string aid16; // HubAccess.AID raw (facility join: HubFacilityData.AID)
};

/// Where the drawn map picture sits relative to the map texture (Render), so the sample thread
/// can place world points on the published picture (GPS view centre, people grouping).
struct MapGeo {
    bool ok{};
    std::string image;          // HubMap.image it was computed for
    u32 tex_w{}, tex_h{};
    float crop_x0{}, crop_y0{}, scale{1}, offx{}, offy{};
    bool reverse{};
};

/// Map art name ("M002", "Solanel") from HubMiniMap.m_image's path, else from the hub scene name.
std::string MapImageName(std::string_view path, std::string_view scene);

/// Everything the Somniel map image is drawn from (one sample).
struct HubMap {
    bool ok{};
    std::string image;                // mapimage bundle base name ("solanel", "m002")
    std::string sprite;               // texture name inside it ("Solanel", "M002")
    float cx{}, cz{}, sx{1}, sz{1};   // HubMiniMap.m_center / m_size (x, z)
    float ux{}, uy{}, uw{1}, uh{1};   // HubMiniMap.m_uv
    float ox{}, oz{};                 // HubMiniMap.m_offsetX / m_offsetY
    bool reverse{};                   // HubMiniMap.m_reverse: the native map is turned 180 deg
    bool player{};                    // player position known
    float px{}, pz{}, dir{};          // player world x/z, HubPlayerController.Dir (degrees)
    std::vector<HubIcon> icons;
    u64 sel{};                        // selected person's HubAccess (ring on the map), 0 = none
};

/// One Somniel picture in flight (as the War map, cef2d03): `shown` is hub.mapkey, `pending` the
/// prefetched hub.mapnext. The newest frame is prefetched; once the module rendered it the host
/// has it cached, and it is swapped in one sample later (no blank frame, no backlog of stale
/// positions); a pending frame that has not rendered in 250 ms is replaced by the newest.
struct FramePipe {
    std::string shown, pending;
    bool seen{};
    std::chrono::steady_clock::time_point since{};
    long swaps{};
    /// `restart`: show `newest` directly (first picture, another map).
    void Step(const std::string& newest, bool pending_rendered, std::chrono::steady_clock::time_point now,
              bool restart);
    void Clear() {
        shown.clear(), pending.clear(), seen = false;
    }
};

/// One moving map marker drawn between position reads: from where it is drawn, linearly to the
/// newest read over the measured read interval (MinMs..MaxMs); a jump over Teleport world units
/// (fast travel) snaps. Ends exactly on the read value, so a marker at rest stops changing.
struct Glide {
    static constexpr double MinMs = 40, MaxMs = 400;
    static constexpr float Teleport = 6.f;
    bool had{};
    float fx{}, fz{}, tx{}, tz{};
    double ms{100};
    std::chrono::steady_clock::time_point since{};
    void Step(float rx, float rz, std::chrono::steady_clock::time_point now, float& x, float& z);
};

/// The Somniel follow camera (hub.view.cx/cy, 0..1 of the picture): eases toward the target by
/// Ease of the gap per sample; snaps on the first sample, a reset bump, a zoom change (centred)
/// or a jump of more than Jump (fast travel). Stops changing once it reaches the target.
struct ViewFollow {
    static constexpr double Ease = 0.22, Jump = 0.12, Settle = 0.0004;
    double cx{0.5}, cy{0.5};
    int rs{-1};
    bool had{}, centred{};
    void Step(double tx, double ty, bool is_centred, int reset);
};

// D2 chips: All, Sword, Lance, Axe, Bow (weapon type of the equipped weapon), No ring.
enum RosterFilter : int { FilterAll = 0, FilterSword = 1, FilterLance = 2, FilterAxe = 3,
                          FilterBow = 4, FilterNoRing = 5 };

class Hub {
public:
    // The game's "<mascot name>'s Shrine" (MID_Hub_Mascot_After, tag dropped) with the name put back.
    static std::string MascotShrine(const std::string& after, const std::string& name,
                                    const std::string& fallback);
    explicit Hub(const EdenDsmodHostApi& host);

    /// Reads the game and publishes hub.*/gmap.*/roster.* (host clears the snapshot per sample).
    void Sample(const EdenDsmodHostApi& host);
    /// "roster_pick <row|-1>", "roster_filter <0 all|1 has-Emblem|2 no-Emblem>". True = taken.
    bool OnAction(const char* action, std::int64_t argument);
    /// Next game call for the Somniel map (scratch = guest out-pointer area), or none.
    std::optional<HubCall> NextCall(u64 scratch);
    /// Result of a call NextCall asked for.
    void OnCall(u32 tag, u64 x0, const u8* scratch, std::size_t n);
    /// The call NextCall asked for was cancelled unserved (HUD call timeout): forget it.
    void OnCallLost();
    /// The player's message tables (MCID_/MID_Hub_ keys -> their words; "" = missing).
    void SetText(std::function<std::string(const std::string&)> text) {
        text_ = std::move(text);
    }
    /// Per-unit extras for roster rows (face, max HP, ring, Engage, weapon type, dot sprite).
    void SetUnitExtra(std::function<void(u64, RosterRow&)> f) {
        extra_ = std::move(f);
    }
    /// fee_sprites renderer ("dot/<name>" suffix -> RGBA) for person sprites on the map.
    void SetSpriteRender(std::function<bool(std::string_view, std::vector<u8>&, u32&, u32&)> f) {
        sprite_render_ = std::move(f);
    }
    /// Whether a read-only game call on a pool unit is safe now: the Somniel sequence runs free
    /// (no menu, fast travel or departure) -- the same gate the hub's own calls use.
    bool UnitCallsSafe() const;
    /// Unit address of the roster row the companion has selected (0 = none).
    u64 SelectedUnit() const {
        return sel_unit_;
    }
    /// Romfs bundle reader for the Somniel map art (the module owns it; null = no image).
    void SetUnity(lp_unity::Reader* unity) {
        unity_ = unity;
    }
    /// "fee:hub/map/<hash>" -> RGBA (asset worker thread). False when unknown.
    bool Render(std::string_view key, std::vector<u8>& rgba, u32& w, u32& h);
    /// Image size of the Somniel map widget (keep in sync with gen_manifest_hub.py).
    static constexpr u32 MapW = 1200, MapH = 1350;
    /// Picture point (0..1 of MapW x MapH) of a world point, given the picture geometry.
    static bool PicturePoint(const HubMap& m, const MapGeo& g, float wx, float wz, float& px, float& py);
    /// u,v (0..1, top-left origin) of a world point on the drawn map, for tests.
    static bool MapPoint(const HubMap& m, float wx, float wz, float& u, float& v);

private:
    bool Read(u64 at, void* out, std::size_t n) const;
    template <typename T>
    std::optional<T> Get(u64 at) const {
        T v{};
        if (!Read(at, &v, sizeof(T)))
            return std::nullopt;
        return v;
    }
    std::optional<u64> Ptr(u64 at) const {
        return Get<u64>(at);
    }
    u64 Slot(u64 rva) const {
        return base_ + rva + static_cast<u64>(delta_);
    }
    u64 StaticPtr(u64 type_info, u64 field) const;
    std::optional<std::string> ManagedString(u64 at, std::size_t max_chars = 96) const;

    bool Resolve(const EdenDsmodHostApi& host);
    int SceneMode() const; // 0 none, 1 hub (Somniel), 2 gmap (world map), 3 other (map/title)
    bool ReadRoster();
    bool ReadRow(u64 at, RosterRow& row) const;
    void ReadChapters();
    bool ReadChapter(u64 at, ChapterRow& row) const;
    void ReadMap();
    void Publish(const EdenDsmodHostApi& host);
    std::shared_ptr<const lp_unity::Image> Sprite(const std::string& bundle,
                                                  const std::string& name);

    EdenDsmodHostApi host_{};
    u64 base_{}, size_{};
    s64 delta_{};
    bool resolved_{};

    int mode_{};
    int sel_{-1};
    int filter_{FilterAll};
    std::vector<RosterRow> roster_;        // the full army force, unfiltered
    std::vector<ChapterRow> chapters_;      // every StructData<ChapterData> row this sample
    ChapterRow current_;                    // GameUserData.m_Chapter (the save's chapter)

    // Somniel map (ReadMap on the sample thread; NextCall/OnCall on the tick thread; Render on
    // the asset worker): everything shared sits behind map_mutex_.
    mutable std::mutex map_mutex_;
    HubMap map_;
    u64 player_root_{};                                  // HubPlayerController.m_PlayerRoot
    std::unordered_map<u64, u64> xform_;                 // GameObject/Component -> Transform
    std::unordered_map<u64, std::array<float, 3>> pos_;  // Transform -> world position
    std::unordered_map<u64, int> talked_;                // HubAccess -> AccessCount
    std::vector<std::pair<u32, u64>> jobs_;              // pending (tag kind, object) calls
    std::size_t job_next_{};
    int call_gap_{};
    u64 inflight_{};
    int ticket_gap_{60};
    int tickets_{-1};
    int newest_gap_{120};
    int tod_gap_{120};
    int tod_{-1};                                        // HubUtil.TimezoneType (-1 = unknown)
    u64 newest_spot_{};                                  // GmapSpotManager.GetNewestSpot()                                    // Relay Tickets (-1 = unknown)
    std::string map_key_;                                // newest frame key
    FramePipe pipe_;                                     // hub.mapkey / prefetched hub.mapnext
    std::deque<std::string> rendered_;                   // frames Render() finished (newest last)
    bool player_turn_{};                                 // NextCall: player position every other call
    std::vector<u8> base_rgba_;                          // Render: static layer (asset worker only)
    std::string base_for_;
    bool base_rev_{};
    ViewFollow view_;
    std::unordered_map<u64, Glide> glide_;               // 0 = the player, else HubAccess
    void GlideMarkers(std::chrono::steady_clock::time_point now);                                    // hub.view.cx/cy (shown frame's centre)
    std::unordered_map<std::string, HubMap> frames_;     // key -> frame (Render)
    std::vector<std::string> frame_order_;
    lp_unity::Reader* unity_{};
    std::function<void(u64, RosterRow&)> extra_;
    std::function<bool(std::string_view, std::vector<u8>&, u32&, u32&)> sprite_render_;
    u64 sel_unit_{};
    std::unordered_map<std::string, std::string> pid_dot_; // army Pid (UTF-8) -> dot sprite
    std::unordered_map<std::string, std::size_t> pid_row_; // army Pid (UTF-8) -> roster_ index
    std::optional<std::string> ManagedUtf8(u64 at, std::size_t max_chars = 64) const;
    std::string RuleLine(const std::string& mid) const; // MID_RULE_* with tags filled
    std::function<std::string(const std::string&)> text_;
    std::string Localized(const std::string& key) const; // key itself when missing
    struct Place {
        std::string name;               // localized MID_Hub_* of the area
        std::vector<std::string> icons; // minimap sprite names of its facilities
    };
    std::vector<Place> places_;         // Somniel "Locations" (HubAreaData), unlocked only
    struct Facility {
        std::u16string aid;             // HubFacilityData.AID (joins HubAccess.AID)
        std::string icon, name;         // IconName, localized MID
        int area{-1};                   // index into places_
    };
    std::vector<Facility> facilities_;  // unlocked facilities of the unlocked places
    // FE3H-style Somniel page (owner 2026-11-01): people by place, legend, GPS view.
    u64 sel_person_{};                  // HubAccess of the selected person (0 = none)
    int view_reset_{};                  // bumps when a selection should snap the map view
    std::vector<u64> person_order_;     // published person index -> HubAccess
    std::vector<int> pr_person_;        // Map tab row -> person index (-1 header)
    std::vector<std::pair<int, int>> pc_person_; // People tab card row -> (left, right) person
    MapGeo geo_;                        // under map_mutex_
    std::mutex table_mutex_;
    std::unordered_map<std::string, std::string> icon_table_; // DisposName -> minimap IconName
    std::string IconSprite(const std::string& dispos);
    void PublishSomniel(const EdenDsmodHostApi& host);
    void ReadPlaces();
    std::mutex sprite_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const lp_unity::Image>> sprites_;
    std::shared_ptr<const lp_unity::Image> map_tex_; // the map texture (cropped when drawn)
    std::string base_name_;
    // Somniel map diagnostics (owner 2026-11-01, Thor: "map not shown"): why the map is missing,
    // logged once per change (host log / logcat) and published as hub.mapwhy.
    int map_why_{-1};
    bool tex_failed_{};      // the map texture did not decode: markers drawn on a plain backdrop
    bool tex_logged_{};
    void LogLine(int level, const std::string& msg) const;
    int crop_x0_{}, crop_y0_{}, crop_x1_{}, crop_y1_{}; // content box of the map texture
};

} // namespace fee_hub
