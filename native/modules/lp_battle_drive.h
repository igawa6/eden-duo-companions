// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace lp_battle_drive {
enum class CursorStep { Wait, Write, Press, Done, Timeout };
// Native menu setup can reset CurrentIndex after it first receives focus. Retry the
// cursor before A, keep a full settle interval after each write, and never extend the deadline.
inline CursorStep MenuCursor(std::uint64_t age, std::uint64_t since_write,
                             bool focus, bool transition, int current, int wanted, bool pressed) {
    if (pressed && !focus) return CursorStep::Done;
    if (age > 150) return CursorStep::Timeout;
    if (pressed || !focus || transition) return CursorStep::Wait;
    if (current != wanted) return CursorStep::Write;
    return since_write >= 4 ? CursorStep::Press : CursorStep::Wait;
}
} // namespace lp_battle_drive
