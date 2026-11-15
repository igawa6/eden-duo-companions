// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD Ready page: the right-stick swipe pad (dev.38, owner Thor feedback 2026-11-12: "i
// sometime cannot swipe to slash in ready view, also no gold swipe marker on it").
//
// dev.17..37 read the pad as a pan_zoom view (@view_pan_x/y:stickpad_<i>): the module saw only the
// pan, never the finger, and recognised a lift by the host's glide home (a fifth of the way per
// tick). A stroke that started while the previous one was still gliding home (back-and-forth
// slashes, about 0.45 s from a 180 px stroke down to the 0.5 px floor) was never fed: the module
// stayed "releasing" until the pan crossed 0.5 px, which the new finger never brings it to. A short
// stroke gave a short deflection (180 px for a full one).
//
// dev.38: PanStroke reads each pad's pan the same way but tells the finger from the glide by the
// glide's own step (the host eases a released view a fifth of the way home per tick): a pan that
// does not follow that step is a finger, also one that landed while the last stroke still glided;
// its displacement is measured from the pan at that moment. The runtime gives a module no finger
// position (only the pan; "@drag_x" is published after the module runs), so Stroke works on a
// finger position made from the displacement around an anchor (the minimap centre, Link's head):
//  - the vector is the displacement from where the stroke started (kTravel px = a full deflection,
//    as before), so a slow hold-and-drag still guides the sword;
//  - a fast finger (kFlickPx within kFlickTicks ticks) is a flick: the vector is pushed to a full
//    deflection along the displacement (the native button-only slash is a full stick flick), and
//    the boost fades out over a few ticks once the finger slows;
//  - a lift keeps the last vector kHoldTicks ticks (a very short stroke still reaches the game for a
//    few frames), then the stick is neutral; a new stroke starts from zero at once;
//  - the stroke's path is kept for the gold trail (Trail): points every kTrailStep px along it, the
//    head first, fading out after the lift; drawn from the anchor, it is the stroke's shape coming
//    out of Link, like the sword's swing. dev.39: a runtime that tells where the finger is (25,
//    @view_touch_x/y) gets FingerTrail below instead, under the finger; this one stays the fallback.
// Kept free of the module so it can be unit tested (sshd_stroke_test.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace sshd {

struct StrokeVec {
    bool feed = false; // write (x, y) to the stick words this tick
    double x = 0, y = 0; // unit stick vector, +y = up
};

class Stroke {
public:
    static constexpr double kTravel = 180;   // canvas px of finger travel for a full deflection
    static constexpr double kSlop = 0;       // px the input starts after the finger landed
    static constexpr double kFlickPx = 28;   // finger travel within kFlickTicks ticks that makes a flick
    static constexpr int kFlickTicks = 3;
    static constexpr double kBoostFade = 0.8; // per tick once the finger slows
    static constexpr int kHoldTicks = 3;     // ticks the last vector is kept after the lift
    static constexpr int kTrailPoints = 24;  // trail dots published
    static constexpr double kTrailStep = 11; // canvas px between trail dots
    static constexpr int kTrailFade = 14;    // ticks the trail takes to fade out after the lift
    static constexpr int kPath = 64;         // finger samples kept (the trail's polyline)

    struct Stats { // the stroke so far (diagnostics: SSHD_DIAG stroke log)
        int ticks = 0;            // ticks the finger was down
        double x0 = 0, y0 = 0;    // where it started (slop-corrected)
        double len = 0;           // path length, px
        double peak = 0;          // peak unit deflection written
        bool flick = false;       // a flick boost happened
        int fed = 0;              // ticks the vector reached the stick words
    };

    // An input that starts only after a tap slop (a runtime drag: 12 px) moves the start back by it.
    void SetSlop(double s) { slop_ = s; }
    // Diagnostics: the trail stays at full strength `ticks` after the lift, then fades.
    void SetTrailHold(int ticks) { hold_trail_ = std::max(0, ticks); }

    // One tick. down: a finger strokes the pad this tick; (x, y): its position in canvas px (y down).
    // (sx, sy): where a new stroke began when the input already moved (a pan pad's first sample
    // holds the whole first tick of motion); unset = at (x, y).
    StrokeVec Step(bool down, double x, double y, double sx = NAN, double sy = NAN) {
        StrokeVec out;
        if (down) {
            if (!active_) {
                active_ = true;
                stats_ = Stats{};
                const bool from = std::isfinite(sx) && std::isfinite(sy);
                stats_.x0 = from ? sx : x, stats_.y0 = from ? sy : y;
                n_ = 0;
                boost_ = 0;
                corrected_ = false;
                age_ = 0;
                if (from && (sx != x || sy != y)) { // the path from the start (trail, flick speed)
                    Push(sx, sy);
                    stats_.len += std::hypot(x - sx, y - sy);
                }
                Push(x, y);
            } else {
                const auto& l = Last();
                stats_.len += std::hypot(x - l[0], y - l[1]);
                Push(x, y);
            }
            ++stats_.ticks;
            double dx = x - stats_.x0, dy = y - stats_.y0;
            const double d = std::hypot(dx, dy);
            if (!corrected_ && d > 0.5 && slop_ > 0) { // the drag began slop_ px after the finger landed
                stats_.x0 -= dx / d * slop_;
                stats_.y0 -= dy / d * slop_;
                dx = x - stats_.x0, dy = y - stats_.y0;
                corrected_ = true;
            }
            // flick: the finger's travel over the last kFlickTicks ticks
            const auto& back = At(std::min(n_ - 1, kFlickTicks));
            const bool fast = std::hypot(x - back[0], y - back[1]) >= kFlickPx;
            boost_ = fast ? 1.0 : boost_ * kBoostFade;
            stats_.flick = stats_.flick || fast;
            double vx = dx / kTravel, vy = -dy / kTravel;
            double m = std::hypot(vx, vy);
            if (m > 1.0) {
                vx /= m, vy /= m, m = 1.0;
            }
            if (m > 1e-6 && boost_ > m) { // a full flick along the displacement
                vx *= boost_ / m, vy *= boost_ / m;
                m = boost_;
            }
            last_ = {vx, vy};
            hold_ = kHoldTicks;
            out.feed = true;
            out.x = vx, out.y = vy;
            stats_.peak = std::max(stats_.peak, m);
            return out;
        }
        if (active_) { // the lift
            active_ = false;
            ended_ = true;
        }
        ++age_;
        if (hold_ > 0) {
            --hold_;
            out.feed = true;
            out.x = last_[0], out.y = last_[1];
            if (hold_ == 0)
                out.x = out.y = 0; // then neutral, once
        }
        return out;
    }

    bool Active() const { return active_; }
    // true once after a stroke ended (the caller logs Stats then)
    bool TakeEnded() {
        const bool e = ended_;
        ended_ = false;
        return e;
    }
    const Stats& LastStats() const { return stats_; }
    void Fed() { ++stats_.fed; }

    struct Dot {
        bool on = false;
        double x = 0, y = 0;
        int alpha = 0; // 0..15
        int scale = 0; // x 1000
    };
    // The gold trail: kTrailPoints dots back along the finger's path from the head (the newest
    // point), every kTrailStep px, larger and brighter at the head; after the lift the whole trail
    // fades out over kTrailFade ticks.
    std::array<Dot, kTrailPoints> Trail() const {
        std::array<Dot, kTrailPoints> dots{};
        const int age = std::max(0, age_ - hold_trail_);
        if (n_ == 0 || (!active_ && age >= kTrailFade))
            return dots;
        const double fade = active_ ? 1.0 : 1.0 - static_cast<double>(age) / kTrailFade;
        int k = 0;
        double want = 0; // distance back from the head of the next dot
        double walked = 0;
        for (int i = 0; i + 1 < n_ && k < kTrailPoints; ++i) {
            const auto& a = At(i);
            const auto& b = At(i + 1);
            const double seg = std::hypot(b[0] - a[0], b[1] - a[1]);
            while (k < kTrailPoints && want <= walked + seg) {
                const double t = seg > 1e-9 ? (want - walked) / seg : 0.0;
                Place(dots[k], a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, k, fade);
                ++k;
                want += kTrailStep;
            }
            walked += seg;
        }
        if (k == 0) // a finger that has not moved yet: one dot at the head
            Place(dots[k++], At(0)[0], At(0)[1], 0, fade);
        return dots;
    }

private:
    static void Place(Dot& d, double x, double y, int k, double fade) {
        const double u = static_cast<double>(k) / (kTrailPoints - 1); // 0 head .. 1 tail
        d.on = true;
        d.x = x, d.y = y;
        d.alpha = std::clamp(static_cast<int>(std::lround(15.0 * fade * (1.0 - 0.75 * u))), 0, 15);
        d.scale = static_cast<int>(std::lround(1000.0 * (1.0 - 0.65 * u)));
        d.on = d.alpha > 0;
    }
    void Push(double x, double y) { // newest first
        head_ = (head_ + kPath - 1) % kPath;
        path_[head_] = {x, y};
        n_ = std::min(n_ + 1, kPath);
    }
    const std::array<double, 2>& At(int i) const { return path_[(head_ + i) % kPath]; } // 0 = newest
    const std::array<double, 2>& Last() const { return At(0); }

    bool active_ = false, ended_ = false, corrected_ = false;
    double slop_ = kSlop;
    int hold_trail_ = 0;
    std::array<std::array<double, 2>, kPath> path_{};
    int head_ = 0, n_ = 0;
    int age_ = 1 << 20; // ticks since the lift
    int hold_ = 0;
    double boost_ = 0;
    std::array<double, 2> last_{0, 0};
    Stats stats_;
};

// One pan pad's finger, from its pan alone (canvas px; pan_x falls as the finger moves right, pan_y
// rises as it moves up). The host eases a released view home a fifth of the way per tick (GlideViews):
// a step p = 0.8 q is a glide, anything else with the pan away from home is a finger (also a finger
// resting still: the glide stops when it lands). down / (dx, dy): the finger's displacement since the
// stroke began, canvas px (+y down).
class PanStroke {
public:
    struct Out {
        bool down = false;
        double dx = 0, dy = 0;
    };
    // one, two or three glide steps since the last sample (the module samples a tick behind and
    // may see two host steps at once, or none: a repeat is never a new finger, see Step)
    static bool Glide(double px, double py, double qx, double qy) {
        const double q = std::fabs(qx) + std::fabs(qy), p = std::fabs(px) + std::fabs(py);
        if (!(q > 0.5 && p < q))
            return false;
        for (const double f : {0.8, 0.64, 0.512})
            if (std::fabs(px - qx * f) <= 0.02 * q + 0.05 && std::fabs(py - qy * f) <= 0.02 * q + 0.05)
                return true;
        return false;
    }
    static constexpr double kStartPx = 1.0; // pan motion that starts a stroke (a finger, not noise)
    Out Step(double px, double py) {
        Out o;
        const bool home = std::fabs(px) + std::fabs(py) <= 0.5;
        const bool glide = Glide(px, py, qx_, qy_);
        if (down_ && (glide || home))
            down_ = false; // the lift (or the finger brought the pan home: no displacement left)
        else if (!down_ && !glide && !home && std::fabs(px - qx_) + std::fabs(py - qy_) >= kStartPx) {
            down_ = true; // a finger: from where the pan was a tick ago (0, or a glide it stopped)
            bx_ = qx_, by_ = qy_;
        }
        qx_ = px, qy_ = py;
        if (down_) {
            o.down = true;
            o.dx = -(px - bx_); // the pan moves against the finger
            o.dy = -(py - by_); // pan_y rises as the finger moves up (canvas y falls)
        }
        return o;
    }
    bool Down() const { return down_; }

private:
    bool down_ = false;
    double qx_ = 0, qy_ = 0, bx_ = 0, by_ = 0;
};

// dev.39: the gold trail under the finger. Runtime 25 tells a module where a pan_zoom gesture's finger
// is (@view_touch_x/y:<id>, canvas px, one tick behind like the pan, absent while no finger is down;
// RCR-20261112-pan-pad-finger-position). An older runtime answers neither key, so nothing is ever
// recorded here and the module keeps the dev.38 trail (Stroke::Trail, drawn out of the minimap
// centre). Drawing only: the stick vector still comes from Stroke over PanStroke, exactly as in dev.38.
//  - the path starts where the finger landed (the first finger sample of the touch, which can come
//    ticks before the pan has moved the 1 px PanStroke needs to start a stroke) and follows the
//    finger samples, newest first;
//  - between samples the path is a centripetal Catmull-Rom curve (a handheld's ticks can be tens of
//    ms apart, so a flick's samples are far apart: straight segments would show corners; centripetal
//    keeps a back-and-forth turn free of loops and overshoot);
//  - the dots are Stroke's: kTrailPoints every kTrailStep px back from the head (the newest finger
//    sample), the same size / brightness ramp and the same fade after the lift;
//  - each dot is kept inside the swipe pads (SetArea): one whose sparkle would cross a pad's edge
//    into a corner button, the footer or the right column is shrunk until it fits, and dropped when
//    that leaves less than kMinFit of it. The canvas edge is no limit (nothing to cover there).
class FingerTrail {
public:
    using Dot = Stroke::Dot;
    static constexpr int kPoints = Stroke::kTrailPoints;
    static constexpr double kStep = Stroke::kTrailStep;
    static constexpr int kFade = Stroke::kTrailFade;
    static constexpr int kPath = 64;         // finger samples kept
    static constexpr double kDotHalf = 44;   // half the drawn sparkle at scale 1000 (gen_manifest TRAIL_PX / 2)
    static constexpr double kMinFit = 0.35;  // a dot squeezed under this share of its size is dropped
    static constexpr double kSameSample = 0.25; // px: a repeated finger sample adds no path point
    static constexpr int kMaxSub = 24;       // curve pieces per finger segment at most
    static constexpr double kSubPx = 3;      // ... about one per this many px

    struct Box {
        double x, y, w, h;
    };

    // Where dots may go: up to two disjoint pad rects inside the canvas.
    void SetArea(const Box* pads, int n, Box canvas) {
        n_pads_ = std::clamp(n, 0, 2);
        for (int i = 0; i < n_pads_; ++i)
            pads_[i] = pads[i];
        canvas_ = canvas;
    }
    // Diagnostics: the trail stays at full strength `ticks` after the lift, then fades (as Stroke).
    void SetHold(int ticks) { hold_ = std::max(0, ticks); }

    // A new stroke: forget the last path; (lx, ly) = where its finger landed, when known.
    void Begin(bool landed, double lx, double ly) {
        n_ = 0;
        age_ = 0;
        if (landed && std::isfinite(lx) && std::isfinite(ly))
            Push(lx, ly);
    }
    // One tick. active: the stroke is on (Stroke::Active); has / (x, y): a finger sample this tick.
    void Step(bool active, bool has, double x, double y) {
        if (!active) {
            ++age_;
            return;
        }
        age_ = 0;
        if (has && std::isfinite(x) && std::isfinite(y) &&
            (n_ == 0 || std::hypot(x - At(0)[0], y - At(0)[1]) > kSameSample))
            Push(x, y);
    }
    // This (or the fading last) stroke has finger samples: draw it instead of the anchored trail.
    bool Used() const { return n_ > 0; }
    int Samples() const { return n_; }
    std::array<double, 2> Head() const { return n_ > 0 ? At(0) : std::array<double, 2>{NAN, NAN}; }

    std::array<Dot, kPoints> Trail() const {
        std::array<Dot, kPoints> dots{};
        const int age = std::max(0, age_ - hold_);
        if (n_ == 0 || age >= kFade)
            return dots;
        const double fade = age_ == 0 ? 1.0 : 1.0 - static_cast<double>(age) / kFade;
        int k = 0;
        double want = 0, walked = 0;
        std::array<double, 2> a = At(0);
        const auto place = [&](double x, double y) {
            Dot& d = dots[k];
            const double u = static_cast<double>(k) / (kPoints - 1); // 0 head .. 1 tail
            d.alpha = std::clamp(static_cast<int>(std::lround(15.0 * fade * (1.0 - 0.75 * u))), 0, 15);
            d.scale = static_cast<int>(std::lround(1000.0 * (1.0 - 0.65 * u)));
            d.x = x, d.y = y;
            const double h = kDotHalf * d.scale / 1000.0;
            const double fit = Fit(x, y, h);
            d.on = d.alpha > 0 && fit >= kMinFit * h;
            if (d.on && fit < h)
                d.scale = static_cast<int>(std::floor(d.scale * fit / h));
            ++k;
            want += kStep;
        };
        place(a[0], a[1]); // the head: under the finger
        for (int i = 0; i + 1 < n_ && k < kPoints; ++i) {
            const auto& p0 = At(std::max(0, i - 1));
            const auto& p1 = At(i);
            const auto& p2 = At(i + 1);
            const auto& p3 = At(std::min(n_ - 1, i + 2));
            const double len = std::hypot(p2[0] - p1[0], p2[1] - p1[1]);
            const int m = std::clamp(static_cast<int>(std::ceil(len / kSubPx)), 1, kMaxSub);
            for (int j = 1; j <= m && k < kPoints; ++j) {
                const auto b = Curve(p0, p1, p2, p3, static_cast<double>(j) / m);
                const double seg = std::hypot(b[0] - a[0], b[1] - a[1]);
                while (k < kPoints && want <= walked + seg) {
                    const double t = seg > 1e-9 ? (want - walked) / seg : 0.0;
                    place(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t);
                }
                walked += seg;
                a = b;
            }
        }
        return dots;
    }

    // The largest half size <= h of a square centred on (x, y) that stays inside the pads (the part
    // past the canvas edge does not count); 0 when the centre is outside them.
    double Fit(double x, double y, double h) const {
        if (n_pads_ == 0)
            return h;
        if (Inside(x, y, h))
            return h;
        double lo = 0, hi = h;
        if (!Inside(x, y, 0))
            return 0;
        for (int it = 0; it < 10; ++it) {
            const double mid = (lo + hi) * 0.5;
            (Inside(x, y, mid) ? lo : hi) = mid;
        }
        return lo;
    }

private:
    static double Overlap(double a0, double a1, double b0, double b1) {
        return std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
    }
    // the square's part on the canvas is covered by the pads (disjoint, so their overlaps add up)
    bool Inside(double x, double y, double h) const {
        const double x0 = x - h, x1 = x + h, y0 = y - h, y1 = y + h;
        if (h <= 0) {
            for (int i = 0; i < n_pads_; ++i) {
                const Box& p = pads_[i];
                if (x >= p.x && x <= p.x + p.w && y >= p.y && y <= p.y + p.h)
                    return true;
            }
            return false;
        }
        const double on_canvas = Overlap(x0, x1, canvas_.x, canvas_.x + canvas_.w) *
                                 Overlap(y0, y1, canvas_.y, canvas_.y + canvas_.h);
        double covered = 0;
        for (int i = 0; i < n_pads_; ++i) {
            const Box& p = pads_[i];
            covered += Overlap(x0, x1, p.x, p.x + p.w) * Overlap(y0, y1, p.y, p.y + p.h);
        }
        return covered >= on_canvas - 1e-6 * std::max(1.0, on_canvas);
    }
    // centripetal Catmull-Rom from p1 (t 0) to p2 (t 1); p0 / p3 the neighbours (repeated at the ends)
    static std::array<double, 2> Curve(const std::array<double, 2>& p0, const std::array<double, 2>& p1,
                                       const std::array<double, 2>& p2, const std::array<double, 2>& p3, double t) {
        const auto knot = [](const std::array<double, 2>& a, const std::array<double, 2>& b) {
            return std::sqrt(std::hypot(b[0] - a[0], b[1] - a[1]));
        };
        const double d1 = knot(p1, p2);
        if (d1 < 1e-6)
            return p1;
        const double d0 = knot(p0, p1), d2 = knot(p2, p3);
        const bool end0 = d0 < 1e-6, end3 = d2 < 1e-6; // an end: the neighbour is the point itself
        // Hermite form of the centripetal spline (tangents from the non-uniform knots); at an end the
        // segment is a parabola (end tangent = 2 x chord - the inner one: a flick's arc keeps its bend
        // up to the head), a lone segment a straight line
        std::array<double, 2> out;
        for (int c = 0; c < 2; ++c) {
            const double chord = p2[c] - p1[c];
            double t1 = end0 ? chord : ((p1[c] - p0[c]) / d0 - (p2[c] - p0[c]) / (d0 + d1) + chord / d1) * d1;
            double t2 = end3 ? chord : (chord / d1 - (p3[c] - p1[c]) / (d1 + d2) + (p3[c] - p2[c]) / d2) * d1;
            if (end0 && !end3)
                t1 = 2 * chord - t2;
            else if (end3 && !end0)
                t2 = 2 * chord - t1;
            const double tt = t * t, ttt = tt * t;
            out[c] = (2 * ttt - 3 * tt + 1) * p1[c] + (ttt - 2 * tt + t) * t1 + (-2 * ttt + 3 * tt) * p2[c] +
                     (ttt - tt) * t2;
        }
        return out;
    }
    void Push(double x, double y) { // newest first
        head_ = (head_ + kPath - 1) % kPath;
        path_[head_] = {x, y};
        n_ = std::min(n_ + 1, kPath);
    }
    const std::array<double, 2>& At(int i) const { return path_[(head_ + i) % kPath]; } // 0 = newest

    std::array<std::array<double, 2>, kPath> path_{};
    int head_ = 0, n_ = 0;
    int age_ = 1 << 20; // ticks since the lift
    int hold_ = 0;
    std::array<Box, 2> pads_{};
    int n_pads_ = 0;
    Box canvas_{0, 0, 0, 0};
};

} // namespace sshd
