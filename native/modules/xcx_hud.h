// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Top-screen HUD on/off without touching the player's saved settings (contracts/OPEN-SKELL-HUD.md
// section 2).
//
// The bottom screen shows the minimap (Map page) and the Skell fuel gauge (Skell page) better, so
// the top copies are hidden while the companion runs (DESIGN-TASTE "HUD on top" rule), one Settings
// toggle each. Nothing is patched and no button is pressed. The player's saved Display Settings
// (SystemData+0x58+k, written to the systemdata file when native Settings closes with a change)
// always hold the player's own values while the field is live, so the save never ends up with the
// minimap or the gauge off because of the companion (e.g. after uninstalling it).
//
// How the game shows them (static RE 20261123-hud-options + live 20261123-open-hud2):
// - Options manager *(main+0x1f90530): working copy of the 0x9f-byte options block at +0x5fd4
//   (HUD bools at +0x6020+k), committed on the next frame when +0x5fd0 is set (fn_3410a0): it
//   notifies its observers (list: head index byte +0x5f21, entries +0x5f18+i*16 = object u64, next
//   index byte +9), then copies the block into SystemData.
// - The field HUD observer (vtable main+0x1c54c90, +0x38 = fn_41ec18) keeps its own copy of the HUD
//   bools at object+0x1b8+k and copies the new block there on every commit.
// - Mini-map (k1): fn_41ec18 posts a show/hide message to the minimap widget only when its copy
//   differs from the committed value; the widget keeps that state through loads, menus and native
//   Settings until the next such message. SystemData itself is not read again.
// - Skell Fuel Gauge (k4): the gauge follows the observer's copy byte (object+0x1bc) every frame.
//
// So: the fuel gauge is hidden by writing object+0x1bc only (runtime state, never saved). The
// minimap needs the message: the module commits Mini-map = Hide through the manager, and as soon as
// the game has applied it (next frame) writes SystemData, the working copy and the observer copy
// back to the player's Show in one atomic batch; the widget stays hidden. Showing it again is the
// reverse: SystemData and the observer copy set to Hide and a commit of the player's Show in one
// batch, which the game turns into the show message and SystemData = Show on the next frame. The
// only moments SystemData differs from the player's value are those single frames, and only while
// the field is live (the systemdata file is written from native Settings, not from autosaves).
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
namespace xcx::hud {
inline constexpr std::uint64_t SystemDataSlot = 0x1dc1408, OptionsManagerSlot = 0x1f90530;
inline constexpr std::uint32_t BlockOffset = 0xc, BlockSize = 0x9f, HudOffset = 0x58;
inline constexpr std::uint32_t WorkingCopy = 0x5fd4, CommitFlag = 0x5fd0;
inline constexpr std::uint32_t ObserverHead = 0x5f21, ObserverEntries = 0x5f18, ObserverNext = 9;
inline constexpr std::uint64_t FieldHudVtable = 0x1c54c90; // main-relative
inline constexpr std::uint32_t ObserverCopy = 0x1b8;       // object+0x1b8+k
enum Element : unsigned { Map = 0, Fuel = 1, Count = 2 };
// k of each element (SystemData+0x58+k): Mini-map k1 (+0x59), Skell Fuel Gauge k4 (+0x5c); found
// live by toggling each in Settings > Field UI (OPEN-SKELL-HUD.md).
inline constexpr std::array<std::uint32_t, Count> ElementK{1, 4};
inline constexpr std::uint32_t HudIndex(std::uint32_t k) { return HudOffset - BlockOffset + k; }
struct Io {
    std::function<bool(std::uint64_t, void *, std::size_t)> read;
    // atomic: every (address, expect, value) applies or none does
    struct Op {
        std::uint64_t address;
        std::uint32_t size;
        const void *expect;
        const void *value;
    };
    std::function<bool(const Op *, std::size_t)> batch;
};
enum class State : std::uint8_t { Unknown, Shown, Hidden, Busy, Failed };
class Controller {
  public:
    // want[e]: the companion wants element e hidden (screen up, toggle on). field_live: the field is
    // running (no menu, loading or scene): minimap commits happen only then. Returns false when
    // nothing could be read (no save loaded yet); call again on a later sample. Busy() asks for an
    // early next call (a commit is waiting to be applied).
    bool Update(const Io &io, std::uint64_t main, const std::array<bool, Count> &want, bool field_live) {
        std::uint64_t sys{}, mgr{};
        if (!io.read || !io.read(main + SystemDataSlot, &sys, 8) || !io.read(main + OptionsManagerSlot, &mgr, 8) ||
            !Plausible(sys) || !Plausible(mgr)) {
            state_.fill(State::Unknown);
            return false;
        }
        std::array<std::uint8_t, BlockSize> block{};
        if (!io.read(sys + BlockOffset, block.data(), block.size()))
            return false;
        const auto found = FindObserver(io, main, mgr);
        if (!found.ok) {
            // A read of the observer list failed: not a new HUD. Keep everything (a commit of ours keeps its
            // phase and finishes on a later call). If it keeps failing, still put the player's value back
            // into SystemData and the working copy, so the save can never keep our Hide.
            if (++unknown_ >= MaxUnknown && phase_ != Phase::Idle)
                Settle(io, sys, mgr);
            return true;
        }
        unknown_ = 0;
        if (found.obj != obs_) { // another HUD (first call, title/load): a new widget starts as the game made it
            // A commit of ours in flight is settled first (the player's value back in SystemData and the
            // working copy); until that succeeds the old state is kept and this is retried.
            const bool ours = phase_ != Phase::Idle || settled_;
            if (phase_ != Phase::Idle) {
                if (!Settle(io, sys, mgr)) {
                    state_[Map] = State::Busy;
                    return true;
                }
                if (!io.read(sys + BlockOffset, block.data(), block.size()))
                    return false;
            }
            obs_ = found.obj;
            phase_ = Phase::Idle;
            // A new widget made while our commit was in flight (SystemData said Hide) starts hidden: its copy
            // says Hide while the player's value says Show. Counted as hidden, so a toggle off shows it again.
            // Any other new HUD starts as before (shown; a wanted Hide is committed as usual).
            std::uint8_t copy = 2;
            map_hidden_ = ours && obs_ && block[HudIndex(ElementK[Map])] == 1 &&
                          io.read(obs_ + ObserverCopy + ElementK[Map], &copy, 1) && copy == 0;
        }
        if (!obs_) {
            state_.fill(State::Failed); // not this game build, or no field HUD yet: never write anything
            return true;
        }
        const auto k_map = ElementK[Map], k_fuel = ElementK[Fuel];
        UpdateFuel(io, block[HudIndex(k_fuel)], want[Fuel]);
        UpdateMap(io, sys, mgr, block, block[HudIndex(k_map)], want[Map], field_live);
        settled_ = false;
        return true;
    }
    // Puts back every runtime state this controller changed (toggle off, teardown).
    // At teardown the game runs no frame in between: a Hide commit not yet applied is withdrawn; an
    // applied one gets its write-back and the Show commit, which the game applies on its next frame.
    void Restore(const Io &io, std::uint64_t main) {
        std::uint64_t sys{}, mgr{};
        if (phase_ != Phase::Idle && io.read && io.read(main + SystemDataSlot, &sys, 8) &&
            io.read(main + OptionsManagerSlot, &mgr, 8) && Plausible(sys) && Plausible(mgr))
            Settle(io, sys, mgr);
        Update(io, main, {}, true); // write-back of an applied Hide (then hidden), fuel copy back
        Update(io, main, {}, true); // the Show commit
    }
    State StateOf(Element e) const { return state_[e]; }
    bool Hidden(Element e) const { return e == Map ? map_hidden_ : fuel_hidden_; }
    bool Busy() const { return phase_ != Phase::Idle; }
    unsigned Commits() const { return commits_; }
    std::uint64_t Observer() const { return obs_; }

  private:
    enum class Phase : std::uint8_t { Idle, HideApplying, ShowApplying };
    static bool Plausible(std::uint64_t p) { return p >= 0x8000000 && p < (1ull << 40) && !(p & 3); }
    // ok = false: a read failed (the list may be changing); obj = 0: the list holds no field HUD.
    struct Found {
        bool ok;
        std::uint64_t obj;
    };
    static Found FindObserver(const Io &io, std::uint64_t main, std::uint64_t mgr) {
        std::uint8_t idx = 0;
        if (!io.read(mgr + ObserverHead, &idx, 1))
            return {false, 0};
        for (int n = 0; idx && n < 32; ++n) {
            const std::uint64_t e = mgr + ObserverEntries + std::uint64_t(idx) * 16;
            std::uint64_t obj{}, vt{};
            if (!io.read(e, &obj, 8) || !Plausible(obj) || !io.read(obj, &vt, 8))
                return {false, 0};
            if (vt == main + FieldHudVtable)
                return {true, obj};
            if (!io.read(e + ObserverNext, &idx, 1))
                return {false, 0};
        }
        return {true, 0};
    }
    // Ends a commit of ours without the observer: SystemData and the working copy get the player's value
    // (player_map_) back. A Hide not yet applied is withdrawn; an applied one is written back (the widget
    // stays hidden: map_hidden_). A pending Show commit carries the player's value itself. The observer's
    // copy is not touched (that object may be gone). False: nothing could be done yet (call again).
    bool Settle(const Io &io, std::uint64_t sys, std::uint64_t mgr) {
        const auto k = ElementK[Map];
        const std::uint64_t sys_byte = sys + HudOffset + k, work_byte = mgr + WorkingCopy + HudIndex(k);
        const std::uint8_t player = player_map_, hide = 0;
        static constexpr std::uint8_t on = 1, off = 0;
        settled_ = true;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::uint8_t busy = 2, sys_now = 2, work_now = 2;
            if (!io.read(mgr + CommitFlag, &busy, 1) || !io.read(sys_byte, &sys_now, 1) ||
                !io.read(work_byte, &work_now, 1))
                return false;
            if (busy) {
                if (phase_ == Phase::ShowApplying || work_now != hide) {
                    phase_ = Phase::Idle; // our Show (or another commit) is pending: it carries the player's value
                    return true;
                }
                const Io::Op ops[] = {{mgr + CommitFlag, 1, &on, &off}, {work_byte, 1, &hide, &player}};
                if (io.batch && io.batch(ops, 2)) {
                    phase_ = Phase::Idle;
                    return true;
                }
                continue; // the game applied it meanwhile: look again
            }
            if (sys_now != hide) { // nothing of ours left in SystemData
                phase_ = Phase::Idle;
                return true;
            }
            const Io::Op ops[] = {{sys_byte, 1, &hide, &player}, {work_byte, 1, &hide, &player}};
            if (io.batch && io.batch(ops, work_now == hide ? 2 : 1)) {
                if (phase_ == Phase::HideApplying)
                    map_hidden_ = true; // the widget got the hide message
                phase_ = Phase::Idle;
                return true;
            }
        }
        return false;
    }
    void UpdateFuel(const Io &io, std::uint8_t player, bool want) {
        if (player > 1) {
            state_[Fuel] = State::Failed;
            return;
        }
        const std::uint8_t target = want && player == 1 ? 0 : player;
        std::uint8_t now = 2;
        if (!io.read(obs_ + ObserverCopy + ElementK[Fuel], &now, 1) || now > 1) {
            state_[Fuel] = State::Failed;
            return;
        }
        if (now != target) {
            const Io::Op op{obs_ + ObserverCopy + ElementK[Fuel], 1, &now, &target};
            if (!io.batch || !io.batch(&op, 1)) {
                state_[Fuel] = State::Busy;
                return;
            }
        }
        fuel_hidden_ = target == 0 && player == 1;
        state_[Fuel] = target ? State::Shown : State::Hidden;
    }
    void UpdateMap(const Io &io, std::uint64_t sys, std::uint64_t mgr, const std::array<std::uint8_t, BlockSize> &block,
                   std::uint8_t now, bool want, bool field_live) {
        const auto k = ElementK[Map];
        const std::uint64_t sys_byte = sys + HudOffset + k, work_byte = mgr + WorkingCopy + HudIndex(k),
                            obs_byte = obs_ + ObserverCopy + k;
        if (now > 1) {
            state_[Map] = State::Failed;
            return;
        }
        std::uint8_t busy = 1;
        if (!io.read(mgr + CommitFlag, &busy, 1))
            return;
        if (phase_ == Phase::HideApplying) {
            if (busy) {
                state_[Map] = State::Busy;
                return;
            }
            if (now != 0) { // the game committed something else in between: our Hide was dropped
                phase_ = Phase::Idle;
            } else {
                // Applied: the widget got the hide message. Put the player's Show back everywhere.
                static constexpr std::uint8_t hide = 0;
                const std::uint8_t show = player_map_;
                const Io::Op ops[] = {
                    {sys_byte, 1, &hide, &show},
                    {work_byte, 1, &hide, &show},
                    {obs_byte, 1, &hide, &show},
                };
                if (!io.batch || !io.batch(ops, 3)) {
                    state_[Map] = State::Busy; // retried on the next (early) call
                    return;
                }
                map_hidden_ = true;
                phase_ = Phase::Idle;
                state_[Map] = State::Hidden;
                return;
            }
        }
        if (phase_ == Phase::ShowApplying) {
            if (busy) {
                state_[Map] = State::Busy;
                return;
            }
            phase_ = Phase::Idle;
            if (now == 1)
                map_hidden_ = false;
        }
        if (now == 0 && map_hidden_) // the player chose Hide in native Settings: the game hid it itself
            map_hidden_ = false;
        const bool hide = want && now == 1;
        state_[Map] = map_hidden_ ? State::Hidden : State::Shown;
        if (hide == map_hidden_ || !field_live || busy)
            return;
        std::array<std::uint8_t, BlockSize> working{};
        std::uint8_t obs_now = 2;
        if (!io.read(mgr + WorkingCopy, working.data(), working.size()) || !io.read(obs_byte, &obs_now, 1) ||
            obs_now > 1) {
            state_[Map] = State::Busy;
            return;
        }
        auto next = block; // the player's whole block, so no other option changes
        static constexpr std::uint8_t idle = 0, go = 1, hide_v = 0, show_v = 1;
        if (hide) {
            next[HudIndex(k)] = 0;
            const Io::Op ops[] = {
                {mgr + WorkingCopy, 64, working.data(), next.data()},
                {mgr + WorkingCopy + 64, 64, working.data() + 64, next.data() + 64},
                {mgr + WorkingCopy + 128, BlockSize - 128, working.data() + 128, next.data() + 128},
                {mgr + CommitFlag, 1, &idle, &go},
            };
            if (io.batch && io.batch(ops, 4)) {
                player_map_ = now; // the player's own value (Show): put back once the game applied the Hide
                phase_ = Phase::HideApplying;
                state_[Map] = State::Busy;
                ++commits_;
            }
        } else {
            // Show: SystemData and the observer copy say Hide for one frame, the commit carries Show.
            const Io::Op ops[] = {
                {sys_byte, 1, &show_v, &hide_v},
                {obs_byte, 1, &obs_now, &hide_v},
                {mgr + WorkingCopy, 64, working.data(), next.data()},
                {mgr + WorkingCopy + 64, 64, working.data() + 64, next.data() + 64},
                {mgr + WorkingCopy + 128, BlockSize - 128, working.data() + 128, next.data() + 128},
                {mgr + CommitFlag, 1, &idle, &go},
            };
            if (io.batch && io.batch(ops, 6)) {
                player_map_ = now;
                phase_ = Phase::ShowApplying;
                state_[Map] = State::Busy;
                ++commits_;
            }
        }
    }
    static constexpr unsigned MaxUnknown = 16; // failed observer reads in a row before a commit is settled anyway
    std::uint64_t obs_{};
    unsigned unknown_{};
    std::uint8_t player_map_{1}; // the player's Mini-map value when our last commit was made
    bool map_hidden_{}, fuel_hidden_{};
    bool settled_{}; // Settle() ended a commit of ours since the observer was last seen unchanged
    Phase phase_{Phase::Idle};
    std::array<State, Count> state_{State::Unknown, State::Unknown};
    unsigned commits_{};
};
} // namespace xcx::hud
