// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone Metroid Dread reader. The original Dread algorithms below are unchanged; this
// translation unit supplies only ABI-backed memory, logging, asset, and output facades.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "dread_mapgen.h"

namespace {

using namespace dsmod_sdk::int_types;
using Json = nlohmann::json;
using f32 = float;
using f64 = double;
using VAddr = u64;

constexpr u64 TitleId = UINT64_C(0x010093801237C000);

u64 DreadNameHash(std::string_view name) {
    // Reflected CRC-64/ECMA, init all ones, no output xor. This is Dread's CStrId/item hash.
    u64 crc = UINT64_MAX;
    for (const u8 byte : name) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT64_C(0xC96C5795D7870F42) : 0);
        }
    }
    return crc;
}

// Title-local types holding only what this reader uses. They are separate from Core::Mods' types
// of the same names (mod_types_map.h: MapMarker, MapLayer, MapArea, VisitedGrid; mod_types.h:
// Manifest, StateSnapshot), which this module never includes.
namespace dread {

struct MapMarker {
    std::string kind;
    std::string icon;
    float x{}, y{};
    std::string name;
    std::string vignette;
    bool hidden{}, opened{}, collected{}, unveiled{}, veiled{};
    bool has_box{};
    float bx0{}, by0{}, bx1{}, by1{};
    bool has_pulse_box{};
    float px0{}, py0{}, px1{}, py1{};
    bool has_hint_box{};
    float hx0{}, hy0{}, hx1{}, hy1{};
};

struct MapLayer {
    std::string geo;
    u32 color{};
};

/// One water pool of an area, baked from the romfs (dread_mapgen.cpp): the actor
/// name, its full-level box (the bmmap mapWaterPoolGeos quad) and its vWaterLevelChanges.
struct WaterPoolDef {
    std::string name;
    std::array<float, 4> box{};
    std::vector<float> levels;
    /// The box at level fraction `level` (the surface sits at minY + level * height).
    std::array<float, 4> BoxAt(float level) const;
    /// The level ChangeIdx `change_idx` selects; nullopt for a pool with no level changes.
    std::optional<float> LevelAt(s64 change_idx) const;
};

struct MapArea {
    std::string geo;
    std::string image;
    bool no_pin{};
    float min_x{}, min_y{}, max_x{}, max_y{};
    std::vector<MapMarker> markers;
    std::vector<MapLayer> layers;
    struct MapOccluder {
        std::string name;
        std::string owner;
        std::string collider;
        std::vector<std::array<float, 6>> tris;
    };
    std::vector<MapOccluder> occluders;
    std::vector<MapOccluder> vignettes;
    std::vector<std::array<float, 4>> camera_rects;
    std::vector<WaterPoolDef> water_pools;
};

struct Manifest {
    std::map<std::string, MapArea, std::less<>> map_areas;
    std::string build_id_file;
    struct {
        bool reveal_required{false};
    } map_style;
};

struct VisitedGrid {
    static constexpr int Cols = 650;
    static constexpr int Rows = 300;
    static constexpr u8 Unexplored = 0;
    static constexpr u8 Revealed = 1;
    static constexpr u8 Visited = 2;
    std::vector<u8> cells = std::vector<u8>(static_cast<size_t>(Cols) * Rows, 0);
    std::vector<u32> change_tick = std::vector<u32>(static_cast<size_t>(Cols) * Rows, 0);
    std::vector<u8> prev = std::vector<u8>(static_cast<size_t>(Cols) * Rows, 0);
    u64 generation{};
    u64 last_reveal_tick{};
};

struct StateSnapshot {
    std::unordered_map<std::string, s64> ints;
    std::unordered_map<std::string, std::string> texts;
    std::unordered_map<std::string, f64> floats;
    struct CustomMarker {
        float x{}, y{};
        s32 color{};
    };
    std::vector<CustomMarker> custom_markers;
};

/// Publishes one vital (energy, missiles, ...) under `name` as both a float and an int. Page
/// widgets read ints (StateSnapshot::GetInt in the host), and the host's own scan point of the
/// same name has already published an int this tick, so a float alone never reaches a widget.
/// The int truncates toward zero, as the game's HUD label does (fcvtzs, main+0xB7AC54).
inline void PublishVital(StateSnapshot& snapshot, const std::string& name, f32 value) {
    snapshot.floats[name] = value;
    snapshot.ints[name] = static_cast<s64>(value);
}

} // namespace dread

class Reader;

/// The asset-free package's map data, rebuilt from the player's romfs on the module's own worker
/// thread (dread_mapgen.cpp): started by Create, waited on by load_data (the runtime's "module:"
/// reads, on the runtime's threads), taken over by the tick thread once done (Reader::Tick).
struct MapJob {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable cv;
    // Everything below is written by the worker before `done` is set under `mutex`, then only
    // read (the tick thread moves `areas` and `localized` out, which load_data never touches).
    bool done{false};
    bool ok{false};
    std::string error;
    std::string areas_json;                             // "module:dread:areas"
    std::map<std::string, std::vector<u8>> blobs;       // "module:dread:<key>"
    std::map<std::string, dread::MapArea, std::less<>> areas; // the module's own parse
    std::unordered_map<std::string, std::string> localized;
    bool localized_ok{false};

    /// Blocks until the job has finished. False when it failed.
    bool Wait() {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return done; });
        return ok;
    }
    bool Done() {
        std::scoped_lock lock{mutex};
        return done;
    }
};

class MemoryFacade {
public:
    explicit MemoryFacade(Reader* owner_) : owner{owner_} {}
    u8 Read8(VAddr address) const;
    u32 Read32(VAddr address) const;
    u64 Read64(VAddr address) const;
    const u8* GetPointerSilent(VAddr address) const;

private:
    Reader* owner;
};

class SystemFacade {
public:
    explicit SystemFacade(Reader* owner) : memory{owner} {}
    MemoryFacade& ApplicationMemory() const {
        return memory;
    }

private:
    mutable MemoryFacade memory;
};

class Reader {
public:
    explicit Reader(const EdenDsmodHostApi& api) : host{api}, system{this} {}
    ~Reader() {
        // The map job's thread must not outlive the Reader (whatever path destroys it).
        if (map_job && map_job->thread.joinable()) {
            map_job->stop = true;
            map_job->thread.join();
        }
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Without the host's is_mapped nothing is sane (dsmod_sdk::MissingIsMapped::Reject).
    bool AddressIsSane(VAddr address, u64 size) const {
        return dsmod_sdk::RangeMapped(host, address, size);
    }
    VAddr HeapLow() const {
        return host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
    }
    VAddr HeapHigh() const {
        return host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
    }
    std::vector<u8> ReadAssetBytes(const std::string& source) const;
    template <typename... Args>
    void Log(u32 level, fmt::format_string<Args...> format, Args&&... args) const {
        if (host.log) {
            const std::string message = fmt::format(format, std::forward<Args>(args)...);
            host.log(host.userdata, level, message.c_str());
        }
    }

    VAddr RootAt(s64 delta) const;
    VAddr Relocated(u64 offset) const;
    bool CutscenePlaying(std::string* name) const;
    VAddr BlackboardRoot() const;
    std::string ReadCStrIdName(VAddr keyptr, int max) const;
    /// Appends the bytes at `p` up to the first NUL, `max` bytes, or the first unmapped byte --
    /// exactly what a byte-at-a-time AddressIsSane + Read8 walk returns, but with one host call
    /// per guest page instead of three or four per character. Returns false as soon as `reject`
    /// flags a byte (that byte is not appended).
    template <typename Reject>
    bool ReadGuestAsciiZ(VAddr p, int max, std::string& out, Reject reject) const;
    bool ForEachBlackboardProp(const std::string& section,
                               const std::function<bool(const std::string&, VAddr)>& fn) const;
    std::string ReadScenarioName(VAddr grid) const;
    std::optional<VAddr> FindMinimapCells() const;
    bool CellsLookValid(VAddr cb) const;
    struct WaterRecord {
        u64 key{}; ///< the actor's CStrId
        u64 id{};  ///< CStrId+0x30: the name's CRC-64, the id DEBA8C matches
        std::string name;
        std::array<float, 4> box{};
    };
    struct WaterMoving {
        std::array<float, 4> record{};
        s64 idx{};
        VAddr comp{};
        bool unresolved{}, live{};
    };
    const dread::WaterPoolDef* WaterPool(const std::string& name) const;
    bool WaterSectionValid() const;
    bool CacheWaterSection() const;
    bool ReadWaterVolumes(std::vector<WaterRecord>& out) const;
    VAddr ResolveActor(u64 key) const;
    VAddr ResolveWaterComponent(u64 key) const;
    bool ReadLiveWaterBox(VAddr comp, std::array<float, 4>& box, bool& moving) const;
    void PublishWaterBoxes(std::vector<std::array<float, 4>>& boxes) const;
    void ResetWater() const;
    void RefreshWaterBoxes() const;
    bool ReadGameVisibility(const std::string& scenario_hint) const;
    VAddr FindBlackboardProp(const std::string& section, const std::string& prop) const;
    std::optional<bool> EmmyDefeatedInScenario(const std::string& scenario) const;
    void ReadCustomMarkers(const std::string& scenario,
                           std::vector<dread::StateSnapshot::CustomMarker>& out) const;
    void ReadMissionLog(std::vector<std::string>& ids) const;
    const std::string& LocalizedText(const std::string& key);
    struct ZoneFlags {
        bool closed{}, dead{};
        f32 maproom_factor{};
    };
    std::optional<ZoneFlags> ReadMinimapZoneFlags() const;
    bool ReadEmmiCenter(VAddr actor) const;
    bool AreaHasEmmi(const std::string& area) const;
    struct EmmiInfo {
        s32 state{-1};
        bool enabled{}, inside{}, owner_is_entity{true};
        u64 vt_off{};
    };
    bool EmmiEntityState(VAddr actor, EmmiInfo& info) const;
    void RefreshEmmiPos() const;
    void ReadBreakableTiles(const std::string& area) const;
    void ReadOccluderDeaths(const std::string& area);
    void ApplyOccluderColliderState(
        const std::string& area, bool state_valid,
        const std::map<std::string, std::set<std::string>, std::less<>>& enabled,
        std::set<std::string>& dead) const;
    void ReadVignettes(const std::string& area);
    void UpdateHiddenMarkers(const std::string& area);
    bool IsVisited(const std::string& area, float wx, float wy) const;

    void UpdateHost(const EdenDsmodHostApi& api);
    bool SetHudHidden(bool hidden) const;
    bool SetMinimapHidden(bool hidden) const;
    bool HandleAction(const char* action, s64 argument);
    void Sample();
    void Tick();
    void TickDread(dread::StateSnapshot& snapshot);
    void SampleTracker(dread::StateSnapshot& snapshot) const;
    void PublishSnapshot(const dread::StateSnapshot& snapshot);
    void PublishMap(const dread::StateSnapshot& snapshot);
    void PublishMapState();

    struct WallTile {
        float x{}, y{};
        u8 type{};
        bool operator==(const WallTile&) const = default;
    };

    EdenDsmodHostApi host{};
    SystemFacade system;
    dread::Manifest manifest;
    u64 tick_count{};
    VAddr main_region_begin{};
    u64 main_region_size{};
    s64 nce_vtable_delta{};
    mutable std::map<std::string, dread::VisitedGrid> map_visited;
    mutable std::map<std::string, std::vector<WallTile>> map_walls;
    mutable std::map<std::string, std::set<std::string>> map_occ_dead;
    mutable std::map<std::string, std::set<std::string>> map_vig_dispelled;
    mutable std::map<std::string, std::set<std::string>> map_door_open;
    mutable std::map<std::string, std::set<std::string>> map_item_picked;
    mutable std::map<std::string, std::set<std::string>> map_item_unveiled;
    mutable std::map<std::string, std::set<std::string>> map_item_veiled;
    mutable std::set<std::string> game_vis_areas;
    mutable std::vector<u8> vis_scratch;
    mutable VAddr root_cache{};
    mutable u64 root_cache_tick{~u64{0}};
    mutable std::string live_scenario;
    mutable std::string scenario_route;
    std::string readers_scenario;
    bool emmy_defeated{}, emmy_sealed{};
    mutable bool map_unlocked{};
    // Raw "doors sealed" state from the live minimap manager (mgr+0xD63D69), tracked
    // independently of emmy_defeated/emmy_sealed so a chase caught mid-read (this tick's zf, or
    // the last one we actually saw) can veto a wrong "dead" verdict regardless of which path
    // produced it -- see the invariant at the EmmyDefeatedInScenario call site.
    mutable bool zone_doors_closed{};
    mutable VAddr emmi_actor{};
    mutable std::string emmi_units_seen;
    mutable std::string emmi_units_scenario;
    // Diagnostics: last logged CU name/state dump and save-record reading per scenario,
    // so EmmyDefeatedInScenario logs on change only (it can be polled at ~1 Hz while unresolved).
    mutable std::string emmy_cu_seen;
    mutable std::string emmy_cu_scenario;
    s64 items_pct_cached{}, items_total_cached{}, energy_parts_cached{}, energy_frag_state_cached{};
    bool energy_frag_logged{};
    // PLAYER_INVENTORY blackboard readout for the vital stats (energy/missile/aeion/
    // power bomb, current+max) -- see the write site for why this replaces the heap vtable-scan
    // array as the value these snapshot keys publish. "resolved" gates whether the cached values
    // are trusted (false until the very first successful walk finds all four current/max pairs).
    mutable bool vitals_resolved{};
    mutable u64 vitals_tick{};
    mutable f32 vitals_energy{}, vitals_energy_max{}, vitals_missile{}, vitals_missile_max{},
        vitals_pbomb{}, vitals_pbomb_max{}, vitals_aeion{}, vitals_aeion_max{};
    std::vector<dread::StateSnapshot::CustomMarker> custom_markers_cached;
    u64 custom_markers_tick{};
    std::string custom_markers_scenario;
    std::unordered_map<std::string, std::string> localized;
    bool localized_loaded{};
    std::vector<std::string> mlog_cached;
    u64 mlog_tick{};
    std::unordered_map<std::string, s64> powers_cached;
    s64 menu_open_cached{};
    mutable bool vis_grid_missing{};
    s64 last_in_game{-1};
    u64 items_pct_tick{};
    std::string items_pct_scenario;
    mutable f32 emmi_x{}, emmi_y{};
    mutable bool emmi_found{}, grid_name_ok{};
    bool cutscene_active{};
    mutable bool cutscene_mgr_logged{};
    mutable u64 emmy_gen{};
    mutable VAddr grid_cells_base{}, grid_scan_cursor{};
    mutable u64 grid_scan_swept_tick{}, grid_scan_diag_tick{};
    // Water: the published pool boxes, the cached scenario section / WATER_VOLUMES / ChangeIdx
    // prop entries, and the pools caught mid level change (see RefreshWaterBoxes).
    mutable std::vector<std::array<float, 4>> water_boxes;
    mutable VAddr water_section{}, water_section_keys{}, water_section_vals{};
    mutable u32 water_section_count{};
    mutable u64 water_section_walks{};
    mutable VAddr water_volumes_entry{};
    mutable std::vector<std::pair<std::string, VAddr>> water_changeidx;
    mutable std::unordered_map<u64, std::string> water_key_names;
    mutable std::map<std::string, WaterMoving> water_moving;
    mutable u32 water_moving_count{};
    mutable u64 water_next_poll_tick{};
    const bool water_debug{std::getenv("EDEN_DSMOD_WATER_DEBUG") != nullptr};
    mutable bool water_debug_dumped{};
    mutable u64 water_gen{}, wall_gen{}, marker_gen{};
    /// Last tick `water_gen` actually bumped, so a sustained live-level change (a draining pool)
    /// can't re-trigger the runtime's `water_gen`-keyed prefog rebuild (GetImage, mod_map.cpp,
    /// ~12.9 MB full-area copy at Dread's 3072-px-class raster resolution) more often than
    /// `min_bump_interval_ticks` (PublishWaterBoxes). Same "0 = never yet" sentinel
    /// the other ticks here use.
    mutable u64 last_water_gen_bump_tick{};
    mutable bool occluder_probe_logged{};
    bool game_is_dread{true};
    std::optional<bool> hud_hidden;
    std::optional<bool> minimap_hidden;
    std::unordered_map<std::string, std::string> sequence_texts;
    dread::StateSnapshot current;
    u64 published_vis_gen{~u64{0}}, published_water_gen{~u64{0}};
    u64 published_wall_gen{~u64{0}}, published_marker_gen{~u64{0}};
    std::string published_area;
    std::string published_state_area;
    // Asset-free package: the map job (map.areas_src "module:dread:areas"), whether its areas have
    // replaced the manifest's authored template, and the game strings the pages bind as
    // "loc:<KEY>" (published once the job has loaded the localization table).
    std::shared_ptr<MapJob> map_job;
    bool map_installed{};
    struct LocText {
        std::string key;                // us_english.txt key
        std::string name;               // "loc:<key>", the published text
        std::vector<std::string> gates; // need_binds of every label showing it (empty: ungated)
    };
    std::vector<LocText> loc_keys;
    void InstallMapJob();
};

u8 MemoryFacade::Read8(VAddr address) const {
    u8 value{};
    // One host call: read_memory performs the same is_mapped range check itself and leaves the
    // output untouched when it fails, so a separate AddressIsSane only doubled the page walk.
    if (owner->host.read_memory && owner->host.is_mapped &&
        address <= UINT64_MAX - sizeof(value)) {
        owner->host.read_memory(owner->host.userdata, address, &value, sizeof(value));
    }
    return value;
}

u32 MemoryFacade::Read32(VAddr address) const {
    u32 value{};
    // One host call: read_memory performs the same is_mapped range check itself and leaves the
    // output untouched when it fails, so a separate AddressIsSane only doubled the page walk.
    if (owner->host.read_memory && owner->host.is_mapped &&
        address <= UINT64_MAX - sizeof(value)) {
        owner->host.read_memory(owner->host.userdata, address, &value, sizeof(value));
    }
    return value;
}

u64 MemoryFacade::Read64(VAddr address) const {
    u64 value{};
    // One host call: read_memory performs the same is_mapped range check itself and leaves the
    // output untouched when it fails, so a separate AddressIsSane only doubled the page walk.
    if (owner->host.read_memory && owner->host.is_mapped &&
        address <= UINT64_MAX - sizeof(value)) {
        owner->host.read_memory(owner->host.userdata, address, &value, sizeof(value));
    }
    return value;
}

const u8* MemoryFacade::GetPointerSilent(VAddr address) const {
    return owner->host.get_read_pointer
               ? owner->host.get_read_pointer(owner->host.userdata, address, 4096)
               : nullptr;
}

std::vector<u8> Reader::ReadAssetBytes(const std::string& source) const {
    if (!host.read_romfs) {
        return {};
    }
    const size_t size = host.read_romfs(host.userdata, source.c_str(), 0, nullptr, 0);
    if (size == 0 || size > (64u << 20)) {
        return {};
    }
    std::vector<u8> bytes(size);
    return host.read_romfs(host.userdata, source.c_str(), 0, bytes.data(), size) == size
               ? bytes
               : std::vector<u8>{};
}

// Dread's CMinimapGrid: a fixed 650x300 array of 0x48-byte cells, 100 world units square.
constexpr size_t Stride = 0x48;
constexpr int GameCols = 650;
constexpr int GameRows = 300;
constexpr float Cell = 100.0f;
constexpr float Eps = 0.5f;
constexpr VAddr RowStride = static_cast<VAddr>(GameCols) * Stride; // 0xB6D0
constexpr size_t CellBytes = static_cast<size_t>(GameCols) * GameRows * Stride;

VAddr Reader::RootAt(s64 delta) const {
    // The Game singleton is *(*(main + 0x1CBA088)). Under NCE the module's data segment is shifted
    // by `delta`, so the caller probes both 0 and the learned vtable delta. Validate that the
    // result is a real blackboard (D=G+0x968 is a sane sections dict whose names include a fixed
    // Dread section) so a wrong shift or a not-yet-populated slot is rejected rather than read.
    auto& memory = system.ApplicationMemory();
    const VAddr slot = main_region_begin + 0x1CBA088 + static_cast<VAddr>(delta);
    if (!AddressIsSane(slot, 8))
        return 0;
    const VAddr holder = memory.Read64(slot);
    if (!AddressIsSane(holder, 8))
        return 0;
    const VAddr G = memory.Read64(holder);
    // Cover the whole sections-dict header we read below: svals@G+0x970, skeys@G+0x978,
    // count@G+0x980.
    if (G == 0 || !AddressIsSane(G, 0x984))
        return 0;
    const VAddr D = G + 0x968;
    const u32 sc = AddressIsSane(D + 0x18, 4) ? memory.Read32(D + 0x18) : 0;
    if (sc == 0 || sc > 4096)
        return 0;
    const VAddr svals = memory.Read64(D + 0x08);
    const VAddr skeys = memory.Read64(D + 0x10);
    // Whole key + value arrays, so the section walks below can index them unchecked.
    if (!AddressIsSane(svals, u64{sc} * 8) || !AddressIsSane(skeys, u64{sc} * 8))
        return 0;
    for (u32 i = 0; i < sc; ++i) {
        const std::string s = ReadCStrIdName(memory.Read64(skeys + i * 8), 24);
        if (s == "PLAYER_INVENTORY" || s == "GAME_PROGRESS" || s == "MINIMAP" ||
            s == "MISSION_LOG") {
            return G;
        }
    }
    return 0;
}

VAddr Reader::Relocated(u64 offset) const {
    // NCE relocates the title's static data segment while the heap objects themselves retain
    // their ordinary guest addresses. Keep the signed delta separate so a future build with a
    // negative relocation cannot wrap a u64 address.
    const s64 base = static_cast<s64>(main_region_begin) + static_cast<s64>(offset);
    const s64 shifted = base + nce_vtable_delta;
    return shifted > 0 ? static_cast<VAddr>(shifted) : 0;
}

bool Reader::SetHudHidden(bool hidden) const {
    if (!host.write_memory || main_region_begin == 0) {
        return false;
    }
    const VAddr global = Relocated(0x72A01914);
    if (!AddressIsSane(global, 1)) {
        Log(EDEN_DSMOD_LOG_WARNING, "DSMod NCE HUD: visibility global is not mapped ({:016X})",
            global);
        return false;
    }
    const u8 value = hidden ? 1 : 0;
    bool wrote = host.write_memory(host.userdata, global, &value, sizeof(value)) != 0;

    // ToggleHideHudAlways mirrors the global into the live game/HUD object at +0x1AE8. The
    // object is recreated on scene transitions, so this is deliberately reapplied by TickDread.
    const VAddr slot = Relocated(0x1CBA088);
    auto& memory = system.ApplicationMemory();
    if (AddressIsSane(slot, 8)) {
        const VAddr holder = memory.Read64(slot);
        const VAddr game = AddressIsSane(holder, 8) ? memory.Read64(holder) : 0;
        if (AddressIsSane(game + 0x1AE8, 1)) {
            wrote = host.write_memory(host.userdata, game + 0x1AE8, &value, sizeof(value)) != 0 ||
                    wrote;
        }
    }
    if (wrote) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod NCE HUD: {}", hidden ? "hidden" : "shown");
    }
    return wrote;
}

bool Reader::SetMinimapHidden(bool hidden) const {
    if (!host.write_memory || main_region_begin == 0) {
        return false;
    }
    // CMinimapManager's enable byte is the address passed to ClearEnableMinimap and tested by
    // IsMinimapEnabled in build 646761F643AFEBB3. Writing only this presentation gate leaves the
    // manager/grid and its water/reveal state alive for the bottom-screen reader.
    const VAddr game = BlackboardRoot();
    if (game == 0 || !AddressIsSane(game + 0x2228, 8)) {
        return false;
    }
    const VAddr manager = system.ApplicationMemory().Read64(game + 0x2228);
    const VAddr enabled = manager + 0xD63D90;
    if (!AddressIsSane(enabled, 1)) {
        return false;
    }
    const u8 value = hidden ? 0 : 1;
    const bool wrote = host.write_memory(host.userdata, enabled, &value, sizeof(value)) != 0;
    if (wrote) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod NCE minimap: {} (manager {:016X})",
            hidden ? "hidden" : "shown", manager);
    }
    return wrote;
}

bool Reader::HandleAction(const char* action, s64 argument) {
    if (!action)
        return false;
    if (std::strcmp(action, "set_hud_hidden") == 0) {
        hud_hidden = argument != 0;
        return SetHudHidden(*hud_hidden);
    }
    if (std::strcmp(action, "set_minimap_hidden") == 0) {
        minimap_hidden = argument != 0;
        return SetMinimapHidden(*minimap_hidden);
    }
    return false;
}

bool Reader::CutscenePlaying(std::string* name) const {
    // The cutscene manager is *(*(main + 0x1CBA0F0)) (same slot shape as the Game singleton).
    // Its "current cutscene" pointer sits at +0x58: Game.GetCurrentCutsceneStr returns "" when
    // it is null, else the name at *(*(cur+0x18)+0x10). Non-null = a cinematic is playing.
    // Static RE of the Lua getters (main+0x1089110 / +0x1089170).
    auto& memory = system.ApplicationMemory();
    for (const s64 delta : {s64{0}, nce_vtable_delta}) {
        const VAddr slot = main_region_begin + 0x1CBA0F0 + static_cast<VAddr>(delta);
        if (!AddressIsSane(slot, 8))
            continue;
        const VAddr holder = memory.Read64(slot);
        if (!AddressIsSane(holder, 8))
            continue;
        const VAddr mgr = memory.Read64(holder);
        if (mgr == 0 || !AddressIsSane(mgr, 0xB20))
            continue;
        const VAddr cur = memory.Read64(mgr + 0x58);
        if (!cutscene_mgr_logged) {
            cutscene_mgr_logged = true;
            Log(EDEN_DSMOD_LOG_INFO,
                "DSMod: cutscene manager {:012X} (slot delta {:#x}), current {:012X}", mgr, delta,
                cur);
        }
        if (cur == 0) {
            return false;
        }
        if (!AddressIsSane(cur, 0x180)) {
            continue;
        }
        if (name != nullptr) {
            name->clear();
            const VAddr obj = memory.Read64(cur + 0x18);
            const VAddr str = AddressIsSane(obj + 0x10, 8) ? memory.Read64(obj + 0x10) : 0;
            ReadGuestAsciiZ(str, 48, *name, [](char) { return false; });
        }
        return true;
    }
    return false;
}

VAddr Reader::BlackboardRoot() const {
    // Delta 0 first (Dynarmic, or NCE before the shift is known), then the learned NCE shift.
    // Cached per tick: several readers ask within one tick, and each RootAt walks the section
    // names byte by byte.
    if (root_cache_tick == tick_count) {
        return root_cache;
    }
    VAddr G = RootAt(0);
    if (G == 0 && nce_vtable_delta != 0) {
        G = RootAt(nce_vtable_delta);
    }
    root_cache = G;
    root_cache_tick = tick_count;
    return G;
}

template <typename Reject>
bool Reader::ReadGuestAsciiZ(VAddr p, int max, std::string& out, Reject reject) const {
    auto& memory = system.ApplicationMemory();
    int k = 0;
    while (k < max) {
        const VAddr at = p + static_cast<VAddr>(k);
        if (at < p) {
            return true; // address wrap: the per-byte range check rejected it too
        }
        // A guest page is contiguous on the host; never ask across a page boundary.
        const int chunk =
            static_cast<int>(std::min<u64>(static_cast<u64>(max - k), 0x1000 - (at & 0xFFF)));
        const u8* const bytes = (at != 0 && host.get_read_pointer)
                                    ? host.get_read_pointer(host.userdata, at,
                                                            static_cast<size_t>(chunk))
                                    : nullptr;
        if (bytes == nullptr) {
            // No host pointer (an unmapped page, or a host without the fast path): the
            // byte-at-a-time walk, which also stops at the first unmapped byte.
            for (; k < max && AddressIsSane(p + static_cast<VAddr>(k), 1); ++k) {
                const char c = static_cast<char>(memory.Read8(p + static_cast<VAddr>(k)));
                if (c == 0) {
                    return true;
                }
                if (reject(c)) {
                    return false;
                }
                out += c;
            }
            return true;
        }
        for (int i = 0; i < chunk; ++i) {
            const char c = static_cast<char>(bytes[i]);
            if (c == 0) {
                return true;
            }
            if (reject(c)) {
                return false;
            }
            out += c;
        }
        k += chunk;
    }
    return true;
}

std::string Reader::ReadCStrIdName(VAddr keyptr, int max) const {
    // A CStrId key is a pointer to an interned entry whose name char* sits at +0x10.
    auto& memory = system.ApplicationMemory();
    std::string s;
    const VAddr sp = AddressIsSane(keyptr + 0x10, 8) ? memory.Read64(keyptr + 0x10) : 0;
    ReadGuestAsciiZ(sp, max, s, [](char) { return false; });
    return s;
}

bool Reader::ForEachBlackboardProp(const std::string& section,
                                   const std::function<bool(const std::string&, VAddr)>& fn) const {
    // Sections dict at Game+0x968: values[+0x08], keys[+0x10], u32 count[+0x18]. A section holds
    // propValues[+0x20] (0x18-byte entries: inline value / data ptr at +0x00), propKeys[+0x28],
    // u32 propCount[+0x30]. RootAt validated the dict header, so its fields read without checks.
    auto& memory = system.ApplicationMemory();
    const VAddr G = BlackboardRoot();
    if (G == 0) {
        return false;
    }
    const VAddr D = G + 0x968;
    const u32 scount = memory.Read32(D + 0x18);
    const VAddr svals = memory.Read64(D + 0x08);
    const VAddr skeys = memory.Read64(D + 0x10);
    if (scount == 0 || scount > 4096 || !AddressIsSane(svals, static_cast<u64>(scount) * 8) ||
        !AddressIsSane(skeys, static_cast<u64>(scount) * 8)) {
        return false; // a blackboard mid-teardown (return to the title): walk nothing
    }
    for (u32 i = 0; i < scount; ++i) {
        if (ReadCStrIdName(memory.Read64(skeys + i * 8), 40) != section) {
            continue;
        }
        const VAddr S = memory.Read64(svals + i * 8);
        if (!AddressIsSane(S, 0x38)) {
            return false;
        }
        const u32 pcount = memory.Read32(S + 0x30);
        const VAddr pvals = memory.Read64(S + 0x20);
        const VAddr pkeys = memory.Read64(S + 0x28);
        if (pcount > 262144 ||
            (pcount != 0 && (!AddressIsSane(pvals, static_cast<u64>(pcount) * 0x18) ||
                             !AddressIsSane(pkeys, static_cast<u64>(pcount) * 8)))) {
            return false;
        }
        for (u32 j = 0; j < pcount; ++j) {
            if (!AddressIsSane(pkeys + j * 8, 8)) {
                return false;
            }
            const VAddr pk = memory.Read64(pkeys + j * 8);
            const VAddr vp = pvals + j * 0x18;
            if (!AddressIsSane(vp, 0x18)) {
                return false;
            }
            if (!fn(ReadCStrIdName(pk, 256), vp)) {
                break;
            }
        }
        return true;
    }
    return false;
}

std::string Reader::ReadScenarioName(VAddr grid) const {
    // Which scenario the live grid belongs to. Only a name that is one of the package's map areas
    // is accepted, so a wrong layout guess can never select a wrong map.
    auto& memory = system.ApplicationMemory();
    const auto ascii_at = [&](VAddr p, int max) -> std::string {
        std::string s;
        if (!ReadGuestAsciiZ(p, max, s, [](char c) { return c < 32 || c >= 127; })) {
            return {}; // not a clean ASCII scenario id
        }
        return s;
    };
    const auto known = [&](const std::string& s) {
        return !s.empty() && manifest.map_areas.contains(s);
    };
    const auto note = [&](const char* route, const std::string& id) {
        // Log once per (route, id) so the log shows which read carried the map's area.
        const std::string key = fmt::format("{}:{}", route, id);
        if (scenario_route != key) {
            scenario_route = key;
            Log(EDEN_DSMOD_LOG_INFO, "DSMod scenario '{}' via {}", id, route);
        }
    };
    // CMinimapManager+0x110 holds the current scenario as a CStrId (the same interned entry
    // the blackboard keys use: name char* at +0x10). It is the field the game's own minimap
    // update compares against the scenario names (main+0xE8FCF0); nothing else names the map.
    const VAddr sid = AddressIsSane(grid + 0x110, 8) ? memory.Read64(grid + 0x110) : 0;
    if (sid != 0 && AddressIsSane(sid + 0x10, 8)) {
        if (const std::string s = ascii_at(memory.Read64(sid + 0x10), 48); known(s)) {
            note("grid+0x110 entry+0x10", s);
            return s;
        }
    }
    return {};
}

std::optional<VAddr> Reader::FindMinimapCells() const {
    // Delta-independent: locate the CMinimapGrid cell array by STRUCTURE, so the bottom-screen map
    // resolves the instant the grid exists rather than waiting on FindEntryArray to learn the NCE
    // vtable delta (which also gates the ammo HUD -- why the map lagged it by ~3 s). The
    // 195000-cell (650x300) array is inline at grid+0x118; find it by shape, then grid = base -
    // 0x118. Sliced like FindPlayerNode / SelectLiveInventory so it never blocks the render thread.
    // Opt-in for now: the structural sweep is unverified on device and a heavy heap scan can
    // perturb a delicate save-load, so it stays off unless EDEN_DSMOD_GRIDFIND is set. Default
    // path is RootAt, which costs the map ~3 s of startup lag but is safe.
    static const bool enabled = std::getenv("EDEN_DSMOD_GRIDFIND") != nullptr;
    if (!enabled || main_region_begin == 0) {
        return std::nullopt;
    }
    auto& memory = system.ApplicationMemory();
    constexpr u32 OffX = 0x00, OffY = 0x04, OffState = 0x08, OffReal = 0x38;
    constexpr int MinRun = 128; // 128 exact +100.0 steps cannot occur by chance; < a 650 row
    const auto nearv = [](float a, float b) { return std::fabs(a - b) < Eps; };
    const auto shaped_host = [&](const u8* p) -> bool {
        if (p[OffState] > 6 || p[OffReal] > 1)
            return false;
        f32 x{}, y{};
        std::memcpy(&x, p + OffX, sizeof(x));
        std::memcpy(&y, p + OffY, sizeof(y));
        return std::isfinite(x) && std::isfinite(y) && std::fabs(x) <= 1.0e6f &&
               std::fabs(y) <= 1.0e6f;
    };
    const auto shaped_at = [&](VAddr c, f32& x, f32& y) -> bool {
        if (!AddressIsSane(c, OffReal + 1))
            return false;
        const u8 st = memory.Read8(c + OffState);
        const u8 rl = memory.Read8(c + OffReal);
        const u32 rx = memory.Read32(c + OffX), ry = memory.Read32(c + OffY);
        std::memcpy(&x, &rx, sizeof(x));
        std::memcpy(&y, &ry, sizeof(y));
        return st <= 6 && rl <= 1 && std::isfinite(x) && std::isfinite(y) &&
               std::fabs(x) <= 1.0e6f && std::fabs(y) <= 1.0e6f;
    };
    if (grid_scan_swept_tick == tick_count) {
        return std::nullopt; // per-tick guard shared with the ReadGameVisibility caller
    }
    grid_scan_swept_tick = tick_count;
    constexpr u64 PagesPerTick = 4096;
    const VAddr HeapBegin = HeapLow(), HeapEnd = HeapHigh();
    if (grid_scan_cursor < HeapBegin || grid_scan_cursor >= HeapEnd) {
        grid_scan_cursor = HeapBegin;
    }
    const VAddr slice_end = std::min<VAddr>(HeapEnd, grid_scan_cursor + PagesPerTick * 0x1000);
    for (VAddr page = grid_scan_cursor; page < slice_end; page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        for (u32 at = 0; at + (OffReal + 1) <= 0x1000; at += 8) {
            if (!shaped_host(host + at)) {
                continue;
            }
            // All-zero memory and small-int pairs pass shaped_host but never have a +100 X
            // neighbour, and they are most of the heap. Reject them on the host pointer before
            // any page-table walk; only a neighbour straddling the page falls through.
            if (at + Stride + OffReal + 1 <= 0x1000) {
                if (!shaped_host(host + at + Stride)) {
                    continue;
                }
                f32 x0{}, y0{}, x1{}, y1{};
                std::memcpy(&x0, host + at + OffX, sizeof(x0));
                std::memcpy(&y0, host + at + OffY, sizeof(y0));
                std::memcpy(&x1, host + at + Stride + OffX, sizeof(x1));
                std::memcpy(&y1, host + at + Stride + OffY, sizeof(y1));
                if (!nearv(x1, x0 + Cell) || !nearv(y1, y0)) {
                    continue;
                }
            }
            const VAddr seed = page + at;
            f32 cx{}, cy{};
            if (!shaped_at(seed, cx, cy)) {
                continue;
            }
            // Confirm a long run of column-adjacent cells (+0x48 in memory == +100 world X, same
            // Y).
            int run = 1;
            for (VAddr c = seed; run < MinRun; ++run, c += Stride) {
                f32 nx{}, ny{};
                if (!shaped_at(c + Stride, nx, ny) || !nearv(nx, cx + Cell) || !nearv(ny, cy)) {
                    break;
                }
                cx = nx;
                cy = ny;
            }
            if (run < MinRun) {
                continue;
            }
            // Walk to the array base (cell 0 == grid+0x118) via the fixed 650x300 geometry.
            VAddr row0 = seed;
            f32 rx{}, ry{};
            shaped_at(row0, rx, ry);
            for (int s = 0; s < GameCols; ++s) {
                f32 bx{}, by{};
                if (!shaped_at(row0 - Stride, bx, by) || !nearv(bx, rx - Cell) || !nearv(by, ry)) {
                    break;
                }
                row0 -= Stride;
                rx = bx;
                ry = by;
            }
            VAddr base = row0;
            f32 bx0{}, by0{};
            shaped_at(base, bx0, by0);
            for (int s = 0; s < GameRows; ++s) {
                f32 ux{}, uy{};
                if (!shaped_at(base - RowStride, ux, uy) || !nearv(ux, bx0) ||
                    !nearv(std::fabs(uy - by0), Cell)) {
                    break;
                }
                base -= RowStride;
                by0 = uy;
            }
            // Confirm against fixed geometry so a mis-walk is rejected rather than published.
            f32 b0x{}, b0y{}, ex{}, ey{}, r1x{}, r1y{};
            const bool ok = shaped_at(base, b0x, b0y) &&
                            shaped_at(base + static_cast<VAddr>(GameCols - 1) * Stride, ex, ey) &&
                            shaped_at(base + RowStride, r1x, r1y) &&
                            nearv(ex, b0x + static_cast<float>(GameCols - 1) * Cell) &&
                            nearv(ey, b0y) && nearv(r1x, b0x) &&
                            nearv(std::fabs(r1y - b0y), Cell) && AddressIsSane(base, CellBytes);
            if (!ok) {
                continue;
            }
            Log(EDEN_DSMOD_LOG_INFO,
                "DSMod: minimap grid cells {:016X} (grid {:016X}) by shape, run {}", base,
                base - 0x118, run);
            grid_scan_cursor = base; // resume near the grid if it must be re-found later
            return base;
        }
    }
    grid_scan_cursor = slice_end;
    if (grid_scan_cursor >= HeapEnd) {
        grid_scan_cursor = HeapBegin;
        if (tick_count - grid_scan_diag_tick > 900) {
            grid_scan_diag_tick = tick_count;
            Log(EDEN_DSMOD_LOG_INFO, "DSMod: minimap grid not found by shape yet");
        }
    }
    return std::nullopt;
}

bool Reader::CellsLookValid(VAddr cb) const {
    // Cheap revalidation of a cached cells base: two column-adjacent cells at index 0 plus the
    // row-1 corner still carry the grid's shape and 100-unit spacing. Rejects a freed/reused
    // allocation (return to title, save reload) so the sweep re-runs.
    auto& memory = system.ApplicationMemory();
    if (!AddressIsSane(cb, CellBytes)) {
        return false;
    }
    const auto cell = [&](VAddr c, f32& x, f32& y) -> bool {
        if (!AddressIsSane(c, 0x39))
            return false;
        if (memory.Read8(c + 0x08) > 6 || memory.Read8(c + 0x38) > 1)
            return false;
        const u32 rx = memory.Read32(c + 0x00), ry = memory.Read32(c + 0x04);
        std::memcpy(&x, &rx, sizeof(x));
        std::memcpy(&y, &ry, sizeof(y));
        return std::isfinite(x) && std::isfinite(y);
    };
    f32 x0{}, y0{}, x1{}, y1{}, xr{}, yr{};
    return cell(cb, x0, y0) && cell(cb + Stride, x1, y1) &&
           cell(cb + static_cast<VAddr>(GameCols) * Stride, xr, yr) &&
           std::fabs(x1 - (x0 + Cell)) < Eps && std::fabs(y1 - y0) < Eps &&
           std::fabs(xr - x0) < Eps && std::fabs(std::fabs(yr - y0) - Cell) < Eps;
}

// ---------------------------------------------------------------------------------------------
// Water (static RE of build 646761F643AFEBB3).
//
// What the game's own area map / HUD minimap draws (main+0xE91F18, run every frame while the
// minimap or the map screen is visible): it walks the blackboard dict
// <scenario>:WATER_VOLUMES (CGameBlackboard section of the map's scenario, prop key global
// *(main+0x1CFB418)), one {CStrId actor -> {min.xy, max.xy}} record per ENABLED pool, and clamps
// that actor's baked mapWaterPoolGeos quad to the record. Only when the map shows the live
// scenario does it resolve the actor (main+0xDEBA8C, then the component getter main+0x899610)
// and override the record with the component's live centre/size (vtable +0x180/+0x178).
//
// Who writes WATER_VOLUMES: CLiquidPoolBaseComponent::UpdateVolume (main+0x900CF4) adds/refreshes
// the pool's record while the pool is enabled and removes it otherwise. It runs when a pool is
// enabled/disabled (the Lua scripts toggle bEnabled: Artaria's water valve, Burenia's pool sets),
// at scenario load (restored level), when a level change STARTS (main+0x902334/0x90248c, still
// the start box) and when it FINISHES (main+0x909CDC -> 0x901260). So outside a running level
// change WATER_VOLUMES equals the live bounds, and it is what survives save/reload and area
// re-entry.
//
// A level change (a drain/fill): the trigger (CWaterTriggerChangeComponent on the block that was
// broken, main+0xA68408) calls CWaterPoolComponent::ChangeLevel (main+0x909530/+0x909C9C) on its
// origin and target pools, which steps the index at comp+0xCF8, sets the pending flag comp+0xD48
// and at once writes <actor>:WATERPOOL:ChangeIdx to the blackboard (the record the load restores,
// main+0x908D30). A pool going from level 0 is enabled then, so it enters WATER_VOLUMES with a
// zero-height record. The trigger then moves both pools' box colliders (comp+0x90, top at
// +0xB4) every frame over its fChangeTime, eased; the finish handler (main+0x909CDC) writes the
// final records and clears +0xD48. Verified live (build 646761F643AFEBB3, breaking
// PRP_DB_CV_008): 08a -2125 -> -2725 and 08b -3000 -> -2725 over 4.0 s; the game's HUD minimap
// shows the surfaces moving, and they freeze while the pause map is open (the game is paused).
// The level fractions are the actor's vWaterLevelChanges (romfs brfld); the level-L box is the
// pool's full mapWaterPoolGeos box with top = minY + L * height (the package bakes both as
// map.areas[].water_pools; the saved 08a record at 0.48936 is exactly -2725.002).
//
// So this reader: polls WATER_VOLUMES + the ChangeIdx records (4 Hz, a few dozen reads); a
// record that differs from the box its ChangeIdx selects is mid-change, and only then the actor
// is resolved the way DEBA8C does (hash tables and the scenario's layer maps, never a heap scan)
// and its live collider box is sampled at 10 Hz until the game writes the final record.
// ---------------------------------------------------------------------------------------------

namespace {
constexpr u64 WaterPoolVtable = 0x1A80EE8;     // CWaterPoolComponent
constexpr u64 LiquidPoolTypeSlot = 0x1CE8138;  // *(slot) -> descriptor, *(descriptor) = type key
constexpr u64 LookupFlagsSlot = 0x1CCB040;     // *(*(slot)) = DEBA8C lookup flags (the map's)
constexpr u64 WaterPollTicks = 15;             // blackboard poll: 4 Hz
constexpr u64 WaterMovingTicks = 6;            // live level sample while a pool moves: 10 Hz
constexpr float WaterBoxEps = 0.5f;            // world units (float noise on saved records)

bool BoxesClose(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    for (size_t i = 0; i < 4; ++i) {
        if (std::fabs(a[i] - b[i]) > WaterBoxEps)
            return false;
    }
    return true;
}
} // namespace

std::array<float, 4> dread::WaterPoolDef::BoxAt(float level) const {
    return {box[0], box[1], box[2], box[1] + level * (box[3] - box[1])};
}

std::optional<float> dread::WaterPoolDef::LevelAt(s64 change_idx) const {
    if (levels.empty())
        return std::nullopt; // a pool without level changes never moves
    const size_t i = static_cast<size_t>(
        std::clamp<s64>(change_idx, 0, static_cast<s64>(levels.size()) - 1));
    return levels[i];
}

const dread::WaterPoolDef* Reader::WaterPool(const std::string& name) const {
    const auto area = manifest.map_areas.find(live_scenario);
    if (area == manifest.map_areas.end())
        return nullptr;
    for (const auto& pool : area->second.water_pools) {
        if (pool.name == name)
            return &pool;
    }
    return nullptr;
}

bool Reader::WaterSectionValid() const {
    auto& memory = system.ApplicationMemory();
    return water_section != 0 && AddressIsSane(water_section, 0x38) &&
           memory.Read32(water_section + 0x30) == water_section_count &&
           memory.Read64(water_section + 0x28) == water_section_keys &&
           memory.Read64(water_section + 0x20) == water_section_vals;
}

bool Reader::CacheWaterSection() const {
    // The scenario's CSection in the Game blackboard (sections dict at Game+0x968). Its prop
    // arrays reallocate when a prop is added (the first ChangeIdx of a pool, WATER_VOLUMES itself
    // at the first enable), so the cache is keyed on the arrays and the count, and the prop names
    // are walked again only when those change.
    auto& memory = system.ApplicationMemory();
    water_section = 0;
    water_volumes_entry = 0;
    water_changeidx.clear();
    const VAddr G = BlackboardRoot();
    if (G == 0 || live_scenario.empty())
        return false;
    const VAddr D = G + 0x968;
    const u32 scount = memory.Read32(D + 0x18);
    const VAddr svals = memory.Read64(D + 0x08);
    const VAddr skeys = memory.Read64(D + 0x10);
    if (scount == 0 || scount > 4096 || !AddressIsSane(svals, u64{scount} * 8) ||
        !AddressIsSane(skeys, u64{scount} * 8))
        return false;
    VAddr S = 0;
    for (u32 i = 0; i < scount && S == 0; ++i) {
        if (ReadCStrIdName(memory.Read64(skeys + i * 8), 40) == live_scenario)
            S = memory.Read64(svals + i * 8);
    }
    if (!AddressIsSane(S, 0x38))
        return false;
    const u32 pcount = memory.Read32(S + 0x30);
    const VAddr pvals = memory.Read64(S + 0x20);
    const VAddr pkeys = memory.Read64(S + 0x28);
    if (pcount > 262144 || (pcount != 0 && (!AddressIsSane(pvals, u64{pcount} * 0x18) ||
                                            !AddressIsSane(pkeys, u64{pcount} * 8))))
        return false;
    static constexpr std::string_view Suffix = ":WATERPOOL:ChangeIdx";
    for (u32 j = 0; j < pcount; ++j) {
        const std::string name = ReadCStrIdName(memory.Read64(pkeys + j * 8), 256);
        const VAddr vp = pvals + j * 0x18;
        if (name == "WATER_VOLUMES") {
            water_volumes_entry = vp;
        } else if (name.size() > Suffix.size() && name.ends_with(Suffix)) {
            water_changeidx.emplace_back(name.substr(0, name.size() - Suffix.size()), vp);
        }
    }
    water_section = S;
    water_section_count = pcount;
    water_section_keys = pkeys;
    water_section_vals = pvals;
    ++water_section_walks;
    return true;
}

bool Reader::ReadWaterVolumes(std::vector<WaterRecord>& out) const {
    // The WATER_VOLUMES prop value: a 0x18-byte entry whose data pointer (native deref rule:
    // flag +0x14 == 1, or a value type wider than 8 bytes) is the dict {values*, keys*, u32
    // count}; a value is {min.x, min.y, max.x, max.y} (16 bytes), a key the actor's CStrId.
    auto& memory = system.ApplicationMemory();
    out.clear();
    if (water_volumes_entry == 0)
        return true; // no pool enabled in this scenario yet: the game's map draws no water
    const VAddr dict = memory.Read64(water_volumes_entry);
    if (!AddressIsSane(dict, 0x14))
        return false;
    const VAddr vals = memory.Read64(dict + 0x00);
    const VAddr keys = memory.Read64(dict + 0x08);
    const u32 count = memory.Read32(dict + 0x10);
    if (count > 256 || (count != 0 && (!AddressIsSane(vals, u64{count} * 16) ||
                                       !AddressIsSane(keys, u64{count} * 8))))
        return false;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        WaterRecord r;
        r.key = memory.Read64(keys + i * 8);
        r.id = AddressIsSane(r.key + 0x30, 8) ? memory.Read64(r.key + 0x30) : 0;
        for (u32 k = 0; k < 4; ++k) {
            const u32 raw = memory.Read32(vals + i * 16 + k * 4);
            std::memcpy(&r.box[k], &raw, 4);
        }
        if (!std::isfinite(r.box[0]) || !std::isfinite(r.box[1]) || !std::isfinite(r.box[2]) ||
            !std::isfinite(r.box[3]) || r.box[2] < r.box[0] || r.box[3] < r.box[1])
            return false;
        auto name = water_key_names.find(r.key);
        if (name == water_key_names.end()) {
            name = water_key_names.emplace(r.key, ReadCStrIdName(r.key, 128)).first;
        }
        r.name = name->second;
        out.push_back(std::move(r));
    }
    return true;
}

VAddr Reader::ResolveActor(u64 key) const {
    // A mirror of main+0xDEBA8C(Game, &key, &flags) with the flags the map passes
    // (*(*(main+0x1CCB040))): Game's two actor hash tables (buckets +0x2608/+0x2650, count
    // +0x2610/+0x2658, bucket count +0x2614/+0x265C; bucket = (lo32 ^ hi32) % n; node: key
    // object +0x00 whose +0x30 is the id, actor +0x08, next +0x20), the pending-destroy list
    // +0x2778, the spawn FIFO +0x2698 (flag 4) and the scenario delegate +0x608 (flag 2,
    // main+0x11A3D74: three arrays of 0x60-byte layer records with the same hash layout; an
    // actor whose +0x1B5 byte is set is skipped). Bounded: no chain is followed past 4096 nodes.
    auto& memory = system.ApplicationMemory();
    const VAddr G = BlackboardRoot();
    if (G == 0 || key == 0 || key == ~u64{0} || !AddressIsSane(G + 0x2600, 0x180))
        return 0;
    const VAddr flags_ptr = memory.Read64(Relocated(LookupFlagsSlot));
    const u32 flags = AddressIsSane(flags_ptr, 4) ? memory.Read32(flags_ptr) : 0;
    const u32 folded = static_cast<u32>(key) ^ static_cast<u32>(key >> 32);
    const auto lookup = [&](VAddr buckets, u32 count, u32 nbuckets) -> VAddr {
        if (count == 0 || nbuckets == 0 || !AddressIsSane(buckets, u64{nbuckets} * 8))
            return 0;
        VAddr node = memory.Read64(buckets + u64{folded % nbuckets} * 8);
        for (u32 guard = 0; node != 0 && guard < 4096; ++guard) {
            if (!AddressIsSane(node, 0x28))
                return 0;
            const VAddr k = memory.Read64(node);
            if (AddressIsSane(k + 0x30, 8) && memory.Read64(k + 0x30) == key)
                return memory.Read64(node + 0x08);
            node = memory.Read64(node + 0x20);
        }
        return 0;
    };
    const auto delegate = [&]() -> VAddr {
        if ((flags & 2) == 0)
            return 0;
        const VAddr sc = memory.Read64(G + 0x608);
        if (!AddressIsSane(sc, 0x1D0))
            return 0;
        for (const auto [base_off, count_off] :
             {std::pair{0x28, 0x38}, std::pair{0xF0, 0x100}, std::pair{0x1B8, 0x1C8}}) {
            const u32 n = memory.Read32(sc + count_off);
            const VAddr recs = memory.Read64(sc + base_off);
            if (n == 0 || n > 4096 || !AddressIsSane(recs, u64{n} * 0x60))
                continue;
            for (u32 i = 0; i < n; ++i) {
                const VAddr rec = recs + u64{i} * 0x60;
                const VAddr actor = lookup(memory.Read64(rec + 0x20), memory.Read32(rec + 0x28),
                                           memory.Read32(rec + 0x2C));
                if (actor != 0 && AddressIsSane(actor + 0x1B5, 1) &&
                    memory.Read8(actor + 0x1B5) == 0)
                    return actor;
            }
        }
        return 0;
    };
    VAddr actor = lookup(memory.Read64(G + 0x2608), memory.Read32(G + 0x2610),
                         memory.Read32(G + 0x2614));
    if (actor == 0) {
        actor = lookup(memory.Read64(G + 0x2650), memory.Read32(G + 0x2658),
                       memory.Read32(G + 0x265C));
    }
    if (actor != 0) {
        if ((flags & 8) == 0) {
            VAddr node = memory.Read64(G + 0x2778);
            for (u32 guard = 0; node != 0 && guard < 4096 && AddressIsSane(node, 0x10); ++guard) {
                if (memory.Read64(node) == actor)
                    return delegate(); // queued for destruction: native asks the scenario
                node = memory.Read64(node + 0x08);
            }
        }
        return actor;
    }
    if ((flags & 4) != 0) {
        VAddr node = memory.Read64(G + 0x2698);
        for (u32 guard = 0; node != 0 && guard < 4096 && AddressIsSane(node, 0x10); ++guard) {
            const VAddr candidate = memory.Read64(node);
            if (candidate != 0 && AddressIsSane(candidate + 0x30, 8)) {
                const VAddr k = memory.Read64(candidate + 0x30);
                if (AddressIsSane(k + 0x30, 8) && memory.Read64(k + 0x30) == key)
                    return candidate;
            }
            node = memory.Read64(node + 0x08);
        }
    }
    return delegate();
}

VAddr Reader::ResolveWaterComponent(u64 key) const {
    // main+0x899610 -> main+0x23341C: the actor's component whose type key (actor +0xA8[i],
    // count +0xB0) equals the liquid-pool descriptor's key; the component (actor +0xA0[i]) must
    // be a CWaterPoolComponent (vtable, NCE-shifted like every static address).
    auto& memory = system.ApplicationMemory();
    const VAddr actor = ResolveActor(key);
    if (actor == 0 || !AddressIsSane(actor + 0xA0, 0x14))
        return 0;
    const VAddr descriptor = memory.Read64(Relocated(LiquidPoolTypeSlot));
    const u64 type = AddressIsSane(descriptor, 8) ? memory.Read64(descriptor) : 0;
    const u32 n = memory.Read32(actor + 0xB0);
    const VAddr comps = memory.Read64(actor + 0xA0);
    const VAddr types = memory.Read64(actor + 0xA8);
    if (type == 0 || n == 0 || n > 4096 || !AddressIsSane(comps, u64{n} * 8) ||
        !AddressIsSane(types, u64{n} * 8))
        return 0;
    const VAddr vtable = Relocated(WaterPoolVtable);
    for (u32 i = 0; i < n; ++i) {
        if (memory.Read64(types + u64{i} * 8) != type)
            continue;
        const VAddr comp = memory.Read64(comps + u64{i} * 8);
        return AddressIsSane(comp, 0xD50) && memory.Read64(comp) == vtable ? comp : 0;
    }
    return 0;
}

bool Reader::ReadLiveWaterBox(VAddr comp, std::array<float, 4>& box, bool& moving) const {
    // What the map's getters (component vtable +0x180/+0x178, main+0xA47D88/+0xA47D20) return:
    // the bounds object at comp+0x88, min/max at +0x40..+0x4C. The getters first refresh it
    // (its vtable +0x98) when its dirty byte +0x60 is set; this reader cannot call that, so a
    // dirty bounds object is replaced by its source, the pool's box collider (comp+0x90, min/max
    // at +0xA8..+0xB4), which ApplyLevel (main+0x900928) and a drain/fill trigger
    // (CWaterTriggerChangeComponent, main+0xA68408) write every frame of a change -- observed
    // live: PRP_CV_watercave08a -2125 -> -2725 and 08b -3000 -> -2725 over 4 s, eased, with
    // bounds and collider equal on every sample. (A collider is only populated while the pool is
    // simulated; the sanity checks reject an idle one.) +0xD48 is ChangeLevel's pending flag:
    // set when the change starts, cleared by the finish handler (main+0x909CDC) right after it
    // writes the final WATER_VOLUMES record.
    auto& memory = system.ApplicationMemory();
    if (!AddressIsSane(comp, 0xD50) || memory.Read64(comp) != Relocated(WaterPoolVtable))
        return false;
    const auto read_box = [&](VAddr at, std::array<float, 4>& out) {
        if (!AddressIsSane(at, 0x10))
            return false;
        for (u32 k = 0; k < 4; ++k) {
            const u32 raw = memory.Read32(at + k * 4);
            std::memcpy(&out[k], &raw, 4);
        }
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) &&
               std::isfinite(out[3]) && out[2] > out[0] && out[3] >= out[1] &&
               std::fabs(out[0]) < 1.0e6f && std::fabs(out[1]) < 1.0e6f &&
               std::fabs(out[2]) < 1.0e6f && std::fabs(out[3]) < 1.0e6f;
    };
    const VAddr bounds = memory.Read64(comp + 0x88);
    const bool bounds_ok = read_box(bounds + 0x40, box);
    const bool dirty = bounds_ok && AddressIsSane(bounds + 0x60, 1) && memory.Read8(bounds + 0x60);
    if (!bounds_ok || dirty) {
        std::array<float, 4> collider{};
        if (read_box(memory.Read64(comp + 0x90) + 0xA8, collider))
            box = collider;
        else if (!bounds_ok)
            return false;
    }
    moving = memory.Read8(comp + 0xD48) != 0;
    return true;
}

void Reader::PublishWaterBoxes(std::vector<std::array<float, 4>>& boxes) const {
    // Order-independent compare: the dict order is the game's insertion order.
    std::ranges::sort(boxes, [](const auto& a, const auto& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    });
    if (boxes == water_boxes) {
        return;
    }
    // A moving pool changes its box on every sample; each bump re-rasterises the area's prefog
    // image in the runtime (GetImage, mod_map.cpp), so bumps are rate-limited here. A too-early
    // call leaves `water_boxes` stale on purpose so the next call still sees the difference.
    // Interval env-overridable for measurement; default ~10 Hz.
    static const u64 min_bump_interval_ticks = [] {
        const char* v = std::getenv("EDEN_DSMOD_WATER_GEN_INTERVAL_TICKS");
        const long parsed = v != nullptr ? std::strtol(v, nullptr, 10) : 0;
        return parsed > 0 ? static_cast<u64>(parsed) : 6; // 6 ticks @ 60 Hz poll_hz ~= 10 Hz
    }();
    if (last_water_gen_bump_tick != 0 &&
        tick_count - last_water_gen_bump_tick < min_bump_interval_ticks) {
        return;
    }
    water_boxes = boxes;
    last_water_gen_bump_tick = tick_count;
    ++water_gen;
    if (water_moving_count == 0 || water_debug) {
        std::string dump;
        for (size_t i = 0; i < water_boxes.size() && i < 16; ++i) {
            dump += fmt::format(" [{:.0f}..{:.0f},{:.0f}..{:.0f}]", water_boxes[i][0],
                                water_boxes[i][2], water_boxes[i][1], water_boxes[i][3]);
        }
        Log(EDEN_DSMOD_LOG_INFO, "DSMod water: {} pool box(es) (gen {}, {} moving):{}",
            water_boxes.size(), water_gen, water_moving_count, dump);
    }
}

void Reader::ResetWater() const {
    water_boxes.clear();
    water_section = 0;
    water_section_count = 0;
    water_section_keys = 0;
    water_section_vals = 0;
    water_volumes_entry = 0;
    water_changeidx.clear();
    water_key_names.clear();
    water_moving.clear();
    water_moving_count = 0;
    water_next_poll_tick = 0;
    ++water_gen;
}

void Reader::RefreshWaterBoxes() const {
    if (main_region_begin == 0 || live_scenario.empty() || tick_count < water_next_poll_tick)
        return;
    auto& memory = system.ApplicationMemory();
    water_next_poll_tick = tick_count + WaterPollTicks;
    if (!WaterSectionValid() && !CacheWaterSection())
        return; // blackboard not ready (load, title): keep the last publication
    std::vector<WaterRecord> records;
    if (!ReadWaterVolumes(records)) {
        water_section = 0; // re-resolve next poll; keep the last publication meanwhile
        return;
    }
    std::unordered_map<std::string, s64> change_idx;
    for (const auto& [pool, vp] : water_changeidx) {
        change_idx[pool] = static_cast<s32>(memory.Read32(vp)); // inline int
    }
    std::vector<std::array<float, 4>> boxes;
    boxes.reserve(records.size());
    std::map<std::string, WaterMoving> still_moving;
    for (const auto& r : records) {
        std::array<float, 4> box = r.box;
        const dread::WaterPoolDef* def = WaterPool(r.name);
        const auto idx = change_idx.find(r.name);
        const std::optional<float> level =
            def != nullptr ? def->LevelAt(idx != change_idx.end() ? idx->second : 0)
                           : std::nullopt;
        // The record the game writes once the change finishes. Only then is a mismatch a
        // change in progress: a record that already equals its target is at rest.
        if (level.has_value() && !BoxesClose(r.box, def->BoxAt(*level))) {
            const auto prev = water_moving.find(r.name);
            WaterMoving m = prev != water_moving.end() && prev->second.record == r.box &&
                                    prev->second.idx == (idx != change_idx.end() ? idx->second : 0)
                                ? prev->second
                                : WaterMoving{};
            m.record = r.box;
            m.idx = idx != change_idx.end() ? idx->second : 0;
            if (m.comp == 0 && !m.unresolved) {
                m.comp = ResolveWaterComponent(r.id);
                m.unresolved = m.comp == 0;
                Log(EDEN_DSMOD_LOG_INFO,
                    "DSMod water: '{}' changing (ChangeIdx {} -> level {:.4f}; record top {:.1f}, "
                    "target {:.1f}); live component {:012X}",
                    r.name, m.idx, *level, r.box[3], def->BoxAt(*level)[3], m.comp);
            }
            bool moving{};
            std::array<float, 4> live{};
            if (m.comp != 0 && ReadLiveWaterBox(m.comp, live, moving)) {
                // What the native map draws for a live-scenario record: the live bounds.
                box = live;
                m.live = moving;
                if (water_debug) {
                    Log(EDEN_DSMOD_LOG_INFO,
                        "DSMod water: '{}' live top {:.1f} pending={} (record top {:.1f})", r.name,
                        live[3], moving, r.box[3]);
                }
            } else {
                m.comp = 0; // freed or unresolvable: the record is what the native map falls
                m.live = false; // back to as well
            }
            still_moving.emplace(r.name, m);
        }
        boxes.push_back(box);
    }
    if (water_debug && !water_debug_dumped && !records.empty()) {
        // One-shot cross-check (EDEN_DSMOD_WATER_DEBUG): every record against its live collider.
        water_debug_dumped = true;
        for (const auto& r : records) {
            const VAddr comp = ResolveWaterComponent(r.id);
            bool pending = false;
            std::array<float, 4> live{};
            const bool ok = comp != 0 && ReadLiveWaterBox(comp, live, pending);
            Log(EDEN_DSMOD_LOG_INFO,
                "DSMod water debug: '{}' record [{},{},{},{}] component {:012X} live {} "
                "[{},{},{},{}] pending={}",
                r.name, r.box[0], r.box[1], r.box[2], r.box[3], comp, ok, live[0], live[1],
                live[2], live[3], pending);
        }
    }
    water_moving = std::move(still_moving);
    water_moving_count = static_cast<u32>(std::ranges::count_if(
        water_moving, [](const auto& entry) { return entry.second.live; }));
    if (water_moving_count != 0)
        water_next_poll_tick = tick_count + WaterMovingTicks;
    PublishWaterBoxes(boxes);
}

bool Reader::ReadGameVisibility(const std::string& scenario_hint) const {
    auto& memory = system.ApplicationMemory();
    // The LIVE explored map is the CMinimapGrid the Game owns: grid = *(Game+0x2228), a 650x300
    // array of 0x48-byte cells inline at grid+0x118 (visibility state at +0x08). Resolve cheapest
    // first: (1) a structurally-found cells base -- delta-independent, so the map appears the
    // instant the grid exists instead of when FindEntryArray learns the NCE shift; (2) the .bss
    // singleton via RootAt (validates the blackboard and covers the shift). The grid is rebuilt in
    // place on a scenario change, so a cached base survives it.
    VAddr grid = 0;
    if (grid_cells_base != 0 && CellsLookValid(grid_cells_base)) {
        grid = grid_cells_base - 0x118;
    } else {
        grid_cells_base = 0; // stale (freed / back to title) -- drop it
        if (const auto cb = FindMinimapCells()) {
            grid_cells_base = *cb;
            grid = *cb - 0x118;
        } else {
            VAddr Game = BlackboardRoot();
            if (Game == 0) {
                vis_grid_missing = true;
                return false; // scan still sweeping and no delta yet
            }
            grid = AddressIsSane(Game + 0x2228, 8) ? memory.Read64(Game + 0x2228) : 0;
        }
    }
    if (!AddressIsSane(grid, 0x120)) {
        vis_grid_missing = true;
        grid_name_ok = false;
        return false;
    }
    const VAddr cells = grid + 0x118;
    if (!AddressIsSane(cells, CellBytes)) {
        vis_grid_missing = true;
        grid_name_ok = false;
        return false;
    }
    vis_grid_missing = false;
    grid_cells_base = cells; // cache only a base whose whole cell array is mapped
    // Which area to draw: the live scenario id (grid+0x110, else the scenario-named blackboard
    // section -- both NCE-safe, so the map follows an elevator ride on the Thor), else the
    // scenario the guest-call sequence resolved (Dynarmic only), else the alphabetically-first
    // area -- Dread's start (s010_cave).
    const std::string grid_scenario = ReadScenarioName(grid);
    std::string area = grid_scenario;
    // The grid's own name goes first when the level is torn down (return to the title): while it
    // is unreadable the blackboard is not safe to walk either -- the readers stand down on it.
    grid_name_ok = !area.empty();
    if (area.empty()) {
        // Only the game's own scenario id (the manager's, else the Lua scenario sequence)
        // selects a map; an unnamed grid publishes nothing.
        if (!scenario_hint.empty() && manifest.map_areas.contains(scenario_hint)) {
            area = scenario_hint;
        } else {
            vis_grid_missing = true;
            return false;
        }
    }
    // The game grid's cells are 100 world units square, its origin the area's (min_x,min_y); the
    // grid runs 650x300 cells = 65000x30000 units, LARGER than the rasterised map bounds. So a cell
    // maps by WORLD position over the same bounds the geometry and marker use -- not by col/650 --
    // and each cell paints the reveal-grid rectangle it covers so upsampling leaves no gaps.
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end()) {
        return false;
    }
    const float span_x = ma->second.max_x - ma->second.min_x;
    const float span_y = ma->second.max_y - ma->second.min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return false;
    }
    // Zero-copy: read only the ONE state byte each reveal cell needs, straight off the host page
    // (the whole cell array was validated sane above): one page lookup per page crossing instead
    // of a page-table walk per cell, and no 13 MB copy. The scratch grid is reused across calls
    // so a call that finds nothing changed (most of them) allocates nothing.
    constexpr int RC = dread::VisitedGrid::Cols, RR = dread::VisitedGrid::Rows;
    auto& fresh = vis_scratch;
    fresh.assign(static_cast<size_t>(RC) * RR, dread::VisitedGrid::Unexplored);
    std::array<int, RC> gcol{};
    for (int mc = 0; mc < RC; ++mc) {
        gcol[static_cast<size_t>(mc)] = static_cast<int>((mc + 0.5f) / RC * span_x / Cell);
    }
    int on = 0;
    // INVERSE sampling: for each reveal cell, read the ONE game cell its centre falls in. Forward
    // rect-filling (game cell -> reveal rectangle) inflated the result -- a game cell is 100 units
    // but a reveal cell is span/650 (~95) units, so floor/ceil painted ~2 reveal cells per game
    // cell and the touched/visited area came out visibly larger than the top-screen minimap.
    for (int mr = 0; mr < RR; ++mr) {
        // Reveal row 0 = top = max_y. World Y of this reveal row's centre, as an offset from min_y.
        const float wy_off = (1.0f - (mr + 0.5f) / RR) * span_y;
        const int grow = static_cast<int>(wy_off / Cell);
        if (grow < 0 || grow >= GameRows) {
            continue;
        }
        const size_t rbase = static_cast<size_t>(grow) * GameCols;
        VAddr page = ~VAddr{0};
        const u8* host = nullptr;
        for (int mc = 0; mc < RC; ++mc) {
            const int gc = gcol[static_cast<size_t>(mc)];
            if (gc < 0 || gc >= GameCols) {
                continue;
            }
            const VAddr a = cells + static_cast<VAddr>((rbase + gc) * Stride + 0x08);
            if ((a & ~VAddr{0xFFF}) != page) {
                page = a & ~VAddr{0xFFF};
                host = memory.GetPointerSilent(page);
            }
            if (host == nullptr) {
                return false; // Keep the previous snapshot if a page disappeared mid-read.
            }
            const u8 state = host[a & 0xFFF];
            // The game draws a cell only when state > 1 (0 unknown, 1 a faint fringe it hides), so
            // the bottom map does not reveal more than the top. state 6 ('@') = walked (visited,
            // bright); 2..5 = only seen (revealed, dim).
            if (state <= 1) {
                continue;
            }
            fresh[static_cast<size_t>(mr) * RC + mc] =
                state >= 6 ? dread::VisitedGrid::Visited : dread::VisitedGrid::Revealed;
            ++on;
        }
    }
    if (!AddressIsSane(grid + 0x110, 8) || ReadScenarioName(grid) != grid_scenario) {
        return false; // A scenario transition interrupted the sample.
    }
    // An all-unexplored grid is a valid state (a fresh scenario before its first reveal): the
    // game shows an empty minimap, so the scenario is published either way.
    if (live_scenario != area) {
        // Download state belongs to this scenario. Until its next native flag sample, keep
        // the coarse overview hidden instead of inheriting the previous area's download.
        if (map_unlocked) {
            map_unlocked = false;
            ++emmy_gen;
        }
        // Per-scenario caches must not carry over: a freed EMMI component keeps its vtable, and
        // the previous area's pools overlap the new one in world space, so both would publish a
        // stale read onto the new map until their next lap.
        emmi_actor = 0;
        zone_doors_closed = false; // do not carry a "chase sealed" veto across a scenario change
        ResetWater();
    }
    live_scenario = area; // publish only a scenario that actually resolved to explored cells
    game_vis_areas.insert(area);
    auto& g = map_visited[area];
    // Mirror the live grid exactly -- no monotonic guard. Failed page reads and scenario changes
    // return before publishing; this is a best-effort sample, not an atomic guest snapshot.
    // Matching it 1:1 is what keeps the bottom map identical to the game's own minimap, and lets a
    // freshly loaded save replace a prior run's state instead of accumulating stale cells.
    if (g.cells != fresh) {
        // Log the explored bounding box in world units too: it places the reveal on the area map
        // without a capture, and shows at once when the marker sits outside the explored rooms.
        int c0 = RC, c1 = -1, r0 = RR, r1 = -1;
        for (int mr = 0; mr < RR; ++mr) {
            for (int mc = 0; mc < RC; ++mc) {
                if (fresh[static_cast<size_t>(mr) * RC + mc] != dread::VisitedGrid::Unexplored) {
                    c0 = std::min(c0, mc);
                    c1 = std::max(c1, mc);
                    r0 = std::min(r0, mr);
                    r1 = std::max(r1, mr);
                }
            }
        }
        const float bx0 = ma->second.min_x + static_cast<float>(c0) / RC * span_x;
        const float bx1 = ma->second.min_x + static_cast<float>(c1 + 1) / RC * span_x;
        const float by0 = ma->second.max_y - static_cast<float>(r1 + 1) / RR * span_y;
        const float by1 = ma->second.max_y - static_cast<float>(r0) / RR * span_y;
        Log(EDEN_DSMOD_LOG_INFO,
            "DSMod game-vis '{}': {} cells (live grid {:012X}) gen {} explored x[{:.0f}..{:.0f}] "
            "y[{:.0f}..{:.0f}]",
            area, on, grid, g.generation + 1, bx0, bx1, by0, by1);
        // Fade bookkeeping: a cell that changed (appeared, or upgraded from seen to walked) records
        // its previous category and the current tick so the renderer fades from the old appearance
        // to the new one; unchanged cells keep their prior change time.
        for (size_t i = 0; i < fresh.size(); ++i) {
            if (fresh[i] != g.cells[i]) {
                g.prev[i] = g.cells[i];
                g.change_tick[i] = static_cast<u32>(tick_count);
            }
        }
        g.cells.swap(fresh);
        ++g.generation;
        g.last_reveal_tick = tick_count;
    }
    return true;
}

VAddr Reader::FindBlackboardProp(const std::string& section, const std::string& prop) const {
    VAddr found = 0;
    const bool complete = ForEachBlackboardProp(section, [&](const std::string& nm, VAddr vp) {
        if (nm != prop) {
            return true;
        }
        found = system.ApplicationMemory().Read64(vp);
        return false;
    });
    return complete ? found : 0;
}

std::optional<bool> Reader::EmmyDefeatedInScenario(const std::string& scenario) const {
    // Which record marks a zone's E.M.M.I. as defeated. "<CU>:CENTRAL_UNIT:EmmyDisabled" turned
    // out to be a live patrol state (it toggled 1/0 during play on the handheld with the EMMI
    // alive), so it is NOT used. "<CU>:CENTRAL_UNIT:State" is a progress enum: the finished
    // tutorial unit (Proto) reads 7 and stays there, the caves unit stepped 2 -> 4 -> 6 while
    // the zone was being cleared. The zone paints green once its Central Unit's State reaches
    // 7. nullopt when the section could not be read.
    //
    // Nothing is filtered by name: every ":CENTRAL_UNIT:State" prop in the scenario section is
    // counted, Proto's included, although the Proto unit lives in the same scenario as the caves
    // one and does not own the emmy layer. Whether that ever produces a false "dead" verdict is
    // NOT confirmed -- it needs the real prop names for a scenario with a live, chasing EMMI,
    // which the diagnostics below capture (see "DSMod EMMI dead-check"). The order is record
    // first, then "every CU reads 7". The chase-state invariant at the call site (TickDread) is a
    // path-independent safety net regardless of which of these two paths is at fault.
    if (scenario.empty()) {
        return std::nullopt;
    }
    auto& memory = system.ApplicationMemory();
    // The save's own record comes first: the MINIMAP section keeps "MINIMAP:EmmyDead[<scenario>]"
    // (seen =1 for s010_cave in a save with the caves EMMI destroyed, absent in a fresh game),
    // which is what the game's map colours from. The Central Unit state below is the fallback.
    const u64 record_raw = FindBlackboardProp("MINIMAP", "MINIMAP:EmmyDead[" + scenario + "]");
    const bool record_dead = (record_raw & 0xFF) != 0;
    // The game's own dead test (main+0xDDB938, run by the minimap manager before it writes the
    // record above) is "every Central Unit component of the scenario reports state 7"; the
    // blackboard keeps that state as <CU>:CENTRAL_UNIT:State, so it is the fallback for a save
    // whose record has not been written yet.
    u32 cu_total = 0, cu_seven = 0;
    std::string cu_seen;
    const bool found = ForEachBlackboardProp(scenario, [&](const std::string& nm, VAddr vp) {
        if (nm.ends_with(":CENTRAL_UNIT:State")) {
            const u32 state = static_cast<u32>(memory.Read64(vp) & 0xFF);
            ++cu_total;
            if (state == 7) {
                ++cu_seven;
            }
            cu_seen += fmt::format(" [{}]={}", nm, state);
        }
        return true;
    });
    // Diagnostics: every CU prop name + state this walk saw, the raw save-record byte,
    // and which verdict each of the two paths would produce, so a device log settles "which path
    // fired" (and whether Proto's own CU is even present in this walk) without guesswork. Logged
    // on change only -- this call can poll at ~1 Hz while the live manager read is unresolved.
    if (cu_seen != emmy_cu_seen || scenario != emmy_cu_scenario) {
        emmy_cu_seen = cu_seen;
        emmy_cu_scenario = scenario;
        Log(EDEN_DSMOD_LOG_INFO,
            "DSMod EMMI dead-check '{}': record MINIMAP:EmmyDead[{}]=0x{:X} (record_dead={}) "
            "CU props read={} total={} seven={}{} -> cu_verdict={}",
            scenario, scenario, record_raw & 0xFF, record_dead, found, cu_total, cu_seven,
            cu_seen.empty() ? " (none)" : cu_seen,
            !found ? "unresolved" : (cu_total != 0 && cu_seven == cu_total ? "dead" : "alive"));
    }
    if (record_dead) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod EMMI dead-check '{}': returning dead=true via save record",
            scenario);
        return true; // The property payload is an inline bool, not a guest pointer.
    }
    if (!found) {
        return std::nullopt;
    }
    // main+0xDDB938: dead iff EVERY Central Unit of the scenario reports state 7.
    return cu_total != 0 && cu_seven == cu_total;
}

void Reader::ReadCustomMarkers(const std::string& scenario,
                               std::vector<dread::StateSnapshot::CustomMarker>& out) const {
    // "<scenario>:MINIMAP:CustomMarkers" (CMinimapManager::TCustomMarkerDataMap), probed live
    // by placing three markers: a keyed array -- values pointer at +0x00, u32 id keys at +0x08,
    // u32 count at +0x10 -- whose value records are 0x118 bytes: u32 colour slot at +0x00 (the
    // dialog's red/green/yellow/magenta/cyan = 0..4), u32 id at +0x04, the marker's map box as
    // floats at +0x2C (x0) +0x30 (y0) +0x34 (x1) +0x38 (y1) -- one map cell around the cursor,
    // so its centre is where the marker sits. The prop only exists once a marker was placed.
    out.clear();
    if (scenario.empty()) {
        return;
    }
    const VAddr map = FindBlackboardProp(scenario, "MINIMAP:CustomMarkers");
    if (map == 0 || !AddressIsSane(map, 0x18)) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const VAddr values = memory.Read64(map);
    const u32 count = memory.Read32(map + 0x10);
    constexpr u32 Stride = 0x118;
    if (values == 0 || count == 0 || count > 16 || !AddressIsSane(values, Stride * count)) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        const VAddr rec = values + static_cast<VAddr>(i) * Stride;
        const u32 color = memory.Read32(rec + 0x00);
        const auto rf = [&](VAddr at) {
            const u32 bits = memory.Read32(at);
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        };
        const float x0 = rf(rec + 0x2C), y0 = rf(rec + 0x30), x1 = rf(rec + 0x34),
                    y1 = rf(rec + 0x38);
        if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1)) ||
            std::fabs(x0) > 1.0e6f || std::fabs(y0) > 1.0e6f || x1 < x0 || y1 < y0 ||
            (x1 - x0) > 5000.0f || (y1 - y0) > 5000.0f || color > 8) {
            continue;
        }
        out.push_back({(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, static_cast<s32>(color)});
    }
}

void Reader::ReadMissionLog(std::vector<std::string>& ids) const {
    // MISSION_LOG:EventsList (section MISSION_LOG): +0x00 ptr -> records, +0x08 u32 count,
    // +0x0C u32 capacity. Records are 0x38 bytes: +0x00 flag (1 = carries dialogue pages),
    // +0x08 CStrId entry of the event id (MLOG_*), +0x10 ptr -> page ids, +0x18 u32 page count.
    // Chronological: the last record is the newest. Probed live (12 records on a real save
    // matched the save file's list).
    ids.clear();
    const VAddr list = FindBlackboardProp("MISSION_LOG", "MISSION_LOG:EventsList");
    if (list == 0 || !AddressIsSane(list, 0x10)) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const VAddr recs = memory.Read64(list);
    const u32 count = memory.Read32(list + 0x08);
    constexpr u32 Stride = 0x38;
    if (recs == 0 || count == 0 || count > 65536 || !AddressIsSane(recs, Stride * count)) {
        return;
    }
    ids.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const VAddr rec = recs + static_cast<VAddr>(i) * Stride;
        const VAddr key = memory.Read64(rec + 0x08);
        std::string name = key != 0 ? ReadCStrIdName(key, 256) : std::string{};
        if (name.starts_with("#")) {
            name.erase(0, 1); // the live ids carry a '#' the localization keys do not
        }
        if (name.starts_with("MLOG_")) {
            ids.push_back(std::move(name));
        }
    }
}

// BTXT: "BTXT" u16 u16, then repeated ASCII key\0 + UTF-16LE text \0\0. Kept as ASCII (the
// canvas font is ASCII): non-ASCII glyphs drop, "{cN}" colour tags drop. False when not a BTXT.
bool ParseBtxt(const std::vector<u8>& bytes, std::unordered_map<std::string, std::string>& out) {
    if (bytes.size() <= 8 || std::memcmp(bytes.data(), "BTXT", 4) != 0)
        return false;
    size_t p = 8;
    while (p < bytes.size()) {
        size_t e = p;
        while (e < bytes.size() && bytes[e] != 0)
            ++e;
        if (e >= bytes.size())
            break;
        std::string k(reinterpret_cast<const char*>(bytes.data() + p), e - p);
        p = e + 1;
        std::string text;
        bool in_tag = false;
        while (p + 1 < bytes.size() && !(bytes[p] == 0 && bytes[p + 1] == 0)) {
            const u16 ch = static_cast<u16>(bytes[p] | (bytes[p + 1] << 8));
            p += 2;
            if (ch == '{') {
                in_tag = true;
                continue;
            }
            if (in_tag) {
                if (ch == '}')
                    in_tag = false;
                continue;
            }
            if (ch >= 32 && ch < 127)
                text += static_cast<char>(ch);
        }
        p += 2;
        out.emplace(std::move(k), std::move(text));
    }
    return true;
}

const std::string& Reader::LocalizedText(const std::string& key) {
    static const std::string empty;
    if (!localized_loaded) {
        // Normally the map job (asset-free package) has loaded the table off the tick thread by
        // now; this is the fallback when it could not.
        localized_loaded = true;
        const auto bytes = ReadAssetBytes("romfs:/system/localization/us_english.txt");
        if (ParseBtxt(bytes, localized)) {
            Log(EDEN_DSMOD_LOG_INFO, "DSMod: {} localized strings loaded", localized.size());
        } else {
            Log(EDEN_DSMOD_LOG_WARNING, "DSMod: no BTXT localization in the romfs ({} bytes)",
                bytes.size());
        }
    }
    const auto it = localized.find(key);
    return it == localized.end() ? empty : it->second;
}

std::optional<Reader::ZoneFlags> Reader::ReadMinimapZoneFlags() const {
    // The game's own minimap state, read where its map colours from. The CMinimapManager (the
    // object at *(Game+0x2228) whose cell array the reveal reader walks) keeps its E.M.M.I. zone
    // record at +0xD63D30: byte +0x39 (manager+0xD63D69) = "zone closed", byte +0x3A
    // (manager+0xD63D6A) = "EMMI dead". The area-map raster (main+0xEA1D78) paints the zone
    // vEmmyZoneClosedColor while +0x39 is set, vEmmyDeadColor while +0x3A is set, vEmmyColor
    // otherwise. The manager's per-frame update (main+0xE8FE8C, whenever the scenario's grid is
    // the live one) recomputes +0x39 through main+0xE91A28: set iff any CDoorEmmyFXComponent
    // holds its "closed" link (comp+0x78 != 0, put there by main+0x7B6BE4 from the chase
    // action's start (main+0x511BE4) / end (main+0x512B94) when bCloseEmmyDoorsDuringChase) or
    // is still fading (comp+0x120 > 0); and it sets +0x3A through main+0xE9128C once every
    // Central Unit component reports state 7 (main+0xDDB938), writing MINIMAP:EmmyDead[<scenario>]
    // at the same moment; a scenario load restores +0x3A from that record (main+0xE99950).
    auto& memory = system.ApplicationMemory();
    VAddr mgr = 0;
    if (grid_cells_base != 0) {
        mgr = grid_cells_base - 0x118;
    } else if (const VAddr G = BlackboardRoot(); G != 0 && AddressIsSane(G + 0x2228, 8)) {
        mgr = memory.Read64(G + 0x2228);
    }
    if (mgr == 0 || !AddressIsSane(mgr + 0xD63D69, 2)) {
        return std::nullopt;
    }
    ZoneFlags f;
    f.closed = memory.Read8(mgr + 0xD63D69) != 0;
    f.dead = memory.Read8(mgr + 0xD63D6A) != 0;
    // g_uColorCellLevels.z of the compositing shader = the map-room factor at mgr+0xD64618
    // (assembled at main+0xE8FE5C): > the pixel's v means "map room unlocked" -- the area map
    // downloaded at a map station, which is when unexplored rooms show at 0.3x their class colour.
    if (AddressIsSane(mgr + 0xD64618, 4)) {
        const u32 r = memory.Read32(mgr + 0xD64618);
        f32 v{};
        std::memcpy(&v, &r, 4);
        f.maproom_factor = std::isfinite(v) ? v : 0.0f;
    }
    return f;
}

bool Reader::ReadEmmiCenter(VAddr actor) const {
    // The actor keeps its world transform as a row-major 4x4 at actor+0x38 (rows of four floats;
    // row 3 is 0,0,0,1), so the translation is the last column: x at +0x44, y at +0x54, z at
    // +0x64. Settled from a handheld log with the Proto EMMI live: (17027, -3310)
    // in that column beside its spawn point SP_EmmyProto (16150, -3336). A unit that has not
    // been placed yet holds the identity, so the column reads (0,0) -- rejected below.
    auto& memory = system.ApplicationMemory();
    if (!AddressIsSane(actor, 0xA0)) {
        return false;
    }
    const auto ma = manifest.map_areas.find(live_scenario);
    if (ma == manifest.map_areas.end()) {
        return false;
    }
    const auto f32_at = [&](VAddr a) {
        const u32 r = memory.Read32(a);
        f32 v{};
        std::memcpy(&v, &r, 4);
        return v;
    };
    const f32 x = f32_at(actor + 0x44), y = f32_at(actor + 0x54);
    const f32 w = f32_at(actor + 0x74); // row 3 ends in 1.0 for any affine transform
    const bool in_area = std::isfinite(x) && std::isfinite(y) &&
                         (std::fabs(x) > 50.0f || std::fabs(y) > 50.0f) && x >= ma->second.min_x &&
                         x <= ma->second.max_x && y >= ma->second.min_y && y <= ma->second.max_y;
    if (!in_area || !(std::fabs(w - 1.0f) < 0.01f)) {
        return false;
    }
    emmi_x = x;
    emmi_y = y;
    emmi_found = true;
    return true;
}

bool Reader::AreaHasEmmi(const std::string& area) const {
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end()) {
        return false;
    }
    return std::ranges::any_of(ma->second.layers, [](const dread::MapLayer& l) {
        return l.geo.find(".emmy.") != std::string::npos;
    });
}

bool Reader::EmmiEntityState(VAddr actor, EmmiInfo& info) const {
    // The game's own test for "this entity is an E.M.M.I.", as SetEmmyPhase1/2 (main+0x1080380 /
    // 0x10804F0) and KillEmmy (0x107FF70) apply it to every entity of the Game's list: the
    // component at entity+0x240 IsKindOf CEmmyAIComponent. IsKindOf resolves the class through
    // vtable slot 0 (GetClass); every concrete EMMI AI class has its own vtable and GetClass
    // thunk, so the test here is "the component's vtable is one of those": the vtable address
    // (a data address, carrying the NCE shift like the attack component's vtable did) or, as a
    // second chance, slot 0 being one of the thunks (a text address). Classes: CEmmyAIComponent
    // vtable 0x1A482D8 / thunk 0x55C338, Cave 0x1A43048 / 0x4F0DA0, Forest 0x1A43A40 / 0x4F325C,
    // Lab 0x1A443E0 / 0x4F8140, Magma 0x1A44D80 / 0x4F9524, Proto 0x1A45720 / 0x4FCE4C, Sanc
    // 0x1A46190 / 0x500154, Shipyard 0x1A46EF8 / 0x504780.
    // What comes back with it: the +0x268 component's state word (+0x70, getter 0x113912C;
    // the bindings act only on 2 or 4); "enabled" as every consumer of the EMMI AI registry
    // tests it (main+0x24FBC8: component+0x20 set, owner+0x1B4 set, and owner+0x1B0 clear or
    // owner+0x1A8 set); and the component's own bTargetInsideEmmyZone (+0x14C35, a reflected
    // bool read by the AI update 0x4FF630 and the zone effects 0x772010) -- Samus is inside
    // this unit's zone, which is when the game's minimap shows the unit (its tutorial tip
    // TIP_031 "EMMI zone: EMMI and the minimap").
    static constexpr std::array<u64, 8> Vtables{0x1A482D8, 0x1A43048, 0x1A43A40, 0x1A443E0,
                                                0x1A44D80, 0x1A45720, 0x1A46190, 0x1A46EF8};
    static constexpr std::array<u64, 8> GetClassThunks{0x55C338, 0x4F0DA0, 0x4F325C, 0x4F8140,
                                                       0x4F9524, 0x4FCE4C, 0x500154, 0x504780};
    auto& memory = system.ApplicationMemory();
    info = EmmiInfo{};
    if (!AddressIsSane(actor + 0x240, 0x30)) {
        return false;
    }
    const VAddr comp = memory.Read64(actor + 0x240);
    if (comp == 0 || !AddressIsSane(comp, 8)) {
        return false;
    }
    const VAddr vt = memory.Read64(comp);
    if (vt == 0 || !AddressIsSane(vt, 8)) {
        return false;
    }
    info.vt_off = vt - main_region_begin - static_cast<u64>(nce_vtable_delta);
    bool is_emmi = std::ranges::find(Vtables, info.vt_off) != Vtables.end();
    if (!is_emmi) {
        const VAddr slot0 = memory.Read64(vt);
        is_emmi =
            slot0 >= main_region_begin &&
            std::ranges::find(GetClassThunks, slot0 - main_region_begin) != GetClassThunks.end();
    }
    if (!is_emmi || !AddressIsSane(comp, 0x14C40)) {
        return false;
    }
    const VAddr sc = memory.Read64(actor + 0x268);
    if (sc != 0 && AddressIsSane(sc + 0x70, 4)) {
        info.state = static_cast<s32>(memory.Read32(sc + 0x70));
    }
    const VAddr owner = memory.Read64(comp + 0x18);
    info.owner_is_entity = owner == actor;
    // main+0x24FBC8 exactly: component+0x20, then the OWNER's +0x1B4 and +0x1B0/+0x1A8; an
    // unreadable owner is "not enabled", never the entity in its place.
    info.enabled = memory.Read8(comp + 0x20) != 0 && AddressIsSane(owner + 0x1B8, 8) &&
                   memory.Read8(owner + 0x1B4) != 0 &&
                   (memory.Read8(owner + 0x1B0) == 0 || memory.Read64(owner + 0x1A8) != 0);
    info.inside = memory.Read8(comp + 0x14C35) != 0;
    return true;
}

void Reader::RefreshEmmiPos() const {
    // Which unit the marker follows and when: the entity the game's own EMMI bindings act on
    // (EmmiEntityState on the Game's entity list at Game+0x2638; node: entity at +8, next at
    // +0x10), drawn while its AI component is enabled and reports Samus inside its zone -- the
    // game's minimap shows the unit under exactly that condition, and a disabled unit (the
    // Proto after its intro) drops its enabled flag. Among several such units the one whose
    // +0x268 state is 2 or 4 is preferred. The list is walked twice a second; the chosen
    // entity's transform and flags are read every tick.
    auto& memory = system.ApplicationMemory();
    emmi_found = false;
    if (live_scenario.empty() || !AreaHasEmmi(live_scenario)) {
        emmi_actor = 0;
        return;
    }
    const auto active = [](s32 st) { return st == 2 || st == 4; };
    const auto shown = [](const EmmiInfo& i) { return i.enabled && i.inside; };
    if (emmi_actor != 0) {
        EmmiInfo info;
        if (!EmmiEntityState(emmi_actor, info) || !shown(info) || !ReadEmmiCenter(emmi_actor)) {
            Log(EDEN_DSMOD_LOG_INFO,
                "DSMod EMMI: unit {:012X} hidden (state {} enabled {} inside {})", emmi_actor,
                info.state, info.enabled, info.inside);
            emmi_actor = 0;
        }
    }
    if ((tick_count % 30) != 11) {
        return;
    }
    const VAddr G = BlackboardRoot();
    if (G == 0 || !AddressIsSane(G + 0x2638, 8)) {
        return;
    }
    std::string seen, vts;
    VAddr pick = 0, any = 0;
    f32 pick_x{}, pick_y{}, any_x{}, any_y{};
    u32 walked = 0, with_comp = 0;
    bool owner_mismatch = false;
    const bool previous_found = emmi_found;
    const f32 previous_x = emmi_x, previous_y = emmi_y;
    std::unordered_set<VAddr> visited;
    constexpr size_t MaxEntityNodes = 16384;
    VAddr node = memory.Read64(G + 0x2638);
    while (node != 0) {
        if (visited.size() >= MaxEntityNodes || !visited.insert(node).second ||
            !AddressIsSane(node, 0x18)) {
            emmi_found = previous_found;
            emmi_x = previous_x;
            emmi_y = previous_y;
            return; // Incomplete discovery must not replace the cached selection.
        }
        const VAddr actor = memory.Read64(node + 8);
        node = memory.Read64(node + 0x10);
        if (actor == 0 || !AddressIsSane(actor + 0x240, 8)) {
            continue;
        }
        ++walked;
        EmmiInfo info;
        const bool is_emmi = EmmiEntityState(actor, info);
        if (memory.Read64(actor + 0x240) != 0) {
            ++with_comp;
            if (with_comp <= 24) {
                vts += fmt::format(" {:X}", info.vt_off);
            }
        }
        if (!is_emmi) {
            continue;
        }
        owner_mismatch |= !info.owner_is_entity;
        seen += fmt::format(" {:012X}:s{}/e{}/z{}", actor, info.state, info.enabled ? 1 : 0,
                            info.inside ? 1 : 0);
        if (shown(info) && ReadEmmiCenter(actor)) {
            if (any == 0) {
                any = actor;
                any_x = emmi_x;
                any_y = emmi_y;
            }
            if (pick == 0 && active(info.state)) {
                pick = actor;
                pick_x = emmi_x;
                pick_y = emmi_y;
            }
        }
    }
    if (seen != emmi_units_seen || live_scenario != emmi_units_scenario) {
        emmi_units_seen = seen;
        emmi_units_scenario = live_scenario;
        Log(EDEN_DSMOD_LOG_INFO,
            "DSMod EMMI: units in '{}' (entity:state/enabled/inside){}{} -- {} entities walked, {} "
            "with a +240 component; vtables:{}",
            live_scenario, seen.empty() ? " none" : seen,
            owner_mismatch ? " [component+0x18 is not the entity]" : "", walked, with_comp, vts);
    }
    if (pick == 0) {
        pick = any;
        pick_x = any_x;
        pick_y = any_y;
    }
    if (pick != 0 && pick != emmi_actor) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod EMMI: following unit {:012X} at ({:.0f},{:.0f})", pick,
            pick_x, pick_y);
        emmi_actor = pick;
    }
    if (emmi_actor != 0) {
        emmi_found = ReadEmmiCenter(emmi_actor);
    }
}

void Reader::ReadBreakableTiles(const std::string& area) const {
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end()) {
        return;
    }
    const float span_x = ma->second.max_x - ma->second.min_x;
    const float span_y = ma->second.max_y - ma->second.min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return;
    }
    const VAddr data = FindBlackboardProp(area, "MINIMAP_TILES");
    if (data == 0 || !AddressIsSane(data, 0x14)) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    // MINIMAP_TILES = CRntSmallDictionary<CStrId, CRntVector<SMinimapTileState>> (sparse). Dict:
    // A=values@+0x00 (0x28 stride), cap@+0x10. Each value vec: tiles@+0x00, count@+0x08. Each tile
    // (0x10): fX@0, fY@4, eTileType@8, uState@0xc (0=hidden/undiscovered, 1=revealed-permanent).
    const VAddr A = memory.Read64(data + 0x00);
    const u32 cap = memory.Read32(data + 0x10);
    if (cap > 65536 || (cap != 0 && !AddressIsSane(A, static_cast<u64>(cap) * 0x28))) {
        return;
    }
    // Collect the REVEALED tiles as exact world positions: the renderer paints each as its true
    // 100-unit lattice block (matching the raster's quantization), the game's permanent
    // weapon-coloured breakable-block marker. Undiscovered tiles stay invisible (fog seals the
    // passage); big demolition blocks are handled separately by the occluder layer.
    std::vector<WallTile> fresh;
    constexpr u64 MaxTilesPerRead = 262144;
    u64 tiles_remaining = MaxTilesPerRead;
    for (u32 i = 0; i < cap; ++i) {
        const VAddr vec = A + static_cast<VAddr>(i) * 0x28;
        if (!AddressIsSane(vec, 0x0C)) {
            return;
        }
        const VAddr tiles = memory.Read64(vec + 0x00);
        const u32 n = memory.Read32(vec + 0x08);
        if (n == 0) {
            continue;
        }
        if (n > 65536 || n > tiles_remaining || !AddressIsSane(tiles, static_cast<u64>(n) * 0x10)) {
            return; // Preserve the previous complete tile snapshot.
        }
        tiles_remaining -= n;
        for (u32 j = 0; j < n; ++j) {
            const VAddr t = tiles + static_cast<VAddr>(j) * 0x10;
            f32 fx{}, fy{};
            const u32 rx = memory.Read32(t + 0x00), ry = memory.Read32(t + 0x04);
            std::memcpy(&fx, &rx, 4);
            std::memcpy(&fy, &ry, 4);
            const u32 ustate = memory.Read32(t + 0x0C);
            // uState: 0 = hidden/undiscovered (the game draws NOTHING -- showing it would spoil
            // secrets), 1 = revealed (a permanent weapon-coloured square, kept even after the
            // block breaks -- how Dread marks bomb-block tunnels). Draw only revealed tiles.
            if (ustate == 0 || !std::isfinite(fx) ||
                !std::isfinite(fy)) { // main+0xE916A8: drawn iff != 0
                continue;
            }
            const u32 tiletype = memory.Read32(t + 0x08); // EBreakableTileType (weapon)
            const float u = (fx - ma->second.min_x) / span_x;
            const float v = (fy - ma->second.min_y) / span_y;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
                continue;
            }
            fresh.push_back({fx, fy, static_cast<u8>(std::min<u32>(tiletype, 14))});
        }
    }
    std::sort(fresh.begin(), fresh.end(), [](const WallTile& a, const WallTile& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    auto& w = map_walls[area];
    if (w != fresh) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod walls '{}': {} revealed breakable tile(s) (gen {})", area,
            fresh.size(), wall_gen + 1);
        w = std::move(fresh);
        ++wall_gen;
    }
}

void Reader::ReadOccluderDeaths(const std::string& area) {
    // One pass over the current scenario's blackboard section: every destructible actor records
    // its death as "<actor>:BLOCKLIFE:Dead" (demolition blocks -- the occluder owners) or
    // "<actor>:LIFE:Dead" (door shields), and a door left permanently open records
    // "<actor>:DOOR:Opened". Collect the actors the map actually draws (baked occluders + named
    // icons); a change drops concealment polys / shield icons, or swaps a door to its open glyph.
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end()) {
        return;
    }
    std::set<std::string, std::less<>> relevant;
    for (const auto& oc : ma->second.occluders) {
        relevant.insert(oc.name);
        if (!oc.owner.empty())
            relevant.insert(oc.owner);
    }
    for (const auto& mk : ma->second.markers) {
        if (!mk.name.empty()) {
            relevant.insert(mk.name);
        }
    }
    if (relevant.empty()) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    std::set<std::string> dead, opened, picked, unveiled, veiled;
    const bool found = ForEachBlackboardProp(area, [&](const std::string& nm, VAddr vp) {
        std::string_view actor;
        std::set<std::string>* into = nullptr;
        if (nm.ends_with(":BLOCKLIFE:Dead")) {
            actor = std::string_view{nm}.substr(0, nm.size() - 15);
            into = &dead;
        } else if (nm.ends_with(":LIFE:Dead")) {
            actor = std::string_view{nm}.substr(0, nm.size() - 10);
            into = &dead;
        } else if (nm.ends_with(":DOOR:Opened")) {
            actor = std::string_view{nm}.substr(0, nm.size() - 12);
            into = &opened;
        } else if (nm.ends_with(":PICKABLE:PickedUp")) {
            // a collected item: its icon turns into the game's "acquired" glyph and its room
            // stops pulsing
            actor = std::string_view{nm}.substr(0, nm.size() - 18);
            into = &picked;
        } else if (nm.ends_with(":PICKABLE:Unveiled")) {
            // Only items hidden in blocks carry this flag: 0 while the block is still unknown
            // (the map shows nothing), 1 once it was found (the icon appears and the room pulses
            // until the item is taken). Plain-sight items have no flag. Probed on a real save:
            // six at 0, two at 1 (both collected), the game blinked no room.
            actor = std::string_view{nm}.substr(0, nm.size() - 18);
            if (relevant.contains(actor)) {
                if ((memory.Read64(vp) & 0xFF) != 0) {
                    unveiled.emplace(actor);
                } else {
                    veiled.emplace(actor);
                }
            }
            return true;
        } else {
            return true;
        }
        if (relevant.contains(actor) && (memory.Read64(vp) & 0xFF) != 0) {
            into->emplace(actor);
        }
        return true;
    });
    if (!found) {
        return;
    }
    // NAVMESH_OCCLUDERS is TEnabledOccluderCollidersMap. The outer CRntSmallDictionary stores
    // actor CStrIds and 0x28-byte enabled-collider collections in parallel. For a PRESENT actor,
    // an absent collider is disabled and must not conceal the map; an absent actor retains legacy
    // actor-death behavior. Keep the previous collider snapshot if
    // any bound or nested collection is malformed, while actor death flags remain authoritative.
    bool collider_state_valid = false;
    std::map<std::string, std::set<std::string>, std::less<>> enabled_colliders;
    const bool debug_collider_state =
        std::getenv("EDEN_DSMOD_OCCLUDER_DEBUG") != nullptr && !occluder_probe_logged;
    if (const VAddr vp = FindBlackboardProp(area, "NAVMESH_OCCLUDERS");
        vp != 0 && AddressIsSane(vp, 0x14)) {
        const VAddr values = memory.Read64(vp);
        const VAddr keys = memory.Read64(vp + 0x08);
        const u32 count = memory.Read32(vp + 0x10);
        collider_state_valid =
            count <= 4096 && (count == 0 || (AddressIsSane(values, u64{count} * 0x28) &&
                                             AddressIsSane(keys, u64{count} * 8)));
        u32 total_colliders = 0;
        if (debug_collider_state) {
            occluder_probe_logged = true;
            Log(EDEN_DSMOD_LOG_INFO,
                "DSMod occluder probe: area={} vp={:012X} values={:012X} keys={:012X} "
                "actors={} outer_valid={}",
                area, vp, values, keys, count, collider_state_valid);
        }
        for (u32 i = 0; collider_state_valid && i < count; ++i) {
            const std::string actor = ReadCStrIdName(memory.Read64(keys + u64{i} * 8), 96);
            const VAddr colliders = memory.Read64(values + u64{i} * 0x28);
            const u32 collider_count = memory.Read32(values + u64{i} * 0x28 + 0x08);
            if (debug_collider_state) {
                Log(EDEN_DSMOD_LOG_INFO,
                    "DSMod occluder probe: outer[{}] actor={} inner={:012X} count={}", i, actor,
                    colliders, collider_count);
            }
            if (actor.empty() || collider_count > 256 || total_colliders > 8192 - collider_count ||
                (collider_count != 0 && !AddressIsSane(colliders, u64{collider_count} * 8))) {
                collider_state_valid = false;
                break;
            }
            total_colliders += collider_count;
            auto& enabled = enabled_colliders[actor];
            for (u32 j = 0; j < collider_count; ++j) {
                const u64 raw_collider = memory.Read64(colliders + u64{j} * 8);
                // Inner CRntVector<CStrId> elements are raw 64-bit identities, matching bmssv.
                const u64 collider_hash = raw_collider;
                if (debug_collider_state &&
                    (i == 0 || actor.find("PlatformCaveWater_B") != std::string::npos)) {
                    Log(EDEN_DSMOD_LOG_INFO,
                        "DSMod occluder probe: actor={} inner_count={} hash=0x{:016X}", actor,
                        collider_count, collider_hash);
                }
                enabled.insert(fmt::format("0x{:016X}", collider_hash));
            }
        }
    }
    ApplyOccluderColliderState(area, collider_state_valid, enabled_colliders, dead);
    bool changed = false;
    if (auto& cur = map_occ_dead[area]; cur != dead) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod occluders '{}': {} disabled mask/actor state(s) (gen {})",
            area, dead.size(), wall_gen + 1);
        cur = std::move(dead);
        ++wall_gen; // concealment polys are baked into the prefog raster
        changed = true;
    }
    if (auto& cur = map_door_open[area]; cur != opened) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod doors '{}': {} recorded opened", area, opened.size());
        cur = std::move(opened); // markers only: no raster rebuild
        changed = true;
    }
    if (auto& cur = map_item_picked[area]; cur != picked) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod items '{}': {} collected", area, picked.size());
        cur = std::move(picked);
        ++wall_gen; // the item-room pulse companion is baked beside the composite
        changed = true;
    }
    if (auto& cur = map_item_unveiled[area]; cur != unveiled) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod items '{}': {} hidden item(s) found", area,
            unveiled.size());
        cur = std::move(unveiled);
        ++wall_gen;
        changed = true;
    }
    if (auto& cur = map_item_veiled[area]; cur != veiled) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod items '{}': {} hidden item(s) still unknown", area,
            veiled.size());
        cur = std::move(veiled);
        changed = true;
    }
    if (changed) {
        UpdateHiddenMarkers(area);
    }
}

void Reader::ApplyOccluderColliderState(
    const std::string& area, bool state_valid,
    const std::map<std::string, std::set<std::string>, std::less<>>& enabled,
    std::set<std::string>& dead) const {
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end())
        return;
    const auto prior = map_occ_dead.find(area);
    for (const auto& oc : ma->second.occluders) {
        if (oc.owner.empty() || oc.collider.empty())
            continue;
        const auto actor = enabled.find(oc.owner);
        if (dead.contains(oc.owner) ||
            (state_valid && actor != enabled.end() && !actor->second.contains(oc.collider)) ||
            (!state_valid && prior != map_occ_dead.end() && prior->second.contains(oc.name))) {
            dead.insert(oc.name);
        }
    }
}

void Reader::ReadVignettes(const std::string& area) {
    // OCCLUDER_VIGNETTES = blackboard dict<CStrId,bool> in the scenario section: true = the
    // hidden-room vignette was dispelled (the room may appear). Probed layout: bool bytes at
    // data+0x00, CStrId* key array at data+0x08, count u32 at data+0x10.
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end() || ma->second.vignettes.empty()) {
        return;
    }
    const VAddr vd = FindBlackboardProp(area, "OCCLUDER_VIGNETTES");
    if (vd == 0 || !AddressIsSane(vd, 0x14)) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const VAddr vals = memory.Read64(vd + 0x00);
    const VAddr keys = memory.Read64(vd + 0x08);
    const u32 n = memory.Read32(vd + 0x10);
    // Size cap BEFORE the range probes: a garbage count would otherwise walk the page table to
    // the end of the mapped heap.
    if (n > 65536 || (n != 0 && (!AddressIsSane(vals, n) || !AddressIsSane(keys, u64{n} * 8)))) {
        return;
    }
    std::set<std::string> dispelled;
    for (u32 i = 0; i < n; ++i) {
        if (memory.Read8(vals + i) == 0) {
            continue;
        }
        if (std::string nm = ReadCStrIdName(memory.Read64(keys + i * 8), 48); !nm.empty()) {
            dispelled.insert(std::move(nm));
        }
    }
    auto& cur = map_vig_dispelled[area];
    if (cur != dispelled) {
        Log(EDEN_DSMOD_LOG_INFO, "DSMod vignettes '{}': {} dispelled (gen {})", area,
            dispelled.size(), wall_gen + 1);
        cur = std::move(dispelled);
        ++wall_gen;
        UpdateHiddenMarkers(area);
    }
}

void Reader::UpdateHiddenMarkers(const std::string& area) {
    // An icon is hidden while its shield actor is dead, or while the vignette hiding its room is
    // still active (not yet dispelled).
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end()) {
        return;
    }
    const auto& dead = map_occ_dead[area];
    const auto& dispelled = map_vig_dispelled[area];
    const auto& opened = map_door_open[area];
    const auto& picked = map_item_picked[area];
    const auto& unveiled = map_item_unveiled[area];
    const auto& veiled = map_item_veiled[area];
    for (auto& mk : ma->second.markers) {
        mk.hidden = (!mk.name.empty() && dead.contains(mk.name)) ||
                    (!mk.vignette.empty() && !dispelled.contains(mk.vignette));
        mk.opened = !mk.name.empty() && opened.contains(mk.name);
        mk.collected = !mk.name.empty() && picked.contains(mk.name);
        mk.unveiled = !mk.name.empty() && unveiled.contains(mk.name);
        mk.veiled = !mk.name.empty() && veiled.contains(mk.name);
    }
    ++marker_gen; // markers only: the UI signature redraws, the raster is untouched
}

bool Reader::IsVisited(const std::string& area, float wx, float wy) const {
    const auto grid = map_visited.find(area);
    if (grid == map_visited.end()) {
        return !manifest.map_style.reveal_required;
    }
    const auto found = manifest.map_areas.find(area);
    if (found == manifest.map_areas.end()) {
        return true;
    }
    const float span_x = found->second.max_x - found->second.min_x;
    const float span_y = found->second.max_y - found->second.min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return true;
    }
    const float u = (wx - found->second.min_x) / span_x;
    const float v = (wy - found->second.min_y) / span_y;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return false;
    }
    const int col =
        std::clamp(static_cast<int>(u * dread::VisitedGrid::Cols), 0, dread::VisitedGrid::Cols - 1);
    const int row = std::clamp(static_cast<int>((1.0f - v) * dread::VisitedGrid::Rows), 0,
                               dread::VisitedGrid::Rows - 1);
    return grid->second.cells[static_cast<size_t>(row) * dread::VisitedGrid::Cols + col] != 0;
}

void Reader::UpdateHost(const EdenDsmodHostApi& api) {
    host = api;
    tick_count = host.get_tick ? host.get_tick(host.userdata) : tick_count;
    main_region_begin = host.main_base;
    main_region_size = host.main_size;
    nce_vtable_delta = host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    sequence_texts.clear();
    if (host.get_text) {
        if (const char* scenario = host.get_text(host.userdata, "__sequence:scenario")) {
            sequence_texts.emplace("scenario", scenario);
        }
    }
}

void Reader::SampleTracker(dread::StateSnapshot& out) const {
    if (host.get_f64) {
        const double px = host.get_f64(host.userdata, "player_x", HUGE_VAL);
        const double py = host.get_f64(host.userdata, "player_y", HUGE_VAL);
        if (std::isfinite(px) && std::isfinite(py)) {
            out.floats["player_x"] = px;
            out.floats["player_y"] = py;
        }
    }
    // Original SampleState Dread tracker and EMMI block.
    if (grid_cells_base != 0) {
        const VAddr tracker = grid_cells_base - 0x118 + 0xD63D6C;
        if (AddressIsSane(tracker, 0x14)) {
            const u32 rx = system.ApplicationMemory().Read32(tracker + 0x0C);
            const u32 ry = system.ApplicationMemory().Read32(tracker + 0x10);
            f32 px{}, py{};
            std::memcpy(&px, &rx, sizeof(px));
            std::memcpy(&py, &ry, sizeof(py));
            if (std::isfinite(px) && std::isfinite(py) && std::fabs(px) < 1.0e7f &&
                std::fabs(py) < 1.0e7f) {
                const bool plausible = !live_scenario.empty() && IsVisited(live_scenario, px, py);
                if (plausible) {
                    out.floats["player_x"] = px;
                    out.floats["player_y"] = py;
                }
            }
        }
    }
    if (last_in_game != 0 && grid_name_ok) {
        RefreshEmmiPos();
    } else {
        emmi_found = false;
        emmi_actor = 0;
    }
    if (emmi_found && !emmy_defeated) {
        out.floats["emmi_x"] = emmi_x;
        out.floats["emmi_y"] = emmi_y;
    } else {
        out.floats.erase("emmi_x");
        out.floats.erase("emmi_y");
    }
}

static u32 BreakableTileColor(int type) {
    switch (type) {
    case 1:
        return 0xFFFFFF96u;
    case 2:
        return 0xFFFF32FFu;
    case 3:
        return 0xFFFF0000u;
    case 4:
        return 0xFF12FF12u;
    case 5:
        return 0xFFFF7D00u;
    case 6:
        return 0xFF004BFFu;
    case 7:
        return 0xFF323232u;
    case 9:
        return 0xFFFF4B32u;
    default:
        return 0xFF000103u;
    }
}

void Reader::PublishSnapshot(const dread::StateSnapshot& snapshot) {
    for (const auto& [name, value] : snapshot.ints) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name.c_str(), value);
    }
    for (const auto& [name, value] : snapshot.floats) {
        if (host.publish_f64)
            host.publish_f64(host.userdata, name.c_str(), value);
    }
    for (const auto& [name, value] : snapshot.texts) {
        if (host.publish_text)
            host.publish_text(host.userdata, name.c_str(), value.c_str());
    }
}

void Reader::PublishMapState() {
    if (!host.publish_text || live_scenario.empty() ||
        (published_state_area == live_scenario && published_marker_gen == marker_gen)) {
        return;
    }
    const auto json_set = [](const std::set<std::string>& values) {
        Json out = Json::array();
        for (const auto& value : values)
            out.push_back(value);
        return out;
    };
    Json state;
    state["area"] = live_scenario;
    state["dead_actors"] = json_set(map_occ_dead[live_scenario]);
    state["dispelled_regions"] = json_set(map_vig_dispelled[live_scenario]);
    state["open_doors"] = json_set(map_door_open[live_scenario]);
    state["picked_items"] = json_set(map_item_picked[live_scenario]);
    state["unveiled_items"] = json_set(map_item_unveiled[live_scenario]);
    state["veiled_items"] = json_set(map_item_veiled[live_scenario]);
    const std::string encoded = state.dump();
    host.publish_text(host.userdata, "__map_state", encoded.c_str());
    published_marker_gen = marker_gen;
    published_state_area = live_scenario;
}

void Reader::PublishMap(const dread::StateSnapshot& snapshot) {
    if (!host.publish_map || live_scenario.empty())
        return;
    EdenDsmodMapFrame frame{};
    frame.struct_size = sizeof(frame);
    frame.area = live_scenario.c_str();
    const auto vis = map_visited.find(live_scenario);
    if (vis != map_visited.end() &&
        (published_area != live_scenario || published_vis_gen != vis->second.generation)) {
        frame.flags |= EDEN_DSMOD_MAP_UPDATE_VISIBILITY;
        frame.visibility_columns = dread::VisitedGrid::Cols;
        frame.visibility_rows = dread::VisitedGrid::Rows;
        frame.visibility = vis->second.cells.data();
        frame.visibility_count = vis->second.cells.size();
        frame.visibility_previous = vis->second.prev.data();
        frame.visibility_change_ticks = vis->second.change_tick.data();
        published_vis_gen = vis->second.generation;
    }
    std::vector<EdenDsmodMapRect> water;
    if (published_area != live_scenario || published_water_gen != water_gen) {
        water.reserve(water_boxes.size());
        for (const auto& box : water_boxes) {
            water.push_back({box[0], box[1], box[2], box[3], 0, 0});
        }
        frame.flags |= EDEN_DSMOD_MAP_UPDATE_WATER;
        frame.water = water.data();
        frame.water_count = water.size();
        published_water_gen = water_gen;
    }
    std::vector<EdenDsmodMapTile> walls;
    if (published_area != live_scenario || published_wall_gen != wall_gen) {
        if (const auto found = map_walls.find(live_scenario); found != map_walls.end()) {
            walls.reserve(found->second.size());
            for (const auto& tile : found->second) {
                walls.push_back({tile.x, tile.y, tile.type, BreakableTileColor(tile.type)});
            }
        }
        frame.flags |= EDEN_DSMOD_MAP_UPDATE_WALLS;
        frame.walls = walls.data();
        frame.wall_count = walls.size();
        published_wall_gen = wall_gen;
    }
    std::vector<EdenDsmodMapPoint> points;
    points.reserve(snapshot.custom_markers.size());
    for (const auto& marker : snapshot.custom_markers) {
        points.push_back({marker.x, marker.y, static_cast<u32>(marker.color),
                          EDEN_DSMOD_POINT_CUSTOM, nullptr, nullptr});
    }
    frame.flags |= EDEN_DSMOD_MAP_UPDATE_POINTS | EDEN_DSMOD_MAP_UPDATE_ZONE;
    frame.points = points.data();
    frame.point_count = points.size();
    if (map_unlocked)
        frame.flags |= EDEN_DSMOD_MAP_UNLOCKED;
    if (emmy_defeated)
        frame.flags |= EDEN_DSMOD_MAP_ZONE_INACTIVE;
    if (emmy_sealed && !emmy_defeated)
        frame.flags |= EDEN_DSMOD_MAP_ZONE_ALERT;
    host.publish_map(host.userdata, &frame);
    published_area = live_scenario;
}

void Reader::TickDread(dread::StateSnapshot& snapshot) {
    // NCE has no Dynarmic BRK bridge for declarative guest calls. Reapply the two presentation
    // gates from the title module instead; scene transitions recreate both objects, so a modest
    // cadence is enough and avoids a write on every frame.
    if ((tick_count % 30) == 0) {
        if (hud_hidden)
            SetHudHidden(*hud_hidden);
        if (minimap_hidden)
            SetMinimapHidden(*minimap_hidden);
    }
    // Mirror the game's own explored map: once a second, read the live CMinimapGrid (the
    // blackboard's MINIMAP_VISIBILITY is only a save-time snapshot) into the current area's
    // reveal grid, so the bottom screen shows real game progress from the first frame
    // (including a save loaded mid-playthrough).
    if (game_is_dread && !manifest.map_areas.empty()) {
        // Structural minimap-grid finder, driven every tick while unresolved so it resolves in a
        // second or two rather than waiting on the ~2 Hz map read or the NCE delta.
        if (grid_cells_base == 0) {
            if (const auto cb = FindMinimapCells()) {
                grid_cells_base = *cb;
            }
        }
        if ((tick_count % 60) == 7) { // ~1 Hz: a 13 MiB grid read; imperceptible for a map
            const auto sc = snapshot.texts.find("scenario");
            ReadGameVisibility(sc != snapshot.texts.end() ? sc->second : std::string{});
            // Back at the title (or between areas) the game frees its minimap grid: after a
            // few reads without one, drop the scenario so map_ready falls to 0 and the page
            // shows the loading screen instead of the last area's map.
            // The Game drops its CMinimapManager (Game+0x2228 -> null) when no map is live,
            // which is the game's own "no map" state; a transient read failure with the manager
            // still present is not.
            if (vis_grid_missing && !live_scenario.empty()) {
                const VAddr G = BlackboardRoot();
                const bool manager_gone = G != 0 && AddressIsSane(G + 0x2228, 8) &&
                                          system.ApplicationMemory().Read64(G + 0x2228) == 0;
                if (manager_gone) {
                    Log(EDEN_DSMOD_LOG_INFO, "DSMod: minimap manager gone; leaving scenario '{}'",
                        live_scenario);
                    live_scenario.clear();
                }
            }
        }
        // Breakable-block walls: refresh on a slower cadence (they change only when one is broken).
        // Every blackboard reader waits for the game to be in play: on the way back to the title
        // the scenario section is torn down under us, and walking it then is at best garbage.
        // (The guest-call answer never arrives on the handheld, where last_in_game stays unknown,
        // so the live grid's readable name is the gate that actually holds there.)
        const bool in_play = last_in_game != 0 && grid_name_ok;
        if (in_play && !live_scenario.empty() && (tick_count % 6) == 3) {
            // The zone's colour state, from the game's own minimap manager (10 Hz, two bytes):
            // "closed" = the EMMI doors are shut, i.e. a chase (its red zone); "dead" = the
            // unit is defeated (green). The save's MINIMAP:EmmyDead[<scenario>] record (what the
            // manager writes at the kill and reloads on a scenario start) is the 1 Hz fallback
            // for the dead state when the manager's record is not readable.
            std::optional<bool> dead;
            bool closed = false;
            bool closed_is_live = false;
            if (const auto zf = ReadMinimapZoneFlags()) {
                dead = zf->dead;
                closed = zf->closed;
                closed_is_live = true;
                // Tracked independently of "dead" (and independently of emmy_sealed,
                // which is only updated once a dead-verdict is actually committed below) so the
                // invariant a few lines down always has the freshest "doors sealed" reading this
                // tick can produce, even on a tick where dead itself came from the CU fallback.
                zone_doors_closed = zf->closed;
                const bool unlocked = zf->maproom_factor > 0.999f;
                if (unlocked != map_unlocked) {
                    map_unlocked = unlocked;
                    ++emmy_gen; // the fog pass composites differently once the map is unlocked
                    Log(EDEN_DSMOD_LOG_INFO, "DSMod: area map '{}' {} (map-room factor {})",
                        live_scenario, unlocked ? "unlocked" : "locked", zf->maproom_factor);
                }
            }
            if (!dead && (tick_count % 60) == 3) {
                dead = EmmyDefeatedInScenario(live_scenario); // record / CU fallback only
            }
            if (dead) {
                // Invariant: an EMMI cannot be simultaneously "dead" (zone paints green)
                // and "actively chasing" (doors sealed -- the same "closed" byte our own emmi_chase
                // red pulse is driven from). Whichever path produced dead=true -- the live manager
                // read above, the save record, or the CU-state fallback, all three go through this
                // one point -- veto it when the doors are sealed right now, or were the last time
                // we actually had a live reading (zone_doors_closed survives a tick where zf failed
                // and the CU fallback fired instead). This is a deliberate safety net, not a
                // substitute for fixing whichever path is wrong: it makes the failure mode "the
                // zone silently stays grey/red one extra tick" instead of "the zone lies green
                // while the EMMI is visibly hunting you", and it is cheap (one bool compare) and
                // path-independent, so it also covers any future third path that gets this wrong.
                const bool doors_sealed = closed_is_live ? closed : zone_doors_closed;
                if (*dead && doors_sealed) {
                    Log(EDEN_DSMOD_LOG_WARNING,
                        "DSMod EMMI invariant: '{}' verdict dead=true rejected -- doors sealed "
                        "(closed={}, live_this_tick={}) means a chase is active; keeping "
                        "defeated={} sealed={} unchanged",
                        live_scenario, doors_sealed, closed_is_live, emmy_defeated, emmy_sealed);
                } else {
                    const bool sealed = closed && !*dead;
                    if (*dead != emmy_defeated || sealed != emmy_sealed) {
                        if (*dead != emmy_defeated) {
                            ++emmy_gen; // re-rasterise so the EMMI zone recolours grey/green
                        }
                        emmy_defeated = *dead;
                        emmy_sealed = sealed;
                        Log(EDEN_DSMOD_LOG_INFO, "DSMod: EMMI zone '{}' defeated={} closed={}",
                            live_scenario, *dead, closed);
                    }
                }
            }
        }
        // Right away when the scenario (re)solves, then once a second: otherwise the first raster
        // after a load or an area change paints every baked occluder and vignette as still
        // standing for up to a second (a black block over rooms the save already cleared).
        if (in_play && !live_scenario.empty() &&
            (live_scenario != readers_scenario || (tick_count % 60) == 22)) {
            readers_scenario = live_scenario;
            ReadBreakableTiles(live_scenario);
            ReadOccluderDeaths(live_scenario);
            ReadVignettes(live_scenario);
        }
        // Water = the game's own <scenario>:WATER_VOLUMES records (4 Hz), with the live level
        // of a pool that is mid level change (10 Hz while it moves). RefreshWaterBoxes paces
        // itself; between polls this is one compare.
        if (in_play) {
            RefreshWaterBoxes();
        }
        // Publish the scenario the reader settled on so the map widget draws that area (room_bind
        // "scenario"); under NCE the guest-call scenario sequence yields nothing, so this is what
        // keeps the drawn map and the reveal in the same scenario.
        if (!live_scenario.empty()) {
            snapshot.texts["scenario"] = live_scenario;
        }
        // "map_ready" = the drawn area has a reveal grid this run (0 before a game is loaded, and
        // for the moment after an area change until the new grid is read). A page can bind a
        // label's hide_bind to it, and the renderer hides icons through IsVisited meanwhile.
        // ... and only while the game is actually being played: the package's "scenario"
        // sequence (Game.GetScenarioID) yields "" on the title / file select, where the minimap
        // grid stays allocated, and the area id during play.
        const auto sq = sequence_texts.find("scenario");
        const bool game_known = sq != sequence_texts.end();
        const bool in_game = !game_known || !sq->second.empty();
        if (game_known && in_game != (last_in_game != 0)) {
            Log(EDEN_DSMOD_LOG_INFO, "DSMod: {} (scenario '{}')",
                in_game ? "in game" : "out of game", sq->second);
            last_in_game = in_game ? 1 : 0;
        }
        // A cinematic hides the map: the page falls back to its loading screen for its duration.
        {
            std::string cut_name;
            const bool cut = CutscenePlaying(&cut_name);
            if (cut != cutscene_active) {
                cutscene_active = cut;
                Log(EDEN_DSMOD_LOG_INFO, "DSMod: cutscene {}{}", cut ? "started " : "ended",
                    cut ? cut_name : std::string{});
            }
        }
        snapshot.ints["cutscene"] = cutscene_active ? 1 : 0;
        snapshot.ints["map_ready"] = (in_game && !cutscene_active && !live_scenario.empty() &&
                                      map_visited.contains(live_scenario))
                                         ? 1
                                         : 0;
        // "build_match" = a per-build data file was found for the running executable. Without one
        // nothing can be read (no offsets, no hooks), so a page shows a version warning instead
        // of waiting for a map that will never come.
        snapshot.ints["build_match"] = manifest.build_id_file.empty() ? 0 : 1;
        // "emmi_chase" = the zone's EMMI doors are closed (the game's red-zone state); a page
        // shows its chase warnings (pulsing wash and border) on it.
        snapshot.ints["emmi_chase"] = (emmy_sealed && !emmy_defeated && !cutscene_active) ? 1 : 0;
        // "emmi_state" = the current-scenario EMMI's minimap class (0 = roaming/
        // alive, 1 = defeated), published as a generic point so a package's map.areas.<a>.
        // room_categories[].color_bind can read it -- the same emmy_defeated bit the map-zone ABI
        // channel (AcceptModuleMap -> map_zone_inactive) already carries, now also published by
        // name for the runtime's generic colour-by-state primitive (MapLayer/MapRoomCategory
        // color_bind).
        snapshot.ints["emmi_state"] = emmy_defeated ? 1 : 0;
        // "area_index" = the drawn area's position among the manifest's areas sorted by id, -1
        // until the map is ready. A page picks one sprite per area (its name plate) with
        // keep_min == keep_max == index; the -1 keeps every plate off the title screen.
        s64 area_index = -1;
        if (snapshot.ints["map_ready"] == 1) {
            std::vector<std::string> ids;
            ids.reserve(manifest.map_areas.size());
            for (const auto& [id, unused] : manifest.map_areas) {
                ids.push_back(id);
            }
            std::ranges::sort(ids);
            if (const auto it = std::ranges::find(ids, live_scenario); it != ids.end()) {
                area_index = static_cast<s64>(it - ids.begin());
            }
        }
        snapshot.ints["area_index"] = area_index;
        // "items_pct" = the map screen's ITEMS figure for the drawn area: tanks picked up in the
        // scenario over the scenario's tank count (<scenario>:NumTanksPickedUp / ...Max -- the
        // game counts expansions only, abilities do not move it), and "items_total_pct" = the
        // file-select figure (GAME:Completion). The game's own counters, read as they stand.
        if (!live_scenario.empty() &&
            (live_scenario != items_pct_scenario || tick_count - items_pct_tick >= 30)) {
            items_pct_scenario = live_scenario;
            items_pct_tick = tick_count;
            const u64 picked = FindBlackboardProp(live_scenario, "NumTanksPickedUp");
            const u64 total = FindBlackboardProp(live_scenario, "NumTanksPickedUpMax");
            items_pct_cached = (total > 0 && total < 1000 && picked <= total)
                                   ? static_cast<s64>(picked * 100 / total)
                                   : 0;
            items_total_cached =
                static_cast<s64>(std::min<u64>(FindBlackboardProp("GAME", "Completion"), 100));
            // Energy parts towards the next tank (0..3): an inventory float, stored as its bits.
            const u32 shard_bits =
                static_cast<u32>(FindBlackboardProp("PLAYER_INVENTORY", "ITEM_LIFE_SHARDS"));
            float shards = 0.0f;
            std::memcpy(&shards, &shard_bits, sizeof(shards));
            energy_parts_cached = (shards >= 0.0f && shards <= 4.0f) ? static_cast<s64>(shards) : 0;
            // The status screen's fragment icon state, the game's own rule (main+0xB6E2C8): the
            // state is "SamusEnergyFragments_FULL" while the life component's flag +0x31C is set,
            // else "SamusEnergyFragments_<(int)shards>". The flag is written by the shard setter
            // (main+0x11699AC) as ITEM_TOTAL_LIFE_SHARDS == 16.0 -- every energy part collected.
            // Published as 0..4, 5 = FULL.
            const u32 total_bits = static_cast<u32>(
                FindBlackboardProp("PLAYER_INVENTORY", "ITEM_TOTAL_LIFE_SHARDS"));
            float total_shards = 0.0f;
            std::memcpy(&total_shards, &total_bits, sizeof(total_shards));
            const s64 frag_state = total_shards == 16.0f ? 5 : energy_parts_cached;
            if (frag_state != energy_frag_state_cached || !energy_frag_logged) {
                Log(EDEN_DSMOD_LOG_INFO, "DSMod energy parts: {} held, {} collected (state {})",
                    energy_parts_cached, static_cast<s64>(total_shards), frag_state);
                energy_frag_logged = true;
            }
            energy_frag_state_cached = frag_state;
        }
        snapshot.ints["items_pct"] = items_pct_cached;
        snapshot.ints["items_total_pct"] = items_total_cached;
        snapshot.ints["energy_parts"] = energy_parts_cached;
        snapshot.ints["energy_frag_state"] = energy_frag_state_cached;
        // energy/energy_max/missile/missile_max/pbomb/pbomb_max/aeion/aeion_max, read directly
        // from the PLAYER_INVENTORY blackboard section (ITEM_CURRENT_LIFE/ITEM_MAX_LIFE/etc. --
        // confirmed live, exact match to the top-screen HUD and to the package's own "inventory"
        // Lua call-sequence output) instead of the heap vtable-scan array
        // (646761F643AFEBB3.json points.energy/energy_max/...). Root-caused why the scan alone can
        // show a stale value (e.g. 99 after picking up a tank that makes the real max 199): the
        // package's "inventory" sequence -- which calls the game's own
        // GetItemAmount("ITEM_MAX_LIFE") Lua API every 700ms and would otherwise keep the scan's
        // result corrected -- runs over the BRK guest-call bridge, which `guest_bridge_supported =
        // !Settings::IsNceEnabled()` (mod_runtime.cpp) disables completely under NCE ("guest calls,
        // sequences, and spies are disabled under NCE because the BRK bridge is only handled by
        // Dynarmic", the code's own log line). The real device runs NCE, so on-device this
        // correction silently never happens and the scan's own live-vs-static heap ambiguity
        // (SelectLiveInventory's whole reason to exist, mod_state.cpp) is exposed with nothing to
        // catch a wrong pick. The blackboard read below is an ordinary memory read (no BRK, no
        // guest call) -- identical code path under NCE and Dynarmic -- and PLAYER_INVENTORY is the
        // game's single canonical inventory store, not duplicated in multiple live/static heap
        // copies the way the raw C++ array is, so there is no "which copy" question to get wrong.
        // This module's snapshot write runs after the host's own point resolution (RunGameModule is
        // called after the manifest "points" loop, and both write into the same StateSnapshot --
        // mod_state.cpp/mod_module_host.cpp), so publishing here authoritatively overrides the
        // scan's value whenever the blackboard is available and leaves the scan as a harmless
        // earlier-boot-only fallback for the brief window before it is (the scan needs a
        // multi-second sliced heap sweep to first resolve; the blackboard needs only a validated
        // root pointer, so in practice this also resolves sooner, not just more correctly). One
        // walk of PLAYER_INVENTORY (a few dozen props) every 4 ticks (~15 Hz, the usual companion
        // cadence for fast-changing HUD numbers) rather than every tick: cheap either way
        // (BlackboardRoot() is already tick-cached, and PLAYER_INVENTORY has tens, not thousands,
        // of entries), but no reason to pay a full section walk 60 times a second for a bar that a
        // 15 Hz republish already reads as smooth. Self-validating: a value only overwrites its
        // cached slot if it is finite, non-negative, and no larger than a generous upper bound
        // (2000, well past Dread's real maxima) -- an implausible/mid-teardown read is silently
        // ignored rather than shown, and the LAST plausible value keeps publishing (matching this
        // file's own "keep the last-known-good state" convention elsewhere, e.g. the water-box
        // throttle).
        if (tick_count - vitals_tick >= 4) {
            vitals_tick = tick_count;
            f32 nv_energy{-1}, nv_energy_max{-1}, nv_missile{-1}, nv_missile_max{-1}, nv_pbomb{-1},
                nv_pbomb_max{-1}, nv_aeion{-1}, nv_aeion_max{-1};
            const std::unordered_map<std::string_view, f32*> wanted{
                {"ITEM_CURRENT_LIFE", &nv_energy},
                {"ITEM_MAX_LIFE", &nv_energy_max},
                {"ITEM_WEAPON_MISSILE_CURRENT", &nv_missile},
                {"ITEM_WEAPON_MISSILE_MAX", &nv_missile_max},
                {"ITEM_WEAPON_POWER_BOMB_CURRENT", &nv_pbomb},
                {"ITEM_WEAPON_POWER_BOMB_MAX", &nv_pbomb_max},
                {"ITEM_CURRENT_SPECIAL_ENERGY", &nv_aeion},
                {"ITEM_MAX_SPECIAL_ENERGY", &nv_aeion_max},
            };
            int found = 0;
            ForEachBlackboardProp("PLAYER_INVENTORY", [&](const std::string& name, VAddr slot) {
                const auto it = wanted.find(std::string_view{name});
                if (it == wanted.end()) {
                    return true;
                }
                const u32 bits = static_cast<u32>(system.ApplicationMemory().Read64(slot));
                float value = 0.0f;
                std::memcpy(&value, &bits, sizeof(value));
                *it->second = value;
                ++found;
                return found < static_cast<int>(wanted.size());
            });
            const auto plausible = [](f32 v) {
                return std::isfinite(v) && v >= 0.0f && v <= 2000.0f;
            };
            if (plausible(nv_energy) && plausible(nv_energy_max)) {
                if (!vitals_resolved || vitals_energy != nv_energy ||
                    vitals_energy_max != nv_energy_max) {
                    Log(EDEN_DSMOD_LOG_INFO, "DSMod vitals: energy {}/{} (blackboard)",
                        static_cast<s64>(nv_energy), static_cast<s64>(nv_energy_max));
                }
                vitals_energy = nv_energy;
                vitals_energy_max = nv_energy_max;
                vitals_resolved = true;
            }
            if (plausible(nv_missile) && plausible(nv_missile_max)) {
                vitals_missile = nv_missile;
                vitals_missile_max = nv_missile_max;
            }
            if (plausible(nv_pbomb) && plausible(nv_pbomb_max)) {
                vitals_pbomb = nv_pbomb;
                vitals_pbomb_max = nv_pbomb_max;
            }
            if (plausible(nv_aeion) && plausible(nv_aeion_max)) {
                vitals_aeion = nv_aeion;
                vitals_aeion_max = nv_aeion_max;
            }
        }
        if (vitals_resolved) {
            dread::PublishVital(snapshot, "energy", vitals_energy);
            dread::PublishVital(snapshot, "energy_max", vitals_energy_max);
            dread::PublishVital(snapshot, "missile", vitals_missile);
            dread::PublishVital(snapshot, "missile_max", vitals_missile_max);
            dread::PublishVital(snapshot, "pbomb", vitals_pbomb);
            dread::PublishVital(snapshot, "pbomb_max", vitals_pbomb_max);
            dread::PublishVital(snapshot, "aeion", vitals_aeion);
            dread::PublishVital(snapshot, "aeion_max", vitals_aeion_max);
        }
        // The player's own map markers, twice a second (one section walk); a scenario change
        // drops the previous area's markers at once.
        if (in_play &&
            (live_scenario != custom_markers_scenario || tick_count - custom_markers_tick >= 30)) {
            std::vector<dread::StateSnapshot::CustomMarker> markers;
            ReadCustomMarkers(live_scenario, markers);
            if (markers.size() != custom_markers_cached.size()) {
                Log(EDEN_DSMOD_LOG_INFO, "DSMod markers '{}': {} custom marker(s)", live_scenario,
                    markers.size());
            }
            custom_markers_cached = std::move(markers);
            custom_markers_scenario = live_scenario;
            custom_markers_tick = tick_count;
        }
        snapshot.custom_markers = custom_markers_cached;
        // Mission log (newest first) and Samus's abilities, once a second: the pause-menu
        // companion draws them from these while the game's own menu is open.
        if (in_play && (tick_count - mlog_tick >= 60 || mlog_tick == 0)) {
            mlog_tick = tick_count;
            std::vector<std::string> ids;
            ReadMissionLog(ids);
            if (ids.size() != mlog_cached.size()) {
                Log(EDEN_DSMOD_LOG_INFO, "DSMod mission log: {} entries", ids.size());
            }
            mlog_cached = std::move(ids);
            static constexpr std::array<const char*, 26> Powers{"ITEM_WEAPON_CHARGE_BEAM",
                                                                "ITEM_WEAPON_DIFFUSION_BEAM",
                                                                "ITEM_WEAPON_WIDE_BEAM",
                                                                "ITEM_WEAPON_PLASMA_BEAM",
                                                                "ITEM_WEAPON_WAVE_BEAM",
                                                                "ITEM_WEAPON_HYPER_BEAM",
                                                                "ITEM_WEAPON_GRAPPLE_BEAM",
                                                                "ITEM_WEAPON_ICE_MISSILE",
                                                                "ITEM_WEAPON_SUPER_MISSILE",
                                                                "ITEM_MULTILOCKON",
                                                                "ITEM_WEAPON_BOMB",
                                                                "ITEM_WEAPON_LINE_BOMB",
                                                                "ITEM_WEAPON_POWER_BOMB",
                                                                "ITEM_VARIA_SUIT",
                                                                "ITEM_GRAVITY_SUIT",
                                                                "ITEM_HYPER_SUIT",
                                                                "ITEM_MORPH_BALL",
                                                                "ITEM_MAGNET_GLOVE",
                                                                "ITEM_OPTIC_CAMOUFLAGE",
                                                                "ITEM_GHOST_AURA",
                                                                "ITEM_SONAR",
                                                                "ITEM_SPEED_BOOSTER",
                                                                "ITEM_DOUBLE_JUMP",
                                                                "ITEM_SPACE_JUMP",
                                                                "ITEM_SCREW_ATTACK",
                                                                "ITEM_FLOOR_SLIDE"};
            // Decode the inventory section once. The old loop called FindBlackboardProp 26
            // times, repeating every section/key/string walk for each ability.
            static const std::unordered_set<std::string_view> Wanted{Powers.begin(), Powers.end()};
            std::unordered_map<std::string, s64> next_powers;
            next_powers.reserve(Powers.size());
            for (const char* const name : Powers) {
                next_powers.emplace(name, 0);
            }
            const bool powers_complete =
                ForEachBlackboardProp("PLAYER_INVENTORY", [&](const std::string& name, VAddr slot) {
                    if (!Wanted.contains(name)) {
                        return true;
                    }
                    const u32 bits = static_cast<u32>(system.ApplicationMemory().Read64(slot));
                    float value = 0.0f;
                    std::memcpy(&value, &bits, sizeof(value));
                    next_powers[name] = (value >= 1.0f && value < 1000.0f) ? 1 : 0;
                    return true;
                });
            if (powers_complete) {
                powers_cached = std::move(next_powers);
            }
        }
        snapshot.ints["mlog_count"] = static_cast<s64>(mlog_cached.size());
        for (size_t i = 0; i < 10; ++i) {
            const std::string key = "mlog_" + std::to_string(i);
            if (i < mlog_cached.size()) {
                const std::string& id = mlog_cached[mlog_cached.size() - 1 - i];
                const std::string& text = LocalizedText(id);
                snapshot.texts[key] = text.empty() ? id : text;
            }
        }
        for (const auto& [name, have] : powers_cached) {
            snapshot.ints["pw_" + name] = have;
        }
        // "menu_open" = the game's own pause (map) menu is up on the top screen; the bottom
        // page swaps the map for the mission log + abilities while it is. Until the game's
        // flag is wired, EDEN_DSMOD_MENU=1 forces it for layout checks.
        static const bool force_menu = std::getenv("EDEN_DSMOD_MENU") != nullptr;
        {
            // Probed live: of the game object's 0x400 words at +0x2000, only the bytes at
            // +0x231C/+0x231D went 0 -> 1 with the pause map open and back to 0 on close.
            const VAddr G = BlackboardRoot();
            const s64 open = (G != 0 && AddressIsSane(G + 0x231C, 2) &&
                              system.ApplicationMemory().Read8(G + 0x231C) != 0)
                                 ? 1
                                 : 0;
            if (open != menu_open_cached) {
                Log(EDEN_DSMOD_LOG_INFO, "DSMod: pause menu {}", open ? "opened" : "closed");
                menu_open_cached = open;
            }
        }
        snapshot.ints["menu_open"] = force_menu ? 1 : menu_open_cached;
    }
}

void Reader::Sample() {
    if (host.begin_output)
        host.begin_output(host.userdata);
    current.ints.clear();
    current.texts.clear();
    current.floats.clear();
    current.custom_markers.clear();
    SampleTracker(current);
    PublishSnapshot(current);
    if (host.end_output)
        host.end_output(host.userdata);
}

void Reader::Tick() {
    if (map_job && !map_installed && map_job->Done())
        InstallMapJob();
    if (host.begin_output)
        host.begin_output(host.userdata);
    current.ints.clear();
    current.texts.clear();
    current.floats.clear();
    current.custom_markers.clear();
    if (host.get_text) {
        if (const char* scenario = host.get_text(host.userdata, "scenario")) {
            current.texts.emplace("scenario", scenario);
        }
    }
    TickDread(current);
    // The game's own strings the pages show ("loc:<KEY>" bind_text), from its localization table.
    // A string whose every label is gated by a need_bind this module publishes (the pause-menu
    // labels: need_bind "menu_open") is only published while one of those gates is open: a label
    // behind a closed need gate is neither drawn nor tappable (WidgetHidden), and the gate and the
    // text change in the same snapshot, so the page is the same -- without ~40 texts per tick
    // going through the host's snapshot the rest of the time.
    if (map_installed) {
        for (const auto& loc : loc_keys) {
            bool open = loc.gates.empty();
            for (const auto& gate : loc.gates) {
                const auto it = current.ints.find(gate);
                if (it == current.ints.end() || it->second != 0) {
                    open = true; // not ours to judge, or open
                    break;
                }
            }
            if (open)
                current.texts[loc.name] = LocalizedText(loc.key);
        }
    }
    PublishSnapshot(current);
    PublishMap(current);
    PublishMapState();
    if (host.end_output)
        host.end_output(host.userdata);
}

void Reader::InstallMapJob() {
    // Tick thread, once the job is done: its fields are final and load_data only reads the
    // areas_json/blobs members, so moving the parsed areas and the strings out is race-free.
    map_installed = true;
    MapJob& job = *map_job;
    if (!job.ok) {
        Log(EDEN_DSMOD_LOG_ERROR, "DSMod Dread: no map data from the romfs ({}); the map stays empty",
            job.error);
        return;
    }
    manifest.map_areas = std::move(job.areas);
    if (job.localized_ok && !localized_loaded) {
        localized = std::move(job.localized);
        localized_loaded = true;
    }
    Log(EDEN_DSMOD_LOG_INFO, "DSMod Dread: map data installed ({} areas, {} localized strings)",
        manifest.map_areas.size(), localized.size());
}

u32 ParseColor(const Json& value, const char* key, u32 fallback) {
    if (!value.contains(key))
        return fallback;
    const auto& color = value.at(key);
    if (color.is_number_unsigned())
        return color.get<u32>();
    if (!color.is_string())
        return fallback;
    std::string text = color.get<std::string>();
    if (!text.empty() && text.front() == '#')
        text.erase(text.begin());
    try {
        const u32 raw = static_cast<u32>(std::stoul(text, nullptr, 16));
        return text.size() <= 6 ? 0xFF000000u | raw : raw;
    } catch (...) {
        return fallback;
    }
}

// One manifest map.areas object -> the module's areas. Used for the package's inline areas (the
// asset-free package's authored template) and for the areas the map generator rebuilds from romfs.
void ParseMapAreas(const Json& areas, std::map<std::string, dread::MapArea, std::less<>>& out) {
    for (const auto& [name, area] : areas.items()) {
        dread::MapArea entry;
        entry.geo = area.value("geo", std::string{});
        entry.image = area.value("image", std::string{});
        entry.no_pin = area.value("no_pin", false);
        if (area.contains("min") && area.at("min").is_array() && area.at("min").size() == 2) {
            entry.min_x = area.at("min")[0].get<float>();
            entry.min_y = area.at("min")[1].get<float>();
        }
        if (area.contains("max") && area.at("max").is_array() && area.at("max").size() == 2) {
            entry.max_x = area.at("max")[0].get<float>();
            entry.max_y = area.at("max")[1].get<float>();
        }
        if (area.contains("icons") && area.at("icons").is_array()) {
            for (const auto& value : area.at("icons")) {
                dread::MapMarker marker;
                marker.kind = value.value("k", std::string{});
                marker.icon = value.value("i", std::string{});
                marker.x = value.value("x", 0.0f);
                marker.y = value.value("y", 0.0f);
                marker.name = value.value("n", std::string{});
                marker.vignette = value.value("v", std::string{});
                marker.hidden = !marker.vignette.empty();
                if (value.contains("bx") && value.at("bx").is_array() &&
                    value.at("bx").size() == 4) {
                    const auto& box = value.at("bx");
                    marker.bx0 = box[0].get<float>();
                    marker.by0 = box[1].get<float>();
                    marker.bx1 = box[2].get<float>();
                    marker.by1 = box[3].get<float>();
                    marker.has_box = true;
                }
                if (value.contains("pb") && value.at("pb").is_array() &&
                    value.at("pb").size() == 4) {
                    const auto& box = value.at("pb");
                    marker.px0 = box[0].get<float>();
                    marker.py0 = box[1].get<float>();
                    marker.px1 = box[2].get<float>();
                    marker.py1 = box[3].get<float>();
                    marker.has_pulse_box = true;
                }
                if (value.contains("hb") && value.at("hb").is_array() &&
                    value.at("hb").size() == 4) {
                    const auto& box = value.at("hb");
                    marker.hx0 = box[0].get<float>();
                    marker.hy0 = box[1].get<float>();
                    marker.hx1 = box[2].get<float>();
                    marker.hy1 = box[3].get<float>();
                    marker.has_hint_box = true;
                }
                entry.markers.push_back(std::move(marker));
            }
        }
        if (area.contains("layers") && area.at("layers").is_array()) {
            for (const auto& value : area.at("layers")) {
                dread::MapLayer layer;
                layer.geo = value.value("geo", std::string{});
                layer.color = ParseColor(value, "color", 0xFF808080u);
                if (!layer.geo.empty())
                    entry.layers.push_back(std::move(layer));
            }
        }
        // gold_polys/transport_polys/emmy_polys are not parsed: this module never read them. The
        // runtime reshapes the same underlying data into MapArea.room_categories[] instead.
        const auto parse_occluders = [](const Json& values,
                                        std::vector<dread::MapArea::MapOccluder>& output) {
            for (const auto& value : values) {
                dread::MapArea::MapOccluder occluder;
                occluder.name = value.value("n", std::string{});
                occluder.owner = value.value("owner", std::string{});
                occluder.collider = value.value("collider", std::string{});
                if (!value.contains("t") || !value.at("t").is_array())
                    continue;
                for (const auto& triangle : value.at("t")) {
                    if (triangle.is_array() && triangle.size() == 6) {
                        occluder.tris.push_back({triangle[0].get<float>(), triangle[1].get<float>(),
                                                 triangle[2].get<float>(), triangle[3].get<float>(),
                                                 triangle[4].get<float>(),
                                                 triangle[5].get<float>()});
                    }
                }
                if (!occluder.name.empty() && !occluder.tris.empty()) {
                    output.push_back(std::move(occluder));
                }
            }
        };
        if (area.contains("occluders") && area.at("occluders").is_array()) {
            parse_occluders(area.at("occluders"), entry.occluders);
        }
        if (area.contains("vignettes") && area.at("vignettes").is_array()) {
            parse_occluders(area.at("vignettes"), entry.vignettes);
        }
        if (area.contains("water_pools") && area.at("water_pools").is_array()) {
            for (const auto& value : area.at("water_pools")) {
                dread::WaterPoolDef pool;
                pool.name = value.value("n", std::string{});
                const auto& b = value.contains("b") ? value.at("b") : Json{};
                if (pool.name.empty() || !b.is_array() || b.size() != 4)
                    continue;
                pool.box = {b[0].get<float>(), b[1].get<float>(), b[2].get<float>(),
                            b[3].get<float>()};
                if (!(pool.box[2] > pool.box[0] && pool.box[3] > pool.box[1]))
                    continue;
                if (value.contains("lv") && value.at("lv").is_array()) {
                    for (const auto& level : value.at("lv")) {
                        if (level.is_number())
                            pool.levels.push_back(std::clamp(level.get<float>(), 0.0f, 1.0f));
                    }
                }
                entry.water_pools.push_back(std::move(pool));
            }
        }
        if (area.contains("camera_rects") && area.at("camera_rects").is_array()) {
            for (const auto& box : area.at("camera_rects")) {
                if (box.is_array() && box.size() == 4) {
                    entry.camera_rects.push_back({box[0].get<float>(), box[1].get<float>(),
                                                  box[2].get<float>(), box[3].get<float>()});
                }
            }
        }
        out.emplace(name, std::move(entry));
    }
}

void ParseMap(const Json& config, Reader& reader) {
    reader.manifest.build_id_file = config.value("_build_match", false) ? "matched" : "";
    if (!config.contains("map") || !config.at("map").is_object() ||
        !config.at("map").contains("areas") || !config.at("map").at("areas").is_object()) {
        return;
    }
    const auto& map = config.at("map");
    if (map.contains("style") && map.at("style").is_object()) {
        reader.manifest.map_style.reveal_required = map.at("style").value("reveal_required", false);
    }
    ParseMapAreas(map.at("areas"), reader.manifest.map_areas);
}

// The map job's body (its own thread): generate the areas + geometry from romfs, parse the areas
// for the module, load the localization table. Everything the job produces is published under
// its mutex at the end, in one step.
void RunMapJob(MapJob& job, EdenDsmodHostApi api, dread_mapgen::Inputs inputs) {
#if defined(__linux__)
    // below the game's threads, like the P5R prewarm: this is background work
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
    const auto t0 = std::chrono::steady_clock::now();
    const auto ms = [&t0] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    inputs.read = dread_romfs::HostReader(api);
    inputs.stop = &job.stop;
    dread_mapgen::Output out;
    bool ok = false;
    std::string error;
    std::map<std::string, dread::MapArea, std::less<>> areas;
    std::string json;
    try {
        ok = dread_mapgen::Generate(inputs, out);
        error = out.error;
        if (ok) {
            ParseMapAreas(out.areas, areas);
            json = out.areas.dump();
        }
    } catch (const std::exception& e) {
        ok = false;
        error = e.what();
    } catch (...) {
        ok = false;
        error = "exception";
    }
    const double gen_ms = ms();
    std::unordered_map<std::string, std::string> loc;
    bool loc_ok = false;
    if (api.read_romfs) {
        static constexpr const char* Btxt = "romfs:/system/localization/us_english.txt";
        const size_t size = api.read_romfs(api.userdata, Btxt, 0, nullptr, 0);
        if (size > 0 && size <= (16u << 20)) {
            std::vector<u8> bytes(size);
            if (api.read_romfs(api.userdata, Btxt, 0, bytes.data(), size) == size)
                loc_ok = ParseBtxt(bytes, loc);
        }
    }
    size_t blob_bytes = 0;
    for (const auto& [key, blob] : out.blobs)
        blob_bytes += blob.size();
    if (api.log) {
        char line[400];
        if (ok)
            std::snprintf(line, sizeof(line),
                          "DSMod Dread map data: %zu areas + %zu geometry blobs (%.1f KiB) "
                          "generated from romfs in %.1f ms (%.1f MiB read, peak %zu nodes, "
                          "areas JSON %.1f KiB); %zu localized strings, %.1f ms total (worker)",
                          areas.size(), out.blobs.size(), blob_bytes / 1024.0, gen_ms,
                          out.romfs_bytes / 1048576.0, out.peak_nodes, json.size() / 1024.0,
                          loc.size(), ms());
        else
            std::snprintf(line, sizeof(line), "DSMod Dread map data: generation failed: %.300s",
                          error.c_str());
        api.log(api.userdata, ok ? EDEN_DSMOD_LOG_INFO : EDEN_DSMOD_LOG_ERROR, line);
        if (ok) {
            for (const auto& note : out.notes)
                api.log(api.userdata, EDEN_DSMOD_LOG_DEBUG, ("DSMod Dread map data: " + note).c_str());
        }
    }
    {
        std::scoped_lock lock{job.mutex};
        job.ok = ok;
        job.error = std::move(error);
        job.areas_json = std::move(json);
        job.blobs = std::move(out.blobs);
        job.areas = std::move(areas);
        job.localized = std::move(loc);
        job.localized_ok = loc_ok;
        job.done = true;
    }
    job.cv.notify_all();
}

// Asset-free package (map.areas_src == "module:dread:areas"): start the map job. The manifest's
// map.areas is then the authored template (geometry references and styling only); the module
// runs on it until the job's areas replace it (Reader::Tick -> InstallMapJob).
void StartMapJob(const Json& config, Reader& reader, const EdenDsmodHostApi& host) {
    if (!config.contains("map") || !config.at("map").is_object())
        return;
    const auto& map = config.at("map");
    if (map.value("areas_src", std::string{}) != "module:dread:areas" || !map.contains("areas"))
        return;
    if (config.contains("pages") && config.at("pages").is_array()) {
        // Every "loc:<KEY>" label, and the need_binds that gate it: the text is ungated as soon as
        // one label showing it has no need_bind.
        std::map<std::string, size_t> index;
        std::vector<bool> ungated;
        for (const auto& page : config.at("pages")) {
            if (!page.contains("widgets") || !page.at("widgets").is_array())
                continue;
            for (const auto& w : page.at("widgets")) {
                const std::string bind = w.value("bind_text", std::string{});
                if (!bind.starts_with("loc:"))
                    continue;
                auto [it, fresh] = index.emplace(bind, reader.loc_keys.size());
                if (fresh) {
                    reader.loc_keys.push_back({bind.substr(4), bind, {}});
                    ungated.push_back(false);
                }
                auto& loc = reader.loc_keys[it->second];
                const std::string gate = w.value("need_bind", std::string{});
                if (gate.empty())
                    ungated[it->second] = true;
                else if (std::find(loc.gates.begin(), loc.gates.end(), gate) == loc.gates.end())
                    loc.gates.push_back(gate);
            }
        }
        for (size_t i = 0; i < reader.loc_keys.size(); ++i) {
            if (ungated[i])
                reader.loc_keys[i].gates.clear();
        }
    }
    dread_mapgen::Inputs inputs;
    inputs.template_areas = map.at("areas");
    if (map.contains("icons") && map.at("icons").is_object()) {
        for (const auto& [name, unused] : map.at("icons").items())
            inputs.known_icons.insert(name);
    }
    if (map.contains("style") && map.at("style").is_object())
        inputs.raster_px = map.at("style").value("raster_px", 3072);
    auto job = std::make_shared<MapJob>();
    reader.map_job = job;
    try {
        job->thread = std::thread([job, api = host, inputs]() mutable {
            RunMapJob(*job, api, std::move(inputs));
        });
    } catch (...) {
        // no thread available: generate here (Create runs once, at game load)
        RunMapJob(*job, host, std::move(inputs));
    }
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    static constexpr std::string_view Supported = "646761F643AFEBB3";
    return build_id && std::string_view{build_id}.starts_with(Supported) ? EDEN_DSMOD_TRUE
                                                                         : EDEN_DSMOD_FALSE;
}

void ReportCallbackFailure(const EdenDsmodHostApi* host, const char* message,
                           std::atomic_flag& reported) noexcept {
    if (!reported.test_and_set() && host && host->log) {
        try {
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, message);
        } catch (...) {
        }
    }
}

void* Create(const EdenDsmodHostApi* host, const char* config_json) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!dsmod_sdk::HostAbiMatches(host)) {
            return nullptr;
        }
        auto reader = std::make_unique<Reader>(*host);
        if (config_json && *config_json) {
            const Json config = Json::parse(config_json);
            ParseMap(config, *reader);
            if (!reader->manifest.map_areas.empty())
                StartMapJob(config, *reader, *host);
        }
        if (reader->manifest.map_areas.empty())
            return nullptr;
        reader->UpdateHost(*host);
        return reader.release();
    } catch (...) {
        ReportCallbackFailure(host, "DSMod Dread module create callback failed", reported);
        return nullptr;
    }
}

void Destroy(void* instance) {
    try {
        delete static_cast<Reader*>(instance); // ~Reader stops and joins the map job
    } catch (...) {
    }
}

void SampleCallback(void* instance, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!instance || !host)
            return;
        auto& reader = *static_cast<Reader*>(instance);
        reader.UpdateHost(*host);
        reader.Sample();
    } catch (...) {
        ReportCallbackFailure(host, "DSMod Dread module sample callback failed", reported);
    }
}

void TickCallback(void* instance, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (!instance || !host)
            return;
        auto& reader = *static_cast<Reader*>(instance);
        reader.UpdateHost(*host);
        reader.Tick();
    } catch (...) {
        ReportCallbackFailure(host, "DSMod Dread module tick callback failed", reported);
    }
}

void ConfigureExtensions(void*, const EdenDsmodHostExtensions*) {}

// Metroid Dread's own MFNT font-metrics container: a fixed header (line height at 0x1C, glyph
// count at 0x20, first-glyph offset at 0x28) followed by fourteen-byte glyph records running in
// codepoint order from the space (glyph 0 is notdef, glyph 1 is the space -> first_codepoint
// 0x20). The same algorithm as the runtime's in-core ParseMfnt (engine_mercury.cpp), which
// remains as the fallback for modules without the font extension: the format is proprietary to
// this engine, so it lives here, behind the font-decode extension, rather than in the shared
// runtime.
EdenDsmodBool DecodeFont(void*, const uint8_t* bytes, size_t size, void* receiver,
                         EdenDsmodFontSink sink) {
    if (!bytes || !sink || size < 0x60 || std::memcmp(bytes, "MFNT", 4) != 0) {
        return EDEN_DSMOD_FALSE;
    }
    const auto read32 = [&](size_t at) {
        uint32_t v{};
        std::memcpy(&v, bytes + at, sizeof(v));
        return v;
    };
    const uint32_t line_height = read32(0x1C);
    const uint32_t count = read32(0x20);
    const uint32_t first = read32(0x28);
    // The count comes from the file, so bound it against the file rather than trusting it.
    if (line_height == 0 || count == 0 || first + static_cast<uint64_t>(count) * 14 > size) {
        return EDEN_DSMOD_FALSE;
    }
    std::vector<EdenDsmodFontGlyph> glyphs;
    glyphs.reserve(count - 1);
    for (uint32_t i = 1; i < count; ++i) {
        const size_t at = first + static_cast<size_t>(i) * 14;
        EdenDsmodFontGlyph g{};
        std::memcpy(&g.x, bytes + at + 0, 2);
        std::memcpy(&g.y, bytes + at + 2, 2);
        std::memcpy(&g.w, bytes + at + 4, 2);
        std::memcpy(&g.h, bytes + at + 6, 2);
        std::memcpy(&g.bearing_x, bytes + at + 8, 2);
        std::memcpy(&g.bearing_y, bytes + at + 10, 2);
        std::memcpy(&g.advance, bytes + at + 12, 2);
        glyphs.push_back(g);
    }
    if (glyphs.empty()) {
        return EDEN_DSMOD_FALSE; // matches FontMetrics::Valid()'s "no glyphs" rejection
    }
    sink(receiver, line_height, 0x20, glyphs.data(), static_cast<uint32_t>(glyphs.size()));
    return EDEN_DSMOD_TRUE;
}

// "module:dread:areas" (the map.areas JSON) and "module:dread:map/<area>[.<layer>].geo" (the
// packed geometry blobs), from the map job. Any runtime thread; blocks until the job is done.
EdenDsmodBool LoadData(void* instance, const EdenDsmodHostApi*, const char* key, void* receiver,
                       EdenDsmodDataSink sink) {
    try {
        if (!instance || !key || !sink)
            return EDEN_DSMOD_FALSE;
        // map_job is set once in Create and never replaced, so reading it here is race-free
        const std::shared_ptr<MapJob> job = static_cast<Reader*>(instance)->map_job;
        static constexpr std::string_view Prefix = "module:dread:";
        const std::string_view k{key};
        if (!job || !k.starts_with(Prefix) || !job->Wait())
            return EDEN_DSMOD_FALSE;
        const std::string_view name = k.substr(Prefix.size());
        if (name == "areas") {
            sink(receiver, reinterpret_cast<const uint8_t*>(job->areas_json.data()),
                 job->areas_json.size());
            return EDEN_DSMOD_TRUE;
        }
        const auto it = job->blobs.find(std::string{name});
        if (it == job->blobs.end())
            return EDEN_DSMOD_FALSE;
        sink(receiver, it->second.data(), it->second.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

EdenDsmodBool ActionCallback(void* instance, const char* action, int64_t argument) {
    try {
        return instance && static_cast<Reader*>(instance)->HandleAction(action, argument)
                   ? EDEN_DSMOD_TRUE
                   : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,    &ConfigureExtensions,
    &ActionCallback,        nullptr,
};

const EdenDsmodModuleApi ModuleApi{
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    TitleId,
    "Metroid Dread DSMod",
    EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_MAP_OUTPUT | EDEN_DSMOD_CAP_EXTENSIONS,
    &SupportsBuild,
    &Create,
    &Destroy,
    &SampleCallback,
    &TickCallback,
};

const EdenDsmodFontExtensions ModuleFontExtensions{
    EDEN_DSMOD_FONT_EXT_VERSION,
    sizeof(EdenDsmodFontExtensions),
    EDEN_DSMOD_FONT_EXT_HASH,
    &DecodeFont,
};

const EdenDsmodModuleDataExtensions ModuleDataExtensions{
    EDEN_DSMOD_DATA_EXT_VERSION,
    sizeof(EdenDsmodModuleDataExtensions),
    EDEN_DSMOD_DATA_EXT_HASH,
    &LoadData,
};

} // namespace

#ifdef DSMOD_DREAD_TESTING
#include "010093801237C000_testing.h"

uint64_t DreadTestNameHash(const char* name) {
    return name ? DreadNameHash(name) : 0;
}

uint32_t DreadTestColliderMask(const EdenDsmodHostApi& host, bool state_valid, bool owner_present,
                               bool actor_dead, uint32_t prior_mask, uint32_t enabled_mask) {
    Reader reader{host};
    auto& area = reader.manifest.map_areas["test"];
    area.occluders.push_back({"Actor#A", "Actor", "0xA", {}});
    area.occluders.push_back({"Actor#B", "Actor", "0xB", {}});
    if (prior_mask & 2)
        reader.map_occ_dead["test"].insert("Actor#A");
    if (prior_mask & 4)
        reader.map_occ_dead["test"].insert("Actor#B");
    std::set<std::string> dead;
    if (actor_dead)
        dead.insert("Actor");
    std::map<std::string, std::set<std::string>, std::less<>> enabled;
    if (owner_present) {
        if (enabled_mask & 1)
            enabled["Actor"].insert("0xA");
        if (enabled_mask & 2)
            enabled["Actor"].insert("0xB");
        if (enabled_mask == 0)
            enabled["Actor"] = {};
    }
    reader.ApplyOccluderColliderState("test", state_valid, enabled, dead);
    return (dead.contains("Actor") ? 1U : 0U) | (dead.contains("Actor#A") ? 2U : 0U) |
           (dead.contains("Actor#B") ? 4U : 0U);
}

void* DreadTestCreateReader(const EdenDsmodHostApi& host, uint64_t main_base) {
    auto* reader = new Reader{host};
    reader->main_region_begin = main_base;
    return reader;
}

void DreadTestDestroyReader(void* opaque) {
    delete static_cast<Reader*>(opaque);
}

bool DreadTestParseMap(void* opaque, const char* manifest_json) {
    auto& reader = *static_cast<Reader*>(opaque);
    const Json config = Json::parse(manifest_json ? manifest_json : "", nullptr, false);
    if (config.is_discarded())
        return false;
    ParseMap(config, reader);
    return true;
}

void DreadTestSetScenario(void* opaque, const char* scenario) {
    auto& reader = *static_cast<Reader*>(opaque);
    reader.live_scenario = scenario ? scenario : "";
    reader.ResetWater();
}

void DreadTestSetTick(void* opaque, uint64_t tick) {
    auto& reader = *static_cast<Reader*>(opaque);
    reader.tick_count = tick;
    reader.root_cache_tick = tick; // keep a pinned root (DreadTestSetRoot) across ticks
}

uint64_t DreadTestResolveWaterComponent(void* opaque, uint64_t key) {
    return static_cast<Reader*>(opaque)->ResolveWaterComponent(key);
}

uint64_t DreadTestWaterSectionWalks(void* opaque) {
    return static_cast<Reader*>(opaque)->water_section_walks;
}

uint64_t DreadTestWaterGen(void* opaque) {
    return static_cast<Reader*>(opaque)->water_gen;
}

void DreadTestSetRoot(void* opaque, uint64_t root) {
    auto& reader = *static_cast<Reader*>(opaque);
    reader.root_cache = root;
    reader.root_cache_tick = reader.tick_count;
}

void DreadTestRefreshWater(void* opaque) {
    static_cast<Reader*>(opaque)->RefreshWaterBoxes();
}

size_t DreadTestWaterBoxes(void* opaque, float* boxes, size_t capacity) {
    const auto& water = static_cast<Reader*>(opaque)->water_boxes;
    const size_t count = std::min(capacity, water.size());
    for (size_t i = 0; i < count; ++i) {
        std::copy(water[i].begin(), water[i].end(), boxes + i * 4);
    }
    return water.size();
}

long long DreadTestPublishVital(float value, double* published_float) {
    dread::StateSnapshot snapshot;
    snapshot.ints["energy"] = 12345; // what the host's scan point left there this tick
    dread::PublishVital(snapshot, "energy", value);
    if (published_float != nullptr) {
        *published_float = snapshot.floats.at("energy");
    }
    return snapshot.ints.at("energy");
}

int DreadTestEmmyDefeated(void* opaque, const char* scenario) {
    const auto r = static_cast<Reader*>(opaque)->EmmyDefeatedInScenario(scenario ? scenario : "");
    return r.has_value() ? (*r ? 1 : 0) : -1;
}

std::string DreadTestReadCStrIdName(const EdenDsmodHostApi& host, uint64_t keyptr, int max) {
    Reader reader{host};
    return reader.ReadCStrIdName(keyptr, max);
}

bool DreadTestReadAsciiZ(const EdenDsmodHostApi& host, uint64_t address, int max,
                         bool reject_non_printable, std::string* out) {
    Reader reader{host};
    return reader.ReadGuestAsciiZ(address, max, *out, [&](char c) {
        return reject_non_printable && (c < 32 || c >= 127);
    });
}

uint32_t DreadTestRead32(const EdenDsmodHostApi& host, uint64_t address) {
    Reader reader{host};
    return reader.system.ApplicationMemory().Read32(address);
}
#endif

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
// Exported (see the module's export list): LoadFont uses DecodeFont above and the in-core
// ParseMfnt only runs when this returns nullptr or the decode fails.
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(ModuleFontExtensions)
DSMOD_SDK_EXPORT_DATA_EXTENSIONS(ModuleDataExtensions)
