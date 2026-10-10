// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace lp_field_actions {
enum class Input { None, Cancel, OpenMenu, Accept, Wheel, Direction, Restore };

// Payloads reserve 0..5 for whole-party drags. Validate before subtracting so
// malformed signed inputs cannot overflow, and never accept an earlier move layout.
inline int MoveSlot(std::int64_t payload, std::int64_t token) {
    if (payload < 100 || token <= 0 || (payload - 100) / 4 != token) return -1;
    return static_cast<int>((payload - 100) % 4);
}
inline std::int64_t NextMoveToken(std::int64_t token) {
    constexpr auto Max = (std::numeric_limits<std::int64_t>::max() - 103) / 4;
    return token < 0 || token >= Max ? 1 : token + 1;
}

// Check the context/deadline before a delayed press: resuming a hidden/loading
// companion must not replay stale X, A or shortcut inputs.
// Escape Rope (78) and Honey (94): field uses the Bag pays one item for (taken after the use ran).
inline bool ConsumedOnUse(int item) { return item == 78 || item == 94; }

inline Input BagStep(int state, std::int64_t age, bool field, bool free, bool menu) {
    if (!state) return Input::None;
    if (!field || age < 0 || age > 4000) return Input::Cancel;
    if (state == 1) return !free ? Input::Cancel : age > 100 ? Input::OpenMenu : Input::None;
    if (state == 2) return menu && age > 900 ? Input::Accept : Input::None;
    return Input::Cancel;
}
inline Input ShortcutStep(int state, std::int64_t age, bool same_field, bool free) {
    if (!state) return Input::None;
    if (!same_field || age < 0 || age > 4000) return Input::Restore;
    if (state == 1) return !free ? Input::Restore : age > 150 ? Input::Wheel : Input::None;
    if (state == 2) return age > 700 ? Input::Direction : Input::None;
    if (state == 3) return age > 1700 ? Input::Restore : Input::None;
    return Input::Restore;
}
} // namespace lp_field_actions
