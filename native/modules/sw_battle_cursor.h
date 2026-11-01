// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// The NATIVE battle menu cursor (what the top screen highlights while the D-pad moves, before A),
// for the command menu (Fight/Pokemon/Bag/Run) and the move list. Live-found 2026-10-31 (label
// battle, gdbstub write watchpoints + 32 MiB heap diffs over D-pad presses, two battles):
//
//   view   = client+0x358            (the battle view; vtable+0xB8/+0xC0/+0xE0 open/poll menus)
//   slot   = *(*(view+0x5C8)+0x38)   (the "current sub-screen" handle Native99E6E0 resolves)
//   ui     = *(*(slot+0x10)+0x6D8)   (battle UI root, vtable main+0x2568630)
//   views  = ui+0x78 .. ui+0x80      (std::vector of the battle sub-views, 21 entries)
//   views[2]  = command list view   (vtable main+0x252DDB8)
//   views[13] = move select view    (vtable main+0x252DC10)
//   list   = *(command_view+0x630)   (generic grid list, vtable main+0x25BC8F0)
//   cursor = *(list+0x408)           (grid cursor, vtable main+0x25BD1F8): +8 index, +0xC columns,
//            +0x10 rows. The writer is the list's own move handler Native15B91F0 (str w21,[x9,#8])
//            and the touch hit-test Native15B8620; both then call Native15B7A20 (move visuals).
//   command cursor index = the fixed cell index of battle_commandSelect_00: 0 Fight, 1 Cheer On
//            (raid only, once your Pokemon fainted), 2 Pokemon, 3 Bag, 4 Run, 5 Back (a doubles
//            second position); hidden cells keep their index (CommandFromNative below).
//   move view +0x678 = the hovered move slot 0..3, written by the move list's cursor callback
//            Native938590 (move list = *(move_view+0x640), Max moves list = +0x648 when
//            move_view+0x61D != 0). Kept after the list closes, so it is only published while the
//            client state is the FIGHT state's input wait (Native803590, step 2).
//
// The command hover is only published while the client waits on the command menu
// (client+0x560 == Native8021D0 && +0x570 == 5). Every pointer of the chain is vtable-checked;
// any failure fails closed (-1), and the caller keeps its own companion-owned selection.
// The main offsets below (state functions, vtables) are the variant's (sw_variant.h kMain), the
// values the comments name are Sword 1.3.2's.

#pragma once

#include <cstdint>

#include "sw_variant.h"

namespace sw_battle_cursor {

inline constexpr std::uint64_t kViewOffset = 0x358;
inline constexpr std::uint64_t kStateFnOffset = 0x560;
inline constexpr std::uint64_t kStateStepOffset = 0x570;
inline constexpr std::uint64_t kCommandWaitFn = sw_variant::kMain.battle_command_wait_fn;
inline constexpr std::uint32_t kCommandWaitStep = 5;
inline constexpr std::uint64_t kFightFn = sw_variant::kMain.battle_fight_fn;
inline constexpr std::uint32_t kFightWaitStep = 2;
inline constexpr std::uint64_t kUiRootVtable = sw_variant::kMain.vt_ui_root;
inline constexpr std::uint64_t kCommandViewVtable = sw_variant::kMain.vt_command_view;
inline constexpr std::uint64_t kMoveViewVtable = sw_variant::kMain.vt_move_view;
inline constexpr std::uint64_t kListVtable = sw_variant::kMain.vt_grid_list;
inline constexpr std::uint64_t kCursorVtable = sw_variant::kMain.vt_grid_cursor;
inline constexpr std::uint64_t kCommandViewIndex = 2;
inline constexpr std::uint64_t kMoveViewIndex = 13;
inline constexpr std::uint64_t kMinViews = 14;
inline constexpr std::uint64_t kMaxViews = 64;

// Target = the doubles/multi target select (FIGHT state step 6, layout battle_target_select_00).
enum class Menu : int { None = 0, Command = 1, Fight = 2, Balls = 3, Target = 4 };

// The battle UI root sits at s1+0x6D8 in regular battles and at s1+0x688 in Max Raids (same s1
// vtable main+0x25BBD70; live 2026-11-01 heap pointer scan, battle-kinds bug 1). Both are tried,
// each vtable-checked.
inline constexpr std::uint64_t kUiRootOffsets[] = {0x6D8, 0x688};
inline constexpr std::uint32_t kFightTargetStep = 6;

struct Snapshot {
    Menu menu = Menu::None;
    int command = -1; // 0 Fight, 1 Pokemon, 2 Bag, 3 Run, 4 Back, 5 Cheer On; -1 unknown
    int move = -1;    // 0..3; -1 unknown
    std::uint64_t cursor = 0;      // command grid cursor object (diagnostics / bottom->top)
    std::uint64_t move_view = 0;   // move select view (bottom->top requests)
    std::uint64_t move_list = 0;   // its active list (+0x640, Max moves +0x648)
    int ball_item = 0;   // Balls: the item id the native quick-ball picker shows
    int ball_index = -1; // its position in the picker's list
    int ball_count = 0;
    int target = -1;            // Target: hovered card 0 far-left, 1 far-right, 2 near-left, 3 near-right
    std::uint64_t target_view = 0, target_list = 0;
    std::uint32_t grid_columns = 0, grid_rows = 0;
    std::uint64_t command_view = 0, command_list = 0;
    int cells = -1;             // Command: visible native cells (bit = cursor index), -1 unknown
    std::uint64_t ui = 0;       // the battle UI root that answered (diagnostics)
    std::uint64_t ui_offset = 0;
};

// Native quick-ball picker (X on the command menu, live-found 2026-10-31 dev24, label battle,
// int diffs over X / D-pad presses): the client runs the Bag state (Native8039C0, step 2) and
// views[4] (vtable main+0x252E320) holds the picker: +0x5F0 = picker object while it is open
// (null otherwise, also for the full Bag), +0x600 = shown index; picker +0x40 = entries
// {u32 item, u32 count}, +0x48 = entry count.
inline constexpr std::uint64_t kBagFn = sw_variant::kMain.battle_bag_fn;
inline constexpr std::uint32_t kBagWaitStep = 2;
inline constexpr std::uint64_t kBallViewIndex = 4;
inline constexpr std::uint64_t kBallViewVtable = sw_variant::kMain.vt_ball_view;

// Native command-grid index -> companion command (0 Fight, 1 Pokemon, 2 Bag, 3 Run, 4 Back,
// 5 Cheer On), -1 = not a command cell.
inline int CommandFromNative(std::uint32_t index) {
    switch (index) {
    case 0: return 0;
    case 1: return 5; // L_button_ouen: the raid's "Cheer On" (shown once your Pokemon fainted)
    case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    case 5: return 4; // L_button_back: a doubles second position's "Back" (battle-kinds bug 2)
    default: return -1;
    }
}

inline bool PlausiblePointer(std::uint64_t p) {
    return p >= 0x1000 && (p & 7) == 0 && p < (1ull << 40);
}

// Target select (doubles/multi, live 2026-11-01 Tower doubles, label battle): the client's FIGHT
// state waits at step 6 while battle_target_select_00 shows. views[3] (vtable main+0x252F138) owns
// it: +0x5D8 = its grid list (main+0x25BC8F0), list+0x408 = the grid cursor (main+0x25BD1F8) with
// +8 index, +0xC columns (12), +0x10 rows (2). Each target card spans half a row: index = row *
// columns + (0 | columns/2). Row 0 = the far (foe) side, row 1 = the near (own) side, left first
// (D-pad: Glaceon 0, Leafeon 6, Venusaur 12, Charizard 18). The cursor can rest on a greyed card
// (the user itself); A does nothing there.
inline constexpr std::uint64_t kTargetViewIndex = 3;
inline constexpr std::uint64_t kTargetViewVtable = sw_variant::kMain.vt_target_view;
inline constexpr std::uint64_t kTargetListOffset = 0x5D8;
inline constexpr std::uint64_t kCommandListOffset = 0x630;

// Target slot (0 far-left, 1 far-right, 2 near-left, 3 near-right) <-> grid index.
inline int TargetSlotFromGrid(std::uint32_t index, std::uint32_t columns, std::uint32_t rows) {
    if (columns < 2 || columns > 32 || (columns & 1) || rows != 2 || index >= columns * rows) return -1;
    const std::uint32_t row = index / columns, col = index % columns;
    if (col != 0 && col != columns / 2) return -1;
    return static_cast<int>(row * 2 + (col ? 1 : 0));
}
inline int TargetGridFromSlot(int slot, std::uint32_t columns, std::uint32_t rows) {
    if (slot < 0 || slot > 3 || columns < 2 || columns > 32 || (columns & 1) || rows != 2) return -1;
    return static_cast<int>((slot / 2) * columns + (slot % 2 ? columns / 2 : 0));
}

// Native command cells of battle_commandSelect_00 (bit = cursor index): the game hides the cells a
// battle does not offer and keeps the cursor index the fixed cell index (battle-kinds survey).
inline constexpr const char* kCellNames[6] = {"L_button_battle", "L_button_ouen", "L_button_pokemon",
                                              "L_button_bag", "L_button_escape", "L_button_back"};

// Layout ownership (hide phase 2, live 2026-11-01 Tower doubles, two boots). Each battle menu
// view keeps its layouts in a holder object, *(view+0x2B0) (vtable main+0x25B9D20). The offsets
// INSIDE the holder move between boots (target cursor at holder+0x1F38 in one boot, +0x1D28 in the
// next; move list cursor via +0x1110 / +0xBE0), exactly like the fixed deltas of the first hide
// research. What is stable is ownership: every holder references exactly ONE Layout named
// "cursor" -- directly, or through a layout work (vtable main+0x250F478) whose Layout* is at
// +0xC0 (the draw hook's own field). So the holder's first 0x2000 bytes are scanned once per
// view/holder, the distinct "cursor" Layouts collected, and the result kept only when there is
// exactly one (else 0 = that arrow stays). Cached results are re-verified (vtable + name) every
// tick. The command menu's own Layout: *(*(cmd_view+0x550)+0x170) + 0x1E38 (cells / catch prompt).
inline constexpr std::uint64_t kLayoutVtable = sw_variant::kMain.vt_layout;
inline constexpr std::uint64_t kHolderVtable = sw_variant::kMain.vt_layout_holder;
inline constexpr std::uint64_t kLayoutWorkVtable = sw_variant::kMain.vt_layout_work;
inline constexpr std::uint64_t kHolderOffset = 0x2B0;
inline constexpr std::size_t kHolderScanBytes = 0x2000;
struct Layouts {
    std::uint64_t command_menu = 0;
    std::uint64_t cursor[4]{};  // 0 command, 1 move list, 2 ball picker, 3 target select
};
// Per-view resolution cache (owned by the caller, one per battle client).
struct LayoutCache {
    std::uint64_t client = 0;
    std::uint64_t view[4]{}, holder[4]{}, cursor[4]{};
    int misses[4]{};
    std::uint64_t menu_view = 0, menu = 0;
    int menu_misses = 0;
};

namespace detail {
template <typename Read>
bool ReadName(Read& read, std::uint64_t at, char (&name)[32]) {
    for (auto& c : name) c = 0;
    if (!read(at, name, sizeof name - 1)) return false;
    name[sizeof name - 1] = 0;
    return true;
}
template <typename Read>
bool IsLayout(Read& read, std::uint64_t main_base, std::uint64_t layout, const char* want) {
    std::uint64_t v = 0, name_ptr = 0;
    char name[32];
    if (!PlausiblePointer(layout) || !read(layout, &v, sizeof v) || v != main_base + kLayoutVtable ||
        !read(layout + 0x30, &name_ptr, sizeof name_ptr) || !PlausiblePointer(name_ptr & ~7ull) ||
        !ReadName(read, name_ptr, name))
        return false;
    for (std::size_t i = 0; i < sizeof name; ++i) {
        if (name[i] != want[i]) return false;
        if (!want[i]) return true;
    }
    return false;
}
// Child pane named `want` directly under `pane` (children list head at pane+0x20, node = pane+8,
// next at node+8; name at pane+0xB0). Bounded: 48 children.
template <typename Read>
std::uint64_t Child(Read& read, std::uint64_t pane, const char* want) {
    std::uint64_t node = 0;
    const std::uint64_t head = pane + 0x20;
    if (!read(head + 8, &node, sizeof node)) return 0;
    for (int k = 0; k < 48 && PlausiblePointer(node) && node != head; ++k) {
        const std::uint64_t child = node - 8;
        char name[32];
        if (!ReadName(read, child + 0xB0, name)) return 0;
        bool same = true;
        for (std::size_t i = 0; i < sizeof name; ++i) {
            if (name[i] != want[i]) { same = false; break; }
            if (!want[i]) break;
        }
        if (same) return child;
        if (!read(node + 8, &node, sizeof node)) return 0;
    }
    return 0;
}
} // namespace detail

// Visible command cells (bit i = cursor index i) of the live battle_commandSelect_00, -1 unknown.
template <typename Read>
int SampleCells(Read&& read, std::uint64_t main_base, std::uint64_t layout) {
    if (!detail::IsLayout(read, main_base, layout, "battle_commandSelect_00")) return -1;
    std::uint64_t pane = 0;
    if (!read(layout + 0x18, &pane, sizeof pane) || !PlausiblePointer(pane)) return -1;
    for (const char* step : {"all", "inout_00", "inout", "command"}) {
        pane = detail::Child(read, pane, step);
        if (!pane) return -1;
    }
    int cells = 0;
    for (int i = 0; i < 6; ++i) {
        const std::uint64_t cell = detail::Child(read, pane, kCellNames[i]);
        std::uint8_t flags = 0;
        if (!cell || !read(cell + 0x58, &flags, 1)) return -1;
        if (flags & 1) cells |= 1 << i;
    }
    return cells;
}

// The raid catch prompt (commandSelect_00 all > inout_00 > inout > ball: Catch / Don't Catch)
// visible: 1, hidden: 0, unknown: -1. The menu hide never hides it.
template <typename Read>
int SampleCatchPrompt(Read&& read, std::uint64_t main_base, std::uint64_t layout) {
    if (!detail::IsLayout(read, main_base, layout, "battle_commandSelect_00")) return -1;
    std::uint64_t pane = 0;
    if (!read(layout + 0x18, &pane, sizeof pane) || !PlausiblePointer(pane)) return -1;
    for (const char* step : {"all", "inout_00", "inout", "ball"}) {
        pane = detail::Child(read, pane, step);
        if (!pane) return -1;
    }
    std::uint8_t flags = 0;
    if (!read(pane + 0x58, &flags, 1)) return -1;
    return flags & 1;
}

// The battle UI root's sub-view vector, shared by Sample and SampleLayouts.
struct Views {
    std::uint64_t begin = 0, count = 0, ui = 0, ui_offset = 0;
};
template <typename Read>
bool ResolveViews(Read& read, std::uint64_t main_base, std::uint64_t client, Views& out) {
    const auto q = [&](std::uint64_t a, std::uint64_t& v) {
        return PlausiblePointer(a) && read(a, &v, sizeof v);
    };
    const auto ptr = [&](std::uint64_t a, std::uint64_t& v) { return q(a, v) && PlausiblePointer(v); };
    std::uint64_t view = 0, sub = 0, slot = 0, s1 = 0, ui = 0, begin = 0, end = 0, v = 0;
    if (!main_base || !PlausiblePointer(client) || !ptr(client + kViewOffset, view) ||
        !ptr(view + 0x5C8, sub) || !ptr(sub + 0x38, slot) || !ptr(slot + 0x10, s1))
        return false;
    for (const auto offset : kUiRootOffsets)
        if (ptr(s1 + offset, ui) && q(ui, v) && v == main_base + kUiRootVtable) {
            out.ui_offset = offset;
            if (!ptr(ui + 0x78, begin) || !ptr(ui + 0x80, end) || end <= begin || (end - begin) % 8 != 0)
                return false;
            const std::uint64_t count = (end - begin) / 8;
            if (count < kMinViews || count > kMaxViews) return false;
            out.begin = begin;
            out.count = count;
            out.ui = ui;
            return true;
        }
    return false;
}

// The one Layout named `name` a holder owns (see Layouts), 0 when none or ambiguous.
template <typename Read>
std::uint64_t HolderLayout(Read& read, std::uint64_t main_base, std::uint64_t holder, const char* name) {
    static thread_local std::uint8_t raw[kHolderScanBytes];
    if (!read(holder, raw, sizeof raw)) return 0;
    std::uint64_t found = 0;
    for (std::size_t o = 0; o + 8 <= sizeof raw; o += 8) {
        std::uint64_t q = 0;
        for (int i = 0; i < 8; ++i) q |= std::uint64_t(raw[o + i]) << (8 * i);
        if (!PlausiblePointer(q) || q == found) continue;
        std::uint64_t vt = 0, layout = 0;
        if (!read(q, &vt, sizeof vt)) continue;
        if (vt == main_base + kLayoutVtable) layout = q;
        else if (vt == main_base + kLayoutWorkVtable && read(q + 0xC0, &layout, sizeof layout)) {}
        else continue;
        if (!layout || layout == found || !detail::IsLayout(read, main_base, layout, name)) continue;
        if (found) return 0; // two different ones: not provably this menu's
        found = layout;
    }
    return found;
}

// Every menu's cursor Layout by ownership (see Layouts), whatever the client state. `cache` keeps
// the per-view scan result for this client; a changed view/holder or a failed re-check rescans
// (at most every 30 calls per view while it keeps failing).
template <typename Read>
Layouts SampleLayouts(Read&& read, std::uint64_t main_base, std::uint64_t client, LayoutCache& cache) {
    Layouts out;
    Views views;
    if (cache.client != client) cache = LayoutCache{client};
    if (!ResolveViews(read, main_base, client, views)) return out;
    const auto q = [&](std::uint64_t a, std::uint64_t& v) {
        return PlausiblePointer(a) && read(a, &v, sizeof v) && PlausiblePointer(v);
    };
    const auto view = [&](std::uint64_t index, std::uint64_t vtable, std::uint64_t& v) {
        std::uint64_t t = 0;
        return q(views.begin + index * 8, v) && read(v, &t, sizeof t) && t == main_base + vtable;
    };
    // The command menu's own Layout: the regular-battle chain, else its holder (raids: the chain's
    // inner offsets differ, live 2026-11-01), cached like the cursors.
    std::uint64_t v = 0, a = 0, b = 0;
    if (view(kCommandViewIndex, kCommandViewVtable, v)) {
        if (q(v + 0x550, a) && q(a + 0x170, b) &&
            detail::IsLayout(read, main_base, b + 0x1E38, "battle_commandSelect_00"))
            out.command_menu = b + 0x1E38;
        else if (cache.menu && cache.menu_view == v &&
                 detail::IsLayout(read, main_base, cache.menu, "battle_commandSelect_00"))
            out.command_menu = cache.menu;
        else if (q(v + kHolderOffset, a) && (cache.menu_view != v || cache.menu_misses++ % 30 == 0)) {
            cache.menu_view = v;
            cache.menu = HolderLayout(read, main_base, a, "battle_commandSelect_00");
            if (cache.menu) cache.menu_misses = 0;
            out.command_menu = cache.menu;
        }
    }
    constexpr std::uint64_t kIndex[4] = {kCommandViewIndex, kMoveViewIndex, kBallViewIndex, kTargetViewIndex};
    constexpr std::uint64_t kVtable[4] = {kCommandViewVtable, kMoveViewVtable, kBallViewVtable, kTargetViewVtable};
    for (int i = 0; i < 4; ++i) {
        std::uint64_t holder = 0, hv = 0;
        if (!view(kIndex[i], kVtable[i], v) || !q(v + kHolderOffset, holder) || !read(holder, &hv, sizeof hv) ||
            hv != main_base + kHolderVtable) {
            cache.view[i] = cache.holder[i] = cache.cursor[i] = 0;
            continue;
        }
        if (cache.view[i] == v && cache.holder[i] == holder && cache.cursor[i] &&
            detail::IsLayout(read, main_base, cache.cursor[i], "cursor")) {
            out.cursor[i] = cache.cursor[i];
            continue;
        }
        const bool same = cache.view[i] == v && cache.holder[i] == holder;
        if (same && cache.misses[i] > 0 && cache.misses[i]++ % 30 != 0) continue;
        cache.view[i] = v;
        cache.holder[i] = holder;
        cache.cursor[i] = HolderLayout(read, main_base, holder, "cursor");
        cache.misses[i] = cache.cursor[i] ? 0 : (same ? cache.misses[i] : 1);
        out.cursor[i] = cache.cursor[i];
    }
    // Two owners must never resolve to the same cursor: fail closed on both.
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (out.cursor[i] && out.cursor[i] == out.cursor[j]) out.cursor[i] = out.cursor[j] = 0;
    return out;
}

// `read(address, out, size)` returns false on any failure (unmapped, torn read).
template <typename Read>
Snapshot Sample(Read&& read, std::uint64_t main_base, std::uint64_t client) {
    Snapshot out;
    if (!main_base || !PlausiblePointer(client)) return out;
    const auto q = [&](std::uint64_t a, std::uint64_t& v) {
        return PlausiblePointer(a) && read(a, &v, sizeof v);
    };
    const auto ptr = [&](std::uint64_t a, std::uint64_t& v) { return q(a, v) && PlausiblePointer(v); };
    const auto vt = [&](std::uint64_t obj, std::uint64_t offset) {
        std::uint64_t v = 0;
        return q(obj, v) && v == main_base + offset;
    };
    std::uint64_t state_fn = 0;
    std::uint32_t step = 0;
    if (!q(client + kStateFnOffset, state_fn) || !read(client + kStateStepOffset, &step, sizeof step))
        return out;
    const bool command = state_fn == main_base + kCommandWaitFn && step == kCommandWaitStep;
    const bool fight = state_fn == main_base + kFightFn && step == kFightWaitStep;
    const bool target = state_fn == main_base + kFightFn && step == kFightTargetStep;
    const bool bag = state_fn == main_base + kBagFn && step == kBagWaitStep;
    if (!command && !fight && !bag && !target) return out;

    Views views;
    if (!ResolveViews(read, main_base, client, views)) return out;
    const std::uint64_t begin = views.begin;
    out.ui = views.ui;
    out.ui_offset = views.ui_offset;

    if (target) {
        std::uint64_t tv = 0, list = 0, cursor = 0;
        std::uint32_t grid[3]{};
        out.menu = Menu::Target; // the state alone is known; the hovered card may stay unknown
        if (!ptr(begin + kTargetViewIndex * 8, tv) || !vt(tv, kTargetViewVtable) ||
            !ptr(tv + kTargetListOffset, list) || !vt(list, kListVtable) || !ptr(list + 0x408, cursor) ||
            !vt(cursor, kCursorVtable) || !read(cursor + 8, grid, sizeof grid))
            return out;
        out.target = TargetSlotFromGrid(grid[0], grid[1], grid[2]);
        if (out.target >= 0) {
            out.target_view = tv;
            out.target_list = list;
            out.grid_columns = grid[1];
            out.grid_rows = grid[2];
            out.cursor = cursor;
        }
        return out;
    }
    if (bag) {
        std::uint64_t bv = 0, picker = 0, entries = 0;
        std::uint32_t index = 0, count = 0, entry[2]{};
        if (!ptr(begin + kBallViewIndex * 8, bv) || !vt(bv, kBallViewVtable) || !ptr(bv + 0x5F0, picker) ||
            !read(bv + 0x600, &index, sizeof index) || !ptr(picker + 0x40, entries) ||
            !read(picker + 0x48, &count, sizeof count) || count == 0 || count > 64 || index >= count ||
            !read(entries + 8ull * index, entry, sizeof entry) || entry[0] == 0 || entry[0] > 2000)
            return out;
        out.menu = Menu::Balls;
        out.ball_item = static_cast<int>(entry[0]);
        out.ball_index = static_cast<int>(index);
        out.ball_count = static_cast<int>(count);
        return out;
    }
    if (command) {
        std::uint64_t cv = 0, list = 0, cursor = 0;
        std::uint32_t index = 0;
        if (!ptr(begin + kCommandViewIndex * 8, cv) || !vt(cv, kCommandViewVtable) ||
            !ptr(cv + 0x630, list) || !vt(list, kListVtable) || !ptr(list + 0x408, cursor) ||
            !vt(cursor, kCursorVtable) || !read(cursor + 8, &index, sizeof index))
            return out;
        const int mapped = CommandFromNative(index);
        if (mapped < 0) return out;
        out.menu = Menu::Command;
        out.command = mapped;
        out.cursor = cursor;
        out.command_view = cv;
        out.command_list = list;
        return out; // cells: SampleCells on SampleLayouts' command_menu (the caller)
    }
    std::uint64_t mv = 0;
    std::uint32_t slot_index = 0;
    if (!ptr(begin + kMoveViewIndex * 8, mv) || !vt(mv, kMoveViewVtable) ||
        !read(mv + 0x678, &slot_index, sizeof slot_index) || slot_index > 3)
        return out;
    // Cross-check against the active list's own cursor: move rows sit in column 1 of a 2-column
    // grid (index = row * 2 + 1).
    std::uint8_t max_list = 0;
    std::uint64_t list = 0, cursor = 0;
    std::uint32_t grid[3]{};
    if (!read(mv + 0x61D, &max_list, 1) || !ptr(mv + (max_list ? 0x648 : 0x640), list) ||
        !vt(list, kListVtable) || !ptr(list + 0x408, cursor) || !vt(cursor, kCursorVtable) ||
        !read(cursor + 8, grid, sizeof grid) || grid[1] == 0 || grid[1] > 4)
        return out;
    if (grid[0] / grid[1] != slot_index) return out;
    out.menu = Menu::Fight;
    out.move = static_cast<int>(slot_index);
    out.cursor = cursor;
    out.move_view = mv;
    out.move_list = list;
    return out;
}

} // namespace sw_battle_cursor
