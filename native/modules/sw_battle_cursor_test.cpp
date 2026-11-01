// SPDX-License-Identifier: GPL-3.0-or-later
// Native battle cursor chain (sw_battle_cursor.h) against a fake guest memory laid out like the
// live 2026-10-31 capture (label battle): command hover 0/2/3/4, move hover via move_view+0x678.
#include "sw_battle_cursor.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using namespace sw_battle_cursor;
namespace {
constexpr std::uint64_t kMain = 0x80b62000;
struct Mem {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void Put(std::uint64_t a, const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) bytes[a + i] = b[i];
    }
    void Q(std::uint64_t a, std::uint64_t v) { Put(a, &v, 8); }
    void D(std::uint64_t a, std::uint32_t v) { Put(a, &v, 4); }
    bool Read(std::uint64_t a, void* out, std::size_t n) const {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = bytes.find(a + i);
            if (it == bytes.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
};
constexpr std::uint64_t kClient = 0x219512ee60, kView = 0x218d893850, kSub = 0x218d8ca3b0,
                        kSlot = 0x218d92fd10, kS1 = 0x218d940000, kUi = 0x2189290e48,
                        kArr = 0x21892a8178, kCmdView = 0x21892a0410, kList = 0x21897bd870,
                        kCursor = 0x21897bde70, kMoveView = 0x21892a5520, kMoveList = 0x2189c00000,
                        kMoveCursor = 0x2189c01000, kMaxList = 0x2189c02000, kMaxCursor = 0x2189c03000;

Mem Build(bool command, std::uint32_t cmd_index, std::uint32_t move_slot, std::uint32_t move_grid) {
    Mem m;
    m.Q(kClient + 0x560, kMain + (command ? kCommandWaitFn : kFightFn));
    m.D(kClient + 0x570, command ? kCommandWaitStep : kFightWaitStep);
    m.Q(kClient + 0x358, kView);
    m.Q(kView + 0x5C8, kSub);
    m.Q(kSub + 0x38, kSlot);
    m.Q(kSlot + 0x10, kS1);
    m.Q(kS1 + 0x6D8, kUi);
    m.Q(kUi, kMain + kUiRootVtable);
    m.Q(kUi + 0x78, kArr);
    m.Q(kUi + 0x80, kArr + 21 * 8);
    for (int i = 0; i < 21; ++i) m.Q(kArr + i * 8, 0x21892b0000 + i * 0x1000);
    m.Q(kArr + 2 * 8, kCmdView);
    m.Q(kArr + 13 * 8, kMoveView);
    m.Q(kCmdView, kMain + kCommandViewVtable);
    m.Q(kCmdView + 0x630, kList);
    m.Q(kList, kMain + kListVtable);
    m.Q(kList + 0x408, kCursor);
    m.Q(kCursor, kMain + kCursorVtable);
    m.D(kCursor + 8, cmd_index);
    m.D(kCursor + 0xC, 1);
    m.D(kCursor + 0x10, 8);
    m.Q(kMoveView, kMain + kMoveViewVtable);
    m.Put(kMoveView + 0x61D, "\0", 1);
    m.D(kMoveView + 0x678, move_slot);
    m.Q(kMoveView + 0x640, kMoveList);
    m.Q(kMoveView + 0x648, kMaxList);
    for (auto [l, c] : {std::pair{kMoveList, kMoveCursor}, std::pair{kMaxList, kMaxCursor}}) {
        m.Q(l, kMain + kListVtable);
        m.Q(l + 0x408, c);
        m.Q(c, kMain + kCursorVtable);
        m.D(c + 0xC, 2);
        m.D(c + 0x10, 4);
    }
    m.D(kMoveCursor + 8, move_grid);
    m.D(kMaxCursor + 8, 1);
    return m;
}
// Raid: the UI root moved to s1+0x688 (s1+0x6D8 holds a non-pointer).
Mem BuildRaid(std::uint32_t cmd_index) {
    Mem m = Build(true, cmd_index, 0, 1);
    m.Q(kS1 + 0x6D8, 0x1600000);
    m.Q(kS1 + 0x688, kUi);
    return m;
}
Snapshot Run(const Mem& m, std::uint64_t client = kClient) {
    return Sample([&](std::uint64_t a, void* o, std::size_t n) { return m.Read(a, o, n); }, kMain, client);
}

void CommandHover() {
    const int want[] = {0, 5, 1, 2, 3, 4};
    for (std::uint32_t i = 0; i < 6; ++i) {
        const auto s = Run(Build(true, i, 0, 1));
        assert(s.command == want[i]);
        assert(s.menu == (want[i] >= 0 ? Menu::Command : Menu::None));
        assert(s.move == -1);
    }
}
void MoveHover() {
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        const auto s = Run(Build(false, 0, slot, slot * 2 + 1));
        assert(s.menu == Menu::Fight && s.move == static_cast<int>(slot) && s.command == -1);
    }
    // Callback slot and the list's own cursor disagree -> unknown.
    assert(Run(Build(false, 0, 2, 1)).menu == Menu::None);
    assert(Run(Build(false, 0, 4, 9)).move == -1);
    // Max moves list selected through move_view+0x61D.
    auto m = Build(false, 0, 0, 5);
    m.Put(kMoveView + 0x61D, "\1", 1);
    assert(Run(m).move == 0);
}
void RaidRoot() {
    for (std::uint32_t i : {0u, 2u, 3u, 4u}) {
        const auto s = Run(BuildRaid(i));
        assert(s.menu == Menu::Command && s.command == CommandFromNative(i) && s.ui_offset == 0x688);
    }
    assert(Run(BuildRaid(1)).command == 5); // Cheer On
    // Neither root vtable-checked -> unknown; a wrong vtable at +0x6D8 falls through to +0x688.
    auto m = BuildRaid(2);
    m.Q(kS1 + 0x688, 0x2189290000);
    assert(Run(m).menu == Menu::None);
    m = Build(true, 2, 0, 1);
    assert(Run(m).ui_offset == 0x6D8);
}
// Doubles target select: FIGHT step 6, views[3] list +0x5D8, 12 x 2 grid.
constexpr std::uint64_t kTargetView = 0x21892a1000, kTargetList = 0x2189d00000, kTargetCursor = 0x2189d01000;
Mem BuildTarget(std::uint32_t index) {
    Mem m = Build(false, 0, 0, 1);
    m.D(kClient + 0x570, kFightTargetStep);
    m.Q(kArr + 3 * 8, kTargetView);
    m.Q(kTargetView, kMain + kTargetViewVtable);
    m.Q(kTargetView + kTargetListOffset, kTargetList);
    m.Q(kTargetList, kMain + kListVtable);
    m.Q(kTargetList + 0x408, kTargetCursor);
    m.Q(kTargetCursor, kMain + kCursorVtable);
    m.D(kTargetCursor + 8, index);
    m.D(kTargetCursor + 0xC, 12);
    m.D(kTargetCursor + 0x10, 2);
    return m;
}
void TargetState() {
    const std::pair<std::uint32_t, int> cases[] = {{0, 0}, {6, 1}, {12, 2}, {18, 3}, {3, -1}, {24, -1}};
    for (auto [index, slot] : cases) {
        const auto s = Run(BuildTarget(index));
        assert(s.menu == Menu::Target && s.target == slot);
        assert((slot >= 0) == (s.target_list == kTargetList && s.target_view == kTargetView));
        assert(s.command == -1 && s.move == -1);
    }
    for (int slot = 0; slot < 4; ++slot) {
        const int grid = TargetGridFromSlot(slot, 12, 2);
        assert(TargetSlotFromGrid(static_cast<std::uint32_t>(grid), 12, 2) == slot);
    }
    assert(TargetGridFromSlot(4, 12, 2) == -1 && TargetGridFromSlot(0, 12, 3) == -1);
    auto m = BuildTarget(6);
    m.Q(kTargetView, kMain + kMoveViewVtable); // wrong owner: state known, card unknown
    const auto s = Run(m);
    assert(s.menu == Menu::Target && s.target == -1 && !s.target_list);
}
// Visible cells + layout ownership: a fake nn::ui2d pane tree under battle_commandSelect_00.
constexpr std::uint64_t kNames = 0x2190000000;
struct Tree {
    Mem& m;
    std::uint64_t next = 0x2191000000, name_at = kNames;
    std::uint64_t Name(const char* n) {
        const auto at = name_at;
        char padded[32]{};
        std::strncpy(padded, n, 31);
        m.Put(at, padded, sizeof padded);
        name_at += 0x40;
        return at;
    }
    std::uint64_t Pane(const char* n, std::uint8_t flags) {
        const auto p = next;
        next += 0x200;
        char name[32]{};
        std::strncpy(name, n, 31);
        m.Put(p + 0xB0, name, 32);
        m.Put(p + 0x58, &flags, 1);
        m.Q(p + 0x28, p + 0x20); // empty child list (head.next == head)
        return p;
    }
    void Kids(std::uint64_t parent, std::initializer_list<std::uint64_t> kids) {
        const std::uint64_t head = parent + 0x20;
        std::uint64_t prev = head;
        for (auto k : kids) {
            m.Q(prev + 8, k + 8);
            prev = k + 8;
        }
        m.Q(prev + 8, head);
    }
    std::uint64_t Layout(std::uint64_t at, const char* n, std::uint64_t root) {
        m.Q(at, kMain + kLayoutVtable);
        m.Q(at + 0x30, Name(n));
        m.Q(at + 0x18, root);
        return at;
    }
};
void CellsAndLayouts() {
    auto m = Build(true, 0, 0, 1);
    Tree t{m};
    const auto root = t.Pane("RootPane", 0x81), all = t.Pane("all", 0x83), io0 = t.Pane("inout_00", 0x81),
               io = t.Pane("inout", 0x81), ball = t.Pane("ball", 0x12), cmd = t.Pane("command", 0x81);
    std::uint64_t cell[6];
    const std::uint8_t tower_mon2[6] = {0x83, 0x12, 0x83, 0x12, 0x12, 0x83}; // Fight/Pokemon/Back
    for (int i = 0; i < 6; ++i) cell[i] = t.Pane(kCellNames[i], tower_mon2[i]);
    t.Kids(root, {all});
    t.Kids(all, {io0});
    t.Kids(io0, {io});
    t.Kids(io, {ball, cmd});
    t.Kids(cmd, {cell[0], cell[1], cell[2], cell[3], cell[4], cell[5]});
    constexpr std::uint64_t kHolder = 0x2192000000, kMenuWork = 0x2193000000;
    m.Q(kCmdView + 0x550, kHolder);
    m.Q(kHolder + 0x170, kMenuWork);
    t.Layout(kMenuWork + 0x1E38, "battle_commandSelect_00", root);
    auto s = Run(m);
    assert(s.menu == Menu::Command && s.command_list == kList && s.command_view == kCmdView);
    const auto read = [&](std::uint64_t a, void* o, std::size_t n) { return m.Read(a, o, n); };
    assert(SampleCells(read, kMain, kMenuWork + 0x1E38) == 0b100101); // Fight / Pokemon / Back
    // Holders: *(view+0x2B0), each 0x2000 bytes scanned for its one "cursor" Layout.
    constexpr std::uint64_t kBallView = 0x21892b4000;
    m.Q(kArr + 4 * 8, kBallView);
    m.Q(kBallView, kMain + kBallViewVtable);
    m.Q(kArr + 3 * 8, kTargetView);
    m.Q(kTargetView, kMain + kTargetViewVtable);
    const std::uint64_t views[4] = {kCmdView, kMoveView, kBallView, kTargetView};
    std::uint64_t holders[4], cursors[4];
    for (int i = 0; i < 4; ++i) {
        holders[i] = 0x2192100000 + i * 0x10000ull;
        cursors[i] = 0x2196000000 + i * 0x10000ull;
        m.Q(views[i] + kHolderOffset, holders[i]);
        std::vector<std::uint8_t> zero(kHolderScanBytes, 0);
        m.Put(holders[i], zero.data(), zero.size());
        m.Q(holders[i], kMain + kHolderVtable);
        t.Layout(cursors[i], "cursor", 0);
    }
    // command: through a layout work (Layout* at +0xC0) at a "boot-varying" offset, plus noise
    constexpr std::uint64_t kWork = 0x2197000000;
    m.Q(kWork, kMain + kLayoutWorkVtable);
    m.Q(kWork + 0xC0, cursors[0]);
    m.Q(holders[0] + 0x1908, kWork);
    m.Q(holders[0] + 0x1920, kWork);
    m.Q(holders[0] + 0x40, kMenuWork + 0x1E38); // its own menu Layout: not a cursor, ignored
    m.Q(holders[1] + 0xBE0, cursors[1]);        // move: direct pointer
    m.Q(holders[1] + 0x1B30, cursors[1]);
    m.Q(holders[2] + 0xE48, kWork + 0x100000);  // ball: another work
    m.Q(kWork + 0x100000, kMain + kLayoutWorkVtable);
    m.Q(kWork + 0x100000 + 0xC0, cursors[2]);
    m.Q(holders[3] + 0x1F00, cursors[3]);       // target: direct
    LayoutCache cache;
    auto l = SampleLayouts(read, kMain, kClient, cache);
    assert(l.command_menu == kMenuWork + 0x1E38);
    for (int i = 0; i < 4; ++i) assert(l.cursor[i] == cursors[i]);
    // Cached: the holder is not rescanned while its cursor still verifies; a renamed cursor drops.
    m.Q(holders[1] + 0x1B30, cursors[0]); // would now be ambiguous on a rescan
    assert(SampleLayouts(read, kMain, kClient, cache).cursor[1] == cursors[1]);
    t.Layout(cursors[1], "battle_skillSelect_00", 0);
    l = SampleLayouts(read, kMain, kClient, cache);
    assert(!l.cursor[1]); // re-verify fails, rescan sees only cursors[0] -> but that one is the
                          // command's: the shared-owner rule drops both
    assert(!l.cursor[0]);
    // A holder that references two different cursors is ambiguous: no cursor for that menu.
    LayoutCache fresh;
    t.Layout(cursors[1], "cursor", 0);
    m.Q(holders[1] + 0x1B30, cursors[2]);
    l = SampleLayouts(read, kMain, kClient, fresh);
    assert(!l.cursor[1] && l.cursor[0] == cursors[0] && l.cursor[2] == cursors[2] && l.cursor[3] == cursors[3]);
    // A holder with the wrong vtable is never scanned.
    m.Q(holders[3], kMain + 0x1234);
    assert(!SampleLayouts(read, kMain, kClient, fresh).cursor[3]);
    // A new battle client resets the cache.
    assert(!SampleLayouts(read, kMain, kClient + 8, fresh).cursor[0]);
    // Raid: the chain misses (renamed here), the command holder still owns the menu Layout.
    LayoutCache raid;
    t.Layout(kMenuWork + 0x1E38, "battle_commandSelect_01", root);
    constexpr std::uint64_t kRaidMenu = 0x2198800000;
    t.Layout(kRaidMenu, "battle_commandSelect_00", root);
    m.Q(holders[0] + 0x1338, kRaidMenu);
    assert(SampleLayouts(read, kMain, kClient, raid).command_menu == kRaidMenu);
    assert(SampleCatchPrompt(read, kMain, kRaidMenu) == 0); // "ball" (Catch / Don't Catch) hidden
    // A missing cell or a renamed menu -> cells unknown (the bottom keeps its full set).
    assert(SampleCells(read, kMain, kMenuWork + 0x1E38) == -1);
}
void FailClosed() {
    // Wrong client state (turn animation), wrong step, a vtable mismatch anywhere, or a missing
    // pointer all give unknown.
    auto m = Build(true, 3, 0, 1);
    m.D(kClient + 0x570, 4);
    assert(Run(m).menu == Menu::None);
    m = Build(true, 3, 0, 1);
    m.Q(kClient + 0x560, kMain + 0x8039C0);
    assert(Run(m).menu == Menu::None);
    for (std::uint64_t obj : {kUi, kCmdView, kList, kCursor}) {
        m = Build(true, 3, 0, 1);
        m.Q(obj, kMain + 0x1234);
        assert(Run(m).command == -1);
    }
    m = Build(false, 0, 1, 3);
    m.Q(kMoveView, kMain + kCommandViewVtable);
    assert(Run(m).move == -1);
    m = Build(true, 3, 0, 1);
    m.Q(kUi + 0x80, kArr + 4 * 8); // too few views
    assert(Run(m).command == -1);
    m = Build(true, 3, 0, 1);
    m.bytes.erase(kSlot + 0x10);
    assert(Run(m).command == -1);
    assert(Run(Build(true, 3, 0, 1), 0).command == -1);
    assert(Sample([](std::uint64_t, void*, std::size_t) { return false; }, kMain, kClient).menu == Menu::None);
}
} // namespace

int main() {
    CommandHover();
    MoveHover();
    RaidRoot();
    TargetState();
    CellsAndLayouts();
    FailClosed();
    std::cout << "sw_battle_cursor_test OK\n";
    return 0;
}
