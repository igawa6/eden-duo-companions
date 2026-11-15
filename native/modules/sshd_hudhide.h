// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD (1.0.1) top-screen HUD hiding (owner 2026-11-11: "hide top screen dpad and
// rupee"; standing rule: what the bottom screen shows better is hidden on top by default, with a
// setting to show it again). The guest helper's draw hook (native/helper/sshd_stick_helper.S,
// main+0x1007440 Pane::Draw) skips every pane in a mailbox list, subtree included, for as long as
// the module keeps refreshing the list's ttl. Nothing in a pane is written, so the native state the
// bottom D-pad mirror reads (visible bits, effective alpha, selectors) stays exactly what the game
// would draw: the mirror keeps working while the top copy is hidden.
//
// This file resolves which panes go in the list (by name, from the same validated widget roots as
// the D-pad reader, D-pad reader analysis "Selective top-screen hiding") and runs
// the small state machine that decides when the list is written, refreshed or cleared.

#pragma once

#include <cstdint>
#include <vector>

#include "sshd_dpad.h"

namespace sshd {

enum HideGroup : uint8_t {
    kHideDpad = 1,  // Cross base + Down / Left / Right arms, icons and captions, the dowsing prompt
                    // and selected target set, the Help / Close prompt
    kHideRupee = 2, // the rupee counter (meter N_rupyAll_00 and the rupy_01 layout's own group)
};

/// One pane the draw hook skips: its address and the first 8 bytes of its name (+0xEC), which the
/// hook compares as well, so a freed pane whose address was reused is never hidden.
struct HideEntry {
    uint64_t pane = 0;
    uint32_t name_lo = 0, name_hi = 0;
    uint8_t group = 0;
    bool operator==(const HideEntry&) const = default;
};

struct HideTargets {
    bool valid = false; // meter / widget identities validated this sample
    bool help = false;  // the Help widget exists (absent on the title and briefly around loads)
    std::vector<HideEntry> entries;
};

constexpr int kHideMax = 16; // entries per mailbox bank (+0x210 / +0x310, 16 bytes each)
// game frames a written list stays valid without a refresh (dev.37: was 30). The module refreshes
// it every sample; only a module that stopped sampling for ~2 s (unloaded, stalled) lets it lapse.
// A Thor that samples late or in bursts no longer lets the panes flash between two refreshes.
constexpr uint32_t kHideTtl = 120;
constexpr uint32_t kHideTtlLegacy = 30; // dev.29..36 (diagnostics replay of the old behaviour)

/// Finds the panes to hide. Pane lookups are cached for the validated allocation (meter, widget
/// roots, Help); every cached pane's name is checked again each sample, and the panes are looked up
/// again only when one of those identities really changes. Never throws.
class HideResolver {
public:
    HideTargets Sample(const DpadRead& rd, uint64_t main);
    void Reset();
    /// Pane lookups so far (diagnostics: pane churn across area / room loads).
    int Resolves() const { return resolves_; }

private:
    bool Resolve(const DpadRead& rd, uint64_t c_root, uint64_t d_root, uint64_t h_root, uint64_t c_owner);
    uint64_t key_[6]{};
    bool cached_ = false;
    int resolves_ = 0;
    std::vector<HideEntry> entries_;
};

/// What the module writes to the mailbox this sample.
struct HideStep {
    enum Kind { kNone, kRefresh, kWrite, kClear } kind = kNone;
    std::vector<HideEntry> list; // kWrite: the new list
};

/// The hide / restore state machine (dev.37). `show_dpad` / `show_rupee` are the settings (On =
/// the native top copy is shown, the default Off hides it); `helper_ok` = the load plan matched
/// and its loop hook is beating. With valid targets the list is written once and its ttl refreshed
/// every sample, in play and in the menus alike (the listed panes are the in-play meter's own; the
/// menus draw their own layouts). A sample that cannot validate the targets (an off-thread read
/// mid-update, the meter rebuilt around a load, the title) or a stalled helper (the game loop not
/// running, nothing is drawn) only stops the refresh: the written list keeps hiding until the
/// targets validate again, else its ttl lapses by itself. The list is cleared only when a setting
/// shows the copy again (and on configure / unload). `legacy` replays dev.29..36 for diagnostics:
/// cleared at once in menus, without Help, on any refused sample and on a stalled helper; `menu` is
/// the module's debounced pause / map state (dev.48: the layout controller's state the gates use, no
/// longer the game-thread stack slot dev.29..47 read here).
class HudHide {
public:
    HideStep Step(bool show_dpad, bool show_rupee, bool helper_ok, const HideTargets& t, bool legacy = false,
                  bool menu = false);
    /// The mailbox list currently holds entries (Destroy / Configure clear it).
    bool Active() const { return active_; }
    void Reset() {
        active_ = false;
        applied_.clear();
    }

private:
    bool active_ = false;
    std::vector<HideEntry> applied_;
};

} // namespace sshd
