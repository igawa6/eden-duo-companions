// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-side tests for the SMO reader (ctest target dsmod-smo-reader). Plain assert()-based, like
// ctr_reader_test.cpp / isaac_reader_test.cpp -- no test framework dependency, built with
// -UNDEBUG (dsmod_add_test KEEP_ASSERTS) so the asserts are never compiled away.

#include "0100000000010000.h"

#include <cassert>
#include <cstring>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace SmoReader {
namespace {

/// A tiny fake guest address space: exactly the bytes Put() wrote are "mapped".
struct FakeMemory {
    std::unordered_map<u64, u8> bytes;
    s64 relocation_delta = 0;
    u64 tear_at = 0;
    int tear_reads = 0;

    template <class T>
    void Put(u64 at, T value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        for (size_t i = 0; i < sizeof value; ++i) {
            bytes[at + i] = p[i];
        }
    }

    /// Zero-fills the whole 4 KiB page containing `at`, so the production code's whole-page
    /// is_mapped(addr, 0x1000) check (the real emulator's heap pages are mapped page-granular)
    /// succeeds for it. Call before Put()-ing the fields that actually matter inside that page.
    void MapPage(u64 at) {
        const u64 page = at & ~u64{0xFFF};
        for (u64 i = 0; i < 0x1000; ++i) {
            bytes.emplace(page + i, u8{0});
        }
    }

    static EdenDsmodBool Mapped(void* p, u64 at, u64 size) {
        auto& m = *static_cast<FakeMemory*>(p);
        for (u64 i = 0; i < size; ++i) {
            if (!m.bytes.contains(at + i)) {
                return EDEN_DSMOD_FALSE;
            }
        }
        return EDEN_DSMOD_TRUE;
    }
    static EdenDsmodBool Read(void* p, u64 at, void* out, u64 size) {
        auto& m = *static_cast<FakeMemory*>(p);
        if (!Mapped(p, at, size)) {
            return EDEN_DSMOD_FALSE;
        }
        for (u64 i = 0; i < size; ++i) {
            static_cast<u8*>(out)[i] = m.bytes.at(at + i);
        }
        if (at == m.tear_at && ++m.tear_reads == 1) m.bytes[at] ^= 1;
        return EDEN_DSMOD_TRUE;
    }
    static s64 GetI64(void* p, const char* name, s64 fallback) {
        auto& m = *static_cast<FakeMemory*>(p);
        if (std::strcmp(name, "__relocation_delta") == 0) {
            return m.relocation_delta;
        }
        return fallback;
    }
};

constexpr u64 kHeapBegin = 0x200000000ull;
constexpr u64 kHeapEnd = kHeapBegin + (4ull << 20); // 4 MiB fake heap: a handful of scan slices
constexpr u64 kMainBase = 0x800000000ull;

EdenDsmodHostApi MakeHost(FakeMemory& mem) {
    EdenDsmodHostApi host{};
    host.userdata = &mem;
    host.main_base = kMainBase;
    host.is_mapped = FakeMemory::Mapped;
    host.read_memory = FakeMemory::Read;
    host.get_heap_begin = [](void*) -> u64 { return kHeapBegin; };
    host.get_heap_end = [](void*) -> u64 { return kHeapEnd; };
    host.get_i64 = FakeMemory::GetI64;
    return host;
}

void RunScanToCompletion(Reader& r, const EdenDsmodHostApi& host, Snapshot& snap, int max_samples) {
    for (int i = 0; i < max_samples; ++i) {
        r.Sample(host, snap);
        if (r.TestStageScene() != 0) {
            return;
        }
    }
}

void BuildGating() {
    assert(SupportsBuildHex(
        "B424BE150A8E7D78701CBE7A439D9EBF00000000000000000000000000000000"));
    assert(!SupportsBuildHex("0000000000000000000000000000000000000000000000000000000000000000"));
    assert(!SupportsBuildHex(nullptr));
    // A 16-char prefix (the package's own "starts with" grain) must NOT satisfy the module's own
    // stricter exact-64-char check (MODULE_GUIDE.md section 3: "should be stricter").
    assert(!SupportsBuildHex("B424BE150A8E7D78"));

    u8 bytes[0x20] = {0xB4, 0x24, 0xBE, 0x15, 0x0A, 0x8E, 0x7D, 0x78, 0x70, 0x1C, 0xBE,
                       0x7A, 0x43, 0x9D, 0x9E, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(SupportsBuildBytes(bytes));
    bytes[0] = 0x00;
    assert(!SupportsBuildBytes(bytes));
}

/// Writes a minimal StageScene -> GameDataHolder -> gameData chain into `mem`, with CoinNum and
/// WorldId set. `stage_scene_addr` must be 8-byte aligned (it is itself scanned as a heap word).
void WriteRoot(FakeMemory& mem, u64 stage_scene_addr) {
    constexpr u64 gs = kHeapBegin + 0x40000, seq = kHeapBegin + 0x50000;
    mem.MapPage(gs);
    mem.MapPage(seq);
    mem.Put<u64>(gs, kMainBase + kGameSystemVtableOffset + static_cast<u64>(mem.relocation_delta));
    mem.Put<u64>(gs + kGameSystemToSequence, seq);
    mem.Put<u64>(seq + kSequenceToStageScene, stage_scene_addr);
}

void WriteChain(FakeMemory& mem, u64 stage_scene_addr, u64 gdh_addr, u64 game_data_addr,
                s32 coins, s32 world_id) {
    WriteRoot(mem, stage_scene_addr);
    mem.MapPage(stage_scene_addr);
    mem.MapPage(gdh_addr);
    mem.MapPage(game_data_addr);
    mem.Put<u64>(stage_scene_addr, kMainBase + kStageSceneVtableOffset); // obj[0] == vtable
    mem.Put<u64>(stage_scene_addr + kStageSceneToGameDataHolder, gdh_addr);
    mem.Put<u64>(stage_scene_addr + 0xA0, stage_scene_addr + 0x900);
    mem.Put<u64>(stage_scene_addr + kStageSceneActorInitInfo, stage_scene_addr + 0xA00);
    mem.Put<u64>(stage_scene_addr + 0xA00 + kSceneInfoPlayerHolder, 0);
    std::array<char, 64> stage{};
    std::memcpy(stage.data(), "SandWorldHomeStage", 18);
    mem.Put(stage_scene_addr + kStageSceneName, stage);
    mem.Put<u64>(gdh_addr + kGameDataHolderToGameData, game_data_addr);
    mem.Put<s32>(game_data_addr + static_cast<u64>(kGameDataCoinNum), coins);
    mem.Put<s32>(game_data_addr + static_cast<u64>(kGameDataWorldId), world_id);
    // The adjacent cached/travel field can still name the previous kingdom.
    mem.Put<s32>(game_data_addr + 0xA3C, world_id == 1 ? 0 : 1);
}

constexpr u64 kTestPoseOffset = 0xC00;

void WritePoseKeeper(FakeMemory& mem, u64 pose, const std::array<float, 3>& position,
                     const std::array<float, 4>& rotation = {0, 0, 0, 1}) {
    mem.MapPage(pose);
    mem.Put<u64>(pose, kMainBase + kQuaternionPoseVtables[1]);
    mem.Put(pose + kPosePosition, position);
    mem.Put(pose + kPoseRotation, rotation);
}

void WritePlayer(FakeMemory& mem, u64 player, u64 scene,
                 const std::array<float, 3>& position,
                 const std::array<float, 4>& rotation = {0, 0, 0, 1}) {
    mem.MapPage(player);
    mem.Put<u64>(player, kMainBase + kPlayerVtableOffset);
    mem.Put<u64>(player + 0xF8, player + 0x800);
    mem.Put<u64>(player + 0x860, scene + 0x900);
    mem.Put<u64>(player + kPlayerActorFlags, player + 0x900);
    mem.Put<u8>(player + 0x900, 0);
    mem.Put<u64>(player + kPlayerPoseKeeper, player + kTestPoseOffset);
    WritePoseKeeper(mem, player + kTestPoseOffset, position, rotation);
    const u64 scene_info=scene+0xA00, holder=scene+0xB00, array=scene+0xC00;
    mem.Put<u64>(scene+kStageSceneActorInitInfo,scene_info);
    mem.Put<u64>(scene_info+kSceneInfoPlayerHolder,holder);
    mem.Put<u64>(holder,array);mem.Put(holder+8,std::array<s32,2>{4,1});
    mem.Put<u64>(array,player);
}

void FindsStageSceneAndReadsCoins() {
    FakeMemory mem;
    const EdenDsmodHostApi host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    WriteChain(mem, ss, gdh, gd, /*coins=*/371, /*world_id=*/1);

    Reader r;
    Snapshot snap;
    RunScanToCompletion(r, host, snap, /*max_samples=*/8);
    assert(r.TestStageScene() == ss);

    // One more sample to walk the now-resolved chain (the sample that found the root does not
    // also publish this tick's values -- RunScanSlice can land mid-page).
    r.Sample(host, snap);
    assert(snap.chain_ready);
    assert(snap.coins_valid);
    assert(snap.coins == 371);
    assert(snap.world_id == 1);
    assert(snap.stage_name == "SandWorldHomeStage");
    assert(snap.stage_scene == ss);
}

void UnavailableBeforeResolved() {
    FakeMemory mem; // empty heap: nothing matches the vtable
    const EdenDsmodHostApi host = MakeHost(mem);
    Reader r;
    Snapshot snap;
    r.Sample(host, snap);
    assert(!snap.chain_ready);
    assert(!snap.coins_valid);
    assert(snap.coins == 0); // cleared, not stale
    assert(r.TestStageScene() == 0);
}

void TornReadRejected() {
    // A CoinNum field that reads differently on the two reads inside one Sample() cannot be
    // constructed through the public API (the fake memory is static per call), so this
    // exercises the sanity-range half of "reject torn reads" instead: a wildly out-of-range
    // value (as a torn 32-bit read straddling two real values could produce) must not publish.
    FakeMemory mem;
    const EdenDsmodHostApi host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    WriteChain(mem, ss, gdh, gd, /*coins=*/-5, /*world_id=*/1);

    Reader r;
    Snapshot snap;
    RunScanToCompletion(r, host, snap, 8);
    assert(r.TestStageScene() == ss);
    r.Sample(host, snap);
    assert(snap.chain_ready); // the chain itself is fine
    assert(!snap.coins_valid); // but a negative coin count is not published
}

void CoinSanityCeilingRejected() {
    FakeMemory mem;
    const EdenDsmodHostApi host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    WriteChain(mem, ss, gdh, gd, /*coins=*/kCoinSanityMax + 1, /*world_id=*/1);
    Reader r;
    Snapshot snap;
    RunScanToCompletion(r, host, snap, 8);
    r.Sample(host, snap);
    assert(snap.chain_ready);
    assert(!snap.coins_valid);
}

void StaleStageSceneIsDropped() {
    FakeMemory mem;
    const EdenDsmodHostApi host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    WriteChain(mem, ss, gdh, gd, 42, 1);
    Reader r;
    Snapshot snap;
    RunScanToCompletion(r, host, snap, 8);
    r.Sample(host, snap);
    assert(snap.coins_valid && snap.coins == 42);

    // The heap block is freed and overwritten (a different vtable, as if the slot were reused
    // for an unrelated object): the next sample must drop the stale root, not keep publishing.
    mem.Put<u64>(ss, kMainBase + 0xDEADBEEF);
    r.Sample(host, snap);
    assert(!snap.chain_ready);
    assert(!snap.coins_valid);
    assert(r.TestStageScene() == 0);
}

void WorldChangeKeepsLiveScene() {
    FakeMemory mem;
    const EdenDsmodHostApi host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    WriteChain(mem, ss, gdh, gd, 10, /*world_id=*/1); // Cascade
    Reader r;
    Snapshot snap;
    RunScanToCompletion(r, host, snap, 8);
    r.Sample(host, snap);
    assert(snap.world_id == 1);
    assert(r.TestStageScene() == ss);

    // A kingdom transition: the live sequence still owns this scene. Keep it, but invalidate
    // the cached player so a new gameplay actor is resolved after the transition.
    mem.Put<s32>(gd + static_cast<u64>(kGameDataWorldId), 2); // Sand
    r.Sample(host, snap);
    assert(snap.chain_ready);
    assert(snap.world_id == 2);
    assert(r.TestStageScene() == ss); // live sequence owns the current scene
}

void RelocationDeltaIsHonoured() {
    FakeMemory mem;
    mem.relocation_delta = 0x1000;
    EdenDsmodHostApi host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    WriteRoot(mem, ss);
    mem.MapPage(ss);
    mem.MapPage(gdh);
    mem.MapPage(gd);
    // The object's vtable slot holds the *relocated* address (main_base + offset + delta), as
    // NCE/ASLR would produce; the un-relocated address (WriteChain's default) would not match.
    mem.Put<u64>(ss, kMainBase + kStageSceneVtableOffset + static_cast<u64>(mem.relocation_delta));
    mem.Put<u64>(ss + kStageSceneToGameDataHolder, gdh);
    mem.Put<u64>(gdh + kGameDataHolderToGameData, gd);
    mem.Put<s32>(gd + static_cast<u64>(kGameDataCoinNum), 7);
    mem.Put<s32>(gd + static_cast<u64>(kGameDataWorldId), 1);

    Reader r;
    Snapshot snap;
    RunScanToCompletion(r, host, snap, 8);
    assert(r.TestStageScene() == ss);
}

void SequenceOwnsCurrentSceneAndPlayer() {
    FakeMemory mem;
    const auto host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000, gd = kHeapBegin + 0x30000;
    constexpr u64 player = kHeapBegin + 0x60000, seq = kHeapBegin + 0x50000;
    WriteChain(mem, ss, gdh, gd, 371, 2);
    WritePlayer(mem, player, ss, {32900, -300, -12700}, {0, 1, 0, 0});
    Reader r;
    Snapshot snap;
    r.Sample(host, snap); // resolve GameSystem and the live scene
    r.Sample(host, snap); // resolve the player only once a scene exists
    assert(snap.coins_valid && snap.coins == 371);
    assert(snap.player_valid && snap.player_position[0] == 32900);
    assert(std::abs(snap.player_heading_deg) < 0.001);
    r.Sample(host, snap);
    assert(snap.scan_pages_this_sample == 0); // steady state performs no heap scans
    mem.Put(player + kTestPoseOffset + kPosePosition, std::array<float, 3>{33100, -300, -12700});
    mem.Put(player + kTestPoseOffset + kPoseRotation, std::array<float, 4>{0, 0.70710678f, 0, 0.70710678f});
    r.Sample(host, snap);
    assert(snap.player_valid && snap.player_position[0] == 33100);
    assert(std::abs(snap.player_heading_deg - 90) < 0.001);
    // Live forward walk moved along -X. A matrix-row interpretation incorrectly gives -Z.
    mem.Put(player + kTestPoseOffset + kPoseRotation,
            std::array<float, 4>{-.0133938f, .70708f, -.00812385f, -.70696f});
    r.Sample(host, snap);
    assert(snap.player_valid && snap.player_front[0] < -.99f);
    assert(std::abs(snap.player_front[2]) < .001f);
    mem.tear_at = player + kTestPoseOffset + kPosePosition;
    mem.tear_reads = 0;
    r.Sample(host, snap);
    assert(!snap.player_valid); // position changed between reads
    mem.tear_at = gd + kGameDataCoinNum;
    mem.tear_reads = 0;
    r.Sample(host, snap);
    assert(!snap.coins_valid); // coin count changed between reads
    mem.tear_at = 0;
    mem.Put(player + kTestPoseOffset + kPoseRotation, std::array<float, 4>{0, 0, 0, 0});
    r.Sample(host, snap);
    assert(!snap.player_valid);
    mem.Put(player + kTestPoseOffset + kPoseRotation, std::array<float, 4>{0, 1, 0, 0});
    mem.Put(player + kTestPoseOffset + kPosePosition,
            std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0, 0});
    r.Sample(host, snap);
    assert(!snap.player_valid);

    // Title/loading clears the live pointer while the old object's vtable still
    // exists.
    mem.Put<u64>(seq + kSequenceToStageScene, 0);
    r.Sample(host, snap);
    assert(!snap.coins_valid && !snap.chain_ready && !snap.player_valid);
    assert(snap.stage_scene == 0 && snap.player == 0);
    r.Sample(host, snap);
    assert(snap.scan_pages_this_sample == 0); // no repeated player scan at title/loading
    // Scene replacement in the SAME kingdom must switch immediately, without
    // finding a vtable.
    constexpr u64 next = kHeapBegin + 0x70000;
    WriteChain(mem, next, gdh, gd, 400, 2);
    r.Sample(host, snap);
    assert(snap.stage_scene == next && snap.coins_valid && snap.coins == 400);
    assert(!snap.player_valid);
    // Old player still has its vtable and a valid pose. Only the new scene's
    // SceneObjHolder distinguishes it from the active actor after kingdom travel.
    mem.Put(player + kTestPoseOffset + kPosePosition, std::array<float,3>{32900,-300,-12700});
    constexpr u64 next_player=kHeapBegin+0x80000;
    WritePlayer(mem, next_player, next, {100, 200, 300});
    // The native holder can remain empty until its current actor is initialized.
    for(int i=0;i<100 && !snap.player_valid;++i)r.Sample(host,snap);
    assert(snap.player_valid && snap.player==next_player && snap.player_position[0]==100);
}

void SameWorldSubstageDropsRetiredPlayers() {
    FakeMemory mem;
    const auto host = MakeHost(mem);
    constexpr u64 scene = kHeapBegin + 0x10000, holder = kHeapBegin + 0x20000;
    constexpr u64 data = kHeapBegin + 0x30000, outside = kHeapBegin + 0x60000;
    constexpr u64 inside = kHeapBegin + 0x70000, returned = kHeapBegin + 0x80000;
    WriteChain(mem, scene, holder, data, 18, 0);
    const auto stage = [&](const char* name) {
        std::array<char, 64> text{};
        std::memcpy(text.data(), name, std::strlen(name));
        mem.Put(scene + kStageSceneName, text);
    };
    stage("CapWorldHomeStage");
    WritePlayer(mem, outside, scene, {100, 200, 300});
    Reader reader; Snapshot snap;
    reader.Sample(host, snap);
    reader.Sample(host, snap);
    assert(snap.player_valid && snap.player == outside);

    // Interior travel can leave every old address/vtable/owner readable. The
    // native dead flag must invalidate both a cached actor and heap candidates.
    mem.Put<u8>(outside + 0x900, 1);
    stage("CapWorldTowerStage");
    WritePlayer(mem, inside, scene, {400, 500, 600});
    reader.Sample(host, snap);
    assert(snap.player_valid && snap.player == inside && snap.world_id == 0);

    mem.Put<u8>(inside + 0x900, 1);
    stage("CapWorldHomeStage");
    WritePlayer(mem, returned, scene, {700, 800, 900});
    reader.Sample(host, snap);
    assert(snap.stage_scene == scene && snap.world_id == 0);
    assert(snap.player_valid && snap.player == returned && snap.player_position[0] == 700);
    mem.Put(returned + kTestPoseOffset + kPosePosition, std::array<float, 3>{710, 800, 900});
    reader.Sample(host, snap);
    assert(snap.player_valid && snap.player_position[0] == 710 && snap.scan_pages_this_sample == 0);

    // A liveness transition between ownership reads is unavailable, never the
    // previous actor's frozen position. Once live again, bounded scan recovers.
    mem.tear_at = returned + 0x900;
    mem.tear_reads = 0;
    reader.Sample(host, snap);
    assert(!snap.player_valid && snap.player == 0);
    mem.tear_at = 0;
    mem.Put<u8>(returned + 0x900, 0);
    for (int i = 0; i < 100 && !snap.player_valid; ++i) reader.Sample(host, snap);
    assert(snap.player_valid && snap.player == returned && snap.player_position[0] == 710);
}

void CurrentPlayerHolderOverridesAliveRetiredActors() {
    FakeMemory mem;const auto host=MakeHost(mem);
    constexpr u64 scene=kHeapBegin+0x10000,holder=kHeapBegin+0x20000,data=kHeapBegin+0x30000;
    constexpr u64 retired=kHeapBegin+0x60000,active=kHeapBegin+0x80000;
    constexpr u64 scene_info=scene+0xA00,players=scene+0xB00,array=scene+0xC00;
    WriteChain(mem,scene,holder,data,18,0);
    WritePlayer(mem,retired,scene,{-1728,7250,-3665});
    Reader reader;Snapshot snap;reader.Sample(host,snap);reader.Sample(host,snap);
    assert(snap.player_valid && snap.player==retired);
    // Both allocations retain valid native identity, owner, alive flag and pose.
    // Only the current scene's holder distinguishes the returned outside Mario.
    WritePlayer(mem,active,scene,{5214,5115,-3542});
    reader.Sample(host,snap);
    assert(snap.player_valid && snap.player==active && snap.player_position[0]==5214);
    assert(snap.scan_pages_this_sample==0);
    mem.Put<u64>(array,retired);reader.Sample(host,snap);
    assert(snap.player_valid && snap.player==retired); // authoritative slot, not address order
    mem.Put<u64>(array,active);
    const auto reject_torn=[&](u64 address,u64 original){
        mem.tear_at=address;mem.tear_reads=0;reader.Sample(host,snap);
        assert(!snap.player_valid && snap.player==0 && snap.scan_pages_this_sample==0);
        mem.tear_at=0;mem.Put<u64>(address,original);reader.Sample(host,snap);
        assert(snap.player_valid && snap.player==active);
    };
    reject_torn(scene+kStageSceneActorInitInfo,scene_info);
    reject_torn(scene_info+kSceneInfoPlayerHolder,players);
    reject_torn(players,array);
    reject_torn(array,active);
    mem.Put(players+8,std::array<s32,2>{4,0});reader.Sample(host,snap);
    assert(!snap.player_valid && snap.scan_pages_this_sample==0);
    mem.Put(players+8,std::array<s32,2>{4,1});
    mem.Put<u64>(scene_info+kSceneInfoPlayerHolder,0);reader.Sample(host,snap);
    assert(!snap.player_valid && snap.scan_pages_this_sample==0); // no heap fallback
    mem.Put<u64>(scene_info+kSceneInfoPlayerHolder,players);reader.Sample(host,snap);
    assert(snap.player_valid && snap.player==active);
}

void PlayerPoseKeeperCanMoveWithoutMovingTheActor() {
    FakeMemory mem; const auto host = MakeHost(mem);
    constexpr u64 scene=kHeapBegin+0x10000, holder=kHeapBegin+0x20000;
    constexpr u64 data=kHeapBegin+0x30000, player=kHeapBegin+0x60000;
    constexpr u64 moved_pose=kHeapBegin+0x90000;
    WriteChain(mem,scene,holder,data,18,0);
    WritePlayer(mem,player,scene,{10,20,30});
    // Preserve the old empirical heap-adjacent pose as a valid decoy.
    mem.Put(player+0x6A8,std::array<float,3>{10,20,30});
    mem.Put(player+0x6B4,std::array<float,4>{0,0,0,1});
    Reader reader; Snapshot snap; reader.Sample(host,snap); reader.Sample(host,snap);
    assert(snap.player_valid && snap.player_position[0]==10);
    assert(snap.player_pose_keeper==player+kTestPoseOffset);
    WritePoseKeeper(mem,moved_pose,{900,800,700});
    mem.Put<u64>(player+kPlayerPoseKeeper,moved_pose);
    reader.Sample(host,snap);
    assert(snap.player_valid && snap.player==player && snap.player_position[0]==900);
    assert(snap.player_pose_keeper==moved_pose && snap.scan_pages_this_sample==0);
    mem.Put(moved_pose+kPosePosition,std::array<float,3>{910,800,700});
    reader.Sample(host,snap); assert(snap.player_position[0]==910);
    // Only native quaternion-bearing keeper layouts share this verified layout.
    for (u64 vt:kQuaternionPoseVtables) {
        mem.Put<u64>(moved_pose,kMainBase+vt);reader.Sample(host,snap);
        assert(snap.player_valid && snap.player_position[0]==910);
    }
    mem.Put<u64>(moved_pose,kMainBase+0x12EF950); // TRSV uses Euler rotation
    reader.Sample(host,snap);assert(!snap.player_valid && snap.player_pose_keeper==0);
    mem.Put<u64>(moved_pose,kMainBase+kQuaternionPoseVtables[1]);
    mem.tear_at=player+kPlayerPoseKeeper;mem.tear_reads=0;
    reader.Sample(host,snap);assert(!snap.player_valid);
    mem.tear_at=0;mem.Put<u64>(player+kPlayerPoseKeeper,moved_pose);
    reader.Sample(host,snap);assert(snap.player_valid && snap.player_position[0]==910);
}

void CameraDirectionIsIndependentOfMario() {
    FakeMemory mem;const auto host=MakeHost(mem);
    constexpr u64 scene=kHeapBegin+0x10000,holder=kHeapBegin+0x20000,data=kHeapBegin+0x30000;
    constexpr u64 player=kHeapBegin+0x60000,scene_info=scene+0xA00;
    constexpr u64 director=kHeapBegin+0x90000;
    constexpr u64 camera_holder=kHeapBegin+0xA0000,array=kHeapBegin+0xB0000;
    constexpr u64 camera=kHeapBegin+0xC0000,pose=kHeapBegin+0xD0000;
    WriteChain(mem,scene,holder,data,18,0);WritePlayer(mem,player,scene,{10,20,30});
    for(u64 at:{scene_info,director,camera_holder,array,camera,pose})mem.MapPage(at);
    mem.Put<u64>(scene+kStageSceneActorInitInfo,scene_info);
    mem.Put<u64>(scene_info+kSceneInfoCameraDirector,director);
    mem.Put<u64>(director,kMainBase+kCameraDirectorVtableOffset);
    mem.Put<u64>(director+0x10,camera_holder);mem.Put<u64>(camera_holder+8,array);
    mem.Put<u64>(array,camera);mem.Put<u64>(camera+8,pose);
    mem.Put<u64>(pose,kMainBase+kCameraPoseVtableOffset);
    mem.Put(pose+kCameraPosition,std::array<float,6>{0,100,200,0,100,0});
    Reader reader;Snapshot snap;reader.Sample(host,snap);reader.Sample(host,snap);
    assert(snap.camera_valid && snap.player_valid);
    assert(snap.camera_front[2]==-1 && snap.player_front[2]==1);
    mem.Put(pose+kCameraPosition,std::array<float,6>{0,100,200,200,100,200});
    reader.Sample(host,snap);
    assert(snap.camera_valid && snap.camera_front[0]==1 && snap.player_front[2]==1);
    mem.tear_at=pose+kCameraPosition;mem.tear_reads=0;
    reader.Sample(host,snap);assert(!snap.camera_valid && snap.player_valid);
    mem.tear_at=0;
    mem.Put(pose+kCameraPosition,std::array<float,6>{0,100,200,200,100,200});
    mem.Put<u64>(director,kMainBase+0x1234);
    reader.Sample(host,snap);assert(!snap.camera_valid && snap.player_valid);
    mem.Put<u64>(director,kMainBase+kCameraDirectorVtableOffset);
    mem.Put<u64>(pose,kMainBase+0x1234);
    reader.Sample(host,snap);assert(!snap.camera_valid);
    mem.Put<u64>(pose,kMainBase+kCameraPoseVtableOffset);
    mem.Put(pose+kCameraPosition,std::array<float,6>{0,100,200,0,100,200});
    reader.Sample(host,snap);assert(!snap.camera_valid); // zero view vector
    mem.Put(pose+kCameraPosition,std::array<float,6>{0,100,200,200,100,200});
    reader.Sample(host,snap);assert(snap.camera_valid && snap.camera_front[0]==1);
    mem.Put<u64>(camera_holder+8,0);
    reader.Sample(host,snap);assert(!snap.camera_valid && snap.player_valid);
}

void HealthAndNativeNerveState() {
    FakeMemory mem;
    const auto host = MakeHost(mem);
    constexpr u64 ss = kHeapBegin + 0x10000, gdh = kHeapBegin + 0x20000;
    constexpr u64 gd = kHeapBegin + 0x30000, hp = kHeapBegin + 0x80000;
    constexpr u64 keeper = kHeapBegin + 0x81000, state = kHeapBegin + 0x82000;
    WriteChain(mem, ss, gdh, gd, 371, 2);
    mem.MapPage(hp); mem.MapPage(keeper); mem.MapPage(state);
    mem.Put<u64>(gd + kGameDataToHitPointData, hp);
    mem.Put<u64>(hp, kMainBase + kHitPointVtableOffset);
    mem.Put<s32>(hp + 0xC, 2);
    mem.Put<u64>(ss + kStageSceneToNerveKeeper, keeper);
    mem.Put<u64>(keeper + 8, kMainBase + kStageScenePlayNerve);
    mem.Put<u64>(ss + kStageSceneActiveState, state);
    mem.Put<u64>(state, kMainBase + kStageMapVtableOffset); // deliberately stale at Play
    Reader reader; Snapshot snap;
    RunScanToCompletion(reader, host, snap, 8);
    reader.Sample(host, snap);
    assert(snap.health_valid && snap.health == 2 && snap.health_max == 3);
    assert(snap.hit_point_data == hp && snap.game_data == gd);
    assert(snap.scene_mode_valid && snap.scene_mode == SceneMode::Playing);
    mem.Put<u8>(hp + 8, 1); mem.Put<s32>(hp + 0xC, 5);
    reader.Sample(host, snap);
    assert(snap.health_valid && snap.health == 5 && snap.health_max == 6);
    mem.Put<u8>(hp + 0x10, 1); mem.Put<s32>(hp + 0xC, 9);
    reader.Sample(host, snap);
    assert(snap.health_valid && snap.health_max == 9);
    mem.Put<u8>(hp + 0x11, 1); mem.Put<s32>(hp + 0xC, 3);
    reader.Sample(host, snap);
    assert(snap.health_valid && snap.health_max == 3);
    mem.Put<s32>(hp + 0xC, 4); reader.Sample(host, snap);
    assert(!snap.health_valid); // impossible forced-three state
    mem.Put<s32>(hp + 0xC, 3); mem.Put<u8>(hp + 8, 2);
    reader.Sample(host, snap); assert(!snap.health_valid);
    mem.Put<u8>(hp + 8, 0);
    mem.tear_at = hp + 8; mem.tear_reads = 0;
    reader.Sample(host, snap); assert(!snap.health_valid);
    mem.tear_at = 0; mem.Put<u8>(hp + 8, 0);
    mem.Put<u64>(keeper + 0x10, kMainBase + kStageScenePauseNerve);
    reader.Sample(host, snap);
    assert(snap.scene_mode_valid && snap.scene_mode == SceneMode::Paused);
    mem.Put<u64>(state, kMainBase + kPauseMenuVtableOffset);
    reader.Sample(host, snap); assert(snap.scene_mode == SceneMode::Paused);
    // Closing the menu can leave +180 stale; effective Play wins immediately.
    mem.Put<u64>(keeper + 8, kMainBase + kStageScenePauseNerve);
    mem.Put<u64>(keeper + 0x10, kMainBase + kStageScenePlayNerve);
    reader.Sample(host, snap); assert(snap.scene_mode == SceneMode::Playing);
    mem.tear_at = keeper + 8; mem.tear_reads = 0;
    reader.Sample(host, snap); assert(!snap.scene_mode_valid);
    mem.tear_at = 0;
    mem.Put<u64>(keeper + 8, kMainBase + kStageScenePlayNerve);
    mem.Put<u64>(keeper + 0x10, 0);
    mem.Put<u64>(hp, kMainBase + 0x1234);
    reader.Sample(host, snap); assert(!snap.health_valid);
    mem.Put<u64>(kHeapBegin + 0x50000 + kSequenceToStageScene, 0);
    reader.Sample(host, snap);
    assert(!snap.health_valid && !snap.scene_mode_valid && !snap.game_data);
}

void TitleAndLoadingUseNativeIdentities() {
    FakeMemory mem;
    const auto host = MakeHost(mem);
    constexpr u64 scene = kHeapBegin + 0x10000, sequence = kHeapBegin + 0x50000;
    constexpr u64 keeper = kHeapBegin + 0x81000;
    WriteRoot(mem, scene); mem.MapPage(scene); mem.MapPage(keeper);
    mem.Put<u64>(scene, kMainBase + kTitleMenuSceneVtableOffset);
    Reader reader; Snapshot snap;
    reader.Sample(host, snap);
    assert(snap.scene_mode_valid && snap.scene_mode == SceneMode::Title && !snap.chain_ready);
    mem.Put<u64>(sequence, kMainBase + kHakoniwaSequenceVtableOffset);
    mem.Put<u64>(sequence + 8, keeper);
    mem.Put<u64>(sequence + kSequenceToStageScene, 0);
    mem.Put<u64>(keeper + 8, kMainBase + 0x12D04B0);
    reader.Sample(host, snap);
    assert(snap.scene_mode_valid && snap.scene_mode == SceneMode::Loading && !snap.chain_ready);
    mem.Put<u64>(keeper + 8, kMainBase + 0x12D0408); // PlayStage without scene is unknown
    reader.Sample(host, snap);
    assert(!snap.scene_mode_valid && snap.scene_mode == SceneMode::Unknown);
}

void CollectionListOwnsNativeStageMap() {
    FakeMemory mem; const auto host = MakeHost(mem);
    constexpr u64 ss=kHeapBegin+0x10000, gdh=kHeapBegin+0x20000, gd=kHeapBegin+0x30000;
    constexpr u64 keeper=kHeapBegin+0x81000, map_keeper=kHeapBegin+0x82000;
    constexpr u64 collection=kHeapBegin+0x83000, map=kHeapBegin+0x84000, pause=kHeapBegin+0x85000;
    WriteChain(mem,ss,gdh,gd,371,2);
    for (u64 addr : {keeper,map_keeper,collection,map,pause}) mem.MapPage(addr);
    mem.Put<u64>(ss+8,keeper);
    mem.Put<u64>(keeper+8,kMainBase+kStageSceneCollectionListNerve);
    mem.Put<u64>(ss+kStageSceneActiveState,pause);
    mem.Put<u64>(pause,kMainBase+kPauseMenuVtableOffset); // stale prior Plus menu
    mem.Put<u64>(ss+kStageSceneToCollectionList,collection);
    mem.Put<u64>(collection,kMainBase+kCollectionListVtableOffset);
    mem.Put<u64>(collection+0x38,map);
    mem.Put<u64>(map,kMainBase+kStageMapVtableOffset);
    mem.Put<u64>(map+0x18,ss); mem.Put<u64>(map+8,map_keeper);
    mem.Put<u64>(map_keeper+8,kMainBase+0x12CBB08); // native Map Wait
    Reader reader; Snapshot snap;
    RunScanToCompletion(reader,host,snap,8); reader.Sample(host,snap);
    assert(snap.scene_mode==SceneMode::StageMap && snap.map_state==map);
    assert(snap.map_nerve==kMainBase+0x12CBB08);
    mem.Put<u8>(map+0x10,1); reader.Sample(host,snap);
    assert(snap.scene_mode==SceneMode::Other && snap.map_state==0); // map ended
    mem.Put<u8>(map+0x10,0); mem.Put<u64>(map+0x18,ss+0x1000); reader.Sample(host,snap);
    assert(snap.scene_mode==SceneMode::Other); // stale map from another scene
    mem.Put<u64>(map+0x18,ss); mem.Put<u64>(map_keeper+8,kMainBase+kStageScenePlayNerve);
    reader.Sample(host,snap); assert(snap.scene_mode==SceneMode::Other); // not a native map nerve
    mem.Put<u64>(map_keeper+8,kMainBase+0x12CBB08);
    mem.Put<u64>(keeper+8,kMainBase+kStageScenePlayNerve);
    reader.Sample(host,snap); assert(snap.scene_mode==SceneMode::Playing && snap.map_state==0);
    mem.Put<u64>(keeper+8,kMainBase+kStageSceneCollectionListNerve);
    mem.Put<u64>(map,kMainBase+kPauseMenuVtableOffset);
    reader.Sample(host,snap); assert(snap.scene_mode==SceneMode::Other); // another collection view
}

void WorldMapGlobeOpenAndCursor() {
    FakeMemory mem; const auto host = MakeHost(mem);
    constexpr u64 ss=kHeapBegin+0x10000, gdh=kHeapBegin+0x20000, gd=kHeapBegin+0x30000;
    constexpr u64 world_map=kHeapBegin+0x86000, keeper=kHeapBegin+0x87000, table=kHeapBegin+0x88000;
    WriteChain(mem,ss,gdh,gd,371,2);
    for (u64 addr : {world_map,keeper,table}) mem.MapPage(addr);
    mem.Put<u64>(ss+kStageSceneToWorldMap,world_map);
    mem.Put<u64>(world_map,kMainBase+kWorldMapVtableOffset);
    mem.Put<u8>(world_map+kStateToEnded,1); // closed baseline
    Reader reader; Snapshot snap;
    RunScanToCompletion(reader,host,snap,8); reader.Sample(host,snap);
    assert(!snap.globe_open && !snap.globe_cursor_valid);

    // Open the globe; the cursor table holds [14,0,1,2,4,3,...] (RE batch 9's captured lookup),
    // cursor position 0 == the player's own (current) kingdom, here Mushroom (WorldId 14).
    mem.Put<u64>(world_map+kStateToOwnNerveKeeper,keeper);
    mem.Put<u64>(world_map+kWorldMapCursorTable,table);
    const u32 lookup[] = {14,0,1,2,4,3,5,6,7,8,9,10,11,12,13,15,16};
    for (size_t i=0;i<17;++i) mem.Put<u32>(table+i*4,lookup[i]);
    mem.Put<u32>(world_map+kWorldMapCursorPosition,0);
    mem.Put<u8>(world_map+kStateToEnded,0);
    reader.Sample(host,snap);
    assert(snap.globe_open && snap.globe_cursor_valid && snap.globe_cursor_world_id==14);

    // Cursor moves to position 7 (Lost Kingdom, WorldId 6) -- a non-adjacent jump, matching the
    // RE batch 9 live data point.
    mem.Put<u32>(world_map+kWorldMapCursorPosition,7);
    reader.Sample(host,snap);
    assert(snap.globe_open && snap.globe_cursor_valid && snap.globe_cursor_world_id==6);

    // An out-of-range table slot must never be guessed at.
    mem.Put<u32>(world_map+kWorldMapCursorPosition,99);
    reader.Sample(host,snap);
    assert(snap.globe_open && !snap.globe_cursor_valid && snap.globe_cursor_world_id==-1);

    // Closing the globe clears both fields, even though the pool slot pointer stays resolvable.
    mem.Put<u32>(world_map+kWorldMapCursorPosition,0);
    mem.Put<u8>(world_map+kStateToEnded,1);
    reader.Sample(host,snap);
    assert(!snap.globe_open && !snap.globe_cursor_valid);

    // A stale/mismatched vtable at the pool slot (object freed and reused) must fail closed, not
    // keep reporting the last-known state.
    mem.Put<u8>(world_map+kStateToEnded,0);
    reader.Sample(host,snap);
    assert(snap.globe_open);
    mem.Put<u64>(world_map,kMainBase+0xDEADBEEF);
    reader.Sample(host,snap);
    assert(!snap.globe_open && !snap.globe_cursor_valid);
}

void CollectionPageNerveSelectsTheOpenList() {
    FakeMemory mem; const auto host = MakeHost(mem);
    constexpr u64 ss=kHeapBegin+0x10000, gdh=kHeapBegin+0x20000, gd=kHeapBegin+0x30000;
    constexpr u64 collection=kHeapBegin+0x89000, keeper=kHeapBegin+0x8A000;
    WriteChain(mem,ss,gdh,gd,371,2);
    for (u64 addr : {collection,keeper}) mem.MapPage(addr);
    mem.Put<u64>(ss+kStageSceneToCollectionList,collection);
    mem.Put<u64>(collection,kMainBase+kCollectionListVtableOffset);
    mem.Put<u8>(collection+kStateToEnded,1); // closed baseline
    Reader reader; Snapshot snap;
    RunScanToCompletion(reader,host,snap,8); reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::None);

    mem.Put<u64>(collection+kStateToOwnNerveKeeper,keeper);
    mem.Put<u8>(collection+kStateToEnded,0);
    mem.Put<u64>(keeper+8,kMainBase+kCollectionNerveMap);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::Map);

    mem.Put<u64>(keeper+8,kMainBase+kCollectionNervePowerMoon);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::PowerMoon);

    mem.Put<u64>(keeper+8,kMainBase+kCollectionNerveCapture);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::Capture);

    mem.Put<u64>(keeper+8,kMainBase+kCollectionNerveSouvenir);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::Souvenir);

    mem.Put<u64>(keeper+8,kMainBase+kCollectionNerveMusic);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::Music);

    // An unrecognised nerve value (e.g. a one-frame transition) is never guessed at.
    mem.Put<u64>(keeper+8,kMainBase+0x12CF000);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::None);

    // Closing the CollectionList clears the page even though the pool object is still resolvable.
    mem.Put<u64>(keeper+8,kMainBase+kCollectionNerveMusic);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::Music);
    mem.Put<u8>(collection+kStateToEnded,1);
    reader.Sample(host,snap);
    assert(snap.collection_page==CollectionPage::None);
}

} // namespace
} // namespace SmoReader

int main() {
    SmoReader::CollectionPageNerveSelectsTheOpenList();
    SmoReader::WorldMapGlobeOpenAndCursor();
    SmoReader::CollectionListOwnsNativeStageMap();
    SmoReader::TitleAndLoadingUseNativeIdentities();
    SmoReader::HealthAndNativeNerveState();
    SmoReader::SequenceOwnsCurrentSceneAndPlayer();
    SmoReader::SameWorldSubstageDropsRetiredPlayers();
    SmoReader::CurrentPlayerHolderOverridesAliveRetiredActors();
    SmoReader::PlayerPoseKeeperCanMoveWithoutMovingTheActor();
    SmoReader::CameraDirectionIsIndependentOfMario();
    SmoReader::BuildGating();
    SmoReader::FindsStageSceneAndReadsCoins();
    SmoReader::UnavailableBeforeResolved();
    SmoReader::TornReadRejected();
    SmoReader::CoinSanityCeilingRejected();
    SmoReader::StaleStageSceneIsDropped();
    SmoReader::WorldChangeKeepsLiveScene();
    SmoReader::RelocationDeltaIsHonoured();
    return 0;
}
