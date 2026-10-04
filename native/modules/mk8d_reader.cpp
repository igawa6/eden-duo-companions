// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Mario Kart 8 Deluxe game-memory reader (mk8d_reader.h), one Profile per supported build.
// Every chain was derived from the build's code (the pins cite it) and verified live on both
// builds: sentinel pokes seen on the game's HUD, the results screens, and the game's own
// minimap icon panes compared numerically with the projection.

#include "mk8d_reader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "mk8d_assets.h"
#include "p5r_publish_filter.h"

namespace Mk8dReader {

/// How a global is decoded from its pinned accessor instructions.
enum class Decode : u8 {
    None,
    AdrpLdrX, ///< A64 ADRP + LDR Xt,[Xn,#imm]: address of the loaded 8-byte cell (GOT slot)
    AdrpLdrS, ///< A64 ADRP + LDR St,[Xn,#imm]: address of the loaded float
    AdrpAdd,  ///< A64 ADRP + ADD Xd,Xn,#imm: the object's address
    A32Got,   ///< A32 `ldr rT,[pc,#imm]` (literal L) + `ldr rT,[pc,rT]`: GOT cell at use+8+L
    A32Add,   ///< A32 `ldr rT,[pc,#imm]` (literal L) + `add rT,pc,rT`: object at use+8+L
    A32Vldr,  ///< A32 `vldr sN,[pc,#imm]`: address of the loaded float
};
struct GlobalSpec {
    const Pin* pin{};
    u64 a{}; ///< ADRP / literal ldr / vldr address
    u64 b{}; ///< LDR / ADD / use address
    Decode kind{Decode::None};
};

/// Everything build-specific. Each offset is the one the build's pins encode (the comments in
/// the 4.0.0 profile cite the pins; the 3.0.3 values come from the 3.0.3 pins likewise).
struct Profile {
    const char* version;
    std::array<u8, 16> build;
    u64 ptr; ///< pointer width in guest memory
    std::span<const Pin* const> ctx_pins, racer_pins, item_pins, table_pins, horn_pins;
    /// Code ranges a known companion plugin (CTGP-DX 1.1.1 on 3.0.3) rewrites with a jump.
    std::span<const u64> patch_windows;
    bool ctgp_plugin; ///< look for the CTGP-DX 1.1.1 plugin's course table (3.0.3 only)
    GlobalSpec root, yscale, ui, tokens, cups, cup_msgs, param_db, string_pool, frames;
    // Framework root and race info (RaceInfoHolder, RaceInfo, ActiveScene, SceneKind).
    u64 root_scene_holder, scene_holder_scene, scene_flags, scene_kind;
    u64 root_engine, engine_raceinfo_holder, holder_raceinfo;
    // Object engine and directors (Phase, KartDirector, ItemDirector).
    u64 scene_object_engine, oe_rd, oe_kd, oe_id;
    // Race director (Phase, LapsTotal, Checker*, Timer*).
    u64 rd_sequence, rd_lap_manager, rd_checker_count, rd_checkers, rd_timer;
    u64 seq_phase, lm_laps, tm_time;
    u64 ck_flags, ck_rank, ck_lap, ck_coins;
    // Kart director (KartDirector, KartUnit, KartUnitPlayer, KartUnitMove, KartUnitPos).
    u64 kd_units, kd_count, ku_vehicle, ku_move, vehicle_player, move_position;
    // Local player (SystemEngine, PlayerManager, LocalPlayerObj, RacerPlayerObj).
    u64 engine_system, se_player_manager, pm_local_obj, pm_racer_obj;
    // Items (ItemDirector, ItemOwnerCtor, ItemSlotItem, ItemSlotState, ItemSlotCount).
    u64 id_owner_count, id_owners, io_slots, owner_index;
    u64 slot_block, slot_state, slot_count, slot_item, slot_block_end;
    // UI resource (UiRoot, UiResource, DriverTableInit, DriverName, DriverMsg, CoursePict,
    // CupOfCourse).
    u64 ui_object, ui_resource3, r3_cup_table, r3_driver_table, r3_pict_table;
    u64 table_name_count, table_names, table_msg_count, table_msgs;
    /// Name-array capacity the game allocates (0 = trust the count): a larger count is read
    /// up to this capacity only (3.0.3 + CTGP-DX raises the picture count to 187 while the
    /// array keeps its 124 slots; CT pictures come from the plugin's own table).
    u64 driver_capacity, pict_capacity;
    // Driver param DB (DriverParamDb, DriverParamRecord, EmblemCode).
    u64 db_root, db_present, db_records, db_count, db_code;
    bool db_code_is_ptr; ///< 3.0.3: rec+0x10 is a char*; 4.0.0: a string-pool offset
    // Horn (PlayerInput, HornEvent).
    u64 se_inputs, inputs_count, inputs_array, input_entry_obj, input_horn_request;
    // Debug-only map-part layout (DebugPaneCompare); 0 = not available for this build.
    u64 map_icon_list, map_tv_flag, icon_pid, icon_ku, icon_part, part_pane, pane_translate;
};

namespace {

namespace v400 {
#include "mk8d_pins.inc"
} // namespace v400
namespace v303 {
#include "mk8d_pins303.inc"
} // namespace v303

// ---- 4.0.0 ------------------------------------------------------------------------------------
namespace p400 {
using namespace v400;
constexpr std::array<const Pin*, 17> ContextPins{
    &PinRaceInfoHolder, &PinRaceInfo,        &PinActiveScene, &PinSceneKind,    &PinPhase,
    &PinEngineClass,    &PinMirror,          &PinMapYScale,   &PinMapYScaleMul, &PinRaceCount,
    &PinRaceRule,       &PinLapsTotal,       &PinTimer,       &PinTimerMin,     &PinTimerSec,
    &PinTimerMs,        &PinCourseTokenIndex};
constexpr std::array<const Pin*, 18> RacerPins{
    &PinCheckerGoalCheck, &PinCheckerGoalSet, &PinCheckerGoalDone, &PinCheckers,
    &PinCheckerRank,      &PinCheckerCoins,   &PinCheckerLap,      &PinDriverEntry,
    &PinDriverVariant,    &PinKartDirector,   &PinKartUnit,        &PinKartUnitPlayer,
    &PinKartUnitPos,      &PinKartUnitMove,   &PinSystemEngine,    &PinPlayerManager,
    &PinLocalPlayerObj,   &PinRacerPlayerObj};
constexpr std::array<const Pin*, 11> ItemPins{
    &PinItemTripleFrame, &PinItemOwnerCreate, &PinItemOwnerCtor, &PinItemDirector,
    &PinItemSlotItem,    &PinItemSlotState,   &PinItemSlotEmpty, &PinItemSlotCount,
    &PinItemFrame,       &PinCoinKind,        &PinAnimalCourse};
constexpr std::array<const Pin*, 17> TablePins{
    &PinDriverParamDb, &PinDriverParamRecord, &PinEmblemFlag,      &PinEmblemCode,
    &PinEmblemName,    &PinEmblemSuffix,      &PinCupMsg,          &PinUiRoot,
    &PinUiResource,    &PinUiTypeCheck,       &PinDriverTableInit, &PinDriverName,
    &PinDriverMsg,     &PinDriverSuffix,      &PinCoursePict,      &PinCupOfCourse,
    &PinCourseTokens};
constexpr std::array<const Pin*, 8> HornPins{&PinPlayerInput, &PinHornEvent,   &PinHornFill,
                                             &PinHornBit,     &PinHornRequest, &PinHornLatch,
                                             &PinHornPlay,    &PinSystemEngine};

constexpr Profile Build{
    .version = "4.0.0",
    .build = {0x2C, 0x33, 0x6A, 0x9B, 0xCF, 0x79, 0xC3, 0x04, 0x0C, 0xE5, 0x06, 0xCD, 0xD3, 0x91,
              0xB5, 0x78},
    .ptr = 8,
    .ctx_pins = ContextPins,
    .racer_pins = RacerPins,
    .item_pins = ItemPins,
    .table_pins = TablePins,
    .horn_pins = HornPins,
    .patch_windows = {},
    .ctgp_plugin = false,
    .root = {&PinRaceInfoHolder, 0x7f7a90, 0x7f7a94, Decode::AdrpLdrX},
    .yscale = {&PinMapYScaleMul, 0x508ac4, 0x508ac8, Decode::AdrpLdrS},
    .ui = {&PinUiRoot, 0x8c43a4, 0x8c43a8, Decode::AdrpLdrX},
    .tokens = {&PinCourseTokens, 0x1b54c, 0x1b550, Decode::AdrpAdd},
    .cups = {&PinCupOfCourse, 0x50d194, 0x50d19c, Decode::AdrpAdd},
    .cup_msgs = {&PinCupMsg, 0x41fc18, 0x41fc1c, Decode::AdrpAdd},
    .param_db = {&PinDriverParamDb, 0x86ba7c, 0x86ba80, Decode::AdrpLdrX},
    .string_pool = {&PinEmblemCode, 0x86ce40, 0x86ce44, Decode::AdrpLdrX},
    .frames = {&PinItemFrame, 0x4234a8, 0x4234ac, Decode::AdrpAdd},
    .root_scene_holder = 0x20,
    .scene_holder_scene = 0x08,
    .scene_flags = 0x180,
    .scene_kind = 0x184,
    .root_engine = 0x28,
    .engine_raceinfo_holder = 0x240,
    .holder_raceinfo = 0x10,
    .scene_object_engine = 0x190 + 8 * 4,
    .oe_rd = 0x218,
    .oe_kd = 0x238,
    .oe_id = 0x240,
    .rd_sequence = 0x50,
    .rd_lap_manager = 0x58,
    .rd_checker_count = 0x60,
    .rd_checkers = 0x68,
    .rd_timer = 0xe0,
    .seq_phase = 0x38,
    .lm_laps = 0x64,
    .tm_time = 0x40,
    .ck_flags = 0x3c,
    .ck_rank = 0x40,
    .ck_lap = 0x44,
    .ck_coins = 0x50,
    .kd_units = 0x50,
    .kd_count = 0xc0,
    .ku_vehicle = 0x48,
    .ku_move = 0x50,
    .vehicle_player = 0xa8,
    .move_position = 0x08 + 0x24,
    .engine_system = 0x190,
    .se_player_manager = 0x800,
    .pm_local_obj = 0x17c,
    .pm_racer_obj = 0x1cc,
    .id_owner_count = 0xe0,
    .id_owners = 0xe8,
    .io_slots = 0x60,
    .owner_index = 0x40,
    .slot_block = 0x40,
    .slot_state = 0x41,
    .slot_count = 0xa0,
    .slot_item = 0xc8,
    .slot_block_end = 0xcc,
    .ui_object = 0x08,
    .ui_resource3 = 0x18 + 8 * 3,
    .r3_cup_table = 0x30,
    .r3_driver_table = 0x298,
    .r3_pict_table = 0x350,
    .table_name_count = 0x78,
    .table_names = 0x80,
    .table_msg_count = 0x88,
    .table_msgs = 0x90,
    .driver_capacity = 0,
    .pict_capacity = 0,
    .db_root = 0xe8,
    .db_present = 0x20,
    .db_records = 0x30,
    .db_count = 0xa2,
    .db_code = 0x10,
    .db_code_is_ptr = false,
    .se_inputs = 0x7f0,
    .inputs_count = 0x50,
    .inputs_array = 0x58,
    .input_entry_obj = 0x38,
    .input_horn_request = 0x35d,
    .map_icon_list = 0xc0,
    .map_tv_flag = 0x449,
    .icon_pid = 0xb4,
    .icon_ku = 0,
    .icon_part = 0x40,
    .part_pane = 0x18,
    .pane_translate = 0x30,
};
} // namespace p400

// ---- 3.0.3 ----------------------------------------------------------------------------------
namespace p303 {
using namespace v303;
constexpr std::array<const Pin*, 20> ContextPins{
    &PinEngineHolder,   &PinRaceInfoHolder, &PinRaceInfo,  &PinActiveScene, &PinSceneKind,
    &PinPhase,          &PinEngineClass,    &PinMirror,    &PinMapYScale,   &PinMapYScaleMul,
    &PinMapYScaleConst, &PinMapProjection,  &PinRaceCount, &PinRaceRule,    &PinLapsTotal,
    &PinTimer,          &PinTimerMin,       &PinTimerSec,  &PinTimerMs,     &PinCourseTokenIndex};
constexpr std::array<const Pin*, 20> RacerPins{
    &PinCheckerGoalCheck, &PinCheckerGoalSet, &PinCheckerGoalDone,   &PinCheckers,
    &PinCheckerRank,      &PinCheckerCoins,   &PinCheckerLap,        &PinDriverEntryBase,
    &PinDriverEntry,      &PinDriverVariant,  &PinDriverEntryStride, &PinKartDirector,
    &PinKartUnit,         &PinKartUnitPlayer, &PinKartUnitPos,       &PinKartUnitMove,
    &PinSystemEngine,     &PinPlayerManager,  &PinLocalPlayerObj,    &PinRacerPlayerObj};
constexpr std::array<const Pin*, 12> ItemPins{
    &PinItemTripleFrame, &PinItemOwnerCreate, &PinItemOwnerCtor, &PinItemDirector,
    &PinItemSlotItem,    &PinItemSlotState,   &PinItemSlotEmpty, &PinItemSlotCount,
    &PinItemSlotLayout,  &PinItemFrame,       &PinCoinKind,      &PinAnimalCourse};
constexpr std::array<const Pin*, 23> TablePins{
    &PinDriverParamDb,    &PinDriverParamDbLit, &PinDriverParamRecord, &PinEmblemFlag,
    &PinEmblemCode,       &PinEmblemName,       &PinEmblemSuffix,      &PinCupMsg,
    &PinUiRoot,           &PinUiResource,       &PinUiTypeCheck,       &PinDriverTableInit,
    &PinDriverTableAlloc, &PinDriverName,       &PinDriverMsg,         &PinDriverSuffix,
    &PinCoursePict,       &PinPictTableAlloc,   &PinCupOfCourse,       &PinCupNameTableLit,
    &PinCupTableInit,     &PinCourseTokens,     &PinCourseTokensLit};
constexpr std::array<const Pin*, 8> HornPins{&PinPlayerInput, &PinHornEvent,   &PinHornFill,
                                             &PinHornBit,     &PinHornRequest, &PinHornLatch,
                                             &PinHornPlay,    &PinSystemEngine};
/// CTGP-DX 1.1.1 (hook sites from the plugin's own hook calls)
/// patches main with an 8-byte ARM jump at these sites, which overlap six pins (CheckerCoins,
/// CupMsg, CoursePict, CupOfCourse, CoinKind, HornBit). None of the overlapped words is one the
/// reader decodes. Within a window the words must be either the vanilla ones or exactly the
/// jump (JumpWord below + a target outside main); every other word of the pin stays exact.
constexpr std::array<u64, 6> PatchWindows{0x55d32c, 0x451f38, 0x44b040,
                                          0x557194, 0x906df8, 0x18cd6c};

constexpr Profile Build{
    .version = "3.0.3",
    .build = {0x6A, 0x85, 0x26, 0x2F, 0x21, 0xB9, 0x03, 0x64, 0x9B, 0xD7, 0xC6, 0x26, 0x28, 0xD2,
              0x6E, 0x43},
    .ptr = 4,
    .ctx_pins = ContextPins,
    .racer_pins = RacerPins,
    .item_pins = ItemPins,
    .table_pins = TablePins,
    .horn_pins = HornPins,
    .patch_windows = PatchWindows,
    .ctgp_plugin = true,
    // root cell: 0x87a144 `ldr r0,[pc,#0x10]` (literal at 0x87a15c) + 0x87a148 `ldr r0,[pc,r0]`
    .root = {&PinRaceInfoHolder, 0x87a144, 0x87a148, Decode::A32Got},
    // Luigi's Mansion Y scale: 0x5537bc `vldr s4,[pc,#0x1c4]` -> 0x553988 (MapYScaleConst)
    .yscale = {&PinMapYScaleMul, 0x5537bc, 0, Decode::A32Vldr},
    .ui = {&PinUiRoot, 0x94e8e8, 0x94e8ec, Decode::A32Got},
    .tokens = {&PinCourseTokens, 0x1c540, 0x1c548, Decode::A32Add},
    .cups = {&PinCupOfCourse, 0x557188, 0x557190, Decode::A32Add},
    .cup_msgs = {&PinCupMsg, 0x451f5c, 0x451f60, Decode::A32Add},
    .param_db = {&PinDriverParamDb, 0x8f2a14, 0x8f2a1c, Decode::A32Got},
    .string_pool = {},
    .frames = {&PinItemFrame, 0x456124, 0x45612c, Decode::A32Add},
    .root_scene_holder = 0x10,
    .scene_holder_scene = 0x04,
    .scene_flags = 0xd4,
    .scene_kind = 0xd8,
    .root_engine = 0x14,
    .engine_raceinfo_holder = 0x140,
    .holder_raceinfo = 0x08,
    .scene_object_engine = 0xe0 + 4 * 4,
    .oe_rd = 0x11c,
    .oe_kd = 0x12c,
    .oe_id = 0x130,
    .rd_sequence = 0x2c,
    .rd_lap_manager = 0x30,
    .rd_checker_count = 0x34,
    .rd_checkers = 0x3c,
    .rd_timer = 0xac,
    .seq_phase = 0x1c,
    .lm_laps = 0x34,
    .tm_time = 0x20,
    .ck_flags = 0x20,
    .ck_rank = 0x24,
    .ck_lap = 0x28,
    .ck_coins = 0x34,
    .kd_units = 0x2c,
    .kd_count = 0x64,
    .ku_vehicle = 0x24,
    .ku_move = 0x28,
    .vehicle_player = 0x54,
    .move_position = 0x04 + 0x24,
    .engine_system = 0xe0,
    .se_player_manager = 0x528,
    .pm_local_obj = 0xd0,
    .pm_racer_obj = 0x120,
    .id_owner_count = 0x74,
    .id_owners = 0x7c,
    .io_slots = 0x34,
    .owner_index = 0x24,
    .slot_block = 0x20,
    .slot_state = 0x21,
    .slot_count = 0x5c,
    .slot_item = 0x7c,
    .slot_block_end = 0x80,
    .ui_object = 0x04,
    .ui_resource3 = 0x0c + 4 * 3,
    .r3_cup_table = 0x1c,
    .r3_driver_table = 0x170,
    .r3_pict_table = 0x1d0,
    .table_name_count = 0x40,
    .table_names = 0x44,
    .table_msg_count = 0x48,
    .table_msgs = 0x4c,
    // DriverTableAlloc main+0x444b18 `mov r0,#0xd8` / 0x36 names; PictTableAlloc main+0x449938
    // `mov r0,#0x1f0` / 0x7c names.
    .driver_capacity = 0x36,
    .pict_capacity = 0x7c,
    .db_root = 0x78,
    .db_present = 0x14,
    .db_records = 0x1c,
    .db_count = 0x56,
    .db_code = 0x10,
    .db_code_is_ptr = true,
    .se_inputs = 0x520,
    .inputs_count = 0x2c,
    .inputs_array = 0x34,
    .input_entry_obj = 0x1c,
    .input_horn_request = 0x321,
    // Racer map icons (icon-create loop main+0x580088..0x5802d8): icon+0x68 = KU[i], +0x6c =
    // CK[i], +0x74 = map part; appended to the map part's list (count +0x64, array [+0x6c]).
    // Per-icon update main+0x554404..0x5544e0: pane translate via main+0x948738 on icon+0x18:
    // [[icon+0x18+8]+0xc]+0x18 (x, y, z); +186 unless u8 map+0x2c5.
    .map_icon_list = 0x6c,
    .map_tv_flag = 0x2c5,
    .icon_pid = 0,
    .icon_ku = 0x68,
    .icon_part = 0x20,
    .part_pane = 0x0c,
    .pane_translate = 0x18,
};
} // namespace p303

constexpr std::array<const Profile*, 2> Profiles{&p400::Build, &p303::Build};

/// The reader copies several fields with one block read into a fixed buffer, relying on the
/// field order of each profile. Checked here for every profile, so a new profile cannot overrun.
constexpr bool BlocksFit(const Profile& p) {
    return (p.ptr == 4 || p.ptr == 8) &&
           // ReadRacers: checker flags .. coins (0x40), kart unit vehicle .. move (0x20), unit
           // pointers before the kart count, race director count .. checker array
           p.ck_flags < p.ck_rank && p.ck_rank + 2 <= p.ck_lap && p.ck_lap + 4 <= p.ck_coins &&
           p.ck_coins + 1 - p.ck_flags <= 0x40 && p.ku_vehicle + p.ptr <= p.ku_move &&
           p.ku_move + p.ptr - p.ku_vehicle <= 0x20 &&
           p.kd_units + MaxRacers * p.ptr <= p.kd_count &&
           p.rd_checker_count + 4 <= p.rd_checkers &&
           // ReadSnapshot: sequence, lap manager .. timer pointers in one block
           p.rd_sequence + p.ptr <= p.rd_lap_manager && p.rd_lap_manager + p.ptr <= p.rd_timer &&
           // ReadItems: owner index .. two slot pointers (0x40), slot block (0x100)
           p.id_owner_count + 4 <= p.id_owners && p.owner_index + 4 <= p.io_slots &&
           p.io_slots + 2 * p.ptr - p.owner_index <= 0x40 && p.slot_block < p.slot_state &&
           p.slot_count + 4 <= p.slot_block_end && p.slot_item + 4 <= p.slot_block_end &&
           p.slot_block_end - p.slot_block <= 0x100 &&
           // ReadLocalSlot: local controller table before the racer table
           p.pm_local_obj + 4 <= p.pm_racer_obj;
}
static_assert(BlocksFit(p400::Build) && BlocksFit(p303::Build));

// ---- CTGP-DX 1.1.1 plugin on 3.0.3 ------------------------------------------------------------
namespace ctgp {
#include "mk8d_pins303_ctgp.inc"
constexpr std::array<const Pin*, 13> Pins{
    &PinCtThunkCupMsg, &PinCtCupMsg,     &PinCtCtx,      &PinCtCourseLookup, &PinCtListCount,
    &PinCtListIndex,   &PinCtEntryName,  &PinCtEntryCup, &PinCtEntryFolder,  &PinCtEntryPict,
    &PinCtCupFind,     &PinCtCupMapFind, &PinCtCupLabel};
/// main+0x451f38: the plugin's function hook of the cup-name function (CupMsg) jumps to its thunk
/// plugin+0x28cb0 (`push; bl 0x66ec; pop`).
constexpr u64 HookSite = 0x451f38, ThunkOffset = 0x28cb0;
/// Plugin image extent (segments 0..0x86000 + bss to 0x87bb0, from the NRO's own headers).
constexpr u64 ImageSize = 0x87bb0;
/// ctx accessor 0x1be0: r3 = (0x1bec + 8) + lit[0x1c04]; slot = r3 + lit[0x1c08]; ctx = [slot].
constexpr u64 CtxAdd = 0x1bec;
constexpr std::size_t CtxLitA = 9, CtxLitB = 10; ///< word indices of the literals in PinCtCtx
constexpr u64 CtxCups = 0x08, CtxCourses = 0x0c; ///< CtCupMsg `ldr r0,[r0,#8]`, CtCourseLookup +0xc
constexpr u64 EntryFolder = 0x00, EntryPict = 0x04, EntryName = 0x18, EntryCup = 0x33;
constexpr u64 CupsMap = 0x08, MapFirst = 0x08, NodeKey = 0x04, NodeValue = 0x08;
constexpr u64 CupLabel = 0x04, CupIcon = 0x08;
constexpr u32 MaxEntries = 512, MaxCupNodes = 64;
} // namespace ctgp

/// A32 jump the CTGP-DX function hooks write (`ldr pc,[pc,#-4]` + absolute target).
constexpr u32 JumpWord = 0xe51ff004;

// ---- offsets shared by both builds (unchanged between 3.0.3 and 4.0.0) --------------------------
// Race info fields (RaceRule, EngineClass, Mirror, RaceCount, MapYScale, DriverEntry,
// DriverVariant): all u32 fields, same layout in both builds.
constexpr u64 RiRule = 0x08, RiSubRule = 0x0c, RiClass = 0x10, RiMirror = 0x26;
constexpr u64 RiEntries = 0x30, RiEntryStride = 0x1c, EntryDriver = 0x0c, EntryVariant = 0x10;
constexpr u64 RiCount = 0x180, RiCourse = 0x1a0, RiBlock = 0x1a4;
constexpr u8 SceneActiveBit = 0x04;
constexpr u64 CupOfCourse = 0xc80, CupStride = 0x80, CupSlotStride = 0x10;
constexpr u64 DriverRecordSize = 0x244, RecordFlags = 0x60;
/// Race phase u32 SEQ+phase (PinPhase). Values named from the code's compares (5/6/7/8, 3..5,
/// 5..6) and live transitions: 0 loading, 1 initialised (loading screen),
/// 2 course fly-by, 3 intro end, 4 grid banner, 5 countdown, 6 racing, 7 local player finished.
constexpr int RacePhaseInitialized = 1;
constexpr int CupNameEntries = 0x18;
// Horn (PinPlayerInput, PinHornEvent, PinHornFill ... PinHornPlay): the horn is action bit 8
// (0x100) of the player's action word. No button maps it (4.0.0: action 8 mask 0; 3.0.3: the
// hard-coded mapper never sets it); the game raises it from the one-shot request byte
// (4.0.0 obj+0x35d, main+0x7d9200..0x7d9210; 3.0.3 obj+0x321, main+0x859f68..0x859f78) and
// clears that byte every frame (main+0x7d9260 / main+0x859fd4). Writing 1 there is one native
// horn event.
// A native horn source re-arms the byte every frame while its input is held (main+0x33270);
// the sound latch needs the request on consecutive frames and plays on release (main+0xaf148).
// The module holds for HornHoldTicks (a tap: ~167 ms at the 60 Hz tick), re-arming the byte
// whenever the game has consumed it, then stops; an unconsumed last write is taken back.
constexpr u64 HornHoldTicks = 10;
/// The horn action is offered only while racing (phase 6, "GO!" until the local finish).
constexpr int HornPhase = 6;

const Profile* ProfileOf(const u8* build_id32) {
    if (!build_id32)
        return nullptr;
    for (const Profile* p : Profiles)
        if (std::equal(p->build.begin(), p->build.end(), build_id32) &&
            std::all_of(build_id32 + 16, build_id32 + 32, [](u8 b) { return b == 0; }))
            return p;
    return nullptr;
}

// ---- AArch64 immediates (the pins fix the opcodes; only the immediates are decoded) ---------
s64 SignExtend(u64 v, int bits) {
    const u64 m = u64{1} << (bits - 1);
    return static_cast<s64>((v ^ m) - m);
}
/// ADRP at image offset `pc`: page offset relative to the image base (base is page aligned).
std::optional<s64> AdrpPage(u64 pc, u32 w) {
    if ((w & 0x9f000000u) != 0x90000000u)
        return std::nullopt;
    const u64 imm = ((w >> 29) & 3u) | (static_cast<u64>((w >> 5) & 0x7ffffu) << 2);
    return static_cast<s64>(pc & ~u64{0xfff}) + SignExtend(imm, 21) * 4096;
}
std::optional<u64> LdrX64Imm(u32 w) { // LDR Xt, [Xn, #imm]
    if ((w & 0xffc00000u) != 0xf9400000u)
        return std::nullopt;
    return static_cast<u64>((w >> 10) & 0xfffu) * 8;
}
std::optional<u64> LdrS32Imm(u32 w) { // LDR St, [Xn, #imm]
    if ((w & 0xffc00000u) != 0xbd400000u)
        return std::nullopt;
    return static_cast<u64>((w >> 10) & 0xfffu) * 4;
}
std::optional<u64> AddX64Imm(u32 w) { // ADD Xd, Xn, #imm{, lsl #12}
    if ((w & 0xff800000u) != 0x91000000u)
        return std::nullopt;
    const u64 imm = (w >> 10) & 0xfffu;
    return (w >> 22) & 1 ? imm << 12 : imm;
}
// ---- A32 (ARM mode, cond AL) ------------------------------------------------------------------
/// `ldr rT, [pc, #+-imm12]` at `pc`: {literal address, rT}.
std::optional<std::pair<u64, u32>> A32LdrLiteral(u64 pc, u32 w) {
    if ((w & 0xff7f0000u) != 0xe51f0000u)
        return std::nullopt;
    const u64 imm = w & 0xfffu;
    const u64 at = (w >> 23) & 1 ? pc + 8 + imm : pc + 8 - imm;
    return std::pair{at, (w >> 12) & 0xfu};
}
/// `ldr rD, [pc, rT]` (0xe79fD00T) or `add rD, pc, rT` (0xe08fD00T): the offset register must be
/// the literal's rt (e.g. 3.0.3 main+0x8f2a1c `ldr r5,[pc,r0]`).
bool A32PcUse(u32 w, u32 rt, bool add) {
    const u32 op = add ? 0xe08f0000u : 0xe79f0000u;
    return (w & 0xffff0ff0u) == op && (w & 0xfu) == rt;
}
/// `vldr sN, [pc, #+-imm8*4]` at `pc`: address of the loaded word.
std::optional<u64> A32VldrLiteral(u64 pc, u32 w) {
    if ((w & 0xff3f0f00u) != 0xed1f0a00u)
        return std::nullopt;
    const u64 imm = static_cast<u64>(w & 0xffu) * 4;
    const u64 aligned = (pc + 8) & ~u64{3};
    return (w >> 23) & 1 ? aligned + imm : aligned - imm;
}
u32 PinWord(const Pin& pin, u64 at) {
    return pin.words[static_cast<std::size_t>((at - pin.offset) / 4)];
}
bool PinCovers(const Pin& pin, u64 at) {
    return at >= pin.offset && at + 4 <= pin.offset + pin.words.size() * 4 && (at & 3) == 0;
}

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && std::string_view{v} == "1";
}

} // namespace

bool SupportsBuildHex(const char* hex) {
    if (!hex || std::strlen(hex) != 64)
        return false;
    std::array<u8, 32> bytes{};
    for (std::size_t i = 0; i < 32; ++i) {
        const auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1; // the host passes upper-case hex
        };
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes[i] = static_cast<u8>(hi << 4 | lo);
    }
    return ProfileOf(bytes.data()) != nullptr;
}
bool SupportsBuildId(const std::uint8_t* build_id32) {
    return ProfileOf(build_id32) != nullptr;
}

// Pre-formatted output names ("r3.map_x"), built once.
struct Reader::Names {
    struct Row {
        std::string valid, rank, driver, variant, lap, coins, is_me, map_x, map_y, map_ok, icon,
            name, item0, item1, item_state, item1_state, item0_count, item1_count, item0_key,
            item1_key, emblem_key, mapf_x, mapf_y, finished;
    };
    std::array<Row, MaxRacers> rows;
    p5r_publish::Filter filter;
    std::array<bool, 24> want{}; ///< per Row field, in declaration order
    Names() {
        for (int i = 0; i < MaxRacers; ++i) {
            const std::string p = "r" + std::to_string(i) + ".";
            auto& r = rows[static_cast<std::size_t>(i)];
            r = {p + "valid",       p + "rank",        p + "driver",     p + "variant",
                 p + "lap",         p + "coins",       p + "is_me",      p + "map_x",
                 p + "map_y",       p + "map_ok",      p + "icon",       p + "name",
                 p + "item0",       p + "item1",       p + "item_state", p + "item1_state",
                 p + "item0_count", p + "item1_count", p + "item0_key",  p + "item1_key",
                 p + "emblem_key",  p + "mapf_x",      p + "mapf_y",     p + "finished"};
        }
    }
};

Reader::Reader(const EdenDsmodHostApi& api, Mk8dAssets::Library& library, const char* config)
    : host{api}, prof{ProfileOf(api.build_id)}, assets{library}, names{std::make_unique<Names>()} {
    names->filter.Configure(config);
    if (EnvFlag("EDEN_DSMOD_MK8D_ALL_OUTPUTS"))
        names->filter.Configure(nullptr);
    const auto& r0 = names->rows[0];
    const std::array<const std::string*, 24> fields{
        &r0.valid,       &r0.rank,        &r0.driver,      &r0.variant,   &r0.lap,
        &r0.coins,       &r0.is_me,       &r0.map_x,       &r0.map_y,     &r0.map_ok,
        &r0.icon,        &r0.name,        &r0.item0,       &r0.item1,     &r0.item_state,
        &r0.item1_state, &r0.item0_count, &r0.item1_count, &r0.item0_key, &r0.item1_key,
        &r0.emblem_key,  &r0.mapf_x,      &r0.mapf_y,      &r0.finished};
    for (std::size_t f = 0; f < fields.size(); ++f)
        names->want[f] = names->filter.Wants(fields[f]->c_str());
    want_map =
        names->want[7] || names->want[8] || names->want[9] || names->want[21] || names->want[22];
    want_names = names->want[11];
    debug = EnvFlag("EDEN_DSMOD_MK8D_DEBUG");
    racer_icon_driver.fill(-2);
    racer_icon_variant.fill(-2);
}

Reader::~Reader() {
    if (camera_job.valid())
        camera_job.wait();
    if (preload_job.valid())
        preload_job.wait();
    if (ctgp_text_job.valid())
        ctgp_text_job.wait();
}

bool Reader::ReadBlock(u64 at, void* out, std::size_t n) const {
    return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, out, n);
}

std::optional<u64> Reader::Word(u64 at) const {
    if (prof && prof->ptr == 4) {
        const auto v = Read<u32>(at);
        return v ? std::optional<u64>{*v} : std::nullopt;
    }
    return Read<u64>(at);
}

u64 Reader::WordIn(const u8* bytes) const {
    if (prof && prof->ptr == 4) {
        u32 v{};
        std::memcpy(&v, bytes, 4);
        return v;
    }
    u64 v{};
    std::memcpy(&v, bytes, 8);
    return v;
}

std::optional<u64> Reader::Ptr(u64 at) const {
    const auto v = Word(at);
    const u64 align = prof ? prof->ptr - 1 : 7;
    if (!v || !*v || (*v & align) != 0)
        return std::nullopt;
    return v;
}

bool Reader::InMain(u64 at, u64 n) const {
    return dsmod_sdk::InMain(base, size, at, n);
}

bool Reader::CheckPin(const Pin& pin, bool* windowed) const {
    const u64 bytes = pin.words.size() * 4;
    if (pin.offset + bytes > size)
        return false;
    std::vector<u32> live(pin.words.size());
    if (!ReadBlock(base + pin.offset, live.data(), bytes))
        return false;
    if (std::equal(live.begin(), live.end(), pin.words.begin()))
        return true;
    // A known plugin patch window (3.0.3 + CTGP-DX): the 8 bytes at the site are exactly the
    // plugin's jump (`ldr pc,[pc,#-4]` + a target outside main); every word outside it is exact.
    // Words at the window read from live memory (they may begin before the pin).
    for (const u64 w : prof ? prof->patch_windows : std::span<const u64>{}) {
        if (w + 8 <= pin.offset || w >= pin.offset + bytes)
            continue;
        // Two forms seen live: the function hook `ldr pc,[pc,#-4]` + target
        // (8 bytes) and the inline hook `b <trampoline>` (4 bytes); both targets lie outside main.
        std::array<u32, 2> jump{};
        if (!ReadBlock(base + w, jump.data(), sizeof(jump)))
            continue;
        u64 len = 0;
        if (jump[0] == JumpWord && !InMain(jump[1], 4)) {
            len = 8;
        } else if ((jump[0] & 0xff000000u) == 0xea000000u) {
            const u64 target =
                base + w + 8 + static_cast<u64>(SignExtend(jump[0] & 0xffffffu, 24) * 4);
            if (!InMain(target, 4))
                len = 4;
        }
        if (!len || w + len <= pin.offset)
            continue;
        bool ok = true;
        for (std::size_t i = 0; i < live.size() && ok; ++i) {
            const u64 at = pin.offset + 4 * i;
            if (at >= w && at < w + len)
                continue;
            ok = live[i] == pin.words[i];
        }
        if (ok) {
            if (windowed)
                *windowed = true;
            return true;
        }
    }
    return false;
}

/// Decode one global from its pinned instructions (the pin itself was checked). A32 literals
/// must lie inside a pin of the same build (so the literal word is checked too).
u64 Reader::Global(const GlobalSpec& g) const {
    if (!prof || !g.pin || g.kind == Decode::None)
        return 0;
    const Pin& pin = *g.pin;
    const auto pinned = [&](u64 at) -> std::optional<u32> {
        for (const auto list :
             {prof->ctx_pins, prof->racer_pins, prof->item_pins, prof->table_pins, prof->horn_pins})
            for (const Pin* p : list)
                if (PinCovers(*p, at))
                    return PinWord(*p, at);
        return std::nullopt;
    };
    u64 at = 0;
    u64 need = 1;
    switch (g.kind) {
    case Decode::AdrpLdrX:
    case Decode::AdrpLdrS:
    case Decode::AdrpAdd: {
        const auto page = AdrpPage(g.a, PinWord(pin, g.a));
        const auto off = g.kind == Decode::AdrpLdrX   ? LdrX64Imm(PinWord(pin, g.b))
                         : g.kind == Decode::AdrpLdrS ? LdrS32Imm(PinWord(pin, g.b))
                                                      : AddX64Imm(PinWord(pin, g.b));
        if (!page || !off || *page < 0)
            return 0;
        at = base + static_cast<u64>(*page) + *off;
        need = g.kind == Decode::AdrpAdd ? 1 : 8;
        break;
    }
    case Decode::A32Got:
    case Decode::A32Add: {
        const auto lit = A32LdrLiteral(g.a, PinWord(pin, g.a));
        if (!lit || !PinCovers(pin, g.b) ||
            !A32PcUse(PinWord(pin, g.b), lit->second, g.kind == Decode::A32Add))
            return 0;
        const auto value = pinned(lit->first);
        if (!value)
            return 0;
        at = base + ((g.b + 8 + *value) & 0xffffffffu);
        need = g.kind == Decode::A32Got ? 4 : 1;
        break;
    }
    case Decode::A32Vldr: {
        const auto lit = A32VldrLiteral(g.a, PinWord(pin, g.a));
        if (!lit || !pinned(*lit))
            return 0;
        at = base + *lit;
        need = 4;
        break;
    }
    case Decode::None:
        return 0;
    }
    return InMain(at, need) ? at : 0;
}

bool Reader::ResolveCode() {
    code_resolved = true;
    code_ok = false;
    racers_code_ok = items_code_ok = tables_code_ok = horn_code_ok = false;
    // Every decoded global is re-derived below from pins that passed in this check; a domain
    // whose pins fail on a re-check must not keep the addresses of an earlier pass.
    root_cell = ui_cell = tokens_at = frames_at = cups_at = yscale_at = cup_msgs_at = 0;
    param_db_cell = string_pool_cell = 0;
    ctgp_windows = 0;
    code_diag.clear();
    base = host.main_base;
    size = host.main_size;
    if (!prof) {
        code_diag = "unsupported build";
        return false;
    }
    if (!base || !size) {
        code_diag = "no main image";
        return false;
    }
    std::string failed;
    const auto check = [&](std::span<const Pin* const> list) {
        bool ok = true;
        for (const Pin* p : list) {
            bool windowed = false;
            if (!CheckPin(*p, &windowed)) {
                ok = false;
                failed += std::string{failed.empty() ? "" : ","} + p->name;
            } else if (windowed) {
                ++ctgp_windows;
            }
        }
        return ok;
    };
    const bool ctx = check(prof->ctx_pins), racers = check(prof->racer_pins),
               items = check(prof->item_pins), tabs = check(prof->table_pins),
               horn = check(prof->horn_pins);
    horn_code_ok = horn;
    racers_code_ok = racers;
    items_code_ok = items;
    tables_code_ok = tabs;
    if (!failed.empty())
        code_diag = "code pin mismatch: " + failed;
    if (!ctx)
        return false;
    // Decode the globals from their own instructions.
    root_cell = Global(prof->root);
    yscale_at = Global(prof->yscale);
    if (tabs) {
        ui_cell = Global(prof->ui);
        tokens_at = Global(prof->tokens);
        cups_at = Global(prof->cups);
        cup_msgs_at = Global(prof->cup_msgs);
        param_db_cell = Global(prof->param_db);
        string_pool_cell = Global(prof->string_pool);
    }
    if (items)
        frames_at = Global(prof->frames);
    code_ok = root_cell != 0;
    if (!code_ok && code_diag.empty())
        code_diag = "root cell decode failed";
    ResolvePlugin();
    return code_ok;
}

void Reader::ResolvePlugin() {
    ct_ok = false;
    ct_base = ct_ctx = 0;
    ct_diag.clear();
    ct_entry = {}; // read again from the (re-)checked plugin
    if (!prof || !prof->ctgp_plugin || !code_ok)
        return;
    std::array<u32, 2> jump{};
    if (!ReadBlock(base + ctgp::HookSite, jump.data(), sizeof(jump)) || jump[0] != JumpWord)
        return; // vanilla 3.0.3 (or the plugin has not hooked yet): no plugin
    if (InMain(jump[1], 4) || jump[1] < ctgp::ThunkOffset) {
        ct_diag = "ctgp:hook";
        return;
    }
    const u64 pbase = jump[1] - ctgp::ThunkOffset;
    std::string failed;
    for (const Pin* pin : ctgp::Pins) {
        std::vector<u32> live(pin->words.size());
        if (pin->offset + live.size() * 4 > ctgp::ImageSize ||
            !ReadBlock(pbase + pin->offset, live.data(), live.size() * 4) ||
            !std::equal(live.begin(), live.end(), pin->words.begin()))
            failed += std::string{failed.empty() ? "" : ","} + pin->name;
    }
    if (!failed.empty()) {
        ct_diag = "ctgp pin mismatch: " + failed;
        return;
    }
    const u64 slot = ctgp::CtxAdd + 8 + ctgp::PinCtCtx.words[ctgp::CtxLitA] +
                     ctgp::PinCtCtx.words[ctgp::CtxLitB];
    const auto ctx = slot + 4 <= ctgp::ImageSize ? Ptr(pbase + slot) : std::nullopt;
    if (!ctx || *ctx < pbase || *ctx >= pbase + ctgp::ImageSize) {
        ct_diag = "ctgp:ctx";
        return;
    }
    ct_base = pbase;
    ct_ctx = *ctx;
    ct_ok = true;
}

Reader::CtEntry Reader::ReadCtEntry(int raw) const {
    CtEntry e;
    e.raw = raw;
    if (!ct_ok || raw < 0)
        return e;
    // CtCourseLookup / CtListCount / CtListIndex: L = [[ctx+0xc]]; i = raw + 1 < u32 [L];
    // vec = [L+4] = {begin, end}; i < (end - begin) / 4; entry = [begin + 4i].
    const auto lp = Ptr(ct_ctx + ctgp::CtxCourses);
    const auto list = lp ? Ptr(*lp) : std::nullopt;
    const auto count = list ? Read<u32>(*list) : std::nullopt;
    const auto vec = list ? Ptr(*list + 4) : std::nullopt;
    const u64 i = static_cast<u64>(raw) + 1;
    std::array<u32, 2> be{};
    if (!count || !vec || *count > ctgp::MaxEntries || i >= *count ||
        !ReadBlock(*vec, be.data(), sizeof(be)) || be[1] < be[0] || (be[1] - be[0]) / 4 <= i)
        return e;
    const auto entry = Ptr(be[0] + 4 * i);
    const auto folder =
        entry ? Word(*entry + ctgp::EntryFolder) : std::nullopt; // char*, any alignment
    const auto pict = entry ? Word(*entry + ctgp::EntryPict) : std::nullopt;
    const auto name = entry ? Read<s32>(*entry + ctgp::EntryName) : std::nullopt;
    const auto cup = entry ? Read<s8>(*entry + ctgp::EntryCup) : std::nullopt;
    const auto fs = folder && *folder ? ReadCString(*folder, 64) : std::nullopt;
    const auto ps = pict && *pict ? ReadCString(*pict, 64) : std::nullopt;
    if (!fs || fs->empty() || !ps || !name || !cup)
        return e;
    for (const char c : *fs)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return e;
    e.folder = *fs;
    e.pict = *ps;
    e.name_label = *name;
    e.cup = *cup;
    // CtCupMsg / CtCupFind / CtCupMapFind / CtCupLabel: map = [[ctx+8]+8]; node list from
    // [map+8]: {next, s8 key +4, value +8}; cup object: +4 name label, +8 icon name char*.
    const auto cups = Ptr(ct_ctx + ctgp::CtxCups);
    const auto map = cups ? Ptr(*cups + ctgp::CupsMap) : std::nullopt;
    auto node = map ? Ptr(*map + ctgp::MapFirst) : std::nullopt;
    for (u32 n = 0; node && n < ctgp::MaxCupNodes; ++n) {
        const auto key = Read<s8>(*node + ctgp::NodeKey);
        if (key && *key == e.cup) {
            const auto value = Ptr(*node + ctgp::NodeValue);
            const auto label = value ? Read<s32>(*value + ctgp::CupLabel) : std::nullopt;
            const auto icon = value ? Word(*value + ctgp::CupIcon) : std::nullopt;
            const auto is = icon && *icon ? ReadCString(*icon, 64) : std::nullopt;
            if (label && is) {
                e.cup_label = *label;
                e.cup_icon = *is;
            }
            break;
        }
        node = Ptr(*node);
    }
    e.ok = true;
    return e;
}

const std::string* Reader::LabelText(int label) const {
    if (label <= 0)
        return nullptr;
    const std::string key = std::to_string(label);
    if (common)
        if (const auto* t = common->Find(key))
            return t;
    if (ctgp_text)
        return ctgp_text->Find(key);
    return nullptr;
}

std::optional<std::string> Reader::ReadCString(u64 at, std::size_t max) const {
    std::string out;
    char chunk[64];
    while (out.size() < max) {
        // Never cross a 4 KiB page in one read (the next page may be unmapped).
        const std::size_t room = static_cast<std::size_t>(0x1000 - (at & 0xfff));
        const std::size_t n = std::min<std::size_t>({sizeof(chunk), room, max - out.size()});
        if (!ReadBlock(at, chunk, n))
            return std::nullopt;
        for (std::size_t i = 0; i < n; ++i) {
            if (!chunk[i])
                return out;
            out.push_back(chunk[i]);
        }
        at += n;
    }
    return std::nullopt;
}

bool Reader::ResolveTables(u64 r3) {
    Tables t;
    t.r3 = r3;
    const u64 pw = prof->ptr;
    std::string why;
    const auto fail = [&](const char* what) {
        tables_diag = std::string{"tables:"} + what + why;
        return false;
    };
    const auto name_table = [&](u64 table, std::vector<std::string>& out, u64 capacity) {
        auto count = Read<u32>(table + prof->table_name_count);
        if (count && capacity && *count > capacity) {
            tables_capped += " " + std::to_string(*count) + ">" + std::to_string(capacity);
            count = static_cast<u32>(capacity);
        }
        const auto arr = Ptr(table + prof->table_names);
        if (!count || !arr || *count == 0 || *count > 256) {
            why = count ? " count " + std::to_string(*count) : " count";
            return false;
        }
        std::vector<u8> raw(*count * pw);
        if (!ReadBlock(*arr, raw.data(), raw.size())) {
            why = " array";
            return false;
        }
        out.clear();
        for (std::size_t i = 0; i < *count; ++i) {
            const u64 p = WordIn(raw.data() + i * pw);
            auto s = p ? ReadCString(p, 96) : std::optional<std::string>{std::string{}};
            if (!s) {
                why = " name " + std::to_string(i);
                return false;
            }
            out.push_back(std::move(*s));
        }
        return true;
    };
    tables_capped.clear();
    if (!name_table(r3 + prof->r3_driver_table, t.driver_names, prof->driver_capacity))
        return fail("drivers");
    if (!name_table(r3 + prof->r3_pict_table, t.pict_names, prof->pict_capacity))
        return fail("pictures");
    {
        const u64 dt = r3 + prof->r3_driver_table;
        const auto count = Read<u32>(dt + prof->table_msg_count);
        const auto arr = Ptr(dt + prof->table_msgs);
        if (!count || !arr || *count == 0 || *count > 256 ||
            (t.driver_msgs.resize(*count),
             !ReadBlock(*arr, t.driver_msgs.data(), t.driver_msgs.size() * 4)))
            return fail("driver-msgs");
    }
    // Course enum text: "Invalid ,Gu_Menu ,Test ,...". Split on ',' and trim blanks.
    if (tokens_at) {
        const auto text = ReadCString(tokens_at, 8192);
        if (!text)
            return fail("tokens");
        std::string cur;
        const auto flush = [&] {
            const auto b = cur.find_first_not_of(' ');
            const auto e = cur.find_last_not_of(' ');
            t.course_tokens.push_back(b == std::string::npos ? std::string{}
                                                             : cur.substr(b, e - b + 1));
            cur.clear();
        };
        for (const char c : *text) {
            if (c == ',')
                flush();
            else
                cur.push_back(c);
        }
        flush();
        if (t.course_tokens.empty() || t.course_tokens[0] != "Invalid")
            return fail("tokens-text");
    }
    if (cups_at) {
        std::array<u8, CupNameEntries * 8> raw{};
        if (!ReadBlock(cups_at, raw.data(), CupNameEntries * pw))
            return fail("cup-names");
        for (std::size_t c = 0; c < CupNameEntries; ++c) {
            const u64 p = WordIn(raw.data() + c * pw);
            auto s = p && InMain(p, 1) ? ReadCString(p, 64) : std::nullopt;
            t.cup_names.push_back(s ? *s : std::string{});
        }
    }
    if (cup_msgs_at) {
        t.cup_msgs.resize(CupNameEntries);
        if (!ReadBlock(cup_msgs_at, t.cup_msgs.data(), t.cup_msgs.size() * 4))
            return fail("cup-msgs");
    }
    if (frames_at && !ReadBlock(frames_at, t.item_frames.data(), t.item_frames.size()))
        return fail("item-frames");
    if (yscale_at) {
        float f{};
        if (ReadBlock(yscale_at, &f, 4))
            t.y_scale_0x44 = f;
    }
    t.ok = true;
    tables_diag.clear();
    tables = std::move(t);
    racer_icon_driver.fill(-2); // icon / name / emblem strings come from these tables
    item_keys_ready = false;    // and the item frame table
    return true;
}

int Reader::ReadLocalSlot(u64 root, int count, std::string& why) const {
    const auto engine = Ptr(root + prof->root_engine);
    const auto se = engine ? Ptr(*engine + prof->engine_system) : std::nullopt;
    const auto pm = se ? Ptr(*se + prof->se_player_manager) : std::nullopt;
    // u32 PM+local[0] (controller 0 -> player object) and u32 PM+racer[i] (racer i -> object).
    const std::size_t first = static_cast<std::size_t>(prof->pm_racer_obj - prof->pm_local_obj) / 4;
    std::vector<s32> idx(first + MaxRacers);
    if (!pm || !ReadBlock(*pm + prof->pm_local_obj, idx.data(), idx.size() * 4)) {
        why += " pm";
        return -1;
    }
    const s32 local = idx[0];
    int found = -1;
    for (int i = 0; i < count; ++i)
        if (local >= 0 && idx[first + static_cast<std::size_t>(i)] == local) {
            if (found >= 0) {
                why += " me-ambiguous";
                return -1;
            }
            found = i;
        }
    if (found < 0)
        why += " me-none";
    return found;
}

void Reader::ReadRacers(Snapshot& s, u64 oe, const std::vector<u8>& ri_block, u64 rd) {
    const Profile& P = *prof;
    const u64 pw = P.ptr;
    std::vector<u8> rdb(P.rd_checkers + pw - P.rd_checker_count);
    if (!ReadBlock(rd + P.rd_checker_count, rdb.data(), rdb.size())) {
        s.diag += " rd";
        return;
    }
    u32 ck_count{};
    std::memcpy(&ck_count, rdb.data(), 4);
    const u64 ck_arr = WordIn(rdb.data() + (P.rd_checkers - P.rd_checker_count));
    s32 ri_count{};
    std::memcpy(&ri_count, ri_block.data() + RiCount, 4);
    if (ri_count < 1 || ri_count > MaxRacers || ck_count < static_cast<u32>(ri_count) || !ck_arr) {
        s.diag += " count";
        return;
    }
    const int n = ri_count;
    std::array<u8, MaxRacers * 8> ckb{};
    if (!ReadBlock(ck_arr, ckb.data(), static_cast<std::size_t>(n) * pw)) {
        s.diag += " checkers";
        return;
    }
    std::array<u64, MaxRacers> ck{};
    for (int i = 0; i < n; ++i)
        ck[static_cast<std::size_t>(i)] = WordIn(ckb.data() + pw * static_cast<std::size_t>(i));
    s.dbg_rd = rd;
    ck_debug = ck;
    const auto kd = Ptr(oe + P.oe_kd);
    std::vector<u8> kdb(P.kd_count + 4 - P.kd_units);
    const bool kd_ok = kd && ReadBlock(*kd + P.kd_units, kdb.data(), kdb.size());
    s32 kd_count{};
    if (kd_ok)
        std::memcpy(&kd_count, kdb.data() + (P.kd_count - P.kd_units), 4);
    s.count = n;
    const std::size_t ck_len = P.ck_coins + 1 - P.ck_flags;
    for (int i = 0; i < n; ++i) {
        Racer& r = s.racers[static_cast<std::size_t>(i)];
        const std::size_t e = RiEntries + RiEntryStride * static_cast<std::size_t>(i);
        u32 driver{};
        std::memcpy(&driver, ri_block.data() + e + EntryDriver, 4);
        r.driver = driver <= 0x7fffffffu ? static_cast<int>(driver) : -1;
        r.variant = ri_block[e + EntryVariant];
        if (!ck[static_cast<std::size_t>(i)])
            continue;
        // Integer block flags..coins, read twice (torn-read rejection).
        std::array<u8, 0x40> a{}, b{};
        bool stable = false;
        for (int attempt = 0; attempt < 3 && !stable; ++attempt)
            stable = ReadBlock(ck[static_cast<std::size_t>(i)] + P.ck_flags, a.data(), ck_len) &&
                     ReadBlock(ck[static_cast<std::size_t>(i)] + P.ck_flags, b.data(), ck_len) &&
                     a == b;
        if (!stable)
            continue;
        u32 flags{};
        u16 rank{};
        u32 lap{};
        std::memcpy(&flags, a.data(), 4);
        std::memcpy(&rank, a.data() + (P.ck_rank - P.ck_flags), 2);
        std::memcpy(&lap, a.data() + (P.ck_lap - P.ck_flags), 4);
        // Goal bit (4.0.0 main+0x8810c0 `orr w8,w8,#1`; 3.0.3 main+0x908110 `orr r0,r0,#1`).
        r.finished = (flags & 1) != 0;
        if (rank >= MaxRacers)
            continue;
        r.rank = rank + 1;
        r.lap_raw = static_cast<int>(std::min<u32>(lap, 99));
        // HUD rule (main+0x512cf4..0x512d0c; 3.0.3 main+0x55d384..): shown lap = min(v + 1, total).
        r.lap = s.laps_total > 0 ? std::min(r.lap_raw + 1, s.laps_total) : r.lap_raw + 1;
        r.coins = a[P.ck_coins - P.ck_flags];
        r.valid = true;
        // Position: KU[i] = [KD+units+ptr*i]; its vehicle's player id must be i (KU vtable slot 0).
        if (!kd_ok || i >= kd_count)
            continue;
        const u64 ku = WordIn(kdb.data() + pw * static_cast<std::size_t>(i));
        std::array<u8, 0x20> vmb{};
        const std::size_t vm_len = P.ku_move + pw - P.ku_vehicle;
        if (!ku || !ReadBlock(ku + P.ku_vehicle, vmb.data(), vm_len))
            continue;
        const u64 veh = WordIn(vmb.data()), mv = WordIn(vmb.data() + (P.ku_move - P.ku_vehicle));
        if (!veh || !mv)
            continue;
        const auto player = Read<u32>(veh + P.vehicle_player);
        if (!player || *player != static_cast<u32>(i))
            continue;
        std::array<float, 3> p{}, q{};
        bool same = false;
        for (int attempt = 0; attempt < 3 && !same; ++attempt)
            same = ReadBlock(mv + P.move_position, p.data(), sizeof(p)) &&
                   ReadBlock(mv + P.move_position, q.data(), sizeof(q)) && p == q;
        if (!same || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            continue;
        r.pos_ok = true;
        move_debug[static_cast<std::size_t>(i)] = mv;
        ku_debug[static_cast<std::size_t>(i)] = ku;
        r.x = p[0];
        r.y = p[1];
        r.z = p[2];
    }
    s.racers_ok = true;
}

void Reader::ReadItems(Snapshot& s, u64 oe) {
    // Owners: the item director creates owner[i] = new Owner(i) for i < RI+0x180 and appends it
    // to [ID+owners] (4.0.0 main+0x2b28..0x2b64, 3.0.3 main+0x2ea4); the Owner ctor stores i at
    // +owner_index (4.0.0 main+0x3f700 +0x40, 3.0.3 main+0x41c10 +0x24). So owner[i] belongs to
    // racer i; the reader requires u32 owner+owner_index == i.
    const Profile& P = *prof;
    const u64 pw = P.ptr;
    const auto id = Ptr(oe + P.oe_id);
    std::vector<u8> idb(P.id_owners + pw - P.id_owner_count);
    if (!id || !ReadBlock(*id + P.id_owner_count, idb.data(), idb.size())) {
        s.diag += " items:director";
        return;
    }
    u32 owners{};
    std::memcpy(&owners, idb.data(), 4);
    const u64 arr = WordIn(idb.data() + (P.id_owners - P.id_owner_count));
    if (owners < static_cast<u32>(s.count) || owners > MaxRacers || !arr) {
        s.diag += " items:owners";
        return;
    }
    std::array<u8, MaxRacers * 8> iob{};
    if (!ReadBlock(arr, iob.data(), static_cast<std::size_t>(s.count) * pw)) {
        s.diag += " items:array";
        return;
    }
    const std::size_t ob_len = P.io_slots + 2 * pw - P.owner_index;
    const std::size_t slot_len = P.slot_block_end - P.slot_block;
    int torn = 0;
    for (int i = 0; i < s.count; ++i) {
        Racer& r = s.racers[static_cast<std::size_t>(i)];
        const u64 io = WordIn(iob.data() + pw * static_cast<std::size_t>(i));
        std::array<u8, 0x40> ob{}; // +index, .., slots
        if (!io || !ReadBlock(io + P.owner_index, ob.data(), ob_len))
            continue;
        u32 index{};
        std::memcpy(&index, ob.data(), 4);
        const u64 s0 = WordIn(ob.data() + (P.io_slots - P.owner_index)),
                  s1 = WordIn(ob.data() + (P.io_slots + pw - P.owner_index));
        if (index != static_cast<u32>(i) || !s0 || !s1)
            continue;
        std::array<u64, 2> slots{s0, s1};
        bool ok = true;
        for (std::size_t k = 0; k < 2 && ok; ++k) {
            std::array<u8, 0x100> a{}, b{};
            bool stable = false;
            for (int attempt = 0; attempt < 3 && !stable; ++attempt)
                stable = ReadBlock(slots[k] + P.slot_block, a.data(), slot_len) &&
                         ReadBlock(slots[k] + P.slot_block, b.data(), slot_len) && a == b;
            if (!stable) {
                ++torn;
                ok = false;
                break;
            }
            ItemSlot& out = r.items[k];
            out.state = a[P.slot_state - P.slot_block];
            s32 item{};
            u32 count{};
            std::memcpy(&item, a.data() + (P.slot_item - P.slot_block), 4);
            std::memcpy(&count, a.data() + (P.slot_count - P.slot_block), 4);
            if (out.state > 3) {
                ok = false;
                break;
            }
            const auto valid_item = [](s32 v) { return v >= 0 && v < Mk8dIds::ItemCount; };
            out.item = out.state != 0 && valid_item(item) ? item : -1;
            out.count = out.state == 3 ? static_cast<int>(std::min<u32>(count, 99)) : 0;
            out.ok = true;
        }
        r.items_ok = ok;
        if (!ok)
            r.items = {};
        if (i == s.me && ok) {
            s.dbg_s0 = s0;
            s.dbg_s1 = s1;
        }
    }
    if (torn)
        s.diag += " items:torn";
    if (s.me >= 0 && s.racers[static_cast<std::size_t>(s.me)].items_ok) {
        s.items = s.racers[static_cast<std::size_t>(s.me)].items;
        s.items_ok = true;
    }
}

Snapshot Reader::ReadSnapshot() {
    Snapshot s;
    if (!code_ok) {
        s.diag = code_diag;
        return s;
    }
    const Profile& P = *prof;
    const u64 pw = P.ptr;
    // Context key: every pointer the domains hang off, read again after the reads.
    struct Key {
        u64 root{}, scene{}, ri{}, oe{}, rd{};
        u32 kind{};
        bool operator==(const Key&) const = default;
    };
    const auto resolve = [&](Key& k, std::string& why) {
        const auto var = Ptr(root_cell);
        const auto root = var && InMain(*var, pw) ? Ptr(*var) : std::nullopt;
        if (!root) {
            why += " root";
            return false;
        }
        k.root = *root;
        const auto engine = Ptr(k.root + P.root_engine);
        const auto holder = engine ? Ptr(*engine + P.engine_raceinfo_holder) : std::nullopt;
        const auto ri = holder ? Ptr(*holder + P.holder_raceinfo) : std::nullopt;
        if (!ri) {
            why += " raceinfo";
            return false;
        }
        k.ri = *ri;
        const auto sh = Ptr(k.root + P.root_scene_holder);
        const auto scene = sh ? Ptr(*sh + P.scene_holder_scene) : std::nullopt;
        std::array<u32, 2> sk{};
        if (scene && ReadBlock(*scene + P.scene_flags, sk.data(), sizeof(sk)) &&
            (sk[0] & SceneActiveBit)) {
            k.scene = *scene;
            k.kind = sk[1];
        }
        if (k.scene && k.kind == SceneRace) {
            const auto oe = Ptr(k.scene + P.scene_object_engine);
            const auto rd = oe ? Ptr(*oe + P.oe_rd) : std::nullopt;
            if (oe && rd) {
                k.oe = *oe;
                k.rd = *rd;
            }
        }
        return true;
    };
    Key key;
    if (!resolve(key, s.diag))
        return s;
    std::vector<u8> ri(RiBlock);
    if (!ReadBlock(key.ri, ri.data(), ri.size())) {
        s.diag += " raceinfo-block";
        return s;
    }
    const auto u32at = [&](u64 off) {
        u32 v{};
        std::memcpy(&v, ri.data() + off, 4);
        return v;
    };
    s.scene = key.kind;
    s.dbg_ri = key.ri;
    const u32 course = u32at(RiCourse);
    s.course = course <= static_cast<u32>(Mk8dIds::MaxCourseRaw) ? static_cast<int>(course) : -1;
    if (s.course < 0 && ct_ok && course <= ctgp::MaxEntries) {
        // CTGP-DX course ids (123..186): the plugin's own table entry names folder, cup, text.
        if (ct_entry.raw != static_cast<int>(course) || !ct_entry.ok)
            ct_entry = ReadCtEntry(static_cast<int>(course));
        if (ct_entry.ok) {
            s.course = static_cast<int>(course);
            s.ct_course = true;
            s.cup = ct_entry.cup;
            s.course_msg = ct_entry.name_label;
        }
    }
    const u32 cls = u32at(RiClass);
    s.cc = cls <= 3 ? static_cast<int>(50 * cls + 50) : 0; // main+0x50cc78..: 50*v + 50
    s.mirror = ri[RiMirror] ? 1 : 0;
    s.mode = static_cast<int>(std::min<u32>(u32at(RiRule), 0x7fffffff));
    s.submode = static_cast<int>(std::min<u32>(u32at(RiSubRule), 0x7fffffff));
    s.ready = true;

    // Cup and course message (UI resource tables).
    if (tables.ok && s.course >= 0 && !s.ct_course) {
        const auto t = Ptr(tables.r3 + P.r3_cup_table);
        std::array<s32, 2> cs{};
        if (t &&
            ReadBlock(*t + CupOfCourse + 8 * static_cast<u64>(s.course), cs.data(), sizeof(cs)) &&
            cs[0] >= 0 && cs[0] < 64 && cs[1] >= 0 && cs[1] < 8) {
            std::array<s32, 3> entry{};
            if (ReadBlock(*t + CupStride * static_cast<u64>(cs[0]) +
                              CupSlotStride * static_cast<u64>(cs[1]),
                          entry.data(), sizeof(entry)) &&
                entry[0] == s.course) { // the entry must name this course back
                s.cup = cs[0];
                s.course_msg = entry[2];
            }
        }
    }

    if (key.rd) {
        std::vector<u8> rdb(P.rd_timer + pw - P.rd_sequence);
        if (ReadBlock(key.rd + P.rd_sequence, rdb.data(), rdb.size())) {
            const u64 seq = WordIn(rdb.data());
            const u64 lm = WordIn(rdb.data() + (P.rd_lap_manager - P.rd_sequence));
            const u64 tm = WordIn(rdb.data() + (P.rd_timer - P.rd_sequence));
            if (const auto phase = seq ? Read<u32>(seq + P.seq_phase) : std::nullopt)
                s.phase = static_cast<int>(std::min<u32>(*phase, 0x7fffffff));
            // main+0x5129d8 (3.0.3 main+0x55d064): laps default 3 when the lap manager is absent.
            if (!lm)
                s.laps_total = 3;
            else if (const auto laps = Read<u8>(lm + P.lm_laps))
                s.laps_total = *laps;
            std::array<u8, 8> t{};
            if (tm && ReadBlock(tm + P.tm_time, t.data(), t.size())) {
                u16 ms{};
                std::memcpy(&ms, t.data() + 6, 2);
                // Phase 0 (loading) still carries the timer's reset sentinel 9'59"999 and
                // uninitialised checkers (live: every rank index 1, coins 0); the race state is
                // real from phase 1 (grid order, starting coins, timer 0).
                if (s.phase >= RacePhaseInitialized && t[5] < 60 && ms < 1000)
                    s.time_ms = (static_cast<s64>(t[4]) * 60 + t[5]) * 1000 + ms;
            }
        } else {
            s.diag += " rd-block";
        }
        if (s.phase >= RacePhaseInitialized && racers_code_ok)
            ReadRacers(s, key.oe, ri, key.rd);
        else
            s.diag += s.phase >= RacePhaseInitialized ? " racers:code" : " phase<1";
        if (s.racers_ok) {
            std::string why;
            s.me = ReadLocalSlot(key.root, s.count, why);
            s.diag += why;
            if (s.me >= 0) {
                s.dbg_ck_me = ck_debug[static_cast<std::size_t>(s.me)];
                s.dbg_move_me = move_debug[static_cast<std::size_t>(s.me)];
            }
            if (items_code_ok)
                ReadItems(s, key.oe);
            else
                s.diag += " items:code";
        }
    }

    // Torn-read rejection: the context must be unchanged after all reads.
    Key again;
    std::string ignored;
    std::vector<u8> ri2(RiBlock);
    if (!resolve(again, ignored) || !(again == key) || !ReadBlock(key.ri, ri2.data(), ri2.size()) ||
        std::memcmp(ri.data(), ri2.data(), RiEntries) != 0 ||
        std::memcmp(ri.data() + RiCount, ri2.data() + RiCount, RiBlock - RiCount) != 0) {
        Snapshot torn;
        torn.diag = "torn (context changed during the read)";
        return torn;
    }
    return s;
}

void Reader::LoadCourseAssets(int course) {
    assets_course = course;
    item_keys_ready = false; // the coin icon depends on the course (main+0x4234b0)
    course_folder.clear();
    course_name.clear();
    map_key.clear();
    pict_key.clear();
    cup_key.clear();
    if (course < 0)
        return;
    if (!tables.ok) {
        assets_course = -2; // the names are not read yet: load again once they are
        return;
    }
    if (ct_entry.ok && ct_entry.raw == course && course > Mk8dIds::MaxCourseRaw) {
        // CTGP-DX course: folder and picture base from the plugin's table entry (+0, +4); the
        // picture base carries the same "_00" the game's own table does (CT_00_00).
        course_folder = ct_entry.folder;
        map_key = Mk8dAssets::Library::MapKey(course_folder);
        const std::string& p = ct_entry.pict;
        if (p.size() > 3 && p.compare(p.size() - 3, 3, "_00") == 0)
            pict_key = Mk8dAssets::Library::CoursePictKey(p.substr(0, p.size() - 3));
        return;
    }
    course_folder = std::string{Mk8dIds::CourseFolder(tables.course_tokens, course)};
    if (!course_folder.empty())
        map_key = Mk8dAssets::Library::MapKey(course_folder);
    const std::string pict = Mk8dIds::CoursePictName(tables.pict_names, course);
    if (!pict.empty())
        pict_key = Mk8dAssets::Library::CoursePictKey(pict);
}

void Reader::Project(Snapshot& s) {
    if (!want_map || s.scene != SceneRace || course_folder.empty())
        return;
    // The camera file and the map fit box (a texture decode) are loaded on a worker thread
    // (Library is thread-safe), one course at a time. A course change while a load runs is
    // loaded when it ends, so the tick never waits (dropping a running std::async future would
    // block until the load finishes). camera / map_fit belong to camera_folder.
    if (camera_job.valid() &&
        camera_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        CourseMap cm = camera_job.get();
        camera = std::move(cm.camera);
        map_fit = cm.fit;
        camera_folder = camera_job_folder;
        map_fit_key = map_fit ? Mk8dAssets::Library::MapFitKey(camera_folder) : std::string{};
    }
    if (!camera_job.valid() && camera_folder != course_folder) {
        camera.reset();
        map_fit.reset();
        map_fit_key.clear();
        camera_folder.clear();
        camera_job_folder = course_folder;
        const EdenDsmodHostApi api = host;
        Mk8dAssets::Library* lib = &assets;
        camera_job = std::async(std::launch::async, [api, lib, folder = course_folder] {
            CourseMap out;
            out.camera = lib->LoadMapCamera(api, folder);
            out.fit = lib->MapFitBox(api, folder);
            return out;
        });
    }
    if (!camera || camera_folder != course_folder)
        return;
    const float yscale = Mk8dIds::MapWorldYScale(s.course, tables.y_scale_0x44);
    for (auto& r : s.racers)
        if (r.valid && r.pos_ok) {
            r.map_ok = camera->Project(r.x, r.y, r.z, r.map_x, r.map_y, yscale);
            // Position inside the fit box (mk8d_assets.h MapFit): (map - u0) / (u1 - u0).
            if (r.map_ok && map_fit && map_fit->u1 > map_fit->u0 && map_fit->v1 > map_fit->v0) {
                r.fit_x = (r.map_x - map_fit->u0) / (map_fit->u1 - map_fit->u0);
                r.fit_y = (r.map_y - map_fit->v0) / (map_fit->v1 - map_fit->v0);
                r.fit_ok = true;
            }
        }
}

// Debug-only verification aid (EDEN_DSMOD_MK8D_MAPPART_FILE names a text file holding the hex
// address of the game's minimap part, found with the console): in the same sample, read the
// layout position the game itself wrote for each racer's map icon (icon list [map+0xc0], icon
// player id s32 +0xb4, pane [[icon+0x40]+0x18] translate +0x30, main+0x50a960..0x50a9ac and
// main+0x8be504) and the module's own projection in the same units ((2u-1)*240 (+186 unless
// byte map+0x449), (1-2v)*240). Never used for any published contract value.
void Reader::DebugPaneCompare() {
    static const char* file = std::getenv("EDEN_DSMOD_MK8D_MAPPART_FILE");
    if (!file || !host.publish_f64 || !prof)
        return;
    u64 map = 0;
    if (FILE* f = std::fopen(file, "r")) {
        unsigned long long v = 0;
        if (std::fscanf(f, "%llx", &v) == 1)
            map = v;
        std::fclose(f);
    }
    const Profile& P = *prof;
    if (!P.map_icon_list || !P.pane_translate)
        return; // the pane chain is known only where the profile sets it
    const auto list = map ? Ptr(map + P.map_icon_list) : std::nullopt;
    const auto flag = map ? Read<u8>(map + P.map_tv_flag) : std::nullopt;
    if (!list || !flag)
        return;
    double worst = 0;
    int n = 0;
    for (int k = 0; k < current.count && k < MaxRacers; ++k) {
        const auto icon = Ptr(*list + P.ptr * static_cast<u64>(k));
        std::optional<s32> pid;
        if (icon && P.icon_pid) {
            pid = Read<s32>(*icon + P.icon_pid);
        } else if (icon && P.icon_ku) { // the icon's kart unit identifies the racer
            if (const auto ku = Word(*icon + P.icon_ku))
                for (int i = 0; i < current.count && i < MaxRacers; ++i)
                    if (*ku && ku_debug[static_cast<std::size_t>(i)] == *ku)
                        pid = i;
        }
        const auto part = icon ? Ptr(*icon + P.icon_part) : std::nullopt;
        const auto pane = part ? Ptr(*part + P.part_pane) : std::nullopt;
        std::array<float, 2> xy{};
        if (!pid || *pid < 0 || *pid >= current.count || !pane ||
            !ReadBlock(*pane + P.pane_translate, xy.data(), sizeof(xy)))
            continue;
        const Racer& r = current.racers[static_cast<std::size_t>(*pid)];
        if (!r.map_ok)
            continue;
        // Mirror: the game swaps the projection's left/right (main+0x507de4), so its icon x is
        // the negated texture-space x; the texture pane is scaled (-1, 1) to match.
        const double sx = current.mirror ? -1.0 : 1.0;
        const double cx = sx * (2.0 * r.map_x - 1.0) * 240.0 + (*flag ? 0.0 : 186.0);
        const double cy = (1.0 - 2.0 * r.map_y) * 240.0;
        char name[48];
        std::snprintf(name, sizeof(name), "mk.dbg.pane.r%d.game_x", *pid);
        host.publish_f64(host.userdata, name, xy[0]);
        std::snprintf(name, sizeof(name), "mk.dbg.pane.r%d.game_y", *pid);
        host.publish_f64(host.userdata, name, xy[1]);
        std::snprintf(name, sizeof(name), "mk.dbg.pane.r%d.calc_x", *pid);
        host.publish_f64(host.userdata, name, cx);
        std::snprintf(name, sizeof(name), "mk.dbg.pane.r%d.calc_y", *pid);
        host.publish_f64(host.userdata, name, cy);
        worst = std::max({worst, std::abs(cx - xy[0]), std::abs(cy - xy[1])});
        ++n;
    }
    dbg_pane_worst = worst;
    dbg_pane_n = n;
    dbg_pane_flag = *flag;
}

void Reader::SetWriteApi(const EdenDsmodHostWriteApi& api) {
    write_api = api;
    have_write = true;
}

// The local player's horn request byte: obj = [[SE+inputs]+array][p]+entry with p = PM+local[0]
// (4.0.0 [[SE+0x7f0]+0x58][p]+0x38, main+0x8bd450, byte obj+0x35d; 3.0.3 [[SE+0x520]+0x34][p]+0x1c,
// main+0x947744, byte obj+0x321), value 0 or 1.
u64 Reader::HornMaskAddress(std::string& why) const {
    const Profile& P = *prof;
    const auto var = Ptr(root_cell);
    const auto root = var && InMain(*var, P.ptr) ? Ptr(*var) : std::nullopt;
    const auto engine = root ? Ptr(*root + P.root_engine) : std::nullopt;
    const auto se = engine ? Ptr(*engine + P.engine_system) : std::nullopt;
    const auto pm = se ? Ptr(*se + P.se_player_manager) : std::nullopt;
    const auto local = pm ? Read<s32>(*pm + P.pm_local_obj) : std::nullopt;
    const auto inputs = se ? Ptr(*se + P.se_inputs) : std::nullopt;
    const auto n = inputs ? Read<u32>(*inputs + P.inputs_count) : std::nullopt;
    const auto arr = inputs ? Ptr(*inputs + P.inputs_array) : std::nullopt;
    if (!local || *local < 0 || !n || static_cast<u32>(*local) >= *n || !arr) {
        why = "horn:player";
        return 0;
    }
    const auto entry = Ptr(*arr + P.ptr * static_cast<u64>(*local));
    if (!entry) {
        why = "horn:entry";
        return 0;
    }
    const u64 at = *entry + P.input_entry_obj + P.input_horn_request;
    const auto now = Read<u8>(at);
    if (!now || *now > 1) {
        why = "horn:request";
        return 0;
    }
    return at;
}

bool Reader::HornAvailable() const {
    const Snapshot& s = current;
    return horn_code_ok && have_write && write_api.write_batch && s.phase == HornPhase &&
           s.racers_ok && s.me >= 0 && s.me < s.count &&
           s.racers[static_cast<std::size_t>(s.me)].valid;
}

bool Reader::OnAction(const char* action, std::int64_t) {
    if (!action || std::string_view{action} != "mk.horn_write")
        return false;
    if (horn_active || !HornAvailable())
        return false;
    horn_request = true;
    return true;
}

// One horn press: a guarded single-byte write 0 -> 1 of the request byte. The game turns it into
// one frame of action bit 8 and clears it itself; the kart's sound fires on the release edge
// (main+0xaf148). The press is re-armed (expect 0 -> 1) while it lasts, then an unconsumed
// request is taken back (expect 1 -> 0) so no stale press fires later. Every step first checks
// that the request byte is still the local player's (same chain, same address): after a scene
// change or a rebuilt input object the press ends without touching the old address.
void Reader::HornStep(u64 tick) {
    const auto write1 = [&](u8 expect, u8 value) {
        const EdenDsmodWriteOp op{horn_mask_at, 1, 0, &expect, &value};
        return have_write && write_api.write_batch &&
               write_api.write_batch(write_api.userdata, &op, 1) != 0;
    };
    if (horn_active) {
        std::string why;
        if (!current.ready || HornMaskAddress(why) != horn_mask_at) {
            horn_active = false; // the object is gone or moved: never write the old address
            return;
        }
        const auto now = Read<u8>(horn_mask_at);
        if (tick < horn_restore_tick && current.phase == HornPhase && current.me >= 0) {
            if (now && *now == 0)
                write1(0, 1); // consumed by the game: keep the press held
            return;
        }
        // Release: take back an unconsumed request (guarded). A refused write (the host was
        // pausing the guest) is retried for at most another HornHoldTicks.
        if (now && *now == 1 && !write1(1, 0) && tick < horn_restore_tick + HornHoldTicks)
            return;
        horn_active = false;
        ++horn_count;
        return;
    }
    if (!horn_request)
        return;
    horn_request = false;
    if (!HornAvailable())
        return;
    std::string why;
    const u64 at = HornMaskAddress(why);
    if (!at)
        return;
    horn_mask_at = at;
    if (!write1(0, 1))
        return;
    horn_active = true;
    horn_restore_tick = tick + HornHoldTicks;
}

void Reader::Shutdown() {
    if (!horn_active)
        return;
    horn_active = false;
    std::string why;
    if (!have_write || !write_api.write_batch || !prof || !code_ok ||
        HornMaskAddress(why) != horn_mask_at)
        return;
    const u8 one = 1, zero = 0;
    const EdenDsmodWriteOp op{horn_mask_at, 1, 0, &one, &zero};
    write_api.write_batch(write_api.userdata, &op, 1);
}

void Reader::Sample(const EdenDsmodHostApi& api) {
    const auto t0 = std::chrono::steady_clock::now();
    host = api;
    if (!code_resolved || host.main_base != base || host.main_size != size) {
        ResolveCode();
        tables = {};
        assets_course = -2;
    } else if (pin_recheck) {
        // Code can change after boot (3.0.3 + CTGP-DX hooks main from its plugin): the pins are
        // checked again on every scene change; a domain whose pins stop matching fails closed.
        pin_recheck = false;
        ResolveCode();
        if (!code_ok || !tables_code_ok) {
            tables = {};
            assets_course = -2; // names and keys derived from the tables go too
        }
    }
    const u64 tick = host.get_tick ? host.get_tick(host.userdata) : sample_count;
    ++sample_count;
    // Read at 30 Hz (every 2nd 60 Hz tick, the runtime's redraw rate); republish in between.
    if (!have_read || tick < last_read_tick || tick - last_read_tick >= 2) {
        last_read_tick = tick;
        have_read = true;
        // UI tables: resolve once (retry every 2 s until the UI resource exists).
        if (code_ok && tables_code_ok && ui_cell && !tables.ok && tick >= tables_retry_tick) {
            tables_retry_tick = tick + 120;
            const auto var = Ptr(ui_cell);
            const auto obj = var && InMain(*var, prof->ptr) ? Ptr(*var) : std::nullopt;
            const auto d = obj ? Ptr(*obj + prof->ui_object) : std::nullopt;
            const auto r3 = d ? Ptr(*d + prof->ui_resource3) : std::nullopt;
            if (r3)
                ResolveTables(*r3);
        }
        current = ReadSnapshot();
        if (current.scene != pin_scene) {
            pin_scene = current.scene;
            pin_recheck = true;
        }
        if (debug && host.log &&
            (current.scene != dbg_last_scene || current.phase != dbg_last_phase ||
             current.ready != dbg_last_ready)) {
            dbg_last_scene = current.scene;
            dbg_last_phase = current.phase;
            dbg_last_ready = current.ready;
            std::string ranks;
            for (int i = 0; i < current.count; ++i) {
                const Racer& r = current.racers[static_cast<std::size_t>(i)];
                ranks += (i ? "," : "") + std::to_string(r.valid ? r.rank : 0) + "/" +
                         std::to_string(r.driver);
            }
            char line[512];
            std::snprintf(line, sizeof(line),
                          "MK8D dbg transition tick=%llu ready=%d scene=%u phase=%d time_ms=%lld "
                          "course=%d mode=%d me=%d laps=%d rank/driver=[%s] diag=%s",
                          static_cast<unsigned long long>(tick), current.ready ? 1 : 0,
                          current.scene, current.phase, static_cast<long long>(current.time_ms),
                          current.course, current.mode, current.me, current.laps_total,
                          ranks.c_str(), current.diag.c_str());
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        if (current.ready && current.course != assets_course)
            LoadCourseAssets(current.course);
        if (current.ready)
            Project(current);
        if (debug)
            DebugPaneCompare();
    }
    HornStep(tick);
    Publish();
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    cost_sum_us += us;
    cost_max_us = std::max(cost_max_us, us);
    if (++cost_n == 600) {
        cost_avg_published = cost_sum_us / static_cast<double>(cost_n);
        // One line per 10 s window with EDEN_DSMOD_MK8D_DEBUG=1, else the first window and then
        // one per 10 minutes (mk.sample_us carries the current average either way).
        if (host.log && (debug || cost_windows++ % 60 == 0)) {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "MK8D reader: sample avg %.1f us, max %.1f us over %llu samples",
                          cost_avg_published, cost_max_us, static_cast<unsigned long long>(cost_n));
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line);
        }
        cost_sum_us = cost_max_us = 0;
        cost_n = 0;
    }
}

// Item id -> module:mk8d:item/<name> for the current course: rodata frame table (main+0x4234a8)
// with the course's coin override, then the ItemPtn animation of the player's rc_L_ItemBox_00.
// Kart emblem code of a driver (4.0.0 main+0x86ba7c..0x86cff8, 3.0.3 main+0x8f2a0c..0x8f4058):
// DB = [[[GOT]]+db_root]; the driver's record = [DB+records] + id*0x244 when [DB+present] and
// [DB+records] are set and id < u16 DB+count; code = 4.0.0: string pool [[GOT]] + u32 rec+0x10,
// 3.0.3: the char* u32 rec+0x10; Mii (0x1d, 0x34) -> "Mii"; if bit 1 of u32 rec+0x60 is set and
// id != 0x10, "%02d" of the colour variant is appended.
std::string Reader::EmblemCode(int driver, int variant) const {
    if (driver == 0x1d || driver == 0x34)
        return "Mii";
    if (driver < 0 || !prof || !param_db_cell || (!prof->db_code_is_ptr && !string_pool_cell))
        return {};
    const Profile& P = *prof;
    const auto var = Ptr(param_db_cell);
    const auto obj = var && InMain(*var, P.ptr) ? Ptr(*var) : std::nullopt;
    const auto db = obj ? Ptr(*obj + P.db_root) : std::nullopt;
    if (!db)
        return {};
    const auto has = Word(*db + P.db_present);
    const auto recs = Ptr(*db + P.db_records);
    const auto count = Read<u16>(*db + P.db_count);
    if (!has || !*has || !recs || !count || driver >= *count)
        return {};
    const u64 rec = *recs + DriverRecordSize * static_cast<u64>(driver);
    const auto off = Read<u32>(rec + P.db_code);
    const auto flags = Read<u32>(rec + RecordFlags);
    if (!off || !*off || !flags)
        return {};
    u64 at = *off;
    if (!P.db_code_is_ptr) {
        const auto pvar = Ptr(string_pool_cell);
        const auto pool = pvar && InMain(*pvar, P.ptr) ? Ptr(*pvar) : std::nullopt;
        if (!pool)
            return {};
        at = *pool + *off;
    }
    auto code = ReadCString(at, 32);
    if (!code || code->empty())
        return {};
    for (const char c : *code)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return {};
    if (((*flags >> 1) & 1) && driver != 0x10) {
        if (variant < 0 || variant > 99)
            return {};
        char digits[4];
        std::snprintf(digits, sizeof(digits), "%02d", variant);
        *code += digits;
    }
    return *code;
}

void Reader::PollPreload() {
    if (!preload_started) {
        preload_started = true;
        const EdenDsmodHostApi api = host;
        Mk8dAssets::Library* lib = &assets;
        const bool names_wanted = want_names;
        preload_job = std::async(std::launch::async, [api, lib, names_wanted] {
            Preload out;
            if (names_wanted)
                out.common = lib->LoadMessages(api, "USen", "Common");
            // rc_L_ItemBox_00.szs (a Yaz0 SARC without names): member
            // anim/rc_L_ItemBox_00_ItemPtn.bflan, found by its SFAT hash.
            const char* path = "UI/cmn/race.sarc#rc_L_ItemBox_00.szs";
            const std::size_t n =
                api.read_romfs ? api.read_romfs(api.userdata, path, 0, nullptr, 0) : 0;
            if (n > 0 && n < (8u << 20)) {
                std::vector<u8> szs(n);
                if (api.read_romfs(api.userdata, path, 0, szs.data(), szs.size()) == n)
                    if (auto sarc = Mk8dAssets::Yaz0Decompress(szs))
                        if (auto m =
                                Mk8dAssets::SarcMember(*sarc, "anim/rc_L_ItemBox_00_ItemPtn.bflan"))
                            out.pattern = Mk8dIds::ParseItemPattern(m->data(), m->size());
            }
            return out;
        });
    }
    if (!preload_done && preload_job.valid() &&
        preload_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Preload p = preload_job.get();
        common = std::move(p.common);
        item_pattern = std::move(p.pattern);
        preload_done = true;
        item_keys_ready = false;    // rebuild with the pattern
        racer_icon_driver.fill(-2); // re-resolve names now that the text is here
    }
}

void Reader::BuildItemKeys() {
    item_keys_ready = true;
    item_keys = {};
    if (!item_pattern)
        return;
    for (int item = 0; item < Mk8dIds::ItemCount; ++item)
        for (int uses = 0; uses < 4; ++uses) {
            const int frame = Mk8dIds::ItemFrame(tables.item_frames, item, assets_course, uses);
            const std::string icon = Mk8dIds::ItemIconName(*item_pattern, frame);
            if (!icon.empty())
                item_keys[static_cast<std::size_t>(item)][static_cast<std::size_t>(uses)] =
                    Mk8dAssets::Library::ItemKey(icon);
        }
}

const std::string& Reader::ItemKeyOf(const ItemSlot& slot) const {
    static const std::string none;
    if (!item_keys_ready || slot.item < 0 || slot.item >= Mk8dIds::ItemCount)
        return none;
    // While the roulette runs (state 1..2) the count is not set yet: the table frame applies.
    const int uses = slot.state == 3 ? std::clamp(slot.count, 0, 3) : 3;
    return item_keys[static_cast<std::size_t>(slot.item)][static_cast<std::size_t>(uses)];
}

void Reader::Publish() {
    const auto I = [&](const char* n, s64 v) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, n, v);
    };
    const auto F = [&](const char* n, double v) {
        if (host.publish_f64)
            host.publish_f64(host.userdata, n, v);
    };
    const auto T = [&](const char* n, const std::string& v) {
        if (host.publish_text)
            host.publish_text(host.userdata, n, v.c_str());
    };
    const Snapshot& s = current;
    std::string diag = s.diag;
    if (!code_diag.empty() && diag.find(code_diag) == std::string::npos)
        diag = code_diag + (diag.empty() ? "" : ";" + diag);
    if (!ct_diag.empty())
        diag += (diag.empty() ? "" : " ") + ct_diag;
    if (!tables.ok && !tables_diag.empty())
        diag += (diag.empty() ? "" : " ") + tables_diag;
    T("mk.diag", diag);
    if (debug) {
        I("mk.dbg.ri", static_cast<s64>(s.dbg_ri));
        I("mk.dbg.rd", static_cast<s64>(s.dbg_rd));
        I("mk.dbg.ck_me", static_cast<s64>(s.dbg_ck_me));
        I("mk.dbg.move_me", static_cast<s64>(s.dbg_move_me));
        I("mk.dbg.s0", static_cast<s64>(s.dbg_s0));
        I("mk.dbg.s1", static_cast<s64>(s.dbg_s1));
        F("mk.dbg.pane_maxdiff", dbg_pane_worst);
        I("mk.dbg.pane_n", dbg_pane_n);
        I("mk.dbg.pane_flag449", dbg_pane_flag);
        I("mk.dbg.patch_windows", ctgp_windows);
        T("mk.dbg.tables_capped", tables_capped);
        I("mk.dbg.ct_base", static_cast<s64>(ct_base));
        I("mk.dbg.ct_ctx", static_cast<s64>(ct_ctx));
        I("mk.dbg.lap_raw_me", s.me >= 0 ? s.racers[static_cast<std::size_t>(s.me)].lap_raw : -1);
        for (int i = 0; i < MaxRacers; ++i) {
            const Racer& r = s.racers[static_cast<std::size_t>(i)];
            char n[32];
            std::snprintf(n, sizeof(n), "mk.dbg.r%d.x", i);
            F(n, r.x);
            std::snprintf(n, sizeof(n), "mk.dbg.r%d.y", i);
            F(n, r.y);
            std::snprintf(n, sizeof(n), "mk.dbg.r%d.z", i);
            F(n, r.z);
        }
    }
    F("mk.sample_us", cost_avg_published);
    I("mk.ready", s.ready);
    I("mk.scene", s.scene);
    I("mk.phase", s.phase);
    I("mk.course", s.course);
    I("mk.cup", s.cup);
    I("mk.cc", s.cc);
    I("mk.mirror", s.mirror);
    I("mk.mode", s.mode);
    I("mk.submode", s.submode);
    I("mk.laps_total", s.laps_total);
    I("mk.time_ms", s.time_ms);
    // Names and image keys of the context.
    const bool ctx = s.ready && s.course >= 0 && s.course == assets_course;
    T("mk.course_folder", ctx ? course_folder : std::string{});
    T("mk.map_key", ctx ? map_key : std::string{});
    const bool fit = ctx && map_fit && camera_folder == course_folder;
    I("mk.mapfit_ok", fit);
    T("mk.mapfit_key", fit ? map_fit_key : std::string{});
    T("mk.pict_key", ctx ? pict_key : std::string{});
    std::string cup;
    const bool ct = s.ct_course && ct_entry.ok && ct_entry.raw == s.course;
    if (ct) {
        // CTGP-DX cup: icon name from the plugin's cup object (+8), e.g. "Boo".
        if (!ct_entry.cup_icon.empty())
            cup = Mk8dAssets::Library::CupKey(ct_entry.cup_icon);
    } else if (s.cup >= 0 && tables.ok) {
        const int index = std::min(s.cup, CupNameEntries - 1); // main+0x50d184 clamp
        if (static_cast<std::size_t>(index) < tables.cup_names.size())
            cup = Mk8dAssets::Library::CupKey(tables.cup_names[static_cast<std::size_t>(index)]);
    }
    T("mk.cup_key", cup);
    PollPreload();
    if (ct_ok && !ctgp_text_started && want_names) {
        // The plugin adds CTGPText.msbt to every message.sarc; its labels (5001xx course names,
        // 7000xx cup names) resolve there. Loaded once, off the tick thread.
        ctgp_text_started = true;
        const EdenDsmodHostApi api = host;
        Mk8dAssets::Library* lib = &assets;
        ctgp_text_job = std::async(
            std::launch::async, [api, lib] { return lib->LoadMessages(api, "USen", "CTGPText"); });
    }
    if (ctgp_text_job.valid() &&
        ctgp_text_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        ctgp_text = ctgp_text_job.get();
    std::string cname;
    if (s.course_msg > 0)
        if (const auto* text = ct       ? LabelText(s.course_msg)
                               : common ? common->Find(std::to_string(s.course_msg))
                                        : nullptr)
            cname = *text;
    T("mk.course_name", cname);
    // Cup name: main+0x41fc00 = rodata u32[cup] (24 entries, -1 for a negative cup id); CTGP-DX
    // cups: the plugin's cup object label (+4).
    std::string cupname;
    if (ct) {
        if (const auto* text = LabelText(ct_entry.cup_label))
            cupname = *text;
    } else if (common && s.cup >= 0 && static_cast<std::size_t>(s.cup) < tables.cup_msgs.size() &&
               tables.cup_msgs[static_cast<std::size_t>(s.cup)] > 0) {
        if (const auto* text =
                common->Find(std::to_string(tables.cup_msgs[static_cast<std::size_t>(s.cup)])))
            cupname = *text;
    }
    T("mk.cup_name", cupname);

    if (!item_keys_ready && tables.ok && s.course >= 0 && s.course == assets_course)
        BuildItemKeys();
    // Racers.
    I("racers.count", s.racers_ok ? s.count : 0);
    const auto& want = names->want;
    for (int i = 0; i < MaxRacers; ++i) {
        const auto& n = names->rows[static_cast<std::size_t>(i)];
        const Racer& r = s.racers[static_cast<std::size_t>(i)];
        const bool v = s.racers_ok && i < s.count && r.valid;
        if (want[0])
            I(n.valid.c_str(), v);
        if (want[1])
            I(n.rank.c_str(), v ? r.rank : 0);
        if (want[2])
            I(n.driver.c_str(), s.racers_ok && i < s.count ? r.driver : -1);
        if (want[3])
            I(n.variant.c_str(), s.racers_ok && i < s.count ? r.variant : 0);
        if (want[4])
            I(n.lap.c_str(), v ? r.lap : 0);
        if (want[5])
            I(n.coins.c_str(), v ? r.coins : 0);
        if (want[6])
            I(n.is_me.c_str(), v && i == s.me);
        const bool m = v && r.map_ok;
        if (want[7])
            F(n.map_x.c_str(), m ? r.map_x : -1.0);
        if (want[8])
            F(n.map_y.c_str(), m ? r.map_y : -1.0);
        if (want[9])
            I(n.map_ok.c_str(), m);
        // (the local racer's entry also feeds me.emblem_key, which is always published)
        if ((want[10] || want[11] || want[20] || i == s.me) && s.racers_ok && i < s.count &&
            tables.ok &&
            (racer_icon_driver[static_cast<std::size_t>(i)] != r.driver ||
             racer_icon_variant[static_cast<std::size_t>(i)] != r.variant)) {
            racer_icon_driver[static_cast<std::size_t>(i)] = r.driver;
            racer_icon_variant[static_cast<std::size_t>(i)] = r.variant;
            const std::string icon =
                Mk8dIds::DriverIconName(tables.driver_names, r.driver, r.variant);
            racer_icon[static_cast<std::size_t>(i)] =
                icon.empty() ? std::string{} : Mk8dAssets::Library::CharaKey(icon);
            const std::string emblem = EmblemCode(r.driver, r.variant);
            racer_emblem[static_cast<std::size_t>(i)] =
                emblem.empty() ? std::string{} : Mk8dAssets::Library::EmblemKey(emblem);
            racer_name[static_cast<std::size_t>(i)].clear();
            if (want_names && common) {
                const int label = Mk8dIds::DriverLabel(tables.driver_msgs, r.driver);
                if (label > 0)
                    if (const auto* text = common->Find(std::to_string(label)))
                        racer_name[static_cast<std::size_t>(i)] = *text;
            }
        }
        // Items of every racer (same semantics as me.item0/1). item_state: 0 empty,
        // 1 roulette (slot state 1..2), 2 held (slot state 3).
        const bool it = v && r.items_ok;
        const auto state_of = [](int st) { return st == 3 ? 2 : (st == 1 || st == 2) ? 1 : 0; };
        if (want[12])
            I(n.item0.c_str(), it ? r.items[0].item : -1);
        if (want[13])
            I(n.item1.c_str(), it ? r.items[1].item : -1);
        if (want[14])
            I(n.item_state.c_str(), it ? state_of(r.items[0].state) : 0);
        if (want[15])
            I(n.item1_state.c_str(), it ? state_of(r.items[1].state) : 0);
        if (want[16])
            I(n.item0_count.c_str(), it ? r.items[0].count : 0);
        if (want[17])
            I(n.item1_count.c_str(), it ? r.items[1].count : 0);
        for (std::size_t k = 0; k < 2; ++k)
            if (want[18 + k]) {
                T(k ? n.item1_key.c_str() : n.item0_key.c_str(),
                  it ? ItemKeyOf(r.items[k]) : std::string{});
            }
        if (want[23])
            I(n.finished.c_str(), v && r.finished);
        if (want[21])
            F(n.mapf_x.c_str(), m && r.fit_ok ? r.fit_x : -1.0);
        if (want[22])
            F(n.mapf_y.c_str(), m && r.fit_ok ? r.fit_y : -1.0);
        const bool named = s.racers_ok && i < s.count && tables.ok;
        if (want[10])
            T(n.icon.c_str(), named ? racer_icon[static_cast<std::size_t>(i)] : std::string{});
        if (want[11])
            T(n.name.c_str(), named ? racer_name[static_cast<std::size_t>(i)] : std::string{});
        if (want[20])
            T(n.emblem_key.c_str(),
              named ? racer_emblem[static_cast<std::size_t>(i)] : std::string{});
    }

    // Local player.
    const bool me_ok = s.racers_ok && s.me >= 0 && s.me < s.count &&
                       s.racers[static_cast<std::size_t>(s.me)].valid;
    const Racer empty{};
    const Racer& me = me_ok ? s.racers[static_cast<std::size_t>(s.me)] : empty;
    I("me.ready", me_ok);
    // Horn action availability (pins + write extension + racing with a local player).
    I("mk.horn_ok", HornAvailable());
    I("mk.horn_busy", horn_active || horn_request);
    I("mk.horn_count", horn_count);
    T("me.emblem_key",
      me_ok && tables.ok ? racer_emblem[static_cast<std::size_t>(s.me)] : std::string{});
    I("me.slot", me_ok ? s.me : -1);
    I("me.rank", me.rank);
    I("me.finished", me.finished);
    I("me.lap", me.lap);
    I("me.coins", me.coins);
    const bool items = me_ok && s.items_ok;
    for (std::size_t k = 0; k < 2; ++k) {
        const ItemSlot& slot = s.items[k];
        const int item = items ? slot.item : -1;
        I(k ? "me.item1" : "me.item0", item);
        I(k ? "me.item1_count" : "me.item0_count", items ? slot.count : 0);
        I(k ? "me.item1_roulette" : "me.item_roulette",
          items && (slot.state == 1 || slot.state == 2));
        T(k ? "me.item1_key" : "me.item0_key", items ? ItemKeyOf(slot) : std::string{});
    }
}

} // namespace Mk8dReader
