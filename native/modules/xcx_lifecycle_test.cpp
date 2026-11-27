// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The companion's non-gameplay states (xcx_lifecycle.h): which view each signal combination gets,
// and the 300 ms hold before a state is shown.
#include "xcx_lifecycle.h"
#include <cstdio>
namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace
int main() {
    using xcx::Stage;
    using xcx::StageFor;
    // Title screen / Load Game list: no party, helper running without a controlled object.
    Check(StageFor(false, true, false, false, false) == Stage::Title, "title with helper");
    Check(StageFor(false, false, false, false, false) == Stage::Title, "title without helper");
    // Fast travel / save load with the party actors gone: the helper's gate bit says a world loads.
    Check(StageFor(false, true, true, false, false) == Stage::Loading, "fast travel, actors gone");
    // Loading screens: a controlled object (or the party) with field_ok 0.
    Check(StageFor(false, true, true, false, false) == Stage::Loading, "first load, no party yet");
    Check(StageFor(true, true, true, false, false) == Stage::Loading, "fast travel / Return");
    Check(StageFor(true, true, false, false, false) == Stage::Loading, "party read, object not yet");
    // The playable field, and the mid-battle Skell eject that drops field_ok without a loading screen.
    Check(StageFor(true, true, true, true, false) == Stage::Live, "field");
    Check(StageFor(true, true, true, false, true) == Stage::Live, "eject stays live");
    // field_ok with no party (an actor handover): loading, never "no save loaded".
    Check(StageFor(false, true, true, true, false) == Stage::Loading, "handover");
    // No helper: the reader alone (the page stays live, as before the helper existed).
    Check(StageFor(true, false, false, false, false) == Stage::Live, "no helper, party");
    Check(!xcx::TabsShown(Stage::Title) && !xcx::TabsShown(Stage::Loading) && xcx::TabsShown(Stage::Live),
          "tabs only live");

    xcx::StageFilter f;
    Check(f.Update(Stage::Title, 0) == Stage::Title, "first state at once");
    Check(f.Update(Stage::Loading, 100) == Stage::Title, "loading held");
    Check(f.Update(Stage::Loading, 399) == Stage::Title, "loading still held");
    Check(f.Update(Stage::Loading, 400) == Stage::Loading, "loading after 300 ms");
    Check(f.Update(Stage::Live, 1000) == Stage::Loading, "live held");
    Check(f.Update(Stage::Loading, 1100) == Stage::Loading, "blink back");
    Check(f.Update(Stage::Live, 1200) == Stage::Loading, "blink restarts the hold");
    Check(f.Update(Stage::Live, 1500) == Stage::Live, "live after a steady 300 ms");
    Check(f.Update(Stage::Loading, 1600) == Stage::Live && f.Update(Stage::Live, 1700) == Stage::Live,
          "a 100 ms loading blink never shows");
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("xcx lifecycle OK");
    return 0;
}
