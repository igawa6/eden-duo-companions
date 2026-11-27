// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Throw selector HUD hide. The game hides and re-shows its own selector by clearing/setting
// bit0 of the layout pane `all` (+0x58), resolved through the layout work's binding table by
// the native hash 080A8E808288E407 (2476688..66E8 clears, 247CC84..CCCC sets). The companion
// writes the same bit: hidden while the bottom screen shows the selector, restored otherwise.
#pragma once
#include "pla_blocks.h"

namespace pla {
inline constexpr uint64_t SelectorAllHash = 0x080A8E808288E407ULL;

struct SelectorHud {
    uint64_t view = 0, bindings = 0, pane = 0;
    unsigned index = 0;
    bool hidden_by_us = false;

    // Re-check the cached binding entry; rescan the table only when the view or table moved.
    bool Resolve(const EdenDsmodHostApi &h, uint64_t selector_view) {
        uint64_t work = 0, table = 0, count = 0;
        if (!selector_view || !Pointer(h, selector_view, 0x2c8, work) || !ReadAt(h, work, 0x148, table) ||
            !ReadAt(h, work, 0x150, count) || table % 8 || !count || count > 512 ||
            !Range(h, table, count * 0x30))
            return Forget();
        const auto entry = [&](unsigned n, uint64_t &out) {
            uint64_t hash = 0;
            return ReadAt(h, table, uint64_t(n) * 0x30, hash) && hash == SelectorAllHash &&
                   ReadAt(h, table, uint64_t(n) * 0x30 + 0x20, out) && out && out % 8 == 0 &&
                   Range(h, out + 0x58, 1);
        };
        uint64_t found = 0;
        if (selector_view == view && table == bindings && index < count && entry(index, found) &&
            found == pane)
            return true;
        unsigned matches = 0, at = 0;
        for (unsigned n = 0; n < count; ++n) {
            uint64_t candidate = 0;
            if (entry(n, candidate)) { ++matches; at = n; found = candidate; }
        }
        if (matches != 1) return Forget();
        if (found != pane) hidden_by_us = false;
        view = selector_view; bindings = table; index = at; pane = found;
        return true;
    }
    bool Forget() { view = bindings = pane = 0; hidden_by_us = false; return false; }

    // want_hidden: clear the native visible bit (every sample; native refreshes set it again).
    // Otherwise put back only a bit this companion cleared.
    void Apply(const EdenDsmodHostApi &h, bool want_hidden) {
        if (!pane || !h.write_memory) return;
        uint8_t flags = 0;
        if (!ReadAt(h, pane, 0x58, flags)) return;
        if (want_hidden) {
            // Remember the bit as ours only when this companion cleared it, so turning the
            // setting off never re-shows a selector the game hid itself (menus, cutscenes).
            if (flags & 1) {
                const uint8_t value = flags & ~1u;
                if (h.write_memory(h.userdata, pane + 0x58, &value, 1)) hidden_by_us = true;
            }
        } else if (hidden_by_us) {
            // The selector may be unreadable now (loading, aux lost): re-walk view -> table ->
            // pane before writing, so a freed pane is never written; the game's own refresh
            // re-shows the selector in that case.
            const uint64_t was = pane;
            if (!Resolve(h, view) || pane != was) { Forget(); return; }
            if (!ReadAt(h, pane, 0x58, flags)) return;
            if (!(flags & 1)) {
                const uint8_t value = flags | 1u;
                h.write_memory(h.userdata, pane + 0x58, &value, 1);
            }
            hidden_by_us = false;
        }
    }
};
} // namespace pla
