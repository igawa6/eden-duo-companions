// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mk8d_anim.h"

#include <algorithm>
#include <cmath>

namespace Mk8dAnim {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double EpsMs = 1e-6; ///< a slide whose time is up to rounding has landed

bool Done(double now, double start, double dur) {
    return now - start >= dur - EpsMs;
}

/// Cubic ease-out: fast start, gentle landing.
double EaseOut(double t) {
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}
} // namespace

void RankTable::Snap(const Input& in) {
    ++snaps;
    for (int i = 0; i < MaxCards; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const bool v = i < in.count && in.valid[k];
        slide[k] = {};
        slide[k].from = slide[k].to = v ? in.rank[k] : 0;
        cards[k] = {v ? static_cast<double>(in.rank[k]) : 0.0, 0.0, 0, false};
    }
    moving = 0;
    pileup = false;
}

Card RankTable::Eval(const Slide& s, double now) const {
    if (!s.active || s.dur <= 0.0 || Done(now, s.start, s.dur))
        return {s.to, 0.0, 0, false};
    const double t = std::clamp((now - s.start) / s.dur, 0.0, 1.0);
    const double e = EaseOut(t);
    Card c;
    c.y = s.from + (s.to - s.from) * e;
    c.dx = s.dx0 * (1.0 - e);
    if (s.up && !s.pile) {
        const double bump = std::sin(Pi * t); // 0 at both ends, 1 at mid-slide
        c.dx += cfg.drift_rows * bump;
        c.glow = static_cast<int>(std::lround(cfg.glow_levels * bump));
    }
    c.moving = true;
    return c;
}

void RankTable::Update(double now_ms, const Input& in) {
    bool snap = !have || now_ms < last_ms || now_ms - last_ms > cfg.snap_gap_ms ||
                in.count != last_count || in.context != last_context;
    for (int i = 0; i < MaxCards && !snap; ++i) {
        const auto k = static_cast<std::size_t>(i);
        snap = (i < in.count && in.valid[k]) != last_valid[k];
    }
    have = true;
    last_ms = now_ms;
    last_count = in.count;
    last_context = in.context;
    for (int i = 0; i < MaxCards; ++i)
        last_valid[static_cast<std::size_t>(i)] =
            i < in.count && in.valid[static_cast<std::size_t>(i)];
    if (snap) {
        Snap(in);
        return;
    }

    // Slides that ended by now are done; then the retargets of this update.
    std::array<bool, MaxCards> retarget{};
    int active = 0, starting = 0;
    for (int i = 0; i < MaxCards; ++i) {
        const auto k = static_cast<std::size_t>(i);
        Slide& s = slide[k];
        if (s.active && Done(now_ms, s.start, s.dur))
            s.active = false;
        if (!last_valid[k])
            continue;
        if (static_cast<double>(in.rank[k]) != s.to) {
            retarget[k] = true;
            ++starting;
        } else if (s.active) {
            ++active;
        }
    }
    if (starting > 0) {
        const bool pile = active + starting >= cfg.pileup_min_moving;
        for (int i = 0; i < MaxCards; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (!retarget[k])
                continue;
            Slide& s = slide[k];
            const Card now = Eval(s, now_ms); // continue from where the card is drawn
            s.from = now.y;
            s.dx0 = now.dx;
            s.to = in.rank[k];
            s.start = now_ms;
            s.dur = pile ? cfg.pileup_ms : cfg.normal_ms;
            s.pile = pile;
            s.up = s.to < s.from;
            s.active = true;
            ++slides_started;
            if (pile)
                ++pile_slides_started;
        }
    }
    moving = 0;
    pileup = false;
    for (int i = 0; i < MaxCards; ++i) {
        const auto k = static_cast<std::size_t>(i);
        cards[k] = last_valid[k] ? Eval(slide[k], now_ms) : Card{};
        if (cards[k].moving) {
            ++moving;
            pileup = pileup || slide[k].pile;
        }
    }
}

} // namespace Mk8dAnim
