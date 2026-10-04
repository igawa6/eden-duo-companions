// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// dsmod-mk8d-anim: the rank table's card-swap animation (mk8d_anim.h), driven at 60 Hz ticks.

#include "mk8d_anim.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
using Mk8dAnim::Input;
using Mk8dAnim::RankTable;

double T(int tick) {
    return tick * 1000.0 / 60.0; // 60 Hz runtime ticks
}

Input Grid(int count = 12) {
    Input in;
    in.count = count;
    for (int i = 0; i < count; ++i) {
        in.valid[static_cast<std::size_t>(i)] = true;
        in.rank[static_cast<std::size_t>(i)] = i + 1;
    }
    return in;
}

void Swap(Input& in, int a, int b) {
    std::swap(in.rank[static_cast<std::size_t>(a)], in.rank[static_cast<std::size_t>(b)]);
}

bool Near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

/// The first update snaps: every card on its rank, nothing moving.
void TestFirstUpdateSnaps() {
    RankTable t;
    Input in = Grid();
    Swap(in, 0, 7);
    t.Update(T(0), in);
    for (int i = 0; i < 12; ++i) {
        assert(Near(t.At(i).y, in.rank[static_cast<std::size_t>(i)]));
        assert(!t.At(i).moving && t.At(i).dx == 0.0 && t.At(i).glow == 0);
    }
    assert(t.Snaps() == 1 && t.Moving() == 0);
}

/// A single overtake: two cards, 150 ms (9 ticks at 60 Hz), ease-out; the overtaking card
/// drifts right (peak mid-slide, 0 at landing) and glows; the overtaken card does neither.
void TestNormalOvertake() {
    RankTable t;
    Input in = Grid();
    t.Update(T(0), in);
    Swap(in, 3, 4); // racer 4 (was 5th) passes racer 3 (was 4th)
    double last_up = 5.0, last_down = 4.0, peak_dx = 0.0;
    int peak_glow = 0;
    for (int k = 1; k <= 9; ++k) {
        t.Update(T(k), in);
        const auto& up = t.At(4);
        const auto& down = t.At(3);
        if (k < 10 && k > 1) {
            assert(up.y < last_up && down.y > last_down); // monotonic, no overshoot
        }
        assert(up.y >= 4.0 && up.y <= 5.0 && down.y >= 4.0 && down.y <= 5.0);
        assert(down.dx == 0.0 && down.glow == 0);
        peak_dx = std::max(peak_dx, up.dx);
        peak_glow = std::max(peak_glow, up.glow);
        last_up = up.y;
        last_down = down.y;
        if (k == 1)
            assert(t.Moving() == 2 && !t.Pileup());
        if (k < 10 && k > 1 && k < 9)
            assert(up.moving && down.moving);
    }
    // the slide started at tick 1: 150 ms later (tick 10) both have landed exactly
    assert(t.At(4).moving); // tick 9 = 133 ms in
    t.Update(T(10), in);
    assert(Near(t.At(4).y, 4.0) && Near(t.At(3).y, 5.0));
    assert(t.At(4).dx == 0.0 && t.At(4).glow == 0 && !t.At(4).moving && t.Moving() == 0);
    assert(peak_dx > 0.25 && peak_dx <= 0.30 + 1e-9);
    assert(peak_glow == 3);
    // ease-out: more than half the distance is covered in the first third
    RankTable e;
    Input g = Grid();
    e.Update(T(0), g);
    Swap(g, 3, 4);
    e.Update(T(0) + 1.0, g);
    e.Update(T(0) + 1.0 + 50.0, g);
    assert(e.At(4).y < 4.5);
    assert(e.Slides() == 2 && e.PileupSlides() == 0);
}

/// Pile-up threshold: 2 moving cards = normal (150 ms, drift), 3 = pile-up (100 ms, no drift,
/// no glow). A third card starting while two slide is a pile-up slide; the two keep 150 ms.
void TestPileupThreshold() {
    { // 3 cards at once: 4th -> 2nd jump (cards 1, 2, 3 move)
        RankTable t;
        Input in = Grid();
        t.Update(T(0), in);
        in.rank[3] = 2;
        in.rank[1] = 3;
        in.rank[2] = 4;
        for (int k = 1; k <= 6; ++k) {
            t.Update(T(k), in);
            assert(t.Pileup() && t.Moving() == 3);
            for (int i = 1; i <= 3; ++i)
                assert(t.At(i).dx == 0.0 && t.At(i).glow == 0);
        }
        t.Update(T(7), in); // 100 ms after the start at tick 1
        assert(t.Moving() == 0 && Near(t.At(3).y, 2.0) && Near(t.At(1).y, 3.0));
        assert(t.PileupSlides() == 3);
    }
    { // 2 moving, then a third starts two ticks later
        RankTable t;
        Input in = Grid();
        t.Update(T(0), in);
        Swap(in, 3, 4);
        t.Update(T(1), in);
        assert(!t.Pileup());
        t.Update(T(2), in);
        Swap(in, 7, 8);
        t.Update(T(3), in); // 2 active + 2 starting = 4 -> the two new ones are pile-up
        assert(t.Pileup() && t.Moving() == 4);
        assert(t.At(8).dx == 0.0); // pile-up: no drift
        assert(t.At(4).dx > 0.0);  // the earlier normal overtake keeps its drift
        t.Update(T(9), in);        // pile-up slides (start tick 3) end at 100 ms = tick 9
        assert(!t.At(8).moving && Near(t.At(8).y, 8.0) && t.At(4).moving);
        t.Update(T(10), in); // the normal slides (start tick 1) end at tick 10
        assert(t.Moving() == 0 && Near(t.At(4).y, 4.0));
    }
    { // exactly one card moving (the other cards' ranks unchanged: a jump into an empty slot)
        RankTable t;
        Input in = Grid(11);
        t.Update(T(0), in);
        in.rank[10] = 12;
        t.Update(T(1), in);
        assert(t.Moving() == 1 && !t.Pileup());
    }
}

/// Retarget mid-slide: the card continues from where it is drawn (no jump) toward the latest
/// rank, and that slide gets the full duration from the retarget.
void TestRetargetMidSlide() {
    RankTable t;
    Input in = Grid();
    t.Update(T(0), in);
    Swap(in, 3, 4); // racer 4: 5th -> 4th
    for (int k = 1; k <= 4; ++k)
        t.Update(T(k), in);
    const double y_before = t.At(4).y;
    const double dx_before = t.At(4).dx;
    assert(y_before > 4.0 && y_before < 5.0 && dx_before > 0.0);
    // one tick later racer 4 passes racer 2 too (4th -> 3rd); racer 2 drops to 4th
    in.rank[4] = 3;
    in.rank[2] = 4;
    t.Update(T(5), in);
    // continuity: one tick of motion at most (< 0.3 rows at 60 Hz for a 2-row slide)
    assert(std::fabs(t.At(4).y - y_before) < 0.3);
    assert(std::fabs(t.At(4).dx - dx_before) < 0.1);
    // active 1 (racer 3) + starting 2 (racers 4 and 2) = 3 -> pile-up, 100 ms from tick 5
    assert(t.Pileup());
    for (int k = 6; k <= 10; ++k) {
        t.Update(T(k), in);
        assert(t.At(4).moving && t.At(4).y > 3.0);
    }
    t.Update(T(11), in); // 100 ms after tick 5
    assert(Near(t.At(4).y, 3.0) && t.At(4).dx == 0.0 && Near(t.At(2).y, 4.0));
    // a retarget back to the rank the card left mid-slide: no jump either
    RankTable b;
    Input g = Grid();
    b.Update(T(0), g);
    Swap(g, 3, 4);
    b.Update(T(1), g);
    b.Update(T(3), g);
    const double mid = b.At(4).y;
    Swap(g, 3, 4); // swapped straight back
    b.Update(T(4), g);
    assert(std::fabs(b.At(4).y - mid) < 0.2);
    for (int k = 5; k <= 13; ++k)
        b.Update(T(k), g);
    assert(Near(b.At(4).y, 5.0) && Near(b.At(3).y, 4.0) && b.Moving() == 0);
}

/// No backlog: a card whose rank changes every reader sample (30 Hz) for a whole second lands
/// one slide duration after the LAST change, never later, and never leaves the span it covers.
void TestNoBacklog() {
    RankTable t;
    Input in = Grid();
    t.Update(T(0), in);
    int tick = 0;
    double prev = t.At(11).y;
    for (int r = 11; r >= 1; --r) { // racer 11: 12th -> 1st, one rank per 2 ticks
        in.rank[11] = r;
        for (int k = 0; k < 2; ++k) {
            t.Update(T(++tick), in);
            const double y = t.At(11).y;
            assert(y <= prev + 1e-9); // only ever toward the target
            assert(y >= 1.0 - 1e-9 && y <= 12.0);
            assert(prev - y < 1.5); // continuous: no teleport
            prev = y;
        }
    }
    const int last_change = tick - 1; // the last rank (1) arrived at this tick
    assert(t.At(11).moving);
    // (only this card moves: normal mode, 150 ms = 9 ticks after the last change)
    while (tick < last_change + 8) {
        t.Update(T(++tick), in);
        assert(t.At(11).moving);
    }
    t.Update(T(last_change + 9), in);
    assert(Near(t.At(11).y, 1.0) && !t.At(11).moving && t.At(11).dx == 0.0);
    // distance-independent duration: a 12th -> 4th jump of a single card lands in 150 ms too
    RankTable j;
    Input g = Grid();
    j.Update(T(0), g);
    g.rank[11] = 4;
    j.Update(T(1), g);
    j.Update(T(9), g);
    assert(j.At(11).moving);
    j.Update(T(10), g);
    assert(Near(j.At(11).y, 4.0) && !j.At(11).moving);
}

/// Snaps: racer count change, validity change, context change, a long gap, Reset().
void TestSnaps() {
    const auto moving_then = [](auto&& change) {
        RankTable t;
        Input in = Grid();
        t.Update(T(0), in);
        Swap(in, 3, 4);
        t.Update(T(1), in);
        t.Update(T(2), in);
        assert(t.Moving() == 2);
        const auto snaps = t.Snaps();
        double now = T(3);
        change(t, in, now);
        t.Update(now, in);
        assert(t.Snaps() == snaps + 1 && t.Moving() == 0);
        for (int i = 0; i < in.count; ++i)
            if (in.valid[static_cast<std::size_t>(i)])
                assert(Near(t.At(i).y, in.rank[static_cast<std::size_t>(i)]) && t.At(i).dx == 0.0 &&
                       t.At(i).glow == 0);
    };
    moving_then([](RankTable&, Input& in, double&) { in.count = 11; });
    { // the count grows while the new slot is not valid yet: still a snap
        RankTable t;
        Input in = Grid(11);
        t.Update(T(0), in);
        Swap(in, 3, 4);
        t.Update(T(1), in);
        assert(t.Moving() == 2);
        in.count = 12;
        in.valid[11] = false;
        t.Update(T(2), in);
        assert(t.Snaps() == 2 && t.Moving() == 0 && Near(t.At(4).y, 4.0));
    }
    moving_then([](RankTable&, Input& in, double&) { in.valid[6] = false; });
    moving_then([](RankTable&, Input& in, double&) { in.context = 7; });
    moving_then([](RankTable&, Input&, double& now) { now = T(2) + 300.0; });
    moving_then([](RankTable& t, Input&, double&) { t.Reset(); });
    moving_then([](RankTable&, Input&, double& now) { now = T(1); }); // clock went back
    // an invalid card publishes 0 and does not count as moving
    RankTable t;
    Input in = Grid();
    in.valid[5] = false;
    t.Update(T(0), in);
    assert(t.At(5).y == 0.0 && !t.At(5).moving);
    in.rank[5] = 9; // an invalid card's rank changes: no slide
    t.Update(T(1), in);
    assert(t.At(5).y == 0.0 && t.Moving() == 0);
}

/// A race start in the reader's 30 Hz cadence: the whole field reshuffles over a few samples.
/// Every slide there is a pile-up slide (100 ms), all cards land on the final ranks.
void TestRaceStartShuffle() {
    RankTable t;
    Input in = Grid();
    t.Update(T(0), in);
    const int order[3][12] = {{2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11},
                              {3, 1, 2, 5, 4, 7, 6, 9, 8, 11, 10, 12},
                              {1, 3, 2, 4, 6, 5, 8, 7, 12, 9, 11, 10}};
    int tick = 0;
    for (const auto& o : order) {
        for (int i = 0; i < 12; ++i)
            in.rank[static_cast<std::size_t>(i)] = o[i];
        for (int k = 0; k < 2; ++k) {
            t.Update(T(++tick), in);
            if (k == 0)
                assert(t.Pileup());
            for (int i = 0; i < 12; ++i)
                assert(t.At(i).dx == 0.0 && t.At(i).glow == 0);
        }
    }
    const int last = tick - 1;
    while (tick < last + 6)
        t.Update(T(++tick), in);
    assert(t.Moving() == 0);
    for (int i = 0; i < 12; ++i)
        assert(Near(t.At(i).y, in.rank[static_cast<std::size_t>(i)]));
    assert(t.Slides() == t.PileupSlides());
}

} // namespace

int main() {
    TestFirstUpdateSnaps();
    TestNormalOvertake();
    TestPileupThreshold();
    TestRetargetMidSlide();
    TestNoBacklog();
    TestSnaps();
    TestRaceStartShuffle();
    std::puts("dsmod-mk8d-anim: passed");
    return 0;
}
