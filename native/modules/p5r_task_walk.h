// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// One walk of the game's two registered-task lists (task manager main+2209F50, heads +0x2C40 /
// +0x2C30, links +0x80 / +0x70) shared by every per-sample task-name lookup that used to walk
// them on its own (field battle probe p5r_enemy::Sample, camp-menu probe CAMP_MAIN). The walk
// records exactly what each reproduced lookup read (status, link, name pointer, name bytes) so
// each lookup can replay its own stop rules over the recorded entries.
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace p5r_tasks {
using namespace dsmod_sdk::int_types;
constexpr u64 Manager = 0x2209f50;
constexpr std::array<u64, 2> Heads{0x2c40, 0x2c30}, Links{0x80, 0x70};
constexpr unsigned MaxTasks = 256;

enum NameState : std::uint8_t {
    NameSkipped = 0, // status 3 (dying): no lookup reads the name
    NamePtrFailed,   // task+0x18 unreadable
    NameFailed,      // not even 7 name bytes readable
    Name7,           // 7 bytes readable, 10 not
    Name10,          // 10 bytes readable
};
struct Entry {
    u64 task{}, next{};
    std::uint32_t status{};
    NameState name_state{};
    std::array<char, 10> name{};
};
enum End : std::uint8_t {
    EndNull = 0,   // reached a null link
    EndReadFailed, // status / link unreadable at entries[count]
    EndCycle,      // the next task was already visited
    EndLimit,      // MaxTasks entries without an end
};
struct List {
    bool head_ok{};
    End end{};
    unsigned count{};
    std::array<Entry, MaxTasks> entries;
};
struct Walk {
    bool manager_ok{}; // manager read succeeded
    u64 manager{};
    std::array<List, 2> lists;
};

template <class Read>
void Take(Read&& read, u64 main, Walk& w) {
    auto get = [&](u64 p, u64 off, auto& v) {
        constexpr u64 max = std::numeric_limits<u64>::max();
        return p && off <= max - p && sizeof(v) <= max - (p + off) && read(p + off, &v, sizeof(v));
    };
    w.manager = 0;
    w.manager_ok = get(main, Manager, w.manager);
    for (auto& l : w.lists) {
        l.head_ok = false;
        l.end = EndNull;
        l.count = 0;
    }
    if (!w.manager_ok || !w.manager)
        return;
    for (unsigned list = 0; list < 2; ++list) {
        auto& l = w.lists[list];
        u64 task{};
        l.head_ok = get(w.manager, Heads[list], task);
        if (!l.head_ok)
            continue;
        for (;;) {
            if (!task) {
                l.end = EndNull;
                break;
            }
            if (l.count >= MaxTasks) {
                l.end = EndLimit;
                break;
            }
            bool cycle = false;
            for (unsigned j = 0; j < l.count; ++j)
                cycle |= l.entries[j].task == task;
            if (cycle) {
                l.end = EndCycle;
                break;
            }
            Entry& e = l.entries[l.count];
            e = Entry{};
            e.task = task;
            // One read of the task header (status +0, name +0x18, link +0x70/+0x80) instead of
            // three; the per-field reads below remain the path when it is refused.
            std::array<std::uint8_t, 0x88> head{};
            const size_t head_size = Links[list] + 8;
            const bool bulk = task <= std::numeric_limits<u64>::max() - head_size &&
                              read(task, head.data(), head_size);
            u64 ptr{};
            bool ptr_ok = false;
            if (bulk) {
                std::memcpy(&e.status, head.data(), 4);
                std::memcpy(&e.next, head.data() + Links[list], 8);
                std::memcpy(&ptr, head.data() + 0x18, 8);
                ptr_ok = true;
            } else if (!get(task, 0, e.status) || !get(task, Links[list], e.next)) {
                l.end = EndReadFailed;
                break;
            }
            if (e.status != 3) {
                std::array<char, 7> short_name{};
                if (!ptr_ok && !get(task, 0x18, ptr))
                    e.name_state = NamePtrFailed;
                else if (get(ptr, 0, e.name))
                    e.name_state = Name10;
                else if (get(ptr, 0, short_name)) {
                    e.name_state = Name7;
                    std::memcpy(e.name.data(), short_name.data(), short_name.size());
                } else
                    e.name_state = NameFailed;
            }
            ++l.count;
            task = e.next;
        }
    }
}

// Replay of the camp-menu probe (Reader::Menu): a live CAMP_MAIN task (status != 3) in either list,
// each list walked until a null link, an unreadable task, a link back to the list head or to the
// task itself, or MaxTasks tasks.
inline bool HasCampMain(const Walk& w) {
    if (!w.manager_ok || !w.manager)
        return false;
    for (const auto& l : w.lists) {
        if (!l.head_ok || !l.count)
            continue;
        const u64 first = l.entries[0].task;
        for (unsigned n = 0; n < l.count; ++n) {
            const Entry& e = l.entries[n];
            if (e.status != 3 && e.name_state == Name10 &&
                std::memcmp(e.name.data(), "CAMP_MAIN", 10) == 0)
                return true;
            if (e.next == first || e.next == e.task)
                break;
        }
    }
    return false;
}
} // namespace p5r_tasks
