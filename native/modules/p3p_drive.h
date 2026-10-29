// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Item use and equipment changes through the game's own camp menu, one button at a time (the P5R
// companion's native-menu drive). The module never writes to the game: each sample Step() looks
// at the camp menu (p3p::CampMenu) and asks for at most one press, which the manifest's enforce
// rules make. A press is asked for only once the menu has held still for a while, so every step
// is a reaction to what the game shows, never a blind sequence; a press the game missed is simply
// made again.
//
// Live flows (harness, lobby clock save). X opens the camp menu on its command list (cursor 0);
// the cursor walks to the command and A enters it.
//   ITEM (command 1): Use tab (L/R switch tabs); the cursor walks to the item (entry row + scroll)
//   and A asks "Who will you use it on?"; the target cursor walks to the member and A uses the
//   item at once, staying on the target list. An item with no effect (SP already full) does
//   nothing on A.
//   EQUIP (command 3): "Change whose equipment?" (member cursor, A), then the four slots (slot
//   cursor, A), then the slot's list (row + scroll); A equips at once and stays on the list.
//   PERSONA (command 2): the stock list; A assigns the row at once and stays on the list. X
//   there discards, so X is only ever pressed with no camp menu open.
// B backs out one level at a time.
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "p3p_reader.h"

namespace p3p_drive {

enum class Button { None = -1, A, B, X, L, Up, Down };

/// What the drive sees each sample.
struct Seen {
    bool ready{};      // the reader's snapshot is valid this sample
    bool blocked{};    // a battle, a message window or anything else that must not be driven through
    bool camp_error{}; // the camp task is listed but could not be read this sample
    std::optional<p3p::CampMenu> camp;
    int equipped{-1};  // equipment requests: what the member wears in the slot now (-1 unknown)
    int selected{-1};  // the protagonist's selected Persona ID now (-1 unknown)
};

/// One request: use `item` on unit `member`; equip `item` on unit `member` in `slot`; or select
/// Persona ID `item`, which is row `slot` of a stock of `member` Personas.
struct Request {
    enum class Kind { Use, Equip, Persona } kind{Kind::Use};
    int item{}, member{}, slot{};
    std::string item_name, member_name;
};

class Drive {
public:
    // A press lasts 8 ticks. The harness runs the game at ~22 fps, where shorter presses and
    // closer spacing were missed now and then.
    static constexpr int Settle = 12;     // samples the menu must hold still before a press
    static constexpr int Spacing = 24;    // samples between presses
    static constexpr int Timeout = 900;   // samples for the whole request (15 s at 60 Hz)
    static constexpr int CloseTimeout = 600;
    static constexpr int EffectWait = 90; // samples to see the change after an A
    static constexpr int ItemCommand = 1, PersonaCommand = 2, EquipCommand = 3;

    bool active() const { return phase_ != Phase::Idle; }
    const std::string& status() const { return status_; }
    bool succeeded() const { return ok_; }
    const Request& request() const { return req_; }

    /// Begins a request. Refused (false, status says why) while another runs or the game's camp
    /// menu is already open.
    bool Start(Request r, const Seen& now) {
        if (active()) { status_ = "The companion is already busy"; return false; }
        if (!now.ready || now.blocked || now.camp_error) { status_ = "The game is busy right now"; return false; }
        if (now.camp) { status_ = "Close the game's menu first"; return false; }
        if (r.kind == Request::Kind::Equip && now.equipped == r.item) {
            status_ = r.member_name + " already has " + r.item_name; return false;
        }
        if (r.kind == Request::Kind::Persona && now.selected == r.item) { status_ = r.item_name + " is already in use"; return false; }
        *this = Drive{};
        req_ = std::move(r);
        phase_ = Phase::Open;
        status_ = req_.kind == Request::Kind::Use     ? "Using " + req_.item_name + " on " + req_.member_name + "..."
                  : req_.kind == Request::Kind::Equip ? "Equipping " + req_.member_name + " with " + req_.item_name + "..."
                                                      : "Switching to " + req_.item_name + "...";
        return true;
    }
    void Cancel() { if (active() && phase_ != Phase::Close) Finish(false, "Cancelled"); }

    /// One sample: the button to press now, if any.
    Button Step(const Seen& s) {
        if (!active()) return Button::None;
        ++frames_; ++since_press_;
        if (s.camp != last_) { last_ = s.camp; still_ = 0; } else ++still_;
        if (s.blocked) { // never drive through a battle or a message; leave the menu to the player
            phase_ = Phase::Idle; ok_ = false; status_ = "Stopped: the game needs you"; return Button::None;
        }
        if (phase_ == Phase::Close) {
            if (!s.camp && !s.camp_error) { phase_ = Phase::Idle; return Button::None; }
            if (frames_ > close_deadline_) { phase_ = Phase::Idle; return Button::None; }
            return Backable(s) && Calm() ? Press(Button::B) : Button::None;
        }
        if (frames_ > Timeout) return Fail("Timed out");
        if (!s.ready || s.camp_error || !Calm()) return Button::None;
        if (!s.camp) {
            if (phase_ != Phase::Open) return Fail("The menu closed");
            if (opened_ && since_press_ < 120) return Button::None;
            if (opened_) return Fail("The menu did not open");
            opened_ = true;
            return Press(Button::X);
        }
        const auto& c = *s.camp;
        if (phase_ == Phase::Open) phase_ = Phase::Walk;
        const int command = req_.kind == Request::Kind::Use ? ItemCommand : req_.kind == Request::Kind::Equip ? EquipCommand : PersonaCommand;
        if (c.state == 2 && c.sub == 0) { // the command list
            if (!c.input) return Button::None;
            if (phase_ != Phase::Walk) return Fail("The menu changed");
            const auto it = std::find(c.commands.begin(), c.commands.end(), command);
            if (it == c.commands.end()) return Fail("That command isn't in the menu");
            return Walk(c.cursor, int(it - c.commands.begin()), Button::A);
        }
        if (c.state == 2 || c.state == 3 || c.state == 13) return Button::None; // between screens
        if (c.cursor < 0 || c.cursor >= int(c.commands.size()) || c.commands[c.cursor] != command)
            return Press(Button::B); // inside another command
        return req_.kind == Request::Kind::Use ? StepUse(c) : req_.kind == Request::Kind::Equip ? StepEquip(c, s.equipped) : StepPersona(c, s.selected);
    }

private:
    enum class Phase { Idle, Open, Walk, Tried, Used, Close };

    Button StepUse(const p3p::CampMenu& c) {
        if (c.state != 4) return Button::None;
        const auto entry = std::find_if(c.use_list.begin(), c.use_list.end(), [&](auto e) { return e.first == req_.item; });
        const int have = entry == c.use_list.end() ? 0 : entry->second;
        if (phase_ == Phase::Tried && c.sub == 4) phase_ = Phase::Walk; // the item asks for a target
        if (phase_ == Phase::Used || phase_ == Phase::Tried) {
            if (have < before_) return Done("Used " + req_.item_name + " on " + req_.member_name);
            if (since_press_ < EffectWait) return Button::None;
            // A press can be missed while the screen settles: ask again, twice at most.
            if (((phase_ == Phase::Used && c.sub == 4) || (phase_ == Phase::Tried && c.sub == 3)) && retries_ < 2) {
                ++retries_; return Press(Button::A);
            }
            if (phase_ == Phase::Used) return Fail(req_.item_name + " had no effect on " + req_.member_name);
            return Fail(req_.item_name + " can't be used here"); // A neither asked for a target nor used it
        }
        if (c.sub == 3) { // the item list
            if (c.tab != 0) return Press(Button::L);
            if (entry == c.use_list.end() || have < 1) return Fail("No " + req_.item_name + " left");
            const int want = int(entry - c.use_list.begin());
            if (c.row + c.scroll == want) { before_ = have; phase_ = Phase::Tried; }
            return Walk(c.row + c.scroll, want, Button::A);
        }
        if (c.sub == 4) { // choosing a target
            const auto t = std::find(c.targets.begin(), c.targets.end(), req_.member);
            if (t == c.targets.end()) return Fail(req_.member_name + " can't be chosen");
            const int want = int(t - c.targets.begin());
            if (c.target == want) { before_ = have; phase_ = Phase::Used; }
            return Walk(c.target, want, Button::A);
        }
        return Button::None;
    }

    Button StepEquip(const p3p::CampMenu& c, int equipped) {
        if (c.state != 6) return Button::None;
        if (equipped == req_.item) return Done("Equipped " + req_.member_name + " with " + req_.item_name);
        if (phase_ == Phase::Used) {
            if (since_press_ < EffectWait) return Button::None;
            if (c.sub == 10 && retries_ < 2) { ++retries_; return Press(Button::A); }
            return Fail(req_.member_name + " can't equip " + req_.item_name);
        }
        const auto who = std::find(c.members.begin(), c.members.end(), req_.member);
        if (who == c.members.end()) return Fail(req_.member_name + " can't be chosen");
        const int member = int(who - c.members.begin());
        if (c.sub == 5) return Walk(c.member, member, Button::A); // whose equipment
        if (c.member != member || (c.sub == 10 && c.slot != req_.slot)) return Press(Button::B);
        if (c.sub == 7) return Walk(c.slot, req_.slot, Button::A); // which slot
        if (c.sub == 10) { // the slot's list
            const auto entry = std::find_if(c.equip_list.begin(), c.equip_list.end(), [&](auto e) { return e.first == req_.item; });
            if (entry == c.equip_list.end()) return Fail(req_.member_name + " can't equip " + req_.item_name);
            const int want = int(entry - c.equip_list.begin()), at = c.equip_row + c.equip_scroll;
            if (at == want) phase_ = Phase::Used;
            return Walk(at, want, Button::A);
        }
        return Button::None;
    }

    Button StepPersona(const p3p::CampMenu& c, int selected) {
        if (c.state != 7) return Button::None;
        if (selected == req_.item) return Done("Switched to " + req_.item_name);
        if (c.sub != 6) return Button::None;
        if (phase_ == Phase::Used) {
            if (since_press_ < EffectWait) return Button::None;
            if (retries_ < 2) { ++retries_; return Press(Button::A); }
            return Fail("Couldn't switch to " + req_.item_name);
        }
        // The game's list must be the stock the companion showed, row for row.
        if (c.personas != req_.member || req_.slot < 0 || req_.slot >= c.personas) return Fail("The Persona list changed");
        if (c.persona_row == req_.slot) phase_ = Phase::Used;
        return Walk(c.persona_row, req_.slot, Button::A);
    }

    // Moves a cursor one step toward `want`, or presses `then` once it is there.
    Button Walk(int at, int want, Button then) { return Press(at == want ? then : at < want ? Button::Down : Button::Up); }
    // Back out only where B is taken, never while the menu animates (3 opening, 13 closing).
    static bool Backable(const Seen& s) {
        if (!s.camp) return false;
        const auto& c = *s.camp;
        return (c.state == 2 && c.input) || (c.state == 4 && (c.sub == 3 || c.sub == 4)) ||
               (c.state == 6 && (c.sub == 5 || c.sub == 7 || c.sub == 10)) || (c.state == 7 && c.sub == 6);
    }
    bool Calm() const { return still_ >= Settle && since_press_ >= Spacing; }
    Button Press(Button b) {
        if (++presses_ > 80) return Fail("Too many steps");
        since_press_ = 0; still_ = 0;
        return b;
    }
    Button Done(std::string why) { Finish(true, std::move(why)); return Button::None; }
    Button Fail(std::string why) { Finish(false, std::move(why)); return Button::None; }
    void Finish(bool ok, std::string why) {
        ok_ = ok; status_ = std::move(why);
        phase_ = Phase::Close; close_deadline_ = frames_ + CloseTimeout;
    }
    Phase phase_{Phase::Idle};
    Request req_;
    int before_{}, retries_{};
    std::string status_;
    bool opened_{}, ok_{};
    int frames_{}, since_press_{1000}, still_{}, presses_{}, close_deadline_{};
    std::optional<p3p::CampMenu> last_;
};

} // namespace p3p_drive
