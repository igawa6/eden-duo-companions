// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The companion's non-gameplay states (design B lifecycle, UX-SPEC s6/s8; research contracts/STATES-1.md):
// - Title: no save is loaded (title screen, Load Game list, settings from the title). The title view:
//   the game's logo, "Ver. 1.0.2", "No save loaded"; no tabs, no values, no actions.
// - Loading: the game shows "Now Loading..." (boot into a save, fast travel, Return to Skell, Barracks
//   doors). The last page dimmed with every live value as "--", the loading plate in place of the tabs,
//   no progress number; with no page yet (first load) the title art with the plate. No actions.
// - Live: the field is playable; a native menu then only dims the page (battle::Dim::Menu).
// A wrong build never reaches the module (the host refuses it; the manifest shows the refusal).
#include <cstdint>

namespace xcx {

enum class Stage : std::uint8_t { Live, Title, Loading };

// `data_valid`: the reader found the party. `helper_fresh`: the guest helper's heartbeat and state word
// are fresh. `helper_object`: a world exists or is loading: its raw state word has StateValid or
// StateGate (set through every loading screen seen live, clear on the title screen and Load Game list). `field_ok`: StateFieldOk (the game's 0xf0b44(), 0 through
// the title, loading and fades). `eject`: the Skell eject sequence mid-battle, which drops field_ok
// without a loading screen (BATTLE-LIVE-3); the page stays live then.
inline Stage StageFor(bool data_valid, bool helper_fresh, bool helper_object, bool field_ok, bool eject) {
    if (!helper_fresh) // no helper (load plan refused or not installed): only the reader decides
        return data_valid ? Stage::Live : Stage::Title;
    if (data_valid && (field_ok || eject))
        return Stage::Live;
    return data_valid || helper_object ? Stage::Loading : Stage::Title;
}

// A state must hold for DelayMs before it is shown (an actor handover or a boarding blink must not
// flash the loading view); the first state is shown at once.
struct StageFilter {
    static constexpr std::int64_t DelayMs = 300;
    Stage shown{Stage::Title}, raw{Stage::Title};
    std::int64_t since{};
    bool started{};
    Stage Update(Stage s, std::int64_t now) {
        if (!started) {
            started = true;
            shown = raw = s;
            since = now;
            return shown;
        }
        if (s != raw) {
            raw = s;
            since = now;
        }
        if (s != shown && now - since >= DelayMs)
            shown = s;
        return shown;
    }
};

// What the host may act on: tabs and every page action only while the field is live.
inline bool TabsShown(Stage s) { return s == Stage::Live; }

} // namespace xcx
