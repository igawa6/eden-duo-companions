// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/mods/modules/dsmod_module_sdk.h"
namespace p5r_analyze {
using namespace dsmod_sdk::int_types;
inline unsigned Category(std::uint32_t value, bool known) {
    if (!known)
        return 6;
    if (value & (1u << 27))
        return 5;
    if (value & (1u << 26))
        return 1;
    if (value & (1u << 24))
        return 3;
    if (value & (1u << 25))
        return 2;
    if (value & (1u << 28))
        return 4;
    return 0;
}
struct Frame {
    u64 task{}, context{}, ui_task{}, ui_context{}, unit{};
    std::uint32_t unit_id{};
    std::uint16_t displayed_id{}, hp{}, sp{};
    std::uint32_t index{}, count{};
    std::uint8_t level{};
    std::array<std::uint32_t, 10> effective{};
    std::array<bool, 10> known{};
};
// D4B1 native Analyze copy; live Agathion fixture matches all ten affinities.
// Caller must verify selectedunit belongs to current validated battle roster.
// registered(task) MUST use current bounded native task registry, not cached results.
// Unknown affinities are zeroed, never exposed through this result.
template <class Read, class Registered>
bool Sample(Read&& read, Registered&& registered, u64 task, Frame& output) {
    output = {};
    auto get = [&](u64 p, u64 off, auto& v) {
        constexpr u64 m = std::numeric_limits<u64>::max();
        return p && off <= m - p && sizeof(v) <= m - (p + off) && read(p + off, &v, sizeof(v));
    };
    auto alive = [&](u64 t) {
        std::uint32_t s{};
        return registered(t) && get(t, 0, s) && s == 2;
    };
    Frame f{};
    f.task = task;
    u64 nameptr{};
    std::array<char, 18> name{};
    std::int16_t phase{};
    std::int32_t selected{};
    std::uint32_t count{}, kind{};
    std::uint16_t type{};
    if (!alive(task) || !get(task, 0x18, nameptr) || !get(nameptr, 0, name) ||
        std::memcmp(name.data(), "BTL_ENEMY_ANALYZE", 17) || name[17] != 0 ||
        !get(task, 0x48, f.context) || !get(f.context, 2, phase) || phase != 4 ||
        !get(f.context, 4, selected) || selected < 0 || !get(f.context, 8, count) || count > 5 ||
        unsigned(selected) >= count || !get(f.context, 0x10 + unsigned(selected) * 8, f.unit) ||
        !get(f.unit, 4, type) || type != 2 || !get(f.unit, 8, f.unit_id) || f.unit_id >= 783 ||
        !get(f.context, 0x60, f.ui_task) || !alive(f.ui_task) ||
        !get(f.ui_task, 0x48, f.ui_context) || !get(f.ui_context, 8, kind) || kind != 6)
        return false;
    std::array<std::uint8_t, 0x74> model{}, again{};
    if (!get(f.ui_context, 0x3c, model))
        return false;
    std::uint32_t flags{};
    std::memcpy(&flags, model.data(), 4);
    f.displayed_id = std::uint16_t(model[6] | unsigned(model[7]) << 8);
    f.level = model[0xb];
    auto expected = f.unit_id;
    if ((expected >= 564 && expected <= 567) || expected == 569)
        expected = 598;
    f.index = static_cast<std::uint32_t>(selected);
    f.count = count;
    std::memcpy(&f.hp, model.data() + 0x6a, 2);
    std::memcpy(&f.sp, model.data() + 0x6e, 2);
    if (!(flags & 1) || f.displayed_id != expected || f.level < 1 || f.level > 99)
        return false;
    for (unsigned i = 0; i < 10; ++i) {
        if (model[0x54 + i] > 1)
            return false;
        f.known[i] = model[0x54 + i] == 0;
        if (f.known[i])
            std::memcpy(&f.effective[i], model.data() + 0x2c + i * 4, 4);
    }
    std::int16_t phase_after{};
    u64 c{}, u{}, uit{}, uic{};
    std::int32_t sel{};
    std::uint32_t n{}, id{}, k{};
    if (!alive(task) || !alive(f.ui_task) || !get(task, 0x48, c) || c != f.context ||
        !get(c, 2, phase_after) || phase_after != phase || !get(c, 4, sel) || sel != selected ||
        !get(c, 8, n) || n != count || !get(c, 0x10 + unsigned(sel) * 8, u) || u != f.unit ||
        !get(u, 8, id) || id != f.unit_id || !get(c, 0x60, uit) || uit != f.ui_task ||
        !get(uit, 0x48, uic) || uic != f.ui_context || !get(uic, 8, k) || k != 6 ||
        !get(uic, 0x3c, again) || again != model)
        return false;
    output = f;
    return true;
}
} // namespace p5r_analyze
