// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-only test for dsmod_hold::HoldFilter: shows at once, holds through a short run of misses,
// a hit before the hold expires resets the miss count, and Reset() hides at once and forgets the
// hold. No emulator, no game dump, no game-specific state.

#include "dsmod_hold.h"

#include <cassert>
#include <cstdio>

using dsmod_hold::HoldFilter;

void TestHiddenUntilFirstWant() {
    HoldFilter f(6);
    assert(f.Step(false) == false);
    assert(f.Step(false) == false);
    assert(f.Step(true) == true); // shows the instant want goes true, no hold on the way up
}

void TestHoldsThroughAShortMissRun() {
    HoldFilter f(6);
    assert(f.Step(true) == true);
    for (int i = 0; i < 6; ++i)
        assert(f.Step(false) == true); // misses 1..6: still held
    assert(f.Step(false) == false);    // miss 7 exceeds the hold: hides
    assert(f.Step(false) == false);    // stays hidden
}

void TestAHitBeforeExpiryResetsTheMissCount() {
    HoldFilter f(3);
    assert(f.Step(true) == true);
    assert(f.Step(false) == true); // miss 1
    assert(f.Step(false) == true); // miss 2
    assert(f.Step(true) == true);  // a real hit inside the hold window: misses reset to 0
    for (int i = 0; i < 3; ++i)
        assert(f.Step(false) == true); // a fresh run of up to hold_ticks misses is held again
    assert(f.Step(false) == false);    // only now (3 + 1 misses since the last hit) does it hide
}

void TestNeverShownStaysHidden() {
    HoldFilter f(10);
    for (int i = 0; i < 50; ++i)
        assert(f.Step(false) == false);
}

void TestResetHidesAtOnceAndForgetsTheHold() {
    HoldFilter f(10);
    assert(f.Step(true) == true);
    assert(f.Step(false) == true);  // still within the hold
    assert(f.Reset() == false);
    assert(f.Step(false) == false); // the hold does not resume after an explicit reset
    assert(f.Step(true) == true);   // a later true re-arms it from scratch
}

void TestZeroHoldTicksHidesOnTheFirstMiss() {
    HoldFilter f(0);
    assert(f.Step(true) == true);
    assert(f.Step(false) == false); // no hold at all: equivalent to publishing the raw value
}

void TestArmIgnoresShortPulses() {
    HoldFilter f(10, 20);
    for (int i = 0; i < 14; ++i)
        assert(f.Step(true) == false); // a 14-tick pulse (spurious message-start read) never shows
    assert(f.Step(false) == false);
    for (int i = 0; i < 19; ++i)
        assert(f.Step(true) == false); // hits restart after a miss: 19 is still short of 20
    assert(f.Step(true) == true);      // hit 20: shown
    for (int i = 0; i < 3; ++i)
        assert(f.Step(false) == true); // a 2-3 tick in-wait drop-out is held
    assert(f.Step(true) == true);      // still shown, no re-arm needed while shown
    assert(f.Reset() == false);
    assert(f.Step(true) == false);     // after Reset it must arm again
}

int main() {
    TestHiddenUntilFirstWant();
    TestHoldsThroughAShortMissRun();
    TestAHitBeforeExpiryResetsTheMissCount();
    TestNeverShownStaysHidden();
    TestResetHidesAtOnceAndForgetsTheHold();
    TestZeroHoldTicksHidesOnTheFirstMiss();
    TestArmIgnoresShortPulses();
    std::puts("dsmod_hold_test: HoldFilter arms after K hits, holds through short misses, resets at once");
    return 0;
}
