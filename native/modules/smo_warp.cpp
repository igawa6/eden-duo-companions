// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_warp.h"
#include <string>

namespace SmoWarp {
namespace {
using U64 = std::uint64_t;
using S32 = std::int32_t;
constexpr U64 SceneVtable = 0x12C4DC8, MapStateVtable = 0x12CB708;
// These are Nerve OBJECT addresses, not their vtable address points.
constexpr U64 Wait = 0x12CBB08, WaitAdsorb = 0x12CBB30, EndWarp = 0x12CBB38;
// StageScene::exeWaitWarpToCheckpoint (4760b0) starts native takeoff, then
// exeWarpToCheckpoint (4763a0) consumes the selected native checkpoint record.
constexpr U64 Play = 0x12C5EB8, WaitWarpToCheckpoint = 0x12C5F48;
template <class T> bool Get(const EdenDsmodHostApi& h, U64 at, T& out) {
    return at && at <= UINT64_MAX - sizeof(T) && h.is_mapped && h.read_memory &&
           h.is_mapped(h.userdata, at, sizeof(T)) && h.read_memory(h.userdata, at, &out, sizeof(T));
}
bool Pointer(const EdenDsmodHostApi& h, U64 at, U64& out) {
    return Get(h, at, out) && out && out <= UINT64_MAX - 0x2000;
}
bool Text(const EdenDsmodHostApi& h, U64 at, std::string_view expected) {
    if (!at || expected.empty() || expected.size() > 127 || at > UINT64_MAX - 128) return false;
    for (size_t i = 0; i <= expected.size(); ++i) {
        std::uint8_t c = 0;
        if (!Get(h, at + i, c) || c != (i == expected.size() ? 0 : std::uint8_t(expected[i])))
            return false;
    }
    return true;
}
bool Vtable(const EdenDsmodHostApi& h, U64 at, U64 offset) {
    U64 v = 0;
    const auto delta=h.get_i64?h.get_i64(h.userdata,"__relocation_delta",0):0;
    return Get(h, at, v) && (v == h.main_base + offset || v == h.main_base + offset+static_cast<U64>(delta));
}
} // namespace

std::optional<Plan> Prepare(const EdenDsmodHostApi& h, U64 scene, U64 game, int world,
                            std::string_view stage, std::string_view id, bool touched,
                            unsigned* rejection_reason, bool direct) {
    if (rejection_reason) *rejection_reason = 0;
    const auto reject = [rejection_reason](unsigned reason) -> std::optional<Plan> {
        if (rejection_reason) *rejection_reason = reason;
        return std::nullopt;
    };
    if (!touched || !scene || !game || !h.main_base || h.main_base > UINT64_MAX - EndWarp ||
        scene > UINT64_MAX - 0x500 || game > UINT64_MAX - 0x2000 || world < 0 || world > 16 ||
        !stage.ends_with("WorldHomeStage") || id.empty() || id.size() > 127 ||
        !Vtable(h, scene, SceneVtable)) return reject(1);
    U64 scene_vtable = 0;
    if (!Get(h, scene, scene_vtable)) return reject(1);
    // Native singleton pointers use the actual relocated image, just like vtables.
    const U64 base = scene_vtable - SceneVtable;
    U64 holder = 0, game_again = 0, menu = 0, state = 0, owner = 0, map = 0, spine = 0;
    U64 live_stage = 0, selected = 0, current_nerve = 0, pending_nerve = 0;
    S32 current_world = -1, step = -1, mode = 0, scenario = -1, selector = -1;
    std::uint8_t enabled = 0, state_ended = 1;
    if (!Pointer(h, scene + 0x2E0, holder) || !Get(h, holder + 0x20, game_again) || game_again != game ||
        !Get(h, game + 0xA38, current_world) || current_world != world ||
        !Pointer(h, game + 0x330, live_stage) || !Text(h, live_stage, stage) ||
        !Get(h, holder + 0x238, enabled) || enabled != 1 ||
        !Get(h, game + 0xA8C, mode) || mode == 1 ||
        !Get(h, game + 0x624, mode) || mode == 1 ||
        !Get(h, game + 0x628, mode) || mode == 1) return reject(2);
    if (!Pointer(h, scene + 0x170, menu) || !Pointer(h, menu + 0x38, state) ||
        !Vtable(h, state, MapStateVtable) || !Get(h, state + 0x18, owner) || owner != scene ||
        !Get(h, state + 0x10, state_ended) || state_ended != (direct ? 1 : 0) ||
        !Pointer(h, state + 0x20, map) || !Pointer(h, (direct ? scene : state) + 8, spine)) return reject(3);
    if (!Get(h, spine, owner) || owner != (direct ? scene : state) ||
        !Get(h, state + 0x28, selected) || !Get(h, spine + 8, current_nerve) ||
        (direct ? current_nerve != base + Play :
            (current_nerve != base + Wait && current_nerve != base + WaitAdsorb)) ||
        !Get(h, spine + 0x10, pending_nerve) || pending_nerve != 0 ||
        !Get(h, spine + 0x18, step) || step < 1) return reject(4);
    if (!Get(h, game + 0xA70, selector) || !Get(h, game + 0xA6C, scenario)) return reject(5);
    // The known home stage belongs to WorldResource; native 4cc860 therefore
    // chooses A6C when no explicit A70 scenario override is active.
    if (selector == -1) selector = scenario;
    if (selector < 0 || selector > 31) return reject(5);
    U64 worlds = 0, checkpoints = 0;
    if (!Pointer(h, game + 0xA30, worlds) || !Pointer(h, worlds + world * 8, checkpoints))
        return reject(6);
    int target_index = -1, filtered = 0;
    for (unsigned i = 0; i < 17; ++i) {
        const U64 entry = checkpoints + i * 0x148;
        std::uint32_t mask = 0;
        if (!Get(h, entry + 0x130, mask)) return reject(6);
        if (!(mask & (std::uint32_t(1) << selector))) continue;
        U64 stage_pointer = 0, id_pointer = 0;
        if (!Pointer(h, entry + 8, stage_pointer) || !Pointer(h, entry + 0xA0, id_pointer))
            return reject(6);
        if (Text(h, stage_pointer, stage) && Text(h, id_pointer, id)) {
            std::uint8_t native_touched = 0;
            // Native Checkpoint save-load 4c4c94 sets 140 only for an actual Checkpoint
            // record in save data. A companion fixture alone cannot authorize it.
            if (!Get(h, entry + 0x140, native_touched) || native_touched != 1) return reject(7);
            if (target_index >= 0) return reject(6);
            target_index = filtered;
        }
        ++filtered;
    }
    if (target_index < 0 || target_index >= 16 || filtered > 16) return reject(6);
    U64 icons = 0, records = 0, target_record = 0;
    S32 icon_count = 0, record_count = 0;
    if (!Pointer(h, map + 0x178, icons) || !Get(h, map + 0x180, icon_count) || icon_count != 16 ||
        !Pointer(h, map + 0x210, records) || !Get(h, map + 0x218, record_count) ||
        record_count < 1 || record_count > 512 || records > UINT64_MAX - 512 * 0x30)
        return reject(8);
    const U64 target_icon = icons + target_index * 16;
    S32 index_a = -1, index_b = -1;
    if (!Get(h, target_icon, index_a) || !Get(h, target_icon + 4, index_b) ||
        index_a != target_index || index_b != target_index) return reject(8);
    for (int i = 0; i < record_count; ++i) {
        const U64 record = records + i * 0x30;
        U64 icon = 0; S32 type = -1; std::uint8_t active = 0;
        if (!Get(h, record, icon) || !Get(h, record + 8, active) || !Get(h, record + 0xC, type))
            return reject(8);
        // Closed maps retain preallocated checkpoint records as inactive/type19.
        // Native 49dbf0 only distinguishes Odyssey(type2), and 49dba0 resolves
        // the checkpoint solely by this icon pointer. Their UI status need not
        // be changed to enter the scene's checkpoint sequence directly.
        const bool usable = (active == 1 && type == 0) ||
                            (direct && active == 0 && type == 19);
        if (icon == target_icon && usable) {
            if (target_record) return reject(8);
            target_record = record;
        }
    }
    if (!target_record) return reject(8);
    // Check ownership/state again immediately before returning the transaction.
    U64 check = 0;
    S32 step_after = -1;
    if (!Get(h, scene + 0x170, check) || check != menu || !Get(h, menu + 0x38, check) || check != state ||
        !Get(h, (direct ? scene : state) + 8, check) || check != spine ||
        !Get(h, state + 0x28, check) || check != selected ||
        !Get(h, spine + 8, check) || check != current_nerve ||
        !Get(h, spine + 0x10, check) || check != pending_nerve ||
        !Get(h, spine + 0x18, step_after) || step_after != step) return reject(9);
    Plan result;
    result.stage_scene = scene; result.game_data = game; result.map_state = state;
    result.record = target_record; result.spine = spine; result.checkpoint_index = target_index;
    result.writes = {{{state + 0x28, selected, target_record, 8},
                      {spine + 0x10, 0, base + (direct ? WaitWarpToCheckpoint : EndWarp), 8},
                      {spine + 0x18, std::uint32_t(step), UINT32_MAX, 4}}};
    return result;
}
} // namespace SmoWarp
