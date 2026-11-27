// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Battle page (design B-Battle-2 "Party first"): what each card, the Skell strip and the state
// chip show, from the reader's snapshot. Pure and header-only so a unit test can drive it.
//
// Data honesty: the page shows only what the readers read; anything untraced is drawn as "--".
// Traced (BATTLE-LIVE-2):
//   - Skell Insurance: the Skell record byte +0x3f8; live, a destroyed piloted Skell took it 2 -> 1
//     together with the native main menu's "Skell Insurance 2" -> "1" (InsuranceLive).
//   - A mounted member's GP on its card is its Skell's status +0x268, the value the strip shows and
//     the native battle row "GP n" shows for that member (MountedPointsLive).
//   - The in-combat flag: the controlled actor's state machine [actor+0x1e0]+0x34 == 1 (the game's
//     own IsBattle); the actor is the Skell while piloting, else the character (FlagFor).
#pragma once
#include "xcx_reader.h"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
namespace xcx::battle {
inline constexpr bool InsuranceLive = true;
inline constexpr bool MountedPointsLive = true;
inline constexpr bool FlagLive = true;
inline constexpr const char *Pending = "--";

enum class Flag { Unknown, In, Out };
enum class Dim { None, Menu, Loading };
enum class Chip { None, Parked, Piloting, Destroyed, NotInBattle, MenuOpen, Loading };

struct Card {
    bool present{}, controlled{}, mounted{}, down{}, wreck{};
    int hp{}, max_hp{1};
    bool gp{};             // the points row is the Skell's GP (mounted), else the member's TP
    bool points_pending{}; // the points value is drawn as "--"
    int points{};
};

// A member's card: on foot its own HP and TP; mounted, its Skell's HP and (pending) GP.
inline Card CardFor(const Member &m) {
    Card c;
    if (!m.pc.present)
        return c;
    c.present = true;
    c.controlled = m.controlled;
    c.mounted = m.mounted || (m.controlled && m.skell.present && m.skell.driven);
    c.down = m.pc.hp == 0;
    const bool use_skell = c.mounted && m.skell.present && m.skell.max_hp > 0;
    c.wreck = use_skell && m.skell.hp == 0;
    c.hp = use_skell ? m.skell.hp : m.pc.hp;
    c.max_hp = std::max(1, use_skell ? m.skell.max_hp : m.pc.max_hp);
    c.gp = c.mounted;
    c.points_pending = c.mounted && !MountedPointsLive;
    c.points = c.mounted ? (use_skell ? m.skell.points : 0) : m.pc.points;
    return c;
}

// Whose Skell the strip shows: the controlled member's, else the first member with one; -1 none.
inline int StripSlot(const Snapshot &s) {
    if (!s.valid)
        return -1;
    if (s.controlled >= 0 && s.controlled < 4 && s.party[s.controlled].pc.present &&
        s.party[s.controlled].skell.present)
        return s.controlled;
    for (int i = 0; i < 4; ++i)
        if (s.party[i].pc.present && s.party[i].skell.present)
            return i;
    return -1;
}

// The controlled member drives its Skell (the driven-actor mark, as the Map marker uses it).
inline bool Piloting(const Snapshot &s) {
    return s.valid && s.controlled >= 0 && s.controlled < 4 &&
           s.party[s.controlled].skell.present && s.party[s.controlled].skell.driven;
}

inline bool Destroyed(const Member &m) {
    return m.skell.present && m.skell.max_hp > 0 && m.skell.hp == 0;
}

// Battle sequences that switch off the game's field gate 0xf0b44 (helper bit15) although nothing
// loads, while the in-combat flag still says In (BATTLE-LIVE-3, all plain gate components unchanged):
//   - the Skell eject: the controlled member's driven Skell at HP 0 (wreck, then the eject cut-in),
//   - the controlled member Down (HP 0: the "Respawn Now / Wait Until Forced" countdown).
// The page stays live then (chip "Destroyed", card "Down"). A real loading screen never has an
// in-battle flag with either; the respawn load itself reads the flag Out with HP refilled.
inline bool EjectSequence(const Snapshot &s, Flag flag) {
    if (flag != Flag::In || !s.valid || s.controlled < 0 || s.controlled > 3)
        return false;
    const auto &m = s.party[s.controlled];
    return m.pc.present && ((m.skell.driven && Destroyed(m)) || m.pc.hp == 0);
}

// Why the page is dimmed: a stale helper or unread snapshot never dims; the field gate off is a
// loading screen, input refused is a menu or scene; during the eject sequence the page stays live.
inline Dim DimFor(bool helper_fresh, bool data_valid, bool field_ok, bool input_ready, bool eject) {
    if (!helper_fresh || !data_valid || eject)
        return Dim::None;
    return !field_ok ? Dim::Loading : !input_ready ? Dim::Menu : Dim::None;
}

// The strip's state chip; a dimmed page names why it is dimmed instead.
inline Chip ChipFor(const Snapshot &s, int strip, Flag flag, Dim dim) {
    if (dim == Dim::Menu)
        return Chip::MenuOpen;
    if (dim == Dim::Loading)
        return Chip::Loading;
    if (strip < 0)
        return Chip::None;
    if (Destroyed(s.party[strip]))
        return Chip::Destroyed;
    if (flag == Flag::Out)
        return Chip::NotInBattle;
    return strip == s.controlled && Piloting(s) ? Chip::Piloting : Chip::Parked;
}

inline const char *ChipText(Chip c) {
    switch (c) {
    case Chip::Parked:
        return "Parked";
    case Chip::Piloting:
        return "Piloting";
    case Chip::Destroyed:
        return "Destroyed";
    case Chip::NotInBattle:
        return "Not in battle";
    case Chip::MenuOpen:
        return "Menu open";
    case Chip::Loading:
        return "Now Loading...";
    case Chip::None:
        break;
    }
    return "";
}

// The in-combat flag of a snapshot: the driven actor of the controlled member (its Skell while
// piloting, else the character) reads state 1 in battle and 2 outside; anything else is Unknown.
inline Flag FlagFor(const Snapshot &s) {
    if (!s.valid || s.controlled < 0 || s.controlled > 3)
        return Flag::Unknown;
    const auto &m = s.party[s.controlled];
    const Actor &a = m.skell.present && m.skell.driven ? m.skell : m.pc;
    if (!a.present || a.battle_state < 0)
        return Flag::Unknown;
    return a.battle_state == 1 ? Flag::In : Flag::Out;
}

// The party's in-combat flag (owner 2026-11-23: the automatic view follows the whole party): In while
// any member's driven actor (its Skell while it rides it, else the character) reads state 1, Out when
// at least one reads and none is in battle, Unknown when no member's state is readable. The controlled
// character alone reads Out while its weapon is sheathed mid-fight (BATTLE-LIVE-4), so the page, the
// chip, Set and Return follow this flag.
inline Flag PartyFlag(const Snapshot &s) {
    if (!s.valid)
        return Flag::Unknown;
    bool readable = false;
    for (const auto &m : s.party) {
        if (!m.pc.present)
            continue;
        const Actor &a = m.skell.present && (m.skell.driven || m.mounted) ? m.skell : m.pc;
        if (!a.present || a.battle_state < 0)
            continue;
        if (a.battle_state == 1)
            return Flag::In;
        readable = true;
    }
    return readable ? Flag::Out : Flag::Unknown;
}

// The Set plate may switch to this member: never to one riding a Skell. The helper's switch hands
// control to the member's character while its Skell stays AI-driven, which locks every input
// (BATTLE-LIVE-1 BUG 1, BATTLE-LIVE-2): refused with "Not available now".
inline bool SwitchTargetAllowed(const Member &m) {
    return m.pc.present && !m.mounted && !(m.skell.present && m.skell.driven);
}

// The Set plate is off during a battle (owner 2026-11-22): a switch mid-battle ended the fight and left
// the stick dead for ~1 min (BATTLE-LIVE-2). Off at once when the flag (last known one for an unreadable
// sample) reads In, and on again only when the automatic view's leave debounce has run out (AutoSwitch
// Engaged: Out for ExitMs), so the plate does not flicker between waves. `party` (PartyInBattle): the
// controlled actor reads Out while its weapon is sheathed (B) although the AI members keep fighting
// (BATTLE-LIVE-4), so any member still in battle keeps the plate off too.
inline bool SetBlockedInBattle(Flag known, bool engaged, bool party = false) {
    return known == Flag::In || engaged || party;
}

// Any party member's driven actor (its Skell while it rides it, else the character) reads state 1.
inline bool PartyInBattle(const Snapshot &s) {
    if (!s.valid)
        return false;
    for (const auto &m : s.party) {
        if (!m.pc.present)
            continue;
        const Actor &a = m.skell.present && (m.skell.driven || m.mounted) ? m.skell : m.pc;
        if (a.present && a.battle_state == 1)
            return true;
    }
    return false;
}

// The automatic Battle view (BATTLE-UX s1 option A). Pure: the module feeds it the raw flag, the
// dimmed state and the page each sample, and every tab tap; Update returns the page to show (an
// automatic change: fade 250 ms, no haptic) or -1.
//   enter   flag In for EnterMs, page not dimmed, no tab tap in the last TapQuietMs
//   leave   flag Out for ExitMs (FlickerExitMs after more than FlickerFlips flips in FlickerWindowMs);
//           the clock pauses while the page is dimmed; back to the page shown before Battle (Map
//           when that was Battle itself or Settings), only if Battle is still shown
//   manual  any tab tap away from Battle during a battle suppresses re-entry until it ends
class AutoSwitch {
public:
    static constexpr std::int64_t EnterMs = 400, ExitMs = 2000, FlickerExitMs = 6000,
                                  TapQuietMs = 3000, FlickerWindowMs = 10000;
    static constexpr int FlickerFlips = 3, BattlePage = 4, SettingsPage = 3, MapPage = 0;

    int Update(std::int64_t now, Flag flag, bool dimmed, int page) {
        if (flag == Flag::Unknown)
            flag = last_; // a single unreadable sample (actor handover) changes nothing
        const std::int64_t dt = last_now_ ? now - last_now_ : 0;
        last_now_ = now;
        if (flag != last_ && last_ != Flag::Unknown && flag != Flag::Unknown) {
            flips_.push_back(now);
            since_ = now;
        } else if (flag != last_) {
            since_ = now;
        }
        last_ = flag;
        while (!flips_.empty() && now - flips_.front() > FlickerWindowMs)
            flips_.pop_front();
        if (engaged_ && int(flips_.size()) > FlickerFlips)
            flicker_ = true;
        if (flag == Flag::Out && dimmed && engaged_)
            since_ += dt; // a native menu or loading screen pauses the leave clock
        int target = -1;
        if (!engaged_ && flag == Flag::Out)
            override_ = false; // a tap during a blip that never became a battle
        if (!engaged_) {
            if (flag == Flag::In && now - since_ >= EnterMs) {
                engaged_ = true;
                entered_ = false;
            }
        } else if (flag == Flag::Out && now - since_ >= (flicker_ ? FlickerExitMs : ExitMs)) {
            if (page == BattlePage && entered_)
                target = prev_ == BattlePage || prev_ == SettingsPage ? MapPage : prev_;
            engaged_ = entered_ = override_ = flicker_ = false;
            return target;
        }
        if (engaged_ && !entered_ && !override_) {
            if (page == BattlePage) { // already there (by hand before the fight)
                entered_ = true;
                prev_ = BattlePage;
            } else if (!dimmed && (last_tap_ == kNever || now - last_tap_ >= TapQuietMs)) {
                entered_ = true;
                prev_ = page;
                target = BattlePage;
            }
        }
        return target;
    }
    // A tab tap from `from` to `to` (both module pages).
    void OnTabTap(std::int64_t now, int from, int to) {
        last_tap_ = now;
        if (!engaged_ && last_ != Flag::In)
            return;
        if (to != BattlePage) {
            override_ = true;
            entered_ = false;
        } else if (from != BattlePage) {
            entered_ = true; // opened by hand mid-battle: the end of the fight still returns
            prev_ = from;
        }
    }
    bool Engaged() const { return engaged_; }
    bool Overridden() const { return override_; }
    bool Flicker() const { return flicker_; }
    Flag Last() const { return last_; }

private:
    static constexpr std::int64_t kNever = INT64_MIN;
    Flag last_{Flag::Unknown};
    std::int64_t since_{}, last_now_{}, last_tap_{kNever};
    bool engaged_{}, entered_{}, override_{}, flicker_{};
    int prev_{MapPage};
    std::deque<std::int64_t> flips_;
};

// The insurance tile's text: the count when traced, else the pending form.
inline std::string InsuranceText(const Member &m) {
    if (!InsuranceLive || !m.owned)
        return Pending;
    return std::to_string(int(m.insurance));
}

// Return to Skell (a 600 ms hold) on the Battle page. ReturnMidBattleLive: tested live mid-battle
// (BATTLE-LIVE-5), the hold acts as on the other pages (the helper's own can_return gate still
// applies); off, it stays refused unless the in-combat flag says the party is out of battle.
inline constexpr bool ReturnMidBattleLive = true;
inline bool ReturnAllowed(Flag flag) { return ReturnMidBattleLive || flag == Flag::Out; }

// The Battle Commands shortcut presses the game's + button (owner-approved fallback). Offered only
// while the field takes input: no menu, scene or loading screen, and the helper's state is fresh.
inline bool CommandsAvailable(bool data_valid, bool helper_fresh, bool field_ok, bool input_ready) {
    return data_valid && helper_fresh && field_ok && input_ready;
}
} // namespace xcx::battle
