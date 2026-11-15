// SPDX-License-Identifier: GPL-3.0-or-later
// Test fixture (dev.44): the pause / map state the module's menu gate reads when the helper's
// game-thread sample is not fresh (MenuSample's direct path): the layout controller
// dLytControlGame_c* at main+0x183AC40, its current state ID at +0x120 = one of its static state
// objects, StateID_Normal main+0x164F988 in play, StateID_Pause main+0x164FAD0 (0x148 after it) in the
// pause menu. A test's `menu` byte (non-zero = a pause menu, as the tests modelled the old stack
// slot main+0x192D0E4) picks the state. The helper's own sample (mailbox +0x6C / +0x70 / +0x74) is
// what MenuHelperSample writes.
#pragma once

#include <cstdint>
#include <cstring>

namespace sshd_test {

constexpr uint64_t kFixCtrlPtr = 0x183AC40, kFixCtrl = 0x7300000, kFixStateNormal = 0x164F988,
                   kFixStatePause = 0x164FAD0;

// Answers the controller reads relative to `main`; ctrl_ok = false makes the controller unreadable.
inline bool MenuFixtureRead(uint64_t main, uint8_t menu, uint64_t a, void* out, size_t n, bool ctrl_ok = true) {
    const auto put64 = [&](uint64_t v) {
        if (n != 8) return false;
        std::memcpy(out, &v, 8);
        return true;
    };
    if (a == main + kFixCtrlPtr) return ctrl_ok && put64(kFixCtrl);
    if (a == kFixCtrl + 0x120) return ctrl_ok && put64(main + (menu != 0 ? kFixStatePause : kFixStateNormal));
    return false;
}

// The helper loop hook's sample for this frame: code (0 play, 1 a pause / map state, 2 no controller),
// the state's distance from StateID_Normal, stamped with the frame's loop heartbeat (+0x68).
inline void MenuHelperSample(uint32_t* mailbox, uint32_t code, uint32_t delta) {
    mailbox[0x6C / 4] = code;
    mailbox[0x74 / 4] = delta;
    mailbox[0x70 / 4] = mailbox[0x68 / 4];
}

} // namespace sshd_test
