// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Module side of the Xenoblade Chronicles X DE 1.0.2 guest helper (native/helper/xcx_guest_helper.S,
// format-2 load plan packages/XenobladeChroniclesXDefinitiveEdition/dualscreen/guest/<build>/
// load-plan.json). Board / Exit Skell are direct calls to the game's own request functions, made on
// the game's own thread from the field HUD's per-frame update; no button is ever simulated.
//
// The offsets below are the single source of truth shared with the assembly (which encodes them as
// literal immediates). Header-only and host-independent: Bridge is templated on two
// offset-addressed primitives (the host extension's load_u32 / store_u32: loads acquire, stores
// release) and time is passed in as milliseconds, so the unit test drives it with a fake mailbox and
// a fake clock.
//
// What the helper can and cannot prove: it runs only while the game's field HUD update runs, so
// `heartbeat` advancing means "the field HUD is updating", not "the player has control". Whether it
// keeps running during menus, events and loading is UNVERIFIED LIVE; if it stops, the heartbeat goes
// stale and the button is disabled, and a request that is never answered times out and is cancelled.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace xcx_helper {

inline constexpr std::uint32_t MailboxSize = 4096;
inline constexpr std::uint32_t EpochOffset = 4092; // host-owned epoch word (mailbox_epoch_offset)

inline constexpr std::uint32_t OffsetModuleMagic = 0x00; // module: 'XCM1'
inline constexpr std::uint32_t OffsetGuestMagic = 0x04;  // guest: 'XCG1' every hook call
inline constexpr std::uint32_t OffsetHeartbeat = 0x08;   // guest: ++ every hook call
inline constexpr std::uint32_t OffsetState = 0x0C;       // guest: state word (below), every 10th call
inline constexpr std::uint32_t OffsetPollBeat = 0x10;    // guest: ++ every state publication
inline constexpr std::uint32_t OffsetPollCountdown = 0x14; // guest-private
inline constexpr std::uint32_t OffsetCtrlHandle = 0x18;  // guest, diagnostics: HUD controlled handle
inline constexpr std::uint32_t OffsetReqSeq = 0x20;      // module, release, written last
inline constexpr std::uint32_t OffsetReqOp = 0x24;
inline constexpr std::uint32_t OffsetReqEpoch = 0x28;
inline constexpr std::uint32_t OffsetCancelSeq = 0x2C;   // module: "do not run req_seq any more"
inline constexpr std::uint32_t OffsetAckSeq = 0x30;      // guest, release, written last
inline constexpr std::uint32_t OffsetResultCode = 0x34;
inline constexpr std::uint32_t OffsetResultValue = 0x38; // the game's own return (1 = submitted)
inline constexpr std::uint32_t OffsetAckBeat = 0x3C;
inline constexpr std::uint32_t OffsetDiagCharacter = 0x40; // u64 actor pointers, diagnostics only
inline constexpr std::uint32_t OffsetDiagSkell = 0x48;
inline constexpr std::uint32_t OffsetDiagControlled = 0x50;
inline constexpr std::uint32_t OffsetAnswered = 0x58;    // guest: requests answered so far

// OP_SWITCH words. The module writes ReqArg/ReqTarget before req_seq (release).
inline constexpr std::uint32_t OffsetReqArg = 0x5C;      // module: k, the party slot whose member becomes controlled (1..3)
inline constexpr std::uint32_t OffsetReqTarget = 0x60;   // module: token (kind<<8|id) of the member it showed in slot k
inline constexpr std::uint32_t OffsetSlot0Token = 0x64;  // guest, every poll: slot 0's token, 0xffffffff unknown
inline constexpr std::uint32_t OffsetSubmitToken = 0x68; // guest: token of the member the last submitted switch makes controlled
inline constexpr std::uint32_t OffsetSubmitCount = 0x6C; // guest: ++ per submitted switch
inline constexpr std::uint32_t OffsetInputMask = 0x70;    // guest, diagnostics: byte +0xb0 of the object at main+0x1fefb70 (0xffffffff: none)
// Diagnostic words for the live diff (LOADING-AND-RETURN Q1), written every poll while the HUD object is verified:
inline constexpr std::uint32_t OffsetDiagActionBlock = 0x74; // u32 [*(main+0x1ff3698)+0x44]
inline constexpr std::uint32_t OffsetDiagBtlLock = 0x78;     // u32 main+0x26e1950
inline constexpr std::uint32_t OffsetDiagFlag2308 = 0x7C;    // u8 main+0x26e2308
inline constexpr std::uint32_t OffsetDiagMode1618 = 0x80;    // u32 main+0x1dc1618
inline constexpr std::uint32_t OffsetDiagWorldBits = 0x84;   // u32 [*(main+0x1df5960)+0x10]
inline constexpr std::uint32_t OffsetDiagWorldByte12 = 0x88; // u8 [*(main+0x1df5960)+0x12]
inline constexpr std::uint32_t OffsetDiagFade = 0x8C;        // low32 of F = *(*(main+0x1dc1410)+0x94b98), 0 none
inline constexpr std::uint32_t OffsetDiagFadeLayers = 0x90;  // (F[+0x10]-F[+0x8])/8
inline constexpr std::uint32_t OffsetDiagFadeLock = 0x94;    // u8 F+0x70
inline constexpr std::uint32_t OffsetDiagHudFlags = 0x98;    // u8 H+0x210d0 | u8 H+0x210d1 << 8
inline constexpr std::uint32_t OffsetDiagWorldId = 0x9C;     // u32 [*(main+0x1df5968)+0x140]
inline constexpr std::uint32_t OffsetDiagZone = 0xA0;        // u32 [*(main+0x1df5968)+0xa8]
inline constexpr std::uint32_t OffsetDiagEventByte = 0xA4;   // u8 main+0x1fefb38
inline constexpr std::uint32_t OffsetDiagObj1f91 = 0xA8;     // u8 [*(main+0x1f91e30)+0x14]
inline constexpr std::uint32_t OffsetDiagObj1fef = 0xAC;     // low32 of [*(main+0x1fefa20)+0x30]
inline constexpr std::uint32_t OffsetDiagReturnByte = 0xB0;  // u8 H+0x263a5 (the Return request byte)
inline constexpr std::uint32_t DiagWords = 16;
inline constexpr std::uint32_t OffsetScratch = 0x400;    // guest-private 0x400 bytes (0xd75c / 0x56bed0 buffer); never touched by the module
inline constexpr std::uint32_t OffsetScratchAux1 = 0x7F4; // u8 filled by 0x56bed0
inline constexpr std::uint32_t OffsetScratchAux2 = 0x7F6; // u16
inline constexpr std::uint32_t UnknownToken = 0xFFFFFFFFu;

inline constexpr std::uint32_t ModuleMagic = 0x314D4358; // "XCM1" little-endian
inline constexpr std::uint32_t GuestMagic = 0x31474358;  // "XCG1"
inline constexpr std::uint32_t PollEveryCalls = 10;      // the guest publishes the state every 10th call

// State word bits (OffsetState), written by the guest only.
inline constexpr std::uint32_t StateValid = 1u << 0;     // the controlled object is the controlled
                                                         // record's character (type 2) or Skell (type 4)
inline constexpr std::uint32_t StatePiloting = 1u << 1;   // the Skell (type 4) carries +0xd0 bit5
inline constexpr std::uint32_t StateOnFoot = 1u << 2;     // the character (type 2) carries +0xd0 bit5
inline constexpr std::uint32_t StateGate = 1u << 3;      // DIAGNOSTIC only: *(main+0x1feffc0)+0x158 != 0; never blocks
inline constexpr std::uint32_t StateCanBoard = 1u << 4;  // game's canRideOn said yes (on foot, gate open)
inline constexpr std::uint32_t StateCanExit = 1u << 5;   // exit singleton ok and canGetOff said yes
inline constexpr std::uint32_t StateTransition = 1u << 6; // neither actor carries the driven mark (+0xd0 bit5)
inline constexpr std::uint32_t StateRecFoot = 1u << 7;   // DIAGNOSTIC: party record byte +10 (1 on foot, 0 mounted)
inline constexpr std::uint32_t StateSwitchReady = 1u << 10; // valid + on foot + request block idle + cooldown clear
inline constexpr std::uint32_t StateIdle = 1u << 11;        // the HUD request block is idle (SWITCH-PATH Q3)
inline constexpr std::uint32_t StateCooldownClear = 1u << 12; // 0xf0388() == 0
inline constexpr std::uint32_t StateInputReady = 1u << 13;  // that object's byte +0xb0 bit2 is clear: the field accepts input
inline constexpr std::uint32_t StateCanReturn = 1u << 14;   // 0xf16b0(): the native Return to Skell predicate
inline constexpr std::uint32_t StateFieldOk = 1u << 15;     // 0xf0b44(): the field is playable (0 through title, loading, fades). REQUIRED for every op and bit
inline constexpr std::uint32_t StateSlotShift = 8;       // party slot of the controlled record (0..3)
inline constexpr std::uint32_t StateSlotMask = 3;
inline constexpr std::uint32_t StateHudOk = 1u << 16;    // the hook's HUD object is the game's current one

enum class Op : std::uint32_t { None = 0, Board = 1, Exit = 2, Switch = 3, Return = 4 };

// Mirrors xcx_guest_helper.S's result codes. Submitted means the game accepted and posted the
// request; the boarding/exit itself completes asynchronously in the game.
enum class Result : std::uint32_t {
    Submitted = 0,
    NotConfigured = 1, // module side only: no helper / no answer
    BadOp = 2,
    StaleEpoch = 3,
    Cancelled = 4,
    NotApplicable = 5, // board while piloting / exit while on foot
    BadHandle = 6,     // no controlled record, unresolved handle or the controlled object differs
    RefusedByGame = 7, // tryRideOn / tryGetOff returned 0
    NotReady = 8,      // the hook's HUD object is not the game's current one
    Transitioning = 9, // neither actor carries the driven mark: boarding/exit in progress
    Cooldown = 10,     // 0xf0388() != 0: the game does not allow a switch yet
    StaleTarget = 11,  // slot k no longer holds the member the module showed
    CannotReturn = 12, // 0xf16b0() said no (the consumer would have refused): nothing was stored
};

struct State {
    bool valid = false, piloting = false, on_foot = false, gate = false, transitioning = false, rec_foot = false;
    bool can_board = false, can_exit = false, hud_ok = false;
    bool switch_ready = false, idle = false, cooldown_clear = false, input_ready = false;
    bool can_return = false, field_ok = false;
    int slot = 0;
};

inline State DecodeState(std::uint32_t w) {
    State s;
    s.valid = (w & StateValid) != 0;
    s.piloting = (w & StatePiloting) != 0;
    s.on_foot = (w & StateOnFoot) != 0;
    s.gate = (w & StateGate) != 0;
    s.can_board = (w & StateCanBoard) != 0;
    s.can_exit = (w & StateCanExit) != 0;
    s.hud_ok = (w & StateHudOk) != 0;
    s.slot = static_cast<int>((w >> StateSlotShift) & StateSlotMask);
    // The guest never sets contradictory bits; a word that does is not trusted at all.
    s.transitioning = (w & StateTransition) != 0;
    s.input_ready = (w & StateInputReady) != 0;
    s.rec_foot = (w & StateRecFoot) != 0;
    s.field_ok = (w & StateFieldOk) != 0;
    if (!s.valid || !s.input_ready || !s.field_ok) {
        // Not valid, or the game does not accept player input now (menu, event, pause): nothing is
        // eligible and every face is the prototype's.
        State t;
        t.transitioning = s.transitioning && s.hud_ok;
        t.hud_ok = s.hud_ok;
        t.input_ready = s.input_ready;
        t.field_ok = s.field_ok;
        t.slot = s.slot;
        return t;
    }
    if (s.piloting == s.on_foot) return {};
    s.transitioning = false;
    s.switch_ready = (w & StateSwitchReady) != 0;
    s.idle = (w & StateIdle) != 0;
    s.cooldown_clear = (w & StateCooldownClear) != 0;
    s.can_return = (w & StateCanReturn) != 0 && s.on_foot;
    s.field_ok = (w & StateFieldOk) != 0;
    s.switch_ready = s.switch_ready && s.on_foot && s.idle && s.cooldown_clear;
    s.can_board = s.can_board && s.on_foot;
    s.can_exit = s.can_exit && s.piloting;
    return s;
}

struct Status {
    bool helper_present = false; // guest magic seen, epoch valid and the heartbeat is fresh
    bool state_fresh = false;    // a state word was published within the adaptive window
    bool busy = false;           // a request is in flight
    State state;
    std::uint32_t heartbeat = 0, poll_beat = 0, answered = 0;
    std::array<std::uint32_t, DiagWords> diag{}; // raw diagnostic words, see OffsetDiag*
    std::uint32_t slot0_token = UnknownToken, submit_token = 0, submit_count = 0;
    std::uint32_t state_word = 0; // the raw published word, 0 unless fresh (diagnostics)
};

struct Outcome {
    bool done = false;
    bool answered = false; // false: timed out without any guest answer
    Op op = Op::None;
    Result code = Result::NotConfigured;
    std::uint32_t value = 0;
};

template <class LoadU32, class StoreU32>
class Bridge {
public:
    Bridge(LoadU32 load32, StoreU32 store32) : load32_(std::move(load32)), store32_(std::move(store32)) {}

    // Once after configure(): claim the mailbox for this module build. Never touches the epoch
    // word (the host owns it) nor anything the guest writes.
    bool Initialize() {
        std::uint32_t ack = 0;
        load32_(OffsetAckSeq, &ack);
        next_seq_ = ack; // never reuse a sequence number the guest already answered
        bool ok = store32_(OffsetCancelSeq, 0);
        ok = store32_(OffsetReqSeq, ack) && ok; // nothing pending
        ok = store32_(OffsetModuleMagic, ModuleMagic) && ok;
        return ok;
    }

    // Adaptive windows (the Sword/BOTW precedent): the guest beats once per game frame; at 30 fps
    // that is ~33 ms, at 8 fps ~125 ms. The state word comes every PollEveryCalls beats.
    std::int64_t FrameMs() const { return frame_ms_; }
    std::int64_t BeatWindowMs() const { return std::clamp<std::int64_t>(frame_ms_ * 6, 300, 1500); }
    std::int64_t PollWindowMs() const {
        return std::clamp<std::int64_t>(frame_ms_ * PollEveryCalls * 3, 600, 4000);
    }
    std::int64_t AckWindowMs() const { return std::clamp<std::int64_t>(frame_ms_ * 3 + 100, 300, 1200); }

    void Update(std::int64_t now_ms) {
        std::uint32_t magic = 0, epoch = 0, beat = 0, poll = 0, word = 0;
        const bool magic_ok = load32_(OffsetGuestMagic, &magic) && magic == GuestMagic;
        const bool epoch_ok = load32_(EpochOffset, &epoch) && epoch != 0;
        if (epoch_ok && epoch != epoch_) {
            // The host paused or invalidated the session: nothing observed before it counts any
            // more. The heartbeat and a fresh state word must be seen again.
            epoch_ = epoch;
            beat_at_ = -1;
            poll_at_ = -1;
            state_ = {};
            if (pending_) FinishUnanswered(Result::StaleEpoch);
        }
        if (load32_(OffsetHeartbeat, &beat) && beat != beat_) {
            if (beat_at_ >= 0) {
                const std::int64_t d = now_ms - beat_at_;
                const std::uint32_t n = beat - beat_; // beats skipped between two samples
                if (d > 0 && n > 0 && n < 64) {
                    const std::int64_t per = d / n;
                    if (per > 0 && per <= 1000)
                        frame_ms_ = frame_ms_ ? (frame_ms_ * 3 + per) / 4 : per;
                }
            }
            if (beat_seen_) beat_at_ = now_ms; // the first value read is history, not a beat
            beat_ = beat;
            beat_seen_ = true;
        }
        status_.heartbeat = beat_;
        status_.helper_present = magic_ok && epoch_ok && beat_at_ >= 0 && now_ms - beat_at_ <= BeatWindowMs();
        if (!status_.helper_present) {
            // The heartbeat went silent (the HUD update stopped): a state word from before the
            // silence says nothing about now; a fresh publication must be seen after it resumes.
            poll_at_ = -1;
            state_ = {};
        }
        if (load32_(OffsetPollBeat, &poll) && poll != poll_beat_) {
            // Only a state word published after the heartbeat was alive counts as fresh.
            if (poll_seen_ && load32_(OffsetState, &word)) {
                state_ = DecodeState(word);
                state_word_ = word;
                poll_at_ = now_ms;
            }
            poll_beat_ = poll;
            poll_seen_ = true;
        }
        status_.poll_beat = poll_beat_;
        status_.state_fresh = status_.helper_present && poll_at_ >= 0 && now_ms - poll_at_ <= PollWindowMs();
        status_.state = status_.state_fresh ? state_ : State{};
        status_.state_word = status_.state_fresh ? state_word_ : 0;
        load32_(OffsetAnswered, &status_.answered);
        status_.slot0_token = UnknownToken;
        std::uint32_t tok = 0;
        if (status_.state_fresh && load32_(OffsetSlot0Token, &tok)) status_.slot0_token = tok;
        for (std::uint32_t i = 0; i < DiagWords; ++i) load32_(OffsetDiagActionBlock + 4 * i, &status_.diag[i]);
        load32_(OffsetSubmitToken, &status_.submit_token);
        load32_(OffsetSubmitCount, &status_.submit_count);
        PollPending(now_ms);
        status_.busy = pending_;
    }

    const Status& status() const { return status_; }

    // Stage one request. Callers decide eligibility first; this re-checks only what the mailbox
    // itself needs (helper present, nothing in flight, non-zero epoch).
    bool Stage(Op op, std::int64_t now_ms, std::uint32_t arg = 0, std::uint32_t target = 0) {
        if (pending_ || !status_.helper_present || epoch_ == 0 || op == Op::None) return false;
        std::uint32_t seq = next_seq_ + 1;
        if (seq == 0) seq = 1;
        bool ok = store32_(OffsetReqOp, static_cast<std::uint32_t>(op));
        ok = store32_(OffsetReqArg, arg) && ok;
        ok = store32_(OffsetReqTarget, target) && ok;
        ok = store32_(OffsetReqEpoch, epoch_) && ok;
        ok = store32_(OffsetReqSeq, seq) && ok; // release, last
        if (!ok) return false;
        next_seq_ = seq;
        pending_ = true;
        pending_op_ = op;
        staged_at_ = now_ms;
        cancel_at_ = -1;
        status_.busy = true;
        return true;
    }

    // Module teardown: cancel anything staged, then withdraw the module magic -- the guest takes no
    // request without it. The hook itself stays installed until the game exits (the host applies load-plan
    // patches for the whole process); without the magic it only beats and publishes its state word.
    void Shutdown() {
        if (pending_) store32_(OffsetCancelSeq, next_seq_);
        pending_ = false;
        store32_(OffsetModuleMagic, 0);
    }

    // The latest finished request, handed out once.
    bool TakeOutcome(Outcome& out) {
        if (!outcome_.done) return false;
        out = outcome_;
        outcome_ = {};
        return true;
    }

private:
    void PollPending(std::int64_t now_ms) {
        if (!pending_) return;
        std::uint32_t ack = 0;
        if (load32_(OffsetAckSeq, &ack) && ack == next_seq_) {
            std::uint32_t code = 0, value = 0;
            load32_(OffsetResultCode, &code);
            load32_(OffsetResultValue, &value);
            Finish(true, code <= static_cast<std::uint32_t>(Result::CannotReturn) ? static_cast<Result>(code)
                                                                              : Result::NotConfigured,
                   value);
            return;
        }
        if (cancel_at_ < 0) {
            if (now_ms - staged_at_ > AckWindowMs()) {
                // The HUD update stopped running (a menu, an event, loading) or the game paused:
                // never let this request run later.
                store32_(OffsetCancelSeq, next_seq_);
                cancel_at_ = now_ms;
            }
            return;
        }
        if (now_ms - cancel_at_ > AckWindowMs()) Finish(false, Result::Cancelled, 0);
    }

    void FinishUnanswered(Result code) {
        // Epoch moved: the guest refuses the stale request itself (or already ran it). Withdraw it
        // and report it as unanswered; the controller treats this like a timeout.
        store32_(OffsetCancelSeq, next_seq_);
        Finish(false, code, 0);
    }

    void Finish(bool answered, Result code, std::uint32_t value) {
        pending_ = false;
        outcome_ = {true, answered, pending_op_, code, value};
        pending_op_ = Op::None;
    }

    LoadU32 load32_;
    StoreU32 store32_;
    Status status_{};
    Outcome outcome_{};
    State state_{};
    std::uint32_t state_word_ = 0;
    std::uint32_t epoch_ = 0, beat_ = 0, poll_beat_ = 0, next_seq_ = 0;
    bool beat_seen_ = false, poll_seen_ = false, pending_ = false;
    Op pending_op_ = Op::None;
    std::int64_t beat_at_ = -1, poll_at_ = -1, frame_ms_ = 0, staged_at_ = 0, cancel_at_ = -1;
};

// ---- The Skell button: what face to show and whether a tap may act ---------------------------------

enum class Face { Return, Board, Exit }; // Return = the read-only prototype's disabled "Return to Skell"
enum class Reason { None, NoHelper, NotNow, Busy, Lockout, Settling, Refused, Hold };

struct Button {
    Face face = Face::Return;
    bool enabled = false;
    Reason reason = Reason::NoHelper;
    bool ring = false; // lockout / pending: the face shows the "working" ring
    // Return to Skell: the face is Face::Return and a 600 ms HOLD acts (a tap only shows the hint).
    bool return_ready = false;
    bool hint = false; // a short tap landed on the Return face: show "Hold"
    bool operator==(const Button&) const = default;
};

// Press outcome for the action handler: false is the refusal haptic.
struct Press {
    bool accepted = false;
    Reason reason = Reason::None;
};

// 600 ms after the piloting state flips the button is locked (UX-SPEC 5.1), so a double tap cannot
// board and instantly exit; after a submitted request it stays locked until the state flips or
// SettleMs elapses (the game completes boarding/exit asynchronously, and a second request while the
// first one animates must never be posted).
inline constexpr std::int64_t LockoutMs = 600;
inline constexpr std::int64_t FlightMs = 10 * 1000; // Board / Exit / Switch in flight end at the latest after this
inline constexpr std::int64_t SettleMs = FlightMs;
inline constexpr std::int64_t RefusedShownMs = 1500;
inline constexpr std::int64_t ReturnSettleMs = 90 * 1000; // a Return in flight ends at the latest after this

class Controller {
public:
    // Call every sample with the bridge's status and whether the party has a Skell at all.
    Button Update(const Status& s, bool has_skell, std::int64_t now_ms) {
        const bool known = s.helper_present && s.state_fresh && s.state.valid && s.state.hud_ok;
        const int mode = known ? (s.state.piloting ? 2 : 1) : 0;
        // A submitted Return is in flight until (a) the Skell is driven, (b) field_ok went 0 (the warp's
        // loading; a silent helper counts) and came back to 1, or (c) ReturnSettleMs passed.
        if (return_settle_until_ > 0) {
            const bool field_ok_now = s.helper_present && s.state_fresh && s.state.field_ok;
            if (now_ms >= return_settle_until_ || mode == 2) {
                return_settle_until_ = 0;
            } else if (!field_ok_now) {
                return_saw_zero_ = true;
            } else if (return_saw_zero_) {
                return_settle_until_ = 0;
            }
        }
        if (mode != 0 && last_mode_ != 0 && mode != last_mode_) {
            lock_until_ = now_ms + LockoutMs; // a real flip
            settle_until_ = 0;                // the awaited flip happened
        }
        if (mode != 0) last_mode_ = mode;
        if (!known && !(s.helper_present && s.state_fresh && s.state.transitioning)) {
            // Anything we knew is stale: a flip while blind is treated as a flip when we see again.
            // (A transition keeps the last mode, so the flip it ends in still locks the button.)
            last_mode_ = 0;
        }
        Button b;
        if (!has_skell || !known) {
            b.reason = s.helper_present ? Reason::NotNow : Reason::NoHelper;
            if (!s.helper_present) b.reason = Reason::NoHelper;
            return Finalise(b, now_ms);
        }
        const bool piloting = s.state.piloting;
        const bool eligible = piloting ? s.state.can_exit : s.state.can_board;
        if (!piloting && !s.state.can_board && !s.busy && settle_until_ <= now_ms) {
            // On foot and nothing to board: the prototype's own face, disabled, unless the native
            // Return to Skell is possible (a 600 ms hold). Board has priority over Return.
            b.reason = Reason::NotNow;
            if (return_settle_until_ > now_ms) {
                b.reason = Reason::Settling;
                b.ring = true;
            } else if (now_ms < lock_until_) {
                b.reason = Reason::Lockout;
                b.ring = true;
            } else if (s.state.can_return) {
                b.return_ready = true;
                b.reason = Reason::None;
            }
            b.hint = b.return_ready && now_ms < hint_until_;
            return Finalise(b, now_ms);
        }
        b.face = piloting ? Face::Exit : Face::Board;
        if (s.busy) {
            b.reason = Reason::Busy;
            b.ring = true;
        } else if (now_ms < lock_until_) {
            b.reason = Reason::Lockout;
            b.ring = true;
        } else if (settle_until_ > now_ms) {
            b.reason = Reason::Settling;
            b.ring = true;
        } else if (!eligible) {
            b.reason = Reason::NotNow;
        } else {
            b.enabled = true;
            b.reason = Reason::None;
        }
        return Finalise(b, now_ms);
    }

    // The tap. `button` is the last Update() result. Returns whether the request was staged.
    template <class BridgeT>
    Press OnPress(BridgeT& bridge, const Button& button, std::int64_t now_ms) {
        if (!button.enabled) {
            if (button.return_ready) hint_until_ = now_ms + RefusedShownMs; // a tap on the Return face: show "Hold"
            return {false, button.return_ready ? Reason::Hold : button.reason};
        }
        const Op op = button.face == Face::Exit ? Op::Exit : Op::Board;
        if (!bridge.Stage(op, now_ms)) return {false, Reason::Busy};
        return {true, Reason::None};
    }

    // The 600 ms hold on the Return face: stage the native Return to Skell (a long loading follows).
    template <class BridgeT>
    Press OnReturnHold(BridgeT& bridge, const Button& button, std::int64_t now_ms) {
        if (!button.return_ready || return_settle_until_ > now_ms) return {false, Reason::Settling};
        if (!bridge.Stage(Op::Return, now_ms)) return {false, Reason::Busy};
        return {true, Reason::None};
    }

    // Feed the bridge's finished request. A submitted one locks the button until the flip.
    void OnOutcome(const Outcome& o, std::int64_t now_ms) {
        if (!o.done || o.op == Op::Switch) return;
        if (o.op == Op::Return) {
            if (o.answered && o.code == Result::Submitted) {
                // The loading that follows takes minutes: wait for the Skell to be driven.
                return_settle_until_ = now_ms + ReturnSettleMs;
                return_saw_zero_ = false;
                lock_until_ = now_ms + LockoutMs;
            } else {
                refused_until_ = now_ms + RefusedShownMs;
            }
            return;
        }
        if (o.answered && o.code == Result::Submitted) {
            settle_until_ = now_ms + SettleMs;
        } else {
            refused_until_ = now_ms + RefusedShownMs;
            settle_until_ = 0;
        }
    }

private:
    Button Finalise(Button b, std::int64_t now_ms) {
        if (now_ms < refused_until_ && !b.enabled && b.reason == Reason::NotNow) b.reason = Reason::Refused;
        return b;
    }

    int last_mode_ = 0; // 0 unknown, 1 on foot, 2 piloting
    std::int64_t lock_until_ = 0, settle_until_ = 0, refused_until_ = 0, return_settle_until_ = 0, hint_until_ = 0;
    bool return_saw_zero_ = false;
};

// ---- The Set-as-controlled button (Party page) ---------------------------------------------------
// A member token is kind<<8|id: kind 1 is the avatar (id 0), kind 2 a party character.
inline std::uint32_t MemberToken(int id) { return ((id == 0 ? 1u : 2u) << 8) | static_cast<std::uint32_t>(id & 0xFF); }

struct SetButton {
    bool enabled = false;
    Reason reason = Reason::NoHelper;
    bool operator==(const SetButton&) const = default;
};

class SwitchController {
public:
    // selected_present: the selected row holds a member; selected_controlled: it is slot 0 already.
    SetButton Update(const Status& s, bool selected_present, bool selected_controlled, std::int64_t now_ms) {
        SetButton b;
        if (!s.helper_present || !s.state_fresh) return b;
        b.reason = Reason::NotNow;
        if (!selected_present || selected_controlled) return Finalise(b, now_ms);
        if (s.busy) b.reason = Reason::Busy;
        else if (now_ms < lock_until_) b.reason = Reason::Lockout;
        else if (s.poll_beat < hold_poll_) b.reason = Reason::Settling;
        else if (now_ms < flight_until_ && s.slot0_token != target_token_) b.reason = Reason::Settling; // until the target is slot 0, or FlightMs
        else if (s.state.switch_ready) { b.enabled = true; b.reason = Reason::None; }
        return Finalise(b, now_ms);
    }
    template <class BridgeT>
    Press OnPress(BridgeT& bridge, const SetButton& button, int slot, int member_id, std::int64_t now_ms) {
        if (!button.enabled) return {false, button.reason};
        if (slot < 1 || slot > 3) return {false, Reason::NotNow};
        if (!bridge.Stage(Op::Switch, now_ms, static_cast<std::uint32_t>(slot), MemberToken(member_id)))
            return {false, Reason::Busy};
        return {true, Reason::None};
    }
    // After a submitted switch every handle changes and the game's own cooldown starts: stay
    // disabled until two fresh polls, then the state's switch_ready (cooldown) decides.
    void OnOutcome(const Outcome& o, const Status& s, std::int64_t now_ms) {
        if (!o.done || o.op != Op::Switch) return;
        if (o.answered && o.code == Result::Submitted) {
            lock_until_ = now_ms + LockoutMs;
            hold_poll_ = s.poll_beat + 2;
            flight_until_ = now_ms + FlightMs;
            target_token_ = o.value;
        } else {
            refused_until_ = now_ms + RefusedShownMs;
        }
    }
    // Completion as the module can see it: the target member is slot 0 and the block is idle again.
    static bool Completed(const Status& s) {
        return s.submit_count != 0 && s.state_fresh && s.state.idle && s.slot0_token == s.submit_token;
    }

private:
    SetButton Finalise(SetButton b, std::int64_t now_ms) {
        if (now_ms < refused_until_ && !b.enabled && b.reason == Reason::NotNow) b.reason = Reason::Refused;
        return b;
    }
    std::int64_t lock_until_ = 0, refused_until_ = 0;
    std::uint32_t hold_poll_ = 0, target_token_ = 0;
    std::int64_t flight_until_ = 0;
};

inline const char* ReasonText(Reason r) {
    switch (r) {
    case Reason::NotNow:
    case Reason::Refused: return "Not available now";
    case Reason::Hold: return "Hold";
    case Reason::Busy:
    case Reason::Settling: return "Working...";
    default: return "";
    }
}

} // namespace xcx_helper
