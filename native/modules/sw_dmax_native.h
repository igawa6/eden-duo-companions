// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Native Dynamax on the bottom screen (owner 2026-11-07: Gym #1 Axew vs Natu -- the top showed
// Max Darkness / Max Darkness / Max Strike / Max Wyrmwind after the native Dynamax button, the
// bottom kept Assurance / Bite / Slash / Dragon Claw and showed no Dynamax emblem; "it works in
// raids but not gyms"). Before this, the bottom's Max moves followed only the companion's own
// raid toggle or an already-Dynamaxed Pokemon, and its toggle was raid-only.
//
// Now, in ANY battle whose native Fight screen offers Dynamax (Gym / trainer singles, Battle Tower,
// raids): the bottom shows the toggle while the native opportunity query (the guest preview, this
// command turn, the Pokemon now choosing) says Dynamax is available, mirrors the TOP's selection
// (move view +0x61D: the Max-move list is active), shows the Max moves the top shows, and drives
// the native Dynamax button with the cursor channel + the A press (never a helper-made commit).

#pragma once

#include <cstdint>

#include "sw_battle_cursor.h"
#include "sw_helper_mailbox.h"

namespace sw_dmax_native {

// The top shows its Max-move list (Dynamax selected natively, or the Pokemon is Dynamaxed).
inline bool NativeMaxSelected(const sw_battle_cursor::Snapshot& nc) {
    return nc.menu == sw_battle_cursor::Menu::Fight && nc.max_list == 1;
}

// The native Fight screen offers Dynamax to the Pokemon now choosing: the native move list waits,
// the last preview (published by the guest from this turn's command menu) is for this client and
// this battler and says eligible, and the Pokemon is not already Dynamaxed.
// `client` = the battle client (Reader::battle_client, held through an emulation stall).
inline bool NativeDmaxOffered(const sw_battle_cursor::Snapshot& nc, const sw_helper::Status& st,
                              const sw_helper::Status::Preview& last, const sw_helper::BattleView& view,
                              std::uint64_t own_actor, std::uint64_t client) {
    return nc.menu == sw_battle_cursor::Menu::Fight && st.helper_present && view.in_battle && view.own_ok &&
           !view.max_active && last.valid && last.eligible && last.client != 0 &&
           last.client == client && last.mon != 0 && last.mon == own_actor;
}

// The bottom's move rows show Max moves (display only: actions keep the base move ids).
inline bool DisplayMax(bool companion_wanted, bool preview_active, bool battler_dynamaxed,
                       const sw_battle_cursor::Snapshot& nc) {
    return companion_wanted || preview_active || battler_dynamaxed || NativeMaxSelected(nc);
}

// The grid index the cursor channel moves the native cursor to for the Dynamax button: the even
// column beside the hovered move row (-1 = no native move list).
inline int DynamaxCell(const sw_battle_cursor::Snapshot& nc) {
    if (nc.menu != sw_battle_cursor::Menu::Fight || !nc.move_list) return -1;
    const int row = nc.move >= 0 && nc.move < 4 ? nc.move : 0;
    return row * 2;
}

} // namespace sw_dmax_native
