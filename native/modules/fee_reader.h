// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage (0100A6301214E000) game-memory reader, Ver. 2.0.0 (v327680) only -- build
// 8C08B9719E085F91847B5E0F935D9488 (fee_pins.inc). Unity IL2CPP: every value is reached through
// TypeInfo-slot -> statics -> instance chains; the offsets are the dumped field offsets, each one
// verified live on that build (fee_pins.inc records the slot and field layout).
//
// Scope: scene flags, force-walked battle units, terrain and the game's danger bitmap;
// selected-unit stats, items, skills, Engage and bonds; native attack-phase matchup. Map taps
// inspect only and never move the native cursor. fee_hub supplies Somniel/world-map/roster data;
// fee_hud supplies native HUD hide/restore through the guest mailbox.
//
// NCE: the IL2CPP TypeInfo slots live in the data segment, which sits at a constant delta under
// Android's NCE backend; the delta is learned from the host ("__relocation_delta", 0 under
// Dynarmic). No direct guest writes. Combat stats, forecast and sortie limit use read-only
// game calls through the module's HUD call stub.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include "fee_assets.h"
#include "fee_map.h"

namespace FeeReader {

using namespace dsmod_sdk::int_types;

/// Only Ver. 2.0.0 (build 8C08B9719E085F91847B5E0F935D9488 + zeros). SupportsBuildHex takes the
/// 64-character upper-case hex supports_build() receives; SupportsBuildId the raw 32 bytes.
bool SupportsBuildHex(const char* hex);
bool SupportsBuildId(const std::uint8_t* build_id32);

/// fee.mode: which scene singleton is live (0 = none of them, e.g. a load between scenes).
enum Mode : int { ModeNone = 0, ModeBattle = 1, ModeHub = 2, ModeGmap = 3, ModeTitle = 4 };
/// fee.page: companion pages (module action "ui_open <page>").
enum Page : int { PageStandby = 0, PageWar = 1, PageUnits = 2 };
/// War page strip (module action "war_mode <mode>", D2 All / Map / Char).
enum WarMode : int { WarAll = 0, WarMap = 1, WarChar = 2 };

/// Width in canvas px of the war-map image widget on each War mode (the manifest's rect); taps
/// arrive relative to the widget, so the cell mapping scales by fee_map::CanvasW / view width.
/// Keep these in sync with packages/FireEmblemEngage/dualscreen/manifest.json.
inline constexpr int MapViewWAll = 624;
inline constexpr int MapViewWMap = 1240;

inline constexpr int MaxPublishedUnits = 60; // war.u{i} cap (UnitPool holds 250; a force <= ~50)

struct UnitRow {
    u64 at{};
    int force{}, lv{}, hp{};
    int x{-1}, z{-1};
    int engage_count{}, engage_turn{};
    bool engaged{};  // m_EngageCountView == Unit.InvalidEngageCount (255)
    int bond{-1};    // GodBond.m_Level with the equipped Emblem; -1 = none/unknown
    std::string pid; // PersonData.Pid ("PID_...")
    std::string name; // PersonData.Name key ("MPID_...")
    std::string job;  // JobData.Name key ("MJID_...")
    std::string god;  // GodData.Name key ("MGID_..."), empty = no Emblem
    std::string fid;  // PersonData.Fid (face sprite name)
    std::string icon; // PersonData.UnitIconID
    std::string god_face; // GodData.FaceIconName (Emblem face sprite name)
    std::string dot;      // map dot sprite candidates "a|b" (unit icon _ job icon M/F _ weapon)
    std::string god_dot;  // Emblem dot sprite ("530Marth_Marth_NoWeapon")
    std::string edit_name; // UnitEdit.m_Name: the avatar's player-given name ("" = none)
    // Displayed stats (CapabilityDefinition.Type order, Emblem/equipment boosts included);
    // -1 = unreadable. stats[0] is max HP.
    std::array<int, 11> stats{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    bool done{};  // acted this phase (Unit.Status Fixed | Vision): not "ready"
    int style{};  // JobData.Style (BattleStyle.Types; 0 = none)
    u64 person{}; // PersonData* (matches the native unit window's calc copy)
    u64 god_unit{};
};

struct ItemRow {
    u64 at{}; // the UnitItem
    std::string iid, name, icon; // ItemData Iid / MIID_ key / icon key
    int uses{}, max_uses{}, refine{}, kind{}, range_min{-1}, range_max{-1};
    bool equipped{};
};

struct SkillRow {
    int index{-1};
    bool invisible{};
    std::string sid, name, icon;
};

/// Commanders (Char overview): named enemies (they have a portrait) anywhere on the field --
/// not only within the published rows -- in walk order, up to `max`.
std::vector<int> PickCommanders(const std::vector<UnitRow>& units,
                                const std::function<bool(const UnitRow&)>& has_face, int max = 3);
/// "While Engaged" stat line: label/value pairs in the Unit Details order, three per line.
std::string FormatStatBonus(const std::array<int, 11>& v);

class Reader {
public:
    Reader(const EdenDsmodHostApi& host, const char* config_json);
    ~Reader();
    Reader(const Reader&) = delete;
    /// Test hook: the millisecond clock the motion pulses run on.
    void SetClock(std::function<std::int64_t()> clock_ms) {
        clock_ms_ = std::move(clock_ms);
    }
    Reader& operator=(const Reader&) = delete;

    /// Reads the game and publishes every output (the host clears the snapshot per sample).
    void Sample(const EdenDsmodHostApi& host);
    /// "ui_open <page>", "war_danger <0|1|-1 toggle>", "war_select <tap payload>",
    /// "war_mode <0 All|1 Map|2 Char>", "war_pick <roster row | -1>". True = taken.
    bool OnAction(const char* action, std::int64_t argument);

    /// Decoded text + faces from the player's romfs (thread-safe).
    fee::Assets& Assets() {
        return assets_;
    }
    /// Combat stats for a unit the native window is not showing: the game's own calculation
    /// (Unit.GetSimpleHit -> Unit.CalcInfo.Update fills the unit's m_CalcInfo cache), run through
    /// the HUD call stub by the module. CalcWanted() = the unit to refresh now (0 = none);
    /// CalcDone() after the call returned.
    u64 CalcWanted();
    /// Same for the Roster unit page outside battle (`at` = roster selection, `safe` = the
    /// hub says a unit call is safe now): refreshed twice a second while it is shown.
    u64 RosterCalcWanted(u64 at, bool safe);
    void CalcDone(u64 unit);
    /// Deploy limit ("Deployed n/limit"): SortieUtil.GetSortieUnitMax(), asked once per battle
    /// scene; the module runs it through the call stub and hands back x0.
    bool SortieLimitWanted();
    void SortieLimitDone(std::int64_t limit);
    /// Combat forecast (Matchup page): the game's own forecast for the selected unit attacking the
    /// chosen enemy, computed by the load plan's `forecast` helper (BattleInfo.CalcSimple +
    /// BattleCalculator.CalcSimulation + the native panel's getters) through the call stub.
    /// ForecastWanted() = true with the pair to compute now; ForecastDone() with the helper's
    /// out block (mailbox scratch, layout in gen_load_plan.py "forecast").
    bool ForecastWanted(u64& offense, u64& defense);
    /// The offense's weapon (UnitItem) of the pair ForecastWanted() returned: the native
    /// forecast's L/R pick (0 = the helper picks).
    u64 ForecastItem() const {
        return fc_asked_item_;
    }
    void ForecastDone(u64 offense, u64 defense, const std::uint8_t* out, std::size_t size);

    /// Roster row extras for fee_hub (any pool unit, outside a battle too): face key, max HP,
    /// Emblem ring key, Engage gauge sprite, weapon type (ItemData.Kinds of the equipped weapon,
    /// else the first weapon), map dot sprite name and the UTF-8 Pid. ok = false when unreadable.
    struct UnitBrief {
        bool ok{};
        int mhp{-1}, eng{}, weapon{};
        std::string pid, face, ring, engimg, dot;
        std::string rface; ///< 72 px faction ring (Somniel people rows)
    };
    UnitBrief Brief(u64 unit);
    /// Owner option A (2026-11-01) portrait key: the close-up face -- or, for a generic unit
    /// without face art, its map sprite -- in the faction ring (round, size "<d>") or framed
    /// plate (size "<w>x<h>"); force 0 blue, 1 red, 2 green. Empty when neither exists.
    std::string UnitArt(const std::string& icon, const std::string& dot, int force, bool round,
                        const std::string& size);
    /// Roster unit page (D2 10b): the Char Details readers for a roster-selected unit, published
    /// as roster.s.* (same suffixes as war.s.*).
    void PublishRosterUnit(const EdenDsmodHostApi& host, u64 unit);

    /// The map images (thread-safe); the module's LoadImage renders from it.
    fee_map::Store& Maps() {
        return maps_;
    }

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
    /// A main-image data-segment address (TypeInfo slots): the NCE delta applies.
    u64 Slot(u64 rva) const {
        return base_ + rva + static_cast<u64>(delta_);
    }
    /// TypeInfo slot -> klass -> statics -> the pointer-sized static field at `field` (0 = none).
    u64 StaticPtr(u64 type_info, u64 field) const;
    /// IL2CPP managed string -> ASCII (name keys are ASCII; others become lowercase hex-free
    /// best-effort, capped). nullopt = unreadable.
    std::optional<std::string> ManagedString(u64 at, std::size_t max_chars = 96) const;
    /// Managed string -> UTF-8 (SIDs are Japanese).
    std::optional<std::string> ManagedUtf8(u64 at, std::size_t max_chars = 96) const;

    bool Resolve(const EdenDsmodHostApi& host);
    /// App.Language.s_Lang (0..11), nullopt while unresolved.
    std::optional<int> Language() const;
    /// The four scene singletons; returns the mode. All four are re-read after the battle read
    /// and the sample is dropped when they changed (torn read mid scene transition).
    int SceneMode() const;
    bool ReadBattle();
    bool ReadUnit(u64 at, UnitRow& row) const;
    int BondLevel(u64 god_unit, const std::string& pid) const;
    void ReadStats(u64 at, UnitRow& row) const;
    std::vector<ItemRow> ReadItems(u64 unit) const;
    bool ReadItem(u64 unit_item, ItemRow& row) const; // false: empty / placeholder slot
    /// The native unit window's displayed values (sum of each UnitParamDetail), true only while
    /// that window's calc unit is `u` (same PersonData and cell).
    bool NativeParams(const UnitRow& u, std::array<int, 17>& out) const;
    /// SkillArray -> rows resolved through the loaded SkillData table.
    std::vector<SkillRow> ReadSkillArray(u64 array) const;
    std::optional<SkillRow> SkillByIndex(int index) const;
    std::optional<SkillRow> SkillBySid(const std::string& sid) const;
    u64 GodBondFor(u64 god_unit, const std::string& pid) const;
    void Publish(const EdenDsmodHostApi& host);
    void FeedPortrait(const std::string& sprite);
    void PublishDetails(const EdenDsmodHostApi& host, const UnitRow& u, const char* prefix = "war.s.");
    /// Bond level of every player Emblem with `u`, the equipped one's Bond Effects (Synchro skills
    /// at the current level) and its stat bonus.
    void PublishBonds(const EdenDsmodHostApi& host, const UnitRow& u);
    /// Terrain ids on this map whose TerrainData name is a chest / door (map glyphs).
    void TagTerrain();
    /// MID_RULE_* text with its argument tags filled in (leader name, rule argument).
    std::string RuleText(const std::string& mid, const std::string& arg) const;
    void SelectAtImagePoint(s64 payload);
    /// Native cursor follow: when the cursor's cell or the unit on it changed since the last
    /// sample, the unit there becomes the selection; an empty tile clears it and its terrain is
    /// inspected (war.tile). The follow camera tracks the cursor. A bottom tap or list pick wins
    /// until the cursor moves again.
    void FollowCursor();
    /// Attack phase (MapSequenceTargetSelect open = the native attack forecast, then the combat
    /// itself): the attacker / defender pair, or none. Read each battle sample.
    void ReadAttack();
    /// The avatar's player-given name (UnitEdit) from the unit pool, every ~2 s.
    void ScanHeroName();
    /// The name the game shows for a unit: its player-given name, else the localized person
    /// name, else (generic soldiers / unknown keys) the class name; never a raw key.
    std::string UnitName(const UnitRow& u) const;
    /// Localized single-line text for a key; "" when it does not decode (never the raw key).
    std::string Loc(const std::string& key) const;
    struct TerrainInfo {
        bool ok{};
        std::string name;
        int def{}, avo{}, heal{};
    };
    TerrainInfo TerrainAt(int x, int z) const;
    /// Publishes war.mapkey (shown) / war.mapnext (prefetch) / war.map.dx/dy / cursor marker for
    /// one War map mode from the frame the camera wants now.
    void PublishMap(const EdenDsmodHostApi& host, const fee_map::Frame& frame, int m);
    /// Drag / pinch on the war map (module-owned view, payloads as the host packs them).
    void PanView(s64 payload);
    void PinchView(s64 payload);
    /// Back to the follow camera on both War map modes.
    void ResetView() {
        cam_ = {};
    }
    /// Matchup: publish war.fc.* for the native attack-phase target.
    void PublishForecast(const EdenDsmodHostApi& host, const UnitRow& u);

    EdenDsmodHostApi host_{};
    u64 base_{}, size_{};
    s64 delta_{};
    bool resolved_{};
    bool synced_{}; // the standby page has been published once since (re)load

    int mode_{ModeNone};
    int page_{PageStandby};
    int war_mode_{WarAll};
    bool danger_on_{true};
    int card_page_{};  // War / All card swipe page 0..3
    int char_page_{};  // War / Char page 0..3
    u64 sel_at_{}; // selected unit address (pool-stable), 0 = none
    // Native map cursor (MapCursor.m_Pos -> cell), read each battle sample; cursor_ok_ false
    // when unreadable. cur_key_ = the last (cell, unit on it) the follow acted on.
    bool cursor_ok_{};
    int cursor_x_{}, cursor_z_{};
    std::tuple<int, int, u64> cur_key_{-1, -1, 0};
    bool focus_cursor_{true};   // the follow camera tracks the cursor (false after a list pick)
    int tile_x_{-1}, tile_z_{-1}; // inspected empty tile (cursor or a bottom tap), -1 = none
    // Shown map image per War map mode and the prefetched next one (war.mapnext): the shown key
    // only changes once the next picture has been rendered (one in flight, no stale backlog);
    // while it renders, a pure pan of the shown picture is a host-side offset (war.map.dx/dy).
    struct MapShown {
        u64 hash{}, content{};
        std::string key;
        int cell{}, mox{}, moy{}, cells_h{};
        bool user{};
    };
    std::array<MapShown, 2> shown_{}, pending_{};
    std::array<bool, 2> pending_seen_{};
    std::array<std::int64_t, 2> pending_since_{};
    std::array<int, 2> hint_x0_{-1, -1}, hint_z0_{-1, -1};
    // Attack phase (ReadAttack): the native forecast pair, the combat that follows, and what the
    // bottom screen showed before it (restored afterwards).
    bool atk_{}, atk_combat_{};
    // During a map combat: the HP the game is drawing per hit (unit, hp) -- the combat scene's own
    // CharacterGameStatus.HP (animations on), else Unit.m_Hp.m_Display; empty outside combat.
    std::vector<std::pair<u64, int>> combat_hp_;
    void ReadCombatHp();
    u64 atk_off_{}, atk_def_{}, atk_item_{};
    std::int64_t atk_seen_ms_{};
    int atk_prev_page_{-1};
    u64 hero_scan_{};
    // User camera per War map mode (All, Map); inactive = follow view. last_cells_ = map size of
    // the last published frame, last_follow_ = that frame's follow geometry per mode (a drag
    // starts from what is on screen).
    std::array<fee_map::Camera, 2> cam_{};
    std::array<fee_map::Frame, 2> last_follow_{};
    bool last_follow_ok_[2]{};
    // Last battle read.
    bool war_ready_{};
    int turn_{}, force_{};
    int map_w_{}, map_h_{};
    std::array<u8, 1024> terrain_{};
    std::array<u32, 32> danger_{};
    std::vector<UnitRow> units_;
    std::vector<UnitRow> bench_; // Force Absent, published only during Battle Prep
    bool prep_{};
    std::string win_mid_, win_arg_, lose_mid_, lose_arg_;
    std::vector<u8> chest_ids_, door_ids_, impassable_ids_;
    int last_scene_{ModeNone}; // last non-None scene (battle entry through a loading gap)
    u64 sortie_seq_{};                    // MapSequence the limit was asked for
    std::atomic<std::int64_t> sortie_limit_{-1};
    std::string sortie_chapter_; // chapter the limit was read for

    // Game-calc combat stats (CalcWanted / CalcDone; tick thread vs sample thread).
    std::atomic<u64> calc_unit_{0};
    u64 roster_calc_asked_{0};
    int roster_calc_wait_{0};  // unit whose m_CalcInfo the game refreshed last
    u64 calc_asked_{};               // unit of the last request (tick thread only)
    int calc_wait_{};                // ticks since that request
    // Combat forecast (ForecastWanted / ForecastDone; tick thread vs sample thread).
    struct ForecastSide {
        int attacks{}, total{}, strings{}, hp{}, status{}, result{}, battle_times{}, total_attack{},
            efficacy{}, total_order{};
        float hit{}, crit{};   // SimpleHit / SimpleCritical (unit-only)
        float bhit{}, bcrit{}; // Hit / Critical against the other side (native panel)
    };
    struct Forecast {
        u64 offense{}, defense{}, item{};
        std::array<ForecastSide, 2> side{};
        int flags{}, range{}, battle_count{}, scenes{};
    };
    mutable std::mutex fc_mu_;
    Forecast fc_{};                   // last finished forecast (fc_mu_)
    std::atomic<u64> fc_off_{0}, fc_def_{0}, fc_item_{0}; // pair the Matchup page wants (sample thread)
    u64 fc_asked_off_{}, fc_asked_def_{}, fc_asked_item_{}; // tick thread
    int fc_wait_{};
    bool fc_published_{}; // this Publish chose a pair
    // war.fx.* motion pulses: turn, phase, selected HP, card page, char page. Timed in
    // milliseconds, not samples, so a pulse stays up long enough to be seen at any sample rate.
    std::array<std::int64_t, 5> fx_last_{};
    std::array<std::int64_t, 5> fx_until_{}; // clock ms the pulse drops at
    u64 fx_unit_{};                           // unit whose HP fx_last_[2] holds
    bool fx_seen_{};
    std::function<std::int64_t()> clock_ms_; // steady clock; tests replace it
    static constexpr std::int64_t FxPulseMs = 700;
    // Chapter (GameUserData.m_Chapter -> ChapterData.Name): "MCID_M010" -> its _PREFIX line.
    std::string chapter_mid_;

    mutable std::unordered_map<std::string, int> sid_index_; // SkillData Sid -> s_list index
    std::unordered_map<std::string, int> portraits_fed_; // sprite -> -1 handed to maps_, else tries
    fee_map::Store maps_;
    fee::Assets assets_;
    u64 samples_{};
};

} // namespace FeeReader
