// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The top-HUD controller (xcx_hud.h) against a fake game: options manager, SystemData, the field-HUD
// observer list and the minimap widget, with a frame step that applies a pending commit the way the
// game does. Every case must end with the player's saved Mini-map value (Show) in SystemData, also
// when the observer changes or a read fails while a commit of the controller is in flight.
#include "xcx_hud.h"
#include <cstdio>
#include <cstring>
#include <map>
namespace {
using namespace xcx::hud;
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
constexpr std::uint64_t Main = 0x10000000, Sys = 0x20000000, Mgr = 0x30000000;
constexpr std::uint64_t ObsA = 0x40000000, ObsB = 0x50000000;
constexpr std::uint32_t K = 1; // Mini-map
struct Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    std::map<std::uint64_t, bool> widget_shown; // per observer object
    bool fail_list = false;                       // reads of the observer list fail
    void Put(std::uint64_t a, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            mem[a + i] = std::uint8_t(v >> (8 * i));
    }
    std::uint8_t At(std::uint64_t a) const {
        const auto it = mem.find(a);
        return it == mem.end() ? 0 : it->second;
    }
    Game() {
        Put(Main + SystemDataSlot, Sys, 8);
        Put(Main + OptionsManagerSlot, Mgr, 8);
        for (std::uint32_t i = 0; i < BlockSize; ++i) { // every option on (1), the player's own choices
            Put(Sys + BlockOffset + i, 1, 1);
            Put(Mgr + WorkingCopy + i, 1, 1);
        }
        Put(Mgr + CommitFlag, 0, 1);
        Observer(ObsA);
    }
    // The field HUD (re)made: one list entry, copy taken from SystemData, widget as SystemData says.
    void Observer(std::uint64_t obj) {
        Put(Mgr + ObserverHead, 1, 1);
        Put(Mgr + ObserverEntries + 16, obj, 8);
        Put(Mgr + ObserverEntries + 16 + ObserverNext, 0, 1);
        Put(obj, Main + FieldHudVtable, 8);
        for (std::uint32_t k = 0; k < 8; ++k)
            Put(obj + ObserverCopy + k, At(Sys + HudOffset + k), 1);
        widget_shown[obj] = At(Sys + HudOffset + K) == 1;
    }
    std::uint64_t Current() const {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= std::uint64_t(At(Mgr + ObserverEntries + 16 + i)) << (8 * i);
        return v;
    }
    // One game frame: a pending commit notifies the observer (a show/hide message only when its copy
    // differs) and is copied into SystemData.
    void Frame() {
        if (!At(Mgr + CommitFlag))
            return;
        const std::uint64_t obj = Current();
        const std::uint8_t v = At(Mgr + WorkingCopy + HudIndex(K));
        if (obj && At(obj + ObserverCopy + K) != v)
            widget_shown[obj] = v == 1;
        if (obj)
            for (std::uint32_t k = 0; k < 8; ++k)
                Put(obj + ObserverCopy + k, At(Mgr + WorkingCopy + HudIndex(k)), 1);
        for (std::uint32_t i = 0; i < BlockSize; ++i)
            Put(Sys + BlockOffset + i, At(Mgr + WorkingCopy + i), 1);
        Put(Mgr + CommitFlag, 0, 1);
    }
    std::uint8_t Saved() const { return At(Sys + HudOffset + K); }
    std::uint8_t Working() const { return At(Mgr + WorkingCopy + HudIndex(K)); }
    bool Shown() { return widget_shown[Current()]; }
    Io MakeIo() {
        Io io;
        io.read = [this](std::uint64_t a, void *out, std::size_t n) {
            if (fail_list && a == Mgr + ObserverHead)
                return false;
            auto *p = static_cast<std::uint8_t *>(out);
            for (std::size_t i = 0; i < n; ++i)
                p[i] = At(a + i);
            return true;
        };
        io.batch = [this](const Io::Op *ops, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i)
                for (std::uint32_t b = 0; b < ops[i].size; ++b)
                    if (At(ops[i].address + b) != static_cast<const std::uint8_t *>(ops[i].expect)[b])
                        return false;
            for (std::size_t i = 0; i < n; ++i)
                for (std::uint32_t b = 0; b < ops[i].size; ++b)
                    Put(ops[i].address + b, static_cast<const std::uint8_t *>(ops[i].value)[b], 1);
            return true;
        };
        return io;
    }
};
constexpr std::array<bool, Count> HideMap{true, false}, Nothing{false, false};
// Hide applied and written back: the steady hidden state.
void HideSteady(Game &g, Controller &c, const Io &io) {
    c.Update(io, Main, HideMap, true); // commit Hide
    g.Frame();
    c.Update(io, Main, HideMap, true); // write-back
}
} // namespace
int main() {
    {
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        HideSteady(g, c, io);
        Check(!g.Shown() && g.Saved() == 1 && g.Working() == 1, "plain hide: hidden, saved Show");
        c.Update(io, Main, Nothing, true);
        Check(g.Saved() == 0, "show commit: SystemData says Hide for one frame");
        g.Frame();
        c.Update(io, Main, Nothing, true);
        Check(g.Shown() && g.Saved() == 1, "plain show: shown, saved Show");
    }
    {
        // Observer change after the game applied our Hide, before the write-back (A1 case 1).
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        c.Update(io, Main, HideMap, true);
        g.Frame();
        Check(g.Saved() == 0, "hide applied: SystemData Hide until the write-back");
        g.Observer(ObsB); // a load: the new HUD is made while SystemData says Hide
        c.Update(io, Main, HideMap, true);
        Check(g.Saved() == 1 && g.Working() == 1, "observer change mid hide: player's value restored");
        Check(!c.Busy(), "observer change mid hide: commit settled");
        for (int i = 0; i < 4; ++i) {
            c.Update(io, Main, HideMap, true);
            g.Frame();
        }
        Check(g.Saved() == 1, "observer change mid hide: SystemData stays Show");
        c.Update(io, Main, Nothing, true); // toggle off
        g.Frame();
        c.Update(io, Main, Nothing, true);
        Check(g.Shown() && g.Saved() == 1, "observer change mid hide: toggle off shows the new widget");
    }
    {
        // A HUD whose copy says Hide on its own (no commit of ours in flight): handled as before, a Hide is
        // committed when wanted and SystemData keeps the player's Show.
        Game g;
        g.Put(ObsA + ObserverCopy + K, 0, 1);
        Controller c;
        const auto io = g.MakeIo();
        HideSteady(g, c, io);
        Check(c.Commits() == 1 && g.Saved() == 1 && c.Hidden(Map), "copy Hide without our commit: Hide committed");
    }
    {
        // Observer change while our Hide commit is still pending (withdrawn).
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        c.Update(io, Main, HideMap, true);
        g.Observer(ObsB);
        c.Update(io, Main, Nothing, true);
        g.Frame();
        Check(g.Saved() == 1 && g.Working() == 1 && g.Shown(), "observer change, pending hide withdrawn");
    }
    {
        // A transient read failure while the Hide is being applied (A1 case 2 / read failure recovery).
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        c.Update(io, Main, HideMap, true);
        g.Frame();
        g.fail_list = true;
        c.Update(io, Main, HideMap, true);
        c.Update(io, Main, HideMap, true);
        g.fail_list = false;
        c.Update(io, Main, HideMap, true);
        Check(g.Saved() == 1 && g.Working() == 1 && !g.Shown(), "transient failure mid hide: written back, hidden");
        c.Update(io, Main, Nothing, true);
        g.Frame();
        c.Update(io, Main, Nothing, true);
        Check(g.Shown() && g.Saved() == 1, "transient failure mid hide: toggle off shows it");
    }
    {
        // A transient read failure while hidden, then the toggle off: the minimap must come back.
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        HideSteady(g, c, io);
        g.fail_list = true;
        c.Update(io, Main, HideMap, true);
        g.fail_list = false;
        c.Update(io, Main, Nothing, true);
        g.Frame();
        c.Update(io, Main, Nothing, true);
        Check(g.Shown() && g.Saved() == 1, "transient failure while hidden: toggle off shows it");
    }
    {
        // The observer list stays unreadable: SystemData still gets the player's value back.
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        c.Update(io, Main, HideMap, true);
        g.Frame();
        g.fail_list = true;
        for (int i = 0; i < 40; ++i) {
            c.Update(io, Main, HideMap, true);
            g.Frame();
        }
        Check(g.Saved() == 1 && g.Working() == 1, "persistent failure mid hide: player's value restored");
        g.fail_list = false;
        c.Update(io, Main, Nothing, true);
        g.Frame();
        c.Update(io, Main, Nothing, true);
        Check(g.Shown() && g.Saved() == 1, "persistent failure mid hide: toggle off shows it");
    }
    {
        // Teardown with the Hide applied but not written back, and with it still pending.
        for (const bool applied : {true, false}) {
            Game g;
            Controller c;
            const auto io = g.MakeIo();
            c.Update(io, Main, HideMap, true);
            if (applied)
                g.Frame();
            c.Restore(io, Main);
            g.Frame();
            Check(g.Saved() == 1 && g.Working() == 1 && g.Shown(),
                  applied ? "teardown after applied hide" : "teardown with pending hide");
        }
        // Teardown with the observer changed mid hide.
        Game g;
        Controller c;
        const auto io = g.MakeIo();
        c.Update(io, Main, HideMap, true);
        g.Frame();
        g.Observer(ObsB);
        c.Restore(io, Main);
        g.Frame();
        Check(g.Saved() == 1 && g.Working() == 1 && g.Shown(), "teardown after an observer change mid hide");
    }
    if (failures)
        return 1;
    std::puts("xcx hud: all passed");
    return 0;
}
