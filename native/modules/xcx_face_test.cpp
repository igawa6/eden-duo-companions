// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Skell button's face per helper state word: disabled, Board, Exit, Return, Return + Hold.
// Regression for the Return face reading greyed while the live state word was 0x1fc8d (valid, on
// foot, input ready, can_return, field_ok, HUD ok): the controller must offer Return for it.
#include "xcx_helper_mailbox.h"
#include <cassert>
#include <cstdio>
using namespace xcx_helper;
namespace {
struct Expect {
    const char *name;
    std::uint32_t word;
    bool has_skell;
    Face face;
    bool enabled, return_ready;
};
Button Sample(std::uint32_t word, bool has_skell, Controller &c, std::int64_t now) {
    Status s;
    s.helper_present = true;
    s.state_fresh = true;
    s.state = DecodeState(word);
    return c.Update(s, has_skell, now);
}
} // namespace
int main() {
    const std::uint32_t base = StateValid | StateGate | StateHudOk | StateInputReady | StateFieldOk;
    const std::uint32_t foot = base | StateOnFoot | StateRecFoot | StateIdle | StateCooldownClear | StateSwitchReady;
    const Expect cases[] = {
        // the live word of 2026-11-21 (FN Site 216, Skell far away): Return, a hold acts
        {"live 0x1fc8d", 0x1fc8d, true, Face::Return, false, true},
        {"on foot, nothing possible", foot, true, Face::Return, false, false},
        {"board", foot | StateCanBoard, true, Face::Board, true, false},
        {"board beats return", foot | StateCanBoard | StateCanReturn, true, Face::Board, true, false},
        {"exit", base | StatePiloting | StateCanExit, true, Face::Exit, true, false},
        {"piloting, exit refused", base | StatePiloting, true, Face::Exit, false, false},
        {"return", foot | StateCanReturn, true, Face::Return, false, true},
        {"no input (menu)", (foot | StateCanReturn) & ~StateInputReady, true, Face::Return, false, false},
        {"no field (loading)", (foot | StateCanReturn) & ~StateFieldOk, true, Face::Return, false, false},
        {"HUD object not current", (foot | StateCanReturn) & ~StateHudOk, true, Face::Return, false, false},
        {"no Skell in the party", foot | StateCanBoard | StateCanReturn, false, Face::Return, false, false},
        {"not valid", StateInputReady | StateFieldOk | StateHudOk, true, Face::Return, false, false},
        {"contradictory (on foot and piloting)", foot | StatePiloting | StateCanBoard, true, Face::Return, false, false},
    };
    for (const auto &c : cases) {
        Controller controller;
        const auto b = Sample(c.word, c.has_skell, controller, 100000);
        std::printf("%-36s word 0x%05x face %d enabled %d return %d\n", c.name, c.word, int(b.face), b.enabled,
                    b.return_ready);
        assert(b.face == c.face && b.enabled == c.enabled && b.return_ready == c.return_ready);
    }
    // Return + Hold: a short tap on the Return face shows the hint for a while, then it goes.
    {
        Controller controller;
        auto b = Sample(0x1fc8d, true, controller, 100000);
        assert(b.return_ready && !b.hint);
        struct NoBridge {
            bool Stage(Op, std::int64_t, std::uint32_t = 0, std::uint32_t = 0) { return false; }
        } bridge;
        const auto press = controller.OnPress(bridge, b, 100100);
        assert(!press.accepted && press.reason == Reason::Hold);
        b = Sample(0x1fc8d, true, controller, 100200);
        assert(b.return_ready && b.hint);
        b = Sample(0x1fc8d, true, controller, 100100 + RefusedShownMs + 10);
        assert(b.return_ready && !b.hint);
    }
    // The face survives repeated identical samples and a stale-then-fresh state without sticking.
    {
        Controller controller;
        for (int n = 0; n < 50; ++n)
            assert(Sample(0x1fc8d, true, controller, 100000 + n * 100).return_ready);
        Status stale;
        stale.helper_present = true; // state not fresh: the prototype's face
        assert(!controller.Update(stale, true, 105100).return_ready);
        assert(Sample(0x1fc8d, true, controller, 105200).return_ready);
    }
    std::puts("xcx face: all checks passed");
    return 0;
}
