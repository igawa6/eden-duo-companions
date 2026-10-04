// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "core/mods/modules/dsmod_module_sdk.h"
#include "p5r_task_walk.h"
namespace p5r_enemy {
using namespace dsmod_sdk::int_types;
struct Enemy {
    u64 unit{};
    std::uint32_t id{}, hp{}, sp{};
    std::uint16_t level{};
    std::string name;
};
struct Snapshot {
    bool active{}, ready{}, state_known{};
    u64 task{}, context{};
    unsigned count{};
    std::array<Enemy, 5> enemies{};
};
// Exact D4B1 build is gated by the module. The natural FIELD encounter uses scene4.
// Registered task identity and native actor predicate are authoritative here.
// `walk`: this sample's shared task-list walk (p5r_task_walk.h), replayed instead of a first walk
// of its own; the closing tear check still walks the lists live.
template <class Read, class Name>
Snapshot Sample(Read&& read, Name&& name, u64 main, const p5r_tasks::Walk* walk = nullptr) {
    Snapshot out;
    auto get = [&](u64 p, u64 off, auto& v) {
        constexpr u64 max = std::numeric_limits<u64>::max();
        return p && off <= max - p && sizeof(v) <= max - (p + off) && read(p + off, &v, sizeof(v));
    };
    u64 manager{};
    if (walk ? !walk->manager_ok : !get(main, 0x2209f50, manager))
        return out;
    if (walk)
        manager = walk->manager;
    constexpr std::array<u64, 2> heads{0x2c40, 0x2c30}, links{0x80, 0x70};
    auto find = [&]() -> std::pair<u64, bool> {
        u64 current_manager{};
        if (!get(main, 0x2209f50, current_manager) || current_manager != manager || !manager)
            return {0, false};
        bool complete = true;
        for (unsigned list = 0; list < 2; ++list) {
            u64 task{};
            if (!get(manager, heads[list], task)) {
                complete = false;
                continue;
            }
            std::array<u64, 256> seen{};
            for (unsigned n = 0; task && n < seen.size(); ++n) {
                bool cycle = false;
                for (unsigned j = 0; j < n; ++j)
                    cycle |= seen[j] == task;
                if (cycle) {
                    complete = false;
                    break;
                }
                seen[n] = task;
                u64 next{}, ptr{};
                std::uint32_t status{};
                std::array<char, 7> text{};
                if (!get(task, 0, status) || !get(task, links[list], next)) {
                    complete = false;
                    break;
                }
                if (status != 3) {
                    if (!get(task, 0x18, ptr) || !get(ptr, 0, text)) {
                        complete = false;
                        break;
                    }
                    if (!std::memcmp(text.data(), "battle", 7)) {
                        if (!get(main, 0x2209f50, current_manager) || current_manager != manager)
                            return {0, false};
                        return {task, true};
                    }
                }
                task = next;
            }
            if (task)
                complete = false;
        }
        if (!get(main, 0x2209f50, current_manager) || current_manager != manager)
            complete = false;
        return {0, complete};
    };
    // Replay of find() over the shared walk: identical reads, identical stop rules.
    auto replay = [&]() -> std::pair<u64, bool> {
        u64 current_manager{};
        if (!manager)
            return {0, false};
        bool complete = true;
        for (const auto& l : walk->lists) {
            if (!l.head_ok) {
                complete = false;
                continue;
            }
            bool stopped = false;
            for (unsigned n = 0; n < l.count; ++n) {
                const auto& e = l.entries[n];
                if (e.status == 3)
                    continue;
                if (e.name_state < p5r_tasks::Name7) {
                    stopped = true;
                    break;
                }
                if (!std::memcmp(e.name.data(), "battle", 7)) {
                    if (!get(main, 0x2209f50, current_manager) || current_manager != manager)
                        return {0, false};
                    return {e.task, true};
                }
            }
            if (stopped || l.end != p5r_tasks::EndNull)
                complete = false;
        }
        if (!get(main, 0x2209f50, current_manager) || current_manager != manager)
            complete = false;
        return {0, complete};
    };
    const auto located = walk ? replay() : find();
    const u64 battle = located.first;
    out.state_known = located.second;
    if (!battle)
        return out;
    out.state_known = false;
    u64 ctx{};
    std::uint32_t status{};
    if (!battle || !get(battle, 0, status) || status == 3 || !get(battle, 0x48, ctx) || !ctx)
        return out;
    out.task = battle;
    out.context = ctx;
    out.active = true;
    out.state_known = true;
    if (status != 2)
        return out;
    u64 head{}, node{};
    std::uint32_t count{};
    std::uint16_t name_count{};
    if (!get(ctx, 0x140, count) || count > 5 || !get(ctx, 0x130, head) ||
        !get(main, 0x22ab51c, name_count) || !name_count || name_count > 783)
        return out;
    node = head;
    struct Identity {
        u64 node{}, next{}, actor{}, controller{}, unit{}, vt{}, flags{};
        std::array<std::uint8_t, 0x1c> data{};
    };
    std::array<Identity, 5> identity{};
    Snapshot candidate;
    candidate.task = battle;
    candidate.context = ctx;
    candidate.active = true;
    candidate.state_known = true;
    auto rd_u32 = [](const auto& b, unsigned at) {
        std::uint32_t v{};
        std::memcpy(&v, b.data() + at, 4);
        return v;
    };
    auto rd_u16 = [](const auto& b, unsigned at) {
        std::uint16_t v{};
        std::memcpy(&v, b.data() + at, 2);
        return v;
    };
    for (unsigned i = 0; i < count; ++i) {
        if (!node)
            return out;
        for (unsigned j = 0; j < i; ++j)
            if (identity[j].node == node)
                return out;
        auto& x = identity[i];
        x.node = node;
        u64 predicate{};
        if (!get(node, 0, x.next) || !get(node, 0x28, x.actor) || !get(x.actor, 0, x.vt) ||
            !get(x.vt, 0x60, predicate) || main > std::numeric_limits<u64>::max() - 0x3ad934 ||
            predicate != main + 0x3ad934 || !get(x.actor, 8, x.flags) ||
            !get(x.actor, 0x30, x.controller) || !get(x.controller, 8, x.unit) ||
            !get(x.unit, 0, x.data))
            return out;
        for (unsigned j = 0; j < i; ++j)
            if (identity[j].actor == x.actor || identity[j].unit == x.unit)
                return out;
        node = x.next;
        if (rd_u16(x.data, 4) != 2)
            return out;
        const auto id = rd_u32(x.data, 8), hp = rd_u32(x.data, 12), sp = rd_u32(x.data, 16),
                   uf = rd_u32(x.data, 0), st = rd_u32(x.data, 20);
        const auto level = rd_u16(x.data, 24);
        if (id >= name_count || !level || level > 99 || hp > 999999 || sp > 999999)
            return out;
        // 3AD934 native validity: flags + lifecycle + conditional incapacitation.
        const bool valid = !(x.flags & 0x08000004) && bool(uf & 0x8000) &&
                           !(((st & 0x80000) || hp < 1) && (uf & 0x2000));
        if (!valid)
            continue;
        auto& e = candidate.enemies[candidate.count++];
        e.unit = x.unit;
        e.id = id;
        e.hp = hp;
        e.sp = sp;
        e.level = level;
        e.name = name(id);
    }
    if (node)
        return out; // Native singly linked list must end at its declared count.
    for (unsigned i = 0; i < count; ++i) {
        const auto& x = identity[i];
        u64 next{}, actor{}, controller{}, unit{}, vt{}, flags{};
        std::array<std::uint8_t, 0x1c> data{};
        if (!get(x.node, 0, next) || next != x.next || !get(x.node, 0x28, actor) ||
            actor != x.actor || !get(actor, 0, vt) || vt != x.vt || !get(actor, 8, flags) ||
            flags != x.flags || !get(actor, 0x30, controller) || controller != x.controller ||
            !get(controller, 8, unit) || unit != x.unit || !get(unit, 0, data) || data != x.data)
            return out;
    }
    u64 c{}, h{};
    std::uint32_t n{}, s{};
    if (!get(battle, 0, s) || s != 2 || !get(battle, 0x48, c) || c != ctx || !get(ctx, 0x130, h) ||
        h != head || !get(ctx, 0x140, n) || n != count)
        return out;
    const auto final_location = find();
    if (final_location.first != battle || !final_location.second) {
        out.state_known = false;
        return out;
    }
    candidate.ready = true;
    return candidate;
}
} // namespace p5r_enemy
