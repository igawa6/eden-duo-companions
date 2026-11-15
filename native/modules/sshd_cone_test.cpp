// SPDX-License-Identifier: GPL-3.0-or-later
// Link's view cone holds the last loaded picture (dev.37, owner Thor feedback "bottom view cone some
// time blinking when move"; sshd_cone.h): a turn onto an angle whose picture is not loaded keeps the
// loaded one and asks for the new one off screen, a nearer loaded angle is used meanwhile, a picture
// evicted from the host's cache (many other images delivered since) is not trusted until it is
// delivered again or the wait runs out, a failed read keeps the cone for a few samples, and the
// dev.36 behaviour (the wanted picture at once, which blanked) replays for diagnostics.
#include <cassert>
#include <cstdio>

#include "sshd_cone.h"

int main() {
    using sshd::ConeHold;
    constexpr uint64_t kCone = 400ull * 400 * 4; // one picture
    ConeHold c;
    // first sample: nothing loaded to hold, the wanted picture is shown (and requested by the cone)
    auto o = c.Step(true, true, 90);
    assert(o.on && o.shown == 90 && o.prefetch == -1);
    assert(!c.ShownReady());
    c.Building(90);
    assert(!c.ShownReady());
    c.Delivered(90, true);
    c.Bytes(kCone);
    assert(c.ShownReady());
    // Link turns to 95 (never built): 90 stays, 95 is prefetched; the cone never blanks
    o = c.Step(true, true, 95);
    assert(o.on && o.shown == 90 && o.prefetch == 95 && c.ShownReady());
    o = c.Step(true, true, 95);
    assert(o.shown == 90 && o.prefetch == 95);
    c.Building(95); // the worker picked the prefetch up
    o = c.Step(true, true, 95);
    assert(o.shown == 90 && o.prefetch == 95 && c.ShownReady());
    c.Delivered(95, true);
    c.Bytes(kCone);
    o = c.Step(true, true, 95);
    assert(o.shown == 95 && o.prefetch == -1 && c.ShownReady());
    // back to 90 (loaded and still fresh): at once
    o = c.Step(true, true, 90);
    assert(o.shown == 90 && o.prefetch == -1);
    // a fast turn to 180: the nearest loaded angle (95) is used until 180 lands
    o = c.Step(true, true, 180);
    assert(o.shown == 95 && o.prefetch == 180 && c.ShownReady());
    c.Building(180);
    c.Delivered(180, true);
    c.Bytes(kCone);
    o = c.Step(true, true, 180);
    assert(o.shown == 180 && o.prefetch == -1);
    // the angle wraps (355 <-> 0): 0 is one step from 355
    c.Delivered(0, true);
    c.Bytes(kCone);
    o = c.Step(true, true, 355);
    assert(o.shown == 0 && o.prefetch == 355); // 0 is nearer to 355 than 180
    c.Delivered(355, true);
    o = c.Step(true, true, 355);
    assert(o.shown == 355);
    // 90 was last drawn long ago and 40 MB of map pictures landed since: the host has likely
    // evicted it, so it is not shown before it is delivered again (it would draw nothing)
    c.Bytes(40ull << 20);
    o = c.Step(true, true, 90);
    assert(o.shown == 355 && o.prefetch == 90);
    c.Building(90);
    for (int i = 0; i < 100; ++i) { // rebuilding: never shown meanwhile, however long it takes
        o = c.Step(true, true, 90);
        assert(o.shown == 355 && c.ShownReady());
    }
    c.Delivered(90, true);
    o = c.Step(true, true, 90);
    assert(o.shown == 90);
    // a stale-looking angle the host still holds is never requested again: shown after kWait
    c.Bytes(40ull << 20);
    int n = 0;
    do {
        o = c.Step(true, true, 95);
        ++n;
    } while (o.shown != 95 && n < 200);
    assert(n == ConeHold::kWait + 1);
    // a failed read keeps the cone (the last picture) for kMiss samples, then hides it
    for (int i = 0; i < ConeHold::kMiss; ++i) {
        o = c.Step(false, false, -1);
        assert(o.on && o.shown == 95);
    }
    o = c.Step(false, false, -1);
    assert(!o.on);
    // off (unknown stage, another floor): hidden at once; back on: the held picture again
    o = c.Step(true, false, -1);
    assert(!o.on);
    o = c.Step(true, true, 95);
    assert(o.on && o.shown == 95);
    // the dev.36 behaviour (diagnostics replay): the wanted picture at once, loaded or not
    o = c.Step(true, true, 270, true);
    assert(o.shown == 270 && o.prefetch == -1 && !c.ShownReady());
    // the reset forgets the shown picture, not what was delivered
    c.Reset();
    o = c.Step(true, true, 180);
    assert(o.shown == 180 && c.ShownReady());
    std::printf("sshd cone: ok\n");
    return 0;
}
