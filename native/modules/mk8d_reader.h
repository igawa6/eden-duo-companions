// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Mario Kart 8 Deluxe game-memory reader, build-parametric:
//   4.0.0 (build 2C336A9BCF79C304, AArch64, 8-byte pointers): pins mk8d_pins.inc
//   3.0.3 (build 6A85262F21B90364, AArch32 ARM, 4-byte pointers; also with the CTGP-DX plugin):
//         pins mk8d_pins303.inc (+ mk8d_pins303_ctgp.inc for the plugin)
// One Profile per build (mk8d_reader.cpp) holds the pins, the global decoders and every offset;
// the published output names are the same for both builds.
//
// Before the reader trusts an offset, it checks the exact instruction words that encode it.
// Globals are decoded from the accessors' own instructions, never hardcoded:
// ADRP+LDR / ADRP+ADD on 4.0.0, `ldr rX,[pc,#imm]` literal + `ldr rX,[pc,rX]` (GOT cell) /
// `add rX,pc,rX` (object) / `vldr sN,[pc,#imm]` on 3.0.3. Reads are bounded and validated; torn
// reads are rejected (context re-checked after the reads, integer blocks read twice); every
// domain fails closed on its own and says why in mk.diag.
//
// Published values (every one verified live against the game's own screens on both builds, or
// by a guest-memory sentinel where the screen cannot show it; the exceptions are noted in place):
//   context  mk.ready mk.scene mk.phase mk.course mk.course_folder mk.cup mk.cc mk.mirror mk.mode
//            mk.submode mk.laps_total mk.time_ms mk.map_key mk.mapfit_ok mk.mapfit_key
//            mk.pict_key mk.cup_key mk.course_name mk.cup_name mk.diag mk.sample_us
//   local    me.ready me.slot me.rank me.lap me.coins me.finished me.emblem_key me.item0 me.item1
//            me.item0_count me.item1_count me.item_roulette me.item1_roulette me.item0_key
//            me.item1_key
//   horn     mk.horn_ok mk.horn_busy mk.horn_count (action "mk.horn_write")
//   racers   racers.count, r{i}.valid rank driver variant lap coins is_me finished map_x map_y
//            map_ok mapf_x mapf_y icon name emblem_key item0 item1 item_state item1_state
//            item0_count item1_count item0_key item1_key; a per-racer field the package never
//            names is not published (p5r_publish filter; EDEN_DSMOD_MK8D_ALL_OUTPUTS=1 disables it)
//   debug    mk.dbg.* only with EDEN_DSMOD_MK8D_DEBUG=1

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "mk8d_assets.h"
#include "mk8d_ids.h"

namespace Mk8dReader {
using namespace dsmod_sdk::int_types;

/// One build-pinned code site: main offset + the exact instruction words expected there.
struct Pin {
    u64 offset;
    std::span<const u32> words;
    const char* name;
};

inline constexpr int MaxRacers = 12;

/// Supported builds: the main build id (16 bytes, the remaining 16 zero). SupportsBuildHex takes
/// the 64-character hex form supports_build() receives; SupportsBuildId the 32 raw bytes.
bool SupportsBuildHex(const char* hex);
bool SupportsBuildId(const std::uint8_t* build_id32);
struct Profile;
struct GlobalSpec;
/// Scene kind enum (rodata 0xf0b83e "Invalid, Boot, Root, Menu, Race, Select, Award, Theater,
/// TheaterMenu, Ending").
inline constexpr u32 SceneRace = 4;

struct ItemSlot {
    bool ok{};
    int state{};  ///< u8 S+0x41 (0 empty, 1..2 roulette, 3 held)
    int item{-1}; ///< u32 S+0xc8 while state != 0
    int count{};  ///< u32 S+0xa0 while held
};

struct Racer {
    bool valid{};
    int rank{};      ///< 1-based (u16 CK+0x40 + 1)
    int lap{};       ///< as the HUD shows it: min(u32 CK+0x44 + 1, laps_total)
    int lap_raw{};   ///< u32 CK+0x44
    int coins{};     ///< u8 CK+0x50
    bool finished{}; ///< goal bit 0 of u32 CK+0x3c
    int driver{-1};  ///< u32 RI+0x3c+0x1c*i
    int variant{};   ///< u8 RI+0x40+0x1c*i
    bool pos_ok{};
    float x{}, y{}, z{};
    bool map_ok{};
    float map_x{}, map_y{};
    bool fit_ok{};
    float fit_x{}, fit_y{};          ///< map_x/y in the MapFitBox (mk8d_assets.h)
    bool items_ok{};                 ///< owner[i] resolved (owner+0x40 == i)
    std::array<ItemSlot, 2> items{}; ///< slots [owner+0x60], [owner+0x68]
};

struct Snapshot {
    bool ready{};  ///< root, active scene and race info resolved
    u32 scene{};   ///< scene kind (0 when no active scene)
    int phase{-1}; ///< u32 SEQ+0x38 in a race scene, else -1
    int course{-1};
    bool ct_course{}; ///< course comes from the CTGP-DX plugin's table (raw > 0x7a)
    int cup{-1};
    int course_msg{-1};
    int cc{};
    int mirror{};
    int mode{-1};
    int submode{-1};
    int laps_total{};
    s64 time_ms{-1};
    bool racers_ok{};
    int count{};
    int me{-1};
    std::array<Racer, MaxRacers> racers{};
    bool items_ok{};
    std::array<ItemSlot, 2> items{};
    std::string diag;
    /// Debug addresses (published only with EDEN_DSMOD_MK8D_DEBUG=1).
    u64 dbg_ri{}, dbg_rd{}, dbg_ck_me{}, dbg_s0{}, dbg_s1{}, dbg_move_me{};
};

/// Name tables read once from the game's UI resource object (R3) and rodata.
struct Tables {
    bool ok{};
    u64 r3{};
    std::vector<std::string> driver_names;  ///< R3+0x298 +0x80
    std::vector<std::int32_t> driver_msgs;  ///< R3+0x298 +0x90
    std::vector<std::string> pict_names;    ///< R3+0x350 +0x80
    std::vector<std::string> course_tokens; ///< rodata course enum text
    std::vector<std::string> cup_names;     ///< .data cup icon name pointer table
    std::vector<std::int32_t> cup_msgs;     ///< rodata cup -> Common.msbt label (main+0x41fc00)
    std::array<u8, Mk8dIds::ItemCount> item_frames{};
    float y_scale_0x44{};
};

class Reader {
public:
    Reader(const EdenDsmodHostApi& host, Mk8dAssets::Library& assets, const char* config_json);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    void Sample(const EdenDsmodHostApi& host);
    /// Write extension; without it the horn action is unavailable.
    void SetWriteApi(const EdenDsmodHostWriteApi& api);
    /// Module actions: "mk.horn_write" (see HornStep). Returns true when accepted.
    bool OnAction(const char* action, std::int64_t argument);
    /// Best-effort restore of any guest write still applied (called before destroy).
    void Shutdown();

    // Test / diagnostics access.
    const Snapshot& Current() const {
        return current;
    }
    const Tables& NameTables() const {
        return tables;
    }
    bool CodeOk() const {
        return code_ok;
    }

private:
    struct Names;
    bool ReadBlock(u64 at, void* out, std::size_t size) const;
    template <typename T>
    std::optional<T> Read(u64 at) const {
        T v{};
        if (!ReadBlock(at, &v, sizeof(T)))
            return std::nullopt;
        return v;
    }
    std::optional<u64> Ptr(u64 at) const;
    /// Pointer-width word (u64 on 4.0.0, u32 on 3.0.3), no null / alignment test.
    std::optional<u64> Word(u64 at) const;
    u64 WordIn(const u8* bytes) const;
    u64 Global(const GlobalSpec& spec) const;
    bool InMain(u64 at, u64 n) const;
    bool CheckPin(const Pin& pin, bool* windowed = nullptr) const;
    bool ResolveCode();
    /// 3.0.3 + CTGP-DX: find the plugin from its own hook at main+0x451f38, check its code pins
    /// (mk8d_pins303_ctgp.inc) and decode its context object. Absent plugin = not an error.
    void ResolvePlugin();
    struct CtEntry {
        bool ok{};
        int raw{-1};
        std::string folder, pict, cup_icon;
        int name_label{-1}, cup{-1}, cup_label{-1};
    };
    /// The plugin's course-table entry for course raw (index raw + 1) and its cup.
    CtEntry ReadCtEntry(int raw) const;
    bool ct_ok{};
    u64 ct_base{}, ct_ctx{};
    std::string ct_diag;
    CtEntry ct_entry; ///< cached for ct_entry.raw
    std::future<std::shared_ptr<const Mk8dAssets::MessageTable>> ctgp_text_job;
    std::shared_ptr<const Mk8dAssets::MessageTable> ctgp_text;
    bool ctgp_text_started{};
    /// Message label -> text as the game resolves it (first message file that has the label;
    /// the USen labels are unique across Common and CTGPText).
    const std::string* LabelText(int label) const;
    bool ResolveTables(u64 r3);
    std::optional<std::string> ReadCString(u64 at, std::size_t max) const;
    Snapshot ReadSnapshot();
    void ReadRacers(Snapshot& s, u64 oe, const std::vector<u8>& ri_block, u64 rd);
    void ReadItems(Snapshot& s, u64 oe);
    int ReadLocalSlot(u64 root, int count, std::string& why) const;
    void Project(Snapshot& s);
    void Publish();
    void LoadCourseAssets(int course);

    EdenDsmodHostApi host{};
    const Profile* prof{}; ///< selected from the build id in the constructor; null = unsupported
    Mk8dAssets::Library& assets;
    std::unique_ptr<Names> names;

    bool code_resolved{};
    bool code_ok{};
    bool racers_code_ok{}, items_code_ok{}, tables_code_ok{};
    u32 pin_scene{~0u};
    bool pin_recheck{};
    int ctgp_windows{}; ///< pins checked through a CTGP-DX patch window (3.0.3)
    std::string code_diag;
    u64 base{}, size{};
    u64 root_cell{}; ///< GOT slot (main) holding &root variable
    u64 ui_cell{};   ///< GOT slot (main) holding &ui variable
    u64 tokens_at{}, frames_at{}, cups_at{}, yscale_at{}, cup_msgs_at{};
    u64 param_db_cell{}, string_pool_cell{}; ///< GOT slots: driver param DB, string pool
    std::string EmblemCode(int driver, int variant) const;

    Tables tables;
    std::string tables_capped; ///< name tables whose count exceeded the allocated capacity
    std::string tables_diag;   ///< why the last ResolveTables failed (published in mk.diag)
    u64 tables_retry_tick{};

    // Per-course assets (loaded on course change, on the tick thread; cached by the Library).
    int assets_course{-2};
    std::string course_folder;
    std::shared_ptr<const Mk8dAssets::MapCamera> camera;
    /// Map camera loads run off the tick thread (romfs read); taken when ready.
    struct CourseMap {
        std::shared_ptr<const Mk8dAssets::MapCamera> camera;
        std::optional<Mk8dAssets::MapFit> fit;
    };
    std::future<CourseMap> camera_job;
    std::optional<Mk8dAssets::MapFit> map_fit;
    std::string map_fit_key;
    std::string camera_folder;     ///< course folder of camera / map_fit
    std::string camera_job_folder; ///< course folder of camera_job
    /// One-time romfs loads (Common.msbt, the ItemPtn animation) on a worker thread.
    struct Preload {
        std::shared_ptr<const Mk8dAssets::MessageTable> common;
        std::optional<Mk8dIds::ItemPattern> pattern;
    };
    std::future<Preload> preload_job;
    bool preload_started{}, preload_done{};
    void PollPreload();
    std::shared_ptr<const Mk8dAssets::MessageTable> common;
    std::optional<Mk8dIds::ItemPattern> item_pattern;
    std::string course_name, map_key, pict_key, cup_key;
    std::array<std::string, MaxRacers> racer_icon, racer_name;
    std::array<int, MaxRacers> racer_icon_driver{}, racer_icon_variant{};
    std::array<std::string, MaxRacers> racer_emblem;
    /// [item][uses_left 0..3] -> key, for assets_course (0 and 3 = the table frame)
    std::array<std::array<std::string, 4>, Mk8dIds::ItemCount> item_keys;
    bool item_keys_ready{};
    void BuildItemKeys();
    const std::string& ItemKeyOf(const ItemSlot& slot) const;

    Snapshot current;
    u64 sample_count{};
    u64 last_read_tick{};
    bool have_read{};
    // Cost accounting (microseconds, tick thread).
    double cost_sum_us{};
    double cost_max_us{};
    u64 cost_n{};
    double cost_avg_published{};
    u64 cost_windows{};
    bool want_map{true}, want_names{true};
    bool debug{};
    // Horn (HornStep): one-shot request byte (4.0.0 obj+0x35d, 3.0.3 obj+0x321).
    bool horn_code_ok{};
    EdenDsmodHostWriteApi write_api{};
    bool have_write{};
    bool horn_request{};
    bool horn_active{};
    u64 horn_mask_at{}; ///< address of the request byte
    u64 horn_restore_tick{};
    int horn_count{};
    u64 HornMaskAddress(std::string& why) const;
    /// Pins + write extension + racing (phase 6) with a valid local racer: mk.horn_ok.
    bool HornAvailable() const;
    void HornStep(u64 tick);
    u32 dbg_last_scene{~0u};
    int dbg_last_phase{-2};
    bool dbg_last_ready{};
    std::array<u64, MaxRacers> ck_debug{}, move_debug{}, ku_debug{};
    void DebugPaneCompare();
    double dbg_pane_worst{-1};
    int dbg_pane_n{};
    int dbg_pane_flag{-1};
};

} // namespace Mk8dReader
