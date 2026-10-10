// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Heal All / Handy Heal All without input or menu: the guest load plan (tools/dq3/gen_load_plan.py)
// hooks the engine main loop and, at most once per frame on the game thread, runs the field Misc
// menu's own Heal All case (main+0x8B9A00 behind the menu's Romaria-throne and no-spells checks).
// The game decides casters, spells, MP costs, revival / cures and the Info counter exactly as from
// the menu; the companion only asks and then shows the menu's own result line. Pure, unit-tested.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace dq3 {

/// Mailbox layout (gen_load_plan.py).
namespace heal_mb {
inline constexpr std::uint32_t Seq = 0x00, Taken = 0x04, Heartbeat = 0x08, Done = 0x0C, Mode = 0x10,
                               Result = 0x18, Size = 0x1000;
inline constexpr std::uint32_t Cancelled = 0xFFFFFFFFu; ///< a mode the helper refuses (0xFF), no call
} // namespace heal_mb

/// What the Misc menu would have said.
enum class HealOutcome : int {
    Healed,        ///< someone was healed (Txt_FieldMenu_Message_AllHeal / _Heal)
    NothingToHeal, ///< 6 / 6 (Txt_FieldMenu_Message_Mantan_Fail / AlmostMantan_Fail)
    MagicFailed,   ///< both 4..6 otherwise (Txt_FieldMenu_Message_Mantan_FailMagic)
    King,          ///< the Romaria throne, King (Txt_FieldMenu_King_Message_MAXHEAL / ALMOSTHEAL)
    Queen,         ///< the Romaria throne, Queen (Txt_FieldMenu_Queen_Message_...)
    Royal,         ///< the throne flag without a King / Queen: the menu does nothing and says nothing
    NoSpellsHere,  ///< the area forbids spells (Txt_Magic_Battle_Invalid_Common)
    Cancelled,     ///< the helper refused the mode (a cancelled request)
};

/// The helper's result word: 0x10000 | a | b << 8 after the Heal All call, 0x20 | royal kind << 8,
/// 0x30 no spells here, 0xFF cancelled. nullopt for any other word.
inline std::optional<HealOutcome> DecodeHealResult(std::uint64_t raw) {
    if ((raw >> 16) == 1) {
        const unsigned a = raw & 0xFF, b = (raw >> 8) & 0xFF;
        if (a == 6 && b == 6)
            return HealOutcome::NothingToHeal;
        if (a - 4 <= 2 && b - 4 <= 2)
            return HealOutcome::MagicFailed;
        return HealOutcome::Healed;
    }
    if ((raw & ~std::uint64_t{0xFF00}) == 0x20) {
        const auto kind = (raw >> 8) & 0xFF;
        return kind == 1 ? HealOutcome::King : kind == 2 ? HealOutcome::Queen : HealOutcome::Royal;
    }
    if (raw == 0x30)
        return HealOutcome::NoSpellsHere;
    if (raw == 0xFF)
        return HealOutcome::Cancelled;
    return std::nullopt;
}

/// The text id the Misc menu shows for `o` (mode 0 Heal All, 1 Handy Heal All; `single` = a party of one,
/// whose line names the member); nullptr when the menu shows nothing.
inline const char* HealMessageId(HealOutcome o, int mode, bool single) {
    switch (o) {
    case HealOutcome::Healed:
        return single ? "Txt_FieldMenu_Message_Heal" : "Txt_FieldMenu_Message_AllHeal";
    case HealOutcome::NothingToHeal:
        return mode ? "Txt_FieldMenu_Message_AlmostMantan_Fail" : "Txt_FieldMenu_Message_Mantan_Fail";
    case HealOutcome::MagicFailed:
        return "Txt_FieldMenu_Message_Mantan_FailMagic";
    case HealOutcome::King:
        return mode ? "Txt_FieldMenu_King_Message_ALMOSTHEAL" : "Txt_FieldMenu_King_Message_MAXHEAL";
    case HealOutcome::Queen:
        return mode ? "Txt_FieldMenu_Queen_Message_ALMOSTHEAL" : "Txt_FieldMenu_Queen_Message_MAXHEAL";
    case HealOutcome::NoSpellsHere:
        return "Txt_Magic_Battle_Invalid_Common";
    case HealOutcome::Royal:
    case HealOutcome::Cancelled:
        break;
    }
    return nullptr;
}

/// Every id HealMessageId can return (the text tables keep these rows).
inline bool IsHealMessageId(std::string_view id) {
    for (const auto o : {HealOutcome::Healed, HealOutcome::NothingToHeal, HealOutcome::MagicFailed, HealOutcome::King,
                         HealOutcome::Queen, HealOutcome::NoSpellsHere})
        for (int mode = 0; mode < 2; ++mode)
            for (const bool single : {false, true})
                if (const char* m = HealMessageId(o, mode, single); m && id == m)
                    return true;
    return false;
}

/// The Heal All / Handy Heal All button gate: never in battle (the Misc menu only exists on the field),
/// only in a free adventure (no loading, event hold or unreadable party), with the native field menu
/// closed (the companion never acts behind an open game menu), without the party drive, and only while
/// the load plan's helper is alive and idle.
struct HealGate {
    bool battle{}, adventure_free{}, menu_closed{}, party_drive{}, helper_alive{}, busy{};
    bool Open() const {
        return !battle && adventure_free && menu_closed && !party_drive && helper_alive && !busy;
    }
};

/// The single-member line names the member: "<Cap><DefSgl_WORD>'s wounds are healed!" with the
/// name in place of the word tag (the remaining tags are left to CleanGameText).
inline std::u32string NameHealMessage(std::u32string_view raw, std::u32string_view name) {
    static constexpr std::u32string_view Word = U"<DefSgl_WORD>";
    std::u32string out{raw};
    if (const auto at = out.find(Word); at != std::u32string::npos && !name.empty())
        out.replace(at, Word.size(), name);
    return out;
}

/// Aligned mailbox accessors (the host's base extensions: loads acquire, stores release).
struct HealMailboxIo {
    std::function<bool(std::uint32_t, std::uint32_t*)> load32;
    std::function<bool(std::uint32_t, std::uint32_t)> store32;
    std::function<bool(std::uint32_t, std::uint64_t*)> load64;
};

/// One request at a time. The stub serves a request when seq != taken, so the mode is written
/// before seq; a new request waits until done == seq (a cancelled one still owns seq until the stub
/// has taken it and the helper refused its mode).
class HealMailbox {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto AliveFor = std::chrono::seconds(1);  ///< heartbeat freshness (a frame is 33 ms)
    static constexpr auto ServeFor = std::chrono::seconds(2);  ///< a request not done by then is cancelled

    struct Finished {
        bool timed_out{};
        std::uint64_t raw{};
        int mode{};
    };

    void Configure(HealMailboxIo io_) {
        io = std::move(io_);
        waiting = false;
        last_beat_change = {};
    }
    bool Present() const {
        return io.load32 && io.store32 && io.load64;
    }
    /// Once per sample: notes a heartbeat change.
    void Beat(Clock::time_point now) {
        std::uint32_t beat = 0;
        if (Present() && io.load32(heal_mb::Heartbeat, &beat) && (beat != last_beat || !beat_seen)) {
            if (beat_seen)
                last_beat_change = now;
            last_beat = beat;
            beat_seen = true;
        }
    }
    /// The stub ran within the last second (the load plan is installed and the game loop runs).
    bool Alive(Clock::time_point now) const {
        return Present() && last_beat_change != Clock::time_point{} && now - last_beat_change < AliveFor;
    }
    bool Busy() const {
        return waiting;
    }
    /// Publishes a request for `mode` (0 / 1). False when busy, dead or the stub still owns seq.
    bool Request(int mode, Clock::time_point now) {
        if (mode < 0 || mode > 1 || waiting || !Alive(now))
            return false;
        std::uint32_t seq = 0, done = 0;
        if (!io.load32(heal_mb::Seq, &seq) || !io.load32(heal_mb::Done, &done) || done != seq)
            return false;
        if (!io.store32(heal_mb::Mode, static_cast<std::uint32_t>(mode)) || !io.store32(heal_mb::Seq, seq + 1))
            return false;
        pending_seq = seq + 1;
        pending_mode = mode;
        issued = now;
        waiting = true;
        return true;
    }
    /// Once per sample after Beat: the finished request (result or timeout), else nullopt.
    std::optional<Finished> Poll(Clock::time_point now) {
        if (!waiting)
            return std::nullopt;
        std::uint32_t done = 0;
        if (io.load32(heal_mb::Done, &done) && done == pending_seq) {
            waiting = false;
            std::uint64_t raw = 0;
            if (!io.load64(heal_mb::Result, &raw))
                return Finished{true, 0, pending_mode};
            return Finished{false, raw, pending_mode};
        }
        if (now - issued > ServeFor) {
            // the game loop stopped (a pause): the stub skips a refused mode once it runs again
            waiting = false;
            io.store32(heal_mb::Mode, heal_mb::Cancelled);
            return Finished{true, 0, pending_mode};
        }
        return std::nullopt;
    }

private:
    HealMailboxIo io;
    bool waiting{}, beat_seen{};
    std::uint32_t pending_seq{}, last_beat{};
    int pending_mode{};
    Clock::time_point issued{}, last_beat_change{};
};

} // namespace dq3
