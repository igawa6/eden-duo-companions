// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Super Mario Odyssey 1.4.1: read-only scene/player reader (RE batch 5, two live boots).
// Scan GameSystem once, then follow +0x10 HakoniwaSequence -> +0x78 current StageScene.
// Application -> GameSystem is still unresolved; this is not a fully static root chain.
// StageScene +0x2E0 -> GameDataHolder +0x20 -> gameData (+0x5D8 coins, +0xA38 current WorldId).
// Scene actor-init info's current PlayerHolder supplies Mario; pose keeper+50 owns position/quat.
// Validate each sample, clear the player on scene/world changes, reject invalid/torn poses.

#pragma once

#include "core/mods/dsmod_module_abi.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace SmoReader {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

inline constexpr u64 TitleId = UINT64_C(0x0100000000010000);

// CORRECTED by an empirical boot-time check ("log build_hex from a stub module on first boot").
// The initial assumption was that the host's build identity follows the Atmosphere/homebrew-cheat
// convention,
// SHA256(SHA256(.text)||SHA256(.rodata)||SHA256(.data)) = FBD61FC2F7E12357592844FEC7B37790...
// That is NOT what this host actually uses. A first boot of this module with that hash in
// manifest.json's module.build_ids produced "DSMod module: dual-screen module does not support
// this game build" and the host's own per-build-data-file log line named the running build
// 'B424BE150A8E7D78' (16 chars) -- the first 16 hex characters of the NSO's own module-id field
// at file offset 0x40 (`host->build_id`, 0x20 raw bytes: B424BE150A8E7D78701CBE7A439D9EBF
// followed by 16 zero bytes), not the cheat-convention hash. The brief's own section 0 flagged
// this exact field as "non-degenerate" without realizing it -- not the cheat convention -- is
// what ModRuntime::Discover / the module ABI's supports_build()/host->build_id actually carry.
// See packages/SuperMarioOdyssey/README.md "Build identity correction" for the full story.
inline constexpr std::string_view Build64 =
    "B424BE150A8E7D78701CBE7A439D9EBF00000000000000000000000000000000";

// Offsets from main_base, RE-confirmed live. RTTI-confirmed primary vtable address points:
//   StageScene      main+0x12C4DC8  (1 vtable)
//   GameDataHolder  main+0x12D2E78  (1 vtable; not used directly -- reached via StageScene)
inline constexpr u64 kStageSceneVtableOffset = 0x12C4DC8;
// RE batch 5: one GameSystem scan per boot; its sequence owns the current
// StageScene.
inline constexpr u64 kGameSystemVtableOffset = 0x12D2F78;
inline constexpr u64 kGameSystemToSequence = 0x10;
inline constexpr u64 kSequenceToStageScene = 0x78;
inline constexpr u64 kPlayerVtableOffset = 0x12B1EF8;
// al::LiveActor::isDead (65d5b0) reads byte0 of this actor-owned flags object.
inline constexpr u64 kPlayerActorFlags = 0x100;
// Native getTrans66d810 follows actor+50, then returns pose+8. Quaternion
// pose keepers' virtualslot28 getters66d570/66d5d0/66d650 return pose+14.
// Earlier actor+6a8/+6b4 observations were heap adjacency, not actor fields.
inline constexpr u64 kPlayerPoseKeeper = 0x50;
inline constexpr u64 kPosePosition = 0x8;
inline constexpr u64 kPoseRotation = 0x14;
inline constexpr std::array<u64, 3> kQuaternionPoseVtables{0x12EFDD0, 0x12EFE90, 0x12EFF50};
// Scene camera getter74c9c0 reads [scene+90]+78. Its IUseCamera
// subobject at+18 uses74c9d0 ([this+78]+78). Native camera getters
// 5b3e60/5b3ec0 traverse director+10 -> holder+8 -> array[0] -> camera+8.
inline constexpr u64 kStageSceneActorInitInfo = 0x90;
// Native scene getter74d320 and PlayerHolder index0 getter713050.
inline constexpr u64 kSceneInfoPlayerHolder = 0x98;
inline constexpr u64 kSceneInfoCameraDirector = 0x78;
inline constexpr u64 kCameraDirectorVtableOffset = 0x12DE5F8;
inline constexpr u64 kCameraPoseVtableOffset = 0x1353CD8; // sead::LookAtCamera
inline constexpr u64 kCameraPosition = 0x38;
inline constexpr u64 kStageSceneName = 0xEC; // inline FixedSafeString<64> buffer (RE batch 5)
inline constexpr u64 kStageSceneToGameDataHolder = 0x2E0;
inline constexpr u64 kGameDataHolderToGameData = 0x20;
inline constexpr u64 kGameDataCoinNum = 0x5D8;
// Current world, as used by the native regional counter, checkpoint lookup and
// ShineCounter. +A3C can agree after loading a save but is not the current-world
// accessor used by those systems across travel.
inline constexpr u64 kGameDataWorldId = 0xA38;
// Native getters 4d38f0/4d3910 -> 4c8530 -> PlayerHitPointData getters.
inline constexpr u64 kGameDataToHitPointData = 0x8A8;
inline constexpr u64 kHitPointVtableOffset = 0x12D31A8;
inline constexpr u64 kStageSceneToNerveKeeper = 0x8;
inline constexpr u64 kStageScenePauseNerve = 0x12C5E48;
inline constexpr u64 kStageScenePlayNerve = 0x12C5EB8;
inline constexpr u64 kStageSceneCollectionListNerve = 0x12C5DA0;
inline constexpr u64 kStageSceneToCollectionList = 0x170;
inline constexpr u64 kCollectionListVtableOffset = 0x12C6E28;
inline constexpr u64 kStageSceneActiveState = 0x180;
inline constexpr u64 kPauseMenuVtableOffset = 0x12CA020;
inline constexpr u64 kStageMapVtableOffset = 0x12CB708;

// ---- context detection (2026-10-23 RE batch 8/9): StageScene's fixed-order state pool ---------
// StageScene+0x138..+0x180 holds one always-allocated StageSceneState* per top-level screen
// (idle objects when not current). Every pointed-to object shares the same base layout: +0x00
// vtable, +0x08 own NerveKeeper*, +0x10 u8 "ended" (1 = idle/not current, 0 = currently active --
// live-confirmed 1<->0<->1 transitions for PauseMenu, CollectionList and WorldMap). This is the
// robust signal (not a raw pointer's stale content, the earlier +0x180 dead end RE batch 5 found
// and this module never relied on -- see 0100000000010000_test.cpp's deliberately-stale decoys).
inline constexpr u64 kStageSceneToWorldMap = 0x138;
inline constexpr u64 kWorldMapVtableOffset = 0x12CCAF8;
inline constexpr u64 kStateToOwnNerveKeeper = 0x8;  // same offset on every pool object
inline constexpr u64 kStateToEnded = 0x10;          // same offset on every pool object
// WorldMap's own cursor fields (RE batch 9, 6 live data points incl. a non-adjacent jump).
inline constexpr u64 kWorldMapCursorPosition = 0x100; // u32: 0-based ribbon list position
inline constexpr u64 kWorldMapCursorTable = 0x108;    // u32[]*: position -> kGameDataWorldId
inline constexpr unsigned kWorldMapCursorTableMax = 17; // 0 (Cap) .. 16 (Darker Side)
// CollectionList's own NerveKeeper (CollectionList+0x08, already read for the StageMap
// sub-object path above) also carries the active List sub-page once List mode is open. Live
// values (RE batch 9 §4), superseding batch 8's unconfirmed static RTTI-name guesses.
inline constexpr u64 kCollectionNerveMap = 0x12C7050;
inline constexpr u64 kCollectionNervePowerMoon = 0x12C7048;
inline constexpr u64 kCollectionNerveCapture = 0x12C7068;
inline constexpr u64 kCollectionNerveSouvenir = 0x12C7070;
inline constexpr u64 kCollectionNerveMusic = 0x12C7058;

enum class CollectionPage : s32 { None, Map, PowerMoon, Capture, Souvenir, Music };

// TODO(shop page): StageScene+0x140 is the Shop pool slot (StageSceneStateShop, vtable_addr_point
// main+0x12CB398), same base layout as WorldMap/CollectionList above. ITS open trigger is NOT
// proven: a search across two kingdoms (Mushroom ship deck, Luncheon Peronza Plaza festival)
// never observed the +0x10 "ended" byte flip 1->0. Do not wire shop detection from these two
// constants alone -- re-run the live search first. Left here only as the known pool-slot address,
// not as a working reader.
inline constexpr u64 kStageSceneToShop = 0x140;
inline constexpr u64 kShopVtableOffset = 0x12CB398;

inline constexpr u64 kHakoniwaSequenceVtableOffset = 0x12CF9E8;
inline constexpr u64 kTitleMenuSceneVtableOffset = 0x12CD510;
enum class SceneMode : s32 { Unknown, Playing, Paused, StageMap, Other, Title, Loading };

// A coin count outside this range cannot be real (the HUD is a 4-digit field; the native save
// reader's own range checks use the same ceiling) -- used only to reject an obviously torn or
// miscomputed read, never to clamp a real value.
inline constexpr s64 kCoinSanityMax = 9999;

/// True iff `hex` (as the host's supports_build()/host->build_id give it, upper-case, no
/// separators) is exactly the one build this module was written against. Stricter than the
/// package's own module.build_ids "starts with" check (MODULE_GUIDE.md section 3), matching the
/// P5R precedent.
bool SupportsBuildHex(const char* hex);
bool SupportsBuildBytes(const u8 build_id[0x20]);

/// One sample's worth of published state. Cleared (every field reset to "unavailable") at the
/// start of every Reader::Sample call; MODULE_GUIDE.md section 1.2 "the snapshot is cleared every
/// tick" means a stale value must never be left for the host to re-publish.
struct Snapshot {
    bool chain_ready = false;  ///< StageScene -> GameDataHolder -> gameData all resolved and sane
    bool health_valid = false;
    s32 health = 0;
    s32 health_max = 0; ///< native capacity: 3/6 normally, up to 9 in Assist + Life-Up
    u64 hit_point_data = 0;
    bool scene_mode_valid = false;
    SceneMode scene_mode = SceneMode::Unknown;
    u64 current_scene = 0;
    u64 sequence_nerve = 0;
    u64 map_state = 0;
    u64 map_nerve = 0;
    u64 scene_nerve = 0; ///< diagnostic: native effective (pending or current) nerve
    bool coins_valid = false;  ///< CoinNum read twice, matched, in range
    s64 coins = 0;
    s32 world_id = -1;         ///< -1 == unknown; used only to detect a stage change
    u64 game_data_holder = 0; ///< validated active-save owner
    u64 game_data = 0;        ///< diagnostic: validated current save-file data owner
    u64 stage_scene = 0;       ///< diagnostic: the resolved address this sample used (0 = none)
    u64 scan_pages_this_sample = 0; ///< diagnostic / perf measurement
    bool player_valid = false;
    std::array<float, 3> player_position{};
    std::array<float, 3> player_front{};
    double player_heading_deg = 0; ///< clockwise from world -Z, before map projection
    u64 player = 0;
    u64 player_pose_keeper = 0;
    bool camera_valid = false;
    std::array<float, 3> camera_front{}; ///< native target minus eye, normalized
    std::string stage_name;

    // ---- context detection (RE batch 8/9) -- see the state-pool comment above --------------
    bool globe_open = false; ///< WorldMap pool slot's own "ended" byte == 0
    bool globe_cursor_valid = false; ///< the cursor's WorldId was read, tear-checked and in range
    s32 globe_cursor_world_id = -1;  ///< kGameDataWorldId of the kingdom under the native cursor
    /// Which Collection List sub-page is open (StageScene+0x170, Minus menu). None when the
    /// CollectionList itself is closed; Map covers the existing scene_mode==StageMap case too
    /// (kept separately so callers do not have to re-derive it from scene_mode).
    CollectionPage collection_page = CollectionPage::None;
};

/// Read-only chain resolver + per-sample reader. One instance per module instance; not
/// thread-safe (the host serializes sample()/tick() on its own tick thread, per MODULE_GUIDE.md
/// section 1.2).
class Reader {
public:
    /// Runs the bounded heap-scan slice (if a scan is active) and/or walks + validates the chain,
    /// filling `out`. Never throws (callers must still wrap in try/catch at the ABI boundary,
    /// per the module checklist, but every internal failure here degrades to "unavailable").
    void Sample(const EdenDsmodHostApi& host, Snapshot& out);

    // ---- test-only introspection (ABI-stable fields, used by 0100000000010000_test.cpp) ----
    u64 TestStageScene() const {
        return stage_scene_;
    }
    bool TestScanActive() const {
        return scan_active_;
    }
    u64 TestScanCursor() const {
        return scan_cursor_;
    }

private:
    bool Read(const EdenDsmodHostApi& host, u64 at, void* out, u64 size) const;
    template <class T>
    bool Get(const EdenDsmodHostApi& host, u64 at, T& out) const {
        return Read(host, at, &out, sizeof out);
    }

    void StartScan(const EdenDsmodHostApi& host);
    /// Bounded scan for the boot's GameSystem vtable.
    void RunScanSlice(const EdenDsmodHostApi& host);
    /// StageScene's own vtable word still equals the expected vtable (the block was not freed and
    /// reused for something else since the last sample).
    bool ValidateStageScene(const EdenDsmodHostApi& host) const;

    bool HasVtable(const EdenDsmodHostApi& host, u64 object, u64 offset) const;
    bool PlayerBelongsToScene(const EdenDsmodHostApi& host, u64 player) const;
    u64 ResolvePlayer(const EdenDsmodHostApi& host) const;
    void ReadPlayer(const EdenDsmodHostApi& host, Snapshot& out) const;
    void ReadCamera(const EdenDsmodHostApi& host, Snapshot& out) const;
    void ReadHealth(const EdenDsmodHostApi& host, u64 game_data, Snapshot& out) const;
    void ReadSceneMode(const EdenDsmodHostApi& host, Snapshot& out) const;
    void ReadSequenceMode(const EdenDsmodHostApi& host, u64 sequence, u64 scene, Snapshot& out) const;
    /// Independent pool-slot reads for the new context pages. Each fails closed to its default
    /// (globe_open=false / collection_page=None) on any missing page, vtable mismatch or torn
    /// read -- never a guess, per the owner's explicit "never show a context page on a guess".
    void ReadWorldMapState(const EdenDsmodHostApi& host, Snapshot& out) const;
    void ReadCollectionPage(const EdenDsmodHostApi& host, Snapshot& out) const;

    u64 main_base_ = 0;
    u64 stage_scene_ = 0; ///< 0 = not resolved
    u64 game_system_ = 0;
    u64 player_ = 0;
    s32 last_world_id_ = -1;

    bool scan_active_ = false;
    u64 scan_cursor_ = 0;
    u64 scan_end_ = 0;
    // Starts already "expired" so the very first sample that needs a scan starts one right away;
    // only a *failed* full pass re-arms the backoff (see RunScanSlice / kScanIdleTicks).
    int scan_idle_ticks_ = 1 << 30;
    u64 last_scan_pages_ = 0; ///< pages the most recent RunScanSlice looked at (perf diagnostic)
};

} // namespace SmoReader
