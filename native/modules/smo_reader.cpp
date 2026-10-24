// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "0100000000010000.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace SmoReader {
namespace {

// 2048 x 4 KiB pages per sample = 8 MiB of heap scanned per tick (480 MiB/s at 60 Hz), the same
// budget CtrReader::Reader::ScanForWidget uses (ctr_reader.cpp ScanPagesPerSample). SMO's heap is
// larger than CTR's ("this game's heap is larger -- 9.9 MB main, big asset set"), so a full
// first pass can take longer; this was measured on a desktop test harness rather than guessing a
// bigger per-tick budget that would compete harder
// with the game for tick time (MODULE_GUIDE.md section 6 "lazy publishing... tick time competes
// with the game").
constexpr u64 kScanPagesPerSample = 2048;
constexpr u64 kPageSize = 0x1000;
constexpr u64 kWordsPerPage = kPageSize / 8;
// A failed full pass (StageScene not up yet -- title screen, loading) waits this many ticks
// before scanning again, so a slow boot does not spend every tick re-scanning the whole heap.
constexpr int kScanIdleTicks = 90; // 1.5 s at 60 Hz

} // namespace

bool SupportsBuildHex(const char* hex) {
    if (!hex) {
        return false;
    }
    // Exact match only (upper-case, as the host documents build_hex): stricter than the
    // package's own module.build_ids "starts with" check, matching the P5R precedent
    // (MODULE_GUIDE.md section 3). A module must never imply support for an unlisted build.
    return Build64 == std::string_view{hex};
}

bool SupportsBuildBytes(const u8 build_id[0x20]) {
    // host->build_id is 32 raw bytes; Build64 is their upper-case hex form. Compare byte for
    // byte instead of re-hex-encoding, so there is no formatting edge case to get wrong.
    if (Build64.size() != 64) {
        return false;
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; ++i) {
        const u8 b = build_id[i];
        if (Build64[static_cast<size_t>(i) * 2] != kHex[b >> 4] ||
            Build64[static_cast<size_t>(i) * 2 + 1] != kHex[b & 0xF]) {
            return false;
        }
    }
    return true;
}

bool Reader::Read(const EdenDsmodHostApi& host, u64 at, void* out, u64 size) const {
    if (at == 0 || size == 0 || at > UINT64_MAX - size) {
        return false;
    }
    if (!host.is_mapped || !host.read_memory) {
        return false;
    }
    if (!host.is_mapped(host.userdata, at, size)) {
        return false;
    }
    return host.read_memory(host.userdata, at, out, size) != 0;
}

void Reader::StartScan(const EdenDsmodHostApi& host) {
    scan_cursor_ =
        host.get_heap_begin ? (host.get_heap_begin(host.userdata) & ~(kPageSize - 1)) : 0;
    scan_end_ = host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
    scan_active_ = scan_cursor_ != 0 && scan_end_ > scan_cursor_;
}

bool Reader::HasVtable(const EdenDsmodHostApi& host, u64 object, u64 offset) const {
    u64 vtable = 0;
    if (!Read(host, object, &vtable, sizeof vtable))
        return false;
    const u64 want = main_base_ + offset;
    const s64 delta = host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    return vtable == want || vtable == want + static_cast<u64>(delta);
}

bool Reader::ValidateStageScene(const EdenDsmodHostApi& host) const {
    return HasVtable(host, stage_scene_, kStageSceneVtableOffset);
}

bool Reader::PlayerBelongsToScene(const EdenDsmodHostApi& host, u64 player) const {
    // Both classes implement al::IUseSceneObjHolder at +20 (native RTTI).
    // Player's getter 67fb40: [this+D8]+60; Scene's getter 4478a0:
    // [this+80]. Thus [player+F8]+60 must equal [scene+A0]. Old scene
    // allocations can retain a perfectly valid PlayerActorHakoniwa vtable.
    // A global first-vtable match alone can otherwise freeze the GPS after travel.
    // A substage can recycle the same scene/holder addresses while retired
    // player allocations retain their vtable and owner. Native makeAlive
    // (67f280) clears flags[0], makeDead (67f3f0) sets it, and isDead
    // (65d5b0) reads it through actor+100. Reject dead actors as an additional
    // guard; ResolvePlayer selects the current actor even when a retired one stays alive.
    u64 info = 0, info_check = 0, player_owner = 0, scene_owner = 0, check = 0;
    u64 flags = 0;
    u8 dead = 1, dead_check = 1;
    return stage_scene_ && Get(host, player + kPlayerActorFlags, flags) && flags &&
        Get(host, flags, dead) && dead == 0 &&
        Get(host, player + 0xF8, info) && info &&
        Get(host, info + 0x60, player_owner) && player_owner &&
        Get(host, stage_scene_ + 0xA0, scene_owner) && scene_owner == player_owner &&
        Get(host, player + 0xF8, info_check) && info == info_check &&
        Get(host, info + 0x60, check) && check == player_owner &&
        Get(host, stage_scene_ + 0xA0, check) && check == scene_owner &&
        Get(host, player + kPlayerActorFlags, check) && check == flags &&
        Get(host, flags, dead_check) && dead_check == dead;
}

u64 Reader::ResolvePlayer(const EdenDsmodHostApi& host) const {
    // The scene's native PlayerHolder is authoritative. Retired actors can stay
    // alive with valid vtables, poses and recycled SceneObjHolder ownership after
    // a substage roundtrip, so heap matching is insufficient even with liveness.
    // 74d320: [[scene+90]+98]; 713050: holder array[0] when capacity/count >0.
    u64 info = 0, holder = 0, array = 0, player = 0, check = 0;
    std::array<s32, 2> counts{}, count_check{};
    if (!stage_scene_ || !Get(host, stage_scene_ + kStageSceneActorInitInfo, info) || !info ||
        !Get(host, info + kSceneInfoPlayerHolder, holder) || !holder ||
        !Get(host, holder + 8, counts) || counts[0] < 1 || counts[1] < 1 || counts[1] > counts[0] ||
        !Get(host, holder, array) || !array || !Get(host, array, player) ||
        !HasVtable(host, player, kPlayerVtableOffset) || !PlayerBelongsToScene(host, player) ||
        !Get(host, array, check) || check != player ||
        !Get(host, holder, check) || check != array ||
        !Get(host, holder + 8, count_check) || counts != count_check ||
        !Get(host, info + kSceneInfoPlayerHolder, check) || check != holder ||
        !Get(host, stage_scene_ + kStageSceneActorInitInfo, check) || check != info) return 0;
    return player;
}

void Reader::ReadPlayer(const EdenDsmodHostApi& host, Snapshot& out) const {
    if (!out.chain_ready || !HasVtable(host, player_, kPlayerVtableOffset) || !PlayerBelongsToScene(host, player_))
        return;
    u64 keeper = 0, keeper_check = 0, vtable = 0, vtable_check = 0;
    if (!Get(host, player_ + kPlayerPoseKeeper, keeper) || !keeper ||
        !Get(host, keeper, vtable) ||
        std::none_of(kQuaternionPoseVtables.begin(), kQuaternionPoseVtables.end(),
            [&](u64 offset) { return HasVtable(host, keeper, offset); })) return;
    std::array<float, 7> pose{}, check{};
    if (!Get(host, keeper + kPosePosition, pose) ||
        !Get(host, keeper + kPosePosition, check) || pose != check ||
        !Get(host, player_ + kPlayerPoseKeeper, keeper_check) || keeper != keeper_check ||
        !Get(host, keeper, vtable_check) || vtable != vtable_check)
        return;
    for (const float value : pose)
        if (!std::isfinite(value)) return;
    const std::array<float, 3> position{pose[0], pose[1], pose[2]};
    for (const float value : position)
        if (std::abs(value) > 1.0e7f) return;
    const float x = pose[3], y = pose[4], z = pose[5], w = pose[6];
    const float norm = x*x + y*y + z*z + w*w;
    if (norm < 0.25f || norm > 2.25f) return;
    // Rotate local +Z by the actor's XYZW quaternion (matrix column 2).
    const std::array<float, 3> front{2*(x*z + w*y)/norm,
                                   2*(y*z - w*x)/norm,
                                   1 - 2*(x*x + y*y)/norm};
    if (front[0]*front[0] + front[2]*front[2] < 0.01f) return;
    if (ResolvePlayer(host) != player_) return;
    out.player_valid = true;
    out.player = player_;
    out.player_pose_keeper = keeper;
    out.player_position = position;
    out.player_front = front;
    out.player_heading_deg = std::atan2(front[0], -front[2]) * 180.0 / std::numbers::pi;
}

void Reader::ReadCamera(const EdenDsmodHostApi& host, Snapshot& out) const {
    if (!out.chain_ready) return;
    u64 info = 0, director = 0, holder = 0, array = 0, camera = 0, pose = 0, check = 0;
    if (!Get(host, stage_scene_ + kStageSceneActorInitInfo, info) || !info ||
        !Get(host, info + kSceneInfoCameraDirector, director) ||
        !HasVtable(host, director, kCameraDirectorVtableOffset) ||
        !Get(host, director + 0x10, holder) || !holder ||
        !Get(host, holder + 8, array) || !array || !Get(host, array, camera) || !camera ||
        !Get(host, camera + 8, pose) || !HasVtable(host, pose, kCameraPoseVtableOffset)) return;
    // Native MapMini::exeWait1d54d8..1d5530 uses camera0's target(+44)
    // minus eye(+38), independently of its earlier Mario-facing calculation.
    std::array<float, 6> view{}, view_check{};
    if (!Get(host, pose + kCameraPosition, view) ||
        !Get(host, pose + kCameraPosition, view_check) || view != view_check ||
        !Get(host, camera + 8, check) || check != pose ||
        !Get(host, array, check) || check != camera ||
        !Get(host, holder + 8, check) || check != array ||
        !Get(host, director + 0x10, check) || check != holder ||
        !Get(host, info + kSceneInfoCameraDirector, check) || check != director ||
        !Get(host, stage_scene_ + kStageSceneActorInitInfo, check) || check != info ||
        !HasVtable(host, director, kCameraDirectorVtableOffset) ||
        !HasVtable(host, pose, kCameraPoseVtableOffset)) return;
    for (float value : view) if (!std::isfinite(value) || std::abs(value) > 1.0e7f) return;
    const double dx = double(view[3]) - view[0], dy = double(view[4]) - view[1];
    const double dz = double(view[5]) - view[2];
    const double norm = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (norm < 1.0e-6 || (dx*dx + dz*dz)/(norm*norm) < 0.01) return;
    out.camera_front = {float(dx/norm), float(dy/norm), float(dz/norm)};
    out.camera_valid = true;
}

void Reader::ReadHealth(const EdenDsmodHostApi& host, u64 game_data, Snapshot& out) const {
    u64 hp = 0, hp_check = 0;
    std::array<u8, 12> data{}, check{};
    if (!Get(host, game_data + kGameDataToHitPointData, hp) ||
        !HasVtable(host, hp, kHitPointVtableOffset) ||
        !Get(host, hp + 8, data) || !Get(host, hp + 8, check) || data != check ||
        !Get(host, game_data + kGameDataToHitPointData, hp_check) || hp != hp_check)
        return;
    // Match native PlayerHitPointData::getMax (main+4e48f0). Do not infer capacity
    // from the current count: Life-Up and Assist Mode have separate native flags.
    if (data[0] > 1 || data[8] > 1 || data[9] > 1) return;
    s32 current = 0;
    std::memcpy(&current, data.data() + 4, sizeof current);
    const s32 maximum = data[9] ? 3 : (data[0] ? 6 : 3) + (data[8] ? 3 : 0);
    if (current < 0 || current > maximum) return;
    out.health_valid = true;
    out.health = current;
    out.health_max = maximum;
    out.hit_point_data = hp;
}

void Reader::ReadSceneMode(const EdenDsmodHostApi& host, Snapshot& out) const {
    u64 keeper = 0, keeper_check = 0;
    std::array<u64, 2> nerves{}, check{};
    if (!Get(host, stage_scene_ + kStageSceneToNerveKeeper, keeper) ||
        !Get(host, keeper + 8, nerves) || !Get(host, keeper + 8, check) || nerves != check ||
        !Get(host, stage_scene_ + kStageSceneToNerveKeeper, keeper_check) || keeper != keeper_check)
        return;
    // Native getNerve (main+6c77a0) prefers pending+10, otherwise current+8.
    const u64 nerve = nerves[1] ? nerves[1] : nerves[0];
    const s64 delta = host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    const auto matches = [&](u64 off) {
        return nerve == main_base_ + off || nerve == main_base_ + off + static_cast<u64>(delta);
    };
    // The contiguous native singleton table contains exactly 56 StageScene nerves.
    const auto in_table = [&](u64 base) {
        return nerve >= base + 0x12C5D90 && nerve <= base + 0x12C5F48 &&
               (nerve - (base + 0x12C5D90)) % 8 == 0;
    };
    if (!in_table(main_base_) && !in_table(main_base_ + static_cast<u64>(delta))) return;
    out.scene_nerve = nerve;
    out.scene_mode_valid = true;
    out.scene_mode = SceneMode::Other;
    if (matches(kStageScenePlayNerve)) out.scene_mode = SceneMode::Playing;
    if (matches(kStageScenePauseNerve)) out.scene_mode = SceneMode::Paused;
    // Minus opens CollectionList, whose owned StageMap is at +38. The PauseMenu
    // pointer at StageScene+180 can remain stale and is not this map's owner.
    if (matches(kStageSceneCollectionListNerve)) {
        u64 collection = 0, state = 0, owner = 0, map_keeper = 0;
        u64 collection_check = 0, state_check = 0, owner_check = 0;
        u8 ended = 1, ended_check = 1;
        std::array<u64, 2> map_nerves{}, map_check{};
        if (!Get(host, stage_scene_ + kStageSceneToCollectionList, collection) ||
            !HasVtable(host, collection, kCollectionListVtableOffset) ||
            !Get(host, collection + 0x38, state) || !HasVtable(host, state, kStageMapVtableOffset) ||
            !Get(host, state + 0x18, owner) || owner != stage_scene_ ||
            !Get(host, state + 0x10, ended) || ended != 0 ||
            !Get(host, state + 8, map_keeper) || !Get(host, map_keeper + 8, map_nerves) ||
            !Get(host, map_keeper + 8, map_check) || map_nerves != map_check ||
            !Get(host, state + 0x10, ended_check) || ended_check != ended ||
            !Get(host, state + 0x18, owner_check) || owner_check != owner ||
            !Get(host, collection + 0x38, state_check) || state_check != state ||
            !Get(host, stage_scene_ + kStageSceneToCollectionList, collection_check) ||
            collection_check != collection) return;
        const u64 map_nerve = map_nerves[1] ? map_nerves[1] : map_nerves[0];
        const auto map_in_table = [&](u64 base) {
            return map_nerve >= base + 0x12CBAE8 && map_nerve <= base + 0x12CBB50 &&
                   (map_nerve - (base + 0x12CBAE8)) % 8 == 0;
        };
        if (!map_in_table(main_base_) && !map_in_table(main_base_ + static_cast<u64>(delta))) return;
        out.scene_mode = SceneMode::StageMap;
        out.map_state = state;
        out.map_nerve = map_nerve;
    }
}

void Reader::ReadWorldMapState(const EdenDsmodHostApi& host, Snapshot& out) const {
    // Pool slot pointer is always allocated (StageScene+0x138, idle when not current) -- only
    // the object it points to tells us whether the globe is actually open (+0x10 ended).
    u64 world_map = 0;
    if (!Get(host, stage_scene_ + kStageSceneToWorldMap, world_map) || world_map == 0 ||
        !HasVtable(host, world_map, kWorldMapVtableOffset))
        return;
    u8 ended = 1, ended_check = 1;
    if (!Get(host, world_map + kStateToEnded, ended) ||
        !Get(host, world_map + kStateToEnded, ended_check) || ended != ended_check)
        return; // torn read: stay closed rather than guess
    if (ended != 0)
        return; // globe not open: leave globe_open=false, cursor unknown
    out.globe_open = true;

    // Cursor: a 0-based ribbon list position (+0x100), looked up through its own table
    // (+0x108 -> u32[]) into kGameDataWorldId -- NOT the WorldId directly (RE batch 9: the
    // ribbon's display order is the story-visit order, which differs from the raw WorldId
    // order at at least one point, Lake before Wooded).
    u32 position = 0, position_check = 0;
    u64 table = 0, table_check = 0;
    if (!Get(host, world_map + kWorldMapCursorPosition, position) ||
        !Get(host, world_map + kWorldMapCursorPosition, position_check) || position != position_check ||
        position >= kWorldMapCursorTableMax ||
        !Get(host, world_map + kWorldMapCursorTable, table) ||
        !Get(host, world_map + kWorldMapCursorTable, table_check) || table != table_check || table == 0)
        return;
    u32 world_id = 0, world_id_check = 0;
    const u64 slot = table + static_cast<u64>(position) * sizeof(u32);
    if (!Get(host, slot, world_id) || !Get(host, slot, world_id_check) || world_id != world_id_check ||
        world_id > 16)
        return;
    out.globe_cursor_valid = true;
    out.globe_cursor_world_id = static_cast<s32>(world_id);
}

void Reader::ReadCollectionPage(const EdenDsmodHostApi& host, Snapshot& out) const {
    u64 collection = 0;
    if (!Get(host, stage_scene_ + kStageSceneToCollectionList, collection) || collection == 0 ||
        !HasVtable(host, collection, kCollectionListVtableOffset))
        return;
    u8 ended = 1, ended_check = 1;
    if (!Get(host, collection + kStateToEnded, ended) ||
        !Get(host, collection + kStateToEnded, ended_check) || ended != ended_check || ended != 0)
        return; // CollectionList itself is closed: leave collection_page == None

    u64 keeper = 0, keeper_check = 0;
    std::array<u64, 2> nerves{}, check{};
    if (!Get(host, collection + kStateToOwnNerveKeeper, keeper) ||
        !Get(host, keeper + 8, nerves) || !Get(host, keeper + 8, check) || nerves != check ||
        !Get(host, collection + kStateToOwnNerveKeeper, keeper_check) || keeper != keeper_check)
        return;
    const u64 nerve = nerves[1] ? nerves[1] : nerves[0];
    const s64 delta = host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    const auto matches = [&](u64 off) {
        return nerve == main_base_ + off || nerve == main_base_ + off + static_cast<u64>(delta);
    };
    if (matches(kCollectionNerveMap)) out.collection_page = CollectionPage::Map;
    else if (matches(kCollectionNervePowerMoon)) out.collection_page = CollectionPage::PowerMoon;
    else if (matches(kCollectionNerveCapture)) out.collection_page = CollectionPage::Capture;
    else if (matches(kCollectionNerveSouvenir)) out.collection_page = CollectionPage::Souvenir;
    else if (matches(kCollectionNerveMusic)) out.collection_page = CollectionPage::Music;
    // An unrecognised nerve (e.g. a mid-transition frame) leaves collection_page == None: the
    // CollectionList is open, but which page is unknown this tick -- never guessed.
}

void Reader::ReadSequenceMode(const EdenDsmodHostApi& host, u64 sequence, u64 scene,
                              Snapshot& out) const {
    out.current_scene = scene;
    if (HasVtable(host, scene, kTitleMenuSceneVtableOffset)) {
        out.scene_mode_valid = true;
        out.scene_mode = SceneMode::Title;
        return;
    }
    if (!HasVtable(host, sequence, kHakoniwaSequenceVtableOffset)) return;
    u64 keeper = 0, keeper_check = 0;
    std::array<u64, 2> nerves{}, check{};
    if (!Get(host, sequence + 8, keeper) || !Get(host, keeper + 8, nerves) ||
        !Get(host, keeper + 8, check) || nerves != check ||
        !Get(host, sequence + 8, keeper_check) || keeper != keeper_check) return;
    const u64 nerve = nerves[1] ? nerves[1] : nerves[0];
    const s64 delta = host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    const auto matches = [&](u64 off) {
        return nerve == main_base_ + off || nerve == main_base_ + off + static_cast<u64>(delta);
    };
    // Exact RTTI-confirmed singleton identities, not "scene absent means loading".
    for (const u64 off : {0x12D0298ULL, 0x12D0420ULL, 0x12D0448ULL, 0x12D0450ULL,
                         0x12D0458ULL, 0x12D0460ULL, 0x12D0468ULL, 0x12D0470ULL,
                         0x12D0478ULL, 0x12D0480ULL, 0x12D0488ULL, 0x12D0490ULL,
                         0x12D0498ULL, 0x12D04A0ULL, 0x12D04A8ULL, 0x12D04B0ULL,
                         0x12D04B8ULL, 0x12D04C0ULL, 0x12D04D0ULL, 0x12D04D8ULL,
                         0x12D04E0ULL, 0x12D04E8ULL, 0x12D04F0ULL, 0x12D04F8ULL,
                         0x12D0500ULL, 0x12D0508ULL}) {
        if (matches(off)) {
            out.sequence_nerve = nerve;
            out.scene_mode_valid = true;
            out.scene_mode = SceneMode::Loading;
            return;
        }
    }
}

void Reader::RunScanSlice(const EdenDsmodHostApi& host) {
    const u64 want = main_base_ + kGameSystemVtableOffset;
    const s64 delta = host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    const u64 want_delta = want + static_cast<u64>(delta);
    u64 page_buf[kWordsPerPage];
    u64 pages_done = 0;
    last_scan_pages_ = 0;
    for (; pages_done < kScanPagesPerSample && scan_cursor_ < scan_end_;
         ++pages_done, scan_cursor_ += kPageSize) {
        const u64* words = nullptr;
        if (host.get_read_pointer) {
            words = reinterpret_cast<const u64*>(
                host.get_read_pointer(host.userdata, scan_cursor_, kPageSize));
        }
        if (!words) {
            if (!host.is_mapped || !host.is_mapped(host.userdata, scan_cursor_, kPageSize) ||
                !host.read_memory ||
                !host.read_memory(host.userdata, scan_cursor_, page_buf, sizeof page_buf)) {
                continue;
            }
            words = page_buf;
        }
        for (u64 w = 0; w < kWordsPerPage; ++w) {
            const u64 v = words[w];
            if (!game_system_ && (v == want || v == want_delta))
                game_system_ = scan_cursor_ + w * 8;
            if (game_system_) {
                scan_active_ = false;
                ++pages_done;              // count this partial page too
                scan_cursor_ += kPageSize; // resume after this page next time, if ever needed
                last_scan_pages_ = pages_done;
                return;
            }
        }
    }
    last_scan_pages_ = pages_done;
    if (scan_cursor_ >= scan_end_) {
        // A full pass found nothing (not in gameplay yet, or the offset moved):
        // stop for a while instead of re-scanning the whole heap every tick.
        scan_active_ = false;
        scan_idle_ticks_ = 0;
    }
}

void Reader::Sample(const EdenDsmodHostApi& host, Snapshot& out) {
    out = Snapshot{};

    if (host.main_base != main_base_) {
        // A reload or a different title/build under the same module instance
        // (should not normally happen -- create() is per-load -- but main_base is
        // the authoritative key, not an assumption): drop any resolved address and
        // start over.
        main_base_ = host.main_base;
        stage_scene_ = game_system_ = player_ = 0;
        scan_active_ = false;
        scan_idle_ticks_ = kScanIdleTicks;
        last_world_id_ = -1;
    }
    if (main_base_ == 0) {
        return; // host not ready yet
    }

    last_scan_pages_ = 0;
    if (game_system_ && !HasVtable(host, game_system_, kGameSystemVtableOffset)) {
        game_system_ = 0;
        scan_active_ = false;
        scan_idle_ticks_ = kScanIdleTicks;
    }
    // The sequence's live StageScene pointer is authoritative, even when an old
    // scene's allocation still has its original vtable. Never keep a dead scene
    // just because it maps.
    u64 sequence = 0, current_scene = 0;
    if (game_system_ && Get(host, game_system_ + kGameSystemToSequence, sequence) && sequence)
        Get(host, sequence + kSequenceToStageScene, current_scene);
    if (current_scene != stage_scene_) {
        if (stage_scene_) {
            player_ = 0;
            scan_active_ = false;
            scan_idle_ticks_ = kScanIdleTicks;
        }
        stage_scene_ = current_scene;
    }
    if (!ValidateStageScene(host))
        stage_scene_ = 0;

    if (!game_system_) {
        if (!scan_active_) {
            if (scan_idle_ticks_ < kScanIdleTicks)
                ++scan_idle_ticks_;
            else
                StartScan(host);
        }
        if (scan_active_)
            RunScanSlice(host);
    }
    // A scan may have resolved GameSystem this tick. Follow it immediately; failed or torn
    // links must also clear the scene found by the earlier walk.
    const u64 earlier_scene = stage_scene_;
    stage_scene_ = 0;
    sequence = 0;
    current_scene = 0;
    if (game_system_ && Get(host, game_system_ + kGameSystemToSequence, sequence) && sequence &&
        Get(host, sequence + kSequenceToStageScene, current_scene)) {
        ReadSequenceMode(host, sequence, current_scene, out);
        if (HasVtable(host, current_scene, kStageSceneVtableOffset)) stage_scene_ = current_scene;
    }
    if (stage_scene_ != earlier_scene) player_ = 0;
    out.stage_scene = stage_scene_;
    out.scan_pages_this_sample = last_scan_pages_;
    if (!stage_scene_)
        return;

    // 3. Walk the fixed chain from the resolved root. Each hop is bounds-checked
    // before the next dereference (MODULE_GUIDE.md section 6 "bounded, validated
    // reads").
    u64 game_data_holder = 0;
    if (!Get(host, stage_scene_ + kStageSceneToGameDataHolder, game_data_holder) ||
        game_data_holder == 0) {
        stage_scene_ = 0; // retry the authoritative sequence pointer next sample
        return;
    }
    u64 game_data = 0;
    if (!Get(host, game_data_holder + kGameDataHolderToGameData, game_data) || game_data == 0) {
        return; // GameDataHolder itself still looks valid; only this hop failed
                // this tick
    }

    s32 world_id = -1;
    if (!Get(host, game_data + kGameDataWorldId, world_id) || world_id < 0 || world_id > 16)
        return;
    if (last_world_id_ != -1 && world_id != last_world_id_) {
        player_ = 0;
        scan_active_ = false;
        scan_idle_ticks_ = kScanIdleTicks;
    }
    last_world_id_ = world_id;
    out.world_id = world_id;

    out.chain_ready = true;
    out.game_data_holder = game_data_holder;
    out.game_data = game_data;
    std::array<char, 64> stage{};
    if (Get(host, stage_scene_ + kStageSceneName, stage)) {
        const auto end = std::find(stage.begin(), stage.end(), '\0');
        if (end != stage.end() && std::all_of(stage.begin(), end, [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '_';
            })) out.stage_name.assign(stage.begin(), end);
    }
    player_ = ResolvePlayer(host);
    ReadPlayer(host, out);
    ReadCamera(host, out);
    ReadHealth(host, game_data, out);
    if (out.scene_mode != SceneMode::Loading) ReadSceneMode(host, out);
    if (out.scene_mode != SceneMode::Loading && out.scene_mode != SceneMode::Title) {
        ReadWorldMapState(host, out);
        ReadCollectionPage(host, out);
    }

    // 5. CoinNum, with torn-read rejection: read twice and require an exact match
    // plus a sane range (MODULE_GUIDE.md section 6 "Reject torn reads";
    // PORTING_A_GAME.md sentinel-test convention). A transient failure clears
    // validity instead of publishing yesterday's value.
    s32 coins_a = 0, coins_b = 0;
    if (Get(host, game_data + kGameDataCoinNum, coins_a) &&
        Get(host, game_data + kGameDataCoinNum, coins_b) && coins_a == coins_b && coins_a >= 0 &&
        coins_a <= kCoinSanityMax) {
        out.coins = coins_a;
        out.coins_valid = true;
    }
}

} // namespace SmoReader
