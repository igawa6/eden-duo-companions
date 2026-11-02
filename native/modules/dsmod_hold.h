// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// A minimal hold filter for a visibility trigger sampled once per tick: hide only after N
// consecutive misses, and optionally show only after K consecutive hits (arm_ticks; 1 = at once). Generalizes a pattern already used twice in this companion set:
// Pokemon Sword's MenuWantFilter (sw_helper_mailbox.h, kHoldTicks -- a native-menu-hide request
// held through failed guest loads) and Luminescent Platinum's own move_done_ticks idiom
// (0100000011D90000.cpp -- a move-page-closed signal held through brief focus/transition
// bounces). Both exist because a raw per-tick game-state read can drop out for one or a few
// samples while something else in the native UI is merely settling, not because the thing the
// trigger represents actually went away.
//
// One caller today: Lumi's battle "continue" trigger (companions issue 2026-10-23 #1,
// 0100000011D90000.cpp). Sword is already published; it keeps its own MenuWantFilter copy and is
// not refactored onto this header.

#pragma once

namespace dsmod_hold {

class HoldFilter {
public:
    explicit HoldFilter(int hold_ticks, int arm_ticks = 1)
        : hold_ticks_(hold_ticks), arm_ticks_(arm_ticks < 1 ? 1 : arm_ticks) {}

    // want: this tick's raw (unsmoothed) condition. Returns the held/smoothed value: shows once
    // `want` has been true for arm_ticks_ consecutive ticks (so a short spurious pulse never
    // shows); once shown, keeps showing through up to hold_ticks_ consecutive ticks where `want`
    // is false (so one skipped or late sample does not flicker it off), then hides. A true sample
    // while shown resets the miss count, so the hold only ever covers a single unbroken run of
    // misses, never an accumulation across separate dips.
    bool Step(bool want) {
        if (want) {
            misses_ = 0;
            if (!shown_ && ++hits_ >= arm_ticks_)
                shown_ = true;
            return shown_;
        }
        hits_ = 0;
        if (shown_ && ++misses_ <= hold_ticks_)
            return true;
        return Reset();
    }

    // Hides at once and forgets the hold -- e.g. at battle end, or when something else (a drive,
    // a menu opening) makes the trigger meaningless until later true Step()s re-arm it.
    bool Reset() {
        shown_ = false;
        hits_ = 0;
        misses_ = 0;
        return false;
    }

private:
    int hold_ticks_;
    int arm_ticks_;
    bool shown_ = false;
    int hits_ = 0;
    int misses_ = 0;
};

} // namespace dsmod_hold
