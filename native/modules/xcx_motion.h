// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Motion helpers for the XCX companion (Eden Duo WORKFLOW step 9b). Pure, header-only and free of
// any host or asset dependency so a unit test can drive them with a fake clock.
//
// The whole bottom screen is one module-drawn picture, so the runtime's widget animations cannot
// move anything inside it. Three things animate, each as brief as the game's own menus:
//   - tab changes: the manifest's page_binds play a short fade between four manifest pages (the
//     runtime does the blending); PagePoints() below publishes which bind may fire;
//   - a large HP jump (damage, healing) rolls the number and gauge to the new value (Roll);
//   - a Skell picture that has just landed fades in (Fade).
// Nothing here runs unless something moves: Active() is false at rest, and the module then keeps
// its normal frame cadence.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace xcx {
constexpr int PageFadeMs = 180;  // manifest page_binds duration_ms
constexpr int ValueRollMs = 240; // an HP jump rolls over this long
constexpr int StillFadeMs = 240; // a landed Skell picture fades in over this long
constexpr int MotionFrameMs = 80; // module frame cadence while something moves (about 3 frames)
// Ease out: starts fast, settles, like the game's menu slide-ins.
inline double EaseOut(double t) {
    t = std::clamp(t, 0., 1.);
    const double u = 1. - t;
    return 1. - u * u * u;
}
// A displayed number that rolls to its target when the target jumps. Small changes (a fuel gauge
// draining, a one-point HP tick) follow at once: only |jump| >= threshold rolls, so continuous
// drain never keeps the roll alive.
class Roll {
    bool started_{};
    double shown_{}, from_{}, to_{};
    std::int64_t begin_{};
    int ms_{ValueRollMs};

  public:
    // Returns the value to draw at `now`. `enabled` false (reduced motion) snaps.
    double Update(double target, std::int64_t now, bool enabled, double threshold,
                  int ms = ValueRollMs) {
        if (!started_ || !enabled) {
            started_ = true;
            shown_ = from_ = to_ = target;
            return shown_;
        }
        if (target != to_) {
            // A new jump rolls from what is on screen now; a small step just retargets.
            if (std::fabs(target - shown_) >= threshold) {
                from_ = shown_;
                begin_ = now;
                ms_ = std::max(1, ms);
            } else if (!Active()) {
                from_ = target;
            }
            to_ = target;
        }
        if (from_ == to_) {
            shown_ = to_;
            return shown_;
        }
        const double t = double(now - begin_) / ms_;
        shown_ = t >= 1. ? to_ : from_ + (to_ - from_) * EaseOut(t);
        if (t >= 1.)
            from_ = to_;
        return shown_;
    }
    bool Active() const { return started_ && from_ != to_; }
    double Shown() const { return shown_; }
};
// Opacity of a picture that appears: 0 -> 1 once when it first becomes present, instantly 1 when
// motion is reduced or when it was already there when the page was first looked at.
class Fade {
    bool seen_absent_{}, present_{};
    std::int64_t begin_{};
    float alpha_{1.f};

  public:
    float Update(bool present, std::int64_t now, bool enabled, int ms = StillFadeMs) {
        if (!present) {
            seen_absent_ = true;
            present_ = false;
            alpha_ = 1.f;
            return alpha_;
        }
        if (!present_) {
            present_ = true;
            if (seen_absent_ && enabled) {
                begin_ = now;
                alpha_ = 0.f;
            }
        }
        if (alpha_ < 1.f) {
            const double t = double(now - begin_) / std::max(1, ms);
            alpha_ = t >= 1. ? 1.f : float(t); // linear: frames are few, so no front-loaded curve
            if (!enabled)
                alpha_ = 1.f;
        }
        return alpha_;
    }
    bool Active() const { return present_ && alpha_ < 1.f; }
};
// Which page_binds may fire. The manifest has five pages (Map, Party, Skell, Settings, Battle) and
// three sets of binds: `fade` plays the 180 ms fade of a tab tap, `cut` switches at once (reduced
// motion), `automatic` the 250 ms fade of a change the module made itself (the automatic Battle
// view, BATTLE-UX s1 A). Exactly one carries the page; the others read -1.
struct PagePoints {
    int fade{-1}, cut{-1}, automatic{-1};
};
inline PagePoints PageBindPoints(int page, bool reduced, bool automatic = false) {
    if (reduced)
        return PagePoints{-1, page, -1};
    return automatic ? PagePoints{-1, -1, page} : PagePoints{page, -1, -1};
}
} // namespace xcx
