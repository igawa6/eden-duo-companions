// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Motion helpers of the XCX companion (xcx_motion.h): rolling HP, picture fade-in, the page-bind
// points, and the rule that nothing is active at rest.
#include "xcx_hud.h"
#include "xcx_motion.h"
#include <map>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool Near(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }
} // namespace
int main() {
    using namespace xcx;
    // Easing: 0 -> 1, monotonic, fast start.
    Check(Near(EaseOut(0), 0) && Near(EaseOut(1), 1), "ease ends");
    Check(EaseOut(.5) > .5 && EaseOut(.2) < EaseOut(.4), "ease out shape");
    Check(Near(EaseOut(-3), 0) && Near(EaseOut(9), 1), "ease clamps");
    // Roll: the first value is taken as is, small steps follow at once, never active.
    Roll r;
    Check(Near(r.Update(8000, 0, true, 800), 8000) && !r.Active(), "first value snaps");
    for (int i = 1; i <= 50; ++i) // a slow drain: 1 point at a time
        Check(Near(r.Update(8000 - i, i * 100, true, 800), 8000 - i) && !r.Active(), "drain snaps");
    // A big drop rolls over ValueRollMs and lands exactly.
    const std::int64_t t0 = 10000;
    Check(r.Update(5000, t0, true, 800) > 4000 && r.Active(), "jump starts rolling from the old value");
    const double mid = r.Update(5000, t0 + ValueRollMs / 2, true, 800);
    Check(mid < 7950 && mid > 5000 && r.Active(), "mid-roll value is between");
    Check(Near(r.Update(5000, t0 + ValueRollMs, true, 800), 5000) && !r.Active(), "roll lands");
    Check(Near(r.Update(5000, t0 + 5000, true, 800), 5000) && !r.Active(), "rests after");
    // Monotonic while rolling down.
    {
        Roll q;
        q.Update(10000, 0, true, 100);
        double last = 10000;
        bool mono = true;
        for (int ms = 0; ms <= ValueRollMs + 40; ms += 16) {
            const double v = q.Update(2000, ms + 1, true, 100);
            mono &= v <= last + 1e-9;
            last = v;
        }
        Check(mono && Near(last, 2000), "roll is monotonic and ends at the target");
    }
    // Reduced motion snaps and never rolls.
    {
        Roll q;
        q.Update(100, 0, false, 8);
        Check(Near(q.Update(10, 5, false, 8), 10) && !q.Active(), "reduced motion snaps");
    }
    // Turning reduced motion on mid-roll snaps to the target.
    {
        Roll q;
        q.Update(100, 0, true, 8);
        q.Update(10, 10, true, 8);
        Check(q.Active(), "rolling");
        Check(Near(q.Update(10, 20, false, 8), 10) && !q.Active(), "reduced motion ends a roll");
    }
    // A jump measured against what is on screen: retarget mid-roll rolls on from there.
    {
        Roll q;
        q.Update(100, 0, true, 8);
        q.Update(20, 100, true, 8);
        const double shown = q.Update(20, 150, true, 8);
        q.Update(60, 160, true, 8); // heal while rolling
        Check(q.Active() && q.Update(60, 160 + ValueRollMs + 1, true, 8) == 60 && shown > 20,
              "retarget during a roll lands on the new value");
    }
    // Fade: a picture present from the first look never fades; one that lands later does.
    {
        Fade f;
        Check(f.Update(true, 0, true) == 1.f && !f.Active(), "already there: no fade");
        Fade g;
        Check(g.Update(false, 0, true) == 1.f && !g.Active(), "absent: nothing to fade");
        const float a0 = g.Update(true, 1000, true);
        Check(a0 == 0.f && g.Active(), "landed picture starts transparent");
        const float a1 = g.Update(true, 1000 + StillFadeMs / 2, true);
        Check(a1 > 0.4f && a1 < 0.6f, "mid fade is half way");
        Check(g.Update(true, 1000 + StillFadeMs, true) == 1.f && !g.Active(), "fade completes");
        Check(g.Update(true, 9000, true) == 1.f && !g.Active(), "stays opaque");
        // It goes away and comes back: it fades again.
        g.Update(false, 9100, true);
        Check(g.Update(true, 9200, true) == 0.f && g.Active(), "returns with a fade");
        // Reduced motion: opaque at once, never active.
        Fade h;
        h.Update(false, 0, false);
        Check(h.Update(true, 10, false) == 1.f && !h.Active(), "reduced motion: no fade");
        // Switching reduced motion on mid fade finishes it.
        Fade k;
        k.Update(false, 0, true);
        k.Update(true, 10, true);
        Check(k.Update(true, 20, false) == 1.f && !k.Active(), "reduced motion ends a fade");
    }
    // Page bind points: exactly one set carries the page.
    for (int p = 0; p < 4; ++p) {
        const auto m = PageBindPoints(p, false), c = PageBindPoints(p, true);
        Check(m.fade == p && m.cut == -1 && c.cut == p && c.fade == -1, "bind points");
    }
    // The documented limits: page 150-300 ms, value 150-400 ms, a still at most 400 ms.
    Check(PageFadeMs >= 150 && PageFadeMs <= 300, "page fade duration");
    Check(ValueRollMs >= 150 && ValueRollMs <= 400 && StillFadeMs >= 150 && StillFadeMs <= 400,
          "value durations");
    Check(MotionFrameMs >= 33, "motion frames are not faster than the host redraws");
    // Top-HUD controller (xcx_hud.h) on a fake guest modelled on the live findings: SystemData, the
    // options manager with its observer list, the field-HUD observer's copy of the HUD bools, the
    // minimap widget (changes only on a commit whose value differs from the observer's copy) and the
    // fuel gauge (follows the observer's copy every frame). The saved value must be the player's
    // whenever the game could write it: after every module call and every game frame but the single
    // frame a commit is applied.
    {
        constexpr std::uint64_t main = 0x8000000, sys = 0x20000000, mgr = 0x30000000, obs = 0x40000000,
                                other = 0x40001000;
        std::map<std::uint64_t, std::uint8_t> mem;
        const auto put = [&](std::uint64_t a, const void *v, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i)
                mem[a + i] = static_cast<const std::uint8_t *>(v)[i];
        };
        const auto put64 = [&](std::uint64_t a, std::uint64_t v) { put(a, &v, 8); };
        const auto get = [&](std::uint64_t a) { return mem.count(a) ? mem[a] : std::uint8_t(0); };
        put64(main + hud::SystemDataSlot, sys);
        put64(main + hud::OptionsManagerSlot, mgr);
        for (std::uint32_t i = 0; i < hud::BlockSize; ++i) { // all HUD options Show, others 7
            const std::uint8_t v = i >= hud::HudOffset - hud::BlockOffset ? 1 : 7;
            put(sys + hud::BlockOffset + i, &v, 1);
            put(mgr + hud::WorkingCopy + i, &v, 1);
        }
        for (std::uint32_t k = 0; k < 0x28; ++k)
            mem[obs + hud::ObserverCopy + k] = 1;
        // observer list: head 3 -> entry 3 (another observer) -> entry 1 (the field HUD) -> end
        mem[mgr + hud::ObserverHead] = 3;
        put64(mgr + hud::ObserverEntries + 3 * 16, other);
        mem[mgr + hud::ObserverEntries + 3 * 16 + hud::ObserverNext] = 1;
        put64(mgr + hud::ObserverEntries + 1 * 16, obs);
        mem[mgr + hud::ObserverEntries + 1 * 16 + hud::ObserverNext] = 0;
        put64(other, main + 0x1234560);
        put64(obs, main + hud::FieldHudVtable);
        bool minimap = true; // the widget
        int batches = 0;
        hud::Io io;
        io.read = [&](std::uint64_t a, void *out, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i)
                static_cast<std::uint8_t *>(out)[i] = get(a + i);
            return true;
        };
        io.batch = [&](const hud::Io::Op *ops, std::size_t n) {
            for (std::size_t o = 0; o < n; ++o)
                for (std::uint32_t i = 0; i < ops[o].size; ++i)
                    if (ops[o].expect && get(ops[o].address + i) != static_cast<const std::uint8_t *>(ops[o].expect)[i])
                        return false;
            for (std::size_t o = 0; o < n; ++o)
                put(ops[o].address, ops[o].value, ops[o].size);
            ++batches;
            return true;
        };
        const auto k_map = hud::HudOffset + hud::ElementK[hud::Map], k_fuel = hud::HudOffset + hud::ElementK[hud::Fuel];
        const auto saved = [&](std::uint64_t k) { return get(sys + k); };
        const auto fuel_gauge = [&] { return get(obs + hud::ObserverCopy + hud::ElementK[hud::Fuel]) == 1; };
        bool committed = false;
        const auto frame = [&] { // fn_3410a0: notify (fn_41ec18), copy into the observer and SystemData
            committed = false;
            if (!get(mgr + hud::CommitFlag))
                return;
            const std::uint8_t nv = get(mgr + hud::WorkingCopy + hud::HudIndex(hud::ElementK[hud::Map]));
            if (get(obs + hud::ObserverCopy + hud::ElementK[hud::Map]) != nv)
                minimap = nv != 0;
            for (std::uint32_t i = 0; i < hud::BlockSize; ++i)
                mem[sys + hud::BlockOffset + i] = get(mgr + hud::WorkingCopy + i);
            for (std::uint32_t k = 0; k < 0x28; ++k)
                mem[obs + hud::ObserverCopy + k] = get(mgr + hud::WorkingCopy + hud::HudIndex(k));
            mem[mgr + hud::CommitFlag] = 0;
            committed = true;
        };
        std::uint8_t player_map = 1, player_fuel = 1;
        int unsafe = 0;
        const auto check_saved = [&](const char *when) {
            // excused: the frame that applied a commit, and a Show commit the next frame applies
            if (!committed && !get(mgr + hud::CommitFlag) && (saved(k_map) != player_map || saved(k_fuel) != player_fuel)) {
                ++unsafe;
                std::fprintf(stderr, "saved settings differ from the player's %s\n", when);
            }
        };
        hud::Controller c;
        const auto step = [&](std::array<bool, 2> want, bool live) {
            c.Update(io, main, want, live);
            check_saved("after a module call");
            frame();
            check_saved("after a game frame");
        };
        for (int i = 0; i < 4; ++i)
            step({true, true}, true);
        Check(c.Observer() == obs, "field-HUD observer found through the list");
        Check(!minimap && !fuel_gauge() && c.Hidden(hud::Map) && c.Hidden(hud::Fuel), "both hidden");
        Check(saved(k_map) == 1 && saved(k_fuel) == 1 && get(mgr + hud::WorkingCopy + hud::HudIndex(1)) == 1,
              "saved and working values stay Show");
        Check(get(sys + hud::BlockOffset) == 7 && saved(hud::HudOffset + 2) == 1, "other options untouched");
        // A load / menu: no minimap commits while the field is not live, nothing comes back.
        const int before_menu = batches;
        for (int i = 0; i < 4; ++i)
            step({true, true}, false);
        Check(!minimap && !fuel_gauge() && batches == before_menu, "nothing written in a menu");
        // Native Settings closes with another option changed: the game commits the whole block; the
        // minimap stays hidden (the observer copy matches), the gauge comes back for a frame and is
        // hidden again.
        mem[mgr + hud::WorkingCopy + 0] = 9;
        mem[mgr + hud::CommitFlag] = 1;
        frame();
        Check(!minimap && fuel_gauge(), "a native commit of another option");
        step({true, true}, true);
        Check(!minimap && !fuel_gauge(), "gauge hidden again");
        // The player chooses Mini-map Hide in native Settings: saved as their choice, never undone.
        mem[mgr + hud::WorkingCopy + hud::HudIndex(1)] = 0;
        mem[mgr + hud::CommitFlag] = 1;
        frame();
        player_map = 0;
        for (int i = 0; i < 4; ++i)
            step({true, true}, true);
        Check(saved(k_map) == 0 && !minimap && !c.Hidden(hud::Map), "a native Hide stays the player's");
        // ... and Show again: the companion hides it again (its toggle is on), saved stays Show.
        mem[mgr + hud::WorkingCopy + hud::HudIndex(1)] = 1;
        mem[mgr + hud::CommitFlag] = 1;
        frame();
        player_map = 1;
        Check(minimap, "native Show shows it");
        for (int i = 0; i < 4; ++i)
            step({true, true}, true);
        Check(!minimap && saved(k_map) == 1, "hidden again, saved Show");
        // Toggle off: both come back.
        for (int i = 0; i < 4; ++i)
            step({false, false}, true);
        Check(minimap && fuel_gauge() && !c.Hidden(hud::Map) && saved(k_map) == 1, "toggle off shows both");
        // Teardown while hidden, including a hide whose commit is still pending.
        for (int i = 0; i < 4; ++i)
            step({true, true}, true);
        Check(!minimap, "hidden before teardown");
        c.Restore(io, main);
        check_saved("after teardown");
        frame();
        check_saved("after the teardown frame");
        Check(minimap && fuel_gauge(), "teardown shows both");
        hud::Controller d;
        d.Update(io, main, {true, false}, true); // Hide commit issued, not yet applied
        d.Restore(io, main);
        frame();
        Check(minimap && saved(k_map) == 1 && !get(mgr + hud::CommitFlag), "a pending hide is withdrawn at teardown");
        Check(unsafe == 0, "the saved settings were the player's at every point");
        // A build without the field-HUD observer: nothing is written.
        put64(obs, main + 0x999990);
        const int before = batches;
        hud::Controller e;
        e.Update(io, main, {true, true}, true);
        Check(batches == before && e.StateOf(hud::Map) == hud::State::Failed, "no observer, no write");
        put64(obs, main + hud::FieldHudVtable);
        // No save loaded (null SystemData): nothing written.
        put64(main + hud::SystemDataSlot, 0);
        hud::Controller f;
        Check(!f.Update(io, main, {true, true}, true) && batches == before, "no write without SystemData");
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("xcx motion helpers OK");
    return 0;
}
