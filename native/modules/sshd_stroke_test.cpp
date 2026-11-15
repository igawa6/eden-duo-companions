// SPDX-License-Identifier: GPL-3.0-or-later
// Skyward Sword HD Ready swipe pad (sshd_stroke.h, dev.38): the finger from the pan pad (PanStroke), the
// stick vector, back-to-back strokes, flicks, the lift and the gold trail; dev.39: FingerTrail, the trail
// under the finger (runtime 25), its curve between sparse samples and its fit inside the pads.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "sshd_stroke.h"

namespace {
struct Pt {
    double x, y;
};

// A stroke from a to b over `ticks` ticks of finger movement (an input with a tap slop starts kSlop px
// along; the pan pads have none), then `rest` ticks still.
std::vector<sshd::StrokeVec> Run(sshd::Stroke& s, Pt a, Pt b, int ticks, int rest = 0) {
    std::vector<sshd::StrokeVec> out;
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    const double ux = (b.x - a.x) / len, uy = (b.y - a.y) / len;
    for (int t = 0; t <= ticks; ++t) {
        const double along = std::max(len * t / ticks, sshd::Stroke::kSlop);
        out.push_back(s.Step(true, a.x + ux * along, a.y + uy * along));
    }
    for (int t = 0; t < rest; ++t)
        out.push_back(s.Step(true, b.x, b.y));
    return out;
}

std::vector<sshd::StrokeVec> Lift(sshd::Stroke& s, int ticks) {
    std::vector<sshd::StrokeVec> out;
    for (int t = 0; t < ticks; ++t)
        out.push_back(s.Step(false, 0, 0));
    return out;
}

constexpr double kAnchorX = 620, kAnchorY = 430; // the module's kStrokeAnchor

double Mag(const sshd::StrokeVec& v) {
    return std::hypot(v.x, v.y);
}
} // namespace

int main() {
    using sshd::Stroke;
    int cases = 0;
    // a fast flick right (90 px in 3 ticks, Thor-like thumb): a full deflection to the right, fed
    // from the first moving tick
    {
        Stroke s;
        const auto v = Run(s, {600, 400}, {690, 400}, 3);
        assert(v[0].feed);
        assert(Mag(v.back()) > 0.99 && v.back().x > 0.99 && std::fabs(v.back().y) < 1e-6);
        assert(s.LastStats().flick || s.Active());
        // the lift: the vector holds kHoldTicks ticks (the last one neutral), then nothing
        const auto l = Lift(s, 6);
        assert(l[0].feed && l[0].x > 0.99 && l[1].feed && l[1].x > 0.99);
        assert(l[2].feed && l[2].x == 0 && l[2].y == 0);
        assert(!l[3].feed && !l[4].feed);
        assert(s.TakeEnded() && !s.TakeEnded());
        assert(s.LastStats().flick && s.LastStats().peak > 0.99);
        ++cases;
    }
    // a slow hold-and-drag guides: proportional (90 px over 30 ticks = 0.5)
    {
        Stroke s;
        const auto v = Run(s, {600, 400}, {600, 310}, 30, 10);
        const double m = Mag(v.back());
        assert(std::fabs(m - 90.0 / Stroke::kTravel) < 0.02); // up: +y
        assert(v.back().y > 0.45 && std::fabs(v.back().x) < 1e-6);
        assert(!s.LastStats().flick);
        ++cases;
    }
    // back-and-forth slashes at Thor speed: every stroke is fed from its first tick, whatever the
    // last one did (dev.17..37 lost a stroke that began while the last pan still glided home)
    {
        Stroke s;
        int fed = 0;
        for (int i = 0; i < 60; ++i) {
            const double dir = i % 2 ? -1.0 : 1.0;
            const auto v = Run(s, {620, 420}, {620 + dir * 110, 420.0 + (i % 3 - 1) * 30}, 4);
            Lift(s, 2 + i % 3); // a new finger 2..4 ticks after the lift
            assert(s.TakeEnded());
            bool full = false;
            for (const auto& x : v)
                full = full || (x.feed && Mag(x) > 0.99 && x.x * dir > 0);
            fed += full;
        }
        assert(fed == 60);
        ++cases;
    }
    // a short flick (40 px in 2 ticks, the minimum a quick thumb makes) is still a full flick
    {
        Stroke s;
        const auto v = Run(s, {500, 500}, {500, 540}, 2);
        assert(Mag(v.back()) > 0.99 && v.back().y < -0.99);
        ++cases;
    }
    // a fast start then a slow hold: the boost fades back to the proportional guide
    {
        Stroke s;
        Run(s, {600, 400}, {660, 400}, 2);
        sshd::StrokeVec last{};
        for (int t = 0; t < 30; ++t)
            last = s.Step(true, 660, 400);
        assert(std::fabs(Mag(last) - 60.0 / Stroke::kTravel) < 0.02);
        ++cases;
    }
    // the trail: the head at the finger, dots every kTrailStep px back along the path, brighter and
    // larger at the head; it fades out after the lift and is gone after kTrailFade ticks
    {
        Stroke s;
        Run(s, {300, 600}, {700, 600}, 8);
        auto dots = s.Trail();
        assert(dots[0].on && std::fabs(dots[0].x - 700) < 0.5 && std::fabs(dots[0].y - 600) < 0.5);
        assert(dots[0].alpha == 15 && dots[0].scale == 1000);
        assert(dots[1].on && std::fabs(dots[1].x - (700 - Stroke::kTrailStep)) < 0.5);
        int on = 0;
        for (int k = 0; k < Stroke::kTrailPoints; ++k)
            if (dots[k].on) {
                ++on;
                if (k > 0)
                    assert(dots[k].alpha <= dots[k - 1].alpha && dots[k].scale < dots[k - 1].scale);
            }
        assert(on == Stroke::kTrailPoints); // 400 px of path > 24 x 11 px
        Lift(s, Stroke::kTrailFade / 2);
        dots = s.Trail();
        assert(dots[0].on && dots[0].alpha < 15 && dots[0].alpha >= 6);
        Lift(s, Stroke::kTrailFade);
        dots = s.Trail();
        for (const auto& d : dots)
            assert(!d.on);
        ++cases;
    }
    // a finger down that has not moved yet (one sample): one dot under it, no stick motion
    {
        Stroke s;
        const auto v = s.Step(true, 400, 400);
        assert(v.feed && Mag(v) < 1e-9);
        assert(s.Trail()[0].on && !s.Trail()[1].on);
        ++cases;
    }
    // PanStroke over a model of the host's pan pad (mod_input.cpp / GlideViews): a finger moves the
    // pan against itself (pan_x -= dx, pan_y += up), a lift glides it a fifth of the way home per tick
    // until it lands at 0 (within 0.5 px); a finger that lands stops the glide where it is.
    // Back-and-forth slashes, the next finger landing 2..24 ticks (33..400 ms) after the lift:
    // dev.17..37's logic (Legacy below, LegacyPadMirror) feeds only the strokes that land after
    // the glide has ended; PanStroke feeds every one from its first pixel.
    {
        struct Host {
            double px = 0, py = 0;
            bool gliding = false;
            void Finger(double dx, double dy) { gliding = false, px -= dx, py -= dy; } // dy: canvas
            void Tick() {
                if (!gliding)
                    return;
                px *= 0.8, py *= 0.8;
                if (std::fabs(px) + std::fabs(py) <= 0.5)
                    px = py = 0, gliding = false;
            }
        };
        struct Legacy { // LegacyPadMirror's decision, verbatim
            double x0 = 0, y0 = 0;
            bool dragging = false, releasing = false;
            bool Step(double px, double py) {
                const double prev = std::fabs(x0) + std::fabs(y0);
                const bool gliding = dragging && prev > 2.0 && std::fabs(px) + std::fabs(py) < prev &&
                                     std::fabs(px - x0 * 0.8) <= 0.02 * prev + 0.05 &&
                                     std::fabs(py - y0 * 0.8) <= 0.02 * prev + 0.05;
                const bool held = std::fabs(px) + std::fabs(py) > 0.5 && !gliding && !releasing;
                if (gliding)
                    releasing = true;
                if (std::fabs(px) + std::fabs(py) <= 0.5)
                    releasing = false;
                x0 = px, y0 = py;
                dragging = std::fabs(px) + std::fabs(py) > 0.5;
                return held;
            }
        };
        Host h;
        Legacy legacy;
        sshd::PanStroke pan;
        Stroke s;
        int legacy_fed = 0, new_fed = 0, new_full = 0, gaps_lost = 0;
        constexpr int kStrokes = 60;
        for (int i = 0; i < kStrokes; ++i) {
            const double dir = i % 2 ? -1.0 : 1.0;
            bool lf = false, nf = false, full = false;
            // 110 px in 4 ticks, then 2 still ticks before the lift (a test driver / a thumb)
            for (int t = 0; t < 6; ++t) {
                if (t < 4)
                    h.Finger(dir * 27.5, 0);
                // the module reads last tick's pan (one tick behind) and the host steps after it
                lf = legacy.Step(h.px, h.py) || lf;
                const auto o = pan.Step(h.px, h.py);
                const auto v = s.Step(o.down, kAnchorX + o.dx, kAnchorY + o.dy);
                nf = nf || (o.down && v.feed && Mag(v) > 0.05);
                full = full || (o.down && Mag(v) > 0.99 && v.x * dir > 0);
                h.Tick();
            }
            h.gliding = true; // the lift
            const int gap = 2 + (i * 7) % 23;
            for (int t = 0; t < gap; ++t) {
                legacy.Step(h.px, h.py);
                const auto o = pan.Step(h.px, h.py);
                s.Step(o.down, kAnchorX + o.dx, kAnchorY + o.dy);
                h.Tick();
            }
            legacy_fed += lf;
            new_fed += nf;
            new_full += full;
            gaps_lost += !lf;
        }
        std::printf("pan pad model, %d back-and-forth strokes: dev.17..37 logic fed %d, PanStroke fed %d (%d full flicks)\n",
                    kStrokes, legacy_fed, new_fed, new_full);
        assert(new_fed == kStrokes && new_full == kStrokes);
        assert(legacy_fed < kStrokes / 2 && gaps_lost > 0); // the root cause, reproduced
        ++cases;
    }
    // PanStroke alone: a resting finger is down (no glide step), a lift is seen on the first glide
    // step, the sign convention (finger right / up = +dx / -dy canvas)
    {
        sshd::PanStroke p;
        assert(!p.Step(0, 0).down);
        auto o = p.Step(-30, 0); // finger 30 px right
        assert(o.down && std::fabs(o.dx - 30) < 1e-9 && std::fabs(o.dy) < 1e-9);
        o = p.Step(-30, 40); // then 40 px up
        assert(o.down && std::fabs(o.dy + 40) < 1e-9);
        o = p.Step(-30, 40); // resting still
        assert(o.down);
        o = p.Step(-24, 32); // the glide's first step: the lift
        assert(!o.down);
        o = p.Step(-19.2, 25.6);
        assert(!o.down);
        o = p.Step(-19.2, 25.6); // a repeat (a finger landed, or a sample with no host step): not yet
        assert(!o.down);
        o = p.Step(-49.2, 25.6); // the finger moves 30 px right from there: a stroke from that point
        assert(o.down && std::fabs(o.dx - 30) < 1e-9);
        // the glide's tail below 2 px and two host steps between samples are still the glide
        sshd::PanStroke g;
        g.Step(-1.5, 0.6);
        assert(!g.Step(-1.2, 0.48).down && !g.Step(-0.768, 0.307).down && !g.Step(0, 0).down);
        ++cases;
    }

    // --- dev.39 FingerTrail: the trail under the finger (runtime 25)
    using sshd::FingerTrail;
    const FingerTrail::Box pads[2] = {{220, 0, 800, 916}, {0, 220, 220, 640}}; // the module's kPadRects
    const FingerTrail::Box canvas = {0, 0, 1240, 1080};
    const auto on_dots = [](const std::array<Stroke::Dot, Stroke::kTrailPoints>& d) {
        int n = 0;
        for (const auto& x : d)
            n += x.on ? 1 : 0;
        return n;
    };
    // no finger samples (an older runtime): unused, nothing drawn (the module draws Stroke's trail)
    {
        FingerTrail f;
        f.SetArea(pads, 2, canvas);
        f.Begin(false, NAN, NAN);
        for (int t = 0; t < 5; ++t)
            f.Step(true, false, 0, 0);
        assert(!f.Used() && on_dots(f.Trail()) == 0);
        ++cases;
    }
    // Thor timing: a curved flick seen as 6 samples 40..60 px apart (a quarter circle, radius 150,
    // around (600, 500)). The dots follow the circle, not the chords between samples: evenly spaced
    // (kTrailStep along the curve), within 0.5 px of the circle (a chord's middle is 1.85 px off), and
    // turning smoothly (no corner at a sample; measured 0.05 px, 0.076 rad)
    {
        FingerTrail f;
        f.SetArea(pads, 2, canvas);
        const double cx = 600, cy = 500, r = 150;
        f.Begin(true, cx + r, cy);
        for (int i = 1; i <= 5; ++i) {
            const double a = -1.5707963 * i / 5; // up and to the left
            f.Step(true, true, cx + r * std::cos(a), cy + r * std::sin(a));
        }
        const auto d = f.Trail();
        assert(f.Used() && f.Samples() == 6);
        assert(std::fabs(d[0].x - cx) < 1e-4 && std::fabs(d[0].y - (cy - r)) < 1e-4); // the head: the finger
        int n = 0;
        double worst_r = 0, worst_turn = 0;
        for (int k = 0; k < Stroke::kTrailPoints && d[k].on; ++k, ++n) {
            worst_r = std::max(worst_r, std::fabs(std::hypot(d[k].x - cx, d[k].y - cy) - r));
            if (k >= 1) {
                const double step = std::hypot(d[k].x - d[k - 1].x, d[k].y - d[k - 1].y);
                assert(step > 10.5 && step <= 11.0 + 1e-6); // the curve is walked in short pieces
            }
            if (k >= 2) {
                const double a1 = std::atan2(d[k - 1].y - d[k - 2].y, d[k - 1].x - d[k - 2].x);
                const double a2 = std::atan2(d[k].y - d[k - 1].y, d[k].x - d[k - 1].x);
                worst_turn = std::max(worst_turn, std::fabs(std::remainder(a2 - a1, 6.2831853)));
            }
        }
        std::printf("arc: %d dots, worst radius error %.3f px, worst turn %.3f rad\n", n, worst_r, worst_turn);
        assert(n == 22); // a 235.6 px arc: dots at 0, 11, ... 231 px
        assert(worst_r < 0.5);
        assert(worst_turn < 0.09); // 11 px on a 150 px circle turns 0.073 rad; a chord corner turns 0.31
        ++cases;
    }
    // a back-and-forth slash (the finger reverses between two samples): the curve does not overshoot
    // past where the finger went, and the dots keep their head-to-tail ramp
    {
        FingerTrail f;
        f.SetArea(pads, 2, canvas);
        f.Begin(true, 400, 500);
        for (const double x : {470.0, 560.0, 620.0, 560.0, 470.0})
            f.Step(true, true, x, 500 + (x - 400) * 0.1);
        const auto d = f.Trail();
        for (int k = 0; k < Stroke::kTrailPoints; ++k) {
            if (!d[k].on)
                continue;
            assert(d[k].x <= 620.5 && d[k].x >= 399.5);
            if (k > 0)
                assert(d[k].alpha <= d[k - 1].alpha && d[k].scale <= d[k - 1].scale);
        }
        assert(d[0].alpha == 15 && d[0].scale == 1000);
        ++cases;
    }
    // inside the pads: shrunk near a button, dropped over one, full size at the canvas edge and across
    // the joint of the two pads
    {
        FingerTrail f;
        f.SetArea(pads, 2, canvas);
        assert(f.Fit(600, 400, 44) == 44);              // the middle of the minimap column
        assert(f.Fit(600, 10, 44) == 44);               // the canvas top: no limit
        assert(f.Fit(10, 500, 44) == 44);               // the left pane at the canvas edge
        assert(f.Fit(222, 500, 44) == 44);              // across the joint of the two pads
        assert(std::fabs(f.Fit(1000, 500, 44) - 20) < 0.1); // 20 px from the right column
        assert(std::fabs(f.Fit(222, 200, 44) - 2) < 0.1);   // left of it, above the left pane: the shield corner
        assert(std::fabs(f.Fit(600, 900, 44) - 16) < 0.1);  // 16 px above the footer
        assert(f.Fit(1100, 500, 44) == 0 && f.Fit(100, 100, 44) == 0 && f.Fit(600, 1000, 44) == 0);
        f.Begin(true, 900, 300);
        f.Step(true, true, 1000, 300);
        auto d = f.Trail();
        assert(d[0].on && d[0].scale >= 450 && d[0].scale <= 455); // 20 px of room: 20 / 44 of the size
        assert(d[0].x + 44.0 * d[0].scale / 1000 <= 1020.01);
        f.Step(true, true, 1012, 300);               // 8 px of room: under kMinFit, not drawn
        d = f.Trail();
        assert(!d[0].on && d[1].on && std::fabs(d[1].x - 1001) < 1e-6);
        ++cases;
    }
    // the lift: the trail stays at full strength while the stroke is on, fades over kTrailFade ticks
    // (after the diagnostics hold, if any) and a new stroke starts a new path
    {
        FingerTrail f;
        f.SetArea(pads, 2, canvas);
        f.SetHold(5);
        f.Begin(true, 500, 500);
        f.Step(true, true, 560, 500);
        f.Step(true, false, 0, 0); // a tick without a finger sample: kept as it was
        assert(f.Trail()[0].x == 560 && f.Trail()[0].alpha == 15);
        for (int t = 0; t < 5; ++t)
            f.Step(false, false, 0, 0);
        assert(f.Trail()[0].alpha == 15); // the hold
        int left = 0;
        while (on_dots(f.Trail()) > 0 && left < 40) {
            f.Step(false, false, 0, 0);
            ++left;
        }
        assert(left == Stroke::kTrailFade);
        f.Begin(true, 300, 600);
        f.Step(true, true, 300, 640);
        assert(f.Samples() == 2 && f.Trail()[0].y == 640 && f.Trail()[0].alpha == 15);
        ++cases;
    }
    std::printf("sshd stroke: %d cases ok\n", cases);
    return 0;
}
